# Cutscene screen

The screen for CG steps (`GameplayScreen::Cutscene` in `ftcore::GameplayPlan`). It plays a shot list
on a blockout set with placeholder figures, then hands control back to the story. There's no art
yet. When art arrives, the same shot lists play on the real set. Only the map and the subject
actors change.

## Regenerate the map

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildCutsceneWorld [-Map=Cutscene] -NullRHI -unattended -nopause -nosplash
```

Writes `/Game/Maps/Cutscene`, a 20 m blockout set:

- **Set:** a floor (top at z = 0), a back wall at x = -8 m, four pillars, a table and three boxes.
  Lit by a sun, a sky light and a lamp.
- **Camera spots `spot_a`..`spot_f`:** `ATargetPoint`s tagged with their ids.
- **Subjects `subj_a`..`subj_c`:** tinted capsule placeholders. Each is a 1.45 m cylinder body
  tagged with the id, with a sphere head attached (head centre at 1.6 m).
- **Director:** an `AFTCutsceneDirector` with its `ACineCameraActor`, plus a player start out of
  shot.

`WorldSettings.DefaultGameMode` is `AFTCutsceneGameMode`. An existing map file is deleted first, as
the stub builder does. The map is generated, never hand-made.

| Id | Position (m) | Use |
|---|---|---|
| `spot_a` | (9, 0, 2.5) | front, high: the establishing view |
| `spot_b` | (6, 5, 1.7) | front right |
| `spot_c` | (3, -5, 1.6) | right side of the pair |
| `spot_d` | (3, 6, 1.6) | left side of the pair |
| `spot_e` | (7, -1, 1.6) | front, low |
| `spot_f` | (4, 5, 1.4) | by the table |
| `subj_a` | (-1, -1.2) | blue, one of the pair |
| `subj_b` | (-1, 1.2) | red, the other of the pair |
| `subj_c` | (2.5, 2) | green, by the table |

## Flow

1. The story director fills `UFTGameInstance::StoryCarry` and opens `CutsceneMap` (`Cutscene`)
   for a CG step, because `ftcore::screenRoutesTo(Cutscene)` now returns `Cutscene`.
2. `AFTCutsceneGameMode::StartPlay` reads the carry, loads `Data/Cutscene/cutscenes.json` and
   looks up the shot list by the carry's `Scene`, `Beat` and `Step`. If that step isn't listed it
   uses `default`. If the file is missing or refused, it logs a warning and plays one wide shot of
   `subj_a` from `spot_a`.
3. It finds the map's `AFTCutsceneDirector` (or spawns one), calls `Play`, and makes the
   director's cine camera the view target.
4. Every frame the director advances `ftcore::CutscenePlayer` and places the camera with
   `ftcore::cutsceneCamera`.
5. The screen ends when the last shot ends, on a skip press, or with autoplay after 1 s of real
   time. It then sets `bDone` and `bWon` on the carry, logs one line and opens `ReturnMap`:

   ```
   Cutscene: done <scene> beat <b> step <s> shots <n> skipped <0|1>
   ```

   The step index is 0-based, the same as in the lookup key. The line carries ids and counts only,
   never text.

## Controls

| | Keyboard | Gamepad |
|---|---|---|
| Skip | Space or Enter | A |

A press counts only after 0.25 s of real time (`CutscenePlayer::kInputGrace`), so the press that
opened the screen can't skip it.

## HUD

`AFTCutsceneHUD` draws on the canvas, with no UMG:

- **Letterbox bars:** 2.39:1, and each bar is at least 8% of the screen height.
- **Top bar:** the step's title from `ftcore::stubTitle` ("Cutscene" for CG) and "Step i of n".
- **Bottom bar:** "Shot i of n" and the skip hint.

All text is built once in `StartPlay`. There's no story text and no step label.

## Autoplay and headless runs

- With `StoryCarry.bAutoPlay`, the cutscene skips itself after **1 s of real time**
  (`CutscenePlayer::kAutoPlayRealLimit`), whatever the game clock does. The stub uses 1 s of game
  time instead. This one is on real time so the alpha stays fast at any time dilation. In autoplay
  the log line normally says `skipped 1`. A shot list shorter than the cap ends by itself with
  `skipped 0`.
- With `-AlphaFast` the shot clock runs at the stub's `speedFactor`. The real-time cap is
  unaffected.
- **Opened directly** (no valid carry, or a carry that's already done): the screen plays the
  `default` list as a preview, then holds the last frame. There's no log line and no map change.

## Alpha counting

A cutscene step counts in the **stub** bucket: `AlphaCounters::addScreen` adds `Cutscene` to
`stub`. This keeps the shape of the director's `Alpha: FINISHED ... (b battle, f field, s stub)`
line, so the gate's expected line and the parsers don't change, and the totals stay the same as
when CG opened the stub card.

## Data: `Data/Cutscene/cutscenes.json`

The file holds ids and numbers only, no text, so it's committed. Units are metres, with z up. The
Unreal side multiplies by 100.

```json
{
  "version": 1,
  "default": { "shots": [
    { "spot": "spot_a", "framing": "wide", "duration": 3, "subject": "subj_a", "move": "push_in", "amount": 1.5 },
    { "spot": "spot_b", "framing": "two_shot", "duration": 2.5, "subject": "subj_a", "subject2": "subj_b",
      "transition": "blend", "blend": 0.8 }
  ] },
  "cutscenes": [
    { "scene": "SC-X", "beat": 4, "step": 1, "shots": [ ... ] }
  ]
}
```

Each entry is keyed by `scene`, `beat` and `step` (the gameplay beat index and the plan step
index), the same way as `Data/Battle/encounters.json`. `step` defaults to 0.

| Shot key | Values | Default |
|---|---|---|
| `spot` | camera spot id (required) | — |
| `subject` | subject id (required) | — |
| `subject2` | second subject id; required by `two_shot` and `over_shoulder` | none |
| `framing` | `wide`, `medium`, `close`, `over_shoulder`, `two_shot`, `insert` | `medium` |
| `duration` | seconds, 0.1..60 | 2 |
| `move` | `static`, `push_in`, `pull_out`, `pan`, `orbit` | `static` |
| `amount` | metres for `push_in` and `pull_out` (0..20); degrees for `pan` and `orbit` (-360..360) | 0.5 m, 20°, 30° |
| `transition` | `cut`, `blend` | `cut` |
| `blend` | seconds at the start of this shot spent blending from the previous shot's last camera, 0..duration | 0.5 |

Ids are lower-case letters, digits and `_`, at most 15 characters. A list holds at most 16 shots
(`CutscenePlan::kMaxShots`). Each of these is an error, and the file is refused:

- a bad id
- a value out of range
- an empty or over-long list
- a duplicate (scene, beat, step)
- `two_shot` or `over_shoulder` without `subject2`

Unknown keys are warnings.

### Framing and moves

The rules are in `ftcore::cutsceneCamera`, with the constants in `ftcore::framingRule`. A subject's
position is the point it stands on: the bottom centre of its actor's bounds. A spot is the
actor's location.

| Framing | Camera | Looks at (above the feet) | Lens |
|---|---|---|---|
| `wide` | at the spot itself | 1.0 m | 24 mm |
| `medium` | 3 m from the subject, toward the spot (seen from above), 1.55 m high | 1.45 m | 35 mm |
| `close` | 1.3 m, 1.62 m high | 1.6 m | 50 mm |
| `insert` | 0.8 m, 1.3 m high (looks down at a detail) | 1.0 m | 50 mm |
| `two_shot` | back from the middle of both subjects, far enough to hold both (at least 3 m, or 1.1 × their spacing + 1.5 m) | 1.45 m | 30 mm |
| `over_shoulder` | 0.8 m behind `subject2` and 0.45 m to the side, 1.65 m high; the spot picks the shoulder | 1.6 m on `subject` | 40 mm |

Without `subject2`, `two_shot` and `over_shoulder` fall back to `medium`. The loader refuses them
anyway. The lenses are set by the director (`CutsceneFocalLength`), and depth of field is off.

Moves follow the shot's progress, eased:

- `push_in`: walks toward the look point by `amount`, stopping 0.3 m short.
- `pull_out`: walks away by `amount`.
- `pan`: turns the look point about the camera from -amount/2 to +amount/2.
- `orbit`: turns the camera about the look point by `amount`.

A `cut` jumps to the new camera. A `blend` lerps from the previous shot's camera at its end, with
an eased weight. The lens blends too.

### Authoring a shot list

1. Find the step's key. The scene id and the gameplay beat index come from the story director's
   log (`Story: step i/n ... at <scene> beat <b>`). The step is `i - 1`.
2. Add an entry under `cutscenes` with those three numbers and a `shots` list. Pick spots and
   subjects by id from the map table above.
3. Start with a `wide` from a high spot, then cover the exchange: `two_shot`, then
   `over_shoulder` back and forth (swap `subject` and `subject2`, and use spots on the same side so
   the line isn't crossed). Use `close` or `insert` for a detail, and a `blend` for a slow change of
   view.
4. Run the Core tests (`powershell -File tools/gate.ps1`). `test_cutscene.cpp` loads the committed
   file and checks that every id it uses exists in the blockout map. If you add spots or subjects
   to the map, extend that test.

Every CG step of the script has its own entry (3 to 8 shots, ids and numbers only); `default` is the fallback. `test_field_goals.cpp` checks the entries against the script's steps.

Never put text in the file. Lines, captions and names belong to the story data, which isn't
committed.

## Rules (Core)

`Core/include/ftcore/Cutscene.h` holds the following, with tests in `Core/tests/test_cutscene.cpp`:

- `CutsceneShot` and `CutscenePlan`: fixed capacity, with ids in fixed buffers.
- `CutscenePlayer`: `update`, `tickReal`, `press`, `skip`, `shotProgress`, `blending` and
  `blendWeight`.
- `cutsceneCamera`, `lerpPose` and `framingRule`.
- `importCutsceneTable` and `CutsceneTable`.

Nothing allocates after load.
