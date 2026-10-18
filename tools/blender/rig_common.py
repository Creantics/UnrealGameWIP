"""bpy helpers shared by the rigging tools: the canonical armature, pose keys, the ROM / T-pose actions,
the rig export profile and evaluation helpers. The data lives in rig_spec.py (no bpy)."""
import math

import bpy
from mathutils import Euler, Matrix, Vector

import ne_blender as nb
import rig_spec

FPS = 30


def build_canonical_armature(name="canonical", rows=None):
    """Armature with the 87 canonical joints in canonical order. Each bone's roll is set so that its
    local +X matches the template's x_axis hint (orthogonalised against the bone direction)."""
    rows = rows or rig_spec.JOINTS
    arm_data = bpy.data.armatures.new(name)
    arm = bpy.data.objects.new(name, arm_data)
    bpy.context.scene.collection.objects.link(arm)
    nb.activate(arm)
    bpy.ops.object.mode_set(mode="EDIT")
    eb = {}
    for jname, parent, head, tail, xa, _group, deform in rows:
        b = arm_data.edit_bones.new(jname)
        b.head, b.tail = Vector(head), Vector(tail)
        y = (b.tail - b.head).normalized()
        x = Vector(xa)
        x = (x - y * x.dot(y)).normalized()
        b.align_roll(x.cross(y))  # align_roll aligns the bone's local Z
        b.use_deform = bool(deform)
        b.use_connect = False
        if parent:
            b.parent = eb[parent]
        eb[jname] = b
    bpy.ops.object.mode_set(mode="OBJECT")
    for pb in arm.pose.bones:
        pb.rotation_mode = "QUATERNION"
    return arm


def bone_x_axis(arm, name):
    """The bone's rest local +X in armature space."""
    return arm.data.bones[name].matrix_local.to_3x3().col[0].normalized()


def reset_pose(arm):
    for pb in arm.pose.bones:
        pb.location = (0, 0, 0)
        pb.rotation_mode = "QUATERNION"
        pb.rotation_quaternion = (1, 0, 0, 0)
        pb.scale = (1, 1, 1)


def set_pose(arm, rot_deg):
    """rot_deg: {joint: (x, y, z) degrees about the bone's local axes (XYZ Euler)}."""
    for name, (x, y, z) in rot_deg.items():
        pb = arm.pose.bones[name]
        pb.rotation_mode = "QUATERNION"
        pb.rotation_quaternion = Euler((math.radians(x), math.radians(y), math.radians(z)), "XYZ").to_quaternion()


def new_action(obj, name):
    act = bpy.data.actions.new(name)
    act.use_fake_user = True
    if obj.animation_data is None:
        obj.animation_data_create()
    obj.animation_data.action = act
    return act


def iter_fcurves(action):
    if hasattr(action, "fcurves"):
        yield from action.fcurves
        return
    for layer in action.layers:
        for strip in layer.strips:
            for bag in strip.channelbags:
                yield from bag.fcurves


def key_all_rotations(arm, frame, names):
    for n in names:
        arm.pose.bones[n].keyframe_insert("rotation_quaternion", frame=frame)


def body_joints(arm):
    """Every pose bone except the face joints: body clips never key jaw / tongue / eyes (§8.2, §10.1)."""
    face = set(rig_spec.FACE_JOINTS)
    return [b.name for b in arm.pose.bones if b.name not in face]


def make_rom_action(arm, joints=None):
    """The ROM clip (§10.2): one canonical pose every ROM_STEP_FRAMES frames, linear between."""
    joints = joints or body_joints(arm)
    act = new_action(arm, "rom")
    for i, (_name, pose) in enumerate(rig_spec.ROM_POSES):
        reset_pose(arm)
        set_pose(arm, rig_spec.expand_pose(pose))
        key_all_rotations(arm, i * rig_spec.ROM_STEP_FRAMES, joints)
    for fc in iter_fcurves(act):
        for kp in fc.keyframe_points:
            kp.interpolation = "LINEAR"
    reset_pose(arm)
    return act


def make_tpose_action(arm, joints=None):
    joints = joints or body_joints(arm)
    act = new_action(arm, "tpose")
    reset_pose(arm)
    set_pose(arm, rig_spec.expand_pose(rig_spec.TPOSE))
    key_all_rotations(arm, 0, joints)
    key_all_rotations(arm, 1, joints)
    reset_pose(arm)
    return act


# --------------------------------------------------------------------------------------------
# Rig export profile (§11). The default nb.export_gltf profile is left untouched.
# --------------------------------------------------------------------------------------------

def export_rig_glb(path, extra=None):
    """Exports the scene with the `rig` profile into one .glb. Delegates to nb.export_rig."""
    return nb.export_rig(path, extra)


def evaluated_positions(obj):
    """World-space evaluated vertex positions (shape keys + armature) of a mesh object."""
    dg = bpy.context.evaluated_depsgraph_get()
    ev = obj.evaluated_get(dg)
    me = ev.to_mesh()
    mw = obj.matrix_world
    out = [mw @ v.co for v in me.vertices]
    ev.to_mesh_clear()
    return out


def joint_world_matrices_gltf(arm):
    """Pose-bone armature-space matrices converted to glTF axes (the engine's joint world matrices,
    with the armature object at the origin). Returns {name: 4x4 list (row-major)}."""
    C = Matrix(((1, 0, 0, 0), (0, 0, 1, 0), (0, -1, 0, 0), (0, 0, 0, 1)))  # Blender -> glTF axes
    out = {}
    for pb in arm.pose.bones:
        m = C @ arm.matrix_world @ pb.matrix @ C.inverted()
        out[pb.name] = [[float(m[r][c]) for c in range(4)] for r in range(4)]
    return out


# --------------------------------------------------------------------------------------------
# Engine round-trip expectations (§10.4): <asset>.rigexpect.json, read by tools/ne_rig_check
# --------------------------------------------------------------------------------------------
_C = Matrix(((1, 0, 0, 0), (0, 0, 1, 0), (0, -1, 0, 0), (0, 0, 0, 1)))  # Blender -> glTF axes


def rom_check_frames(count=12):
    """`count` ROM key frames spread over the clip (pose 1 .. last). Keys only: between keys Blender
    nlerps quaternion channels while glTF LINEAR slerps, so an unsampled export differs there by design
    (the rig profile samples every frame; the proof head keeps keys to stay small)."""
    last = len(rig_spec.ROM_POSES) - 1
    idx = sorted({1 + round(k * (last - 1) / (count - 1)) for k in range(count)})
    return [i * rig_spec.ROM_STEP_FRAMES for i in idx]


def _joint_state(arm):
    """{name: (head position glTF, skinning rotation quaternion glTF wxyz)}. The skinning rotation is
    world * inverse-bind, which does not depend on the exporter's bone-axis convention."""
    out = {}
    mw = arm.matrix_world
    for pb in arm.pose.bones:
        head = _C @ mw @ pb.matrix.translation.to_4d()
        skin = _C @ mw @ pb.matrix @ pb.bone.matrix_local.inverted() @ mw.inverted() @ _C.inverted()
        q = skin.to_quaternion()
        if q.w < 0:
            q = -q
        out[pb.name] = ([round(float(c), 6) for c in head[:3]], [round(float(c), 7) for c in q])
    return out


def write_rigexpect(arm, mesh_obj, path, clip="rom", samples=64, seed=11):
    """Joint heads + skinning rotations at rest and at rom_check_frames() of `clip`, plus `samples`
    evaluated vertex positions of `mesh_obj` (shape keys at 0). Everything in glTF axes."""
    import random
    act = bpy.data.actions[clip]
    for kb in (mesh_obj.data.shape_keys.key_blocks if mesh_obj.data.shape_keys else []):
        kb.value = 0.0
    if arm.animation_data is None:
        arm.animation_data_create()
    arm.animation_data.action = None
    reset_pose(arm)
    bpy.context.view_layer.update()
    rest = [v.copy() for v in evaluated_positions(mesh_obj)]
    keyq = {}
    for i, v in enumerate(rest):
        keyq.setdefault((round(v.x, 6), round(v.y, 6), round(v.z, 6)), []).append(i)
    uniq = [ix[0] for ix in keyq.values() if len(ix) == 1]
    rnd = random.Random(seed)
    pick = sorted(rnd.sample(uniq, min(samples, len(uniq))))
    names = [pb.name for pb in arm.pose.bones]
    frames = []

    def snap(label, t):
        st = _joint_state(arm)
        pos = evaluated_positions(mesh_obj)
        frames.append({"label": label, "time": t,
                       "heads": [st[n][0] for n in names], "rot": [st[n][1] for n in names],
                       "vertices": [[round(c, 6) for c in nb.to_gltf_axes(pos[i])] for i in pick]})

    snap("rest", None)
    arm.animation_data.action = act
    for f in rom_check_frames():
        bpy.context.scene.frame_set(f)
        bpy.context.view_layer.update()
        snap(rig_spec.ROM_POSES[f // rig_spec.ROM_STEP_FRAMES][0], f / FPS)
    arm.animation_data.action = None
    bpy.context.scene.frame_set(0)
    reset_pose(arm)
    bpy.context.view_layer.update()
    data = {
        "about": "Blender-evaluated joints and skinned vertices for tools/ne_rig_check (rigging_plan.md 10.4). "
                 "heads: joint world positions; rot: world * inverseBind rotation (w, x, y, z). glTF axes, metres.",
        "clip": clip,
        "joint_names": names,
        "tolerance": {"pos_m": rig_spec.THRESHOLDS["R13"]["pos_m"], "rot_deg": rig_spec.THRESHOLDS["R13"]["rot_deg"],
                      "vertex_m": 0.001, "bind_identity_eps": rig_spec.THRESHOLDS["R10"]["bind_identity_eps"]},
        "samples": [[round(c, 6) for c in nb.to_gltf_axes(rest[i])] for i in pick],
        "frames": frames,
    }
    import json
    import os
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w") as f:  # compact: one frame per line
        frames_txt = ",\n".join(json.dumps(fr, separators=(",", ":")) for fr in frames)
        head = {k: v for k, v in data.items() if k != "frames"}
        f.write(json.dumps(head, separators=(",", ":"))[:-1] + ',\n"frames":[\n' + frames_txt + "]}\n")
    return data


# --------------------------------------------------------------------------------------------
# Skin weights: automatic (heat map) weights and the weight post-pass (§9.2, §7: R6-R8)
# --------------------------------------------------------------------------------------------
# Joints that never get automatic weights: sockets carry props, helpers are driven by joint rules (§11),
# tongue / eyes are rigid islands weighted by hand (a heat map would pull face skin onto them).
NO_AUTO_WEIGHT = set(rig_spec.SOCKETS) | {r[0] for r in rig_spec.JOINTS if r[5] == "helper"} | {
    "tongue_01", "tongue_02", "tongue_03", "eye_l", "eye_r"}

# Twist bands (left names; right mirrored): the weight a segment's chain holds is redistributed along the
# segment's axis by tent functions at these parameters (0 = segment head, 1 = the next joint's head).
TWIST_BANDS = [
    (("upperarm", "upperarm_twist_01", "upperarm_twist_02"), "lowerarm", (0.0, 1.0 / 3.0, 2.0 / 3.0)),
    (("lowerarm", "lowerarm_twist_01", "lowerarm_twist_02"), "hand", (0.0, 1.0 / 3.0, 2.0 / 3.0)),
    (("thigh", "thigh_twist_01"), "calf", (0.0, 0.5)),
    (("calf", "calf_twist_01"), "foot", (0.0, 0.5)),
]


def bind_mesh(obj, arm):
    """Armature modifier + parent, no Preserve Volume (E3: the engine does linear blend skinning)."""
    for m in [m for m in obj.modifiers if m.type == "ARMATURE"]:
        obj.modifiers.remove(m)
    mod = obj.modifiers.new("Armature", "ARMATURE")
    mod.object = arm
    mod.use_deform_preserve_volume = False
    mw = obj.matrix_world.copy()
    obj.parent = arm
    obj.matrix_world = mw
    return mod


def auto_weights(obj, arm):
    """Heat-map weights from the deform joints minus NO_AUTO_WEIGHT. Vertices the heat solve leaves empty
    (it fails on non-manifold or disconnected pieces) get their nearest bone segment. -> count of those."""
    saved = {b.name: b.use_deform for b in arm.data.bones}
    for b in arm.data.bones:
        if b.name in NO_AUTO_WEIGHT:
            b.use_deform = False
    for o in bpy.context.selected_objects:
        o.select_set(False)
    obj.select_set(True)
    arm.select_set(True)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.parent_set(type="ARMATURE_AUTO")
    for b in arm.data.bones:
        b.use_deform = saved[b.name]
    bind_mesh(obj, arm)
    for name in saved:  # every deform joint has a group (possibly empty) so names resolve on export
        if saved[name] and name not in obj.vertex_groups:
            obj.vertex_groups.new(name=name)
    names = [b.name for b in arm.data.bones if b.use_deform and b.name not in NO_AUTO_WEIGHT]
    gi = {obj.vertex_groups[n].index for n in names}
    segs = [(n, arm.matrix_world @ arm.data.bones[n].head_local, arm.matrix_world @ arm.data.bones[n].tail_local)
            for n in names]
    mw = obj.matrix_world
    empty = 0
    for v in obj.data.vertices:
        if any(g.group in gi and g.weight > 0 for g in v.groups):
            continue
        p = mw @ v.co
        best, bd = None, 1e9
        for n, h, t in segs:
            d = t - h
            u = max(0.0, min(1.0, (p - h).dot(d) / max(d.length_squared, 1e-12)))
            dist = (h + d * u - p).length
            if dist < bd:
                best, bd = n, dist
        obj.vertex_groups[best].add([v.index], 1.0, "REPLACE")
        empty += 1
    return empty


def _weight_matrix(obj, names):
    import numpy as np
    col = {n: k for k, n in enumerate(names)}
    gname = {g.index: g.name for g in obj.vertex_groups}
    W = np.zeros((len(obj.data.vertices), len(names)))
    for v in obj.data.vertices:
        for g in v.groups:
            k = col.get(gname.get(g.group))
            if k is not None:
                W[v.index, k] = g.weight
    return W


def weight_post_pass(obj, arm, mirror=True, twist=True, prune=None, limit=None, mirror_eps=0.0005):
    """The §9.2 post-pass on one mesh: twist bands -> mirror average -> prune < 0.01 -> keep the 4 largest
    -> normalise. Returns a small dict of what changed (for the report)."""
    import numpy as np
    from mathutils.kdtree import KDTree
    prune = rig_spec.THRESHOLDS["R7"]["weight_min"] if prune is None else prune
    limit = rig_spec.THRESHOLDS["R8"]["influences_max"] if limit is None else limit
    names = [b.name for b in arm.data.bones if b.use_deform]
    col = {n: k for k, n in enumerate(names)}
    W = _weight_matrix(obj, names)
    mw = obj.matrix_world
    P = np.array([tuple(mw @ v.co) for v in obj.data.vertices])
    stats = {"vertices": len(P)}
    if twist:
        moved = 0
        for side in ("_l", "_r"):
            for chain, nxt, ts in TWIST_BANDS:
                ks = [col.get(c + side) for c in chain]
                if any(k is None for k in ks):
                    continue
                h = np.array(arm.matrix_world @ arm.data.bones[chain[0] + side].head_local)
                e = np.array(arm.matrix_world @ arm.data.bones[nxt + side].head_local)
                d = e - h
                t = np.clip(((P - h) @ d) / max(float(d @ d), 1e-12), 0.0, 1.0)
                tot = W[:, ks].sum(axis=1)
                sel = tot > 0
                share = np.zeros((len(P), len(ks)))
                for i in range(len(ks)):  # tents through ts; the first / last node own everything beyond
                    lo = ts[i - 1] if i > 0 else None
                    hi = ts[i + 1] if i + 1 < len(ks) else None
                    s = np.ones(len(P))
                    if lo is not None:
                        s = np.minimum(s, np.clip((t - lo) / (ts[i] - lo), 0.0, 1.0))
                    if hi is not None:
                        s = np.minimum(s, np.clip((hi - t) / (hi - ts[i]), 0.0, 1.0))
                    share[:, i] = s
                share /= np.maximum(share.sum(axis=1, keepdims=True), 1e-12)
                W[np.ix_(sel, ks)] = share[sel] * tot[sel, None]
                moved += int(sel.sum())
        stats["twist_band_updates"] = moved
    if mirror:
        kd = KDTree(len(P))
        for i, p in enumerate(P):
            kd.insert(p, i)
        kd.balance()
        perm = [col.get(rig_spec.mirror_name(n), k) for k, n in enumerate(names)]
        M = W.copy()
        unpaired = 0
        for i, p in enumerate(P):
            _co, j, dist = kd.find((-p[0], p[1], p[2]))
            if j is None or dist > mirror_eps:
                unpaired += 1
                continue
            M[i] = 0.5 * (W[i] + W[j][perm])
        W = M
        stats["mirror_unpaired"] = unpaired
    W[W < prune] = 0.0
    if W.shape[1] > limit:
        # Keep the `limit` largest. Weights tied at the cut are kept only if all of them fit, so a left /
        # right tie never resolves by column order (which would break the mirror, R6).
        thr = -np.sort(-W, axis=1)[:, limit - 1:limit]
        gt = W > thr + 1e-9
        tie = (np.abs(W - thr) <= 1e-9) & (W > 0)
        fits = (gt.sum(axis=1) + tie.sum(axis=1) <= limit)[:, None]
        W = np.where(gt | (tie & fits), W, 0.0)
    s = W.sum(axis=1, keepdims=True)
    stats["unweighted"] = int((s[:, 0] <= 0).sum())
    W = np.where(s > 0, W / np.maximum(s, 1e-12), W)
    for n in names:  # write back
        g = obj.vertex_groups.get(n) or obj.vertex_groups.new(name=n)
        g.remove(list(range(len(P))))
        k = col[n]
        for i in np.nonzero(W[:, k])[0]:
            g.add([int(i)], float(W[i, k]), "REPLACE")
    stats["max_influences"] = int((W > 0).sum(axis=1).max(initial=0))
    return stats
