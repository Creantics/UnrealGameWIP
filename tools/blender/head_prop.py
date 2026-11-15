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
lashes, is nearer than "front_gap". Each temple then turns about its hinge and stretches so its bend lands where the
temple, lowered onto the ear from above at the scalp, first rests on it; the tip turns down "tip_angle" degrees from
the run, and the last 25 mm of the run and the tip move sideways to lie "temple_gap" off the scalp (in or out; the run before
stays straight), then clear of the ear. Nose pads (objects named Pad_*, their wire arms PadArm_*) take no part in the
front's push: once the front is placed, each pad is turned (at most "pad_turn_max" deg, 25) to face the nose where a
ray along its contact normal meets it and moved along that normal until it lies "pad_seat" mm (0.3) off the skin; its
arm follows it rigidly at the pad end, stays put at the rim end and eases between. A pad that finds no nose, or moves
more than "pad_travel_max" mm (6), stops the fit. The lens polygons and any UCX_ hulls are removed (the lenses wait for a translucent master); the rest is
decimated to at most "tris" triangles in one material. Options: tilt (deg, 4), lens_eye (mm, -2), pad_gap (1),
front_gap (1.5), temple_gap (1.5), ear_lift (1.2), tip_angle (deg, 50), temple_start (mm behind the lenses' back where the temples
start, 9), tris (11000), textures (folder holding *_BC.png, *_N.png (DirectX) and *_ORM.png; default the glasses FBX's
folder), tint (sRGB "r,g,b": the non-metal frame parts take this colour, keeping "tint_var" (0.3) of the source's light
and dark; the metal hinges keep theirs).

mask: a shell wrapped round the face from the forehead to the chin, laid out round a vertical axis through the ears'
middle: an egg outline "wrap" degrees each side at its widest ("widest" mm above the eyes; kept 10 degrees in front of
the ears), its top "top" mm above the eyes, its bottom where the chin has turned "chin_back" mm under. The skin's
distance out along each ray from the axis is smoothed (a Gaussian of "relief" mm: the nose, lips and eye hollows
soften), set "standoff" mm out and held at least "min_standoff" mm off the skin and the lashes, then solidified "shell"
mm outward with its inner rim turned up to "lip" mm towards the face (never nearer than "min_standoff"). UVs from the (arc, height) layout and a procedural
cracked-porcelain set: a clean glaze, "cracks" long cracks run in from the rim (jagged, with short branches), a patchy
fine net mostly near them and a faint mouth line ("mouth", 0..1). Options: wrap (68), widest (5), top (72),
chin_back (6), relief (8), standoff (3.5), min_standoff (3.0), shell (3.0), lip (1.0), spacing (3.5 mm), tex (2048),
seed (3), cracks (4), crack (0..1, how much of the fine net shows away from the cracks, 0.25), crack_w and crack_len
(multipliers on the long cracks' width and length, 1), mouth (0.5), envelope (1: a blank shell, each row and column
of the field bridged to its least bulging curve over skin + min_standoff, smoothed "env_smooth" mm (7) over
"env_iter" passes (25), its last "taper" mm (14) coming down to the skin + "rim_off" mm (2.5); 0: the skin smoothed by
"relief" and set "standoff" out), bevel (0.9 mm, the shell's edge rounded; 0: flat rim faces),
colour (sRGB "r,g,b", 0.86,0.84,0.80).

cap: a hair cap under a groom, so the scalp between strands reads as hair: the skin faces near the groom's roots or
inside the sheet's hairline, set "offset" mm (0.5) out along the skin normals, laid out round the vertical through
the ears' middle (no seam, the crown at the centre). ROOTS.npz is a groom_strands.py "dump" of the groom on this face.
Its alpha (NAME_A.png, linear) is the larger of the roots' cover (splatted, blurred "spread" mm (3), full at 2/3 of
the dense part's level) and the region above "hairline" (the sheet's front-view hairline, symmetric "|x|:z" pairs
in IPDs from the eyes' midpoint, x across and z up, feathered over "feather" mm (4), in front of the ears only); the
ears stay bare. NAME_BC.png is "colour" (sRGB, 0.05,0.04,0.035) mottled by "mottle" (0.15); "rough" (0.75) goes in the
ORM; "tex" (1024) px. NAME_review.png shows the cap's alpha over the skin from the front and the side, the hairline
in red.

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
from mathutils.kdtree import KDTree

MM = 0.001


def log(*a):
    print("head_prop:", *a, flush=True)


def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not argv or argv[0] not in ("glasses", "mask", "cap", "measure", "transplant"):
        sys.exit("head_prop: glasses BODY.fbx FACE.fbx GLASSES.fbx OUT_DIR | mask BODY.fbx FACE.fbx OUT_DIR | "
                 "cap BODY.fbx FACE.fbx ROOTS.npz OUT_DIR [k=v ...] | measure BODY.fbx FACE.fbx OUT_DIR roots=A.npz;B.npz [k=v ...] | "
                 "transplant BODY.fbx FACE.fbx OUT_DIR roots=A.npz [k=v ...]")
    mode = argv[0]
    pos = [a for a in argv[1:] if "=" not in a]
    opt = dict(a.split("=", 1) for a in argv[1:] if "=" in a)
    need = 4 if mode in ("glasses", "cap") else 3
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
    polys = [tuple(p.vertices) for p in me.polygons]

    def bone_head(name):
        if farm is not None and name in farm.data.bones:
            return np.array(farm.matrix_world @ farm.data.bones[name].head_local)
        return None

    def slot_centroid(key):  # by slot name; only when the face has no eye bones
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
    roles = face_roles(face0, eyeL, eyeR)
    counts = np.bincount(mi, minlength=len(mats))
    log("face %s: %d verts; sections (slot name: role, faces) %s" % (face0.name, len(F), ", ".join(
        "%s: %s %d" % (mats[i] if i < len(mats) else i, roles.get(i, "-"), counts[i]) for i in range(len(counts)))))
    skin_slot = next(s for s, r in roles.items() if r == "skin")
    skin_polys = [polys[k] for k in np.nonzero(mi == skin_slot)[0]]
    skin_bvh = BVHTree.FromPolygons(F.tolist(), skin_polys)
    lash_slots = [s for s, r in roles.items() if r == "lash"]
    lash_polys = [polys[k] for k in np.nonzero(np.isin(mi, lash_slots))[0]] if lash_slots else []
    lash_bvh = BVHTree.FromPolygons(F.tolist(), lash_polys) if lash_polys else None
    lash_vids = np.unique(np.concatenate([np.array(p) for p in lash_polys])) if lash_polys else np.zeros(0, int)
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


def face_roles(obj, eyeL, eyeR):
    """Tells a MetaHuman face's sections apart by shape: the FBX's slot names can be off by one (a Real3 face lists the
    teeth's name on the skin). The biggest section is the skin; a small one centred on an eye bone is an eye; one
    across both eyes at eye height is the lashes when it has more vertices than faces (loose cards), else a
    see-through cover (eye shell, tear film, a hide strip); one below the eyes is the mouth (teeth, tongue)."""
    me = obj.data
    mw = obj.matrix_world
    P = np.array([tuple(mw @ v.co) for v in me.vertices])
    mi = np.empty(len(me.polygons), dtype=np.int64)
    me.polygons.foreach_get("material_index", mi)
    n = max(len(me.materials), int(mi.max()) + 1 if len(mi) else 0)
    counts = np.bincount(mi, minlength=n)
    skin = int(counts.argmax())
    roles = {skin: "skin"}
    ez = (eyeL[2] + eyeR[2]) / 2
    x0, x1 = sorted((eyeL[0], eyeR[0]))
    for s in range(n):
        if s == skin or counts[s] == 0:
            continue
        vids = np.unique(np.concatenate([np.array(me.polygons[k].vertices) for k in np.nonzero(mi == s)[0]]))
        lo, hi = P[vids].min(0), P[vids].max(0)
        c = (lo + hi) / 2
        if (hi - lo).max() < 0.04 and min(np.linalg.norm(c - eyeL), np.linalg.norm(c - eyeR)) < 0.015:
            roles[s] = "eye"
        elif lo[0] < x0 and hi[0] > x1 and abs(c[2] - ez) < 0.02:
            roles[s] = "lash" if len(vids) > counts[s] else "cover"
        elif c[2] < ez - 0.03:
            roles[s] = "mouth"
        else:
            roles[s] = "cover"
    return roles


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


def seat_pads(head, P, loc_g, is_pad, is_arm, opt):
    """Seats each nose pad (in place in P, world): turned at most pad_turn_max towards the nose's normal where a ray
    along the pad's contact normal meets it, then moved along that normal to lie pad_seat off the skin. The pad arm
    blends from still at its rim end to rigid with the pad at its pad end. Exits when a pad finds no nose or moves more
    than pad_travel_max."""
    left, E, up, fwd = head["left"], head["eye_mid"], head["up"], head["fwd"]
    seat = fopt(opt, "pad_seat", 0.3) * MM
    turn_max = math.radians(fopt(opt, "pad_turn_max", 25))
    travel_max = fopt(opt, "pad_travel_max", 6) * MM
    rep = {}
    for side, sg in (("L", 1.0), ("R", -1.0)):
        pm = is_pad & (np.sign(loc_g[:, 0]) == sg)
        if not pm.any():
            continue
        am = is_arm & (np.sign(loc_g[:, 0]) == sg)
        V = P[pm].copy()
        c0 = V.mean(0)
        _, ev = np.linalg.eigh(np.cov((V - c0).T))
        n_p = ev[:, 0] if ev[:, 0] @ (-sg * left) > 0 else -ev[:, 0]  # the contact face's normal, in onto the nose
        loc, nrm, _, _ = head["skin_bvh"].ray_cast(Vector(c0 - n_p * 15 * MM), Vector(n_p), 0.04)
        if loc is None:
            sys.exit("head_prop: pad %s: no nose along its contact normal (a ray from 15 mm out missed)" % side)
        n_s = unit(np.array(nrm) * head["sgn"])
        want = -n_s
        ang = math.acos(float(np.clip(n_p @ want, -1.0, 1.0)))
        turn = min(ang, turn_max)
        R = np.array(Matrix.Rotation(turn, 3, Vector(unit(np.cross(n_p, want))))) if ang > 1e-6 else np.eye(3)
        n2 = R @ n_p
        shift = np.zeros(3)
        for _ in range(10):
            d, _ = signed_dist(head, c0 + (V - c0) @ R.T + shift)
            step = d.min() - seat
            if abs(step) < 0.02 * MM:
                break
            shift = shift + n2 * step

        def xf(X):
            return c0 + (X - c0) @ R.T + shift
        Vn = xf(V)
        travel = float(np.linalg.norm(Vn - V, axis=1).max())
        d_pad, _ = signed_dist(head, Vn)
        P[pm] = Vn
        arm_min = float("nan")
        if am.any():
            A = P[am].copy()
            dc = np.linalg.norm(A - c0, axis=1)
            far = A[dc >= np.percentile(dc, 90)].mean(0)
            near = A[dc <= np.percentile(dc, 10)].mean(0)
            ax = near - far
            s = np.clip(((A - far) @ ax) / max(ax @ ax, 1e-12), 0.0, 1.0)
            w = s * s * (3 - 2 * s)
            P[am] = A + w[:, None] * (xf(A) - A)
            arm_min = float(signed_dist(head, P[am])[0].min())
        cp = Vn.mean(0) - E
        rep[side] = {"gap_mm": float(d_pad.min() / MM), "travel_mm": travel / MM, "turn_deg": math.degrees(turn),
                     "skin_angle_deg": math.degrees(ang), "along_normal_mm": float(shift @ n2 / MM),
                     "centre_mm": [float(abs(cp @ left) / MM), float(cp @ up / MM), float(cp @ fwd / MM)],
                     "arm_nearest_skin_mm": arm_min / MM,
                     "nose_normal_out_up_fwd": [float(n_s @ (sg * left)), float(n_s @ up), float(n_s @ fwd)]}
        log("pad %s: seated %.2f mm off the nose at %.1f mm out, %.1f up, %.1f forward of the eye centres; moved %.1f mm"
            " (%.1f along its normal), turned %.1f of %.1f deg; its arm %.1f mm off the skin at the nearest"
            % (side, d_pad.min() / MM, abs(cp @ left) / MM, cp @ up / MM, cp @ fwd / MM, travel / MM,
               shift @ n2 / MM, math.degrees(turn), math.degrees(ang), arm_min / MM))
        log("pad %s: the nose's normal there (out, up, forward) %s" % (side, np.round(rep[side]["nose_normal_out_up_fwd"], 3)))
        if travel > travel_max:
            sys.exit("head_prop: pad %s moved %.1f mm to reach the nose (at most %.1f): move it in the source"
                     % (side, travel / MM, travel_max / MM))
        if arm_min < 0.2 * MM:
            log("WARNING: pad arm %s is %.1f mm off the skin" % (side, arm_min / MM))
    return rep


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
    # nose pads and their wire arms (named objects): left out of the front's push, then seated on the nose
    lname = [n.lower().replace("_", "") for n in src_names]
    arm_ids = [i for i, n in enumerate(lname) if n.startswith("padarm")]
    pad_ids = [i for i, n in enumerate(lname) if n.startswith("pad") and i not in arm_ids]
    is_arm, is_pad = np.isin(src, arm_ids), np.isin(src, pad_ids)
    if pad_ids:
        log("nose pads: %d verts, pad arms %d verts (named objects)" % (int(is_pad.sum()), int(is_arm.sum())))

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
    fm = ~is_temple & ~is_pad & ~is_arm
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
    log("front: the rims and bridge want %.1f mm forward, the lashes %.1f mm" % (need / MM, lash_need / MM))
    cap = 12 * MM
    if push > cap:
        log("WARNING: the front wanted %.1f mm forward; capped at %.1f" % (push / MM, cap / MM))
        push = cap
    P += fwd * push
    N_w = N_w + fwd * push
    # a slanted contact (the lower rim beside the nose) gains less than the push: push again, at most twice more
    for _ in range(2):
        d_front, _ = signed_dist(head, P[fm & ~is_lens])
        extra = min(front_gap - d_front.min(), cap - push)
        if extra < 0.1 * MM:
            break
        P += fwd * extra
        N_w = N_w + fwd * extra
        push += extra
    d_bridge = float(signed_dist(head, N_w[None])[0][0])
    log("front: the nose-rest point is %.1f mm off the nose" % (d_bridge / MM))
    d_front, _ = signed_dist(head, P[fm & ~is_lens])
    jn = (P[fm & ~is_lens] - E)[int(np.argmin(d_front))]
    log("front: nose-rest %.1f mm above the eye centres, pushed forward %.1f mm, nearest the skin %.1f mm (%.1f mm out,"
        " %.1f up, %.1f forward of the eye centres), tilt %.1f deg"
        % (nose_h / MM, push / MM, d_front.min() / MM, abs(jn @ left) / MM, (jn @ up) / MM, (jn @ fwd) / MM,
           math.degrees(tilt)))
    rep_p = seat_pads(head, P, loc_g, is_pad, is_arm, opt) if pad_ids else {}
    if rep_p:
        log("front: the pads carry it (L %.1f mm, R %.1f mm off the nose); the rims and bridge are %.1f mm off the skin"
            % (rep_p.get("L", {}).get("gap_mm", float("nan")), rep_p.get("R", {}).get("gap_mm", float("nan")),
               d_front.min() / MM))

    # temples: each turns about its hinge and stretches so its bend lands where the temple, lowered from above at the
    # scalp, first rests on the ear; the tip turns down to "tip_angle" below the run; then the run's last 25 mm and the
    # tip move sideways to lie "temple_gap" off the scalp (in or out), then clear of the ear
    temple_gap = fopt(opt, "temple_gap", 1.5) * MM
    ear_lift = fopt(opt, "ear_lift", 1.2) * MM  # the run's underside above the top of the ear (the bend's curve dips)
    tip_angle = math.radians(fopt(opt, "tip_angle", 50))
    F = head["F"]
    rep_t = {}
    kern = np.exp(-0.5 * (np.arange(-4, 5) / 2.0) ** 2)
    kern /= kern.sum()

    def scalp_lat(f, u, out_dir):  # the side of the head, found from inside so the ear never answers
        hh = ray_skin(head, E + fwd * f + up * u, out_dir, 0.25)
        return None if hh is None else float((hh - E) @ out_dir)

    for side, sg in (("L", 1.0), ("R", -1.0)):
        m = temples[side]
        if not m.any():
            continue
        lg = loc_g[m]
        dsrc = lg[:, 2]
        d0 = dsrc.min()
        straight = (dsrc > d0 + 10 * MM) & (dsrc < d0 + 45 * MM)
        if straight.sum() < 10:
            straight = dsrc < d0 + 0.4 * np.ptp(dsrc)
        half_t = (np.percentile(lg[straight, 0], 95) - np.percentile(lg[straight, 0], 5)) / 2
        # the run's underside as a line; the bend starts where the underside has left it by 1 mm
        bins_d = np.arange(d0 + 10 * MM, dsrc.max(), 2 * MM)
        hb = np.full(len(bins_d), np.nan)
        for i, b0 in enumerate(bins_d):
            sel = (dsrc >= b0) & (dsrc < b0 + 2 * MM)
            if sel.sum() > 2:
                hb[i] = np.percentile(lg[sel, 1], 5)
        fit = (bins_d + MM < d0 + 45 * MM) & np.isfinite(hb)
        c1, c0 = np.polyfit(bins_d[fit] + MM, hb[fit], 1)
        drop = (c0 + c1 * (bins_d + MM)) - hb
        past = np.nonzero((bins_d > d0 + 20 * MM) & (drop > 1.0 * MM))[0]
        d_b = float(bins_d[past[0]] + MM) if len(past) else float(dsrc.max() - 25 * MM)
        near_b = np.abs(dsrc - d_b) < 2 * MM
        hinge = dsrc < d0 + 2 * MM
        Bg = np.array([np.median(lg[near_b, 0]), c0 + c1 * d_b, d_b])
        Pg = np.array([np.median(lg[hinge, 0]), np.median(lg[hinge, 1]), d0])
        # the hinge and the bend through the front's placement: to_world at the pre-push nose-rest point, tilt, push
        N0 = N_w - fwd * push
        Bw = N0 + (to_world(Bg[None], N0)[0] - N0) @ Rt.T + fwd * push
        Pw = N0 + (to_world(Pg[None], N0)[0] - N0) @ Rt.T + fwd * push
        out_dir = left * sg
        ear = head["ears"][side]
        T, tsrc = None, "ear bone + 15 mm"
        if ear:
            Ev = F[ear]
            kd = KDTree(len(Ev))
            for i, v in enumerate(Ev):
                kd.insert(Vector(v), i)
            kd.balance()
            ef, eu = (Ev - E) @ fwd, (Ev - E) @ up
            best = None
            for f in np.arange(ef.min(), ef.max(), 1 * MM):
                sl = scalp_lat(f, eu.max() + 4 * MM, out_dir)
                if sl is None:
                    continue
                for off in (temple_gap, temple_gap + half_t, temple_gap + 2 * half_t):
                    hh = ray_skin(head, E + fwd * f + up * (eu.max() + 15 * MM) + out_dir * (sl + off), -up, 0.08)
                    if hh is None or kd.find(Vector(hh))[2] > 3 * MM:
                        continue
                    hu = float((hh - E) @ up)
                    sl_h = scalp_lat(f, hu, out_dir)
                    if sl_h is None or (hh - E) @ out_dir < sl_h + 0.5 * MM:  # the scalp, not the ear
                        continue
                    if best is None or hu > best[0]:
                        best = (hu, f, sl)
            if best is not None:
                hu, f, sl = best
                T = E + fwd * (f - 2 * MM) + up * (hu + ear_lift) + out_dir * (sl + temple_gap + half_t)
                tsrc = "rests on the ear %.1f mm above the eyes, %.1f mm behind them" % (hu / MM, -f / MM)
        if T is None:
            T = (head["earL"] if side == "L" else head["earR"]) + up * 15.5 * MM + out_dir * (half_t + temple_gap)
        a, b = Bw - Pw, T - Pw
        q = Vector(a).rotation_difference(Vector(b))
        Rm = np.array(q.to_matrix())
        stretch = float(np.clip(np.linalg.norm(b) / max(np.linalg.norm(a), 1e-9), 0.85, 1.35))
        t = unit(b)
        V = Pw + (P[m] - Pw) @ Rm.T
        V = V + (stretch - 1.0) * ((V - Pw) @ t)[:, None] * t[None]
        Tb = Pw + t * np.linalg.norm(a) * stretch
        # the tip turns down about the bend, eased in over it
        upp = unit(up - t * (up @ t))
        k = unit(np.cross(t, -upp))
        tipm = dsrc > dsrc.max() - 10 * MM
        v = V[tipm].mean(0) - Tb
        cur = math.atan2(-(v @ upp), v @ t)
        delta = tip_angle - cur
        w = np.clip((dsrc - (d_b - 3 * MM)) / (9 * MM), 0, 1)
        th = delta * w * w * (3 - 2 * w)
        X = V - Tb
        V = Tb + X * np.cos(th)[:, None] + np.cross(k[None], X) * np.sin(th)[:, None] \
            + np.outer(X @ k, k) * (1 - np.cos(th))[:, None]
        # every 2 mm: the innermost vertex "temple_gap" off the scalp (from the hinge's 12 mm on), kept smooth
        bins = np.clip(((dsrc - d0) / (2 * MM)).astype(int), 0, None)
        nb = bins.max() + 1
        ramp = np.clip((np.arange(nb) * 2 * MM) / (12 * MM), 0, 1)  # the hinge never moves
        ramp = ramp * ramp * (3 - 2 * ramp)
        # the run stays straight (rigid acetate doesn't follow the temple's hollow): only the last 25 mm before the bend
        # and the tip come in to the scalp
        hug = np.clip((d0 + (np.arange(nb) + 0.5) * 2 * MM - (d_b - 25 * MM)) / (15 * MM), 0, 1)
        hug = hug * hug * (3 - 2 * hug) * ramp
        moved = np.zeros(nb)
        for it in range(4):
            lat = (V - E) @ out_dir
            want = np.full(nb, np.nan)
            for i in range(nb):
                sel = np.nonzero(bins == i)[0]
                if not len(sel):
                    continue
                j = sel[np.argmin(lat[sel])]
                sl = scalp_lat(float((V[j] - E) @ fwd), float((V[j] - E) @ up), out_dir)
                if sl is not None:
                    want[i] = sl + temple_gap - lat[j]
            good = np.isfinite(want)
            if not good.any():
                break
            want = np.interp(np.arange(nb), np.nonzero(good)[0], want[good])
            want = np.convolve(np.pad(want, 4, mode="edge"), kern, mode="valid") * hug
            V = V + want[bins][:, None] * out_dir[None]
            moved += want
            if np.abs(want).max() < 0.1 * MM:
                break
        # then clear of the ear and the head: each 2 mm moves sideways by the largest push any vertex there needs
        for it in range(4):
            d, n = signed_dist(head, V)
            lack = np.clip(0.8 * MM - d, 0, None)
            if lack.max() < 0.05 * MM:
                break
            sn = n @ out_dir
            push_v = np.where(sn >= 0, 1.0, -1.0) * lack / np.clip(np.abs(sn), 0.3, 1.0)
            fb = np.zeros(nb)
            for i in range(nb):
                sel = np.nonzero(bins == i)[0]
                if len(sel):
                    fb[i] = push_v[sel][np.argmax(np.abs(push_v[sel]))]
            fb = np.array([fb[max(0, i - 3):i + 4][np.argmax(np.abs(fb[max(0, i - 3):i + 4]))] for i in range(nb)])
            fb = np.convolve(np.pad(fb, 4, mode="edge"), kern, mode="valid") * ramp
            V = V + fb[bins][:, None] * out_dir[None]
            moved += fb
        d, _ = signed_dist(head, V)
        jn = int(np.argmin(d))
        log("temple %s: nearest the head %.1f mm at %.1f mm along it (%.1f mm out, %.1f up, %.1f forward of the eye centres)"
            % (side, d[jn] / MM, (dsrc[jn] - d0) / MM, abs((V[jn] - E) @ left) / MM, ((V[jn] - E) @ up) / MM,
               ((V[jn] - E) @ fwd) / MM))
        P[m] = V
        tip_lat = float(((V[tipm] - E) @ out_dir).max())
        bend_err = np.linalg.norm(Tb - T)  # nonzero only when the stretch clamps
        rep_t[side] = {"stretch": stretch, "turn_deg": math.degrees(q.angle), "tip_turn_deg": math.degrees(delta),
                       "sideways_mm": [float(moved.min() / MM), float(moved.max() / MM)],
                       "nearest_head_mm": float(d.min() / MM), "bend_to_ear_mm": float(bend_err / MM),
                       "tip_out_from_eyes_mm": tip_lat / MM, "ear_target": tsrc}
        log("temple %s: turned %.1f deg, stretched x%.3f, tip turned %.1f deg, moved sideways %.1f..%.1f mm, nearest the"
            " head %.1f mm, bend %.1f mm from the target, tip %.1f mm out (%s)"
            % (side, math.degrees(q.angle), stretch, math.degrees(delta), moved.min() / MM, moved.max() / MM,
               d.min() / MM, bend_err / MM, tip_lat / MM, tsrc))
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
    if opt.get("tint"):
        tint = np.array([float(x) for x in opt["tint"].split(",")])
        frame = orm[..., 2] < 0.5
        lum = bc[..., :3].mean(2)
        used = frame & (lum > 0.05)
        ref = float(np.median(lum[used])) if used.any() else 1.0
        shade = 1 + fopt(opt, "tint_var", 0.3) * (np.clip(lum / max(ref, 1e-3), 0.6, 1.4) - 1)
        bc = bc.copy()
        bc[..., :3] = np.where(frame[..., None], tint[None, None] * shade[..., None], bc[..., :3])
        log("glasses: non-metal frame tinted %s (source median %.3f), %.0f %% of the map" % (tint, ref, 100 * frame.mean()))
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
              "front_push_wanted_mm": {"rims_bridge": float(need / MM), "lashes": float(lash_need / MM)}, "pads": rep_p,
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


def crack_paths(res, side_mm, rim_mm, n, rng, wmul=1.0, lmul=1.0):
    """n long cracks run in from the rim (points in texture mm, x along the columns, y along the rows): straight runs
    of 2.5-6 mm with sharp turns (about 20 deg) that keep pulling back to the crack's heading, 45-95 mm long, 0.45-0.6 mm
    wide and tapering, with short side branches. Returns the crack core, a 2 mm halo and a 6 mm 'near' field (each 0..1)
    and the segment count."""
    px = res / side_mm
    core, halo, near = np.zeros((res, res)), np.zeros((res, res)), np.zeros((res, res))
    cen = rim_mm.mean(0)
    segs = []

    def walk(p, heading, length, w0, depth):
        done, ang, i = 0.0, heading, 0
        while done < length:
            step = rng.uniform(2.5, 6.0)
            ang = 0.7 * (ang + rng.normal(0, 0.35)) + 0.3 * heading
            q = p + step * np.array([math.cos(ang), math.sin(ang)])
            w = w0 * (1 - 0.6 * done / length)
            segs.append((p, q, w))
            if depth < 2 and i > 1 and rng.random() < 0.18:
                walk(q, ang + rng.choice([-1, 1]) * rng.uniform(0.4, 0.9), rng.uniform(8, 25), w * 0.6, depth + 1)
            p, done, i = q, done + step, i + 1
    off = rng.random()
    for i in range(n):
        k = int(((off + (i + rng.uniform(-0.25, 0.25)) / max(n, 1)) % 1.0) * len(rim_mm))
        p = rim_mm[k].astype(float)
        to_c = cen - p
        ang = math.atan2(to_c[1], to_c[0]) + rng.uniform(-0.5, 0.5)
        walk(p, ang, rng.uniform(45, 95) * lmul, rng.uniform(0.45, 0.60) * wmul, 0)
    r = 12.0
    for p, q, w in segs:
        lo = np.floor((np.minimum(p, q) - r) * px).astype(int).clip(0, res - 1)
        hi = np.ceil((np.maximum(p, q) + r) * px).astype(int).clip(0, res - 1)
        xs = (np.arange(lo[0], hi[0] + 1) + 0.5) / px
        ys = (np.arange(lo[1], hi[1] + 1) + 0.5) / px
        X, Y = np.meshgrid(xs, ys)
        d_ = q - p
        tt = np.clip(((X - p[0]) * d_[0] + (Y - p[1]) * d_[1]) / max(float(d_ @ d_), 1e-9), 0, 1)
        d = np.hypot(X - p[0] - tt * d_[0], Y - p[1] - tt * d_[1])
        sl = (slice(lo[1], hi[1] + 1), slice(lo[0], hi[0] + 1))
        core[sl] = np.maximum(core[sl], np.exp(-(d / w) ** 2))
        halo[sl] = np.maximum(halo[sl], np.exp(-(d / 2.0) ** 2))
        near[sl] = np.maximum(near[sl], np.exp(-(d / 6.0) ** 2))
    return core, halo, near, len(segs)


def porcelain_maps(res, side_mm, seed, crack, colour, rim_mm, n_cracks, mouth=None, wmul=1.0, lmul=1.0):
    """Cracked porcelain: a glazed off-white with slow warm mottling, a few long cracks in from the rim and a patchy
    fine net, mostly near them, grime in the cracks. Returns BC (sRGB), normal (OpenGL, +Y up the rows) and ORM."""
    rng = np.random.default_rng(seed)
    x = (np.arange(res) + 0.5) / res * side_mm
    X, Y = np.meshgrid(x, x)
    core, near_c, near6, nseg = crack_paths(res, side_mm, rim_mm, n_cracks, rng, wmul, lmul)
    log("mask: %d cracks from the rim, %d segments" % (n_cracks, nseg))
    fx = (fbm(X, Y, 5.0, rng, 3) - 0.5) * 2.0
    fy = (fbm(X, Y, 5.0, rng, 3) - 0.5) * 2.0
    fine = voronoi_edge(X + fx, Y + fy, 7.0, rng)
    patch = np.clip((fbm(X, Y, 30.0, rng, 3) - (0.72 - 0.4 * crack)) / 0.1, 0, 1)
    show_fine = np.maximum(patch, 0.7 * near6)
    line_major = core
    line_fine = np.exp(-(fine / (2 * 0.12)) ** 2) * show_fine * 0.6
    halo = near_c * 0.25
    mline = mwide = np.zeros_like(X)
    if mouth is not None and mouth[3] > 0:  # (x, y, half length, strength) in texture mm: a faint closed-lip line
        cx, cy, hl, st = mouth
        tt = np.clip((X - cx) / hl, -1, 1)
        dm = np.hypot(np.clip(np.abs(X - cx) - hl, 0, None), Y - (cy - 1.5 * tt ** 2))
        fade = (1 - 0.6 * np.abs(tt) ** 4) * st
        mline = np.exp(-(dm / 0.4) ** 2) * fade
        mwide = np.exp(-(dm / 1.6) ** 2) * fade
    lines = np.clip(line_major + line_fine + 0.35 * mline, 0, 1)
    mott = fbm(X, Y, 40.0, rng, 3) - 0.5
    base = np.array(colour, float)[None, None] * (1 + 0.04 * mott[..., None])
    base[..., 2] *= 1 - 0.03 * mott  # warmer in the mottles
    grime = np.array([0.20, 0.18, 0.16])
    bc = base * (1 - 0.18 * halo[..., None])
    bc = bc * (1 - lines[..., None] * 0.85) + grime[None, None] * lines[..., None] * 0.85
    height = -(line_major * 1.0 + line_fine * 0.5 + 0.8 * mline + 0.5 * mwide) + 0.02 * mott
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
    spacing = fopt(opt, "spacing", 3.5) * MM
    top = fopt(opt, "top", 72) * MM
    standoff = fopt(opt, "standoff", 3.5) * MM
    min_off = fopt(opt, "min_standoff", 3.0) * MM
    relief = fopt(opt, "relief", 8) * MM
    lip = fopt(opt, "lip", 1.0) * MM
    shell = fopt(opt, "shell", 3.0) * MM
    # the layout: a vertical axis through the ears' middle at the eyes' height; (s, u) = the arc along a cylinder of
    # radius R0 round it (s > 0 on the wearer's left) and the height above the eyes; r the distance out from the axis
    em = (head["earL"] + head["earR"]) / 2
    C0 = E + fwd * float((em - E) @ fwd)
    h0 = ray_skin(head, C0 + fwd * 0.3, -fwd)
    R0 = float((h0 - C0) @ fwd) if h0 is not None else 0.095

    def dirs(s):
        th = np.asarray(s, float) / R0
        return np.cos(th)[..., None] * fwd + np.sin(th)[..., None] * left

    def radial(s, u):  # the skin's distance out along each (s, u) ray, cast from outside; nan where it misses
        s, u = np.ravel(s), np.ravel(u)
        d = dirs(s)
        r = np.full(len(s), np.nan)
        for k in range(len(s)):
            a = C0 + up * u[k]
            hh = ray_skin(head, a + d[k] * 0.3, -d[k], 0.3)
            if hh is not None:
                r[k] = (hh - a) @ d[k]
        return r

    def world(s, u, r):
        return C0[None] + np.ravel(u)[:, None] * up[None] + dirs(np.ravel(s)) * np.ravel(r)[:, None]
    # the chin and the lips from the midline profile
    us = np.arange(-160, -50, 1.0) * MM
    prof = radial(np.zeros_like(us), us)
    if np.isfinite(prof).sum() < 10:
        sys.exit("head_prop: the midline profile under the eyes missed the face")
    chin_i = int(np.nanargmax(np.where(us < -90 * MM, prof, -np.inf)))
    under = chin_i
    chin_back = fopt(opt, "chin_back", 6) * MM
    while under > 0 and np.isfinite(prof[under - 1]) and prof[under - 1] > prof[chin_i] - chin_back:
        under -= 1
    bottom = us[under]
    mouth_u = None
    idx = np.nonzero((us > -95 * MM) & (us < -55 * MM) & np.isfinite(prof))[0]
    pk = [i for i in idx[1:-1] if prof[i] >= prof[i - 1] and prof[i] >= prof[i + 1]]
    if len(pk) >= 2:
        pk = sorted(sorted(pk, key=lambda i: -prof[i])[:2])
        mid = np.arange(pk[0], pk[1] + 1)
        mouth_u = float(us[mid[np.argmin(prof[mid])]])
    if mouth_u is None:
        mouth_u = -68 * MM
        log("mask: no two lip crests on the midline; the mouth line goes 68 mm below the eyes")
    # the width: "wrap" degrees each side, kept 10 degrees in front of the ears
    wrap = math.radians(fopt(opt, "wrap", 68))
    ear_th = []
    for sd in ("L", "R"):
        if head["ears"][sd]:
            v = head["F"][head["ears"][sd]] - C0
            hz = v - np.outer(v @ up, up)
            ear_th.append(float(np.abs(np.arctan2(hz @ left, hz @ fwd)).min()))
    if ear_th and wrap > min(ear_th) - math.radians(10):
        log("mask: wrap %.0f deg cut to %.0f (the ears start at %.0f)"
            % (math.degrees(wrap), math.degrees(min(ear_th)) - 10, math.degrees(min(ear_th))))
        wrap = min(ear_th) - math.radians(10)
    hw = R0 * wrap
    u_mid = fopt(opt, "widest", 5) * MM
    n_top, n_bot = fopt(opt, "n_top", 2.4), fopt(opt, "n_bottom", 2.0)
    log("mask: axis %.1f mm behind the eyes, R0 %.1f mm; chin forward %.1f mm below the eyes, bottom %.1f, mouth %.1f;"
        " %.1f..+%.1f mm high, %.0f deg (%.1f mm of arc) each side"
        % (-(C0 - E) @ fwd / MM, R0 / MM, -us[chin_i] / MM, -bottom / MM, -mouth_u / MM, bottom / MM, top / MM,
           math.degrees(wrap), hw / MM))

    def half_w(u):
        t = np.where(u >= u_mid, (u - u_mid) / (top - u_mid), (u_mid - u) / (u_mid - bottom))
        n = np.where(u >= u_mid, n_top, n_bot)
        return hw * np.clip(1 - np.clip(t, 0, 1) ** n, 0, 1) ** (1 / n)
    # outline: up the wearer's right side (-s), down the left (+s); the ends close at the top and bottom points
    uo = bottom + (top - bottom) * (0.5 - 0.5 * np.cos(np.linspace(0, math.pi, 400)))  # denser near the ends
    wo = half_w(uo)
    outline = np.vstack([np.stack([-wo, uo], 1), np.stack([wo[::-1], uo[::-1]], 1)[1:-1]])
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
        for s in np.arange(-w + off, w, spacing):
            pts.append((s, u))
    allp = [Vector((p[0], p[1])) for p in ol] + [Vector(p) for p in pts]
    edges = [(i, (i + 1) % nb) for i in range(nb)]
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
    for f in faces:  # counter-clockwise in (s, u): the normal then points out from the axis
        a, b, c = V2[f[0]], V2[f[1]], V2[f[2]]
        if (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]) < 0:
            f.reverse()
    nV = len(V2)
    # the skin's radius on a 1.5 mm grid with a margin round the outline
    g = 1.5 * MM
    gs = np.arange(-hw - 20 * MM, hw + 20 * MM + g / 2, g)
    gu = np.arange(bottom - 4 * MM, top + 20 * MM + g / 2, g)
    S, U = np.meshgrid(gs, gu)
    rsk = radial(S, U).reshape(S.shape)
    valid = np.isfinite(rsk) & (U >= bottom - 1 * MM)
    ear_ids = list(head["ears"]["L"]) + list(head["ears"]["R"])
    if ear_ids:
        kd = KDTree(len(ear_ids))
        for i, v in enumerate(head["F"][ear_ids]):
            kd.insert(Vector(v), i)
        kd.balance()
        Pg = world(S, U, np.nan_to_num(rsk))
        for k in np.nonzero(valid.ravel())[0]:
            if kd.find(Vector(Pg[k]))[2] < 6 * MM:
                valid.flat[k] = False

    def blur(a, sig):
        sp = sig / g
        x = np.arange(-int(3 * sp) - 1, int(3 * sp) + 2)
        kk = np.exp(-0.5 * (x / sp) ** 2)
        kk /= kk.sum()
        a = np.apply_along_axis(np.convolve, 0, a, kk, mode="same")
        return np.apply_along_axis(np.convolve, 1, a, kk, mode="same")

    def nconv(val, wgt, sig):  # a Gaussian over the valid cells only
        return blur(np.where(wgt > 0, val, 0.0) * wgt, sig) / np.maximum(blur(wgt, sig), 1e-9)

    def grow(a, n):
        o = a.copy()
        for dy in range(-n, n + 1):
            for dx in range(-n, n + 1):
                o = np.maximum(o, np.roll(np.roll(a, dy, 0), dx, 1))
        return o
    for _ in range(2):  # rays through the eye and mouth openings land deep: drop them
        sm = nconv(rsk, valid.astype(float), 6 * MM)
        valid &= ~(rsk < sm - 10 * MM)
    log("mask: %d x %d grid, %d cells on the skin" % (len(gu), len(gs), int(valid.sum())))
    def concave_major(y):  # the least concave function over y's finite samples (bridges every hollow), nan kept outside
        x = np.nonzero(np.isfinite(y))[0]
        if len(x) < 3:
            return y
        hull = []
        for i in x:
            while len(hull) >= 2 and ((y[hull[-1]] - y[hull[-2]]) * (i - hull[-2])
                                      <= (y[i] - y[hull[-2]]) * (hull[-1] - hull[-2])):
                hull.pop()
            hull.append(i)
        o2 = y.copy()
        o2[x[0]:x[-1] + 1] = np.interp(np.arange(x[0], x[-1] + 1), hull, y[hull])
        return o2

    if opt.get("envelope", "1") != "0":
        # a blank shell: across each row and down each column the least bulging curve that clears the skin by
        # min_standoff (no eye pits, lips or brow ridges: only what stands out most, the nose tip, brows, cheekbones and
        # chin, holds it up), then a light membrane smoothing that never comes nearer than min_standoff
        env_s = fopt(opt, "env_smooth", 7) * MM
        fl = np.where(valid, rsk + min_off, -np.inf)
        rm = np.where(valid, rsk + min_off, np.nan)
        for _ in range(2):
            rm = np.apply_along_axis(concave_major, 1, rm)
            rm = np.apply_along_axis(concave_major, 0, rm)
        fin = np.isfinite(rm)
        rm = nconv(np.nan_to_num(rm), fin.astype(float), env_s)
        ones = np.ones(S.shape)
        for _ in range(int(opt.get("env_iter", 25))):
            rm = np.maximum(nconv(rm, ones, env_s), fl)
        # the rim: over its last "taper" mm the shell comes down to the lightly smoothed skin + rim_off
        taper = fopt(opt, "taper", 14) * MM
        rim_off = fopt(opt, "rim_off", 2.5) * MM
        dense = []
        for k in range(nb):
            a0, b0 = ol[k], ol[(k + 1) % nb]
            nn = max(int(np.linalg.norm(b0 - a0) / (0.5 * MM)), 1)
            dense += [a0 + (b0 - a0) * t for t in np.arange(nn) / nn]
        kd_o = KDTree(len(dense))
        for i, q in enumerate(dense):
            kd_o.insert(Vector((q[0], q[1], 0.0)), i)
        kd_o.balance()
        dist_o = np.array([kd_o.find(Vector((a0, b0, 0.0)))[2] for a0, b0 in zip(S.ravel(), U.ravel())]).reshape(S.shape)
        wt = np.clip(dist_o / max(taper, 1e-9), 0, 1)
        wt = wt * wt * (3 - 2 * wt)
        rim_r = nconv(rsk, valid.astype(float), 5 * MM) + rim_off
        stand = np.where(valid, rm - rsk, np.nan)
        rm = rim_r + (rm - rim_r) * wt
        j0 = int(np.argmin(np.abs(gs)))
        mid = (gu >= bottom) & (gu <= top)
        prof_m = rm[mid, j0]
        sag = float(np.nanmax(concave_major(prof_m.copy()) - prof_m))
        ins = (wt > 0.999) & valid
        log("mask: envelope (rows and columns bridged, smoothed %.0f mm): off the skin inside the taper p50 %.1f p95 %.1f"
            " max %.1f mm; rim tapers to %.1f mm over %.0f mm; the midline sags %.2f mm below its hull"
            % (env_s / MM, np.nanpercentile(stand[ins], 50) / MM, np.nanpercentile(stand[ins], 95) / MM,
               np.nanmax(stand[ins]) / MM, rim_off / MM, taper / MM, sag / MM))
    else:
        r_s = nconv(rsk, valid.astype(float), relief)
        a_ = r_s + standoff
        b_ = np.where(valid, rsk + min_off, a_ - 1.0)
        rm = 0.5 * (a_ + b_ + np.sqrt((a_ - b_) ** 2 + (2 * MM) ** 2))  # a soft max: no crease where the skin rises
    if len(head["lash_vids"]):
        Lv = head["F"][head["lash_vids"]] - C0
        lu = Lv @ up
        hz = Lv - np.outer(lu, up)
        ls = np.arctan2(hz @ left, hz @ fwd) * R0
        iu = np.round((lu - gu[0]) / g).astype(int)
        is_ = np.round((ls - gs[0]) / g).astype(int)
        ok = (iu >= 0) & (iu < len(gu)) & (is_ >= 0) & (is_ < len(gs))
        fl = np.full(S.shape, -np.inf)
        np.maximum.at(fl, (iu[ok], is_[ok]), np.linalg.norm(hz, axis=1)[ok] + min_off)
        e = np.clip(grow(fl, 2) - rm, 0, None)
        rm = rm + np.maximum(blur(grow(e, 2), 3 * MM), e)
        log("mask: lashes raise the shell by up to %.1f mm" % (e.max() / MM))
    region = (U >= bottom - 2 * MM) & (np.abs(S) <= hw + 6 * MM)
    for it in range(3):  # the true distance: anything nearer than min_off is lifted along its ray, smoothly
        d, n = signed_dist(head, world(S[region], U[region], rm[region]))
        need = np.zeros(S.shape)
        need[region] = np.clip(min_off - d, 0, None) / np.clip(np.sum(n * dirs(S[region]), 1), 0.3, 1.0)
        if need.max() < 0.05 * MM:
            break
        rm = rm + np.maximum(blur(grow(need, 2), 3 * MM), need)
        log("mask: hold pass %d lifted up to %.1f mm" % (it + 1, need.max() / MM))

    def sample(field, s, u):  # bilinear on the grid
        fx, fy = (s - gs[0]) / g, (u - gu[0]) / g
        x0 = np.clip(np.floor(fx).astype(int), 0, len(gs) - 2)
        y0 = np.clip(np.floor(fy).astype(int), 0, len(gu) - 2)
        tx, ty = np.clip(fx - x0, 0, 1), np.clip(fy - y0, 0, 1)
        return (field[y0, x0] * (1 - tx) * (1 - ty) + field[y0, x0 + 1] * tx * (1 - ty)
                + field[y0 + 1, x0] * (1 - tx) * ty + field[y0 + 1, x0 + 1] * tx * ty)
    rv = sample(rm, V2[:, 0], V2[:, 1])
    P = world(V2[:, 0], V2[:, 1], rv)
    d, n = signed_dist(head, P)
    fix = np.clip(min_off - d, 0, None) / np.clip(np.sum(n * dirs(V2[:, 0]), 1), 0.3, 1.0)
    if fix.max() > 0:
        rv = rv + fix
        P = world(V2[:, 0], V2[:, 1], rv)
        log("mask: %d sheet points lifted up to %.2f mm after the sampling" % (int((fix > 0).sum()), fix.max() / MM))
    d, _ = signed_dist(head, P)
    log("mask sheet: %d verts, %d tris; off the skin min %.2f p5 %.2f p50 %.2f p95 %.2f mm"
        % (nV, len(faces), d.min() / MM, np.percentile(d, 5) / MM, np.median(d) / MM, np.percentile(d, 95) / MM))
    me = bpy.data.meshes.new(name)
    me.from_pydata(P.tolist(), [], faces)
    me.validate()
    o = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(o)
    # UVs from the (s, u) layout over one square
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
    me.update()
    bm = bmesh.new()
    bm.from_mesh(me)
    bnd = sorted({v.index for e in bm.edges if e.is_boundary for v in e.verts})
    bm.free()
    nrm = np.array([tuple(me.vertices[i].normal) for i in bnd])
    bpy.ops.object.select_all(action="DESELECT")
    o.select_set(True)
    bpy.context.view_layer.objects.active = o
    sol = o.modifiers.new("shell", "SOLIDIFY")
    sol.thickness = shell
    sol.offset = 1.0
    sol.use_rim = True
    sol.use_even_offset = True
    sol.use_quality_normals = True
    bpy.ops.object.modifier_apply(modifier=sol.name)
    # the inner rim turns towards the face, so the edge reads as a rolled porcelain lip, not a lifted sheet
    co = np.empty(len(me.vertices) * 3)
    me.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    inner = None
    if len(co) >= 2 * nV:
        if np.allclose(co[:nV], P, atol=1e-6):
            inner, outer = np.array(bnd), np.array(bnd) + nV
        elif np.allclose(co[nV:2 * nV], P, atol=1e-6):
            inner, outer = np.array(bnd) + nV, np.array(bnd)
    flat = 0
    if inner is None:
        log("mask: the shell's vertex order isn't the sheet's; no lip, the rim stays smooth")
    else:
        lip_mv = np.zeros(len(inner))
        if lip > 0:  # never nearer the skin than min_standoff
            d_in, _ = signed_dist(head, co[inner])
            lip_mv = np.clip(d_in - min_off, 0, lip)
            co[inner] -= nrm * lip_mv[:, None]
            me.vertices.foreach_set("co", co.ravel())
            me.update()
            d_in, _ = signed_dist(head, co[inner])
            log("mask: inner rim turned in %.1f..%.1f mm (median %.1f); the rim off the skin min %.2f p50 %.2f mm"
                % (lip_mv.min() / MM, lip_mv.max() / MM, np.median(lip_mv) / MM, d_in.min() / MM, np.median(d_in) / MM))
        rim = set(inner.tolist()) | set(outer.tolist())
        if fopt(opt, "bevel", 0.9) <= 0:
            for p in me.polygons:  # the rim's quads flat-shaded so the edge reads as a cut shell
                if len(p.vertices) == 4 and all(v in rim for v in p.vertices):
                    p.use_smooth = False
                    flat += 1
            log("mask: %d rim faces flat" % flat)
    bev = fopt(opt, "bevel", 0.9) * MM
    if bev > 0:  # the edge rounded over: a glazed rolled rim, not a cut plate
        bv = o.modifiers.new("bevel", "BEVEL")
        bv.width = bev
        bv.segments = 3
        bv.limit_method = "ANGLE"
        bv.angle_limit = math.radians(50)
        bpy.ops.object.modifier_apply(modifier=bv.name)
        for p in me.polygons:
            p.use_smooth = True
        log("mask: rim bevelled %.1f mm, 3 segments" % (bev / MM))
    # porcelain set
    res = int(opt.get("tex", 2048))
    col = [float(x) for x in opt.get("colour", "0.86,0.84,0.80").split(",")]
    rim_mm = (ol - c2) / MM + side / MM / 2  # the outline in texture mm
    mouth = ((0 - c2[0]) / MM + side / MM / 2, (mouth_u - c2[1]) / MM + side / MM / 2, fopt(opt, "mouth_len", 22),
             fopt(opt, "mouth", 0.5))
    bc, n_gl, orm = porcelain_maps(res, side / MM, int(opt.get("seed", 3)), fopt(opt, "crack", 0.25), col, rim_mm,
                                   int(opt.get("cracks", 4)), mouth, fopt(opt, "crack_w", 1.0),
                                   fopt(opt, "crack_len", 1.0))
    n_dx = n_gl.copy()
    n_dx[..., 1] = 1.0 - n_dx[..., 1]
    im_bc = save_png(os.path.join(out, name + "_BC.png"), bc, True)
    save_png(os.path.join(out, name + "_N.png"), n_dx, False)
    im_ngl = save_png(os.path.join(out, name + "_N_gl.png"), n_gl, False)
    im_orm = save_png(os.path.join(out, name + "_ORM.png"), orm, False)
    me.materials.append(preview_material("M_" + name, im_bc, im_ngl, im_orm))
    report = {"mode": "mask", "layout": "cylinder round the ears' axis", "R0_mm": R0 / MM, "wrap_deg": math.degrees(wrap),
              "span_mm": [bottom / MM, top / MM], "mouth_mm": mouth_u / MM, "relief_mm": relief / MM,
              "sheet_off_skin_mm": {"min": float(d.min() / MM), "p5": float(np.percentile(d, 5) / MM),
                                    "p50": float(np.median(d) / MM), "p95": float(np.percentile(d, 95) / MM)},
              "envelope": opt.get("envelope", "1") != "0", "shell_mm": shell / MM, "lip_mm": float(np.median(lip_mv) / MM) if inner is not None else 0.0, "rim_faces_flat": flat,
              "face_hide": "not needed" if d.min() >= 2.5 * MM else "CLIPS: hide the covered face region",
              "normal": "DirectX in _N.png", "texture_side_mm": side / MM}
    skin_and_export(head, o, name, out, report)


# -- hair cap -----------------------------------------------------------------------------------------------------------

def smooth01(t):
    t = np.clip(t, 0.0, 1.0)
    return t * t * (3 - 2 * t)


def gauss_blur(img, sigma):
    """A Gaussian blur of a 2D array by FFT (wrap-around is harmless: the cap's layout leaves a blank border)."""
    h, w = img.shape
    fy, fx = np.fft.fftfreq(h)[:, None], np.fft.fftfreq(w)[None, :]
    k = np.exp(-2 * (math.pi * sigma) ** 2 * (fx * fx + fy * fy))
    return np.real(np.fft.ifft2(np.fft.fft2(img) * k))


def load_roots(head, roots_npz):
    """The groom's roots (a groom_strands.py dump: Unreal's groom frame, cm) in the face FBX's frame (m): Y flips. The
    x sign is whichever puts the roots on the skin; exits when neither does (wrong groom or face)."""
    d = np.load(roots_npz)
    R0 = d["P"][np.concatenate([[0], np.cumsum(d["n"])[:-1]])]
    best = None
    for tag, s in (("x,-y,z", np.array([1, -1, 1])), ("-x,-y,z", np.array([-1, -1, 1]))):
        R = R0 * s / 100.0
        dist, _ = signed_dist(head, R[:: max(1, len(R) // 2000)])
        med = float(np.median(np.abs(dist)))
        log("roots as %s sit p50 %.2f mm off the skin" % (tag, med / MM))
        if best is None or med < best[0]:
            best = (med, tag, R)
    if best[0] > 3 * MM:
        sys.exit("head_prop: the roots do not sit on this face (p50 %.1f mm); wrong groom or face" % (best[0] / MM))
    return best


def parse_hairline(opt):
    hl = np.array([[float(a) for a in p.split(":")] for p in opt.get("hairline", "0:1.1,1.0:0.5,1.2:0").split(",")])
    return hl[np.argsort(hl[:, 0])]


MEASURE_ZONES = ("fringe", "temples", "crown", "nape-occiput")


def measure_zones(head, opt):
    """The zone test and each zone's skin (area, vertices) on this face, for run_measure and the transplant."""
    hl = parse_hairline(opt)
    ipd = float(np.linalg.norm(head["eyeL"] - head["eyeR"]))
    eye_mid, fwd, up, left = head["eye_mid"], head["fwd"], head["up"], head["left"]
    ear_mid = (head["earL"] + head["earR"]) / 2
    band, tx = fopt(opt, "band", 25) * MM, fopt(opt, "temple_x", 0.6)
    crown_c = math.cos(math.radians(fopt(opt, "crown_deg", 32)))
    back, nape, occ = fopt(opt, "back", 40) * MM, fopt(opt, "nape", 50) * MM, fopt(opt, "occ", 40) * MM

    def zone_of(P):
        q = P - eye_mid
        x, z = np.abs(q @ left) / ipd, (q @ up) / ipd
        hz = np.interp(x, hl[:, 0], hl[:, 1])
        e = P - ear_mid
        a, h = e @ fwd, e @ up
        v = e / np.maximum(np.linalg.norm(e, axis=1, keepdims=True), 1e-12)
        front = (a > 0) & (z >= hz) & (z <= hz + band / ipd)
        Z = np.full(len(P), -1)
        Z[front & (x < tx)] = 0
        Z[front & (x >= tx) & (x < 1.15)] = 1
        Z[(v @ up >= crown_c) & (a <= 0)] = 2
        Z[(a < -back) & (h >= -nape) & (h <= occ)] = 3
        return Z

    face0, F = head["face0"], head["F"]
    me = face0.data
    mi = np.empty(len(me.polygons), dtype=np.int64)
    me.polygons.foreach_get("material_index", mi)
    roles = face_roles(face0, head["eyeL"], head["eyeR"])
    skin = next(s for s, r in roles.items() if r == "skin")
    ears = np.zeros(len(F), bool)
    ears[head["ears"]["L"] + head["ears"]["R"]] = True
    polys = [list(p.vertices) for p in me.polygons]
    ks = [k for k in np.nonzero(mi == skin)[0] if not ears[polys[k]].any()]
    cen = np.array([F[polys[k]].mean(0) for k in ks])
    area = np.array([sum(0.5 * np.linalg.norm(np.cross(F[polys[k][i]] - F[polys[k][0]], F[polys[k][i + 1]] - F[polys[k][0]]))
                         for i in range(1, len(polys[k]) - 1)) for k in ks])
    zc = zone_of(cen)
    zarea = [float(area[zc == i].sum()) / 1e-4 for i in range(4)]  # cm2
    ec = cen - ear_mid
    bb = np.floor((ec @ up) / (10 * MM)).astype(int)
    bk = (ec @ fwd) < -back
    bands = list(range(-6, 10))
    barea = [float(area[bk & (bb == b)].sum()) / 1e-4 for b in bands]
    sv = np.unique(np.concatenate([polys[k] for k in ks]))
    return {"hl": hl, "ipd": ipd, "zone_of": zone_of, "zarea": zarea, "sv": sv, "zv": zone_of(F[sv]), "ear_mid": ear_mid,
            "bands": bands, "barea": barea, "back": back, "polys": [polys[k] for k in ks], "parea": area, "pzone": zc,
            "pcen": cen}


def measure_groom(head, Z, R, opt, P=None, sid=None):
    """Hairline stations, zone densities and bare shares of roots R (face frame, m); see run_measure. With the
    strands' points P and their strand ids sid, a station measures the fringe edge instead of the roots: each strand's
    lowest point in the station band, 5th percentile over the strands."""
    hl, ipd, zarea, sv, zv = Z["hl"], Z["ipd"], Z["zarea"], Z["sv"], Z["zv"]
    tol, minf, bare_r = fopt(opt, "tol", 8), fopt(opt, "min_frac", 0.6), fopt(opt, "bare", 4) * MM
    eye_mid, fwd, left, up = head["eye_mid"], head["fwd"], head["left"], head["up"]
    q = R - eye_mid
    sx, rz = (q @ left) / ipd, (q @ up) / ipd
    rz = rz - np.interp(np.abs(sx), hl[:, 0], hl[:, 1])  # height over the sheet's line at the root's own x
    ahead = ((R - Z["ear_mid"]) @ fwd) > 0
    if P is not None:
        pq = P - eye_mid
        psx, prz = (pq @ left) / ipd, (pq @ up) / ipd
        prz = prz - np.interp(np.abs(psx), hl[:, 0], hl[:, 1])
        pahead = ((P - Z["ear_mid"]) @ fwd) > 0
    hair = {}
    for st in (0.0, 0.35, 0.45, 0.55, 0.75, 0.95):
        for side, sg in ((("L", 1), ("R", -1)) if st > 0 else (("C", 0),)):
            m = ahead & ((np.abs(sx) < 0.05) if st == 0 else (np.abs(sg * sx - st) < 0.05))
            if P is None:
                low, cnt = (np.percentile(rz[m], 2) if m.sum() >= 20 else None), m.sum()
            else:
                pm = pahead & ((np.abs(psx) < 0.05) if st == 0 else (np.abs(sg * psx - st) < 0.05))
                lo = np.full(int(sid[-1]) + 1, np.inf)
                np.minimum.at(lo, sid[pm], prz[pm])
                lo = lo[np.isfinite(lo)]
                low, cnt = (np.percentile(lo, 5) if len(lo) >= 20 else None), len(lo)
            hair["%s%.2f" % (side, st)] = (round(float(low * ipd / MM), 1)
                                          if cnt >= 20 else None)
    er = R - Z["ear_mid"]
    rb, rk = np.floor((er @ up) / (10 * MM)).astype(int), (er @ fwd) < -Z["back"]
    prof = {"%+d" % (10 * b): round(float((rk & (rb == b)).sum()) / a, 1) for b, a in zip(Z["bands"], Z["barea"]) if a > 0.5}
    zr = Z["zone_of"](R)
    dens = [float((zr == i).sum()) / max(zarea[i], 1e-9) for i in range(4)]
    kd = KDTree(len(R))
    for i, p in enumerate(R.tolist()):
        kd.insert(p, i)
    kd.balance()
    F = head["F"]
    bare = []
    for i in range(4):
        vv = F[sv[zv == i]]
        bare.append(round(100.0 * float(np.mean([kd.find(p)[2] > bare_r for p in vv.tolist()])), 1) if len(vv) else 0.0)
    fails = [k for k, g in hair.items() if g is None or abs(g) > tol]
    fails += ["%s %.0f%% of crown" % (MEASURE_ZONES[i], 100 * dens[i] / max(dens[2], 1e-9))
              for i in (0, 1, 3) if dens[i] < minf * dens[2]]
    return {"strands": int(len(R)), "hairline_mm": hair, "density_per_cm2": dict(zip(MEASURE_ZONES, [round(d, 1) for d in dens])),
            "bare_pct": dict(zip(MEASURE_ZONES, bare)), "back_profile_per_cm2": prof, "pass": not fails, "fails": fails}


def measure_line(name, row):
    return "%s %d strands; hairline %s; density %s; bare%% %s; %s" % (
        name, row["strands"], " ".join("%s:%s" % (k, "none" if g is None else "%+.1f" % g) for k, g in row["hairline_mm"].items()),
        " ".join("%s %.1f" % kv for kv in row["density_per_cm2"].items()), " ".join("%s %.0f" % kv for kv in row["bare_pct"].items()),
        "PASS" if row["pass"] else "FAIL: " + ", ".join(row["fails"])) + "; back mm:dens " + " ".join(
        "%s:%.0f" % kv for kv in row["back_profile_per_cm2"].items())


def run_measure(head, out, opt):
    """Front hairline and root density of each groom dump in roots= (';'-separated) on this face, one table.
    Hairline: at each station (|x| 0, 0.35, 0.45, 0.55, 0.75, 0.95 IPD from the eye centres' midpoint, each side), the
    lowest roots (2nd percentile) in a 0.1 IPD band in front of the ears minus the sheet's line, mm (+ = the groom's
    line higher). Density: roots per cm2 of skin in four zones (fringe band and temples: from the sheet's line to
    "band" mm (25) above it, |x| under and over "temple_x" (0.6) up to 1.15 IPD; crown: within "crown_deg" (32) of
    the vertical through the ears' middle and no further forward than it; nape to occiput: "back" mm (40) behind the
    ears, from "nape" mm (50) below them to "occ" mm (40) above), and the share of each zone's skin vertices with no
    root within "bare" mm (4). Pass: every station within "tol" mm (8) and every zone at least "min_frac" (0.6) of the
    crown's density."""
    Z = measure_zones(head, opt)
    log("measure: IPD %.1f mm; zone skin areas cm2 %s" % (Z["ipd"] / MM, ", ".join("%s %.1f" % (n, a) for n, a in zip(MEASURE_ZONES, Z["zarea"]))))
    rows = []
    for npz in [r for r in opt.get("roots", "").split(";") if r]:
        med, frame, R = load_roots(head, npz)
        row = {"groom": os.path.basename(npz).replace("_dump.npz", ""), "frame": frame, "roots_off_skin_p50_mm": round(med / MM, 2)}
        P = sid = None
        if opt.get("edge") == "fringe":  # measure the fringe's edge (all strand points), not the root line
            d = np.load(npz)
            P = d["P"] * (np.array([1, -1, 1]) if frame == "x,-y,z" else np.array([-1, -1, 1])) / 100.0
            sid = np.repeat(np.arange(len(d["n"])), d["n"])
            row["edge"] = "fringe"
        row.update(measure_groom(head, Z, R, opt, P, sid))
        rows.append(row)
        log("measure: " + measure_line(row["groom"], row))
    rep_ = {"mode": "measure", "ipd_mm": Z["ipd"] / MM, "hairline_ipd": Z["hl"].tolist(),
            "zone_area_cm2": dict(zip(MEASURE_ZONES, Z["zarea"])), "tol_mm": fopt(opt, "tol", 8),
            "min_frac": fopt(opt, "min_frac", 0.6), "grooms": rows}
    with open(os.path.join(out, opt.get("name", "measure") + ".json"), "w") as f:
        json.dump(rep_, f, indent=1)


def run_transplant(head, out, opt):
    """New roots for one groom (roots=, a groom_strands.py dump) where its root density falls short, each with a donor
    strand to copy: groom_strands.py's "graft" op then copies the donor onto the new root. Two places: the front (in
    front of the ears, |x| up to 1.15 IPD, from the sheet's line to "gmax" mm (35) above it), filled to the groom's
    own front density (the larger of its fringe and temple zones' densities), and the back (the nape-to-occiput zone
    and up to the crown's lower edge, "back" mm behind the ears), filled to "back_target" times (1.0) its own
    nape-to-occiput density. A skin point is sampled at the target density and kept with the chance that brings the
    local density (roots within "r" mm, 4) up to the target; its donor is one of the "k" (8) nearest existing roots,
    picked at random from those at least as far from the sheet's line (front) or as high (back), so copies carry the
    neighbouring length and direction. Writes OUT/<name>_graft.npz (roots in the groom's frame, cm, and donor strand
    indices) and a report with the measure before and after (the new roots counted as roots)."""
    rng = np.random.default_rng(int(fopt(opt, "seed", 1)))
    Z = measure_zones(head, opt)
    npz = opt["roots"]
    med, frame, R = load_roots(head, npz)
    sgn = np.array([1, -1, 1]) if frame == "x,-y,z" else np.array([-1, -1, 1])
    before = measure_groom(head, Z, R, opt)
    log("transplant: before " + measure_line(os.path.basename(npz), before))
    ipd, hl, eye_mid = Z["ipd"], Z["hl"], head["eye_mid"]
    fwd, up, left = head["fwd"], head["up"], head["left"]
    gmax, rr, kn = fopt(opt, "gmax", 35) * MM, fopt(opt, "r", 4) * MM, int(fopt(opt, "k", 8))
    dens = before["density_per_cm2"]
    t_front = max(dens["fringe"], dens["temples"])
    t_back = fopt(opt, "back_target", 1.0) * dens["nape-occiput"]

    def gap(Pp):  # height over the sheet's line at the point's own x (m)
        q = Pp - eye_mid
        return (q @ up) - np.interp(np.abs(q @ left) / ipd, hl[:, 0], hl[:, 1]) * ipd

    def region(Pp):
        e = Pp - Z["ear_mid"]
        a, h = e @ fwd, e @ up
        x = np.abs((Pp - eye_mid) @ left) / ipd
        g = gap(Pp)
        front = (a > 0) & (x < 1.15) & (g >= 0) & (g <= gmax)
        back = (a < -Z["back"]) & (h >= -fopt(opt, "nape", 50) * MM) & (h <= fopt(opt, "back_top", 70) * MM)
        return np.where(front, 1, np.where(back, 2, 0))

    # skin triangles (fans of the skin polygons) and their regions
    F = head["F"]
    tri = np.array([(pp[0], pp[i], pp[i + 1]) for pp in Z["polys"] for i in range(1, len(pp) - 1)])
    ta = 0.5 * np.linalg.norm(np.cross(F[tri[:, 1]] - F[tri[:, 0]], F[tri[:, 2]] - F[tri[:, 0]]), axis=1)
    treg = region(F[tri].mean(1))
    kd = KDTree(len(R))
    for i, pp in enumerate(R.tolist()):
        kd.insert(pp, i)
    kd.balance()
    cell = math.pi * (rr / 0.01) ** 2  # cm2
    new, donor, where = [], [], []
    for rg, target in ((1, t_front), (2, t_back)):
        sel = np.flatnonzero(treg == rg)
        A = ta[sel].sum() / 1e-4
        n = rng.poisson(target * A)
        if n == 0:
            continue
        t = sel[rng.choice(len(sel), n, p=ta[sel] / ta[sel].sum())]
        u, v = rng.random(n), rng.random(n)
        flip = u + v > 1
        u[flip], v[flip] = 1 - u[flip], 1 - v[flip]
        S = F[tri[t, 0]] + (F[tri[t, 1]] - F[tri[t, 0]]) * u[:, None] + (F[tri[t, 2]] - F[tri[t, 0]]) * v[:, None]
        S = S[region(S) == rg]
        local = np.array([len(kd.find_range(pp, rr)) for pp in S.tolist()]) / cell
        keep = rng.random(len(S)) < np.clip(1 - local / target, 0, 1)
        S = S[keep]
        gS = gap(S) if rg == 1 else (S - Z["ear_mid"]) @ up
        gR = gap(R) if rg == 1 else (R - Z["ear_mid"]) @ up
        for pp, g in zip(S.tolist(), gS):
            near = [j for _, j, _ in kd.find_n(pp, kn * 3)]
            ok = [j for j in near if gR[j] >= g][:kn] or near[:kn]
            donor.append(int(rng.choice(ok)))
            new.append(pp)
            where.append(rg)
        log("transplant: %s target %.1f roots/cm2 on %.1f cm2: %d new roots" % ("front" if rg == 1 else "back", target, A, len(S)))
    new = np.array(new).reshape(-1, 3)
    after = measure_groom(head, Z, np.concatenate([R, new]), opt)
    log("transplant: after (new roots counted) " + measure_line(os.path.basename(npz), after))
    name = opt.get("name", "transplant")
    np.savez(os.path.join(out, name + "_graft.npz"), roots=new * sgn * 100.0, donor=np.array(donor, np.int64),
             where=np.array(where, np.int64))
    with open(os.path.join(out, name + ".json"), "w") as f:
        json.dump({"mode": "transplant", "groom": npz, "frame": frame, "strands_before": int(len(R)),
                   "new_front": int(sum(1 for w in where if w == 1)), "new_back": int(sum(1 for w in where if w == 2)),
                   "strands_after": int(len(R) + len(new)), "target_front": t_front, "target_back": t_back,
                   "before": before, "after": after}, f, indent=1)


def run_cap(head, roots_npz, out, opt):
    name = opt.get("name", "SK_HairCap")
    res = int(opt.get("tex", 1024))
    offset, feather, sigma = fopt(opt, "offset", 0.5) * MM, fopt(opt, "feather", 4.0) * MM, fopt(opt, "spread", 3.0) * MM
    face0, F, sgn = head["face0"], head["F"], head["sgn"]
    eye_mid, fwd, up, left = head["eye_mid"], head["fwd"], head["up"], head["left"]
    ipd = float(np.linalg.norm(head["eyeL"] - head["eyeR"]))
    ear_mid = (head["earL"] + head["earR"]) / 2
    # the sheet's front-view hairline, symmetric: "|x|:z" pairs in IPDs from the eye centres' midpoint (x across, z up)
    hl = parse_hairline(opt)

    med, frame, R = load_roots(head, roots_npz)

    def sheet_alpha(p):
        q = p - eye_mid
        x, z = np.abs(q @ left) / ipd, (q @ up) / ipd
        a = smooth01((z - np.interp(x, hl[:, 0], hl[:, 1])) * ipd / feather + 0.5)
        return a * smooth01(((p - ear_mid) @ fwd) / (10 * MM) + 0.5)  # in front of the ears only

    # how far the groom's front hairline sits from the sheet's, across the face: per band of |x| (0.1 IPD), the
    # lowest roots (2nd percentile) in front of the ears against the sheet's line there; + = groom's line higher
    q = R - eye_mid
    rx, rz = np.abs(q @ left) / ipd, (q @ up) / ipd
    ahead = ((R - ear_mid) @ fwd) > 0
    gap = []
    for b0 in np.arange(0.0, hl[hl[:, 1] >= 0, 0].max(), 0.1):
        m = ahead & (rx >= b0) & (rx < b0 + 0.1)
        if m.sum() >= 20:
            xc = b0 + 0.05
            gap.append((round(float(xc), 2), round(float((np.percentile(rz[m], 2) - np.interp(xc, hl[:, 0], hl[:, 1]))
                                                          * ipd / MM), 1)))
    g = np.array([d for _, d in gap]) if gap else np.zeros(1)
    log("cap: front hairline, groom minus sheet (mm, + = groom's higher) by |x| in IPD: %s; p50 %.1f, max |%.1f|"
        % (", ".join("%.2f:%+.1f" % t for t in gap), float(np.median(g)), float(np.abs(g).max())))

    # the cap's faces: skin near a root or inside the sheet's hairline, ears left out
    me = face0.data
    mi = np.empty(len(me.polygons), dtype=np.int64)
    me.polygons.foreach_get("material_index", mi)
    roles = face_roles(face0, head["eyeL"], head["eyeR"])
    skin = next(s for s, r in roles.items() if r == "skin")
    kd = KDTree(len(R))
    for i, p in enumerate(R.tolist()):
        kd.insert(p, i)
    kd.balance()
    near = np.array([len(kd.find_range(p, 6 * MM)) > 0 for p in F.tolist()])
    sel_v = (near | (sheet_alpha(F) > 0.01))
    ears = np.zeros(len(F), bool)
    ears[head["ears"]["L"] + head["ears"]["R"]] = True
    sel_v &= ~ears
    polys = [tuple(p.vertices) for p in me.polygons]
    keep = [k for k in np.nonzero(mi == skin)[0] if sel_v[list(polys[k])].any() and not ears[list(polys[k])].any()]
    used = np.unique(np.concatenate([np.array(polys[k]) for k in keep]))
    remap = -np.ones(len(F), np.int64)
    remap[used] = np.arange(len(used))
    vn = np.empty(len(me.vertices) * 3)
    me.vertices.foreach_get("normal", vn)
    M3 = np.array(face0.matrix_world)[:3, :3]
    vn = vn.reshape(-1, 3) @ M3.T
    vn /= np.maximum(np.linalg.norm(vn, axis=1, keepdims=True), 1e-12)
    V = F[used] + vn[used] * sgn * offset
    faces = [tuple(remap[list(polys[k])][::-1] if sgn < 0 else remap[list(polys[k])]) for k in keep]

    # UVs: an azimuthal layout round the vertical through the ears' middle (no seam), the crown at the centre
    def layout(p, phimax):
        v = p - ear_mid
        v = v / np.maximum(np.linalg.norm(v, axis=1, keepdims=True), 1e-12)
        phi = np.arccos(np.clip(v @ up, -1, 1))
        th = np.arctan2(v @ left, v @ fwd)
        r = 0.5 * phi / phimax
        return np.stack([0.5 + r * np.sin(th), 0.5 + r * np.cos(th)], 1)
    v0 = V - ear_mid
    phimax = float(np.arccos(np.clip((v0 / np.linalg.norm(v0, axis=1, keepdims=True)) @ up, -1, 1)).max()) * 1.04
    UV = layout(V, phimax)

    # per texel: the cap's surface point (rasterised in the layout), then the alpha there
    pos = np.zeros((res, res, 3))
    hit = np.zeros((res, res), bool)
    px = UV * res - 0.5
    for f in faces:
        for k in range(1, len(f) - 1):
            t = (f[0], f[k], f[k + 1])
            a, b, c = px[list(t)]
            lo, hi = np.floor(np.minimum(np.minimum(a, b), c)).astype(int), np.ceil(np.maximum(np.maximum(a, b), c)).astype(int)
            lo, hi = np.clip(lo, 0, res - 1), np.clip(hi, 0, res - 1)
            X, Y = np.meshgrid(np.arange(lo[0], hi[0] + 1), np.arange(lo[1], hi[1] + 1))
            den = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
            if abs(den) < 1e-12:
                continue
            l1 = ((b[1] - c[1]) * (X - c[0]) + (c[0] - b[0]) * (Y - c[1])) / den
            l2 = ((c[1] - a[1]) * (X - c[0]) + (a[0] - c[0]) * (Y - c[1])) / den
            l3 = 1 - l1 - l2
            ins = (l1 >= -0.02) & (l2 >= -0.02) & (l3 >= -0.02)
            if ins.any():
                P3 = l1[ins, None] * V[t[0]] + l2[ins, None] * V[t[1]] + l3[ins, None] * V[t[2]]
                pos[Y[ins], X[ins]] = P3
                hit[Y[ins], X[ins]] = True
    a_sheet = np.zeros((res, res))
    a_sheet[hit] = sheet_alpha(pos[hit])
    # root cover: the roots splatted in the same layout, blurred by "spread" mm, against the dense part's level
    mm_px = (2 * phimax * float(np.linalg.norm(V - ear_mid, axis=1).mean())) / res
    ruv = layout(R, phimax) * res - 0.5
    ix = np.clip(np.round(ruv).astype(int), 0, res - 1)
    cnt = np.zeros((res, res))
    np.add.at(cnt, (ix[:, 1], ix[:, 0]), 1.0)
    dens = gauss_blur(cnt, sigma / mm_px)
    ref = np.percentile(dens[dens > dens.max() * 0.05], 60)
    a_root = smooth01(dens / max(ref, 1e-9) * 1.5)
    alpha = np.where(hit, np.maximum(a_root, a_sheet), 0.0)
    # bleed the colour/alpha a few texels past the triangles (mip and filter safety): alpha 0 there anyway
    log("cap: %d faces, %d verts, offset %.2f mm, layout %.2f mm/px; roots %s; alpha > 0.5 on %.0f cm2 (roots %.0f, "
        "sheet line %.0f)" % (len(faces), len(V), offset / MM, mm_px / MM, frame,
                              (alpha > 0.5).sum() * (mm_px / MM) ** 2 / 100, (a_root > 0.5).sum() * (mm_px / MM) ** 2 / 100,
                              ((a_sheet > 0.5) & hit).sum() * (mm_px / MM) ** 2 / 100))

    # maps: hair colour with a slight mottle, the alpha, flat normal, rough non-metal
    rng = np.random.default_rng(int(opt.get("seed", 3)))
    col = np.array([float(c) for c in opt.get("colour", "0.05,0.04,0.035").split(",")])
    Yg, Xg = np.mgrid[0:res, 0:res].astype(float) * mm_px / MM
    mott = 1.0 + float(opt.get("mottle", 0.15)) * (fbm(Xg, Yg, 6.0, rng) - 0.5) * 2
    bc = np.clip(col[None, None, :] * mott[..., None], 0, 1)
    i_bc = save_png(os.path.join(out, name + "_BC.png"), bc, True)
    save_png(os.path.join(out, name + "_A.png"), np.repeat(alpha[..., None], 3, 2), False)
    flat = np.zeros((res, res, 3))
    flat[...] = (0.5, 0.5, 1.0)
    save_png(os.path.join(out, name + "_N.png"), flat, False)
    i_n = save_png(os.path.join(out, name + "_N_gl.png"), flat, False)
    orm = np.zeros((res, res, 3))
    orm[...] = (1.0, fopt(opt, "rough", 0.75), 0.0)
    i_orm = save_png(os.path.join(out, name + "_ORM.png"), orm, False)

    # a review sheet: front and side, the face's skin in grey, the cap's alpha in the hair colour, the line in red
    W = 600
    img = np.ones((W, 2 * W, 3))
    span = 0.26
    def plot(P, vals, colour, panel, ax):
        u = ((P - eye_mid) @ ax) / span + 0.5
        v = ((P - eye_mid) @ up) / span + 0.35
        x, y = (u * W).astype(int) + panel * W, (v * W).astype(int)
        ok = (x >= panel * W) & (x < (panel + 1) * W) & (y >= 0) & (y < W)
        img[y[ok], x[ok]] = img[y[ok], x[ok]] * (1 - vals[ok, None]) + np.asarray(colour) * vals[ok, None]
    sk = F[np.unique(np.concatenate([np.array(polys[k]) for k in np.nonzero(mi == skin)[0]]))]
    for panel, ax in ((0, left), (1, fwd)):
        plot(sk, np.full(len(sk), 0.35), (0.6, 0.6, 0.6), panel, ax)
        plot(pos[hit], alpha[hit] * 0.8, (0.1, 0.07, 0.05), panel, ax)
    xs = np.linspace(-hl[:, 0].max(), hl[:, 0].max(), 400)
    for sgnx in (1,):
        hp = eye_mid[None, :] + (xs[:, None] * left[None, :] + np.interp(np.abs(xs), hl[:, 0], hl[:, 1])[:, None]
                                 * up[None, :]) * ipd + fwd[None, :] * 0.12
        plot(hp, np.ones(len(hp)), (0.9, 0.1, 0.1), 0, left)
    save_png(os.path.join(out, name + "_review.png"), img, True)

    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(V.tolist(), [], [list(map(int, f)) for f in faces])
    mesh.update()
    uvl = mesh.uv_layers.new(name="UVMap")
    li = np.empty(len(mesh.loops), np.int64)
    mesh.loops.foreach_get("vertex_index", li)
    uvl.data.foreach_set("uv", UV[li].ravel())
    o = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(o)
    mesh.materials.append(preview_material("M_" + name, i_bc, i_n, i_orm))
    report = {"mode": "cap", "roots": roots_npz, "roots_frame": frame, "roots_off_skin_p50_mm": med / MM,
              "offset_mm": offset / MM, "feather_mm": feather / MM, "spread_mm": sigma / MM, "ipd_mm": ipd / MM,
              "hairline_ipd": hl.tolist(), "hairline_gap_mm": gap, "tex": res, "mm_per_px": mm_px / MM, "colour": col.tolist(),
              "alpha": name + "_A.png (linear; 1 = hair cover)"}
    skin_and_export(head, o, name, out, report)


def main():
    mode, paths, opt = parse_args()
    if mode == "glasses":
        body, face, gl, out = paths
    elif mode == "cap":
        body, face, roots, out = paths
    else:
        body, face, out = paths
    os.makedirs(out, exist_ok=True)
    head = load_head(body, face)
    if mode == "glasses":
        run_glasses(head, gl, out, opt)
    elif mode == "cap":
        run_cap(head, roots, out, opt)
    elif mode == "measure":
        run_measure(head, out, opt)
    elif mode == "transplant":
        run_transplant(head, out, opt)
    else:
        run_mask(head, out, opt)


main()
