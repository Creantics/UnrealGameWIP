"""The hero corner's paper lanterns and string posts, modelled by script.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/hero_corner_props.py -- <out_dir>

Writes into <out_dir>:
  SM_HC_LanternLarge.glb   0.60 m tall, 0.45 m across; origin at the hook (its top), hanging down -Z (Blender)
  SM_HC_LanternSmall.glb   0.32 m tall, 0.25 m across; origin at the hook
  SM_HC_StringPost.glb     4.0 m timber post; origin at its foot; an iron collar at 3.8 m (the cable's anchor)
  hero_corner_props.json   triangles, bounds and slots per file, from the files read back
Every piece is round or square about its vertical axis, so its yaw on import doesn't matter. The cables are not meshes:
FTBuildMassingWorld -HeroCorner lays them from ftcore::heroCornerLayout's polyline.

Materials are flat factors (no textures). The paper's slot is "lantern_paper": FTBuildMassingWorld dresses it in the
night glow. One UV set (cylindrical on the lathed parts, planar on boxes), no vertex colour; checked on read-back.
"""
import json
import math
import os
import struct
import sys

import bmesh
import bpy

SEG = 32

# name: (base colour linear rgb, roughness, metallic)
MATERIALS = {
    "lantern_paper": ((0.72, 0.16, 0.08), 0.85, 0.0),   # vermilion washi
    "lantern_wood": ((0.045, 0.028, 0.018), 0.6, 0.0),  # lacquered dark wood caps
    "lantern_iron": ((0.03, 0.03, 0.03), 0.45, 1.0),    # hook and wire
    "post_timber": ((0.11, 0.075, 0.05), 0.75, 0.0),    # weathered cedar
    "post_iron": ((0.035, 0.035, 0.035), 0.5, 1.0),     # collar, eye, cap band
}


def material(name):
    m = bpy.data.materials.get(name)
    if m:
        return m
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes.get("Principled BSDF")
    col, rough, metal = MATERIALS[name]
    bsdf.inputs["Base Color"].default_value = (*col, 1.0)
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["Metallic"].default_value = metal
    return m


class Builder:
    """Accumulates parts into one bmesh with a UV layer and per-face material index."""

    def __init__(self, name):
        self.name = name
        self.bm = bmesh.new()
        self.uv = self.bm.loops.layers.uv.new("UVMap")
        self.slots = []

    def slot(self, mat):
        if mat not in self.slots:
            self.slots.append(mat)
        return self.slots.index(mat)

    def lathe(self, profile, mat, seg=SEG, cap_top=False, cap_bottom=False, smooth=True):
        """profile: [(r, z)] from bottom to top. Cylindrical UVs: u around, v by profile length (m)."""
        idx = self.slot(mat)
        bm = self.bm
        rings = []
        for r, z in profile:
            ring = [bm.verts.new((r * math.cos(2 * math.pi * k / seg), r * math.sin(2 * math.pi * k / seg), z)) for k in range(seg)]
            rings.append(ring)
        vlen = [0.0]
        for i in range(1, len(profile)):
            (r0, z0), (r1, z1) = profile[i - 1], profile[i]
            vlen.append(vlen[-1] + math.hypot(r1 - r0, z1 - z0))
        circ = 2 * math.pi * max(r for r, _ in profile)
        for i in range(len(rings) - 1):
            for k in range(seg):
                k1 = (k + 1) % seg
                f = bm.faces.new((rings[i][k], rings[i][k1], rings[i + 1][k1], rings[i + 1][k]))
                f.material_index = idx
                f.smooth = smooth
                us = (k / seg * circ, (k + 1) / seg * circ)
                for loop, (u, v) in zip(f.loops, ((us[0], vlen[i]), (us[1], vlen[i]), (us[1], vlen[i + 1]), (us[0], vlen[i + 1]))):
                    loop[self.uv].uv = (u, v)
        for ring, on, flip in ((rings[0], cap_bottom, True), (rings[-1], cap_top, False)):
            if not on:
                continue
            verts = list(reversed(ring)) if flip else ring
            f = bm.faces.new(verts)
            f.material_index = idx
            f.smooth = False
            for loop in f.loops:
                loop[self.uv].uv = (loop.vert.co.x, loop.vert.co.y)

    def torus(self, major, minor, z, mat, seg=SEG, tube=8, axis="Z"):
        """A ring about Z at height z (axis Z), or standing in the XZ plane centred at (0, 0, z) (axis Y)."""
        idx = self.slot(mat)
        bm = self.bm
        grid = []
        for i in range(seg):
            a = 2 * math.pi * i / seg
            row = []
            for j in range(tube):
                b = 2 * math.pi * j / tube
                rr = major + minor * math.cos(b)
                h = minor * math.sin(b)
                if axis == "Z":
                    co = (rr * math.cos(a), rr * math.sin(a), z + h)
                else:
                    co = (rr * math.cos(a), h, z + rr * math.sin(a))
                row.append(bm.verts.new(co))
            grid.append(row)
        for i in range(seg):
            for j in range(tube):
                i1, j1 = (i + 1) % seg, (j + 1) % tube
                f = bm.faces.new((grid[i][j], grid[i1][j], grid[i1][j1], grid[i][j1]))
                f.material_index = idx
                f.smooth = True
                for loop, (u, v) in zip(f.loops, ((i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1))):
                    loop[self.uv].uv = (u / seg * 2 * math.pi * major, v / tube * 2 * math.pi * minor)

    def box(self, x0, y0, z0, x1, y1, z1, mat):
        idx = self.slot(mat)
        bm = self.bm
        v = [bm.verts.new((x, y, z)) for z in (z0, z1) for y in (y0, y1) for x in (x0, x1)]
        quads = ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5))
        for q in quads:
            f = bm.faces.new([v[i] for i in q])
            f.material_index = idx
            f.smooth = False
            f.normal_update()
            n = f.normal
            for loop in f.loops:
                c = loop.vert.co
                if abs(n.z) > 0.5:
                    loop[self.uv].uv = (c.x, c.y)
                elif abs(n.x) > 0.5:
                    loop[self.uv].uv = (c.y, c.z)
                else:
                    loop[self.uv].uv = (c.x, c.z)

    def finish(self):
        bmesh.ops.recalc_face_normals(self.bm, faces=self.bm.faces)
        me = bpy.data.meshes.new(self.name)
        self.bm.to_mesh(me)
        self.bm.free()
        for m in self.slots:
            me.materials.append(material(m))
        ob = bpy.data.objects.new(self.name, me)
        bpy.context.scene.collection.objects.link(ob)
        return ob


def lantern(name, height, diameter):
    """A paper lantern hanging from its hook: the hook's top at the origin, the body below."""
    b = Builder(name)
    s = height / 0.6  # proportions are drawn for the 0.6 m lantern
    cap_r = 0.13 * s
    cap_h = 0.045 * s
    hook_h = 0.03 * s
    top = -hook_h
    body_top = top - cap_h
    body_bot = -height + cap_h
    R = diameter / 2
    # The paper: an ellipsoid between the caps, a bamboo rib every ~4 cm standing out as a small ridge.
    ribs = max(6, int(round((body_top - body_bot) / (0.04 * s))))
    prof = []
    steps = ribs * 4
    zc = 0.5 * (body_top + body_bot)
    hz = 0.5 * (body_top - body_bot)
    for i in range(steps + 1):
        t = i / steps
        z = body_bot + t * (body_top - body_bot)
        e = math.sqrt(max(0.0, 1.0 - ((z - zc) / (hz * 1.04)) ** 2))
        r = max(cap_r * 0.92, R * e)
        rib = 0.5 + 0.5 * math.cos(2 * math.pi * t * ribs)
        r += 0.004 * s * rib ** 6
        prof.append((r, z))
    b.lathe(prof, "lantern_paper")
    # Caps: lacquered rings with a lip, closed.
    b.lathe([(cap_r * 0.95, body_top), (cap_r, body_top + 0.008 * s), (cap_r, top - 0.008 * s), (cap_r * 0.9, top)],
            "lantern_wood", cap_top=True, cap_bottom=True, smooth=False)
    b.lathe([(cap_r * 0.9, -height), (cap_r, -height + 0.008 * s), (cap_r, body_bot - 0.008 * s), (cap_r * 0.95, body_bot)],
            "lantern_wood", cap_top=True, cap_bottom=True, smooth=False)
    # The hook: a small iron ring standing on the top cap, its top at the origin.
    ring_r = hook_h * 0.5
    b.torus(ring_r - 0.003 * s, 0.003 * s, top + ring_r, "lantern_iron", seg=16, tube=6, axis="Y")
    # A tassel cord under the bottom cap (thin box), read only close up.
    b.box(-0.004 * s, -0.004 * s, -height - 0.06 * s, 0.004 * s, 0.004 * s, -height, "lantern_iron")
    return b.finish()


def post(name, height=4.0, side=0.14, anchor_below_top=0.2):
    b = Builder(name)
    h = side / 2
    # The timber; an iron cap, an iron foot shoe and the iron collar the cable is tied to.
    b.box(-h, -h, 0.0, h, h, height - 0.03, "post_timber")
    b.box(-h - 0.012, -h - 0.012, height - 0.03, h + 0.012, h + 0.012, height, "post_iron")
    b.box(-h - 0.01, -h - 0.01, 0.0, h + 0.01, h + 0.01, 0.25, "post_iron")
    z = height - anchor_below_top
    b.box(-h - 0.008, -h - 0.008, z - 0.04, h + 0.008, h + 0.008, z + 0.04, "post_iron")
    return b.finish()


def export(ob, path):
    bpy.context.view_layer.update()
    for o in bpy.context.view_layer.objects:
        o.select_set(o is ob)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True, export_apply=True, export_yup=True,
                              export_extras=False, export_tangents=False, export_vertex_color="NONE", export_cameras=False,
                              export_lights=False, export_animations=False, export_materials="EXPORT",
                              export_image_format="NONE", export_skins=False, export_morph=False)


def read_back(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, _v, _l = struct.unpack_from("<4sII", data, 0)
    assert magic == b"glTF", path
    clen, _ = struct.unpack_from("<I4s", data, 12)
    js = json.loads(data[20:20 + clen].decode("utf-8"))
    errs, tris, uvsets = [], 0, 0
    lo, hi = [1e9] * 3, [-1e9] * 3
    for mesh in js.get("meshes", []):
        for p in mesh["primitives"]:
            a = p["attributes"]
            n = 0
            while f"TEXCOORD_{n}" in a:
                n += 1
            uvsets = max(uvsets, n)
            if "COLOR_0" in a:
                errs.append("vertex colour")
            acc = js["accessors"][a["POSITION"]]
            for i in range(3):
                lo[i] = min(lo[i], acc["min"][i])
                hi[i] = max(hi[i], acc["max"][i])
            tris += js["accessors"][p["indices"]]["count"] // 3
    if uvsets != 1:
        errs.append(f"{uvsets} UV sets")
    if js.get("images"):
        errs.append("has images")
    slots = [m["name"] for m in js.get("materials", [])]
    # glTF is +Y up: report bounds as (x, up, -forward).
    return {"tris": tris, "min": [round(v, 4) for v in lo], "max": [round(v, 4) for v in hi], "slots": slots, "errors": errs}


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not argv:
        raise SystemExit("usage: ... -- <out_dir>")
    out = os.path.abspath(argv[0])
    os.makedirs(out, exist_ok=True)
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    parts = {
        "SM_HC_LanternLarge": lambda n: lantern(n, 0.60, 0.45),
        "SM_HC_LanternSmall": lambda n: lantern(n, 0.32, 0.25),
        "SM_HC_StringPost": lambda n: post(n),
    }
    report = {}
    failed = False
    for name, make in parts.items():
        ob = make(name)
        path = os.path.join(out, name + ".glb")
        export(ob, path)
        r = read_back(path)
        report[name] = r
        failed |= bool(r["errors"])
        print(f"{name}: {r['tris']} tris, bounds {r['min']}..{r['max']} (glTF +Y up), slots {r['slots']}, errors {r['errors'] or 'none'}")
    with open(os.path.join(out, "hero_corner_props.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    if failed:
        raise SystemExit("read-back errors")


main()
