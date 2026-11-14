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
scene: the spring arm now probes the world (`ECC_Camera`) and is pulled in. Locked (or following, under the autopilot),
the view also orbits round the lead (`ftcore::arenaFrameTurn`, `ArenaLook.h`) so the target sits on the view's centre line
and the lead keeps its place left of it, rather than the lead hiding a close target; the turn is at most 30 degrees,
eases in and out, is 0 while aiming, and goes into the free camera's yaw when the lock ends. The arm, the pivot and the
shoulder are never touched, so it never pulls the camera out, and the hit pause holds the turn where it is. The stick, the
crosshair and the fight's view all read the view as drawn (the lock's line plus the turn, `ftcore::arenaFrameYawStep`),
so the lead goes where the stick points on the screen, and a release, which folds the turn into the free camera's yaw,
leaves a held stick's heading where it was. The settings menu's **Lock framing** row (on by default, `lockFraming` in the
settings file) turns the orbit off. A zone may hold the turn lower with `frameTurnCapDeg` in `Data/Battle/arenas.json`
(degrees, 0 to 30; left out, the global 30): `lat_facade_a` sets 0, so its view stays on the lock's line, because turned
its lens would sit in a building (the fit reads 16/48 of its edge blocked at the default cap). The host's camera (yaw,
boom, tilt, arm, pivot, shoulder, height, fov) is `ftcore::arenaCameraPose`, from the start of the fight on.
`cameraBlocked` (a fit result, logged as a warning when above 0) is the share of the zone's edge where a player standing
on it, facing in, would have the lens inside a building taller than the lens, so a bad spot is known before it is played.
The lens is Core's (`arenaCameraPose`, `arenaCameraLens`) at the zone's lock pitch, in three views: on the lock's line, at
the zone's framing cap, and fully aimed; a spot counts when any of them is within 0.5 m of a building's footprint. The
human-sized facades are 27% (`sol_facade_a`, 13/48) and 29% (`lat_facade_a`, 14/48, with its cap of 0), both under the
30% bar and pinned in the zone test; the tower plaza with a giant (`lat_plaza_a` with `-ArenaLineup=titan`) is 0 at its
25.8 m ring.

**Impact jolt** (`ftcore/ArenaImpact.h`). A blow that lands kicks the view a few degrees and lets it settle in 0.22 s:
the player's blows by their move's `jolt`, an enemy's blow on the lead by its move's `jolt` (the kit's `hurtJolt` when it
names none), a blocked blow by half. A kick replaces the one running only when it is stronger, so a blow on three enemies
kicks once. It is rotation only, added by a camera modifier after the camera has placed itself
(`UFTArenaJoltModifier`): the control rotation, the arm and the field of view are never touched, so it never pulls the
camera out, and it plays through the hit pause. The **Camera shake** setting (0 to 100%, in tenths) scales it; 0 turns it
off. `-ArenaCameraShake=<0..1>` sets the scale for a run (headless runs read no settings).

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
crossfade between clips, or the table's own top-level `blend_s`, or a row's own `blend_s` for the crossfade into that clip): a move's own clip through an attack, timed by Core's clock for the move at the rate
`impact_s / startup` (so the blow lands as the startup ends; held on its last frame if the clip is shorter than the move);
`stagger` while staggered, or after a parry of a move that has a `<clip>_recoil` row, that recoil, from the parry through
the Open and the stagger after it (one clip on one clock from the parry, so the two states make no crossfade; an Open that
outlasts the pair window, `pairWindow` plus one step, goes on as an open without an `open` clip does, below, and a stagger
after that starts the recoil over); `open` (optional) while open after a parry; `face_down` (looping) while down after a riposte, or open
without an `open` clip, after the optional one-shot
`face_down_in` from the start of the spell (an Open chained into a Down is one spell) and before the optional `face_down_out`, timed
to end as the state does (Core's `stateLength`: the kind's openTime or downTime, or the host's hold; never in a hold until a
release; it wins a state too short for both; a spell that ends before its get-up has run out, as a release from a hold does,
still plays the rest of `face_down_out` from where it got to, ahead of everything between moves, unless a move, a stagger or
a death comes first); `hit_light` or `hit_heavy`
(a heavy blow, a riposte or a Release) between moves, but not for a blow on its knees or during its get-up (the go-down is
its reaction, and one landing just before it is dropped); `walk` (looping, optional) between moves while the body moves, once its
smoothed ground speed passes 0.35 m/s and until it drops under 0.1 m/s, played at that speed over the row's `speed_mps` (rate 0.5 to
1.6; a walk row without `speed_mps` is left out); `defeat` on death, held on its last frame, or the optional `defeat_kneel` for a
death on its knees; else `idle`, or `idle_p2` under
60% hp. The stone_bearer's slot differs in four ways: its walk plays at rate 1.0 and, between moves, the runner moves its body
at the walk's `speed_mps` along Core's wanted direction while it plays and holds it still otherwise (an owed get-up too; Core's
speed only starts the walk, above 0.35 m/s with the wanted direction within 60 degrees of the facing, and stops it under 0.1 m/s
or outside that cone, after at least 1 s); a get-up once begun plays out in real time, and a down that runs on past it kneels
again through `face_down_in` (or stands it out when too little is left); it dies on `defeat_kneel` only while the kneel shows
(down, going down, or its get-up under 40% in), else on `defeat`; and a playing hit reaction holds 0.4 s before any blow
restarts it, and after that only a heavy one does. A clip the table lacks is left out (that state plays the idle). If the clip table, the mesh or the idle clip doesn't load, the
kind keeps its shapes and the log has one warning. The camera looks up at a boss at least 4 m tall (hover included) toward 70%
of its height (`ftcore::arenaLookUpPitch`, up to the view's 85 degree limit); the camera never pulls out for a big boss. The assets are local; nothing here needs them to load.
A row marked `"additive_in": true` (the white glove's `hit_light` and `hit_heavy`) also fades in additively: while its crossfade
runs, its own motion since its first frame goes on top of the blend by what is left of the fade, so a blow taken mid-swing jolts
the held swing pose at the reaction's full size at once instead of easing into the reaction's rest start; cut off mid-fade (a
second blow), it keeps that share and lets it go as it fades out.

The kite boss (`kite_boss`, `Data/Anim/kite_clips.json`, assets under `/Game/Bosses/KiteBoss/kite_boss/SkeletalMeshes/`,
the import being `FTImportCharacter -Source=kite_boss.glb -Dest=/Game/Bosses/KiteBoss -Face=0`): 1100 hp, 9.1 m from its
finial to its tail tip (the whole figure, `height` 9.1) and hanging 0.9 m off the ground, drifting at 0.6 to 1 m/s. The mesh is a
rebuild of the first one with the same 48-bone rig. Its 27 clips are the first one's 23 plus a recoil for each swoop
(`swoop_recoil`, `swoop_slant_recoil`, `swoop_feint_recoil`: from that swoop's impact pose onto the stagger, as long as it) and
`face_down_out`; only `swoop_recoil` plays, as the other two swoops aren't moves yet. Eight moves, each its
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
- before each tick hands ArenaFight a move mask (`setEnemyMoveMask`): only the head impacts of the moves in the phase's table,
  none while the boss is invulnerable or down, and only moves the figure has a clip for (a clipless move would stand in the
  idle behind its tell); the lead's blows refresh it inside the tick;
- on each AttackStarted `startMove`s the boss move (side from `arenaSideOf`); a move the boss still refuses is cancelled
  silently (no hold, its blows vetoed, nothing of it drawn);
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
- the carried piece breaks (`Broke`, and the runner hangs its halves on the `slab` bone, each where its part of the slab was) on the boss's first Stagger, or when its
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

The Unbending also needs its slab halves, which its Broke split hangs on the `slab` bone: `UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTImportProp -Source=<path>/slab_halves.glb -Dest=/Game/Props/StoneBearerHalves -NullRHI -unattended -nopause -nosplash`.

An import replaces what is there, so rerunning it after a boss's model changes is safe. Exit code 0 is a clean import (the
log's "face bones" line is a warning for a boss, which has no human head).

Pick the lead with `-ArenaLead=Devin` or `-ArenaLead=Duy` (the name of a `PartyLooks` entry in the local
`Config/UserGame.ini`, any case); each plays its own fighter kit (Devin gauntlets; Duy kicks, and fires his pistol only
while aiming). The third party slot is a support slot with no attack kit (`"weapon": "none", "attacks": false` in
`arena.json`): it is never the lead, so `-ArenaLead=Third` is refused with a warning and the first lead with attacks
plays. The magic kit (`"weapon": "magic"`: a cast clip, `spell_bolt`, `spell_impact`, `spell_ward`) stays in the code
and its Core tests for a future fighter kind; no shipped fighter uses it. Without the flag the lead is the first such look (Devin in the main checkout).

```
UnrealEditor.exe ForgottenThird.uproject /Game/Maps/Demo_Arena -game -windowed -ResX=1600 -ResY=900 -ArenaStage=stone_ring -ArenaLead=Devin
UnrealEditor.exe ForgottenThird.uproject /Game/Maps/Demo_Arena -game -windowed -ResX=1600 -ResY=900 -ArenaStage=kite_ring -ArenaLead=Duy
```

`-ArenaStage=stone_ring` hosts `stone_bearer` and `-ArenaStage=kite_ring` hosts `kite_boss`; `-ArenaLineup=<kind>` fights a
kind in whatever zone `-ArenaStage` names (the sand ring by default). `-ArenaAutoPlay` has the autopilot play the lead.
For a picture with no window: `-RenderOffScreen -ArenaShot=<path prefix> -ArenaShotDelay=8` in place of `-windowed`
(needs a renderer, not `-NullRHI`).

Carrying (slice 1, off unless asked, never in shipping builds): `-ArenaChairs=chair_a01,chair_a02,chair_a03` names those
chairs of `Data/Battle/chairs.json` (four seats; `none` names none, so the Load cap is 40, three named give 100),
`-ArenaCap=N` sets the cap outright, `-ArenaAlly` adds the ally alone, `-ArenaTakeHalfAuto` has the autopilot take half of
blows aimed at the ally and `-ArenaAutoBlock` has it guard through every wind-up (blocks store a quarter of the blocked
damage as Load). Any of the first four turns carrying on; the result is one `Arena: CARRY ...` line at the end.

Briefs (Lattice slice 1, off unless asked, never in shipping builds): `-ArenaBrief=bailiff` or `-ArenaBrief=notary` with
`-ArenaLead=Duy` lays that Brief's kit from `Data/Battle/briefs.json` over the guns lead's own kit: a six-round cylinder
with a 1.2 s reload (the shot spends a round instead of stamina; with none left a shot press is dropped and kicks still
go), plus the Brief's kit changes. The cylinder sends three fight events for the host's clip and sound: `ReloadStarted`
(the reload's length in ms), `Reloaded` (the rounds) and `DryPress` (a press dropped on the empty cylinder); the arena
logs each one and plays its sfx cue (`SFX-GUN-RELOAD`, `SFX-GUN-LOADED`, `SFX-GUN-DRY` in `Data/Audio/cues.json`;
silent until the sounds are imported; an instant reload plays only `SFX-GUN-LOADED`). The reload clip `lead_reload` is
posed each frame by the fight's reload progress, so it spans the reload whatever its length, and `lead_dry_flinch` plays
from a dry press; both sit in the look's clip folder, play on the upper body under a strike's share, and are skipped
(logged at Display) until they exist (`ftcore::arenaReloadLayer`, `arenaFlinchLayer`, `arenaGunCue`). Any other lead is
left alone with a warning. `-ArenaAutoShoot` has the autopilot use
the pistol; `-ArenaLineup=dummy,maw_pup` is the test line-up. The result is one `Arena: BRIEF ...` line at the end.

### The leads' weapons

Each lead fights with its kit's weapon (`Source/ForgottenThird/FTLeadWeapons.h`): its light attacks play strike clips
retargeted onto its body, Devin wears gauntlets, Duy holds a pistol (his light is a kick; with aim held, light fires one
round), and each weapon has its effects. Nothing below is
committed; a lead without it fights as before (one warning per missing piece). In order, from the checkout the game runs
from:

1. The props: the pistol with `-run=FTImportProp -Source=<path>/Duy_Pistol.glb -Dest=/Game/Props/Weapons`, as for the
   slab halves above (then LOD1 and LOD2 made on the mesh in the editor's import settings). The gauntlets are six
   skeletal meshes skinned to Devin's body (glove, wrist strap and bracer per hand, `FTWeaponProps::GauntletPieces`),
   imported as FBX onto that body's skeleton into `/Game/Props/Weapons/Devin_Gauntlet` under those names; they ride the
   body by leader pose, so they need no sockets.
2. The pistol's sockets: `-run=FTWeaponSockets -Pistol=<pistol socket .json>` puts the grip and holster sockets on
   Duy's body and the `muzzle` socket on the pistol (`FTWeaponSocketsCommandlet.h` has the axes).
3. The effects: `-run=FTBuildVfx -Only=<name>` for `muzzle_flash`, `gauntlet_spark`, `spell_bolt`, `spell_ward`,
   `gun_tracer`, `gun_impact` and `spell_impact` ([VFX_UE.md](VFX_UE.md)).
4. The strike clips: the test human's `lead_jab`, `lead_cross`, `lead_kick_r`, `lead_counter` and `lead_finisher`
   ([BEAT_ANIMS.md](BEAT_ANIMS.md)) retargeted onto each lead's body into its look's `ClipFolder`:
   `-run=FTBuildRig -Mesh=<the look's body> -Dest=/Game/Characters/<Lead>/RigReal3/LeadRig
   -RetargetFrom=/Game/Characters/TestHuman/Rig/IK_test_human.IK_test_human -Clips=<the five> -Names=<their names>
   -Out=<the look's ClipFolder> -AlignPose`. Duy's aimed round plays `lead_fire` (one-hand aimed pistol, recoil) and
   the unused magic kit's cast `lead_cast` (palm push); both are hand-keyed outside the repo and listed in `beat_clips.json` without an
   asset, so they are first retargeted onto the test human locally, then onto the leads with the other five.

To try a weapon without being hit back, fight the target dummy: `-ArenaLineup=dummy` is a passive enemy kind (it never
takes an attack token, stands where it spawns and has 100000 hp).

On each of the lead's attacks the clip of its move plays over the locomotion, keyed so its impact lands as the attack
goes active (a kick, shot or cast with no clip of its own is thrown as the punch on its side), and fades out after.
The lead aims at the locked enemy, else the nearest one standing: the hit point is the near side of its body at its
chest. The striking arm is aimed there (shots and casts too; kicks are not), the tracer or bolt flies there and the
impact effect plays there; the run logs `Arena: aim <clip> <deg> deg` and `Arena: impact <effect> <cm> cm from the hit
point`. `-ArenaShotStrikes=N` shoots the first N of them (`_s<k>_<clip>`; `-ArenaShotClose` from close by), `-ArenaNoWeaponProps`
and `-ArenaNoWeaponFx` leave the props and the effects off for an OFF/ON pair.

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

### Feature checks

After the result line the runner prints one `Arena check:` line per combat feature (Core's `arenaFeatureChecks`
over the fight's stats), each PASS, FAIL, barred, or n/a where the run gave it no chance to show:

- `lights free`: lights were started and cost no stamina (n/a with no light started: an idle run, an early death, a
  kit without attacks);
- `cancels`: an action (a buffered attack, a roll, the guard) cut into an attack, `(<cancels> of <chances>
  chances)`; a chance is an attack in whose cut window a press or the guard waited. n/a with no chance;
- `flinch`: an enemy flinched: `(<flinches> of <chances> chances)`. A chance is a blow of the kind's `flinchPoise`
  or more that left a kind with a `flinchTime` upright and the host's veto off; n/a with none (bosses have no
  `flinchTime`), FAIL with chances and no flinch (also when the flinch run's cap or an active or armoured attack
  stopped every one: a chance is counted before those rules);
- `break-out`: an enemy broke out of a run of blows: `(<n>; due <d>, barred <b>; <u> upright blows; best run 4 of
  5)`, the best run being four blows of a kind's five, counted as each lands. FAIL when a blow brought an upright
  enemy's count to its `breakAt` without staggering it and the move was free yet none followed, and when upright
  blows landed on a kind with a `breakAt` but the run never moved; barred when the move was due and the enemy's move
  mask (or the host's veto on every upright blow) barred it; n/a with no upright blow on such a kind, or when the
  poise always broke first;
- `soft target`: an attack started on a soft target (the nearest living enemy in `softRange` and `softArcCos` of
  the stick, or of the facing, with nothing locked) and homed up to `homingReach` (`<n> of <m> chances`: n/a when no
  attack started unlocked and unaimed, as under the autopilot, which locks unless `-ArenaNoLock`);
- `shots`: a guns kit fired a shot that hit (n/a for any other kit, and for a guns kit under the autopilot without
  `-ArenaAutoShoot`, which never aims);
- `shared count bonus`: two leads on one count, a held blow's bonus landed: `(<held> held, <joined> joined, <n>
  bonuses, +<damage> damage)`. n/a without `-ArenaSharedCount`.

Three more lines carry numbers only, no verdict:

- `jolt (<n> blows asked; peak <deg> deg handed to the camera; camera shake <scale>)`: 0.00 peak at camera shake 0;
- `off-frame tells (<n> windups off frame; <m> stingers)`, with "no audio device: the marker alone" when a stinger fired with no sound to play it (`-nosound`);
- `pace: kills <n>, ttk 0 <s> s, 1 n/a; punish <used>/<windows> windows used`: per enemy slot, the mean seconds from
  the lead's first blow on it to its death (n/a for a slot never killed), and the punish windows (a run of an enemy
  staggered, open or down, counted once) the lead's side landed a blow in, summed over fights again. They wait for a
  play report.

`-ArenaNoLock` has the autopilot never press lock, so its attacks take soft targets. `-ArenaAutoShoot` has it aim and
shoot with a guns kit on the way in (the target in the shot's range and outside the light chain's reach, a dodge's
stamina kept). A flinch and a break-out need the line-up's kinds to allow them with the lead's kit. On a skirmisher
(poise 22, `flinchPoise` 12, `breakAt` 2) Duy's lights (10, 10) are under its `flinchPoise` and the second breaks it
out; his third (20) then meets 2 poise and staggers it, a shot (6) is too light and his heavy (45) staggers it. The
hulk has no `flinchTime`. So `skirmisher,hulk` on Duy gives no flinch chance and reads n/a; Duy flinches a duelist
(`flinchPoise` 10).

The break-out is tuned by lead (provisional, for the playtest). On paper Devin's light string (poise 15, 15, 18, 32)
staggers every kind with a `breakAt` by the blow that reaches it; in a fight his strikes slow him but don't stop him
(the autopilot steers them as a player does), so a count can build across strings and some seeds still break out on
him.
Duy's (10, 10, 20, and 6 a shot) leaves it upright, so he has to hit and move. Each kind's `breakDecay` equals
its `poiseDelay`, so a count reset always comes with a poise refill. Core pins this per kind and lead
(`arena break-out tuning`):

| kind | poise | `breakAt` | Devin's poise at `breakAt` | Duy's |
|---|---|---|---|---|
| skirmisher | 22 | 2 | 30, staggers | 20, breaks out |
| breaker | 80 | 5 | 95, staggers | 60, breaks out |
| duelist | 65 | 4 | 80, staggers | 50, breaks out |
| hulk | 110 | 7 | 128, staggers | 90, breaks out |
| spitter | 25 | 2 | 30, staggers | 20, breaks out |

Core's host-mirror tests play the host's own fight (the fallback fight with one attacker, fitted to `sand_ring`, the
lock never pressed, the partner's press on the step after a parry); seed 1 of a skirmisher and a hulk with Devin
gives the host's result line to the letter. The autopilot fills each attack's steer and turn from the attack-move
table, as the player's input does. With Devin no seed from 1 to 12 of a skirmisher and a hulk shows a break-out (n/a;
the best run reaches `breakAt` only with the blow that staggers); against a duelist and a skirmisher 8 of the 12
break out once (seed 1 among them), the rest read n/a. Duy shooting against a skirmisher and a hulk shows one on every
seed, and against a duelist and a skirmisher two or three (shooting or not); unaimed against the hulk the
autopilot's parries cut his runs short (n/a). Five runs cover every line:
Devin against the stone bearer, each lead against a skirmisher and a hulk, then each against a duelist and a
skirmisher:

```
UnrealEditor-Cmd.exe ForgottenThird.uproject Demo_Arena -game -NullRHI -ArenaStage=stone_ring -ArenaLead=Devin -ArenaAutoPlay -ArenaNoLock -ArenaQuitOnFinish -unattended -nosplash -nosound -stdout
UnrealEditor-Cmd.exe ForgottenThird.uproject Demo_Arena -game -NullRHI -ArenaLineup=skirmisher,hulk -ArenaLead=Devin -ArenaAutoPlay -ArenaNoLock -ArenaQuitOnFinish -unattended -nosplash -nosound -stdout
UnrealEditor-Cmd.exe ForgottenThird.uproject Demo_Arena -game -NullRHI -ArenaLineup=skirmisher,hulk -ArenaLead=Duy -ArenaAutoPlay -ArenaNoLock -ArenaAutoShoot -ArenaQuitOnFinish -unattended -nosplash -nosound -stdout
UnrealEditor-Cmd.exe ForgottenThird.uproject Demo_Arena -game -NullRHI -ArenaLineup=duelist,skirmisher -ArenaLead=Devin -ArenaAutoPlay -ArenaNoLock -ArenaQuitOnFinish -unattended -nosplash -nosound -stdout
UnrealEditor-Cmd.exe ForgottenThird.uproject Demo_Arena -game -NullRHI -ArenaLineup=duelist,skirmisher -ArenaLead=Duy -ArenaAutoPlay -ArenaNoLock -ArenaAutoShoot -ArenaQuitOnFinish -unattended -nosplash -nosound -stdout
```

The fight session judges every blow the player lands on an enemy in the fight's blow hook, after whatever ruling
the sub-hosts set (kind crits, the kite, the stone bearer): `judgeHeldBlow` on the damage they leave. A final outcome
scales the blow (x1 when alone); a blow inside a count's window is Held, lands at base, and when its window is joined
it settles at x2.5: after the session's tick the runner deals the extra through `ArenaFight::hostBlow` and logs
`Arena: a held blow on the count adds <n> damage to <kind> (move <m>).` The arena has no partner body yet, so
`-ArenaSharedCount` scripts one for the check line: the lead calls the count whenever it can, and the other lead
joins the window the step a lead's blow is held. Core's host-mirror test plays the same run (seed 1: Devin 3 bonuses,
+78 damage; Duy shooting 1 bonus, +17):

```
UnrealEditor-Cmd.exe ForgottenThird.uproject Demo_Arena -game -NullRHI -ArenaLineup=duelist,skirmisher -ArenaLead=Devin -ArenaAutoPlay -ArenaNoLock -ArenaSharedCount -ArenaQuitOnFinish -unattended -nosplash -nosound -stdout
```

The hit pause shows on screen: the lead's body and strike clips (`GetDrawnTime` is on Core's held clock) and a hit
enemy's clips stand still for its pause, and the camera holds where it is (it never pulls out). A flinching enemy leans
back a little (a placeholder pose until flinch clips exist) and plays its light hit reaction when it has a skin. The
runner hands the clocks to the fight session through `GetHeldTime`, `GetHeldDelta` (0 through the player's pause),
`GetHitStopLeft`, `GetPlayerStopLeft` and `GetEnemyStopLeft`. After every session Begin (the fight's begin and each restart) the runner binds the session to the game
instance's story state (`UFTStoryStateSubsystem::BindFightSession`). `ArenaFight::hostBlow` takes extra damage from
outside a blow (a held blow's settle): the hp at once, a `HostHit` (then `Downed` on a kill) with the next tick.

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
   Space or pad A starts it again (the autopilot starts again by itself after 4 s unless told to quit; after a victory
   over a boss whose `defeat` clip is longer than 3.5 s, 0.5 s after that clip ends instead, so the death plays out. The
   log gets one line a fight, `Arena: autoplay fights again <s> s after the result (longest defeat clip <s> s).`).

## Controls

| | Keyboard and mouse | Gamepad |
|---|---|---|
| Move (camera-relative) | W A S D | Left stick |
| Look (when not locked) | Mouse | Right stick |
| Light attack (a chain of hits; on an open enemy, a riposte) | Left mouse (tap) | RT (tap) |
| Heavy attack | Left mouse held 0.25 s | RT held 0.25 s |
| Aim (hold or toggle): camera over the shoulder, reticle, face the aim, strafe; a gun kit fires one round per press | Right mouse | LT |
| Guard (hold or toggle); parry (tap just before a hit) | Left Ctrl | LB |
| Roll (backstep with no direction) | Left Alt | B |
| Heal (uses a charge; on Gift's key for now) | G | D-pad up |
| Release (above a third of the Load) | R | D-pad down |
| Partner (the second lead's press, paired with a parry; on Count's key for now) | F | Y |
| Intercept a called blow (stone_bearer; hold or toggle) | V | L3 |
| Read (the second lead's pick, while the cards are up; those keys are the picks only then) | 1 / 2 / 3 | D-pad left / up / right |
| Lock on / off | Middle mouse | R3 |
| Switch lock target (locked) | flick the mouse sideways | flick the right stick |
| Fight again (after the outcome) | Enter or Space | A |
| Pause menu | Esc | Start |

A strike commits the body (`ftcore::attackMove`, `Data/Battle/attack_move.json`): each part of a move has its own
share of the stick's walk (`startupScale`, `activeScale`, `recoveryScale`), and the full walk comes back at the cancel
point. A fist's or a spell's heavy commits: 30% in startup, 0 through active (the lead slides to a stop at its brake
rate, `ArenaInput::attackSpeed`, and the lunge carries it), 40% in recovery. Its light slows the walk: 40% in startup,
55% through active, 70% in recovery. A gun's shot walks as it did before: a light at 65%, a heavy at 45% then 30% from
0.25 s before contact (`commit`), each easing back to full pace over its recovery (`recoveryRamp`). Every strike still
turns at its capped rate (`attackTurn`).

The lead's legs follow its body as drawn. Their gait plays at the speed the body moved over the last step, not the
sim's wish. They turn toward the way it moves (up to 95 degrees from the facing, with the chest turned back), so a
strafe walks forward gaits sideways. While a strike's clip has the legs, and through a hit pause, the feet stay where
they stand. A foot the body has gone 26 cm past steps to the clip's place, one foot at a time on a short lifted arc.
The clips stand on nearly straight legs, so the hips sink (eased, up to 9 cm) as far as the held feet need to stay
in reach. The legs step at the median of the last three steps' body speed, not the last step's alone: a body pushed
in and out against an enemy swings step to step, and the walk/run blend and the foot hold would flip with it.
Through a hit pause that speed, the legs' turn and the upper-body share hold too, as the clips do. Under an
upper-body strike the legs are rebuilt from the locomotion layers' own weights (`LocoWeight`), so a strike clip at
full weight never takes the legs. The figures' meshes tick after the actor that runs the fight (the game mode or the
host), so each frame's pose is drawn where the body stands that frame, not a frame behind it.

`-ArenaGlide` logs foot slip per move part on quit (`Arena glide: <part>: ...`). It gives the planted foot's
frame-to-frame speed and the net slip per plant (as the field's `-LocoSlide`). `-ArenaGlide=toward` and `=across` make
the autopilot walk at the target or strafe it; `=backstep` lets go of the stick for each roll (a backstep under the
lock) and `=sidestep` holds it across the lock for each, to the target's right then its left (the side dashes). `-ArenaNoFootLock` leaves the feet to the clips, for comparison.
`=jump`, `=jumpmove` and `=jumpstrike` have the autopilot only jump (no attack, roll, guard or heal), 1.2 s after
each jump's end: standing, with the stick held across the target, or standing with the air light pressed 0.22 s
into the air clock. Every frame on the jump's clock (take-off, air, landing, an air light) goes to its own `jump`
row, and these runs add the check line `Arena check: jump <PASS|FAIL|n/a> (<n> started, <n> landed).`
Each line ends with its pops: frames in which a foot moved more than 20 cm, a pose snap (still counted in the slip).
Moving is split four ways: steady, speeding up and braking (the legs' speed changing faster than 2 m/s²) and turning
(the facing turning faster than 1.5 rad/s, whatever the speed does).

Every key here comes from the binding table (`ftcore/InputBindings.h`, one set for the field and the arena; the
defaults are provisional). Override any of them in `Saved/InputBindings.json`, read once at start, e.g.
`{"version": 1, "keys": {"guard": "Z"}, "pad": {"read": "Gamepad_RightShoulder"}, "hold": {"guard": "toggle"}}`
(engine key names; `""` unbinds; a key twice in one context, or a Read-held key on Read's or Guard's, refuses the
whole file). Each held action (Aim, Guard, Read, Run, Crouch, Intercept) is held, or pressed once to toggle when
the game's hold-toggle setting is on or its own `"hold"` mode says `"toggle"`.
Presses are read each frame and handed to the fight on its next step, so no press is lost between steps.

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
  burst flashes.
- **Off-frame tells:** an enemy winding up off the screen, or out of the camera's view less a 6 degree margin
  (`arenaOffFrameTells`, from the camera's place, facing, pitch and field of view: a ground enemy under the frame while aiming high counts), shows as an arrow at the screen's edge
  in the tell's colour, with the answer beside it and a bar filling toward the hit. It swells and rings once at the
  windup's onset (a steady arrow with Reduce flashes on). A stinger (a chime, at master times effects volume) doubles
  the onset once per windup, no closer than 0.25 s to the last; the arrow is the tell, so it reads with the sound off.
  At impact the arrow turns white.
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
| `-ArenaNoLock` | The autopilot never presses lock, so its attacks take soft targets (see Feature checks). |
| `-ArenaAutoShoot` | With a guns kit the autopilot aims and shoots on the way in (`ArenaFight::setAutopilotShoots`); off, it never aims. |
| `-ArenaAimCheck` | With the autopilot and a guns kit: 3 s in, the camera aims at the tallest standing enemy's top and one aimed shot is fired (the `aim up` check line: the view pitch, the lens's height and distance from the lead, what the crosshair meets, whether the shot hit; PASS needs the rendered pitch within 1.5 degrees of the solved one, the crosshair on that enemy and its hp down). Run it on `kite_ring` with `-ArenaLead=Duy`. Not in shipping builds. |
| `-ArenaAssistCheck` | With `-ArenaAimCheck`: after its shot, four steps with aim assist on, each capped at 6 s: a half-way right stick sweeps the view off the body and back (a lock from before is let go), a lock is pressed on and let go, aim is let go and held again, and the target is killed by a host blow. Each change of the stick's share is logged with its cause (`aim assist look scale 0.60 -> 1.00 (locked on; ...)`). The `aim assist` check line compares the sweep's turn rate over a body with the rate off it and says whether the share came back to 1 as the crosshair left, on lock-on, as aim let go and as the target died (PASS: ratio 0.60 within 0.05 and all four). Not in shipping builds. |
| `-ArenaCoverCheck` | Once, a second into the fight: a synthetic crosshair line (not the camera's) from 0.5 m over the floor through the cover drum nearest a standing enemy to its body's middle, picked as the reticle picks (the world's first hit clips the line). The `reticle behind cover` check line gives the world hit, the body's distance on the same line with the world left out (the control) and the pick: PASS when the drum is hit first and no body is picked, so aim assist has no body to slow over and the round's aim body is none. Needs a zone with cover drums (`stone_ring`); with `-ArenaQuitOnFinish` the run ends after it. Not in shipping builds. |
| `-ArenaFrameCheck` | Samples the lock's framing as the rendered camera shows it on every followed frame: the lead's and the target's place across the screen, the turn and its rate, the boom's arm and the lens's distance from the pivot. The `framing` check line passes when every settled frame (the lock's line within 5 degrees, the turn within 1 of its want) has both bodies in the middle third (not in a zone whose framing cap is 0, where only the turn staying 0 and the line
settling on the target are checked), the turn never passes its cap and the arm never passes the rig's; the bands under it give the target's mean place and the least edge gap by distance. Not in shipping builds. |
| `-ArenaNoFrame` | Holds the lock's framing at 0, for a side-by-side with `-ArenaFrameCheck`, whatever the **Lock framing** setting says (players turn it off there). Not in shipping builds. |
| `-ArenaCameraShake=<0..1>` | The impact jolt's scale for this run, over the Camera shake setting (0: no jolt). |
| `-ArenaSharedCount` | With the autopilot: the lead calls the count whenever it can and a scripted partner joins a held blow's window, so a shared count's bonus lands (the `shared count bonus` check line). |
| `-ArenaIdle` | Nobody plays the lead (no input at all); wins over `-ArenaAutoPlay`. |
| `-ArenaQuitOnFinish` | Quit when the fight ends, after the result line (exit code 0). |
| `-ArenaSeed=N` | The fight's seed (default 1). |
| `-ArenaStage=<id>` | The zone, by id from `arenas.json` (see "Zones"); the default is `sand_ring`. A zone in a world opens that world's map. Its bosses are the line-up, and the walls, spawn, obstacles and camera are fitted to the enemies in it. An unknown id logs the ids and uses the default. |
| `-ArenaLineup=a,b,...` | The enemies by id from `arena.json` (`brute`, `skirmisher`, `breaker`, `duelist`, `hulk`, `titan`, `spitter`), up to six, instead of the stage's own bosses (the sand ring's are `brute,skirmisher,breaker`); `-ArenaLineup=brute` fights one brute. The stage is scaled to whoever is in the fight. |
| `-ArenaLead=<name>` | The lead the player plays. Its look is the entry of that name in the explore character's `PartyLooks` (`Config/UserGame.ini`, local), else the first; with none, or none whose body loads (one warning), it is the test human. Its fighter kit is the `arena.json` fighter of that name in lower case (`devin`, `duy`), even when no look loads; without the flag it is the first playable look's name, and an id with no kit plays `default`. A kit without attacks (`third`) is refused with a warning. The start line names the kit (`kit devin`). |
| `-ArenaAttackers=N` | How many enemies may attack at once (1 to the line-up's size). |
| `-ArenaHoldLoad` | The autopilot keeps its Load until it is overloaded (to see the overload; not a way to play). |
| `-ArenaParryHold=<s>` | No follow-up for s seconds after each parry: the autopilot's blows and Release and the partner's press are dropped, so the open boss's own clips (face down, the get-up) show. A check of the clips, not a way to play; the fight differs from plain autoplay. |
| `-ArenaBossLoop=<clip>` | The boss's skin plays that clip of its table (by id, e.g. `idle`, `idle_p2`, `face_down`) looped from the fight's start, with the table's blend, whatever the fight does; the fight itself runs unchanged. A check of the clips' loops, not a way to play. One log line at each fight's start (`Arena: boss loop <clip> on enemy <n> from the fight's start (<len> s) (-ArenaBossLoop).`) and one at each wrap (`Arena: boss loop <clip> wraps (<n>) at <fight time> s (<len> s).`); the log's `[..][counter]` prefix (`GFrameCounter % 1000`) on a wrap line is the frame whose pose is the clip's first again, which is where a cutter finds the seam. An unknown clip (or no skinned boss) logs a warning and the flag does nothing. The boss is the Kite-Mother's slot, else the stone bearer's, else the first skinned enemy. |
| `-ArenaNoKindFx` | No kind's crit effects (`arena_crit.json`: the tell, open, crit and hit) are made; the fight runs the same. The OFF half of an effects OFF/ON capture. |
| `-ArenaBossCam` | Once the first enemy stands, the view moves to a camera fixed 9 m off that enemy's left side at half its height (field of view 40), so a capture shows the boss's own turns apart from the follow camera's. The log gets `ArenaBossCam: t <s> s yaw <deg>` each time the boss has turned 10 degrees. Review captures only; the fight runs the same. |
| `-ArenaStoryProbe` | Headless check of the story wiring (`-NullRHI` is fine): the fight is handed off as the story's with no step marked (as a reload into it), one gift press is counted, the arena restarts the fight, one more is counted; each stage logs `Arena: story probe <stage>: bound, presses, retries` (expected presses 0, 1, 0, 1 and one retry), then the game quits. |
| `-ArenaShot=<path prefix>` | Autoplay and save the views (`_1_ring`, `_2_tell` with the Read cards picked, `_3_impact`, `_4_load` with the gauge past a third, or the overload under `-ArenaHoldLoad`, `_5_release`, `_6_result` .png), then quit; a view the fight never reaches is skipped. Needs a renderer (`-RenderOffScreen`, not `-NullRHI`). |
| `-ArenaShotDelay=S` | Seconds before the first screenshot (the fight waits for it). |
| `-ArenaOverview` | With `-ArenaShot`, a first view `_0_overview` of the whole zone from a camera high over its south-west side (pulled in along the line if a building is in the way), HUD off, with the zone's walls (green), obstacles (red) and road mouths (yellow) drawn as debug lines for the picture only; then `_0b_edge`, the player 1.4 m from the wall on his left to show the edge cue. |
| `-ArenaShotAt=S,S,...` | With `-ArenaShot`, an extra view `_t_<S>` at each of those fight times (after `_1_ring`; the six views and the quit are unchanged), so two runs of the same fight are shot at the same moments. |
| `-ArenaShotSide` | With `-ArenaShotAt`, those views from the lead's right side: 3.4 m off it and 0.9 m above its hips, aimed a frame before each view, so the feet show. |
| `-ArenaShotStrikes=N` | With `-ArenaShot`, an extra view `_s<k>_<clip>` at each of the lead's first N attacks (after `_1_ring`; `<clip>` is the strike clip it plays, `noclip` when none): odd views just after the impact (a strike's landing, a shot's or cast's arrival at the hit point), even views a shot's or cast's mid-flight (`FFTLeadWeapons::GetViewTime`). The frame is that moment exactly: the fight's clock is brought to it and held, the lead's effects are run to their age then and paused (Niagara keeps the world's clock, which runs on in the frames before the view), and the lead's own clock stops until the view is taken. Attacks that go by while another view holds the fight are skipped. |
| `-ArenaShotClose` | With `-ArenaShotStrikes`, those views close and HUD off: a camera 65 cm from the striking hand (the foot for a kick), square to the lead's side on that hand's side, the hand at the frame's centre, after a 0.6 s settle, then `_front` from 55 cm ahead of it (off to its side, a little above). A lead with a pistol adds `_1_holstered` and `_1_drawn` after the ring view, 60 cm off its right side. With gauntlets the first strike adds `_s1_<clip>_arms`, both forearms from 1.4 m ahead of the lead on the striking side. Each view logs where the hand (and the pistol's muzzle) fall in the frame. |
| `-ArenaJump` | The jump's key (Space, the pad's A) is read in a fight and the controls line names it after the dodge; without it the key does nothing in a fight and default fights play as before the jump. Until the user has tried the jump; not in shipping builds. The jump costs 0.6 of a dodge's stamina, takes off for 0.08 s, is 0.6 s in the air (0.55 m at the top) and lands for 0.3 s, the first 0.15 s of it locked; low sweeps and ground-shock slams pass under it from 0.2 s to 0.5 s of its clock; any blow in the air knocks it out; one air light per jump (the kit's `jumpLight`). The run's log adds `Arena: jump takes off at <s> s` and `Arena: jump ends at <s> s, <s> s after take-off, into <action>` and the check line `Arena check: jump ...`. The lead plays its field jump, fall and land clips (moving ones while it moves). |
| `-ArenaJumpFloor` | Each frame on the jump's clock and 0.4 s after it, the lead's pelvis, head and balls over the floor and the arc's lift (`Arena jump floor: clock <s>, air|ground, ...`). Not in shipping builds. |
| `-ArenaNoDash` | The squash in place of the locked dash clips, and no side dashes (the before half of a side-by-side); not in shipping builds. |
| `-ArenaDashTall` | The dash clips as they are, to compare by feel with the default tightened dashes (the body lowered by 0.4 of the lower foot's height over its standing height, none at take-off or landing, and the trunk pitched 15 degrees back toward upright under the clip, held through its fade); not in shipping builds. |
| `-ArenaRollFloor` | Each frame of a roll on its clip logs the pelvis, head and balls over the floor and the floor lift; the lift's fade after it is logged too. Not in shipping builds. |
| `-ArenaNoWeaponProps` | The lead's weapon props (gauntlets, pistol) stay off; the fight runs the same. |
| `-ArenaNoWeaponFx` | The lead's weapon effects (muzzle flash, gauntlet sparks, the Third's bolt, burst and ward) stay off; the fight runs the same (the OFF half of an OFF/ON capture). |
| `-ArenaNoBossFx` | The stone bearer's effects (tells, impacts, chips, the weak point's glow) stay off and the fight runs the same (the OFF half of an OFF/ON capture). |
| `-ArenaNoKiteFx` | The Kite-Mother's effects stay off and the fight runs the same (the OFF half of an OFF/ON capture). |
| `-ArenaWorldView` | With `-ArenaShot` (and without `-ArenaOverview`), a first view `_0_world`: a low wide look across the zone at the scene round it, HUD off, nothing drawn; for judging the world's look (docs/PHOTOREAL_LOOK.md). |

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
  from the look's retargeted locomotion clips, `UFTLocomotionAnimInstance`), else the test human. Its attacks play the
  strike clips retargeted onto its body over that locomotion, and it carries its weapon (see "The leads' weapons"); without
  the clips, and for its guard and roll, the figure keeps its own overlay (a blade on a pivot arm, a
  crouch and lean; enemy figures also show a guard plate, the player's never). A roll plays the land-roll clip; a
  locked dodge back from the target (within 45 degrees of straight back) plays a hop back that keeps facing it, and
  a locked dodge across it (more than 45 degrees off both straight at it and straight back) is a side dash: drawn
  side-on, facing across its way on the target's side, leaping to the body's right or left. The hop is
  `MM_Dash_Backward_to_<body mesh>`, next to the run loop, and the side dashes `MM_Dash_Left` and `MM_Dash_Right`;
  side dashes are on (`ArenaFight::setSideDashes`) only when the body has both side clips, else those dodges stay
  rolls. Each is played from its take-off to its landing (back 0.20-0.73 s, left 0.23-0.70 s, right 0.27-0.77 s),
  fitted to the roll's time, with the root held, so the rules alone move the body; the arc is flattened and the trunk
  stood up a little (`-ArenaDashTall` plays them as they are). Without the clip the figure squashes, as before. **PROVISIONAL:** these
  dash clips are the engine's MoverExamples dash animations retargeted (`FTBuildRig`, local assets, never committed).
  They are the motion target for our own hand-keyed or mocap clips, which must replace them before any shipping
  build. Enemies are primitive figures in their kind's colour, or a skinned mesh (above). Swings, guards, rolls, staggers and falls are posed from the fight's output each frame.
- The camera is a spring arm over the lead's shoulder, its length, height, shoulder offset and lock pitch from
  the zone's camera rig (6.4 m, 1.5 m, 1.2 m and -20 degrees, the same for every fight: a giant is found by locking
  on or looking up, from -50 to +85 degrees, free, aimed and locked alike: `ftcore::kArenaLook` in `ArenaLook.h`). The
  boom itself swings up only to 15 degrees, which keeps the lens over the floor and behind the lead at the fight's arm
  and the aim's; a steeper look tilts the lens on the boom's end, so the arm never shortens. The aim's reticle line
  meets the enemies' bodies (`ftcore::arenaRayEntersBody`: an upright cylinder from the figure's hover to its scaled
  height, its radius scaled too; a lens inside a body sees through it), and reads the view without the impact jolt, so
  a kick never wobbles the heading. On a body, the heading takes the middle of the stretch of the line inside it,
  so the round goes into the body rather than grazing its edge or passing under a hanging one (with the line's far end
  inside the body, its centre line at the entry's height). A gun's round is a line in space: it leaves the lead
  1.4 m up (`ftcore::kArenaShotHeight`) along that heading, climbing to the point (`ArenaInput::aimPitched`; locked on,
  to the locked body's middle), and meets the first body it enters within the shot's range (Duy: 30 m, most of
  kite_ring's 40 by 14 m), each body the same cylinder widened by half the shot's width. So a round passes over a short
  enemy's head to a hovering boss behind it. A body the lead stands in counts (point-blank) only when its centre is
  ahead or right on the lead, and the round leaves it through its side: a steep round that climbs out through its top,
  or dives out through its base, passes it. A shot keeps its start's choice (pitched or the flat lane), so letting the
  aim go in the startup does not change it, and its climb follows the crosshair only until it fires. A
  crosshair on a body the round would not reach is drawn faint, without its dot; one where another body on the
  muzzle's line would take the round is dimmed, with a small cross for the dot (`ArenaFight::shotMeets`; neither while
  locked on, when the round goes at the lock). The round and the readout share one rule: the crosshair's body
  (`ArenaInput::aimBody`) takes the round when it is met within 0.25 m (`kArenaShotGraze`) along the round of the body met
  first, so blocked means the round goes elsewhere, and the dimming changes only once the new state has held 4 frames,
  so an edge does not flicker. Aim assist (Settings, "Aim assist", on by default; `ftcore::aimAssistTurn`): while the
  unlocked crosshair is over a body the round would take, the look turns at 0.8 of its rate with the mouse and 0.6 with
  the stick; off, or under `-ArenaAutoPlay`, it is unchanged. The autopilot's shots, unpitched, stay a lane on the ground. The lead's aim pose is the arm's IK alone (no spine or head pitch): above
  about 45 degrees the arm lifts over the shoulder while the chest stays level. The arm probes the world (it may leave the zone's walls but never passes through a building or the
  ground) and is pulled in. Locked on, it turns toward the target and holds it; otherwise the mouse or right
  stick turns it (the autopilot and an idle run face the nearest enemy).
- Nothing in the host allocates per frame: the HUD's words are static, and its numbers are rebuilt only
  when they change.
