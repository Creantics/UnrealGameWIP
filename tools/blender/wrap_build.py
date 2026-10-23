# Build bandage wraps (forearm, shin, hand...) around a body: a tube that follows the limb's surface between two
# points, with a spiral of overlapping turns raised in the geometry, so the wrap reads as a strip wound round the limb
# instead of a smooth sleeve. Plain Python (numpy + scipy), no Blender; the OBJ it writes goes through md_garment like
# an MD export (one group per limb).
#
#   python wrap_build.py body.obj out.obj --limb wrap_l 46 95.4 0.7 43 116.6 -5.8 -0.05 0.68 [--limb ...]
#
# Each --limb is a group name, the start and end joints (body units and axes, e.g. wrist then elbow) and the stretch of
# that segment to cover, as fractions (0 = start joint, 1 = end joint). The tube sits --gap off the body; each turn is
# --strip wide and advances --pitch along the limb (pitch < strip, so turns overlap), and a turn's leading edge stands
# --step proud of the one it covers.
import argparse
import numpy as np
from scipy.ndimage import uniform_filter1d
from scipy.spatial import cKDTree


def recentre(body, a, b, t0, t1, reach, rounds=3):
    """Joints sit off the limb's middle; move the axis onto the centroids of body slices at t0 and t1."""
    for _ in range(rounds):
        ax = b - a
        L = np.linalg.norm(ax)
        d = body - a
        t = d @ ax / L ** 2
        r = np.linalg.norm(d - np.outer(t, ax), axis=1)
        ends = []
        for tt in (t0, t1):
            m = (np.abs(t - tt) < 0.04) & (r < reach)
            ends.append(body[m].mean(0) if m.sum() > 8 else a + tt * ax)
        # keep the end points at the joints' parameter values on the new line
        c0, c1 = ends
        a, b = c0 - t0 / (t1 - t0) * (c1 - c0), c0 + (1 - t0) / (t1 - t0) * (c1 - c0)
    return a, b


def surface_radius(body, a, b, t0, t1, nt, na, reach):
    """Body radius round the a->b axis, sampled on an nt x na grid over [t0, t1] (max radius per bin)."""
    ax = b - a
    L = np.linalg.norm(ax)
    ax /= L
    u = np.cross(ax, [0, 0, 1.0] if abs(ax[2]) < 0.9 else [1.0, 0, 0])
    u /= np.linalg.norm(u)
    v = np.cross(ax, u)
    d = body - a
    t = d @ ax / L
    rv = d - np.outer(d @ ax, ax)
    r = np.linalg.norm(rv, axis=1)
    pad = 0.3 / nt
    m = (t > t0 - pad) & (t < t1 + pad) & (r < reach)
    t, r, ang = t[m], r[m], np.arctan2(rv[m] @ v, rv[m] @ u)
    ti = np.clip(((t - t0) / (t1 - t0) * (nt - 1)).round().astype(int), 0, nt - 1)
    ai = ((ang + np.pi) / (2 * np.pi) * na).astype(int) % na
    R = np.full((nt, na), np.nan)
    for i, j, rr in zip(ti, ai, r):
        if np.isnan(R[i, j]) or rr > R[i, j]:
            R[i, j] = rr
    # fill empty bins from their filled neighbours (round the ring first, then along the limb)
    for _ in range(max(nt, na)):
        holes = np.isnan(R)
        if not holes.any():
            break
        P = np.pad(R, ((1, 1), (0, 0)), mode="edge")
        nb = np.stack([np.roll(R, 1, 1), np.roll(R, -1, 1), P[:-2], P[2:]])
        R[holes] = np.nanmean(nb, axis=0)[holes]
    # a thumb base or an elbow point catches a bin or two: hold each ring to a smooth cross-section
    med = np.median(R, axis=1, keepdims=True)
    R = np.minimum(R, 1.3 * med)
    R = uniform_filter1d(uniform_filter1d(R, 3, axis=1, mode="wrap"), 3, axis=0, mode="nearest")
    return R, ax, u, v, L


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("body"); ap.add_argument("out")
    ap.add_argument("--limb", nargs=9, action="append", required=True,
                    metavar=("NAME", "X0", "Y0", "Z0", "X1", "Y1", "Z1", "T0", "T1"))
    ap.add_argument("--gap", type=float, default=0.25)
    ap.add_argument("--strip", type=float, default=5.0)
    ap.add_argument("--pitch", type=float, default=3.6)
    ap.add_argument("--step", type=float, default=0.18)
    ap.add_argument("--reach", type=float, default=7.5, help="ignore body vertices farther than this off the axis")
    ap.add_argument("--bin", type=float, default=1.5, help="body sampling bin length along the limb")
    ap.add_argument("--bin-sides", type=int, default=20, help="body sampling bins round the limb")
    ap.add_argument("--rows", type=int, default=6, help="rings per cm along the limb")
    ap.add_argument("--sides", type=int, default=72)
    a = ap.parse_args()

    body = np.array([l.split()[1:4] for l in open(a.body) if l.startswith("v ")], float)
    out, base, tbase = ["# wrap_build"], 1, 1
    for name, *nums in a.limb:
        p0, p1, (t0, t1) = np.array(nums[:3], float), np.array(nums[3:6], float), map(float, nums[6:])
        p0, p1 = recentre(body, p0, p1, t0, t1, a.reach * 1.6)
        length = np.linalg.norm(p1 - p0) * (t1 - t0)
        nt, na = max(8, int(length * a.rows)), a.sides
        # sample the body on a coarse grid (bins wider than its vertex spacing), then upsample to the tube's rings
        ct, ca = max(4, int(length / a.bin)), a.bin_sides
        Rc, ax, u, v, L = surface_radius(body, p0, p1, t0, t1, ct, ca, a.reach)
        fi = np.linspace(0, ct - 1, nt)
        fj = (np.arange(na) + 0.5) / na * ca - 0.5
        i0 = np.clip(np.floor(fi).astype(int), 0, ct - 2)
        j0 = np.floor(fj).astype(int)
        wi, wj = (fi - i0)[:, None], (fj - j0)[None, :]
        Rrow = Rc[i0] * (1 - wi) + Rc[i0 + 1] * wi
        R = Rrow[:, j0 % ca] * (1 - wj) + Rrow[:, (j0 + 1) % ca] * wj
        T = np.linspace(t0, t1, nt)
        A = np.linspace(-np.pi, np.pi, na, endpoint=False) + np.pi / na
        s = (T[:, None] * L + (A[None, :] + np.pi) / (2 * np.pi) * a.pitch) % a.pitch / a.pitch
        # sawtooth: each turn rises over the one before it, with its leading edge rounded off
        lift = a.step * np.minimum(s / 0.15, 1.0) ** 0.5 * (1.0 - 0.35 * s)
        rad = R + a.gap + lift
        pts = (p0 + np.outer(T * L, ax)[:, None, :]
               + rad[..., None] * (np.cos(A)[None, :, None] * u + np.sin(A)[None, :, None] * v))
        out.append("g " + name)
        out += ["v %.5f %.5f %.5f" % tuple(q) for q in pts.reshape(-1, 3)]
        # UVs unrolled at true size (body units), with a seam column so the last face doesn't wrap back to u = 0
        circ = 2 * np.pi * rad.mean()
        out += ["vt %.5f %.5f" % (j / na * circ, T[i] * L) for i in range(nt) for j in range(na + 1)]
        idx = lambda i, j: base + i * na + j % na
        tdx = lambda i, j: tbase + i * (na + 1) + j
        for i in range(nt - 1):
            for j in range(na):
                q = [(i, j), (i, j + 1), (i + 1, j + 1), (i + 1, j)]
                out.append("f " + " ".join("%d/%d" % (idx(*c), tdx(*c)) for c in q))
        base += nt * na
        tbase += nt * (na + 1)
        print("wrap_build: %s %.1f long, radius %.1f..%.1f, %d x %d" % (name, length, rad.min(), rad.max(), nt, na))
    open(a.out, "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
