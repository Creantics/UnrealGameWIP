# Movement moves

The lead's moves beyond walking, running and jumping are the slide, the landing roll, the dodge roll
and the ledge grab. Vaults and climbs share their code.

- **Rules.** Each move's rules are a Core state machine with doctest coverage:
  - `ftcore::SlideSet` (Slide.h)
  - `ftcore::Locomotion`'s landing roll and `TraversalSet::planRoll` (Locomotion.h, Traversal.h)
  - `ftcore::DodgeRoll` (DodgeRoll.h)
  - `ftcore::LedgeHang` with `TraversalSet::planCatch` (LedgeHang.h, Traversal.h)
- **Wrapper.** `AFTExploreCharacter` runs these machines on the lead.
- **Clips.** The clips come from the mocap sample, driven from C++ by `UFTLocomotionAnimInstance`.
- **Tunables.** Every tunable is in `Data/Movement/movement.json`, read once at start by
  `ftcore::importMoveTuning`. If a name is misspelt or a number is out of range, the whole file is
  refused, the log shows a `Movement:` warning, and the built-in defaults apply. The file ships with
  the defaults, so tuning starts from the code's values.

The dodge machine needs no clip and no character, so the arena can drive the same `ftcore::DodgeRoll`
for its own dodge later. Today the arena keeps its own rules in `ArenaFight`.

## Controls

| Move | Keyboard and mouse | Gamepad | When |
|---|---|---|---|
| Slide | C or Left Ctrl | Right stick click | While sprinting (Left Shift or left trigger held) |
| Jump out of a slide | Space | A | During a slide, once it has run for `slide.jumpFrom` |
| Get up from a slide | Press crouch again (crouch is a toggle) | Same | Any time; a slide slower than `slide.outSpeed` gets up by itself |
| Dodge roll | Left Alt | B | On the ground; it goes the way the stick points |
| Landing roll | Be crouched as you land, or press Left Alt / B in the air | Same | See "Landing roll" below |
| Grab a ledge | Hold Space or push the stick into the wall, in the air | Hold A, or the stick into the wall | When a ledge is in reach |
| Climb straight up at the grab | Hold jump and push the stick into the wall | Same | At the moment of the catch |
| Climb up from a hang | Space or A, or push the stick into the wall | Same | After hanging for `ledge.minHang` |
| Drop from a hang | Crouch, or pull the stick away from the wall | Same | After hanging for `ledge.minHang` |

## The moves

- **Slide.** Pressing crouch while sprinting starts a slide. It keeps the sprint's speed, loses speed
  to friction, and gains speed down slopes. It ends with a get-up that matches the input at that
  moment.
  - Jump cancel: pressing jump during a slide (after `jumpFrom`) ends it at once and launches the
    lead along the slide. The take-off speed is the slide's speed times `jumpCarry`, capped at
    `jumpMaxSpeed`, with the normal jump height on top.
  - A jump pressed before `jumpFrom` is kept for 0.2 s, so it still counts once the slide reaches
    `jumpFrom`.
- **Landing roll.** A landing turns into a roll that keeps the lead's speed, instead of a hard
  landing, when any of these hold:
  - the lead has been in the air for `landing.rollMinAir` or longer, is moving, and either has
    crouch on or pressed roll within `landing.pressWindow` before touchdown;
  - the lead has fallen `landing.rollDrop` or more, whatever the input.
- **Dodge roll.** Pressing roll on the ground starts a dodge, unless the cooldown is still running.
  If `needsDirection` is 1, the stick must also be pushed. The roll's kind depends on speed:
  - **Long:** at `longFrom` or faster. The roll keeps the lead's speed, and is never slower than
    `longSpeed`.
  - **Short:** below `longFrom`. The roll goes at `shortSpeed`. It costs the same stamina as a Long
    roll.
  - **Tired:** when stamina is below `staminaCost`. The roll goes at `tiredSpeed` and keeps only
    `tiredInvuln` of the invulnerability window. The default is 0, so a Tired roll gives no
    protection.

  The lead can't be hurt from `invulnStart` to `invulnEnd` into the roll (`IsInvulnerable()`). The
  cooldown starts when the roll ends. Stamina comes back at `staminaRegen` per second once
  `regenDelay` has passed since it was last spent. With `staminaCost` 0, dodges cost nothing and
  are never Tired. Landing rolls are free and ignore the cooldown.
- **Ledge grab.** Holding jump in the air, or pushing the stick into a wall, grabs a ledge in reach.
  The catch clip is timed to the lead's flight. When the hands reach the edge, one of two things
  happens:
  - **Climb straight up:** if jump is still held and the stick pushes into the wall, or if
    `ledge.hang` is 0.
  - **Hang:** otherwise. The clip holds on its catch frame.

  While hanging, jump or the stick into the wall climbs up, and the clip plays on. Crouch or the
  stick away from the wall drops: the lead falls, pushed `dropPush` cm/s off the wall, and grabs no
  ledge for `regrab` seconds. If drop and climb are asked in the same frame, the drop wins.

## Tunables (`Data/Movement/movement.json`)

| Section | Tunable | Default | Meaning |
|---|---|---|---|
| slide | minEntrySpeed | 350 | Slowest speed that can start a slide (cm/s) |
| slide | friction / gravity | 400 / 980 | Slowdown on flat ground, and the slope pull (cm/s²) |
| slide | outSpeed / maxSpeed | 250 / 900 | Speed below which the lead gets up, and the fastest a slide gets |
| slide | steerRate | 45 | How fast the stick turns the slide (degrees per second) |
| slide | jumpFrom / jumpCarry / jumpMaxSpeed | 0.1 / 1 / 900 | The jump cancel (see above) |
| slide | exitAfter / speedCost / footCost / minEntryRate / maxEntryRate | 0.3 / 0.005 / 0.3 / 0.8 / 1.5 | Clip choice, play rate and hand-back |
| traversal | rollMinSpeed / rollMaxRate / rollTurnOff | 550 / 2.5 / 30 | Roll speed floor, fastest play rate, and the stick angle that ends a roll early |
| traversal | minCatchLead / maxCatchLead / maxCatchMiss / maxCatchStretch / catchMissCost | 0.1 / 0.4 / 40 / 15 / 0.025 | Ledge catch timing and how far the flight may miss the edge |
| traversal | standDepth, heightSlack, minHeightSlack, maxDrop, minLead, maxPush, maxPull, heightCost, speedCost, footCost, exitAfter, min/maxEntryRate | see the file | Vaults and climbs (unchanged by these moves) |
| landing | rollMinAir / rollDrop / pressWindow | 0.4 / 250 / 0.3 | The landing roll (see above) |
| dodge | length | 0.8 | How long a dodge lasts if the clip gives no length (s) |
| dodge | invulnStart / invulnEnd | 0.05 / 0.4 | The window in which the lead can't be hurt (s into the roll) |
| dodge | cooldown | 0.25 | Wait after a dodge ends before the next can start (s) |
| dodge | staminaCost / staminaMax / staminaRegen / regenDelay | 25 / 100 / 30 / 1 | Stamina (cost 0 turns stamina off) |
| dodge | longFrom / shortSpeed / longSpeed / tiredSpeed | 500 / 550 / 650 / 300 | Roll kinds and their speeds (cm/s) |
| dodge | tiredInvuln / needsDirection | 0 / 0 | How much of the window a Tired roll keeps; 1 means a dodge needs the stick pushed |
| ledge | hang | 1 | 0 means every grab climbs straight up |
| ledge | minHang | 0.2 | Shortest hang before a climb or drop is taken (s) |
| ledge | stickShare / intoFace | 0.5 / 50 | How far the stick must be pushed, and the angle that counts as into the wall (away: 180 minus this) |
| ledge | dropPush / regrab | 80 / 0.5 | The push off the wall when dropping, and the wait before the next grab |

## Headless checks

- **Core doctest** (`ctest` in `Core/build`) runs every transition of every machine:
  - test_slide.cpp: the jump cancel in each phase, jumpFrom, jumpCarry and the cap.
  - test_locomotion.cpp: the landing roll with the shipped tunables (a long fall, a hop, a fall with
    crouch on).
  - test_dodge_roll.cpp: the three roll kinds, the invulnerability window, the cooldown, stamina
    and its regen delay, free dodges, and needsDirection.
  - test_ledge_hang.cpp: hang versus climbing straight up, climb, drop, minHang, the regrab, and
    drop winning over climb.
  - test_move_tuning.cpp: the shipped file and every refusal.
  - None of these tick paths allocate.
- **In the game** (main checkout only: the mocap clips are local there, not in worktrees), with
  `-game -NullRHI`:
  - `-LocoCatch` grabs three ledges. At each it hangs for half a second, then climbs, and logs
    `Ledge: hanging` and `Ledge: climbing up`.
  - `-LocoRollEvery=<s>` dodges every s seconds and logs `Dodge: <kind> at ... stamina ...`.
  - `-LocoRun -LocoCrouchEvery=<s>` slides.
  - Every move logs its transitions under `LogForgottenThird` (`Slide:`, `Roll (landing|asked):`,
    `Dodge:`, `Ledge:`, `Catch:`).

## Animation gaps

The mocap sample has no clip for these, so a stand-in plays:

- **Hanging idle.** The catch clip is frozen on its catch frame. The pose is right, but the body is
  still: no sway and no breathing.
- **Shimmy along a ledge.** There is no shimmy, so a hang can't move sideways.
- **Drop from a hang.** The frozen catch frame fades into the fall.
- **Climb up from a hang.** The catch clip plays on from its catch frame. It was recorded as one
  motion from a jump, so it may show a hitch when it starts from a still hang.
- **Standalone dodge roll.** The dodge plays the land-roll clips from their touchdown, so the
  start is a land roll's. There is no ground-start sidestep or dive.
- **Tired roll.** It plays the same clips slowed down (no slower than the minimum play rate). There
  is no stumbling roll.
- **Heavy landing.** The heavy land clips are only dumped (`-LocoJumpDump`), never played. A fall
  past `rollDrop` always rolls, so no hard heavy landing is ever needed yet.
- **Slide jump take-off.** The slide clip is released into the normal jump. There is no slide-to-jump
  transition clip.

## For a visual review in the real game

Run these from `D:\projects\unrealgamewip` after merging this branch:

1. Slide, then jump at each phase: dropping in, sliding, getting up. Check that the take-off doesn't
   pop and that the speed feels carried.
2. Landing rolls:
   - Walk off a 3 m drop: it should roll without input.
   - Jump down about 1.5 m while running with crouch on.
   - Repeat, pressing roll just before touchdown.
3. Dodge:
   - Long roll from a sprint, Short roll from a walk.
   - Dodge four times quickly to see the Tired roll.
   - Check the cooldown feel.
4. Ledges (`-LocoCatch`):
   - Check the hang pose holds on the wall: the hands on the edge, no sinking.
   - Climb from a hang with jump and with the stick.
   - Drop with crouch and with the stick away, and check the push clears the wall.
   - Hold jump and the stick into the wall: the lead should climb straight up.
   - With followers shown (P): they replay the lead's catch straight through without hanging, so they
     may climb while the lead still hangs. Check that this reads well.
