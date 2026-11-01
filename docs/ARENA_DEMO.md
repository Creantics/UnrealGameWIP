# Arena demo

A playable version of the real-time arena fight: one lead against one to six enemies (by default one
brute; placeholder shapes) in a round ring, with a lock-on camera and a canvas HUD. The rules all live in
Core (`ftcore::ArenaFight`, `Core/include/ftcore/ArenaFight.h`); the enemies, their moves and the ring
come from `Data/Battle/arena.json`. See [COMBAT_SYSTEMS.md](COMBAT_SYSTEMS.md), "Arena melee", for the
rules themselves.

Map: `/Game/Maps/Demo_Arena` (Content/Maps/Demo_Arena.umap). The map's game mode is `AFTArenaGameMode`
(no story; HUD `AFTArenaHUD`; pawn `AFTArenaFighter`). The project's default map is unchanged.

The map is built from code, never by hand. After a change to the commandlet or to
`AFTArenaGameMode::MapRingRadius`, build it again:

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildArenaWorld -NullRHI -unattended -nopause -nosplash
```

It makes a stone disc with the ring's edge painted on it, a knee-high wall just outside the ring (the
camera looks over it), eight pillars with a lamp each, the shared sky and sun, and a player start.
Everything that fights is spawned by the game mode.

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
defeat. A single brute falls before the Load fills; to see a Release, fight a wave:
`-ArenaLineup=brute,skirmisher,breaker`. From Git Bash, write the map as `Demo_Arena` (Git Bash rewrites a leading `/Game` into a path).

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
7. The fight ends in VICTORY when every enemy is down, DEFEAT when the lead is. After a moment Enter,
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
| Heal (uses a charge) | H | D-pad up |
| Release (above a third of the Load) | R | LT |
| Partner (the second lead's press, paired with a parry) | F | D-pad down |
| Lock on / off | Tab or middle mouse | R3 |
| Switch lock target | Q / E | D-pad left / right, or flick the right stick |
| Fight again (after the outcome) | Enter or Space | A |
| Pause menu | Esc | Start |

The action keys are the four action buttons (`FTActionButtons.h`: Attack J / pad X, Guard I / pad Y,
Dodge L / pad B, Jump K / pad A), with the mouse and the shoulders as second keys. Presses are read
each frame and handed to the fight on its next step, so no press is lost between steps.

## HUD

- **Player panel**, bottom left: hp (red under a third, with the number), stamina, the Load gauge
  (orange, with its number and a white mark at a third where the Release opens; RELEASE READY above it;
  red, pulsing and OVERLOADED with the locked verbs when full), poise and the heal charges left.
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
- **Lock marker:** brackets and a dot on the locked enemy.
- **Popups:** damage beside the body hit (red on the lead, gold for a riposte), heals in green, and
  BLOCKED, GUARD BROKEN, PARRIED, EVADED, STAGGERED, DOWN and NO STAMINA; +N LOAD beside the lead when a
  parry stores Load, and COUNTER with its damage over an enemy the paired counter hits. The screen's edge flashes red
  when the lead is hit and gold on a parry.
- **Banners:** RELEASE with the Load it spent and an orange edge; OVERLOADED with the locked verbs.
- **Result:** VICTORY or DEFEAT, the fight's time and counts, and the fight-again prompt.
- The key hint runs along the bottom.

## Flags

| Flag | Effect |
|---|---|
| `-ArenaAutoPlay` | The Core autopilot (`ArenaFight::autopilot`) plays the lead, and the host plays the partner: it presses the pair on the step after each parry. |
| `-ArenaIdle` | Nobody plays the lead (no input at all); wins over `-ArenaAutoPlay`. |
| `-ArenaQuitOnFinish` | Quit when the fight ends, after the result line (exit code 0). |
| `-ArenaSeed=N` | The fight's seed (default 1). |
| `-ArenaLineup=a,b,...` | The enemies by id from `arena.json` (`brute`, `skirmisher`, `breaker`), up to six. |
| `-ArenaAttackers=N` | How many enemies may attack at once (1 to the line-up's size). |
| `-ArenaHoldLoad` | The autopilot keeps its Load until it is overloaded (to see the overload; not a way to play). |
| `-ArenaShot=<path prefix>` | Autoplay and save the views (`_1_ring`, `_2_tell`, `_3_impact`, `_4_load` with the gauge past a third, or the overload under `-ArenaHoldLoad`, `_5_release`, `_6_result` .png), then quit; a view the fight never reaches is skipped. Needs a renderer (`-RenderOffScreen`, not `-NullRHI`). |
| `-ArenaShotDelay=S` | Seconds before the first screenshot (the fight waits for it). |

At the end of every fight the log gets one line with counts only:

```
ARENA RESULT: Victory in 26.6 s (autoplay, seed 1, 3 enemies, hp 104/140, 3 heals left; 28 hits, 2 taken, ...; 92 load stored, 1 releases, 0 overloads, 5 counter staggers; dealt 530, took 36).
```

A fight that runs past 180 s of fight time (or 360 s of wall time) logs `ARENA RESULT: Timeout` as a
warning and quits with exit code 2, so an unattended run never hangs.

## How it runs

- The fight steps at a fixed 1/60 s (at most 8 steps a frame); the figures are drawn between the last
  two steps, so the speed of the frame never changes the fight.
- The lead is `AFTArenaFighter` with the test human body; enemies are primitive figures in their kind's
  colour. Swings, guards, rolls, staggers and falls are posed from the fight's output each frame.
- The camera is a spring arm over the lead's shoulder. Locked on, it turns toward the target; otherwise
  the mouse or right stick turns it (the autopilot and an idle run face the nearest enemy).
- Nothing in the host allocates per frame: the HUD's words are static, and its numbers are rebuilt only
  when they change.
