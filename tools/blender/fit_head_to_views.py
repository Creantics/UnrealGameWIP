"""Fits the head of a fitted body to a head panel: proportions from the panel, cross-sections from the base.

    blender -b fit.blend --python-exit-code 1 --python tools/blender/fit_head_to_views.py -- \\
        HEAD_MASKS --object NAME --out head.blend --mpp 0.00312,0.00319 --cut 1.598 --chin 0.254 \\
        [--top 1.88] [--iters 2] [--sigma 4] [--ramp 0.04] [--turn 0] [--min 0.88,0.92] [--preview DIR]

HEAD_MASKS holds hull_from_views.py's front.png and side.png for the head panel (crown at the top row, side view
facing right); --mpp is their metres per pixel (front,side). The panel's crown row sits at --top, the figure's
height, and the chin --chin metres below it (given, not read off the mask: a drawn beard hangs below the chin).
A side panel drawn a little turned towards the viewer (--turn degrees) is squeezed in depth to a true profile.
  1. z is warped piecewise-linearly so the mesh's crown, nose tip and chin land on the panel's;
  2. the head is scaled once in x by the cranium width (ears left out) and in y by the depth at the brow, then
     the head is tilted about the ear to the pitch that best matches the side view, and the lower face is
     widened to the jaw's width with a scale ramping from the brow to just above the chin;
  3. each pass moves the front and back edges in the side view onto the panel's, row by row from the nose to the
     chin (smoothed over --sigma rows), each fading to nothing at mid-depth, so the nose, lips and nape follow
     the profile while the dome, the brow, the ears and the jaw keep the base's shape; below the lips the front
     moves no further forward than the lips did (a drawn beard is not the chin);
  4. a smooth grab brings the chin forward to 4 mm behind the lower lip if it was left receding.
A silhouette map row by row (as fit_to_views does for the body) turns a face into a caricature, so there is none
here. Nothing below --cut moves; motion ramps in over --ramp metres above it. Prints the IoU of each view over
crown..chin and exits 1 below --min (front,side), a sanity floor: the real check is the close-ups --preview
writes. The front floor is lower because a drawn beard and neck widen the panel below the jaw.
"""
import os
import sys

import bpy
import numpy as np
from mathutils import Matrix, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fit_to_views import gauss_rows, load_mask, render  # noqa: E402

PAD = 30   # px of margin each side of the sheet columns in the renders


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:]
    a = {"masks": argv[0], "object": None, "out": None, "mpp": None, "cut": None, "chin": None, "top": "1.88",
         "iters": "2", "sigma": "4", "ramp": "0.04", "min": "0.88,0.92", "turn": "0", "preview": None}
    i = 1
    while i < len(argv):
        k = argv[i][2:]
        if k not in a:
            raise SystemExit("fit_head_to_views: unknown argument %s" % argv[i])
        a[k] = argv[i + 1]
        i += 2
    for k in ("object", "out", "mpp", "cut", "chin"):
        if not a[k]:
            raise SystemExit("fit_head_to_views: --%s is required" % k)
    return a


def extents(m):
    """Per row (first, last) filled column, or (-1, -1) for an empty row."""
    out = np.full((m.shape[0], 2), -1, dtype=np.int64)
    for v in range(m.shape[0]):
        c = np.flatnonzero(m[v])
        if len(c):
            out[v] = c[0], c[-1]
    return out


def profile_marks(side):
    """(nose row, chin row) of a right-facing side mask whose top row is the crown. The nose tip is the most
    forward row in the middle of the head; the chin is the last row before the front edge falls back towards
    the neck (the largest one-row drop below the nose)."""
    h = side.shape[0]
    e = extents(side)
    front = e[:, 1].astype(float)
    lo, hi = int(0.3 * h), int(0.65 * h)
    nose = lo + int(np.argmax(front[lo:hi]))
    below = np.arange(nose + 3, h - 1)
    drop = front[below] - front[below + 1]
    chin = int(below[np.argmax(drop)])
    return nose, chin


def ear_rows(m, nose, chin):
    """Rows of a front head mask where the ears stick out: wider than the forehead's widest row by more than
    2 px, between the brow (0.8 of the way down to the nose) and the chin."""
    e = extents(m)
    w = np.where(e[:, 0] >= 0, e[:, 1] - e[:, 0] + 1, 0)
    ref = w[int(0.35 * nose):int(0.8 * nose)].max()
    out = np.zeros(m.shape[0], dtype=bool)
    lo, hi = int(0.8 * nose), min(chin + 1, m.shape[0])
    out[lo:hi] = w[lo:hi] > ref + 2
    if out.any():
        idx = np.flatnonzero(out)
        out[max(0, idx[0] - 2):idx[-1] + 3] = True   # the ear's root rows too
    return out


class HeadFitter:
    def __init__(self, obj, masks, mpp, cut, top, chin, sigma, ramp, turn=0.0):
        self.obj, self.cut, self.top, self.sigma, self.ramp = obj, cut, top, sigma, ramp
        self.mpp_f, self.mpp_s = mpp
        self.front = load_mask(os.path.join(masks, "front.png"))
        self.side = load_mask(os.path.join(masks, "side.png"))
        self.hf, self.hs = self.front.shape[0], self.side.shape[0]
        self.wf, self.ws = self.front.shape[1] + 2 * PAD, self.side.shape[1] + 2 * PAD
        self.sheet_f = np.pad(self.front, ((0, 0), (PAD, PAD)))
        self.sheet_s = np.pad(self.side, ((0, 0), (PAD, PAD)))
        self.nose_s = profile_marks(self.side)[0]
        self.chin_s = int(round(chin / self.mpp_s - 0.5))
        cols = np.flatnonzero(self.front[:self.nose_s * self.hf // self.hs].any(axis=0))
        self.cx_f = 0.5 * (cols[0] + cols[-1]) + PAD     # front sheet column of x = 0 (cranium centre)
        print("fit_head_to_views: sheet nose row %d, chin row %d (crown-chin %.3f m)"
              % (self.nose_s, self.chin_s, (self.chin_s + 1) * self.mpp_s))
        if turn:
            self._unturn(np.radians(turn))

    def _unturn(self, t):
        """A side panel drawn turned t towards the viewer shows depth*cos t + width*sin t: squeeze its columns
        about the brow-row centre so the brow depth is the true depth, with the width from the front panel."""
        brow = np.arange(int(0.72 * self.nose_s), int(0.78 * self.nose_s) + 1)
        e = extents(self.sheet_s[brow])
        ds = float((e[:, 1] - e[:, 0] + 1).mean()) * self.mpp_s
        ef = extents(self.sheet_f[(brow * self.mpp_s / self.mpp_f).astype(int)])
        wf = float((ef[:, 1] - ef[:, 0] + 1).mean()) * self.mpp_f
        k = (ds - wf * np.sin(t)) / np.cos(t) / ds
        c = float(e.mean())
        src = np.clip(np.round(c + (np.arange(self.ws) - c) / k).astype(int), 0, self.ws - 1)
        self.sheet_s = self.sheet_s[:, src]
        print("fit_head_to_views: side panel turned %.0f deg: depth %.3f -> %.3f m (x %.3f)"
              % (np.degrees(t), ds, ds * k, k))

    # rows: front row v is z = top - (v + 0.5) * mpp_f; side row v is z = top - (v + 0.5) * mpp_s
    def z_f(self, v):
        return self.top - (np.asarray(v) + 0.5) * self.mpp_f

    def z_s(self, v):
        return self.top - (np.asarray(v) + 0.5) * self.mpp_s

    def world(self):
        o = self.obj
        co = np.empty(len(o.data.vertices) * 3)
        o.data.vertices.foreach_get("co", co)
        M = np.array(o.matrix_world)
        return co.reshape(-1, 3) @ M[:3, :3].T + M[:3, 3]

    def set_world(self, P, Q):
        o = self.obj
        Mi = np.array(o.matrix_world.inverted())
        o.data.vertices.foreach_set("co", (Q @ Mi[:3, :3].T + Mi[:3, 3]).ravel())
        o.data.update()
        for ch in o.children:
            # rigid children (the eyes) follow the local affine fit of the surface's motion around them, which
            # carries a scale about a point behind the lids correctly where a mean of the nearest moves does not
            c = np.array(ch.matrix_world.translation)
            near = np.argsort(((P - c) ** 2).sum(1))[:96]
            mp, mq = P[near].mean(0), Q[near].mean(0)
            A = np.linalg.lstsq(P[near] - mp, Q[near] - mq, rcond=None)[0]
            ch.matrix_world = Matrix.Translation(Vector((c - mp) @ A + mq - c)) @ ch.matrix_world
        bpy.context.view_layer.update()

    def head_x(self):
        """World x of the head's mirror plane: the middle of the cage above the cut (the body was centred on
        its box, which a splayed finger can move off the midline)."""
        P = self.world()
        x = P[P[:, 2] > self.cut + self.ramp, 0]
        return 0.5 * (float(x.min()) + float(x.max()))

    def render_front(self):
        # centre the render so that the sheet's cranium centre column lands on the head's mirror plane
        cx = self.head_x() + (self.wf / 2 - self.cx_f) * self.mpp_f
        return render(self.obj, Vector((0, 1, 0)), Vector((1, 0, 0)), self.mpp_f, self.wf, self.hf,
                      (cx, 0, self.top - self.hf * self.mpp_f / 2))

    def render_side(self, y_mid, top=None):
        # screen right is -y; the render is centred on world y = y_mid, its top row at top (default: the panel's)
        top = self.top if top is None else top
        return render(self.obj, Vector((1, 0, 0)), Vector((0, -1, 0)), self.mpp_s, self.ws, self.hs,
                      (0, y_mid, top - self.hs * self.mpp_s / 2))

    def side_shift(self, mesh_s):
        """Columns to roll the side sheet by so its mean row centre over crown..chin is the mesh render's."""
        rows = np.arange(self.chin_s + 1)
        eb, ed = extents(mesh_s[rows]), extents(self.sheet_s[rows])
        good = (eb[:, 0] >= 0) & (ed[:, 0] >= 0)
        return int(round(float((eb[good].mean(1) - ed[good].mean(1)).mean())))

    def weight(self, z):
        return np.clip((z - self.cut) / self.ramp, 0.0, 1.0)

    def z_pass(self):
        P = self.world()
        y_mid = float(np.median(P[P[:, 2] > self.cut, 1]))
        mesh = self.render_side(y_mid)
        # the crown from the evaluated surface: it may sit above the panel's frame
        dg = bpy.context.evaluated_depsgraph_get()
        ev = self.obj.evaluated_get(dg)
        crown = max((ev.matrix_world @ Vector(c)).z for c in ev.bound_box)
        nose_b, chin_b = profile_marks(mesh[:, :] if crown <= self.top else self.render_side(y_mid, crown))
        if crown > self.top:
            # rendered from the crown down: shift the rows back into the panel frame
            off = (crown - self.top) / self.mpp_s
            nose_b, chin_b = nose_b - off, chin_b - off
        print("fit_head_to_views: mesh crown %.4f nose row %.1f chin row %.1f" % (crown, nose_b, chin_b))
        zb = np.array([self.cut, self.z_s(chin_b), self.z_s(nose_b), crown])
        zd = np.array([self.cut, self.z_s(self.chin_s), self.z_s(self.nose_s), self.top])
        Q = P.copy()
        up = P[:, 2] > self.cut
        Q[up, 2] = np.interp(P[up, 2], zb, zd, right=None)
        # above the crown key, keep the last segment's slope
        hi = P[:, 2] > zb[-1]
        k = (zd[-1] - zd[-2]) / (zb[-1] - zb[-2])
        Q[hi, 2] = zd[-1] + (P[hi, 2] - zb[-1]) * k
        self.set_world(P, Q)

    def scale_pass(self):
        """One anisotropic scale: x by the cranium width (rows between the forehead and the brow, ears out of the
        way), y by the depth at the brow, about the head's own centre there."""
        P = self.world()
        up = np.flatnonzero(P[:, 2] > self.cut)
        nose_f = int(round((self.nose_s + 0.5) * self.mpp_s / self.mpp_f - 0.5))
        rows = np.arange(int(0.5 * nose_f), int(0.85 * nose_f))

        def wid(m):
            return np.mean([e[1] - e[0] + 1 for e in extents(m[rows]) if e[0] >= 0])
        sx = wid(self.sheet_f) / wid(self.render_front())
        y_mid = float(np.median(P[up, 1]))
        mesh_s = self.render_side(y_mid)
        brow = np.arange(int(0.72 * self.nose_s), int(0.78 * self.nose_s) + 1)
        eb, ed = extents(mesh_s[brow]), extents(self.sheet_s[brow])
        sy = float((ed[:, 1] - ed[:, 0]).mean() / (eb[:, 1] - eb[:, 0]).mean())
        # screen column u <-> y: u = -(y - y_mid) / mpp + ws/2 - 0.5
        yc = y_mid - (float(eb.mean()) - self.ws / 2 + 0.5) * self.mpp_s
        print("fit_head_to_views: scale x %.3f y %.3f (about y %.4f)" % (sx, sy, yc))
        Q = P.copy()
        w = self.weight(P[up, 2])
        x0 = self.head_x()
        Q[up, 0] = x0 + (P[up, 0] - x0) * (1.0 + (sx - 1.0) * w)
        Q[up, 1] = yc + (P[up, 1] - yc) * (1.0 + (sy - 1.0) * w)
        self.set_world(P, Q)

    def pitch_pass(self, span=10, step=1):
        """Tilts the head about the ear (x axis through the brow-row centre at nose height) to the angle, within
        +-span degrees, whose side view best matches the panel's: a drawn head is often tipped a few degrees from
        the base's, which no scale or front-edge move can undo. The tilt ramps in above the cut like every pass."""
        P = self.world()
        up = np.flatnonzero(P[:, 2] > self.cut)
        y_mid = float(np.median(P[up, 1]))
        brow = np.arange(int(0.72 * self.nose_s), int(0.78 * self.nose_s) + 1)
        eb = extents(self.render_side(y_mid)[brow])
        yc = y_mid - (float(eb.mean()) - self.ws / 2 + 0.5) * self.mpp_s
        zc = self.z_s(self.nose_s)
        w = self.weight(P[up, 2])

        def tilt(deg):
            a = np.radians(deg) * w
            Q = P.copy()
            dy, dz = P[up, 1] - yc, P[up, 2] - zc
            Q[up, 1] = yc + dy * np.cos(a) - dz * np.sin(a)
            Q[up, 2] = zc + dy * np.sin(a) + dz * np.cos(a)
            return Q
        base = self.iou()[1]
        best = (base, 0)
        for deg in range(-span, span + 1, step):
            if deg:
                Q = tilt(deg)
                self.set_world(P, Q)
                best = max(best, (self.iou()[1], deg))
                self.set_world(Q, P)
        # a tilt has to earn its place: small gains are the panel's drawing noise
        if best[0] - base < 0.01:
            best = (base, 0)
        print("fit_head_to_views: pitch %+d deg (side IoU %.4f)" % (best[1], best[0]))
        if best[1]:
            self.set_world(P, tilt(best[1]))

    def jaw_pass(self):
        """Widens (or narrows) the lower face to the panel's jaw: the x scale ramps linearly from 1 at the brow
        to the jaw row's width ratio (a few rows above the chin, below the ears) and holds to the chin, then
        fades back to 1 at the cut. Two knots, so the face's features keep their shape."""
        P = self.world()
        up = np.flatnonzero(P[:, 2] > self.cut)
        k = self.mpp_s / self.mpp_f
        brow = int(0.75 * (self.nose_s + 0.5) * k)
        chin_f = int((self.chin_s + 0.5) * k)
        jaw = np.arange(chin_f - 8, chin_f - 3)
        mesh = self.render_front()

        def wid(m):
            return np.mean([e[1] - e[0] + 1 for e in extents(m[jaw]) if e[0] >= 0])
        r = wid(self.sheet_f) / wid(mesh)
        print("fit_head_to_views: jaw width x %.3f" % r)
        zk = np.array([self.cut, self.z_f(chin_f), self.z_f(jaw[-1]), self.z_f(brow)])
        sk = np.array([1.0, r, r, 1.0])
        z = P[up, 2]
        sc = np.interp(z, zk, sk)
        x0 = self.head_x()
        Q = P.copy()
        Q[up, 0] = x0 + (P[up, 0] - x0) * (1.0 + (sc - 1.0) * self.weight(z))
        self.set_world(P, Q)

    def chin_pass(self, setback=0.004, radius=0.025):
        """Brings the chin forward to `setback` behind the lower lip with a smooth grab (a Gaussian of `radius`
        about the chin point, front half only), when stretching the lower face has left it receding. The panel
        cannot say where the chin is under a drawn beard, so the lip is the reference."""
        P = self.world()
        up = np.flatnonzero(P[:, 2] > self.cut)
        y_mid = float(np.median(P[up, 1]))
        eb = extents(self.render_side(y_mid))
        f = eb[:, 1].astype(float)
        lip = np.arange(self.nose_s + 6, int(self.nose_s + 0.6 * (self.chin_s - self.nose_s)) + 1)
        chin = np.arange(self.chin_s - 8, self.chin_s)
        vl = lip[np.argmax(f[lip])]
        vc = chin[np.argmax(f[chin])]
        d = (f[vl] - setback / self.mpp_s - f[vc]) * self.mpp_s
        print("fit_head_to_views: chin %.1f mm behind the lower lip, moved %.1f mm"
              % (1000 * (f[vl] - f[vc]) * self.mpp_s, 1000 * max(d, 0.0)))
        if d <= 0:
            return
        c = np.array([self.head_x(), y_mid - (f[vc] - self.ws / 2 + 0.5) * self.mpp_s, self.z_s(vc)])
        r2 = ((P[up] - c) ** 2).sum(1)
        # front half only: the fraction of the way from the back edge to the front edge in this row
        z = P[up, 2]
        vs = np.clip((self.top - z) / self.mpp_s - 0.5, 0, self.hs - 1)
        u = -(P[up, 1] - y_mid) / self.mpp_s + self.ws / 2 - 0.5
        ar = np.arange(self.hs)
        has = eb[:, 0] >= 0
        ub, uf = np.interp(vs, ar[has], eb[has, 0]), np.interp(vs, ar[has], eb[has, 1])
        g = np.clip((np.clip((u - ub) / np.maximum(uf - ub, 1.0), 0, 1) - 0.5) / 0.5, 0.0, 1.0)
        Q = P.copy()
        Q[up, 1] -= d * np.exp(-r2 / (2 * radius ** 2)) * g * self.weight(z)
        self.set_world(P, Q)

    def profile_pass(self):
        """Per side-view row from crown to chin, moves the front edge onto the panel's and the back edge onto
        its back edge, each fading to nothing at mid-depth, with the two silhouettes centred on each other."""
        P = self.world()
        up = np.flatnonzero(P[:, 2] > self.cut)
        y_mid = float(np.median(P[up, 1]))
        mesh = self.render_side(y_mid)
        eb = extents(mesh)
        ed = extents(np.roll(self.sheet_s, self.side_shift(mesh), axis=1))
        n = self.hs
        off, ok = np.zeros((n, 2)), np.zeros(n, dtype=bool)
        for v in range(min(n, self.chin_s + 1)):
            if eb[v, 0] >= 0 and ed[v, 0] >= 0 and eb[v, 1] - eb[v, 0] > 6:
                off[v], ok[v] = ed[v] - eb[v], True
        idx = np.flatnonzero(ok)
        # below the chin the correction fades out over 6 rows (the neck and beard are not the face)
        fade = np.clip((self.chin_s + 6 - np.arange(n)) / 6.0, 0.0, 1.0)
        off = np.stack([gauss_rows(np.interp(np.arange(n), idx, off[idx, j]), self.sigma * (2 - j)) * fade
                        for j in (0, 1)], 1)
        # the dome keeps the base's shape: the front edge moves from the nose down (above it a turned panel's
        # edge is the far brow and temple, not the midline), the back edge from the nose down (the nape),
        # smoothed twice as much so the neck's bulk blends into the skull
        ar = np.arange(n)
        off[:, 1] *= np.clip((ar - (self.nose_s - 8)) / 8.0, 0.0, 1.0)
        # and below the lips moves no further forward than the lips did: a drawn goatee pushes the panel's chin
        # forward, so the chin keeps the base's set-back from the lips
        lip = int(self.nose_s + 0.5 * (self.chin_s - self.nose_s))
        off[lip:, 1] = np.minimum(off[lip:, 1], off[lip, 1])
        off[:, 0] *= np.clip((ar - self.nose_s) / 24.0, 0.0, 1.0)
        z = P[up, 2]
        vs = np.clip((self.top - z) / self.mpp_s - 0.5, 0, n - 1)
        u = -(P[up, 1] - y_mid) / self.mpp_s + self.ws / 2 - 0.5
        ar = np.arange(n)
        has = eb[:, 0] >= 0
        ub = np.interp(vs, ar[has], eb[has, 0])
        uf = np.interp(vs, ar[has], eb[has, 1])
        f = np.clip((u - ub) / np.maximum(uf - ub, 1.0), 0.0, 1.0)
        du = (np.interp(vs, ar, off[:, 1]) * np.clip((f - 0.5) / 0.5, 0.0, 1.0)
              + np.interp(vs, ar, off[:, 0]) * np.clip((0.5 - f) / 0.5, 0.0, 1.0)) * self.weight(z)
        Q = P.copy()
        Q[up, 1] -= du * self.mpp_s
        self.set_world(P, Q)
        print("fit_head_to_views: profile max move %.1f mm" % (1000 * np.abs(du).max() * self.mpp_s))

    def iou(self):
        """IoU per view over the rows from the crown to the chin."""
        P = self.world()
        y_mid = float(np.median(P[P[:, 2] > self.cut, 1]))
        out = []
        mesh_s = self.render_side(y_mid)
        sheet_s = np.roll(self.sheet_s, self.side_shift(mesh_s), axis=1)
        self.last = (self.render_front(), mesh_s, sheet_s)
        for mesh, sheet, n in ((self.last[0], self.sheet_f, int((self.chin_s + 1) * self.mpp_s / self.mpp_f)),
                               (mesh_s, sheet_s, self.chin_s + 1)):
            mesh, sheet = mesh[:n], sheet[:n]
            best = 0.0
            for du in range(-4, 5):   # the side sheet's placement in y is free
                m = np.roll(mesh, du, axis=1)
                best = max(best, float((m & sheet).sum()) / max(1.0, float((m | sheet).sum())))
            out.append(best)
        return tuple(out)


def head_preview(obj, top, outdir, size=0.36):
    """Studio-lit orthographic close-ups of the head: front, side, three-quarter."""
    import math
    sc = bpy.context.scene
    sc.render.film_transparent = False
    sc.display.shading.light = "STUDIO"
    sc.render.resolution_x = sc.render.resolution_y = 1024
    cam = bpy.data.objects.new("head_cam", bpy.data.cameras.new("head_cam"))
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = size
    sc.collection.objects.link(cam)
    sc.camera = cam
    M = obj.matrix_world
    ys = [(M @ v.co).y for v in obj.data.vertices if (M @ v.co).z > top - 0.22]   # crown to above the chin
    c = Vector((0.0, 0.5 * (min(ys) + max(ys)), top - 0.45 * size))
    for name, ang in (("front", 0), ("side", 90), ("threequarter", 35)):
        r = math.radians(ang)
        cam.location = (c.x + 5 * math.sin(r), c.y - 5 * math.cos(r), c.z)
        cam.rotation_euler = (math.radians(90), 0, r)
        sc.render.filepath = os.path.join(outdir, "head_%s.png" % name)
        bpy.ops.render.render(write_still=True)
    bpy.data.objects.remove(cam, do_unlink=True)


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
    mpp = tuple(float(t) for t in a["mpp"].split(","))
    fit = HeadFitter(obj, a["masks"], mpp, float(a["cut"]), float(a["top"]), float(a["chin"]), float(a["sigma"]),
                     float(a["ramp"]), float(a["turn"]))
    print("fit_head_to_views: start IoU front %.4f side %.4f" % fit.iou())
    fit.z_pass()
    fit.scale_pass()
    fit.z_pass()
    fit.pitch_pass()
    fit.z_pass()
    fit.jaw_pass()
    print("fit_head_to_views: scaled IoU front %.4f side %.4f" % fit.iou())
    for i in range(int(a["iters"])):
        fit.profile_pass()
        print("fit_head_to_views: pass %d IoU front %.4f side %.4f" % ((i + 1,) + fit.iou()))
    fit.chin_pass()
    res = fit.iou()
    if a["preview"]:
        os.makedirs(a["preview"], exist_ok=True)
        head_preview(obj, float(a["top"]), a["preview"])
    bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(a["out"]))
    floor = [float(t) for t in a["min"].split(",")] * 2
    ok = res[0] >= floor[0] and res[1] >= floor[1]
    print("fit_head_to_views: %s (front %.4f side %.4f, min %s) -> %s"
          % ("PASS" if ok else "FAIL", res[0], res[1], a["min"], a["out"]))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
