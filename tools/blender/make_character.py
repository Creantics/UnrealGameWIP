"""Builds assets/models/blender/hero.{glb,_gltf/} : a skinned character exported by Blender's glTF
exporter.

  * 21-bone humanoid armature (Blender convention: stands on +Z, faces -Y, T-pose along X)
  * 8-sided capsule segment per bone, vertex groups with smooth blending at the joints
  * 2 materials (textured cloth with baseColor + normal map, flat skin) -> 2 primitives
  * 4 actions exported as separate animations:
        idle  BEZIER   (-> CUBICSPLINE samplers)
        walk  LINEAR
        wave  BEZIER rotations + LINEAR hip bob
        snap  CONSTANT (-> STEP samplers)
  * hero.expected.json: bone head positions in glTF axes (Y-up check: Blender (x, y, z) -> (x, z, -y))
    and the pose-bone positions Blender evaluates at sample frames of every action; the unit tests
    check the engine against them.
"""
import math
import os

import bpy
from mathutils import Quaternion, Vector

import ne_blender as nb

# name, parent, head, tail (Blender coordinates), radius of the skin segment
S = 1.0
BONES = [
    ("hips", None, (0, 0, 0.95), (0, 0, 1.10), 0.15),
    ("spine", "hips", (0, 0, 1.10), (0, 0, 1.28), 0.14),
    ("chest", "spine", (0, 0, 1.28), (0, 0, 1.48), 0.16),
    ("neck", "chest", (0, 0, 1.48), (0, 0, 1.58), 0.05),
    ("head", "neck", (0, 0, 1.58), (0, 0, 1.82), 0.11),
]
for side, sx in (("L", 1), ("R", -1)):
    BONES += [
        (f"shoulder.{side}", "chest", (0.05 * sx, 0, 1.44), (0.20 * sx, 0, 1.44), 0.05),
        (f"upper_arm.{side}", f"shoulder.{side}", (0.20 * sx, 0, 1.44), (0.46 * sx, 0, 1.44), 0.05),
        (f"forearm.{side}", f"upper_arm.{side}", (0.46 * sx, 0, 1.44), (0.72 * sx, 0, 1.44), 0.045),
        (f"hand.{side}", f"forearm.{side}", (0.72 * sx, 0, 1.44), (0.84 * sx, 0, 1.44), 0.04),
        (f"thigh.{side}", "hips", (0.10 * sx, 0, 0.95), (0.10 * sx, 0, 0.52), 0.075),
        (f"shin.{side}", f"thigh.{side}", (0.10 * sx, 0, 0.52), (0.10 * sx, 0, 0.10), 0.06),
        (f"foot.{side}", f"shin.{side}", (0.10 * sx, 0, 0.10), (0.10 * sx, -0.12, 0.04), 0.05),
        (f"toe.{side}", f"foot.{side}", (0.10 * sx, -0.12, 0.04), (0.10 * sx, -0.24, 0.03), 0.04),
    ]
SKIN_BONES = {"head", "neck", "hand.L", "hand.R", "toe.L", "toe.R"}
SEGMENTS = 8
FPS = 30


def build_armature():
    arm_data = bpy.data.armatures.new("HeroArmature")
    arm = bpy.data.objects.new("Hero", arm_data)
    bpy.context.scene.collection.objects.link(arm)
    nb.activate(arm)
    bpy.ops.object.mode_set(mode="EDIT")
    eb = {}
    for name, parent, head, tail, _ in BONES:
        b = arm_data.edit_bones.new(name)
        b.head, b.tail = Vector(head), Vector(tail)
        b.roll = 0.0
        if parent:
            b.parent = eb[parent]
        eb[name] = b
    bpy.ops.object.mode_set(mode="OBJECT")
    return arm


def smoothstep(x):
    x = max(0.0, min(1.0, x))
    return x * x * (3 - 2 * x)


def build_mesh(arm, cloth_mat, skin_mat):
    verts, faces, uvs, mats = [], [], [], []
    weights = {}  # vertex index -> {bone: w}
    rings_t = [0.0, 0.2, 0.5, 0.8, 1.0]
    for bone_index, (name, parent, head, tail, radius) in enumerate(BONES):
        h, t = Vector(head), Vector(tail)
        axis = (t - h)
        length = axis.length
        d = axis.normalized()
        ref = Vector((0, 1, 0)) if abs(d.y) < 0.9 else Vector((1, 0, 0))
        u_axis = d.cross(ref).normalized()
        v_axis = d.cross(u_axis).normalized()
        mat_idx = 1 if name in SKIN_BONES else 0
        ring_start = []
        for ti in rings_t:
            centre = h + d * (length * ti)
            r = radius * (1.0 - 0.15 * abs(ti - 0.5))  # slight bulge in the middle
            ring_start.append(len(verts))
            # weights: blend towards the parent over the first 40 % of the segment
            wself = 0.5 + 0.5 * smoothstep(ti / 0.4) if parent and ti < 0.4 else 1.0
            for k in range(SEGMENTS):
                a = 2 * math.pi * k / SEGMENTS
                p = centre + (u_axis * math.cos(a) + v_axis * math.sin(a)) * r
                weights[len(verts)] = {name: wself} if wself >= 1.0 else {name: wself, parent: 1.0 - wself}
                verts.append(tuple(p))
        for ri in range(len(rings_t) - 1):
            for k in range(SEGMENTS):
                k2 = (k + 1) % SEGMENTS
                a0, a1 = ring_start[ri] + k, ring_start[ri] + k2
                b0, b1 = ring_start[ri + 1] + k, ring_start[ri + 1] + k2
                faces.append((a0, a1, b1, b0))
                mats.append(mat_idx)
                # texel density ~1 texture repeat per 0.35 m: u around the segment, v along it
                circ = 2 * math.pi * radius / 0.35
                v0 = (length * rings_t[ri] + bone_index * 0.13) / 0.35
                v1 = (length * rings_t[ri + 1] + bone_index * 0.13) / 0.35
                uvs.extend([(circ * k / SEGMENTS, v0), (circ * (k + 1) / SEGMENTS, v0),
                            (circ * (k + 1) / SEGMENTS, v1), (circ * k / SEGMENTS, v1)])
        for end, ti, flip in ((0, 0.0, True), (1, 1.0, False)):  # caps with their own vertices
            base_ring = ring_start[0 if end == 0 else -1]
            centre_idx = len(verts)
            verts.append(tuple(h + d * (length * ti)))
            weights[centre_idx] = dict(weights[base_ring])
            cap_start = len(verts)
            for k in range(SEGMENTS):
                verts.append(verts[base_ring + k])
                weights[cap_start + k] = dict(weights[base_ring + k])
            for k in range(SEGMENTS):
                k2 = (k + 1) % SEGMENTS
                tri = (centre_idx, cap_start + k2, cap_start + k) if flip else (centre_idx, cap_start + k, cap_start + k2)
                faces.append(tri)
                mats.append(mat_idx)
                uvs.extend([(0.5, 0.5 + bone_index * 0.13)] * 3)
    obj = nb.mesh_object("HeroBody", verts, faces, uvs)
    mesh = obj.data
    mesh.materials.append(cloth_mat)
    mesh.materials.append(skin_mat)
    for p, m in zip(mesh.polygons, mats):
        p.material_index = m
    nb.set_smooth(obj, True)
    # Fix winding: make normals point outwards.
    nb.activate(obj)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.normals_make_consistent(inside=False)
    bpy.ops.object.mode_set(mode="OBJECT")
    groups = {n: obj.vertex_groups.new(name=n) for n, *_ in BONES}
    for vi, ws in weights.items():
        for bone, w in ws.items():
            groups[bone].add([vi], w, "REPLACE")
    mod = obj.modifiers.new("Armature", "ARMATURE")
    mod.object = arm
    obj.parent = arm
    return obj


def make_materials():
    cloth, nt, bsdf = nb.new_material("cloth")
    cloth.use_backface_culling = True
    base = nb.image_node(nt, nb.tex_cloth(), (-700, 250))
    nt.links.new(base.outputs["Color"], bsdf.inputs["Base Color"])
    nimg = nb.image_node(nt, nb.tex_cloth_normal(), (-700, -150))
    nmap = nt.nodes.new("ShaderNodeNormalMap")
    nmap.location = (-300, -150)
    nt.links.new(nimg.outputs["Color"], nmap.inputs["Color"])
    nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
    bsdf.inputs["Roughness"].default_value = 0.7
    skin, nt, bsdf = nb.new_material("skin")
    skin.use_backface_culling = True
    bsdf.inputs["Base Color"].default_value = (0.85, 0.62, 0.48, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.55
    return cloth, skin


# ---------------------------------------------------------------------------------------------
# Animation
# ---------------------------------------------------------------------------------------------

def rest_rot(arm, bone):
    return arm.data.bones[bone].matrix_local.to_3x3()


def rot_about(arm, bone, axis_armature, degrees):
    """Pose-bone quaternion rotating `bone` by `degrees` about an armature-space axis (parents unposed)."""
    local_axis = rest_rot(arm, bone).inverted() @ Vector(axis_armature)
    return Quaternion(local_axis.normalized(), math.radians(degrees))


def key_rot(arm, bone, frame, axis, degrees):
    pb = arm.pose.bones[bone]
    pb.rotation_mode = "QUATERNION"
    pb.rotation_quaternion = rot_about(arm, bone, axis, degrees)
    pb.keyframe_insert("rotation_quaternion", frame=frame)


def key_loc(arm, bone, frame, offset_armature):
    pb = arm.pose.bones[bone]
    pb.location = rest_rot(arm, bone).inverted() @ Vector(offset_armature)
    pb.keyframe_insert("location", frame=frame)


def set_interpolation(action, mode):
    for fc in iter_fcurves(action):
        for kp in fc.keyframe_points:
            kp.interpolation = mode
            if mode == "BEZIER":
                kp.handle_left_type = kp.handle_right_type = "AUTO_CLAMPED"


def iter_fcurves(action):
    # Blender 5 stores fcurves in layers/strips/channelbags; 4.x had action.fcurves.
    if hasattr(action, "fcurves"):
        yield from action.fcurves
        return
    for layer in action.layers:
        for strip in layer.strips:
            for bag in strip.channelbags:
                yield from bag.fcurves


X, Y, Z = (1, 0, 0), (0, 1, 0), (0, 0, 1)


def new_action(arm, name):
    act = bpy.data.actions.new(name)
    act.use_fake_user = True
    if arm.animation_data is None:
        arm.animation_data_create()
    arm.animation_data.action = act
    return act


def make_idle(arm):  # 2 s breathing loop, BEZIER
    act = new_action(arm, "idle")
    for f, c in ((0, 0.0), (30, 1.0), (60, 0.0)):
        key_rot(arm, "chest", f, X, 4.0 * c)
        key_rot(arm, "spine", f, X, -2.0 * c)
        key_rot(arm, "head", f, X, -3.0 * c)
        key_rot(arm, "upper_arm.L", f, (0, 1, 0), -6.0 * c)
        key_rot(arm, "upper_arm.R", f, (0, 1, 0), 6.0 * c)
    set_interpolation(act, "BEZIER")
    return act


def make_walk(arm):  # 1 s loop, LINEAR
    act = new_action(arm, "walk")
    swing = 32.0
    for f, s in ((0, 0.0), (7, 1.0), (15, 0.0), (22, -1.0), (30, 0.0)):
        key_rot(arm, "thigh.L", f, X, swing * s)
        key_rot(arm, "thigh.R", f, X, -swing * s)
        key_rot(arm, "shin.L", f, X, -max(0.0, -s) * 40.0)
        key_rot(arm, "shin.R", f, X, -max(0.0, s) * 40.0)
        key_rot(arm, "upper_arm.L", f, Z, -swing * s * 0.8)
        key_rot(arm, "upper_arm.R", f, Z, -swing * s * 0.8)
        key_rot(arm, "chest", f, Z, 6.0 * s)
        bob = 0.03 * (1.0 - abs(s))
        key_loc(arm, "hips", f, (0, 0, bob))
    set_interpolation(act, "LINEAR")
    return act


def make_wave(arm):  # right arm up, forearm waves, 1.6 s, BEZIER
    act = new_action(arm, "wave")
    for f, w in ((0, 0.0), (9, 1.0), (21, -1.0), (33, 1.0), (45, 0.0)):
        up = min(1.0, f / 9.0) if f < 39 else max(0.0, (45 - f) / 6.0)
        key_rot(arm, "upper_arm.R", f, Y, 85.0 * up)
        key_rot(arm, "forearm.R", f, Y, 25.0 * up - 25.0 * w * up)
        key_rot(arm, "hand.R", f, Z, 15.0 * w * up)
        key_rot(arm, "head", f, Z, 10.0 * up)
    set_interpolation(act, "BEZIER")
    return act


def make_snap(arm):  # stepped pose changes, CONSTANT
    act = new_action(arm, "snap")
    for f, a in ((0, 0.0), (10, 40.0), (20, -40.0), (30, 0.0)):
        key_rot(arm, "chest", f, Z, a)
        key_rot(arm, "head", f, Z, -a * 0.5)
        key_rot(arm, "upper_arm.L", f, X, a)
    set_interpolation(act, "CONSTANT")
    return act


def evaluate(arm, action, frames, bones):
    """Blender's own evaluation: armature-space pose-bone head positions in glTF axes."""
    arm.animation_data.action = action
    out = {}
    for f in frames:
        for pb in arm.pose.bones:  # channels the action does not animate stay at rest (as in the engine)
            pb.location = (0, 0, 0)
            pb.rotation_quaternion = (1, 0, 0, 0)
        bpy.context.scene.frame_set(f)
        bpy.context.view_layer.update()
        out[str(f)] = {b: nb.to_gltf_axes(arm.pose.bones[b].head) for b in bones}
    return out


def main():
    out = nb.script_args()
    nb.reset_scene()
    cloth, skin = make_materials()
    arm = build_armature()
    body = build_mesh(arm, cloth, skin)
    body.data.name = "HeroBody"
    nb.activate(arm)
    bpy.ops.object.mode_set(mode="POSE")
    actions = [make_idle(arm), make_walk(arm), make_wave(arm), make_snap(arm)]
    bpy.ops.object.mode_set(mode="OBJECT")
    for pb in arm.pose.bones:  # back to the rest pose before sampling/exporting
        pb.location = (0, 0, 0)
        pb.rotation_quaternion = (1, 0, 0, 0)

    probes = ["hips", "thigh.L", "foot.L", "toe.R", "upper_arm.R", "forearm.R", "hand.R", "head", "chest"]
    frames = {"idle": [0, 30, 45], "walk": [0, 4, 7, 11, 22], "wave": [0, 15, 27, 45], "snap": [0, 5, 10, 15, 20, 29]}
    expected = {
        "units": "metres, bone heads in glTF axes (x, z, -y) of Blender armature space",
        "frame_rate": FPS,
        "rest_heads": {n: nb.to_gltf_axes(h) for n, _, h, _, _ in BONES},
        "bone_count": len(BONES),
        "height": 1.82,
        "forward_probe": {"bone": "toe.L", "head": nb.to_gltf_axes(Vector((0.10, -0.12, 0.04)))},
        "actions": {},
    }
    for act in actions:
        expected["actions"][act.name] = {
            "first_frame": int(act.frame_range[0]), "last_frame": int(act.frame_range[1]),
            "samples": evaluate(arm, act, frames[act.name], probes),
        }
    arm.animation_data.action = None
    bpy.context.scene.frame_set(1)
    bpy.context.view_layer.update()

    d = os.path.join(out, "models", "blender")
    nb.export_gltf(d, "hero")
    nb.write_json(os.path.join(d, "hero.expected.json"), expected)
    print("character exported to", d)


if __name__ == "__main__":
    main()
