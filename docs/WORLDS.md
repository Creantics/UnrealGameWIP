# The three big worlds

The story's field steps play in three big worlds:

- **Solhaven**: a town. Its Ink-Solhaven and Restored scenes play here.
- **Lattice**: towers.
- **Unwritten**: ruins.

Scenes that aren't placed in a world, such as the house and the pocket, still play on the small Explore map.

The **overworld** joins them: a small walkable map with one gate per world. When the story moves from one world to another, the party walks the road between the two gates before the next field step starts.

Each world is a blockout for now: shapes, roads and landmarks on the world's own hills under the shared photoreal look ([SCENE_LOOK.md](SCENE_LOOK.md)). The ground comes from Core (see [Ground](#ground)) and is imported as a Landscape. The ground wears photographed ground, rock on its steep faces and its high ground, and the roads, but the trees and rocks are still simple shapes, nothing grows on the ground yet, and there is no streaming and there are no biomes.

## Size

Each world is 1 km a side. Districts sit at least 150 m apart, joined by straight roads. A district is a stone plaza 72 m across, ringed by buildings, with a tall landmark topped by a gold beacon. The climax's ring stays open over the half that faces the world's centre, so its plaza looks out over the world. The buildings it leaves out still draw their random numbers, so nothing else on the map moves.

| World | Districts | Kind | Roads | Buildings | Props | Median road between neighbours |
|---|---|---|---|---|---|---|
| solhaven | 16 | town | 19 | 260 | 1400 | ~185 m: 30 s running, about a minute walking |
| lattice | 12 | towers | 14 | 156 | 700 | ~210 m: 34 s running |
| unwritten | 10 | ruins | 10 | 156 | 1600 | ~236 m: 38 s running |

Running is 6.2 m/s. A test keeps the median run between neighbouring districts within 20–40 s. The `-WorldAutoPlay` tour walks, so its legs take about twice as long.

## Data

`Data/Field/worlds.json` holds only ids and numbers, never story text. `ftcore::WorldTable` (`Core/include/ftcore/WorldLayout.h`) loads it.

Each world entry has:
- `id`, `map`, `style`
- `kind`: town, towers or ruins
- `side` in metres
- `seed` and `districts`
- `combat` (optional): `rhythm` (the default), `arena` or `beat`, the combat system every fight in its scenes plays (see [COMBAT_SYSTEMS.md](COMBAT_SYSTEMS.md)); an unknown value is a load error
- `story`: the script's world names it plays
- `sites` (optional): the districts the ground is shaped around, as district indices and tiers: one `entry` (where the world starts), one `climax` and any number of `major` sites, each district at most once

Each row of `scenes` places one scene in a district of a world. It can also set a `style` that overrides the world's own. A test checks that every scene of a world named in `story` is placed, and that no house or pocket scene is.

`ftcore::generateWorldLayout` turns a world entry into a layout. It is deterministic in the seed, does no allocation after load, and fails with a reason when the districts don't fit. The map builder and the game both call it, so they always agree. The layout contains:
- district centres placed best-candidate, and roads that form a tree plus a few shortcuts, none crossing another district
- each district's frame
- the buildings, trees and rocks

Nothing stands on a road or in a plaza.

## Ground

`ftcore::generateWorldTerrain` (`Core/include/ftcore/Terrain.h`) shapes each world's ground from its entry, its layout and its `sites`. The result is one height field 1.5 km across, the 1 km playable square and 250 m round it, with a sample every 1.98 m, 757 a side. It is deterministic in the seed and allocates only at load. The map builder imports it as the world's Landscape (see [Building the maps](#building-the-maps)).

- Every district stands on a flat pad. A road climbs in a straight line between its two pads, and the ground beside it banks at 0.65 at most.
- The climax is the world's highest pad, 36 m up where the roads allow. It stays in sight from the entry: the ground near the line between them keeps under it.
- Between the pads the town rolls in hills, the towers stand among broader, higher hills, and the ruins lie on a low plain broken by mesas and gullies. Off the sheer walls no slope is steeper than 1.8 (61 degrees).
- Then rain wears that ground. 200,000 droplets, and at other sides as many to each sample, run downhill over the square and 60 m round it, cutting where they run fast and laying down what they carry where they slow, so the hills take branching rills and valleys. Steep ground wears less, as rock. The pads, the roads' shoulders, the ruins' mesas and gullies and the ground just round each don't wear, and worn ground stays within the roads' banks and under the climax and the sightline. The mountains, the coast, the abyss and the features come after it.
- Past the edge, mountains rise round the towers and round the town's three dry sides, their foot wandering up to 60 m out. Their noise comes from the world's seed and its kind, so two kinds of world on one seed, as the town and the towers are, raise different ranges. The town's fourth side is its sea, behind a quay. The ruins run on 16 to 114 m in bays and headlands, then drop sheer into an abyss.
- Rain wears the mountains too, once they stand, but never inside the square, which it leaves exactly as it was. It wears them twice: first on a field four times coarser, as it would wear mountains four times smaller, so that 200,000 droplets cut gullies tens of metres across down their slopes and lay fans at their feet over the broken band of rock there; then on the full field, so that rills run down the gullies' sides. Their steepest faces wear less and the steepest not at all, as bare rock. The field's outer 20 m, the horizon, stays exactly as raised, and neither the town's coast nor the ruins wear. Between 60 and 220 m past the edge the mountains move 3.8 to 4.1 m on average: their foot rises 1 to 1.6 m under the fans and their slopes come down 2 to 6 m. All the wear makes generation four to five times as long as it is unworn: 2.3 s for the town, 2.6 s for the towers and 1.6 s for the ruins in an optimized build, 7 to 10 s in Debug.
- The town has its sea, the towers up to two trenches and three shallow pools, and the ruins one or two pits. Each keeps 25 m from the roads' edges, the pads' blends and the others, and 30 m inside the walls.
- Last, the generator follows the rain over the finished ground (`flow()`), filling nothing. Each sample passes the rain that falls on it, and all that reaches it from uphill, to every lower neighbour, most of it the steepest way, until it runs off the field's edge or into water, a trench's floor or a pit's shaft; a closed hollow's floor, or a flat, keeps what reaches it. Rain falls everywhere but on the pads and the roads' shoulders and fades in round them as the wear does, so they read 0. `flow()` reads 0 to 1 on a log scale counted in about one sample's rain at any side, 1 where a hectare's rain runs through. It stays low on the crests and the open slopes and rises along the gullies and the valleys' floors: the hollows read 1.8 to 2.5 times what the crests do. `ftcore::flowMask` resamples it over the whole field for the ground's material, which turns the ground wet where it gathers and dries it on the crests ([SCENE_LOOK.md](SCENE_LOOK.md)). The rain runs over the heights as stored, on whole steps of 7.8 mm. Steps that fine leave no terraces on the open ground that could steer the rain along their edges. Measured by running it over the unrounded heights instead, 1.3 to 2.0% of the samples where it gathers then stand on a flat of whole steps, against 1.0 to 1.4% on the stored heights. The mask moves by 8 or more on 0.8 to 1.5% of its pixels, with no change you could see. Where the rain gathers it follows the worn ground's own rills: inside the square, on open ground sloping 3 to 50%, 84 to 93% of the wettest twentieth of the samples lie in a hollow over two samples, against 28 to 29% of the drier half. Down a long, even slope the wear cuts its rills side by side, so the rain runs in parallel lines there rather than branching and joining; making them branch and join is a change to the wear, not the flow, and is queued as a pass of its own. The pass adds 45 to 104 ms to generation in an optimized build. Each closed hollow keeps its own rain, so a basin's floor shows wet specks, 1,400 to 2,500 over each field, rather than one channel, and almost nothing reaches 1: inside the square 0.3 to 0.6% of the mask reads 200 or more and at most 0.005% reads 255. The ground just below a pad or across a level road reads dry, since no rain falls on them to spill over.
- `Core/tests/test_terrain.cpp` checks all of this on the three worlds, that the open ground has no terraces and few flats, that the wear leaves the open ground curving about 1.5 times as sharply as it does unworn (1.25 times at least), that the mountains' wear moves them 1.5 m on average at least and touches nothing it may not, and that the flow reads 0 on the pads and the roads, reads 200 or more on 0.1% of the square at least and 255 on 1% at most, gathers in the hollows at least 1.5 times as much as on the crests, and comes out the same twice. Set `FTCORE_WORLD_SIDE` to a side from 600 to 4000 m and it generates all three at that side instead of their own, for measuring (see [At other sides](#at-other-sides)); any other value is ignored with a message. Its first case prints each world's generation time and a hash of its heights, cliffs and flow, which a change meant to leave the ground as it was must keep.

### At other sides

`side` can grow past 1 km: `importWorldTable` takes up to 4000 m and nothing refuses a larger world, but its ground comes out coarser. The field keeps 757 samples a side, which the Landscape's 6 × 6 components need, and 250 m round the square, so its samples spread: 1.98 m apart at 1 km, 2.65 m at 1.5 km, 3.31 m at 2 km and 5.95 m at 4 km.

- What the generator sizes in metres stays as it is: the pads' 62 m, the districts' 150 m, the roads' widths, banks and grades, the mountains' foot and climb, the features' sizes.
- What it counts in samples grows with them: the droplets' brush, path and blur, the coarse field's cells, the shape mask's rings.
- The droplets on the open ground and `flow()`'s unit follow the samples from their 1 km values, so each sample takes as much rain at any side, and the 1 km worlds come out bit for bit as before. The mountains' droplets fall over the whole field, so they already come as many to each sample.
- The map builder's masks stay 2048 pixels over the square and 1024 over the field.

Measured with `FTCORE_WORLD_SIDE` in an optimized build, at low priority:

| Side | Spacing | Solhaven | Lattice | Unwritten | Checks that fail |
|---|---|---|---|---|---|
| 1 km | 1.98 m | 1.8 s | 2.2 s | 1.7 s | none |
| 1.5 km | 2.65 m | 2.0 s | 2.6 s | 1.9 s | the road's strip, by 2.8 cm at most |
| 2 km | 3.31 m | 2.0 s | 3.1 s | 2.0 s | the strip, by 5.1 cm; the towers' relief, 1.17 times the unworn |
| 4 km | 5.95 m | 2.1 s | 6.8 s | 2.4 s | the strip, by 34 cm; the pools, 0.31 to 0.36 m deep; the relief in all three, 0.87 to 1.18 times |

The road-mask case fails at every side past 1 km, since it bakes its own 512-pixel mask, 1.95 m pixels over a 1 km square; the builder's 2048 pixels keep that size up to 4 km. The towers take longest, since their trenches and pools are sought on a 3 m grid over the square. The relief falls because the droplets' brush and path count samples, and the pools fall short because their beds start a sample in from their edges.

The road's strip breaks first. The ground is carved 15 cm under a road only within its 3 m half-width, and its shoulders stand level with the road. So the ground lies exactly under a road only while every sample round a point on its centre line lies inside the carve, which with 757 samples holds up to about 1.1 km a side. Past that the ground rises toward the road, and at 4 km it misses by more than the carve and can come through. A larger world needs about as many samples as it has 1.98 m spacings, such as 1,261 a side (10 × 10 components) at 2 km or 2,269 (18 × 18) at 4 km, with the builder's masks and its component check to match and about 9 times the time and memory at 4 km. The other way is to keep the side where the road holds.

The layout binds as well: 16 districts and 32 roads at most, while districts 150 m apart fit 36 at most at 1 km, 169 at 2 km and 676 at 4 km; keeping neighbours 20–40 s apart at a run takes more districts as the side grows; and the shortcuts, 400 m long at most, give out, so from 1.9 km each world keeps only the tree of its roads (15, 11 and 9).

## District frame

Every district uses the frame of `Data/Field/goals.json`:
- The origin is the spawn.
- +x is the way the spawn faces, toward the world's centre.
- The plaza is centred 9 m ahead.

All goals and waypoints in `goals.json` lie inside the plaza. So a field step plays the same in any district as on the Explore map: the director moves its goal and waypoint into the district with `toWorld`.

## In the game

1. The game instance loads `worlds.json` at start: `Worlds: 3 worlds, 60 scenes placed.`
2. When a field step's scene is placed, the story director opens that world's map and carries the world, district and style in `FFTStoryCarry`. If the map isn't built, it warns and uses the Explore map.
   - If the last field step played in a different world and the overworld map is built, it opens the overworld first, with `FromWorld` set: `Story: overworld hop from solhaven to lattice for <scene>.` The party walks from the old world's gate to the new one's (the autopilot walks it on autoplay), and Use at the gate opens the world's map with the same carry. Each hop adds one to the alpha line's `overworld hops`.
   - A CG step in a placed scene opens the world's map too, when it's built and no hop has to come first: the cutscene plays around the lead at the district's spawn (`Story: the CG at <scene> beat <b> step <s> plays on <map>, in district <d>.`), so the field steps beside it play in place. When a hop is due, the CG plays on the Cutscene stage and the hop waits for the next field step. See [CUTSCENE_SCREEN.md](CUTSCENE_SCREEN.md).
3. On a world's map (where `AFTExploreDirector::WorldId` is set), the director:
   - lays the world out again
   - starts the player at the district's spawn, on the district's pad: the builder saves each pad's height with the map (`DistrictFloors`), and the director puts the goals and the autopilot's waypoints on the same floor
   - switches the map's style controller to the scene's style, when the map was built with one (`-Style=`)
   - runs the field step as usual

   Log: `World solhaven: field step in district 3 (photoreal look).`, or `(style inksolhaven)` on a map with a style controller.

Without a story step, the world is a free walk. The party follows, and the HUD shows a WORLD panel ("solhaven  district 4 of 16" or "open ground").

The game opens every map by its long name under /Game/Maps (`FTStep::OpenMap`, which takes `ftcore::mapPackage`), never by the short name that `worlds.json`, the screens and the return maps carry. The engine resolves a short name to whichever package of that name it finds first anywhere under /Game, so a local copy of a world map kept elsewhere in Content, such as a before-and-after capture, was played in its place: the headless story run at a537d8b played Solhaven's steps on a copy under /Game/Trial and logged no Solhaven placings. The loader takes only short names, so a world's map can't point outside /Game/Maps, and no screen calls `OpenLevel` itself.

## The overworld

`worlds.json` has an `"overworld"` entry: its map (`Overworld`), its side in metres (300 to 1200, default 480) and a seed. `ftcore::generateOverworldLayout` lays it out the same way at build time and at run time:
- One node per world, in table order, on a ring round the centre. Neighbouring gates are 150 to 250 m apart by road, a 25 to 40 s run.
- Each node is a gate with the world's emblem 26 m behind it: a small ring of blocks in the world's kind and colours round a landmark with a beacon. The player stays full size.
- Straight roads join the gates round the ring. Nothing stands on a road or within 8 m of a gate, so the walk needs no navmesh.

A world is open once the script's first scene placed in it has started (`worldFirstScenes`, `worldsOpen`). On a free walk, Use at an open gate enters that world; a closed one says so. The story's own hops always go through.

The HUD shows an OVERWORLD panel: "to lattice" on a hop, "lattice  E  Enter" at an open gate, or "open road".

Build it with `-World=overworld`:

```
UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTBuildExploreWorld -World=overworld -NullRHI -unattended -nopause -nosplash
```

The map has the photoreal look; `-Style=<spec>` adds a style controller over it. The default build logs `overworld (look photoreal): 3 worlds, 3 roads, 18 emblem blocks, 256 props, 433 instances, 38 actors; bounds 482 x 482 m, 48 m tall.` The tour walks every gate along the roads:

```
UnrealEditor-Cmd.exe ForgottenThird.uproject /Game/Maps/Overworld -game -NullRHI -OverworldAutoPlay -AlphaFast -WorldQuitOnDone -unattended -nosplash -nosound
```

It logs `Overworld: reached the <id> gate (leg i/n) at <t> s game time.` for each leg, then `OVERWORLD TOUR DONE`. On the default layout that's all 3 gates in 2 legs, 117.5 s game time (16 s real on the 8x clock). Core covers the layout in `Core/tests/test_overworld.cpp`: the loader, determinism and clearances for 1 to 8 worlds, no allocation, the open rule, and the real script's hops (when `Data/Story` is present).

## Building the maps

The maps are generated; don't edit them by hand.

```
UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTBuildExploreWorld -World=solhaven -NullRHI -unattended -nopause -nosplash
```

Run it again with `-World=lattice` and `-World=unwritten`. `-Style=<spec>` adds a style controller over the photoreal look; the world's `style` in `worlds.json` isn't used yet. Build `M_WorldSurface` (`-run=FTBuildWorldMaterials`), the photographed surfaces (`-run=FTBuildSurfaces`) and the ground master (`-run=FTBuildGround`) first; see [SCENE_LOOK.md](SCENE_LOOK.md). Each run writes `/Game/Maps/<map>` and contains:
- the ground: the world's height field as a Landscape 1.5 km across (6 x 6 components of 2 x 63 quads, a quad every 1.98 m), with the roads carved into it, wearing the map's instance of the ground master and its road mask
- hidden walls along the playable square's edge, and round each trench, pool and pit at its rim, so the player can't walk into the sea, the abyss or a cut
- the sea and the pools as water planes
- plazas, buildings, trees and rocks as instanced meshes on a few holder actors. Each plaza and building stands on its district's pad. Each tree and rock stands on the ground under it, and the build leaves out any on ground steeper than 0.8, under water or in a cut
- the landmarks and beacons
- one large anchor shape floating 260 m over the centre
- the shared look (sun, sky light, atmosphere, fog, clouds and the camera finish), and the director
- a navmesh per district

The build fails if any field goal, from `kFieldGoals` or `goals.json`, has no full navmesh path from any district's spawn. The roads between districts have no navmesh, because they are straight and clear.

The ground wears the ground master: the kind's ground surface, rock wherever the ground is steeper than about 0.85 in 1 (40 degrees) and on the mountains' upper sides from 10 m over the square's highest ground (45.6 m in the town and 46.0 m among the towers; nothing in the ruins stands that high), sooner on their ridges and later in their gullies by a mask of the ground's shape the build makes from the height field, meeting the ground by height so that the stone breaks through first at its high points, and the road surface along every road, drawn from a mask of the roads the build makes from the layout ([SCENE_LOOK.md](SCENE_LOOK.md#the-ground)). The plazas, buildings, roofs, landmarks, trunks and rocks wear the photographed surfaces that `Data/Field/surfaces.json` gives the world's kind, and the rest keep flat tints. The build logs what the ground wears and how many roles are dressed:

| World | Ground | Rock | Road | On a road | Roles dressed |
|---|---|---|---|---|---|
| solhaven | leafy_grass (tinted) | worn_rock_natural_01 (tinted) | gravel_road | 2.5% | 8 of 8 |
| lattice | gravel_floor_02 (tinted) | worn_rock_natural_01 (tinted) | clean_asphalt | 2.1% | 3 of 8 |
| unwritten | gravelly_sand | worn_rock_natural_01 | dry_mud_field_001 | 1.7% | 3 of 8 |

The tints are the ones surfaces.json gives: the town's grass greener, (0.5, 0.9, 0.35), because the photo's dry grass showed as khaki from any distance; the towers' gravel a touch cooler, (0.85, 0.9, 1.0); and their rock, the ruins' worn rock, grey, (0.45, 0.6, 0.9). The towers first wore a dark rock photo, but even at the tint's cap of 2 it stayed darker than the shadows beside it, and the mountains' steep faces read as holes from afar; the grey worn rock reads lighter than those shadows and darker than the sand. The town's mountains wear the same worn rock, tinted to a light, slightly cool grey, (0.58, 0.78, 1.3), a little brighter than the tinted grass. Its first rock read warm brown beside the grass, and since surfaces.json must wear every surface it lists, that surface was retired along with its textures.

For Solhaven: `FTBuildExploreWorld: the ground wears /Game/Rendering/M_WorldGround (Ground leafy_grass, Rock worn_rock_natural_01, Road gravel_road), its roads from a 2048 x 2048 mask (2.5% of the square on a road), rock on the high ground from 45.6 m up, keeping to the ridges by a 1024 x 1024 shape mask, darker where the rain gathers by a flow mask as large (11.42% of the field at half or more), wet from byte 153 (0.600) to 210 (0.824), the open ground's 95th and 99.5th percentiles inside the square.` then `FTBuildExploreWorld: 8 of 8 surface roles wear photographed surfaces.` The road mask adds 56 to 72 KB to a map, the shape mask 0.3 to 0.6 MB and the flow mask 0.5 to 0.7 MB.

The flow mask's `FlowStart` and `FlowEnd` are set per map from the open ground inside the square, at its 95th and 99.5th percentiles ([SCENE_LOOK.md](SCENE_LOOK.md)):

| World | Flow mask at half or more (whole field) | FlowStart | FlowEnd |
|---|---|---|---|
| solhaven | 11.42% | byte 153 (0.600) | byte 210 (0.824) |
| lattice | 15.24% | byte 156 (0.612) | byte 209 (0.820) |
| unwritten | 9.11% | byte 150 (0.588) | byte 194 (0.761) |

The master's own 0.4 and 0.8 would set 22 to 25% of each world's open ground darkening. The builder counts the mask's bytes over every pixel inside the square that isn't 0, so the pads and the roads, which read 0, don't count, and takes the first byte at which the count reaches each share; a map with no open ground keeps 0.4 and 0.8.

The build logs the ground's numbers, then the map's. For Solhaven:

`FTBuildExploreWorld: world solhaven's ground: -33.0 to 146.4 m (pads -19.7 to 35.6 m), 0.0% of the square steeper than 1 in 1; 0 cuts walled in by 0 hidden walls; 1 water planes; 0 props left off steep, wet or cut ground.`

`FTBuildExploreWorld: world solhaven (look photoreal): 16 districts, 19 roads, 260 buildings, 1400 props, 2964 instances, 16 landmarks, 89 actors; bounds 990 x 990 m, 290 m tall; 136 goals reachable from each district.`

| World | Ground | Pads | Steeper than 1 in 1 | Cuts, hidden walls | Water planes | Props left off |
|---|---|---|---|---|---|---|
| solhaven | -33.0 to 146.4 m | -19.7 to 35.6 m | 0.0% | 0, 0 | 1 | 0 |
| lattice | -38.6 to 146.3 m | -12.1 to 36.0 m | 1.0% | 2, 8 | 3 | 25 |
| unwritten | -185.5 to 36.0 m | -18.0 to 36.0 m | 2.8% | 2, 64 | 0 | 78 |

## Headless checks

- Core: `Core/tests/test_world_layout.cpp` covers:
  - the loader
  - determinism and layout invariants for every kind and district count
  - no allocation
  - the committed worlds' median runs
  - the climax's open ring: the buildings ahead of its plaza left out (7 to 9), those behind kept (6 to 8), and nothing else moved
  - the real script's placements (when `Data/Story` is present)
- The tour walks the roads through every district:

  ```
  UnrealEditor-Cmd.exe ForgottenThird.uproject /Game/Maps/World_Solhaven -game -NullRHI -WorldAutoPlay -AlphaFast -WorldQuitOnDone -unattended -nosplash -nosound
  ```

  It logs `World solhaven: reached district <d> (leg i/n) at <t> s game time.` for each leg, then `TOUR DONE`. Solhaven takes 18 legs and 814 s of game time, about 106 s real with `-AlphaFast`; Lattice 15 legs and 803 s, Unwritten 17 legs and 992 s, each with the lead's feet 5 cm over the entry's floor.
- `-WorldDistrict=<n>`, outside a story step, starts the lead on district n's spawn and floor instead of the PlayerStart, and starts a tour there. With `-ExplorePortrait` it frames the ground from that district's pad, such as the climax's hilltop (Solhaven's district 14, Lattice's 1, Unwritten's 4).
- A second after each placing of the lead on a world map (the start of a free walk or the tour, and every story step, field or CG), the director logs its feet against the floor saved for the district it stands in: `World solhaven: the lead's feet at z 5 cm in district 0, whose floor is 0 cm.` The two agree to a few cm when the map's ground is the one the builder imported. The entry's floor is always 0, so the tour's line only shows the ground is there; a story step in another district logs that district's floor.
- The alpha gate (`tools/gate.ps1 -Alpha`) plays the whole story, with Solhaven's field steps on its map.
