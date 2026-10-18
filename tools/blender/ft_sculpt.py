"""Scripted sculpting for headless Blender.

Blender's sculpt brushes (bpy.ops.sculpt.*) need a 3D viewport, so they don't run under `blender -b`. This module
does the same kind of work on a mesh's vertex array with numpy: brushes with a smooth falloff, applied at a point
or along a stroke (a list of points). Typical use:

    import ft_sculpt as fs
    s = fs.Sculpt.sphere("Rock", radius=1.0, subdivisions=6)
    s.draw(stroke, radius=0.3, strength=0.05)        # raise along the normal
    s.crease(stroke, radius=0.1, strength=0.02)      # cut a groove
    s.smooth(radius=None, iterations=2)              # relax everything
    s.commit()                                       # write back to the Blender mesh

Units are metres, Blender axes (+Z up, front -Y). Every brush is deterministic.
"""
import bmesh
import bpy
import numpy as np


def falloff(d, radius):
    """Smooth 1 -> 0 over [0, radius] (Blender's 'smooth' brush curve)."""
    t = np.clip(1.0 - d / radius, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def resample(points, spacing):
    """Points along a polyline, `spacing` apart, so strokes deposit evenly."""
    pts = np.asarray(points, dtype=float)
    if len(pts) < 2:
        return pts
    seg = np.linalg.norm(np.diff(pts, axis=0), axis=1)
    total = seg.sum()
    n = max(int(total / spacing) + 1, 2)
    at = np.linspace(0.0, total, n)
    cum = np.concatenate([[0.0], np.cumsum(seg)])
    return np.stack([np.interp(at, cum, pts[:, k]) for k in range(3)], axis=1)


class Sculpt:
    def __init__(self, obj):
        self.obj = obj
        mesh = obj.data
        self.v = np.empty((len(mesh.vertices), 3))
        mesh.vertices.foreach_get("co", self.v.ravel())
        edges = np.empty((len(mesh.edges), 2), dtype=np.int64)
        mesh.edges.foreach_get("vertices", edges.ravel())
        self.edges = edges
        # Fan-triangulate the polygons once, for vectorised normals.
        tris = [(p.vertices[0], p.vertices[i], p.vertices[i + 1])
                for p in mesh.polygons for i in range(1, len(p.vertices) - 1)]
        self.tris = np.array(tris, dtype=np.int64).reshape(-1, 3)
        deg = np.bincount(edges.ravel(), minlength=len(self.v))
        self.degree = np.maximum(deg, 1)

    # ---- construction -------------------------------------------------------------------------------------
    @classmethod
    def sphere(cls, name, radius=1.0, subdivisions=5, location=(0, 0, 0)):
        """A quad sphere (subdivided cube projected to a sphere): even topology, good for sculpting."""
        mesh = bpy.data.meshes.new(name)
        bm = bmesh.new()
        bmesh.ops.create_cube(bm, size=2.0)
        bmesh.ops.subdivide_edges(bm, edges=bm.edges[:], cuts=2 ** subdivisions - 1, use_grid_fill=True)
        for vert in bm.verts:
            vert.co = vert.co.normalized() * radius
        bm.to_mesh(mesh)
        bm.free()
        obj = bpy.data.objects.new(name, mesh)
        obj.location = location
        bpy.context.scene.collection.objects.link(obj)
        s = cls(obj)
        s.smooth(iterations=3, keep_volume=True)
        return s

    # ---- geometry queries ---------------------------------------------------------------------------------
    def normals(self):
        a, b, c = (self.v[self.tris[:, k]] for k in range(3))
        fn = np.cross(b - a, c - a)
        n = np.zeros_like(self.v)
        for k in range(3):
            np.add.at(n, self.tris[:, k], fn)
        length = np.linalg.norm(n, axis=1, keepdims=True)
        return n / np.maximum(length, 1e-12)

    def nearest_surface(self, p):
        """The vertex position closest to p: use it to put a stroke on the surface."""
        return self.v[np.argmin(np.linalg.norm(self.v - np.asarray(p), axis=1))].copy()

    def _weights(self, centre, radius):
        return falloff(np.linalg.norm(self.v - centre, axis=1), radius)

    def _stroke(self, stroke, radius, fn, spacing=None):
        pts = resample(stroke, spacing or radius * 0.25) if np.ndim(stroke) == 2 else [np.asarray(stroke, float)]
        for p in pts:
            fn(np.asarray(p, float), radius)

    # ---- brushes ------------------------------------------------------------------------------------------
    def draw(self, stroke, radius, strength):
        """Raise (strength > 0) or dig (< 0) along the surface normal. `stroke` is one point or a polyline."""
        n = self.normals()

        def dab(c, r):
            w = self._weights(c, r)
            self.v += n * (w * strength)[:, None]
        self._stroke(stroke, radius, dab)

    def inflate(self, stroke, radius, strength):
        """Like draw, but each dab uses fresh normals, so forms swell outward."""
        def dab(c, r):
            w = self._weights(c, r)
            if w.any():
                self.v += self.normals() * (w * strength)[:, None]
        self._stroke(stroke, radius, dab)

    def grab(self, centre, radius, offset):
        """Pull a region by a vector, like the Grab brush."""
        w = self._weights(np.asarray(centre, float), radius)
        self.v += w[:, None] * np.asarray(offset, float)

    def crease(self, stroke, radius, strength, pinch=0.5):
        """Cut a sharp groove: dig along the normal and pinch vertices towards the stroke line."""
        n = self.normals()

        def dab(c, r):
            w = self._weights(c, r)
            self.v -= n * (w * strength)[:, None]
            self.v += (c - self.v) * (w * pinch * 0.2)[:, None]
        self._stroke(stroke, radius, dab)

    def flatten(self, centre, radius, strength=1.0):
        """Press a region towards its average plane, like the Flatten brush."""
        c = np.asarray(centre, float)
        w = self._weights(c, radius)
        sel = w > 0
        if sel.sum() < 3:
            return
        pts = self.v[sel]
        mean = np.average(pts, axis=0, weights=w[sel])
        normal = np.average(self.normals()[sel], axis=0, weights=w[sel])
        normal /= max(np.linalg.norm(normal), 1e-12)
        dist = (self.v - mean) @ normal
        self.v -= normal * (dist * w * strength)[:, None]

    def smooth(self, centre=None, radius=None, iterations=1, strength=0.5, keep_volume=False):
        """Laplacian relax, everywhere or under a brush. keep_volume uses Taubin smoothing (no shrinking)."""
        w = np.ones(len(self.v)) if centre is None else self._weights(np.asarray(centre, float), radius)
        for _ in range(iterations):
            for lam in ((strength, -strength * 1.04) if keep_volume else (strength,)):
                avg = np.zeros_like(self.v)
                np.add.at(avg, self.edges[:, 0], self.v[self.edges[:, 1]])
                np.add.at(avg, self.edges[:, 1], self.v[self.edges[:, 0]])
                avg /= self.degree[:, None]
                self.v += (avg - self.v) * (w * lam)[:, None]

    def noise(self, scale, strength, seed=0, octaves=3):
        """Displace along the normal by smooth value noise: surface breakup for rock, bark, skin."""
        rng = np.random.default_rng(seed)
        n = self.normals()
        total = np.zeros(len(self.v))
        amp, freq = 1.0, 1.0 / scale
        for _ in range(octaves):
            dirs = rng.normal(size=(24, 3))
            dirs /= np.linalg.norm(dirs, axis=1, keepdims=True)
            phase = rng.uniform(0, 2 * np.pi, 24)
            total += amp * np.sin(self.v @ dirs.T * freq + phase).mean(axis=1)
            amp *= 0.5
            freq *= 2.0
        total /= max(total.std(), 1e-12)  # unit spread, so `strength` is the displacement's standard deviation
        self.v += n * (total * strength)[:, None]

    def trim(self, point, normal, radius=None):
        """Cut a crisp flat facet: vertices in front of the plane (point, normal) are pushed onto it, like the
        Trim / Line Project tools. `radius` limits it to a patch (soft-edged at the rim)."""
        p, nrm = np.asarray(point, float), np.asarray(normal, float)
        nrm = nrm / np.linalg.norm(nrm)
        dist = (self.v - p) @ nrm
        w = (dist > 0).astype(float)
        if radius is not None:
            w *= falloff(np.linalg.norm(self.v - p, axis=1), radius) ** 0.25
        self.v -= nrm * (dist * w)[:, None]

    # ---- output -------------------------------------------------------------------------------------------
    def commit(self, shade_smooth=True):
        mesh = self.obj.data
        mesh.vertices.foreach_set("co", self.v.ravel())
        mesh.update()
        if shade_smooth:
            mesh.shade_smooth()
        return self.obj


def material(obj, name, color, roughness=0.8):
    """A Principled BSDF material with a base colour (it also sets the viewport colour for previews)."""
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (*color, 1.0)
    bsdf.inputs["Roughness"].default_value = roughness
    mat.diffuse_color = (*color, 1.0)
    obj.data.materials.append(mat)
    return mat
