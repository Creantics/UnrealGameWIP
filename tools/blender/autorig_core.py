"""Auto-rig core for single-mesh A-pose humanoids (Tripo bosses, enemies, NPCs). Plain numpy, no bpy:
ft_autorig.py drives it inside headless Blender, and `python -I autorig_core.py` runs the self-check on a
synthetic figure (it is a ctest, see Core/CMakeLists.txt).

Frame: metres, Z up, the figure faces -Y, left = +X. `normalize` finds the floor, the symmetry plane and the
facing and returns the shift (and a 180 degree turn when the figure faces +Y) that puts the mesh in this frame.

Pipeline (each step a function below):
1. voxelize: surface samples -> closed shell -> outside flood fill -> a solid occupancy grid. Overlapping garment
   sheets fuse into one solid; the gaps between legs and between arm and torso stay outside.
2. find_landmarks: horizontal slices of the solid (connected components) give the legs, crotch, armpits, arm
   and leg centrelines, neck narrowing and head top; slices across each limb's own axis give wrist and ankle;
   the hand is split into five fingers by the angle of its distal vertices around the wrist. Each landmark
   carries a source ("detected", "ratio", "template") so failures are visible. Guide points from a json
   override any landmark.
3. build_skeleton: the Unreal mannequin hierarchy and names (SKELETON), twist bones and finger metacarpals
   included, with the mannequin's axis convention (bone X along the bone, its sign per side and limb; see AXES).
4. bind: voxel geodesic distance from every deform bone through the solid, weight = exp(-k (d - d_min) / d_min)
   (blend width scales with the local limb radius), loose cloth below the crotch blended toward the pelvis
   (hem rule), Laplacian smoothing on the position-welded surface, twist bands, top-K influences, normalise.

Where the mesh shows nothing (straight elbow, covered knee, hidden finger joints) the joints come from
segment proportions: the elbow's and ankle's from published anthropometry (Drillis and Contini), the rest
(spine fractions, finger joint spacing, hip offset) are our own estimates, set on the self-check figure and on
test meshes. The weight fall-offs (SHARP_HEAD) and the joint offsets were tuned by eye and by deformation
tests on clothed meshes, so they are not anatomical constants.
"""
import math
import sys

import numpy as np

# ------------------------------------------------------------------------------------------------
# Skeleton: Unreal mannequin names and hierarchy. (name, parent, deform)
# ------------------------------------------------------------------------------------------------
FINGERS = ("index", "middle", "ring", "pinky")


def _side_bones(s):
    b = [("clavicle_" + s, "spine_05", True), ("upperarm_" + s, "clavicle_" + s, True),
         ("lowerarm_" + s, "upperarm_" + s, True), ("hand_" + s, "lowerarm_" + s, True)]
    for f in FINGERS:
        b.append(("%s_metacarpal_%s" % (f, s), "hand_" + s, True))
        b.append(("%s_01_%s" % (f, s), "%s_metacarpal_%s" % (f, s), True))
        b.append(("%s_02_%s" % (f, s), "%s_01_%s" % (f, s), True))
        b.append(("%s_03_%s" % (f, s), "%s_02_%s" % (f, s), True))
    b += [("thumb_01_" + s, "hand_" + s, True), ("thumb_02_" + s, "thumb_01_" + s, True),
          ("thumb_03_" + s, "thumb_02_" + s, True)]
    b += [("upperarm_twist_01_" + s, "upperarm_" + s, True), ("upperarm_twist_02_" + s, "upperarm_" + s, True),
          ("lowerarm_twist_01_" + s, "lowerarm_" + s, True), ("lowerarm_twist_02_" + s, "lowerarm_" + s, True)]
    b += [("thigh_" + s, "pelvis", True), ("calf_" + s, "thigh_" + s, True), ("foot_" + s, "calf_" + s, True),
          ("ball_" + s, "foot_" + s, True),
          ("thigh_twist_01_" + s, "thigh_" + s, True), ("thigh_twist_02_" + s, "thigh_" + s, True),
          ("calf_twist_01_" + s, "calf_" + s, True), ("calf_twist_02_" + s, "calf_" + s, True)]
    return b


SKELETON = ([("root", None, False), ("pelvis", "root", True), ("spine_01", "pelvis", True),
             ("spine_02", "spine_01", True), ("spine_03", "spine_02", True), ("spine_04", "spine_03", True),
             ("spine_05", "spine_04", True), ("neck_01", "spine_05", True), ("neck_02", "neck_01", True),
             ("head", "neck_02", True)]
            + _side_bones("l") + _side_bones("r")
            + [("ik_foot_root", "root", False), ("ik_foot_l", "ik_foot_root", False),
               ("ik_foot_r", "ik_foot_root", False), ("ik_hand_root", "root", False),
               ("ik_hand_gun", "ik_hand_root", False), ("ik_hand_r", "ik_hand_gun", False),
               ("ik_hand_l", "ik_hand_gun", False)])
NAMES = [b[0] for b in SKELETON]
PARENT = {b[0]: b[1] for b in SKELETON}
DEFORM = {b[0]: b[2] for b in SKELETON}

# Twist bones: (segment, next joint, [(twist bone, position along the segment 0..1)]). The segment bone keeps the
# weight at its own joint; the band tents hand it over to the twist bones along the segment.
TWISTS = [("upperarm", "lowerarm", [("upperarm_twist_01", 0.30), ("upperarm_twist_02", 0.62)]),
          ("lowerarm", "hand", [("lowerarm_twist_02", 0.45), ("lowerarm_twist_01", 0.80)]),
          ("thigh", "calf", [("thigh_twist_01", 0.30), ("thigh_twist_02", 0.62)]),
          ("calf", "foot", [("calf_twist_02", 0.45), ("calf_twist_01", 0.80)])]
TWIST_NAMES = {t + "_" + s for _a, _b, ts in TWISTS for t, _p in ts for s in "lr"}
# Keys a guides json may set: the landmark joints (not root, ik or twist bones, which are built from them), the
# crotch (its height sets the hem rule) and the toe tips (they aim the ball bones).
GUIDE_KEYS = ({n for n in NAMES if n != "root" and not n.startswith("ik_")} - TWIST_NAMES) | {"crotch", "toe_l", "toe_r"}
FINGER_GUIDE = ("index", "middle", "ring", "pinky", "thumb")


def check_guides(guides):
    """A guides mapping {key: [x, y, z]} -> {key: array}; ValueError naming every unknown key or bad value."""
    if not isinstance(guides, dict):
        raise ValueError("guides must be a json object {joint: [x, y, z]}")
    bad = sorted(k for k in guides if k not in GUIDE_KEYS)
    if bad:
        raise ValueError("unknown guide keys: %s (allowed: landmark joints such as hand_l, plus crotch, toe_l, toe_r)"
                         % ", ".join(bad))
    out = {}
    for k, v in guides.items():
        try:
            a = np.asarray(v, float)
        except (TypeError, ValueError):
            a = None
        if a is None or a.shape != (3,) or not np.isfinite(a).all():
            raise ValueError("guide %s must be three finite numbers [x, y, z], got %r" % (k, v))
        out[k] = a
    return out


def mirror_name(n):
    return n[:-2] + "_r" if n.endswith("_l") else n[:-2] + "_l" if n.endswith("_r") else n


def side_of(n):
    return "l" if n.endswith("_l") else "r" if n.endswith("_r") else "c"


def chain_of(n):
    """'arm', 'hand', 'leg', 'spine' or 'other' for the axis convention and binding sharpness."""
    if any(n.startswith(f) for f in FINGERS + ("thumb",)) or n.startswith("hand_"):
        return "hand"
    if n.startswith(("clavicle", "upperarm", "lowerarm")):
        return "arm"
    if n.startswith(("thigh", "calf", "foot", "ball")):
        return "leg"
    if n.startswith(("pelvis", "spine", "neck", "head")):
        return "spine"
    return "other"


def axis_sign(n):
    """The mannequin's bone X: along the bone (toward the child) on the spine, the left arm and hand, and the
    right leg; against it (toward the parent) on the right arm and hand and the left leg."""
    c, s = chain_of(n), side_of(n)
    if c in ("arm", "hand"):
        return 1.0 if s == "l" else -1.0
    if c == "leg":
        if n.startswith("ball"):
            return 1.0 if s == "l" else -1.0
        return -1.0 if s == "l" else 1.0
    return 1.0


def y_hint(n):
    """The mannequin's bone Y before orthogonalising: backward (+Y) on the spine, the left arm and the left leg,
    forward on the right ones; the hand and finger bones take the palm side (forward on the left), the thumb the
    back of the hand, the ball bone up (down on the right)."""
    c, s = chain_of(n), side_of(n)
    v = np.array([0.0, 1.0, 0.0])
    if n.startswith("ball"):
        v = np.array([0.0, 0.0, 1.0])
    elif c == "hand" and not n.startswith("thumb"):
        v = -v
    if s == "r":
        v = -v
    return v


# ------------------------------------------------------------------------------------------------
# Small helpers
# ------------------------------------------------------------------------------------------------
def unit(v):
    v = np.asarray(v, float)
    n = np.linalg.norm(v)
    return v / n if n > 1e-12 else v


def smoothstep(x):
    x = np.clip(x, 0.0, 1.0)
    return x * x * (3.0 - 2.0 * x)


def _shift(a, d, axis, fill):
    """a shifted by d cells along axis (result[i] = a[i - d]); cells shifted in get `fill`."""
    out = np.empty_like(a)
    sl_dst = [slice(None)] * a.ndim
    sl_src = [slice(None)] * a.ndim
    sl_fill = [slice(None)] * a.ndim
    if d > 0:
        sl_dst[axis], sl_src[axis], sl_fill[axis] = slice(d, None), slice(None, -d), slice(None, d)
    else:
        sl_dst[axis], sl_src[axis], sl_fill[axis] = slice(None, d), slice(-d, None), slice(d, None)
    out[tuple(sl_dst)] = a[tuple(sl_src)]
    out[tuple(sl_fill)] = fill
    return out


def dilate(m, steps=1):
    for _ in range(steps):
        o = m.copy()
        for ax in range(m.ndim):
            o |= _shift(m, 1, ax, False) | _shift(m, -1, ax, False)
        m = o
    return m


def label2d(mask):
    """4-connected components of a 2D bool array -> int labels (0 = background), by min-propagation."""
    h, w = mask.shape
    big = h * w + 1
    lab = np.where(mask, np.arange(1, h * w + 1).reshape(h, w), big)
    while True:
        m = lab.copy()
        for ax in (0, 1):
            for d in (1, -1):
                m = np.minimum(m, _shift(lab, d, ax, big))
        m = np.where(mask, m, big)
        if np.array_equal(m, lab):
            break
        lab = m
    lab = np.where(mask, lab, 0)
    u = np.unique(lab[lab > 0])
    remap = np.zeros(big + 1, int)
    remap[u] = np.arange(1, len(u) + 1)
    return remap[lab]


def weld(V, eps):
    """Index of a representative vertex for each vertex (positions within eps share one), for adjacency."""
    key = np.round(V / eps).astype(np.int64)
    _u, inv = np.unique(key, axis=0, return_inverse=True)
    return inv.reshape(-1)


# ------------------------------------------------------------------------------------------------
# 1. Frame and voxels
# ------------------------------------------------------------------------------------------------
def normalize(V):
    """-> (offset, turn): add offset to every vertex (after a 180 degree turn about Z when turn is True) to put
    the soles on z = 0, the symmetry plane on x = 0 and the toes toward -Y."""
    V = np.asarray(V, float)
    H = V[:, 2].max() - V[:, 2].min()
    low = V[V[:, 2] < V[:, 2].min() + 0.04 * H]
    # Facing: the toes reach further from the ankle column than the heels do.
    shin = (V[:, 2] > V[:, 2].min() + 0.06 * H) & (V[:, 2] < V[:, 2].min() + 0.3 * H)
    yc = np.median(V[shin, 1]) if shin.any() else np.median(V[:, 1])
    turn = bool(low[:, 1].max() - yc > yc - low[:, 1].min())
    W = V * np.array([-1.0, -1.0, 1.0]) if turn else V
    # Symmetry plane: the x that best mirrors a subsample onto itself (coarse search on a 2D occupancy).
    xs = W[:, 0]
    best, bx = 1e18, 0.5 * (xs.min() + xs.max())
    cell = H / 200.0
    sub = W[:: max(1, len(W) // 20000)]
    for c in np.linspace(bx - 0.03 * H, bx + 0.03 * H, 61):
        a = {tuple(r) for r in np.round(np.c_[sub[:, 0] - c, sub[:, 2]] / cell).astype(int)}
        b = {tuple(r) for r in np.round(np.c_[c - sub[:, 0], sub[:, 2]] / cell).astype(int)}
        miss = len(a ^ b)
        if miss < best:
            best, bx = miss, c
    off = np.array([-bx, -np.median(W[:, 1]), -W[:, 2].min()])
    return off, turn


class Grid:
    """A solid occupancy grid: solid[i, j, k] for the cell at lo + (i, j, k) * h."""

    def __init__(self, V, F, h):
        V = np.asarray(V, float)
        F = np.asarray(F, int)
        self.h = h
        self.lo = V.min(0) - 3 * h
        self.shape = tuple(np.ceil((V.max(0) - self.lo) / h).astype(int) + 4)
        surf = np.zeros(self.shape, bool)
        A, B, C = V[F[:, 0]], V[F[:, 1]], V[F[:, 2]]
        edge = np.maximum.reduce([np.linalg.norm(B - A, axis=1), np.linalg.norm(C - B, axis=1),
                                  np.linalg.norm(A - C, axis=1)])
        ks = np.maximum(1, np.ceil(edge / (0.5 * h)).astype(int))
        for k in np.unique(ks):
            sel = ks == k
            ij = np.array([(i, j) for i in range(k + 1) for j in range(k + 1 - i)], float) / k
            a, b = ij[:, 0][None, :, None], ij[:, 1][None, :, None]
            P = A[sel][:, None] * (1 - a - b) + B[sel][:, None] * a + C[sel][:, None] * b
            idx = np.floor((P.reshape(-1, 3) - self.lo) / h).astype(int)
            surf[idx[:, 0], idx[:, 1], idx[:, 2]] = True
        surf = dilate(surf, 1)  # closes cracks up to a cell
        out = np.zeros(self.shape, bool)
        out[0], out[-1], out[:, 0], out[:, -1], out[:, :, 0], out[:, :, -1] = True, True, True, True, True, True
        out &= ~surf
        free = ~surf
        while True:  # flood the outside through the empty cells, one masked step at a time
            n = out
            for _ in range(8):
                n = dilate(n, 1) & free
            if n.sum() == out.sum():
                break
            out = n
        self.solid = ~out
        self.surface = surf

    def index(self, P):
        return np.clip(np.floor((np.asarray(P) - self.lo) / self.h).astype(int), 0, np.array(self.shape) - 1)

    def center(self, idx):
        return self.lo + (np.asarray(idx) + 0.5) * self.h

    def x(self, i):
        return self.lo[0] + (i + 0.5) * self.h

    def k_of(self, z):
        return int(np.clip(np.floor((z - self.lo[2]) / self.h), 0, self.shape[2] - 1))


# ------------------------------------------------------------------------------------------------
# 2. Landmarks
# ------------------------------------------------------------------------------------------------
def _slice_parts(g, k):
    """Components of horizontal slice k -> list of dicts (n, cx, cy, x0, x1, y0, y1, cells)."""
    lab = label2d(g.solid[:, :, k])
    parts = []
    for c in range(1, lab.max() + 1):
        ii, jj = np.nonzero(lab == c)
        xs, ys = g.lo[0] + (ii + 0.5) * g.h, g.lo[1] + (jj + 0.5) * g.h
        parts.append(dict(n=len(ii), cx=xs.mean(), cy=ys.mean(), x0=xs.min(), x1=xs.max(), y0=ys.min(), y1=ys.max(),
                          ii=ii, jj=jj))
    return parts


def arm_vertices(g, V, s, edge, armpit, seed_parts, seed_z):
    """Vertices of the arm: the solid lateral of the torso's edge that is connected to the free arm seen in the
    slice just under the armpit (a hand that hangs beside a coat stays apart from the coat)."""
    sg = 1 if s == "l" else -1
    xs = g.lo[0] + (np.arange(g.shape[0]) + 0.5) * g.h
    allow = g.solid & ((xs * sg > edge + 0.5 * g.h)[:, None, None])
    reg = np.zeros_like(allow)
    k = g.k_of(seed_z)
    p = max(seed_parts, key=lambda q: q["n"])
    reg[p["ii"], p["jj"], k] = True
    reg &= allow
    while True:
        n = reg
        for _ in range(6):
            n = dilate(n, 1) & allow
        if n.sum() == reg.sum():
            break
        reg = n
    reg = dilate(reg, 1)
    vi = g.index(V)
    return reg[vi[:, 0], vi[:, 1], vi[:, 2]] & (V[:, 0] * sg > edge)


def _line_fit(P):
    c = P.mean(0)
    _u, _s, vt = np.linalg.svd(P - c)
    return c, unit(vt[0])


def find_landmarks(V, F, voxels=160, guides=None, log=None):
    """V in the normalised frame. -> (L, info): L joint landmark name -> position; info: per-landmark source,
    the grid and measurements other steps reuse (crotch, leg axes, limb radii, hand frames)."""
    V = np.asarray(V, float)
    say = log or (lambda *_a: None)
    H = V[:, 2].max()
    g = Grid(V, F, H / voxels)
    h = g.h
    src, L = {}, {}
    info = {"H": H, "grid": g, "src": src}

    def put(n, p, how):
        L[n] = np.asarray(p, float)
        src[n] = how

    slices = {}

    def parts(z):
        k = g.k_of(z)
        if k not in slices:
            slices[k] = _slice_parts(g, k)
        return slices[k]

    zs = np.arange(0.5 * h, H, h)
    # --- legs: components wholly on one side, near the centre (hands hang further out) -----------------
    def leg_parts(z, s):
        sg = 1 if s == "l" else -1
        return [p for p in parts(z) if (p["x0"] * sg > -0.25 * h if sg > 0 else p["x1"] < 0.25 * h)
                and abs(p["cx"]) < 0.17 * H and p["n"] * h * h > 0.0008 * H * H]

    # crotch: coming down from the waist, the last slice where the centre column (x = 0) is still solid through
    # nearly all of the body's depth; below it the legs part (an open jacket or coat hem leaves that gap outside)
    crotch = None
    i0 = int(np.floor((0.0 - g.lo[0]) / h))
    xs_c = g.lo[0] + (np.arange(g.shape[0]) + 0.5) * h
    for z in zs[(zs > 0.30 * H) & (zs < 0.62 * H)][::-1]:
        k = g.k_of(z)
        col = g.solid[max(0, i0 - 1):i0 + 2, :, k].any(0)
        jj = np.nonzero(g.solid[np.abs(xs_c) < 0.15 * H, :, k].any(0))[0]
        if len(jj) == 0:
            continue
        if col.sum() < 0.8 * (jj.max() - jj.min() + 1):  # a notch at x = 0 even where trouser legs touch
            crotch = z + h
            break
    if crotch is None:
        crotch = 0.47 * H
        src["crotch"] = "ratio"
        say("crotch not found in the slices: ratio 0.47 H")
    else:
        src["crotch"] = "detected"
    info["crotch"] = crotch
    # leg centrelines per side: centroids of the leg component from 0.12 H to just under the crotch
    leg = {}
    for s in "lr":
        pts = []
        for z in zs[(zs > 0.12 * H) & (zs < crotch - 0.04 * H)]:
            lp = leg_parts(z, s)
            if lp:
                p = max(lp, key=lambda q: q["n"])
                pts.append((p["cx"], p["cy"], z, math.sqrt(p["n"] / math.pi) * h))
        pts = np.array(pts) if pts else np.zeros((0, 4))
        if len(pts) >= 5:
            c, d = _line_fit(pts[:, :3])
            if d[2] < 0:
                d = -d
            r_leg = float(np.median(pts[:, 3]))
        else:
            sg = 1 if s == "l" else -1
            c, d, r_leg = np.array([sg * 0.06 * H, 0.0, 0.3 * H]), np.array([0, 0, 1.0]), 0.055 * H
            say("leg %s centreline not found: template" % s)
        leg[s] = dict(c=c, d=d, r=r_leg, pts=pts)

    def on_leg(s, z):
        c, d = leg[s]["c"], leg[s]["d"]
        return c + d * (z - c[2]) / d[2]
    info["leg"] = leg
    # ankle: the foot is long in Y; going up, the leg's depth falls toward its width
    for s in "lr":
        zt = None
        for z in zs[zs < 0.14 * H]:
            lp = leg_parts(z, s)
            if not lp:
                continue
            p = max(lp, key=lambda q: q["n"])
            if (p["y1"] - p["y0"]) < 1.35 * (p["x1"] - p["x0"]) and z > 0.02 * H:
                zt = z
                break
        how = "detected"
        if zt is None:
            zt, how = 0.045 * H, "ratio"
        # the ankle joint: the textbook ankle height (0.039 H) plus a shoe sole, kept under the shoe's collar
        # (where the foot's length gives way to the leg's round section) when that is seen lower
        ankle_z = min(0.055 * H, zt - 0.005 * H) if how == "detected" else 0.055 * H
        foot = [p for z in zs[zs < max(zt, 0.03 * H)] for p in leg_parts(z, s)]
        put("foot_" + s, on_leg(s, ankle_z), "ratio")
        if foot:
            y_toe = min(p["y0"] for p in foot)
            y_heel = max(p["y1"] for p in foot)
            x_mid = float(np.mean([p["cx"] for p in foot]))
            put("ball_" + s, (x_mid, y_heel + 0.73 * (y_toe - y_heel), 0.35 * ankle_z), "detected")
            info["toe_" + s] = np.array([x_mid, y_toe, 0.35 * ankle_z])
        else:
            a = L["foot_" + s]
            put("ball_" + s, (a[0], a[1] - 0.08 * H, 0.35 * ankle_z), "ratio")
            info["toe_" + s] = L["ball_" + s] + np.array([0, -0.03 * H, 0])
        # hip joint centre: above the crotch, on the leg's centreline
        hip_z = crotch + 0.045 * H
        put("thigh_" + s, on_leg(s, hip_z), "detected" if src["crotch"] == "detected" else "ratio")
        # knee: half way from ankle to hip (the femur and tibia are near equal); the centroid there, when seen
        kz = ankle_z + 0.5 * (hip_z - ankle_z)
        lp = leg_parts(kz, s)
        kp = on_leg(s, kz)
        if lp:
            p = max(lp, key=lambda q: q["n"])
            if np.hypot(p["cx"] - kp[0], p["cy"] - kp[1]) < 2 * leg[s]["r"]:
                kp = np.array([p["cx"], p["cy"], kz])
        put("calf_" + s, kp, "ratio")
    # --- torso and arms -------------------------------------------------------------------------------
    def centre_part(z):
        ps = parts(z)
        if not ps:
            return None
        return min(ps, key=lambda p: (0 if p["x0"] <= 0 <= p["x1"] else 1, abs(p["cx"])))

    def arm_parts(z, s):
        c = centre_part(z)
        sg = 1 if s == "l" else -1
        return [p for p in parts(z) if p is not c and p["cx"] * sg > 0 and
                (p["x0"] > 0 if sg > 0 else p["x1"] < 0) and not (z < crotch and abs(p["cx"]) < 0.17 * H)]

    arm = {}
    for s in "lr":
        sg = 1 if s == "l" else -1
        run = [z for z in zs[(zs > 0.35 * H) & (zs < 0.92 * H)] if arm_parts(z, s)]
        if not run:
            raise ValueError("no free arm on side %s: the mesh is not in an A-pose or the arm touches the body" % s)
        # armpit: the top of the lowest run of slices that shows a free arm, extended upward
        run = np.array(run)
        top = run[0]
        for z in run[1:]:
            if z - top > 1.5 * h:
                break
            top = z
        armpit = top + 0.5 * h
        pts = np.array([[p["cx"], p["cy"], z] for z in run if z <= top
                        for p in [max(arm_parts(z, s), key=lambda q: q["n"])]])
        # arm vertices: lateral of the torso's edge below the armpit
        cp = centre_part(armpit - 2 * h)
        edge = (cp["x1"] if sg > 0 else -cp["x0"]) if cp else 0.12 * H
        mask = arm_vertices(g, V, s, edge, armpit, arm_parts(armpit - 2 * h, s), armpit - 2 * h)
        AV = V[mask]
        c0, d = _line_fit(pts[pts[:, 2] > pts[:, 2].min() + 0.35 * (top - pts[:, 2].min())]) if len(pts) > 8 \
            else _line_fit(pts)
        if d[0] * sg < 0:
            d = -d
        sproj = (AV - c0) @ d
        tip_s = np.percentile(sproj, 99.8)
        tip = AV[sproj > tip_s - 0.004 * H].mean(0)
        # perpendicular radius profile along the arm axis
        rel = AV - c0
        perp = np.linalg.norm(rel - np.outer(rel @ d, d), axis=1)
        bins = np.arange(sproj.min(), tip_s, 0.01 * H)
        prof = []
        for b in bins:
            m = (sproj >= b) & (sproj < b + 0.01 * H)
            prof.append(np.percentile(perp[m], 90) if m.sum() > 5 else np.nan)
        prof = np.array(prof)
        win = (bins > tip_s - 0.15 * H) & (bins < tip_s - 0.075 * H) & ~np.isnan(prof)
        if win.any():
            sm = np.convolve(np.nan_to_num(prof, nan=1e9), np.ones(3) / 3, mode="same")
            wb = bins[win][np.argmin(sm[win])] + 0.005 * H
            how = "detected"
        else:
            wb, how = tip_s - 0.108 * H, "ratio"
        wm = np.abs(sproj - wb) < 0.008 * H
        wrist = AV[wm].mean(0) if wm.sum() > 5 else c0 + d * wb
        put("hand_" + s, wrist, how)
        # upper-arm radius from the free arm just below the armpit
        ua = (sproj > np.percentile(sproj, 2)) & (AV[:, 2] < armpit) & (AV[:, 2] > armpit - 0.08 * H)
        r_ua = float(np.median(perp[ua])) if ua.sum() > 10 else 0.04 * H
        # shoulder joint: the arm's own centreline where it crosses the torso's side wall (the torso's lateral
        # edge just under the armpit), then half an upper-arm radius further in along that line: the humeral head
        # sits inside the deltoid cap, and a pivot on the wall leaves the cap behind when the arm is raised
        t = (sg * edge - c0[0]) / d[0] if abs(d[0]) > 1e-6 else 0.0
        t -= 0.5 * r_ua * np.sign(d[0] * sg)
        put("upperarm_" + s, c0 + d * t, "detected")
        # elbow: the forearm and upper arm in their textbook ratio (0.186 : 0.146 of height) along the
        # shoulder-wrist line, pulled onto the arm's centre there
        sh = L["upperarm_" + s]
        el = sh + 0.56 * (wrist - sh)
        em = np.linalg.norm(AV - el, axis=1) < 1.2 * r_ua
        if em.sum() > 10:
            q = AV[em].mean(0)
            el = el + (q - el) - np.dot(q - el, unit(wrist - sh)) * unit(wrist - sh)
        put("lowerarm_" + s, el, "ratio")
        arm[s] = dict(c=c0, d=d, armpit=armpit, r=r_ua, tip=tip, edge=edge, verts=np.nonzero(mask)[0])
    info["arm"] = arm
    # --- spine, neck, head ------------------------------------------------------------------------------
    widths = []
    for z in zs[zs > min(a["armpit"] for a in arm.values())]:
        c = centre_part(z)
        if c is not None:
            widths.append((z, c["x1"] - c["x0"], c["cx"], c["cy"], c["y0"], c["y1"]))
    widths = np.array(widths)
    up = widths[widths[:, 0] < H - 0.05 * H]
    neck_i = int(np.argmin(up[:, 1]))
    neck_z, neck_w = up[neck_i, 0], up[neck_i, 1]
    base = [r for r in up[:neck_i][::-1] if r[1] > 1.6 * neck_w]
    n1z = base[0][0] if base else neck_z - 0.04 * H
    ny = up[neck_i, 3]
    put("neck_01", (0.0, ny, n1z), "detected" if base else "ratio")
    put("head", (0.0, ny, neck_z + 0.30 * (H - neck_z)), "detected")
    put("neck_02", 0.5 * (L["neck_01"] + L["head"]), "ratio")
    info["head_top"] = np.array([0.0, ny, H])
    pel_z = crotch + 0.06 * H

    def torso_y(z):
        c = centre_part(z)
        return c["cy"] if c is not None else 0.0
    put("pelvis", (0.0, torso_y(pel_z), pel_z), src["crotch"])
    for i, f in enumerate((0.10, 0.25, 0.42, 0.60, 0.80)):
        z = pel_z + f * (n1z - pel_z)
        put("spine_0%d" % (i + 1), (0.0, torso_y(z), z), "ratio")
    for s in "lr":
        sg = 1 if s == "l" else -1
        sh = L["upperarm_" + s]
        # the collar bone runs out to the shoulder about level in depth: its root sits half way between the
        # neck's and the shoulder joint's depth
        put("clavicle_" + s, (sg * 0.2 * abs(sh[0]), 0.5 * (L["neck_01"][1] + sh[1]),
                              sh[2] + 0.35 * (n1z - sh[2])), "ratio")
    # --- guides override (before the fingers, so a guided hand or wrist moves the finger chains) ------------
    guides = check_guides(guides or {})
    fingers_g = {}
    for n, p in guides.items():
        if n == "crotch" or n.startswith("toe_"):
            info[n] = float(p[2]) if n == "crotch" else p
            src[n] = "guide"
        elif n.startswith(FINGER_GUIDE):
            fingers_g[n] = p
        else:
            put(n, p, "guide")
    # --- hands --------------------------------------------------------------------------------------------
    for s in "lr":
        hand_fingers(V, L, src, info, s, say)
    for n, p in fingers_g.items():  # a finger joint's own guide wins over the detected chain
        put(n, p, "guide")
    if guides:
        say("guides: " + ", ".join(sorted(guides)))
    return L, info


def source_landmarks(bones, H, say=None):
    """--keep-weights: the landmarks and the info build_skeleton and finish_weights need, from a source rig's bones
    {name: (head, tail)} already in the output frame, with no detection (it fails on rigs posed with the arms at the
    body). The crotch is estimated under the hip joints. ValueError names missing mannequin bones."""
    say = say or (lambda m: None)
    names = [n for n in NAMES if n != "root" and not n.startswith("ik_") and n not in TWIST_NAMES]
    tips = ["ball_" + s for s in "lr"] + [f + "_03_" + s for s in "lr" for f in FINGER_GUIDE]
    missing = sorted({n for n in names + tips if n not in bones})
    if missing:
        raise ValueError("source rig lacks mannequin bones: " + ", ".join(missing[:12])
                         + (" and %d more" % (len(missing) - 12) if len(missing) > 12 else ""))
    L = {n: np.asarray(bones[n][0], float) for n in names}
    info = {"H": float(H), "src": {n: "source rig" for n in L}, "hand": {}, "leg": {}}
    for s in "lr":
        info["toe_" + s] = np.asarray(bones["ball_" + s][1], float)
        info["hand"][s] = {"tip_" + f: np.asarray(bones[f + "_03_" + s][1], float) for f in FINGERS}
        info["hand"][s]["thumb_tip"] = np.asarray(bones["thumb_03_" + s][1], float)
    hip_z = 0.5 * (L["thigh_l"][2] + L["thigh_r"][2])
    info["crotch"] = hip_z - 0.03 * H
    info["src"]["crotch"] = "source hips"
    say("crotch %.3f from the source hips (0.03 H under them)" % info["crotch"])
    return L, info


def source_weights(V, gnames, Wk, L, info, say=None):
    """--keep-weights: the source rig's weight matrix (vertices x gnames) -> (names, W) over the mannequin's deform
    bones: other groups dropped, rows normalised, an unweighted vertex given its nearest weighted vertex's weights.
    Also fills what the hem rule needs from the rig (no detection): info["hem_gate"] (vertices the source gives
    mostly to the pelvis, spine_01 and thighs) and info["leg"] (thigh-to-calf axis, radius from the inner thigh
    vertices). ValueError when the rig carries no weights."""
    say = say or (lambda m: None)
    V = np.asarray(V, float)
    use = [i for i, n in enumerate(gnames) if DEFORM.get(n)]
    dropped = [n for n in gnames if not DEFORM.get(n)]
    if dropped:
        say("source groups with no mannequin deform bone, dropped: " + ", ".join(dropped[:10]))
    names = [gnames[i] for i in use]
    W = np.asarray(Wk, float)[:, use]
    s_ = W.sum(1, keepdims=True)
    if not len(names) or not (s_[:, 0] >= 1e-6).any():
        raise ValueError("source rig carries no skin weights on mannequin deform bones")
    say("source weights: %d vertices unweighted, row sums %.3f..%.3f, up to %d influences"
        % (int((s_[:, 0] < 1e-6).sum()), float(s_.min()), float(s_.max()), int((W > 1e-4).sum(1).max())))
    W = W / np.maximum(s_, 1e-12)
    bare = np.nonzero(s_[:, 0] < 1e-6)[0]
    if len(bare):  # an unweighted vertex would skin to nothing: it takes its nearest weighted vertex's weights
        held = np.nonzero(s_[:, 0] >= 1e-6)[0]
        for c in range(0, len(bare), 16):
            b = bare[c:c + 16]
            d = ((V[b, None, :] - V[None, held, :]) ** 2).sum(-1)
            W[b] = W[held[d.argmin(1)]]
        say("%d unweighted vertices took the weights of their nearest weighted vertex" % len(bare))

    def part(*bases):  # a segment's weight with its twist bones folded in
        cols = [k for k, n in enumerate(names) if n in bases or any(
            n.startswith(b_[:-2] + "_twist") and n.endswith(b_[-2:]) for b_ in bases if b_[-2:] in ("_l", "_r"))]
        return W[:, cols].sum(1) if cols else np.zeros(len(V))
    info["hem_gate"] = part("pelvis", "thigh_l", "thigh_r", "spine_01") > 0.5
    for s in "lr":
        c, d = L["thigh_" + s], unit(L["calf_" + s] - L["thigh_" + s])
        own = (part("thigh_" + s) > 0.5) & (V[:, 2] < c[2]) & (V[:, 2] > L["calf_" + s][2])
        rel = V[own] - c
        dist = np.linalg.norm(rel - np.outer(rel @ d, d), axis=1)
        r = float(np.percentile(dist, 15)) if len(dist) > 50 else 0.06 * info["H"]
        info["leg"][s] = dict(c=c, d=d, r=r, pts=np.zeros((0, 4)))
    info.pop("arm", None)  # the gate already leaves the arms out
    return names, W


def mesh_islands(V, F, eps=1e-5):
    """Connected pieces of the mesh (positions within eps * size welded) -> label per vertex, 0 the one with the
    most vertices."""
    V = np.asarray(V, float)
    F = np.asarray(F, int)
    size = float(np.ptp(V, axis=0).max()) if len(V) else 1.0
    rep_ = weld(V, max(eps * size, 1e-12))
    lab = np.arange(rep_.max() + 1)
    E = rep_[np.concatenate([F[:, [0, 1]], F[:, [1, 2]]])] if len(F) else np.zeros((0, 2), int)
    while True:
        m = lab.copy()
        np.minimum.at(m, E[:, 0], m[E[:, 1]])
        np.minimum.at(m, E[:, 1], m[E[:, 0]])
        m = m[m]  # pointer jumping
        if np.array_equal(m, lab):
            break
        lab = m
    _u, inv, cnt = np.unique(lab[rep_], return_inverse=True, return_counts=True)
    order = np.argsort(-cnt)
    rank = np.empty_like(order)
    rank[order] = np.arange(len(order))
    return rank[inv.reshape(-1)]


def loose_islands(V, F, say=None):
    """Warn about a separate piece that sets the soles or the height the frame and --height use: a flat, wide piece
    holding the lowest point (a ground disc or a base), or a piece holding the highest point clear of the largest
    piece's footprint (a weapon held up beside the body). Separate garment, head or hair pieces over the body pass.
    -> list of (why, vertex count, z min, z max)."""
    say = say or (lambda m: None)
    V = np.asarray(V, float)
    lab = mesh_islands(V, F)
    # the body: the tallest piece among those with at least a quarter of the largest one's vertices (a dense
    # ground disc is wide but flat)
    cnt = np.bincount(lab)
    big = [k for k in range(len(cnt)) if cnt[k] >= 0.25 * cnt[0]]
    body = max(big, key=lambda k: float(np.ptp(V[lab == k, 2])))
    main = V[lab == body]
    Ht = float(np.ptp(V[:, 2])) or 1.0
    out = []
    lo_k, hi_k = int(lab[np.argmin(V[:, 2])]), int(lab[np.argmax(V[:, 2])])
    if lo_k != body:
        P = V[lab == lo_k]
        if np.ptp(P[:, 2]) < 0.03 * Ht and np.ptp(P[:, :2], axis=0).max() > 0.2 * Ht:
            out.append(("a flat piece under the body (a ground disc or base?)", len(P), float(P[:, 2].min()),
                        float(P[:, 2].max())))
    if hi_k != body:
        P = V[lab == hi_k]
        m0, m1 = main[:, :2].min(0), main[:, :2].max(0)
        if (P[:, :2].max(0) < m0).any() or (P[:, :2].min(0) > m1).any():
            out.append(("a piece above the body beside it (a held weapon?)", len(P), float(P[:, 2].min()),
                        float(P[:, 2].max())))
    for why, n, z0, z1 in out:
        say("WARNING: %s, %d vertices, z %.3f..%.3f, sets the %s; remove it from the source or check the result"
            % (why, n, z0, z1, "soles" if "under" in why else "height"))
    return out


def hand_fingers(V, L, src, info, s, say):
    """Five finger chains and metacarpals from the hand's distal vertices; a fanned template when the angle
    histogram does not show five fingers."""
    sg = 1 if s == "l" else -1
    a = info["arm"][s]
    w = L["hand_" + s]
    AV = V[a["verts"]]
    u = unit(a["tip"] - w)
    rel = AV - w
    along = rel @ u
    Lh = float(np.percentile(along, 99.8))
    hv = AV[(along > -0.01 * info["H"])]
    hrel = hv - w
    # palm normal: least spread of the hand perpendicular to u
    # the fan's spread direction v and palm normal n: principal axes of the hand across u
    e1 = unit(np.cross(u, [0.0, 0.0, 1.0]) if abs(u[2]) < 0.9 else np.cross(u, [1.0, 0.0, 0.0]))
    e2 = np.cross(u, e1)
    q = np.c_[hrel @ e1, hrel @ e2]
    _ev, evec = np.linalg.eigh(np.cov(q.T))
    v = unit(evec[0, 1] * e1 + evec[1, 1] * e2)
    n = np.cross(u, v)
    # distal fingers: angle about the wrist in the palm plane
    pu, pv = hrel @ u, hrel @ v
    rad = np.hypot(pu, pv)
    dist = rad > 0.55 * Lh
    th = np.degrees(np.arctan2(pv, pu))
    peaks = []
    if dist.sum() > 50:
        bins = np.arange(-90, 91, 2.0)
        hist, _e = np.histogram(th[dist], bins=bins)
        hs = np.convolve(hist, [1, 2, 1], mode="same").astype(float)
        cand = [i for i in range(1, len(hs) - 1) if hs[i] >= hs[i - 1] and hs[i] > hs[i + 1] and hs[i] > 0.03 * hs.sum()]
        # split the distal points at the valleys between peaks
        cand = sorted(cand, key=lambda i: -hs[i])[:5]
        cand.sort()
        if len(cand) == 5:
            cuts = [bins[0]] + [0.5 * (bins[cand[i]] + bins[cand[i + 1]]) + 1 for i in range(4)] + [bins[-1]]
            for i in range(5):
                m = dist & (th >= cuts[i]) & (th < cuts[i + 1])
                pts = hv[m]
                d = np.linalg.norm(pts - w, axis=1)
                peaks.append((pts, d))
    if len(peaks) == 5 and min(len(p) for p, _d in peaks) < 8:
        say("hand %s: a finger cluster has under 8 vertices" % s)
        peaks = []
    if len(peaks) == 5:
        reach = [d.max() for _p, d in peaks]
        # the thumb is the shorter of the two outer clusters
        thumb_first = reach[0] < reach[4]
        order = [0, 1, 2, 3, 4] if thumb_first else [4, 3, 2, 1, 0]
        tips = []
        for i in order:
            pts, d = peaks[i]
            tips.append(pts[d >= np.percentile(d, 97)].mean(0))
        how = "detected"
    else:
        say("hand %s: %d finger clusters, template fan" % (s, len(peaks)))
        how = "template"
        side = -1.0 if np.dot(v, [0, -1, 0]) < 0 else 1.0  # thumb toward the front
        tips = [w + Lh * unit(u * math.cos(math.radians(a_)) + v * side * math.sin(math.radians(a_))) * r_
                for a_, r_ in ((45, 0.62), (12, 0.92), (3, 1.0), (-6, 0.95), (-16, 0.82))]
    names = ["thumb"] + list(FINGERS)
    info.setdefault("hand", {})[s] = dict(u=u, v=v, n=n, Lh=Lh, tips=dict(zip(names, tips)))
    for nm, tip in zip(names, tips):
        if nm == "thumb":
            for j, f in ((1, 0.22), (2, 0.55), (3, 0.78)):
                put_ = w + f * (tip - w)
                L["thumb_0%d_%s" % (j, s)] = _centre_near(hv, put_, 0.03 * Lh)
                src["thumb_0%d_%s" % (j, s)] = how
            info["hand"][s]["thumb_tip"] = tip
            continue
        knuckle = w + 0.46 * Lh * unit(tip - w)
        L["%s_metacarpal_%s" % (nm, s)] = w + 0.10 * (knuckle - w)
        for j, f in ((1, 0.0), (2, 0.45), (3, 0.75)):
            p = knuckle + f * (tip - knuckle)
            L["%s_0%d_%s" % (nm, j, s)] = _centre_near(hv, p, 0.035 * Lh)
            src["%s_0%d_%s" % (nm, j, s)] = how
        src["%s_metacarpal_%s" % (nm, s)] = how
        info["hand"][s]["tip_" + nm] = tip
    _ = sg


def _centre_near(P, p, r):
    m = np.linalg.norm(P - p, axis=1) < r
    return P[m].mean(0) if m.sum() >= 4 else np.asarray(p, float)


# ------------------------------------------------------------------------------------------------
# 3. Skeleton
# ------------------------------------------------------------------------------------------------
def build_skeleton(L, info):
    """-> {name: (head, tail, y_axis)}: head positions, a tail along the mannequin's bone X (axis_sign), and the
    bone's Y axis. Twist bones sit along their segment; ik bones copy the feet and hands; leaves extend their
    parent's direction."""
    J = {}
    H = info["H"]
    heads = dict(L)
    heads["root"] = np.zeros(3)
    for s in "lr":
        for seg, nxt, ts in TWISTS:
            a, b = heads[seg + "_" + s], heads[nxt + "_" + s]
            for t, f in ts:
                heads[t + "_" + s] = a + f * (b - a)
    heads["ik_foot_root"] = np.zeros(3)
    heads["ik_hand_root"] = np.zeros(3)
    for s in "lr":
        heads["ik_foot_" + s] = heads["foot_" + s]
        heads["ik_hand_" + s] = heads["hand_" + s]
    heads["ik_hand_gun"] = heads["hand_r"]
    children = {}
    for n, p, _d in SKELETON:
        children.setdefault(p, []).append(n)
    toward = {"pelvis": "spine_01", "spine_05": "neck_01", "hand_l": "middle_metacarpal_l",
              "hand_r": "middle_metacarpal_r", "upperarm_l": "lowerarm_l", "upperarm_r": "lowerarm_r",
              "lowerarm_l": "hand_l", "lowerarm_r": "hand_r", "thigh_l": "calf_l", "thigh_r": "calf_r",
              "calf_l": "foot_l", "calf_r": "foot_r", "foot_l": "ball_l", "foot_r": "ball_r",
              "clavicle_l": "upperarm_l", "clavicle_r": "upperarm_r"}
    for n in NAMES:
        h = heads[n]
        nxt = toward.get(n)
        if nxt is None:
            kids = [c for c in children.get(n, []) if c not in TWIST_NAMES and not c.startswith("ik_")]
            nxt = kids[0] if len(kids) == 1 else None
        if n in TWIST_NAMES:
            base = n.split("_twist")[0] + "_" + n[-1]
            d = unit(J[base][3]) if base in J else np.array([1.0, 0, 0])
            length = 0.04 * H
        elif nxt is not None and np.linalg.norm(heads[nxt] - h) > 1e-6:
            d = unit(heads[nxt] - h)
            length = np.linalg.norm(heads[nxt] - h)
        elif n == "head":
            d, length = np.array([0, 0, 1.0]), 0.10 * H
        elif n.startswith("ball"):
            d = unit(info["toe_" + n[-1]] - h)
            d = np.array([d[0], d[1], 0.0])
            if np.linalg.norm(d) < 1e-3:  # a toe straight above or below the ball: the foot's own forward
                fw = h - heads["foot_" + n[-1]]
                d = np.array([fw[0], fw[1], 0.0])
                if np.linalg.norm(d) < 1e-3 * H:
                    d = np.array([0.0, -1.0, 0.0])
            d = unit(d)
            length = 0.04 * H
        elif n.startswith("ik_") or n == "root":
            d, length = np.array([0, 1.0, 0]) if n == "root" else np.array([0, 0, 1.0]), 0.05 * H
        elif n.endswith(("_03_l", "_03_r")):
            s = n[-1]
            tip = info["hand"][s].get("thumb_tip" if n.startswith("thumb") else "tip_" + n.split("_")[0])
            d = unit(tip - h) if tip is not None else unit(h - heads[PARENT[n]])
            length = max(np.linalg.norm(tip - h) if tip is not None else 0.0, 0.008 * H)
        else:
            d, length = unit(h - heads[PARENT[n]]), 0.03 * H
        if n.startswith("foot"):  # the mannequin's foot X runs up the shin; the bone still binds toward the ball
            up = unit(heads["calf_" + n[-1]] - h)
            x = up if n.endswith("_l") else -up
        elif n in ("root", "ik_foot_root", "ik_hand_root"):
            x = np.array([1.0, 0.0, 0.0])
        else:
            x = axis_sign(n) * d
        y = y_hint(n)
        y = y - np.dot(y, x) * x
        if np.linalg.norm(y) < 1e-6:
            y = np.array([0.0, 0.0, 1.0]) - x[2] * x
        J[n] = (h, h + x * max(length, 0.004 * H), unit(y), d)
    for ik, src_ in (("ik_foot_l", "foot_l"), ("ik_foot_r", "foot_r"), ("ik_hand_l", "hand_l"), ("ik_hand_r", "hand_r"),
                     ("ik_hand_gun", "hand_r")):
        h0, t0, y0, d0 = J[src_]
        J[ik] = (h0, t0, y0, d0)
    return {n: (v[0], v[1], v[2]) for n, v in J.items()}, {n: v[3] for n, v in J.items()}


# ------------------------------------------------------------------------------------------------
# 4. Binding
# ------------------------------------------------------------------------------------------------
# Sharpness k of the exp fall-off at the joint at each bone's head (higher = a narrower blend, in local radii).
# A vertex takes the sharpness of the joint between its two nearest bones (the deeper bone's head): hinge joints
# (elbow, knee, wrist, ankle, fingers) blend over about a limb radius, ball joints (shoulder, hip) and the spine
# over two to three.
SHARP_HEAD = {"pelvis": 3.0, "spine": 3.5, "neck_01": 4.5, "neck_02": 4.5, "head": 6.0, "clavicle": 3.0,
              "upperarm": 4.0, "lowerarm": 8.0, "hand": 9.0, "finger": 9.0, "thigh": 4.5, "calf": 8.0, "foot": 8.0,
              "ball": 8.0}


def _sharp_key(n):
    if n.startswith(("spine",)):
        return "spine"
    if any(n.startswith(f) for f in FINGERS + ("thumb",)):
        return "finger"
    return n[:-2] if n.endswith(("_l", "_r")) else n


def _depth(n):
    d = 0
    while PARENT[n]:
        n, d = PARENT[n], d + 1
    return d


NO_WEIGHT = TWIST_NAMES | {"root"} | {n for n in NAMES if n.startswith("ik_")}

_OFFS = [(a, b, c) for a in (-1, 0, 1) for b in (-1, 0, 1) for c in (-1, 0, 1) if (a, b, c) != (0, 0, 0)]


def geodesic_field(g, seeds, cap, roi):
    """Chamfer distance through the solid from seed cells, inside the box roi = (lo_idx, hi_idx); np.inf
    beyond cap (metres)."""
    lo, hi = roi
    S = g.solid[lo[0]:hi[0], lo[1]:hi[1], lo[2]:hi[2]]
    D = np.full(S.shape, np.inf)
    sd = seeds - lo
    ok = np.all((sd >= 0) & (sd < np.array(S.shape)), axis=1)
    sd = sd[ok]
    D[sd[:, 0], sd[:, 1], sd[:, 2]] = 0.0
    cost = {o: g.h * math.sqrt(o[0] ** 2 + o[1] ** 2 + o[2] ** 2) for o in _OFFS}
    for _it in range(int(cap / g.h) + 3):
        prev = D
        D = D.copy()
        for o in _OFFS:
            sh = D
            for ax in range(3):
                if o[ax]:
                    sh = _shift(sh, o[ax], ax, np.inf)
            np.minimum(D, sh + cost[o], out=D)
        D[~S] = np.inf
        D[sd[:, 0], sd[:, 1], sd[:, 2]] = 0.0
        if np.array_equal(np.isfinite(D), np.isfinite(prev)) and np.allclose(D[np.isfinite(D)], prev[np.isfinite(D)]):
            break
    D[D > cap] = np.inf
    return D


def _segment_cells(g, a, b):
    n = max(2, int(np.linalg.norm(b - a) / (0.5 * g.h)) + 1)
    P = a[None] + np.linspace(0, 1, n)[:, None] * (b - a)[None]
    idx = np.unique(g.index(P), axis=0)
    inside = g.solid[idx[:, 0], idx[:, 1], idx[:, 2]]
    if inside.any():
        return idx[inside]
    # the segment runs outside the solid (a thin limb at a coarse grid): seed the nearest solid cells
    sol = np.argwhere(g.solid)
    c = g.index(0.5 * (a + b))
    d = np.linalg.norm(sol - c, axis=1)
    return sol[d <= d.min() + 1.5]


def bone_segments(J, dirs, info):
    """Deform bones as segments head -> next joint (the bone's own direction, not the signed X)."""
    segs = {}
    for n in NAMES:
        if not DEFORM[n] or n in NO_WEIGHT:
            continue
        h, t, _y = J[n]
        length = np.linalg.norm(t - h)
        if n.startswith("clavicle"):  # the collar bone stops short of the shoulder: the deltoid cap is the arm's
            length *= 0.55
        segs[n] = (h, h + dirs[n] * length)
    return segs


def bind(V, F, J, dirs, info, max_infl=8, smooth_iters=3, twist=True, hem=True, log=None):
    """-> (names, W) with W[v, b] the weight of vertex v on deform bone names[b] (rows sum to 1)."""
    say = log or (lambda *_a: None)
    V = np.asarray(V, float)
    g = info["grid"]
    H = info["H"]
    segs = bone_segments(J, dirs, info)
    names = list(segs)
    nv = len(V)
    # vertex -> nearest solid cell
    vi = g.index(V)
    bad = ~g.solid[vi[:, 0], vi[:, 1], vi[:, 2]]
    if bad.any():
        for k in np.nonzero(bad)[0]:
            best = None
            for o in _OFFS:
                c = vi[k] + np.array(o)
                if np.all(c >= 0) and np.all(c < np.array(g.shape)) and g.solid[tuple(c)]:
                    best = c
                    break
            if best is not None:
                vi[k] = best
    D = np.full((nv, len(names)), np.inf)
    for b, n in enumerate(names):
        a, e = segs[n]
        cap = (0.06 if chain_of(n) == "hand" else 0.16 if n.startswith(("spine", "pelvis", "clavicle")) else 0.12) * H
        seeds = _segment_cells(g, a, e)
        if n == "pelvis":  # the pelvis bone spans the hips, not just its short stub up to spine_01
            seeds = np.unique(np.concatenate([seeds, _segment_cells(g, J["thigh_l"][0], J["thigh_r"][0])]), axis=0)
        lo = np.maximum(seeds.min(0) - int(cap / g.h) - 2, 0)
        hi = np.minimum(seeds.max(0) + int(cap / g.h) + 3, np.array(g.shape))
        F_ = geodesic_field(g, seeds, cap, (lo, hi))
        inb = np.all((vi >= lo) & (vi < hi), axis=1)
        idx = vi[inb] - lo
        D[inb, b] = F_[idx[:, 0], idx[:, 1], idx[:, 2]]
    # vertices no field reached (outside every cap): Euclidean distance to the segments
    lost = ~np.isfinite(D).any(1)
    if lost.any():
        say("%d vertices beyond every geodesic cap: Euclidean fallback" % lost.sum())
        for b, n in enumerate(names):
            a, e = segs[n]
            d = e - a
            t = np.clip(((V[lost] - a) @ d) / max(d @ d, 1e-12), 0, 1)
            D[lost, b] = np.linalg.norm(V[lost] - (a + np.outer(t, d)), axis=1)
    dmin = D.min(1)
    k = np.array([SHARP_HEAD.get(_sharp_key(n), 4.0) for n in names])
    dep = np.array([_depth(n) for n in names])
    two = np.argsort(D, axis=1)[:, :2]
    deeper = np.where(dep[two[:, 0]] >= dep[two[:, 1]], two[:, 0], two[:, 1])
    kv = k[deeper]
    rloc = np.maximum(dmin, 0.012 * H)
    W = np.exp(-kv[:, None] * (D - dmin[:, None]) / rloc[:, None])
    W[~np.isfinite(D)] = 0.0
    # fingers and metacarpals only blend inside their own hand; the hand bone blends with the forearm
    W[W < 1e-3] = 0.0
    W /= W.sum(1, keepdims=True)
    return finish_weights(V, F, J, info, names, W, max_infl=max_infl, smooth_iters=smooth_iters, twist=twist,
                          hem=hem, log=log)


def finish_weights(V, F, J, info, names, W, max_infl=8, smooth_iters=3, twist=True, hem=True, log=None):
    """The shared clean-up of a weight matrix W (vertices x names, rows summing to 1), whatever bound it: the
    hem rule, Laplacian smoothing on the welded surface, twist bands (any weight already on a twist bone is
    folded back into its segment first, then re-spread along the segment), prune under 0.01, keep the
    max_infl largest, normalise. J gives the joint heads ({name: (head, ...)}). -> (names, W).

    The twist bands are tents along each segment: the segment bone at 0, its twist bones at their TWISTS
    fractions. Past its first twist bone the segment bone holds no weight, and past the last one the last twist
    bone holds it all (a forearm vertex at 0.9 of the way to the wrist is all lowerarm_twist_01). That skin rolls
    with the limb only when the twist bones are driven: in Unreal FTBuildRig bakes their tracks after the retarget
    (its default; -NoBakeTwist leaves them at the bind pose, and the skin then follows the segment without rolling).
    twist=False leaves twist columns as W has them (a source rig's own twist weights kept, none added) and adds no
    bands. A segment with no column in W (a source rig without that group) gets no band, with a log line."""
    say = log or (lambda m: None)
    nv = len(V)
    H = info["H"]
    names = list(names)
    W = np.asarray(W, float).copy()
    if twist:
        for tn in [n for n in names if n in TWIST_NAMES]:
            base = tn.split("_twist")[0] + "_" + tn[-1]
            if base not in names:  # nowhere to fold it: keep the source's twist column as it is
                say("%s kept: no %s column to fold it into" % (tn, base))
                continue
            W[:, names.index(base)] += W[:, names.index(tn)]
            keep = [i for i, n in enumerate(names) if n != tn]
            W, names = W[:, keep], [names[i] for i in keep]
    col = {n: i for i, n in enumerate(names)}
    if hem:
        W = hem_rule(V, W, col, info, say)
    # Laplacian smoothing on the welded surface
    rep = weld(V, 1e-5 * H)
    E = np.concatenate([F[:, [0, 1]], F[:, [1, 2]], F[:, [2, 0]]])
    E = rep[E]
    E = E[E[:, 0] != E[:, 1]]
    nu = rep.max() + 1
    Wu = np.zeros((nu, W.shape[1]))
    np.add.at(Wu, rep, W)
    cnt = np.bincount(rep, minlength=nu)[:, None]
    Wu /= np.maximum(cnt, 1)
    deg = np.bincount(E[:, 0], minlength=nu) + np.bincount(E[:, 1], minlength=nu)
    for _it in range(smooth_iters):
        acc = np.zeros_like(Wu)
        np.add.at(acc, E[:, 0], Wu[E[:, 1]])
        np.add.at(acc, E[:, 1], Wu[E[:, 0]])
        nb = acc / np.maximum(deg, 1)[:, None]
        Wu = np.where(deg[:, None] > 0, 0.5 * Wu + 0.5 * nb, Wu)
    W = Wu[rep]
    # twist bands
    allnames = names + ([t for t in sorted(TWIST_NAMES)] if twist else [])
    if twist:
        W = np.concatenate([W, np.zeros((nv, len(TWIST_NAMES)))], axis=1)
        col = {n: i for i, n in enumerate(allnames)}
        for s in "lr":
            for seg, nxt, ts in TWISTS:
                if seg + "_" + s not in col:
                    say("no %s_%s weights to spread: no twist band there" % (seg, s))
                    continue
                a, e = J[seg + "_" + s][0], J[nxt + "_" + s][0]
                d = e - a
                t = np.clip(((V - a) @ d) / max(d @ d, 1e-12), 0.0, 1.0)
                nodes = [(seg + "_" + s, 0.0)] + [(tn + "_" + s, f) for tn, f in ts]
                tot = W[:, col[seg + "_" + s]].copy()
                share = np.zeros((nv, len(nodes)))
                pos = [p for _n, p in nodes]
                for i in range(len(nodes)):
                    sh = np.ones(nv)
                    if i > 0:
                        sh = np.minimum(sh, np.clip((t - pos[i - 1]) / (pos[i] - pos[i - 1]), 0, 1))
                    if i + 1 < len(nodes):
                        sh = np.minimum(sh, np.clip((pos[i + 1] - t) / (pos[i + 1] - pos[i]), 0, 1))
                    share[:, i] = sh
                share /= np.maximum(share.sum(1, keepdims=True), 1e-12)
                for i, (n, _p) in enumerate(nodes):
                    W[:, col[n]] = share[:, i] * tot
    names = allnames
    # prune, top-K, normalise
    W[W < 0.01] = 0.0
    if W.shape[1] > max_infl:
        thr = -np.sort(-W, axis=1)[:, max_infl - 1:max_infl]
        W = np.where(W >= thr, W, 0.0)
        # ties at the cut: keep the first max_infl
        over = (W > 0).sum(1) > max_infl
        for v in np.nonzero(over)[0]:
            keep = np.argsort(-W[v])[:max_infl]
            m = np.zeros(W.shape[1], bool)
            m[keep] = True
            W[v, ~m] = 0.0
    s = W.sum(1, keepdims=True)
    W = W / np.maximum(s, 1e-12)
    return names, W


def hem_rule(V, W, col, info, say):
    """Loose cloth below the crotch (a coat or jacket hem, a skirt) hangs from the pelvis: blended from the
    geodesic weights toward pelvis (1 - a) and thighs (a, split softly across x = 0), a rising from 0 at the
    crotch to 0.55 at the hem, by how far the vertex stands off the leg's centreline beyond the leg radius."""
    H = info["H"]
    crotch = info["crotch"]
    leg = info["leg"]
    lacking = [n for n in ("pelvis", "thigh_l", "thigh_r") if n not in col]
    if lacking:
        say("hem rule skipped: no %s weights" % ", ".join(lacking))
        return W
    zc = V[:, 2]
    cand = (zc < crotch + 0.02 * H) & (zc > 0.08 * H)
    for s in info.get("arm", {}):  # hands hanging beside the hips are not cloth
        cand[info["arm"][s]["verts"]] = False
    if info.get("hem_gate") is not None:  # a caller that knows the body parts (a source rig) limits the cloth
        cand &= info["hem_gate"]
    if not cand.any():
        return W
    ex = np.full(len(V), np.inf)  # stand-off beyond the nearer leg's surface, in leg radii
    for s in "lr":
        c, d, r = leg[s]["c"], leg[s]["d"], leg[s]["r"]
        pts = leg[s]["pts"]
        if len(pts) >= 3:  # the leg's own radius at each height (wider trouser cuffs are leg, not cloth)
            o = np.argsort(pts[:, 2])
            r = np.interp(zc, pts[o, 2], pts[o, 3])
        rel = V - c
        dist = np.linalg.norm(rel - np.outer(rel @ d, d), axis=1)
        ex = np.minimum(ex, (dist - 1.3 * r) / r)
    ex = np.where(cand, ex, -1.0)
    cloth = cand & (ex > 0)
    if not cloth.any():
        return W
    # the hanging cloth runs down from the crotch without a break; flared cuffs and shoes below a gap are not it
    hem_z = crotch
    for z in np.arange(crotch, 0.08 * H, -0.01 * H):
        if not (cloth & (zc < z) & (zc >= z - 0.02 * H)).any():
            break
        hem_z = z - 0.02 * H
    cloth &= zc >= hem_z
    if crotch - hem_z < 0.03 * H:
        hem_z = crotch - 0.03 * H
    t = np.clip((crotch + 0.02 * H - zc) / (crotch + 0.02 * H - hem_z), 0, 1)
    a = 0.55 * t
    f = smoothstep(0.5 + V[:, 0] / (0.10 * H))
    target = np.zeros_like(W)
    target[:, col["pelvis"]] = 1 - a
    target[:, col["thigh_l"]] = a * f
    target[:, col["thigh_r"]] = a * (1 - f)
    c = np.where(cloth, smoothstep(ex), 0.0)[:, None]
    say("hem rule: %d cloth vertices from z %.3f to the crotch %.3f" % (cloth.sum(), hem_z, crotch))
    info["hem"] = dict(count=int(cloth.sum()), hem_z=float(hem_z))
    return (1 - c) * W + c * target


# ------------------------------------------------------------------------------------------------
# Self-check: a synthetic A-pose figure of overlapping closed capsules with known joints
# ------------------------------------------------------------------------------------------------
def _capsule(a, b, r, seg=12, rings=6):
    a, b = np.asarray(a, float), np.asarray(b, float)
    ax = unit(b - a)
    t1 = unit(np.cross(ax, [0.3, 0.5, 0.8]))
    t2 = np.cross(ax, t1)
    V, F = [], []
    prof = []  # (axial offset, radius) along the capsule, caps as hemispheres
    for i in range(rings + 1):
        ph = -math.pi / 2 + (math.pi / 2) * i / rings
        prof.append((r * math.sin(ph), r * math.cos(ph), 0))
    for i in range(rings + 1):
        ph = (math.pi / 2) * i / rings
        prof.append((r * math.sin(ph), r * math.cos(ph), 1))
    L = np.linalg.norm(b - a)
    for off, rr, end in prof:
        c = a + ax * (off + (L if end else 0.0))
        for j in range(seg):
            an = 2 * math.pi * j / seg
            V.append(c + rr * (math.cos(an) * t1 + math.sin(an) * t2))
    n = len(prof)
    for i in range(n - 1):
        for j in range(seg):
            p, q = i * seg + j, i * seg + (j + 1) % seg
            F += [(p, q, q + seg), (p, q + seg, p + seg)]
    return np.array(V), np.array(F)


def synthetic_figure():
    """-> (V, F, joints): 1.8 m, A-pose (arms 45 degrees down), five spread fingers per hand."""
    J = {"pelvis": (0, 0, 0.98), "neck_01": (0, 0, 1.47), "head": (0, 0, 1.62), "head_top": (0, 0, 1.80)}
    for s, sg in (("l", 1), ("r", -1)):
        sh = np.array([sg * 0.19, 0, 1.40])
        d = np.array([sg * math.cos(math.radians(45)), 0, -math.sin(math.radians(45))])
        J["upperarm_" + s] = sh
        J["lowerarm_" + s] = sh + d * 0.30
        J["hand_" + s] = sh + d * 0.55
        J["thigh_" + s] = np.array([sg * 0.10, 0, 0.92])
        J["calf_" + s] = np.array([sg * 0.11, -0.01, 0.50])
        J["foot_" + s] = np.array([sg * 0.12, 0, 0.08])
        J["toe_" + s] = np.array([sg * 0.12, -0.17, 0.03])
    parts = [(J["pelvis"], (0, 0, 1.32), 0.15), ((0, 0, 1.20), (0, 0, 1.36), 0.17),
             ((0, 0, 1.36), J["head"], 0.06), ((0, 0, 1.68), (0, 0, 1.70), 0.10)]
    for s, sg in (("l", 1), ("r", -1)):
        parts += [((sg * 0.10, 0, 1.40), J["upperarm_" + s], 0.065), (J["upperarm_" + s], J["lowerarm_" + s], 0.045),
                  (J["lowerarm_" + s], J["hand_" + s], 0.035), (J["thigh_" + s], J["calf_" + s], 0.065),
                  (J["calf_" + s], J["foot_" + s], 0.05), (J["foot_" + s] + np.array([0, 0.04, -0.04]),
                                                           J["toe_" + s], 0.035)]
        w = np.asarray(J["hand_" + s], float)
        u = unit(J["hand_" + s] - J["lowerarm_" + s])
        v = np.array([0, -1.0, 0])  # palms back, thumb forward
        parts.append((w, w + u * 0.08, 0.03))
        for ang, ln in ((50, 0.10), (14, 0.17), (2, 0.19), (-10, 0.18), (-22, 0.15)):
            dd = unit(u * math.cos(math.radians(ang)) + v * math.sin(math.radians(ang)))
            st = w + u * 0.06 + v * 0.015 * math.sin(math.radians(ang)) * 3
            parts.append((st, st + dd * ln, 0.009))
    Vs, Fs, o = [], [], 0
    for a, b, r in parts:
        v_, f_ = _capsule(a, b, r)
        Vs.append(v_)
        Fs.append(f_ + o)
        o += len(v_)
    V = np.concatenate(Vs)
    V[:, 2] -= V[:, 2].min()
    return V, np.concatenate(Fs), {k: np.asarray(v, float) for k, v in J.items()}


def _selfcheck():
    V, F, J = synthetic_figure()
    off, turn = normalize(V)
    assert not turn, "facing"
    assert abs(off[0]) < 0.01, off
    V = V + off
    msgs = []
    L, info = find_landmarks(V, F, voxels=140, log=msgs.append)
    H = info["H"]
    shift = off
    errs = {}
    for n in ("upperarm_l", "upperarm_r", "lowerarm_l", "hand_l", "hand_r", "foot_l", "foot_r", "calf_l"):
        errs[n] = float(np.linalg.norm(L[n] - (J[n] + shift)))
    tol = {"upperarm": 0.06, "lowerarm": 0.05, "hand": 0.04, "foot": 0.04, "calf": 0.05}
    for n, e in errs.items():
        lim = tol[n.rsplit("_", 1)[0]] * H
        assert e < lim, "%s off by %.3f m (limit %.3f): %s" % (n, e, lim, msgs)
    # the crotch lies between the hip joints and the knees
    assert J["calf_l"][2] + shift[2] < info["crotch"] < J["thigh_l"][2] + shift[2] + 0.05, info["crotch"]
    assert L["neck_01"][2] < L["head"][2] < H, (L["neck_01"], L["head"])
    assert all(info["src"].get("%s_01_l" % f) == "detected" for f in FINGERS), info["src"]
    Jt, dirs = build_skeleton(L, info)
    assert set(Jt) == set(NAMES)
    # axis convention: bone X along the bone on the left arm, against it on the right arm and the left leg
    x = unit(Jt["lowerarm_l"][1] - Jt["lowerarm_l"][0])
    assert np.dot(x, unit(L["hand_l"] - L["lowerarm_l"])) > 0.99
    x = unit(Jt["lowerarm_r"][1] - Jt["lowerarm_r"][0])
    assert np.dot(x, unit(L["hand_r"] - L["lowerarm_r"])) < -0.99
    x = unit(Jt["thigh_l"][1] - Jt["thigh_l"][0])
    assert x[2] > 0.95, x
    for n in NAMES:
        h, t, y = Jt[n]
        assert abs(np.dot(unit(t - h), y)) < 1e-6, n
    names, W = bind(V, F, Jt, dirs, info, max_infl=4)
    assert np.allclose(W.sum(1), 1.0) and ((W > 0).sum(1) <= 4).all()
    col = {n: i for i, n in enumerate(names)}

    def top(p):
        i = np.argmin(np.linalg.norm(V - p, axis=1))
        return names[int(np.argmax(W[i]))]
    # a vertex on the forearm's middle follows the forearm chain, one on the shin the calf chain
    mid_fa = 0.5 * (L["lowerarm_l"] + L["hand_l"]) + np.array([0, 0, 0.035])
    assert top(mid_fa).startswith("lowerarm"), top(mid_fa)
    assert top(0.5 * (L["calf_r"] + L["foot_r"]) + np.array([0, -0.05, 0])).startswith("calf"), "shin"
    # no left-leg weight on the right leg's surface
    rleg = (V[:, 0] < -0.05) & (V[:, 2] < 0.4)
    lw = sum(W[rleg, col[n]].sum() for n in names if n.endswith("_l"))
    assert lw < 1e-6 * rleg.sum() + 1e-9, lw
    _check_right_axes(Jt, L)
    _check_turned(J)
    _check_finish_weights()
    _check_hem_rule()
    _check_keep_weights(V, F, Jt, names, W, H)
    _check_guides(V, F, L)
    _check_ball_and_islands(L, info, V, F)
    print("autorig_core self-check OK (%d vertices, %d bones, joint errors cm: %s)" % (
        len(V), len(names), ", ".join("%s %.1f" % (n, 100 * e) for n, e in errs.items())))


def _check_right_axes(Jt, L):
    """The right side's bone X signs beyond the forearm: against the bone on the arm and hand, along it on the leg,
    against it on the ball; the left leg's thigh X runs against the bone (up)."""
    def x_of(n):
        return unit(Jt[n][1] - Jt[n][0])
    assert np.dot(x_of("upperarm_r"), unit(L["lowerarm_r"] - L["upperarm_r"])) < -0.99
    assert np.dot(x_of("hand_r"), unit(L["hand_r"] - L["lowerarm_r"])) < -0.5
    assert x_of("thigh_r")[2] < -0.95 and x_of("calf_r")[2] < -0.95, (x_of("thigh_r"), x_of("calf_r"))
    assert x_of("calf_l")[2] > 0.95
    assert x_of("ball_r")[1] > 0.9 and x_of("ball_l")[1] < -0.9, (x_of("ball_r"), x_of("ball_l"))  # toes toward -Y
    assert x_of("clavicle_r")[0] > 0.9 and x_of("clavicle_l")[0] > 0.9


def _check_turned(J):
    """A figure turned 180 degrees about Z (facing +Y) is detected as turned and lands where the unturned one does."""
    V, _F, _J = synthetic_figure()
    off0, turn0 = normalize(V)
    Vt = V * np.array([-1.0, -1.0, 1.0]) + np.array([0.3, -0.2, 0.5])
    off1, turn1 = normalize(Vt)
    assert not turn0 and turn1, (turn0, turn1)
    back = Vt * np.array([-1.0, -1.0, 1.0]) + off1
    err = np.abs(back - (V + off0)).max()
    assert err < 0.01, err


def _limb_fixture():
    """A left forearm along +X from (0.3, 0, 1.3) to (0.6, 0, 1.3), joints for every segment pair (the twist loop
    visits them all), and four vertices: t = 0.9 and t = 0.6 on the forearm, t = 0.6 split with an old twist
    column, and one spread over six bones."""
    J = {}
    for s, sg in (("l", 1.0), ("r", -1.0)):
        J["upperarm_" + s] = (np.array([sg * 0.1, 0, 1.3]),)
        J["lowerarm_" + s] = (np.array([sg * 0.3, 0, 1.3]),)
        J["hand_" + s] = (np.array([sg * 0.6, 0, 1.3]),)
        J["thigh_" + s] = (np.array([sg * 0.1, 0, 0.9]),)
        J["calf_" + s] = (np.array([sg * 0.1, 0, 0.5]),)
        J["foot_" + s] = (np.array([sg * 0.1, 0, 0.1]),)
    V = np.array([[0.57, 0.0, 1.33], [0.48, 0.0, 1.33], [0.48, 0.02, 1.27], [0.0, 0.0, 1.5]])
    F = np.array([[0, 1, 2]])
    names = ["pelvis", "spine_01", "spine_02", "spine_03", "lowerarm_l", "hand_l", "lowerarm_twist_01_l"]
    W = np.zeros((4, len(names)))
    W[0, 4] = W[1, 4] = 1.0
    W[2, 4], W[2, 6] = 0.5, 0.5
    W[3, :6] = [0.30, 0.25, 0.20, 0.13, 0.08, 0.04]
    info = {"H": 1.8, "crotch": 0.8}
    return V, F, J, info, names, W


def _check_finish_weights():
    V, F, J, info, names, W = _limb_fixture()
    msgs = []
    out, Wo = finish_weights(V, F, J, info, names, W, max_infl=4, smooth_iters=0, hem=False, log=msgs.append)
    c = {n: i for i, n in enumerate(out)}
    assert np.allclose(Wo.sum(1), 1.0) and ((Wo > 0).sum(1) <= 4).all()
    # t = 0.9, past the last twist bone (0.80): all lowerarm_twist_01, none on the forearm bone
    assert abs(Wo[0, c["lowerarm_twist_01_l"]] - 1.0) < 1e-9 and Wo[0, c["lowerarm_l"]] == 0.0, Wo[0]
    # t = 0.6, between twist_02 (0.45) and twist_01 (0.80): the tent shares 4/7 and 3/7
    assert abs(Wo[1, c["lowerarm_twist_02_l"]] - 4 / 7) < 1e-9 and abs(Wo[1, c["lowerarm_twist_01_l"]] - 3 / 7) < 1e-9
    # an old twist column folds back into the segment first, so a split vertex lands where a plain one does
    assert np.allclose(Wo[2], Wo[1]), (Wo[2], Wo[1])
    # the influence cap keeps the four largest (0.30, 0.25, 0.20, 0.13), renormalised
    keep = np.array([0.30, 0.25, 0.20, 0.13])
    got = np.array([Wo[3, c[n]] for n in ("pelvis", "spine_01", "spine_02", "spine_03")])
    assert np.allclose(got, keep / keep.sum()), got
    assert Wo[3, c["lowerarm_l"]] == 0.0 and Wo[3, c["hand_l"]] == 0.0
    # every segment without a column in W says so (no KeyError)
    assert sum("no twist band there" in m for m in msgs) == 7, msgs
    # twist=False keeps the source twist column as given and adds none
    out2, W2 = finish_weights(V, F, J, info, names, W, max_infl=4, smooth_iters=0, twist=False, hem=False)
    assert out2 == names and np.allclose(W2[2, [4, 6]], [0.5, 0.5]) and np.allclose(W2[0, 4], 1.0)
    # a twist column with no segment column to fold into is kept, so its vertices keep their weight
    names3 = [n for n in names if n != "lowerarm_l"]
    W3 = np.delete(W, 4, axis=1)
    W3[0, 5] = W3[1, 5] = 1.0  # vertices 0 and 1 now on lowerarm_twist_01_l only
    msgs3 = []
    out3, Wo3 = finish_weights(V, F, J, info, names3, W3, max_infl=4, smooth_iters=0, hem=False, log=msgs3.append)
    assert "lowerarm_twist_01_l" in out3 and np.allclose(Wo3.sum(1), 1.0), Wo3
    assert Wo3[0, out3.index("lowerarm_twist_01_l")] == 1.0, Wo3[0]
    assert any("lowerarm_twist_01_l kept" in m for m in msgs3), msgs3


def _check_hem_rule():
    """A coat-front column hanging from the crotch blends toward the pelvis (thighs split evenly at x = 0); a vertex
    on a leg keeps its weights; a mesh without thigh columns is left alone with a log line."""
    H, crotch = 1.8, 0.8
    leg = {s: dict(c=np.array([sg * 0.1, 0, 0.9]), d=np.array([0, 0, -1.0]), r=0.06, pts=np.zeros((0, 4)))
           for s, sg in (("l", 1), ("r", -1))}
    zs = np.arange(0.79, 0.45, -0.01)
    cloth = np.c_[np.zeros(len(zs)), np.full(len(zs), -0.30), zs]
    onleg = np.array([[0.1, -0.04, 0.6]])
    V = np.r_[cloth, onleg]
    col = {"pelvis": 0, "thigh_l": 1, "thigh_r": 2}
    W = np.zeros((len(V), 3))
    W[:-1, 1] = 0.5
    W[:-1, 2] = 0.5
    W[-1, 1] = 1.0
    info = dict(H=H, crotch=crotch, leg=leg)
    msgs = []
    Wh = hem_rule(V, W.copy(), col, info, msgs.append)
    assert info["hem"]["count"] == len(zs), info["hem"]
    low = len(zs) - 1  # the hem's lowest vertex: a = 0.55 t toward the thighs (t by height between crotch and hem)
    top = crotch + 0.02 * H
    a = 0.55 * (top - zs[-1]) / (top - info["hem"]["hem_z"])
    assert 0.5 < a <= 0.55 and abs(Wh[low, 0] - (1 - a)) < 1e-6 and abs(Wh[low, 1] - Wh[low, 2]) < 1e-9, (Wh[low], a)
    assert Wh[0, 0] > 0.9, Wh[0]  # just under the crotch: nearly all pelvis
    assert np.allclose(Wh[-1], W[-1]), Wh[-1]
    msgs = []
    assert np.array_equal(hem_rule(V, W, {"pelvis": 0}, dict(info), msgs.append), W) and "hem rule skipped" in msgs[0]


def _check_keep_weights(V, F, Jt, names, W, H):
    """--keep-weights on a synthetic rig: the bound figure's own skeleton and weights as the source, plus a group
    no mannequin bone has and an unweighted vertex. The joints come back without detection, the extra group is
    dropped, the bare vertex takes its neighbour's weights, and the repaired matrix is clean."""
    bones = {n: (Jt[n][0], Jt[n][1]) for n in Jt}
    msgs = []
    L2, info2 = source_landmarks(bones, H, msgs.append)
    assert "grid" not in info2 and "arm" not in info2
    J2, _d2 = build_skeleton(L2, info2)
    for n in ("hand_l", "calf_r", "index_02_l", "ball_r"):
        assert np.allclose(J2[n][0], Jt[n][0]), n
    gn = list(names) + ["weapon_grip"]
    Wk = np.c_[W, np.zeros(len(W))]
    Wk[0] = 0.0
    Wk[1, -1] = 0.7  # weight on the extra group only counts toward the row before it is dropped
    srcn, Ws = source_weights(V, gn, Wk, L2, info2, msgs.append)
    assert "weapon_grip" not in srcn and any("dropped: weapon_grip" in m for m in msgs)
    near = np.argsort(np.linalg.norm(V - V[0], axis=1))[1:]
    assert np.allclose(Ws[0], Ws[near[(Wk[near, :-1].sum(1) > 1e-6)][0]])
    assert set(info2["leg"]) == {"l", "r"} and info2["hem_gate"].dtype == bool
    out, Wo = finish_weights(V, F, J2, info2, srcn, Ws, max_infl=6, smooth_iters=1, hem=True, log=msgs.append)
    assert np.allclose(Wo.sum(1), 1.0) and ((Wo > 0).sum(1) <= 6).all()
    try:
        source_landmarks({k: v for k, v in bones.items() if k != "ball_l"}, H)
        raise AssertionError("a rig without ball_l passed")
    except ValueError as e:
        assert "ball_l" in str(e)
    try:
        source_weights(V, gn, np.zeros_like(Wk), L2, dict(info2))
        raise AssertionError("an unweighted rig passed")
    except ValueError as e:
        assert "no skin weights" in str(e)


def _check_guides(V, F, L):
    for bad, word in (({"root": [0, 0, 0]}, "root"), ({"upperarm_twist_01_l": [0, 0, 1]}, "twist"),
                      ({"hand_l": [1, 2]}, "three"), ({"hand_l": [0, "x", 1]}, "three"), ({"crotch": [0, 0, float("nan")]}, "finite")):
        try:
            check_guides(bad)
            raise AssertionError("guide passed: %r" % bad)
        except ValueError as e:
            assert word in str(e) or "unknown" in str(e), (bad, e)
    moved = L["hand_l"] + np.array([0.03, 0.0, -0.02])
    g = {"crotch": [0.0, 0.0, 0.70], "hand_l": moved.tolist(), "ring_02_r": (L["ring_02_r"] + 0.01).tolist()}
    Lg, ig = find_landmarks(V, F, voxels=100, guides=g)
    assert abs(ig["crotch"] - 0.70) < 1e-9 and ig["src"]["crotch"] == "guide"
    assert np.allclose(Lg["hand_l"], moved) and np.allclose(Lg["ring_02_r"], L["ring_02_r"] + 0.01)
    # the finger chains are built from the guided hand: they move with it
    assert np.linalg.norm(Lg["middle_01_l"] - L["middle_01_l"]) > 0.01, (Lg["middle_01_l"], L["middle_01_l"])
    assert "crotch" not in Lg


def _check_ball_and_islands(L, info, V, F):
    """A toe straight under the ball still gives the ball bone a forward direction; a ground disc under the feet
    is reported as a loose piece, a body alone is not."""
    i2 = dict(info)
    i2["toe_l"] = L["ball_l"] - np.array([0, 0, 0.02])
    Jb, _d = build_skeleton(L, i2)
    x = unit(Jb["ball_l"][1] - Jb["ball_l"][0])
    assert np.isfinite(x).all() and abs(np.linalg.norm(x) - 1) < 1e-6 and x[1] < -0.9, x
    assert loose_islands(V, F) == []
    an = np.linspace(0, 2 * math.pi, 24, endpoint=False)
    disc = np.c_[0.4 * np.cos(an), 0.4 * np.sin(an), np.full(24, -0.05)]
    Vd = np.r_[V, disc, [[0.0, 0.0, -0.05]]]
    c0 = len(V)
    Fd = np.r_[F, [[c0 + 24, c0 + i, c0 + (i + 1) % 24] for i in range(24)]]
    msgs = []
    got = loose_islands(Vd, Fd, msgs.append)
    assert len(got) == 1 and got[0][1] == 25 and "WARNING" in msgs[0], (got, msgs)


if __name__ == "__main__":
    _selfcheck()
    sys.exit(0)
