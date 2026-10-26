# Dialogue UI

The story on screen: a dialogue box that types out each line, numbered options for choices, a
title card when a scene starts and an end card when the story is over. It plays the story in
`Data/Story` (loaded by `UFTGameInstance` at start-up) on the default game mode.

Map: `/Game/Maps/Demo`, the project's default and editor start-up map. Open it and press Play.
The story starts at its first scene; `AFTGameMode` spawns an `AFTStoryDirector` and uses
`AFTStoryHUD` as its HUD. The battle and exploration demo maps have their own game modes and don't
start the story; the story opens them for its gameplay beats (see below).

## Controls

| | Keyboard and mouse | Gamepad |
|---|---|---|
| Finish the text, then continue | Space, left click or Enter | A or Start |
| Pick an option (once the prompt is out) | 1-4 | D-pad up, right, down, left |
| Backlog | B or mouse wheel up; B or Esc closes | |
| Scroll the backlog | Up / Down, mouse wheel | |
| Skip (hold) | Ctrl or S | Right trigger |
| Settings | Esc (Esc, Backspace or the Back row closes) | pad B closes |

The first press shows the whole line; the next one moves on. Greyed options can't be picked.

### Backlog

B (or a wheel notch up) opens a scrollable overlay of what you were shown: lines with their
speaker, directions, choice prompts and the option you picked (in green, with a ">" in front).
It keeps the last 200 entries, newest at the bottom; Up / Down or the wheel go back and forward in
time. While it is open the story stands still (no typing, a hold waits) and presses are ignored. B
or Esc closes it. Not available on the end card. The log starts over on New Game, on loading a
save, on returning to the title and when the story is reloaded; it is never saved.

### Skip

Holding Ctrl, S or the pad's right trigger fast-forwards: each line or direction shows at once and
the story moves on every 0.08 s (`SkipInterval` on the director); a hold is passed as soon as it
is reached. It stops at a choice, at a gameplay beat and at the end, and carries on after you
answer if the key is still down. A "SKIP >>" tag shows top right while it runs. With the setting
**Skip: Seen lines only** (the default) it also stops at a line you have not seen before, which
types out normally; **All lines** never stops for that. A beat counts as seen once you move off it.
The seen beats are a bitset (one bit per beat) saved with the story slot (see GAME_FRAME.md), so
they survive quitting; a new game keeps them.

### Auto-advance

With **Auto-advance** on (settings) a fully shown line or direction moves on by itself after the
delay (0.25 s to 5 s, default 1.5 s). Choices and gameplay beats always wait for you. Skip takes
priority while held. An "AUTO" tag shows top right.

### Settings

Esc opens the settings overlay from the story (and the title's Settings entry opens the same
one). Up / Down (or the mouse) pick a row, Left / Right or a click on the left / right half of the
value step it, Enter / Space / click cycle it. Changes apply at once and are written to
`Saved/Settings.json` when the overlay closes (Esc, or the Back row). The story is paused behind it.

| Row | Values | Used by |
|---|---|---|
| Text speed | Slow 22, Normal 45, Fast 90 characters per second, Instant | the presenter's reveal rate |
| Auto-advance, delay | on / off, 0.25 to 5 s in quarters | the director |
| Skip | seen lines only / all lines | skip |
| Master, music, voice, effects volume | 0 to 100 % in steps of 10 | stored; no audio yet |
| Defence window assist | off / wide | stored; the combat stream reads it |
| Subtitle size | small 0.85x, medium, large 1.3x | the dialogue box's text size |
| Reduce flashes | on / off | stored; the style controller reads it |
| Quick-time events | play / auto-complete | the QTE screen (see QTE_SCREEN.md) |
| Hold inputs | hold down / press once | the QTE screen's hold prompts |
| Skip choices | off / on | stored; mirrored to the script's flag, nothing acts on it yet |

The last three are the script's assist options. Each is mirrored onto the matching `F_SYS_OPT_*`
flag (`ftcore::settingsOptionFlags`) when the story starts, after a save loads (the settings win
over the saved value) and whenever the setting changes, so the script can branch on them. A script
that doesn't declare one of the flags is left alone. Because the mirror runs once the story has
started, a check in the opening beats before the story's first stop sees the flag's default.

The rules (`ftcore::SettingsMenu`, `ftcore::Settings`, JSON with a `version` field, defaults and
clamping) are in `Core/include/ftcore/Settings.h` with tests in `test_settings.cpp`. The settings
never touch autoplay: a headless run (`-StoryAutoPlay`, `-AlphaAutoPlay`, ...) keeps its own text
speed, never binds the backlog, skip or settings keys, and is never blocked by them.

## What the box shows

- **Lines**: the speaker's name above the box, the text in white.
- **Directions**: no speaker, pale blue text.
- **Choices**: the prompt types out, then the options appear below it. A wrong answer in a dialogue
  exchange leads to the correction and the question again, as the script says.
- **Holds**: the box hides for the pause; the story moves on by itself.
- **Gameplay beats**: the beat's notation waits in the box with "Space / Enter: continue".
  Continuing past a gameplay beat opens its screens one after another (see "Gameplay hand-off"); a
  beat with no step in its plan is skipped.
- **Scene title**: shown for a few seconds at the top when a scene starts.
- **The End**: when the story finishes.

## How it fits together

- `ftcore::DialoguePresenter` (`Core/include/ftcore/Dialogue.h`) holds the rules: the typewriter
  (counted in UTF-8 codepoints, so a multibyte character appears whole), what a press or an option
  key means on each beat type, and what autoplay does. It reads the `StoryRunner` and hands back a
  `DialogueCommand`; it never moves the runner itself. No allocation.
- `AFTStoryDirector` applies those commands through `UFTStorySubsystem` (so the subsystem's events
  still fire), turns off the player controller's own story routing while it runs, and caches the
  beat's strings once per beat for the HUD. Logs carry scene ids and counts only.
- `AFTStoryHUD` draws on the canvas (no UMG). It wraps the text once per beat and draws the typed
  part with a reused buffer, so steady-state drawing builds no strings.

- `ftcore::DialogueBacklog` (`Backlog.h`) is a fixed ring of 200 entries holding database indices
  (scene, beat, option), not text, so a push never allocates; the text is read back from the story
  database on demand. `ftcore::SeenLines` (`Seen.h`) is the seen-beat bitset, `ftcore::SkipController`
  and `ftcore::AutoAdvancer` (`Skip.h`) turn held-key and delay into `DialogueCommand`s, and
  `ftcore::Settings` / `SettingsMenu` (`Settings.h`) hold the options. Tests: `test_backlog.cpp`,
  `test_skip.cpp`, `test_settings.cpp`.
- `UFTStorySubsystem` keeps the backlog, the seen bits and a pause flag (so they outlive the
  director, which is spawned again after each gameplay screen). `UFTSettingsSubsystem` keeps the
  settings and the overlay state; `AFTPlayerController` has the reading keys (`OnBacklog`,
  `OnWheel`, `OnSkip`, and `OnMenuAdjust` in the menu context, which also shows the mouse cursor).

Tuning on the director: `RevealRate` (characters per second, default 45; autoplay only, play by hand
uses the text speed setting), `AutoPlayPause`, `SkipInterval` (0.08 s).

## Gameplay hand-off

Continuing past a gameplay beat lowers it with `ftcore::GameplayPlan` (its `mode` param, or its kind
when the mode is empty) and plays the plan's steps in order. Each step shows a banner and, after
`HandoffDelay` (1.2 s), opens that step's map:

| Step screen | Tokens | Banner | Map (director property) |
|---|---|---|---|
| battle | CMB | "Battle!" | `BattleMap`, default `BattleDemo` |
| field | ENG, PLAY | "Into the field" | `FieldMap`, default `Explore` |
| choice | CHC, HE | the token ("CHC") | `ChoiceMap`, default `Choice` ([CHOICE_SCREEN.md](CHOICE_SCREEN.md)) |
| qte | QTE, co-op QTE | the token ("QTE") | `QteMap`, default `Qte` ([QTE_SCREEN.md](QTE_SCREEN.md)) |
| stub | BNT, CG, MINI, anything unknown | the token ("CG") | `StubMap`, default `Stub` |
| none | a plan with no step | none | the beat stays in the box; continuing skips it |

The director logs `Story: step i/n <screen> at <scene> beat <b>.` (the routed screen's name: battle, field, stub, choice, qte or cutscene) (i from 1) for each.

### The contract for a screen

Before opening a step's map the director fills `UFTGameInstance::StoryCarry` (`FFTStoryCarry`):

| Field | Meaning |
|---|---|
| `bValid` | set |
| `ReturnMap` | the story map to open again when done |
| `Scene`, `Beat`, `Kind` | the gameplay beat's scene id, index and kind as written |
| `Screen` | `Battle`, `Field` or `Stub` |
| `Step`, `StepCount` | 0-based step within the beat and the plan's step count |
| `StepToken`, `StepLabel` | the step's token as notation and its qualifiers ("duel"); label empty when none |
| `bAutoPlay` | the screen plays itself (headless runs) |
| `bDone`, `bWon` | written by the screen |

A screen reads the carry, plays, then sets `bDone` (and `bWon`) and opens `ReturnMap`; it never
touches the story. Setting `bDone` false (or leaving it) means it gave up. `-AlphaFast` is read
through `UFTGameInstance::IsAlphaFast()`; screens apply their own time dilation to their autopilot.

The plan itself lives on the game instance (`UFTGameInstance::Chain`, an `ftcore::StoryChain`), and so
does the alpha tally (`Alpha`). The story subsystem lives there too, so the runner waits on the
gameplay beat across all the map changes.

- The battle demo plays its usual fight. A win sets the carry done and goes back after 2.5 s; after
  a loss, Space or Enter restarts the fight.
- The exploration map becomes a free walk: no spots, encounters or objectives. "E  Back to the
  story" (E, Enter or pad X) goes back.
- Back on the story map the game mode sees the story already running, so it spawns a director
  without starting the story again. The director opens the next step of the beat (no title card), or
  after the last step resumes the story past the beat if the runner is still on it.
- If a screen came back with `bDone` false, or the story moved, the director logs a warning and
  leaves the beat in the box. Autoplay would try the beat again; in alpha mode it skips the beat
  instead, and the warning counts.

The `FINISHED` line counts moves, scenes and time from the latest map visit only.

### Alpha mode

`-AlphaAutoPlay` is fast autoplay (each text shown whole, many moves a frame) that hands off on every
step, with `carry.bAutoPlay` set. The game instance keeps the counters across maps: scenes, battle,
field and stub screens played, STUCK and warnings (unknown or dropped plan tokens, screens that came
back not done, a story that moved). When the story finishes it logs

```
Alpha: FINISHED <S> scenes, <N> screens (<b> battle, <f> field, <s> stub), <k> STUCK, <w> warnings
```

and, with `-StoryQuitOnFinish`, exits with code 0 only if there was no STUCK and no warning (1
otherwise). A STUCK run also exits 1 with `-StoryQuitOnFinish`. `-AlphaFast` makes the gameplay
screens dilate time and shortens the banner pause to 0.1 s.

```
UnrealEditor-Cmd.exe ForgottenThird.uproject -game -NullRHI -AlphaAutoPlay -AlphaFast -StoryQuitOnFinish -unattended -nosplash -nosound
```

The existing single hand-off flags keep working: `-StoryAutoPlayHandoff` and `-StoryQuitOnReturn`
(which now exits after the whole beat, all its steps, resumes).

## Headless checks

- Core: `test_dialogue.cpp` covers codepoint counting, pacing, the two-step press, options, gameplay
  and hold beats, and autoplay to the end without allocating. `test_story_import.cpp` has the
  presenter's autoplay play the real script to the end.
- Unreal end to end, the whole script with each text shown at once:

```
UnrealEditor-Cmd.exe ForgottenThird.uproject /Game/Maps/Demo -game -NullRHI -StoryAutoPlayFast -StoryQuitOnFinish -unattended -nosplash -nosound
```

It logs one `Story: scene <id>` line per scene and `Story: FINISHED after 3042 moves, 73 scenes`,
then exits. `-StoryAutoPlay` plays at reading pace instead (typing, then a pause), and
`-StoryScene=<id>` starts at a given scene. `Story: STUCK` means autoplay found nothing to do for
10 s. Paced `-StoryAutoPlay` hands off too, and the opened screen plays itself; fast autoplay
skips the hand-off, so the whole-script run never leaves the map.

- A hand-off round trip: `-StoryAutoPlayHandoff` turns on autoplay that hands off even when fast,
  and the opened screen plays itself; `-StoryQuitOnReturn` exits once the story resumes.

```
UnrealEditor-Cmd.exe ForgottenThird.uproject /Game/Maps/Demo -game -NullRHI -StoryScene=SC-P1-02 -StoryAutoPlay -StoryAutoPlayHandoff -StoryQuitOnReturn -unattended -nosplash -nosound
```

It logs `Story: hand-off CMB at SC-P1-02 beat 0 to the battle screen.`, the battle's `VICTORY`,
`BattleDemo: returning to the story on Demo.` and `Story: resumed after CMB at SC-P1-02 beat 0.`
(about 65 s). `-StoryScene=SC-P1-04` does the same for a PLAY beat through the field (beat 6).

## What's missing

- **Gameplay set-up from the beat.** The beat's params beyond its mode (enemies, place, goal) are
  read by the screens that implement them; the stub screen stands in for the mini-games.
- **Losing.** A lost story fight just waits for a retry; there's no game-over or retry menu.
- **Voice.** Lines carry their VO ids (`GetLineId`), but nothing plays audio yet.
- **Localisation.** The box shows the script's text; the loc keys aren't looked up.
- **Pad control of the settings values.** The d-pad moves between rows and A cycles a value, but
  there is no left / right step on the pad (the d-pad's left and right already move up and down in
  menus); use Enter / A, or the keyboard or mouse.
- **Backlog voice replay.** The backlog is text only (there is no audio yet).
- **Presentation.** Engine fonts on a flat panel; no portraits, staging or camera work.
- **At most four options** can be picked with the keys. The script has none with more.
