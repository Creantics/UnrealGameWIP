"""Turns a sewn garment exported from a cloth tool (one OBJ group per pattern piece, UVs in pattern space) into game
pieces, headless: each piece joins its pattern groups, welds their seams, packs the groups into one texture atlas and
paints that texture per texel from colour rules written in pattern millimetres, so panel edges are as crisp as the
texture allows.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/md_garment.py -- SPEC.json OUT_DIR

SPEC.json:
    {"obj": "garment.obj", "scale": 0.01, "forward": "NEGATIVE_Z", "up": "Y", "weld": 0.002,
     "pieces": {"NAME": {"groups": ["front", "back", ...], "tex": 2048, "roughness": 0.8, "color": [r, g, b],
                         "rules": {"front": {"box": [x0, y0, x1, y1], "color": [r, g, b],
                                             "strips": [{"y": [...], "x0": [...], "x1": [...], "color": [r, g, b]}]}}}}}

"obj" is read against the spec's folder; "scale", "forward" and "up" undo the export so the pieces land where the body
is in world space (an OBJ in centimetres, Y up, -Z forward by default). Groups not named by a piece (the avatar) are
dropped. Colours are sRGB. A group's "box" is its pattern bounding box in millimetres (x0, y0, x1, y1); the group's UV
bounding box is mapped onto it, so a rule reads pattern coordinates. Without a box the rule coordinates are 0..1 of the
UV box. A strip paints where |x| lies between x0(y) and x1(y), each linear in y between the listed "y" stations
(mirrored about x = 0; "mirror": false paints only the listed side). A "crest" {"center": [x, y], "r", "rays", "color",
"sharp" (ray exponent: above 1 pointed), "disc" (radius fraction), "base" (where the rays start), "alt" (shorter rays
between the long ones, that fraction long), "outline": [width, RGB]} paints a rayed sun over the strips, in the rule's
units. A group with no rule takes its own "color" or the piece's. Each piece is written to OUT_DIR/NAME_albedo.png and kept as object NAME in OUT_DIR/garments.blend, with one
double-sided material using that texture; shell_bind.py --keep-material then skins it to the body.
"""
import json
import os
import sys

import bmesh
import bpy
import numpy as np


def _spec():
    argv = sys.argv[sys.argv.index("--") + 1:]
    if len(argv) < 2:
        sys.exit("md_garment: SPEC.json OUT_DIR")
    spec_path, out = os.path.abspath(argv[0]), os.path.abspath(argv[1])
    spec = json.load(open(spec_path))
    spec["obj"] = os.path.join(os.path.dirname(spec_path), spec["obj"])
    return spec, out


def import_groups(spec):
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o)
    bpy.ops.wm.obj_import(filepath=spec["obj"], global_scale=spec.get("scale", 0.01),
                          forward_axis=spec.get("forward", "NEGATIVE_Z"), up_axis=spec.get("up", "Y"),
                          use_split_objects=False, use_split_groups=True)
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)  # the importer leaves scale on the object
    return {o.name.split(".")[0]: o for o in bpy.data.objects if o.type == "MESH"}


def shelf_pack(sizes, pad):
    """Places rectangles (w, h) in a square by rows, tallest first; returns offsets and the square's side."""
    order = sorted(range(len(sizes)), key=lambda i: -sizes[i][1])
    area = sum((w + pad) * (h + pad) for w, h in sizes)
    side = max(max(w for w, _ in sizes) + 2 * pad, np.sqrt(area) * 1.05)
    for _ in range(60):
        pos, x, y, row = [None] * len(sizes), pad, pad, 0.0
        for i in order:
            w, h = sizes[i]
            if x + w + pad > side:
                x, y, row = pad, y + row + pad, 0.0
            pos[i] = (x, y)
            x += w + pad
            row = max(row, h)
        if y + row + pad <= side:
            return pos, side
        side *= 1.04
    sys.exit("md_garment: atlas packing failed")


def rule_colour(px, py, rule, base):
    """Colour per texel (px, py arrays in pattern units) from a group's rule."""
    c = np.empty(px.shape + (3,))
    c[:] = rule.get("color", base)
    for s in rule.get("strips", []):
        x0 = np.interp(py, s["y"], s["x0"])
        x1 = np.interp(py, s["y"], s["x1"])
        ax = np.abs(px) if s.get("mirror", True) else px
        c[(ax >= x0) & (ax <= x1)] = s["color"]
    cr = rule.get("crest")
    if cr:
        dx, dy = px - cr["center"][0], py - cr["center"][1]
        rr, aa = np.hypot(dx, dy), np.arctan2(dy, dx)
        rays, disc, base, alt = cr.get("rays", 12), cr.get("disc", 0.42), cr.get("base", 0.55), cr.get("alt", 0)

        def sun(a_):
            tri = lambda u: np.maximum(0, 1 - np.abs((u % 1) - 0.5) * 2) ** cr.get("sharp", 0.5)
            u = a_ * rays / (2 * np.pi) + 0.5  # a long ray points straight up
            spike = cr["r"] * (base + (1 - base) * tri(u))
            if alt:  # shorter rays between the long ones, "alt" times the radius
                spike = np.maximum(spike, cr["r"] * (base + (alt - base) * tri(u + 0.5)))
            return np.maximum(spike, cr["r"] * disc)
        inside = rr < sun(aa)
        ol = cr.get("outline")  # [width, RGB]: a rim round the sun and a ring round its disc
        if ol:
            da = ol[0] / np.maximum(rr, 1e-4)
            c[rr - ol[0] < np.maximum.reduce([sun(aa), sun(aa + da), sun(aa - da)])] = ol[1]
            inside &= np.abs(rr - cr["r"] * disc) > ol[0] / 2
        c[inside] = cr["color"]
    return c


def build_piece(name, ps, objs, weld, out):
    groups = [g for g in ps["groups"] if g in objs]
    missing = set(ps["groups"]) - set(groups)
    if missing:
        sys.exit("md_garment: %s: groups not in the OBJ: %s" % (name, ", ".join(sorted(missing))))
    base = ps.get("color", [0.8, 0.8, 0.8])
    rules = ps.get("rules", {})
    # Each group's UV box and its size on the body (texel density follows the real panel size).
    info = []
    for g in groups:
        me = objs[g].data
        uv = np.zeros(len(me.loops) * 2)
        me.uv_layers.active.data.foreach_get("uv", uv)
        uv = uv.reshape(-1, 2)
        lo, hi = uv.min(0), uv.max(0)
        # Metres per UV unit along u and along v (a cloth tool may scale them differently): least squares over the
        # triangle edges of |edge|^2 = (ku du)^2 + (kv dv)^2.
        me.calc_loop_triangles()
        co = np.zeros(len(me.vertices) * 3)
        me.vertices.foreach_get("co", co)
        co = co.reshape(-1, 3)
        lv = np.zeros(len(me.loops), dtype=np.int64)
        me.loops.foreach_get("vertex_index", lv)
        tri = np.array([t.loops[:] for t in me.loop_triangles])
        a, b = tri.ravel(), np.roll(tri, -1, axis=1).ravel()
        d3 = ((co[lv[a]] - co[lv[b]]) ** 2).sum(1)
        duv = (uv[a] - uv[b]) ** 2
        k2 = np.linalg.lstsq(duv, d3, rcond=None)[0]
        k = np.sqrt(np.maximum(k2, 1e-12))
        info.append({"g": g, "lo": lo, "hi": hi, "size": ((hi - lo) * k).tolist()})
    pos, side = shelf_pack([i["size"] for i in info], pad=0.02)
    res = int(ps.get("tex", 2048))
    img = np.zeros((res, res, 3))
    img[:] = base
    for i, (ox, oy) in zip(info, pos):
        g, (w, h) = i["g"], i["size"]
        me = objs[g].data
        uvl = me.uv_layers.active.data
        uv = np.zeros(len(uvl) * 2)
        uvl.foreach_get("uv", uv)
        uv = uv.reshape(-1, 2)
        t = (uv - i["lo"]) / np.maximum(i["hi"] - i["lo"], 1e-12)  # 0..1 inside the group's UV box
        new = (np.array([ox, oy]) + t * np.array([w, h])) / side
        uvl.foreach_set("uv", new.ravel())
        # Paint the tile (plus a texel border) from the rule, 2x supersampled.
        x0p, y0p = int(ox / side * res) - 1, int(oy / side * res) - 1
        x1p, y1p = int(np.ceil((ox + w) / side * res)) + 1, int(np.ceil((oy + h) / side * res)) + 1
        x0p, y0p, x1p, y1p = max(x0p, 0), max(y0p, 0), min(x1p, res), min(y1p, res)
        ss = 2
        xs = (np.arange(x0p * ss, x1p * ss) + 0.5) / (res * ss) * side
        ys = (np.arange(y0p * ss, y1p * ss) + 0.5) / (res * ss) * side
        tu, tv = np.meshgrid(np.clip((xs - ox) / w, 0, 1), np.clip((ys - oy) / h, 0, 1))
        rule = rules.get(g, {"color": ps.get("colors", {}).get(g, base)})
        bx = rule.get("box", [0.0, 0.0, 1.0, 1.0])
        px, py = bx[0] + tu * (bx[2] - bx[0]), bx[1] + tv * (bx[3] - bx[1])
        c = rule_colour(px, py, rule, base)
        c = c.reshape(c.shape[0] // ss, ss, c.shape[1] // ss, ss, 3)
        lin = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4).mean(axis=(1, 3))
        img[y0p:y1p, x0p:x1p] = np.where(lin <= 0.0031308, lin * 12.92, 1.055 * lin ** (1 / 2.4) - 0.055)
    # Join the groups into one object and weld the seams the cloth tool sewed.
    bpy.ops.object.select_all(action="DESELECT")
    for g in groups:
        objs[g].select_set(True)
    bpy.context.view_layer.objects.active = objs[groups[0]]
    bpy.ops.object.join()
    o = bpy.context.view_layer.objects.active
    o.name = o.data.name = name
    bm = bmesh.new()
    bm.from_mesh(o.data)
    n0 = len(bm.verts)
    bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=weld)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    bm.to_mesh(o.data)
    bm.free()
    for p in o.data.polygons:
        p.use_smooth = True
    # Texture and material.
    path = os.path.join(out, name + "_albedo.png")
    im = bpy.data.images.new(name + "_albedo", res, res, alpha=False)
    rgba = np.ones((res, res, 4))
    rgba[..., :3] = np.clip(img, 0, 1)
    im.pixels.foreach_set(rgba.astype(np.float32).ravel())
    im.filepath_raw = path
    im.file_format = "PNG"
    im.save()
    mat = bpy.data.materials.new(name)
    mat.use_backface_culling = False
    if mat.node_tree is None:
        mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = im
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = ps.get("roughness", 0.8)
    o.data.materials.clear()
    o.data.materials.append(mat)
    print("md_garment: %s groups %s, %d -> %d vertices, %d faces, atlas %.2f m -> %s"
          % (name, groups, n0, len(o.data.vertices), len(o.data.polygons), side, path))
    return o


def main():
    spec, out = _spec()
    os.makedirs(out, exist_ok=True)
    objs = import_groups(spec)
    keep = set()
    for name, ps in spec["pieces"].items():
        keep.add(build_piece(name, ps, objs, spec.get("weld", 0.002), out).name)
    for o in list(bpy.data.objects):
        if o.name not in keep:
            bpy.data.objects.remove(o)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "garments.blend"))


main()
