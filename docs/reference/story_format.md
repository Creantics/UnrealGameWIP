# Story data format

The story runtime (`ne/story`) plays the Forgotten Third script from JSON that `tools/story_import` generates
from the design package. This file is the contract between the two. Version: `"format": 1`.

Sources the importer reads (all in `design/forgotten_third/build/`):

- `draft55/04a..04e_Script_*.md`: the script (Draft 55 is the applied copy; Draft 54 stays locked).
- `meta/meta_*.csv`: one slate row per scene (world, location, mode, cast, props, cues).
- `meta/inserts_*.csv`: `[GAMEPLAY → …]` and `[CHOICE: …]` rows placed before an anchor line.
- `flags.csv`, `cues_final.csv`, `draft55/VO_Master_List.csv`.

## Files

```
assets/story/story.json            index: chapters, scene order, start scene
assets/story/scenes/SC-XX-YY.json  one file per scene (97)
assets/story/loc/en.json           every player-visible string, keyed (see Localization)
```

### story.json

```json
{
  "format": 1,
  "title": "The Forgotten Third",
  "start": "SC-P0-01",
  "chapters": [
    { "id": "P0", "title": "Prologue · ...", "scenes": ["SC-P0-01"] },
    { "id": "01", "title": "Chapter 1 · ...", "scenes": ["SC-01-U1", "SC-01-D1"] }
  ],
  "flags": [ { "id": "F_SQ_STOPS", "default": false } ],
  "cues":  [ { "id": "MUS-KNK", "kind": "music" } ]
}
```

`chapters[].scenes` is play order. The scene that follows the last beat of a scene is the next id in this
list unless the scene's `end` beat names one.

### SC-*.json

```json
{
  "id": "SC-07-Q1",
  "title": "Strangers",
  "chapter": "07",
  "world": "Solhaven",
  "location": "Lanternfall alley (the seam)",
  "time": "dusk",
  "mode": "PLAY",
  "cast": ["DEVIN", "DUY"],
  "props": ["wheelbarrow"],
  "cues": ["SFX-KNOCK"],
  "beats": [ ... ]
}
```

`world` is one of: Solhaven, Lattice, Unwritten, Ink-Solhaven, Cleft pocket, House, Restored, Interstice.

## Beats

Every beat has `"type"`. Beats play in order.

| type | fields | meaning |
|---|---|---|
| `line` | `id`, `speaker`, `direction`?, `text`, `loc` | A spoken line. `speaker` is the script's label (`DUY`, `CHORUS`); `direction` is the delivery tag without parentheses. |
| `direction` | `text`, `loc` | A stage direction (shown as narration in the dialogue box, italic). |
| `choice` | `label`, `loc`, `scripted`, `options[]` | A player choice. `options[]`: `{ "text", "loc", "effects": [flag ops], "next": beat index or null, "miss"? }`. `scripted: true` means a single outcome (the CHC rows): show the prompt, one option. |
| `gameplay` | `kind`, `text`, `loc`, `params` | Hands control to gameplay. `kind`: `CMB` (battle), `ENG` (explore / talk / puzzle), `PLAY` (free exploration), `MINI` (legacy: the importer no longer emits it, see below). `text` is the `[GAMEPLAY → …]` notation; `params` is free-form (`{"enemies": "...", "minutes": 6}`). The story resumes at the next beat when the gameplay reports done. A kind the game has no screen for (a leftover `MINI`, anything unknown) waits in the dialogue box and continues on confirm. |
| `cue` | `id`, `action` | `action`: `play`, `stop`, `stinger`. Audio may be missing; a cue the runtime cannot play is ignored. |
| `flag` | `op`, `id`?, `value`? | `op`: `set`, `clear`, `require`, `end`. A `require` that fails skips past the next `end` flag beat of the scene (blocks do not nest; `end` may carry the `id` it closes, for readability). A `require` with a boolean `value` tests truthiness (a counter ≥ 1 is `true`); any other value must match exactly. `clear` returns a counter or enum flag to its default and a boolean to `false`. |
| `hold` | `seconds` | A silence: no text advance for this long. Lengths come from the hold ladder (see `rulings_advisor_draft55.md` §D): first Tells 1.5–2.0 s, 06-U2 4.0, 11-Q3 3.0, 14-Q3/15-Q1 5.0, 22-Q1 8.0, EP-03 12.0. |
| `end` | `next`? | Scene end. `next` overrides the chapter order. |

## Ids and localization

- **Line ids are VO ids.** A `line` takes its id from `VO_Master_List.csv` (`VO-07-Q1-012`) wherever the
  script line matches a VO row (same scene, speaker, text). Lines with no VO row get `TX-<scene>-<nnn>`.
  Every id is unique across the story. Audio hooks in later by id.
- **Every player-visible string has a loc key**, written into `assets/story/loc/en.json` and referenced by the
  beat's `loc` field: `story.<scene>.<id>` for lines (`story.SC-07-Q1.VO-07-Q1-012`), `story.<scene>.d<nnn>`
  for directions, `story.<scene>.c<nnn>` / `.c<nnn>.o<k>` for choices, `story.<scene>.g<nnn>` for gameplay
  prompts, `story.<scene>.title` for titles.
- **Placeholder lines are `PH-` ids.** Lines invented for the dialogue exchanges (below), which have no script
  line and no VO row, get `PH-<scene without SC->-<nnn>` (`PH-18-Q1-004`), numbered per scene in order, with loc
  `story.<scene>.PH-...`; their choices and directions get `story.<scene>.pc<nnn>` / `.pc<nnn>.o<k>` and
  `story.<scene>.pd<nnn>`, so the script's own `c` / `d` keys do not move. A `PH-` line is a placeholder for the
  writers: it is replaced by a script line (and then gets a VO id) or rewritten in place. The `text` field duplicates the English for tools and tests;
  the runtime displays the string table entry. Six text-only languages follow later
  (FR, IT, DE, ES, JA, zh-Hans).
- Text keeps the script's markdown emphasis (`*word*`). The runtime may strip it or render it.

## Rules the importer checks

the draft's scene count (73 for Draft 93.1, 97 for Drafts 55 and 63); every scene in a chapter; every `CHOICE` / `GAMEPLAY` insert placed exactly once at its anchor;
every id unique; every VO row of the script's scenes matched to exactly one line (report misses); every
`loc` key present in `en.json`; JSON is UTF-8, LF, deterministic (sorted where order is free).

## Optional fields and importer conventions

These extend format 1 without changing it; a runtime that ignores them still reads every required field.

- Scene `gameplay` (string): the script's `**Gameplay:**` header text. Scene `mode` is the raw `**Mode:**` string
  (`ENG`, `CMB → ENG`, ...), not one of the four gameplay kinds.
- `direction.hidden` (bool) with `direction.note` (`OPTIONAL`, `TIMED`, `VARIANT`) and optional `direction.source`:
  a designer notation from `inserts_*.csv` (or the script's in-text `***VARIANT.**` note), kept for tools. A hidden
  direction is **never displayed**, has no `loc` and is not in `en.json`; the runtime skips it.
- `hold.note` (string): why the hold exists (`Tell`, or the `[HOLD: ...]` notation of an input hold).
- `choice.options[].selectable` (bool, default true): `false` for the authored "greyed out" options (SC-12-Q2).
- `choice.options[].miss` (bool, default false): a wrong pick in a dialogue exchange. The option moves on to the
  companion's correction (its `next`, usually `null`) and the exchange asks again further down; it never moves
  the story past the exchange. Runtimes treat it like an option that returns to its own prompt: it is never the
  automatic player's default (`StoryRunner::optionReturns`, `defaultOption`). A choice whose every option is a
  miss is an importer error.
- A `choice` option's `next` of `null` continues with the following beat; SC-25-Q4's refused answers name the
  choice's own beat index (the prompt returns).
- Flag values: `flag.value` and an option effect's `value` are `true`, `false`, an integer or a string (enum flags
  such as `F_CH_GRANITE_SIT` = `SAT` | `WALKED_ON`); option effects are `set` / `clear` only. `story.json` flag
  `default` is `false`, an integer (counters) or the string from `flags.csv` (`UNSET`, `NOT_STARTED`).
  Auto-resolving `UNSET` is the runtime's job. A flag beat can only set a value, so a counter is raised with a
  descending `require` chain (`require 2, set 3, end; require 1, set 2, end; require 0, set 1, end`: SC-13-HE1's
  `F_HE_LANTERN_GUARD_SCORE`).
- Chapter ids are zero-padded (`01`..`25`, `P0`..`P3`, `EP`). Chapter titles read `Chapter N · Title`,
  `Prologue · Title`, `Epilogue · Title`.
- `TX-` ids use the scene id without `SC-` and the line's ordinal among the scene's spoken lines
  (`TX-21-Q2-002`), matching how `VO-` ids are written.
- Holds: `[HOLD: ...]` inserts become a `hold` (plus a `flag` set beat when the notation says `Sets F_X`);
  ladder scenes use the ladder length, other input holds 2.0 s. Every scene whose slate says
  `tell_insert=yes` (and has no `[HOLD]` insert) gets one `hold` (note `Tell`, 1.75 s, ladder length in the ladder
  scenes) after its first line (direction, or dialogue incl. delivery tag) with a tap and a hold in one sentence,
  else the first tap, else the first hover/hesitation. The slate
  `sound_cues` go in the scene's `cues` list only; no `cue` beats are generated.
- `[CHOICE: ...]` rows are structured by hand in `tools/story_import/story_import.py` (`CHOICES`); SC-21-Q3's
  Pledges row yields two choice beats (Devin, Duy). An in-text `***CHOICE ...**` / `***VARIANT.**` note that has an
  insert on it is merged with that insert.
- **Dialogue exchanges (the former `MINI` rows).** The seven `[GAMEPLAY → MINI ...]` inserts (SC-09-HE1,
  SC-10-HE1, SC-13-HE1, SC-15-HE1, SC-15-HE2, SC-18-Q1, SC-21-HE1) are not mini-games any more: each is re-authored
  by hand in `story_import.py` (`MINI`) as `PH-` lines, shown directions, holds, flag beats and 2–4 successive
  choices (at most 4 options each) inside the scene; the player never leaves the dialogue box. The insert's
  notation stays at its anchor as a hidden direction with note `MINI`, and the scene's `**Gameplay:**` designer
  directions (`*Gameplay: ...*`) become hidden directions with note `MINI` too. An exchange may be split into parts
  placed before given script lines (SC-18-Q1, SC-21-HE1). Each question is a linear ladder, so it cannot loop:
  ask (wrong: correction A) → ask again (wrong: correction B) → ask a third time (wrong: the companion supplies the
  answer) → the right pick, or the supplied answer, joins the concede lines. Every flag the mini-games wrote is
  still written, with its type. See `story_mini_dialogue.md` for each scene.
- **Saves.** A save keeps the story position as (scene id, beat index). The exchanges add beats to the seven
  scenes above, so a save made inside one of them before this change resumes at a shifted beat; the scenes are
  short and the runtime clamps a past-the-end index to the next scene, but a save there should be treated as
  invalid (start the scene again). Saves in every other scene are unaffected.
