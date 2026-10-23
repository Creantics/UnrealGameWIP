"""Gives a scanned lead (rig_scan.py's .blend) the proof head's face: its 56 shapes and its jaw weights.

    blender -b RIG.blend --factory-startup --python-exit-code 1 --python tools/blender/face_transfer.py -- \\
        --face face.json --out name.glb [--blend name_face.blend] [--preview DIR]

face.json gives the scan's side of the ANCHORS correspondences below, in metres (Blender axes, facing -Y):
  explicit anchors ("eye_l", "crown", "neck"): [x, y, z];
  "front" anchors: [x, z], y is where a ray from the front first meets the mesh;
  "back" anchors: [x, z], from behind; "side" anchors: [y, z], from +x.
_l anchors are mirrored to _r. The proof head is warped onto the scan by a 3D thin-plate spline through
the anchors. Each scan vertex then takes the deltas of the nearest point on the warped proof skin (lids and
brows included; eyes, teeth, tongue and mouth bag excluded), carried through the warp: warp(p + d) - warp(p).
Vertices more than FAR metres from that surface, or below the neck anchor, get exact zeros, which keeps the
sparse morph targets small. The eye-look shapes carry only the lids following the gaze (the eyes are bones).
A scan has no tongue, so tongueOut pushes the lower lip forward instead: Unreal drops an all-zero target,
and the import needs all 56. The jaw weights are replaced by the proof head's jaw mask carried the same way
(heat weighting can't place that bone), or by face.json's "jaw" cut (jaw_mask) when the scan's mouth is far
wider than the proof's. face.json's "slit" ({"z", "half_width", "zone", "back_y"}) is for a mouth opened by
mouth_cut.py: near the cut (within "zone" of z, in front of "back_y") the jaw weight goes to 0 above z and 1 below,
fully in the middle and easing out to the corners at "half_width", faded into the carried weights, so the two lips
part though they lie a millimetre apart. mouth_cut.py's pocket (its "mouth" material) is split the same way at z and
takes no shape deltas: the jaw bone alone opens it.
"mouth_parts": true (or a list from "bag", "teeth", "tongue") then adds the proof's mouth bag, teeth and tongue to
the mesh, carried through the same warp with their own weights (head, jaw, tongue_01..03), materials and shape
deltas; leave out "bag" when mouth_cut.py made the pocket. "mouth_parts_fit" ({"scale", "offset"}) moves them as one
so they sit inside that pocket.
Import the result with FTImportCharacter -Face=1. --preview renders the warped proof over the scan, then
Basis, jawOpen (key plus jaw bone), mouthSmileLeft, mouthPucker, eyeBlinkLeft and browInnerUp, front view.
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bpy  # noqa: E402
import numpy as np  # noqa: E402
from mathutils import Vector  # noqa: E402
from mathutils.bvhtree import BVHTree  # noqa: E402

import face_spec as fsp  # noqa: E402
import make_leads as ml  # noqa: E402
import ne_blender as nb  # noqa: E402
import proof_head as ph  # noqa: E402
import rig_common as rc  # noqa: E402
import rig_spec as rs  # noqa: E402

NEAR, FAR = 0.008, 0.02  # full transfer within NEAR of the warped proof skin, none beyond FAR
TONGUE_PUSH, TONGUE_RADIUS = 0.002, 0.012  # tongueOut's stand-in: the lower lip pushed forward (no tongue in a scan)

# name -> (mode, proof head value); the scan's value comes from face.json in the same mode
ANCHORS = {
    "eye_l": ("point", (rs.EYE_X, rs.EYE_Y, rs.EYE_Z)),
    "mouth_l": ("front", (ph.MOUTH_W, rs.MOUTH_Z)),
    "upper_lip": ("front", (0.0, rs.MOUTH_Z + 0.006)),
    "lower_lip": ("front", (0.0, rs.MOUTH_Z - 0.008)),
    "nose_tip": ("nose", None),
    "chin": ("front", (0.0, 1.59)),
    "brow": ("front", (0.0, rs.EYE_Z + 0.04)),
    "brow_l": ("front", (rs.EYE_X, rs.EYE_Z + 0.03)),
    "crown": ("point", (0.0, rs.HEAD_CENTER[1], rs.HEAD_CENTER[2] + rs.HEAD_RADII[2])),
    "back": ("back", (0.0, rs.EYE_Z)),
    "side_l": ("side", (rs.HEAD_CENTER[1], rs.EYE_Z + 0.04)),
    "neck": ("point", (0.0, rs.HEAD_CENTER[1], rs.HEAD_CENTER[2] - rs.HEAD_RADII[2])),
}


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    a = {"face": None, "out": None, "blend": None, "preview": None}
    for i in range(0, len(argv), 2):
        k = argv[i][2:]
        if not argv[i].startswith("--") or k not in a or i + 1 >= len(argv):
            raise SystemExit("face_transfer: bad argument %s\n%s" % (argv[i], __doc__))
        a[k] = argv[i + 1]
    if not (a["face"] and a["out"]):
        raise SystemExit(__doc__)
    return a


def _ray(bvh, origin, direction):
    hit = bvh.ray_cast(Vector(origin), Vector(direction))
    if hit[0] is None:
        raise SystemExit("face_transfer: no surface along %r -> %r" % (origin, direction))
    return np.array(hit[0][:])


def resolve(bvh, mode, value, verts):
    """One anchor's 3D position on a mesh (bvh over its vertices `verts`)."""
    if mode == "point":
        return np.array(value, float)
    if mode == "front":
        return _ray(bvh, (value[0], -1.0, value[1]), (0.0, 1.0, 0.0))
    if mode == "back":
        return _ray(bvh, (value[0], 1.0, value[1]), (0.0, -1.0, 0.0))
    if mode == "side":
        return _ray(bvh, (1.0, value[0], value[1]), (-1.0, 0.0, 0.0))
    if mode == "side_r":
        return _ray(bvh, (-1.0, value[0], value[1]), (1.0, 0.0, 0.0))
    if mode == "nose":  # the proof's front-most centre-line skin point between mouth and eyes
        sel = (np.abs(verts[:, 0]) < 0.004) & (verts[:, 2] > rs.MOUTH_Z + 0.01) & (verts[:, 2] < rs.EYE_Z)
        return verts[sel][np.argmin(verts[sel][:, 1])]
    raise ValueError(mode)


def anchor_pairs(proof_bvh, proof_v, scan_bvh, scan_v, face):
    names, src, dst = [], [], []
    for name, (mode, pv) in ANCHORS.items():
        smode = "point" if mode == "nose" else mode
        for nm, sx in ((name, 1.0), (name[:-2] + "_r", -1.0)) if name.endswith("_l") else ((name, 1.0),):
            p = resolve(proof_bvh, *_mirror(pv, mode, sx), proof_v)
            q = resolve(scan_bvh, *_mirror(face[name], smode, sx), scan_v)
            names.append(nm)
            src.append(p)
            dst.append(q)
    return names, np.array(src), np.array(dst)


def _mirror(v, mode, sx):
    if v is None or sx > 0:
        return mode, v
    if mode == "point":
        return mode, (-v[0], v[1], v[2])
    if mode in ("front", "back"):
        return mode, (-v[0], v[1])
    if mode == "side":
        return "side_r", v
    raise ValueError("can't mirror a %s anchor" % mode)


class Tps:
    """3D thin-plate spline (kernel |r|) from src to dst points, exact at the anchors."""

    def __init__(self, src, dst):
        n = len(src)
        K = np.linalg.norm(src[:, None] - src[None], axis=2)
        Pm = np.hstack([np.ones((n, 1)), src])
        A = np.zeros((n + 4, n + 4))
        A[:n, :n], A[:n, n:], A[n:, :n] = K, Pm, Pm.T
        b = np.zeros((n + 4, 3))
        b[:n] = dst
        self.src, self.coef = src, np.linalg.solve(A, b)

    def __call__(self, P):
        P = np.atleast_2d(P)
        K = np.linalg.norm(P[:, None] - self.src[None], axis=2)
        n = len(self.src)
        return K @ self.coef[:n] + self.coef[n] + P @ self.coef[n + 1:]


def corner_weights(bvh, faces, verts, points):
    """For each point: (distance to the surface, corner indices, inverse-distance corner weights)."""
    out = []
    for p in points:
        loc, _n, fi, d = bvh.find_nearest(Vector(p))
        f = faces[fi]
        dd = np.linalg.norm(verts[list(f)] - np.array(loc[:]), axis=1) + 1e-6
        w = (1.0 / dd) / (1.0 / dd).sum()
        out.append((d, list(f), w))
    return out


def jaw_mask(SV, spec):
    """face.json "jaw": {"cut_z", "pivot": [x, y, z], "front_y", "blend", "neck_z": [lo, hi]} -> {vertex: weight}.
    The jaw takes what lies below a cut through the mouth slit (cut_z at front_y) that rises to the pivot
    under the ear, softened over `blend` metres. Nothing behind the pivot. Under the chin (behind front_y) it
    fades out between neck_z hi and lo, so the neck stays on the head chain."""
    py, pz = spec["pivot"][1], spec["pivot"][2]
    fy, cz, b = spec["front_y"], spec["cut_z"], spec["blend"]
    lo, hi = spec["neck_z"]
    x, y, z = SV[:, 0], SV[:, 1], SV[:, 2]
    t = np.clip((y - fy) / (py - fy), 0.0, 1.0)
    cut = cz + (pz - cz) * t
    w = _smooth((cut + b / 2 - z) / b)
    w *= _smooth((py - y) / 0.015)
    w *= np.where(y > fy, _smooth((z - lo) / (hi - lo)), 1.0)
    w *= z > lo
    return {int(i): float(w[i]) for i in np.nonzero(w > 1e-3)[0]}


def slit_across(x, spec):
    """How fully the slit overrides the jaw weight at x: 1 in the middle, easing to 0 at the lip corners (half_width)
    so the corners keep their carried weight and the mouth opens as a lens, not a box."""
    hw = spec["half_width"]
    return _smooth((hw - np.abs(x)) / max(spec["zone"], 0.5 * hw))


def slit_weights(SV, jaw, spec):
    """Overrides jaw weights next to a cut mouth: 0 above the slit, 1 below, faded out over "zone" (see the doc)."""
    sz, zone, by = spec["z"], spec["zone"], spec["back_y"]
    y, z = SV[:, 1], SV[:, 2]
    s = _smooth(1.0 - np.abs(z - sz) / zone) * slit_across(SV[:, 0], spec) * (y < by)
    out = dict(jaw)
    for i in np.nonzero(s > 1e-3)[0]:
        j = (1.0 - s[i]) * jaw.get(int(i), 0.0) + s[i] * float(z[i] < sz)
        if j > 1e-3:
            out[int(i)] = float(j)
        else:
            out.pop(int(i), None)
    return out


def pocket_verts(mesh):
    """Vertices of mouth_cut.py's pocket: those whose faces all use its "mouth" material."""
    mi = [k for k, m in enumerate(mesh.data.materials) if m and m.name.split(".")[0] == "mouth"]
    if not mi:
        return np.zeros(0, dtype=int)
    other = set()
    mine = set()
    for p in mesh.data.polygons:
        (mine if p.material_index == mi[0] else other).update(p.vertices)
    return np.array(sorted(mine - other), dtype=int)


def pocket_weights(SV, jaw, pocket, spec, blend=0.003):
    """The pocket follows the jaw below the slit and the head above it, split within `blend` of z, so its floor drops
    with the lower teeth; toward its ends the split eases into the carried weights as the lips' does (slit_across)."""
    out = dict(jaw)
    s = slit_across(SV[pocket, 0], spec)
    for k, i in enumerate(pocket):
        split = float(_smooth(0.5 + (spec["z"] - SV[i, 2]) / (2.0 * blend)))
        j = (1.0 - s[k]) * jaw.get(int(i), 0.0) + s[k] * split
        if j > 1e-3:
            out[int(i)] = float(j)
        else:
            out.pop(int(i), None)
    return out


def fit_parts(P, spec):
    """face.json "mouth_parts_fit" {"scale": [sx, sy, sz], "offset": [dx, dy, dz]}: the parts scaled about the middle of
    their front (x 0, their frontmost y, the middle of their height), then moved."""
    if not spec:
        return P
    pivot = np.array([0.0, P[:, 1].min(), 0.5 * (P[:, 2].min() + P[:, 2].max())])
    scale = np.array(spec.get("scale", (1.0, 1.0, 1.0)))
    return pivot + (P - pivot) * scale + np.array(spec.get("offset", (0.0, 0.0, 0.0)))


def add_mouth_parts(mesh, HB, WV, which, fit=None):
    """Appends the proof's bag, teeth and tongue (positions WV, the warped proof) to `mesh` with their weights and
    materials, placed by fit_parts. Returns (the proof vertex index of each new vertex in order, their positions)."""
    import bmesh
    isl = np.array(HB.island)
    names = ("bag", "teeth", "tongue") if which is True else which
    keep = {"bag": (ph.BAG,), "teeth": (ph.TEETH_U, ph.TEETH_L), "tongue": (ph.TONGUE,)}
    keep = tuple(i for n in names for i in keep[n])
    parts = [i for i in range(len(isl)) if isl[i] in keep]
    new = {pi: k for k, pi in enumerate(parts)}
    P = fit_parts(WV[parts], fit)
    mats = {}
    for name in ("teeth", "tongue", "mouth"):
        m = bpy.data.materials.get("ne_" + name) or bpy.data.materials.new("ne_" + name)
        m.diffuse_color = ph.MAT_COLOR[name] + (1.0,)
        m.use_nodes = True
        m.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = m.diffuse_color
        if m.name not in mesh.data.materials:
            mesh.data.materials.append(m)
        mats[ph.MAT[name]] = list(mesh.data.materials).index(m)
    bm = bmesh.new()
    bm.from_mesh(mesh.data)
    n0 = len(bm.verts)
    for p in P:
        bm.verts.new(p)
    bm.verts.ensure_lookup_table()
    for idx, mat in HB.faces:
        if all(i in new for i in idx):
            f = bm.faces.new([bm.verts[n0 + new[i]] for i in idx])
            f.material_index = mats[mat]
    bm.to_mesh(mesh.data)
    bm.free()
    for k, pi in enumerate(parts):
        for bone, w in HB.weights[pi].items():
            g = mesh.vertex_groups.get(bone) or mesh.vertex_groups.new(name=bone)
            g.add([n0 + k], w, "REPLACE")
    return np.array(parts), P


def _smooth(t):
    t = np.clip(t, 0.0, 1.0)
    return t * t * (3 - 2 * t)


def set_jaw_weights(mesh, arm, jaw):
    """jaw: {vertex: weight}. Every other vertex loses its jaw weight; each vertex keeps its top 4."""
    groups = {g.index: g.name for g in mesh.vertex_groups}
    vg = mesh.vertex_groups
    for v in mesh.data.vertices:
        w = {groups[g.group]: g.weight for g in v.groups if g.weight > 0.0}
        j = jaw.get(v.index, 0.0)
        if "jaw" not in w and j <= 0.0:
            continue
        w.pop("jaw", None)
        if not w:  # weighted to the jaw alone (heat weighting can reach the jaw on a scan): the head takes it
            w = {"head": 1.0}
        total = sum(w.values()) or 1.0
        w = {k: x * (1.0 - j) / total for k, x in w.items()}
        if j > 0.0:
            w["jaw"] = j
        keep = dict(sorted(w.items(), key=lambda kv: -kv[1])[:4])
        s = sum(keep.values())
        for g in list(v.groups):
            vg[g.group].remove([v.index])
        for k, x in keep.items():
            if x / s > 1e-4:
                (vg.get(k) or vg.new(name=k)).add([v.index], x / s, "REPLACE")


def jaw_sign(arm, mesh, chin):
    """+1 if a positive local-X rotation of the jaw bone lowers the chin, else -1."""
    pb = arm.pose.bones["jaw"]
    pb.rotation_mode = "XYZ"
    pb.rotation_euler = (math.radians(20), 0.0, 0.0)
    bpy.context.view_layer.update()
    z = rc.evaluated_positions(mesh)[chin][2]
    pb.rotation_euler = (0.0, 0.0, 0.0)
    bpy.context.view_layer.update()
    return 1.0 if z < mesh.data.vertices[chin].co.z else -1.0


def preview(arm, mesh, warped, outdir, sign):
    os.makedirs(outdir, exist_ok=True)
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_WORKBENCH"
    sc.display.shading.light = "STUDIO"
    sc.display.shading.color_type = "TEXTURE"
    sc.render.resolution_x = sc.render.resolution_y = 640
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    cam.data.type, cam.data.ortho_scale = "ORTHO", 0.36
    top = max(v.co.z for v in mesh.data.vertices)
    eyes_z = top - 0.13
    cam.location, cam.rotation_euler = (0.0, -2.0, eyes_z), (math.radians(90), 0.0, 0.0)
    sc.collection.objects.link(cam)
    sc.camera = cam

    def shoot(tag):
        sc.render.filepath = os.path.join(outdir, tag + ".png")
        bpy.ops.render.render(write_still=True)

    arm.animation_data.action = None
    rc.reset_pose(arm)
    red = bpy.data.materials.new("warp")
    red.diffuse_color = (1.0, 0.1, 0.1, 1.0)
    warped.data.materials.append(red)
    sc.display.shading.show_xray, sc.display.shading.xray_alpha = True, 0.5
    shoot("warp_front")
    cam.location, cam.rotation_euler = (2.0, 0.0, eyes_z), (math.radians(90), 0.0, math.radians(90))
    shoot("warp_side")
    sc.display.shading.show_xray = False
    warped.hide_render = True
    cam.location, cam.rotation_euler = (0.0, -2.0, eyes_z), (math.radians(90), 0.0, 0.0)
    keys = mesh.data.shape_keys.key_blocks
    shoot("basis")
    jaw = arm.pose.bones["jaw"]
    jaw.rotation_mode = "XYZ"
    for name in ("jawOpen", "mouthSmileLeft", "mouthPucker", "eyeBlinkLeft", "browInnerUp"):
        keys[name].value = 1.0
        if name == "jawOpen":
            jaw.rotation_euler = (sign * math.radians(fsp.JAW_MAX_DEG), 0.0, 0.0)
        bpy.context.view_layer.update()
        shoot(name)
        keys[name].value = 0.0
        jaw.rotation_euler = (0.0, 0.0, 0.0)
    bpy.context.view_layer.update()
    bpy.data.objects.remove(cam, do_unlink=True)


def main():
    a = _args()
    with open(a["face"]) as f:
        face = json.load(f)
    arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
    mesh = next(o for o in bpy.data.objects if o.type == "MESH")
    if mesh.data.shape_keys:
        raise SystemExit("face_transfer: the mesh already has shape keys (start from rig_scan's .blend)")
    arm.animation_data.action = None
    rc.reset_pose(arm)
    bpy.context.view_layer.update()

    HB = ph.build()
    shapes, _masks = ph.all_shapes(HB)
    PV = np.array(HB.v)
    isl = np.array(HB.island)
    skin_faces = [tuple(f) for f, _ in HB.faces if all(isl[i] in (ph.SKIN, ph.LID_U, ph.LID_L, ph.BROW) for i in f)]
    SV = np.array([v.co[:] for v in mesh.data.vertices])
    SF = [tuple(p.vertices) for p in mesh.data.polygons]
    scan_bvh = BVHTree.FromPolygons([tuple(p) for p in SV], SF)
    proof_skin_bvh = BVHTree.FromPolygons([tuple(p) for p in PV], skin_faces)

    names, src, dst = anchor_pairs(proof_skin_bvh, PV, scan_bvh, SV, face)
    for n, p, q in zip(names, src, dst):
        print("anchor %-10s proof %s scan %s" % (n, np.round(p, 3), np.round(q, 3)))
    warp = Tps(src, dst)
    WV = warp(PV)
    neck_z = dst[names.index("neck")][2]
    warped_bvh = BVHTree.FromPolygons([tuple(p) for p in WV], skin_faces)

    region = np.where((SV[:, 2] > neck_z) & (np.linalg.norm(SV - WV.mean(axis=0), axis=1) < 0.25))[0]
    cw = corner_weights(warped_bvh, skin_faces, WV, SV[region])
    fall = np.array([1.0 if d <= NEAR else max(0.0, 1.0 - (d - NEAR) / (FAR - NEAR)) for d, _, _ in cw])
    fall = fall * fall * (3 - 2 * fall)

    parts = None
    if face.get("mouth_parts"):
        parts, PP = add_mouth_parts(mesh, HB, WV, face["mouth_parts"], face.get("mouth_parts_fit"))
    n_scan = len(SV)
    if parts is not None:
        SV = np.vstack([SV, PP])
    pocket = pocket_verts(mesh) if "slit" in face else np.zeros(0, dtype=int)

    mesh.shape_key_add(name="Basis", from_mix=False)
    moved = {}
    for sname in fsp.ALL_SHAPES:
        D = np.zeros_like(SV)
        if sname == "tongueOut":
            r = np.linalg.norm(SV - dst[names.index("lower_lip")], axis=1)
            D[:, 1] = -TONGUE_PUSH * _smooth(1.0 - r / TONGUE_RADIUS)
        else:
            DW = warp(PV + shapes[sname]) - WV
            for k, (vi, (_d, corners, w)) in enumerate(zip(region, cw)):
                if fall[k] > 0.0:
                    D[vi] = fall[k] * (w @ DW[corners])
            if parts is not None:
                D[n_scan:] = DW[parts]
            D[pocket] = 0.0
            D[np.abs(D) < 1e-5] = 0.0
        kb = mesh.shape_key_add(name=sname, from_mix=False)
        kb.data.foreach_set("co", (SV + D).astype(np.float32).ravel())
        kb.slider_min, kb.slider_max = 0.0, 1.0
        moved[sname] = int(np.count_nonzero(np.abs(D).sum(axis=1)))
    for kb in mesh.data.shape_keys.key_blocks:
        kb.value = 0.0
    mesh["ne_face_shape_set"] = "face_presets.json v%d" % fsp.VERSION

    if "jaw" in face:
        jaw = jaw_mask(SV, face["jaw"])
    else:
        wj = np.array([w.get("jaw", 0.0) for w in HB.weights])
        jaw = {}
        for k, (vi, (_d, corners, w)) in enumerate(zip(region, cw)):
            j = float(fall[k] * (w @ wj[corners]))
            if j > 1e-3:
                jaw[int(vi)] = j
    if "slit" in face:
        jaw = slit_weights(SV[:n_scan], jaw, face["slit"])
        jaw = pocket_weights(SV, jaw, pocket, face["slit"])
    if parts is not None:
        for k, pi in enumerate(parts):
            if HB.weights[pi].get("jaw", 0.0) > 0.0:
                jaw[n_scan + k] = HB.weights[pi]["jaw"]
    set_jaw_weights(mesh, arm, jaw)
    chin = int(region[np.argmin(np.linalg.norm(SV[region] - dst[names.index("chin")], axis=1))])
    sign = jaw_sign(arm, mesh, chin)

    warped = nb.mesh_object("proof_warped", [tuple(p) for p in WV], skin_faces)
    if a["preview"]:
        preview(arm, mesh, warped, a["preview"], sign)
    bpy.data.objects.remove(warped, do_unlink=True)

    arm.animation_data.action = None
    rc.reset_pose(arm)
    bpy.context.scene.frame_set(0)
    bpy.context.view_layer.update()
    out = os.path.abspath(a["out"])
    glb = nb.export_rig(out, {"export_morph_normal": False, "export_force_sampling": False})
    rc.write_rigexpect(arm, mesh, os.path.splitext(out)[0] + ".rigexpect.json")
    info = ml.check_glb(glb, face=True)
    if a["blend"]:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(a["blend"]))
    top = max(moved.items(), key=lambda kv: kv[1])
    print("face_transfer: %s, anchor fit %.2g m, region %d verts, jaw-weighted %d, jaw sign %+d, most moved %s %d,"
          " %d bytes -> %s" % (info, float(np.abs(warp(src) - dst).max()), len(region), len(jaw), sign, top[0], top[1],
                               os.path.getsize(glb), glb))


if __name__ == "__main__":
    main()
