# Tools

These are Python tools carried over unchanged from newengine `tools/`. They're pure Python, run with `python3 -I`, and need no Blender or engine.

The design package (`design/forgotten_third/`) and the generated assets still live in the newengine checkout. Run the tools with their path options pointing there:

| Tool | What it does | Run from here |
|---|---|---|
| `story_import/` | Turns the script and design CSVs into story JSON (`docs/reference/story_format.md`). `ftcore::StoryImport` reads that output. | `python3 -I tools/story_import/story_import.py --build-dir ../newengine/design/forgotten_third/build --out-dir ../newengine/assets/story` |
| `lipsync/bake_face_tracks.py` | Turns phoneme alignment into `.nface` tracks (`docs/reference/lipsync_track_format.md`). `gold_set.json` is its hand-check set. | It imports `tools/blender/face_spec.py` by relative path. |
| `blender/face_spec.py` | The face shape spec: shapes, layers, visemes, expressions. With no argument it writes `assets/rig/face_presets.json`, so pass the output path. | `python3 -I tools/blender/face_spec.py ../newengine/assets/rig/face_presets.json` |
| `vo_direction/` | Extracts VO lines, carries their directions forward between drafts, and validates them. `test_vo_direction.py` is its unittest. | `build_directions.py` reads `design/forgotten_third/voice/voice_prompts.md` relative to this repo's root, so run it in newengine until the design package moves. |

These tools haven't been run from this repo yet, because this PC has no Python.
