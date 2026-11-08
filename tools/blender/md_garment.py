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
units. A group with no rule takes its own "color" or the piece's.

A piece's "surface" adds the base layer stack a texture painter lays under fabric: {"weave": {"pitch" (thread spacing,
mm; keep it at 4 texels or more), "slub"}, "ao", "cavity", "edge" (how much AO, creases and crests shade the albedo),
"normal" (weave normal strength), "ao_distance" (m), "rough_weave", "rough_mottle", "samples", "stitch": {"hem",
"topstitch", "stitch", "pitch" (mm)}}. "stitch" puts the seams (a pressed groove, the allowance and a topstitch) and
the hems (a turned band, a rolled edge and its stitch line) into the height that drives the normal. It bakes ambient
occlusion and curvature in Cycles, folds them into the albedo, and writes NAME_normal.png and NAME_orm.png (occlusion,
roughness, metallic) wired into the material, so a glTF export carries all three. "occluders" (top level) lists glTF
files, or {"file", "objects": [names]}, that shade the AO bake (the body under the garment) and are not kept. After the
bakes, a piece's "decimate" (face ratio) thins the mesh and its "thickness" (m) solidifies it with a closed rim, so
hems and cut ends read as cloth with a turned edge. "thickness_offset" picks the side it grows to: 0 about the sheet,
1 outward, -1 inward (an under layer grows in and an over layer out, so the gap between them is kept). Each piece is written to OUT_DIR/NAME_albedo.png and kept as object NAME in OUT_DIR/garments.blend, with one
double-sided material using that texture; garment_bind.py (or shell_bind.py --keep-material) then skins it to the body.
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
    spec["dir"] = os.path.dirname(spec_path)
    spec["obj"] = os.path.join(spec["dir"], spec["obj"])
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


def weave_height(xm, ym, wv):
    """Plain-weave height 0..1 at tile coordinates in metres: warp threads run along v, weft along u, and they swap
    over at every crossing. "pitch" is the thread spacing in millimetres, "slub" how uneven the threads are."""
    p = wv.get("pitch", 2.5) / 1000.0
    u, v = xm / p, ym / p
    warp = np.abs(np.cos(np.pi * u)) ** 0.6 * (0.55 + 0.45 * np.abs(np.sin(np.pi * v)))
    weft = np.abs(np.cos(np.pi * v)) ** 0.6 * (0.55 + 0.45 * np.abs(np.sin(np.pi * u)))
    over = (np.floor(u) + np.floor(v)) % 2 == 0
    hgt = np.where(over, np.maximum(warp, 0.6 * weft), np.maximum(weft, 0.6 * warp))
    slub = wv.get("slub", 0.25)
    if slub:  # threads vary in thickness along their length
        hgt *= 1 - slub * 0.5 * (1 + np.sin(np.floor(u) * 12.9898 + v * 0.7) * np.sin(np.floor(v) * 78.233 + u * 0.9))
    return hgt


def save_image(name, rgb, out, colour=True):
    res = rgb.shape[0]
    im = bpy.data.images.new(name, res, res, alpha=False, is_data=not colour)
    rgba = np.ones((res, res, 4))
    rgba[..., :3] = rgb
    im.pixels.foreach_set(rgba.astype(np.float32).ravel())
    im.filepath_raw = os.path.join(out, name + ".png")
    im.file_format = "PNG"
    im.save()
    if not colour:
        im.colorspace_settings.name = "Non-Color"
    return im


def import_occluders(spec):
    """Meshes (the body) that shade the garments in the AO bake; main() drops them before saving."""
    occ = []
    for entry in spec.get("occluders", []):
        path, names = (entry, None) if isinstance(entry, str) else (entry["file"], entry.get("objects"))
        before = set(bpy.data.objects)
        bpy.ops.import_scene.gltf(filepath=os.path.join(spec["dir"], path))
        new = [o for o in bpy.data.objects if o not in before]
        for o in new:
            if o.type == "MESH" and (names is None or o.name.split(".")[0] in names):
                mw = o.matrix_world.copy()
                o.parent = None
                o.matrix_world = mw
                for m in list(o.modifiers):
                    o.modifiers.remove(m)
                o["md_occluder"] = True
                occ.append(o)
        for o in new:
            if o not in occ:
                bpy.data.objects.remove(o)
    for o in occ:
        lo = np.min([o.matrix_world @ v.co for v in o.data.vertices], axis=0)
        hi = np.max([o.matrix_world @ v.co for v in o.data.vertices], axis=0)
        print("md_garment: occluder %s spans %s .. %s" % (o.name, np.round(lo, 3), np.round(hi, 3)))
    return occ


def bake(o, kind, res, setup=None):
    """Bakes one Cycles pass of object o into a float image on its UVs; returns (res, res, 3) linear values."""
    im = bpy.data.images.new("bake_" + kind, res, res, alpha=False, float_buffer=True, is_data=True)
    mats = list(o.data.materials)
    if setup:
        o.data.materials.clear()
        o.data.materials.append(setup)
    nt = o.data.materials[0].node_tree
    node = nt.nodes.new("ShaderNodeTexImage")
    node.image = im
    nt.nodes.active = node
    # Only this piece and the occluders take part: another piece (a sash over the tunic) would print its shadow in.
    hidden = [x for x in bpy.data.objects if x is not o and not x.get("md_occluder") and not x.hide_render]
    for x in hidden:
        x.hide_render = True
    bpy.ops.object.select_all(action="DESELECT")
    o.select_set(True)
    bpy.context.view_layer.objects.active = o
    bpy.ops.object.bake(type=kind, margin=8, use_clear=True)
    for x in hidden:
        x.hide_render = False
    px = np.array(im.pixels[:], dtype=np.float32).reshape(res, res, 4)[..., :3]
    nt.nodes.remove(node)
    if setup:
        o.data.materials.clear()
        for m in mats:
            o.data.materials.append(m)
    bpy.data.images.remove(im)
    return px.astype(np.float64)


def blur(a, r):
    """Box blur of a 2D array, r texels each way, three passes (close to a Gaussian)."""
    for _ in range(3):
        for ax in (0, 1):
            pad = [(r + 1, r) if i == ax else (0, 0) for i in range(2)]
            c = np.cumsum(np.pad(a, pad, mode="edge"), axis=ax)
            n = c.shape[ax]
            a = (np.take(c, range(2 * r + 1, n), axis=ax) - np.take(c, range(0, n - 2 * r - 1), axis=ax)) / (2 * r + 1)
    return a


def base_stack(o, name, ps, img, height, out):
    """The base layer stack a texture painter puts under every fabric: baked ambient occlusion multiplied in, curvature
    as cavity (creases darker, crests a touch lighter), a weave normal map and roughness that varies with the weave and
    the creases. Writes NAME_normal.png and NAME_orm.png (glTF packing: occlusion R, roughness G, metallic B) beside
    the albedo, rewrites the albedo with the stack in it, and wires all three into the material."""
    sf = ps["surface"]
    res = img.shape[0]
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    sc.cycles.device = "CPU"
    sc.cycles.samples = sf.get("samples", 64)
    if sc.world is None:
        sc.world = bpy.data.worlds.new("bake")
    sc.world.light_settings.distance = sf.get("ao_distance", 0.08)
    ao = bake(o, "AO", res)[..., 0]
    # Curvature from Cycles pointiness (0.5 flat, lower in a crease), emitted and baked.
    cm = bpy.data.materials.new("bake_curv")
    cm.use_nodes = True
    ct = cm.node_tree
    for n in list(ct.nodes):
        if n.type != "OUTPUT_MATERIAL":
            ct.nodes.remove(n)
    geo, em = ct.nodes.new("ShaderNodeNewGeometry"), ct.nodes.new("ShaderNodeEmission")
    ct.links.new(geo.outputs["Pointiness"], em.inputs["Color"])
    ct.links.new(em.outputs["Emission"], ct.nodes["Material Output"].inputs["Surface"])
    curv = bake(o, "EMIT", res, cm)[..., 0] - 0.5
    bpy.data.materials.remove(cm)
    curv = np.clip(curv / max(np.percentile(np.abs(curv), 98), 1e-4), -1, 1)  # contrast pushed hard, as the stack wants
    lo = np.percentile(ao, 2)
    ao = np.clip((ao - lo) / max(1 - lo, 1e-4), 0, 1)
    ao_k, cav_k, edge_k = sf.get("ao", 0.6), sf.get("cavity", 0.35), sf.get("edge", 0.12)
    shade = (1 - ao_k * (1 - ao)) * (1 - cav_k * np.maximum(-curv, 0)) * (1 + edge_k * np.maximum(curv, 0))
    shade *= 1 - sf.get("weave_albedo", 0.12) * (1 - height)  # thread tops catch light, the gaps stay dark
    lin = np.where(img <= 0.04045, img / 12.92, ((img + 0.055) / 1.055) ** 2.4) * shade[..., None]
    alb = np.where(lin <= 0.0031308, lin * 12.92, 1.055 * np.clip(lin, 0, None) ** (1 / 2.4) - 0.055)
    # Weave normal, OpenGL convention (+Y up the v axis, which is the image's row order).
    st = sf.get("normal", 2.0)
    gy, gx = np.gradient(height)
    nrm = np.dstack([-gx * st, -gy * st, np.ones_like(height)])
    nrm /= np.linalg.norm(nrm, axis=2, keepdims=True)
    # Roughness: rougher in the thread gaps and creases, with slow mottling so a big panel doesn't read as one sheet.
    mott = blur(np.random.default_rng(7).standard_normal((res, res)), max(res // 64, 2))
    mott /= max(np.abs(mott).max(), 1e-6)
    rough = (ps.get("roughness", 0.8) + sf.get("rough_weave", 0.08) * (1 - height) + 0.06 * np.maximum(-curv, 0)
             + sf.get("rough_mottle", 0.05) * mott)
    orm = np.dstack([1 - 0.5 * (1 - ao), np.clip(rough, 0.05, 1), np.zeros_like(rough)])
    im_alb = save_image(name + "_albedo", np.clip(alb, 0, 1), out)
    im_nrm = save_image(name + "_normal", nrm * 0.5 + 0.5, out, colour=False)
    im_orm = save_image(name + "_orm", orm, out, colour=False)
    # Material: ORM through a colour split, the normal through a normal map node, occlusion to the glTF output group.
    nt = o.data.materials[0].node_tree
    bsdf = nt.nodes["Principled BSDF"]
    for n in nt.nodes:
        if n.type == "TEX_IMAGE":
            n.image = im_alb
    t_orm, t_nrm = nt.nodes.new("ShaderNodeTexImage"), nt.nodes.new("ShaderNodeTexImage")
    t_orm.image, t_nrm.image = im_orm, im_nrm
    sep, nmap = nt.nodes.new("ShaderNodeSeparateColor"), nt.nodes.new("ShaderNodeNormalMap")
    nt.links.new(t_orm.outputs["Color"], sep.inputs["Color"])
    nt.links.new(sep.outputs["Green"], bsdf.inputs["Roughness"])
    nt.links.new(sep.outputs["Blue"], bsdf.inputs["Metallic"])
    nt.links.new(t_nrm.outputs["Color"], nmap.inputs["Color"])
    nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
    grp = bpy.data.node_groups.get("glTF Material Output")
    if grp is None:
        grp = bpy.data.node_groups.new("glTF Material Output", "ShaderNodeTree")
        grp.interface.new_socket("Occlusion", in_out="INPUT", socket_type="NodeSocketFloat")
    gn = nt.nodes.new("ShaderNodeGroup")
    gn.node_tree = grp
    nt.links.new(sep.outputs["Red"], gn.inputs["Occlusion"])
    print("md_garment: %s base stack: weave %.1f mm, roughness %.2f..%.2f, shade %.2f..%.2f"
          % (name, sf.get("weave", {}).get("pitch", 2.5), rough.min(), rough.max(), shade.min(), shade.max()))


def stitch_relief(objs, groups, img, height, res, side, st, weld):
    """Seams and hems drawn into the height (so the normal map) and the albedo, from the pieces' own outlines in
    atlas space. A boundary edge whose two ends meet another piece's boundary (within the weld) is a seam: a pressed
    groove on the line with the seam allowance raised beside it, and a topstitch "topstitch" mm in. Any other
    boundary edge is a hem: a turned band "hem" mm wide standing proud, a rolled edge, and a stitch line near the
    band's inner edge. Stitches are dashes "stitch" mm long at "pitch" mm. Widths never go below 1.5 texels."""
    from mathutils import kdtree
    px_mm = res / (side * 1000.0)  # texels per millimetre
    hem_mm, top_mm = st.get("hem", 12.0), st.get("topstitch", 6.0)
    dash, pitch = st.get("stitch", 3.0), st.get("pitch", 4.0)
    groove = max(st.get("groove", 0.8), 1.5 / px_mm)
    thread_rgb = st.get("thread")
    bnd = []  # (group, vert index, co)
    edges = []  # (group, a uv, b uv, inward uv dir, va, vb)
    for g in groups:
        bm = bmesh.new()
        bm.from_mesh(objs[g].data)
        uvl = bm.loops.layers.uv.active
        for e in bm.edges:
            if not e.is_boundary or not e.link_loops:
                continue
            lp = e.link_loops[0]
            a, b = np.array(lp[uvl].uv), np.array(lp.link_loop_next[uvl].uv)
            fc = np.mean([np.array(l[uvl].uv) for l in lp.face.loops], axis=0)
            d = b - a
            nrm = np.array([-d[1], d[0]])
            if nrm @ (fc - a) < 0:
                nrm = -nrm
            nrm /= max(np.linalg.norm(nrm), 1e-12)
            edges.append((g, a, b, nrm, lp.vert.index, lp.link_loop_next.vert.index))
        for v in bm.verts:
            if v.is_boundary:
                bnd.append((g, v.index, v.co.copy()))
        bm.free()
    kd = kdtree.KDTree(len(bnd))
    for k, (_, _, co) in enumerate(bnd):
        kd.insert(co, k)
    kd.balance()
    where = {(g, i): co for g, i, co in bnd}

    def sewn(g, i):
        co = where[(g, i)]
        return any(bnd[k][:2] != (g, i) for _, k, _ in kd.find_range(co, weld * 1.5))
    dmin = np.full((res, res), np.inf)
    s_al = np.zeros((res, res))
    kind = np.zeros((res, res), np.int8)  # 1 seam, 2 hem
    band = (max(hem_mm, top_mm) + 3.0) * px_mm
    n_seam = n_hem = 0
    for g, a, b, nrm, va, vb in edges:
        is_seam = sewn(g, va) and sewn(g, vb)
        n_seam += is_seam
        n_hem += not is_seam
        A, B = a * res, b * res
        lo = np.floor(np.minimum(A, B) - band).astype(int)
        hi = np.ceil(np.maximum(A, B) + band).astype(int)
        x0, y0 = max(lo[0], 0), max(lo[1], 0)
        x1, y1 = min(hi[0], res), min(hi[1], res)
        if x1 <= x0 or y1 <= y0:
            continue
        xs, ys = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
        ab = B - A
        L = max(np.linalg.norm(ab), 1e-9)
        t = ((xs - A[0]) * ab[0] + (ys - A[1]) * ab[1]) / (L * L)
        d = ((xs - A[0]) * nrm[0] + (ys - A[1]) * nrm[1])  # texels, positive inside the piece
        ok = (t >= -0.02) & (t <= 1.02) & (d >= -1.0) & (d <= band)
        cur = dmin[y0:y1, x0:x1]
        better = ok & (d < cur)
        cur[better] = d[better]
        # texels along the edge's own line, so the dashes run on unbroken across the many short edges of one hem
        s_al[y0:y1, x0:x1][better] = (t * L + (A @ ab) / L)[better]
        kind[y0:y1, x0:x1][better] = 1 if is_seam else 2
    d_mm = np.where(np.isfinite(dmin), dmin / px_mm, np.inf)
    s_mm = s_al / px_mm
    on_dash = (s_mm % pitch) < dash
    gauss = lambda x, w: np.exp(-(x / w) ** 2)
    relief = np.zeros((res, res))
    dark = np.zeros((res, res))
    thread = np.zeros((res, res))
    seam, hem = kind == 1, kind == 2
    dm = np.where(np.isfinite(d_mm), d_mm, 1e9)
    # seam: the pressed line, the allowance under the fabric beside it, a topstitch
    relief += seam * (-0.7 * gauss(dm, groove * 1.2) + 0.25 * np.clip((dm - groove) / 2.0, 0, 1)
                      * np.clip((top_mm + 3 - dm) / 2.0, 0, 1))
    dark += seam * 0.35 * gauss(dm, groove * 1.5)
    ts = gauss(dm - top_mm, groove * 0.6) * on_dash
    relief += seam * (0.45 * ts - 0.3 * gauss(dm - top_mm, groove) * ~on_dash)
    thread += seam * ts
    # hem: the turned band, its rolled edge, the stitch near the band's inner edge
    band_up = np.clip(dm / 1.5, 0, 1) * np.clip((hem_mm - dm) / 1.5, 0, 1)
    relief += hem * (0.35 * band_up - 0.4 * gauss(dm, groove))
    hs = gauss(dm - (hem_mm - 3.0), groove * 0.6) * on_dash
    relief += hem * (0.45 * hs - 0.3 * gauss(dm - (hem_mm - 3.0), groove) * ~on_dash)
    thread += hem * hs
    dark += hem * 0.2 * gauss(dm - hem_mm, groove * 1.5)
    height += relief * st.get("depth", 1.0)
    shade = 1 - st.get("darken", 0.25) * np.clip(dark, 0, 1)
    img *= shade[..., None]
    if thread_rgb is not None:
        tw = np.clip(thread, 0, 1)[..., None] * 0.85
        img[:] = img * (1 - tw) + np.array(thread_rgb)[None, None] * tw
    print("md_garment: stitch relief: %d seam edges, %d hem edges, %.2f texels/mm, groove %.2f mm"
          % (n_seam, n_hem, px_mm, groove))


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
    surf = ps.get("surface")
    height = np.zeros((res, res)) if surf else None
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
        if surf:  # weave height from the tile's real size, so the threads keep one pitch on every panel
            hgt = weave_height(tu * w, tv * h, surf.get("weave", {}))
            height[y0p:y1p, x0p:x1p] = hgt.reshape(hgt.shape[0] // ss, ss, hgt.shape[1] // ss, ss).mean(axis=(1, 3))
    if surf and surf.get("stitch"):
        stitch_relief(objs, groups, img, height, res, side, surf["stitch"], weld)
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
    im = save_image(name + "_albedo", np.clip(img, 0, 1), out)
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
    if surf:
        base_stack(o, name, ps, img, height, out)
    # Game geometry after the bakes, which want the single sheet: fewer faces, then the cloth's thickness, so a hem
    # or a tail end shows a turned edge instead of a paper-thin card.
    if ps.get("decimate"):
        dec = o.modifiers.new("decimate", "DECIMATE")
        dec.ratio = ps["decimate"]
        bpy.ops.object.modifier_apply(modifier=dec.name)
    if ps.get("thickness"):
        sol = o.modifiers.new("thickness", "SOLIDIFY")
        sol.thickness, sol.offset = ps["thickness"], ps.get("thickness_offset", 0.0)
        # Plain offset: each vertex moves one thickness along its normal. Even offset scales that by the crease angle
        # and shoots a vertex a metre off where panels meet at a sharp seam (a crotch, a collar).
        sol.use_rim = True
        bpy.ops.object.modifier_apply(modifier=sol.name)
    path = im.filepath_raw
    print("md_garment: %s groups %s, %d -> %d vertices, %d faces, atlas %.2f m -> %s"
          % (name, groups, n0, len(o.data.vertices), len(o.data.polygons), side, path))
    return o


def main():
    spec, out = _spec()
    os.makedirs(out, exist_ok=True)
    objs = import_groups(spec)
    named = {g for ps in spec["pieces"].values() for g in ps["groups"]}
    for g in [g for g in objs if g not in named]:  # the avatar, which would shade the AO bake where it pokes through
        bpy.data.objects.remove(objs.pop(g))
    import_occluders(spec)  # shade the AO bake; dropped with the other unnamed objects below
    keep = set()
    for name, ps in spec["pieces"].items():
        keep.add(build_piece(name, ps, objs, spec.get("weld", 0.002), out).name)
    for o in list(bpy.data.objects):
        if o.name not in keep:
            bpy.data.objects.remove(o)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "garments.blend"))


main()
