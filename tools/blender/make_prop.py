"""Builds assets/models/blender/crate_prop.{glb,_gltf/} : a textured prop with every material feature
the engine supports, exported by Blender's glTF exporter.

  Crate (mesh, 2 material slots = 2 primitives)
    wood   : baseColor + normal map (both through a Mapping node -> KHR_texture_transform),
             metallicRoughness via Separate Color (G roughness, B metallic) is on the bands
    bands  : metal bands, metallicRoughness texture + baseColor factor
  Lamp     : child sphere, emissive texture, clamped + nearest sampler
  Leaf     : child plane, alpha MASK (Math > cutoff 0.4), double sided, node scale != 1
  Glass    : child plane, alpha BLEND (alpha factor 0.35)
  Marker   : empty with custom properties (extras)

Also writes crate_prop.expected.json (node world positions in glTF axes, material facts) which the
unit tests compare the engine's loader against.
"""
import math
import os

import bpy
from mathutils import Euler, Vector

import ne_blender as nb


def build():
    nb.reset_scene()
    root = bpy.data.objects.new("PropRoot", None)
    bpy.context.scene.collection.objects.link(root)

    # --- materials -----------------------------------------------------------------------
    wood_img, wood_n_img = nb.tex_planks(), nb.tex_planks_normal()
    mr_img, glow_img, leaf_img = nb.tex_metal_rough(), nb.tex_glow(), nb.tex_leaf()
    mapping = ((0.25, 0.0), 0.0, (2.0, 2.0))  # tile 2x, offset .25: becomes KHR_texture_transform

    wood, nt, bsdf = nb.new_material("wood")
    base = nb.image_node(nt, wood_img, (-700, 300), mapping=mapping)
    nt.links.new(base.outputs["Color"], bsdf.inputs["Base Color"])
    nrm_tex = nb.image_node(nt, wood_n_img, (-700, -100), mapping=mapping)
    nmap = nt.nodes.new("ShaderNodeNormalMap")
    nmap.inputs["Strength"].default_value = 1.0
    nmap.location = (-300, -100)
    nt.links.new(nrm_tex.outputs["Color"], nmap.inputs["Color"])
    nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
    bsdf.inputs["Roughness"].default_value = 0.8
    bsdf.inputs["Metallic"].default_value = 0.0
    wood.use_backface_culling = True  # glTF doubleSided = false

    bands, nt, bsdf = nb.new_material("bands")
    bands.use_backface_culling = True
    bsdf.inputs["Base Color"].default_value = (0.8, 0.82, 0.88, 1.0)
    mr = nb.image_node(nt, mr_img, (-900, 0))
    sep = nt.nodes.new("ShaderNodeSeparateColor")
    sep.location = (-500, 0)
    nt.links.new(mr.outputs["Color"], sep.inputs["Color"])
    nt.links.new(sep.outputs["Green"], bsdf.inputs["Roughness"])
    nt.links.new(sep.outputs["Blue"], bsdf.inputs["Metallic"])

    lamp, nt, bsdf = nb.new_material("lamp_glow")
    lamp.use_backface_culling = True
    bsdf.inputs["Base Color"].default_value = (0.05, 0.04, 0.03, 1.0)
    glow = nb.image_node(nt, glow_img, (-700, 0), extension="EXTEND", interpolation="Closest")
    nt.links.new(glow.outputs["Color"], bsdf.inputs["Emission Color"])
    bsdf.inputs["Emission Strength"].default_value = 1.0

    leaf, nt, bsdf = nb.new_material("leaf_mask")
    leaf.use_backface_culling = False  # glTF doubleSided
    tex = nb.image_node(nt, leaf_img, (-900, 0))
    gt = nt.nodes.new("ShaderNodeMath")
    gt.operation = "GREATER_THAN"
    gt.inputs[1].default_value = 0.4  # alphaCutoff
    gt.location = (-500, -200)
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(tex.outputs["Alpha"], gt.inputs[0])
    nt.links.new(gt.outputs["Value"], bsdf.inputs["Alpha"])
    bsdf.inputs["Roughness"].default_value = 0.6

    glass, nt, bsdf = nb.new_material("glass_blend")
    glass.use_backface_culling = False
    bsdf.inputs["Base Color"].default_value = (0.3, 0.7, 0.9, 1.0)
    bsdf.inputs["Alpha"].default_value = 0.35
    bsdf.inputs["Roughness"].default_value = 0.1

    # --- geometry --------------------------------------------------------------------------
    verts, faces, uvs = nb.box_geometry((1.0, 1.0, 1.0), (0, 0, 0.5))
    n_wood_faces = len(faces)
    for z in (0.15, 0.5, 0.85):  # three metal bands, slightly larger than the crate: material slot 1
        v, f, u = nb.box_geometry((1.04, 1.04, 0.1), (0, 0, z))
        off = len(verts)
        verts += v
        faces += [tuple(i + off for i in face) for face in f]
        uvs += u
    crate = nb.mesh_object("Crate", verts, faces, uvs)
    crate.data.materials.append(wood)
    crate.data.materials.append(bands)
    for i, p in enumerate(crate.data.polygons):
        p.material_index = 0 if i < n_wood_faces else 1
    nb.set_smooth(crate, False)
    crate.parent = root

    bpy.ops.mesh.primitive_uv_sphere_add(segments=24, ring_count=12, radius=0.18, location=(0.0, 0.0, 1.35))
    lamp_o = bpy.context.active_object
    lamp_o.name = "Lamp"
    lamp_o.data.materials.append(lamp)
    nb.set_smooth(lamp_o, True)
    lamp_o.parent = root

    bpy.ops.mesh.primitive_plane_add(size=1.0, location=(0.9, -0.2, 0.6))
    leaf_o = bpy.context.active_object
    leaf_o.name = "Leaf"
    leaf_o.rotation_euler = Euler((math.radians(90), 0, math.radians(30)))
    leaf_o.scale = (0.8, 0.8, 1.0)  # node scale must survive export
    leaf_o.data.materials.append(leaf)
    leaf_o.parent = root

    bpy.ops.mesh.primitive_plane_add(size=1.0, location=(-1.0, 0.3, 0.7))
    glass_o = bpy.context.active_object
    glass_o.name = "Glass"
    glass_o.rotation_euler = Euler((math.radians(90), 0, math.radians(-20)))
    glass_o.data.materials.append(glass)
    glass_o.parent = root

    marker = bpy.data.objects.new("Marker", None)
    marker.empty_display_type = "ARROWS"
    marker.location = (0.0, 0.0, 1.8)
    marker["ne_note"] = "extras test"
    marker["ne_count"] = 3
    marker["ne_scale"] = 1.5
    marker["ne_flag"] = True
    marker.parent = root
    bpy.context.scene.collection.objects.link(marker)

    for o in bpy.context.scene.objects:
        if o.type == "MESH":
            o.data.name = o.name
    root.location = (0.0, 0.0, 0.0)
    bpy.context.view_layer.update()
    return root


def main():
    out = nb.script_args()
    root = build()
    d = os.path.join(out, "models", "blender")
    # The .glb carries exporter-made tangents, the .gltf does not: the engine must cope with both.
    nb.export_gltf(d, "crate_prop", glb_extra={"export_tangents": True}, gltf_extra={"export_tangents": False})

    objs = {}
    for o in bpy.context.scene.objects:
        objs[o.name] = {"world_position": nb.to_gltf_axes(o.matrix_world.translation)}
    nb.write_json(os.path.join(d, "crate_prop.expected.json"), {
        "nodes": objs,
        "materials": {
            # Blender Mapping (location 0.25, 0; scale 2) -> glTF offset (0.25, 1 - 2) because glTF's v runs down
            "wood": {"alphaMode": "OPAQUE", "texTransformScale": [2.0, 2.0], "texTransformOffset": [0.25, -1.0]},
            "bands": {"alphaMode": "OPAQUE", "hasMetallicRoughness": True},
            "lamp_glow": {"alphaMode": "OPAQUE", "hasEmissive": True},
            "leaf_mask": {"alphaMode": "MASK", "alphaCutoff": 0.4, "doubleSided": True},
            "glass_blend": {"alphaMode": "BLEND", "alpha": 0.35, "doubleSided": True},
        },
        "extras": {"Marker": {"ne_note": "extras test", "ne_count": 3, "ne_scale": 1.5, "ne_flag": True}},
    })
    print("prop exported to", d)


if __name__ == "__main__":
    main()
