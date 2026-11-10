"""Builds a thin rectangular eyeglass frame as a head_prop.py glasses source: an FBX with the frame (two rims, a bridge,
two end pieces), the lenses (material "M_Glasses_Lens", which head_prop uses for the frame axes and drops at export) and
two temple objects named Temple_L / Temple_R, plus solid _BC / _N / _ORM maps for it.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/glasses_build.py -- OUT_DIR [key=value ...]

Options (millimetres unless noted): lens_w (54), lens_h (31), corner (5), corner_nose (11, the
lower corner beside the nose, cut away so the rim clears it), corner_nose_top (the upper one, default corner), gap (18, between the lens openings), rim (2.8,
the rim's face-on width outside the lens), rim_in (0.6, how far the rim's groove laps over the lens), rim_d (3.5, front
to back), wrap (5 deg, each lens turned back at its outer edge), bridge_h (2.8), bridge_z (5, the bridge's underside
above the lens centres), arch (1.5), hinge_z (11, the temples' centre above the lens centres), end_d (8, the end
pieces' depth behind the rims), flare (4, how far they reach out sideways), temple_w (1.6), temple_h (3.2), temple_tip_h (2.4), run (98, the temples' straight
run), bend_r (18), bend (38 deg), tip (26), colour (sRGB "r,g,b", default 0.02,0.02,0.022), rough (0.45), name
(GlassesThin), profile ("box": the rims' cross-section a rectangle; "oct": its corners cut by "chamfer" (0.3) of the
smaller side, which reads round once smooth-shaded).

pads (0; 1 adds nose pads): an oval pad each side, Pad_L / Pad_R (pad_w 7 across, pad_h 10 tall, pad_t 1.2 thick),
centred "pad_x" (gap / 2 - 1) out from the middle, "pad_z" (-4) above the lens centres and "pad_back" (3) behind the
rims' back face, its contact face turned "pad_face" (35 deg) from straight in towards the back and "pad_tip" (0 deg)
down, and its top leaned
"pad_lean" (15 deg) in towards the middle; and a wire pad arm each side, PadArm_L / PadArm_R ("arm_t" 0.8 mm square),
from the rim's back on the nose side "arm_z" (1) above the lens centres, back and round to the pad's back. head_prop
seats the pads on the nose (it finds them by name). The maps are then three solid bands: the frame ("colour", "rough",
metal 0), the pads ("pad_colour" 0.62,0.62,0.6, "pad_rough" 0.35, metal 0) and the pad arms ("arm_colour"
0.55,0.55,0.56, "arm_rough" 0.35, metal 1); each object's planar UVs sit in the middle of its band.

Writes OUT_DIR/SM_<name>.fbx and OUT_DIR/T_<name>_BC.png, _N.png (flat) and _ORM.png (R AO 1, G rough, B metal). Frame
axes in the file: x across (the wearer's left +), z up, y back (the temples run to +y)."""
import math
import os
import sys

import bmesh
import bpy
import numpy as np

MM = 0.001


def log(*a):
    print("glasses_build:", *a, flush=True)


argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if not argv:
    sys.exit("usage: glasses_build.py -- OUT_DIR [key=value ...]")
out = argv[0]
opt = dict(a.split("=", 1) for a in argv[1:])


def f(k, d):
    return float(opt.get(k, d))


lw, lh, rc, gap = f("lens_w", 54), f("lens_h", 31), f("corner", 5), f("gap", 18)
rc_nose = f("corner_nose", 11)
rc_nose_top = f("corner_nose_top", rc)
rim, rim_in, rim_d, wrap = f("rim", 2.8), f("rim_in", 0.6), f("rim_d", 3.5), math.radians(f("wrap", 5))
bh, bz, arch, hz, end_d = f("bridge_h", 2.8), f("bridge_z", 5), f("arch", 1.5), f("hinge_z", 11), f("end_d", 8)
flare = f("flare", 4)
tw, th, tth, run = f("temple_w", 1.6), f("temple_h", 3.2), f("temple_tip_h", 2.4), f("run", 98)
bend_r, bend, tip = f("bend_r", 18), math.radians(f("bend", 38)), f("tip", 26)
name = opt.get("name", "GlassesThin")
profile, chamfer = opt.get("profile", "box"), f("chamfer", 0.3)
if profile not in ("box", "oct"):
    sys.exit("glasses_build: profile is box or oct, not %s" % profile)
pads = int(f("pads", 0))
pad_w, pad_h, pad_t = f("pad_w", 7), f("pad_h", 10), f("pad_t", 1.2)
pad_x, pad_z, pad_back = f("pad_x", 0.5 * gap - 1.0), f("pad_z", -4), f("pad_back", 3)
pad_face, pad_lean, pad_tip = math.radians(f("pad_face", 35)), math.radians(f("pad_lean", 15)), math.radians(f("pad_tip", 0))
arm_t, arm_z = f("arm_t", 0.8), f("arm_z", 1)
os.makedirs(out, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)


def rounded_rect(a, b, r, n=128):
    """n points CCW (seen from the front, -y) on a rectangle of half sizes a, b with rounded corners, evenly spaced by
    arc length, with outward unit normals. r is one radius or four (top right, top left, bottom left, bottom right).
    Returns (pts (n, 2) in x, z), normals (n, 2))."""
    tr, tl, bl, br = (r,) * 4 if np.isscalar(r) else r
    segs = [("line", (a, -(b - br)), (0, 1), 2 * b - br - tr, (1, 0)), ("arc", (a - tr, b - tr), 0.0, tr),
            ("line", (a - tr, b), (-1, 0), 2 * a - tr - tl, (0, 1)), ("arc", (-(a - tl), b - tl), 0.5 * math.pi, tl),
            ("line", (-a, b - tl), (0, -1), 2 * b - tl - bl, (-1, 0)), ("arc", (-(a - bl), -(b - bl)), math.pi, bl),
            ("line", (-(a - bl), -b), (1, 0), 2 * a - bl - br, (0, -1)),
            ("arc", (a - br, -(b - br)), 1.5 * math.pi, br)]
    per = sum(sg[3] if sg[0] == "line" else 0.5 * math.pi * sg[3] for sg in segs)
    pts, nrm = [], []
    for k in range(n):
        s = per * k / n
        for sg in segs:
            if sg[0] == "line":
                L = sg[3]
                if s <= L:
                    pts.append((sg[1][0] + sg[2][0] * s, sg[1][1] + sg[2][1] * s))
                    nrm.append(sg[4])
                    break
                s -= L
            else:
                rr = sg[3]
                arc = 0.5 * math.pi * rr
                if s <= arc:
                    t = sg[2] + s / max(rr, 1e-9)
                    pts.append((sg[1][0] + rr * math.cos(t), sg[1][1] + rr * math.sin(t)))
                    nrm.append((math.cos(t), math.sin(t)))
                    break
                s -= arc
        else:
            pts.append(pts[0])
            nrm.append(nrm[0])
    return np.array(pts), np.array(nrm)


def turn(P, cx, ang):
    """Turns points (x, y, z) about the vertical line x = cx, y = 0 by ang (the outer edge goes back, +y)."""
    Q = P.copy()
    x, y = P[:, 0] - cx, P[:, 1]
    c, s = math.cos(ang), math.sin(ang)
    Q[:, 0] = cx + c * x - s * y
    Q[:, 1] = s * x + c * y
    return Q


def new_obj(nm, bm, mats):
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    me = bpy.data.meshes.new(nm)
    bm.to_mesh(me)
    bm.free()
    for m in mats:
        me.materials.append(m)
    o = bpy.data.objects.new(nm, me)
    bpy.context.collection.objects.link(o)
    return o


def rim_section(s0, s1, d):
    """The rim's cross-section as (offset out along the lens contour's normal, depth back) points going round: front
    face, outer wall, back face, inner wall (the lens groove); "oct" cuts the four corners."""
    if profile == "box":
        return [(s0, 0.0), (s1, 0.0), (s1, d), (s0, d)]
    c = chamfer * min(s1 - s0, d)
    return [(s0 + c, 0.0), (s1 - c, 0.0), (s1, c), (s1, d - c), (s1 - c, d), (s0 + c, d), (s0, d - c), (s0, c)]


def ring(bm, loops):
    """A closed rim from contour loops (m loops of n points (n, 3), going round the cross-section); quads all round."""
    vs = [[bm.verts.new(tuple(p)) for p in lp] for lp in loops]
    m, n = len(vs), len(vs[0])
    for i in range(m):
        a, b = vs[i], vs[(i + 1) % m]
        for k in range(n):
            j = (k + 1) % n
            bm.faces.new((a[k], b[k], b[j], a[j]))


def box_sweep(bm, path, half_w, half_h, side, up_dir=None, cap=True):
    """Sweeps a rectangle (half_w along 'side', half_h along the path's up) along a polyline path (m, 3); half sizes
    may be per point. Returns nothing; caps both ends."""
    path = np.asarray(path, float)
    m = len(path)
    hw = np.broadcast_to(np.asarray(half_w, float), (m,))
    hh = np.broadcast_to(np.asarray(half_h, float), (m,))
    rings = []
    for i in range(m):
        t = path[min(i + 1, m - 1)] - path[max(i - 1, 0)]
        t = t / np.linalg.norm(t)
        sd = np.asarray(side, float)
        sd = sd - (sd @ t) * t
        sd = sd / np.linalg.norm(sd)
        up = np.cross(t, sd)
        if up_dir is not None and up @ np.asarray(up_dir) < 0:
            up = -up
        c = path[i]
        rings.append([bm.verts.new(tuple(c + a * hw[i] * sd + b * hh[i] * up))
                      for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1))])
    for i in range(m - 1):
        for k in range(4):
            j = (k + 1) % 4
            bm.faces.new((rings[i][k], rings[i][j], rings[i + 1][j], rings[i + 1][k]))
    if cap:
        bm.faces.new(tuple(reversed(rings[0])))
        bm.faces.new(tuple(rings[-1]))


def mat(nm):
    m = bpy.data.materials.new(nm)
    m.use_nodes = True
    return m


m_frame, m_lens = mat("M_Glasses_Frame"), mat("M_Glasses_Lens")

# lenses and rims
a, b = 0.5 * lw * MM, 0.5 * lh * MM
cx = 0.5 * gap * MM + a
base, nrm = rounded_rect(a, b, (rc * MM, rc_nose_top * MM, rc_nose * MM, rc * MM))  # -x: the nose side of both
bm_f = bmesh.new()
bm_l = bmesh.new()
ends = {}
nose = {}  # per side: the rim's inner edge (the lens contour) on the nose side, at its back face
for sg in (1.0, -1.0):
    outer2 = base + rim * MM * nrm
    lens2 = base.copy()

    def lift(c2):
        P = np.stack([sg * (cx + c2[:, 0]), np.zeros(len(c2)), c2[:, 1]], 1)
        Q = turn(P, sg * cx, sg * wrap)  # the outer edge goes back
        return Q[::-1] if sg < 0 else Q  # mirrored: keep the contours counter-clockwise seen from the front

    outer, lens = lift(outer2), lift(lens2)
    loops = []
    for s, dpt in rim_section(-rim_in, rim, rim_d):
        lp = lift(base + s * MM * nrm)
        lp[:, 1] += dpt * MM
        loops.append(lp)
    ring(bm_f, loops)
    lv = [bm_l.verts.new((p[0], 0.5 * rim_d * MM + p[1], p[2])) for p in lens]
    bm_l.faces.new(lv)
    nose_side = sg * lens[:, 0] < cx  # the half of the contour towards the middle
    nose[sg] = (lens[nose_side], rim_d * MM)
    # the end piece: from the outer rim at the hinge height, back by end_d
    near = np.nonzero(np.abs(outer[:, 2] - hz * MM) < 1.5 * MM)[0]
    e = outer[near[np.argmax(sg * outer[near, 0])]]  # the outer side, not the nose side
    ends[sg] = e
    p0 = np.array([e[0] - sg * 0.8 * MM, e[1] + 0.2 * MM, hz * MM])
    p1 = np.array([e[0] + sg * flare * MM, e[1] + (rim_d + end_d) * MM, hz * MM])  # out, then back
    box_sweep(bm_f, [p0, 0.5 * (p0 + p1), p1], 0.5 * rim * MM, 0.5 * (th + 1.0) * MM, (1, 0, 0), (0, 0, 1))
# the bridge: a shallow arch between the rims' inner edges at bridge_z
xs = np.linspace(-(gap * 0.5 - 1.0) * MM, (gap * 0.5 - 1.0) * MM, 9)  # ends inside the rims
zs = (bz + 0.5 * bh) * MM - arch * MM * (xs / xs.max()) ** 2
path = np.stack([xs, np.full(9, (0.5 * rim_d * MM - math.sin(wrap) * a)), zs], 1)  # the rims' nose side
box_sweep(bm_f, path, 0.5 * bh * MM, 0.5 * (rim_d - 0.4) * MM, (0, 0, 1), None)
frame = new_obj("Frame", bm_f, [m_frame])
lenses = new_obj("Lenses", bm_l, [m_lens])


def pad_disc(bm, c, n, u, v, w, h, t, k=20):
    """An oval pad: contact face (towards +n) a little smaller than the middle, the back a little smaller too, so the
    edge reads rounded; fan caps round a centre vertex."""
    rings = []
    for off, sc in ((-0.5 * t, 0.88), (0.0, 1.0), (0.5 * t, 0.82)):
        rings.append([bm.verts.new(tuple(c + n * off + sc * (0.5 * w * math.cos(a) * v + 0.5 * h * math.sin(a) * u)))
                      for a in np.linspace(0, 2 * math.pi, k, endpoint=False)])
    for i in range(2):
        for j in range(k):
            jj = (j + 1) % k
            bm.faces.new((rings[i][j], rings[i][jj], rings[i + 1][jj], rings[i + 1][j]))
    for rr, off in ((rings[0], -0.5 * t), (rings[2], 0.5 * t)):
        cv = bm.verts.new(tuple(c + n * off))
        for j in range(k):
            bm.faces.new((cv, rr[j], rr[(j + 1) % k]))


pad_objs = []
if pads:
    for sg, sd in ((1.0, "L"), (-1.0, "R")):
        lens_n, back = nose[sg]
        # the pad's centre: pad_x out, pad_z up, pad_back behind the rim's back face beside it
        j = np.argmin(np.abs(lens_n[:, 2] - pad_z * MM) + 0.3 * np.abs(sg * lens_n[:, 0] - pad_x * MM))
        y_back = lens_n[j, 1] + back
        c = np.array([sg * pad_x * MM, y_back + pad_back * MM, pad_z * MM])
        n = np.array([-sg * math.cos(pad_face) * math.cos(pad_tip), math.sin(pad_face) * math.cos(pad_tip),
                      -math.sin(pad_tip)])  # the contact face: in, back and pad_tip down, onto the nose
        mid = np.array([-sg, 0.0, 0.0])
        mid = mid - n * (n @ mid)
        mid /= np.linalg.norm(mid)
        u = math.cos(pad_lean) * np.array([0.0, 0.0, 1.0]) + math.sin(pad_lean) * mid  # its top leans in
        u -= n * (n @ u)
        u /= np.linalg.norm(u)
        v = np.cross(n, u)
        bm_p = bmesh.new()
        pad_disc(bm_p, c, n, u, v, pad_w * MM, pad_h * MM, pad_t * MM)
        pad_objs.append(new_obj("Pad_" + sd, bm_p, [m_frame]))
        # the pad arm: from inside the rim's back on the nose side at arm_z, back, then round to the pad's back
        ja = np.argmin(np.abs(lens_n[:, 2] - arm_z * MM))
        a0 = lens_n[ja] + np.array([-sg * 0.5 * rim * MM, back - 0.6 * arm_t * MM, 0.0])  # inside the rim
        a2 = c - n * 0.35 * pad_t * MM
        a1 = a0 + np.array([0.0, 0.6 * (a2[1] - a0[1]) + 0.8 * MM, 0.0])
        a1[2] = 0.5 * (a0[2] + a2[2]) + 1.0 * MM
        pts = [(1 - s) ** 2 * a0 + 2 * (1 - s) * s * a1 + s * s * a2 for s in np.linspace(0, 1, 9)]
        bm_a = bmesh.new()
        box_sweep(bm_a, np.array(pts), 0.5 * arm_t * MM, 0.5 * arm_t * MM, (1, 0, 0), None)
        pad_objs.append(new_obj("PadArm_" + sd, bm_a, [m_frame]))

# temples: a straight run, a bend down, a tapering tip
for sg, nm in ((1.0, "Temple_L"), (-1.0, "Temple_R")):
    e = ends[sg]
    x0 = e[0] + sg * (flare - 0.3) * MM
    y0 = e[1] + (rim_d + end_d - 0.5) * MM
    pts = [np.array([x0, y0 + s * MM, hz * MM]) for s in np.linspace(0, run, 15)]
    c0 = pts[-1] + np.array([0, 0, -bend_r * MM])
    for t in np.linspace(0, bend, 7)[1:]:
        pts.append(c0 + bend_r * MM * np.array([0, math.sin(t), math.cos(t)]))
    d = np.array([0, math.cos(bend), -math.sin(bend)])
    for _ in range(7):
        pts.append(pts[-1] + d * (tip / 7) * MM)
    m = len(pts)
    hh = np.concatenate([np.full(15, 0.5 * th), np.linspace(0.5 * th, 0.5 * tth, m - 15)]) * MM
    hw = np.concatenate([np.full(15 + 6, 0.5 * tw), np.linspace(0.5 * tw, 0.5 * tw * 1.15, m - 21)]) * MM
    bm_t = bmesh.new()
    box_sweep(bm_t, np.array(pts), hw, hh, (1, 0, 0), (0, 0, 1))
    new_obj(nm, bm_t, [m_frame])

# UVs: one planar layer each (the maps are solid); smooth shading off (crisp edges). With pads the maps are three
# bands (frame, pads, pad arms) and each object's UVs sit in the middle of its own band.
bands = 3 if pads else 1


def band_of(nm):
    return 0 if not pads or not nm.startswith("Pad") else (2 if nm.startswith("PadArm") else 1)


for o in bpy.data.objects:
    me = o.data
    uv = me.uv_layers.new(name="UVMap")
    co = np.array([v.co for v in me.vertices])
    u0, sc = (band_of(o.name) + 0.5) / bands, 1.0 / bands
    for li, loop in enumerate(me.loops):
        p = co[loop.vertex_index]
        uv.data[li].uv = (u0 + sc * p[0] * 3.0, 0.5 + (p[2] + p[1]) * 3.0)

# maps
col = np.array([float(x) for x in opt.get("colour", "0.02,0.02,0.022").split(",")])
rough = f("rough", 0.45)


def rgb3(k, d):
    return np.array([float(x) for x in opt.get(k, d).split(",")])


def save(nm, rgbs, srgb):
    """A 64 px map of len(rgbs) solid vertical bands, left to right."""
    im = bpy.data.images.new(nm, 64, 64, alpha=False)
    im.colorspace_settings.name = "sRGB" if srgb else "Non-Color"
    px = np.ones((64, 64, 4), np.float32)
    edges = np.linspace(0, 64, len(rgbs) + 1).round().astype(int)
    for k, rgb in enumerate(rgbs):
        px[:, edges[k]:edges[k + 1], :3] = rgb
    im.pixels.foreach_set(px.ravel())
    im.filepath_raw = os.path.join(out, nm + ".png")
    im.file_format = "PNG"
    im.save()


bc_bands, orm_bands = [col], [np.array([1.0, rough, 0.0])]
if pads:
    bc_bands += [rgb3("pad_colour", "0.62,0.62,0.6"), rgb3("arm_colour", "0.55,0.55,0.56")]
    orm_bands += [np.array([1.0, f("pad_rough", 0.35), 0.0]), np.array([1.0, f("arm_rough", 0.35), 1.0])]
save("T_%s_BC" % name, bc_bands, True)
save("T_%s_N" % name, [np.array([0.5, 0.5, 1.0])], False)
save("T_%s_ORM" % name, orm_bands, False)

for o in bpy.data.objects:
    tris = sum(len(p.vertices) - 2 for p in o.data.polygons)
    co = np.array([v.co for v in o.data.vertices])
    log("%s: %d verts, %d tris, x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f mm" % (o.name, len(co), tris, *(
        v for k in range(3) for v in (co[:, k].min() / MM, co[:, k].max() / MM))))
fbx = os.path.join(out, "SM_%s.fbx" % name)
bpy.ops.export_scene.fbx(filepath=fbx, use_selection=False, object_types={"MESH"}, mesh_smooth_type="FACE",
                         add_leaf_bones=False, bake_space_transform=False)
log("WROTE", fbx)
