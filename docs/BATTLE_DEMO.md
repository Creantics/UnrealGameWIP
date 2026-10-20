# Battle demo

A playable version of the rhythm fight system: two party members (the leads Devin and Duy) against one
to three enemies (by default Brute and Shade; placeholder shapes), with a canvas HUD. The leads loop their idle clips. The rules all live in Core
(`ftcore::Battle`); the line-up, tuning and autopilot are in `Core/include/ftcore/BattleDemo.h`.

Map: `/Game/Maps/BattleDemo` (Content/Maps/BattleDemo.umap). Open it and press Play. The map's
game mode is `AFTBattleGameMode` (no story; HUD `AFTBattleHUD`). The project's default map is
unchanged.

## Flow

1. Turn order comes from the speed-gauge turn queue (shown top left: NOW, then NEXT).
2. On a party turn, pick a skill and a target. The skill's arrow combo then scrolls up the
   centre lane on the 120 BPM beat grid; step on each arrow as it reaches the hit line.
   Each note is graded PERFECT / GREAT / GOOD / MISS (WRONG for the wrong arrow), and the
   grades scale the damage. A full combo adds a bonus.
3. On an enemy turn there is a short windup ("Name: Skill - defend!"), then a defence combo:
   good steps block damage. Blocking nearly everything counters.
4. Downed figures fall over. The battle ends in VICTORY or DEFEAT. After a second,
   Enter / Space / pad A starts a new fight.

## Controls

| | Keyboard | Gamepad |
|---|---|---|
| Arrows during a combo (lanes Left, Down, Up, Right) | Arrow keys or A S W D | D-pad, or X A Y B |
| Choose a skill (menu) | Up / Down, or 1-4 to use at once | D-pad up / down |
| Choose a target (menu) | Left / Right, Q / E, Tab | D-pad left / right, shoulders |
| Use the skill (menu) | Enter or Space | A or Start |
| Fight again (after the outcome) | Enter or Space | A or Start |

The battle keys are a separate Enhanced Input context (`AFTPlayerController::BeginBattleInput`),
active only while a battle runs. It sits above the story context, so the shared keys don't reach
the story.

## From the exploration demo

The exploration demo's ambush opens this map (see [EXPLORE_DEMO.md](EXPLORE_DEMO.md)). It leaves
the field's state in `UFTGameInstance::ExploreCarry`; on VICTORY the battle shows "Returning to the
field" and opens the field map again after 2.5 s. A DEFEAT stays here for a retry. With a carry set,
`-BattleQuitOnOutcome` doesn't quit, and `-ExploreAutoPlay` also turns on the battle autopilot.

## From the story

A fight (CMB) step in the story opens this map with `UFTGameInstance::StoryCarry` set. The enemy
line-up comes from data (below). A win shows "Returning to the story" and opens the story map again
after 2.5 s, where the story picks up after the beat.

A loss stays here. The DEFEAT screen offers **Enter: Retry** and **Esc / Q: Quit to title**
(Esc, Backspace and pad B come through the controller's cancel; Q is polled). Retry rebuilds both
sides from the line-up, so the party starts again at battle-start HP, and replays the same
encounter. Quit opens the story map (`carry.ReturnMap`) with the URL option `?Title` (the story
map's game mode shows the title screen on it) and leaves `carry.bDone` false. A plain demo fight
(no carry) and an exploration ambush only offer the retry.

The autopilot runs the fight when the story was on autoplay with `-StoryAutoPlayHandoff` (or
`-AlphaAutoPlay`). With a carry it always retries after a loss, so a headless run never stops on a
defeat.

## Line-up from data

`Data/Battle/enemies.json` is the enemy roster (id, placeholder name, stats, skills) and
`Data/Battle/encounters.json` maps a story step (scene id, beat index, plan step index) to a line-up
of enemy ids. Both hold ids and numbers only. The loader and the rules are in
`Core/include/ftcore/Encounter.h`.

- The battle looks up `StoryCarry.Scene`, `.Beat`, `.Step` and falls back to the table's `default`
  (the original Brute and Shade) when there is no carry or no entry. If the files can't be read it
  uses the built-in demo line-up (`ftcore::demo::enemies()`).
- The party is always the two leads from `BattleDemo.h`. A line-up has one to three enemies (the
  HUD stacks three panels).
- `tier` (0 to 9, on an entry or on `default`) makes a fight tougher: every tier adds 8 percent of
  each enemy's base max HP and attack. Later acts use higher tiers.
- Roster: brute, shade, wisp (fast, fragile), hound, bulwark (slow, tanky), lurker (hits hard).
  The entries are keyed by real scene ids and real battle steps; a Core test checks that against the
  script when it's available.

Add a fight: add a roster entry if needed, add an `encounters` entry, then run the Core tests. The
balance gate in `test_encounter.cpp` plays the default line-up and every entry headless: perfect
presses must win (also at a coarse 130 ms frame step, the dilated frame length) and no presses must
lose.

## Alpha fast

With a carry on autoplay and `-AlphaFast` on the command line (the same switch as
`UFTGameInstance::IsAlphaFast()`), the battle sets global time dilation 8 and restores it when it
ends. The silent song clock follows game time in that case
(`UFTConductorComponent::bWallClockFollowsGameTime`), so the beat grid, combo timing and the
autopilot's presses stay in step; the autopilot presses at each note's own time
(`UFTBattleComponent::PressAtTime`), so a long frame doesn't make PERFECT late. The result line
gives real and game seconds: `BattleDemo: VICTORY after 7.6 s (game 61.0 s).`

## Regenerating the map

The map is generated by a commandlet; don't edit it by hand.

```
UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTBuildBattleDemo -NullRHI -unattended -nopause -nosplash
```

## Headless checks

- Core: `test_encounter.cpp` is the balance gate over every encounter (see above).
  `test_battle_demo.cpp` plays the whole demo fight headless. With perfect presses it ends in
  victory (11 party turns, 5 defences, 61 s of battle time). With no presses it ends in defeat.
- Unreal end to end (the autopilot picks skills and steps on every note):

```
UnrealEditor-Cmd.exe ForgottenThird.uproject /Game/Maps/BattleDemo -game -NullRHI -BattleAutoPlay -BattleQuitOnOutcome -unattended -nosplash -nosound
```

It logs `BattleDemo: VICTORY after 61.5 s (game 61.5 s).` and exits (no carry, so the default
line-up and no dilation).

## What's missing

- **No music.** The song clock is a silent 120 BPM wall clock
  (`UFTConductorComponent::bWallClockWithoutAudio`). Bind a track with `BindAudio` to sync to
  real audio. Latency calibration exists but isn't wired into the demo.
- **Placeholder enemies.** The party are the lead meshes looping their idle clips; the enemies are
  engine cubes. There are no attack or hit animations, only a lunge, a squash-and-flash on hits
  and a topple when downed. Without the lead meshes the party falls back to basic shapes.
- **Placeholder encounters.** Only 14 steps have their own line-up, all tuned only against the
  autopilot and the no-input run; the rest play the default fight.
- **Input timing.** Presses are judged at the time the frame handles them, not at the hardware
  event time, so low frame rates cost accuracy.
- **Missing actions.** No items, flee, guard or party switching. Enemy AI is Core's simple
  round-robin skills against the most wounded target.
- **Minimal HUD.** The UI is a canvas HUD with engine fonts, not UMG.
