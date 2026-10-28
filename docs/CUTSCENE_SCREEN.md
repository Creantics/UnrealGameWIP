# Cutscene screen

The screen for CG steps (`GameplayScreen::Cutscene` in `ftcore::GameplayPlan`). It plays a shot list,
then hands control back to the story. It plays in one of two places:

- **In place:** on the field map the lead is already on, staged around the lead. The camera eases
  out of the gameplay view into the first shot and back into the gameplay view at the end. Play goes
  into the cutscene and out of it with no map load and no cut.
- **On the stage:** the `Cutscene` map, a blockout set with placeholder figures, for a step that
  can't play in place.

There's no art yet. When art arrives, the same shot lists play on the real sets. Only the maps and
the subject actors change.

## Where a CG step plays

`FTStep` (`FTStepHost.h`) picks the place for each step:

- **After a field step**, a CG step plays in place on that field's map. The field step's screen calls
  `FTStep::ContinueHere`, and the map's `AFTCutsceneDirector` is the host that plays it. When the
  cutscene ends, the beat's next step plays on the same map too, if a host there can play it.
- **When the story director opens it** (it starts a beat, or comes after a step that went back to
  the story map), `FTStep::Prepare` routes it:
  - In a scene placed in one of the big worlds (`Data/Field/worlds.json`), it loads that world's map
    when the map is built and no overworld hop has to come first. The lead stands at the district's
    spawn and the cutscene plays around the lead in place, so the field steps beside it play in place
    as well. Moving the lead to the spawn settles its camera boom there too, so the blend in starts
    from the gameplay view at the spawn instead of a camera still sweeping in from where the map
    started the lead. The story log says `Story: the CG at <scene> beat <b> step <s> plays on <map>,
    in district <d>.`
  - Otherwise it plays on the stage. An overworld hop that's due waits for the next field step.

A step is counted once, by `FTStep::FinishCarried`, wherever it plays.

## The stage map

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

The spots and subjects come from one table, `ftcore::cutsceneStageMark`. An in-place cutscene lays
the same table over the field map, so a shot list frames the same way in both places.

| Id | Position (m) | Use |
|---|---|---|
| `spot_a` | (9, 0, 2.5) | front, high: the establishing view |
| `spot_b` | (6, 5, 1.7) | front right |
| `spot_c` | (3, -5, 1.6) | right side of the pair |
| `spot_d` | (3, 6, 1.6) | left side of the pair |
| `spot_e` | (7, -1, 1.6) | front, low |
| `spot_f` | (4, 5, 1.4) | by the table |
| `subj_a` | (-1, -1.2) | blue, one of the pair; in place, the lead |
| `subj_b` | (-1, 1.2) | red, the other of the pair |
| `subj_c` | (2.5, 2) | green, by the table |

## On the stage

1. The story director fills `UFTGameInstance::StoryCarry` and opens the `Cutscene` map.
2. `AFTCutsceneGameMode::StartPlay` reads the carry, finds the map's `AFTCutsceneDirector` (or spawns
   one) and has it play the step. The director loads `Data/Cutscene/cutscenes.json` and looks up the
   shot list by the carry's `Scene`, `Beat` and `Step`. If that step isn't listed it uses `default`.
   If the file is missing or refused, it logs a warning and plays one wide shot of `subj_a` from
   `spot_a`.
3. The director's cine camera becomes the view target. The director finds the map's actors tagged
   with the shot ids once, when it starts. Every frame it advances `ftcore::CutscenePlayer` and places
   the camera with `ftcore::cutsceneCamera`.
4. The screen ends when the last shot ends, on a skip press, or with autoplay after 1 s of real
   time. It then sets `bDone` and `bWon` on the carry, logs one line and opens `ReturnMap`:

   ```
   Cutscene: done <scene> beat <b> step <s> shots <n> skipped <0|1>
   ```

   The step index is 0-based, the same as in the lookup key. The line carries ids and counts only,
   never text.

## In place

`AFTExploreDirector` spawns an `AFTCutsceneDirector` on every field map. That director registers as
the map's cutscene host (`HostSteps(true, true)`). When a CG step reaches it:

1. **Start.** It loads the shot list the same way as on the stage, and turns the lead's input off.
   It holds the party, so the followers stop taking up the trail. If the lead is mid-traversal
   (vault, climb, roll), it waits for that to end first.
2. **Staging.** The stage goes down around the lead (`ftcore::anchorForLead`). `subj_a`'s mark lands
   on the lead's feet, and the stage's front points along the lead's facing. The followers in view
   stand in for the other subject marks, nearest first. A mark no follower stands for is framed where
   it lies.
3. **Walks.** A follower whose mark some shot frames walks onto it while the camera blends in
   (`ftcore::MarkWalk`). It goes round the lead, brakes onto the mark and turns to face the lead. It
   walks only when:
   - the ground at the mark is within a metre of the lead's feet, and
   - nothing stands in the way at waist height.

   Otherwise it stays where it stands, and the shots frame it there.
4. **Blend in.** The view switches to the cine camera while it sits exactly on the gameplay camera.
   The camera then orbits round the lead from the gameplay view into the first shot, over the plan's
   `blend_in` time (0.8 s by default). The letterbox grows in with it.
5. **Shots.** The shots play as on the stage. A sphere sweep from the look point keeps the camera in
   front of walls.
6. **Blend out.** When the last shot ends, or on a skip, the camera eases back onto the gameplay
   camera over `blend_out` (0.8 s by default), and the letterbox shrinks away. A skip during the blend
   in eases out from wherever the camera got to. The view switches back once the camera is on the
   gameplay camera again.
7. **Hand back.** The lead gets input back. The party is let go and takes up the trail from where it
   stands. Then the beat's next step plays here when a host on this map can play it
   (`FTStep::ContinueHere`). Otherwise the story map opens, as on the stage.

Under autoplay there are no blends, so both view switches are cuts. Their size is logged: the cut in
is the first shot's distance from the gameplay camera (about 14 m on the committed shot lists), so a
cut of tens of metres means the gameplay camera wasn't at the lead. The log lines carry ids and
counts only:

```
Cutscene: done <scene> beat <b> step <s> shots <n> skipped <0|1> in place, pulled <k>, walked <w>
Cutscene: in place, view to the cine camera (<cm> cm, <deg> deg, fov <d>)
Cutscene: in place, view back to the lead (<cm> cm, <deg> deg, fov <d>)
Cutscene: in place, follower <k> walks <m> m onto <mark>
Cutscene: in place, follower <k> on its mark after <s> s
```

- `pulled`: the shots whose camera the sweep pulled in.
- `walked`: the followers set walking, whether or not they reached their marks.

## Controls

| | Keyboard | Gamepad |
|---|---|---|
| Skip | Space or Enter | A |
| Pause menu | Escape | Start |

A press counts only after 0.25 s of real time (`CutscenePlayer::kInputGrace`), so the press that
opened the screen can't skip it. The pause menu (see [GAME_FRAME.md](GAME_FRAME.md), "Pause")
stops the shot clock, and the real-time clock that the grace and the autoplay cap run on doesn't
count the time spent in it (`UFTPauseSubsystem::WallSeconds`).

## HUD

`FT::DrawCutsceneHUD` (`FTCutsceneHUD.h`) draws from the director on the canvas, with no UMG. On the
stage `AFTCutsceneHUD` calls it. In place `AFTExploreHUD` calls it instead of drawing the field's
own lines, from the step's start until it ends (`AFTCutsceneDirector::IsPlayingInPlace`).

- **Letterbox bars:** 2.39:1, and each bar is at least 8% of the screen height. In place they scale
  with `CutscenePlayer::presence`, so they grow in and shrink away with the blends.
- **Top bar, on the stage only:** the step's title from `ftcore::stubTitle` ("Cutscene" for CG) and
  "Step i of n".
- **Bottom bar:** "Shot i of n" on the stage only, and the skip hint while a skip still does
  something. In place the hint fades with the bars.

The bars appear only once the shots have started. All text is built once per step, when the step
starts. There's no story text and no step label.

## Autoplay and headless runs

- With `StoryCarry.bAutoPlay`, the cutscene skips itself after **1 s of real time**
  (`CutscenePlayer::kAutoPlayRealLimit`), whatever the game clock does. The stub uses 1 s of game
  time instead. This one is on real time so the alpha stays fast at any time dilation. In autoplay
  the log line normally says `skipped 1`. A shot list shorter than the cap ends by itself with
  `skipped 0`.
- With `-AlphaFast` the shot clock runs at the stub's `speedFactor`. The real-time cap is
  unaffected.
- **`-CutsceneBlendTest`** plays an autoplay step in place at full length, with its blends. Use it in
  an alpha run to watch the blends and the walks; the follower lines above show each walk and each
  arrival.
- **Opened directly** (no valid carry, or a carry that's already done): the stage plays the
  `default` list as a preview, then holds the last frame. There's no log line and no map change.

## Alpha counting

A cutscene step counts in the **stub** bucket: `AlphaCounters::addScreen` adds `Cutscene` to
`stub`. This keeps the shape of the director's `Alpha: FINISHED ... (b battle, f field, s stub)`
line, so the gate's expected line and the parsers don't change, and the totals stay the same as
when CG opened the stub card. Playing in place changes where a step plays, never how it counts.

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

With `"version": 2`, `default` and each entry may also set the in-place blend times:

| Key | Values | Default |
|---|---|---|
| `blend_in` | seconds easing from the gameplay camera into the first shot, 0..5 | 0.8 |
| `blend_out` | seconds easing from the last camera back to the gameplay camera, 0..5 | 0.8 |

A zero time cuts. On the stage both are ignored. A version 1 file gets the defaults, and there the
two keys are unknown keys.

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
4. Remember the list may play in place, with `subj_a` on the lead and the stage turned to the lead's
   facing. Followers stand in for `subj_b` and `subj_c`, and walk onto their marks only when a shot
   frames them.
5. Run the Core tests (`powershell -File tools/gate.ps1`). `test_cutscene.cpp` loads the committed
   file and checks that every id it uses is a stage mark. To add spots or subjects, add them to
   `ftcore::cutsceneStageMark` and rebuild the map.

Every CG step of the script has its own entry (3 to 8 shots, ids and numbers only); `default` is the fallback. `test_field_goals.cpp` checks the entries against the script's steps.

Never put text in the file. Lines, captions and names belong to the story data, which isn't
committed.

## Rules (Core)

`Core/include/ftcore/Cutscene.h` holds the following, with tests in `Core/tests/test_cutscene.cpp`:

- `CutsceneShot` and `CutscenePlan`: fixed capacity, with ids in fixed buffers. The plan carries the
  in-place blend times (`setBlends`, `blendIn`, `blendOut`).
- `CutscenePlayer`: `begin` (with `inPlace`), `update`, `tickReal`, `press`, `skip`,
  `shotProgress`, `blending` and `blendWeight`. In place it adds the `BlendIn` and `BlendOut`
  phases, with `blendInWeight`, `blendOutWeight` and `presence`.
- `cutsceneCamera`, `lerpPose` and `framingRule`.
- The stage: `cutsceneStageMark`, `findCutsceneStageMark` and `kCutsceneLeadMark`; `StageFrame`,
  `anchorForLead`, `stageToWorld` and `stageYawToWorld`.
- `MarkWalk`: a follower's walk onto its mark, round the lead, with the speed and stopping distance
  its animation needs (`wantSpeed`, `stopDistance`).
- `importCutsceneTable` and `CutsceneTable`.

Nothing allocates after load.
