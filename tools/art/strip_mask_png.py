"""System Python: the texture mask for UE's SkeletalMeshEditorSubsystem.strip_lod_geometry, from the triangles
tools/blender/body_mask.py writes.
    python -I -X utf8 tools/art/strip_mask_png.py <prefix> [res=4096] [jitter=0.05] [trials=8] [free=1]
Simulates LODUtilities.cpp's StripLODGeometry exactly: UV0 (UE's v = 1 - Blender's v) plus KINDA_SMALL_NUMBER, snapped to
floor(uv * res); a triangle whose snapped box is one pixel thin tests the pixel (MinU, MinV) only; otherwise it is stripped
unless a non-black pixel in [Min, Max) meets the snapped triangle under TriangleStripHelper::IntersectTriangleAndAABB.
Paints black every pixel the strip triangles meet, then whitens, for each keep triangle that would go, the pixel it meets
that the fewest strip triangles need (then the same under `trials` jitters of +-`jitter` px, for float differences).
With free=1 every pixel no keep triangle of any LOD reads goes black too, so LOD1+ triangles whose footprints poke a pixel
past the painted islands are stripped with them (keep triangles of every LOD are rescued again after; about ten
minutes on a 60k-triangle body at 4096).
Writes <prefix>_mask.png (RGB, black = strip), <prefix>_mask_preview.png and <prefix>_mask_report.json, and simulates the
other LODs (<prefix>_lods.npz) against the mask."""
import sys, json
import numpy as np
from PIL import Image, ImageDraw

PRE = sys.argv[1]
opt = dict(a.split("=", 1) for a in sys.argv[2:] if "=" in a)
RES = int(opt.get("res", 4096))
JIT = float(opt.get("jitter", 0.05))
TRIALS = int(opt.get("trials", 8))
KSN = float(np.float32(1e-4))  # KINDA_SMALL_NUMBER, a float added to the double UVs
rep0 = json.load(open(PRE + "_report.json"))
d = np.load(PRE + "_tris.npz")
S, K = d["strip"], d["keep"]


def snap(T, jit=None):  # (n, 3, 2) Blender UVs -> UE pixel coordinates, as LODUtilities.cpp computes them
    u = T[..., 0].astype(np.float32).astype(np.float64)
    v = (np.float32(1.0) - T[..., 1].astype(np.float32)).astype(np.float64)
    uv = np.stack([u, v], -1)
    if jit is not None:
        uv = uv + jit
    P = np.floor((uv + KSN) * RES).astype(np.int64)
    return np.fmod(P, RES + 1)  # C++ % keeps the dividend's sign


def seg(a0, a1, b0x, b0y, b1x, b1y):  # SegmentIntersection2D, segment a fixed, segments b as arrays
    vax, vay = a1[0] - a0[0], a1[1] - a0[1]
    vbx, vby = b1x - b0x, b1y - b0y
    den = -vbx * vay + vax * vby
    with np.errstate(divide="ignore", invalid="ignore"):
        s = (-vay * (a0[0] - b0x) + vax * (a0[1] - b0y)) / den
        t = (vbx * (a0[1] - b0y) - vby * (a0[0] - b0x)) / den
    return (s >= 0) & (s <= 1) & (t >= 0) & (t <= 1)


def meets(P, bx, by):  # IntersectTriangleAndAABB(snapped triangle P, pixel boxes [bx, bx+1] x [by, by+1])
    T = P.astype(np.float64)
    x0 = bx.astype(np.float64); y0 = by.astype(np.float64); x1 = x0 + 1.0; y1 = y0 + 1.0
    tmin, tmax = T.min(0), T.max(0)
    ok = ~((tmin[0] >= x1) | (x0 >= tmax[0]) | (tmin[1] >= y1) | (y0 >= tmax[1]))
    res = np.zeros(len(x0), bool)
    for p in T:
        res |= (p[0] >= x0) & (p[0] <= x1) & (p[1] >= y0) & (p[1] <= y1)
    ext = 2.0 * np.sqrt(2.0) + 2.0 * float(np.hypot(*(tmax - tmin)))
    edges = ((T[0], T[1]), (T[1], T[2]), (T[2], T[0]))
    for cx, cy in ((x0, y0), (x1, y1), (x0, y1), (x1, y0)):
        cnt = sum(seg(a, b, cx, cy, cx + ext, cy + ext).astype(np.int8) for a, b in edges)
        res |= cnt == 1
    for a, b in edges:
        res |= seg(a, b, x0, y0, x0, y1) | seg(a, b, x1, y1, x0, y1) | seg(a, b, x1, y1, x1, y0) | seg(a, b, x0, y0, x1, y0)
    return ok & res


def touched(P):  # the pixels UE reads for this triangle: (ys, xs); stripped iff all of them are black
    mn = np.clip(P.min(0), 0, RES); mx = np.clip(P.max(0), 0, RES)
    if mn[0] == mx[0] or mn[1] == mx[1]:
        return np.array([min(mn[1], RES - 1)]), np.array([min(mn[0], RES - 1)])
    gx, gy = np.meshgrid(np.arange(mn[0], mx[0]), np.arange(mn[1], mx[1]))
    gx = gx.ravel(); gy = gy.ravel()
    m = meets(P, gx, gy)
    return gy[m], gx[m]


def sat(black):  # summed-area table of the black pixels
    I = np.zeros((RES + 1, RES + 1), np.int32)
    I[1:, 1:] = black.cumsum(0, dtype=np.int32).cumsum(1, dtype=np.int32)
    return I


def stripped(Ps, black, I=None):  # UE's verdict for every triangle
    I = sat(black) if I is None else I
    mn = np.clip(Ps.min(1), 0, RES); mx = np.clip(Ps.max(1), 0, RES)
    area = np.maximum(mx[:, 0] - mn[:, 0], 1) * np.maximum(mx[:, 1] - mn[:, 1], 1)
    x0, y0 = mn[:, 0], mn[:, 1]; x1 = np.maximum(mx[:, 0], x0 + 1); y1 = np.maximum(mx[:, 1], y0 + 1)
    x1 = np.minimum(x1, RES); y1 = np.minimum(y1, RES); x0 = np.minimum(x0, RES - 1); y0 = np.minimum(y0, RES - 1)
    nb = I[y1, x1] - I[y0, x1] - I[y1, x0] + I[y0, x0]
    out = np.zeros(len(Ps), bool)
    for i in np.nonzero(nb > 0)[0]:  # no black pixel in the box: kept
        if nb[i] == area[i] and not (mn[i, 0] == mx[i, 0] or mn[i, 1] == mx[i, 1]):
            out[i] = True  # an all-black box
            continue
        ys, xs = touched(Ps[i])
        out[i] = bool(black[ys, xs].all())
    return out


SP = snap(S); KP = snap(K)
black = np.zeros((RES, RES), bool)
need = np.zeros((RES, RES), np.int32)
for P in SP:
    ys, xs = touched(P)
    black[ys, xs] = True; need[ys, xs] += 1
print("MASKPNG painted", int(black.sum()), "pixels for", len(SP), "strip triangles")


def rescue(Ps, label):  # whiten one pixel for every keep triangle that would be stripped
    lost = np.nonzero(stripped(Ps, black))[0]
    for i in lost:
        ys, xs = touched(Ps[i])
        c = Ps[i].mean(0)
        j = np.lexsort((np.hypot(xs + 0.5 - c[0], ys + 0.5 - c[1]), need[ys, xs]))[0]
        black[ys[j], xs[j]] = False
    print("MASKPNG", label, "keep triangles rescued", len(lost))
    return len(lost)


rescued = [rescue(KP, "exact")]
rng = np.random.default_rng(7)
for t in range(TRIALS):
    rescued.append(rescue(snap(K, rng.uniform(-JIT, JIT, K.shape) / RES), "jitter %d" % t))
try:
    L = np.load(PRE + "_lods.npz")
    LODS = [k for k in range(1, 8) if "lod%d_tris" % k in L]
except FileNotFoundError:
    L, LODS = None, []
free_added = 0
if opt.get("free", "0") == "1":
    # every pixel that no keep triangle of any LOD reads (exactly or under 4 jitters) goes black: no keep verdict can
    # change, and the LOD1+ triangles whose footprints poke a pixel past the painted islands (their outlines are not
    # LOD0's) go with their islands
    prot = np.zeros((RES, RES), bool)
    keeps = [K] + [L["lod%d_tris" % k][~L["lod%d_want" % k]] for k in LODS]
    for T in keeps:
        for jit in [None] + [rng.uniform(-JIT, JIT, T.shape) / RES for _ in range(4)]:
            for P in snap(T, jit):
                ys, xs = touched(P)
                prot[ys, xs] = True
    free_added = int((~prot & ~black).sum())
    black |= ~prot
    print("MASKPNG free pixels blackened", free_added)
    for k in LODS:
        KL = L["lod%d_tris" % k][~L["lod%d_want" % k]]
        rescued.append(rescue(snap(KL), "lod%d exact" % k))
        rescued.append(rescue(snap(KL, rng.uniform(-JIT, JIT, KL.shape) / RES), "lod%d jitter" % k))
    rescued.append(rescue(KP, "exact again"))
I = sat(black)
s_ok = stripped(SP, black, I); k_bad = stripped(KP, black, I)
jit_s, jit_k = [], []
for t in range(4):  # fresh jitters, verdicts only
    jit_s.append(int(stripped(snap(S, rng.uniform(-JIT, JIT, S.shape) / RES), black).sum()))
    jit_k.append(int(stripped(snap(K, rng.uniform(-JIT, JIT, K.shape) / RES), black).sum()))
rep = {"res": RES, "strip_tris": int(len(S)), "keep_tris": int(len(K)), "black_pixels": int(black.sum()),
       "free_added": free_added, "rescued_per_pass": rescued, "lod0_strip_stripped": int(s_ok.sum()),
       "lod0_keep_stripped": int(k_bad.sum()), "fresh_jitter_strip_stripped": jit_s, "fresh_jitter_keep_stripped": jit_k}

# the other LODs against the mask; a wrongly stripped triangle outside the band would show
lo = rep0["cuff_tops"]; band = (max(lo.values()) + rep0["margin"], rep0["waist_zmin"] - rep0["margin"])
if L is not None:
    for k in LODS:
        T, W, C = L["lod%d_tris" % k], L["lod%d_want" % k], L["lod%d_c" % k]
        st = stripped(snap(T), black, I)
        bad = st & ~W
        rep["lod%d" % k] = {"tris": int(len(T)), "want_strip": int(W.sum()), "stripped": int(st.sum()),
                            "stripped_unwanted": int(bad.sum()), "kept_wanted": int((~st & W).sum()),
                            "unwanted_outside_band": int((bad & ((C[:, 2] <= band[0]) | (C[:, 2] >= band[1]))).sum()),
                            "unwanted_z": [round(float(C[bad, 2].min()), 3), round(float(C[bad, 2].max()), 3)] if bad.any() else None}
print("MASKPNG report", json.dumps(rep))
json.dump(rep, open(PRE + "_mask_report.json", "w"), indent=1)
img = np.where(black, 0, 255).astype(np.uint8)
Image.fromarray(np.stack([img] * 3, -1), "RGB").save(PRE + "_mask.png")
# preview: the mask at 2048 with every LOD0 triangle outlined (strip red, keep grey)
pv = Image.fromarray(img).resize((2048, 2048), Image.NEAREST).convert("RGB")
dr = ImageDraw.Draw(pv)
for T, col in ((K, (150, 150, 150)), (S, (220, 30, 30))):
    for tri in T:
        dr.polygon([(float(a) * 2048, (1.0 - float(b)) * 2048) for a, b in tri], outline=col)
pv.save(PRE + "_mask_preview.png")
print("MASKPNG_DONE")
