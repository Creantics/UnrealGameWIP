"""Recut a stock groom outside Unreal: read the strands the FTGroomTools plugin wrote (.ftgs), edit them, and write them
as Alembic for Unreal's groom importer (the groom_patch.py convention) with a before/after preview.

In the editor:  unreal.FTGroomToolsLibrary.export_groom_strands(unreal.load_asset(groom_path), "D:/out/x.ftgs")
Then:           blender -b --factory-startup --python-exit-code 1 --python tools/blender/groom_strands.py -- spec.json

spec.json: {"in": ftgs, "out": abc, "preview": png, "head": npz, "seed": 1, "guides": false, "name": "groom", "ops": [..]}
Positions are in groom space, which for a MetaHuman groom is the head's own Unreal frame (cm, x lateral, +Y the face
direction, Z up). Guide strands are dropped unless "guides" is true; Unreal builds its own guides from the rest. Every
strand is written into one group (Blender's curves carry no group attribute), so a two-group groom comes back as one.
Without "out" the script only prints the summary (root bounds and lengths per group) for choosing regions. "head" is
an .npz with "v", the groom head's vertices in the same frame, drawn in grey under the strands in the preview;
"preview_k" is how many hits on a pixel make it fully dark (default 6; raise it for a dense scalp groom).

Each op applies to the strands whose root lies in its "where" region, through a soft mask (1 inside, 0 outside):
  "x" / "absx" / "y" / "z": [lo, hi]   root coordinate (or |x|) between lo and hi; null leaves that side open
  "below": [z0, k]   z < z0 + k*x*x       "above": [z0, k]   z > z0 + k*x*x
  "behind": [y0, k]  y < y0 - k*x*x       "ahead": [y0, k]   y > y0 - k*x*x
  "ahead_pts" / "behind_pts": [[x, y], ..]   y ahead of / behind the line through these points (x ascending), a free
             curve such as a hairline set back at the temples
  "above_pts" / "below_pts": [[x, z], ..]   z above / below the line through these points (x ascending): the top
             and bottom edges of a brow
  "oval": [y0, z0, ry, rz]   inside the ellipse about (y0, z0) with these radii in the side (y-z) plane: an arch over
             the ear
  "xprofile": [[x, f], ..]   the mask times a piecewise-linear factor over the root's x (held flat past the ends): a
             parting moved over with "move" + "snap", full at the old parting and easing out to either side
  "soft": cm   each limit fades to nothing over this width at its edge (default 0.5)
  "group": id  only the strands of this group
  "on": "tip"  test the strand's tip instead of its root
With no "where" an op applies to every strand. Ops run in order:
  {"op": "mirror"}                        x -> -x for every strand (the parting moves to the other side)
  {"op": "mirror", "keep": "+x"}          make the groom symmetric: drop the strands rooted on the other side and
                                          replace them with the kept side's strands mirrored (a thinner brow evened up)
  {"op": "dup", "frac": f, "d": [x, y, z], "jitter": cm}
                                          copy each strand inside with probability frac * mask (frac up to 1; repeat
                                          the op for more), moved by d plus a random sideways jitter up to this size;
                                          with "head" the copy's root is put back on the head surface (projected onto
                                          the tangent plane at the nearest head vertex) and the strand moves with it.
                                          Fills a bare patch from the hair beside it, or thickens a sparse region
  {"op": "length", "scale": s}            cut or extend each strand to s times its arc length, s blended toward 1 by
                                          the mask; an extension carries on along the tip direction
  {"op": "length", "max": cm}             cut each strand to at most this arc length (blended the same way)
  {"op": "length", "add": cm, "min": cm}  lengthen (or with a negative value shorten) each strand by this much, a
                                          shortened one never below "min" (default 0.3) or its own length if shorter
  {"op": "cut", "at": {..}, "min": cm}    cut each strand where it first enters the "at" region (tested on its points,
                                          same keys as "where"), at a random depth into the region's soft edge so the
                                          cut line is dithered; never shorter than "min" (default 0.2). A strand rooted
                                          inside is cut to "min" too, unless "rooted": "keep" leaves it whole (so a cut
                                          over the ear doesn't shave the scalp under it)
  {"op": "delete"}                        remove each strand with probability = mask (a dithered edge)
  {"op": "thin", "keep": f}               keep a fraction f of the strands inside (probability 1 - (1 - f) * mask)
  {"op": "move", "d": [x, y, z], "power": p}
                                          add d * mask * t^p to each point, t its arc position (0 root, 1 tip):
                                          p 0 moves whole strands, p 1 bends them about the root; with "snap": true
                                          (and "head") each moved root goes back onto the head surface and its strand
                                          moves with it
  {"op": "lift", "clear": cm}             (needs "head") push each point that sits below the head surface back out
                                          along the nearest head vertex's normal, to at least "clear" (default 0.01)
                                          above the surface plus the root's own height (blended by the mask): a strand
                                          moved or bent into the skin ends up lying on it instead
  {"op": "shell", "file": npz, "frac": f, "radius": cm, "smooth": cm, "kmin": a, "kmax": b}
                                          (needs "head") give the hair a target volume: "file" holds "v", a surface in
                                          groom space (the outside of a sculpted or scanned hairstyle). At each root the
                                          shell height is how far that surface reaches out along the head normal (its
                                          points within "radius", default 1.2, of the normal line), box-averaged over
                                          roots within "smooth" (default 2.5) so single spikes don't count. Each strand's
                                          offset along its root normal is then scaled so its highest point sits at frac
                                          (default 1) times that height, the factor held to [kmin, kmax] (default 0.3, 4)
                                          and blended toward 1 by the mask. Offsets are scaled, not re-aimed, so strands
                                          keep their flow; spikes come after from clump and bend
  {"op": "grow", "file": npz, "frac": f, "rise": a, "climb": c, "hmin": cm, "radius": cm, "smooth": cm}
                                          (needs "head") rebuild each strand from its root against a target volume
                                          instead of rescaling it: the shell height is found as in "shell" (times
                                          frac, at least hmin, default 0.2), and the strand walks over the scalp along
                                          its own root-to-tip direction, keeping its length and point count, rising
                                          to the shell height under the point it has reached by the fraction "rise"
                                          of its length (default 0.35, smoothstep), never climbing faster than
                                          "climb" (default 0.9) cm per cm walked. Blended toward the old strand by the
                                          mask. Follow it with "clump" for locks and "bend" for spikes
  {"op": "width", "scale": s}             scale the point widths (s blended toward 1 by the mask)
  {"op": "wave", "amp": cm, "wavelength": cm, "power": p}
                                          a sine wave across each strand, random phase and direction per strand,
                                          growing as t^p from the root (default 1)
  {"op": "bend", "amount": f, "power": p} push each tip sideways by a random f times the strand length, the bend
                                          growing as t^p (default 2): more direction variation between strands
  {"op": "clump", "size": cm, "amount": f, "power": p}
                                          gather the strands into locks: roots are binned into cells about this size
                                          (randomly offset), one random strand per cell is the lock's centre, and every
                                          point moves a fraction amount * mask * t^p (default p 1.5) of the way toward
                                          the centre strand's point at the same arc fraction: the roots stay, the tips
                                          converge, and small gaps open between the locks. With "spike": cm (and
                                          "head") each centre strand is first lifted out along its root normal by a
                                          random 0..spike cm times t^2, so the locks stand up as spikes
  {"op": "smooth", "amount": f, "iters": n}
                                          straighten the curl in each strand: n passes (default 4) moving every point a
                                          fraction f (default 0.5) toward the middle of its neighbours, the root fixed
A cut or extended strand keeps its point count, resampled evenly along its new length.
"""
import json, struct, sys
import numpy as np

spec = json.load(open(sys.argv[sys.argv.index("--") + 1]))
rng = np.random.default_rng(spec.get("seed", 1))

raw = open(spec["in"], "rb").read()
if raw[:4] != b"FTGS" or struct.unpack_from("<i", raw, 4)[0] != 1:
    raise SystemExit("not an FTGS v1 file: " + spec["in"])
S, V = struct.unpack_from("<ii", raw, 8)
ST = np.dtype([("n", "<i4"), ("g", "<i4"), ("guide", "<i4"), ("u", "<f4"), ("v", "<f4"), ("w", "<f4")])
st = np.frombuffer(raw, ST, S, 16).copy()
pts = np.frombuffer(raw, "<f4", V * 4, 16 + S * ST.itemsize).reshape(V, 4).astype(np.float64)
P, W = pts[:, :3].copy(), pts[:, 3].copy()


def offsets(n):
    return np.concatenate([[0], np.cumsum(n)])


def keep_strands(keep):
    global st, P, W
    vk = np.repeat(keep, st["n"])
    st, P, W = st[keep], P[vk], W[vk]


if not spec.get("guides"):
    keep_strands(st["guide"] == 0)


def arcs():
    """Per vertex: strand index and arc length from the root; per strand: total length."""
    n = st["n"]
    off = offsets(n)
    sid = np.repeat(np.arange(len(n)), n)
    d = np.zeros(len(P))
    d[1:] = np.linalg.norm(P[1:] - P[:-1], axis=1)
    d[off[:-1]] = 0
    c = np.cumsum(d)
    c -= np.repeat(c[off[:-1]], n)
    return sid, c, c[off[1:] - 1], off


def ramp(d, w):
    t = np.clip(d / w, 0, 1) if w > 0 else (d > 0).astype(float)
    return t * t * (3 - 2 * t)


def region(where, R=None):
    off = offsets(st["n"])
    if R is None:
        R = P[off[1:] - 1] if where and where.get("on") == "tip" else P[off[:-1]]
    m = np.ones(len(R))
    if not where:
        return m
    w = where.get("soft", 0.5)
    x, y, z = R[:, 0], R[:, 1], R[:, 2]
    for k, val in (("x", x), ("absx", np.abs(x)), ("y", y), ("z", z)):
        if k in where:
            lo, hi = where[k]
            if lo is not None:
                m *= ramp(val - lo, w)
            if hi is not None:
                m *= ramp(hi - val, w)
    x2 = x * x
    if "below" in where:
        m *= ramp(where["below"][0] + where["below"][1] * x2 - z, w)
    if "above" in where:
        m *= ramp(z - where["above"][0] - where["above"][1] * x2, w)
    if "behind" in where:
        m *= ramp(where["behind"][0] - where["behind"][1] * x2 - y, w)
    if "ahead" in where:
        m *= ramp(y - where["ahead"][0] + where["ahead"][1] * x2, w)
    if "ahead_pts" in where:  # [[x, y], ..] sorted by x: y beyond the line through these points (held flat past the ends)
        q = np.asarray(where["ahead_pts"], float)
        m *= ramp(y - np.interp(x, q[:, 0], q[:, 1]), w)
    if "behind_pts" in where:
        q = np.asarray(where["behind_pts"], float)
        m *= ramp(np.interp(x, q[:, 0], q[:, 1]) - y, w)
    if "above_pts" in where:  # [[x, z], ..] sorted by x: z above the line through these points (a brow's arch)
        q = np.asarray(where["above_pts"], float)
        m *= ramp(z - np.interp(x, q[:, 0], q[:, 1]), w)
    if "below_pts" in where:
        q = np.asarray(where["below_pts"], float)
        m *= ramp(np.interp(x, q[:, 0], q[:, 1]) - z, w)
    if "xprofile" in where:  # [[x, f], ..] sorted by x
        q = np.asarray(where["xprofile"], float)
        m *= np.interp(x, q[:, 0], q[:, 1])
    if "oval" in where:
        y0, z0, ry, rz = where["oval"]
        m *= ramp((1 - np.sqrt(((y - y0) / ry) ** 2 + ((z - z0) / rz) ** 2)) * min(ry, rz), w)
    if "group" in where:
        m *= st["g"] == where["group"] if len(R) == len(st) else np.repeat(st["g"] == where["group"], st["n"])
    return m


def kdtree(V):
    """A mathutils KD-tree over points V (Blender's Python has no scipy)."""
    from mathutils.kdtree import KDTree
    t = KDTree(len(V))
    for i, v in enumerate(V.tolist()):
        t.insert(v, i)
    t.balance()
    return t


_head = {}


def head_near(q):
    """For points q: the nearest head vertex and its normal, the normal turned to point out of the head (away from
    the head's centre). The head and its KD-tree are built once."""
    if not _head:
        hd = np.load(spec["head"])
        Hv, Hf = hd["v"].astype(float), hd["f"]
        fn = np.cross(Hv[Hf[:, 1]] - Hv[Hf[:, 0]], Hv[Hf[:, 2]] - Hv[Hf[:, 0]])
        Hn = np.zeros_like(Hv)
        for c in range(3):
            np.add.at(Hn, Hf[:, c], fn)
        Hn /= np.maximum(np.linalg.norm(Hn, axis=1, keepdims=True), 1e-9)
        if ((Hv - Hv.mean(0)) * Hn).sum() < 0:
            Hn = -Hn
        _head.update(v=Hv, n=Hn, t=kdtree(Hv))
    f = _head["t"].find
    j = np.fromiter((f(p)[1] for p in q.tolist()), np.int64, len(q))
    return _head["v"][j], _head["n"][j]


def onto_head(r, shift):
    """The shifts that put roots r + shift back on the head surface: each moved root is projected onto the tangent
    plane at its nearest head vertex."""
    q = r + shift
    hv, hn = head_near(q)
    return shift - (((q - hv) * hn).sum(1))[:, None] * hn


def shell_height(r, n, C, radius, reach=8.0):
    """For roots r with outward normals n: the furthest point of the surface points C within radius of each root's
    normal line, measured along it (0 where none lies within reach)."""
    C = C[np.all((C > r.min(0) - reach) & (C < r.max(0) + reach), axis=1)]
    find = kdtree(C).find_range  # candidates: a chain of small balls along each root's normal segment
    step = radius
    ts = np.arange(step / 2 - 0.5, reach + step / 2, step)
    ball = float(np.hypot(step / 2, radius))
    H = np.zeros(len(r))
    for i in range(len(r)):
        j = list({k for c in (r[i] + n[i] * ts[:, None]).tolist() for _, k, _ in find(c, ball)})
        if j:
            p = r[i]
            q = C[j] - p
            t = q @ n[i]
            ok = ((q * q).sum(1) - t * t < radius * radius) & (t > -0.5) & (t < reach)
            H[i] = t[ok].max() if ok.any() else 0.0
    return H


def box_smooth(r, h, size):
    """Average h over the roots in the 3x3x3 block of size-cm cells around each root's cell."""
    c = np.floor(r / size).astype(np.int64) + 1000
    key = (c[:, 0] * 4096 + c[:, 1]) * 4096 + c[:, 2]
    u, inv = np.unique(key, return_inverse=True)
    s, cnt = np.bincount(inv, h, len(u)), np.bincount(inv, None, len(u))
    tot, num = np.zeros(len(r)), np.zeros(len(r))
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for dz in (-1, 0, 1):
                nk = key + (dx * 4096 + dy) * 4096 + dz
                j = np.clip(np.searchsorted(u, nk), 0, len(u) - 1)
                hit = u[j] == nk
                tot[hit] += s[j[hit]]
                num[hit] += cnt[j[hit]]
    return tot / np.maximum(num, 1)


def perp(L):
    """A random unit vector per strand, at right angles to its root-to-tip line."""
    off = offsets(st["n"])
    a = P[off[1:] - 1] - P[off[:-1]]
    a /= np.maximum(np.linalg.norm(a, axis=1, keepdims=True), 1e-9)
    r = rng.normal(size=a.shape)
    r -= a * (r * a).sum(1, keepdims=True)
    return r / np.maximum(np.linalg.norm(r, axis=1, keepdims=True), 1e-9)


def resample(newlen):
    """Each strand to its new arc length, same point count, points evenly spaced; past the old tip the strand carries on
    along its last segment."""
    global P, W
    sid, c, L, off = arcs()
    n = st["n"]
    j = np.arange(len(P)) - np.repeat(off[:-1], n)
    a = np.repeat(newlen, n) * j / np.maximum(np.repeat(n, n) - 1, 1)
    key = sid * 1e6 + c  # arc lengths sorted within each strand, strands in order: one global search
    i = np.searchsorted(key, sid * 1e6 + np.minimum(a, np.repeat(L, n)), side="right") - 1
    i = np.clip(i, off[sid], np.maximum(off[sid + 1] - 2, off[sid]))
    seg = np.maximum(c[np.minimum(i + 1, off[sid + 1] - 1)] - c[i], 1e-9)
    t = np.clip((a - c[i]) / seg, 0, 1)
    i1 = np.minimum(i + 1, off[sid + 1] - 1)
    NP = P[i] + (P[i1] - P[i]) * t[:, None]
    NW = W[i] + (W[i1] - W[i]) * t
    last, prev = off[1:] - 1, np.maximum(off[1:] - 2, off[:-1])
    tip = P[last] - P[prev]
    tip /= np.maximum(np.linalg.norm(tip, axis=1, keepdims=True), 1e-9)
    over = np.maximum(a - np.repeat(L, n), 0)
    ext = over > 0
    NP[ext] = (np.repeat(P[last], n, axis=0) + np.repeat(tip, n, axis=0) * over[:, None])[ext]
    NW[ext] = np.repeat(W[last], n)[ext]
    P, W = NP, NW


def summary(tag):
    _, _, L, off = arcs()
    R = P[off[:-1]]
    print("%s: %d strands, %d points" % (tag, len(st), len(P)))
    for g in np.unique(st["g"]):
        k = st["g"] == g
        print("  group %d: %d strands, roots x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f, length p10/50/90 %.2f/%.2f/%.2f cm"
              % ((g, k.sum()) + tuple(np.r_[R[k].min(0), R[k].max(0)][[0, 3, 1, 4, 2, 5]])
                 + tuple(np.percentile(L[k], [10, 50, 90]))))


summary("in")
P0, n0 = P.copy(), st["n"].copy()
for op in spec.get("ops", []):
    kind = op["op"]
    if kind == "mirror":
        if "keep" not in op:
            P[:, 0] *= -1
            continue
        sgn = 1.0 if op["keep"] == "+x" else -1.0
        off = offsets(st["n"])
        side = P[off[:-1], 0] * sgn > 0
        keep_strands(side)
        Q = P.copy()
        Q[:, 0] *= -1
        P, W, st = np.concatenate([P, Q]), np.concatenate([W, W]), np.concatenate([st, st])
        print("op mirror %d strands kept on the %s side, mirrored" % (int(side.sum()), op["keep"]))
        continue
    if kind == "dup":
        m = region(op.get("where"))
        k = np.flatnonzero(rng.random(len(m)) < op["frac"] * m)
        off = offsets(st["n"])
        idx = np.concatenate([np.arange(off[i], off[i + 1]) for i in k]) if len(k) else np.zeros(0, int)
        shift = np.asarray(op.get("d", [0, 0, 0]), float)[None, :] + perp(None)[k] * rng.uniform(
            0, op.get("jitter", 0.0), len(k))[:, None]
        if spec.get("head") and len(k):
            shift = onto_head(P[off[:-1]][k], shift)
        P = np.concatenate([P, P[idx] + np.repeat(shift, st["n"][k], axis=0)])
        W, st = np.concatenate([W, W[idx]]), np.concatenate([st, st[k]])
        print("op dup    %d strands copied" % len(k))
        continue
    m = region(op.get("where"))
    if kind == "length":
        _, _, L, _ = arcs()
        if "scale" in op:
            target = L * op["scale"]
        elif "add" in op:
            target = np.maximum(L + op["add"], np.minimum(L, op.get("min", 0.3)))
        else:
            target = np.minimum(L, op["max"])
        resample(L + (target - L) * m)
    elif kind == "cut":
        sid, c, L, off = arcs()
        thr = rng.uniform(0.05, 0.95, len(st))
        inside = region(op["at"], P) > thr[sid]
        rooted = inside[off[:-1]].copy()
        inside[off[:-1]] = False  # a strand rooted inside the region keeps its minimum length
        first = np.full(len(st), np.inf)
        np.minimum.at(first, sid[inside], c[inside])
        if op.get("rooted") == "keep":
            first[rooted] = np.inf
        cutlen = np.where(np.isfinite(first), np.maximum(first, op.get("min", 0.2)), L)
        resample(L + (np.minimum(cutlen, L) - L) * m)
    elif kind == "delete":
        keep_strands(rng.random(len(m)) >= m)
    elif kind == "thin":
        keep_strands(rng.random(len(m)) < 1 - (1 - op["keep"]) * m)
    elif kind == "move":
        sid, c, L, off = arcs()
        t = (c / np.maximum(L[sid], 1e-9)) ** op.get("power", 1.0)
        P += np.asarray(op["d"], float)[None, :] * (m[sid] * t)[:, None]
        if op.get("snap") and spec.get("head"):
            k = np.flatnonzero(m > 1e-3)
            if len(k):
                fix = np.zeros((len(st), 3))
                fix[k] = onto_head(P[off[:-1]][k], np.zeros((len(k), 3)))
                P += np.repeat(fix, st["n"], axis=0)
    elif kind == "lift":
        if not spec.get("head"):
            raise SystemExit("lift needs a head")
        sid, _, _, off = arcs()
        k = np.flatnonzero(np.repeat(m, st["n"]) > 1e-3)
        k = k[~np.isin(k, off[:-1])]  # the roots stay where they are
        if len(k):
            hv, hn = head_near(P[k])
            h = ((P[k] - hv) * hn).sum(1)
            rv, rn = head_near(P[off[:-1]])
            h0 = np.maximum(((P[off[:-1]] - rv) * rn).sum(1), 0)
            push = np.maximum(h0[sid[k]] + op.get("clear", 0.01) - h, 0) * m[sid[k]]
            P[k] += push[:, None] * hn
            print("op lift   %d points pushed out, max %.3f cm" % (int((push > 1e-4).sum()), push.max()))
    elif kind == "shell":
        if not spec.get("head"):
            raise SystemExit("shell needs a head")
        sid, _, _, off = arcs()
        r = P[off[:-1]]
        _, rn = head_near(r)
        H = box_smooth(r, shell_height(r, rn, np.load(op["file"])["v"].astype(float), op.get("radius", 1.2)),
                       op.get("smooth", 2.5))
        h = ((P - r[sid]) * rn[sid]).sum(1)
        top = np.zeros(len(st))
        np.maximum.at(top, sid, h)
        k = np.clip(op.get("frac", 1.0) * H / np.maximum(top, 0.2), op.get("kmin", 0.3), op.get("kmax", 4.0))
        k = 1 + (k - 1) * m
        P += ((k[sid] - 1) * h)[:, None] * rn[sid]
        print("op shell  height p10/50/90 %.2f/%.2f/%.2f cm, scale p10/50/90 %.2f/%.2f/%.2f"
              % (*np.percentile(H, [10, 50, 90]), *np.percentile(k, [10, 50, 90])))
    elif kind == "grow":
        if not spec.get("head"):
            raise SystemExit("grow needs a head")
        _, _, L, off = arcs()
        n = st["n"]
        r = P[off[:-1]]
        rv, rn = head_near(r)
        H = box_smooth(r, shell_height(r, rn, np.load(op["file"])["v"].astype(float), op.get("radius", 1.2)),
                       op.get("smooth", 2.5))
        H = np.maximum(H * op.get("frac", 1.0), op.get("hmin", 0.2))
        hlook = kdtree(r).find
        h0 = ((r - rv) * rn).sum(1)
        d = P[off[1:] - 1] - r  # the flow: each strand's own root-to-tip direction, laid on the scalp
        d -= rn * (d * rn).sum(1, keepdims=True)
        d /= np.maximum(np.linalg.norm(d, axis=1, keepdims=True), 1e-9)
        j = np.arange(len(P)) - np.repeat(off[:-1], n)
        ds = np.repeat(L / np.maximum(n - 1, 1), n)
        Q = P.copy()
        pos, dir_, hprev = r.copy(), d.copy(), h0.copy()
        rise, cap = op.get("rise", 0.35), op.get("climb", 0.9)
        for k in range(1, int(n.max())):
            s = np.flatnonzero(n > k)  # strands that have a point k
            v = off[s] + k
            t = k / (n[s] - 1)
            Ht = H[np.fromiter((hlook(p)[1] for p in pos[s].tolist()), np.int64, len(s))]
            u = np.minimum(t / rise, 1)
            h = h0[s] + np.minimum(Ht * u * u * (3 - 2 * u), cap * k * ds[v])
            dh = h - hprev[s]
            tang = np.sqrt(np.maximum(ds[v] ** 2 - dh * dh, 0.04 * ds[v] ** 2))
            q = pos[s] + dir_[s] * tang[:, None]
            hv, hn = head_near(q)
            q -= hn * ((q - hv) * hn).sum(1, keepdims=True)  # back onto the scalp, then up to this height
            nd = q - pos[s]
            nd -= hn * (nd * hn).sum(1, keepdims=True)
            dir_[s] = nd / np.maximum(np.linalg.norm(nd, axis=1, keepdims=True), 1e-9)
            pos[s], hprev[s] = q, h
            Q[v] = q + hn * h[:, None]
        mv = np.repeat(m, n)[:, None]
        P = P + (Q - P) * mv
        print("op grow   shell height p10/50/90 %.2f/%.2f/%.2f cm" % tuple(np.percentile(H, [10, 50, 90])))
    elif kind == "width":
        W *=np.repeat(1 + (op["scale"] - 1) * m, st["n"])
    elif kind == "wave":
        sid, c, L, _ = arcs()
        d, ph = perp(L), rng.uniform(0, 2 * np.pi, len(st))
        t = (c / np.maximum(L[sid], 1e-9)) ** op.get("power", 1.0)
        amp = op["amp"] * m[sid] * t * np.sin(2 * np.pi * c / op["wavelength"] + ph[sid])
        P += d[sid] * amp[:, None]
    elif kind == "bend":
        sid, c, L, _ = arcs()
        d = perp(L) * (rng.uniform(0, 1, len(st)) * op["amount"] * L * m)[:, None]
        P += d[sid] * ((c / np.maximum(L[sid], 1e-9)) ** op.get("power", 2.0))[:, None]
    elif kind == "clump":
        sid, c, L, off = arcs()
        R = P[off[:-1]]
        cell = np.floor((R - R.min(0) + rng.uniform(0, op["size"], 3)) / op["size"]).astype(np.int64)
        _, inv = np.unique(cell[:, 0] * 1000003 + cell[:, 1] * 1009 + cell[:, 2], return_inverse=True)
        perm = rng.permutation(len(st))
        _, first = np.unique(inv[perm], return_index=True)
        cen = perm[first][inv]  # each strand's lock centre, a random member of its cell
        fr = np.minimum(c / np.maximum(L[sid], 1e-9), 1.0)
        if op.get("spike") and spec.get("head"):  # lift each lock's centre strand out from the scalp first: spiky locks
            _, rn = head_near(R)
            lead = np.unique(cen)
            up = np.zeros(len(st))
            up[lead] = rng.uniform(0, op["spike"], len(lead))
            P += rn[sid] * (up[sid] * m[sid] * fr ** 2)[:, None]
        key = sid + fr * 0.999  # fractions sorted within each strand, strands in order: one global search
        g = cen[sid]
        i = np.searchsorted(key, g + fr * 0.999, side="right") - 1
        i = np.clip(i, off[g], np.maximum(off[g + 1] - 2, off[g]))
        i1 = np.minimum(i + 1, off[g + 1] - 1)
        u = np.clip((fr - fr[i]) / np.maximum(fr[i1] - fr[i], 1e-9), 0, 1)
        G = P[i] + (P[i1] - P[i]) * u[:, None]
        a = op.get("amount", 0.5) * m[sid] * fr ** op.get("power", 1.5)
        P += (G - P) * a[:, None]
        print("op clump  %d locks" % len(first))
    elif kind == "smooth":
        # pull each inner point toward the middle of its neighbours (the root stays put, the tip follows its neighbour's
        # direction): calms curl and frizz without changing where the strand goes
        off = offsets(st["n"])
        a = op.get("amount", 0.5) * np.repeat(m, st["n"])
        inner = np.ones(len(P), bool)
        inner[off[:-1]] = inner[off[1:] - 1] = False
        tip = off[1:] - 1
        tip = tip[st["n"] > 2]
        for _ in range(op.get("iters", 4)):
            Q = P.copy()
            Q[inner] = 0.5 * (P[np.flatnonzero(inner) - 1] + P[np.flatnonzero(inner) + 1])
            Q[tip] = 2 * P[tip - 1] - P[tip - 2]
            P += (Q - P) * a[:, None]
    else:
        raise SystemExit("unknown op " + kind)
    print("op %-6s %d strands in the region (mask > .5)" % (kind, int((m > 0.5).sum())))
summary("out")

if spec.get("preview"):
    # the front half from the front, the left half from the left and the right half from the right (face toward the
    # camera's side), the input on the top row and the result below, darker where strands pile up (Blender has no PIL:
    # the PNG goes through a Blender image)
    import bpy
    lo, hi = P0.min(0), P0.max(0)
    span = max(hi - lo) + 4
    N = 600
    s = N / span
    cen = (lo + hi) / 2
    img = np.ones((2 * N, 3 * N), np.float32)

    def views(Q):
        # (column, horizontal coordinate, which points) per panel
        return ((0, Q[:, 0] - cen[0], Q[:, 1] > cen[1]), (1, Q[:, 1] - cen[1], Q[:, 0] > cen[0]),
                (2, cen[1] - Q[:, 1], Q[:, 0] < cen[0]))

    if spec.get("head"):
        Hv = np.load(spec["head"])["v"]
        Hv = Hv[(Hv[:, 2] > lo[2] - 4) & (Hv[:, 2] < hi[2] + 2)]
        for row in (0, 1):
            for col, h, sel in views(Hv):
                u = (N / 2 + h * s).astype(int) + col * N
                v = (N / 2 - (Hv[:, 2] - cen[2]) * s).astype(int) + row * N
                k = sel & (u >= col * N) & (u < (col + 1) * N) & (v >= row * N) & (v < (row + 1) * N)
                img[v[k], u[k]] = 0.8

    def draw(Q, n, row):
        off = offsets(n)
        mid = np.ones(len(Q), bool)
        mid[off[1:] - 1] = False
        Qs = [Q] + [Q[mid] + (Q[1:][mid[:-1]] - Q[mid]) * f for f in (0.25, 0.5, 0.75)]
        Q = np.concatenate(Qs)
        for col, h, sel in views(Q):
            u = (N / 2 + h * s).astype(int) + col * N
            v = (N / 2 - (Q[:, 2] - cen[2]) * s).astype(int) + row * N
            k = sel & (u >= col * N) & (u < (col + 1) * N) & (v >= row * N) & (v < (row + 1) * N)
            acc = np.zeros_like(img)
            np.add.at(acc, (v[k], u[k]), 1.0)
            img[:] = np.minimum(img, 1 - np.clip(acc / spec.get("preview_k", 6.0), 0, 0.92))

    draw(P0, n0, 0)
    draw(P, st["n"], 1)
    img[:, N - 1:N + 1] = img[:, 2 * N - 1:2 * N + 1] = 0.5
    img[N - 1:N + 1, :] = 0.5
    rgba = np.ones((2 * N, 3 * N, 4), np.float32)
    rgba[..., :3] = img[::-1, :, None]
    im = bpy.data.images.new("preview", 3 * N, 2 * N, alpha=True)
    im.pixels.foreach_set(rgba.ravel())
    im.filepath_raw = spec["preview"]
    im.file_format = "PNG"
    im.save()

if spec.get("out"):
    import bpy
    bpy.ops.wm.read_factory_settings(use_empty=True)
    cu = bpy.data.curves.new(spec.get("name", "groom"), "CURVE")
    cu.dimensions = "3D"
    off = offsets(st["n"])
    co = np.empty((len(P), 4))
    co[:, 0], co[:, 1], co[:, 2], co[:, 3] = P[:, 0] / 100, -P[:, 1] / 100, P[:, 2] / 100, 1
    rad = W / 200  # Alembic widths come from the point radius (m); Unreal reads them in cm
    for a, b in zip(off[:-1], off[1:]):
        sp = cu.splines.new("POLY")
        sp.points.add(int(b - a) - 1)
        sp.points.foreach_set("co", co[a:b].ravel())
        sp.points.foreach_set("radius", rad[a:b])
    ob = bpy.data.objects.new(spec.get("name", "groom"), cu)
    bpy.context.scene.collection.objects.link(ob)
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.wm.alembic_export(filepath=spec["out"], selected=True, start=1, end=1, global_scale=100.0,
                              export_hair=False, export_particles=False)
    print("CURVES", len(st), spec["out"])
