"""The hero corner's lanterns, string posts, shop fittings and lane dressing, modelled by script.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/hero_corner_props.py -- <out_dir> [--only A,B]

Writes into <out_dir> (metres, Z up in Blender, glTF +Y up in the files):
  SM_HC_LanternLarge.glb      0.60 m lantern, 0.45 m across the paper; origin at the hook's top, hanging down -Z; a
                              0.25 m pull cord and bead below the bottom cap
  SM_HC_LanternSmall.glb      0.32 m, 0.25 m across; 0.15 m cord
  SM_HC_LanternLargeMark.glb  the large lantern with an invented brush mark on its +Y and -Y faces (slot lantern_ink)
  SM_HC_StringPost.glb        4.0 m timber post; origin at its foot; an iron collar at 3.8 m (the cable's anchor)
  SM_HC_ShelfUnit.glb         1.8 x 0.4 x 1.9 m stocked shop shelving; origin at the bottom centre of its back face,
                              front -Y, back at y = 0
  SM_HC_Counter.glb           2.4 x 0.6 x 0.95 m shop counter with its top dressed; same origin convention
  SM_HC_HangGoods.glb         0.6 m bamboo pole with dried goods hanging 0.4-0.6 m below; origin at the pole's centre
  SM_HC_HangSignL.glb         0.40 x 1.30 m vertical signboard on a 0.7 m iron wall bracket; origin at the bracket's
  SM_HC_HangSignS.glb         wall mount (the wall is y = 0, the arm points -Y); the board's faces look along X
  SM_HC_Banner.glb            0.45 x 1.6 m cloth banner on a 0.55 m bamboo rod; origin at the rod's centre
  SM_HC_Crate.glb             0.6 x 0.4 x 0.35 m slat crate; origin at the bottom centre
  SM_HC_Bucket.glb            0.30 m stave bucket with a rope handle; origin at the bottom centre
  SM_HC_Gate.glb              timber lane gateway, posts at x = +-3.6; origin at the ground centre
  BICYCLE.glb                 1.70 m city bicycle along Y (front wheel at -Y), the kit's BICYCLE replaced at its
                              bounds; origin at the ground under its middle
  SM_HC_StockPot.glb          dented, sooted 0.45 m aluminium stock pot (0.335 m) on a 0.065 m gas ring, rim 0.40 m over
                              the counter; origin at the bottom centre; handles along X, lid slid to +X on the ladle,
                              gas valve and hose to +Y (the cook's side); slots hc_pot_metal_soot / hc_pot_metal / hc_broth_water
  SM_HC_GoodsBottles.glb      0.42 x 0.26 m tray of bottles (dark glass, slot hc_glass), a flask, two jars, bowls;
                              origin at the bottom centre; labels and marks face -Y
  hero_corner_props.json      triangles, bounds, slots and images per file, from the files read back (merged)
With --only, only the named pieces are built and their rows replace those in the json.

The lanterns, the post and every piece without a textured slot have flat material factors and no images. The textured
pieces carry generated (procedural, numpy) 1024 px maps embedded in the glb. Every sign or mark is an invented abstract
brush mark: no letters, no real script. One UV set, no vertex colour, no collision hulls (the importer makes its own).
The lanterns' paper slot "lantern_paper" is the one Unreal makes glow; no other slot name contains "paper".
"""
import json
import math
import os
import random
import struct
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector


def _load_ct():
    """make_clock_tower.py's texture and material helpers, loaded without running its main()."""
    import types
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_clock_tower.py")
    with open(path, encoding="utf-8") as f:
        src = f.read()
    src = src[:src.rindex("\nmain()")]
    mod = types.ModuleType("make_clock_tower")
    mod.__file__ = path
    exec(compile(src, path, "exec"), mod.__dict__)
    return mod


ct = _load_ct()
pnoise, fbm, smooth01 = ct.pnoise, ct.fbm, ct.smooth01

SEG = 24

# =====================================================================================================================
# Materials
# =====================================================================================================================

# Flat slots: name: (base colour linear rgb, roughness, metallic)
MATERIALS = {
    "lantern_paper": ((0.72, 0.16, 0.08), 0.85, 0.0),    # vermilion washi (Unreal: the night glow)
    # Dry roughness per the 11-16 gloss order (GLOSS_SETTS_V6_PLAN_1116.md, Order 1): nothing but glaze, glass and
    # liquid under 0.40; metals stay metal only where the name says so (iron, metal, brass, bronze, steel ...).
    "lantern_wood": ((0.030, 0.016, 0.011), 0.55, 0.0),  # black-brown lacquered caps and the cord's bead
    "lantern_iron": ((0.03, 0.03, 0.03), 0.62, 1.0),     # hook
    "lantern_rib": ((0.05, 0.035, 0.02), 0.6, 0.0),      # dark smoked bamboo hoops
    "lantern_cord": ((0.11, 0.028, 0.018), 0.88, 0.0),   # dark red-brown pull cord
    "lantern_ink": ((0.012, 0.011, 0.010), 0.7, 0.0),    # near-black ink
    "post_timber": ((0.11, 0.075, 0.05), 0.75, 0.0),
    "post_iron": ((0.035, 0.035, 0.035), 0.62, 1.0),
    "hc_glaze_brown": ((0.10, 0.042, 0.018), 0.28, 0.0),  # iron-brown glaze
    "hc_glaze_green": ((0.11, 0.19, 0.12), 0.30, 0.0),    # grey-green glaze
    "hc_glaze_cream": ((0.52, 0.47, 0.37), 0.32, 0.0),    # pale bowls
    "hc_bisque": ((0.36, 0.25, 0.16), 0.80, 0.0),         # unglazed feet
    "hc_metal": ((0.42, 0.40, 0.37), 0.55, 1.0),          # tinplate
    "hc_iron": ((0.035, 0.034, 0.033), 0.65, 1.0),        # brackets, hooks, hoops, nails
    "hc_string": ((0.46, 0.38, 0.26), 0.9, 0.0),          # twine
    "hc_straw": ((0.42, 0.33, 0.15), 0.9, 0.0),           # straw rope
    "hc_lacquer": ((0.022, 0.011, 0.008), 0.45, 0.0),     # black-brown lacquer (signs: off the 0.40 edge)
    "hc_signink": ((0.62, 0.42, 0.14), 0.60, 0.0),        # ochre-gold pigment in the carved marks (not metal)
    "hc_bannerink": ((0.70, 0.66, 0.56), 0.85, 0.0),      # pale dye
    "hc_bamboo": ((0.30, 0.23, 0.10), 0.60, 0.0),         # aged bamboo
    "hc_chilli": ((0.30, 0.018, 0.010), 0.48, 0.0),       # dried chillies (waxy skin, dusty)
    "hc_stem": ((0.12, 0.10, 0.04), 0.8, 0.0),
    "hc_herb": ((0.13, 0.12, 0.05), 0.85, 0.0),           # dried leaves
    "hc_rooftile": ((0.055, 0.06, 0.065), 0.60, 0.0),     # grey fired tile
}

# Textured slots: name: (generator, seed, tile metres, fallback roughness)
TEXTURED = {}
TILE = {}
_MATS = {}


def _save(name, rgb, n, cs, fmt):
    img, mean = ct.save_image(name, rgb, cs, fmt)
    return img, mean


def _tex(name, rgb, h, tile, depth, mr, n):
    bc = _save(name + "_BC", rgb, n, "sRGB", "JPEG")
    nm = _save(name + "_N", ct.normal_from_height(h, tile, depth, n=n), n, "Non-Color", "PNG")
    m = _save(name + "_MR", mr, n, "Non-Color", "PNG")
    return bc, nm, m


def _mr(dirt, rough, metal=0.0):
    n = dirt.shape[0]
    return np.dstack([np.clip(dirt, 0, 1), np.clip(rough, 0.02, 1), np.full((n, n), metal)])


def gen_timber(seed, tile, n=1024, worn_post=None):
    """Planed, aged cedar: grain (rings and figure) along v, latewood lines raised (weathered), pores, broad tone.
    worn_post = post height (m): v spans that height and the foot darkens, roughens and greys (dirt mask in R)."""
    rng = np.random.default_rng(seed)
    u, v = ct._uv(tile, n)
    period = tile / round(tile / 0.009)
    # flat-sawn figure: the ring lines wander across the board in long arches, plus a little waver
    # pnoise's aniso scales sigma per axis: (1, k) with k > 1 stretches the noise along v, the grain
    warp = pnoise(rng, 60, n, aniso=(1.0, 4.0)) * 0.012 + pnoise(rng, 15, n, aniso=(1.0, 6.0)) * 0.0008
    fig = pnoise(rng, 2, n, aniso=(1.0, 25.0))
    ph = (u + warp) / period
    rings = 0.5 + 0.5 * np.sin(2 * math.pi * ph)
    late = smooth01((rings - 0.6) / 0.4) ** 1.5
    tone = pnoise(rng, 160, n)
    board = pnoise(rng, 30, n, aniso=(1.0, 5.0))
    pores = pnoise(rng, 0.8, n, aniso=(1.0, 10.0))
    early = np.array([0.46, 0.31, 0.19])
    latec = np.array([0.27, 0.16, 0.09])
    c = early * (1 + 0.07 * tone[..., None] + 0.06 * fig[..., None] + 0.05 * board[..., None])
    c = c * (1 - 0.65 * late[..., None]) + latec * 0.65 * late[..., None]
    c = np.where((pores > 2.6)[..., None], c * 0.88, c)
    st = smooth01(pnoise(rng, 3, n, aniso=(1.0, 20.0)) * 0.35 + 0.1)
    dirt = st * 0.4
    # Roughness spread (p10..p90 about 0.64..0.85): weathered / rubbed patches stretched along the grain, the hard
    # latewood smoother than the eroded earlywood, dirt streaks rougher. The extra noise has its own generator so the
    # colour and normal maps keep their draws.
    rng2 = np.random.default_rng(seed + 7000)
    wpatch = pnoise(rng2, 24, n, aniso=(1.0, 8.0))
    rough = 0.755 + 0.06 * wpatch + 0.04 * fig - 0.09 * late + 0.08 * st
    h = 0.5 + 0.25 * late + 0.04 * fig
    if worn_post:
        zz = v / tile * worn_post  # metres up the post
        blot = pnoise(rng, 30, n)
        foot = smooth01((0.55 + 0.06 * blot - zz) / 0.5)
        grey = np.array([0.20, 0.19, 0.16])
        c = c * (1 - 0.55 * foot[..., None]) + grey * 0.35 * foot[..., None]
        rough = rough + 0.2 * foot
        dirt = np.maximum(dirt, foot)
        scuff = smooth01((pnoise(rng, 3, n, aniso=(1.0, 4.0)) - 1.4) / 0.6) * foot
        h = h - 0.3 * scuff
        # hands polish the post between about 0.9 and 1.7 m: up to 0.10 smoother there (band median about 0.69)
        hand = smooth01(1 - np.abs(zz - 1.3 - 0.06 * pnoise(rng2, 30, n)) / 0.4)
        rough = rough - 0.10 * hand
    return np.clip(c, 0, 1), h, 0.0007, _mr(dirt, rough), tile


def gen_stone(seed, tile, n=1024):
    """Grey granite footing: speckle, broad blotches, chisel pitting, grime."""
    rng = np.random.default_rng(seed)
    blob = 0.3 * pnoise(rng, 8, n) + 0.4 * pnoise(rng, 40, n) + 0.3 * pnoise(rng, 140, n)
    sp = pnoise(rng, 0.9, n)
    sp2 = pnoise(rng, 1.4, n)
    base = np.array([0.50, 0.49, 0.46])
    c = base * (1 + 0.07 * blob[..., None])
    c = np.where((sp > 1.6)[..., None], c * 0.45, c)
    c = np.where((sp2 > 2.0)[..., None], np.minimum(c * 1.35, 1), c)
    grime = smooth01(pnoise(rng, 70, n) * 0.4 + 0.1)
    c = c * (1 - 0.25 * grime[..., None])
    pit = pnoise(rng, 2.2, n)
    pitm = smooth01((pit - 1.3) / 0.8)
    h = 0.5 + 0.15 * blob - 0.35 * pitm + 0.05 * sp
    # Roughness spread (p10..p90 about 0.70..0.91): worn and weathered patches, grime and dust in the chisel pits
    # rougher, the dark mica flecks smoother (they glint). Own generator: the colour and normal maps keep their draws.
    rng2 = np.random.default_rng(seed + 7000)
    wpatch = pnoise(rng2, 20, n)
    rough = 0.79 + 0.05 * blob + 0.07 * wpatch + 0.08 * grime + 0.10 * pitm - 0.10 * (sp > 1.6)
    return np.clip(c, 0, 1), h, 0.003, _mr(grime, rough), tile


def gen_wrapping(seed, tile, n=1024):
    """Kraft / washi wrapping paper: fibres in two directions, flecks, soft creases."""
    rng = np.random.default_rng(seed)
    f1 = pnoise(rng, 0.9, n, aniso=(1.0, 8.0))   # long fibres along v
    f2 = pnoise(rng, 0.9, n, aniso=(8.0, 1.0))   # and across
    mid = pnoise(rng, 40, n)
    blot = pnoise(rng, 8, n)
    fl = pnoise(rng, 1.5, n)
    crease = np.abs(pnoise(rng, 90, n))
    crease = smooth01(1 - crease / 0.05) * 0.6
    base = np.array([0.66, 0.55, 0.40])
    c = base * (1 + 0.02 * f1[..., None] + 0.02 * f2[..., None] + 0.05 * mid[..., None] + 0.03 * blot[..., None])
    c = np.where((fl > 3.1)[..., None], c * 0.88, c)
    c = c * (1 - 0.12 * crease[..., None])
    h = 0.5 + 0.025 * (f1 + f2) - 0.4 * crease
    # Roughness spread (p10..p90 about 0.77..0.95): fibre direction, broad tone, handled patches smoother (grease from
    # hands), creases rougher (broken fibres). Own generator: the colour and normal maps keep their draws.
    rng2 = np.random.default_rng(seed + 7000)
    wpatch = pnoise(rng2, 12, n)
    handled = smooth01((pnoise(rng2, 40, n) - 0.8) / 0.8)
    rough = 0.87 + 0.02 * f1 + 0.02 * f2 + 0.03 * mid + 0.05 * wpatch - 0.10 * handled + 0.06 * crease
    return np.clip(c, 0, 1), h, 0.0008, _mr(0.1 * crease, rough), tile


def gen_weave(seed, tile, n=1024, colour=(0.55, 0.46, 0.33), thread=0.0025, mottle=0.08, rough=0.88):
    """Plain-weave cloth: over-under threads, slubs, dye mottling."""
    rng = np.random.default_rng(seed)
    u, v = ct._uv(tile, n)
    p = tile / round(tile / thread)
    a = np.sin(2 * math.pi * u / p)
    b = np.sin(2 * math.pi * v / p)
    warp_up = (np.floor(u / p) + np.floor(v / p)) % 2
    weave = np.where(warp_up > 0.5, 0.5 + 0.5 * np.abs(b), 0.5 + 0.5 * np.abs(a))
    slub = pnoise(rng, 3, n, aniso=(1.0, 0.05)) + pnoise(rng, 3, n, aniso=(0.05, 1.0))
    mot = pnoise(rng, 60, n)
    col = np.array(colour)
    c = col * (1 + 0.10 * (weave[..., None] - 0.5) + 0.04 * slub[..., None] + mottle * mot[..., None])
    h = weave + 0.05 * slub
    # Roughness spread (p10..p90 about rough -0.09..+0.09): thread crowns pressed smoother than the gaps between
    # them, slubs, dye mottle with its dirt, handled / weathered patches. Own generator: colour and normal keep draws.
    rng2 = np.random.default_rng(seed + 7000)
    wpatch = pnoise(rng2, 16, n)
    r = rough - 0.10 * (weave - 0.818) + 0.03 * slub + 0.02 * mot + 0.05 * wpatch
    return np.clip(c, 0, 1), h, 0.0006, _mr(smooth01(mot * 0.3 + 0.1) * 0.3, r), tile


def gen_worn(seed, tile, base, under, chips, rough_top, rough_under, metal_top=0.0, metal_under=0.0, grime=0.3,
             n=1024, streak=0.0, wear=0.05):
    """A worn coating over what is under it (paint over rusting steel, leather's dye over its hide, plating over steel):
    chips and scuffs (fraction chips) down to the under colour, mottle, streaked grime; R dirt, G roughness, B metal.
    wear: rubbed (smoother) and weathered (rougher) patches in the roughness only; with the mottle it gives the coat
    a p10..p90 spread of about 0.18 before chips and grime."""
    rng = np.random.default_rng(seed)
    mot = pnoise(rng, 40, n)
    fine = pnoise(rng, 2, n)
    chipn = pnoise(rng, 6, n) + 0.6 * pnoise(rng, 1.5, n) + 0.3 * pnoise(rng, 25, n)
    thr = float(np.quantile(chipn, 1.0 - chips)) if chips > 0 else 1e9
    m = smooth01((chipn - thr) / 0.25)
    halo = smooth01((chipn - thr + 0.5) / 0.5) * (1 - m)  # rust bleeding into the paint round a chip
    top = np.array(base) * (1 + 0.08 * mot[..., None] + 0.03 * fine[..., None])
    und = np.array(under) * (1 + 0.25 * pnoise(rng, 3, n)[..., None] + 0.1 * mot[..., None])
    c = top * (1 - m[..., None]) + und * m[..., None]
    c = c * (1 - 0.35 * halo[..., None]) + np.array(under) * 0.35 * halo[..., None] * (chips > 0)
    g = smooth01(pnoise(rng, 60, n, aniso=(1.0, 6.0)) * 0.5 + 0.1) * (1.0 + streak)
    c = c * (1 - grime * g[..., None])
    h = 0.5 - 0.35 * m + 0.03 * mot + 0.02 * fine
    rng2 = np.random.default_rng(seed + 7000)  # own generator: the colour and normal maps keep their draws
    rub = pnoise(rng2, 10, n)
    rough = rough_top * (1 - m) + rough_under * m + 0.05 * mot + 0.15 * grime * g + wear * rub
    metal = metal_top * (1 - m) + metal_under * m
    mr = np.dstack([np.clip(np.maximum(g * grime, 0.6 * m), 0, 1), np.clip(rough, 0.02, 1), np.clip(metal, 0, 1)])
    return np.clip(c, 0, 1), h, 0.0004, mr, tile


TEXTURED.update({
    "hc_timber": (lambda: gen_timber(21, 0.8), 0.65),
    "hc_timber_post": (lambda: gen_timber(22, 0.8, worn_post=4.2), 0.7),
    "hc_stone": (lambda: gen_stone(23, 1.0), 0.85),
    "hc_wrapping": (lambda: gen_wrapping(24, 0.4), 0.86),
    "hc_cloth": (lambda: gen_weave(25, 0.3, colour=(0.52, 0.44, 0.31), thread=0.0028), 0.9),
    "hc_bannercloth": (lambda: gen_weave(26, 0.2, colour=(0.13, 0.20, 0.36), thread=0.0011, mottle=0.035,
                                         rough=0.86), 0.86),
})
# The worn post map covers the post's height in v (0.8 m across in u).
TILE.update({"hc_timber": 0.8, "hc_timber_post": 0.8, "hc_stone": 1.0, "hc_wrapping": 0.4, "hc_cloth": 0.3,
             "hc_bannercloth": 0.2})
# The bicycle: dark green-black enamel chipped to rust, a rusting wire basket, worn plated steel, a worn black saddle.
TEXTURED.update({
    # (the second number is the fallback roughness, unused while the MR map exists: set to the map's median)
    "hc_bikepaint": (lambda: gen_worn(31, 0.5, (0.020, 0.032, 0.026), (0.16, 0.065, 0.025), 0.06, 0.46, 0.85,
                                      grime=0.25), 0.47),
    # grey scale over rusting iron, not bare metal (metal_top was 0.8 on a name without a metal token)
    "hc_rust": (lambda: gen_worn(32, 0.4, (0.13, 0.12, 0.11), (0.20, 0.075, 0.03), 0.45, 0.55, 0.9, metal_top=0.0,
                                 grime=0.35, streak=0.5), 0.67),
    "hc_steel": (lambda: gen_worn(33, 0.4, (0.50, 0.49, 0.47), (0.17, 0.08, 0.04), 0.04, 0.46, 0.85, metal_top=1.0,
                                  grime=0.3), 0.47),
    "hc_saddle": (lambda: gen_worn(34, 0.3, (0.018, 0.016, 0.015), (0.10, 0.07, 0.05), 0.12, 0.47, 0.8, grime=0.2),
                  0.49),
})
TILE.update({"hc_bikepaint": 0.5, "hc_rust": 0.4, "hc_steel": 0.4, "hc_saddle": 0.3})
MATERIALS["hc_rubber"] = ((0.022, 0.021, 0.020), 0.86, 0.0)
MATERIALS["hc_reflector"] = ((0.32, 0.015, 0.008), 0.42, 0.0)  # dusty, weathered plastic (was 0.25)
MATERIALS["hc_lens"] = ((0.55, 0.53, 0.46), 0.42, 0.0)  # opaque stand-in for a fogged, aged lens (was 0.08)


def gen_sooted(seed, tile, soot_v, fade, n=1024):
    """Worn aluminium (gen_worn's scuffs, scratches and grime) blackened by the flame from v = 0 (the lathe's bottom
    pole) up to about soot_v metres along the profile: a ragged edge in licks, crusty, rough, not metallic."""
    c, h, depth, mr, _ = gen_worn(seed, tile, (0.46, 0.45, 0.43), (0.27, 0.26, 0.25), 0.05, 0.48, 0.42,
                                  metal_top=1.0, metal_under=1.0, grime=0.35, n=n, streak=0.4)
    rng = np.random.default_rng(seed + 100)
    u, v = ct._uv(tile, n)
    licks = pnoise(rng, 25, n, aniso=(1.0, 8.0))  # varies round the pot, nearly constant up it
    edge = soot_v + 0.05 * licks + 0.012 * pnoise(rng, 5, n)
    s = smooth01((edge - v) / fade)
    crust = pnoise(rng, 3, n)
    soot = np.clip(s * (0.88 + 0.12 * smooth01(pnoise(rng, 12, n) * 0.5 + 0.5)), 0, 1)
    col = np.array([0.018, 0.016, 0.014]) * (1 + 0.25 * crust[..., None])
    c = c * (1 - soot[..., None]) + col * soot[..., None]
    h = h + 0.08 * soot * crust
    mr = mr.copy()
    mr[..., 0] = np.maximum(mr[..., 0], soot)
    mr[..., 1] = np.clip(mr[..., 1] * (1 - soot) + (0.86 + 0.05 * crust) * soot, 0.02, 1)
    mr[..., 2] = mr[..., 2] * (1 - 0.9 * soot)
    return np.clip(c, 0, 1), h, depth, mr, tile


# The stock pot: its outside sooted from the base, its inside, lid and broth clean. Each tile divides its lathe's
# circumference a whole number of times (2), so the map has no seam round the pot.
POT_R, POT_H, POT_T, POT_BEAD, POT_LID_R = 0.225, 0.335, 0.003, 0.0075, 0.236
_POT_OUT_TILE = 2 * math.pi * (POT_R + POT_BEAD) / 2
_POT_IN_TILE = 2 * math.pi * (POT_R - 0.001) / 2
TEXTURED.update({
    # hc_pot_metal_soot / hc_pot_metal were hc_potsoot / hc_potmetal: metal 1 needs a whole metal token in the name
    # (the gloss guard splits names at "_"; "potmetal" is not "metal")
    "hc_pot_metal_soot": (lambda: gen_sooted(41, _POT_OUT_TILE, 0.30, 0.10), 0.54),
    "hc_pot_metal": (lambda: gen_worn(42, _POT_IN_TILE, (0.47, 0.46, 0.44), (0.30, 0.29, 0.28), 0.04, 0.48, 0.42,
                                     metal_top=1.0, metal_under=1.0, grime=0.3), 0.48),
})
TILE.update({"hc_pot_metal_soot": _POT_OUT_TILE, "hc_pot_metal": _POT_IN_TILE})
MATERIALS["hc_broth_water"] = ((0.11, 0.055, 0.020), 0.08, 0.0)     # amber stock, glossy (a liquid: needs a whitelist token)
MATERIALS["hc_brass"] = ((0.45, 0.32, 0.12), 0.45, 1.0)       # gas valve, tarnished and greasy (was 0.38)
# The bottles: opaque dark glass (the only slot named glass), a red plastic cap, label ink.
MATERIALS["hc_glass"] = ((0.030, 0.022, 0.012), 0.06, 0.0)
MATERIALS["hc_cap_red"] = ((0.34, 0.025, 0.018), 0.45, 0.0)   # moulded plastic cap (was 0.35)
MATERIALS["hc_ink"] = ((0.012, 0.011, 0.010), 0.7, 0.0)


def material(name):
    if name in _MATS:
        return _MATS[name]
    if name in TEXTURED:
        gen, rough = TEXTURED[name]
        rgb, h, depth, mr, tile = gen()
        bc, nm, m = _tex("T_" + name, rgb, h, tile, depth, mr, rgb.shape[0])
        mat = ct.mat_tex(name, bc, nm, rough, mr=m)
        # print only (the import's gloss guard is the gate): the map's roughness spread and metal, as the guard reads
        p10, p50, p90 = (float(x) for x in np.quantile(mr[..., 1], (0.1, 0.5, 0.9)))
        print(f"{name}: MR roughness p10 {p10:.3f} p50 {p50:.3f} p90 {p90:.3f} spread {p90 - p10:.3f}, "
              f"metal mean {float(mr[..., 2].mean()):.2f}" + ("" if p90 - p10 >= 0.15 else " (spread under 0.15)"))
    else:
        col, rough, metal = MATERIALS[name]
        mat = bpy.data.materials.new(name)
        mat.use_nodes = True
        bsdf = mat.node_tree.nodes.get("Principled BSDF")
        bsdf.inputs["Base Color"].default_value = (*col, 1.0)
        bsdf.inputs["Roughness"].default_value = rough
        bsdf.inputs["Metallic"].default_value = metal
        mat.diffuse_color = (*col, 1.0)
    _MATS[name] = mat
    return mat


# =====================================================================================================================
# Geometry: temp bmesh primitives merged into one Builder per piece
# =====================================================================================================================

AX = {"x": 0, "y": 1, "z": 2}


class Builder:
    """Accumulates parts into one bmesh with a UV layer and per-face material index."""

    def __init__(self, name):
        self.name = name
        self.bm = bmesh.new()
        self.uv = self.bm.loops.layers.uv.new("UVMap")
        self.slots = []

    def slot(self, mat):
        if mat not in self.slots:
            self.slots.append(mat)
        return self.slots.index(mat)

    def add(self, src, mat, M=None, uv="box", grain="z", smooth=None, uvoff=None, uvscale=1.0, crease=45):
        """Merge the temp bmesh src. uv="box": box projection in the part's own frame, v along its grain axis (the
        end faces get concentric end-grain rings); uv="keep": src's own UV layer (metres). Both / the slot's tile."""
        idx = self.slot(mat)
        tile = TILE.get(mat, 1.0) / uvscale
        if uvoff is None:
            uvoff = (0.0, 0.0)
        sl = src.loops.layers.uv.active
        loops_uv = []
        g = AX[grain]
        for f in src.faces:
            if uv == "keep" and sl is not None:
                loops_uv.append([(l[sl].uv[0] / tile + uvoff[0], l[sl].uv[1] / tile + uvoff[1]) for l in f.loops])
                continue
            n = f.normal
            a = max(range(3), key=lambda k: abs(n[k]))
            others = [k for k in range(3) if k != a]
            if a == g:  # end grain: rings round the face's centre (off-centre pith), u = radius
                c = f.calc_center_median()
                pith = [c[k] + 0.17 * ((-1) ** k) * 0.05 for k in range(3)]
                loops_uv.append([(math.hypot(l.vert.co[others[0]] - pith[others[0]],
                                             l.vert.co[others[1]] - pith[others[1]]) / tile + uvoff[0],
                                  0.37 + uvoff[1]) for l in f.loops])
            else:
                ua = others[0] if others[1] == g else others[1]
                va = g if g in others else others[1]
                loops_uv.append([(l.vert.co[ua] / tile + uvoff[0], l.vert.co[va] / tile + uvoff[1]) for l in f.loops])
        if M is not None:
            src.transform(M)
        src.normal_update()
        # creases sharper than 45 degrees stay hard on smooth-shaded parts (lips, rims, ribbon edges)
        sharp = set()
        for e in src.edges:
            if len(e.link_faces) == 2 and e.calc_face_angle(0.0) > math.radians(crease):
                sharp.add(e)
        vmap = {v: self.bm.verts.new(v.co) for v in src.verts}
        for f, uvs in zip(src.faces, loops_uv):
            try:
                nf = self.bm.faces.new([vmap[v] for v in f.verts])
            except ValueError:
                continue
            nf.material_index = idx
            nf.smooth = f.smooth if smooth is None else smooth
            for l, t, ls in zip(nf.loops, uvs, f.loops):
                l[self.uv].uv = t
                if ls.edge in sharp:
                    l.edge.smooth = False
        src.free()

    def finish(self):
        me = bpy.data.meshes.new(self.name)
        self.bm.to_mesh(me)
        self.bm.free()
        for m in self.slots:
            me.materials.append(material(m))
        ob = bpy.data.objects.new(self.name, me)
        bpy.context.scene.collection.objects.link(ob)
        return ob

    def tris(self):
        return sum(len(f.verts) - 2 for f in self.bm.faces)


def bm_box(x0, y0, z0, x1, y1, z1, bevel=0.0, seg=1, jitter=0.0, rng=None):
    """Closed box, every edge chamfered (seg 1) or rounded (seg 2+); optional vertex jitter (worn, uneven)."""
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    cx, cy, cz = (x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2
    for v in bm.verts:
        v.co = Vector((cx + v.co.x * abs(x1 - x0), cy + v.co.y * abs(y1 - y0), cz + v.co.z * abs(z1 - z0)))
    if bevel > 0:
        bmesh.ops.bevel(bm, geom=list(bm.edges) + list(bm.verts), offset=bevel, segments=seg, profile=0.5,
                        affect="EDGES", clamp_overlap=True)
    if jitter > 0 and rng is not None:
        for v in bm.verts:
            v.co += Vector((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1))) * jitter
    bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
    for f in bm.faces:
        f.smooth = False
    return bm


def bm_lathe(profile, seg=SEG, phase=0.0, smooth=True, rfun=None):
    """Surface of revolution about +Z; profile [(r, z)] from bottom to top (r = 0 ends are poles). UVs in metres."""
    bm = bmesh.new()
    uvl = bm.loops.layers.uv.new("UVMap")
    rings = []
    for r, z in profile:
        if r < 1e-6:
            rings.append([bm.verts.new((0.0, 0.0, z))] * seg)
            continue
        ring = []
        for k in range(seg):
            a = phase + 2 * math.pi * k / seg
            rr = r * (rfun(a, z) if rfun else 1.0)
            ring.append(bm.verts.new((rr * math.cos(a), rr * math.sin(a), z)))
        rings.append(ring)
    vlen = [0.0]
    for i in range(1, len(profile)):
        (r0, z0), (r1, z1) = profile[i - 1], profile[i]
        vlen.append(vlen[-1] + math.hypot(r1 - r0, z1 - z0))
    circ = 2 * math.pi * max(r for r, _ in profile)
    for i in range(len(rings) - 1):
        for k in range(seg):
            k1 = (k + 1) % seg
            a, b, c, d = rings[i][k], rings[i][k1], rings[i + 1][k1], rings[i + 1][k]
            us = (k / seg * circ, (k + 1) / seg * circ)
            quad = [(a, (us[0], vlen[i])), (b, (us[1], vlen[i])), (c, (us[1], vlen[i + 1])), (d, (us[0], vlen[i + 1]))]
            quad = [q for j, q in enumerate(quad) if q[0] not in [p[0] for p in quad[:j]]]
            if len(quad) < 3:
                continue
            try:
                f = bm.faces.new([q[0] for q in quad])
            except ValueError:
                continue
            f.smooth = smooth
            for l, q in zip(f.loops, quad):
                l[uvl].uv = q[1]
    bm.normal_update()
    return bm


def frames(pts):
    """Parallel-transport frames along a polyline: [(T, N, B)]."""
    P = [Vector(p) for p in pts]
    T = []
    for i in range(len(P)):
        d = P[min(i + 1, len(P) - 1)] - P[max(i - 1, 0)]
        T.append(d.normalized())
    up = Vector((0, 0, 1)) if abs(T[0].z) < 0.9 else Vector((1, 0, 0))
    N = (up - T[0] * up.dot(T[0])).normalized()
    out = []
    for t in T:
        N = (N - t * N.dot(t))
        N = N.normalized() if N.length > 1e-9 else (Vector((1, 0, 0)) - t * t.x).normalized()
        out.append((t, N, t.cross(N)))
    return P, out


def bm_tube(pts, r, seg=6, caps=True, smooth=True, squash=1.0):
    """Tube along a polyline; r a float or a per-point list. UVs: u around (m), v along (m)."""
    P, F = frames(pts)
    rs = r if isinstance(r, (list, tuple)) else [r] * len(P)
    bm = bmesh.new()
    uvl = bm.loops.layers.uv.new("UVMap")
    rings = []
    for p, (t, n, b), rr in zip(P, F, rs):
        rings.append([bm.verts.new(p + rr * (math.cos(2 * math.pi * k / seg) * n +
                                             squash * math.sin(2 * math.pi * k / seg) * b)) for k in range(seg)])
    L = [0.0]
    for i in range(1, len(P)):
        L.append(L[-1] + (P[i] - P[i - 1]).length)
    circ = 2 * math.pi * max(rs)
    for i in range(len(P) - 1):
        for k in range(seg):
            k1 = (k + 1) % seg
            f = bm.faces.new((rings[i][k], rings[i][k1], rings[i + 1][k1], rings[i + 1][k]))
            f.smooth = smooth
            for l, uv in zip(f.loops, ((k / seg * circ, L[i]), ((k + 1) / seg * circ, L[i]),
                                       ((k + 1) / seg * circ, L[i + 1]), (k / seg * circ, L[i + 1]))):
                l[uvl].uv = uv
    if caps:
        for ring, rev in ((rings[0], True), (rings[-1], False)):
            if min(rs[0], rs[-1]) < 0:
                continue
            f = bm.faces.new(list(reversed(ring)) if rev else ring)
            f.smooth = False
            for l in f.loops:
                l[uvl].uv = (l.vert.co.x, l.vert.co.y)
    bm.normal_update()
    return bm


def bm_shell(top, bot, uvs=None, smooth=True):
    """Closed slab between two grids of points (rows of Vectors, same shape): top sheet, bottom sheet and rim."""
    bm = bmesh.new()
    uvl = bm.loops.layers.uv.new("UVMap")
    nj, ni = len(top), len(top[0])
    T = [[bm.verts.new(p) for p in r] for r in top]
    Bt = [[bm.verts.new(p) for p in r] for r in bot]
    uvof = {}
    for j in range(nj):
        for i in range(ni):
            uv = uvs[j][i] if uvs else (i * 0.01, j * 0.01)
            uvof[T[j][i]] = uv
            uvof[Bt[j][i]] = uv
    faces = []
    for j in range(nj - 1):
        for i in range(ni - 1):
            faces.append((T[j][i], T[j][i + 1], T[j + 1][i + 1], T[j + 1][i]))
            faces.append((Bt[j][i], Bt[j + 1][i], Bt[j + 1][i + 1], Bt[j][i + 1]))
    ring = ([(0, i) for i in range(ni)] + [(j, ni - 1) for j in range(1, nj)] +
            [(nj - 1, i) for i in range(ni - 2, -1, -1)] + [(j, 0) for j in range(nj - 2, 0, -1)])
    for k in range(len(ring)):
        (a, b), (c, d) = ring[k], ring[(k + 1) % len(ring)]
        faces.append((T[a][b], Bt[a][b], Bt[c][d], T[c][d]))
    for fv in faces:
        try:
            f = bm.faces.new(fv)
        except ValueError:
            continue
        f.smooth = smooth
        for l in f.loops:
            l[uvl].uv = uvof[l.vert]
    bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
    return bm


def bm_prism(poly, y0, y1, bevel=0.0):
    """A polygon [(x, z)] in the XZ plane extruded from y0 to y1 (closed), optionally chamfered."""
    bm = bmesh.new()
    n = len(poly)
    a = [bm.verts.new((x, y0, z)) for x, z in poly]
    b = [bm.verts.new((x, y1, z)) for x, z in poly]
    bm.faces.new(a)
    bm.faces.new(list(reversed(b)))
    for i in range(n):
        bm.faces.new((a[i], a[(i + 1) % n], b[(i + 1) % n], b[i]))
    bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
    if bevel > 0:
        bmesh.ops.bevel(bm, geom=list(bm.edges), offset=bevel, segments=1, profile=0.5, affect="EDGES",
                        clamp_overlap=True)
        bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
    for f in bm.faces:
        f.smooth = False
    return bm


def bm_torus(major, minor, seg=16, tube=6, smooth=True):
    """Torus about Z at the origin."""
    bm = bmesh.new()
    uvl = bm.loops.layers.uv.new("UVMap")
    grid = []
    for i in range(seg):
        a = 2 * math.pi * i / seg
        row = []
        for j in range(tube):
            b = 2 * math.pi * j / tube
            rr = major + minor * math.cos(b)
            row.append(bm.verts.new((rr * math.cos(a), rr * math.sin(a), minor * math.sin(b))))
        grid.append(row)
    for i in range(seg):
        for j in range(tube):
            i1, j1 = (i + 1) % seg, (j + 1) % tube
            f = bm.faces.new((grid[i][j], grid[i1][j], grid[i1][j1], grid[i][j1]))
            f.smooth = smooth
            for l, (u, v) in zip(f.loops, ((i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1))):
                l[uvl].uv = (u / seg * 2 * math.pi * major, v / tube * 2 * math.pi * minor)
    bm.normal_update()
    return bm


def T(x=0.0, y=0.0, z=0.0):
    return Matrix.Translation((x, y, z))


def R(deg, axis):
    return Matrix.Rotation(math.radians(deg), 4, axis)


def S(x, y=None, z=None):
    y = x if y is None else y
    z = x if z is None else z
    return Matrix.Diagonal((x, y, z, 1.0))


# =====================================================================================================================
# Invented brush marks
# =====================================================================================================================

def _lerp2(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)


def _bez(p0, p1, p2, p3, t):
    a = 1 - t
    return (a ** 3 * p0[0] + 3 * a * a * t * p1[0] + 3 * a * t * t * p2[0] + t ** 3 * p3[0],
            a ** 3 * p0[1] + 3 * a * a * t * p1[1] + 3 * a * t * t * p2[1] + t ** 3 * p3[1])


def brush_mark(seed, h, w, n=None, wmin=0.01, wmax=0.03, N=16):
    """An invented abstract brush mark inside a w x h box centred on (0, 0) (see _brush_column). No letter, script
    or figure is imitated. n is kept for callers and unused. Returns [(centreline [(s, t)], widths [m])]."""
    rng = random.Random(seed)
    # Every mark is the flowing composition (water or smoke, never a figure or a letter); on a wide field it runs across.
    if w > 1.15 * h:
        return [([(t, -s) for (s, t) in pts], ws) for (pts, ws) in _brush_column(rng, seed, w, h, wmin, wmax, N)]
    return _brush_column(rng, seed, h, w, wmin, wmax, N)


def _stroke(p0, p1, p2, p3, wm, press, N, seed, wmin):
    pts, ws = [], []
    for i in range(N):
        t = i / (N - 1)
        pts.append(_bez(p0, p1, p2, p3, t))
        if press == "press":  # loaded at the start, thinning out as the brush lifts
            wt = wm * (0.4 + 0.6 * math.sin(0.5 * math.pi * min(t / 0.22, 1.0))) * (1 - t) ** 0.5
        elif press == "taper":  # enters thin, swells, leaves thin
            wt = wm * (0.25 + 0.75 * math.sin(math.pi * t) ** 0.8)
        else:
            wt = wm * math.sin(math.pi * t) ** 0.6
        wt *= 1 + 0.07 * math.sin(7 * t + seed)
        ws.append(max(wt, wmin * 0.35))
    return pts, ws


def _brush_column(rng, seed, h, w, wmin, wmax, N):
    """For tall boards: a flowing gesture like water or smoke, not a figure. One long gentle S sweep down the board,
    a shorter sweep running beside it (same curve, offset, never touching), a third low and short on the other side,
    and the main sweep running dry into bristle trails at its foot."""
    flip = -1.0 if rng.random() < 0.5 else 1.0
    out = []
    sw = rng.uniform(0.12, 0.2) * w
    main = ((-0.12 * w, 0.48 * h), (sw + 0.1 * w, 0.2 * h), (-sw - 0.1 * w, -0.15 * h), (0.1 * w, -0.4 * h))
    out.append(main + (rng.uniform(0.9, 1.0) * wmax, "taper"))

    def sub(ta, tb, dx, dy=0.0):
        # the main curve's piece between ta and tb, shifted by (dx, dy): de Casteljau split, twice
        def split(c, t):
            a, b_, c_, d = c
            ab = _lerp2(a, b_, t)
            bc = _lerp2(b_, c_, t)
            cd = _lerp2(c_, d, t)
            abc = _lerp2(ab, bc, t)
            bcd = _lerp2(bc, cd, t)
            m = _lerp2(abc, bcd, t)
            return (a, ab, abc, m), (m, bcd, cd, d)
        _, right = split(main, ta)
        piece, _ = split(right, (tb - ta) / (1 - ta))
        return tuple((p[0] + dx, p[1] + dy) for p in piece)

    gap = 0.75 * wmax + 0.06 * w
    out.append(sub(rng.uniform(0.08, 0.14), rng.uniform(0.48, 0.56), gap, -0.02 * h) +
               (rng.uniform(0.5, 0.6) * wmax, "taper"))
    out.append(sub(rng.uniform(0.56, 0.62), rng.uniform(0.8, 0.86), -gap * 0.9, 0.0) +
               (rng.uniform(0.38, 0.48) * wmax, "taper"))
    # dry brush at the foot: thin bristle trails carrying on from the main sweep's end
    e = _bez(*main, 0.9)
    d = (main[3][0] - main[2][0], main[3][1] - main[2][1])
    dl = math.hypot(*d) or 1.0
    d = (d[0] / dl, d[1] / dl)
    nrm = (-d[1], d[0])
    for j in range(3):
        o = (j - 1) * 0.28 * wmax
        L = rng.uniform(0.06, 0.1) * h
        p0 = (e[0] + nrm[0] * o, e[1] + nrm[1] * o)
        out.append((p0, (p0[0] + d[0] * L * 0.33, p0[1] + d[1] * L * 0.33),
                    (p0[0] + d[0] * L * 0.66 + nrm[0] * o * 0.3, p0[1] + d[1] * L * 0.66),
                    (p0[0] + d[0] * L + nrm[0] * o * 0.6, p0[1] + d[1] * L), rng.uniform(0.14, 0.22) * wmax, "press"))
    res = []
    for (p0, p1, p2, p3, wm, press) in out:
        p0, p1, p2, p3 = [(flip * p[0], p[1]) for p in (p0, p1, p2, p3)]
        res.append(_stroke(p0, p1, p2, p3, wm, press, N, seed, wmin))
    return res


def add_mark(b, strokes, surf, mat, proud=(0.0007, 0.0009), embed=0.0004):
    """Each stroke as a closed ribbon conforming to surf(s, t) -> (point, outward normal): its top proud of the
    surface, its underside just under it. Strokes sit at slightly different heights so crossings don't fight."""
    for k, (pts, ws) in enumerate(strokes):
        pr = proud[0] + (proud[1] - proud[0]) * (k % 3) / 2.0
        top, bot, uvs = [], [], []
        for side in (-1.0, 0.0, 1.0):
            rt, rb, ru = [], [], []
            for i, ((s, t), w) in enumerate(zip(pts, ws)):
                j0, j1 = max(i - 1, 0), min(i + 1, len(pts) - 1)
                dx, dy = pts[j1][0] - pts[j0][0], pts[j1][1] - pts[j0][1]
                L = math.hypot(dx, dy) or 1.0
                nx, ny = -dy / L, dx / L
                ss, tt = s + side * nx * w / 2, t + side * ny * w / 2
                p, nrm = surf(ss, tt)
                rt.append(p + nrm * pr)
                rb.append(p - nrm * embed)
                ru.append((ss, tt))
            top.append(rt)
            bot.append(rb)
            uvs.append(ru)
        b.add(bm_shell(top, bot, uvs, smooth=True), mat, uv="keep")


# =====================================================================================================================
# Lanterns
# =====================================================================================================================

def lantern(name, height, diameter, hoops, cord, mark_seed=None):
    """A paper lantern hanging from its hook: the hook's top at the origin, the body below, the pull cord under it."""
    b = Builder(name)
    s = height / 0.6
    rng = random.Random(1609 + hoops)
    cap_r = 0.13 * s
    cap_h = 0.06 * s
    hook_h = 0.03 * s
    top = -hook_h
    body_top = top - cap_h
    body_bot = -height + cap_h
    R0 = diameter / 2
    zc = 0.5 * (body_top + body_bot)
    hz = 0.5 * (body_top - body_bot)
    phase = math.pi / SEG  # a facet's middle faces +-X and +-Y... vertices at +-Y for the marks: see below
    phase = 0.0

    def rp(z):
        e = math.sqrt(max(0.0, 1.0 - ((z - zc) / (hz * 1.06)) ** 2))
        return max(cap_r * 0.92, R0 * e)

    # The paper: a smooth ellipsoid of revolution between the caps (the hoops are their own geometry).
    nz = 24
    zs = [body_bot - 0.004 * s + (body_top - body_bot + 0.008 * s) * i / nz for i in range(nz + 1)]
    prof = [(rp(z), z) for z in zs]
    b.add(bm_lathe(prof, SEG, phase), "lantern_paper", uv="keep")

    def rpoly(z):  # the lathe's ring radius at any z (piecewise linear between its rings)
        if z <= zs[0]:
            return prof[0][0]
        for i in range(1, len(zs)):
            if z <= zs[i]:
                t = (z - zs[i - 1]) / (zs[i] - zs[i - 1])
                return prof[i - 1][0] * (1 - t) + prof[i][0] * t
        return prof[-1][0]

    def facet(a):  # a ring's polygon radius factor at angle a (1 at the vertices, cos(pi/SEG) mid-facet)
        step = 2 * math.pi / SEG
        d = (a - phase) % step - step / 2
        return math.cos(step / 2) / math.cos(d)

    # Bamboo hoops: 16 (large) / 14 (small), spacing jittered ~15%, each 1-2 mm proud with a small tilt and wobble.
    span0, span1 = body_bot + 0.012 * s, body_top - 0.012 * s
    pitch = (span1 - span0) / (hoops - 1)
    zr = [span0 + i * pitch + (rng.uniform(-0.15, 0.15) * pitch if 0 < i < hoops - 1 else 0.0) for i in range(hoops)]
    hw = 0.0022 * s ** 0.5
    for z0 in zr:
        proud = rng.uniform(0.0010, 0.0020)
        tilt = rng.uniform(0.002, 0.004) * s
        tphi = rng.uniform(0, 2 * math.pi)
        bm = bmesh.new()
        uvl = bm.loops.layers.uv.new("UVMap")
        cols = []
        for k in range(SEG):
            a = phase + 2 * math.pi * k / SEG
            z = z0 + tilt * math.cos(a - tphi)
            r = rpoly(z)
            dr = (rpoly(z + 0.002) - rpoly(z - 0.002)) / 0.004
            nr, nzv = 1.0, -dr
            L = math.hypot(nr, nzv)
            nr, nzv = nr / L, nzv / L
            ur, uz = -nzv, nr  # up the meridian
            pr = proud + rng.uniform(-0.0003, 0.0003)
            cs, sn = math.cos(a), math.sin(a)

            def P(off_n, off_u):
                rr = r + nr * off_n + ur * off_u
                return bm.verts.new((rr * cs, rr * sn, z + nzv * off_n + uz * off_u))
            cols.append([P(-0.0004, -hw), P(pr, 0.0), P(-0.0004, hw)])  # a round cane: smooth-shaded ridge
        circ = 2 * math.pi * R0
        for k in range(SEG):
            k1 = (k + 1) % SEG
            for j in range(2):
                f = bm.faces.new((cols[k][j], cols[k][j + 1], cols[k1][j + 1], cols[k1][j]))
                f.smooth = True
                for l, uv in zip(f.loops, ((k, j), (k, j + 1), (k + 1, j + 1), (k + 1, j))):
                    l[uvl].uv = (uv[0] / SEG * circ, uv[1] * 0.003)
        bm.normal_update()
        # outward check: the outer face's normal must point away from the axis
        bm.faces.ensure_lookup_table()
        f0 = bm.faces[1] if len(bm.faces) > 1 else None
        if f0 is not None:
            c = f0.calc_center_median()
            if f0.normal.dot(Vector((c.x, c.y, 0.0))) < 0:
                bmesh.ops.reverse_faces(bm, faces=list(bm.faces))
        b.add(bm, "lantern_rib", uv="keep", crease=180)

    # Caps: lacquered rings, taller, a lip flaring over the paper and at the outer end, a rim bead round the middle.
    def cap(z_in, z_out):
        d = 1 if z_out > z_in else -1
        h = abs(z_out - z_in)
        pts = [(cap_r * 0.90, z_in - d * 0.006 * s), (cap_r * 1.08, z_in - d * 0.003 * s),
               (cap_r * 1.08, z_in + d * 0.004 * s), (cap_r * 1.0, z_in + d * 0.008 * s),
               (cap_r * 1.0, z_in + d * 0.42 * h), (cap_r * 1.05, z_in + d * 0.48 * h),
               (cap_r * 1.05, z_in + d * 0.58 * h), (cap_r * 1.0, z_in + d * 0.64 * h),
               (cap_r * 1.0, z_out - d * 0.012 * s), (cap_r * 1.07, z_out - d * 0.009 * s),
               (cap_r * 1.07, z_out - d * 0.002 * s), (cap_r * 0.9, z_out), (0.0, z_out + d * 0.002 * s)]
        if d < 0:
            pts = list(reversed(pts))
        b.add(bm_lathe(pts, SEG, phase, smooth=True), "lantern_wood", uv="keep")
    cap(body_top, top)
    cap(body_bot, -height)

    # The hook: a small iron ring standing on the top cap, its top at the origin.
    ring_r = hook_h * 0.5
    b.add(bm_torus(ring_r - 0.003 * s, 0.003 * s, 12, 6), "lantern_iron", M=T(0, 0, top + ring_r) @ R(90, "X"),
          uv="keep")
    # The pull cord with a slight bend, a knot under the cap and a turned wooden bead at its end.
    cr = 0.0024 * s ** 0.5
    pts = []
    nseg = 12
    for i in range(nseg + 1):
        t = i / nseg
        pts.append((0.004 * s * math.sin(t * 2.2), 0.003 * s * math.sin(t * 3.1 + 0.5), -height - 0.002 - cord * t))
    b.add(bm_tube(pts, cr, seg=6), "lantern_cord", uv="keep")
    b.add(bm_lathe([(0.0, -height - 0.004), (cr * 2.0, -height - 0.008), (cr * 2.2, -height - 0.014),
                    (cr * 1.4, -height - 0.020), (0.0, -height - 0.022)], 8), "lantern_cord", uv="keep")
    br = 0.011 * s ** 0.5
    end = Vector(pts[-1])
    bead = [(0.0, -br * 1.05), (br * 0.55, -br * 0.95), (br * 0.92, -br * 0.5), (br, 0.0), (br * 0.92, br * 0.5),
            (br * 0.55, br * 0.95), (0.0, br * 1.05)]
    b.add(bm_lathe(bead, 12), "lantern_wood", M=T(end.x, end.y, end.z), uv="keep")

    if mark_seed is not None:
        def surf_at(a0):
            def surf(sv, tv):
                z = zc + tv
                r = rpoly(z)
                a = a0 + sv / max(r, 1e-3)
                rr = r * facet(a)
                dr = (rpoly(z + 0.002) - rpoly(z - 0.002)) / 0.004
                nrm = Vector((math.cos(a), math.sin(a), -dr)).normalized()
                return Vector((rr * math.cos(a), rr * math.sin(a), z)), nrm
            return surf
        strokes = brush_mark(mark_seed, 0.18, 0.13, n=4, wmin=0.01, wmax=0.03, N=12)
        for a0 in (math.pi / 2, -math.pi / 2):
            add_mark(b, strokes, surf_at(a0), "lantern_ink", proud=(0.0005, 0.0009), embed=0.0004)
    return b.finish()


def post(name, height=4.0, side=0.14, anchor_below_top=0.2):
    b = Builder(name)
    h = side / 2
    b.add(bm_box(-h, -h, 0.0, h, h, height - 0.03), "post_timber")
    b.add(bm_box(-h - 0.012, -h - 0.012, height - 0.03, h + 0.012, h + 0.012, height), "post_iron")
    b.add(bm_box(-h - 0.01, -h - 0.01, 0.0, h + 0.01, h + 0.01, 0.25), "post_iron")
    z = height - anchor_below_top
    b.add(bm_box(-h - 0.008, -h - 0.008, z - 0.04, h + 0.008, h + 0.008, z + 0.04), "post_iron")
    return b.finish()


# =====================================================================================================================
# Small goods (local origin at the item's bottom centre)
# =====================================================================================================================

def wobble(rng, amt):
    p1, p2, p3 = rng.uniform(0, 6.3), rng.uniform(0, 6.3), rng.uniform(0, 6.3)
    k = rng.uniform(-1, 1)

    def f(a, z):
        return 1 + amt * (math.sin(2 * a + p1) * 0.6 + math.sin(3 * a + p2 + 4 * z) * 0.3 + k * 0.2 * math.sin(a + p3))
    return f


def jar(b, M, h, r, glaze, rng, seg=12):
    """Glazed storage jar: unglazed foot, belly, shoulder, neck and rim; a lid (wood or ceramic) with a knob.
    Returns the height of the lid's top."""
    f = wobble(rng, 0.015)
    foot = [(0.0, 0.0), (r * 0.6, 0.0), (r * 0.67, 0.018)]
    body = [(r * 0.67, 0.018), (r * 0.9, 0.15 * h), (r * 1.0, 0.45 * h), (r * 0.82, 0.76 * h), (r * 0.5, 0.9 * h),
            (r * 0.54, 0.97 * h), (r * 0.42, h)]
    b.add(bm_lathe(foot, seg, rfun=f), "hc_bisque", M=M, uv="keep")
    b.add(bm_lathe(body, seg, rfun=f), glaze, M=M, uv="keep")
    lid_mat = "hc_timber" if rng.random() < 0.5 else glaze
    lr = r * 0.6
    lid = [(0.0, h - 0.004), (lr, h + 0.004), (lr, h + 0.012), (lr * 0.3, h + 0.022), (lr * 0.22, h + 0.034),
           (0.0, h + 0.035)]
    b.add(bm_lathe(lid, seg), lid_mat, M=M, uv="keep")
    return h + 0.035


def tin(b, M, h, r, rng, seg=12):
    """Tinplate canister with a slip-on lid (a step at the lid's foot) and sometimes a blank paper band."""
    pts = [(0.0, 0.0), (r * 0.97, 0.0), (r * 0.985, 0.006), (r * 0.985, h - 0.024), (r * 1.012, h - 0.022),
           (r * 1.012, h - 0.003), (r * 0.97, h), (0.0, h)]
    b.add(bm_lathe(pts, seg), "hc_metal", M=M, uv="keep")
    if rng.random() < 0.5:
        z0, z1 = h * rng.uniform(0.15, 0.25), h * rng.uniform(0.55, 0.7)
        b.add(bm_lathe([(r * 0.988, z0 - 0.0005), (r * 1.004, z0), (r * 1.004, z1), (r * 0.988, z1 + 0.0005)], seg),
              "hc_wrapping", M=M, uv="keep")
    return h


def packet(b, M, w, d, h, rng, tie=True, knot=True):
    """Paper-wrapped packet: a chamfered, slightly uneven block, folded end flaps, string tied one or both ways (over
    the top and down the sides) with a knot and a loose end on top (knot=False under another packet)."""
    b.add(bm_box(-w / 2, -d / 2, 0, w / 2, d / 2, h, bevel=min(0.004, h / 5), seg=1, jitter=0.001, rng=rng),
          "hc_wrapping", M=M, uvoff=(rng.random(), rng.random()))
    for sx in (-1, 1):  # folded flaps lying on the two end faces
        x = sx * (w / 2 + 0.0006)
        poly = [(-d / 2 + 0.003, h * 0.15), (d / 2 - 0.003, h * 0.15), (0.0, h * 0.62)]
        b.add(bm_prism(poly, -0.0005, 0.0005), "hc_wrapping", M=M @ T(x, 0, 0) @ R(90, "Z"))
    if tie:
        t = 0.0017
        bands = [(-t, -d / 2 - t, h - 0.0005, t, d / 2 + t, h + t),
                 (-t, -d / 2 - t, 0.0, t, -d / 2 + 0.0005, h),
                 (-t, d / 2 - 0.0005, 0.0, t, d / 2 + t, h)]
        if rng.random() < 0.6:
            bands += [(-w / 2 - t, -t, h - 0.0005, w / 2 + t, t, h + t),
                      (-w / 2 - t, -t, 0.0, -w / 2 + 0.0005, t, h),
                      (w / 2 - 0.0005, -t, 0.0, w / 2 + t, t, h)]
        for (x0, y0, z0, x1, y1, z1) in bands:
            b.add(bm_box(x0, y0, z0, x1, y1, z1), "hc_string", M=M)
        if knot:
            b.add(bm_lathe([(0.0, h), (0.006, h + 0.002), (0.0045, h + 0.006), (0.0, h + 0.007)], 6), "hc_string",
                  M=M, uv="keep")
            b.add(bm_tube([(0.0, 0.0, h + 0.004), (0.012, 0.004, h + 0.004), (0.02, 0.012, h + 0.003)], 0.0015,
                          seg=4), "hc_string", M=M, uv="keep")
    return h


def bowl_stack(b, M, n, r, rng, glaze="hc_glaze_cream", seg=12):
    """n bowls stacked, each rim showing as a step; the top bowl hollow. Returns the stack's height."""
    pts = [(0.0, 0.0), (r * 0.42, 0.0), (r * 0.45, 0.008)]
    z = 0.008
    step = r * 0.32
    for i in range(n):
        top = z + step
        pts += [(r * 0.8, z + step * 0.45), (r, top), (r * 0.96, top + 0.0025)]
        z = top - step * 0.62 if i < n - 1 else top
    pts += [(r * 0.62, z - step * 0.7), (0.0, z - step * 0.85)]
    b.add(bm_lathe(pts, seg, rfun=wobble(rng, 0.01)), glaze, M=M, uv="keep")
    return z


def sack(b, M, h, r, rng, seg=12):
    """A small cloth sack: slumped belly, a pinched neck tied with twine and a ruffled top."""
    f = wobble(rng, 0.13)
    pts = [(0.0, 0.0), (r * 0.85, 0.004), (r * 1.06, 0.2 * h), (r * 1.0, 0.45 * h), (r * 0.78, 0.66 * h),
           (r * 0.32, 0.82 * h), (r * 0.46, 0.92 * h), (r * 0.3, h), (0.0, h * 0.96)]
    b.add(bm_lathe(pts, seg, rfun=lambda a, z: f(a, z) * (1 + 0.07 * math.sin(7 * a) * (z > 0.86 * h))), "hc_cloth",
          M=M, uv="keep")
    b.add(bm_torus(r * 0.34, 0.0025, 12, 3), "hc_string", M=M @ T(0, 0, 0.82 * h), uv="keep")
    return h


# =====================================================================================================================
# Shop fittings
# =====================================================================================================================

def shelf_unit(name):
    """1.8 w x 0.4 d x 1.9 h shelving: origin at the bottom centre of its back face, the front at y = -0.4."""
    b = Builder(name)
    rng = random.Random(1810)
    W, D, H = 1.8, 0.4, 1.9
    hw = W / 2
    tk = 0.024
    bev = 0.003
    # sides and a centre upright
    for x0 in (-hw, hw - tk):
        b.add(bm_box(x0, -D, 0.0, x0 + tk, 0.0, H, bevel=bev, seg=2), "hc_timber", grain="z",
              uvoff=(rng.random(), 0))
    b.add(bm_box(-tk / 2, -D + 0.02, 0.09, tk / 2, -0.016, H - 0.03, bevel=bev, seg=2), "hc_timber", grain="z")
    # top board with a small overhang and a cornice strip
    b.add(bm_box(-hw - 0.01, -D - 0.015, H - 0.028, hw + 0.01, 0.0, H, bevel=0.004, seg=2), "hc_timber", grain="x")
    b.add(bm_box(-hw, -D - 0.008, H - 0.06, hw, -D + 0.01, H - 0.028, bevel=0.003, seg=1), "hc_timber", grain="x")
    # kick plinth, recessed
    b.add(bm_box(-hw + tk, -D + 0.02, 0.0, hw - tk, -D + 0.04, 0.09, bevel=bev, seg=1), "hc_timber", grain="x")
    # back boards with gaps
    xb = -hw + tk
    while xb < hw - tk - 0.01:
        wdt = min(rng.uniform(0.13, 0.17), hw - tk - xb)
        b.add(bm_box(xb + 0.002, -0.014, 0.09, xb + wdt - 0.002, -0.002, H - 0.028, bevel=0.0015, seg=1), "hc_timber",
              grain="z", uvoff=(rng.random(), rng.random()))
        xb += wdt
    shelves = [0.09 + k * 0.355 for k in range(5)]
    for z in shelves:
        for (x0, x1) in ((-hw + tk, -tk / 2), (tk / 2, hw - tk)):
            b.add(bm_box(x0, -D + 0.01, z, x1, -0.014, z + 0.02, bevel=0.002, seg=1), "hc_timber", grain="x",
                  uvoff=(rng.random(), rng.random()))
            # the front lip: a rounded strip standing 22 mm over the shelf
            b.add(bm_box(x0, -D + 0.002, z, x1, -D + 0.018, z + 0.042, bevel=0.004, seg=2), "hc_timber", grain="x",
                  uvoff=(rng.random(), rng.random()))
    # stock every bay
    themes = ["jars", "tins", "packets", "bowls", "sacks"]
    for si, z in enumerate(shelves):
        clear = (shelves[si + 1] if si + 1 < len(shelves) else H - 0.028) - z - 0.02 - 0.02
        for bay, (x0, x1) in enumerate(((-hw + tk, -tk / 2), (tk / 2, hw - tk))):
            theme = themes[(si * 2 + bay * 3) % len(themes)]
            stock_bay(b, rng, x0 + 0.012, x1 - 0.012, -D + 0.03, -0.02, z + 0.02, clear, theme, si)
    print("shelf stock (count, tris):", STATS, "total", b.tris())
    return b.finish()


STATS = {}


def stock_bay(b, rng, x0, x1, y0, y1, z, clear, theme, shelf):
    """Fill a bay two rows deep: tall goods at the back, low goods at the front, every row packed edge to edge with
    small irregular gaps, every item a little turned and off-line."""
    rows = [(y1 - 0.09, 0.17, True), (y0 + 0.07, 0.15, False)]  # (centre y, depth, back row)
    mixes = {
        "jars": ["jar", "jar", "jar_small", "packet"],
        "tins": ["tin", "tin", "jar_small", "packet"],
        "packets": ["packets", "packets", "tin", "bowls"],
        "bowls": ["bowls", "bowls", "jar_small", "tin"],
        "sacks": ["sack", "sack", "jar", "packets"],
    }
    for (yc, dep, back) in rows:
        x = x0 + rng.uniform(0.0, 0.015)
        while True:
            kind = rng.choice(mixes[theme])
            if back and kind == "bowls":  # bowl stacks stay low: they go to the front
                kind = "jar"
            if back:  # the back row stands tall: 60-90% of the clear height
                hmax = clear - 0.015
                hmin = 0.6 * clear
            else:
                hmax = min(clear * 0.6, 0.19)
                hmin = 0.06
            if kind == "jar":
                r = rng.uniform(0.055, 0.075)
                wd = 2 * r
            elif kind == "jar_small":
                r = rng.uniform(0.035, 0.05)
                wd = 2 * r
            elif kind == "tin":
                r = rng.uniform(0.035, 0.05)
                wd = 2 * r
            elif kind == "bowls":
                r = rng.uniform(0.06, 0.075)
                wd = 2 * r
            elif kind == "sack":
                r = rng.uniform(0.06, 0.08)
                wd = 2 * r * 1.05
            else:
                wd = rng.uniform(0.09, 0.14)
            if x + wd > x1:
                break
            cx = x + wd / 2
            cy = yc + rng.uniform(-0.012, 0.012)
            M = T(cx, cy, z) @ R(rng.uniform(-180, 180), "Z") @ R(rng.uniform(-1.2, 1.2), "X")
            seg = 10 if back else 12
            t0 = b.tris()
            glz = rng.choice(["hc_glaze_brown", "hc_glaze_green", "hc_glaze_brown"])
            target = rng.uniform(hmin, hmax)
            if kind in ("jar", "jar_small"):
                if kind == "jar_small" and back and rng.random() < 0.5:  # two small jars, one standing on a wooden lid board on the other
                    top = jar(b, M, min(0.13, target * 0.5 - 0.035), r, glz, rng, seg)
                    b.add(bm_box(-r * 0.9, -r * 0.9, top, r * 0.9, r * 0.9, top + 0.012, bevel=0.002), "hc_timber",
                          M=M)
                    jar(b, M @ T(0, 0, top + 0.012), max(0.05, min(0.13, target - top - 0.05)), r * 0.9,
                        rng.choice(["hc_glaze_brown", "hc_glaze_green"]), rng, seg)
                else:
                    jar(b, M, max(0.06, target - 0.035), r, glz, rng, seg)
            elif kind == "tin":
                hh = rng.uniform(0.08, 0.14)
                zz = 0.0
                while zz + hh <= target + 0.01:  # tins stacked to the target height
                    tin(b, M @ T(0, 0, zz), hh, r, rng, seg)
                    zz += hh + 0.001
                if zz == 0.0:
                    tin(b, M, max(0.05, target), r, rng, seg)
            elif kind == "bowls":
                n = max(2, int((target - 0.01) / (r * 0.32 * 0.38)) - 1)
                bowl_stack(b, M, min(n, 7), r, rng, rng.choice(["hc_glaze_cream", "hc_glaze_green"]), seg)
            elif kind == "sack":
                sack(b, M, max(0.1, target), r, rng, seg)
            else:  # a stack of packets; only the top one shows its knot
                hs, zz = [], 0.0
                while True:
                    ph = rng.uniform(0.035, 0.065)
                    if zz + ph > target and hs:
                        break
                    hs.append(ph)
                    zz += ph + 0.002
                zz = 0.0
                for i, ph in enumerate(hs):
                    pd = min(dep - 0.02, rng.uniform(0.08, 0.12))
                    Mp = T(cx + rng.uniform(-0.006, 0.006), cy, z + zz) @ R(rng.uniform(-8, 8), "Z")
                    packet(b, Mp, wd - 0.01, pd, ph, rng, tie=rng.random() < 0.7, knot=i == len(hs) - 1)
                    zz += ph + 0.002
            STATS[kind] = STATS.get(kind, [0, 0])
            STATS[kind][0] += 1
            STATS[kind][1] += b.tris() - t0
            x += wd + rng.uniform(0.006, 0.018)


def counter(name):
    """2.4 w x 0.6 d x 0.95 h counter: origin at the bottom centre of its back face, the front at y = -0.6."""
    b = Builder(name)
    rng = random.Random(2406)
    W, D, H = 2.4, 0.6, 0.95
    hw = W / 2
    # carcass: corner posts, rails, an inner shelf and a back rail on the shop-keeper's side
    for x in (-hw + 0.03, hw - 0.075):
        for (y0, y1) in ((-D + 0.06, -D + 0.105), (-0.05, -0.005)):
            b.add(bm_box(x, y0, 0.0, x + 0.045, y1, H - 0.05, bevel=0.004, seg=1), "hc_timber", grain="z",
                  uvoff=(rng.random(), rng.random()))
    b.add(bm_box(-hw + 0.03, -D + 0.06, 0.32, hw - 0.03, -0.01, 0.345, bevel=0.003), "hc_timber", grain="x")
    b.add(bm_box(-hw + 0.03, -0.05, 0.62, hw - 0.03, -0.005, 0.69, bevel=0.004), "hc_timber", grain="x")
    # front panel: vertical boards with V-joints between a top and a bottom rail; a recessed kick plinth
    fy0, fy1 = -D + 0.035, -D + 0.06
    b.add(bm_box(-hw + 0.02, fy0 - 0.008, H - 0.12, hw - 0.02, fy1, H - 0.05, bevel=0.005, seg=2), "hc_timber",
          grain="x")
    b.add(bm_box(-hw + 0.02, fy0 - 0.008, 0.08, hw - 0.02, fy1, 0.15, bevel=0.005, seg=2), "hc_timber", grain="x")
    b.add(bm_box(-hw + 0.04, fy0 + 0.025, 0.0, hw - 0.04, fy0 + 0.045, 0.08, bevel=0.003), "hc_timber", grain="x")
    x = -hw + 0.02
    while x < hw - 0.03:
        wdt = min(rng.uniform(0.13, 0.16), hw - 0.02 - x)
        b.add(bm_box(x + 0.0015, fy0, 0.15, x + wdt - 0.0015, fy1, H - 0.12, bevel=0.004, seg=1), "hc_timber",
              grain="z", uvoff=(rng.random(), rng.random()))
        x += wdt
    # end panels the same way
    for sx in (-1, 1):
        xo0, xo1 = (sx * hw - 0.03, sx * hw - 0.005) if sx > 0 else (sx * hw + 0.005, sx * hw + 0.03)
        b.add(bm_box(xo0, -D + 0.035, H - 0.12, xo1, -0.0, H - 0.05, bevel=0.004), "hc_timber", grain="y")
        b.add(bm_box(xo0, -D + 0.035, 0.08, xo1, -0.0, 0.15, bevel=0.004), "hc_timber", grain="y")
        y = -D + 0.035
        while y < -0.01:
            wdt = min(rng.uniform(0.13, 0.16), -y)
            b.add(bm_box(xo0 + 0.004, y + 0.0015, 0.15, xo1 - 0.002, y + wdt - 0.0015, H - 0.12, bevel=0.004),
                  "hc_timber", grain="z", uvoff=(rng.random(), rng.random()))
            y += wdt
    # the top: a thick board with an overhang, its front edge rounded and worn uneven
    top = bm_box(-hw, -D, H - 0.05, hw, 0.0, H, bevel=0.01, seg=3)
    for v in top.verts:
        if v.co.y < -D + 0.012 and v.co.z > H - 0.012:
            v.co.z -= rng.uniform(0.0, 0.0025)
            v.co.y += rng.uniform(0.0, 0.002)
    b.add(top, "hc_timber", grain="x")
    # dressing on top: a cash box, a stack of packets, a tray with bowls, kept off the two slots Core sets pieces on
    # (ftcore kHeroCounterSlotLeft -0.56 and kHeroCounterSlotRight 0.47, each clear 0.3 m either side)
    zt = H
    Mc = T(0.98, -0.32, zt) @ R(4, "Z")
    cw, cd, chh = 0.34, 0.24, 0.13
    b.add(bm_box(-cw / 2, -cd / 2, 0.0, cw / 2, cd / 2, chh - 0.02, bevel=0.004, seg=2), "hc_timber", M=Mc, grain="x")
    b.add(bm_box(-cw / 2 - 0.004, -cd / 2 - 0.004, chh - 0.02, cw / 2 + 0.004, cd / 2 + 0.004, chh, bevel=0.005, seg=2),
          "hc_lacquer", M=Mc, grain="x")
    b.add(bm_box(-0.04, -0.004, chh - 0.001, 0.04, 0.004, chh + 0.001), "hc_iron", M=Mc)  # coin slot plate
    b.add(bm_box(-0.035, -0.0025, chh, 0.035, 0.0025, chh + 0.0012), "hc_lacquer", M=Mc)
    b.add(bm_box(-0.02, -cd / 2 - 0.007, chh - 0.055, 0.02, -cd / 2 - 0.002, chh - 0.005, bevel=0.001), "hc_iron",
          M=Mc)  # hasp
    for sx in (-1, 1):
        for sy in (-1, 1):  # L-shaped iron corner plates round the box's foot
            xa, xb = sorted((sx * (cw / 2 - 0.035), sx * (cw / 2 + 0.0015)))
            ya, yb = sorted((sy * (cd / 2 - 0.0005), sy * (cd / 2 + 0.0015)))
            b.add(bm_box(xa, ya, 0.0, xb, yb, 0.035, bevel=0.0005), "hc_iron", M=Mc)
            xa, xb = sorted((sx * (cw / 2 - 0.0005), sx * (cw / 2 + 0.0015)))
            ya, yb = sorted((sy * (cd / 2 - 0.035), sy * (cd / 2 + 0.0015)))
            b.add(bm_box(xa, ya, 0.0, xb, yb, 0.035, bevel=0.0005), "hc_iron", M=Mc)
    zz = 0.0
    for k in range(4):
        ph = rng.uniform(0.04, 0.06)
        packet(b, T(-1.0 + rng.uniform(-0.01, 0.01), -0.3, zt + zz) @ R(rng.uniform(-10, 10), "Z"),
               rng.uniform(0.17, 0.21), rng.uniform(0.12, 0.15), ph, rng, tie=k % 2 == 0)
        zz += ph + 0.001
    # tray: lacquer with a rim, three small bowls
    Mt = T(-0.05, -0.36, zt) @ R(-6, "Z")
    tw, td = 0.36, 0.24
    b.add(bm_box(-tw / 2, -td / 2, 0.0, tw / 2, td / 2, 0.01, bevel=0.003), "hc_lacquer", M=Mt)
    for (x0, y0, x1, y1) in ((-tw / 2, -td / 2, tw / 2, -td / 2 + 0.012), (-tw / 2, td / 2 - 0.012, tw / 2, td / 2),
                             (-tw / 2, -td / 2, -tw / 2 + 0.012, td / 2), (tw / 2 - 0.012, -td / 2, tw / 2, td / 2)):
        b.add(bm_box(x0, y0, 0.0, x1, y1, 0.03, bevel=0.003), "hc_lacquer", M=Mt)
    for (bx, by) in ((-0.1, 0.0), (0.0, 0.03), (0.1, -0.02)):
        bowl_stack(b, Mt @ T(bx, by, 0.01), 1, 0.045, rng, rng.choice(["hc_glaze_cream", "hc_glaze_green"]))
    return b.finish()


# =====================================================================================================================
# Lane dressing
# =====================================================================================================================

def bamboo_profile(L, r, nodes, rng):
    """Bamboo culm lathe profile along +Z, 0..L, with node ridges."""
    pts = [(0.0, 0.0), (r * 0.98, 0.0)]
    zs = sorted(nodes)
    for zn in zs:
        pts += [(r, zn - 0.006), (r * 1.08, zn - 0.002), (r * 1.08, zn + 0.002), (r, zn + 0.006)]
    pts += [(r * 0.98, L), (0.0, L)]
    return pts


def hang_goods(name):
    """A bamboo pole (0.6 m along X, origin at its centre) with dried goods hanging 0.4-0.6 m below."""
    b = Builder(name)
    rng = random.Random(606)
    r = 0.016
    b.add(bm_lathe(bamboo_profile(0.6, r, [0.07, 0.29, 0.51], rng), 14), "hc_bamboo", M=T(-0.3, 0, 0) @ R(90, "Y"),
          uv="keep")
    xs = [-0.245, -0.15, -0.05, 0.05, 0.15, 0.245]
    kinds = ["chilli", "herb", "parcel", "chilli", "herb", "parcel"]
    rng.shuffle(kinds)
    for x, kind in zip(xs, kinds):
        drop = rng.uniform(0.4, 0.6)
        # a twine loop round the pole and the hanging cord
        b.add(bm_torus(r + 0.002, 0.0018, 12, 4), "hc_string", M=T(x, 0, 0) @ R(90, "Y"), uv="keep")
        sway = rng.uniform(-0.01, 0.01)
        if kind == "chilli":
            top = -0.05
            cord = [(x, 0, -r), (x + sway * 0.2, 0, top)]
            b.add(bm_tube(cord, 0.0015, seg=4), "hc_string", uv="keep")
            n = 40
            for i in range(n):
                t = i / (n - 1)
                z = top - 0.02 - t * (drop - 0.1)
                a = i * 2.39996 + rng.uniform(-0.3, 0.3)
                L = rng.uniform(0.05, 0.075)
                out = Vector((math.cos(a), math.sin(a), 0.0))
                p0 = Vector((x + sway * t, 0, z))
                d = (out * rng.uniform(0.3, 0.5) + Vector((0, 0, -0.9))).normalized()
                curl = Vector((-out.y, out.x, 0.0)) * rng.uniform(-0.25, 0.25)
                pts = [p0 + out * 0.004]
                for k in range(1, 6):
                    u = k / 5
                    pts.append(pts[0] + (d * u + curl * u * u) * L + Vector((0, 0, -0.01 * u * u)))
                rad = [0.0065 * (1 - (k / 5) ** 1.6) + 0.0006 for k in range(6)]
                rad[0] = 0.005
                b.add(bm_tube(pts, rad, seg=6), "hc_chilli", uv="keep")
                b.add(bm_tube([pts[0] - d * 0.008, pts[0] + d * 0.002], 0.0052, seg=6), "hc_stem", uv="keep")
            b.add(bm_tube([(x, 0, top), (x + sway, 0, top - drop + 0.06)], 0.0015, seg=4), "hc_string", uv="keep")
        elif kind == "herb":
            top = -0.07
            b.add(bm_tube([(x, 0, -r), (x, 0, top)], 0.0017, seg=4), "hc_straw", uv="keep")
            for k in range(3):  # straw rope wraps round the tied stems
                b.add(bm_torus(0.011 - k * 0.0005, 0.0026, 10, 5), "hc_straw", M=T(x, 0, top - 0.006 - k * 0.005),
                      uv="keep")
            nst = 26
            for i in range(nst):
                a = rng.uniform(0, 2 * math.pi)
                rr = rng.uniform(0.0, 0.009)
                L = drop - rng.uniform(0.04, 0.12)
                spread = rng.uniform(0.012, 0.04)
                p0 = Vector((x + rr * math.cos(a), rr * math.sin(a), top + 0.02))
                pts = [p0]
                for k in range(1, 5):
                    u = k / 4
                    pts.append(p0 + Vector((math.cos(a) * spread * u * u + sway * u, math.sin(a) * spread * u * u,
                                            -L * u)))
                b.add(bm_tube(pts, [0.0014, 0.0013, 0.0011, 0.0009, 0.0006], seg=3, caps=False), "hc_stem",
                      uv="keep")
                for k in range(rng.randint(4, 6)):  # dried leaves, crumpled, thickest toward the bunch's foot
                    u = rng.uniform(0.4, 1.0) ** 0.7
                    p = Vector(pts[0]).lerp(Vector(pts[-1]), u)
                    p.x += math.cos(a) * spread * u * u
                    p.y += math.sin(a) * spread * u * u
                    lw, ll = rng.uniform(0.01, 0.017), rng.uniform(0.025, 0.04)
                    prof = [(0.0, 0.0), (lw * 0.6, ll * 0.15), (lw, ll * 0.5), (lw * 0.5, ll * 0.85), (0.0, ll)]
                    Ml = (T(p.x, p.y, p.z) @ R(rng.uniform(0, 360), "Z") @ R(rng.uniform(120, 230), "X") @
                          S(1.0, 0.3, 1.0))
                    b.add(bm_lathe(prof, 4, rfun=wobble(rng, 0.15)), "hc_herb", M=Ml, uv="keep")
        else:  # paper-wrapped parcels on a string, one or two
            top = -r
            n = rng.randint(1, 2)
            z = top
            for k in range(n):
                hh = rng.uniform(0.11, 0.16)
                ln = (drop - 0.04 - n * 0.14) / n
                b.add(bm_tube([(x, 0, z), (x + sway * 0.5, 0, z - ln)], 0.0015, seg=4), "hc_string", uv="keep")
                z -= ln
                w, d = rng.uniform(0.07, 0.1), rng.uniform(0.05, 0.07)
                packet(b, T(x + sway * 0.5, 0, z - hh) @ R(rng.uniform(-20, 20), "Z"), w, d, hh, rng)
                z -= hh + 0.004
    return b.finish()


def hang_sign(name, W, H, t, seed, metal_mark=True):
    """A vertical signboard on an iron wall bracket: origin at the mount on the wall (y = 0), the arm 0.7 m out along
    -Y at z = 0, the board's faces looking along +-X. Board: lacquer frame, timber panel, a raised invented mark in
    hc_signink on both faces."""
    b = Builder(name)
    rng = random.Random(seed)
    arm = 0.022
    # wall plate with four bolt heads
    b.add(bm_box(-0.035, -0.008, -0.33, 0.035, 0.0, 0.05, bevel=0.002), "hc_iron")
    for z in (-0.3, 0.02):
        for x in (-0.02, 0.02):
            b.add(bm_lathe([(0.0, 0.0), (0.006, 0.0), (0.006, 0.003), (0.004, 0.005), (0.0, 0.0055)], 8), "hc_iron",
                  M=T(x, -0.008, z) @ R(90, "X"), uv="keep")
    # the arm, its brace and a scroll at the tip
    b.add(bm_box(-arm / 2, -0.70, -arm / 2, arm / 2, -0.004, arm / 2, bevel=0.003), "hc_iron", grain="y")
    brace = [(0.0, -0.006, -0.30), (0.0, -0.2, -0.18), (0.0, -0.36, -0.06), (0.0, -0.45, -arm / 2)]
    b.add(bm_tube(brace, 0.0085, seg=8, squash=1.0), "hc_iron", uv="keep")
    scroll = []
    for i in range(16):
        a = i / 15 * 1.6 * math.pi
        rr = 0.045 * (1 - i / 22)
        scroll.append((0.0, -0.70 - 0.0 + rr * math.sin(a) * 0.9 - 0.0, -rr + rr * math.cos(a) - 0.0 + 0.0))
    scroll = [(0.0, -0.70 + p[1] + 0.70, p[2]) for p in scroll]
    b.add(bm_tube(scroll, 0.007, seg=6), "hc_iron", uv="keep")
    # the board's hang points
    yc = -0.44
    ztop = -0.112  # the hood's top at -0.094 carries the eye bolts that hang from the last chain links
    zb = ztop - H
    for yh in (yc - W / 2 + 0.06, yc + W / 2 - 0.06):
        # eye on the arm, two links, eye bolt on the board
        b.add(bm_torus(0.009, 0.0025, 10, 5), "hc_iron", M=T(0, yh, -arm / 2 - 0.007) @ R(90, "Y"), uv="keep")
        b.add(bm_torus(0.011, 0.0022, 10, 5), "hc_iron", M=T(0, yh, -arm / 2 - 0.03) @ R(0, "Y") @ R(90, "X"),
              uv="keep")
        b.add(bm_torus(0.011, 0.0022, 10, 5), "hc_iron", M=T(0, yh, -arm / 2 - 0.055) @ R(90, "Y"), uv="keep")
        b.add(bm_torus(0.009, 0.0025, 10, 5), "hc_iron", M=T(0, yh, -arm / 2 - 0.072) @ R(90, "X"), uv="keep")
        b.add(bm_tube([(0, yh, ztop + 0.012), (0, yh, -arm / 2 - 0.081)], 0.003, seg=6), "hc_iron", uv="keep")
        b.add(bm_lathe([(0.0, 0.0), (0.008, 0.0), (0.008, 0.002), (0.005, 0.004), (0.0, 0.0045)], 8), "hc_iron",
              M=T(0, yh, ztop + 0.018), uv="keep")  # the bolt's washer on the hood
    # the board: a lacquer frame round a timber panel, a small lacquer hood on top
    fw = 0.035
    y0, y1 = yc - W / 2, yc + W / 2
    b.add(bm_box(-t / 2, y0, zb, t / 2, y0 + fw, ztop, bevel=0.004, seg=2), "hc_lacquer", grain="z")
    b.add(bm_box(-t / 2, y1 - fw, zb, t / 2, y1, ztop, bevel=0.004, seg=2), "hc_lacquer", grain="z")
    b.add(bm_box(-t / 2, y0 + fw - 0.002, ztop - fw, t / 2, y1 - fw + 0.002, ztop, bevel=0.004, seg=2), "hc_lacquer",
          grain="y")
    b.add(bm_box(-t / 2, y0 + fw - 0.002, zb, t / 2, y1 - fw + 0.002, zb + fw, bevel=0.004, seg=2), "hc_lacquer",
          grain="y")
    pt = t - 0.012
    b.add(bm_box(-pt / 2, y0 + fw - 0.003, zb + fw - 0.003, pt / 2, y1 - fw + 0.003, ztop - fw + 0.003, bevel=0.002),
          "hc_timber", grain="z", uvoff=(rng.random(), rng.random()))
    b.add(bm_box(-t / 2 - 0.012, y0 - 0.012, ztop, t / 2 + 0.012, y1 + 0.012, ztop + 0.018, bevel=0.005, seg=2),
          "hc_lacquer", grain="y")
    # the mark, carved and gilded, raised on both faces
    mh, mw = (H - 2 * fw) * 0.8, (W - 2 * fw) * 0.78
    strokes = brush_mark(seed, mh, mw, n=rng.randint(4, 5), wmin=0.012 * mh / 0.18 * 0.5,
                         wmax=0.03 * mh / 0.18 * 0.55, N=18)
    zc = (zb + ztop) / 2
    for sx in (-1, 1):
        def surf(sv, tv, sx=sx):
            return Vector((sx * pt / 2, yc + sx * sv, zc + tv)), Vector((sx, 0.0, 0.0))
        add_mark(b, strokes, surf, "hc_signink", proud=(0.0012, 0.0018), embed=0.0005)
    return b.finish()


def cloth_panel(fn, w, h, nu, nv, thick, xoff=0.0):
    """A cloth sheet x in [-w/2, w/2] + xoff, z from 0 down to -h, y = fn(x, z) (the street side is -Y). Returns
    (front rows, back rows, uv rows) for bm_shell, the back offset +Y by thick."""
    top, bot, uvs = [], [], []
    for j in range(nv + 1):
        z = -h * j / nv
        rt, rb, ru = [], [], []
        for i in range(nu + 1):
            x = -w / 2 + w * i / nu + xoff
            y = fn(x, z)
            rt.append(Vector((x, y, z)))
            rb.append(Vector((x, y + thick, z)))
            ru.append((x, z))
        top.append(rt)
        bot.append(rb)
        uvs.append(ru)
    return top, bot, uvs


def banner(name):
    """A vertical cloth banner 0.45 x 1.6 m from a 0.55 m bamboo rod (origin at the rod's centre)."""
    b = Builder(name)
    rng = random.Random(4516)
    w, h, rr = 0.45, 1.6, 0.012
    b.add(bm_lathe(bamboo_profile(0.55, rr, [0.06, 0.3, 0.5], rng), 12), "hc_bamboo", M=T(-0.275, 0, 0) @ R(90, "Y"),
          uv="keep")
    # a hanging cord from the rod's ends to a peak
    b.add(bm_tube([(-0.25, 0, rr), (-0.12, 0, 0.09), (0.0, 0, 0.13), (0.12, 0, 0.09), (0.25, 0, rr)], 0.0022, seg=5),
          "hc_string", uv="keep")
    for x in (-0.25, 0.25):
        b.add(bm_torus(rr + 0.0025, 0.0025, 10, 4), "hc_string", M=T(x, 0, 0) @ R(90, "Y"), uv="keep")
    sleeve_r = rr + 0.004
    top_z = -sleeve_r
    ph = rng.uniform(0, 6.3)

    def fy(x, z):
        d = -z  # metres down from the sleeve
        ramp = min(1.0, d / 0.35)
        wave = 0.011 * math.sin(2 * math.pi * d / 0.52 + ph) * ramp
        billow = -0.006 * math.cos(math.pi * x / w) * ramp
        curl = 0.004 * ((2 * x / w) ** 4) * ramp
        sway = -0.012 * (d / h) ** 2
        return wave + billow + curl + sway

    def F(x, z):
        return fy(x, z - top_z)

    top, bot, uvs = cloth_panel(lambda x, z: fy(x, z), w - 0.01, h - sleeve_r - 0.006, 12, 44, 0.003)
    for rows in (top, bot):
        for row in rows:
            for p in row:
                p.z += top_z
    b.add(bm_shell(top, bot, uvs), "hc_bannercloth", uv="keep")
    # the sleeve: the cloth folded over the rod
    sl = []
    for k in range(13):
        a = -math.pi / 2 + 2 * math.pi * k / 12
        sl.append(a)
    rows_o, rows_i, ruv = [], [], []
    for i in range(13):
        x = -w / 2 + 0.005 + (w - 0.01) * i / 12
        ro, ri, ru = [], [], []
        for a in sl:
            ro.append(Vector((x, -(sleeve_r + 0.003) * math.cos(a), -(sleeve_r + 0.003) * math.sin(a) * -1.0)))
            ri.append(Vector((x, -sleeve_r * math.cos(a), sleeve_r * math.sin(a))))
            ru.append((x, a * sleeve_r))
        rows_o.append(ro)
        rows_i.append(ri)
        ruv.append(ru)
    b.add(bm_shell(rows_o, rows_i, ruv), "hc_bannercloth", uv="keep")
    # rolled hems: both sides and the foot
    nv = 44
    for sx in (-1, 1):
        x = sx * (w / 2 - 0.004)
        pts = [(x, fy(x, -(h - sleeve_r) * j / nv) + 0.0015, top_z - (h - sleeve_r - 0.006) * j / nv)
               for j in range(nv + 1)]
        b.add(bm_tube(pts, 0.0038, seg=6), "hc_bannercloth", uv="keep")
    zf = top_z - (h - sleeve_r - 0.006)
    pts = [(-w / 2 + 0.002 + (w - 0.004) * i / 16, fy(-w / 2 + (w * i / 16), zf - top_z) + 0.0015, zf)
           for i in range(17)]
    b.add(bm_tube(pts, 0.0048, seg=6), "hc_bannercloth", uv="keep")
    # the pale mark, raised 1 mm, on both faces
    strokes = brush_mark(4517, 0.95, 0.3, n=5, wmin=0.02, wmax=0.06, N=20)
    zc = top_z - 0.72
    for side in (-1, 1):
        def surf(sv, tv, side=side):
            x = -side * sv
            z = zc + tv
            y = fy(x, z - top_z) + (0.003 if side > 0 else 0.0)
            dz = 0.002
            dydz = (fy(x, z - top_z + dz) - fy(x, z - top_z - dz)) / (2 * dz)
            return Vector((x, y, z)), Vector((0.0, float(side), -side * dydz)).normalized()
        add_mark(b, strokes, surf, "hc_bannerink", proud=(0.0009, 0.0011), embed=0.0004)
    return b.finish()


def crate(name):
    """0.6 x 0.4 x 0.35 m slat crate: origin at the bottom centre."""
    b = Builder(name)
    rng = random.Random(6043)
    L, Wd, H = 0.6, 0.4, 0.35
    hx, hy = L / 2, Wd / 2
    pt = 0.012
    post = 0.035
    rb = 0.022
    # runners and bottom boards
    for y in (-hy + 0.05, hy - 0.05):
        b.add(bm_box(-hx + 0.01, y - 0.022, 0.0, hx - 0.01, y + 0.022, rb, bevel=0.003, seg=2, jitter=0.0006, rng=rng),
              "hc_timber", grain="x", uvoff=(rng.random(), rng.random()))
    n = 4
    for k in range(n):
        y0 = -hy + pt + k * (Wd - 2 * pt) / n
        b.add(bm_box(-hx + pt, y0 + 0.004, rb, hx - pt, y0 + (Wd - 2 * pt) / n - 0.004, rb + 0.012, bevel=0.002,
                     jitter=0.0005, rng=rng), "hc_timber", grain="x", uvoff=(rng.random(), rng.random()))
    # corner posts
    for sx in (-1, 1):
        for sy in (-1, 1):
            x0 = sx * (hx - pt) - (post if sx > 0 else 0)
            y0 = sy * (hy - pt) - (post if sy > 0 else 0)
            b.add(bm_box(x0, y0, rb, x0 + post, y0 + post, H - 0.004, bevel=0.003, seg=2, jitter=0.0005, rng=rng),
                  "hc_timber", grain="z", uvoff=(rng.random(), rng.random()))
    # side planks with gaps, nailed to the posts
    ph = 0.098
    gap = 0.018
    zs = [rb + k * (ph + gap) for k in range(3)]
    nail = [(0.0, 0.0), (0.0042, 0.0), (0.0042, 0.0006), (0.0032, 0.0014), (0.0, 0.0018)]
    for z0 in zs:
        z1 = min(z0 + ph, H)
        for sy in (-1, 1):  # long sides
            y0, y1 = (hy - pt, hy) if sy > 0 else (-hy, -hy + pt)
            b.add(bm_box(-hx, y0, z0, hx, y1, z1, bevel=0.003, seg=2, jitter=0.0007, rng=rng), "hc_timber", grain="x",
                  uvoff=(rng.random(), rng.random()))
            for sx in (-1, 1):
                for zn in (z0 + 0.028, z1 - 0.028):
                    x = sx * (hx - pt - post / 2)
                    b.add(bm_lathe(nail, 6), "hc_iron", M=T(x + rng.uniform(-0.003, 0.003), sy * hy, zn) @
                          R(-90 * sy, "X"), uv="keep")
        for sx in (-1, 1):  # short sides, between the long planks
            x0, x1 = (hx - pt, hx) if sx > 0 else (-hx, -hx + pt)
            b.add(bm_box(x0, -hy + pt + 0.0005, z0, x1, hy - pt - 0.0005, z1, bevel=0.003, seg=2, jitter=0.0007,
                         rng=rng), "hc_timber", grain="y", uvoff=(rng.random(), rng.random()))
            for sy in (-1, 1):
                for zn in (z0 + 0.03, z1 - 0.03):
                    y = sy * (hy - pt - post / 2)
                    b.add(bm_lathe(nail, 6), "hc_iron", M=T(sx * hx, y + rng.uniform(-0.003, 0.003), zn) @
                          R(90 * sx, "Y"), uv="keep")
    return b.finish()


def bucket(name):
    """A 0.30 m wooden stave bucket (0.30 high) with two iron hoops and a rope handle: origin at the bottom centre."""
    b = Builder(name)
    rng = random.Random(3030)
    n = 16
    H = 0.30
    th = 0.013

    def ro(z):
        return 0.140 + 0.010 * z / H

    gap = 0.0008
    for k in range(n):
        a0 = 2 * math.pi * k / n
        a1 = 2 * math.pi * (k + 1) / n
        da = gap / 0.145
        cols = 4
        top, bot, uvs = [], [], []
        hk = H + rng.uniform(-0.002, 0.001)
        for zz in (0.0, hk * 0.5, hk):
            rt, rb, ru = [], [], []
            for c in range(cols + 1):
                a = a0 + da / 2 + (a1 - a0 - da) * c / cols
                r = ro(zz) + rng.uniform(-0.0003, 0.0003)
                rt.append(Vector((r * math.cos(a), r * math.sin(a), zz)))
                rb.append(Vector(((r - th) * math.cos(a), (r - th) * math.sin(a), zz)))
                ru.append((a * 0.145 + k * 0.37, zz))
            top.append(rt)
            bot.append(rb)
            uvs.append(ru)
        b.add(bm_shell(top, bot, uvs, smooth=True), "hc_timber", uv="keep")
    # the bottom, set up 18 mm (a chime under it)
    rb = ro(0.02) - th + 0.001
    b.add(bm_lathe([(0.0, 0.018), (rb, 0.018), (rb, 0.043), (0.0, 0.043)], 32, smooth=False), "hc_timber", uv="keep")
    # two iron hoops
    for z0 in (0.045, 0.235):
        z1 = z0 + 0.026
        p = [(ro(z0) - 0.001, z0), (ro(z0) + 0.0022, z0 + 0.001), (ro(z1) + 0.0022, z1 - 0.001), (ro(z1) - 0.001, z1)]
        b.add(bm_lathe(p, 48, smooth=True), "hc_iron", uv="keep")
    # rope handle: two twisted strands arching over, knotted outside the staves
    path = []
    for i in range(33):
        t = i / 32
        a = math.pi * t
        path.append(Vector((ro(0.27) * math.cos(a) * 1.0 + 0.0, 0.012 * math.sin(a), 0.27 + 0.13 * math.sin(a))))
    path[0] = Vector((ro(0.27) - 0.02, 0.0, 0.27))
    path[-1] = Vector((-ro(0.27) + 0.02, 0.0, 0.27))
    P, F = frames(path)
    for sgn in (0, 1):
        strand = []
        for i, (p, (t_, nn, bb)) in enumerate(zip(P, F)):
            ang = i * 0.9 + sgn * math.pi
            strand.append(p + 0.0034 * (math.cos(ang) * nn + math.sin(ang) * bb))
        b.add(bm_tube(strand, 0.0042, seg=6), "hc_straw", uv="keep")
    for sx in (-1, 1):
        b.add(bm_lathe([(0.0, -0.011), (0.008, -0.009), (0.011, 0.0), (0.008, 0.009), (0.0, 0.011)], 8,
                       rfun=wobble(rng, 0.15)), "hc_straw", M=T(sx * (ro(0.27) + 0.006), 0, 0.27) @ R(90, "Y"),
              uv="keep")
    return b.finish()


def gate(name):
    """An open timber lane gateway: posts at x = +-3.6 on stone footings, a tie beam at 3.6 m, a top beam on the posts
    at 4.4 m under a small tiled ridge roof 0.9 m deep; lantern hooks under the tie beam; a plaque at its centre."""
    b = Builder(name)
    rng = random.Random(7400)
    ps = 0.28
    hp = ps / 2
    foot_h = 0.3
    for sx in (-1, 1):
        x = sx * 3.6
        st = bm_box(x - 0.26, -0.26, -0.05, x + 0.26, 0.26, foot_h, bevel=0.03, seg=2, jitter=0.004, rng=rng)
        b.add(st, "hc_stone", grain="z", uvoff=(rng.random(), rng.random()))
        # the post: chamfered, its foot worn (the post map darkens and roughens its lowest half metre)
        pbm = bm_box(x - hp, -hp, foot_h, x + hp, hp, 4.4, bevel=0.022, seg=1)
        for v in pbm.verts:  # the foot's edges rounded off a little more, uneven
            if v.co.z < foot_h + 0.25:
                k = (foot_h + 0.25 - v.co.z) / 0.25
                d = Vector((v.co.x - x, v.co.y, 0.0))
                if abs(d.x) > hp - 0.03 and abs(d.y) > hp - 0.03:
                    v.co.x -= math.copysign(0.004 * k, d.x)
                    v.co.y -= math.copysign(0.004 * k, d.y)
        b.add(pbm, "hc_timber_post", grain="z", uvoff=(rng.random() * 0.5, 0.0),
              uvscale=0.8 / 4.2)  # v spans the post map's 4.2 m
        # iron foot band where the post meets the stone
        b.add(bm_box(x - hp - 0.006, -hp - 0.006, foot_h, x + hp + 0.006, hp + 0.006, foot_h + 0.05, bevel=0.004),
              "hc_iron")
    # tie beam (through the posts), its ends standing out with wedges
    tb0, tb1 = 3.5, 3.7
    tbd = 0.13
    b.add(bm_box(-3.95, -tbd / 2, tb0, 3.95, tbd / 2, tb1, bevel=0.012, seg=1), "hc_timber", grain="x")
    for sx in (-1, 1):
        x = sx * (3.6 + hp + 0.04)
        b.add(bm_prism([(-0.02, tb1 + 0.0), (0.02, tb1 + 0.0), (0.012, tb0 + 0.02), (-0.012, tb0 + 0.02)], -tbd / 2 - 0.02,
                       tbd / 2 + 0.02, bevel=0.003), "hc_timber", M=T(x, 0, 0), grain="z")
    # top beam on the posts: deeper, longer, its underside curved up toward the ends
    tz0 = 4.4
    prof = []
    for i in range(21):
        xx = -4.1 + 8.2 * i / 20
        lift = 0.06 * (abs(xx) / 4.1) ** 3
        prof.append((xx, tz0 + lift))
    poly = prof + [(4.1, tz0 + 0.24), (-4.1, tz0 + 0.24)]
    b.add(bm_prism(poly, -0.14, 0.14, bevel=0.012), "hc_timber", grain="x")
    # the small roof: rafters, a tiled slope each side and a ridge
    rz = tz0 + 0.24
    pitch = math.radians(24)
    run = 0.45
    for k in range(-8, 9):
        xx = k * 0.5
        for sy in (-1, 1):
            p0 = Vector((xx, 0, rz + 0.02 + run * math.tan(pitch)))
            p1 = Vector((xx, sy * (run + 0.03), rz + 0.02 - 0.03 * math.tan(pitch)))
            b.add(bm_tube([p0, p1], 0.022, seg=4, smooth=False), "hc_timber", uv="keep")
    tile_slope(b, 8.3, run + 0.06, pitch, rz + 0.095)  # tiles clear the rafters' tops by about 1 cm
    # ridge: a stacked ridge and a round cap with end discs
    ztop = rz + 0.095 + run * math.tan(pitch)
    b.add(bm_box(-4.12, -0.07, ztop - 0.01, 4.12, 0.07, ztop + 0.07, bevel=0.01), "hc_rooftile", grain="x")
    b.add(bm_lathe([(0.0, -4.18), (0.05, -4.18), (0.055, -4.16), (0.055, 4.16), (0.05, 4.18), (0.0, 4.18)], 12),
          "hc_rooftile", M=T(0, 0, ztop + 0.09) @ R(90, "Y"), uv="keep")
    for sx in (-1, 1):  # barge boards: 0.1 m planks following the slope, the gable under them left open
        for sy in (-1, 1):
            L = run + 0.1
            za = ztop + 0.03
            zbb = za - L * math.tan(pitch)
            b.add(bm_prism([(0.0, za - 0.1), (L, zbb - 0.1), (L, zbb), (0.0, za)] if sy > 0 else
                           [(0.0, za - 0.1), (0.0, za), (-L, zbb), (-L, zbb - 0.1)],
                           -0.0, 0.03, bevel=0.004), "hc_timber", M=T(sx * 4.16, 0, 0) @ R(-90, "Z"), grain="y")
    # lantern hooks under the tie beam: a plate, an eye and a J hook
    for x in (-2.4, -0.8, 0.8, 2.4):
        b.add(bm_box(x - 0.03, -0.04, tb0 - 0.006, x + 0.03, 0.04, tb0, bevel=0.002), "hc_iron")
        hook = [(x, 0, tb0), (x, 0, tb0 - 0.05), (x, 0, tb0 - 0.09)]
        for i in range(1, 9):
            a = math.pi * i / 8 * 1.15
            hook.append((x + 0.022 - 0.022 * math.cos(a), 0, tb0 - 0.09 - 0.022 * math.sin(a)))
        b.add(bm_tube(hook, 0.0055, seg=6), "hc_iron", uv="keep")
    # the plaque between the tie beam and the top beam: lacquer frame, timber panel, gilded mark both faces
    pw, phh, pt = 0.44, 0.58, 0.06
    zb, zt = tb1 + 0.06, tb1 + 0.06 + phh
    fw = 0.045
    for (x0, z0, x1, z1) in ((-pw / 2, zb, -pw / 2 + fw, zt), (pw / 2 - fw, zb, pw / 2, zt),
                             (-pw / 2 + fw - 0.002, zt - fw, pw / 2 - fw + 0.002, zt),
                             (-pw / 2 + fw - 0.002, zb, pw / 2 - fw + 0.002, zb + fw)):
        b.add(bm_box(x0, -pt / 2, z0, x1, pt / 2, z1, bevel=0.005, seg=2), "hc_lacquer")
    b.add(bm_box(-pw / 2 + fw - 0.003, -pt / 2 + 0.008, zb + fw - 0.003, pw / 2 - fw + 0.003, pt / 2 - 0.008,
                 zt - fw + 0.003, bevel=0.002), "hc_timber", grain="z")
    for x in (-0.12, 0.12):  # posts holding it off the beams
        b.add(bm_box(x - 0.02, -0.02, tb1, x + 0.02, 0.02, zb, bevel=0.003), "hc_lacquer")
        b.add(bm_box(x - 0.02, -0.02, zt, x + 0.02, 0.02, tz0 + 0.004, bevel=0.003), "hc_lacquer")
    strokes = brush_mark(7401, (phh - 2 * fw) * 0.78, (pw - 2 * fw) * 0.75, n=4, wmin=0.015, wmax=0.05, N=16)
    zc = (zb + zt) / 2
    for sy in (-1, 1):
        def surf(sv, tv, sy=sy):
            return Vector((-sy * sv, sy * (pt / 2 - 0.008), zc + tv)), Vector((0.0, float(sy), 0.0))
        add_mark(b, strokes, surf, "hc_signink", proud=(0.0015, 0.002), embed=0.0005)
    return b.finish()


def bez3(p0, p1, p2, p3, n):
    P = [Vector(p) for p in (p0, p1, p2, p3)]
    out = []
    for i in range(n):
        t = i / (n - 1)
        out.append(P[0] * (1 - t) ** 3 + P[1] * 3 * (1 - t) ** 2 * t + P[2] * 3 * (1 - t) * t * t + P[3] * t ** 3)
    return out


def rrect(x0, x1, y0, y1, rad, z, k=4):
    """A rounded rectangle's outline at height z, anticlockwise, the first point not repeated."""
    pts = []
    for cx, cy, a0 in ((x1 - rad, y0 + rad, -90), (x1 - rad, y1 - rad, 0), (x0 + rad, y1 - rad, 90),
                       (x0 + rad, y0 + rad, 180)):
        for i in range(k + 1):
            a = math.radians(a0 + 90 * i / k)
            pts.append(Vector((cx + rad * math.cos(a), cy + rad * math.sin(a), z)))
    return pts


def bicycle(name):
    """A city step-through bicycle, 1.70 m long along Y (the front wheel at -Y), 0.58 m across the bars, 1.0 m to the
    basket's rim: origin at the ground under the middle, both tyres and the centre stand's foot on z = 0. Green-black
    enamel chipped to rust, a rusting wire basket on the front, a rear rack, mudguards, a chain guard on the right, a
    sprung saddle, swept-back bars with a bell, a lamp on the fork. Replaces the kit's BICYCLE (same bounds)."""
    b = Builder(name)
    rng = random.Random(9090)
    R0, AZ = 0.325, 0.325
    FA, RA = Vector((0, -0.525, AZ)), Vector((0, 0.525, AZ))
    BB = Vector((0, 0.03, 0.27))
    paint, steel, rust, rub = "hc_bikepaint", "hc_steel", "hc_rust", "hc_rubber"

    def tube(pts, r, mat, seg=10, caps=True):
        b.add(bm_tube(pts, r, seg=seg, caps=caps), mat, uv="keep", uvoff=(rng.random(), rng.random()))

    # wheels: tyre, rim, hub, 32 tangent-laced spokes
    hub = [(0.0, -0.05), (0.008, -0.05), (0.008, -0.042), (0.031, -0.037), (0.031, -0.031), (0.019, -0.026),
           (0.019, 0.026), (0.031, 0.031), (0.031, 0.037), (0.008, 0.042), (0.008, 0.05), (0.0, 0.05)]
    for c in (FA, RA):
        M = T(*c) @ R(90, "Y")
        b.add(bm_torus(R0 - 0.0185, 0.0185, seg=56, tube=8), rub, M=M, uv="keep")
        b.add(bm_torus(0.291, 0.0085, seg=56, tube=6), steel, M=M @ S(1, 1, 1.4), uv="keep")
        b.add(bm_lathe(hub, 12), steel, M=M, uv="keep")
        for k in range(32):
            side = 1 if k % 2 else -1
            a = 2 * math.pi * k / 32
            ar = a + side * 0.33
            p0 = c + Vector((side * 0.034, 0.028 * math.cos(a), 0.028 * math.sin(a)))
            p1 = c + Vector((0.0, 0.284 * math.cos(ar), 0.284 * math.sin(ar)))
            tube([p0, p1], 0.0011, steel, seg=3, caps=False)
        for sx in (-1, 1):  # axle nuts
            b.add(bm_lathe([(0.0, 0.0), (0.009, 0.0), (0.009, 0.008), (0.0, 0.008)], 6, smooth=False), steel,
                  M=T(c.x + sx * 0.052, c.y, c.z) @ R(90 * sx, "Y"), uv="keep")
    # mudguards: a crowned steel strip round each wheel, a stay each side
    for c, a0, a1 in ((FA, 25, 200), (RA, -25, 155)):
        rows_t, rows_b, uvs = [], [], []
        w = 0.066
        for j in range(30):
            a = math.radians(a0 + (a1 - a0) * j / 29)
            d = Vector((0, math.cos(a), math.sin(a)))
            rt, rb, ru = [], [], []
            for i in range(7):
                x = -w / 2 + w * i / 6
                q = (2 * x / w) ** 2
                rad = R0 + 0.022 + 0.007 * (1 - q) - 0.006 * q * q
                rt.append(c + d * rad + Vector((x, 0, 0)))
                rb.append(c + d * (rad - 0.0018) + Vector((x, 0, 0)))
                ru.append((x, (a - math.radians(a0)) * 0.35))
            rows_t.append(rt)
            rows_b.append(rb)
            uvs.append(ru)
        b.add(bm_shell(rows_t, rows_b, uvs), paint, uv="keep")
        am = math.radians(a0 + (a1 - a0) * (0.22 if c is FA else 0.12))
        for sx in (-1, 1):
            tube([c + Vector((sx * 0.036, 0, 0)), c + Vector((sx * 0.034, math.cos(am) * (R0 + 0.026),
                                                                  math.sin(am) * (R0 + 0.026)))], 0.0028, steel, seg=4)
    # frame: head tube, a low curved main tube and a second tube over it, seat tube, stays, bottom bracket
    H0, H1 = Vector((0, -0.50, 0.64)), Vector((0, -0.54, 0.80))
    tube([H0 - (H1 - H0) * 0.08, H1], 0.021, paint, seg=14)
    tube(bez3((0, -0.505, 0.68), (0, -0.42, 0.40), (0, -0.17, 0.22), BB + Vector((0, -0.01, 0.0)), 18), 0.022, paint,
         seg=12)
    ST = Vector((0, 0.155, 0.80))
    tube(bez3((0, -0.52, 0.755), (0, -0.36, 0.50), (0, -0.06, 0.43), (0, 0.112, 0.62), 16), 0.015, paint, seg=10)
    tube([BB, ST], 0.019, paint, seg=12)
    b.add(bm_lathe([(0.0, -0.045), (0.024, -0.045), (0.024, 0.045), (0.0, 0.045)], 14, smooth=False), paint,
          M=T(*BB) @ R(90, "Y"), uv="keep")
    for sx in (-1, 1):
        tube([BB + Vector((sx * 0.03, 0.02, 0)), RA + Vector((sx * 0.062, -0.02, 0))], 0.0105, paint, seg=8)
        tube([Vector((sx * 0.022, 0.15, 0.755)), RA + Vector((sx * 0.062, -0.01, 0.01))], 0.009, paint, seg=8)
        b.add(bm_box(sx * 0.058, 0.50, AZ - 0.02, sx * 0.068, 0.545, AZ + 0.02, bevel=0.003), paint)  # drop-out
    # fork: crown, two raked blades to the front axle
    b.add(bm_box(-0.06, -0.525, 0.605, 0.06, -0.48, 0.635, bevel=0.008, seg=2), paint)
    for sx in (-1, 1):
        tube(bez3((sx * 0.048, -0.50, 0.615), (sx * 0.052, -0.495, 0.50), (sx * 0.054, -0.50, 0.40),
                  FA + Vector((sx * 0.054, 0, 0.005)), 12), 0.0125, paint, seg=8)
    # stem and swept-back bars, grips, brake levers, bell
    tube([H1, Vector((0, -0.56, 0.905)), Vector((0, -0.592, 0.95))], 0.013, steel, seg=10)
    bar = bez3((0, -0.595, 0.952), (0.16, -0.60, 0.958), (0.25, -0.53, 0.985), (0.285, -0.40, 0.99), 12)
    bar = [Vector((-p.x, p.y, p.z)) for p in reversed(bar)] + bar[1:]
    tube(bar, 0.0115, steel, seg=10)
    for sx in (-1, 1):
        g = [p for p in bar if p.x * sx > 0.235]
        tube(g, 0.0165, rub, seg=10)
        tube([Vector((sx * 0.20, -0.575, 0.968)), Vector((sx * 0.235, -0.62, 0.955)),
              Vector((sx * 0.27, -0.57, 0.958))], 0.005, steel, seg=6)
    b.add(bm_lathe([(0.0, 0.0), (0.028, 0.0), (0.027, 0.012), (0.02, 0.026), (0.008, 0.033), (0.0, 0.034)], 16),
          steel, M=T(0.13, -0.585, 0.962), uv="keep")
    # saddle: a sprung shell on two coils, a clamp on the post
    tube([ST, Vector((0, 0.172, 0.875))], 0.0125, steel, seg=10)
    b.add(bm_box(-0.03, 0.155, 0.865, 0.03, 0.20, 0.885, bevel=0.004), steel)
    rows_t, rows_b, uvs = [], [], []
    for j in range(14):
        t = j / 13  # 0 the rear, 1 the nose
        y = 0.33 - 0.26 * t
        w = 0.235 - 0.17 * smooth01(min(1.0, t * 1.3))
        rt, rb, ru = [], [], []
        for i in range(11):
            x = -w / 2 + w * i / 10
            q = (2 * x / w) ** 2
            z = 0.935 + 0.014 * (1 - q) - 0.012 * t - 0.012 * q * q - (0.02 * (1 - j / 2) if j < 2 else 0.0)
            rt.append(Vector((x, y, z)))
            rb.append(Vector((x * 0.97, y, z - 0.032 + 0.018 * q)))
            ru.append((x, y))
        rows_t.append(rt)
        rows_b.append(rb)
        uvs.append(ru)
    b.add(bm_shell(rows_t, rows_b, uvs), "hc_saddle", uv="keep")
    for sx in (-1, 1):
        coil = [Vector((sx * 0.06 + 0.014 * math.cos(2 * math.pi * i / 12), 0.27 + 0.014 * math.sin(2 * math.pi * i / 12),
                        0.862 + 0.045 * i / 60)) for i in range(61)]
        tube(coil, 0.0026, steel, seg=4)
        tube([Vector((sx * 0.06, 0.27, 0.86)), Vector((sx * 0.03, 0.19, 0.876)), Vector((sx * 0.016, 0.07, 0.905))],
             0.004, steel, seg=6)
    # chain guard (right), chainring under it, cranks and pedals
    poly = []
    for i in range(13):
        a = math.radians(90 + 180 * i / 12)
        poly.append((BB.y + 0.125 * math.cos(a), BB.z + 0.125 * math.sin(a)))
    for i in range(9):
        a = math.radians(-90 + 180 * i / 8)
        poly.append((RA.y + 0.06 * math.cos(a), RA.z + 0.06 * math.sin(a)))
    b.add(bm_prism(poly, -0.097, -0.089, bevel=0.0025), paint, M=R(90, "Z"))
    b.add(bm_lathe([(0.0, 0.0), (0.102, 0.0), (0.102, 0.004), (0.0, 0.004)], 32, smooth=False), steel,
          M=T(0.074, BB.y, BB.z) @ R(90, "Y"), uv="keep")
    for sx, ang in ((1, -35), (-1, 145)):
        a = math.radians(ang)
        end = Vector((sx * 0.088, BB.y + 0.165 * math.cos(a), BB.z + 0.165 * math.sin(a)))
        tube([Vector((sx * 0.06, BB.y, BB.z)), Vector((sx * 0.088, BB.y, BB.z)), end], 0.0105, steel, seg=8)
        px = sx * 0.15
        b.add(bm_box(px - 0.045, end.y - 0.028, end.z - 0.012, px + 0.045, end.y + 0.028, end.z + 0.012, bevel=0.004,
                     seg=2), rub)
        tube([end, Vector((px, end.y, end.z))], 0.006, steel, seg=6)
    # rear rack and a reflector
    for x in (-0.07, 0.0, 0.07):
        tube([Vector((x, 0.30, 0.72)), Vector((x, 0.79, 0.72))], 0.0055, paint, seg=6)
    for y in (0.32, 0.47, 0.62, 0.78):
        tube([Vector((-0.072, y, 0.72)), Vector((0.072, y, 0.72))], 0.0055, paint, seg=6)
    for sx in (-1, 1):
        tube([Vector((sx * 0.07, 0.74, 0.72)), RA + Vector((sx * 0.066, 0.01, 0.0))], 0.0065, paint, seg=6)
        tube([Vector((sx * 0.07, 0.31, 0.72)), Vector((sx * 0.026, 0.17, 0.745))], 0.005, paint, seg=6)
    b.add(bm_box(-0.04, 0.792, 0.69, 0.04, 0.806, 0.715, bevel=0.003), "hc_reflector")
    # front basket: a rusting wire box on the bars, two struts to the fork's ends
    bx0, bx1, by0, by1, bz0, bz1 = -0.17, 0.17, -0.842, -0.60, 0.765, 0.995
    for z, r in ((bz0, 0.0035), (0.84, 0.0022), (0.92, 0.0022), (bz1, 0.0042)):
        ring = rrect(bx0, bx1, by0, by1, 0.03, z)
        tube(ring + [ring[0]], r, rust, seg=4, caps=False)
    ring = rrect(bx0, bx1, by0, by1, 0.03, bz0)
    per = [0.0]
    for i in range(1, len(ring) + 1):
        per.append(per[-1] + (ring[i % len(ring)] - ring[i - 1]).length)
    nw = int(per[-1] / 0.033)
    for k in range(nw):
        s = per[-1] * k / nw
        i = max(i for i in range(len(per) - 1) if per[i] <= s)
        p = ring[i].lerp(ring[(i + 1) % len(ring)], (s - per[i]) / max(1e-9, per[i + 1] - per[i]))
        tube([p, Vector((p.x, p.y, bz1))], 0.0019, rust, seg=3, caps=False)
    for x in [bx0 + 0.04 * k for k in range(1, 9)]:
        tube([Vector((x, by0 + 0.01, bz0)), Vector((x, by1 - 0.01, bz0))], 0.0019, rust, seg=3, caps=False)
    for y in [by0 + 0.04 * k for k in range(1, 7)]:
        tube([Vector((bx0 + 0.01, y, bz0)), Vector((bx1 - 0.01, y, bz0))], 0.0019, rust, seg=3, caps=False)
    for sx in (-1, 1):
        tube([Vector((sx * 0.12, -0.80, bz0)), FA + Vector((sx * 0.06, -0.005, 0.01))], 0.0045, rust, seg=6)
    tube([Vector((0, by1, 0.86)), Vector((0, -0.575, 0.86)), Vector((0, -0.565, 0.92))], 0.006, rust, seg=6)
    # lamp on the fork crown, facing forward
    b.add(bm_lathe([(0.0, 0.0), (0.024, 0.0), (0.03, 0.03), (0.033, 0.06), (0.0, 0.06)], 16), steel,
          M=T(0, -0.53, 0.60) @ R(90, "X"), uv="keep")
    b.add(bm_lathe([(0.0, 0.0), (0.03, 0.0), (0.0, 0.006)], 16), "hc_lens", M=T(0, -0.59, 0.60) @ R(90, "X"), uv="keep")
    # ring lock across the rear wheel, centre stand to the ground
    b.add(bm_box(-0.04, 0.33, 0.43, 0.04, 0.37, 0.50, bevel=0.008, seg=2), steel)
    for sx in (-1, 1):
        tube([RA + Vector((sx * 0.07, 0.0, 0.0)), Vector((sx * 0.10, 0.60, 0.09)), Vector((sx * 0.115, 0.62, 0.009))],
             0.0085, steel, seg=8)
    tube([Vector((-0.115, 0.62, 0.009)), Vector((0.115, 0.62, 0.009))], 0.009, steel, seg=8)
    # a cable from the right lever down the bars and the tubes to the rear brake
    tube(bez3((0.22, -0.60, 0.955), (0.10, -0.66, 0.90), (0.04, -0.58, 0.72), (0.0, -0.45, 0.58), 10) +
         bez3((0.0, -0.44, 0.55), (0.0, -0.30, 0.42), (0.01, 0.0, 0.44), (0.02, 0.10, 0.62), 10)[1:] +
         [Vector((0.03, 0.30, 0.55)), Vector((0.04, 0.46, 0.40))], 0.0025, rub, seg=4)
    return b.finish()


def tile_slope(b, L, run, pitch, z_eave):
    """Two tiled slopes (ridge along X at y = 0, eaves at y = +-run): round tile rolls over pans, a course step, round
    end tiles at the eaves."""
    per = 0.25
    nx = int(round(L / per))
    samples = 6
    xs = [-L / 2 + L * i / (nx * samples) for i in range(nx * samples + 1)]
    for sy in (-1, 1):
        rows_t, rows_b, uvs = [], [], []
        nc = 2
        steps = []
        for c in range(nc):
            s0, s1 = c / nc, (c + 1) / nc - 0.01
            steps += [(s0, 0.025), (s1, 0.0)]
        for (s, step) in steps:
            y = sy * run * (1 - s)
            z = z_eave + run * s * math.tan(pitch)
            rt, rb, ru = [], [], []
            for x in xs:
                wv = 0.5 + 0.5 * math.cos(2 * math.pi * (x / per))
                lift = 0.045 * wv ** 1.5 + step
                rt.append(Vector((x, y + sy * 0.0, z + lift)))
                rb.append(Vector((x, y, z + lift - 0.022)))
                ru.append((x, s * run))
            rows_t.append(rt)
            rows_b.append(rb)
            uvs.append(ru)
        b.add(bm_shell(rows_t, rows_b, uvs, smooth=True), "hc_rooftile", uv="keep")
        for i in range(nx + 1):
            x = -L / 2 + per * i
            if abs(x) > L / 2 - 0.05:
                continue
            c = Vector((x, sy * (run + 0.005), z_eave + 0.03))
            b.add(bm_tube([c - Vector((0, sy * 0.04, 0)), c + Vector((0, sy * 0.012, 0))], 0.036, seg=10),
                  "hc_rooftile", uv="keep")


# =====================================================================================================================
# Eatery counter pieces
# =====================================================================================================================

def _dents(dents, base):
    """A lathe rfun: base(a, z) less smooth gaussian dents [(angle, z, depth fraction, angle sigma, z sigma)]."""
    def f(a, z):
        k = base(a, z)
        for (a0, z0, d, sa, sz) in dents:
            da = (a - a0 + math.pi) % (2 * math.pi) - math.pi
            k -= d * math.exp(-(da / sa) ** 2 - ((z - z0) / sz) ** 2)
        return k
    return f


def _orient(n):
    """A rotation taking +Z to the direction n."""
    return Vector((0.0, 0.0, 1.0)).rotation_difference(Vector(n).normalized()).to_matrix().to_4x4()


def stock_pot(name):
    """A dented, sooted aluminium stock pot (0.45 m across, 0.335 m tall) on a low cast-iron gas ring (0.065 m), its
    rim 0.40 m over the counter; origin at the bottom centre (on the counter). Handles along X, the lid slid toward +X
    and propped on the ladle's handle, which leans out over -X; the gas valve and hose toward +Y (the cook's side)."""
    b = Builder(name)
    rng = random.Random(4545)
    RP, H, t, hs = POT_R, POT_H, POT_T, 0.065
    zb = hs
    rim = zb + H
    # --- the ring stand: base ring on three pads, a cast spider to the burner, four pot supports, ported burner cap
    ring = [(0.140, 0.006), (0.168, 0.006), (0.170, 0.009), (0.170, 0.016), (0.168, 0.018), (0.140, 0.018),
            (0.138, 0.016), (0.138, 0.008), (0.140, 0.006)]
    b.add(bm_lathe(ring, 32), "hc_rust", uv="keep")
    for ang in (30, 150, 270):
        b.add(bm_box(0.139, -0.012, 0.0, 0.169, 0.012, 0.0065, bevel=0.0015), "hc_rust", M=R(ang, "Z"))
    for ang in (90, 210, 330):
        b.add(bm_box(0.045, -0.006, 0.008, 0.142, 0.006, 0.016, bevel=0.0015), "hc_rust", M=R(ang, "Z"), grain="x")
    for k in range(4):
        Mk = R(45 + 90 * k, "Z")
        b.add(bm_box(0.150, -0.006, 0.016, 0.162, 0.006, hs, bevel=0.0015), "hc_rust", M=Mk)
        b.add(bm_box(0.088, -0.0055, hs - 0.011, 0.156, 0.0055, hs, bevel=0.0015), "hc_rust", M=Mk, grain="x")
    burner = [(0.0, 0.006), (0.050, 0.006), (0.055, 0.012), (0.055, 0.030), (0.062, 0.034), (0.062, 0.042),
              (0.058, 0.046), (0.030, 0.049), (0.0, 0.049)]

    def ports(a, z):  # 18 flame ports notched round the crown
        return 1.0 - 0.07 * (0.034 <= z <= 0.0425 and math.cos(18 * a) > 0.3)
    b.add(bm_lathe(burner, 36, rfun=ports, smooth=False), "hc_rust", uv="keep")
    # gas feed toward +Y: pipe, brass valve, black knob, a rubber hose clamped on and lying back along the counter
    b.add(bm_tube([(0.0, 0.05, 0.030), (0.0, 0.238, 0.030)], 0.009, seg=8), "hc_steel", uv="keep")
    b.add(bm_lathe([(0.0, 0.0), (0.014, 0.0), (0.016, 0.006), (0.016, 0.03), (0.012, 0.036), (0.0, 0.036)], 14),
          "hc_brass", M=T(0, 0.236, 0.030) @ R(-90, "X"), uv="keep")
    b.add(bm_lathe([(0.0, 0.0), (0.006, 0.0), (0.006, 0.01), (0.016, 0.012), (0.017, 0.02), (0.014, 0.024),
                    (0.0, 0.024)], 14), "hc_lacquer", M=T(0, 0.252, 0.044), uv="keep")
    hose = [(0.0, 0.268, 0.030), (0.0, 0.278, 0.026), (0.004, 0.286, 0.016), (0.02, 0.29, 0.0095),
            (0.08, 0.29, 0.009), (0.16, 0.29, 0.009)]
    b.add(bm_tube(hose, 0.0085, seg=8), "hc_rubber", uv="keep")
    b.add(bm_torus(0.0098, 0.0018, 12, 4), "hc_steel", M=T(0, 0.274, 0.030) @ R(-90, "X"), uv="keep")
    # --- the pot: one lathe up the outside over the rolled bead (sooted from the base), one down the inside (clean),
    # both carrying the same dents so the wall keeps its 3 mm thickness
    dents = [(0.6, 0.20, 0.025, 0.22, 0.05), (2.9, 0.10, 0.018, 0.30, 0.04), (4.4, 0.26, 0.014, 0.18, 0.035),
             (5.6, 0.07, 0.020, 0.25, 0.03)]
    f = _dents(dents, wobble(rng, 0.004))
    zw = [0.035 + (H - 0.055) * k / 9 for k in range(10)]
    outer = ([(0.0, 0.0), (RP - 0.06, 0.0), (RP - 0.03, 0.0008), (RP - 0.012, 0.004), (RP - 0.003, 0.014), (RP, 0.03)] +
             [(RP, z) for z in zw] +
             [(RP, H - 0.012), (RP + 0.004, H - 0.0092), (RP + POT_BEAD, H - 0.0045), (RP + 0.0035, H),
              (RP - 0.001, H - 0.002)])
    b.add(bm_lathe(outer, 36, rfun=f), "hc_pot_metal_soot", M=T(0, 0, zb), uv="keep")
    inner = ([(RP - 0.001, H - 0.002), (RP - t, H - 0.009)] + [(RP - t, z) for z in reversed(zw)] +
             [(RP - t, 0.03 + 0.002), (RP - 0.003 - 0.0028, 0.014 + 0.0015), (RP - 0.012 - 0.0015, 0.004 + t),
              (RP - 0.03, 0.0008 + t), (RP - 0.06, t), (0.0, t)])
    b.add(bm_lathe(inner, 36, rfun=f), "hc_pot_metal", M=T(0, 0, zb), uv="keep")
    zl = 0.8 * H  # the stock's surface, dented with the wall so it never pokes through
    b.add(bm_lathe([(RP - t - 0.001, zl), (0.0, zl)], 36, rfun=f), "hc_broth_water", M=T(0, 0, zb), uv="keep")
    # two riveted loop handles along X
    zh = zb + H - 0.055
    for sx in (-1, 1):
        yw = math.sqrt(RP * RP - 0.05 * 0.05)
        loop = [(yw - 0.005, -0.05), (0.232, -0.05), (0.250, -0.045), (0.261, -0.029), (0.264, 0.0),
                (0.261, 0.029), (0.250, 0.045), (0.232, 0.05), (yw - 0.005, 0.05)]
        pts = [(sx * x, y, zh + (0.006 if abs(y) < 0.04 else 0.0)) for x, y in loop]
        b.add(bm_tube(pts, 0.0055, seg=8), "hc_steel", uv="keep")
        for sy in (-1, 1):
            nrm = Vector((sx * yw, sy * 0.05, 0.0)).normalized()
            plate = [(0.0, 0.0), (0.011, 0.0), (0.011, 0.0015), (0.007, 0.0035), (0.0, 0.004)]
            b.add(bm_lathe(plate, 10), "hc_steel", M=T((nrm * (RP - 0.0015)).x, (nrm * (RP - 0.0015)).y, zh) @ _orient(nrm), uv="keep")
    # the lid, slid toward +X: its edge roll rests on the rim's crest where the two overlap and, at -X, on the ladle
    RL = POT_LID_R
    LX = 0.215
    lid = [(0.0, 0.004), (0.12, 0.0035), (0.19, 0.002), (RL - 0.006, 0.0), (RL - 0.004, -0.002), (RL, 0.001),
           (RL - 0.002, 0.005), (RL - 0.01, 0.007), (0.17, 0.014), (0.09, 0.024), (0.035, 0.028), (0.0, 0.0285)]
    zlid = rim + 0.0025  # the roll's underside (local -0.002) 0.5 mm over the crest
    Ml = T(LX, 0.0, zlid)
    b.add(bm_lathe(lid, 40, rfun=wobble(rng, 0.003)), "hc_pot_metal", M=Ml, uv="keep",
          uvscale=2 * _POT_IN_TILE / (2 * math.pi * RL))
    b.add(bm_lathe([(0.0, 0.027), (0.018, 0.027), (0.02, 0.032), (0.014, 0.04), (0.022, 0.05), (0.02, 0.056),
                    (0.0, 0.0565)], 16), "hc_lacquer", M=Ml, uv="keep")
    # the ladle: a flat steel handle up out of the stock, under the lid's -X edge, leaning out over -X, hooked at the end
    rh = 0.0065
    d = Vector((-0.5, 0.22, 0.84)).normalized()
    P = Vector((LX - RL + 0.004, 0.0, zlid - 0.002 - rh * 0.55))
    B = P - d * 0.17
    E = P + d * 0.30
    handle = [B + (E - B) * (i / 7) for i in range(8)]
    handle += [E + d * 0.02 + Vector((-0.006, 0.0, 0.002)), E + d * 0.026 + Vector((-0.022, 0.0, -0.01)),
               E + d * 0.02 + Vector((-0.03, 0.0, -0.026))]
    b.add(bm_tube(handle, rh, seg=8, squash=0.55), "hc_steel", uv="keep")
    print("stock pot tris", b.tris())
    return b.finish()


def _lathe_r(profile, z):
    """A lathe profile's radius at height z (piecewise linear; profile rising in z)."""
    for (r0, z0), (r1, z1) in zip(profile, profile[1:]):
        if z0 <= z <= z1 and z1 > z0:
            return r0 + (r1 - r0) * (z - z0) / (z1 - z0)
    return profile[-1][0]


def _lathe_mark(b, M, profile, seg, zc, a0, w, h, seed, wmax):
    """An invented brush mark (three strokes) on a lathe's faceted surface, centred at height zc and angle a0."""
    strokes = brush_mark(seed, h, w, wmin=0.0012, wmax=wmax, N=6)[:3]
    step = 2 * math.pi / seg

    def surf(s, tt):
        z = zc + tt
        r = _lathe_r(profile, z)
        a = a0 + s / max(r, 1e-4)
        dd = a % step - step / 2
        rr = r * math.cos(step / 2) / math.cos(dd)  # on the facet, not the true circle
        dr = (_lathe_r(profile, z + 0.002) - _lathe_r(profile, z - 0.002)) / 0.004
        p = M @ Vector((rr * math.cos(a), rr * math.sin(a), z))
        nrm = (M.to_3x3() @ Vector((math.cos(a), math.sin(a), -dr))).normalized()
        return p, nrm
    add_mark(b, strokes, surf, "hc_ink")


def goods_bottles(name):
    """An eatery's bottles and jars on a 0.42 x 0.26 m wooden tray: a tall sauce bottle and a squat dispenser in dark
    glass with red caps and paper labels, a glazed flask, two lidded jars, a stack of four bowls. Origin at the bottom
    centre; the labels and the flask's mark face -Y (the lane). Every mark is an invented abstract brush mark."""
    b = Builder(name)
    rng = random.Random(5150)
    TW, TD, tk, rimh = 0.42, 0.26, 0.012, 0.032
    b.add(bm_box(-TW / 2, -TD / 2, 0.0, TW / 2, TD / 2, tk, bevel=0.003), "hc_timber", grain="x")
    for (x0, y0, x1, y1, g) in ((-TW / 2, -TD / 2, TW / 2, -TD / 2 + tk, "x"), (-TW / 2, TD / 2 - tk, TW / 2, TD / 2, "x"),
                                (-TW / 2, -TD / 2 + tk, -TW / 2 + tk, TD / 2 - tk, "y"),
                                (TW / 2 - tk, -TD / 2 + tk, TW / 2, TD / 2 - tk, "y")):
        b.add(bm_box(x0, y0, tk, x1, y1, rimh, bevel=0.003, seg=2), "hc_timber", grain=g,
              uvoff=(rng.random(), rng.random()))
    z0 = tk
    front = -math.pi / 2
    SB = 20
    # tall sauce bottle
    h, r = 0.24, 0.032
    M = T(-0.155, 0.055, z0) @ R(rng.uniform(-20, 20), "Z")
    tall = [(0.0, 0.0), (r * 0.9, 0.0), (r, 0.006), (r, 0.6 * h), (r * 0.93, 0.66 * h), (r * 0.6, 0.75 * h),
            (r * 0.36, 0.82 * h), (r * 0.34, 0.9 * h), (r * 0.4, 0.915 * h), (r * 0.4, 0.935 * h), (r * 0.33, 0.94 * h),
            (0.0, 0.94 * h)]
    b.add(bm_lathe(tall, SB), "hc_glass", M=M, uv="keep")
    b.add(bm_lathe([(0.0, 0.9 * h), (r * 0.43, 0.9 * h), (r * 0.45, 0.905 * h), (r * 0.45, 0.99 * h), (r * 0.4, h),
                    (0.0, h)], SB), "hc_cap_red", M=M, uv="keep")
    lz0, lz1 = 0.2 * h, 0.52 * h
    band = [(r * 0.985, lz0 - 0.0005), (r * 1.004, lz0), (r * 1.004, lz1), (r * 0.985, lz1 + 0.0005)]
    b.add(bm_lathe(band, SB), "hc_wrapping", M=M, uv="keep")
    # the mark is placed in the bottle's own frame, so with the bottle's turn it faces within 20 degrees of -Y
    _lathe_mark(b, M, [(r * 1.004, lz0), (r * 1.004, lz1)], SB, 0.5 * (lz0 + lz1), front, 0.05, 0.065, 9101, 0.007)
    # squat dispenser with a red spout cap
    h, r = 0.13, 0.035
    M = T(-0.115, -0.06, z0)
    soy = [(0.0, 0.0), (r * 0.9, 0.0), (r, 0.008), (r, 0.55 * h), (r * 0.85, 0.68 * h), (r * 0.5, 0.76 * h),
           (r * 0.45, 0.8 * h), (0.0, 0.8 * h)]
    b.add(bm_lathe(soy, SB), "hc_glass", M=M, uv="keep")
    b.add(bm_lathe([(0.0, 0.78 * h), (r * 0.55, 0.78 * h), (r * 0.6, 0.8 * h), (r * 0.6, 0.9 * h), (r * 0.35, 0.96 * h),
                    (0.0, 0.97 * h)], SB), "hc_cap_red", M=M, uv="keep")
    b.add(bm_tube([(0.0, -0.004, 0.93 * h), (0.0, -r * 0.6, 0.965 * h), (0.0, -r * 0.78, 0.972 * h)], 0.004, seg=6),
          "hc_cap_red", M=M, uv="keep")
    lz0, lz1 = 0.18 * h, 0.48 * h
    band = [(r * 0.985, lz0 - 0.0005), (r * 1.004, lz0), (r * 1.004, lz1), (r * 0.985, lz1 + 0.0005)]
    b.add(bm_lathe(band, SB), "hc_wrapping", M=M, uv="keep")
    _lathe_mark(b, M, [(r * 1.004, lz0), (r * 1.004, lz1)], SB, 0.5 * (lz0 + lz1), front, 0.045, 0.034, 9202, 0.005)
    # glazed flask: unglazed foot, round belly, narrow neck, a flared lip open at the top; a brush mark on the belly
    h, r = 0.175, 0.048
    M = T(-0.04, 0.05, z0)
    b.add(bm_lathe([(0.0, 0.0), (r * 0.62, 0.0), (r * 0.66, 0.01)], SB), "hc_bisque", M=M, uv="keep")
    belly = [(r * 0.66, 0.01), (r * 0.9, 0.05), (r, 0.3 * h), (r * 0.92, 0.48 * h), (r * 0.6, 0.66 * h),
             (r * 0.3, 0.8 * h), (r * 0.27, 0.9 * h), (r * 0.36, 0.97 * h), (r * 0.38, h), (r * 0.28, h - 0.002),
             (r * 0.22, 0.95 * h), (0.0, 0.93 * h)]
    b.add(bm_lathe(belly, SB), "hc_glaze_cream", M=M, uv="keep")
    _lathe_mark(b, M, belly[:6], SB, 0.3 * h, front, 0.05, 0.05, 9303, 0.006)
    # two lidded jars and a stack of bowls
    jar(b, T(0.035, -0.055, z0) @ R(rng.uniform(-180, 180), "Z"), 0.09, 0.042, "hc_glaze_green", rng, seg=14)
    jar(b, T(0.09, 0.06, z0) @ R(rng.uniform(-180, 180), "Z"), 0.11, 0.048, "hc_glaze_brown", rng, seg=14)
    bowl_stack(b, T(0.14, -0.045, z0) @ R(rng.uniform(-180, 180), "Z"), 4, 0.05, rng, "hc_glaze_cream", seg=16)
    print("goods bottles tris", b.tris())
    return b.finish()


# =====================================================================================================================
# Export and read-back
# =====================================================================================================================

def export(ob, path, images):
    bpy.context.view_layer.update()
    for o in bpy.context.view_layer.objects:
        o.select_set(o is ob)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True, export_apply=True, export_yup=True,
                              export_extras=False, export_tangents=False, export_vertex_color="NONE", export_cameras=False,
                              export_lights=False, export_animations=False, export_materials="EXPORT",
                              export_image_format="AUTO" if images else "NONE", export_skins=False,
                              export_morph=False)


def read_back(path, allow_images=False):
    with open(path, "rb") as f:
        data = f.read()
    magic, _v, _l = struct.unpack_from("<4sII", data, 0)
    assert magic == b"glTF", path
    clen, _ = struct.unpack_from("<I4s", data, 12)
    js = json.loads(data[20:20 + clen].decode("utf-8"))
    bin_off = 20 + clen + 8
    errs, tris, uvsets = [], 0, 0
    lo, hi = [1e9] * 3, [-1e9] * 3
    for node in js.get("nodes", []):
        for k in ("translation", "rotation", "scale"):
            if k in node and node[k] not in ([0, 0, 0], [0, 0, 0, 1], [1, 1, 1]):
                errs.append("node %s has a %s" % (node.get("name"), k))
    for mesh in js.get("meshes", []):
        for p in mesh["primitives"]:
            a = p["attributes"]
            n = 0
            while f"TEXCOORD_{n}" in a:
                n += 1
            uvsets = max(uvsets, n)
            if "COLOR_0" in a:
                errs.append("vertex colour")
            acc = js["accessors"][a["POSITION"]]
            for i in range(3):
                lo[i] = min(lo[i], acc["min"][i])
                hi[i] = max(hi[i], acc["max"][i])
            tris += js["accessors"][p["indices"]]["count"] // 3
    if uvsets != 1:
        errs.append(f"{uvsets} UV sets")
    imgs = []
    for im in js.get("images", []):
        if "bufferView" not in im:
            errs.append("image %s not embedded" % im.get("name"))
            continue
        bv = js["bufferViews"][im["bufferView"]]
        raw = data[bin_off + bv.get("byteOffset", 0): bin_off + bv.get("byteOffset", 0) + bv["byteLength"]]
        fmt, w, h = ct._img_size(raw)
        imgs.append("%s %s %dx%d" % (im.get("name"), fmt, w, h))
        if fmt == "?" or w > 1024 or h > 1024:
            errs.append("image %s: %s %dx%d" % (im.get("name"), fmt, w, h))
    if imgs and not allow_images:
        errs.append("has images")
    slots = [m["name"] for m in js.get("materials", [])]
    # glTF is +Y up: bounds as (x, up, -forward); blender_* gives them back in Blender's Z-up frame.
    return {"tris": tris, "min": [round(v, 4) for v in lo], "max": [round(v, 4) for v in hi],
            "blender_min": [round(lo[0], 4), round(-hi[2], 4), round(lo[1], 4)],
            "blender_max": [round(hi[0], 4), round(-lo[2], 4), round(hi[1], 4)],
            "slots": slots, "images": imgs, "bytes": len(data), "errors": errs}


# =====================================================================================================================
# Frontage clutter (11-17): the lane's bare frontage gets a steel drum, a lidded bin, a downpipe run, a cloth awning
# and a pile of grain sacks. Each sits on the ground (origin at its bottom centre) except the wall pieces (downpipe,
# awning), whose origin is on the wall (y = 0) and which stand off it to -Y like the signs.
# =====================================================================================================================

TEXTURED.update({
    # painted steel drum over rusting steel: a deep blue-green coat, chipped at the hoops' height, streaked grime
    "hc_drum_paint": (lambda: gen_worn(51, 0.5, (0.020, 0.045, 0.050), (0.17, 0.07, 0.03), 0.10, 0.45, 0.85,
                                       grime=0.4, streak=0.6), 0.5),
    # moulded street-bin plastic: dark green, scuffed paler where hands and wheels rub, grime down the front
    "hc_bin_plastic": (lambda: gen_worn(52, 0.4, (0.018, 0.035, 0.022), (0.07, 0.08, 0.07), 0.05, 0.55, 0.70,
                                        grime=0.45, streak=0.4, wear=0.08), 0.56),
    # awning canvas: faded oxblood, waxed and stained
    "hc_awning_cloth": (lambda: gen_weave(53, 0.25, colour=(0.16, 0.045, 0.035), thread=0.0018, mottle=0.06,
                                          rough=0.85), 0.85),
})
TILE.update({"hc_drum_paint": 0.5, "hc_bin_plastic": 0.4, "hc_awning_cloth": 0.25})


def drum(name):
    """A 200 l steel drum, 0.58 m across and 0.88 m tall: rolled chimes top and bottom, two rolling hoops, a lid with
    two bung caps, a dent on the -Y side. Origin at the bottom centre."""
    b = Builder(name)
    rng = random.Random(5101)
    R0, H = 0.286, 0.88
    dent = wobble(rng, 0.02)

    def rf(a, z):  # one shallow dent facing the lane, low down
        d = math.exp(-((a + math.pi / 2) ** 2) / 0.12 - ((z - 0.28) ** 2) / 0.02)
        return dent(a, z) * (1.0 - 0.035 * d)

    prof = [(R0 - 0.006, 0.0), (R0 + 0.004, 0.006), (R0 + 0.004, 0.022), (R0, 0.03)]
    for zc in (0.29, 0.59):  # rolling hoops: a raised bead each
        prof += [(R0, zc - 0.018), (R0 + 0.010, zc - 0.006), (R0 + 0.010, zc + 0.006), (R0, zc + 0.018)]
    prof += [(R0, H - 0.03), (R0 + 0.004, H - 0.022), (R0 + 0.004, H - 0.004), (R0 - 0.010, H), (R0 - 0.016, H - 0.012)]
    b.add(bm_lathe(prof, 40, rfun=rf), "hc_drum_paint", uv="keep")
    # the lid, set down 12 mm inside the chime, and the base
    b.add(bm_lathe([(R0 - 0.016, H - 0.012), (0.0, H - 0.012)], 40, smooth=False), "hc_drum_paint", uv="keep")
    b.add(bm_lathe([(0.0, 0.012), (R0 - 0.006, 0.012)], 40, smooth=False), "hc_drum_paint", uv="keep")
    for x, rr in ((0.17, 0.035), (-0.18, 0.022)):  # bung caps
        b.add(bm_lathe([(rr, 0.0), (rr, 0.012), (rr * 0.7, 0.016), (0.0, 0.016)], 12, smooth=False), "hc_steel",
              M=T(x, 0.02, H - 0.012), uv="keep")
    return b.finish()


def street_bin(name):
    """A lidded street bin, 0.58 x 0.50 m and 0.95 m tall to the lid's top: a tapered rounded-square body, a moulded
    rim, a lid hinged at +Y with a grab lip at -Y, two wheels on an axle at the back. Origin at the bottom centre."""
    b = Builder(name)

    def sq(p, sx, sy):
        def f(a, z):
            c, s = abs(math.cos(a)) / sx, abs(math.sin(a)) / sy
            return 1.0 / max(1e-6, (c ** p + s ** p)) ** (1.0 / p)
        return f

    body = sq(6.0, 1.0, 0.86)
    prof = [(0.0, 0.05), (0.235, 0.05), (0.25, 0.07), (0.30, 0.82), (0.305, 0.84), (0.31, 0.86)]  # open mouth, lid over
    b.add(bm_lathe(prof, 32, rfun=body), "hc_bin_plastic", uv="keep", crease=35)
    # the rim lip round the mouth
    b.add(bm_lathe([(0.30, 0.83), (0.322, 0.835), (0.322, 0.865), (0.30, 0.87)], 32, rfun=body), "hc_bin_plastic",
          uv="keep", crease=35)
    # the lid: a shallow dome, closed, a grab lip at -Y
    lid = sq(6.0, 1.0, 0.86)
    b.add(bm_lathe([(0.33, 0.87), (0.33, 0.885), (0.30, 0.92), (0.18, 0.945), (0.0, 0.95)], 32, rfun=lid),
          "hc_bin_plastic", uv="keep", crease=35)
    b.add(bm_box(-0.12, -0.31, 0.855, 0.12, -0.27, 0.885, bevel=0.006, seg=2), "hc_bin_plastic")
    # hinge blocks at +Y
    for x in (-0.18, 0.18):
        b.add(bm_box(x - 0.04, 0.24, 0.85, x + 0.04, 0.30, 0.90, bevel=0.008, seg=2), "hc_bin_plastic")
    # wheels and axle at the back
    for x in (-0.24, 0.24):
        b.add(bm_lathe([(0.0, -0.025), (0.085, -0.025), (0.10, -0.012), (0.10, 0.012), (0.085, 0.025), (0.0, 0.025)],
                       20), "hc_saddle", M=T(x, 0.20, 0.10) @ R(90, "Y"), uv="keep")
    b.add(bm_tube([Vector((-0.27, 0.20, 0.10)), Vector((0.27, 0.20, 0.10))], 0.012, seg=8), "hc_iron", uv="keep")
    # the moulded number plate on the front: a raised blank panel (no letters)
    b.add(bm_box(-0.09, -0.268 - 0.006, 0.55, 0.09, -0.262, 0.66, bevel=0.004, seg=2), "hc_bin_plastic")
    return b.finish()


def down_pipe(name):
    """A 4.2 m cast downpipe with a hopper head under the eave, a swan neck off the wall, five wall brackets and a
    shoe kicking out at the foot. Origin on the wall at the foot (wall y = 0); the pipe stands 0.09 m off it (-Y)."""
    b = Builder(name)
    rng = random.Random(5303)
    r, off = 0.048, -0.09
    path = [  # shoe kicking out at the foot, then up the wall
        Vector((0, off - 0.10, 0.02)), Vector((0, off - 0.05, 0.10)), Vector((0, off, 0.20))]
    path += [Vector((0, off, z)) for z in (0.6, 1.4, 2.2, 3.0, 3.55)]
    path += [Vector((0, off - 0.04, 3.70)), Vector((0, off - 0.14, 3.85)), Vector((0, off - 0.16, 3.95))]  # swan neck
    b.add(bm_tube(path, r, seg=14, caps=True), "hc_rust", uv="keep")
    # socket collars every joint
    for z in (0.75, 1.55, 2.35, 3.15):
        b.add(bm_lathe([(r, -0.03), (r + 0.010, -0.025), (r + 0.010, 0.025), (r, 0.03)], 14), "hc_rust",
              M=T(0, off, z), uv="keep")
    # brackets: an iron band round the pipe and a lug into the wall
    for z in (0.5, 1.3, 2.1, 2.9, 3.45):
        b.add(bm_torus(r + 0.006, 0.006, 16, 4), "hc_iron", M=T(0, off, z), uv="keep")
        b.add(bm_box(-0.012, off + r, z - 0.012, 0.012, 0.0, z + 0.012, bevel=0.002), "hc_iron")
    # hopper head under the eave line
    hz = 3.95
    b.add(bm_lathe([(r * 0.9, hz), (0.10, hz + 0.10), (0.15, hz + 0.24), (0.155, hz + 0.27), (0.14, hz + 0.27),
                    (0.135, hz + 0.25), (0.09, hz + 0.12), (r * 0.8, hz + 0.02)], 4, phase=math.pi / 4, smooth=False,
                   rfun=wobble(rng, 0.01)), "hc_rust", M=T(0, off - 0.16, 0), uv="keep")
    return b.finish()


def awning(name):
    """A 2.7 m cloth awning over a shop front: a wall rail, two folding iron arms out 1.1 m and down 0.45 m, the
    canvas sagging between rail and front bar, a 0.22 m straight valance hanging off the front. Origin on the wall at
    the rail's centre (wall y = 0, out to -Y, down to -Z)."""
    b = Builder(name)
    W, P, D = 2.7, 1.1, 0.45
    b.add(bm_box(-W / 2, -0.07, -0.05, W / 2, 0.0, 0.03, bevel=0.01, seg=2), "hc_iron")  # rail and roller box
    bar = [Vector((-W / 2 - 0.02, -P, -D)), Vector((W / 2 + 0.02, -P, -D))]
    b.add(bm_tube(bar, 0.022, seg=10), "hc_iron", uv="keep")
    for x in (-W / 2 + 0.08, W / 2 - 0.08):  # arms: elbowed, from the wall low to the front bar
        arm = [Vector((x, -0.02, -0.75)), Vector((x, -0.45, -0.62)), Vector((x, -P + 0.02, -D))]
        b.add(bm_tube(arm, 0.014, seg=8), "hc_iron", uv="keep")
        b.add(bm_box(x - 0.04, -0.02, -0.80, x + 0.04, 0.0, -0.70, bevel=0.004), "hc_iron")
    # canvas: z drops linearly from rail to bar with a sag between the arms and across
    nu, nv = 28, 10

    def surf(u, v):
        x = -W / 2 + W * u
        y = -0.06 - (P - 0.06) * v
        z = -0.0 - D * v - 0.06 * math.sin(math.pi * v) * (0.6 + 0.4 * math.sin(math.pi * u)) \
            - 0.012 * math.sin(9 * math.pi * u) * math.sin(math.pi * v)
        return Vector((x, y, z))
    top, bot, uvs = [], [], []
    for j in range(nv + 1):
        rt, rb, ru = [], [], []
        for i in range(nu + 1):
            p = surf(i / nu, j / nv)
            rt.append(p)
            rb.append(p + Vector((0, 0, -0.004)))
            ru.append((W * i / nu, P * j / nv))
        top.append(rt)
        bot.append(rb)
        uvs.append(ru)
    b.add(bm_shell(top, bot, uvs), "hc_awning_cloth", uv="keep")
    # valance: hangs from the bar, a slight ripple
    top, bot, uvs = cloth_panel(lambda x, z: -P - 0.024 + 0.008 * math.sin(11 * x) * (-z / 0.22), W + 0.02, 0.22,
                                30, 3, 0.004)
    top = [[p + Vector((0, 0, -D)) for p in row] for row in top]
    bot = [[p + Vector((0, 0, -D)) for p in row] for row in bot]
    b.add(bm_shell(top, bot, uvs), "hc_awning_cloth", uv="keep")
    return b.finish()


def sack_pile(name):
    """Five grain sacks: three along the bottom, two laid on them, 1.1 x 0.55 m and 0.75 m high. Origin at the
    bottom centre."""
    b = Builder(name)
    rng = random.Random(5404)
    for k, (x, y) in enumerate(((-0.36, 0.0), (0.0, 0.03), (0.36, -0.02))):
        sack(b, T(x, y, 0) @ R(rng.uniform(-20, 20), "Z") @ S(1.0, 0.8, 1.0), 0.48 + rng.uniform(-0.03, 0.03), 0.19,
             rng, seg=14)
    for x in (-0.18, 0.19):  # the top two lie on their sides along X
        sack(b, T(x - 0.22, rng.uniform(-0.03, 0.03), 0.42) @ R(90 + rng.uniform(-8, 8), "Y") @ S(0.8, 0.9, 1.0),
             0.46, 0.17, rng, seg=14)
    return b.finish()


# name: (builder, triangle budget, textured)
PARTS = {
    "SM_HC_LanternLarge": (lambda n: lantern(n, 0.60, 0.45, 16, 0.25), 6000, False),
    "SM_HC_LanternSmall": (lambda n: lantern(n, 0.32, 0.25, 14, 0.15), 6000, False),
    "SM_HC_LanternLargeMark": (lambda n: lantern(n, 0.60, 0.45, 16, 0.25, mark_seed=1717), 6000, False),
    "SM_HC_StringPost": (lambda n: post(n), 500, False),
    "SM_HC_ShelfUnit": (shelf_unit, 40000, True),
    "SM_HC_Counter": (counter, 15000, True),
    "SM_HC_HangGoods": (hang_goods, 20000, True),
    "SM_HC_HangSignL": (lambda n: hang_sign(n, 0.40, 1.30, 0.045, 8101), 8000, True),
    "SM_HC_HangSignS": (lambda n: hang_sign(n, 0.28, 0.80, 0.035, 8202), 8000, True),
    "SM_HC_Banner": (banner, 10000, True),
    "SM_HC_Crate": (crate, 8000, True),
    "SM_HC_Bucket": (bucket, 8000, True),
    "SM_HC_Gate": (gate, 15000, True),
    "BICYCLE": (bicycle, 16000, True),  # the kit's piece: import with -Kit -NoNanite to /Game/Props/BKIT
    "SM_HC_StockPot": (stock_pot, 6500, True),  # 6380 measured on first run (11-16)
    "SM_HC_GoodsBottles": (goods_bottles, 5000, True),
    "SM_HC_Drum": (drum, 6000, True),
    "SM_HC_Bin": (street_bin, 6000, True),
    "SM_HC_DownPipe": (down_pipe, 5000, True),
    "SM_HC_Awning": (awning, 4000, True),
    "SM_HC_SackPile": (sack_pile, 5000, True),
}


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not argv:
        raise SystemExit("usage: ... -- <out_dir> [--only A,B]")
    out = os.path.abspath(argv[0])
    only = set(argv[argv.index("--only") + 1].split(",")) if "--only" in argv else None
    os.makedirs(out, exist_ok=True)
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    jpath = os.path.join(out, "hero_corner_props.json")
    report = {}
    if os.path.exists(jpath):
        with open(jpath, encoding="utf-8") as f:
            report = json.load(f)
    failed = False
    for name, (make, budget, textured) in PARTS.items():
        if only and name not in only:
            continue
        ob = make(name)
        path = os.path.join(out, name + ".glb")
        export(ob, path, textured)
        r = read_back(path, allow_images=textured)
        if r["tris"] > budget:
            r["errors"].append("over budget %d" % budget)
        r["budget"] = budget
        r["path"] = path
        report[name] = r
        failed |= bool(r["errors"])
        print(f"{name}: {r['tris']} tris (budget {budget}), bounds {r['blender_min']}..{r['blender_max']} (Blender Z up), "
              f"slots {r['slots']}, images {len(r['images'])}, errors {r['errors'] or 'none'}")
        bpy.data.objects.remove(ob, do_unlink=True)
    with open(jpath, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    if failed:
        raise SystemExit("read-back errors")


main()
