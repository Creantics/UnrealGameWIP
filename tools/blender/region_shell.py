"""Blocks out hair and accessories on a fitted head as separate meshes, from outlines measured on the sheet, headless.

    blender -b head.blend --python-exit-code 1 --python tools/blender/region_shell.py -- \\
        SPEC.json --object NAME --out shells.blend [--preview DIR] [--top 1.88]

Brows, a beard, a moustache are first judged as shapes: where they sit, how far they reach, how much they stand off
the skin. Two kinds of shell cover that:
  "surface": the head's faces whose centres fall inside a front outline (x, z), and inside a side outline (y, z) when
             one is given, copied out and given a thickness with Solidify. It follows the skin (brows, a moustache,
             a beard on the jaw).
  "prism":   the volume inside both a front outline and a side outline (and a "top" outline [x, y] if given), voxel remeshed and smoothed. It is a mass
             that leaves the skin (a goatee hanging below the chin). Its inside overlaps the head, which hides it.

SPEC (lengths in metres, relative to the eye line and the head's mirror plane as in landmark_warp; y backwards):
  {"level": 2,
   "shells": [
     {"name": "brow", "kind": "surface", "mirror": true, "thickness": 0.0015, "lift": 0.0002,
      "front": [[0.070, 0.020], [0.061, 0.026], ...], "color": [0.05, 0.04, 0.03]},
     {"name": "goatee_hang", "kind": "prism", "mirror": true, "voxel": 0.0015, "smooth": 6,
      "front": [[0, -0.100], [0.040, -0.114], ...], "side": [[-0.044, -0.110], ...]}]}
"front" is a closed outline of [x, z] points; with "mirror" a surface shell repeats at -x, and a prism's outline
(drawn on the +x side, from the midline round to the midline) is mirrored into one symmetric outline. "side" is
[y, z]; a prism's "top" is [x, y], drawn and mirrored like its front, and rounds a mass that
the two side-on outlines alone would leave square in plan. A surface shell skips faces turned backwards (normal y above "back", default 0.3) so an outline cast
through the head takes the front only; "lift" moves the shell off the skin before the thickness is added. "level"
is the multires level the surface shells are cut from; the cut border is smoothed "rim_smooth" times (default 4) so
it does not follow the face edges in steps; pinholes inside it are closed over "close" passes (default 3). "head_color" is the head's colour in the preview.
"""
import json
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fit_head_to_views import head_preview  # noqa: E402
from landmark_warp import Warp  # noqa: E402


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:]
    a = {"spec": argv[0], "object": None, "out": None, "preview": None, "top": "1.88"}
    i = 1
    while i < len(argv):
        k = argv[i][2:]
        if k not in a:
            raise SystemExit("region_shell: unknown argument %s" % argv[i])
        a[k] = argv[i + 1]
        i += 2
    for k in ("object", "out"):
        if not a[k]:
            raise SystemExit("region_shell: --%s is required" % k)
    if a["preview"]:
        a["preview"] = os.path.abspath(a["preview"])
    return a


def inside(poly, p):
    """Even-odd test of points p (n x 2) against a closed outline poly (m x 2)."""
    poly = np.asarray(poly, dtype=float)
    a, b = poly, np.roll(poly, -1, axis=0)
    x, y = p[:, 0:1], p[:, 1:2]
    crosses = ((a[:, 1] > y) != (b[:, 1] > y)) & \
        (x < a[:, 0] + (y - a[:, 1]) * (b[:, 0] - a[:, 0]) / np.where(b[:, 1] == a[:, 1], 1e-12, b[:, 1] - a[:, 1]))
    return crosses.sum(1) % 2 == 1


def material(name, color):
    m = bpy.data.materials.new(name)
    m.diffuse_color = tuple(color) + (1.0,)
    return m


def link(name, me, color):
    o = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(o)
    o.color = tuple(color) + (1.0,)
    me.materials.append(material(name, color))
    return o


def bake(o):
    """Replaces an object's mesh with its evaluated mesh (modifiers applied)."""
    dg = bpy.context.evaluated_depsgraph_get()
    me = bpy.data.meshes.new_from_object(o.evaluated_get(dg))
    old = o.data
    o.modifiers.clear()
    o.data = me
    bpy.data.meshes.remove(old)


class Shells:
    def __init__(self, obj, spec):
        self.obj, self.spec = obj, spec
        w = Warp(obj, spec)
        self.origin = np.array([w.x0, w.ye, w.ze])
        self._head = None

    def head(self):
        """World-space vertices, face centres and normals of the head at the spec's multires level."""
        if self._head is None:
            mr = next((m for m in self.obj.modifiers if m.type == "MULTIRES"), None)
            keep = mr.levels if mr else None
            if mr:
                mr.levels = min(int(self.spec.get("level", 2)), mr.total_levels)
            dg = bpy.context.evaluated_depsgraph_get()
            me = bpy.data.meshes.new_from_object(self.obj.evaluated_get(dg))
            if mr:
                mr.levels = keep
            me.transform(self.obj.matrix_world)
            self._head = me
        return self._head

    def surface(self, sh):
        me = self.head()
        bm = bmesh.new()
        bm.from_mesh(me)
        bm.faces.ensure_lookup_table()
        c = np.array([f.calc_center_median() for f in bm.faces]) - self.origin
        n = np.array([f.normal for f in bm.faces])
        keep = n[:, 1] < float(sh.get("back", 0.3))
        front = np.asarray(sh["front"], dtype=float)
        hit = inside(front, c[:, [0, 2]])
        if sh.get("mirror"):
            hit |= inside(front * [-1, 1], c[:, [0, 2]])
        keep &= hit
        if "side" in sh:
            keep &= inside(sh["side"], c[:, [1, 2]])
        # a face dropped inside the region (a normal or a centre just over a test) leaves a pinhole: take back the
        # faces whose every corner is shared with a kept face; the rim's faces have outer corners, so it stays put
        for _ in range(int(sh.get("close", 3))):
            used = np.zeros(len(bm.verts), dtype=bool)
            for f, k in zip(bm.faces, keep):
                if k:
                    used[[v.index for v in f.verts]] = True
            add = np.array([not k and all(used[v.index] for v in f.verts) for f, k in zip(bm.faces, keep)])
            if not add.any():
                break
            keep |= add
        bmesh.ops.delete(bm, geom=[f for f, k in zip(bm.faces, keep) if not k], context="FACES")
        # the cut follows face edges: smooth the border so it does not show the stair steps
        rim = [v for v in bm.verts if v.is_boundary]
        for _ in range(int(sh.get("rim_smooth", 4))):
            bmesh.ops.smooth_vert(bm, verts=rim, factor=0.5, use_axis_x=True, use_axis_y=True, use_axis_z=True)
        lift = float(sh.get("lift", 0.0002))
        for v in bm.verts:
            v.co += v.normal * lift
        out = bpy.data.meshes.new(sh["name"])
        bm.to_mesh(out)
        bm.free()
        o = link(sh["name"], out, sh.get("color", [0.05, 0.04, 0.03]))
        sol = o.modifiers.new("thickness", "SOLIDIFY")
        sol.thickness = float(sh.get("thickness", 0.0015))
        sol.offset = 1.0
        sol.use_even_offset = True
        bake(o)
        return o, int(keep.sum())

    def prism(self, sh):
        front = np.asarray(sh["front"], dtype=float)
        if sh.get("mirror"):
            # drawn on +x from the midline round to the midline: append the mirror image, reversed
            front = np.vstack([front, (front[-2:0:-1] * [-1, 1])])
        side = np.asarray(sh["side"], dtype=float)
        o0 = self.origin

        def extrude(name, outline, to3, axis):
            bm = bmesh.new()
            vs = [bm.verts.new(Vector(to3(p) + o0)) for p in outline]
            f = bm.faces.new(vs)
            r = bmesh.ops.extrude_face_region(bm, geom=[f])
            d = Vector(axis) * 0.6
            bmesh.ops.translate(bm, vec=d, verts=[e for e in r["geom"] if isinstance(e, bmesh.types.BMVert)])
            bmesh.ops.translate(bm, vec=-0.5 * d, verts=bm.verts[:])
            bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
            me = bpy.data.meshes.new(name)
            bm.to_mesh(me)
            bm.free()
            o = bpy.data.objects.new(name, me)
            bpy.context.scene.collection.objects.link(o)
            return o

        a = extrude(sh["name"], front, lambda p: np.array([p[0], 0.0, p[1]]), (0, 1, 0))
        cutters = [extrude(sh["name"] + "_side", side, lambda p: np.array([0.0, p[0], p[1]]), (1, 0, 0))]
        if "top" in sh:
            top = np.asarray(sh["top"], dtype=float)
            if sh.get("mirror"):
                top = np.vstack([top, (top[-2:0:-1] * [-1, 1])])
            cutters.append(extrude(sh["name"] + "_top", top, lambda p: np.array([p[0], p[1], 0.0]), (0, 0, 1)))
        for b in cutters:
            boo = a.modifiers.new("intersect", "BOOLEAN")
            boo.operation, boo.solver, boo.object = "INTERSECT", "EXACT", b
            bake(a)
            bpy.data.meshes.remove(b.data)
        rem = a.modifiers.new("remesh", "REMESH")
        rem.mode, rem.voxel_size = "VOXEL", float(sh.get("voxel", 0.0015))
        sm = a.modifiers.new("smooth", "SMOOTH")
        sm.iterations, sm.factor = int(sh.get("smooth", 6)), 0.8
        bake(a)
        color = sh.get("color", [0.05, 0.04, 0.03])
        a.color = tuple(color) + (1.0,)
        a.data.materials.append(material(sh["name"], color))
        return a, len(a.data.vertices)


def main():
    a = _args()
    with open(a["spec"]) as f:
        spec = json.load(f)
    obj = bpy.data.objects.get(a["object"])
    if obj is None:
        raise SystemExit("region_shell: no object %s" % a["object"])
    s = Shells(obj, spec)
    for sh in spec["shells"]:
        old = bpy.data.objects.get(sh["name"])
        if old is not None:
            bpy.data.objects.remove(old, do_unlink=True)
        kind = sh.get("kind", "surface")
        if kind == "surface":
            o, k = s.surface(sh)
            print("region_shell: %-14s surface, %d faces, %d vertices" % (sh["name"], k, len(o.data.vertices)))
        elif kind == "prism":
            o, k = s.prism(sh)
            print("region_shell: %-14s prism, %d vertices" % (sh["name"], k))
        else:
            raise SystemExit("region_shell: unknown kind %s" % kind)
        o.parent = obj
        o.matrix_parent_inverse = obj.matrix_world.inverted()
    if s._head is not None:
        bpy.data.meshes.remove(s._head)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(a["out"]))
    if a["preview"]:
        os.makedirs(a["preview"], exist_ok=True)
        bpy.context.scene.render.engine = "BLENDER_WORKBENCH"
        bpy.context.scene.display.shading.color_type = "OBJECT"
        obj.color = tuple(spec.get("head_color", [0.62, 0.45, 0.35])) + (1.0,)
        head_preview(obj, float(a["top"]), a["preview"])
    print("region_shell: wrote %s" % a["out"])


if __name__ == "__main__":
    main()
