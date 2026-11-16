"""Fold bake: simulate gravity folds on one region of a game garment in Blender's cloth solver, then bake the folds
(tangent normal and AO) from that high copy back onto the game mesh's own UVs, and derive the masks the garment master
reads. Headless:

    blender -b GARMENT.blend --factory-startup -t 6 --python-exit-code 1 --python tools/blender/fold_bake.py -- SPEC.json

SPEC.json (lengths in metres, in the blend's world space):
    {"garment": "SK_...",              the game mesh (its armature modifier is dropped: the bake is in the rest pose)
     "collider": "Body_LOD0" | null,    a body the cloth rests on (optional)
     "out": "<output folder>", "name": "prefix",
     "region": {"axis": "y", "min": 0.05, "max": 9, "zmin": 0.95, "zmax": 1.45},
                                        faces whose centre is in the box get folds; the rest stays as it is
     "hi": "grid",                      "grid": a clean sheet cast onto the region ("cell" 0.004, "lift" 0.0015);
                                        "subdiv": the region's own faces cut "subdiv" times (crumples on scan meshes)
     "ao_dist": 0.05,                   AO looks this far for occluders
     "frames": 30, "shrink": -0.05,     solver frames, and the region cloth's shrink: below 0 it grows, the pinned outline
                                        holds it, and the spare cloth buckles into folds under gravity
     "bending": 0.5, "mass": 0.3,       cloth stiffness and weight (leather: bending 1 to 3)
     "max_disp": 0.012,                 the high copy may stand at most this far off the game mesh along its normal
     "res": 2048, "cage": 0.02, "samples": 16,
     "base_normal_gl" or "base_normal_dx": "...png" | null, the piece's own normal map, the folds are laid over it
     "base_orm": "...png" | null,       its ORM (R takes the fold AO)
     "base_wear": "...png" | null,      its wear map (R curvature, 0.5 flat) the fold curvature adds to
     "curv_gain": 2.0, "curv_blur": 6,  fold curvature scale into the wear map, box blur half-width in texels
     "hole_max_m": 0.5,                 an open boundary loop shorter than this is flagged as a hole (FOLD_BAKE_OPEN_BOUNDARY)
     "openings": 3,                     ways through a closed shell the garment should have; more are flagged as holes
     "allow_holes": false,              holes (either kind) stop the run with exit code 3 unless this is true
     "min_z": 0.6, "feather_cm": 3}     min_z: a baked texel tilted further than this from the sheet counts as a miss;
                                        feather_cm: the folds fade to flat over this distance in from the pinned outline

Checks: a missing key, a garment or collider not in the blend, a garment with no faces, a region that selects nothing,
a closed-shell garment with no "openings", a cast grid over GRID_MAX points, frames outside 1..FRAMES_MAX or res outside 64..8192 stop the run with
FOLD_BAKE_FAILED and exit code 2, before anything is simulated or written.

Deterministic: the same spec, blend and Blender build give the same maps. The cloth solver has no random input and
the Cycles seed is fixed (0, not animated). Not checked across machines or thread counts.

Writes into out: <name>_fold_gl.png (the folds alone), <name>_N_gl.png and <name>_N.png (base plus folds, OpenGL and
DirectX green), <name>_ORM.png, <name>_W.png (R curvature: above 0.5 ridge, below crease; G fold AO; B dirt = crease x
AO), <name>_hi.obj (the simulated high copy) and <name>_report.json.
"""
import bpy, bmesh, sys, json, os
import numpy as np

GRID_MAX = 1_500_000  # cast-grid points: 4 mm cells over about 4.9 m x 4.9 m of cloth
FRAMES_MAX = 400


def fail(msg, code=2):
    print("FOLD_BAKE_FAILED: " + msg)
    sys.stdout.flush()
    sys.exit(code)


a = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if not a or not os.path.isfile(a[0]):
    fail("usage: blender -b GARMENT.blend --python fold_bake.py -- SPEC.json (spec file not found)")
try:
    S = json.load(open(a[0]))
except ValueError as e:
    fail("spec %s is not JSON: %s" % (a[0], e))
for key in ("garment", "out", "name", "region"):
    if key not in S:
        fail("spec has no \"%s\"" % key)
if not isinstance(S["region"], dict) or not all(isinstance(v, (int, float)) for k, v in S["region"].items() if k != "axis"):
    fail("\"region\" must be an object of numbers (min, max, zmin, zmax) and an axis")
if S["region"].get("axis", "y") not in ("x", "y", "z"):
    fail("region axis must be x, y or z")
if not 1 <= int(S.get("frames", 30)) <= FRAMES_MAX:
    fail("frames %s outside 1..%d" % (S.get("frames"), FRAMES_MAX))
if not 64 <= int(S.get("res", 2048)) <= 8192:
    fail("res %s outside 64..8192" % S.get("res"))
if S.get("hi", "grid") not in ("grid", "subdiv"):
    fail("hi must be grid or subdiv")
if S["garment"] not in bpy.data.objects or bpy.data.objects[S["garment"]].type != "MESH":
    fail("no mesh object %s in the blend" % S["garment"])
if S.get("collider") and S["collider"] not in bpy.data.objects:
    fail("no collider object %s in the blend" % S["collider"])
if not bpy.data.objects[S["garment"]].data.polygons:
    fail("garment %s has no faces" % S["garment"])
for key in ("base_normal_gl", "base_normal_dx", "base_orm", "base_wear"):
    if S.get(key) and not os.path.isfile(S[key]):
        fail("%s %s not found" % (key, S[key]))
OUT, NAME = S["out"], S["name"]
os.makedirs(OUT, exist_ok=True)
rep = {"spec": S}
sc = bpy.context.scene
sc.cycles.seed = 0
sc.cycles.use_animated_seed = False


def mesh_copy(src, name):
    ob = src.copy()
    ob.data = src.data.copy()
    ob.name = ob.data.name = name
    ob.modifiers.clear()
    ob.parent = None
    ob.matrix_world = src.matrix_world.copy()
    sc.collection.objects.link(ob)
    return ob


g = bpy.data.objects[S["garment"]]
for ob in list(sc.objects):  # nothing else renders or collides
    if ob.name not in (S["garment"], S.get("collider")):
        ob.hide_render = True
lo = mesh_copy(g, "fold_lo")
hi = mesh_copy(g, "fold_hi")


def open_loops(ob):  # the mesh's open boundary loops, longest first: hem, cuffs and collar are long; a short loop is a hole
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    bm.transform(ob.matrix_world)
    left = {e for e in bm.edges if e.is_boundary}
    loops = []
    while left:
        stack, part = [left.pop()], []
        while stack:
            e = stack.pop()
            part.append(e)
            for v in e.verts:
                for f in v.link_edges:
                    if f in left:
                        left.remove(f)
                        stack.append(f)
        pts = np.array([v.co for e in part for v in e.verts])
        loops.append({"edges": len(part), "length_m": round(sum(e.calc_length() for e in part), 3),
                      "centre": [round(float(c), 3) for c in pts.mean(0)]})
    bm.free()
    return sorted(loops, key=lambda l: -l["length_m"])


def shell_openings(ob):  # per closed piece (a cloth shell with thickness): genus + 1 = the ways through the shell
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    seen, out = set(), []
    for f0 in bm.faces:
        if f0 in seen:
            continue
        seen.add(f0)
        stack, faces = [f0], 0
        vs, es = set(), set()
        while stack:
            f = stack.pop()
            faces += 1
            vs.update(f.verts)
            es.update(f.edges)
            for e in f.edges:
                for n in e.link_faces:
                    if n not in seen:
                        seen.add(n)
                        stack.append(n)
        if faces >= 500 and not any(e.is_boundary for e in es):
            out.append({"faces": faces, "openings": 1 + (2 - (len(vs) - len(es) + faces)) // 2})
    bm.free()
    return out


# Every garment is checked for holes the body shows through, faults of the mesh itself, flagged even where the bake does
# not reach them. An open surface: an open loop shorter than "hole_max_m" (default 0.5 m) is a hole. A shell with
# thickness has no open edges; there each opening is a tunnel, counted by the genus, and more than the spec's
# "openings" (an open jacket: 3, front with hem and collar, two cuffs) means holes.
rep["open_loops"] = open_loops(lo)
rep["holes"] = [l for l in rep["open_loops"] if l["length_m"] < S.get("hole_max_m", 0.5)]
for l in rep["holes"]:
    print("FOLD_BAKE_OPEN_BOUNDARY hole: %d edges, %.3f m round, centre %s" % (l["edges"], l["length_m"], l["centre"]))
rep["shells"] = shell_openings(lo)
if rep["shells"] and not isinstance(S.get("openings"), int):
    fail("%s is a closed shell: the spec needs \"openings\" (the ways through it the garment should have)" % S["garment"])
for s_ in rep["shells"]:
    want = S["openings"]
    if s_["openings"] > want:
        print("FOLD_BAKE_OPEN_BOUNDARY shell of %d faces has %d openings (spec \"openings\": %s)" % (
            s_["faces"], s_["openings"], want))
        rep.setdefault("shell_holes", []).append(s_)
if (rep["holes"] or rep.get("shell_holes")) and not S.get("allow_holes", False):
    json.dump(rep, open(os.path.join(OUT, NAME + "_report.json"), "w"), indent=1)
    fail("%s has holes (see FOLD_BAKE_OPEN_BOUNDARY and %s_report.json); fix the mesh or set \"allow_holes\": true" % (
        S["garment"], NAME), 3)
g.hide_render = True
for attr in ("visible_camera", "visible_diffuse", "visible_glossy", "visible_shadow", "visible_transmission", "visible_volume_scatter"):
    setattr(lo, attr, False)  # the bake target must not shade the high copy (it sits within max_disp of it)
for ob in (lo, hi):
    ob.data.transform(ob.matrix_world)
    ob.matrix_world.identity()

R = S["region"]
ax = "xyz".index(R.get("axis", "y"))
if S.get("hi", "grid") == "grid":
    # A clean cloth sheet over the region, not the game mesh's own triangles (a scan or generated mesh's uneven,
    # layered triangles crumple in the solver): a square grid of "cell" metres in the plane across the axis, cast along
    # -axis onto the game mesh from outside, set "lift" off it along the surface normal, the outline pinned. The game
    # mesh itself is the collider, so the sheet buckles outward, the way spare cloth folds over what is under it.
    from mathutils.bvhtree import BVHTree
    from mathutils import Vector
    hi.hide_render = True
    tree = BVHTree.FromObject(lo, bpy.context.evaluated_depsgraph_get())
    V = np.empty(len(lo.data.vertices) * 3)
    lo.data.vertices.foreach_get("co", V)
    V = V.reshape(-1, 3)
    u_ax, v_ax = [i for i in range(3) if i != ax]
    sel = (V[:, ax] >= R.get("min", -9)) & (V[:, ax] <= R.get("max", 9)) & (V[:, 2] >= R.get("zmin", -9)) & (V[:, 2] <= R.get("zmax", 9))
    if not sel.any():
        fail("region selects no vertices of %s" % S["garment"])
    cell, lift = S.get("cell", 0.004), S.get("lift", 0.0015)
    if not cell > 0:
        fail("cell must be above 0")
    us = np.arange(V[sel, u_ax].min(), V[sel, u_ax].max(), cell)
    vs = np.arange(max(V[sel, v_ax].min(), R.get("zmin", -9)), min(V[sel, v_ax].max(), R.get("zmax", 9)), cell)
    if len(us) * len(vs) > GRID_MAX:
        fail("cast grid %d x %d over GRID_MAX %d: raise cell or shrink the region" % (len(us), len(vs), GRID_MAX))
    top = V[:, ax].max() + 0.05
    dirv = Vector([0, 0, 0])
    dirv[ax] = -1.0
    idx = -np.ones((len(vs), len(us)), int)
    pts, nrm = [], []
    for j, vv in enumerate(vs):
        for i, uu in enumerate(us):
            o = Vector([0, 0, 0])
            o[u_ax], o[v_ax], o[ax] = uu, vv, top
            hit, n, fi, dist = tree.ray_cast(o, dirv)
            if hit is None or hit[ax] < R.get("min", -9) or n[ax] < 0.3:
                continue
            idx[j, i] = len(pts)
            pts.append(np.array(hit) + np.array(n) * lift)
            nrm.append(np.array(n))
    if not pts:
        fail("no ray from the grid hit %s inside the region" % S["garment"])
    pts, nrm = np.array(pts), np.array(nrm)
    faces = []
    for j in range(len(vs) - 1):
        for i in range(len(us) - 1):
            q = [idx[j, i], idx[j, i + 1], idx[j + 1, i + 1], idx[j + 1, i]]
            if min(q) < 0:
                continue
            e = np.linalg.norm(pts[q] - pts[q[1:] + q[:1]], axis=1)
            if e.max() < cell * 3:  # no face across a step or a gap in the cloth
                faces.append(q)
    if not faces:
        fail("the cast grid made no faces (cell too coarse for the region)")
    fa = np.array(faces)
    fn = np.cross(pts[fa[:, 1]] - pts[fa[:, 0]], pts[fa[:, 3]] - pts[fa[:, 0]])
    if (fn * nrm[fa[:, 0]]).sum() < 0:  # face the sheet the way the surface it was cast on faces
        faces = fa[:, ::-1].tolist()
    used = np.unique(np.array(faces).ravel())
    remap = -np.ones(len(pts), int)
    remap[used] = np.arange(len(used))
    pts, nrm = pts[used], nrm[used]
    me = bpy.data.meshes.new("fold_grid")
    me.from_pydata(pts.tolist(), [], remap[np.array(faces)].tolist())
    me.shade_smooth()  # flat-shaded quads bake as facets
    hi = bpy.data.objects.new("fold_grid", me)
    sc.collection.objects.link(hi)
    cnt = np.bincount(np.array(faces).ravel(), minlength=len(remap))[used]
    nv = len(pts)
    bound = set(np.nonzero(cnt < 4)[0].tolist())
    free = set(range(nv))
    rep["grid"] = [len(us), len(vs), nv, len(faces)]
    col_ob = mesh_copy(lo, "fold_col")
    col_ob.hide_render = True
    col_ob.modifiers.new("fold_col", "COLLISION")
    col_ob.collision.thickness_outer = S.get("lift", 0.0015) * 0.5
else:
    # Region faces on the high copy, cut finer; their outline and every vertex outside them is pinned.
    bm = bmesh.new()
    bm.from_mesh(hi.data)
    bm.faces.ensure_lookup_table()

    def inside(f):
        c = f.calc_center_median()
        return R.get("min", -9) <= c[ax] <= R.get("max", 9) and R.get("zmin", -9) <= c.z <= R.get("zmax", 9)

    reg = [f for f in bm.faces if inside(f)]
    rep["region_faces"] = len(reg)
    if not reg:
        fail("region selects no faces of %s" % S["garment"])
    if S.get("subdiv", 1) > 0 and reg:
        edges = list({e for f in reg for e in f.edges})
        bmesh.ops.subdivide_edges(bm, edges=edges, cuts=int(S.get("subdiv", 1)), use_grid_fill=True)
    bm.faces.ensure_lookup_table()
    free = set()
    for f in bm.faces:
        if inside(f):
            free.update(v.index for v in f.verts)
    bound = set()
    for v in bm.verts:
        if v.index in free and any(not inside(f) for f in v.link_faces):
            bound.add(v.index)
    bm.to_mesh(hi.data)
    bm.free()
    hi.data.update()
    nv = len(hi.data.vertices)
    nrm = None
pin = hi.vertex_groups.new(name="fold_pin")
pinned = [i for i in range(nv) if i not in free or i in bound]
pin.add(pinned, 1.0, "REPLACE")
rep["hi_verts"], rep["free_verts"] = nv, len(free) - len(bound)
P0 = np.empty(nv * 3)
hi.data.vertices.foreach_get("co", P0)
P0 = P0.reshape(-1, 3)
# Feather weight per vertex, in 3D: 0 on the pinned outline, rising to 1 "feather_cm" in from it, so the folds fade out at the
# cast edge and nowhere else (a weight in texture space would also fade them at every UV seam). The bake reads it below.
from mathutils.kdtree import KDTree
kd = KDTree(max(len(pinned), 1))
for i in pinned:
    kd.insert(P0[i], i)
kd.balance()
fw = np.zeros(nv, np.float32)
if pinned:
    fm = S.get("feather_cm", 3.0) / 100.0
    for i in free - bound:
        d = kd.find(P0[i])[2]
        t = min(d / fm, 1.0) if fm > 0 else 1.0
        fw[i] = t * t * (3 - 2 * t)
else:
    fw[:] = 1.0
fa_w = hi.data.attributes.new("fold_w", "FLOAT", "POINT")
fa_w.data.foreach_set("value", fw)
if nrm is None:
    N0 = np.empty(nv * 3)
    hi.data.vertices.foreach_get("normal", N0)
    N0 = N0.reshape(-1, 3)
else:
    N0 = nrm

if S.get("collider"):
    col = bpy.data.objects[S["collider"]]
    col.modifiers.new("fold_col", "COLLISION")
    col.collision.thickness_outer = 0.004
cm = hi.modifiers.new("fold_cloth", "CLOTH")
cs = cm.settings
cs.quality = 6
cs.mass = S.get("mass", 0.3)
cs.tension_stiffness = cs.compression_stiffness = 15.0
cs.shear_stiffness = 5.0
cs.bending_stiffness = S.get("bending", 0.5)
cs.vertex_group_mass = "fold_pin"
cs.shrink_min = S.get("shrink", -0.05)
cm.collision_settings.use_collision = bool(S.get("collider"))
cm.collision_settings.distance_min = 0.003
cm.point_cache.frame_start = 1
cm.point_cache.frame_end = int(S.get("frames", 30))
for f in range(1, int(S.get("frames", 30)) + 1):
    sc.frame_set(f)
dg = bpy.context.evaluated_depsgraph_get()
me = bpy.data.meshes.new_from_object(hi.evaluated_get(dg))
P1 = np.empty(nv * 3)
me.vertices.foreach_get("co", P1)
P1 = P1.reshape(-1, 3)
hi.modifiers.clear()
# Keep the high copy on the game mesh: the normal part of the move is capped at max_disp, the sliding part at half that,
# so every texel of the bake still lands on its own spot of the cloth.
d = P1 - P0
dn = (d * N0).sum(1)
dt = d - dn[:, None] * N0
mx = S.get("max_disp", 0.012)
dn = np.clip(dn, -mx, mx)
tl = np.linalg.norm(dt, axis=1)
dt *= (np.minimum(tl, mx * 0.5) / np.maximum(tl, 1e-9))[:, None]
P2 = P0 + dn[:, None] * N0 + dt
hi.data.vertices.foreach_set("co", P2.ravel())
hi.data.update()
fr = np.array(sorted(free - bound), int)
rep["disp_normal_mm"] = [float(np.percentile(np.abs(dn[fr]) * 1000, p)) for p in (50, 90, 99)] if len(fr) else []
bpy.ops.object.select_all(action="DESELECT")
hi.select_set(True)
bpy.context.view_layer.objects.active = hi
try:
    bpy.ops.wm.obj_export(filepath=os.path.join(OUT, NAME + "_hi.obj"), export_selected_objects=True, export_materials=False)
except RuntimeError as e:  # the operator's own failure (path, permissions): the high copy is a by-product, so go on
    rep["obj_export_error"] = str(e)
    print("FOLD_BAKE_WARNING: high copy not exported: %s" % e)

# Bake onto the game mesh's UVs.
res = int(S.get("res", 2048))
sc.render.engine = "CYCLES"
sc.cycles.device = "CPU"
sc.cycles.samples = int(S.get("samples", 16))
sc.render.bake.use_selected_to_active = True
sc.render.bake.cage_extrusion = S.get("cage", 0.02)
sc.render.bake.max_ray_distance = S.get("cage", 0.02) * 2
sc.render.bake.margin = 8
if sc.world is None:
    sc.world = bpy.data.worlds.new("fold_world")
sc.world.light_settings.distance = S.get("ao_dist", 0.05)
mat = bpy.data.materials.new("fold_bake")
mat.use_nodes = True
lo.data.materials.clear()
lo.data.materials.append(mat)
hi.data.materials.clear()
hi.data.materials.append(mat)
tn = mat.node_tree.nodes.new("ShaderNodeTexImage")
mat.node_tree.nodes.active = tn


def bake(kind, colorspace):
    img = bpy.data.images.new("fold_" + kind, res, res, alpha=False, float_buffer=True)
    img.colorspace_settings.name = colorspace
    tn.image = img
    bpy.ops.object.select_all(action="DESELECT")
    hi.select_set(True)
    lo.select_set(True)
    bpy.context.view_layer.objects.active = lo
    if kind == "NORMAL":
        sc.render.bake.normal_space = "TANGENT"
    bpy.ops.object.bake(type=kind)
    px = np.empty(res * res * 4, np.float32)
    img.pixels.foreach_get(px)
    return px.reshape(res, res, 4)[::-1, :, :3]  # top row first, like a PNG


Nf = bake("NORMAL", "Non-Color")
AO = bake("AO", "Non-Color")[..., 0]
# Which texels found the front of the high copy: an emission bake, white on the front face and black on the back
# (misses stay black in every bake; a lining face under the sheet meets its back).
bsdf = next(n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
bsdf.inputs["Emission Color"].default_value = (1, 1, 1, 1)
geo = mat.node_tree.nodes.new("ShaderNodeNewGeometry")
inv = mat.node_tree.nodes.new("ShaderNodeMath")
inv.operation = "SUBTRACT"
inv.inputs[0].default_value = 1.0
mat.node_tree.links.new(geo.outputs["Backfacing"], inv.inputs[1])
att = mat.node_tree.nodes.new("ShaderNodeAttribute")
att.attribute_name = "fold_w"
mul = mat.node_tree.nodes.new("ShaderNodeMath")
mul.operation = "MULTIPLY"
mat.node_tree.links.new(inv.outputs[0], mul.inputs[0])
mat.node_tree.links.new(att.outputs["Fac"], mul.inputs[1])
mat.node_tree.links.new(mul.outputs[0], bsdf.inputs["Emission Strength"])  # front face x feather weight
HIT = bake("EMIT", "Non-Color")[..., 0]


def load(p, n):
    if not p:
        return None
    im = bpy.data.images.load(p)
    im.colorspace_settings.name = "Non-Color"
    w, h = im.size
    px = np.empty(w * h * im.channels, np.float32)
    im.pixels.foreach_get(px)
    px = px.reshape(h, w, im.channels)[::-1]
    if px.shape[2] < n:
        px = np.dstack([px] + [np.ones((h, w), np.float32)] * (n - px.shape[2]))
    return px[..., :n]


def scale(arr, size):  # bilinear, through a Blender image: the folds are broad, so a 2k bake serves a 4k map
    if arr.shape[0] == size:
        return arr
    a3 = arr if arr.ndim == 3 else arr[..., None]
    h, w, c = a3.shape
    rgba = np.ones((h, w, 4), np.float32)
    rgba[..., :c] = a3
    im = bpy.data.images.new("fold_scale", w, h, alpha=True, float_buffer=True)
    im.colorspace_settings.name = "Non-Color"
    im.pixels.foreach_set(np.ascontiguousarray(rgba[::-1]).ravel())
    im.scale(size, size)
    px = np.empty(size * size * 4, np.float32)
    im.pixels.foreach_get(px)
    out = px.reshape(size, size, 4)[::-1, :, :c]
    return out if arr.ndim == 3 else out[..., 0]


def save(arr, name):
    h, w = arr.shape[:2]
    rgba = np.ones((h, w, 4), np.float32)
    rgba[..., :arr.shape[2]] = arr
    im = bpy.data.images.new(name, w, h, alpha=True)
    im.colorspace_settings.name = "Non-Color"
    im.pixels.foreach_set(np.ascontiguousarray(rgba[::-1]).ravel())
    im.filepath_raw = os.path.join(OUT, name + ".png")
    im.file_format = "PNG"
    im.save()


def box(arr, k):  # mean over a (2k+1)-texel square, edges padded with their own values
    c = np.cumsum(np.cumsum(np.pad(arr, k + 1, mode="edge"), 0), 1)
    n = 2 * k + 1
    return ((c[n:, n:] - c[:-n, n:] - c[n:, :-n] + c[:-n, :-n]) / n ** 2)[:arr.shape[0], :arr.shape[1]]


# The folds alone: unit vectors; texels the bake missed are flat, and so are texels whose ray met the sheet at a
# grazing angle ("min_z": a lining or side face under the cast sheet, not a fold on it).
nf = Nf * 2 - 1
miss = (HIT < 0.01) | (np.linalg.norm(nf, axis=2) < 0.5) | (nf[..., 2] < S.get("min_z", 0.6))
# A texel fully dark within ao_dist found the sheet from under it (a lining or inner layer), not a fold on its face.
rep["buried_texels_share"] = float((~miss & (AO < 0.02)).mean())
miss |= AO < 0.02
rep["grazing_texels_share"] = float(((HIT >= 0.01) & (nf[..., 2] < S.get("min_z", 0.6))).mean())
nf[miss] = (0, 0, 1)
AO[miss] = 1.0
# Fade the folds to flat by the sheet's feather weight (baked into HIT), so the cast region has no step at its edge.
wt = np.clip(HIT, 0, 1) * ~miss
nf = nf * wt[..., None] + np.array([0, 0, 1.0]) * (1 - wt[..., None])
AO = AO * wt + (1 - wt)
nf /= np.linalg.norm(nf, axis=2, keepdims=True)
save(nf * 0.5 + 0.5, NAME + "_fold_gl")

# Base plus folds, reoriented normal mapping (the base's detail follows the fold's slope). A DirectX base
# ("base_normal_dx") has its green flipped on load. Everything below runs at the base maps' size.
base = load(S.get("base_normal_gl"), 3)
if base is None and S.get("base_normal_dx"):
    base = load(S["base_normal_dx"], 3)
    base[..., 1] = 1 - base[..., 1]
T = base.shape[0] if base is not None else res
if T != res:
    nf = scale(nf, T)
    nf /= np.linalg.norm(nf, axis=2, keepdims=True)
    AO = scale(AO, T)
    miss = scale(miss.astype(np.float32), T) > 0.5
    res = T
if base is not None:
    b = base * 2 - 1
    t = nf + np.array([0, 0, 1.0])
    u = b * np.array([-1, -1, 1.0])
    nn = t * (t * u).sum(2, keepdims=True) / t[..., 2:3] - u
    nn /= np.linalg.norm(nn, axis=2, keepdims=True)
else:
    nn = nf
save(nn * 0.5 + 0.5, NAME + "_N_gl")
dx = nn.copy()
dx[..., 1] *= -1
save(dx * 0.5 + 0.5, NAME + "_N")

# Fold curvature from the fold normal's divergence in texture space (+ ridge, - crease), smoothed over a few texels.
cv = np.gradient(nf[..., 0], axis=1) - np.gradient(nf[..., 1], axis=0)  # PNG rows run down, the tangent's v up
cv = box(cv, int(S.get("curv_blur", 6))) * (res / 1024.0) * S.get("curv_gain", 2.0)
cv[miss] = 0
W0 = load(S.get("base_wear"), 4)  # its alpha (a tape mask on some pieces) is kept
wr = np.clip((W0[..., 0] if W0 is not None else 0.5) + cv, 0, 1)
crease = np.clip(-cv * 2, 0, 1)
dirt = np.clip(crease * (1.2 - AO), 0, 1)
save(np.dstack([wr, AO, dirt, W0[..., 3] if W0 is not None else np.ones_like(wr)]), NAME + "_W")
orm = load(S.get("base_orm"), 3)
if orm is not None:
    orm = orm.copy()
    orm[..., 0] *= AO
    save(orm, NAME + "_ORM")
rep["fold_texels_share"] = float((~miss & (np.abs(nf[..., 2]) < 0.995)).mean())
rep["curv_p1_p99"] = [float(np.percentile(cv[~miss], 1)), float(np.percentile(cv[~miss], 99))]
rep["ao_p5"] = float(np.percentile(AO[~miss], 5))
json.dump(rep, open(os.path.join(OUT, NAME + "_report.json"), "w"), indent=1)
print("FOLD_BAKE_DONE", json.dumps({k: v for k, v in rep.items() if k not in ("spec", "open_loops")}))
