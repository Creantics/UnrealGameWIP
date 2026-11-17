"""Generate the solhaven_granite_sett surface: grey granite setts in staggered, wandering courses, tileable.

The hero corner's lane paving (our own generated route; a scanned set may replace the files later under its own id).
v6 (default --layout courses): one tile is 400 cm square at 4096 px (0.98 mm a texel, about a screen pixel at 1 m), so
the same stone comes round only every 4 m (the material can stochastic-sample on top of that; this texture does not
need it to hold at 1 m). About 40 courses of 8.5, 10 and 12 cm (jittered); each course draws its setts from three or
four lengths of 9-18 cm (jittered), and each course is laid again (up to 40 times) until its perpends clear the next
course's by 2.5 cm (the report's stagger_min_cm). Bed joints change width along the course and perpends from joint to
joint (8-17 mm nominal, about 7-19 mm measured with the split edges), and the courses wander a few mm (a periodic
warp) while the perpends lean up to 4 degrees, so no joint runs ruler straight. Each stone carries one hashed random value that sets its tone (+-12%), its warm
or cool drift (+-4%), its roughness (+-0.12), its height (+-3 mm) and a tilt of up to 2 degrees; 1 in 12 is a replaced
stone, clearly darker or lighter, and 1 in 40 is cracked or broken at a corner. Dark grime gathers in the joints, on the
arrises and in the breaks (worn tracks, puddles and wetness are the material's, Core SurfaceWeather).
SurfaceJoints' entry {"granite_sett", depth 1.3 cm, no unit grid} reads it: joints are 1.3 cm under a stone's nominal
top, filled low with grit, and the AO channel keeps the joint-mask contract (tops >= 0.92, joints <= 0.6; breaks in a
stone 0.8-0.92, read as a shallow recess). --layout grid gives the old stack bond (pass --tile-cm 120 for the v4 size;
--units-u/--units-v/--bond running the 20 x 10 cm running bond); the joint table must then match.

Writes <out>/<id>_diff_<tag>.jpg (sRGB albedo), <id>_nor_dx_<tag>.jpg (DirectX normal from the true height),
<id>_arm_<tag>.jpg (R ambient occlusion and joint mask; G roughness; B metal 0, read as Metallic: never spare),
<id>_height_<tag>.png (16-bit, 0 the joint floor, white HEIGHT_MAX_CM), <id>_stone_<tag>.png (RGBA, the per-stone map:
R the stone's random value 1-255, G its height offset 1-255 over -3..+3 mm, B the stone-top mask, A 85 a plain stone,
170 a replaced one, 255 a cracked or chipped one; 0 everywhere in the joints. Not imported: FTBuildSurfaces reads diff,
nor_dx and arm only), <id>_tile2x2.jpg, lit previews (<id>_preview_1m.png at native res, _preview_tile.png, the 12 m
_preview_3x3.jpg) and <id>_report.json. The surfaces.json row must carry the same tileCm and res (400, "4k").

    python -I tools/make_granite_sett.py <out_dir> [--res 4096] [--tile-cm 400] [--seed 7]
"""

import argparse
import json
import math
import os

import numpy as np
from PIL import Image

ID = "solhaven_granite_sett"
DEPTH_CM = 1.3                    # joint floor under a sett's nominal top (SurfaceJoints' granite_sett depth matches)
HEIGHT_MAX_CM = 2.4               # the height PNG's white
COURSE_CM = (8.5, 10.0, 12.0)     # course heights, mixed per tile
SETT_POOL_CM = (9.0, 11.0, 13.0, 15.0, 18.0)   # each course draws three or four of these
STAGGER_CM = 2.5                  # no perpend this close to one in the next course
JOINT_CM = (0.8, 1.7)             # nominal joint width range (the split edges take +-1.5 mm a side)
GRID_JOINT_CM = 1.2               # --layout grid's joint
GRIT = np.array((0.035, 0.032, 0.029), np.float32)   # the joints' fill, linear albedo: near-black grit ...
SILT = np.array((0.085, 0.077, 0.064), np.float32)   # ... and a dusty silt in places
JOINT_AO = 0.42                   # the joints' AO ceiling
TOP_AO = 0.93                     # the stone tops' AO floor
BREAK_AO = 0.80                   # a crack's or chip's floor AO (a shallow recess to the joint parallax)
VALUE_VAR = 0.12                  # per stone: tone +-12%
HUE_VAR = 0.04                    # warm/cool drift +-4%
ROUGH_BASE, ROUGH_VAR = 0.68, 0.12
HEIGHT_VAR_CM = 0.3               # +-3 mm
TILT_MAX_DEG = 2.0
REPLACED_SHARE = 1.0 / 12.0
DAMAGED_SHARE = 1.0 / 40.0
F32 = np.float32


class Fields:
    """Periodic Gaussian-filtered noise and blurs on the n x n tile, float32 throughout (rfft2: half the spectrum)."""

    def __init__(self, n, px_cm, rng):
        self.n, self.px_cm, self.rng = n, px_cm, rng
        f = np.fft.fftfreq(n).astype(F32)
        fr = np.fft.rfftfreq(n).astype(F32)
        self.fy2 = (f * f)[:, None]
        self.fx2 = (fr * fr)[None, :]

    def _kernel(self, sx_cm, sy_cm):
        sx, sy = sx_cm / self.px_cm, sy_cm / self.px_cm
        return np.exp(F32(-2.0 * math.pi ** 2) * (F32(sx * sx) * self.fx2 + F32(sy * sy) * self.fy2)).astype(F32)

    def filt(self, a, sx_cm, sy_cm=None):
        spec = np.fft.rfft2(a.astype(F32, copy=False))
        spec *= self._kernel(sx_cm, sx_cm if sy_cm is None else sy_cm)
        return np.fft.irfft2(spec, s=a.shape).astype(F32)

    def noise(self, sx_cm, sy_cm=None):
        out = self.filt(self.rng.standard_normal((self.n, self.n), dtype=F32), sx_cm, sy_cm)
        out /= F32(out.std() + 1e-9)
        return out

    def noise1d(self, rows, sigma_cm):
        """rows periodic noises along x, unit deviation each."""
        white = self.rng.standard_normal((rows, self.n), dtype=F32)
        spec = np.fft.rfft(white, axis=1)
        s = sigma_cm / self.px_cm
        spec *= np.exp(-2.0 * math.pi ** 2 * s * s * np.fft.rfftfreq(self.n) ** 2).astype(F32)[None, :]
        out = np.fft.irfft(spec, n=self.n, axis=1).astype(F32)
        out /= out.std(axis=1, keepdims=True) + F32(1e-9)
        return out


def shash(ids, salt, seed):
    """One uniform value in [0, 1) per stone id and salt (splitmix64), so every trait is reproducible from the id."""
    with np.errstate(over="ignore"):
        x = (np.asarray(ids, np.uint64) * np.uint64(0x9E3779B97F4A7C15) + np.uint64(seed * 7919 + 1)
             + np.uint64(salt) * np.uint64(0xD1B54A32D192ED03))
        x ^= x >> np.uint64(30)
        x *= np.uint64(0xBF58476D1CE4E5B9)
        x ^= x >> np.uint64(27)
        x *= np.uint64(0x94D049BB133111EB)
        x ^= x >> np.uint64(31)
    return ((x >> np.uint64(11)).astype(np.float64) / float(1 << 53)).astype(F32)


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def circ_dist(a, b, period):
    d = np.abs(np.asarray(a)[:, None] - np.asarray(b)[None, :]) % period
    return np.minimum(d, period - d)


def lay_courses(rng, tile):
    """Course heights and, per course, its start shift and stone lengths, every perpend staggered from its neighbours'."""
    nc = max(3, int(round(tile / np.mean(COURSE_CM))))
    heights = rng.permutation(np.resize(np.array(COURSE_CM), nc)) + rng.uniform(-0.4, 0.4, nc)
    heights *= tile / heights.sum()
    lo, hi = min(SETT_POOL_CM), max(SETT_POOL_CM)
    courses = []          # (shift, lengths)
    perps = []            # each course's perpends, tile x
    for r in range(nc):
        near = []
        if r > 0:
            near.append(perps[r - 1])
        if r == nc - 1 and nc > 1:
            near.append(perps[0])                    # the tile wraps: the last course meets the first
        near = np.concatenate(near) if near else np.zeros(0)

        def clear(x):
            return near.size == 0 or circ_dist([x % tile], near, tile).min() >= STAGGER_CM

        best = None
        for _attempt in range(40):                   # lay the course again (new start, new lengths) until it clears
            shift = rng.uniform(0.0, tile)
            for _ in range(64):
                if clear(shift):
                    break
                shift = rng.uniform(0.0, tile)
            kinds = rng.choice(SETT_POOL_CM, size=int(rng.integers(3, 5)), replace=False)
            lens, pos = [], 0.0
            while True:
                rem = tile - pos
                if rem <= 1.2 * hi and rem >= 0.8 * lo:
                    lens.append(rem)
                    break
                cands = [float(k + rng.uniform(-0.6, 0.6)) for _ in range(4) for k in rng.permutation(kinds)]
                cands = [c for c in cands if rem - c >= 0.8 * lo]
                if not cands:                        # cannot happen with this pool; close the course anyway
                    lens.append(rem)
                    break
                pick = next((c for c in cands if clear(shift + pos + c)), None)
                if pick is None:
                    pick = max(cands, key=lambda c: circ_dist([(shift + pos + c) % tile], near, tile).min())
                lens.append(pick)
                pos += pick
            lens = np.array(lens)
            pp = (shift + np.concatenate([[0.0], np.cumsum(lens)[:-1]])) % tile
            gap_min = circ_dist(pp, near, tile).min() if near.size else tile
            if best is None or gap_min > best[0]:
                best = (gap_min, shift, lens, pp)
            if gap_min >= STAGGER_CM:
                break
        courses.append((best[1], best[2]))
        perps.append(best[3])
    stag = [circ_dist(perps[r], perps[(r + 1) % nc], tile).min() for r in range(nc)]
    return heights, courses, np.array(stag)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--res", type=int, default=4096)
    ap.add_argument("--tile-cm", type=float, default=None, help="400 for courses, 120 for grid when left out")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--units-u", type=int, default=12)
    ap.add_argument("--units-v", type=int, default=12)
    ap.add_argument("--bond", choices=("stack", "running"), default="stack")
    ap.add_argument("--layout", choices=("courses", "grid"), default="courses")
    ap.add_argument("--no-previews", action="store_true")
    args = ap.parse_args()
    courses_layout = args.layout == "courses"
    tile = args.tile_cm if args.tile_cm else (400.0 if courses_layout else 120.0)
    n = args.res
    seed = args.seed
    rng = np.random.default_rng(seed)
    px_cm = tile / n
    fl = Fields(n, px_cm, rng)
    xs = ((np.arange(n) + 0.5) * px_cm).astype(F32)

    # ---- Layout: per pixel the stone id, its local coordinates (cm from the cell's centre) and four joint insets.
    if courses_layout:
        heights, courses, stagger = lay_courses(rng, tile)
        nc = len(heights)
        y_edges = np.concatenate([[0.0], np.cumsum(heights)])
        cy = 0.5 * (y_edges[:-1] + y_edges[1:])
        starts, lens_all, stone_row, nxt = [], [], [], []
        for r, (shift, lens) in enumerate(courses):
            base = len(lens_all)
            e = np.concatenate([[0.0], np.cumsum(lens)[:-1]])
            starts += list(r * tile + e)
            lens_all += list(lens)
            stone_row += [r] * len(lens)
            nxt += [base + (k + 1) % len(lens) for k in range(len(lens))]
        starts = np.array(starts)
        L_s = np.array(lens_all, F32)
        row_s = np.array(stone_row)
        nxt = np.array(nxt)
        stones = len(L_s)
        H_s = heights[row_s].astype(F32)
        shifts = np.array([c[0] for c in courses])

        # Courses wander a few mm (long along the course), perpends lean a little: one periodic warp each way.
        wy = 0.20 * fl.noise(30.0, 12.0) + 0.12 * fl.noise(90.0, 40.0)
        np.clip(wy, -0.5, 0.5, out=wy)
        Y = np.mod(xs[:, None] + wy, F32(tile))
        del wy
        wx = np.clip(0.09 * fl.noise(5.0, 5.0), -0.25, 0.25)
        X = np.mod(xs[None, :] + wx, F32(tile))
        del wx
        row = np.clip(np.searchsorted(y_edges, Y, side="right") - 1, 0, nc - 1).astype(np.int32)
        ly = (Y - cy[row].astype(F32)).astype(F32)
        del Y
        xx = np.mod(X - shifts[row].astype(F32), F32(tile))
        del X
        g = row.astype(np.float64) * tile + xx
        sid = np.clip(np.searchsorted(starts, g, side="right") - 1, 0, stones - 1).astype(np.int32)
        del g
        left = (starts - row_s * tile).astype(F32)
        lx = (xx - (left + 0.5 * L_s)[sid]).astype(F32)
        del xx

        # Perpends: one width per joint (each stone owns the joint at its start), split unevenly between the two setts.
        wj = JOINT_CM[0] + (JOINT_CM[1] - JOINT_CM[0]) * shash(np.arange(stones), 1, seed) ** 2.2  # median ~9.6 mm
        aj = 0.5 + 0.3 * (shash(np.arange(stones), 2, seed) - 0.5)
        insL_s = (wj * (1.0 - aj)).astype(F32)
        insR_s = (wj[nxt] * aj[nxt]).astype(F32)
        # Each perpend leans up to 4 degrees, both setts beside it alike, so its width holds and none runs square.
        lean_s = (0.07 * (2.0 * shash(np.arange(stones), 3, seed) - 1.0)).astype(F32)
        insL = insL_s[sid] + lean_s[sid] * ly
        insR = insR_s[sid] - lean_s[nxt][sid] * ly
        # Bed joints: width and split change along the course (a periodic noise along x per course boundary).
        bw = np.clip(0.98 + 0.2 * fl.noise1d(nc, 18.0) + 0.08 * fl.noise1d(nc, 3.0), *JOINT_CM).astype(F32)
        ba = (0.5 + 0.1 * fl.noise1d(nc, 25.0)).astype(F32)
        col = np.arange(n, dtype=np.int32)[None, :]
        insT = bw[row, col] * (1.0 - ba[row, col])
        rb = (row + 1) % nc
        insB = bw[rb, col] * ba[rb, col]
        del rb, col
        Lp = L_s[sid]
        Hp = heights[row].astype(F32)
        del row
        layout_desc = ("staggered wandering courses %s cm (jittered), setts of 3-4 lengths from %s cm (jittered)"
                       % (list(COURSE_CM), list(SETT_POOL_CM)))
    else:
        UU, UV = args.units_u, args.units_v
        running = args.bond == "running"
        c = ((np.arange(n) + 0.5) / n).astype(F32)
        u, v = np.meshgrid(c, c)
        row = np.floor(v * UV).astype(np.int32)
        cu = u * UU - ((0.5 * (row % 2)).astype(F32) if running else F32(0.0))
        colg = np.floor(cu).astype(np.int32)
        sid = ((row % UV) * UU + (colg % UU)).astype(np.int32)
        su, sv = tile / UU, tile / UV
        lx = ((cu - colg - 0.5) * su).astype(F32)
        ly = ((v * UV - row - 0.5) * sv).astype(F32)
        del u, v, cu, colg, row
        stones = UU * UV
        L_s = np.full(stones, su, F32)
        H_s = np.full(stones, sv, F32)
        ids = np.arange(stones)
        ins_s = [(0.3 * GRID_JOINT_CM + 0.65 * (shash(ids, 40 + k, seed) - 0.15)).astype(F32) for k in range(4)]
        insL, insR, insT, insB = (a[sid] for a in ins_s)
        Lp, Hp = L_s[sid], H_s[sid]
        layout_desc = "running, half" if running else "stack"

    # ---- Per-stone traits, all from the stone id's hash.
    ids = np.arange(stones)
    u0 = shash(ids, 0, seed)                                         # the stone's own random value (the stone map's R)
    replaced = shash(ids, 7, seed) < REPLACED_SHARE
    damaged = shash(ids, 8, seed) < DAMAGED_SHARE
    crack = damaged & (shash(ids, 9, seed) < 0.55)                   # else a broken corner
    lighter = shash(ids, 10, seed) < 0.3
    value = 1.0 + VALUE_VAR * (2.0 * shash(ids, 11, seed) - 1.0)
    value = np.where(replaced, np.where(lighter, 1.2, 0.7) * (1.0 + 0.04 * (2.0 * u0 - 1.0)), value).astype(F32)
    hue = (HUE_VAR * (2.0 * shash(ids, 12, seed) - 1.0)
           + np.where(replaced, 0.05 * np.sign(shash(ids, 13, seed) - 0.5), 0.0)).astype(F32)
    rough_s = (ROUGH_BASE + ROUGH_VAR * (2.0 * shash(ids, 14, seed) - 1.0)).astype(F32)
    off_s = (HEIGHT_VAR_CM * (2.0 * shash(ids, 15, seed) - 1.0)).astype(F32)
    tilt_deg = TILT_MAX_DEG * np.sqrt(shash(ids, 16, seed))
    tilt_dir = 2.0 * math.pi * shash(ids, 17, seed)
    slope_x = (np.tan(np.radians(tilt_deg)) * np.cos(tilt_dir)).astype(F32)
    slope_y = (np.tan(np.radians(tilt_deg)) * np.sin(tilt_dir)).astype(F32)
    dome_s = (0.04 + 0.12 * shash(ids, 18, seed)).astype(F32)
    radius_s = (0.6 + 1.2 * shash(ids, 19, seed)).astype(F32)
    bevel_s = (0.35 + 0.45 * shash(ids, 20, seed)).astype(F32)
    grime_s = (0.7 + 0.6 * shash(ids, 21, seed)) * np.where(replaced, 0.6, 1.0)

    # ---- The outline: a rounded rectangle inside the four insets, split-stone edges at two scales (held to +-1.5 mm so
    # a 6 mm joint stays open).
    dl = (lx + 0.5 * Lp) - insL
    dr = (0.5 * Lp - lx) - insR
    dt = (ly + 0.5 * Hp) - insT
    db = (0.5 * Hp - ly) - insB
    ox = 0.5 * (insL - insR)
    oy = 0.5 * (insT - insB)
    hx = np.maximum(0.5 * (dl + dr), 0.1)     # the outline's half sizes here
    hy = np.maximum(0.5 * (dt + db), 0.1)
    del insL, insR, insT, insB, Lp, Hp
    ex = np.minimum(dl, dr)
    ey = np.minimum(dt, db)
    del dl, dr, dt, db
    r = np.minimum(radius_s[sid], 0.9 * np.minimum(hx, hy))
    qx = r - ex
    qy = r - ey
    del ex, ey
    d = np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r
    del qx, qy, r
    en = np.clip(0.05 * fl.noise(5.0) + 0.07 * fl.noise(2.0) + 0.04 * fl.noise(0.35), -0.15, 0.15)
    inside = (-d + en).astype(F32)                 # cm inside the sett's outline
    del d
    stone = inside > 0

    # ---- Height, cm over the joint floor.
    s = np.clip(inside / bevel_s[sid], 0.0, 1.0)
    profile = (1.0 - (1.0 - s) ** 2.4).astype(F32)
    del s
    nx = (lx - ox) / hx
    ny = (ly - oy) / hy
    top = (DEPTH_CM + off_s[sid] + slope_x[sid] * (lx - ox) + slope_y[sid] * (ly - oy)
           + dome_s[sid] * (1.0 - np.clip(nx * nx, 0, 1)) * (1.0 - np.clip(ny * ny, 0, 1)))
    del nx, ny
    top += 0.03 * fl.noise(1.2) + 0.018 * fl.noise(0.4) + 0.009 * fl.noise(0.12)
    stone_h = np.where(stone, profile * top, 0.0).astype(F32)
    del top

    # Wear chips off the arrises: up to three per sett, a scoop into the edge.
    hx_s = 0.5 * L_s
    hy_s = 0.5 * H_s
    for k in range(3):
        has = shash(ids, 50 + k, seed) < 0.35
        side = (4 * shash(ids, 60 + k, seed)).astype(np.int32)
        along = 1.6 * shash(ids, 70 + k, seed) - 0.8
        cr = 0.5 + 0.9 * shash(ids, 80 + k, seed)
        cdep = 0.3 + 0.4 * shash(ids, 90 + k, seed)
        ccx = np.where(side == 0, -hx_s, np.where(side == 1, hx_s, along * hx_s)).astype(F32)
        ccy = np.where(side == 2, -hy_s, np.where(side == 3, hy_s, along * hy_s)).astype(F32)
        dist = np.hypot(lx - ccx[sid], ly - ccy[sid]) / cr[sid].astype(F32)
        cut = np.where(has[sid] & stone, cdep[sid].astype(F32) * np.clip(1.0 - dist, 0.0, 1.0) ** 0.7, 0.0)
        stone_h = np.maximum(stone_h - cut, 0.0).astype(F32)
        del dist, cut

    # Damage, 1 stone in 40: a crack across it (a jagged V groove with a small step) or a broken corner.
    brk = np.zeros((n, n), F32)
    pix = np.flatnonzero(damaged[sid] & stone)
    if pix.size:
        ps = sid.ravel()[pix]
        plx = lx.ravel()[pix] - ox.ravel()[pix]
        ply = ly.ravel()[pix] - oy.ravel()[pix]
        pen = en.ravel()[pix]
        th = math.pi * shash(ps, 30, seed)
        cxp = 0.3 * (2.0 * shash(ps, 31, seed) - 1.0) * hx_s[ps]
        cyp = 0.3 * (2.0 * shash(ps, 32, seed) - 1.0) * hy_s[ps]
        sd = (plx - cxp) * np.sin(th) - (ply - cyp) * np.cos(th) + 0.6 * pen
        w = 0.05 + 0.05 * shash(ps, 33, seed)
        dep = 0.35 + 0.3 * shash(ps, 34, seed)
        step = (0.03 + 0.05 * shash(ps, 35, seed)) * (sd > 0)
        crack_cut = dep * np.clip(1.0 - np.abs(sd) / w, 0.0, 1.0) ** 0.6 + step
        corner = (4 * shash(ps, 36, seed)).astype(np.int32)
        kx = np.where(corner % 2 == 0, -1.0, 1.0) * hx_s[ps]
        ky = np.where(corner < 2, -1.0, 1.0) * hy_s[ps]
        cr = 2.5 + 2.0 * shash(ps, 37, seed)
        cd = 0.4 + 0.4 * shash(ps, 38, seed)
        dist = (np.hypot(plx - kx, ply - ky) + 0.8 * pen) / cr
        floor_n = 1.0 + 0.25 * pen / 0.2
        chip_cut = cd * floor_n * smoothstep(1.0, 0.8, dist)
        brk.reshape(-1)[pix] = np.where(crack[ps], crack_cut, chip_cut).astype(F32)
        del ps, plx, ply, pen, sd, crack_cut, chip_cut, dist
    del pix
    stone_h = np.maximum(stone_h - brk * np.minimum(profile * 4.0, 1.0), 0.0).astype(F32)

    # The joints' grit fill: a little under half way up in places, near the floor in others.
    fill = np.clip(0.11 + 0.08 * fl.noise(1.5) + 0.04 * fl.noise(0.1), 0.03, 0.4).astype(F32)
    height = np.maximum(stone_h, fill)
    height = fl.filt(height, 0.6 * px_cm)
    in_joint = stone_h <= fill + 0.03
    del fill

    # ---- AO from the edge profile (the joint mask: a sunk stone stays a top) and the cavity; breaks a shallow recess.
    cavity = np.clip(fl.filt(height, 0.8) - height, 0.0, None) / DEPTH_CM
    ao = 0.40 + 0.58 * profile ** 0.7 - 1.2 * cavity
    np.clip(ao, 0.25, 1.0, out=ao)
    top_mask = stone & (profile >= 0.97)
    ao = np.where(top_mask, np.maximum(ao, TOP_AO), ao)
    brk_n = np.clip(brk / 0.6, 0.0, 1.0)
    ao = np.where(top_mask & (brk > 0.02), np.minimum(ao, 0.92 - (0.92 - BREAK_AO) * brk_n), ao)
    ao = np.where(in_joint, np.minimum(ao, JOINT_AO), ao).astype(F32)

    # ---- Albedo: grey granite (black mica, white feldspar), each stone's tone and drift, grime in toward the joints.
    grain_a = fl.noise(0.09)
    grain_b = fl.noise(0.07)
    grain_c = fl.noise(0.25)
    lum = (0.27 * value[sid] * (1.0 + 0.04 * fl.noise(15.0) + 0.025 * fl.noise(3.0)) + 0.035 * grain_c).astype(F32)
    lum = np.where(grain_a > 1.35, lum * 0.35, lum)                               # mica
    lum = np.where((grain_b > 1.75) & (grain_a < 1.0), lum * 1.35 + 0.08, lum)    # feldspar
    np.clip(lum, 0.04, 0.92, out=lum)
    hp = hue[sid]
    rgb = np.stack([lum * (1.0 + hp + 0.01), lum * (1.0 + 0.3 * hp), lum * (1.0 - hp + 0.015)], axis=-1).astype(F32)
    del lum, hp
    # Grime: a dark band in from the outline (its width wandering), the hollows of the top, and the breaks.
    gn = np.clip(1.0 + 0.45 * fl.noise(1.0), 0.2, 1.8)
    band = (1.0 - smoothstep(0.0, 0.6 + 0.6 * gn, inside)) * stone
    hollow = np.clip(cavity * 6.0, 0.0, 1.0)
    grime = np.clip((0.32 * band + 0.25 * hollow) * grime_s[sid] * gn + 0.7 * brk_n, 0.0, 0.85).astype(F32)
    del gn, band, hollow
    dirt = np.array((0.045, 0.040, 0.034), F32)
    rgb = rgb * (1.0 - grime[..., None]) + dirt * grime[..., None]
    sn = smoothstep(0.4, 1.6, fl.noise(4.0))
    fill_rgb = (GRIT * (1.0 - sn[..., None]) + SILT * sn[..., None]) * (1.0 + 0.25 * fl.noise(0.08))[..., None]
    rgb = np.where(in_joint[..., None], fill_rgb, rgb)
    del fill_rgb, sn
    np.clip(rgb, 0.02, 0.95, out=rgb)
    srgb = np.where(rgb <= 0.0031308, rgb * 12.92, 1.055 * np.power(rgb, 1 / 2.4) - 0.055).astype(F32)

    # ---- Roughness: each stone's own honed-but-weathered top, rough arrises, grime and grit.
    rgh = rough_s[sid] + 0.04 * grain_c - 0.03 * (top_mask & (grain_a > 1.55))
    del grain_a, grain_b, grain_c
    rgh = np.where(stone & ~top_mask, np.maximum(rgh, 0.8), rgh)
    rgh = rgh + 0.12 * grime
    rgh = np.where(in_joint, 0.92, rgh)
    rgh = np.clip(fl.filt(rgh.astype(F32), 0.5 * px_cm), 0.4, 0.95).astype(F32)

    # ---- DirectX normal from the true height (cm).
    dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) / F32(2.0 * px_cm)
    dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) / F32(2.0 * px_cm)
    nrm = np.stack([-dx, -dy, np.ones_like(dx)], axis=-1)
    del dx, dy
    nrm /= np.linalg.norm(nrm, axis=-1, keepdims=True)

    # ---- Write.
    os.makedirs(args.out, exist_ok=True)
    tag = {1024: "1k", 2048: "2k", 4096: "4k"}.get(n, f"{n}px")

    def u8(a):
        return np.clip(a * 255.0 + 0.5, 0, 255).astype(np.uint8)

    def p(name):
        return os.path.join(args.out, f"{ID}_{name}")

    Image.fromarray(u8(srgb)).save(p(f"diff_{tag}.jpg"), quality=95, subsampling=0)
    Image.fromarray(u8(nrm * 0.5 + 0.5)).save(p(f"nor_dx_{tag}.jpg"), quality=95, subsampling=0)
    Image.fromarray(u8(np.stack([ao, rgh, np.zeros_like(ao)], axis=-1))).save(p(f"arm_{tag}.jpg"), quality=95, subsampling=0)
    Image.fromarray(np.clip(height / HEIGHT_MAX_CM * 65535.0, 0, 65535).astype(np.uint16)).save(p(f"height_{tag}.png"))
    flags = np.where(damaged, 255, np.where(replaced, 170, 85)).astype(np.uint8)
    stone_map = np.stack([
        np.where(stone, 1 + np.round(254 * u0[sid]), 0),
        np.where(stone, 1 + np.round(254 * (off_s[sid] + HEIGHT_VAR_CM) / (2 * HEIGHT_VAR_CM)), 0),
        np.where(stone, np.round(255 * profile), 0),
        np.where(stone, flags[sid], 0)], axis=-1).astype(np.uint8)
    Image.fromarray(stone_map, "RGBA").save(p(f"stone_{tag}.png"))
    del stone_map
    step2 = max(1, n // 1024)
    Image.fromarray(u8(np.tile(srgb[::step2, ::step2], (2, 2, 1)))).save(p("tile2x2.jpg"), quality=90)

    # ---- Measured.
    gap = ~stone

    def runs(mask_lines, id_lines):
        """Gap runs along each line that part two different stones (a grazed corner or an edge pocket does not)."""
        out = []
        for line, idl in zip(mask_lines, id_lines):
            dline = np.diff(np.concatenate([[0], line.astype(np.int8), [0]]))
            st, en_ = np.flatnonzero(dline == 1), np.flatnonzero(dline == -1)
            ok = (st > 0) & (en_ < line.size)
            st, en_ = st[ok], en_[ok]
            ok = idl[st - 1] != idl[en_]
            out += list((en_[ok] - st[ok]) * px_cm)
        return np.array(out)

    every = max(1, n // 128)
    bed = runs(gap[:, ::every].T, sid[:, ::every].T)
    perp = runs(gap[::every, :], sid[::every, :])
    bed = bed[bed < 3.0]
    perp = perp[perp < 3.0]
    widths = np.concatenate([bed, perp])
    tm = top_mask
    st_ids = sid[tm]
    cnt = np.bincount(st_ids, minlength=stones)
    lin_lum = rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722
    mean_lum = np.bincount(st_ids, weights=lin_lum[tm], minlength=stones) / np.maximum(cnt, 1)
    mean_rgh = np.bincount(st_ids, weights=rgh[tm], minlength=stones) / np.maximum(cnt, 1)
    seen = cnt > 50
    plain = seen & ~replaced & ~damaged
    med = float(np.median(mean_lum[plain]))
    pct = lambda a, q: float(np.percentile(a, q)) if a.size else None
    report = {
        "id": ID,
        "version": "v6" if courses_layout else "grid",
        "source": "generated: tools/make_granite_sett.py",
        "seed": seed,
        "res": n,
        "tile_cm": tile,
        "repeat_m": tile / 100.0,
        "mm_per_texel": round(10.0 * px_cm, 3),
        "units": [0, 0] if courses_layout else [args.units_u, args.units_v],
        "bond": layout_desc,
        "courses": int(len(heights)) if courses_layout else args.units_v,
        "stagger_min_cm": round(float(stagger.min()), 2) if courses_layout else None,
        "stagger_under_2_5cm": int((stagger < STAGGER_CM).sum()) if courses_layout else None,
        "stones": int(stones),
        "replaced_share": float(replaced.mean()),
        "damaged_share": float(damaged.mean()),
        "cracked": int(crack.sum()),
        "broken_corners": int((damaged & ~crack).sum()),
        "joint_mm_p5_median_p95": [round(10 * pct(widths, q), 1) for q in (5, 50, 95)],
        "bed_joint_mm_median": round(10 * pct(bed, 50), 1),
        "perpend_mm_median": round(10 * pct(perp, 50), 1),
        "depth_cm": DEPTH_CM,
        "height_png_white_cm": HEIGHT_MAX_CM,
        "stone_offset_mm": [round(10 * float(off_s.min()), 2), round(10 * float(off_s.max()), 2)],
        "tilt_deg_max": round(float(tilt_deg.max()), 2),
        "stone_lum_vs_median_plain_p5_p95": [round(pct(mean_lum[plain], q) / med - 1.0, 3) for q in (5, 95)],
        "replaced_lum_vs_median": [round(float(x) / med - 1.0, 3) for x in
                                   (np.median(mean_lum[seen & replaced & lighter]),
                                    np.median(mean_lum[seen & replaced & ~lighter]))],
        "stone_rough_p5_p95": [round(pct(mean_rgh[plain], q), 3) for q in (5, 95)],
        "ao_top_min": float(ao[tm & (brk <= 0.02)].min()),
        "ao_joint_max": float(ao[in_joint].max()),
        "ao_break_min": float(ao[tm & (brk > 0.02)].min()) if (tm & (brk > 0.02)).any() else None,
        "stone_share": float(tm.mean()),
        "joint_share": float(in_joint.mean()),
        "roughness_top_mean": float(rgh[tm].mean()),
        "roughness_joint_mean": float(rgh[in_joint].mean()),
        "albedo_top_mean_linear": float(rgb[tm].mean()),
        "stone_map": "R stone value 1-255, G height offset 1-255 over -3..+3 mm, B top mask, A 85 plain / 170 replaced"
                     " / 255 damaged; 0 in joints",
        "metal": "none (ARM blue is read as Metallic: not a spare channel)",
        "license": "project",
    }
    with open(p("report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1)
    print(json.dumps(report))

    # ---- Lit previews: albedo x (sky x AO + a low sun on the normal), sRGB.
    if not args.no_previews:
        del lin_lum, gap
        el, az = math.radians(22.0), math.radians(-135.0)
        L = np.array((math.cos(el) * math.cos(az), math.cos(el) * math.sin(az), math.sin(el)), F32)
        # DirectX normal: +y is image-down; the light's y the same frame.
        ndl = np.clip(nrm[..., 0] * L[0] + nrm[..., 1] * L[1] + nrm[..., 2] * L[2], 0.0, None)
        lit = rgb * (0.35 * ao + 2.6 * ndl * np.clip(ao * 1.15, 0, 1))[..., None]
        del ndl
        lit = np.clip(lit, 0.0, 1.0)
        lit = np.where(lit <= 0.0031308, lit * 12.92, 1.055 * np.power(lit, 1 / 2.4) - 0.055).astype(F32)
        # 1 m crop at native res, centred on a damaged stone if there is one (else the tile's centre).
        m = int(round(100.0 / px_cm))
        cand = np.flatnonzero(damaged & (cnt > 50))
        if cand.size:
            yy, xq = np.nonzero(sid[::8, ::8] == cand[0])
            cy0, cx0 = int(yy.mean()) * 8, int(xq.mean()) * 8
        else:
            cy0 = cx0 = n // 2
        y0 = int(np.clip(cy0 - m // 2, 0, max(0, n - m)))
        x0 = int(np.clip(cx0 - m // 2, 0, max(0, n - m)))
        Image.fromarray(u8(lit[y0:y0 + m, x0:x0 + m])).save(p("preview_1m.png"))
        full = Image.fromarray(u8(lit))
        full.resize((min(n, 2048),) * 2, Image.LANCZOS).save(p("preview_tile.png"))
        sm = full.resize((683, 683), Image.LANCZOS)
        three = Image.new("RGB", (683 * 3, 683 * 3))
        for i in range(3):
            for j in range(3):
                three.paste(sm, (683 * i, 683 * j))
        three.save(p("preview_3x3.jpg"), quality=90)


if __name__ == "__main__":
    main()
