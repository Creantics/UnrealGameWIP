"""The hero corner's harbour backdrop: three moored boats and a lit far-bank tower, modelled by script.

    blender -b --factory-startup -t 6 --python-exit-code 1 --python tools/blender/harbour_props.py -- <out_dir> [--only A,B]

Run it as one Blender at BelowNormal priority. Writes into <out_dir> (metres, Z up in Blender, glTF +Y up in the files):
  SM_HB_WorkBoat.glb   6.0 x 1.8 m open working boat: foredeck, plank sole, thwarts, a 6.5 m mast with a furled sail on
                       its boom and gaff, oars on the thwarts, a rope coil, two fenders to port
  SM_HB_CabinBoat.glb  7.2 x 2.1 m boat with a small arched cabin aft of an 8.5 m mast, a stove pipe, a ring on the
                       cabin's aft wall, fenders both sides
  SM_HB_Skiff.glb      7.5 x 1.35 m long, low skiff with a woven-mat barrel canopy, a 4.5 m mast and a sculling oar
  SM_HB_FarTower.glb   about 24.4 m four-tier tower on a stone plinth: tiled hip roofs with upturned eaves, rafter tails, lattice
                       paper windows on tiers 1-3, a round glowing face on each side of tier 4, a bronze finial
  harbour_props.json   triangles, bounds, slots and images per file, from the files read back (merged)
With --only, only the named pieces are built and their rows replace those in the json.

Boats: origin at the waterline under the hull's middle (z = 0 is the waterline), bow along +Y, port at -X. Each boat has
stays (forestay, two shrouds, backstay) and ONE mast-head lamp hung from a short arm, slot lantern_paper (the slot Unreal
makes glow; no light in the file).
Tower: origin at the ground under the plinth's centre; its door faces -Y. Windows and faces are on slot tower_paper, which
Unreal also makes glow (every slot whose name contains "paper").

These are backdrop pieces: the playable vehicle-class boat is a separate design that waits for its sheet. Every piece is an
original design, not a copy of any real boat or building. There are no letters or marks on any piece. Builders and materials
come from hero_corner_props.py, loaded without its main().
"""
import json
import math
import os
import random
import sys

import bpy
from mathutils import Vector


def _load_hc():
    """hero_corner_props.py's builders, materials and export helpers, loaded without running its main()."""
    import types
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hero_corner_props.py")
    with open(path, encoding="utf-8") as f:
        src = f.read()
    src = src[:src.rindex("\nmain()")]
    mod = types.ModuleType("hero_corner_props")
    mod.__file__ = path
    exec(compile(src, path, "exec"), mod.__dict__)
    return mod


hc = _load_hc()
Builder, T, R = hc.Builder, hc.T, hc.R
bm_box, bm_lathe, bm_tube, bm_shell, bm_prism, bm_torus = (hc.bm_box, hc.bm_lathe, hc.bm_tube, hc.bm_shell, hc.bm_prism,
                                                           hc.bm_torus)

# =====================================================================================================================
# Materials (registered in hero_corner_props' tables, so its material() builds them)
# =====================================================================================================================

hc.MATERIALS.update({
    "hb_tarline": ((0.030, 0.024, 0.018), 0.75, 0.0),     # tarred standing rigging and rope
    "hb_bronze": ((0.30, 0.20, 0.09), 0.40, 1.0),         # tower finial, face rims
    "hb_pane": ((0.012, 0.014, 0.016), 0.12, 0.0),        # dark cabin windows (unlit)
    "hb_plaster": ((0.46, 0.42, 0.36), 0.85, 0.0),        # tower infill walls
    "hb_canvas": ((0.060, 0.055, 0.045), 0.80, 0.0),      # tarred canvas cabin roof
    "hb_ring": ((0.25, 0.035, 0.020), 0.35, 0.0),         # the cabin boat's ring
    "tower_paper": ((0.80, 0.62, 0.38), 0.85, 0.0),       # warm shoji paper (Unreal: the night glow)
})
hc.TEXTURED.update({
    "hb_hull_teal": (lambda: hc.gen_worn(51, 0.6, (0.030, 0.075, 0.075), (0.11, 0.075, 0.045), 0.10, 0.5, 0.8,
                                         grime=0.35, streak=0.6), 0.55),
    "hb_hull_oxblood": (lambda: hc.gen_worn(52, 0.6, (0.11, 0.025, 0.018), (0.11, 0.075, 0.045), 0.12, 0.5, 0.8,
                                            grime=0.35, streak=0.6), 0.55),
    "hb_hull_black": (lambda: hc.gen_worn(53, 0.6, (0.018, 0.018, 0.019), (0.10, 0.07, 0.045), 0.08, 0.4, 0.8,
                                          grime=0.25, streak=0.5), 0.5),
    "hb_sail": (lambda: hc.gen_weave(54, 0.3, colour=(0.36, 0.20, 0.11), thread=0.003, mottle=0.12), 0.9),
    "hb_mat": (lambda: hc.gen_weave(55, 0.3, colour=(0.30, 0.23, 0.12), thread=0.006, mottle=0.10), 0.9),
})
hc.TILE.update({"hb_hull_teal": 0.6, "hb_hull_oxblood": 0.6, "hb_hull_black": 0.6, "hb_sail": 0.3, "hb_mat": 0.3})


def _sm(t):
    t = min(1.0, max(0.0, t))
    return t * t * (3 - 2 * t)


def line(a, b, sag=0.004, n=6):
    """A rope from a to b sagging by sag x its length at the middle."""
    A, B = Vector(a), Vector(b)
    L = (B - A).length
    return [A.lerp(B, i / n) - Vector((0, 0, 4 * sag * L * (i / n) * (1 - i / n))) for i in range(n + 1)]


# =====================================================================================================================
# Hulls: u runs 0 (transom) .. 1 (stem); a cross-section runs s = -1 (port gunwale) .. 0 (keel) .. 1 (starboard)
# =====================================================================================================================

P_BILGE = 1.7   # cross-section: z = g - (g - k)(1 - |s|^P): a round V
T_PLANK = 0.035


class Hull:
    def __init__(self, L, B, fb, draft, sheer_bow, sheer_stern, transom):
        self.L, self.B, self.fb, self.draft = L, B, fb, draft
        self.sheer_bow, self.sheer_stern, self.transom = sheer_bow, sheer_stern, transom

    def y(self, u):
        return -self.L / 2 + self.L * u

    def hb(self, u):  # half-beam: a transom aft, full at u 0.42, fine to the stem
        if u <= 0.42:
            return 0.5 * self.B * (self.transom + (1 - self.transom) * math.sin(0.5 * math.pi * u / 0.42))
        t = (u - 0.42) / 0.58
        return max(0.012, 0.5 * self.B * math.cos(0.5 * math.pi * min(1.0, t)) ** 0.85)

    def zg(self, u):  # gunwale (sheer line) over the waterline: rises to the bow, a little to the stern
        return (self.fb + self.sheer_bow * max(0.0, (u - 0.45) / 0.55) ** 2 +
                self.sheer_stern * max(0.0, (0.3 - u) / 0.3) ** 2)

    def zk(self, u):  # keel line: the forefoot rises into the stem, the run lifts a little to the transom
        return -self.draft * (1 - 0.85 * _sm((u - 0.72) / 0.28)) * (1 - 0.3 * _sm((0.12 - u) / 0.12))

    @staticmethod
    def xs(h, s):  # half-width at section parameter s >= 0: some flare, a rounded bilge
        return h * (0.7 * math.sin(0.5 * math.pi * s) + 0.3 * s)

    def point(self, u, s, inset=0.0):
        h = max(0.004, self.hb(u) - inset)
        g, k = self.zg(u), self.zk(u) + inset
        z = g - (g - k) * (1 - abs(s) ** P_BILGE)
        return Vector((math.copysign(self.xs(h, abs(s)), s), self.y(u), z))

    def half_width_at(self, u, z, inset=T_PLANK):
        """The inner hull's half-width at height z in station u."""
        h = max(0.004, self.hb(u) - inset)
        g, k = self.zg(u), self.zk(u) + inset
        f = min(1.0, max(0.0, (z - k) / max(1e-6, g - k)))
        return self.xs(h, f ** (1.0 / P_BILGE))


def hull_skin(b, H, mat, nj=40, ni=20):
    """The planked shell: an outer and an inner sheet closed along the gunwales and the ends."""
    outer, inner, uvs = [], [], []
    for j in range(nj + 1):
        u = j / nj
        ro, ri, ru = [], [], []
        girth = H.zg(u) - H.zk(u) + H.hb(u)
        for i in range(ni + 1):
            s = -1 + 2 * i / ni
            ro.append(H.point(u, s))
            ri.append(H.point(u, s, T_PLANK))
            ru.append((H.y(u), s * girth))
        outer.append(ro)
        inner.append(ri)
        uvs.append(ru)
    b.add(bm_shell(outer, inner, uvs, smooth=True), mat, uv="keep")
    # laps between the strakes: thin beads along the outside at three heights
    for s0 in (0.38, 0.62, 0.82):
        for sg in (-1, 1):
            pts = []
            for j in range(2, nj - 1):
                u = j / nj
                p = H.point(u, sg * s0)
                pts.append(p + Vector((sg * 0.006, 0, 0)))
            b.add(bm_tube(pts, 0.011, seg=5), mat, uv="keep")
    # rubbing strake under the gunwale
    for sg in (-1, 1):
        pts = [H.point(j / nj, sg * 1.0) + Vector((sg * 0.022, 0, -0.045)) for j in range(1, nj)]
        b.add(bm_tube(pts, 0.03, seg=6, squash=0.8), "hc_timber", uv="keep")
    # the transom: a plate across the stern section, capped
    sec = [H.point(0.0, -1 + 2 * i / ni) for i in range(ni + 1)]
    poly = [(p.x, p.z) for p in sec]
    y0 = H.y(0.0)
    b.add(bm_prism(poly, y0 - 0.035, y0 + 0.02), mat)
    b.add(bm_box(sec[0].x - 0.03, y0 - 0.05, sec[0].z - 0.01, sec[-1].x + 0.03, y0 + 0.04, sec[0].z + 0.04),
          "hc_timber", grain="x")
    # the stem post: up the forefoot and over the sheer
    pts = [Vector((0, H.y(u), H.zk(u) - 0.01)) for u in (0.80, 0.86, 0.91, 0.95, 0.98, 1.0)]
    pts.append(Vector((0, H.y(1.0) + 0.03, H.zg(1.0) + 0.14)))
    b.add(bm_tube(pts, 0.045, seg=8, squash=0.7), "hc_timber", uv="keep")


def plate(b, H, u0, u1, z, th, mat, f0=-1.0, f1=1.0, n=10, clear=0.012):
    """A flat board inside the hull at height z from station u0 to u1, its edges fitted to the inner skin; f0..f1 the
    board's share of the width (-1 port, 1 starboard)."""
    top, bot, uvs = [], [], []
    for j in range(n + 1):
        u = u0 + (u1 - u0) * j / n
        y = H.y(u)
        w = max(0.005, H.half_width_at(u, z - th) - clear)
        xa, xb = f0 * w, f1 * w
        top.append([Vector((xa, y, z)), Vector((xb, y, z))])
        bot.append([Vector((xa, y, z - th)), Vector((xb, y, z - th))])
        uvs.append([(xa, y), (xb, y)])
    b.add(bm_shell(top, bot, uvs, smooth=False), mat, uv="keep")


def thwart(b, H, u, z, depth=0.22, th=0.045):
    w = H.half_width_at(u, z - th) - 0.01
    y = H.y(u)
    b.add(bm_box(-w, y - depth / 2, z - th, w, y + depth / 2, z, bevel=0.006), "hc_timber", grain="x")
    return z


def sole(b, H, u0, u1):
    """Plank floor: four boards with small gaps. Returns its height."""
    z = H.zk(0.45) + 0.17
    for f0, f1 in ((-1.0, -0.51), (-0.49, -0.01), (0.01, 0.49), (0.51, 1.0)):
        plate(b, H, u0, u1, z, 0.03, "hc_timber", f0, f1)
    return z


def mast_lamp(b, x, y, z):
    """One paper lamp hung by its hook at (x, y, z): iron hook, lacquered caps, the glowing paper between."""
    M = T(x, y, z)
    b.add(bm_torus(0.018, 0.004, 10, 5), "lantern_iron", M=M @ R(90, "X") @ T(0, 0, 0), uv="keep")
    b.add(bm_tube([(0, 0, -0.012), (0, 0, -0.045)], 0.004, seg=6), "lantern_iron", M=M, uv="keep")
    b.add(bm_lathe([(0.0, -0.045), (0.05, -0.045), (0.055, -0.06), (0.045, -0.07), (0.0, -0.07)], 16),
          "lantern_wood", M=M, uv="keep")
    zt, zb = -0.066, -0.29
    zc, hz = 0.5 * (zt + zb), 0.5 * (zt - zb)
    prof = []
    for i in range(13):
        zz = zb + (zt - zb) * i / 12
        e = math.sqrt(max(0.0, 1 - ((zz - zc) / (hz * 1.06)) ** 2))
        prof.append((max(0.042, 0.085 * e), zz))
    b.add(bm_lathe(prof, 16), "lantern_paper", M=M, uv="keep")
    b.add(bm_lathe([(0.0, -0.31), (0.045, -0.31), (0.052, -0.296), (0.046, -0.286), (0.0, -0.286)], 16),
          "lantern_wood", M=M, uv="keep")


def rig(b, H, mast_u, mast_h, z_step, rng, backstay=True):
    """Mast stepped on the sole, a truck on top, the lamp on its arm, forestay, two shrouds and (not with a boom that
    would cross it) a backstay."""
    ym = H.y(mast_u)
    r0 = 0.04 + 0.006 * mast_h
    b.add(bm_tube([(0, ym, z_step), (0, ym, mast_h * 0.5), (0, ym, mast_h - 0.05)],
                  [r0, r0 * 0.9, r0 * 0.62], seg=10), "hc_timber", uv="keep")
    b.add(bm_lathe([(0.0, mast_h - 0.06), (r0 * 0.9, mast_h - 0.06), (r0 * 0.9, mast_h), (0.0, mast_h + 0.01)], 12),
          "hc_timber", uv="keep")
    # partners: the mast through a thwart
    thwart(b, H, mast_u, H.zg(mast_u) - 0.18, depth=0.26)
    za = mast_h - 0.32
    b.add(bm_tube([(0, ym, za), (0, ym + 0.42, za + 0.02)], 0.012, seg=6), "hc_iron", uv="keep")
    b.add(bm_tube([(0, ym, za - 0.18), (0, ym + 0.30, za)], 0.008, seg=6), "hc_iron", uv="keep")  # brace
    mast_lamp(b, 0.0, ym + 0.42, za + 0.0)
    head = (0, ym, mast_h - 0.2)
    stays = [line(head, (0, H.y(1.0) + 0.02, H.zg(1.0) + 0.08))]
    if backstay:
        stays.append(line(head, (0, H.y(0.0) + 0.05, H.zg(0.0) + 0.05)))
    for sg in (-1, 1):
        foot = (sg * (H.hb(mast_u) + 0.02), ym - 0.28, H.zg(mast_u) - 0.06)
        stays.append(line((0, ym, mast_h - 0.35), foot, sag=0.002))
        b.add(bm_box(foot[0] - 0.006 * sg - 0.008, foot[1] - 0.02, foot[2] - 0.16, foot[0] + 0.008 - 0.006 * sg,
                     foot[1] + 0.02, foot[2] + 0.02), "hc_iron")  # chainplate
    for pts in stays:
        b.add(bm_tube(pts, 0.006, seg=4), "hb_tarline", uv="keep")


def fender(b, H, u, sg):
    x = sg * (H.hb(u) + 0.11)
    y = H.y(u)
    z = H.zg(u)
    b.add(bm_tube([(sg * (H.hb(u) - 0.01), y, z + 0.01), (x, y, z - 0.02), (x, y, z - 0.12)], 0.006, seg=4),
          "hb_tarline", uv="keep")
    prof = [(0.0, -0.55), (0.05, -0.54), (0.085, -0.48), (0.095, -0.34), (0.085, -0.2), (0.05, -0.14), (0.0, -0.13)]
    b.add(bm_lathe(prof, 12), "hc_rubber", M=T(x, y, z), uv="keep")


def oar(b, a, c, blade_at_c=True):
    A, C = Vector(a), Vector(c)
    d = (C - A).normalized()
    k = A.lerp(C, 0.68)
    b.add(bm_tube([A, k], 0.026, seg=8), "hc_timber", uv="keep")
    b.add(bm_tube([k, k + d * 0.15, C - d * 0.05, C], [0.026, 0.028, 0.028, 0.018], seg=8, squash=3.4), "hc_timber",
          uv="keep")


def rope_coil(b, x, y, z, turns=3):
    for i in range(turns):
        b.add(bm_torus(0.17 - 0.012 * i, 0.012, 20, 5), "hb_tarline", M=T(x, y, z + 0.012 + 0.022 * i), uv="keep")


# =====================================================================================================================
# The three boats
# =====================================================================================================================

def work_boat(name):
    b = Builder(name)
    rng = random.Random(61)
    H = Hull(6.0, 1.8, 0.55, 0.35, 0.32, 0.08, 0.72)
    hull_skin(b, H, "hb_hull_teal")
    zs = sole(b, H, 0.08, 0.80)
    zd = H.zg(0.8) - 0.05
    plate(b, H, 0.80, 0.985, zd, 0.04, "hc_timber")
    for u in (0.22, 0.44):
        thwart(b, H, u, H.zg(u) - 0.2)
    rig(b, H, 0.66, 6.5, zs, rng, backstay=False)
    # boom, furled sail and gaff, aft from the mast
    ym = H.y(0.66)
    zb = H.zg(0.66) + 1.05
    boom = [Vector((0, ym - 0.08, zb)), Vector((0.03, H.y(0.35), zb + 0.08)), Vector((0.05, H.y(0.1), zb + 0.16))]
    b.add(bm_tube(boom, [0.045, 0.04, 0.032], seg=8), "hc_timber", uv="keep")
    bundle = []
    radii = []
    for i in range(9):
        t = i / 8
        p = boom[0].lerp(boom[1], min(1, t * 1.8)) if t < 0.55 else boom[1].lerp(boom[2], (t - 0.55) / 0.45)
        bundle.append(p + Vector((0.0, 0.0, 0.12 + 0.02 * rng.uniform(-1, 1))))
        radii.append((0.09 + 0.05 * math.sin(math.pi * t)) * (1 + 0.12 * rng.uniform(-1, 1)))
    b.add(bm_tube(bundle, radii, seg=10, squash=0.75), "hb_sail", uv="keep")
    for i in (1, 3, 5, 7):
        p = bundle[i]
        b.add(bm_torus(radii[i] * 0.95, 0.011, 14, 5), "hb_tarline", M=T(p.x, p.y, p.z) @ R(90, "X"), uv="keep")
    gaff = [Vector((0, ym - 0.1, zb + 0.34)), Vector((0.04, H.y(0.2), zb + 0.42))]
    b.add(bm_tube(gaff, [0.036, 0.028], seg=8), "hc_timber", uv="keep")
    b.add(bm_tube(line((0, ym, 6.5 - 0.5), (0.04, H.y(0.38), zb + 0.4), sag=0.01), 0.006, seg=4), "hb_tarline",
          uv="keep")
    # oars across the thwarts, a rope coil on the foredeck, fenders to port
    zt = H.zg(0.33) - 0.15
    oar(b, (0.38, H.y(0.08), zt), (0.42, H.y(0.62), zt + 0.02))
    oar(b, (-0.36, H.y(0.10), zt + 0.01), (-0.30, H.y(0.60), zt + 0.03))
    rope_coil(b, 0.05, H.y(0.88), zd)
    for u in (0.3, 0.55):
        fender(b, H, u, -1)
    print("work boat tris", b.tris())
    return b.finish()


def cabin_boat(name):
    b = Builder(name)
    rng = random.Random(62)
    H = Hull(7.2, 2.1, 0.65, 0.45, 0.38, 0.10, 0.78)
    hull_skin(b, H, "hb_hull_oxblood")
    zs = sole(b, H, 0.08, 0.82)
    zd = H.zg(0.82) - 0.05
    plate(b, H, 0.82, 0.985, zd, 0.04, "hc_timber")
    rig(b, H, 0.70, 8.5, zs, rng)
    # the cabin, aft of the mast, on its own deck 0.25 m over the waterline (the hull only widens above it)
    u0, u1 = 0.16, 0.50
    y0, y1 = H.y(u0), H.y(u1)
    zc0 = 0.25
    plate(b, H, u0 - 0.02, u1 + 0.02, zc0, 0.04, "hc_timber")
    w = 0.9 * min(H.half_width_at(u, zc0 - 0.04) for u in (u0, (u0 + u1) / 2, u1)) - 0.04
    z1 = H.zg(0.33) + 0.95
    b.add(bm_box(-w, y0, zc0, w, y1, z1, bevel=0.01), "hc_timber")
    for sg in (-1, 1):  # corner posts
        for yy in (y0, y1):
            b.add(bm_box(sg * w - 0.035, yy - 0.035, zc0, sg * w + 0.035, yy + 0.035, z1 + 0.02), "hc_lacquer")
    arc = 10
    rise = 0.2
    top = [(-(w + 0.09) + 2 * (w + 0.09) * i / arc, z1 + rise * math.sin(math.pi * i / arc)) for i in range(arc + 1)]
    poly = top + [(x, z - 0.05) for x, z in reversed(top)]
    b.add(bm_prism(poly, y0 - 0.12, y1 + 0.12), "hb_canvas")
    zc = 0.5 * (zc0 + z1) + 0.1
    for sg in (-1, 1):  # two windows a side, framed
        for yc in (y0 + 0.32 * (y1 - y0), y0 + 0.72 * (y1 - y0)):
            x = sg * (w + 0.004)
            b.add(bm_box(x - 0.006, yc - 0.26, zc - 0.17, x + 0.006, yc + 0.26, zc + 0.17), "hb_pane")
            for (ya, yb, za, zb2) in ((yc - 0.3, yc + 0.3, zc + 0.17, zc + 0.21), (yc - 0.3, yc + 0.3, zc - 0.21, zc - 0.17),
                                      (yc - 0.3, yc - 0.26, zc - 0.17, zc + 0.17), (yc + 0.26, yc + 0.3, zc - 0.17, zc + 0.17)):
                b.add(bm_box(x - 0.016, ya, za, x + 0.016, yb, zb2), "hc_timber", grain="y")
    # the forward door, the stove pipe, a ring on the aft wall
    b.add(bm_box(-0.3, y1, zc0 + 0.05, 0.3, y1 + 0.02, z1 - 0.08), "hc_lacquer")
    b.add(bm_box(0.2, y1 + 0.02, zc - 0.13, 0.24, y1 + 0.05, zc - 0.07), "hc_brass")
    px, py = 0.42 * w, y0 + 0.25
    b.add(bm_tube([(px, py, z1 + 0.1), (px, py, z1 + 0.75)], 0.045, seg=10), "hc_iron", uv="keep")
    b.add(bm_lathe([(0.0, z1 + 0.75), (0.09, z1 + 0.76), (0.07, z1 + 0.81), (0.0, z1 + 0.82)], 12), "hc_iron",
          uv="keep")
    b.add(bm_torus(0.26, 0.05, 24, 8), "hb_ring", M=T(0, y0 - 0.055, zc) @ R(90, "X"), uv="keep")
    for k in range(4):
        a = math.pi / 4 + k * math.pi / 2
        b.add(bm_torus(0.055, 0.008, 10, 4), "hc_straw",
              M=T(0.26 * math.cos(a), y0 - 0.055, zc + 0.26 * math.sin(a)) @ R(90, "X") @
              R(math.degrees(a) + 90, "Z") @ R(90, "Y"), uv="keep")  # axis along the ring's tube
    # thwart forward, a coil on the foredeck, fenders both sides
    thwart(b, H, 0.58, H.zg(0.58) - 0.2)
    rope_coil(b, -0.05, H.y(0.9), zd, 4)
    for u, sg in ((0.28, -1), (0.48, -1), (0.33, 1)):
        fender(b, H, u, sg)
    print("cabin boat tris", b.tris())
    return b.finish()


def skiff(name):
    b = Builder(name)
    rng = random.Random(63)
    H = Hull(7.5, 1.35, 0.42, 0.25, 0.45, 0.30, 0.45)
    hull_skin(b, H, "hb_hull_black")
    zs = sole(b, H, 0.1, 0.84)
    zd = H.zg(0.86) - 0.05
    plate(b, H, 0.86, 0.985, zd, 0.035, "hc_timber")
    for u in (0.18, 0.7):
        thwart(b, H, u, H.zg(u) - 0.16, depth=0.18)
    rig(b, H, 0.80, 4.5, zs, rng)
    # barrel canopy: bamboo hoops under a woven mat
    ua, ub = 0.28, 0.64
    na, ns = 12, 10
    top, bot, uvs = [], [], []
    for j in range(ns + 1):
        u = ua + (ub - ua) * j / ns
        y = H.y(u)
        wc = H.hb(u) - 0.05
        z0 = H.zg(u) - 0.03
        hh = 0.62 + 0.04 * math.sin(math.pi * j / ns)
        rt, rb, ru = [], [], []
        for i in range(na + 1):
            a = math.pi * i / na
            rt.append(Vector((wc * math.cos(a), y, z0 + hh * math.sin(a))))
            rb.append(Vector(((wc - 0.012) * math.cos(a), y, z0 + (hh - 0.012) * math.sin(a))))
            ru.append((a * wc, y))
        top.append(rt)
        bot.append(rb)
        uvs.append(ru)
    b.add(bm_shell(top, bot, uvs, smooth=True), "hb_mat", uv="keep")
    for j in (0, 3, 7, 10):
        u = ua + (ub - ua) * j / ns
        y, wc, z0 = H.y(u), H.hb(u) - 0.05, H.zg(u) - 0.03
        hh = 0.62 + 0.04 * math.sin(math.pi * j / ns)
        pts = [Vector(((wc + 0.014) * math.cos(math.pi * i / 10), y, z0 + (hh + 0.014) * math.sin(math.pi * i / 10)))
               for i in range(11)]
        b.add(bm_tube(pts, 0.016, seg=6), "hc_bamboo", uv="keep")
    # the sculling oar over the stern, in its crutch
    st = Vector((0.12, H.y(0.0) + 0.1, H.zg(0.0) + 0.06))
    b.add(bm_tube([st + Vector((0, 0, -0.08)), st + Vector((0, 0, 0.04))], 0.02, seg=6), "hc_iron", uv="keep")
    oar(b, st + Vector((-0.2, 1.1, 0.35)), st + Vector((0.35, -2.1, -0.62)))
    rope_coil(b, 0.0, H.y(0.92), zd, 2)
    print("skiff tris", b.tris())
    return b.finish()


# =====================================================================================================================
# The far-bank tower
# =====================================================================================================================

def roof_z(x, y, E, top, z_eave, rise, upturn):
    """A square hip roof: the eave at max(|x|, |y|) = E, the top at top; concave (flat at the eave, steep at the top),
    the eave's corners lifted by upturn."""
    ax, ay = abs(x), abs(y)
    d = max(ax, ay)
    t = min(1.0, max(0.0, (E - d) / max(1e-6, E - top)))
    c = min(ax, ay) / max(d, 1e-6)
    return z_eave + rise * t ** 1.5 + upturn * c ** 6 * (1 - t) ** 2


def hip_roof(b, half, overhang, z_eave, rise, top, upturn=0.35, th=0.09, n=36, per=0.3):
    E = half + overhang
    rows_t, rows_b, uvs = [], [], []
    for j in range(n + 1):
        yy = -E + 2 * E * j / n
        rt, rb, ru = [], [], []
        for i in range(n + 1):
            xx = -E + 2 * E * i / n
            z = roof_z(xx, yy, E, top, z_eave, rise, upturn)
            rt.append(Vector((xx, yy, z)))
            rb.append(Vector((xx, yy, z - th)))
            ru.append((xx, yy))
        rows_t.append(rt)
        rows_b.append(rb)
        uvs.append(ru)
    b.add(bm_shell(rows_t, rows_b, uvs, smooth=True), "hc_rooftile", uv="keep", crease=30)
    for k in range(4):  # round tile rolls down each face (built on +X, turned to the others)
        M = R(90 * k, "Z")
        y0 = -E + per / 2
        while y0 < E - per / 2 + 1e-6:
            xs0 = max(abs(y0) + 0.03, top)
            if E - xs0 > 0.12:
                pts = [Vector((xs0 + (E - xs0) * i / 6, y0, 0.0)) for i in range(7)]
                for p in pts:
                    p.z = roof_z(p.x, p.y, E, top, z_eave, rise, upturn) + 0.032
                b.add(bm_tube(pts, 0.036, seg=6), "hc_rooftile", M=M, uv="keep")
            y0 += per
        # the hip ridge and its curled tip
        pts = [Vector((top + (E - top) * i / 8, top + (E - top) * i / 8, 0.0)) for i in range(9)]
        for p in pts:
            p.z = roof_z(p.x, p.y, E, top, z_eave, rise, upturn) + 0.06
        b.add(bm_tube(pts, 0.07, seg=8), "hc_rooftile", M=M, uv="keep")
        c = pts[-1]
        tip = [c, c + Vector((0.08, 0.08, 0.1)), c + Vector((0.1, 0.1, 0.26))]
        b.add(bm_tube(tip, [0.07, 0.055, 0.025], seg=8), "hc_rooftile", M=M, uv="keep")
        # rafter tails under the overhang of the -Y face (turned to the others)
        xr = -half + 0.25
        while xr < half - 0.2:
            ztop = roof_z(xr, -(E - 0.2), E, top, z_eave, rise, upturn) - th - 0.01
            b.add(bm_box(xr - 0.05, -(E - 0.18), ztop - 0.11, xr + 0.05, -(half + 0.1), ztop), "hc_timber", M=M,
                  grain="y")
            xr += 0.45


def tier_body(b, half, z0, z1, nwin, round_face=False):
    """Plaster walls in a timber frame, a cornice on top; nwin lattice paper windows on each face, or one round
    glowing face per side (round_face)."""
    b.add(bm_box(-half + 0.06, -half + 0.06, z0, half - 0.06, half - 0.06, z1), "hb_plaster")
    for sx in (-1, 1):
        for sy in (-1, 1):
            b.add(bm_box(sx * (half - 0.1) - 0.13, sy * (half - 0.1) - 0.13, z0, sx * (half - 0.1) + 0.13,
                         sy * (half - 0.1) + 0.13, z1), "hc_timber")
    b.add(bm_box(-half - 0.2, -half - 0.2, z1, half + 0.2, half + 0.2, z1 + 0.25, bevel=0.02), "hc_timber")
    for k in range(4):
        M = R(90 * k, "Z")
        for za, zb in ((z0, z0 + 0.22), (z1 - 0.26, z1)):  # sill and head beams
            b.add(bm_box(-half, -half - 0.02, za, half, -half + 0.1, zb), "hc_timber", M=M, grain="x")
        if round_face:
            zc = 0.5 * (z0 + z1)
            Mr = M @ T(0, -half + 0.06, zc) @ R(90, "X")  # local +Z out of the -Y face, from the plaster's face
            b.add(bm_lathe([(0.0, 0.0), (0.75, 0.0), (0.75, 0.1), (0.0, 0.1)], 32), "tower_paper", M=Mr, uv="keep")
            b.add(bm_torus(0.79, 0.055, 32, 8), "hb_bronze", M=Mr @ T(0, 0, 0.1), uv="keep")
            b.add(bm_lathe([(0.0, 0.1), (0.09, 0.1), (0.07, 0.16), (0.0, 0.18)], 16), "hb_bronze", M=Mr, uv="keep")
            for t in range(12):  # plain bronze ticks round the face: no numerals, no hands
                L = 0.16 if t % 3 == 0 else 0.09
                b.add(bm_box(-0.025, 0.66 - L, 0.1, 0.025, 0.66, 0.13), "hb_bronze", M=Mr @ R(30 * t, "Z"))
            continue
        usable = 2 * (half - 0.35)
        W = min(0.95, usable / nwin - 0.3)
        wz0 = z0 + 0.3 * (z1 - z0)
        wz1 = z1 - 0.2 * (z1 - z0)
        for i in range(nwin):
            xc = -usable / 2 + usable * (i + 0.5) / nwin
            x0, x1 = xc - W / 2, xc + W / 2
            y = -half + 0.06
            b.add(bm_box(x0, y - 0.03, wz0, x1, y + 0.01, wz1), "tower_paper", M=M)
            for xb in (x0 + W * (q + 1) / 4 for q in range(3)):  # lattice
                b.add(bm_box(xb - 0.018, y - 0.06, wz0, xb + 0.018, y - 0.03, wz1), "hc_timber", M=M)
            for q in range(4):
                zb = wz0 + (wz1 - wz0) * (q + 1) / 5
                b.add(bm_box(x0, y - 0.06, zb - 0.018, x1, y - 0.03, zb + 0.018), "hc_timber", M=M, grain="x")
            for (a0, a1, c0, c1) in ((x0 - 0.07, x1 + 0.07, wz1, wz1 + 0.07), (x0 - 0.07, x1 + 0.07, wz0 - 0.07, wz0),
                                     (x0 - 0.07, x0, wz0, wz1), (x1, x1 + 0.07, wz0, wz1)):
                b.add(bm_box(a0, y - 0.09, c0, a1, y - 0.0, c1), "hc_timber", M=M)


def far_tower(name):
    b = Builder(name)
    # stone plinth with a cap course and a lacquered door on -Y
    b.add(bm_box(-3.5, -3.5, -0.4, 3.5, 3.5, 2.2, bevel=0.04, seg=2), "hc_stone")
    b.add(bm_box(-3.65, -3.65, 2.2, 3.65, 3.65, 2.45, bevel=0.03, seg=2), "hc_stone")
    for x0, x1, z0, z1 in ((-0.85, -0.62, 0.0, 2.05), (0.62, 0.85, 0.0, 2.05), (-0.85, 0.85, 1.85, 2.05)):
        b.add(bm_box(x0, -3.62, z0, x1, -3.48, z1, bevel=0.01), "hc_stone")  # door surround
    b.add(bm_box(-0.62, -3.52, 0.0, 0.62, -3.48, 1.85), "hc_lacquer")
    b.add(bm_box(0.4, -3.56, 0.85, 0.47, -3.52, 1.0), "hc_brass")
    # four tiers: (half, z0, z1, eave overhang, roof rise, windows a face)
    tiers = [(2.7, 2.45, 6.40, 1.30, 1.50, 3), (2.3, 6.85, 11.05, 1.15, 1.35, 3), (1.9, 11.5, 15.25, 1.00, 1.20, 2),
             (1.5, 15.7, 19.20, 0.95, 2.40, 0)]
    th = 0.09
    peak = 0.0
    for k, (half, z0, z1, over, rise, nwin) in enumerate(tiers):
        tier_body(b, half, z0, z1, nwin, round_face=(nwin == 0))
        top = tiers[k + 1][0] - 0.1 if k + 1 < len(tiers) else 0.12
        Eh = half + 0.2  # the roof's underside rests on the cornice; the eave hangs lower, the next tier rises out of it
        tw = over / (Eh + over - top)
        z_eave = z1 + 0.25 + th - rise * tw ** 1.5
        hip_roof(b, Eh, over, z_eave, rise, top, upturn=0.32 if k < 3 else 0.25, th=th)
        peak = z_eave + rise
    # the finial: rings and a spire in bronze
    zf = peak - 0.05
    prof = [(0.0, zf), (0.22, zf), (0.22, zf + 0.12), (0.12, zf + 0.2), (0.16, zf + 0.42), (0.1, zf + 0.6)]
    for r in range(5):
        z = zf + 0.6 + 0.3 * r
        prof += [(0.07, z), (0.17 - 0.015 * r, z + 0.06), (0.17 - 0.015 * r, z + 0.14), (0.07, z + 0.2)]
    z = prof[-1][1]
    prof += [(0.05, z + 0.25), (0.11, z + 0.42), (0.05, z + 0.6), (0.018, z + 1.0), (0.0, z + 1.05)]
    b.add(bm_lathe(prof, 20), "hb_bronze", uv="keep")
    print("far tower tris", b.tris())
    return b.finish()


# name: (builder, triangle budget, textured)
PARTS = {
    "SM_HB_WorkBoat": (work_boat, 30000, True),
    "SM_HB_CabinBoat": (cabin_boat, 30000, True),
    "SM_HB_Skiff": (skiff, 30000, True),
    "SM_HB_FarTower": (far_tower, 140000, True),
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
    jpath = os.path.join(out, "harbour_props.json")
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
        hc.export(ob, path, textured)
        r = hc.read_back(path, allow_images=textured)
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
