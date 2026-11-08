"""Fits a rigid head prop to a lead's head and skins it 100 % to the body's head bone, headless.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/head_prop.py -- \
        glasses BODY.fbx FACE.fbx GLASSES.fbx OUT_DIR [name=SK_...] [key=value ...]
    blender -b --factory-startup --python-exit-code 1 --python tools/blender/head_prop.py -- \
        mask BODY.fbx FACE.fbx OUT_DIR [name=SK_...] [key=value ...]

Both modes read the head off the face FBX (its LOD0, the skin section and the face skeleton): the eye centres from the
FACIAL_L_Eye / FACIAL_R_Eye bones (else the eye materials), the ears from the FACIAL_*_Ear bones and the vertices they
carry, and a head frame (forward from the ears to the eyes, level; up; the wearer's left).

glasses: the frame axes come from the lens polygons (a material named *lens*): their widest spread is the width, their
thinnest the lens normal; the temples lie behind the lenses and their tips hang below. The nose-rest point is the
underside of the bridge at the middle, on its back face (the FBX origin when that agrees). The front goes on with the
lens centres "lens_eye" mm above the eye centres (negative: below), tipped "tilt" degrees with the bottom towards the
cheeks, the nose-rest point "pad_gap" mm off the nose; it moves forward while any part of the front, or a lens over the
lashes, is nearer than "front_gap". Each temple then turns about its hinge and stretches along its length so its bend
lands on top of the ear root, and bows outward wherever it comes nearer the head than "temple_gap". The lens polygons
and any UCX_ hulls are removed (the lenses wait for a translucent master); the rest is decimated to at most "tris"
triangles in one material. Options: tilt (deg, 4), lens_eye (mm, -2), pad_gap (1), front_gap (1.5), temple_gap (1.5),
temple_start (mm behind the lenses' back where the temples start, 9), tris (11000), textures (folder holding *_BC.png,
*_N.png (DirectX) and *_ORM.png; default the glasses FBX's folder).

mask: a shell over the face from the forehead to under the chin, "half_width" mm each side at its widest, its top "top"
mm above the eyes, its bottom "chin_under" mm below where the chin turns under. The sheet starts "standoff" mm off the
skin, is smoothed "smooth" times (the features soften to a faint nose and lips, the eye hollows fill), is held at least
"min_standoff" mm off the skin and the lashes everywhere and at most "max_off" mm (along forward) near its edge, then
solidified "shell" mm outward with a closed, flat-shaded rim. Planar UVs from the front and a procedural
cracked-porcelain set. Options: half_width (68), top (75), chin_under (2), standoff (4.5), min_standoff (3.0),
max_off (8), shell (3.0), smooth (60), spacing (3.5 mm), tex (2048), seed (3), crack (0..1, how much of the fine crack
net shows, 0.55), colour (sRGB "r,g,b", 0.86,0.84,0.80).

Writes OUT_DIR/NAME.fbx (the prop on the body armature, bind_r3's proven export settings), NAME_BC.png, NAME_N.png
(DirectX, for UE), NAME_N_gl.png (OpenGL, for the .blend), NAME_ORM.png (R occlusion, G roughness, B metal),
NAME_fit.blend (body, face and prop, for the review renders) and NAME_log.json."""
import json
import math
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree
from mathutils.geometry import delaunay_2d_cdt

MM = 0.001


def log(*a):
    print("head_prop:", *a, flush=True)


def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not argv or argv[0] not in ("glasses", "mask"):
        sys.exit("head_prop: glasses BODY.fbx FACE.fbx GLASSES.fbx OUT_DIR | mask BODY.fbx FACE.fbx OUT_DIR [k=v ...]")
    mode = argv[0]
    pos = [a for a in argv[1:] if "=" not in a]
    opt = dict(a.split("=", 1) for a in argv[1:] if "=" in a)
    need = 4 if mode == "glasses" else 3
    if len(pos) != need:
        sys.exit("head_prop: %s takes %d paths, got %d" % (mode, need, len(pos)))
    return mode, [os.path.abspath(p) for p in pos], opt


def fopt(opt, key, default):
    return float(opt.get(key, default))


def wverts(o):
    me = o.data
    co = np.empty(len(me.vertices) * 3)
    me.vertices.foreach_get("co", co)
    M = np.array(o.matrix_world)
    return co.reshape(-1, 3) @ M[:3, :3].T + M[:3, 3]


def unit(v):
    v = np.asarray(v, float)
    return v / max(np.linalg.norm(v), 1e-12)


# -- the head -----------------------------------------------------------------------------------------------------------

def load_head(body_fbx, face_fbx):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=body_fbx)
    arm = next((o for o in bpy.data.objects if o.type == "ARMATURE"), None)
    if arm is None or "head" not in arm.data.bones:
        sys.exit("head_prop: the body FBX has no armature with a head bone")
    bodies = [o for o in bpy.data.objects if o.type == "MESH"]
    lod0 = max(bodies, key=lambda o: len(o.data.vertices))
    for o in bodies:
        if o is not lod0:
            bpy.data.objects.remove(o, do_unlink=True)
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=face_fbx)
    fnew = [o for o in bpy.data.objects if o not in before]
    farm = next((o for o in fnew if o.type == "ARMATURE"), None)
    fmeshes = [o for o in fnew if o.type == "MESH"]
    face0 = max(fmeshes, key=lambda o: len(o.data.vertices))
    for o in fmeshes:
        if o is not face0:
            bpy.data.objects.remove(o, do_unlink=True)
    F = wverts(face0)
    me = face0.data
    mi = np.empty(len(me.polygons), dtype=np.int64)
    me.polygons.foreach_get("material_index", mi)
    mats = [m.name if m else "" for m in me.materials]
    counts = np.bincount(mi, minlength=len(mats))
    skin = [i for i, n in enumerate(mats) if "skin" in n.lower()]
    skin_slot = max(skin, key=lambda i: counts[i]) if skin else int(counts.argmax())
    log("face %s: %d verts; sections %s; skin %s" % (face0.name, len(F),
        ", ".join("%s %d" % (mats[i], counts[i]) for i in range(len(mats))), mats[skin_slot]))
    polys = [tuple(p.vertices) for p in me.polygons]
    skin_polys = [polys[k] for k in np.nonzero(mi == skin_slot)[0]]
    skin_bvh = BVHTree.FromPolygons(F.tolist(), skin_polys)
    lash_slots = [i for i, n in enumerate(mats) if "lash" in n.lower()]
    lash_polys = [polys[k] for k in np.nonzero(np.isin(mi, lash_slots))[0]] if lash_slots else []
    lash_bvh = BVHTree.FromPolygons(F.tolist(), lash_polys) if lash_polys else None
    lash_vids = np.unique(np.concatenate([np.array(p) for p in lash_polys])) if lash_polys else np.zeros(0, int)

    def bone_head(name):
        if farm is not None and name in farm.data.bones:
            return np.array(farm.matrix_world @ farm.data.bones[name].head_local)
        return None

    def slot_centroid(key):
        sl = [i for i, n in enumerate(mats) if key.lower() in n.lower()]
        if not sl:
            return None
        vids = np.unique(np.concatenate([np.array(polys[k]) for k in np.nonzero(np.isin(mi, sl))[0]]))
        return F[vids].mean(0)
    eyeL, eyeR = bone_head("FACIAL_L_Eye"), bone_head("FACIAL_R_Eye")
    src = "bones"
    if eyeL is None or eyeR is None:
        eyeL, eyeR, src = slot_centroid("EyeL"), slot_centroid("EyeR"), "eye materials"
    if eyeL is None or eyeR is None:
        sys.exit("head_prop: no FACIAL_*_Eye bones and no EyeL/EyeR materials on the face")
    # the vertices each ear carries (their strongest weight is an ear bone)
    vg = {g.index: g.name for g in face0.vertex_groups}
    ears = {"L": [], "R": []}
    for v in me.vertices:
        if not v.groups:
            continue
        nm = vg[max(v.groups, key=lambda e: e.weight).group]
        for s in ("L", "R"):
            if nm.startswith("FACIAL_%s_Ear" % s):
                ears[s].append(v.index)
    earL, earR = bone_head("FACIAL_L_Ear"), bone_head("FACIAL_R_Ear")
    if earL is None or earR is None:
        if not ears["L"] or not ears["R"]:
            sys.exit("head_prop: no FACIAL_*_Ear bones or ear vertices on the face")
        earL, earR = F[ears["L"]].mean(0), F[ears["R"]].mean(0)
    up = np.array([0.0, 0.0, 1.0])
    eye_mid = (eyeL + eyeR) / 2
    fwd = eye_mid - (earL + earR) / 2
    fwd = unit(fwd - up * (fwd @ up))
    left = np.cross(up, fwd)  # the wearer's left
    if (eyeL - eyeR) @ left < 0:
        log("WARNING: FACIAL_L_Eye lies on the wearer's right of FACIAL_R_Eye (props are symmetric; going on)")
    # BVH normals: outward if a point far in front reads positive
    loc, nrm, _, _ = skin_bvh.find_nearest(Vector(eye_mid + fwd * 0.3))
    sgn = 1.0 if (Vector(eye_mid + fwd * 0.3) - loc).dot(nrm) > 0 else -1.0
    log("eyes (%s) L %s R %s, IPD %.1f mm; forward %s; ears %d/%d verts; skin normals %s"
        % (src, np.round(eyeL, 4), np.round(eyeR, 4), np.linalg.norm(eyeL - eyeR) / MM, np.round(fwd, 3),
           len(ears["L"]), len(ears["R"]), "outward" if sgn > 0 else "inward (flipped)"))
    return {"arm": arm, "lod0": lod0, "farm": farm, "face0": face0, "F": F, "skin_bvh": skin_bvh, "sgn": sgn,
            "lash_bvh": lash_bvh, "lash_vids": lash_vids, "eyeL": eyeL, "eyeR": eyeR, "eye_mid": eye_mid,
            "fwd": fwd, "up": up, "left": left, "ears": ears, "earL": earL, "earR": earR}


def signed_dist(head, pts):
    """Signed distance of each point to the skin (positive outside) and the nearest normal (outward)."""
    bvh, sgn = head["skin_bvh"], head["sgn"]
    d = np.empty(len(pts))
    n = np.zeros((len(pts), 3))
    for k, p in enumerate(pts):
        loc, nrm, _, _ = bvh.find_nearest(Vector(p))
        if loc is None:
            d[k] = 1.0
            continue
        nn = np.array(nrm) * sgn
        d[k] = (p - np.array(loc)) @ nn
        n[k] = nn
    return d, n


def ray_skin(head, origin, direction, dist=0.6):
    loc, _, _, _ = head["skin_bvh"].ray_cast(Vector(origin), Vector(direction), dist)
    return None if loc is None else np.array(loc)


# -- output -------------------------------------------------------------------------------------------------------------

def save_png(path, rgb, colour):
    h, w = rgb.shape[:2]
    im = bpy.data.images.new(os.path.basename(path), w, h, alpha=False, is_data=not colour)
    rgba = np.ones((h, w, 4), np.float32)
    rgba[..., :3] = np.clip(rgb, 0, 1)
    im.pixels.foreach_set(rgba.ravel())
    im.filepath_raw = path
    im.file_format = "PNG"
    im.save()
    if not colour:
        im.colorspace_settings.name = "Non-Color"
    return im


def load_png(path):
    im = bpy.data.images.load(path, check_existing=False)
    w, h = im.size
    px = np.empty(w * h * 4, np.float32)
    im.pixels.foreach_get(px)
    bpy.data.images.remove(im)
    return px.reshape(h, w, 4)[..., :3].astype(np.float64)


def preview_material(name, bc, n_gl, orm):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    t_bc, t_n, t_orm = (nt.nodes.new("ShaderNodeTexImage") for _ in range(3))
    t_bc.image, t_n.image, t_orm.image = bc, n_gl, orm
    sep, nmap = nt.nodes.new("ShaderNodeSeparateColor"), nt.nodes.new("ShaderNodeNormalMap")
    nt.links.new(t_bc.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(t_orm.outputs["Color"], sep.inputs["Color"])
    nt.links.new(sep.outputs["Green"], bsdf.inputs["Roughness"])
    nt.links.new(sep.outputs["Blue"], bsdf.inputs["Metallic"])
    nt.links.new(t_n.outputs["Color"], nmap.inputs["Color"])
    nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
    return mat


def skin_and_export(head, o, name, out, report):
    """100 % on the body's head bone, parented with an Armature modifier, exported with bind_r3's settings."""
    arm = head["arm"]
    o.vertex_groups.clear()
    vg = o.vertex_groups.new(name="head")
    vg.add(list(range(len(o.data.vertices))), 1.0, "REPLACE")
    mw = o.matrix_world.copy()
    o.parent = arm
    o.matrix_world = mw
    am = o.modifiers.new("Armature", "ARMATURE")
    am.object = arm
    o.name = o.data.name = name
    bpy.ops.object.select_all(action="DESELECT")
    o.select_set(True)
    arm.select_set(True)
    bpy.context.view_layer.objects.active = arm
    fbx = os.path.join(out, name + ".fbx")
    bpy.ops.export_scene.fbx(filepath=fbx, use_selection=True, object_types={"ARMATURE", "MESH"},
                             add_leaf_bones=False, bake_anim=False, use_mesh_modifiers=False,
                             mesh_smooth_type="FACE", use_armature_deform_only=False)
    tris = sum(len(p.vertices) - 2 for p in o.data.polygons)
    report.update({"fbx": fbx, "tris": tris, "verts": len(o.data.vertices), "bone": "head"})
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, name + "_fit.blend"))
    json.dump(report, open(os.path.join(out, name + "_log.json"), "w"), indent=1)
    log("WROTE %s: %d tris, %d verts, 100 %% head" % (fbx, tris, len(o.data.vertices)))


# -- glasses ------------------------------------------------------------------------------------------------------------

def import_glasses(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    for o in new:
        log("glasses FBX object %s (%s) at %s" % (o.name, o.type, np.round(np.array(o.matrix_world.translation), 4)))
    meshes = [o for o in new if o.type == "MESH" and not o.name.upper().startswith("UCX_")]
    if not meshes:
        sys.exit("head_prop: no mesh in the glasses FBX")
    origins = {o.name: np.array(o.matrix_world.translation) for o in meshes}
    for o in meshes:
        mw = o.matrix_world.copy()
        o.parent = None
        o.matrix_world = mw
    for o in new:
        if o not in meshes:
            bpy.data.objects.remove(o, do_unlink=True)
    # remember which object each vertex came from (temple objects, if the FBX kept them apart)
    names = [o.name for o in meshes]
    for k, o in enumerate(meshes):
        a = o.data.attributes.new("src", "INT", "POINT")
        a.data.foreach_set("value", np.full(len(o.data.vertices), k, np.int32))
    bpy.ops.object.select_all(action="DESELECT")
    for o in meshes:
        o.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    if len(meshes) > 1:
        bpy.ops.object.join()
    g = bpy.context.view_layer.objects.active
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    return g, names, origins


def run_glasses(head, gl_fbx, out, opt):
    name = opt.get("name", "SK_Glasses")
    g, src_names, origins = import_glasses(gl_fbx)
    me = g.data
    co = wverts(g)
    src = np.empty(len(co), np.int32)
    me.attributes["src"].data.foreach_get("value", src)
    mats = [m.name if m else "" for m in me.materials]
    mi = np.empty(len(me.polygons), dtype=np.int64)
    me.polygons.foreach_get("material_index", mi)
    lens_slots = [i for i, n in enumerate(mats) if "lens" in n.lower()]
    log("glasses materials %s, lens slots %s, %d verts, %d faces" % (mats, lens_slots, len(co), len(me.polygons)))
    if not lens_slots:
        sys.exit("head_prop: no lens material on the glasses (the frame axes come from the lenses)")
    lens_vids = np.unique(np.concatenate([np.array(me.polygons[k].vertices) for k in np.nonzero(np.isin(mi, lens_slots))[0]]))
    is_lens = np.zeros(len(co), bool)
    is_lens[lens_vids] = True
    # frame axes from the lenses
    L = co[is_lens]
    c = L.mean(0)
    _, vec = np.linalg.eigh(np.cov((L - c).T))
    ax_d, ax_h, ax_w = vec[:, 0], vec[:, 1], vec[:, 2]
    width = np.ptp((L - c) @ ax_w)
    k = 1.0
    for cand in (1.0, 0.01, 0.001):
        if 0.08 <= width * cand <= 0.25:
            k = cand
            break
    if k != 1.0:
        co = c + (co - c) * k
        L = co[is_lens]
        origins = {n: c + (p - c) * k for n, p in origins.items()}
        log("glasses rescaled by %g (lens span %.3f in file units)" % (k, width))
    rest = co[~is_lens]
    s = (rest - c) @ ax_d
    D = ax_d if s.max() > -s.min() else -ax_d  # towards the temples
    dd = (co - c) @ D
    # the temple tips hang below the temples' straight run: up points from the tips to the run
    tips = co[dd > 0.92 * dd.max()]
    run = co[(dd > 0.3 * dd.max()) & (dd < 0.6 * dd.max())]
    U = ax_h if ((run.mean(0) - tips.mean(0)) @ ax_h) > 0 else -ax_h
    W = np.cross(U, -D)  # the wearer's left
    loc_g = np.stack([(co - c) @ W, (co - c) @ U, (co - c) @ D], 1)  # w, h, d in the glasses frame (m)
    lw, lh, ld = loc_g[is_lens].T
    lens_back = ld.max()
    log("glasses: lens span %.1f x %.1f mm, depth %.1f..%.1f mm; total depth %.1f mm"
        % (np.ptp(lw) / MM, np.ptp(lh) / MM, ld.min() / MM, ld.max() / MM, loc_g[:, 2].max() / MM))
    # nose-rest point: the bridge's underside at the middle, on its back face
    front = (~is_lens) & (loc_g[:, 2] < lens_back + 12 * MM)
    mid = front & (np.abs(loc_g[:, 0]) < 3 * MM)
    if not mid.any():
        sys.exit("head_prop: no bridge at the middle of the glasses")
    hb = loc_g[mid, 1].min()
    low = mid & (loc_g[:, 1] < hb + 1.5 * MM)
    N_g = np.array([0.0, hb, loc_g[low, 2].max()])
    best = None
    for nm, p in origins.items():
        pg = np.array([(p - c) @ W, (p - c) @ U, (p - c) @ D])
        dist = np.linalg.norm(pg - N_g)
        log("origin of %s at %s mm in the glasses frame, %.1f mm from the bridge estimate" % (nm, np.round(pg / MM, 1), dist / MM))
        if dist < 4 * MM and (best is None or dist < best[0]):
            best = (dist, pg)
    if best is not None:
        N_g = best[1]
    # temples: named objects, else everything far enough behind the lenses
    t_start = fopt(opt, "temple_start", 9) * MM
    named = [i for i, n in enumerate(src_names) if "temple" in n.lower()]
    temples = {}
    for side, sg in (("L", 1.0), ("R", -1.0)):
        if named:
            m = np.isin(src, named) & (np.sign(loc_g[:, 0]) == sg)
        else:
            m = (~is_lens) & (loc_g[:, 2] > lens_back + t_start) & (np.sign(loc_g[:, 0]) == sg)
        temples[side] = m
        log("temple %s: %d verts (%s)" % (side, int(m.sum()), "named objects" if named else "depth rule"))
    is_temple = temples["L"] | temples["R"]

    # place the front
    fwd, up, left, E = head["fwd"], head["up"], head["left"], head["eye_mid"]
    lens_eye = fopt(opt, "lens_eye", -2) * MM
    h_lc = 0.0  # the lens centroid is the frame's origin
    nose_h = lens_eye + (N_g[1] - h_lc)
    hit = ray_skin(head, E + up * nose_h + fwd * 0.3, -fwd)
    if hit is None:
        sys.exit("head_prop: the midline ray at the nose-rest height missed the face")
    N_w = hit + fwd * fopt(opt, "pad_gap", 1) * MM

    def to_world(lg, origin_world_of_N):
        # glasses (w, h, d) -> world, with the nose-rest point N_g at origin_world_of_N; d runs backwards
        rel = lg - N_g
        return origin_world_of_N + rel[:, :1] * left + rel[:, 1:2] * up - rel[:, 2:3] * fwd
    P = to_world(loc_g, N_w)
    tilt = math.radians(fopt(opt, "tilt", 4))
    Rt = np.array(Matrix.Rotation(tilt, 3, Vector(left)))
    P = N_w + (P - N_w) @ Rt.T
    # push forward until the front (and a lens over the lashes) clears
    front_gap = fopt(opt, "front_gap", 1.5) * MM
    fm = ~is_temple
    d_front, _ = signed_dist(head, P[fm & ~is_lens])
    need = front_gap - d_front.min()
    lash_need = -1.0
    if head["lash_bvh"] is not None and is_lens.any():
        lens_pts = P[is_lens]
        lb = BVHTree.FromPolygons(P.tolist(), [tuple(me.polygons[k].vertices) for k in np.nonzero(np.isin(mi, lens_slots))[0]])
        gaps = []
        for v in head["F"][head["lash_vids"]]:
            h = lb.ray_cast(Vector(v), Vector(fwd), 0.05)
            if h[0] is not None:
                gaps.append(h[3])
        if gaps:
            lash_need = front_gap - min(gaps)
            log("lash tips to the lens back: min %.1f mm over %d lash verts under a lens" % (min(gaps) / MM, len(gaps)))
        del lens_pts
    push = max(need, lash_need, 0.0)
    cap = 12 * MM
    if push > cap:
        log("WARNING: the front wanted %.1f mm forward; capped at %.1f" % (push / MM, cap / MM))
        push = cap
    P += fwd * push
    N_w = N_w + fwd * push
    d_front, _ = signed_dist(head, P[fm & ~is_lens])
    log("front: nose-rest %.1f mm above the eye centres, pushed forward %.1f mm, nearest the skin %.1f mm, tilt %.1f deg"
        % (nose_h / MM, push / MM, d_front.min() / MM, math.degrees(tilt)))

    # temples: turn and stretch onto the ear root, then bow out to clear the head
    temple_gap = fopt(opt, "temple_gap", 1.5) * MM
    F = head["F"]
    rep_t = {}
    for side, sg in (("L", 1.0), ("R", -1.0)):
        m = temples[side]
        if not m.any():
            continue
        lg = loc_g[m]
        d0 = lg[:, 2].min()
        straight = (lg[:, 2] > d0 + 10 * MM) & (lg[:, 2] < d0 + 45 * MM)
        h_bot = np.percentile(lg[straight, 1], 5) if straight.any() else np.percentile(lg[:, 1], 5)
        half_t = (np.percentile(lg[straight, 0], 95) - np.percentile(lg[straight, 0], 5)) / 2 if straight.any() else 2 * MM
        dropped = lg[:, 1] < h_bot - 4 * MM
        d_b = lg[dropped & (lg[:, 2] > d0 + 20 * MM), 2].min() if (dropped & (lg[:, 2] > d0 + 20 * MM)).any() \
            else lg[:, 2].max() - 5 * MM
        near_b = np.abs(lg[:, 2] - d_b) < 3 * MM
        hinge = lg[:, 2] < d0 + 2 * MM
        Bg = np.array([np.median(lg[near_b, 0]), h_bot, d_b])
        Pg = np.array([np.median(lg[hinge, 0]), np.median(lg[hinge, 1]), d0])
        # the hinge and the bend through the front's placement: to_world at the pre-push nose-rest point, tilt, push
        N0 = N_w - fwd * push
        Bw = N0 + (to_world(Bg[None], N0)[0] - N0) @ Rt.T + fwd * push
        Pw = N0 + (to_world(Pg[None], N0)[0] - N0) @ Rt.T + fwd * push
        out_dir = left * sg
        ear = head["ears"][side]
        if ear:
            Ev = F[ear]
            hz = Ev @ up
            band = Ev[hz > hz.max() - 6 * MM]
            lat = np.abs((band - E) @ left)
            R = band[np.argmin(lat)]
            tsrc = "ear vertices"
        else:
            R = (head["earL"] if side == "L" else head["earR"]) + up * 15 * MM
            tsrc = "ear bone + 15 mm"
        T = R + up * 0.5 * MM + out_dir * (half_t + temple_gap)
        a, b = Bw - Pw, T - Pw
        q = Vector(a).rotation_difference(Vector(b))
        Rm = np.array(q.to_matrix())
        stretch = float(np.clip(np.linalg.norm(b) / max(np.linalg.norm(a), 1e-9), 0.85, 1.35))
        t = unit(b)
        V = Pw + (P[m] - Pw) @ Rm.T
        V = V + (stretch - 1.0) * ((V - Pw) @ t)[:, None] * t[None]
        # bow out: per 2 mm along the temple the largest outward push any vertex there needs, kept smooth
        s_al = (V - Pw) @ t
        bins = np.clip((s_al / (2 * MM)).astype(int), 0, None)
        nb = bins.max() + 1
        total = np.zeros(nb)
        for _ in range(3):
            d, n = signed_dist(head, V)
            lack = np.clip(temple_gap - d, 0, None) / np.clip(n @ out_dir, 0.3, 1.0)
            f = np.zeros(nb)
            np.maximum.at(f, bins, lack)
            if f.max() < 0.05 * MM:
                break
            f = np.array([f[max(0, i - 3):i + 4].max() for i in range(nb)])
            kern = np.exp(-0.5 * (np.arange(-4, 5) / 2.0) ** 2)
            f = np.convolve(np.pad(f, 4, mode="edge"), kern / kern.sum(), mode="valid")
            ramp = np.clip((np.arange(nb) * 2 * MM) / (12 * MM), 0, 1)
            f *= ramp * ramp * (3 - 2 * ramp)
            total += f
            V = V + f[bins][:, None] * out_dir[None]
        d, _ = signed_dist(head, V)
        P[m] = V
        bend_err = np.linalg.norm(Pw + t * np.linalg.norm(a) * stretch - T)  # nonzero only when the stretch clamps
        rep_t[side] = {"stretch": stretch, "turn_deg": math.degrees(q.angle), "bow_max_mm": float(total.max() / MM),
                       "nearest_head_mm": float(d.min() / MM), "bend_to_ear_mm": float(bend_err / MM),
                       "ear_target": tsrc}
        log("temple %s: turned %.1f deg, stretched x%.3f, bowed out up to %.1f mm, nearest the head %.1f mm, bend %.1f mm"
            " from the ear root (%s)" % (side, math.degrees(q.angle), stretch, total.max() / MM, d.min() / MM,
                                          bend_err / MM, tsrc))
    # write back, drop the lenses, one material, decimate
    me.vertices.foreach_set("co", P.ravel())
    g.matrix_world = Matrix.Identity(4)
    me.update()
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.material_index in lens_slots], context="FACES")
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context="VERTS")
    for f in bm.faces:
        f.material_index = 0
    bm.to_mesh(me)
    bm.free()
    if me.attributes.get("src"):
        me.attributes.remove(me.attributes["src"])
    tris = sum(len(p.vertices) - 2 for p in me.polygons)
    target = int(opt.get("tris", 11000))
    if tris > target:
        bpy.ops.object.select_all(action="DESELECT")
        g.select_set(True)
        bpy.context.view_layer.objects.active = g
        dec = g.modifiers.new("decimate", "DECIMATE")
        dec.ratio = target / tris * 0.98
        dec.use_collapse_triangulate = True
        bpy.ops.object.modifier_apply(modifier=dec.name)
        log("decimated %d -> %d tris" % (tris, sum(len(p.vertices) - 2 for p in me.polygons)))
    # textures: the source set, the normal kept DirectX for UE and flipped to OpenGL for the .blend
    tdir = opt.get("textures", os.path.dirname(gl_fbx))
    files = {k: next((os.path.join(tdir, f) for f in sorted(os.listdir(tdir)) if f.upper().endswith(k + ".PNG")), None)
             for k in ("_BC", "_N", "_ORM")}
    if None in files.values():
        sys.exit("head_prop: textures missing in %s: %s" % (tdir, files))
    bc, nx, orm = (load_png(files[k]) for k in ("_BC", "_N", "_ORM"))
    ngl = nx.copy()
    ngl[..., 1] = 1.0 - ngl[..., 1]
    im_bc = save_png(os.path.join(out, name + "_BC.png"), bc, True)
    save_png(os.path.join(out, name + "_N.png"), nx, False)
    im_ngl = save_png(os.path.join(out, name + "_N_gl.png"), ngl, False)
    im_orm = save_png(os.path.join(out, name + "_ORM.png"), orm, False)
    me.materials.clear()
    me.materials.append(preview_material("M_" + name, im_bc, im_ngl, im_orm))
    for p in me.polygons:
        p.use_smooth = True
    report = {"mode": "glasses", "source": gl_fbx, "nose_rest_above_eyes_mm": nose_h / MM, "front_push_mm": push / MM,
              "front_nearest_skin_mm": float(d_front.min() / MM), "tilt_deg": math.degrees(tilt), "temples": rep_t,
              "lenses": "removed: translucent lens pending a master", "normal": "DirectX in _N.png"}
    skin_and_export(head, g, name, out, report)


# -- mask ---------------------------------------------------------------------------------------------------------------

def value_noise(X, Y, cell, rng):
    fx, fy = X / cell + 1.0, Y / cell + 1.0
    G = rng.random((int(fy.max()) + 3, int(fx.max()) + 3))
    ix, iy = np.floor(fx).astype(int), np.floor(fy).astype(int)
    tx, ty = fx - ix, fy - iy
    tx, ty = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
    a, b = G[iy, ix], G[iy, ix + 1]
    c, d = G[iy + 1, ix], G[iy + 1, ix + 1]
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty


def fbm(X, Y, cell, rng, octaves=4):
    s, amp, tot = np.zeros_like(X), 1.0, 0.0
    for o in range(octaves):
        s += amp * value_noise(X, Y, cell / 2 ** o, rng)
        tot += amp
        amp *= 0.5
    return s / tot


def voronoi_edge(X, Y, cell, rng):
    """F2 - F1 of a jittered-grid Voronoi net in mm: about twice the distance to the nearest cell edge."""
    fx, fy = X / cell + 1.0, Y / cell + 1.0
    J = rng.random((int(fy.max()) + 4, int(fx.max()) + 4, 2))
    ix, iy = np.floor(fx).astype(int), np.floor(fy).astype(int)
    d1 = np.full(X.shape, 1e9)
    d2 = np.full(X.shape, 1e9)
    for oy in (-1, 0, 1):
        for ox in (-1, 0, 1):
            cx, cy = ix + ox, iy + oy
            px = cx + J[cy + 1, cx + 1, 0]
            py = cy + J[cy + 1, cx + 1, 1]
            d = np.hypot(fx - px, fy - py)
            d2 = np.where(d < d1, d1, np.minimum(d2, d))
            d1 = np.minimum(d1, d)
    return (d2 - d1) * cell


def porcelain_maps(res, side_mm, seed, crack, colour):
    """Cracked porcelain: a glazed off-white with slow warm mottling, a few long cracks and a patchy fine net, grime
    in the cracks. Returns BC (sRGB), normal (OpenGL, +Y up the rows) and ORM."""
    rng = np.random.default_rng(seed)
    x = (np.arange(res) + 0.5) / res * side_mm
    X, Y = np.meshgrid(x, x)
    wx = (fbm(X, Y, 14.0, rng) - 0.5) * 6.0
    wy = (fbm(X, Y, 14.0, rng) - 0.5) * 6.0
    major = voronoi_edge(X + wx, Y + wy, 26.0, rng)
    fx = (fbm(X, Y, 5.0, rng, 3) - 0.5) * 2.0
    fy = (fbm(X, Y, 5.0, rng, 3) - 0.5) * 2.0
    fine = voronoi_edge(X + fx, Y + fy, 7.0, rng)
    show_major = np.clip((fbm(X, Y, 45.0, rng, 2) - 0.38) / 0.12, 0, 1)
    show_fine = np.clip((fbm(X, Y, 30.0, rng, 3) - (0.72 - 0.4 * crack)) / 0.1, 0, 1)
    line_major = np.exp(-(major / (2 * 0.22)) ** 2) * show_major
    line_fine = np.exp(-(fine / (2 * 0.12)) ** 2) * show_fine * 0.8
    halo = np.exp(-(major / (2 * 1.2)) ** 2) * show_major * 0.25
    lines = np.clip(line_major + line_fine, 0, 1)
    mott = fbm(X, Y, 40.0, rng, 3) - 0.5
    base = np.array(colour, float)[None, None] * (1 + 0.04 * mott[..., None])
    base[..., 2] *= 1 - 0.03 * mott  # warmer in the mottles
    grime = np.array([0.30, 0.27, 0.24])
    bc = base * (1 - 0.18 * halo[..., None])
    bc = bc * (1 - lines[..., None] * 0.85) + grime[None, None] * lines[..., None] * 0.85
    height = -(line_major * 1.0 + line_fine * 0.5) + 0.02 * mott
    px_mm = res / side_mm
    gy, gx = np.gradient(height * px_mm * 0.05)  # depth units: about 50 um per unit at this scale
    n = np.dstack([-gx * 4.0, -gy * 4.0, np.ones_like(height)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    orm = np.dstack([1 - 0.5 * lines - 0.1 * halo, np.clip(0.18 + 0.42 * lines + 0.03 * mott, 0.05, 1),
                     np.zeros_like(lines)])
    return bc, n * 0.5 + 0.5, orm


def run_mask(head, out, opt):
    name = opt.get("name", "SK_Mask")
    fwd, up, left, E = head["fwd"], head["up"], head["left"], head["eye_mid"]
    hw = fopt(opt, "half_width", 68) * MM
    top = fopt(opt, "top", 75) * MM
    spacing = fopt(opt, "spacing", 3.5) * MM
    # the chin from the midline profile: the most forward point below the mouth, then down to where it turns under
    us = np.arange(-160, -60, 1.0) * MM
    prof = []
    for u in us:
        h = ray_skin(head, E + up * u + fwd * 0.3, -fwd)
        prof.append(np.nan if h is None else (h - E) @ fwd)
    prof = np.array(prof)
    ok = np.isfinite(prof)
    if ok.sum() < 10:
        sys.exit("head_prop: the midline profile under the eyes missed the face")
    chin_i = int(np.nanargmax(np.where(us < -90 * MM, prof, -np.inf)))
    under = chin_i
    while under > 0 and np.isfinite(prof[under - 1]) and prof[under - 1] > prof[chin_i] - 15 * MM:
        under -= 1
    bottom = us[under] - fopt(opt, "chin_under", 2) * MM
    u_mid = -10 * MM
    log("mask: chin forward point %.1f mm below the eyes, turns under at %.1f mm; mask spans %.1f..+%.1f mm"
        % (-us[chin_i] / MM, -us[under] / MM, bottom / MM, top / MM))

    def half_w(u):
        t = np.where(u >= u_mid, (u - u_mid) / (top - u_mid), (u_mid - u) / (u_mid - bottom))
        n = np.where(u >= u_mid, 2.3, 1.8)  # a full brow, a narrower chin
        return hw * np.clip(1 - np.clip(t, 0, 1) ** n, 0, 1) ** (1 / n)
    # outline: up the wearer's right side (-l), down the left (+l); the ends close at the top and bottom points
    uo = bottom + (top - bottom) * (0.5 - 0.5 * np.cos(np.linspace(0, math.pi, 400)))  # denser near the ends
    wo = half_w(uo)
    outline = np.vstack([np.stack([-wo, uo], 1), np.stack([wo[::-1], uo[::-1]], 1)[1:-1]])
    # resample the outline evenly
    seg = np.linalg.norm(np.diff(np.vstack([outline, outline[:1]]), axis=0), axis=1)
    cum = np.concatenate([[0], np.cumsum(seg)])
    nb = max(int(cum[-1] / spacing), 24)
    sl = np.linspace(0, cum[-1], nb, endpoint=False)
    ol = np.stack([np.interp(sl, cum, np.append(outline[:, 0], outline[0, 0])),
                   np.interp(sl, cum, np.append(outline[:, 1], outline[0, 1]))], 1)
    # interior: a triangular lattice kept half a spacing inside the outline
    pts = []
    row = 0
    for u in np.arange(bottom + spacing * 0.6, top - spacing * 0.5, spacing * math.sqrt(3) / 2):
        w = float(half_w(np.array(u))) - spacing * 0.6
        off = (spacing / 2) if row % 2 else 0.0
        row += 1
        if w <= 0:
            continue
        for l in np.arange(-w + off, w, spacing):
            pts.append((l, u))
    allp = [Vector((p[0], p[1])) for p in ol] + [Vector(p) for p in pts]
    edges = [(i, (i + 1) % nb) for i in range(nb)]
    # the convex-hull triangulation with the outline as constraint edges, then only the triangles inside the outline
    cdt = delaunay_2d_cdt(allp, edges, [], 0, 1e-7, True)
    V2 = np.array([[v.x, v.y] for v in cdt[0]])
    faces = [list(f) for f in cdt[2] if len(f) == 3]
    cen = np.array([V2[f].mean(0) for f in faces])
    inside = np.zeros(len(cen), bool)
    ox, oy = ol[:, 0], ol[:, 1]
    ox2, oy2 = np.roll(ox, -1), np.roll(oy, -1)
    for k, (x, y) in enumerate(cen):  # even-odd ray crossing
        cross = ((oy > y) != (oy2 > y)) & (x < (ox2 - ox) * (y - oy) / np.where(oy2 == oy, 1e-12, oy2 - oy) + ox)
        inside[k] = cross.sum() % 2 == 1
    faces = [f for f, keep in zip(faces, inside) if keep]
    used = np.unique(np.array(faces).ravel())
    remap = np.full(len(V2), -1)
    remap[used] = np.arange(len(used))
    V2 = V2[used]
    faces = [[int(remap[i]) for i in f] for f in faces]
    # orient every face counter-clockwise in (l, u): its normal is then forward
    for f in faces:
        a, b, c = V2[f[0]], V2[f[1]], V2[f[2]]
        if (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]) < 0:
            f.reverse()
    nV = len(V2)
    # skin depth along forward under each sheet point
    hd = np.full(nV, np.nan)
    for k, (l, u) in enumerate(V2):
        h = ray_skin(head, E + left * l + up * u + fwd * 0.3, -fwd)
        if h is not None:
            hd[k] = (h - E) @ fwd
    miss = ~np.isfinite(hd)
    if miss.all():
        sys.exit("head_prop: no ray hit the face inside the mask outline")
    if miss.any():  # fill from the nearest hit
        good = np.nonzero(~miss)[0]
        for k in np.nonzero(miss)[0]:
            hd[k] = hd[good[np.argmin(np.linalg.norm(V2[good] - V2[k], axis=1))]]
        log("mask: %d of %d sheet points missed the face; filled from their nearest neighbours" % (miss.sum(), nV))
    # 2D distance to the outline: the edge band is held near the face
    d_edge = np.array([np.min(np.linalg.norm(ol - p, axis=1)) for p in V2])
    edge_band = np.clip(1 - d_edge / (12 * MM), 0, 1)
    nbr_a = np.array([f[i] for f in faces for i in range(3)])
    nbr_b = np.array([f[(i + 1) % 3] for f in faces for i in range(3)])
    ea, eb = np.concatenate([nbr_a, nbr_b]), np.concatenate([nbr_b, nbr_a])
    deg = np.bincount(ea, minlength=nV).astype(float)
    standoff = fopt(opt, "standoff", 4.5) * MM
    min_off = fopt(opt, "min_standoff", 3.0) * MM
    max_off = fopt(opt, "max_off", 8) * MM
    h = hd + standoff
    # the lashes as a floor: the sheet stays min_off in front of the most forward lash within 3 mm of each point
    lash_floor = np.full(nV, -np.inf)
    if len(head["lash_vids"]):
        Lv = head["F"][head["lash_vids"]] - E
        lash2 = np.stack([Lv @ left, Lv @ up], 1)
        lash_d = Lv @ fwd
        for k in range(nV):
            near = np.sum((lash2 - V2[k]) ** 2, axis=1) < (3 * MM) ** 2
            if near.any():
                lash_floor[k] = lash_d[near].max() + min_off
        log("mask: %d sheet points lie over the lashes" % int(np.isfinite(lash_floor).sum()))

    def world(hh):
        return E[None] + V2[:, :1] * left[None] + V2[:, 1:2] * up[None] + hh[:, None] * fwd[None]

    def hold(hh):
        d, n = signed_dist(head, world(hh))
        hh = hh + np.clip(min_off - d, 0, None) / np.clip(n @ fwd, 0.3, 1.0)
        hh = np.maximum(hh, lash_floor)
        cap = hd + max_off  # the edge band is drawn back towards the face
        return np.where(edge_band > 0, np.minimum(hh, cap * edge_band + hh * (1 - edge_band)), hh)
    n_smooth = int(opt.get("smooth", 60))
    for it in range(n_smooth):
        s = np.zeros(nV)
        np.add.at(s, ea, h[eb])
        h = 0.5 * h + 0.5 * s / np.maximum(deg, 1)
        h = hold(h)
    # the lower bound last, so the edge cap never pulls the sheet into the skin
    d, n = signed_dist(head, world(h))
    h = np.maximum(h + np.clip(min_off - d, 0, None) / np.clip(n @ fwd, 0.3, 1.0), lash_floor)
    P = world(h)
    d, _ = signed_dist(head, P)
    log("mask sheet: %d verts, %d tris; off the skin min %.2f p5 %.2f p50 %.2f p95 %.2f mm"
        % (nV, len(faces), d.min() / MM, np.percentile(d, 5) / MM, np.median(d) / MM, np.percentile(d, 95) / MM))
    me = bpy.data.meshes.new(name)
    me.from_pydata(P.tolist(), [], faces)
    me.validate()
    o = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(o)
    # planar UVs from the front over one square
    lo2, hi2 = V2.min(0), V2.max(0)
    side = float((hi2 - lo2).max()) * 1.04
    c2 = (lo2 + hi2) / 2
    uv = (V2 - c2) / side + 0.5
    uvl = me.uv_layers.new(name="UVMap")
    lv = np.empty(len(me.loops), dtype=np.int64)
    me.loops.foreach_get("vertex_index", lv)
    uvl.data.foreach_set("uv", uv[lv].ravel())
    for p in me.polygons:
        p.use_smooth = True
    bpy.ops.object.select_all(action="DESELECT")
    o.select_set(True)
    bpy.context.view_layer.objects.active = o
    sol = o.modifiers.new("shell", "SOLIDIFY")
    sol.thickness = fopt(opt, "shell", 3.0) * MM
    sol.offset = 1.0
    sol.use_rim = True
    sol.use_even_offset = False
    sol.use_quality_normals = True
    bpy.ops.object.modifier_apply(modifier=sol.name)
    # the rim flat-shaded so the edge reads as a cut shell
    bm = bmesh.new()
    bm.from_mesh(me)
    bm.normal_update()
    flat = 0
    for f in bm.faces:
        if abs(Vector(fwd).dot(f.normal)) < 0.35 and f.calc_area() < (spacing * 0.8) * fopt(opt, "shell", 3.0) * MM * 2:
            f.smooth = False
            flat += 1
    bm.to_mesh(me)
    bm.free()
    # porcelain set
    res = int(opt.get("tex", 2048))
    col = [float(x) for x in opt.get("colour", "0.86,0.84,0.80").split(",")]
    bc, n_gl, orm = porcelain_maps(res, side / MM, int(opt.get("seed", 3)), fopt(opt, "crack", 0.55), col)
    n_dx = n_gl.copy()
    n_dx[..., 1] = 1.0 - n_dx[..., 1]
    im_bc = save_png(os.path.join(out, name + "_BC.png"), bc, True)
    save_png(os.path.join(out, name + "_N.png"), n_dx, False)
    im_ngl = save_png(os.path.join(out, name + "_N_gl.png"), n_gl, False)
    im_orm = save_png(os.path.join(out, name + "_ORM.png"), orm, False)
    me.materials.append(preview_material("M_" + name, im_bc, im_ngl, im_orm))
    report = {"mode": "mask", "span_mm": [bottom / MM, top / MM], "half_width_mm": hw / MM,
              "sheet_off_skin_mm": {"min": float(d.min() / MM), "p5": float(np.percentile(d, 5) / MM),
                                    "p50": float(np.median(d) / MM), "p95": float(np.percentile(d, 95) / MM)},
              "shell_mm": fopt(opt, "shell", 3.0), "smooth": n_smooth, "rim_faces_flat": flat,
              "face_hide": "not needed" if d.min() >= 2.5 * MM else "CLIPS: hide the covered face region",
              "normal": "DirectX in _N.png", "texture_side_mm": side / MM}
    skin_and_export(head, o, name, out, report)


def main():
    mode, paths, opt = parse_args()
    if mode == "glasses":
        body, face, gl, out = paths
    else:
        body, face, out = paths
    os.makedirs(out, exist_ok=True)
    head = load_head(body, face)
    if mode == "glasses":
        run_glasses(head, gl, out, opt)
    else:
        run_mask(head, out, opt)


main()
