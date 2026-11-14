"""Modular building kit for a contemporary East-Asian coastal city: the first set of facade, roof and street
pieces, modelled by script, plus a test street assembled from them.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_building_kit.py -- <out_dir>
            [--previews] [--only ID,ID,...]

Conventions (every piece):
  * metres, Z up, the facade faces -Y. Relief (sills, slabs, signs) sticks out towards -Y; the wall body runs from
    y = 0 (the wall face) to y = +0.25.
  * origin at the bottom-left of the piece on the wall face: x = 0 is the left edge seen from the street, z = 0 the
    floor line of the storey (or the top of the wall for roof pieces). Bays are 0.9 / 1.8 / 2.7 / 4.5 m wide, a storey
    is 3.0 m, a shop storey 3.6 m, so bays stack (snap T) and tile (snap R) with no gap.
  * material slots are named jpk_<slot>[_<default surface>]: wall, trim, timber, metal, shutter, glass, rooftile,
    cloth, accent, sign, plant. Base colours are neutral (grey value) so the module master can tint them per instance.
    The metal-rough map carries the dirt / ageing mask in R (G roughness, B metal); vertex colour is not allowed.
  * every piece a player can touch has UCX_ hulls; the rest have one coarse box hull.
  * sign faces show an invented script of abstract glyphs (no language, no names).
Writes <out_dir>/<ID>.glb, textures/*.png|jpg, pieces.csv (id, group, size, triangles, slots, snap points) and with
--previews previews/piece_*.png and previews/street_*.png (a two-sided test street built from the pieces).
"""
import csv
import math
import os
import random
import shutil
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector



def _load_helpers():
    """The clock tower script's texture, mesh, export and render helpers, loaded without running its main()."""
    import types
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_clock_tower.py")
    with open(path, encoding="utf-8") as f:
        src = f.read()
    src = src[:src.rindex("\nmain()")]
    mod = types.ModuleType("make_clock_tower")
    mod.__file__ = path
    exec(compile(src, path, "exec"), mod.__dict__)
    return mod


ct = _load_helpers()

TEX = ct.TEX
pnoise, fbm, smooth01, nfh, save = ct.pnoise, ct.fbm, ct.smooth01, ct.normal_from_height, ct.save_image
Acc = ct.Acc

# =====================================================================================================================
# Textures: neutral, tileable, metres per tile in TILES
# =====================================================================================================================


def _mr(dirt, rough, metal=0.0):
    n = dirt.shape[0]
    return np.dstack([np.clip(dirt, 0, 1), np.clip(rough, 0.02, 1), np.full((n, n), metal)])


def _streaks(rng, strength=1.0):
    """Tileable vertical rain streaks in [0, 1]: long in v, narrow in u."""
    s = pnoise(rng, 3, aniso=(1.0, 0.04)) * 0.6 + pnoise(rng, 12, aniso=(1.0, 0.08)) * 0.4
    return smooth01(s * 0.45 * strength + 0.15)


def tex_smalltile(seed, tile=1.0):
    """45 x 95 mm facade tile (laid wide), running bond, 5 mm grout."""
    rng = np.random.default_rng(seed)
    u, v = ct._uv(tile)
    tw, th, g = 0.1, 0.05, 0.0045
    row = np.floor(v / th).astype(int)
    uu = u + np.where(row % 2 == 1, tw / 2, 0.0)
    col = np.floor(uu / tw).astype(int)
    lu, lv = uu % tw, v % th
    d = np.minimum(np.minimum(lu, tw - lu), np.minimum(lv, th - lv))
    key = (row % 20) * 13 + (col % 13)
    t = rng.uniform(-1, 1, 20 * 13 + 13)[key]
    t2 = rng.uniform(0, 1, 20 * 13 + 13)[key]
    fine = pnoise(rng, 1.0)
    st = _streaks(rng)
    base = 0.78 * (1 + 0.07 * t + 0.03 * fine) * np.where(t2 > 0.9, 0.9, 1.0)
    grout = d < g / 2
    val = np.where(grout, 0.52 + 0.03 * fine, base) * (1 - 0.12 * st)
    pillow = smooth01((d - g / 2) / 0.006)
    h = np.where(grout, 0.0, 0.55 + 0.45 * pillow) + 0.02 * fine
    rough = np.where(grout, 0.9, 0.22 + 0.08 * t2) + 0.25 * st
    return val, h, 0.004, _mr(st, rough), tile


def tex_concrete(seed, tile=2.0):
    """Mortar-washed concrete: soft cloudy tone, pores, faint vertical rain streaks, a formwork joint per tile."""
    rng = np.random.default_rng(seed)
    u, v = ct._uv(tile)
    mid = fbm(rng, [8, 30, 90], [0.3, 0.4, 0.3])
    fine = pnoise(rng, 0.8)
    pores = pnoise(rng, 1.2)
    st = _streaks(rng, 1.3)
    jv = np.minimum(v % 0.9, 0.9 - (v % 0.9))
    joint = jv < 0.003
    val = 0.70 * (1 + 0.06 * mid + 0.03 * fine) * (1 - 0.16 * st)
    val = np.where(pores > 2.3, val * 0.7, val)
    val = np.where(joint, val * 0.85, val)
    h = 0.5 + 0.08 * mid + 0.04 * fine - np.where(pores > 2.3, 0.3, 0.0) - np.where(joint, 0.4, 0.0)
    return val, h, 0.004, _mr(st, 0.86 + 0.06 * mid), tile


def tex_stucco(seed, tile=2.0):
    rng = np.random.default_rng(seed)
    blob = fbm(rng, [1.5, 4, 16], [0.5, 0.3, 0.2])
    mid = fbm(rng, [30, 100], [0.6, 0.4])
    st = _streaks(rng, 0.8)
    val = 0.84 * (1 + 0.025 * blob + 0.04 * mid) * (1 - 0.12 * st)
    h = 0.5 + 0.25 * blob
    return val, h, 0.0025, _mr(st, 0.9 - 0.05 * blob), tile


def tex_timber(seed, tile=1.2, board=0.18):
    """Vertical boards (grain along v), a joint every `board` along u, weathered greying in the dirt channel."""
    rng = np.random.default_rng(seed)
    u, v = ct._uv(tile)
    bi = np.floor(u / board).astype(int) % int(round(tile / board))
    lu = u % board
    d = np.minimum(lu, board - lu)
    t = rng.uniform(-1, 1, 64)[bi]
    grain = pnoise(rng, 1.5, aniso=(1.0, 0.03))
    rings = np.sin(u * 180 + 3 * pnoise(rng, 20)) * 0.5 + 0.5
    st = _streaks(rng, 1.1)
    val = 0.62 * (1 + 0.12 * t + 0.10 * grain + 0.06 * rings) * (1 - 0.1 * st)
    gap = d < 0.003
    val = np.where(gap, val * 0.3, val)
    h = np.where(gap, 0.0, 0.7 + 0.3 * smooth01((d - 0.003) / 0.008)) + 0.08 * grain
    return val, h, 0.004, _mr(st, 0.78 + 0.08 * grain), tile


def tex_corrugated(seed, tile=0.8):
    """Corrugated / roller-shutter sheet: ribs along v (vertical corrugation as laid on a wall: u across)."""
    rng = np.random.default_rng(seed)
    u, v = ct._uv(tile)
    rib = 0.5 + 0.5 * np.cos(2 * math.pi * u / 0.08)
    fine = pnoise(rng, 1.0)
    st = _streaks(rng, 1.4)
    rust = smooth01(pnoise(rng, 6) * 0.5 - 0.3) * st
    val = 0.74 * (1 + 0.05 * fine + 0.08 * (rib - 0.5)) * (1 - 0.2 * st)
    h = rib + 0.02 * fine
    return val, h, 0.012, _mr(np.maximum(st, rust), 0.42 + 0.25 * rust, 0.0), tile


def tex_rooftile(seed, tile=0.9):
    """Glazed roof tile skin: the waves and courses are geometry; this adds glaze pooling and per-tile tone."""
    rng = np.random.default_rng(seed)
    u, v = ct._uv(tile)
    per, course = 0.225, 0.25
    key = (np.floor(u / per).astype(int) % 4) * 7 + (np.floor(v / course).astype(int) % 7)
    t = rng.uniform(-1, 1, 64)[key]
    pool = fbm(rng, [4, 14], [0.5, 0.5])
    lv = v % course
    edge = smooth01((course - lv) / 0.03)
    st = _streaks(rng, 1.0)
    val = 0.42 * (1 + 0.10 * t + 0.06 * pool) * (0.85 + 0.15 * edge)
    h = 0.5 + 0.1 * pool
    rough = 0.28 + 0.1 * pool + 0.3 * st
    return val, h, 0.002, _mr(st, rough), tile


def tex_metal(seed, tile=1.0):
    rng = np.random.default_rng(seed)
    fine = pnoise(rng, 0.8)
    mid = fbm(rng, [10, 40], [0.5, 0.5])
    scratch = pnoise(rng, 0.6, aniso=(0.02, 1.0))
    st = _streaks(rng, 1.0)
    val = 0.76 * (1 + 0.02 * fine + 0.03 * mid) * (1 - 0.15 * st)
    h = 0.5 + 0.03 * fine - 0.2 * (scratch > 2.6)
    return val, h, 0.001, _mr(st, 0.38 + 0.08 * mid + 0.2 * st), tile


def tex_cloth(seed, tile=0.5):
    rng = np.random.default_rng(seed)
    u, v = ct._uv(tile)
    weave = (np.sin(u * 2 * math.pi / 0.004) * np.sin(v * 2 * math.pi / 0.004)) * 0.5 + 0.5
    mid = fbm(rng, [10, 50], [0.5, 0.5])
    val = 0.86 * (1 + 0.03 * weave + 0.04 * mid)
    return val, 0.5 + 0.4 * weave, 0.0008, _mr(smooth01(mid * 0.3 + 0.2), 0.92 + 0 * mid), tile


def tex_plant(seed, tile=0.6):
    rng = np.random.default_rng(seed)
    leaf = fbm(rng, [1.5, 4], [0.6, 0.4])
    val = 0.55 * (1 + 0.25 * leaf)
    return val, 0.5 + 0.5 * leaf, 0.01, _mr(0 * leaf, 0.6 + 0.1 * leaf), tile


# ---- invented glyphs ------------------------------------------------------------------------------------------------

def glyph_atlas(seed, n=TEX, cells=16):
    """An atlas of invented glyphs (white strokes on black), 16 x 16 cells. Each glyph is built like a dense block
    script without being one: an optional narrow left component and a main component, 5 to 9 strokes on a 5 x 5
    lattice (bars, posts, enclosures, hooks, falling sweeps, ticks). No real character set is used or imitated."""
    rng = random.Random(seed)
    img = np.zeros((n, n))
    c = n // cells
    ys, xs = np.mgrid[0:c, 0:c].astype(float)
    ys = c - 1 - ys  # glyph space y up

    def seg(cell, a, b, w):
        ax, ay, bx, by = a[0] * c, a[1] * c, b[0] * c, b[1] * c
        dx, dy = bx - ax, by - ay
        L2 = dx * dx + dy * dy + 1e-6
        t = np.clip(((xs - ax) * dx + (ys - ay) * dy) / L2, 0, 1)
        dist = np.hypot(xs - (ax + t * dx), ys - (ay + t * dy))
        np.maximum(cell, smooth01((w * c - dist) / 1.2), out=cell)

    def part(cell, x0, x1, y0, y1, k):
        gx = [x0 + (x1 - x0) * i / 4 for i in range(5)]
        gy = [y0 + (y1 - y0) * i / 4 for i in range(5)]
        w = 0.042
        for _ in range(k):
            r = rng.random()
            if r < 0.3:  # bar
                j = rng.randint(0, 4)
                i0 = rng.randint(0, 2)
                seg(cell, (gx[i0], gy[j]), (gx[rng.randint(i0 + 2, 4)], gy[j]), w)
            elif r < 0.55:  # post
                i = rng.randint(0, 4)
                j0 = rng.randint(0, 2)
                seg(cell, (gx[i], gy[j0]), (gx[i], gy[rng.randint(j0 + 2, 4)]), w)
            elif r < 0.68:  # enclosure
                i0, j0 = rng.randint(0, 2), rng.randint(0, 2)
                i1, j1 = rng.randint(i0 + 1, 4), rng.randint(j0 + 1, 4)
                for (p, q) in (((i0, j0), (i1, j0)), ((i1, j0), (i1, j1)), ((i1, j1), (i0, j1)), ((i0, j1), (i0, j0))):
                    seg(cell, (gx[p[0]], gy[p[1]]), (gx[q[0]], gy[q[1]]), w)
            elif r < 0.78:  # hook: post with a kick
                i = rng.randint(1, 4)
                seg(cell, (gx[i], gy[4]), (gx[i], gy[0]), w)
                seg(cell, (gx[i], gy[0]), (gx[i - 1], gy[1]), w)
            elif r < 0.9:  # falling sweep
                i = rng.randint(1, 3)
                seg(cell, (gx[i], gy[4]), (gx[0], gy[rng.randint(0, 1)]), w)
            else:  # paired ticks
                i, j = rng.randint(0, 3), rng.randint(1, 4)
                seg(cell, (gx[i], gy[j]), (gx[i] + 0.06, gy[j] - 0.07), w)
                seg(cell, (gx[i + 1], gy[j]), (gx[i + 1] - 0.05, gy[j] - 0.07), w)

    for gy in range(cells):
        for gx in range(cells):
            cell = np.zeros((c, c))
            if rng.random() < 0.55:
                part(cell, 0.14, 0.36, 0.14, 0.86, rng.randint(2, 3))
                part(cell, 0.44, 0.86, 0.14, 0.86, rng.randint(4, 6))
            else:
                part(cell, 0.14, 0.86, 0.14, 0.86, rng.randint(5, 8))
            # every glyph carries one long diagonal sweep off the lattice, so no glyph collapses into a plain
            # box-and-bar shape that could match a real character by chance
            xa = rng.uniform(0.55, 0.85)
            seg(cell, (xa, rng.uniform(0.6, 0.86)), (xa - rng.uniform(0.3, 0.45), rng.uniform(0.12, 0.3)), 0.042)
            img[gy * c:(gy + 1) * c, gx * c:(gx + 1) * c] = cell[::-1]
    return img


def vending_face(seed, n=TEX):
    """Front of a drinks machine: a lit display of product rows with buttons, an invented-glyph band, a pay panel."""
    rng = np.random.default_rng(seed)
    img = np.full((n, n, 3), 0.85)
    ys, xs = np.mgrid[0:n, 0:n] / n  # ys: 0 bottom
    disp = (ys > 0.42) & (ys < 0.94) & (xs > 0.06) & (xs < 0.72)
    img[disp] = (0.92, 0.93, 0.95)
    for r in range(4):
        y0 = 0.44 + r * 0.125
        for k in range(7):
            x0 = 0.08 + k * 0.09
            col = rng.uniform(0.1, 0.95, 3)
            m = (ys > y0 + 0.02) & (ys < y0 + 0.10) & (xs > x0) & (xs < x0 + 0.06)
            img[m] = col
            cap = (ys > y0 + 0.10) & (ys < y0 + 0.105) & (xs > x0 + 0.01) & (xs < x0 + 0.05)
            img[cap] = (0.7, 0.7, 0.72)
            b = (ys > y0 + 0.005) & (ys < y0 + 0.015) & (xs > x0 + 0.02) & (xs < x0 + 0.04)
            img[b] = (0.2, 0.2, 0.2)
    pay = (ys > 0.42) & (ys < 0.94) & (xs > 0.76) & (xs < 0.94)
    img[pay] = (0.25, 0.26, 0.28)
    slot = (ys > 0.08) & (ys < 0.2) & (xs > 0.1) & (xs < 0.7)
    img[slot] = (0.08, 0.08, 0.09)
    band = (ys > 0.26) & (ys < 0.38) & (xs > 0.06) & (xs < 0.94)
    img[band] = (0.95, 0.95, 0.95)
    g = glyph_atlas(seed + 1, n=256, cells=8)
    strip = np.concatenate([g[i * 32:(i + 1) * 32, :] for i in range(2)], axis=1)  # 32 x 512
    y0, x0 = int(0.28 * n), int(0.1 * n)
    sub = strip[:, :int(0.8 * n) - x0]
    sh = sub.shape
    stretch = np.repeat(sub, 2, axis=0)[:min(2 * sh[0], int(0.1 * n))]
    img[y0:y0 + stretch.shape[0], x0:x0 + stretch.shape[1]] *= (1 - 0.9 * stretch[..., None])
    return img


# =====================================================================================================================
# Materials
# =====================================================================================================================

TILES = {}
MATS = {}
TEXFILES = []


def _tex_mat(name, gen, seed, rough_default=0.8, metal=0.0, strength=1.0, flip=True):
    val, h, depth, mr, tile = gen(seed)
    rgb = np.dstack([val, val, val])
    pre = name.replace("jpk_", "jpk_t_")
    bc = save(pre + "_BC", rgb, "sRGB", "JPEG")
    nm = save(pre + "_N", nfh(h, tile, depth), "Non-Color", "PNG")
    m = save(pre + "_MR", mr, "Non-Color", "PNG")
    TEXFILES.extend([pre + "_BC.jpg", pre + "_N.png", pre + "_MR.png"])
    mat = ct.mat_tex(name, bc, nm, rough_default, metal=metal, mr=m, strength=strength)
    TILES[name] = tile
    MATS[name] = mat
    return mat


def _flat(name, srgb, rough, metal=0.0):
    MATS[name] = ct.mat_flat(name, srgb, rough, metal)
    return MATS[name]


def _img_mat(name, img_rgb, rough, tile=1.0):
    bc = save(name + "_BC", img_rgb, "sRGB", "PNG")
    TEXFILES.append(name + "_BC.png")
    mat, nt, bsdf = ct._new_mat(name)
    t = nt.nodes.new("ShaderNodeTexImage")
    t.image = bc[0]
    nt.links.new(t.outputs["Color"], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = rough
    TILES[name] = tile
    MATS[name] = mat
    return mat


def build_materials():
    _tex_mat("jpk_wall_tile", tex_smalltile, 101, 0.3)
    _tex_mat("jpk_wall_concrete", tex_concrete, 102, 0.86)
    _tex_mat("jpk_wall_stucco", tex_stucco, 103, 0.9)
    _tex_mat("jpk_trim", tex_stucco, 104, 0.8)
    _tex_mat("jpk_timber", tex_timber, 105, 0.78)
    _tex_mat("jpk_metal", tex_metal, 106, 0.4)
    _tex_mat("jpk_shutter", tex_corrugated, 107, 0.45)
    _tex_mat("jpk_rooftile", tex_rooftile, 108, 0.3)
    _tex_mat("jpk_cloth", tex_cloth, 109, 0.92)
    _tex_mat("jpk_accent", tex_cloth, 110, 0.85)
    _tex_mat("jpk_plant", tex_plant, 111, 0.6)
    _flat("jpk_glass", (0.25, 0.29, 0.32), 0.05)
    _flat("jpk_dark", (0.05, 0.05, 0.055), 0.6)
    g = glyph_atlas(7)
    _img_mat("jpk_sign", np.dstack([g, g, g]), 0.35, tile=1.0)
    _img_mat("jpk_sign_vend", vending_face(9), 0.3, tile=1.0)
    TILES["jpk_glass"] = 1.0
    TILES["jpk_dark"] = 1.0


# =====================================================================================================================
# Geometry helpers
# =====================================================================================================================

def cyl(A, p0, p1, r, mat, seg=8, smooth=True):
    p0, p1 = Vector(p0), Vector(p1)
    d = p1 - p0
    L = d.length
    q = Vector((0, 0, 1)).rotation_difference(d.normalized())
    M = Matrix.Translation(p0) @ q.to_matrix().to_4x4()
    A.lathe([(0, 0), (r, 0), (r, L), (0, L)], seg, mat, M=M, smooth=smooth)


def torus(A, c, R, r, mat, seg=20, tseg=6):
    """Torus in the YZ plane (axis X), centre c."""
    base = len(A.v)
    verts = []
    for i in range(seg):
        a = 2 * math.pi * i / seg
        for j in range(tseg):
            b = 2 * math.pi * j / tseg
            rr = R + r * math.cos(b)
            verts.append((c[0] + r * math.sin(b), c[1] + rr * math.cos(a), c[2] + rr * math.sin(a)))
    faces = []
    for i in range(seg):
        for j in range(tseg):
            i1, j1 = (i + 1) % seg, (j + 1) % tseg
            faces.append((i * tseg + j, i1 * tseg + j, i1 * tseg + j1, i * tseg + j1))
    A._push(verts, faces, mat, True)
    del base


def B(A, x0, y0, z0, x1, y1, z1, mat, bevel=0.0, M=None):
    A.box(min(x0, x1), min(y0, y1), min(z0, z1), max(x0, x1), max(y0, y1), max(z0, z1), mat, M=M, bevel=bevel)


def wall_open(A, W, z0, z1, holes, mat, d=0.25):
    """Wall slab x 0..W, z z0..z1, y 0..d with rectangular holes [(x0, x1, hz0, hz1)] (non-overlapping in x)."""
    holes = sorted(holes)
    x = 0.0
    for (hx0, hx1, hz0, hz1) in holes:
        if hx0 > x:
            B(A, x, 0, z0, hx0, d, z1, mat)
        if hz0 > z0:
            B(A, hx0, 0, z0, hx1, d, hz0, mat)
        if hz1 < z1:
            B(A, hx0, 0, hz1, hx1, d, z1, mat)
        x = hx1
    if x < W:
        B(A, x, 0, z0, W, d, z1, mat)


def alu_window(A, x0, x1, z0, z1, rec=0.08, sill=True, sill_mat="jpk_trim", grille=False):
    """Aluminium sliding window in an opening: outer frame, two overlapping sashes, glass, projecting sill."""
    f, t = 0.045, 0.06
    y0 = rec
    B(A, x0, y0, z0, x0 + f, y0 + t, z1, "jpk_metal")
    B(A, x1 - f, y0, z0, x1, y0 + t, z1, "jpk_metal")
    B(A, x0 + f, y0, z0, x1 - f, y0 + t, z0 + f, "jpk_metal")
    B(A, x0 + f, y0, z1 - f, x1 - f, y0 + t, z1, "jpk_metal")
    mid = (x0 + x1) / 2
    for (a, b, yy) in ((x0 + f, mid + 0.02, y0 + 0.005), (mid - 0.02, x1 - f, y0 + 0.03)):
        s = 0.035
        B(A, a, yy, z0 + f, a + s, yy + 0.025, z1 - f, "jpk_metal")
        B(A, b - s, yy, z0 + f, b, yy + 0.025, z1 - f, "jpk_metal")
        B(A, a + s, yy, z0 + f, b - s, yy + 0.025, z0 + f + s, "jpk_metal")
        B(A, a + s, yy, z1 - f - s, b - s, yy + 0.025, z1 - f, "jpk_metal")
        B(A, a + s, yy + 0.01, z0 + f + s, b - s, yy + 0.016, z1 - f - s, "jpk_glass")
    if sill:
        B(A, x0 - 0.05, -0.06, z0 - 0.06, x1 + 0.05, rec + 0.02, z0, sill_mat)
    if grille:
        for k in range(1, int((x1 - x0) / 0.11)):
            xx = x0 + k * (x1 - x0) / int((x1 - x0) / 0.11)
            B(A, xx - 0.008, -0.05, z0 + 0.02, xx + 0.008, -0.034, z0 + (z1 - z0) * 0.55, "jpk_metal")
        B(A, x0, -0.06, z0 + (z1 - z0) * 0.55, x1, -0.03, z0 + (z1 - z0) * 0.55 + 0.03, "jpk_metal")


def grid_shell(A, rows, off, mat):
    """Closed slab from a grid of points (rows of Vectors): top sheet, a copy moved by `off`, and rim quads."""
    bm = bmesh.new()
    top = [[bm.verts.new(p) for p in r] for r in rows]
    bot = [[bm.verts.new(p + off) for p in r] for r in rows]
    nj, ni = len(rows), len(rows[0])
    for j in range(nj - 1):
        for i in range(ni - 1):
            bm.faces.new((top[j][i], top[j][i + 1], top[j + 1][i + 1], top[j + 1][i]))
            bm.faces.new((bot[j][i], bot[j + 1][i], bot[j + 1][i + 1], bot[j][i + 1]))
    ring = ([(0, i) for i in range(ni)] + [(j, ni - 1) for j in range(1, nj)] +
            [(nj - 1, i) for i in range(ni - 2, -1, -1)] + [(j, 0) for j in range(nj - 2, 0, -1)])
    for k in range(len(ring)):
        (a, b), (c, d) = ring[k], ring[(k + 1) % len(ring)]
        bm.faces.new((top[a][b], bot[a][b], bot[c][d], top[c][d]))
    bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
    bm.verts.index_update()
    A._push([tuple(v.co) for v in bm.verts], [tuple(v.index for v in f.verts) for f in bm.faces], mat, False)
    bm.free()


def tile_surface(A, W, run, pitch_deg, ov, mat="jpk_rooftile", per=0.225, course=0.25, samples=5, amp=0.032,
                 x_off=0.0, ends=True, thick=0.04):
    """Corrugated glazed tile slope: x 0..W, from the eave (y = -ov) up to the ridge line (y = run). Tile waves are
    geometry (`amp` m), courses step 3 cm; rounded tile ends at the eave. Closed shell."""
    th = math.radians(pitch_deg)
    L = (run + ov) / math.cos(th)
    base0 = Vector((0, -ov, -ov * math.tan(th)))
    dvec = Vector((0, math.cos(th), math.sin(th)))
    nvec = Vector((0, -math.sin(th), math.cos(th)))
    nx = int(round(W / per)) * samples
    xs = [W * i / nx for i in range(nx + 1)]
    nc = int(math.ceil(L / course))
    rows = []
    for k in range(nc):
        s0 = k * course
        s1 = min((k + 1) * course - 0.004, L)
        rows += [(s0, 0.03), (s1, 0.0)]
        if s1 >= L:
            break
    grid = []
    for (s, step) in rows:
        grid.append([base0 + dvec * s + nvec * (amp * math.sin(2 * math.pi * (x + x_off) / per) + step + amp)
                     + Vector((x, 0, 0)) for x in xs])
    grid_shell(A, grid, -nvec * thick, mat)
    if ends:  # round eave-tile ends on each crest
        for i in range(int(round(W / per))):
            x = per * i + per / 4 - x_off % per
            if x < 0.05 or x > W - 0.05:
                continue
            c = base0 + nvec * (2 * amp + 0.03) + Vector((x, 0, 0))
            cyl(A, c - dvec * 0.03, c + dvec * 0.06, 0.055, mat, seg=10, smooth=True)
    return th


def ucx_box(name, x0, y0, z0, x1, y1, z1, i=0):
    return ct.ucx(name, [MATS["jpk_metal"]], ct.boxv(min(x0, x1), min(y0, y1), min(z0, z1), max(x0, x1),
                                                    max(y0, y1), max(z0, z1)), i)


# =====================================================================================================================
# Pieces. Each builder returns (Acc, hulls [(x0,y0,z0,x1,y1,z1)], snaps {name: (x, y, z)})
# =====================================================================================================================

STOREY, SHOP_H = 3.0, 3.6


def _band(A, W, mat="jpk_trim"):
    """Slab-edge string course at the storey line."""
    B(A, 0, -0.04, 0, W, 0.25, 0.22, mat)


def _bay_snaps(W, H):
    return {"R": (W, 0, 0), "T": (0, 0, H)}


def p_bay_win(W=1.8, wall="jpk_wall_tile"):
    A = Acc()
    _band(A, W)
    ox0, ox1 = (W - 1.2) / 2, (W + 1.2) / 2
    wall_open(A, W, 0.22, STOREY, [(ox0, ox1, 1.0, 2.3)], wall)
    alu_window(A, ox0, ox1, 1.0, 2.3)
    B(A, ox0 - 0.08, -0.12, 2.3, ox1 + 0.08, 0.0, 2.36, "jpk_metal")  # small rain hood
    return A, [(0, -0.12, 0, W, 0.25, STOREY)], dict(_bay_snaps(W, STOREY), AC=(ox1 + 0.1, -0.05, 1.2))


def p_bay_blank(W=0.9, wall="jpk_wall_concrete"):
    A = Acc()
    _band(A, W)
    B(A, 0, 0, 0.22, W, 0.25, STOREY, wall)
    B(A, W - 0.1, -0.12, 0.22, W - 0.04, -0.06, STOREY, "jpk_metal")  # downpipe (square)
    return A, [(0, -0.12, 0, W, 0.25, STOREY)], _bay_snaps(W, STOREY)


def p_bay_balc(W=2.7, wall="jpk_wall_tile"):
    A = Acc()
    D = 1.1
    B(A, 0, 0, 0, W, 0.25, 0.22, "jpk_trim")
    wall_open(A, W, 0.22, STOREY, [(0.35, W - 0.35, 0.22, 2.3)], wall)
    # glazed sliding doors
    x0, x1 = 0.35, W - 0.35
    B(A, x0, 0.08, 0.22, x1, 0.14, 0.27, "jpk_metal")
    B(A, x0, 0.08, 2.25, x1, 0.14, 2.3, "jpk_metal")
    for xx in (x0, (x0 + x1) / 2 - 0.02, x1 - 0.05):
        B(A, xx, 0.08, 0.27, xx + 0.05, 0.14, 2.25, "jpk_metal")
    B(A, x0 + 0.05, 0.1, 0.27, x1 - 0.05, 0.11, 2.25, "jpk_glass")
    # slab with a drip edge, solid parapet with coping, side screens
    B(A, 0, -D, 0, W, 0, 0.18, "jpk_trim")
    B(A, 0, -D - 0.02, -0.04, W, -D + 0.04, 0.0, "jpk_trim")
    B(A, 0, -D, 0.18, W, -D + 0.12, 1.15, wall)
    B(A, -0.01, -D - 0.025, 1.15, W + 0.01, -D + 0.145, 1.2, "jpk_trim")
    for sx in (0.0, W - 0.1):
        B(A, sx, -D + 0.12, 0.18, sx + 0.1, 0, 2.6, "jpk_trim")
    # laundry pole brackets hanging from the soffit and a pole
    for bx in (0.35, W - 0.35):
        B(A, bx - 0.015, -0.75, 2.45, bx + 0.015, -0.72, STOREY, "jpk_metal")
        B(A, bx - 0.015, -0.9, 2.42, bx + 0.015, -0.6, 2.46, "jpk_metal")
    cyl(A, (0.25, -0.86, 2.48), (W - 0.25, -0.86, 2.48), 0.017, "jpk_metal", seg=6)
    cyl(A, (0.25, -0.65, 2.48), (W - 0.25, -0.65, 2.48), 0.017, "jpk_metal", seg=6)
    # outdoor air-conditioner unit on the balcony floor
    _ac(A, W - 1.0, -0.85, 0.18)
    return A, [(0, -D - 0.03, 0, W, 0.25, 1.2), (0, 0, 1.2, W, 0.25, STOREY)], _bay_snaps(W, STOREY)


def _ac(A, x0, y0, z0, w=0.8, d=0.28, h=0.55):
    """Outdoor AC unit, front at y0 (facing -Y)."""
    B(A, x0, y0, z0 + 0.06, x0 + w, y0 + d, z0 + h, "jpk_metal", bevel=0.012)
    for fx in (x0 + 0.06, x0 + w - 0.1):
        B(A, fx, y0 + 0.02, z0, fx + 0.04, y0 + d - 0.02, z0 + 0.06, "jpk_dark")
    cx, cz = x0 + w * 0.4, z0 + 0.06 + (h - 0.06) / 2
    r = (h - 0.06) * 0.4
    cyl(A, (cx, y0 + 0.01, cz), (cx, y0 - 0.008, cz), r, "jpk_dark", seg=16)
    for k in range(5):
        zz = cz - r + (k + 0.5) * 2 * r / 5
        B(A, cx - r, y0 - 0.02, zz - 0.005, cx + r, y0 - 0.012, zz + 0.005, "jpk_metal")
    B(A, x0 + w * 0.78, y0 - 0.01, z0 + 0.15, x0 + w * 0.94, y0, z0 + h - 0.1, "jpk_dark")
    cyl(A, (x0 + w - 0.05, y0 + d - 0.05, z0 + 0.25), (x0 + w + 0.15, y0 + d - 0.05, z0 + 0.25), 0.012, "jpk_metal",
        seg=6)


def p_bay_corr(W=2.7, wall="jpk_wall_concrete"):
    A = Acc()
    D = 1.3
    B(A, 0, 0, 0, W, 0.25, 0.22, "jpk_trim")
    wall_open(A, W, 0.22, STOREY, [(0.3, 1.2, 0.22, 2.3), (1.6, 2.3, 1.2, 2.0)], wall)
    # steel door with frame, lever, peep, newspaper slot
    B(A, 0.3, 0.02, 0.22, 1.2, 0.06, 2.3, "jpk_shutter")
    for (a, b, c, d) in ((0.27, 0.3, 0.22, 2.33), (1.2, 1.23, 0.22, 2.33), (0.27, 1.23, 2.3, 2.33)):
        B(A, a, -0.02, c, b, 0.06, d, "jpk_metal")
    B(A, 1.05, -0.02, 1.0, 1.12, 0.02, 1.04, "jpk_dark")
    B(A, 0.6, 0.0, 1.05, 0.9, 0.02, 1.09, "jpk_dark")
    alu_window(A, 1.6, 2.3, 1.2, 2.0, sill=False, grille=True)
    B(A, 2.4, -0.18, 1.3, 2.62, 0.0, 1.75, "jpk_metal")  # meter box
    cyl(A, (2.55, -0.06, 0.22), (2.55, -0.06, STOREY), 0.02, "jpk_metal", seg=6)  # gas pipe
    # corridor slab + drip + steel rail on a low upstand
    B(A, 0, -D, 0, W, 0, 0.18, "jpk_trim")
    B(A, 0, -D - 0.02, -0.05, W, -D + 0.05, 0.0, "jpk_trim")
    B(A, 0, -D, 0.18, W, -D + 0.12, 0.35, "jpk_trim")
    n = int(round(W / 0.12))
    for k in range(n):
        xx = (k + 0.5) * W / n
        B(A, xx - 0.009, -D + 0.05, 0.35, xx + 0.009, -D + 0.068, 1.08, "jpk_metal")
    for xx in (0.05, W / 2, W - 0.05):
        B(A, xx - 0.03, -D + 0.03, 0.35, xx + 0.03, -D + 0.09, 1.12, "jpk_metal")
    B(A, 0, -D + 0.02, 1.08, W, -D + 0.1, 1.13, "jpk_metal")
    B(A, 0, -D + 0.045, 0.4, W, -D + 0.075, 0.43, "jpk_metal")
    return A, [(0, -D - 0.03, 0, W, 0.25, 1.13), (0, 0, 1.13, W, 0.25, STOREY)], _bay_snaps(W, STOREY)


def p_bay_pencil(W=4.5, wall="jpk_wall_tile"):
    A = Acc()
    _band(A, W)
    ox0, ox1 = 0.9, 3.6
    wall_open(A, W, 0.22, STOREY, [(ox0, ox1, 0.9, 2.4)], wall)
    for (a, b) in ((0, 0.25), (W - 0.25, W)):  # pilasters
        B(A, a, -0.07, 0.22, b, 0.0, STOREY, "jpk_trim")
    # fixed three-light window with mullions, sill and a hood
    B(A, ox0, 0.08, 0.9, ox1, 0.14, 0.95, "jpk_metal")
    B(A, ox0, 0.08, 2.35, ox1, 0.14, 2.4, "jpk_metal")
    for k in range(4):
        xx = ox0 + k * (ox1 - ox0) / 3
        B(A, xx - 0.025, 0.08, 0.95, xx + 0.025, 0.14, 2.35, "jpk_metal")
    B(A, ox0, 0.1, 0.95, ox1, 0.11, 2.35, "jpk_glass")
    B(A, ox0 - 0.05, -0.06, 0.84, ox1 + 0.05, 0.1, 0.9, "jpk_trim")
    # vertical sign bracket on the right (arms at 0.4 and 2.6, standoff 0.9)
    for z in (0.4, 2.6):
        B(A, W - 0.48, -0.95, z, W - 0.42, 0.0, z + 0.06, "jpk_metal")
        B(A, W - 0.5, -0.02, z - 0.1, W - 0.4, 0.0, z + 0.16, "jpk_metal")
    return A, [(0, -0.12, 0, W, 0.25, STOREY)], dict(_bay_snaps(W, STOREY), SIGN=(W - 0.45, -0.25, 0.46))


def _storefront(A, W, H, door_w=1.8, transom=2.25, M=None, recess=0.15):
    """Aluminium glazed front in local x 0..W (face at y = 0): kick plate, fixed lights, sliding doors in the middle,
    a transom bar and transom lights."""
    y0, y1 = recess, recess + 0.07
    top = H
    B(A, 0, y0 - 0.02, 0, W, y1, 0.12, "jpk_metal", M=M)
    B(A, 0, y0, transom, W, y1, transom + 0.07, "jpk_metal", M=M)
    B(A, 0, y0, top - 0.06, W, y1, top, "jpk_metal", M=M)
    dx0, dx1 = (W - door_w) / 2, (W + door_w) / 2
    posts = [0.0, W - 0.06, dx0 - 0.03, dx1 - 0.03]
    side = dx0
    if side > 1.4:
        posts.append(side / 2 - 0.03)
        posts.append(W - side / 2 - 0.03)
    for p in posts:
        B(A, p, y0, 0.12, p + 0.06, y1, top - 0.06, "jpk_metal", M=M)
    nt = max(1, int(round(W / 1.2)))
    for k in range(1, nt):
        p = k * W / nt
        B(A, p - 0.025, y0, transom + 0.07, p + 0.025, y1, top - 0.06, "jpk_metal", M=M)
    B(A, 0.03, y0 + 0.03, 0.12, W - 0.03, y0 + 0.04, top - 0.06, "jpk_glass", M=M)
    # sliding door leaves (frame proud of the glass line)
    for (a, b, yy) in ((dx0, (dx0 + dx1) / 2 + 0.02, y0 - 0.04), ((dx0 + dx1) / 2 - 0.02, dx1, y0 - 0.075)):
        B(A, a, yy, 0.12, a + 0.05, yy + 0.035, transom, "jpk_metal", M=M)
        B(A, b - 0.05, yy, 0.12, b, yy + 0.035, transom, "jpk_metal", M=M)
        B(A, a, yy, 0.12, b, yy + 0.035, 0.2, "jpk_metal", M=M)
        B(A, a, yy, transom - 0.05, b, yy + 0.035, transom, "jpk_metal", M=M)
        B(A, a + 0.15, yy - 0.015, 0.95, a + 0.18, yy, 1.25, "jpk_dark", M=M)


def _shop_frame(A, W, H, wall, Hopen=2.9):
    """Pilasters and lintel around a shop opening of the ground storey."""
    B(A, 0, -0.05, 0, 0.3, 0.25, H, wall)
    B(A, W - 0.3, -0.05, 0, W, 0.25, H, wall)
    B(A, 0.3, 0, Hopen, W - 0.3, 0.25, H, wall)
    B(A, 0, -0.08, Hopen - 0.04, W, 0.0, Hopen, "jpk_trim")  # soffit edge / drip


def p_shop_glazed(W=3.6, wall="jpk_wall_concrete"):
    A = Acc()
    H = SHOP_H
    _shop_frame(A, W, H, wall)
    M = Matrix.Translation((0.3, 0, 0))
    _storefront(A, W - 0.6, 2.9, door_w=1.8 if W > 4 else 1.6, M=M)
    B(A, 0.3, -0.45, -0.0, W - 0.3, 0.0, 0.1, "jpk_trim")  # step
    return (A, [(0, -0.45, 0, W, 0.25, H)],
            {"R": (W, 0, 0), "T": (0, 0, H), "FASCIA": (0, -0.08, 2.95), "AWNING": (0.3, 0, 2.85)})


def p_shop_shutter(W=6.3, wall="jpk_wall_concrete"):
    A = Acc()
    H = SHOP_H
    _shop_frame(A, W, H, wall)
    x0, x1 = 0.3, W - 0.3
    B(A, x0 - 0.02, -0.32, 2.6, x1 + 0.02, 0.0, 2.92, "jpk_shutter", bevel=0.02)  # shutter box
    for gx in (x0, x1 - 0.08):
        B(A, gx, -0.12, 0, gx + 0.08, 0.0, 2.6, "jpk_metal")  # guide rails
    B(A, x0 + 0.08, -0.07, 0.0, x1 - 0.08, -0.05, 2.6, "jpk_shutter")  # curtain
    n = int(2.55 / 0.09)
    for k in range(n):
        z = 0.06 + k * 0.09
        B(A, x0 + 0.08, -0.085, z, x1 - 0.08, -0.07, z + 0.025, "jpk_shutter")
    B(A, x0 + 0.08, -0.1, 0.0, x1 - 0.08, -0.05, 0.06, "jpk_metal")  # bottom bar
    for hx in (x0 + 0.6, x1 - 0.75):
        B(A, hx, -0.11, 0.25, hx + 0.15, -0.1, 0.29, "jpk_dark")
    return (A, [(0, -0.32, 0, W, 0.25, H)],
            {"R": (W, 0, 0), "T": (0, 0, H), "FASCIA": (0, -0.32, 2.95), "AWNING": (0.3, 0, 2.95)})


def p_shop_koshi(W=5.4, wall="jpk_wall_stucco"):
    """Timber town-house front: lattice (koshi) window, recessed sliding entrance with a noren rail and curtain."""
    A = Acc()
    H = STOREY
    for px in (0.0, 2.85, W - 0.15):
        B(A, px, -0.06, 0, px + 0.15, 0.2, 2.45, "jpk_timber")  # posts
    B(A, 0, -0.08, 2.3, W, 0.2, 2.5, "jpk_timber")  # head beam
    B(A, 0, 0.05, 2.5, W, 0.2, H, wall)  # plaster band up to the pent eave
    # lattice: base board, slats, rails
    B(A, 0.15, -0.02, 0.0, 2.85, 0.15, 0.45, "jpk_timber")
    B(A, 0.15, 0.08, 0.45, 2.85, 0.1, 2.3, "jpk_dark")
    n = int(2.7 / 0.055)
    for k in range(n):
        xx = 0.15 + (k + 0.5) * 2.7 / n
        B(A, xx - 0.014, -0.04, 0.45, xx + 0.014, 0.0, 2.3, "jpk_timber")
    for z in (0.45, 1.35):
        B(A, 0.15, -0.05, z, 2.85, 0.02, z + 0.04, "jpk_timber")
    # entrance: recessed sliding timber-and-glass doors
    ex0, ex1 = 3.0, W - 0.15
    B(A, ex0, 0.3, 0.0, ex1, 0.36, 2.3, "jpk_glass")
    for (a, b, yy) in ((ex0, (ex0 + ex1) / 2 + 0.03, 0.22), ((ex0 + ex1) / 2 - 0.03, ex1, 0.26)):
        for (c, d) in ((a, a + 0.05), (b - 0.05, b)):
            B(A, c, yy, 0.0, d, yy + 0.035, 2.3, "jpk_timber")
        for z in (0.0, 0.7, 1.5, 2.25):
            B(A, a, yy, z, b, yy + 0.035, z + 0.05, "jpk_timber")
    B(A, ex0, 0.0, 0.0, ex1, 0.38, 0.05, "jpk_trim")  # threshold
    B(A, ex0, 0.0, 2.3, ex1, 0.38, 2.32, "jpk_timber")
    # noren rail and three curtain panels with slits
    cyl(A, (ex0 + 0.05, -0.12, 2.22), (ex1 - 0.05, -0.12, 2.22), 0.015, "jpk_timber", seg=6)
    for (a, b) in ((ex0, ex1 - 0.15),):
        B(A, a, -0.15, 2.18, a + 0.03, 0.0, 2.25, "jpk_metal")
        B(A, b + 0.12, -0.15, 2.18, b + 0.15, 0.0, 2.25, "jpk_metal")
    pw = (ex1 - ex0 - 0.1 - 0.04) / 3
    for k in range(3):
        a = ex0 + 0.05 + k * (pw + 0.02)
        B(A, a, -0.13, 1.35, a + pw, -0.115, 2.2, "jpk_cloth")
    return (A, [(0, -0.15, 0, W, 0.25, H)],
            {"R": (W, 0, 0), "T": (0, 0, H), "EAVE": (0, 0, H)})


def p_shop_corner(W=6.3, D=3.6, wall="jpk_wall_concrete"):
    """Corner unit: glazed on the front and the left return, a chamfered corner entrance."""
    A = Acc()
    H = SHOP_H
    ch = 1.2
    # piers: right end, back end of the return, and corner beam
    B(A, W - 0.3, -0.05, 0, W, 0.25, H, wall)
    B(A, -0.05, D - 0.3, 0, 0.25, D, H, wall)
    # front run from x = ch to W - 0.3
    _storefront(A, W - 0.3 - ch, 2.9, door_w=1.6, M=Matrix.Translation((ch, 0, 0)))
    # return run on the x = 0 side, from y = ch to D - 0.3, face -X: local x runs +Y
    Mr = Matrix.Translation((0, D - 0.3, 0)) @ Matrix.Rotation(math.radians(-90), 4, "Z")
    _storefront(A, D - 0.3 - ch, 2.9, door_w=0.0, M=Mr)
    # chamfer: automatic door on the diagonal
    Mc = Matrix.Translation((0, ch, 0)) @ Matrix.Rotation(math.radians(-45), 4, "Z")
    _storefront(A, ch * math.sqrt(2), 2.9, door_w=1.2, M=Mc, recess=0.05)
    # lintel wrapping the corner (fascia band carrier) and soffit
    B(A, ch, 0, 2.9, W - 0.3, 0.25, H, wall)
    B(A, 0, ch, 2.9, 0.25, D - 0.3, H, wall)
    A.prism_xy([(0, ch), (ch, 0), (ch, 0.25), (0.25, ch)], 2.9, H, wall)
    A.prism_xy([(0, ch), (ch, 0), (ch + 0.3, -0.35), (-0.35, ch + 0.3)], 2.86, 2.92, "jpk_trim")
    B(A, ch, -0.08, 2.86, W, 0.0, 2.9, "jpk_trim")
    B(A, -0.08, ch, 2.86, 0.0, D, 2.9, "jpk_trim")
    B(A, 0.4, -0.5, 0.0, W - 0.3, 0.0, 0.08, "jpk_trim")
    return (A, [(0, -0.5, 0, W, D, H)],
            {"R": (W, 0, 0), "T": (0, 0, H), "RETURN_END": (0, D, 0), "FASCIA": (ch, -0.08, 2.95)})


# ---- roof, flat ------------------------------------------------------------------------------------------------------

def p_parapet(W=0.9, wall="jpk_wall_concrete"):
    A = Acc()
    B(A, 0, -0.05, 0, W, 0.25, 0.18, "jpk_trim")  # cornice band
    B(A, 0, 0, 0.18, W, 0.2, 1.05, wall)
    B(A, 0, -0.04, 1.05, W, 0.24, 1.12, "jpk_trim")  # coping with overhang
    return A, [(0, -0.05, 0, W, 0.25, 1.12)], {"R": (W, 0, 0)}


def p_parapet_corner(wall="jpk_wall_concrete"):
    """Outer corner block: fills the 0.25 x 0.25 m corner where a front and a side parapet run meet (x -0.25..0)."""
    A = Acc()
    B(A, -0.25, -0.05, 0, 0.0, 0.25, 0.18, "jpk_trim")
    B(A, -0.05, -0.05, 0, 0.0, 0.0, 0.18, "jpk_trim")
    B(A, -0.2, 0, 0.18, 0.0, 0.2, 1.05, wall)
    B(A, -0.24, -0.04, 1.05, 0.0, 0.24, 1.12, "jpk_trim")
    return A, [(-0.25, -0.05, 0, 0, 0.25, 1.12)], {"FRONT_RUN": (0, 0, 0)}


def p_rail(W=1.8):
    A = Acc()
    for xx in (0.025, W / 2, W - 0.025):
        cyl(A, (xx, 0.1, 0), (xx, 0.1, 1.1), 0.024, "jpk_metal", seg=8)
        B(A, xx - 0.06, 0.04, 0, xx + 0.06, 0.16, 0.012, "jpk_metal")
    for z in (0.55, 1.1):
        cyl(A, (0, 0.1, z), (W, 0.1, z), 0.021, "jpk_metal", seg=8)
    return A, [(0, 0.05, 0, W, 0.15, 1.12)], {"R": (W, 0, 0)}


def p_tank():
    """Panel-type water tank on a steel stand with a ladder."""
    A = Acc()
    w, d, h, st = 2.0, 1.5, 1.5, 1.1
    for (x, y) in ((0.1, 0.1), (w - 0.1, 0.1), (0.1, d - 0.1), (w - 0.1, d - 0.1), (w / 2, 0.1), (w / 2, d - 0.1)):
        B(A, x - 0.05, y - 0.05, 0, x + 0.05, y + 0.05, st, "jpk_metal")
        B(A, x - 0.12, y - 0.12, 0, x + 0.12, y + 0.12, 0.15, "jpk_wall_concrete")
    for y in (0.1, d - 0.1):
        B(A, 0.05, y - 0.04, st - 0.12, w - 0.05, y + 0.04, st, "jpk_metal")
        cyl(A, (0.12, y, 0.2), (w / 2 - 0.05, y, st - 0.15), 0.02, "jpk_metal", seg=6)
        cyl(A, (w - 0.12, y, 0.2), (w / 2 + 0.05, y, st - 0.15), 0.02, "jpk_metal", seg=6)
    for x in (0.1, w - 0.1):
        B(A, x - 0.04, 0.05, st - 0.12, x + 0.04, d - 0.05, st, "jpk_metal")
    B(A, 0, 0, st, w, d, st + h, "jpk_trim", bevel=0.03)
    for k in range(1, 4):  # panel ribs on the long faces and ends
        z = st + k * h / 4
        B(A, -0.03, -0.03, z - 0.025, w + 0.03, d + 0.03, z + 0.025, "jpk_trim")
    for k in range(1, 5):
        x = k * w / 5
        B(A, x - 0.025, -0.03, st, x + 0.025, d + 0.03, st + h, "jpk_trim")
    B(A, w / 2 - 0.3, d / 2 - 0.3, st + h, w / 2 + 0.3, d / 2 + 0.3, st + h + 0.12, "jpk_metal")  # manhole
    for xx in (w + 0.05, w + 0.45):  # ladder
        B(A, xx, 0.5, 0, xx + 0.04, 0.54, st + h + 0.3, "jpk_metal")
    for k in range(9):
        z = 0.3 * (k + 1)
        B(A, w + 0.05, 0.5, z, w + 0.49, 0.53, z + 0.025, "jpk_metal")
    cyl(A, (0.5, 0.75, 0.3), (0.5, 0.75, st), 0.04, "jpk_metal", seg=8)
    return A, [(0, 0, 0, w + 0.5, d, st + h + 0.12)], {"BASE": (0, 0, 0)}


def p_plant_room(wall="jpk_wall_concrete"):
    A = Acc()
    w, d, h = 2.4, 2.4, 2.5
    wall_open(A, w, 0, h, [(0.7, 1.55, 0.0, 2.05)], wall, d=d)
    B(A, 0.72, 0.02, 0.0, 1.53, 0.06, 2.03, "jpk_shutter")
    for (a, b, c, e) in ((0.66, 0.7, 0, 2.09), (1.55, 1.59, 0, 2.09), (0.66, 1.59, 2.05, 2.09)):
        B(A, a, -0.03, c, b, 0.03, e, "jpk_metal")
    B(A, -0.12, -0.12, h, w + 0.12, d + 0.12, h + 0.15, "jpk_trim")  # roof slab with overhang
    B(A, -0.14, -0.14, h + 0.15, w + 0.14, d + 0.14, h + 0.2, "jpk_trim")
    B(A, 1.8, -0.05, 1.4, 2.2, 0.0, 2.0, "jpk_dark")  # louvre
    for k in range(6):
        z = 1.42 + k * 0.1
        B(A, 1.8, -0.09, z, 2.2, -0.04, z + 0.03, "jpk_metal")
    B(A, 0.6, -0.4, 0, 1.65, 0.0, 0.15, "jpk_trim")
    return A, [(-0.14, -0.4, 0, w + 0.14, d + 0.14, h + 0.2)], {"BASE": (0, 0, 0)}


def p_antenna():
    A = Acc()
    for a in range(3):
        ang = math.radians(90 + a * 120)
        cyl(A, (0, 0, 0.0), (0.7 * math.cos(ang), 0.7 * math.sin(ang), 0.02), 0.015, "jpk_metal", seg=6)
        cyl(A, (0, 0, 1.0), (0.7 * math.cos(ang), 0.7 * math.sin(ang), 0.02), 0.01, "jpk_metal", seg=6)
    cyl(A, (0, 0, 0), (0, 0, 3.2), 0.03, "jpk_metal", seg=8)
    for (z, L) in ((3.0, 1.6), (2.4, 1.2)):  # two yagis
        cyl(A, (-0.2, -L / 2, z), (-0.2, L / 2, z), 0.012, "jpk_metal", seg=6)
        n = int(L / 0.12)
        for k in range(n):
            y = -L / 2 + (k + 0.5) * L / n
            hw = 0.24 - 0.06 * k / n
            cyl(A, (-0.2 - hw, y, z), (-0.2 + hw, y, z), 0.005, "jpk_metal", seg=4)
    cyl(A, (0, 0, 2.0), (0.0, 0.5, 2.0), 0.012, "jpk_metal", seg=6)
    A.lathe([(0, 1.9), (0.22, 1.85), (0.24, 1.9), (0.0, 2.04)], 16, "jpk_trim",
            M=Matrix.Translation((0, 0.55, 0)) @ Matrix.Rotation(math.radians(-90), 4, "X")
            @ Matrix.Translation((0, -2.0, 0)), smooth=True)  # small dish
    return A, [(-0.3, -0.3, 0, 0.3, 0.3, 3.2)], {"BASE": (0, 0, 0)}


# ---- roof, tile (house depth 5.4: run 2.7 each side) -----------------------------------------------------------------

RUN, PITCH, OV = 2.7, 24.0, 0.6
RISE = RUN * math.tan(math.radians(PITCH))


def p_tile_slope(W=0.9):
    """One 0.9 m strip of the front slope from the eave (overhang 0.6) to the ridge line, origin at the front wall
    top. The back slope is the same piece turned 180 degrees about the ridge."""
    A = Acc()
    tile_surface(A, W, RUN, PITCH, OV)
    th = math.radians(PITCH)
    # eave fascia and rafter ends under the overhang
    ez = -OV * math.tan(th)
    B(A, 0, -OV - 0.02, ez - 0.2, W, -OV + 0.03, ez + 0.01, "jpk_timber")
    for xx in (0.2, 0.65):
        B(A, xx, -OV + 0.03, ez - 0.16, xx + 0.07, 0.0, ez - 0.04, "jpk_timber")
    B(A, 0, -OV + 0.03, ez - 0.04, W, 0.0, ez - 0.02, "jpk_timber")  # soffit board
    return (A, [(0, -OV, ez - 0.2, W, RUN, RISE + 0.1)],
            {"R": (W, 0, 0), "RIDGE": (0, RUN, RISE), "EAVE": (0, -OV, ez)})


def p_ridge(W=0.9):
    A = Acc()
    z = RISE + 0.07
    for k, (wd, h) in enumerate(((0.42, 0.05), (0.34, 0.05), (0.28, 0.05))):
        B(A, 0, RUN - wd / 2, z, W, RUN + wd / 2, z + h, "jpk_rooftile")
        z += h
    cyl(A, (0, RUN, z + 0.03), (W, RUN, z + 0.03), 0.1, "jpk_rooftile", seg=12)
    B(A, 0, RUN - 0.3, RISE - 0.02, W, RUN + 0.3, RISE + 0.07, "jpk_wall_stucco")  # mortar bed
    return A, [(0, RUN - 0.3, RISE, W, RUN + 0.3, z + 0.13)], {"R": (W, 0, 0)}


def p_gable_end():
    """Gable end at x = 0 for a 5.4 m deep house: plaster gable, barge boards, verge tile rolls, ridge-end block."""
    A = Acc()
    D = 2 * RUN
    th = math.radians(PITCH)
    ez = -OV * math.tan(th)
    # gable wall (stucco) above the wall top
    poly = [(0.0, 0.0), (D, 0.0), (RUN, RISE)]
    A.prism_xy([(0, 0), (0.2, 0), (0.2, D), (0, D)], -0.25, 0.0, "jpk_trim")
    Mg = Matrix.Rotation(math.radians(90), 4, "Z")  # XZ triangle into YZ plane
    A.prism_xz([(p[0], p[1]) for p in poly], -0.2, 0.0, "jpk_wall_stucco", M=Mg)
    # barge boards and verge rolls along both rakes
    for sgn in (1, -1):
        y0, y1 = (-OV, RUN) if sgn > 0 else (D + OV, RUN)
        z0 = ez
        p0 = Vector((-0.05, y0, z0 + 0.05))
        p1 = Vector((-0.05, y1, RISE + 0.05))
        dv = (p1 - p0)
        L = dv.length
        rot = Vector((0, 1, 0)).rotation_difference(dv.normalized()).to_matrix().to_4x4()
        Mb = Matrix.Translation(p0) @ rot
        B(A, -0.04, 0, -0.28, 0.0, L, 0.0, "jpk_timber", M=Mb)
        cyl(A, p0 + Vector((0.06, 0, 0.12)), p1 + Vector((0.06, 0, 0.12)), 0.07, "jpk_rooftile", seg=10)
    # ridge-end ornament block
    B(A, -0.12, RUN - 0.2, RISE + 0.05, 0.1, RUN + 0.2, RISE + 0.55, "jpk_rooftile", bevel=0.03)
    cyl(A, (-0.15, RUN, RISE + 0.5), (0.1, RUN, RISE + 0.5), 0.17, "jpk_rooftile", seg=14)
    return (A, [(-0.25, -OV, -0.25, 0.2, D + OV, RISE + 0.6)], {"RIDGE": (0, RUN, RISE)})


def p_hip_end():
    """Hip end at x = 0 for a 5.4 m deep house: three stepped tile faces of the end pyramid (x -0.6..2.7) and two
    hip rolls. Full-width slope strips start at x = 2.7."""
    A = Acc()
    D = 2 * RUN
    th = math.tan(math.radians(PITCH))
    ez = -OV * th
    apex = Vector((RUN, RUN, RISE))
    corners = [Vector((-OV, -OV, ez)), Vector((-OV, D + OV, ez))]
    course = 0.25
    # faces: (eave edge a, eave edge b) -> to the apex line; front face ends at (RUN, -OV)
    faces = [
        (Vector((-OV, -OV, ez)), Vector((RUN, -OV, ez)), apex, apex),               # front triangle-trapezoid part
        (Vector((RUN, D + OV, ez)), Vector((-OV, D + OV, ez)), apex, apex),          # back part
        (Vector((-OV, D + OV, ez)), Vector((-OV, -OV, ez)), apex, apex),             # end triangle
    ]
    for (a, b, c, d) in faces:
        n = 12
        bm = bmesh.new()
        rows = []
        for k in range(n + 1):
            t = k / n
            for lift in ((0.03, 0.0) if 0 < k < n else (0.0,)):
                pa = a.lerp(d, t)
                pb = b.lerp(c, t)
                nrm = (b - a).cross(d - a).normalized()
                rows.append((bm.verts.new(pa + nrm * (0.06 + lift)), bm.verts.new(pb + nrm * (0.06 + lift))))
        for j in range(len(rows) - 1):
            (p, q), (r, s) = rows[j], rows[j + 1]
            if (p.co - r.co).length < 1e-5 and (q.co - s.co).length < 1e-5:
                continue
            try:
                bm.faces.new((p, q, s, r))
            except ValueError:
                pass
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-4)
        bmesh.ops.solidify(bm, geom=list(bm.faces), thickness=0.04)
        bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
        bm.verts.index_update()
        A._push([tuple(v.co) for v in bm.verts], [tuple(v.index for v in f.verts) for f in bm.faces],
                "jpk_rooftile", False)
        bm.free()
        del course
        course = 0.25
    for cn in corners:
        cyl(A, cn + Vector((0, 0, 0.12)), apex + Vector((0, 0, 0.14)), 0.085, "jpk_rooftile", seg=10)
    B(A, -OV - 0.02, -OV - 0.02, ez - 0.2, -OV + 0.03, D + OV + 0.02, ez + 0.01, "jpk_timber")
    B(A, -OV, -OV - 0.02, ez - 0.2, RUN, -OV + 0.03, ez + 0.01, "jpk_timber")
    B(A, -OV, D + OV - 0.03, ez - 0.2, RUN, D + OV + 0.02, ez + 0.01, "jpk_timber")
    cyl(A, apex + Vector((-0.1, 0, 0.14)), apex + Vector((0.2, 0, 0.14)), 0.12, "jpk_rooftile", seg=12)
    return (A, [(-OV, -OV, ez - 0.2, RUN, D + OV, RISE + 0.25)], {"RIDGE": (RUN, RUN, RISE)})


def p_eave_pent(W=0.9):
    """Pent eave (a short tiled roof between the ground floor and the first floor), origin on the wall at the top."""
    A = Acc()
    run, ov = 0.0, 0.85
    th = tile_surface(A, W, 0.15, 22.0, ov, per=0.225, course=0.24, thick=0.035)
    ez = -ov * math.tan(th)
    B(A, 0, -ov - 0.02, ez - 0.16, W, -ov + 0.03, ez + 0.01, "jpk_timber")
    for xx in (0.1, 0.55):
        B(A, xx, -ov + 0.03, ez - 0.13, xx + 0.07, 0.0, ez - 0.03, "jpk_timber")
        B(A, xx, -0.06, ez - 0.13, xx + 0.07, 0.0, 0.06, "jpk_timber")
    B(A, 0, -0.04, 0.05, W, 0.0, 0.18, "jpk_wall_stucco")  # flashing upstand
    del run
    return A, [(0, -ov, ez - 0.16, W, 0.2, 0.2)], {"R": (W, 0, 0)}


# ---- signs, awnings, clutter ----------------------------------------------------------------------------------------

def p_sign_vert(H=3.0):
    """Blade sign (perpendicular to the wall): 0.25 thick (x), 0.6 out (y), H tall; both faces carry glyphs."""
    A = Acc()
    t, d = 0.25, 0.6
    B(A, -t / 2, -d - 0.25, 0, t / 2, -0.25, H, "jpk_metal", bevel=0.01)
    for sx in (-1, 1):
        x = sx * (t / 2 + 0.005)
        B(A, min(x, sx * t / 2), -d - 0.21, 0.04, max(x, sx * t / 2), -0.29, H - 0.04, "jpk_sign")
    for z in (0.06, H - 0.12):
        B(A, -0.03, -0.27, z, 0.03, 0.0, z + 0.06, "jpk_metal")
    B(A, -0.06, -d - 0.3, H, 0.06, -0.2, H + 0.05, "jpk_metal")
    return A, [(-t / 2, -d - 0.25, 0, t / 2, 0, H)], {"MOUNT": (0, 0, 0)}


def p_sign_fascia(W=3.6):
    A = Acc()
    h = 0.6
    B(A, 0, -0.18, 0, W, 0.0, h, "jpk_metal", bevel=0.01)
    B(A, 0.04, -0.185, 0.04, W - 0.04, -0.17, h - 0.04, "jpk_sign")
    return A, [(0, -0.18, 0, W, 0, h)], {"R": (W, 0, 0)}


def p_awning(W=2.7):
    """Fixed sloping canvas awning on a steel frame, with a straight valance; cloth in the accent slot."""
    A = Acc()
    out, drop = 1.1, 0.45
    pts = [(0, 0.0), (out, -drop)]  # (y outward, z)
    for xx in (0.05, W / 2, W - 0.05):
        cyl(A, (xx, 0.0, -0.02), (xx, -out, -drop - 0.02), 0.015, "jpk_metal", seg=6)
        cyl(A, (xx, 0.0, -0.6), (xx, -out * 0.7, -drop * 0.7 - 0.03), 0.012, "jpk_metal", seg=6)
    # canvas: slightly bellied between bars (3 segments)
    nx, ny = 12, 4
    vv = []
    for j in range(ny + 1):
        t = j / ny
        vv.append([Vector((W * i / nx, -out * t,
                           -drop * t - 0.03 * math.sin(math.pi * ((W * i / nx / (W / 2)) % 1.0)) * math.sin(math.pi * t)))
                   for i in range(nx + 1)])
    grid_shell(A, vv, Vector((0, 0, -0.012)), "jpk_accent")
    B(A, 0, -out - 0.012, -drop - 0.25, W, -out, -drop + 0.01, "jpk_accent")  # valance
    B(A, 0, -0.06, -0.05, W, 0.0, 0.03, "jpk_metal")  # wall rail
    del pts
    return A, [(0, -out, -drop - 0.25, W, 0, 0.03)], {"R": (W, 0, 0), "MOUNT": (0, 0, 0)}


def p_ac_unit():
    A = Acc()
    _ac(A, 0, -0.42, 0.0)
    B(A, -0.02, -0.45, -0.04, 0.82, 0.0, 0.0, "jpk_metal")  # wall bracket shelf
    for xx in (0.05, 0.71):
        B(A, xx, -0.42, -0.35, xx + 0.04, 0.0, -0.04, "jpk_metal")
    return A, [(0, -0.45, -0.35, 0.82, 0, 0.6)], {"MOUNT": (0, 0, 0)}


def p_vending():
    A = Acc()
    w, d, h = 1.0, 0.75, 1.85
    B(A, 0, 0.0, 0.05, w, d, h, "jpk_accent", bevel=0.02)
    B(A, 0.03, 0.03, 0.0, w - 0.03, d - 0.03, 0.05, "jpk_dark")
    B(A, 0.04, -0.02, 0.1, w - 0.04, 0.0, h - 0.08, "jpk_sign_vend")
    B(A, 0.08, -0.06, 0.18, 0.66, -0.02, 0.35, "jpk_dark")  # pick-up flap
    B(A, -0.02, -0.03, h - 0.03, w + 0.02, d + 0.02, h + 0.02, "jpk_metal")
    return A, [(0, -0.06, 0, w, d, h + 0.02)], {"BASE": (0, 0, 0)}


def p_pole():
    """Concrete utility pole (12 m) with two cross-arms, insulators, a pole-top transformer and step bolts."""
    A = Acc()
    A.lathe([(0, 0), (0.17, 0), (0.17, 0.05), (0.115, 12.0), (0.0, 12.05)], 12, "jpk_wall_concrete", smooth=True)
    snaps = {}
    for (z, L, tag) in ((11.2, 1.8, "A"), (10.4, 1.5, "B")):
        B(A, -L / 2, -0.06, z, L / 2, 0.06, z + 0.09, "jpk_metal")
        cyl(A, (-L * 0.35, 0, z), (0, 0, z - 0.6), 0.015, "jpk_metal", seg=5)
        cyl(A, (L * 0.35, 0, z), (0, 0, z - 0.6), 0.015, "jpk_metal", seg=5)
        for k, xx in enumerate((-L / 2 + 0.12, -0.25, 0.25, L / 2 - 0.12)):
            A.lathe([(0, 0), (0.035, 0), (0.05, 0.05), (0.03, 0.09), (0.045, 0.12), (0.025, 0.17), (0, 0.18)], 8,
                    "jpk_trim", M=Matrix.Translation((xx, 0, z + 0.09)))
            snaps["WIRE_%s%d" % (tag, k)] = (round(xx, 3), 0.0, round(z + 0.27, 3))
    # transformer can on a bracket, facing the street (-Y)
    B(A, -0.08, -0.5, 8.0, 0.08, -0.1, 8.08, "jpk_metal")
    B(A, -0.08, -0.5, 9.0, 0.08, -0.1, 9.08, "jpk_metal")
    A.lathe([(0, 0), (0.3, 0), (0.3, 0.95), (0.28, 1.0), (0.0, 1.02)], 16, "jpk_trim",
            M=Matrix.Translation((0, -0.5, 7.95)), smooth=True)
    for k in range(3):
        A.lathe([(0, 0), (0.03, 0), (0.03, 0.18), (0, 0.2)], 6, "jpk_dark",
                M=Matrix.Translation((-0.15 + 0.15 * k, -0.55, 8.97)))
    for k in range(10):  # step bolts
        z = 2.5 + 0.45 * k
        ang = math.pi / 2 * (k % 2) + math.pi / 4
        cyl(A, (0.12 * math.cos(ang), 0.12 * math.sin(ang), z), (0.38 * math.cos(ang), 0.38 * math.sin(ang), z),
            0.012, "jpk_metal", seg=5)
    B(A, -0.25, -0.18, 1.6, 0.25, -0.155, 2.2, "jpk_sign")  # pole plate (glyphs)
    snaps["DROP"] = (0.0, -0.5, 8.7)
    return A, [(-0.17, -0.17, 0, 0.17, 0.17, 12.0)], snaps


def p_bicycle():
    A = Acc()
    for y in (-0.52, 0.52):
        torus(A, (0, y, 0.33), 0.31, 0.022, "jpk_dark", seg=20, tseg=5)
        torus(A, (0, y, 0.33), 0.285, 0.008, "jpk_metal", seg=14, tseg=3)
        cyl(A, (-0.03, y, 0.33), (0.03, y, 0.33), 0.03, "jpk_metal", seg=8)
    m = "jpk_accent"
    P = {"rear": (0, -0.52, 0.33), "crank": (0, 0.0, 0.3), "seat": (0, -0.18, 0.82), "head": (0, 0.36, 0.86),
         "front": (0, 0.52, 0.33)}
    for (a, b) in (("rear", "crank"), ("crank", "seat"), ("rear", "seat"), ("crank", "head"), ("head", "front")):
        cyl(A, P[a], P[b], 0.018, m, seg=6)
    cyl(A, (0, -0.2, 0.86), (0, -0.16, 0.76), 0.012, "jpk_metal", seg=5)
    B(A, -0.09, -0.3, 0.86, 0.09, -0.08, 0.92, "jpk_dark", bevel=0.02)  # saddle
    cyl(A, (-0.3, 0.38, 1.0), (0.3, 0.38, 1.0), 0.012, "jpk_metal", seg=6)  # bars
    cyl(A, P["head"], (0, 0.38, 1.0), 0.015, "jpk_metal", seg=6)
    B(A, -0.17, 0.5, 0.8, 0.17, 0.82, 1.06, "jpk_metal")  # front basket (block)
    B(A, -0.15, -0.62, 0.6, 0.15, -0.3, 0.63, "jpk_metal")  # rear rack
    B(A, -0.08, -0.62, 0.32, 0.08, -0.45, 0.6, "jpk_accent")  # mudguard / stand stand-in
    B(A, -0.15, -0.05, 0.27, 0.15, 0.05, 0.33, "jpk_dark")
    return A, [(-0.3, -0.85, 0, 0.3, 0.85, 1.06)], {"BASE": (0, 0, 0)}


def p_planters():
    A = Acc()
    rng = random.Random(5)
    spots = [(0.0, 0.0, 0.22, 0.32), (0.45, 0.05, 0.16, 0.25), (0.8, -0.05, 0.19, 0.42), (0.28, -0.35, 0.13, 0.2),
             (1.12, 0.02, 0.14, 0.22)]
    for (x, y, r, h) in spots:
        A.lathe([(0, 0), (r * 0.75, 0), (r, h), (r * 1.06, h), (r * 1.06, h + 0.03), (r * 0.9, h + 0.03),
                 (0, h + 0.01)], 12, "jpk_trim", M=Matrix.Translation((x, y, 0)), smooth=False)
        for k in range(3):
            bm = bmesh.new()
            bmesh.ops.create_icosphere(bm, subdivisions=1, radius=r * (0.9 + 0.3 * rng.random()))
            for v in bm.verts:
                v.co *= 0.8 + 0.4 * rng.random()
                v.co.z *= 0.8
            bm.transform(Matrix.Translation((x + rng.uniform(-r, r) * 0.5, y + rng.uniform(-r, r) * 0.5,
                                             h + r * 0.6 + 0.15 * k)))
            bm.verts.index_update()
            A._push([tuple(v.co) for v in bm.verts], [tuple(v.index for v in f.verts) for f in bm.faces],
                    "jpk_plant", True)
            bm.free()
    return A, [(-0.25, -0.5, 0, 1.3, 0.25, 0.6)], {"BASE": (0, 0, 0)}


def p_guard_rail(W=2.0):
    A = Acc()
    for xx in (0.04, W - 0.04):
        cyl(A, (xx, 0, 0), (xx, 0, 0.8), 0.03, "jpk_metal", seg=10)
        A.lathe([(0, 0), (0.035, 0), (0.035, 0.02), (0, 0.05)], 10, "jpk_metal",
                M=Matrix.Translation((xx, 0, 0.8)))
    for z in (0.78, 0.45):
        cyl(A, (0, 0, z), (W, 0, z), 0.024, "jpk_metal", seg=10)
    for xx in (W / 3, 2 * W / 3):
        cyl(A, (xx, 0, 0.45), (xx, 0, 0.78), 0.016, "jpk_metal", seg=6)
    return A, [(0, -0.04, 0, W, 0.04, 0.82)], {"R": (W, 0, 0)}


# id: (group, builder, kwargs, triangle budget, note)
PIECES = [
    ("SHOP_GLAZED_36", "ground", p_shop_glazed, dict(W=3.6), 2500, "glazed front, sliding doors, transom"),
    ("SHOP_GLAZED_81", "ground", p_shop_glazed, dict(W=8.1), 2500, "glazed front, sliding doors, transom"),
    ("SHOP_SHUTTER_63", "ground", p_shop_shutter, dict(W=6.3), 2500, "roll-down shutter, box and guides"),
    ("SHOP_KOSHI_54", "ground", p_shop_koshi, dict(W=5.4), 2500, "timber lattice, sliding door, noren"),
    ("SHOP_CORNER_63", "ground", p_shop_corner, dict(W=6.3), 3000, "corner unit, 3.6 m return, chamfer door"),
    ("BAY_WIN_18", "upper", p_bay_win, dict(W=1.8), 1500, "small-tile wall, aluminium sliding window, sill"),
    ("BAY_BALC_27", "upper", p_bay_balc, dict(W=2.7), 2500, "balcony, solid parapet, laundry poles, AC unit"),
    ("BAY_CORR_27", "upper", p_bay_corr, dict(W=2.7), 2500, "open corridor, steel rail, door, meter"),
    ("BAY_BLANK_09", "upper", p_bay_blank, dict(W=0.9), 300, "blank party-wall bay with downpipe"),
    ("BAY_PENCIL_45", "upper", p_bay_pencil, dict(W=4.5), 1500, "pencil-building bay, sign bracket"),
    ("PARAPET_09", "roof", p_parapet, dict(W=0.9), 300, "parapet run with coping"),
    ("PARAPET_CORNER", "roof", p_parapet_corner, {}, 300, "parapet outer corner"),
    ("RAIL_18", "roof", p_rail, dict(W=1.8), 800, "roof guard rail run"),
    ("TANK_M", "roof", p_tank, {}, 1500, "panel water tank on a steel stand"),
    ("PLANT_ROOM_S", "roof", p_plant_room, {}, 1500, "stair / plant room box"),
    ("ANTENNA", "roof", p_antenna, {}, 1500, "TV aerial mast with dish"),
    ("TILE_SLOPE_54_09", "roof", p_tile_slope, dict(W=0.9), 3000, "glazed tile slope strip with eave tile ends"),
    ("RIDGE_09", "roof", p_ridge, dict(W=0.9), 600, "stacked ridge with round cap"),
    ("GABLE_END_54", "roof", p_gable_end, {}, 3000, "gable, barge boards, verge rolls, ridge end"),
    ("HIP_END_54", "roof", p_hip_end, {}, 3000, "hip end pyramid with hip rolls"),
    ("EAVE_PENT_09", "roof", p_eave_pent, dict(W=0.9), 2500, "pent eave strip with rafter ends"),
    ("SIGN_VERT_S", "sign", p_sign_vert, dict(H=1.5), 400, "blade sign 1.5 m"),
    ("SIGN_VERT_M", "sign", p_sign_vert, dict(H=3.0), 400, "blade sign 3 m"),
    ("SIGN_VERT_L", "sign", p_sign_vert, dict(H=5.0), 400, "blade sign 5 m"),
    ("SIGN_FASCIA_36", "sign", p_sign_fascia, dict(W=3.6), 400, "horizontal fascia sign"),
    ("AWNING_27", "sign", p_awning, dict(W=2.7), 1500, "sloping canvas awning"),
    ("AWNING_36", "sign", p_awning, dict(W=3.6), 1500, "sloping canvas awning"),
    ("AC_UNIT", "clutter", p_ac_unit, {}, 800, "wall-bracket AC outdoor unit"),
    ("VENDING", "clutter", p_vending, {}, 800, "drinks vending machine"),
    ("UTILITY_POLE", "clutter", p_pole, {}, 3000, "concrete pole, cross-arms, transformer"),
    ("BICYCLE", "clutter", p_bicycle, {}, 3000, "city bicycle"),
    ("PLANTERS", "clutter", p_planters, {}, 3000, "potted plant group"),
    ("GUARD_RAIL_2", "clutter", p_guard_rail, dict(W=2.0), 800, "steel pipe guard rail run"),
]

# Sign UV rects in the glyph atlas (cells of 1/16): per piece, a column of glyphs
SIGN_RECTS = {"SIGN_VERT_S": (0, 0, 2, 5), "SIGN_VERT_M": (2, 0, 2, 10), "SIGN_VERT_L": (4, 0, 2, 16),
              "SIGN_FASCIA_36": (6, 0, 10, 2), "UTILITY_POLE": (6, 4, 2, 2)}


def sign_uvs(obj, rect):
    """Map every jpk_sign face onto a rect of the glyph atlas by its own bounds (two axes of largest extent)."""
    me = obj.data
    idx = [i for i, m in enumerate(me.materials) if m.name.startswith("jpk_sign") and m.name != "jpk_sign_vend"]
    vidx = [i for i, m in enumerate(me.materials) if m.name == "jpk_sign_vend"]
    uvl = me.uv_layers.active.data
    for p in me.polygons:
        if p.material_index in idx and rect is not None:
            gx, gy, gw, gh = rect
            n = p.normal
            ax = "y" if abs(n.x) > 0.5 else "x"
            co = [me.vertices[me.loops[li].vertex_index].co for li in p.loop_indices]
            a = [getattr(c, ax) for c in co]
            zz = [c.z for c in co]
            a0, a1, z0, z1 = min(a), max(a), min(zz), max(zz)
            for li, c in zip(p.loop_indices, co):
                s = (getattr(c, ax) - a0) / max(a1 - a0, 1e-6)
                if (ax == "y" and n.x < 0) or (ax == "x" and n.y > 0):
                    s = 1 - s
                if ax == "y":
                    s = 1 - s
                t = (c.z - z0) / max(z1 - z0, 1e-6)
                uvl[li].uv = ((gx + s * gw) / 16.0, (gy + t * gh) / 16.0)
        elif p.material_index in vidx:
            co = [me.vertices[me.loops[li].vertex_index].co for li in p.loop_indices]
            for li, c in zip(p.loop_indices, co):
                uvl[li].uv = (c.x / 1.0, (c.z - 0.1) / 1.67)


# =====================================================================================================================
# Build and export
# =====================================================================================================================

def make_piece(pid, fn, kw):
    A, hulls, snaps = fn(**kw)
    used = sorted(set(A.m))
    mats = [MATS[n] for n in used]
    ob = A.to_object(pid, mats)
    ct.planar_uvs(ob, TILES)
    if pid in SIGN_RECTS or any(n.startswith("jpk_sign") for n in used):
        sign_uvs(ob, SIGN_RECTS.get(pid))
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
    bm.to_mesh(ob.data)
    bm.free()
    ucxs = [ucx_box(pid, *h, i=i) for i, h in enumerate(hulls)]
    return ob, ucxs, snaps, used


def build_all(out, only=None):
    os.makedirs(out, exist_ok=True)
    build_materials()
    rows, meshes = [], {}
    for (pid, grp, fn, kw, budget, note) in PIECES:
        if only and pid not in only:
            continue
        ob, ucxs, snaps, used = make_piece(pid, fn, kw)
        path = os.path.join(out, pid + ".glb")
        ct.export([ob] + ucxs, path)
        st = ct.check_glb(path, max_tex=1024)
        if st["errors"]:
            print("building_kit: CHECK", pid, st["errors"])
        rows.append({"id": pid, "group": grp, "note": note,
                     "size_x": st["size_xyz_m"][0], "size_y": st["size_xyz_m"][1], "size_z": st["size_xyz_m"][2],
                     "min": "/".join("%.2f" % v for v in st["min_xyz_m"]), "triangles": st["triangles"],
                     "budget": budget, "over_budget": int(st["triangles"] > budget), "ucx": st["ucx_hulls"],
                     "slots": ";".join(used),
                     "snaps": ";".join("%s:%.3f/%.3f/%.3f" % (k, *v) for k, v in snaps.items()),
                     "file": pid + ".glb", "errors": "|".join(st["errors"])})
        meshes[pid] = (ob, snaps)
        for u in ucxs:
            bpy.data.objects.remove(u, do_unlink=True)
        print("building_kit: %-18s %6d tris (budget %d) %s" % (pid, st["triangles"], budget, st["errors"] or ""))
    if not only:
        with open(os.path.join(out, "pieces.csv"), "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
    td = os.path.join(out, "textures")
    os.makedirs(td, exist_ok=True)
    for fn in TEXFILES:
        src = os.path.join(ct._TMP, fn)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(td, fn))
    return meshes, rows


# =====================================================================================================================
# Test street
# =====================================================================================================================

def srgb(c):
    return tuple(x ** 2.2 for x in c)


_TINTED = {}


def tinted(base, col):
    """A copy of a kit material whose base colour is multiplied by `col` (display sRGB)."""
    key = (base, tuple(round(c, 3) for c in col))
    if key in _TINTED:
        return _TINTED[key]
    src = MATS[base]
    m = src.copy()
    m.name = "%s_t%d" % (base, len(_TINTED))
    nt = m.node_tree
    bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
    lin = srgb(col)
    links = [l for l in nt.links if l.to_socket == bsdf.inputs["Base Color"]]
    if links:
        src_sock = links[0].from_socket
        nt.links.remove(links[0])
        mul = nt.nodes.new("ShaderNodeVectorMath")
        mul.operation = "MULTIPLY"
        mul.inputs[1].default_value = (lin[0] * 1.25, lin[1] * 1.25, lin[2] * 1.25)
        nt.links.new(src_sock, mul.inputs[0])
        nt.links.new(mul.outputs[0], bsdf.inputs["Base Color"])
    else:
        bsdf.inputs["Base Color"].default_value = (*lin, 1)
    _TINTED[key] = m
    return m


def sign_mat(bg, fg, glow=0.0):
    key = ("sign", bg, fg, glow)
    if key in _TINTED:
        return _TINTED[key]
    m = MATS["jpk_sign"].copy()
    m.name = "jpk_sign_t%d" % len(_TINTED)
    nt = m.node_tree
    bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
    img = next(n for n in nt.nodes if n.type == "TEX_IMAGE")
    for l in [l for l in nt.links if l.to_socket == bsdf.inputs["Base Color"]]:
        nt.links.remove(l)
    ma = nt.nodes.new("ShaderNodeVectorMath")
    ma.operation = "MULTIPLY_ADD"
    b, f = srgb(bg), srgb(fg)
    ma.inputs[1].default_value = (f[0] - b[0], f[1] - b[1], f[2] - b[2])
    ma.inputs[2].default_value = b
    nt.links.new(img.outputs["Color"], ma.inputs[0])
    nt.links.new(ma.outputs[0], bsdf.inputs["Base Color"])
    if glow > 0:
        nt.links.new(ma.outputs[0], bsdf.inputs["Emission Color"])
        bsdf.inputs["Emission Strength"].default_value = glow
    _TINTED[key] = m
    return m


class Street:
    def __init__(self, meshes, coll):
        self.meshes = meshes
        self.coll = coll
        self.M = Matrix.Identity(4)

    def put(self, pid, x, y, z, rot=0.0, tint=None, sx=1.0):
        ob0 = self.meshes[pid][0]
        o = bpy.data.objects.new(pid, ob0.data)
        self.coll.objects.link(o)
        o.matrix_world = self.M @ Matrix.Translation((x, y, z)) @ Matrix.Rotation(math.radians(rot), 4, "Z") \
            @ Matrix.Diagonal((sx, 1, 1, 1))
        if tint:
            for i, slot in enumerate(o.material_slots):
                nm = ob0.data.materials[i].name
                for key, val in tint.items():
                    if nm.startswith(key):
                        slot.link = "OBJECT"
                        slot.material = val if isinstance(val, bpy.types.Material) else tinted(nm, val)
                        break
        return o

    def box(self, x0, y0, z0, x1, y1, z1, mat):
        a = Acc()
        a.box(x0, y0, z0, x1, y1, z1, mat.name)
        o = a.to_object("body", [mat], self.coll)
        ct.planar_uvs(o, {mat.name: 2.0})
        o.matrix_world = self.M @ o.matrix_world
        return o


PAL_WALL = {"offwhite": (0.86, 0.84, 0.79), "warmgrey": (0.66, 0.63, 0.58), "charcoal": (0.30, 0.30, 0.31),
            "tilegreen": (0.52, 0.62, 0.55), "tileblue": (0.55, 0.62, 0.70), "beige": (0.80, 0.72, 0.60),
            "brownt": (0.62, 0.50, 0.42), "cream": (0.90, 0.86, 0.74), "slate": (0.48, 0.50, 0.52)}
TIMBER = {"dark": (0.42, 0.30, 0.22), "weathered": (0.60, 0.52, 0.44), "black": (0.22, 0.19, 0.17)}
ACCENTS = {"red": (0.72, 0.12, 0.10), "yellow": (0.92, 0.72, 0.12), "blue": (0.12, 0.28, 0.62),
           "green": (0.15, 0.48, 0.30), "white": (0.92, 0.92, 0.9), "orange": (0.88, 0.42, 0.12)}


def tintset(wall, trim=(0.86, 0.85, 0.82), timber="dark", shutter=(0.75, 0.76, 0.76), accent="red",
            metal=(0.78, 0.78, 0.76), roof=(0.55, 0.57, 0.62), cloth=(0.20, 0.24, 0.36)):
    return {"jpk_wall": PAL_WALL[wall] if isinstance(wall, str) else wall, "jpk_trim": trim,
            "jpk_timber": TIMBER[timber], "jpk_shutter": shutter, "jpk_accent": ACCENTS[accent],
            "jpk_metal": metal, "jpk_rooftile": roof, "jpk_cloth": cloth}


def _walls(t, wall_surface):
    """Swap the wall slot material to the chosen surface (the master would pick it from the array)."""
    col = t["jpk_wall"]
    out = dict(t)
    out["jpk_wall"] = tinted(wall_surface, col)
    return out


def interior_mat(kind, col):
    key = ("int", kind, col)
    if key not in _TINTED:
        m = ct.mat_flat("int_%s_%d" % (kind, len(_TINTED)), col, 0.8)
        if kind == "shop":
            bsdf = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
            bsdf.inputs["Emission Color"].default_value = (*srgb(col), 1)
            bsdf.inputs["Emission Strength"].default_value = 3.0
        _TINTED[key] = m
    return _TINTED[key]


def building(S, x0, lot, rng):
    """Places one building from kit pieces; returns its width."""
    W = lot["w"]
    base_M = S.M
    S.M = base_M @ Matrix.Translation((0, lot.get("setback", 0.0), 0))
    t = _walls(tintset(lot["wall"], **lot.get("tint", {})), lot.get("surface", "jpk_wall_tile"))
    tg = dict(t)
    tg["jpk_wall"] = tinted(lot.get("ground_surface", "jpk_wall_concrete"), lot.get("ground_col", (0.72, 0.71, 0.69)))
    z = 0.0
    x = x0
    for (pid, w) in lot["ground"]:
        S.put(pid, x, 0, 0, tint=tg if pid != "BAY_BLANK_09" else t)
        x += w
    z = SHOP_H if lot["ground"][0][0] != "SHOP_KOSHI_54" else STOREY
    if lot["ground"][0][0] == "BAY_BLANK_09" and lot.get("ground_h"):
        z = lot["ground_h"]
    ground_h = z
    for f in range(lot["floors"]):
        x = x0
        bays = lot["upper"] if not callable(lot["upper"]) else lot["upper"](f)
        for (pid, w) in bays:
            S.put(pid, x, 0, z, tint=t)
            x += w
        z += STOREY
    top = z
    D = lot.get("depth", 9.0)
    wc = PAL_WALL[lot["wall"]] if isinstance(lot["wall"], str) else lot["wall"]
    body_mat = tinted("jpk_wall_concrete", lot.get("side", tuple(0.88 * c for c in wc)))
    S.box(x0 + 0.02, 2.0, 0, x0 + W - 0.02, D, ground_h, body_mat)
    S.box(x0 + 0.02, 0.36, ground_h, x0 + W - 0.02, D, top - 0.01, body_mat)
    for (a, b) in ((x0 + 0.02, x0 + 0.3), (x0 + W - 0.3, x0 + W - 0.02)):  # side walls of the shop storey
        S.box(a, 0.25, 0, b, 2.0, ground_h, body_mat)
    S.box(x0 + 0.3, 0.2, -0.1, x0 + W - 0.3, 2.0, 0.02, interior_mat("floor", (0.35, 0.33, 0.3)))
    shop = interior_mat("shop" if lot.get("lit", True) else "dark",
                        (1.0, 0.92, 0.78) if lot.get("lit", True) else (0.12, 0.12, 0.12))
    S.box(x0 + 0.3, 1.94, 0.02, x0 + W - 0.3, 2.0, ground_h - 0.3, shop)
    zf = ground_h
    while zf < top - 0.5:  # curtains / rooms behind the upper windows, one tone per floor
        tone = rng.choice([(0.86, 0.84, 0.78), (0.78, 0.8, 0.82), (0.9, 0.86, 0.74), (0.3, 0.3, 0.3),
                           (0.6, 0.55, 0.5)])
        S.box(x0 + 0.05, 0.3, zf + 0.2, x0 + W - 0.05, 0.36, zf + STOREY, interior_mat("room", tone))
        zf += STOREY
    roof = lot["roof"]
    if roof in ("gable", "hip"):
        rt = {"jpk_rooftile": lot.get("roofcol", (0.42, 0.45, 0.52)), "jpk_timber": TIMBER[lot.get("timber", "dark")],
              "jpk_wall": tinted("jpk_wall_stucco", (0.9, 0.88, 0.82))}
        n = int(round(W / 0.9))
        x_start, x_end = 0, n
        if roof == "hip":
            x_start, x_end = 3, n - 3
        for k in range(x_start, x_end):
            S.put("TILE_SLOPE_54_09", x0 + k * 0.9, 0, top, tint=rt)
            S.put("TILE_SLOPE_54_09", x0 + (k + 1) * 0.9, 2 * RUN, top, rot=180, tint=rt)
            S.put("RIDGE_09", x0 + k * 0.9, 0, top, tint=rt)
        if roof == "gable":
            S.put("GABLE_END_54", x0, 0, top, tint=rt)
            S.put("GABLE_END_54", x0 + W, 2 * RUN, top, rot=180, tint=rt)
        else:
            S.put("HIP_END_54", x0, 0, top, tint=rt)
            S.put("HIP_END_54", x0 + W, 2 * RUN, top, rot=180, tint=rt)
        if D > 2 * RUN + 0.3:
            S.box(x0 + 0.3, 2 * RUN + 0.3, 0, x0 + W - 0.3, D, top - 1.0, body_mat)
    else:
        n = int(round(W / 0.9))
        pt = {"jpk_wall": t["jpk_wall"], "jpk_trim": (0.8, 0.79, 0.76)}
        for k in range(n):
            S.put("PARAPET_09", x0 + k * 0.9, 0, top, tint=pt)
        nd = int((D - 0.25) / 0.9)
        for k in range(nd):
            S.put("PARAPET_09", x0, 0.25 + (k + 1) * 0.9, top, rot=-90, tint=pt)
            S.put("PARAPET_09", x0 + W, 0.25 + k * 0.9, top, rot=90, tint=pt)
        for k in range(n):
            S.put("PARAPET_09", x0 + (k + 1) * 0.9, 0.25 + nd * 0.9, top, rot=180, tint=pt)
        S.put("PARAPET_CORNER", x0, 0, top, tint=pt)
        S.box(x0 + 0.05, 0.26, top - 0.01, x0 + W - 0.05, D - 0.05, top + 0.03,
              tinted("jpk_wall_concrete", (0.6, 0.6, 0.59)))  # neutral roof deck
        for (pid, dx, dy, rot) in lot.get("roofkit", []):
            S.put(pid, x0 + dx, dy, top, rot=rot, tint={"jpk_trim": (0.82, 0.84, 0.86), "jpk_metal": (0.5, 0.5, 0.5),
                                                       "jpk_wall": tinted("jpk_wall_concrete", (0.7, 0.7, 0.68))})
    for (pid, dx, dy, dz, rot, extra) in lot.get("extras", []):
        tt = dict(t)
        if pid in ("BICYCLE", "VENDING", "PLANTERS", "AC_UNIT", "GUARD_RAIL_2"):
            tt["jpk_accent"] = (0.22, 0.27, 0.33)  # clutter stays off the sign/awning reds and yellows
        tt.update(extra)
        S.put(pid, x0 + dx, dy, dz if dz >= 0 else top + dz, rot=rot, tint=tt)
    S.M = base_M
    return top, ground_h


def lots_near():
    """One street side, about 60 m: nine lots, five types, 2 to 7 storeys."""
    sign = lambda bg, fg: sign_mat(ACCENTS[bg] if isinstance(bg, str) else bg, ACCENTS[fg] if isinstance(fg, str) else fg)  # noqa: E731
    L = []
    # 1 corner shop, 3 floors over, tile wall, awning, vending machines
    L.append(dict(w=6.3, wall="offwhite", surface="jpk_wall_tile", floors=2, gap=0.0,
                  ground=[("SHOP_CORNER_63", 6.3)], upper=[("BAY_WIN_18", 1.8)] * 3 + [("BAY_BLANK_09", 0.9)],
                  roof="flat", roofkit=[("TANK_M", 1.2, 4.0, 0), ("ANTENNA", 5.2, 2.0, 0)],
                  tint=dict(accent="green"),
                  extras=[("SIGN_FASCIA_36", 1.4, -0.08, 2.95, 0, {"jpk_sign": sign("white", "green")}),
                          ("SIGN_FASCIA_36", 3.0, -0.08, 2.95, 0, {"jpk_sign": sign("green", "white")}),
                          ("VENDING", 0.3, -1.6, 0, 90, {"jpk_accent": ACCENTS["white"]}),
                          ("VENDING", 0.3, -0.55, 0, 90, {"jpk_accent": ACCENTS["blue"]}),
                          ("AC_UNIT", 1.5, 0.0, 4.6, 0, {}), ("AC_UNIT", 3.3, 0.0, 7.6, 0, {})]))
    # 2 pencil building, 6 floors, blade sign
    L.append(dict(w=4.5, wall="charcoal", surface="jpk_wall_tile", floors=6, gap=0.25,
                  ground=[("SHOP_GLAZED_36", 3.6), ("BAY_BLANK_09", 0.9)], upper=[("BAY_PENCIL_45", 4.5)],
                  roof="flat", roofkit=[("ANTENNA", 1.0, 3.0, 0), ("PLANT_ROOM_S", 1.6, 4.5, 0)],
                  extras=[("SIGN_VERT_L", 4.05, 0.0, 9.9, 0, {"jpk_sign": sign("red", "white")}),
                          ("SIGN_VERT_M", 4.05, 0.0, 15.6, 0, {"jpk_sign": sign("yellow", "blue")}),
                          ("SIGN_VERT_S", 4.05, 0.0, 4.2, 0, {"jpk_sign": sign("white", "red")}),
                          ("SIGN_VERT_S", 4.05, 0.0, 7.2, 0, {"jpk_sign": sign("green", "white")}),
                          ("SIGN_VERT_S", 4.05, 0.0, 19.2, 0, {"jpk_sign": sign("white", "blue")}),
                          ("SIGN_FASCIA_36", 0.0, -0.08, 2.95, 0, {"jpk_sign": sign("blue", "white")}),
                          ("AWNING_36", 0.0, 0.0, 2.9, 0, {"jpk_accent": ACCENTS["white"]}),
                          ("BICYCLE", 1.0, -2.0, 0, 80, {"jpk_accent": (0.25, 0.3, 0.45)})]))
    # 3 walk-up with open corridors, 4 floors, shuttered shop
    L.append(dict(w=8.1, wall="warmgrey", surface="jpk_wall_concrete", floors=4, gap=0.4,
                  ground=[("SHOP_SHUTTER_63", 6.3), ("BAY_BLANK_09", 0.9), ("BAY_BLANK_09", 0.9)],
                  upper=[("BAY_CORR_27", 2.7)] * 3, roof="flat",
                  roofkit=[("RAIL_18", 0.5, 1.2, 0), ("RAIL_18", 2.3, 1.2, 0), ("TANK_M", 5.0, 4.5, 0)],
                  tint=dict(shutter=(0.62, 0.66, 0.70), trim=(0.84, 0.83, 0.80)),
                  extras=[("SIGN_FASCIA_36", 1.4, -0.32, 2.95, 0, {"jpk_sign": sign("yellow", "red")}),
                          ("SIGN_VERT_M", 7.75, 0.0, 4.2, 0, {"jpk_sign": sign("white", "red")}),
                          ("PLANTERS", 6.75, -0.6, 0, 0, {}), ("BICYCLE", 7.4, -1.6, 0, 100, {}),
                          ("BICYCLE", 6.8, -1.7, 0, 95, {"jpk_accent": (0.6, 0.6, 0.62)})]))
    # 4 timber town house, 2 storeys, gable tile roof, pent eave
    L.append(dict(w=5.4, wall="cream", surface="jpk_wall_stucco", floors=1, gap=0.3, depth=5.4,
                  ground=[("SHOP_KOSHI_54", 5.4)], upper=[("BAY_WIN_18", 1.8)] * 3, roof="gable",
                  roofcol=(0.36, 0.38, 0.44), ground_surface="jpk_wall_stucco", ground_col=(0.9, 0.87, 0.8),
                  tint=dict(timber="dark", cloth=(0.13, 0.17, 0.32)),
                  extras=[("EAVE_PENT_09", 0.9 * k, 0, 3.0, 0, {"jpk_rooftile": (0.36, 0.38, 0.44)}) for k in range(6)]
                  + [("PLANTERS", 0.3, -0.4, 0, 0, {})]))
    # 5 tiled apartment block, 6 floors, balconies
    L.append(dict(w=10.8, wall="tilegreen", surface="jpk_wall_tile", floors=6, gap=0.5,
                  ground=[("SHOP_GLAZED_81", 8.1), ("BAY_BLANK_09", 0.9), ("BAY_BLANK_09", 0.9), ("BAY_BLANK_09", 0.9)],
                  upper=[("BAY_BALC_27", 2.7)] * 4, roof="flat",
                  roofkit=[("PLANT_ROOM_S", 1.0, 3.5, 0), ("TANK_M", 6.0, 4.0, 0), ("ANTENNA", 9.5, 2.0, 0)],
                  extras=[("AWNING_27", 0.3 + 2.7 * k, 0.0, 2.85, 0, {"jpk_accent": ACCENTS["orange"]})
                          for k in range(3)]
                  + [("SIGN_FASCIA_36", 0.6, -0.08, 2.95, 0, {"jpk_sign": sign("white", "blue")}),
                     ("SIGN_FASCIA_36", 4.2, -0.08, 2.95, 0, {"jpk_sign": sign("white", "blue")})]))
    # 6 small office / shop, 2 storeys, flat, warm grey stucco
    L.append(dict(w=7.2, wall="beige", surface="jpk_wall_stucco", floors=2, gap=0.0,
                  ground=[("SHOP_GLAZED_36", 3.6), ("SHOP_GLAZED_36", 3.6)],
                  upper=[("BAY_WIN_18", 1.8)] * 4, roof="flat", roofkit=[("RAIL_18", 2.7, 1.0, 0)],
                  extras=[("AWNING_36", 0.0, 0.0, 2.9, 0, {"jpk_accent": ACCENTS["red"]}),
                          ("SIGN_FASCIA_36", 3.6, -0.08, 2.95, 0, {"jpk_sign": sign("blue", "yellow")}),
                          ("AC_UNIT", 0.1, 0.0, 4.4, 0, {}), ("AC_UNIT", 5.5, 0.0, 7.4, 0, {})]))
    # 7 narrow pencil, 7 floors, other sign stack
    L.append(dict(w=4.5, wall="tileblue", surface="jpk_wall_tile", floors=6, gap=0.2,
                  ground=[("BAY_BLANK_09", 0.9), ("SHOP_GLAZED_36", 3.6)], upper=[("BAY_PENCIL_45", 4.5)],
                  roof="flat", roofkit=[("TANK_M", 0.6, 3.0, 0)],
                  extras=[("SIGN_VERT_L", 4.05, 0.0, 4.0, 0, {"jpk_sign": sign("yellow", "red")}),
                          ("SIGN_VERT_S", 4.05, 0.0, 9.6, 0, {"jpk_sign": sign("white", "blue")}),
                          ("SIGN_VERT_S", 4.05, 0.0, 12.6, 0, {"jpk_sign": sign("blue", "white")}),
                          ("SIGN_VERT_M", 4.05, 0.0, 15.6, 0, {"jpk_sign": sign("red", "white")}),
                          ("AWNING_36", 0.9, 0.0, 2.9, 0, {"jpk_accent": ACCENTS["blue"]})]))
    # 8 hip-roofed town house
    L.append(dict(w=7.2, wall="offwhite", surface="jpk_wall_stucco", floors=1, gap=0.6, depth=5.4,
                  ground=[("SHOP_KOSHI_54", 5.4), ("BAY_BLANK_09", 0.9), ("BAY_BLANK_09", 0.9)],
                  upper=[("BAY_WIN_18", 1.8)] * 4, roof="hip", roofcol=(0.30, 0.31, 0.33),
                  ground_surface="jpk_wall_stucco", ground_col=(0.88, 0.86, 0.8),
                  tint=dict(timber="weathered", cloth=(0.55, 0.12, 0.1)),
                  extras=[("EAVE_PENT_09", 0.9 * k, 0, 3.0, 0, {"jpk_rooftile": (0.30, 0.31, 0.33)}) for k in range(8)]
                  + [("BICYCLE", 6.3, -1.4, 0, 95, {"jpk_accent": (0.75, 0.75, 0.72)})]))
    # 9 walk-up end, 3 floors
    L.append(dict(w=5.4, wall="brownt", surface="jpk_wall_tile", floors=3, gap=0.0,
                  ground=[("BAY_BLANK_09", 0.9), ("SHOP_GLAZED_36", 3.6), ("BAY_BLANK_09", 0.9)],
                  upper=[("BAY_CORR_27", 2.7)] * 2, roof="flat", roofkit=[("PLANT_ROOM_S", 2.0, 4.0, 0)],
                  extras=[("SIGN_FASCIA_36", 0.9, -0.08, 2.95, 0, {"jpk_sign": sign("orange", "white")})]))
    return L


def street_side(S, lots, rng):
    x = 0.0
    tops = []
    for lot in lots:
        x += lot.get("gap", 0.0)
        top, _ = building(S, x, lot, rng)
        tops.append((x, lot["w"], top))
        x += lot["w"]
    return x, tops


def wire(coll, p0, p1, sag=0.35, r=0.011, name="wire"):
    cu = bpy.data.curves.new(name, "CURVE")
    cu.dimensions = "3D"
    cu.bevel_depth = r
    cu.bevel_resolution = 1
    sp = cu.splines.new("POLY")
    n = 12
    sp.points.add(n)
    for i in range(n + 1):
        t = i / n
        p = Vector(p0).lerp(Vector(p1), t)
        p.z -= sag * 4 * t * (1 - t)
        sp.points[i].co = (p.x, p.y, p.z, 1)
    o = bpy.data.objects.new(name, cu)
    coll.objects.link(o)
    return o


def build_street(meshes, out):
    coll = bpy.data.collections.new("street")
    bpy.context.scene.collection.children.link(coll)
    for ob, _ in meshes.values():
        ob.hide_render = True
        ob.hide_viewport = True
    rng = random.Random(3)
    # preview only (the glbs are already written): see-through glass so the interiors read
    g = bpy.data.materials.get("jpk_glass")
    if g:
        gb = next(n for n in g.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
        gb.inputs["Transmission Weight"].default_value = 0.8
        gb.inputs["Roughness"].default_value = 0.02
    S = Street(meshes, coll)
    near = lots_near()
    for lot, sb in zip(near, (0, 0.3, 0, 0.45, 0, 0.15, 0, 0.4, 0.1)):
        lot["setback"] = sb
    length, tops_n = street_side(S, near, rng)
    SW = 9.0  # facade to facade
    # opposite side: other order, other walls and heights, turned to face +Y
    far = lots_near()
    order = [4, 1, 8, 6, 3, 0, 7, 2, 5]
    far = [far[i] for i in order]
    walls = ["warmgrey", "cream", "slate", "offwhite", "tileblue", "beige", "charcoal", "tilegreen", "offwhite"]
    for lot, wl, df, sb in zip(far, walls, (-2, 1, -1, 0, 1, -1, 0, 0, 1), (0.2, 0, 0.4, 0, 0.1, 0, 0.35, 0, 0)):
        lot["wall"] = wl
        lot["setback"] = sb
        if lot["roof"] == "flat":
            lot["floors"] = max(1, lot["floors"] + df)
    S.M = Matrix.Translation((length, -SW, 0)) @ Matrix.Rotation(math.pi, 4, "Z")
    street_side(S, far, rng)
    S.M = Matrix.Identity(4)
    # ground: road, footways, kerbs, gutters
    road = ct.mat_flat("road", (0.34, 0.34, 0.35), 0.85)
    foot = ct.mat_flat("foot", (0.55, 0.54, 0.51), 0.85)
    line = ct.mat_flat("line", (0.85, 0.85, 0.82), 0.6)
    S.box(-30, -SW - 30, -0.3, length + 30, 30, -0.12, road)
    for (y0, y1) in ((-1.6, 1.2), (-SW - 1.2, -SW + 1.6)):  # footways with a low kerb
        S.box(-30, y0, -0.3, length + 30, y1, 0.0, foot)
    for y in (-1.75, -SW + 1.75):  # white edge lines (no centre line on a narrow street)
        S.box(-30, y - 0.07, -0.13, length + 30, y + 0.07, -0.115, line)
    # poles: near side at the footway edge, every ~25 m, with wires between their snaps and drops to buildings
    pole_x = [3.0, 28.5, 54.0]
    psn = meshes["UTILITY_POLE"][1]
    poles = []
    for px in pole_x:
        S.put("UTILITY_POLE", px, -1.35, 0, rot=0, tint={"jpk_trim": (0.6, 0.62, 0.62),
                                                         "jpk_sign": sign_mat((0.9, 0.9, 0.85), (0.1, 0.1, 0.1))})
        poles.append(px)
    for (a, b) in zip(poles, poles[1:]):
        for i, (k, v) in enumerate(sorted(psn.items())):
            if k.startswith("WIRE"):
                wire(coll, (a + v[0], -1.35 + v[1], v[2]), (b + v[0], -1.35 + v[1], v[2]), sag=0.3 + 0.07 * i,
                     r=0.016)
        for (z, r, sag) in ((6.6, 0.035, 0.45), (6.3, 0.022, 0.6), (5.9, 0.018, 0.7)):  # telecom bundle
            wire(coll, (a, -1.55, z), (b, -1.55, z), sag=sag, r=r)
        S.put("AC_UNIT", (a + b) / 2 - 0.4, -1.4, 6.2, rot=0, tint={"jpk_metal": (0.35, 0.36, 0.36)})
    for (px, tx, tz) in ((3.0, 2.0, 6.2), (28.5, 26.0, 9.0), (28.5, 33.0, 4.5), (54.0, 52.0, 3.4)):
        d = psn["DROP"]
        wire(coll, (px + d[0], -1.35 + d[1], d[2]), (tx, -0.05, tz), sag=0.4, r=0.008)
    for px in (14.0, 42.0):  # far-side poles (shorter run, cross-street wires)
        S.put("UTILITY_POLE", length - px, -SW + 1.35, 0, rot=180,
              tint={"jpk_trim": (0.6, 0.62, 0.62), "jpk_sign": sign_mat((0.9, 0.9, 0.85), (0.1, 0.1, 0.1))})
    for k in range(4):
        wire(coll, (length - 14.0 - 0.7 + 0.45 * k, -SW + 1.35, 11.47),
             (length - 42.0 - 0.7 + 0.45 * k, -SW + 1.35, 11.47), sag=0.5 + 0.05 * k, r=0.016)
    for (z, r) in ((6.5, 0.03), (6.1, 0.02)):
        wire(coll, (length - 14.0, -SW + 1.55, z), (length - 42.0, -SW + 1.55, z), sag=0.6, r=r)
        wire(coll, (length - 14.0, -SW + 1.55, z), (length + 8.0, -SW + 1.55, z), sag=0.5, r=r)
        wire(coll, (length - 42.0, -SW + 1.55, z), (-8.0, -SW + 1.55, z), sag=0.7, r=r)
    for k in range(3):
        wire(coll, (-8.0, -1.35, 11.1 - 0.4 * k), (3.0, -1.35, 11.47 - 0.8 * k), sag=0.2, r=0.016)
    wire(coll, (28.5 + 0.78, -1.35, 11.4), (length - 42.0, -SW + 1.35, 11.4), sag=0.6)
    for gx in (10.0, 12.0, 36.0, 38.0, 40.0):
        S.put("GUARD_RAIL_2", gx, -1.45, 0, tint={"jpk_metal": (0.92, 0.92, 0.9)})
    wm = ct.mat_flat("wire", (0.03, 0.03, 0.03), 0.5)
    for o in coll.objects:
        if o.type == "CURVE":
            o.data.materials.append(wm)
    return length, SW


def render_street(out, length, SW, tag=""):
    scene = bpy.context.scene
    ct.setup_render(scene, 1280, 720, samples=24)
    try:
        scene.cycles.device = "CPU"
        scene.render.threads_mode = "AUTO"
    except Exception:
        pass
    ct.sky_and_sun(scene, sky=(0.50, 0.62, 0.80), strength=1.0, sun=4.2, elev=36, azim=-25)
    pv = os.path.join(out, "previews")
    os.makedirs(pv, exist_ok=True)
    shots = {
        "street_eye": ((-1.0, -SW + 2.6, 1.65), (30.0, -2.0, 7.0), 24),
        "street_across": ((length * 0.5 + 3, -6.3, 1.7), (length * 0.5 + 3, 0.0, 5.5), 14),
        "street_above": ((length * 0.62, -SW - 16.0, 34.0), (length * 0.58, 3.0, 4.0), 22),
    }
    for name, (loc, tgt, lens) in shots.items():
        ct.camera(scene, loc, tgt, lens=lens)
        ct.render(scene, os.path.join(pv, name + tag + ".png"))


def render_sheets(meshes, rows, out, tag="", lights=False):
    """One contact sheet per group: every piece in a row, untinted, 3/4 orthographic view from the front."""
    scene = bpy.context.scene
    if lights:  # not already set up by render_street
        ct.setup_render(scene, 1600, 700, samples=24)
        try:
            scene.cycles.device = "CPU"
        except Exception:
            pass
        ct.sky_and_sun(scene, sky=(0.50, 0.62, 0.80), strength=1.0, sun=4.2, elev=36, azim=-25)
    st = bpy.data.collections.get("street")
    if st:
        st.hide_render = True
    scene.render.resolution_x, scene.render.resolution_y = 1600, 700
    ground = ct.mat_flat("sheet_ground", (0.5, 0.5, 0.5), 0.9)
    for grp in ("ground", "upper", "roof", "sign", "clutter"):
        coll = bpy.data.collections.new("sheet_" + grp)
        scene.collection.children.link(coll)
        S = Street(meshes, coll)
        x, hz = 0.0, 0.0
        for r in rows:
            if r["group"] != grp:
                continue
            vs = [v.co for v in meshes[r["id"]][0].data.vertices]
            mnx, mxx = min(v.x for v in vs), max(v.x for v in vs)
            mnz, mxz = min(v.z for v in vs), max(v.z for v in vs)
            S.put(r["id"], x - mnx, 0, -mnz)
            x += (mxx - mnx) + 0.7
            hz = max(hz, mxz - mnz)
        S.box(-3, -8, -0.05, x + 3, 10, 0.0, ground)
        cx, d = x * 0.5, max(x, hz * 2.3)
        ct.camera(scene, (cx - 0.35 * d, -1.6 * d, hz * 0.5 + 0.8 * d), (cx, 0.0, hz * 0.4), lens=50)
        scene.camera.data.type = "ORTHO"
        scene.camera.data.ortho_scale = max(x * 1.15, hz * 2.6)
        ct.render(scene, os.path.join(out, "previews", "kit_%s%s.png" % (grp, tag)))
        coll.hide_render = True


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    out = argv[0] if argv else os.path.join(os.getcwd(), "kit_out")
    only = None
    if "--only" in argv:
        only = set(argv[argv.index("--only") + 1].split(","))
    ct.fresh()
    meshes, rows = build_all(out, only)
    tot = sum(r["triangles"] for r in rows)
    print("building_kit: %d pieces, %d triangles total" % (len(rows), tot))
    if "--previews" in argv and not only:
        length, SW = build_street(meshes, out)
        tag = argv[argv.index("--tag") + 1] if "--tag" in argv else ""
        if "--sheets-only" not in argv:
            render_street(out, length, SW, tag=tag)
        render_sheets(meshes, rows, out, tag=tag, lights="--sheets-only" in argv)


if __name__ == "__main__":
    main()
