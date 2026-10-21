"""Silhouette gate: how well a mesh's orthographic outline matches the concept sheet's views.

    blender -b model.blend --python-exit-code 1 --python tools/blender/silhouette_iou.py -- MASKS_DIR OUT_DIR \\
        [--views front,side,back] [--side-faces right] [--min 0.95] [--object NAME]

MASKS_DIR holds hull_from_views.py's front/side/back.png (figure cropped to its box). Each view is rendered at
the mask's pixel size, framed on the mesh's own box in that view (so the score is about shape, not placement),
and compared at the best shift within 6 px: IoU per view, and an overlay OUT_DIR/iou_<view>.png with sheet-only pixels red, mesh-only green,
both white. Exits 1 when any view is under --min. Every visible mesh counts unless --object names one.
"""
import os
import sys

import bpy
import numpy as np
from mathutils import Vector

# camera direction (from the camera to the figure) and screen right, in Blender axes, figure facing -y
VIEWS = {"front": (Vector((0, 1, 0)), Vector((1, 0, 0))),
         "back": (Vector((0, -1, 0)), Vector((-1, 0, 0)))}
SHIFT = 6   # px searched each way when aligning the render on the sheet


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:]
    a = {"masks": argv[0], "out": argv[1], "views": "front,side,back", "side-faces": "right", "min": "0.95",
         "object": None}
    i = 2
    while i < len(argv):
        k = argv[i][2:]
        if k not in a:
            raise SystemExit("silhouette_iou: unknown argument %s" % argv[i])
        a[k] = argv[i + 1]
        i += 2
    return a


def load_mask(path):
    img = bpy.data.images.load(path)
    w, h = img.size
    px = np.array(img.pixels[:], dtype=np.float32).reshape(h, w, 4)[::-1]  # top row first
    bpy.data.images.remove(img)
    return px[..., :3].mean(axis=2) > 0.5


def render_mask(objs, look, right, w, h):
    up = right.cross(look)
    pts = [o.matrix_world @ Vector(c) for o in objs for c in o.bound_box]
    us, vs = [p.dot(right) for p in pts], [p.dot(up) for p in pts]
    sc = bpy.context.scene
    sc.render.resolution_x, sc.render.resolution_y = w, h
    cam = bpy.data.objects.new("iou_cam", bpy.data.cameras.new("iou_cam"))
    cam.data.type = "ORTHO"
    sc.collection.objects.link(cam)
    sc.camera = cam
    # the mesh box fills the frame exactly; the mask is cropped the same way
    su, sv = max(us) - min(us), max(vs) - min(vs)
    cam.data.sensor_fit = "HORIZONTAL" if su / w >= sv / h else "VERTICAL"
    cam.data.ortho_scale = su if cam.data.sensor_fit == "HORIZONTAL" else sv
    centre = right * (0.5 * (max(us) + min(us))) + up * (0.5 * (max(vs) + min(vs)))
    cam.location = centre - look * 10.0
    cam.rotation_euler = look.to_track_quat("-Z", "Y").to_euler()
    cam.data.clip_end = 40.0
    path = os.path.join(bpy.app.tempdir, "iou_render.png")
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)
    bpy.data.objects.remove(cam, do_unlink=True)
    img = bpy.data.images.load(path)
    px = np.array(img.pixels[:], dtype=np.float32).reshape(h, w, 4)[::-1]
    bpy.data.images.remove(img)
    return px[..., 3] > 0.5


def best_shift(sheet, mesh, reach=SHIFT):
    """IoU at the best small whole-pixel shift: box framing is thrown off by a few pixels when one view's
    extreme point (a toe, a fingertip) differs, which is placement, not shape."""
    best = (-1.0, 0, 0)
    for dv in range(-reach, reach + 1):
        for du in range(-reach, reach + 1):
            m = np.roll(mesh, (dv, du), axis=(0, 1))
            iou = float((sheet & m).sum()) / max(1, float((sheet | m).sum()))
            if iou > best[0]:
                best = (iou, dv, du)
    return best


def save_overlay(sheet, mesh, path):
    h, w = sheet.shape
    rgb = np.zeros((h, w, 4), dtype=np.float32)
    rgb[..., 3] = 1.0
    rgb[sheet & ~mesh, 0] = 1.0
    rgb[mesh & ~sheet, 1] = 1.0
    rgb[sheet & mesh, :3] = 1.0
    img = bpy.data.images.new("iou_overlay", w, h)
    img.pixels[:] = rgb[::-1].ravel()
    img.filepath_raw = path
    img.file_format = "PNG"
    img.save()
    bpy.data.images.remove(img)


def main():
    a = _args()
    os.makedirs(a["out"], exist_ok=True)
    side_look = Vector((1, 0, 0)) if a["side-faces"] == "right" else Vector((-1, 0, 0))
    views = dict(VIEWS, side=(side_look, Vector((0, -1, 0)) if a["side-faces"] == "right" else Vector((0, 1, 0))))
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_WORKBENCH"
    sc.render.film_transparent = True
    sc.display.shading.light = "FLAT"
    objs = [o for o in sc.objects if o.type == "MESH" and o.visible_get()
            and (a["object"] is None or o.name == a["object"])]
    if not objs:
        raise SystemExit("silhouette_iou: no mesh to render")
    worst = 1.0
    for name in a["views"].split(","):
        sheet = load_mask(os.path.join(a["masks"], name + ".png"))
        h, w = sheet.shape
        mesh = render_mask(objs, *views[name], w, h)
        iou, dv, du = best_shift(sheet, mesh)
        mesh = np.roll(mesh, (dv, du), axis=(0, 1))
        worst = min(worst, iou)
        save_overlay(sheet, mesh, os.path.join(a["out"], "iou_%s.png" % name))
        print("silhouette_iou: %s %.4f (shift %d,%d px)" % (name, iou, du, dv))
    ok = worst >= float(a["min"])
    print("silhouette_iou: %s (worst %.4f, min %s)" % ("PASS" if ok else "FAIL", worst, a["min"]))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
