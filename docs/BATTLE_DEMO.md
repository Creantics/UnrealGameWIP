# Battle demo

A playable version of the rhythm fight system: two party members (the leads Devin and Duy) against one
to five enemies (by default Brute and Shade; placeholder shapes), with a canvas HUD. The leads loop their idle clips. The rules all live in Core
(`ftcore::Battle`); the line-up, tuning and autopilot are in `Core/include/ftcore/BattleDemo.h`.

Map: `/Game/Maps/BattleDemo` (Content/Maps/BattleDemo.umap). Open it and press Play. The map's
game mode is `AFTBattleGameMode` (no story; HUD `AFTBattleHUD`). The project's default map is
unchanged.

## Flow

1. Turn order comes from the speed-gauge turn queue (shown top left: NOW, then NEXT). Under an
   enemy's soonest NEXT slot, in orange, is the skill it will use on that turn.
2. On a party turn, pick a skill and a target. The skill's arrow combo then scrolls up the
   centre lane on the 120 BPM beat grid; step on each arrow as it reaches the hit line.
   Each note is graded PERFECT / GREAT / GOOD / MISS (WRONG for the wrong arrow), and the
   grades scale the damage. A full combo adds a bonus. Instead of a skill the member can **Aim**
   (2 AP): the enemy's weak points show, and a shot on one deals bonus damage. See
   [BATTLE_AIM.md](BATTLE_AIM.md).
3. On an enemy turn the banner names the attack ("Name: Skill"), then its hits come in on the
   attack's timeline. Dodge, parry or jump each one (see Defence below). Parrying every hit
   counters.
4. A boss can change phase when its HP falls past a threshold (see Enemy phases below): a banner
   says so and its pattern changes.
5. Downed figures fall over. The battle ends in VICTORY or DEFEAT, and the results panel shows the
   fight's totals (see Results below). After a second, Enter / Space / pad A starts a new fight.

## Controls

| | Keyboard | Gamepad |
|---|---|---|
| Arrows during a combo (lanes Left, Down, Up, Right) | Arrow keys or A S W D | D-pad, or X A Y B |
| Dodge (enemy turn) | Left / Right, or A / D | D-pad left / right, or X / B |
| Parry (enemy turn) | Down, or S | D-pad down, or A |
| Jump (enemy turn, jumpable hits) | Up, or W | D-pad up, or Y |
| Choose a skill (menu) | Up / Down, or 1-4 to use at once | D-pad up / down |
| Choose a target (menu) | Left / Right, Q / E, Tab | D-pad left / right, shoulders |
| Use the skill (menu) | Enter or Space | A |
| Aim (menu row, or at once) | Aim row, F or right mouse | Aim row, or LT |
| Move the reticle (aiming) | Mouse, or hold the arrows / WASD | Either stick |
| Fire / cancel (aiming) | Left mouse, Enter, Space / Esc, right mouse | A / B, Start or LT |
| Fight again (after the outcome) | Enter or Space | A |
| Pause menu | Esc | Start |

Esc and Start pause the battle like every other screen (see [GAME_FRAME.md](GAME_FRAME.md), "Pause"):
the battle clock stops, and the press doesn't reach the battle while the menu is up. While aiming
they put the shot back first. Pad Start used to use the skill and fight again; it is the pause key now.

The defence keys are the combo arrows (`AFTPlayerController::OnBattleArrow`), read as defence inputs
while an enemy attack runs. The battle keys are a separate Enhanced Input context (`AFTPlayerController::BeginBattleInput`),
active only while a battle runs. It sits above the story context, so the shared keys don't reach
the story.

## Defence

An enemy skill is an attack timeline (`ftcore::DefenceTimeline`, `Core/include/ftcore/DefenceTimeline.h`):
the banner, then up to 8 hits at the offsets the data gives. The clock is the battle clock. With
music on (`BattleTuning::music`), the hits land on the song's beats: `ftcore::snapToBeat` moves each
hit later, to the first beat at or after its own time and after the previous hit's beat, so no hit
comes earlier than the data says, the hits stay in order, and two never share a beat. Quick hits
that the data puts less than a beat apart are spread a beat apart. Music off, the hits stay where
the data puts them. Each hit can have:

- a **tell**: the cue before the impact. The attacker flashes white as the tell starts. The HUD
  draws a ring that closes in on the defender and lands on a faint inner ring at the impact, with
  "!" (or "JUMP!" in gold on a jumpable hit);
- a **telegraph**: a longer warning for big hits. The HUD fills a red column next to the defender.

**The count.** With music on, the beats before the attack's first hit count down to it: a
`CountIn` event on each (`beat` = the beats left, down to 1), as many as `BattleTuning::countIn`
asks (2 by default; 0 for none) and only those that come after the banner. The hit lands one beat
after the last count. In Unreal, `UFTBattleComponent::OnCountIn` fires with the beats left, and the
HUD shows the number in cyan in the middle of the screen, one per beat.

Each hit is answered by one press inside its window, which ends at the impact:

| Input | Window | Notes |
|---|---|---|
| Dodge | 0.30 s | The last 0.06 s is a perfect dodge. No lockout, so mashing is safe. Not on jumpable hits. |
| Parry | 0.12 s | Always a perfect parry. A parry that answers nothing locks parry out for 0.4 s. |
| Jump | 0.25 s | Jumpable hits only (they also take a parry). A miss locks jump out for 0.3 s. |

Each hit reports PERFECT, DODGE, JUMP or HIT (a `Defended` event). A press up to 0.20 s after a
hit that landed (`DefenceWindows::late`, `FFTBattleTuning::LateWindow`) adds a second `Defended`
event, LATE. It changes nothing else, and it is also accepted in the recovery just after the attack.
The HUD shows a row of pips under the scene (gold perfect, cyan dodged or jumped, red hit, orange
hit with a late press). Above the pips one judgement line names the last hit: PERFECT (with the AP
refunded), DEFENDED, LATE or MISSED. A hit that lands also shows its damage number. A perfect parry or perfect dodge refunds 1 AP (`BattleTuning::apOnPerfect`, capped at
max AP). A hit that lands deals its weight's share of the blow. Parrying every hit of the attack
gives the defender a counter. The windows are `BattleTuning::defence` in Core and
`FFTBattleTuning` (DodgeWindow, PerfectDodgeWindow, ParryWindow, JumpWindow, ParryLockout,
ApOnPerfect) in Unreal.

Timelines come from the `hits` array of a skill in `Data/Battle/enemies.json`:

```json
{ "id": "crush", "hits": [ { "at": 2.0, "tell": 0.5, "telegraph": 1.5, "jumpable": true } ] }
```

- `at`: seconds after the banner, in (0, 30], required, strictly increasing.
- `tell`: 0.4 to 2 s (default 0.4; `kMinTell` is the floor, so no hit comes with less warning than that). `telegraph`: 0 to 10 s (default 0). Neither may be longer than `at`.
- `weight`: share of the blow, above 0 (default 1). `jumpable`: true or false (default false).
- 1 to 8 hits. A skill without `hits` (older data) gets one default hit at 1.2 s with a 0.4 s tell.

The autopilot parries every hit at its ideal time, read from the timeline (never from audio):
`ftcore::demo::pilotDefend` in Core, and the same loop in `AFTBattleDemoActor::TickAutoPlay`, which
looks up to two frames ahead and presses at the exact time (`UFTBattleComponent::DefendAtTime`).

The autopilot also aims: see [BATTLE_AIM.md](BATTLE_AIM.md).

### Poise and stagger

An enemy may have poise (`"poise"` in its `stats`, 0 to 1000; without it, 0, and it is never
staggered). Tiers leave it alone. Each counter (a fully parried attack) costs the attacker
`BattleTuning::counterPoise` poise (10 by default). At 0 it is staggered: its next turn passes
without an attack, its poise refills, and the next turn comes after the recovery pause. A staggered
enemy loses no more poise until then. Events (appended): `PoiseLost` (amount), `Staggered`, and
`Steadied` on the skipped turn (amount = poise restored); `BattleStats::staggers` counts them. The
HUD draws a thin gold poise bar over an enemy's HP bar and tags a staggered enemy. No enemy in the
shipped data has poise yet, so the balance gates are unchanged.

## From the exploration demo

The exploration demo's ambush opens this map (see [EXPLORE_DEMO.md](EXPLORE_DEMO.md)). It leaves
the field's state in `UFTGameInstance::ExploreCarry`; on VICTORY the battle shows "Returning to the
field" and the autopilot opens the field map again after 2.5 s; a player presses Enter to go back
once they've read the results. A DEFEAT stays here for a retry. With a carry set,
`-BattleQuitOnOutcome` doesn't quit, and `-ExploreAutoPlay` also turns on the battle autopilot.

## From the story

A fight (CMB) step in the story opens this map with `UFTGameInstance::StoryCarry` set. The enemy
line-up comes from data (below). A win shows the results with the reward and **Enter: Continue**,
which opens the story map again, where the story picks up after the beat. The autopilot continues
on its own after 2.5 s ("Returning to the story").

A loss stays here. The DEFEAT screen offers **Enter: Retry** and **Q / pad B: Quit to title**
(Backspace and pad B come through the controller's cancel; Q is polled; Esc and Start open the
pause menu, whose Quit to Title does the same, with a confirm step). Retry rebuilds both
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
- The party is always the two leads from `BattleDemo.h`. A line-up has one to five enemies
  (`kMaxLineupEnemies`; the loader refuses more). The HUD stacks up to five panels, and the stage
  staggers four or five enemies tighter so the row stays in frame. A test plays two line-ups of five
  (a pack of five weak enemies at level 1, and a mixed five at level 4) through the balance gate: won
  by the autopilot on fine and long frames, with parries and with dodges, and lost by an idle player.
- `tier` (0 to 9, on an entry or on `default`) makes a fight tougher: every tier adds 10 percent of
  each enemy's base max HP and 11 percent of its base attack. Later acts use higher tiers; the
  party's levels (below) keep pace with them.

### Experience, gold and levels

Each roster enemy has a `reward` (`{"xp": N, "gold": N}`, and optionally item drops: see Items). A
story fight that is won pays the line-up's rewards, plus 10 percent per tier (xp and gold only), to
the party (`ftcore::encounterReward` and
`ftcore::award` in `Core/include/ftcore/Progression.h`). The rewards in the roster follow
xp = round((maxHp + 6 × attack + 4 × defense) / 20) and gold = round(xp × 0.6); keep new enemies on
that rule.

- The whole party shares one level, 1 to 10, from total xp on a fixed curve. Each level above 1
  adds 8 percent of the level 1 max HP and 6 percent of attack and defense, and one technique every
  two levels. Speed never changes.
- Winning every story fight in script order gives a level about every four fights and the cap a
  few fights before the end. A Core test walks the encounter table to keep it that way, and checks
  that the table lists the fights in script order.
- `UFTStorySubsystem` holds the party's xp and gold (`GetParty`, `AwardBattle`) and the save
  keeps them (`"party"` in the save JSON). The battle builds the party at the story's level and
  logs `Party: +N xp, +N gold (...)` on a win. A plain demo fight uses level 1 and pays nothing.
- Gold is spent in the shop (see Items below). Core has the shop; there is no shop screen yet.

### Enemy phases

An enemy in `enemies.json` can have `phases`, which turn one fight into a set piece in stages
(`ftcore::EnemyPhase` in `Core/include/ftcore/CombatTypes.h`; the loader is in `Encounter.h`):

```json
"phases": [
  { "pattern": ["hammer", "decree"] },
  { "below": 0.6, "label": "Overdrive", "pattern": ["rally", "hammer", "rally", "decree"], "attack": 10 },
  { "below": 0.25, "label": "Last Stand", "pattern": ["verdict", "onslaught", "rally"], "loop": 1, "attack": 20 }
]
```

- At most 4 phases. The first holds from the start (no `below`). Each later one starts when HP
  falls to `below` of max HP; the thresholds must fall from phase to phase (0 < below < 1). A big
  hit past two thresholds goes straight to the deeper phase.
- `pattern` lists 1 to 8 of the enemy's own skill ids. Its turns follow the pattern in order, then
  repeat from index `loop` (default 0), so the steps before `loop` are an opener played once.
  Entering a phase starts its pattern from the top.
- `attack` (0 to 300) sets attack to the battle-start attack plus that percent; `heal` (0 to 100)
  heals that percent of max HP on entry. `label` (at most 23 bytes) is shown on the banner.
- An enemy without phases uses its skills in order, as before.
- The battle emits `PhaseChanged` (Unreal: `EFTBattleEventType::PhaseChanged`, not to be confused
  with the battle-phase delegate `OnPhaseChanged`). The HUD shows "Name enters Label!" in orange,
  a green +HP popup when it healed, and the log has `BattleDemo: <name> enters phase <k> "<label>"
  (+<n> HP).` `UFTBattleComponent::GetEnemyPhase` and `GetNextEnemySkill` expose the state.

The set piece is the **Brass Regent** (480 HP, speed 13): the opening phase trades Hammer and Decree;
Overdrive (below 60%) adds Rally between them at +10% attack; Last Stand (below 25%) opens with
Verdict, a telegraphed jumpable hit, then loops Onslaught, a four-hit flurry, and Rally at +20%
attack. A Core test plays it with every pilot and checks that each one sees all three phases.

### Results

`ftcore::Battle::stats()` keeps running totals from the start to the outcome (`BattleStats`):
damage dealt and taken, notes judged, perfect and missed, full combos, enemy hits defended, perfect
and landed, counters, weak-point hits, turns per side, phase changes and battle time. Unreal reads
them with `UFTBattleComponent::GetResults()`. The outcome panel shows four lines of them, and a won
story fight adds the xp, gold and level (with LEVEL UP when it went up). The log gets one line:
`BattleDemo: results: <s> s, dealt <n>, taken <n>, notes ...`.

### Balance targets

`ctest` plays every encounter with three simulated pilots (`demo::PilotSkill`), with the party at
the level it would have reached by that fight:

| Pilot | Notes pressed | Hits answered | Must |
|---|---|---|---|
| perfect | all | all | win every fight (the Unreal autopilot plays this way) |
| typical | 3 in 4 | 3 in 5 | win every fight within 60 + 8 × tier seconds |
| novice | 1 in 2 | 3 in 10 | win every fight at tier 0 or 1 |

Misses are spread evenly over the fight, with no randomness. A missed note is a wrong arrow at the
note's time; a missed hit gets no press. Run `ftcore_tests -tc="balance:*" -s` to print one line per
encounter. Every enemy has weak points, so aiming is a way through for players who struggle with combos.
- Roster: brute, shade, wisp (fast, fragile), hound, bulwark (slow, tanky), lurker (hits hard).
  The entries are keyed by real scene ids and real battle steps; a Core test checks that against the
  script when it's available.

Add a fight: add a roster entry if needed, add an `encounters` entry, then run the Core tests. The
balance gate in `test_encounter.cpp` plays the default line-up and every entry headless: perfect
presses must win (also at a coarse 130 ms frame step, the dilated frame length, and with perfect
dodges and jumps instead of parries) and no presses must lose.

## Items

Consumables exist in Core only for now: the rules, the data and their tests are done, but the battle
screen has no Items row and there is no shop screen (see "What Unreal still needs" below). All of it
is plain C++20 with fixed-size storage, so none of it allocates while a battle runs.

### Data

| File | Holds | Core |
|---|---|---|
| `Data/Battle/items.json` | every item: id, effect, scope, amounts, buy and sell price | `ftcore::ItemTable`, `importItemTable` (`Inventory.h`) |
| `Data/Battle/shops.json` | the shop's `default` stock, a list of item ids | `ftcore::ShopStock`, `importShopStock` (`Shop.h`) |
| `Data/Battle/enemies.json` | an enemy's `reward` may list `items` (its drops) | `ftcore::Reward::items` |
| `Data/Loc/en/ui.json` | display text: `item.<id>.name` and `item.<id>.desc` | `ftcore::lookupUiText` |

The files hold ids and numbers only. An item looks like this (the format and its limits are at the
top of `Inventory.h`; a bad file is refused with a reason for each problem):

```json
{ "id": "bandage_roll", "effect": "heal", "scope": "ally", "hp": 30, "hpPercent": 10, "buyPrice": 30, "sellPrice": 15 }
```

- `effect` is `heal` (HP to a living ally), `restore_ap` (AP to a living ally) or `revive` (a downed
  ally gets up). `scope` is `ally` (the default: the one the player picks) or `party` (everyone the
  effect applies to). A heal or revive restores `hp` plus `hpPercent` of the recipient's max HP; a
  restore gives `ap`. Prices are 0 to 100000, and an item never sells for more than it costs.
- Six items ship: Bandage Roll (small heal), Field Kit (big heal), Shared Rations (party heal),
  Energy Bar (AP), Trail Mix (party AP) and Wake Bell (revive). The prices follow the gold the story
  pays: a fight pays 25 gold on average (14 in the first ten, 38 in the last ten), the small heal
  costs 30 and everything sells for half its price. A Core test keeps the cheapest heal between
  about one and two typical fights of gold.
- `ui.json` needs both texts for every item. The `ui_text_keys` check reads `items.json` to count
  `item.<id>.name` and `item.<id>.desc` as used (the game builds those keys from the id at run
  time, so the sources never spell them out), and a Core test checks that no `item.` text is left
  over for an item that is gone.
- Drops: some enemies drop an item, listed as `"reward": { "xp": 8, "gold": 5, "items": [ { "id":
  "bandage_roll", "count": 1 } ] }` (at most 4 per enemy, a count of 1 to 99, default 1). A Core
  test checks that every drop id is in `items.json`. Drops are certain, not random.

### Rules

- **Inventory** (`ftcore::Inventory`). A list of stacks (item id and count), at most 24 different
  ids and 99 of each, in pickup order; a stack that is used up leaves the list. A full inventory
  refuses a new id but still tops up the stacks it has. It never looks ids up, so an id the item
  table no longer has stays in the bag.
- **Save.** The party's inventory is `PartyProgress::items`, so the save keeps it:
  `"party": {"xp": 240, "gold": 130, "items": [{"id": "bandage_roll", "count": 2}]}`. A save
  without `items` (every save made before this) loads with an empty inventory, and the save
  format stays 2. A bad entry refuses the save; entries that do not fit are dropped.
- **Winning.** `encounterReward` adds up the drops of the line-up (the tier raises xp and gold only)
  and `award` puts them into `progress.items`. What does not fit is lost and counted in
  `AwardResult::itemsLost`.
- **Using an item in battle.**
  - `Battle::setItems(table, carried)` loads the table and the party's inventory before `start()`.
    The battle keeps its own copy. `reset()` empties it, so a retry calls `setItems` again.
  - `Battle::useItem(slot, target, now)` works in `AwaitingCommand` for the member whose turn it is.
    `slot` is an index into `carriedItems()`. It costs the turn and no AP: the item takes effect, then
    Recovery, then the next turn, as after a skill. Errors: `WrongPhase`, `NoSuchItem` (a slot past the
    end, or an id the table does not know) and `InvalidTarget`; a refused use changes nothing.
  - Heal: the target must be a living ally; HP stops at max HP. Restore AP: a living ally; AP stops at
    max AP. Revive: a downed ally; it comes back with the item's HP (at least 1, at most max HP) and
    rejoins the turn queue with an empty gauge. An item used on a target that is already full is used
    up all the same.
  - A `party` item reaches every eligible member (living for a heal or AP, downed for a revive), and
    `target` is ignored. If nobody is eligible it is refused with `InvalidTarget` and not spent.
  - One `ItemUsed` event per recipient: `source` the user, `target` the recipient, `amount` the HP or
    AP restored (0 when it was full), `label` the item id (it points into the battle's own copy).
    `BattleStats::itemsUsed` counts items used, not recipients.
  - After the fight `Battle::carriedItems()` holds what is left, unknown ids included. The game writes
    it back to `PartyProgress::items` before it awards the drops.
- **Shop** (`ftcore::buy`, `ftcore::sell`). `buy` checks, in this order, the count (`InvalidCount`), the
  item (`UnknownItem`), the stock (`NotStocked`), the gold (`NotEnoughGold`) and the room
  (`InventoryFull`); `sell` checks the count, the item and `NotCarried`. A deal is all or nothing, so
  a refused one changes nothing. Gold stays within 0 to `kMaxGold`. Selling needs no stock.

### Autopilot

The pilot never uses items by default, so the balance gate above plays as before. Passing a
`demo::PilotItems` to `demo::playHeadless` makes it use a carried heal when a member's HP is at or
below `healBelow` of its max (`demo::pilotItemChoice` picks the smallest heal that covers what is
missing, else the biggest). `HeadlessResult::itemsUsed` counts them.

### What Unreal still needs

- An **Items row** in the battle menu (`AFTBattleHUD`, `UFTBattleComponent`): list `carriedItems()` with
  `item.<id>.name` and the count, pick a target for single items, call `useItem`, and show the
  `ItemUsed` events (a green +HP or an AP popup). It also has to load `items.json` and call `setItems`
  before `start()`, after `reset()` on a retry.
- **Enum mirrors** in `FTBattleComponent.h`: `EFTBattleEventType` needs `ItemUsed` and `EFTCommandError`
  needs `NoSuchItem`, both appended (the casts from `ftcore::EventType` and `ftcore::CommandError`
  are by position), and `FFTBattleStats` an `ItemsUsed`.
- **Writing the counts back.** After a fight, `UFTStorySubsystem` stores `Battle::carriedItems()` in
  its `PartyProgress::items` (`SetParty`) and only then calls `AwardBattle` for the drops. The results
  panel can show the drops from the `Reward` and a "pack full" line from `AwardResult::itemsLost`.
- A **shop screen** over `ftcore::buy` and `ftcore::sell`, with `shops.json` loaded through
  `importShopStock`, the gold and the party's items on screen, and a line for each `ShopResult`.
- The **interface text** for those screens (menu row, shop headings, the `ShopResult` lines) goes in
  `ui.json` with the code that uses it, as the `ui_text_keys` check requires.

## Alpha fast

With a carry on autoplay and `-AlphaFast` on the command line (the same switch as
`UFTGameInstance::IsAlphaFast()`), the battle sets global time dilation 8 and restores it when it
ends. The silent song clock follows game time in that case
(`UFTConductorComponent::bWallClockFollowsGameTime`), so the beat grid, combo timing and the
autopilot's presses stay in step; the autopilot presses at each note's own time
(`UFTBattleComponent::PressAtTime`) and each hit's ideal defence time
(`UFTBattleComponent::DefendAtTime`), so a long frame doesn't make PERFECT late. The result line
gives real and game seconds: `BattleDemo: VICTORY after 7.6 s (game 61.0 s).`

## Regenerating the map

The map is generated by a commandlet; don't edit it by hand.

```
UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTBuildBattleDemo -NullRHI -unattended -nopause -nosplash
```

## Headless checks

- Core: `test_encounter.cpp` is the balance gate over every encounter (see above).
  `test_battle_demo.cpp` plays the whole demo fight headless. With perfect presses it ends in
  victory (9 party turns, 4 defences, about 45.5 s of battle time). With no presses it ends in
  defeat. `test_defence.cpp` covers the windows, lockouts and the autopilot.
- Unreal end to end (the autopilot picks skills and steps on every note):

```
UnrealEditor-Cmd.exe ForgottenThird.uproject /Game/Maps/BattleDemo -game -NullRHI -BattleAutoPlay -BattleQuitOnOutcome -unattended -nosplash -nosound
```

It logs `BattleDemo: VICTORY after <real> s (game <game> s).` and the results line, and exits (no
carry, so the default line-up and no dilation). `-BattleEncounter=<scene>,<beat>,<step>` plays that
entry of `encounters.json` instead, without a carry (so the win pays nothing); the Brass Regent is
`-BattleEncounter=SC-P2-01,12,0`, and its run logs both phase changes. Before the defence timeline it took about 61.5 s; expect roughly the
headless figure above now (not yet measured in Unreal).

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
- **Missing actions.** No flee, guard or party switching. Aim is the only extra command on the battle
  screen. Items exist in Core (see Items) but not on the screen yet. Enemies
  follow their phase patterns (or their skills in order) against the most wounded target. No waves
  or adds yet: a line-up is fixed for the whole fight.
- **No shop screen.** `ftcore::buy` and `ftcore::sell` work, but nothing in Unreal calls them, so
  gold still piles up unspent in the game.
- **Minimal HUD.** The UI is a canvas HUD with engine fonts, not UMG.
