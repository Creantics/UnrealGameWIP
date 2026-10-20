"""Builds the three lead characters into Data/Models/{devin,duy,third}.glb (stylised, not realistic).

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_leads.py -- \\
        [--outdir Data/Models] [--only devin,duy,third] [--blend-dir <dir>]

Each character is one skinned mesh on the canonical 87-joint skeleton (rig_spec.py), so everything that works for
proof_head and test_human (import, IK Rig, retargeting, the face code) works for them:

* Head: the proof head (proof_head.py) with all 56 face shapes (face_spec.ALL_SHAPES), its eyes, lids, teeth,
  tongue and mouth bag on the face joints. Per character it gets a small lateral warp of the base mesh (rounder or
  narrower lower face), facial hair as material regions pushed out a few mm (so it rides the jaw weights and the
  mouth shapes), and its own skin, lip, brow and iris colours. The shape deltas themselves are untouched.
* Body: test_human's lofted tubes (make_test_human.py) with per-character thickness tables for the torso, neck,
  arms, legs, hands and feet, plus a skirt loft for long tunics and coats. Clothing is material regions on the
  body plus a few extra pieces (discs, rings, glasses) rigidly weighted to one bone.
* Hair: sheets over the skull, offset along the skull normal, weighted to `head`. They have no shape deltas.
* Height: the character is built at the canonical size (skull top 1.808 m), then the skeleton, mesh and shape
  deltas are scaled uniformly so the top of the skull sits at the given height.
* Clips: `idle`, `walk` and `rom` with LINEAR keys, as for test_human (per-character arm angles so thick arms
  clear the torso).

Weights: the body goes through rig_common.weight_post_pass (it is mirror-symmetric); the head keeps proof_head's
curated weights (its lip-line vertices are doubled, which the mirror pass would average across the lips).
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bpy  # noqa: E402
import numpy as np  # noqa: E402

import face_spec as fsp  # noqa: E402
import make_test_human as th  # noqa: E402
import ne_blender as nb  # noqa: E402
import proof_head as ph  # noqa: E402
import rig_common as rc  # noqa: E402
import rig_spec as rs  # noqa: E402

C = np.array(rs.HEAD_CENTER)
RAD = np.array(rs.HEAD_RADII)
EZ, MZ = rs.EYE_Z, rs.MOUTH_Z
SKULL_TOP = C[2] + RAD[2]  # 1.808 m on the canonical skeleton
head, tail, unit, sm = th.head, th.tail, th.unit, th.smoothstep
HEAD_W = {"head": 1.0}

COMMON = {"sclera": "#EEEBE3", "pupil": "#080808", "teeth": "#F2EEE2", "tongue": "#C46B6E", "mouth": "#2A0B0E"}


def lin(hexstr):
    h = hexstr.lstrip("#")
    c = [int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]
    return tuple(x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in c)


# ------------------------------------------------------------------------------------------------------------
# Outfits: material name from where a face sits. z / p are canonical (pre-scale) metres.
# ------------------------------------------------------------------------------------------------------------
def devin_torso(z, p, hw, hd, cy):
    if 1.0 <= z <= 1.066:
        return "sash"
    side = abs(p[0]) > 0.76 * hw and abs(p[1] - cy) < 0.8 * hd
    return "panel" if side else "tunic"


def devin_arm(s, S):
    if s < S[1] + 0.13:
        return "tunic"
    if s < S[1] + 0.165:
        return "panel"
    if S[2] + 0.03 < s < S[3] - 0.004:
        return "wrap" if int((s - S[2]) / 0.018) % 2 == 0 else "wrap2"
    return "skin"


def duy_torso(z, p, hw, hd, cy):
    if z < 0.885:
        return "pants"
    if z < 0.91:
        return "trim"
    if abs(p[0]) < 0.0075 and p[1] < cy:
        return "trim"
    return "jacket"


def duy_arm(s, S):
    if s < S[3] - 0.05:
        return "jacket"
    if s < S[3] - 0.038:
        return "trim"
    return "skin"


def third_torso(z, p, hw, hd, cy):
    return "pants" if z < 0.94 else "coat"


def third_arm(s, S):
    return "coat" if s < S[3] - 0.022 else "skin"


# ------------------------------------------------------------------------------------------------------------
# The characters. Torso multipliers are per th.TORSO row (z = 0.872 ... 1.485): tw width, td depth, tf the share
# of the extra depth that goes to the front (belly / chest) rather than the back.
# ------------------------------------------------------------------------------------------------------------
CHARS = {
    "devin": dict(
        code="dv", height=1.88, expression="amused",
        tw=[1.2, 1.2, 1.2, 1.2, 1.24, 1.27, 1.3, 1.36, 1.4, 1.4, 1.42, 1.5, 1.55, 1.4],
        td=[1.15, 1.2, 1.2, 1.25, 1.45, 1.5, 1.42, 1.32, 1.35, 1.35, 1.3, 1.25, 1.25, 1.25],
        tf=[0.55] * 14,
        band=(1.0, 1.066, 0.007), neck=1.42, arm=(1.6, 1.5, 1.42, 1.25), hand=1.15,
        leg=(1.3, 1.32, 1.3, 1.22, 1.2, 1.3, 1.25, 1.15, 1.15), foot=1.12, idle_arm=-12.0, walk_arm=-16.0,
        face_wide=0.15, face_chin=0.04, skirt=(1.03, 0.72, 0.05), torso_mat=devin_torso, arm_mat=devin_arm,
        leg_mat=lambda s, S: "boot" if s > S[2] + 0.10 else "pants", foot_mat="boot", ankle_mat="boot",
        neck_mat=lambda z: "panel" if z < 1.468 else "skin", hair=None, beard="devin",
        colors={"skin": "#E2B08A", "lips": "#B9735E", "brow": "#1C1512", "iris": "#3A2417", "beard": "#1A1310",
                "tunic": "#FF8A1F", "panel": "#1F5BFF", "gold": "#FFC93C", "sash": "#E2620F", "wrap": "#E9DFC9",
                "wrap2": "#CDBFA3", "pants": "#2B2D38", "boot": "#5B2A1E"},
        metal=("gold",), emissive=()),
    "duy": dict(
        code="dy", height=1.70, expression="neutral",
        tw=[1.0, 1.06, 1.06, 1.06, 1.0, 0.86, 0.86, 0.86, 0.86, 0.86, 0.86, 0.86, 0.9, 0.95],
        td=[0.86] * 14, tf=[0.5] * 14, band=None, neck=0.88, arm=(0.86, 0.85, 0.85, 0.9), hand=0.92,
        leg=(0.88,) * 9, foot=0.95, idle_arm=-30.0, walk_arm=-32.0, face_wide=-0.01, face_chin=-0.07, skirt=None,
        torso_mat=duy_torso, arm_mat=duy_arm, leg_mat=lambda s, S: "pants", foot_mat="shoe", ankle_mat="pants",
        neck_mat=lambda z: "jacket" if z < 1.468 else "skin", hair="duy", beard=None,
        colors={"skin": "#EEC7A2", "lips": "#C98F7A", "brow": "#121012", "iris": "#2B1C12", "hair": "#0F0E12",
                "jacket": "#1B1F5E", "trim": "#7B2FF7", "gold": "#FFC93C", "glow": "#9B5CFF", "pants": "#1C1C24",
                "shoe": "#131316", "glasses": "#0B0B0D"},
        metal=("gold",), emissive=("glow",)),
    "third": dict(
        code="tr", height=1.80, expression="neutral",
        tw=[1.0] * 14, td=[1.0] * 14, tf=[0.5] * 14, band=None, neck=1.0, arm=(0.88, 0.97, 1.0, 1.0), hand=1.0,
        leg=(1.0,) * 9, foot=1.0, idle_arm=-26.0, walk_arm=-28.0, face_wide=0.01, face_chin=-0.02,
        skirt=(1.03, 0.42, 0.07), torso_mat=third_torso, arm_mat=third_arm, leg_mat=lambda s, S: "pants",
        foot_mat="shoe", ankle_mat="pants", neck_mat=lambda z: "coat" if z < 1.466 else "skin", hair="third",
        beard="third",
        colors={"skin": "#F3D9C8", "lips": "#C9877A", "brow": "#2A1E18", "iris": "#6B4426", "hair": "#EAE4D2",
                "hair2": "#4B3B2D", "beard": "#2B201A", "coat": "#EEEADF", "pants": "#26262C", "shoe": "#1D1D20"},
        metal=(), emissive=()),
}


# ------------------------------------------------------------------------------------------------------------
# Body
# ------------------------------------------------------------------------------------------------------------
def torso_rows(ch):
    rows = []
    for k, (z, w, d, cy) in enumerate(th.TORSO):
        w2, d2 = w * ch["tw"][k], d * ch["td"][k]
        rows.append((z, w2, d2, cy - (d2 - d) * ch["tf"][k]))
    return rows


SPINE = th.chain_weights([1.03, 1.17, 1.29, 1.465], ["pelvis", "spine_01", "spine_02", "spine_03", "neck_01"],
                         [0.04, 0.05, 0.05, 0.02])


def band_bump(ch, z):
    if not ch["band"]:
        return 0.0
    z0, z1, h = ch["band"]
    return h * float(sm((z - z0) / 0.008) * sm((z1 - z) / 0.008))


def build_torso(B, ch, M, rows):
    zs = [r[0] for r in rows]
    path = [(0.0, r[3], r[0]) for r in rows]
    S = list(th.arclen(path))
    z_of = lambda s: float(np.interp(s, S, zs))  # noqa: E731
    col = lambda k, z: float(np.interp(z, zs, [r[k] for r in rows]))  # noqa: E731

    def radii(s, _t):
        z = z_of(s)
        b = band_bump(ch, z)
        return col(1, z) + b, col(2, z) + b

    def weights(s, p):
        z = z_of(s)
        w = SPINE(z)
        side = "_l" if p[0] >= 0 else "_r"
        ax = abs(p[0])
        cl = float(sm((ax - 0.06) / 0.10) * sm((z - 1.33) / 0.09)) * 0.7
        tg = float(sm((ax - 0.03) / 0.08) * sm((0.97 - z) / 0.10)) * 0.6
        out = {b: x * (1.0 - cl - tg) for b, x in w.items()}
        if cl > 0:
            out["clavicle" + side] = out.get("clavicle" + side, 0.0) + cl
        if tg > 0:
            out["thigh" + side] = out.get("thigh" + side, 0.0) + tg
        return out

    def mat_fn(s, p):
        z = z_of(s)
        return M[ch["torso_mat"](z, p, col(1, z), col(2, z), col(3, z))]

    B.loft(path, radii, (1, 0, 0), n=40, weights=weights, mat_fn=mat_fn, expo=2.5, rings_per_m=110)
    return col


def build_skirt(B, ch, M, col):
    """An open, flared tube from z0 down to z1 around the hips: a tunic or coat skirt. It hands from the pelvis
    over to the thighs as it goes down, split left / right by x."""
    z0, z1, flare = ch["skirt"]
    leg_r = 0.090 * ch["leg"][1] * 1.04  # the thigh tube's half width at the hip (build_left)
    w0 = max(col(1, z0) + 0.008, head("thigh_l")[0] + leg_r + 0.014)
    d0, cy = max(col(2, z0) + 0.008, leg_r / 1.04 + 0.03), col(3, z0)
    length = z0 - z1
    path = [(0.0, cy, z0), (0.0, cy, z1)]

    def radii(s, t):
        return w0 + flare * t ** 1.2, d0 + 0.6 * flare * t ** 1.2

    def weights(s, p):
        t = s / length
        wl = float(sm((p[0] + 0.07) / 0.14))
        wt = 0.85 * t ** 0.8
        out = {"pelvis": 1.0 - wt, "thigh_l": wt * wl, "thigh_r": wt * (1.0 - wl)}
        return {b: x for b, x in out.items() if x > 1e-4}

    def mat_fn(s, p):
        t = s / length
        return M[ch["torso_mat"](1.2, p, w0 + flare * t ** 1.2, d0 + 0.6 * flare * t ** 1.2, cy)]  # a top colour

    B.loft(path, radii, (1, 0, 0), n=40, weights=weights, mat_fn=mat_fn, cap0=False, cap1=False, rings_per_m=90)


def build_neck(B, ch, M):
    path = [(0.0, 0.008, 1.44), (0.0, 0.002, 1.53), (0.0, -0.008, 1.64)]
    neck = th.chain_weights([1.475, 1.52, 1.585], ["spine_03", "neck_01", "neck_02", "head"], [0.02, 0.03, 0.03])
    k = ch["neck"]
    B.loft(path, lambda s, t: ((0.050 - 0.004 * t) * k, (0.054 - 0.006 * t) * k), (1, 0, 0), n=24,
           weights=lambda s, p: neck(1.44 + s), mat_fn=lambda s, p: M[ch["neck_mat"](p[2])], cap0=False, cap1=False,
           rings_per_m=150)


def build_left(L, ch, M):
    """Ear, arm, hand, leg and foot on the character's left (+X); mirrored for the right."""
    L.ellipsoid((0.079, 0.002, 1.690), (0.012, 0.020, 0.030), n=12, rings=8, weights=lambda p: HEAD_W, mat=M["skin"])

    sh, el, wr = head("upperarm_l"), head("lowerarm_l"), head("hand_l")
    d1 = unit(el - sh)
    path = [sh - d1 * 0.045, sh, el, wr]
    S = th.arclen(path)
    delt, up, lo, wrist = ch["arm"]
    prof_s = [0.0, S[1], S[1] + 0.06, S[1] + 0.16, S[2] - 0.02, S[2], S[2] + 0.06, S[2] + 0.16, S[3]]
    prof_r = np.array([0.042, 0.056, 0.052, 0.047, 0.039, 0.039, 0.042, 0.034, 0.027]) * \
        np.array([delt, delt, 0.5 * (delt + up), up, up, 0.5 * (up + lo), lo, lo, wrist])
    arm_w = th.chain_weights([S[1] - 0.025, S[2], S[3] - 0.01], ["clavicle_l", "upperarm_l", "lowerarm_l", "hand_l"],
                             [0.03, 0.035, 0.015])
    arm_x = np.array(th.J["lowerarm_l"][4])
    L.loft(path, lambda s, t: (float(np.interp(s, prof_s, prof_r)),
                               float(np.interp(s, prof_s, prof_r)) * (1.0 if s < S[2] else 0.86)),
           arm_x, n=20, weights=arm_w, mat_fn=lambda s, p: M[ch["arm_mat"](s, S)], cap1=False)
    arm_info = dict(path=path, S=S, prof_s=prof_s, prof_r=prof_r, arm_x=arm_x)

    hs = ch["hand"]
    d2 = unit(tail("hand_l") - head("hand_l"))
    w = np.array(th.J["index_01_l"][4])
    w = unit(w - d2 * np.dot(w, d2))
    p0 = wr - d2 * 0.008 + w * 0.004
    p1 = wr + d2 * 0.104 + w * 0.004

    def palm_r(s, t):
        x = 2 * t - 1
        k = (1.0 - abs(x) ** 3) ** (1.0 / 3.0)
        wide = (0.030 + 0.014 * sm((t - 0.05) / 0.4)) * hs
        return (wide * max(k, 0.15), 0.017 * hs * max(k, 0.15) * (1.0 - 0.25 * t))
    L.loft([p0, p1], palm_r, w, n=20, weights=lambda s, p: {"hand_l": 1.0}, mat=M["skin"], cap0=False, cap1=False,
           rings_per_m=200)
    for f, scale in (("index", 1.0), ("middle", 1.03), ("ring", 0.97), ("pinky", 0.85)):
        names = ["%s_%02d_l" % (f, i) for i in (1, 2, 3)]
        pts = [head(n) for n in names] + [tail(names[-1])]
        fpath = [pts[0] - d2 * 0.016] + pts
        FS = th.arclen(fpath)
        fw = th.chain_weights([FS[1] - 0.004, FS[2], FS[3]], ["hand_l"] + names, [0.006, 0.005, 0.004])
        r0 = 0.0088 * scale * hs
        L.loft(fpath, lambda s, t, r0=r0: (r0 * (1.0 - 0.22 * t), r0 * (0.92 - 0.2 * t)), w, n=10, weights=fw,
               mat=M["skin"], cap0=False, rings_per_m=400)
    names = ["thumb_01_l", "thumb_02_l", "thumb_03_l"]
    pts = [head(n) for n in names] + [tail(names[-1])]
    TS = th.arclen(pts)
    tw = th.chain_weights([0.012, TS[1], TS[2]], ["hand_l"] + names, [0.010, 0.006, 0.005])
    L.loft(pts, lambda s, t: (0.0125 * hs * (1.0 - 0.28 * t), 0.0115 * hs * (1.0 - 0.28 * t)), w, n=12, weights=tw,
           mat=M["skin"], rings_per_m=400)

    hip, knee, ankle = head("thigh_l"), head("calf_l"), head("foot_l")
    lpath = [hip + np.array((0.0, 0.0, 0.07)), hip, knee, ankle]
    LS = th.arclen(lpath)
    lprof_s = [0.0, LS[1], LS[1] + 0.12, LS[2] - 0.08, LS[2], LS[2] + 0.10, LS[2] + 0.20, LS[3] - 0.06, LS[3]]
    lprof_r = np.array([0.056, 0.090, 0.083, 0.060, 0.052, 0.059, 0.051, 0.036, 0.034]) * np.array(ch["leg"])
    leg_w = th.chain_weights([LS[1] - 0.05, LS[2], LS[3] - 0.012], ["pelvis", "thigh_l", "calf_l", "foot_l"],
                             [0.04, 0.045, 0.02])
    L.loft(lpath, lambda s, t: (float(np.interp(s, lprof_s, lprof_r)) * 1.04, float(np.interp(s, lprof_s, lprof_r))),
           (0, -1, 0), n=20, weights=leg_w, mat_fn=lambda s, p: M[ch["leg_mat"](s, LS)], cap0=False, cap1=False)

    fs_ = ch["foot"]
    ball = head("ball_l")
    fx = ankle[0]
    pts2 = [(fx, 0.045), (fx, 0.0), (ball[0], ball[1]), (ball[0], ball[1] - 0.075 * fs_)]
    hgt = [h * fs_ for h in (0.034, 0.042, 0.026, 0.016)]
    wid = [x * fs_ for x in (0.032, 0.042, 0.047, 0.038)]
    fpath = [(x, y, h) for (x, y), h in zip(pts2, hgt)]
    FS = th.arclen(fpath)
    L.loft(fpath, lambda s, _t: (float(np.interp(s, FS, wid)), float(np.interp(s, FS, hgt))), (1, 0, 0), n=16,
           weights=th.chain_weights([FS[2] - 0.005], ["foot_l", "ball_l"], [0.02]), mat=M[ch["foot_mat"]],
           rings_per_m=200)
    a = ch["leg"][-1]
    L.ellipsoid(ankle + np.array((0.0, 0.0, -0.015)), (0.036 * a, 0.040 * a, 0.042 * a), n=16, rings=10,
                weights=lambda p: {"foot_l": 0.75, "calf_l": 0.25}, mat=M[ch["ankle_mat"]])
    return arm_info


def build_body(ch, M):
    B, L = th.Builder(), th.Builder()
    rows = torso_rows(ch)
    col = build_torso(B, ch, M, rows)
    if ch["skirt"]:
        build_skirt(B, ch, M, col)
    build_neck(B, ch, M)
    arm_info = build_left(L, ch, M)
    B.append_mirrored(L)
    return B, col, arm_info


# ------------------------------------------------------------------------------------------------------------
# Extra pieces (not mirrored, no shape deltas)
# ------------------------------------------------------------------------------------------------------------
def orient(X, f0, inside):
    """Flips faces from index f0 on so their normals point away from the point `inside`."""
    for i in range(f0, len(X.f)):
        f = X.f[i]
        P = [np.asarray(X.v[k]) for k in f]
        n = np.cross(P[1] - P[0], P[2] - P[0])
        if np.dot(n, sum(P) / len(P) - inside) < 0:
            X.f[i] = tuple(reversed(f))


def disc(X, centre, nrm, radius, dome, mat, w, n=32, rings=6):
    """A domed disc (badge, shield): a cap bulging along `nrm` plus a flat back."""
    c, nrm = np.asarray(centre, float), unit(nrm)
    u = unit(np.cross(nrm, (0, 0, 1)) if abs(nrm[2]) < 0.9 else np.cross(nrm, (1, 0, 0)))
    v = np.cross(nrm, u)
    f0 = len(X.f)
    top = len(X.v)
    X.v.append(c + nrm * dome)
    X.w.append(dict(w))
    ring_idx = []
    for i in range(1, rings + 1):
        r = radius * i / rings
        h = dome * (1.0 - (r / radius) ** 2)
        ring_idx.append(len(X.v))
        for k in range(n):
            a = 2 * math.pi * k / n
            X.v.append(c + nrm * h + (u * math.cos(a) + v * math.sin(a)) * r)
            X.w.append(dict(w))
    back = len(X.v)
    X.v.append(c - nrm * 0.003)
    X.w.append(dict(w))
    for k in range(n):
        k2 = (k + 1) % n
        X.f.append((top, ring_idx[0] + k, ring_idx[0] + k2))
        X.fm.append(mat)
        X.f.append((back, ring_idx[-1] + k2, ring_idx[-1] + k))
        X.fm.append(mat)
        for i in range(rings - 1):
            a0, b0 = ring_idx[i], ring_idx[i + 1]
            X.f.append((a0 + k, b0 + k, b0 + k2, a0 + k2))
            X.fm.append(mat)
    orient(X, f0, c + nrm * dome * 0.3)


def skull_dir(t, p):
    """Unit direction on the skull: polar angle t from the crown, azimuth p (0 = front, +pi/2 = left)."""
    return np.array([math.sin(t) * math.sin(p), -math.sin(t) * math.cos(p), math.cos(t)])


def shell(X, mat, off, keep, shift=None, n=128, rings=76, t_max=2.25):
    """A sheet of hair over the skull. off(t, p, S) is the height above the skull along its normal; keep(t, p, S)
    keeps a face (by its centre); shift(t, p, S) moves a vertex sideways (sweeps)."""
    f0 = len(X.f)
    crown = len(X.v)
    S0 = C + RAD * skull_dir(0.0, 0.0)
    X.v.append(S0 + np.array((0, 0, 1.0)) * off(0.0, 0.0, S0))
    X.w.append(dict(HEAD_W))

    def vert(t, p):
        d = skull_dir(t, p)
        S = C + RAD * d
        N = unit(d / RAD)
        q = S + N * off(t, p, S)
        if shift:
            q = q + shift(t, p, S)
        return q

    base = len(X.v)
    for i in range(1, rings + 1):
        t = t_max * i / rings
        for k in range(n):
            X.v.append(vert(t, 2 * math.pi * k / n - math.pi))
            X.w.append(dict(HEAD_W))

    def centre(i, k):  # ring i (0 = crown) .. i + 1, column k .. k + 1
        t = t_max * (i + 0.5) / rings
        p = 2 * math.pi * (k + 0.5) / n - math.pi
        return t, p, C + RAD * skull_dir(t, p)

    for k in range(n):
        k2 = (k + 1) % n
        if keep(*centre(0, k)):
            X.f.append((crown, base + k, base + k2))
            X.fm.append(mat)
    for i in range(rings - 1):
        for k in range(n):
            k2 = (k + 1) % n
            if keep(*centre(i + 1, k)):
                a0, a1 = base + i * n + k, base + i * n + k2
                b0, b1 = base + (i + 1) * n + k, base + (i + 1) * n + k2
                X.f.append((a0, a1, b1, b0))
                X.fm.append(mat)
    orient(X, f0, C)


def hairline(p, front, side, back):
    return float(np.interp(abs(p), [0.0, 0.8, 1.45, 2.2, math.pi], [front, front, side, side, back]))


def hair_duy(X, M):
    def keep(t, p, S):
        jag = 0.006 * math.sin(11 * p) if abs(p) < 0.9 else 0.0
        return S[2] > hairline(p, EZ + 0.030 + jag, 1.722, 1.615)

    def off(t, p, S):
        o = 0.009 + 0.013 * float(sm((S[2] - 1.70) / 0.09))
        o += 0.0035 * math.sin(7 * p + 3 * t) * math.sin(5 * t)
        if abs(p) < 1.0:  # the fringe stands off the forehead
            o += 0.004 * float(sm((1.78 - S[2]) / 0.03))
        return o
    shell(X, M["hair"], off, keep)


def hair_third(X, M):
    # short, dark sides and back (the roots show below the bleached top)
    shell(X, M["hair2"], lambda t, p, S: 0.0035 + 0.0008 * math.sin(13 * p) * math.sin(9 * t),
          lambda t, p, S: S[2] > hairline(p, 1.766, 1.722, 1.63))

    # bleached top: higher at the front, swept up and over to the character's right (-X), textured
    def keep(t, p, S):
        return t < float(np.interp(abs(p), [0.0, 1.2, math.pi], [0.90, 0.80, 0.76]))

    def off(t, p, S):
        front = float(sm(1.0 - abs(p) / 1.5))
        o = 0.010 + 0.020 * front * float(sm(t / 0.7)) + 0.008 * float(np.clip(-S[0] / RAD[0], 0, 1))
        o += 0.006 * abs(math.sin(9 * p + 4 * t)) * float(sm(t / 0.4))
        edge = float(sm((0.9 - t) / 0.12))  # thins out toward its edge so the dark roots show
        return 0.004 + (o - 0.004) * edge

    def shift(t, p, S):
        return np.array((-0.012 * float(sm(t / 0.8)) * float(sm(1.0 - abs(p) / 2.5)), 0.0, 0.0))
    shell(X, M["hair"], off, keep, shift=shift)


def glasses_duy(X, M):
    """Rectangular black frames, rigid on the head bone."""
    G = th.Builder()
    ly, cx, cz, a, b = -0.1185, 0.033, EZ, 0.0235, 0.0145
    pts = []
    for k in range(29):
        ang = 2 * math.pi * k / 28
        c_, s_ = math.cos(ang), math.sin(ang)
        pts.append((cx + a * math.copysign(abs(c_) ** 0.35, c_), ly, cz + b * math.copysign(abs(s_) ** 0.35, s_)))
    tube = lambda s, t: (0.0017, 0.0024)  # noqa: E731
    G.loft(pts, tube, (0, 1, 0), n=8, weights=lambda s, p: HEAD_W, mat=M["glasses"], cap0=False, cap1=False,
           rings_per_m=900)
    temple = [(cx + a + 0.0005, ly + 0.002, cz + 0.008), (0.071, -0.100, 1.712), (0.083, -0.060, 1.718),
              (0.087, -0.012, 1.722), (0.085, 0.030, 1.712)]
    G.loft(temple, lambda s, t: (0.0015, 0.0022), (0, 0, 1), n=8, weights=lambda s, p: HEAD_W, mat=M["glasses"],
           rings_per_m=500)
    X.append_mirrored(G)
    bridge = [(cx - a + 0.001, ly, cz + 0.006), (0.0, ly - 0.001, cz + 0.009), (-(cx - a + 0.001), ly, cz + 0.006)]
    X.loft(bridge, lambda s, t: (0.0015, 0.0020), (0, 1, 0), n=8, weights=lambda s, p: HEAD_W, mat=M["glasses"],
           rings_per_m=600)


def extras(ch, name, M, col, arm_info):
    X = th.Builder()
    if name == "devin":
        z = 1.31
        disc(X, (0.0, col(3, z) - col(2, z) + 0.002, z), (0, -1, 0), 0.052, 0.009, M["gold"], SPINE(z))
        z = 1.27
        back = col(3, z) + col(2, z) + 0.006
        disc(X, (0.0, back, z), (0, 1, 0), 0.165, 0.032, M["gold"], SPINE(z))
        disc(X, (0.0, back + 0.03, z), (0, 1, 0), 0.032, 0.016, M["sash"], SPINE(z))  # the shield's boss
    if name == "duy":
        hair_duy(X, M)
        glasses_duy(X, M)
        path, S, prof_s, prof_r = arm_info["path"], arm_info["S"], arm_info["prof_s"], arm_info["prof_r"]
        sh, el = np.array(path[1]), np.array(path[2])
        d1 = unit(el - sh)
        o = unit(np.cross(d1, (0, 1, 0)))  # up and out from the upper arm
        r = float(np.interp(S[1] + 0.07, prof_s, prof_r))
        disc(X, sh + d1 * 0.07 + o * (r + 0.001), o, 0.016, 0.004, M["gold"], {"upperarm_l": 1.0})
        # the glowing cuff on the left wrist
        wr = np.array(path[3])
        d2 = unit(wr - el)
        r = float(np.interp(S[3] - 0.024, prof_s, prof_r))
        X.loft([wr - d2 * 0.032, wr - d2 * 0.016], lambda s, t: (r + 0.004, r * 0.86 + 0.004), arm_info["arm_x"],
               n=20, weights=lambda s, p: {"lowerarm_twist_02_l": 1.0}, mat=M["glow"], cap0=False, cap1=False,
               rings_per_m=600)
    if name == "third":
        hair_third(X, M)
    return X


# ------------------------------------------------------------------------------------------------------------
# Head: proof head + per-character warp, facial hair regions and colours
# ------------------------------------------------------------------------------------------------------------
def beard_region(kind, c):
    """'beard' when a skin face centred at c is facial hair for this character."""
    x, y, z = c
    ax = abs(x)
    if y > C[1] + 0.03 or z < 1.556:
        return False
    if kind == "third":
        if MZ + 0.003 < z < MZ + 0.021 and ax < 0.034:  # moustache
            return True
        if ax < 0.021 and MZ - 0.009 < z < MZ + 0.003:  # keep the lower lip readable
            return False
        line = float(np.interp(ax, [0.0, 0.03, 0.05, 0.072, 0.1], [MZ + 0.014, MZ + 0.016, 1.668, 1.697, 1.705]))
        return z < line
    if kind == "devin":
        if MZ + 0.0045 < z < MZ + 0.0115 and ax < 0.027:  # thin moustache
            return True
        return ax < 0.016 - 0.2 * max(0.0, MZ - 0.022 - z) and MZ - 0.046 < z < MZ - 0.013  # short, tapered chin beard
    return False


def face_warp(P, isl, wide, chin):
    """Widens (wide > 0) or narrows the cheeks and jaw below the eyes, front half only; chin tapers below the
    mouth. Eyeballs are left alone (they turn on their joints)."""
    x, y, z = P[:, 0], P[:, 1], P[:, 2]
    front = sm(0.5 + (C[1] - y) / 0.08)
    band = sm((EZ - 0.008 - z) / 0.03)
    low = sm((MZ - z) / 0.04)
    k = 1.0 + (wide * band + chin * low) * front
    k[isl == ph.EYE] = 1.0
    Q = P.copy()
    Q[:, 0] = x * k
    return Q


def build_head(ch, M):
    HB = ph.build()
    shapes, _masks = ph.all_shapes(HB)
    P = np.array(HB.v)
    isl = np.array(HB.island)
    faces = [f for f, _ in HB.faces]
    fmat = [M[ph.MATERIALS[m]] for _, m in HB.faces]
    if ch["beard"]:
        push = np.zeros(len(P))
        t = 0.0025 if ch["beard"] == "third" else 0.0016
        for i, (f, m) in enumerate(HB.faces):
            if ph.MATERIALS[m] != "skin":
                continue
            if beard_region(ch["beard"], P[list(f)].mean(axis=0)):
                fmat[i] = M["beard"]
                push[list(f)] = t
        P = P + ph._outward_normal(P) * push[:, None]
    P = face_warp(P, isl, ch["face_wide"], ch["face_chin"])
    return P, faces, fmat, HB.weights, shapes


# ------------------------------------------------------------------------------------------------------------
# Blender objects
# ------------------------------------------------------------------------------------------------------------
def make_materials(ch):
    mats, index = [], {}
    colors = dict(ch["colors"])
    colors.update({k: v for k, v in COMMON.items() if k not in colors})
    for name, hexc in colors.items():
        mat, _nt, bsdf = nb.new_material("%s_%s" % (ch["code"], name))
        c = lin(hexc)
        bsdf.inputs["Base Color"].default_value = c + (1.0,)
        bsdf.inputs["Roughness"].default_value = 0.2 if name in ("sclera", "iris", "pupil", "glasses") else (
            0.35 if name in ch["metal"] else 0.6)
        bsdf.inputs["Metallic"].default_value = 0.8 if name in ch["metal"] else 0.0
        if name in ch["emissive"]:
            bsdf.inputs["Emission Color"].default_value = c + (1.0,)
            bsdf.inputs["Emission Strength"].default_value = 6.0
        mat.diffuse_color = c + (1.0,)
        mat.use_backface_culling = False
        index[name] = len(mats)
        mats.append(mat)
    return mats, index


def mesh_from(name, verts, faces, fmat, weights, mats, arm):
    obj = nb.mesh_object(name, [tuple(float(c) for c in p) for p in verts], faces)
    me = obj.data
    for m in mats:
        me.materials.append(m)
    me.polygons.foreach_set("material_index", list(fmat))
    me.polygons.foreach_set("use_smooth", [True] * len(faces))
    me.update()
    groups = {}
    for i, ws in enumerate(weights):
        for bone, x in ws.items():
            if bone not in groups:
                groups[bone] = obj.vertex_groups.get(bone) or obj.vertex_groups.new(name=bone)
            groups[bone].add([i], float(x), "REPLACE")
    for b in arm.data.bones:
        if b.use_deform and b.name not in obj.vertex_groups:
            obj.vertex_groups.new(name=b.name)
    rc.bind_mesh(obj, arm)
    return obj


def make_idle(arm, joints, drop):
    act = rc.new_action(arm, "idle")
    n = 60
    for f in range(0, n + 1, 3):
        ph_ = 2 * math.pi * f / n
        b = math.sin(ph_)
        pose = {"spine_01": (0.8 * b, 0, 0), "spine_02": (-1.2 * b, 0, 0), "spine_03": (-1.8 * b, 0, 0),
                "neck_01": (1.0 * b, 0, 0), "head": (1.5 * math.sin(ph_ + 0.6), 1.5 * math.sin(ph_ * 0.5), 0)}
        th.sided(pose, "clavicle", (1.5 * b, 0, 0))
        th.sided(pose, "upperarm", (3.0 * math.sin(ph_ + 0.4), 0, drop + 1.5 * b))
        th.sided(pose, "lowerarm", (12 + 2.0 * math.sin(ph_ + 0.8), 0, 0))
        th.fingers_relaxed(pose)
        th.key_pose(arm, joints, f, pose, pelvis_up=-0.003 * (1 - math.cos(ph_)) * 0.5)
    rc.bake_linear(act)
    return act


def make_walk(arm, joints, arm_z):
    act = rc.new_action(arm, "walk")
    n = 32
    for f in range(0, n + 1):
        ph_ = 2 * math.pi * f / n
        pose = {}
        for side, sgn in (("_l", 1.0), ("_r", -1.0)):
            p = ph_ if sgn > 0 else ph_ + math.pi
            m = 1.0 if side == "_l" else -1.0
            pose["thigh" + side] = (-24 * math.sin(p), 0, m * 2)
            pose["calf" + side] = (5 + 58 * max(0.0, math.cos(p + 0.5)) ** 1.5, 0, 0)
            pose["foot" + side] = (14 * max(0.0, -math.sin(p)) * max(0.0, math.cos(p + 0.9))
                                   - 12 * max(0.0, math.cos(p)), 0, 0)
            pose["upperarm" + side] = (-20 * math.sin(p), 0, m * arm_z)
            pose["lowerarm" + side] = (18 - 6 * math.sin(p), 0, 0)
        tw = math.sin(ph_)
        pose["pelvis"] = (0, -5 * tw, 2 * math.sin(2 * ph_))
        pose["spine_01"] = (4, 2 * tw, 0)
        pose["spine_02"] = (1, 3 * tw, 0)
        pose["spine_03"] = (0, 3 * tw, 0)
        pose["head"] = (-3, -1 * tw, 0)
        th.fingers_relaxed(pose)
        th.key_pose(arm, joints, f, pose, pelvis_up=0.018 * math.cos(2 * ph_) - 0.012)
    rc.bake_linear(act)
    return act


def check_glb(path):
    doc = nb.read_glb_json(path)
    assert len(doc["meshes"]) == 1, "expected one mesh"
    m = doc["meshes"][0]
    names = m.get("extras", {}).get("targetNames")
    assert names == fsp.ALL_SHAPES, "targetNames missing or out of order"
    for p in m["primitives"]:
        assert len(p.get("targets", [])) == len(fsp.ALL_SHAPES), "a primitive lacks the face targets"
    joints = [doc["nodes"][j]["name"] for j in doc["skins"][0]["joints"]]
    assert joints == rs.JOINT_NAMES, "skin joint order differs from the canonical order"
    anims = sorted(a["name"] for a in doc.get("animations", []))
    assert anims == ["idle", "rom", "walk"], anims
    interp = {s.get("interpolation", "LINEAR") for a in doc.get("animations", []) for s in a["samplers"]}
    assert interp == {"LINEAR"}, "non-LINEAR samplers: %r" % sorted(interp)
    return {"joints": len(joints), "shapes": len(names), "animations": anims, "primitives": len(m["primitives"])}


def build_character(name, outdir, blend_dir=None):
    ch = CHARS[name]
    nb.reset_scene()
    mats, M = make_materials(ch)
    B, col, arm_info = build_body(ch, M)
    P, hfaces, hmat, hweights, shapes = build_head(ch, M)
    X = extras(ch, name, M, col, arm_info)

    s = ch["height"] / SKULL_TOP
    rows = [(r[0], r[1], tuple(s * c for c in r[2]), tuple(s * c for c in r[3])) + tuple(r[4:]) for r in rs.JOINTS]
    arm = rc.build_canonical_armature(name + "_rig", rows=rows)

    body = mesh_from(name + "_body", np.array(B.v) * s, B.f, B.fm, B.w, mats, arm)
    nb.activate(body)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.normals_make_consistent(inside=False)
    bpy.ops.object.mode_set(mode="OBJECT")
    stats = rc.weight_post_pass(body, arm)

    nh = len(P)
    verts = np.concatenate([P, np.array(X.v).reshape(-1, 3)]) * s
    faces = hfaces + [tuple(nh + i for i in f) for f in X.f]
    hd = mesh_from(name, verts, faces, hmat + list(X.fm), list(hweights) + list(X.w), mats, arm)
    hd.shape_key_add(name="Basis", from_mix=False)
    pad = np.zeros((len(X.v), 3))
    for sname in fsp.ALL_SHAPES:
        kb = hd.shape_key_add(name=sname, from_mix=False)
        kb.data.foreach_set("co", (verts + np.concatenate([shapes[sname], pad]) * s).astype(np.float32).ravel())
        kb.slider_min, kb.slider_max = 0.0, 1.0
    hd.data.update()

    bpy.ops.object.select_all(action="DESELECT")
    body.select_set(True)
    hd.select_set(True)
    bpy.context.view_layer.objects.active = hd
    bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    obj.name = name
    obj["ne_face_shape_set"] = "face_presets.json v%d" % fsp.VERSION
    assert len(obj.data.shape_keys.key_blocks) == len(fsp.ALL_SHAPES) + 1
    for kb in obj.data.shape_keys.key_blocks:
        kb.value = 0.0  # the glTF default weights; the join leaves them at 1

    joints = rc.body_joints(arm)
    make_idle(arm, joints, ch["idle_arm"])
    make_walk(arm, joints, ch["walk_arm"])
    rc.make_rom_action(arm, joints)
    arm.animation_data.action = None
    rc.reset_pose(arm)
    bpy.context.scene.frame_set(0)
    bpy.context.view_layer.update()

    zs = [v.co.z for v in obj.data.vertices]
    out = os.path.abspath(os.path.join(outdir, name + ".glb"))
    glb = nb.export_rig(out, {"export_morph_normal": False, "export_force_sampling": False})
    info = check_glb(glb)
    print("%s: %d verts, %d faces, scale %.3f, top %.3f m, soles %.4f, weights %s, %s, %d bytes"
          % (name, len(obj.data.vertices), len(obj.data.polygons), s, max(zs), min(zs), stats, info,
             os.path.getsize(glb)))
    if blend_dir:
        os.makedirs(os.path.abspath(blend_dir), exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(os.path.join(blend_dir, name + ".blend")))


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    outdir, only, blend_dir = "Data/Models", list(CHARS), None
    for i, a in enumerate(argv):
        if a == "--outdir":
            outdir = argv[i + 1]
        if a == "--only":
            only = argv[i + 1].split(",")
        if a == "--blend-dir":
            blend_dir = argv[i + 1]
    for name in only:
        build_character(name, outdir, blend_dir)


if __name__ == "__main__":
    main()
