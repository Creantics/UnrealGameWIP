# Arena demo

A playable version of the real-time arena fight: one lead against one to six enemies (by default a wave
of three, a brute, a skirmisher and a breaker, one attacking at a time; placeholder shapes) in a round ring on one of
several generated arenas ("Stages" below), with a lock-on camera and a canvas HUD. The rules all live in
Core (`ftcore::ArenaFight`, `Core/include/ftcore/ArenaFight.h`); the enemies, their moves and the ring
come from `Data/Battle/arena.json`. See [COMBAT_SYSTEMS.md](COMBAT_SYSTEMS.md), "Arena melee", for the
rules themselves.

Map: `/Game/Maps/Demo_Arena` (Content/Maps/Demo_Arena.umap). The map's game mode is `AFTArenaGameMode`
(no story; HUD `AFTArenaHUD`; pawn `AFTArenaFighter`). The project's default map is unchanged.

The map is built from code, never by hand, and holds only a player start: the arena itself is built at run time
from the stage (below), so one map serves every stage. After a change to the commandlet, build it again:

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildArenaWorld -NullRHI -unattended -nopause -nosplash
```

It also writes the glow material the stages' lit parts wear, `/Game/Rendering/M_ArenaGlow` (a colour times a
strength into the emissive colour; a stage falls back to a bright plain surface when it is missing).
Everything that fights is spawned by the game mode.

## Stages

Where the fight is held is data: a stage in `Data/Battle/arenas.json` (ids and numbers only). Core
(`Core/include/ftcore/ArenaStage.h`) turns a stage and the fight it holds into a layout of boxes, cylinders,
lamps, a scene look and a camera rig, deterministically from the stage's `seed`; `FTArenaStage::Build` makes it
in the world on the first tick. `-ArenaStage=<id>` picks one; without it the default is `sand_ring`, which is the
old demo (a stone disc, a knee-high wall, eight pillars with a lamp each, the same ring, spawn and camera).

```json
{ "id": "solhaven_canal", "world": "solhaven", "theme": "canal", "bosses": ["hulk"],
  "floor": "circle", "bound": "water", "seed": 12,
  "features": { "cover": 6, "pillars": 6, "landmarks": 4, "pools": 1, "cracks": 4, "lights": 6,
                "lightIntensity": 1.2, "fog": 1 } }
```

- `world`: `none`, `solhaven`, `lattice`, `unwritten` (`ftcore::StylePack`). It sets the colours of the seven paint
  roles, the lamp colours, and the scene look (sun, sky light, fog, exposure, bloom). The worlds are tuned light
  and air on the photoreal base, not the post-process style packs, which stay off (STYLE_PACKS_UE.md).
- `theme`: the shape of the cover, pillars and landmarks: `plain`, `street` (crates, stacked towers), `canal`
  (bollards, arches), `circuit` (plinths, slim pylons), `grid` (plinths, tilted monoliths), `shore` (leaning
  slabs, broken columns), `void` (broken chunks, leaning monoliths).
- `bosses`: the enemy kinds it hosts, the default line-up (`-ArenaLineup` overrides it, and the scale follows
  whoever is really in the fight).
- `floor`: `circle`, `square` (the ring inside a square slab), `broken_ring` (a disc under the ring with slabs
  sinking away around it) or `island` (a disc on a stalk over water).
- `bound`: `wall` (a wall just outside the ring, ground beyond), `edge` (a kerb, then a drop), `water` (a low kerb,
  then still water) or `fog_wall` (tall dark curtains far out in thicker fog).
- `features`: counts of `cover`, `pillars` (with a lamp each when there are lamps), `landmarks` (set pieces
  well outside the bound), `pools` (water on the floor), `cracks` (glowing seams on the floor) and `lights`;
  `lightIntensity` multiplies the lamps and `fog` the world's fog.
- `ring`, `spawn`, `attackers` (all optional): pin the ring radius, the spawn distance or the attackers. Without
  them they come from the enemies' size.

### Scale

The stage grows with the biggest enemy in the fight. `k` is the larger of its `bodyRadius` / 0.5 m and its
`height` / 2.2 m (`height` is an optional key of an enemy kind in `arena.json`; without it the old
radius-derived height, 1.75 m at 0.4 m), held to 1..6: the shipped wave and a human duelist are 1, the 3.5 m
`hulk` 2.4, the 7 m `titan` 4.4. Then:

| | Rule |
|---|---|
| Ring radius | (8.5 m + 1.5 m per enemy past one + 0.75 m per attacker past one) x (0.4 + 0.6 k), held to 7..60 m (a pinned `ring` is 6..80 m) |
| Spawn distance | 0.58 of the ring, at least the biggest body radius + 4 m, at most 12 m (the lock reaches 15 m) and 0.9 of the ring |
| Props | `propScale` = 1 + 0.55 (k - 1): the wall's height and depth, the pillars' size and gaps, cover, landmarks (8 to 14 times it tall), the floor's rim (2.4 times it) and the lamps' height and reach |
| Camera | arm 6.4 m x (0.35 + 0.65 k), at most 4.2x; the pivot's height, the shoulder offset and the lock pitch (-20 toward -8 degrees) follow, so a giant's head stays in frame |
| Fog | thinned by 1 / (0.5 + 0.5 propScale), so a big boss stays readable |

The worlds' light is set in `Core/src/ArenaStage.cpp` (`worldLook`): Solhaven is a night (the sun under the horizon, a held
exposure, sodium and purple lamps, a faint blue water sheen), Lattice a bright blue sky with pale structure and glowing
blue seams and water, Unwritten thick fog and one or two cold lamps over grey, half-formed ground. The lamps are in
candelas (`SetIntensityUnits`): the old demo's unitless lamps were far dimmer than the same number in candelas, so the
generic world's lamps are scaled down to match. A held exposure means the worlds do not brighten toward the same grey;
if a world is retuned, retune its glow strengths with it (they are in the same units as the exposure).

The figure of an enemy is as tall as its `height` and as wide as its `bodyRadius` (up to 6 m). The numbers for
the shipped stages are printed by the `arena stage` tests and in the log line `Arena: built stage ...`.

### What the generator guarantees

The host moves bodies with `arenaStep`, which knows no props, so the layout keeps everything solid outside the
ring: the wall, cover, pillars, landmarks and fragments never touch it (cover is a decoration of the rim, not a
place to hide). Only floor-level decals (pools, cracks, loose slabs) lie inside the ring, and they keep out of
the player's start (3 m), each enemy's spawn disc (its radius + 2.5 m) and the lane between the starts (2 m +
the boss's radius). The player's start is at least the boss's radius + 3 m from every enemy's. Every stage in a
world has water (a water bound, an island, or a pool). `checkArenaStage` holds all of it, the doctests run it on
every stage and on every floor x bound x theme x world x boss size, and the host logs a broken layout
(`Arena: stage ... breaks a rule`). Pools and cracks are scenery: nothing in the fight reads them.

### The shipped stages

Two for each world, one human-sized (the `duelist`) and one large, plus the default. The bosses are generic
placeholders (a human-sized `duelist`, a 3.5 m `hulk`, a 7 m `titan`) built from the shipped enemies' numbers
scaled; the real ones come with the story.

| Stage | World | Boss | Floor, bound | Ring |
|---|---|---|---|---|
| `sand_ring` (default) | none | brute, skirmisher, breaker | circle, wall | 12 m |
| `solhaven_alley` | Solhaven | duelist | square, wall | 8.5 m |
| `solhaven_canal` | Solhaven | hulk | circle, water | 15.6 m |
| `lattice_court` | Lattice | duelist | square, edge | 8.5 m |
| `lattice_platform` | Lattice | titan | island, edge | 25.8 m |
| `unwritten_shore` | Unwritten | duelist | broken ring, fog wall | 8.5 m |
| `unwritten_maw` | Unwritten | hulk | island, fog wall | 15.6 m |

## Launch

Play it (a window, keyboard or pad):

```
UnrealEditor.exe ForgottenThird.uproject /Game/Maps/Demo_Arena -game -windowed -ResX=1600 -ResY=900
```

Watch the autopilot win, then quit (no window; prints the result line):

```
UnrealEditor-Cmd.exe ForgottenThird.uproject /Game/Maps/Demo_Arena -game -NullRHI -ArenaAutoPlay -ArenaQuitOnFinish -unattended -nosplash -nosound
```

The same with `-ArenaIdle` instead of `-ArenaAutoPlay` plays a player who presses nothing; it ends in
defeat. The default wave is long enough for the Load to fill and the autopilot to release; to fight one
enemy instead, pass its id: `-ArenaLineup=brute` (a single brute falls before the Load fills). From Git Bash, write the map as `Demo_Arena` (Git Bash rewrites a leading `/Game` into a path).

## Flow

1. The fight starts at once: the lead stands on the ring's west side facing the centre, the enemies
   spawn across from them.
2. Enemies close in and attack one at a time (`attackers` in the data). An attack starts up for at
   least 0.4 s before it can hit: that is the tell (see HUD below).
3. Answer it: parry (tap guard just before the hit) for a parryable move, block (hold guard) for one
   that can only be blocked, roll away from an unblockable one. A parry leaves the enemy open: hit it
   then for a riposte.
4. Hits drain the enemy's poise as well as its hp; at no poise it staggers and is open too.
5. Every parry stores Load (the parried blow's damage) in a second gauge beside stamina. Above a third of it
   the Release opens: one invulnerable strike that spends all the Load at once and breaks the poise of every
   enemy it reaches. At full Load the lead is overloaded: light, heavy and heal are locked until a Release
   (guard, parry, roll, moving and the lock still work).
6. The partner key is the second lead's press. Pressed within 0.12 s of a parry, before or after it, the two
   pair into a counter: the parried enemy takes three light hits' worth at once and staggers.
7. The Read is the second lead's other verb. As each enemy move starts up, three cards are dealt above its
   tell bar, each naming a shape of move (overhead, thrust, sweep, ...): the coming move and two decoys from
   the living enemies' other moves. Nothing pauses. Pick one (1 / 2 / 3) before the move goes active; the
   cards go when it does. Right: a Read stack (up to five) and, when the move's active part ends or a parry
   or stagger cuts it short, the partner strikes the attacker (half a light hit, a fifth more per stack past
   the first, and a little poise). Wrong: the stacks go back to none and the attacker regains some poise. While
   one deal is open, a move that starts elsewhere deals nothing.
8. The fight ends in VICTORY when every enemy is down, DEFEAT when the lead is. After a moment Enter,
   Space or pad A starts it again (the autopilot starts again by itself after 4 s unless told to quit).

## Controls

| | Keyboard and mouse | Gamepad |
|---|---|---|
| Move (camera-relative) | W A S D | Left stick |
| Look (when not locked) | Mouse | Right stick |
| Light attack (a chain of hits; on an open enemy, a riposte) | J or left mouse | X or RB |
| Heavy attack | K | A or RT |
| Guard (hold); parry (tap just before a hit) | I or right mouse | Y or LB |
| Roll (backstep with no direction) | L or Space | B |
| Heal (uses a charge) | H | D-pad up (not while Read cards are up) |
| Release (above a third of the Load) | R | LT |
| Partner (the second lead's press, paired with a parry) | F | D-pad down |
| Read (the second lead's pick, while the cards are up) | 1 / 2 / 3 | D-pad left / up / right |
| Lock on / off | Tab or middle mouse | R3 |
| Switch lock target | Q / E | D-pad left / right (not while Read cards are up), or flick the right stick |
| Fight again (after the outcome) | Enter or Space | A |
| Pause menu | Esc | Start |

The action keys are the four action buttons (`FTActionButtons.h`: Attack J / pad X, Guard I / pad Y,
Dodge L / pad B, Jump K / pad A), with the mouse and the shoulders as second keys. Presses are read
each frame and handed to the fight on its next step, so no press is lost between steps.

## HUD

- **Player panel**, bottom left: hp (red under a third, with the number), stamina, the Load gauge
  (orange, with its number and a white mark at a third where the Release opens; RELEASE READY above it;
  red, pulsing and OVERLOADED with the locked verbs when full), poise, the heal charges left and the Read
  stacks (five pips, cyan while held).
- **Target bar**, top centre: the locked enemy's hp and poise, or the nearest enemy's when nothing is
  locked.
- **Over each enemy:** a small hp bar and poise bar; STAGGERED while it reels, RIPOSTE! in gold while
  it is open.
- **The tell:** while an enemy's move starts up, a bar over it fills toward the hit, labelled with the
  answer: PARRY (gold) for a parryable move, BLOCK (orange) for one that can only be blocked, DODGE!
  (red) for an unblockable one. A ring closes in on the enemy's body as it fills, and an arc on the
  floor shows the move's reach. The moment the move goes active the bar turns white, NOW shows and a
  burst flashes. An enemy winding up off the screen shows as an arrow at the screen's edge in the
  tell's colour.
- **Read cards:** while a deal is open, three cards sit on the tell bar (the answer moves above them), each
  with its key number and the shape it names. The picked card turns green when right and red when wrong,
  the others dim. If the enemy is off the screen the cards show at the bottom centre instead.
- **Lock marker:** brackets and a dot on the locked enemy.
- **Popups:** damage beside the body hit (red on the lead, gold for a riposte), heals in green, and
  BLOCKED, GUARD BROKEN, PARRIED, EVADED, STAGGERED, DOWN and NO STAMINA; +N LOAD beside the lead when a
  parry stores Load, and COUNTER with its damage over an enemy the paired counter hits; READ xN (the
  stacks) or MISREAD beside the attacker on a pick, and READ COUNTER with its damage when the Read counter
  lands. The screen's edge flashes red
  when the lead is hit and gold on a parry.
- **Banners:** RELEASE with the Load it spent and an orange edge; OVERLOADED with the locked verbs.
- **Result:** VICTORY or DEFEAT, the fight's time and counts, and the fight-again prompt.
- The key hint runs along the bottom.

## Flags

| Flag | Effect |
|---|---|
| `-ArenaAutoPlay` | The Core autopilot (`ArenaFight::autopilot`) plays the lead and picks the Read (the right card 0.2 s into a startup), releasing at 50 Load; the host presses the partner's pair on the step after each parry. |
| `-ArenaIdle` | Nobody plays the lead (no input at all); wins over `-ArenaAutoPlay`. |
| `-ArenaQuitOnFinish` | Quit when the fight ends, after the result line (exit code 0). |
| `-ArenaSeed=N` | The fight's seed (default 1). |
| `-ArenaStage=<id>` | The arena, by id from `arenas.json` (see "Stages"); the default is `sand_ring`. Its bosses are the line-up, and the ring, spawn, props and camera are scaled to them. An unknown id logs the ids and uses the default. |
| `-ArenaLineup=a,b,...` | The enemies by id from `arena.json` (`brute`, `skirmisher`, `breaker`, `duelist`, `hulk`, `titan`), up to six, instead of the stage's own bosses (the sand ring's are `brute,skirmisher,breaker`); `-ArenaLineup=brute` fights one brute. The stage is scaled to whoever is in the fight. |
| `-ArenaAttackers=N` | How many enemies may attack at once (1 to the line-up's size). |
| `-ArenaHoldLoad` | The autopilot keeps its Load until it is overloaded (to see the overload; not a way to play). |
| `-ArenaShot=<path prefix>` | Autoplay and save the views (`_1_ring`, `_2_tell` with the Read cards picked, `_3_impact`, `_4_load` with the gauge past a third, or the overload under `-ArenaHoldLoad`, `_5_release`, `_6_result` .png), then quit; a view the fight never reaches is skipped. Needs a renderer (`-RenderOffScreen`, not `-NullRHI`). |
| `-ArenaShotDelay=S` | Seconds before the first screenshot (the fight waits for it). |
| `-ArenaOverview` | With `-ArenaShot`, a first view `_0_overview` of the whole stage from a camera high over the side with the widest gap between landmarks, HUD off. |

At the end of every fight the log gets one line with counts only:

```
ARENA RESULT: Victory in 22.9 s (autoplay, seed 1, 3 enemies, hp 96/140, 3 heals left; 25 hits, 3 taken, ...; 68 load stored, 1 releases, 0 overloads, 3 counter staggers; 10 reads dealt, 10 picked, 10 right, 10 read counters for 93; dealt 530, took 44).
```

A fight that runs past 180 s of fight time (or 360 s of wall time) logs `ARENA RESULT: Timeout` as a
warning and quits with exit code 2, so an unattended run never hangs.

## How it runs

- The fight steps at a fixed 1/60 s (at most 8 steps a frame); the figures are drawn between the last
  two steps, so the speed of the frame never changes the fight.
- The lead is `AFTArenaFighter` with the test human body; enemies are primitive figures in their kind's
  colour. Swings, guards, rolls, staggers and falls are posed from the fight's output each frame.
- The camera is a spring arm over the lead's shoulder, its length, height, shoulder offset and lock pitch from
  the stage's camera rig (6.4 m, 1.5 m, 1.2 m and -20 degrees for a human-sized fight, longer and higher for a
  giant). Locked on, it turns toward the target; otherwise the mouse or right stick turns it (the autopilot and
  an idle run face the nearest enemy).
- Nothing in the host allocates per frame: the HUD's words are static, and its numbers are rebuilt only
  when they change.
