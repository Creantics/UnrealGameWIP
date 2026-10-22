"""Sculpts surface detail onto a multires head from a spec of brush strokes, headless.

    blender -b head.blend --python-exit-code 1 --python tools/blender/face_sculpt.py -- \\
        SPEC.json --object NAME --out sculpted.blend [--preview DIR] [--top 1.88]

The cage passes (fit_head_to_views, landmark_warp) set proportions; what they cannot make are sharp local forms:
a philtrum, the cupid's bow, a lid fold, a crisp jaw edge. Those are brush strokes. The multires is evaluated at its
current level into a temporary mesh, the strokes run on it with ft_sculpt's brushes, and the result is written back
with Reshape, so the cage underneath is untouched and the spec can be re-run after a later cage change.

SPEC (lengths in metres, points relative to the eye line and the head's mirror plane as [x, y, z], y backwards):
  {"strokes": [
     {"op": "crease", "points": [[0.030, 0, 0.006], [0.041, 0, 0.0075], [0.052, 0, 0.006]], "mirror": true,
      "radius": 0.0018, "strength": 0.0008, "pinch": 0.5},
     {"op": "draw", "points": [[0.005, 0, -0.049], [0.0065, 0, -0.064]], "mirror": true,
      "radius": 0.003, "strength": 0.0006},
     {"op": "crease", "points": [[0, 0.075, -0.088]], "dir": [-1, 0, 0], "mirror": true, ...}]}
Each point is put on the surface by a ray through it along "dir" (default [0, 1, 0], from the front), starting
0.3 m before it; a hit more than "reach" (default 0.07) beyond the point is a gap and the point is dropped; "mirror" repeats the stroke at -x (with dir mirrored). ops: draw, crease, flatten, smooth, grab
("offset": [outward, back, up]), and box, which takes no points:
     {"op": "box", "x": [0.098, 0.118], "y": [0.105, 0.150], "z": [-0.010, -0.004, 0.010, 0.012],
      "offset": [-0.008, 0, 0], "mirror": true}
moves the region by the offset with a weight rising linearly in x from x[0] (the root, which stays) to x[1], a
trapezoid in z (0 at z[0], 1 from z[1] to z[2], 0 at z[3]) and a hard y range: an ear rim folded in toward the
skull without a fold at its root. Strokes run in order.
"""
import json
import os
import sys

import bpy
import numpy as np
from mathutils import Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ft_sculpt as fs  # noqa: E402
from fit_head_to_views import head_preview  # noqa: E402
from landmark_warp import Warp  # noqa: E402


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:]
    a = {"spec": argv[0], "object": None, "out": None, "preview": None, "top": "1.88"}
    i = 1
    while i < len(argv):
        k = argv[i][2:]
        if k not in a:
            raise SystemExit("face_sculpt: unknown argument %s" % argv[i])
        a[k] = argv[i + 1]
        i += 2
    for k in ("object", "out"):
        if not a[k]:
            raise SystemExit("face_sculpt: --%s is required" % k)
    if a["preview"]:
        a["preview"] = os.path.abspath(a["preview"])
    return a


def multires(obj):
    for m in obj.modifiers:
        if m.type == "MULTIRES":
            return m
    raise SystemExit("face_sculpt: %s has no multires modifier" % obj.name)


def main():
    a = _args()
    with open(a["spec"]) as f:
        spec = json.load(f)
    obj = bpy.data.objects.get(a["object"])
    if obj is None:
        raise SystemExit("face_sculpt: no object %s" % a["object"])
    mr = multires(obj)
    if mr.levels != mr.total_levels:
        raise SystemExit("face_sculpt: viewport level %d is not the top level %d; Reshape would write a coarser level"
                         % (mr.levels, mr.total_levels))
    w = Warp(obj, spec)          # eye line, mirror plane, surface BVH
    bvh = w.bvh()
    origin = np.array([w.x0, w.ye, w.ze])

    dg = bpy.context.evaluated_depsgraph_get()
    tmp = bpy.data.objects.new("face_sculpt_tmp", bpy.data.meshes.new_from_object(obj.evaluated_get(dg)))
    bpy.context.scene.collection.objects.link(tmp)
    tmp.matrix_world = obj.matrix_world.copy()
    s = fs.Sculpt(tmp)
    Mi = np.array(obj.matrix_world.inverted())
    scale = float(np.cbrt(abs(np.linalg.det(np.array(obj.matrix_world)[:3, :3]))))
    print("face_sculpt: level %d, %d vertices" % (mr.levels, len(s.v)))

    def surface(p, d, reach):
        d = Vector(d).normalized()
        o = Vector(origin + p) - 0.3 * d
        hit = bvh.ray_cast(o, d, 0.8)[0]
        if hit is None or (np.array(hit) - origin - p) @ np.array(d) > reach:
            # through a gap (between the lips, say) to a far surface: the point is dropped
            print("face_sculpt: point %s finds no surface within %.0f mm, skipped" % (tuple(np.round(1000 * p, 1)),
                                                                                     1000 * reach))
            return None
        return np.array(hit) @ Mi[:3, :3].T + Mi[:3, 3]

    M = np.array(obj.matrix_world)

    def box(st, side):
        """A graded move of a region: the weight rises linearly in x from the box's root face to its outer face
        (so the root stays and the far side moves most) and follows a trapezoid in z."""
        p = s.v @ M[:3, :3].T + M[:3, 3] - origin
        x = side * p[:, 0]
        x0, x1 = st["x"]
        y0, y1 = st["y"]
        z0, z1, z2, z3 = st["z"]
        wx = np.clip((x - x0) / (x1 - x0), 0.0, 1.0) * (x >= x0)
        wz = np.clip(np.minimum((p[:, 2] - z0) / max(z1 - z0, 1e-9), (z3 - p[:, 2]) / max(z3 - z2, 1e-9)), 0.0, 1.0)
        wy = (p[:, 1] >= y0) & (p[:, 1] <= y1)
        w = wx * wz * wy
        off = (np.array(st["offset"], dtype=float) * [side, 1.0, 1.0]) @ Mi[:3, :3].T
        s.v += w[:, None] * off
        return int((w > 0).sum())

    for st in spec["strokes"]:
        if st["op"] == "box":
            n = [box(st, side) for side in ((1, -1) if st.get("mirror") else (1,))]
            print("face_sculpt: box      %-14s %s vertices moved" % (st.get("name", ""), "/".join(map(str, n))))
            continue
        for side in ((1, -1) if st.get("mirror") else (1,)):
            flip = np.array([side, 1.0, 1.0])
            d = np.array(st.get("dir", [0, 1, 0]), dtype=float) * flip
            hits = [surface(np.array(p, dtype=float) * flip, d, float(st.get("reach", 0.07))) for p in st["points"]]
            pts = np.array([h for h in hits if h is not None])
            if not len(pts):
                continue
            stroke = pts if len(pts) > 1 else pts[0]
            r = float(st.get("radius", 0.003)) / scale
            k = float(st.get("strength", 0.0005)) / scale
            op = st["op"]
            if op == "draw":
                s.draw(stroke, r, k)
            elif op == "crease":
                s.crease(stroke, r, k, pinch=float(st.get("pinch", 0.5)))
            elif op == "flatten":
                for p in np.atleast_2d(pts):
                    s.flatten(p, r, float(st.get("strength", 0.5)))
            elif op == "smooth":
                for p in np.atleast_2d(pts):
                    s.smooth(p, r, iterations=int(st.get("iterations", 2)), strength=float(st.get("strength", 0.5)))
            elif op == "grab":
                s.grab(pts[0], r, (np.array(st["offset"], dtype=float) * flip) @ Mi[:3, :3].T)
            else:
                raise SystemExit("face_sculpt: unknown op %s" % op)
        print("face_sculpt: %-8s %-14s %d point(s)%s" % (st["op"], st.get("name", ""), len(st["points"]),
                                                       " x2" if st.get("mirror") else ""))
    s.commit()

    with bpy.context.temp_override(object=obj, active_object=obj, selected_objects=[tmp, obj],
                                   selected_editable_objects=[tmp, obj]):
        bpy.ops.object.multires_reshape(modifier=mr.name)
    me = tmp.data
    bpy.data.objects.remove(tmp)
    bpy.data.meshes.remove(me)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(a["out"]))
    if a["preview"]:
        os.makedirs(a["preview"], exist_ok=True)
        head_preview(obj, float(a["top"]), a["preview"])
    print("face_sculpt: wrote %s" % a["out"])


if __name__ == "__main__":
    main()
