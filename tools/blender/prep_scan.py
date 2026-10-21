"""Readies a generated or scanned character mesh (one big textured mesh, no rig) for fit_canonical.py.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/prep_scan.py -- MESH --out prepped.glb \\
        [--height 1.88] [--rot-z -90] [--faces 60000] [--tex 2048] [--grid DIR] [--retopo QUADS] \
        [--region X0,Y0,Z0,X1,Y1,Z1:N ...]

Steps: import (.glb/.gltf/.obj/.fbx), join every mesh into one, turn it about Z by --rot-z degrees so it faces
-Y, scale it to --height metres, centre it on x = y = 0 with the soles on z = 0, decimate (collapse, UVs
kept) to about --faces triangles, scale every image down to at most --tex pixels (square, power of two),
and export a .glb with no skin. With --grid, also renders orthographic front and side views on a 5 cm
grid into DIR (front.png, side.png, plus grid.json with the pixel-to-metre mapping) for picking the 14
landmarks fit_canonical needs.

--region X0,Y0,Z0,X1,Y1,Z1:N (repeatable) gives a box its own budget: faces whose centres lie inside it (metres,
after scaling) decimate to about N triangles, the first matching box wins, and the rest of the body goes to
--faces. A 150k body otherwise flattens the face to 5 mm triangles while the hair keeps thousands. Each region
is decimated in edit mode with everything else unselected, so the seams between them stay closed.

--retopo QUADS replaces the decimation with a clean low-poly mesh: a voxel remesh closes the scan into one
watertight shell, QuadriFlow lays about QUADS quads over it (mirrored across x = 0, so fit_canonical can
mirror the weights), smart UV project and pack give it fresh UVs, and Cycles bakes the full-resolution
scan onto it: base colour and ORM (occlusion, roughness, metallic) through emission, and a tangent-space
normal map that keeps the scan's detail. --voxel sets the remesh voxel size (metres, default 0.006).
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bmesh  # noqa: E402
import bpy  # noqa: E402
import numpy as np  # noqa: E402

import ne_blender as nb  # noqa: E402


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    a = {"mesh": None, "out": None, "height": 1.88, "rot-z": -90.0, "faces": 60000, "tex": 2048, "grid": None, "retopo": 0, "voxel": 0.006,
         "region": []}
    i = 0
    while i < len(argv):
        k = argv[i]
        if k == "--region":
            box, _, n = argv[i + 1].partition(":")
            a["region"].append(([float(c) for c in box.split(",")], int(n)))
            i += 2
            continue
        if k.startswith("--") and k[2:] in a:
            a[k[2:]] = argv[i + 1]
            i += 2
            continue
        if not k.startswith("--") and a["mesh"] is None:
            a["mesh"] = k
        else:
            raise SystemExit("prep_scan: unknown argument %s" % k)
        i += 1
    if not a["mesh"] or not a["out"]:
        raise SystemExit(__doc__)
    a["height"], a["rot-z"] = float(a["height"]), float(a["rot-z"])
    a["faces"], a["tex"], a["retopo"], a["voxel"] = int(a["faces"]), int(a["tex"]), int(a["retopo"]), float(a["voxel"])
    return a


def import_one(path):
    nb.reset_scene()
    ext = os.path.splitext(path)[1].lower()
    if ext in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=os.path.abspath(path))
    elif ext == ".obj":
        bpy.ops.wm.obj_import(filepath=os.path.abspath(path))
    elif ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=os.path.abspath(path))
    else:
        raise SystemExit("prep_scan: unsupported mesh format %s" % ext)
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    if not meshes:
        raise SystemExit("prep_scan: no meshes in %s" % path)
    for o in meshes:
        mw = o.matrix_world.copy()
        o.parent = None
        o.matrix_world = mw
    for o in [o for o in bpy.data.objects if o.type != "MESH"]:
        bpy.data.objects.remove(o, do_unlink=True)
    bpy.ops.object.select_all(action="DESELECT")
    for o in meshes:
        o.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    if len(meshes) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    return obj


def place(obj, height, rot_z):
    obj.rotation_mode = "XYZ"  # the glTF importer leaves quaternion mode, which ignores rotation_euler
    obj.rotation_euler = (0.0, 0.0, math.radians(rot_z))
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=False)
    P = np.empty(len(obj.data.vertices) * 3)
    obj.data.vertices.foreach_get("co", P)
    P = P.reshape(-1, 3)
    lo, hi = P.min(axis=0), P.max(axis=0)
    s = height / float(hi[2] - lo[2])
    P = (P - np.array(((lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, lo[2]))) * s
    obj.data.vertices.foreach_set("co", P.ravel())
    obj.data.update()
    return s


def weld(obj):
    """Merges coincident vertices. glTF splits a vertex at every UV seam, and a mesh with a per-triangle UV
    atlas arrives as loose triangles that decimation collapses to nothing and the voxel remesh can't close."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    n = len(bm.verts)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
    if len(bm.verts) < n:
        bm.to_mesh(obj.data)
        print("prep_scan: welded %d -> %d vertices" % (n, len(bm.verts)))
    bm.free()


def decimate(obj, faces):
    tris = sum(len(p.vertices) - 2 for p in obj.data.polygons)
    if tris <= faces:
        return tris, tris
    m = obj.modifiers.new("decimate", "DECIMATE")
    m.decimate_type = "COLLAPSE"
    m.ratio = faces / tris
    m.use_collapse_triangulate = True
    nb.activate(obj)
    bpy.ops.object.modifier_apply(modifier=m.name)
    return tris, sum(len(p.vertices) - 2 for p in obj.data.polygons)


def _image_node(mat, kind):
    """The image feeding the Principled BSDF's `kind` input ("Base Color", "Roughness", "Normal"), following
    separate-colour and normal-map nodes back to their image."""
    nodes = [n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"]
    if not nodes or not nodes[0].inputs[kind].links:
        return None, None
    n, sock = nodes[0].inputs[kind].links[0].from_node, nodes[0].inputs[kind].links[0].from_socket
    while n.type != "TEX_IMAGE":
        ins = [i for i in n.inputs if i.links]
        if not ins:
            return None, None
        n, sock = ins[-1].links[0].from_node, ins[-1].links[0].from_socket
    return n, sock


def keep_largest_shell(obj):
    """Deletes every connected piece but the largest. Parts grafted with an overlap leave closed voids
    behind, and the voxel remesh turns each into an inward-facing shell that QuadriFlow refuses."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.verts.ensure_lookup_table()
    comp = [-1] * len(bm.verts)
    sizes = []
    for v0 in bm.verts:
        if comp[v0.index] >= 0:
            continue
        c = len(sizes)
        comp[v0.index] = c
        stack, n = [v0], 0
        while stack:
            v = stack.pop()
            n += 1
            for e in v.link_edges:
                w = e.other_vert(v)
                if comp[w.index] < 0:
                    comp[w.index] = c
                    stack.append(w)
        sizes.append(n)
    if len(sizes) > 1:
        big = sizes.index(max(sizes))
        bmesh.ops.delete(bm, geom=[v for v in bm.verts if comp[v.index] != big], context="VERTS")
        bm.to_mesh(obj.data)
        print("prep_scan: kept the largest of %d shells (%d of %d vertices)" % (len(sizes), max(sizes), sum(sizes)))
    bm.free()


def retopo(high, quads, voxel):
    """A watertight QuadriFlow copy of `high`, mirrored across x = 0, with fresh packed UVs."""
    bpy.ops.object.select_all(action="DESELECT")
    nb.activate(high)
    bpy.ops.object.duplicate()
    low = bpy.context.view_layer.objects.active
    low.name = high.name + "_low"
    low.data.materials.clear()
    low.data.remesh_voxel_size = voxel
    low.data.use_remesh_fix_poles = True
    bpy.ops.object.voxel_remesh()
    keep_largest_shell(low)
    voxel_faces = len(low.data.polygons)
    bpy.ops.object.quadriflow_remesh(use_mesh_symmetry=True, use_preserve_sharp=False, use_preserve_boundary=False,
                                     smooth_normals=True, mode="FACES", target_faces=quads, seed=0)
    if len(low.data.polygons) >= voxel_faces:
        # QuadriFlow leaves the mesh alone when it finds a non-manifold spot (fingers that touch after the
        # remesh); a collapse decimate of the watertight voxel mesh is the fallback, triangles not quads
        before, after = decimate(low, 2 * quads)
        print("prep_scan: QuadriFlow refused the mesh, decimated %d -> %d triangles instead" % (before, after))
    for uv in list(low.data.uv_layers):
        low.data.uv_layers.remove(uv)
    low.data.uv_layers.new(name="UVMap")
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(66), island_margin=0.004, area_weight=0.0)
    bpy.ops.uv.pack_islands(rotate=True, margin=0.004)
    bpy.ops.object.mode_set(mode="OBJECT")
    return low


def bake(high, low, size):
    """Bakes high's base colour, ORM and normals onto low's UVs; gives low a material that uses them."""
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    sc.cycles.device = "CPU"
    sc.cycles.samples = 1
    sc.render.bake.margin = 8
    mat = bpy.data.materials.new(low.name)
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
    low.data.materials.append(mat)
    target = nt.nodes.new("ShaderNodeTexImage")
    nt.nodes.active = target
    images = {}
    # every material of the high mesh takes part (a mesh grafted from parts keeps one material per part)
    mats = []
    for hm in high.data.materials:
        out = next(n for n in hm.node_tree.nodes if n.type == "OUTPUT_MATERIAL")
        mats.append((hm, hm.node_tree.nodes.new("ShaderNodeEmission"), out, out.inputs["Surface"].links[0].from_socket))
    for name, kind in (("basecolor", "Base Color"), ("orm", "Roughness"), ("normal", None)):
        im = bpy.data.images.new("%s_%s" % (low.name, name), size, size, alpha=False)
        if name != "basecolor":
            im.colorspace_settings.name = "Non-Color"
        target.image = im
        nb.activate(low)  # activate() clears the selection, so the high mesh is selected after it
        low.select_set(True)
        high.select_set(True)
        if kind:
            srcs = [_image_node(hm, kind)[0] for hm, _, _, _ in mats]
            if all(src is None for src in srcs):
                continue
            for (hm, emit, out, keep), src in zip(mats, srcs):
                # the whole texture, not one channel: ORM's occlusion, roughness and metallic travel together
                if src is None:  # a part without this map bakes its own constant
                    emit.inputs["Color"].default_value = (1.0, 0.7, 0.0, 1.0) if kind == "Roughness" else (0.8, 0.8, 0.8, 1.0)
                else:
                    hm.node_tree.links.new(src.outputs["Color"], emit.inputs["Color"])
                hm.node_tree.links.new(emit.outputs["Emission"], out.inputs["Surface"])
            bpy.ops.object.bake(type="EMIT", use_selected_to_active=True, cage_extrusion=0.02, max_ray_distance=0.05,
                                margin=8)
        else:
            for hm, _, out, keep in mats:
                hm.node_tree.links.new(keep, out.inputs["Surface"])
            bpy.ops.object.bake(type="NORMAL", normal_space="TANGENT", use_selected_to_active=True,
                                cage_extrusion=0.02, max_ray_distance=0.05, margin=8)
        im.pack()
        images[name] = im
    for hm, _, out, keep in mats:
        hm.node_tree.links.new(keep, out.inputs["Surface"])
    nt.nodes.remove(target)
    tex = {}
    for name, im in images.items():
        n = nt.nodes.new("ShaderNodeTexImage")
        n.image = im
        tex[name] = n
    if "basecolor" in tex:
        nt.links.new(tex["basecolor"].outputs["Color"], bsdf.inputs["Base Color"])
    if "orm" in tex:
        sep = nt.nodes.new("ShaderNodeSeparateColor")
        nt.links.new(tex["orm"].outputs["Color"], sep.inputs["Color"])
        nt.links.new(sep.outputs["Green"], bsdf.inputs["Roughness"])
        nt.links.new(sep.outputs["Blue"], bsdf.inputs["Metallic"])
    if "normal" in tex:
        nm = nt.nodes.new("ShaderNodeNormalMap")
        nt.links.new(tex["normal"].outputs["Color"], nm.inputs["Color"])
        nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    return sorted(images)


def shrink_images(limit):
    out = []
    for im in bpy.data.images:
        w, h = im.size
        if w == 0 or h == 0:
            continue
        n = 1 << int(math.floor(math.log2(min(limit, max(w, h)))))
        if (w, h) != (n, n):
            im.scale(n, n)
        out.append((im.name, w, h, n))
    return out


def render_grid(obj, outdir):
    """Orthographic front (looking +Y) and side (looking -X) views on a white background with a 5 cm grid."""
    os.makedirs(outdir, exist_ok=True)
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_WORKBENCH"
    sc.display.shading.light = "STUDIO"
    sc.display.shading.color_type = "TEXTURE"
    sc.render.resolution_x = sc.render.resolution_y = 1024
    sc.render.film_transparent = False
    world = bpy.data.worlds.new("w")
    world.color = (1.0, 1.0, 1.0)
    sc.world = world
    P = np.array([v.co for v in obj.data.vertices])
    top = float(P[:, 2].max())
    size = top * 1.1
    cz = top / 2
    # thin grid bars as flat boxes, both views share them (one set in the XZ plane, one in YZ)
    mat = bpy.data.materials.new("grid")
    mat.diffuse_color = (0.2, 0.6, 1.0, 1.0)
    bars = []
    for z in np.arange(0.0, top + 0.001, 0.05):
        wide = 0.0025 if abs(z * 10 - round(z * 10)) < 1e-6 else 0.001
        for axis in ("x", "y"):
            bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0, 0, z))
            b = bpy.context.active_object
            b.scale = (size, 0.001, wide) if axis == "x" else (0.001, size, wide)
            b.location = (0, 0.6, z) if axis == "x" else (0.6, 0, z)
            b.data.materials.append(mat)
            b["view"] = "front" if axis == "x" else "side"
            bars.append(b)
    for x in np.arange(-size / 2, size / 2 + 0.001, 0.05):
        xr = round(x / 0.05) * 0.05
        wide = 0.0025 if abs(xr * 10 - round(xr * 10)) < 1e-6 else 0.001
        for axis in ("x", "y"):
            bpy.ops.mesh.primitive_cube_add(size=1.0)
            b = bpy.context.active_object
            b.scale = (wide, 0.001, size) if axis == "x" else (0.001, wide, size)
            b.location = (xr, 0.6, cz) if axis == "x" else (0.6, xr, cz)
            b.data.materials.append(mat)
            b["view"] = "front" if axis == "x" else "side"
            bars.append(b)
    cam_data = bpy.data.cameras.new("cam")
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = size
    cam = bpy.data.objects.new("cam", cam_data)
    sc.collection.objects.link(cam)
    sc.camera = cam
    views = {"front": ((0.0, -5.0, cz), (math.radians(90), 0.0, 0.0)),
             "side": ((-5.0, 0.0, cz), (math.radians(90), 0.0, math.radians(-90)))}
    for name, (loc, rot) in views.items():
        cam.location, cam.rotation_euler = loc, rot
        for b in bars:  # show only the bars behind the subject in this view
            b.hide_render = b["view"] != name
        sc.render.filepath = os.path.join(outdir, name + ".png")
        bpy.ops.render.render(write_still=True)
    with open(os.path.join(outdir, "grid.json"), "w") as f:
        json.dump({"pixels": 1024, "metres_across": size, "centre_z": cz,
                   "front": "screen right = +x", "side": "screen right = -y (the character faces screen right)",
                   "note": "pixel (u, v) from the top-left -> horizontal (u/1024 - 0.5) * metres_across, "
                           "z = centre_z + (0.5 - v/1024) * metres_across; bold lines every 10 cm"}, f, indent=1)
    for b in bars:
        bpy.data.objects.remove(b, do_unlink=True)
    bpy.data.objects.remove(cam, do_unlink=True)


def decimate_regions(obj, regions, faces):
    """regions: [([x0, y0, z0, x1, y1, z1], triangles)]. Each box's faces decimate to its budget, the rest to faces."""
    me = obj.data
    before = sum(len(p.vertices) - 2 for p in me.polygons)
    nb.activate(obj)

    def owner(c):
        for k, (b, _n) in enumerate(regions):
            if b[0] <= c.x <= b[3] and b[1] <= c.y <= b[4] and b[2] <= c.z <= b[5]:
                return k
        return len(regions)

    bpy.ops.object.mode_set(mode="EDIT")
    bpy.context.tool_settings.mesh_select_mode = (False, False, True)
    for k, target in enumerate([n for _b, n in regions] + [faces]):
        bm = bmesh.from_edit_mesh(me)
        for v in bm.verts:
            v.select = False
        for e in bm.edges:
            e.select = False
        tris = 0
        for f in bm.faces:
            f.select = owner(f.calc_center_median()) == k
            if f.select:
                tris += len(f.verts) - 2
                for v in f.verts:
                    v.select = True
                for e in f.edges:
                    e.select = True
        bm.select_flush_mode()
        bmesh.update_edit_mesh(me)
        if tris > target:
            bpy.ops.mesh.decimate(ratio=target / tris)
        print("prep_scan: region %d: %d -> ~%d triangles" % (k, tris, min(tris, target)))
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.quads_convert_to_tris()
    bpy.ops.object.mode_set(mode="OBJECT")
    return before, sum(len(p.vertices) - 2 for p in me.polygons)


def main():
    a = _args()
    obj = import_one(a["mesh"])
    s = place(obj, a["height"], a["rot-z"])
    weld(obj)
    if a["retopo"]:
        before = sum(len(p.vertices) - 2 for p in obj.data.polygons)
        low = retopo(obj, a["retopo"], a["voxel"])
        images = bake(obj, low, a["tex"])
        old = obj.name
        bpy.data.objects.remove(obj, do_unlink=True)
        obj = low
        obj.name = old
        after = sum(len(p.vertices) - 2 for p in obj.data.polygons)
    else:
        if a["region"]:
            before, after = decimate_regions(obj, a["region"], a["faces"])
        else:
            before, after = decimate(obj, a["faces"])
        images = shrink_images(a["tex"])
    obj.name = os.path.splitext(os.path.basename(a["out"]))[0]
    nb.set_smooth(obj, True)
    os.makedirs(os.path.dirname(os.path.abspath(a["out"])), exist_ok=True)
    bpy.ops.export_scene.gltf(filepath=os.path.abspath(a["out"]), export_format="GLB", export_yup=True,
                              export_image_format="AUTO", export_cameras=False, export_lights=False,
                              export_animations=False, export_skins=False)
    print("prep_scan: scale %.4f, triangles %d -> %d, %d vertices, images %s -> %s"
          % (s, before, after, len(obj.data.vertices), images, a["out"]))
    if a["grid"]:
        render_grid(obj, a["grid"])
        print("prep_scan: grid views in %s" % a["grid"])


if __name__ == "__main__":
    main()
