"""The strip's wall basin under water: a modelled silt bed and its dressing (stones, weed, harbour debris, mooring
chains off the wall rings, and a shop mannequin's torso standing in the silt at the deep end), by script.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_sea_basin.py -- <out_dir>
            [--seed N] [--grid M] [--deck M] [--previews]

Behind the wall's foot, the boat cellar (make_cellar): the one location there reached only by diving. Arch A in the
wall's face, a vaulted passage west under the quay, a chamber under the clock square whose vault stands over the sea's
level (the basin's one air pocket, a street drain at its crown, a stair up to a locked grate, a boat's remains on
trestles, a find on a ledge), and a bent passage back out through breach B. --deck is the street's height over the
datum (default 2.0 m, to be measured); the vault's crown is 0.45 m under it. The strip builder cuts the wall and the
paving where pieces.json says.

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
  * collision: the bed and the cellar's shell are their own (complex as simple); crates, the dinghy and the bicycle get simple boxes in Unreal
    at import; weed, chains, rope and the torso take none (the swimmer passes through weed; the rest are small).

Writes <out_dir>/basin_<group>.glb for group in bed, stones, weed, debris, chains, mannequin, cellar, grates (the
street's two, for the paving's cuts), find (the ledge and its box, for an interact actor), and pieces.json (the
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
    "sb_masonry": (0.30, 0.31, 0.29, 1.0),
    "sb_keystone": (0.62, 0.60, 0.55, 1.0),
    "sb_metal": (0.20, 0.17, 0.14, 1.0),
    "sb_find": (0.45, 0.40, 0.30, 1.0),
}


# ----------------------------------------------------------------------------------------------- arguments
def args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not argv or argv[0].startswith("--"):
        raise SystemExit("make_sea_basin: usage: -- <out_dir> [--seed N] [--grid M] [--previews]")
    o = {"out": os.path.abspath(argv[0]), "seed": 1114, "grid": 0.25, "previews": False, "deck": 2.0}
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--deck":
            o["deck"] = float(argv[i + 1])
            if o["deck"] < 1.45:
                raise SystemExit("make_sea_basin: --deck %.2f leaves the cellar no air pocket (needs 1.45 m or more)" % o["deck"])
            i += 2
        elif a == "--seed":
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


# ----------------------------------------------------------------------------------------------- the boat cellar
# The location behind the wall's foot, reached only by diving (UNDERWATER_PLAN section 8): arch A in the wall's face,
# passage A west under the quay, the chamber under the clock square (its vault over the sea's level: the basin's one air
# pocket, with a street drain at its crown and a stair up to a locked grate), passage B bending back to breach B in the
# wall. Shell faces point inward. Every surface is set from the frame's numbers here; the deck (the street's height over
# the datum) is --deck, measured in step 1.
CH_X = (-26.0, -12.0)        # the chamber (plan x 914..928)
CH_Y = (-22.0, -10.0)        # (plan y 648..660)
CH_FLOOR = -3.2
CH_SPRING = 0.4              # the vault springs from the long walls here
A_Y, A_WIDTH, A_CROWN, A_FLOOR = -18.0, 2.2, -0.9, -3.0
B_PATH = [(-14.0, -10.0), (-14.0, -6.0), (-6.0, 0.0), (-3.0, 6.5), (0.3, 6.5)]
B_WIDTH, B_CROWN, B_FLOOR = 1.6, -1.2, -3.2
DRAIN = (-19.5, -16.0, 0.5)  # the drain's cell in the vault: its low corner (x, y) and size
STAIR_X, STAIR_WIDTH = -15.4, 1.2
STAIR_RISE, STAIR_RUN, STAIR_FIRST = 0.173, 0.28, -0.6  # steps from 0.6 m under the water up to the deck


def orient(face, verts, want):
    """face (indices into verts) wound so its normal points along want (a vector)."""
    p = [Vector(verts[i]) for i in face]
    n = Vector((0.0, 0.0, 0.0))
    for k in range(len(p)):
        a, b = p[k], p[(k + 1) % len(p)]
        n += Vector(((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)))  # Newell
    return face if n.dot(want) >= 0.0 else tuple(reversed(face))


def add_quads_inward(acc, slot, quads, inside):
    """Quads (four points each) facing a point inside the room (or a function of the quad's centre giving it)."""
    v, f, uv = [], [], []
    for q in quads:
        base = len(v)
        c = sum((Vector(p) for p in q), Vector()) / 4.0
        target = inside(c) if callable(inside) else Vector(inside)
        if orient((0, 1, 2, 3), q, target - c) != (0, 1, 2, 3):
            q = [q[0], q[3], q[2], q[1]]  # turned round (its UVs follow it)
        v.extend(q)
        f.append(tuple(range(base, base + 4)))
        span = max((Vector(q[1]) - Vector(q[0])).length, 1e-3)
        tall = max((Vector(q[3]) - Vector(q[0])).length, 1e-3)
        uv.append([(c.x + c.y, c.z), (c.x + c.y + span, c.z), (c.x + c.y + span, c.z + tall), (c.x + c.y, c.z + tall)])
    acc.add(v, f, slot, uv)


def arch_section(width, crown, floor, n=10):
    """A passage's section, (u across, z): straight walls up to the spring, a round arch of half the width over them."""
    r = width / 2.0
    spring = crown - r
    pts = [(r, floor), (-r, floor), (-r, spring)]
    for k in range(1, n):
        a = math.pi - math.pi * k / n
        pts.append((r * math.cos(a), spring + r * math.sin(a)))
    pts.append((r, spring))
    return pts


def arch_top(u, width, crown):
    """The height of an arched opening's top at u across it (|u| <= width / 2)."""
    r = width / 2.0
    return crown - r + math.sqrt(max(r * r - u * u, 0.0))


def passage(acc, path, width, crown, floor_from, floor_to):
    """A vaulted passage swept along a level polyline (plan points), mitred at its bends, open at both ends."""
    section = arch_section(width, crown, 0.0)
    pts = [Vector((x, y, 0.0)) for x, y in path]
    n = len(pts)
    total = sum((pts[i + 1] - pts[i]).length for i in range(n - 1))
    rings, along = [], 0.0
    for i in range(n):
        if i:
            along += (pts[i] - pts[i - 1]).length
        t_in = (pts[i] - pts[i - 1]).normalized() if i else (pts[1] - pts[0]).normalized()
        t_out = (pts[i + 1] - pts[i]).normalized() if i < n - 1 else t_in
        t = (t_in + t_out).normalized()
        side = Vector((0.0, 0.0, 1.0)).cross(t).normalized()
        mitre = 1.0 / max(t.dot(t_out), 0.3)
        floor = floor_from + (floor_to - floor_from) * (along / total if total > 0 else 0.0)
        rings.append([tuple(pts[i] + side * (u * mitre) + Vector((0.0, 0.0, floor + z))) for u, z in section])
    v = [p for ring in rings for p in ring]
    f, uv = [], []
    m = len(section)
    for i in range(n - 1):
        mid = (pts[i] + pts[i + 1]) / 2.0 + Vector((0.0, 0.0, floor_from + 1.0))
        for s in range(m):
            s2 = (s + 1) % m
            face = (i * m + s, i * m + s2, (i + 1) * m + s2, (i + 1) * m + s)
            c = sum((Vector(v[k]) for k in face), Vector()) / 4.0
            f.append(orient(face, v, mid - c))
            uv.append([(s * 0.3, i * 2.0), ((s + 1) * 0.3, i * 2.0), ((s + 1) * 0.3, (i + 1) * 2.0), (s * 0.3, (i + 1) * 2.0)])
    acc.add(v, f, "sb_masonry", uv)


def wall_with_openings(along, fixed, axis, z_floor, z_top, openings, inside, step=0.25):
    """A flat wall's quads as columns along it: from the floor to its top (z_top(u)), less each opening (u centre,
    width, sill, top(u)). axis "x": the wall runs along x at y = fixed; "y": along y at x = fixed."""
    cuts = {round(along[0] + k * step, 4) for k in range(int((along[1] - along[0]) / step) + 1)}
    cuts.add(along[1])
    for uc, w, _sill, _top in openings:
        for k in range(11):
            cuts.add(round(uc - w / 2.0 + w * k / 10.0, 4))
    us = sorted(c for c in cuts if along[0] <= c <= along[1])

    def at(u, z):
        return (u, fixed, z) if axis == "x" else (fixed, u, z)

    def opening_at(u):
        for uc, w, sill, top in openings:
            if abs(u - uc) <= w / 2.0 + 1e-6:
                return sill, top(u - uc)
        return None

    quads = []
    for u0, u1 in zip(us, us[1:]):
        o = opening_at((u0 + u1) / 2.0)
        spans = [(z_floor, z_floor, z_top(u0), z_top(u1))]
        if o is not None:
            sill, _ = o
            top0 = opening_at(u0)[1] if opening_at(u0) else z_floor
            top1 = opening_at(u1)[1] if opening_at(u1) else z_floor
            spans = []
            if sill > z_floor + 1e-3:
                spans.append((z_floor, z_floor, sill, sill))
            spans.append((min(top0, z_top(u0)), min(top1, z_top(u1)), z_top(u0), z_top(u1)))
        for b0, b1, t0, t1 in spans:
            if t0 - b0 > 1e-3 or t1 - b1 > 1e-3:
                quads.append([at(u0, b0), at(u1, b1), at(u1, t1), at(u0, t0)])
    return quads


def make_cellar(rng, deck, notes):
    acc = Acc("basin_cellar")
    crown = deck - 0.45
    rise = crown - CH_SPRING
    yc, half = (CH_Y[0] + CH_Y[1]) / 2.0, (CH_Y[1] - CH_Y[0]) / 2.0

    def vault_z(y):
        return CH_SPRING + rise * math.sqrt(max(1.0 - ((y - yc) / half) ** 2, 0.0))

    inside = Vector(((CH_X[0] + CH_X[1]) / 2.0, yc, -1.0))
    # The floor: silt over flags, with small lumps.
    floor_q = []
    for i in range(28):
        for j in range(24):
            x0, y0 = CH_X[0] + i * 0.5, CH_Y[0] + j * 0.5

            def fz(x, y):
                return CH_FLOOR + 0.05 * fbm(x * 0.7, y * 0.7, 2, 131.0)

            floor_q.append([(x0, y0, fz(x0, y0)), (x0 + 0.5, y0, fz(x0 + 0.5, y0)), (x0 + 0.5, y0 + 0.5, fz(x0 + 0.5, y0 + 0.5)),
                            (x0, y0 + 0.5, fz(x0, y0 + 0.5))])
    add_quads_inward(acc, "sb_silt", floor_q, lambda c: c + Vector((0.0, 0.0, 1.0)))
    # The long walls (up to the spring): the south with the stair's opening, the north with passage B's.
    stair_open = (STAIR_X, STAIR_WIDTH, STAIR_FIRST, lambda u: CH_SPRING)
    b_open = (B_PATH[0][0], B_WIDTH, B_FLOOR, lambda u: arch_top(u, B_WIDTH, B_CROWN))
    add_quads_inward(acc, "sb_masonry", wall_with_openings(CH_X, CH_Y[0], "x", CH_FLOOR, lambda u: CH_SPRING, [stair_open], inside), inside)
    add_quads_inward(acc, "sb_masonry", wall_with_openings(CH_X, CH_Y[1], "x", CH_FLOOR, lambda u: CH_SPRING, [b_open], inside), inside)
    # The end walls, up under the vault: the east with arch A, the west plain (the find's ledge on it).
    a_open = (A_Y, A_WIDTH, CH_FLOOR, lambda u: arch_top(u, A_WIDTH, A_CROWN))
    add_quads_inward(acc, "sb_masonry", wall_with_openings(CH_Y, CH_X[1], "y", CH_FLOOR, vault_z, [a_open], inside), inside)
    add_quads_inward(acc, "sb_masonry", wall_with_openings(CH_Y, CH_X[0], "y", CH_FLOOR, vault_z, [], inside), inside)
    # The vault, its crown cell for the drain left open.
    vault_q = []
    nx, ny = int((CH_X[1] - CH_X[0]) / 0.5), 24
    for i in range(nx):
        for j in range(ny):
            x0 = CH_X[0] + i * 0.5
            y0, y1 = CH_Y[0] + j * (2 * half / ny), CH_Y[0] + (j + 1) * (2 * half / ny)
            if abs(x0 - DRAIN[0]) < 1e-3 and abs(y0 - DRAIN[1]) < 1e-3:
                continue
            vault_q.append([(x0, y0, vault_z(y0)), (x0 + 0.5, y0, vault_z(y0)), (x0 + 0.5, y1, vault_z(y1)), (x0, y1, vault_z(y1))])
    add_quads_inward(acc, "sb_masonry", vault_q, lambda c: Vector((c.x, yc, CH_FLOOR)))
    # The drain's shaft up to the street, and its grate (exported with the street pieces).
    dx, dy, ds = DRAIN
    z0 = vault_z(dy + ds / 2.0)
    shaft = []
    for (ax, ay), (bx, by) in (((dx, dy), (dx + ds, dy)), ((dx + ds, dy), (dx + ds, dy + ds)), ((dx + ds, dy + ds), (dx, dy + ds)), ((dx, dy + ds), (dx, dy))):
        shaft.append([(ax, ay, vault_z(ay)), (bx, by, vault_z(by)), (bx, by, deck - 0.06), (ax, ay, deck - 0.06)])
    add_quads_inward(acc, "sb_masonry", shaft, Vector((dx + ds / 2.0, dy + ds / 2.0, z0 + 0.3)))
    # Passage A (from the wall's face to the chamber's east wall) and passage B (from the north wall to the breach).
    passage(acc, [(0.3, A_Y), (CH_X[1], A_Y)], A_WIDTH, A_CROWN, A_FLOOR, CH_FLOOR)
    passage(acc, B_PATH, B_WIDTH, B_CROWN, B_FLOOR, B_FLOOR + 0.1)
    # Arch A's dressed surround on the wall's face: voussoirs and a pale keystone (the hint from the quay).
    r = A_WIDTH / 2.0
    spring = A_CROWN - r
    for k in range(9):
        a = math.pi * (k + 0.5) / 9
        key = k == 4
        v, f, uv = box(0.3, 0.42 if key else 0.34, 0.38 if key else 0.3)
        at = (0.12, A_Y + (r + 0.17) * math.cos(a), spring + (r + 0.17) * math.sin(a))
        acc.add(v, f, "sb_keystone" if key else "sb_masonry", uv, place(at, (a - math.pi / 2, 0.0, 0.0)), smooth=False)
    # Breach B: the wall slumped round the hole, rubble half closing it.
    for _ in range(14):
        bx, by = rng.uniform(-0.5, 1.8), B_PATH[-1][1] + rng.uniform(-1.4, 1.4)
        size = rng.uniform(0.25, 0.6)
        v, f, uv = rock(rng)
        acc.add(v, f, "sb_stone", uv, place((bx, by, max(bed_z(bx, by), B_FLOOR) + size * 0.3), (0.0, 0.0, rng.uniform(0, 6.28)),
                                           (size, size * 0.8, size * 0.6)))
    # The stair: from the south wall's opening, out of the water and up to the grate, in a narrow well.
    steps = int(math.ceil((deck - STAIR_FIRST) / STAIR_RISE))
    well = []
    for k in range(steps):
        z = STAIR_FIRST + k * STAIR_RISE
        y_front = CH_Y[0] - k * STAIR_RUN
        v, f, uv = box(STAIR_WIDTH, STAIR_RUN, STAIR_RISE)
        acc.add(v, f, "sb_masonry", uv, place((STAIR_X, y_front - STAIR_RUN / 2.0, z - STAIR_RISE / 2.0)), smooth=False)
    y_top = CH_Y[0] - steps * STAIR_RUN
    # Its roof rises at the flight's pitch from the chamber's spring to the street; past that (y_roof to y_top) the well
    # is open under the grate.
    roof_lo, roof_hi = CH_SPRING, deck - 0.06
    y_roof = CH_Y[0] - max(0.0, (roof_hi - roof_lo)) / max(STAIR_RISE / STAIR_RUN, 1e-3)

    def under_steps(y):
        return STAIR_FIRST - STAIR_RISE + (CH_Y[0] - y) / STAIR_RUN * STAIR_RISE

    for sx in (STAIR_X - STAIR_WIDTH / 2.0, STAIR_X + STAIR_WIDTH / 2.0):
        well.append([(sx, CH_Y[0], under_steps(CH_Y[0])), (sx, y_roof, under_steps(y_roof)), (sx, y_roof, roof_hi), (sx, CH_Y[0], roof_lo)])
        well.append([(sx, y_roof, under_steps(y_roof)), (sx, y_top, under_steps(y_top)), (sx, y_top, roof_hi), (sx, y_roof, roof_hi)])
    well.append([(STAIR_X - STAIR_WIDTH / 2.0, CH_Y[0], roof_lo), (STAIR_X + STAIR_WIDTH / 2.0, CH_Y[0], roof_lo),
                 (STAIR_X + STAIR_WIDTH / 2.0, y_roof, roof_hi), (STAIR_X - STAIR_WIDTH / 2.0, y_roof, roof_hi)])
    add_quads_inward(acc, "sb_masonry", well, lambda c: Vector((STAIR_X, c.y, c.z - 0.5)))
    # The boat cellar's remains: two trestles, a rotted keel and a few ribs (some broken off), a hoist chain from the vault.
    for tx in (-22.0, -17.0):
        v, f, uv = box(0.15, 1.2, 0.8)
        acc.add(v, f, "sb_wood", uv, place((tx, -19.0, CH_FLOOR + 0.4), (0.0, rng.uniform(-0.05, 0.05), 0.0)), smooth=False)
    keel = [(-23.2, -19.0, CH_FLOOR + 0.86), (-19.5, -19.02, CH_FLOOR + 0.84), (-15.8, -18.97, CH_FLOOR + 0.9)]
    v, f, uv = tube(keel, 0.07, 8)
    acc.add(v, f, "sb_wood", uv)
    for k, rx in enumerate((-22.6, -21.6, -20.6, -19.6, -18.6, -17.6, -16.6)):
        for side in (-1, 1):
            if rng.random() < 0.3:
                continue  # that side's rib gone
            reach = rng.uniform(0.45, 1.0)
            arc = []
            for s in range(7):
                a = (math.pi / 2) * reach * s / 6
                arc.append((rx, -19.0 + side * 0.75 * math.sin(a), CH_FLOOR + 0.9 + 0.75 * (1.0 - math.cos(a))))
            v, f, uv = tube(arc, 0.03, 6)
            acc.add(v, f, "sb_wood", uv)
    links_along(acc, [(-21.0, -16.0, vault_z(-16.0) - 0.05), (-21.0, -16.05, -1.0), (-20.95, -16.1, -1.4)])
    notes["cellar"] = {
        "arch_a": plan_point(0.0, A_Y), "breach_b": plan_point(0.0, B_PATH[-1][1]),
        "chamber": [plan_point(CH_X[0], CH_Y[0]), plan_point(CH_X[1], CH_Y[1])], "floor_z": CH_FLOOR,
        "air_pocket": {"z": [0.0, round(crown, 2)], "box": [plan_point(CH_X[0], CH_Y[0]), plan_point(CH_X[1], CH_Y[1])]},
        "drain": plan_point(dx + ds / 2.0, dy + ds / 2.0), "stair_grate": plan_point(STAIR_X, (y_top + y_roof) / 2.0),
        "deck_z": deck, "passage_a_m": round(abs(CH_X[1] - 0.3), 2),
        "passage_b_m": round(sum(math.dist(B_PATH[i], B_PATH[i + 1]) for i in range(len(B_PATH) - 1)), 2),
    }
    return acc, (y_top, y_roof)


def make_grates(deck, stair_y):
    """The street's two grates over the cellar, to set in the paving's cuts: the drain's at the square, and the stair's
    locked grate (its frame, bars, a chain round two bars and a padlock)."""
    acc = Acc("basin_grates")
    dx, dy, ds = DRAIN
    for k in range(6):
        v, f, uv = box(0.025, ds, 0.04)
        acc.add(v, f, "sb_metal", uv, place((dx + 0.04 + k * (ds - 0.08) / 5.0, dy + ds / 2.0, deck - 0.02)), smooth=False)
    for y in (dy + 0.02, dy + ds - 0.02):
        v, f, uv = box(ds, 0.04, 0.05)
        acc.add(v, f, "sb_metal", uv, place((dx + ds / 2.0, y, deck - 0.025)), smooth=False)
    y_top, y_roof = stair_y
    gy0, gy1 = y_top, y_roof  # the well's open top, over the flight's upper steps
    length = max(abs(gy1 - gy0), 0.6)
    gyc = (gy0 + gy1) / 2.0
    for k in range(9):
        v, f, uv = box(0.022, length, 0.022)
        acc.add(v, f, "sb_metal", uv, place((STAIR_X - STAIR_WIDTH / 2.0 + 0.07 + k * (STAIR_WIDTH - 0.14) / 8.0, gyc, deck + 0.01)), smooth=False)
    for y in (gyc - length / 2.0, gyc, gyc + length / 2.0):
        v, f, uv = box(STAIR_WIDTH, 0.05, 0.03)
        acc.add(v, f, "sb_metal", uv, place((STAIR_X, y, deck + 0.012)), smooth=False)
    # The chain through the two middle bars and the padlock hanging from it.
    cx = STAIR_X - STAIR_WIDTH / 2.0 + 0.07 + 4 * (STAIR_WIDTH - 0.14) / 8.0
    pts = [(cx - 0.14, gyc, deck + 0.03), (cx - 0.05, gyc + 0.02, deck + 0.05), (cx + 0.05, gyc - 0.02, deck + 0.05), (cx + 0.14, gyc, deck + 0.03)]
    links_along(acc, pts, pitch=0.06)
    v, f, uv = box(0.06, 0.025, 0.07)
    acc.add(v, f, "sb_metal", uv, place((cx, gyc + 0.05, deck + 0.02)), smooth=False)
    return acc


def make_find():
    """The find on the chamber's west ledge: a sealed tin document box (its text is the story lane's), and the ledge."""
    acc = Acc("basin_find")
    v, f, uv = box(0.5, 3.0, 0.3)
    acc.add(v, f, "sb_masonry", uv, place((CH_X[0] + 0.25, -16.5, 0.2)), smooth=False)
    v, f, uv = box(0.3, 0.2, 0.1)
    acc.add(v, f, "sb_find", uv, place((CH_X[0] + 0.25, -16.3, 0.35 + 0.05), (0.0, 0.0, 0.12)), smooth=False)
    v, f, uv = box(0.31, 0.21, 0.025)
    acc.add(v, f, "sb_find", uv, place((CH_X[0] + 0.25, -16.3, 0.35 + 0.112), (0.0, 0.0, 0.12)), smooth=False)
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
    # The cellar: arch A from the basin, the chamber from the pocket (up at the drain, across at the find).
    shot("arch_a_3m", (3.2, A_Y + 0.4, -1.8), (0.0, A_Y, -2.0), lens=24.0)
    shot("pocket_up_drain", (-18.0, -15.0, 0.2), (DRAIN[0] + 0.25, DRAIN[1] + 0.25, 1.5), lens=18.0)
    shot("pocket_find_2m", (-23.5, -16.0, 0.3), (CH_X[0] + 0.25, -16.3, 0.4), lens=28.0)
    shot("chamber_floor", (-13.0, -12.0, -1.6), (-19.5, -19.0, -2.6), lens=18.0)


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
    cellar, stair_y = make_cellar(rng, o["deck"], notes)
    groups.append(("cellar", cellar))
    groups.append(("grates", make_grates(o["deck"], stair_y)))
    groups.append(("find", make_find()))
    report = {
        "frame": {"units": "m", "up": "Z", "plan_origin": list(PLAN_ORIGIN),
                  "engine_place": "(940 - side/2, side/2 - 670, 0) m, no rotation", "seed": o["seed"], "grid_m": o["grid"],
                  "deck_m": o["deck"]},
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
