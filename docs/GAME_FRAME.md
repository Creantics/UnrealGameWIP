# Game frame: title, saving, end

The frame around the story on the `Demo` map: a title screen, an autosave and the way back to the
title when the story ends. It is a state of `AFTGameMode`, not a new map.

## Flow

1. **Title.** A fresh start (no story running and no gameplay hand-off pending) or the map option
   `?Title` shows `AFTTitleHUD` with New Game, Continue and Settings. `?Title` is what the battle
   screen's "Quit to title" opens (`Demo?Title`); it also drops any story run and hand-off carry left
   over.
2. **New Game** starts the story at its start scene (`StartScene` on the game mode, empty: the
   story's start). **Continue** loads the slot and resumes at the scene it was saved at.
   **Settings** opens the settings overlay (text speed, auto-advance, skip, volumes, assists,
   subtitle size, reduce flashes; see DIALOGUE_UI.md) over the title; Esc or its Back row closes it
   and writes `Saved/Settings.json`. The same overlay opens from the story with Esc.
3. **Story.** The game mode switches to `AFTStoryHUD` and spawns the director, as before.
4. **End.** When the story finishes, the end card shows. After a short delay (`EndCardDelay`, 0.6 s)
   Space, Enter, click or pad A / Start reopens `Demo?Title`.

Returning from a battle or field screen (story running, carry valid) never shows the title.

## Controls (title)

| | Keyboard and mouse | Gamepad |
|---|---|---|
| Move the selection | Up / Down, W / S | d-pad up / down (left / right too) |
| Pick the selected item | Enter, Space or left click | A or Start |
| Select with the mouse | move the mouse over an item, then click | |
| Shortcuts | 1 New Game, 2 Continue, 3 Settings | |

Continue is greyed when there is no usable save: the selection can't land on it (Down from New Game
steps over it to Settings) and its shortcut does nothing. With a usable save the selection starts on
Continue. Settings is always available. The rules are `ftcore::TitleMenu`
(`Core/include/ftcore/TitleMenu.h`, tests in `test_title.cpp`). While the title is up
`AFTPlayerController` has a menu input context (`BeginMenuInput`) that takes the d-pad off the
story's choose actions and broadcasts `OnMenuStep` (and `OnMenuAdjust` for left / right); it also
shows the mouse cursor. While the settings overlay is open the title ignores input
(`UFTSettingsSubsystem::BlocksInput`).

## Settings

`UFTSettingsSubsystem` (a game instance subsystem) loads `Saved/Settings.json` at start-up and
writes it when the overlay closes; a missing, unreadable or newer-version file means the defaults.
The file is `ftcore::encodeSettings` JSON (a `version` field, values clamped on read). The director
applies text speed, auto-advance and skip mode as they change; volumes, the defence-window assist and
reduce-flashes are stored for the audio, combat and style code to read later (poll
`GetRevision()`, read `Get()`). Headless runs ignore all of it.

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
- Format 2 of the save text adds `"seen"`: for each scene with a seen beat, its id and a hex string
  with one bit per beat (`StorySave::seen`, `Seen.h`). It is what "skip only seen lines" reads.
  Format 1 saves still load, with nothing seen. Continue replaces the seen bits with the slot's; New
  Game keeps the ones in memory (or, with none, the slot's) before its first autosave overwrites the
  slot. Scenes or beats a newer script no longer has are dropped on load. The backlog is not saved.
- The save text also holds `"party": {"xp": N, "gold": N}` (`StorySave::party`, `Progression.h`),
  the party's experience and gold from won fights. A save without it loads with both at 0. New Game
  resets them; Continue restores them.

## Command line

| Flag | Effect |
|---|---|
| `-StoryAutoPlay`, `-StoryAutoPlayFast`, `-StoryAutoPlayHandoff`, `-StoryScene=<id>`, `-AlphaAutoPlay` | Headless: skip the title, start the story at once, never return to the title at the end, and never write the save slot (so a check can't overwrite a player's save). `-AlphaAutoPlay` also logs `Title: New Game (autoplay).` |
| `-StoryQuitOnFinish` | Quit at the end (headless behaviour), no return to the title. |

Log lines: `Title: shown, continue on|off (<code>).`, `Title: New Game.`, `Title: Continue.`,
`Title: Settings.`, `Title: end card up.`, `Title: back to the title.`, `Save: slot written at scene <id>.`,
`Settings: loaded.` / `Settings: no file, using defaults.` / `Settings: opened.` / `Settings: closed.`

## Files

`FTGameMode.*` (states, input handling, end return), `FTTitleHUD.*` (drawing),
`FTStorySubsystem.*` (slot read / write, autosave, backlog and seen bits), `FTStorySaveGame.*` (the
save object), `FTSettingsSubsystem.*` and `FTSettingsOverlay.*` (settings file, overlay state and
drawing), `FTPlayerController.*` (menu context), Core `TitleMenu.h`, `Settings.*`, `Seen.*` and
`StorySave.*` (`checkStorySave`, `saveRefusalName`).
