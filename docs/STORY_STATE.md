# Story state

`Core/include/ftcore/StoryState.h`, `Core/src/StoryState.cpp`, data in `Data/Story/`.

The facts a playthrough remembers, kept as one plain value (`StoryState`, under 4 KB) and saved as one
fixed 2048-byte block. Facts are bytes in numbered slots; counters are u32 in numbered "wide" slots.
Ids are neutral (`p03_1`, `104_1`, `c_milestone`); what they mean lives outside this repo.

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
facts or 8 wide values. The registry hash is FNV-1a over the ids in slot order.

`Data/Story/story_tuning.json`: the track length and scene, and the counter milestones (a scene
and a value; at that scene's commit the count rises to the value).

## Use

- `storyBeginScene` / `storyCommitScene` / `storyRestartScene`: one open scene, strictly after the last
  committed. A restart restores facts, stamps and counters (not RAW) and bumps `c_retries`. A commit
  closes unanswered gated choices as 255 (not offered) and optional acts as 1 or 255, then applies the
  milestones and the track mark (side 1's presses in the track scene, capped at the track length).
- `storySet` checks in order: unknown, no scene, wrong scene, range, gate, unchanged, once, mono.
  Every refusal is a `Rejected` event (`id` with bit 15 for wide slots).
- `storyGet` reads Unset (the default), Set, NotOffered, Stale (out of range: the default) or Unknown.
- `storyCountHelp(side, kind)` counts one third-slot press (per kind, per scene, raw total) and records
  the last kind.
- `storySceneReads(scene)`: every fact and wide value the scene reads plus derived values D1..D7.
  Call at scene start; it does not allocate.
- `storyValidate`: rules V01..V20; reports, never repairs; at most 32 issues. V16 checks only that a
  scene counter is zero before its scene; the order it was counted in is not in the state.
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
empty, too short, bad magic, a newer layout, a wrong size, a bad checksum, an open or last committed
scene that is neither a scene nor 255.

## Tests

`Core/tests/test_story_state.cpp`, including a walk through every act 1 branch (196,608 paths,
about 5 s in Debug) that validates each end state, its reads and derived values, and round-trips the
block.
