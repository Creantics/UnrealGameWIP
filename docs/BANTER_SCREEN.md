# Banter screen

The real screen for BNT gameplay steps (`GameplayScreen::Banter`). It plays a short exchange of
lines between the leads. Each line holds for a while and then gives way to the next by itself. The
player can press to move on sooner or skip the rest.

## The rules

- **Lines.** 1 to 32 lines, each with a speaker id and its text. Up to 4 speakers per file.
- **Hold.** A line shows for `1.0 + 0.05 s per byte` of text, clamped to 1.5–8 s. A line's own
  `hold` (0.5–15 s) replaces that.
- **Sides.** The first speaker in the file speaks from the left, the second from the right. A third
  and fourth alternate again (left, right).
- **Press** shows the next line at once. **Skip** ends the banter. A long frame runs through as many
  lines as it covers.
- **No score and no fail state.**

The rules live in `ftcore::BanterSequence` (`Core/include/ftcore/Banter.h`). It plays a loaded
`BanterDef`, returns string views into it and never allocates. Tests are in
`Core/tests/test_banter.cpp`, and their fixture text is invented.

## Banter files

Banter is story text, and the repo is public. So the real files live in the gitignored story
folder, one per gameplay step:

```
Data/Story/banter/<scene>_<beat>_<step>.json     e.g. Data/Story/banter/SC-X-01_4_0.json
```

The key is the same one the choice screen's loop files use: `<beat>` is the gameplay beat's index in
its scene, and `<step>` is the 0-based step in the beat's `GameplayPlan`. The folder sits under the
game instance's `StoryDir`, `Data/Story` by default. Writing real banter is story work. The only
committed file is the invented example `Data/Banter/example_banter.json`, which the tests load.

```json
{
  "version": 1,
  "scene": "EX-01", "beat": 2, "step": 0,
  "lines": [
    { "speaker": "left",  "text": "Did you hear that?" },
    { "speaker": "right", "text": "Only the wind.", "hold": 2.0 }
  ]
}
```

| Key | Meaning |
|---|---|
| `version` | `1` |
| `scene`, `beat`, `step` | The key. It must match the file name. The screen refuses a file that names another step. |
| `lines` | 1 to 32 lines |
| `lines[].speaker` | A speaker id, shown as written. At most 4 distinct ids. |
| `lines[].text` | The line |
| `lines[].hold` | Optional seconds the line shows, 0.5–15 |

**Validation** is strict. Any of these is an error, and the screen falls back to the step card: bad
JSON, a missing key, a version other than 1, no lines or more than 32, an empty speaker or text, a
hold out of range, or more than 4 speakers. Unknown keys are warnings. Errors and warnings name
paths and keys only (`lines[3].hold`), never line text.

## Controls

| | Keyboard and mouse | Gamepad |
|---|---|---|
| Next line now | Space, left click or Enter | A |
| Skip the rest | Backspace | B |
| Pause menu | Escape | Start |

These are `AFTPlayerController`'s story actions (`OnAdvance`, `OnConfirm`, `OnCancel`), bound by the
game mode. Presses in the first 0.25 s are ignored, as on the stub screen. Escape and pad Start
open the pause menu (see [GAME_FRAME.md](GAME_FRAME.md), "Pause"): the lines stop, and these keys
are ignored while it is up. Escape no longer skips, and Start no longer shows the next line.

## Regenerate the map

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildBanterWorld [-Map=Banter] -NullRHI -unattended -nopause -nosplash
```

This writes `/Game/Maps/Banter`: the choice map's room (floor, back wall, sun, sky light, lamp,
camera, player start) with `WorldSettings.DefaultGameMode` set to `AFTBanterGameMode`. The map is
generated, never hand-made. **The map has to exist before the alpha runs.** The director opens
`BanterMap` (`Banter`) for every BNT step.

## Flow

1. The story director fills `UFTGameInstance::StoryCarry` and opens `BanterMap`
   ([DIALOGUE_UI.md](DIALOGUE_UI.md), "The contract for a screen").
2. `AFTBanterGameMode::StartPlay` reads the carry and builds the stub card's text exactly as
   `AFTStubGameMode` does. It then loads `banter/<scene>_<beat>_<step>.json`.
3. With a file, `AFTBanterHUD` draws a box at the bottom, on the speaker's side: the speaker, the
   wrapped line, a bar along the top that runs down while the line holds, "i / n" and the key
   prompt. Strings are cached once per line change, so drawing builds none.
4. When the banter ends, the game mode marks the carry done and won, logs the line below, and
   opens `ReturnMap`:

```
Banter: done <scene> beat <b> step <s> lines <n> skipped <0|1>
```

`lines` counts the lines that were shown. Other log lines are
`Banter: lines <n> speakers <k> at <scene> beat <b> step <s>, <w> warnings.` and
`Banter: no banter file for ...; showing the step card.` Logs carry ids and counts only, never line
text.

## Fallback: no banter file

When the step's file is missing, unreadable, refused or keyed to another step, the screen shows
exactly the stub card: the step's title (`stubTitle`, so BNT is "Banter"), its label, and "Step i of
n". It continues on a press, using `ftcore::StubScreen` itself, and the done line says
`lines 0 skipped 0`. This keeps the alpha green without any banter data. Opened without a valid
carry, the screen says "No story step" and nothing else happens.

## Autoplay

With `StoryCarry.bAutoPlay` (the alpha's `-AlphaAutoPlay`), the first line shows for 0.25 s and
then the banter skips itself, logging `skipped 1`. Timers follow the stub screen:
`StubScreen::speedFactor` with `-AlphaFast`, so a run at 8x takes about 0.03 s real. The fallback
card continues after 1 s, as the stub does.

## Alpha counting

`screenRoutesTo(Banter)` returns `Banter`, and `AlphaCounters::addScreen` counts a banter screen in
the **stub bucket**, as it does choice, quick-time event and cutscene screens. The alpha line keeps
its exact fields and totals:

```
Alpha: FINISHED 73 scenes, 167 screens (40 battle, 91 field, 36 stub), 0 STUCK, 0 warnings
```

## Known gaps

- **No walking.** The lines play over the room's fixed camera. Walking while the leads talk would
  need the explore character, which belongs to the explore stream (and binds Space to jump, which
  clashes with Advance). That is a later step.
- No portraits, voice or typewriter. Each line shows whole.
- Refused files are logged as warnings but not added to the alpha's warning count.
