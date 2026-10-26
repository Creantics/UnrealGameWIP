"""Rigs a generated head and neck bust (a Tripo-style mesh with its eyes, teeth, lashes and hair as loose parts) on
the canonical skeleton, headless, in two steps around face_transfer.py:

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/rig_head.py -- prep HEAD.glb --out DIR \\
        [--height 1.68] [--name NAME]
    blender -b DIR/skin.blend --factory-startup --python-exit-code 1 --python tools/blender/face_transfer.py -- \\
        --face DIR/face.json --out DIR/face.glb --blend DIR/face.blend [--preview DIR/face_preview]
    blender -b DIR/face.blend --factory-startup --python-exit-code 1 --python tools/blender/rig_head.py -- finish \\
        --parts DIR/parts.blend --out NAME.glb [--blend NAME.blend] [--preview DIR/rig_preview]

prep welds the glTF seam splits, separates the loose parts and sorts them:
  skin   the largest part (head, neck, the top of the chest); it gets the weights and, later, the face shapes;
  eyes   the biggest mirrored pair in front of the face between the mouth and the brow;
  mouth  parts on the centre line around the mouth (teeth, gums, tongue);
  face   other small parts in front of the face from the chin to just above the eyes (brows, lashes, lids, tear
         ducts): they follow the skin under them, face shapes included;
  hair   everything else (fixed to the head joint).
The bust is scaled so its crown-to-mouth height matches the canonical skeleton at --height (metres), and moved so its
mouth, eye line and centre sit where the skeleton has them. The eye joints go to the eyeball centres, the jaw joint
keeps its place relative to the eyes and mouth, and the neck joints move onto the neck's centre line. The skin is
weighted along spine_03, neck_01, neck_02 and head by height (the jaw weights come from face_transfer). The armature
gets the lead clips (idle, rom, walk) so the glb passes make_leads.check_glb like any lead. prep writes:
  skin.blend   the armature and the skinned skin alone, ready for face_transfer.py;
  parts.blend  the other parts, already in the rig's frame, named eye_l, eye_r, mouth*, face*, hair*;
  face.json    face_transfer's anchors, read off the mesh;
  parts.json   the sorting and the fit (what went where, the scale, the joint positions);
  prep/        a front and side render with the parts coloured by class and a dot on each head joint.
finish (run on face_transfer's .blend) first rebuilds what the transfer gets wrong on a scan: the lid shapes, made
afresh from the skin's own lid margins (procedural_lids), and the jaw weights, made to fall off smoothly over the
cheeks and the front of the neck (smooth_jaw). It then joins the parts back: each eye to its eye joint, mouth parts
to the head above the mouth line and to the jaw below it (a part three quarters on one side goes whole), face parts
with the weights and shape deltas of the nearest skin point, hair to the head. The scans are open-mouthed, so it
turns the jaw until the lips meet, makes that the rest mesh (close_mouth) and smooths the skin's move from open to
closed (relax_closing). The result is one mesh with the 56 face shapes; import it with FTImportCharacter -Face=1.
Its report line gives the closing angle and how many skin faces closing turned over outside the mouth (0 when clean).
--preview renders Basis, jawOpen, eyeBlinkLeft, eyeWideLeft, eyeSquintLeft, mouthSmileLeft, browInnerUp and the eyes
turned left, front and three-quarter.
"""

import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bmesh  # noqa: E402
import bpy  # noqa: E402
import numpy as np  # noqa: E402
from mathutils import Matrix, Vector  # noqa: E402
from mathutils.bvhtree import BVHTree  # noqa: E402

import make_leads as ml  # noqa: E402
import ne_blender as nb  # noqa: E402
import rig_common as rc  # noqa: E402
import rig_spec as rs  # noqa: E402

SKULL_TOP = 1.808  # crown height of the canonical skeleton (make_leads.SKULL_TOP)
NECK_CHAIN = ["spine_03", "neck_01", "neck_02", "head"]
CLASS_COLOURS = {"skin": (0.8, 0.62, 0.5, 1), "eye": (0.2, 0.5, 1, 1), "mouth": (1, 0.2, 0.2, 1),
                 "face": (0.2, 0.9, 0.3, 1), "hair": (0.95, 0.85, 0.2, 1)}


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not argv or argv[0] not in ("prep", "finish"):
        raise SystemExit(__doc__)
    a = {"step": argv[0], "in": None, "out": None, "height": "1.68", "name": "head", "parts": None, "blend": None,
         "preview": None}
    i = 1
    while i < len(argv):
        if argv[i].startswith("--"):
            k = argv[i][2:]
            if k not in a or i + 1 >= len(argv):
                raise SystemExit("rig_head: bad argument %s\n%s" % (argv[i], __doc__))
            a[k] = argv[i + 1]
            i += 2
        else:
            a["in"] = argv[i]
            i += 1
    if a["step"] == "prep" and not (a["in"] and a["out"]):
        raise SystemExit(__doc__)
    if a["step"] == "finish" and not (a["parts"] and a["out"]):
        raise SystemExit(__doc__)
    a["height"] = float(a["height"])
    return a


# ------------------------------------------------------------------------------------------------ prep


def _activate(obj):
    if bpy.context.object and bpy.context.object.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    bpy.ops.object.select_all(action="DESELECT")
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)


def import_bust(path):
    """The bust as one welded mesh, then split into its loose parts (largest first)."""
    nb.reset_scene()
    bpy.ops.import_scene.gltf(filepath=os.path.abspath(path))
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    for o in meshes:
        o.data.transform(o.matrix_world)
        o.parent = None
        o.matrix_world = Matrix()
    for o in [o for o in bpy.data.objects if o.type != "MESH"]:
        bpy.data.objects.remove(o, do_unlink=True)
    bpy.context.view_layer.update()
    _activate(meshes[0])
    for o in meshes[1:]:
        o.select_set(True)
    if len(meshes) > 1:
        bpy.ops.object.join()
    o = meshes[0]
    bm = bmesh.new()
    bm.from_mesh(o.data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
    bm.to_mesh(o.data)
    bm.free()
    _activate(o)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.separate(type="LOOSE")
    bpy.ops.object.mode_set(mode="OBJECT")
    parts = [p for p in bpy.data.objects if p.type == "MESH"]
    return sorted(parts, key=lambda p: -len(p.data.vertices))


def _verts(o):
    return np.array([v.co[:] for v in o.data.vertices])


def _stats(o):
    V = _verts(o)
    return {"n": len(V), "c": V.mean(axis=0), "min": V.min(axis=0), "max": V.max(axis=0), "size": np.ptp(V, axis=0)}


def boundary_loops(o):
    """Vertex index lists of the mesh's open boundary loops."""
    bm = bmesh.new()
    bm.from_mesh(o.data)
    left = {e for e in bm.edges if e.is_boundary}
    loops = []
    while left:
        e = left.pop()
        stack, verts = [e], set(v.index for v in e.verts)
        while stack:
            c = stack.pop()
            for v in c.verts:
                for f in v.link_edges:
                    if f in left:
                        left.remove(f)
                        stack.append(f)
                        verts.update(w.index for w in f.verts)
        loops.append(sorted(verts))
    bm.free()
    return loops


def mouth_from_pocket(skin, eye_z, height):
    """An open mouth whose inside is a closed pocket of the skin (no boundary loop), seen from the front: a notch, a
    height where a ray from the front goes deeper than both lips around it. Lips parted only a few millimetres at the
    centre still show one (the ray slips between them into the pocket); the gap under the chin doesn't, since the
    neck below it is further back still. Down the centre line the deepest notch is the opening (the creases under the
    nose and over the chin are shallow ones), and each lip edge is the biggest step forward in the profile from there;
    sideways the notch is followed as it curves until it fades at the corners.
    Returns four outline points, the corners and the upper and lower lip edges, or None."""
    SV = _verts(skin)
    bvh = BVHTree.FromPolygons([Vector(p) for p in SV], [tuple(p.vertices) for p in skin.data.polygons])
    step, thr = 0.002 * height, 0.01 * height
    far = SV[:, 1].max() + height  # a ray that goes through the opening

    def y_at(x, z):
        h = front_profile(bvh, x, z)
        return far if h is None else h[1]

    def notch(x, z, y=None):
        y = y_at(x, z) if y is None else y
        return max(y - max(y_at(x, z + d), y_at(x, z - d)) for d in np.array([0.01, 0.02, 0.04, 0.08]) * height)

    zs = np.arange(eye_z - 0.08 * height, eye_z - 0.45 * height, -step)
    yv = np.array([y_at(0.0, z) for z in zs])
    depth = np.array([notch(0.0, z, y) for z, y in zip(zs, yv)])
    runs, i = [], 0
    while i < len(zs):
        if depth[i] > thr:
            j = i
            while j + 1 < len(zs) and depth[j + 1] > thr:
                j += 1
            if j - i >= 2:
                runs.append((depth[i:j + 1].max(), i, j))
            i = j + 1
        else:
            i += 1
    if not runs:
        return None
    _d, i, j = max(runs)
    k = i + int(np.argmax(depth[i:j + 1]))
    z = zs[k]
    # the lip edges: the biggest step forward in the profile going up, and going down, from the deepest point
    reach = int(0.12 * height / step)
    up = np.arange(max(1, k - reach), k + 1)
    lo = np.arange(k, min(len(zs) - 1, k + reach))
    i = up[np.argmax(yv[up] - yv[up - 1])]
    j = lo[np.argmax(yv[lo] - yv[lo + 1])]
    z_up, z_lo = zs[i] + step / 2, zs[j] - step / 2
    win = np.arange(-10, 11) * step
    x = 0.0
    while x < 0.2 * height:
        cand = [(notch(x + step, z + dz), z + dz) for dz in win]
        best = max(cand)
        if best[0] <= thr:
            break
        x, z = x + step, best[1]
    y = min(y_at(0.0, z_up + step), y_at(0.0, z_lo - step))
    return np.array([[x, y, z], [-x, y, z], [0.0, y, z_up], [0.0, y, z_lo]])


def sort_parts(parts):
    skin = parts[0]
    S = {p.name: _stats(p) for p in parts}
    sk = S[skin.name]
    height = sk["size"][2]
    # eyes: the biggest mirrored pair in front of the face
    best = None
    rest = parts[1:]
    for i, p in enumerate(rest):
        a = S[p.name]
        if a["c"][0] <= 0.02 * height or a["n"] < 50:
            continue
        for q in rest:
            b = S[q.name]
            if q is p or b["c"][0] >= -0.02 * height:
                continue
            if (abs(a["c"][0] + b["c"][0]) < 0.01 * height and abs(a["c"][1] - b["c"][1]) < 0.01 * height
                    and abs(a["c"][2] - b["c"][2]) < 0.01 * height and a["c"][1] < sk["c"][1]):
                vol = float(np.prod(a["size"]))
                if best is None or vol > best[0]:
                    best = (vol, p, q)
    if best is None:
        raise SystemExit("rig_head: no mirrored eye pair among the loose parts")
    eye_l, eye_r = best[1], best[2]  # +x is the character's left (it faces -y)
    E = S[eye_l.name]
    eye_z, eye_w = E["c"][2], E["size"][0]
    # the mouth: the opening down the centre line. An open loop of the skin there is only a fallback: it can be just
    # the inner edge of one lip running back into the mouth, which misses the other lip
    ML = mouth_from_pocket(skin, eye_z, height)
    if ML is None:
        mouth_loop, mouth_c = None, None
        SV = _verts(skin)
        for loop in boundary_loops(skin):
            c = SV[loop].mean(axis=0)
            if abs(c[0]) < 0.03 * height and c[2] < eye_z - 0.5 * eye_w and c[1] < E["c"][1] + eye_w and len(loop) > 8:
                if mouth_c is None or c[2] > mouth_c[2]:
                    mouth_loop, mouth_c = loop, c
        if mouth_loop is None:
            raise SystemExit("rig_head: no open mouth found (close-mouthed heads go through mouth_cut.py)")
        ML = SV[mouth_loop]
    mouth_c = ML.mean(axis=0)
    face_top = eye_z + 0.7 * eye_w
    chin_z = mouth_c[2] - 2.0 * (eye_z - mouth_c[2])  # generous: only used to bound the face band
    kinds = {eye_l.name: "eye", eye_r.name: "eye"}
    for p in rest:
        if p.name in kinds:
            continue
        a = S[p.name]
        in_front = a["c"][1] < E["c"][1] + 1.5 * eye_w
        if abs(a["c"][0]) < 0.03 * height and mouth_c[2] - 2.5 * eye_w < a["c"][2] < mouth_c[2] + eye_w and in_front:
            kinds[p.name] = "mouth"
        elif (chin_z < a["c"][2] < face_top and in_front and abs(a["c"][0]) < abs(E["c"][0]) + eye_w
              and a["size"].max() < 2.0 * eye_w):
            kinds[p.name] = "face"
        else:
            kinds[p.name] = "hair"
    kinds[skin.name] = "skin"
    return skin, eye_l, eye_r, kinds, ML, mouth_c


def _ray(bvh, origin, direction):
    hit = bvh.ray_cast(Vector(origin), Vector(direction))
    return None if hit[0] is None else np.array(hit[0][:])


def front_profile(bvh, x, z):
    return _ray(bvh, (x, -5.0, z), (0, 1, 0))


def back_profile(bvh, x, z):
    return _ray(bvh, (x, 5.0, z), (0, -1, 0))


def chin_bottom(bvh, lip_z, step, back=0.015, reach=0.12):
    """Height of the under-chin edge: the chin's most forward point on the centre line below the lower lip, then down
    from it to where the front surface lies `back` metres behind it (the jaw's underside turning into the neck)."""
    zs = np.arange(lip_z - 2 * step, lip_z - reach, -step)
    hits = [front_profile(bvh, 0.0, z) for z in zs]
    ys = np.array([h[1] if h is not None else np.inf for h in hits])
    k = int(np.argmin(ys[: len(ys) // 2]))
    for z, y in zip(zs[k:], ys[k:]):
        if y - ys[k] > back:
            return float(z)
    raise SystemExit("rig_head: no chin found below the mouth")


def place(objs, skin, eye_l, eye_r, mouth_c, ML, s):
    """Scales and moves every part into the skeleton's frame; returns the transform applied."""
    SV = _verts(skin)
    crown = SV[:, 2].max()
    eye_mid = (_stats(eye_l)["c"] + _stats(eye_r)["c"]) / 2
    k = (SKULL_TOP - rs.MOUTH_Z) * s / (crown - mouth_c[2])
    t = Vector((-eye_mid[0] * k, rs.EYE_Y * s - eye_mid[1] * k, rs.MOUTH_Z * s - mouth_c[2] * k))
    M = Matrix.Translation(t) @ Matrix.Scale(k, 4)
    for o in objs:
        o.data.transform(M)
        o.data.update()
    return M, k


def eye_centre(o):
    """An eyeball's centre: the middle of a round part, or half its width behind the front of a flat one."""
    st = _stats(o)
    r = st["size"][0] / 2
    return np.array([st["c"][0], st["min"][1] + r, st["c"][2]]) if st["size"][1] < 1.5 * r else st["c"]


def fit_armature(name, s, skin, eye_l, eye_r, mouth_z):
    rows = [(r[0], r[1], tuple(s * c for c in r[2]), tuple(s * c for c in r[3])) + tuple(r[4:]) for r in rs.JOINTS]
    arm = rc.build_canonical_armature(name + "_rig", rows=rows)
    SV = _verts(skin)
    bvh = BVHTree.FromPolygons([Vector(p) for p in SV], [tuple(p.vertices) for p in skin.data.polygons])
    _activate(arm)
    bpy.ops.object.mode_set(mode="EDIT")
    eb = arm.data.edit_bones
    moved = {}
    for side, o in (("l", eye_l), ("r", eye_r)):
        c = eye_centre(o)
        b = eb["eye_" + side]
        b.head = Vector(c)
        b.tail = Vector(c) + Vector((0, -0.025 * s, 0))
        moved["eye_" + side] = [round(x, 4) for x in c]
    for n in ("neck_01", "neck_02", "head"):  # onto the neck's centre line, x = 0
        b = eb[n]
        for end in ("head", "tail"):
            p = getattr(b, end)
            f, k = front_profile(bvh, 0.0, p.z), back_profile(bvh, 0.0, p.z)
            if f is not None and k is not None:
                setattr(b, end, Vector((0.0, (f[1] + k[1]) / 2, p.z)))
        moved[n] = [round(x, 4) for x in b.head]
    for n in ("eye_l", "eye_r", "neck_01", "neck_02", "head"):  # the roll again, as build_canonical_armature sets it
        b = eb[n]
        y = (b.tail - b.head).normalized()
        x = Vector((1.0, 0.0, 0.0))
        b.align_roll((x - y * x.dot(y)).normalized().cross(y))
    bpy.ops.object.mode_set(mode="OBJECT")
    return arm, moved


def neck_weights(skin, arm):
    """Weights along spine_03 -> neck_01 -> neck_02 -> head by height (hat functions on knots between the joints)."""
    z = {n: arm.data.bones[n].head_local.z for n in NECK_CHAIN[1:]}
    top = arm.data.bones["head"].head_local.z
    knots = [z["neck_01"] - (z["neck_02"] - z["neck_01"]), (z["neck_01"] + z["neck_02"]) / 2,
             (z["neck_02"] + top) / 2, top + 0.6 * (top - z["neck_02"])]
    V = _verts(skin)
    skin.vertex_groups.clear()
    groups = {n: skin.vertex_groups.new(name=n) for n in NECK_CHAIN}
    for i, vz in enumerate(V[:, 2]):
        if vz <= knots[0]:
            w = {NECK_CHAIN[0]: 1.0}
        elif vz >= knots[-1]:
            w = {NECK_CHAIN[-1]: 1.0}
        else:
            j = max(k for k in range(3) if knots[k] <= vz)
            t = (vz - knots[j]) / (knots[j + 1] - knots[j])
            t = t * t * (3 - 2 * t)
            w = {NECK_CHAIN[j]: 1.0 - t, NECK_CHAIN[j + 1]: t}
        for n, x in w.items():
            if x > 1e-4:
                groups[n].add([i], x, "REPLACE")
    skin.parent = arm
    mod = skin.modifiers.new("Armature", "ARMATURE")
    mod.object = arm
    return [round(k, 4) for k in knots]


def face_anchors(skin, eye_l, mouth_c, ML, s):
    """face_transfer's anchors (its ANCHORS table), read off the placed mesh, in metres."""
    SV = _verts(skin)
    bvh = BVHTree.FromPolygons([Vector(p) for p in SV], [tuple(p.vertices) for p in skin.data.polygons])
    eye = eye_centre(eye_l)
    kz = (eye[2] - mouth_c[2]) / (rs.EYE_Z - rs.MOUTH_Z)  # this face's vertical scale against the proof's
    corner = ML[np.argmax(ML[:, 0])]
    mid = ML[np.abs(ML[:, 0]) < 0.25 * corner[0]]
    z_up, z_lo = mid[:, 2].max(), mid[:, 2].min()
    nose_sel = (np.abs(SV[:, 0]) < 0.004) & (SV[:, 2] > mouth_c[2] + 0.01 * kz) & (SV[:, 2] < eye[2])
    nose = SV[nose_sel][np.argmin(SV[nose_sel][:, 1])]
    cb = chin_bottom(bvh, z_lo, 0.0015)
    crown = SV[np.argmax(SV[:, 2])]
    f_eye, b_eye = front_profile(bvh, 0.0, eye[2]), back_profile(bvh, 0.0, eye[2])
    centre_y = (f_eye[1] + b_eye[1]) / 2
    f_neck, b_neck = front_profile(bvh, 0.0, cb - 0.005), back_profile(bvh, 0.0, cb - 0.005)
    neck_y = (f_neck[1] + b_neck[1]) / 2 if f_neck is not None and b_neck is not None else centre_y
    face = {
        "eye_l": [round(float(x), 5) for x in eye],
        "mouth_l": [round(float(corner[0]), 5), round(float(corner[2]), 5)],
        "upper_lip": [0.0, round(float(z_up + 0.004 * kz), 5)],
        "lower_lip": [0.0, round(float(z_lo - 0.005 * kz), 5)],
        "nose_tip": [round(float(x), 5) for x in nose],
        "chin": [0.0, round(float(cb + 0.3 * (z_lo - cb)), 5)],
        "brow": [0.0, round(float(eye[2] + 0.04 * kz), 5)],
        "brow_l": [round(float(eye[0]), 5), round(float(eye[2] + 0.03 * kz), 5)],
        "crown": [round(float(x), 5) for x in crown],
        "back": [0.0, round(float(eye[2]), 5)],
        "side_l": [round(float(centre_y), 5), round(float(eye[2] + 0.04 * kz), 5)],
        "neck": [0.0, round(float(neck_y), 5), round(float(cb - 0.005), 5)],
    }
    return face, float(cb)


def joint_dots(arm, names, r):
    dots = []
    for n in names:
        bpy.ops.mesh.primitive_uv_sphere_add(radius=r, location=arm.data.bones[n].head_local)
        d = bpy.context.active_object
        d.color = (1, 0, 1, 1)
        dots.append(d)
    return dots


def render_views(objs, outdir, views, colour_by=None, size=768, focus=None):
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_WORKBENCH"
    sc.display.shading.light = "STUDIO"
    sc.display.shading.color_type = "OBJECT" if colour_by else "TEXTURE"
    sc.display.shading.show_xray = False
    sc.render.resolution_x = sc.render.resolution_y = size
    if colour_by:
        for o, c in colour_by.items():
            o.color = c
    pts = [o.matrix_world @ Vector(c) for o in objs for c in o.bound_box]
    mn = Vector([min(p[i] for p in pts) for i in range(3)])
    mx = Vector([max(p[i] for p in pts) for i in range(3)])
    ctr = focus if focus is not None else (mn + mx) / 2
    span = max(mx - mn) * (1.1 if focus is None else 0.6)
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    sc.collection.objects.link(cam)
    sc.camera = cam
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = span
    os.makedirs(outdir, exist_ok=True)
    for tag, yaw in views:
        d = Vector((math.sin(math.radians(yaw)), -math.cos(math.radians(yaw)), 0))
        cam.location = ctr + d * 3.0
        cam.rotation_euler = (ctr - cam.location).to_track_quat("-Z", "Y").to_euler()
        sc.render.filepath = os.path.join(os.path.abspath(outdir), tag + ".png")
        bpy.ops.render.render(write_still=True)
    bpy.data.objects.remove(cam, do_unlink=True)


def prep(a):
    out = os.path.abspath(a["out"])
    os.makedirs(out, exist_ok=True)
    parts = import_bust(a["in"])
    skin, eye_l, eye_r, kinds, ML, mouth_c = sort_parts(parts)
    s = a["height"] / SKULL_TOP
    M, k = place(parts, skin, eye_l, eye_r, mouth_c, ML, s)
    ML = np.array([M @ Vector(p) for p in ML])
    mouth_c = np.array(M @ Vector(mouth_c))
    arm, joints = fit_armature(a["name"], s, skin, eye_l, eye_r, mouth_c[2])
    knots = neck_weights(skin, arm)
    face, cb = face_anchors(skin, eye_l, mouth_c, ML, s)
    # the lead clips, as rig_scan gives them
    joints_body = rc.body_joints(arm)
    rc.make_rom_action(arm, joints_body)
    ml.make_idle(arm, joints_body, -30.0)
    ml.make_walk(arm, joints_body, -32.0)
    arm.animation_data.action = None
    rc.reset_pose(arm)
    bpy.context.scene.frame_set(0)
    # names
    skin.name = skin.data.name = a["name"]
    eye_l.name, eye_r.name = "eye_l", "eye_r"
    counts = {}
    for p in parts:
        kd = kinds.get(p.name)
        if kd in ("mouth", "face", "hair"):
            counts[kd] = counts.get(kd, 0) + 1
            p.name = "%s_%02d" % (kd, counts[kd])
    kinds = {p.name: ("skin" if p is skin else "eye" if p in (eye_l, eye_r) else p.name.split("_")[0]) for p in parts}
    report = {"input": os.path.abspath(a["in"]), "height": a["height"], "bust_scale": round(k, 5),
              "mouth_z": round(float(mouth_c[2]), 5), "chin_bottom": round(cb, 5), "joints": joints,
              "neck_knots": knots,
              "parts": {p.name: {"kind": kinds[p.name], "verts": len(p.data.vertices)} for p in parts}}
    with open(os.path.join(out, "face.json"), "w") as f:
        json.dump(face, f, indent=1)
    with open(os.path.join(out, "parts.json"), "w") as f:
        json.dump(report, f, indent=1)
    # preview: parts by class, head joints as dots
    dots = joint_dots(arm, ["neck_01", "neck_02", "head", "jaw", "eye_l", "eye_r"], 0.004)
    render_views(parts + dots, os.path.join(out, "prep"), [("front", 0), ("side", 90)],
                 colour_by={p: CLASS_COLOURS[kinds[p.name]] for p in parts})
    for d in dots:
        bpy.data.objects.remove(d, do_unlink=True)
    # parts.blend: everything but the skin and the armature
    others = [p for p in parts if p is not skin]
    bpy.data.libraries.write(os.path.join(out, "parts.blend"), set(others), fake_user=True)
    for p in others:
        bpy.data.objects.remove(p, do_unlink=True)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "skin.blend"))
    print("rig_head prep: scale %.4f, mouth z %.4f, parts %s -> %s" % (
        k, mouth_c[2], {kd: sum(1 for v in kinds.values() if v == kd) for kd in CLASS_COLOURS}, out))


# ------------------------------------------------------------------------------------------------ finish


def bary(p, a, b, c):
    """Barycentric coordinates of p's projection on triangle abc, clipped to the triangle (as shell_bind)."""
    v0, v1, v2 = b - a, c - a, p - a
    d00, d01, d11, d20, d21 = v0.dot(v0), v0.dot(v1), v1.dot(v1), v2.dot(v0), v2.dot(v1)
    den = d00 * d11 - d01 * d01 or 1e-20
    v = (d11 * d20 - d01 * d21) / den
    w = (d00 * d21 - d01 * d20) / den
    w3 = np.clip([1.0 - v - w, v, w], 0.0, None)
    return w3 / (w3.sum() or 1.0)


def skin_sampler(mesh):
    me = mesh.data
    P = _verts(mesh)
    me.calc_loop_triangles()
    tris = [t.vertices[:] for t in me.loop_triangles]
    tree = BVHTree.FromPolygons([Vector(p) for p in P], tris)
    groups = [g.name for g in mesh.vertex_groups]
    W = np.zeros((len(P), len(groups)))
    for v in me.vertices:
        for ge in v.groups:
            W[v.index, ge.group] = ge.weight
    keys = me.shape_keys.key_blocks
    basis = np.array([d.co[:] for d in keys[0].data])
    D = {kb.name: np.array([d.co[:] for d in kb.data]) - basis for kb in keys[1:]}

    def sample(V):
        corner, wt = np.zeros((len(V), 3), int), np.zeros((len(V), 3))
        for i, p in enumerate(V):
            loc, _n, ti, _d = tree.find_nearest(Vector(p))
            t = tris[ti]
            corner[i] = t
            wt[i] = bary(np.array(loc), P[t[0]], P[t[1]], P[t[2]])
        Wv = np.einsum("nk,nkg->ng", wt, W[corner])
        Dv = {n: np.einsum("nk,nkc->nc", wt, d[corner]) for n, d in D.items()}
        return Wv, groups, Dv

    return sample


def _smooth01(t):
    t = np.clip(t, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


# Each eye shape as (upper lid, lower lid) travel, as a share of the open angle between the lid margins: + closes.
# The two may add up to 1 (a blink: the lids meet on one line) but no more: the skin inside the opening (where each
# margin wraps in toward the eyeball) turns by a blend of the two, and past 1 that blend turns it inside out. A
# closing upper lid also rides out over the lower one, so the shut lids don't sit through each other.
LID_SHAPES = {"eyeBlink": (0.95, 0.05), "eyeSquint": (0.1, 0.35), "eyeWide": (-0.35, -0.05),
              "eyeLookUp": (-0.2, -0.1), "eyeLookDown": (0.35, -0.1), "eyeLookIn": (0.0, 0.0), "eyeLookOut": (0.0, 0.0)}


def lid_aperture(skin_bvh, eye_bvh, c, r):
    """The eye opening seen from the front: for columns of x across the eye, the heights where a ray from the front
    meets the eyeball before the skin. Returns x samples with the upper and lower margin as angles about the eye
    centre c (0 straight ahead, + up), or None if the eye shows nowhere."""
    xs, up, lo = [], [], []
    for x in np.linspace(c[0] - 1.6 * r, c[0] + 1.6 * r, 49):
        open_at = []
        for z in np.linspace(c[2] - 1.4 * r, c[2] + 1.4 * r, 113):
            e = _ray(eye_bvh, (x, -5.0, z), (0, 1, 0))
            if e is None:
                continue
            k = _ray(skin_bvh, (x, -5.0, z), (0, 1, 0))
            if k is None or e[1] < k[1] + 1e-4:
                open_at.append((z, e[1]))
        if len(open_at) >= 2:
            (z0, y0), (z1, y1) = open_at[0], open_at[-1]
            xs.append(x)
            lo.append(math.atan2(z0 - c[2], c[1] - y0))
            up.append(math.atan2(z1 - c[2], c[1] - y1))
    if len(xs) < 6:
        return None
    xs = np.array(xs)
    # Ray steps make the margins ragged column to column; a smooth curve across the eye closes without zigzags.
    fit = lambda m: np.polyval(np.polyfit(xs, m, 4), xs)  # noqa: E731
    return xs, fit(np.array(up)), fit(np.array(lo))


def procedural_lids(mesh, arm, eyes):
    """Rebuilds the lid shapes (LID_SHAPES, both sides) on the skin from its own lid margins. The transferred ones
    crumple on a coarse lid rim, since neighbouring rim points take deltas from different layers of the proof lid.
    Lid skin turns about the eye centre's x axis, so it slides over the eyeball: the upper lid (above the opening)
    and the lower lid (below it) each by their share of the open angle at that x, full near the margin and fading out
    toward the brow, the cheek and past the eye corners. Within about two eye radii the transferred deltas of these
    shapes are replaced; outside they are kept."""
    keys = mesh.data.shape_keys.key_blocks
    P = np.array([d.co[:] for d in keys[0].data])
    me = mesh.data
    me.calc_loop_triangles()
    skin_bvh = BVHTree.FromPolygons([Vector(p) for p in P], [t.vertices[:] for t in me.loop_triangles])
    done = {}
    for side, o in eyes.items():
        c = np.array(arm.matrix_world @ arm.data.bones["eye_" + side].head_local)
        V = _verts(o)
        r = max(np.ptp(V[:, 0]), np.ptp(V[:, 2])) / 2
        o.data.calc_loop_triangles()
        eye_bvh = BVHTree.FromPolygons([Vector(v) for v in V], [t.vertices[:] for t in o.data.loop_triangles])
        ap = lid_aperture(skin_bvh, eye_bvh, c, r)
        if ap is None:
            print("rig_head: no lid opening found for eye_%s, its lid shapes stay as transferred" % side)
            continue
        xs, up, lo = ap
        hw = (xs[-1] - xs[0]) / 2
        xc = (xs[-1] + xs[0]) / 2
        u, v = c[1] - P[:, 1], P[:, 2] - c[2]
        phi = np.arctan2(v, u)
        phi_u = np.interp(P[:, 0], xs, up)
        phi_l = np.interp(P[:, 0], xs, lo)
        span = np.maximum(phi_u - phi_l, 0.0)
        t = np.clip((phi - phi_l) / np.maximum(span, 1e-6), 0.0, 1.0)
        across = 1.0 - _smooth01((np.abs(P[:, 0] - xc) - hw) / (0.6 * r))
        near = (u > -0.5 * r) & (np.hypot(u, v) < 2.2 * r)
        # Only skin lying on the eyeball turns; a brow overhanging at the same angle sits much further out.
        on_ball = 1.0 - _smooth01((np.hypot(u, v) - 1.3 * r) / (0.5 * r))
        d = np.hypot((P[:, 0] - xc) / max(hw, r), v / r)
        mask = (1.0 - _smooth01((d - 1.5) / 0.8)) * near
        for base, (k_up, k_lo) in LID_SHAPES.items():
            name = base + ("Left" if side == "l" else "Right")
            if name not in keys:
                continue
            # Past the margin the turn fades out. A fade steeper than the angle it crosses would turn skin over (the
            # skin just past the margin overtaking the skin beyond it): with smoothstep's slope of 1.5 that needs a
            # fade at least 1.5 times the turn at the margin wide, so a blink (most of the opening) fades over about
            # 70 degrees and a squint's lower lid over the minimum.
            w_up = np.maximum(0.35, 1.6 * abs(k_up) * span)
            w_lo = np.maximum(0.45, 1.6 * abs(k_lo) * span)
            f_up = 1.0 - _smooth01((phi - phi_u) / w_up)
            f_lo = 1.0 - _smooth01((phi_l - phi - 0.15) / w_lo)
            turn = -span * (k_up * t * f_up - k_lo * (1.0 - t) * f_lo) * across * on_ball * near
            cu, sn = np.cos(turn), np.sin(turn)
            u2, v2 = u * cu - v * sn, u * sn + v * cu
            # The upper lid ends in front of the lower: one closed line instead of two rims through each other.
            rho = np.maximum(np.hypot(u, v), 1e-6)
            out = 1.0 + 0.12 * r * max(k_up, 0.0) * t * f_up * across * on_ball * near / rho
            u2, v2 = u2 * out, v2 * out
            proc = np.zeros_like(P)
            proc[:, 1] = (c[1] - u2) - P[:, 1]
            proc[:, 2] = (c[2] + v2) - P[:, 2]
            kb = keys[name]
            co = np.array([dd.co[:] for dd in kb.data])
            delta = co - P
            delta = delta * (1.0 - mask)[:, None] + proc * mask[:, None]
            kb.data.foreach_set("co", (P + delta).astype(np.float32).ravel())
        done[side] = (round(float(r), 4), round(math.degrees(float(np.max(up - lo))), 1), round(math.degrees(float(np.max(up))), 1),
                      int((mask > 0).sum()))
    return done


def _lip_ellipse(P, face, report):
    """The lips and the mouth behind them: an ellipse in front view between the lip corners, in front of the neck."""
    mz, cw = report["mouth_z"], abs(face["mouth_l"][0])
    return ((P[:, 0] / max(cw - 0.002, 0.005)) ** 2 + ((P[:, 2] - mz) / 0.014) ** 2 < 1.0) & (P[:, 1] < face["neck"][1])


def smooth_jaw(mesh, face, report, iters=8000):
    """Rebuilds the skin's jaw weights to fall off smoothly. The transferred ones are a band (chin and jaw line at 1,
    the rest 0) with hard edges on the cheeks and under the chin, which tear once the jaw turns. Kept: the transferred
    weights between the lip corners (the split between the lips stays exact) and the full-weight band; fixed at 0: the
    face above the mouth (from 4 mm up at the corners, easing to 3 cm up past them), the last centimetre of the
    bust's cut edge and everything behind the neck's centre line; the band is held at 1 only from 1.5 cm below the
    mouth. The rest is filled in by diffusion over the mesh edges (a harmonic fill), so the cheeks and the whole
    front of the neck ease between them: closing the jaw moves the chin about 2 cm, which folds the skin when the
    falloff is shorter than a few centimetres. The other groups are scaled to fill what the jaw leaves."""
    me = mesh.data
    P = _verts(mesh)
    n = len(P)
    names = [g.name for g in mesh.vertex_groups]
    W = np.zeros((n, len(names)))
    for v in me.vertices:
        for ge in v.groups:
            W[v.index, ge.group] = ge.weight
    j = names.index("jaw")
    w = W[:, j].copy()
    mz, cw = report["mouth_z"], abs(face["mouth_l"][0])
    neck_y = face["neck"][1]
    lips = _lip_ellipse(P, face, report)
    # Beside the mouth the cheeks ease in from about the nose tip down, so the cheek takes the chin's travel over a
    # few centimetres instead of folding along a line from the mouth corner to the jaw angle.
    top = mz + 0.004 + 0.026 * _smooth01((np.abs(P[:, 0]) - cw) / 0.02)
    bottom = P[:, 2].min() + 0.01  # the bust's cut edge, left still for the body graft
    zero = ((P[:, 2] > top) & ~lips) | (P[:, 2] < bottom) | (P[:, 1] > neck_y)
    one = (w > 0.99) & (P[:, 2] < mz - 0.015) & ~zero & ~lips
    fixed = lips | zero | one
    w[zero & ~lips] = 0.0
    w[one] = 1.0
    E = np.array([e.vertices[:] for e in me.edges])
    deg = np.bincount(E[:, 0], minlength=n) + np.bincount(E[:, 1], minlength=n)
    free = ~fixed & (deg > 0)
    for _ in range(iters):
        avg = (np.bincount(E[:, 0], w[E[:, 1]], n) + np.bincount(E[:, 1], w[E[:, 0]], n)) / np.maximum(deg, 1)
        w[free] = avg[free]
    rest = np.delete(np.arange(len(names)), j)
    other = W[:, rest].sum(axis=1)
    scale = np.where(other > 1e-6, (1.0 - w) / np.maximum(other, 1e-6), 0.0)
    W[:, rest] *= scale[:, None]
    lone = (other <= 1e-6) & (w < 1.0)  # skin the transfer gave the jaw alone: the remainder goes to the head
    W[lone, names.index("head")] = 1.0 - w[lone]
    W[:, j] = w
    for gi, g in enumerate(mesh.vertex_groups):
        on = np.nonzero(W[:, gi] > 1e-4)[0]
        off = np.nonzero(W[:, gi] <= 1e-4)[0]
        if len(off):
            g.remove([int(i) for i in off])
        for i in on:
            g.add([int(i)], float(W[i, gi]), "REPLACE")
    return int((w > 1e-3).sum())


def jaw_open_sign(arm, mesh):
    """+1 if turning the jaw bone about +x opens the mouth (the chin drops), else -1."""
    gi = mesh.vertex_groups["jaw"].index
    chin = max(mesh.data.vertices, key=lambda v: sum(g.weight for g in v.groups if g.group == gi)).index
    pb = arm.pose.bones["jaw"]
    pb.rotation_mode = "XYZ"
    pb.rotation_euler = (math.radians(20), 0.0, 0.0)
    bpy.context.view_layer.update()
    sign = 1.0 if rc.evaluated_positions(mesh)[chin][2] < mesh.data.vertices[chin].co.z else -1.0
    pb.rotation_euler = (0.0, 0.0, 0.0)
    bpy.context.view_layer.update()
    return sign


def close_mouth(arm, mesh, face, n_skin, max_deg=30.0):
    """Bakes the mouth shut: the scans are made open-mouthed, but the rest pose (and jawOpen at 0) is closed. The jaw
    turns closed until the lower lip (face.json's lower_lip on the centre line) comes within 1 mm of the upper lip, at
    most max_deg; the posed mesh, each face shape included, then becomes the rest mesh. The jaw joint keeps its
    canonical rest. Returns the angle in degrees and the lip gap left (m)."""
    P = _verts(mesh)[:n_skin]

    def lip(z):
        near = np.nonzero((np.abs(P[:, 0]) < 0.004) & (np.abs(P[:, 2] - z) < 0.002))[0]
        return int(near[np.argmin(P[near, 1])])

    up, lo = lip(face["upper_lip"][1]), lip(face["lower_lip"][1])
    action = arm.animation_data.action if arm.animation_data else None
    if action:
        arm.animation_data.action = None
    rc.reset_pose(arm)
    sign = jaw_open_sign(arm, mesh)
    pb = arm.pose.bones["jaw"]
    pb.rotation_mode = "XYZ"

    def gap(deg):
        pb.rotation_euler = (math.radians(-sign * deg), 0.0, 0.0)
        bpy.context.view_layer.update()
        E = rc.evaluated_positions(mesh)
        return E[up].z - E[lo].z

    a, b = 0.0, max_deg
    if gap(b) > 0.001:
        a = b
    else:
        for _ in range(24):
            m = (a + b) / 2
            if gap(m) > 0.001:
                a = m
            else:
                b = m
    deg = a if a == max_deg else b
    left = gap(deg)
    keys = mesh.data.shape_keys.key_blocks
    inv = mesh.matrix_world.inverted()
    for kb in keys:
        kb.value = 0.0
    posed = []
    for kb in keys:
        if kb != keys[0]:
            kb.value = 1.0
        bpy.context.view_layer.update()
        posed.append(np.array([(inv @ c)[:] for c in rc.evaluated_positions(mesh)], dtype=np.float32))
        kb.value = 0.0
    for kb, co in zip(keys, posed):
        kb.data.foreach_set("co", co.ravel())
    mesh.data.vertices.foreach_set("co", posed[0].ravel())
    rc.reset_pose(arm)
    if action:
        arm.animation_data.action = action
    bpy.context.view_layer.update()
    return round(deg, 2), round(float(left), 4)


def relax_closing(mesh, n_skin, P_open, face, report, iters=30):
    """Smooths how closing the mouth moved the skin. Skinning leaves kinks where the jaw's pull changes fast: along the
    jaw line, on the cheek and at the mouth corners, where the lips (about 10 cm from the jaw joint) part by a single
    ring of vertices. The move from the open scan to the closed one is blurred over the mesh edges (about 1.5 cm) and
    the difference added to the rest mesh and every face shape alike, so the shapes' deltas are unchanged. The middle of
    the lips is held, so they still meet. Returns the largest correction (m)."""
    P = _verts(mesh)
    D = P[:n_skin] - P_open
    mz, cw = report["mouth_z"], abs(face["mouth_l"][0])
    held = _lip_ellipse(P_open, face, report) & (np.abs(P_open[:, 0]) < 0.6 * cw)
    E = np.array([e.vertices[:] for e in mesh.data.edges])
    E = E[(E < n_skin).all(axis=1)]
    deg = np.bincount(E[:, 0], minlength=n_skin) + np.bincount(E[:, 1], minlength=n_skin)
    free = ~held & (deg > 0)
    S = D.copy()
    for _ in range(iters):
        avg = np.stack([np.bincount(E[:, 0], S[E[:, 1], k], n_skin) + np.bincount(E[:, 1], S[E[:, 0], k], n_skin)
                        for k in range(3)], axis=1) / np.maximum(deg, 1)[:, None]
        S[free] = avg[free]
    C = S - D
    for kb in mesh.data.shape_keys.key_blocks:
        co = np.zeros(len(P) * 3, dtype=np.float32)
        kb.data.foreach_get("co", co)
        co = co.reshape(-1, 3)
        co[:n_skin] += C
        kb.data.foreach_set("co", co.ravel())
    co = P.astype(np.float32)
    co[:n_skin] += C
    mesh.data.vertices.foreach_set("co", co.ravel())
    return round(float(np.linalg.norm(C, axis=1).max()), 4)


def count_folds(mesh, n_skin, P_open, face, report):
    """Skin faces that closing the mouth turned over (normal more than 90 degrees from the open scan's), as (outside
    the mouth, on the lips and inside the mouth). The mouth here reaches 1.5 cm past the corners and 2 cm above and
    below its line. Some inside it are expected; outside there should be none."""
    P = _verts(mesh)
    mz, cw = report["mouth_z"], abs(face["mouth_l"][0])
    lips = ((P_open[:, 0] / (cw + 0.015)) ** 2 + ((P_open[:, 2] - mz) / 0.02) ** 2 < 1.0) & (P_open[:, 1] < face["neck"][1])
    out = [0, 0]
    for poly in mesh.data.polygons:
        vs = list(poly.vertices)
        if max(vs) >= n_skin:
            continue
        a, b, c = vs[:3]
        if np.cross(P_open[b] - P_open[a], P_open[c] - P_open[a]) @ np.cross(P[b] - P[a], P[c] - P[a]) < 0:
            out[int(lips[vs].any())] += 1
    return tuple(out)


def finish(a):
    arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
    mesh = next(o for o in bpy.data.objects if o.type == "MESH" and any(m.type == "ARMATURE" for m in o.modifiers))
    if not mesh.data.shape_keys:
        raise SystemExit("rig_head finish: run it on face_transfer.py's .blend (the skin has no face shapes)")
    rc.reset_pose(arm)
    bpy.context.view_layer.update()
    with open(os.path.join(os.path.dirname(os.path.abspath(a["parts"])), "parts.json")) as f:
        report = json.load(f)
    mouth_z = report["mouth_z"]
    with open(os.path.join(os.path.dirname(os.path.abspath(a["parts"])), "face.json")) as f:
        face = json.load(f)
    n_skin = len(mesh.data.vertices)
    with bpy.data.libraries.load(os.path.abspath(a["parts"])) as (src, dst):
        dst.objects = list(src.objects)
    lids = procedural_lids(mesh, arm, {o.name[-1]: o for o in dst.objects if o.name in ("eye_l", "eye_r")})
    jaw_verts = smooth_jaw(mesh, face, report)
    sample = skin_sampler(mesh)
    added = {}
    for o in sorted(dst.objects, key=lambda o: o.name):
        bpy.context.scene.collection.objects.link(o)
        o.parent = None
        o.matrix_world = Matrix()
        o.vertex_groups.clear()
        V = _verts(o)
        name = o.name
        kind = name.split("_")[0]
        deltas = None
        if kind == "eye":
            o.vertex_groups.new(name=o.name).add(range(len(V)), 1.0, "REPLACE")
        elif kind == "mouth":
            below = V[:, 2] < mouth_z
            share = below.mean()
            if share > 0.75 or share < 0.25:
                below[:] = share > 0.5
            if below.any():
                o.vertex_groups.new(name="jaw").add([int(i) for i in np.nonzero(below)[0]], 1.0, "REPLACE")
            if (~below).any():
                o.vertex_groups.new(name="head").add([int(i) for i in np.nonzero(~below)[0]], 1.0, "REPLACE")
            kind = "mouth (jaw %d, head %d)" % (int(below.sum()), int((~below).sum()))
        elif kind == "face":
            Wv, groups, deltas = sample(V)
            for gi, group in enumerate(groups):
                idx = np.nonzero(Wv[:, gi] > 1e-4)[0]
                if len(idx):
                    vg = o.vertex_groups.new(name=group)
                    for i in idx:
                        vg.add([int(i)], float(Wv[i, gi]), "REPLACE")
        else:
            o.vertex_groups.new(name="head").add(range(len(V)), 1.0, "REPLACE")
        start = len(mesh.data.vertices)
        bpy.ops.object.select_all(action="DESELECT")
        _activate(mesh)
        o.select_set(True)
        bpy.ops.object.join()
        if deltas:
            for kb in mesh.data.shape_keys.key_blocks[1:]:
                co = np.array([d.co[:] for d in kb.data])
                co[start:start + len(V)] += deltas[kb.name]
                kb.data.foreach_set("co", co.astype(np.float32).ravel())
        added[name] = (len(V), kind)
    P_open = _verts(mesh)[:n_skin]
    closed = close_mouth(arm, mesh, face, n_skin)
    relaxed = relax_closing(mesh, n_skin, P_open, face, report)
    folds = count_folds(mesh, n_skin, P_open, face, report)
    for kb in mesh.data.shape_keys.key_blocks:
        kb.value = 0.0
    rc.reset_pose(arm)
    bpy.context.scene.frame_set(0)
    bpy.context.view_layer.update()
    out = os.path.abspath(a["out"])
    os.makedirs(os.path.dirname(out), exist_ok=True)
    glb = nb.export_rig(out, {"export_morph_normal": False, "export_force_sampling": False})
    rc.write_rigexpect(arm, mesh, os.path.splitext(out)[0] + ".rigexpect.json")
    info = ml.check_glb(glb, face=True)
    if a["blend"]:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(a["blend"]))
    if a["preview"]:
        preview(arm, mesh, a["preview"])
    print("rig_head finish: %s, mouth closed by (deg, lip gap m) %s, relaxed by up to %s m,"
          " folds (outside the mouth, in it) %s,"
          " lids (eye r, open deg, upper margin deg, verts) %s, jaw-weighted %d, parts %s, %d bytes -> %s"
          % (info, closed, relaxed, folds, lids, jaw_verts, added, os.path.getsize(glb), glb))


def preview(arm, mesh, outdir):
    arm.animation_data.action = None
    keys = mesh.data.shape_keys.key_blocks
    eye_l = arm.data.bones["eye_l"].head_local
    head_top = max((mesh.matrix_world @ Vector(c)).z for c in mesh.bound_box)
    focus = Vector((0.0, eye_l.y, (eye_l.z + head_top) / 2 - 0.06))
    sign = jaw_open_sign(arm, mesh)

    def pose(shape=None, eyes=0.0):
        for kb in keys:
            kb.value = 0.0
        if shape:
            keys[shape].value = 1.0
        rc.reset_pose(arm)
        for n in ("eye_l", "eye_r"):
            pb = arm.pose.bones[n]
            pb.rotation_mode = "XYZ"
            pb.rotation_euler = (0.0, 0.0, math.radians(eyes))
        if shape == "jawOpen":
            pb = arm.pose.bones["jaw"]
            pb.rotation_mode = "XYZ"
            pb.rotation_euler = (math.radians(12 * sign), 0.0, 0.0)
        bpy.context.view_layer.update()

    for tag, shape, eyes in (("basis", None, 0.0), ("jawOpen", "jawOpen", 0.0), ("eyeBlinkLeft", "eyeBlinkLeft", 0.0),
                             ("eyeWideLeft", "eyeWideLeft", 0.0), ("eyeSquintLeft", "eyeSquintLeft", 0.0),
                             ("mouthSmileLeft", "mouthSmileLeft", 0.0), ("browInnerUp", "browInnerUp", 0.0),
                             ("eyesLeft", None, 20.0)):
        pose(shape, eyes)
        render_views([mesh], outdir, [(tag + "_front", 0), (tag + "_34", 35)], focus=focus)
    pose()


def main():
    a = _args()
    if a["step"] == "prep":
        prep(a)
    else:
        finish(a)


if __name__ == "__main__":
    main()
