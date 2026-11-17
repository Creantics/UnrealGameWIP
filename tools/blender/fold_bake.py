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
     "min_z": 0.6, "feather_cm": 3,   min_z: a baked texel tilted further than this from the sheet counts as a miss;
                                        feather_cm: the folds fade to flat over this distance in from the pinned outline
     "outer_only": false,               a shell with thickness whose inner faces share the outer faces' UVs: bake only the
                                        faces whose normal points away from the collider (needs "collider")
     "ease_u": [0.0, 0.0],              grid only: extra cloth across the sheet (along its u axis, not down it), as a share of
                                        its width, at the region's bottom and top (linear between): the springs rest that
                                        much wider than the pinned outline holds them, so the spare width hangs in folds
                                        running down the sheet (gathered or draped cloth), not in shrink's pillow buckles
     "seed": null,                      grid only: folds laid into the sheet before the solver runs, so it relaxes them instead
                                        of buckling at random: {"folds": [{"a": [u, v], "b": [u, v] (the fold's line in the
                                        sheet's u, v axes, m; it fades in from a over "fade_in" of its length, 0.35, and
                                        breaks off past b over "end" m, 0.012), "amp_mm": height off the surface,
                                        "sigma": half-width (m)}, ...], "curl_mm": a lift of the hem below "curl_below" (m),
                                        rising over "curl_width" (m, 0.025) to the edge}. The seeded shape is also the
                                        springs' rest shape (ease_u then spreads it)
     "hem": null,                       grid only: a turned, stitched hem along the cloth's real edges (found by bisecting the
                                        cast rays, so it follows the edge, not the grid's steps), added after the solver:
                                        {"width": m, "raise_mm": its thickness, "round_mm": 1.5 (the rolled edge),
                                        "stitch_at": m in from the edge, "stitch_mm": groove depth, "stitch_sigma": m,
                                        "period": m from one stitch to the next}
     "min_island": 0.0,                 grid only: drop cast pieces smaller than this share of the sheet's faces (stray bits
                                        where the rays hit a lower layer of the cloth)
     "normal_blur": 0.0,                grid only: Gaussian sigma (m) of the normals the sheet is lifted, seeded, hemmed and
                                        clamped along. The cast normals are the game mesh's face normals, so a fold laid along
                                        them tears at every facet edge; blurred over about a fold's width they do not
     "jump": null,                      grid only: the longest quad edge (m) kept as cloth; longer is a step or a gap
                                        (3 x cell when null)
     "snap_edge": false,                grid only: move the sheet's outline vertices onto the bisected edge points, so the
                                        outline follows the cloth's edge, not the grid's steps
     "edge_max": null,                  subdiv only: cut the region's edges in two, again and again, until none is longer than
                                        this (m); "subdiv" cuts first
     "pos_blur": 0.0,                   subdiv only: Gaussian sigma (m) the region's positions are smoothed over, outline and
                                        real edge held (the game mesh's facets, cut finer, would bake as facets)
     "edge_min": 0.0,                   subdiv only: open-boundary pieces shorter than this (m) are not the cloth's edge (slits
                                        between layers of a generated mesh) and get no hem or curl
     "drop_inner": false,               subdiv only: a shell with thickness loses its inner faces inside the region (those
                                        whose normal points at the collider), so the high copy there is one open sheet whose
                                        open boundary is the cloth's real edge (the hem, curl and edge distance read it).
                                        On subdiv, seed, hem and normal_blur work as on a grid (normal_blur smooths the
                                        vertex normals over the mesh's edges)
     "coll_dist": 0.003,                the cloth's collision distance (m); keep it under "lift" on a seeded sheet
     "collide": true,                   false: the sheet ignores the collider and the game mesh (pins, rest shape and the
                                        max_disp clamp alone hold it; for a seeded sheet the solver only relaxes)
     "free_below": null,                a height (m): the cloth's bottom edge below it is left free, not pinned (on a grid, the
                                        vertices with no grid point under them; cuts up the sides stay pinned): a hem that
                                        hangs, so the folds run out through it instead of stopping at a pinned row
     "sim_only": false,                 stop after the high copy (<name>_hi.obj) and the report: look at it before baking
     "overwrite": false}                earlier <name>_*.png, <name>_hi.obj or <name>_report.json in out stop the run unless true

The environment variable FOLD_BAKE_ROOT must name the folder "out" stays under; unset, or "out" outside it, stops the run.

Checks: a missing key, a garment or collider not in the blend, a garment with no faces or no UV layer, a name that is not
a plain prefix, FOLD_BAKE_ROOT unset or "out" outside it, earlier outputs without "overwrite", a region that selects nothing,
a closed-shell garment with no "openings", a cast grid over GRID_MAX points, frames outside 1..FRAMES_MAX or res outside 64..8192 stop the run with
FOLD_BAKE_FAILED and exit code 2, before anything is simulated or written.

Not deterministic: a rerun of one spec on the same blend and Blender build gave a different cloth (median fold lift
3.87 against 3.84 mm, fold texels 4.0 against 3.0%), cause not isolated (the first run's thread count was not
recorded; the report now records it). Keep and judge the maps of a run; the spec alone does not reproduce them. The
Cycles seed is fixed (0, not animated), so the bake itself adds no run-to-run noise.

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
EASE = S.get("ease_u", [0.0, 0.0])
if (not isinstance(EASE, list) or len(EASE) != 2 or not all(isinstance(e, (int, float)) and 0.0 <= e <= 0.5 for e in EASE)):
    fail("ease_u must be [bottom, top], each 0..0.5")
if any(EASE) and S.get("hi", "grid") != "grid":
    fail("ease_u needs hi grid (the sheet's u axis)")
FREE_BELOW = S.get("free_below")
SEED = S.get("seed")
if SEED is not None:
    def fold_ok(f):
        return (isinstance(f, dict) and all(k in f for k in ("a", "b", "amp_mm", "sigma")) and len(f["a"]) == 2
                and len(f["b"]) == 2 and f["a"] != f["b"] and 0 <= f["amp_mm"] <= 30 and f["sigma"] > 0
                and 0 < f.get("fade_in", 0.35) <= 1 and f.get("end", 0.012) > 0)
    if (not isinstance(SEED, dict) or not all(fold_ok(f) for f in SEED.get("folds", []))
            or not 0 <= SEED.get("curl_mm", 0) <= 15 or not SEED.get("curl_width", 0.025) > 0):
        fail("seed must be {folds: [{a, b, amp_mm 0..30, sigma > 0, fade_in 0..1, end > 0}], curl_mm 0..15, curl_width > 0}")
HEM = S.get("hem")
if HEM is not None:
    if (not isinstance(HEM, dict) or not HEM.get("width", 0) > 0 or not 0 <= HEM.get("raise_mm", 2.5) <= 10
            or not HEM.get("round_mm", 1.5) > 0 or not HEM.get("stitch_sigma", 0.001) > 0 or not HEM.get("period", 0.006) > 0):
        fail("hem must be {width > 0, raise_mm 0..10, round_mm > 0, stitch_sigma > 0, period > 0, ...}")
NBLUR, JUMP = S.get("normal_blur", 0.0), S.get("jump")
if (not isinstance(NBLUR, (int, float)) or not 0 <= NBLUR <= 0.2 or (JUMP is not None and not (isinstance(JUMP, (int, float)) and JUMP > 0))
        or ((JUMP is not None or S.get("snap_edge")) and S.get("hi", "grid") != "grid")
        or ((S.get("edge_max") is not None or S.get("drop_inner")) and S.get("hi", "grid") != "subdiv")
        or (S.get("edge_max") is not None and not (isinstance(S["edge_max"], (int, float)) and S["edge_max"] >= 0.0005))
        or (S.get("drop_inner") and not S.get("collider"))):
    fail("normal_blur 0..0.2; jump > 0 or null, snap_edge: hi grid only; edge_max >= 0.0005, drop_inner (needs a collider): hi subdiv only")
MIN_ISLAND = S.get("min_island", 0.0)
if not isinstance(MIN_ISLAND, (int, float)) or not 0 <= MIN_ISLAND < 1:
    fail("min_island must be 0..1")
if FREE_BELOW is not None and not isinstance(FREE_BELOW, (int, float)):
    fail("free_below must be a height in metres or null")
if S["garment"] not in bpy.data.objects or bpy.data.objects[S["garment"]].type != "MESH":
    fail("no mesh object %s in the blend" % S["garment"])
if S.get("collider") and S["collider"] not in bpy.data.objects:
    fail("no collider object %s in the blend" % S["collider"])
if S.get("outer_only", False) and not S.get("collider"):
    fail("outer_only needs a collider: inner faces are the ones whose normal points at it")
if not bpy.data.objects[S["garment"]].data.polygons:
    fail("garment %s has no faces" % S["garment"])
for key in ("base_normal_gl", "base_normal_dx", "base_orm", "base_wear"):
    if S.get(key) and not os.path.isfile(S[key]):
        fail("%s %s not found" % (key, S[key]))
if not bpy.data.objects[S["garment"]].data.uv_layers:
    fail("garment %s has no UV layer: the folds bake onto its own UVs" % S["garment"])
OUT, NAME = S["out"], S["name"]
if not isinstance(NAME, str) or not NAME or NAME != os.path.basename(NAME) or NAME in (".", ".."):
    fail("name %r must be a plain file prefix (no folders)" % (NAME,))
ROOT = os.environ.get("FOLD_BAKE_ROOT", "")
if not ROOT:
    fail("FOLD_BAKE_ROOT is not set: set it to the folder the output must stay under")
root_r, out_r = os.path.realpath(ROOT), os.path.realpath(OUT)
if os.path.normcase(os.path.commonpath([root_r, out_r])) != os.path.normcase(root_r):
    fail("out %s is not under FOLD_BAKE_ROOT" % OUT)
if os.path.isdir(OUT) and not S.get("overwrite", False):
    old = sorted(f for f in os.listdir(OUT) if f.startswith(NAME + "_") and
                 (f.endswith(".png") or f == NAME + "_hi.obj" or f == NAME + "_report.json"))
    if old:
        fail("%d earlier outputs of %s in out (%s ...): set \"overwrite\": true in the spec to replace them" % (len(old), NAME, old[0]))
os.makedirs(OUT, exist_ok=True)
rep = {"spec": S}
sc = bpy.context.scene
rep["blender"], rep["threads"] = bpy.app.version_string, [sc.render.threads_mode, sc.render.threads]
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
    miss = np.zeros((len(vs), len(us)), bool)
    pts, nrm = [], []
    for j, vv in enumerate(vs):
        for i, uu in enumerate(us):
            o = Vector([0, 0, 0])
            o[u_ax], o[v_ax], o[ax] = uu, vv, top
            hit, n, fi, dist = tree.ray_cast(o, dirv)
            if hit is None:
                miss[j, i] = True  # no cloth here: the cell next to it holds a real edge
                continue
            if hit[ax] < R.get("min", -9):
                miss[j, i] = True  # past the region (the far side of the garment): an edge of this sheet too
                continue
            if n[ax] < 0.3:
                continue
            idx[j, i] = len(pts)
            pts.append(np.array(hit))
            nrm.append(np.array(n))
    if not pts:
        fail("no ray from the grid hit %s inside the region" % S["garment"])
    pts, nrm = np.array(pts), np.array(nrm)
    if NBLUR > 0:  # normalised convolution over the grid's hit cells, separable Gaussian
        k = int(np.ceil(3 * NBLUR / cell))
        ker = np.exp(-0.5 * (np.arange(-k, k + 1) * cell / NBLUR) ** 2)
        has = idx >= 0
        G = np.zeros((len(vs), len(us), 4))
        G[has, :3], G[has, 3] = nrm, 1.0
        for axis in (0, 1):
            G = np.apply_along_axis(lambda r: np.convolve(r, ker, mode="same"), axis, G)
        nb = G[has, :3] / G[has, 3:4]
        nb /= np.linalg.norm(nb, axis=1, keepdims=True)
        rep["normal_blur_deg_p50_p99"] = [round(float(x), 1) for x in np.percentile(
            np.degrees(np.arccos(np.clip((nb * nrm).sum(1), -1, 1))), [50, 99])]
        nrm = nb
    pts = pts + nrm * lift
    faces = []
    for j in range(len(vs) - 1):
        for i in range(len(us) - 1):
            q = [idx[j, i], idx[j, i + 1], idx[j + 1, i + 1], idx[j + 1, i]]
            if min(q) < 0:
                continue
            e = np.linalg.norm(pts[q] - pts[q[1:] + q[:1]], axis=1)
            if e.max() < (JUMP or cell * 3):  # no face across a step or a gap in the cloth
                faces.append(q)
    if not faces:
        fail("the cast grid made no faces (cell too coarse for the region)")
    fa = np.array(faces)
    fn = np.cross(pts[fa[:, 1]] - pts[fa[:, 0]], pts[fa[:, 3]] - pts[fa[:, 0]])
    if (fn * nrm[fa[:, 0]]).sum() < 0:  # face the sheet the way the surface it was cast on faces
        faces = fa[:, ::-1].tolist()
    if MIN_ISLAND > 0:  # the sheet's connected pieces; drop the small ones
        par = list(range(len(pts)))

        def root(x):
            while par[x] != x:
                par[x] = par[par[x]]
                x = par[x]
            return x
        for q in faces:
            r0 = root(q[0])
            for x in q[1:]:
                rx = root(x)
                if rx != r0:
                    par[rx] = r0
        lab = [root(q[0]) for q in faces]
        size = {}
        for l in lab:
            size[l] = size.get(l, 0) + 1
        big = {l for l, c in size.items() if c >= MIN_ISLAND * len(faces)}
        rep["islands"] = [len(size), len(big)]
        faces = [q for q, l in zip(faces, lab) if l in big]
    used = np.unique(np.array(faces).ravel())
    remap = -np.ones(len(pts), int)
    remap[used] = np.arange(len(used))
    pts, nrm = pts[used], nrm[used]
    # the bottom edge of the cloth: a vertex with no grid point under it (a hem, not a cut up the side)
    open_below = {int(remap[idx[j, i]]) for j in range(len(vs)) for i in range(len(us))
                  if idx[j, i] >= 0 and remap[idx[j, i]] >= 0 and (j == 0 or idx[j - 1, i] < 0)}
    # The cloth's real edges, between a kept grid point and a grid point whose ray found no cloth: bisected along that
    # cell side to a fraction of a cell, so a hem drawn from them follows the edge, not the grid's steps. Then walked into
    # chains, each point with its arc length (the stitches' phase).
    from mathutils.kdtree import KDTree as _KD
    EP = []
    if HEM is not None or (SEED is not None and SEED.get("curl_mm", 0)):
        def cast_at(uu, vv):
            o = Vector([0, 0, 0])
            o[u_ax], o[v_ax], o[ax] = uu, vv, top
            h_, n_, f_, d_ = tree.ray_cast(o, dirv)
            return (h_, n_) if h_ is not None and h_[ax] >= R.get("min", -9) and n_[ax] >= 0.3 else None
        for j in range(len(vs)):
            for i in range(len(us)):
                if idx[j, i] < 0 or remap[idx[j, i]] < 0:
                    continue
                for dj, di in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    jj, ii = j + dj, i + di
                    if not (0 <= jj < len(vs) and 0 <= ii < len(us)) or not miss[jj, ii]:
                        continue
                    a_, b_, best = 0.0, 1.0, None
                    for _ in range(8):
                        m_ = 0.5 * (a_ + b_)
                        r_ = cast_at(us[i] + m_ * di * cell, vs[j] + m_ * dj * cell)
                        if r_ is None:
                            b_ = m_
                        else:
                            a_, best = m_, r_
                    best = best or cast_at(us[i], vs[j])
                    if best:
                        EP.append(np.array(best[0]) + np.array(best[1]) * lift)
    EP = np.array(EP).reshape(-1, 3)
    ES = np.zeros(len(EP))
    if len(EP):
        kde = _KD(len(EP))
        for k, q in enumerate(EP):
            kde.insert(q, k)
        kde.balance()
        seen = np.zeros(len(EP), bool)
        arc, chains = 0.0, 0
        for k0 in range(len(EP)):
            if seen[k0]:
                continue
            k, chains = k0, chains + 1
            seen[k], ES[k] = True, arc
            while True:
                nxt = next(((kk, dd) for q, kk, dd in kde.find_n(EP[k], 8) if not seen[kk] and dd < cell * 3), None)
                if nxt is None:
                    break
                k, arc = nxt[0], arc + nxt[1]
                seen[k], ES[k] = True, arc
            arc += 1.0
        rep["edge"] = {"points": int(len(EP)), "chains": chains}

        def edge_near(P):
            out = [kde.find(q) for q in P]
            return np.array([o[2] for o in out]), np.array([o[1] for o in out], int)
    if S.get("snap_edge") and len(EP):  # the outline onto the bisected edge, where one is within a cell and a half
        cnt0 = np.bincount(remap[np.array(faces)].ravel(), minlength=len(pts))
        sn = 0
        for k in np.nonzero(cnt0 < 4)[0]:
            q, kk, dd = kde.find(pts[k])
            if dd < cell * 1.5:
                pts[k] = np.array(q)
                sn += 1
        rep["snapped"] = sn
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

    u_ax, v_ax = [i for i in range(3) if i != ax]
    reg = [f for f in bm.faces if inside(f)]
    rep["region_faces"] = len(reg)
    if not reg:
        fail("region selects no faces of %s" % S["garment"])
    if S.get("drop_inner"):
        from mathutils.bvhtree import BVHTree
        _col = bpy.data.objects[S["collider"]]
        _body = BVHTree.FromObject(_col, bpy.context.evaluated_depsgraph_get())
        _to, _from = _col.matrix_world.inverted(), _col.matrix_world
        inner = []
        for f in reg:
            c = f.calc_center_median()
            q = _body.find_nearest(_to @ c)[0]
            if q is not None and f.normal.dot(c - _from @ q) < 0:
                inner.append(f)
        bmesh.ops.delete(bm, geom=inner, context="FACES")  # their loose edges and vertices go too
        bm.faces.ensure_lookup_table()
        reg = [f for f in bm.faces if inside(f)]
        rep["inner_dropped"] = len(inner)
    if S.get("subdiv", 1) > 0 and reg:
        edges = list({e for f in reg for e in f.edges})
        bmesh.ops.subdivide_edges(bm, edges=edges, cuts=int(S.get("subdiv", 1)), use_grid_fill=True)
    if S.get("edge_max"):
        for _ in range(12):
            long_ = list({e for f in bm.faces if inside(f) for e in f.edges if e.calc_length() > S["edge_max"]})
            if not long_:
                break
            bmesh.ops.subdivide_edges(bm, edges=long_, cuts=1, use_grid_fill=True)
            bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 4])
        rep["edge_max_left"] = len(long_)
    bm.faces.ensure_lookup_table()
    bm.verts.index_update()
    # the cloth's real edge inside the region: its open boundary (with drop_inner, where the outer sheet ends)
    adj = {}
    for e in bm.edges:
        if len(e.link_faces) == 1 and inside(e.link_faces[0]):
            a_, b_ = e.verts[0].index, e.verts[1].index
            adj.setdefault(a_, []).append(b_)
            adj.setdefault(b_, []).append(a_)
    ek = sorted(adj)
    bm.verts.ensure_lookup_table()
    EPc = {k: np.array(bm.verts[k].co) for k in ek}
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
    if S.get("pos_blur", 0) > 0:  # the game mesh's flat facets, cut finer, are still flat: diffuse the positions inside the
        # region (its outline and the cloth's real edge held), about 2 (sigma / edge)^2 half-steps
        Pp = np.empty(nv * 3)
        hi.data.vertices.foreach_get("co", Pp)
        Pp = Pp.reshape(-1, 3)
        EE = np.empty(len(hi.data.edges) * 2, int)
        hi.data.edges.foreach_get("vertices", EE)
        EE = EE.reshape(-1, 2)
        mv = np.zeros(nv, bool)
        mv[list(free)] = True
        mv[list(bound)] = False
        mv[ek] = False
        EE = EE[mv[EE[:, 0]] | mv[EE[:, 1]]]
        el = float(np.median(np.linalg.norm(Pp[EE[:, 0]] - Pp[EE[:, 1]], axis=1)))
        its = int(min(2000, np.ceil(2 * (S["pos_blur"] / max(el, 1e-5)) ** 2)))
        deg = np.bincount(EE.ravel(), minlength=nv).astype(float)[:, None]
        P_ = Pp.copy()
        for _ in range(its):
            S_ = np.zeros_like(P_)
            np.add.at(S_, EE[:, 0], P_[EE[:, 1]])
            np.add.at(S_, EE[:, 1], P_[EE[:, 0]])
            Q_ = 0.5 * P_ + 0.5 * np.where(deg > 0, S_ / np.maximum(deg, 1), P_)
            P_[mv] = Q_[mv]
        rep["pos_blur"] = [its, round(float(np.percentile(np.linalg.norm(P_ - Pp, axis=1)[mv], 99) * 1000), 2)]
        hi.data.vertices.foreach_set("co", P_.ravel())
        hi.data.update()
    nrm = None
    open_below = None
    EP = np.array([EPc[k] for k in ek]).reshape(-1, 3)
    ES = np.zeros(len(EP))
    if len(EP):  # arc length along each boundary loop (the stitches' phase)
        pos = {k: i for i, k in enumerate(ek)}
        seen, arc, chains = set(), 0.0, 0
        for k0 in ek:
            if k0 in seen:
                continue
            chains += 1
            k = k0
            seen.add(k)
            ES[pos[k]] = arc
            while True:
                nxt = next((m for m in adj[k] if m not in seen), None)
                if nxt is None:
                    break
                arc += float(np.linalg.norm(EPc[nxt] - EPc[k]))
                k = nxt
                seen.add(k)
                ES[pos[k]] = arc
            arc += 1.0
        rep["edge"] = {"points": int(len(EP)), "chains": chains}
        if S.get("edge_min", 0) > 0:  # boundary pieces shorter than this are slits between layers, not the cloth's edge
            comp, clen = {}, []
            for k0 in ek:
                if k0 in comp:
                    continue
                c, st, ln = len(clen), [k0], 0.0
                comp[k0] = c
                while st:
                    k = st.pop()
                    for m in adj[k]:
                        ln += 0.5 * float(np.linalg.norm(EPc[m] - EPc[k]))
                        if m not in comp:
                            comp[m] = c
                            st.append(m)
                clen.append(ln)
            keep = np.array([clen[comp[k]] >= S["edge_min"] for k in ek])
            EP, ES = EP[keep], ES[keep]
            rep["edge"]["kept"] = [int(keep.sum()), int(sum(1 for x in clen if x >= S["edge_min"])), len(clen)]
    if len(EP):
        from mathutils.kdtree import KDTree as _KD
        kde = _KD(len(EP))
        for k, q in enumerate(EP):
            kde.insert(q, k)
        kde.balance()

        def edge_near(P):
            out = [kde.find(q) for q in P]
            return np.array([o[2] for o in out]), np.array([o[1] for o in out], int)
pin = hi.vertex_groups.new(name="fold_pin")
Z0 = np.empty(nv * 3)
hi.data.vertices.foreach_get("co", Z0)
Z0 = Z0.reshape(-1, 3)[:, 2]
hem = set() if FREE_BELOW is None else {i for i in bound if Z0[i] < FREE_BELOW and (open_below is None or i in open_below)}
pinned = [i for i in range(nv) if i not in free or (i in bound and i not in hem)]
moving = sorted(set(range(nv)) - set(pinned))  # free inside, plus a free hem
rep["free_hem_verts"] = len(hem)
pin.add(pinned, 1.0, "REPLACE")
rep["hi_verts"], rep["free_verts"] = nv, nv - len(pinned)
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
    for i in moving:
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
    if NBLUR > 0:  # diffuse the vertex normals over the mesh's edges: about 2 (sigma / edge)^2 half-steps
        EE = np.empty(len(hi.data.edges) * 2, int)
        hi.data.edges.foreach_get("vertices", EE)
        EE = EE.reshape(-1, 2)
        fm_ = np.zeros(nv, bool)
        fm_[list(free)] = True
        EE = EE[fm_[EE[:, 0]] & fm_[EE[:, 1]]]  # inside the region only: past it the shell turns over (inner faces)
        el = float(np.median(np.linalg.norm(P0[EE[:, 0]] - P0[EE[:, 1]], axis=1)))
        its = int(min(4000, np.ceil(2 * (NBLUR / max(el, 1e-5)) ** 2)))
        deg = np.bincount(EE.ravel(), minlength=nv).astype(float)[:, None]
        N1 = N0.copy()
        for _ in range(its):
            S_ = np.zeros_like(N1)
            np.add.at(S_, EE[:, 0], N1[EE[:, 1]])
            np.add.at(S_, EE[:, 1], N1[EE[:, 0]])
            N1 = 0.5 * N1 + 0.5 * np.where(deg > 0, S_ / np.maximum(deg, 1), N1)
            N1 /= np.maximum(np.linalg.norm(N1, axis=1, keepdims=True), 1e-9)
        rep["normal_blur_its"] = [its, round(el * 1000, 2), [round(float(x), 1) for x in np.percentile(
            np.degrees(np.arccos(np.clip((N1 * N0).sum(1), -1, 1))), [50, 99])]]
        N0 = N1
else:
    N0 = nrm
PB = P0  # the sheet the solver starts from: the cast, or the cast with seeded folds
if SEED is not None:
    h = np.zeros(nv)
    UV = P0[:, [u_ax, v_ax]]
    for fo in SEED.get("folds", []):
        A, D = np.array(fo["a"], float), np.array(fo["b"], float) - np.array(fo["a"], float)
        L = float(np.linalg.norm(D))
        Q = UV - A
        t = (Q @ D) / (L * L)  # 0 at a, 1 at b
        dp = np.abs(Q[:, 0] * D[1] - Q[:, 1] * D[0]) / L  # off the fold's line
        x = np.clip(t / fo.get("fade_in", 0.35), 0.0, 1.0)
        w = np.where(t > 1.0, np.exp(-0.5 * ((t - 1.0) * L / fo.get("end", 0.012)) ** 2), x * x * (3 - 2 * x))
        h += fo["amp_mm"] / 1000.0 * np.exp(-0.5 * (dp / fo["sigma"]) ** 2) * w
    if SEED.get("curl_mm", 0) and len(EP):
        e_, k_ = edge_near(P0)
        c_ = np.clip(1.0 - e_ / SEED.get("curl_width", 0.025), 0.0, 1.0) ** 2 * (P0[:, 2] < SEED.get("curl_below", 9))
        h += SEED["curl_mm"] / 1000.0 * c_
    h[pinned] = 0.0  # the outline stays where it was cast
    PB = P0 + N0 * h[:, None]
    hi.data.vertices.foreach_set("co", PB.ravel())
    hi.data.update()
    rep["seed"] = {"folds": len(SEED.get("folds", [])), "max_mm": round(float(h.max() * 1000), 1)}

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
if any(EASE):
    # The rest shape: the sheet spread along u about its middle, by ease_u at its height. The cloth starts from the cast
    # sheet (the basis) and its springs rest at the spread lengths, so the pinned outline holds it narrower than it wants.
    hi.shape_key_add(name="fold_basis", from_mix=False)
    rest = hi.shape_key_add(name="fold_rest", from_mix=False)
    vmin, vmax = PB[:, v_ax].min(), PB[:, v_ax].max()
    t = (P0[:, v_ax] - vmin) / max(vmax - vmin, 1e-9)
    e = EASE[0] + (EASE[1] - EASE[0]) * t
    uc = 0.5 * (PB[:, u_ax].min() + PB[:, u_ax].max())
    PR = PB.copy()
    PR[:, u_ax] = uc + (P0[:, u_ax] - uc) * (1.0 + e)
    rest.data.foreach_set("co", PR.ravel())
    cs.rest_shape_key = rest
    rep["ease_u"] = EASE
cm.collision_settings.use_collision = bool(S.get("collider")) and bool(S.get("collide", True))
cm.collision_settings.distance_min = S.get("coll_dist", 0.003)
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
if hi.data.shape_keys:
    hi.shape_key_clear()  # the basis key would otherwise hold the vertices where the cast left them
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
if HEM is not None and len(EP):
    # the turned hem: a band "width" in from the real edge, rolled over "round_mm" at the edge, raised "raise_mm", with a
    # dashed stitch groove along it ("period" apart, phase from the edge's arc length)
    e_, k_ = edge_near(P0)
    rnd = np.sqrt(1.0 - (1.0 - np.clip(e_ / (HEM.get("round_mm", 1.5) / 1000.0), 0.0, 1.0)) ** 2)
    x = np.clip((HEM["width"] - e_) / 0.002, 0.0, 1.0)
    band = HEM.get("raise_mm", 2.5) / 1000.0 * rnd * x * x * (3 - 2 * x)
    dash = np.clip((0.5 - 0.5 * np.cos(2 * np.pi * ES[k_] / HEM.get("period", 0.006))) * 1.6, 0.0, 1.0)
    groove = (HEM.get("stitch_mm", 0.6) / 1000.0 * dash
              * np.exp(-0.5 * ((e_ - HEM.get("stitch_at", 0.008)) / HEM.get("stitch_sigma", 0.001)) ** 2))
    P2 = P2 + N0 * (band - groove)[:, None]
    rep["hem"] = {"verts": int((band > 1e-5).sum()), "stitch_verts": int((groove > 1e-5).sum())}
hi.data.vertices.foreach_set("co", P2.ravel())
hi.data.update()
fr = np.array(moving, int)
rep["disp_normal_mm"] = [float(np.percentile(np.abs(dn[fr]) * 1000, p)) for p in (50, 90, 99)] if len(fr) else []
bpy.ops.object.select_all(action="DESELECT")
hi.select_set(True)
bpy.context.view_layer.objects.active = hi
try:
    bpy.ops.wm.obj_export(filepath=os.path.join(OUT, NAME + "_hi.obj"), export_selected_objects=True, export_materials=False)
except RuntimeError as e:  # the operator's own failure (path, permissions): the high copy is a by-product, so go on
    rep["obj_export_error"] = str(e)
    print("FOLD_BAKE_WARNING: high copy not exported: %s" % e)
if S.get("sim_only", False):
    json.dump(rep, open(os.path.join(OUT, NAME + "_report.json"), "w"), indent=1)
    print("FOLD_BAKE_SIM_ONLY " + json.dumps(rep))
    sys.exit(0)

# A shell with thickness whose inner faces reuse the outer faces' UVs bakes each shared texel twice, and the inner face
# (it meets the sheet from under it, a miss) can write last and flatten the fold. "outer_only" drops from the bake target
# the faces whose normal points at the collider, so every shared texel takes the outer face's bake. The game mesh and
# its UVs are not touched (the bake target is a copy).
if S.get("outer_only", False):
    from mathutils.bvhtree import BVHTree
    col = bpy.data.objects[S["collider"]]
    body = BVHTree.FromObject(col, bpy.context.evaluated_depsgraph_get())
    to_col, from_col = col.matrix_world.inverted(), col.matrix_world
    bm = bmesh.new()
    bm.from_mesh(lo.data)
    mw, nw = lo.matrix_world, lo.matrix_world.to_3x3().inverted().transposed()
    inner = []
    for f in bm.faces:
        c = mw @ f.calc_center_median()
        q = body.find_nearest(to_col @ c)[0]
        if q is not None and (nw @ f.normal).dot(c - from_col @ q) < 0:
            inner.append(f)
    rep["outer_only_dropped_faces"] = [len(inner), len(bm.faces)]
    if len(inner) > 0.55 * len(bm.faces):
        fail("outer_only would drop %d of %d faces: the garment's normals face the body (flip them first)" % (len(inner), len(bm.faces)))
    bmesh.ops.delete(bm, geom=inner, context="FACES_ONLY")
    bm.to_mesh(lo.data)
    bm.free()
    lo.data.update()
    print("FOLD_BAKE outer_only: dropped %d of %d faces from the bake target" % tuple(rep["outer_only_dropped_faces"]))

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
