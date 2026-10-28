# Choice screen

The real screen for CHC and HE gameplay steps (`GameplayScreen::Choice`): a dialogue loop. The
structured choices inside the script are not this. The dialogue box already plays those
([DIALOGUE_UI.md](DIALOGUE_UI.md)). A loop is a separate, fixed-shape exchange stored in its own file.

## The loop

Each round is a Claim followed by an Audit. After the last round comes the Tag.

1. **Claim.** The host states the problem in one line.
2. **Audit.** The player picks one of 2 to 4 options. The claim stays on screen.
3. **Escalate.** On a wrong pick the host answers with that option's own correction (its `reply`).
   The option greys out and can't be picked again. Then back to the Audit.
4. **Concede.** The round's second miss plays the round's `concede` line, a new host line, in place
   of the option's correction. The third miss plays the round's `companion` line: the companion
   gives the answer in their own voice, and the round counts as resolved.
5. **Tag.** After the last round: the closing exchange (`tag`, one or more lines). Then the loop is
   done and its flags are set.

A right pick resolves the round. Its `reply`, if it has one, plays first. A round can have every
option right.

- **Pick cap.** At most 5 picks across the whole loop. Once the cap is reached, the loop goes to the
  Tag after the current line. If the fifth pick is wrong, it plays the companion line, as a third
  miss would. The loader warns when a loop's worst case goes past the cap.
- **No score and no fail state.** Nothing is counted on screen.
- **Skip.** Skip jumps straight to the Tag from anywhere before it.
- **No-resolve loops** (`"resolve": false`) have no Concede and no Tag. A miss only escalates.
  Skip, or the end of the last round, exits at once, and the flags are set on that exit.

The rules live in `ftcore::DialogueLoop` (`Core/include/ftcore/DialogueLoop.h`). It's a state
machine over a loaded `DialogueLoopDef` that returns string views into it and never allocates.
Tests are in `Core/tests/test_dialogue_loop.cpp`, and their fixture text is invented.

## Loop files

Loops are story text, and the repo is public. So the real files live in the gitignored story
folder, one per gameplay step:

```
Data/Story/loops/<scene>_<beat>_<step>.json     e.g. Data/Story/loops/SC-X-01_4_0.json
```

`<beat>` is the gameplay beat's index in its scene. `<step>` is the 0-based step in the beat's
`GameplayPlan`, the same key `Data/Battle/encounters.json` uses. The folder sits under the game
instance's `StoryDir`, `Data/Story` by default. Writing real loops is story work. The only
committed loop is the invented example `Data/Choice/example_loop.json`, which the tests load.

```json
{
  "version": 1,
  "scene": "EX-01", "beat": 3, "step": 0,
  "resolve": true,
  "host": "host", "companion": "companion",
  "rounds": [
    { "claim": "Which lamp is lit first?",
      "options": [
        { "text": "The blue lamp.", "right": false, "reply": "Blue is lit last, after the others." },
        { "text": "The red lamp.",  "right": true,  "reply": "Red, yes. It shows the way in." }
      ],
      "concede": "Think about what has to be seen before anything else.",
      "companion": { "speaker": "companion", "text": "It's the red one." } }
  ],
  "tag": [ "Then the hall is ready.", { "speaker": "companion", "text": "Told you we'd get there." } ],
  "flags": [ "example_loop_done", { "id": "example_lamp_count", "value": 2 } ]
}
```

| Key | Meaning |
|---|---|
| `version` | `1` |
| `scene`, `beat`, `step` | The key. It must match the file name. The screen refuses a file that names another step. |
| `resolve` | `true` (the default) or `false` for a no-resolve loop |
| `host`, `companion` | Speaker ids for lines written as plain strings. They default to `host` and `companion`. A speaker id is shown as written. |
| `rounds` | 1 to 5 rounds |
| `rounds[].claim` | One line |
| `rounds[].options` | 2 to 4 options: `text`, `right` (true or false), `reply` (a line; required on a wrong option) |
| `rounds[].concede` | A line. Required in a resolving loop when the round has 2 or more wrong options |
| `rounds[].companion` | A line, spoken by `companion` by default. Required in a resolving loop when the round has 3 wrong options |
| `tag` | 1 to 8 lines. Required in a resolving loop |
| `flags` | Up to 8. Each is an id, set to `true`, or `{ "id", "value" }` with a value of true, false, an integer or a string |

A line is either a string, spoken by the role's default speaker, or `{ "speaker": id, "text": text }`.

**Validation** is strict. Any of the following is an error, and the screen falls back to the step
card: bad JSON, a missing key, an out-of-range count, a round with no right option, a wrong option
without a reply, or a missing concede, companion or tag line where the rules need one. Warnings are
counted and logged: unknown keys, concede, companion or tag lines in a no-resolve loop (they're
ignored), a worst case past 5 picks, and a loop with no flags. Errors and warnings name paths and
keys only, never line text.

## Controls

| | Keyboard and mouse | Gamepad |
|---|---|---|
| Continue past a line | Space, left click or Enter | A |
| Pick an option | 1-4 | D-pad up, right, down, left |
| Skip to the Tag | Backspace | B |
| Pause menu | Escape | Start |

These are `AFTPlayerController`'s story actions (`OnAdvance`, `OnConfirm`, `OnChoose`, `OnCancel`),
bound by the game mode. Presses in the first 0.25 s are ignored, as on the stub screen. Escape and
pad Start open the pause menu (see [GAME_FRAME.md](GAME_FRAME.md), "Pause"): the loop stops, and
these keys are ignored while it is up. Escape no longer skips, and Start no longer continues.

## Regenerate the map

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildChoiceWorld [-Map=Choice] -NullRHI -unattended -nopause -nosplash
```

This writes `/Game/Maps/Choice`, the stub map's room (floor, back wall, sun, sky light, lamp,
camera, player start) with `WorldSettings.DefaultGameMode` set to `AFTChoiceGameMode`. The map is
generated, never hand-made. **The map has to exist before the alpha runs.** The director opens
`ChoiceMap` (`Choice`) for every CHC and HE step.

## Flow

1. The story director fills `UFTGameInstance::StoryCarry` and opens `ChoiceMap`
   ([DIALOGUE_UI.md](DIALOGUE_UI.md), "The contract for a screen").
2. `AFTChoiceGameMode::StartPlay` reads the carry and builds the stub card's text exactly as
   `AFTStubGameMode` does. It then loads `loops/<scene>_<beat>_<step>.json`.
3. With a loop, `AFTChoiceHUD` draws a box at the bottom: the speaker, the wrapped line, the
   numbered options during a pick (used wrong ones dim) and the key prompt. Strings are cached once
   per loop change, so drawing builds none.
4. When the loop finishes, the game mode sets each flag with `UFTStorySubsystem::SetFlag(id, value)`.
   An id or value the story doesn't know is logged as a warning by id and skipped. The flags live on
   the story runner, so the next autosave keeps them. Then the game mode marks the carry done and
   won, logs the line below, and opens `ReturnMap`:

```
Choice: done <scene> beat <b> step <s> picks <n> skipped <0|1>
```

Other log lines: `Choice: loop <scene> beat <b> step <s>: <r> rounds, <t> tag lines, <f> flags, <w> warnings.`,
`Choice: set <n> of <m> flags at ...`, and `Choice: no loop file for ...; showing the step card.`
Logs carry ids and counts only, never line text.

## Fallback: no loop file

When the step's file is missing, unreadable, refused or keyed to another step, the screen shows
exactly the stub card: the step's title (`stubTitle`, so CHC is "Choice" and HE is "Dialogue
loop"), its label, and "Step i of n". It continues on a press, using `ftcore::StubScreen` itself.
Nothing is set and the done line says `picks 0 skipped 0`. This keeps the alpha green without any
loop data. Opened without a valid carry, the screen says "No story step" and nothing else happens.

## Autoplay

With `StoryCarry.bAutoPlay` (the alpha's `-AlphaAutoPlay`), the loop takes the skip path:
`DialogueLoop::autoplay()` returns Skip at 0.25 s, so the Tag shows for a moment. At 1.0 s it
presses through the Tag and finishes, logging `skipped 1`. Timers follow the stub screen:
`StubScreen::speedFactor` with `-AlphaFast`, so a run at 8x takes about 0.13 s real. The fallback
card continues after 1 s, as the stub does.

## Alpha counting (decision)

`screenRoutesTo(Choice)` now returns `Choice`, and `AlphaCounters::addScreen` counts a choice
screen **in the stub bucket**. The director's line keeps its exact fields and totals:

```
Alpha: FINISHED <S> scenes, <N> screens (<b> battle, <f> field, <s> stub), <k> STUCK, <w> warnings
```

Here `stub` means "stub or choice". A separate `choice` field would change the line that
ALPHA.md records and that the other screen streams also touch. It can be appended after `s stub)`
later, once the QTE and cutscene screens land and the line is reworked once for all of them.

## Known gaps

- Refused loop files and unknown flags are logged as warnings but not added to the alpha's warning
  count. A screen doesn't touch `UFTGameInstance::Alpha` today.
- There's no typewriter. Each line shows whole.
