# story_import

Builds the runtime story JSON (`docs/story_format.md`) from the Forgotten Third design package.

    python3 -I tools/story_import/story_import.py            # regenerate assets/story/
    python3 -I tools/story_import/story_import.py --check    # validate what is on disk, exit 1 on any problem

Options: `--build-dir` (default `design/forgotten_third/build`), `--out-dir` (default `assets/story`).

Inputs: the Draft 55 script (`draft55/04*.md`, parsed by `scriptparse.py`; `python3 -I tools/story_import/scriptparse.py
<dir>` is its self-test), `draft55/VO_Master_List.csv`, `meta/meta_*.csv`, `meta/inserts_*.csv`, `flags.csv`,
`cues_final.csv`. Outputs: `story.json`, `scenes/SC-*.json` (97), `loc/en.json`. Output is byte-deterministic.

How it maps: dialogue lines become `line` beats (id from the VO list when scene + speaker + normalized text match,
else `TX-...`); italic lines and bold cards become `direction`; `inserts_*.csv` rows are placed before the first body
line starting with their anchor (curly quotes, `…` and whitespace folded; `@END` = end of scene): GAMEPLAY ->
`gameplay`, CHOICE -> `choice` (structure in the `CHOICES` table), HOLD -> `hold`, OPTIONAL/TIMED/VARIANT -> hidden
`direction`. Header `Mode`/`Gameplay` and the slate row fill the scene fields. See "Optional fields and importer
conventions" in the spec for the details and the extra optional fields.

`--check` rebuilds in memory and verifies the files on disk against the spec's rules (97 scenes, chapter order,
unique ids, VO rows matched once, every `loc` key present and unreferenced keys absent, inserts placed once, flag
and cue ids known, UTF-8/LF) and that they equal a fresh build. It prints a summary: scenes, beats by type, VO
matched/unmatched, inserts placed. To add a choice, edit `CHOICES` and regenerate.
