"""Appends chosen objects (with their child objects) from a library .blend into a fresh file, and previews them.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/base_mesh_pick.py -- LIB.blend OUT.blend \\
        "Object A" "Object B" ... [--preview DIR] [--levels]

Used to take base meshes out of a downloaded asset bundle (e.g. Blender Studio's Human Base Meshes) without
working in the downloaded file itself; open that with --factory-startup so its scripts never run. --levels
prints each mesh's multires levels. --preview renders each picked root object front and side, solid shading.
"""
import math
import os
import sys

import bpy


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:]
    lib, out, names, preview, levels = argv[0], argv[1], [], None, False
    i = 2
    while i < len(argv):
        if argv[i] == "--preview":
            preview = argv[i + 1]
            i += 2
        elif argv[i] == "--levels":
            levels = True
            i += 1
        else:
            names.append(argv[i])
            i += 1
    return lib, out, names, preview, levels


def render(obj, outdir):
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_WORKBENCH"
    sc.display.shading.light = "STUDIO"
    sc.render.resolution_x, sc.render.resolution_y = 768, 1024
    objs = [obj] + list(obj.children_recursive)
    from mathutils import Vector
    pts = [o.matrix_world @ Vector(c) for o in objs if o.type == "MESH" for c in o.bound_box]
    lo = Vector([min(p[i] for p in pts) for i in range(3)])
    hi = Vector([max(p[i] for p in pts) for i in range(3)])
    c = 0.5 * (lo + hi)
    cam = bpy.data.objects.new("pick_cam", bpy.data.cameras.new("pick_cam"))
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = 1.1 * max(hi.z - lo.z, (hi.x - lo.x) * 1024 / 768, (hi.y - lo.y) * 1024 / 768)
    sc.collection.objects.link(cam)
    sc.camera = cam
    for o in sc.objects:
        o.hide_render = o.type == "MESH" and o not in objs
    tag = obj.name.replace(" ", "_").replace("-", "")
    for name, ang in (("front", 0), ("side", 90), ("threequarter", 35)):
        r = math.radians(ang)
        cam.location = (c.x + 5 * math.sin(r), c.y - 5 * math.cos(r), c.z)
        cam.rotation_euler = (math.radians(90), 0, r)
        sc.render.filepath = os.path.join(outdir, "%s_%s.png" % (tag, name))
        bpy.ops.render.render(write_still=True)
    bpy.data.objects.remove(cam, do_unlink=True)
    for o in sc.objects:
        o.hide_render = False


def main():
    lib, out, names, preview, levels = _args()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    with bpy.data.libraries.load(lib, link=False) as (src, dst):
        dst.objects = list(src.objects)
    # keep the picked roots and their descendants; drop the rest
    keep = set()
    for o in bpy.data.objects:
        if o.name in names:
            keep.add(o)
            keep.update(o.children_recursive)
    missing = [n for n in names if n not in bpy.data.objects]
    if missing:
        raise SystemExit("base_mesh_pick: not in the library: %s" % missing)
    for o in list(bpy.data.objects):
        if o not in keep:
            bpy.data.objects.remove(o, do_unlink=True)
    for o in keep:
        bpy.context.scene.collection.objects.link(o)
        if levels and o.type == "MESH":
            mr = [m for m in o.modifiers if m.type == "MULTIRES"]
            print("base_mesh_pick: %s v%d multires %s" % (o.name, len(o.data.vertices),
                                                          [(m.levels, m.sculpt_levels, m.render_levels, m.total_levels) for m in mr]))
    bpy.ops.outliner.orphans_purge(do_recursive=True)
    if preview:
        os.makedirs(preview, exist_ok=True)
        for n in names:
            render(bpy.data.objects[n], preview)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(out))
    print("base_mesh_pick: %d objects -> %s" % (len(keep), out))


if __name__ == "__main__":
    main()
