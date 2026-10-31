# Combat systems

The game has three combat systems, and each world picks one. Every fight in a world's scenes plays that world's system:

| System | Kind | What it is | Core |
|---|---|---|---|
| Rhythm battle | `rhythm` | The turn-based battle: skills as arrow combos on the beat, reactive dodge and parry on enemy turns | `Battle.h` (see [BATTLE_DEMO.md](BATTLE_DEMO.md)) |
| Arena melee | `arena` | Real time: free movement inside a ring, light and heavy attacks, a dodge roll, guard and parry, stamina, lock-on | `ArenaFight.h` |
| Beat fight | `beat` | Real time on the music's beat grid: enemies take turns, one bar each, and the lead answers each prompt on its eighth note | `BeatFight.h` |

The rhythm battle is unchanged. This page covers the routing and the two new systems. The arena is Core only so far (rules, data, autopilot and tests); its Unreal screen comes after the animation merge (see "Unreal work still needed"). The beat fight has its screen too: a generated map and a game mode, described in [BEAT_SCREEN.md](BEAT_SCREEN.md).

## Arrow combos: Turns only

The skill arrow combos (`ComboSequence.h`, `ComboGenerator.h`) belong to the turn-based battle alone. The arena and the beat fight never build one:

- The four directions live in `Arrow.h`, which `ComboSequence.h` includes. `BeatFight.h` and `Qte.h` include `Arrow.h` only.
- The `combo_fence` ctest case (`Core/cmake/CheckComboFence.cmake`) fails if `ArenaFight`, `BeatFight` or `BeatScreen` (and any later `BeatBoss` source) names `ComboSequence`, `ComboGenerator` or `generateCombo`.
- `comboSequencesBuilt()` counts every combo built or restarted; a doctest case plays a whole arena fight and a whole beat fight, won by the autopilot and lost by an idle player, and checks it never moves.

The arrows themselves are Turns' too. Everything else is answered with the four action buttons (`ftcore::ActionButton`, `ActionButton.h`: Attack, Guard, Dodge, Jump, the pad's face buttons and the keyboard's J, I, L, K; the Unreal key table and glyph are `FTActionButtons.h`):

- The beat fight's prompts take a button, and `beat.json` patterns name it by letter (`A`, `G`, `D`, `J`).
- The quick-time event's prompts take a button; `qte.json` spells it `attack`, `guard`, `dodge` or `jump`, and an arrow is a load error.
- The mini-game keeps its combo chart for judging (it is a rhythm round), but shows and takes its lanes as buttons: lane i is button i (`miniButton`, `miniLane`).
- `combo_fence` also fails if `Qte`, `BeatFight` or `BeatScreen` (and any later `BeatBoss` source) names `Arrow`, `kArrows` or `arrowName`.

## Routing

`Data/Field/worlds.json` gives each world a `combat` key (`ftcore::CombatKind`, `CombatKind.h`):

| World | `combat` |
|---|---|
| `solhaven` | `arena` |
| `lattice` | `rhythm` |
| `unwritten` | `beat` |

- A world with no `combat` key plays `rhythm`. An unknown value is a load error.
- A scene with no world plays `rhythm`.
- `WorldTable::combatForScene(scene)` and `combatForWorld(idOrMap)` answer the question. Both fall back to `rhythm`.
- A host that can't run a world's system yet plays `rhythm` instead (`playableCombat(want, supportedMask)`). Every fight stays playable while the new screens are built.

`GameplayPlan` is unchanged: a CMB step still lowers to `GameplayScreen::Battle`. The step dispatcher (`FTStep::Prepare`, `Source/ForgottenThird/FTStepHost.h`) asks the world table which system that step plays and opens its screen:

- A beat fight opens the `Beat` map, once it is built (`FTBuildBeatWorld`; see [BEAT_SCREEN.md](BEAT_SCREEN.md)).
- An arena fight plays in place on the scene's world map, like a field step, once the arena host exists. Until then the arena is left out of `FTStep::kPlayableCombat`.
- Anything else, and any system whose screen isn't there yet, plays the rhythm battle on `BattleDemo`. That fallback logs one line per system per run and never counts as a warning.

The story carry's `Combat` field tells the screen which system the step plays. There is no per-encounter override yet (see "Follow-ups").

## Arena melee

The lead fights one to six enemies inside a ring.

- The player has a chain of light attacks, a heavy attack and a dodge roll with invulnerable frames.
- The guard parries in its first moments. Stamina, poise and healing charges limit what the player can do.
- A lock-on keeps the camera and attacks on one enemy.
- Every enemy attack has a telegraph (its startup), and enemies take turns to attack (attack tokens).
- Poise breaks into a stagger. A parry leaves an enemy open to a riposte.

The full rules are in `ArenaFight.h`.

### Host contract: Core judges, the host moves

Each frame the host does three things:

1. It reports where the bodies stand, after moving them itself with its own collision.
2. It reports what the player pressed.
3. It calls `ArenaFight::tick(dt, input, bodies)`.

The tick returns the frame's events, plus what each body wants next: a velocity, a facing, an action and the part of an attack it is in. The host moves each pawn at that velocity, sets its rotation to Core's facing and plays the action's animation. The hit arcs then match what the player sees.

Core owns every facing and every timer. It never knows walls or the navmesh. It only knows the ring: a circle the host supplies, which no body leaves. `arenaStep()` moves bodies the way a host with no walls would. Tests, the balance check and headless runs use it in place of an engine.

What the host reads besides the velocity and facing:

- `ArenaDef::enemyKind`: each slot's index into the table's enemy kinds, so the host picks that enemy's mesh and clips. A def built in code leaves `kArenaNoKind`.
- `ArenaPlayerOut::dodgeDir` and `backstep`: set once when a dodge starts and kept until the next one. The host picks the roll clip from them once, because a direction read from the velocity turns when a roll slides along the ring's edge.

Core works on Unreal's ground plane: x forward, y to the right. A positive cross product is a turn to the right, and the lock switch's +1 moves the lock to the right.

### The ring

Fights happen in place on the world map:

- `arenaRingAt(playerPos, facing, radius, bounds)` centres the ring a little ahead of the player, so the enemies' side has room.
- The ring is kept inside `bounds`, normally the district's plaza (`WorldLayout`'s plaza radius is 36 m, the default ring 12 m).
- The followers wait just outside the ring at `arenaParkSpot()`.
- The enemies start on an arc in front of the player, at the fight's `spawn` distance.

### Rules in brief

| Input | Does |
|---|---|
| Move | Walk at up to `moveSpeed`. Locked on, the player strafes facing the target |
| Light | The light chain, up to 4 attacks, which wraps. A riposte instead when an open enemy is in reach and in front |
| Heavy | One slow attack with armor |
| Dodge | A roll along the stick, or a backstep with the stick idle. Invulnerable in its middle part |
| Guard (held) | Blocks from the front. A blocked hit costs stamina, and an empty bar breaks the guard. Pressing guard opens a short parry window, unless the previous press was too recent |
| Heal | Uses a charge and heals after a short delay. Hitstun before then cancels it and keeps the charge |
| Lock / switch | Locks the best enemy near the view direction, or lets go. The switch moves the lock left or right |

The other rules:

- Each attack has a startup, an active part and a recovery. It can connect only in its active part, at most once per target. A hit test covers the whole tick, so a long frame can't skip a short active part.
- An attack's `maxTargets` caps how many targets it connects with over its whole active part, nearest first, so a long, narrow attack doesn't pierce a whole line. The shipped riposte hits only its own enemy.
- Movement ramps up and down. The player's velocity moves toward the stick's at `moveAccel` and `moveBrake` (in Idle, Move, Guard and Heal), and an enemy's at its kind's `accel` (in Approach and Strafe). Lunges, dodges and reactions set their velocity directly, without a ramp.
- An input pressed while an action can't start is buffered for 0.2 s.
- Stamina regenerates after a short delay.
- Poise refills after a delay without poise damage.

### Enemies

The enemy brain runs Approach, Strafe, Attack, Staggered, Open, Down and Dead:

- Only `attackers` enemies attack at once. The one that has waited longest gets the token and keeps it through its whole string.
- An enemy picks a move whose range fits, weighted, from a seeded RNG.
- A move's `chain` names the move that follows it at once.
- A move can be unblockable, or not parryable.

### Data

`Data/Battle/arena.json` holds ids and numbers only. Its schema is in `ArenaFight.h`.

- Fighters, enemy kinds with their moves, a `default` fight, and fights keyed by scene id, gameplay beat index and plan step index.
- A missing key keeps the struct's default, so entries list only what differs.
- Only the `default` fighter ships. Per-lead fighters are allowed by the schema and fall back to it.
- Three placeholder enemy kinds ship:
  - `brute`: a slam with a follow-up, and a slow overhead that can't be parried.
  - `skirmisher`: quick two-hit jabs.
  - `breaker`: a smash, and a grab that can be neither blocked nor parried.
- The default fight is one brute. No scene fights ship yet.
- An attack's damage rises with its telegraph, and a test checks this.

### Autopilot and balance

`ArenaFight::autopilot()` is a steady player:

- It locks the nearest enemy and closes in.
- It parries or sidesteps attacks about to land.
- It ripostes open enemies and attacks with the light chain between enemy windups.
- It heals when low and safe.

The tests run every fight at a fixed 1/60 s step with `arenaStep`. The autopilot must win each one within 120 s, and a player who presses nothing must lose. `tick`, `autopilot` and `arenaStep` never allocate.

## Beat fight

The enemies take turns, one 4/4 bar each, in round robin. On its turn an enemy plays one bar of one of its patterns. Each eighth note of the bar can hold a prompt:

| Prompt | Pattern token | Answer |
|---|---|---|
| Strike | `sL`, `sD`, `sU`, `sR` | Press the arrow on its eighth: the lead lands a hit |
| Counter | `cL` ... | The enemy strikes on its eighth: press the arrow to parry and hit back. If it's unanswered or late, the lead is hit |
| Hold | `hU2` ... `hU8`, then `-` slots | Press on its eighth and keep the arrow down for 2 to 8 eighths: a heavier hit |

Correct answers in a row build a chain, and the chain builds a damage multiplier (up to x4). A Miss resets both. When an enemy is low, its next turn is a finisher instead: a fast run of strikes that takes the rest of its hp, unless one is missed. Only missed counters hurt the lead.

The full rules are in `BeatFight.h`.

### Clock

`BeatFight` runs on the fight's clock: the music's song time from the host's `Conductor`, or a pause-aware wall clock when no music plays. The def's `bpm` and `offset` place the beat grid, so they must match the cue. Presses are stamped with `Conductor::songTimeForEvent`, as the rhythm battle's are. The host's rule for choosing between the two (and for `-AlphaFast`) is in [BEAT_SCREEN.md](BEAT_SCREEN.md), "The clock".

### Judging

- **Strikes and hold presses** use symmetric windows around the eighth: ±0.05 s is Perfect, ±0.12 s is Good.
- **Counters** use the defence maths of `DefenceTimeline`, the same way a narrow QTE prompt is judged ([QTE_SCREEN.md](QTE_SCREEN.md)). The impact sits half a parry window after the eighth, so the parry window is centred on the beat. A press there is Perfect; a press in the rest of the dodge window is Good.
- **Fast tempos:** each window edge is capped to a share of an eighth, so neighbouring notes stay apart. A strike's windows reach at most half an eighth either side, and a counter's window opens at most one eighth early. At 120 bpm the windows are as above.
- **Mashing a counter** is punished, on that input only. Pressing the arrow of a counter whose marker is showing, before its window opens, is a whiff: it locks counters out briefly. Strikes and holds are never locked out.
- **Order:** a press answers the earliest open prompt. There are two exceptions:
  - A press that matches the next prompt instead counts as skipping a note: the earlier prompt is a Miss.
  - A press or release just before a hold's end completes the hold first.
- **Strays:** any other press that answers nothing is counted, and does nothing else.

### Data

`Data/Battle/beat.json` holds ids, numbers and pattern strings. Its schema is in `BeatFight.h`.

- Enemies with `hp`, `damage` and up to six one-bar patterns. Every pattern holds at least one counter, so every bar threatens.
- A `default` fight, and fights keyed like `qte.json`, with `bpm`, `offset`, the lead's `hp`, the line-up and a `music` cue id.
- Optional window overrides per fight.

### Autopilot and balance

`BeatFight::autopilot()` presses every prompt on its beat and lets go of each hold at its end. The `qteAutoComplete` assist lets it play the fight, and `holdToggle` makes holds latch, the same options the QTE screen honours.

The tests require two things of every fight within 64 bars: the autopilot wins, and no input loses. `ftcore::BeatScreen` (the screen's state machine) plays that autopilot under autoplay and checks the same thing through the screen: it wins the default fight and every fight in `beat.json`.

## Techniques used

These are numbered as in [COMBAT_REFERENCE_STUDY.md](COMBAT_REFERENCE_STUDY.md):

- **1:** fixed, learnable timings (enemy move timings, and patterns on the grid).
- **2 and 8:** layered warnings, with the telegraph scaled to the attack (each move's startup; the beat markers' approach).
- **4:** a different window per input (dodge i-frames against a narrow parry; the counters' parry and dodge windows).
- **5:** a retry restarts the same fight (the same seed).
- **10:** perfect defence pays (a parry opens a riposte; a perfect counter deals damage).
- **11:** mashing is punished on the hard input only (`parryRearm`; the counter whiff lockout).

## Unreal work still needed

These wait until the animation session's merge is pushed.

- **Arena:**
  - The arena host on the explore character (the ring, the follower parking spots, enemy pawns).
  - Attack, hit, guard and stagger animations, and a dodge roll that reuses the movement work.
  - A lock-on camera.
  - The HUD: hp, stamina, heal charges, the lock marker and the enemy hp.
- **Logging:** the arena screen logs one line per fight with ids and counts only, like the other screens (the beat screen does already: see [BEAT_SCREEN.md](BEAT_SCREEN.md)). The routing is done (see "Routing").

## Follow-ups

- Status effects (parked).
- A per-encounter override of the world's system.
- Ranged enemy attacks in the arena.
- A synthesized beat loop for fights with no cue.
- In-place staging of beat fights on the world map (the fight plays on its step's world map, as an arena fight does, instead of opening the `Beat` map).
- Per-lead fighter kits in `arena.json`.
