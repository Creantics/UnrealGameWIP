"""Turns hull_from_views.py's occupancy grid (.npy + .json) into a smooth closed surface.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/hull_remesh.py -- hull.npy \
        --out hull.blend [--glb hull.glb] [--voxel 0.004] [--smooth 4] [--preview DIR]

The grid goes through OpenVDB into a volume object, whose 0.5 level set becomes the mesh (no voxel steps, since
the grid is blurred), then a voxel remesh for even quads and a short Laplacian smooth. --preview renders front,
side, back and three-quarter views in plain solid shading, orthographic, framed on the whole figure.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bpy  # noqa: E402


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:]
    a = {"grid": argv[0], "out": None, "glb": None, "voxel": "0.004", "smooth": "4", "preview": None}
    i = 1
    while i < len(argv):
        k = argv[i][2:]
        if k not in a:
            raise SystemExit("hull_remesh: unknown argument %s" % argv[i])
        a[k] = argv[i + 1]
        i += 2
    return a


def preview(obj, outdir, tag="hull"):
    os.makedirs(outdir, exist_ok=True)
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_WORKBENCH"
    sc.display.shading.light = "STUDIO"
    sc.display.shading.color_type = "SINGLE"
    sc.render.resolution_x, sc.render.resolution_y = 768, 1024
    lo = [min(v[i] for v in obj.bound_box) for i in range(3)]
    hi = [max(v[i] for v in obj.bound_box) for i in range(3)]
    cz = 0.5 * (lo[2] + hi[2])
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = 1.08 * max(hi[2] - lo[2], (hi[0] - lo[0]) * 1024 / 768)
    sc.collection.objects.link(cam)
    sc.camera = cam
    for name, ang in (("front", 0), ("side", 90), ("back", 180), ("threequarter", 35)):
        r = math.radians(ang)
        cam.location = (3.0 * math.sin(r), -3.0 * math.cos(r), cz)
        cam.rotation_euler = (math.radians(90), 0.0, r)
        bpy.context.view_layer.update()
        sc.render.filepath = os.path.join(outdir, "%s_%s.png" % (tag, name))
        bpy.ops.render.render(write_still=True)
    bpy.data.objects.remove(cam, do_unlink=True)


def grid_to_mesh(npy):
    import json
    import numpy as np
    import openvdb
    meta = json.load(open(os.path.splitext(npy)[0] + ".json"))
    grid = openvdb.FloatGrid()
    grid.copyFromArray(np.load(npy).astype(np.float32))
    grid.transform = openvdb.createLinearTransform(voxelSize=meta["voxel"])
    grid.name = "density"
    vdb = os.path.splitext(os.path.abspath(npy))[0] + ".vdb"
    openvdb.write(vdb, grids=[grid])
    vol = bpy.data.objects.new("hull_volume", bpy.data.volumes.new("hull_volume"))
    vol.data.filepath = vdb
    bpy.context.scene.collection.objects.link(vol)
    me = bpy.data.objects.new("hull", bpy.data.meshes.new("hull"))
    bpy.context.scene.collection.objects.link(me)
    m = me.modifiers.new("v2m", "VOLUME_TO_MESH")
    m.object = vol
    m.grid_name = "density"
    m.threshold = 0.5
    m.resolution_mode = "GRID"
    bpy.context.view_layer.objects.active = me
    me.select_set(True)
    bpy.ops.object.modifier_apply(modifier=m.name)
    bpy.data.objects.remove(vol, do_unlink=True)
    me.location = meta["origin"]
    bpy.ops.object.transform_apply(location=True, rotation=False, scale=False)
    return me


def main():
    a = _args()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    obj = grid_to_mesh(a["grid"])
    m = obj.modifiers.new("remesh", "REMESH")
    m.mode = "VOXEL"
    m.voxel_size = float(a["voxel"])
    m.adaptivity = 0.0
    bpy.ops.object.modifier_apply(modifier=m.name)
    s = obj.modifiers.new("smooth", "LAPLACIANSMOOTH")
    s.iterations = int(a["smooth"])
    s.lambda_factor = 0.5
    s.use_volume_preserve = True
    bpy.ops.object.modifier_apply(modifier=s.name)
    for p in obj.data.polygons:
        p.use_smooth = True
    print("hull_remesh: %d verts, %d faces" % (len(obj.data.vertices), len(obj.data.polygons)))
    if a["preview"]:
        preview(obj, a["preview"])
    if a["glb"]:
        bpy.ops.export_scene.gltf(filepath=a["glb"], export_format="GLB", use_selection=False)
    if a["out"]:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(a["out"]))


if __name__ == "__main__":
    main()
