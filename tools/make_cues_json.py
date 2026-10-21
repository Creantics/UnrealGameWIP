"""Writes Data/Audio/cues.json (ids and numbers only) from the private cue sheet.

    python tools/make_cues_json.py <cues_final.csv> [Data/Audio/cues.json]

The cue sheet lives in the newengine design folder and stays private. Only these columns are read:
cue_id, type, loop_or_stinger, tempo_bpm. Names, descriptions and notes never reach the output.
Re-running keeps hand edits to existing entries (file, offset, loop points, fades, gain); it only
adds cues that are new in the sheet and drops ones the sheet no longer has.
"""

import csv
import json
import re
import sys
from pathlib import Path

# A cue id ending in -NONE means "no music": the game handles it as a stop, so it has no file.
SILENCE_SUFFIX = "-NONE"
MUSIC_FADE_IN = 1.0
MUSIC_FADE_OUT = 2.0


def playback_for(kind: str, loop: str) -> tuple[str, str]:
    """(bus, playback) from the sheet's type and loop_or_stinger columns."""
    kind = kind.strip().lower()
    loop = loop.strip().lower()
    if kind == "sfx":
        # A bare "layer" is a held bed under a scene; everything else fires once.
        return ("ambience", "loop") if loop == "layer" else ("sfx", "oneshot")
    if kind == "stinger" or (loop.startswith("stinger") and "loop" not in loop):
        return "music", "stinger"
    if loop in ("scene", "finale cue") or "single" in loop:
        return "music", "oneshot"
    return "music", "loop"


def bpm_for(tempo: str):
    """A single tempo when the sheet gives one plainly ("72", "72 (4/4)", "54 dotted-quarter"), else None."""
    m = re.match(r"^(\d+(?:\.\d+)?)(.*)$", tempo.strip())
    if not m:
        return None
    rest = m.group(2)
    if any(bad in rest for bad in (" or ", ";", "->", "-", "/")) and not re.fullmatch(r"\s*\([^)]*\)", rest):
        return None
    if rest and not (rest.startswith(" (") or rest.startswith(" dotted")):
        return None
    value = float(m.group(1))
    return int(value) if value.is_integer() else value


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    sheet = Path(sys.argv[1])
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parent.parent / "Data/Audio/cues.json"

    existing = {}
    if out.exists():
        for cue in json.loads(out.read_text(encoding="utf-8")).get("cues", []):
            existing[cue["id"]] = cue

    cues = []
    with sheet.open(encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            cue_id = row["cue_id"].strip()
            if not cue_id or cue_id.endswith(SILENCE_SUFFIX):
                continue
            bus, playback = playback_for(row["type"], row["loop_or_stinger"])
            cue = {"id": cue_id, "bus": bus, "playback": playback}
            bpm = bpm_for(row["tempo_bpm"])
            if bpm is not None and bus == "music":
                cue["bpm"] = bpm
            if bus == "music" and playback == "loop":
                cue["fadeIn"] = MUSIC_FADE_IN
                cue["fadeOut"] = MUSIC_FADE_OUT
            # Hand edits win over the generated defaults.
            for key, value in existing.get(cue_id, {}).items():
                if key not in ("id", "bus", "playback", "bpm"):
                    cue[key] = value
            cues.append(cue)

    out.parent.mkdir(parents=True, exist_ok=True)
    lines = ",\n".join("    " + json.dumps(c, ensure_ascii=True) for c in cues)
    out.write_text('{\n  "version": 1,\n  "cues": [\n' + lines + "\n  ]\n}\n", encoding="utf-8", newline="\n")
    counts = {}
    for c in cues:
        counts[c["bus"] + " " + c["playback"]] = counts.get(c["bus"] + " " + c["playback"], 0) + 1
    print(f"{out}: {len(cues)} cues ({', '.join(f'{v} {k}' for k, v in sorted(counts.items()))})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
