"""Blender -b: which MH body faces a worn garment hides, as UV0 triangles for tools/art/strip_mask_png.py (the mask UE's
SkeletalMeshEditorSubsystem.strip_lod_geometry reads; LODUtilities.cpp strips a triangle when every mask pixel its UV0
footprint touches is black, and any non-black pixel it touches keeps it -- the header's comment says the reverse).
    blender -b --factory-startup --python tools/blender/body_mask.py -- <mh_body.fbx> <garment.fbx> <out prefix> [reach=0.15] [margin=0.05]
A LOD0 body vertex is hidden when all of these hold:
  - a ray along its normal, started 1 cm under the skin, leaves the garment (meets a face from behind) within `reach` m;
  - that hit is more than `margin` from the garment's openings (its boundary loops: the waist and the two cuffs);
  - the vertex sits above its leg's cuff top + margin and below the waist's bottom - margin;
  - it carries no weight on an arm, hand, foot, ankle, neck, head or upper-spine bone (the A-pose hands hang by the thighs).
A face is stripped when all its vertices are hidden; so is any kept island that lies wholly in the band with no such
weight (the garment is closed there except at its openings). Writes <out>_tris.npz (LOD0 UV0 triangles: strip, keep),
<out>_lods.npz (the other LODs' UV0 triangles, each wanted stripped when the LOD0 face nearest its centre is),
<out>_report.json and Workbench renders of the body with the strip in red (and with the garment on) from five cameras."""
import sys, json, math, bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

argv = sys.argv[sys.argv.index("--") + 1:]
BODY, GARM, OUT = argv[:3]
opt = dict(a.split("=", 1) for a in argv[3:] if "=" in a)
REACH = float(opt.get("reach", 0.15))
MARGIN = float(opt.get("margin", 0.05))
BAD = ("clavicle", "upperarm", "lowerarm", "hand", "wrist", "thumb", "index", "middle", "ring", "pinky", "foot", "ball",
       "toe", "ankle", "neck", "head", "spine_03", "spine_04", "spine_05")
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
waist, cuffs = loops[0], loops[1:]
print("MASK openings", [(len(L), round(float(Gv[L, 2].min()), 3), round(float(Gv[L, 2].max()), 3), round(float(Gv[L, 0].mean()), 3)) for L in loops])
assert len(loops) == 3, "expected the waist and two cuffs"
open_pts = Gv[np.concatenate(loops)]
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

# per vertex tests
hit_ok = np.zeros(nb, bool); hit_front = np.zeros(nb, bool); hit_d = np.full(nb, np.nan); hit_open = np.full(nb, np.nan)
from mathutils import kdtree
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
ctop = np.array([cuff_top.get(float(s), max(cuff_top.values())) for s in side])
in_band = (z > ctop + MARGIN) & (z < waist_zmin - MARGIN)
far_open = np.nan_to_num(hit_open, nan=0.0) > MARGIN
clean = badw <= 0.02
hidden = hit_ok & far_open & in_band & clean
print("MASK vertices hit %d, far from openings %d, in band %d, clean %d -> hidden %d" % (
    hit_ok.sum(), (hit_ok & far_open).sum(), (hit_ok & in_band).sum(), (hit_ok & clean).sum(), hidden.sum()))

# faces: stripped when all their vertices are hidden
me = lod0.data
F = [list(p.vertices) for p in me.polygons]
strip_f = np.array([bool(hidden[f].all()) for f in F])
print("MASK polygon sizes", {int(k): int(c) for k, c in enumerate(np.bincount([len(f) for f in F])) if c})

# kept islands lying wholly in the band with no arm/hand/foot weight are stripped too: the garment is closed there except
# at its openings, so they are covered (the crotch rays meet the garment's crotch panel from the front)
okv = in_band & clean
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
rep = {"body_vertices": nb, "body_faces": len(me.polygons), "strip_faces": int(strip_f.sum()),
       "strip_tris": int(len(S)), "keep_tris": int(len(K)), "garment_winding_outward": round(frac, 4), "orient": orient,
       "waist_zmin": round(waist_zmin, 4), "cuff_tops": {str(k): round(v, 4) for k, v in cuff_top.items()},
       "hidden_vertices": int(hidden.sum()), "filled_islands": filled, "strip_z": [round(float(fz[strip_f].min()), 3), round(float(fz[strip_f].max()), 3)],
       "strip_left_right": [int((strip_f & (np.array([Bv[list(p.vertices), 0].mean() for p in me.polygons]) > 0)).sum()),
                            int((strip_f & (np.array([Bv[list(p.vertices), 0].mean() for p in me.polygons]) <= 0)).sum())],
       "hit_depth_cm_p50_p95_max": [round(float(np.nanpercentile(hit_d[hidden], q)) * 100, 2) for q in (50, 95, 100)],
       "reach": REACH, "margin": MARGIN}
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
views = (("front", fwd, 0.62, 3.0, 0.1), ("back", -fwd, 0.62, 3.0, 0.1), ("side", lat, 0.62, 3.0, 0.1),
         ("q", (fwd + lat).normalized(), 0.62, 3.0, 0.1), ("crotch_low", fwd, 0.72, 1.2, -0.35))
for tag_on, show_g, show_s in (("strip", False, True), ("cut", False, False), ("worn", True, False)):
    gm.hide_render = not show_g
    strip_o.hide_render = not show_s
    for tag, d, tz, dist, dz in views:
        tgt = Vector((pel.x, pel.y, tz))
        cam.location = tgt + d * dist + Vector((0, 0, dz))
        cam.rotation_euler = (tgt - cam.location).to_track_quat("-Z", "Y").to_euler()
        sc.render.filepath = OUT + "_" + tag_on + "_" + tag + ".png"
        bpy.ops.render.render(write_still=True)
print("MASK_DONE")
