"""Joins separate rigid parts (hair cards, glasses) into a rigged lead, each weighted fully to one joint.

    blender -b RIG.blend --factory-startup --python-exit-code 1 --python tools/blender/attach_rigid.py -- \\
        --part hair.glb:head [--part glasses.glb:head ...] --out name.glb [--face 1] [--blend name.blend]

RIG.blend is rig_scan's (--face 0) or face_transfer's (--face 1) .blend. Each part must already sit in the
rigged mesh's frame (the frame prep_scan left it in). Thin parts like hair cards can't go through prep_scan's
voxel remesh, and heat weighting fails on loose pieces, so they join here: the part keeps its own materials,
every vertex goes to the joint with weight 1, and on a mesh with face shapes its vertices are still in every
shape (with no delta). The glb is checked like the input's.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bpy  # noqa: E402
from mathutils import Matrix  # noqa: E402

import make_leads as ml  # noqa: E402
import ne_blender as nb  # noqa: E402
import rig_common as rc  # noqa: E402


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    a = {"part": [], "out": None, "face": "0", "blend": None}
    i = 0
    while i < len(argv):
        k = argv[i]
        if k.startswith("--") and k[2:] in a and i + 1 < len(argv):
            if k == "--part":
                a["part"].append(argv[i + 1])
            else:
                a[k[2:]] = argv[i + 1]
            i += 2
            continue
        raise SystemExit("attach_rigid: unknown argument %s" % k)
    if not (a["part"] and a["out"]):
        raise SystemExit(__doc__)
    a["face"] = a["face"] not in ("0", "false", "no")
    return a


def import_part(path):
    """The part's meshes as one object with world transforms applied."""
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=os.path.abspath(path))
    new = [o for o in bpy.data.objects if o not in before]
    meshes = [o for o in new if o.type == "MESH"]
    if not meshes:
        raise SystemExit("attach_rigid: no mesh in %s" % path)
    for o in meshes:
        o.data.transform(o.matrix_world)
        o.parent = None
        o.matrix_world = Matrix()
    for o in new:
        if o not in meshes:
            bpy.data.objects.remove(o, do_unlink=True)
    if len(meshes) > 1:
        nb.activate(meshes[0])
        for o in meshes[1:]:
            o.select_set(True)
        bpy.ops.object.join()
    return meshes[0]


def main():
    a = _args()
    arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
    mesh = next(o for o in bpy.data.objects if o.type == "MESH" and any(m.type == "ARMATURE" for m in o.modifiers))
    for o in [o for o in bpy.data.objects if o.type == "MESH" and o is not mesh]:
        bpy.data.objects.remove(o, do_unlink=True)
    added = {}
    for spec in a["part"]:
        path, _, joint = spec.rpartition(":")
        if not path or joint not in arm.data.bones:
            raise SystemExit("attach_rigid: give --part FILE:JOINT with a joint of %s (got %s)" % (arm.name, spec))
        part = import_part(path)
        part.vertex_groups.new(name=joint).add(range(len(part.data.vertices)), 1.0, "REPLACE")
        added[os.path.basename(path)] = (len(part.data.vertices), joint)
        nb.activate(mesh)
        part.select_set(True)
        bpy.ops.object.join()
    rc.reset_pose(arm)
    bpy.context.scene.frame_set(0)
    bpy.context.view_layer.update()
    out = os.path.abspath(a["out"])
    os.makedirs(os.path.dirname(out), exist_ok=True)
    glb = nb.export_rig(out, {"export_morph_normal": False, "export_force_sampling": False})
    rc.write_rigexpect(arm, mesh, os.path.splitext(out)[0] + ".rigexpect.json")
    info = ml.check_glb(glb, face=a["face"])
    if a["blend"]:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(a["blend"]))
    print("attach_rigid: %s, parts %s, %d bytes -> %s" % (info, added, os.path.getsize(glb), glb))


if __name__ == "__main__":
    main()
