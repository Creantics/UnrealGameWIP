"""Headless auto-rig for a single-mesh A-pose humanoid (a Tripo glb, an fbx or an obj): finds the joints on the
mesh, builds an Unreal-mannequin skeleton (names, hierarchy, axes, twist bones, finger metacarpals), binds the
mesh with voxel-geodesic weights and exports an FBX that FTImportCharacter takes. The work is in autorig_core.py.

  blender -b --factory-startup --python-exit-code 1 --python tools/blender/ft_autorig.py -- \
      in.glb out.fbx [--height 1.80] [--guides guides.json] [--max-infl 8] [--voxels 160] \
      [--report report.json] [--blend out.blend] [--no-hem] [--no-twist] [--heat] [--keep-weights]

--height 0 keeps the mesh's own size. --guides is a json {joint name: [x, y, z]} in the output frame (metres,
Z up, facing -Y, soles on z = 0, after --height scaling) that overrides detected landmarks. --heat binds with
Blender's heat weights instead (for comparisons). --keep-weights takes the joints and skin weights from a
rig the source file already carries (a Tripo export with mannequin bone names): it rebuilds the skeleton with
the mannequin's axes, twist and ik bones, then repairs the weights (hem rule, smoothing, twist bands, influence
limit, normalise) instead of binding from scratch. Bone axes: each Blender bone's Y (head to tail) is the
mannequin bone's Y and its X the mannequin's X, so the FBX exporter's default axis mapping writes the
mannequin's frames.
"""
import json
import os
import sys
import time

import bpy
import numpy as np
from mathutils import Matrix, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import autorig_core as ac  # noqa: E402


def args():
    a = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    o = dict(height=1.80, guides=None, max_infl=8, voxels=160, report=None, blend=None, hem=True, twist=True,
             heat=False, keep=False)
    pos = []
    i = 0
    while i < len(a):
        k = a[i]
        if k == "--height":
            o["height"] = float(a[i + 1]); i += 1
        elif k == "--guides":
            o["guides"] = a[i + 1]; i += 1
        elif k == "--max-infl":
            o["max_infl"] = int(a[i + 1]); i += 1
        elif k == "--voxels":
            o["voxels"] = int(a[i + 1]); i += 1
        elif k == "--report":
            o["report"] = a[i + 1]; i += 1
        elif k == "--blend":
            o["blend"] = a[i + 1]; i += 1
        elif k == "--no-hem":
            o["hem"] = False
        elif k == "--no-twist":
            o["twist"] = False
        elif k == "--heat":
            o["heat"] = True
        elif k == "--keep-weights":
            o["keep"] = True
        else:
            pos.append(k)
        i += 1
    if len(pos) != 2:
        raise SystemExit("usage: ft_autorig.py -- in.(glb|gltf|fbx|obj) out.fbx [options]")
    o["src"], o["out"] = pos
    return o


def load_mesh(path, keep=False):
    """-> mesh object (joined, transforms applied, rig removed). keep: also -> the source rig as
    ({bone: (head, tail)} in world space, [group names], weights array) from the one skinned mesh; other meshes
    (bone display shapes) are dropped."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    ext = os.path.splitext(path)[1].lower()
    if ext in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=path)
    elif ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=path)
    elif ext == ".obj":
        bpy.ops.wm.obj_import(filepath=path)
    else:
        raise SystemExit("unsupported input " + ext)
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    if not meshes:
        raise SystemExit("no mesh in " + path)
    src_rig = None
    if keep:
        skinned = [o for o in meshes if any(m.type == "ARMATURE" and m.object for m in o.modifiers)]
        if len(skinned) != 1:
            raise SystemExit("--keep-weights needs exactly one skinned mesh, found %d" % len(skinned))
        ob = skinned[0]
        arm = [m.object for m in ob.modifiers if m.type == "ARMATURE"][0]
        AW = arm.matrix_world
        bones = {b.name: (np.array(AW @ b.head_local), np.array(AW @ b.tail_local)) for b in arm.data.bones}
        gnames = [g.name for g in ob.vertex_groups]
        Wk = np.zeros((len(ob.data.vertices), len(gnames)))
        for v in ob.data.vertices:
            for g in v.groups:
                Wk[v.index, g.group] = g.weight
        src_rig = (bones, gnames, Wk)
        for o in meshes:
            if o is not ob:
                bpy.data.objects.remove(o, do_unlink=True)
        meshes = [ob]
    for o in meshes:  # bake parents and transforms, drop any rig the file came with
        mw = o.matrix_world.copy()
        for m in [m for m in o.modifiers if m.type == "ARMATURE"]:
            o.modifiers.remove(m)
        o.parent = None
        o.matrix_world = mw
        o.vertex_groups.clear()
        if o.data.shape_keys:
            o.shape_key_clear()
    for o in list(bpy.data.objects):
        if o.type != "MESH":
            bpy.data.objects.remove(o, do_unlink=True)
    bpy.ops.object.select_all(action="DESELECT")
    for o in meshes:
        o.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    if len(meshes) > 1:
        bpy.ops.object.join()
    ob = bpy.context.view_layer.objects.active
    return (ob, src_rig) if keep else ob


def mesh_arrays(ob):
    me = ob.data
    me.calc_loop_triangles()
    V = np.empty(len(me.vertices) * 3)
    me.vertices.foreach_get("co", V)
    F = np.empty(len(me.loop_triangles) * 3, dtype=np.int64)
    me.loop_triangles.foreach_get("vertices", F)
    return V.reshape(-1, 3), F.reshape(-1, 3)


def build_armature(J, name="Armature", along=None):
    """along: {name: direction} builds plain Blender bones (tail along the limb) instead, for heat weighting."""
    arm = bpy.data.armatures.new(name)
    ob = bpy.data.objects.new(name, arm)
    bpy.context.scene.collection.objects.link(ob)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.mode_set(mode="EDIT")
    for n in ac.NAMES:
        h, t, y = J[n]
        x = ac.unit(t - h)
        length = float(np.linalg.norm(t - h))
        eb = arm.edit_bones.new(n)
        eb.head = Vector(h)
        if along is not None:
            eb.tail = Vector(h + along[n] * length)
        else:
            eb.tail = Vector(h + y * length)  # Blender's bone Y carries the mannequin's Y
            eb.align_roll(Vector(np.cross(x, y)))  # then Blender's X is the mannequin's X
        eb.use_deform = ac.DEFORM[n]
    for n in ac.NAMES:
        p = ac.PARENT[n]
        if p:
            arm.edit_bones[n].parent = arm.edit_bones[p]
            arm.edit_bones[n].use_connect = False
    bpy.ops.object.mode_set(mode="OBJECT")
    return ob


def axis_error(arm_ob, J):
    """Largest angle (degrees) between a built bone's X/Y and the wanted mannequin X/Y."""
    worst = 0.0
    for n in ac.NAMES:
        h, t, y = J[n]
        x = ac.unit(t - h)
        M = arm_ob.data.bones[n].matrix_local.to_3x3()
        bx, by = np.array(M.col[0]), np.array(M.col[1])
        for a, b in ((bx, x), (by, y)):
            worst = max(worst, float(np.degrees(np.arccos(np.clip(np.dot(ac.unit(a), b), -1, 1)))))
    return worst


def main():
    o = args()
    t0 = time.time()
    log = []

    def say(m):
        log.append(m)
        print("ft_autorig:", m)
    if o["keep"]:
        ob, src_rig = load_mesh(o["src"], keep=True)
    else:
        ob, src_rig = load_mesh(o["src"]), None
    V, F = mesh_arrays(ob)
    off, turn = ac.normalize(V)
    M = Matrix.Translation(Vector(off))
    if turn:
        M = M @ Matrix.Rotation(np.pi, 4, "Z")
    ob.data.transform(M)
    V, F = mesh_arrays(ob)
    H0 = float(V[:, 2].max())
    scale = o["height"] / H0 if o["height"] > 0 else 1.0
    if scale != 1.0:
        ob.data.transform(Matrix.Scale(scale, 4))
        V = V * scale
    ob.data.update()
    say("%d vertices, %d triangles, height %.3f -> %.3f m%s" % (len(V), len(F), H0, H0 * scale,
                                                             ", turned to face -Y" if turn else ""))
    guides = None
    if o["guides"]:
        guides = {k: np.asarray(v, float) for k, v in json.load(open(o["guides"])).items()}
    L, info = ac.find_landmarks(V, F, voxels=o["voxels"], guides=guides, log=say)
    if src_rig is not None:
        # the source rig's joints replace the detected ones; detection still supplies the crotch, legs and arm
        # vertices the hem rule needs
        bones, gnames, Wk = src_rig
        Mn = np.array(Matrix.Scale(scale, 4) @ M)

        def tr(p):
            return Mn[:3, :3] @ p + Mn[:3, 3]
        missing = [n for n in L if n not in bones]
        if missing:
            raise SystemExit("source rig lacks mannequin bones: " + ", ".join(sorted(missing)[:12]))
        for n in L:
            L[n] = tr(bones[n][0])
        for s in "lr":
            info["toe_" + s] = tr(bones["ball_" + s][1])
            for f in ("index", "middle", "ring", "pinky"):
                info["hand"][s]["tip_" + f] = tr(bones[f + "_03_" + s][1])
            info["hand"][s]["thumb_tip"] = tr(bones["thumb_03_" + s][1])
        info["src"] = {n: "source rig" for n in L}
        # a skirt or loincloth can hide the leg gap, so the crotch may be detected too high; the source rig's
        # hips bound it (the crotch sits below the hip joints)
        hip_z = 0.5 * (L["thigh_l"][2] + L["thigh_r"][2])
        if info["crotch"] > hip_z - 0.03 * info["H"]:
            say("crotch %.3f is above the source hips, set to %.3f" % (info["crotch"], hip_z - 0.03 * info["H"]))
            info["crotch"] = hip_z - 0.03 * info["H"]
        say("--keep-weights: joints and %d weight groups from the source rig" % len(gnames))
    t1 = time.time()
    J, dirs = ac.build_skeleton(L, info)
    arm_ob = build_armature(J, along=dirs if o["heat"] else None)
    aerr = 0.0 if o["heat"] else axis_error(arm_ob, J)
    if aerr > 0.5:
        raise SystemExit("bone axes off by %.2f degrees" % aerr)
    ob.name = os.path.splitext(os.path.basename(o["out"]))[0]
    ob.data.name = ob.name
    if o["heat"]:
        # the repo's current method for comparison: Blender heat weights, a voxel proxy where heat fails
        import rig_common as rc
        rc.auto_weights(ob, arm_ob)
        names = [g.name for g in ob.vertex_groups]
        say("heat weights (rig_common.auto_weights): %d groups; bones are plain Blender bones, not mannequin axes"
            % len(names))
        infl = np.array([sum(1 for g in v.groups if g.weight > 1e-4) for v in ob.data.vertices])
    else:
        if src_rig is not None:
            use = [i for i, n in enumerate(gnames) if n in ac.DEFORM and ac.DEFORM[n]]
            dropped = [n for n in gnames if not (n in ac.DEFORM and ac.DEFORM[n])]
            if dropped:
                say("source groups with no mannequin deform bone, dropped: " + ", ".join(dropped[:10]))
            Wk = Wk[:, use]
            s_ = Wk.sum(1, keepdims=True)
            say("source weights: %d vertices unweighted, row sums %.3f..%.3f, up to %d influences"
                % (int((s_[:, 0] < 1e-6).sum()), float(s_.min()), float(s_.max()), int((Wk > 1e-4).sum(1).max())))
            Wk = Wk / np.maximum(s_, 1e-12)
            names, W = ac.finish_weights(V, F, J, info, [gnames[i] for i in use], Wk, max_infl=o["max_infl"],
                                         twist=o["twist"], hem=o["hem"], log=say)
        else:
            names, W = ac.bind(V, F, J, dirs, info, max_infl=o["max_infl"], twist=o["twist"], hem=o["hem"],
                               log=say)
        for b, n in enumerate(names):
            vg = ob.vertex_groups.new(name=n)
            nz = np.nonzero(W[:, b])[0]
            if len(nz) == 0:
                continue
            # group vertices by weight rounded to 1e-4 so add() runs once per distinct value
            w = np.round(W[nz, b], 4)
            order = np.argsort(w)
            ws, idx = w[order], nz[order]
            cuts = np.nonzero(np.diff(ws))[0] + 1
            for seg_w, seg_i in zip(np.split(ws, cuts), np.split(idx, cuts)):
                vg.add(seg_i.tolist(), float(seg_w[0]), "REPLACE")
        mod = ob.modifiers.new("Armature", "ARMATURE")
        mod.object = arm_ob
        ob.parent = arm_ob
        infl = (W > 0).sum(1)
    t2 = time.time()
    if o["blend"]:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(o["blend"]))
    bpy.ops.object.select_all(action="DESELECT")
    ob.select_set(True)
    arm_ob.select_set(True)
    bpy.context.view_layer.objects.active = arm_ob
    os.makedirs(os.path.dirname(os.path.abspath(o["out"])), exist_ok=True)
    bpy.ops.export_scene.fbx(filepath=os.path.abspath(o["out"]), use_selection=True,
                             object_types={"ARMATURE", "MESH"}, add_leaf_bones=False, bake_anim=False,
                             apply_scale_options="FBX_SCALE_ALL", mesh_smooth_type="FACE",
                             primary_bone_axis="Y", secondary_bone_axis="X", path_mode="COPY",
                             embed_textures=True, use_armature_deform_only=False)
    rep = dict(src=o["src"], out=o["out"], height=H0 * scale, scale=scale, turned=turn, vertices=len(V),
               bones=len(ac.NAMES), weighted_groups=len([n for n in names]), axis_error_deg=aerr,
               influences=np.bincount(infl).tolist(), landmarks={k: [round(float(c), 4) for c in v] for k, v in L.items()},
               source=info["src"], crotch=info["crotch"], hem=info.get("hem"), log=log,
               seconds=dict(landmarks=round(t1 - t0, 1), bind=round(t2 - t1, 1), total=round(time.time() - t0, 1)))
    if o["report"]:
        json.dump(rep, open(o["report"], "w"), indent=1)
    say("done in %.1f s (landmarks %.1f, bind %.1f): %s" % (time.time() - t0, t1 - t0, t2 - t1, o["out"]))


main()
