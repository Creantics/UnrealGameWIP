# Game frame: title, saving, end

The frame around the story on the `Demo` map: a title screen, an autosave and the way back to the
title when the story ends. It is a state of `AFTGameMode`, not a new map.

## Flow

1. **Title.** A fresh start (no story running and no gameplay hand-off pending) or the map option
   `?Title` shows `AFTTitleHUD` with New Game and Continue. `?Title` is what the battle screen's
   "Quit to title" opens (`Demo?Title`); it also drops any story run and hand-off carry left over.
2. **New Game** starts the story at its start scene (`StartScene` on the game mode, empty: the
   story's start). **Continue** loads the slot and resumes at the scene it was saved at.
3. **Story.** The game mode switches to `AFTStoryHUD` and spawns the director, as before.
4. **End.** When the story finishes, the end card shows. After a short delay (`EndCardDelay`, 0.6 s)
   Space, Enter, click or pad A / Start reopens `Demo?Title`.

Returning from a battle or field screen (story running, carry valid) never shows the title.

## Controls (title)

| | Keyboard and mouse | Gamepad |
|---|---|---|
| Move the selection | Up / Down, W / S | d-pad up / down (left / right too) |
| Pick the selected item | Enter, Space or left click | A or Start |
| Shortcuts | 1 New Game, 2 Continue | |

Continue is greyed when there is no usable save: the selection can't land on it and its shortcut
does nothing. With a usable save the selection starts on Continue. The rules are
`ftcore::TitleMenu` (`Core/include/ftcore/TitleMenu.h`, tests in `test_title.cpp`). While the title
is up `AFTPlayerController` has a menu input context (`BeginMenuInput`) that takes the d-pad off the
story's choose actions and broadcasts `OnMenuStep`.

## Saving

- One slot, `Slot0` (user index 0), a `UFTStorySaveGame` holding the JSON text of
  `ftcore::StorySave` (`encodeStorySave`). On disk: `Saved/SaveGames/Slot0.sav` in the project
  (or the packaged game's own Saved folder).
- It is written at every scene start by `UFTStorySubsystem` (on its scene-changed event, not by
  the director). Nothing is written in the middle of a gameplay beat. A save therefore resumes at
  the start of its scene, with the flags as they were then. Starting a New Game writes the first
  scene's save at once, replacing the old one.
- Loading goes through `ftcore::checkStorySave` first. A save is refused, with a reason code, when
  it is empty (`empty`), unreadable or in an unknown format (`invalid`), no story is loaded
  (`no_story`), its scene is gone from the script (`scene_missing`) or its beat is past the end of
  the scene (`beat_out_of_range`). The title checks the slot when it opens; a refused save
  greys out Continue, and a load that fails after being chosen greys it too. The log carries only
  the code (`Save: load refused (scene_missing).`), never text.
- Flags a newer script no longer has are skipped (counted in the log), as `applyStory` does.

## Command line

| Flag | Effect |
|---|---|
| `-StoryAutoPlay`, `-StoryAutoPlayFast`, `-StoryAutoPlayHandoff`, `-StoryScene=<id>`, `-AlphaAutoPlay` | Headless: skip the title, start the story at once, never return to the title at the end, and never write the save slot (so a check can't overwrite a player's save). `-AlphaAutoPlay` also logs `Title: New Game (autoplay).` |
| `-StoryQuitOnFinish` | Quit at the end (headless behaviour), no return to the title. |

Log lines: `Title: shown, continue on|off (<code>).`, `Title: New Game.`, `Title: Continue.`,
`Title: end card up.`, `Title: back to the title.`, `Save: slot written at scene <id>.`

## Files

`FTGameMode.*` (states, input handling, end return), `FTTitleHUD.*` (drawing),
`FTStorySubsystem.*` (slot read / write, autosave), `FTStorySaveGame.*` (the save object),
`FTPlayerController.*` (menu context), Core `TitleMenu.h` and `StorySave.*` (`checkStorySave`,
`saveRefusalName`).
