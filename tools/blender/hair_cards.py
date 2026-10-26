"""Grow a short haircut (or a short beard) on a rigged lead as hair cards: guide curves over the skin, then textured cards
along them and a cap, skinned to the head bone (or the body's weights) and exported as a glb outfit piece
(FTImportCharacter -Role=outfit -Face=0). A beard is the same groom in a band: "hairline" is its lower edge (the
neckline), "topline" its upper edge (the cheek line) and "holes" keep the lips clear.

    blender -b --factory-startup --python-exit-code 1 lead.blend --python tools/blender/hair_cards.py -- spec.json

The lead's body mesh (spec "body", default "body") gives the scalp; its armature is exported with the hair. Positions
use a head frame: "center" [x, y, z] in metres, the face toward -Y. A point's phi is its angle round the head from the
front (degrees, +90 the +X side, +-180 the back) and theta its height above the centre's horizontal plane (degrees).

spec.json keys (lengths in cm unless said):
  "center": [x, y, z]       head frame origin (a sphere fitted to the cranium works)
  "hairline": [[phi, theta], ..]
                            the edge of the hair: a root grows only above this theta at its phi (periodic,
                            piecewise linear; a dip makes a sideburn). "mirror": true adds every point at -phi
  "topline": [[phi, theta], ..]
                            optional upper edge: a root grows only below this theta too (mirrored like the hairline)
  "holes": [[phi, theta, half_phi, half_theta], ..]
                            optional ellipses in (phi, theta) where nothing grows (the lips); mirrored too
  "fields": {"length": [[phi, theta, cm], ..], "volume": [[phi, theta, cm], ..]}
                            strand length and how far the hair stands off the scalp, blended between the points with
                            a Gaussian of angular width "sigma" (degrees, default 22); mirrored with "mirror" too
  "flow": {"split": [[phi, from, to], ..], "sweep": x, "part": [phi, width], "sweep_small": x, "down_back": y, "out": z}
                            above theta "to" the hair runs back over the head (+Y) leaning sideways by "sweep" (+X);
                            below "from" it runs down, leaning back by "down_back" (negative: forward) and away from
                            the midline by "out" (fading to 0 at the sides); blended in between. Roots on the
                            -X side of the "part" phi lean by -"sweep_small" instead (a soft side parting)
  "hairline_jitter": deg    random lift of each root's hairline, so the edge isn't a line (default 1.5)
  "outer_only": bool        grow only on skin seen from outside along rays toward "center" (no roots or cap inside the
                            mouth, the nostrils or the ear's folds)
  "overshoot": deg          how far past an edge a strand may reach before it's cut (default 1.5)
  "layers": [{"spacing": cm, "height": f, "width": cm, "rows": [..], "min_volume": cm, "twist": deg, "count": n}]
                            card layers, innermost first: roots at least "spacing" apart (closer where the hair is
                            short, down to "short_spacing" times it, default 0.55), standing at "height" times the
                            volume, cards "width" wide using these atlas rows.
                            "count" instead of "spacing" takes that many random roots from the layer above (flyaways)
  "rise": f, "climb": f     a strand reaches its height by the fraction "rise" of its length (default 0.35),
                            climbing at most "climb" cm per cm (default 1.2)
  "bulge": f                each card's middle row stands out of its plane by this fraction of its width (0.12)
  "shell_normals": f        how far each card's normals lean to the scalp's (default 0.8; 0 = the card's own)
  "clear": cm               every card point but the root row is pushed at least this far out of the scalp (0.15)
  "tuck": cm                roots sink this far under the scalp (default 0.2)
  "colors": {"root": rgb, "mid": rgb, "tip": rgb, "jitter": f}   sRGB 0..255 strand colours along the strand
  "atlas": {"size": px, "rows": n, "strands": [n per row ..], "clump": [f per row ..], "thick": [lo, hi] px,
            "wave": [lo, hi] px, "waves": [lo, hi]}
                            the card texture: n rows, strands along U (root at U 0), so the mesh tangent follows them;
                            each strand waves by an amplitude in "wave" (default [0.5, 3]), "waves" times along it
                            (default [1, 3]): coarse, wavy beard hair wants more of both
  "cap": {"offset": cm, "fade": cm, "tile": cm, "size": px, "subdiv": n, "color": rgb, "fill": [u0, u1], "strands": n}
                            a shell over the scalp under the cards; its U runs from 0 at the hairline curve to 1 "fade"
                            in, its V round the head repeating about every "tile". Its texture is solid "color" from U
                            "fill"[1] (fading in from "fill"[0]) with "strands" short strands over it
  "bone": name              the bone every hair vertex is weighted to (default "head")
  "skin_from_body": bool    instead, each vertex takes the body's bone weights at its nearest scalp point, so the nape
                            follows the neck bones the skin under it follows
  "bind": "nearest"|"anchor"
                            which skin point a card vertex takes its weights (and morphs) from: the nearest to the
                            vertex (default), or the one its guide point was grown over ("anchor"), so a card hanging
                            off the chin moves with the chin, not with the neck it happens to be nearest
  "morphs": bool            copy the body's shape keys onto the hair (each vertex moves as the skin under it does), so
                            the face's morph targets also drive it; keys that move it less than "morph_min" mm
                            (default 0.05) are left out
  "seed": n
  "out": glb, "blend": path, "preview": dir   outputs; the preview renders front, side, back and a three-quarter
                            (or the [name, [x, y, z]] view directions in "preview_views")
"""
import json, math, os, sys

import bpy
import bmesh
import numpy as np
from mathutils import Vector, kdtree
from mathutils.bvhtree import BVHTree

spec = json.load(open(sys.argv[sys.argv.index("--") + 1]))
rng = np.random.default_rng(spec.get("seed", 1))
CM = 0.01
O = Vector(spec["center"])


def mirrored(pts, n):
    """Points [phi, ..] plus their -phi twins when the spec asks for it."""
    if not spec.get("mirror"):
        return [list(p) for p in pts]
    out = [list(p) for p in pts]
    out += [[-p[0]] + list(p[1:n]) for p in pts if abs(p[0]) > 1e-6 and abs(abs(p[0]) - 180) > 1e-6]
    return out


def head_angles(p):
    d = p - O
    return math.degrees(math.atan2(d.x, -d.y)), math.degrees(math.atan2(d.z, math.hypot(d.x, d.y)))


def periodic(pts):
    """A function of phi, piecewise linear through [phi, value] points, wrapping at +-180."""
    pts = sorted(pts)
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    xs = [xs[-1] - 360] + xs + [xs[0] + 360]
    ys = [ys[-1]] + ys + [ys[0]]
    return lambda phi: float(np.interp(((phi + 180) % 360) - 180, xs, ys))


def unit(phi, th):
    p, t = math.radians(phi), math.radians(th)
    return np.array([math.sin(p) * math.cos(t), -math.cos(p) * math.cos(t), math.sin(t)])


def field(pts, sigma):
    U = np.array([unit(p[0], p[1]) for p in pts])
    vals = np.array([p[2] for p in pts])
    s2 = math.radians(sigma) ** 2

    def f(phi, th):
        ang = np.arccos(np.clip(U @ unit(phi, th), -1, 1))
        w = np.exp(-(ang * ang) / s2) + 1e-9
        return float((w * vals).sum() / w.sum())
    return f


def smoothstep(a, b, x):
    if b <= a:
        return 1.0 if x >= a else 0.0
    t = min(max((x - a) / (b - a), 0.0), 1.0)
    return t * t * (3 - 2 * t)


hairline = periodic(mirrored(spec["hairline"], 2))
topline = periodic(mirrored(spec["topline"], 2)) if spec.get("topline") else None
holes = mirrored(spec.get("holes", []), 4)
sigma = spec.get("sigma", 22)
length_f = field(mirrored(spec["fields"]["length"], 3), sigma)
volume_f = field(mirrored(spec["fields"]["volume"], 3), sigma)
flow = spec["flow"]
split_from = periodic([[p[0], p[1]] for p in mirrored(flow["split"], 3)])
split_to = periodic([[p[0], p[2]] for p in mirrored(flow["split"], 3)])
part_phi, part_w = flow.get("part", [-999, 1])

# ---- the scalp: the body's rest-pose surface, with smooth normals interpolated over each triangle

body = bpy.data.objects[spec.get("body", "body")]
arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
dg = bpy.context.evaluated_depsgraph_get()
bme = body.evaluated_get(dg).to_mesh()
bme.calc_loop_triangles()
MW = body.matrix_world
NW = MW.to_3x3().inverted().transposed()
BV = [MW @ v.co for v in bme.vertices]
BN = [(NW @ v.normal).normalized() for v in bme.vertices]
tris = [tuple(t.vertices) for t in bme.loop_triangles]
bvh = BVHTree.FromPolygons(BV, tris)


def bary(loc, ti):
    """Barycentric weights of loc in body triangle ti."""
    a, b, c = (BV[i] for i in tris[ti])
    v0, v1, v2 = b - a, c - a, loc - a
    d00, d01, d11, d20, d21 = v0.dot(v0), v0.dot(v1), v1.dot(v1), v2.dot(v0), v2.dot(v1)
    den = d00 * d11 - d01 * d01 or 1e-12
    wb = (d11 * d20 - d01 * d21) / den
    wc = (d00 * d21 - d01 * d20) / den
    return 1 - wb - wc, wb, wc


def surface(p):
    """Nearest scalp point, its smooth normal (pointing out), and the signed height of p above it."""
    loc, fn, ti, _ = bvh.find_nearest(p)
    wa, wb, wc = bary(loc, ti)
    ia, ib, ic = tris[ti]
    n = (BN[ia] * wa + BN[ib] * wb + BN[ic] * wc).normalized()
    if n.dot(fn) < 0:
        n = -n
    return loc, n, (p - loc).dot(fn)


def inner(phi, th):
    """How far (phi, theta) is inside the hair, in degrees: above the hairline, below the topline, out of every hole."""
    d = th - hairline(phi)
    if topline:
        d = min(d, topline(phi) - th)
    for hp, ht, rp, rt in holes:
        dp = (((phi - hp) + 180) % 360 - 180) / rp
        d = min(d, (math.hypot(dp, (th - ht) / rt) - 1) * min(rp, rt))
    return d


def inside(phi, th, margin=0.0):
    return inner(phi, th) > margin


# Scalp triangles: every corner above the hairline.
outer_only = spec.get("outer_only", False)
def outer(t):
    """True when triangle t is the first skin a ray from outside toward the frame centre meets, so the mouth's inside,
    the nostrils and the ear's folds stay bare ("outer_only")."""
    c = (BV[t[0]] + BV[t[1]] + BV[t[2]]) / 3
    d = (c - O).normalized()
    hit = bvh.ray_cast(O + d * 0.3, -d)[0]
    return hit is None or (hit - c).length < 0.002


def tri_in(t):
    return all(inside(*head_angles(BV[i])) for i in t) and (not outer_only or outer(t))


scalp_tris = [t for t in tris if tri_in(t)]
areas = np.array([((BV[b] - BV[a]).cross(BV[c] - BV[a])).length / 2 for a, b, c in scalp_tris])
print("SCALP triangles %d, area %.1f cm2" % (len(scalp_tris), areas.sum() / CM / CM))


def scatter(n):
    """n random points on the scalp, area weighted, kept only above the (jittered) hairline."""
    pick = rng.choice(len(scalp_tris), size=n, p=areas / areas.sum())
    r1, r2 = rng.random(n), rng.random(n)
    out = []
    jit = spec.get("hairline_jitter", 1.5)
    for k, i in enumerate(pick):
        a, b, c = (BV[j] for j in scalp_tris[i])
        s = math.sqrt(r1[k])
        p = a * (1 - s) + b * (s * (1 - r2[k])) + c * (s * r2[k])
        phi, th = head_angles(p)
        if inside(phi, th, rng.random() * jit):
            out.append(p)
    return out


def poisson(cands, spacing_at):
    """Keep candidates at least their own spacing from every kept one (a grid hash, cell = the largest spacing)."""
    cell = max(spacing_at(p) for p in cands)
    grid, kept = {}, []
    for p in cands:
        s = spacing_at(p)
        key = tuple(int(math.floor(c / cell)) for c in p)
        near = False
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for q in grid.get((key[0] + dx, key[1] + dy, key[2] + dz), ()):
                        if (q - p).length < s:
                            near = True
                            break
                    if near:
                        break
                if near:
                    break
            if near:
                break
        if not near:
            grid.setdefault(key, []).append(p)
            kept.append(p)
    return kept


def flow_dir(p, phi, th, root_phi):
    w = smoothstep(split_from(phi), split_to(phi), th)
    small = 1 - smoothstep(part_phi - part_w, part_phi + part_w, root_phi) if -110 < root_phi < 0 else 0.0
    sweep = flow.get("sweep", 0.0) * (1 - small) - flow.get("sweep_small", 0.0) * small
    top = Vector((sweep, 1.0, 0.0))
    out = flow.get("out", 0.0) * math.copysign(max(math.cos(math.radians(phi)), 0.0) ** 2, phi)
    down = Vector((out, flow.get("down_back", 0.3), -1.0))
    return (top.normalized() * w + down.normalized() * (1 - w))


def grow(root, height_k, rnd_len, rnd_h):
    """A guide strand from root over the scalp, along the flow, standing off it toward its volume height. Returns its
    points, the scalp normal under each and the scalp point each was grown over (its anchor)."""
    phi0, th0 = head_angles(root)
    L = length_f(phi0, th0) * CM * rnd_len
    nseg = int(min(16, max(4, round(L / (0.5 * CM)))))
    ds = L / nseg
    loc, n, _ = surface(root)
    pts = [loc - n * spec.get("tuck", 0.2) * CM]
    normals = [n]
    anchors = [loc]
    prev = None
    h = 0.0
    walk = loc
    rise, climb = spec.get("rise", 0.35), spec.get("climb", 1.2)
    over = spec.get("overshoot", 1.5)
    for k in range(1, nseg + 1):
        loc, n, _ = surface(walk)
        phi, th = head_angles(loc)
        d = flow_dir(loc, phi, th, phi0)
        d = (d - n * d.dot(n))
        if d.length < 1e-6:
            d = prev or Vector((0, 0, -1))
        d.normalize()
        if prev is not None:
            d = (d * 0.6 + prev * 0.4).normalized()
        prev = d
        walk, n2, _ = surface(loc + d * ds)
        phi2, th2 = head_angles(walk)
        if inner(phi2, th2) < -over and k > 1:
            break
        t = k / nseg
        target = volume_f(phi2, th2) * CM * height_k * rnd_h
        h = min(target * smoothstep(0, rise, t) + 0.1 * CM, h + climb * ds)
        pts.append(walk + n2 * h)
        normals.append(n2)
        anchors.append(walk)
    return pts, normals, anchors


# ---- guide curves for each layer

layers = []
short_spacing = spec.get("short_spacing", 0.55)
for li, lay in enumerate(spec["layers"]):
    if "count" in lay:
        src = layers[-1]["roots"]
        idx = rng.choice(len(src), size=min(lay["count"], len(src)), replace=False)
        roots = [src[i] for i in idx]
    else:
        def spacing_at(p, lay=lay):
            phi, th = head_angles(p)
            return lay["spacing"] * CM * min(1.0, max(short_spacing, length_f(phi, th) / 3.0))
        cands = scatter(int(areas.sum() / (lay["spacing"] * CM) ** 2 * 6))
        cands = [p for p in cands if volume_f(*head_angles(p)) >= lay.get("min_volume", 0.0)]
        roots = poisson(cands, spacing_at)
    strands = []
    for r in roots:
        pts, nrm, anc = grow(r, lay["height"], rng.uniform(0.85, 1.1), rng.uniform(0.8, 1.05))
        if len(pts) >= 3:
            strands.append((pts, nrm, anc))
    layers.append({"spec": lay, "roots": roots, "strands": strands})
    print("LAYER %d: %d roots, %d strands" % (li, len(roots), len(strands)))

# The groom: every guide as a Blender hair curve, kept in the .blend to look at.
all_strands = [s for L in layers for s in L["strands"]]
try:
    hc = bpy.data.hair_curves.new("hair_guides")
    hc.add_curves([len(s[0]) for s in all_strands])
    hc.position_data.foreach_set("vector", [c for s in all_strands for p in s[0] for c in p])
    guides = bpy.data.objects.new("hair_guides", hc)
    bpy.context.scene.collection.objects.link(guides)
    guides.hide_render = True
except Exception as e:  # the curves API moved between versions; the cards don't need it
    print("GUIDES not kept:", e)

# ---- textures


def srgb_to_lin(c):
    c = np.asarray(c, dtype=np.float64) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


cols = spec["colors"]
C_ROOT, C_MID, C_TIP = (np.asarray(cols[k], dtype=np.float64) / 255.0 for k in ("root", "mid", "tip"))
JIT = cols.get("jitter", 0.06)


def strand_color(t, tone):
    t = np.asarray(t)[:, None]
    c = np.where(t < 0.3, C_ROOT + (C_MID - C_ROOT) * (t / 0.3) ** 0.7, C_MID + (C_TIP - C_MID) * ((t - 0.3) / 0.7))
    return np.clip(c * tone, 0, 1)


def draw(C, A, x, y, half, alpha, col, wrap_y):
    """Composite one strand: for each column x, a soft line at height y, half-width half, over C/A (premultiplied)."""
    H = A.shape[0]
    for off in range(-3, 4):
        ry = np.floor(y).astype(int) + off
        cov = np.clip(half + 0.5 - np.abs(ry + 0.5 - y), 0, 1) * alpha
        if wrap_y:
            ry = ry % H
        else:
            ok = (ry >= 0) & (ry < H)
            ry, cx, cov2, cc = ry[ok], x[ok], cov[ok], col[ok]
            A[ry, cx] = A[ry, cx] * (1 - cov2) + cov2
            C[ry, cx] = C[ry, cx] * (1 - cov2[:, None]) + cc * cov2[:, None]
            continue
        A[ry, x] = A[ry, x] * (1 - cov) + cov
        C[ry, x] = C[ry, x] * (1 - cov[:, None]) + col * cov[:, None]


def finish(C, A, fill):
    """Straight (un-premultiplied) RGBA with the empty texels coloured like the hair, so mips don't darken edges."""
    rgb = np.where(A[..., None] > 0.02, C / np.maximum(A[..., None], 1e-6), fill)
    return np.concatenate([np.clip(rgb, 0, 1), A[..., None]], axis=2)


def atlas_texture():
    at = spec["atlas"]
    S, R = at["size"], at["rows"]
    C = np.zeros((S, S, 3))
    A = np.zeros((S, S))
    band = S / R
    thick = at.get("thick", [2.5, 4.0])
    wave, waves = at.get("wave", [0.5, 3.0]), at.get("waves", [1.0, 3.0])
    for r in range(R):
        y0 = r * band
        pad = band * 0.06
        n = at["strands"][r]
        clump = at["clump"][r]
        centres = [y0 + band * f for f in (0.3, 0.5, 0.7)]
        for _ in range(n):
            # Strands bunch toward the middle of the band, so a card's long edges are wispy rather than cut.
            v0 = y0 + band / 2 + np.clip(rng.normal(0, 0.2), -0.44, 0.44) * band
            u0 = rng.random() ** 2 * 0.25 * S
            u1 = (0.55 + 0.45 * rng.random()) * S
            x = np.arange(int(math.ceil(u0)), int(u1))
            t = np.clip((x - u0) / (u1 - u0), 0, 1)
            c = min(centres, key=lambda cc: abs(cc - v0))
            amp, ph, fr = rng.uniform(*wave), rng.uniform(0, 6.28), rng.uniform(*waves)
            y = v0 + (c - v0) * clump * t ** 1.5 + amp * np.sin(ph + fr * 6.28 * t) * t
            y = np.clip(y, y0 + pad, y0 + band - pad)
            w0 = rng.uniform(*thick)
            half = 0.5 * w0 * np.where(t > 0.6, 1 - 0.75 * (t - 0.6) / 0.4, 1.0)
            alpha = np.where(t > 0.7, 1 - 0.8 * (t - 0.7) / 0.3, 1.0) * np.where(t < 0.06, t / 0.06, 1.0)
            tone = 1 + rng.uniform(-JIT, JIT)
            # Colour by the card's U, not the strand's own length: every root on a card darkens at the scalp.
            draw(C, A, x, y, half, alpha, strand_color(x / S, tone), False)
    return finish(C, A, C_MID)


def cap_texture():
    cp = spec["cap"]
    S = cp["size"]
    C = np.zeros((S, S, 3))
    A = np.zeros((S, S))
    # A solid underlayer from just inside the hairline: skin never shows between the cards.
    u = (np.arange(S) + 0.5) / S
    fill_a = np.array([smoothstep(*cp.get("fill", [0.05, 0.3]), x) for x in u])
    base = np.asarray(cp.get("color", cols["root"]), dtype=np.float64) / 255.0
    A[:] = fill_a[None, :]
    C[:] = base[None, None, :] * fill_a[None, :, None]
    # Short strands along U, sparse at the hairline and dense inside.
    n = int(cp.get("strands", 2600))
    for _ in range(n):
        u0 = 0.03 + rng.random() * 0.92  # column 0 stays empty: the cap below the hairline samples it
        if rng.random() > 0.35 + smoothstep(0.0, 0.3, u0):
            continue
        ln = rng.uniform(0.08, 0.22)
        x = np.arange(int(u0 * S), int(min(u0 + ln, 1.0) * S))
        if len(x) < 4:
            continue
        t = (x - x[0]) / max(len(x) - 1, 1)
        y = rng.random() * S + rng.uniform(-3, 3) * t
        half = 0.5 * rng.uniform(2.0, 3.2) * np.where(t > 0.6, 1 - 0.7 * (t - 0.6) / 0.4, 1.0)
        alpha = np.where(t > 0.75, 1 - 0.7 * (t - 0.75) / 0.25, 1.0)
        draw(C, A, x, y, half, alpha, strand_color(0.2 + t * 0.6, 1 + rng.uniform(-JIT, JIT)), True)
    return finish(C, A, base)


def to_image(name, rgba):
    S = rgba.shape[0]
    img = bpy.data.images.new(name, S, S, alpha=True)
    img.colorspace_settings.name = "sRGB"
    # Blender images are bottom-up; row 0 here is V = 0 (the bottom), which is what glTF's flip expects.
    img.pixels.foreach_set(np.ascontiguousarray(rgba, dtype=np.float32).ravel())
    img.file_format = "PNG"
    img.alpha_mode = "STRAIGHT"
    out = spec.get("preview")
    if out:
        os.makedirs(out, exist_ok=True)
        img.filepath_raw = os.path.join(out, name + ".png")
        img.save()
    img.pack()
    return img


atlas_img = to_image("T_hair_cards", atlas_texture())
cap_img = to_image("T_hair_cap", cap_texture())


def hair_material(name, img):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    rnd = nt.nodes.new("ShaderNodeMath")
    rnd.operation = "ROUND"  # the glTF exporter reads image alpha through a Round as alphaMode MASK, cutoff 0.5
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(tex.outputs["Alpha"], rnd.inputs[0])
    nt.links.new(rnd.outputs[0], bsdf.inputs["Alpha"])
    bsdf.inputs["Metallic"].default_value = 0.0
    bsdf.inputs["Roughness"].default_value = 0.45
    m.use_backface_culling = False
    try:
        m.surface_render_method = "DITHERED"
    except AttributeError:
        m.blend_method = "CLIP"
    return m


mat_cards = hair_material("hair_cards", atlas_img)
mat_cap = hair_material("hair_cap", cap_img)

# ---- cards along the guides

at_rows = spec["atlas"]["rows"]
clear = spec.get("clear", 0.15) * CM
bulge = spec.get("bulge", 0.12)
verts, vnorm, faces, uvs, mats = [], [], [], [], []
vanchor = []  # the scalp point each vertex binds to with "bind": "anchor"
# Card normals lean to the scalp's, so neighbouring cards shade as one mass instead of as flat flakes.
shell = spec.get("shell_normals", 0.8)


def push_out(p):
    loc, n, h = surface(p)
    return p if h >= clear else loc + n * clear


for L in layers:
    lay = L["spec"]
    for pts, nrm, anc in L["strands"]:
        m = len(pts)
        row = int(rng.choice(lay["rows"]))
        vlo = (row + 0.04) / at_rows
        vhi = (row + 0.96) / at_rows
        if rng.random() < 0.5:
            vlo, vhi = vhi, vlo
        phi0, th0 = head_angles(pts[0])
        # Short hair gets narrow cards: a wide card on a 1 cm strand reads as a chip.
        w = lay["width"] * CM * min(1.0, max(0.4, length_f(phi0, th0) / 4.0)) * rng.uniform(0.85, 1.15)
        twist = math.radians(rng.uniform(-1, 1) * lay.get("twist", 10))
        arc = [0.0]
        for i in range(1, m):
            arc.append(arc[-1] + (pts[i] - pts[i - 1]).length)
        base = len(verts)
        for i in range(m):
            T = (pts[min(i + 1, m - 1)] - pts[max(i - 1, 0)]).normalized()
            N = nrm[i]
            B = T.cross(N).normalized()
            B = (B * math.cos(twist) + T.cross(B) * math.sin(twist)).normalized()
            Cn = B.cross(T).normalized()
            if Cn.dot(N) < 0:
                Cn = -Cn
            t = arc[i] / arc[-1]
            wi = w * (1 - 0.5 * t)
            row_pts = [pts[i] - B * wi / 2, pts[i] + Cn * wi * bulge, pts[i] + B * wi / 2]
            if i > 0:
                row_pts = [push_out(p) for p in row_pts]
            verts.extend(row_pts)
            vanchor.extend([anc[i]] * 3)
            for q in row_pts:
                vnorm.append((surface(q)[1] * shell + Cn * (1 - shell)).normalized())
            u = t
            uvs.append([(u, vlo), (u, (vlo + vhi) / 2), (u, vhi)])
        for i in range(m - 1):
            a = base + i * 3
            b = a + 3
            faces += [(a, a + 1, b + 1, b), (a + 1, a + 2, b + 2, b + 1)]  # front faces outward, with the normals
            mats += [0, 0]

card_uv = [uv for row in uvs for uv in row]
print("CARDS %d verts, %d quads" % (len(verts), len(faces)))

# ---- the cap: a shell over the scalp, lifted along the smooth normal. It takes every triangle that reaches above the
# hairline and fades in from the hairline curve itself, so the mesh's stepped edge never shows.

cp = spec["cap"]
cap_tris = [t for t in tris if any(inside(*head_angles(BV[i]), -1.0) for i in t) and (not outer_only or outer(t))]
bm = bmesh.new()
vmap = {}
for t in cap_tris:
    for i in t:
        if i not in vmap:
            vmap[i] = bm.verts.new(BV[i])
for t in cap_tris:
    try:
        bm.faces.new([vmap[i] for i in t])
    except ValueError:
        pass
bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-6)
for _ in range(cp.get("subdiv", 2)):
    bmesh.ops.subdivide_edges(bm, edges=bm.edges[:], cuts=1, use_grid_fill=True)
bmesh.ops.triangulate(bm, faces=bm.faces[:])
fade = cp.get("fade", 1.2) * CM
# V runs round the head with a whole number of tiles, so it closes up at the back.
radius = sum((v.co - O).length for v in bm.verts) / max(len(bm.verts), 1)
tiles = max(1, round(2 * math.pi * radius / (cp.get("tile", 4.0) * CM)))
cap_base = len(verts)
cap_index, cap_u, cap_phi = {}, {}, {}
for v in bm.verts:
    loc, n, _ = surface(v.co)
    phi, th = head_angles(loc)
    above = math.radians(inner(phi, th)) * (loc - O).length
    cap_u[v.index] = min(max(above / fade, 0.0), 1.0) * 0.97
    cap_phi[v.index] = phi
    cap_index[v.index] = len(verts)
    verts.append(loc + n * cp.get("offset", 0.15) * CM)
    vnorm.append(n)
    vanchor.append(loc)
cap_faces = []
cap_uv = []
for f in bm.faces:
    us = [cap_u[v.index] for v in f.verts]
    if max(us) <= 0.0:
        continue
    ph = [cap_phi[v.index] for v in f.verts]
    if max(ph) - min(ph) > 180:  # across the back seam
        ph = [x + 360 if x < 0 else x for x in ph]
    vs = [x / 360 * tiles for x in ph]
    cap_faces.append(tuple(cap_index[v.index] for v in f.verts))
    cap_uv.append(list(zip(us, vs)))
bm.free()
print("CAP %d verts, %d faces" % (len(verts) - cap_base, len(cap_faces)))

# ---- one mesh, two materials, skinned to the head bone or to the body's own weights under each vertex


def skin_points():
    """For each hair vertex, the body triangle and barycentric weights of the skin point it binds to ("bind")."""
    if len(body.data.vertices) != len(BV):
        raise SystemExit("skin: the evaluated body changed topology; can't map its weights or shape keys")
    src = vanchor if spec.get("bind", "nearest") == "anchor" else verts
    out = []
    for p in src:
        loc, _, ti, _ = bvh.find_nearest(p)
        out.append((ti, bary(loc, ti)))
    return out


def skin_from_body(points):
    """For each hair vertex, the body's bone weights at its skin point (at most 4 bones, normalised), so hair
    at the nape follows the neck as the skin under it does."""
    names = {g.index: g.name for g in body.vertex_groups}
    bones = {b.name for b in arm.data.bones if b.use_deform}
    bw = [{names[g.group]: g.weight for g in v.groups if names.get(g.group) in bones and g.weight > 0}
          for v in body.data.vertices]
    out = []
    for ti, kk in points:
        acc = {}
        for i, k in zip(tris[ti], kk):
            for name, w in bw[i].items():
                acc[name] = acc.get(name, 0.0) + w * max(k, 0.0)
        top = sorted(acc.items(), key=lambda kv: -kv[1])[:4]
        s = sum(w for _, w in top)
        out.append({n: w / s for n, w in top} if s > 1e-6 else {bone: 1.0})
    return out


def morphs_from_body(obj, points):
    """The body's shape keys on the hair: each vertex moves by the key's offset at its skin point."""
    keys = body.data.shape_keys
    if not keys:
        return []
    def coords(kb):
        a = np.empty(len(kb.data) * 3)
        kb.data.foreach_get("co", a)
        return a.reshape(-1, 3)
    basis = coords(keys.reference_key)
    M3 = np.array(MW.to_3x3())
    tri_idx = np.array([tris[ti] for ti, _ in points])
    tri_w = np.clip(np.array([kk for _, kk in points]), 0.0, None)
    tri_w /= np.maximum(tri_w.sum(axis=1, keepdims=True), 1e-9)
    rest = np.array([tuple(v.co) for v in obj.data.vertices])
    kept = []
    for kb in keys.key_blocks:
        if kb == keys.reference_key:
            continue
        delta = (coords(kb) - basis) @ M3.T
        d = (delta[tri_idx] * tri_w[:, :, None]).sum(axis=1)
        if np.abs(d).max() < spec.get("morph_min", 0.05) * 0.001:
            continue
        if obj.data.shape_keys is None:
            obj.shape_key_add(name="Basis", from_mix=False)
        sk = obj.shape_key_add(name=kb.name, from_mix=False)
        sk.value = 0.0  # added keys start at 1; the export's default morph weights come from these
        sk.data.foreach_set("co", (rest + d).ravel())
        kept.append(kb.name)
    return kept


me = bpy.data.meshes.new("hair")
me.from_pydata([tuple(v) for v in verts], [], faces + cap_faces)
me.materials.append(mat_cards)
me.materials.append(mat_cap)
uvl = me.uv_layers.new(name="UVMap")
loop_uv = []
for fi, poly in enumerate(me.polygons):
    if fi < len(faces):
        poly.material_index = 0
        loop_uv += [card_uv[vi] for vi in poly.vertices]
    else:
        poly.material_index = 1
        loop_uv += cap_uv[fi - len(faces)]
uvl.data.foreach_set("uv", [c for uv in loop_uv for c in uv])
me.update()
for p in me.polygons:
    p.use_smooth = True
me.normals_split_custom_set_from_vertices([tuple(n) for n in vnorm])
hair = bpy.data.objects.new("hair", me)
bpy.context.scene.collection.objects.link(hair)
bone = spec.get("bone", "head")
points = skin_points() if spec.get("skin_from_body") or spec.get("morphs") else None
if spec.get("skin_from_body"):
    groups = {}
    for vi, ws in enumerate(skin_from_body(points)):
        for name, w in ws.items():
            if name not in groups:
                groups[name] = hair.vertex_groups.new(name=name)
            groups[name].add([vi], w, "REPLACE")
    print("SKIN from body: %s" % sorted(groups))
else:
    vg = hair.vertex_groups.new(name=bone)
    vg.add(list(range(len(verts))), 1.0, "REPLACE")
if spec.get("morphs"):
    print("MORPHS %s" % morphs_from_body(hair, points))
hair.parent = arm
mod = hair.modifiers.new("Armature", "ARMATURE")
mod.object = arm
print("HAIR %d verts, %d faces" % (len(me.vertices), len(me.polygons)))

if spec.get("blend"):
    os.makedirs(os.path.dirname(spec["blend"]), exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=spec["blend"])

if spec.get("out"):
    for o in bpy.context.scene.objects:
        o.select_set(False)
    arm.select_set(True)
    hair.select_set(True)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.export_scene.gltf(filepath=spec["out"], export_format="GLB", use_selection=True, export_animations=False,
                              export_morph=bool(spec.get("morphs")), export_morph_normal=False)
    raw = open(spec["out"], "rb").read()
    jlen = int.from_bytes(raw[12:16], "little")
    gl = json.loads(raw[20:20 + jlen])
    print("GLB %s %d bytes; materials %s" % (spec["out"], len(raw),
          [(m.get("name"), m.get("alphaMode", "OPAQUE")) for m in gl.get("materials", [])]))

# ---- preview: the head with its hair from four sides

out = spec.get("preview")
if out:
    sc = bpy.context.scene
    for eng in ("BLENDER_EEVEE", "BLENDER_EEVEE_NEXT"):
        try:
            sc.render.engine = eng
            break
        except TypeError:
            continue
    sc.render.resolution_x = sc.render.resolution_y = int(spec.get("preview_size", 800))
    sc.render.film_transparent = False
    for o in sc.objects:
        if o.type == "LIGHT":
            o.hide_render = True
    sun = bpy.data.objects.new("key", bpy.data.lights.new("key", "SUN"))
    sun.data.energy = 3.0
    sun.rotation_euler = (math.radians(50), 0, math.radians(-35))
    sc.collection.objects.link(sun)
    if sc.world is None:
        sc.world = bpy.data.worlds.new("w")
    sc.world.use_nodes = True
    bg = sc.world.node_tree.nodes.get("Background")
    if bg:
        bg.inputs[0].default_value = (0.35, 0.35, 0.37, 1)
        bg.inputs[1].default_value = 0.8
    cam = bpy.data.objects.new("hair_cam", bpy.data.cameras.new("hair_cam"))
    sc.collection.objects.link(cam)
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = spec.get("preview_scale", 0.34)
    sc.camera = cam
    views = spec.get("preview_views") or (("front", (0, -1, 0)), ("side", (1, 0, 0)), ("back", (0, 1, 0)),
                                          ("q", (0.7, -0.7, 0.2)), ("sideR", (-1, 0, 0)), ("top", (0, -0.3, 1)))
    for name, d in views:
        dv = Vector(d).normalized()
        cam.location = O + dv * 1.0 + Vector((0, 0, -0.03))
        cam.rotation_euler = (-dv).to_track_quat("-Z", "Y").to_euler()
        sc.render.filepath = os.path.join(out, "hair_%s.png" % name)
        bpy.ops.render.render(write_still=True)
    print("PREVIEW", out)
