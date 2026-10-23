"""Adds toe joints to a rigged body after the canonical chain is done, headless: two per toe under each ball joint,
named like the engine's humanoid (bigtoe_01_l, bigtoe_02_l, indextoe_..., middletoe_..., ringtoe_..., littletoe_...),
with the toe weights moved off the ball onto them.

    blender -b BODY.blend --factory-startup --python-exit-code 1 --python tools/blender/add_toes.py -- \\
        --out OUT.glb [--blend OUT.blend] [--body OBJECT] [--preview DIR]

The canonical skeleton stops at ball_l/ball_r, and every chain tool checks for exactly that joint list, so this runs
last (before shell_bind and garment_shells, which copy whatever skeleton the body has). For each foot it takes the
skin in front of the ball joint, puts the toe base 65 mm behind the tip of the furthest toe and splits the row's
width there into anatomical toe widths, the big toe on the inner side (generated feet often have fused toes, so the
clefts can't be read off the surface). Each toe gets a proximal joint at the base and a distal one at 55 % of the way
to its tip; weights blend in over 12 mm round the proximal joints and across 3 mm between neighbouring toes. --preview renders both soles (from below) and the side of the feet at rest and with the toes curled.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bpy  # noqa: E402
import numpy as np  # noqa: E402
from mathutils import Vector  # noqa: E402

import ne_blender as nb  # noqa: E402
import rig_common as rc  # noqa: E402

TOES = ("bigtoe", "indextoe", "middletoe", "ringtoe", "littletoe")
WIDTHS = np.array([0.30, 0.19, 0.18, 0.17, 0.16])  # share of the toe row, inner to outer
BLEND_IN = 0.012
SIDE_BLEND = 0.003
DISTAL = 0.55
TOE_LENGTH = 0.065  # the toe joints sit this far behind the tip of the furthest toe


def _args():
    argv = sys.argv[sys.argv.index("--") + 1:]
    a = {"out": None, "blend": None, "body": None, "preview": None}
    i = 0
    while i < len(argv):
        k = argv[i][2:]
        if k not in a:
            sys.exit("add_toes: unknown argument %s" % argv[i])
        a[k] = argv[i + 1]
        i += 2
    if not a["out"]:
        sys.exit(__doc__)
    return a


def smooth(x, lo, hi):
    t = np.clip((x - lo) / (hi - lo), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def row_bounds(t, d, base_d, big):
    """Cuts across the toe row at the toe base: the row's width there (1st to 99th percentile of the skin within 4 mm of
    base_d) split by WIDTHS from the big toe's side. Generated feet often have the toes fused into one blob, so the
    clefts can't be found from the surface; anatomical widths can."""
    band = np.abs(d - base_d) < 0.004
    lo, hi = np.percentile(t[band], [1, 99])
    start = hi if big > 0 else lo
    return list(start - big * np.cumsum(WIDTHS)[:-1] * (hi - lo))


def add_side(arm, body, side, W, P):
    ball = arm.data.bones["ball_" + side]
    mw = arm.matrix_world
    head, tail = mw @ ball.head_local, mw @ ball.tail_local
    f = Vector((tail.x - head.x, tail.y - head.y, 0.0)).normalized()
    lat = f.cross(Vector((0, 0, 1))).normalized()
    groups = {g.name: g.index for g in body.vertex_groups}
    gi = [groups[n] for n in ("ball_" + side, "foot_" + side) if n in groups]
    rel = P - np.array(head)
    d = rel @ np.array(f)
    t = rel @ np.array(lat)
    owned = W[:, gi].sum(1) > 0.5
    sel = owned & (d > 0.0) & (P[:, 2] < head.z + 0.05)
    if sel.sum() < 50:
        sys.exit("add_toes: no toe skin in front of ball_%s" % side)
    big = 1 if (lat.x > 0) != (head.x > 0) else -1  # the big toe is on the inner side, nearer x = 0
    base_d = float(np.percentile(d[sel], 99.5)) - TOE_LENGTH
    cuts = row_bounds(t[sel], d[sel], base_d, big)
    # bounds per toe as (towards the big toe, away from it) in t
    edge = [big * np.inf] + cuts + [-big * np.inf]
    bounds = [(max(edge[k], edge[k + 1]), min(edge[k], edge[k + 1])) for k in range(5)]

    bpy.ops.object.mode_set(mode="OBJECT")
    nb.activate(arm)
    bpy.ops.object.mode_set(mode="EDIT")
    eb = arm.data.edit_bones
    inv = mw.inverted()
    made = []
    for k, name in enumerate(TOES):
        hi, lo = bounds[k]
        m = sel & (t <= hi) & (t > lo)
        if m.sum() < 10:
            sys.exit("add_toes: toe %s_%s has %d vertices" % (name, side, m.sum()))
        tc = float(np.median(t[m]))
        reach = float(np.percentile(d[m], 98))
        near = m & (np.abs(d - base_d) < 0.006)
        zc = float(np.median(P[near if near.sum() >= 5 else m, 2]))
        ztip = float(np.median(P[m & (d > base_d + 0.8 * (reach - base_d)), 2]))
        base = head + lat * tc + f * base_d
        base.z = zc
        tip = head + lat * tc + f * reach
        tip.z = ztip
        mid = base.lerp(tip, DISTAL)
        parent = eb["ball_" + side]
        for seg, (h, tl) in enumerate(((base, mid), (mid, tip)), 1):
            b = eb.new("%s_%02d_%s" % (name, seg, side))
            b.head, b.tail = inv @ h, inv @ tl
            b.align_roll(inv.to_3x3() @ Vector((0, 0, 1)))
            b.parent = parent
            b.use_connect = seg == 2
            b.use_deform = True
            parent = b
        made.append((name, k, hi, lo, base_d + DISTAL * (reach - base_d)))
    bpy.ops.object.mode_set(mode="OBJECT")

    # weights: the toe share takes over the vertex in front of the ball, split between neighbours over SIDE_BLEND
    take = np.where(sel, smooth(d, base_d - BLEND_IN / 2, base_d + BLEND_IN / 2), 0.0)
    shares = []
    for name, k, hi, lo, dmid in made:
        s = np.ones_like(t)
        if np.isfinite(hi):
            s *= smooth(hi - t, -SIDE_BLEND / 2, SIDE_BLEND / 2)
        if np.isfinite(lo):
            s *= smooth(t - lo, -SIDE_BLEND / 2, SIDE_BLEND / 2)
        shares.append(s)
    shares = np.array(shares)
    shares /= np.maximum(shares.sum(0), 1e-9)
    moved = 0
    for v in np.nonzero(take > 1e-3)[0]:
        vert = body.data.vertices[int(v)]
        for g in vert.groups:
            g.weight *= 1.0 - take[v]
        for (name, k, hi, lo, dmid), s in zip(made, shares):
            w = take[v] * s[v]
            if w < 1e-3:
                continue
            w2 = w * smooth(d[v], dmid - 0.006, dmid + 0.006)
            for seg, ww in ((1, w - w2), (2, w2)):
                if ww > 1e-3:
                    gname = "%s_%02d_%s" % (name, seg, side)
                    vg = body.vertex_groups.get(gname) or body.vertex_groups.new(name=gname)
                    vg.add([int(v)], float(ww), "ADD")
        moved += 1
    return {"toe_base_mm": round(base_d * 1000), "vertices": moved}


def preview(arm, body, outdir):
    os.makedirs(outdir, exist_ok=True)
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_WORKBENCH"
    sc.display.shading.light = "STUDIO"
    sc.render.resolution_x, sc.render.resolution_y = 900, 450
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    cam.data.type, cam.data.ortho_scale = "ORTHO", 0.6
    sc.collection.objects.link(cam)
    sc.camera = cam
    feet = [arm.matrix_world @ arm.data.bones["ball_" + s].head_local for s in ("l", "r")]
    c = (feet[0] + feet[1]) / 2
    arm.animation_data.action = None
    for tag, ang in (("rest", 0.0), ("curl", 40.0)):
        for pb in arm.pose.bones:
            if any(pb.name.startswith(n) for n in TOES):
                pb.rotation_mode = "XYZ"
                pb.rotation_euler = (math.radians(ang), 0, 0)
        bpy.context.view_layer.update()
        for view, loc, rot in (("sole", (c.x, c.y - 0.05, -2.0), (math.radians(180), 0.0, 0.0)),
                               ("side", (c.x - 2, c.y, 0.08), (math.radians(85), 0, math.radians(-90)))):
            cam.location, cam.rotation_euler = loc, rot
            sc.render.filepath = os.path.join(outdir, "toes_%s_%s.png" % (tag, view))
            bpy.ops.render.render(write_still=True)
    rc.reset_pose(arm)
    bpy.data.objects.remove(cam, do_unlink=True)


def main():
    a = _args()
    arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
    body = bpy.data.objects[a["body"]] if a["body"] else max((o for o in bpy.data.objects if o.type == "MESH"),
                                                             key=lambda o: len(o.data.vertices))
    if "bigtoe_01_l" in arm.data.bones:
        sys.exit("add_toes: the rig already has toes")
    rc.reset_pose(arm)
    bpy.context.scene.frame_set(0)
    mw = body.matrix_world
    P = np.array([mw @ v.co for v in body.data.vertices])
    W = np.zeros((len(P), len(body.vertex_groups)))
    for v in body.data.vertices:
        for g in v.groups:
            W[v.index, g.group] = g.weight
    report = {s: add_side(arm, body, s, W, P) for s in ("l", "r")}
    rc.reset_pose(arm)
    bpy.context.scene.frame_set(0)
    out = os.path.abspath(a["out"])
    glb = nb.export_rig(out, {"export_morph_normal": False, "export_force_sampling": False})
    rc.write_rigexpect(arm, body, os.path.splitext(out)[0] + ".rigexpect.json")
    if a["blend"]:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(a["blend"]))
    if a["preview"]:
        preview(arm, body, os.path.abspath(a["preview"]))
    print("add_toes: %d joints, %s, %d bytes -> %s" % (len(arm.data.bones), report, os.path.getsize(glb), glb))


if __name__ == "__main__":
    main()
