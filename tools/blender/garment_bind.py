"""Skins md_garment pieces (garments.blend, already where the body is in world space) to a MetaHuman Real3 body's own
armature and writes one FBX per piece for UE, headless.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/garment_bind.py -- BIND.json OUT_DIR

BIND.json:
    {"body": "mh_body.fbx", "garments": "md_out/garments.blend",
     "pieces": {"NAME": {"export": "SK_Third_UnderRobe", "drop": ["arm"], "side": "l", "smooth": 12,
                         "smooth_gap": 0.01, "smooth_full": 0.05, "rigid": "foot_l", "limit": 8,
                         "legs": {"below": 0.92, "fade": 0.1, "blend": 0.04, "mid": 0.0, "k": 12, "yscale": 0.3,
                                  "fold": 0.5, "hold": 1}, "keep_below": 0.84,
                         "free_below": {"z": 0.5, "fade": 0.1, "bone": "pelvis"}, "weights_from": "outer.fbx",
                         "liner": {"mid": 0.0, "x": 0.02, "y": [-0.15, -0.05], "z": [0.56, 0.70], "offset": 0.0025}}}}

Paths are read against the spec's folder. Each named piece is appended from the garments file and takes the weights of
the nearest point on the LOD0 body's surface (Data Transfer, nearest face interpolated, as bind_r3 proved on Real3).
"drop" lists bone classes the piece keeps none of (arm: upperarm, lowerarm, hand and the fingers; leg: thigh, calf,
foot, ball; head: neck, head); "side" l or r keeps no bone of the other side (a sleeve, one boot); a vertex left with
under half its weight refills from its neighbours on the piece. "smooth" relaxes the weights over the piece's own mesh
that many times, faded in from "smooth_gap" metres off the body to fully at "smooth_full", so cloth that hangs free (a
skirt between the legs, a sleeve's drop) moves as one sheet while cloth on the skin keeps the skin's weights. "rigid"
puts the whole piece on one bone (a boot shaft's stiff collar: no; a buckle, a holster: yes). "legs" is for a skirt
over both legs: below "below" (rest height, metres; faded in over "fade" metres downwards) each vertex takes the
weights of the nearest body vertex on its own side of the midline x = "mid", blended across the midline over
"blend" metres either side, so each half of the skirt follows its own leg through a stride instead of the nearest
surface flipping between the legs at the centre. With "k" above 1 a vertex averages its k nearest body vertices on
that side (inverse distance), measured with the depth axis y scaled by "yscale", so two layers of a wrap lying over
each other a centimetre or two apart take the same weights and move together instead of crossing. "fold" moves that
share of the lower-leg weights (calf, foot, ball) up to the same side's thigh, so a long hem swings with the thigh
instead of shearing round the bent knee. "hold" 1 keeps the split weights out of the "smooth" relaxing (faded with
the split), since relaxing over each panel's own mesh would part two layers again at their edges; "yscale" 0 makes
the split weights a function of x and z alone. "keep_below" deletes the piece's vertices above that rest height
first (an under-layer variant cut where a closed outer layer always covers it; the full piece stays its own export).
"free_below" puts everything under rest height "z" on one bone ("bone", default pelvis), faded in over "fade" metres
above it, after the leg split and the relaxing: a loose hem below the knee that runtime cloth will move, so a posed
review shows it hanging free from the hips instead of gathered round each shin by the per-leg weights.
"weights_from" (a skinned FBX on the same skeleton, e.g. the outer garment over this one) makes that FBX's largest
mesh, at its bind pose, the Data Transfer source instead of the body: an under-layer fitted a millimetre or two
inside an outer one takes the outer one's weights and so deforms with it, instead of each layer taking the skin's
weights from a different nearest point and crossing in a pose (a tee through a jacket at the armpit in a stride).
Then the weights are cleaned, limited to "limit" (8) bones a vertex and normalised.
"liner" then joins a gusset copied from the body into the piece: the LOD0 faces with every vertex inside the box (rest
metres: |x - "mid"| up to "x", y and z between the given pairs), "offset" metres out along the body's normals, with
the body's own weights on each vertex unchanged (no transfer, no limit), all its UVs on the piece's UV at the vertex
nearest its centre (plain cloth of the piece's colour). Where the body's own skinning parts a seam the cloth can't
follow (a MetaHuman crotch midline is weighted left and right in a zigzag, so in a stride its vertices swing a few
centimetres apart), the liner moves exactly as the skin under it does, so what shows through is cloth, not skin.

Each piece leaves as OUT_DIR/EXPORT.fbx (the piece on the body armature, bind_r3's export settings) with
EXPORT_BC.png (sRGB), EXPORT_N.png (DirectX, green flipped from md_garment's OpenGL map, for UE), EXPORT_N_gl.png
(OpenGL, for Blender) and EXPORT_ORM.png (R occlusion, G roughness, B metal) copied from md_garment's NAME_albedo,
NAME_normal and NAME_orm next to the garments file. OUT_DIR/bound.blend keeps the body and the bound pieces;
OUT_DIR/bind_log.json has, per piece, its triangles, vertices, bones and the largest bones by weight."""
import json
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import kdtree

CLASSES = {"arm": ("upperarm", "lowerarm", "hand", "thumb", "index", "middle", "ring", "pinky"),
           "leg": ("thigh", "calf", "foot", "ball"), "head": ("neck", "head", "FACIAL")}


def log(*a):
    print("garment_bind:", *a, flush=True)


def _spec():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if len(argv) < 2:
        sys.exit("garment_bind: BIND.json OUT_DIR")
    path, out = os.path.abspath(argv[0]), os.path.abspath(argv[1])
    spec = json.load(open(path))
    d = os.path.dirname(path)
    for k in ("body", "garments"):
        spec[k] = os.path.join(d, spec[k])
    for ps in spec["pieces"].values():
        if ps.get("weights_from"):
            ps["weights_from"] = os.path.join(d, ps["weights_from"])
    return spec, out


def weight_source(path, cache):
    """The largest mesh of a skinned FBX on the same skeleton, at its bind pose in world space, armature removed."""
    if path in cache:
        return cache[path]
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    meshes = [o for o in new if o.type == "MESH"]
    if not meshes:
        sys.exit("garment_bind: no mesh in %s" % path)
    src = max(meshes, key=lambda o: len(o.data.vertices))
    mw = src.matrix_world.copy()
    src.parent = None
    src.matrix_world = mw
    src.modifiers.clear()
    for o in new:
        if o is not src:
            bpy.data.objects.remove(o, do_unlink=True)
    activate(src)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    src.hide_render = True
    cache[path] = src
    return src


def activate(o):
    bpy.ops.object.select_all(action="DESELECT")
    o.select_set(True)
    bpy.context.view_layer.objects.active = o


def wverts(o):
    co = np.empty(len(o.data.vertices) * 3)
    o.data.vertices.foreach_get("co", co)
    M = np.array(o.matrix_world)
    return co.reshape(-1, 3) @ M[:3, :3].T + M[:3, 3]


def read_weights(o):
    W = np.zeros((len(o.data.vertices), len(o.vertex_groups)))
    for v in o.data.vertices:
        for e in v.groups:
            W[v.index, e.group] = e.weight
    return W


def write_weights(o, W):
    idx = list(range(len(o.data.vertices)))
    for vg in o.vertex_groups:
        vg.remove(idx)
    for gi in np.nonzero(W.max(0) > 0)[0]:
        vg = o.vertex_groups[int(gi)]
        col = W[:, gi]
        for vi in np.nonzero(col > 0)[0]:
            vg.add([int(vi)], float(col[vi]), "REPLACE")


def neighbours(o):
    e = np.empty(len(o.data.edges) * 2, np.int64)
    o.data.edges.foreach_get("vertices", e)
    e = e.reshape(-1, 2)
    a, b = np.concatenate([e[:, 0], e[:, 1]]), np.concatenate([e[:, 1], e[:, 0]])
    return a, b, np.bincount(a, minlength=len(o.data.vertices)).astype(float)


def relax(W, a, b, deg, alpha, iters):
    """iters passes of W <- W + alpha (mean of the neighbours - W), alpha per vertex (0 keeps a vertex as it is)."""
    for _ in range(iters):
        S = np.zeros_like(W)
        np.add.at(S, a, W[b])
        M = S / np.maximum(deg, 1)[:, None]
        W = W + alpha[:, None] * (M - W)
    return W


def leg_split(o, W, gnames, body, lg):
    """Skirt weights below lg["below"]: each vertex takes the weights of the nearest body vertex (or the inverse-distance
    mean of the lg["k"] nearest, depth scaled by lg["yscale"]) on its own side of the midline, blended across it, with
    lg["fold"] of the lower-leg weights moved to the thigh; faded in from the nearest-surface weights W over
    lg["fade"] metres downwards."""
    below, fade = float(lg.get("below", 0.92)), float(lg.get("fade", 0.1))
    blend, mid = float(lg.get("blend", 0.04)), float(lg.get("mid", 0.0))
    k, fold = max(1, int(lg.get("k", 1))), float(lg.get("fold", 0.0))
    S = np.array([1.0, float(lg.get("yscale", 1.0)), 1.0])
    BW = read_weights(body)
    bnames = [vg.name for vg in body.vertex_groups]
    col = {n: i for i, n in enumerate(gnames)}
    for n in bnames:  # the garment may lack a group the leg weights need
        if n not in col:
            o.vertex_groups.new(name=n)
            col[n] = len(col)
            W = np.hstack([W, np.zeros((len(W), 1))])
            gnames.append(n)
    Wb = np.zeros((len(BW), W.shape[1]))
    for bi, n in enumerate(bnames):
        Wb[:, col[n]] = BW[:, bi]
    BP = wverts(body)
    P = wverts(o)
    zone = BP[:, 2] < below + 0.08
    trees = {}
    for side, sel in (("l", zone & (BP[:, 0] > mid)), ("r", zone & (BP[:, 0] < mid))):
        idx = np.nonzero(sel)[0]
        kd = kdtree.KDTree(len(idx))
        for i in idx:
            kd.insert(BP[i] * S, int(i))
        kd.balance()
        trees[side] = kd
    f = np.clip((below - P[:, 2]) / max(fade, 1e-6), 0, 1)
    f = f * f * (3 - 2 * f)
    t = np.clip((P[:, 0] - mid) / max(blend, 1e-6) * 0.5 + 0.5, 0, 1)  # 1 = the wearer's left (+x)
    t = t * t * (3 - 2 * t)
    lower = {s: [col[g] for g in gnames if g.endswith("_" + s) and g.startswith(("calf", "foot", "ball"))]
             for s in "lr"}

    def near(side, p):
        if k == 1:
            return Wb[trees[side].find(p)[1]]
        hits = trees[side].find_n(p, k)
        d = np.array([h[2] for h in hits])
        w = 1.0 / (d + 0.01) ** 2
        return (w[:, None] * Wb[[h[1] for h in hits]]).sum(0) / w.sum()

    n = 0
    for vi in np.nonzero(f > 0)[0]:
        legw = t[vi] * near("l", P[vi] * S) + (1 - t[vi]) * near("r", P[vi] * S)
        for s in "lr":
            if fold > 0 and lower[s] and "thigh_" + s in col:
                moved = legw[lower[s]] * fold
                legw[lower[s]] -= moved
                legw[col["thigh_" + s]] += moved.sum()
        W[vi] = (1 - f[vi]) * W[vi] + f[vi] * legw
        n += 1
    log("%s: leg-split weights on %d vertices below z %.2f (mid x %.3f, blend %.0f mm, k %d, fold %.2f)"
        % (o.name, n, below, mid, blend * 1000, k, fold))
    return W / np.maximum(W.sum(1, keepdims=True), 1e-12), f

def add_liner(o, lod0, lin):
    """Join to o a copy of the body faces inside the liner box, pushed out along the body normals, with the body's
    weights verbatim and one UV (the piece's, at its vertex nearest the liner's centre). Returns (faces, tris, max
    influences)."""
    mid, hx = float(lin.get("mid", 0.0)), float(lin.get("x", 0.02))
    (y0, y1), (z0, z1) = lin["y"], lin["z"]
    off = float(lin.get("offset", 0.0025))
    BP = wverts(lod0)
    nrm = np.empty(len(lod0.data.vertices) * 3)
    lod0.data.vertices.foreach_get("normal", nrm)
    M3 = np.array(lod0.matrix_world.to_3x3().inverted().transposed())
    N = nrm.reshape(-1, 3) @ M3.T
    N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-12)
    inside = ((np.abs(BP[:, 0] - mid) <= hx) & (BP[:, 1] >= y0) & (BP[:, 1] <= y1) & (BP[:, 2] >= z0)
              & (BP[:, 2] <= z1))
    faces = [tuple(p.vertices) for p in lod0.data.polygons if all(inside[i] for i in p.vertices)]
    if not faces:
        log("WARNING: %s: the liner box holds no body faces; no liner" % o.name)
        return 0, 0, 0
    used = sorted({i for f in faces for i in f})
    remap = {v: k for k, v in enumerate(used)}
    P = BP[used] + N[used] * off
    me = bpy.data.meshes.new(o.name + "_liner")
    me.from_pydata(P.tolist(), [], [[remap[i] for i in f] for f in faces])
    me.update()
    lo = bpy.data.objects.new(o.name + "_liner", me)
    bpy.context.scene.collection.objects.link(lo)
    BW = read_weights(lod0)[used]
    bn = [vg.name for vg in lod0.vertex_groups]
    for gi in np.nonzero(BW.max(0) > 0)[0]:
        vg = lo.vertex_groups.new(name=bn[int(gi)])
        for k in np.nonzero(BW[:, gi] > 0)[0]:
            vg.add([int(k)], float(BW[k, gi]), "REPLACE")
    uvl = o.data.uv_layers.active
    if uvl is not None:
        GP = wverts(o)
        near = int(np.argmin(np.linalg.norm(GP - P.mean(0), axis=1)))
        lv = np.empty(len(o.data.loops), np.int64)
        o.data.loops.foreach_get("vertex_index", lv)
        uv = tuple(uvl.data[int(np.nonzero(lv == near)[0][0])].uv)
        lu = me.uv_layers.new(name=uvl.name)
        for d in lu.data:
            d.uv = uv
    for m in o.data.materials:
        me.materials.append(m)
    activate(o)
    lo.select_set(True)
    bpy.ops.object.join()
    tris = sum(len(f) - 2 for f in faces)
    return len(faces), tris, int((BW > 0).sum(1).max())


def copy_textures(gdir, name, export, out):
    src = {k: os.path.join(gdir, "%s_%s.png" % (name, k)) for k in ("albedo", "normal", "orm")}
    if not os.path.exists(src["albedo"]):
        log("WARNING: %s has no %s; the FBX goes without textures" % (name, src["albedo"]))
        return {}
    done = {}
    for k, suffix, colour in (("albedo", "_BC", True), ("normal", "_N", False), ("orm", "_ORM", False)):
        if not os.path.exists(src[k]):
            continue
        im = bpy.data.images.load(src[k], check_existing=False)
        w, h = im.size
        px = np.empty(w * h * 4, np.float32)
        im.pixels.foreach_get(px)
        px = px.reshape(h, w, 4)
        outs = [(suffix, px)]
        if k == "normal":
            dx = px.copy()
            dx[..., 1] = 1.0 - dx[..., 1]
            outs = [("_N", dx), ("_N_gl", px)]
        for sfx, arr in outs:
            o_im = bpy.data.images.new(export + sfx, w, h, alpha=False, is_data=not colour)
            o_im.pixels.foreach_set(np.ascontiguousarray(arr).ravel())
            o_im.filepath_raw = os.path.join(out, export + sfx + ".png")
            o_im.file_format = "PNG"
            o_im.save()
            done[sfx] = o_im.filepath_raw
        bpy.data.images.remove(im)
    return done


def main():
    spec, out = _spec()
    os.makedirs(out, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=spec["body"])
    arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
    bodies = [o for o in bpy.data.objects if o.type == "MESH"]
    lod0 = max(bodies, key=lambda o: len(o.data.vertices))
    for o in bodies:
        if o is not lod0:
            bpy.data.objects.remove(o, do_unlink=True)
    log("body %s: %d verts; armature %s, %d bones, scale %s" % (lod0.name, len(lod0.data.vertices), arm.name,
        len(arm.data.bones), tuple(round(s, 4) for s in arm.scale)))
    names = list(spec["pieces"])
    with bpy.data.libraries.load(spec["garments"], link=False) as (src, dst):
        missing = [n for n in names if n not in src.objects]
        if missing:
            sys.exit("garment_bind: not in %s: %s" % (spec["garments"], ", ".join(missing)))
        dst.objects = names
    body_kd = kdtree.KDTree(len(lod0.data.vertices))
    for i, p in enumerate(wverts(lod0)):
        body_kd.insert(p, i)
    body_kd.balance()
    gdir = os.path.dirname(spec["garments"])
    report = {}
    wsrc = {}
    for o in dst.objects:
        ps = spec["pieces"][o.name]
        name = o.name
        export = ps.get("export", name)
        bpy.context.scene.collection.objects.link(o)
        o.parent = None
        o.modifiers.clear()
        o.vertex_groups.clear()
        activate(o)
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
        if ps.get("keep_below") is not None:
            zc = float(ps["keep_below"])
            bm = bmesh.new()
            bm.from_mesh(o.data)
            cut = [v for v in bm.verts if v.co.z > zc]
            bmesh.ops.delete(bm, geom=cut, context="VERTS")
            bm.to_mesh(o.data)
            bm.free()
            o.data.update()
            log("%s: %d vertices above z %.3f cut (keep_below)" % (name, len(cut), zc))
        if ps.get("rigid"):
            bone = ps["rigid"]
            if bone not in arm.data.bones:
                sys.exit("garment_bind: %s: no bone %s" % (name, bone))
            o.vertex_groups.new(name=bone).add(list(range(len(o.data.vertices))), 1.0, "REPLACE")
            orphans, n_smooth = 0, 0
        else:
            src = lod0
            if ps.get("weights_from"):
                src = weight_source(ps["weights_from"], wsrc)
                activate(o)
                log("%s: weights from %s (%s, %d verts) instead of the body" % (name, ps["weights_from"], src.name,
                                                                                 len(src.data.vertices)))
            m = o.modifiers.new("dt", "DATA_TRANSFER")
            m.object = src
            m.use_vert_data = True
            m.data_types_verts = {"VGROUP_WEIGHTS"}
            m.vert_mapping = "POLYINTERP_NEAREST"
            m.layers_vgroup_select_src = "ALL"
            m.layers_vgroup_select_dst = "NAME"
            bpy.ops.object.datalayout_transfer(modifier="dt")
            bpy.ops.object.modifier_apply(modifier="dt")
            W = read_weights(o)
            before = W.sum(1)
            gnames = [vg.name for vg in o.vertex_groups]
            dropc = tuple(c for k in ps.get("drop", []) for c in CLASSES[k])
            side = ps.get("side")
            opp = {"l": "_r", "r": "_l"}.get(side)
            for gi, gn in enumerate(gnames):
                if (dropc and gn.startswith(dropc)) or (opp and gn.endswith(opp)):
                    W[:, gi] = 0
            a, b, deg = neighbours(o)
            orph = W.sum(1) <= np.maximum(1e-6, 0.5 * before)
            orphans = int(orph.sum())
            if orphans and not orph.all():
                # refill the orphans from the kept vertices around them, ring by ring
                W[orph] = 0
                for _ in range(200):
                    if not orph.any():
                        break
                    S = np.zeros_like(W)
                    np.add.at(S, a, W[b])
                    got = orph & (S.sum(1) > 0)
                    W[got] = S[got] / S[got].sum(1, keepdims=True)
                    orph &= ~got
            elif orph.all():
                pel = gnames.index("pelvis") if "pelvis" in gnames else None
                if pel is None:
                    o.vertex_groups.new(name="pelvis")
                    W = np.hstack([W, np.zeros((len(W), 1))])
                    pel = W.shape[1] - 1
                    gnames.append("pelvis")
                W[:] = 0
                W[:, pel] = 1
            W = W / np.maximum(W.sum(1, keepdims=True), 1e-12)
            legf = None
            if ps.get("legs"):
                W, legf = leg_split(o, W, gnames, lod0, ps["legs"])
            n_smooth = int(ps.get("smooth", 0))
            if n_smooth:
                P = wverts(o)
                dist = np.array([body_kd.find(p)[2] for p in P])
                g0, g1 = ps.get("smooth_gap", 0.01), ps.get("smooth_full", 0.05)
                t = np.clip((dist - g0) / max(g1 - g0, 1e-6), 0, 1)
                alpha = 0.5 * t * t * (3 - 2 * t)
                if legf is not None and ps["legs"].get("hold"):
                    alpha = alpha * (1 - legf)
                W = relax(W, a, b, deg, alpha, n_smooth)
                log("%s: weights relaxed %d times on %d free-hanging vertices (over %.0f mm off the body), %d partly"
                    % (name, n_smooth, int((t >= 1).sum()), g1 * 1000, int(((t > 0) & (t < 1)).sum())))
            fb = ps.get("free_below")
            if fb:
                zf, ff, bone = float(fb.get("z", 0.5)), float(fb.get("fade", 0.1)), fb.get("bone", "pelvis")
                if bone not in gnames:
                    o.vertex_groups.new(name=bone)
                    W = np.hstack([W, np.zeros((len(W), 1))])
                    gnames.append(bone)
                g = np.clip((zf + ff - wverts(o)[:, 2]) / max(ff, 1e-6), 0, 1)
                g = g * g * (3 - 2 * g)
                W = W * (1 - g)[:, None]
                W[:, gnames.index(bone)] += g
                log("%s: %d vertices on %s below z %.2f, %d fading over %.0f mm above"
                    % (name, int((g >= 1).sum()), bone, zf, int(((g > 0) & (g < 1)).sum()), ff * 1000))
            W[W < 0.001] = 0
            write_weights(o, W)
            activate(o)
            bpy.ops.object.vertex_group_limit_total(group_select_mode="ALL", limit=int(ps.get("limit", 8)))
            bpy.ops.object.vertex_group_normalize_all(lock_active=False)
        liner = None
        if ps.get("liner"):
            liner = add_liner(o, lod0, ps["liner"])
            log("%s: liner of %d body faces (%d tris, up to %d bones a vertex, the body's own weights) joined"
                % (name, *liner))
        # bones by weight, for the log
        Wf = read_weights(o)
        tot = Wf.sum(0)
        top = sorted(((o.vertex_groups[i].name, float(tot[i] / max(len(Wf), 1))) for i in range(len(tot)) if tot[i] > 0),
                     key=lambda kv: -kv[1])
        mw = o.matrix_world.copy()
        o.parent = arm
        o.matrix_world = mw
        am = o.modifiers.new("Armature", "ARMATURE")
        am.object = arm
        o.name = o.data.name = export
        bpy.ops.object.select_all(action="DESELECT")
        o.select_set(True)
        arm.select_set(True)
        bpy.context.view_layer.objects.active = arm
        fbx = os.path.join(out, export + ".fbx")
        bpy.ops.export_scene.fbx(filepath=fbx, use_selection=True, object_types={"ARMATURE", "MESH"},
                                 add_leaf_bones=False, bake_anim=False, use_mesh_modifiers=False,
                                 mesh_smooth_type="FACE", use_armature_deform_only=False)
        tex = copy_textures(gdir, name, export, out)
        tris = sum(len(p.vertices) - 2 for p in o.data.polygons)
        report[export] = {"piece": name, "fbx": fbx, "tris": tris, "verts": len(o.data.vertices), "bones": len(top),
                          "top_bones": [(n, round(w, 3)) for n, w in top[:6]], "orphans_refilled": orphans,
                          "smooth": n_smooth, "rigid": ps.get("rigid"), "textures": tex,
                          "liner_tris": liner[1] if liner else 0}
        log("WROTE %s: %d tris, %d verts, %d bones (%s), %d orphans refilled; textures %s"
            % (fbx, tris, len(o.data.vertices), len(top), ", ".join("%s %.2f" % kv for kv in top[:4]), orphans,
               sorted(tex)))
        if not 3000 <= tris <= 12000:
            log("NOTE: %s has %d triangles, outside 3k-12k" % (export, tris))
    for src in wsrc.values():
        bpy.data.objects.remove(src, do_unlink=True)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "bound.blend"))
    json.dump(report, open(os.path.join(out, "bind_log.json"), "w"), indent=1)


main()
