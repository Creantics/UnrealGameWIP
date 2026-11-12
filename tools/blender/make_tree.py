"""Procedural trees for the city's streets and parks, made headless in Blender, ready for FTImportPlant.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_tree.py -- \
        --species broadleaf|ornamental|park|pine|cherry|cherry_old --variant a|b --out <dir> [--seed N] [--sheet] [--no-billboard]
    blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_tree.py -- --grate --out <dir>

Species (our own presets, no real product or library copied):
  broadleaf   street tree, 7-9 m (the street rows), clear trunk about 2.5 m, ovoid crown
  ornamental  small tree, 4-6 m, short trunk, round crown, small light leaves
  park        park tree, 9-11 m, short trunk forking into a wide spreading dome
  pine        conifer for slopes, 11-14 m, straight stem, whorled layered branches, needle tufts
  cherry      young street cherry, 5-7 m, upright vase form, banded bark; blossom cards (<name>_bloom.png)
  cherry_old  old park cherry, 8-10 m, crown wider than tall on low heavy limbs, dark banded bark; blossom cards

The skeleton grows by space colonisation (attraction points in a crown envelope; the pine by whorls), the limbs'
radii by the pipe model, the trunk gets a lobed root flare sunk 0.15 m under the origin (the trunk's foot). Every limb
thicker than an LOD's cut-off is a real tube with bark UVs; thinner twigs live in the leaf texture. Leaf cards are folded,
drooped quads carrying a leaf-cluster atlas drawn by this script (leaf shapes, midrib and veins, per-leaf colour,
alpha with the colour dilated under its edge), their normals taken mostly from the crown's envelope (the usual
crown-shading trick: the cards light as one canopy, never as flat rectangles).

Writes into <out>, for TREE-<species>-<variant>:
  <name>.glb            nodes <name>_LOD0..LOD2 (bark + leaf), <name>_LOD3 (three crossed billboard cards) and
                        UCX_<name>_LOD0_00 (a trunk-only convex hull); materials Bark, Leaf, Billboard (no images)
  <name>_bark.png, <name>_bark_n.png, <name>_leaf.png (RGBA), <name>_leaf_n.png, <name>_bb.png (RGBA): square, 1024 px
                        at most; normal maps in the engine's convention (green down)
  <name>_leaf_m.png     masks: R leaf, G twig (also under the leaves: what winter keeps), B per-leaf id
  <name>_bloom.png      (cherries) blossom cards in the leaf atlas' cells: flower clusters, a few young leaves
  <name>_bb.png         the billboard: a 2 x 2 atlas of the tree leafy (top left), bare (top right), in blossom (bottom
                        left; leafy for the others)
  <name>.json           heights, crown, trunk, triangle counts per LOD, season palette (the importer reads them)
  <name>_sheet.png      (--sheet) near (10 m, LOD0) and far (80 m, LOD2) on neutral grey, Cycles
Wind lives in the material (it reads height and distance from the trunk), so the mesh carries no vertex colour.
"""
import json
import math
import os
import random
import sys
import time

import bpy
import numpy as np
from mathutils import Matrix, Vector

# ----------------------------------------------------------------------------------------------- species presets
SPECIES = {
    "broadleaf": dict(
        height=(7.2, 8.0), crown_base=(2.4, 2.7), crown_rx=(2.6, 3.0), crown_shape="ovoid",
        attractors=1400, step=0.26, influence=1.9, kill=0.44, tropism=0.12, trunk_r=0.14, lean=0.04,
        leaf_r=0.020, cards=1300, card_size=(0.95, 1.25), fold=0.22, droop=0.10,
        leaf=dict(color=(84, 104, 54), length=(0.11, 0.15), width=0.50, veins=7, tip=1.0, side_twigs=4),
        bark=dict(color=(98, 90, 80), fissure=0.55, stretch=5.0),
        lod_r=(0.012, 0.035, 0.08), lod_cards=(1.0, 0.42, 0.2),
        season=dict(autumn=((196, 142, 44), (172, 84, 30)), twig=(84, 76, 66), spring=(1.12, 1.18, 0.78)),
    ),
    "ornamental": dict(
        height=(4.4, 5.6), crown_base=(1.3, 1.6), crown_rx=(2.0, 2.5), crown_shape="round",
        attractors=1000, step=0.20, influence=1.5, kill=0.34, tropism=0.06, trunk_r=0.10, lean=0.06,
        leaf_r=0.016, cards=800, card_size=(0.6, 0.8), fold=0.25, droop=0.12,
        leaf=dict(color=(104, 116, 60), length=(0.085, 0.115), width=0.56, veins=5, tip=0.7, side_twigs=5),
        bark=dict(color=(112, 104, 96), fissure=0.25, stretch=2.5),
        lod_r=(0.010, 0.028, 0.06), lod_cards=(1.0, 0.42, 0.2),
        season=dict(autumn=((204, 92, 40), (150, 40, 38)), twig=(96, 86, 78), spring=(1.10, 1.16, 0.82)),
    ),
    "park": dict(
        height=(9.0, 10.2), crown_base=(2.4, 2.7), crown_rx=(5.0, 5.6), crown_shape="dome",
        attractors=2400, step=0.32, influence=2.4, kill=0.54, tropism=-0.02, trunk_r=0.28, lean=0.03,
        leaf_r=0.026, cards=2100, card_size=(1.1, 1.45), fold=0.20, droop=0.12,
        leaf=dict(color=(72, 92, 50), length=(0.12, 0.16), width=0.48, veins=8, tip=1.0, side_twigs=4),
        bark=dict(color=(82, 74, 66), fissure=0.75, stretch=4.0),
        lod_r=(0.015, 0.045, 0.11), lod_cards=(1.0, 0.40, 0.2),
        season=dict(autumn=((182, 128, 46), (128, 72, 32)), twig=(70, 62, 56), spring=(1.14, 1.20, 0.76)),
    ),
    "pine": dict(
        height=(11.0, 13.6), crown_base=(3.2, 4.2), crown_rx=(2.6, 3.4), crown_shape="cone",
        trunk_r=0.22, lean=0.05, leaf_r=0.02, cards=1000, card_size=(0.70, 0.95), fold=0.10, droop=0.05,
        leaf=dict(color=(54, 72, 50), needles=True, length=(0.10, 0.15), count=4),
        bark=dict(color=(108, 82, 64), fissure=0.85, stretch=1.6, plates=True),
        lod_r=(0.012, 0.035, 0.08), lod_cards=(1.0, 0.42, 0.2),
        season=dict(autumn=((60, 74, 50), (66, 76, 52)), twig=(92, 72, 58), spring=(1.04, 1.08, 0.94), evergreen=True),
    ),
}
SPECIES["cherry"] = dict(
    height=(5.4, 6.6), crown_base=(1.9, 2.3), crown_rx=(1.9, 2.3), crown_shape="vase",
    attractors=1100, step=0.22, influence=1.6, kill=0.36, tropism=0.24, trunk_r=0.11, lean=0.03,
    leaf_r=0.016, cards=950, card_size=(0.62, 0.82), fold=0.22, droop=0.08,
    leaf=dict(color=(86, 108, 56), length=(0.09, 0.12), width=0.46, veins=8, tip=1.0, side_twigs=4, twig=(70, 52, 48)),
    bark=dict(color=(84, 60, 54), fissure=0.12, stretch=1.2, bands=0.6),
    lod_r=(0.010, 0.028, 0.06), lod_cards=(1.0, 0.42, 0.2),
    bloom=dict(petal=(246, 214, 222), leaf_share=0.10, flowers=(3, 5), size=0.20),
    season=dict(autumn=((214, 106, 44), (176, 52, 40)), twig=(70, 54, 50), spring=(1.08, 1.14, 0.82)),
)
SPECIES["cherry_old"] = dict(
    height=(8.2, 9.6), crown_base=(1.5, 1.9), crown_rx=(5.6, 6.4), crown_shape="spread",
    attractors=2600, step=0.34, influence=2.6, kill=0.58, tropism=-0.10, trunk_r=0.36, lean=0.08,
    leaf_r=0.024, cards=2300, card_size=(1.0, 1.3), fold=0.20, droop=0.14,
    leaf=dict(color=(80, 102, 54), length=(0.10, 0.13), width=0.46, veins=8, tip=1.0, side_twigs=4, twig=(62, 46, 44)),
    bark=dict(color=(60, 44, 42), fissure=0.35, stretch=1.4, bands=1.0),
    lod_r=(0.015, 0.045, 0.10), lod_cards=(1.0, 0.40, 0.2),
    bloom=dict(petal=(244, 208, 218), leaf_share=0.06, flowers=(4, 6), size=0.20),
    season=dict(autumn=((206, 96, 42), (160, 44, 38)), twig=(62, 48, 46), spring=(1.08, 1.14, 0.82)),
)
LOD_SCREEN = (1.0, 0.45, 0.20, 0.06)  # what the importer sets; the sheet's far shot uses the LOD2 they imply at 80 m


def args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    o = dict(species="broadleaf", variant="a", out=None, seed=None, sheet=False, billboard=True, grate=False, tex=1024)
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("--species", "--variant", "--out", "--seed", "--tex") and i + 1 < len(argv):
            o[a[2:]] = argv[i + 1]
            i += 1
        elif a == "--sheet":
            o["sheet"] = True
        elif a == "--no-billboard":
            o["billboard"] = False
        elif a == "--grate":
            o["grate"] = True
        else:
            raise SystemExit("make_tree: unknown argument %s" % a)
        i += 1
    if not o["out"]:
        raise SystemExit("make_tree: pass --out <dir>")
    if not o["grate"] and o["species"] not in SPECIES:
        raise SystemExit("make_tree: --species is one of %s" % ", ".join(SPECIES))
    o["tex"] = int(o["tex"])
    if o["seed"] is None:
        o["seed"] = (sorted(SPECIES).index(o["species"]) * 101 + (ord(o["variant"][0]) - 96) * 7919) if not o["grate"] else 1
    o["seed"] = int(o["seed"])
    return o


def rng_range(r, lohi):
    return lohi[0] + (lohi[1] - lohi[0]) * r.random()


# ----------------------------------------------------------------------------------------------- textures (numpy)
def srgb_to_lin(c):
    c = np.asarray(c, dtype=np.float64)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def lin_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


def fft_noise(n, rng, beta=2.0, sx=1.0, sy=1.0):
    """Periodic noise (tiles seamlessly): white noise shaped by 1/f^beta, stretched by sx, sy (larger = longer)."""
    white = rng.standard_normal((n, n))
    fy = np.fft.fftfreq(n)[:, None] * sy
    fx = np.fft.fftfreq(n)[None, :] * sx
    f = np.sqrt(fx * fx + fy * fy)
    f[0, 0] = 1.0
    spec = np.fft.fft2(white) / f ** (beta / 2.0)
    spec[0, 0] = 0.0
    out = np.real(np.fft.ifft2(spec))
    out -= out.mean()
    return out / (out.std() + 1e-9)


def normal_from_height(h, strength):
    """Engine-convention normal map (x right, green down) from a periodic height field (rows down)."""
    dx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * 0.5 * strength
    dy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * 0.5 * strength
    n = np.stack([-dx, dy, np.ones_like(h)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n * 0.5 + 0.5


def bark_maps(spec, n, seed):
    rng = np.random.default_rng(seed)
    st = spec["stretch"]
    base = fft_noise(n, rng, 2.2, 1.0, st)            # long vertical ridges
    fine = fft_noise(n, rng, 1.4, 1.0, 1.5)
    if spec.get("plates"):
        plates = fft_noise(n, rng, 2.6, 1.2, 1.2)
        ridge = 1.0 - np.abs(np.tanh(plates * 1.6))      # dark cracks between flaky plates
        h = 0.65 * (1.0 - ridge) + 0.25 * np.tanh(base) + 0.1 * fine
    else:
        ridge = 1.0 - np.abs(np.tanh(base * 1.3))         # 1 in a fissure
        h = (1.0 - spec["fissure"] * ridge) + 0.12 * fine
    bands = spec.get("bands", 0.0)
    lent = None
    if bands:
        # horizontal banding and lenticel dashes (stretched across the trunk), the vertical fissures kept faint
        band = np.tanh(1.6 * fft_noise(n, rng, 2.4, 14.0, 1.0))
        lent = np.clip((fft_noise(n, rng, 1.1, 9.0, 0.9) - 1.5) * 1.6, 0, 1)
        h = h + bands * (0.20 * band + 0.35 * lent)
    h = (h - h.min()) / (h.max() - h.min() + 1e-9)
    col = srgb_to_lin(np.array(spec["color"]) / 255.0)
    tone = 0.55 + 0.75 * h + 0.10 * fft_noise(n, rng, 2.0, 1, 1)
    lichen = np.clip(fft_noise(n, rng, 2.4, 1, 1) - 1.3, 0, 1) * 0.5     # a few grey-green patches
    rgb = col[None, None, :] * tone[..., None]
    rgb = rgb * (1 - lichen[..., None]) + srgb_to_lin(np.array([118, 122, 104]) / 255.0) * tone[..., None] * lichen[..., None]
    if lent is not None:
        dark = 1.0 - 0.45 * bands * np.clip(band, 0, 1) + 0.12 * bands * np.clip(-band, 0, 1)   # dark and pale bands
        rgb = rgb * dark[..., None]
        pale = srgb_to_lin(np.array([150, 120, 98]) / 255.0)               # pale tan lenticels
        rgb = rgb * (1 - 0.8 * lent[..., None]) + pale * 0.8 * lent[..., None]
    return lin_to_srgb(rgb), normal_from_height(h, 6.0)


def leaf_profile(u, spec):
    """Half-width (fraction of length) along a leaf, u 0 at the stalk, 1 at the tip."""
    w = spec["width"] * 0.5
    tip = spec.get("tip", 1.0)
    return w * np.power(np.clip(4 * u * (1 - u), 0, 1), 0.55) * (1.0 - 0.25 * u * tip) * (1 + 0.15 * (1 - tip) * u)


class Canvas:
    """The supersampled atlas being drawn (rows down): colour (linear), coverage, height, and the masks the material
    reads: leaf coverage, twig coverage (also under leaves: what stays in winter), and a per-leaf random id."""

    def __init__(self, n):
        self.rgb = np.zeros((n, n, 3))
        self.alpha = np.zeros((n, n))
        self.height = np.zeros((n, n))
        self.leaf = np.zeros((n, n))
        self.twig = np.zeros((n, n))
        self.lid = np.zeros((n, n))


def draw_leaf(cv, base, axis, length, spec, color, rng):
    """Paint one leaf into the supersampled canvas (rows down, pixels)."""
    H, W = cv.alpha.shape
    perp = np.array([-axis[1], axis[0]])
    needles = spec.get("needles", False)
    half = length * (0.03 if needles else spec["width"] * 0.6)
    pts = np.array([base, base + axis * length, base + perp * half, base - perp * half, base + axis * length + perp * half,
                    base + axis * length - perp * half])
    x0, y0 = np.floor(pts.min(0)).astype(int) - 2
    x1, y1 = np.ceil(pts.max(0)).astype(int) + 2
    x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, W), min(y1, H)
    if x1 <= x0 or y1 <= y0:
        return
    ys, xs = np.mgrid[y0:y1, x0:x1]
    d = np.stack([xs + 0.5 - base[0], ys + 0.5 - base[1]], -1)
    u = (d @ axis) / length
    v = (d @ perp) / length
    if needles:
        hw = 0.012 * np.power(np.clip(1 - u, 0, 1), 0.25)
        inside = (u >= 0) & (u <= 1) & (np.abs(v) <= hw)
        shade = 0.85 + 0.25 * (1 - np.abs(v) / 0.012).clip(0, 1)
        hgt = 0.6 * (1 - np.abs(v) / 0.012).clip(0, 1)
    else:
        hw = leaf_profile(u, spec)
        inside = (u >= 0) & (u <= 1) & (np.abs(v) <= hw)
        rel = np.abs(v) / np.maximum(hw, 1e-4)
        midrib = np.exp(-(np.abs(v) / 0.010) ** 2)
        s = u - 1.1 * np.abs(v) / max(spec["width"], 0.1)
        veins = np.exp(-((np.mod(s * spec["veins"], 1.0) - 0.5) / 0.06) ** 2) * (rel < 0.92) * (u > 0.06)
        side = np.where(v > 0, 1.04, 0.93)
        shade = side * (1.0 - 0.18 * rel ** 3) * (0.86 + 0.18 * u) + 0.20 * midrib + 0.06 * veins
        hgt = (1 - rel ** 2) * 0.7 - 0.25 * veins - 0.35 * midrib
    if not inside.any():
        return
    c = np.clip(color[None, None, :] * shade[..., None], 0, 1)
    sub = (slice(y0, y1), slice(x0, x1))
    cv.rgb[sub][inside] = c[inside]
    cv.alpha[sub][inside] = 1.0
    cv.height[sub][inside] = hgt[inside]
    cv.leaf[sub][inside] = 1.0
    cv.lid[sub][inside] = rng.random()


def draw_flower(cv, c, R, petal_lin, rng):
    """One five-petalled blossom, centre c, radius R (pixels), into the supersampled canvas."""
    H, W = cv.alpha.shape
    x0, y0 = int(c[0] - R - 2), int(c[1] - R - 2)
    x1, y1 = int(c[0] + R + 3), int(c[1] + R + 3)
    x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, W), min(y1, H)
    if x1 <= x0 or y1 <= y0 or R < 1.0:
        return
    ys, xs = np.mgrid[y0:y1, x0:x1]
    dx, dy = xs + 0.5 - c[0], ys + 0.5 - c[1]
    r = np.sqrt(dx * dx + dy * dy) / R
    th = np.arctan2(dy, dx) - rng.uniform(0, 2 * math.pi / 5)
    lobe = 0.5 + 0.5 * np.cos(5 * th)                                   # 1 mid-petal, 0 between petals
    notch = 0.10 * np.exp(-((np.mod(th * 5 / (2 * math.pi) + 0.5, 1.0) - 0.5) / 0.06) ** 2)
    edge = (0.42 + 0.58 * lobe ** 0.45) * (1 - notch)
    inside = r <= edge
    if not inside.any():
        return
    centre = np.clip(1 - r / 0.32, 0, 1)
    deep = srgb_to_lin(np.array([206, 112, 136]) / 255.0)
    stamen = srgb_to_lin(np.array([228, 196, 120]) / 255.0)
    col = petal_lin[None, None, :] * (0.94 + 0.10 * r[..., None].clip(0, 1))
    col = col * (1 - 0.7 * centre[..., None]) + deep * 0.7 * centre[..., None]
    dots = (r > 0.14) & (r < 0.30) & (np.cos(17 * th) > 0.6)
    col[dots] = stamen
    # the petals' veins, faint
    col = col * (1 - 0.05 * (np.cos(th * 40) > 0.7)[..., None])
    sub = (slice(y0, y1), slice(x0, x1))
    cv.rgb[sub][inside] = np.clip(col[inside], 0, 1)
    cv.alpha[sub][inside] = 1.0
    cv.height[sub][inside] = (0.55 * (1 - r) + 0.2 * lobe)[inside]
    cv.leaf[sub][inside] = 1.0
    cv.lid[sub][inside] = rng.random()


def leaf_color(base_lin, rng, spec):
    c = base_lin * rng.uniform(0.72, 1.18)
    c = c * np.array([rng.uniform(0.93, 1.07), 1.0, rng.uniform(0.9, 1.08)])
    if rng.random() < 0.06:                              # a few yellowing or tired leaves
        c = c * np.array([1.45, 1.15, 0.7])
    if rng.random() < 0.05:
        c = c * 0.62
    return c


def rot2(v, a):
    return np.array([v[0] * math.cos(a) - v[1] * math.sin(a), v[0] * math.sin(a) + v[1] * math.cos(a)])


def twig_path(foot, direction, length, bend, n=16):
    d = direction / (np.linalg.norm(direction) + 1e-9)
    perp = np.array([-d[1], d[0]])
    return [foot + d * length * k + perp * bend * length * math.sin(math.pi * k * 0.8) for k in np.linspace(0, 1, n)]


def spray(rng, spec, foot, direction, length, base_lin, leaves, twigs, width):
    """A twig with alternate side twigs, leaves (or needle tufts) along all of them."""
    main = twig_path(foot, direction, length, rng.uniform(-0.12, 0.12))
    paths = [(main, width)]
    if not spec.get("needles"):
        sides = spec.get("side_twigs", 4)
        for k in range(sides):
            t = 0.18 + 0.55 * (k + rng.random() * 0.4) / max(sides, 1)
            i = int(t * (len(main) - 1))
            fwd = main[i + 1] - main[i]
            a = math.radians((1 if k % 2 == 0 else -1) * rng.uniform(32, 55))
            paths.append((twig_path(main[i], rot2(fwd, a), length * rng.uniform(0.38, 0.55) * (1 - 0.4 * t),
                                    rng.uniform(-0.15, 0.15), 10), width * 0.7))
    twigs.extend(paths)
    for path, _ in paths:
        n = len(path) - 1
        plen = sum(np.linalg.norm(b - a) for a, b in zip(path[:-1], path[1:]))
        if spec.get("needles"):
            for k in range(max(4, int(plen / 0.05))):
                t_on = 0.1 + 0.9 * (k + rng.random() * 0.5) / max(4, int(plen / 0.05))
                i = min(int(t_on * n), n - 1)
                fwd = path[i + 1] - path[i]
                fwd /= np.linalg.norm(fwd) + 1e-9
                for _ in range(spec["count"] * 3):
                    dv = rot2(fwd, math.radians(rng.uniform(-65, 65)))
                    leaves.append((path[i].copy(), dv, rng.uniform(*spec["length"]) * (0.7 + 0.4 * t_on),
                                   leaf_color(base_lin, rng, spec)))
            continue
        cnt = max(3, int(plen / (spec["length"][0] * spec.get("spacing", 0.45))))
        for k in range(cnt):
            t_on = 0.12 + 0.86 * (k + rng.random() * 0.5) / cnt
            i = min(int(t_on * n), n - 1)
            fwd = path[i + 1] - path[i]
            fwd /= np.linalg.norm(fwd) + 1e-9
            a = math.radians((1 if k % 2 == 0 else -1) * rng.uniform(35, 70))
            dv = rot2(fwd, a)
            leaves.append((path[i] + dv * 0.008, dv, rng.uniform(*spec["length"]) * (0.8 + 0.3 * math.sin(math.pi * t_on)),
                           leaf_color(base_lin, rng, spec)))
        fwd = path[-1] - path[-3]
        fwd /= np.linalg.norm(fwd) + 1e-9
        leaves.append((path[-1].copy(), fwd, spec["length"][1], leaf_color(base_lin, rng, spec)))


def draw_disc_stroke(cv, a_, b_, r, color):
    for s in np.linspace(0, 1, max(2, int(np.linalg.norm(b_ - a_) / max(r, 0.5)) + 1)):
        q = a_ + (b_ - a_) * s
        rr = max(r, 0.8)
        ys, xs = np.mgrid[int(q[1] - rr - 1):int(q[1] + rr + 2), int(q[0] - rr - 1):int(q[0] + rr + 2)]
        m = (xs + 0.5 - q[0]) ** 2 + (ys + 0.5 - q[1]) ** 2 <= rr * rr
        ys, xs = ys[m], xs[m]
        ok = (ys >= 0) & (ys < cv.alpha.shape[0]) & (xs >= 0) & (xs < cv.alpha.shape[1])
        cv.rgb[ys[ok], xs[ok]] = color
        cv.alpha[ys[ok], xs[ok]] = 1.0
        cv.height[ys[ok], xs[ok]] = 0.5
        cv.twig[ys[ok], xs[ok]] = 1.0


def draw_cluster(cv, cell, spec, rng, dense, twig_only=False, bloom=None):
    """A leafy spray in the cell (x0, y0, size) of the supersampled canvas, its foot at the cell's bottom middle; dense:
    three sprays (the far LOD's bigger cards); twig_only: the same twigs bare (winter)."""
    x0, y0, size = cell
    base_lin = srgb_to_lin(np.array(spec["color"]) / 255.0)
    leaves, twigs = [], []
    n = 3 if dense else 1
    for t in range(n):
        ang = math.radians(rng.uniform(-10, 10) + (t - (n - 1) / 2) * 32)
        spray(rng, spec, np.array([0.0, 0.0]), np.array([math.sin(ang), -math.cos(ang)]), rng.uniform(0.8, 0.9),
              base_lin, leaves, twigs, 0.011)
    # fit everything inside the cell with a margin: nothing touches the card's edge (no straight cut in the alpha)
    pts = [p for path, _ in twigs for p in path]
    for p, dv, ln, _ in leaves:
        pts += [p, p + dv * ln, p + rot2(dv, 0.5) * ln * 0.3, p + rot2(dv, -0.5) * ln * 0.3]
    pts = np.array(pts)
    lo, hi = pts.min(0), pts.max(0)
    margin = 0.04
    scale = min((1 - 2 * margin) / max(hi[0] - lo[0], 1e-3), (1 - margin - 0.01) / max(hi[1] - lo[1], 1e-3))

    def to_px(p):
        return np.array([x0 + (0.5 + (p[0] - (lo[0] + hi[0]) / 2) * scale) * size, y0 + (0.99 - (0 - p[1]) * scale) * size])
    bark = srgb_to_lin(np.array(spec.get("twig", (78, 64, 52))) / 255.0)
    for path, w in twigs:
        for i, (a_, b_) in enumerate(zip(path[:-1], path[1:])):
            r = w * scale * size * (1.0 - 0.55 * i / len(path))
            draw_disc_stroke(cv, to_px(a_), to_px(b_), r, bark * rng.uniform(0.85, 1.15))
    if twig_only:
        return
    rng.shuffle(leaves)
    if bloom is None:
        for p, dv, ln, col in leaves:
            draw_leaf(cv, to_px(p), dv, ln * size * scale, spec, col, rng)
        return
    # blossom: a few small young leaves, then dense flower clusters along the same twigs
    petal = srgb_to_lin(np.array(bloom["petal"]) / 255.0)
    young = srgb_to_lin(np.array([128, 150, 70]) / 255.0)
    for p, dv, ln, col in leaves:
        if rng.random() < bloom["leaf_share"]:
            draw_leaf(cv, to_px(p), dv, ln * 0.6 * size * scale, spec, young * rng.uniform(0.8, 1.1), rng)
    for p, dv, ln, col in leaves:
        for _ in range(rng.integers(bloom["flowers"][0], bloom["flowers"][1] + 1)):
            q = p + dv * ln * rng.uniform(0.15, 0.85) + rot2(dv, math.pi / 2) * ln * rng.uniform(-0.3, 0.3)
            tint = petal * rng.uniform(0.94, 1.04) * np.array([1.0, rng.uniform(0.94, 1.06), rng.uniform(0.96, 1.04)])
            if rng.random() < 0.15:
                tint = np.clip(tint * np.array([1.0, 1.08, 1.06]), 0, 1)   # a few near-white
            draw_flower(cv, to_px(q), bloom["size"] * ln * size * scale * rng.uniform(0.8, 1.15), tint, rng)


def downsample(a, f):
    h, w = a.shape[:2]
    return a.reshape(h // f, f, w // f, f, *a.shape[2:]).mean(axis=(1, 3))


def dilate_under_alpha(rgb, alpha, passes=24):
    """Spread the leaf colour into the transparent texels (no dark halo when mips or filtering mix them in)."""
    filled = alpha > 0.5
    out = rgb.copy()
    acc = filled.astype(np.float64)
    for _ in range(passes):
        if filled.all():
            break
        s = np.zeros_like(out)
        c = np.zeros(acc.shape)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            s += np.roll(np.roll(out * filled[..., None], dy, 0), dx, 1)
            c += np.roll(np.roll(filled, dy, 0), dx, 1)
        grow = (~filled) & (c > 0)
        out[grow] = s[grow] / c[grow][:, None]
        filled = filled | grow
    mean = rgb[alpha > 0.5].mean(0) if (alpha > 0.5).any() else np.array([0.1, 0.12, 0.05])
    out[~filled] = mean
    return out


def leaf_atlas(spec, n, seed, bloom=None):
    """2 x 2 atlas: cells 0-2 single sprays, cell 3 a dense three-spray clump (the far LOD's cards). Rows down.
    Returns colour (sRGB), alpha, normal and the mask (R leaf coverage, G twig coverage, B per-leaf random id)."""
    rng = np.random.default_rng(seed)
    ss = 2
    N = n * ss
    cv = Canvas(N)
    cs = N // 2
    for k, (cx, cy) in enumerate(((0, 0), (1, 0), (0, 1), (1, 1))):
        draw_cluster(cv, (cx * cs, cy * cs, cs), spec, rng, dense=(k == 3), bloom=bloom)
    rgb, alpha, height = downsample(cv.rgb, ss), downsample(cv.alpha, ss), downsample(cv.height, ss)
    leaf, twig = downsample(cv.leaf, ss), downsample(cv.twig, ss)
    lid = downsample(cv.lid, ss) / np.maximum(leaf, 1e-3)
    rgb = dilate_under_alpha(rgb, alpha)
    lid = dilate_under_alpha(np.repeat(lid[..., None], 3, -1), leaf)[..., 0]
    nrm = normal_from_height(height * (alpha > 0.1), 3.0)
    mask = np.stack([leaf, twig, np.clip(lid, 0, 1)], -1)
    return lin_to_srgb(rgb), alpha, nrm, mask


# ----------------------------------------------------------------------------------------------- Blender helpers
def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def np_image(name, rgb, alpha=None, noncolor=False, path=None):
    """A Blender image from a rows-down numpy array (sRGB values unless noncolor); saved as PNG when path is given."""
    h, w = rgb.shape[:2]
    img = bpy.data.images.new(name, w, h, alpha=alpha is not None, is_data=noncolor)
    if noncolor:
        # before the pixels: changing a generated image's colour space regenerates (blanks) its buffer
        img.colorspace_settings.name = "Non-Color"
    a = alpha if alpha is not None else np.ones((h, w))
    px = np.concatenate([rgb, a[..., None]], -1)[::-1].astype(np.float32)   # Blender's rows go up
    img.pixels.foreach_set(px.ravel())
    if path:
        img.filepath_raw = path
        img.file_format = "PNG"
        img.save()
    return img


def make_mesh(name, verts, faces, uvs, mat_index, materials, normals=None):
    me = bpy.data.meshes.new(name)
    me.from_pydata([tuple(v) for v in verts], [], [tuple(f) for f in faces])
    for m in materials:
        me.materials.append(m)
    me.polygons.foreach_set("material_index", mat_index)
    uvl = me.uv_layers.new(name="UVMap")
    loop_vi = np.zeros(len(me.loops), dtype=np.int32)
    me.loops.foreach_get("vertex_index", loop_vi)
    uv = np.asarray(uvs, dtype=np.float32)[loop_vi]
    uvl.uv.foreach_set("vector", uv.ravel())
    me.polygons.foreach_set("use_smooth", [True] * len(me.polygons))
    if normals is not None:
        nl = np.asarray(normals, dtype=np.float32)[loop_vi]
        me.normals_split_custom_set([tuple(x) for x in nl])
    me.update()
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    return ob


# ----------------------------------------------------------------------------------------------- skeleton
class Skeleton:
    def __init__(self):
        self.pos = []
        self.parent = []

    def add(self, p, parent):
        self.pos.append(np.asarray(p, dtype=np.float64))
        self.parent.append(parent)
        return len(self.pos) - 1


def envelope(sp, H, cb, rx, rng):
    """Crown envelope as (centre, radii up/down, test) for the species' shape."""
    shape = sp["crown_shape"]
    if shape == "dome":
        zc = cb + 0.38 * (H - cb)
        return np.array([0, 0, zc]), np.array([rx, rx * rng.uniform(0.88, 1.0), H - zc]), zc - cb - 0.4
    if shape == "vase":
        zc = cb + 0.62 * (H - cb)
        return np.array([0, 0, zc]), np.array([rx, rx * rng.uniform(0.9, 1.0), H - zc]), zc - cb
    if shape == "spread":
        zc = cb + 0.42 * (H - cb)
        return np.array([0, 0, zc]), np.array([rx, rx * rng.uniform(0.88, 0.98), H - zc]), zc - cb + 0.2
    if shape == "round":
        zc = (cb + H) / 2
        return np.array([0, 0, zc]), np.array([rx, rx * rng.uniform(0.9, 1.0), (H - cb) / 2]), (H - cb) / 2
    zc = cb + 0.5 * (H - cb)
    return np.array([0, 0, zc]), np.array([rx, rx * rng.uniform(0.88, 1.0), (H - cb) / 2]), (H - cb) / 2


def in_env(p, c, r, rdown):
    d = p - c
    rz = np.where(d[..., 2] > 0, r[2], rdown)
    return (d[..., 0] / r[0]) ** 2 + (d[..., 1] / r[1]) ** 2 + (d[..., 2] / rz) ** 2


def grow_colonise(sp, rng, H, cb, rx):
    nprng = np.random.default_rng(rng.randrange(1 << 30))
    c, r, rdown = envelope(sp, H, cb, rx, rng)
    # attraction points, denser toward the envelope's skin, a few holes (clumps of air) for an irregular crown
    pts = []
    holes = [c + (nprng.random(3) - 0.5) * r * 1.4 for _ in range(4)]
    while len(pts) < sp["attractors"]:
        p = c + (nprng.random(3) * 2 - 1) * np.array([r[0], r[1], max(r[2], rdown)])
        q = in_env(p, c, r, rdown)
        if q > 1 or nprng.random() > 0.3 + 0.7 * q:
            continue
        if any(np.linalg.norm((p - h) / (r * 0.28)) < 1 for h in holes):
            continue
        if sp["crown_shape"] == "vase":
            # a goblet: narrow where the limbs leave the trunk, widening upward
            t = (p[2] - (c[2] - rdown)) / max(r[2] + rdown, 1e-3)
            if math.hypot(p[0], p[1]) > r[0] * (0.28 + 0.72 * min(1.0, max(t, 0.0) / 0.7) ** 0.7):
                continue
        pts.append(p)
    A = np.array(pts)
    sk = Skeleton()
    # the trunk: fine steps near the foot (the flare), a slight lean and wander, up into the crown's foot
    lean = np.array([rng.uniform(-1, 1), rng.uniform(-1, 1), 0]) * sp["lean"]
    z, prev, p = -0.15, -1, np.array([0.0, 0.0, -0.15])
    top = cb + 0.5
    while z < top:
        prev = sk.add(p.copy(), prev)
        step = 0.15 if z < 1.5 else sp["step"]
        z += step
        p = p + np.array([0, 0, step]) + lean * step + np.array([rng.gauss(0, 0.01), rng.gauss(0, 0.01), 0])
    prev = sk.add(p.copy(), prev)
    trunk_n = len(sk.pos)
    alive = np.ones(len(A), bool)
    up = np.array([0, 0, 1.0])
    for it in range(400):
        P = np.array(sk.pos)
        Aa = A[alive]
        if len(Aa) == 0:
            break
        d2 = ((Aa[:, None, :] - P[None, :, :]) ** 2).sum(-1)
        near = d2.argmin(1)
        dmin = np.sqrt(d2[np.arange(len(Aa)), near])
        idx_alive = np.nonzero(alive)[0]
        alive[idx_alive[dmin < sp["kill"]]] = False
        use = dmin < sp["influence"]
        if not use.any():
            break
        grown = 0
        dirs = {}
        for a_i, n_i in zip(np.nonzero(use)[0], near[use]):
            v = Aa[a_i] - P[n_i]
            v /= np.linalg.norm(v) + 1e-9
            dirs.setdefault(n_i, np.zeros(3))
            dirs[n_i] += v
        for n_i, v in dirs.items():
            if n_i < trunk_n - 1 and n_i < trunk_n - 3:
                continue  # nothing grows out of the clear trunk
            nv = np.linalg.norm(v)
            if nv < 1e-3:
                continue
            v = v / nv + up * sp["tropism"]
            v /= np.linalg.norm(v)
            q = P[n_i] + v * sp["step"]
            sk.add(q, int(n_i))
            grown += 1
        if grown == 0:
            break
    return sk, (c, r, rdown)


def grow_pine(sp, rng, H, cb, rx):
    """A monopodial conifer: a straight stem to the top, whorls of side branches, each with alternate twigs."""
    sk = Skeleton()
    lean = np.array([rng.uniform(-1, 1), rng.uniform(-1, 1), 0]) * sp["lean"]
    p = np.array([0.0, 0.0, -0.15])
    prev = -1
    stem = []
    z = -0.15
    while z < H:
        prev = sk.add(p.copy(), prev)
        stem.append(prev)
        step = 0.15 if z < 1.5 else 0.3
        z += step
        bendz = 0.25 * math.sin(z * 0.45 + rng.random())  # a crooked, wind-bent stem
        p = np.array([0, 0, z]) + lean * z + np.array([bendz * 0.25, bendz * 0.12, 0])
    sk.add(p.copy(), prev)
    stem_z = np.array([sk.pos[i][2] for i in stem])
    z = cb
    while z < H - 0.6:
        k = int(np.argmin(np.abs(stem_z - z)))
        root = stem[k]
        frac = (z - cb) / (H - cb)
        length = rx * (1.0 - frac) ** 0.85 * rng.uniform(0.75, 1.1) + 0.5
        nb = rng.choice((3, 4, 4, 5))
        az0 = rng.random() * 2 * math.pi
        for b in range(nb):
            az = az0 + 2 * math.pi * b / nb + rng.gauss(0, 0.3)
            elev = math.radians(rng.uniform(-12, 18) - 20 * (1 - frac))  # lower branches droop
            d = np.array([math.cos(az) * math.cos(elev), math.sin(az) * math.cos(elev), math.sin(elev)])
            q, prevb = sk.pos[root].copy(), root
            steps = max(3, int(length / 0.3))
            for s in range(steps):
                t = s / steps
                d = d + np.array([0, 0, 0.05 * t]) + np.array([rng.gauss(0, 0.05), rng.gauss(0, 0.05), rng.gauss(0, 0.03)])
                d /= np.linalg.norm(d)
                q = q + d * 0.3
                prevb = sk.add(q.copy(), prevb)
                if s > 0 and s % 2 == 0 and s < steps - 1:  # side twigs in the branch's plane
                    for side in (-1, 1):
                        sd = np.cross(d, [0, 0, 1])
                        sd = sd / (np.linalg.norm(sd) + 1e-9) * side + d * 0.6
                        sd /= np.linalg.norm(sd)
                        qq, pt = q.copy(), prevb
                        for _ in range(max(1, int((1 - t) * 3))):
                            qq = qq + sd * 0.25
                            pt = sk.add(qq.copy(), pt)
        z += rng.uniform(0.45, 0.75)
    c = np.array([0, 0, (cb + H) / 2])
    return sk, (c, np.array([rx, rx, (H - cb) / 2]), (H - cb) / 2)


def finish_skeleton(sk, sp):
    P = np.array(sk.pos)
    par = np.array(sk.parent)
    n = len(P)
    children = [[] for _ in range(n)]
    for i in range(n):
        if par[i] >= 0:
            children[par[i]].append(i)
    # smoothing (not the trunk's foot)
    for _ in range(3):
        Q = P.copy()
        for i in range(n):
            if par[i] < 0 or not children[i] or P[i][2] < 1.6:
                continue
            ch = max(children[i], key=lambda j: len(children[j]))
            Q[i] = 0.5 * P[i] + 0.25 * (P[par[i]] + P[ch])
        P = Q
    # pipe model radii (children always come after their parents)
    e = 2.4
    tip = 0.006
    R = np.zeros(n)
    for i in range(n - 1, -1, -1):
        R[i] = tip if not children[i] else sum(R[j] ** e for j in children[i]) ** (1 / e)
    # scale so the trunk at 1 m is the species' radius
    trunk = [i for i in range(n) if P[i][2] <= 1.0]
    k = sp["trunk_r"] / max(R[max(trunk, key=lambda i: P[i][2])], 1e-6)
    R = np.maximum(R * k, 0.004)
    # the radius never grows toward a tip
    for i in range(n):
        if par[i] >= 0:
            R[i] = min(R[i], R[par[i]])
    return P, par, children, R


# ----------------------------------------------------------------------------------------------- geometry
def frames(points):
    t = [points[min(i + 1, len(points) - 1)] - points[max(i - 1, 0)] for i in range(len(points))]
    t = [v / (np.linalg.norm(v) + 1e-9) for v in t]
    ref = np.array([0, 0, 1.0]) if abs(t[0][2]) < 0.9 else np.array([1.0, 0, 0])
    nrm = np.cross(t[0], ref)
    nrm /= np.linalg.norm(nrm)
    out = []
    for i, ti in enumerate(t):
        if i > 0:
            a = t[i - 1]
            axis = np.cross(a, ti)
            s = np.linalg.norm(axis)
            if s > 1e-6:
                ang = math.atan2(s, float(np.dot(a, ti)))
                nrm = np.array(Matrix.Rotation(ang, 3, Vector(axis / s)) @ Vector(nrm))
        b = np.cross(ti, nrm)
        out.append((ti, nrm, b))
    return out


class Geo:
    def __init__(self):
        self.v, self.f, self.uv, self.mat, self.nrm = [], [], [], [], []

    def tri_count(self):
        return sum(len(f) - 2 for f in self.f)


def add_tube(g, pts, rads, sides, flare, uv_scale=0.7):
    n0 = len(g.v)
    fr = frames(pts)
    circ0 = 2 * math.pi * max(rads[0], 0.01)
    urep = max(1, round(circ0 / uv_scale))
    vacc = 0.0
    lobes_phase = 0.7
    for i, (c, r) in enumerate(zip(pts, rads)):
        if i > 0:
            vacc += np.linalg.norm(pts[i] - pts[i - 1]) / uv_scale
        t, nn, b = fr[i]
        for k in range(sides + 1):
            th = 2 * math.pi * k / sides
            rr = r
            if flare:
                zz = c[2] + 0.15
                rr = r * (1 + 0.65 * math.exp(-zz / 0.28)) * (1 + 0.22 * math.exp(-zz / 0.4) * math.cos(5 * th + lobes_phase))
            d = nn * math.cos(th) + b * math.sin(th)
            g.v.append(c + d * rr)
            g.nrm.append(d)
            g.uv.append((urep * k / sides, vacc))
    for i in range(len(pts) - 1):
        for k in range(sides):
            a = n0 + i * (sides + 1) + k
            bb = a + sides + 1
            g.f.append((a, a + 1, bb + 1, bb))
            g.mat.append(0)
    # close the tip with a point
    tipc = pts[-1] + fr[-1][0] * rads[-1] * 1.5
    ti = len(g.v)
    g.v.append(tipc)
    g.nrm.append(fr[-1][0])
    g.uv.append((0.5 * urep, vacc + 0.05))
    last = n0 + (len(pts) - 1) * (sides + 1)
    for k in range(sides):
        g.f.append((last + k, last + k + 1, ti))
        g.mat.append(0)


def chains(P, par, children, R):
    """Split the tree into chains: each node's thickest child continues its chain, the others start new ones."""
    out = []
    starts = [i for i in range(len(P)) if par[i] < 0]
    stack = list(starts)
    while stack:
        s = stack.pop()
        chain = [s] if par[s] < 0 else [par[s], s]
        i = s
        while children[i]:
            ch = sorted(children[i], key=lambda j: -R[j])
            stack.extend(ch[1:])
            i = ch[0]
            chain.append(i)
        out.append(chain)
    return out


def bark_geo(P, par, children, R, sp, lod):
    g = Geo()
    rmin = sp["lod_r"][lod]
    keep_every = (1, 2, 3)[lod]
    for ch in chains(P, par, children, R):
        r_start = R[ch[1] if len(ch) > 1 and par[ch[0]] >= 0 and ch[0] == par[ch[1]] else ch[0]]
        if r_start < rmin:
            continue
        nodes = [ch[0]]
        for j, i in enumerate(ch[1:], 1):
            if R[i] < rmin * 0.6:
                break
            if j % keep_every == 0 or not children[i] or P[i][2] < 1.6:
                nodes.append(i)
        if len(nodes) < 2:
            continue
        pts = [P[i] for i in nodes]
        rads = [R[i] for i in nodes]
        if par[nodes[0]] >= 0:
            rads[0] = rads[1]  # a branch starts inside its parent at its own radius
        rr = rads[0]
        if lod == 0:
            sides = 14 if rr > 0.15 else 10 if rr > 0.08 else 7 if rr > 0.035 else 5
        elif lod == 1:
            sides = 9 if rr > 0.15 else 6 if rr > 0.06 else 4
        else:
            sides = 6 if rr > 0.12 else 4
        add_tube(g, pts, rads, sides, flare=(par[nodes[0]] < 0))
    return g


def card_anchor_set(P, par, children, R, sp, rng):
    # the twigs: thinner than the species' leaf radius, or (a sparse crown, whose tips the pipe model keeps thicker)
    # within 1.6 times its thinnest tip
    tips = [R[i] for i in range(len(P)) if not children[i]]
    rmax = max(sp["leaf_r"], 1.6 * min(tips)) if tips else sp["leaf_r"]
    cand = [i for i in range(len(P)) if R[i] <= rmax and P[i][2] > 1.0]
    w = np.array([3.0 if not children[i] else 1.0 for i in cand])
    w /= w.sum()
    nprng = np.random.default_rng(rng.randrange(1 << 30))
    order = nprng.choice(cand, size=len(cand), replace=False, p=w)
    # at most a few cards in any 0.45 m cube: no dark knot where the growth crowded its tips
    per, out = {}, []
    for i in order:
        key = tuple(np.floor(P[i] / 0.45).astype(int))
        if per.get(key, 0) >= 4:
            continue
        per[key] = per.get(key, 0) + 1
        out.append(int(i))
        if len(out) >= sp["cards"]:
            break
    return out


def add_cards(g, P, par, R, anchors, sp, rng, env, scale, cells, mat=1):
    c, r, rdown = env
    for i in anchors:
        d = P[i] - P[par[i]] if par[i] >= 0 else np.array([0, 0, 1.0])
        d = d / (np.linalg.norm(d) + 1e-9)
        out = P[i] - c
        out[2] *= 0.6
        out /= np.linalg.norm(out) + 1e-9
        axis = d * 0.55 + out * 0.45 + np.array([rng.gauss(0, 0.35), rng.gauss(0, 0.35), rng.gauss(0, 0.25) - 0.08])
        axis /= np.linalg.norm(axis)
        nrm = out - axis * np.dot(out, axis)
        if np.linalg.norm(nrm) < 1e-3:
            nrm = np.cross(axis, [1, 0, 0])
        nrm /= np.linalg.norm(nrm)
        ang = rng.uniform(-0.9, 0.9)
        side = np.cross(axis, nrm)
        nrm, side = nrm * math.cos(ang) + side * math.sin(ang), side * math.cos(ang) - nrm * math.sin(ang)
        size = rng_range(rng, sp["card_size"]) * scale
        L, W = size, size
        base = P[i] - axis * L * 0.12
        cell = rng.choice(cells)
        u0, v0 = (cell % 2) * 0.5, (1 - cell // 2) * 0.5   # cell 0 top-left in the image = (0, 0.5) in UV
        m = 0.004
        n0 = len(g.v)
        for row, tv in enumerate((0.0, 0.5, 1.0)):
            for col, su in enumerate((-1.0, 0.0, 1.0)):
                fold = sp["fold"] * abs(su) * W * 0.5
                droop = sp["droop"] * tv * tv * L
                p = base + axis * (tv * L) + side * (su * W * 0.5) + nrm * fold - np.array([0, 0, droop])
                g.v.append(p)
                # normals: mostly the envelope's (the crown shades as one), a little of the card's own
                e = (p - c) / np.array([r[0] ** 2, r[1] ** 2, (r[2] if p[2] > c[2] else rdown) ** 2])
                e /= np.linalg.norm(e) + 1e-9
                nn = 0.78 * e + 0.22 * nrm * (1 if np.dot(nrm, e) >= 0 else -1) + np.array([0, 0, 0.12])
                g.nrm.append(nn / np.linalg.norm(nn))
                g.uv.append((u0 + m + (su * 0.5 + 0.5) * (0.5 - 2 * m), v0 + m + tv * (0.5 - 2 * m)))
        for row in range(2):
            for col in range(2):
                a = n0 + row * 3 + col
                g.f.append((a, a + 1, a + 4, a + 3))
                g.mat.append(mat)


def billboard_geo(H, size, zc):
    g = Geo()
    for k in range(3):
        th = math.pi * k / 3
        dx, dy = math.cos(th), math.sin(th)
        n0 = len(g.v)
        for (s, t) in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            p = np.array([dx * s * size / 2, dy * s * size / 2, zc + t * size / 2])
            g.v.append(p)
            e = np.array([dx * s * 0.6, dy * s * 0.6, 0.5 + 0.3 * t])
            nrm = np.array([-dy, dx, 0]) * 0.5 + e
            g.nrm.append(nrm / np.linalg.norm(nrm))
            g.uv.append(((s + 1) / 2, (t + 1) / 2))
        g.f.append((n0, n0 + 1, n0 + 2, n0 + 3))
        g.mat.append(2)
    return g


def trunk_hull(P, par, R, cb):
    """UCX_: an eight-sided prism around the trunk from its foot to the crown's foot (never the crown)."""
    trunk = [i for i in range(len(P)) if P[i][2] < cb]
    r1 = max(R[i] for i in trunk if P[i][2] > 0.8) if any(P[i][2] > 0.8 for i in trunk) else 0.2
    cx = float(np.mean([P[i][0] for i in trunk]))
    cy = float(np.mean([P[i][1] for i in trunk]))
    v, f = [], []
    for z in (-0.15, cb):
        for k in range(8):
            th = 2 * math.pi * k / 8
            v.append((cx + r1 * math.cos(th), cy + r1 * math.sin(th), z))
    f.append(tuple(range(7, -1, -1)))
    f.append(tuple(range(8, 16)))
    for k in range(8):
        a, b = k, (k + 1) % 8
        f.append((a, b, b + 8, a + 8))
    return v, f, r1


# ----------------------------------------------------------------------------------------------- materials
def plain_material(name, color):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    m.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (*color, 1)
    return m


def render_material(name, img, nimg=None, leaf=False):
    m = bpy.data.materials.new(name + "_r")
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes["Principled BSDF"]
    t = nt.nodes.new("ShaderNodeTexImage")
    t.image = img
    nt.links.new(t.outputs["Color"], b.inputs["Base Color"])
    b.inputs["Roughness"].default_value = 0.75
    if leaf:
        nt.links.new(t.outputs["Alpha"], b.inputs["Alpha"])
        b.inputs["Subsurface Weight"].default_value = 0.0
        # cheap translucency: a translucent lobe mixed in
        tr = nt.nodes.new("ShaderNodeBsdfTranslucent")
        nt.links.new(t.outputs["Color"], tr.inputs["Color"])
        mix = nt.nodes.new("ShaderNodeMixShader")
        mix.inputs["Fac"].default_value = 0.25
        out = nt.nodes["Material Output"]
        nt.links.new(b.outputs["BSDF"], mix.inputs[1])
        nt.links.new(tr.outputs["BSDF"], mix.inputs[2])
        tr_mix = nt.nodes.new("ShaderNodeMixShader")
        transp = nt.nodes.new("ShaderNodeBsdfTransparent")
        nt.links.new(t.outputs["Alpha"], tr_mix.inputs["Fac"])
        nt.links.new(transp.outputs["BSDF"], tr_mix.inputs[1])
        nt.links.new(mix.outputs["Shader"], tr_mix.inputs[2])
        nt.links.new(tr_mix.outputs["Shader"], out.inputs["Surface"])
    return m


def bare_material(mask_img, twig_srgb):
    """Winter: the leaf cards show only their twigs (the mask's green as alpha)."""
    m = bpy.data.materials.new("Bare_r")
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes["Principled BSDF"]
    t = nt.nodes.new("ShaderNodeTexImage")
    t.image = mask_img
    sep = nt.nodes.new("ShaderNodeSeparateColor")
    nt.links.new(t.outputs["Color"], sep.inputs["Color"])
    b.inputs["Base Color"].default_value = (*srgb_to_lin(np.array(twig_srgb) / 255.0), 1)
    b.inputs["Roughness"].default_value = 0.85
    mix = nt.nodes.new("ShaderNodeMixShader")
    transp = nt.nodes.new("ShaderNodeBsdfTransparent")
    nt.links.new(sep.outputs["Green"], mix.inputs["Fac"])
    nt.links.new(transp.outputs["BSDF"], mix.inputs[1])
    nt.links.new(b.outputs["BSDF"], mix.inputs[2])
    nt.links.new(mix.outputs["Shader"], nt.nodes["Material Output"].inputs["Surface"])
    return m


# ----------------------------------------------------------------------------------------------- rendering
def cycles(scene, samples, w, h, transparent):
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = samples
    scene.cycles.use_denoising = True
    scene.cycles.transparent_max_bounces = 48
    scene.cycles.max_bounces = 6
    scene.render.resolution_x, scene.render.resolution_y = w, h
    scene.render.film_transparent = transparent
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA" if transparent else "RGB"
    scene.view_settings.view_transform = "Standard"


def world(color, strength):
    w = bpy.data.worlds.new("W")
    w.use_nodes = True
    bg = w.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value = (*color, 1)
    bg.inputs["Strength"].default_value = strength
    bpy.context.scene.world = w


def camera(name, loc, target, ortho=None, lens=35):
    cd = bpy.data.cameras.new(name)
    if ortho:
        cd.type = "ORTHO"
        cd.ortho_scale = ortho
    else:
        cd.lens = lens
    cd.clip_end = 1000
    ob = bpy.data.objects.new(name, cd)
    bpy.context.scene.collection.objects.link(ob)
    ob.location = loc
    d = Vector(target) - Vector(loc)
    ob.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
    bpy.context.scene.camera = ob
    return ob


def render_billboard(lod0, H, size, zc, path, res):
    """One picture of lod0 from the side (straight colour dilated under its alpha), rows down: (sRGB, alpha)."""
    scene = bpy.context.scene
    for ob in scene.objects:
        ob.hide_render = ob is not lod0
    cycles(scene, 48, res, res, True)
    world((0.85, 0.88, 0.92), 1.0)
    sun = bpy.data.objects.new("bb_sun", bpy.data.lights.new("bb_sun", "SUN"))
    sun.data.energy = 1.4
    sun.rotation_euler = (math.radians(50), 0, math.radians(15))
    scene.collection.objects.link(sun)
    cam = camera("bb_cam", (0, -60, zc), (0, 0, zc), ortho=size)
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    bpy.data.objects.remove(cam)
    bpy.data.objects.remove(sun)
    img = bpy.data.images.load(path)
    px = np.array(img.pixels[:]).reshape(res, res, 4)[::-1]
    bpy.data.images.remove(img)
    rgb = px[..., :3]
    a = px[..., 3]
    # straight colour (un-premultiply), then dilate it under the alpha; brighten a touch (no sun's back-light in game)
    rgb = np.where(a[..., None] > 1e-3, rgb / np.maximum(a[..., None], 1e-3), 0)
    lin = srgb_to_lin(rgb)
    lin = dilate_under_alpha(lin, a, passes=40)
    a = np.clip((a - 0.25) / 0.5, 0, 1)
    return lin_to_srgb(lin), a


def render_sheet(objs_near, obj_far, H, path, tmpdir):
    scene = bpy.context.scene
    world((0.5, 0.5, 0.5), 0.6)
    gm = bpy.data.materials.new("grey")
    gm.use_nodes = True
    gm.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.21, 0.21, 0.21, 1)
    bpy.ops.mesh.primitive_plane_add(size=400)
    ground = bpy.context.object
    ground.data.materials.append(gm)
    sun = bpy.data.objects.new("sh_sun", bpy.data.lights.new("sh_sun", "SUN"))
    sun.data.energy = 3.2
    sun.data.angle = math.radians(0.6)
    sun.rotation_euler = (math.radians(58), 0, math.radians(-35))
    scene.collection.objects.link(sun)
    shots = []
    for tag, objs, dist, w, h in (("near", objs_near, 10.0, 900, 900), ("far", [obj_far], 80.0, 2560, 1440)):
        for ob in scene.objects:
            if ob.type == "MESH":
                ob.hide_render = not (ob is ground or ob in objs)
        cycles(scene, 64 if tag == "near" else 32, w, h, False)
        eye = (0, -dist, 1.6) if tag == "far" else (0, -dist, 1.7)
        at = (0, 0, H * 0.5) if tag == "near" else (0, 0, H * 0.4)
        cam = camera("sh_" + tag, eye, at, lens=16 if tag == "near" else 28)
        f = os.path.join(tmpdir, "sheet_%s.png" % tag)
        scene.render.filepath = f
        bpy.ops.render.render(write_still=True)
        bpy.data.objects.remove(cam)
        img = bpy.data.images.load(f)
        px = np.array(img.pixels[:]).reshape(h, w, 4)[::-1][..., :3]
        bpy.data.images.remove(img)
        if tag == "far":  # the tree's own pixels at 2560 wide, cropped and shown 2x nearest
            cy, cx = h // 2 - 120, w // 2
            px = px[cy - 225:cy + 225, cx - 225:cx + 225]
            px = np.repeat(np.repeat(px, 2, 0), 2, 1)
        shots.append(px)
    sheet = np.concatenate(shots, 1)
    np_image("sheet", sheet, None, path=path)
    bpy.data.objects.remove(sun)
    bpy.data.objects.remove(ground)


# ----------------------------------------------------------------------------------------------- the grate
def make_grate(out):
    reset()
    iron = plain_material("Iron", (0.035, 0.035, 0.038))
    iron.node_tree.nodes["Principled BSDF"].inputs["Metallic"].default_value = 0.7
    iron.node_tree.nodes["Principled BSDF"].inputs["Roughness"].default_value = 0.55
    soil = plain_material("Soil", (0.045, 0.035, 0.028))
    soil.node_tree.nodes["Principled BSDF"].inputs["Roughness"].default_value = 0.95
    v, f, mats = [], [], []

    def box(x0, x1, y0, y1, z0, z1, m):
        n = len(v)
        for z in (z0, z1):
            v.extend([(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)])
        for q in ((0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)):
            f.append(tuple(n + i for i in q))
            mats.append(m)
    S, fw, top, hole = 0.75, 0.06, 0.02, 0.36
    box(-S, S, -S, -S + fw, 0.0, top, 0)
    box(-S, S, S - fw, S, 0.0, top, 0)
    box(-S, -S + fw, -S + fw, S - fw, 0.0, top, 0)
    box(S - fw, S, -S + fw, S - fw, 0.0, top, 0)
    # slats across x, split round the trunk's hole; a square collar round the hole
    y = -S + fw + 0.045
    while y < S - fw - 0.02:
        if abs(y) > hole:
            box(-S + fw, S - fw, y - 0.012, y + 0.012, 0.004, top - 0.002, 0)
        else:
            box(-S + fw, -hole, y - 0.012, y + 0.012, 0.004, top - 0.002, 0)
            box(hole, S - fw, y - 0.012, y + 0.012, 0.004, top - 0.002, 0)
        y += 0.055
    box(-hole - 0.03, -hole, -hole - 0.03, hole + 0.03, 0.0, top, 0)
    box(hole, hole + 0.03, -hole - 0.03, hole + 0.03, 0.0, top, 0)
    box(-hole, hole, -hole - 0.03, -hole, 0.0, top, 0)
    box(-hole, hole, hole, hole + 0.03, 0.0, top, 0)
    # the soil under the slats (the trunk stands in the middle)
    n = len(v)
    v.extend([(-S + fw, -S + fw, 0.002), (S - fw, -S + fw, 0.002), (S - fw, S - fw, 0.002), (-S + fw, S - fw, 0.002)])
    f.append((n, n + 1, n + 2, n + 3))
    mats.append(1)
    uvs = [(p[0] + 0.75, p[1] + 0.75) for p in v]
    ob = make_mesh("TREE-grate", v, f, uvs, mats, [iron, soil])
    for p in ob.data.polygons:
        p.use_smooth = False
    bpy.ops.object.select_all(action="DESELECT")
    ob.select_set(True)
    path = os.path.join(out, "TREE-grate.glb")
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True, export_yup=True, export_apply=False,
                              export_normals=True, export_tangents=False, export_materials="EXPORT",
                              export_vertex_color="NONE", export_animations=False, export_skins=False, export_morph=False)
    tris = sum(len(p.vertices) - 2 for p in ob.data.polygons)
    print("make_tree: grate %s, %d triangles" % (path, tris))


# ----------------------------------------------------------------------------------------------- main
def main():
    o = args()
    os.makedirs(o["out"], exist_ok=True)
    if o["grate"]:
        make_grate(o["out"])
        return
    t0 = time.time()
    sp = SPECIES[o["species"]]
    name = "TREE-%s-%s" % (o["species"], o["variant"])
    rng = random.Random(o["seed"])
    reset()
    H = rng_range(rng, sp["height"])
    cb = rng_range(rng, sp["crown_base"])
    rx = rng_range(rng, sp["crown_rx"])
    if o["species"] == "pine":
        sk, env = grow_pine(sp, rng, H, cb, rx)
    else:
        sk, env = grow_colonise(sp, rng, H, cb, rx)
    P, par, children, R = finish_skeleton(sk, sp)
    print("make_tree: %s seed %d: %d nodes, height %.1f m, crown base %.1f, crown radius %.1f, trunk r %.2f m (%.1f s)"
          % (name, o["seed"], len(P), H, cb, rx, sp["trunk_r"], time.time() - t0))

    # textures
    n = o["tex"]
    tdir = o["out"]
    bark_rgb, bark_n = bark_maps(sp["bark"], n, o["seed"] + 11)
    leaf_rgb, leaf_a, leaf_n, leaf_m = leaf_atlas(sp["leaf"], n, o["seed"] + 23)
    img_bloom = None
    if sp.get("bloom"):
        bloom_rgb, bloom_a, _, _ = leaf_atlas(sp["leaf"], n, o["seed"] + 23, bloom=sp["bloom"])
        img_bloom = np_image(name + "_bloom", bloom_rgb, bloom_a, path=os.path.join(tdir, name + "_bloom.png"))
    f_bark = os.path.join(tdir, name + "_bark.png")
    f_leaf = os.path.join(tdir, name + "_leaf.png")
    img_bark = np_image(name + "_bark", bark_rgb, None, path=f_bark)
    np_image(name + "_bark_n", bark_n, None, noncolor=True, path=os.path.join(tdir, name + "_bark_n.png"))
    img_leaf = np_image(name + "_leaf", leaf_rgb, leaf_a, path=f_leaf)
    np_image(name + "_leaf_n", leaf_n, None, noncolor=True, path=os.path.join(tdir, name + "_leaf_n.png"))
    img_mask = np_image(name + "_leaf_m", leaf_m, None, noncolor=True, path=os.path.join(tdir, name + "_leaf_m.png"))
    mean_leaf = (leaf_rgb[leaf_a > 0.5].mean(0) * 255).round().astype(int).tolist()
    print("make_tree: textures (%d px), leaf mean sRGB %s (%.1f s)" % (n, mean_leaf, time.time() - t0))

    # meshes
    m_bark = plain_material("Bark", (0.1, 0.08, 0.06))
    m_leaf = plain_material("Leaf", (0.1, 0.16, 0.05))
    m_bb = plain_material("Billboard", (0.1, 0.16, 0.05))
    anchors = card_anchor_set(P, par, children, R, sp, rng)
    lods, tris = [], []
    for lod in range(3):
        g = bark_geo(P, par, children, R, sp, lod)
        bark_tris = g.tri_count()
        keep = sp["lod_cards"][lod]
        lr = random.Random(o["seed"] * 31 + lod)
        sub = anchors if keep >= 1 else lr.sample(anchors, max(8, int(len(anchors) * keep)))
        scale = 1.0 / math.sqrt(keep) * (0.97 if lod else 1.0)
        cells = [0, 1, 2] if lod < 2 else [3]
        add_cards(g, P, par, R, sub, sp, lr, env, scale, cells)
        ob = make_mesh("%s_LOD%d" % (name, lod), g.v, g.f, g.uv, g.mat, [m_bark, m_leaf, m_bb], g.nrm)
        lods.append(ob)
        tris.append(g.tri_count())
        print("make_tree: LOD%d: %d triangles (bark %d, %d cards)" % (lod, g.tri_count(), bark_tris, len(sub)))
    # the billboard's frame: square, round the whole tree
    co =np.array([v.co[:] for v in lods[0].data.vertices])
    w = max(co[:, 0].max() - co[:, 0].min(), co[:, 1].max() - co[:, 1].min())
    size = max(w, co[:, 2].max() - co[:, 2].min()) * 1.04
    zc = (co[:, 2].max() + co[:, 2].min()) / 2
    bb = billboard_geo(H, size, zc)
    ob_bb = make_mesh("%s_LOD3" % name, bb.v, bb.f, bb.uv, bb.mat, [m_bark, m_leaf, m_bb], bb.nrm)
    tris.append(bb.tri_count())
    hv, hf, r1 = trunk_hull(P, par, R, cb)
    ucx = make_mesh("UCX_%s_LOD0_00" % name, hv, hf, [(0, 0)] * len(hv), [0] * len(hf), [m_bark])
    for p in ucx.data.polygons:
        p.use_smooth = False

    # billboard texture, rendered from LOD0 with the real textures
    f_bb = os.path.join(tdir, name + "_bb.png")
    if o["billboard"]:
        # the seasonal atlas: leafy top left, bare top right, blossom bottom left (leafy again where there is none)
        res = min(512, n)
        mat_leaf = render_material("Leaf", img_leaf, leaf=True)
        states = [mat_leaf, bare_material(img_mask, sp["season"]["twig"]),
                  render_material("Bloom", img_bloom, leaf=True) if img_bloom else mat_leaf]
        pics = []
        for k, mat in enumerate(states):
            if k == 2 and img_bloom is None:
                pics.append(pics[0])
                continue
            for ob in lods:
                ob.data.materials[0] = render_material("Bark", img_bark)
                ob.data.materials[1] = mat
            pics.append(render_billboard(lods[0], H, size, zc, os.path.join(tdir, name + "_bb_%d.png" % k), res))
            os.remove(os.path.join(tdir, name + "_bb_%d.png" % k))
        rgb = np.zeros((2 * res, 2 * res, 3))
        alpha = np.zeros((2 * res, 2 * res))
        for (rgb_k, a_k), (cy, cx) in zip(pics + [pics[0]], ((0, 0), (0, 1), (1, 0), (1, 1))):
            rgb[cy * res:(cy + 1) * res, cx * res:(cx + 1) * res] = rgb_k
            alpha[cy * res:(cy + 1) * res, cx * res:(cx + 1) * res] = a_k
        np_image("bb_out", rgb, alpha, path=f_bb)
        for ob in lods:
            ob.data.materials[1] = mat_leaf
        print("make_tree: billboard atlas %s (%.1f s)" % (f_bb, time.time() - t0))
    if o["sheet"]:
        bbimg = bpy.data.images.load(f_bb) if os.path.exists(f_bb) else img_leaf
        ob_bb.data.materials[2] = render_material("Billboard", bbimg, leaf=True)
        for ob in lods:
            if ob.data.materials[0].name == "Bark":
                ob.data.materials[0] = render_material("Bark", img_bark)
                ob.data.materials[1] = render_material("Leaf", img_leaf, leaf=True)
        if img_bloom is not None:
            # the default look (spring) has the cherries in blossom: show that
            mat_bloom = render_material("Bloom", img_bloom, leaf=True)
            for ob in lods:
                ob.data.materials[1] = mat_bloom
        render_sheet([lods[0]], lods[2], H, os.path.join(tdir, name + "_sheet.png"), tdir)
        print("make_tree: sheet (%.1f s)" % (time.time() - t0))
    for ob in lods + [ob_bb]:
        ob.data.materials[0], ob.data.materials[1], ob.data.materials[2] = m_bark, m_leaf, m_bb
        ob.hide_render = False

    # export
    bpy.ops.object.select_all(action="DESELECT")
    for ob in lods + [ob_bb, ucx]:
        ob.select_set(True)
    path = os.path.join(o["out"], name + ".glb")
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True, export_yup=True, export_apply=False,
                              export_normals=True, export_tangents=False, export_materials="EXPORT",
                              export_vertex_color="NONE", export_animations=False, export_skins=False, export_morph=False,
                              export_image_format="NONE")
    meta = dict(name=name, species=o["species"], variant=o["variant"], seed=o["seed"], height_m=round(float(co[:, 2].max()), 2),
                crown_m=round(float(w), 2), crown_base_m=round(cb, 2), trunk_radius_m=round(float(r1), 3),
                trunk_foot_m=-0.15, lod_triangles=tris, lod_screen_size=list(LOD_SCREEN), cards=len(anchors),
                billboard_size_m=round(float(size), 2), billboard_centre_z_m=round(float(zc), 2), leaf_mean_srgb=mean_leaf,
                autumn_a=list(sp["season"]["autumn"][0]), autumn_b=list(sp["season"]["autumn"][1]),
                twig=list(sp["season"]["twig"]), spring=list(sp["season"]["spring"]),
                evergreen=1 if sp["season"].get("evergreen") else 0, billboard_atlas=1 if o["billboard"] else 0)
    with open(os.path.join(o["out"], name + ".json"), "w") as fh:
        json.dump(meta, fh, indent=1)
    print("make_tree: wrote %s: LOD triangles %s, height %.1f m, crown %.1f m (%.1f s)"
          % (path, tris, meta["height_m"], meta["crown_m"], time.time() - t0))


main()
