# Game frame: title, saving, end

The frame around the story on the `Demo` map: a title screen, an autosave, the way back to the
title when the story ends, and the pause menu every gameplay screen shares. The title is a state of
`AFTGameMode`, not a new map.

## Flow

1. **Title.** A fresh start (no story running and no gameplay hand-off pending) or the map option
   `?Title` shows `AFTTitleHUD` with New Game, Continue, Settings and Quit. `?Title` is what the
   battle screen's "Quit to title" and the pause menu's Quit to Title open (`Demo?Title`); it also
   drops any story run and hand-off carry left over.
2. **New Game** starts the story at its start scene (`StartScene` on the game mode, empty: the
   story's start). **Continue** loads the slot and resumes at the scene it was saved at.
   **Settings** opens the settings overlay (text speed, auto-advance, skip, volumes, assists,
   subtitle size, reduce flashes, and the QTE, hold and skip-choice assists; see DIALOGUE_UI.md) over the title; Esc or its Back row closes it
   and writes `Saved/Settings.json`. The same overlay opens from the pause menu's Settings row (see
   Pause). **Quit** closes the game at once. It has no confirm step: nothing is open on the title,
   and the story autosaves at every scene start.
3. **Story.** The game mode switches to `AFTStoryHUD` and spawns the director, as before.
4. **End.** When the story finishes, the end card shows. After a short delay (`EndCardDelay`, 0.6 s)
   Space, Enter, click or pad A reopens `Demo?Title`.

Returning from a battle or field screen (story running, carry valid) never shows the title.

## Controls (title)

| | Keyboard and mouse | Gamepad |
|---|---|---|
| Move the selection | Up / Down, W / S | d-pad up / down (left / right too) |
| Pick the selected item | Enter, Space or left click | A or Start |
| Select with the mouse | move the mouse over an item, then click | |
| Shortcuts | 1 New Game, 2 Continue, 3 Settings, 4 Quit | |

Pad Start confirms on the title only. The story context no longer maps it (a gameplay screen's
Start is the pause key), so the menu input context does (`BeginMenuInput`); the pause context, which
the title never has, would take it first.

Continue is greyed when there is no usable save: the selection can't land on it (Down from New Game
steps over it to Settings) and its shortcut does nothing. With a usable save the selection starts on
Continue. Settings and Quit are always available. The rules are `ftcore::TitleMenu`
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
`GetRevision()`, read `Get()`). The QTE screen reads the QTE and hold assists when it starts, and
every change of a setting re-mirrors the three assist options onto the story's `F_SYS_OPT_*` flags
(`UFTStorySubsystem::ApplyOptionFlags`). Headless runs keep their own text speed and skip, but the
file is still loaded, so the option flags are mirrored from it (QTE auto-complete stays off under
autoplay either way).

## Pause

Esc or pad Start pauses every gameplay screen: the story, exploration, battle, choice, banter,
quick-time, mini-game, cutscene and stub screens. The title has no pause (Start confirms there, and
Esc closes its settings overlay). `UFTPauseSubsystem` (a game instance subsystem) holds the menu,
`ftcore::PauseMenu` (`Core/include/ftcore/PauseMenu.h`, tests in `test_pause.cpp`), and each HUD draws
it at the end of its `DrawHUD` through `FT::DrawPauseOverlay` (`FTPauseOverlay.*`): a dimmed screen and
a centred panel with Resume, Settings, Quit to Title and Quit Game, or in their place the Yes / No
prompt of a quit item. The same call draws the settings overlay on top when that is open.

| | Keyboard and mouse | Gamepad |
|---|---|---|
| Open | Esc | Start |
| Move the selection | Up / Down, W / S, or the mouse | d-pad up / down (left / right too) |
| Pick | Enter, Space or left click | A |
| Back (closes the quit prompt; resumes from the list) | Esc, Backspace | Start, B |
| Yes / No in the quit prompt | Up / Down, Left / Right, or the mouse | d-pad |

- **Layers.** Esc and Start back out of the topmost layer first, in this order: the settings overlay
  (it opens over the menu); the pause menu itself (the quit prompt closes, else it resumes); a layer
  of the screen (the story's backlog, an aim in progress in the battle, which puts the shot back);
  and only then they open the pause menu. `AFTPlayerController::HandlePause` does the routing. A
  screen with a layer of its own listens to the controller's `OnBack` and calls `ClaimBack()` when it
  closes one. Esc is no longer a cancel on a gameplay screen and Start no longer confirms: the pause
  context (priority 3, `BeginPauseInput`, on every screen but the title) consumes both. Backspace and
  pad B are still cancels. In the story Esc used to open Settings directly; it opens the pause menu
  now, and Settings is one row down.
- **Items.** Resume; Settings (the settings overlay opens on top, and its Back returns to the list);
  Quit to Title and Quit Game, which ask first ("Quit to the title screen?" / "Quit the game?", with
  No selected; Up / Down flip Yes and No, and Back is a No). Back on the list resumes. Greyed items
  are skipped like the title's (`PauseMenu::setEnabled`), so a screen can grey one; none does yet.
- **Quit to Title** unpauses, then opens the title the way the battle's own "Quit to title" does:
  the story carry's return map when a screen was handed the story, else the game mode's `TitleMap`,
  with `?Title`. The title then drops the story run and the hand-off carry, and the field's carry is
  cleared. The progress since the last autosave (the start of the scene) is lost. **Quit Game**
  calls `UKismetSystemLibrary::QuitGame`.
- **What freezes.** The world: actors, timers, the walk and its followers, the explore input, and
  every screen timer that runs on game time (the quick-time, mini-game, banter, choice, stub and
  cutscene shot clocks). The story (`UFTStorySubsystem::SetPaused`, since its hold timer ticks outside
  the world's pause). The music, the voice line and the ambience (`UFTAudioSubsystem::SetGamePaused`;
  they are UI sounds, which the world's pause doesn't reach), so the audio cursor the conductor
  follows stops. And the wall clock, below.
- **The wall clock.** The battle clock is wall time, not world time: `UFTBattleComponent` follows the
  conductor's song time, and the conductor extrapolates it from the platform clock. Pausing the world
  alone would leave every pending combo note and defence hit to be judged late on resume.
  `UFTPauseSubsystem::WallSeconds()` is platform time minus all the time spent paused (process-wide,
  across maps), and the gameplay timing reads it: the conductor (`FTConductorComponent`), the battle
  component's no-music clock, the controller's tap time (`GetEventSongTime` reads the same clock), the
  cutscene director's real-time clock (its autoplay cap and skip grace), and the battle screen's own
  banner, popup, judgement and results-delay times (`AFTBattleDemoActor::Wall()` stamps them and
  `AFTBattleHUD` ages them, so both read it). Every timestamp compared with it has to come from it.
  `AFTExploreDirector` keeps `FPlatformTime`: its clock only ages notices and banners and feeds
  elapsed-time logs, so a pause makes a notice fade early and counts in a log.
- **Input.** While the menu is open the controller adds a menu context at priority 5, above every
  screen's own context, with up / down / left / right, advance, confirm and cancel on the same actions
  the menu context uses, so a screen's context (the explore pawn's Move, Jump and Interact) can't
  take a key the menu is waiting for. The actions the menu and the settings overlay read have
  `bTriggerWhenPaused` set. The screens ignore their own input while it is open: they check
  `BlocksInput()` where they check the settings overlay's (the story director, the end card, and the
  battle, banter and choice screens). It stays true for the rest of the frame the menu closed in, so
  the press that closes it doesn't also reach them. The screens that read held keys by polling (the
  battle's aim, the quick-time and mini-game screens) skip edges in the frame after it closes
  (`JustResumed()`), so the press that picked Resume isn't read as theirs.
- **Headless.** The menu never opens in a headless run: `-StoryAutoPlay*`, `-AlphaAutoPlay`,
  `-StoryScene=`, `-ExploreAutoPlay`, `-BattleAutoPlay`, `-WorldAutoPlay`, `-OverworldAutoPlay`, or a
  hand-off carry on autoplay. A map load closes it, undoes what it froze and puts every item back on.

Log lines (no text): `Pause: opened.`, `Pause: resumed.`, `Pause: settings.`, `Pause: quit to title.`,
`Pause: quit game.`, `Pause: closed by a map change.`

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
- The save text also holds `"party": {"xp": N, "gold": N, "items": [{"id": "...", "count": N}]}`
  (`StorySave::party`, `Progression.h`), the party's experience, gold and carried items from won
  fights and the shop. A save without it loads with xp and gold at 0, and one without `items` loads
  with an empty inventory (`Inventory.h`). New Game resets them; Continue restores them.

## Command line

| Flag | Effect |
|---|---|
| `-StoryAutoPlay`, `-StoryAutoPlayFast`, `-StoryAutoPlayHandoff`, `-StoryScene=<id>`, `-AlphaAutoPlay` | Headless: skip the title, start the story at once, never return to the title at the end, never open the pause menu, and never write the save slot (so a check can't overwrite a player's save). `-AlphaAutoPlay` also logs `Title: New Game (autoplay).` |
| `-ExploreAutoPlay`, `-BattleAutoPlay`, `-WorldAutoPlay`, `-OverworldAutoPlay` | The screens' own autopilots: they never open the pause menu either. |
| `-StoryQuitOnFinish` | Quit at the end (headless behaviour), no return to the title. |

Log lines: `Title: shown, continue on|off (<code>).`, `Title: New Game.`, `Title: Continue.`,
`Title: Settings.`, `Title: Quit.`, `Title: end card up.`, `Title: back to the title.`, `Save: slot written at scene <id>.`,
`Settings: loaded.` / `Settings: no file, using defaults.` / `Settings: opened.` / `Settings: closed.`,
and the `Pause:` lines above.

## Files

`FTGameMode.*` (states, input handling, end return), `FTTitleHUD.*` (drawing),
`FTStorySubsystem.*` (slot read / write, autosave, backlog and seen bits), `FTStorySaveGame.*` (the
save object), `FTSettingsSubsystem.*` and `FTSettingsOverlay.*` (settings file, overlay state and
drawing), `FTPauseSubsystem.*` (the pause menu's state and the pause-aware clock) and
`FTPauseOverlay.*` (its drawing), `FTPlayerController.*` (menu and pause contexts), Core `TitleMenu.h`,
`PauseMenu.h`, `Settings.*`, `Seen.*` and `StorySave.*` (`checkStorySave`, `saveRefusalName`).

## What's missing

- A 3D menu backdrop with the leads behind the title and the pause menu. Both draw over a plain dim
  today. It is editor and GPU work: a lit map with the leads posed, the menu drawn over it.
- Save slots. There is one autosave slot, `Slot0`.
- Credits.
