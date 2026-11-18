# Tools

These are Python tools carried over from another checkout. They're pure Python, run with `python3 -I`, and need no Blender or engine.

The design package and the generated assets live in a private design folder outside this repo. Run the tools with their path options pointing there; story values (tables, names, the gold set) come from private files named by options:

| Tool | What it does | Run from here |
|---|---|---|
| `story_import/` | Turns the script and design CSVs into story JSON (`docs/reference/story_format.md`). `ftcore::StoryImport` reads that output. | `python3 -I tools/story_import/story_import.py --tables <private tables JSON> --build-dir <design build folder> --out-dir <assets>/story` |
| `lipsync/bake_face_tracks.py` | Turns phoneme alignment into `.nface` tracks (`docs/reference/lipsync_track_format.md`). The hand-check gold set is private (`--gold` or `LIPSYNC_GOLD`); `gold_sample.json` is a made-up three-line sample. | It imports `tools/blender/face_spec.py` by relative path. |
| `blender/face_spec.py` | The face shape spec: shapes, layers, visemes, expressions. With no argument it writes `assets/rig/face_presets.json`, so pass the output path. | `python3 -I tools/blender/face_spec.py <assets>/rig/face_presets.json` |
| `vo_direction/` | Extracts VO lines, carries their directions forward between drafts, and validates them. `test_vo_direction.py` is its unittest. | `build_directions.py` needs `--voice-prompts` (the design package's voice prompts) and `--names` (a private JSON of name overrides, aliases and voice references); `extract_lines.py` needs `--scene-prefix`. |
| `anim/clip_curves.py` | Curve report for a skinned `.glb`: per-clip joint speed, acceleration and jerk peaks, spikes, one-frame jumps, holds, C1 breaks, foot drift, root speed, loop closure and idle boundary diffs. Needs numpy; PNGs need matplotlib. Writes `curves.json`, `curves.md` and per-clip CSVs. | `python3 -I tools/anim/clip_curves.py <file.glb> <out_dir> [--clips Data/Anim/<boss>_clips.json] [--idle idle]` |

These tools haven't been run from this repo yet, because this PC has no Python.
