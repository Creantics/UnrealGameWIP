"""Fits the canonical skeleton to an unrigged mesh and skins it (docs/rigging_plan.md §9.2 path 1, §13.2).

    uv run --offline --python 3.11 --with bpy==5.0.1 python -I tools/blender/fit_canonical.py MESH \\
        (--landmarks landmarks.json | --landmarks-from service_rig.glb --map retarget_maps/<source>.json) \\
        --out character.blend [--export character.glb] [--height 1.62] [--no-symmetrize] [--no-mirror-weights]

    ... fit_canonical.py --selftest --out DIR      # a mannequin on a re-proportioned skeleton, end to end

MESH is .glb / .gltf / .obj / .fbx / .blend; every mesh in it is skinned (armatures and old skinning in it
are dropped: a service skeleton is never shipped). Landmarks are canonical joint heads in Blender axes
(metres, facing -Y, soles on z = 0): the 14 in rig_fit.REQUIRED, from 14 clicks or from a service rig
(AccuRIG for humanoids, UniRig for creatures) through a map {canonical_joint: source_bone}. Extra landmarks
pin their joints exactly.

Steps: fit (rig_fit.fit_joints; left/right averaged unless --no-symmetrize) -> canonical armature with
rolls from the carried x_axis hints (§4.2) -> heat-map weights (rig_common.auto_weights) -> weight post-pass
(twist bands, mirror, prune < 0.01, limit 4, normalise) -> `rom` + `tpose` actions -> .blend (and .glb with
the rig profile + .rigexpect.json for ne_rig_check). Then run rig_validate.py on the .blend; only a PASS
report is accepted (§10).
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bpy  # noqa: E402
import numpy as np  # noqa: E402

import ne_blender as nb  # noqa: E402
import rig_common as rc  # noqa: E402
import rig_fit  # noqa: E402
import rig_spec as rs  # noqa: E402


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    a = {"mesh": None, "landmarks": None, "from": None, "map": None, "out": None, "export": None, "height": None,
         "symmetrize": True, "mirror": True, "selftest": False}
    i = 0
    while i < len(argv):
        k = argv[i]
        if k in ("--landmarks", "--landmarks-from", "--map", "--out", "--export", "--height"):
            a[{"--landmarks-from": "from"}.get(k, k[2:])] = argv[i + 1]
            i += 2
            continue
        if k == "--no-symmetrize":
            a["symmetrize"] = False
        elif k == "--no-mirror-weights":
            a["mirror"] = False
        elif k == "--selftest":
            a["selftest"] = True
        elif not k.startswith("--") and a["mesh"] is None:
            a["mesh"] = k
        else:
            raise SystemExit("fit_canonical: unknown argument %s" % k)
        i += 1
    if not a["out"] or not (a["selftest"] or (a["mesh"] and (a["landmarks"] or (a["from"] and a["map"])))):
        raise SystemExit(__doc__)
    return a


# --------------------------------------------------------------------------------------------
# Input
# --------------------------------------------------------------------------------------------
def import_meshes(path):
    """Loads MESH into an empty scene; returns its mesh objects with transforms applied and any old
    skinning (armature modifiers, armatures, vertex groups, parenting) removed."""
    ext = os.path.splitext(path)[1].lower()
    if ext == ".blend":
        bpy.ops.wm.open_mainfile(filepath=os.path.abspath(path))
    else:
        nb.reset_scene()
        if ext in (".glb", ".gltf"):
            bpy.ops.import_scene.gltf(filepath=os.path.abspath(path))
        elif ext == ".obj":
            bpy.ops.wm.obj_import(filepath=os.path.abspath(path))
        elif ext == ".fbx":
            bpy.ops.import_scene.fbx(filepath=os.path.abspath(path))
        else:
            raise SystemExit("fit_canonical: unsupported mesh format %s" % ext)
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    for o in meshes:
        mw = o.matrix_world.copy()
        o.parent = None
        o.matrix_world = mw
        for m in [m for m in o.modifiers if m.type == "ARMATURE"]:
            o.modifiers.remove(m)
        o.vertex_groups.clear()
    for o in [o for o in bpy.data.objects if o.type in ("ARMATURE", "EMPTY")]:
        bpy.data.objects.remove(o, do_unlink=True)
    for o in bpy.context.selected_objects:
        o.select_set(False)
    for o in meshes:
        o.select_set(True)
    if meshes:
        bpy.context.view_layer.objects.active = meshes[0]
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    return meshes


def landmarks_from_rig(path, mapping):
    """Joint heads (world, Blender axes) of a service rig's bones, renamed through {canonical: source}."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    if path.lower().endswith(".fbx"):
        bpy.ops.import_scene.fbx(filepath=os.path.abspath(path))
    else:
        bpy.ops.import_scene.gltf(filepath=os.path.abspath(path))
    arms = [o for o in bpy.data.objects if o.type == "ARMATURE"]
    if len(arms) != 1:
        raise SystemExit("fit_canonical: %s has %d armatures (want 1)" % (path, len(arms)))
    arm = arms[0]
    out = {}
    for canon, src in mapping.items():
        if canon.startswith("_"):
            continue
        b = arm.data.bones.get(src)
        if b is not None:
            out[canon] = list(arm.matrix_world @ b.head_local)
    return out


# --------------------------------------------------------------------------------------------
# Fit
# --------------------------------------------------------------------------------------------
def build_fitted_armature(rows, height):
    arm = rc.build_canonical_armature("canonical", rows=rows)
    arm["ne_x_hints"] = json.dumps({r[0]: [round(c, 6) for c in r[4]] for r in rows})
    arm["ne_height_m"] = float(height)
    return arm


def fit(meshes, landmarks, symmetrize=True, mirror=True, height=None):
    zs = np.concatenate([[(o.matrix_world @ v.co).z for v in o.data.vertices] for o in meshes])
    floor, top = float(zs.min()), float(zs.max())
    if symmetrize:
        landmarks = rig_fit.symmetrize(landmarks)
    rows, info = rig_fit.fit_joints(landmarks, floor_z=floor)
    arm = build_fitted_armature(rows, height or (top - floor))
    report = {"landmarks": sorted(landmarks), "landmark_residual_m": info["landmark_residual_m"],
              "floor_z": floor, "height_m": top - floor, "meshes": {}}
    for o in meshes:
        empty = rc.auto_weights(o, arm)
        stats = rc.weight_post_pass(o, arm, mirror=mirror)
        stats["heat_fallback_vertices"] = empty
        report["meshes"][o.name] = stats
    rc.make_rom_action(arm)
    rc.make_tpose_action(arm)
    arm.animation_data.action = None
    rc.reset_pose(arm)
    bpy.context.view_layer.update()
    return arm, report


def save_outputs(arm, meshes, out_blend, export=None, report=None):
    os.makedirs(os.path.dirname(os.path.abspath(out_blend)), exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(out_blend))
    if export:
        rc.export_rig_glb(export)
        biggest = max(meshes, key=lambda o: len(o.data.vertices))
        rc.write_rigexpect(arm, biggest, os.path.splitext(export)[0] + ".rigexpect.json")
    if report is not None:
        with open(os.path.splitext(out_blend)[0] + ".fit.json", "w") as f:
            json.dump(report, f, indent=1)


# --------------------------------------------------------------------------------------------
# Self-test mannequin: capsules around the main bones of a joint table (also used by bake_to_canonical)
# --------------------------------------------------------------------------------------------
MANNEQUIN = [  # (from joint head, to joint head or tail "name.tail", radius x, radius y)
    ("pelvis", "neck_01", 0.13, 0.09), ("neck_01", "head", 0.05, 0.05), ("head", "head.tail", 0.085, 0.10),
    ("upperarm_l", "lowerarm_l", 0.045, 0.045), ("lowerarm_l", "hand_l", 0.037, 0.037),
    ("hand_l", "middle_03_l.tail", 0.03, 0.03),
    ("thigh_l", "calf_l", 0.075, 0.075), ("calf_l", "foot_l", 0.055, 0.055), ("foot_l", "ball_l.tail", 0.04, 0.04),
]


def _capsule(a, b, rx, ry, n_around=16, n_len=8, n_cap=4):
    a, b = np.asarray(a, float), np.asarray(b, float)
    ax = b - a
    L = float(np.linalg.norm(ax))
    w = ax / L
    e1 = np.array([1.0, 0.0, 0.0]) - w * w[0]
    if np.linalg.norm(e1) < 1e-6:
        e1 = np.array([0.0, 1.0, 0.0]) - w * w[1]
    e1 /= np.linalg.norm(e1)
    e2 = np.cross(w, e1)
    if e2[1] < 0 or (abs(e2[1]) < 1e-9 and e2[2] < 0):
        e2 = -e2  # a frame that mirrors with x -> -x
    rings = []
    for k in range(n_cap - 1, 0, -1):  # head cap (the pole is one vertex, not a ring of radius 0)
        t = math.pi / 2 * k / n_cap
        rings.append((-math.sin(t), math.cos(t)))
    for k in range(n_len + 1):
        rings.append((k / n_len, 1.0))
    for k in range(1, n_cap):
        t = math.pi / 2 * k / n_cap
        rings.append((1.0 + math.sin(t), math.cos(t)))
    V, F = [], []
    r_end = min(rx, ry)
    for s, rad in rings:
        if s < 0:
            c = a + w * (s * r_end)
        elif s > 1:
            c = b + w * ((s - 1.0) * r_end)
        else:
            c = a + w * (s * L)
        for j in range(n_around):
            th = 2 * math.pi * j / n_around
            V.append(c + rad * (rx * math.cos(th) * e1 + ry * math.sin(th) * e2))
    V = [a - w * r_end] + V + [b + w * r_end]
    nr = len(rings)
    for j in range(n_around):
        j2 = (j + 1) % n_around
        F.append((0, 1 + j2, 1 + j))
        for i in range(nr - 1):
            p, q = 1 + i * n_around, 1 + (i + 1) * n_around
            F.append((p + j, p + j2, q + j2, q + j))
        last = 1 + (nr - 1) * n_around
        F.append((last + j, last + j2, len(V) - 1))
    return V, F


def make_mannequin(joints, name="mannequin"):
    """joints: {canonical name: (head, tail)}. Left limbs are built and mirrored exactly (R6)."""
    def pt(key):
        if key.endswith(".tail"):
            return np.asarray(joints[key[:-5]][1], float)
        return np.asarray(joints[key][0], float)

    V, F = [], []
    for a, b, rx, ry in MANNEQUIN:
        if a.startswith("foot"):  # thick enough to reach below the floor, then flattened onto it
            rx = ry = max(rx, min(pt(a)[2], pt(b)[2]) + 0.004)
        va, fa = _capsule(pt(a), pt(b), rx, ry)
        o = len(V)
        V += va
        F += [tuple(i + o for i in f) for f in fa]
        if a.endswith("_l"):
            o = len(V)
            V += [p * np.array([-1.0, 1.0, 1.0]) for p in va]
            F += [tuple(i + o for i in reversed(f)) for f in fa]
    P = np.array(V)
    P[:, 2] = np.maximum(P[:, 2], 0.0)  # soles flat on the floor
    obj = nb.mesh_object(name, [tuple(map(float, p)) for p in P], F)
    nb.set_smooth(obj, True)
    return obj


def _reproportion(rows):
    """A different character: 0.92 x height, 6 % longer arms, shoulders 1.5 cm wider, legs 3 % shorter."""
    out = {}
    for n, p in rig_fit.template_landmarks(rs.JOINT_NAMES).items():
        out[n] = p * np.array([1.0, 1.0, 0.92])
    for side, sx in (("_l", 1.0), ("_r", -1.0)):
        sh = out["upperarm" + side] + np.array([0.015 * sx, 0.0, 0.0])
        for n in ("lowerarm", "hand"):
            out[n + side] = sh + 1.06 * (out[n + side] - out["upperarm" + side])
        out["upperarm" + side] = sh
        hip = out["thigh" + side]
        for n in ("calf", "foot"):
            out[n + side] = hip + 0.97 * (out[n + side] - hip)
    return {n: out[n] for n in rig_fit.REQUIRED + ["head"]}


def selftest(a):
    nb.reset_scene()
    lm = _reproportion(rs.JOINTS)
    truth, _ = rig_fit.fit_joints(lm, floor_z=0.0)
    mesh = make_mannequin({r[0]: (r[2], r[3]) for r in truth})
    # landmarks as a person would click them: off by up to 4 mm, not symmetric
    rng = np.random.default_rng(5)
    clicked = {n: list(np.asarray(p) + rng.uniform(-0.004, 0.004, 3)) for n, p in lm.items()}
    arm, report = fit([mesh], clicked, symmetrize=a["symmetrize"], mirror=a["mirror"])
    got = {b.name: np.array(b.head_local) for b in arm.data.bones}
    err = max(float(np.linalg.norm(got[r[0]] - np.asarray(r[2]))) for r in truth)
    report["selftest"] = {"joint_error_vs_truth_max_m": err, "click_noise_m": 0.004}
    out = os.path.join(a["out"], "fit_selftest.blend")
    save_outputs(arm, [mesh], out, export=os.path.join(a["out"], "fit_selftest.glb"), report=report)
    print(json.dumps(report, indent=1))
    print("fit selftest: worst joint error vs the true skeleton %.2f mm (clicks off by <= 4 mm)" % (err * 1000))
    print("validate: uv run --offline --python 3.11 --with bpy==5.0.1 python -I tools/blender/rig_validate.py %s "
          "--template assets/rig/canonical_humanoid.json --out %s" % (out, a["out"]))


def main():
    a = _args()
    if a["selftest"]:
        selftest(a)
        return
    if a["landmarks"]:
        with open(a["landmarks"]) as f:
            landmarks = json.load(f)
    else:
        with open(a["map"]) as f:
            landmarks = landmarks_from_rig(a["from"], json.load(f))
    meshes = import_meshes(a["mesh"])
    if not meshes:
        raise SystemExit("fit_canonical: no meshes in %s" % a["mesh"])
    arm, report = fit(meshes, landmarks, a["symmetrize"], a["mirror"],
                      float(a["height"]) if a["height"] else None)
    save_outputs(arm, meshes, a["out"], a["export"], report)
    print(json.dumps(report, indent=1))


if __name__ == "__main__":
    main()
