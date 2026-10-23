"""Builds a pair of leather boots around a body's feet, headless, from a JSON spec, and writes them as an OBJ with one
group per part (in the body OBJ's own frame and units), ready for md_garment.py (texture) and for a cloth tool to use
as a collider together with the body.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/boot_build.py -- SPEC.json OUT.obj

SPEC.json (lengths in the body OBJ's units, centimetres for a cloth-tool avatar; Y up, toes towards +Z):
    {"body": "avatar_cm.obj", "top": 30, "ankle": 12, "sole_top": 2.2, "margin": 0.9, "voxel": 0.35,
     "shaft_round": 2.4, "toe_round": 7, "heel_round": 4, "toe_height": 0.55, "decimate": 0.5, "wall": 0.3,
     "cuff": {"height": 6.5, "out": 0.6},
     "strap": {"y": 10.5, "shift": 1.5, "tilt": 20, "width": 2.6, "out": 0.45},
     "bands": [{"at": 0.58, "tilt": 15, "width": 1.6, "out": 0.35}, ...],
     "buckle": {"size": [2.6, 2.2], "bar": 0.4, "depth": 0.3},
     "sole": {"out": 0.55, "arch": [0.3, 0.52], "arch_lift": 0.9}}

"top" is the shaft's upper edge, "ankle" the height below which the body counts as foot. Each leg is the body's
vertices on its side of x = 0 below "top". The upper is the voxel union of a shaft (a superellipse ring per height,
fitted to the leg's slice plus "margin") and a last (a flat-bottomed arch per station along the foot's long axis,
rounded at the toe and heel), smoothed, cut level at "top" and "sole_top" and given "wall" thickness inwards. The
cuff, strap and bands are strips lofted over the upper's surface along plane sections, "out" proud of it; the
buckle sits on the strap at the outer ankle; the sole is the upper's outline at "sole_top", "out" wider, down to the
floor, lifted by "arch_lift" between the "arch" fractions of the foot's length (from the heel), so the heel stands
apart. Groups are boot_<part>_l / _r (l = +x). Every strip is a grid with a rectangular UV island; the upper gets a
smart UV projection. The body vertices left outside the upper are counted and printed (should be 0).
"""
import json
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree


def read_obj(path):
    vs, fs = [], []
    for line in open(path):
        if line.startswith("v "):
            vs.append([float(x) for x in line.split()[1:4]])
        elif line.startswith("f "):
            fs.append([int(t.split("/")[0]) - 1 for t in line.split()[1:]])
    return np.array(vs), fs


def smooth1d(a, k=5):
    if len(a) < k:
        return a
    pad = np.pad(a, (k // 2, k // 2), mode="edge")
    return np.convolve(pad, np.ones(k) / k, mode="valid")


def new_object(name, verts, faces):
    me = bpy.data.meshes.new(name)
    me.from_pydata([tuple(v) for v in verts], [], faces)
    me.update()
    o = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(o)
    return o


def ring_faces(rows, cols, closed=True, offset=0):
    f = []
    for r in range(rows - 1):
        for c in range(cols if closed else cols - 1):
            c2 = (c + 1) % cols
            f.append([offset + r * cols + c, offset + r * cols + c2, offset + (r + 1) * cols + c2, offset + (r + 1) * cols + c])
    return f


def superellipse(cx, cz, hx, hz, n, m):
    t = np.linspace(0, 2 * np.pi, m, endpoint=False)
    c, s = np.cos(t), np.sin(t)
    return np.stack([cx + hx * np.sign(c) * np.abs(c) ** (2 / n), cz + hz * np.sign(s) * np.abs(s) ** (2 / n)], 1)


def evaluated_copy(o, name):
    dg = bpy.context.evaluated_depsgraph_get()
    me = bpy.data.meshes.new_from_object(o.evaluated_get(dg))
    for m in list(o.modifiers):
        o.modifiers.remove(m)
    old = o.data
    o.data = me
    me.name = name
    bpy.data.meshes.remove(old)


class Leg:
    def __init__(self, V, side, sp):
        self.side, self.sp = side, sp
        top = sp["top"]
        self.P = V[(side * V[:, 0] > 3) & (V[:, 1] < top + 4)]
        F = self.P[self.P[:, 1] < 4]
        c = F[:, [0, 2]].mean(0)
        w, vec = np.linalg.eigh(np.cov((F[:, [0, 2]] - c).T))
        d = vec[:, np.argmax(w)]
        if d[1] < 0:
            d = -d
        e = np.array([d[1], -d[0]])
        if side * e[0] < 0:
            e = -e
        self.c, self.d, self.e = c, d, e  # foot centre (x, z), long axis, outward lateral axis

    def xz(self, s, lat):
        return self.c + np.outer(s, self.d) + np.outer(lat, self.e)

    def st(self, pts):
        q = pts[:, [0, 2]] - self.c
        return q @ self.d, q @ self.e

    def shaft(self):
        sp, m, M = self.sp, self.sp["margin"], 48
        hs = np.arange(sp["ankle"] - 2.0, sp["top"] + 1.01, 1.0)
        heel = self.P[self.P[:, 1] < sp["ankle"], 2].min()  # the back of the heel
        hb = sp.get("heel_blend", 8.0)
        cx, cz, hx, hz = [], [], [], []
        for h in hs:
            s = self.P[np.abs(self.P[:, 1] - h) < 0.6]
            z0, z1 = s[:, 2].min(), s[:, 2].max()
            w = np.clip((sp["ankle"] + hb - h) / hb, 0, 1)  # low on the shaft its back runs down over the heel
            z0 = min(z0, heel * w + z0 * (1 - w))
            cx.append((s[:, 0].min() + s[:, 0].max()) / 2)
            cz.append((z0 + z1) / 2)
            hx.append((s[:, 0].max() - s[:, 0].min()) / 2 + m)
            hz.append((z1 - z0) / 2 + m)
        cx, cz, hx, hz = (smooth1d(np.array(a), 9) for a in (cx, cz, hx, hz))
        st_ = sp.get("straight", 0)  # a boot shaft is a tube: no narrower than this fraction of its widest
        hx, hz = np.maximum(hx, st_ * hx.max()), np.maximum(hz, st_ * hz.max())
        verts = []
        for i, h in enumerate(hs):
            r = superellipse(cx[i], cz[i], hx[i], hz[i], sp.get("shaft_round", 2.4), M)
            verts += [(x, h, z) for x, z in r]
        faces = ring_faces(len(hs), M)
        faces += [list(range(M))[::-1], list(range((len(hs) - 1) * M, len(hs) * M))]
        return new_object("shaft", verts, faces)

    def last(self):
        sp, m = self.sp, self.sp["margin"]
        foot = self.P[self.P[:, 1] < sp["ankle"]]
        s, lat = self.st(foot)
        s0, s1 = s.min() - m, s.max() + m * 1.3
        sts = np.linspace(s0, s1, 44)
        l0, l1, yt = [], [], []
        for x in sts:
            k = np.abs(s - x) < 0.9
            if k.sum() > 5:
                l0.append(lat[k].min()); l1.append(lat[k].max()); yt.append(foot[k, 1].max())
            else:
                l0.append(np.nan); l1.append(np.nan); yt.append(np.nan)
        l0, l1, yt = (np.array(a) for a in (l0, l1, yt))
        ok = ~np.isnan(l0)
        for a in (l0, l1, yt):  # the stations past the body's ends take the nearest measured one
            a[:] = np.interp(sts, sts[ok], a[ok])
        l0, l1, yt = smooth1d(l0) - m, smooth1d(l1) + m, smooth1d(yt) + m
        lc, hw = (l0 + l1) / 2, (l1 - l0) / 2
        yb = sp["sole_top"]
        rt, rh = sp.get("toe_round", 7.0), sp.get("heel_round", 4.0)
        u = np.clip((sts - (s1 - rt)) / rt, 0, 0.985)
        tn = sp.get("toe_blunt", 2.0)  # 2 an elliptic toe, higher blunter
        prof = (1 - u ** tn) ** (1 / tn)
        hw = hw * prof
        th = sp.get("toe_height", 0.55)
        yt = yb + (yt - yb) * (th + (1 - th) * prof)
        uh = np.clip(((s0 + rh) - sts) / rh, 0, 0.985)
        hw = hw * np.maximum(np.sqrt(1 - uh ** 2), 0.35)
        A, B = 32, 8
        th_ = np.linspace(0, np.pi, A)
        verts = []
        for i, x in enumerate(sts):
            c, sn = np.cos(th_), np.sin(th_)
            la = lc[i] + hw[i] * np.sign(c) * np.abs(c) ** (2 / 3)
            yy = yb + (yt[i] - yb) * np.abs(sn) ** (2 / 3)
            bot = np.linspace(lc[i] - hw[i], lc[i] + hw[i], B + 2)[1:-1]
            la = np.concatenate([la, bot]); yy = np.concatenate([yy, np.full(B, yb)])
            p = self.xz(np.full(len(la), x), la)
            verts += [(p[j, 0], yy[j], p[j, 1]) for j in range(len(la))]
        M = A + B
        faces = ring_faces(len(sts), M)
        faces += [list(range(M)), list(range((len(sts) - 1) * M, len(sts) * M))[::-1]]
        return new_object("last", verts, faces)


def bm_of(o):
    bm = bmesh.new()
    bm.from_mesh(o.data)
    return bm


def slice_loop(bm, co, no):
    """Points where the plane cuts the mesh, as the longest chain of cut edges (ordered), and whether it closes."""
    b = bm.copy()
    res = bmesh.ops.bisect_plane(b, geom=b.verts[:] + b.edges[:] + b.faces[:], dist=1e-5,
                                 plane_co=Vector(co), plane_no=Vector(no))
    edges = [g for g in res["geom_cut"] if isinstance(g, bmesh.types.BMEdge)]
    adj = {}
    for ed in edges:
        a, c = ed.verts
        adj.setdefault(a.index, []).append(c.index)
        adj.setdefault(c.index, []).append(a.index)
    b.verts.ensure_lookup_table()
    pos = {i: np.array(b.verts[i].co) for i in adj}
    seen, best, best_closed = set(), [], False
    for start in adj:
        if start in seen:
            continue
        ends = [v for v in adj if len(adj[v]) == 1]
        comp, stack = set(), [start]  # this component, and an end of it to walk from if it is open
        while stack:
            v = stack.pop()
            if v not in comp:
                comp.add(v); stack += adj[v]
        seen |= comp
        open_ends = [v for v in comp if len(adj[v]) == 1]
        cur = open_ends[0] if open_ends else start
        chain, prev = [cur], None
        while True:
            nxt = [w for w in adj[cur] if w != prev and w not in chain[-2:]]
            nxt = [w for w in nxt if w not in chain or (w == chain[0] and len(chain) > 2)]
            if not nxt or nxt[0] == chain[0]:
                break
            prev, cur = cur, nxt[0]
            chain.append(cur)
        if len(chain) > len(best):
            best, best_closed = chain, not open_ends
        del ends
    b.free()
    return np.array([pos[i] for i in best]), best_closed


def resample(pts, n, closed):
    q = np.vstack([pts, pts[:1]]) if closed else pts
    seg = np.linalg.norm(np.diff(q, axis=0), axis=1)
    t = np.concatenate([[0], np.cumsum(seg)])
    tt = np.linspace(0, t[-1], n, endpoint=not closed)
    return np.stack([np.interp(tt, t, q[:, k]) for k in range(3)], 1)


def orient_loop(pts, axis, start_dir):
    """A closed loop turned counter-clockwise about axis, starting at its point furthest along start_dir."""
    c = pts.mean(0)
    area = np.cross(pts - c, np.roll(pts, -1, 0) - c).sum(0)
    if np.dot(area, axis) < 0:
        pts = pts[::-1]
    return np.roll(pts, -int(np.argmax(pts @ start_dir)), 0)


def offset_along_normals(tree, pts, d):
    out = []
    for p in pts:
        loc, nrm, _, _ = tree.find_nearest(Vector(p))
        out.append(np.array(loc) + np.array(nrm) * d if loc is not None else p)
    return np.array(out)


def grid_object(name, rows, closed):
    """A strip from rows of points (each row the same length); a closed strip repeats its first column for the UV seam."""
    rows = [np.vstack([r, r[:1]]) if closed else r for r in rows]
    R, C = len(rows), len(rows[0])
    verts = np.vstack(rows)
    o = new_object(name, verts, ring_faces(R, C, closed=False))
    me = o.data
    uv = me.uv_layers.new(name="UVMap")
    lens_u = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(rows[R // 2], axis=0), axis=1))])
    lens_v = np.concatenate([[0], np.cumsum([np.linalg.norm(rows[r + 1] - rows[r], axis=1).mean() for r in range(R - 1)])])
    for poly in me.polygons:
        for li in poly.loop_indices:
            vi = me.loops[li].vertex_index
            uv.data[li].uv = (lens_u[vi % C], lens_v[vi // C])
    return o


def build_leg(V, side, sp):
    tag = "l" if side > 0 else "r"
    leg = Leg(V, side, sp)
    shaft, last = leg.shaft(), leg.last()
    # Union by voxel remesh, then smooth the voxel steps out.
    bm = bm_of(shaft)
    bm.from_mesh(last.data)
    bm.to_mesh(shaft.data)
    bm.free()
    bpy.data.objects.remove(last)
    mod = shaft.modifiers.new("remesh", "REMESH")
    mod.mode, mod.voxel_size = "VOXEL", sp.get("voxel", 0.35)
    evaluated_copy(shaft, "upper")
    bm = bm_of(shaft)
    for _ in range(12):
        bmesh.ops.smooth_vert(bm, verts=bm.verts, factor=0.5, use_axis_x=True, use_axis_y=True, use_axis_z=True)
    top, yb = sp["top"], sp["sole_top"]
    bmesh.ops.bisect_plane(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:], plane_co=(0, top, 0), plane_no=(0, 1, 0), clear_outer=True)
    bmesh.ops.bisect_plane(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:], plane_co=(0, yb + 0.05, 0), plane_no=(0, 1, 0), clear_inner=True)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(shaft.data)
    shaft.data.update()
    surf = bm  # the open outer surface, for sections and offsets
    tree = BVHTree.FromBMesh(surf)
    # Body vertices left outside the upper.
    body = leg.P[(leg.P[:, 1] > yb + 0.5) & (leg.P[:, 1] < top - 0.5)]
    outside, worst = 0, 0.0
    for p in body:
        loc, nrm, _, _ = tree.find_nearest(Vector(p))
        dd = np.dot(np.array(p) - np.array(loc), np.array(nrm))
        if dd > 0:
            outside += 1; worst = max(worst, dd)
    print("boot_build: %s: %d of %d body vertices outside the upper (worst %.2f)" % (tag, outside, len(body), worst))
    parts = {}
    up = np.array([0.0, 1.0, 0.0])
    d3 = np.array([leg.d[0], 0, leg.d[1]])
    e3 = np.array([leg.e[0], 0, leg.e[1]])
    # Cuff: a folded band round the top, lipped over the rim.
    cf = sp.get("cuff", {})
    ch, co_ = cf.get("height", 6.5), cf.get("out", 0.6)
    N = 96
    base = []
    for h in (top - ch - 0.2, top - ch, top - ch / 2, top - 0.4):
        pts, closed = slice_loop(surf, (0, h, 0), up)
        base.append(orient_loop(resample(pts, N, True), up, d3))
    rimp = base[-1]
    rows = [offset_along_normals(tree, base[0], 0.15), offset_along_normals(tree, base[1], co_),
            offset_along_normals(tree, base[2], co_), offset_along_normals(tree, base[3], co_)]
    cen = rimp.mean(0)
    radial = (rimp - cen) * np.array([1, 0, 1])
    radial /= np.linalg.norm(radial, axis=1, keepdims=True)
    lip = rimp + up * 0.65
    rows += [lip + radial * (co_ * 0.55), lip - radial * (sp.get("wall", 0.3) + 0.1),
             rimp - up * 0.6 - radial * (sp.get("wall", 0.3) + 0.12)]
    parts["cuff"] = grid_object("boot_cuff_" + tag, rows, True)
    # Ankle strap: a band on a plane tilted so it rises towards the heel.
    stp = sp.get("strap", {})
    tilt = np.radians(stp.get("tilt", 20))
    n = up * np.cos(tilt) + d3 * np.sin(tilt)
    p0c = leg.xz(np.array([stp.get("shift", 1.5)]), np.array([0.0]))[0]
    p0 = np.array([p0c[0], stp.get("y", 10.5), p0c[1]])
    w, so = stp.get("width", 2.6) / 2, stp.get("out", 0.45)
    rows, mid = [], None
    for k, (f, o_) in enumerate(((-1.0, 0.13), (-0.85, so), (0, so * 1.1), (0.85, so), (1.0, 0.13))):
        pts, closed = slice_loop(surf, p0 + n * f * w, n)
        if not closed:
            print("boot_build: %s: strap section %d is open; lower the tilt" % (tag, k))
        loop = orient_loop(resample(pts, N, True), n, d3)
        rows.append(offset_along_normals(tree, loop, o_))
        if f == 0:
            mid = rows[-1]
    parts["strap"] = grid_object("boot_strap_" + tag, rows, True)
    # Instep bands: open arcs over the top of the foot, across the long axis.
    s_all, _ = leg.st(leg.P[leg.P[:, 1] < sp["ankle"]])
    smin, smax = s_all.min(), s_all.max()
    bands = []
    for b in sp.get("bands", []):
        tb = np.radians(b.get("tilt", 15))
        nb = d3 * np.cos(tb) + up * np.sin(tb)
        pc = leg.xz(np.array([smin + b["at"] * (smax - smin)]), np.array([0.0]))[0]
        pc = np.array([pc[0], yb + 3, pc[1]])
        hw, bo = b.get("width", 1.6) / 2, b.get("out", 0.35)
        rws = []
        for f, o_ in ((-1.0, 0.12), (-0.8, bo), (0, bo * 1.1), (0.8, bo), (1.0, 0.12)):
            pts, _ = slice_loop(surf, pc + nb * f * hw, nb)
            pts = pts[pts[:, 1] > yb + 0.8]
            lat = (pts[:, [0, 2]] - leg.c) @ leg.e
            pts = pts[np.argsort(np.arctan2(pts[:, 1] - yb, lat))]  # from the outer side over the top to the inner
            rws.append(offset_along_normals(tree, resample(pts, 48, False), o_))
        bands.append(rws)
    if bands:
        verts, faces, uvs = [], [], []
        o = None
        rows_all = []
        for rws in bands:
            rows_all.append(rws)
        objs = [grid_object("band", rws, False) for rws in rows_all]
        bm2 = bmesh.new()
        for ob in objs:
            bm2.from_mesh(ob.data)
            bpy.data.objects.remove(ob)
        me = bpy.data.meshes.new("boot_band_" + tag)
        bm2.to_mesh(me)
        bm2.free()
        o = bpy.data.objects.new("boot_band_" + tag, me)
        bpy.context.scene.collection.objects.link(o)
        parts["band"] = o
    # Buckle on the strap at the outer ankle.
    bk = sp.get("buckle")
    if bk and mid is not None:
        i = int(np.argmax((mid[:, [0, 2]] - leg.c) @ leg.e))
        p = mid[i]
        t = mid[(i + 1) % len(mid)] - mid[i - 1]
        t /= np.linalg.norm(t)
        _, nrm, _, _ = tree.find_nearest(Vector(p))
        nrm = np.array(nrm)
        b_ = np.cross(nrm, t)
        (W, H), bar, dep = bk.get("size", [2.6, 2.2]), bk.get("bar", 0.4), bk.get("depth", 0.3)
        verts, faces = [], []
        for z in (0.0, dep):
            for (hx, hy) in ((W / 2, H / 2), (W / 2 - bar, H / 2 - bar)):
                for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                    verts.append(p + nrm * (0.05 + z) + t * sx * hx + b_ * sy * hy)
        # rings: 0 outer bottom, 4 inner bottom, 8 outer top, 12 inner top
        for a, c in ((8, 12), (12, 4), (4, 0), (0, 8)):
            for k in range(4):
                k2 = (k + 1) % 4
                faces.append([a + k, a + k2, c + k2, c + k])
        tongue = [p + nrm * (0.05 + dep * 0.6) + t * (-W / 2 + bar * 0.5) + b_ * sy * 0.12 for sy in (-1, 1)]
        tongue += [q + t * (W - bar) for q in tongue[::-1]]
        verts += tongue
        faces.append([16, 17, 18, 19])
        o = new_object("boot_buckle_" + tag, verts, faces)
        uv = o.data.uv_layers.new(name="UVMap")
        for poly in o.data.polygons:
            for li in poly.loop_indices:
                q = np.array(o.data.vertices[o.data.loops[li].vertex_index].co) - p
                uv.data[li].uv = (q @ t, q @ b_)
        parts["buckle"] = o
    # Sole: the upper's outline at the sole top, wider, down to the floor, lifted under the arch.
    so_ = sp.get("sole", {})
    pts, closed = slice_loop(surf, (0, yb + 0.25, 0), up)
    loop = orient_loop(resample(pts, 128, True), up, d3)
    loop[:, 1] = yb
    nxt, prv = np.roll(loop, -1, 0), np.roll(loop, 1, 0)
    tan = nxt - prv
    out = np.cross(tan, up)
    out /= np.linalg.norm(out, axis=1, keepdims=True)
    cen = loop.mean(0)
    if np.mean(np.sum(out * (loop - cen), 1)) < 0:
        out = -out
    s_loop = ((loop[:, [0, 2]] - leg.c) @ leg.d - smin) / (smax - smin)
    a0, a1 = so_.get("arch", [0.3, 0.52])
    ramp = 0.05
    lift = np.clip(np.minimum((s_loop - a0) / ramp, (a1 - s_loop) / ramp), 0, 1)
    lift = so_.get("arch_lift", 0.9) * (0.5 - 0.5 * np.cos(np.pi * lift))
    sw = so_.get("out", 0.55)
    yv = lambda y: np.stack([np.zeros(len(loop)), y, np.zeros(len(loop))], 1)
    rows = [loop - out * 0.4, loop + out * (sw - 0.1), loop + out * sw + yv(np.full(len(loop), -0.25)),
            loop * np.array([1, 0, 1]) + out * sw + yv(lift + 0.25), loop * np.array([1, 0, 1]) + out * (sw - 0.15) + yv(lift)]
    flat = loop * np.array([1, 0, 1])
    for f in (0.75, 0.45, 0.2):
        rows.append(cen * np.array([1, 0, 1]) + (flat - cen * np.array([1, 0, 1])) * f + yv(lift))
    o = grid_object("boot_sole_" + tag, rows, True)
    bm2 = bm_of(o)
    edge = [v for v in bm2.verts if v.index >= (len(rows) - 1) * (len(loop) + 1)]
    bmesh.ops.contextual_create(bm2, geom=[e for e in bm2.edges if all(v in edge for v in e.verts)])
    bm2.to_mesh(o.data)
    bm2.free()
    parts["sole"] = o
    # The upper: thickness inwards, fewer faces, smart UVs.
    if sp.get("decimate", 0.5) < 1:
        mod = shaft.modifiers.new("dec", "DECIMATE")
        mod.ratio = sp.get("decimate", 0.5)
        evaluated_copy(shaft, "upper")
    mod = shaft.modifiers.new("wall", "SOLIDIFY")
    mod.thickness, mod.offset = sp.get("wall", 0.3), -1.0
    evaluated_copy(shaft, "boot_upper_" + tag)
    shaft.name = "boot_upper_" + tag
    bpy.context.view_layer.objects.active = shaft
    for ob in bpy.context.scene.objects:
        ob.select_set(ob is shaft)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=1.15, island_margin=0.01)
    bpy.ops.object.mode_set(mode="OBJECT")
    parts["upper"] = shaft
    surf.free()
    return parts


def write_obj(path, objs):
    with open(path, "w") as f:
        f.write("# boot_build.py\n")
        vo, to = 1, 1
        for o in objs:
            me = o.data
            uvl = me.uv_layers.active.data if me.uv_layers.active else None
            f.write("g %s\n" % o.name)
            for v in me.vertices:
                f.write("v %.5f %.5f %.5f\n" % tuple(v.co))
            for li in range(len(me.loops)):
                f.write("vt %.6f %.6f\n" % (tuple(uvl[li].uv) if uvl else (0.0, 0.0)))
            for poly in me.polygons:
                f.write("f " + " ".join("%d/%d" % (me.loops[li].vertex_index + vo, li + to) for li in poly.loop_indices) + "\n")
            vo += len(me.vertices)
            to += len(me.loops)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    if len(argv) < 2:
        sys.exit("boot_build: SPEC.json OUT.obj")
    spec_path, out = os.path.abspath(argv[0]), os.path.abspath(argv[1])
    sp = json.load(open(spec_path))
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o)
    V, _ = read_obj(os.path.join(os.path.dirname(spec_path), sp["body"]))
    objs = []
    for side in (1, -1):
        parts = build_leg(V, side, sp)
        objs += [parts[k] for k in ("upper", "cuff", "strap", "band", "buckle", "sole") if k in parts]
    for o in objs:
        print("boot_build: %s %d vertices, %d faces" % (o.name, len(o.data.vertices), len(o.data.polygons)))
    write_obj(out, objs)
    print("boot_build: wrote", out)


main()
