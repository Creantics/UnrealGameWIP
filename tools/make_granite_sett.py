"""Generate the solhaven_granite_sett surface: grey granite setts in staggered courses, tileable.

The hero corner's lane paving (our own generated route; a scanned set may replace the files later under its own id).
One tile is 120 cm square. v5 (default --layout courses): courses of 8.5, 10 and 12 cm mixed, setts of 9, 12 and 15 cm
along each course, every course started at its own offset so the joints stagger; SurfaceJoints' entry
{"granite_sett", depth 1.3 cm, no unit grid} reads it. Joints are about 1.2 cm wide and 1.3 cm deep, filled low with
near-black grit, so the pattern reads from the lane's camera. --layout grid gives the old 12 x 12 stack bond (and
--units-u/--units-v/--bond running the 20 x 10 cm running bond); the joint table must then match.

Writes <out>/<id>_diff_2k.jpg (sRGB albedo), <id>_nor_dx_2k.jpg (DirectX normal from the true height),
<id>_arm_2k.jpg (R ambient occlusion: >= 0.92 on the stone tops, <= 0.6 in the joints; G roughness; B metal 0),
<id>_height_2k.png, <id>_tile2x2.jpg and <id>_report.json.

    python -I tools/make_granite_sett.py <out_dir> [--res 2048] [--seed 7]
"""

import argparse
import json
import os

import numpy as np
from PIL import Image

ID = "solhaven_granite_sett"
TILE_CM = 120.0
JOINT_CM = 1.2                    # joint width (both sides' insets together)
DEPTH_CM = 1.3                    # joint depth under a sett's top (v5; SurfaceJoints' granite_sett depth matches)
COURSE_CM = (8.5, 10.0, 12.0)     # v5 course heights, mixed per tile
SETT_CM = (9.0, 12.0, 15.0)       # v5 sett lengths along a course
GRIT = (0.035, 0.032, 0.029)      # the joints' fill, linear albedo: near-black wet grit
JOINT_AO = 0.42                   # the joints' AO ceiling


def periodic_noise(rng, n, sigma_px):
    """Gaussian-filtered white noise, periodic on the tile, scaled to unit deviation."""
    white = rng.standard_normal((n, n))
    f = np.fft.fftfreq(n)
    fx, fy = np.meshgrid(f, f)
    kernel = np.exp(-2.0 * (np.pi * sigma_px) ** 2 * (fx * fx + fy * fy))
    out = np.real(np.fft.ifft2(np.fft.fft2(white) * kernel))
    return out / (out.std() + 1e-9)


def blur(a, sigma_px):
    n = a.shape[0]
    f = np.fft.fftfreq(n)
    fx, fy = np.meshgrid(f, f)
    kernel = np.exp(-2.0 * (np.pi * sigma_px) ** 2 * (fx * fx + fy * fy))
    return np.real(np.fft.ifft2(np.fft.fft2(a) * kernel))


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--res", type=int, default=2048)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--units-u", type=int, default=12)
    ap.add_argument("--units-v", type=int, default=12)
    ap.add_argument("--bond", choices=("stack", "running"), default="stack")
    ap.add_argument("--layout", choices=("courses", "grid"), default="courses")
    args = ap.parse_args()
    UNITS_U, UNITS_V = args.units_u, args.units_v
    running = args.bond == "running"
    n = args.res
    rng = np.random.default_rng(args.seed)
    px_cm = TILE_CM / n
    stones = UNITS_U * UNITS_V
    sett_u, sett_v = TILE_CM / UNITS_U, TILE_CM / UNITS_V   # 10 x 10 cm

    c = (np.arange(n) + 0.5) / n
    u, v = np.meshgrid(c, c)
    if args.layout == "courses":
        # v5: courses of three heights, setts of three lengths, each course started at its own offset so no joint runs
        # on from the course below (staggered); the tile still wraps. SurfaceJoints reads no unit grid for it (0 x 0).
        heights = np.array(COURSE_CM, dtype=float)
        courses = rng.permutation(np.resize(heights, int(round(TILE_CM / heights.mean()))))
        courses *= TILE_CM / courses.sum()
        y_edges = np.concatenate([[0.0], np.cumsum(courses)])
        rows_x = []
        for _ in courses:
            lens = []
            while sum(lens) < TILE_CM - max(SETT_CM):
                lens.append(float(rng.choice(SETT_CM)))
            lens.append(TILE_CM - sum(lens))
            if lens[-1] < min(SETT_CM) * 0.75:              # fold a sliver into its neighbour
                lens[-2] += lens.pop()
            rows_x.append((np.concatenate([[0.0], np.cumsum(lens)]), rng.uniform(0.0, TILE_CM)))
        y_cm = v * TILE_CM
        x_cm = u * TILE_CM
        row = np.clip(np.searchsorted(y_edges, y_cm, side="right") - 1, 0, len(courses) - 1)
        cy_r = 0.5 * (y_edges[:-1] + y_edges[1:])
        ly = y_cm - cy_r[row]
        sett_v = courses[row]
        sid = np.zeros((n, n), np.int64)
        lx = np.zeros((n, n))
        sett_u = np.zeros((n, n))
        sizes_u, sizes_v = [], []
        base_id = 0
        for r_i, (edges, shift) in enumerate(rows_x):
            m = row == r_i
            xx = np.mod(x_cm[m] - shift, TILE_CM)
            k = np.clip(np.searchsorted(edges, xx, side="right") - 1, 0, len(edges) - 2)
            ctr = 0.5 * (edges[:-1] + edges[1:])
            dx_c = xx - ctr[k]
            lx[m] = dx_c
            sett_u[m] = (edges[1:] - edges[:-1])[k]
            sid[m] = base_id + k
            sizes_u += list(edges[1:] - edges[:-1])
            sizes_v += [courses[r_i]] * (len(edges) - 1)
            base_id += len(edges) - 1
        stones = base_id
        su_s, sv_s = np.array(sizes_u), np.array(sizes_v)
    else:
        # The grid, as SurfaceJoints reads it: row = floor(v * unitsV); in running bond odd rows shift half a unit.
        row = np.floor(v * UNITS_V).astype(np.int64)
        cu = u * UNITS_U - (0.5 * (row % 2) if running else 0.0)
        col = np.floor(cu).astype(np.int64)
        fu = cu - col
        fv = v * UNITS_V - row
        sid = (row % UNITS_V) * UNITS_U + (col % UNITS_U)
        lx = (fu - 0.5) * sett_u      # cm from the cell's centre
        ly = (fv - 0.5) * sett_v
        su_s, sv_s = np.full(stones, sett_u), np.full(stones, sett_v)

    # Per-sett character.
    half_j = 0.5 * JOINT_CM
    # Left, right, top, bottom insets, cm: hand-split setts sit unevenly, so no two joints run the same width.
    ins = 0.6 * half_j + rng.uniform(-0.1, 0.55, (stones, 4))
    radius = rng.uniform(0.6, 1.8, stones)
    bevel = rng.uniform(0.5, 1.0, stones)
    top = rng.uniform(0.93, 0.975, stones)
    tilt = rng.uniform(-0.02, 0.02, (stones, 2))
    dome = rng.uniform(0.005, 0.025, stones)
    tone = rng.normal(0.0, 1.0, stones)
    warmth = rng.normal(0.0, 1.0, stones)
    rough = rng.uniform(0.6, 0.74, stones)

    ox = 0.5 * (ins[sid, 0] - ins[sid, 1])
    oy = 0.5 * (ins[sid, 2] - ins[sid, 3])
    hx = 0.5 * sett_u - 0.5 * (ins[sid, 0] + ins[sid, 1])
    hy = 0.5 * sett_v - 0.5 * (ins[sid, 2] + ins[sid, 3])
    r = radius[sid]
    qx = np.abs(lx - ox) - (hx - r)
    qy = np.abs(ly - oy) - (hy - r)
    d = np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r
    # Split-stone edges: irregular at two scales.
    d += 0.18 * periodic_noise(rng, n, 2.0 / px_cm) + 0.06 * periodic_noise(rng, n, 0.35 / px_cm)
    inside = -d                                    # cm inside the sett's outline

    # Height, 0 the joint's floor, 1 a sett's highest point.
    s = np.clip(inside / bevel[sid], 0.0, 1.0)
    profile = 1.0 - (1.0 - s) ** 2.4
    nx = (lx - ox) / np.maximum(hx, 1e-3)
    ny = (ly - oy) / np.maximum(hy, 1e-3)
    surface = (top[sid] + tilt[sid, 0] * nx + tilt[sid, 1] * ny
               + dome[sid] * (1.0 - np.clip(nx * nx, 0, 1)) * (1.0 - np.clip(ny * ny, 0, 1))
               + 0.022 * periodic_noise(rng, n, 1.2 / px_cm) + 0.014 * periodic_noise(rng, n, 0.4 / px_cm)
               + 0.007 * periodic_noise(rng, n, 0.1 / px_cm))
    stone_h = np.where(inside > 0, profile * np.minimum(surface, 1.0), 0.0)

    # Chips off the arrises: up to three per sett, a scoop into the edge.
    for k in range(3):
        has = rng.random(stones) < 0.55
        side = rng.integers(0, 4, stones)
        along = rng.uniform(-0.8, 0.8, stones)
        cr = rng.uniform(0.5, 1.4, stones)
        cdepth = rng.uniform(0.25, 0.6, stones)
        hxs = 0.5 * su_s - 0.5 * (ins[:, 0] + ins[:, 1])
        hys = 0.5 * sv_s - 0.5 * (ins[:, 2] + ins[:, 3])
        oxs = 0.5 * (ins[:, 0] - ins[:, 1])
        oys = 0.5 * (ins[:, 2] - ins[:, 3])
        ccx = np.where(side == 0, oxs - hxs, np.where(side == 1, oxs + hxs, oxs + along * hxs))
        ccy = np.where(side == 2, oys - hys, np.where(side == 3, oys + hys, oys + along * hys))
        dist = np.hypot(lx - ccx[sid], ly - ccy[sid]) / cr[sid]
        cut = np.where(has[sid], cdepth[sid] * np.clip(1.0 - dist, 0.0, 1.0) ** 0.7, 0.0)
        stone_h = np.maximum(stone_h - cut * (inside > 0), 0.0)

    # The joints' grit fill: a little under half way up in places, near the floor in others.
    fill = np.clip(0.08 + 0.06 * periodic_noise(rng, n, 1.5 / px_cm)
                   + 0.03 * periodic_noise(rng, n, 0.1 / px_cm), 0.02, 0.3)
    height = np.maximum(stone_h, fill)
    height = blur(height, 0.6)

    # Ambient occlusion from the height and its cavity: tops >= 0.92, joints <= 0.6.
    cavity = np.clip(blur(height, 0.8 / px_cm) - height, 0.0, None)
    ao = 0.40 + 0.58 * np.clip(height, 0, 1) ** 0.7 - 1.2 * cavity
    ao = np.clip(ao, 0.25, 1.0)
    top_mask = height >= 0.88
    ao = np.where(top_mask, np.maximum(ao, 0.93), ao)

    # Albedo: grey granite, black mica and white feldspar grains, a tone per sett, grime toward the joints.
    grain_a = periodic_noise(rng, n, 0.06 / px_cm)
    grain_b = periodic_noise(rng, n, 0.045 / px_cm)
    grain_c = periodic_noise(rng, n, 0.18 / px_cm)
    base = (0.27 + 0.03 * tone[sid] + 0.012 * periodic_noise(rng, n, 3.0 / px_cm)
            + 0.02 * periodic_noise(rng, n, 15.0 / px_cm))
    lum = base + 0.035 * grain_c
    lum = np.where(grain_a > 1.35, lum * 0.35, lum)                        # mica
    lum = np.where((grain_b > 1.75) & (grain_a < 1.0), lum * 1.35 + 0.08, lum)  # feldspar
    lum = np.clip(lum, 0.05, 0.92)
    w = 0.03 * warmth[sid]
    rgb = np.stack([lum * (1.0 + w + 0.01), lum * (1.0 + 0.3 * w), lum * (1.0 - w + 0.015)], axis=-1)
    edge = smoothstep(0.0, 1.6, inside)
    grime = 1.0 - 0.28 * (1.0 - edge) * (inside > 0)
    rgb *= grime[..., None]
    grit = np.stack(GRIT) * (1.0 + 0.25 * periodic_noise(rng, n, 0.08 / px_cm))[..., None]
    in_joint = height <= fill + 0.02
    ao = np.where(in_joint, np.minimum(ao, JOINT_AO), ao)
    rgb = np.where(in_joint[..., None], grit, rgb)
    rgb = np.clip(rgb, 0.02, 0.95)
    srgb = np.where(rgb <= 0.0031308, rgb * 12.92, 1.055 * rgb ** (1 / 2.4) - 0.055)

    # Roughness: honed-but-weathered tops, rough grit.
    rgh = rough[sid] + 0.04 * grain_c - 0.03 * (top_mask & (grain_a > 1.55))
    rgh = np.where(in_joint, 0.92, rgh)
    rgh = np.where((~in_joint) & (height < 0.88), np.maximum(rgh, 0.8), rgh)
    rgh = np.clip(blur(rgh, 0.5), 0.5, 0.95)

    # DirectX normal from the true height (cm).
    hc = height * DEPTH_CM
    dx = (np.roll(hc, -1, axis=1) - np.roll(hc, 1, axis=1)) / (2.0 * px_cm)
    dy = (np.roll(hc, -1, axis=0) - np.roll(hc, 1, axis=0)) / (2.0 * px_cm)
    nrm = np.stack([-dx, -dy, np.ones_like(dx)], axis=-1)
    nrm /= np.linalg.norm(nrm, axis=-1, keepdims=True)

    os.makedirs(args.out, exist_ok=True)
    tag = "2k" if n == 2048 else ("1k" if n == 1024 else f"{n}px")

    def save(path, arr, **kw):
        Image.fromarray(np.clip(arr * 255.0 + 0.5, 0, 255).astype(np.uint8)).save(path, **kw)

    p = lambda name: os.path.join(args.out, f"{ID}_{name}")
    save(p(f"diff_{tag}.jpg"), srgb, quality=95)
    save(p(f"nor_dx_{tag}.jpg"), nrm * 0.5 + 0.5, quality=95)
    save(p(f"arm_{tag}.jpg"), np.stack([ao, rgh, np.zeros_like(ao)], axis=-1), quality=95)
    Image.fromarray(np.clip(height * 65535.0, 0, 65535).astype(np.uint16)).save(p(f"height_{tag}.png"))
    tile = np.tile(srgb[::2, ::2], (2, 2, 1))
    save(p("tile2x2.jpg"), tile, quality=90)

    report = {
        "id": ID,
        "source": "generated: tools/make_granite_sett.py",
        "seed": args.seed,
        "res": n,
        "tile_cm": TILE_CM,
        "units": [0, 0] if args.layout == "courses" else [UNITS_U, UNITS_V],
        "bond": ("staggered courses %s cm, setts %s cm" % (list(COURSE_CM), list(SETT_CM)) if args.layout == "courses"
                 else ("running, half" if running else "stack")),
        "stones": int(stones),
        "joint_cm": JOINT_CM,
        "depth_cm": DEPTH_CM,
        "ao_top_min": float(ao[top_mask].min()),
        "ao_joint_max": float(ao[in_joint].max()),
        "stone_share": float(top_mask.mean()),
        "joint_share": float(in_joint.mean()),
        "roughness_top_mean": float(rgh[top_mask].mean()),
        "roughness_joint_mean": float(rgh[in_joint].mean()),
        "albedo_top_mean_linear": float(rgb[top_mask].mean()),
        "metal": "none",
        "license": "project",
    }
    with open(p("report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1)
    print(json.dumps(report))


if __name__ == "__main__":
    main()
