"""Silhouette gate: how well a mesh's orthographic outline matches the concept sheet's views.

    blender -b model.blend --python-exit-code 1 --python tools/blender/silhouette_iou.py -- MASKS_DIR OUT_DIR \\
        [--views front,side,back] [--side-faces right] [--min 0.95] [--object NAME] [--frame box|height] \
        [--ignore-rows 0-0.11,0.47-0.6]

MASKS_DIR holds hull_from_views.py's front/side/back.png (figure cropped to its box). Each view is rendered at
the mask's pixel size, framed on the mesh's own box in that view (so the score is about shape, not placement),
and compared at the best shift within 6 px: IoU per view, and an overlay OUT_DIR/iou_<view>.png with sheet-only pixels red, mesh-only green,
both white. --frame height scales by the vertical extent alone (centred on the box), so a stray fingertip that
widens the box cannot shrink the whole render. --ignore-rows drops rows (fractions of the height, top 0) from both
sheet and render after framing, for parts another step fits. Exits 1 when any view is under --min. Every visible mesh counts unless --object names one
(that object and its child meshes, such as eyes).
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
         "object": None, "frame": "box", "ignore-rows": ""}
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


def render_mask(objs, look, right, w, h, frame="box"):
    up = right.cross(look)
    # the evaluated box: with multires or subdivision the cage's own box sits outside the surface
    dg = bpy.context.evaluated_depsgraph_get()
    pts = [o.matrix_world @ Vector(c) for o in objs for c in o.evaluated_get(dg).bound_box]
    us, vs = [p.dot(right) for p in pts], [p.dot(up) for p in pts]
    sc = bpy.context.scene
    sc.render.resolution_x, sc.render.resolution_y = w, h
    cam = bpy.data.objects.new("iou_cam", bpy.data.cameras.new("iou_cam"))
    cam.data.type = "ORTHO"
    sc.collection.objects.link(cam)
    sc.camera = cam
    # the mesh box fills the frame exactly; the mask is cropped the same way
    su, sv = max(us) - min(us), max(vs) - min(vs)
    if frame == "height":
        su = 0.0
    cam.data.sensor_fit = "HORIZONTAL" if su / w >= sv / h else "VERTICAL"
    cam.data.ortho_scale = su if cam.data.sensor_fit == "HORIZONTAL" else sv
    centre = right * (0.5 * (max(us) + min(us))) + up * (0.5 * (max(vs) + min(vs)))
    cam.location = centre - look * 10.0
    cam.rotation_euler = look.to_track_quat("-Z", "Y").to_euler()
    cam.data.clip_end = 40.0
    # only the measured meshes go into the render (a library file keeps loose parts beside the body)
    hidden = [o for o in sc.objects if o.type == "MESH" and o not in objs and not o.hide_render]
    for o in hidden:
        o.hide_render = True
    path = os.path.join(bpy.app.tempdir, "iou_render.png")
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)
    bpy.data.objects.remove(cam, do_unlink=True)
    for o in hidden:
        o.hide_render = False
    img = bpy.data.images.load(path)
    px = np.array(img.pixels[:], dtype=np.float32).reshape(h, w, 4)[::-1]
    bpy.data.images.remove(img)
    return px[..., 3] > 0.5


def best_shift(sheet, mesh, reach=SHIFT, keep=None):
    """IoU at the best small whole-pixel shift: box framing is thrown off by a few pixels when one view's
    extreme point (a toe, a fingertip) differs, which is placement, not shape."""
    best = (-1.0, 0, 0)
    for dv in range(-reach, reach + 1):
        for du in range(-reach, reach + 1):
            m = np.roll(mesh, (dv, du), axis=(0, 1))
            if keep is not None:
                m = m & keep[:, None]   # blank the dropped rows after the shift, or the band slides into kept rows
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
            and (a["object"] is None or o.name == a["object"]
                 or (o.parent is not None and o.parent.name == a["object"]))]
    if not objs:
        raise SystemExit("silhouette_iou: no mesh to render")
    worst = 1.0
    for name in a["views"].split(","):
        sheet = load_mask(os.path.join(a["masks"], name + ".png"))
        h, w = sheet.shape
        mesh = render_mask(objs, *views[name], w, h, a["frame"])
        keep = None
        if a["ignore-rows"]:
            # rows owned by another step (head, hands, feet) count for neither side
            keep = np.ones(h, dtype=bool)
            for span in a["ignore-rows"].split(","):
                lo, hi = (float(t) for t in span.split("-"))
                keep[int(lo * h):int(np.ceil(hi * h))] = False
            sheet = sheet & keep[:, None]
            # the box framing was set partly by the dropped rows (long feet move the box centre): line the kept
            # rows up by their column centroids before the small shift search
            cu = lambda m: float(np.nonzero(m)[1].mean()) if m.any() else 0.0
            mesh = np.roll(mesh, int(round(cu(sheet) - cu(mesh & keep[:, None]))), axis=1)
        iou, dv, du = best_shift(sheet, mesh, keep=keep)
        mesh = np.roll(mesh, (dv, du), axis=(0, 1))
        if keep is not None:
            mesh = mesh & keep[:, None]
        worst = min(worst, iou)
        save_overlay(sheet, mesh, os.path.join(a["out"], "iou_%s.png" % name))
        print("silhouette_iou: %s %.4f (shift %d,%d px)" % (name, iou, du, dv))
    ok = worst >= float(a["min"])
    print("silhouette_iou: %s (worst %.4f, min %s)" % ("PASS" if ok else "FAIL", worst, a["min"]))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
