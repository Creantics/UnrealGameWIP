"""Rebuilds the painted eyes of a scanned lead with face shapes as real eyes: an eyeball on each eye joint and
lids made of clean rings that close over it.

    blender -b FACE.blend --factory-startup --python-exit-code 1 --python tools/blender/face_patch.py -- \\
        --face face.json --out name.glb [--blend name.blend] [--preview DIR]

FACE.blend is face_transfer's .blend; face.json is the one face_transfer used (eye_l, optional "eye" entry below).
A scan's eye is paint on a closed surface, and cutting a hole in 2 mm triangles leaves a jagged edge that no shape
can close. So the scan's faces round each eye are replaced by a patch of rings. The rows next to the opening are
laid out in angles round the eyeball centre (the proof head's lid_point parameterisation): three rows rolling back
under the lid margin, the margin, and a lid-thickness row. The rows beyond are laid out from the front, a few
millimetres apart, and settle from the lid onto the scan's own surface. The outermost ring lies on the scan and is
stitched to the jagged edge of the removed faces. Each new vertex takes the texture, joint weights and face-shape
deltas of the scan surface it stands for (the lid rows sample the skin just outside the margin), and eyeBlink,
eyeWide and eyeSquint are rebuilt for the patch by moving the rows in elevation round the eyeball. The scan's own
eye-key deltas fade to zero near the patch.

face.json "eye" (all optional): {"r": eyeball radius, "half_w", "up", "down": lid opening half width and heights
in metres, "tilt": lateral corner lift in degrees, "mask_mm": how far the painted eye reaches past the new margin, "iris": [r, g, b], "left", "right": [x, z] of each eye's centre when
the scan's eyes are not at eye_l and its mirror}.
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bmesh  # noqa: E402
import bpy  # noqa: E402
import numpy as np  # noqa: E402
from mathutils import Vector  # noqa: E402
from mathutils.bvhtree import BVHTree  # noqa: E402

import make_leads as ml  # noqa: E402
import ne_blender as nb  # noqa: E402
import rig_common as rc  # noqa: E402

EYE = {"r": 0.0125, "half_w": 0.0112, "up": 0.0050, "down": 0.0044, "tilt": 3.0, "iris": (0.11, 0.065, 0.04)}
LID_GAP = 0.0012       # lid margin radius over the eyeball radius
INNER_GAP = 0.0004     # the rows under the lid
SEGS = 48              # vertices per ring
# Rings, innermost first. ("ang", degrees outward from the margin, radius over the eyeball, f): a row round the
# eyeball. ("flat", millimetres outward from the margin seen from the front, blend from the lid onto the scan, f):
# a row on the face. f is how much of the margin's blink the row follows.
RINGS = [("ang", 22.0, INNER_GAP * 0.5, 1.0), ("ang", 10.0, INNER_GAP, 1.0), ("ang", 0.0, INNER_GAP, 1.0),
         ("ang", 0.0, LID_GAP, 1.0), ("ang", 3.5, LID_GAP + 0.0005, 1.0), ("flat", 1.5, 0.25, 0.8),
         ("flat", 3.2, 0.6, 0.4), ("flat", 5.2, 0.9, 0.12), ("flat", 7.5, 1.0, 0.0)]
LATERAL = 0.45         # the face rows come this much closer on the ear side, where the face turns away
DELETE_DEPTH = 0.03    # the cut takes faces up to this far in front of the eye's surface (hair over the eye stays)
RAY_START = 0.03       # front rays start this far in front of the eye's surface
LID_SAMPLE = 0.0025    # lid rows take the scan's texture and shape deltas this far outside the margin
EYE_KEYS = {"blink": "eyeBlink%s", "wide": "eyeWide%s", "squint": "eyeSquint%s"}
FADE = 0.010           # the scan's own eye-key deltas fade out this close to the patch
COLORS = {"sclera": (0.80, 0.77, 0.72), "pupil": (0.004, 0.004, 0.004), "lash": (0.025, 0.018, 0.015)}
PUPIL_DEG, IRIS_DEG, LIMBUS_DEG = 9.0, 27.0, 31.0
CORNEA = 0.0006        # the cornea bulges this far out of the eyeball at the pupil
LASH_RISE = 0.0015       # the lash line covers upper-margin columns this far above the eye centre
BAKE_SIZE = 2048       # the face's new texture
MASK_MM = 6.0          # the scan's painted eye reaches this far past the new lid margin
MASK_MEDIAL = 15.0     # and this much further toward the nose


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    a = {"face": None, "out": None, "blend": None, "preview": None}
    i = 0
    while i < len(argv):
        k = argv[i][2:]
        if not argv[i].startswith("--") or k not in a or i + 1 >= len(argv):
            raise SystemExit("face_patch: unknown argument %s" % argv[i])
        a[k] = argv[i + 1]
        i += 2
    if not (a["face"] and a["out"]):
        raise SystemExit(__doc__)
    return a


def _material(name, rgb, rough=0.5):
    mat, _nt, bsdf = nb.new_material(name)
    bsdf.inputs["Base Color"].default_value = tuple(rgb) + (1.0,)
    bsdf.inputs["Roughness"].default_value = rough
    mat.diffuse_color = tuple(rgb) + (1.0,)
    return mat


class Scan:
    """The original surface: positions, faces, per-face-corner UVs, materials, weights and shape deltas."""

    def __init__(self, mesh):
        me = mesh.data
        self.V = np.array([v.co[:] for v in me.vertices])
        self.F = [tuple(p.vertices) for p in me.polygons]
        self.bvh = BVHTree.FromPolygons([tuple(p) for p in self.V], self.F)
        uvl = me.uv_layers.active.data
        self.fuv = [[uvl[li].uv[:] for li in p.loop_indices] for p in me.polygons]
        self.fmat = [p.material_index for p in me.polygons]
        self.keys = [kb.name for kb in me.shape_keys.key_blocks]
        self.delta = {}
        for kb in me.shape_keys.key_blocks[1:]:
            co = np.empty(len(self.V) * 3)
            kb.data.foreach_get("co", co)
            self.delta[kb.name] = co.reshape(-1, 3) - self.V
        self.groups = [[(g.group, g.weight) for g in v.groups] for v in me.vertices]

    def bary(self, fi, p):
        """Barycentric weights of p (on face fi) over the face's first three corners."""
        a, b, c = (self.V[i] for i in self.F[fi][:3])
        v0, v1, v2 = b - a, c - a, np.asarray(p) - a
        d00, d01, d11, d20, d21 = v0 @ v0, v0 @ v1, v1 @ v1, v2 @ v0, v2 @ v1
        den = d00 * d11 - d01 * d01
        if abs(den) < 1e-18:
            return np.array([1.0, 0.0, 0.0])
        v = (d11 * d20 - d01 * d21) / den
        w = (d00 * d21 - d01 * d20) / den
        return np.clip(np.array([1.0 - v - w, v, w]), 0.0, 1.0)

    def front(self, x, z, y0):
        """First scan hit looking along +y from (x, y0, z): (point, face) or (None, None)."""
        hit = self.bvh.ray_cast(Vector((x, y0, z)), Vector((0.0, 1.0, 0.0)), 1.0)
        if hit[0] is None:
            return None, None
        return np.array(hit[0][:]), hit[2]

    def sample(self, fi, p):
        """(uv, material, weights, {key: delta}) of the scan at p on face fi."""
        w = self.bary(fi, p)
        corners = self.F[fi][:3]
        uv = sum(w[k] * np.array(self.fuv[fi][k]) for k in range(3))
        delta = {n: sum(w[k] * d[corners[k]] for k in range(3)) for n, d in self.delta.items()}
        near = corners[int(np.argmax(w))]
        return uv, self.fmat[fi], self.groups[near], delta


# ------------------------------------------------------------------------------------------------------------
# The lid layout, in (psi, e) degrees round the eyeball centre: psi lateral (toward the ear), e up.
# ------------------------------------------------------------------------------------------------------------
def _dir(side, psi, e):
    psi, e = math.radians(psi), math.radians(e)
    return np.array([side * math.sin(psi) * math.cos(e), -math.cos(psi) * math.cos(e), math.sin(e)])


def margin_layout(cfg):
    """SEGS (psi, e) points round the lid margin from the lateral corner: upper lid first (0 < j < SEGS/2), the
    medial corner at SEGS/2, then the lower lid. Returns the points, their outward unit normals in (psi, e), and
    the corner angle."""
    rl = cfg["r"] + LID_GAP
    pmax = math.degrees(math.asin(min(0.99, cfg["half_w"] / rl)))
    eu = math.degrees(math.asin(cfg["up"] / rl))
    el = math.degrees(math.asin(cfg["down"] / rl))
    half = SEGS // 2
    P = []
    for j in range(SEGS):
        upper = j < half
        t = j / half if upper else (j - half) / half
        psi = pmax * math.cos(math.pi * t) if upper else -pmax * math.cos(math.pi * t)
        u = psi / pmax
        mid = cfg["tilt"] * u  # the corner line: lateral corner higher
        bump = max(0.0, 1.0 - u * u) ** 0.8
        if upper:  # the upper lid peaks a little toward the nose, the lower a little toward the ear
            e = mid + eu * bump * (1.0 - 0.12 * u)
        else:
            e = mid - el * bump * (1.0 + 0.15 * u)
        P.append((psi, e))
    P = np.array(P)
    N = []
    for j in range(SEGS):
        t = P[(j + 1) % SEGS] - P[(j - 1) % SEGS]
        n = np.array([t[1], -t[0]])
        N.append(n / (np.linalg.norm(n) + 1e-12))
    N = np.array(N)
    if np.mean(np.sum((P - P.mean(axis=0)) * N, axis=1)) < 0:
        N = -N
    return P, N, pmax


# ------------------------------------------------------------------------------------------------------------
def build_eye(bm, L, scan, side, centre_xz, cfg, mats, eye_group):
    """Adds one eye's lids and eyeball to bm. Returns a report."""
    cx, cz = centre_xz
    hit = scan.front(cx, cz, -1.0)[0]
    if hit is None:
        raise SystemExit("face_patch: nothing in front of the eye at %.3f, %.3f" % (cx, cz))
    sy = hit[1]
    y0 = sy - RAY_START
    R = cfg["r"]
    rl = R + LID_GAP
    E = np.array([cx, sy + 0.0005 + R, cz])
    P, N, pmax = margin_layout(cfg)
    half = SEGS // 2
    # blink: the margins meet on a line 30% of the way up from the lower lid, column by column
    meet = []
    for j in range(SEGS):
        jj = SEGS - j if j else 0  # the column on the other lid with the same psi
        lo, hi = (P[jj][1], P[j][1]) if j < half else (P[j][1], P[jj][1])
        meet.append(lo + 0.3 * (hi - lo))
    M3 = np.array([E + rl * _dir(side, P[j][0], P[j][1]) for j in range(SEGS)])
    m2 = M3[:, [0, 2]]
    c2 = m2.mean(axis=0)
    n2 = []
    for j in range(SEGS):
        t = m2[(j + 1) % SEGS] - m2[(j - 1) % SEGS]
        n = np.array([t[1], -t[0]])
        n /= np.linalg.norm(n) + 1e-12
        n2.append(n if np.dot(n, m2[j] - c2) >= 0 else -n)
    n2 = np.array(n2)
    # millimetres of offset per millimetre asked for, round the margin: less toward the ear
    squeeze = np.array([1.0 - LATERAL * max(0.0, side * n2[j][0]) for j in range(SEGS)])

    def lid_y(x, z, yscan, blend):
        d2 = (x - E[0]) ** 2 + (z - E[2]) ** 2
        y = yscan
        if d2 < rl * rl:
            ys = E[1] - math.sqrt(rl * rl - d2)
            y = ys + blend * (yscan - ys) if yscan is not None else ys
        if y is None:
            return E[1] - rl
        rc_ = R + 0.0006  # never inside the eyeball
        if d2 < rc_ * rc_:
            y = min(y, E[1] - math.sqrt(rc_ * rc_ - d2))
        return y

    def pos(k, j, de=0.0):
        kind, off, a, f = RINGS[k]
        if kind == "ang":
            psi, e = P[j] + off * N[j]
            psi = max(-pmax - 10.0, min(pmax + 10.0, psi))  # the rows under the lid stop short of the eyeball's side
            return E + (R + a) * _dir(side, psi, e + de * f)
        dz = (E + rl * _dir(side, P[j][0], P[j][1] + de))[2] - M3[j][2] if de else 0.0
        x, z = m2[j] + 0.001 * off * squeeze[j] * n2[j] + np.array([0.0, f * dz])
        h = scan.front(x, z, y0)[0]
        return np.array([x, lid_y(x, z, h[1] if h is not None else None, a), z])

    # removal region: the front surface inside the outermost ring, seen from the front
    poly = [tuple(m2[j] + 0.001 * RINGS[-1][1] * squeeze[j] * n2[j]) for j in range(SEGS)]

    def inside(x, z):
        return _inside(poly, x, z)

    # every front face touching the inside of the outer ring goes, so the hole's jagged edge lies wholly outside
    # the ring and the stitch band between them cannot fold
    gone = []
    for f in bm.faces:
        c = f.calc_center_median()
        if abs(c.x - cx) > 0.04 or abs(c.z - cz) > 0.03 or c.y > E[1] + 0.003:
            continue
        if not any(inside(v.co.x, v.co.z) for v in f.verts):
            continue
        if c.y > sy - DELETE_DEPTH:
            gone.append(f)
    rim = {v for f in gone for v in f.verts}
    bmesh.ops.delete(bm, geom=gone, context="FACES")
    rim = {v for v in rim if v.is_valid}
    loops = _boundary_loops(rim)
    if not loops:
        raise SystemExit("face_patch: no hole boundary found round the eye (%d removed)" % len(gone))
    # a scan whose eye sits in a socket leaves a second hole edge deep in it, hidden behind the eyeball: of the
    # long edges, the front one is the face's
    big = [lp for lp in loops if len(lp) >= 0.4 * max(len(x) for x in loops)]
    loop = min(big, key=lambda lp: sum(v.co.y for v in lp) / len(lp))
    if os.environ.get("FP_DEBUG"):
        for lp in loops:
            c = np.mean([v.co[:] for v in lp], axis=0)
            print("face_patch debug: hole loop %d verts at %.4f %.4f %.4f" % (len(lp), c[0], c[1], c[2]))
    _relax(loop, scan, L["shapes"])

    # new vertices, ring by ring, each with the texture, weights and shape deltas of the scan it stands for
    deform, uvl, shapes = L["deform"], L["uv"], L["shapes"]
    rings, info = [], []
    for k in range(len(RINGS)):
        kind, off = RINGS[k][0], RINGS[k][1]
        row = []
        for j in range(SEGS):
            p = pos(k, j)
            soff = LID_SAMPLE if kind == "ang" else max(LID_SAMPLE, 0.001 * off * squeeze[j])
            sx, sz = m2[j] + soff * n2[j]
            sp, sfi = scan.front(sx, sz, y0)
            if sfi is None:
                loc, _n, sfi, _d = scan.bvh.find_nearest(Vector(p))
                sp = np.array(loc[:])
            uv, mi, groups, delta = scan.sample(sfi, sp)
            v = bm.verts.new(p)
            for g, w in groups:
                v[deform][g] = w
            row.append(v)
            info.append((v, k, j, delta, uv, mi))
        rings.append(row)
    vinfo = {x[0]: x for x in info}

    new_faces = []
    for k in range(len(RINGS) - 1):
        for j in range(SEGS):
            j2 = (j + 1) % SEGS
            new_faces.append(bm.faces.new((rings[k][j], rings[k][j2], rings[k + 1][j2], rings[k + 1][j])))
    outer = rings[-1]
    stitch = _zipper(outer, loop, c2)
    for tri in stitch:
        try:
            new_faces.append(bm.faces.new(tri))
        except ValueError:
            pass
    # orient: the rings and the stitch share one winding; match it to the scan faces across the stitch
    flip = _stitch_flip(stitch, set(loop), set(new_faces))
    if flip is None:
        votes = []
        for f in new_faces[(len(RINGS) - 3) * SEGS:(len(RINGS) - 2) * SEGS]:
            f.normal_update()
            votes.append(f.normal.y > 0)
        flip = sum(votes) > len(votes) / 2
    loop_set = set(loop)
    for f in new_faces:
        if flip:
            f.normal_flip()
        f.normal_update()
        f.smooth = True
        mis = [vinfo[v][5] for v in f.verts if v in vinfo]
        f.material_index = max(set(mis), key=mis.count) if mis else 0
        for lo in f.loops:
            if lo.vert in vinfo:
                lo[uvl].uv = vinfo[lo.vert][4]
            elif lo.vert in loop_set:
                lo[uvl].uv = _vert_uv(lo.vert, uvl)
    # lash line: the upper lid's margin and its front edge, short of the corners
    for k in (2, 3):
        for j in range(SEGS):
            if rings[3][j].co.z > cz + LASH_RISE and rings[3][(j + 1) % SEGS].co.z > cz + LASH_RISE:
                new_faces[k * SEGS + j].material_index = mats["lash"]
    # shape keys: the scan's deltas, then the rebuilt eye keys
    for v, k, j, delta, _uv, _mi in info:
        for name, layer in shapes.items():
            v[layer] = v.co if name == L["basis"] else v.co + Vector(delta.get(name, (0.0, 0.0, 0.0)))
    sname = "Left" if side > 0 else "Right"
    for kind, fmt in EYE_KEYS.items():
        layer = shapes.get(fmt % sname)
        if layer is None:
            continue
        for v, k, j, _d, _uv, _mi in info:
            upper, lower = 0 < j < half, j > half
            gap = meet[j] - P[j][1]
            de = 0.0
            if kind == "blink":
                de = gap
            elif kind == "wide":
                de = 5.0 if upper else (-1.5 if lower else 0.0)
            elif kind == "squint":
                de = 0.45 * gap if lower else (0.12 * gap if upper else 0.0)
            v[layer] = Vector(pos(k, j, de)) if de else v.co
    # the scan's own eye-key deltas fade out toward the patch
    ring_pts = np.array([v.co[:] for v in outer])
    for fmt in EYE_KEYS.values():
        layer = shapes.get(fmt % sname)
        if layer is None:
            continue
        for v in bm.verts:
            if v in vinfo or abs(v.co.x - cx) > 0.05 or abs(v.co.z - cz) > 0.05:
                continue
            dmin = float(np.min(np.linalg.norm(ring_pts - np.array(v.co[:]), axis=1)))
            if dmin < FADE:
                v[layer] = v.co + (v[layer] - v.co) * (dmin / FADE)

    back = [0] * len(RINGS)
    for i, f in enumerate(new_faces):
        if f.is_valid and f.normal.y > 0.2:
            back[min(i // SEGS, len(RINGS) - 1)] += 1
    if os.environ.get("FP_DEBUG"):
        print("face_patch debug: back-facing per band (last = stitch):", back)
    neye = _eyeball(bm, L, E, R, mats, eye_group)
    mask = [tuple(m2[j] + 0.001 * (cfg.get("mask_mm", MASK_MM) + MASK_MEDIAL * max(0.0, -side * n2[j][0])) * n2[j])
            for j in range(SEGS)]
    return {"centre": [round(float(c), 4) for c in E], "removed": len(gone), "hole_edge": len(loop),
            "_mask": (mask, sy - DELETE_DEPTH),
            "loops": len(loops), "stitch": len(stitch), "eyeball_verts": neye, "pmax": round(pmax, 1)}


def _inside(poly, x, z):
    n, ins = len(poly), False
    for i in range(n):
        (x1, y1), (x2, y2) = poly[i], poly[(i + 1) % n]
        if (y1 > z) != (y2 > z) and x < x1 + (z - y1) * (x2 - x1) / (y2 - y1):
            ins = not ins
    return ins


def _boundary_loops(rim):
    """Closed loops of open edges among rim vertices, walked face by face (bow-tie vertices are fine)."""
    starts = [lo for v in rim for e in v.link_edges if e.is_boundary and e.other_vert(v) in rim
              for lo in e.link_loops if lo.vert is v]
    used, loops = set(), []
    for lo in starts:
        if lo in used:
            continue
        chain, cur = [], lo
        for _ in range(50000):
            used.add(cur)
            chain.append(cur.vert)
            nxt = cur.link_loop_next
            for _ in range(64):
                if nxt.edge.is_boundary:
                    break
                nxt = nxt.link_loop_radial_next.link_loop_next
            cur = nxt
            if cur is lo:
                loops.append(chain)
                break
            if cur in used:
                break
    return loops


def _relax(loop, scan, shapes, passes=2, k=0.5):
    """Smooths the hole's jagged edge along itself and puts it back on the scan, so the stitch does not fold."""
    for _ in range(passes):
        n = len(loop)
        target = [(loop[i - 1].co + loop[(i + 1) % n].co) * 0.5 for i in range(n)]
        for v, t in zip(loop, target):
            p = scan.bvh.find_nearest(v.co.lerp(t, k))[0]
            d = p - v.co
            v.co = p
            for layer in shapes.values():
                v[layer] = v[layer] + d


def _angle2(v, c2):
    return math.atan2(v.co.z - c2[1], v.co.x - c2[0])


def _zipper(ring, loop, c2):
    """Triangles joining two closed loops round the same centre (seen from the front), matched by angle."""
    ra = [_angle2(v, c2) for v in ring]
    la = [_angle2(v, c2) for v in loop]

    def winding(a):
        return sum((a[(i + 1) % len(a)] - a[i] + math.pi) % (2 * math.pi) - math.pi for i in range(len(a)))
    if winding(ra) * winding(la) < 0:
        loop, la = loop[::-1], la[::-1]
    sgn = 1.0 if winding(ra) > 0 else -1.0
    # start both at the angle nearest the ring's first vertex, unwrap monotonically
    i0 = int(np.argmin([abs((a - ra[0] + math.pi) % (2 * math.pi) - math.pi) for a in la]))
    loop, la = loop[i0:] + loop[:i0], la[i0:] + la[:i0]

    def unwrap(a):
        out, acc = [0.0], 0.0
        for i in range(1, len(a)):
            acc += max(sgn * ((a[i] - a[i - 1] + math.pi) % (2 * math.pi) - math.pi), 0.0)
            out.append(acc)
        total = acc + max(0.0, sgn * ((a[0] - a[-1] + math.pi) % (2 * math.pi) - math.pi))
        return [x / total for x in out] + [1.0]
    ru, lu = unwrap(ra), unwrap(la)
    nr, nl = len(ring), len(loop)
    i = j = 0
    tris = []
    while i < nr or j < nl:
        a, b = ring[i % nr], loop[j % nl]
        if j >= nl or (i < nr and ru[i + 1] <= lu[j + 1]):
            tris.append((a, ring[(i + 1) % nr], b))
            i += 1
        else:
            tris.append((a, loop[(j + 1) % nl], b))
            j += 1
    return [t for t in tris if len(set(t)) == 3]


def _stitch_flip(stitch, loop_set, new_set):
    """True when the stitch runs the hole's edges the same way as the scan faces beside them (so must flip),
    False when opposite, None when no edge could be checked."""
    same = opp = 0
    for tri in stitch:
        for i in range(3):
            a, b = tri[i], tri[(i + 1) % 3]
            if a not in loop_set or b not in loop_set:
                continue
            e = next((e for e in a.link_edges if e.other_vert(a) is b), None)
            if e is None:
                continue
            for f in e.link_faces:
                if f in new_set:
                    continue
                vs = list(f.verts)
                if vs[(vs.index(a) + 1) % len(vs)] is b:
                    same += 1
                else:
                    opp += 1
    if same == opp == 0:
        return None
    return same > opp


def _vert_uv(v, uvl):
    for lo in v.link_loops:
        return lo[uvl].uv.copy()
    return (0.0, 0.0)


def _eyeball(bm, L, E, R, mats, eye_group):
    deform, uvl, shapes = L["deform"], L["uv"], L["shapes"]
    rings = [0.0, 4.0, PUPIL_DEG, 14.0, 20.0, IRIS_DEG, LIMBUS_DEG, 40.0, 55.0, 70.0, 90.0, 110.0, 135.0, 160.0, 180.0]
    segs = 32
    verts = []
    for a in rings:
        ar = math.radians(a)
        bulge = CORNEA * max(0.0, math.cos(math.pi * 0.5 * min(1.0, a / LIMBUS_DEG))) ** 2
        row = []
        for i in range(1 if a in (0.0, 180.0) else segs):
            b = 2 * math.pi * i / segs
            d = np.array([math.sin(ar) * math.cos(b), -math.cos(ar), math.sin(ar) * math.sin(b)])
            v = bm.verts.new(E + d * (R + bulge))
            v[deform][eye_group] = 1.0
            for layer in shapes.values():
                v[layer] = v.co
            row.append(v)
        verts.append(row)

    def mat(a):
        return mats["pupil"] if a < PUPIL_DEG else (mats["iris"] if a < IRIS_DEG else mats["sclera"])
    faces = []
    for r in range(len(rings) - 1):
        A, B = verts[r], verts[r + 1]
        m = mat(0.5 * (rings[r] + rings[r + 1]))
        for i in range(segs):
            i2 = (i + 1) % segs
            if len(A) == 1:
                f = bm.faces.new((A[0], B[i2], B[i]))
            elif len(B) == 1:
                f = bm.faces.new((A[i], A[i2], B[0]))
            else:
                f = bm.faces.new((A[i], A[i2], B[i2], B[i]))
            f.material_index = m
            f.smooth = True
            faces.append(f)
    for f in faces:
        f.normal_update()
        if np.dot(np.array(f.normal[:]), np.array(f.calc_center_median()[:]) - E) < 0:
            f.normal_flip()
        for lo in f.loops:  # a planar projection from the front: the iris is a disc in UV space
            d = (np.array(lo.vert.co[:]) - E) / R
            lo[uvl].uv = (0.5 + 0.5 * d[0], 0.5 + 0.5 * d[2])
    return sum(len(r) for r in verts)


# ------------------------------------------------------------------------------------------------------------
# The face's own texture. The patch's vertices sample the scan's texture atlas, and a triangle whose corners fall
# in different atlas islands smears across the seams between them. So the region round the patches gets its own
# UVs and a texture baked from a copy of the unpatched scan, with the painted eyes cut out of the copy: texels
# whose rays find nothing there are filled from the skin round them.
# ------------------------------------------------------------------------------------------------------------
def _fill(rgb, w, smooth=6):
    """Push-pull fill: texels with weight 0 take the colour of their surroundings, blended coarse to fine by how
    much of each coarse texel was covered, then softened where nothing was baked."""
    levels = [(rgb * w[..., None], w)]
    while levels[-1][1].shape[0] > 1:
        c, ww = levels[-1]
        h = c.shape[0] // 2
        levels.append((c.reshape(h, 2, h, 2, 3).mean(axis=(1, 3)), ww.reshape(h, 2, h, 2).mean(axis=(1, 3))))
    col = levels[-1][0] / np.maximum(levels[-1][1], 1e-9)[..., None]
    for c, ww in reversed(levels[:-1]):
        up = col.repeat(2, axis=0).repeat(2, axis=1)
        a = np.clip(2.0 * ww, 0.0, 1.0)[..., None]
        col = c / np.maximum(ww, 1e-9)[..., None] * a + up * (1.0 - a)
    hole = w < 0.5
    for _ in range(smooth):
        blur = (col + np.roll(col, 1, 0) + np.roll(col, -1, 0) + np.roll(col, 1, 1) + np.roll(col, -1, 1)) / 5.0
        col[hole] = blur[hole]
    return col


def _skin_fill(src, masks, dark=0.75, pale=1.3, power=2.0):
    """Makes a material for src's faces inside masks whose colour is the skin round the mask, spread inward per
    vertex by inverse distance (dark samples, liner and brow paint, and pale ones, sclera paint, are left out). Returns it and the faces."""
    me = src.data
    gone = [p.index for p in me.polygons
            if any(p.center.y > ymin and _inside(poly, p.center.x, p.center.z) for poly, ymin in masks)]
    gone_set = set(gone)
    inner = {v for fi in gone for v in me.polygons[fi].vertices}
    images = {}
    uvl = me.uv_layers.active.data
    edge = {}
    for p in me.polygons:
        if p.index in gone_set:
            continue
        for v, li in zip(p.vertices, p.loop_indices):
            if v not in inner or v in edge:
                continue
            mat = me.materials[p.material_index]
            node = next((n for n in mat.node_tree.nodes if n.type == "TEX_IMAGE" and n.image), None) if mat else None
            if node is None:
                continue
            im = node.image
            if im.name not in images:
                a = np.empty(im.size[0] * im.size[1] * 4, dtype=np.float32)
                im.pixels.foreach_get(a)
                images[im.name] = a.reshape(im.size[1], im.size[0], 4)
            a = images[im.name]
            u, w = uvl[li].uv
            edge[v] = a[int(np.clip(w, 0, 1) * (a.shape[0] - 1)), int(np.clip(u, 0, 1) * (a.shape[1] - 1)), :3]
    ev = np.array(list(edge.keys()))
    ec = np.array(list(edge.values()), dtype=np.float64)
    lum = ec @ np.array([0.3, 0.59, 0.11])
    good = (lum >= dark * np.median(lum)) & (lum <= pale * np.median(lum))
    ev, ec = ev[good], ec[good]
    # image pixels hold sRGB values, while a float colour attribute is linear
    ec = np.where(ec <= 0.04045, ec / 12.92, ((ec + 0.055) / 1.055) ** 2.4)
    kept = dict(zip(ev.tolist(), ec))
    ep = np.array([me.vertices[v].co[:] for v in ev])
    attr = me.color_attributes.new("skin_fill", "FLOAT_COLOR", "POINT")
    for v in inner:
        if v in kept:
            c = kept[v]
        else:
            d = np.linalg.norm(ep - np.array(me.vertices[v].co[:]), axis=1)
            wt = 1.0 / np.maximum(d, 1e-4) ** power
            c = (ec * wt[:, None]).sum(axis=0) / wt.sum()
        attr.data[v].color = (float(c[0]), float(c[1]), float(c[2]), 1.0)
    mat = bpy.data.materials.new("skin_fill")
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    emit = nt.nodes.new("ShaderNodeEmission")
    col = nt.nodes.new("ShaderNodeVertexColor")
    col.layer_name = "skin_fill"
    nt.links.new(col.outputs["Color"], emit.inputs["Color"])
    nt.links.new(emit.outputs["Emission"], out.inputs["Surface"])
    return mat, gone


def bake_face(mesh, src, boxes, masks, skip_mats, size, outdir):
    """Gives mesh's faces inside boxes ((x0, x1, z0, z1, ymax) each) new UVs and a material with a texture baked
    from src (the unpatched scan); src faces inside masks ((polygon in x, z, ymin) each) show plain skin instead."""
    fill_mat, gone = _skin_fill(src, masks)
    me = mesh.data
    faces = [p.index for p in me.polygons if p.material_index not in skip_mats and
             any(x0 < p.center.x < x1 and z0 < p.center.z < z1 and p.center.y < ymax for x0, x1, z0, z1, ymax in boxes)]
    used = sorted({v for fi in faces for v in me.polygons[fi].vertices})
    remap = {v: i for i, v in enumerate(used)}
    tmp = nb.mesh_object("face_bake_target", [me.vertices[v].co[:] for v in used],
                         [[remap[v] for v in me.polygons[fi].vertices] for fi in faces])
    tmp.data.uv_layers.new(name="UVMap")
    nb.activate(tmp)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(70), island_margin=0.003, area_weight=0.0)
    bpy.ops.uv.pack_islands(rotate=True, margin=0.003)
    bpy.ops.object.mode_set(mode="OBJECT")

    im = bpy.data.images.new("face_bake", size, size, alpha=True)
    im.pixels.foreach_set(np.zeros(size * size * 4, dtype=np.float32))
    mat, nt, bsdf = nb.new_material("face_bake")
    tex = nb.image_node(nt, im, extension="EXTEND")
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = 0.55
    nt.nodes.active = tex
    tmp.data.materials.append(mat)
    # the source shows its base colour unlit: an emission shader fed from its image, put back afterwards
    rewired = []
    for sm in src.data.materials:
        if sm is None or not sm.node_tree:
            continue
        snt = sm.node_tree
        out = next(n for n in snt.nodes if n.type == "OUTPUT_MATERIAL")
        keep = out.inputs["Surface"].links[0].from_socket if out.inputs["Surface"].links else None
        emit = snt.nodes.new("ShaderNodeEmission")
        b = next((n for n in snt.nodes if n.type == "BSDF_PRINCIPLED"), None)
        link = b.inputs["Base Color"].links[0] if b and b.inputs["Base Color"].links else None
        if link:
            snt.links.new(link.from_socket, emit.inputs["Color"])
        elif b:
            emit.inputs["Color"].default_value = b.inputs["Base Color"].default_value
        snt.links.new(emit.outputs["Emission"], out.inputs["Surface"])
        rewired.append((snt, out, keep, emit))
    fnt = fill_mat.node_tree
    rewired.append((fnt, next(n for n in fnt.nodes if n.type == "OUTPUT_MATERIAL"), None,
                    next(n for n in fnt.nodes if n.type == "EMISSION")))
    src.data.materials.append(fill_mat)
    slot = len(src.data.materials) - 1
    for fi in gone:
        src.data.polygons[fi].material_index = slot
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    sc.cycles.device = "CPU"
    sc.cycles.samples = 1
    nb.activate(tmp)
    src.select_set(True)
    bpy.ops.object.bake(type="EMIT", use_selected_to_active=True, cage_extrusion=0.004, max_ray_distance=0.012,
                        margin=0)
    px = np.empty(size * size * 4, dtype=np.float32)
    im.pixels.foreach_get(px)
    # a ray that finds nothing writes black too, so a second pass with the source all white marks the hits
    cov = bpy.data.images.new("face_bake_cover", size, size, alpha=False)
    cov.pixels.foreach_set(np.zeros(size * size * 4, dtype=np.float32))
    tex.image = cov
    for snt, out, keep, emit in rewired:
        for lk in list(emit.inputs["Color"].links):
            snt.links.remove(lk)
        emit.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    bpy.ops.object.bake(type="EMIT", use_selected_to_active=True, cage_extrusion=0.004, max_ray_distance=0.012,
                        margin=0)
    cp = np.empty(size * size * 4, dtype=np.float32)
    cov.pixels.foreach_get(cp)
    tex.image = im
    bpy.data.images.remove(cov)
    for snt, out, keep, emit in rewired:
        if keep is not None:
            snt.links.new(keep, out.inputs["Surface"])
        snt.nodes.remove(emit)
    px = px.reshape(size, size, 4)
    w = (cp.reshape(size, size, 4)[..., 0] > 0.5).astype(np.float64)
    filled = np.ones((size, size, 4), dtype=np.float32)
    filled[..., :3] = _fill(px[..., :3].astype(np.float64), w)
    im.pixels.foreach_set(filled.ravel())
    if outdir:
        os.makedirs(outdir, exist_ok=True)
        im.filepath_raw = os.path.join(outdir, "face_bake.png")
        im.file_format = "PNG"
        im.save()
    im.pack()

    slot = len(me.materials)
    me.materials.append(mat)
    uvs, tuv = me.uv_layers.active.data, tmp.data.uv_layers.active.data
    for i, fi in enumerate(faces):
        p, tp = me.polygons[fi], tmp.data.polygons[i]
        p.material_index = slot
        for k, li in enumerate(p.loop_indices):
            uvs[li].uv = tuv[tp.loop_start + k].uv
    bpy.data.objects.remove(tmp, do_unlink=True)
    return {"faces": len(faces), "source_cut": len(gone), "hit": round(float(w.mean()), 3)}


def move_eye_joint(arm, joint, E):
    """Puts the eye joint at the eyeball centre (the eyeball turns about it), keeping its direction and length."""
    nb.activate(arm)
    bpy.ops.object.mode_set(mode="EDIT")
    eb = arm.data.edit_bones[joint]
    shift = arm.matrix_world.inverted() @ Vector(E) - eb.head
    eb.head += shift
    eb.tail += shift
    bpy.ops.object.mode_set(mode="OBJECT")
    return shift.length


def preview(arm, mesh, outdir, face, eyes):
    os.makedirs(outdir, exist_ok=True)
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_WORKBENCH"
    sc.display.shading.light = "STUDIO"
    sc.render.resolution_x = sc.render.resolution_y = 768
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    cam.data.type = "ORTHO"
    sc.collection.objects.link(cam)
    sc.camera = cam
    keys = mesh.data.shape_keys.key_blocks
    ez = face["eye_l"][2]
    mz = face.get("jaw", {}).get("cut_z", face["mouth_l"][1])
    el = eyes["eye_l"]["centre"]

    def shoot(tag, at, scale, side=False):
        cam.data.ortho_scale = scale
        if side:
            cam.location, cam.rotation_euler = (at[0] + 2.0, at[1], at[2]), (math.radians(90), 0, math.radians(90))
        else:
            cam.location, cam.rotation_euler = (at[0], -2.0, at[2]), (math.radians(90), 0, 0)
        bpy.context.view_layer.update()
        for ct in ("TEXTURE", "SINGLE"):
            sc.display.shading.color_type = ct
            sc.render.filepath = os.path.join(outdir, "%s_%s.png" % (tag, "tex" if ct == "TEXTURE" else "solid"))
            bpy.ops.render.render(write_still=True)

    rc.reset_pose(arm)
    shoot("face", (0, 0, (ez + mz) / 2), 0.16)
    for amount in (0.0, 0.5, 1.0):
        keys["eyeBlinkLeft"].value = amount
        shoot("eye_l_blink%d" % int(amount * 100), (el[0], 0, el[2]), 0.05)
    keys["eyeBlinkLeft"].value = 0.0
    shoot("eye_l_side", (el[0], el[1], el[2]), 0.05, side=True)
    keys["eyeBlinkLeft"].value = keys["eyeBlinkRight"].value = 1.0
    shoot("face_blink", (0, 0, (ez + mz) / 2), 0.16)
    keys["eyeBlinkLeft"].value = keys["eyeBlinkRight"].value = 0.0
    keys["eyeWideLeft"].value = 1.0
    shoot("eye_l_wide", (el[0], 0, el[2]), 0.05)
    keys["eyeWideLeft"].value = 0.0
    for j in ("eye_l", "eye_r"):
        pb = arm.pose.bones[j]
        pb.rotation_mode = "XYZ"
        pb.rotation_euler = (0.0, 0.0, math.radians(20))
    shoot("face_look", (0, 0, (ez + mz) / 2), 0.16)
    rc.reset_pose(arm)
    bpy.context.view_layer.update()
    bpy.data.objects.remove(cam, do_unlink=True)


def main():
    a = _args()
    with open(a["face"]) as f:
        face = json.load(f)
    cfg = dict(EYE)
    cfg.update(face.get("eye", {}))
    arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
    mesh = next(o for o in bpy.data.objects if o.type == "MESH" and any(m.type == "ARMATURE" for m in o.modifiers))
    if not mesh.data.shape_keys:
        raise SystemExit("face_patch: the mesh has no face shapes (start from face_transfer's .blend)")
    mw = mesh.matrix_world
    if any(abs(mw[r][c] - (1.0 if r == c else 0.0)) > 1e-6 for r in range(4) for c in range(4)):
        raise SystemExit("face_patch: the mesh object has a transform; apply it first")
    if arm.animation_data:
        arm.animation_data.action = None
    rc.reset_pose(arm)
    for kb in mesh.data.shape_keys.key_blocks:
        kb.value = 0.0
    mesh.active_shape_key_index = 0
    bpy.context.view_layer.update()
    ex, _ey, ez = face["eye_l"]
    left = face.get("eye", {}).get("left", (abs(ex), ez))
    right = face.get("eye", {}).get("right", (-abs(ex), ez))
    scan = Scan(mesh)
    mats = {"sclera": _material("eye_sclera", COLORS["sclera"], 0.25),
            "iris": _material("eye_iris", tuple(cfg["iris"]), 0.25),
            "pupil": _material("eye_pupil", COLORS["pupil"], 0.1),
            "lash": _material("eye_lash", COLORS["lash"], 0.6)}
    for m in mats.values():
        mesh.data.materials.append(m)
    slots = {n: mesh.data.materials.find(m.name) for n, m in mats.items()}

    src = mesh.copy()
    src.data = mesh.data.copy()
    src.name = "face_bake_source"
    bpy.context.scene.collection.objects.link(src)
    src.modifiers.clear()
    src.shape_key_clear()

    bm = bmesh.new()
    bm.from_mesh(mesh.data)
    L = {"deform": bm.verts.layers.deform.verify(), "uv": bm.loops.layers.uv.active,
         "shapes": {n: bm.verts.layers.shape[n] for n in scan.keys}, "basis": scan.keys[0]}
    groups = {g.name: g.index for g in mesh.vertex_groups}
    for j in ("eye_l", "eye_r"):
        if j not in groups:
            groups[j] = mesh.vertex_groups.new(name=j).index
    report = {}
    for side, joint, xz in ((1, "eye_l", tuple(left)), (-1, "eye_r", tuple(right))):
        report[joint] = build_eye(bm, L, scan, side, xz, cfg, slots, groups[joint])
    bm.to_mesh(mesh.data)
    bm.free()
    mesh.data.update()
    masks = [report[j].pop("_mask") for j in ("eye_l", "eye_r")]
    el, er = report["eye_l"]["centre"], report["eye_r"]["centre"]
    boxes = [(er[0] - 0.03, el[0] + 0.03, ez - 0.03, ez + 0.024, max(el[1], er[1]) + 0.004)]
    report["bake"] = bake_face(mesh, src, boxes, masks, set(slots.values()), BAKE_SIZE,
                               os.path.dirname(os.path.abspath(a["out"])))
    src_data = src.data
    bpy.data.objects.remove(src, do_unlink=True)
    bpy.data.meshes.remove(src_data)
    for joint in ("eye_l", "eye_r"):
        if joint in arm.data.bones:
            report[joint]["joint_moved_mm"] = round(1000 * move_eye_joint(arm, joint, report[joint]["centre"]), 2)
    nb.activate(mesh)
    if a["preview"]:
        preview(arm, mesh, os.path.abspath(a["preview"]), face, report)
    rc.reset_pose(arm)
    bpy.context.scene.frame_set(0)
    bpy.context.view_layer.update()
    out = os.path.abspath(a["out"])
    os.makedirs(os.path.dirname(out), exist_ok=True)
    glb = nb.export_rig(out, {"export_morph_normal": False, "export_force_sampling": False})
    rc.write_rigexpect(arm, mesh, os.path.splitext(out)[0] + ".rigexpect.json")
    info = ml.check_glb(glb, face=True)
    if a["blend"]:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(a["blend"]))
    print("face_patch: %s, %s, %d bytes -> %s" % (info, json.dumps(report), os.path.getsize(glb), glb))


if __name__ == "__main__":
    main()
