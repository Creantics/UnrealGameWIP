"""Shared helpers for the NewEngine Blender test-asset generators (headless bpy).

Run any generator with a Python that has the `bpy` wheel (pip install bpy), or inside Blender:

    python tools/blender/make_all.py [--out assets]
    blender -b -P tools/blender/make_prop.py -- --out assets

Nothing here depends on a UI; every asset is built from scratch (no .blend inputs) and the
procedural textures are deterministic, so re-running produces the same files.
"""
import json
import math
import os
import sys

import bpy
import numpy as np

TEX_SIZE = 256


def script_args():
    """`--out DIR` from the command line (after `--` when run through `blender -P`)."""
    argv = sys.argv
    argv = argv[argv.index("--") + 1:] if "--" in argv else argv[1:]
    out = "assets"
    i = 0
    while i < len(argv):
        if argv[i] == "--out" and i + 1 < len(argv):
            out = argv[i + 1]
            i += 1
        i += 1
    return out


def reset_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1.0  # 1 Blender unit = 1 metre = 1 glTF unit
    scene.render.fps = 30
    scene.frame_start = 0
    scene.frame_end = 60
    return scene


# --------------------------------------------------------------------------------------------
# Procedural images. Pixels are float RGBA in [0,1] (row 0 = bottom, Blender convention).
# --------------------------------------------------------------------------------------------

def _grid(n=TEX_SIZE):
    ys, xs = np.mgrid[0:n, 0:n]
    return (xs + 0.5) / n, (ys + 0.5) / n  # u, v (v = 0 at the bottom row)


_TMP = None


def make_image(name, rgba, colorspace="sRGB"):
    """Creates an image from an (n, n, 4) float array (rows bottom-to-top). The pixels are written to
    a PNG and loaded back as a file-backed, packed image: the glTF exporter reliably re-encodes
    those (a purely generated image can export blank)."""
    global _TMP
    import tempfile
    if _TMP is None:
        _TMP = tempfile.mkdtemp(prefix="ne_blender_")
    n = rgba.shape[0]
    gen = bpy.data.images.new(name + "_gen", n, n, alpha=True)
    gen.colorspace_settings.name = colorspace
    gen.pixels.foreach_set(np.clip(rgba, 0, 1).astype(np.float32).ravel())
    path = os.path.join(_TMP, name + ".png")
    gen.filepath_raw = path
    gen.file_format = "PNG"
    gen.save()
    bpy.data.images.remove(gen)
    img = bpy.data.images.load(path)
    img.colorspace_settings.name = colorspace
    img.alpha_mode = "STRAIGHT"
    img.pack()
    return img


def tex_planks(name="wood_base"):
    """Wood planks: warm brown with vertical grain stripes and dark grooves. sRGB colour."""
    u, v = _grid()
    plank = np.floor(u * 4)
    fu = (u * 4) % 1.0
    grain = 0.5 + 0.5 * np.sin(v * 40 + plank * 2.1 + np.sin(u * 30) * 1.5)
    base = np.array([0.55, 0.34, 0.16])
    col = base[None, None, :] * (0.75 + 0.25 * grain[..., None])
    col *= (0.85 + 0.15 * (plank % 2)[..., None])
    groove = (np.minimum(fu, 1 - fu) < 0.03)[..., None]
    col = np.where(groove, col * 0.35, col)
    return make_image(name, np.dstack([col, np.ones_like(u)]), "sRGB")


def tex_planks_normal(name="wood_normal"):
    """Tangent-space normal map matching tex_planks: grooves between the planks. Non-Color."""
    u, v = _grid()
    fu = (u * 4) % 1.0
    h = np.clip(np.minimum(fu, 1 - fu) / 0.08, 0, 1)  # 0 in the groove, 1 on the plank
    h = h * h * (3 - 2 * h)
    h += 0.04 * np.sin(v * 40 + np.floor(u * 4) * 2.1)
    dx = np.gradient(h, axis=1) * TEX_SIZE
    dy = np.gradient(h, axis=0) * TEX_SIZE  # +v (up) direction
    n = np.dstack([-dx * 0.35, -dy * 0.35, np.ones_like(h)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return make_image(name, np.dstack([n * 0.5 + 0.5, np.ones_like(h)]), "Non-Color")


def tex_metal_rough(name="metalrough"):
    """glTF-style packing in one image: G = roughness, B = metallic. Non-Color.
    Left half (u < 0.5) is rough dielectric, right half shiny metal."""
    u, v = _grid()
    rough = np.where(u < 0.5, 0.85, 0.25 + 0.1 * np.sin(v * 25))
    metal = np.where(u < 0.5, 0.0, 1.0)
    return make_image(name, np.dstack([np.zeros_like(u), rough, metal, np.ones_like(u)]), "Non-Color")


def tex_glow(name="glow"):
    """Emission colour map: bright rings on black. sRGB."""
    u, v = _grid()
    r = np.hypot(u - 0.5, v - 0.5)
    ring = (np.sin(r * 60) > 0.2).astype(np.float32) * np.clip(1.2 - r * 2, 0, 1)
    col = np.dstack([ring * 1.0, ring * 0.65, ring * 0.2])
    return make_image(name, np.dstack([col, np.ones_like(u)]), "sRGB")


def tex_leaf(name="leaf"):
    """Leaf cut-out: green blade shape in the alpha channel (alpha-mask material). sRGB + alpha."""
    u, v = _grid()
    x = (u - 0.5) * 2
    y = v
    width = 0.55 * np.sin(np.clip(y, 0, 1) * math.pi) ** 0.8
    inside = np.abs(x) < width
    vein = np.abs(x) < 0.04
    col = np.where(vein[..., None], np.array([0.65, 0.8, 0.3]), np.array([0.2, 0.55, 0.15]))
    col = col * (0.85 + 0.15 * np.sin(y * 50))[..., None]
    return make_image(name, np.dstack([col, inside.astype(np.float32)]), "sRGB")


def tex_cloth(name="cloth_base", tint=(0.2, 0.35, 0.75), accent=(0.95, 0.8, 0.25)):
    """Character texture: diagonal cloth weave in `tint` with `accent` stripes. sRGB."""
    u, v = _grid()
    weave = 0.5 + 0.5 * np.sin((u + v) * 48) * np.sin((u - v) * 48)
    stripe = (np.sin(v * 28) > 0.75)[..., None]
    col = np.array(tint)[None, None, :] * (0.8 + 0.2 * weave[..., None])
    col = np.where(stripe, np.array(accent)[None, None, :] * (0.85 + 0.15 * weave[..., None]), col)
    return make_image(name, np.dstack([col, np.ones_like(u)]), "sRGB")


def tex_cloth_normal(name="cloth_normal"):
    u, v = _grid()
    h = 0.5 + 0.5 * np.sin((u + v) * 48) * np.sin((u - v) * 48)
    dx = np.gradient(h, axis=1) * TEX_SIZE
    dy = np.gradient(h, axis=0) * TEX_SIZE
    n = np.dstack([-dx * 0.2, -dy * 0.2, np.ones_like(h)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return make_image(name, np.dstack([n * 0.5 + 0.5, np.ones_like(h)]), "Non-Color")


def tex_tiles(name="tiles", a=(0.55, 0.55, 0.5), b=(0.38, 0.4, 0.45)):
    """Level floor: checker tiles with grout lines. sRGB."""
    u, v = _grid()
    odd = ((np.floor(u * 4) + np.floor(v * 4)) % 2)[..., None]
    col = np.where(odd > 0, np.array(a), np.array(b))
    grout = ((u * 4 % 1 < 0.04) | (v * 4 % 1 < 0.04))[..., None]
    col = np.where(grout, col * 0.5, col)
    return make_image(name, np.dstack([col, np.ones_like(u)]), "sRGB")


# --------------------------------------------------------------------------------------------
# Materials (Principled BSDF graphs the glTF exporter understands)
# --------------------------------------------------------------------------------------------

def new_material(name):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    out.location = (400, 0)
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    return mat, nt, bsdf


def image_node(nt, image, loc=(-600, 0), extension="REPEAT", interpolation="Linear", mapping=None):
    """Image Texture node. `mapping` = (offset(x, y), rotation_z, scale(x, y)) inserts a Mapping
    node between the UVs and the image (exported as KHR_texture_transform)."""
    node = nt.nodes.new("ShaderNodeTexImage")
    node.image = image
    node.extension = extension
    node.interpolation = interpolation
    node.location = loc
    if mapping is not None:
        uv = nt.nodes.new("ShaderNodeTexCoord")
        mp = nt.nodes.new("ShaderNodeMapping")
        mp.vector_type = "POINT"
        off, rot, scl = mapping
        mp.inputs["Location"].default_value = (off[0], off[1], 0)
        mp.inputs["Rotation"].default_value = (0, 0, rot)
        mp.inputs["Scale"].default_value = (scl[0], scl[1], 1)
        uv.location = (loc[0] - 500, loc[1])
        mp.location = (loc[0] - 250, loc[1])
        nt.links.new(uv.outputs["UV"], mp.inputs["Vector"])
        nt.links.new(mp.outputs["Vector"], node.inputs["Vector"])
    return node


# --------------------------------------------------------------------------------------------
# Meshes
# --------------------------------------------------------------------------------------------

def mesh_object(name, verts, faces, uvs=None, collection=None):
    """Mesh object from lists. `uvs` is one (u, v) per face corner, in face order."""
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    if uvs is not None:
        layer = mesh.uv_layers.new(name="UVMap")
        for i, uv in enumerate(uvs):
            layer.data[i].uv = uv
    obj = bpy.data.objects.new(name, mesh)
    (collection or bpy.context.scene.collection).objects.link(obj)
    return obj


def box_geometry(size=(1, 1, 1), center=(0, 0, 0)):
    """Box with 24 verts (per-face normals and UVs). Returns verts, faces, uvs (per corner)."""
    sx, sy, sz = (s / 2 for s in size)
    cx, cy, cz = center

    def P(x, y, z):
        return (cx + x * sx, cy + y * sy, cz + z * sz)

    quads = [  # CCW seen from outside
        [P(-1, -1, 1), P(1, -1, 1), P(1, 1, 1), P(-1, 1, 1)],      # +Z
        [P(-1, 1, -1), P(1, 1, -1), P(1, -1, -1), P(-1, -1, -1)],  # -Z
        [P(1, -1, -1), P(1, 1, -1), P(1, 1, 1), P(1, -1, 1)],      # +X
        [P(-1, 1, -1), P(-1, -1, -1), P(-1, -1, 1), P(-1, 1, 1)],  # -X
        [P(1, 1, -1), P(-1, 1, -1), P(-1, 1, 1), P(1, 1, 1)],      # +Y
        [P(-1, -1, -1), P(1, -1, -1), P(1, -1, 1), P(-1, -1, 1)],  # -Y
    ]
    verts, faces, uvs = [], [], []
    for q in quads:
        base = len(verts)
        verts.extend(q)
        faces.append((base, base + 1, base + 2, base + 3))
        uvs.extend([(0, 0), (1, 0), (1, 1), (0, 1)])
    return verts, faces, uvs


def set_smooth(obj, smooth):
    for p in obj.data.polygons:
        p.use_smooth = smooth


def activate(obj):
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)


# --------------------------------------------------------------------------------------------
# Export
# --------------------------------------------------------------------------------------------

def export_gltf(out_dir, stem, extra=None, glb_extra=None, gltf_extra=None):
    """Exports the current scene twice: <out_dir>/<stem>.glb and <out_dir>/<stem>_gltf/<stem>.gltf
    (+ .bin and textures/*.png). `extra` overrides exporter options for both; `glb_extra` /
    `gltf_extra` only for one of them (e.g. export tangents into just one)."""
    opts = dict(
        export_apply=True,
        export_yup=True,
        export_extras=True,
        export_tangents=False,
        export_vertex_color="NONE",
        export_cameras=False,
        export_lights=False,
        export_animations=True,
        export_animation_mode="ACTIONS",
        export_force_sampling=False,
        export_optimize_animation_size=False,
        export_nla_strips=False,
        export_image_format="AUTO",  # PNG stays PNG
        export_materials="EXPORT",
        export_skins=True,
        export_influence_nb=4,
        export_leaf_bone=False,
        export_def_bones=False,
        export_morph=False,
    )
    if extra:
        opts.update(extra)
    os.makedirs(out_dir, exist_ok=True)
    glb = os.path.join(out_dir, stem + ".glb")
    sep_dir = os.path.join(out_dir, stem + "_gltf")
    os.makedirs(sep_dir, exist_ok=True)
    gltf = os.path.join(sep_dir, stem + ".gltf")
    bpy.ops.export_scene.gltf(filepath=glb, export_format="GLB", **{**opts, **(glb_extra or {})})
    bpy.ops.export_scene.gltf(filepath=gltf, export_format="GLTF_SEPARATE", export_texture_dir="textures",
                              **{**opts, **(gltf_extra or {})})
    return glb, gltf


# The `rig` profile (docs/rigging_plan.md §11): characters with a canonical skeleton and face morphs.
# Shape keys survive only with export_apply=False (applying modifiers drops them); the Armature
# modifier is never applied anyway. Deform bones only, every frame sampled (constraints baked).
RIG_PROFILE = dict(
    export_apply=False,
    export_morph=True,
    export_morph_normal=True,
    export_morph_tangent=False,
    export_morph_animation=True,
    export_try_sparse_sk=True,
    export_def_bones=True,
    export_force_sampling=True,
    export_influence_nb=4,
    export_all_influences=False,
    export_optimize_animation_size=False,
    export_rest_position_armature=True,
)


def export_rig(path, extra=None):
    """Exports the current scene into one .glb with the rig profile (on top of export_gltf's defaults)."""
    opts = dict(
        export_yup=True,
        export_extras=True,
        export_tangents=False,
        export_vertex_color="NONE",
        export_cameras=False,
        export_lights=False,
        export_animations=True,
        export_animation_mode="ACTIONS",
        export_nla_strips=False,
        export_image_format="AUTO",
        export_materials="EXPORT",
        export_skins=True,
        export_leaf_bone=False,
    )
    opts.update(RIG_PROFILE)
    if extra:
        opts.update(extra)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    # Blender 5.2's exporter walks children sorted by name, which breaks the canonical joint order
    # (creation order, parents first). Keep creation order for the duration of the export.
    tree = sys.modules.get("io_scene_gltf2.blender.exp.tree")
    if tree is None:
        try:
            import importlib
            tree = importlib.import_module("io_scene_gltf2.blender.exp.tree")
        except ImportError:
            tree = None
    sort = getattr(tree, "_sort_by_name", None)
    if sort is not None:
        tree._sort_by_name = list
    try:
        bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", **opts)
    finally:
        if sort is not None:
            tree._sort_by_name = sort
    return path


def read_glb_json(path):
    """The JSON chunk of a .glb (for post-export assertions)."""
    import struct
    with open(path, "rb") as f:
        data = f.read()
    magic, _ver, _len = struct.unpack_from("<4sII", data, 0)
    assert magic == b"glTF", path
    clen, ctype = struct.unpack_from("<I4s", data, 12)
    assert ctype == b"JSON"
    return json.loads(data[20:20 + clen].decode("utf-8"))


def to_gltf_axes(v):
    """Blender (X right, Y forward, Z up) -> glTF (X right, Y up, -Z forward) = (x, z, -y)."""
    return [float(v[0]), float(v[2]), float(-v[1])]


def write_json(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(data, f, indent=1, sort_keys=True)
        f.write("\n")
