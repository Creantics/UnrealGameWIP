# Dialogue UI

The story on screen: a dialogue box that types out each line, numbered options for choices, a
title card when a scene starts and an end card when the story is over. It plays the story in
`Data/Story` (loaded by `UFTGameInstance` at start-up) on the default game mode.

Map: `/Game/Maps/Demo`, the project's default and editor start-up map. Open it and press Play.
The story starts at its first scene; `AFTGameMode` spawns an `AFTStoryDirector` and uses
`AFTStoryHUD` as its HUD. The battle and exploration demo maps have their own game modes and don't
start the story.

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
- **Gameplay beats** (fights, exploration, mini-games): nothing hands them off yet. The beat's
  notation waits in the box with "Space / Enter: continue", and continuing skips the gameplay.
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
10 s.

## What's missing

- **Gameplay hand-off.** Combat, exploration and mini-game beats should open the battle or field
  and resume the story afterwards; for now they wait in the box.
- **Voice.** Lines carry their VO ids (`GetLineId`), but nothing plays audio yet.
- **Localisation.** The box shows the script's text; the loc keys aren't looked up.
- **Menus, saving, backlog and skip.** None yet; the story always starts from the beginning.
- **Presentation.** Engine fonts on a flat panel; no portraits, staging or camera work.
- **At most four options** can be picked with the keys. The script has none with more.
