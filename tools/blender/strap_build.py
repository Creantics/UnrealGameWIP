"""Builds webbing straps (a harness, a belt, a bandolier) round a body and the garments under them, headless, from a
JSON spec, and writes them as an OBJ (metres, Z up, the body FBX's frame) with one group per strap and per buckle,
ready for md_garment.py (UVs are the strap's own length and width in metres) and garment_bind.py.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/strap_build.py -- SPEC.json OUT.obj

SPEC.json:
    {"body": "mh_body.fbx", "over": ["SK_Jacket.fbx", ...],
     "skip_bones": ["upperarm", "lowerarm", "hand", "neck", "head"],
     "straps": [{"name": "strap_chest", "points": [[x, y, z], [x, y, z], [x, y, z]], "width": 0.038,
                 "thick": 0.003, "gap": 0.003, "samples": 220, "smooth": 3, "reach": 0.0}, ...],
     "buckles": [{"name": "buckle_chest", "strap": "strap_chest", "near": [x, y, z], "size": [0.05, 0.046],
                  "bar": 0.006, "depth": 0.004, "middle": true}, ...],
     "pouches": [{"name": "holster", "strap": "strap_thigh", "near": [x, y, z], "size": [0.07, 0.18],
                  "depth": 0.035, "drop": 0.0, "taper": 0.75, "gap": 0.004, "round": 0.008}, ...]}

A strap is a taut loop: it lies in the plane through its three "points" and follows the convex hull of everything it
goes round in that plane (the body and the "over" meshes at rest, less the vertices whose heaviest bone starts with one
of "skip_bones", so the arms and the head are not wrapped in), so it bridges hollows (an open jacket front, the small of
the back) the way a pulled strap does. Five sections across the width (the plane moved along its normal) are hulled
and lofted: the two edge rows sit "gap" off the hull, the inner rows "gap" + "thick", so the edges round off. A buckle
is a rectangular frame (and a middle bar if "middle") standing on the strap's centre line at the strap point nearest
"near", its long side across the strap. Each strap's start is its hull point furthest towards the front (-Y).
A strap with "reach" > 0 hulls only what lies within that distance of its points' centre (a cuff round one wrist,
a thigh strap round one leg; set "skip_bones": [] so the limb is not left out).
A strap with "x_range": [lo, hi] hulls only what lies between those world x, so a thigh strap goes round its own
leg where the thighs touch (its inner side then runs along the crease).
A strap with "over_straps": [names] also goes round those straps (built earlier in the list), so where the two cross
it lies on top and the other runs under it.
A pouch (a holster, a pocket on a strap) is a closed box hung on a strap at the strap point nearest "near": "size" is
its width along the strap and its height down (world -Z, square to the strap's outward direction), its top "drop"
below the strap's centre line, its width narrowing to "taper" of the top at the bottom; the inner face is pushed out
until it clears everything under it by "gap", and the outer face stands "depth" off the inner one with its rim pulled
in by "round" so the box reads stitched rather than cut."""
import json
import os
import sys

import bpy
import numpy as np
from mathutils import Vector
from mathutils.geometry import convex_hull_2d

argv = sys.argv[sys.argv.index("--") + 1:]
SP = json.load(open(argv[0]))
OUT = argv[1]


def load_points(path, skip):
    """Rest-pose vertex positions (world) of every mesh in an FBX or OBJ, less those mostly on a skipped bone."""
    before = set(bpy.data.objects)
    if path.lower().endswith(".fbx"):
        bpy.ops.import_scene.fbx(filepath=path)
    else:
        bpy.ops.wm.obj_import(filepath=path, forward_axis="Y", up_axis="Z")
    pts = []
    for o in set(bpy.data.objects) - before:
        if o.type != "MESH":
            continue
        names = [g.name for g in o.vertex_groups]
        M = o.matrix_world
        for v in o.data.vertices:
            if skip and v.groups:
                g = max(v.groups, key=lambda e: e.weight)
                if names[g.group].startswith(tuple(skip)):
                    continue
            pts.append(tuple(M @ v.co))
    return np.array(pts)


def hull_loop(P2, n):
    """The convex hull of 2-D points, counter-clockwise, resampled to n points by arc length."""
    idx = convex_hull_2d([tuple(p) for p in P2])
    H = P2[idx]
    if np.cross(H[1] - H[0], H[2] - H[1]) < 0:
        H = H[::-1]
    q = np.vstack([H, H[:1]])
    t = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(q, axis=0), axis=1))])
    tt = np.linspace(0, t[-1], n, endpoint=False)
    return np.stack([np.interp(tt, t, q[:, 0]), np.interp(tt, t, q[:, 1])], 1)


def smooth_closed(L, k):
    for _ in range(k):
        L = (np.roll(L, 1, 0) + 2 * L + np.roll(L, -1, 0)) / 4
    return L


def outward(L):
    t = np.roll(L, -1, 0) - np.roll(L, 1, 0)
    nrm = np.stack([t[:, 1], -t[:, 0]], 1)  # right of a counter-clockwise tangent is outward
    return nrm / np.linalg.norm(nrm, axis=1, keepdims=True)


bpy.ops.wm.read_factory_settings(use_empty=True)
skip = SP.get("skip_bones", ["upperarm", "lowerarm", "hand", "neck", "head"])
P = np.vstack([load_points(p, skip) for p in [SP["body"]] + SP.get("over", [])])
print("strap_build: %d rest vertices to go round" % len(P))

objs, centre = [], {}
for st in SP["straps"]:
    A = np.array(st["points"], float)
    c0 = A.mean(0)
    n = np.cross(A[1] - A[0], A[2] - A[0])
    n /= np.linalg.norm(n)
    fwd = np.array([0.0, -1.0, 0.0])
    e1 = fwd - n * (fwd @ n)
    if np.linalg.norm(e1) < 1e-3:
        e1 = np.array([1.0, 0, 0]) - n * n[0]
    e1 /= np.linalg.norm(e1)
    e2 = np.cross(n, e1)
    w, th, gap = st.get("width", 0.038), st.get("thick", 0.003), st.get("gap", 0.003)
    N, band = st.get("samples", 220), st.get("band", 0.006)
    reach = st.get("reach", 0.0)
    Ps = P[np.linalg.norm(P - c0, axis=1) < reach] if reach > 0 else P
    if "x_range" in st:
        lo, hi = st["x_range"]
        Ps = Ps[(Ps[:, 0] >= lo) & (Ps[:, 0] <= hi)]
    built = {nm: V for nm, V, _, _ in objs}
    if st.get("over_straps"):
        Ps = np.vstack([Ps] + [built[nm] for nm in st["over_straps"]])
    rows = []
    ref = None
    for f, off in ((-1.0, gap), (-0.8, gap + th), (0.0, gap + th), (0.8, gap + th), (1.0, gap)):
        c = c0 + n * f * w / 2
        d = (Ps - c) @ n
        S = Ps[np.abs(d) < band]
        if len(S) < 20:
            raise SystemExit("strap_build: %s: only %d vertices near the plane at %.2f" % (st["name"], len(S), f))
        P2 = np.stack([(S - c) @ e1, (S - c) @ e2], 1)
        L = smooth_closed(hull_loop(P2, N), st.get("smooth", 3))
        L = L + outward(L) * off  # (smoothing pulls the corners in by well under a millimetre; the gap covers it)
        k0 = int(np.argmax(L[:, 0])) if ref is None else int(np.argmin(np.linalg.norm(L - ref, axis=1)))
        L = np.roll(L, -k0, 0)
        if ref is None:
            ref = L[0]
        rows.append(c + np.outer(L[:, 0], e1) + np.outer(L[:, 1], e2))
    mid = rows[2]
    centre[st["name"]] = (mid, n)
    seg = np.linalg.norm(np.diff(np.vstack([mid, mid[:1]]), axis=0), axis=1)
    print("strap_build: %s: loop %.3f m, width %.3f m, z %.3f..%.3f, plane normal %s" % (
        st["name"], seg.sum(), w, mid[:, 2].min(), mid[:, 2].max(), np.round(n, 3)))
    R, C = len(rows), N + 1
    verts = np.vstack([np.vstack([r, r[:1]]) for r in rows])
    faces = [[r * C + k, r * C + k + 1, (r + 1) * C + k + 1, (r + 1) * C + k] for r in range(R - 1) for k in range(N)]
    lu = np.concatenate([[0], np.cumsum(seg)])
    lv = np.concatenate([[0], np.cumsum([np.linalg.norm(rows[r + 1] - rows[r], axis=1).mean() for r in range(R - 1)])])
    uvs = [(lu[i % C], lv[i // C]) for i in range(len(verts))]
    objs.append((st["name"], verts, faces, uvs))

for bk in SP.get("buckles", []):
    mid, n = centre[bk["strap"]]
    i = int(np.argmin(np.linalg.norm(mid - np.array(bk["near"]), axis=1)))
    p = mid[i]
    t = mid[(i + 1) % len(mid)] - mid[i - 1]
    t /= np.linalg.norm(t)
    out = np.cross(t, n)
    if out @ (p - mid.mean(0)) < 0:
        out = -out
    (Wb, Hb), bar, dep = bk.get("size", [0.05, 0.046]), bk.get("bar", 0.006), bk.get("depth", 0.004)
    verts, faces = [], []
    base = p + out * 0.001
    for z in (0.0, dep):
        for (hx, hy) in ((Hb / 2, Wb / 2), (Hb / 2 - bar, Wb / 2 - bar)):
            for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                verts.append(base + out * z + t * sx * hx + n * sy * hy)
    for a, c in ((8, 12), (12, 4), (4, 0), (0, 8)):
        for k in range(4):
            k2 = (k + 1) % 4
            faces.append([a + k, a + k2, c + k2, c + k])
    if bk.get("middle", True):
        o = len(verts)
        for z in (0.0, dep):
            for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                verts.append(base + out * z + t * sx * bar / 2 + n * sy * (Wb / 2 - bar))
        for a, c in ((o + 4, o), ):
            for k in range(4):
                k2 = (k + 1) % 4
                faces.append([c + k, c + k2, a + k2, a + k])
        faces += [[o + 7, o + 6, o + 5, o + 4]]
    uvs = [((q - p) @ t + Hb, (q - p) @ n + Wb) for q in verts]
    objs.append((bk["name"], np.array(verts), faces, uvs))
    print("strap_build: %s on %s at %s" % (bk["name"], bk["strap"], np.round(p, 3)))

Q = np.vstack([P] + [V for _, V, _, _ in objs])  # what a pouch must clear: the body, the "over" meshes, the straps
for pc in SP.get("pouches", []):
    mid, n = centre[pc["strap"]]
    i = int(np.argmin(np.linalg.norm(mid - np.array(pc["near"]), axis=1)))
    p = mid[i]
    t = mid[(i + 1) % len(mid)] - mid[i - 1]
    t /= np.linalg.norm(t)
    out = np.cross(t, n)
    if out @ (p - mid.mean(0)) < 0:
        out = -out
    down = np.array([0.0, 0.0, -1.0]) + out * out[2]
    down /= np.linalg.norm(down)
    side = np.cross(out, down)
    (W, Hh), dep = pc.get("size", [0.07, 0.18]), pc.get("depth", 0.035)
    drop, taper, pg, rnd = pc.get("drop", 0.0), pc.get("taper", 0.75), pc.get("gap", 0.004), pc.get("round", 0.008)
    nu, nv = 7, 13
    U, Vv = np.meshgrid(np.linspace(-0.5, 0.5, nu), np.linspace(0.0, 1.0, nv))
    um, vm = U * W * (1 - (1 - taper) * Vv), Vv * Hh
    inner = p + down * (drop + vm)[..., None] + side * um[..., None]
    # Push the inner face out until it clears what lies under it (within 1.5 cm across, less than 6 cm in front).
    need = np.zeros(nv * nu)
    for k, q in enumerate(inner.reshape(-1, 3)):
        d = Q - q
        a = d @ out
        m = (np.linalg.norm(d - np.outer(a, out), axis=1) < 0.015) & (a < 0.06)
        need[k] = max(0.0, a[m].max() + pg) if m.any() else 0.0
    s = need.reshape(nv, nu)
    for _ in range(6):  # spread the push so the face stays flat-ish instead of stepping
        pad = np.pad(s, 1, mode="edge")
        s = np.maximum(s, (pad[:-2, 1:-1] + pad[2:, 1:-1] + pad[1:-1, :-2] + pad[1:-1, 2:]) / 4)
    inner = inner + out * s[..., None]
    outer = inner + out * dep
    outer[0] += down * rnd
    outer[-1] -= down * rnd
    outer[:, 0] += side * rnd
    outer[:, -1] -= side * rnd
    verts, faces, uvs = [], [], []

    def sheet(G, uoff, flip):
        o = len(verts)
        verts.extend(G.reshape(-1, 3))
        uvs.extend(zip((um + uoff).ravel(), vm.ravel()))
        for j in range(nv - 1):
            for k in range(nu - 1):
                a, b, c, d = o + j * nu + k, o + j * nu + k + 1, o + (j + 1) * nu + k + 1, o + (j + 1) * nu + k
                faces.append([a, d, c, b] if flip else [a, b, c, d])

    sheet(inner, 0.0, False)  # faces the body
    sheet(outer, W + 0.02, True)  # faces out
    B = ([(0, k) for k in range(nu)] + [(j, nu - 1) for j in range(1, nv)] +
         [(nv - 1, k) for k in range(nu - 2, -1, -1)] + [(j, 0) for j in range(nv - 2, -1, -1)])
    ring_in, ring_out = np.array([inner[j, k] for j, k in B]), np.array([outer[j, k] for j, k in B])
    arc = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(ring_in, axis=0), axis=1))])
    o, L = len(verts), len(B)
    verts.extend(ring_in)
    verts.extend(ring_out)
    uvs.extend(zip(arc, np.full(L, Hh + 0.02)))
    uvs.extend(zip(arc, np.full(L, Hh + 0.02 + dep)))
    faces += [[o + k, o + L + k, o + L + k + 1, o + k + 1] for k in range(L - 1)]
    objs.append((pc["name"], np.array(verts), faces, uvs))
    print("strap_build: %s on %s at %s, pushed out up to %.3f m" % (pc["name"], pc["strap"], np.round(p, 3), s.max()))

lines, vb = ["# strap_build, metres, Z up, UVs in metres"], 1
for name, V, F, UV in objs:
    lines.append("g " + name)
    lines += ["v %.6f %.6f %.6f" % tuple(v) for v in V]
    lines += ["vt %.6f %.6f" % tuple(u) for u in UV]
    lines += ["f " + " ".join("%d/%d" % (vb + k, vb + k) for k in f) for f in F]
    vb += len(V)
os.makedirs(os.path.dirname(os.path.abspath(OUT)), exist_ok=True)
open(OUT, "w").write("\n".join(lines) + "\n")
print("strap_build: wrote %s: %d groups, %d vertices" % (OUT, len(objs), vb - 1))
