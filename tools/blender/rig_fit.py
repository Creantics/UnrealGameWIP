"""Fits the canonical skeleton (rig_spec.JOINTS) to joint landmarks. No bpy: fit_canonical.py and
bake_to_canonical.py call it, and it runs under plain python3 + numpy (`python3 rig_fit.py` self-checks).

A landmark is a canonical joint name -> its head position (metres, Blender axes, character facing -Y).
Every joint is placed by one similarity transform (rotation + uniform scale + translation) taken from an
anchor segment A -> B of landmarked joints:
  A = the joint itself if it has a landmark, else its nearest landmarked ancestor;
  B = the first landmarked joint in the joint's own subtree (breadth first, canonical child order, one on
      the joint's own side preferred), else the first one in A's subtree, else A's nearest landmarked
      ancestor (the segment is then B -> A).
The similarity maps the template's A -> B onto the landmarks' A -> B with the smallest rotation, so a joint
keeps its place relative to the segment it belongs to (twist bones stay at 1/3 and 2/3 of the arm, finger
chains follow the forearm, the face follows neck -> head). The joint's tail and its roll hint (x_axis) go
through the same transform, so hinge axes (§4.2) are carried, not re-guessed. `root` goes to the origin on
the floor (R16): the mesh is expected to stand over the origin.

The 14 landmarks a person clicks (or AccuRIG / UniRig give us) are REQUIRED below; more are welcome (head,
ball, finger tips via the next joint, eyes): each extra landmark pins its joint exactly.
"""
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig_spec as rs  # noqa: E402

REQUIRED = ["pelvis", "neck_01", "upperarm_l", "upperarm_r", "lowerarm_l", "lowerarm_r", "hand_l", "hand_r",
            "thigh_l", "thigh_r", "calf_l", "calf_r", "foot_l", "foot_r"]


def _rows():
    return {r[0]: r for r in rs.JOINTS}


def _children():
    ch = {}
    for name, parent, *_ in rs.JOINTS:
        ch.setdefault(parent, []).append(name)
    return ch


def min_rotation(u, v):
    """3x3 rotation taking direction u onto direction v by the smallest angle (Rodrigues)."""
    u = np.asarray(u, float) / np.linalg.norm(u)
    v = np.asarray(v, float) / np.linalg.norm(v)
    c = float(np.dot(u, v))
    w = np.cross(u, v)
    s = float(np.linalg.norm(w))
    if s < 1e-12:
        if c > 0:
            return np.eye(3)
        a = np.cross(u, [1.0, 0.0, 0.0])  # 180 degrees about any axis perpendicular to u
        if np.linalg.norm(a) < 1e-6:
            a = np.cross(u, [0.0, 1.0, 0.0])
        a /= np.linalg.norm(a)
        return 2.0 * np.outer(a, a) - np.eye(3)
    k = w / s
    K = np.array([[0, -k[2], k[1]], [k[2], 0, -k[0]], [-k[1], k[0], 0]])
    ang = math.atan2(s, c)
    return np.eye(3) + math.sin(ang) * K + (1 - math.cos(ang)) * (K @ K)


def symmetrize(landmarks):
    """Averages each _l / _r landmark pair with its mirror (x -> -x) and puts centre joints on x = 0."""
    out = {}
    for n, p in landmarks.items():
        p = np.asarray(p, float)
        m = rs.mirror_name(n)
        if m != n and m in landmarks:
            q = np.asarray(landmarks[m], float) * [-1.0, 1.0, 1.0]
            out[n] = 0.5 * (p + q)
        elif m == n:
            out[n] = np.array([0.0, p[1], p[2]])
        else:
            out[n] = p
    return out


def fit_joints(landmarks, floor_z=0.0, strict=True):
    """-> (rows, info). rows: rig_spec.JOINTS-shaped tuples (name, parent, head, tail, x_axis, group, deform)
    fitted to `landmarks`; info: {joint: anchor segment used} and the landmark residual (0 by construction)."""
    L = {n: np.asarray(p, float) for n, p in landmarks.items() if n in rs.JOINT_NAMES}
    missing = [n for n in REQUIRED if n not in L]
    if strict and missing:
        raise ValueError("missing required landmarks: %s" % missing)
    rows = _rows()
    ch = _children()
    T0 = {n: np.asarray(rows[n][2], float) for n in rows}  # template heads

    def ancestor_with_landmark(n):
        p = rows[n][1]
        while p is not None and p not in L:
            p = rows[p][1]
        return p

    def side(n):
        return "l" if n.endswith("_l") else "r" if n.endswith("_r") else "c"

    def first_landmark_below(n, want):
        """Breadth first; a landmark on the same side as `want` (centre for centre joints) wins, so the
        pelvis aims at the neck, not a hip, and left / right helpers stay mirror images."""
        queue, found = list(ch.get(n, [])), []
        while queue:
            c = queue.pop(0)
            if c in L:
                found.append(c)
            queue += ch.get(c, [])
        same = [c for c in found if side(c) == side(want)]
        return (same or found or [None])[0]

    def similarity(a, b):
        u, v = T0[b] - T0[a], L[b] - L[a]
        s = np.linalg.norm(v) / max(np.linalg.norm(u), 1e-9)
        return min_rotation(u, v), s

    out, info = [], {}
    for name, parent, head, tail, xa, group, deform in rs.JOINTS:
        if name == "root":
            continue
        a = name if name in L else ancestor_with_landmark(name)
        if a is None:
            raise ValueError("joint %s has no landmarked ancestor" % name)
        b = first_landmark_below(name, name) or first_landmark_below(a, name)
        if b is not None:
            R, s = similarity(a, b)
            info[name] = (a, b)
        else:
            up = ancestor_with_landmark(a)
            if up is None:
                R, s = np.eye(3), 1.0
                info[name] = (a, None)
            else:
                R, s = similarity(up, a)
                info[name] = (up, a)
        h0, t0 = np.asarray(head, float), np.asarray(tail, float)
        h = L[name] if name in L else L[a] + s * (R @ (h0 - T0[a]))
        t = h + s * (R @ (t0 - h0))
        x = R @ np.asarray(xa, float)
        out.append((name, parent, h, t, x, group, deform))
    # root: on the floor under the pelvis, pointing forward like the template
    pel = next(r for r in out if r[0] == "pelvis")[2]
    r = rows["root"]
    rh = np.array([0.0, 0.0, floor_z])  # R16: the root sits at the origin; the character stands over it
    out.insert(0, ("root", None, rh, rh + (np.asarray(r[3]) - np.asarray(r[2])), np.asarray(r[4], float), r[5], r[6]))
    order = {n: i for i, n in enumerate(rs.JOINT_NAMES)}
    out.sort(key=lambda r: order[r[0]])
    resid = max((float(np.linalg.norm(next(r for r in out if r[0] == n)[2] - p)) for n, p in L.items()), default=0.0)
    return [tuple([r[0], r[1], tuple(map(float, r[2])), tuple(map(float, r[3])), tuple(map(float, r[4])), r[5], r[6]])
            for r in out], {"anchors": info, "landmark_residual_m": resid}


def template_landmarks(names=None):
    rows = _rows()
    return {n: np.asarray(rows[n][2], float) for n in (names or REQUIRED)}


def _selfcheck():
    # identity: the template's own landmarks give back the template
    rows, _ = fit_joints(template_landmarks())
    err = max(float(np.linalg.norm(np.subtract(a[2], b[2])) + np.linalg.norm(np.subtract(a[3], b[3])))
              for a, b in zip(rows, rs.JOINTS))
    assert err < 1e-9, err
    # a 0.9 scale is reproduced exactly on every joint (landmarks off the floor scale about the origin)
    L = {n: 0.9 * p for n, p in template_landmarks().items()}
    rows, _ = fit_joints(L)
    err = max(float(np.linalg.norm(np.subtract(a[2], 0.9 * np.asarray(b[2])))) for a, b in zip(rows[1:], rs.JOINTS[1:]))
    assert err < 1e-9, err
    print("rig_fit self-check OK (identity, uniform scale)")


if __name__ == "__main__":
    _selfcheck()
