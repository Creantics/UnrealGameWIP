# Combat systems

The game has three combat systems, and every world has all three. Each fight plays one by what it is: minor fights are Turns, bosses Arena, and each world's final Beat (see "Routing"):

| System | Kind | What it is | Core |
|---|---|---|---|
| Turns (the rhythm battle) | `rhythm` | The turn-based battle: skills as arrow combos on the beat, reactive dodge and parry on enemy turns | `Battle.h` (see [BATTLE_DEMO.md](BATTLE_DEMO.md)) |
| Arena melee | `arena` | Real time: free movement inside a ring, light and heavy attacks, a dodge roll, guard and parry, stamina, lock-on | `ArenaFight.h` |
| Beat fight | `beat` | Real time on the music's beat grid: enemies take turns, one bar each, and the lead answers each prompt on its eighth note | `BeatFight.h` |

The rhythm battle's enemy turns play on the song's beat: with music on, each hit is moved later onto a beat, no tell is shorter than 0.4 s, and a count runs down to the first hit (`CountIn`; see [BATTLE_DEMO.md](BATTLE_DEMO.md), "Defence"). Its party members may also carry verbs beyond the arrow combos: Guard and Release with Load and Overload, Intercept (a hold taken for an ally), Take Half (an ally evens out Load and the Torn status) and Read (three cards dealt from the enemies' actions as an attack starts; the right one brings a free counter); see [BATTLE_DEMO.md](BATTLE_DEMO.md), "Guard, Load and Release", "Intercept, Torn and Take Half" and "Read cards". This page covers the routing and the two new systems. The arena has a demo screen too: a generated ring map, a game mode with a lock-on camera and a canvas HUD, described in [ARENA_DEMO.md](ARENA_DEMO.md); hosting it on the explore character comes after the animation merge (see "Unreal work still needed"). The beat fight has its screen too: a generated map and a game mode, described in [BEAT_SCREEN.md](BEAT_SCREEN.md).

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

Each fight names its system by what it is, not where it is. In `Data/Battle/encounters.json` an encounter takes an optional `combat` key (`ftcore::CombatKind`, `CombatKind.h`):

| Fight | `combat` | Plays now |
|---|---|---|
| Every minor fight: any line-up without a boss | absent (Turns, `CombatKind::Rhythm`) | Turns |
| A boss fight | `arena` | Turns, until the arena host is built |
| A world's final (one per world) | `beat` | the beat fight, until the beat boss mode lands |

- An encounter without the key, any fight not listed, and the `default` line-up are Turns fights. The `default` refuses the key; an unknown value is a load error with its path.
- `EncounterTable::combatFor(scene, beat, step)` answers, without allocating.
- In the data, 24 fights are Turns, 13 Arena and 3 Beat. A test checks that a fight is tagged exactly when its line-up has a boss (an enemy of 340 base max HP or more), and that each Beat fight is a single boss.
- `worlds.json`'s old per-world `combat` key is retired: the loader warns and ignores it, and the file no longer has it.
- A host that can't run a fight's system yet plays Turns instead (`playableCombat(want, supportedMask)`), so every fight stays playable while the new screens are built.

`GameplayPlan` is unchanged: a CMB step still lowers to `GameplayScreen::Battle`. The step dispatcher (`FTStep::Prepare`, `Source/ForgottenThird/FTStepHost.h`) asks the game instance's encounter table (loaded at Init) which system the step plays and opens its screen:

- A beat fight opens the `Beat` map, once it is built (`FTBuildBeatWorld`; see [BEAT_SCREEN.md](BEAT_SCREEN.md)).
- An arena fight plays in place on the scene's world map, like a field step, once the arena host exists. Until then the arena is left out of `FTStep::kPlayableCombat`.
- Anything else, and any system whose screen isn't there yet, plays Turns on `BattleDemo`. That fallback logs one line per system per run and never counts as a warning.

The story carry's `Combat` field tells the screen which system the step plays, and `WantedCombat` which one its encounter names. An alpha run ends with a second line counting both: `Alpha: FIGHTS <n> (tagged <t> turns, <a> arena, <b> beat; played <t> turns, <a> arena, <b> beat)` (`AlphaCounters::formatFights`).

## Arena melee

The lead fights one to six enemies inside a ring.

- The player has a chain of light attacks, a heavy attack and a dodge roll with invulnerable frames.
- The guard parries in its first moments. Stamina, poise and healing charges limit what the player can do.
- A lock-on keeps the camera and attacks on one enemy.
- Every enemy attack has a telegraph (its startup), and enemies take turns to attack (attack tokens).
- Poise breaks into a stagger. A parry leaves an enemy open to a riposte.
- A parry also stores Load, a second gauge beside stamina, which a Release spends as poise damage.

The full rules are in `ArenaFight.h`.

Where a fight is held is data too: a stage in `Data/Battle/arenas.json` (a world style, a floor shape, a bound, counts of cover, pillars, landmarks, pools, cracks and lamps) that `ArenaStage.h` turns into a layout the host builds. The ring, spawn distance, prop sizes and camera scale with the boss's size (`height` and `bodyRadius` of the enemy kind) and the number of enemies and attackers; the fight itself still knows only the ring, and the generator keeps everything solid outside it. See [ARENA_DEMO.md](ARENA_DEMO.md), "Stages".

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
| Release | A riposte-shaped strike, invulnerable, open only above 33 Load (`kArenaReleaseOpenLoad`). It spends all the Load at once, and each enemy it hits loses `poiseDamage` plus the spent Load times `releasePoise` in poise. It costs no stamina |
| Pair | The second lead's input. Pressed within `pairWindow` (0.12 s) of a parry, before or after it, the parried enemy takes a Counter-Stagger: `pairHits` (3) light hits' damage at once, and a stagger. One press per parry |
| Read | The second lead's other input. As each enemy move starts, three cards are dealt: the coming move and two decoys from the living enemies' moves, each named by its `tell` (no two alike; `ReadCards`, as in the Turns battle). Nothing pauses. A pick counts while that move is still in its startup. Right: one Read stack (up to 5), and when the move's active part ends, or a parry or stagger cuts it short, the partner strikes the attacker: `readPower` (0.5) times light[0]'s damage, plus 20% per stack past the first, and `readPoise` poise. Wrong: the stacks go back to 0 and the attacker regains `readWrongPoise` (10) poise |
| Lock / switch | Locks the best enemy near the view direction, or lets go. The switch moves the lock left or right |

The other rules:

- Each attack has a startup, an active part and a recovery. It can connect only in its active part, at most once per target. A hit test covers the whole tick, so a long frame can't skip a short active part.
- An attack's `maxTargets` caps how many targets it connects with over its whole active part, nearest first, so a long, narrow attack doesn't pierce a whole line. The shipped riposte hits only its own enemy.
- Movement ramps up and down. The player's velocity moves toward the stick's at `moveAccel` and `moveBrake` (in Idle, Move, Guard and Heal), and an enemy's at its kind's `accel` (in Approach and Strafe). Lunges, dodges and reactions set their velocity directly, without a ramp.
- An input pressed while an action can't start is buffered for 0.2 s.
- Stamina regenerates after a short delay.
- Poise refills after a delay without poise damage.
- Load sits beside stamina and never replaces it. Stamina still pays for attacks and the roll and refills on its own. Load fills only from parries (the parried attack's damage times `loadShare`, up to 100) and never refills or drains on its own.
- At 100 Load the player is Overloaded, told once as the gauge fills. Light (and so the riposte), heavy and heal are refused until a Release. The guard, the parry, the roll, movement and the lock stay open, as the Turns battle never gates the defence.
- With a `bpm` in the data, every enemy move's startup is a whole number of beats, so telegraphs land on the music's grid. The loader refuses a startup off the beat by more than `kArenaBeatTolerance` (2 ms), naming its path.

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
- The top-level `bpm` (150 ships, a 0.4 s beat) sets the beat every startup lands on. Without it nothing is checked.
- The fighter's `release` is an attack object like `riposte`. `loadShare`, `releasePoise`, `pairWindow` and `pairHits` tune the Load, the Release and the paired counter.

### Autopilot and balance

`ArenaFight::autopilot()` is a steady player:

- It locks the nearest enemy and closes in.
- It parries or sidesteps attacks about to land.
- It ripostes open enemies and attacks with the light chain between enemy windups.
- It releases at 50 Load on an upright enemy in reach, and at once when Overloaded.
- It heals when low and safe. It never presses the paired counter, which needs the second lead, but it reads for the partner: the right card, 0.2 s into the startup (half of a shorter one).

The tests run every fight at a fixed 1/60 s step with `arenaStep`. The autopilot must win each one within 120 s, and a player who presses nothing must lose. `tick`, `autopilot` and `arenaStep` never allocate, also over a fight that stores Load, releases it and deals and picks Read cards.

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

## Beat boss

A world's final is a beat boss: one authored track instead of the beat fight's round robin. Every move the boss makes is pinned to a bar and a beat of its music, so the fight is learned like a song. It is answered with the action buttons (Attack, Guard, Dodge, Jump), never arrows. `ftcore::BeatBoss` has a demo host on its own map, `Demo_BeatBoss` ([BEAT_BOSS_DEMO.md](BEAT_BOSS_DEMO.md)); no story step opens it yet.

The full rules are in `BeatBoss.h`.

### Rules in brief

- **The bar map.** Each phase has its own tempo (at most 150 bpm), meter and loop of bars. One bar can break the meter: a 5/4 or 3/4 bar in a 4/4 loop, or a 7/8 bar in a 6/8 one. 6/8 counts in dotted quarters. Beats can be left empty, and the beat line draws them hollow.
- **Pinned moves.** Each move has a tell, where its marker appears, and an impact, where it is answered, both written as `bar.beat` (`"4.2&"`). It also has an answer, a stem tag for the marker's size, and a miss cost. The answers are strike, hold, release (spends the Load), guard, parry, dodge, jump and paired (two buttons together). Guard, parry, dodge, jump and paired are counters. Only a missed counter costs hp, and never more than 35% of the lead's max hp.
- **Judging** uses the beat fight's windows. Strikes get ±0.05 s for Perfect and ±0.12 s for Good. A counter is Perfect within 0.06 s of its impact, and its window opens 0.3 s early. Mashing a counter's button before its window opens locks counters out briefly. Strikes and holds are never locked out.
- **Streak.** Each answer adds one and any Miss resets it. The multiplier rises one step every eight, up to x4. Named rewards fire at set counts: they open a rest, heal, or store Load.
- **Open rests.** These are beats the music leaves open on purpose. An answered counter marked `opens`, or a reward, opens the next one, and a Miss closes the last one opened. Four open at once stagger the boss, and the next bar plays without its counters.
- **Phases and finishers.** The boss's hp stops at each phase's floor, and the next phase starts at the next bar line. When the boss is close to the floor, the phase's finisher bars replace the loop's next bar, once a loop. Every strike of the finisher takes an equal share of what's left, and a Miss breaks it at no cost.
- **Scripted silences.** At a share of the boss's hp or at a given bar, the track stops for a set time. Nothing shows and nothing lands. Then a one-bar count brings it back.
- **Checkpoints.** Each phase's first bar line saves the phase, the boss's hp, the Load and the silences already played. A retry goes back there with the lead at full hp, counts one bar in, and starts the phase's bar 1.
- **The beat line** is the sound-off fallback. It holds the current bar and the next, with their ticks, rests and markers, laid out by time so a short bar is drawn short. It is filled into fixed arrays and allocates nothing.

### Data

`Data/Battle/beat_boss/<id>.json` holds one track per file, with ids and numbers only. Its schema is in `BeatBoss.h`. `test_track.json` is synthetic, and the tests run on it. It has two phases: a 4/4 one with a 5/4 bar and a 3/4 bar, an hp silence and a release bar, and a 6/8 one with a 7/8 bar, an empty beat and a bar silence. The loader refuses each of these with its path:

- a tell or impact on an empty beat, or on a lost beat (one the bar doesn't have)
- a tell under 0.4 s before its impact
- a tempo over 150
- two impacts closer than 0.2 s
- a loop whose bars don't sum to its seconds
- a miss cost over 35%

### Autopilot and balance

`BeatBoss::autopilot()` presses every prompt on its impact, holds each guard through its impact, and lets go of holds at their end. The `qteAutoComplete` assist lets it play the fight. The tests require the following:

- The autopilot wins the test track within 24 bars, at 120 Hz, 60 Hz and 130 ms frames, with no miss.
- Doing nothing loses.
- One miss costs at most 35%.
- A whole fight allocates nothing after loading, including its retries and the beat line.

## Techniques used

These are numbered as in [COMBAT_REFERENCE_STUDY.md](COMBAT_REFERENCE_STUDY.md):

- **1:** fixed, learnable timings (enemy move timings, and patterns on the grid).
- **2 and 8:** layered warnings, with the telegraph scaled to the attack (each move's startup; the beat markers' approach).
- **4:** a different window per input (dodge i-frames against a narrow parry; the counters' parry and dodge windows).
- **5:** a retry restarts the same fight (the same seed).
- **10:** perfect defence pays (a parry opens a riposte; a perfect counter deals damage).
- **11:** mashing is punished on the hard input only (`parryRearm`; the counter whiff lockout).

## Demo fights

Each battle type has a demo fight that plays on its own map, with the mannequin standing in for the leads:

- **Turns:** `BattleDemo` with `-BattleLineup=turns`, a fight that uses every Turns verb: Guard and Release, Intercept, Take Half and the Read cards ([BATTLE_DEMO.md](BATTLE_DEMO.md), "Turns demo").
- **Arena:** `Demo_Arena` ([ARENA_DEMO.md](ARENA_DEMO.md)); `-ArenaStage=<id>` holds the fight on one of the generated arenas, each shaped by its world and scaled to its boss's size (ARENA_DEMO.md, "Stages").
- **Beat:** `Demo_BeatBoss`, the synthetic test track played as a beat boss with a metronome in place of music ([BEAT_BOSS_DEMO.md](BEAT_BOSS_DEMO.md)).

Each demo takes an autoplay flag that wins the fight and an idle flag that loses it, and logs one result line, so it can be checked headless.

## Unreal work still needed

These wait until the animation session's merge is pushed.

- **Arena:** the demo screen exists ([ARENA_DEMO.md](ARENA_DEMO.md): ring map, lock-on camera, HUD with hp, stamina, the Load gauge, poise, heal charges, the tell, the lock marker and the enemy hp; the Release, the overload and the partner's paired press are hosted). Still to do:
  - The arena host on the explore character (the ring, the follower parking spots), reached through the routing.
  - Attack, hit, guard and stagger animations, and a dodge roll that reuses the movement work.
- **Logging:** the arena screen logs one line per fight with ids and counts only, like the other screens (the beat screen does already: see [BEAT_SCREEN.md](BEAT_SCREEN.md)). The routing is done (see "Routing").

## Follow-ups

- Status effects (parked).
- Ranged enemy attacks in the arena.
- A synthesized beat loop for fights with no cue.
- In-place staging of beat fights on the world map (the fight plays on its step's world map, as an arena fight does, instead of opening the `Beat` map).
- Per-lead fighter kits in `arena.json`.
