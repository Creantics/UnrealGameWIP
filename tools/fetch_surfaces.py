"""Fetch the world surface textures that Data/Field/surfaces.json lists.

    python -I tools/fetch_surfaces.py --out <dir> [--dry-run]

For each surface, it downloads three JPEG maps from the Poly Haven API at the surface's `res`:
the colour (diff), the DirectX normal (nor_dx) and the packed AO/roughness/metal (arm). Each
surface goes into its own folder, <dir>/<id>/<id>_{diff,nor_dx,arm}_<res>.jpg, the names that
-run=FTBuildSurfaces reads. A file that's already there with the API's MD5 is kept. Every source
must be CC0; anything else is refused. The files aren't committed: the import keeps each JPEG
inside its texture asset.
"""
import argparse
import hashlib
import json
import pathlib
import sys
import urllib.request

API = "https://api.polyhaven.com/files/"
PAGE = "https://polyhaven.com/a/"
MAPS = (("diff", "Diffuse"), ("nor_dx", "nor_dx"), ("arm", "arm"))
AGENT = {"User-Agent": "ForgottenThird-asset-fetch"}


def get(url, timeout=120):
    with urllib.request.urlopen(urllib.request.Request(url, headers=AGENT), timeout=timeout) as r:
        return r.read()


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="folder that gets one subfolder per surface")
    ap.add_argument("--data", default=str(root / "Data" / "Field" / "surfaces.json"))
    ap.add_argument("--dry-run", action="store_true", help="list the files and their total size only")
    args = ap.parse_args()

    table = json.loads(pathlib.Path(args.data).read_text(encoding="utf-8"))
    out = pathlib.Path(args.out)
    plan = []
    for s in table["surfaces"]:
        sid, res = s["id"], s.get("res", "2k")
        if s.get("license") != "CC0" or s.get("source") != PAGE + sid:
            print(f"fetch_surfaces: {sid}: only CC0 surfaces from {PAGE}<id> are fetched")
            return 1
        files = json.loads(get(API + sid))
        for short, key in MAPS:
            entry = files.get(key, {}).get(res, {}).get("jpg")
            if not entry:
                print(f"fetch_surfaces: {sid}: no {res} jpg {key} map")
                return 1
            plan.append((sid, out / sid / f"{sid}_{short}_{res}.jpg", entry["url"], entry["size"], entry["md5"]))

    total = sum(p[3] for p in plan)
    print(f"fetch_surfaces: {len(plan)} files for {len(table['surfaces'])} surfaces, {total / 1e6:.1f} MB")
    if args.dry_run:
        for sid, path, url, size, _ in plan:
            print(f"  {path.name}  {size / 1e6:.1f} MB")
        return 0

    fetched = kept = 0
    for sid, path, url, size, want in plan:
        if path.exists() and md5(path) == want:
            kept += 1
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        data = get(url, timeout=600)
        if hashlib.md5(data).hexdigest() != want or len(data) != size:
            print(f"fetch_surfaces: {path.name}: size or MD5 differs from the API's")
            return 1
        tmp = path.with_suffix(".part")
        tmp.write_bytes(data)
        tmp.replace(path)
        fetched += 1
        print(f"  {path.name}  {size / 1e6:.1f} MB")
    print(f"fetch_surfaces: fetched {fetched}, kept {kept}, in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
