"""Render a model to PNG previews (front, three-quarter, side) so it can be checked without opening Blender.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/ft_preview.py -- <model.glb|.blend> <out_dir> [--shape name=weight ...] [--size 768]

Uses the Workbench engine with studio lighting and object colours. The camera frames the model's bounding
box. Shape keys can be set before rendering, to look at one face shape.
"""
import math
import os
import sys

import bpy
from mathutils import Vector


def args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if len(argv) < 2:
        raise SystemExit("usage: ft_preview.py -- <model> <out_dir> [--shape name=weight] [--size N]")
    model, out, shapes, size = argv[0], argv[1], {}, 768
    i = 2
    while i < len(argv):
        if argv[i] == "--shape" and i + 1 < len(argv):
            name, _, w = argv[i + 1].partition("=")
            shapes[name] = float(w or 1.0)
            i += 1
        elif argv[i] == "--size" and i + 1 < len(argv):
            size = int(argv[i + 1])
            i += 1
        i += 1
    return model, out, shapes, size


def load(model):
    if model.lower().endswith(".blend"):
        bpy.ops.wm.open_mainfile(filepath=model)
    else:
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.import_scene.gltf(filepath=model)


def bounds():
    lo, hi = Vector((1e9, 1e9, 1e9)), Vector((-1e9, -1e9, -1e9))
    for obj in bpy.context.scene.objects:
        # The glTF importer adds a hidden Icosphere as the bone display shape; skip anything not rendered.
        if obj.type != "MESH" or obj.hide_render or not obj.visible_get():
            continue
        for corner in obj.bound_box:
            p = obj.matrix_world @ Vector(corner)
            lo = Vector(map(min, lo, p))
            hi = Vector(map(max, hi, p))
    return lo, hi


def set_shapes(shapes):
    found = set()
    for obj in bpy.context.scene.objects:
        keys = obj.data.shape_keys if obj.type == "MESH" and obj.data.shape_keys else None
        if not keys:
            continue
        for kb in keys.key_blocks:
            if kb.name in shapes:
                kb.value = shapes[kb.name]
                found.add(kb.name)
    for name in shapes:
        if name not in found:
            print(f"ft_preview: no shape key '{name}'")


def main():
    model, out, shapes, size = args()
    os.makedirs(out, exist_ok=True)
    load(model)
    set_shapes(shapes)

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "MATERIAL"
    scene.display.shading.show_cavity = True
    scene.display.shading.cavity_type = "BOTH"
    scene.display.shading.show_shadows = True
    scene.render.resolution_x = scene.render.resolution_y = size
    scene.render.film_transparent = False
    scene.world = scene.world or bpy.data.worlds.new("World")

    lo, hi = bounds()
    centre = (lo + hi) / 2
    radius = max((hi - lo).length / 2, 0.01)

    cam_data = bpy.data.cameras.new("PreviewCam")
    cam_data.lens = 85
    cam = bpy.data.objects.new("PreviewCam", cam_data)
    scene.collection.objects.link(cam)
    scene.camera = cam
    dist = radius / math.tan(cam_data.angle / 2) * 1.1

    # Blender front is -Y: the front view looks along +Y from -Y.
    views = {"front": 0.0, "threequarter": 35.0, "side": 90.0}
    stem = os.path.splitext(os.path.basename(model))[0]
    for name, deg in views.items():
        a = math.radians(deg)
        cam.location = centre + Vector((math.sin(a) * dist, -math.cos(a) * dist, radius * 0.1))
        cam.rotation_euler = (centre - cam.location).to_track_quat("-Z", "Y").to_euler()
        scene.render.filepath = os.path.join(out, f"{stem}_{name}.png")
        bpy.ops.render.render(write_still=True)
        print(f"ft_preview: wrote {scene.render.filepath}")


main()
