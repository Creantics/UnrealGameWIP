"""Test sculpt: a boulder, built with ft_sculpt brushes and exported as a static mesh.

    blender -b --factory-startup --python-exit-code 1 --python tools/blender/sculpt_boulder.py -- <out.glb>
"""
import os
import sys

import bpy
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ft_sculpt as fs  # noqa: E402


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    out = argv[0] if argv else "boulder.glb"
    bpy.ops.wm.read_factory_settings(use_empty=True)

    s = fs.Sculpt.sphere("Boulder", radius=0.6, subdivisions=5)
    s.v *= np.array([1.35, 1.0, 0.75])  # a wide, low stone
    s.v[:, 2] += 0.45

    # Big forms: one shoulder pulled up, the far side slumped, a flat bed on the ground.
    s.grab((0.5, -0.2, 0.75), 0.6, (0.08, -0.05, 0.18))
    s.grab((-0.6, 0.3, 0.6), 0.7, (-0.1, 0.06, -0.1))
    s.flatten((0, 0, 0.0), 0.9, strength=1.0)
    s.v[:, 2] = np.maximum(s.v[:, 2], 0.0)
    s.smooth(iterations=4, keep_volume=True)

    # Chipped planes: flatten patches all round so it reads as fractured stone, not a pebble.
    rng = np.random.default_rng(3)
    for _ in range(11):
        d = rng.normal(size=3)
        d[2] = abs(d[2]) * 0.8 + 0.15
        d /= np.linalg.norm(d)
        p = s.nearest_surface(np.array([0, 0, 0.45]) + d * 2.0)
        s.trim(p - d * rng.uniform(0.03, 0.08), d, radius=rng.uniform(0.35, 0.55))
    s.smooth(iterations=1, strength=0.25)

    # Secondary: lumps and a dent.
    s.inflate((-0.25, -0.55, 0.6), 0.3, 0.02)
    s.inflate((0.6, 0.3, 0.55), 0.25, 0.015)
    s.draw((0.1, -0.6, 0.35), 0.22, -0.06)

    # Cracks: strokes laid on the surface.
    crack1 = [s.nearest_surface(p) for p in [(-0.7, -0.5, 0.5), (-0.3, -0.6, 0.75), (0.1, -0.5, 0.95), (0.3, -0.2, 1.05)]]
    crack2 = [s.nearest_surface(p) for p in [(0.8, -0.3, 0.2), (0.7, -0.45, 0.55), (0.45, -0.55, 0.7)]]
    s.crease(crack1, radius=0.05, strength=0.025)
    s.crease(crack2, radius=0.045, strength=0.02)

    # Surface breakup, then a light relax to take the noise off the edges.
    s.noise(scale=0.3, strength=0.012, seed=7)
    s.noise(scale=0.06, strength=0.003, seed=11)

    obj = s.commit()
    fs.material(obj, "stone", (0.42, 0.40, 0.37), roughness=0.9)

    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.export_scene.gltf(filepath=out, export_format="GLB", use_selection=True, export_apply=True)
    print(f"sculpt_boulder: {len(s.v)} verts -> {out}")


main()
