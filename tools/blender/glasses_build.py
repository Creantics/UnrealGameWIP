"""Builds a thin rectangular eyeglass frame as a head_prop.py glasses source: an FBX with the frame (two rims, a bridge,
two end pieces), the lenses (material "M_Glasses_Lens", which head_prop uses for the frame axes and drops at export) and
two temple objects named Temple_L / Temple_R, plus solid _BC / _N / _ORM maps for it.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/glasses_build.py -- OUT_DIR [key=value ...]

Options (millimetres unless noted): lens_w (54), lens_h (31), corner (5), corner_nose (11, the
lower corner beside the nose, cut away so the rim clears it), gap (18, between the lens openings), rim (2.8,
the rim's face-on width outside the lens), rim_in (0.6, how far the rim's groove laps over the lens), rim_d (3.5, front
to back), wrap (5 deg, each lens turned back at its outer edge), bridge_h (2.8), bridge_z (5, the bridge's underside
above the lens centres), arch (1.5), hinge_z (11, the temples' centre above the lens centres), end_d (8, the end
pieces' depth behind the rims), flare (4, how far they reach out sideways), temple_w (1.6), temple_h (3.2), temple_tip_h (2.4), run (98, the temples' straight
run), bend_r (18), bend (38 deg), tip (26), colour (sRGB "r,g,b", default 0.02,0.02,0.022), rough (0.45), name
(GlassesThin). Writes OUT_DIR/SM_<name>.fbx and OUT_DIR/T_<name>_BC.png, _N.png (flat) and _ORM.png (R AO 1, G rough,
B metal 0). Frame axes in the file: x across (the wearer's left +), z up, y back (the temples run to +y)."""
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
rim, rim_in, rim_d, wrap = f("rim", 2.8), f("rim_in", 0.6), f("rim_d", 3.5), math.radians(f("wrap", 5))
bh, bz, arch, hz, end_d = f("bridge_h", 2.8), f("bridge_z", 5), f("arch", 1.5), f("hinge_z", 11), f("end_d", 8)
flare = f("flare", 4)
tw, th, tth, run = f("temple_w", 1.6), f("temple_h", 3.2), f("temple_tip_h", 2.4), f("run", 98)
bend_r, bend, tip = f("bend_r", 18), math.radians(f("bend", 38)), f("tip", 26)
name = opt.get("name", "GlassesThin")
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


def ring(bm, inner, outer, y0, y1):
    """A closed rim: inner and outer contours (n, 3) at y0 (front); the same at y1 (back); quads all round."""
    n = len(inner)
    vi0 = [bm.verts.new((p[0], y0 + p[1], p[2])) for p in inner]
    vo0 = [bm.verts.new((p[0], y0 + p[1], p[2])) for p in outer]
    vi1 = [bm.verts.new((p[0], y1 + p[1], p[2])) for p in inner]
    vo1 = [bm.verts.new((p[0], y1 + p[1], p[2])) for p in outer]
    for k in range(n):
        j = (k + 1) % n
        bm.faces.new((vi0[k], vo0[k], vo0[j], vi0[j]))  # front
        bm.faces.new((vo1[k], vi1[k], vi1[j], vo1[j]))  # back
        bm.faces.new((vo0[k], vo1[k], vo1[j], vo0[j]))  # outer wall
        bm.faces.new((vi1[k], vi0[k], vi0[j], vi1[j]))  # inner wall (the lens groove)


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
base, nrm = rounded_rect(a, b, (rc * MM, rc * MM, rc_nose * MM, rc * MM))  # -x is the nose side of both lenses
bm_f = bmesh.new()
bm_l = bmesh.new()
ends = {}
for sg in (1.0, -1.0):
    inner2 = base - rim_in * MM * nrm
    outer2 = base + rim * MM * nrm
    lens2 = base.copy()

    def lift(c2):
        P = np.stack([sg * (cx + c2[:, 0]), np.zeros(len(c2)), c2[:, 1]], 1)
        return turn(P, sg * cx, sg * wrap)  # the outer edge goes back

    inner, outer, lens = lift(inner2), lift(outer2), lift(lens2)
    if sg < 0:  # mirrored: keep the contours counter-clockwise seen from the front
        inner, outer, lens = inner[::-1], outer[::-1], lens[::-1]
    ring(bm_f, inner, outer, 0.0, rim_d * MM)
    lv = [bm_l.verts.new((p[0], 0.5 * rim_d * MM + p[1], p[2])) for p in lens]
    bm_l.faces.new(lv)
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

# UVs: one planar layer each (the maps are solid); smooth shading off (crisp edges)
for o in bpy.data.objects:
    me = o.data
    uv = me.uv_layers.new(name="UVMap")
    co = np.array([v.co for v in me.vertices])
    for li, loop in enumerate(me.loops):
        p = co[loop.vertex_index]
        uv.data[li].uv = (0.5 + p[0] * 3.0, 0.5 + (p[2] + p[1]) * 3.0)

# maps
col = np.array([float(x) for x in opt.get("colour", "0.02,0.02,0.022").split(",")])
rough = f("rough", 0.45)


def save(nm, rgb, srgb):
    im = bpy.data.images.new(nm, 64, 64, alpha=False)
    im.colorspace_settings.name = "sRGB" if srgb else "Non-Color"
    px = np.ones((64, 64, 4), np.float32)
    px[..., :3] = rgb
    im.pixels.foreach_set(px.ravel())
    im.filepath_raw = os.path.join(out, nm + ".png")
    im.file_format = "PNG"
    im.save()


save("T_%s_BC" % name, col, True)
save("T_%s_N" % name, np.array([0.5, 0.5, 1.0]), False)
save("T_%s_ORM" % name, np.array([1.0, rough, 0.0]), False)

for o in bpy.data.objects:
    tris = sum(len(p.vertices) - 2 for p in o.data.polygons)
    co = np.array([v.co for v in o.data.vertices])
    log("%s: %d verts, %d tris, x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f mm" % (o.name, len(co), tris, *(
        v for k in range(3) for v in (co[:, k].min() / MM, co[:, k].max() / MM))))
fbx = os.path.join(out, "SM_%s.fbx" % name)
bpy.ops.export_scene.fbx(filepath=fbx, use_selection=False, object_types={"MESH"}, mesh_smooth_type="FACE",
                         add_leaf_bones=False, bake_space_transform=False)
log("WROTE", fbx)
