"""Grow patches of short hair curves on a head mesh and write them as Alembic for Unreal's groom importer.

blender -b --factory-startup --python-exit-code 1 --python tools/blender/groom_patch.py -- spec.json

Each patch is a band on the surface, given in Unreal axes (cm, +Y is the face direction, Z up) as functions of the
distance x from the centre line:
  "x":      [a, soft]          keep |x| < a
  "top":    [z0, k, soft]      keep z < z0 + k*x*x        (a smile-shaped upper edge when k > 0)
  "bottom": [z0, k, soft]      keep z > z0 + k*x*x        (follow a jaw line that rises toward the ears)
  "front":  [y0, k, soft]      keep y > y0 - k*x*x        (how far the band reaches back under the chin)
Each limit fades to nothing over its own "soft" width, so a wide soft top gives a sparse upper edge over a dense
lower one. Surface facing away from the face is skipped. Roots are spread at "density" per cm2 times the mask, then
each curve leaves the surface along the downward tangent mixed with the normal ("normal_mix"), "length" [min, max]
cm, with "points" points and a little "jitter". Under a chin, where the surface faces down, the curves comb back
toward the neck. A patch may override any of the curve settings given at the top level.

spec.json: {"mesh": fbx, "material": 0, "out": abc, "preview": png, "seed": 1, "density": .., "length": [..],
            "points": 5, "normal_mix": .3, "jitter": .1, "width": .01, "patches": [{"x": .., "top": .., ...}]}
The FBX is an Unreal skeletal mesh export (Blender imports it at 1/100 with Y flipped); the curves are written in
Blender's frame at 100x scale, which Unreal's default Alembic conversion turns back into the mesh's own frame.
"""
import json, random, sys
import bpy
from mathutils import Vector

spec = json.load(open(sys.argv[sys.argv.index("--") + 1]))
rnd = random.Random(spec.get("seed", 1))
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=spec["mesh"])
head = [o for o in bpy.data.objects if o.type == "MESH"][0]
mw = head.matrix_world
rot = mw.to_3x3()
me = head.data
me.calc_loop_triangles()


def ue(v):  # Blender world -> Unreal axes (scale applied by the caller)
    return Vector((v.x, -v.y, v.z))


def ramp(d, w):
    t = max(0.0, min(1.0, d / w)) if w > 0 else float(d > 0)
    return t * t * (3 - 2 * t)


def mask(pt, p, n):
    if n.y < -0.3:
        return 0.0
    x2 = p.x * p.x
    m = ramp(pt["x"][0] - abs(p.x), pt["x"][1])
    if "top" in pt:
        z0, k, w = pt["top"]
        m *= ramp(z0 + k * x2 - p.z, w)
    if "bottom" in pt:
        z0, k, w = pt["bottom"]
        m *= ramp(p.z - (z0 + k * x2), w)
    if "front" in pt:
        y0, k, w = pt["front"]
        m *= ramp(p.y - (y0 - k * x2), w)
    return m


tris = []
for t in me.loop_triangles:
    if t.material_index != spec.get("material", 0):
        continue
    a, b, c = (ue(mw @ me.vertices[i].co) * 100 for i in t.vertices)
    area = (b - a).cross(c - a).length / 2
    if area > 0:
        tris.append((a, b, c, area, ue(rot @ t.normal).normalized()))

down, back = Vector((0, 0, -1)), Vector((0, -1, 0))
curves = []
for pt in spec["patches"]:
    cfg = {k: pt.get(k, spec.get(k)) for k in ("density", "length", "points", "normal_mix", "jitter")}
    roots = []
    for a, b, c, area, n in tris:
        if max(mask(pt, q, n) for q in (a, b, c, (a + b + c) / 3)) <= 0:
            continue
        k = area * cfg["density"]
        for _ in range(int(k) + (1 if rnd.random() < k - int(k) else 0)):
            r1, r2 = rnd.random(), rnd.random()
            if r1 + r2 > 1:
                r1, r2 = 1 - r1, 1 - r2
            p = a + (b - a) * r1 + (c - a) * r2
            if rnd.random() < mask(pt, p, n):
                roots.append((p, n))
    for p, n in roots:
        t = down - n * down.dot(n)
        if t.length < 0.5:
            t = back - n * back.dot(n)
        t.normalize()
        d = (t * (1 - cfg["normal_mix"]) + n * cfg["normal_mix"]).normalized()
        seg = rnd.uniform(*cfg["length"]) / (cfg["points"] - 1)
        q = p - n * 0.02  # root just under the skin so no gap shows
        pts = [q]
        for _ in range(cfg["points"] - 1):
            g = Vector((rnd.gauss(0, 1), rnd.gauss(0, 1), rnd.gauss(0, 1)))
            d = (d + t * 0.08 + g * cfg["jitter"]).normalized()
            q = q + d * seg
            pts.append(q)
        curves.append(pts)
    print("PATCH", len(roots), "roots")

cu = bpy.data.curves.new("patch", "CURVE")
cu.dimensions = "3D"
for pts in curves:
    s = cu.splines.new("POLY")
    s.points.add(len(pts) - 1)
    for sp, q in zip(s.points, pts):
        sp.co = (q.x / 100, -q.y / 100, q.z / 100, 1)
        sp.radius = spec.get("width", 0.01) / 200  # Alembic widths come from the point radius (m); Unreal reads them in cm
ob = bpy.data.objects.new(spec.get("name", "patch"), cu)
bpy.context.scene.collection.objects.link(ob)
for o in bpy.data.objects:
    o.select_set(o == ob)
bpy.context.view_layer.objects.active = ob
bpy.ops.wm.alembic_export(filepath=spec["out"], selected=True, start=1, end=1, global_scale=100.0,
                          export_hair=False, export_particles=False)
print("CURVES", len(curves), spec["out"])

if spec.get("preview"):
    # front and side views of the band: head surface grey, curves black, 40 px per cm (Blender has no PIL)
    import numpy as np
    zc = spec.get("preview_z", 151)
    W, H = 1200, 600
    img = np.ones((H, W, 4), np.float32)

    def plot(view, q, val):
        h = q.x if view == 0 else q.y - 8
        u, v = int(300 + 600 * view + h * 40), int(300 - (q.z - zc) * 40)
        if 0 <= u < W and 0 <= v < H:
            img[H - 1 - v, u, :3] = val

    for v in me.vertices:
        q = ue(mw @ v.co) * 100
        if abs(q.z - zc) < 7.5 and q.y > -2:
            for view in (0, 1):
                if view == 1 or q.y > 6:
                    plot(view, q, 0.75)
    for pts in curves:
        for a, b in zip(pts, pts[1:]):
            for i in range(9):
                for view in (0, 1):
                    plot(view, a.lerp(b, i / 8), 0.0)
    im = bpy.data.images.new("preview", W, H, alpha=True)
    im.pixels.foreach_set(img.ravel())
    im.filepath_raw = spec["preview"]
    im.file_format = "PNG"
    im.save()
