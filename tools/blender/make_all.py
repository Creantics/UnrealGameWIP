#!/usr/bin/env python3
"""Regenerates every Blender-made test asset under <out>/ (default: assets/).

    python -m venv .venv && .venv/bin/pip install bpy        # once (bpy 5.x wheel, Python 3.11)
    .venv/bin/python tools/blender/make_all.py [--out assets]

Then convert the level: build/tools/ne_level_import assets/levels/blender/yard.glb ...  (docs/blender.md)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import make_character  # noqa: E402
import make_level  # noqa: E402
import make_prop  # noqa: E402

if __name__ == "__main__":
    for mod in (make_prop, make_character, make_level):
        mod.main()
