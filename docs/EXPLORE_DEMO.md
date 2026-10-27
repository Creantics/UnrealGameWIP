# Exploration demo

A playable slice of field exploration: walk Devin around a walled village blockout, talk to Duy,
collect three light shards, open the old gate and walk into an ambush. The ambush is the battle
demo (see [BATTLE_DEMO.md](BATTLE_DEMO.md)); after a victory the game returns to the field, the
ambush is cleared and the demo is complete. The rules all live in Core (`ftcore::Exploration`,
`Core/include/ftcore/Explore.h`); the spots, objectives and autopilot are in
`Core/include/ftcore/ExploreDemo.h`.

Map: `/Game/Maps/Explore` (Content/Maps/Explore.umap). Open it and press Play. The map's game mode
is `AFTExploreGameMode` (no story; pawn `AFTExploreCharacter`, HUD `AFTExploreHUD`). The project's
default map is unchanged.

## Flow

1. **Talk to Duy.** He waits by the path from the start. Walk up, face him and press E.
2. **Find the light shards** (3). They glow and bob around the village; they unlock after
   talking to Duy.
3. **Open the old gate.** The gap in the inner wall is barred until all three shards are found.
4. **Win the ambush.** Two enemies wait in the courtyard beyond the gate. Walking into them opens
   the battle map; win and the field comes back with the ambush cleared and "Demo complete".

**Guards.** Two guards walk loops around the east and south shards. They aren't objectives, but
one that touches you starts the same fight as the ambush; win it and that guard is gone for the rest
of the demo. See "Field guards" below.

The objective list sits top left; the prompt for whatever you face shows near the bottom. Locked
spots say which objective they wait for.

## Controls

| | Keyboard and mouse | Gamepad |
|---|---|---|
| Move | W A S D or arrow keys | Left stick |
| Look | Mouse | Right stick |
| Run | Left Shift (hold) | Left trigger (hold) |
| Jump | Space | A |
| Use (talk, pick up, open) | E or Enter | X |
| Switch lead (with a party in config) | Tab | RB |
| Show or hide the followers | P | LB |

The keys are an Enhanced Input context made at runtime (`AFTExploreCharacter`), priority 2, above
the story context.

## How it fits together

- `AFTExploreDirector` (one per map) holds the `ftcore::Exploration`. Each tick it turns the pawn's
  position (cm to m) and facing into Core's `focus` and `encounterAt`, highlights the focused
  spot and handles Use.
- `AFTExploreSpot` is the visible side of one entry in `kExploreSpots` (chosen by `SpotId`): Duy
  with his idle clip, a shard, the gate (pillars and a bar that sinks), or the waiting enemies.
- The fight round trip goes through `UFTGameInstance::ExploreCarry`: the director stores the field
  state, the pawn's transform and the encounter id, then opens `BattleDemo`. The battle sets
  `bWon` on victory and opens the field again. The director restores everything, clears the
  encounter and puts the pawn back where it was.

## Field guards

The rules are `ftcore::GuardSet` (`Core/include/ftcore/Guard.h`, tests in `test_guard.cpp`); the
demo's two routes are `ftcore::demo::kGuardSpecs`.

- **States.** Idle (a 1 s pause at each route point), Patrol (1.5 m/s), Notice (stops and faces you
  for 0.5 s), Chase (4 m/s: faster than a walk at 3.3 m/s, slower than a run at 6.2 m/s), Return
  (back to the route point it was walking to).
- **Seeing you.** Inside an 8 m cone of about 100 degrees, or within 2.5 m whatever its facing.
  While chasing only the distance counts (12 m). Core does the cone and the distances; the line of
  sight is a line trace on the Unreal side (`AFTFieldGuard::CanSee`, eyes at 160 cm to the middle
  of the pawn), done only for guards close enough to need it. A chase out of sight for 2 s gives up
  and the guard walks back.
- **Contact** (closer than 0.8 m) shows "Caught!" and opens `BattleDemo` after the same 1.2 s as
  the ambush. The carry holds the guard's id (`ExploreCarry.Guard`, with `Encounter` at -1) and the
  defeated guards (`ExploreCarry.Guards`). A win removes the guard; on any return to the field every
  guard ignores you for 3 s (`kGuardHoldOffSeconds`) and the others start their routes again. A loss
  stays on the battle screen (retry or quit), as for the ambush, and there is no flee. Demo fights
  pay no xp or gold: party progression belongs to the story.
- **On the map.** `AFTFieldGuard` is a blockout figure: a tinted column (dark red; amber while it
  notices you, red while it chases, with a marker over its head) and a visor that shows its facing.
  The director spawns the guards at run time, so the map needs no rebuild. They have no controller,
  movement component or collision: Core places them, they can't block the pawn or the autopilot, and
  a chase goes straight through props. Their routes keep 0.5 m clear of every prop (a Core test).
- **Not on story field steps.** A field step from the story spawns no guards, so the alpha run's
  counts don't change.
- **Autopilot.** The pilot ignores the guards. In Core's walk (`test_guard.cpp`, at walk speed) the
  south guard catches it once and the east one has its back turned; on the map the count can differ
  (navmesh path, acceleration), and each catch is one more battle-demo fight before COMPLETE. The
  headless run on 2026-10-25 matched Core: one guard fight (the south guard, guard 1, caught it at
  19.6 s), won, then the ambush fight.

Log lines: `ExploreDemo: 2 guards on patrol.`, `guard <i> noticed you.`, `guard <i> gives chase.`,
`guard <i> lost you.`, `guard <i> caught you after <s> s.`, `back from the fight, guard <i> defeated.`

## From the story

A field step whose scene is placed in one of the three big worlds plays on that world's map instead, in its district (see `WORLDS.md`); everything below applies there too, moved into the district's frame.

A field step (ENG or PLAY, `StoryCarry.Screen == Field`) opens this map with
`UFTGameInstance::StoryCarry` set. `AFTExploreDirector` hides the demo spots, skips the encounters
and objectives and shows ONE goal marker (a glowing column and orb; `AFTExploreSpot` with
`SpotId == ftcore::demo::kFieldGoalSpotId`, spawned at run time, so the map needs no change). Its
position comes from `Data/Field/goals.json` (`ftcore::FieldGoalTable`, keyed by scene, beat and step: a goal
and an optional `via` waypoint the autopilot passes first; every step of the script has an entry, all on clear
ground and reached in under 10 s). When the file or the entry is missing it falls back to
`ftcore::demo::fieldGoalIndex(scene, beat, step)`, a deterministic pick from
`kFieldGoals` (six points, each a straight prop-free line from the start). The HUD reads
"Reach the marker  <ENG|PLAY>  <step label>" (notation only). Standing at the marker, the prompt is
"E  Use the marker"; pressing it sets `StoryCarry.bDone` and opens `ReturnMap`. Fallback: after
`kFieldFallbackSeconds` (5 min) the prompt becomes "E  Back to the story" and works anywhere.
The rules (`fieldGoalInReach`, `fieldStepUsable`, `fieldPilotStep`) are in `ExploreDemo.h`, tested
in `test_explore.cpp` (every goal reachable by the pilot in under 10 s, no prop on the line).

**The party follows.** On a field step the rest of the party (`ftcore::demo::kPartyNames` after
the first, the battle's line-up, so the field and the fights agree; at most three) walks behind
the lead. The director keeps an `ftcore::PartyTrail` (`PartyTrail.h`): the lead's ground positions
sampled every 0.25 m, so a long or fast-forwarded frame still leaves the corners in the path. Each
follower is placed, not steered: member i stands i x 1.6 m behind the lead along that path, so it
moves exactly when and as fast as the lead did and never falls further behind. The trail starts as
a straight line behind the lead's facing, so the party starts in a line. A follower
(`AFTPartyFollower`) is the member's skeletal mesh from `/Game/Characters/<Name>/<name>/SkeletalMeshes/`
with its idle and walk clips, set down on the ground found below it. It has no controller, no
movement component and no collision, so it can't block the lead or the autopilot. Log:
`ExploreDemo: <n> following the lead.`

**The party in config.** `PartyLooks` on `AFTExploreCharacter` (under
`[/Script/ForgottenThird.FTExploreCharacter]` in the local `Config/UserGame.ini`) lists the party in
order, each member with a body, clips, pieces and grooms (`FFTLeadLook`, `FTLook.h`; an empty path
uses the member's own asset above):

```
+PartyLooks=(Name="Duy",BodyMeshPath=/Game/...,IdleClipPath=/Game/...,WalkClipPath=/Game/...,PiecePaths=(/Game/...),Grooms=((Groom=/Game/...,Binding=/Game/...)))
```

The pawn wears one member's look and the others follow in order after it. Tab or RB switches to the
next member: the pawn changes look and the followers line up again behind it. P or LB hides or shows
the followers, to see the lead alone (hidden followers keep walking the trail). `StartLead=<Name>` and
`bStartSolo=True` set the start; `-ExploreLead=<Name>` and `-ExploreSolo` do it for `-game` runs, and
`-ExplorePortrait` hides the followers. The player's last choice is kept across a fight
(`UFTGameInstance`). With a party in config the demo walk has followers too, so the demo map's own
Duy (a talk spot) can stand beside a following Duy. Without one, the pawn wears its single look
(`BodyMeshPath` and the rest) as `kPartyNames[0]`, and only the story's field steps have followers.
The log line per look is `FTExploreCharacter: <Name>: body <mesh>, <n> pieces, <c> with cloth,
<g> grooms.`

On autoplay (`StoryCarry.bAutoPlay`) the autopilot walks to the marker and uses it, logging
`ExploreDemo: field step done at <scene> beat <b> step <i> after <t> s.` (game time, under 10 s).
With `-AlphaFast` global time dilation is 8 while it plays. The same stuck detection applies
(`ExploreDemo: STUCK`). If you move a prop or marker, keep `kFieldObstacles` in step with the map
builder. See `DIALOGUE_UI.md`.

## Regenerating the map

The map is generated by a commandlet; don't edit it by hand.

```
UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTBuildExploreWorld -NullRHI -unattended -nopause -nosplash
```

`-Map=<name>` writes another map under `/Game/Maps` (default `Explore`); `-Style=<spec>` sets the
style controller (default `solhaven`, `none` for no controller). The props stay off the straight
lines the autopilot walks between spots, so check those lines when moving spots or props.

## Headless checks

- Core: `test_explore.cpp` covers focus, locks, encounters, save and load, and walks the whole demo
  with Core's pilot (every spot in order, under 60 s, no allocation).
- Unreal end to end (the autopilot walks to each objective, uses it, fights the ambush with the
  battle autopilot and returns):

```
UnrealEditor-Cmd.exe ForgottenThird.uproject /Game/Maps/Explore -game -NullRHI -ExploreAutoPlay -ExploreQuitOnComplete -unattended -nosplash -nosound
```

It logs `ExploreDemo: COMPLETE after 26.6 s.` (time in the field; fights are not counted) and
exits, with one `used spot` line per spot and a battle demo `VICTORY` line for each fight (the guard's and the ambush's). `ExploreDemo: STUCK` means the autopilot
made no progress toward a spot for 15 s (a prop in the way).

## What's missing

- **Blockout art.** Houses, trees and walls are engine basic shapes in flat tints. The enemies in
  the courtyard are cubes.
- **No dialogue.** Talking to Duy only counts toward the objective; there is no conversation yet.
- **One fixed fight.** The ambush always opens the battle demo's line-up. Encounter data doesn't
  come from `Data/` yet.
- **No saving to disk.** The field state survives the fight in the game instance only.
- **Simple autopilot.** It follows the navmesh path the map builder saves (see ALPHA.md), and walks
  straight only when there is none.
- **Guards are blockouts.** No enemy model, no walk clip, and they walk through props while chasing.
- **Followers on the field demo.** Without a party in config the demo's own walk has no followers.
  With one, the talk spot's Duy is still the old model and can appear next to the party's Duy.
