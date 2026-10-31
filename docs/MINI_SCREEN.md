# Mini-game screen

The real screen for MINI gameplay steps (`GameplayScreen::Mini`). It plays one small, generic
rhythm round. Button steps on a beat grid fall down their lanes to a line, and the player presses
each action button as it lands. The mini-game is not a battle, so it shows no arrows (those are the
Turns battle's combos, [COMBAT_SYSTEMS.md](COMBAT_SYSTEMS.md)): lane i is `ftcore::ActionButton` i. Every MINI step plays this same kind of round. Only its numbers change per
step.

## The rules

- **The chart** comes from `generateComboInto` (`ComboGenerator.h`) on the round's beat grid: `steps`
  notes, one every `subdivision` beats, in `lanes` lanes, starting `lead` seconds in. The seed is
  the round's `seed` plus the try number, so a retry plays a fresh chart of the same shape.
- **Judgement** is the battle's own: `ComboSequence` with the standard windows scaled by `window`.
  Perfect, Great and Good count 1, 0.8 and 0.5. A wrong button in a note's window misses it. A press
  with no note in range is a stray, and locks input out for 0.15 s.
- **The beat grid** is a `Conductor`'s helpers (`beatAt`, `secondsPerBeat`) on the screen's own
  clock. No song plays, so no audio cursor is fed. The judgement line pulses on the beat.
- **Pass.** A round passes when its accuracy reaches `pass` (CLEARED).
- **No fail state that blocks the story.** A round below the pass mark is retried while tries remain
  (AGAIN). After the last try the story continues anyway (ONWARD). The outcome shows for 1.5 s. The
  carry is always marked won, and nothing branches on the result.

The rules live in `ftcore::MiniScreen` (`Core/include/ftcore/Mini.h`). It allocates its buffers once
in `begin()`, and `tick()` and `press()` never allocate. Tests are in `Core/tests/test_mini.cpp`.

## The table

`Data/Mini/mini.json` holds ids and numbers only, no story text, so it is committed. Like
`Data/Qte/qte.json`, it is keyed by scene id, gameplay beat index and plan step index. A step that
isn't listed plays the `default` entry. With no file, or a refused one, every step plays the
built-in default.

```json
{
  "version": 1,
  "default": { "bpm": 100, "steps": 12, "lanes": 3, "subdivision": 1, "seed": 7,
               "pass": 0.6, "tries": 3, "lead": 2.0, "window": 1.0 },
  "minis": [
    { "scene": "SC-X-01", "beat": 4, "step": 0, "bpm": 120, "steps": 16, "lanes": 4 }
  ]
}
```

The committed table has only the default.

| Key | Meaning | Range | Built-in |
|---|---|---|---|
| `bpm` | Tempo of the beat grid | 60–200 | 100 |
| `steps` | Notes in a round | 4–32 | 12 |
| `lanes` | Lanes used (Attack and Jump, then Dodge, then Guard) | 2–4 | 3 |
| `subdivision` | Beats between notes | 1 or 0.5 | 1 |
| `seed` | Chart seed (each retry adds 1) | 0–2^31−1 | 7 |
| `pass` | Accuracy that clears the round | 0–1 | 0.6 |
| `tries` | Rounds before the story goes on anyway | 1–9 | 3 |
| `lead` | Seconds before the first note | 1–4 | 2 |
| `window` | Scale on the judgement windows | 0.5–2 | 1 |

Every key is optional. In an entry, a missing key takes the built-in value, not the table's
default. `scene` and `beat` are required in an entry, and `step` defaults to 0. A duplicate
(scene, beat, step) is an error, and so is any value out of range. Unknown keys are warnings.
Errors name paths (`minis[2].bpm`).

## Controls

| Button | Keyboard | Gamepad |
|---|---|---|
| Attack | J | face left (X) |
| Guard | I | face top (Y) |
| Dodge | L | face right (B) |
| Jump | K | face bottom (A) |

This is the QTE and Beat screens' mapping (`FTActionButtons.h`): the pad's face buttons and the keyboard's I, J, K, L diamond in the same layout. The game mode polls the keys every frame and presses
before the tick, so a press is stamped with the round's clock as it stood at the start of the
frame. Presses in the first 0.25 s of each round are ignored. Escape (keyboard) or Start (pad)
opens the pause menu (see [GAME_FRAME.md](GAME_FRAME.md), "Pause"): the round's clock stops with the
world, and a key still held in the frame after the menu closes (the pad's A that picked Resume) is
not read as a press until it is let go and pressed again.

## Regenerate the map

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildMiniWorld [-Map=Mini] -NullRHI -unattended -nopause -nosplash
```

This writes `/Game/Maps/Mini`: the choice map's room (floor, back wall, sun, sky light, lamp,
camera, player start) with `WorldSettings.DefaultGameMode` set to `AFTMiniGameMode`. The map is
generated, never hand-made. The director opens the `Mini` map for every MINI step.

## Flow

1. The story director fills `UFTGameInstance::StoryCarry` and opens the `Mini` map.
2. `AFTMiniGameMode::StartPlay` loads the table and picks the step's round. It logs
   `Mini: round at <scene> beat <b> step <s>: steps <n> lanes <l> bpm <bpm> tries <t>.`
3. `AFTMiniHUD` draws four lanes in button order, each named under the line (lanes the round doesn't use are dim), the falling
   notes (a hit note vanishes, a missed one turns red), the pulsing line, the latest grade, "Try i
   of n" and, between rounds, the outcome with the round's accuracy.
4. When the screen finishes, the game mode marks the carry done and won, logs the line below, and
   opens `ReturnMap`:

```
Mini: done <scene> beat <b> step <s> tries <t> accuracy <pct> cleared <0|1>
```

`accuracy` is the best try's, in percent.

Opened without a valid carry, the screen says "No story step" and nothing else happens.

## Autoplay

With `StoryCarry.bAutoPlay`, input is ignored and an autopilot presses every note at its own time.
The round's clock is compressed so the whole round fits 0.6 s of game time, at any frame rate. The
result shows for 0.25 s. Autoplay clears the first try, so the screen ends within about 0.85 s of
game time. With `-AlphaFast` the timers run at 8x (`StubScreen::speedFactor`).

## Alpha counting

`screenRoutesTo(Mini)` returns `Mini`, and `AlphaCounters::addScreen` counts a mini-game screen in
the **stub bucket**. The alpha line keeps its exact fields and totals.

## Known gaps

- **The alpha never opens this screen.** The script names MINI only in hidden direction notes, not
  in any gameplay beat's mode, so no plan has a MINI step. The screen is covered by the Core tests,
  and by opening the map by hand.
- **One kind of game.** The script's notes describe varied mini-games (puzzles, sorting and the
  like). This screen is a generic stand-in for all of them. Real per-step games are later work.
- No music. The beat grid is silent, and the line's pulse is the only cue.
