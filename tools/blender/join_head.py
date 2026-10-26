"""Joins a rigged head bust (rig_head.py's finish .blend, with the 56 face shapes) onto a rigged headless body
(rig_scan.py's .blend) at the neck, into one lead with one mesh and one skeleton.

    blender -b HEAD.blend --factory-startup --python-exit-code 1 --python tools/blender/join_head.py -- \\
        --body BODY.blend --out name.glb [--blend name.blend] [--seam Z] [--gap 0.004] \\
        [--neck BASE_Z,CHIN_Z,LENGTH] [--preview DIR]

Both must already share a frame: give prep_scan.py the --height that brings the body's neck to the bust's (compare
their neck widths by height). The seam is a level cut through the neck where both have it; by default the height in
their overlap where the two neck sections (width, depth) match best. The bust moves across (x, y, never up or down)
so its neck centres on the body's there, the body's neck tapers over the last 4 cm below the seam to the bust's
section, the bust skin is cut away below seam + gap and the body above seam - gap, and the two open rings are
zipped with one band of triangles (UVs and material from the face beside each edge). Everything else on the bust
(eyes, mouth, hair, lashes) stays whole.

The skeleton is the body's (its rest pose and clips fit the body); the head joint and everything under it (jaw,
eyes, the face joints) take the bust's rest positions, so the face shapes and jaw weights still line up. The skin
around the seam is reweighted along spine_03 -> neck_01 -> neck_02 -> head by height (as rig_head weights a bust),
fading into each side's own weights a few centimetres away. A body stump and a bust neck stacked can make the neck
too long: --neck shortens the stretch from BASE_Z (the notch at its base) to CHIN_Z (just under the chin) to LENGTH
after the join, scaling heights there (eased in and out over a centimetre, so no crease) and dropping everything
above, the bones and shape keys with it; widths are untouched, so the seam still fits. The body's colour differs from the bust's, so the seam
shows a step in colour; blending the textures there is a separate job. The glb is checked with the face shapes.
--preview renders the whole lead front and three-quarter, and the neck up close at rest, bent and turned.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bmesh  # noqa: E402
import bpy  # noqa: E402
import numpy as np  # noqa: E402
from mathutils import Vector  # noqa: E402

import make_leads as ml  # noqa: E402
import ne_blender as nb  # noqa: E402
import rig_common as rc  # noqa: E402
import rig_head as rh  # noqa: E402

NECK_REACH = 0.1  # metres from the centre line that count as neck in a section


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    a = {"body": None, "out": None, "blend": None, "seam": None, "gap": "0.004", "neck": None, "preview": None}
    i = 0
    while i < len(argv):
        k = argv[i]
        if k.startswith("--") and k[2:] in a and i + 1 < len(argv):
            a[k[2:]] = argv[i + 1]
            i += 2
            continue
        raise SystemExit("join_head: unknown argument %s" % k)
    if not (a["body"] and a["out"]):
        raise SystemExit(__doc__)
    a["gap"] = float(a["gap"])
    a["seam"] = float(a["seam"]) if a["seam"] is not None else None
    if a["neck"]:
        a["neck"] = [float(x) for x in a["neck"].split(",")]
        if len(a["neck"]) != 3 or not 0 < a["neck"][2] < a["neck"][1] - a["neck"][0]:
            raise SystemExit("join_head: --neck takes BASE_Z,CHIN_Z,LENGTH with 0 < LENGTH < CHIN_Z - BASE_Z")
    return a


def _rigged(objs):
    arm = next(o for o in objs if o.type == "ARMATURE")
    mesh = next(o for o in objs if o.type == "MESH" and any(m.type == "ARMATURE" for m in o.modifiers))
    return arm, mesh


def islands(me):
    """The connected part each vertex belongs to (union-find over the edges)."""
    parent = np.arange(len(me.vertices))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    for e in me.edges:
        a, b = find(e.vertices[0]), find(e.vertices[1])
        if a != b:
            parent[max(a, b)] = min(a, b)
    return np.array([find(i) for i in range(len(parent))])


def section(V, E, z):
    """Width, depth and centre (x, y) of the level cut at z through the edges E near the centre line, or None."""
    a, b = V[E[:, 0]], V[E[:, 1]]
    cross = (a[:, 2] - z) * (b[:, 2] - z) < 0
    a, b = a[cross], b[cross]
    if len(a) < 6:
        return None
    t = ((z - a[:, 2]) / (b[:, 2] - a[:, 2]))[:, None]
    p = a + t * (b - a)
    p = p[np.abs(p[:, 0]) < NECK_REACH]
    if len(p) < 6:
        return None
    lo, hi = p.min(axis=0), p.max(axis=0)
    return {"w": float(hi[0] - lo[0]), "d": float(hi[1] - lo[1]), "c": (lo[:2] + hi[:2]) / 2}


def rims(V, B):
    """The open rims (boundary edges B grouped into connected loops) as arrays of their vertices' heights."""
    parent = {}

    def find(i):
        while parent.setdefault(i, i) != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    for a, b in B:
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[max(ra, rb)] = min(ra, rb)
    loops = {}
    for i in parent:
        loops.setdefault(find(i), []).append(i)
    return [V[np.array(v), 2] for v in loops.values()]


def boundary_edges(me, sel=None):
    count = {}
    for p in me.polygons:
        if sel is not None and not sel[p.vertices[0]]:
            continue
        for k in p.edge_keys:
            count[k] = count.get(k, 0) + 1
    return np.array([k for k, n in count.items() if n == 1], dtype=int).reshape(-1, 2)


def pick_seam(Vh, Eh, Bh, Vb, Eb, Bb, seam):
    """The seam height: given, or where the two neck sections match best in their overlap, which runs from the top
    of the bust's open bottom (its lowest rim) to the bottom of the body's neck stump (its highest rim)."""
    lo = float(min(rims(Vh, Bh), key=lambda z: z.min()).max())
    hi = float(max(rims(Vb, Bb), key=lambda z: z.max()).min())
    if seam is None:
        best = None
        for z in np.linspace(lo + 0.006, hi - 0.006, 41):
            h, b = section(Vh, Eh, z), section(Vb, Eb, z)
            if h and b:
                cost = abs(h["w"] - b["w"]) + abs(h["d"] - b["d"])
                if best is None or cost < best[0]:
                    best = (cost, float(z))
        if best is None:
            raise SystemExit("join_head: the bust's neck (bottom rim up to %.3f) and the body's (stump from %.3f) "
                             "don't overlap; rescale the body with prep_scan --height" % (lo, hi))
        seam = best[1]
    h, b = section(Vh, Eh, seam), section(Vb, Eb, seam)
    if not (h and b):
        raise SystemExit("join_head: no neck in both meshes at z = %.3f" % seam)
    return seam, h, b, (lo, hi)


def cut(obj, z, keep_above, only=None):
    """Bisects obj's mesh (only the vertices flagged in `only`, if given) at height z and deletes the side not
    kept. Shape keys ride along: bisect interpolates them like any vertex data."""
    me = obj.data
    bm = bmesh.new()
    bm.from_mesh(me)
    bm.verts.ensure_lookup_table()
    verts = [v for v in bm.verts if only is None or only[v.index]]
    vs = set(verts)
    edges = [e for e in bm.edges if e.verts[0] in vs and e.verts[1] in vs]
    faces = [f for f in bm.faces if all(v in vs for v in f.verts)]
    bmesh.ops.bisect_plane(bm, geom=verts + edges + faces, plane_co=(0, 0, z), plane_no=(0, 0, 1),
                           clear_inner=keep_above, clear_outer=not keep_above, dist=1e-6)
    bm.to_mesh(me)
    me.update()
    bm.free()


def ring(bm, z, cx, cy):
    """The longest closed chain of boundary edges lying at height z near (cx, cy), in order."""
    on = [e for e in bm.edges if e.is_boundary and all(abs(v.co.z - z) < 1e-5 and math.hypot(v.co.x - cx, v.co.y - cy)
                                                       < 2 * NECK_REACH for v in e.verts)]
    nbr = {}
    for e in on:
        a, b = e.verts
        nbr.setdefault(a, []).append(b)
        nbr.setdefault(b, []).append(a)
    seen, best = set(), []
    for start in nbr:
        if start in seen:
            continue
        chain, prev, cur = [start], None, start
        seen.add(start)
        while True:
            nxt = [v for v in nbr[cur] if v is not prev]
            if not nxt or nxt[0] is start or nxt[0] in seen:
                break
            prev, cur = cur, nxt[0]
            chain.append(cur)
            seen.add(cur)
        if len(chain) > len(best):
            best = chain
    return best


def zip_rings(bm, top, bot, cx, cy):
    """Joins two rings around (cx, cy) with one band of triangles, walking both by angle. Returns the faces."""
    def ordered(r):
        ang = np.array([math.atan2(v.co.y - cy, v.co.x - cx) for v in r])
        if np.sum(np.diff(np.unwrap(ang))) < 0:  # make both run the same way round
            r, ang = r[::-1], ang[::-1]
        k = int(np.argmin(np.mod(ang, 2 * math.pi)))
        r = r[k:] + r[:k]
        a = np.unwrap(np.array([math.atan2(v.co.y - cy, v.co.x - cx) for v in r]))
        return r, a - a[0]
    A, ta = ordered(top)
    B, tb = ordered(bot)
    ta, tb = np.append(ta, 2 * math.pi), np.append(tb, 2 * math.pi)
    tris, i, j = [], 0, 0
    while i < len(A) or j < len(B):
        if j >= len(B) or (i < len(A) and ta[i + 1] <= tb[j + 1]):
            tris.append((A[i % len(A)], A[(i + 1) % len(A)], B[j % len(B)]))
            i += 1
        else:
            tris.append((A[i % len(A)], B[(j + 1) % len(B)], B[j % len(B)]))
            j += 1
    # Wind the band like the bust face beside its first edge: a shared edge runs opposite ways in the two faces.
    a0, a1 = A[0], A[1]
    old = next(f for f in a0.link_faces if a1 in f.verts)
    seq = list(old.verts)
    k = seq.index(a0)
    flip = seq[(k + 1) % len(seq)] is a1  # the old face runs a0 -> a1, so the band must run a1 -> a0
    uv = bm.loops.layers.uv.active
    faces = []
    for t in tris:
        f = bm.faces.new(t[::-1] if flip else t)
        side = next((g for g in f.verts[0].link_faces if g is not f), None)
        if side is not None:
            f.material_index = side.material_index
            f.smooth = True
        if uv is not None:
            for loop in f.loops:
                src = next((lp for g in loop.vert.link_faces if g is not f for lp in g.loops if lp.vert is loop.vert),
                           None)
                if src is not None:
                    loop[uv].uv = src[uv].uv
        faces.append(f)
    # The body's faces should then run the other way along their edges too; count any that don't.
    band, bset, against = set(faces), set(B), 0
    for f in faces:
        vs = list(f.verts)
        for k in range(3):
            p, q = vs[k], vs[(k + 1) % 3]
            if p in bset and q in bset:
                old = next((g for g in p.link_faces if g not in band and q in g.verts), None)
                if old is not None:
                    s = list(old.verts)
                    against += s[(s.index(p) + 1) % len(s)] is q
    return faces, against


def neck_height_weights(arm, z):
    """rig_head.neck_weights' hat functions along spine_03 -> neck_01 -> neck_02 -> head, for one height."""
    b = arm.data.bones
    n1, n2, top = b["neck_01"].head_local.z, b["neck_02"].head_local.z, b["head"].head_local.z
    knots = [n1 - (n2 - n1), (n1 + n2) / 2, (n2 + top) / 2, top + 0.6 * (top - n2)]
    chain = rh.NECK_CHAIN
    if z <= knots[0]:
        return {chain[0]: 1.0}
    if z >= knots[-1]:
        return {chain[-1]: 1.0}
    j = max(k for k in range(3) if knots[k] <= z)
    t = (z - knots[j]) / (knots[j + 1] - knots[j])
    t = t * t * (3 - 2 * t)
    return {chain[j]: 1.0 - t, chain[j + 1]: t}


def reweight_seam(mesh, arm, seam, cx, cy, below=0.04, above=0.012, core=0.008):
    """Blends each skin vertex (the biggest connected part) near the seam toward the height weights: fully within
    `core` of it, fading out to `below` under it and `above` over it (the chin and its jaw weights sit just above).
    Hair and other parts keep their weights."""
    isl = islands(mesh.data)
    skin = isl == np.bincount(isl).argmax()
    names = {g.index: g.name for g in mesh.vertex_groups}
    groups = {g.name: g for g in mesh.vertex_groups}
    for n in rh.NECK_CHAIN:
        if n not in groups:
            groups[n] = mesh.vertex_groups.new(name=n)
            names[groups[n].index] = n
    done = 0
    for v in mesh.data.vertices:
        dz = v.co.z - seam
        if not skin[v.index] or not -below < dz < above or math.hypot(v.co.x - cx, v.co.y - cy) > 2 * NECK_REACH:
            continue
        if abs(dz) <= core:
            a = 1.0
        else:
            t = (abs(dz) - core) / ((below if dz < 0 else above) - core)
            a = 1.0 - t * t * (3 - 2 * t)
        w = {names[g.group]: g.weight * (1.0 - a) for g in v.groups}
        for n, x in neck_height_weights(arm, v.co.z).items():
            w[n] = w.get(n, 0.0) + a * x
        top = sorted(w.items(), key=lambda kv: -kv[1])[:4]
        total = sum(x for _, x in top) or 1.0
        for g in list(v.groups):
            mesh.vertex_groups[g.group].remove([v.index])
        for n, x in top:
            if x / total > 1e-4:
                groups[n].add([v.index], x / total, "REPLACE")
        done += 1
    return done


def neck_map(z0, z1, length, ramp=0.01):
    """The height map that shortens the neck from z0 (its base) to z1 (under the chin) to `length`: the local scale
    eases from 1 to k = length / (z1 - z0) over `ramp` either side of z0 and back over `ramp` either side of z1, so
    the surface keeps its slope there (no crease under the jaw); below it nothing moves, above it everything drops
    by z1 - z0 - length."""
    k = length / (z1 - z0)
    g = np.linspace(z0 - ramp, z1 + ramp, 4001)
    on = np.clip((g - (z0 - ramp)) / (2 * ramp), 0, 1)
    off = np.clip(((z1 + ramp) - g) / (2 * ramp), 0, 1)
    w = np.minimum(on * on * (3 - 2 * on), off * off * (3 - 2 * off))
    W = np.concatenate([[0.0], np.cumsum((w[1:] + w[:-1]) / 2 * np.diff(g))])
    W *= (z1 - z0) / W[-1]

    def f(z):
        return z - (1 - k) * np.interp(z, g, W)
    return f


def shorten_neck(mesh, arm, f):
    """Applies the height map f to the mesh, every shape key (they hold positions, so the same map) and every bone."""
    me = mesh.data
    for kb in me.shape_keys.key_blocks:
        co = np.empty(len(kb.data) * 3, dtype=np.float64)
        kb.data.foreach_get("co", co)
        co = co.reshape(-1, 3)
        co[:, 2] = f(co[:, 2])
        kb.data.foreach_set("co", co.astype(np.float32).ravel())
    co = np.empty(len(me.vertices) * 3, dtype=np.float32)
    me.shape_keys.reference_key.data.foreach_get("co", co)
    me.vertices.foreach_set("co", co)
    me.update()
    nb.activate(arm)
    bpy.ops.object.mode_set(mode="EDIT")
    for eb in arm.data.edit_bones:
        h, t = eb.head.copy(), eb.tail.copy()
        h.z, t.z = float(f(h.z)), float(f(t.z))
        eb.head, eb.tail = h, t
    bpy.ops.object.mode_set(mode="OBJECT")


def append_body(path):
    with bpy.data.libraries.load(path) as (src, dst):
        dst.objects = list(src.objects)
        dst.actions = list(src.actions)
    objs = [o for o in dst.objects if o is not None]
    for o in objs:
        bpy.context.scene.collection.objects.link(o)
    return objs, [a for a in dst.actions if a is not None]


def preview(arm, mesh, outdir):
    arm.animation_data.action = None
    for kb in mesh.data.shape_keys.key_blocks:
        kb.value = 0.0
    rc.reset_pose(arm)
    bpy.context.view_layer.update()
    rh.render_views([mesh], outdir, [("full_front", 0), ("full_34", 35)], size=1024)
    neck = arm.data.bones["neck_02"].head_local
    focus = Vector((neck.x, neck.y, arm.data.bones["head"].head_local.z))
    for tag, pose in (("rest", {}), ("bent", {"neck_01": (20, 0, 0), "neck_02": (15, 0, 0), "head": (10, 0, 0)}),
                      ("turned", {"neck_01": (0, 25, 0), "neck_02": (0, 20, 0), "head": (0, 15, 0)}),
                      ("tilted", {"neck_01": (0, 0, 15), "neck_02": (0, 0, 12), "head": (0, 0, 8)})):
        rc.reset_pose(arm)
        rc.set_pose(arm, pose)
        bpy.context.view_layer.update()
        rh.render_views([mesh], outdir, [("neck_%s_front" % tag, 0), ("neck_%s_side" % tag, 90),
                                         ("neck_%s_back" % tag, 180)], size=640, focus=focus, span=0.32)
    # the face shapes on the joined mesh: a shape that lost its deltas at the seam would open the neck here
    keys = mesh.data.shape_keys.key_blocks
    sign = rh.jaw_open_sign(arm, mesh)
    for shape in ("jawOpen", "eyeBlinkLeft"):
        rc.reset_pose(arm)
        keys[shape].value = 1.0
        if shape == "jawOpen":
            pb = arm.pose.bones["jaw"]
            pb.rotation_mode = "XYZ"
            pb.rotation_euler = (math.radians(12 * sign), 0.0, 0.0)
        bpy.context.view_layer.update()
        rh.render_views([mesh], outdir, [("neck_%s_front" % shape, 0), ("neck_%s_side" % shape, 90)], size=640,
                        focus=focus, span=0.32)
        keys[shape].value = 0.0
    rc.reset_pose(arm)


def main():
    a = _args()
    arm_h, mesh_h = _rigged(bpy.data.objects)
    for o in [o for o in bpy.data.objects if o not in (arm_h, mesh_h)]:
        bpy.data.objects.remove(o, do_unlink=True)
    for act in list(bpy.data.actions):  # the body's clips replace the bust's
        bpy.data.actions.remove(act)
    objs, actions = append_body(os.path.abspath(a["body"]))
    arm_b, mesh_b = _rigged(objs)
    for o in objs:
        if o not in (arm_b, mesh_b):
            bpy.data.objects.remove(o, do_unlink=True)

    # where to cut
    me_h, me_b = mesh_h.data, mesh_b.data
    Vh = np.array([v.co[:] for v in me_h.vertices])
    Vb = np.array([v.co[:] for v in me_b.vertices])
    isl = islands(me_h)
    skin_id = np.bincount(isl).argmax()
    skin = isl == skin_id
    Eh = np.array([e.vertices[:] for e in me_h.edges if skin[e.vertices[0]]], dtype=int)
    Eb = np.array([e.vertices[:] for e in me_b.edges], dtype=int)
    Bh, Bb = boundary_edges(me_h, skin), boundary_edges(me_b)
    seam, sh, sb, overlap = pick_seam(Vh, Eh, Bh, Vb, Eb, Bb, a["seam"])
    g = a["gap"]

    # the bust across onto the body's neck centre, its head joints with it
    off = np.array([sb["c"][0] - sh["c"][0], sb["c"][1] - sh["c"][1], 0.0])
    for kb in me_h.shape_keys.key_blocks:
        co = np.empty(len(kb.data) * 3, dtype=np.float32)
        kb.data.foreach_get("co", co)
        kb.data.foreach_set("co", (co.reshape(-1, 3) + off).astype(np.float32).ravel())
    me_h.update()

    # the body's neck tapers to the bust's section over the 4 cm below the seam
    cx, cy = sb["c"]
    rw, rd = sh["w"] / sb["w"], sh["d"] / sb["d"]
    taper = 0.04
    for v in me_b.vertices:
        t = (v.co.z - (seam - taper)) / taper
        if t <= 0:
            continue
        s = min(t, 1.0)
        s = s * s * (3 - 2 * s)
        v.co.x = cx + (v.co.x - cx) * (1 + s * (rw - 1))
        v.co.y = cy + (v.co.y - cy) * (1 + s * (rd - 1))
    me_b.update()

    cut(mesh_h, seam + g, keep_above=True, only=skin)
    cut(mesh_b, seam - g, keep_above=False)

    # the head and everything under it take the bust's rest positions
    face = [b.name for b in arm_h.data.bones if b.name == "head" or any(p.name == "head" for p in b.parent_recursive)]
    nb.activate(arm_h)
    bpy.ops.object.mode_set(mode="EDIT")
    rest = {n: (arm_h.data.edit_bones[n].head.copy(), arm_h.data.edit_bones[n].tail.copy(),
                arm_h.data.edit_bones[n].roll) for n in face}
    bpy.ops.object.mode_set(mode="OBJECT")
    nb.activate(arm_b)
    bpy.ops.object.mode_set(mode="EDIT")
    shift = Vector(off)
    for n, (h, t, r) in rest.items():
        eb = arm_b.data.edit_bones[n]
        eb.use_connect = False
        eb.head, eb.tail, eb.roll = h + shift, t + shift, r
    bpy.ops.object.mode_set(mode="OBJECT")

    # one mesh: the body joins the bust (which keeps its shape keys, the body taking none)
    for m in list(mesh_h.modifiers):
        if m.type == "ARMATURE":
            mesh_h.modifiers.remove(m)
    nb.activate(mesh_h)
    mesh_b.select_set(True)
    bpy.ops.object.join()
    mesh = bpy.context.view_layer.objects.active
    for kb in mesh.data.shape_keys.key_blocks:
        kb.value = 0.0
    mod = next((m for m in mesh.modifiers if m.type == "ARMATURE"), None) or mesh.modifiers.new("Armature", "ARMATURE")
    mod.object = arm_b
    mesh.parent = arm_b

    bm = bmesh.new()
    bm.from_mesh(mesh.data)
    top, bot = ring(bm, seam + g, cx, cy), ring(bm, seam - g, cx, cy)
    if len(top) < 8 or len(bot) < 8:
        raise SystemExit("join_head: open neck rings not found (bust %d, body %d vertices)" % (len(top), len(bot)))
    band, against = zip_rings(bm, top, bot, cx, cy)
    if against:
        print("join_head: %d band edges wind against the body's faces (normals differ across the seam)" % against)
    bm.to_mesh(mesh.data)
    mesh.data.update()
    bm.free()
    if a["neck"]:
        z0, z1, length = a["neck"]
        f = neck_map(z0, z1, length)
        shorten_neck(mesh, arm_b, f)
        print("join_head: neck %.4f..%.4f shortened to %.4f, head down %.4f, seam now z %.4f"
              % (z0, z1, length, z1 - z0 - length, float(f(seam))))
        seam = float(f(seam))
    reweighted = reweight_seam(mesh, arm_b, seam, cx, cy)
    bpy.data.objects.remove(arm_h, do_unlink=True)

    arm_b.name = os.path.splitext(os.path.basename(a["out"]))[0] + "_rig"
    mesh.name = os.path.splitext(os.path.basename(a["out"]))[0]
    for act in actions:
        act.use_fake_user = True
    arm_b.animation_data_create()
    arm_b.animation_data.action = None
    rc.reset_pose(arm_b)
    bpy.context.scene.frame_set(0)
    bpy.context.view_layer.update()
    out = os.path.abspath(a["out"])
    os.makedirs(os.path.dirname(out), exist_ok=True)
    glb = nb.export_rig(out, {"export_morph_normal": False, "export_force_sampling": False})
    rc.write_rigexpect(arm_b, mesh, os.path.splitext(out)[0] + ".rigexpect.json")
    info = ml.check_glb(glb, face=True)
    if a["blend"]:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(a["blend"]))
    if a["preview"]:
        preview(arm_b, mesh, os.path.abspath(a["preview"]))
    print("join_head: %s, seam z %.4f (overlap %.4f..%.4f), bust neck %.4f x %.4f, body %.4f x %.4f, bust moved "
          "(%.4f, %.4f), rings (%d, %d) -> %d triangles, reweighted %d, %d vertices, %d bytes -> %s"
          % (info, seam, overlap[0], overlap[1], sh["w"], sh["d"], sb["w"], sb["d"], off[0], off[1], len(top),
             len(bot), len(band), reweighted, len(mesh.data.vertices), os.path.getsize(glb), glb))


if __name__ == "__main__":
    main()
