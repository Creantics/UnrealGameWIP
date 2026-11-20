"""Write a surface set transposed (rows become columns) under a new id, for faces whose grain must run along V.

M_WorldSurfaceTex projects a box's sides from its object axes with U horizontal, so a set whose grain runs along U
(solhaven_oak_plank: its boards run along U) lays that grain flat on every side face. The transposed set runs it along
V, up a standing post's sides, while the box stays upright, so the master's weathering (the foot grime band and the
edge wear, both read on the box's local Z) stays where it belongs. The master is not touched.

diff and arm are transposed as they are. The DirectX normal is transposed and its R and G swapped: R leans along +u and
G along +v, so after the swap each channel still leans along the axis it was drawn for. Blue, the tile size and the
resolution are kept, so the surfaces.json row carries the source's tileCm and res.

    python -I tools/make_transposed_surface.py <src_dir> <src_id> <out_dir> <out_id> [--tag 1k]

Reads <src_dir>/<src_id>_{diff,arm,nor_dx}_<tag>.jpg. Writes <out_dir>/<out_id>_{diff,arm,nor_dx}_<tag>.jpg (JPEG
quality 95, no chroma subsampling), <out_id>_tile2x2.jpg (the diff tiled 2 x 2, to check the seams) and
<out_id>_report.json (the source, the maps and the round-trip error: each map transposed back against its source).
"""

import argparse
import json
import os
import sys

import numpy as np
from PIL import Image

MAPS = ("diff", "arm", "nor_dx")


def transpose_map(name, img):
    a = np.asarray(img.convert("RGB"), dtype=np.uint8)
    t = np.ascontiguousarray(a.transpose(1, 0, 2))
    if name == "nor_dx":
        t = np.ascontiguousarray(t[:, :, [1, 0, 2]])
    return t


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src_dir")
    ap.add_argument("src_id")
    ap.add_argument("out_dir")
    ap.add_argument("out_id")
    ap.add_argument("--tag", default="1k")
    args = ap.parse_args()
    if args.src_id == args.out_id:
        sys.exit("out_id must differ from src_id")
    os.makedirs(args.out_dir, exist_ok=True)
    report = {"id": args.out_id, "source_id": args.src_id, "tag": args.tag, "transposed": True, "normal_rg_swapped": True, "maps": {}}
    diff = None
    for name in MAPS:
        src = os.path.join(args.src_dir, f"{args.src_id}_{name}_{args.tag}.jpg")
        with Image.open(src) as img:
            w, h = img.size
            if w != h:
                sys.exit(f"{src}: {w} x {h}, not square (the tile size would change on one axis)")
            t = transpose_map(name, img)
            back = t[:, :, [1, 0, 2]] if name == "nor_dx" else t
            err = int(np.abs(back.transpose(1, 0, 2).astype(np.int16) - np.asarray(img.convert("RGB"), dtype=np.int16)).max())
        out = os.path.join(args.out_dir, f"{args.out_id}_{name}_{args.tag}.jpg")
        Image.fromarray(t, "RGB").save(out, quality=95, subsampling=0)
        report["maps"][name] = {"source": os.path.basename(src), "size": w, "round_trip_max_error": err}
        if name == "diff":
            diff = t
    tile = np.tile(diff, (2, 2, 1))
    Image.fromarray(tile, "RGB").resize((tile.shape[1] // 2, tile.shape[0] // 2), Image.LANCZOS).save(
        os.path.join(args.out_dir, f"{args.out_id}_tile2x2.jpg"), quality=90)
    with open(os.path.join(args.out_dir, f"{args.out_id}_report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1)
    print(json.dumps(report))


if __name__ == "__main__":
    main()
