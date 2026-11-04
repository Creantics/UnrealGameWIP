#!/usr/bin/env python3
"""Animation curve report for skinned glTF (.glb) meshes.

Pure Python 3 + numpy (matplotlib is optional, used only for PNG plots).
Parses the .glb itself (JSON + BIN chunks), resamples every animation at its
own frame rate, runs forward kinematics and reports, per clip:

  * peak angular speed / acceleration / jerk (top joints), spike flags
  * one-frame pose jumps
  * holds (dead stops) and C1 breaks (abrupt angular-velocity changes)
  * foot-lock drift
  * root and pelvis world horizontal speed
  * loop closure (loop clips)
  * boundary pose differences against the idle clip (non-loop clips)

Usage:
  python tools/anim/clip_curves.py <file.glb> <out_dir> [--clips <clip_table.json>]
                                   [--idle idle] [--up y|z] [--scale 1.0]

Outputs in <out_dir>: curves.json (everything), curves.md (summary),
<clip>.csv (per frame), <clip>.png (if matplotlib is installed).

Conventions
  * Angles are in degrees. Rotation steps use the local joint quaternions
    (q and -q are the same rotation).
  * omega[i]  = rotation vector of q[i]*conj(q[i-1]) * fps      (deg/s, frame i-1 -> i)
    accel[i]  = (omega[i]-omega[i-1]) * fps, as a vector magnitude (deg/s^2)
    jerk[i]   = (accel_vec[i]-accel_vec[i-1]) * fps, magnitude    (deg/s^3)
    CSV rows are frame-aligned with these backward differences (value at row i
    uses frames <= i). Event times are physical centres: omega (i-0.5)/fps,
    accel (i-1)/fps (the pivot frame between two intervals), jerk (i-1.5)/fps.
  * Up axis is Y (glTF) unless --up z. Positions are metres * --scale.
"""
import argparse
import csv
import json
import math
import os
import re
import struct
import sys

import numpy as np

# ---------------------------------------------------------------- thresholds
SPIKE_RATIO = 6.0          # spike: value > 6x the joint's median of non-zero values ...
ACCEL_FLOOR = 3000.0       # ... and above this absolute floor, deg/s^2 (100 deg/s change in 1 frame at 30 fps)
JERK_FLOOR = 90000.0       # deg/s^3
NONZERO_EPS = 1.0          # values <= this are "zero" for the median (deg/s^2, deg/s^3)
JUMP_RATIO = 4.0           # one-frame jump: step > 4x BOTH neighbouring steps ...
JUMP_MIN_DEG = 3.0         # ... and larger than this many degrees in one frame
HOLD_SPEED = 1.0           # deg/s, a joint is "still" below this
HOLD_FRAC = 0.95           # an interval is a hold when >= 95% of joints are still
C1_ABS = 40.0              # C1 break: major joint |omega change| in one frame > 40 deg/s ...
C1_RATIO = 3.0             # ... and > 3x the median change over the 3 frames either side (ramps and eases pass)
C1_WINDOW = 3
CORE_ACTIVE_DEG_S = 5.0    # core holds: a major joint is "active" if its peak speed in the clip exceeds this
CORE_HOLD_FRAC = 0.75      # core hold: >= 75% of active major joints under HOLD_SPEED
FOOT_BAND_M = 0.03         # planted: within 3 cm of the joint's minimum height ...
FOOT_SPEED = 0.15          # ... and horizontal speed under 0.15 m/s
FOOT_MIN_SPAN = 3          # frames
FOOT_DRIFT_CM = 2.0        # flag above this
LOOP_POSE_DEG = 0.5        # loop pose diff PASS below this (max joint angle)
LOOP_VEL_DEG_S = 10.0      # loop velocity diff PASS below this (max joint, deg/s),
LOOP_SEAM_WINDOW = 3       # ... or at most LOOP_SEAM_RATIO x the joint's largest step-to-step velocity change in the
LOOP_SEAM_RATIO = 1.5      #     LOOP_SEAM_WINDOW frames either side of the seam (fast motion that is still smooth)
BOUNDARY_NOTE_DEG = 15.0   # boundary diff above this is marked NOTE
MAX_LIST = 40              # max rows listed per section in curves.md

FINGER_RE = re.compile(r"thumb|index|middle|ring|pinky|finger", re.I)
HELPER_RE = re.compile(r"twist|corrective|roll|helper|ik_|_ik|pole", re.I)
FOOT_RE = re.compile(r"foot|ball|toe", re.I)
FOOT_EXCL_RE = re.compile(r"eye|finger|thumb", re.I)

# ----------------------------------------------------------------- glb parse
COMP = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16,
        5125: np.uint32, 5126: np.float32}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT2": 4, "MAT3": 9, "MAT4": 16}


def load_glb(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, _version, _length = struct.unpack_from("<4sII", data, 0)
    if magic != b"glTF":
        raise ValueError("not a .glb file")
    off = 12
    js = None
    binc = b""
    while off + 8 <= len(data):
        clen, ctype = struct.unpack_from("<I4s", data, off)
        chunk = data[off + 8: off + 8 + clen]
        if ctype == b"JSON":
            js = json.loads(chunk.decode("utf-8"))
        elif ctype == b"BIN\x00" and not binc:
            binc = chunk
        off += 8 + clen
    if js is None:
        raise ValueError("no JSON chunk")
    return js, binc


def read_accessor(js, binc, idx):
    acc = js["accessors"][idx]
    dt = np.dtype(COMP[acc["componentType"]]).newbyteorder("<")
    n = NCOMP[acc["type"]]
    count = acc["count"]
    esize = dt.itemsize * n
    out = np.zeros((count, n), dtype=dt)
    if "bufferView" in acc:
        bv = js["bufferViews"][acc["bufferView"]]
        base = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
        stride = bv.get("byteStride", 0) or esize
        if stride == esize:
            out = np.frombuffer(binc, dtype=dt, count=count * n, offset=base).reshape(count, n).copy()
        else:
            raw = b"".join(binc[base + i * stride: base + i * stride + esize] for i in range(count))
            out = np.frombuffer(raw, dtype=dt).reshape(count, n).copy()
    sp = acc.get("sparse")
    if sp:
        ic = sp["indices"]
        ibv = js["bufferViews"][ic["bufferView"]]
        idt = np.dtype(COMP[ic["componentType"]]).newbyteorder("<")
        idx_arr = np.frombuffer(binc, dtype=idt, count=sp["count"],
                                offset=ibv.get("byteOffset", 0) + ic.get("byteOffset", 0))
        vv = sp["values"]
        vbv = js["bufferViews"][vv["bufferView"]]
        vals = np.frombuffer(binc, dtype=dt, count=sp["count"] * n,
                             offset=vbv.get("byteOffset", 0) + vv.get("byteOffset", 0)).reshape(sp["count"], n)
        out[idx_arr.astype(np.int64)] = vals
    out = out.astype(np.float64)
    if acc.get("normalized"):
        ct = acc["componentType"]
        if ct == 5120:
            out = np.maximum(out / 127.0, -1.0)
        elif ct == 5121:
            out = out / 255.0
        elif ct == 5122:
            out = np.maximum(out / 32767.0, -1.0)
        elif ct == 5123:
            out = out / 65535.0
    return out


# ------------------------------------------------------------- quaternions (xyzw)
def qmul(a, b):
    ax, ay, az, aw = a[..., 0], a[..., 1], a[..., 2], a[..., 3]
    bx, by, bz, bw = b[..., 0], b[..., 1], b[..., 2], b[..., 3]
    return np.stack([aw * bx + ax * bw + ay * bz - az * by,
                     aw * by - ax * bz + ay * bw + az * bx,
                     aw * bz + ax * by - ay * bx + az * bw,
                     aw * bw - ax * bx - ay * by - az * bz], axis=-1)


def qconj(a):
    return a * np.array([-1.0, -1.0, -1.0, 1.0])


def qnorm(a):
    n = np.linalg.norm(a, axis=-1, keepdims=True)
    return a / np.where(n > 1e-12, n, 1.0)


def qangle_deg(a, b):
    """Angle between two rotations (handles q / -q)."""
    r = qmul(a, qconj(b))
    return np.degrees(2.0 * np.arctan2(np.linalg.norm(r[..., :3], axis=-1), np.abs(r[..., 3])))


def qrotvec_deg(q):
    """Rotation vector in degrees, shortest way round."""
    q = np.where(q[..., 3:4] < 0, -q, q)
    v = q[..., :3]
    n = np.linalg.norm(v, axis=-1, keepdims=True)
    ang = 2.0 * np.arctan2(n, q[..., 3:4])
    fac = np.where(n > 1e-12, ang / np.where(n > 1e-12, n, 1.0), 2.0)
    return np.degrees(v * fac)


def qmat(q):
    x, y, z, w = q[..., 0], q[..., 1], q[..., 2], q[..., 3]
    m = np.zeros(q.shape[:-1] + (3, 3))
    m[..., 0, 0] = 1 - 2 * (y * y + z * z)
    m[..., 0, 1] = 2 * (x * y - z * w)
    m[..., 0, 2] = 2 * (x * z + y * w)
    m[..., 1, 0] = 2 * (x * y + z * w)
    m[..., 1, 1] = 1 - 2 * (x * x + z * z)
    m[..., 1, 2] = 2 * (y * z - x * w)
    m[..., 2, 0] = 2 * (x * z - y * w)
    m[..., 2, 1] = 2 * (y * z + x * w)
    m[..., 2, 2] = 1 - 2 * (x * x + y * y)
    return m


def mat_to_quat(m):
    t = m[0, 0] + m[1, 1] + m[2, 2]
    if t > 0:
        s = math.sqrt(t + 1.0) * 2
        q = [(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, 0.25 * s]
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = math.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = [0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s, (m[2, 1] - m[1, 2]) / s]
    elif m[1, 1] > m[2, 2]:
        s = math.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = [(m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s, (m[0, 2] - m[2, 0]) / s]
    else:
        s = math.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = [(m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s, (m[1, 0] - m[0, 1]) / s]
    return qnorm(np.array(q))


# ------------------------------------------------------------------ sampling
def slerp(a, b, s):
    """a, b (N,4), s (N,) -> (N,4)."""
    d = np.sum(a * b, axis=-1)
    b = np.where(d[:, None] < 0, -b, b)
    d = np.abs(d)
    lin = d > 0.9995
    th = np.arccos(np.clip(d, -1, 1))
    sth = np.sin(th)
    sth_safe = np.where(sth > 1e-9, sth, 1.0)
    wa = np.where(lin, 1 - s, np.sin((1 - s) * th) / sth_safe)
    wb = np.where(lin, s, np.sin(s * th) / sth_safe)
    return qnorm(wa[:, None] * a + wb[:, None] * b)


def sample_channel(times, values, interp, t_out, is_rot):
    n = len(times)
    t_out = np.clip(t_out, times[0], times[-1])
    if n == 1:
        v = values[1] if interp == "CUBICSPLINE" else values[0]
        return np.tile(v, (len(t_out), 1))
    idx = np.clip(np.searchsorted(times, t_out, side="right") - 1, 0, n - 2)
    t0, t1 = times[idx], times[idx + 1]
    dt = t1 - t0
    s = np.where(dt > 0, (t_out - t0) / np.where(dt > 0, dt, 1.0), 0.0)
    if interp == "STEP":
        out = values[idx].copy()
        out[t_out >= times[-1]] = values[-1]
        return out
    if interp == "CUBICSPLINE":
        v0, b0 = values[3 * idx + 1], values[3 * idx + 2]
        a1, v1 = values[3 * (idx + 1)], values[3 * (idx + 1) + 1]
        s_ = s[:, None]
        d_ = dt[:, None]
        out = ((2 * s_ ** 3 - 3 * s_ ** 2 + 1) * v0 + (s_ ** 3 - 2 * s_ ** 2 + s_) * d_ * b0
               + (-2 * s_ ** 3 + 3 * s_ ** 2) * v1 + (s_ ** 3 - s_ ** 2) * d_ * a1)
        return qnorm(out) if is_rot else out
    a, b = values[idx], values[idx + 1]
    if is_rot:
        return slerp(a, b, s)
    return a + (b - a) * s[:, None]


# --------------------------------------------------------------------- scene
class Rig:
    pass


def build_rig(js):
    nodes = js["nodes"]
    nn = len(nodes)
    rig = Rig()
    rig.names = [nd.get("name", "node%d" % i) for i, nd in enumerate(nodes)]
    rig.parent = [-1] * nn
    for i, nd in enumerate(nodes):
        for c in nd.get("children", []):
            rig.parent[c] = i
    rest_t = np.zeros((nn, 3))
    rest_r = np.tile([0.0, 0.0, 0.0, 1.0], (nn, 1))
    rest_s = np.ones((nn, 3))
    for i, nd in enumerate(nodes):
        if "matrix" in nd:
            m = np.array(nd["matrix"], dtype=np.float64).reshape(4, 4).T
            rest_t[i] = m[:3, 3]
            sc = np.linalg.norm(m[:3, :3], axis=0)
            sc = np.where(sc > 1e-12, sc, 1.0)
            rest_s[i] = sc
            rest_r[i] = mat_to_quat(m[:3, :3] / sc)
        else:
            if "translation" in nd:
                rest_t[i] = nd["translation"]
            if "rotation" in nd:
                rest_r[i] = qnorm(np.array(nd["rotation"], dtype=np.float64))
            if "scale" in nd:
                rest_s[i] = nd["scale"]
    rig.rest_t, rig.rest_r, rig.rest_s = rest_t, rest_r, rest_s
    order, seen = [], [False] * nn

    def visit(i):
        if seen[i]:
            return
        if rig.parent[i] >= 0:
            visit(rig.parent[i])
        seen[i] = True
        order.append(i)
    for i in range(nn):
        visit(i)
    rig.order = order
    return rig


def pick_joints(js, rig):
    skins = js.get("skins", [])
    if skins:
        sk = max(skins, key=lambda s: len(s["joints"]))
        return list(sk["joints"]), sk.get("skeleton")
    tgt = []
    for a in js.get("animations", []):
        for ch in a["channels"]:
            n = ch["target"].get("node")
            if n is not None and n not in tgt and ch["target"]["path"] in ("translation", "rotation", "scale"):
                tgt.append(n)
    return sorted(tgt), None


def load_animation(js, binc, rig, ai):
    anim = js["animations"][ai]
    name = anim.get("name") or "anim%d" % ai
    chans = []
    all_times = []
    for ch in anim["channels"]:
        path = ch["target"]["path"]
        node = ch["target"].get("node")
        if node is None or path not in ("translation", "rotation", "scale"):
            continue
        smp = anim["samplers"][ch["sampler"]]
        t = read_accessor(js, binc, smp["input"])[:, 0]
        v = read_accessor(js, binc, smp["output"])
        chans.append((node, path, t, v, smp.get("interpolation", "LINEAR")))
        all_times.append(t)
    if not chans:
        return None
    big = max(all_times, key=len)
    t0 = min(t[0] for t in all_times)
    t1 = max(t[-1] for t in all_times)
    dts = np.diff(big)
    dts = dts[dts > 1e-9]
    fps = 1.0 / float(np.median(dts)) if len(dts) else 30.0
    if abs(fps - round(fps)) < 0.002 * fps:
        fps = float(round(fps))
    nf = int(round((t1 - t0) * fps)) + 1
    t_out = t0 + np.arange(nf) / fps
    nn = len(rig.names)
    T = np.broadcast_to(rig.rest_t[:, None, :], (nn, nf, 3)).copy()
    R = np.broadcast_to(rig.rest_r[:, None, :], (nn, nf, 4)).copy()
    S = np.broadcast_to(rig.rest_s[:, None, :], (nn, nf, 3)).copy()
    animated = {"translation": set(), "rotation": set(), "scale": set()}
    for node, path, t, v, interp in chans:
        if path == "rotation":
            R[node] = sample_channel(t, v, interp, t_out, True)
        elif path == "translation":
            T[node] = sample_channel(t, v, interp, t_out, False)
        else:
            S[node] = sample_channel(t, v, interp, t_out, False)
        animated[path].add(node)
    return dict(name=name, fps=fps, nf=nf, t0=float(t0), duration=float(t1 - t0), T=T, R=R, S=S,
                animated=animated, keys=max(len(t) for t in all_times))


def forward_kinematics(rig, clip):
    nn, nf = len(rig.names), clip["nf"]
    W = np.zeros((nn, nf, 4, 4))
    for i in rig.order:
        L = np.zeros((nf, 4, 4))
        L[:, :3, :3] = qmat(clip["R"][i]) * clip["S"][i][:, None, :]
        L[:, :3, 3] = clip["T"][i]
        L[:, 3, 3] = 1.0
        p = rig.parent[i]
        W[i] = L if p < 0 else W[p] @ L
    return W[:, :, :3, 3]  # (nn, nf, 3) world positions


# ----------------------------------------------------------------- analysis
def spans_from_mask(mask):
    spans, start = [], None
    for i, m in enumerate(mask):
        if m and start is None:
            start = i
        elif not m and start is not None:
            spans.append((start, i - 1))
            start = None
    if start is not None:
        spans.append((start, len(mask) - 1))
    return spans


def derivatives(Q, fps):
    """Q (J,F,4) local rotations -> omega, accel, jerk vectors (J,F,3), zero before defined."""
    J, F, _ = Q.shape
    om = np.zeros((J, F, 3))
    ac = np.zeros((J, F, 3))
    je = np.zeros((J, F, 3))
    if F > 1:
        om[:, 1:] = qrotvec_deg(qmul(Q[:, 1:], qconj(Q[:, :-1]))) * fps
    if F > 2:
        ac[:, 2:] = (om[:, 2:] - om[:, 1:-1]) * fps
    if F > 3:
        je[:, 3:] = (ac[:, 3:] - ac[:, 2:-1]) * fps
    return om, ac, je


def top_peaks(mag, jn, start, tfun, k=5):
    if mag.shape[1] <= start:
        return []
    sub = mag[:, start:]
    pk = sub.max(axis=1)
    arg = sub.argmax(axis=1) + start
    order = np.argsort(-pk)[:k]
    return [dict(joint=jn[j], value=float(pk[j]), frame=int(arg[j]), time_s=float(tfun(arg[j]))) for j in order]


def spike_flags(mag, jn, start, floor, tfun, major):
    out = []
    for j in range(mag.shape[0]):
        v = mag[j, start:]
        nz = v[v > NONZERO_EPS]
        if len(nz) < 3:
            continue
        med = float(np.median(nz))
        for k in np.nonzero((v > SPIKE_RATIO * med) & (v > floor))[0]:
            i = int(k + start)
            out.append(dict(joint=jn[j], frame=i, time_s=float(tfun(i)), value=float(v[k]),
                            ratio=float(v[k] / med), major=bool(major[j])))
    out.sort(key=lambda e: -e["value"])
    return out


def analyse_clip(rig, joints, jn, clip, P, is_loop, impact_s, root_i, pelvis_i, up, foot_idx, major):
    fps, F = clip["fps"], clip["nf"]
    Q = clip["R"][joints]                          # (J,F,4)
    om, ac, je = derivatives(Q, fps)
    sp = np.linalg.norm(om, axis=2)
    am = np.linalg.norm(ac, axis=2)
    jm = np.linalg.norm(je, axis=2)
    res = dict(name=clip["name"], fps=fps, frames=F, duration_s=clip["duration"], keys=clip["keys"],
               loop=bool(is_loop), impact_s=impact_s)
    jset = set(joints)
    res["animated_joints"] = {k: sorted(rig.names[n] for n in v if n in jset) for k, v in clip["animated"].items()}
    t_om = lambda i: (i - 0.5) / fps
    t_ac = lambda i: (i - 1) / fps
    t_je = lambda i: (i - 1.5) / fps
    res["peaks"] = dict(speed_deg_s=top_peaks(sp, jn, 1, t_om), accel_deg_s2=top_peaks(am, jn, 2, t_ac),
                        jerk_deg_s3=top_peaks(jm, jn, 3, t_je))
    res["spikes"] = dict(accel=spike_flags(am, jn, 2, ACCEL_FLOOR, t_ac, major),
                         jerk=spike_flags(jm, jn, 3, JERK_FLOOR, t_je, major))

    # one-frame pose jumps
    step = sp / fps                                 # deg per frame, index i = frame i-1 -> i
    jumps = []
    for j in range(len(joints)):
        s = step[j]
        for i in range(1, F):
            prev = s[i - 1] if i - 1 >= 1 else None
            nxt = s[i + 1] if i + 1 < F else None
            nb = [x for x in (prev, nxt) if x is not None]
            if not nb or s[i] < JUMP_MIN_DEG:
                continue
            if all(s[i] > JUMP_RATIO * max(x, 1e-9) for x in nb):
                jumps.append(dict(joint=jn[j], frame=i, time_s=float(t_om(i)), step_deg=float(s[i]),
                                  prev_deg=None if prev is None else float(prev),
                                  next_deg=None if nxt is None else float(nxt)))
    jumps.sort(key=lambda e: -e["step_deg"])
    res["jumps"] = jumps

    # holds (non-loop only)
    holds = []
    if not is_loop and F > 1:
        still = (sp[:, 1:] < HOLD_SPEED).mean(axis=0) >= HOLD_FRAC      # interval i=1..F-1
        for a, b in spans_from_mask(still):
            ia, ib = a + 1, b + 1
            tag = "lead-in" if ia == 1 else ("tail" if ib == F - 1 else "mid")
            holds.append(dict(start_s=float((ia - 1) / fps), end_s=float(ib / fps),
                              duration_s=float((ib - ia + 1) / fps), frames=int(ib - ia + 1), where=tag))
    res["holds"] = holds
    res["hold_frames"] = int(sum(h["frames"] for h in holds))
    res["hold_frames_mid"] = int(sum(h["frames"] for h in holds if h["where"] == "mid"))
    # core holds: same idea over the active major joints only (fingers and hand flutter do not hide a stop)
    core_holds = []
    act = major & (sp.max(axis=1) > CORE_ACTIVE_DEG_S)
    if not is_loop and F > 1 and act.any():
        still = (sp[act][:, 1:] < HOLD_SPEED).mean(axis=0) >= CORE_HOLD_FRAC
        for a, b in spans_from_mask(still):
            ia, ib = a + 1, b + 1
            tag = "lead-in" if ia == 1 else ("tail" if ib == F - 1 else "mid")
            core_holds.append(dict(start_s=float((ia - 1) / fps), end_s=float(ib / fps),
                                   duration_s=float((ib - ia + 1) / fps), frames=int(ib - ia + 1), where=tag))
    res["core_holds"] = core_holds
    res["core_hold_frames"] = int(sum(h["frames"] for h in core_holds))
    res["core_hold_frames_mid"] = int(sum(h["frames"] for h in core_holds if h["where"] == "mid"))

    # C1 breaks: major joints, omega vector change at pivot frame p (between intervals p and p+1)
    dv = np.zeros((len(joints), F))
    if F > 2:
        dv[:, 1:F - 1] = np.linalg.norm(om[:, 2:] - om[:, 1:-1], axis=2)
    c1 = {}
    for j in range(len(joints)):
        if not major[j]:
            continue
        for p in range(1, F - 1):
            nb = [dv[j, q] for q in range(max(1, p - C1_WINDOW), min(F - 2, p + C1_WINDOW) + 1) if q != p]
            v = dv[j, p]
            if v > C1_ABS and nb and v > C1_RATIO * max(float(np.median(nb)), 1e-9):
                c1.setdefault(p, []).append((jn[j], float(v)))
    c1l = []
    for p in sorted(c1):
        js_ = sorted(c1[p], key=lambda e: -e[1])
        c1l.append(dict(frame=p, time_s=float(p / fps), joints=len(js_), worst_joint=js_[0][0],
                        worst_delta_deg_s=js_[0][1]))
    res["c1_breaks"] = c1l

    # foot lock
    hz = [i for i in range(3) if i != up]
    feet = []
    for fi in foot_idx:
        pos = P[fi]
        h = pos[:, up]
        hor = pos[:, hz]
        v = np.zeros(F)
        if F > 2:
            v[1:-1] = np.linalg.norm(hor[2:] - hor[:-2], axis=1) * fps / 2.0
            v[0], v[-1] = v[1], v[-2]
        planted = ((h - h.min()) < FOOT_BAND_M) & (v < FOOT_SPEED)
        spans, worst = [], 0.0
        for a, b in spans_from_mask(planted):
            if b - a + 1 < FOOT_MIN_SPAN:
                continue
            seg = hor[a:b + 1]
            d = np.linalg.norm(seg[:, None, :] - seg[None, :, :], axis=2).max() * 100.0
            spans.append(dict(start_s=float(a / fps), end_s=float(b / fps), drift_cm=float(d)))
            worst = max(worst, d)
        feet.append(dict(joint=rig.names[fi], planted_frames=int(planted.sum()), spans=spans,
                         max_drift_cm=float(worst), flag=bool(worst > FOOT_DRIFT_CM),
                         min_height_m=float(h.min()),
                         path_length_cm=float(np.linalg.norm(np.diff(hor, axis=0), axis=1).sum() * 100)))
    res["feet"] = feet

    # root speed
    def hspeed(ni):
        hor = P[ni][:, hz]
        s = np.zeros(F)
        if F > 1:
            s[1:] = np.linalg.norm(np.diff(hor, axis=0), axis=1) * fps
        return s
    rs, ps = hspeed(root_i), hspeed(pelvis_i)
    res["root"] = dict(joint=rig.names[root_i], max_mps=float(rs.max()), mean_mps=float(rs[1:].mean()) if F > 1 else 0.0)
    res["pelvis"] = dict(joint=rig.names[pelvis_i], max_mps=float(ps.max()), mean_mps=float(ps[1:].mean()) if F > 1 else 0.0)

    # loop closure
    if is_loop and F > 2:
        pose = qangle_deg(Q[:, 0], Q[:, -1])
        vd = np.linalg.norm(om[:, -1] - om[:, 1], axis=1)
        # A seamless loop still changes velocity across the seam by one frame of its acceleration, so the seam's change is
        # also judged against the same joint's step-to-step changes in the LOOP_SEAM_WINDOW frames either side of it.
        dv = np.linalg.norm(np.diff(om[:, 1:], axis=1), axis=2)            # (J, F-2): change from step k to step k+1
        w = min(LOOP_SEAM_WINDOW, dv.shape[1])
        near = np.maximum(dv[:, :w].max(axis=1), dv[:, -w:].max(axis=1)) if w else np.zeros(len(vd))
        excess = vd - LOOP_SEAM_RATIO * near
        seam_ok = bool(np.all((vd < LOOP_VEL_DEG_S) | (excess <= 0.0)))
        rootd = np.linalg.norm(P[root_i][0] - P[root_i][-1]) * 100.0
        res["loop_check"] = dict(pose_max_deg=float(pose.max()), pose_mean_deg=float(pose.mean()),
                                 pose_worst_joint=jn[int(np.argmax(pose))], vel_max_deg_s=float(vd.max()),
                                 vel_mean_deg_s=float(vd.mean()), vel_worst_joint=jn[int(np.argmax(vd))],
                                 seam_ok=seam_ok, seam_worst_excess_deg_s=float(excess.max()),
                                 seam_worst_joint=jn[int(np.argmax(excess))], root_pos_diff_cm=float(rootd))
    series = dict(speed=sp, accel=am, jerk=jm, root_speed=rs, pelvis_speed=ps)
    return res, series


def pose_diff(Qa, Qb, jn, Ta=None, Tb=None):
    ang = qangle_deg(Qa, Qb)
    d = dict(max_deg=float(ang.max()), mean_deg=float(ang.mean()), worst_joint=jn[int(np.argmax(ang))])
    if Ta is not None:
        td = np.linalg.norm(Ta - Tb, axis=-1) * 100.0
        d["max_translation_cm"] = float(td.max())
        d["worst_translation_joint"] = jn[int(np.argmax(td))]
    return d


def boundary(clips_q, clips_t, name, idle, jn):
    q, t = clips_q[name], clips_t[name]
    qi, ti = clips_q[idle], clips_t[idle]
    out = dict(idle=idle)
    out["first_vs_idle0"] = pose_diff(q[:, 0], qi[:, 0], jn, t[:, 0], ti[:, 0])
    out["last_vs_idle0"] = pose_diff(q[:, -1], qi[:, 0], jn, t[:, -1], ti[:, 0])
    Fi = qi.shape[1]
    allf = np.array([qangle_deg(q[:, 0], qi[:, f]).max() for f in range(Fi)])
    fw, fb = int(np.argmax(allf)), int(np.argmin(allf))
    out["first_vs_any_idle_frame"] = dict(worst_max_deg=float(allf[fw]), worst_idle_frame=fw,
                                          best_max_deg=float(allf[fb]), best_idle_frame=fb,
                                          worst=pose_diff(q[:, 0], qi[:, fw], jn))
    allb = np.array([qangle_deg(q[:, -1], qi[:, f]).max() for f in range(Fi)])
    bw, bb = int(np.argmax(allb)), int(np.argmin(allb))
    out["last_vs_any_idle_frame"] = dict(worst_max_deg=float(allb[bw]), worst_idle_frame=bw,
                                         best_max_deg=float(allb[bb]), best_idle_frame=bb)
    return out


# ------------------------------------------------------------------- output
def clean(o, nd=3):
    if isinstance(o, dict):
        return {k: clean(v, nd) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [clean(v, nd) for v in o]
    if isinstance(o, np.ndarray):
        return clean(o.tolist(), nd)
    if isinstance(o, (np.floating, float)):
        f = float(o)
        return None if (math.isnan(f) or math.isinf(f)) else round(f, nd)
    if isinstance(o, np.integer):
        return int(o)
    return o


def safe_name(s):
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", s)


def write_csv(path, res, series, jn, fps):
    head = ["frame", "time_s", "root_speed_mps", "pelvis_speed_mps"]
    head += ["speed_" + n for n in jn] + ["accel_" + n for n in jn] + ["jerk_" + n for n in jn]
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(head)
        for i in range(res["frames"]):
            row = [i, "%.4f" % (i / fps), "%.4f" % series["root_speed"][i], "%.4f" % series["pelvis_speed"][i]]
            for key, first in (("speed", 1), ("accel", 2), ("jerk", 3)):
                col = series[key][:, i]
                row += ["" if i < first else "%.2f" % x for x in col]
            w.writerow(row)


def plot_clip(path, res, series, jn, plt):
    fps, F = res["fps"], res["frames"]
    t = (np.arange(F) - 0.5) / fps
    sp = series["speed"]
    top = np.argsort(-sp.max(axis=1))[:6]
    fig, (a1, a2) = plt.subplots(2, 1, figsize=(11, 6.5), sharex=True, gridspec_kw=dict(height_ratios=[3, 1.3]))
    for j in top:
        a1.plot(t[1:], sp[j, 1:], lw=1.1, label=jn[j])
    for h in res["core_holds"]:
        a1.axvspan(h["start_s"], h["end_s"], color="0.85", zorder=0)
    for b in res["c1_breaks"]:
        a1.axvline(b["time_s"], color="tab:red", lw=0.5, alpha=0.4)
    a1.set_ylabel("angular speed (deg/s)")
    a1.set_title("%s  (%d frames @ %g fps)  grey=core hold, red=C1 break" % (res["name"], F, fps))
    a2.plot(t[1:], series["root_speed"][1:], lw=1.1, color="k", label="root " + res["root"]["joint"])
    if res["pelvis"]["joint"] != res["root"]["joint"]:
        a2.plot(t[1:], series["pelvis_speed"][1:], lw=1.1, color="tab:green", label="pelvis")
    a2.set_ylabel("root speed (m/s)")
    a2.set_xlabel("time (s)")
    if res.get("impact_s"):
        for ax in (a1, a2):
            ax.axvline(res["impact_s"], color="tab:orange", lw=1.6, ls="--")
        a1.text(res["impact_s"], a1.get_ylim()[1] * 0.97, " impact", color="tab:orange", va="top")
    a1.legend(loc="upper left", fontsize=8, ncol=3)
    a2.legend(loc="upper left", fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=100)
    plt.close(fig)


def times_str(items, key="time_s", n=12):
    s = ", ".join("%.3f" % e[key] for e in items[:n])
    return s + (", ... (+%d)" % (len(items) - n) if len(items) > n else "")


def mark(ok):
    return "PASS" if ok else "FLAG"


def write_md(path, meta, results):
    L = []
    L.append("# Animation curve report: %s\n" % meta["file"])
    L.append("Joints: %d (%d major). Root: `%s`, pelvis: `%s`. Rest height: %.2f m. Idle reference: `%s`.\n"
             % (meta["joints"], meta["major_joints"], meta["root"], meta["pelvis"], meta["rest_height_m"], meta["idle"]))
    L.append("Feet: %s\n" % (", ".join("`%s`" % f for f in meta["foot_joints"]) or "none found"))
    L.append("Thresholds: spike = value > %gx joint's median of non-zero values and accel > %g deg/s^2 or jerk > %g deg/s^3; "
             "jump = step > %gx both neighbours and > %g deg; hold = >= %d%% of all joints under %g deg/s (core hold: >= 75%% of active major joints); "
             "C1 break = major joint omega change > %g deg/s in one frame and > %gx the median change over the %d frames either side; "
             "planted foot = within %g cm of its minimum height and under %g m/s; "
             "marks: foot drift <= %g cm, no jumps, loop pose < %g deg and loop velocity < %g deg/s or within %gx its change in the %d frames either side of the seam.\n"
             % (SPIKE_RATIO, ACCEL_FLOOR, JERK_FLOOR, JUMP_RATIO, JUMP_MIN_DEG, int(HOLD_FRAC * 100), HOLD_SPEED,
                C1_ABS, C1_RATIO, C1_WINDOW, FOOT_BAND_M * 100, FOOT_SPEED, FOOT_DRIFT_CM, LOOP_POSE_DEG, LOOP_VEL_DEG_S, LOOP_SEAM_RATIO, LOOP_SEAM_WINDOW))
    L.append("## Summary\n")
    L.append("| clip | frames | fps | dur s | loop | peak speed | peak accel | spike frames a/j | jumps | holds mid/all, core mid/all (frames) | C1 breaks | foot drift cm | root max m/s | loop pose deg / vel deg/s | foot | jumps | loop |")
    L.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in results:
        pk = r["peaks"]
        ps = (pk["speed_deg_s"] or [None])[0]
        pa = (pk["accel_deg_s2"] or [None])[0]
        drift = max([f["max_drift_cm"] for f in r["feet"]] or [0.0])
        lc = r.get("loop_check")
        lp = "%.2f / %.1f" % (lc["pose_max_deg"], lc["vel_max_deg_s"]) if lc else "-"
        lm = mark(lc["pose_max_deg"] < LOOP_POSE_DEG and lc["seam_ok"]) if lc else "-"
        L.append("| %s | %d | %g | %.3f | %s | %s | %s | %d / %d | %d | %s | %d | %.2f | %.3f | %s | %s | %s | %s |" % (
            r["name"], r["frames"], r["fps"], r["duration_s"], "yes" if r["loop"] else "no",
            ("%.0f %s" % (ps["value"], ps["joint"])) if ps else "-",
            ("%.0f %s" % (pa["value"], pa["joint"])) if pa else "-",
            len({e["frame"] for e in r["spikes"]["accel"]}), len({e["frame"] for e in r["spikes"]["jerk"]}), len(r["jumps"]),
            "%d/%d, %d/%d" % (r["hold_frames_mid"], r["hold_frames"], r["core_hold_frames_mid"], r["core_hold_frames"]), len(r["c1_breaks"]), drift, r["root"]["max_mps"], lp,
            mark(drift <= FOOT_DRIFT_CM) if r["feet"] else "-", mark(not r["jumps"]), lm))
    L.append("")
    for r in results:
        L.append("## %s\n" % r["name"])
        L.append("%d frames @ %g fps, %.3f s, %d keyframes on the longest channel, %s clip%s." % (
            r["frames"], r["fps"], r["duration_s"], r["keys"], "loop" if r["loop"] else "non-loop",
            (", impact %.3f s" % r["impact_s"]) if r.get("impact_s") else ""))
        if r.get("table_length_s") is not None:
            L.append("Clip table length_s %.3f vs glb duration %.3f." % (r["table_length_s"], r["duration_s"]))
        an = r["animated_joints"]
        L.append("Animated joints: rotation %d, translation %d, scale %d.\n" % (
            len(an["rotation"]), len(an["translation"]), len(an["scale"])))
        for label, key in (("Speed (deg/s)", "speed_deg_s"), ("Accel (deg/s^2)", "accel_deg_s2"),
                           ("Jerk (deg/s^3)", "jerk_deg_s3")):
            L.append("**%s top 5:** %s\n" % (label, "; ".join("%s %.0f @%.3fs" % (p["joint"], p["value"], p["time_s"])
                                                             for p in r["peaks"][key])))
        for label, key in (("Accel spikes", "accel"), ("Jerk spikes", "jerk")):
            sl = r["spikes"][key]
            txt = ""
            if sl:
                txt = " " + "; ".join("%s @%.3fs %.0f (x%.0f)" % (e["joint"], e["time_s"], e["value"], e["ratio"])
                                      for e in sl[:10]) + (" ..." if len(sl) > 10 else "")
            L.append("**%s: %d**%s\n" % (label, len(sl), txt))
        L.append("**One-frame jumps: %d** %s" % (len(r["jumps"]), mark(not r["jumps"])))
        for e in r["jumps"][:MAX_LIST]:
            L.append("- %s @%.3fs: %.1f deg in one frame (neighbours %s / %s)" % (
                e["joint"], e["time_s"], e["step_deg"],
                "-" if e["prev_deg"] is None else "%.2f" % e["prev_deg"],
                "-" if e["next_deg"] is None else "%.2f" % e["next_deg"]))
        L.append("")
        if not r["loop"]:
            L.append("**Holds:** %d frames (%d mid-clip) in %d spans" % (r["hold_frames"], r["hold_frames_mid"], len(r["holds"])))
            for h in r["holds"][:MAX_LIST]:
                L.append("- %.3f to %.3f s (%d frames, %s)" % (h["start_s"], h["end_s"], h["frames"], h["where"]))
            L.append("**Core holds** (active major joints): %d frames (%d mid-clip) in %d spans" % (
                r["core_hold_frames"], r["core_hold_frames_mid"], len(r["core_holds"])))
            for h in r["core_holds"][:MAX_LIST]:
                L.append("- %.3f to %.3f s (%d frames, %s)" % (h["start_s"], h["end_s"], h["frames"], h["where"]))
            L.append("")
        L.append("**C1 breaks (major joints): %d frames.** Times: %s" % (len(r["c1_breaks"]), times_str(r["c1_breaks"], n=30)))
        for b in r["c1_breaks"][:MAX_LIST]:
            L.append("- %.3f s: %d joints, worst %s (%.0f deg/s change)" % (b["time_s"], b["joints"], b["worst_joint"], b["worst_delta_deg_s"]))
        L.append("")
        if r["feet"]:
            L.append("**Foot lock:**")
            for f in r["feet"]:
                L.append("- %s: planted %d frames, %d spans, max drift %.2f cm %s" % (
                    f["joint"], f["planted_frames"], len(f["spans"]), f["max_drift_cm"], mark(not f["flag"])))
            L.append("")
        L.append("**Root speed:** `%s` max %.3f / mean %.3f m/s; pelvis `%s` max %.3f / mean %.3f m/s.\n" % (
            r["root"]["joint"], r["root"]["max_mps"], r["root"]["mean_mps"],
            r["pelvis"]["joint"], r["pelvis"]["max_mps"], r["pelvis"]["mean_mps"]))
        lc = r.get("loop_check")
        if lc:
            ok = lc["pose_max_deg"] < LOOP_POSE_DEG and lc["seam_ok"]
            L.append("**Loop closure %s:** first vs last pose max %.3f deg (%s), mean %.3f deg; velocity diff max %.2f deg/s (%s), mean %.2f; worst excess over %.1fx its largest change in the %d frames either side %.2f deg/s (%s); root position diff %.2f cm.\n" % (
                mark(ok), lc["pose_max_deg"], lc["pose_worst_joint"], lc["pose_mean_deg"],
                lc["vel_max_deg_s"], lc["vel_worst_joint"], lc["vel_mean_deg_s"], LOOP_SEAM_RATIO, LOOP_SEAM_WINDOW,
                lc["seam_worst_excess_deg_s"], lc["seam_worst_joint"], lc["root_pos_diff_cm"]))
        b = r.get("boundary")
        if b:
            def fmt(d):
                s = "%.1f deg (%s)" % (d["max_deg"], d["worst_joint"])
                if "max_translation_cm" in d:
                    s += ", translation %.1f cm (%s)" % (d["max_translation_cm"], d["worst_translation_joint"])
                return s
            note = lambda v: "NOTE " if v > BOUNDARY_NOTE_DEG else ""
            L.append("**Boundary vs `%s`:**" % b["idle"])
            L.append("- frame 0 vs idle frame 0: %s%s" % (note(b["first_vs_idle0"]["max_deg"]), fmt(b["first_vs_idle0"])))
            L.append("- last frame vs idle frame 0: %s%s" % (note(b["last_vs_idle0"]["max_deg"]), fmt(b["last_vs_idle0"])))
            a = b["first_vs_any_idle_frame"]
            L.append("- frame 0 vs any idle frame: worst %s%.1f deg at idle frame %d (%s), best %.1f deg at idle frame %d"
                     % (note(a["worst_max_deg"]), a["worst_max_deg"], a["worst_idle_frame"], a["worst"]["worst_joint"],
                        a["best_max_deg"], a["best_idle_frame"]))
            a = b["last_vs_any_idle_frame"]
            L.append("- last frame vs any idle frame: worst %.1f deg at idle frame %d, best %.1f deg at idle frame %d\n"
                     % (a["worst_max_deg"], a["worst_idle_frame"], a["best_max_deg"], a["best_idle_frame"]))
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")


# ---------------------------------------------------------------------- main
def main(argv=None):
    ap = argparse.ArgumentParser(description="Animation curve report for skinned .glb meshes.")
    ap.add_argument("glb")
    ap.add_argument("out_dir")
    ap.add_argument("--clips", help="clip table json: {clips:[{id, impact_s, loop, length_s}]}")
    ap.add_argument("--idle", default="idle", help="name of the idle clip used for boundary checks")
    ap.add_argument("--up", default="y", choices=["y", "z"], help="up axis of the world (glTF is y)")
    ap.add_argument("--scale", type=float, default=1.0, help="multiply positions by this to get metres")
    args = ap.parse_args(argv)
    up = {"y": 1, "z": 2}[args.up]

    js, binc = load_glb(args.glb)
    rig = build_rig(js)
    joints, skel = pick_joints(js, rig)
    if not joints:
        print("no skin joints and no animated nodes found", file=sys.stderr)
        return 2
    jset = set(joints)
    jn = [rig.names[j] for j in joints]

    table = {}
    if args.clips:
        with open(args.clips, "r", encoding="utf-8") as f:
            tj = json.load(f)
        for c in tj.get("clips", []):
            table[c["id"].lower()] = c

    major = np.array([not (FINGER_RE.search(n) or HELPER_RE.search(n)) for n in jn])
    jparent = {}
    for j in joints:
        p = rig.parent[j]
        while p >= 0 and p not in jset:
            p = rig.parent[p]
        jparent[j] = p
    roots = [j for j in joints if jparent[j] < 0]
    desc = {j: 0 for j in joints}
    for j in joints:
        p = jparent[j]
        while p >= 0:
            desc[p] += 1
            p = jparent[p]
    root_i = skel if (skel is not None and skel in jset) else max(roots, key=lambda j: desc[j])
    pel = [j for j in joints if re.search(r"pelvis|^hips?$|hips?$", rig.names[j], re.I)]
    pelvis_i = pel[0] if pel else root_i
    haskids = {jparent[j] for j in joints if jparent[j] >= 0}
    rest_clip = dict(nf=1, T=rig.rest_t[:, None, :], R=rig.rest_r[:, None, :], S=rig.rest_s[:, None, :])
    Wrest = forward_kinematics(rig, rest_clip)[:, 0, :] * args.scale
    rest_height = float(Wrest[joints][:, up].max() - Wrest[joints][:, up].min())
    foot = [j for j in joints if FOOT_RE.search(rig.names[j]) and not FOOT_EXCL_RE.search(rig.names[j])]
    if not foot:
        leaves = [j for j in joints if j not in haskids]
        foot = sorted(leaves, key=lambda j: Wrest[j][up])[:2]

    loaded, used = [], {}
    for ai in range(len(js.get("animations", []))):
        c = load_animation(js, binc, rig, ai)
        if c is None:
            continue
        base = c["name"]
        used[base] = used.get(base, 0) + 1
        if used[base] > 1:
            c["name"] = "%s_%d" % (base, used[base])
        loaded.append(c)
    if not loaded:
        print("no animations found", file=sys.stderr)
        return 2
    names = [c["name"] for c in loaded]
    idle = None
    for cand in (lambda n: n == args.idle, lambda n: n.lower() == args.idle.lower(),
                 lambda n: n.lower().startswith(args.idle.lower())):
        m = [n for n in names if cand(n)]
        if m:
            idle = m[0]
            break

    os.makedirs(args.out_dir, exist_ok=True)
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception:
        plt = None

    results, clips_q, clips_t = [], {}, {}
    for c in loaded:
        P = forward_kinematics(rig, c) * args.scale
        row = table.get(c["name"].lower())
        is_loop = bool(row.get("loop", False)) if row is not None else c["name"].lower().startswith("idle")
        impact = float(row.get("impact_s", 0) or 0) if row else 0.0
        res, series = analyse_clip(rig, joints, jn, c, P, is_loop, impact, root_i, pelvis_i, up, foot, major)
        if row is not None and "length_s" in row:
            res["table_length_s"] = float(row["length_s"])
        clips_q[c["name"]] = c["R"][joints]
        clips_t[c["name"]] = c["T"][joints]
        res["_series"] = series
        results.append(res)

    for r in results:
        if idle and not r["loop"] and r["name"] != idle:
            r["boundary"] = boundary(clips_q, clips_t, r["name"], idle, jn)

    for r in results:
        series = r.pop("_series")
        fn = safe_name(r["name"])
        write_csv(os.path.join(args.out_dir, fn + ".csv"), r, series, jn, r["fps"])
        if plt is not None:
            plot_clip(os.path.join(args.out_dir, fn + ".png"), r, series, jn, plt)
        r["series"] = dict(root_speed_mps=clean(series["root_speed"], 4), pelvis_speed_mps=clean(series["pelvis_speed"], 4),
                           joint_speed_deg_s={n: clean(series["speed"][k], 2) for k, n in enumerate(jn)},
                           joint_accel_deg_s2={n: clean(series["accel"][k], 1) for k, n in enumerate(jn)},
                           joint_jerk_deg_s3={n: clean(series["jerk"][k], 0) for k, n in enumerate(jn)})

    meta = dict(file=os.path.basename(args.glb), joints=len(joints), major_joints=int(major.sum()),
                root=rig.names[root_i], pelvis=rig.names[pelvis_i], rest_height_m=rest_height,
                foot_joints=[rig.names[f] for f in foot], idle=idle, clip_table=args.clips,
                joint_names=jn, major=[n for n, m in zip(jn, major) if m],
                thresholds=dict(SPIKE_RATIO=SPIKE_RATIO, ACCEL_FLOOR=ACCEL_FLOOR, JERK_FLOOR=JERK_FLOOR,
                                NONZERO_EPS=NONZERO_EPS, JUMP_RATIO=JUMP_RATIO, JUMP_MIN_DEG=JUMP_MIN_DEG,
                                HOLD_SPEED=HOLD_SPEED, HOLD_FRAC=HOLD_FRAC, C1_ABS=C1_ABS, C1_RATIO=C1_RATIO, C1_WINDOW=C1_WINDOW,
                                CORE_ACTIVE_DEG_S=CORE_ACTIVE_DEG_S, CORE_HOLD_FRAC=CORE_HOLD_FRAC,
                                FOOT_BAND_M=FOOT_BAND_M, FOOT_SPEED=FOOT_SPEED, FOOT_MIN_SPAN=FOOT_MIN_SPAN,
                                FOOT_DRIFT_CM=FOOT_DRIFT_CM, LOOP_POSE_DEG=LOOP_POSE_DEG, LOOP_VEL_DEG_S=LOOP_VEL_DEG_S,
                                BOUNDARY_NOTE_DEG=BOUNDARY_NOTE_DEG))
    with open(os.path.join(args.out_dir, "curves.json"), "w", encoding="utf-8") as f:
        json.dump(clean(dict(meta=meta, clips=results)), f, indent=1)
    write_md(os.path.join(args.out_dir, "curves.md"), meta, results)
    print("wrote %s (%d clips%s)" % (args.out_dir, len(results), "" if plt else ", no matplotlib: no PNGs"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
