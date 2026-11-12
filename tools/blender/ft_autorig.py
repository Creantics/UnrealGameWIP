"""Headless auto-rig for a single-mesh A-pose humanoid (a Tripo glb, an fbx or an obj): finds the joints on the
mesh, builds an Unreal-mannequin skeleton (names, hierarchy, axes, twist bones, finger metacarpals), binds the
mesh with voxel-geodesic weights and exports a .glb (the format FTImportCharacter takes) or an FBX. The work is in autorig_core.py.

  blender -b --factory-startup --python-exit-code 1 --python tools/blender/ft_autorig.py -- \
      in.glb out.glb|out.fbx [--height 1.80] [--guides guides.json] [--max-infl 8] [--max-tex 1024] [--voxels 160] \
      [--report report.json] [--blend out.blend] [--no-hem] [--no-twist] [--heat] [--keep-weights [--hem]]

Twist bones: past a segment's first twist bone the segment bone holds no skin weight (the twist bones hold it,
see autorig_core.finish_weights), so in game that skin rolls only when the twist tracks are driven. FTBuildRig
bakes them after the retarget by default; a clip built with -NoBakeTwist leaves them at the bind pose. Route B
(--keep-weights) depends on that bake as much as the bound route does. --no-twist adds no twist bands: on the
bound route the twist bones get no weight; with --keep-weights the source rig's own twist weights stay as they are.

The output, --report and --blend are written to temporary files next to them and moved over the old ones only
once all of them have been written, so a failed run never leaves a broken file in place of a good one.

--height 0 keeps the mesh's own size. --guides is a json {joint name: [x, y, z]} in the output frame (metres,
Z up, facing -Y, soles on z = 0, after --height scaling) that overrides detected landmarks; a crotch guide
sets only the hem rule's height (the pelvis, spine and thighs come from the detected crotch). --heat binds with
Blender's heat weights instead (for comparisons). --keep-weights takes the joints and skin weights from a
rig the source file already carries (a Tripo export with mannequin bone names): it rebuilds the skeleton with
the mannequin's axes, twist and ik bones, then repairs the weights (hem rule, smoothing, twist bands, influence
limit, normalise) instead of binding from scratch. The hem rule is off there unless --hem is given: with
no landmark detection to trust, cloth and leg armour cannot be told apart reliably (on a test mesh it moved
most of the thigh plates to the pelvis). Bone axes: each Blender bone's Y (head to tail) is the
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


USAGE = "usage: ft_autorig.py -- in.(glb|gltf|fbx|obj) out.(glb|fbx) [options] (see the docstring)"


def args():
    a = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    o = dict(height=1.80, guides=None, max_infl=8, voxels=160, report=None, blend=None, hem=True, twist=True,
             heat=False, keep=False, max_tex=0)
    pos = []
    i = 0

    def value(k, kind, lo, hi=None):
        if i + 1 >= len(a) or a[i + 1].startswith("--"):
            raise SystemExit("%s needs a value. %s" % (k, USAGE))
        try:
            v = kind(a[i + 1])
        except ValueError:
            raise SystemExit("%s takes a %s, got %r" % (k, kind.__name__, a[i + 1]))
        if lo is not None and (v < lo or (hi is not None and v > hi)):
            raise SystemExit("%s must be %s, got %r" % (k, "%s..%s" % (lo, hi) if hi is not None else ">= %s" % lo, v))
        return v
    while i < len(a):
        k = a[i]
        if k == "--height":
            o["height"] = value(k, float, 0.0, 100.0); i += 1
        elif k == "--guides":
            o["guides"] = value(k, str, None); i += 1
        elif k == "--max-infl":
            o["max_infl"] = value(k, int, 1, 12); i += 1
        elif k == "--voxels":
            o["voxels"] = value(k, int, 40, 1000); i += 1
        elif k == "--report":
            o["report"] = value(k, str, None); i += 1
        elif k == "--blend":
            o["blend"] = value(k, str, None); i += 1
        elif k == "--no-hem":
            o["hem"] = False
        elif k == "--no-twist":
            o["twist"] = False
        elif k == "--heat":
            o["heat"] = True
        elif k == "--keep-weights":
            o["keep"] = True
        elif k == "--hem":
            o["hem_on"] = True
        elif k == "--max-tex":
            o["max_tex"] = value(k, int, 0); i += 1
        elif k.startswith("--"):
            raise SystemExit("unknown option %s. %s" % (k, USAGE))
        else:
            pos.append(k)
        i += 1
    if len(pos) != 2:
        raise SystemExit(USAGE)
    o["src"], o["out"] = pos
    if not os.path.isfile(o["src"]):
        raise SystemExit("no such input file: " + o["src"])
    if os.path.splitext(o["out"])[1].lower() not in (".glb", ".fbx"):
        raise SystemExit("the output must be a .glb or an .fbx: " + o["out"])
    if o["heat"] and o["keep"]:
        raise SystemExit("--heat and --keep-weights are two different bindings; pick one")
    if o["guides"]:
        if o["keep"]:
            raise SystemExit("--guides has no effect with --keep-weights (the joints come from the source rig)")
        try:
            with open(o["guides"]) as fh:
                o["guides"] = ac.check_guides(json.load(fh))
        except (ValueError, OSError) as e:  # json.JSONDecodeError is a ValueError
            raise SystemExit("guides %s: %s" % (o["guides"], e))
    for k in ("out", "report", "blend"):
        if o[k]:
            os.makedirs(os.path.dirname(os.path.abspath(o[k])), exist_ok=True)
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
    shapes = {pb.custom_shape for o in bpy.data.objects if o.type == "ARMATURE" for pb in o.pose.bones}
    for o in [o for o in bpy.data.objects if o in shapes]:  # bone display shapes are not the character
        bpy.data.objects.remove(o, do_unlink=True)
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
        if o.data.users > 1:  # instanced mesh data: each object gets its own copy before transforms are applied
            print("ft_autorig: mesh data %s is shared by %d objects; %s takes its own copy"
                  % (o.data.name, o.data.users, o.name))
            o.data = o.data.copy()
        mw = o.matrix_world.copy()
        for m in [m for m in o.modifiers if m.type == "ARMATURE"]:
            o.modifiers.remove(m)
        o.parent = None
        o.matrix_world = mw
        o.vertex_groups.clear()
        if o.data.shape_keys:  # the rig is built on the basis shape; the output carries no morphs
            keys = [k.name for k in o.data.shape_keys.key_blocks]
            print("ft_autorig: WARNING %s: %d shape keys dropped, the basis shape kept (%s)"
                  % (o.name, len(keys), ", ".join(keys[:8])))
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
    if o.get("hem_on") and not o["keep"]:
        say("--hem only applies with --keep-weights (the detected hem rule is on by default; --no-hem turns it off)")
    if o["keep"]:
        ob, src_rig = load_mesh(o["src"], keep=True)
    else:
        ob, src_rig = load_mesh(o["src"]), None
    V, F = mesh_arrays(ob)
    if not len(F):
        raise SystemExit("the mesh in %s has no faces" % o["src"])
    islands = ac.loose_islands(V, F, say)
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
    guides = o["guides"]  # checked in args()
    if src_rig is not None:
        # the source rig's joints replace detection entirely (detection fails on rigs posed with the arms at the
        # body); the crotch comes from the source hips and the hem rule's legs from the source weights
        bones, gnames, Wk = src_rig
        Mn = np.array(Matrix.Scale(scale, 4) @ M)

        def tr(p):
            return Mn[:3, :3] @ p + Mn[:3, 3]
        try:
            L, info = ac.source_landmarks({n: (tr(h), tr(t)) for n, (h, t) in bones.items()}, float(V[:, 2].max()), say)
        except ValueError as e:
            raise SystemExit(str(e))
        say("--keep-weights: joints and %d weight groups from the source rig" % len(gnames))
    else:
        try:
            L, info = ac.find_landmarks(V, F, voxels=o["voxels"], guides=guides, log=say)
        except (ValueError, IndexError) as e:
            raise SystemExit("landmark detection failed: %s. Fix the pose, pass --guides for the joints it misses, or "
                             "for a mesh that carries a mannequin rig use --keep-weights" % e)
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
            # the hem rule works from the source rig, not from detection (which fails on the cloth this rule is
            # for): see autorig_core.source_weights
            try:
                srcn, Wk = ac.source_weights(V, gnames, Wk, L, info, say)
            except ValueError as e:
                raise SystemExit(str(e))
            if o.get("hem_on"):
                say("hem rule from the source rig: %d candidate vertices, leg radius %.3f / %.3f m"
                % (int((info["hem_gate"] & (V[:, 2] < info["crotch"])).sum()), info["leg"]["l"]["r"],
                   info["leg"]["r"]["r"]))
            names, W = ac.finish_weights(V, F, J, info, srcn, Wk, max_infl=o["max_infl"],
                                         twist=o["twist"], hem=o["hem"] and o.get("hem_on", False), log=say)
            # what the repair changed, by the bone that owned each vertex in the source (twists folded into
            # their segments for the comparison)
            base = [n.split("_twist")[0] + n[-2:] if "_twist" in n else n for n in srcn]
            sb = np.array(base)[Wk.argmax(1)]
            nb = np.array([n.split("_twist")[0] + n[-2:] if "_twist" in n else n for n in names])[W.argmax(1)]
            moved = sb != nb
            if moved.any():
                u, c = np.unique(sb[moved], return_counts=True)
                say("dominant bone changed on %d vertices; by source bone: %s" % (int(moved.sum()), ", ".join(
                    "%s %d" % (b_, c_) for b_, c_ in sorted(zip(u, c), key=lambda x: -x[1])[:10])))
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
    rep = dict(src=o["src"], out=o["out"], height=H0 * scale, scale=scale, turned=turn, vertices=len(V),
               bones=len(ac.NAMES), weighted_groups=len(names), axis_error_deg=aerr,
               influences=np.bincount(infl).tolist(), landmarks={k: [round(float(c), 4) for c in v] for k, v in L.items()},
               source=info["src"], crotch=info["crotch"], hem=info.get("hem"), loose_pieces=islands, log=log)
    out = os.path.abspath(o["out"])
    stem, ext = os.path.splitext(out)
    tmp = stem + ".tmp" + ext  # the exporters keep the extension they are given
    temps = [tmp]
    rtmp = btmp = None
    if o["report"]:
        rtmp = os.path.abspath(o["report"]) + ".tmp"
        temps.append(rtmp)
    if o["blend"]:
        bstem, bext = os.path.splitext(os.path.abspath(o["blend"]))
        btmp = bstem + ".tmp" + bext
        temps.append(btmp)
    try:
        export(o, ob, arm_ob, tmp, say)
        rep["seconds"] = dict(landmarks=round(t1 - t0, 1), bind=round(t2 - t1, 1), total=round(time.time() - t0, 1))
        if rtmp:
            with open(rtmp, "w") as fh:
                json.dump(rep, fh, indent=1)
        if btmp:
            bpy.ops.wm.save_as_mainfile(filepath=btmp, copy=True)
        # every file is written; the output goes in first, then what describes it
        os.replace(tmp, out)
        if btmp:
            os.replace(btmp, os.path.abspath(o["blend"]))
        if rtmp:
            os.replace(rtmp, os.path.abspath(o["report"]))
    except BaseException:
        for f in temps:
            if os.path.isfile(f):
                os.remove(f)
        raise
    say("done in %.1f s (landmarks %.1f, bind %.1f): %s" % (time.time() - t0, t1 - t0, t2 - t1, o["out"]))


def export(o, ob, arm_ob, path, say):
    """Exports the rig and mesh to path (.glb through the repo's rig profile, else FBX); textures over --max-tex
    are scaled down first."""
    bpy.ops.object.select_all(action="DESELECT")
    ob.select_set(True)
    arm_ob.select_set(True)
    bpy.context.view_layer.objects.active = arm_ob
    os.makedirs(os.path.dirname(path), exist_ok=True)
    for img in bpy.data.images:  # the import asset rules cap texture size per role (docs/ASSET_INTAKE.md)
        if o["max_tex"] and max(img.size) > o["max_tex"]:
            f = o["max_tex"] / max(img.size)
            say("texture %s %dx%d -> %dx%d" % (img.name, img.size[0], img.size[1], round(img.size[0] * f), round(img.size[1] * f)))
            img.scale(round(img.size[0] * f), round(img.size[1] * f))
    if path.lower().endswith(".glb"):  # the format FTImportCharacter takes (docs/ASSET_INTAKE.md)
        import ne_blender as nb
        nb.export_rig(path, dict(use_selection=True, export_animations=False,
                                                      export_def_bones=False, export_influence_nb=o["max_infl"]))
    else:
        bpy.ops.export_scene.fbx(filepath=path, use_selection=True,
                                 object_types={"ARMATURE", "MESH"}, add_leaf_bones=False, bake_anim=False,
                                 mesh_smooth_type="FACE", primary_bone_axis="Y", secondary_bone_axis="X",
                                 path_mode="COPY", embed_textures=True, use_armature_deform_only=False)
    if not os.path.isfile(path) or os.path.getsize(path) == 0:
        raise RuntimeError("the exporter wrote nothing to " + path)


main()
