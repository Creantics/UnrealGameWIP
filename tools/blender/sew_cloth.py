"""Sews a garment from flat pattern pieces round a body in Blender's cloth solver, headless, and writes it the way a
cloth tool exports one: an OBJ with one group per pattern piece (UVs in pattern metres) for md_garment.py.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/sew_cloth.py -- SPEC.json OUT_DIR

SPEC.json (lengths in the pattern are millimetres, everything placed in the body's world space is metres):
    {"collider": {"fbx": "mh_body.fbx", "face": "mh_face.fbx", "extra": ["under/garment.obj"]}
                 | {"cylinder": {"center": [x, y, z], "radius": r, "height": h}}
                 | {"cone": {"center", "r_bottom", "r_top", "height"}},
     "edge": 0.015,        target edge length of the cloth mesh (m)
     "frames": 60,         solver frames; "settle" more frames with the sewing springs off (default 0; nothing
                           holds the seams then, and on a body both 20-frame tries tore the garment apart: leave it 0)
     "fabric": {...},      defaults for every piece (see FABRIC below); a piece's own "fabric" overrides them
     "self_collision": false,
     "pieces": [{"name": "front", "outline": [[u, v], ...] (mm, counter-clockwise seen from outside, u right, v up),
                 "place": PLACE, "pin": [REGION, ...], "shrink": [[REGION, factor], ...], "fabric": {...}}],
     "seams": [{"a": [piece, i, j], "b": [piece, k, l], "flip": "auto" | true | false}]}

A seam side [piece, i, j] is the outline from corner i to corner j going forward through the corners (wrapping); a
and b may be the same piece (a side seam of a panel folded round the body, the underarm seam of a sleeve tube). Both
sides are cut into the same number of segments, so every sewing spring joins one vertex to one vertex and the seam
closes fully when the springs pull it shut; ease (one side longer) is gathered evenly. "flip" pairs a's start with
b's end; "auto" picks the pairing with the shorter springs once the pieces are placed.

PLACE wraps the flat piece round an axis before the solver starts, the way a cloth tool's arrangement points do:
    {"type": "cylinder", "center": [x, y, z], "axis": [x, y, z] (pattern v runs along it), "front": [x, y, z] (angle 0),
     "radius": r or "radius_at": [[v mm, r], ...] (a cone), "angle": deg (where "at" lands, positive towards the viewer's right seen from outside), "at": [u, v]
     (the pattern point placed on "center"'s height), "conform": "out" | "in" | null, "gap": m}
    {"type": "plane", "center": [...], "normal": [...] (the outside), "up": [...], "at": [u, v]}
    {"type": "ribbon", "path": [[x, y, z], ...], "width": [x, y, z] or one per path point, "at": [u, v], "s_at": m}
     pattern u runs along the path by arc length (u = at[0] lands "s_at" metres along it; straight on past both
     ends), v along the width direction (v = at[1] on the path). A panel that runs from the back hem over the
     shoulder to the front hem without a shoulder seam, or a collar laid along a neckline. Seen from outside the
     outline is still counter-clockwise: the outside is path direction x width. "conform" "in" casts back along that
     normal from "reach" metres out (default 0.12), so only body within reach of the panel moves it.
A collider's "extra" OBJs (a garment sewn before this one, as sew_cloth writes it) collide too, so an over layer drapes
on the under layer instead of through it. "conform" pushes each vertex off the body along the cylinder's radius so the
piece starts outside it: "out" casts from the axis outwards (a torso piece: the first hit is the torso, not the arm
hanging beside it), "in" from outside inwards (a skirt round both legs; "reach", default 1.5 m, is how far out the cast
starts). The vertex lands "gap" (default 0.01 m) off the surface it hit, never inside it.

REGION picks pattern vertices: {"v_above": mm} | {"v_below": mm} | {"u_above": mm} | {"u_below": mm} |
{"rect": [u0, v0, u1, v1]}; several keys in one region must all hold. "pin" holds the region where it was placed
(goal stiffness 1); "snap": true on a pin region first moves it onto the body plus "gap", so a waistband or collar is
held on the body instead of in the air. "shrink" pulls the region's fabric in by factor (0.1 = 10 %), an elastic band.

FABRIC (Blender cloth values; the presets' numbers, tuned for a metre-scale scene): "mass" (kg per vertex), "tension",
"compression", "shear", "bending", "air", "quality" (solver steps per frame), "distance" (collision distance to the
body, m), "self_distance", "friction", "sewing_force" (cap on a sewing spring's pull; 0 = none), "thickness" (m; only
logged, md_garment solidifies). Named presets: "cotton", "denim", "wool", "linen", "jersey", "gauze", "leather",
"nylon"; {"preset": "denim", "bending": 20} starts from one.

Writes OUT_DIR/garment.obj (+ .mtl), OUT_DIR/avatar.glb (the collider body, for md_garment's AO "occluders"),
OUT_DIR/sewn.blend (the draped pieces and the body) and OUT_DIR/sew_log.json: per seam the mean and largest gap after
the drape, per piece how many vertices ended inside the body, and the largest edge stretch."""
import json
import math
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from mathutils.geometry import delaunay_2d_cdt

PRESETS = {
    # mass kg/vertex, then the spring stiffnesses; bending is what makes a heavy fabric fold in big soft waves
    "cotton": {"mass": 0.3, "tension": 15, "compression": 15, "shear": 5, "bending": 0.5, "air": 1.0},
    "jersey": {"mass": 0.25, "tension": 8, "compression": 8, "shear": 3, "bending": 0.2, "air": 1.0},
    "linen": {"mass": 0.3, "tension": 25, "compression": 25, "shear": 8, "bending": 1.5, "air": 1.0},
    "wool": {"mass": 0.45, "tension": 20, "compression": 20, "shear": 8, "bending": 3.0, "air": 1.0},
    "denim": {"mass": 1.0, "tension": 40, "compression": 40, "shear": 40, "bending": 10, "air": 1.0},
    "nylon": {"mass": 0.3, "tension": 30, "compression": 30, "shear": 10, "bending": 2.0, "air": 1.5},
    "gauze": {"mass": 0.12, "tension": 5, "compression": 5, "shear": 2, "bending": 0.05, "air": 1.5},
    "leather": {"mass": 0.4, "tension": 80, "compression": 80, "shear": 80, "bending": 150, "air": 1.0},
}
FABRIC_DEFAULT = {"preset": "cotton", "quality": 10, "distance": 0.004, "self_distance": 0.003, "friction": 8.0,
                  "sewing_force": 8.0, "thickness": 0.002}


def log(*a):
    print("SEW", *a, flush=True)


def fabric(spec_fab, piece_fab=None):
    f = dict(FABRIC_DEFAULT)
    f.update(spec_fab or {})
    f.update(piece_fab or {})
    base = dict(PRESETS[f.get("preset", "cotton")])
    base.update({k: v for k, v in f.items() if k != "preset"})
    return base


# -- the flat pieces ---------------------------------------------------------------------------------------------------

def arc(pts):
    d = np.linalg.norm(np.diff(pts, axis=0), axis=1)
    return np.concatenate([[0.0], np.cumsum(d)])


def corner_path(outline, i, j):
    """Corner points from i to j going forward (wrapping)."""
    n = len(outline)
    idx = [i]
    while idx[-1] != j:
        idx.append((idx[-1] + 1) % n)
        if len(idx) > n + 1:
            raise ValueError("bad seam range %d..%d" % (i, j))
    return np.array([outline[k] for k in idx]), idx


def sample_path(pts, nseg):
    """nseg + 1 points evenly by arc length along a polyline."""
    s = arc(pts)
    t = np.linspace(0.0, s[-1], nseg + 1)
    return np.stack([np.interp(t, s, pts[:, 0]), np.interp(t, s, pts[:, 1])], axis=1)


def plan_ranges(spec, edge_mm):
    """Per piece, the outline split into ranges (corner i to corner j) with a segment count each, so both sides of
    every seam get the same count."""
    pieces = {p["name"]: p for p in spec["pieces"]}
    ranges = {name: [] for name in pieces}
    for si, sm in enumerate(spec.get("seams", [])):
        (pa, ia, ja), (pb, ib, jb) = sm["a"], sm["b"]
        la = arc(corner_path(np.array(pieces[pa]["outline"], float), ia, ja)[0])[-1]
        lb = arc(corner_path(np.array(pieces[pb]["outline"], float), ib, jb)[0])[-1]
        nseg = max(1, int(round(max(la, lb) / edge_mm)))
        ranges[pa].append({"i": ia, "j": ja, "n": nseg, "seam": si, "side": "a"})
        ranges[pb].append({"i": ib, "j": jb, "n": nseg, "seam": si, "side": "b"})
        if abs(la - lb) > 0.15 * max(la, lb):
            log("seam %d (%s %d-%d / %s %d-%d): lengths %.0f / %.0f mm, %.0f %% ease"
                % (si, pa, ia, ja, pb, ib, jb, la, lb, 100 * abs(la - lb) / min(la, lb)))
    for name, p in pieces.items():
        n = len(p["outline"])
        used = {}
        for r in ranges[name]:
            _, idx = corner_path(np.array(p["outline"], float), r["i"], r["j"])
            for a, b in zip(idx[:-1], idx[1:]):
                if a in used:
                    raise ValueError("%s: outline edge %d is in two seams (%s and %s)" % (name, a, used[a], r["seam"]))
                used[a] = r["seam"]
        for k in range(n):  # an unsewn edge (a hem, an opening) is its own range
            if k not in used:
                pts = np.array([p["outline"][k], p["outline"][(k + 1) % n]], float)
                ranges[name].append({"i": k, "j": (k + 1) % n, "n": max(1, int(round(arc(pts)[-1] / edge_mm))),
                                     "seam": None})
        ranges[name].sort(key=lambda r: r["i"])
    return ranges


def in_polygon(pts, poly):
    x, y = pts[:, 0][:, None], pts[:, 1][:, None]
    x0, y0 = poly[:, 0][None], poly[:, 1][None]
    x1, y1 = np.roll(poly[:, 0], -1)[None], np.roll(poly[:, 1], -1)[None]
    cross = (y0 > y) != (y1 > y)
    xh = x0 + (y - y0) * (x1 - x0) / np.where(y1 == y0, 1e-12, y1 - y0)
    return (np.sum(cross & (x < xh), axis=1) % 2) == 1


def seg_dist(pts, poly):
    a, b = poly, np.roll(poly, -1, axis=0)
    ab = b - a
    t = np.clip(((pts[:, None] - a[None]) * ab[None]).sum(2) / np.maximum((ab ** 2).sum(1), 1e-12)[None], 0, 1)
    return np.linalg.norm(pts[:, None] - (a[None] + t[..., None] * ab[None]), axis=2).min(1)


def build_piece(p, rngs, edge_mm):
    """Triangulates one flat piece: boundary from the planned ranges, an even triangular lattice inside.
    Returns (uv points mm, triangles, boundary index list, {(seam index, side): boundary vertex ids in order})."""
    outline = np.array(p["outline"], float)
    if sum((outline[:, 0] * np.roll(outline[:, 1], -1) - np.roll(outline[:, 0], -1) * outline[:, 1])) < 0:
        raise ValueError("%s: outline is clockwise; patterns are counter-clockwise seen from outside" % p["name"])
    bnd, seam_ids = [], {}
    for r in rngs:
        pts = sample_path(corner_path(outline, r["i"], r["j"])[0], r["n"])
        ids = list(range(len(bnd), len(bnd) + r["n"])) + [None]  # the range's last point is the next range's first
        bnd.extend(pts[:-1].tolist())
        if r["seam"] is not None:
            seam_ids[(r["seam"], r["side"])] = ids
    nb = len(bnd)
    for ids in seam_ids.values():  # close each seam range on the vertex that starts the next range
        last = ids[-2] + 1
        ids[-1] = last % nb
    bnd = np.array(bnd)
    # interior: an equilateral lattice clipped to the piece, kept off the boundary
    lo, hi = outline.min(0), outline.max(0)
    dy = edge_mm * math.sqrt(3) / 2
    rows = np.arange(lo[1] + dy / 2, hi[1], dy)
    pts = []
    for k, y in enumerate(rows):
        xs = np.arange(lo[0] + (edge_mm / 2 if k % 2 else 0), hi[0], edge_mm)
        pts.append(np.stack([xs, np.full_like(xs, y)], axis=1))
    inner = np.concatenate(pts) if pts else np.zeros((0, 2))
    if len(inner):
        inner = inner[in_polygon(inner, bnd)]
        inner = inner[seg_dist(inner, bnd) > 0.55 * edge_mm]
    allp = np.concatenate([bnd, inner])
    edges = [(k, (k + 1) % nb) for k in range(nb)]
    out = delaunay_2d_cdt([Vector(q) for q in allp.tolist()], edges, [list(range(nb))], 1, 1e-6, True)
    verts, _, faces, orig_v = out[0], out[1], out[2], out[3]
    if len(verts) != len(allp):
        # the triangulator only merges or adds points when the outline crosses itself; keep its points
        log("%s: triangulator changed the vertex count %d -> %d" % (p["name"], len(allp), len(verts)))
    remap = {}
    for new_i, origs in enumerate(orig_v):
        for o in origs:
            remap[o] = new_i
    uv = np.array([[v.x, v.y] for v in verts])
    seams = {key: [remap[i] for i in ids] for key, ids in seam_ids.items()}
    boundary = [remap[k] for k in range(nb)]
    tris = [f for f in faces if len(f) == 3]
    return uv, tris, boundary, seams


# -- placing ------------------------------------------------------------------------------------------------------------

def unit(v):
    v = np.asarray(v, float)
    return v / max(np.linalg.norm(v), 1e-12)


def place(uv_mm, pl):
    at = np.array(pl.get("at", [0, 0]), float)
    du = (uv_mm[:, 0] - at[0]) / 1000.0
    dv = (uv_mm[:, 1] - at[1]) / 1000.0
    c = np.array(pl.get("center", [0.0, 0.0, 0.0]), float)  # a ribbon has a path instead
    if pl["type"] == "plane":
        n, up = unit(pl["normal"]), unit(pl.get("up", [0, 0, 1]))
        up = unit(up - n * (up @ n))
        right = np.cross(up, n)
        return c[None] + du[:, None] * right[None] + dv[:, None] * up[None], None
    if pl["type"] == "ribbon":
        path = np.array(pl["path"], float)
        seg = np.linalg.norm(np.diff(path, axis=0), axis=1)
        s_cum = np.r_[0.0, np.cumsum(seg)]
        wid = np.array(pl["width"], float)
        if wid.ndim == 1:
            wid = np.tile(wid, (len(path), 1))
        su = float(pl.get("s_at", 0.0)) + du
        k = np.clip(np.searchsorted(s_cum, su) - 1, 0, len(seg) - 1)
        t = (su - s_cum[k]) / np.maximum(seg[k], 1e-9)  # past either end t runs on: the path is carried on straight
        tc = np.clip(t, 0.0, 1.0)
        w = wid[k] + tc[:, None] * (wid[k + 1] - wid[k])
        w /= np.maximum(np.linalg.norm(w, axis=1), 1e-12)[:, None]
        tang = (path[k + 1] - path[k]) / np.maximum(seg[k], 1e-9)[:, None]
        nrm = np.cross(tang, w)
        nrm /= np.maximum(np.linalg.norm(nrm, axis=1), 1e-12)[:, None]
        co = path[k] + t[:, None] * (path[k + 1] - path[k]) + dv[:, None] * w
        return co, (co.copy(), nrm)
    ax = unit(pl.get("axis", [0, 0, 1]))
    fr = unit(pl.get("front", [0, -1, 0]))
    fr = unit(fr - ax * (fr @ ax))
    side = np.cross(ax, fr)
    if "radius_at" in pl:  # a cone: the radius follows pattern v, so a flared panel wraps without stretching
        ra = np.array(pl["radius_at"], float)
        r = np.interp(uv_mm[:, 1], ra[:, 0], ra[:, 1])
    else:
        r = np.full(len(uv_mm), float(pl["radius"]))
    phi = math.radians(pl.get("angle", 0.0)) + du / r
    radial = np.cos(phi)[:, None] * fr[None] + np.sin(phi)[:, None] * side[None]
    axis_pt = c[None] + dv[:, None] * ax[None]
    return axis_pt + r[:, None] * radial, (axis_pt, radial)


def conform(co, frame, bvh, mode, gap, reach=1.5):
    """Pushes vertices off the body along the cylinder's radius (see the module doc)."""
    axis_pt, radial = frame
    moved = 0
    for k in range(len(co)):
        o, d = Vector(axis_pt[k]), Vector(radial[k])
        r_now = (Vector(co[k]) - o).dot(d)
        if mode == "out":
            hit = bvh.ray_cast(o, d, 2.0)
            if hit[0] is None:
                continue
            need = (hit[0] - o).dot(d) + gap
        else:
            far = o + d * reach
            hit = bvh.ray_cast(far, -d, reach)
            if hit[0] is None:
                continue
            need = (hit[0] - o).dot(d) + gap
        if r_now < need:
            co[k] = np.array(o + d * need)
            moved += 1
    return moved


def region_mask(uv_mm, reg):
    m = np.ones(len(uv_mm), bool)
    u, v = uv_mm[:, 0], uv_mm[:, 1]
    if "v_above" in reg:
        m &= v >= reg["v_above"]
    if "v_below" in reg:
        m &= v <= reg["v_below"]
    if "u_above" in reg:
        m &= u >= reg["u_above"]
    if "u_below" in reg:
        m &= u <= reg["u_below"]
    if "rect" in reg:
        u0, v0, u1, v1 = reg["rect"]
        m &= (u >= u0) & (u <= u1) & (v >= v0) & (v <= v1)
    return m


# -- the collider -------------------------------------------------------------------------------------------------------

def load_collider(spec, sdir):
    col = spec["collider"]
    if "cylinder" in col or "cone" in col:
        cy = col.get("cylinder") or col["cone"]
        if "cone" in col:  # radius r_bottom at the base, r_top at the top: a dress form's hips to waist
            bpy.ops.mesh.primitive_cone_add(vertices=96, radius1=cy["r_bottom"], radius2=cy["r_top"],
                                            depth=cy["height"], location=cy["center"])
        else:
            bpy.ops.mesh.primitive_cylinder_add(vertices=96, radius=cy["radius"], depth=cy["height"],
                                                location=cy["center"])
        body = bpy.context.active_object
        body.name = "avatar"
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
        return body, []
    bpy.ops.import_scene.fbx(filepath=os.path.join(sdir, col["fbx"]))
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    body = max(meshes, key=lambda o: len(o.data.vertices))
    extra = []
    if col.get("face"):
        before = set(bpy.data.objects)
        bpy.ops.import_scene.fbx(filepath=os.path.join(sdir, col["face"]))
        fm = [o for o in bpy.data.objects if o not in before and o.type == "MESH"]
        if fm:
            extra.append(max(fm, key=lambda o: len(o.data.vertices)))
        meshes += fm
    for o in meshes:  # the LODs below 0 only slow the collision down
        if o is not body and o not in extra:
            bpy.data.objects.remove(o, do_unlink=True)
    for path in col.get("extra", []):  # garments sewn before this one (an under layer): OBJs in this world, metres, Z up
        before = set(bpy.data.objects)
        bpy.ops.wm.obj_import(filepath=os.path.join(sdir, path), forward_axis="Y", up_axis="Z")
        got = [o for o in bpy.data.objects if o not in before and o.type == "MESH"]
        extra += got
        log("collider extra %s: %d objects, %d verts" % (path, len(got), sum(len(o.data.vertices) for o in got)))
    keep = [body] + extra
    # freeze the rest pose into plain meshes (the solver collides with the evaluated mesh anyway; plain is faster)
    dg = bpy.context.evaluated_depsgraph_get()
    frozen = []
    for o in keep:
        me = bpy.data.meshes.new_from_object(o.evaluated_get(dg))
        me.transform(o.matrix_world)
        f = bpy.data.objects.new("avatar" if o is body else "avatar_face", me)
        bpy.context.scene.collection.objects.link(f)
        frozen.append(f)
    for o in list(bpy.data.objects):
        if o not in frozen:
            bpy.data.objects.remove(o, do_unlink=True)
    lo = np.min([np.array(v.co) for v in frozen[0].data.vertices], axis=0)
    hi = np.max([np.array(v.co) for v in frozen[0].data.vertices], axis=0)
    log("collider %s %d verts, bbox %s .. %s" % (frozen[0].name, len(frozen[0].data.vertices), np.round(lo, 3),
                                                 np.round(hi, 3)))
    return frozen[0], frozen[1:]


def bvh_of(objs):
    bm = bmesh.new()
    for o in objs:
        tmp = bmesh.new()
        tmp.from_mesh(o.data)
        tmp.transform(o.matrix_world)
        me = bpy.data.meshes.new("tmp")
        tmp.to_mesh(me)
        tmp.free()
        bm.from_mesh(me)
        bpy.data.meshes.remove(me)
    tree = BVHTree.FromBMesh(bm)
    return tree, bm


def add_collision(o, fab):
    m = o.modifiers.new("collision", "COLLISION")
    cs = o.collision
    cs.thickness_outer = max(0.001, fab["distance"] * 0.5)
    cs.thickness_inner = 0.02
    cs.cloth_friction = fab["friction"]
    cs.damping = 0.5
    return m


# -- main ---------------------------------------------------------------------------------------------------------------

def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    if len(argv) < 2:
        sys.exit("sew_cloth: SPEC.json OUT_DIR")
    spec_path, out = os.path.abspath(argv[0]), os.path.abspath(argv[1])
    os.makedirs(out, exist_ok=True)
    spec = json.load(open(spec_path))
    sdir = os.path.dirname(spec_path)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    sc.unit_settings.system = "METRIC"
    sc.unit_settings.scale_length = 1.0
    edge_mm = spec.get("edge", 0.015) * 1000.0
    fab0 = fabric(spec.get("fabric"))

    body, extra = load_collider(spec, sdir)
    bvh, bm_body = bvh_of([body] + extra)

    ranges = plan_ranges(spec, edge_mm)
    allco, alluv, tris, piece_of, owners, pins, snaps, shrinks = [], [], [], [], [], [], [], []
    seam_sides = {}
    gap_default = spec.get("gap", 0.01)
    for pi, p in enumerate(spec["pieces"]):
        uv, ptris, boundary, seams = build_piece(p, ranges[p["name"]], edge_mm)
        co, frame = place(uv, p["place"])
        if p["place"].get("conform") and frame is not None:
            n = conform(co, frame, bvh, p["place"]["conform"], p["place"].get("gap", gap_default),
                        p["place"].get("reach", 0.12 if p["place"]["type"] == "ribbon" else 1.5))
            log("%s: conform %s moved %d of %d vertices" % (p["name"], p["place"]["conform"], n, len(co)))
        base = len(allco)
        pin_m = np.zeros(len(uv), bool)
        snap_m = np.zeros(len(uv), bool)
        for reg in p.get("pin", []):
            m = region_mask(uv, reg)
            pin_m |= m
            if reg.get("snap"):
                snap_m |= m
                g = reg.get("gap", gap_default * 0.5)
                for k in np.nonzero(m)[0]:
                    loc, nrm, _, _ = bvh.find_nearest(Vector(co[k]))
                    if loc is not None:
                        co[k] = np.array(loc + nrm * g)
        shr = np.zeros(len(uv))
        for reg, factor in p.get("shrink", []):
            shr[region_mask(uv, reg)] = factor
        allco.extend(co.tolist())
        alluv.extend((uv / 1000.0).tolist())
        tris.extend([[base + a for a in t] for t in ptris])
        piece_of.extend([pi] * len(uv))
        pins.extend(pin_m.tolist())
        snaps.extend(snap_m.tolist())
        shrinks.extend(shr.tolist())
        for (si, side), ids in seams.items():
            seam_sides.setdefault(si, {})[side] = [base + i for i in ids]
        log("%s: %d vertices, %d triangles, %d boundary, %d pinned, %d seam sides"
            % (p["name"], len(uv), len(ptris), len(boundary), int(pin_m.sum()), len(seams)))
    allco = np.array(allco)
    # sewing springs: loose edges joining the seam sides one to one
    sew = []
    seam_pairs = {}
    for si, sm in enumerate(spec.get("seams", [])):
        a, b = seam_sides[si]["a"], seam_sides[si]["b"]
        if len(a) != len(b):
            sys.exit("sew_cloth: seam %d sides have %d and %d points" % (si, len(a), len(b)))
        flip = sm.get("flip", "auto")
        if flip == "auto":
            d0 = np.linalg.norm(allco[a] - allco[b], axis=1).sum()
            d1 = np.linalg.norm(allco[a] - allco[b[::-1]], axis=1).sum()
            flip = d1 < d0
        bb = b[::-1] if flip else b
        pairs = [(x, y) for x, y in zip(a, bb) if x != y]
        seam_pairs[si] = pairs
        d = np.linalg.norm(allco[[x for x, _ in pairs]] - allco[[y for _, y in pairs]], axis=1)
        log("seam %d %s-%s: %d springs, flip %s, start gap mean %.3f max %.3f m"
            % (si, sm["a"][0], sm["b"][0], len(pairs), flip, d.mean(), d.max()))
        sew.extend(pairs)

    me = bpy.data.meshes.new("garment")
    edges_loose = [list(e) for e in sew]
    me.from_pydata(allco.tolist(), edges_loose, tris)
    me.validate(clean_customdata=False)
    uvl = me.uv_layers.new(name="UVMap")
    lv = np.zeros(len(me.loops), dtype=np.int64)
    me.loops.foreach_get("vertex_index", lv)
    uvl.data.foreach_set("uv", np.array(alluv)[lv].ravel())
    gar = bpy.data.objects.new("garment", me)
    sc.collection.objects.link(gar)
    pa = me.attributes.new("piece", "INT", "POINT")
    pa.data.foreach_set("value", np.array(piece_of, dtype=np.int32))
    vg_pin = gar.vertex_groups.new(name="pin")
    vg_shr = gar.vertex_groups.new(name="shrink")
    pin_idx = [k for k, f in enumerate(pins) if f]
    if pin_idx:
        vg_pin.add(pin_idx, 1.0, "REPLACE")
    shr = np.array(shrinks)
    smax = float(shr.max()) if len(shr) else 0.0
    for k in np.nonzero(shr > 0)[0]:
        vg_shr.add([int(k)], float(shr[k] / smax), "REPLACE")

    # body collision
    for o in [body] + extra:
        add_collision(o, fab0)
    # the cloth (one fabric for the whole object: Blender's solver has one set; pieces' own fabrics set the mass and
    # stiffness by vertex group scaling where Blender allows it, else the spec's default wins)
    cl = gar.modifiers.new("cloth", "CLOTH")
    st = cl.settings
    st.quality = int(fab0["quality"])
    st.mass = fab0["mass"]
    st.air_damping = fab0["air"]
    st.tension_stiffness = fab0["tension"]
    st.compression_stiffness = fab0["compression"]
    st.shear_stiffness = fab0["shear"]
    st.bending_stiffness = fab0["bending"]
    # pieces of different fabrics in one cloth: Blender scales tension, shear and bending per vertex between the
    # setting (weight 0) and its "max" (weight 1), so each piece's own value goes in as a weight
    fabs = [fabric(spec.get("fabric"), p.get("fabric")) for p in spec["pieces"]]
    if any(p.get("fabric") for p in spec["pieces"]):
        piece_arr0 = np.array(piece_of)
        for key, grp, lo_attr, hi_attr in (("tension", "stiff_tension", "tension_stiffness", "tension_stiffness_max"),
                                           ("shear", "stiff_shear", "shear_stiffness", "shear_stiffness_max"),
                                           ("bending", "stiff_bending", "bending_stiffness", "bending_stiffness_max")):
            vals = [f[key] for f in fabs]
            lo, hi = min(vals), max(vals)
            setattr(st, lo_attr, lo)
            if key == "tension":
                st.compression_stiffness = min(f["compression"] for f in fabs)
                st.compression_stiffness_max = max(f["compression"] for f in fabs)
            if hi <= lo:
                continue
            setattr(st, hi_attr, hi)
            vg = gar.vertex_groups.new(name=grp)
            for pi, f in enumerate(fabs):
                w = (f[key] - lo) / (hi - lo)
                idx = np.nonzero(piece_arr0 == pi)[0].tolist()
                if idx and w > 0:
                    vg.add(idx, float(w), "REPLACE")
            if key == "tension":
                st.vertex_group_structural_stiffness = grp
            elif key == "shear":
                st.vertex_group_shear_stiffness = grp
            else:
                st.vertex_group_bending = grp
        log("per-piece fabric: " + ", ".join("%s %s/%s/%s" % (p["name"], f["tension"], f["shear"], f["bending"])
                                             for p, f in zip(spec["pieces"], fabs)))
    st.tension_damping = 5
    st.compression_damping = 5
    st.shear_damping = 5
    st.bending_model = "ANGULAR"
    st.use_sewing_springs = True
    st.sewing_force_max = fab0["sewing_force"]
    if pin_idx:
        st.vertex_group_mass = "pin"
        st.pin_stiffness = 1.0
    if smax > 0:
        st.shrink_min = 0.0
        st.shrink_max = smax
        st.vertex_group_shrink = "shrink"
    cs = cl.collision_settings
    cs.use_collision = True
    cs.distance_min = fab0["distance"]
    cs.collision_quality = 4
    cs.use_self_collision = bool(spec.get("self_collision", False))
    cs.self_distance_min = fab0["self_distance"]
    cs.self_friction = 5
    frames = int(spec.get("frames", 60))
    settle = int(spec.get("settle", 0))
    cache = cl.point_cache
    cache.frame_start = 1
    cache.frame_end = frames + settle
    sc.frame_start, sc.frame_end = 1, frames + settle
    log("cloth: %d vertices, %d triangles, %d sewing springs, %d pinned, shrink max %.2f, quality %d, frames %d+%d, "
        "self collision %s" % (len(me.vertices), len(tris), len(sew), len(pin_idx), smax, st.quality, frames, settle,
                               cs.use_self_collision))
    for f in range(1, frames + settle + 1):
        if f == frames + 1 and settle:
            st.use_sewing_springs = False  # the seams are shut; the springs would keep tugging the welded verts
        sc.frame_set(f)
        if f % 10 == 0 or f == 1:
            dg = bpy.context.evaluated_depsgraph_get()
            ev = gar.evaluated_get(dg).to_mesh()
            c = np.zeros(len(ev.vertices) * 3)
            ev.vertices.foreach_get("co", c)
            gar.evaluated_get(dg).to_mesh_clear()
            c = c.reshape(-1, 3)
            dsew = np.linalg.norm(c[[x for x, _ in sew]] - c[[y for _, y in sew]], axis=1) if sew else np.zeros(1)
            log("frame %d: z %.3f..%.3f, sewing gap mean %.4f max %.4f m" % (f, c[:, 2].min(), c[:, 2].max(),
                                                                          dsew.mean(), dsew.max()))
    dg = bpy.context.evaluated_depsgraph_get()
    ev = gar.evaluated_get(dg)
    final = bpy.data.meshes.new_from_object(ev)
    co = np.zeros(len(final.vertices) * 3)
    final.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    bpy.data.meshes.remove(final)
    if not np.isfinite(co).all():
        sys.exit("sew_cloth: the solver blew up (non-finite positions)")

    # report: seam gaps, vertices inside the body, edge stretch against the flat pattern
    report = {"seams": {}, "pieces": {}}
    for si, pairs in seam_pairs.items():
        d = np.linalg.norm(co[[x for x, _ in pairs]] - co[[y for _, y in pairs]], axis=1)
        report["seams"][si] = {"mean": float(d.mean()), "max": float(d.max())}
    # every vertex a spring joins (a corner where two seams meet joins three or four) moves to its group's mean, so
    # md_garment's weld closes the seams exactly
    parent = list(range(len(co)))

    def root(k):
        while parent[k] != k:
            parent[k] = parent[parent[k]]
            k = parent[k]
        return k
    for x, y in sew:
        rx, ry = root(x), root(y)
        if rx != ry:
            parent[rx] = ry
    groups = {}
    for x, y in sew:
        for k in (x, y):
            groups.setdefault(root(k), set()).add(k)
    for ks in groups.values():
        ks = list(ks)
        co[ks] = co[ks].mean(axis=0)
    uvm = np.array(alluv)
    tri = np.array(tris)
    e0, e1 = np.concatenate([tri[:, 0], tri[:, 1], tri[:, 2]]), np.concatenate([tri[:, 1], tri[:, 2], tri[:, 0]])
    rest = np.linalg.norm(uvm[e0] - uvm[e1], axis=1)
    now = np.linalg.norm(co[e0] - co[e1], axis=1)
    stretch = now / np.maximum(rest, 1e-6)
    piece_arr = np.array(piece_of)
    for pi, p in enumerate(spec["pieces"]):
        vm = piece_arr == pi
        inside = 0
        for k in np.nonzero(vm)[0]:
            loc, nrm, _, dist = bvh.find_nearest(Vector(co[k]))
            if loc is not None and (Vector(co[k]) - loc).dot(nrm) < -0.002:
                inside += 1
        em = piece_arr[e0] == pi
        report["pieces"][p["name"]] = {"verts": int(vm.sum()), "inside_body": inside,
                                       "stretch_p95": float(np.percentile(stretch[em], 95)),
                                       "stretch_max": float(stretch[em].max()),
                                       "z": [float(co[vm, 2].min()), float(co[vm, 2].max())]}
        log("%s: %d inside the body, stretch p95 %.3f max %.3f, z %.3f..%.3f"
            % (p["name"], inside, report["pieces"][p["name"]]["stretch_p95"], stretch[em].max(), co[vm, 2].min(),
               co[vm, 2].max()))
    for si, r in report["seams"].items():
        log("seam %s end gap mean %.4f max %.4f m" % (si, r["mean"], r["max"]))

    # the draped garment as a plain mesh (pieces split by group), then the OBJ a cloth tool would write
    gar.modifiers.clear()
    me.vertices.foreach_set("co", co.ravel())
    me.update()
    obj_path = os.path.join(out, "garment.obj")
    with open(obj_path, "w") as fh:
        fh.write("# sew_cloth.py: one group per pattern piece, metres, Z up, UVs in pattern metres\n")
        fh.write("mtllib garment.mtl\n")
        for v in co:
            fh.write("v %.6f %.6f %.6f\n" % tuple(v))
        for t in uvm:
            fh.write("vt %.6f %.6f\n" % tuple(t))
        for pi, p in enumerate(spec["pieces"]):
            fh.write("g %s\nusemtl %s\n" % (p["name"], p["name"]))
            for t in tri:
                if piece_arr[t[0]] == pi:
                    fh.write("f %d/%d %d/%d %d/%d\n" % (t[0] + 1, t[0] + 1, t[1] + 1, t[1] + 1, t[2] + 1, t[2] + 1))
    with open(os.path.join(out, "garment.mtl"), "w") as fh:
        for p in spec["pieces"]:
            fh.write("newmtl %s\nKd 0.8 0.8 0.8\n" % p["name"])
    # the body for md_garment's AO bake
    for o in bpy.data.objects:
        o.select_set(o is body)
    bpy.context.view_layer.objects.active = body
    body.modifiers.clear()
    bpy.ops.export_scene.gltf(filepath=os.path.join(out, "avatar.glb"), use_selection=True, export_format="GLB",
                              export_apply=True)
    for o in [body] + extra:
        o.modifiers.clear()
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "sewn.blend"))
    report["edge_mm"] = edge_mm
    report["frames"] = frames
    report["settle"] = settle
    json.dump(report, open(os.path.join(out, "sew_log.json"), "w"), indent=1)
    bm_body.free()
    log("WROTE", obj_path)


main()
