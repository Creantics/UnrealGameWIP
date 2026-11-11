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

Decode builds the state aside and writes its output only on Ok: every refusal, a late one in the
extension included, leaves the output as it was. `storyLoadOrNew` is the loader's call: the decoded
state on Ok, a new game's state (`storyReset`) on any other result, never the state from before. `storyLoadName` names a result for logs.

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
missing or a refused import it warns and does the same. A later import (a story reload) that finds no registry
or is refused keeps the registry and state it had. It begins, commits and restarts scenes, starts a
new game, and hands the state and registry to the fight session (`BindFightSession`, after every Begin).
The story save game keeps the block beside the story text (`StoryBlock`): captured with each save,
loaded after the text with `storyLoadOrNew`, so a save from before the block loads as a new game's state.
`-StoryStateCheck` runs headless: a block round trip and a new game, one `StoryState check:` log line,
then the game exits; it never writes the save slot.

`UFTGameInstance::ReloadStory` imports the registry again after the story reloads (a failed reload keeps
both), so the two never part. A registry import resets the state to a new game's: its slots and scenes
belong to the registry it was written against.

### The scene flow and the facts (`ftcore::StoryBridge`)

The bridge (`Core/include/ftcore/StoryBridge.h`, tests in `Core/tests/test_story_bridge.cpp`) ties the story
runner to the state. The state is the truth; the runner's flags named `sf.<fact id>` are a view of it.

- **Scenes.** A runner scene plays the registry scene whose code is its id (identity). A private
  `story_scene_map.csv` beside the registry links other ids instead (`importStorySceneMap`):

  ```
  scene,code
  r-one,s1
  r-two,s2
  ```

  Blank lines and `#` lines are skipped; a missing header, a line without two fields, an empty field, a code
  the registry does not list or an id given twice refuses the whole file (the bridge then uses identity).
  Entering a mapped scene commits the open one and begins its own (`storyEnterScene`); runner scenes in a
  row that play one registry scene keep it open; an unmapped scene leaves the state as it is; `start()`
  resets the state; the story's end commits.
- **Facts into flags.** When a scene is entered or restarted, each `sf.` flag whose fact the scene reads or
  sets takes the fact's value (a bool flag: the fact is set; an int flag: its value, 255 when never
  offered). Every other `sf.` flag goes back to its initial value, so a scene branches only on what it reads.
- **Flags into facts.** When the story sets an `sf.` flag (a flag beat, an option effect, a gameplay
  result), the bridge calls `storySet` and writes the fact's value back to the flag, so a refused set
  (once-only, wrong scene, out of range, gate) leaves the flag as the fact is; the refusal is logged.
- **Gameplay results.** A gameplay beat's `sets` param names the flag its result goes to:
  `StoryRunner::resume(value)` (Unreal: `ResumeAfterGameplayWithValue`) sets it, then continues. A plain
  `resume()` sets the beat's `stub` param (an integer) when it has one, a stand-in for a screen not built
  yet, so autoplay walks through. A bool flag takes a non-zero value as true.
- **Restart.** `StoryRunner::restartScene()` (Unreal: `RestartCurrentScene`) goes back to the scene's first
  beat with the runner's flags as they were when it was entered, and restarts the registry scene
  (`storyRestartScene`) back to the facts as that runner scene began: a second runner scene of one registry
  scene keeps what the first one set.
- **Saves.** The bridge enters the state's scene inside the runner, before the subsystem reports the scene
  change, so the autosave on a scene change holds the new scene open, with its first beats up to the first
  stop played: runner and block are taken at one moment.
- **A load.** The save game restores the runner (the bridge pauses: nothing the runner does while it settles
  reaches the state), decodes the block, then calls `SyncAfterLoad`. The runner stays where it was
  restored and the state as decoded. When the block has another registry scene open, or none, the
  runner's scene begins; at the story's end the open scene commits. The flags are pushed, and a restart
  after the load goes back to the load point for the runner and the state alike.
- **Binding.** A story with fact flags none of whose scenes plays a registry scene is refused: every write
  would be.

`UFTStoryStateSubsystem` binds the bridge again after every story load and registry import. A story with an
`sf.` flag the registry lacks, a string `sf.` flag, or fact flags with no mapped scene leave it unbound with
a warning. A registry import that fails keeps the current registry, state and scene map. The save keeps the
last good block: when the state refuses to encode, that block is written instead. A fight retry resets
the fight, not the scene, so it calls nothing here.
