"""A landmark clock tower and its plaza furniture, modelled by script for a present-day coastal city.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_clock_tower.py -- <out_dir> [--previews]

Writes into <out_dir>:
  clock_tower.glb               the tower (about 37.5 m, 9 x 9 m plinth), origin at ground centre, front (door) faces -Y
  clock_hand_hour.glb           hands: origin at the pivot, pointing up (+Z) at 12, lying in the XZ plane, face normal -Y
  clock_hand_minute.glb
  clock_sockets.txt             per dial: pivot, outward normal, dial radius, yaw for the hands
  plaza_bench.glb, plaza_lamp.glb, plaza_steps.glb, plaza_bollard.glb, plaza_tree_grate.glb
  clock_stats.json              measured triangle counts, bounds, slots and texture sizes (the README is written from it)
  README.md
With --previews it then imports the written files back and renders Cycles stills into <out_dir>/previews: each asset
from three sides, and the tower in context (a boulevard with 15 to 25 m blocks, seen from 150 m, 60 m, 8 m and 3 m).

Every texture is generated here (tileable, 1024 px at most) and the UVs are planar per face in metres divided by each
material's tile size, so texel density is the same everywhere (about 430 px per metre on stone, 570 on brick).
Each glb is read back and checked: one UV set, no vertex colour, embedded PNG/JPEG of 1024 px or less, bounds.
"""
import json
import math
import os
import struct
import sys
import tempfile

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector

TEX = 1024
_TMP = tempfile.mkdtemp(prefix="clock_tower_")


# =====================================================================================================================
# Textures (rows bottom to top, values are display sRGB for colour maps)
# =====================================================================================================================

def _uv(tile_m, n=TEX):
    ys, xs = np.mgrid[0:n, 0:n]
    return (xs + 0.5) / n * tile_m, (ys + 0.5) / n * tile_m


def pnoise(rng, sigma_px, n=TEX, aniso=(1.0, 1.0)):
    """Tileable gaussian-filtered noise with unit deviation. aniso stretches it (x, y)."""
    w = rng.standard_normal((n, n))
    ky = np.fft.fftfreq(n)[:, None] * aniso[1]
    kx = np.fft.fftfreq(n)[None, :] * aniso[0]
    g = np.exp(-2.0 * (math.pi * sigma_px) ** 2 * (kx ** 2 + ky ** 2))
    r = np.real(np.fft.ifft2(np.fft.fft2(w) * g))
    return r / (r.std() + 1e-9)


def fbm(rng, sigmas, weights, aniso=(1.0, 1.0)):
    return sum(wt * pnoise(rng, s, aniso=aniso) for s, wt in zip(sigmas, weights))


def smooth01(x):
    x = np.clip(x, 0.0, 1.0)
    return x * x * (3 - 2 * x)


def normal_from_height(h, tile_m, depth_m, n=TEX):
    """h in [0, 1] mapped to depth_m metres of relief; tileable central differences."""
    px = tile_m / n
    hm = h * depth_m
    dx = (np.roll(hm, -1, axis=1) - np.roll(hm, 1, axis=1)) / (2 * px)
    dy = (np.roll(hm, -1, axis=0) - np.roll(hm, 1, axis=0)) / (2 * px)
    nn = np.dstack([-dx, -dy, np.ones_like(hm)])
    nn /= np.linalg.norm(nn, axis=2, keepdims=True)
    return nn * 0.5 + 0.5


def save_image(name, rgb, colorspace, fmt):
    n = rgb.shape[0]
    rgba = np.dstack([np.clip(rgb, 0, 1), np.ones((n, n))]).astype(np.float32)
    gen = bpy.data.images.new(name + "_gen", n, n, alpha=False)
    gen.colorspace_settings.name = colorspace
    gen.pixels.foreach_set(rgba.ravel())
    ext = ".jpg" if fmt == "JPEG" else ".png"
    path = os.path.join(_TMP, name + ext)
    gen.filepath_raw = path
    gen.file_format = fmt
    try:
        gen.save(filepath=path, quality=90)
    except TypeError:
        gen.save()
    bpy.data.images.remove(gen)
    img = bpy.data.images.load(path)
    img.name = name
    img.colorspace_settings.name = colorspace
    img.pack()
    return img, rgb.reshape(-1, 3).mean(axis=0)


def segments_id(u, cuts, period):
    """Index of the interval of `cuts` (sorted positions in [0, period)) that u falls in, wrapping; and the distance
    to the nearest cut."""
    cuts = np.asarray(cuts)
    uu = np.mod(u, period)
    idx = np.searchsorted(cuts, uu, side="right") - 1
    idx = np.mod(idx, len(cuts))
    d = np.min(np.abs(((uu[..., None] - cuts[None, None, :]) + period / 2) % period - period / 2), axis=-1)
    return idx, d


def tex_ashlar(prefix, seed, tile=2.4, course=0.6, base=(0.70, 0.68, 0.63), joint=0.004, tone=0.07,
               joint_col=(0.50, 0.49, 0.46), soot=0.0):
    rng = np.random.default_rng(seed)
    u, v = _uv(tile)
    nc = int(round(tile / course))
    c = np.floor(v / course).astype(int) % nc
    dv = np.minimum(v % course, course - (v % course))
    bid = np.zeros_like(c)
    du = np.zeros_like(u)
    for k in range(nc):
        lens = rng.uniform(0.5, 1.3, 8)
        cuts = np.cumsum(lens)
        cuts = cuts[cuts < tile - 0.35]
        cuts = np.concatenate([[0.0], cuts]) + rng.uniform(0, tile)
        cuts = np.sort(np.mod(cuts, tile))
        m = c == k
        i, d = segments_id(u, cuts, tile)
        bid = np.where(m, i + 20 * k, bid)
        du = np.where(m, d, du)
    dist = np.minimum(du, dv)
    block_tone = rng.uniform(-1, 1, 20 * nc + 20)[bid]
    fine = pnoise(rng, 0.8)
    mid = fbm(rng, [6, 24, 70], [0.4, 0.35, 0.25])
    streak = pnoise(rng, 60, aniso=(0.15, 1.0))  # long in v: vertical rain streaks
    col = np.array(base)[None, None, :] * (1 + tone * block_tone[..., None] + 0.035 * fine[..., None]
                                          + 0.05 * mid[..., None])
    col = col * (1 - soot * smooth01(streak * 0.5 + 0.3)[..., None])
    arris = smooth01((dist - joint) / 0.012)
    jm = (dist < joint)[..., None]
    col = np.where(jm, np.array(joint_col)[None, None, :] * (1 + 0.06 * fine[..., None]), col)
    col = col * (0.88 + 0.12 * arris[..., None])
    h = np.where(dist < joint, 0.0, 0.55 + 0.45 * arris) + 0.04 * mid + 0.015 * fine
    h = h + 0.03 * block_tone * arris
    return (save_image(prefix + "_BC", col, "sRGB", "JPEG"),
            save_image(prefix + "_N", normal_from_height(h, tile, 0.012), "Non-Color", "PNG"))


def tex_brick(prefix, seed, tile=1.8):
    rng = np.random.default_rng(seed)
    u, v = _uv(tile)
    bl, ch = 0.225, 0.075
    c = np.floor(v / ch).astype(int)
    header = (c % 6) == 5
    unit = np.where(header, bl / 2, bl)
    off = np.where(c % 2 == 1, bl / 2, 0.0) + np.where(header, bl / 4, 0.0)
    b = np.floor((u + off) / unit).astype(int)
    lu = (u + off) % unit
    lv = v % ch
    du = np.minimum(lu, unit - lu)
    dv = np.minimum(lv, ch - lv)
    dist = np.minimum(du, dv)
    nb_ = np.where(header, 16, 8)
    key = (c % 24) * 17 + np.mod(b, nb_)
    r1 = rng.uniform(0, 1, 24 * 17 + 17)[key]
    r2 = rng.uniform(-1, 1, 24 * 17 + 17)[key]
    palette = np.array([(0.36, 0.26, 0.21), (0.30, 0.23, 0.20), (0.24, 0.21, 0.20), (0.42, 0.29, 0.22),
                        (0.33, 0.28, 0.25)])
    pick = np.minimum((r1 * len(palette)).astype(int), len(palette) - 1)
    col = palette[pick] * (1 + 0.08 * r2[..., None])
    col = np.where(header[..., None], col * 0.8, col)
    fine = pnoise(rng, 0.9)
    mid = fbm(rng, [3, 12, 50], [0.4, 0.35, 0.25])
    soot = smooth01(pnoise(rng, 60, aniso=(1.0, 0.2)) * 0.35 + 0.45)
    col = col * (1 + 0.06 * fine[..., None] + 0.06 * mid[..., None])
    mortar = np.array([0.55, 0.53, 0.49]) * (1 + 0.07 * fine[..., None])
    jm = (dist < 0.005)[..., None]
    col = np.where(jm, mortar, col)
    col = col * (1 - 0.35 * soot[..., None])
    arris = smooth01((dist - 0.005) / 0.006)
    h = np.where(dist < 0.005, 0.0, 0.5 + 0.5 * arris) + 0.05 * mid + 0.02 * fine
    return (save_image(prefix + "_BC", col, "sRGB", "JPEG"),
            save_image(prefix + "_N", normal_from_height(h, tile, 0.012), "Non-Color", "PNG"))


def tex_tile(prefix, seed, tile=1.2, size=0.15, glaze=(0.10, 0.32, 0.34)):
    rng = np.random.default_rng(seed)
    u, v = _uv(tile)
    iu, iv = np.floor(u / size).astype(int), np.floor(v / size).astype(int)
    lu, lv = u % size, v % size
    dist = np.minimum(np.minimum(lu, size - lu), np.minimum(lv, size - lv))
    k = (iu % 8) * 8 + (iv % 8)
    t = rng.uniform(-1, 1, 64)[k]
    t2 = rng.uniform(0, 1, 64)[k]
    fine = pnoise(rng, 1.2)
    pool = fbm(rng, [8, 20], [0.6, 0.4])
    col = np.array(glaze)[None, None, :] * (1 + 0.16 * t[..., None] + 0.05 * pool[..., None])
    col = np.where((t2 > 0.86)[..., None], col * np.array([0.75, 0.85, 0.95]), col)
    # glaze breaks paler at the tile edges
    edge = 1 - smooth01((dist - 0.002) / 0.012)
    col = col + edge[..., None] * 0.10
    grout = (dist < 0.0022)[..., None]
    col = np.where(grout, np.array([0.58, 0.57, 0.54]) * (1 + 0.05 * fine[..., None]), col)
    pillow = smooth01((dist - 0.002) / 0.01)
    h = np.where(dist < 0.0022, 0.0, 0.6 + 0.4 * pillow) + 0.01 * pool
    rough = np.where(dist < 0.0022, 0.85, 0.12 + 0.06 * (pool * 0.5 + 0.5))
    mr = np.dstack([np.ones_like(u), rough, np.zeros_like(u)])
    return (save_image(prefix + "_BC", col, "sRGB", "JPEG"),
            save_image(prefix + "_N", normal_from_height(h, tile, 0.004), "Non-Color", "PNG"),
            save_image(prefix + "_MR", mr, "Non-Color", "PNG"))


def tex_seam_roof(prefix, seed, tile=2.0, pitch=0.5):
    rng = np.random.default_rng(seed)
    u, v = _uv(tile)
    lu = u % pitch
    d = np.minimum(lu, pitch - lu)
    seam = 1 - smooth01((d - 0.008) / 0.012)
    streak = pnoise(rng, 40, aniso=(1.0, 0.08))
    fine = pnoise(rng, 1.0)
    col = np.array([0.30, 0.32, 0.33])[None, None, :] * (1 + 0.10 * streak[..., None] + 0.03 * fine[..., None])
    col = col * (1 + 0.15 * seam[..., None])
    h = seam + 0.03 * pnoise(rng, 12)
    return (save_image(prefix + "_BC", col, "sRGB", "JPEG"),
            save_image(prefix + "_N", normal_from_height(h, tile, 0.025), "Non-Color", "PNG"))


def tex_timber(prefix, seed, tile=1.2, board=0.15, base=(0.30, 0.19, 0.12)):
    """Boards run along u (grain along u), board joints every `board` along v."""
    rng = np.random.default_rng(seed)
    u, v = _uv(tile)
    bi = np.floor(v / board).astype(int) % int(round(tile / board))
    lv = v % board
    d = np.minimum(lv, board - lv)
    t = rng.uniform(-1, 1, 64)[bi]
    grain = pnoise(rng, 2.0, aniso=(0.04, 1.0))
    knots = pnoise(rng, 10)
    col = np.array(base)[None, None, :] * (1 + 0.12 * t[..., None] + 0.12 * grain[..., None]
                                          - 0.1 * smooth01(knots - 1.5)[..., None])
    gap = (d < 0.003)[..., None]
    col = np.where(gap, col * 0.35, col)
    h = np.where(d < 0.003, 0.0, 0.7 + 0.3 * smooth01((d - 0.003) / 0.006)) + 0.05 * grain
    return (save_image(prefix + "_BC", col, "sRGB", "JPEG"),
            save_image(prefix + "_N", normal_from_height(h, tile, 0.006), "Non-Color", "PNG"))


def tex_granite(prefix, seed, tile=1.0, base=(0.52, 0.52, 0.51)):
    rng = np.random.default_rng(seed)
    fine = pnoise(rng, 0.7)
    fleck = pnoise(rng, 1.5)
    mid = fbm(rng, [10, 40], [0.6, 0.4])
    col = np.array(base)[None, None, :] * (1 + 0.10 * fine[..., None] + 0.04 * mid[..., None])
    col = np.where((fleck > 2.0)[..., None], col * 0.45, col)
    col = np.where((fleck < -2.2)[..., None], col * 1.25, col)
    h = 0.5 + 0.15 * fine + 0.2 * mid
    return (save_image(prefix + "_BC", col, "sRGB", "JPEG"),
            save_image(prefix + "_N", normal_from_height(h, tile, 0.0015), "Non-Color", "PNG"))


# =====================================================================================================================
# Materials
# =====================================================================================================================

def _new_mat(name):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    out.location = (400, 0)
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    mat.use_backface_culling = True
    return mat, nt, bsdf


def mat_flat(name, srgb, rough, metal=0.0):
    mat, nt, bsdf = _new_mat(name)
    lin = [c ** 2.2 for c in srgb]
    bsdf.inputs["Base Color"].default_value = (*lin, 1.0)
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["Metallic"].default_value = metal
    mat.diffuse_color = (*lin, 1.0)
    mat.roughness = rough
    mat.metallic = metal
    return mat


def mat_tex(name, bc, nrm, rough, metal=0.0, mr=None, strength=1.0):
    mat, nt, bsdf = _new_mat(name)
    (img, mean) = bc
    t = nt.nodes.new("ShaderNodeTexImage")
    t.image = img
    t.location = (-600, 300)
    nt.links.new(t.outputs["Color"], bsdf.inputs["Base Color"])
    nimg = nrm[0]
    tn = nt.nodes.new("ShaderNodeTexImage")
    tn.image = nimg
    tn.location = (-600, -200)
    nm = nt.nodes.new("ShaderNodeNormalMap")
    nm.inputs["Strength"].default_value = strength
    nt.links.new(tn.outputs["Color"], nm.inputs["Color"])
    nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    if mr is not None:
        tm = nt.nodes.new("ShaderNodeTexImage")
        tm.image = mr[0]
        tm.location = (-900, 0)
        sep = nt.nodes.new("ShaderNodeSeparateColor")
        nt.links.new(tm.outputs["Color"], sep.inputs["Color"])
        nt.links.new(sep.outputs["Green"], bsdf.inputs["Roughness"])
        nt.links.new(sep.outputs["Blue"], bsdf.inputs["Metallic"])
    else:
        bsdf.inputs["Roughness"].default_value = rough
        bsdf.inputs["Metallic"].default_value = metal
    lin = [float(c) ** 2.2 for c in mean]
    mat.diffuse_color = (*lin, 1.0)
    return mat


# =====================================================================================================================
# Geometry accumulator
# =====================================================================================================================

class Acc:
    """Collects world-space pieces with a material slot name and a smooth flag; becomes one mesh object."""

    def __init__(self):
        self.v, self.f, self.m, self.s = [], [], [], []

    def _push(self, verts, faces, mat, smooth):
        o = len(self.v)
        self.v += [tuple(p) for p in verts]
        for fc in faces:
            self.f.append(tuple(i + o for i in fc))
            self.m.append(mat)
            self.s.append(smooth)

    def closed(self, verts, faces, mat, M=None, bevel=0.0, smooth=False):
        """A closed piece: normals recalculated, optional bevel of every edge."""
        bm = bmesh.new()
        bv = [bm.verts.new(Vector(p)) for p in verts]
        for fc in faces:
            bm.faces.new([bv[i] for i in fc])
        bm.normal_update()
        if bevel > 0:
            bmesh.ops.bevel(bm, geom=list(bm.edges), offset=bevel, segments=1, profile=0.5, affect="EDGES",
                            clamp_overlap=True)
        bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
        if M is not None:
            bm.transform(M)
        bm.verts.index_update()
        verts2 = [tuple(v.co) for v in bm.verts]
        faces2 = [tuple(v.index for v in f.verts) for f in bm.faces]
        bm.free()
        self._push(verts2, faces2, mat, smooth)

    def box(self, x0, y0, z0, x1, y1, z1, mat, M=None, bevel=0.0):
        xs, ys, zs = (x0, x1), (y0, y1), (z0, z1)
        verts = [(xs[i & 1], ys[(i >> 1) & 1], zs[(i >> 2) & 1]) for i in range(8)]
        faces = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]
        self.closed(verts, faces, mat, M, bevel)

    def boxc(self, cx, cy, cz0, sx, sy, sz, mat, M=None, bevel=0.0):
        self.box(cx - sx / 2, cy - sy / 2, cz0, cx + sx / 2, cy + sy / 2, cz0 + sz, mat, M, bevel)

    def prism_xz(self, poly, y0, y1, mat, M=None, bevel=0.0):
        """A polygon in the XZ plane extruded along Y."""
        n = len(poly)
        verts = [(x, y0, z) for x, z in poly] + [(x, y1, z) for x, z in poly]
        faces = [tuple(range(n)), tuple(range(2 * n - 1, n - 1, -1))]
        faces += [(i, (i + 1) % n, n + (i + 1) % n, n + i) for i in range(n)]
        self.closed(verts, faces, mat, M, bevel)

    def prism_xy(self, poly, z0, z1, mat, M=None, bevel=0.0):
        n = len(poly)
        verts = [(x, y, z0) for x, y in poly] + [(x, y, z1) for x, y in poly]
        faces = [tuple(range(n)), tuple(range(2 * n - 1, n - 1, -1))]
        faces += [(i, (i + 1) % n, n + (i + 1) % n, n + i) for i in range(n)]
        self.closed(verts, faces, mat, M, bevel)

    def lathe(self, prof, seg, mat, M=None, sharp_deg=35.0, smooth=True, phase=0.0):
        """Surface of revolution about +Z. prof = [(r, z), ...] traversed counter-clockwise in the (r, z) plane
        (bottom axis -> out -> up -> top axis) for outward normals. Bands split where the profile turns sharply."""
        pts = [(float(r), float(z)) for r, z in prof]
        M = M or Matrix.Identity(4)
        verts, faces = [], []
        angs = [phase + 2 * math.pi * j / seg for j in range(seg)]

        def ring(r, z):
            if r < 1e-6:
                verts.append(tuple(M @ Vector((0, 0, z))))
                return [len(verts) - 1] * seg
            base = len(verts)
            for a in angs:
                verts.append(tuple(M @ Vector((r * math.cos(a), r * math.sin(a), z))))
            return list(range(base, base + seg))

        def turn(i):
            if i <= 0 or i >= len(pts) - 1:
                return 180.0
            a = Vector((pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]))
            b = Vector((pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1]))
            if a.length < 1e-9 or b.length < 1e-9:
                return 180.0
            return math.degrees(a.angle(b))

        prev = ring(*pts[0])
        for i in range(1, len(pts)):
            cur = ring(*pts[i])
            for j in range(seg):
                j1 = (j + 1) % seg
                a0, a1, b0, b1 = prev[j], prev[j1], cur[j], cur[j1]
                if a0 == a1:
                    faces.append((a0, b1, b0))
                elif b0 == b1:
                    faces.append((a0, a1, b0))
                else:
                    faces.append((a0, a1, b1, b0))
            if i < len(pts) - 1 and turn(i) > sharp_deg and pts[i][0] > 1e-6:
                prev = ring(*pts[i])
            else:
                prev = cur
        self._push(verts, faces, mat, smooth)

    def to_object(self, name, mats, collection=None):
        me = bpy.data.meshes.new(name)
        me.from_pydata(self.v, [], self.f)
        me.update()
        for m in mats:
            me.materials.append(m)
        idx = {m.name: i for i, m in enumerate(mats)}
        me.polygons.foreach_set("material_index", [idx[n] for n in self.m])
        me.polygons.foreach_set("use_smooth", self.s)
        me.update()
        ob = bpy.data.objects.new(name, me)
        (collection or bpy.context.scene.collection).objects.link(ob)
        return ob


def cyl_y(acc, cx, cz, r, y0, y1, mat, seg=32, M=None):
    """Closed cylinder along Y (a cutter or a drum)."""
    poly = [(cx + r * math.cos(2 * math.pi * k / seg), cz + r * math.sin(2 * math.pi * k / seg)) for k in range(seg)]
    acc.prism_xz(poly, y0, y1, mat, M)


def arch_poly(w, z0, zs, seg=16):
    """Round-headed opening outline in XZ: width w, sill z0, springing zs (CCW)."""
    r = w / 2
    pts = [(-r, z0), (r, z0)]
    for k in range(seg + 1):
        a = math.pi * k / seg
        pts.append((r * math.cos(a), zs + r * math.sin(a)))
    pts = pts[:2] + pts[2:]
    # remove duplicate (r, zs) == next of (r, z0) chain is fine; ensure no repeated point
    out = []
    for p in pts:
        if not out or (abs(out[-1][0] - p[0]) > 1e-6 or abs(out[-1][1] - p[1]) > 1e-6):
            out.append(p)
    return out


def apply_boolean(obj, cutters, op="DIFFERENCE"):
    for c in cutters:
        mod = obj.modifiers.new("cut", "BOOLEAN")
        mod.operation = op
        mod.solver = "EXACT"
        mod.material_mode = "TRANSFER"
        mod.object = c
    dg = bpy.context.evaluated_depsgraph_get()
    ev = obj.evaluated_get(dg)
    me = bpy.data.meshes.new_from_object(ev, preserve_all_data_layers=True, depsgraph=dg)
    old = obj.data
    obj.modifiers.clear()
    obj.data = me
    bpy.data.meshes.remove(old)
    for c in cutters:
        bpy.data.objects.remove(c, do_unlink=True)


def join(objs, name):
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.object.join()
    ob = bpy.context.view_layer.objects.active
    ob.name = name
    ob.data.name = name
    return ob


def planar_uvs(obj, tiles):
    """Per-face planar UVs in metres / tile size of the face's material (horizontal u, up-slope v)."""
    me = obj.data
    nl = len(me.loops)
    if me.uv_layers:
        for l in list(me.uv_layers):
            me.uv_layers.remove(l)
    uvl = me.uv_layers.new(name="UVMap")
    co = np.empty(len(me.vertices) * 3)
    me.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    lv = np.empty(nl, dtype=np.int64)
    me.loops.foreach_get("vertex_index", lv)
    pn = np.empty(len(me.polygons) * 3)
    me.polygons.foreach_get("normal", pn)
    pn = pn.reshape(-1, 3)
    ls = np.empty(len(me.polygons), dtype=np.int64)
    lt = np.empty(len(me.polygons), dtype=np.int64)
    pm = np.empty(len(me.polygons), dtype=np.int64)
    me.polygons.foreach_get("loop_start", ls)
    me.polygons.foreach_get("loop_total", lt)
    me.polygons.foreach_get("material_index", pm)
    tile = np.array([tiles.get(m.name, 1.0) if m else 1.0 for m in me.materials])[pm]
    n = np.repeat(pn, lt, axis=0)
    tl = np.repeat(tile, lt)
    z = np.array([0.0, 0.0, 1.0])
    t = np.cross(np.broadcast_to(z, n.shape), n)
    tn = np.linalg.norm(t, axis=1, keepdims=True)
    flat = (np.abs(n[:, 2]) > 0.92)[:, None] | (tn < 1e-6)
    t = np.where(flat, np.array([1.0, 0.0, 0.0]), t / np.maximum(tn, 1e-9))
    b = np.cross(n, t)
    p = co[lv]
    uv = np.stack([(p * t).sum(1), (p * b).sum(1)], 1) / tl[:, None]
    uvl.data.foreach_set("uv", uv.ravel())


# =====================================================================================================================
# Shared material set
# =====================================================================================================================

TILES = {}


def build_materials():
    global TILES
    st = tex_ashlar("tower_stone", 11, tile=2.4, course=0.6, base=(0.72, 0.70, 0.65), soot=0.08)
    br = tex_brick("tower_brick", 12, tile=1.8)
    tl = tex_tile("tower_tile", 13, tile=1.2)
    rf = tex_seam_roof("tower_roof", 14, tile=2.0)
    tb = tex_timber("tower_timber", 15, tile=1.2)
    gr = tex_granite("plaza_granite", 16, tile=1.0)
    bt = tex_timber("plaza_timber", 17, tile=1.2, board=0.6, base=(0.42, 0.29, 0.18))
    M = {}
    M["tower_stone"] = mat_tex("tower_stone", st[0], st[1], 0.82, strength=1.0)
    M["tower_brick"] = mat_tex("tower_brick", br[0], br[1], 0.86, strength=1.0)
    M["tower_tile"] = mat_tex("tower_tile", tl[0], tl[1], 0.2, mr=tl[2])
    M["tower_roof"] = mat_tex("tower_roof", rf[0], rf[1], 0.5, metal=0.6)
    M["tower_timber"] = mat_tex("tower_timber", tb[0], tb[1], 0.7)
    M["tower_iron"] = mat_flat("tower_iron", (0.07, 0.075, 0.08), 0.5, 0.0)
    M["tower_gold"] = mat_flat("tower_gold", (0.98, 0.78, 0.40), 0.28, 1.0)
    M["tower_bell"] = mat_flat("tower_bell", (0.30, 0.25, 0.17), 0.45, 1.0)
    M["tower_dark"] = mat_flat("tower_dark", (0.05, 0.055, 0.06), 0.25, 0.0)
    M["clock_face"] = mat_flat("clock_face", (0.90, 0.88, 0.80), 0.35, 0.0)
    M["tower_lamp_glass"] = mat_flat("tower_lamp_glass", (0.80, 0.76, 0.62), 0.15, 0.0)
    M["hand_iron"] = mat_flat("hand_iron", (0.05, 0.05, 0.055), 0.45, 0.0)
    M["hand_gold"] = mat_flat("hand_gold", (0.98, 0.78, 0.40), 0.28, 1.0)
    M["furn_granite"] = mat_tex("furn_granite", gr[0], gr[1], 0.7)
    M["furn_timber"] = mat_tex("furn_timber", bt[0], bt[1], 0.65)
    M["furn_steel"] = mat_flat("furn_steel", (0.60, 0.62, 0.63), 0.42, 1.0)
    M["lamp_iron"] = mat_flat("lamp_iron", (0.08, 0.10, 0.09), 0.42, 0.0)
    M["lamp_glass"] = mat_flat("lamp_glass", (0.82, 0.78, 0.66), 0.12, 0.0)
    M["step_granite"] = M["furn_granite"]
    M["step_nosing"] = mat_flat("step_nosing", (0.20, 0.20, 0.20), 0.6, 0.0)
    M["grate_iron"] = mat_flat("grate_iron", (0.14, 0.12, 0.10), 0.65, 0.4)
    TILES = {"tower_stone": 2.4, "tower_brick": 1.8, "tower_tile": 1.2, "tower_roof": 2.0, "tower_timber": 1.2,
             "furn_granite": 1.0, "furn_timber": 1.2}
    return M


# =====================================================================================================================
# The tower
# =====================================================================================================================

# Levels (metres)
Z_PLINTH = 0.6        # top of the two plinth courses = door threshold
Z_BASE_TOP = 5.4      # top of the stone base body
Z_SHAFT = 6.0         # top of the base cornice, foot of the brick shaft
Z_FRIEZE0, Z_FRIEZE1 = 19.8, 21.4
Z_STAGE0, Z_STAGE1 = 22.2, 28.6
DIAL_Z = 25.4
DIAL_R = 2.25
Z_BELFRY0, Z_BELFRY1 = 29.2, 32.6
HALF_BASE = 4.2       # base body half width
HALF_SHAFT = 3.1      # slim brick shaft, so base and clock stage both step out of it
HALF_STAGE = 4.2
HALF_BELFRY = 3.0
WALL_DIAL = HALF_STAGE  # the dial walls
DIAL_RECESS = 0.16

YAWS = {"south": 0.0, "east": 90.0, "north": 180.0, "west": -90.0}  # south = -Y = the door / boulevard side


def face_matrix(yaw_deg, depth_out, z):
    """Local frame of a wall face: local x = right as seen from outside, local y = up, local z = outward. Origin on
    the face centre line at height z, `depth_out` from the tower axis."""
    return (Matrix.Rotation(math.radians(yaw_deg), 4, "Z") @ Matrix.Translation((0, -depth_out, z))
            @ Matrix.Rotation(math.radians(90), 4, "X"))


def wall_matrix(yaw_deg):
    """World rotation for a piece modelled on the south (-Y) face in world axes."""
    return Matrix.Rotation(math.radians(yaw_deg), 4, "Z")


def build_tower(M):
    mats = list({id(m): m for m in M.values()}.values())
    objs = []
    A = Acc()  # everything not booleaned

    # --- plinth: two stepped courses and the base body -------------------------------------------------------------
    A.boxc(0, 0, 0.0, 9.0, 9.0, 0.3, "tower_stone", bevel=0.02)
    A.boxc(0, 0, 0.3, 8.7, 8.7, 0.3, "tower_stone", bevel=0.02)
    base = Acc()
    base.boxc(0, 0, Z_PLINTH - 0.01, 2 * HALF_BASE, 2 * HALF_BASE, Z_BASE_TOP - Z_PLINTH + 0.01, "tower_stone")
    base_o = base.to_object("base", mats)
    # door (south) and blind arches (other three sides)
    cutters = []
    for side, yaw in YAWS.items():
        c = Acc()
        deep = 0.75 if side == "south" else 0.22
        R = wall_matrix(yaw)
        w = 2.2 if side == "south" else 2.0
        c.prism_xz(arch_poly(w, Z_PLINTH, 3.1, 20), -HALF_BASE - 0.3, -HALF_BASE + deep, "tower_stone", M=R)
        co = c.to_object("cut_" + side, mats)
        cutters.append(co)
    apply_boolean(base_o, cutters)
    objs.append(base_o)

    for side, yaw in YAWS.items():
        R = wall_matrix(yaw)
        y_face = -HALF_BASE
        w = 2.2 if side == "south" else 2.0
        r_in = w / 2
        # voussoirs: 13 wedges and a proud keystone
        nv = 13
        for k in range(nv):
            a0 = math.pi * k / nv + 0.012
            a1 = math.pi * (k + 1) / nv - 0.012
            key = k == nv // 2
            r_out = r_in + (0.62 if key else 0.42)
            poly = [(r_in * math.cos(a0), 3.1 + r_in * math.sin(a0)), (r_out * math.cos(a0), 3.1 + r_out * math.sin(a0)),
                    (r_out * math.cos(a1), 3.1 + r_out * math.sin(a1)), (r_in * math.cos(a1), 3.1 + r_in * math.sin(a1))]
            poly = poly[::-1]
            A.prism_xz(poly, y_face - (0.12 if key else 0.06), y_face + 0.05, "tower_stone", M=R, bevel=0.01)
        # jambs: alternating long and short blocks
        for i in range(4):
            z0 = Z_PLINTH + i * 0.625
            wj = 0.42 if i % 2 == 0 else 0.30
            for sgn in (-1, 1):
                x0 = sgn * r_in
                x1 = sgn * (r_in + wj)
                A.box(min(x0, x1), y_face - 0.05, z0 + 0.008, max(x0, x1), y_face + 0.05, z0 + 0.617, "tower_stone",
                      M=R, bevel=0.012)
        if side == "south":
            # door leaves, panel frames, fanlight with radial bars
            back = -HALF_BASE + 0.75
            A.box(-r_in, back - 0.12, Z_PLINTH, r_in, back + 0.02, 3.1, "tower_timber", M=R)
            for sx in (-1, 1):
                for zz in (1.0, 2.0):
                    A.box(sx * 0.12 if sx > 0 else -0.98, back - 0.16, zz, sx * 0.98 if sx > 0 else -0.12, back - 0.12,
                          zz + 0.8, "tower_timber", M=R, bevel=0.01)
            A.box(-0.03, back - 0.17, Z_PLINTH, 0.03, back - 0.11, 3.1, "tower_iron", M=R)
            A.box(-r_in, back - 0.16, 3.04, r_in, back - 0.08, 3.14, "tower_iron", M=R)
            fan = [(r_in * math.cos(math.pi * k / 20), 3.1 + r_in * math.sin(math.pi * k / 20)) for k in range(21)]
            A.prism_xz(fan[::-1] if False else fan, back - 0.04, back + 0.02, "tower_dark", M=R)
            for k in range(1, 8):
                a = math.pi * k / 8
                c, s = math.cos(a), math.sin(a)
                poly = [(0.05 * c - 0.02 * s, 3.1 + 0.05 * s + 0.02 * c), (r_in * c - 0.02 * s, 3.1 + r_in * s + 0.02 * c),
                        (r_in * c + 0.02 * s, 3.1 + r_in * s - 0.02 * c), (0.05 * c + 0.02 * s, 3.1 + 0.05 * s - 0.02 * c)]
                A.prism_xz(poly, back - 0.09, back - 0.04, "tower_iron", M=R)
            # wall lanterns either side of the door
            for sx in (-1.9, 1.9):
                L = R @ Matrix.Translation((sx, y_face, 3.3))
                A.box(-0.04, -0.45, -0.04, 0.04, 0.0, 0.04, "tower_iron", M=L)
                A.box(-0.12, -0.05, -0.35, 0.12, 0.0, 0.25, "tower_iron", M=L)
                A.lathe([(0, -0.30), (0.12, -0.30), (0.17, 0.05), (0.0, 0.05)], 6, "tower_lamp_glass",
                        M=L @ Matrix.Translation((0, -0.45, -0.1)), smooth=False, phase=math.pi / 6)
                A.lathe([(0, 0.05), (0.21, 0.05), (0.04, 0.24), (0.0, 0.28)], 6, "tower_iron",
                        M=L @ Matrix.Translation((0, -0.45, -0.1)), smooth=False, phase=math.pi / 6)
                A.lathe([(0, -0.40), (0.08, -0.36), (0.12, -0.30), (0, -0.30)], 6, "tower_iron",
                        M=L @ Matrix.Translation((0, -0.45, -0.1)), smooth=False, phase=math.pi / 6)
        else:
            # tile infill in the blind arch
            A.prism_xz(arch_poly(w - 0.02, Z_PLINTH + 0.6, 3.1, 20), y_face + 0.18, y_face + 0.23, "tower_tile", M=R)
            A.box(-r_in - 0.1, y_face - 0.12, Z_PLINTH + 0.45, r_in + 0.1, y_face + 0.2, Z_PLINTH + 0.6,
                  "tower_stone", M=R, bevel=0.015)

    # quoins on the base corners (alternate long/short, proud 5 cm)
    for k in range(8):
        z0 = Z_PLINTH + 0.6 * k
        lng = k % 2 == 0
        for qx in (-1, 1):
            for qy in (-1, 1):
                ax, ay = (0.95, 0.55) if lng else (0.55, 0.95)
                x_out, y_out = qx * (HALF_BASE + 0.05), qy * (HALF_BASE + 0.05)
                A.box(min(x_out, x_out - qx * ax), min(y_out, y_out - qy * 0.3), z0 + 0.012,
                      max(x_out, x_out - qx * ax), max(y_out, y_out - qy * 0.3), z0 + 0.588, "tower_stone", bevel=0.015)
                # second arm starts behind the first so the two never share the corner column
                y_in = y_out - qy * 0.3
                A.box(min(x_out, x_out - qx * 0.3), min(y_in, y_out - qy * ay), z0 + 0.012,
                      max(x_out, x_out - qx * 0.3), max(y_in, y_out - qy * ay), z0 + 0.588, "tower_stone", bevel=0.015)
    # base cornice and a sloped weathering set-off up to the slim shaft
    A.boxc(0, 0, Z_BASE_TOP, 8.75, 8.75, 0.3, "tower_stone", bevel=0.03)
    A.boxc(0, 0, Z_BASE_TOP + 0.3, 9.1, 9.1, 0.3, "tower_stone", bevel=0.04)
    hb, ht = 4.4, HALF_SHAFT + 0.15
    zs0, zs1 = Z_SHAFT - 0.02, Z_SHAFT + 0.55
    sv = [(sx * hb, sy * hb, zs0) for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))] + \
         [(sx * ht, sy * ht, zs1) for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
    A.closed(sv, [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)], "tower_stone")

    # steps up to the door: four risers of 0.15, 0.33 treads
    for i in range(1, 4):
        h = 0.15 * i
        y_end = -HALF_BASE - (4 - i) * 0.33 - 0.15
        A.box(-1.6, y_end, 0.0, 1.6, -HALF_BASE + 0.1, h, "tower_stone", bevel=0.015)
    for sx in (-1, 1):
        A.box(sx * 1.6, -HALF_BASE - 1.14, 0.0, sx * 2.1, -HALF_BASE + 0.1, 0.75, "tower_stone", bevel=0.02)
        A.box(sx * 1.55, -HALF_BASE - 1.18, 0.75, sx * 2.15, -HALF_BASE + 0.1, 0.85, "tower_stone", bevel=0.02)

    # --- shaft: brick core, slit windows, stone pilasters, string courses, tile frieze ---------------------------
    sh = Acc()
    sh.boxc(0, 0, Z_SHAFT - 0.01, 2 * HALF_SHAFT, 2 * HALF_SHAFT, Z_STAGE0 - Z_SHAFT + 0.02, "tower_brick")
    shaft_o = sh.to_object("shaft", mats)
    windows = []  # (x, sill, springing, width)
    windows += [(0.0, 8.4, 10.3, 0.42)]
    windows += [(-0.55, 13.0, 15.6, 0.38), (0.55, 13.0, 15.6, 0.38)]
    windows += [(0.0, 17.6, 18.9, 0.42)]
    cutters = []
    for side, yaw in YAWS.items():
        R = wall_matrix(yaw)
        c = Acc()
        for x, z0, zs, w in windows:
            poly = [(px + x, pz) for px, pz in arch_poly(w, z0, zs, 10)]
            c.prism_xz(poly, -HALF_SHAFT - 0.3, -HALF_SHAFT + 0.34, "tower_stone", M=R)
        cutters.append(c.to_object("wcut_" + side, mats))
    apply_boolean(shaft_o, cutters)
    objs.append(shaft_o)
    for side, yaw in YAWS.items():
        R = wall_matrix(yaw)
        y = -HALF_SHAFT
        for x, z0, zs, w in windows:
            A.box(x - w / 2 - 0.02, y + 0.28, z0, x + w / 2 + 0.02, y + 0.36, zs + w / 2 + 0.02, "tower_dark", M=R)
            A.box(x - w / 2 - 0.12, y - 0.10, z0 - 0.16, x + w / 2 + 0.12, y + 0.10, z0 + 0.0, "tower_stone", M=R,
                  bevel=0.015)
            A.box(x - 0.11, y - 0.06, zs + w / 2 - 0.02, x + 0.11, y + 0.05, zs + w / 2 + 0.34, "tower_stone", M=R,
                  bevel=0.012)
        # stone panel frame around the paired windows
        A.box(-1.0, y - 0.05, 12.7, 1.0, y + 0.05, 12.86, "tower_stone", M=R, bevel=0.01)
    # pilasters at the corners: a lower block and an upper block with a weathered set-off
    for qx in (-1, 1):
        for qy in (-1, 1):
            cx, cy = qx * (HALF_SHAFT - 0.12), qy * (HALF_SHAFT - 0.12)
            A.boxc(cx, cy, Z_SHAFT, 1.3, 1.3, 11.4 - Z_SHAFT, "tower_stone", bevel=0.03)
            A.boxc(cx, cy, 11.4, 1.42, 1.42, 0.35, "tower_stone", bevel=0.03)
            A.boxc(cx, cy, 11.75, 1.12, 1.12, Z_FRIEZE0 - 11.75, "tower_stone", bevel=0.03)
            A.boxc(cx, cy, Z_FRIEZE0, 1.3, 1.3, Z_STAGE0 - Z_FRIEZE0, "tower_stone", bevel=0.03)
    # string courses
    for z in (11.45, 16.7):
        A.boxc(0, 0, z, 2 * HALF_SHAFT + 0.3, 2 * HALF_SHAFT + 0.3, 0.26, "tower_stone", bevel=0.03)
    # tile frieze with stone bands
    A.boxc(0, 0, Z_FRIEZE0 - 0.22, 2 * HALF_SHAFT + 0.34, 2 * HALF_SHAFT + 0.34, 0.22, "tower_stone", bevel=0.03)
    for side, yaw in YAWS.items():
        R = wall_matrix(yaw)
        A.box(-2.45, -HALF_SHAFT - 0.05, Z_FRIEZE0, 2.45, -HALF_SHAFT + 0.05, Z_FRIEZE1, "tower_tile", M=R)
        # three stone roundels in the frieze
        for x in (-1.45, 0.0, 1.45):
            Lm = R @ Matrix.Translation((x, -HALF_SHAFT - 0.05, (Z_FRIEZE0 + Z_FRIEZE1) / 2)) @ \
                Matrix.Rotation(math.radians(90), 4, "X")
            A.lathe([(0.0, 0.0), (0.42, 0.0), (0.46, 0.04), (0.42, 0.08), (0.0, 0.08)], 20, "tower_stone", M=Lm,
                    sharp_deg=30)
    # corbel table under the clock stage
    A.boxc(0, 0, Z_FRIEZE1, 2 * HALF_SHAFT + 0.34, 2 * HALF_SHAFT + 0.34, 0.2, "tower_stone", bevel=0.03)
    for side, yaw in YAWS.items():
        R = wall_matrix(yaw)
        # deep corbels carry the clock stage 1.1 m out over the slim shaft
        for k in range(5):
            x = -1.8 + k * 0.9
            d = HALF_STAGE - HALF_SHAFT + 0.02
            prof = [(0.0, 0.0), (0.0, 1.0), (-d, 1.0), (-d, 0.78), (-0.62, 0.5), (-0.32, 0.14), (-0.26, 0.0)]
            # bracket profile in YZ, extruded along X
            Lb = R @ Matrix.Translation((x, -HALF_SHAFT, Z_STAGE0 - 1.0)) @ Matrix.Rotation(math.radians(90), 4, "Z")
            A.prism_xz(prof[::-1], -0.17, 0.17, "tower_stone", M=Lb, bevel=0.012)
    A.boxc(0, 0, Z_STAGE0 - 0.02, 2 * HALF_STAGE + 0.2, 2 * HALF_STAGE + 0.2, 0.22, "tower_stone", bevel=0.03)

    # --- clock stage ------------------------------------------------------------------------------------------------
    cs = Acc()
    cs.boxc(0, 0, Z_STAGE0, 2 * HALF_STAGE, 2 * HALF_STAGE, Z_STAGE1 - Z_STAGE0, "tower_stone")
    stage_o = cs.to_object("stage", mats)
    cutters = []
    for side, yaw in YAWS.items():
        c = Acc()
        F = face_matrix(yaw, WALL_DIAL, DIAL_Z)
        c.lathe([(0, -DIAL_RECESS), (DIAL_R + 0.12, -DIAL_RECESS), (DIAL_R + 0.12, 0.3), (0, 0.3)], 48, "tower_stone",
                M=F, smooth=False)
        cutters.append(c.to_object("dcut_" + side, mats))
    apply_boolean(stage_o, cutters)
    objs.append(stage_o)
    sockets = []
    for side, yaw in YAWS.items():
        F = face_matrix(yaw, WALL_DIAL, DIAL_Z)
        back = -DIAL_RECESS
        # dial disc
        A.lathe([(0, back), (DIAL_R + 0.1, back), (DIAL_R + 0.1, back + 0.04), (0, back + 0.04)], 48, "clock_face",
                M=F, smooth=False)
        s = back + 0.04  # dial surface
        # chapter ring lines
        for r0, r1 in ((1.62, 1.645), (2.02, 2.04), (2.17, 2.2)):
            A.lathe([(r0, s), (r1, s), (r1, s + 0.006), (r0, s + 0.006), (r0, s)], 64, "tower_iron", M=F, smooth=False)
        # minute ticks and hour bars
        for k in range(60):
            a = -2 * math.pi * k / 60
            T = F @ Matrix.Rotation(a, 4, "Z")
            if k % 5 == 0:
                if k == 0:
                    for dx in (-0.07, 0.07):
                        A.box(dx - 0.035, 1.66, s, dx + 0.035, 2.16, s + 0.012, "tower_iron", M=T)
                else:
                    A.box(-0.045, 1.66, s, 0.045, 2.16, s + 0.012, "tower_iron", M=T)
            else:
                A.box(-0.012, 2.04, s, 0.012, 2.17, s + 0.008, "tower_iron", M=T)
        # bezel (gold) and stone surround
        A.lathe([(DIAL_R - 0.03, back + 0.02), (DIAL_R + 0.13, back + 0.02), (DIAL_R + 0.13, 0.06),
                 (DIAL_R + 0.05, 0.08), (DIAL_R - 0.03, 0.06), (DIAL_R - 0.03, back + 0.02)], 64, "tower_gold", M=F, sharp_deg=50)
        A.lathe([(DIAL_R + 0.12, -0.02), (DIAL_R + 0.6, -0.02), (DIAL_R + 0.6, 0.08), (DIAL_R + 0.5, 0.16),
                 (DIAL_R + 0.22, 0.16), (DIAL_R + 0.12, 0.1), (DIAL_R + 0.12, -0.02)], 48, "tower_stone", M=F, sharp_deg=40)
        # hub boss
        A.lathe([(0, s), (0.16, s), (0.14, s + 0.03), (0, s + 0.035)], 16, "tower_iron", M=F)
        pivot = F @ Vector((0, 0, s + 0.005))
        normal = (F.to_3x3() @ Vector((0, 0, 1))).normalized()
        sockets.append((side, pivot, normal, DIAL_R, yaw))
        # tile spandrels: the corner fields between the round surround and the square stage face
        R = wall_matrix(yaw)
        r_s, hx, hz, z_lo = DIAL_R + 0.66, 3.4, 2.6, 0.9
        a0, a1 = math.asin(z_lo / r_s), math.asin(hz / r_s)
        arc = [(r_s * math.cos(a0 + (a1 - a0) * k / 10), r_s * math.sin(a0 + (a1 - a0) * k / 10)) for k in range(11)]
        sp = arc + [(hx, hz), (hx, z_lo)]
        for sx in (-1, 1):
            for sz in (-1, 1):
                poly = [(sx * x, DIAL_Z + sz * z) for x, z in sp]
                A.prism_xz(poly, -WALL_DIAL - 0.04, -WALL_DIAL + 0.02, "tower_tile", M=R)
    # corner turrets (octagonal shafts) rising to pinnacles
    for qx in (-1, 1):
        for qy in (-1, 1):
            Tm = Matrix.Translation((qx * HALF_STAGE, qy * HALF_STAGE, 0))
            A.lathe([(0, Z_STAGE0 - 0.3), (0.62, Z_STAGE0 - 0.3), (0.62, Z_STAGE0 + 0.1), (0.52, Z_STAGE0 + 0.2),
                     (0.52, Z_STAGE1 + 1.2), (0.64, Z_STAGE1 + 1.3), (0.64, Z_STAGE1 + 1.55), (0.12, Z_STAGE1 + 3.0),
                     (0, Z_STAGE1 + 3.1)], 8, "tower_stone", M=Tm, smooth=False, phase=math.pi / 8)
            A.lathe([(0, Z_STAGE1 + 2.95), (0.1, Z_STAGE1 + 3.0), (0.12, Z_STAGE1 + 3.12), (0.0, Z_STAGE1 + 3.3)],
                    8, "tower_gold", M=Tm)
    # main cornice and parapet
    A.boxc(0, 0, Z_STAGE1, 2 * HALF_STAGE + 0.3, 2 * HALF_STAGE + 0.3, 0.25, "tower_stone", bevel=0.03)
    A.boxc(0, 0, Z_STAGE1 + 0.25, 2 * HALF_STAGE + 0.7, 2 * HALF_STAGE + 0.7, 0.35, "tower_stone", bevel=0.04)
    for side, yaw in YAWS.items():
        R = wall_matrix(yaw)
        for k in range(9):
            x = -3.4 + k * 0.85
            Lb = R @ Matrix.Translation((x, -HALF_STAGE, Z_STAGE1 - 0.45)) @ Matrix.Rotation(math.radians(90), 4, "Z")
            prof = [(0.0, 0.0), (0.0, 0.46), (-0.32, 0.46), (-0.32, 0.36), (-0.12, 0.12), (-0.1, 0.0)]
            A.prism_xz(prof[::-1], -0.1, 0.1, "tower_stone", M=Lb, bevel=0.008)
        # parapet: stone posts and galvanised rails
        yp = -HALF_STAGE - 0.2
        for k in range(7):
            x = -3.0 + k * 1.0
            A.box(x - 0.09, yp - 0.09, Z_STAGE1 + 0.6, x + 0.09, yp + 0.09, Z_STAGE1 + 1.6, "tower_iron", M=R)
        for zr in (Z_STAGE1 + 1.0, Z_STAGE1 + 1.5):
            A.box(-3.6, yp - 0.04, zr, 3.6, yp + 0.04, zr + 0.08, "tower_iron", M=R)

    # --- belfry: open arcade, hollow, a bell -------------------------------------------------------------------
    bf = Acc()
    bf.boxc(0, 0, Z_BELFRY0, 2 * HALF_BELFRY, 2 * HALF_BELFRY, Z_BELFRY1 - Z_BELFRY0, "tower_stone")
    belfry_o = bf.to_object("belfry", mats)
    cutters = []
    spring = Z_BELFRY0 + 2.3
    for yaw in (0.0, 90.0):
        R = wall_matrix(yaw)
        c = Acc()
        for x in (-1.15, 1.15):
            poly = [(px + x, pz) for px, pz in arch_poly(1.5, Z_BELFRY0 + 0.25, spring, 14)]
            c.prism_xz(poly, -HALF_BELFRY - 0.5, HALF_BELFRY + 0.5, "tower_stone", M=R)
        cutters.append(c.to_object("bcut_%d" % int(yaw), mats))
    c = Acc()
    c.boxc(0, 0, Z_BELFRY0 + 0.25, 3.9, 3.9, spring + 0.75 - Z_BELFRY0 - 0.25, "tower_stone")
    cutters.append(c.to_object("bcut_core", mats))
    apply_boolean(belfry_o, cutters)
    objs.append(belfry_o)
    for side, yaw in YAWS.items():
        R = wall_matrix(yaw)
        for x in (-1.15, 1.15):
            # sill and impost
            A.box(x - 0.85, -HALF_BELFRY - 0.08, Z_BELFRY0 + 0.1, x + 0.85, -HALF_BELFRY + 0.6, Z_BELFRY0 + 0.25,
                  "tower_stone", M=R, bevel=0.015)
            # timber louvres in the lower third so the openings still show sky above
            for k in range(3):
                zl = Z_BELFRY0 + 0.4 + k * 0.28
                A.box(x - 0.75, -HALF_BELFRY + 0.25, zl, x + 0.75, -HALF_BELFRY + 0.32, zl + 0.2, "tower_roof",
                      M=R)
        # mullion capital band
        A.box(-0.45, -HALF_BELFRY - 0.06, spring - 0.12, 0.45, -HALF_BELFRY + 0.2, spring, "tower_stone", M=R,
              bevel=0.012)
    A.boxc(0, 0, Z_BELFRY1, 2 * HALF_BELFRY + 0.5, 2 * HALF_BELFRY + 0.5, 0.35, "tower_stone", bevel=0.03)
    # bell on a beam
    A.box(-1.95, -0.12, spring + 0.55, 1.95, 0.12, spring + 0.75, "tower_timber")
    A.lathe([(0, spring - 0.75), (0.62, spring - 0.75), (0.66, spring - 0.68), (0.55, spring - 0.45),
             (0.40, spring - 0.05), (0.33, spring + 0.25), (0.2, spring + 0.38), (0.0, spring + 0.4)], 20,
            "tower_bell", sharp_deg=60)
    A.lathe([(0.0, spring + 0.35), (0.08, spring + 0.35), (0.08, spring + 0.6), (0.0, spring + 0.6)], 8, "tower_iron",
            smooth=False)

    # --- crown: flared hip roof in standing-seam zinc, gold finial -------------------------------------------------
    zr0 = Z_BELFRY1 + 0.35
    rings = [(4.35, zr0 - 0.05, 0.42), (3.85, zr0 + 0.14, 0.26), (3.2, zr0 + 0.45, 0.12), (2.45, zr0 + 0.95, 0.04),
             (1.6, zr0 + 1.7, 0.0), (0.8, zr0 + 2.45, 0.0), (0.3, zr0 + 2.95, 0.0)]
    per = 8  # verts per side per ring

    def sq_ring(hw, z, lift):
        pts = []
        for side in range(4):
            for j in range(per):
                t = -1 + 2 * j / per
                x, y = t * hw, -hw
                rot = side * math.pi / 2
                xr = x * math.cos(rot) - y * math.sin(rot)
                yr = x * math.sin(rot) + y * math.cos(rot)
                pts.append((xr, yr, z + lift * abs(t) ** 2.2))
        return pts

    verts, faces = [], []
    # eave underside ring (soffit) at the belfry wall line
    soffit = sq_ring(HALF_BELFRY + 0.25, zr0 - 0.02, 0.0)
    all_rings = [soffit] + [sq_ring(hw, z, lift) for hw, z, lift in rings]
    # fascia: drop the outer ring by 0.16 for a thickness
    fascia = [(x, y, z - 0.16) for x, y, z in all_rings[1]]
    all_rings = [soffit, fascia] + all_rings[1:]
    nr = per * 4
    for rg in all_rings:
        verts += rg
    for i in range(len(all_rings) - 1):
        a, b = i * nr, (i + 1) * nr
        for j in range(nr):
            j1 = (j + 1) % nr
            faces.append((a + j, a + j1, b + j1, b + j))
    verts.append((0, 0, zr0 - 0.02))
    c0 = len(verts) - 1
    faces += [(c0, (j + 1) % nr, j) for j in range(nr)]
    verts.append((0, 0, rings[-1][1]))
    c1 = len(verts) - 1
    top = (len(all_rings) - 1) * nr
    faces += [(top + j, top + (j + 1) % nr, c1) for j in range(nr)]
    A.closed(verts, faces, "tower_roof")
    # gold finial: drum, orb, spire; gold balls on the eave corners
    zf = rings[-1][1] - 0.1
    A.lathe([(0, zf), (0.42, zf), (0.42, zf + 0.22), (0.28, zf + 0.32), (0.15, zf + 0.44), (0.15, zf + 0.54),
             (0.0, zf + 0.54)], 16, "tower_gold", sharp_deg=40)
    orb = [(0.0, zf + 0.5)]
    for k in range(1, 14):
        a = -math.pi / 2 + math.pi * k / 14
        orb.append((0.45 * math.cos(a), zf + 0.95 + 0.45 * math.sin(a)))
    orb.append((0.0, zf + 1.4))
    A.lathe(orb, 24, "tower_gold", sharp_deg=60)
    A.lathe([(0, zf + 1.33), (0.1, zf + 1.33), (0.12, zf + 1.5), (0.065, zf + 1.62), (0.025, zf + 1.88), (0.0, zf + 2.05)],
            12, "tower_gold", sharp_deg=50)
    for qx in (-1, 1):
        for qy in (-1, 1):
            hw, z, lift = rings[0]
            Tb = Matrix.Translation((qx * hw * 0.98, qy * hw * 0.98, z + lift + 0.05))
            ball = [(0.0, -0.12)] + [(0.12 * math.cos(-math.pi / 2 + math.pi * k / 6),
                                      0.12 * math.sin(-math.pi / 2 + math.pi * k / 6)) for k in range(1, 6)] + [(0, 0.12)]
            A.lathe(ball, 10, "tower_gold", M=Tb, sharp_deg=70)

    rest = A.to_object("rest", mats)
    objs.append(rest)
    tower = join(objs, "clock_tower")
    planar_uvs(tower, TILES)
    return tower, sockets


def tower_hulls(mats):
    """Convex UCX_ boxes for the tower: plinth, steps, cheek walls, base, shaft, stage, belfry, roof."""
    hulls = []

    def hull(i, verts):
        a = Acc()
        bm = bmesh.new()
        for p in verts:
            bm.verts.new(p)
        bmesh.ops.convex_hull(bm, input=list(bm.verts))
        bm.verts.index_update()
        bm.faces.ensure_lookup_table()
        v = [tuple(x.co) for x in bm.verts]
        f = [tuple(x.index for x in fc.verts) for fc in bm.faces]
        bm.free()
        a.v, a.f = v, f
        a.m = ["tower_stone"] * len(f)
        a.s = [False] * len(f)
        o = a.to_object("UCX_clock_tower_%02d" % i, mats)
        planar_uvs(o, {})
        hulls.append(o)

    def boxv(x0, y0, z0, x1, y1, z1):
        return [(x, y, z) for x in (x0, x1) for y in (y0, y1) for z in (z0, z1)]

    hull(0, boxv(-4.55, -4.55, 0, 4.55, 4.55, Z_SHAFT + 0.6))
    for i in range(1, 4):
        h = 0.15 * i
        y_end = -HALF_BASE - (4 - i) * 0.33 - 0.15
        hull(i, boxv(-1.6, y_end, 0, 1.6, -HALF_BASE, h))
    hull(4, boxv(-2.15, -HALF_BASE - 1.18, 0, -1.6, -HALF_BASE, 0.85))
    hull(5, boxv(1.6, -HALF_BASE - 1.18, 0, 2.15, -HALF_BASE, 0.85))
    hull(6, boxv(-3.7, -3.7, Z_SHAFT, 3.7, 3.7, Z_STAGE0))
    hull(7, boxv(-4.45, -4.45, Z_STAGE0, 4.45, 4.45, Z_STAGE1 + 0.6))
    hull(8, boxv(-3.25, -3.25, Z_BELFRY0, 3.25, 3.25, Z_BELFRY1 + 0.35))
    hull(9, [(x, y, Z_BELFRY1 + 0.3) for x in (-4.4, 4.4) for y in (-4.4, 4.4)] + [(0, 0, Z_BELFRY1 + 3.4)])
    return hulls


# =====================================================================================================================
# Hands
# =====================================================================================================================

def build_hand(M, kind):
    mats = [M["hand_iron"], M["hand_gold"]]
    A = Acc()
    if kind == "hour":
        L, w, tail, y0, y1 = 1.32, 0.11, 0.42, -0.035, -0.008
        # shaft
        A.prism_xz([(-w / 2, -tail + 0.1), (w / 2, -tail + 0.1), (w * 0.35, L - 0.42), (-w * 0.35, L - 0.42)][::-1],
                   y0, y1, "hand_iron")
        # open spade: a diamond with a pierced centre suggested by a gold inlay
        A.prism_xz([(0, L - 0.5), (0.17, L - 0.25), (0, L), (-0.17, L - 0.25)][::-1], y0, y1, "hand_iron")
        A.prism_xz([(0, L - 0.4), (0.08, L - 0.26), (0, L - 0.12), (-0.08, L - 0.26)][::-1], y0 - 0.006, y0,
                   "hand_gold")
        # counterweight disc
        A.lathe([(0, -0.012), (0.15, -0.012), (0.15, 0.012), (0, 0.012)], 20, "hand_gold",
                M=Matrix.Translation((0, (y0 + y1) / 2, -tail)) @ Matrix.Rotation(math.radians(90), 4, "X"),
                smooth=False)
        # pivot collar
        A.lathe([(0, -0.014), (0.13, -0.014), (0.13, 0.014), (0, 0.014)], 20, "hand_iron",
                M=Matrix.Translation((0, (y0 + y1) / 2, 0)) @ Matrix.Rotation(math.radians(90), 4, "X"), smooth=False)
    else:
        L, w, tail, y0, y1 = 1.95, 0.075, 0.55, -0.075, -0.048
        A.prism_xz([(-w / 2, -tail + 0.1), (w / 2, -tail + 0.1), (w * 0.3, L - 0.2), (-w * 0.3, L - 0.2)][::-1],
                   y0, y1, "hand_iron")
        A.prism_xz([(0, L - 0.28), (0.07, L - 0.2), (0, L), (-0.07, L - 0.2)][::-1], y0, y1, "hand_iron")
        A.lathe([(0, -0.012), (0.12, -0.012), (0.12, 0.012), (0, 0.012)], 20, "hand_gold",
                M=Matrix.Translation((0, (y0 + y1) / 2, -tail)) @ Matrix.Rotation(math.radians(90), 4, "X"),
                smooth=False)
        A.lathe([(0, -0.014), (0.1, -0.014), (0.1, 0.014), (0, 0.014)], 20, "hand_iron",
                M=Matrix.Translation((0, (y0 + y1) / 2, 0)) @ Matrix.Rotation(math.radians(90), 4, "X"), smooth=False)
        # gold hub cap in front
        cap = [(0.0, 0.0), (0.08, 0.0), (0.075, 0.02), (0.05, 0.04), (0.0, 0.05)]
        A.lathe(cap, 16, "hand_gold", M=Matrix.Translation((0, y0, 0)) @ Matrix.Rotation(math.radians(90), 4, "X"),
                sharp_deg=60)
    o = A.to_object("clock_hand_" + kind, mats)
    planar_uvs(o, {})
    return o


# =====================================================================================================================
# Plaza furniture
# =====================================================================================================================

def ucx(name, mats, verts, i=0):
    bm = bmesh.new()
    for p in verts:
        bm.verts.new(p)
    bmesh.ops.convex_hull(bm, input=list(bm.verts))
    bm.verts.index_update()
    a = Acc()
    a.v = [tuple(x.co) for x in bm.verts]
    a.f = [tuple(x.index for x in fc.verts) for fc in bm.faces]
    bm.free()
    a.m = [mats[0].name] * len(a.f)
    a.s = [False] * len(a.f)
    o = a.to_object("UCX_%s_%02d" % (name, i), mats)
    planar_uvs(o, {})
    return o


def boxv(x0, y0, z0, x1, y1, z1):
    return [(x, y, z) for x in (x0, x1) for y in (y0, y1) for z in (z0, z1)]


def build_bench(M):
    mats = [M["furn_granite"], M["furn_timber"], M["furn_steel"]]
    A = Acc()
    for sx in (-0.62, 0.62):
        A.boxc(sx, 0, 0.0, 0.42, 0.56, 0.06, "furn_granite", bevel=0.01)  # foot course
        A.boxc(sx, 0, 0.06, 0.36, 0.5, 0.3, "furn_granite", bevel=0.012)
        A.boxc(sx, 0, 0.36, 0.42, 0.54, 0.03, "furn_granite", bevel=0.008)
        A.box(sx - 0.2, -0.24, 0.39, sx + 0.2, 0.24, 0.40, "furn_steel")  # steel bearer plate
    for k in range(5):
        y = -0.22 + k * 0.11
        A.box(-0.9, y - 0.045, 0.40, 0.9, y + 0.045, 0.455, "furn_timber", bevel=0.008)
    o = A.to_object("plaza_bench", mats)
    planar_uvs(o, TILES)
    return o, [ucx("plaza_bench", mats, boxv(-0.9, -0.28, 0, 0.9, 0.28, 0.455))]


def build_lamp(M):
    mats = [M["lamp_iron"], M["lamp_glass"]]
    A = Acc()
    # octagonal base, fluted shaft, collars
    A.lathe([(0, 0), (0.2, 0), (0.2, 0.06), (0.17, 0.1), (0.17, 0.42), (0.14, 0.5), (0.11, 0.56), (0.11, 0.62),
             (0, 0.62)], 8, "lamp_iron", smooth=False, phase=math.pi / 8)
    fl = 16
    prof_r = lambda z: 0.075 - 0.025 * (z - 0.6) / 3.0
    for k in range(fl):
        pass
    # fluted shaft: a 16-gon with alternating radii, tapered
    verts, faces = [], []
    zs = [0.6, 3.6]
    for z in zs:
        for j in range(fl * 2):
            a = 2 * math.pi * j / (fl * 2)
            r = prof_r(z) * (1.0 if j % 2 == 0 else 0.9)
            verts.append((r * math.cos(a), r * math.sin(a), z))
    n = fl * 2
    faces = [(j, (j + 1) % n, n + (j + 1) % n, n + j) for j in range(n)]
    A._push(verts, faces, "lamp_iron", True)
    for zc, rc in ((0.7, 0.1), (1.6, 0.085), (3.6, 0.075)):
        A.lathe([(0, zc - 0.04), (rc, zc - 0.04), (rc + 0.015, zc), (rc, zc + 0.04), (0, zc + 0.04)], 16, "lamp_iron",
                sharp_deg=60)
    # ladder bar
    A.box(-0.32, -0.018, 3.42, 0.32, 0.018, 3.47, "lamp_iron")
    for sx in (-0.32, 0.32):
        A.lathe([(0, -0.03), (0.03, -0.03), (0.03, 0.03), (0, 0.03)], 8, "lamp_iron",
                M=Matrix.Translation((sx, 0, 3.445)))
    # lantern: spigot, bowl, four-sided tapered glass, cap, finial
    A.lathe([(0, 3.6), (0.06, 3.6), (0.06, 3.72), (0.1, 3.75), (0, 3.75)], 12, "lamp_iron", smooth=False)
    A.lathe([(0, 3.72), (0.12, 3.75), (0.17, 3.8), (0, 3.8)], 4, "lamp_iron", smooth=False, phase=math.pi / 4)
    A.lathe([(0, 3.8), (0.17, 3.8), (0.25, 4.2), (0, 4.2)], 4, "lamp_glass", smooth=False, phase=math.pi / 4)
    # corner bars
    for k in range(4):
        a = math.pi / 4 + k * math.pi / 2
        p0 = Vector((0.17 * math.cos(a), 0.17 * math.sin(a), 3.8))
        p1 = Vector((0.25 * math.cos(a), 0.25 * math.sin(a), 4.2))
        d = (p1 - p0)
        Mb = Matrix.Translation(p0) @ d.to_track_quat("Z", "Y").to_matrix().to_4x4()
        A.lathe([(0, 0), (0.016, 0), (0.016, d.length), (0, d.length)], 4, "lamp_iron", M=Mb, smooth=False)
    A.lathe([(0, 4.2), (0.31, 4.2), (0.31, 4.24), (0.12, 4.38), (0.05, 4.4), (0, 4.4)], 4, "lamp_iron",
            smooth=False, phase=math.pi / 4)
    A.lathe([(0, 4.38), (0.04, 4.38), (0.05, 4.43), (0.012, 4.48), (0, 4.5)], 8, "lamp_iron", sharp_deg=60)
    o = A.to_object("plaza_lamp", mats)
    planar_uvs(o, {})
    hulls = [ucx("plaza_lamp", mats, boxv(-0.2, -0.2, 0, 0.2, 0.2, 0.62), 0),
             ucx("plaza_lamp", mats, boxv(-0.08, -0.08, 0.62, 0.08, 0.08, 3.75), 1)]
    return o, hulls


def build_steps(M):
    mats = [M["step_granite"], M["step_nosing"]]
    A = Acc()
    depth = 0.4
    for i in range(3):
        h0, h1 = 0.15 * i, 0.15 * (i + 1)
        y0 = -0.6 + i * depth
        A.box(-1.5 + 0.004, y0, h0 if i else 0.0, 1.5 - 0.004, 0.6, h1, "furn_granite", bevel=0.012)
        A.box(-1.49, y0 - 0.002, h1 - 0.012, 1.49, y0 + 0.05, h1 + 0.002, "step_nosing")
    o = A.to_object("plaza_steps", mats)
    planar_uvs(o, TILES)
    hulls = [ucx("plaza_steps", mats, boxv(-1.5, -0.6 + i * depth, 0, 1.5, 0.6, 0.15 * (i + 1)), i) for i in range(3)]
    return o, hulls


def build_bollard(M):
    mats = [M["lamp_iron"], M["furn_steel"]]
    A = Acc()
    A.lathe([(0, 0), (0.15, 0), (0.15, 0.03), (0.12, 0.06), (0.1, 0.1), (0.1, 0.72), (0.112, 0.74), (0.112, 0.78),
             (0.1, 0.8), (0.095, 0.84), (0.07, 0.89), (0.035, 0.91), (0, 0.915)], 16, "lamp_iron", sharp_deg=40)
    A.lathe([(0.101, 0.6), (0.104, 0.6), (0.104, 0.66), (0.101, 0.66), (0.101, 0.6)], 16, "furn_steel", smooth=True, sharp_deg=80)
    o = A.to_object("plaza_bollard", mats)
    planar_uvs(o, {})
    pts = [(0.15 * math.cos(2 * math.pi * k / 8), 0.15 * math.sin(2 * math.pi * k / 8), z) for k in range(8)
           for z in (0.0, 0.915)]
    return o, [ucx("plaza_bollard", mats, pts)]


def build_tree_grate(M):
    mats = [M["grate_iron"]]
    A = Acc()
    t = 0.03
    half, r_hole = 0.75, 0.25
    # frame
    for k in range(4):
        R = Matrix.Rotation(k * math.pi / 2, 4, "Z")
        A.box(-half, -half, 0, half - 0.07, -half + 0.07, t, "grate_iron", M=R)
    # concentric rings
    for r0, r1 in ((r_hole, r_hole + 0.05), (0.42, 0.45), (0.58, 0.61)):
        A.lathe([(r0, 0), (r1, 0), (r1, t), (r0, t), (r0, 0)], 48, "grate_iron", smooth=False)
    # radial bars, clipped to the frame
    for k in range(32):
        a = 2 * math.pi * k / 32 + math.pi / 32
        c, s = math.cos(a), math.sin(a)
        rmax = (half - 0.06) / max(abs(c), abs(s))
        r0 = r_hole + 0.04
        Mr = Matrix.Rotation(a, 4, "Z")
        A.box(r0, -0.012, 0.004, rmax, 0.012, t - 0.003, "grate_iron", M=Mr)
    o = A.to_object("plaza_tree_grate", mats)
    planar_uvs(o, {})
    hulls = []
    # four slabs around the hole, so the trunk can pass
    hulls.append(ucx("plaza_tree_grate", mats, boxv(-half, -half, 0, half, -r_hole, t), 0))
    hulls.append(ucx("plaza_tree_grate", mats, boxv(-half, r_hole, 0, half, half, t), 1))
    hulls.append(ucx("plaza_tree_grate", mats, boxv(-half, -r_hole, 0, -r_hole, r_hole, t), 2))
    hulls.append(ucx("plaza_tree_grate", mats, boxv(r_hole, -r_hole, 0, half, r_hole, t), 3))
    return o, hulls


# =====================================================================================================================
# Export and checks
# =====================================================================================================================

def export(objs, path):
    bpy.context.view_layer.update()  # objects made through bpy.data are not exported until the layer updates
    for o in bpy.context.view_layer.objects:
        o.select_set(o in objs)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True, export_apply=True,
                              export_yup=True, export_extras=False, export_tangents=False,
                              export_vertex_color="NONE", export_cameras=False, export_lights=False,
                              export_animations=False, export_materials="EXPORT", export_image_format="AUTO",
                              export_skins=False, export_morph=False)


def _img_size(data):
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        w, h = struct.unpack(">II", data[16:24])
        return "PNG", w, h
    if data[:2] == b"\xff\xd8":
        i = 2
        while i < len(data):
            if data[i] != 0xFF:
                i += 1
                continue
            mk = data[i + 1]
            ln = struct.unpack(">H", data[i + 2:i + 4])[0]
            if mk in (0xC0, 0xC1, 0xC2):
                h, w = struct.unpack(">HH", data[i + 5:i + 9])
                return "JPEG", w, h
            i += 2 + ln
    return "?", 0, 0


def check_glb(path, max_tex=1024):
    with open(path, "rb") as f:
        data = f.read()
    magic, _v, _l = struct.unpack_from("<4sII", data, 0)
    assert magic == b"glTF"
    clen, _ = struct.unpack_from("<I4s", data, 12)
    js = json.loads(data[20:20 + clen].decode("utf-8"))
    bin_off = 20 + clen + 8
    errs = []
    tris, ucx_n = 0, 0
    lo, hi = [1e9] * 3, [-1e9] * 3
    for node in js.get("nodes", []):
        if "mesh" not in node:
            continue
        for k in ("translation", "rotation", "scale"):
            if k in node and node[k] not in ([0, 0, 0], [0, 0, 0, 1], [1, 1, 1]):
                errs.append("node %s has a %s" % (node.get("name"), k))
        mesh = js["meshes"][node["mesh"]]
        is_ucx = node.get("name", "").startswith("UCX_")
        ucx_n += is_ucx
        for prim in mesh["primitives"]:
            at = prim["attributes"]
            if any(k.startswith("COLOR_") for k in at):
                errs.append("vertex colour on " + node.get("name", ""))
            if any(k.startswith("TEXCOORD_") and k != "TEXCOORD_0" for k in at):
                errs.append("more than one UV set on " + node.get("name", ""))
            if is_ucx:
                continue
            tris += js["accessors"][prim["indices"]]["count"] // 3
            pa = js["accessors"][at["POSITION"]]
            lo = [min(a, b) for a, b in zip(lo, pa["min"])]
            hi = [max(a, b) for a, b in zip(hi, pa["max"])]
    imgs = []
    for im in js.get("images", []):
        if "bufferView" not in im:
            errs.append("image %s not embedded" % im.get("name"))
            continue
        bv = js["bufferViews"][im["bufferView"]]
        raw = data[bin_off + bv.get("byteOffset", 0): bin_off + bv.get("byteOffset", 0) + bv["byteLength"]]
        fmt, w, h = _img_size(raw)
        imgs.append((im.get("name"), fmt, w, h))
        if fmt == "?" or w > max_tex or h > max_tex:
            errs.append("image %s: %s %dx%d" % (im.get("name"), fmt, w, h))
    # glTF is Y up: (x, z, -y) -> Blender (x, -z_gltf, y_gltf)
    size = (hi[0] - lo[0], hi[2] - lo[2], hi[1] - lo[1])
    blo = (lo[0], -hi[2], lo[1])
    bhi = (hi[0], -lo[2], hi[1])
    mats = [m["name"] for m in js.get("materials", [])]
    return {"file": os.path.basename(path), "bytes": len(data), "triangles": tris, "ucx_hulls": ucx_n,
            "size_xyz_m": [round(s, 3) for s in size], "min_xyz_m": [round(s, 3) for s in blo],
            "max_xyz_m": [round(s, 3) for s in bhi], "materials": mats,
            "images": ["%s %s %dx%d" % i for i in imgs], "errors": errs}


# =====================================================================================================================
# Main build
# =====================================================================================================================

def fresh():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.context.scene.unit_settings.system = "METRIC"


def build_all(out):
    os.makedirs(out, exist_ok=True)
    stats = {}

    fresh()
    M = build_materials()
    mats = list({id(m): m for m in M.values()}.values())
    tower, sockets = build_tower(M)
    # keep only the slots the tower uses, in a stable order
    used = sorted({tower.data.materials[p.material_index].name for p in tower.data.polygons})
    hulls = tower_hulls(mats)
    export([tower] + hulls, os.path.join(out, "clock_tower.glb"))
    stats["clock_tower"] = check_glb(os.path.join(out, "clock_tower.glb"))

    with open(os.path.join(out, "clock_sockets.txt"), "w", newline="\n") as f:
        f.write("# Clock dial sockets, tower-local, metres. Blender frame: right-handed, +Z up, the door faces -Y.\n")
        f.write("# Unreal (as the glTF/Interchange import lands it): (x, -y, z) * 100 cm.\n")
        f.write("# Hands: place the hand glb at the pivot, yawed about +Z by yaw_deg (its -Y face normal then points\n")
        f.write("# along the dial normal). Time angle t (clockwise from 12 as seen from outside) = rotate by +t about\n")
        f.write("# the hand's own +Y axis (= -t about the outward normal) in Blender; check the sign once in Unreal,\n")
        f.write("# whose frame is left-handed. Hour hand sits 8-35 mm, minute hand 48-75 mm (hub cap to 125 mm) in\n")
        f.write("# front of the pivot plane, which is 5 mm proud of the dial.\n")
        f.write("# face  pivot_x pivot_y pivot_z  normal_x normal_y normal_z  dial_radius  yaw_deg\n")
        for side, p, n, r, yaw in sockets:
            f.write("%-6s %8.4f %8.4f %8.4f  %7.4f %7.4f %7.4f  %6.3f  %7.1f\n" %
                    (side, p.x, p.y, p.z, n.x, n.y, n.z, r, yaw))
    stats["sockets"] = [{"face": s, "pivot": [round(c, 4) for c in p], "normal": [round(c, 4) for c in n],
                         "radius": r, "yaw_deg": yaw} for s, p, n, r, yaw in sockets]
    stats["tower_slots_used"] = used

    for kind in ("hour", "minute"):
        fresh()
        M = build_materials_hands()
        o = build_hand(M, kind)
        p = os.path.join(out, "clock_hand_%s.glb" % kind)
        export([o], p)
        stats["clock_hand_" + kind] = check_glb(p)

    for name, fn in (("plaza_bench", build_bench), ("plaza_lamp", build_lamp), ("plaza_steps", build_steps),
                     ("plaza_bollard", build_bollard), ("plaza_tree_grate", build_tree_grate)):
        fresh()
        M = build_materials_furniture()
        o, hulls = fn(M)
        p = os.path.join(out, name + ".glb")
        export([o] + hulls, p)
        stats[name] = check_glb(p)

    with open(os.path.join(out, "clock_stats.json"), "w", newline="\n") as f:
        json.dump(stats, f, indent=1)
    bad = {k: v["errors"] for k, v in stats.items() if isinstance(v, dict) and v.get("errors")}
    tw = stats["clock_tower"]
    h = tw["size_xyz_m"][2]
    if not 34.0 <= h <= 38.0:
        bad["clock_tower_height"] = h
    if tw["triangles"] > 60000:
        bad["clock_tower_triangles"] = tw["triangles"]
    write_readme(out, stats)
    for k, v in stats.items():
        if isinstance(v, dict) and "triangles" in v:
            print("clock_tower: %-20s %7d tris  %s m  %d hulls  %d bytes" % (k, v["triangles"], v["size_xyz_m"],
                                                                           v["ucx_hulls"], v["bytes"]))
    if bad:
        print("clock_tower: CHECK FAILED", bad)
        sys.exit(1)
    print("clock_tower: all checks passed")


def build_materials_hands():
    M = {}
    M["hand_iron"] = mat_flat("hand_iron", (0.05, 0.05, 0.055), 0.45, 0.0)
    M["hand_gold"] = mat_flat("hand_gold", (0.98, 0.78, 0.40), 0.28, 1.0)
    return M


def build_materials_furniture():
    global TILES
    gr = tex_granite("plaza_granite", 16, tile=1.0)
    bt = tex_timber("plaza_timber", 17, tile=1.2, board=0.6, base=(0.42, 0.29, 0.18))
    M = {}
    M["furn_granite"] = mat_tex("furn_granite", gr[0], gr[1], 0.7)
    M["furn_timber"] = mat_tex("furn_timber", bt[0], bt[1], 0.65)
    M["furn_steel"] = mat_flat("furn_steel", (0.60, 0.62, 0.63), 0.42, 1.0)
    M["lamp_iron"] = mat_flat("lamp_iron", (0.08, 0.10, 0.09), 0.42, 0.0)
    M["lamp_glass"] = mat_flat("lamp_glass", (0.82, 0.78, 0.66), 0.12, 0.0)
    M["step_granite"] = M["furn_granite"]
    M["step_nosing"] = mat_flat("step_nosing", (0.20, 0.20, 0.20), 0.6, 0.0)
    M["grate_iron"] = mat_flat("grate_iron", (0.14, 0.12, 0.10), 0.65, 0.4)
    TILES = {"furn_granite": 1.0, "furn_timber": 1.2}
    return M


def write_readme(out, st):
    def row(k):
        v = st[k]
        sx, sy, sz = v["size_xyz_m"]
        return "| %s | %.2f x %.2f x %.2f | %d | %d | %s | %.1f MB |" % (
            v["file"], sx, sy, sz, v["triangles"], v["ucx_hulls"], ", ".join(v["materials"]), v["bytes"] / 1e6)

    lines = [
        "# Clock tower and plaza furniture",
        "",
        "Made by `tools/blender/make_clock_tower.py` (headless Blender, all textures generated in the script).",
        "Rebuild: `blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_clock_tower.py -- "
        "<this folder> --previews`.",
        "",
        "All files: glb, metres, origin at ground (hands: at the pivot), Blender +Z up, the tower's door faces -Y",
        "(Unreal: a Blender point (x, y, z) lands at (x, -y, z) * 100 cm). One UV set, no vertex colour, textures",
        "1024 px or less (base colour JPEG, normal and metal-rough PNG). UVs are planar per face in metres divided",
        "by the material's tile size, so the tileable maps repeat at a fixed texel density (stone 2.4 m per tile,",
        "brick 1.8 m, tile 1.2 m, roof 2.0 m, timber 1.2 m, granite 1.0 m).",
        "",
        "| file | size x, y, z (m) | triangles | UCX hulls | material slots | file size |",
        "|---|---|---|---|---|---|",
    ]
    for k in ("clock_tower", "clock_hand_hour", "clock_hand_minute", "plaza_bench", "plaza_lamp", "plaza_steps",
              "plaza_bollard", "plaza_tree_grate"):
        lines.append(row(k))
    lines += [
        "",
        "Emissive-ready slots (exported with no emission; code drives them at night): `clock_face` (the four dial",
        "discs only; ring lines, ticks and hands are in other slots so they read dark against a lit dial),",
        "`tower_lamp_glass` (two wall lanterns by the door) and `lamp_glass` (the lamp standard's lantern).",
        "Gold accent slots: `tower_gold` (dial bezels, finial, eave balls, turret tips) and `hand_gold`.",
        "The hands have no UCX hull: turn their collision off (the importer would add a box).",
        "",
        "## Clock sockets",
        "",
        "`clock_sockets.txt` lists each dial's pivot (tower-local metres), outward normal, dial radius (%.2f m;"
        % DIAL_R,
        "face %.1f m across with the gold bezel) and the yaw that turns a hand onto that face." % (2 * (DIAL_R + 0.13)),
        "Dial centre height %.2f m. The hands point at 12 when unrotated; rotate clockwise as seen from outside." % DIAL_Z,
        "",
        "## Suggested plaza layout (tower at the centre of a 30 x 30 m plaza, boulevard arriving from -Y)",
        "",
        "- Dais: a 15 x 15 m granite dais 0.45 m high under the tower, its edge built from `plaza_steps` (3 m runs",
        "  of three 0.15 m risers): five runs (15 m) across the boulevard side, three runs centred on each other",
        "  side, plain plinth faces at the corners. The tower's own four door steps then rise from the dais.",
        "- Paving: two tones of granite. Pale banded courses running across the boulevard axis on the approach, then",
        "  a radial fan on the dais: 16 dark granite rays from the tower base out to the dais edge, pale between,",
        "  with a dark 0.6 m band ringing the dais edge. The rays point the eye up the tower from every side.",
        "- Lamp standards: one at each end of the boulevard-side flight (x = +-8.5, y = -8.5) and at each dais",
        "  corner, then every 7.5 m round the plaza edge (16 in all), 0.6 m in from the kerb.",
        "- Benches: in pairs facing the tower on the diagonals between the dais corners and the plaza corners",
        "  (about 11 m from the centre), and backing onto the tree grates on the side edges.",
        "- Trees: four in `plaza_tree_grate` at (+-11.5, +-11.5), continuing the boulevard's two rows of street",
        "  trees so the avenue closes on the tower.",
        "- Bollards: a row at 1.5 m spacing along the boulevard-side kerb (y = -15), stopping cars short of the",
        "  steps, with gaps for the crossings.",
        "",
        "Previews are in `previews/` (Cycles, grey daylight).",
    ]
    with open(os.path.join(out, "README.md"), "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


# =====================================================================================================================
# Previews
# =====================================================================================================================

def import_glb(path, drop_ucx=True):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    keep = []
    for o in new:
        if drop_ucx and o.name.startswith("UCX_"):
            bpy.data.objects.remove(o, do_unlink=True)
        else:
            keep.append(o)
    return [o for o in keep if o.type == "MESH"]


def place(objs, M):
    for o in objs:
        o.matrix_world = M @ o.matrix_world


def setup_render(scene, w, h, samples=48):
    try:
        scene.render.engine = "CYCLES"
        scene.cycles.device = "CPU"
        scene.cycles.samples = samples
        scene.cycles.use_denoising = True
        scene.cycles.max_bounces = 4
    except Exception:
        scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x, scene.render.resolution_y = w, h
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    try:
        scene.view_settings.view_transform = "AgX"
        scene.view_settings.look = "AgX - Medium High Contrast"
    except TypeError:
        pass


def sky_and_sun(scene, sky=(0.55, 0.60, 0.66), strength=0.9, sun=3.2, elev=38, azim=-35):
    world = bpy.data.worlds.new("Sky")
    world.use_nodes = True
    bg = world.node_tree.nodes.get("Background")
    bg.inputs["Color"].default_value = (*sky, 1)
    bg.inputs["Strength"].default_value = strength
    scene.world = world
    sd = bpy.data.lights.new("Sun", "SUN")
    sd.energy = sun
    sd.angle = math.radians(1.2)
    sd.color = (1.0, 0.97, 0.93)
    so = bpy.data.objects.new("Sun", sd)
    so.rotation_euler = (math.radians(90 - elev), 0, math.radians(azim))
    scene.collection.objects.link(so)


def plain_mat(name, srgb, rough=0.8, metal=0.0):
    return mat_flat(name, srgb, rough, metal)


def add_box_obj(name, x0, y0, z0, x1, y1, z1, mat):
    a = Acc()
    a.box(x0, y0, z0, x1, y1, z1, mat.name)
    return a.to_object(name, [mat])


def camera(scene, loc, target, lens=35):
    cd = bpy.data.cameras.new("Cam")
    cd.lens = lens
    cd.clip_end = 3000
    co = bpy.data.objects.new("Cam", cd)
    scene.collection.objects.link(co)
    co.location = Vector(loc)
    co.rotation_euler = (Vector(target) - Vector(loc)).to_track_quat("-Z", "Y").to_euler()
    scene.camera = co
    return co


def render(scene, path):
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    print("clock_tower: wrote", path)


def hand_matrix(pivot, yaw, t_deg):
    return (Matrix.Translation(pivot) @ Matrix.Rotation(math.radians(yaw), 4, "Z")
            @ Matrix.Rotation(math.radians(t_deg), 4, "Y"))


def previews(out, only=None):
    pv = os.path.join(out, "previews")
    os.makedirs(pv, exist_ok=True)
    with open(os.path.join(out, "clock_stats.json")) as f:
        st = json.load(f)
    socks = st["sockets"]

    # --- each asset on its own, three sides ------------------------------------------------------------------------
    assets = ["clock_tower", "clock_hand_hour", "clock_hand_minute", "plaza_bench", "plaza_lamp", "plaza_steps",
              "plaza_bollard", "plaza_tree_grate"]
    if only is None or "assets" in only:
        for name in assets:
            fresh()
            scene = bpy.context.scene
            setup_render(scene, 640, 800 if name in ("clock_tower", "plaza_lamp") else 640, samples=32)
            sky_and_sun(scene, sky=(0.75, 0.77, 0.8), strength=1.0)
            objs = import_glb(os.path.join(out, name + ".glb"))
            lo, hi = Vector((1e9,) * 3), Vector((-1e9,) * 3)
            for o in objs:
                for c in o.bound_box:
                    p = o.matrix_world @ Vector(c)
                    lo, hi = Vector(map(min, lo, p)), Vector(map(max, hi, p))
            ctr = (lo + hi) / 2
            rad = (hi - lo).length / 2
            g = plain_mat("ground", (0.5, 0.5, 0.5))
            if not name.startswith("clock_hand"):
                add_box_obj("ground", -200, -200, -0.1, 200, 200, 0.0, g)
            for view, deg in (("front", 0), ("threequarter", 35), ("back", 200)):
                a = math.radians(deg)
                lens = 50
                dist = rad / math.tan(math.atan(18 / lens)) * 1.08
                el = rad * 0.15
                loc = ctr + Vector((math.sin(a) * dist, -math.cos(a) * dist, el))
                camera(scene, loc, ctr, lens)
                render(scene, os.path.join(pv, "%s_%s.png" % (name, view)))

    # --- the tower in context ------------------------------------------------------------------------------------
    if only is None or "context" in only:
        fresh()
        scene = bpy.context.scene
        setup_render(scene, 1280, 720, samples=48)
        sky_and_sun(scene)
        dais_h = 0.45
        tower = import_glb(os.path.join(out, "clock_tower.glb"))
        place(tower, Matrix.Translation((0, 0, dais_h)))
        hh = import_glb(os.path.join(out, "clock_hand_hour.glb"))[0]
        mh = import_glb(os.path.join(out, "clock_hand_minute.glb"))[0]
        for i, s in enumerate(socks):
            pvt = Vector(s["pivot"]) + Vector((0, 0, dais_h))
            for src, t in ((hh, 10 * 30 + 10 * 0.5), (mh, 10 * 6)):
                o = src if i == 0 else src.copy()
                if i:
                    scene.collection.objects.link(o)
                o.matrix_world = hand_matrix(pvt, s["yaw_deg"], t)
        mats = {
            "pave_pale": plain_mat("pave_pale", (0.62, 0.61, 0.58)),
            "pave_dark": plain_mat("pave_dark", (0.36, 0.36, 0.35)),
            "road": plain_mat("road", (0.22, 0.22, 0.23), 0.9),
            "block": plain_mat("block", (0.55, 0.55, 0.54)),
            "block2": plain_mat("block2", (0.46, 0.45, 0.43)),
            "sea": plain_mat("sea", (0.30, 0.38, 0.42), 0.15),
            "leaf": plain_mat("leaf", (0.22, 0.30, 0.18), 0.9),
            "trunk": plain_mat("trunk", (0.25, 0.2, 0.16), 0.9),
        }
        # ground: plaza and boulevard
        add_box_obj("plaza", -15, -15, -0.2, 15, 15, 0.0, mats["pave_pale"])
        add_box_obj("dais", -7.5, -7.5, 0.0, 7.5, 7.5, dais_h, mats["pave_pale"])
        a = Acc()
        for k in range(16):  # radial rays on the dais
            ang = 2 * math.pi * k / 16
            Mr = Matrix.Rotation(ang, 4, "Z")
            a.box(4.8, -0.25, dais_h, 7.4, 0.25, dais_h + 0.004, "pave_dark", M=Mr)
        a.to_object("rays", [mats["pave_dark"]])
        add_box_obj("boulevard", -14, -400, -0.25, 14, -15, -0.05, mats["road"])
        add_box_obj("walkL", -24, -400, -0.2, -14, -15, 0.0, mats["pave_pale"])
        add_box_obj("walkR", 14, -400, -0.2, 24, -15, 0.0, mats["pave_pale"])
        add_box_obj("plazaside", -60, -15, -0.2, 60, 20, 0.0, mats["pave_pale"])
        add_box_obj("sea", -2000, 20, -1.5, 2000, 3000, -1.2, mats["sea"])
        rng = np.random.default_rng(3)
        y = -20.0
        while y > -400:
            wdt = rng.uniform(18, 32)
            for sx in (-1, 1):
                hgt = rng.uniform(15, 25)
                add_box_obj("blk", sx * 24, y - wdt, 0, sx * 50, y - 0.5, hgt,
                            mats["block"] if rng.uniform() > 0.5 else mats["block2"])
            y -= wdt
        for sx in (-1, 1):  # side blocks framing the plaza
            add_box_obj("blk", sx * 26, -15, 0, sx * 60, 14, rng.uniform(16, 22), mats["block2"])
        tree = Acc()
        for sx in (-15.5, 15.5):
            yy = -18.0
            while yy > -400:
                tree.lathe([(0, 0), (0.18, 0), (0.15, 3.5), (0, 3.5)], 6, "trunk", M=Matrix.Translation((sx, yy, 0)),
                           smooth=False)
                tree.lathe([(0, 2.8)] + [(2.6 * math.sin(math.pi * k / 6), 5.6 - 2.8 * math.cos(math.pi * k / 6))
                                         for k in range(1, 6)] + [(0, 8.4)], 8, "leaf",
                           M=Matrix.Translation((sx, yy, 0)))
                yy -= 9.0
        for qx in (-11.5, 11.5):
            for qy in (-11.5, 11.5):
                tree.lathe([(0, 0), (0.2, 0), (0.17, 3.5), (0, 3.5)], 6, "trunk", M=Matrix.Translation((qx, qy, 0)),
                           smooth=False)
                tree.lathe([(0, 2.8)] + [(3.0 * math.sin(math.pi * k / 6), 6.0 - 3.2 * math.cos(math.pi * k / 6))
                                         for k in range(1, 6)] + [(0, 9.2)], 8, "leaf",
                           M=Matrix.Translation((qx, qy, 0)))
        tree.to_object("trees", [mats["trunk"], mats["leaf"]])
        # furniture
        def put(name, M):
            objs = import_glb(os.path.join(out, name + ".glb"))
            place(objs, M)
        for k in range(5):
            put("plaza_steps", Matrix.Translation((-6 + 3 * k, -8.1, 0)) @ Matrix.Rotation(0, 4, "Z"))
        for qx in (-11.5, 11.5):
            for qy in (-11.5, 11.5):
                put("plaza_tree_grate", Matrix.Translation((qx, qy, 0.001)))
        for x in (-8.5, 8.5):
            put("plaza_lamp", Matrix.Translation((x, -8.6, 0)))
        for x in (-7.5, 7.5):
            for yy in (7.5,):
                put("plaza_lamp", Matrix.Translation((x * 0.93, yy * 0.93, dais_h)))
        for sx in (-1, 1):
            for yy in (-3.0, 3.0):
                put("plaza_lamp", Matrix.Translation((sx * 14.4, yy * 3, 0)))
        for sx in (-1, 1):
            put("plaza_bench", Matrix.Translation((sx * 10.5, -11.0, 0)) @ Matrix.Rotation(math.radians(sx * 45), 4, "Z"))
            put("plaza_bench", Matrix.Translation((sx * 13.0, 0, 0)) @ Matrix.Rotation(math.radians(90), 4, "Z"))
        for k in range(-8, 9):
            if abs(k) < 2:
                continue
            put("plaza_bollard", Matrix.Translation((k * 1.5, -15.3, 0)))

        tz = 18.0
        shots = [("ctx_150m", (2.0, -150, 1.7), (0, 0, tz), 35),
                 ("ctx_60m", (-4.0, -60, 1.7), (0, 0, 16), 28),
                 ("ctx_8m_door", (1.5, -12.5, 1.7), (0, -4.2, 3.0), 24),
                 ("ctx_3m_base", (-6.8, -7.8, 1.7), (-4.3, -4.4, 1.6), 35),
                 ("ctx_dial_close", (-2.0, -30, 22.0), (0, -4.2, 25.6), 85),
                 ("ctx_overview", (40, -60, 45), (0, 0, 10), 35)]
        for name, loc, tgt, lens in shots:
            if only and "context" in only and len(only) > 1 and name not in only:
                continue
            camera(scene, loc, tgt, lens)
            render(scene, os.path.join(pv, name + ".png"))


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not argv:
        raise SystemExit("usage: make_clock_tower.py -- <out_dir> [--previews] [--previews-only [assets|context ...]]")
    out = argv[0]
    if "--previews-only" in argv:
        rest = argv[argv.index("--previews-only") + 1:]
        previews(out, set(rest) if rest else None)
        return
    build_all(out)
    if "--previews" in argv:
        previews(out)


main()
