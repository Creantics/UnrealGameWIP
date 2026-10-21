"""Carves a character's volume from the silhouettes of an orthographic turnaround sheet (front, side, back).

    python -I tools/art/hull_from_views.py SHEET.png --height 1.88 --front x0,y0,x1,y1 --side x0,y0,x1,y1 \\
        --back x0,y0,x1,y1 --out hull.npy [--voxel 0.005] [--side-faces right] [--masks DIR]

Each box is the figure's pixel box on the sheet. A view's silhouette is everything darker than the white paper,
closed and hole-filled, scaled so its own height is --height (turnaround figures differ by a few percent), then
blurred a little along z so the sheet's pixel stairs don't become ribs.
Front and back give x and z, the side gives y and z. Each row's cross-section is a superellipse inscribed in the
box the views allow (one per separate front run, so the legs stay apart), cut to the side silhouette, so the
silhouettes are kept but the corners are round, except in the arms: in a T- or A-pose the side view sees the arm end-on in front of the torso, so arm
columns (front-view columns whose run through the shoulder line is short) get a round section as deep as the
run is tall (but no deeper than 1.15 times a typical upper arm, so the armpit doesn't bulge), columns outboard
of the innermost arm column count as arm too (the hands hang below the shoulder line), and the side view's depth
across the arm band can be bridged from the rows above and below it (--bridge; only needed when the end-on arm
shows outside the torso in the side view). --arm-run 0 turns the arm rule off (a head).
The output is a lightly blurred occupancy grid (float32 .npy, x, y, z; 0.5 is the surface) in Blender axes (+z up,
facing -y, metres) with its origin and voxel size in a .json beside it; hull_remesh.py turns it into a mesh.
Pure numpy/scipy/PIL, no Blender.
"""
import argparse
import json
import os

import numpy as np
from PIL import Image
from scipy import ndimage as nd

PAPER = 225          # a pixel is paper when all its channels are at least this
ARM_RUN = 0.32       # a front column whose run through the shoulder line is shorter than this (m) is an arm
SUPER = 2.5          # superellipse exponent of the body's cross-sections (2 = ellipse, higher = boxier)


def silhouette(sheet, box):
    x0, y0, x1, y1 = box
    a = sheet[y0:y1, x0:x1]
    m = a.min(axis=2) < PAPER
    m = nd.binary_closing(m, iterations=2)
    m = nd.binary_fill_holes(m)
    lab, n = nd.label(m)
    if n > 1:  # the figure is the biggest piece; stray marks go
        sizes = nd.sum(m, lab, range(1, n + 1))
        m = lab == (1 + int(np.argmax(sizes)))
    rows = np.where(m.any(axis=1))[0]
    cols = np.where(m.any(axis=0))[0]
    return m[rows[0]:rows[-1] + 1, cols[0]:cols[-1] + 1]


class View:
    """A silhouette with metres per pixel; sample(h, z) -> covered, h horizontal from the centre."""

    def __init__(self, mask, height, centre=None):
        self.m = mask
        self.s = height / mask.shape[0]
        self.cu = mask.shape[1] / 2.0 if centre is None else centre

    def cover(self, h, z):
        """Boolean array over the broadcast grid of h (m, image right positive) and z (m, up)."""
        u = np.floor(h / self.s + self.cu).astype(int)
        v = np.floor(self.m.shape[0] - z / self.s).astype(int)
        ok = (u >= 0) & (u < self.m.shape[1]) & (v >= 0) & (v < self.m.shape[0])
        out = np.zeros(np.broadcast(u, v).shape, dtype=bool)
        uu, vv = np.broadcast_arrays(u, v)
        out[ok] = self.m[vv[ok], uu[ok]]
        return out


def side_centre(mask, s):
    """Pixel column of the torso's middle in the side view: the middle of the silhouette at hip height."""
    h = mask.shape[0]
    rows = mask[int(h * 0.42):int(h * 0.50)]
    cols = [np.where(r)[0] for r in rows if r.any()]
    return float(np.median([0.5 * (c[0] + c[-1]) for c in cols]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sheet")
    ap.add_argument("--height", type=float, required=True)
    for k in ("front", "side", "back"):
        ap.add_argument("--" + k, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--voxel", type=float, default=0.005)
    ap.add_argument("--side-faces", choices=("right", "left"), default="right")
    ap.add_argument("--shoulder", type=float, default=None, help="shoulder line height in m (default 0.8 * height)")
    ap.add_argument("--masks", default=None)
    ap.add_argument("--bridge", action="store_true", help="bridge the side depth across the arm band")
    ap.add_argument("--arm-run", type=float, default=ARM_RUN, help="m; 0 = no arms (e.g. a head)")
    ap.add_argument("--side-centre", type=float, default=None, help="side view centre column, px in the cropped mask")
    a = ap.parse_args()
    sheet = np.asarray(Image.open(a.sheet).convert("RGB")).astype(int)
    box = {k: [int(x) for x in getattr(a, k).split(",")] for k in ("front", "side", "back")}
    H = a.height
    ms = {k: silhouette(sheet, b) for k, b in box.items()}
    if a.masks:
        os.makedirs(a.masks, exist_ok=True)
        for k, m in ms.items():
            Image.fromarray((m * 255).astype(np.uint8)).save(os.path.join(a.masks, k + ".png"))
    front, back = View(ms["front"], H), View(ms["back"], H)
    sc = a.side_centre if a.side_centre is not None else side_centre(ms["side"], H / ms["side"].shape[0])
    side = View(ms["side"], H, sc)
    sgn = -1.0 if a.side_faces == "right" else 1.0  # image right = -y when the figure faces right

    vx = a.voxel
    half_w = 0.5 * max(ms["front"].shape[1] * front.s, ms["back"].shape[1] * back.s) + 2 * vx
    xs = np.arange(-half_w, half_w, vx) + 0.5 * vx
    ys = np.arange(-0.35, 0.35, vx) + 0.5 * vx
    zs = np.arange(0.0, H + 2 * vx, vx) + 0.5 * vx
    X, Z = np.meshgrid(xs, zs, indexing="ij")
    fb = front.cover(X, Z) & back.cover(-X, Z)                  # (x, z)
    fb = nd.gaussian_filter(fb.astype(np.float32), (0.7, 1.5)) > 0.5   # the sheet's pixel stairs make ribs

    # arm columns: the run through the shoulder line is short
    zsh = a.shoulder if a.shoulder else 0.8 * H
    k_sh = int(np.argmin(abs(zs - zsh)))
    arm = np.zeros(len(xs), dtype=bool)
    run = {}
    for i in range(len(xs)):
        col = fb[i]
        if not col[k_sh]:
            continue
        lo = hi = k_sh
        while lo > 0 and col[lo - 1]:
            lo -= 1
        while hi < len(zs) - 1 and col[hi + 1]:
            hi += 1
        if (hi - lo + 1) * vx < a.arm_run:
            arm[i] = True
            run[i] = (lo, hi)
    # arm columns are outboard of the torso: keep only those connected to the outside
    for side_sign in (1, -1):
        idx = [i for i in range(len(xs)) if np.sign(xs[i]) == side_sign]
        idx.sort(key=lambda i: -abs(xs[i]))
        inside = False
        for i in idx:
            if not arm[i] and fb[i].any() and inside:
                arm[[j for j in idx if abs(xs[j]) < abs(xs[i])]] = False
                break
            if arm[i]:
                inside = True
    # hands hang a little below the shoulder line, so their columns miss it: everything outboard of the
    # innermost arm column on each side is arm too
    for side_sign in (1, -1):
        on = [i for i in range(len(xs)) if arm[i] and np.sign(xs[i]) == side_sign]
        if not on:
            continue
        edge = min(abs(xs[i]) for i in on)
        for i in range(len(xs)):
            if np.sign(xs[i]) == side_sign and abs(xs[i]) >= edge and not arm[i] and fb[i].any():
                hi = int(np.where(fb[i])[0][-1])   # the top segment; a flared hem lower down stays body
                lo = hi
                while lo > 0 and fb[i][lo - 1]:
                    lo -= 1
                if zs[lo] > 0.5 * H:
                    arm[i] = True
                    run[i] = (lo, hi)
    armxz = np.zeros_like(fb)
    for i in np.where(arm)[0]:
        armxz[i, run[i][0]:run[i][1] + 1] = True
    band = [run[i] for i in run if arm[i]]
    z_lo = min(r[0] for r in band) - 2 if band else None
    z_hi = max(r[1] for r in band) + 2 if band else None

    # side depth per row, bridged across the arm band
    Y, Z2 = np.meshgrid(ys, zs, indexing="ij")
    sd = side.cover(sgn * Y, Z2)                                # (y, z)
    sd = nd.gaussian_filter(sd.astype(np.float32), (0.7, 1.5)) > 0.5
    if band and a.bridge:
        def extent(k):
            yy = np.where(sd[:, k])[0]
            return (yy[0], yy[-1]) if len(yy) else None
        a_ext, b_ext = extent(max(0, z_lo - 1)), extent(min(len(zs) - 1, z_hi + 1))
        if a_ext and b_ext:
            for k in range(z_lo, z_hi + 1):
                t = (k - z_lo) / max(1, z_hi - z_lo)
                y0 = int(round(a_ext[0] + t * (b_ext[0] - a_ext[0])))
                y1 = int(round(a_ext[1] + t * (b_ext[1] - a_ext[1])))
                sd[:, k] = False
                sd[y0:y1 + 1, k] = True

    vol = np.zeros((len(xs), len(ys), len(zs)), dtype=bool)
    for k in range(len(zs)):
        yy = np.where(sd[:, k])[0]
        if not len(yy):
            continue
        cy, b = 0.5 * (ys[yy[0]] + ys[yy[-1]]), 0.5 * (yy[-1] - yy[0] + 1) * vx
        lab, n = nd.label(fb[:, k] & ~armxz[:, k])
        for r in range(1, n + 1):
            xx = np.where(lab == r)[0]
            cx, ax = 0.5 * (xs[xx[0]] + xs[xx[-1]]), 0.5 * (xx[-1] - xx[0] + 1) * vx
            sec = (np.abs((xs[xx, None] - cx) / ax) ** SUPER + np.abs((ys[None, :] - cy) / b) ** SUPER) <= 1.0
            vol[xx, :, k] = sec & sd[None, :, k]
    yc = 0.0
    arm_i = np.where(arm)[0]
    # the arm is no deeper than it is tall, and near the armpit (where the front run grows) no deeper than a
    # typical upper arm, or it bulges out of the chest and back
    r_ref = float(np.median([0.5 * (run[i][1] - run[i][0] + 1) * vx for i in arm_i])) if len(arm_i) else 0.0
    for i in arm_i:
        lo, hi = run[i]
        zc, rz = 0.5 * (zs[lo] + zs[hi]), 0.5 * (hi - lo + 1) * vx
        ry = min(rz, 1.15 * r_ref)
        sec = ((Z2.T - zc) / rz) ** 2 + ((Y.T - yc) / ry) ** 2 <= 1.0   # (z, y)
        vol[i, :, lo:hi + 1] = sec.T[:, lo:hi + 1] & fb[i][None, lo:hi + 1]
    vol = nd.binary_opening(vol, iterations=1)
    lab, n = nd.label(vol)
    if n > 1:
        sizes = nd.sum(vol, lab, range(1, n + 1))
        vol = lab == (1 + int(np.argmax(sizes)))
    dens = nd.gaussian_filter(vol.astype(np.float32), sigma=1.5)
    np.save(a.out, dens)
    meta = {"origin": [float(xs[0]), float(ys[0]), float(zs[0])], "voxel": vx, "voxels": int(vol.sum()), "grid": list(vol.shape), "arm_columns": int(arm.sum()),
            "arm_band_m": [round(float(zs[z_lo]), 3), round(float(zs[z_hi]), 3)] if band else None,
            "scale_m_per_px": {k: round(H / m.shape[0], 5) for k, m in ms.items()}}
    with open(os.path.splitext(a.out)[0] + ".json", "w") as f:
        json.dump(meta, f, indent=1)
    print("hull_from_views:", json.dumps(meta))


if __name__ == "__main__":
    main()
