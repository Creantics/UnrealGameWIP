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

The first press shows the whole line; the next one moves on. Greyed options can't be picked.

## What the box shows

- **Lines**: the speaker's name above the box, the text in white.
- **Directions**: no speaker, pale blue text.
- **Choices**: the prompt types out, then the options appear below it. A wrong answer in a dialogue
  exchange leads to the correction and the question again, as the script says.
- **Holds**: the box hides for the pause; the story moves on by itself.
- **Gameplay beats**: the beat's notation waits in the box with "Space / Enter: continue".
  Continuing past a fight or an exploration beat opens its screen (see "Gameplay hand-off"); other
  kinds (mini-games) are skipped.
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

Tuning on the director: `RevealRate` (characters per second, default 45), `AutoPlayPause`.

## Gameplay hand-off

Continuing past a gameplay beat shows a banner ("Battle!" or "Into the field") and, after
`HandoffDelay` (1.2 s), opens a map:

| Beat kind | Screen | Map (director property) |
|---|---|---|
| CMB | battle | `BattleMap`, default `BattleDemo` |
| ENG, PLAY | field | `FieldMap`, default `Explore` |
| anything else | stays in the box | none; continuing skips it |

Before opening the map the director fills `UFTGameInstance::StoryCarry` (`FFTStoryCarry`): the map
to return to, the beat's scene id, index and kind, and the screen. The story subsystem lives on the
game instance, so the runner waits on the gameplay beat across the map change.

- The battle demo plays its usual fight. A win sets the carry done and goes back after 2.5 s; a loss
  stays for a retry.
- The exploration map becomes a free walk: no spots, encounters or objectives. "E  Back to the
  story" (E, Enter or pad X) goes back.
- Back on the story map the game mode sees the story already running, so it spawns a director
  without starting the story again. The director resumes after the beat if the runner is still on
  it, with no title card for the scene it came back to. If the screen wasn't done or the story
  moved, it logs a warning and leaves the beat in the box.

The `FINISHED` line counts moves, scenes and time from the latest map visit only.

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
10 s. Fast autoplay skips the hand-off, so this run never leaves the map.

- A hand-off round trip: `-StoryAutoPlayHandoff` makes autoplay hand off as well, and the screen
  plays itself; `-StoryQuitOnReturn` exits once the story resumes.

```
UnrealEditor-Cmd.exe ForgottenThird.uproject /Game/Maps/Demo -game -NullRHI -StoryScene=SC-P1-02 -StoryAutoPlay -StoryAutoPlayHandoff -StoryQuitOnReturn -unattended -nosplash -nosound
```

It logs `Story: hand-off CMB at SC-P1-02 beat 0 to the battle screen.`, the battle's `VICTORY`,
`BattleDemo: returning to the story on Demo.` and `Story: resumed after CMB at SC-P1-02 beat 0.`
(about 65 s). `-StoryScene=SC-P1-04` does the same for a PLAY beat through the field (beat 6).

## What's missing

- **Gameplay set-up from the beat.** Every fight is the demo fight and every field beat the demo
  map; the beat's params (enemies, place, goal) aren't read yet. Mini-game beats have no screen.
- **Losing.** A lost story fight just waits for a retry; there's no game-over or retry menu.
- **Voice.** Lines carry their VO ids (`GetLineId`), but nothing plays audio yet.
- **Localisation.** The box shows the script's text; the loc keys aren't looked up.
- **Menus, saving, backlog and skip.** None yet; the story always starts from the beginning.
- **Presentation.** Engine fonts on a flat panel; no portraits, staging or camera work.
- **At most four options** can be picked with the keys. The script has none with more.
