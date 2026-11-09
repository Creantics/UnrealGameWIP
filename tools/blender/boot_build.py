"""Builds a pair of leather boots around a body's feet, headless, from a JSON spec, and writes them as an OBJ with one
group per part (in the body OBJ's own frame and units), ready for md_garment.py (texture) and for a cloth tool to use
as a collider together with the body.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/boot_build.py -- SPEC.json OUT.obj

SPEC.json (lengths in the body OBJ's units, centimetres for a cloth-tool avatar; Y up, toes towards +Z):
    {"body": "avatar_cm.obj", "top": 30, "ankle": 12, "sole_top": 2.2, "margin": 0.9, "voxel": 0.35,
     "shaft_round": 2.4, "toe_round": 7, "heel_round": 4, "toe_height": 0.55, "decimate": 0.5, "wall": 0.3,
     "cuff": {"height": 6.5, "out": 0.6},
     "strap": {"y": 10.5, "shift": 1.5, "tilt": 20, "width": 2.6, "out": 0.45},
     "straps": [{"y": 10.5, ...}, {"y": 24, "tilt": 0, "buckle": true}, ...],   (instead of "strap", for a strapped shaft)
     "bands": [{"at": 0.58, "tilt": 15, "width": 1.6, "out": 0.35}, ...],
     "buckle": {"size": [2.6, 2.2], "bar": 0.4, "depth": 0.3},
     "sole": {"out": 0.55, "arch": [0.3, 0.52], "arch_lift": 0.9, "cover": 0.4}, "toe_box": 1.4,
     "tongue": {"from": 0.62, "width": 3.4, "out": 0.3}, "laces": {"count": 5, "width": 0.45, "out": 0.22, "at": 0.7}}

"top" is the shaft's upper edge, "ankle" the height below which the body counts as foot. Each leg is the body's
vertices on its side of x = 0 below "top". The upper is the voxel union of a shaft (a superellipse ring per height,
fitted to the leg's slice plus "margin") and a last (a flat-bottomed arch per station along the foot's long axis,
rounded at the toe and heel, its nose at least "toe_box" above the sole top), smoothed, cut level at "top" and
"sole_top" and given "wall" thickness inwards. The cuff, straps and bands are strips lofted over the upper's surface
along plane sections, "out" proud of it; a buckle sits on each strap (unless its "buckle" is false) at its outer
side, the extra straps grouped boot_strap2_l and so on; the sole is the upper's outline at "sole_top", "out" wider,
down to the floor, lifted by "arch_lift" between the "arch" fractions of the foot's length (from the heel), so the
heel stands apart, and pushed out wherever the foot below the sole top (toes rest lower than a sole is thick) comes
within "cover" of its edge. Groups are boot_<part>_l / _r (l = +x). Every strip is a grid with a rectangular UV
island; the upper gets a smart UV projection. The body vertices left outside the upper are counted and printed
(should be 0).
A band's plane passes "y" (default 3) above the sole top at "at" of the foot's length and leans "tilt" degrees from
upright, so its lower ends move towards the toe: a thin band (width ~0.3, "out" ~0.06) with a small tilt draws a
toe-cap line, one with a large tilt (~65) low down at ~0.4 a heel-counter line rising round the heel (what dips below
the sole top is dropped).
"tongue" is a strip up the instep's centre line (sections parallel to the foot's long vertical plane), from "from" of
the foot's length (from the heel) over the instep and up the shaft's front to the top, "out" proud of the upper;
A trainer's sole takes more keys: "points" (outline points, default 128; lugs want ~320), "toe_spring" (cm the floor
rises towards the toe tip, eased in from "spring_from" of the length, default 0.75) and "heel_bevel" (cm it rises at
the heel's back edge, over the last "bevel_len", default 0.08), "groove" {"y", "depth", "width"} (a parting line
"depth" deep round the side wall "y" above the floor) and "lugs" {"pitch", "depth", "height", "gap_lift"} (an outsole
band "height" tall whose lugs, one per "pitch" of outline, stand "depth" proud, the floor edge lifted "gap_lift"
between them). It prints each sole row's UV v (0..1) so a texture spec can put a colour on the groove or outsole.
The sole's bottom is the floor edge row and inner "rings" (fractions of the outline about its centre, default
[0.75, 0.45, 0.2]) closed by a face; "under" (cm) keeps every bottom point that far below the lowest foot vertex
within "under_r" (cm, default 1.5) of it in plan, so the lifts never leave the foot's underside showing through the
bottom (rings then default to [0.8, 0.62, 0.45, 0.3, 0.17, 0.06]; the bottom may dip below the floor by about "under").
"laces" are "count" crossings (an X per gap) between two eyelet lines at "at" of the tongue's half width, each a flat
cord "width" wide standing "out" proud of the tongue (groups boot_tongue_<side>, boot_lace_<side>); "style": "bar"
makes them "count" straight bars, each across between one pair of eyelets. A band with "tilt" over 45 (a heel counter)
orders its section points round the heel rather than by height. "straps": [] gives no strap.
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


def buckle_object(name, mid, bk, leg, tree):
    """A buckle frame and tongue on a strap's centre line, at its outermost point."""
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
    o = new_object(name, verts, faces)
    uv = o.data.uv_layers.new(name="UVMap")
    for poly in o.data.polygons:
        for li in poly.loop_indices:
            q = np.array(o.data.vertices[o.data.loops[li].vertex_index].co) - p
            uv.data[li].uv = (q @ t, q @ b_)
    return o


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
        # toes rest lower than a sole is thick: without a floor on its height the last's nose thins to a sliver the
        # remesh drops, and the toe tips end up outside the boot
        yt = np.maximum(yt, sp["sole_top"] + sp.get("toe_box", 0.0))
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


def inside_poly(pts, poly):
    """Which 2D points lie inside the closed polygon (even-odd rule)."""
    x, y = pts[:, 0][:, None], pts[:, 1][:, None]
    x0, y0 = poly[:, 0][None], poly[:, 1][None]
    x1, y1 = np.roll(poly[:, 0], -1)[None], np.roll(poly[:, 1], -1)[None]
    cross = ((y0 > y) != (y1 > y)) & (x < x0 + (y - y0) * (x1 - x0) / np.where(y1 == y0, 1e-12, y1 - y0))
    return cross.sum(1) % 2 == 1


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
    # Straps: bands on planes tilted so they rise towards the heel, each with a buckle at the outer side. "straps"
    # lists them up the shaft; a lone "strap" is the ankle strap.
    bk = sp.get("buckle")
    for si, stp in enumerate(sp.get("straps", [sp.get("strap", {})])):
        sfx = "" if si == 0 else str(si + 1)
        tilt = np.radians(stp.get("tilt", 20))
        n = up * np.cos(tilt) + d3 * np.sin(tilt)
        p0c = leg.xz(np.array([stp.get("shift", 1.5)]), np.array([0.0]))[0]
        p0 = np.array([p0c[0], stp.get("y", 10.5), p0c[1]])
        w, so = stp.get("width", 2.6) / 2, stp.get("out", 0.45)
        rows, mid = [], None
        for k, (f, o_) in enumerate(((-1.0, 0.13), (-0.85, so), (0, so * 1.1), (0.85, so), (1.0, 0.13))):
            pts, closed = slice_loop(surf, p0 + n * f * w, n)
            if not closed:
                print("boot_build: %s: strap%s section %d is open; lower the tilt" % (tag, sfx, k))
            loop = orient_loop(resample(pts, N, True), n, d3)
            rows.append(offset_along_normals(tree, loop, o_))
            if f == 0:
                mid = rows[-1]
        parts["strap" + sfx] = grid_object("boot_strap%s_%s" % (sfx, tag), rows, True)
        if bk and stp.get("buckle", True):
            parts["buckle" + sfx] = buckle_object("boot_buckle%s_%s" % (sfx, tag), mid, bk, leg, tree)
    # Instep bands: open arcs over the top of the foot, across the long axis.
    s_all, _ = leg.st(leg.P[leg.P[:, 1] < sp["ankle"]])
    smin, smax = s_all.min(), s_all.max()
    bands = []
    for b in sp.get("bands", []):
        tb = np.radians(b.get("tilt", 15))
        nb = d3 * np.cos(tb) + up * np.sin(tb)
        pc = leg.xz(np.array([smin + b["at"] * (smax - smin)]), np.array([0.0]))[0]
        pc = np.array([pc[0], yb + b.get("y", 3.0), pc[1]])
        hw, bo = b.get("width", 1.6) / 2, b.get("out", 0.35)
        rws = []
        for f, o_ in ((-1.0, 0.12), (-0.8, bo), (0, bo * 1.1), (0.8, bo), (1.0, 0.12)):
            pts, _ = slice_loop(surf, pc + nb * f * hw, nb)
            pts = pts[pts[:, 1] > yb + 0.8]
            lat = (pts[:, [0, 2]] - leg.c) @ leg.e
            if b.get("tilt", 15) > 45:  # a heel counter lies almost flat round the heel: from the outer side round
                s_b, _ = leg.st(pts)    # the back to the inner (by height the order zigzags: a smeared strip)
                s_c = smin + b["at"] * (smax - smin) + 3.0
                pts = pts[np.argsort(np.arctan2(s_c - s_b, lat))]
            else:
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
    # Tongue: the front run of sections parallel to the foot's long vertical plane (at each height the section has a
    # front and a back crossing; the front one is the bigger s), from "from" of the foot's length up to the top.
    tg = sp.get("tongue")
    if tg:
        hw, to_ = tg.get("width", 3.4) / 2, tg.get("out", 0.3)
        s_from = smin + tg.get("from", 0.62) * (smax - smin)
        pc0 = leg.xz(np.array([0.0]), np.array([0.0]))[0]
        pc0 = np.array([pc0[0], 0.0, pc0[1]])

        def front_run(f, n=40):
            pts, _ = slice_loop(surf, pc0 + e3 * f * hw, e3)
            pts = pts[pts[:, 1] > yb + 0.8]
            s_, _ = leg.st(pts)
            front = np.zeros(len(pts), bool)
            for i in range(len(pts)):
                k = np.abs(pts[:, 1] - pts[i, 1]) < 0.3
                front[i] = s_[i] > 0.5 * (s_[k].min() + s_[k].max())
            pts = pts[front & (s_ <= s_from)]
            pts = pts[np.argsort(pts[:, 1])]
            return resample(pts, n, False)

        trows = [offset_along_normals(tree, front_run(f), o_)
                 for f, o_ in ((-1.0, 0.1), (-0.85, to_), (0.0, to_ * 1.15), (0.85, to_), (1.0, 0.1))]
        parts["tongue"] = grid_object("boot_tongue_" + tag, trows, False)
        lc = sp.get("laces")
        if lc:
            ttree = BVHTree.FromObject(parts["tongue"], bpy.context.evaluated_depsgraph_get())
            Lr, Rr = front_run(-lc.get("at", 0.7)), front_run(lc.get("at", 0.7))
            n = lc.get("count", 5)
            lw, lo = lc.get("width", 0.45) / 2, lc.get("out", 0.22)
            if lc.get("style", "x") == "bar":  # straight across between each eyelet pair: reads as laces, not stitching
                idx = np.linspace(3, len(Lr) - 4, n).round().astype(int)
                pairs = [(Lr[i], Rr[i], 0.0) for i in idx]
            else:
                idx = np.linspace(3, len(Lr) - 4, n + 1).round().astype(int)
                pairs = [p_ for k in range(n) for p_ in ((Lr[idx[k]], Rr[idx[k + 1]], 0.06),
                                                         (Rr[idx[k]], Lr[idx[k + 1]], 0.0))]
            cords = []
            for a, b, up_ in pairs:
                ch = a[None, :] + np.linspace(0, 1, 9)[:, None] * (b - a)[None, :]
                # the tongue's normals face in on the mirrored side: orient each by the upper's outward normal
                hit = [ttree.find_nearest(Vector(q)) for q in ch]
                nrm = []
                for h_ in hit:
                    nt, nu_ = np.array(h_[1]), np.array(tree.find_nearest(h_[0])[1])
                    nrm.append(nt if nt @ nu_ >= 0 else -nt)
                nrm = np.array(nrm)
                mid = np.array([np.array(h_[0]) for h_ in hit]) + nrm * (lo + up_)  # one cord of each X over the other
                t_ = (b - a) / np.linalg.norm(b - a)
                side = np.cross(t_, nrm)
                side /= np.linalg.norm(side, axis=1, keepdims=True)
                cords.append(grid_object("lace", [mid - side * lw - nrm * 0.08, mid, mid + side * lw - nrm * 0.08],
                                         False))
            bm3 = bmesh.new()
            for ob in cords:
                bm3.from_mesh(ob.data)
                bpy.data.objects.remove(ob)
            me3 = bpy.data.meshes.new("boot_lace_" + tag)
            bm3.to_mesh(me3)
            bm3.free()
            parts["lace"] = bpy.data.objects.new("boot_lace_" + tag, me3)
            bpy.context.scene.collection.objects.link(parts["lace"])
    # Sole: the upper's outline at the sole top, wider, down to the floor, lifted under the arch.
    so_ = sp.get("sole", {})
    pts, closed = slice_loop(surf, (0, yb + 0.25, 0), up)
    NP = int(so_.get("points", 128))  # outline points (lugs want ~320)
    loop = orient_loop(resample(pts, NP, True), up, d3)
    loop[:, 1] = yb
    # The foot below the sole top stays inside the sole: the outline is pushed out from its centre wherever the
    # footprint (body vertices below the sole top) plus "cover" reaches past it.
    fp = leg.P[leg.P[:, 1] < yb + 0.25][:, [0, 2]]
    loop0 = loop.copy()  # the upper's own outline: the sole's top starts inside it, so a pushed-out rim shows no gap
    print("boot_build: %s: %d of %d footprint vertices outside the upper at the sole top"
          % (tag, int((~inside_poly(fp, loop[:, [0, 2]])).sum()), len(fp)))
    c2 = loop[:, [0, 2]].mean(0)
    la = np.arctan2(loop[:, 2] - c2[1], loop[:, 0] - c2[0])
    lr = np.linalg.norm(loop[:, [0, 2]] - c2, axis=1)
    fa = np.arctan2(fp[:, 1] - c2[1], fp[:, 0] - c2[0])
    fr = np.linalg.norm(fp - c2, axis=1) + so_.get("cover", 0.4)
    scale = np.ones(len(loop))
    for i in range(len(loop)):
        k = np.abs((fa - la[i] + np.pi) % (2 * np.pi) - np.pi) < 3 * np.pi / 128
        if k.any():
            scale[i] = max(1.0, fr[k].max() / lr[i])
    w1, w2 = max(1, round(3 * NP / 128)), max(1, round(4 * NP / 128))
    scale = np.max([np.roll(scale, j) for j in range(-w1, w1 + 1)], 0)  # widen, then ease the steps out
    scale = np.mean([np.roll(scale, j) for j in range(-w2, w2 + 1)], 0)
    loop[:, [0, 2]] = c2 + (loop[:, [0, 2]] - c2) * scale[:, None]
    print("boot_build: %s: sole outline pushed out at %d of %d points (most %.2f); %d footprint vertices outside it"
          % (tag, int((scale > 1.001).sum()), len(loop), float(((scale - 1) * lr).max()),
             int((~inside_poly(fp, loop[:, [0, 2]])).sum())))
    def outward(lp):
        o_ = np.cross(np.roll(lp, -1, 0) - np.roll(lp, 1, 0), up)
        o_ /= np.linalg.norm(o_, axis=1, keepdims=True)
        return -o_ if np.mean(np.sum(o_ * (lp - lp.mean(0)), 1)) < 0 else o_
    out, out0 = outward(loop), outward(loop0)
    cen = loop.mean(0)
    s_loop = ((loop[:, [0, 2]] - leg.c) @ leg.d - smin) / (smax - smin)
    a0, a1 = so_.get("arch", [0.3, 0.52])
    ramp = 0.05
    lift = np.clip(np.minimum((s_loop - a0) / ramp, (a1 - s_loop) / ramp), 0, 1)
    lift = so_.get("arch_lift", 0.9) * (0.5 - 0.5 * np.cos(np.pi * lift))
    sw = so_.get("out", 0.55)
    # toe spring and heel bevel: the floor rises towards the toe tip (eased in from "spring_from" of the length) and
    # towards the heel's back edge (over the last "bevel_len"), so the sole rocks instead of lying a flat slab
    sf, bl = so_.get("spring_from", 0.75), so_.get("bevel_len", 0.08)
    floor = lift + so_.get("toe_spring", 0.0) * np.clip((s_loop - sf) / (1 - sf), 0, 1) ** 2 \
        + so_.get("heel_bevel", 0.0) * np.clip((bl - s_loop) / bl, 0, 1) ** 2
    lg = so_.get("lugs")  # {"pitch": cm along the outline, "depth": cm proud, "height": cm, "gap_lift": cm}
    if lg:
        seg = np.linalg.norm(np.roll(loop, -1, 0) - loop, axis=1)
        cum = np.concatenate([[0.0], np.cumsum(seg)[:-1]])
        n_l = max(4, round((cum[-1] + seg[-1]) / lg.get("pitch", 2.0)))
        sq = 0.5 + 0.5 * np.tanh(3.0 * np.sin(2 * np.pi * n_l * cum / (cum[-1] + seg[-1])))  # 1 on a lug, 0 between
        ld, lh, gl = lg.get("depth", 0.25), lg.get("height", 0.6), lg.get("gap_lift", 0.25)
    else:
        sq, ld, lh, gl = np.zeros(len(loop)), 0.0, 0.25, 0.0
    yv = lambda y: np.stack([np.zeros(len(loop)), y, np.zeros(len(loop))], 1)
    flat = loop * np.array([1, 0, 1])
    rows = [loop0 - out0 * 0.4, loop + out * (sw - 0.1), loop + out * sw + yv(np.full(len(loop), -0.25))]
    gr = so_.get("groove")  # {"y": cm above the floor, "depth": cm in, "width": cm}: the midsole/outsole parting line
    if gr:
        gy, gd, gw = gr.get("y", 1.0), gr.get("depth", 0.2), gr.get("width", 0.3)
        rows += [flat + out * sw + yv(floor + gy + gw / 2), flat + out * (sw - gd) + yv(floor + gy),
                 flat + out * sw + yv(floor + gy - gw / 2)]
    if lg:  # the outsole band: lugs stand "depth" proud below "height"; between them the floor edge is lifted
        rows.append(flat + out * sw + yv(floor + lh))
    rows += [flat + out * (sw + ld * sq)[:, None] + yv(floor + 0.25),
             flat + out * (sw - 0.15 + ld * sq)[:, None] + yv(floor + gl * (1 - sq))]
    under = so_.get("under")  # cm the bottom stays under the foot (None: the floor as lifted, the foot may poke out)
    rings = so_.get("rings", [0.75, 0.45, 0.2] if under is None else [0.8, 0.62, 0.45, 0.3, 0.17, 0.06])
    for f in rings:
        rows.append(cen * np.array([1, 0, 1]) + (flat - cen * np.array([1, 0, 1])) * f + yv(floor))
    if under is not None:
        # The lifts (arch, toe spring, heel bevel, lug gaps) raise the floor above the foot's own underside, which then
        # shows through the sole when the foot lifts: each bottom point (the floor edge row and the rings) is kept
        # "under" below the lowest foot vertex within "under_r" of it (in plan), so the bottom follows the foot down.
        foot = leg.P[leg.P[:, 1] < yb]
        ur, low = so_.get("under_r", 1.5), 0.0
        for r in range(len(rows) - len(rings) - 1, len(rows)):
            d = np.linalg.norm(rows[r][:, None, [0, 2]] - foot[None, :, [0, 2]], axis=2)
            fm = np.where(d < ur, foot[None, :, 1], np.inf).min(1) - under
            low = max(low, float(np.max(rows[r][:, 1] - np.minimum(rows[r][:, 1], fm))))
            rows[r][:, 1] = np.minimum(rows[r][:, 1], fm)
        print("boot_build: %s: sole bottom kept %.2f under the foot (r %.2f), lowered at most %.2f, lowest %.2f"
              % (tag, under, ur, low, float(min(rw[:, 1].min() for rw in rows))))
    lv = np.concatenate([[0], np.cumsum([np.linalg.norm(rows[r + 1] - rows[r], axis=1).mean() for r in range(len(rows) - 1)])])
    print("boot_build: %s: sole rows v %s" % (tag, " ".join("%.3f" % x for x in lv / lv[-1])))
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
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
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
        objs += [parts[k] for k in ("upper", "cuff", "band", "tongue", "lace", "sole") if k in parts]
        objs += [o for k, o in parts.items() if k.startswith(("strap", "buckle"))]
    for o in objs:
        print("boot_build: %s %d vertices, %d faces" % (o.name, len(o.data.vertices), len(o.data.polygons)))
    write_obj(out, objs)
    print("boot_build: wrote", out)


main()
