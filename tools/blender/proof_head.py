"""The procedural proof head (docs/rigging_plan.md §13.2 P1a): a small, fully scripted face that carries
all 52 facial-action shapes + the house shapes, rigid eyes/teeth/tongue on the face joints of the
canonical skeleton, region masks (`rgn_<mask>` vertex groups) and a mouth bag. It exists to prove the
pipeline (validator, export profile, loader, CPU morph, contact sheets), not to be pretty.

Everything is one mesh object (one glTF mesh, one primitive per material) so all primitives share the
same morph target list. Geometry is built for the left side / centre and mirrored, so L/R shapes are
exact mirrors (F11). Blender axes: X = character left, -Y = forward, Z up, metres.
"""
import math

import numpy as np

import face_spec as fs
import rig_spec as rs

C = np.array(rs.HEAD_CENTER)
RAD = np.array(rs.HEAD_RADII)
MZ = rs.MOUTH_Z
EX, EZ, ER = rs.EYE_X, rs.EYE_Z, rs.EYE_R
EYE_C = np.array([EX, rs.EYE_Y, EZ])
MOUTH_W = 0.025  # half width of the mouth slit
MM = 0.001

# island codes
SKIN, LID_U, LID_L, BROW, EYE, TEETH_U, TEETH_L, TONGUE, BAG = range(9)
MATERIALS = ["skin", "lips", "brow", "sclera", "iris", "pupil", "teeth", "tongue", "mouth"]
MAT = {n: i for i, n in enumerate(MATERIALS)}
MAT_COLOR = {"skin": (0.80, 0.60, 0.50), "lips": (0.70, 0.36, 0.36), "brow": (0.18, 0.12, 0.09),
             "sclera": (0.93, 0.92, 0.88), "iris": (0.22, 0.42, 0.55), "pupil": (0.03, 0.03, 0.03),
             "teeth": (0.95, 0.93, 0.85), "tongue": (0.75, 0.35, 0.38), "mouth": (0.12, 0.03, 0.04)}

LID_UPPER_E = [20.0, 30.0, 42.0, 56.0, 70.0, 85.0]
LID_LOWER_E = [-22.0, -32.0, -44.0, -58.0, -72.0]
LID_PSI = list(np.linspace(-75.0, 75.0, 13))
LID_R_U, LID_R_L = ER + 0.0013, ER + 0.0009


def smoothstep(e0, e1, x):
    t = np.clip((np.asarray(x, dtype=float) - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def gauss(x, z, x0, z0, sx, sz):
    return np.exp(-((x - x0) / sx) ** 2 - ((z - z0) / sz) ** 2)


def forward_bump(x, z):
    """Sculpt on top of the ellipsoid: metres pushed toward -Y (negative = recessed)."""
    ax = np.abs(x)
    b = 0.016 * gauss(x, z, 0.0, 1.670, 0.0095, 0.017)          # nose bridge + ridge
    b += 0.004 * gauss(x, z, 0.0, 1.655, 0.008, 0.007)          # nose tip
    b += 0.0035 * gauss(x, z, 0.0, MZ, 0.024, 0.011)            # lips
    b += 0.004 * gauss(x, z, 0.0, 1.598, 0.018, 0.012)          # chin
    b += 0.003 * gauss(ax, z, 0.03, EZ + 0.019, 0.022, 0.008)   # brow ridge
    b -= 0.0015 * gauss(ax, z, EX, EZ, 0.013, 0.010)           # eye socket
    b += 0.002 * gauss(ax, z, 0.045, EZ - 0.022, 0.015, 0.012)  # cheekbones
    return b


def ellipsoid_point(theta, phi):
    st = np.sin(theta)
    return np.stack([C[0] + RAD[0] * st * np.sin(phi), C[1] - RAD[1] * st * np.cos(phi),
                     C[2] + RAD[2] * np.cos(theta)], axis=-1)


def front_factor(y):
    return smoothstep(0.0, 0.5, -(y - C[1]) / RAD[1])


EYE_RIM_DEPTH = 0.0045  # the skin rim around each eyeball sits this far in front of the eyeball's centre


def eye_rim(x, y, z):
    """Brings the skin around each eye forward so the lids and rim cover the eyeball's equator. Without it the
    eyeball stood ~3.5 mm proud of the skull at its lateral side and cast a hard wedge shadow onto the temple
    (the dark wedge beside the eye on every contact sheet). Only moves skin forward (toward -Y), never back;
    inside the eyeball's silhouette the skin stays behind the visible eyeball (F12 lids_eyes is lid-only)."""
    r = np.hypot(np.abs(x) - EX, z - EZ)
    target = EYE_C[1] - EYE_RIM_DEPTH
    w = smoothstep(ER - 0.003, ER + 0.0005, r) * (1.0 - smoothstep(ER + 0.003, ER + 0.014, r))
    w = w * front_factor(y)
    return y + w * np.minimum(target - y, 0.0)


def sculpt(p):
    p = p.copy()
    p[..., 1] -= forward_bump(p[..., 0], p[..., 2]) * front_factor(p[..., 1])
    p[..., 1] = eye_rim(p[..., 0], p[..., 1], p[..., 2])
    return p


def surface_y(x, z):
    t = 1.0 - ((x - C[0]) / RAD[0]) ** 2 - ((z - C[2]) / RAD[2]) ** 2
    y = C[1] - RAD[1] * np.sqrt(np.maximum(t, 0.0))
    y = y - forward_bump(x, z) * front_factor(y)
    return eye_rim(x, y, z)


class Builder:
    def __init__(self):
        self.v = []          # positions
        self.island = []
        self.lip = []        # +1 upper-lip copy / skin above the lip line, -1 lower, 0 on the line
        self.lid = []        # (eye_side, upper?, e_deg, psi_deg, radius) for lid verts else None
        self.weights = []    # {bone: w}
        self.faces = []      # (indices, material)

    def add(self, p, island, w, lip=0, lid=None):
        self.v.append(np.asarray(p, dtype=float))
        self.island.append(island)
        self.lip.append(lip)
        self.lid.append(lid)
        self.weights.append(w)
        return len(self.v) - 1

    def face(self, idx, mat):
        self.faces.append((tuple(int(i) for i in idx), mat))

    def mirror_of(self, start, end, faces_start, faces_end):
        """Mirrors verts [start, end) and faces [faces_start, faces_end) across x = 0 (winding reversed)."""
        remap = {}
        for i in range(start, end):
            p = self.v[i].copy()
            p[0] = -p[0]
            w = {rs.mirror_name(b): x for b, x in self.weights[i].items()}
            lid = self.lid[i]
            if lid is not None:
                lid = (-lid[0],) + lid[1:]
            remap[i] = self.add(p, self.island[i], w, self.lip[i], lid)
        for idx, mat in self.faces[faces_start:faces_end]:
            self.face([remap[i] for i in reversed(idx)], mat)


# ------------------------------------------------------------------------------------------------
# Geometry
# ------------------------------------------------------------------------------------------------

def _rows(n_rows):
    th_m = math.acos((MZ - C[2]) / RAD[2])
    th_e = math.acos((EZ - C[2]) / RAD[2])
    g = np.linspace(0.0, math.pi, 4001)
    dens = 1.0 + 2.5 * np.exp(-((g - th_m) / 0.18) ** 2) + 1.0 * np.exp(-((g - th_e) / 0.15) ** 2)
    cum = np.concatenate([[0.0], np.cumsum((dens[1:] + dens[:-1]) * 0.5 * np.diff(g))])
    cum /= cum[-1]
    u = np.arange(1, n_rows + 1) / (n_rows + 1)
    th = np.interp(u, cum, g)
    jm = int(np.argmin(np.abs(th - th_m)))
    th[jm] = th_m
    return th, jm


def _cols(n_cols, a=0.4):
    s = -1.0 + 2.0 * np.arange(n_cols) / n_cols
    return math.pi * s * (a + (1.0 - a) * s * s)


def jaw_weight(p, lip):
    x, y, z = p
    dz = z - MZ
    if lip == -1 and abs(x) <= MOUTH_W + 1e-9 and abs(dz) < 1e-9:
        return 1.0
    if lip == 1 and abs(dz) < 1e-9:
        return 0.0
    s1 = float(smoothstep(0.004, -0.012, dz))
    s2 = float(smoothstep(0.03, -0.02, y - C[1]))
    w = s1 * s2
    if dz < 0:
        r = math.sqrt((x / (MOUTH_W + 0.004)) ** 2 + (dz / 0.012) ** 2)
        w = max(w, float(np.clip(1.5 - 1.5 * r, 0.0, 1.0)) * s2)
    elif abs(x) < MOUTH_W + 0.004:
        w *= float(smoothstep(0.0, 0.006, dz)) if dz > 0 else 1.0
        w = min(w, 0.15)
    return float(np.clip(w, 0.0, 1.0))


def build_skin(B, n_cols=64, n_rows=48):
    th, jm = _rows(n_rows)
    ph = _cols(n_cols)
    grid = np.zeros((n_rows, n_cols), dtype=int)
    lower_dup = {}
    for j in range(n_rows):
        for k in range(n_cols):
            p = sculpt(ellipsoid_point(th[j], ph[k]))
            if j == jm:
                p[2] = MZ
            dz = p[2] - MZ
            lip = 0 if j == jm else (1 if dz > 0 else -1)
            in_slit = j == jm and abs(p[0]) < MOUTH_W
            if in_slit:
                lip = 1
            wj = jaw_weight(p, lip)
            grid[j, k] = B.add(p, SKIN, _jw(wj), lip)
            if in_slit:
                lower_dup[k] = B.add(p, SKIN, _jw(jaw_weight(p, -1)), -1)
    top = B.add(sculpt(ellipsoid_point(0.0, 0.0)), SKIN, {"head": 1.0}, 1)
    bot = B.add(sculpt(ellipsoid_point(math.pi, 0.0)), SKIN, _jw(0.0), -1)
    lipmat = lambda p: MAT["lips"] if ((p[0] / (MOUTH_W + 0.002)) ** 2 + ((p[2] - MZ) / (0.0075 if p[2] >= MZ else 0.009)) ** 2) < 1.0 else MAT["skin"]
    for j in range(n_rows - 1):
        for k in range(n_cols):
            k2 = (k + 1) % n_cols
            a, b, c, d = grid[j, k], grid[j + 1, k], grid[j + 1, k2], grid[j, k2]
            if j == jm:  # quads below the lip line use the lower copies
                a = lower_dup.get(k, a)
                d = lower_dup.get(k2, d)
            ctr = (B.v[a] + B.v[b] + B.v[c] + B.v[d]) / 4.0
            B.face((a, b, c, d), lipmat(ctr))
    for k in range(n_cols):
        k2 = (k + 1) % n_cols
        B.face((top, grid[0, k], grid[0, k2]), MAT["skin"])
        B.face((bot, grid[n_rows - 1, k2], grid[n_rows - 1, k]), MAT["skin"])
    return grid, jm


def _jw(w):
    w = float(np.clip(w, 0.0, 1.0))
    if w <= 0.01:
        return {"head": 1.0}
    if w >= 0.99:
        return {"jaw": 1.0}
    return {"head": 1.0 - w, "jaw": w}


def _lid_blend(e_deg):
    """0 on the rows near the lid edge (they ride on the eyeball), 1 on the far rows (they lie on the skin)."""
    if e_deg >= 0.0:
        return float(smoothstep(LID_UPPER_E[2], LID_UPPER_E[-1], e_deg))
    return float(smoothstep(-LID_LOWER_E[2], -LID_LOWER_E[-1], -e_deg))


def lid_point(side, e_deg, psi_deg, r):
    """A lid-shell point: on a sphere of radius r round the eye near the edge, blended onto the skin surface
    toward the far rows so the shell meets the face without a ledge (a ledge cast a shadow under the eye)."""
    e, psi = math.radians(e_deg), math.radians(psi_deg)
    c = EYE_C * np.array([side, 1, 1])
    p = c + r * np.array([side * math.sin(psi) * math.cos(e), -math.cos(psi) * math.cos(e), math.sin(e)])
    t = _lid_blend(e_deg)
    if t > 0.0:
        ys = float(surface_y(p[0], p[2])) - 0.0002
        rxz = math.hypot(p[0] - c[0], p[2] - c[2])
        rc = ER + EYE_CLEARANCE * 2
        if rxz < rc:  # never inside the eyeball: stay on or in front of its front surface
            ys = min(ys, c[1] - math.sqrt(rc * rc - rxz * rxz))
        p[1] += t * (ys - p[1])
    return p


def build_eye_and_lids_left(B):
    v0, f0 = len(B.v), len(B.faces)
    # Eyeball: rings by angle from the front pole (pupil < 11 deg < iris < 27 deg).
    rings = [11, 27, 32, 45, 60, 80, 100, 125, 150]
    nb = 16
    front = B.add(EYE_C + np.array([0, -ER, 0]), EYE, {"eye_l": 1.0})
    ring_idx = []
    for a in rings:
        ar = math.radians(a)
        row = []
        for i in range(nb):
            b = 2 * math.pi * i / nb
            d = np.array([math.sin(ar) * math.cos(b), -math.cos(ar), math.sin(ar) * math.sin(b)])
            row.append(B.add(EYE_C + ER * d, EYE, {"eye_l": 1.0}))
        ring_idx.append(row)
    back = B.add(EYE_C + np.array([0, ER, 0]), EYE, {"eye_l": 1.0})
    for i in range(nb):
        i2 = (i + 1) % nb
        B.face((front, ring_idx[0][i2], ring_idx[0][i]), MAT["pupil"])
    for r in range(len(rings) - 1):
        mat = MAT["iris"] if r == 0 else MAT["sclera"]
        for i in range(nb):
            i2 = (i + 1) % nb
            B.face((ring_idx[r][i], ring_idx[r][i2], ring_idx[r + 1][i2], ring_idx[r + 1][i]), mat)
    for i in range(nb):
        i2 = (i + 1) % nb
        B.face((back, ring_idx[-1][i], ring_idx[-1][i2]), MAT["sclera"])
    # Lid shells (skin material, skinned to head; the morphs move them).
    for upper, es, r in ((True, LID_UPPER_E, LID_R_U), (False, LID_LOWER_E, LID_R_L)):
        grid = [[B.add(lid_point(1, e, psi, r), LID_U if upper else LID_L, {"head": 1.0}, 0, (1, upper, e, psi, r))
                 for psi in LID_PSI] for e in es]
        for a in range(len(es) - 1):
            for k in range(len(LID_PSI) - 1):
                q = (grid[a][k], grid[a][k + 1], grid[a + 1][k + 1], grid[a + 1][k])
                B.face(q if upper else tuple(reversed(q)), MAT["skin"])
    # Brow ribbon (dark), floating 1.2 mm above the skin.
    xs = np.linspace(0.010, 0.056, 12)
    rows = []
    for t in (-1.0, 0.0, 1.0):
        row = []
        for x in xs:
            u = (x - 0.006) / 0.05
            zc = EZ + 0.016 + 0.005 * math.sin(math.pi * min(max(u, 0.0), 1.0))
            half = 0.0028 - 0.0012 * u
            z = zc + t * half
            row.append(B.add((x, float(surface_y(x, z)) - 0.0012, z), BROW, {"head": 1.0}, 1))
        rows.append(row)
    for a in range(2):
        for k in range(len(xs) - 1):
            B.face((rows[a + 1][k], rows[a + 1][k + 1], rows[a][k + 1], rows[a][k]), MAT["brow"])
    return v0, len(B.v), f0, len(B.faces)


def build_teeth(B):
    for upper in (True, False):
        r, cy = (0.030, -0.058) if upper else (0.028, -0.058)
        z0, z1 = (MZ - 0.001, MZ + 0.008) if upper else (MZ - 0.010, MZ - 0.0015)
        w = {"head": 1.0} if upper else {"jaw": 1.0}
        isl = TEETH_U if upper else TEETH_L
        cols = []
        for a in np.linspace(-65, 65, 15):
            ar = math.radians(a)
            ring = []
            for rr, z in ((r, z1), (r, z0), (r - 0.004, z0), (r - 0.004, z1)):
                ring.append(B.add((rr * math.sin(ar), cy - rr * math.cos(ar), z), isl, w, 0))
            cols.append(ring)
        for k in range(len(cols) - 1):
            for s in range(4):
                s2 = (s + 1) % 4
                B.face((cols[k][s], cols[k][s2], cols[k + 1][s2], cols[k + 1][s]), MAT["teeth"])
        B.face(tuple(cols[0]), MAT["teeth"])
        B.face(tuple(reversed(cols[-1])), MAT["teeth"])


def build_bag(B):
    c = np.array([0.0, -0.05, MZ - 0.002])
    rad = np.array([0.034, 0.045, 0.026])
    n_a, n_b = 12, 16
    rows = []
    for i in range(n_a + 1):
        a = math.radians(48 + (180 - 48) * i / n_a)  # angle from the -Y pole; front cap removed
        row = []
        for j in range(n_b):
            b = 2 * math.pi * j / n_b
            d = np.array([math.sin(a) * math.cos(b), -math.cos(a), math.sin(a) * math.sin(b)])
            p = c + rad * d
            wj = float(smoothstep(MZ, MZ - 0.008, p[2]))
            row.append(B.add(p, BAG, _jw(wj), 0) if i < n_a else None)
        rows.append(row)
    back = B.add(c + np.array([0, rad[1], 0]), BAG, {"head": 1.0}, 0)
    for i in range(n_a - 1):
        for j in range(n_b):
            j2 = (j + 1) % n_b
            B.face((rows[i][j], rows[i + 1][j], rows[i + 1][j2], rows[i][j2]), MAT["mouth"])  # faces inward
    for j in range(n_b):
        j2 = (j + 1) % n_b
        B.face((rows[n_a - 1][j], back, rows[n_a - 1][j2]), MAT["mouth"])


def build_tongue(B):
    n_u, n_r = 9, 12
    y0, y1, zc = -0.02, -0.076, 1.6285  # tip (y1 - 3 mm) sits behind the lower incisors (inner face y -0.081)
    rings = []
    for i in range(n_u):
        u = i / (n_u - 1)
        prof = math.sin(math.pi * (0.15 + 0.8 * u)) ** 0.5 if u > 0 else 0.6
        y = y0 + (y1 - y0) * u
        if y > -0.04:
            w = {"tongue_01": 1.0}
        elif y > -0.05:
            t = (-0.04 - y) / 0.01
            w = {"tongue_01": 1 - t, "tongue_02": t}
        elif y > -0.06:
            w = {"tongue_02": 1.0}
        elif y > -0.07:
            t = (-0.06 - y) / 0.01
            w = {"tongue_02": 1 - t, "tongue_03": t}
        else:
            w = {"tongue_03": 1.0}
        ring = []
        for j in range(n_r):
            b = 2 * math.pi * j / n_r
            ring.append(B.add((0.016 * prof * math.cos(b), y, zc + 0.003 * prof * math.sin(b)), TONGUE, w, 0))
        rings.append(ring)
    for i in range(n_u - 1):
        for j in range(n_r):
            j2 = (j + 1) % n_r
            B.face((rings[i][j], rings[i][j2], rings[i + 1][j2], rings[i + 1][j]), MAT["tongue"])
    root = B.add((0, y0 + 0.002, zc), TONGUE, {"tongue_01": 1.0}, 0)
    tip = B.add((0, y1 - 0.003, zc), TONGUE, {"tongue_03": 1.0}, 0)
    for j in range(n_r):
        j2 = (j + 1) % n_r
        B.face((root, rings[0][j2], rings[0][j]), MAT["tongue"])
        B.face((tip, rings[-1][j], rings[-1][j2]), MAT["tongue"])


def build():
    B = Builder()
    build_skin(B)
    v0, v1, f0, f1 = build_eye_and_lids_left(B)
    B.mirror_of(v0, v1, f0, f1)
    build_teeth(B)
    build_bag(B)
    build_tongue(B)
    return B


# ------------------------------------------------------------------------------------------------
# Region masks and shapes
# ------------------------------------------------------------------------------------------------

def region_masks(B):
    P = np.array(B.v)
    x, y, z = P[:, 0], P[:, 1], P[:, 2]
    isl = np.array(B.island)
    skinlike = np.isin(isl, [SKIN, BROW, LID_U, LID_L])
    front = y < C[1] - 0.02
    ax = np.abs(x)
    m = {}
    m["brow"] = skinlike & front & (z > EZ + 0.004) & (ax < 0.085) & (z < EZ + 0.06)
    around = ((ax - EX) / 0.03) ** 2 + ((z - EZ) / 0.03) ** 2 < 1.0
    lids = np.isin(isl, [LID_U, LID_L])
    m["eye_l"] = ((skinlike & front & around) | lids) & (x > 0)
    m["eye_r"] = ((skinlike & front & around) | lids) & (x < 0)
    m["eyes"] = m["eye_l"] | m["eye_r"]
    m["cheek"] = (isl == SKIN) & front & (z > MZ - 0.045) & (z < EZ) & (ax > 0.012) & (ax < 0.08)
    m["nose"] = (isl == SKIN) & front & (ax < 0.024) & (z > MZ + 0.006) & (z < EZ + 0.012)
    m["mouth"] = (isl == SKIN) & front & ((x / 0.05) ** 2 + ((z - MZ) / 0.032) ** 2 < 1.0)
    wj = np.array([w.get("jaw", 0.0) for w in B.weights])
    m["jaw"] = (isl == SKIN) & (wj > 0.0) & front
    m["tongue"] = isl == TONGUE
    return m


def lid_remap(e, edge0, edge1, far):
    """Moves a lid row at elevation e so the edge goes edge0 -> edge1, the far row stays put."""
    return far - (far - e) * (far - edge1) / (far - edge0)


def mirror_map(B):
    """Index of each vertex's mirror partner (same island and lip side), by nearest mirrored position."""
    P = np.array(B.v)
    tag = np.array(B.lip) * 16 + np.array(B.island)
    Q = P * np.array([-1.0, 1.0, 1.0])
    mm = np.zeros(len(P), dtype=int)
    for s in range(0, len(P), 512):
        d = np.linalg.norm(Q[s:s + 512, None, :] - P[None, :, :], axis=2)
        d[tag[s:s + 512, None] != tag[None, :]] = 1e9
        mm[s:s + 512] = np.argmin(d, axis=1)
        assert d[np.arange(len(d)), mm[s:s + 512]].max() < 1e-6, "proof head is not mirror-symmetric"
    return mm


def _outward_normal(P):
    n = (P - C) / (RAD ** 2)
    return n / np.linalg.norm(n, axis=1, keepdims=True)


def left_shapes(B):
    """Shape deltas (metres, Blender axes). Left-sided shapes, centre shapes; right ones are mirrored."""
    P = np.array(B.v)
    N = len(P)
    x, y, z = P[:, 0], P[:, 1], P[:, 2]
    dz = z - MZ
    isl = np.array(B.island)
    lip = np.array(B.lip)
    skin = (isl == SKIN) | (isl == BROW)
    S = {}
    zero = lambda: np.zeros((N, 3))
    sideL = smoothstep(-0.006, 0.006, x)
    upper = lip == 1
    lower = lip == -1
    nrm = _outward_normal(P)

    def vec(dxv, dyv, dzv, w):
        d = zero()
        d[:, 0], d[:, 1], d[:, 2] = dxv * w, dyv * w, dzv * w
        return d

    def onskin(d):
        d[~skin] = 0.0
        return d

    zb = EZ + 0.02
    above_eye = smoothstep(EZ + 0.004, EZ + 0.012, z)
    # --- brows
    w = gauss(x, z, 0.032, zb, 0.026, 0.02) * smoothstep(0.0, 0.01, x) * above_eye
    inner = gauss(x, z, 0.015, zb, 0.012, 0.02)
    S["browDownLeft"] = onskin(vec(0.0, -2.5 * MM, -10.0 * MM, w)
                               + vec(-5.0 * MM, -1.5 * MM, -2.0 * MM, inner * smoothstep(0.0, 0.01, x) * above_eye))
    w = (gauss(x, z, 0.014, zb, 0.012, 0.02) + gauss(x, z, -0.014, zb, 0.012, 0.02)) * above_eye
    S["browInnerUp"] = onskin(vec(-np.sign(x) * 2.0 * MM, -0.5 * MM, 13.0 * MM, w))
    w = gauss(x, z, 0.046, zb, 0.014, 0.02) * smoothstep(0.0, 0.01, x) * above_eye
    S["browOuterUpLeft"] = onskin(vec(0.0, 0.0, 12.0 * MM, w))
    # --- cheeks
    w = (gauss(x, z, 0.046, MZ + 0.006, 0.02, 0.022) + gauss(x, z, -0.046, MZ + 0.006, 0.02, 0.022))
    d = nrm * (6.0 * MM) * w[:, None]
    S["cheekPuff"] = onskin(d)
    w = gauss(x, z, 0.038, EZ - 0.026, 0.020, 0.016) * smoothstep(0.0, 0.01, x)
    S["cheekSquintLeft"] = onskin(vec(0.0, -3.5 * MM, 7.0 * MM, w))
    # --- eyes: lids recomputed from their (e, psi) parameters
    S.update(_lid_shapes(B, P, skin, x, z))
    S["cheekSquintLeft"] = S["cheekSquintLeft"] + S.pop("_cheekSquintLidLeft")
    # --- jaw
    wj = np.array([w_.get("jaw", 0.0) for w_ in B.weights]) * (isl == SKIN)
    chin_pad = gauss(x, z, 0.0, MZ - 0.03, 0.022, 0.014) * (isl == SKIN)  # the chin pad leads a jaw thrust
    S["jawForward"] = vec(0.0, -5.0 * MM, 0.0, wj) + vec(0.0, -2.0 * MM, 1.0 * MM, chin_pad * wj)
    S["jawLeft"] = vec(5.0 * MM, 0.0, 0.0, wj)
    corner = gauss(np.abs(x), z, MOUTH_W, MZ, 0.008, 0.008)
    lowlip = lower * np.exp(-(x / 0.028) ** 2) * np.exp(-(dz / 0.01) ** 2)
    S["jawOpen"] = onskin(vec(-np.sign(x) * 1.5 * MM, 0.0, -1.5 * MM, corner) + vec(0.0, -1.0 * MM, 0.0, lowlip))
    # --- mouth
    gx = np.exp(-(x / 0.03) ** 4)
    band = np.exp(-(dz / 0.012) ** 2)
    S["mouthClose"] = onskin(vec(0.0, 1.5 * MM, 0.0, gx * np.exp(-(dz / 0.008) ** 2))
                             + vec(0.0, 0.0, -dz * 0.35, gx * band))
    env = np.exp(-(x / 0.03) ** 2 - (dz / 0.014) ** 2)
    xin = np.clip(x / MOUTH_W, -1.2, 1.2)
    open_ = np.clip(1.0 - (x / MOUTH_W) ** 2, 0.0, 1.0) * np.exp(-(dz / 0.01) ** 2)
    vside = np.where(upper, 1.0, np.where(lower, -1.0, 0.0))
    S["mouthFunnel"] = onskin(vec(-xin * 5.0 * MM, -5.0 * MM, 0.0, env) + vec(0.0, 0.0, 2.5 * MM * vside, open_))
    S["mouthPucker"] = onskin(vec(-xin * 8.0 * MM, -7.0 * MM, 0.0, env) + vec(0.0, 0.0, -dz * 0.2, env))
    wide = gauss(x, z, 0.0, MZ, 0.035, 0.02)
    S["mouthLeft"] = onskin(vec(6.0 * MM, 0.0, 0.0, wide))
    cornL = gauss(x, z, MOUTH_W, MZ, 0.011, 0.012)
    # corner back-up-out plus the nasolabial bulge the corner pushes into the cheek (the "smile" read)
    S["mouthSmileLeft"] = onskin(vec(5.5 * MM, 3.0 * MM, 11.0 * MM, cornL)
                                 + vec(1.0 * MM, -3.0 * MM, 3.0 * MM, gauss(x, z, 0.036, MZ + 0.014, 0.011, 0.012)))
    S["mouthFrownLeft"] = onskin(vec(1.0 * MM, 0.5 * MM, -10.0 * MM, gauss(x, z, MOUTH_W, MZ, 0.010, 0.012)))
    S["mouthDimpleLeft"] = onskin(vec(1.5 * MM, 3.0 * MM, 0.0, gauss(x, z, MOUTH_W, MZ, 0.012, 0.012)))
    S["mouthStretchLeft"] = onskin(vec(5.0 * MM, 1.0 * MM, -2.0 * MM, gauss(x, z, MOUTH_W, MZ - 0.002, 0.014, 0.014))
                                   + vec(0.0, 0.0, -1.0 * MM, lower * sideL * np.exp(-(x / 0.026) ** 2) * band))
    S["mouthPressLeft"] = onskin(vec(0.0, 1.0 * MM, 0.0, gx * sideL * np.exp(-(dz / 0.008) ** 2))
                                 + vec(0.0, 0.0, -dz * 0.4, gx * sideL * band))
    lipzone = np.exp(-(x / 0.026) ** 2) * np.exp(-(dz / 0.012) ** 2)
    S["mouthLowerDownLeft"] = onskin(vec(0.0, -0.5 * MM, -4.0 * MM, lower * sideL * lipzone))
    S["mouthUpperUpLeft"] = onskin(vec(0.0, -0.5 * MM, 4.0 * MM, upper * sideL * lipzone))
    roll = np.exp(-(x / 0.028) ** 2) * np.exp(-(dz / 0.008) ** 2)
    S["mouthRollLower"] = onskin(vec(0.0, 4.0 * MM, 0.0, lower * roll))
    S["mouthRollUpper"] = onskin(vec(0.0, 4.0 * MM, 0.0, upper * roll))
    chin = gauss(x, z, 0.0, MZ - 0.018, 0.02, 0.012) * smoothstep(0.0, -0.006, dz)
    S["mouthShrugLower"] = onskin(vec(0.0, 0.0, 5.0 * MM, chin) + vec(0.0, -2.5 * MM, 0.0, lower * lipzone))
    S["mouthShrugUpper"] = onskin(vec(0.0, -2.5 * MM, 1.5 * MM, upper * lipzone))
    S["noseSneerLeft"] = onskin(vec(-0.5 * MM, -1.0 * MM, 5.0 * MM, gauss(x, z, 0.012, 1.662, 0.009, 0.013))
                                + vec(0.0, 0.0, 2.0 * MM, upper * sideL * gauss(x, z, 0.01, MZ + 0.006, 0.01, 0.006)))
    tw = np.clip((-y - 0.02) / 0.062, 0.0, 1.0) * (isl == TONGUE)
    S["tongueOut"] = vec(0.0, -18.0 * MM, -2.0 * MM, tw)
    corn = gauss(np.abs(x), z, MOUTH_W, MZ, 0.011, 0.012)
    S["mouthSmileNoCheek"] = onskin(vec(np.sign(x) * 5.5 * MM, 3.0 * MM, 11.0 * MM, corn))
    S["lipsTogetherSoft"] = onskin(vec(0.0, -0.8 * MM, 0.0, gx * np.exp(-(dz / 0.008) ** 2))
                                   + vec(0.0, 0.0, -dz * 0.2, gx * band))
    S["jawOpen_x_mouthSmile"] = onskin(vec(-np.sign(x) * 2.0 * MM, 0.0, -2.0 * MM, corn))
    return S


def _lid_shapes(B, P, skin, x, z):
    N = len(P)
    S = {}
    lids = [(i, l) for i, l in enumerate(B.lid) if l is not None and l[0] == 1]

    def lidshape(up_edge, lo_edge, dpsi=0.0):
        d = np.zeros((N, 3))
        for i, (side, upper, e, psi, r) in lids:
            if upper:
                e2 = lid_remap(e, LID_UPPER_E[0], up_edge, LID_UPPER_E[-1])
            else:
                e2 = lid_remap(e, LID_LOWER_E[0], lo_edge, LID_LOWER_E[-1])
            d[i] = lid_point(side, e2, psi + dpsi, r) - P[i]
        return d

    def skinpatch(dzv, x0, z0, sx, sz):
        d = np.zeros((N, 3))
        d[:, 2] = dzv * gauss(x, z, x0, z0, sx, sz) * skin * (x > 0)
        return d

    S["eyeBlinkLeft"] = lidshape(-9.0, -6.0) + skinpatch(-1.0 * MM, EX, EZ + 0.014, 0.014, 0.006)
    S["eyeLookDownLeft"] = lidshape(3.0, -30.0)
    S["eyeLookUpLeft"] = lidshape(34.0, -14.0) + skinpatch(1.0 * MM, EX, EZ + 0.016, 0.014, 0.006)
    S["eyeLookInLeft"] = lidshape(20.0, -22.0, -8.0)
    S["eyeLookOutLeft"] = lidshape(20.0, -22.0, 8.0)
    S["eyeSquintLeft"] = lidshape(14.0, -3.0) + skinpatch(3.0 * MM, EX, EZ - 0.016, 0.014, 0.008)
    S["eyeWideLeft"] = lidshape(44.0, -30.0) + skinpatch(3.0 * MM, EX, EZ + 0.016, 0.016, 0.008)
    S["_cheekSquintLidLeft"] = lidshape(20.0, -12.0)
    t = lidshape(20.0, -14.0)
    # eyeLidTension is centre (both eyes): add the mirrored copy below in all_shapes
    S["_eyeLidTensionLeft"] = t
    return S


SIDED_SHAPES = [n for n in fs.ALL_SHAPES if n.endswith("Left")]
EYE_CLEARANCE = 0.0002  # skin may slide over the eyeball but stays this far outside it (F12 lids_eyes)


def _keep_off_eyes(B, shapes):
    """Skin, brow and lid vertices that start outside an eyeball and a shape would push into it are slid
    back out along the eyeball's radius (brows lowering over the lid fold, cheeks squinting up)."""
    P = np.array(B.v)
    isl = np.array(B.island)
    soft = (isl != EYE) & (isl != TEETH_U) & (isl != TEETH_L) & (isl != TONGUE) & (isl != BAG)
    for side in (1.0, -1.0):
        c = EYE_C * np.array([side, 1.0, 1.0])
        r0 = np.linalg.norm(P - c, axis=1)
        near = soft & (r0 >= ER - 1e-4) & (r0 < ER + 0.008)
        for d in shapes.values():
            q = P[near] + d[near]
            v = q - c
            r = np.linalg.norm(v, axis=1)
            target = np.minimum(r0[near], ER + EYE_CLEARANCE)  # never further out than it started
            inside = r < target - 1e-9
            if inside.any():
                fix = c + v[inside] / r[inside, None] * target[inside, None]
                idx = np.nonzero(near)[0][inside]
                d[idx] = fix - P[idx]


def all_shapes(B):
    """{name: (N, 3) delta} for every name in fs.ALL_SHAPES, masked to the allowed regions."""
    L = left_shapes(B)
    mm = mirror_map(B)
    M = np.array([-1.0, 1.0, 1.0])
    out = {}
    for name in fs.ALL_SHAPES:
        if name.endswith("Left"):
            out[name] = L[name]
        elif name.endswith("Right"):
            out[name] = L[name[:-5] + "Left"][mm] * M
        elif name == "eyeLidTension":
            t = L["_eyeLidTensionLeft"]
            out[name] = t + t[mm] * M
        else:
            out[name] = L[name]
    _keep_off_eyes(B, out)
    masks = region_masks(B)
    for name, d in out.items():
        allowed = np.zeros(len(d), dtype=bool)
        for m in fs.MASK_ALLOW[fs.SHAPE_INFO[name]["mask"]]:
            allowed |= masks[m]
        d[~allowed] = 0.0
        d[np.linalg.norm(d, axis=1) < 2e-5] = 0.0  # cut gaussian tails (keeps the sparse lists sparse)
    return out, masks
