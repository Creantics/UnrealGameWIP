"""Rigs a mesh from prep_scan.py on the canonical skeleton and gives it the lead clips (idle, rom, walk).

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/rig_scan.py -- PREPPED.glb \\
        --landmarks lm.json --out name.glb [--blend name.blend] [--idle-arm -30] [--walk-arm -32] [--skirt 0.6,1.08,0.6,0.3] [--preview DIR]

fit_canonical fits the joints to the 14 landmarks and skins the mesh. Its `tpose` clip is then swapped for
make_leads' idle and walk, so the clips match the other leads exactly (check_glb). The face joints get no
weights and the mesh has no face shapes: import it with FTImportCharacter -Face=0. --idle-arm and
--walk-arm lower the upper arms (degrees) from the scan's A-pose. --skirt HEM,TOP,FOLLOW,HALF_WIDTH reweights a tunic
or skirt hanging between those heights (see skirt_weights). --preview renders the rest pose in x-ray
with a red dot on each landmark joint, plus walk frames 0 and 8, front and side, into DIR.
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bpy  # noqa: E402

import fit_canonical as fc  # noqa: E402
import make_leads as ml  # noqa: E402
import ne_blender as nb  # noqa: E402
import rig_common as rc  # noqa: E402
import rig_fit  # noqa: E402


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    a = {"mesh": None, "landmarks": None, "out": None, "blend": None, "idle-arm": -30.0, "walk-arm": -32.0,
         "preview": None, "skirt": None}
    i = 0
    while i < len(argv):
        k = argv[i]
        if k.startswith("--") and k[2:] in a:
            a[k[2:]] = argv[i + 1]
            i += 2
            continue
        if not k.startswith("--") and a["mesh"] is None:
            a["mesh"] = k
        else:
            raise SystemExit("rig_scan: unknown argument %s" % k)
        i += 1
    if not (a["mesh"] and a["landmarks"] and a["out"]):
        raise SystemExit(__doc__)
    a["idle-arm"], a["walk-arm"] = float(a["idle-arm"]), float(a["walk-arm"])
    if a["skirt"]:
        a["skirt"] = [float(x) for x in a["skirt"].split(",")]
    return a


def skirt_weights(mesh, hem, top, follow, half_width, split=0.10):
    """Reweights a tunic or skirt between z = hem and z = top so it doesn't tear between the legs: pelvis
    1 - a and the thighs a, with a rising from 0 at top to `follow` at the hem, split softly across x = 0
    over `split` metres. Below the hem (the legs) and beyond |x| = half_width (the hands) the fitted weights stay."""
    groups = {n: mesh.vertex_groups.get(n) or mesh.vertex_groups.new(name=n) for n in ("pelvis", "thigh_l", "thigh_r")}
    changed = 0
    for v in mesh.data.vertices:
        z = v.co.z
        if not hem <= z <= top or abs(v.co.x) > half_width:
            continue
        t = (top - z) / (top - hem)
        a = follow * t * t * (3 - 2 * t)
        f = min(1.0, max(0.0, 0.5 + v.co.x / split))
        for g in v.groups:
            mesh.vertex_groups[g.group].remove([v.index])
        for n, w in (("pelvis", 1.0 - a), ("thigh_l", a * f), ("thigh_r", a * (1.0 - f))):
            if w > 0.01:
                groups[n].add([v.index], w, "REPLACE")
        changed += 1
    return changed


def preview(arm, mesh, outdir):
    os.makedirs(outdir, exist_ok=True)
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_WORKBENCH"
    sc.display.shading.light = "STUDIO"
    sc.display.shading.color_type = "TEXTURE"
    sc.render.resolution_x, sc.render.resolution_y = 768, 768
    top = max((mesh.matrix_world @ v.co).z for v in mesh.data.vertices)
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    cam.data.type, cam.data.ortho_scale = "ORTHO", top * 1.1
    sc.collection.objects.link(cam)
    sc.camera = cam
    red = bpy.data.materials.new("dot")
    red.diffuse_color = (1.0, 0.0, 0.0, 1.0)
    dots = []
    for n in rig_fit.REQUIRED:
        bpy.ops.mesh.primitive_uv_sphere_add(radius=0.018, location=arm.matrix_world @ arm.data.bones[n].head_local)
        d = bpy.context.active_object
        d.data.materials.append(red)
        dots.append(d)
    views = {"front": ((0.0, -5.0, top / 2), (math.radians(90), 0.0, 0.0)),
             "side": ((-5.0, 0.0, top / 2), (math.radians(90), 0.0, math.radians(-90)))}

    def shoot(tag):
        for name, (loc, rot) in views.items():
            cam.location, cam.rotation_euler = loc, rot
            sc.render.filepath = os.path.join(outdir, "%s_%s.png" % (tag, name))
            bpy.ops.render.render(write_still=True)

    arm.animation_data.action = None
    rc.reset_pose(arm)
    bpy.context.view_layer.update()
    sc.display.shading.show_xray, sc.display.shading.xray_alpha = True, 0.4
    shoot("rest")
    sc.display.shading.show_xray = False
    for d in dots:
        d.hide_render = True
    arm.animation_data.action = bpy.data.actions["walk"]
    for f in (0, 8):
        sc.frame_set(f)
        shoot("walk%02d" % f)
    arm.animation_data.action = None
    rc.reset_pose(arm)
    for d in dots:
        bpy.data.objects.remove(d, do_unlink=True)
    bpy.data.objects.remove(cam, do_unlink=True)


def main():
    a = _args()
    with open(a["landmarks"]) as f:
        landmarks = json.load(f)
    meshes = fc.import_meshes(a["mesh"])
    if len(meshes) != 1:
        raise SystemExit("rig_scan: expected one mesh (run prep_scan.py first), got %d" % len(meshes))
    mesh = meshes[0]
    # glTF splits vertices along UV seams; welding them back gives heat weighting and the weight mirror one
    # connected surface (UVs live on face corners, so they survive)
    nb.activate(mesh)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.remove_doubles(threshold=1e-5)
    bpy.ops.object.mode_set(mode="OBJECT")
    mesh.name = os.path.splitext(os.path.basename(a["out"]))[0]
    arm, report = fc.fit(meshes, landmarks)
    if a["skirt"]:
        report["skirt_vertices"] = skirt_weights(mesh, *a["skirt"])
    bpy.data.actions.remove(bpy.data.actions["tpose"])
    joints = rc.body_joints(arm)
    ml.make_idle(arm, joints, a["idle-arm"])
    ml.make_walk(arm, joints, a["walk-arm"])
    arm.animation_data.action = None
    rc.reset_pose(arm)
    bpy.context.scene.frame_set(0)
    bpy.context.view_layer.update()
    out = os.path.abspath(a["out"])
    os.makedirs(os.path.dirname(out), exist_ok=True)
    glb = nb.export_rig(out, {"export_morph_normal": False, "export_force_sampling": False})
    rc.write_rigexpect(arm, mesh, os.path.splitext(out)[0] + ".rigexpect.json")
    info = ml.check_glb(glb, face=False)
    if a["blend"]:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(a["blend"]))
    if a["preview"]:
        preview(arm, mesh, a["preview"])
    print("rig_scan: %s, landmark residual %.4f m, weights %s, skirt %s, %d bytes -> %s"
          % (info, report["landmark_residual_m"], report["meshes"][mesh.name], report.get("skirt_vertices"),
             os.path.getsize(glb), glb))


if __name__ == "__main__":
    main()
