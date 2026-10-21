"""Moves facial landmarks of a fitted head by measured amounts with one smooth warp: a 3D thin-plate spline.

    blender -b head.blend --python-exit-code 1 --python tools/blender/landmark_warp.py -- \\
        SPEC.json --object NAME --out warped.blend [--preview DIR]

The row-by-row fits get a head's silhouette and feature heights; what is left is local form (how far the nose and
lips project, how wide the alae and the jaw are, where the brow sits). Those are landmark moves: each one is found
on the surface by a ray and given a move in millimetres, and a thin-plate spline (kernel r, the 3D biharmonic)
through them and through fixed anchors carries the rest of the cage smoothly. Anchors are cage vertices further
than "free" from every landmark (the cranium, ears, neck), so the warp stays local to the face. Nothing below
"cut" moves; motion ramps in over "ramp" above it. Then the upper and lower lids can be rotated about each
eyeball's centre (narrowing the opening without pushing the lid into the eye).

SPEC (lengths in metres, relative to the eye line and the head's mirror plane; moves are [outward, back, up]):
  {"cut": 1.598, "ramp": 0.04, "free": 0.045,
   "landmarks": [{"name": "nose_tip", "x": 0, "z": -0.034, "move": [0, 0.002, 0.003]},
                 {"name": "ala", "x": 0.022, "z": -0.044, "mirror": true, "move": [0.003, 0, 0]},
                 {"name": "gonion", "y": 0.075, "z": -0.085, "from": "side", "mirror": true, "move": [-0.004, 0, 0]}],
   "lids": {"upper_deg": 8, "lower_deg": 0}}
A landmark is the first surface hit of a ray from the front (+y) at (x, z), or with "from": "side" from outside
inwards at (y behind the eye centres, z), "back" from behind at (x, z), "below" upwards from (x, y, z), which
reaches the underside of the chin and jaw, or "above" downwards at (x, y) onto the crown ("z" unused). "mirror" adds the same landmark at -x. Prints each landmark's surface
point and the largest move left on an anchor (the fit's residual, which should be zero).
"""
import json
import os
import sys

import bpy
import numpy as np
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fit_head_to_views import head_preview  # noqa: E402


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:]
    a = {"spec": argv[0], "object": None, "out": None, "preview": None, "top": "1.88"}
    i = 1
    while i < len(argv):
        k = argv[i][2:]
        if k not in a:
            raise SystemExit("landmark_warp: unknown argument %s" % argv[i])
        a[k] = argv[i + 1]
        i += 2
    for k in ("object", "out"):
        if not a[k]:
            raise SystemExit("landmark_warp: --%s is required" % k)
    if a["preview"]:
        a["preview"] = os.path.abspath(a["preview"])
    return a


def tps_fit(C, D):
    """Thin-plate spline in 3D (kernel r plus an affine part) through control points C with displacements D."""
    n = len(C)
    K = np.sqrt(((C[:, None, :] - C[None, :, :]) ** 2).sum(2))
    P = np.hstack([np.ones((n, 1)), C])
    A = np.zeros((n + 4, n + 4))
    A[:n, :n], A[:n, n:], A[n:, :n] = K, P, P.T
    b = np.zeros((n + 4, 3))
    b[:n] = D
    return np.linalg.solve(A, b)


def tps_eval(C, W, X, chunk=4096):
    out = np.empty((len(X), 3))
    for i in range(0, len(X), chunk):
        x = X[i:i + chunk]
        K = np.sqrt(((x[:, None, :] - C[None, :, :]) ** 2).sum(2))
        out[i:i + chunk] = K @ W[:len(C)] + np.hstack([np.ones((len(x), 1)), x]) @ W[len(C):]
    return out


class Warp:
    def __init__(self, obj, spec):
        self.obj, self.spec = obj, spec
        self.cut = float(spec.get("cut", 1.598))
        self.ramp = float(spec.get("ramp", 0.04))
        self.eyes = [ch for ch in obj.children if ch.type == "MESH" and ch.matrix_world.translation.z > self.cut]
        if len(self.eyes) != 2:
            raise SystemExit("landmark_warp: expected two eye children above the cut, found %d" % len(self.eyes))
        e = [np.array(ch.matrix_world.translation) for ch in self.eyes]
        self.x0 = 0.5 * (e[0][0] + e[1][0])
        self.ye = 0.5 * (e[0][1] + e[1][1])
        self.ze = 0.5 * (e[0][2] + e[1][2])

    def world(self):
        o = self.obj
        co = np.empty(len(o.data.vertices) * 3)
        o.data.vertices.foreach_get("co", co)
        M = np.array(o.matrix_world)
        return co.reshape(-1, 3) @ M[:3, :3].T + M[:3, 3]

    def set_world(self, Q):
        o = self.obj
        Mi = np.array(o.matrix_world.inverted())
        o.data.vertices.foreach_set("co", (Q @ Mi[:3, :3].T + Mi[:3, 3]).ravel())
        o.data.update()
        bpy.context.view_layer.update()

    def bvh(self):
        dg = bpy.context.evaluated_depsgraph_get()
        me = self.obj.evaluated_get(dg).to_mesh()
        M = self.obj.matrix_world
        t = BVHTree.FromPolygons([M @ v.co for v in me.vertices], [tuple(p.vertices) for p in me.polygons])
        self.obj.evaluated_get(dg).to_mesh_clear()
        return t

    def weight(self, z):
        return np.clip((z - self.cut) / self.ramp, 0.0, 1.0)

    def landmarks(self):
        """Surface points and world moves of the spec's landmarks (mirrored ones doubled)."""
        bvh = self.bvh()
        pts, moves, names = [], [], []
        for lm in self.spec["landmarks"]:
            sides = (1, -1) if lm.get("mirror") else (1,)
            for s in sides:
                frm = lm.get("from", "front")
                if "z" not in lm and frm != "above":
                    raise SystemExit("landmark_warp: landmark %s needs a z" % lm["name"])
                z = self.ze + float(lm.get("z", 0.0))
                if frm == "side":
                    o = Vector((self.x0 + s * 0.3, self.ye + float(lm["y"]), z))
                    hit = bvh.ray_cast(o, Vector((-s, 0.0, 0.0)), 0.6)[0]
                elif frm == "back":
                    o = Vector((self.x0 + s * float(lm.get("x", 0.0)), self.ye + 0.5, z))
                    hit = bvh.ray_cast(o, Vector((0.0, -1.0, 0.0)), 0.8)[0]
                elif frm == "below":
                    # upwards from z (under the chin, say): the first surface above the start point
                    o = Vector((self.x0 + s * float(lm.get("x", 0.0)), self.ye + float(lm["y"]), z))
                    hit = bvh.ray_cast(o, Vector((0.0, 0.0, 1.0)), 0.3)[0]
                elif frm == "above":
                    o = Vector((self.x0 + s * float(lm.get("x", 0.0)), self.ye + float(lm["y"]), self.ze + 0.4))
                    hit = bvh.ray_cast(o, Vector((0.0, 0.0, -1.0)), 0.8)[0]
                else:
                    o = Vector((self.x0 + s * float(lm.get("x", 0.0)), self.ye - 0.4, z))
                    hit = bvh.ray_cast(o, Vector((0.0, 1.0, 0.0)), 0.8)[0]
                if hit is None:
                    raise SystemExit("landmark_warp: landmark %s finds no surface" % lm["name"])
                mv = np.array(lm["move"], dtype=float)
                pts.append(np.array(hit))
                moves.append(np.array([s * mv[0], mv[1], mv[2]]))
                names.append(lm["name"] + ("" if len(sides) == 1 else ".L" if s > 0 else ".R"))
        return np.array(pts), np.array(moves), names

    def warp(self):
        P = self.world()
        L, D, names = self.landmarks()
        for n, p, d in zip(names, L, D):
            print("landmark_warp: %-12s at (%+.1f, %+.1f, %+.1f) mm, move (%+.1f, %+.1f, %+.1f) mm"
                  % ((n,) + tuple(1000 * (p - [self.x0, self.ye, self.ze])) + tuple(1000 * d)))
        free = float(self.spec.get("free", 0.045))
        up = np.flatnonzero(P[:, 2] > self.cut)
        far = ((P[up, None, :] - L[None, :, :]) ** 2).sum(2).min(1) > free ** 2
        anchors = P[up[far]]
        anchors = anchors[:: max(1, len(anchors) // 600)]
        C = np.vstack([L, anchors])
        W = tps_fit(C, np.vstack([D, np.zeros((len(anchors), 3))]))
        disp = tps_eval(C, W, P[up]) * self.weight(P[up, 2])[:, None]
        Q = P.copy()
        Q[up] += disp
        res = np.abs(tps_eval(C, W, anchors)).max()
        print("landmark_warp: %d landmarks, %d anchors, largest anchor residual %.3f mm, largest move %.1f mm"
              % (len(L), len(anchors), 1000 * res, 1000 * np.sqrt((disp ** 2).sum(1)).max()))
        # the eyes ride the warp at their centres (rigid: a translation only)
        for ch in self.eyes:
            c = np.array(ch.matrix_world.translation)
            d = tps_eval(C, W, c[None])[0]
            ch.matrix_world = Matrix.Translation(Vector(d)) @ ch.matrix_world
        self.set_world(Q)

    def lids(self, upper_deg, lower_deg, reach=0.009):
        """Rotates the lids about each eyeball's horizontal axis: the upper lid down by upper_deg, the lower lid up
        by lower_deg, fading out from the eyeball's shell to `reach` beyond it, front half and lid width only."""
        if not upper_deg and not lower_deg:
            return
        P = self.world()
        Q = P.copy()
        for ch in self.eyes:
            c = np.array(ch.matrix_world.translation)
            r = max(ch.dimensions) / 2
            d = P - c
            dist = np.sqrt((d ** 2).sum(1))
            shell = np.clip(1.0 - (dist - r) / reach, 0.0, 1.0)
            front = np.clip(-d[:, 1] / (0.4 * r), 0.0, 1.0)
            wide = np.clip((1.6 * r - np.abs(d[:, 0])) / (0.4 * r), 0.0, 1.0)
            w = shell * front * wide * (dist < r + reach)
            # only this eye's lid vertices: writing the rest would undo the other eye's rotation
            for sel, deg in (((d[:, 2] > 0) & (w > 0), upper_deg), ((d[:, 2] <= 0) & (w > 0), -lower_deg)):
                # rotation about +x through the centre: a positive angle takes a point in front (-y) downwards
                t = np.radians(deg) * w[sel] * np.clip(np.abs(d[sel, 2]) / (0.25 * r), 0.0, 1.0)
                y, z = d[sel, 1], d[sel, 2]
                Q[sel, 1] = c[1] + y * np.cos(t) - z * np.sin(t)
                Q[sel, 2] = c[2] + y * np.sin(t) + z * np.cos(t)
        moved = np.sqrt(((Q - P) ** 2).sum(1)).max()
        print("landmark_warp: lids upper %+.1f deg, lower %+.1f deg (largest move %.1f mm)"
              % (upper_deg, lower_deg, 1000 * moved))
        self.set_world(Q)


def main():
    a = _args()
    with open(a["spec"]) as f:
        spec = json.load(f)
    obj = bpy.data.objects.get(a["object"])
    if obj is None:
        raise SystemExit("landmark_warp: no object %s" % a["object"])
    w = Warp(obj, spec)
    if spec.get("landmarks"):
        w.warp()
    lids = spec.get("lids", {})
    w.lids(float(lids.get("upper_deg", 0)), float(lids.get("lower_deg", 0)))
    bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(a["out"]))
    if a["preview"]:
        os.makedirs(a["preview"], exist_ok=True)
        head_preview(obj, float(a["top"]), a["preview"])
    print("landmark_warp: wrote %s" % a["out"])


if __name__ == "__main__":
    main()
