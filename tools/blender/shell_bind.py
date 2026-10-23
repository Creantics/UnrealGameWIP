"""Binds loose meshes that lie on a skinned body (brows, a beard, a moustache from region_shell) to its rig, headless:
each vertex takes the weights and the shape-key deltas of the nearest point on the body's surface, so the pieces
follow the bones and the face shapes as separate skinned meshes.

    blender -b BODY.blend --factory-startup --python-exit-code 1 --python tools/blender/shell_bind.py -- \\
        SHELLS.blend NAME [NAME ...] --out OUT_DIR [--body OBJECT] [--color R,G,B] [--roughness R]

BODY.blend holds the armature and the skinned body (the mesh with the most vertices unless --body names it). The named
objects are appended from SHELLS.blend, placed where they were (world space), bound, and each is written to
OUT_DIR/<NAME>.glb with the armature; the bound scene is saved as OUT_DIR/bound.blend. A vertex reads the corner
values of the nearest body triangle, blended by its barycentric coordinates.

Each piece leaves with one double-sided material of its own name. Its base colour is --color (linear RGB, e.g. a near
black 0.022,0.016,0.012 for dark hair) or else the mean of the shell's colour attribute (region_shell's "color"), and
its roughness is --roughness (default 0.75). Without either the piece would arrive in the engine as Blender's default
light grey.
"""
import os
import sys

import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:]
    a = {"shells": argv[0], "names": [], "out": None, "body": None, "color": None, "roughness": "0.75"}
    i = 1
    while i < len(argv):
        if argv[i] in ("--out", "--body", "--color", "--roughness"):
            a[argv[i][2:]] = argv[i + 1]
            i += 2
        else:
            a["names"].append(argv[i])
            i += 1
    if not a["out"] or not a["names"]:
        sys.exit("shell_bind: SHELLS.blend NAME [NAME ...] --out OUT_DIR")
    a["shells"], a["out"] = os.path.abspath(a["shells"]), os.path.abspath(a["out"])  # Blender reads a bare relative path against its own directory
    a["color"] = [float(c) for c in a["color"].split(",")] if a["color"] else None
    a["roughness"] = float(a["roughness"])
    return a


def give_material(o, color, roughness):
    """One double-sided material named after the piece: color, or the mean of its colour attribute if color is None."""
    if color is None and o.data.color_attributes:
        ca = o.data.color_attributes[0]
        c = np.zeros(len(ca.data) * 4)
        ca.data.foreach_get("color", c)
        color = list(c.reshape(-1, 4)[:, :3].mean(0))
    mat = bpy.data.materials.new(o.name)
    mat.use_backface_culling = False
    if mat.node_tree is None:  # Blender 5 gives a new material its node tree; older versions need use_nodes
        mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    if color is not None:
        bsdf.inputs["Base Color"].default_value = (*color[:3], 1.0)
    bsdf.inputs["Roughness"].default_value = roughness
    bsdf.inputs["Metallic"].default_value = 0.0
    o.data.materials.clear()
    o.data.materials.append(mat)
    return color


def bary(p, a, b, c):
    """Barycentric coordinates of p's projection on triangle abc, clipped to the triangle."""
    v0, v1, v2 = b - a, c - a, p - a
    d00, d01, d11, d20, d21 = v0.dot(v0), v0.dot(v1), v1.dot(v1), v2.dot(v0), v2.dot(v1)
    den = d00 * d11 - d01 * d01 or 1e-20
    v = (d11 * d20 - d01 * d21) / den
    w = (d00 * d21 - d01 * d20) / den
    w3 = np.clip([1.0 - v - w, v, w], 0.0, None)
    return w3 / (w3.sum() or 1.0)


def main():
    a = _args()
    arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
    body = bpy.data.objects[a["body"]] if a["body"] else max((o for o in bpy.data.objects if o.type == "MESH"),
                                                             key=lambda o: len(o.data.vertices))
    with bpy.data.libraries.load(a["shells"]) as (src, dst):
        dst.objects = [n for n in a["names"] if n in src.objects]
    missing = set(a["names"]) - {o.name.split(".")[0] for o in dst.objects}
    if missing:
        sys.exit("shell_bind: not in %s: %s" % (a["shells"], ", ".join(sorted(missing))))

    me = body.data
    mw = body.matrix_world
    P = np.array([mw @ v.co for v in me.vertices])
    me.calc_loop_triangles()
    tris = [t.vertices[:] for t in me.loop_triangles]
    tree = BVHTree.FromPolygons([Vector(p) for p in P], tris)
    groups = [g.name for g in body.vertex_groups]
    W = np.zeros((len(P), len(groups)))
    for v in me.vertices:
        for ge in v.groups:
            W[v.index, ge.group] = ge.weight
    keys = me.shape_keys.key_blocks if me.shape_keys else []
    basis = np.array([k.co for k in me.vertices]) if not keys else np.array([d.co for d in keys[0].data])
    R = np.array(mw)[:3, :3]
    D = {k.name: (np.array([d.co for d in k.data]) - basis) @ R.T for k in keys[1:]}

    os.makedirs(a["out"], exist_ok=True)
    for o in dst.objects:
        bpy.context.scene.collection.objects.link(o)
        wm = o.matrix_world.copy()
        o.parent = None
        o.matrix_world = wm
        bpy.context.view_layer.objects.active = o
        for m in list(o.modifiers):  # what the shell looks like now (solidify, smoothing) is what gets bound
            bpy.ops.object.modifier_apply(modifier=m.name)
        color = give_material(o, a["color"], a["roughness"])
        for c in list(o.data.color_attributes):  # the engine's intake takes colour from materials, not vertices
            o.data.color_attributes.remove(c)
        V = np.array([o.matrix_world @ v.co for v in o.data.vertices])
        corner, wt = np.zeros((len(V), 3), int), np.zeros((len(V), 3))
        for i, p in enumerate(V):
            loc, _n, ti, _d = tree.find_nearest(Vector(p))
            t = tris[ti]
            corner[i] = t
            wt[i] = bary(np.array(loc), P[t[0]], P[t[1]], P[t[2]])
        o.vertex_groups.clear()
        Wv = np.einsum("nk,nkg->ng", wt, W[corner])
        for gi, name in enumerate(groups):
            idx = np.nonzero(Wv[:, gi] > 1e-4)[0]
            if not len(idx):
                continue
            vg = o.vertex_groups.new(name=name)
            for i in idx:
                vg.add([int(i)], float(Wv[i, gi]), "REPLACE")
        inv = np.array(o.matrix_world.inverted())[:3, :3]
        if D:
            o.shape_key_add(name="Basis", from_mix=False)
            for name, d in D.items():
                kb = o.shape_key_add(name=name, from_mix=False)
                dv = np.einsum("nk,nkc->nc", wt, d[corner]) @ inv.T
                co = np.array([v.co for v in o.data.vertices]) + dv
                kb.data.foreach_set("co", co.ravel())
        o.parent = arm
        o.matrix_parent_inverse = arm.matrix_world.inverted()
        m = o.modifiers.new("Armature", "ARMATURE")
        m.object = arm
        bpy.ops.object.select_all(action="DESELECT")
        arm.select_set(True)
        o.select_set(True)
        bpy.context.view_layer.objects.active = arm
        path = os.path.join(a["out"], o.name + ".glb")
        bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True, export_animations=False)
        print("shell_bind: %s %d vertices, %d groups, %d shape keys, colour %s -> %s"
              % (o.name, len(V), len(o.vertex_groups), len(D), None if color is None else [round(c, 3) for c in color], path))
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(a["out"], "bound.blend"))


main()
