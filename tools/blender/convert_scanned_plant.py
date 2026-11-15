"""Turn a scanned plant pack (glTF with variants, their LODs, a flat billboard card and a shared texture atlas) into
plants in the layout tools/blender/make_tree.py writes and FTImportPlant reads (see FTImportPlantCommandlet.h).

  blender -b --factory-startup --python tools/blender/convert_scanned_plant.py -- --src <pack dir> --out <dir>
      --name <base> [--variants A,C,E] [--count 3] [--hull] [--lod0-max 60000] [--tex 1024]

The pack: one .gltf (any depth under --src) whose mesh nodes are <x>_Var<K> (LOD0) and <x>_Var<K>_LOD<n>; the faces
of the far card use a material whose name has "Billboard"; Textures/ holds *_B.png (colour; *_B-O.png when there is
no plain one), *_N.png (normal), *_ORT.png (R opacity, G roughness, B translucency) and *_Billboard_B-O.png (the far
card's colour and opacity). Real size in metres, Z up after import.

Per variant it writes, as <base>-<k> (k = a, b, c ...), into --out:
  <NAME>.glb      <NAME>_LOD0.._LOD2 (the pack's own LODs; a missing one decimated from the one before, a LOD0 over
                  --lod0-max triangles skipped for the next), <NAME>_LOD3 (the pack's billboard card and a copy turned
                  90 degrees), UCX_<NAME>_LOD0_00 with --hull (an eight-sided prism round the lower half: for woody
                  shrubs; without it the importer gives the plant no collision and only logs a warning); materials
                  Leaf and Billboard (no images); origin at the ground centre (x, y of LOD0's box; z as scanned);
                  normals bent out from the plant's middle and up (the canopy lights as one mass; the scan has none).
                  No vertex colours: the importer's wind is by height alone (json crown_base_m 0: the whole plant
                  bends from the ground, more with height).
  <NAME>_leaf.png (sRGB colour + cut-out alpha, colour dilated under the alpha edge), _leaf_n.png (the pack's normal,
                  green flipped to the engine's green-down), _leaf_m.png (R coverage, G 0, B a per-leaf-ish random
                  id), _bb.png (the pack's billboard picture, dilated): square, --tex px at most
  <NAME>.json     heights, width, LOD triangles, leaf mean colour, evergreen 1 (G 0: no winter leaf drop)
"""
import json
import math
import os
import re
import sys

import bpy
import numpy as np

LOD_SCREEN = (1.0, 0.45, 0.20, 0.06)


def args():
    a = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    o = dict(src=None, out=None, name=None, variants=None, count=3, hull=False, lod0_max=60000, tex=1024)
    i = 0
    while i < len(a):
        k = a[i]
        if k == "--hull":
            o["hull"] = True
        elif k in ("--src", "--out", "--name", "--variants"):
            o[k[2:]] = a[i + 1]
            i += 1
        elif k in ("--count", "--lod0-max", "--tex"):
            o[k[2:].replace("-", "_")] = int(a[i + 1])
            i += 1
        else:
            raise SystemExit("convert_scanned_plant: unknown argument %s" % k)
        i += 1
    if not (o["src"] and o["out"] and o["name"]):
        raise SystemExit("convert_scanned_plant: pass --src <pack dir> --out <dir> --name <base>")
    return o


# ----------------------------------------------------------------------------------------------- files
def find_gltf(src):
    if src.lower().endswith((".gltf", ".glb")):
        return src
    found = []
    for root, _, files in os.walk(src):
        for f in files:
            if f.lower().endswith((".gltf", ".glb")):
                found.append(os.path.join(root, f))
    if not found:
        raise SystemExit("convert_scanned_plant: no .gltf under %s" % src)
    # the engine-flavoured file first (a plain colour map), else whatever there is
    found.sort(key=lambda p: ("nonue" in p.lower(), len(p)))
    return found[0]


def find_maps(gltf):
    d = os.path.dirname(gltf)
    for _ in range(3):
        if os.path.isdir(os.path.join(d, "Textures")):
            break
        d = os.path.dirname(d)
    tdir = os.path.join(d, "Textures")
    files = os.listdir(tdir) if os.path.isdir(tdir) else []

    def pick(suffix):
        for f in files:
            if f.lower().endswith(suffix.lower()):
                return os.path.join(tdir, f)
        return None
    maps = dict(color=pick("_B.png") or pick("_B-O.png"), color_o=pick("_B-O.png"), normal=pick("_N.png"),
                ort=pick("_ORT.png"), bb=pick("_Billboard_B-O.png"))
    if not maps["color"] or not maps["normal"]:
        raise SystemExit("convert_scanned_plant: no colour or normal map in %s" % tdir)
    return maps


def load_px(path):
    """Rows-down float array (h, w, 4) of an 8-bit image's stored values (sRGB colour stays sRGB)."""
    img = bpy.data.images.load(path)
    img.colorspace_settings.name = "Non-Color"
    w, h = img.size
    px = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(px)
    bpy.data.images.remove(img)
    return px.reshape(h, w, 4)[::-1].astype(np.float64)


def downsample(a, f):
    if f <= 1:
        return a
    h, w = a.shape[:2]
    return a.reshape(h // f, f, w // f, f, *a.shape[2:]).mean(axis=(1, 3))


def dilate_under_alpha(rgb, alpha, passes=24):
    """Spread the colour into the transparent texels (no dark halo when mips or filtering mix them in)."""
    filled = alpha > 0.5
    out = rgb.copy()
    for _ in range(passes):
        if filled.all():
            break
        s = np.zeros_like(out)
        c = np.zeros(filled.shape)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            s += np.roll(np.roll(out * filled[..., None], dy, 0), dx, 1)
            c += np.roll(np.roll(filled, dy, 0), dx, 1)
        grow = (~filled) & (c > 0)
        out[grow] = s[grow] / c[grow][:, None]
        filled = filled | grow
    mean = rgb[alpha > 0.5].mean(0) if (alpha > 0.5).any() else np.array([0.3, 0.35, 0.2])
    out[~filled] = mean
    return out


def cutout(rgb, alpha, n):
    """Alpha-weighted downscale to n px, then the colour dilated under the alpha."""
    f = max(1, rgb.shape[0] // n)
    a = downsample(alpha, f)
    c = downsample(rgb * alpha[..., None], f) / np.maximum(a[..., None], 1e-4)
    return dilate_under_alpha(np.clip(c, 0, 1), a), np.clip(a, 0, 1)


def save_png(path, rgb, alpha=None, noncolor=False):
    h, w = rgb.shape[:2]
    img = bpy.data.images.new(os.path.basename(path), w, h, alpha=alpha is not None, is_data=noncolor)
    if noncolor:
        img.colorspace_settings.name = "Non-Color"
    a = alpha if alpha is not None else np.ones((h, w))
    px = np.concatenate([rgb, a[..., None]], -1)[::-1].astype(np.float32)
    img.pixels.foreach_set(px.ravel())
    img.filepath_raw = path
    img.file_format = "PNG"
    img.save()
    bpy.data.images.remove(img)


def make_maps(maps, n):
    col = load_px(maps["color"])
    if maps["ort"]:
        alpha = load_px(maps["ort"])[..., 0]
    elif maps["color_o"]:
        alpha = load_px(maps["color_o"])[..., 3]
    else:
        alpha = col[..., 3]
    rgb, a = cutout(col[..., :3], alpha, n)
    nrm = load_px(maps["normal"])[..., :3]
    f = max(1, nrm.shape[0] // n)
    v = downsample(nrm, f) * 2 - 1
    v /= np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-6)
    v[..., 1] = -v[..., 1]                     # glTF (green up) -> the engine's green down
    nrm = v * 0.5 + 0.5
    # a per-leaf-ish id: random values on a coarse grid, constant over each cell
    cells = max(8, n // 24)
    rng = np.random.default_rng(7)
    ids = rng.random((cells, cells))
    lid = np.kron(ids, np.ones((n // cells + 1, n // cells + 1)))[:a.shape[0], :a.shape[1]]
    mask = np.stack([a, np.zeros_like(a), lid], -1)
    bb = None
    if maps["bb"]:
        b = load_px(maps["bb"])
        bb = cutout(b[..., :3], b[..., 3], n)
    mean = (rgb[a > 0.5].mean(0) * 255).round().astype(int).tolist() if (a > 0.5).any() else [80, 90, 50]
    return dict(leaf=(rgb, a), n=nrm, m=mask, bb=bb, mean=mean)


# ----------------------------------------------------------------------------------------------- meshes
def tris(me):
    return sum(len(p.vertices) - 2 for p in me.polygons)


def is_billboard(ob):
    return any("billboard" in (m.name.lower() if m else "") for m in ob.data.materials)


def variants_in_scene():
    """{key: {lod index: object}} from <x>_Var<K>[_LOD<n>] mesh objects (names may carry a .001)."""
    out = {}
    for ob in bpy.data.objects:
        if ob.type != "MESH":
            continue
        m = re.search(r"_Var([A-Za-z0-9]+?)(?:_LOD(\d+))?(?:\.\d+)?$", ob.name)
        if not m:
            continue
        out.setdefault(m.group(1), {})[int(m.group(2) or 0)] = ob
    return out


def baked_copy(ob, name):
    me = ob.data.copy()
    me.transform(ob.matrix_world)
    me.name = name
    new = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(new)
    return new


def decimated(src, name, ratio):
    ob = baked_copy(src, name)      # src is already baked (identity), so this is a plain copy
    if tris(ob.data) > 300:
        mod = ob.modifiers.new("dec", "DECIMATE")
        mod.ratio = ratio
        mod.use_collapse_triangulate = True
        bpy.context.view_layer.objects.active = ob
        for o in bpy.context.selected_objects:
            o.select_set(False)
        ob.select_set(True)
        bpy.ops.object.modifier_apply(modifier="dec")
    return ob


def set_material(ob, mat):
    me = ob.data
    me.materials.clear()
    me.materials.append(mat)
    for p in me.polygons:
        p.material_index = 0


def canopy_normals(ob, centre, up=0.6):
    """Normals bent from the plant's middle and up: the cards light as one mass (both faces keep them in game)."""
    me = ob.data
    co = np.empty(len(me.vertices) * 3)
    me.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    d = co - np.array(centre)
    d /= np.maximum(np.linalg.norm(d, axis=1, keepdims=True), 1e-6)
    n = d + np.array([0.0, 0.0, up])
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-6)
    for p in me.polygons:
        p.use_smooth = True
    me.normals_split_custom_set_from_vertices([tuple(v) for v in n])


def hull_prism(lod0, h):
    co = np.array([v.co[:] for v in lod0.data.vertices])
    low = co[co[:, 2] < 0.5 * h]
    if len(low) < 8:
        low = co
    r = float(np.percentile(np.hypot(low[:, 0], low[:, 1]), 75)) * 0.8
    r = max(r, 0.05)
    z0, z1 = float(min(0.0, co[:, 2].min())), 0.5 * h
    v, f = [], []
    for z in (z0, z1):
        for k in range(8):
            th = 2 * math.pi * k / 8
            v.append((r * math.cos(th), r * math.sin(th), z))
    f.append(tuple(range(7, -1, -1)))
    f.append(tuple(range(8, 16)))
    for k in range(8):
        a, b = k, (k + 1) % 8
        f.append((a, b, b + 8, a + 8))
    return v, f, r


def pick_variants(vs, o):
    if o["variants"]:
        keys = [k.strip() for k in o["variants"].split(",") if k.strip()]
        missing = [k for k in keys if k not in vs]
        if missing:
            raise SystemExit("convert_scanned_plant: no variant %s (have %s)" % (missing, sorted(vs)))
        return keys
    # automatic: leave out flat litter, then spread the rest over size
    info = []
    for k, lods in vs.items():
        real = [lods[i] for i in sorted(lods) if not is_billboard(lods[i])]
        if not real:
            continue
        zs = [(real[0].matrix_world @ __import__("mathutils").Vector(c)).z for c in real[0].bound_box]
        info.append((max(zs), k))
    if not info:
        return []
    top = max(h for h, _ in info)
    keep = sorted([x for x in info if x[0] > 0.3 * top])
    if len(keep) <= o["count"]:
        return [k for _, k in keep]
    idx = np.linspace(0, len(keep) - 1, o["count"]).round().astype(int)
    return [keep[i][1] for i in idx]


def convert_variant(key, lods, name, o, mats, m_leaf, m_bb):
    real = [lods[i] for i in sorted(lods) if not is_billboard(lods[i])]
    bbs = [lods[i] for i in sorted(lods) if is_billboard(lods[i])]
    while len(real) > 1 and tris(real[0].data) > o["lod0_max"]:
        real = real[1:]
    out = []
    for lod in range(3):
        nm = "%s_LOD%d" % (name, lod)
        if lod < len(real):
            out.append(baked_copy(real[lod], nm))
        else:
            out.append(decimated(out[-1], nm, 0.5))
    if bbs:
        a = baked_copy(bbs[0], name + "_LOD3")
        b = baked_copy(bbs[0], name + "_bbx")
        b.data.transform(__import__("mathutils").Matrix.Rotation(math.pi / 2, 4, "Z"))
        bpy.ops.object.select_all(action="DESELECT")
        a.select_set(True)
        b.select_set(True)
        bpy.context.view_layer.objects.active = a
        bpy.ops.object.join()
        bb = a
    else:
        bb = decimated(out[-1], name + "_LOD3", 0.3)
    # origin: x, y of LOD0's box centre (z as scanned: roots below the ground are meant)
    co = np.array([v.co[:] for v in out[0].data.vertices])
    cx = (co[:, 0].max() + co[:, 0].min()) / 2
    cy = (co[:, 1].max() + co[:, 1].min()) / 2
    shift = __import__("mathutils").Matrix.Translation((-cx, -cy, 0))
    # (the far card was turned round the pack's origin, the plant's root, before this)
    for ob in out + [bb]:
        ob.data.transform(shift)
    co = np.array([v.co[:] for v in out[0].data.vertices])
    h = float(co[:, 2].max())
    w = float(max(co[:, 0].max() - co[:, 0].min(), co[:, 1].max() - co[:, 1].min()))
    centre = (0.0, 0.0, 0.35 * h if w < 2.5 * h else -0.5 * w)
    for ob in out:
        set_material(ob, m_leaf)
        canopy_normals(ob, centre)
    set_material(bb, m_bb)
    canopy_normals(bb, centre)
    objs = out + [bb]
    hull_r = 0.0
    if o["hull"]:
        hv, hf, hull_r = hull_prism(out[0], h)
        me = bpy.data.meshes.new("UCX_%s_LOD0_00" % name)
        me.from_pydata(hv, [], hf)
        me.materials.append(m_leaf)
        ucx = bpy.data.objects.new(me.name, me)
        bpy.context.scene.collection.objects.link(ucx)
        objs.append(ucx)
    bpy.ops.object.select_all(action="DESELECT")
    for ob in objs:
        ob.select_set(True)
    path = os.path.join(o["out"], name + ".glb")
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True, export_yup=True, export_apply=False,
                              export_normals=True, export_tangents=False, export_materials="EXPORT",
                              export_vertex_color="NONE", export_animations=False, export_skins=False, export_morph=False,
                              export_image_format="NONE")
    tri = [tris(ob.data) for ob in out + [bb]]
    meta = dict(name=name, source_variant=key, height_m=round(h, 3), width_m=round(w, 3), crown_base_m=0.0,
                lod_triangles=tri, lod_screen_size=list(LOD_SCREEN), leaf_mean_srgb=mats["mean"], evergreen=1,
                billboard_atlas=0, hull=1 if o["hull"] else 0, hull_radius_m=round(hull_r, 3))
    with open(os.path.join(o["out"], name + ".json"), "w") as fh:
        json.dump(meta, fh, indent=1)
    for suffix, (rgb, a) in (("leaf", mats["leaf"]),) + ((("bb", mats["bb"]),) if mats["bb"] else ()):
        save_png(os.path.join(o["out"], "%s_%s.png" % (name, suffix)), rgb, a)
    save_png(os.path.join(o["out"], name + "_leaf_n.png"), mats["n"], None, noncolor=True)
    save_png(os.path.join(o["out"], name + "_leaf_m.png"), mats["m"], None, noncolor=True)
    print("convert_scanned_plant: %s <- Var%s: height %.2f m, width %.2f m, LOD triangles %s" % (name, key, h, w, tri))
    for ob in objs:
        bpy.data.objects.remove(ob)
    return meta


def main():
    o = args()
    os.makedirs(o["out"], exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    gltf = find_gltf(o["src"])
    mats = make_maps(find_maps(gltf), o["tex"])
    bpy.ops.import_scene.gltf(filepath=gltf)
    vs = variants_in_scene()
    keys = pick_variants(vs, o)
    m_leaf = bpy.data.materials.new("Leaf")
    m_bb = bpy.data.materials.new("Billboard")
    for k, key in enumerate(keys):
        convert_variant(key, vs[key], "%s-%s" % (o["name"], chr(ord("a") + k)), o, mats, m_leaf, m_bb)


if __name__ == "__main__":
    main()
