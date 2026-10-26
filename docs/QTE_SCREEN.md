# Quick-time event screen

The screen that QTE and `co-op QTE` gameplay steps open (`GameplayScreen::Qte` in `ftcore::GameplayPlan`). It is a short timed input sequence outside combat: rings close in on prompts, the player presses the matching direction as each ring lands, and a one-line result shows before the story continues. It borrows the battle's dodge and parry timing (see the Defence section of [BATTLE_DEMO.md](BATTLE_DEMO.md)) and has no fail state.

## Regenerate the map

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildQteWorld [-Map=Qte] -NullRHI -unattended -nopause -nosplash
```

Writes `/Game/Maps/Qte`: the stub screen's small lit room (floor, back wall, sun, sky light, one lamp), a camera and a player start, with `AFTQteGameMode` as the default game mode. An existing map file is deleted first. The map is generated, never hand-made. The story director opens it for every QTE and co-op QTE step (`QteMap`, default `Qte`), so it must exist.

## Flow

1. The director fills `UFTGameInstance::StoryCarry` (as for every gameplay screen) and opens `/Game/Maps/Qte`.
2. `AFTQteGameMode::StartPlay` reads the carry, loads `Data/Qte/qte.json`, picks the sequence for the step's scene, beat and step (the table's `default` when it isn't listed, the built-in sequence when the file is missing or refused) and starts `ftcore::QteScreen`. A step whose token is `co-op QTE` plays as co-op.
3. Each frame it polls the keys, feeds the presses and releases to the screen, then ticks it. `AFTQteHUD` draws the prompts.
4. When every prompt is resolved the result shows for about one second (Perfect, Good, Fair or Poor). Then it sets `bDone` and `bWon`, logs one line and opens `ReturnMap`:

```
QTE: done <scene> beat <b> step <s> prompts <n> perfect <p> good <g> miss <m>
```

The line has ids and counts only. Opened without a valid carry, the screen says "No story step" and nothing else happens.

## Prompts

| Kind | Input | Grade |
|---|---|---|
| Tap | one direction | Pressed before its time `at`. Wide window: the last 0.06 s is Perfect, the rest of 0.30 s is Good. Narrow window: the last 0.12 s is Perfect, the rest of the wide window is Good |
| Hold | one direction, then keep it down for `hold` s | Pressed like a tap; let go more than 0.15 s before the end and it is a Miss, otherwise it keeps the tap grade |
| Mash | one direction, `mash` presses inside `limit` s from `at` | Perfect if done within 60% of the limit, Good if later, Miss if short |

A press of another direction is ignored. The window widths are `DefenceTimeline`'s dodge and parry widths: each tap or hold is judged by a one-hit `DefenceTimeline` (a Wide prompt is a dodge press, a Narrow one a parry press, with a dodge press as the Good fallback), with the lockouts turned off so a miss never blocks the next prompt. Nothing here repeats that window arithmetic.

There is no fail state. A prompt that runs out unanswered is a Miss and the sequence plays on. The overall outcome is only a grade: Perfect (every prompt Perfect), Good (no Miss), Fair (at most half missed), Poor (more). The story never branches on it.

In a co-op step the prompts alternate between actor slots 0 and 1. The screen tints them by slot and tags them 1 and 2.

## Assist options

Two settings (see DIALOGUE_UI.md) change the screen, read once when it starts (`ftcore::QteAssist`):

- **Quick-time events: auto-complete.** The autopilot plays every prompt perfectly, on the normal clock (no compression, no 10 s cap), and the player's input is ignored. It is off under autoplay, where the autopilot already plays, and when there is no step.
- **Hold inputs: press once.** A hold prompt latches on its press: letting go early is not a Miss, and it keeps its tap grade once its time is up. Taps and mashes are unchanged.

## Keys

The battle screen's mapping: left, down, up and right are the arrow keys, WASD, the pad d-pad, or the pad's X, A, Y and B. Keys are polled each frame (so a hold sees its key go up), and presses are stamped with the clock as it stood at the start of the frame. Input is ignored for the first 0.25 s, so the press that opened the screen can't count.

## HUD

A ring per prompt closes in on a target ring over 0.6 s (the battle tell frame, as a circle), with a direction glyph and its key names under it. A second inner ring marks a narrow prompt. The current prompt and the next two show. A hold draws a bar that fills, a mash a row of boxes that fills. One pip per prompt runs along the top (grey pending, gold perfect, cyan good, red miss). PERFECT, GOOD and MISS popups rise from the current prompt. All strings are static.

## Data

`Data/Qte/qte.json`, ids and numbers only (no story text), keyed like `Data/Battle/encounters.json`:

```json
{ "version": 1,
  "default": { "prompts": [ { "input": "left", "at": 1.0 },
                            { "input": "up", "at": 2.0, "window": "narrow" },
                            { "input": "down", "at": 3.0, "hold": 0.8 },
                            { "input": "right", "at": 5.0, "mash": 8, "limit": 1.5 } ] },
  "qtes": [ { "scene": "SC-X", "beat": 4, "step": 0, "prompts": [ ... ] } ] }
```

Prompt keys: `input` (`left`, `down`, `up`, `right`), `at` (seconds from the start; the first is at least 0.5), `window` (`wide` default or `narrow`), `hold` (0.2 to 5 s), `mash` (2 to 30 presses, needs `limit`) and `limit` (0.5 to 8 s, mash only). Hold and mash are exclusive. 1 to 24 prompts. Each prompt starts at least 0.4 s after the previous one ends. `step` defaults to 0; a duplicate (scene, beat, step) is an error and unknown keys are warnings. Every `co-op QTE` step of the script has its own entry (the script has no plain `QTE` step), generated from the step's position in the story: later steps run longer, with more narrow windows, holds and mashes, and every sequence ends by 8 s. The default is only the fallback. `test_field_goals.cpp` checks the entries against the script's steps.

## Rules (Core)

`Core/include/ftcore/Qte.h` holds `QteDef`, `QteSequence` (judging, grades, the autopilot), `QteScreen` (clock, input grace, result pause, autoplay) and the table loader. Fixed capacity, no allocation after loading, no Unreal types. Tests are in `Core/tests/test_qte.cpp`.

## Headless behaviour and the alpha

- With `bAutoPlay` the autopilot presses at the ideal time (the middle of the parry window or of the perfect-dodge part of the dodge window), lets go of a hold at its end and spreads a mash over half its limit, so it always grades Perfect. Its actions carry their own times, so a long frame (the alpha runs at 8x dilation) can't make it late.
- A sequence longer than 8 s of game time is played on a compressed clock so it fits, and the screen ends by 10 s at the latest (anything still open is graded a Miss). The default sequence is 6.5 s long.
- With `-AlphaFast` the screen's timers run at 8x, as on the stub screen. The world's own dilation already counts.
- **Counting.** `screenRoutesTo(Qte)` now returns `Qte`, and `AlphaCounters::addScreen` counts a QTE step in the `stub` bucket, so the `Alpha: FINISHED ... (b battle, f field, s stub)` line keeps its fields and its total. Adding a fourth field would change the gate's line, so the QTE stays in the stub bucket (as the choice screen does) until the line is changed on purpose.
