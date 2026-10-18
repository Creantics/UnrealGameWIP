"""Bakes a character authored on a control rig (Rigify) onto the canonical skeleton (docs/rigging_plan.md
§9.2 path 3, §13.2). The control-rig .blend stays the source; this writes the export file.

    uv run --offline --python 3.11 --with bpy==5.0.1 python -I tools/blender/bake_to_canonical.py SOURCE.blend \\
        --out character.blend [--export character.glb] [--map tools/blender/retarget_maps/rigify.json] \\
        [--rig RIG_OBJECT] [--actions a,b,c] [--mirror-weights]

    ... bake_to_canonical.py --selftest --out DIR   # Rigify human metarig + mannequin, end to end, measured

What it does:
 1. Fits the canonical skeleton to the control rig's deform bones (rig_fit.fit_joints with every mapped
    bone head as a landmark, except the map's `_no_landmark` joints), rolls from the carried hints (§4.2).
 2. Each canonical joint FOLLOWS its mapped bone rigidly: at every frame its armature-space matrix is
    M_bone(f) * M_bone(rest)^-1 * M_joint(rest). Because linear blend skinning only sees M * M_rest^-1,
    a vertex weighted to that bone deforms exactly as before, wherever the joint sits. Unmapped joints
    (helpers, sockets) follow their parent.
 3. Vertex groups are renamed through the map; groups of unlisted bones (face detail, palms, breasts,
    pelvis wings) go to the nearest listed ancestor (DEF-/ORG-/MCH- prefixes looked through). Then the
    post-pass: prune < 0.01, limit 4, normalise (no twist re-banding: the control rig's twist split is
    kept). Weights are NOT mirrored by default: a bake must reproduce the authored deformation, and mirror
    averaging changes it wherever the source is not exactly symmetric (the self-test measured 50 mm on a
    Rigify mannequin). --mirror-weights opts in; the bake then evaluates every baked frame with and without
    the mirror pass and fails if mirroring moved any vertex more than MIRROR_ERROR_MAX_MM (0.1 mm).
 4. Every action of the control rig is baked onto the canonical armature, every frame, rotation keys on
    all joints and translation keys only on root / pelvis (§11, ACT.translation). The `rom` and `tpose`
    test clips are added for the validator. The control rig is removed (no constraints, no drivers,
    one armature: R15).
Expected losses, which --selftest measures: B-bone curvature (the engine has none: a 10-segment Rigify
B-bone becomes one rigid joint), stretch / scale (dropped), merged groups and the 4-influence limit.
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bpy  # noqa: E402
import numpy as np  # noqa: E402
from mathutils import Euler, Matrix, Quaternion  # noqa: E402

import fit_canonical as fc  # noqa: E402
import ne_blender as nb  # noqa: E402
import rig_common as rc  # noqa: E402
import rig_fit  # noqa: E402
import rig_spec as rs  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_MAP = os.path.join(HERE, "retarget_maps", "rigify.json")
MIRROR_ERROR_MAX_MM = 0.1  # --mirror-weights may not move any vertex further than this on any baked frame


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    a = {"source": None, "out": None, "export": None, "map": DEFAULT_MAP, "rig": None, "actions": None,
         "mirror": False, "selftest": False}
    i = 0
    while i < len(argv):
        k = argv[i]
        if k in ("--out", "--export", "--map", "--rig", "--actions"):
            a[k[2:]] = argv[i + 1]
            i += 2
            continue
        if k == "--mirror-weights":
            a["mirror"] = True
        elif k == "--no-mirror-weights":  # the default; accepted for old command lines
            a["mirror"] = False
        elif k == "--selftest":
            a["selftest"] = True
        elif not k.startswith("--") and a["source"] is None:
            a["source"] = k
        else:
            raise SystemExit("bake_to_canonical: unknown argument %s" % k)
        i += 1
    if not a["out"] or not (a["selftest"] or a["source"]):
        raise SystemExit(__doc__)
    return a


def load_map(path):
    """-> ({canonical: source bone} for both sides, set of joints that are not landmarks)."""
    with open(path) as f:
        raw = json.load(f)
    no_lm = set()
    for n in raw.get("_no_landmark", []):
        no_lm |= {n, n + "_l", n + "_r"} if n not in rs.JOINT_NAMES else {n}
    m = {}
    for canon, bone in raw.items():
        if canon.startswith("_"):
            continue
        m[canon] = bone
        if canon.endswith("_l"):
            m[canon[:-2] + "_r"] = bone[:-2] + ".R" if bone.endswith(".L") else bone.replace(".L.", ".R.")
    return m, no_lm


def _strip(name):
    for p in ("DEF-", "ORG-", "MCH-"):
        if name.startswith(p):
            return name[len(p):]
    return name


# --------------------------------------------------------------------------------------------
# 1. Fit
# --------------------------------------------------------------------------------------------
def fit_to_rig(rig, mapping, no_lm, height):
    mw = rig.matrix_world
    lm = {}
    for canon, bone in mapping.items():
        if canon in no_lm or canon == "root":
            continue
        b = rig.data.bones.get(bone)
        if b is not None:
            lm[canon] = list(mw @ b.head_local)
    rows, info = rig_fit.fit_joints(rig_fit.symmetrize(lm), floor_z=0.0)
    arm = fc.build_fitted_armature(rows, height)
    arm.name = "canonical"
    return arm, info


# --------------------------------------------------------------------------------------------
# 3. Weights
# --------------------------------------------------------------------------------------------
def transfer_groups(obj, rig, mapping, arm, mirror=True):
    """Renames / merges the control rig's groups onto canonical joints; -> {source group: canonical}."""
    by_bone = {}
    for canon, bone in mapping.items():  # first listed canonical joint owns a bone's weights
        by_bone.setdefault(bone, canon)
    by_stripped = {_strip(b): c for b, c in by_bone.items()}

    def target(gname):
        b = rig.data.bones.get(gname)
        name = gname
        while True:
            if name in by_bone:
                return by_bone[name]
            if _strip(name) in by_stripped:
                return by_stripped[_strip(name)]
            if b is None or b.parent is None:
                return "root" if b is not None else None
            b = b.parent
            name = b.name

    deform = {b.name for b in rig.data.bones if b.use_deform}
    route = {g.name: target(g.name) for g in obj.vertex_groups if g.name in deform}
    W = {}
    gname = {g.index: g.name for g in obj.vertex_groups}
    for v in obj.data.vertices:
        acc = {}
        for g in v.groups:
            t = route.get(gname.get(g.group))
            if t:
                acc[t] = acc.get(t, 0.0) + g.weight
        W[v.index] = acc
    obj.vertex_groups.clear()
    for b in arm.data.bones:
        if b.use_deform:
            obj.vertex_groups.new(name=b.name)
    for i, acc in W.items():
        for t, w in acc.items():
            obj.vertex_groups[t].add([i], w, "REPLACE")
    rc.bind_mesh(obj, arm)
    twin = None
    if mirror:  # the same weights without the mirror pass, kept to measure what mirroring changes
        twin = obj.copy()
        twin.data = obj.data.copy()
        twin.name = obj.name + ".nomirror"
        bpy.context.scene.collection.objects.link(twin)
        rc.weight_post_pass(twin, arm, mirror=False, twist=False)
    stats = rc.weight_post_pass(obj, arm, mirror=mirror, twist=False)
    if twin is not None:
        stats["_twin"] = twin
    stats["groups_routed"] = {k: v for k, v in sorted(route.items()) if v is not None and k not in by_bone}
    return stats


# --------------------------------------------------------------------------------------------
# 2 + 4. Follow and bake
# --------------------------------------------------------------------------------------------
def bake_action(rig, arm, mapping, action, frames=None):
    """Bakes `action` (on `rig`) onto `arm` as a new action of the same name. -> the new action."""
    rig.animation_data.action = action
    f0, f1 = (int(math.floor(action.frame_range[0])), int(math.ceil(action.frame_range[1])))
    frames = frames or list(range(f0, f1 + 1))
    rest_c = {b.name: b.matrix_local.copy() for b in arm.data.bones}
    rest_d = {b.name: b.matrix_local.copy() for b in rig.data.bones}
    rig_to_arm = arm.matrix_world.inverted() @ rig.matrix_world
    names = [b.name for b in arm.data.bones]  # canonical order: parents first
    parent = {b.name: (b.parent.name if b.parent else None) for b in arm.data.bones}
    samples = []
    for f in frames:
        bpy.context.scene.frame_set(f)
        bpy.context.view_layer.update()
        M = {}
        for n in names:
            src = mapping.get(n)
            pb = rig.pose.bones.get(src) if src else None
            if pb is not None:
                M[n] = rig_to_arm @ pb.matrix @ rest_d[src].inverted() @ rig_to_arm.inverted() @ rest_c[n]
            else:
                p = parent[n]
                M[n] = (M[p] @ rest_c[p].inverted() @ rest_c[n]) if p else rest_c[n].copy()
        basis = {}
        for n in names:
            p = parent[n]
            local_rest = (rest_c[p].inverted() @ rest_c[n]) if p else rest_c[n]
            parent_pose = M[p] if p else Matrix.Identity(4)
            B = local_rest.inverted() @ parent_pose.inverted() @ M[n]
            loc, rot, _scale = B.decompose()  # stretch / scale are dropped (the engine has none per joint)
            basis[n] = (loc, rot)
        samples.append((f - f0, basis))
    rig.animation_data.action = None
    # face joints (jaw, tongue, eyes) belong to face clips (§11): key them in a body clip only if it moves them
    still = {n for n in rs.FACE_JOINTS if n in rest_c and all(
        b[n][1].rotation_difference(Quaternion()).angle < math.radians(0.01) for _f, b in samples)}
    src_name = action.name
    action.name = src_name + ".control"  # the baked clip takes the original name
    new = rc.new_action(arm, src_name)
    for fr, basis in samples:
        for n, (loc, rot) in basis.items():
            if n in still:
                continue
            pb = arm.pose.bones[n]
            pb.rotation_quaternion = rot
            pb.keyframe_insert("rotation_quaternion", frame=fr)
            if n in rs.TRANSLATION_KEY_JOINTS:
                pb.location = loc
                pb.keyframe_insert("location", frame=fr)
    for fc_ in rc.iter_fcurves(new):
        for kp in fc_.keyframe_points:
            kp.interpolation = "LINEAR"
    arm.animation_data.action = None
    rc.reset_pose(arm)
    return new


def bake(rig, meshes, mapping, no_lm, actions=None, mirror=True):
    height = max((rig.matrix_world @ v.co).z for o in meshes for v in o.data.vertices) - \
        min((rig.matrix_world @ v.co).z for o in meshes for v in o.data.vertices)
    if rig.animation_data is None:
        rig.animation_data_create()
    keep_action = rig.animation_data.action
    rig.animation_data.action = None
    for pb in rig.pose.bones:  # rest the control rig before reading bind matrices
        pb.location, pb.scale = (0, 0, 0), (1, 1, 1)
        pb.rotation_quaternion, pb.rotation_euler = (1, 0, 0, 0), (0, 0, 0)
    bpy.context.view_layer.update()
    arm, info = fit_to_rig(rig, mapping, no_lm, height)
    report = {"landmarks": len(info["anchors"]), "meshes": {}, "actions": []}
    for o in meshes:
        report["meshes"][o.name] = transfer_groups(o, rig, mapping, arm, mirror)
    acts = [bpy.data.actions[n] for n in actions] if actions else \
        [a for a in bpy.data.actions if a.name not in ("rom", "tpose") and _drives(a, rig)]
    if keep_action and keep_action not in acts:
        acts.append(keep_action)
    report["baked"] = {}
    for act in acts:
        name = act.name
        report["baked"][name] = bake_action(rig, arm, mapping, act)
        report["actions"].append(name)
    if mirror:
        report["mirror_error"] = mirror_error(arm, meshes, report)
    rc.make_rom_action(arm)
    rc.make_tpose_action(arm)
    arm.animation_data.action = None
    rc.reset_pose(arm)
    return arm, report


def mirror_error(arm, meshes, report):
    """Max vertex distance (mm) between the mirrored and the unmirrored weights over every frame of every baked
    clip (and the bind pose). Raises SystemExit above MIRROR_ERROR_MAX_MM; removes the twins either way."""
    pairs = []
    for o in meshes:
        st = report["meshes"][o.name]
        pairs.append((o, st.pop("_twin")))
    worst = (0.0, None, None, None)
    try:
        clips = [(None, [0])] + [(act, list(range(int(math.floor(act.frame_range[0])),
                                                  int(math.ceil(act.frame_range[1])) + 1)))
                                 for act in report["baked"].values()]
        for act, frames in clips:
            arm.animation_data.action = act
            for f in frames:
                bpy.context.scene.frame_set(f)
                bpy.context.view_layer.update()
                for o, twin in pairs:
                    d = np.linalg.norm(_evaluated(o) - _evaluated(twin), axis=1)
                    i = int(np.argmax(d))
                    if d[i] * 1000 > worst[0]:
                        worst = (float(d[i]) * 1000, o.name, act.name if act else "bind", (f, i))
        arm.animation_data.action = None
        rc.reset_pose(arm)
    finally:
        for _o, twin in pairs:
            me = twin.data
            bpy.data.objects.remove(twin, do_unlink=True)
            bpy.data.meshes.remove(me)
    out = {"max_mm": round(worst[0], 4), "mesh": worst[1], "clip": worst[2],
           "frame_vertex": list(worst[3]) if worst[3] else None, "limit_mm": MIRROR_ERROR_MAX_MM}
    if worst[0] > MIRROR_ERROR_MAX_MM:
        raise SystemExit("bake_to_canonical: --mirror-weights moves vertex %d of '%s' by %.3f mm on clip '%s' frame %d "
                         "(limit %.1f mm): the source weights are not symmetric enough to mirror without changing the "
                         "authored deformation. Bake without --mirror-weights, or make the source weights symmetric."
                         % (worst[3][1], worst[1], worst[0], worst[2], worst[3][0], MIRROR_ERROR_MAX_MM))
    return out


def _drives(action, rig):
    names = set(rig.pose.bones.keys())
    for fcu in rc.iter_fcurves(action):
        dp = fcu.data_path
        if dp.startswith('pose.bones["') and dp.split('"')[1] in names:
            return True
    return False


def remove_control_rig(rig, report=None):
    for act in [a for a in bpy.data.actions if a.name.endswith(".control")]:  # the control rig's source clips
        bpy.data.actions.remove(act)
    if report is not None:
        report.pop("baked", None)
    for o in list(bpy.data.objects):
        if o.type == "ARMATURE" and o.name != "canonical":
            bpy.data.objects.remove(o, do_unlink=True)
    for c in list(bpy.data.collections):  # Rigify widget collections
        if c.name.startswith("WGTS") or c.name.startswith("Widgets"):
            for o in list(c.objects):
                bpy.data.objects.remove(o, do_unlink=True)
            bpy.data.collections.remove(c)
    for o in [o for o in bpy.data.objects if o.name.startswith("WGT-")]:
        bpy.data.objects.remove(o, do_unlink=True)


# --------------------------------------------------------------------------------------------
# Self-test: Rigify human metarig -> generated rig -> mannequin skinned to DEF bones -> FK clip -> bake
# --------------------------------------------------------------------------------------------
FK_CLIP = [  # (frame, {control: (x, y, z) degrees})
    (0, {}),
    (12, {"upper_arm_fk.L": (40, 0, -30), "forearm_fk.L": (60, 0, 0), "thigh_fk.R": (-45, 0, 0),
          "shin_fk.R": (70, 0, 0), "chest": (10, 15, 0), "neck": (-10, 20, 0)}),
    (24, {"upper_arm_fk.L": (-20, 30, 20), "forearm_fk.L": (100, 40, 0), "upper_arm_fk.R": (30, -20, 40),
          "thigh_fk.L": (-60, 10, 0), "shin_fk.L": (90, 0, 0), "chest": (-10, -15, 5), "head": (15, 0, -10)}),
    (36, {}),
]


def _selftest_rig():
    import addon_utils
    nb.reset_scene()  # (a factory reset unloads add-ons, so enable after it)
    addon_utils.enable("rigify", default_set=True)
    bpy.ops.object.armature_human_metarig_add()
    meta = bpy.context.active_object
    bpy.ops.pose.rigify_generate()
    rig = [o for o in bpy.data.objects if o.type == "ARMATURE" and o != meta][0]
    bpy.data.objects.remove(meta, do_unlink=True)
    return rig


def _limit_weights(obj, limit=4, prune=0.01):
    """Engine budget on the source's own groups: top `limit` influences, none below `prune`, normalised."""
    names = {g.index: g.name for g in obj.vertex_groups}
    for v in obj.data.vertices:
        ws = sorted(((g.weight, g.group) for g in v.groups if g.weight > 0.0), reverse=True)
        keep = [(w, i) for w, i in ws[:limit] if w >= prune] or ws[:1]
        tot = sum(w for w, _ in keep) or 1.0
        kept = {i for _, i in keep}
        for w, i in ws:
            if i not in kept:
                obj.vertex_groups[names[i]].remove([v.index])
        for w, i in keep:
            obj.vertex_groups[names[i]].add([v.index], w / tot, "REPLACE")


def _evaluated(obj):
    return np.array([tuple(v) for v in rc.evaluated_positions(obj)])


def selftest(a):
    mapping, no_lm = load_map(a["map"])
    rig = _selftest_rig()
    # the mannequin: built on the canonical fit of this rig, skinned with heat weights to the DEF bones
    lm = {c: list(rig.matrix_world @ rig.data.bones[b].head_local) for c, b in mapping.items()
          if c not in no_lm and c != "root" and b in rig.data.bones}
    rows, _ = rig_fit.fit_joints(rig_fit.symmetrize(lm))
    mesh = fc.make_mannequin({r[0]: (r[2], r[3]) for r in rows})
    for o in bpy.context.selected_objects:
        o.select_set(False)
    mesh.select_set(True)
    rig.select_set(True)
    bpy.context.view_layer.objects.active = rig
    bpy.ops.object.parent_set(type="ARMATURE_AUTO")
    # FK clip on the controls
    for side in ("L", "R"):
        for p in ("upper_arm_parent.", "thigh_parent."):
            if p + side in rig.pose.bones:
                rig.pose.bones[p + side]["IK_FK"] = 1.0
    rig.animation_data_create()
    act = bpy.data.actions.new("rigify_fk_test")
    rig.animation_data.action = act
    for fr, pose in FK_CLIP:
        for pb in rig.pose.bones:
            if pb.name in {n for _, p in FK_CLIP for n in p}:
                x, y, z = pose.get(pb.name, (0, 0, 0))
                pb.rotation_mode = "QUATERNION"
                pb.rotation_quaternion = Euler((math.radians(x), math.radians(y), math.radians(z))).to_quaternion()
                pb.keyframe_insert("rotation_quaternion", frame=fr)
    rig.animation_data.action = None
    # references: the Rigify-skinned mesh evaluated at check frames. 'authored' = heat weights as generated
    # (any number of influences), 'limited' = the same weights cut to the engine's budget first (4 per vertex,
    # none < 0.01, normalised) so that comparison isolates the bake itself; each with Rigify's B-bones as
    # authored and with every bone at 1 segment (the engine has no B-bones).
    frames = [6, 12, 18, 24, 30]

    def capture():
        ref = mesh.copy()
        ref.data = mesh.data.copy()
        bpy.context.scene.collection.objects.link(ref)
        rig.animation_data.action = act
        out = {}
        for f in frames:
            bpy.context.scene.frame_set(f)
            bpy.context.view_layer.update()
            out[f] = _evaluated(ref)
        rig.animation_data.action = None
        bpy.data.objects.remove(ref, do_unlink=True)
        return out

    def capture_both():
        bb = capture()
        saved = {b.name: b.bbone_segments for b in rig.data.bones}
        for b in rig.data.bones:
            b.bbone_segments = 1
        rigid = capture()
        for b in rig.data.bones:
            b.bbone_segments = saved[b.name]
        return bb, rigid

    refs = {}
    refs["authored_bbones"], refs["authored_rigid"] = capture_both()
    _limit_weights(mesh)
    refs["limited_bbones"], refs["limited_rigid"] = capture_both()
    arm, report = bake(rig, [mesh], mapping, no_lm, actions=[act.name], mirror=a["mirror"])
    baked = report["baked"]["rigify_fk_test"]
    arm.animation_data.action = baked
    dev = {k: [] for k in refs}
    for f in frames:
        bpy.context.scene.frame_set(f)
        bpy.context.view_layer.update()
        got = _evaluated(mesh)
        for k in refs:
            dev[k].append(np.linalg.norm(got - refs[k][f], axis=1))
    arm.animation_data.action = None
    rc.reset_pose(arm)
    bpy.context.view_layer.update()
    st = {"frames": frames, "vertices": len(mesh.data.vertices)}
    for k, d in dev.items():
        fi, vi = np.unravel_index(int(np.argmax(np.stack(d))), np.stack(d).shape)
        d = np.concatenate(d)
        st[k] = {"max_mm": round(float(d.max()) * 1000, 4), "p95_mm": round(float(np.percentile(d, 95)) * 1000, 4),
                 "mean_mm": round(float(d.mean()) * 1000, 4), "worst": [frames[fi], int(vi)]}
    st["about"] = ("vertex distance between the canonical bake (as exported) and the Rigify-skinned mesh over the FK "
                   "clip. authored_*: Rigify heat weights as generated; limited_*: the same cut to 4 influences "
                   "before the bake (isolates the bake from the influence limit). *_bbones: Rigify's 10-segment "
                   "B-bones; *_rigid: every bone 1 segment (isolates the bake from the B-bone loss). worst: [frame, "
                   "vertex].")
    report["selftest"] = st
    remove_control_rig(rig, report)
    out = os.path.join(a["out"], "bake_selftest.blend")
    fc.save_outputs(arm, [mesh], out, export=os.path.join(a["out"], "bake_selftest.glb"), report=report)
    print(json.dumps(st, indent=1))
    print("bake selftest (max / p95 mm): limited+rigid %.3f / %.3f, limited+bbones %.1f / %.1f, authored+bbones "
          "%.1f / %.1f" % (st["limited_rigid"]["max_mm"], st["limited_rigid"]["p95_mm"], st["limited_bbones"]["max_mm"],
                           st["limited_bbones"]["p95_mm"], st["authored_bbones"]["max_mm"],
                           st["authored_bbones"]["p95_mm"]))

def main():
    a = _args()
    if a["selftest"]:
        selftest(a)
        return
    bpy.ops.wm.open_mainfile(filepath=os.path.abspath(a["source"]))
    mapping, no_lm = load_map(a["map"])
    rigs = [o for o in bpy.data.objects if o.type == "ARMATURE" and (a["rig"] is None or o.name == a["rig"])]
    rigs = [r for r in rigs if any(mapping[c] in r.data.bones for c in rig_fit.REQUIRED)]
    if len(rigs) != 1:
        raise SystemExit("bake_to_canonical: want one control rig with the mapped bones, found %d" % len(rigs))
    rig = rigs[0]
    meshes = [o for o in bpy.data.objects if o.type == "MESH" and
              any(m.type == "ARMATURE" and m.object == rig for m in o.modifiers)]
    arm, report = bake(rig, meshes, mapping, no_lm, a["actions"].split(",") if a["actions"] else None, a["mirror"])
    remove_control_rig(rig, report)
    fc.save_outputs(arm, meshes, a["out"], a["export"], report)
    print(json.dumps(report, indent=1, default=str))


if __name__ == "__main__":
    main()
