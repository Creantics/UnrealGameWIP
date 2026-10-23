"""Swaps a generated head's built-in eyeballs for rigged ones, turns its skin to face out and rewrites the blinks,
headless, after face_transfer.

    blender -b FACE.blend --factory-startup --python-exit-code 1 --python tools/blender/eye_swap.py -- \\
        --out name.glb [--blend name.blend] [--preview DIR] [--iris 0.12,0.07,0.04] [--search 0.022]

A generator (Tripo and the like) puts a sphere behind each lid opening, welded into the one body mesh, with a cone
inside it for the pupil. It cannot turn, and nothing shines. For each of eye_l / eye_r this fits a sphere to the
vertices round the joint (within `search`, the back half only, so the lids do not pull the fit), removes every face
whose corners all lie on or inside it, and puts an eyeball of the same size there (sclera, iris, pupil; looking
down -y) weighted wholly to the joint. The eyeball sits `inset` inside the old sphere so the lids, which were shaped
round it, still cover its rim, and it is in every face shape with no delta. The front of the old ball bulges like a cornea; the cut follows it there and the
new ball copies the bulge.

A generated mesh's winding is mixed (on Devin, 40k skin faces faced in and would be holes in the engine), so every
skin face is turned to face out (outward_normals). The generic eyeBlinkLeft/Right from face_transfer drop the brow and
socket instead of closing the lids, so they are rewritten: the lids turn over the new ball about its hinge until they
meet (blink). The other eye shapes (look, squint, wide) are left as face_transfer made them. FACE.blend is
face_transfer's .blend.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bmesh  # noqa: E402
import bpy  # noqa: E402
import numpy as np  # noqa: E402
from mathutils.bvhtree import BVHTree  # noqa: E402

import make_leads as ml  # noqa: E402
import ne_blender as nb  # noqa: E402
import rig_common as rc  # noqa: E402

TOL = 0.0005          # a face within this of the fitted sphere (or inside it) is the old eyeball's; lids hug it at +0.6 mm
CAP_TOL = 0.0024      # ... within this at the front, where the old eyeball bulges like a cornea and the lids stand off
CAP, CAP_END = 35.0, 50.0  # degrees from straight ahead where the front tolerance holds, and where it is back to TOL
SHARD = 40            # face islands left round the eye with fewer faces than this are the old eyeball's too
INSET = 0.0006        # the new eyeball's radius is this much under the old one
IRIS, PUPIL = 0.36, 0.13  # iris and pupil radii as fractions of the eyeball's
SCLERA, PUPIL_RGB = (0.80, 0.77, 0.72), (0.004, 0.004, 0.004)


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    a = {"out": None, "blend": None, "preview": None, "iris": "0.12,0.07,0.04", "search": "0.022"}
    i = 0
    while i < len(argv):
        k = argv[i][2:]
        if not argv[i].startswith("--") or k not in a or i + 1 >= len(argv):
            raise SystemExit("eye_swap: unknown argument %s" % argv[i])
        a[k] = argv[i + 1]
        i += 2
    if not a["out"]:
        raise SystemExit(__doc__)
    a["iris"] = tuple(float(c) for c in a["iris"].split(","))
    a["search"] = float(a["search"])
    return a


def fit_sphere(P, guess, search):
    """Least-squares sphere through the points within `search` of the guess and behind it; refit 6 times."""
    c = np.array(guess, dtype=float)
    R = search
    for _ in range(6):
        d = np.linalg.norm(P - c, axis=1)
        s = P[(d > 0.35 * search) & (d < search) & (P[:, 1] > c[1] - 0.25 * search)]
        if len(s) < 12:
            raise SystemExit("eye_swap: only %d vertices round %s; is there a built-in eyeball?" % (len(s), guess))
        A = np.c_[2 * s, np.ones(len(s))]
        k = np.linalg.lstsq(A, (s ** 2).sum(1), rcond=None)[0]
        c = k[:3]
        R = math.sqrt(k[3] + (c ** 2).sum())
    d = np.linalg.norm(s - c, axis=1)
    return c, R, float(np.percentile(np.abs(d - R), 90))


def _angle(P, c):
    """Degrees between each point's direction from c and straight ahead (-y)."""
    d = P - c
    return np.degrees(np.arccos(np.clip(-d[:, 1] / np.maximum(np.linalg.norm(d, axis=1), 1e-9), -1.0, 1.0)))


def _tol(theta):
    t = np.clip((theta - CAP) / (CAP_END - CAP), 0.0, 1.0)
    return CAP_TOL + (TOL - CAP_TOL) * t


def remove_inside(mesh, c, R):
    """Deletes the faces whose corners all lie within R + tol of c (tol wider at the front, where the old eyeball
    bulges), then the small islands that leaves near the eye (shards of the old sphere a little outside the fit).
    Returns (faces removed, the bulge: median distance outside R of the removed corners in 5-degree bands from
    straight ahead, out to CAP_END)."""
    bm = bmesh.new()
    bm.from_mesh(mesh.data)
    P = np.array([v.co[:] for v in bm.verts])
    lim = R + _tol(_angle(P, c))
    inside = np.linalg.norm(P - c, axis=1) < lim
    bm.verts.index_update()
    gone = [f for f in bm.faces if all(inside[v.index] for v in f.verts)]
    corners = sorted({v.index for f in gone for v in f.verts})
    bmesh.ops.delete(bm, geom=gone, context="FACES")
    seen, shards = set(), []
    for f in bm.faces:
        if f in seen or np.linalg.norm(np.array(f.calc_center_median()) - c) > R + 0.004:
            continue
        isl, stack = [], [f]
        seen.add(f)
        while stack:
            g = stack.pop()
            isl.append(g)
            for e in g.edges:
                for h in e.link_faces:
                    if h not in seen:
                        seen.add(h)
                        stack.append(h)
        if len(isl) < SHARD:
            shards += isl
    bmesh.ops.delete(bm, geom=shards, context="FACES")
    gone += shards
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context="VERTS")
    bm.to_mesh(mesh.data)
    bm.free()
    mesh.data.update()
    Q = P[corners]
    th, out = _angle(Q, c), np.linalg.norm(Q - c, axis=1) - R
    bulge = []
    for a in np.arange(0.0, CAP_END, 5.0):
        sel = (th >= a) & (th < a + 5.0) & (out > -0.002)  # not the pupil cone deep inside
        bulge.append(float(np.clip(np.median(out[sel]), 0.0, CAP_TOL)) if sel.sum() >= 3 else 0.0)
    for i in range(len(bulge) - 2, -1, -1):  # never thinner toward the middle
        bulge[i] = max(bulge[i], bulge[i + 1])
    return len(gone), bulge




def outward_normals(mesh, passes=50):
    """Turns every skin face (material slot 0) to face out: a generated mesh's winding is mixed, and the engine draws
    one side only. After Blender's recalculation, each face casts a ray along its normal and one against it: if only
    the one behind gets out of the body, the face is turned. A face where both or neither get out (in a crease, or a
    thin part) takes the winding of most of its decided neighbours, spreading out pass by pass. The mouth pocket,
    teeth and tongue keep theirs (the pocket faces in), and the eyeballs are added after, facing out. Returns how many
    faces turned."""
    bm = bmesh.new()
    bm.from_mesh(mesh.data)
    skin = [f for f in bm.faces if f.material_index == 0]
    before = {f: f.normal.copy() for f in skin}
    bmesh.ops.recalc_face_normals(bm, faces=skin)
    bm.normal_update()
    tree = BVHTree.FromBMesh(bm)

    def free(o, d):
        return tree.ray_cast(o + d * 1e-5, d, 10.0)[0] is None

    decided = set()
    for f in skin:
        o, n = f.calc_center_median(), f.normal.copy()
        out, back = free(o, n), free(o, -n)
        if out != back:
            if back:
                f.normal_flip()
            decided.add(f)

    def agrees(g, h, e):
        lg = next(x for x in g.loops if x.edge is e)
        lh = next(x for x in h.loops if x.edge is e)
        return lg.vert is not lh.vert

    for _ in range(passes):
        grown = []
        for g in skin:
            if g in decided:
                continue
            votes = 0
            for e in g.edges:
                if len(e.link_faces) != 2:
                    continue
                h = e.link_faces[0] if e.link_faces[1] is g else e.link_faces[1]
                if h in decided:
                    votes += 1 if agrees(g, h, e) else -1
            if votes:
                grown.append((g, votes < 0))
        if not grown:
            break
        for g, flip in grown:
            if flip:
                g.normal_flip()
            decided.add(g)
    bm.normal_update()
    turned = sum(1 for f in skin if f.normal.dot(before[f]) < 0.0)
    bm.to_mesh(mesh.data)
    bm.free()
    mesh.data.update()
    return turned


def _lid_angles(P, c):
    """(azimuth, elevation) in degrees about c: azimuth from straight ahead toward +x, elevation from straight ahead
    toward +z in the plane through the x axis (the blink's hinge)."""
    v = P - c
    return np.degrees(np.arctan2(v[:, 0], -v[:, 1])), np.degrees(np.arctan2(v[:, 2], np.hypot(v[:, 0], v[:, 1])))


def blink(mesh, key, c, R, close=0.3, reach=0.007, fall=22.0):
    """Rewrites the blink shape `key` for the eye at c: the lids turn about the eye's hinge (the x axis through c)
    over the eyeball until they meet at `close` of the way up from the lower lid's edge to the upper's, so they slide
    on the ball instead of dropping through it. The lid edges are the rim of the hole the old eyeball left. A vertex
    turns fully within 3 mm of the ball's surface, fading out by `reach`, and fully up to 6 degrees past its lid's
    edge, fading out `fall` degrees past it. The rest of the key round the eye (a generic shape's brow drop) is
    cleared. Returns the largest move in metres."""
    keys = mesh.data.shape_keys.key_blocks
    if key not in keys:
        return 0.0
    n = len(mesh.data.vertices)
    base = np.zeros(n * 3)
    keys[0].data.foreach_get("co", base)
    base = base.reshape(-1, 3)
    bm = bmesh.new()
    bm.from_mesh(mesh.data)
    rim = sorted({v.index for e in bm.edges if e.is_boundary for v in e.verts
                  if np.linalg.norm(np.array(v.co) - c) < R + 0.006 and v.co.y < c[1]})
    bm.free()
    if len(rim) < 8:
        raise SystemExit("eye_swap: no lid rim round %s" % np.round(c, 4))
    az, el = _lid_angles(base[rim], c)
    bins = np.linspace(az.min(), az.max(), 13)
    mids, up, lo = [], [], []
    for a0, a1 in zip(bins[:-1], bins[1:]):
        sel = (az >= a0) & (az <= a1)
        if sel.sum():
            mids.append(0.5 * (a0 + a1))
            up.append(el[sel].max())
            lo.append(el[sel].min())
    mids, up, lo = np.array(mids), np.array(up), np.array(lo)
    fu, fl = np.polyval(np.polyfit(mids, up, 2), mids), np.polyval(np.polyfit(mids, lo, 2), mids)
    up = fu + max(0.0, float((up - fu).max()))  # the lid edges as smooth arcs, not the rim's jags, moved out to
    lo = fl - max(0.0, float((fl - lo).max()))  # the furthest rim vertex so every margin vertex turns far enough
    d = np.linalg.norm(base - c, axis=1)
    A, E = _lid_angles(base, c)
    near = (d < R + reach) & (d > R - 0.004) & (base[:, 1] < c[1] + 0.004) & (A > az.min() - 4) & (A < az.max() + 4)
    u = np.interp(A, mids, up)
    lw = np.interp(A, mids, lo)
    meet = lw + close * (u - lw)
    w_rad = np.clip((R + reach - d) / (reach - 0.003), 0.0, 1.0)
    turn = np.zeros(n)
    above = near & (E >= meet)
    w = np.clip(1.0 - (E - u - 6.0) / fall, 0.0, 1.0) * w_rad
    turn[above] = -((u - meet) + 1.0)[above] * w[above]
    below = near & (E < meet)
    w = np.clip(1.0 - (lw - E - 6.0) / fall, 0.0, 1.0) * w_rad
    turn[below] = (meet - lw)[below] * w[below]
    # taper to nothing at the corners, where the lids already meet
    span = az.max() - az.min()
    edge = np.clip(np.minimum(A - az.min(), az.max() - A) / (0.12 * span), 0.0, 1.0)
    turn *= edge
    v = base - c
    ang = np.radians(turn)
    h = np.hypot(v[:, 0], v[:, 1])
    el0 = np.arctan2(v[:, 2], h)
    rr = np.hypot(h, v[:, 2])
    new = base.copy()
    scale = np.cos(el0 + ang) * rr / np.maximum(h, 1e-9)
    new[:, 0] = c[0] + v[:, 0] * scale
    new[:, 1] = c[1] + v[:, 1] * scale
    new[:, 2] = c[2] + np.sin(el0 + ang) * rr
    co = np.zeros(n * 3)
    keys[key].data.foreach_get("co", co)
    co = co.reshape(-1, 3)
    clear = np.linalg.norm(base - c, axis=1) < 0.035
    co[clear] = base[clear]
    co[near] = new[near]
    keys[key].data.foreach_set("co", co.ravel())
    mesh.data.update()
    return float(np.linalg.norm(co - base, axis=1).max())


def _material(name, rgb, rough):
    mat, _nt, bsdf = nb.new_material(name)
    bsdf.inputs["Base Color"].default_value = tuple(rgb) + (1.0,)
    bsdf.inputs["Roughness"].default_value = rough
    mat.diffuse_color = tuple(rgb) + (1.0,)
    return mat


def eyeball(mesh, side, joint, c, r, iris_rgb, bulge=(), rings=24, segs=36):
    """A UV sphere on `joint`, its pole looking down -y, banded into sclera / iris / pupil; the front pushed out by
    `bulge` (metres per 5-degree band from straight ahead) to the old eyeball's cornea."""
    V, F, fmat = [], [], []
    for i in range(rings + 1):
        th = math.pi * i / rings
        for j in range(segs):
            ph = 2 * math.pi * j / segs
            deg = math.degrees(th)
            b = float(np.interp(deg, [2.5 + 5.0 * k for k in range(len(bulge))], bulge, right=0.0)) if bulge else 0.0
            V.append(c + (r + b) * np.array([math.sin(th) * math.cos(ph), -math.cos(th), math.sin(th) * math.sin(ph)]))
    for i in range(rings):
        for j in range(segs):
            a, b = i * segs + j, i * segs + (j + 1) % segs
            F.append((a, a + segs, b + segs, b))
            th = math.pi * (i + 0.5) / rings
            rad = math.sin(th) if th < math.pi / 2 else 1.0
            fmat.append(2 if rad < PUPIL else (1 if rad < IRIS else 0))
    mats = [_material("eye_sclera_" + side, SCLERA, 0.15), _material("eye_iris_" + side, iris_rgb, 0.2),
            _material("eye_pupil_" + side, PUPIL_RGB, 0.1)]
    obj = nb.mesh_object("eye_" + side, [tuple(float(x) for x in p) for p in V], F)
    for m in mats:
        obj.data.materials.append(m)
    for p, mi in zip(obj.data.polygons, fmat):
        p.material_index = mi
        p.use_smooth = True
    obj.vertex_groups.new(name=joint).add(list(range(len(V))), 1.0, "REPLACE")
    nb.activate(mesh)
    obj.select_set(True)
    bpy.ops.object.join()
    return len(V)


def preview(arm, mesh, outdir, c):
    """Front close-ups of the eyes: rest, a blink, and both eyes turned to the left and down."""
    os.makedirs(outdir, exist_ok=True)
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_EEVEE" if "BLENDER_EEVEE" in [e.identifier for e in
                                                               bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items] else "BLENDER_WORKBENCH"
    sc.render.resolution_x, sc.render.resolution_y = 1000, 500
    cam = bpy.data.objects.new("eye_cam", bpy.data.cameras.new("eye_cam"))
    sc.collection.objects.link(cam)
    sc.camera = cam
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = 0.14
    cam.location = (0.0, -1.0, c[2])
    cam.rotation_euler = (math.pi / 2, 0.0, 0.0)
    if not sc.world:
        sc.world = bpy.data.worlds.new("w")
    sc.world.color = (0.6, 0.6, 0.6)
    light = bpy.data.objects.new("eye_key", bpy.data.lights.new("eye_key", "SUN"))
    sc.collection.objects.link(light)
    light.rotation_euler = (math.radians(60), 0.0, math.radians(-20))
    keys = mesh.data.shape_keys.key_blocks

    def shoot(tag):
        bpy.context.view_layer.update()
        sc.render.filepath = os.path.join(outdir, tag + ".png")
        bpy.ops.render.render(write_still=True)

    shoot("rest")
    for name in ("eyeBlinkLeft", "eyeBlinkRight"):
        if name in keys:
            keys[name].value = 1.0
    shoot("blink")
    for kb in keys:
        kb.value = 0.0
    for j in ("eye_l", "eye_r"):
        pb = arm.pose.bones[j]
        pb.rotation_mode = "XYZ"
        pb.rotation_euler = (math.radians(12), 0.0, math.radians(20))
    shoot("look")
    rc.reset_pose(arm)


def main():
    a = _args()
    arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
    mesh = next(o for o in bpy.data.objects if o.type == "MESH" and any(m.type == "ARMATURE" for m in o.modifiers))
    if arm.animation_data:
        arm.animation_data.action = None
    rc.reset_pose(arm)
    bpy.context.view_layer.update()
    if mesh.matrix_world != mesh.matrix_world.Identity(4):
        raise SystemExit("eye_swap: the mesh has an object transform; apply it first")
    report, centres = {"turned": 0}, []
    fits = []
    for side, joint in (("l", "eye_l"), ("r", "eye_r")):
        if joint not in arm.data.bones:
            raise SystemExit("eye_swap: no %s joint" % joint)
        guess = np.array(arm.matrix_world @ arm.data.bones[joint].head_local)
        P = np.array([v.co[:] for v in mesh.data.vertices])
        c, R, res = fit_sphere(P, guess, a["search"])
        if res > 0.15 * R:
            raise SystemExit("eye_swap: %s fit is loose (90th percentile off by %.4f of R %.4f)" % (joint, res, R))
        gone, bulge = remove_inside(mesh, c, R)
        fits.append((side, joint, c, R, bulge))
        report[joint] = {"centre": [round(float(x), 4) for x in c], "radius": round(R, 4), "fit_p90": round(res, 5),
                         "off_joint": round(float(np.linalg.norm(c - guess)), 4), "removed": gone,
                         "bulge_mm": [round(b * 1000, 1) for b in bulge]}
    report["turned"] = outward_normals(mesh)
    for side, joint, c, R, _bulge in fits:
        report[joint]["blink_mm"] = round(1000 * blink(mesh, "eyeBlinkLeft" if side == "l" else "eyeBlinkRight", c, R), 1)
    for side, joint, c, R, bulge in fits:
        report[joint]["added"] = eyeball(mesh, side, joint, c, R - INSET, a["iris"], bulge)
        centres.append(c)
    for kb in mesh.data.shape_keys.key_blocks if mesh.data.shape_keys else ():
        kb.value = 0.0
    if a["preview"]:
        preview(arm, mesh, os.path.abspath(a["preview"]), np.mean(centres, axis=0))
    rc.reset_pose(arm)
    bpy.context.scene.frame_set(0)
    bpy.context.view_layer.update()
    out = os.path.abspath(a["out"])
    os.makedirs(os.path.dirname(out), exist_ok=True)
    glb = nb.export_rig(out, {"export_morph_normal": False, "export_force_sampling": False})
    rc.write_rigexpect(arm, mesh, os.path.splitext(out)[0] + ".rigexpect.json")
    info = ml.check_glb(glb, face=True)
    if a["blend"]:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(a["blend"]))
    print("eye_swap: %s, %s, %d bytes -> %s" % (info, report, os.path.getsize(glb), glb))


if __name__ == "__main__":
    main()
