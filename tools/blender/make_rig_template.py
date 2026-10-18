"""Builds the canonical rig template and the face proof head (docs/rigging_plan.md §4, §5, §13.2 P1a).

    uv run --offline --python 3.11 --with bpy==5.0.1 python -I tools/blender/make_rig_template.py [--out assets]
        [--blend /path/template.blend]

Writes:
  <out>/rig/canonical_humanoid.json      the canonical skeleton, ROM poses, thresholds (rig_spec.py)
  <out>/rig/face_presets.json            the shape set, 15 visemes, 14 expressions (face_spec.py)
  <out>/models/rig/proof_head.glb        the proof head on the canonical skeleton (rig export profile)
  <out>/models/rig/proof_head.expected.json   Blender's evaluated shapes, for the engine's CPU-morph test
  <out>/models/rig/proof_head.rigexpect.json  joints + skinned vertices at rest and 12 ROM keys (ne_rig_check)
and optionally a .blend of the template (armature, proof head, `rom` / `tpose` / face actions; not committed),
which is what tools/blender/rig_validate.py reads for the full report.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bpy  # noqa: E402
import numpy as np  # noqa: E402
from mathutils import Euler  # noqa: E402

import face_spec as fs  # noqa: E402
import ne_blender as nb  # noqa: E402
import proof_head as ph  # noqa: E402
import rig_common as rc  # noqa: E402
import rig_spec as rs  # noqa: E402


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    out, blend = "assets", None
    for i, a in enumerate(argv):
        if a == "--out":
            out = argv[i + 1]
        if a == "--blend":
            blend = argv[i + 1]
    return out, blend


def make_materials():
    mats = []
    for name in ph.MATERIALS:
        mat, nt, bsdf = nb.new_material("ph_" + name)
        r, g, b = ph.MAT_COLOR[name]
        bsdf.inputs["Base Color"].default_value = (r, g, b, 1.0)
        bsdf.inputs["Roughness"].default_value = 0.55 if name not in ("sclera", "pupil", "iris") else 0.2
        bsdf.inputs["Metallic"].default_value = 0.0
        mat.use_backface_culling = False
        mats.append(mat)
    return mats


def build_head_object(arm, B, shapes, masks):
    verts = [tuple(float(c) for c in p) for p in B.v]
    faces = [f for f, _ in B.faces]
    obj = nb.mesh_object("proof_head", verts, faces)
    me = obj.data
    for m in make_materials():
        me.materials.append(m)
    me.polygons.foreach_set("material_index", [m for _, m in B.faces])
    me.polygons.foreach_set("use_smooth", [True] * len(faces))
    me.update()
    # skin weights + region masks
    groups = {}
    for i, ws in enumerate(B.weights):
        for bone, w in ws.items():
            if bone not in groups:
                groups[bone] = obj.vertex_groups.new(name=bone)
            groups[bone].add([i], float(w), "REPLACE")
    for name, m in masks.items():
        g = obj.vertex_groups.new(name="rgn_" + name)
        g.add([int(i) for i in np.nonzero(m)[0]], 1.0, "REPLACE")
    # shape keys in canonical order
    obj.shape_key_add(name="Basis", from_mix=False)
    base = np.array(B.v)
    for name in fs.ALL_SHAPES:
        kb = obj.shape_key_add(name=name, from_mix=False)
        kb.data.foreach_set("co", (base + shapes[name]).astype(np.float32).ravel())
        kb.slider_min, kb.slider_max = 0.0, 1.0
    me.update()
    mod = obj.modifiers.new("Armature", "ARMATURE")
    mod.object = arm
    mod.use_deform_preserve_volume = False
    obj.parent = arm
    obj["ne_face_shape_set"] = "face_presets.json v%d" % fs.VERSION
    return obj


def make_face_clips(arm, head):
    """Two tiny clips so the loader's weights-channel path has real data: a jaw-bone clip and a
    shape-key blink clip (exported as glTF `weights` channels)."""
    act = rc.new_action(arm, "face_jaw")
    for f, deg in ((0, 0.0), (6, 20.0), (12, 0.0)):
        rc.reset_pose(arm)
        rc.set_pose(arm, {"jaw": (deg, 0.0, 0.0)})
        arm.pose.bones["jaw"].keyframe_insert("rotation_quaternion", frame=f)
    rc.reset_pose(arm)
    keys = head.data.shape_keys
    keys.animation_data_create()
    act2 = bpy.data.actions.new("face_blink")
    act2.use_fake_user = True
    keys.animation_data.action = act2
    for f, w in ((0, 0.0), (3, 1.0), (8, 0.0)):
        for n in ("eyeBlinkLeft", "eyeBlinkRight"):
            kb = keys.key_blocks[n]
            kb.value = w
            kb.keyframe_insert("value", frame=f)
    for kb in keys.key_blocks:
        kb.value = 0.0
    rc.bake_linear(act)  # LINEAR, not Bezier: UE Interchange drops CUBICSPLINE samplers
    rc.bake_linear(act2)
    return act, act2


def unique_vertices(P, eps=1e-6):
    """Indices whose position no other vertex shares (lip-line copies are excluded)."""
    q = np.round(P / eps).astype(np.int64)
    _, inv, counts = np.unique(q, axis=0, return_inverse=True, return_counts=True)
    return np.nonzero(counts[inv.ravel()] == 1)[0]


def evaluate_case(arm, head, weights, pose):
    for kb in head.data.shape_keys.key_blocks:
        kb.value = 0.0
    for n, w in weights.items():
        head.data.shape_keys.key_blocks[n].value = float(w)
    rc.reset_pose(arm)
    rc.set_pose(arm, {j: (deg, 0.0, 0.0) for j, deg in pose.items()})
    bpy.context.view_layer.update()
    return np.array([tuple(v) for v in rc.evaluated_positions(head)])


def make_expected(arm, head, B, shapes):
    P = np.array(B.v)
    uniq = set(unique_vertices(P).tolist())
    sample = []
    for name in fs.ALL_SHAPES:
        mag = np.linalg.norm(shapes[name], axis=1)
        order = [i for i in np.argsort(-mag) if i in uniq and mag[i] > 0][:2]
        sample += [int(i) for i in order]
    wj = np.array([w.get("jaw", 0.0) for w in B.weights])
    sample += [int(i) for i in np.argsort(-(wj * (np.abs(P[:, 0]) < 0.02))) if i in uniq][:4]
    rng = np.random.default_rng(7)
    sample += [int(i) for i in rng.choice(sorted(uniq), 12, replace=False)]
    sample = sorted(set(sample))
    cases = [(n, {n: 1.0}, {}) for n in fs.ALL_SHAPES]
    for v in fs.VISEME_ORDER:
        cases.append(("viseme_" + v, dict(fs.VISEMES[v]["shapes"]), {}))
    for e, v in (("warm", "aa"), ("warm", "PP"), ("angry", "ou"), ("sad", "E")):
        w = fs.layer_weights(fs.EXPRESSIONS[e], fs.VISEMES[v]["shapes"], 1.0)
        cases.append(("layer_%s_%s" % (e, v), {k: x for k, x in w.items() if x > 0}, {}))
    aa = fs.VISEMES["aa"]
    jaw_deg = aa["shapes"]["jawOpen"] * fs.JAW_MAX_DEG
    cases.append(("pose_aa_jaw", dict(aa["shapes"]), {"jaw": jaw_deg, "tongue_01": float(aa["tongue"][0])}))
    cases.append(("pose_blink_jaw_only", {"eyeBlinkLeft": 1.0}, {"jaw": 10.0}))

    rest = evaluate_case(arm, head, {}, {})
    assert np.abs(rest - P).max() < 1e-6, "evaluated rest differs from the base mesh"
    out_cases = []
    worst = 0.0
    for name, weights, pose in cases:
        ev = evaluate_case(arm, head, weights, pose)
        if not pose:  # self-check: Blender's evaluation equals base + sum(w * delta)
            ref = P + sum(w * shapes[n] for n, w in weights.items())
            worst = max(worst, float(np.abs(ev - ref).max()))
        moved = [[k] + nb.to_gltf_axes(ev[i]) for k, i in enumerate(sample) if np.linalg.norm(ev[i] - P[i]) > 1e-7]
        out_cases.append({"name": name, "weights": {k: round(float(x), 6) for k, x in weights.items()},
                          "pose_x_deg": pose, "moved": [[m[0]] + [round(c, 7) for c in m[1:]] for m in moved]})
    assert worst < 1e-6, "Blender shape evaluation differs from the reference sum by %g" % worst
    rc.reset_pose(arm)
    for kb in head.data.shape_keys.key_blocks:
        kb.value = 0.0
    bpy.context.view_layer.update()
    return {
        "units": "metres, glTF axes (x, z, -y of Blender); vertex positions in the mesh's space",
        "about": "Blender-evaluated proof head (shape keys, then LBS with Preserve Volume off). `pose_x_deg` "
                 "rotates each joint about its local +X from rest; the engine composes rest * angleAxis.",
        "shape_names": fs.ALL_SHAPES,
        "tolerance_m": 1e-4,
        "samples": [nb.to_gltf_axes(P[i]) for i in sample],
        "cases": out_cases,
        "vertex_count_blender": len(P),
    }


def check_glb(path, n_shapes):
    doc = nb.read_glb_json(path)
    meshes = doc["meshes"]
    assert len(meshes) == 1, "expected one mesh"
    m = meshes[0]
    names = m.get("extras", {}).get("targetNames")
    assert names == fs.ALL_SHAPES, "targetNames missing or out of order: %r" % (names,)
    for p in m["primitives"]:
        assert len(p.get("targets", [])) == n_shapes, "primitive without %d targets" % n_shapes
    joints = [doc["nodes"][j]["name"] for j in doc["skins"][0]["joints"]]
    assert joints == rs.JOINT_NAMES, "skin joint order differs from the canonical order"
    anims = sorted(a["name"] for a in doc.get("animations", []))
    paths = sorted({c["target"]["path"] for a in doc.get("animations", []) for c in a["channels"]})
    interp = {s.get("interpolation", "LINEAR") for a in doc.get("animations", []) for s in a["samplers"]}
    assert interp == {"LINEAR"}, "non-LINEAR samplers: %r" % sorted(interp)
    return {"primitives": len(m["primitives"]), "joints": len(joints), "animations": anims, "paths": paths}


def main():
    out, blend = _args()
    rs.write_template(os.path.join(out, "rig", "canonical_humanoid.json"))
    fs.write_presets(os.path.join(out, "rig", "face_presets.json"))

    nb.reset_scene()
    arm = rc.build_canonical_armature("canonical")
    # hinge check (R12): the built rolls match the template hints
    for name, _p, h, t, xa, _g, _d in rs.JOINTS:
        y = np.array(t) - np.array(h)
        y /= np.linalg.norm(y)
        x = np.array(xa) - y * np.dot(xa, y)
        x /= np.linalg.norm(x)
        got = np.array(rc.bone_x_axis(arm, name))
        assert np.degrees(np.arccos(np.clip(np.dot(got, x), -1, 1))) < 0.5, name

    B = ph.build()
    shapes, masks = ph.all_shapes(B)
    head = build_head_object(arm, B, shapes, masks)
    expected = make_expected(arm, head, B, shapes)
    make_face_clips(arm, head)
    # The body clips go into the proof head too, so ne_rig_check can sample `rom` (R13). They key the
    # body joints only (never jaw / tongue / eyes) and stay unsampled: linear keys, about 40 KB.
    rc.make_rom_action(arm)
    rc.make_tpose_action(arm)
    arm.animation_data.action = None
    rc.reset_pose(arm)
    bpy.context.view_layer.update()

    d = os.path.join(out, "models", "rig")
    # The proof head drops NORMAL deltas (they are ~65% of the file: every vertex next to a moved one
    # gets one) and per-frame sampling (its two clips are plain keys, no constraints). The loader's
    # NORMAL-target path is covered by a synthetic glTF in tests/test_morph.cpp instead.
    glb = nb.export_rig(os.path.join(d, "proof_head.glb"),
                        {"export_morph_normal": False, "export_force_sampling": False})
    info = check_glb(glb, len(fs.ALL_SHAPES))
    expected["export"] = info
    nb.write_json(os.path.join(d, "proof_head.expected.json"), expected)
    rx = rc.write_rigexpect(arm, head, os.path.join(d, "proof_head.rigexpect.json"))
    print("rigexpect:", len(rx["frames"]), "frames,", len(rx["samples"]), "sample vertices")
    print("proof head:", len(B.v), "verts,", len(fs.ALL_SHAPES), "shapes,", info, os.path.getsize(glb), "bytes")

    if blend:
        arm.animation_data.action = None
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(blend))
        print("template saved to", blend)


if __name__ == "__main__":
    main()
