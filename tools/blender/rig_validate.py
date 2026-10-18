"""Rig validator: static body + face checks, the ROM pass and the contact sheets (docs/rigging_plan.md §10.1-10.3).

    uv run --offline --python 3.11 --with bpy==5.0.1 python -I tools/blender/rig_validate.py \\
        <file.blend|file.glb> --template assets/rig/canonical_humanoid.json --out report/ \\
        [--presets assets/rig/face_presets.json] [--no-sheets] [--samples 16] [--cell 192] \\
        [--export report/asset.glb]

Writes <out>/<asset>.rigreport.json and, unless --no-sheets, the contact sheets
<out>/<asset>_visemes.png, _expressions.png, _expressions_closeup.png, _turntable.png (a body ROM sheet is P1b).
--export also writes the asset with the rig export profile plus <asset>.rigexpect.json for tools/ne_rig_check.

Every check reports PASS, FAIL, WARN or SKIP (with the reason). The asset is accepted only on PASS
(a WARN needs a reviewer waiver). A .blend gives the full report. A .glb loses the region-mask vertex
groups (`rgn_*`) and the bone rolls are the importer's, so those checks SKIP there.

Sheets are rendered with Cycles on the CPU (16 samples, no denoiser): in this headless bpy build
Workbench and EEVEE need a GPU context that is not available. See docs/rigging.md.
"""
import argparse
import json
import math
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bpy  # noqa: E402
import numpy as np  # noqa: E402
from mathutils import Euler, Vector  # noqa: E402
from mathutils.bvhtree import BVHTree  # noqa: E402
from mathutils.kdtree import KDTree  # noqa: E402

import face_spec as fs  # noqa: E402
import ne_blender as nb  # noqa: E402
import rig_common as rc  # noqa: E402
import rig_spec as rs  # noqa: E402

MM = 0.001


# ------------------------------------------------------------------------------------------------------------
# Report
# ------------------------------------------------------------------------------------------------------------
class Report:
    def __init__(self):
        self.checks = []

    def add(self, cid, status, detail, **values):
        assert status in ("PASS", "FAIL", "WARN", "SKIP"), status
        self.checks.append({"id": cid, "status": status, "detail": detail, **values})
        return status

    def check(self, cid, ok, detail, warn=False, **values):
        return self.add(cid, "PASS" if ok else ("WARN" if warn else "FAIL"), detail, **values)

    def status(self):
        st = {c["status"] for c in self.checks}
        return "FAIL" if "FAIL" in st else "WARN" if "WARN" in st else "PASS"


# ------------------------------------------------------------------------------------------------------------
# Loading
# ------------------------------------------------------------------------------------------------------------
def load_asset(path):
    if path.lower().endswith(".blend"):
        bpy.ops.wm.open_mainfile(filepath=os.path.abspath(path))
        return "blend"
    nb.reset_scene()
    bpy.ops.import_scene.gltf(filepath=os.path.abspath(path))
    return "glb"


def find_rig():
    arms = [o for o in bpy.data.objects if o.type == "ARMATURE"]
    if len(arms) != 1:
        raise SystemExit("rig_validate: expected exactly one armature, found %d" % len(arms))
    arm = arms[0]
    meshes = [o for o in bpy.data.objects if o.type == "MESH" and
              any(m.type == "ARMATURE" and m.object == arm for m in o.modifiers)]
    return arm, meshes


def face_mesh(meshes):
    best, n = None, 0
    for o in meshes:
        keys = o.data.shape_keys
        if keys:
            k = sum(1 for kb in keys.key_blocks if kb.name in fs.SHAPE_INFO)
            if k > n:
                best, n = o, k
    return best


def deform_bones(arm):
    return [b for b in arm.data.bones if b.use_deform]


def vertex_weights(obj, deform_names):
    """-> list of {bone: weight} over deform groups, per vertex."""
    gname = {g.index: g.name for g in obj.vertex_groups}
    out = []
    for v in obj.data.vertices:
        out.append({gname[g.group]: g.weight for g in v.groups if gname.get(g.group) in deform_names and g.weight > 0})
    return out


def region_groups(obj):
    gname = {g.index: g.name[4:] for g in obj.vertex_groups if g.name.startswith("rgn_")}
    if not gname:
        return None
    n = len(obj.data.vertices)
    masks = {m: np.zeros(n, bool) for m in gname.values()}
    for v in obj.data.vertices:
        for g in v.groups:
            if g.group in gname and g.weight > 0:
                masks[gname[g.group]][v.index] = True
    return masks


def mesh_arrays(obj):
    me = obj.data
    P = np.empty(len(me.vertices) * 3)
    me.vertices.foreach_get("co", P)
    polys = [tuple(p.vertices) for p in me.polygons]
    mats = [me.materials[p.material_index].name if me.materials else "" for p in me.polygons]
    return P.reshape(-1, 3), polys, mats


def evaluated(obj):
    dg = bpy.context.evaluated_depsgraph_get()
    ev = obj.evaluated_get(dg)
    me = ev.to_mesh()
    P = np.empty(len(me.vertices) * 3)
    me.vertices.foreach_get("co", P)
    ev.to_mesh_clear()
    M = np.array(obj.matrix_world)
    P = P.reshape(-1, 3)
    return P @ M[:3, :3].T + M[:3, 3]


def mirror_candidates(P, tol):
    """i -> every vertex index at (-x, y, z) within tol. Coincident copies (a lip line split into an upper
    and a lower vertex) give several candidates; a check accepts the best-matching one."""
    kd = KDTree(len(P))
    for i, p in enumerate(P):
        kd.insert(Vector(p), i)
    kd.balance()
    return [[j for _co, j, _d in kd.find_range(Vector((-p[0], p[1], p[2])), tol)] for p in P]


def alias_index(P, eps=1e-6):
    """i -> the first vertex at the same position (coincident copies share one id)."""
    seen, out = {}, np.arange(len(P))
    for i, p in enumerate(P):
        out[i] = seen.setdefault(tuple(np.round(p / eps).astype(np.int64)), i)
    return out


def touching(polys, alias, a, b):
    return bool(set(alias[list(polys[a])]) & set(alias[list(polys[b])]))


def fit_sphere(Q):
    """Least-squares sphere through points Q (n, 3) -> (centre, radius)."""
    A = np.c_[2 * Q, np.ones(len(Q))]
    b = (Q * Q).sum(axis=1)
    x, *_ = np.linalg.lstsq(A, b, rcond=None)
    c = x[:3]
    return c, float(math.sqrt(max(0.0, x[3] + c @ c)))


# ------------------------------------------------------------------------------------------------------------
# Body checks (R6-R9, R11, R12, R15, R16, modifiers, actions)
# ------------------------------------------------------------------------------------------------------------
def body_checks(R, arm, meshes, tpl, kind, glb_doc):
    th = tpl["thresholds"]
    tj = {j["name"]: j for j in tpl["joints"]}
    canon = [j["name"] for j in tpl["joints"]]
    extras = [s.strip() for s in str(arm.get("ne_extra_joints", "")).split(",") if s.strip()]
    asym = bool(arm.get("ne_asymmetric", False))

    # R12: names, parents, order, hinge axes
    if glb_doc is not None:
        names = [glb_doc["nodes"][j]["name"] for j in glb_doc["skins"][0]["joints"]]
    else:
        names = [b.name for b in deform_bones(arm)]
    canon_part = [n for n in names if n not in extras]
    R.check("R12.order", canon_part == canon,
            "deform joints match canonical names and order (order hash %s)" % rs.order_hash(canon_part)
            if canon_part == canon else "joint order differs from the template",
            order_hash=rs.order_hash(canon_part), template_hash=tpl["order_hash"],
            missing=[n for n in canon if n not in names], unknown=[n for n in names if n not in canon and n not in extras])
    bad_parent = []
    for b in arm.data.bones:
        if b.name in tj:
            want = tj[b.name]["parent"]
            got = b.parent.name if b.parent else None
            if got != want:
                bad_parent.append([b.name, got, want])
    R.check("R12.parents", not bad_parent, "%d joints with a wrong parent" % len(bad_parent), wrong=bad_parent[:10])
    if kind == "glb":
        R.add("R12.hinges", "SKIP", "glb input: bone rolls are the importer's, not the authored ones (validate the .blend)")
    else:
        worst, bad = 0.0, []
        # The roll hint: the fitted one (fit_canonical / bake_to_canonical store the template hint carried
        # through the fit) or the template's, orthogonalised against the bone's actual direction.
        fitted = json.loads(arm.get("ne_x_hints", "{}"))
        for h in tpl["hinges"]:
            if h not in arm.data.bones:
                continue
            bone = arm.data.bones[h]
            y = np.array(bone.tail_local - bone.head_local)
            y /= np.linalg.norm(y)
            hint = np.array(fitted.get(h, tj[h]["x_axis"]), dtype=float)
            x = hint - y * np.dot(hint, y)
            x /= np.linalg.norm(x)
            got = np.array(rc.bone_x_axis(arm, h))
            a = math.degrees(math.acos(max(-1.0, min(1.0, float(np.dot(got, x))))))
            worst = max(worst, a)
            if a > th["R12"]["hinge_axis_deg"]:
                bad.append([h, round(a, 2)])
        R.check("R12.hinges", not bad, "hinge axes within %.1f deg (worst %.3f deg over %d hinges)" %
                (th["R12"]["hinge_axis_deg"], worst, len(tpl["hinges"])), worst_deg=worst, bad=bad)

    # R9
    n_def = len(names)
    R.check("R9", n_def <= th["R9"]["skin_joints_max"] and len(canon_part) <= th["R9"]["canonical_max"],
            "%d skin joints (max %d), %d canonical (max %d)" % (n_def, th["R9"]["skin_joints_max"], len(canon_part),
                                                              th["R9"]["canonical_max"]), joints=n_def)

    # R15: deform-only skin, constraints / drivers
    stray = [n for n in names if n not in canon and n not in extras]
    cons = [[pb.name, c.type] for pb in arm.pose.bones for c in pb.constraints if pb.bone.use_deform]
    drivers = len(arm.animation_data.drivers) if arm.animation_data else 0
    for o in meshes:
        if o.data.shape_keys and o.data.shape_keys.animation_data:
            drivers += len(o.data.shape_keys.animation_data.drivers)
    R.check("R15.joints", not stray, "skin joints are canonical + declared extras" if not stray else
            "joints outside the canonical set: %s" % stray[:10], extras=extras)
    R.check("R15.bake", not cons and not drivers,
            "no constraints on deform bones, no drivers" if not cons and not drivers else
            "%d constraints, %d drivers: export with the rig profile (export_force_sampling) to bake them" %
            (len(cons), drivers), warn=True, constraints=cons[:10], drivers=drivers)

    # R11: transforms, units, height
    bad_xf = []
    for o in [arm] + meshes:
        if any(abs(s - 1.0) > 1e-6 for s in o.scale) or any(abs(r) > 1e-6 for r in o.rotation_euler):
            bad_xf.append(o.name)
        if o.matrix_world.determinant() <= 0:
            bad_xf.append(o.name + " (negative determinant)")
    unit = bpy.context.scene.unit_settings.scale_length
    R.check("R11.transforms", not bad_xf and abs(unit - 1.0) < 1e-9,
            "unit scale %.3f; objects with unapplied scale/rotation or mirroring: %s" % (unit, bad_xf or "none"),
            objects=bad_xf, unit_scale=unit)
    body = [o for o in meshes if o.data.shape_keys is None or face_mesh([o]) is None]
    zs = np.concatenate([evaluated(o)[:, 2] for o in meshes]) if meshes else np.zeros(1)
    reaches_floor = bool(meshes) and float(zs.min()) < 0.05
    if not reaches_floor:
        R.add("R11.height", "SKIP", "no mesh reaches the floor (head-only asset): height is checked on a full body")
        R.add("R16.soles", "SKIP", "no mesh reaches the floor (head-only asset): soles are checked on a full body")
    else:
        h = float(zs.max() - zs.min())
        want = float(arm.get("ne_height_m", tpl["height_m"]))
        R.check("R11.height", abs(h / want - 1.0) <= th["R11"]["height_tolerance"],
                "height %.3f m vs %.3f m (tolerance %.0f%%)" % (h, want, 100 * th["R11"]["height_tolerance"]),
                height_m=h)
        R.check("R16.soles", abs(float(zs.min())) <= th["R16"]["sole_z_m"], "lowest vertex at z = %.4f m" % zs.min(),
                sole_z=float(zs.min()))
    root = arm.data.bones.get("root")
    pelvis = arm.data.bones.get("pelvis")
    if root and pelvis:
        rh = arm.matrix_world @ root.head_local
        ph_ = arm.matrix_world @ pelvis.head_local
        R.check("R16.root", rh.length <= th["R16"]["sole_z_m"] and ph_.z > rh.z,
                "root at %s, pelvis z %.3f" % ([round(c, 4) for c in rh], ph_.z))
    else:
        R.add("R16.root", "FAIL", "root or pelvis missing")

    # Modifiers: Armature only, Preserve Volume off (E6: Blender must evaluate the engine's LBS)
    bad_mod, pv = [], []
    for o in meshes:
        for m in o.modifiers:
            if m.type != "ARMATURE":
                bad_mod.append([o.name, m.type])
            elif m.use_deform_preserve_volume:
                pv.append(o.name)
    R.check("MOD.preserve_volume", not pv, "Preserve Volume off on every Armature modifier" if not pv else
            "Preserve Volume on: %s (the engine has no DQS)" % pv)
    R.check("MOD.others", not bad_mod, "no modifiers besides Armature" if not bad_mod else
            "unapplied modifiers: %s" % bad_mod)

    # R6 / R7 / R8 and E2 per skinned mesh
    dnames = {b.name for b in deform_bones(arm)}
    tot = {"verts": 0, "unweighted": 0, "unnormalised": 0, "tiny": 0, "over4": 0, "max_inf": 0, "sym_worst": 0.0,
           "sym_bad": 0, "sym_pairs": 0}
    for o in meshes:
        W = vertex_weights(o, dnames)
        P = np.array([tuple(v.co) for v in o.data.vertices])
        tot["verts"] += len(W)
        for w in W:
            s = sum(w.values())
            tot["unweighted"] += s == 0
            tot["unnormalised"] += s < th["R7"]["weight_sum_min"]
            tot["tiny"] += sum(1 for x in w.values() if x < th["R7"]["weight_min"])
            tot["over4"] += len(w) > th["R8"]["influences_max"]
            tot["max_inf"] = max(tot["max_inf"], len(w))
        if asym or o.get("ne_asymmetric"):
            continue
        mc = mirror_candidates(P, th["R6"]["mirror_match_m"])
        for i, cands in enumerate(mc):
            if not cands:
                continue
            tot["sym_pairs"] += 1
            wi = W[i]
            d = min(max([abs(wi.get(b, 0.0) - W[j].get(rs.mirror_name(b), 0.0))
                         for b in set(wi) | {rs.mirror_name(x) for x in W[j]}] or [0.0]) for j in cands)
            tot["sym_worst"] = max(tot["sym_worst"], d)
            tot["sym_bad"] += d > th["R6"]["weight_diff_max"]
    R.check("R7", tot["unnormalised"] == 0 and tot["tiny"] == 0,
            "%d vertices: %d with sum < %.3f (%d with no weight at all; E2: the engine silently gives those weight 1 "
            "on their first listed joint), %d weights below %.2f" %
            (tot["verts"], tot["unnormalised"], th["R7"]["weight_sum_min"], tot["unweighted"], tot["tiny"],
             th["R7"]["weight_min"]), unweighted=tot["unweighted"], unnormalised=tot["unnormalised"], tiny=tot["tiny"])
    R.check("R8", tot["max_inf"] <= th["R8"]["influences_max"],
            "max %d influences per vertex (E1: the importer keeps 4 and drops the rest)" % tot["max_inf"],
            max_influences=tot["max_inf"], over4=tot["over4"])
    if asym:
        R.add("R6", "SKIP", "armature tagged ne_asymmetric")
    else:
        R.check("R6", tot["sym_bad"] == 0, "%d mirrored vertex pairs, worst weight difference %.4f (max %.2f)" %
                (tot["sym_pairs"], tot["sym_worst"], th["R6"]["weight_diff_max"]),
                pairs=tot["sym_pairs"], worst=tot["sym_worst"], bad=tot["sym_bad"])

    # Actions: start at 0, body clips never key face joints, translation only on root / pelvis / declared
    face = set(tpl["face_joints"])
    trans_ok = set(tpl["translation_key_joints"]) | set(extras)
    acts = []
    for act in bpy.data.actions:
        bones, trans, other = set(), set(), 0
        for fc in rc.iter_fcurves(act):
            dp = fc.data_path
            if dp.startswith('pose.bones["'):
                b = dp.split('"')[1]
                bones.add(b)
                if dp.endswith(".location"):
                    trans.add(b)
            else:
                other += 1
        if not bones:
            continue  # shape-key (face) clip
        body_clip = bool(bones - face)
        start = float(act.frame_range[0])
        acts.append({"name": act.name, "start": start, "body": body_clip, "joints": len(bones),
                     "face_keys": sorted(bones & face) if body_clip else [],
                     "bad_translation": sorted(trans - trans_ok)})
    bad_start = [a["name"] for a in acts if abs(a["start"]) > 1e-6]
    bad_face = [[a["name"], a["face_keys"]] for a in acts if a["face_keys"]]
    bad_tr = [[a["name"], a["bad_translation"]] for a in acts if a["bad_translation"]]
    R.check("ACT.start", not bad_start, "%d skeletal actions, all start at frame 0" % len(acts) if not bad_start else
            "actions not starting at frame 0: %s" % bad_start, actions=[a["name"] for a in acts])
    R.check("ACT.face_joints", not bad_face, "no body clip keys jaw / tongue / eyes" if not bad_face else
            "body clips keying face joints: %s" % bad_face)
    R.check("ACT.translation", not bad_tr, "translation keys only on %s" % sorted(trans_ok) if not bad_tr else
            "translation keys on other joints: %s" % bad_tr)


# ------------------------------------------------------------------------------------------------------------
# Face: static checks (F11, F15, normals) and the face ROM (F12, F13)
# ------------------------------------------------------------------------------------------------------------
def shape_deltas(obj):
    keys = obj.data.shape_keys
    n = len(obj.data.vertices)
    base = np.empty(n * 3)
    keys.reference_key.data.foreach_get("co", base)
    base = base.reshape(-1, 3)
    out = {}
    for kb in keys.key_blocks:
        if kb == keys.reference_key:
            continue
        a = np.empty(n * 3)
        kb.data.foreach_get("co", a)
        out[kb.name] = a.reshape(-1, 3) - base
    return base, out


def face_static(R, head, presets, kind, glb_doc):
    th = rs.THRESHOLDS
    base, D = shape_deltas(head)
    names = list(D)
    missing = [n for n in fs.SHAPES_52 if n not in D]
    house = [n for n in fs.HOUSE_SHAPES if n in D]
    unknown = [n for n in names if n not in fs.SHAPE_INFO]
    R.check("F11.names", not missing, "%d / 52 facial-action shapes, house shapes %s" % (52 - len(missing), house)
            if not missing else "missing shapes: %s" % missing, missing=missing, house=house)
    R.check("F11.undeclared", not unknown, "no undeclared shapes" if not unknown else
            "shapes not declared in face_presets.json: %s" % unknown, warn=True, unknown=unknown)
    order_ok = [n for n in names if n in fs.SHAPE_INFO] == [n for n in fs.ALL_SHAPES if n in D]
    R.check("F11.order", order_ok, "shape keys in the canonical slot order" if order_ok else
            "shape keys out of canonical order (the engine looks shapes up by name, so this is cosmetic)", warn=True)

    # symmetry
    mc = mirror_candidates(base, th["F11"]["mirror_m"])
    S = np.array([-1.0, 1.0, 1.0])
    worst, worst_at, unpaired = 0.0, None, 0
    for n in names:
        if not n.endswith("Left") or n[:-4] + "Right" not in D:
            continue
        L, Rr = D[n], D[n[:-4] + "Right"]
        moved = np.nonzero(np.linalg.norm(L, axis=1) > 1e-6)[0]
        for i in moved:
            if not mc[i]:
                unpaired += 1
                continue
            e = min(float(np.linalg.norm(L[i] - S * Rr[j])) for j in mc[i])
            if e > worst:
                worst, worst_at = e, [n, int(i)]
    R.check("F11.symmetry", worst <= th["F11"]["mirror_m"] and unpaired == 0,
            "L/R pairs mirror within %.3f mm (worst %.4f mm), %d moved vertices without a mirror partner" %
            (th["F11"]["mirror_m"] / MM, worst / MM, unpaired), worst_m=worst, at=worst_at, unpaired=unpaired)

    # region masks
    masks = region_groups(head)
    if masks is None:
        R.add("F11.masks", "SKIP", "no rgn_* vertex groups (%s)" %
              ("glb import drops vertex groups that carry no skin weight" if kind == "glb" else "not authored"))
    else:
        bad = []
        for n in names:
            if n not in fs.SHAPE_INFO:
                continue
            allow = np.zeros(len(base), bool)
            for m in fs.MASK_ALLOW[fs.SHAPE_INFO[n]["mask"]]:
                if m in masks:
                    allow |= masks[m]
            mag = np.linalg.norm(D[n], axis=1)
            out = np.nonzero((mag > 1e-4) & ~allow)[0]
            if len(out):
                bad.append([n, int(len(out)), round(float(mag[out].max()) / MM, 3)])
        R.check("F11.masks", not bad, "every shape's deltas (> 0.1 mm) stay inside its region mask" if not bad else
                "%d shapes move vertices outside their mask: %s" % (len(bad), bad[:8]), bad=bad)

    # normals
    if glb_doc is None:
        R.add("F11.normals", "SKIP", ".blend input: NORMAL deltas exist only in the export; the proxy-normal region "
              "is defined with the TBO morph path (P1b)")
    else:
        has_n = any("NORMAL" in t for m in glb_doc["meshes"] for p in m["primitives"] for t in p.get("targets", []))
        R.add("F11.normals", "PASS" if not has_n else "WARN",
              "no NORMAL deltas exported" if not has_n else "NORMAL deltas exported; the proxy-normal region mask "
              "that zeroes them is P1b")

    # F15 budget (counted on the Blender mesh; the glTF export splits vertices at UV / material seams)
    mags = np.stack([np.linalg.norm(D[n], axis=1) for n in names], axis=1) if names else np.zeros((len(base), 0))
    per_v = (mags > 1e-7).sum(axis=1)
    entries = int(per_v.sum())
    texels = 2 * entries
    b = th["F15"]
    R.check("F15", len(names) <= b["shapes_max"] and int(per_v.max(initial=0)) <= b["entries_per_vertex_max"] and
            texels <= b["texels_max"],
            "%d shapes (max %d); max %d entries per vertex (max %d); %d entries = %d texels (max %d)" %
            (len(names), b["shapes_max"], int(per_v.max(initial=0)), b["entries_per_vertex_max"], entries, texels,
             b["texels_max"]), shapes=len(names), max_entries_per_vertex=int(per_v.max(initial=0)),
            texels=texels)
    return base, D


def _layer_cases():
    cases = [("shape:" + n, {n: 1.0}) for n in fs.ALL_SHAPES]
    cases += [("viseme:" + v, dict(fs.VISEMES[v]["shapes"]), v) for v in fs.VISEME_ORDER]
    cases += [("expr:" + e, dict(fs.EXPRESSIONS[e])) for e in fs.EXPRESSION_ORDER]
    for e in fs.EXPRESSION_ORDER:
        for v in fs.VISEME_ORDER:
            w = fs.layer_weights(fs.EXPRESSIONS[e], fs.VISEMES[v]["shapes"], 1.0)
            cases.append(("layer:%s+%s" % (e, v), {k: x for k, x in w.items() if x > 0}, v))
    return [c if len(c) == 3 else (c[0], c[1], None) for c in cases]


_FACE_ACTIONS = {}  # head name -> shape-key action set aside while set_face drives the keys


def set_face(arm, head, weights, viseme=None):
    """Poses the face. A shape-key action on the head (the proof head carries `face_blink`) would override the
    key values at evaluation time -- it silently held eyeBlink* at 0 in every sheet and F12 case -- so it is
    set aside here and put back by restore_face_anim (before export)."""
    ad = head.data.shape_keys.animation_data
    if ad is not None and ad.action is not None:
        _FACE_ACTIONS[head.name] = ad.action
        ad.action = None
    keys = head.data.shape_keys.key_blocks
    for kb in keys:
        kb.value = 0.0
    for n, w in weights.items():
        if n in keys:
            keys[n].value = float(w)
    rc.reset_pose(arm)
    pose = {}
    if "jaw" in arm.pose.bones:
        pose["jaw"] = (weights.get("jawOpen", 0.0) * fs.JAW_MAX_DEG, 0.0, 0.0)
    if viseme is not None:
        for b, deg in zip(("tongue_01", "tongue_02", "tongue_03"), fs.VISEMES[viseme]["tongue"]):
            if b in arm.pose.bones:
                pose[b] = (float(deg), 0.0, 0.0)
    rc.set_pose(arm, pose)
    bpy.context.view_layer.update()


def restore_face_anim(head):
    act = _FACE_ACTIONS.pop(head.name, None)
    if act is not None:
        head.data.shape_keys.animation_data.action = act


def _faces_where(polys, mats, key):
    return [i for i, m in enumerate(mats) if key in m.lower()]


def _pair_depth(P, polys, a, b):
    """Penetration estimate for two overlapping polygons: how far a's vertices sit behind b's plane and
    vice versa; the smaller of the two (the shallower body is the one that went in)."""
    def behind(src, dst):
        q = P[list(polys[dst])]
        n = np.cross(q[1] - q[0], q[2] - q[0])
        ln = np.linalg.norm(n)
        if ln < 1e-12:
            return 0.0
        n /= ln
        d = (P[list(polys[src])] - q[0]) @ n
        return float(max(0.0, -d.min()))
    return min(behind(a, b), behind(b, a))


def face_rom(R, arm, head, base):
    """F12 (BVH overlap of lips vs teeth, lips vs lips, lid vertices vs eyeballs) and F13 (layering math)."""
    _, polys, mats = mesh_arrays(head)
    lips = _faces_where(polys, mats, "lip")
    teeth = _faces_where(polys, mats, "teeth")
    eye_f = sorted(set(_faces_where(polys, mats, "sclera") + _faces_where(polys, mats, "iris") +
                       _faces_where(polys, mats, "pupil")))
    skin = _faces_where(polys, mats, "skin")
    eye_v = sorted({v for f in eye_f for v in polys[f]})
    eyes = []  # (centre, radius) per eyeball: a sphere fitted to the sclera vertices on each side
    scl_v = sorted({v for f in _faces_where(polys, mats, "sclera") for v in polys[f]})
    for side in (1, -1):
        vs = [v for v in scl_v if base[v][0] * side > 0]
        if len(vs) >= 4:
            eyes.append(fit_sphere(base[vs]))
    skin_v = sorted({v for f in skin for v in polys[f]})
    # Lid vertices: skin within 8 mm of an eyeball and outside it at rest (skin the eyeball covers by
    # construction, e.g. a skull surface the eye pokes through, is hidden and does not count).
    lid_v = [v for v in skin_v if any(r - 1e-4 <= np.linalg.norm(base[v] - c) < r + 0.008 for c, r in eyes)]
    alias = alias_index(base)
    if not lips or not teeth:
        R.add("F12", "SKIP", "no lips / teeth materials found (name materials *lip*, *teeth*, *sclera*)")
        return None
    cases = _layer_cases()
    worst = {"lips_teeth": (0.0, None), "lips_lips": (0.0, None), "lids_eyes": (0.0, None)}
    pairs = {"lips_teeth": 0, "lips_lips": 0}
    for name, weights, vis in cases:
        set_face(arm, head, weights, vis)
        P = evaluated(head)
        tl = BVHTree.FromPolygons([tuple(p) for p in P], [polys[i] for i in lips])
        tt = BVHTree.FromPolygons([tuple(p) for p in P], [polys[i] for i in teeth])
        for i, j in tl.overlap(tt):
            pairs["lips_teeth"] += 1
            d = _pair_depth(P, polys, lips[i], teeth[j])
            if d > worst["lips_teeth"][0]:
                worst["lips_teeth"] = (d, name)
        for i, j in tl.overlap(tl):
            if i >= j:
                continue
            a, b = lips[i], lips[j]
            if touching(polys, alias, a, b):
                continue  # neighbours (shared or coincident vertices) touch by construction
            pairs["lips_lips"] += 1
            d = _pair_depth(P, polys, a, b)
            if d > worst["lips_lips"][0]:
                worst["lips_lips"] = (d, name)
        for c, r in eyes:  # eyeballs are rigid (eye bones at rest here), so the rest centre holds
            if lid_v:
                dist = np.linalg.norm(P[lid_v] - c, axis=1)
                d = float(max(0.0, (r - dist).max()))
                if d > worst["lids_eyes"][0]:
                    worst["lids_eyes"] = (d, name)
    set_face(arm, head, {})
    lim = rs.THRESHOLDS["F12"]["cross_m"]
    for k, (d, at) in worst.items():
        R.check("F12." + k, d <= lim, "worst penetration %.3f mm (max %.1f mm)%s over %d cases" %
                (d / MM, lim / MM, " at " + at if at else "", len(cases)), depth_m=d, at=at,
                overlapping_pairs=pairs.get(k))
    # F13: the layering math on every (expression, viseme) pair
    clos, roll, over = [], [], 0
    for e in fs.EXPRESSION_ORDER:
        for v in ("PP", "FF"):
            w = fs.layer_weights(fs.EXPRESSIONS[e], fs.VISEMES[v]["shapes"], 1.0)
            if v == "PP" and w["mouthClose"] < 0.8:
                clos.append([e, round(w["mouthClose"], 3)])
            if v == "FF" and w["mouthRollLower"] < 0.5:
                roll.append([e, round(w["mouthRollLower"], 3)])
        for v in fs.VISEME_ORDER:
            w = fs.layer_weights(fs.EXPRESSIONS[e], fs.VISEMES[v]["shapes"], 1.0)
            over += sum(1 for x in w.values() if x > 1.0 + 1e-9)
    R.check("F13", not clos and not roll and over == 0,
            "%d expression x viseme pairs: every weight <= 1, PP keeps mouthClose >= 0.8 and FF mouthRollLower >= 0.5 "
            "under every expression" % (len(fs.EXPRESSION_ORDER) * len(fs.VISEME_ORDER)),
            closure_fail=clos, roll_fail=roll, over_one=over)
    return {"cases": len(cases), "lid_vertices": len(lid_v), "eyes": len(eyes)}


# ------------------------------------------------------------------------------------------------------------
# ROM pass (§10.2): stretch, flips, new self-intersections per pose
# ------------------------------------------------------------------------------------------------------------
def rom_pass(R, arm, meshes, head):
    act = bpy.data.actions.get("rom")
    if act is None:
        R.add("ROM", "SKIP", "no `rom` action in the file (build it with make_rig_template.py --blend)")
        return []
    if head is not None:
        set_face(arm, head, {})
    out = []
    data = []
    for o in meshes:
        P0, polys, _ = mesh_arrays(o)
        edges = np.array([tuple(e.vertices) for e in o.data.edges])
        tris = [p[:3] for p in polys]
        rest = evaluated(o)
        L0 = np.linalg.norm(rest[edges[:, 0]] - rest[edges[:, 1]], axis=1)
        keep = L0 > 1e-7
        n0 = np.cross(rest[[t[1] for t in tris]] - rest[[t[0] for t in tris]],
                      rest[[t[2] for t in tris]] - rest[[t[0] for t in tris]])
        alias = alias_index(P0)
        bv = BVHTree.FromPolygons([tuple(p) for p in rest], polys)
        base_pairs = {(min(i, j), max(i, j)) for i, j in bv.overlap(bv) if i != j}
        data.append((o, edges, keep, L0, tris, n0, polys, base_pairs, alias, rest))
    arm.animation_data.action = act
    for k, (pname, _pose) in enumerate(rs.ROM_POSES):
        bpy.context.scene.frame_set(k * rs.ROM_STEP_FRAMES)
        bpy.context.view_layer.update()
        row = {"pose": pname, "frame": k * rs.ROM_STEP_FRAMES, "stretch_max": 0.0, "flips": 0, "new_overlaps": 0,
               "overlap_depth_max_m": 0.0}
        for o, edges, keep, L0, tris, n0, polys, base_pairs, alias, rest in data:
            P = evaluated(o)
            L = np.linalg.norm(P[edges[:, 0]] - P[edges[:, 1]], axis=1)
            row["stretch_max"] = max(row["stretch_max"], float(np.abs(L[keep] / L0[keep] - 1.0).max(initial=0)))
            n1 = np.cross(P[[t[1] for t in tris]] - P[[t[0] for t in tris]], P[[t[2] for t in tris]] - P[[t[0] for t in tris]])
            ok = (np.linalg.norm(n0, axis=1) > 1e-12)
            row["flips"] += int(((n0 * n1).sum(axis=1)[ok] < 0).sum())
            bv = BVHTree.FromPolygons([tuple(p) for p in P], polys)
            new = [(min(i, j), max(i, j)) for i, j in bv.overlap(bv)
                   if i != j and (min(i, j), max(i, j)) not in base_pairs and not touching(polys, alias, i, j)]
            row["new_overlaps"] += len(set(new))
            for i, j in set(new):  # depth the pose added (a pair the BVH just misses at bind keeps its bind depth)
                d = _pair_depth(P, polys, i, j) - _pair_depth(rest, polys, i, j)
                row["overlap_depth_max_m"] = max(row["overlap_depth_max_m"], d)
        out.append(row)
    arm.animation_data.action = None
    bpy.context.scene.frame_set(0)
    rc.reset_pose(arm)
    bpy.context.view_layer.update()
    th = rs.THRESHOLDS
    smax = max(r["stretch_max"] for r in out)
    flips = sum(r["flips"] for r in out)
    newo = sum(r["new_overlaps"] for r in out)
    depth = max(r["overlap_depth_max_m"] for r in out)
    lim = th["R1"]["intersect_depth_max_m"]
    R.check("ROM.stretch", smax <= th["R1"]["stretch_max"], "max edge stretch %.2f%% over %d ROM poses (max %.0f%%)" %
            (100 * smax, len(out), 100 * th["R1"]["stretch_max"]), stretch_max=smax)
    R.check("ROM.flips", flips == 0, "%d triangle normal reversals over the ROM poses" % flips, flips=flips)
    R.check("ROM.self_intersection", depth <= lim,
            "%d polygon pairs overlap in some pose but not at bind; deepest %.3f mm (max %.1f mm)" %
            (newo, depth / MM, lim / MM), new_overlaps=newo, depth_m=depth)
    R.add("ROM.volume", "SKIP", "segment volume between cut planes (R1, R2, R4) is not implemented yet; it needs "
          "a closed limb mesh (the pilot body), which P1a does not have")
    return out


# ------------------------------------------------------------------------------------------------------------
# Contact sheets (Cycles CPU)
# ------------------------------------------------------------------------------------------------------------
class Sheet:
    def __init__(self, head, cell, samples):
        self.cell, self.head = cell, head
        sc = bpy.context.scene
        sc.render.engine = "CYCLES"
        sc.cycles.device = "CPU"
        sc.cycles.samples = samples
        sc.cycles.use_denoising = False
        sc.cycles.max_bounces = 3
        sc.render.resolution_x = sc.render.resolution_y = cell
        sc.render.resolution_percentage = 100
        sc.render.image_settings.file_format = "PNG"
        sc.render.film_transparent = False
        world = sc.world or bpy.data.worlds.new("sheet_world")
        sc.world = world
        world.use_nodes = True
        bg = world.node_tree.nodes.get("Background")
        bg.inputs[0].default_value = (0.18, 0.19, 0.21, 1.0)
        bg.inputs[1].default_value = 0.7
        P = evaluated(head)
        lo, hi = P.min(axis=0), P.max(axis=0)
        self.centre = Vector(((lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2))
        size = float(max(hi[0] - lo[0], hi[2] - lo[2]))
        self.pivot = bpy.data.objects.new("sheet_pivot", None)
        sc.collection.objects.link(self.pivot)
        self.pivot.location = self.centre
        cam_data = bpy.data.cameras.new("sheet_cam")
        cam_data.type = "ORTHO"
        cam_data.ortho_scale = size * 1.3
        cam_data.clip_start, cam_data.clip_end = 0.001, 10.0
        self.cam = bpy.data.objects.new("sheet_cam", cam_data)
        sc.collection.objects.link(self.cam)
        self.cam.parent = self.pivot
        self.cam.location = (0.0, -1.0, 0.0)
        self.cam.rotation_euler = (math.radians(90), 0.0, 0.0)
        sc.camera = self.cam
        for nm, rot, energy in (("key", (55, 0, -35), 3.5), ("fill", (70, 0, 50), 1.2), ("rim", (110, 0, 180), 2.0)):
            ld = bpy.data.lights.new("sheet_" + nm, "SUN")
            ld.energy = energy
            lo_ = bpy.data.objects.new("sheet_" + nm, ld)
            sc.collection.objects.link(lo_)
            lo_.rotation_euler = tuple(math.radians(a) for a in rot)
        mat = bpy.data.materials.new("sheet_label")
        mat.use_nodes = True
        nt = mat.node_tree
        nt.nodes.clear()
        em = nt.nodes.new("ShaderNodeEmission")
        em.inputs[0].default_value = (1.0, 1.0, 1.0, 1.0)
        em.inputs[1].default_value = 1.5
        outn = nt.nodes.new("ShaderNodeOutputMaterial")
        nt.links.new(em.outputs[0], outn.inputs[0])
        td = bpy.data.curves.new("sheet_label", "FONT")
        td.size = cam_data.ortho_scale * 0.075
        td.materials.append(mat)
        self.label = bpy.data.objects.new("sheet_label", td)
        sc.collection.objects.link(self.label)
        self.label.parent = self.cam
        self.label.location = (-cam_data.ortho_scale * 0.47, -cam_data.ortho_scale * 0.47, -0.05)
        self.tmp = tempfile.mkdtemp(prefix="rigsheet_")
        self.n = 0

    def zoom(self, cell, scale, lift=0.0):
        """Re-frame for a close-up: `scale` is the ortho width as a fraction of the full-head frame, `lift` moves
        the frame centre up by that fraction of the full frame (to centre the face rather than the skull)."""
        sc = bpy.context.scene
        full = self.cam.data.ortho_scale / getattr(self, "_zoom", 1.0)
        self._zoom = scale
        self.cell = cell
        sc.render.resolution_x = sc.render.resolution_y = cell
        self.cam.data.ortho_scale = full * scale
        self.cam.location = (0.0, -1.0, full * lift)
        self.label.data.size = self.cam.data.ortho_scale * 0.06
        self.label.location = (-self.cam.data.ortho_scale * 0.47, -self.cam.data.ortho_scale * 0.47, -0.05)

    def render(self, text, yaw_deg=0.0):
        self.pivot.rotation_euler = (0.0, 0.0, math.radians(yaw_deg))
        self.label.data.body = text
        bpy.context.view_layer.update()
        path = os.path.join(self.tmp, "c%04d.png" % self.n)
        self.n += 1
        bpy.context.scene.render.filepath = path
        bpy.ops.render.render(write_still=True)
        img = bpy.data.images.load(path)
        px = np.empty(self.cell * self.cell * 4, np.float32)
        img.pixels.foreach_get(px)
        bpy.data.images.remove(img)
        return px.reshape(self.cell, self.cell, 4)

    @staticmethod
    def save(cells, cols, path):
        h, w = cells[0].shape[:2]
        rows = (len(cells) + cols - 1) // cols
        canvas = np.empty((rows * h, cols * w, 4), np.float32)
        canvas[...] = cells[0][1, 1]  # unused cells take the background colour of a rendered corner
        for k, c in enumerate(cells):
            r, q = divmod(k, cols)
            y0 = (rows - 1 - r) * h  # Blender image rows run bottom-up
            canvas[y0:y0 + h, q * w:(q + 1) * w] = c
        img = bpy.data.images.new(os.path.basename(path), cols * w, rows * h, alpha=False)
        img.pixels.foreach_set(canvas.ravel())
        img.filepath_raw = path
        img.file_format = "PNG"
        img.save()
        bpy.data.images.remove(img)
        return path


def contact_sheets(arm, head, meshes, out, stem, cell, samples):
    sh = Sheet(head, cell, samples)
    sheets = {}
    cells = []
    for v in fs.VISEME_ORDER:
        set_face(arm, head, fs.VISEMES[v]["shapes"], v)
        cells.append(sh.render(v))
        cells.append(sh.render(v + " 3/4", -35.0))
    sheets["visemes"] = Sheet.save(cells, 6, os.path.join(out, stem + "_visemes.png"))
    cells = []
    for e in fs.EXPRESSION_ORDER:
        for i in (2, 4):
            set_face(arm, head, {k: x * 0.2 * i for k, x in fs.EXPRESSIONS[e].items()})
            cells.append(sh.render("%s %d" % (e, i)))
    for e in ("warm", "sad", "angry", "afraid"):
        for v in ("aa", "ou", "PP"):
            w = fs.layer_weights({k: x * 0.8 for k, x in fs.EXPRESSIONS[e].items()}, fs.VISEMES[v]["shapes"], 1.0)
            set_face(arm, head, {k: x for k, x in w.items() if x > 0}, v)
            cells.append(sh.render("%s 4+%s" % (e, v)))
    sheets["expressions"] = Sheet.save(cells, 8, os.path.join(out, stem + "_expressions.png"))
    # close-up: ~512 px per head, every expression at its full preset weights and at 50 %, frontal
    # (pairs side by side: the 50 % cell is what a face mostly shows mid-line, so it has to read too)
    sh.zoom(max(cell, 512), 0.68, -0.08)
    cells = []
    for e in fs.EXPRESSION_ORDER:
        for f in (1.0, 0.5):
            set_face(arm, head, {k: x * f for k, x in fs.EXPRESSIONS[e].items()})
            cells.append(sh.render(e if f == 1.0 else e + " 50%"))
    sheets["expressions_closeup"] = Sheet.save(cells, 4, os.path.join(out, stem + "_expressions_closeup.png"))
    sh.zoom(cell, 1.0)
    set_face(arm, head, {})
    cells = [sh.render("%d deg" % a, float(a)) for a in range(0, 360, 45)]
    sheets["turntable"] = Sheet.save(cells, 8, os.path.join(out, stem + "_turntable.png"))
    sheets["rom"] = None  # the body ROM sheet is P1b (it needs the pilot body); ROM is measured, not drawn, here
    shutil.rmtree(sh.tmp, ignore_errors=True)
    return sheets


# ------------------------------------------------------------------------------------------------------------
def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("asset")
    ap.add_argument("--template", required=True)
    ap.add_argument("--presets", default=None)
    ap.add_argument("--out", required=True)
    ap.add_argument("--no-sheets", action="store_true")
    ap.add_argument("--no-face-rom", action="store_true")
    ap.add_argument("--samples", type=int, default=16)
    ap.add_argument("--cell", type=int, default=192)
    ap.add_argument("--export", default=None, help="also export with the rig profile + write the .rigexpect.json")
    a = ap.parse_args(argv)
    tpl = json.load(open(a.template))
    if a.presets:  # the validator runs on the same spec the engine loads
        pj = json.load(open(a.presets))
        assert [s["name"] for s in pj["shapes"]] == fs.ALL_SHAPES, "face_presets.json differs from face_spec.py"
    os.makedirs(a.out, exist_ok=True)
    stem = os.path.splitext(os.path.basename(a.asset))[0]
    kind = load_asset(a.asset)
    glb_doc = nb.read_glb_json(a.asset) if kind == "glb" else None
    arm, meshes = find_rig()
    head = face_mesh(meshes)
    R = Report()
    body_checks(R, arm, meshes, tpl, kind, glb_doc)
    face = {}
    if head is None:
        R.add("F11.names", "SKIP", "no mesh with face shape keys")
    else:
        base, _ = face_static(R, head, a.presets, kind, glb_doc)
        if a.no_face_rom:
            R.add("F12", "SKIP", "--no-face-rom")
        else:
            face = face_rom(R, arm, head, base) or {}
    rom = rom_pass(R, arm, meshes, head)
    sheets = {}
    if not a.no_sheets and head is not None:
        sheets = contact_sheets(arm, head, meshes, os.path.abspath(a.out), stem, a.cell, a.samples)
    export = None
    if a.export:
        if kind != "blend":
            R.add("EXPORT", "SKIP", "--export needs a .blend input")
        else:
            if head:
                set_face(arm, head, {})
                restore_face_anim(head)
            arm.animation_data.action = None
            rc.reset_pose(arm)
            bpy.context.view_layer.update()
            glb = nb.export_rig(os.path.abspath(a.export), {"export_morph_normal": False,
                                                            "export_force_sampling": False})
            rx = os.path.splitext(glb)[0] + ".rigexpect.json"
            if bpy.data.actions.get("rom") and (head or meshes):
                rc.write_rigexpect(arm, head or meshes[0], rx)
            export = {"glb": glb, "rigexpect": rx}
    report = {
        "asset": os.path.abspath(a.asset),
        "input": kind,
        "template": os.path.abspath(a.template),
        "template_order_hash": tpl["order_hash"],
        "status": R.status(),
        "checks": R.checks,
        "face_rom": face,
        "rom": rom,
        "sheets": sheets,
        "export": export,
        "not_measured_here": ["R1-R5 segment volume, ring area, finger penetration (need a body mesh)",
                              "R10/R13 engine round-trip (tools/ne_rig_check on the exported .glb)",
                              "R14 (skinned_demo --model, P1b)", "F10 (blind review of the expression sheet)",
                              "F12 depth is a plane-distance estimate on overlapping polygon pairs, not exact"],
    }
    path = os.path.join(a.out, stem + ".rigreport.json")
    nb.write_json(path, report)
    for c in R.checks:
        print("%-22s %-4s %s" % (c["id"], c["status"], c["detail"]))
    print("%s -> %s (%s)" % (a.asset, path, report["status"]))
    for k, v in sheets.items():
        print("sheet", k, v)
    return 0 if report["status"] != "FAIL" else 1


if __name__ == "__main__":
    sys.exit(main())
