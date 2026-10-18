"""Builds assets/levels/blender/yard.{glb,_gltf/} : a small level authored the way an artist would in
Blender, with custom properties (exported as glTF `extras`) that the engine's importer
(ne/scene/BlenderLevel.h, tools/ne_level_import) turns into level entities:

    ne_prefab       "prefabs/crate.prefab.json"      instantiate a prefab at this transform
    ne_collider     box | sphere | capsule | mesh-bounds
    ne_trigger      "level_exit"                     trigger volume raising that event
    ne_spawn        "player"                         spawn point
    ne_light        point | directional (+ ne_light_color [r,g,b], ne_light_intensity, ne_light_radius)
    ne_ai_waypoints "WP1,WP2,WP3"                    names of empties; patrol route for an `ai` entity
    ne_body         static | dynamic                 (colliders; default static)
    ne_invisible    1                                no render mesh (collision-only geometry)

Everything else with a mesh becomes a static glTF mesh entity.  Writes yard.expected.json with the
world positions (glTF axes) the importer must reproduce.
"""
import math
import os

import bpy
from mathutils import Euler, Vector

import ne_blender as nb


def tag(obj, **props):
    for k, v in props.items():
        obj[k] = v
    return obj


def empty(name, loc, rot_deg=(0, 0, 0), scale=(1, 1, 1), display="PLAIN_AXES", parent=None, size=1.0):
    e = bpy.data.objects.new(name, None)
    e.empty_display_type = display
    e.empty_display_size = size
    e.location = loc
    e.rotation_euler = Euler([math.radians(a) for a in rot_deg])
    e.scale = scale
    bpy.context.scene.collection.objects.link(e)
    if parent:
        e.parent = parent
    return e


def box(name, size, loc, mat, rot_deg=(0, 0, 0), scale=(1, 1, 1), parent=None, uv_scale=1.0):
    v, f, uv = nb.box_geometry(size, (0, 0, 0))
    uv = [(a * uv_scale, b * uv_scale) for a, b in uv]
    o = nb.mesh_object(name, v, f, uv)
    o.data.materials.append(mat)
    o.location = loc
    o.rotation_euler = Euler([math.radians(a) for a in rot_deg])
    o.scale = scale
    if parent:
        o.parent = parent
    return o


def main():
    out = nb.script_args()
    nb.reset_scene()

    tiles = nb.tex_tiles()
    floor_mat, nt, bsdf = nb.new_material("floor_tiles")
    floor_mat.use_backface_culling = True
    t = nb.image_node(nt, tiles, (-600, 0))
    nt.links.new(t.outputs["Color"], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = 0.9
    stone, nt, bsdf = nb.new_material("stone")
    stone.use_backface_culling = True
    bsdf.inputs["Base Color"].default_value = (0.55, 0.52, 0.48, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.85
    iron, nt, bsdf = nb.new_material("iron")
    iron.use_backface_culling = True
    bsdf.inputs["Base Color"].default_value = (0.35, 0.36, 0.4, 1.0)
    bsdf.inputs["Metallic"].default_value = 1.0
    bsdf.inputs["Roughness"].default_value = 0.35

    expected = {"objects": {}}

    # Floor: big slab, UVs tiled 8x (REPEAT sampler), box collider fitted to the mesh bounds.
    floor = box("Floor", (30, 30, 0.4), (0, 0, -0.2), floor_mat, uv_scale=8.0)
    tag(floor, ne_collider="mesh-bounds")

    # Four walls built from one unit cube each, positioned and scaled with object transforms.
    for name, loc, scl in (("Wall_N", (0, 14.5, 1.5), (30, 1, 3)), ("Wall_S", (0, -14.5, 1.5), (30, 1, 3)),
                           ("Wall_E", (14.5, 0, 1.5), (1, 28, 3)), ("Wall_W", (-14.5, 0, 1.5), (1, 28, 3))):
        w = box(name, (1, 1, 1), loc, stone, scale=scl)
        tag(w, ne_collider="box")

    # A colonnade: an empty (rotated 90 degrees about Z) with three pillar children, to exercise
    # hierarchy composition. Pillars are cylinders with capsule colliders.
    colonnade = empty("Colonnade", (8, 3, 0), rot_deg=(0, 0, 90))
    for i, off in enumerate((-3.0, 0.0, 3.0)):
        bpy.ops.mesh.primitive_cylinder_add(vertices=16, radius=0.45, depth=3.0, location=(0, 0, 0))
        p = bpy.context.active_object
        p.name = f"Pillar_{i}"
        p.data.name = p.name
        p.data.materials.append(stone)
        nb.set_smooth(p, True)
        p.parent = colonnade
        p.location = (off, 0, 1.5)
        tag(p, ne_collider="capsule")

    # Boulder (sphere collider, scaled non-uniformly) and an invisible collision-only block.
    bpy.ops.mesh.primitive_uv_sphere_add(segments=24, ring_count=12, radius=0.8, location=(-6, -5, 0.8))
    boulder = bpy.context.active_object
    boulder.name = "Boulder"
    boulder.data.name = "Boulder"
    boulder.data.materials.append(iron)
    nb.set_smooth(boulder, True)
    tag(boulder, ne_collider="sphere")
    blocker = box("Blocker", (2, 2, 2), (-8, 6, 1), stone)
    tag(blocker, ne_collider="mesh-bounds", ne_invisible=1)

    # Prefab instances.
    tag(empty("Crate_A", (-3, 4, 0), rot_deg=(0, 0, 25), display="CUBE", size=0.5), ne_prefab="prefabs/crate.prefab.json")
    tag(empty("Crate_B", (-3.2, 5.4, 0), rot_deg=(0, 0, -10), display="CUBE", size=0.5), ne_prefab="prefabs/crate.prefab.json")
    guard = tag(empty("Guard", (4.5, -5, 0), rot_deg=(0, 0, 0), display="ARROWS"),
                ne_prefab="prefabs/guard.prefab.json", ne_ai_waypoints="WP_1,WP_2,WP_3,WP_4",
                ne_ai_patrolSpeed=1.8, ne_ai_visionRange=9.0)
    for n, p in (("WP_1", (4.5, -5, 0)), ("WP_2", (4.5, -10, 0)), ("WP_3", (-2, -10, 0)), ("WP_4", (-2, -5, 0))):
        empty(n, p)

    # Gameplay markers.
    tag(empty("PlayerStart", (1.5, 4, 0.2), rot_deg=(0, 0, 0), display="ARROWS"), ne_spawn="player")
    tag(empty("ExitTrigger", (0, -12.5, 1), scale=(2, 1, 1), display="CUBE", size=1.0), ne_trigger="level_exit")
    tag(empty("Torch_1", (3, 2, 2.2)), ne_light="point", ne_light_color=[1.0, 0.6, 0.3], ne_light_intensity=3.0,
        ne_light_radius=9.0)
    tag(empty("Torch_2", (-4, -6, 2.2)), ne_light="point", ne_light_color=[0.5, 0.7, 1.0], ne_light_intensity=2.0,
        ne_light_radius=7.0)
    # Blender faces -Y, so the engine's +Z is Blender -Y (see docs/blender.md).
    # Sun: Blender empties' local -Z is the shine direction; 55 degrees down, turned 35 about Z.
    tag(empty("Sun", (0, 0, 10), rot_deg=(55, 0, 35), display="SINGLE_ARROW"), ne_light="directional",
        ne_light_color=[1.0, 0.93, 0.8], ne_light_intensity=1.0)

    for o in bpy.context.scene.objects:
        if o.type == "MESH" and o.data.name != o.name:
            o.data.name = o.name
    bpy.context.view_layer.update()
    for o in bpy.context.scene.objects:
        expected["objects"][o.name] = {
            "world_position": nb.to_gltf_axes(o.matrix_world.translation),
            "props": {k: (list(v) if hasattr(v, "__len__") and not isinstance(v, str) else v)
                      for k, v in o.items()},
        }

    sun = bpy.data.objects["Sun"]
    expected["sun_shine_direction"] = nb.to_gltf_axes(sun.matrix_world.to_3x3() @ Vector((0, 0, -1)))
    d = os.path.join(out, "levels", "blender")
    nb.export_gltf(d, "yard", extra={"export_animations": False, "export_skins": False})
    nb.write_json(os.path.join(d, "yard.expected.json"), expected)
    print("level exported to", d)


if __name__ == "__main__":
    main()
