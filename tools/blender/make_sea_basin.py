"""The strip's wall basin under water: a modelled silt bed and its dressing (stones, weed, harbour debris, mooring
chains off the wall rings, and a shop mannequin's torso standing in the silt at the deep end), by script.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_sea_basin.py -- <out_dir>
            [--seed N] [--grid M] [--previews]

Written 2026-11-14 during a text-only hold: NOT YET RUN. Run it once, read its previews and pieces.json, and fix what
the first run shows before anything imports it.

The frame (every piece shares it, so the strip builder places them all with one transform and no rotation):
  * metres, Z up, Blender's X = the plan's x - 940 (the sea's edge, the wall's face), Blender's Y = the plan's y - 670.
    Z 0 is the datum (the sea plane stands at it). The bed runs from the wall's face (x 0) out to x 66 (plan 1006, past
    the buoy line at 1000), y -56..56 (plan 614..726), so the dive area (plan x 940..1000, y 620..720) is covered with
    a margin.
  * Blender's +Y comes into Unreal as -Y through glTF, and the plan's y turns over into the engine's the same way
    (engine y = side/2 - plan y), so a piece placed at the engine point (940 - side/2, side/2 - 670, 0) m with no
    rotation lands on its plan place. Check that with one still on the first import.
  * the bed: about 3 m deep at the wall's foot (a scour along it), falling to about 9.6 m at x 55 (plan 995) and
    nearly flat past it, with a slow swell, small lumps and silt ripples that fade with depth. bed_z() below is that
    surface; every piece is set on it.
  * material slots are named sb_<surface> (silt, stone, weed_blade, weed_leaf, rust, rubber, wood, hull, chain, rope,
    mannequin, mannequin_cap). The look materials are assigned in Unreal by slot name; the colours here are for the
    previews only. Weed cards are open strips (two-sided, alpha-tested in Unreal), UV v along the blade.
  * collision: the bed is its own (complex as simple); crates, the dinghy and the bicycle get simple boxes in Unreal
    at import; weed, chains, rope and the torso take none (the swimmer passes through weed; the rest are small).

Writes <out_dir>/basin_<group>.glb for group in bed, stones, weed, debris, chains, mannequin, and pieces.json (the
frame, each group's triangles and bounds, and the places of the debris and the torso in the plan frame), and with
--previews previews/*.png (Workbench: the basin from over the top; from the deep end looking back at the wall, as the
swimmer turning back sees the torso; the torso at 1 m and 2 m; the wall's foot at 1 m).
"""
import json
import math
import os
import random
import sys

import bpy
from mathutils import Euler, Matrix, Vector, noise

PLAN_ORIGIN = (940.0, 670.0)  # the plan point at the frame's origin (the wall's face, mid-basin)
BED_X = (-0.4, 66.0)           # the bed's extent in the frame (just under the wall's face to past the buoy line)
BED_Y = (-56.0, 56.0)
WALL_FOOT_DEPTH = 3.0
DEEP_DEPTH = 9.6
DEEP_X = 55.0                  # the deep end (plan 995)

SLOT_COLOURS = {
    "sb_silt": (0.32, 0.30, 0.25, 1.0),
    "sb_stone": (0.36, 0.36, 0.34, 1.0),
    "sb_weed_blade": (0.18, 0.26, 0.10, 1.0),
    "sb_weed_leaf": (0.22, 0.34, 0.12, 1.0),
    "sb_rust": (0.35, 0.18, 0.09, 1.0),
    "sb_rubber": (0.05, 0.05, 0.05, 1.0),
    "sb_wood": (0.30, 0.24, 0.16, 1.0),
    "sb_hull": (0.42, 0.48, 0.50, 1.0),
    "sb_chain": (0.26, 0.17, 0.10, 1.0),
    "sb_rope": (0.48, 0.44, 0.33, 1.0),
    "sb_mannequin": (0.70, 0.66, 0.60, 1.0),
    "sb_mannequin_cap": (0.25, 0.25, 0.26, 1.0),
}


# ----------------------------------------------------------------------------------------------- arguments
def args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not argv or argv[0].startswith("--"):
        raise SystemExit("make_sea_basin: usage: -- <out_dir> [--seed N] [--grid M] [--previews]")
    o = {"out": os.path.abspath(argv[0]), "seed": 1114, "grid": 0.25, "previews": False}
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--seed":
            o["seed"] = int(argv[i + 1])
            i += 2
        elif a == "--grid":
            o["grid"] = max(0.1, float(argv[i + 1]))
            i += 2
        elif a == "--previews":
            o["previews"] = True
            i += 1
        else:
            raise SystemExit("make_sea_basin: unknown argument %s" % a)
    return o


# ----------------------------------------------------------------------------------------------- the bed's surface
def smooth01(t):
    t = min(max(t, 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


def fbm(x, y, octaves, salt):
    total, amp, freq, norm = 0.0, 1.0, 1.0, 0.0
    for k in range(octaves):
        total += amp * noise.noise(Vector((x * freq, y * freq, salt + 3.7 * k)))
        norm += amp
        amp *= 0.5
        freq *= 2.03
    return total / norm


def bed_z(x, y):
    """The bed's height (m, datum 0) at the frame point (x, y)."""
    base = -WALL_FOOT_DEPTH - (DEEP_DEPTH - WALL_FOOT_DEPTH) * smooth01(x / DEEP_X)
    if x > DEEP_X:
        base -= 0.02 * (x - DEEP_X)
    scour = -0.35 * math.exp(-((x - 1.4) / 1.1) ** 2)       # the wash's trench along the wall's foot
    swell = 0.35 * fbm(x * 0.07, y * 0.07, 3, 11.0)          # slow lumps over tens of metres
    lumps = 0.06 * fbm(x * 0.6, y * 0.6, 3, 29.0)            # the silt's own unevenness
    shallow = 1.0 - smooth01((-base - WALL_FOOT_DEPTH) / 4.0)  # ripples where the wash reaches, gone by 7 m
    ang = 0.35 + 0.3 * fbm(x * 0.02, y * 0.02, 2, 47.0)
    phase = (y * math.cos(ang) + x * math.sin(ang)) * (2.0 * math.pi / 0.7) + 2.5 * fbm(x * 0.15, y * 0.15, 2, 53.0)
    ripples = 0.025 * shallow * math.sin(phase)
    return base + scour + swell + lumps + ripples


# ----------------------------------------------------------------------------------------------- building geometry
class Acc:
    """Vertices, faces, per-face slots, UVs and smoothing for one object, built from parts."""

    def __init__(self, name):
        self.name = name
        self.slots = []
        self.v = []
        self.f = []
        self.m = []
        self.uv = []
        self.smooth = []

    def add(self, verts, faces, slot, uvs=None, matrix=None, smooth=True):
        if slot not in self.slots:
            self.slots.append(slot)
        si = self.slots.index(slot)
        base = len(self.v)
        mtx = matrix if matrix is not None else Matrix.Identity(4)
        for p in verts:
            self.v.append(tuple(mtx @ Vector(p)))
        for k, face in enumerate(faces):
            self.f.append(tuple(base + i for i in face))
            self.m.append(si)
            self.uv.append(uvs[k] if uvs is not None else [(0.0, 0.0)] * len(face))
            self.smooth.append(smooth)

    def tris(self):
        return sum(len(face) - 2 for face in self.f)

    def build(self):
        me = bpy.data.meshes.new(self.name)
        me.from_pydata(self.v, [], self.f)
        uvl = me.uv_layers.new(name="UVMap")
        for poly, fuv in zip(me.polygons, self.uv):
            for j in range(poly.loop_total):
                uvl.data[poly.loop_start + j].uv = fuv[j]
        me.polygons.foreach_set("material_index", self.m)
        me.polygons.foreach_set("use_smooth", self.smooth)
        for slot in self.slots:
            me.materials.append(material(slot))
        me.validate(clean_customdata=False)
        me.update()
        ob = bpy.data.objects.new(self.name, me)
        bpy.context.scene.collection.objects.link(ob)
        return ob


def material(slot):
    mat = bpy.data.materials.get(slot)
    if mat is None:
        mat = bpy.data.materials.new(slot)
        mat.diffuse_color = SLOT_COLOURS.get(slot, (0.5, 0.5, 0.5, 1.0))
    return mat


def place(t, rot=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0)):
    return Matrix.Translation(Vector(t)) @ Euler(rot, "XYZ").to_matrix().to_4x4() @ Matrix.Diagonal((*scale, 1.0))


def box(sx, sy, sz):
    """A box centred on the origin; UVs in metres per face."""
    hx, hy, hz = sx / 2.0, sy / 2.0, sz / 2.0
    v = [(-hx, -hy, -hz), (hx, -hy, -hz), (hx, hy, -hz), (-hx, hy, -hz),
         (-hx, -hy, hz), (hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz)]
    f = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    uv = [[(0, 0), (0, sy), (sx, sy), (sx, 0)], [(0, 0), (sx, 0), (sx, sy), (0, sy)],
          [(0, 0), (sx, 0), (sx, sz), (0, sz)], [(0, 0), (sy, 0), (sy, sz), (0, sz)],
          [(0, 0), (sx, 0), (sx, sz), (0, sz)], [(0, 0), (sy, 0), (sy, sz), (0, sz)]]
    return v, f, uv


def tube(points, radius, sides=8, caps=True):
    """A tube swept along a polyline (parallel-transported rings). radius: a number or one per point."""
    pts = [Vector(p) for p in points]
    n = len(pts)
    radii = radius if isinstance(radius, (list, tuple)) else [radius] * n
    tangents = []
    for i in range(n):
        a = pts[max(i - 1, 0)]
        b = pts[min(i + 1, n - 1)]
        t = b - a
        tangents.append(t.normalized() if t.length > 1e-9 else Vector((0, 0, 1)))
    up = Vector((0, 0, 1)) if abs(tangents[0].z) < 0.9 else Vector((1, 0, 0))
    normal = (up - tangents[0] * up.dot(tangents[0])).normalized()
    v, f, uv = [], [], []
    length = 0.0
    lengths = []
    for i in range(n):
        if i > 0:
            length += (pts[i] - pts[i - 1]).length
            normal = (normal - tangents[i] * normal.dot(tangents[i]))
            normal = normal.normalized() if normal.length > 1e-9 else tangents[i].orthogonal().normalized()
        lengths.append(length)
        binormal = tangents[i].cross(normal)
        for s in range(sides):
            a = 2.0 * math.pi * s / sides
            v.append(tuple(pts[i] + radii[i] * (math.cos(a) * normal + math.sin(a) * binormal)))
    circ = 2.0 * math.pi * max(radii)
    for i in range(n - 1):
        for s in range(sides):
            s2 = (s + 1) % sides
            f.append((i * sides + s, i * sides + s2, (i + 1) * sides + s2, (i + 1) * sides + s))
            u0, u1 = circ * s / sides, circ * (s + 1) / sides
            uv.append([(u0, lengths[i]), (u1, lengths[i]), (u1, lengths[i + 1]), (u0, lengths[i + 1])])
    if caps:
        f.append(tuple(reversed(range(sides))))
        uv.append([(0.0, 0.0)] * sides)
        f.append(tuple((n - 1) * sides + s for s in range(sides)))
        uv.append([(0.0, 0.0)] * sides)
    return v, f, uv


def torus(major, minor, segs=12, sides=6, stretch=1.0):
    """A ring in the XY plane about the origin, its centre line stretched along X (a chain link at stretch > 1)."""
    v, f, uv = [], [], []
    for i in range(segs):
        a = 2.0 * math.pi * i / segs
        cx, cy = math.cos(a) * major * stretch, math.sin(a) * major
        out = Vector((math.cos(a), math.sin(a), 0.0))
        for j in range(sides):
            b = 2.0 * math.pi * j / sides
            p = Vector((cx, cy, 0.0)) + minor * (math.cos(b) * out + Vector((0, 0, math.sin(b))))
            v.append(tuple(p))
    for i in range(segs):
        i2 = (i + 1) % segs
        for j in range(sides):
            j2 = (j + 1) % sides
            f.append((i * sides + j, i2 * sides + j, i2 * sides + j2, i * sides + j2))
            uv.append([(i / segs, j / sides), ((i + 1) / segs, j / sides), ((i + 1) / segs, (j + 1) / sides),
                       (i / segs, (j + 1) / sides)])
    return v, f, uv


def loft(rings, cap_bottom=True, cap_top=True):
    """A closed surface through rings of equal vertex count (bottom to top)."""
    count = len(rings[0])
    v = [p for ring in rings for p in ring]
    f, uv = [], []
    height = 0.0
    hs = [0.0]
    for k in range(1, len(rings)):
        height += abs(rings[k][0][2] - rings[k - 1][0][2])
        hs.append(height)
    for k in range(len(rings) - 1):
        for s in range(count):
            s2 = (s + 1) % count
            f.append((k * count + s, k * count + s2, (k + 1) * count + s2, (k + 1) * count + s))
            uv.append([(s / count, hs[k]), ((s + 1) / count, hs[k]), ((s + 1) / count, hs[k + 1]), (s / count, hs[k + 1])])
    if cap_bottom:
        f.append(tuple(reversed(range(count))))
        uv.append([(0.0, 0.0)] * count)
    if cap_top:
        top = (len(rings) - 1) * count
        f.append(tuple(top + s for s in range(count)))
        uv.append([(0.0, 0.0)] * count)
    return v, f, uv


def rock(rng, rings=6, segs=10):
    """A lumpy unit stone (a sphere pushed in and out by noise)."""
    salt = rng.uniform(0.0, 100.0)
    v, f, uv = [], [], []
    v.append((0.0, 0.0, -1.0))
    for r in range(1, rings):
        lat = -math.pi / 2 + math.pi * r / rings
        for s in range(segs):
            lon = 2.0 * math.pi * s / segs
            d = Vector((math.cos(lat) * math.cos(lon), math.cos(lat) * math.sin(lon), math.sin(lat)))
            k = 0.82 + 0.3 * noise.noise(d * 1.7 + Vector((salt, 0, 0))) + 0.08 * noise.noise(d * 4.1 + Vector((0, salt, 0)))
            v.append(tuple(d * k))
    v.append((0.0, 0.0, 1.0))
    top = len(v) - 1
    for s in range(segs):
        s2 = (s + 1) % segs
        f.append((0, 1 + s2, 1 + s))
        uv.append([(0, 0), (1, 0), (0, 1)])
    for r in range(rings - 2):
        for s in range(segs):
            s2 = (s + 1) % segs
            a, b = 1 + r * segs, 1 + (r + 1) * segs
            f.append((a + s, a + s2, b + s2, b + s))
            uv.append([(s / segs, r / rings), ((s + 1) / segs, r / rings), ((s + 1) / segs, (r + 1) / rings),
                       (s / segs, (r + 1) / rings)])
    last = 1 + (rings - 2) * segs
    for s in range(segs):
        s2 = (s + 1) % segs
        f.append((last + s, last + s2, top))
        uv.append([(0, 0), (1, 0), (0, 1)])
    return v, f, uv


# ----------------------------------------------------------------------------------------------- the groups
def make_bed(grid):
    acc = Acc("basin_bed")
    nx = int(round((BED_X[1] - BED_X[0]) / grid)) + 1
    ny = int(round((BED_Y[1] - BED_Y[0]) / grid)) + 1
    verts = []
    for j in range(ny):
        y = BED_Y[0] + j * grid
        for i in range(nx):
            x = BED_X[0] + i * grid
            verts.append((x, y, bed_z(x, y)))
    faces, uvs = [], []
    for j in range(ny - 1):
        for i in range(nx - 1):
            a = j * nx + i
            faces.append((a, a + 1, a + 1 + nx, a + nx))
            x0, y0 = verts[a][0] / 2.0, verts[a][1] / 2.0
            uvs.append([(x0, y0), (x0 + grid / 2.0, y0), (x0 + grid / 2.0, y0 + grid / 2.0), (x0, y0 + grid / 2.0)])
    acc.add(verts, faces, "sb_silt", uvs)
    return acc


def make_stones(rng):
    """Rubble fallen off the wall along its foot, and stones scattered thinner out over the silt, a third sunk in."""
    acc = Acc("basin_stones")
    placed = 0
    tries = 0
    while placed < 110 and tries < 5000:
        tries += 1
        x = rng.uniform(0.25, 60.0)
        y = rng.uniform(-54.0, 54.0)
        weight = 1.0 if x < 4.0 else 0.15 + 0.35 * max(0.0, noise.noise(Vector((x * 0.08, y * 0.08, 71.0))))
        if rng.random() > weight:
            continue
        size = rng.uniform(0.08, 0.25) if rng.random() < 0.7 else rng.uniform(0.25, 0.6)
        if x < 4.0:
            size *= 1.3  # the wall's dressed blocks are the bigger pieces
        sx, sy, sz = size * rng.uniform(0.8, 1.2), size * rng.uniform(0.7, 1.1), size * rng.uniform(0.5, 0.8)
        z = bed_z(x, y) + sz * 0.3
        v, f, uv = rock(rng)
        acc.add(v, f, "sb_stone", uv, place((x, y, z), (rng.uniform(-0.2, 0.2), rng.uniform(-0.2, 0.2), rng.uniform(0, 6.28)), (sx, sy, sz)))
        placed += 1
    return acc


def weed_blade(rng, length, width, segs=7):
    """A strap blade (an open card): from its holdfast up, leaning and swaying, narrowing to its tip."""
    lean = rng.uniform(0.15, 0.5)
    sway = rng.uniform(-0.25, 0.25)
    v, f, uv = [], [], []
    for k in range(segs + 1):
        t = k / segs
        cx = lean * length * t * t
        cy = sway * length * math.sin(t * math.pi)
        cz = length * t * (1.0 - 0.25 * lean * t)
        w = width * (0.35 + 0.65 * math.sin(min(t * 1.25, 1.0) * math.pi * 0.5)) * (1.0 - 0.75 * t ** 3)
        twist = 0.9 * t
        dx, dy = -math.sin(twist) * w / 2.0, math.cos(twist) * w / 2.0
        v.append((cx - dx, cy - dy, cz))
        v.append((cx + dx, cy + dy, cz))
    for k in range(segs):
        a = 2 * k
        f.append((a, a + 1, a + 3, a + 2))
        uv.append([(0.0, k / segs), (1.0, k / segs), (1.0, (k + 1) / segs), (0.0, (k + 1) / segs)])
    return v, f, uv


def weed_leaf(rng, size):
    """A ruffled leaf (sea lettuce) lying over the silt: a patch whose edge waves up and down."""
    rings, segs = 4, 14
    v, f, uv = [(0.0, 0.0, 0.0)], [], []
    for r in range(1, rings + 1):
        rr = size * r / rings
        for s in range(segs):
            a = 2.0 * math.pi * s / segs
            ruffle = 0.18 * size * (r / rings) ** 2 * math.sin(a * rng.choice((5, 6, 7)) + rng.uniform(0, 1))
            v.append((rr * math.cos(a) * (1.0 + 0.2 * math.sin(2 * a)), rr * math.sin(a), 0.04 * size * r + ruffle))
    for s in range(segs):
        s2 = (s + 1) % segs
        f.append((0, 1 + s, 1 + s2))
        uv.append([(0.5, 0.5), (0.5 + 0.5 * math.cos(2 * math.pi * s / segs) / rings, 0.5 + 0.5 * math.sin(2 * math.pi * s / segs) / rings),
                   (0.5 + 0.5 * math.cos(2 * math.pi * (s + 1) / segs) / rings, 0.5 + 0.5 * math.sin(2 * math.pi * (s + 1) / segs) / rings)])
    for r in range(rings - 1):
        for s in range(segs):
            s2 = (s + 1) % segs
            a, b = 1 + r * segs, 1 + (r + 1) * segs
            f.append((a + s, b + s, b + s2, a + s2))
            ru, rv = (r + 1) / rings, (r + 2) / rings
            ca, sa = math.cos(2 * math.pi * s / segs), math.sin(2 * math.pi * s / segs)
            cb, sb = math.cos(2 * math.pi * (s + 1) / segs), math.sin(2 * math.pi * (s + 1) / segs)
            uv.append([(0.5 + 0.5 * ru * ca, 0.5 + 0.5 * ru * sa), (0.5 + 0.5 * rv * ca, 0.5 + 0.5 * rv * sa),
                       (0.5 + 0.5 * rv * cb, 0.5 + 0.5 * rv * sb), (0.5 + 0.5 * ru * cb, 0.5 + 0.5 * ru * sb)])
    return v, f, uv


def make_weed(rng):
    """Weed beds where the light still reaches (3..6.5 m), thickest along the wall's foot and on the rubble; leaves
    lying between them; a few thin tufts deeper."""
    acc = Acc("basin_weed")
    tufts = 0
    tries = 0
    while tufts < 70 and tries < 4000:
        tries += 1
        x = rng.uniform(0.3, 40.0)
        y = rng.uniform(-54.0, 54.0)
        depth = -bed_z(x, y)
        bed = max(0.0, noise.noise(Vector((x * 0.12, y * 0.12, 91.0)))) + (0.5 if x < 3.0 else 0.0)
        light = 1.0 - smooth01((depth - 5.0) / 2.5)
        if rng.random() > bed * light:
            continue
        z = bed_z(x, y) - 0.02
        for _ in range(rng.randint(5, 12)):
            ox, oy = rng.uniform(-0.15, 0.15), rng.uniform(-0.15, 0.15)
            length = rng.uniform(0.4, 1.6) * light + 0.2
            v, f, uv = weed_blade(rng, length, rng.uniform(0.05, 0.12))
            acc.add(v, f, "sb_weed_blade", uv, place((x + ox, y + oy, z), (0.0, 0.0, rng.uniform(0, 6.28))))
        tufts += 1
    for _ in range(45):
        x = rng.uniform(1.0, 35.0)
        y = rng.uniform(-50.0, 50.0)
        v, f, uv = weed_leaf(rng, rng.uniform(0.12, 0.3))
        acc.add(v, f, "sb_weed_leaf", uv, place((x, y, bed_z(x, y) + 0.01), (rng.uniform(-0.1, 0.1), rng.uniform(-0.1, 0.1), rng.uniform(0, 6.28))))
    return acc, tufts


def bicycle(acc, matrix):
    """A town bicycle (diamond frame, 26 in wheels, a basket bracket), built upright at the origin facing +X."""
    rim, tyre = 0.31, 0.33
    for hub in ((-0.52, 0.0, tyre), (0.55, 0.0, tyre)):
        hubm = matrix @ place(hub, (math.pi / 2, 0.0, 0.0))
        v, f, uv = torus(tyre, 0.022, 32, 8)
        acc.add(v, f, "sb_rubber", uv, hubm)
        v, f, uv = torus(rim, 0.011, 32, 6)
        acc.add(v, f, "sb_rust", uv, hubm)
        v, f, uv = tube([(0, 0, -0.05), (0, 0, 0.05)], 0.02, 8)
        acc.add(v, f, "sb_rust", uv, hubm)
        for s in range(18):
            a = 2.0 * math.pi * s / 18
            side = 0.03 if s % 2 == 0 else -0.03
            v, f, uv = tube([(0, 0, side), (rim * math.cos(a), rim * math.sin(a), 0.0)], 0.0018, 4, caps=False)
            acc.add(v, f, "sb_rust", uv, hubm)
    bb, seat, head_top, head_bot = (0.0, 0.0, 0.29), (-0.13, 0.0, 0.80), (0.43, 0.0, 0.86), (0.46, 0.0, 0.74)
    rear, front = (-0.52, 0.0, tyre), (0.55, 0.0, tyre)
    for a, b in ((bb, seat), (bb, head_bot), (seat, head_top), (head_bot, head_top)):
        v, f, uv = tube([a, b], 0.017, 8)
        acc.add(v, f, "sb_rust", uv, matrix)
    for side in (-0.05, 0.05):
        for a, b in (((0.0, side * 0.6, 0.29), (rear[0], side, rear[2])), ((seat[0], side * 0.4, seat[2]), (rear[0], side, rear[2])),
                     ((head_bot[0], side * 0.5, head_bot[2]), (front[0], side, front[2]))):
            v, f, uv = tube([a, b], 0.011, 6)
            acc.add(v, f, "sb_rust", uv, matrix)
    # Stem, bars, post and saddle, cranks and pedals.
    v, f, uv = tube([head_top, (0.41, 0.0, 0.98)], 0.012, 6)
    acc.add(v, f, "sb_rust", uv, matrix)
    v, f, uv = tube([(0.36, -0.27, 1.0), (0.41, -0.2, 0.98), (0.41, 0.2, 0.98), (0.36, 0.27, 1.0)], 0.011, 6)
    acc.add(v, f, "sb_rust", uv, matrix)
    for grip in (-0.27, 0.27):
        v, f, uv = tube([(0.36, grip, 1.0), (0.33, grip * 1.08, 1.0)], 0.016, 8)
        acc.add(v, f, "sb_rubber", uv, matrix)
    v, f, uv = tube([seat, (-0.16, 0.0, 0.93)], 0.012, 6)
    acc.add(v, f, "sb_rust", uv, matrix)
    v, f, uv = box(0.26, 0.15, 0.05)
    acc.add(v, f, "sb_rubber", uv, matrix @ place((-0.15, 0.0, 0.96), (0.0, -0.06, 0.0)), smooth=False)
    for side, ang in ((-1, 0.6), (1, 0.6 + math.pi)):
        end = (0.17 * math.cos(ang), 0.07 * side, 0.29 + 0.17 * math.sin(ang))
        v, f, uv = tube([(0.0, 0.07 * side, 0.29), end], 0.01, 6)
        acc.add(v, f, "sb_rust", uv, matrix)
        v, f, uv = box(0.1, 0.07, 0.025)
        acc.add(v, f, "sb_rubber", uv, matrix @ place((end[0], end[1] + 0.04 * side, end[2])), smooth=False)
    # A front basket's bent bracket, its basket long gone.
    v, f, uv = tube([(0.44, -0.12, 0.92), (0.62, -0.12, 0.88), (0.62, 0.12, 0.88), (0.44, 0.12, 0.92)], 0.006, 5)
    acc.add(v, f, "sb_rust", uv, matrix)


def crate(rng, acc, matrix, broken=False):
    """A slatted fish crate, 0.6 x 0.4 x 0.32 m: three slats a side on four corner posts, a slatted floor."""
    sx, sy, sz = 0.6, 0.4, 0.32
    post = 0.035
    for cx in (-sx / 2 + post / 2, sx / 2 - post / 2):
        for cy in (-sy / 2 + post / 2, sy / 2 - post / 2):
            v, f, uv = box(post, post, sz)
            acc.add(v, f, "sb_wood", uv, matrix @ place((cx, cy, sz / 2)), smooth=False)
    slat_h, slat_t = 0.085, 0.014
    for k in range(3):
        z = 0.03 + k * (slat_h + 0.02) + slat_h / 2
        for side in (-1, 1):
            if broken and side == 1 and k > 0:
                continue  # the slats on that side broken out
            v, f, uv = box(sx, slat_t, slat_h)
            acc.add(v, f, "sb_wood", uv, matrix @ place((0.0, side * (sy / 2 - slat_t / 2), z)), smooth=False)
            if broken and side == -1 and k == 2:
                continue
            v, f, uv = box(slat_t, sy - 2 * slat_t, slat_h)
            acc.add(v, f, "sb_wood", uv, matrix @ place((side * (sx / 2 - slat_t / 2), 0.0, z)), smooth=False)
    for k in range(4):
        y = -sy / 2 + 0.06 + k * (sy - 0.12) / 3
        v, f, uv = box(sx, 0.07, slat_t)
        acc.add(v, f, "sb_wood", uv, matrix @ place((0.0, y, slat_t / 2)), smooth=False)
    if broken:
        # One slat lying loose beside it.
        v, f, uv = box(sx, slat_t, slat_h)
        acc.add(v, f, "sb_wood", uv, matrix @ place((0.15, sy / 2 + 0.25, 0.01), (math.pi / 2, 0.0, rng.uniform(0.3, 0.9))), smooth=False)


def dinghy(acc, matrix):
    """A sunk rowing dinghy, 3.0 m: an open clinker-ish hull with thickness, a transom and two thwarts."""
    stations, sides = 22, 14
    length, beam, depth, thick = 3.0, 1.3, 0.5, 0.03
    outer, inner = [], []
    for k in range(stations):
        t = k / (stations - 1)
        x = -length / 2 + length * t
        q = (x / (length / 2))
        half = beam / 2 * max(0.02, (1.0 - max(q, 0.0) ** 2) ** 0.65) * (0.72 + 0.28 * min(1.0, (q + 1.0) * 1.6))
        sheer = depth + 0.14 * max(q, 0.0) ** 2
        ring_o, ring_i = [], []
        for s in range(sides):
            a = -math.pi / 2 + math.pi * s / (sides - 1)
            ring_o.append((x, half * math.sin(a), sheer - sheer * math.cos(a)))
            ring_i.append((x, (half - thick) * math.sin(a), sheer - (sheer - thick) * math.cos(a) + (0.0 if k else thick)))
        outer.append(ring_o)
        inner.append(ring_i)
    v, f, uv = [], [], []
    for ring in outer + inner:
        v.extend(ring)
    off = stations * sides
    for k in range(stations - 1):
        for s in range(sides - 1):
            a, b = k * sides + s, (k + 1) * sides + s
            f.append((a, b, b + 1, a + 1))
            uv.append([(k * 0.14, s * 0.1), ((k + 1) * 0.14, s * 0.1), ((k + 1) * 0.14, (s + 1) * 0.1), (k * 0.14, (s + 1) * 0.1)])
            f.append((off + a, off + a + 1, off + b + 1, off + b))
            uv.append([(k * 0.14, s * 0.1), (k * 0.14, (s + 1) * 0.1), ((k + 1) * 0.14, (s + 1) * 0.1), ((k + 1) * 0.14, s * 0.1)])
        # The gunwales: the rim between the outer and the inner skin on both sides.
        for s in (0, sides - 1):
            a, b = k * sides + s, (k + 1) * sides + s
            f.append((a, off + a, off + b, b) if s == 0 else (a, b, off + b, off + a))
            uv.append([(k * 0.14, 0), (k * 0.14, 0.03), ((k + 1) * 0.14, 0.03), ((k + 1) * 0.14, 0)])
    # The transom (stern) and the stem: the outer skin's end rings closed, the inner joined to them.
    for k in (0, stations - 1):
        for s in range(sides - 1):
            a = k * sides + s
            f.append((a, a + 1, off + a + 1, off + a) if k == 0 else (a, off + a, off + a + 1, a + 1))
            uv.append([(0, s * 0.1), (0, (s + 1) * 0.1), (0.03, (s + 1) * 0.1), (0.03, s * 0.1)])
    acc.add(v, f, "sb_hull", uv, matrix)
    for x in (-0.55, 0.45):
        v2, f2, uv2 = box(0.2, beam * 0.78, 0.025)
        acc.add(v2, f2, "sb_wood", uv2, matrix @ place((x, 0.0, depth - 0.16)), smooth=False)
    v2, f2, uv2 = box(0.04, beam * 0.62, 0.36)
    acc.add(v2, f2, "sb_wood", uv2, matrix @ place((-length / 2 + 0.02, 0.0, depth - 0.2)), smooth=False)


def make_debris(rng, notes):
    """The harbour's history on the bed: a bicycle on its side, a stack of fish crates (one broken), a sunk dinghy."""
    acc = Acc("basin_debris")
    # The bicycle, lying on its side where it was thrown off the quay, half in the silt.
    bx, by = 6.5, -18.0
    bicycle(acc, place((bx, by, bed_z(bx, by) + 0.06), (1.42, 0.05, 2.2)))
    notes["bicycle"] = plan_point(bx, by)
    # Crates: two side by side, one leaning on them, one broken out a little way off.
    cx, cy = 11.0, 9.0
    cz = bed_z(cx, cy)
    crate(rng, acc, place((cx, cy, cz - 0.08), (0.03, -0.02, 0.4)))
    crate(rng, acc, place((cx + 0.55, cy + 0.35, cz - 0.05), (-0.02, 0.04, 0.55)))
    crate(rng, acc, place((cx + 0.2, cy + 0.15, cz + 0.22), (0.45, 0.1, 0.48)))
    crate(rng, acc, place((cx + 2.6, cy - 1.4, bed_z(cx + 2.6, cy - 1.4) - 0.12), (0.12, -0.3, 1.9)), broken=True)
    notes["crates"] = plan_point(cx, cy)
    # The dinghy, on its side in the deeper silt, sunk a quarter of its depth.
    dx, dy = 31.0, 19.0
    dinghy(acc, place((dx, dy, bed_z(dx, dy) - 0.12), (0.42, -0.05, 2.7)))
    notes["dinghy"] = plan_point(dx, dy)
    return acc


def chain_path(ring, rng):
    """A mooring chain's line: from the ring down the wall to the bed, then trailing out over it with a slack pile."""
    rx, ry, rz = ring
    pts = []
    foot = (1.0 + rng.uniform(0.0, 0.8), ry + rng.uniform(-0.4, 0.4))
    footz = bed_z(*foot) + 0.03
    for k in range(13):
        t = k / 12
        sag = t ** 1.6
        x = rx + (foot[0] - rx) * sag
        y = ry + (foot[1] - ry) * t
        z = rz + (footz - rz) * (t ** 0.85)
        pts.append((x, y, z))
    heading = rng.uniform(-0.6, 0.6)
    x, y = foot
    for k in range(1, 30):
        heading += rng.uniform(-0.35, 0.35)
        x += 0.17 * math.cos(heading)
        y += 0.17 * math.sin(heading)
        pts.append((x, y, bed_z(x, y) + 0.03))
    return pts


def links_along(acc, pts, pitch=0.105):
    """Chain links laid along a polyline every pitch metres, each turned a quarter about the line from the last."""
    v, f, uv = torus(0.042, 0.011, 12, 6, stretch=1.45)
    seg_i, along, count = 0, 0.0, 0
    p = [Vector(q) for q in pts]
    while seg_i < len(p) - 1:
        a, b = p[seg_i], p[seg_i + 1]
        seg = (b - a).length
        if along > seg:
            along -= seg
            seg_i += 1
            continue
        d = (b - a).normalized() if seg > 1e-9 else Vector((1, 0, 0))
        at = a + d * along
        rot = d.to_track_quat("X", "Z").to_matrix().to_4x4()
        roll = Matrix.Rotation(math.pi / 2 if count % 2 else 0.0, 4, "X")
        acc.add(v, f, "sb_chain", uv, Matrix.Translation(at) @ rot @ roll)
        along += pitch
        count += 1
    return count


def make_chains(rng, notes):
    """Mooring rings on the wall's face at the waterline, each with its chain hanging down and trailing over the bed."""
    acc = Acc("basin_chains")
    rings = []
    for y in (-31.0, -9.0, 13.0, 35.0):
        ring = (0.12, y, 0.45)
        # The eye plate on the wall, the ring hanging from it in the plane square to the wall.
        v, f, uv = box(0.03, 0.14, 0.18)
        acc.add(v, f, "sb_chain", uv, place((0.015, y, 0.62)), smooth=False)
        v, f, uv = torus(0.085, 0.014, 16, 6)
        acc.add(v, f, "sb_chain", uv, place((0.09, y, 0.52), (math.pi / 2, 0.0, math.pi / 2)))
        n = links_along(acc, chain_path(ring, rng))
        rings.append({"plan": plan_point(0.0, y), "links": n})
    notes["chains"] = rings
    return acc


def torso_profile(z):
    """The mannequin torso's section at height z (m over the hip cut): (half-width, front depth, back depth)."""
    table = [(0.00, 0.170, 0.110, 0.120), (0.10, 0.175, 0.115, 0.125), (0.212, 0.140, 0.097, 0.103),
             (0.30, 0.130, 0.095, 0.096), (0.45, 0.155, 0.115, 0.100), (0.55, 0.170, 0.125, 0.105),
             (0.63, 0.185, 0.115, 0.095), (0.69, 0.195, 0.090, 0.085), (0.73, 0.140, 0.070, 0.070),
             (0.76, 0.055, 0.050, 0.050), (0.84, 0.050, 0.045, 0.045)]
    if z <= table[0][0]:
        return table[0][1:]
    for (z0, *a), (z1, *b) in zip(table, table[1:]):
        if z <= z1:
            t = (z - z0) / (z1 - z0)
            return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))
    return table[-1][1:]


def torso_point(z, phi, inset=0.0):
    w, front, back = torso_profile(z)
    c = math.cos(phi)
    depth = front if c < 0.0 else back  # the front faces -X (the wall)
    return (c * (depth - inset), math.sin(phi) * (w - inset), z)


def make_mannequin(rng, notes):
    """A shop mannequin's torso standing upright in the silt at the deep end, facing the wall, half-wrapped in mooring
    line. It reads as a mannequin only from close: the waist seam, the flat arm and neck caps, the stand's socket."""
    acc = Acc("basin_mannequin")
    tx, ty = 47.0, 2.0  # plan (987, 672): 6-8 m in from where the swimmer is turned back at the deep end
    tz = bed_z(tx, ty) - 0.12  # the hip cut sunk in the silt
    m = place((tx, ty, tz), (rng.uniform(-0.03, 0.03), rng.uniform(-0.05, -0.02), rng.uniform(-0.12, 0.12)))
    segs = 32
    heights = [0.0, 0.05, 0.10, 0.16, 0.205, 0.209, 0.215, 0.219, 0.25, 0.30, 0.38, 0.45, 0.50, 0.55, 0.60, 0.63,
               0.66, 0.69, 0.71, 0.73, 0.745, 0.76, 0.80, 0.84]
    rings = []
    for z in heights:
        inset = 0.005 if 0.208 < z < 0.217 else 0.0  # the waist seam: a groove 5 mm deep, 6 mm wide
        rings.append([torso_point(z, 2.0 * math.pi * s / segs, inset) for s in range(segs)])
    v, f, uv = loft(rings)
    acc.add(v, f, "sb_mannequin", uv, m)
    # The arm caps (flat discs where the arms came off) and the neck cap.
    for side in (-1, 1):
        w, _, _ = torso_profile(0.655)
        v, f, uv = tube([(0.0, side * (w - 0.012), 0.655), (0.0, side * (w + 0.004), 0.655)], 0.052, 16)
        acc.add(v, f, "sb_mannequin_cap", uv, m)
    v, f, uv = tube([(0.0, 0.0, 0.835), (0.0, 0.0, 0.85)], 0.04, 16)
    acc.add(v, f, "sb_mannequin_cap", uv, m)
    # The stand: a socket plate under the hips and its pole stub down into the silt.
    v, f, uv = tube([(0.0, 0.0, -0.012), (0.0, 0.0, 0.002)], 0.055, 16)
    acc.add(v, f, "sb_mannequin_cap", uv, m)
    v, f, uv = tube([(0.0, 0.0, -0.5), (0.0, 0.0, -0.01)], 0.016, 10)
    acc.add(v, f, "sb_chain", uv, m)
    # Mooring line wound round its lower half (2.25 turns, hip to under the chest), its free end down to the bed
    # and trailing off, the other end a short loose tail.
    rope_r = 0.012
    helix = []
    turns, z0, z1 = 2.25, 0.06, 0.44
    steps = 120
    for k in range(steps + 1):
        t = k / steps
        z = z0 + (z1 - z0) * t
        phi = 2.0 * math.pi * turns * t + 0.4
        x, y, _ = torso_point(z, phi)
        r = math.hypot(x, y)
        scale = (r + rope_r * 0.8) / r if r > 1e-6 else 1.0
        helix.append((x * scale, y * scale, z + 0.01 * math.sin(phi * 3.0)))
    world = [tuple(m @ Vector(p)) for p in helix]
    tail = [world[0]]
    x, y = world[0][0], world[0][1]
    heading = math.atan2(world[0][1] - ty, world[0][0] - tx)
    for k in range(1, 20):
        heading += rng.uniform(-0.3, 0.3)
        x += 0.16 * math.cos(heading)
        y += 0.16 * math.sin(heading)
        tail.append((x, y, bed_z(x, y) + rope_r if k > 2 else world[0][2] - 0.08 * k))
    v, f, uv = tube(list(reversed(tail)) + world[1:], rope_r, 8)
    acc.add(v, f, "sb_rope", uv)
    end = world[-1]
    loose = [end, (end[0] + 0.05, end[1] + 0.06, end[2] + 0.09), (end[0] + 0.07, end[1] + 0.13, end[2] + 0.16)]
    v, f, uv = tube(loose, rope_r, 8)
    acc.add(v, f, "sb_rope", uv)
    notes["mannequin"] = {"plan": plan_point(tx, ty), "bed_z": round(tz, 2), "faces": "the wall (-x)", "height_m": 0.85}
    return acc


def plan_point(x, y):
    return [round(PLAN_ORIGIN[0] + x, 2), round(PLAN_ORIGIN[1] + y, 2)]


# ----------------------------------------------------------------------------------------------- output
def export(ob, path):
    bpy.ops.object.select_all(action="DESELECT")
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True, export_yup=True, export_apply=False,
                              export_normals=True, export_tangents=False, export_materials="EXPORT",
                              export_vertex_color="NONE", export_animations=False, export_skins=False, export_morph=False)


def bounds(ob):
    xs = [v.co.x for v in ob.data.vertices]
    ys = [v.co.y for v in ob.data.vertices]
    zs = [v.co.z for v in ob.data.vertices]
    return [[round(min(xs), 2), round(min(ys), 2), round(min(zs), 2)], [round(max(xs), 2), round(max(ys), 2), round(max(zs), 2)]]


def render_previews(out):
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.render.resolution_x, scene.render.resolution_y = 1280, 720
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "MATERIAL"
    world = bpy.data.worlds.new("basin_preview")
    world.color = (0.05, 0.12, 0.13)
    scene.world = world
    cam_data = bpy.data.cameras.new("basin_cam")
    cam = bpy.data.objects.new("basin_cam", cam_data)
    scene.collection.objects.link(cam)
    scene.camera = cam
    os.makedirs(os.path.join(out, "previews"), exist_ok=True)

    def shot(name, eye, at, lens=24.0, ortho=0.0):
        cam_data.type = "ORTHO" if ortho > 0.0 else "PERSP"
        cam_data.ortho_scale = ortho if ortho > 0.0 else cam_data.ortho_scale
        cam_data.lens = lens
        cam_data.clip_start, cam_data.clip_end = 0.05, 400.0
        cam.location = Vector(eye)
        cam.rotation_euler = (Vector(at) - Vector(eye)).to_track_quat("-Z", "Y").to_euler()
        scene.render.filepath = os.path.join(out, "previews", name + ".png")
        bpy.ops.render.render(write_still=True)

    mid = (33.0, 0.0, -6.0)
    shot("basin_top", (33.0, 0.001, 60.0), mid, ortho=120.0)
    shot("deep_end_back", (54.0, 2.5, -6.5), (40.0, 2.0, -7.5), lens=20.0)  # turning back from the deep end
    tz = bed_z(47.0, 2.0)
    shot("mannequin_2m", (45.0, 2.0, tz + 0.6), (47.0, 2.0, tz + 0.4), lens=28.0)
    shot("mannequin_1m", (46.0, 2.3, tz + 0.5), (47.0, 2.0, tz + 0.3), lens=28.0)
    shot("wall_foot_1m", (1.6, -9.0, bed_z(1.6, -9.0) + 0.9), (0.4, -9.0, bed_z(0.4, -9.0) + 0.2), lens=24.0)


def main():
    o = args()
    os.makedirs(o["out"], exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    noise.seed_set(o["seed"])
    rng = random.Random(o["seed"])
    notes = {}
    groups = [("bed", make_bed(o["grid"]))]
    groups.append(("stones", make_stones(rng)))
    weed, tufts = make_weed(rng)
    groups.append(("weed", weed))
    groups.append(("debris", make_debris(rng, notes)))
    groups.append(("chains", make_chains(rng, notes)))
    groups.append(("mannequin", make_mannequin(rng, notes)))
    report = {
        "frame": {"units": "m", "up": "Z", "plan_origin": list(PLAN_ORIGIN),
                  "engine_place": "(940 - side/2, side/2 - 670, 0) m, no rotation", "seed": o["seed"], "grid_m": o["grid"]},
        "bed": {"wall_foot_depth_m": WALL_FOOT_DEPTH, "deep_depth_m": DEEP_DEPTH, "deep_plan_x": PLAN_ORIGIN[0] + DEEP_X,
                "samples": {"wall_foot": round(bed_z(0.5, 0.0), 2), "x960": round(bed_z(20.0, 0.0), 2),
                            "x980": round(bed_z(40.0, 0.0), 2), "x995": round(bed_z(55.0, 0.0), 2)}},
        "weed_tufts": tufts,
        "places": notes,
        "groups": {},
    }
    for name, acc in groups:
        ob = acc.build()
        path = os.path.join(o["out"], "basin_%s.glb" % name)
        export(ob, path)
        report["groups"][name] = {"file": os.path.basename(path), "triangles": acc.tris(), "slots": acc.slots, "bounds": bounds(ob)}
        print("make_sea_basin: %s, %d triangles, slots %s" % (path, acc.tris(), ",".join(acc.slots)))
    with open(os.path.join(o["out"], "pieces.json"), "w", encoding="utf-8") as fh:
        json.dump(report, fh, indent=2)
    if o["previews"]:
        render_previews(o["out"])
    print("make_sea_basin: done, %d groups in %s" % (len(groups), o["out"]))


main()
