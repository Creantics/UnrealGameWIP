# Beat fight screen

The screen that a fight step in a beat world opens (`CombatKind::Beat` in `ftcore::CombatKind`; the routing is in [COMBAT_SYSTEMS.md](COMBAT_SYSTEMS.md), the rules in `Core/include/ftcore/BeatFight.h`). The enemies take turns, one bar each, on the music's beat grid. Each eighth note of a bar can hold a prompt (a strike, a counter or a hold) over an enemy, a ring closes in on it, and the player answers with the matching action button as the ring lands. A defeat offers Retry or Give up; a victory goes back to the story.

## Regenerate the map

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildBeatWorld [-Map=Beat] -NullRHI -unattended -nopause -nosplash
```

Writes `/Game/Maps/Beat`: a lit room (floor, back wall, sun, sky light, one lamp), a camera at the near end looking down it, a player start behind the camera and one stand-in for each enemy a fight can have (`ftcore::kMaxBeatEnemies`, 4). A stand-in is a cylinder body in its own tint with a sphere head and a flat visor on its front, so the way it faces shows. They stand in a shallow arc around the camera and face it. The body of each carries the tag `BeatEnemy0` ... `BeatEnemy3` (`AFTBeatGameMode::StandInTag`). `AFTBeatGameMode` is the map's game mode. An existing map file is deleted first. The map is generated, never hand-made.

`FTStep::Prepare` opens the map for every fight step in a beat world once it exists. Until it does, those fights play the rhythm battle and the dispatcher logs one warning per run that names `FTBuildBeatWorld`.

## Flow

1. The dispatcher fills `UFTGameInstance::StoryCarry` (with `Combat` set to `Beat`) and opens `/Game/Maps/Beat`.
2. `AFTBeatGameMode::StartPlay` (it skips `AFTGameMode::StartPlay`: no story on play) reads the carry's scene, gameplay beat index, plan step index and `bAutoPlay`, loads `Data/Battle/beat.json` and picks the step's fight (the table's `default` fight when the step isn't listed). A missing file logs one line and plays the built-in default fight; a refused one logs one warning with the loader's errors (paths and keys only) and does the same. The quick-time assists come from the settings, as on the QTE screen: auto-complete lets the autopilot play while the player watches, hold toggle latches a hold on its press.
3. It finds the stand-ins by their tags, seats as many as the fight has enemies (the same arc, centred on the camera), hides the rest, makes the map's camera the view and chooses the clock (below).
4. The fight begins on the first tick, once the clock runs, so a hitch while the map loaded can't eat the lead-in. The seed is a hash of the scene id, beat and step, so a step always plays the same fight; a retry plays it again from the top (the same patterns in the same order).
5. Each frame it polls the keys, feeds `ftcore::BeatScreen` the presses and releases with their event times, then ticks it with the clock. `AFTBeatHUD` draws the markers.
6. A defeat shows for two seconds, then asks: Retry restarts the same fight, Give up ends the step lost. A victory shows for two seconds. Then it sets `bDone` (and `bWon`: true for a victory, false after Give up), logs one line and lets `FTStep::ContinueHere` start the beat's next step in place (no host plays a step in place on this map yet, so today it opens `ReturnMap`, as the QTE screen does).

The screen never counts the story step: the dispatcher counts every step once (`FTStep::FinishCarried`), won or lost.

Opened without a story step (the map on its own, or after the story handed its carry back) the screen plays the table's default fight and, when it ends, stays on the result. It has nowhere to go back to.

## The clock

The fight runs on one clock, `UFTConductorComponent`'s song time, and the rule is:

- **Music.** When the fight (`beat.json`) names a music cue, and the audio subsystem is playing that cue, and the cue has a tempo, the conductor is bound to it (`BindMusic`). The fight's clock is the song's, and presses are stamped with `songTimeForEvent` (latency compensated), as the rhythm battle's are. The fight's own `bpm` and `offset` place the grid, so they must match the cue's; a mismatch is logged as a warning. The screen never starts or stops music: the story's cue carries over the hand-off.
- **Wall clock.** Otherwise (no cue named, a named cue that isn't the one playing, no tempo, `-nosound`, headless) the conductor runs on `UFTPauseSubsystem::WallSeconds()`, which stands still while the pause menu is open.
- **`-AlphaFast`.** A story step on autoplay under `-AlphaFast` binds no music (it isn't dilated). The conductor's clock follows game time (`bWallClockFollowsGameTime`, then `ResetClock()`) and the world runs at global time dilation 8, as the battle's does. The dilation is put back when the screen finishes.
- A bound music clock that has not reported its first position after two seconds gives way to the wall clock, with a warning.

Which clock it chose is logged once, when the map starts:

```
Beat: clock: song time of music cue <cue id> (<bpm> bpm).
Beat: clock: wall clock (the fight names no music cue).
Beat: clock: wall clock (music cue <cue id> isn't playing).
Beat: clock: wall clock (music cue <cue id> has no tempo or no sound).
Beat: clock: game time (wall clock following it), -AlphaFast, game time x8.
```

The pause menu freezes all of it: the world, the wall clock and the music stop together, so the fight's clock does not move.

## Keys

The buttons are the QTE screen's (`ftcore::ActionButton`, `FTActionButtons.h`): Attack is J or the pad's face left, Guard I or face top, Dodge L or face right, Jump K or face bottom. Beat prompts never take arrows (those are the Turns battle's combos); in `beat.json` a pattern names its button by letter: `A`, `G`, `D` or `J` (`sA . sJ . cD . . .`). Keys are polled each frame (so a hold sees its key go up) and a press is stamped with the clock's latency-compensated time for the frame. Input is ignored for the first 0.25 s after the fight starts or restarts, so the press that opened the screen, or the one that chose Retry, can't count. Escape (keyboard) or Start (pad) opens the pause menu (see [GAME_FRAME.md](GAME_FRAME.md), "Pause"): a key still held in the frame after the menu closes is not read as a press until it is let go and pressed again.

At the Retry prompt (it ignores the first half second, so the press that ended the fight can't pick):

| Pick | Keys |
|---|---|
| Retry | Enter, Space, pad A |
| Give up | Q, Backspace, pad B |

## Flags

For checking the map on its own, headless:

```
UnrealEditor-Cmd.exe ForgottenThird.uproject /Game/Maps/Beat -game -NullRHI -BeatAutoPlay -BeatQuitOnFinish -unattended -nosplash -nosound
```

- `-BeatAutoPlay`: the autopilot plays the fight. (A story step also autoplays with `StoryCarry.bAutoPlay`.)
- `-BeatQuitOnFinish`: a fight on its own exits when it ends, with code 0 for a victory and 1 otherwise. It does nothing for a story step.

The pause menu stays available under both: `UFTPauseSubsystem::IsHeadlessRun()` does not know them.

## Log lines

One line per fight, ids and counts only, never text:

```
Beat: VICTORY at <scene> beat <b> step <s> after <t> s, <perfect> perfect, <good> good, <miss> miss, best chain <c>, <r> restarts.
Beat: DEFEAT at <scene> beat <b> step <s> after <t> s, ...
```

`<t>` is the clock the fight spent playing over every try; the counts and the restarts run on through retries. A fight on its own logs `standalone` for the scene. Under autoplay a defeat that survives all three retries also logs a warning and adds one to the alpha run's warning count:

```
Beat: autoplay lost <scene> beat <b> step <s> and its 3 retries; the step ends lost.
```

## HUD

All of it is canvas (no UMG), and drawing builds no strings: every word is a static `FTUiText` (the keys are in `Data/Loc/en/ui.json`) and a number is rebuilt only when it changes.

- **Markers.** Each open prompt hangs over its enemy: the stand-in's anchor (above its head) projected to the screen, or a place on the screen when the map has no such stand-in. A ring closes in on the target ring as `approach()` runs from 1 to 0, and the face-button diamond is inside, the prompt's button filled. A strike is cyan. A counter reads as a threat: red, with spikes around the ring and the word COUNTER, greyed and crossed through while counters are locked out. A hold is gold, with a bar as long as the hold that fills with `holdProgress()` once it is held. A finisher's strikes are gold with a second ring inside. An enemy's prompts that follow the first stand to its right, smaller and dimmer: only the first is the one to answer.
- **Panels.** Each enemy has a panel at the top left with its number and an hp bar, framed in gold while it is that enemy's turn (`turnEnemy()`) and marked DOWN once it is down (its stand-in leaves the stage too). The lead's hp is at the bottom left. At the bottom right the chain and the multiplier, which swells when it changes.
- **Lead-in.** The beats left until the first bar, counted down in the middle of the screen.
- **Popups.** PERFECT, GOOD and MISS rise from the enemy as each prompt is judged; LATE after a counter that landed just before the press; LOCKED for a press the lockout ignored.
- **Flashes.** The screen's edge flashes red when the lead is hit and orange when a whiff locks counters out (LOCKED shows above the lead's panel until it ends). FINISH! rises when an enemy's next turn becomes a finisher.
- **Result.** VICTORY or DEFEAT in a band across the screen, with the fight's counts (perfect, good, miss, best chain). After a defeat the Retry and Give up lines follow.
- **Pause.** The pause overlay is drawn last, on every path out of the HUD, as every other screen does.

## Rules (Core)

`Core/include/ftcore/BeatScreen.h` holds `ftcore::BeatScreen`, the screen's state machine, as `QteScreen` is for the QTE. It owns the `BeatFight`, takes the fight's clock on every call, and has these phases:

| Phase | What it is |
|---|---|
| `NoStep` | Opened with nothing to play: inert |
| `Playing` | The fight runs |
| `Result` | Victory or defeat shows for 2 s (0.5 s under autoplay) |
| `RetryPrompt` | After a defeat: `choose(Retry)` restarts the same seed, `choose(Quit)` ends the step lost |
| `Done` | Finished; `won()` and `outcome()` say how |

Under autoplay it asks `autopilot(now)` until the autopilot has nothing to do, applying every press and release at its own time, and plays a long frame in steps of at most 0.1 s so the autopilot sees each bar before it falls due. It ignores input then, and a defeat restarts the fight on its own after the result, three times at most. After the third it ends the step lost and `gaveUp()` is true, which the host logs as a counted warning. Fixed capacity: nothing allocates after `begin()`. Tests are in `Core/tests/test_beat_screen.cpp`: autoplay wins the default fight at any frame rate and every fight in `beat.json`, a player who presses nothing reaches the Retry prompt, Retry restarts with the same seed, a screen with no step stays inert, and the input grace drops early presses.

## Headless behaviour and the alpha

- A story fight with `bAutoPlay` is played by the autopilot, which never misses, so it wins every fight in `beat.json` with nothing taken (the Core tests require it). Its clock is the wall clock, or game time under `-AlphaFast` (see the clock rule). The screen is quick: the fight itself, then half a second of result.
- A fight the autopilot loses (a data bug) retries three times and then ends the step lost with a counted warning, so an alpha run reports it instead of hanging.
- The step counts as a `battle` screen in the alpha tally like any fight; the dispatcher counts it, once, won or lost.
