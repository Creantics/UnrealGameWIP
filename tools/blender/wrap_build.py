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
# --step proud of the one it covers. --cross adds a second pass wound the other way over the first (each point takes
# the higher of the two), so the bands cross in diamonds the way a bandage wound up and back down does.
#
# --ribbon builds the bandage from real strips instead of a lifted tube, so the turns keep their edges after a mesh is
# thinned: an anchor turn round the start, then a spiral whose turns each lie over the upper part of the one before (the
# covered part dips under the next turn's edge, which stands a lip proud), its last third of a turn narrowing to a torn
# end, and, with --cross, a narrower pass (--cross-width) wound the other way over the top. --strip is the strip width,
# --step the height of one layer; UVs are one island per turn, at true size.
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


def radius_at(Rc, t0, t1, L, s, a):
    """Bilinear body radius at axial distance s from the start joint and angle a, from surface_radius's grid."""
    ct, ca = Rc.shape
    fi = np.clip((s / L - t0) / (t1 - t0) * (ct - 1), 0, ct - 1 - 1e-6)
    fj = np.mod(a + np.pi, 2 * np.pi) / (2 * np.pi) * ca - 0.5
    i0, j0 = np.floor(fi).astype(int), np.floor(fj).astype(int)
    wi, wj = fi - i0, fj - j0
    i1 = np.minimum(i0 + 1, ct - 1)
    r0 = Rc[i0, j0 % ca] * (1 - wj) + Rc[i0, (j0 + 1) % ca] * wj
    r1 = Rc[i1, j0 % ca] * (1 - wj) + Rc[i1, (j0 + 1) % ca] * wj
    return r0 * (1 - wi) + r1 * wi


def ribbon(p0, ax, u, v, L, Rc, t0, t1, a, out, base, tbase):
    """One limb's bandage as strips (see --ribbon). Appends v/vt/f lines to out; returns the next indices."""
    s0, s1 = t0 * L, t1 * L
    strip, pitch = a.strip, a.pitch
    n_sp = max(1.0, (s1 - s0 - strip) / pitch)
    # rows across the strip (distance from its lower edge) and each row's lift in layers: the lower edge stands a lip
    # proud, the part the next turn covers dips under that turn's edge
    rows = np.clip(np.array([0.0, 0.07 * pitch, 0.18 * pitch, 0.5 * pitch, pitch - 0.3, pitch + 0.1, pitch + 0.45,
                             strip]), 0, strip)
    prof = np.array([1.05, 1.25, 1.1, 1.0, 0.95, 0.62, 0.45, 0.45])
    phi = np.linspace(0, 2 * np.pi * (1 + n_sp), int(a.per_turn * (1 + n_sp)) + 1)
    turn = phi / (2 * np.pi)
    s_low = s0 + np.maximum(0.0, turn - 1.0) * pitch
    shift = np.interp(turn, [0.0, 0.8, 1.0], [-0.5, -0.5, 0.0])        # the anchor turn lies one layer down
    wscale = 1.0 - 0.55 * np.clip((turn - (1 + n_sp - 0.35)) / 0.35, 0, 1)

    def emit(ang, sl, wrow, lift, island):
        nonlocal base, tbase
        n, nr = wrow.shape
        S = sl[:, None] + wrow
        A = np.repeat(ang[:, None], nr, 1)
        R = radius_at(Rc, t0, t1, L, S, A) + a.gap + lift * a.step
        P = p0 + S[..., None] * ax + R[..., None] * (np.cos(A)[..., None] * u + np.sin(A)[..., None] * v)
        out.extend("v %.5f %.5f %.5f" % tuple(q) for q in P.reshape(-1, 3))
        rm = float(R.mean())
        flip = ang[-1] < ang[0]          # wound the other way: keep the faces facing out
        vt = {}
        for i in range(n - 1):
            k = int(island[i])
            ua = (abs(ang[i] - ang[0]) - 2 * np.pi * k) * rm
            ub = ua + abs(ang[i + 1] - ang[i]) * rm
            for j in range(nr - 1):
                q = [(i, j, ua), (i + 1, j, ub), (i + 1, j + 1, ub), (i, j + 1, ua)]
                if flip:
                    q.reverse()
                c = []
                for ii, jj, uu in q:
                    if (ii, jj, k) not in vt:
                        vt[(ii, jj, k)] = tbase + len(vt)
                        out.append("vt %.5f %.5f" % (uu, wrow[ii, jj]))
                    c.append("%d/%d" % (base + ii * nr + jj, vt[(ii, jj, k)]))
                out.append("f " + " ".join(c))
        base += n * nr
        tbase += len(vt)

    emit(phi, s_low, rows[None, :] * wscale[:, None], prof[None, :] + shift[:, None], np.floor(turn - 1e-9).clip(0))
    if a.cross > 0:
        wc = a.cross_width
        n_c = max(1.0, (s1 - s0 - wc - 0.6) / a.cross)
        ph = np.linspace(0, 2 * np.pi * n_c, int(a.per_turn * n_c) + 1)
        tc = ph / (2 * np.pi)
        ends = np.minimum(np.clip(tc / 0.25, 0, 1), np.clip((n_c - tc) / 0.25, 0, 1))   # tucked, narrowing ends
        cw = (0.5 + 0.5 * ends)[:, None] * (np.array([0.0, 0.5, 1.0]) * wc)[None, :]
        cl = (1.75 + 0.35 * ends)[:, None] + np.array([0.0, 0.15, 0.0])[None, :]
        emit(-ph, s0 + 0.3 + tc * a.cross, cw, cl, np.floor(tc - 1e-9).clip(0))
    return base, tbase


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
    ap.add_argument("--cross", type=float, default=0.0,
                    help="pitch of a second pass wound the other way (0: none)")
    ap.add_argument("--ribbon", action="store_true", help="build the bandage from real strips (see the header)")
    ap.add_argument("--per-turn", type=int, default=28, help="--ribbon: samples per turn")
    ap.add_argument("--cross-width", type=float, default=2.4, help="--ribbon: the cross pass's strip width")
    ap.add_argument("--keep-axis", action="store_true",
                    help="use the given axis as is (a band round the torso: slice centroids there are lopsided and "
                         "would tilt it)")
    a = ap.parse_args()

    body = np.array([l.split()[1:4] for l in open(a.body) if l.startswith("v ")], float)
    out, base, tbase = ["# wrap_build"], 1, 1
    for name, *nums in a.limb:
        p0, p1, (t0, t1) = np.array(nums[:3], float), np.array(nums[3:6], float), map(float, nums[6:])
        if not a.keep_axis:
            p0, p1 = recentre(body, p0, p1, t0, t1, a.reach * 1.6)
        length = np.linalg.norm(p1 - p0) * (t1 - t0)
        nt, na = max(8, int(length * a.rows)), a.sides
        # sample the body on a coarse grid (bins wider than its vertex spacing), then upsample to the tube's rings
        ct, ca = max(4, int(length / a.bin)), a.bin_sides
        Rc, ax, u, v, L = surface_radius(body, p0, p1, t0, t1, ct, ca, a.reach)
        if a.ribbon:
            out.append("g " + name)
            n0 = len(out)
            base, tbase = ribbon(p0, ax, u, v, L, Rc, t0, t1, a, out, base, tbase)
            print("wrap_build: %s %.1f long, ribbon, %d faces" % (name, length, sum(1 for x in out[n0:] if x[0] == "f")))
            continue
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
        if a.cross > 0:
            s2 = (T[:, None] * L + (np.pi - A[None, :]) / (2 * np.pi) * a.cross) % a.cross / a.cross
            lift = np.maximum(lift, a.step * np.minimum(s2 / 0.15, 1.0) ** 0.5 * (1.0 - 0.35 * s2))
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
