"""Opens the mouth of a scanned or generated head whose lips are one closed surface, headless, before rig_scan.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/mouth_cut.py -- \\
        IN.glb|IN.blend --seam 1.649 --corner 0.036 --out OUT.glb [--blend OUT.blend] [--part 0.0006] \\
        [--front 0.006] [--min-island 500]

A generated head (Tripo and the like) has its lips sculpted on: jawOpen then stretches a curtain between them. This
cuts the front surface along the plane z = seam between x = -corner and +corner (the crease between the lips), splits
the cut so the two lips are separate edges joined only at the corners, and parts them by `part` at the middle (tapered
to nothing at the corners) so rig_scan's merge by distance cannot weld them again and face_transfer's "slit" can tell
the upper lip from the lower. Only faces within `front` of the first surface seen from the front are cut, so folds
behind the lips stay whole. Loose islands under `min-island` vertices (blobs a generator leaves inside the mouth)
are dropped first. The mouth inside is a separate mesh (mouth_bag.py) made after the rig.
"""
import math
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:]
    a = {"in": argv[0], "seam": None, "corner": None, "out": None, "blend": None, "part": 0.0006, "front": 0.006,
         "min-island": 500, "band": 0.012, "depth": 0.03, "bag": 1}
    i = 1
    while i < len(argv):
        k = argv[i][2:]
        a[k] = argv[i + 1]
        i += 2
    for k in ("seam", "corner", "part", "front", "band", "depth"):
        a[k] = float(a[k])
    a["min-island"] = int(a["min-island"])
    a["bag"] = int(a["bag"])
    if a["out"] is None:
        sys.exit("mouth_cut: --out is required")
    return a


def drop_islands(bm, keep):
    """Removes every connected island with fewer than `keep` vertices; returns how many vertices went."""
    seen, gone = set(), []
    for v in bm.verts:
        if v in seen:
            continue
        isl, stack = [], [v]
        seen.add(v)
        while stack:
            u = stack.pop()
            isl.append(u)
            for e in u.link_edges:
                w = e.other_vert(u)
                if w not in seen:
                    seen.add(w)
                    stack.append(w)
        if len(isl) < keep:
            gone += isl
    bmesh.ops.delete(bm, geom=gone, context="VERTS")
    return len(gone)


def seen_from(tree, f, c, eyes, facing=False):
    """True if some eye's ray to the face's centre c meets f first. With `facing`, only on its outer side: a ray
    reaching a face's back through a gap doesn't count. A generated mesh's normals aren't consistent, so that is only
    asked of faces well behind the outer surface."""
    for e in eyes:
        d = c - e
        if (not facing or f.normal.dot(d) < 0.0) and tree.ray_cast(e, d.normalized(), d.length + 1e-4)[2] == f.index:
            return True
    return False


def add_bag(bm, front_y, seam, corner, slot, nu=24, nt=16):
    """The mouth inside: a pocket behind the slit, open at the front, its rim tucked a few millimetres up into the
    upper lip and down into the lower one so the lips hide it, deepest at the middle. Its cross-section at each x is
    an ellipse whose front follows the face 3 mm behind the slit. Body faces round it that no front view can see
    (folds, the lips' inner sides) are removed first. Its faces take material `slot`. Returns (pocket faces, body
    faces removed)."""
    w = corner + 0.003
    secs = []
    for u in np.linspace(-1.0, 1.0, nu + 1):
        x = u * w
        s = max(0.15, math.sqrt(max(0.0, 1.0 - u * u)))
        fy = front_y(x, seam)
        if fy is None:
            fy = front_y(0.0, seam)
        rz, ry = 0.004 + 0.010 * s, 0.006 + 0.026 * s
        yc = fy + 0.003 + ry
        t0 = math.asin(min(0.95, (0.0025 + 0.0035 * s) / rz))
        secs.append((x, yc, ry, rz, t0))

    bm.faces.index_update()
    bm.normal_update()
    tree = BVHTree.FromBMesh(bm)
    eyes = [Vector((x, -0.6, z)) for x in (-0.25, -0.12, 0.0, 0.12, 0.25) for z in (seam - 0.15, seam, seam + 0.15)]

    def inside(f):
        """In the slab round the pocket (8 mm taller above so the nostrils stay, 2.5 cm below, 3 cm deeper) and seen
        from no front view."""
        c = f.calc_center_median()
        u = c.x / w
        if abs(u) >= 1.0:
            return False
        k = min(nu, max(0, int(round((u + 1.0) * nu / 2))))
        _x, yc, ry, rz, _t0 = secs[k]
        if not -(rz + 0.025) < c.z - seam < rz + 0.008 or c.y > yc + ry + 0.03:
            return False
        fy = front_y(c.x, c.z)
        return not seen_from(tree, f, c, eyes, facing=fy is not None and c.y - fy > 0.004)
    junk = [f for f in bm.faces if inside(f)]
    bmesh.ops.delete(bm, geom=junk, context="FACES")
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context="VERTS")
    rows = []
    for x, yc, ry, rz, t0 in secs:
        row = []
        for t in np.linspace(t0, 2.0 * math.pi - t0, nt + 1):
            row.append(bm.verts.new((x, yc - ry * math.cos(t), seam + rz * math.sin(t))))
        rows.append(row)
    made = []
    for i in range(nu):
        for j in range(nt):
            made.append(bm.faces.new((rows[i][j], rows[i][j + 1], rows[i + 1][j + 1], rows[i + 1][j])))
    for row, (x, yc, _ry, _rz, _t0) in ((rows[0], secs[0]), (rows[-1], secs[-1])):
        c = bm.verts.new((x, yc, seam))
        for j in range(nt):
            made.append(bm.faces.new((row[j], row[j + 1], c) if x < 0 else (row[j + 1], row[j], c)))
    for f in made:
        f.material_index = slot
    return len(made), len(junk)


def main():
    a = _args()
    if a["in"].lower().endswith(".blend"):
        bpy.ops.wm.open_mainfile(filepath=a["in"])
    else:
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.import_scene.gltf(filepath=a["in"])
    obj = next(o for o in bpy.data.objects if o.type == "MESH")
    seam, corner = a["seam"], a["corner"]
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    dropped = drop_islands(bm, a["min-island"])
    bm.verts.ensure_lookup_table()
    bm.faces.ensure_lookup_table()
    bm.faces.index_update()
    bm.normal_update()
    tree = BVHTree.FromBMesh(bm)

    def front_y(x, z):
        hit = tree.ray_cast(Vector((x, -10.0, z)), Vector((0.0, 1.0, 0.0)), 20.0)
        return None if hit[0] is None else hit[0].y

    # Folds a generator leaves inside the mouth: faces in the box round the crease that no front view can see.
    eyes = [Vector((x, -0.6, z)) for x in (-0.25, -0.12, 0.0, 0.12, 0.25) for z in (seam - 0.1, seam, seam + 0.1)]
    box, hidden = [], []
    for f in bm.faces:
        c = f.calc_center_median()
        if abs(c.x) > corner + 0.008 or abs(c.z - seam) > a["band"]:
            continue
        fy = front_y(c.x, c.z)
        if fy is None or c.y - fy > a["depth"]:
            continue
        box.append(f)
        if not seen_from(tree, f, c, eyes):
            hidden.append(f)
    bmesh.ops.delete(bm, geom=hidden, context="FACES")
    # The slit: what is left of the crease band between the corners.
    band = [f for f in bm.faces if min(v.co.z for v in f.verts) <= seam <= max(v.co.z for v in f.verts)
            and abs(f.calc_center_median().x) < corner and f.calc_center_median().z - seam < a["band"]
            and front_y(f.calc_center_median().x, seam) is not None
            and f.calc_center_median().y - front_y(f.calc_center_median().x, seam) < a["depth"]]
    bmesh.ops.delete(bm, geom=band, context="FACES")
    dropped += drop_islands(bm, a["min-island"])
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context="VERTS")
    # Close the slit to `part` at the middle: lip-edge vertices near the front are drawn to the seam.
    moved = 0
    xs = []
    for v in bm.verts:
        if not v.is_boundary or abs(v.co.x) >= corner or abs(v.co.z - seam) > a["band"]:
            continue
        fy = front_y(v.co.x, v.co.z)
        if fy is None or v.co.y - fy > a["front"]:
            continue
        side = np.mean([f.calc_center_median().z for f in v.link_faces]) - seam
        t = 1.0 - min(1.0, abs(v.co.x) / corner) ** 2
        target = seam + np.sign(side) * a["part"] * t
        if abs(v.co.z - seam) > abs(target - seam):
            v.co.z = target
            moved += 1
        xs.append(v.co.x)
    faces, chain = box, hidden
    if not xs:
        sys.exit("mouth_cut: no lip edge found at z = %.4f" % seam)
    bag_faces, cleared = add_bag(bm, front_y, seam, corner, max(1, len(obj.data.materials))) if a["bag"] else (0, 0)
    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()
    print("mouth_cut: dropped %d island vertices, %d faces round the crease, %d hidden folds and %d band faces removed,"
          " lip edge from x %.4f to %.4f, %d vertices drawn to the seam" % (dropped, len(faces), len(chain), len(band),
                                                                             min(xs), max(xs), moved))
    if a["bag"]:
        while len(obj.data.materials) < 1:
            obj.data.materials.append(bpy.data.materials.new("skin"))
        m = bpy.data.materials.new("mouth")
        m.diffuse_color = (0.12, 0.03, 0.04, 1.0)
        m.use_nodes = True
        m.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = m.diffuse_color
        obj.data.materials.append(m)
        print("mouth_cut: pocket of %d faces, %d body faces inside it removed" % (bag_faces, cleared))
    if a["blend"]:
        bpy.ops.wm.save_as_mainfile(filepath=a["blend"])
    bpy.ops.export_scene.gltf(filepath=a["out"], export_format="GLB")
    print("mouth_cut: %d bytes -> %s" % (os.path.getsize(a["out"]), a["out"]))


main()
