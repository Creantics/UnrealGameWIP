"""Fits a base body mesh to a concept sheet's front and side silhouettes, row by row.

    blender -b base.blend --factory-startup --python-exit-code 1 --python tools/blender/fit_to_views.py -- \\
        MASKS_DIR --object NAME --out fit.blend [--height 1.88] [--iters 5] [--sigma 8] [--sigma-side 5] [--preview DIR]

MASKS_DIR holds hull_from_views.py's front.png and side.png (figure cropped to its box, side view facing right).
The mesh is scaled to --height with its feet at z=0, then each pass:
  1. warps z piecewise-linearly so crotch, armpit and clavicle heights match the sheet, with the arms' own warp
     (armpit to fingertip) blended in on the arm vertices;
  2. per pixel row, maps the mesh's front x-runs onto the sheet's (each sheet run paired with the base run it
     overlaps most; unpaired base runs move with their nearest pair) and the side extent onto the sheet's in y;
  3. smooths those maps along z (Gaussian, --sigma rows for x, --sigma-side for y) and moves the vertices by them.
Hands (below the wrist), feet (below the ankle) and head with neck (above the clavicle) move rigidly, ramping into
the field over BLEND metres, and the rows they share with the legs or shoulders are interpolated instead of mapped: their
shape comes from the bundle and the head panel, not from this sheet. Only the base cage (mesh.vertices) moves,
so multires detail rides along. Child objects (eyes) move with their nearest vertices. Uses numpy only, since Blender's Python has no scipy.
"""
import os
import sys

import bpy
import numpy as np
from mathutils import Matrix, Vector


BLEND = 0.05   # metres above a hand or foot cut over which rigid motion ramps into the field


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:]
    a = {"masks": argv[0], "object": None, "out": None, "height": "1.88", "iters": "5", "sigma": "8", "sigma-side": "5",
         "preview": None}
    i = 1
    while i < len(argv):
        k = argv[i][2:]
        if k not in a:
            raise SystemExit("fit_to_views: unknown argument %s" % argv[i])
        a[k] = argv[i + 1]
        i += 2
    if not a["object"] or not a["out"]:
        raise SystemExit("fit_to_views: --object and --out are required")
    return a


def load_mask(path):
    img = bpy.data.images.load(path)
    w, h = img.size
    px = np.array(img.pixels[:], dtype=np.float32).reshape(h, w, 4)[::-1]
    bpy.data.images.remove(img)
    return px[..., :3].mean(axis=2) > 0.5


def render(obj, look, right, mpp, w, h, centre):
    """Orthographic alpha mask of obj: w x h px at mpp metres per px, centred on the world point centre."""
    sc = bpy.context.scene
    sc.render.resolution_x, sc.render.resolution_y = w, h
    cam = bpy.data.objects.new("fit_cam", bpy.data.cameras.new("fit_cam"))
    cam.data.type = "ORTHO"
    cam.data.sensor_fit = "HORIZONTAL"
    cam.data.ortho_scale = w * mpp
    cam.data.clip_end = 40.0
    sc.collection.objects.link(cam)
    sc.camera = cam
    cam.location = Vector(centre) - look * 10.0
    cam.rotation_euler = look.to_track_quat("-Z", "Y").to_euler()
    path = os.path.join(bpy.app.tempdir, "fit_render.png")
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)
    bpy.data.objects.remove(cam, do_unlink=True)
    img = bpy.data.images.load(path)
    px = np.array(img.pixels[:], dtype=np.float32).reshape(h, w, 4)[::-1]
    bpy.data.images.remove(img)
    return px[..., 3] > 0.5


def runs(row):
    """(start, end) column pairs of the True runs in a boolean row, end inclusive."""
    d = np.diff(np.concatenate(([0], row.astype(np.int8), [0])))
    return list(zip(np.flatnonzero(d == 1), np.flatnonzero(d == -1) - 1))


def gauss_rows(a, sigma):
    """Gaussian blur along axis 0 with edge padding."""
    if sigma <= 0:
        return a
    r = int(3 * sigma)
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    p = np.concatenate([np.repeat(a[:1], r, axis=0), a, np.repeat(a[-1:], r, axis=0)])
    out = np.zeros_like(a)
    for i, kv in enumerate(k):
        out += kv * p[i:i + a.shape[0]]
    return out


def match_runs(rb, rd):
    """Pairs each sheet run with the base run it overlaps most (nearest centre when none overlap), one to one,
    keeping the pairs in order on both sides. Returns (pairs, unmatched base runs)."""
    def score(b, t):
        ov = min(b[1], t[1]) - max(b[0], t[0]) + 1
        return ov if ov > 0 else -abs(0.5 * (b[0] + b[1]) - 0.5 * (t[0] + t[1])) / 1000.0
    cand = sorted(((score(b, t), i, j) for i, b in enumerate(rb) for j, t in enumerate(rd)), reverse=True)
    used_b, used_t, pairs = set(), set(), []
    for _, i, j in cand:
        if i in used_b or j in used_t:
            continue
        # keep the pairing order-preserving
        if any((i < pi) != (j < pj) for pi, pj in pairs):
            continue
        pairs.append((i, j))
        used_b.add(i)
        used_t.add(j)
    pairs.sort()
    return [(rb[i], rd[j]) for i, j in pairs], [rb[i] for i in range(len(rb)) if i not in used_b]


def ankle_row(m):
    """Row of the narrowest pair of leg runs between 14 % and 5 % up a front mask (the rounded soles and toe
    tips below that are narrower still)."""
    h = m.shape[0]
    best = None
    for v in range(int(0.86 * h), int(0.95 * h)):
        rs = runs(m[v])
        if len(rs) == 2:
            width = sum(r[1] - r[0] for r in rs)
            if best is None or width <= best[0]:
                best = (width, v)
    return None if best is None else best[1]


def landmarks(m):
    """Row indices (top row 0) of crotch, armpit, neck, clavicle and fingertip in a front mask, or None where not
    found. The neck is the narrowest row under the head, which on a thick neck is where the jaw meets it; the
    clavicle is the first row below that where the outline has widened to 1.5 necks (the base of the neck)."""
    h, w = m.shape
    cols = np.flatnonzero(m.any(axis=0))
    c = (cols[0] + cols[-1]) // 2
    counts = [len(runs(m[v])) for v in range(h)]
    crotch = None
    for v in range(int(0.95 * h), int(0.3 * h), -1):
        if m[v, c - 1:c + 2].all():
            crotch = v
            break
    neck, best = None, None
    for v in range(int(0.06 * h), int(0.25 * h)):
        rs = [r for r in runs(m[v]) if r[0] <= c <= r[1]]
        if rs and (best is None or rs[0][1] - rs[0][0] < best):
            neck, best = v, rs[0][1] - rs[0][0]
    clavicle = None
    if neck is not None:
        for v in range(neck, int(0.3 * h)):
            rs = [r for r in runs(m[v]) if r[0] <= c <= r[1]]
            if rs and rs[0][1] - rs[0][0] >= 1.5 * best:
                clavicle = v
                break
    armpit = None
    for v in range(neck or int(0.15 * h), crotch or int(0.5 * h)):
        if counts[v] >= 3 and counts[v + 1] >= 3:
            armpit = v
            break
    # fingertips may hang above or below the crotch; the lowest row with an arm run beside torso or legs
    finger = None
    for v in range(h - 1, armpit or int(0.3 * h), -1):
        if counts[v] >= 3 and counts[v - 1] >= 3:
            finger = v
            break
    return {"crotch": crotch, "armpit": armpit, "neck": neck, "clavicle": clavicle, "finger": finger}


class Fitter:
    def __init__(self, obj, masks, height, sigma, sigma_side):
        self.obj, self.height, self.sigma, self.sigma_side = obj, height, sigma, sigma_side
        self.front = load_mask(os.path.join(masks, "front.png"))
        self.side = load_mask(os.path.join(masks, "side.png"))
        self.h = self.front.shape[0]
        self.mpp = height / self.h            # front mask rows span the full height
        self.mpp_s = height / self.side.shape[0]
        self.pad = 60
        self.w = self.front.shape[1] + 2 * self.pad
        self.ws = self.side.shape[1] + 2 * self.pad
        self.sheet_f = self._centred(self.front, self.w)
        self.sheet_s = self._resample_rows(self._centred(self.side, self.ws), self.h)
        ed = np.empty(len(obj.data.edges) * 2, dtype=np.int64)
        obj.data.edges.foreach_get("vertices", ed)
        self.edges = (ed[0::2], ed[1::2])

    @staticmethod
    def _centred(m, w):
        out = np.zeros((m.shape[0], w), dtype=bool)
        cols = np.flatnonzero(m.any(axis=0))
        off = w // 2 - (cols[0] + cols[-1]) // 2
        out[:, off:off + m.shape[1]] = m
        return out

    @staticmethod
    def _resample_rows(m, h):
        idx = np.clip(((np.arange(h) + 0.5) * m.shape[0] / h).astype(int), 0, m.shape[0] - 1)
        return m[idx]

    # world <-> pixel: row v is z = height - (v + 0.5) * mpp; front col u is x, side col u is -y
    def z_of(self, v):
        return self.height - (np.asarray(v) + 0.5) * self.mpp

    def v_of(self, z):
        return (self.height - np.asarray(z)) / self.mpp - 0.5

    def world(self):
        o = self.obj
        co = np.empty(len(o.data.vertices) * 3)
        o.data.vertices.foreach_get("co", co)
        co = co.reshape(-1, 3)
        M = np.array(o.matrix_world)
        return co @ M[:3, :3].T + M[:3, 3]

    def wrists(self, mask):
        """Per arm (0 left, -1 right): (wrist row, arm run, neighbouring run), or None. The wrist is the
        narrowest arm row 12-26 cm above the fingertip (lower down, the fingers are narrower still)."""
        lm = landmarks(mask)
        out = {0: None, -1: None}
        if lm["armpit"] is None or lm["finger"] is None:
            return lm, out
        lo = max(lm["armpit"], lm["finger"] - int(0.26 / self.mpp))
        hi = lm["finger"] - int(0.12 / self.mpp)
        for side in (0, -1):
            best = None
            for v in range(lo, hi + 1):
                rs = runs(mask[v])
                if len(rs) < 3:
                    continue
                r = rs[side]
                if best is None or r[1] - r[0] <= best[0]:
                    best = (r[1] - r[0], v, r, rs[1] if side == 0 else rs[-2])
            if best is not None:
                out[side] = best[1:]
        return lm, out

    def rigid_groups(self, P):
        """Parts that move as one, keeping the bundle's shape and size: hands below the wrist, feet below the
        ankle, the head above the neck. Each is (indices, rigid weights, cut centroid)."""
        mesh = self.render_front()
        lm, wr = self.wrists(mesh)
        groups = []
        for side in (0, -1):
            if wr[side] is None:
                continue
            v, r, nxt = wr[side]
            zc = float(self.z_of(v))
            # split between the hand and its neighbour (thigh) at the middle of the gap
            xs = (0.5 * (r[1] + 1 + nxt[0]) if side == 0 else 0.5 * (nxt[1] + 1 + r[0])) - self.w / 2
            xs *= self.mpp
            seed = (np.abs(P[:, 2] - zc) < 0.01) & ((P[:, 0] < xs) if side == 0 else (P[:, 0] > xs))
            g = self._group(P, seed, zc)
            if g is not None:
                groups.append(g)
        ank = ankle_row(mesh)
        if ank is not None:
            zc = float(self.z_of(ank))
            for sgn in (-1, 1):
                g = self._group(P, (np.abs(P[:, 2] - zc) < 0.01) & (np.sign(P[:, 0]) == sgn), zc)
                if g is not None:
                    groups.append(g)
        if lm["clavicle"] is not None:
            # head and neck are fitted on their own (to the head panel); here they only ride on the shoulders
            zc = float(self.z_of(lm["clavicle"]))
            g = self._group(P, (np.abs(P[:, 2] - zc) < 0.01) & (np.abs(P[:, 0]) < 0.15), zc, up=True)
            if g is not None:
                groups.append(g)
        return groups

    def _group(self, P, seed, zc, up=False):
        """Everything connected to the cut ring below zc moves rigidly; the next BLEND metres above it ramp
        from rigid to the field, so the cut leaves no step. Returns (indices, rigid weights, ring centroid)."""
        if not seed.any():
            return None
        allowed = P[:, 2] > zc - BLEND if up else P[:, 2] < zc + BLEND
        cur = seed & allowed
        e0, e1 = self.edges
        while True:
            nb = np.zeros_like(cur)
            nb[e1[cur[e0]]] = True
            nb[e0[cur[e1]]] = True
            nxt = cur | (nb & allowed)
            if nxt.sum() == cur.sum():
                break
            cur = nxt
        idx = np.flatnonzero(cur)
        wgt = np.clip(((P[idx, 2] - zc) if up else (zc - P[idx, 2])) / BLEND + 1.0, 0.0, 1.0)
        return idx, wgt, P[seed].mean(0)

    def set_world(self, P, moved, rigid=()):
        o = self.obj
        Q = moved(P)
        for idx, wgt, c in rigid:
            r = P[idx] + (moved(c[None])[0] - c)
            Q[idx] = wgt[:, None] * r + (1.0 - wgt[:, None]) * Q[idx]
        D = Q - P
        P = Q
        Mi = np.array(o.matrix_world.inverted())
        o.data.vertices.foreach_set("co", (P @ Mi[:3, :3].T + Mi[:3, 3]).ravel())
        o.data.update()
        for ch in o.children:
            # children (eyes) move with the mean motion of the nearest vertices
            c = np.array(ch.matrix_world.translation)
            near = np.argsort(((P - D - c) ** 2).sum(1))[:16]
            d = D[near].mean(0)
            ch.matrix_world = Matrix.Translation(Vector(d)) @ ch.matrix_world
        bpy.context.view_layer.update()

    def normalise(self):
        """Uniform scale to the target height, feet at z=0, centred on x=y=0 (on the evaluated mesh)."""
        dg = bpy.context.evaluated_depsgraph_get()
        ev = self.obj.evaluated_get(dg)
        pts = np.array([ev.matrix_world @ Vector(c) for c in ev.bound_box])
        lo, hi = pts.min(0), pts.max(0)
        s = self.height / (hi[2] - lo[2])
        T = Matrix.Translation(Vector((-0.5 * (lo[0] + hi[0]) * s, -0.5 * (lo[1] + hi[1]) * s, -lo[2] * s)))
        self.obj.matrix_world = T @ Matrix.Scale(s, 4) @ self.obj.matrix_world
        bpy.context.view_layer.update()
        print("fit_to_views: scaled by %.4f to %.3f m" % (s, self.height))

    def render_front(self):
        return render(self.obj, Vector((0, 1, 0)), Vector((1, 0, 0)), self.mpp, self.w, self.h,
                      (0, 0, self.height / 2))

    def render_side(self):
        return render(self.obj, Vector((1, 0, 0)), Vector((0, -1, 0)), self.mpp, self.ws, self.h,
                      (0, 0, self.height / 2))

    def z_pass(self):
        mesh = self.render_front()
        lb, ld = landmarks(mesh), landmarks(self.sheet_f)
        print("fit_to_views: landmarks mesh %s sheet %s" % (lb, ld))
        keys = [k for k in ("crotch", "armpit", "clavicle") if lb[k] is not None and ld[k] is not None]
        zb = np.array([0.0] + [self.z_of(lb[k]) for k in keys] + [self.height])
        zd = np.array([0.0] + [self.z_of(ld[k]) for k in keys] + [self.height])
        order = np.argsort(zb)
        zb, zd = zb[order], zd[order]
        if np.any(np.diff(zd) <= 0):
            raise SystemExit("fit_to_views: landmark heights out of order on the sheet")
        arm = None
        if None not in (lb["armpit"], ld["armpit"], lb["finger"], ld["finger"]):
            ab, fb = self.z_of(lb["armpit"]), self.z_of(lb["finger"])
            ad, fd = self.z_of(ld["armpit"]), self.z_of(ld["finger"])
            arm = (ab, ad, (ad - fd) / (ab - fb))
            inner = self._arm_inner(mesh, lb)

        def moved(P):
            P = P.copy()
            zbody = np.interp(P[:, 2], zb, zd)
            if arm is not None:
                ab, ad, k = arm
                zarm = ad + (P[:, 2] - ab) * k
                wgt = self._arm_weight(P, inner, ab)
                P[:, 2] = zbody + wgt * (zarm - zbody)
            else:
                P[:, 2] = zbody
            return P
        P = self.world()
        self.set_world(P, moved, self.rigid_groups(P))

    def _arm_inner(self, mesh, lm):
        """Per row, the x of each arm run's inner edge (left, right), NaN where the arm is not separate."""
        left = np.full(self.h, np.nan)
        right = np.full(self.h, np.nan)
        for v in range(lm["armpit"], lm["finger"] + 1):
            rs = runs(mesh[v])
            if len(rs) >= 3:
                left[v] = (rs[0][1] + 1 - self.w / 2) * self.mpp
                right[v] = (rs[-1][0] - self.w / 2) * self.mpp
        return left, right

    def _arm_weight(self, P, inner, z_armpit):
        left, right = inner
        v = np.clip(np.round(self.v_of(P[:, 2])).astype(int), 0, self.h - 1)
        out = (P[:, 0] < left[v] - 0.004) | (P[:, 0] > right[v] + 0.004)
        out &= ~np.isnan(left[v])
        # fade to the body warp over 4 cm below the armpit, where both warps agree anyway
        fade = np.clip((z_armpit - P[:, 2]) / 0.04, 0.0, 1.0)
        return out * fade

    def xy_pass(self):
        mesh_f, mesh_s = self.render_front(), self.render_side()
        gx = (np.arange(self.w) + 0.5 - self.w / 2) * self.mpp
        gy = -(np.arange(self.ws) + 0.5 - self.ws / 2) * self.mpp
        # rows where hands or feet share the outline with the legs say nothing reliable about the legs:
        # the field there is interpolated from the clean rows above and below
        skip = np.zeros(self.h, dtype=bool)
        hand = int(0.20 / self.mpp)
        for m in (mesh_f, self.sheet_f):
            lm = landmarks(m)
            if lm["finger"] is not None:
                skip[max(0, lm["finger"] - hand):lm["finger"] + 4] = True
        ank = [r for r in (ankle_row(mesh_f), ankle_row(self.sheet_f)) if r is not None]
        if ank:
            skip[max(0, min(ank) - int(0.06 / self.mpp)):] = True
        necks = [landmarks(m)["clavicle"] for m in (mesh_f, self.sheet_f)]
        if None not in necks:
            skip[:max(necks)] = True
        dx = self._row_maps(mesh_f, self.sheet_f, gx, by_run=True, skip=skip, sigma=self.sigma)
        dy = self._row_maps(mesh_s, self.sheet_s, gy, by_run=False, skip=skip, sigma=self.sigma_side)

        def moved(P):
            P = P.copy()
            v = self.v_of(P[:, 2])
            P[:, 0] += self._sample(dx, v, (P[:, 0] / self.mpp) + self.w / 2 - 0.5)
            P[:, 1] += self._sample(dy, v, (-P[:, 1] / self.mpp) + self.ws / 2 - 0.5)
            return P
        P = self.world()
        self.set_world(P, moved, self.rigid_groups(P))
        present = mesh_f[:, :]
        step = np.abs(np.diff(dx, axis=0))[present[1:] & present[:-1]]
        print("fit_to_views: max row-to-row dx change %.2f mm" % (1000 * step.max() if step.size else 0.0))

    def _row_maps(self, mesh, sheet, g, by_run, skip, sigma):
        """Displacement field (rows x cols) taking the mesh's runs onto the sheet's in each row."""
        h, w = mesh.shape
        sign = 1.0 if g[1] > g[0] else -1.0
        d = np.full((h, w), np.nan)
        for v in range(h):
            if skip[v]:
                continue
            rb, rd = runs(mesh[v]), runs(sheet[v])
            if not rb or not rd:
                continue
            if not by_run:
                rb = [(rb[0][0], rb[-1][1])]
                rd = [(rd[0][0], rd[-1][1])]
            pairs, unmatched = match_runs(rb, rd)
            if not pairs:
                continue
            src = np.array([c for b, _ in pairs for c in (b[0], b[1] + 1)], dtype=float)
            dst = np.array([c for _, t in pairs for c in (t[0], t[1] + 1)], dtype=float)
            cols = np.arange(w) + 0.5
            # constant displacement beyond the outer edges
            mapped = np.interp(cols, src, dst)
            mapped[cols < src[0]] = cols[cols < src[0]] + dst[0] - src[0]
            mapped[cols > src[-1]] = cols[cols > src[-1]] + dst[-1] - src[-1]
            # a base run with no partner on the sheet moves rigidly with its nearest matched neighbour
            for b in unmatched:
                mid = 0.5 * (b[0] + b[1] + 1)
                k = int(np.argmin(np.abs(src - mid)))
                span = (cols >= b[0]) & (cols <= b[1] + 1)
                mapped[span] = cols[span] + dst[k] - src[k]
            d[v] = sign * (mapped - cols) * self.mpp
        # rows without a map are interpolated along z (held constant past the ends)
        ok = np.flatnonzero(~np.isnan(d[:, 0]))
        if len(ok) == 0:
            return np.zeros((h, w))
        rows = np.arange(h)
        d = np.stack([np.interp(rows, ok, d[ok, u]) for u in range(w)], axis=1)
        return gauss_rows(gauss_rows(d, sigma).T, 2.0).T

    @staticmethod
    def _sample(field, v, u):
        h, w = field.shape
        v = np.clip(v, 0, h - 1.001)
        u = np.clip(u, 0, w - 1.001)
        v0, u0 = v.astype(int), u.astype(int)
        fv, fu = v - v0, u - u0
        return ((1 - fv) * ((1 - fu) * field[v0, u0] + fu * field[v0, u0 + 1])
                + fv * ((1 - fu) * field[v0 + 1, u0] + fu * field[v0 + 1, u0 + 1]))

    def iou(self):
        f = self.render_front()
        s = self.render_side()
        return (float((f & self.sheet_f).sum()) / max(1, float((f | self.sheet_f).sum())),
                float((s & self.sheet_s).sum()) / max(1, float((s | self.sheet_s).sum())))


def main():
    a = _args()
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_WORKBENCH"
    sc.render.film_transparent = True
    sc.display.shading.light = "FLAT"
    obj = bpy.data.objects[a["object"]]
    keep = {obj} | set(obj.children_recursive)
    for o in sc.objects:
        o.hide_render = o not in keep
    fit = Fitter(obj, a["masks"], float(a["height"]), float(a["sigma"]), float(a["sigma-side"]))
    fit.normalise()
    print("fit_to_views: start IoU front %.4f side %.4f" % fit.iou())
    for i in range(int(a["iters"])):
        fit.z_pass()
        fit.xy_pass()
        print("fit_to_views: pass %d IoU front %.4f side %.4f" % ((i + 1,) + fit.iou()))
    if a["preview"]:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from base_mesh_pick import render as preview
        os.makedirs(a["preview"], exist_ok=True)
        sc.render.film_transparent = False
        sc.display.shading.light = "STUDIO"
        preview(obj, a["preview"])
    bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(a["out"]))
    print("fit_to_views: saved %s" % a["out"])


if __name__ == "__main__":
    main()
