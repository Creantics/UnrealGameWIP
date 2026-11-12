# story_import

Builds the runtime story JSON (`docs/story_format.md`) from a story design package.

    python3 -I tools/story_import/story_import.py --tables FILE            # regenerate assets/story/
    python3 -I tools/story_import/story_import.py --tables FILE --check    # validate what is on disk, exit 1 on any problem

`--tables` (required) names the story's own tables, a JSON file kept outside the repo: the title, the worlds, the
scene count, the script folder and scene id prefix, the hold ladder and its anchors, the gameplay keywords, and the
hand-authored choices and exchanges. Its keys are listed in the comment above `load_tables()`. The tool holds no
story data.

Options: `--build-dir` (default `design/forgotten_third/build`), `--out-dir` (default `assets/story`).

Inputs: the script (`<script_dir>/04*.md`, parsed by `scriptparse.py`; `python3 -I tools/story_import/scriptparse.py
<dir> [expected scenes]` is its self-test), `<script_dir>/VO_Master_List.csv`, `meta/meta_*.csv`, `meta/inserts_*.csv`,
`flags.csv`, `cues_final.csv`. Outputs: `story.json`, `scenes/<scene id>.json`, `loc/en.json`. Output is
byte-deterministic.

How it maps: dialogue lines become `line` beats (id from the VO list when scene + speaker + normalized text match,
else `TX-...`); italic lines and bold cards become `direction`; `inserts_*.csv` rows are placed before the first body
line starting with their anchor (curly quotes, `…` and whitespace folded; `@END` = end of scene): GAMEPLAY ->
`gameplay`, CHOICE -> `choice` (structure in the tables' `choices`), HOLD -> `hold`, OPTIONAL/TIMED/VARIANT -> hidden
`direction`. Header `Mode`/`Gameplay` and the slate row fill the scene fields. See "Optional fields and importer
conventions" in the spec for the details and the extra optional fields.

`--check` rebuilds in memory and verifies the files on disk against the spec's rules (the tables' scene count,
chapter order, unique ids, VO rows matched once, every `loc` key present and unreferenced keys absent, inserts placed
once, flag and cue ids known, UTF-8/LF) and that they equal a fresh build. It prints a summary: scenes, beats by type,
VO matched/unmatched, inserts placed. To add a choice, edit the tables' `choices` and regenerate.
