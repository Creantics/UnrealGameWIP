# Story state

`Core/include/ftcore/StoryState.h`, `Core/src/StoryState.cpp`, data in `Data/Story/`.

The facts a playthrough remembers, kept as one plain value (`StoryState`, under 4 KB) and saved as one
fixed 2048-byte block. Facts are bytes in numbered slots; counters are u32 in numbered "wide" slots.
This repo holds the machine only. Which slots exist, what they are called, which scenes there are
and which rules bind to which ids all come from the registry in `Data/Story`, which is gitignored and
never committed. Generic rule kinds are code; their bindings are data.

## The registry: `Data/Story/story_facts.csv`

```
slot,id,kind,max,default,mut,set_scenes,gate,read_scenes,fallback,derive
```

| Column | Meaning |
|---|---|
| slot | fact 1..255, wide 0..63; never reused |
| id | `[a-z0-9_]{1,15}`, unique |
| kind | EV (1), OP (1 passed, 2 done), C2/C3/C4 (1..n = A..), NUM / ST (0..max), BITS (mask), PK (2-bit fields from bit 0, 3 illegal, a leftover bit is a flag), `retired`; wide: CTR, RAW (kept across a restart), MASK |
| default | what a stored 0 reads as (NUM, ST) |
| mut | `once`, `mono` (never down; bits never cleared; packed per field), `free` |
| set_scenes | `any` or scene codes; a `once` fact has one |
| gate | `id=V` or `id.fN=V` (whole fact), `X:id=V` (option X only); V is A..D or a number |
| read_scenes | `all`, codes, or `X+` (X onward); never before the first set scene |
| fallback | `all` or codes: OP and choices must mark every read scene |
| derive | `V:id=V`: an older save past the set scene gets V on load when the condition holds |

Import collects every error as `story_facts.csv line N: ...` and leaves the registry empty on failure.
It also checks a gate's fact is set no later than the gated fact and that no scene reads more than 24
facts or 8 wide values. The registry hash is FNV-1a over the scene codes, then the fact and wide ids in slot order. A UTF-8 byte order
mark before the header is skipped.

### `Data/Story/story_tuning.json`

Required with the csv: one without the other is an import error. Every field but `version` and
`scenes` is optional; an unbound role switches its behaviour off.

| Key | Meaning |
|---|---|
| `version` | 2 |
| `scenes` | the scene codes in play order (`[a-z0-9_]{1,7}`, at most 128); a scene's index is its ordinal |
| `roles` | ids bound to built-in jobs: `pressCounters` (2 sides x 3 kinds, CTR with max at most 65535), `pressScene`, `pressTotal`, `retries`, `milestone`, `trackMark` (wide ids); `lastKind` (2 NUM facts, max at least 3), `firstPress`, `sideOpen` (2 facts, `""` for none); no slot may serve two roles |
| `pressFloor` | 0..65535 (default 0): `storyPressTotal` counts each side as at least this |
| `track` | `length`, `scene`, `side`: the presses on that side in that scene, capped at the length, go to `trackMark` |
| `milestones` | `scene`, `value`, optional `needs` (a fact): at the scene's commit the milestone counter rises to the value; scenes and values must rise |
| `derived` | up to 16 values computed for a scene's reads: `value` (a fact's value), `read` (its stored byte), `in` (1 when the fact's value is in `values`), `upgrade` (`from` becomes `to` while `if` holds), `topKind`, `pressTotal` |
| `rules` | data-bound checks with their own issue number: `needs` (`when` holds → `then` holds; `settled` checks only after the when-fact's first set scene is committed), `bitsInOrder` (a BITS fact's bits set from bit 0 up) |

A condition is `id` (set), `id=V`, `id.fN=V` (a packed field) or `id&M` (any of the bits).

## Use

- `storyBeginScene` / `storyCommitScene` / `storyRestartScene`: one open scene, strictly after the last
  committed. A restart restores facts, stamps and counters (not RAW) and bumps the `retries` counter. A commit
  closes unanswered gated choices as 255 (not offered) and optional acts as 1 or 255, then applies the
  milestones and the track mark.
- `storySet` checks in order: unknown, no scene, wrong scene, range, gate, unchanged, once, mono.
  Every refusal is a `Rejected` event (`id` with bit 15 for wide slots).
- `storyGet` reads Unset (the default), Set, NotOffered, Stale (out of range: the default) or Unknown.
- `storyCountPress(side, kind)` counts one press (per side and kind, per scene, raw total), records
  the last kind and latches the first press; a side with an unset `sideOpen` fact refuses it.
- `storySceneReads(scene)`: every fact and wide value the scene reads plus the derived values.
  Call at scene start; it does not allocate.
- `storyValidate`: the built-in rules plus the registry's rules; reports, never repairs; at most 32
  issues. V16 checks only that a scene counter is zero before its scene; the order it was counted in is not in the state.
- `storyMakeSandbox`: a scene-select copy that can never be encoded. Once facts written at the chosen
  scene or later read as unset; facts written in several scenes and the counters keep their values.

## The block (2048 bytes, little endian)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `FTSS` |
| 4 | 2 | layout version (1) |
| 6 | 2 | block size (2048) |
| 8 | 2 | facts known |
| 10 | 2 | wide known |
| 12 | 4 | registry hash |
| 16 | 1 | open scene (255 none) |
| 17 | 1 | last committed (255 none) |
| 18 | 2 | flags (bit 0: written mid-scene) |
| 20 | 4 | save serial |
| 24 | 4 | reserved |
| 28 | 4 | CRC-32 of bytes 0..27 and 32..2047 |
| 32 | 256 | facts |
| 288 | 256 | facts at scene entry |
| 544 | 256 | stamps (scene that last wrote each slot) |
| 800 | 256 | wide (64 x u32) |
| 1056 | 256 | wide at scene entry |
| 1312 | 736 | extension: records `tag u16, len u16, bytes`, ended by tag 0 |

Tag 1 holds the stamps at scene entry (written only mid-scene). Unknown tags, slots past this build's
registry, and a malformed extension (kept raw) are written back unchanged. Decode refuses, in order:
empty, too short, bad magic, a version of 0, a newer layout, a wrong size, a bad checksum, an open or last committed
scene or a stamp that is neither a scene nor 255. A mid-scene save with no room left in the extension
for tag 1 is still written, without it, and counts in `StoryState::lostStampRecords`: that save
restarts the open scene with the stamps as saved.

Decode may fill part of its output before a late refusal, so a loader decodes into a temporary.
`storyLoadOrNew` does that: the decoded state on Ok, a new game's state (`storyReset`) on any other
result, never half of one. `storyLoadName` names a result for logs.

## Tests

`Core/tests/test_story_state.cpp`. Every public test runs on a small registry built in the test and
never skips: import errors, set and get, gates and the commit, presses and milestones, restart, the
sandbox (the real state stays byte-identical), the block, a newer writer, no registry, derive on
load, every validation rule, no heap use, and a walk through every branch of a small walk script.
The walker validates each end, checks each scene's reads against a model, saves mid-scene, loads,
restarts and replays, round-trips the block, and is itself checked to fail on wrong expectations.

When `Data/Story` is present, its registry must import cleanly and its walk file
(`story_walk.txt`, same format, documented in the test) is walked too; a csv without the json fails.
A clean clone has no `Data/Story`, so it runs fewer story tests: those cases show as skipped, not failed.

## In Unreal

`UFTStoryStateSubsystem` (a game-instance subsystem) owns one state and the registry. At start it imports
the registry from the story folder (`StoryDir`, default `Data/Story`); with both files absent it runs on an empty registry, and with one file
missing or a refused import it warns and does the same. It begins, commits and restarts scenes, starts a
new game, and hands the state and registry to the fight session (`BindFightSession`, after every Begin).
The story save game keeps the block beside the story text (`StoryBlock`): captured with each save,
loaded after the text with `storyLoadOrNew`, so a save from before the block loads as a new game's state.
`-StoryStateCheck` runs headless: a block round trip and a new game, one `StoryState check:` log line,
then the game exits; it never writes the save slot.
