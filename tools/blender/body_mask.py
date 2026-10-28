"""Blender -b: which MH body faces a worn garment hides, as UV0 triangles for tools/art/strip_mask_png.py (the mask UE's
SkeletalMeshEditorSubsystem.strip_lod_geometry reads; LODUtilities.cpp strips a triangle when every mask pixel its UV0
footprint touches is black, and any non-black pixel it touches keeps it -- the header's comment says the reverse).
    blender -b --factory-startup --python tools/blender/body_mask.py -- <mh_body.fbx> <garment.fbx> <out prefix> [reach=0.15] [margin=0.05]
        [mode=trousers|top] [also=<other prefix>_strip_faces.npy] [seam=<margin>] [seam_neck=<m>]
A LOD0 body vertex is hidden when all of these hold:
  - a ray along its normal, started 1 cm under the skin, leaves the garment (meets a face from behind) within `reach` m;
  - that hit is more than `margin` from the garment's openings (its boundary loops: the waist and the two cuffs, the
    three largest; a smaller loop, a pinhole under 3 cm where a seam weld left an edge open, is reported, not used);
  - the vertex sits above its leg's cuff top + margin and below the waist's bottom - margin;
  - it carries no weight on an arm, hand, foot, ankle, neck, head or upper-spine bone (the A-pose hands hang by the thighs);
  - it is more than `seam` m (default `margin`) from the body's own open edge, its seam with the face mesh (found with the
    UV splits welded by position; on an MH body it runs over the shoulder tops and dips at the front): the face's edge
    sits on it, and a void under that edge would show down a loose collar.
    seam_neck=<m> (top mode) keeps that band only within <m> of the neck opening (the highest boundary loop): a top
    rests on the shoulders, so the band shows nothing elsewhere, and there it poked through the cloth (Devin's tunic in
    the run pose, the shoulder front beside the collar, 4-8 mm under the cloth and 11 cm or more below the opening).
A face is stripped when all its vertices are hidden; so is any kept island that lies wholly in the band with no such
weight and nothing within `seam` of the seam (the garment is closed there except at its openings).
mode=top is for a shirt or tunic: every boundary loop is an opening (neck, cuffs, hem with its slits, any small hole),
there is no height band, and only forearm, hand, head and leg weight keeps a vertex (the shoulders, upper arms and upper
spine are what a top covers, and the legs move out from under a hem). The fill still needs that floor: without it, an
uncovered forearm cut off by the strip would be an island with no keeping weight.
also= adds another garment's strip on the same body (an earlier run's <prefix>_strip_faces.npy, LOD0 face indices) and
fills the kept islands again, for a body worn under both; this garment's own strip goes to <out>_own_strip_faces.npy.
Writes <out>_tris.npz (LOD0 UV0 triangles: strip, keep), <out>_lods.npz (the other LODs' UV0 triangles, each wanted
stripped when the LOD0 face nearest its centre is), <out>_report.json, <out>_strip_faces.npy and Workbench renders of
the body with the strip in red (and with the garment on)."""
import sys, json, math, bpy
import numpy as np
from mathutils import Vector, kdtree
from mathutils.bvhtree import BVHTree

argv = sys.argv[sys.argv.index("--") + 1:]
BODY, GARM, OUT = argv[:3]
opt = dict(a.split("=", 1) for a in argv[3:] if "=" in a)
REACH = float(opt.get("reach", 0.15))
MARGIN = float(opt.get("margin", 0.05))
MODE = opt.get("mode", "trousers")
assert MODE in ("trousers", "top"), "mode is trousers or top"
ALSO = opt.get("also")
BAD = ("clavicle", "upperarm", "lowerarm", "hand", "wrist", "thumb", "index", "middle", "ring", "pinky", "foot", "ball",
       "toe", "ankle", "neck", "head", "spine_03", "spine_04", "spine_05")
if MODE == "top":
    BAD = ("lowerarm", "hand", "wrist", "thumb", "index", "middle", "ring", "pinky", "head", "thigh", "calf", "foot",
           "ball", "toe", "ankle")
bpy.ops.wm.read_factory_settings(use_empty=True)


def imp(p):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=p)
    return [o for o in bpy.data.objects if o not in before]


bo = imp(BODY)
barm = next(o for o in bo if o.type == "ARMATURE")
lod0 = max((o for o in bo if o.type == "MESH"), key=lambda o: len(o.data.vertices))
go = imp(GARM)
gm = next(o for o in go if o.type == "MESH")
dg = bpy.context.evaluated_depsgraph_get()


def evaluated(o):  # world-space vertices, vertex normals, polygons (rest pose)
    oe = o.evaluated_get(dg); me = oe.to_mesh()
    n = len(me.vertices)
    co = np.empty(n * 3); me.vertices.foreach_get("co", co); co = co.reshape(-1, 3)
    vn = np.empty(n * 3); me.vertex_normals.foreach_get("vector", vn); vn = vn.reshape(-1, 3)
    P = [tuple(p.vertices) for p in me.polygons]
    M = np.array(o.matrix_world); M3 = M[:3, :3]
    w = co @ M3.T + M[:3, 3]
    wn = vn @ np.linalg.inv(M3)  # normals transform by the inverse transpose
    wn /= np.linalg.norm(wn, axis=1, keepdims=True) + 1e-12
    oe.to_mesh_clear()
    return w, wn, P


Bv, Bn, Bp = evaluated(lod0)
Gv, Gn, Gp = evaluated(gm)
nb = len(Bv)
print("MASK body", lod0.name, nb, len(Bp), "garment", gm.name, len(Gv), len(Gp), "reach", REACH, "margin", MARGIN)

# the garment's openings: boundary edges (edges with one face), split into loops
from collections import defaultdict
ecount = defaultdict(int)
for f in Gp:
    for i in range(len(f)):
        a, b = f[i], f[(i + 1) % len(f)]
        ecount[(min(a, b), max(a, b))] += 1
adj = defaultdict(list)
for (a, b), c in ecount.items():
    if c == 1:
        adj[a].append(b); adj[b].append(a)
seen = set(); loops = []
for s in adj:
    if s in seen:
        continue
    comp = []; st = [s]; seen.add(s)
    while st:
        v = st.pop(); comp.append(v)
        for w in adj[v]:
            if w not in seen:
                seen.add(w); st.append(w)
    loops.append(np.array(comp))
loops.sort(key=lambda L: -Gv[L, 2].mean())
openings = [(len(L), round(float(Gv[L, 2].min()), 3), round(float(Gv[L, 2].max()), 3), round(float(Gv[L, 0].mean()), 3)) for L in loops]
print("MASK openings", openings)
holes = []
if MODE == "trousers":  # the waist and the cuffs are the three largest loops; any other is a pinhole a seam weld left
    main = sorted(sorted(range(len(loops)), key=lambda k: -len(loops[k]))[:3])  # keeps the height order
    holes = [(len(loops[k]), round(float(np.ptp(Gv[loops[k]], axis=0).max()), 4)) for k in range(len(loops)) if k not in main]
    assert len(main) == 3 and all(s < 0.03 for _n, s in holes), "expected the waist, two cuffs and at most pinholes"
    loops = [loops[k] for k in main]
    if holes:
        print("MASK pinholes (vertices, span m), not openings", holes)
open_pts = Gv[np.concatenate(loops)]
if MODE == "trousers":
    waist, cuffs = loops[0], loops[1:]
    waist_zmin = float(Gv[waist, 2].min())
    cuff_top = {float(np.sign(Gv[L, 0].mean())): float(Gv[L, 2].max()) for L in cuffs}

# the garment's winding: do its faces point away from the body?
bvh_b = BVHTree.FromPolygons(Bv.tolist(), Bp)
bvh_g = BVHTree.FromPolygons(Gv.tolist(), Gp)
outw = 0; tot = 0
for fi in range(0, len(Gp), 13):
    f = Gp[fi]
    c = Vector(Gv[list(f)].mean(axis=0).tolist())
    a, b, d = (Vector(Gv[f[k]].tolist()) for k in (0, 1, 2))
    n = (b - a).cross(d - a)
    q, _n, _i, _d = bvh_b.find_nearest(c)
    if q is None or n.length < 1e-12:
        continue
    tot += 1; outw += 1 if (c - q).dot(n) > 0 else 0
frac = outw / max(tot, 1)
print("MASK garment winding outward %d / %d (%.3f)" % (outw, tot, frac))
orient = 1.0 if frac > 0.9 else (-1.0 if frac < 0.1 else 0.0)  # 0 = mixed: the leave-the-garment test is skipped

# bone weights: the vertices on arm, hand, foot, ankle, neck, head and upper-spine bones are never stripped
bad_g = {vg.index for vg in lod0.vertex_groups if any(k in vg.name.lower() for k in BAD)}
badw = np.zeros(nb)
for v in lod0.data.vertices:
    for g in v.groups:
        if g.group in bad_g:
            badw[v.index] += g.weight

# the body's own open edge: edges with one face once the UV splits are welded by position (unwelded, every UV seam
# would read as an edge); the vertices within SEAM of it are never stripped
SEAM = float(opt.get("seam", MARGIN))
weld = np.unique(np.round(Bv / 1e-5).astype(np.int64), axis=0, return_inverse=True)[1].ravel()
wcount = defaultdict(int)
for f in Bp:
    for i in range(len(f)):
        a, b = weld[f[i]], weld[f[(i + 1) % len(f)]]
        wcount[(min(a, b), max(a, b))] += 1
seam_w = np.unique(np.array([e for e, c in wcount.items() if c == 1], dtype=np.int64).ravel())
first = np.zeros(weld.max() + 1, np.int64); first[weld[::-1]] = np.arange(nb)[::-1]
seam_pts = Bv[first[seam_w]]
near_seam = np.zeros(nb, bool)
if len(seam_pts):
    d2 = (Bv ** 2).sum(1)[:, None] + (seam_pts ** 2).sum(1)[None, :] - 2.0 * Bv @ seam_pts.T
    near_seam = d2.min(1) < SEAM ** 2
seam_rep = {"edge_verts": int(len(seam_pts)), "band": SEAM, "kept_verts": int(near_seam.sum()),
            "z": [round(float(seam_pts[:, 2].min()), 4), round(float(seam_pts[:, 2].max()), 4)] if len(seam_pts) else None}
if "seam_neck" in opt and len(seam_pts):
    assert MODE == "top", "seam_neck is for a top (its highest boundary loop is the neck)"
    SEAM_NECK = float(opt["seam_neck"])
    kn = kdtree.KDTree(len(loops[0]))
    for k, i in enumerate(loops[0]):
        kn.insert(Vector(Gv[i].tolist()), k)
    kn.balance()
    to_neck = lambda P: np.array([kn.find(Vector(p.tolist()))[2] for p in P])
    de = to_neck(seam_pts)
    band = np.nonzero(near_seam)[0]
    near_seam[band[to_neck(Bv[band]) >= SEAM_NECK]] = False
    seam_rep.update(neck=SEAM_NECK, band_verts=int(len(band)), kept_verts=int(near_seam.sum()),
                    edge_to_neck_min_p50_max=[round(float(x), 4) for x in (de.min(), np.median(de), de.max())])
print("MASK seam", json.dumps(seam_rep))

# per vertex tests
hit_ok = np.zeros(nb, bool); hit_front = np.zeros(nb, bool); hit_d = np.full(nb, np.nan); hit_open = np.full(nb, np.nan)
kd = kdtree.KDTree(len(open_pts))
for i, p in enumerate(open_pts):
    kd.insert(Vector(p.tolist()), i)
kd.balance()
for i in range(nb):
    p = Vector(Bv[i].tolist()); n = Vector(Bn[i].tolist())
    loc, nrm, _fi, dist = bvh_g.ray_cast(p - n * 0.01, n, REACH + 0.01)
    if loc is None:
        continue
    if orient != 0.0 and nrm.dot(n) * orient <= 0:  # met a garment face from the front: outside the garment
        hit_front[i] = True
        continue
    hit_d[i] = dist - 0.01
    hit_open[i] = kd.find(loc)[2]
    hit_ok[i] = True
z = Bv[:, 2]; side = np.sign(Bv[:, 0])
if MODE == "trousers":
    ctop = np.array([cuff_top.get(float(s), max(cuff_top.values())) for s in side])
    in_band = (z > ctop + MARGIN) & (z < waist_zmin - MARGIN)
else:
    in_band = np.ones(nb, bool)
far_open = np.nan_to_num(hit_open, nan=0.0) > MARGIN
clean = badw <= 0.02
hidden = hit_ok & far_open & in_band & clean & ~near_seam
print("MASK vertices hit %d, far from openings %d, in band %d, clean %d, off the seam %d -> hidden %d" % (
    hit_ok.sum(), (hit_ok & far_open).sum(), (hit_ok & in_band).sum(), (hit_ok & clean).sum(), (hit_ok & ~near_seam).sum(),
    hidden.sum()))

# faces: stripped when all their vertices are hidden
me = lod0.data
F = [list(p.vertices) for p in me.polygons]
strip_f = np.array([bool(hidden[f].all()) for f in F])
print("MASK polygon sizes", {int(k): int(c) for k, c in enumerate(np.bincount([len(f) for f in F])) if c})

# kept islands lying wholly in the band with no arm/hand/foot weight are stripped too: the garment is closed there except
# at its openings, so they are covered (the crotch rays meet the garment's crotch panel from the front)
okv = in_band & clean & ~near_seam


def fill(strip_f):
    edge_faces = defaultdict(list)
    for fi, f in enumerate(F):
        if not strip_f[fi]:
            for k in range(len(f)):
                a, b = f[k], f[(k + 1) % len(f)]
                edge_faces[(min(a, b), max(a, b))].append(fi)
    comp = np.full(len(F), -1); islands = []
    for s in range(len(F)):
        if strip_f[s] or comp[s] >= 0:
            continue
        comp[s] = len(islands); st = [s]; members = []
        while st:
            fi = st.pop(); members.append(fi); f = F[fi]
            for k in range(len(f)):
                a, b = f[k], f[(k + 1) % len(f)]
                for g in edge_faces[(min(a, b), max(a, b))]:
                    if comp[g] < 0:
                        comp[g] = len(islands); st.append(g)
        islands.append(members)
    filled = []
    for members in islands:
        vs = np.unique(np.concatenate([F[fi] for fi in members]))
        if okv[vs].all():
            strip_f[members] = True
            filled.append({"faces": len(members), "verts": len(vs), "ray_missed": int((~hit_ok[vs] & ~hit_front[vs]).sum()),
                           "ray_met_front": int(hit_front[vs].sum()), "z": [round(float(z[vs].min()), 3), round(float(z[vs].max()), 3)]})
    print("MASK kept components %d (largest %s), filled %s" % (len(islands), sorted(len(m) for m in islands)[-5:], filled))
    return filled


filled = fill(strip_f)
own = strip_f.copy()
also_rep = None
if ALSO:  # another garment's strip on the same body: the union, and the islands it now cuts off
    other = np.load(ALSO)
    assert other.max() < len(F), "the other strip is for a different body"
    new = int((~strip_f[other]).sum())
    strip_f[other] = True
    also_rep = {"file": ALSO.replace("\\", "/").rsplit("/", 1)[-1], "faces": int(len(other)), "new": new,
                "filled_islands": fill(strip_f)}
    np.save(OUT + "_own_strip_faces.npy", np.nonzero(own)[0])
    print("MASK also", json.dumps(also_rep), "own", int(own.sum()), "union", int(strip_f.sum()))

# UV0 triangles: a quad gives both diagonals' triangles (whichever split UE made, its triangles are covered); n-gons all fans
uvl = me.uv_layers[0].data
luv = np.empty(len(me.loops) * 2); uvl.foreach_get("uv", luv); luv = luv.reshape(-1, 2)
S, K = [], []
for p in me.polygons:
    li = list(p.loop_indices); n = len(li)
    fans = range(n) if n > 3 else (0,)
    for r in fans:
        for k in range(1, n - 1):
            (S if strip_f[p.index] else K).append(luv[[li[r], li[(r + k) % n], li[(r + k + 1) % n]]])
S = np.array(S); K = np.array(K)
np.savez_compressed(OUT + "_tris.npz", strip=S, keep=K)

# the other LODs' UV0 triangles, each wanted stripped when the LOD0 face nearest its polygon's centre is
lods = sorted((o for o in bo if o.type == "MESH"), key=lambda o: -len(o.data.vertices))
print("MASK lods", [(o.name, len(o.data.vertices), len(o.data.polygons)) for o in lods])
extra = {}
for k, o in enumerate(lods[1:], 1):
    V, _vn, P = evaluated(o)
    luvk = np.empty(len(o.data.loops) * 2); o.data.uv_layers[0].data.foreach_get("uv", luvk); luvk = luvk.reshape(-1, 2)
    T, W, C = [], [], []
    for p in o.data.polygons:
        li = list(p.loop_indices); c = V[list(p.vertices)].mean(axis=0)
        _q, _n, fi, _d = bvh_b.find_nearest(Vector(c.tolist()))
        for j in range(1, len(li) - 1):
            T.append(luvk[[li[0], li[j], li[j + 1]]]); W.append(bool(strip_f[fi])); C.append(c)
    extra["lod%d_tris" % k] = np.array(T); extra["lod%d_want" % k] = np.array(W); extra["lod%d_c" % k] = np.array(C)
np.savez_compressed(OUT + "_lods.npz", **extra)
fz = np.array([Bv[list(p.vertices), 2].mean() for p in me.polygons])
rep = {"mode": MODE, "body_vertices": nb, "body_faces": len(me.polygons), "strip_faces": int(strip_f.sum()),
       "own_strip_faces": int(own.sum()), "also": also_rep, "openings": openings, "pinholes": holes,
       "strip_tris": int(len(S)), "keep_tris": int(len(K)), "garment_winding_outward": round(frac, 4), "orient": orient,
       "hidden_vertices": int(hidden.sum()), "filled_islands": filled, "strip_z": [round(float(fz[strip_f].min()), 3), round(float(fz[strip_f].max()), 3)],
       "strip_left_right": [int((strip_f & (np.array([Bv[list(p.vertices), 0].mean() for p in me.polygons]) > 0)).sum()),
                            int((strip_f & (np.array([Bv[list(p.vertices), 0].mean() for p in me.polygons]) <= 0)).sum())],
       "hit_depth_cm_p50_p95_max": [round(float(np.nanpercentile(hit_d[hidden], q)) * 100, 2) for q in (50, 95, 100)],
       "reach": REACH, "margin": MARGIN, "seam": seam_rep}
if MODE == "trousers":
    rep["waist_zmin"] = round(waist_zmin, 4); rep["cuff_tops"] = {str(k): round(v, 4) for k, v in cuff_top.items()}
print("MASK report", json.dumps(rep))
json.dump(rep, open(OUT + "_report.json", "w"), indent=1)

# renders: static world-space copies of the body split into the strip (red) and the rest (skin), object colours
np.save(OUT + "_strip_faces.npy", np.nonzero(strip_f)[0])


def part(name, faces, col):
    m = bpy.data.meshes.new(name)
    m.from_pydata(Bv.tolist(), [], [Bp[i] for i in faces])
    m.update()
    o = bpy.data.objects.new(name, m); bpy.context.scene.collection.objects.link(o)
    o.color = col
    return o


keep_o = part("keep", np.nonzero(~strip_f)[0], (0.78, 0.58, 0.47, 1.0))
strip_o = part("strip", np.nonzero(strip_f)[0], (0.85, 0.08, 0.08, 1.0))
for o in bo:  # the FBX carries every LOD; they would z-fight with the parts
    if o.type == "MESH":
        o.hide_render = True
sc = bpy.context.scene
sc.render.engine = "BLENDER_WORKBENCH"
sc.display.shading.light = "STUDIO"; sc.display.shading.color_type = "OBJECT"
sc.render.resolution_x, sc.render.resolution_y = 700, 1000
gm.color = (0.16, 0.19, 0.25, 1.0)
cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam")); sc.collection.objects.link(cam); sc.camera = cam
cam.data.lens = 50


def head_w(name):
    return barm.matrix_world @ barm.pose.bones[name].head


fwd = head_w("ball_l") - head_w("foot_l"); fwd.z = 0; fwd.normalize()
lat = fwd.cross(Vector((0, 0, 1))).normalized()
pel = head_w("pelvis")


def at(tz):
    return Vector((pel.x, pel.y, tz))


views = [("front", fwd, at(0.62), 3.0, 0.1), ("back", -fwd, at(0.62), 3.0, 0.1), ("side", lat, at(0.62), 3.0, 0.1),
         ("q", (fwd + lat).normalized(), at(0.62), 3.0, 0.1), ("crotch_low", fwd, at(0.72), 1.2, -0.35)]
if MODE == "top":  # the body framed on the garment, the collar from above and each armpit from below
    gz0, gz1 = float(Gv[:, 2].min()), float(Gv[:, 2].max())
    views = [(t, d, at((gz0 + gz1) / 2), 3.0, 0.1) for t, d, _tz, _di, _dz in views[:4]]
    views.append(("collar_high", fwd, at(gz1 - 0.06), 0.9, 0.55))
    for s in "lr":
        sh = head_w("upperarm_" + s); out = sh - pel; out.z = 0; out.normalize()
        views.append(("pit_low_" + s, (fwd * 0.5 + out * 0.6).normalized(), sh + Vector((0, 0, -0.1)), 0.9, -0.5))
for tag_on, show_g, show_s in (("strip", False, True), ("cut", False, False), ("worn", True, False)):
    gm.hide_render = not show_g
    strip_o.hide_render = not show_s
    for tag, d, tgt, dist, dz in views:
        cam.location = tgt + d * dist + Vector((0, 0, dz))
        cam.rotation_euler = (tgt - cam.location).to_track_quat("-Z", "Y").to_euler()
        sc.render.filepath = OUT + "_" + tag_on + "_" + tag + ".png"
        bpy.ops.render.render(write_still=True)
print("MASK_DONE")
