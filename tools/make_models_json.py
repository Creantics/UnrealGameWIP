"""Writes Data/Assets/models.json, the id-only list of 3D models FTValidateAssets expects.

    python -I tools/make_models_json.py <ASSET_TRACKER.xlsx> --leads "<name>,<name>,..." [out]

Reads the tracker's "3D models" sheet (private; it stays outside this repo) and keeps only what the
game needs to check a delivery: the id, character or prop, lead or supporting, and the rule pack.
Names, worlds and prompts never leave the tracker. Hand edits to an existing entry (file, role,
pack) are kept on re-runs.
"""

import json
import os
import sys

import openpyxl

# --leads: the start of every row name that marks a lead's model (each lead's turnarounds and
# looks), comma separated. Given on the command line so no name is stored here; matched locally
# and never written out.


def pack_for(world):
    w = (world or "").lower()
    if "ink" in w:
        return "InkSolhaven"
    for key, pack in (("lattice", "Lattice"), ("unwritten", "Unwritten"), ("cleft", "Unwritten"),
                      ("solhaven", "Solhaven"), ("restored", "Restored")):
        if key in w:
            return pack
    return None


def main():
    args = sys.argv[1:]
    if "--leads" not in args or args.index("--leads") + 1 >= len(args):
        sys.exit(__doc__)
    i = args.index("--leads")
    lead_prefixes = tuple(n.strip().lower() for n in args[i + 1].split(",") if n.strip())
    del args[i:i + 2]
    if not args:
        sys.exit(__doc__)
    out_path = args[1] if len(args) > 1 else os.path.join(
        os.path.dirname(__file__), "..", "Data", "Assets", "models.json")
    wb = openpyxl.load_workbook(args[0], read_only=True)
    rows = list(wb["3D models"].iter_rows(values_only=True))
    head = [str(h) for h in rows[0]]
    col = {name: head.index(name) for name in ("ID", "Name", "World", "Type")}

    old = {}
    if os.path.exists(out_path):
        with open(out_path, encoding="utf-8") as f:
            old = {m["id"]: m for m in json.load(f)["models"]}

    models = []
    for r in rows[1:]:
        mid = r[col["ID"]]
        if not mid:
            continue
        kind = "prop" if "static" in (r[col["Type"]] or "").lower() else "character"
        entry = {"id": mid, "kind": kind}
        if kind == "character":
            name = (r[col["Name"]] or "").lower()
            enemy = "enemy" in (r[col["Type"]] or "").lower()
            entry["role"] = "lead" if not enemy and name.startswith(lead_prefixes) else "supporting"
        pack = pack_for(r[col["World"]])
        if pack:
            entry["pack"] = pack
        for key in ("file", "role", "pack"):
            if key in old.get(mid, {}):
                entry[key] = old[mid][key]
        models.append(entry)

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write('{ "version": 1, "models": [\n')
        f.write(",\n".join("  " + json.dumps(m) for m in models))
        f.write("\n] }\n")
    print(f"{len(models)} models -> {out_path}")


if __name__ == "__main__":
    main()
