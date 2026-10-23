"""Builds clothes for a skinned body as separate skinned meshes, headless, from a JSON spec, and exports them as FBX.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/garment_shells.py -- \\
        BODY.fbx SPEC.json OUT_DIR [--preview]

BODY.fbx is a skinned body (armature plus the largest mesh is taken as the body). Every garment is skinned to the
same armature, so in the engine it can follow the body's skeleton by bone name.

Where a garment sits is said in bone segments, not in mesh indices, so a spec carries over between bodies of one
skeleton: each body vertex is put on its nearest bone segment (a bone's head to the head of the "next" bone in
SEGMENTS) with a position t from 0 at the head to 1 at the far end. Kinds:
  "shell": the body faces whose vertices fall in "bones" ({fnmatch pattern: [t0, t1]}, plus optional "zmin"/"zmax"
           as fractions of the body height), subdivided "subdiv" times, pushed out "offset" metres along the normal
           and "bag" further away from the bone axis ([[t, metres], ...] per pattern, linear between), then given
           "thickness". "floor" flattens anything below that height (a boot sole); "cap" {"pat|pat": dist} clears the convex hull of the body there (a toe cap); "smooth_more" [[pattern, iterations], ...]
           smooths further on those segments. "push" {pattern: [[t, metres], ...]} shifts those segments forward
           (negative: back) in the body frame. "also" {pattern: metres} adds that segment's vertices closer than that
           to the midline. "clear" (metres) pushes anything the smoothing left closer to the body than that back out
           to it; "relax" [rounds, iterations] then smooths and clears in turn, so the cloth spans the hollows
           between muscles. "trim" (fraction) cuts the shell level at that height and drops what is above (one clean top
           edge); "fill" (n) closes holes of up to n sides. Weights come from the body.
  "skirt": a ring hung from height "top" to "bottom" (fractions of the height), its section the body's own
           hull at "top" plus "offset", flaring by "flare" at the hem; "slit" [angle_degrees, from_fraction] leaves
           the sides open below that point; "squash" [width, depth] scales the section about the body centre.
           Weighted to the pelvis, giving way to the thigh on its side.
  "band":  a ring round the body at height "z" (fraction), "height" tall, "offset" off the hull (a sash).
  "tails": strips hanging from the front of a band: "z", "length", "width", "spread", "count".
  "knot":  a flattened ball on the front at height "z", "size" [x, y, z].
A top-level "height" (metres) sets the height the fractions are of, for a body mesh that stops at the neck; a top-level
"face" (an FBX path, relative to the spec) is a separate head mesh that "clear" also keeps the cloth outside of; a shell's
"face_below" (fraction) adds that head mesh's faces below the height to the shell (a neck skirt over the body's hole;
faces turned to the floor, under the jaw, stay out),
its rim welded onto the body's neck hole where they lie within "weld" metres (default 0.003).
  "collar": a cone round a bone with no body under it (the neck of a body mesh that stops there): rings from "z0" to
           "z1" (fractions) with half-widths "r0" to "r1" ([left, forward] metres), centred on "bone"'s head; with
           "hull" (metres) each ring instead hugs the body and head surface at its height within "neck" metres of the bone
           (default 1.6 times the larger half-width; the shoulders at that height stay out), that far off it, also hugging
           the garments named in "over" (built earlier in the spec, so the collar lies on top of them); "radial"
           {"front"|"back": [[s, metres], ...]} widens the rings that much at the front or back (s 0 bottom row, 1 top).
  "drape": cloth laid over the shoulders from above (a yoke): a polar grid round "bone"'s head from the body and head
           surface at height "ztop" (fraction; the neck is what lies within "neck", default 0.15 m, of the bone) plus "hull"
           metres out a further "r1" ([left, forward] metres), each
           point put on the highest upward-facing body, head or "over" garment surface above it below the first
           surface facing down (a ray up from inside the torso: the underside of the jaw stops it) and no higher than "rise"
           (default 0.03) metres over "ztop", and lifted "offset" metres off it, then smoothed; it follows the slope of the shoulders where rings at fixed heights would stand out
           as a shelf. A point with no such surface (past the top of the back) wraps down onto the nearest body or head
           surface instead of running on level. Weights come from the nearest vertex of the mesh each point rests on.
  "boot":  each leg's boot as rings from "top" (metres above the floor) down to the sole, every ring the convex
           hull of that leg's body section at its height ("band" metres thick) plus "offset": no toes or creases show;
           garments named in "over" (trousers tucked in) count as part of the section.
           Below the ankle the walls only go out going down. "profile" {"l"|"r": [[height, left, right, forward,
           back], ...]} gives the art's half-extents (metres, from the calf bone, or below the ankle from the ankle's
           spot); each ring goes out to at least that outline (a quarter ellipse between neighbouring extents), then
           "smooth" passes even the rings out along the boot and around it (default 6). The
           last "sole" metres are a block following the outline down to "below" metres under the body's floor (the
           foot stands on it), closed underneath. Weights come from the
           nearest body vertex.
Colour: "color" is the base RGB (sRGB, as picked from the art); "rules" is a list of [expression, RGB] painted in order. An expression is numpy
over per-vertex arrays: x (body left, metres from the midline), f (forward, metres from the body's centre plane),
z (height fraction), t (segment position), ang (radians around the segment axis), ax=abs(x), front=f>0, and
m("pattern") for "on a segment matching pattern". "crest" {"center": [x, z], "r": metres, "rays": n, "color": RGB}
paints a rayed sun disc on the front. Colours are baked to a texture per garment ("tex", default 1024).
"""
import fnmatch
import json
import math
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector

SEGMENTS = {  # bone -> the bone whose head ends its segment (Epic/MetaHuman names; others fall back to the tail)
    "pelvis": "spine_01", "spine_01": "spine_02", "spine_02": "spine_03", "spine_03": "spine_04",
    "spine_04": "spine_05", "spine_05": "neck_01", "neck_01": "neck_02", "neck_02": "head",
    "clavicle_?": "upperarm_?", "upperarm_?": "lowerarm_?", "lowerarm_?": "hand_?", "hand_?": "middle_01_?",
    "thigh_?": "calf_?", "calf_?": "foot_?", "foot_?": "ball_?", "ball_?": "middletoe_02_?",
}


def args():
    a = sys.argv[sys.argv.index("--") + 1:]
    return a[0], a[1], a[2], "--preview" in a


def load_body(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=path, automatic_bone_orientation=False)
    arm = next(o for o in bpy.context.scene.objects if o.type == "ARMATURE")
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    body = max(meshes, key=lambda o: len(o.data.vertices))
    for o in meshes:
        if o is not body:
            bpy.data.objects.remove(o)
    return arm, body


def segments(arm):
    """[(name, head, end)] in world space for every bone that has a segment rule."""
    mw = arm.matrix_world
    bones = arm.data.bones
    out = []
    for b in bones:
        nxt = None
        for pat, n in SEGMENTS.items():
            if fnmatch.fnmatch(b.name, pat):
                side = b.name[-1]
                nxt = bones.get(n.replace("?", side))
        if nxt is None and not any(fnmatch.fnmatch(b.name, p) for p in SEGMENTS):
            continue
        h = mw @ b.head_local
        e = mw @ (nxt.head_local if nxt else b.tail_local)
        out.append((b.name, np.array(h), np.array(e)))
    return out


def classify(P, segs):
    """Nearest segment per point: (index into segs, t, closest point on the segment)."""
    best = np.full(len(P), 1e9); idx = np.zeros(len(P), int); T = np.zeros(len(P)); C = np.zeros_like(P)
    for i, (_, h, e) in enumerate(segs):
        ab = e - h
        t = np.clip(((P - h) @ ab) / max(ab @ ab, 1e-12), 0, 1)
        c = h + t[:, None] * ab
        d = np.linalg.norm(P - c, axis=1)
        m = d < best
        best[m] = d[m]; idx[m] = i; T[m] = t[m]; C[m] = c[m]
    return idx, T, C


class Frame:
    """Body axes: up z, forward from the toes, left = up x forward; heights as fractions of the body."""

    def __init__(self, arm, body, segs, height=None):
        P = world_verts(body)
        self.floor, self.top = P[:, 2].min(), P[:, 2].max()
        self.height = height or self.top - self.floor  # a body mesh without its head needs the full height given
        names = {s[0]: s for s in segs}
        foot = names.get("foot_l") or names.get("foot_r")
        fwd = foot[2] - foot[1] if foot else np.array([0, -1.0, 0])
        fwd[2] = 0; self.fwd = fwd / np.linalg.norm(fwd)
        self.up = np.array([0, 0, 1.0])
        self.left = np.cross(self.up, self.fwd)
        self.centre = np.array([P[:, 0].mean(), P[:, 1].mean(), 0])
        if names.get("thigh_l") is not None and (names["thigh_l"][1] - self.centre) @ self.left < 0:
            self.left = -self.left  # left as the skeleton names it

    def local(self, P):
        d = P - self.centre
        return d @ self.left, d @ self.fwd, (P[:, 2] - self.floor) / self.height

    def z(self, frac):
        return self.floor + frac * self.height


def world_verts(o):
    mw = np.array(o.matrix_world)
    V = np.array([v.co for v in o.data.vertices])
    return V @ mw[:3, :3].T + mw[:3, 3]


def new_object(name, V, F, arm):
    me = bpy.data.meshes.new(name)
    me.from_pydata([tuple(v) for v in V], [], [tuple(f) for f in F])
    me.update()
    o = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(o)
    o.parent = arm
    o.matrix_parent_inverse = arm.matrix_world.inverted()  # vertices are in world space
    return o


def copy_weights(dst, src_obj, src_idx, blend=None):
    """dst vertex i takes src vertex src_idx[i]'s weights (or a {group: weight-array} blend when given)."""
    names = [g.name for g in src_obj.vertex_groups]
    groups = {n: dst.vertex_groups.new(name=n) for n in names}
    if blend is not None:
        for n, w in blend.items():
            g = groups.get(n) or dst.vertex_groups.new(name=n)
            for i, wi in enumerate(w):
                if wi > 1e-4:
                    g.add([i], float(wi), "REPLACE")
        return
    sv = src_obj.data.vertices
    for i, si in enumerate(src_idx):
        for ge in sv[si].groups:
            groups[names[ge.group]].add([i], ge.weight, "REPLACE")


def skin(o, arm):
    m = o.modifiers.new("arm", "ARMATURE"); m.object = arm


def apply_mods(o):
    bpy.context.view_layer.objects.active = o
    for m in list(o.modifiers):
        if m.type != "ARMATURE":
            bpy.ops.object.modifier_apply(modifier=m.name)


def interp(table, t):
    table = sorted(table)
    return np.interp(t, [a for a, _ in table], [b for _, b in table])


def shell(g, arm, body, segs, fr):
    P = world_verts(body)
    idx, T, _ = classify(P, segs)
    _, _, zf = fr.local(P)
    keep = np.zeros(len(P), bool)
    for pat, (t0, t1) in g["bones"].items():
        for i, s in enumerate(segs):
            if fnmatch.fnmatch(s[0], pat):
                keep |= (idx == i) & (T >= t0) & (T <= t1)
    xs, _, _ = fr.local(P)
    for pat, xmax in g.get("also", {}).items():  # trunk parts a limb's segment claims (the lats under a hanging arm)
        for i, s in enumerate(segs):
            if fnmatch.fnmatch(s[0], pat):
                keep |= (idx == i) & (np.abs(xs) < xmax)
    keep &= (zf >= g.get("zmin", -1)) & (zf <= g.get("zmax", 2))
    faces = [[v for v in p.vertices] for p in body.data.polygons if keep[list(p.vertices)].all()]
    if not faces:
        raise SystemExit("garment %s: no body faces in %s" % (g["name"], g["bones"]))
    used = sorted({v for f in faces for v in f})
    remap = {v: i for i, v in enumerate(used)}
    V0, F0 = P[used], [[remap[v] for v in f] for f in faces]
    head = CLEAR_ALSO[0] if CLEAR_ALSO and "face_below" in g else None
    if head is not None:  # the head's neck skirt covers the body down to the collarbones (the body has a wide hole
        # there): its faces below "face_below" (a height fraction) join the shell, so the cloth reaches the neck
        PF = world_verts(head)
        lowf = fr.local(PF)[2] <= g["face_below"]
        def down(f):  # under the jaw the skin faces the floor: not neck, and offset it would hang into the neck hole
            q = PF[f]
            return np.cross(q[1] - q[0], q[2] - q[0])[2] < -0.5 * np.linalg.norm(np.cross(q[1] - q[0], q[2] - q[0]))
        ff = [[v for v in p.vertices] for p in head.data.polygons if lowf[list(p.vertices)].all() and not down(list(p.vertices))]
        uf = sorted({v for f in ff for v in f})
        rf = {v: len(used) + i for i, v in enumerate(uf)}
        V0 = np.vstack([V0, PF[uf]]); F0 = F0 + [[rf[v] for v in f] for f in ff]
        print("garment %s: %d head faces below %.3f join the shell" % (g["name"], len(ff), g["face_below"]))
    o = new_object(g["name"], V0, F0, arm)
    copy_weights(o, body, used)
    if head is not None:
        hn = [q.name for q in head.vertex_groups]
        have = {q.name for q in o.vertex_groups}
        hv = head.data.vertices
        for i, si in enumerate(uf):
            ws = [(hn[ge.group], ge.weight) for ge in hv[si].groups if hn[ge.group] in have and ge.weight > 1e-4]
            tot = sum(w for _, w in ws) or 1.0
            for n, w in ws:
                o.vertex_groups[n].add([len(used) + i], w / tot, "REPLACE")
    bm = bmesh.new(); bm.from_mesh(o.data)  # an imported body is split at its UV seams; joined, the shell stays closed there
    if head is not None:  # the head's neck rim lies on the body's neck hole, but with its own vertex spacing: stitch
        # it (each rim vertex near the other rim splits that edge and joins it), so no T-junction opens a slit later
        src = bm.verts.layers.int.new("src")
        bm.verts.ensure_lookup_table()
        for v in bm.verts:
            v[src] = int(v.index >= len(used))
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-4)
        print("garment %s: stitch joined %d rim vertices" % (g["name"], stitch(bm, src, g.get("weld", 0.008))))
    if "trim" in g:  # one clean top edge: cut level before any smoothing, so a ragged boundary never ripples the cloth
        zt = (np.array(o.matrix_world.inverted()) @ np.array([0, 0, fr.z(g["trim"]), 1.0]))[2]
        bmesh.ops.bisect_plane(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:], plane_co=(0, 0, zt), plane_no=(0, 0, 1),
                               clear_outer=True)
    if g.get("fill", 0):  # small holes where a face fell outside "bones" (a vertex on another segment): close them
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
        nb = sum(1 for e in bm.edges if e.is_boundary)
        bmesh.ops.holes_fill(bm, edges=[e for e in bm.edges if e.is_boundary], sides=g["fill"])
        print("garment %s: fill closed holes of up to %d sides, boundary edges %d -> %d"
              % (g["name"], g["fill"], nb, sum(1 for e in bm.edges if e.is_boundary)))
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-4 if head is not None else 1e-5); bm.to_mesh(o.data); bm.free()
    if g.get("subdiv", 1):
        m = o.modifiers.new("sub", "SUBSURF"); m.levels = m.render_levels = g.get("subdiv", 1)
        apply_mods(o)
    me = o.data
    me.update()
    V = world_verts(o)
    N = np.array([v.normal for v in me.vertices]) @ np.array(o.matrix_world)[:3, :3].T
    N /= np.linalg.norm(N, axis=1, keepdims=True) + 1e-12
    vi, vt, vc = classify(V, segs)
    D = N * g.get("offset", 0.01)
    for pat, table in g.get("bag", {}).items():
        on = np.array([fnmatch.fnmatch(segs[i][0], pat) for i in vi])
        r = V - vc
        r /= np.linalg.norm(r, axis=1, keepdims=True) + 1e-12
        D += (on * interp(table, vt))[:, None] * r
    for pat, table in g.get("push", {}).items():  # a shift forward (negative: back) in the body frame, per segment
        on = np.array([fnmatch.fnmatch(segs[i][0], pat) for i in vi])
        D += (on * interp(table, vt))[:, None] * fr.fwd[None, :]
    V = V + D
    if "floor" in g:
        V[:, 2] = np.maximum(V[:, 2], fr.z(g["floor"]))
    set_world_verts(o, V)
    if g.get("smooth", 0):
        m = o.modifiers.new("sm", "SMOOTH"); m.iterations = g["smooth"]; m.factor = 0.5
    for k, (pat, its) in enumerate(g.get("smooth_more", [])):  # extra smoothing where the body has detail the cloth hides (toes)
        vg = o.vertex_groups.new(name="_sm%d" % k)
        vg.add([i for i, si in enumerate(vi) if fnmatch.fnmatch(segs[si][0], pat)], 1.0, "REPLACE")
        m = o.modifiers.new("sm%d" % k, "SMOOTH"); m.iterations = its; m.factor = 1.0; m.vertex_group = vg.name
    if g.get("clear", 0):  # smoothing pulls a tight shell inside the body over convex parts: push it back out
        apply_mods(o)
        clear(o, body, g["clear"])
    for _ in range(g.get("relax", [0, 0])[0]):  # smoothed and cleared in turn, the cloth spans the hollows between
        # muscles and rests on the peaks; one long smooth then one clear only lays it back on the body at "clear"
        set_world_verts(o, taubin(world_verts(o), o.data, g["relax"][1]))
        clear(o, body, g.get("clear", 0.004))
    for pat, dist in g.get("cap", {}).items():  # a smooth cap over detail the cloth hides (toes): clear the convex hull
        apply_mods(o)
        cap(o, body, segs, vi, pat, dist)
    if g.get("thickness", 0):
        m = o.modifiers.new("sol", "SOLIDIFY"); m.thickness = g["thickness"]; m.offset = 1.0
    apply_mods(o)
    for vg in [v for v in o.vertex_groups if v.name.startswith("_sm")]:
        o.vertex_groups.remove(vg)
    return o


def taubin(V, me, its, lam=0.5, mu=-0.53):
    """Smooths V (the mesh's vertices) without shrinking it: each iteration a step toward the neighbours' mean, then a
    slightly larger step back, so detail goes and the overall shape stays (Taubin)."""
    E = np.array([e.vertices[:] for e in me.edges])
    deg = np.bincount(E.ravel(), minlength=len(V)).astype(float)[:, None]
    V = V.copy()
    for _ in range(its):
        for f in (lam, mu):
            S = np.zeros_like(V)
            np.add.at(S, E[:, 0], V[E[:, 1]]); np.add.at(S, E[:, 1], V[E[:, 0]])
            V = V + f * np.where(deg > 0, S / np.maximum(deg, 1) - V, 0.0)
    return V


def stitch(bm, src, dist):
    """Join the open rims of two parts of one bmesh (layer src 0/1) that run along each other within dist: every rim
    vertex of one part lands on the nearest rim edge of the other (splitting it) or on its end, and the pairs merge."""
    joined = 0
    for side in (1, 0):
        for v in [v for v in bm.verts if v.is_boundary and v[src] == side]:
            if not v.is_valid:
                continue
            best = None
            for e in (e for e in bm.edges if e.is_boundary and e.verts[0][src] != side and e.verts[1][src] != side):
                a, b = e.verts[0].co, e.verts[1].co
                ab = b - a
                t = max(0.0, min(1.0, (v.co - a).dot(ab) / max(ab.length_squared, 1e-12)))
                d = (a + ab * t - v.co).length
                if d <= dist and (best is None or d < best[0]):
                    best = (d, e, t)
            if best is None:
                continue
            d, e, t = best
            a, b = e.verts
            L = e.calc_length()
            if t * L < 0.002 or (1 - t) * L < 0.002:  # close to an end: join that vertex
                w = a if t * L < (1 - t) * L else b
            else:
                ne, w = bmesh.utils.edge_split(e, a, t)
                w[src] = 1 - side
            w.co = v.co if side == 0 else w.co  # keep the body's rim where it is; the head rim comes to it
            bmesh.ops.pointmerge(bm, verts=[v, w], merge_co=w.co)
            joined += 1
    return joined


CLEAR_ALSO = []  # other meshes the cloth must stay outside of (a separate head over the neck)
BUILT = {}  # garments built so far, by name (a collar can hug one built before it)


def clear(o, body, dist):
    """Move every vertex closer than dist to the body (or inside it) out to dist along the nearest face's normal."""
    from mathutils.bvhtree import BVHTree
    trees = [BVHTree.FromPolygons([Vector(p) for p in world_verts(m)], [list(p.vertices) for p in m.data.polygons])
             for m in [body] + CLEAR_ALSO]
    V = world_verts(o)
    moved = 0
    for k, tree in enumerate(trees):
        for i, v in enumerate(V):
            loc, n, _, d = tree.find_nearest(Vector(v))
            if loc is None or (k and d > 0.03):  # an open head mesh only counts where the cloth runs close to it
                continue
            h = (Vector(v) - loc).dot(n)
            if h < dist:
                V[i] = np.array(loc + n * dist); moved += 1
    set_world_verts(o, V)
    print("garment %s: clear %.3f moved %d of %d" % (o.name, dist, moved, len(V)))


def cap(o, body, segs, vi, pat, dist):
    """Put the shell's vertices on segments matching pat ("|" between alternatives) at dist outside the convex hull of
    the body's vertices there (one hull per side), so the cloth runs over the toes as one smooth cap, not each toe."""
    from mathutils.bvhtree import BVHTree
    P = world_verts(body)
    idx, _, _ = classify(P, segs)
    V = world_verts(o)
    moved = 0
    hit = lambda name: any(fnmatch.fnmatch(name, q) for q in pat.split("|"))
    for side in sorted({s[0][-1] for s in segs if hit(s[0])}):
        on = [i for i, s in enumerate(segs) if hit(s[0]) and s[0][-1] == side]
        pts = P[np.isin(idx, on)]
        mine = np.isin(np.asarray(vi), on)
        if len(pts) < 4 or not mine.any():
            continue
        bm = bmesh.new()
        for q in pts:
            bm.verts.new(Vector(q))
        bmesh.ops.convex_hull(bm, input=bm.verts)
        for e in [e for e in bm.verts if not e.link_faces]:
            bm.verts.remove(e)
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        tree = BVHTree.FromBMesh(bm)
        c = Vector(pts.mean(0))
        for i in np.nonzero(mine)[0]:
            loc, n, _, _ = tree.find_nearest(Vector(V[i]))
            if loc is None:
                continue
            if n.dot(loc - c) < 0:
                n = -n
            V[i] = np.array(loc + n * dist); moved += 1  # onto the hull, also where the cloth stood further out
        bm.free()
        ed = np.array([e.vertices[:] for e in o.data.edges]); ed = ed[mine[ed].all(1)]
        deg = np.bincount(ed.ravel(), minlength=len(V))
        sm = mine & (deg > 0)
        for _ in range(4):  # the hull is faceted
            acc = np.zeros_like(V); np.add.at(acc, ed[:, 0], V[ed[:, 1]]); np.add.at(acc, ed[:, 1], V[ed[:, 0]])
            V[sm] = 0.5 * V[sm] + 0.5 * acc[sm] / deg[sm, None]
    set_world_verts(o, V)
    print("garment %s: cap %s %.3f moved %d" % (o.name, pat, dist, moved))


def set_world_verts(o, V):
    inv = np.array(o.matrix_world.inverted())
    L = V @ inv[:3, :3].T + inv[:3, 3]
    o.data.vertices.foreach_set("co", L.ravel().astype(np.float32))
    o.data.update()


ARMS = ("upperarm_*", "lowerarm_*", "hand_*")  # hanging arms are not part of a section round the trunk


def hull_profile(body, fr, segs, zfrac, band, n):
    """Trunk section radius per angle (around the body centre) at a height: the largest radius in each wedge."""
    P = world_verts(body)
    x, f, zf = fr.local(P)
    idx, _, _ = classify(P, segs)
    arm = np.array([any(fnmatch.fnmatch(s[0], p) for p in ARMS) for s in segs])
    sel = (np.abs(zf - zfrac) < band) & ~arm[idx]
    ang = np.arctan2(f[sel], x[sel])
    r = np.hypot(x[sel], f[sel])
    bins = ((ang + math.pi) / (2 * math.pi) * n).astype(int) % n
    prof = np.zeros(n)
    np.maximum.at(prof, bins, r)
    for _ in range(3):  # fill empty wedges and soften
        prof = np.maximum(prof, 0.5 * (np.roll(prof, 1) + np.roll(prof, -1)) * (prof == 0))
        prof = 0.25 * np.roll(prof, 1) + 0.5 * prof + 0.25 * np.roll(prof, -1)
    return prof


def ring_points(fr, prof, zw, scale, add):
    n = len(prof)
    a = -math.pi + (np.arange(n) + 0.5) / n * 2 * math.pi
    r = prof * scale + add
    return (fr.centre[None, :] + np.outer(r * np.cos(a), fr.left) + np.outer(r * np.sin(a), fr.fwd)
            + np.outer(np.full(n, 1.0), np.array([0, 0, zw - fr.centre[2]]))), a


def skirt(g, arm, body, segs, fr):
    n, rows = g.get("segments", 64), g.get("rows", 14)
    prof = hull_profile(body, fr, segs, g["top"], 0.01, n)
    prof = np.maximum(prof, np.convolve(np.r_[prof[-4:], prof, prof[:4]], np.ones(9) / 9, "same")[4:-4])
    V, A, S = [], [], []
    for k in range(rows + 1):
        s = k / rows
        zw = fr.z(g["top"] + (g["bottom"] - g["top"]) * s)
        pts, a = ring_points(fr, prof, zw, 1 + g.get("flare", 0.15) * s, g.get("offset", 0.02))
        sx, sf = g.get("squash", [1.0, 1.0])  # width and depth scale of the section about the body centre
        d = pts - fr.centre[None, :]
        pts = pts + np.outer(d @ fr.left * (sx - 1), fr.left) + np.outer(d @ fr.fwd * (sf - 1), fr.fwd)
        V += list(pts); A += list(a); S += [s] * n
    slit_a, slit_from = g.get("slit", [0, 2])
    F = []
    for k in range(rows):
        for i in range(n):
            a = A[k * n + i]
            side = min(abs(abs(a) - 0.0), abs(abs(a) - math.pi))  # angle from the left/right axis
            if math.degrees(side) < slit_a / 2 and (k + 0.5) / rows > slit_from:
                continue
            j = (i + 1) % n
            F.append([k * n + i, k * n + j, (k + 1) * n + j, (k + 1) * n + i])
    o = new_object(g["name"], np.array(V), F, arm)
    S = np.array(S); X = np.cos(np.array(A))
    side = np.clip(0.5 + X * 1.5, 0, 1)  # 1 on the left, 0 on the right, a blend at front and back
    give = g.get("leg_share", 0.5) * S ** g.get("leg_curve", 1.0)  # under 1: the thighs take the cloth higher up
    copy_weights(o, body, None, {"pelvis": 1 - give, "thigh_l": give * side, "thigh_r": give * (1 - side)})
    if g.get("thickness", 0):
        m = o.modifiers.new("sol", "SOLIDIFY"); m.thickness = g["thickness"]; m.offset = 1.0
    m = o.modifiers.new("sub", "SUBSURF"); m.levels = 1
    apply_mods(o)
    return o


def band(g, arm, body, segs, fr):
    n = g.get("segments", 64)
    prof = hull_profile(body, fr, segs, g["z"], 0.015, n)
    V = []
    for zf in (g["z"] - g["height"] / 2, g["z"], g["z"] + g["height"] / 2):
        pts, _ = ring_points(fr, prof, fr.z(zf), 1.0, g.get("offset", 0.02) * (1.15 if zf == g["z"] else 1.0))
        V += list(pts)
    F = [[k * n + i, k * n + (i + 1) % n, (k + 1) * n + (i + 1) % n, (k + 1) * n + i] for k in range(2) for i in range(n)]
    o = new_object(g["name"], np.array(V), F, arm)
    one = np.ones(len(V))
    copy_weights(o, body, None, {g.get("bone", "pelvis"): one})
    if g.get("thickness", 0):
        m = o.modifiers.new("sol", "SOLIDIFY"); m.thickness = g["thickness"]; m.offset = 1.0
    m = o.modifiers.new("sub", "SUBSURF"); m.levels = 1
    apply_mods(o)
    return o


def front_radius(body, fr, segs, zfrac):
    prof = hull_profile(body, fr, segs, zfrac, 0.015, 64)
    return prof[48]  # the wedge facing forward (angle +90 degrees)


def tails(g, arm, body, segs, fr):
    V, F, Wp = [], [], []
    rows = 10
    r0 = front_radius(body, fr, segs, g["z"]) + g.get("offset", 0.04)
    for c in range(g.get("count", 2)):
        side = -1 if c == 0 else 1
        x0 = side * g.get("spread", 0.05)
        base = len(V)
        for k in range(rows + 1):
            s = k / rows
            zw = fr.z(g["z"]) - g["length"] * s
            xc = x0 + side * g.get("splay", 0.04) * s
            w = g["width"] * (1 + 0.3 * s)
            for e in (-0.5, 0.5):
                p = fr.centre + fr.left * (xc + e * w) + fr.fwd * (r0 + g.get("lean", 0.03) * s)
                p[2] = zw
                V.append(p); Wp.append(s)
        for k in range(rows):
            a = base + 2 * k
            F.append([a, a + 1, a + 3, a + 2])
    o = new_object(g["name"], np.array(V), F, arm)
    s = np.array(Wp)
    copy_weights(o, body, None, {"pelvis": 1 - 0.4 * s, "thigh_l": 0.2 * s, "thigh_r": 0.2 * s})
    m = o.modifiers.new("sol", "SOLIDIFY"); m.thickness = g.get("thickness", 0.004); m.offset = 0
    m = o.modifiers.new("sub", "SUBSURF"); m.levels = 1
    apply_mods(o)
    return o


def knot(g, arm, body, segs, fr):
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=20, v_segments=12, radius=1.0)
    sx, sy, sz = g["size"]
    r0 = front_radius(body, fr, segs, g["z"]) + g.get("offset", 0.04)
    c = fr.centre + fr.fwd * r0; c[2] = fr.z(g["z"])
    V = np.array([c + fr.left * v.co.x * sx + fr.fwd * v.co.y * sy + np.array([0, 0, v.co.z * sz]) for v in bm.verts])
    F = [[v.index for v in f.verts] for f in bm.faces]
    bm.free()
    o = new_object(g["name"], V, F, arm)
    copy_weights(o, body, None, {"pelvis": np.ones(len(V))})
    return o


def collar(g, arm, body, segs, fr):
    n, rows = g.get("segments", 48), g.get("rows", 4)
    b = arm.data.bones[g.get("bone", "neck_01")]
    c = np.array(arm.matrix_world @ b.head_local)
    a = -math.pi + (np.arange(n) + 0.5) / n * 2 * math.pi
    V = []
    for k in range(rows + 1):
        s = k / rows
        rl, rf = (np.array(g["r0"]) * (1 - s) + np.array(g["r1"]) * s)
        zw = fr.z(g["z0"] + (g["z1"] - g["z0"]) * s)
        if g.get("hull") is not None:  # hug the neck: per angle, the body/head surface at this height plus "hull" metres
            def ring(meshes, cap=None):
                Q = np.vstack([world_verts(m) for m in meshes])
                Q = Q[np.abs(Q[:, 2] - zw) < 0.008] - c
                qa = np.arctan2(Q @ fr.fwd, Q @ fr.left); qr = np.hypot(Q @ fr.fwd, Q @ fr.left)
                near = qr < (g.get("neck", 1.6 * max(rl, rf)) if cap is None else np.inf)  # the neck, not the shoulders
                out = []
                for k2, ai in enumerate(a):
                    w = near & (np.abs((qa - ai + math.pi) % (2 * math.pi) - math.pi) < 0.15)
                    if cap is not None:
                        w &= qr < cap[k2]
                    out.append(qr[w].max(initial=0.0))
                return np.array(out)
            rad = ring([body] + CLEAR_ALSO)
            if g.get("over"):  # cloth lying on the neck within 3 cm of it, not the cloth on the shoulders at this height
                rad = np.maximum(rad, ring([BUILT[q] for q in g["over"]], rad + 0.03))
            for _ in range(3):
                rad = 0.25 * np.roll(rad, 1) + 0.5 * rad + 0.25 * np.roll(rad, -1)
            rad = rad + interp(g.get("radial", {}).get("front", [[0, 0.0]]), s) * np.maximum(np.sin(a), 0)                 + interp(g.get("radial", {}).get("back", [[0, 0.0]]), s) * np.maximum(-np.sin(a), 0)
            for ai, ri in zip(a, rad):
                p = c + (fr.left * math.cos(ai) + fr.fwd * math.sin(ai)) * (ri + g["hull"])
                V.append([p[0], p[1], zw])
            continue
        for ai in a:
            p = c + fr.left * rl * math.cos(ai) + fr.fwd * rf * math.sin(ai)
            V.append([p[0], p[1], zw])
    F = [[k * n + i, k * n + (i + 1) % n, (k + 1) * n + (i + 1) % n, (k + 1) * n + i] for k in range(rows) for i in range(n)]
    o = new_object(g["name"], np.array(V), F, arm)
    s = np.repeat(np.arange(rows + 1) / rows, n)
    copy_weights(o, body, None, {"spine_05": 1 - s, g.get("bone", "neck_01"): s})
    m = o.modifiers.new("sol", "SOLIDIFY"); m.thickness = g.get("thickness", 0.004); m.offset = 1.0
    m = o.modifiers.new("sub", "SUBSURF"); m.levels = 1
    apply_mods(o)
    return o


def drape(g, arm, body, segs, fr):
    from mathutils.bvhtree import BVHTree
    n, rows = g.get("segments", 64), g.get("rows", 10)
    c = np.array(arm.matrix_world @ arm.data.bones[g.get("bone", "neck_01")].head_local)
    a = -math.pi + (np.arange(n) + 0.5) / n * 2 * math.pi
    zt = fr.z(g["ztop"])
    meshes = [body] + CLEAR_ALSO + [BUILT[q] for q in g.get("over", [])]
    Q = np.vstack([world_verts(m) for m in CLEAR_ALSO or [body]])  # the neck: at the sides the body's rim is the shoulder
    Q = Q[(Q[:, 2] > zt - 0.02) & (Q[:, 2] < zt + 0.008)] - c
    qa = np.arctan2(Q @ fr.fwd, Q @ fr.left); qr = np.hypot(Q @ fr.fwd, Q @ fr.left)
    r1 = np.array(g["r1"])
    inner = np.array([qr[(qr < g.get("neck", 0.15)) & (np.abs((qa - ai + math.pi) % (2 * math.pi) - math.pi) < 0.15)]
                      .max(initial=0.0) for ai in a])
    ok = inner > 0  # an angle with no neck surface near this height takes its neighbours' radius
    inner = np.interp(np.arange(n), np.arange(n)[ok], inner[ok], period=n)
    for _ in range(3):
        inner = 0.25 * np.roll(inner, 1) + 0.5 * inner + 0.25 * np.roll(inner, -1)
    inner += g.get("hull", 0.006)
    outer = inner + 1 / np.hypot(np.cos(a) / r1[0], np.sin(a) / r1[1])  # out from the neck by an ellipse of widths
    trees = [BVHTree.FromPolygons([Vector(p) for p in world_verts(m)], [list(p.vertices) for p in m.data.polygons])
             for m in meshes]
    R = inner[None, :] + (outer - inner)[None, :] * (np.arange(rows + 1) / rows)[:, None]
    XY = c[None, None, :2] + R[..., None] * (np.cos(a)[None, :, None] * fr.left[:2] + np.sin(a)[None, :, None] * fr.fwd[:2])
    Z = np.full(R.shape, np.nan)
    S = np.zeros(R.shape, int)  # which mesh the point rests on (a garment counts as the body under it)
    for k in range(rows + 1):
        for i in range(n):
            hits = []  # every surface along a ray up from inside the torso
            for j, t in enumerate(trees):
                o0 = Vector((*XY[k, i], zt - 0.3))
                for _ in range(12):
                    h = t.ray_cast(o0, Vector((0, 0, 1)), zt + g.get("rise", 0.03) - o0.z)
                    if h[0] is None:
                        break
                    hits.append((h[0].z, h[1].z, j)); o0 = h[0] + Vector((0, 0, 1e-4))
            z = j = None
            for hz, nz, hj in sorted(hits):  # the highest skin or cloth facing up, below the first facing down (the jaw)
                if nz < -0.2 and z is not None:
                    break
                if nz > 0.05:
                    z, j = hz, hj
            if z is not None:
                if z > zt - 0.12 and (k == 0 or np.isnan(Z[k - 1, i]) or Z[k - 1, i] - z < g.get("drop", 0.05)):
                    # not through a hole
                    Z[k, i] = z; S[k, i] = j if j <= len(CLEAR_ALSO) else 0
    miss = np.isnan(Z)
    for k in range(rows + 1):  # a column with no surface under it takes its neighbours' height
        row = Z[k]
        if np.isnan(row).all():
            Z[k] = Z[k - 1]
        elif np.isnan(row).any():
            ok = ~np.isnan(row)
            Z[k] = np.interp(np.arange(n), np.arange(n)[ok], row[ok], period=n)
    floor = Z + g.get("offset", 0.006)
    Z = floor.copy()
    for _ in range(g.get("smooth", 4)):  # smooth round and across, never sinking below what it rests on
        Z = 0.25 * np.roll(Z, 1, 1) + 0.5 * Z + 0.25 * np.roll(Z, -1, 1)
        Z[1:-1] = 0.25 * Z[:-2] + 0.5 * Z[1:-1] + 0.25 * Z[2:]
        Z = np.maximum(Z, floor)
    V = np.c_[XY.reshape(-1, 2), Z.reshape(-1)].reshape(rows + 1, n, 3)
    step = (R[-1] - R[0]) / rows
    N = np.zeros((n, 3)); N[:, 2] = 1.0  # surface normal under the last point of each column
    for k in range(1, rows + 1):  # past an edge (the top of the back) the cloth wraps down onto the nearest surface
        for i in np.nonzero(miss[k])[0]:   # instead of running on level into the air: each step goes outward
            out = np.r_[V[k - 1, i, :2] - c[:2], 0.0]   # along the surface (down a vertical back)
            out /= np.linalg.norm(out) + 1e-9
            d = out - (out @ N[i]) * N[i]
            d = d / np.linalg.norm(d) if np.linalg.norm(d) > 0.3 else np.array([0.0, 0.0, -1.0])
            q = Vector(V[k - 1, i] + d * step[i])
            near = [t.find_nearest(q) for t in trees]
            loc, nrm, _, _ = min((h for h in near if h[0] is not None), key=lambda h: h[3])
            V[k, i] = np.array(loc + nrm * g.get("offset", 0.006))
            N[i] = np.array(nrm)
    wrap = miss.copy(); wrap[1:] |= miss[:-1]  # the wrapped points and the row they hang from
    for _ in range(3 * g.get("smooth", 4)):  # a column that hangs next to one that rests would fold at the change
        A = 0.25 * (np.roll(V, 1, 1) + np.roll(V, -1, 1) + np.r_[V[:1], V[:-1]] + np.r_[V[1:], V[-1:]])
        for k, i in zip(*np.nonzero(wrap)):
            near = [t.find_nearest(Vector(A[k, i])) for t in trees]
            loc, nrm, _, _ = min((h for h in near if h[0] is not None), key=lambda h: h[3])
            V[k, i] = np.array(loc + nrm * g.get("offset", 0.006))
    V = V.reshape(-1, 3)
    F = [[k * n + i, k * n + (i + 1) % n, (k + 1) * n + (i + 1) % n, (k + 1) * n + i] for k in range(rows) for i in range(n)]
    o = new_object(g["name"], V, F, arm)
    from mathutils.kdtree import KDTree
    blend = {}  # weights of the nearest vertex of the mesh each point rests on: over the head's neck skirt the
    S = S.reshape(-1)  # cloth must follow the neck as the skirt does, not the shoulder under it
    for j, m in enumerate([body] + CLEAR_ALSO):
        if not (S == j).any():
            continue
        P = world_verts(m); kd = KDTree(len(P))
        for i, p in enumerate(P):
            kd.insert(Vector(p), i)
        kd.balance()
        names = [vg.name for vg in m.vertex_groups]
        for i in np.nonzero(S == j)[0]:
            for ge in m.data.vertices[kd.find(Vector(V[i]))[1]].groups:
                blend.setdefault(names[ge.group], np.zeros(len(V)))[i] = ge.weight
    copy_weights(o, body, None, blend)
    print("garment %s: drape on head %d of %d, z %.3f..%.3f, inner radius %.3f..%.3f" % (g["name"], (S == 1).sum(), len(S), (Z.min() - fr.floor) / fr.height,
          (Z.max() - fr.floor) / fr.height, inner.min(), inner.max()))
    m = o.modifiers.new("sol", "SOLIDIFY"); m.thickness = g.get("thickness", 0.004); m.offset = 1.0
    m = o.modifiers.new("sub", "SUBSURF"); m.levels = 1
    apply_mods(o)
    return o


def hull_radius(Q, a):
    """Radius along each angle a of the 2D convex hull of points Q (around the origin, which must lie inside)."""
    P = sorted(map(tuple, Q))
    def half(pts):
        h = []
        for p in pts:
            while len(h) > 1 and (h[-1][0] - h[-2][0]) * (p[1] - h[-2][1]) - (h[-1][1] - h[-2][1]) * (p[0] - h[-2][0]) <= 0:
                h.pop()
            h.append(p)
        return h
    H = np.array(half(P)[:-1] + half(P[::-1])[:-1])  # counter-clockwise
    E = np.roll(H, -1, 0) - H
    nrm = np.c_[E[:, 1], -E[:, 0]]; nrm /= np.linalg.norm(nrm, axis=1)[:, None] + 1e-12
    d = (H * nrm).sum(1)
    U = np.c_[np.cos(a), np.sin(a)]
    c = U @ nrm.T
    return np.where(c > 1e-9, d[None, :] / np.maximum(c, 1e-9), np.inf).min(1)


def boot(g, arm, body, segs, fr):
    n = g.get("segments", 64)
    a = -math.pi + (np.arange(n) + 0.5) / n * 2 * math.pi
    P = world_verts(body)
    idx, _, _ = classify(P, segs)
    names = {s[0]: s for s in segs}
    off, sole = g.get("offset", 0.006), g.get("sole", 0.015)
    U = np.c_[np.cos(a), np.sin(a)]
    Vall, Fall = [], []
    for side in "lr":
        calf, foot = names["calf_" + side], names["foot_" + side]
        leg = [i for i, s_ in enumerate(segs) if s_[0] in ("calf_" + side, "foot_" + side, "ball_" + side)]
        Q = P[np.isin(idx, leg)]
        for q in g.get("over", []):  # cloth tucked into the boot (trousers): the rings go round it as well
            W = world_verts(BUILT[q])
            Q = np.vstack([Q, W[np.isin(classify(W, segs)[0], leg)]])
        ztop, zank = fr.floor + g["top"], foot[1][2]
        zs = np.r_[np.arange(ztop, zank, -g.get("step", 0.01)), np.linspace(zank, fr.floor + sole, 12)]
        ka = int(np.sum(zs >= zank))  # rings on the shin; the last of them is the ankle's
        ank = calf[1] + (zank - calf[1][2]) / (calf[2][2] - calf[1][2]) * (calf[2] - calf[1])
        rows, cents = [], []
        for zw in zs:
            band = Q[np.abs(Q[:, 2] - zw) < g.get("band", 0.006)]
            if zw >= zank:  # on the shin: round the calf bone at this height
                cen = calf[1] + (zw - calf[1][2]) / (calf[2][2] - calf[1][2]) * (calf[2] - calf[1])
            else:  # on the foot: round the ankle's spot, so walls that go out going down do so in place
                cen = np.r_[ank[:2], zw]
            L = np.c_[(band - cen) @ fr.left, (band - cen) @ fr.fwd]
            rows.append(hull_radius(L, a) if len(L) > 2 else rows[-1]); cents.append(np.array(cen, float))
        R = np.array(rows); C = np.array(cents)
        for k in range(ka, len(zs)):  # below the ankle the walls only go out going down (no toes, a boot's last)
            R[k] = np.maximum(R[k], R[k - 1])
        for _ in range(3):
            R = 0.25 * np.roll(R, 1, 1) + 0.5 * R + 0.25 * np.roll(R, -1, 1)
            R[1:-1] = 0.25 * R[:-2] + 0.5 * R[1:-1] + 0.25 * R[2:]
            C[1:-1] = 0.25 * C[:-2] + 0.5 * C[1:-1] + 0.25 * C[2:]
        R += off
        prof = np.array(g.get("profile", {}).get(side, []), float).reshape(-1, 5)

        def outline(z):  # the art's outline at height z: four half-extents joined by a quarter ellipse each
            if not len(prof):
                return np.zeros(n)
            l, r, f, b = (np.maximum(np.interp(z - fr.floor, prof[:, 0], prof[:, j], left=0, right=0), 1e-4) for j in range(1, 5))
            A = np.where(U[:, 0] > 0, l, r); Bf = np.where(U[:, 1] > 0, f, b)
            return 1 / np.sqrt((U[:, 0] / A) ** 2 + (U[:, 1] / Bf) ** 2) - g.get("thickness", 0.005)  # cloth grows outward
        H = R.copy()  # the body's hull plus offset: smoothing the outline never takes a ring inside it
        R = np.maximum(R, np.array([outline(z) for z in zs]))
        for _ in range(g.get("smooth", 6)):  # the art's outline is cut a pixel row at a time: even it out along the boot
            R[1:-1] = 0.25 * R[:-2] + 0.5 * R[1:-1] + 0.25 * R[2:]
            R = np.maximum(0.25 * np.roll(R, 1, 1) + 0.5 * R + 0.25 * np.roll(R, -1, 1), H)
        low = Q[Q[:, 2] < fr.floor + sole]  # toes below the last ring stay inside the sole too
        Lw = np.c_[(low - C[-1]) @ fr.left, (low - C[-1]) @ fr.fwd]
        inner = np.maximum(R[-1], hull_radius(Lw, a) + off if len(Lw) > 2 else 0)
        zsole = np.linspace(fr.floor + sole, fr.floor - g.get("below", 0.0), 4)  # the sole block, to the art's outline
        R = np.vstack([R] + [np.maximum(inner, outline(z)) for z in zsole])
        C = np.vstack([C] + [C[-1] * [1, 1, 0] + [0, 0, z] for z in zsole])
        base = len(Vall)
        for k in range(len(R)):
            for i in range(n):
                Vall.append(C[k] + (fr.left * U[i, 0] + fr.fwd * U[i, 1]) * R[k, i])
        rows_ = len(R)
        Fall += [[base + k * n + i, base + k * n + (i + 1) % n, base + (k + 1) * n + (i + 1) % n, base + (k + 1) * n + i]
                 for k in range(rows_ - 1) for i in range(n)]
        Vall.append(C[-1]); ci = len(Vall) - 1  # the bottom of the sole, closed
        Fall += [[base + (rows_ - 1) * n + i, base + (rows_ - 1) * n + (i + 1) % n, ci] for i in range(n)]
    V = np.array(Vall)
    o = new_object(g["name"], V, Fall, arm)
    from mathutils.kdtree import KDTree
    kd = KDTree(len(P))
    for i, p in enumerate(P):
        kd.insert(Vector(p), i)
    kd.balance()
    copy_weights(o, body, [kd.find(Vector(v))[1] for v in V])
    m = o.modifiers.new("sol", "SOLIDIFY"); m.thickness = g.get("thickness", 0.005); m.offset = 1.0
    m = o.modifiers.new("sub", "SUBSURF"); m.levels = 1
    apply_mods(o)
    return o

def paint(o, g, segs, fr):
    """Per-vertex colour from the rules, baked to a texture through a smart UV projection."""
    V = world_verts(o)
    x, f, z = fr.local(V)
    vi, t, c = classify(V, segs)
    r = V - c
    ang = np.arctan2(r @ fr.fwd, r @ fr.left)
    names = np.array([segs[i][0] for i in vi])
    env = {"np": np, "x": x, "f": f, "z": z, "t": t, "ang": ang, "ax": np.abs(x), "front": f > 0,
           "m": lambda p: np.array([fnmatch.fnmatch(n, p) for n in names])}
    col = np.tile(np.array(g.get("color", [0.5, 0.5, 0.5]), float), (len(V), 1))
    for expr, rgb in g.get("rules", []):
        col[np.asarray(eval(expr, env), bool) & np.ones(len(V), bool)] = rgb
    cr = g.get("crest")
    if cr:
        dx, dz = x - cr["center"][0], (z - cr["center"][1]) * fr.height
        rr = np.hypot(dx, dz); aa = np.arctan2(dz, dx)
        rays = cr.get("rays", 12)
        spike = cr["r"] * (0.55 + 0.45 * np.maximum(0, 1 - np.abs(((aa * rays / (2 * math.pi)) % 1) - 0.5) * 2) ** 0.5)
        sun = ((rr < cr["r"] * 0.42) | (rr < spike)) & (f > 0)
        col[sun] = cr["color"]
    me = o.data
    attr = me.color_attributes.new("col", "FLOAT_COLOR", "POINT")
    col = np.where(col <= 0.04045, col / 12.92, ((col + 0.055) / 1.055) ** 2.4)  # spec colours are sRGB, the bake is linear
    rgba = np.c_[col, np.ones(len(col))].astype(np.float32)
    attr.data.foreach_set("color", rgba.ravel())
    bpy.context.view_layer.objects.active = o
    for s in bpy.context.selected_objects:
        s.select_set(False)
    o.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT"); bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(60), island_margin=0.02)
    bpy.ops.object.mode_set(mode="OBJECT")
    size = g.get("tex", 1024)
    img = bpy.data.images.new("T_" + g["name"], size, size)
    img.generated_color = (*np.median(col, axis=0), 1.0)  # a gap between islands reads as the main colour, not black
    mat = bpy.data.materials.new("M_" + g["name"]); mat.use_nodes = True
    nt = mat.node_tree; bsdf = nt.nodes["Principled BSDF"]
    vc = nt.nodes.new("ShaderNodeVertexColor"); vc.layer_name = "col"
    em = nt.nodes.new("ShaderNodeEmission"); out = nt.nodes["Material Output"]
    nt.links.new(vc.outputs["Color"], em.inputs["Color"]); nt.links.new(em.outputs[0], out.inputs["Surface"])
    tex = nt.nodes.new("ShaderNodeTexImage"); tex.image = img; nt.nodes.active = tex
    bsdf.inputs["Roughness"].default_value = g.get("roughness", 0.8)
    me.materials.clear(); me.materials.append(mat)
    sc = bpy.context.scene; sc.render.engine = "CYCLES"; sc.cycles.samples = 1; sc.cycles.device = "CPU"
    sc.render.bake.margin = 32
    sc.render.bake.use_clear = False  # keep the main-colour fill: off-island texels (and the mips built from them) would be black
    bpy.ops.object.bake(type="EMIT")
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"]); nt.links.new(bsdf.outputs[0], out.inputs["Surface"])
    return img


def preview(arm, body, objs, fr, out_dir):
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_EEVEE"; sc.render.resolution_x, sc.render.resolution_y = 700, 1000
    w = bpy.data.worlds.new("w"); sc.world = w; w.use_nodes = True
    w.node_tree.nodes["Background"].inputs[0].default_value = (1, 1, 1, 1)
    w.node_tree.nodes["Background"].inputs[1].default_value = 0.8
    skin_m = bpy.data.materials.new("skin"); skin_m.use_nodes = True
    skin_m.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.55, 0.36, 0.24, 1)
    body.data.materials.clear(); body.data.materials.append(skin_m)
    L = bpy.data.lights.new("k", "SUN"); L.energy = 3.0; lo = bpy.data.objects.new("k", L); sc.collection.objects.link(lo)
    lo.rotation_euler = (math.radians(50), 0, math.radians(30))
    cam = bpy.data.cameras.new("c"); cam.type = "ORTHO"; cam.ortho_scale = fr.height * 1.1
    co = bpy.data.objects.new("c", cam); sc.collection.objects.link(co); sc.camera = co
    mid = fr.centre.copy(); mid[2] = fr.floor + fr.height / 2
    for name, d in (("front", fr.fwd), ("side", fr.left), ("back", -fr.fwd), ("q", (fr.fwd + fr.left) / np.linalg.norm(fr.fwd + fr.left))):
        pos = mid + d * 5
        co.location = Vector(pos)
        co.rotation_euler = (Vector(mid) - Vector(pos)).to_track_quat("-Z", "Y").to_euler()
        sc.render.filepath = os.path.join(out_dir, "prev_" + name + ".png")
        bpy.ops.render.render(write_still=True)


def main():
    body_path, spec_path, out_dir, prev = args()
    out_dir = os.path.abspath(out_dir)  # images save relative to the .blend, not the working directory
    os.makedirs(out_dir, exist_ok=True)
    spec = json.load(open(spec_path))
    arm, body = load_body(body_path)
    if spec.get("face"):  # a separate head mesh over the body's neck: "clear" keeps the cloth outside it too
        before = set(bpy.data.objects)
        bpy.ops.import_scene.fbx(filepath=os.path.join(os.path.dirname(os.path.abspath(spec_path)), spec["face"]),
                                 automatic_bone_orientation=False)
        CLEAR_ALSO.append(max((o for o in set(bpy.data.objects) - before if o.type == "MESH"), key=lambda o: len(o.data.vertices)))
    segs = segments(arm)
    fr = Frame(arm, body, segs, spec.get("height"))
    print("body height %.3f, %d segments" % (fr.height, len(segs)))
    build = {"shell": shell, "skirt": skirt, "band": band, "tails": tails, "knot": knot, "collar": collar, "drape": drape, "boot": boot}
    objs = []
    for g in spec["garments"]:
        o = build[g["kind"]](g, arm, body, segs, fr)
        for p in o.data.polygons:
            p.use_smooth = True
        paint(o, g, segs, fr)
        skin(o, arm)
        objs.append(o)
        BUILT[g["name"]] = o
        print("garment", g["name"], len(o.data.vertices), "verts")
    for img in bpy.data.images:
        if img.name.startswith("T_"):
            img.filepath_raw = os.path.join(out_dir, img.name + ".png"); img.file_format = "PNG"; img.save()
    if prev:
        preview(arm, body, objs, fr, out_dir)
    bpy.ops.object.select_all(action="DESELECT")
    arm.select_set(True)
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.export_scene.fbx(filepath=os.path.join(out_dir, spec.get("out", "garments") + ".fbx"), use_selection=True,
                             add_leaf_bones=False, use_armature_deform_only=False, mesh_smooth_type="FACE",
                             path_mode="COPY", embed_textures=True, bake_anim=False)
    print("GARMENTS_DONE", len(objs))


if __name__ == "__main__":
    main()
