"""Cycles review renders of a dressed MetaHuman Real3 lead, headless: the body, its face and any bound pieces
(garment_bind.py or head_prop.py FBXs on the same armature) at rest, in walk and turn poses, and close up.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/review_renders.py -- \
        BODY.fbx FACE.fbx OUT_DIR PIECE.fbx [PIECE.fbx ...] [shots=rest,walk,turn,close]
        [close=hem,seam,neck,face,ears,feet,wrists,chest] [res=900] [samples=96] [gpu=1] [skin=r,g,b]
        [prefix=set_b] [exposure=0]

Every piece is imported with its own armature copy; the body armature is posed and every other armature (the face's,
each piece's) follows it bone for bone (Copy Transforms on the bones they share; the facial bones ride the head), so
the pieces deform as they would in the game. A piece finds its textures next to its FBX: NAME_BC.png, NAME_N_gl.png
(else NAME_N.png, read as DirectX and flipped) and NAME_ORM.png. The face keeps its skin, eye, mouth and lash sections
in plain colours and drops the hidden and the see-through ones and the lash cards (lashes=1 keeps them), told apart
by shape (face_roles), not by slot name.

rest: front, side (the wearer's left), back and three-quarter (front left), the whole figure.
walk: four frames of a stride (left contact, passing, right contact, passing), front three-quarter and side.
turn: two frames of a turn in place to the wearer's left (hips 35 deg, shoulders 55), front and back three-quarter.
close: hem (the lowest rim of the lowest piece, front and side), seam (the waist at each side), neck (the collar, front
and back), face (the head front and three-quarter: glasses, a mask), ears (each side of the head), feet (both
feet front, side and back: shoes), wrists (each forearm: wraps, cuffs), chest (front, side and upper back: a harness).
Writes OUT_DIR/PREFIX_<shot>.png and OUT_DIR/PREFIX_shots.json (the list with what each shows, for the reviewer)."""
import json
import math
import os
import sys

import bpy
import numpy as np
from mathutils import Matrix, Vector


def log(*a):
    print("review_renders:", *a, flush=True)


argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
pos = [a for a in argv if "=" not in a]
opt = dict(a.split("=", 1) for a in argv if "=" in a)
if len(pos) < 4:
    sys.exit("review_renders: BODY.fbx FACE.fbx OUT_DIR PIECE.fbx [...] [k=v]")
BODY, FACE, OUT = (os.path.abspath(p) for p in pos[:3])
PIECES = [os.path.abspath(p) for p in pos[3:]]
SHOTS = opt.get("shots", "rest,walk,turn,close").split(",")
CLOSE = opt.get("close", "hem,seam,neck").split(",")
RES = int(opt.get("res", 900))
PREFIX = opt.get("prefix", "review")
os.makedirs(OUT, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)


def imp(p):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=p)
    return [o for o in bpy.data.objects if o not in before]


def wverts(o, evaluated=False):
    if evaluated:
        dg = bpy.context.evaluated_depsgraph_get()
        oe = o.evaluated_get(dg)
        me = oe.to_mesh()
    else:
        me = o.data
    co = np.empty(len(me.vertices) * 3)
    me.vertices.foreach_get("co", co)
    if evaluated:
        oe.to_mesh_clear()
    M = np.array(o.matrix_world)
    return co.reshape(-1, 3) @ M[:3, :3].T + M[:3, 3]


def plain(name, rgb, rough=0.5, sss=0.0):
    m = bpy.data.materials.new(name)
    if m.node_tree is None:
        m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = (*rgb, 1)
    b.inputs["Roughness"].default_value = rough
    if sss:
        b.inputs["Subsurface Weight"].default_value = sss
        b.inputs["Subsurface Radius"].default_value = (0.012, 0.004, 0.0025)
    return m


def textured(name, folder):
    def find(sfx):
        p = os.path.join(folder, name + sfx + ".png")
        return p if os.path.exists(p) else None
    bc, ngl, ndx, orm = find("_BC"), find("_N_gl"), find("_N"), find("_ORM")
    if not bc:
        return None
    m = bpy.data.materials.new("R_" + name)
    if m.node_tree is None:
        m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes["Principled BSDF"]
    t = nt.nodes.new("ShaderNodeTexImage")
    t.image = bpy.data.images.load(bc)
    nt.links.new(t.outputs["Color"], b.inputs["Base Color"])
    if orm:
        to = nt.nodes.new("ShaderNodeTexImage")
        to.image = bpy.data.images.load(orm)
        to.image.colorspace_settings.name = "Non-Color"
        sp = nt.nodes.new("ShaderNodeSeparateColor")
        nt.links.new(to.outputs["Color"], sp.inputs["Color"])
        nt.links.new(sp.outputs["Green"], b.inputs["Roughness"])
        nt.links.new(sp.outputs["Blue"], b.inputs["Metallic"])
    npath = ngl or ndx
    if npath:
        tn = nt.nodes.new("ShaderNodeTexImage")
        tn.image = bpy.data.images.load(npath)
        tn.image.colorspace_settings.name = "Non-Color"
        nm = nt.nodes.new("ShaderNodeNormalMap")
        src = tn.outputs["Color"]
        if not ngl:  # DirectX: flip green for Blender
            sp = nt.nodes.new("ShaderNodeSeparateColor")
            inv = nt.nodes.new("ShaderNodeMath")
            inv.operation = "SUBTRACT"
            inv.inputs[0].default_value = 1.0
            cm = nt.nodes.new("ShaderNodeCombineColor")
            nt.links.new(src, sp.inputs["Color"])
            nt.links.new(sp.outputs["Red"], cm.inputs["Red"])
            nt.links.new(sp.outputs["Green"], inv.inputs[1])
            nt.links.new(inv.outputs[0], cm.inputs["Green"])
            nt.links.new(sp.outputs["Blue"], cm.inputs["Blue"])
            src = cm.outputs["Color"]
        nt.links.new(src, nm.inputs["Color"])
        nt.links.new(nm.outputs["Normal"], b.inputs["Normal"])
    log("material %s: BC %s, normal %s, ORM %s" % (name, bool(bc), "OpenGL" if ngl else ("DirectX" if ndx else "-"),
                                                     bool(orm)))
    return m


# -- the lead -----------------------------------------------------------------------------------------------------------
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


skin_rgb = tuple(float(c) for c in opt.get("skin", "0.62,0.45,0.36").split(","))
SKIN = plain("skin", skin_rgb, 0.45, 0.15)
bo = imp(BODY)
barm = next(o for o in bo if o.type == "ARMATURE")
lod0 = max((o for o in bo if o.type == "MESH"), key=lambda o: len(o.data.vertices))
for o in bo:
    if o.type == "MESH" and o is not lod0:
        bpy.data.objects.remove(o, do_unlink=True)
lod0.data.materials.clear()
lod0.data.materials.append(SKIN)
fo = imp(FACE)
farm = next((o for o in fo if o.type == "ARMATURE"), None)
face = max((o for o in fo if o.type == "MESH"), key=lambda o: len(o.data.vertices))
for o in fo:
    if o.type == "MESH" and o is not face:
        bpy.data.objects.remove(o, do_unlink=True)
cols = {"eye": ((0.80, 0.78, 0.74), 0.15), "mouth": ((0.85, 0.82, 0.74), 0.3), "lash": ((0.03, 0.025, 0.02), 0.6)}
fm = face.data
eyes = [np.array(farm.matrix_world @ farm.data.bones[b].head_local) if farm is not None and b in farm.data.bones
        else None for b in ("FACIAL_L_Eye", "FACIAL_R_Eye")]
if any(e is None for e in eyes):
    sys.exit("review_renders: the face has no FACIAL_L_Eye / FACIAL_R_Eye bones")
roles = face_roles(face, *eyes)
log("face sections (slot name: role) " + ", ".join(
    "%s: %s" % (fm.materials[i].name if i < len(fm.materials) and fm.materials[i] else i, r) for i, r in sorted(roles.items())))
# the lashes are opacity-mapped cards: without their mask they render as solid tufts, so they go unless lashes=1
dropped = ("cover",) if opt.get("lashes") == "1" else ("cover", "lash")
drop_slots = [i for i in range(len(fm.materials)) if roles.get(i, "cover") in dropped]
for i in range(len(fm.materials)):
    r = roles.get(i, "cover")
    if r not in dropped:
        fm.materials[i] = SKIN if r == "skin" else plain("f_%s_%d" % (r, i), *cols[r])
if drop_slots:
    import bmesh
    bm = bmesh.new()
    bm.from_mesh(fm)
    bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.material_index in drop_slots], context="FACES")
    bm.to_mesh(fm)
    bm.free()
pieces = []
arms = [a for a in (farm,) if a is not None]
for p in PIECES:
    new = imp(p)
    a = next((o for o in new if o.type == "ARMATURE"), None)
    ms = [o for o in new if o.type == "MESH"]
    nm = os.path.splitext(os.path.basename(p))[0]
    mat = textured(nm, os.path.dirname(p))
    for o in ms:
        if mat is not None:
            o.data.materials.clear()
            o.data.materials.append(mat)
        pieces.append(o)
    if a is not None:
        arms.append(a)
    log("piece %s: %s" % (nm, ", ".join("%s %d verts" % (o.name, len(o.data.vertices)) for o in ms)))
for a in arms:  # follow the body armature bone for bone
    for pb in a.pose.bones:
        if pb.name in barm.pose.bones:
            c = pb.constraints.new("COPY_TRANSFORMS")
            c.target, c.subtarget = barm, pb.name
bpy.context.view_layer.update()


def head(name):
    return barm.matrix_world @ barm.pose.bones[name].head


up = Vector((0, 0, 1))
# forward from the heels to the toes
toe = (head("ball_l") + head("ball_r")) * 0.5 if "ball_l" in barm.pose.bones else None
heel = (head("foot_l") + head("foot_r")) * 0.5
fwd = (toe - heel) if toe is not None else Vector((0, -1, 0))
fwd.z = 0
fwd.normalize()
left = up.cross(fwd)
log("forward %s, wearer's left %s" % (tuple(round(c, 3) for c in fwd), tuple(round(c, 3) for c in left)))


def rot(name, deg, axis):
    if name not in barm.pose.bones:
        return
    pb = barm.pose.bones[name]
    mw = barm.matrix_world
    piv = mw @ pb.head
    R = Matrix.Translation(piv) @ Matrix.Rotation(math.radians(deg), 4, axis) @ Matrix.Translation(-piv)
    pb.matrix = mw.inverted() @ R @ mw @ pb.matrix
    bpy.context.view_layer.update()


def swing(name, deg, child):
    """Turn a limb bone about the wearer's left axis so that its child moves forward for deg > 0."""
    if name not in barm.pose.bones or child not in barm.pose.bones:
        return
    c0 = head(child).copy()
    rot(name, 5, left)
    s = 1.0 if (head(child) - c0).dot(fwd) > 0 else -1.0
    rot(name, -5, left)
    rot(name, s * deg, left)


def reset():
    for pb in barm.pose.bones:
        pb.matrix_basis = Matrix()
    bpy.context.view_layer.update()


def walk(phase):
    """A stride: phase 0 left heel strike, 1 passing (left planted), 2 right heel strike, 3 passing (right planted)."""
    reset()
    a, b = ("l", "r") if phase in (0, 1) else ("r", "l")
    if phase in (0, 2):  # a leads, b trails
        swing("thigh_" + a, 24, "calf_" + a)
        swing("calf_" + a, -6, "foot_" + a)
        swing("thigh_" + b, -16, "calf_" + b)
        swing("calf_" + b, -28, "foot_" + b)
        swing("upperarm_" + b, 18, "lowerarm_" + b)
        swing("upperarm_" + a, -14, "lowerarm_" + a)
        rot("pelvis", 5 if a == "l" else -5, up)
        rot("spine_03", -8 if a == "l" else 8, up)
    else:  # a planted under the body, b swinging through with its knee bent
        swing("thigh_" + a, -4, "calf_" + a)
        swing("thigh_" + b, 18, "calf_" + b)
        swing("calf_" + b, -45, "foot_" + b)
    for s in "lr":
        swing("lowerarm_" + s, 18, "hand_" + s)


def turn(step):
    reset()
    k = 1.0 if step == 0 else 0.6
    # the legs swing about the rest pose's left axis before the pelvis turns (as in walk), so the swing stays pure
    # flexion; swinging after a 35 deg pelvis turn tipped the thighs sideways and drove one thigh into the other
    swing("thigh_l", 14 * k, "calf_l")
    swing("calf_l", -18 * k, "foot_l")
    swing("thigh_r", -8 * k, "calf_r")
    rot("pelvis", 35 * k, up)
    rot("spine_01", 8 * k, up)
    rot("spine_03", 12 * k, up)
    rot("head", 10 * k, up)


# -- the stage ----------------------------------------------------------------------------------------------------------
sc = bpy.context.scene
sc.render.engine = "CYCLES"
sc.cycles.samples = int(opt.get("samples", 96))
sc.cycles.use_denoising = True
sc.render.film_transparent = False
sc.view_settings.view_transform = "AgX"
sc.view_settings.exposure = float(opt.get("exposure", 0.0))  # stops; the stage's lights read dark cloth a stop light
if opt.get("gpu", "0") == "1":
    try:
        cp = bpy.context.preferences.addons["cycles"].preferences
        for kind in ("OPTIX", "CUDA", "HIP"):
            try:
                cp.compute_device_type = kind
                cp.refresh_devices()
                if any(d.type == kind for d in cp.devices):
                    for d in cp.devices:
                        d.use = d.type == kind
                    sc.cycles.device = "GPU"
                    log("Cycles on the GPU (%s)" % kind)
                    break
            except TypeError:
                continue
    except Exception as e:  # noqa: BLE001 - any failure falls back to the CPU
        log("GPU setup failed (%s); CPU" % e)
world = bpy.data.worlds.new("w")
sc.world = world
world.use_nodes = True
world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.32, 0.33, 0.35, 1)
world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.35
floor_me = bpy.data.meshes.new("floor")
floor_me.from_pydata([(-4, -4, 0), (4, -4, 0), (4, 4, 0), (-4, 4, 0)], [], [(0, 1, 2, 3)])
floor = bpy.data.objects.new("floor", floor_me)
sc.collection.objects.link(floor)
floor_z = float(wverts(lod0)[:, 2].min())
floor.location.z = floor_z
floor.data.materials.append(plain("floor", (0.30, 0.30, 0.31), 0.8))
mid = (head("pelvis") + head("neck_01")) * 0.5


def light(name, energy, size, pos_dir, dist):
    L = bpy.data.lights.new(name, "AREA")
    L.energy, L.size = energy, size
    o = bpy.data.objects.new(name, L)
    sc.collection.objects.link(o)
    o.location = mid + pos_dir.normalized() * dist
    o.rotation_euler = (mid - o.location).to_track_quat("-Z", "Y").to_euler()


light("key", 900, 1.5, fwd * 1.0 + left * 0.8 + up * 0.9, 3.0)
light("fill", 300, 2.5, fwd * 1.0 - left * 1.0 + up * 0.2, 3.5)
light("rim", 600, 1.0, -fwd * 1.0 + left * 0.3 + up * 1.0, 3.0)
cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
sc.collection.objects.link(cam)
sc.camera = cam
shots = []


def shoot(name, tgt, d, dist, lens, aspect=(3, 4), what=""):
    cam.data.lens = lens
    sc.render.resolution_x = int(RES * aspect[0] / max(aspect))
    sc.render.resolution_y = int(RES * aspect[1] / max(aspect))
    cam.location = tgt + d.normalized() * dist
    cam.rotation_euler = (tgt - cam.location).to_track_quat("-Z", "Y").to_euler()
    path = os.path.join(OUT, "%s_%s.png" % (PREFIX, name))
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)
    shots.append({"file": os.path.basename(path), "shows": what})
    log("shot %s" % path)


top_z = float(max(wverts(face)[:, 2].max(), wverts(lod0)[:, 2].max()))
body_c = Vector((mid.x, mid.y, (floor_z + top_z) / 2))
FULL = 4.4  # m: about a third-person game camera's distance, framed on the whole figure


def full(name, d, what):
    shoot(name, body_c, d, FULL, 70, (3, 4), what)


if "rest" in SHOTS:
    reset()
    full("rest_front", fwd, "at rest, from the front")
    full("rest_side", left, "at rest, from the wearer's left side")
    full("rest_back", -fwd, "at rest, from the back")
    full("rest_q34", fwd + left, "at rest, three-quarter front left")
if "walk" in SHOTS:
    for ph, lab in enumerate(("left_contact", "passing_a", "right_contact", "passing_b")):
        walk(ph)
        full("walk_%d_%s_q34" % (ph, lab), fwd + left * 0.8, "walk frame %d (%s), three-quarter front" % (ph, lab))
        full("walk_%d_%s_side" % (ph, lab), left, "walk frame %d (%s), from the side" % (ph, lab))
if "turn" in SHOTS:
    for st in (0, 1):
        turn(st)
        full("turn_%d_q34" % st, fwd + left, "turn frame %d, three-quarter front" % st)
        full("turn_%d_back" % st, -fwd + left * 0.6, "turn frame %d, three-quarter back" % st)
if "close" in SHOTS:
    reset()
    hd = head("head")
    if "hem" in CLOSE and pieces:
        lowest = min(pieces, key=lambda o: wverts(o, True)[:, 2].min())
        P = wverts(lowest, True)
        z = float(P[:, 2].min())
        tgt = Vector((float(P[:, 0].mean()), float(P[:, 1].mean()), z + 0.08))
        shoot("close_hem_front", tgt, fwd + up * 0.3, 1.1, 60, (4, 3), "close: the lowest hem from the front")
        shoot("close_hem_side", tgt, left + up * 0.3, 1.1, 60, (4, 3), "close: the lowest hem from the side")
    if "seam" in CLOSE:
        waist = (head("pelvis") + head("spine_02")) * 0.5
        for s, sd in (("l", 1), ("r", -1)):
            shoot("close_seam_%s" % s, waist, left * sd + fwd * 0.4, 1.0, 60, (1, 1),
                  "close: the waist and side seam, wearer's %s" % ("left" if sd > 0 else "right"))
    if "neck" in CLOSE:
        nk = head("neck_01")
        shoot("close_neck_front", nk, fwd + up * 0.2, 0.8, 60, (1, 1), "close: the collar from the front")
        shoot("close_neck_back", nk, -fwd + up * 0.2, 0.8, 60, (1, 1), "close: the collar from the back")
    if "feet" in CLOSE:
        ft = (head("foot_l") + head("foot_r")) * 0.5 + up * 0.02
        shoot("close_feet_front", ft, fwd + up * 0.35, 1.0, 60, (4, 3), "close: both feet from the front")
        shoot("close_feet_side", ft, left + up * 0.25, 1.0, 60, (4, 3), "close: both feet from the wearer's left")
        shoot("close_feet_back", ft, -fwd + left * 0.3 + up * 0.3, 1.0, 60, (4, 3), "close: both feet from behind")
    if "wrists" in CLOSE:
        for s, sd in (("l", 1), ("r", -1)):
            fa = (head("lowerarm_" + s) + head("hand_" + s)) * 0.5
            shoot("close_wrist_%s" % s, fa, fwd + left * sd * 0.7, 0.8, 60, (1, 1),
                  "close: the wearer's %s forearm and wrist" % ("left" if sd > 0 else "right"))
    if "chest" in CLOSE:
        ch = head("spine_04")
        shoot("close_chest_front", ch, fwd + up * 0.1, 1.0, 60, (1, 1), "close: the chest from the front")
        shoot("close_chest_side", ch, left + fwd * 0.3, 1.0, 60, (1, 1), "close: the chest from the wearer's left")
        shoot("close_chest_back", ch, -fwd + up * 0.1, 1.0, 60, (1, 1), "close: the upper back")
    if "face" in CLOSE:
        fc = hd + up * 0.07 + fwd * 0.06
        shoot("close_face_front", fc, fwd, 0.75, 85, (1, 1), "close: the face from the front")
        shoot("close_face_q34", fc, fwd + left, 0.75, 85, (1, 1), "close: the face, three-quarter left")
        shoot("close_face_top", fc, fwd + up * 1.2, 0.75, 85, (1, 1), "close: the face from above the brow")
    if "ears" in CLOSE:
        ec = hd + up * 0.07
        for s, sd in (("l", 1), ("r", -1)):
            shoot("close_ear_%s" % s, ec, left * sd + fwd * 0.15, 0.7, 85, (1, 1),
                  "close: the side of the head over the wearer's %s ear" % ("left" if sd > 0 else "right"))
json.dump(shots, open(os.path.join(OUT, PREFIX + "_shots.json"), "w"), indent=1)
log("done: %d shots in %s" % (len(shots), OUT))
