# Arena demo

A playable version of the real-time arena fight: one lead against one to six enemies (by default a wave
of three, a brute, a skirmisher and a breaker, one attacking at a time; placeholder shapes) held inside invisible walls
in a zone of a world's own scene ("Zones" below), with a lock-on camera and a canvas HUD. The rules all live in
Core (`ftcore::ArenaFight`, `Core/include/ftcore/ArenaFight.h`); the enemies, their moves and the ring
come from `Data/Battle/arena.json`. See [COMBAT_SYSTEMS.md](COMBAT_SYSTEMS.md), "Arena melee", for the
rules themselves.

Map: `/Game/Maps/Demo_Arena` (Content/Maps/Demo_Arena.umap). The map's game mode is `AFTArenaGameMode`
(no story; HUD `AFTArenaHUD`; pawn `AFTArenaFighter`). The project's default map is unchanged.

The map is built from code, never by hand, and holds only a player start: a zone in a world is held in that world's own
map, which the game mode opens, and the plain zone gets its ground and look from the game mode at run time. After a change to
the commandlet, build the map again:

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildArenaWorld -NullRHI -unattended -nopause -nosplash
```

Everything that fights is spawned by the game mode's runner (`UFTArenaRunner`, a component): the game mode loads the
table, fits the zone and builds the ground, and the runner holds the fight, the figures, the keys, the camera and the
result line. The arena step's host in a world (below, "In a world") runs the same runner.

## In a world

A story fight that plays the arena system is fought in place on its scene's world map (`AFTArenaHost`, spawned by the
explore director on every world map; see [COMBAT_SYSTEMS.md](COMBAT_SYSTEMS.md), "Routing"). It starts after a field step
in place, or on a map opened for it, once the lead stands at the scene's district spawn.

- The zone is the one `arenas.json` anchors on the step's world and district, with its bosses; else a zone on that
  district's plaza with the default zone's line-up (`<world>_d<district>` in the result line). It is fitted to the
  world's real layout like a `-ArenaStage` zone; a district with no room gives a plain zone about the lead.
- The lead plays its own fighter kit (the explore character's look, `GetLeadIndex()`).
- The explore character steps out: hidden, its collision and movement off. The controller takes the lead's figure,
  which wears the same look, and the arena HUD replaces the explore HUD.
- The followers are held and stand at `arenaParkSpot()` behind the lead's start, pulled inside the zone's walls.
- The result stays up for 3 s (1 s under the autopilot). Then the explore character comes back where the figure
  stands, the explore HUD and the party come back, the step is marked done (won or lost), and the beat's next step
  starts in place or the story map opens (`FTStep::ContinueHere`). There is no fight-again.
- Without a story step, `-ArenaHere` starts a standalone fight in the lead's district (with `-WorldDistrict=<n>` to pick
  it); `-ArenaAutoPlay`, `-ArenaIdle` and `-ArenaQuitOnFinish` work as on `Demo_Arena`. A story step's autoplay comes from
  its carry, and only a standalone fight may quit the process. The host logs one setup line, `Arena: in place on <map>,
  district <n>: zone <id>, kit <id>, ...`, then the usual result line.
- Not yet: the zone's dressing (its water sheets and the road barriers) is the demo's only.

```
UnrealEditor-Cmd ForgottenThird.uproject World_Solhaven -game -NullRHI -ArenaHere -ArenaAutoPlay -ArenaQuitOnFinish -WorldDistrict=3 -unattended -nosplash -nosound -stdout
```

## Zones

Where the fight is held is data: a zone in `Data/Battle/arenas.json` (ids and numbers only). A zone is a portion of
a world's own scene, a plaza or a street between buildings: nothing is built for it, the fight happens in the world map
with the world's art and light, and the player, the boss and the minions are held inside the zone's invisible walls.
`-ArenaStage=<id>` picks one. Without it the default is `sand_ring`, a plain 12 m circle of flat ground (the old demo's
ring, spawn and camera, with the shared look and no props) so `Demo_Arena` plays with no flag.

For a zone in a world the game mode opens that world's map (`World_Solhaven`, `World_Lattice`, `World_Unwritten`) with
itself as the game mode (`?game=`), takes the map's explore director out before it begins play, finds the ground height
there with a trace onto the Landscape (the zone stands on one district's flat pad, so it is one height), and runs the
fight. The map's own lighting stays.

```json
{ "id": "sol_facade_a", "world": "solhaven", "district": 2, "bosses": ["duelist"],
  "offset": [0, 34], "yaw": 90, "shape": { "kind": "rect", "aspect": 1.6, "corner": 0.3 }, "seed": 12,
  "water": [ { "at": [7, 24], "radius": 3 } ] }
```

- Anchor: `world` (a `worlds.json` id) with a `scene` (one of its scene ids) or a `district`, or an `at` [x, y] in world
  metres. A district's frame has its plaza centre as the origin, +x the way its spawn faces (toward the world's
  centre) and +y to the left; `offset` is in that frame. The log prints each zone's district centre, and the Core test
  `arena zone frame` prints every district's centre, facing and landmark, so coordinates can be checked.
- `shape`: `circle`, `rect` (rounded rectangle: `aspect` long over short, `corner` of the short half) or `polygon`
  (`points`, in multiples of the ring radius, convex). `yaw` (degrees, in the anchor's frame) turns the shape and
  also the lane the fight starts along: the player starts behind the centre against it and the boss ahead.
- `bosses`: the enemy kinds it hosts, the default line-up (`-ArenaLineup` overrides it; the zone is fitted to whoever
  is in the fight). `ring`, `spawn`, `attackers`: pin the ring radius, the spawn distance or the attackers; without them
  they come from the enemies' size.
- `water`: shallow sheets (`at`, `radius`) in the anchor's frame; they are drawn and never block. The plazas are dry
  paving and the Unwritten has no water of its own, so a zone brings some.
- `cover`: things that block both sides (`kind` circle with `radius`, or box with `half` and `yaw` degrees, `at`), drawn
  as plain crates and posts. A plaza has none of its own.

### Size and fit

The zone's size follows its boss, as the old rings did: `k` is the larger of the biggest enemy's `bodyRadius` / 0.5 m
and `height` / 2.2 m (`height` is an optional key of an enemy kind in `arena.json`), held to 1..6. The ring asked for
is (8.5 m + 1.5 per enemy past one + 0.75 per attacker past one) x (0.4 + 0.6 k), held to 7..60 m: a duelist 8.5 m, the 3.5 m
hulk 15.6 m, the 7 m titan 25.8 m. The spawn distance is 0.58 of it, at least the biggest radius + 4 m and at most 12 m (the
lock reaches 15 m). The camera arm is 6.4 m x (0.35 + 0.65 k) (at most 4.2x; 6.4, 12.2 and 20.5 m for those three), with
the pivot, shoulder offset and lock pitch following.

`fitArenaZone` (Core, `ArenaZone.h`) then puts that size in the world's real layout (`generateWorldLayout`):

- A building the shape crosses **cuts** it: a half-plane along the face that costs the least area, the zone's centre staying
  on the open side, with 0.6 m between wall and face. So a zone pressed on a facade becomes a polygon with a flat wall
  along it, and the wall never runs through a building.
- A building that stands wholly inside the zone, the district's landmark (r 4 m) and every tree or rock touching it are
  **obstacles**: boxes and circles that block both sides.
- The size may shrink up to three steps (0.88, 0.77, 0.68), and the centre may move up to two steps off the anchor, until
  what is left is room enough: at least half the area of its circle, an inscribed radius of the biggest body radius + 3 m,
  a size of at least 0.65 of what was asked, and wholly on the district's flat pad (61 m from its plaza centre). A shrink
  or a move is logged as a warning; no room at all is a refusal (the host then uses the plain zone, with a warning).
- The lanes: the fit tries a fan of 16 lanes, the longest spawn first, until the player's start and every enemy's are
  inside the walls, clear of obstacles, and the lines between them are clear. Nothing the fit finds ever stands on a start
  or a lane.

### The walls

`arenaStep(fight, bodies, dt, zone)` is the old `arenaStep` plus the zone: every body is held inside the shape (shrunk by
its own radius) and pushed out of every obstacle, by Core, with no Unreal blocking volume. Core's fight still judges the same
fight in the zone's bounding circle (`ArenaZoneFit::ring`). A body that walks into a wall slides along it, and in a zone in a
world it is **braked** over the last 1.5 m to 60% of its speed (no hit-stop; the plain zone is the old ring and does not
brake, so the default fight is step for step the old one). Velocity is not cut at obstacles, and the enemies do not path
around them: one pressing into a pillar stays put.

The wall is learned from the air, never drawn as a line: within about 3 m of it the HUD adds the world's own weather,
in the world's colours: rain streaks falling through sodium and violet haze in Solhaven, a faint blue ruled grid that pulses
in the Lattice, paper-edge fog bars in the Unwritten (dust in sand on the plain zone). Where the wall crosses a **road** (the
fit lists these mouths) the host puts a row of crates across it, just outside, so the wall is a thing there.

### Camera

The camera stays on the boss: the lock turns it, never to show the wall. It may leave the zone but not pass through the
scene: the spring arm now probes the world (`ECC_Camera`) and is pulled in. `cameraBlocked` (a fit result, logged as a
warning when above 0) is the share of the zone's edge where a player standing on it would have the camera inside a building
taller than the camera, so a bad spot is known before it is played: a giant at a tower plaza is 67% (`lat_plaza_a` with
`-ArenaLineup=titan`), a giant's climax zone 0 to 15%, the human-sized facades 27%.

### The shipped zones

District-frame offsets from the level layout notes (ids and numbers only). Each runs with any boss; the default line-up is
the one listed.

| Zone | World, district | Offset | Shape | Yaw | Boss | Ring |
|---|---|---|---|---|---|---|
| `sand_ring` (default) | none | 0, 0 | circle | 0 | brute, skirmisher, breaker | 12 m |
| `stone_ring` | none | 0, 0 | circle, 12 cover posts | 0 | stone_bearer | 18 m (pinned) |
| `kite_ring` | none | 0, 0 | rect 2.86, c 0.15 | 0 | kite_boss | 13.4 m (pinned), about 40 x 14 m |
| `sol_plaza_a` | Solhaven 3 | 0, 0 | circle | 180 | hulk | 15.6 m |
| `sol_facade_a` | Solhaven 2 | 0, 34 | rect 1.6, c 0.3 | 90 | duelist | 8.5 m, cut |
| `sol_climax_a` | Solhaven 14 (climax) | 22, 0 | circle | 0 | titan | 36 m (pinned) |
| `lat_plaza_a` | Lattice 6 | 0, 0 | circle | 180 | hulk | 15.6 m |
| `lat_facade_a` | Lattice 5 | 0, -34 | rect 1.6, c 0.3 | -90 | duelist | 8.5 m, cut |
| `lat_climax_a` | Lattice 1 (climax) | 22, 0 | circle | 0 | titan | 36 m (pinned) |
| `unw_plaza_a` | Unwritten 3 | 0, 0 | circle | 180 | duelist | 8.5 m |
| `unw_court_a` | Unwritten 3 | 0, 0 | circle | 180 | hulk | 54 m (pinned) |
| `unw_climax_a` | Unwritten 4 (climax) | 22, 0 | circle | 0 | titan | 36 m (pinned) |

The climax zones span the open half of the climax district's ring, up to the pad's rim; the court's ruin walls become cover and
its landmark an obstacle. Zones off the flat pad (a quay, a pool, the rim of the square, open ground) come later: they need a
ground-height query and a slope check in place of the flat-pad one.

## Skinned enemies and the kite boss

An enemy kind in `arena.json` may be drawn as a skeletal mesh with its own animation clips instead of the placeholder shapes.
Everything about it is data and host-side: the fight's rules never read it.

```json
"look": { "clips": "Data/Anim/kite_clips.json", "hover": 0.9, "scale": 1, "yawOffset": -90 }
```

- `look.clips`: the project-relative path (under `Data/`) of a clip table in the beat clips' format (`id`, `asset`, `length_s`,
  `impact_s`, `loop`; see [BEAT_ANIMS.md](BEAT_ANIMS.md)) with one extra top-level key, `mesh`, the skeletal mesh asset path.
  `hover` (m) lifts the mesh above the enemy's position on the ground (the mesh's origin is its bottom; the tell arcs stay at
  ground level), `scale` scales it, `yawOffset` (degrees) turns it from its own facing (`FTMeshFacing`, which finds none on a
  mesh with no head or feet bones: set it by eye; a glTF export whose front is Blender -Y needs -90).
- A move's `clip`: the id of the clip played for it (`tell` stays the Read card's word, one of the nine). Ids are 1..31
  letters, digits, `_` and `-`, one per move.
- A kind's `bpm`: the beat its startups are whole numbers of, in place of the file's (the kite's 72 against the file's 150).
- Up to 24 moves per kind (it was 8): a boss move of n impacts is n chained moves. The storage is fixed, so an
  `ArenaDef` grew from about 7 to 17.4 KiB and an `ArenaFight` from about 10 to 23 KiB (MSVC x64).

The mode plays, from Core's state each frame (`UFTBeatFighterAnimInstance`, a pose that is a function of the fight, with a 0.12 s
crossfade between clips, or the table's own top-level `blend_s`): a move's own clip through an attack, timed by Core's clock for the move at the rate
`impact_s / startup` (so the blow lands as the startup ends; held on its last frame if the clip is shorter than the move);
`stagger` while staggered; `face_down` (looping) while open after a parry or down after a riposte; `hit_light` or `hit_heavy`
(a heavy blow, a riposte or a Release) between moves; `walk` (looping, optional) between moves while the body moves, once its
smoothed ground speed passes 0.3 m/s and until it drops under 0.15 m/s, played at that speed over the row's `speed_mps` (rate 0.5 to
1.6; a walk row without `speed_mps` is left out); `defeat` on death, held on its last frame; else `idle`, or `idle_p2` under
60% hp. A clip the table lacks is left out (that state plays the idle). If the clip table, the mesh or the idle clip doesn't load, the
kind keeps its shapes and the log has one warning. The camera looks up at a boss at least 4 m tall (hover included) toward 70%
of its height, up to 15 degrees above level (the free camera's limit too); the camera never pulls out for a big boss. The assets are local; nothing here needs them to load.

The kite boss (`kite_boss`, `Data/Anim/kite_clips.json`, assets under `/Game/Bosses/KiteBoss/kite_boss/SkeletalMeshes/`,
the import being `FTImportCharacter -Source=kite_boss.glb -Dest=/Game/Bosses/KiteBoss -Face=0`): 1100 hp, 9.1 m from its
finial to its tail tip (the whole figure, `height` 9.1) and hanging 0.9 m off the ground, drifting at 0.6 to 1 m/s. The mesh is a
rebuild of the first one with the same 48-bone rig and the same 23 clips, so the clip table did not change. Eight moves, each its
clip's id: `mitten_drop`, `swoop`, `peg_spray`,
`snag`, `wet_sheet`, `undertow`, `string_flick`, `sheet_slap` (Read card words slam, sweep, strike, charge, overhead, thrust,
slash, backhand). Core has no "parry-opener" flag or multi-hit, so the marked move is made by `parryable`: only `swoop` (and the
two basic strikes) can be parried; a parry of it opens the boss, the big ones are blocked or dodged, and `peg_spray` is one hit.
`swoop` has weight 20 and a cooldown of 8 s, which comes to about one every 11 s. No phase system exists, so the second-phase moves
(`*_double`, `swoop_feint`, `rockabye`) are not wired; `idle_p2` is the only second-phase clip played.

Its weak point is the smile seam on the paper face, `seam` in `Data/Battle/arena_boss/kite_boss_weak.json`, read with
`importArenaWeakPoints` (a standalone `{ "version": 1, "weakPoints": { ... } }` file: the block `importArenaBoss` reads from a boss
file, because the kite keeps arena.json's moves and has no boss file of its own). One point on the front (`facing` 0, `arc` 60:
the lead must stand within 60 degrees of the face), radius 0.06, multiplier 2.0, in figure space (the unit is the 9.1 m height, y
up from the mesh's bottom; the 0.9 m hover lifts the whole mesh, so y does not count it). Its `y` is the seam's height above the
mesh bottom over 9.1 m (provisional until the final mesh is measured). It is hosted by `FFTKiteHost`
(`Source/ForgottenThird/FTKiteHost.*`, no Unreal headers), owned by the runner when the line-up holds a `kite_boss`:

- the swoop's parry (any Parried of the kite) opens a `CritWindow` for the kind's `openTime`, a Staggered for its `staggerTime`;
  the window shuts at its end;
- in the blow hook a lead's blow on the kite is judged locked on, as the stone_bearer's are (`judgeLockOn`: the gate then the
  point), and a crit scales the blow's damage and poise by the point's multiplier. The fight has one blow hook, so the runner
  gives the kite no host (one warning) in a fight that has the stone_bearer's;
- notes, one log line each as `Arena: kite ...`: crit opened (until when), crit landed (the point, its multiplier, `first` for
  the first of an opening), crit closed; also each telegraph (a kite AttackStarted) and each hit taken, at Verbose. The weak
  point's mesh socket is `seam` (`FFTKiteHost::WeakPointSocket`; the rig's bone has the same name).

Stubs, each a named hook that only leaves a note, for the effects step to spawn Niagara systems from: `OnTelegraph(move, t)`,
`OnCritOpened`, `OnCritLanded`, `OnHitTaken`.

Play it in the 40 x 14 m ring:

```
UnrealEditor.exe ForgottenThird.uproject /Game/Maps/Demo_Arena -game -windowed -ResX=1600 -ResY=900 -ArenaStage=kite_ring
```

or in the plain 12 m sand ring with `-ArenaLineup=kite_boss` instead of `-ArenaStage=kite_ring` (cramped for a 9 m boss).

## The stone_bearer

The first Arena boss (`stone_bearer`, `Data/Battle/arena_boss/stone_bearer.json`) is one `ftcore::StoneBearer` beside its
ArenaFight body, hosted by `FFTStoneBearerHost` (`Source/ForgottenThird/FTStoneBearerHost.*`, no Unreal headers) as
`StoneBearer.h`'s host section asks. When the zone's line-up holds it, the runner:

- rebuilds the kind's moves from the boss file before `Fight.begin` (one chained arena move per impact, at most 24; a
  follower has weight 0; startups from the beat gaps; tile moves strike all round, lane moves a 1 m lane, called moves a
  front arc; only `Any` moves can be parried, dodge, jump, tile and cover moves can't be blocked);
- `begin`s the boss with the fight, sets the hp floor at the pedestal's share (so no blow skips it) and hooks the blows;
- on each AttackStarted `startMove`s the boss move (side from `arenaSideOf`); a move the boss refuses (invulnerable, down,
  or not in its phase) is cut by a 0.05 s Down hold and its blows are vetoed, and it picks again;
- each step: the Intercept and Guard buttons (zone from `arenaZoneOf` at the move's start), then Blocked → `onBlocked`,
  the boss's Staggered → `onStagger`, Released → `onRelease` with the Load before it was spent, then `update`;
- in the blow hook: the lead's blows through `onHit` (its damage and poise multipliers), the boss's blows scaled by
  `damageScale()` and capped per move, an intercepted called blow vetoed, the guest's keep-out clamps (`clampArea`,
  `clampLane`, `clampArc`);
- the pedestal: at its share the boss is held Down (drawn standing) on a 0.5 m plinth at the zone's centre; a Release over a
  third of the Load on the guest's call lifts it (floor off, Down for `downTime`). The boss offers no parries there, so a
  lead who reaches it with too little Load is topped up to one Release (`giveLoad`). CritOpened holds it Open until
  `until` (a riposte invites), CritClosed ends that;
- a full intercept's Load (the marked throw's `LoadGained`) is given to the lead with `ArenaFight::giveLoad`;
- the carried piece breaks (`Broke`, and the runner puts its halves in the fists) on the boss's first Stagger, or when its
  hp share first falls to `breakable.at` (0.4), whichever comes first. The hp break cancels no move and comes even with
  the poise frozen, so every fight that gets that far plays it.

The kind's hp, poise, staggerTime and openTime come from the boss file (the kind's own values in `arena.json` are
overwritten); the rest of the kind, `poiseDelay` among it, comes from `arena.json`.

Its weak point (`nape` in the json, on the lower back) is the mesh socket `setstone`
(`FFTStoneBearerHost::WeakPointSocket`); any marker or crit VFX uses that name.

Stubs, each a named hook that logs: `OnBroke` (no breakable prop yet), `OnShockRing` (no second ring blow), `OnGuestCall`
(no guest voice or figure), `OnTileRain` (one all-round blow, no falling tiles), and stone pickups (`onSoloLift` is never called: no pickups in the zone).

```
UnrealEditor.exe ForgottenThird.uproject /Game/Maps/Demo_Arena -game -windowed -ResX=1600 -ResY=900 -ArenaStage=stone_ring
```

Add `-ArenaAutoPlay` (the autopilot, with the host pressing Intercept and Guard on the call and releasing on the plinth) or
`-ArenaIdle`. Intercept is U, the mouse's thumb button or the left stick's click, held; the boss's Guard is the guard.

## Boss demo: the bosses with the three leads

The boss meshes and their clips are local assets (`Content/Bosses/`, never committed: the main checkout holds untracked
ones, and committing `.uasset` files there would make a merge fail on them). Each is one `FTImportCharacter` run from the
boss's `.glb`; the file's stem names the asset folder, so the glb must be called `<stem>.glb`. A boss kind whose assets
are missing keeps its placeholder shapes and the log has one warning. Run from the project folder, with the editor
closed:

| Boss | Kind | Zone | Import |
|---|---|---|---|
| the Unbending | `stone_bearer` | `stone_ring` | `-Source=<path>/stone_bearer.glb -Dest=/Game/Bosses/StoneBearer -Face=0` |
| the Kite-Mother | `kite_boss` | `kite_ring` | `-Source=<path>/kite_boss.glb -Dest=/Game/Bosses/KiteBoss -Face=0` |
| the Dealer | `white_glove` | `stone_ring` with `-ArenaLineup=white_glove` | `-Source=<path>/white_glove.glb -Dest=/Game/Bosses/WhiteGlove -Face=0` |

```
UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTImportCharacter <the import column> -NullRHI -unattended -nopause -nosplash
```

The Unbending also needs its slab halves, which its Broke split puts in the fists: `UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTImportProp -Source=<path>/slab_halves.glb -Dest=/Game/Props/StoneBearerHalves -NullRHI -unattended -nopause -nosplash`.

An import replaces what is there, so rerunning it after a boss's model changes is safe. Exit code 0 is a clean import (the
log's "face bones" line is a warning for a boss, which has no human head).

Pick the lead with `-ArenaLead=Devin`, `-ArenaLead=Duy` or `-ArenaLead=Third` (the name of a `PartyLooks` entry in the local
`Config/UserGame.ini`, any case); each plays its own fighter kit (Devin the blade, Duy kicks and a gun, the Third spells).
Without the flag the lead is the first look (Devin in the main checkout).

```
UnrealEditor.exe ForgottenThird.uproject /Game/Maps/Demo_Arena -game -windowed -ResX=1600 -ResY=900 -ArenaStage=stone_ring -ArenaLead=Devin
UnrealEditor.exe ForgottenThird.uproject /Game/Maps/Demo_Arena -game -windowed -ResX=1600 -ResY=900 -ArenaStage=kite_ring -ArenaLead=Duy
UnrealEditor.exe ForgottenThird.uproject /Game/Maps/Demo_Arena -game -windowed -ResX=1600 -ResY=900 -ArenaStage=stone_ring -ArenaLineup=white_glove -ArenaLead=Third
```

`-ArenaStage=stone_ring` hosts `stone_bearer` and `-ArenaStage=kite_ring` hosts `kite_boss`; `-ArenaLineup=<kind>` fights a
kind in whatever zone `-ArenaStage` names (the sand ring by default). `-ArenaAutoPlay` has the autopilot play the lead.
For a picture with no window: `-RenderOffScreen -ArenaShot=<path prefix> -ArenaShotDelay=8` in place of `-windowed`
(needs a renderer, not `-NullRHI`).

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
| Intercept a called blow (stone_bearer; hold) | U or mouse thumb button | L3 |
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
- **Boss panel** (stone_bearer), under the target bar: WEIGHT and its stacks as five pips; INVULNERABLE and "R on the call"
  on the plinth, WEAK POINT OPEN, DOWN, or CALL and the called zone (LEFT, RIGHT, PATH; green when the lead stands in it);
  and the latest answer or rule, fading: INTERCEPT, GUARD, WRONG SIDE, MISSED, BROKEN, SHOCK RING, CRITICAL, LIFTED.
- **Over each enemy:** a small hp bar and poise bar; STAGGERED while it reels, RIPOSTE! in gold while
  it is open.
- **The tell:** while an enemy's move starts up, a bar over it fills toward the hit, labelled with the
  answer: PARRY (gold) for a parryable move, BLOCK (orange) for one that can only be blocked, DODGE!
  (red) for an unblockable one. A ring closes in on the enemy's body as it fills, and an arc on the
  floor shows the move's reach. A shot (a move with a `width`, a straight lane) shows two dashed rails
  instead, the lane's edges for the player's centre, running out from the shooter to its reach; step
  outside them before it fires and it misses. The moment the move goes active the bar turns white, NOW shows and a
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
| `-ArenaStage=<id>` | The zone, by id from `arenas.json` (see "Zones"); the default is `sand_ring`. A zone in a world opens that world's map. Its bosses are the line-up, and the walls, spawn, obstacles and camera are fitted to the enemies in it. An unknown id logs the ids and uses the default. |
| `-ArenaLineup=a,b,...` | The enemies by id from `arena.json` (`brute`, `skirmisher`, `breaker`, `duelist`, `hulk`, `titan`, `spitter`), up to six, instead of the stage's own bosses (the sand ring's are `brute,skirmisher,breaker`); `-ArenaLineup=brute` fights one brute. The stage is scaled to whoever is in the fight. |
| `-ArenaLead=<name>` | The lead the player plays. Its look is the entry of that name in the explore character's `PartyLooks` (`Config/UserGame.ini`, local), else the first; with none, or none whose body loads (one warning), it is the test human. Its fighter kit is the `arena.json` fighter of that name in lower case (`devin`, `duy`, `third`), even when no look loads; without the flag it is the first look's name, and an id with no kit plays `default`. The start line names the kit (`kit devin`). |
| `-ArenaAttackers=N` | How many enemies may attack at once (1 to the line-up's size). |
| `-ArenaHoldLoad` | The autopilot keeps its Load until it is overloaded (to see the overload; not a way to play). |
| `-ArenaShot=<path prefix>` | Autoplay and save the views (`_1_ring`, `_2_tell` with the Read cards picked, `_3_impact`, `_4_load` with the gauge past a third, or the overload under `-ArenaHoldLoad`, `_5_release`, `_6_result` .png), then quit; a view the fight never reaches is skipped. Needs a renderer (`-RenderOffScreen`, not `-NullRHI`). |
| `-ArenaShotDelay=S` | Seconds before the first screenshot (the fight waits for it). |
| `-ArenaOverview` | With `-ArenaShot`, a first view `_0_overview` of the whole zone from a camera high over its south-west side (pulled in along the line if a building is in the way), HUD off, with the zone's walls (green), obstacles (red) and road mouths (yellow) drawn as debug lines for the picture only; then `_0b_edge`, the player 1.4 m from the wall on his left to show the edge cue. |

At the end of every fight the log gets one line, ids and counts only, never text, as the beat screen logs its own ([BEAT_SCREEN.md](BEAT_SCREEN.md), "Log lines"). Core formats it into a fixed buffer (`ftcore::arenaResultLine`):

```
Arena: VICTORY at <scene> beat <b> step <s> after <t> s, zone <zone>, kit <kit>, enemies <id>+<id>+<id>, <pilot> seed <n>, hp <h>/<max>, <x> hits, <y> taken, <b> blocks, <p> parries, <r> ripostes, <e> evades, <g> guard breaks, <s> staggers, <h> heals, <l> releases, <o> overloads, <c>/<d> reads right, dealt <n>, took <n>, <r> restarts.
Arena: DEFEAT at <scene> beat <b> step <s> after <t> s, ...
```

A fight on its own logs `standalone` for the scene, with beat and step 0; a fight a story step carried here logs that step's scene, beat and step. `<t>` is the clock of the last try; the counts and the restarts run on through retries. The enemies are their `arena.json` kinds (`built-in` for the built-in fight's), the pilot is `autoplay`, `idle` or `player`. An id is cut to 48 characters, and a space or a comma in it becomes `_`, so the line always splits the same way.

A fight that runs past 180 s of fight time (or 360 s of wall time) logs the same line with `TIMEOUT` as a
warning and quits with exit code 2, so an unattended run never hangs.

## How it runs

- The fight steps at a fixed 1/60 s (at most 8 steps a frame); the figures are drawn between the last
  two steps, so the speed of the frame never changes the fight.
- The lead is `AFTArenaFighter` wearing a look from `PartyLooks` (body, face, outfit and grooms; idle, walk and run by speed
  from the look's retargeted locomotion clips, `UFTLocomotionAnimInstance`), else the test human; there are no combat clips for
  the look yet, so its swings, guard and roll stay the figure's own overlay (a blade on a pivot arm, a shield plate, a
  crouch and lean). Enemies are primitive figures in their kind's colour, or a skinned mesh (above). Swings, guards, rolls, staggers and falls are posed from the fight's output each frame.
- The camera is a spring arm over the lead's shoulder, its length, height, shoulder offset and lock pitch from
  the zone's camera rig (6.4 m, 1.5 m, 1.2 m and -20 degrees, the same for every fight: a giant is found by locking
  on or looking up, from -50 to +15 degrees). The arm probes the world (it may leave the zone's walls but never passes through a building or the
  ground) and is pulled in. Locked on, it turns toward the target and holds it; otherwise the mouse or right
  stick turns it (the autopilot and an idle run face the nearest enemy).
- Nothing in the host allocates per frame: the HUD's words are static, and its numbers are rebuilt only
  when they change.
