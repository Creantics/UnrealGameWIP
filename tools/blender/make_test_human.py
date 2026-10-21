"""Builds Data/Models/test_human.glb: a plain full-body human on the canonical skeleton, for animation tests.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_test_human.py -- \\
        [--out Data/Models/test_human.glb] [--blend <path.blend>]

* Skeleton: the 87 canonical joints from rig_spec.py (1.8 m, A-pose, UE mannequin names), built the same way as
  for proof_head (rig_common.build_canonical_armature). The head matches proof_head's skull (rig_spec.HEAD_*).
* Body: lofted tubes along the joints (torso, neck, arms, legs, feet, palm, five fingers per hand) plus
  ellipsoids for the head, jaw, nose, ears, eyes and hair. The left side is built once and mirrored, so the mesh
  is exactly symmetric.
* Skin weights: procedural. Every ring of a tube knows its arc length along the joint path; the weight hands over
  from one joint to the next with a smoothstep around each joint (the make_character.py idea, generalised), then
  rig_common.weight_post_pass spreads the twist bands, mirrors, prunes and keeps 4 influences.
* Clips (LINEAR keys only, UE Interchange drops CUBICSPLINE): `idle` (2 s breathing loop), `walk` (in-place,
  ~1.07 s loop) and `rom` (the canonical range-of-motion clip, rig_spec.ROM_POSES).
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bpy  # noqa: E402
import numpy as np  # noqa: E402

import ne_blender as nb  # noqa: E402
import rig_common as rc  # noqa: E402
import rig_spec as rs  # noqa: E402

J = {r[0]: r for r in rs.JOINTS}


def head(n):
    return np.array(J[n][2], float)


def tail(n):
    return np.array(J[n][3], float)


def unit(v):
    v = np.asarray(v, float)
    return v / max(np.linalg.norm(v), 1e-12)


def smoothstep(x):
    x = np.clip(x, 0.0, 1.0)
    return x * x * (3.0 - 2.0 * x)


# Materials: index -> (name, base colour)
MATS = [("skin", (0.80, 0.58, 0.45)), ("shirt", (0.22, 0.38, 0.62)), ("shorts", (0.16, 0.16, 0.18)),
        ("hair", (0.18, 0.11, 0.07)), ("eye", (0.05, 0.05, 0.06)), ("shoe", (0.42, 0.30, 0.20))]
SKIN, SHIRT, SHORTS, HAIR, EYE, SHOE = range(len(MATS))


# ------------------------------------------------------------------------------------------------------------
# Geometry builder
# ------------------------------------------------------------------------------------------------------------
class Builder:
    def __init__(self):
        self.v, self.f, self.fm, self.w = [], [], [], []

    def loft(self, path, radii, ref, n=16, weights=None, mat=SKIN, mat_fn=None, cap0=True, cap1=True,
             expo=2.0, rings_per_m=120, min_rings=6, keep=None):
        """A tube along the polyline `path`. `radii(s, t)` -> (ru, rv) half-sizes of the cross-section at arc
        length s (t = s / total). The cross-section's u axis is `ref` made perpendicular to the path.
        `weights(s, p)` -> {bone: w}. Rounded (pole) caps at both ends unless disabled. `keep(s, ctr)` drops the
        side faces whose centre it rejects (slits, openings)."""
        path = [np.asarray(p, float) for p in path]
        seg = [np.linalg.norm(path[i + 1] - path[i]) for i in range(len(path) - 1)]
        cum = np.concatenate([[0.0], np.cumsum(seg)])
        total = cum[-1]
        m = max(min_rings, int(total * rings_per_m))
        ss = list(np.linspace(0.0, total, m + 1))

        def at(s):
            i = min(max(np.searchsorted(cum, s, side="right") - 1, 0), len(seg) - 1)
            t = (s - cum[i]) / max(seg[i], 1e-12)
            p = path[i] + (path[i + 1] - path[i]) * t
            # tangent: blend the neighbouring segment directions near the joints (smooth bends)
            d = unit(path[i + 1] - path[i])
            if i > 0 and t < 0.15:
                d = unit(d + unit(path[i] - path[i - 1]) * (0.15 - t) / 0.3)
            if i + 1 < len(seg) and t > 0.85:
                d = unit(d + unit(path[i + 2] - path[i + 1]) * (t - 0.85) / 0.3)
            return p, d

        rings = []  # (centre, u, v, ru, rv, s)
        for s in ss:
            p, d = at(s)
            u = unit(np.asarray(ref, float) - d * np.dot(ref, d))
            vv = np.cross(d, u)
            ru, rv = radii(s, s / total)
            rings.append((p, u, vv, ru, rv, s, d))
        k_cap = 4
        pre, post = [], []
        if cap0:
            p, u, vv, ru, rv, s, d = rings[0]
            r = 0.5 * (ru + rv)
            for j in range(k_cap, 0, -1):
                a = (math.pi / 2) * j / (k_cap + 1)
                pre.append((p - d * r * math.sin(a), u, vv, ru * math.cos(a), rv * math.cos(a), s - r * math.sin(a), d))
        if cap1:
            p, u, vv, ru, rv, s, d = rings[-1]
            r = 0.5 * (ru + rv)
            for j in range(1, k_cap + 1):
                a = (math.pi / 2) * j / (k_cap + 1)
                post.append((p + d * r * math.sin(a), u, vv, ru * math.cos(a), rv * math.cos(a), s + r * math.sin(a), d))
        rings = pre + rings + post
        base = len(self.v)
        e = 2.0 / expo
        for (p, u, vv, ru, rv, s, _d) in rings:
            for k in range(n):
                a = 2 * math.pi * k / n
                c, sn = math.cos(a), math.sin(a)
                q = p + u * ru * math.copysign(abs(c) ** e, c) + vv * rv * math.copysign(abs(sn) ** e, sn)
                self.v.append(q)
                self.w.append(weights(s, q) if weights else {})
        nr = len(rings)
        for i in range(nr - 1):
            for k in range(n):
                k2 = (k + 1) % n
                a0, a1 = base + i * n + k, base + i * n + k2
                b0, b1 = base + (i + 1) * n + k, base + (i + 1) * n + k2
                face = (a0, a1, b1, b0)
                ctr = sum(self.v[x] for x in face) / 4
                if keep is not None and not keep(rings[i][5], ctr):
                    continue
                self.f.append(face)
                self.fm.append(mat_fn(rings[i][5], ctr) if mat_fn else mat)
        for end, (p, u, vv, ru, rv, s, d) in ((0, rings[0]), (1, rings[-1])):
            if (end == 0 and not cap0) or (end == 1 and not cap1):
                continue
            r = 0.5 * (rings[0 if end == 0 else -1][3] + rings[0 if end == 0 else -1][4])
            # pole: the cap rings already shrank to cos(a) ~ 0.31; close with a centre vertex
            pole = p + d * (-1 if end == 0 else 1) * r * 0.25
            pi = len(self.v)
            self.v.append(pole)
            self.w.append(weights(s, pole) if weights else {})
            ring0 = base + (0 if end == 0 else (nr - 1) * n)
            for k in range(n):
                k2 = (k + 1) % n
                self.f.append((pi, ring0 + k2, ring0 + k) if end == 0 else (pi, ring0 + k, ring0 + k2))
                self.fm.append(mat_fn(s, pole) if mat_fn else mat)
        return base

    def ellipsoid(self, centre, radii, axes=None, n=24, rings=16, weights=None, mat=SKIN, keep=None):
        """Ellipsoid with semi-axes `radii` along `axes` (default world X, Y, Z). `keep(p)` drops faces whose
        centre fails it (for the hair cap)."""
        c = np.asarray(centre, float)
        ax = [np.asarray(a, float) for a in (axes or ((1, 0, 0), (0, 1, 0), (0, 0, 1)))]
        base = len(self.v)
        for i in range(1, rings):
            th = math.pi * i / rings
            for k in range(n):
                ph = 2 * math.pi * k / n
                p = c + ax[0] * radii[0] * math.sin(th) * math.cos(ph) + ax[1] * radii[1] * math.sin(th) * math.sin(ph) \
                    - ax[2] * radii[2] * math.cos(th)
                self.v.append(p)
                self.w.append(weights(p) if weights else {})
        bot = len(self.v)
        self.v.append(c - ax[2] * radii[2])
        self.w.append(weights(self.v[-1]) if weights else {})
        top = len(self.v)
        self.v.append(c + ax[2] * radii[2])
        self.w.append(weights(self.v[-1]) if weights else {})

        def add(face):
            ctr = sum(self.v[x] for x in face) / len(face)
            if keep is None or keep(ctr):
                self.f.append(face)
                self.fm.append(mat)
        for i in range(rings - 2):
            for k in range(n):
                k2 = (k + 1) % n
                a0, a1 = base + i * n + k, base + i * n + k2
                b0, b1 = base + (i + 1) * n + k, base + (i + 1) * n + k2
                add((a0, a1, b1, b0))
        for k in range(n):
            k2 = (k + 1) % n
            add((bot, base + k2, base + k))
            last = base + (rings - 2) * n
            add((top, last + k, last + k2))

    def append_mirrored(self, other):
        """Appends `other` as is and mirrored in x (left -> right names, winding flipped)."""
        for mirror in (False, True):
            base = len(self.v)
            for p, w in zip(other.v, other.w):
                self.v.append(np.array((-p[0], p[1], p[2])) if mirror else p)
                self.w.append({rs.mirror_name(b): x for b, x in w.items()} if mirror else dict(w))
            for face, m in zip(other.f, other.fm):
                face = tuple(base + i for i in face)
                self.f.append(tuple(reversed(face)) if mirror else face)
                self.fm.append(m)


def chain_weights(bounds, bones, blend):
    """Partition of unity along arc length: bones[i] owns s between bounds[i-1] and bounds[i], handing over with
    a smoothstep of half-width `blend` (a number or a list per bound)."""
    blends = blend if isinstance(blend, (list, tuple)) else [blend] * len(bounds)

    def fn(s, _p=None):
        f = [float(smoothstep((s - (b - h)) / (2 * h))) for b, h in zip(bounds, blends)]
        out = {}
        for i, bone in enumerate(bones):
            lo = f[i - 1] if i > 0 else 1.0
            hi = f[i] if i < len(f) else 0.0
            w = lo - hi
            if w > 1e-4:
                out[bone] = out.get(bone, 0.0) + w
        return out
    return fn


def arclen(points):
    return np.concatenate([[0.0], np.cumsum([np.linalg.norm(np.subtract(points[i + 1], points[i]))
                                             for i in range(len(points) - 1)])])


# ------------------------------------------------------------------------------------------------------------
# Body parts
# ------------------------------------------------------------------------------------------------------------
TORSO = [  # z, half width, half depth, centre y
    (0.872, 0.06, 0.045, 0.012), (0.89, 0.142, 0.098, 0.013), (0.91, 0.163, 0.108, 0.014), (0.96, 0.168, 0.108, 0.012),
    (1.02, 0.152, 0.098, 0.006), (1.08, 0.140, 0.092, 0.002), (1.14, 0.143, 0.095, 0.000), (1.22, 0.156, 0.102, -0.002),
    (1.30, 0.168, 0.108, -0.004), (1.36, 0.175, 0.106, -0.002), (1.41, 0.172, 0.096, 0.004), (1.445, 0.140, 0.080, 0.008),
    (1.47, 0.095, 0.064, 0.008), (1.485, 0.060, 0.054, 0.006),
]


def build_torso(B):
    zs = [r[0] for r in TORSO]
    path = [(0.0, r[3], r[0]) for r in TORSO]
    s_of_z = dict(zip(zs, arclen(path)))
    # spine bands by height
    spine = chain_weights([1.03, 1.17, 1.29, 1.465], ["pelvis", "spine_01", "spine_02", "spine_03", "neck_01"],
                          [0.04, 0.05, 0.05, 0.02])

    def radii(s, _t):
        z = np.interp(s, list(s_of_z.values()), zs)
        return (float(np.interp(z, zs, [r[1] for r in TORSO])), float(np.interp(z, zs, [r[2] for r in TORSO])))

    def weights(s, p):
        z = float(np.interp(s, list(s_of_z.values()), zs))
        w = spine(z)
        side = "_l" if p[0] >= 0 else "_r"
        ax = abs(p[0])
        # shoulder top / armpit follows the clavicle, the hips follow the thighs
        cl = float(smoothstep((ax - 0.06) / 0.10) * smoothstep((z - 1.33) / 0.09)) * 0.7
        th = float(smoothstep((ax - 0.03) / 0.08) * smoothstep((0.97 - z) / 0.10)) * 0.6
        out = {b: x * (1.0 - cl - th) for b, x in w.items()}
        if cl > 0:
            out["clavicle" + side] = out.get("clavicle" + side, 0.0) + cl
        if th > 0:
            out["thigh" + side] = out.get("thigh" + side, 0.0) + th
        return out

    def mat_fn(s, _p):
        z = float(np.interp(s, list(s_of_z.values()), zs))
        return SHIRT if z > 1.03 else SHORTS

    B.loft(path, radii, (1, 0, 0), n=36, weights=weights, mat_fn=mat_fn, expo=2.5, rings_per_m=110)


def build_neck_head(B):
    path = [(0.0, 0.008, 1.44), (0.0, 0.002, 1.53), (0.0, -0.008, 1.64)]
    neck = chain_weights([1.475, 1.52, 1.585], ["spine_03", "neck_01", "neck_02", "head"], [0.02, 0.03, 0.03])
    s0 = 1.44
    B.loft(path, lambda s, t: (0.050 - 0.004 * t, 0.054 - 0.006 * t), (1, 0, 0), n=20,
           weights=lambda s, p: neck(s0 + s), mat=SKIN, cap0=False, cap1=False, rings_per_m=150)
    hw = lambda p: {"head": 1.0}  # noqa: E731
    B.ellipsoid(rs.HEAD_CENTER, rs.HEAD_RADII, n=32, rings=24, weights=hw)
    B.ellipsoid((0.0, -0.040, 1.612), (0.060, 0.064, 0.048), n=24, rings=14, weights=hw)  # jaw / chin
    B.ellipsoid((0.0, -0.108, 1.668), (0.011, 0.018, 0.024), n=12, rings=8, weights=hw)  # nose
    B.ellipsoid((0.0, -0.098, 1.737), (0.060, 0.016, 0.018), n=16, rings=8, weights=hw)  # brow ridge
    hc, hr = rs.HEAD_CENTER, rs.HEAD_RADII
    B.ellipsoid((hc[0], hc[1] + 0.004, hc[2] + 0.010), (hr[0] + 0.006, hr[1] + 0.008, hr[2] + 0.002), n=32, rings=24,
                weights=hw, mat=HAIR, keep=lambda p: p[2] > 1.645 and (p[1] > -0.04 or p[2] > 1.762))


def build_left(L):
    """Everything on the character's left (+X); mirrored for the right."""
    hw = lambda p: {"head": 1.0}  # noqa: E731
    L.ellipsoid((0.079, 0.002, 1.690), (0.012, 0.020, 0.030), n=12, rings=8, weights=hw)  # ear
    L.ellipsoid((rs.EYE_X, rs.EYE_Y, rs.EYE_Z), (rs.EYE_R, rs.EYE_R, rs.EYE_R), n=12, rings=8,
                weights=lambda p: {"eye_l": 1.0}, mat=EYE)

    # Arm: shoulder -> elbow -> wrist
    sh, el, wr = head("upperarm_l"), head("lowerarm_l"), head("hand_l")
    d1 = unit(el - sh)
    start = sh - d1 * 0.045
    path = [start, sh, el, wr]
    S = arclen(path)
    prof_s = [0.0, S[1], S[1] + 0.06, S[1] + 0.16, S[2] - 0.02, S[2], S[2] + 0.06, S[2] + 0.16, S[3]]
    prof_r = [0.042, 0.056, 0.052, 0.047, 0.039, 0.039, 0.042, 0.034, 0.027]
    arm_w = chain_weights([S[1] - 0.025, S[2], S[3] - 0.01], ["clavicle_l", "upperarm_l", "lowerarm_l", "hand_l"],
                          [0.03, 0.035, 0.015])
    arm_x = np.array(J["lowerarm_l"][4])
    L.loft(path, lambda s, t: (float(np.interp(s, prof_s, prof_r)),
                               float(np.interp(s, prof_s, prof_r)) * (1.0 if s < S[2] else 0.86)),
           arm_x, n=18, weights=arm_w, mat_fn=lambda s, p: SHIRT if s < S[1] + 0.09 else SKIN, cap1=False)

    # Hand: palm along the hand axis (d2), width w (the finger flexion axis), thickness nrm
    d2 = unit(tail("hand_l") - head("hand_l"))
    w = np.array(J["index_01_l"][4])
    w = unit(w - d2 * np.dot(w, d2))
    nrm = np.cross(d2, w)
    centre_off = 0.004  # the palm sits a little off the bone line, toward the knuckle row's centre
    p0 = wr - d2 * 0.008 + w * centre_off
    p1 = wr + d2 * 0.104 + w * centre_off
    palm_len = np.linalg.norm(p1 - p0)

    def palm_r(s, t):
        x = 2 * t - 1
        k = (1.0 - abs(x) ** 3) ** (1.0 / 3.0)
        wide = 0.030 + 0.014 * smoothstep((t - 0.05) / 0.4)
        return (wide * max(k, 0.15), 0.017 * max(k, 0.15) * (1.0 - 0.25 * t))
    L.loft([p0, p1], palm_r, w, n=20, weights=lambda s, p: {"hand_l": 1.0}, cap0=False, cap1=False,
           rings_per_m=200)
    del palm_len

    for f, scale in (("index", 1.0), ("middle", 1.03), ("ring", 0.97), ("pinky", 0.85)):
        names = ["%s_%02d_l" % (f, i) for i in (1, 2, 3)]
        pts = [head(n) for n in names] + [tail(names[-1])]
        base = pts[0] - d2 * 0.016
        path = [base] + pts
        S = arclen(path)
        fw = chain_weights([S[1] - 0.004, S[2], S[3]], ["hand_l"] + names, [0.006, 0.005, 0.004])
        r0 = 0.0088 * scale
        L.loft(path, lambda s, t, r0=r0: (r0 * (1.0 - 0.22 * t), r0 * (0.92 - 0.2 * t)), w, n=10, weights=fw,
               cap0=False, rings_per_m=400)
    names = ["thumb_01_l", "thumb_02_l", "thumb_03_l"]
    pts = [head(n) for n in names] + [tail(names[-1])]
    S = arclen(pts)
    tw = chain_weights([0.012, S[1], S[2]], ["hand_l"] + names, [0.010, 0.006, 0.005])
    L.loft(pts, lambda s, t: (0.0125 * (1.0 - 0.28 * t), 0.0115 * (1.0 - 0.28 * t)), w, n=12, weights=tw,
           rings_per_m=400)

    # Leg: hip -> knee -> ankle
    hip, knee, ankle = head("thigh_l"), head("calf_l"), head("foot_l")
    top = hip + np.array((0.0, 0.0, 0.07))
    path = [top, hip, knee, ankle]
    S = arclen(path)
    prof_s = [0.0, S[1], S[1] + 0.12, S[2] - 0.08, S[2], S[2] + 0.10, S[2] + 0.20, S[3] - 0.06, S[3]]
    prof_r = [0.078, 0.090, 0.083, 0.060, 0.052, 0.059, 0.051, 0.036, 0.034]
    leg_w = chain_weights([S[1] - 0.05, S[2], S[3] - 0.012], ["pelvis", "thigh_l", "calf_l", "foot_l"],
                          [0.04, 0.045, 0.02])
    L.loft(path, lambda s, t: (float(np.interp(s, prof_s, prof_r)) * 1.04, float(np.interp(s, prof_s, prof_r))),
           (0, -1, 0), n=20, weights=leg_w, mat_fn=lambda s, p: SHORTS if s < S[1] + 0.13 else SKIN,
           cap0=False, cap1=False)

    # Foot: heel -> ball -> toe tip, sole at z = 0
    ball = head("ball_l")
    fx = ankle[0]
    pts2 = [(fx, 0.045), (fx, 0.0), (ball[0], ball[1]), (ball[0], ball[1] - 0.075)]
    hgt = [0.034, 0.042, 0.026, 0.016]
    wid = [0.032, 0.042, 0.047, 0.038]
    ys = [p[1] for p in pts2]
    path = [(x, y, h) for (x, y), h in zip(pts2, hgt)]
    S = arclen(path)

    def foot_r(s, _t):
        return (float(np.interp(s, S, wid)), float(np.interp(s, S, hgt)))
    fw = chain_weights([S[2] - 0.005], ["foot_l", "ball_l"], [0.02])
    L.loft(path, foot_r, (1, 0, 0), n=16, weights=fw, mat=SHOE, rings_per_m=200)
    del ys
    # ankle: closes the gap between the shin and the top of the foot
    L.ellipsoid(ankle + np.array((0.0, 0.0, -0.015)), (0.036, 0.040, 0.042), n=16, rings=10,
                weights=lambda p: {"foot_l": 0.75, "calf_l": 0.25})


def build_body():
    B, L = Builder(), Builder()
    build_torso(B)
    build_neck_head(B)
    build_left(L)
    B.append_mirrored(L)
    return B


# ------------------------------------------------------------------------------------------------------------
# Blender objects
# ------------------------------------------------------------------------------------------------------------
def make_materials():
    out = []
    for name, (r, g, b) in MATS:
        mat, _nt, bsdf = nb.new_material("th_" + name)
        bsdf.inputs["Base Color"].default_value = (r, g, b, 1.0)
        bsdf.inputs["Roughness"].default_value = 0.3 if name == "eye" else 0.6
        mat.diffuse_color = (r, g, b, 1.0)
        out.append(mat)
    return out


def build_mesh_object(B, arm):
    verts = [tuple(float(c) for c in p) for p in B.v]
    obj = nb.mesh_object("test_human", verts, B.f)
    me = obj.data
    for m in make_materials():
        me.materials.append(m)
    me.polygons.foreach_set("material_index", B.fm)
    me.polygons.foreach_set("use_smooth", [True] * len(B.f))
    me.update()
    groups = {}
    for i, ws in enumerate(B.w):
        for bone, x in ws.items():
            if bone not in groups:
                groups[bone] = obj.vertex_groups.new(name=bone)
            groups[bone].add([i], float(x), "REPLACE")
    for b in arm.data.bones:  # every deform joint gets a group so names resolve on export
        if b.use_deform and b.name not in obj.vertex_groups:
            obj.vertex_groups.new(name=b.name)
    rc.bind_mesh(obj, arm)
    return obj


# ------------------------------------------------------------------------------------------------------------
# Clips
# ------------------------------------------------------------------------------------------------------------
def sided(pose, base, left, right=None):
    """Writes base_l = left and base_r = right (both in left-side convention; the right is mirrored: y, z negated)."""
    right = left if right is None else right
    pose[base + "_l"] = tuple(left)
    pose[base + "_r"] = (right[0], -right[1], -right[2])


def fingers_relaxed(pose, k=1.0):
    for f in ("index", "middle", "ring", "pinky"):
        sided(pose, f + "_01", (12 * k, 0, 0))
        sided(pose, f + "_02", (18 * k, 0, 0))
        sided(pose, f + "_03", (10 * k, 0, 0))
    sided(pose, "thumb_02", (10 * k, 0, 0))


def key_pose(arm, joints, frame, pose, pelvis_up=0.0):
    rc.reset_pose(arm)
    rc.set_pose(arm, pose)
    arm.pose.bones["pelvis"].location = (0.0, pelvis_up, 0.0)  # pelvis local +Y is world up
    rc.key_all_rotations(arm, frame, joints)
    arm.pose.bones["pelvis"].keyframe_insert("location", frame=frame)


def make_idle(arm, joints):
    act = rc.new_action(arm, "idle")
    n = 60
    for f in range(0, n + 1, 3):
        ph = 2 * math.pi * f / n
        b = math.sin(ph)
        pose = {"spine_01": (0.8 * b, 0, 0), "spine_02": (-1.2 * b, 0, 0), "spine_03": (-1.8 * b, 0, 0),
                "neck_01": (1.0 * b, 0, 0), "head": (1.5 * math.sin(ph + 0.6), 1.5 * math.sin(ph * 0.5), 0)}
        sided(pose, "clavicle", (1.5 * b, 0, 0))
        sided(pose, "upperarm", (3.0 * math.sin(ph + 0.4), 0, -28 + 1.5 * b))
        sided(pose, "lowerarm", (12 + 2.0 * math.sin(ph + 0.8), 0, 0))
        fingers_relaxed(pose)
        key_pose(arm, joints, f, pose, pelvis_up=-0.003 * (1 - math.cos(ph)) * 0.5)
    rc.bake_linear(act)
    return act


def make_walk(arm, joints):
    """An in-place walk cycle. Signs from rig_spec.ROM_POSES: thigh -x = hip flexion (leg forward), calf +x =
    knee flexion, upperarm +x = arm forward, foot +x = toes down."""
    act = rc.new_action(arm, "walk")
    n = 32
    for f in range(0, n + 1):
        ph = 2 * math.pi * f / n
        pose = {}
        for side, sgn in (("_l", 1.0), ("_r", -1.0)):
            p = ph if sgn > 0 else ph + math.pi
            thigh = -24 * math.sin(p)
            calf = 5 + 58 * max(0.0, math.cos(p + 0.5)) ** 1.5
            foot = 14 * max(0.0, -math.sin(p)) * max(0.0, math.cos(p + 0.9)) - 12 * max(0.0, math.cos(p))
            arm_sw = -20 * math.sin(p)
            m = 1.0 if side == "_l" else -1.0  # right side: y and z mirror
            pose["thigh" + side] = (thigh, 0, m * 2)
            pose["calf" + side] = (calf, 0, 0)
            pose["foot" + side] = (foot, 0, 0)
            pose["upperarm" + side] = (arm_sw, 0, m * -30)
            pose["lowerarm" + side] = (18 - 6 * math.sin(p), 0, 0)
        tw = math.sin(ph)
        pose["pelvis"] = (0, -5 * tw, 2 * math.sin(2 * ph))
        pose["spine_01"] = (4, 2 * tw, 0)
        pose["spine_02"] = (1, 3 * tw, 0)
        pose["spine_03"] = (0, 3 * tw, 0)
        pose["head"] = (-3, -1 * tw, 0)
        fingers_relaxed(pose)
        key_pose(arm, joints, f, pose, pelvis_up=0.018 * math.cos(2 * ph) - 0.012)
    rc.bake_linear(act)
    return act


# ------------------------------------------------------------------------------------------------------------
def check_glb(path):
    doc = nb.read_glb_json(path)
    joints = [doc["nodes"][j]["name"] for j in doc["skins"][0]["joints"]]
    assert joints == rs.JOINT_NAMES, "skin joint order differs from the canonical order"
    anims = sorted(a["name"] for a in doc.get("animations", []))
    assert anims == ["idle", "rom", "walk"], anims
    interp = {s.get("interpolation", "LINEAR") for a in doc.get("animations", []) for s in a["samplers"]}
    assert interp == {"LINEAR"}, "non-LINEAR samplers: %r" % sorted(interp)
    return {"joints": len(joints), "animations": anims, "primitives": len(doc["meshes"][0]["primitives"])}


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    out, blend = "Data/Models/test_human.glb", None
    for i, a in enumerate(argv):
        if a == "--out":
            out = argv[i + 1]
        if a == "--blend":
            blend = argv[i + 1]

    nb.reset_scene()
    arm = rc.build_canonical_armature("test_human_rig")
    B = build_body()
    obj = build_mesh_object(B, arm)
    nb.activate(obj)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.normals_make_consistent(inside=False)
    bpy.ops.object.mode_set(mode="OBJECT")
    stats = rc.weight_post_pass(obj, arm)
    print("weights:", stats)
    zmin = min(v.co.z for v in obj.data.vertices)
    zmax = max(v.co.z for v in obj.data.vertices)
    print("height: %.3f m (soles at %.4f)" % (zmax - zmin, zmin))

    joints = rc.body_joints(arm)
    make_idle(arm, joints)
    make_walk(arm, joints)
    rc.make_rom_action(arm, joints)
    arm.animation_data.action = None
    rc.reset_pose(arm)
    bpy.context.scene.frame_set(0)
    bpy.context.view_layer.update()

    glb = nb.export_rig(os.path.abspath(out), {"export_morph": False, "export_force_sampling": False})
    info = check_glb(glb)
    print("test_human: %d verts, %d faces, %s, %d bytes" % (len(obj.data.vertices), len(obj.data.polygons), info,
                                                            os.path.getsize(glb)))
    if blend:
        os.makedirs(os.path.dirname(os.path.abspath(blend)), exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(blend))
        print("saved", blend)


if __name__ == "__main__":
    main()
