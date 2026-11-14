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
- `combat`: retired. Each fight names its own system in `Data/Battle/encounters.json` (see [COMBAT_SYSTEMS.md](COMBAT_SYSTEMS.md)); the loader warns and ignores the key
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

Since 2026-11-06 surfaces.json gives the town solhaven_paving_slab for its ground, clean_asphalt for its roads and worn_rock_natural_01 at (0.62, 0.78, 1.2) for its rock, and board-formed concrete, bitumen roofing and grey-tinted stone and cobbles for its buildings, roofs, landmarks and plazas. World_Solhaven still wears the row above until it is rebuilt.
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

### Massing maps

A world whose production plan blocks its town out in a massing table is built from that plan's files instead, which stay outside the repo and are named on the command line (the commandlet's header, `FTBuildMassingWorldCommandlet.h`, lists them):

```
UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTBuildMassingWorld -Height=<json> -Boxes=<csv> -Walls=<csv> -Goals=<csv> -Entry=<x,y,z> -Look=<x,y> -Sea=<x> -Map=<name> -World=<id> -NullRHI -unattended -nopause -nosplash
```

Without `-World` the map is a free walk with no director. With `-World=<id>` it is that world's: the goals table's rows `D0`, `D1`, ... give each district's plaza centre and floor, and each district's spawn is the row's `spawn_x`, `spawn_y` when given, or else the generator's own rule (9 m behind the plaza's centre, facing the square's centre). The spawn stands on the highest deck or landing at most 6 m over the higher of the plaza's floor and the ground there. The build pins the world's layout to these places (`WorldLayout::pinDistricts`), so every district's frame, `districtAt` and a story step's placing follow the plan while the generated roads and blocks stay as they were (the massing map doesn't build them). It logs each district, warning when the lead would stand inside a box or wall at its spawn:

`FTBuildMassingWorld: district 9: plaza (435.0, 395.0) at 35.0 m; spawn (430.3, 387.3) on <id> at 40.0 m, footing 0.00 m (0 of 17 points skipped as walls).`

Positions in the log are the plan frame's metres, as in the tables. It then places the director with the districts' floors, centres and spawns, and with the navmesh checks each district's spawn as well as each goal for a path from the entry; the summary line adds `district spawns reached <n> of <m>`, and `-Strict` fails on a missing one.

Each spawn's footing is checked as well (`ftcore::massingSpawnFooting`): the largest step between the spawn's floor and the floor at 17 points within half a metre of it, read as the spawn's own floor is, skipping a point where a box stands in the lead's way (a wall, not a floor). A step over 0.5 m means the spawn sits on a steep bank's toe or a box's edge. The lead slides off it, and the navmesh, which leaves such a slope out, has nothing under the point, so a path asked to it finds none; a check that starts there never notices, since the entry is projected with a 3 m reach. That spawn gets a warning naming the point, and the goals table's `spawn_x`, `spawn_y` move it. A summary line counts the spawns that hold, the reach summary ends `spawn footing within 0.50 m: <n> of <m>`, and `-Strict` fails on one over. The entry gets the same check, with how far its z lies from its floor, since it is the player start too. Where the entry is also a district's spawn, the two move together.

The navmesh climbs no ledge the lead can't. Recast joins two sides of a ledge when they are at most `ceil(AgentMaxStepHeight / CellHeight)` 10 cm cells apart, each side rounded to a cell. At the engine's default of 35 (four cells), ledges up to 49 cm can join, above the lead's 45 cm step (the character movement's `MaxStepHeight`), and a path then runs up an edge the lead walks into. `Config/DefaultEngine.ini` sets 25 (three cells), so a ledge of 40 cm or more never joins, and a full path is one the lead can walk. A raised plate whose every edge stands 40 cm or more above the ground around it has no path onto it. The setting is project-wide: every navmesh built after it uses three cells. Navmeshes already built keep their tiles until they are rebuilt.

`-Entry`'s z is the surface the lead stands on at x, y (the ground, or a deck or landing), not the floor of the plaza it serves: a z inside a box, such as a bowl's floor, is off the navmesh, and then no walk starts. A point within 1 m of the entry counts as reached. A miss says where the navmesh met the point and where the path from the entry stops:

`FTBuildMassingWorld: goal D5 (190, 430, +80): no full path from the entry; on the navmesh at (190.0, 430.0, +80.1); the path stops at (190.0, 437.5, +80.1), 7.5 m short.`

A path that stops at a box's face points at a goal inside a box that is still solid; one that stops at a step or a drop points at a gap in the massing.

The check's path search has a budget of 32768 nodes, well over the engine's default of 2048, so a long way across a big map isn't cut short by the search itself. Each target's line ends with what its query came back with, `query full`, `query partial` or `query none`, and `node limit hit` or `node limit not hit` (`no query` for a point off the navmesh or at the entry). A summary line counts them:

`FTBuildMassingWorld: reach (search budget 32768 nodes): goals <n> of <m> reached (<f> full paths, <e> at the entry; <p> partial, <q> no path, <o> off the navmesh; the node limit hit on <k>); district spawns ...`

A miss whose search hit the limit may only be the budget's; a miss with the limit not hit is one the navmesh has no way to.

`-Load` builds nothing. It loads the saved map in an editor world of its own, with the navmesh saved with it (the one the game walks), runs the same check from `-Entry` with the same lines, and never saves, so the map's file is left as it was; hash it before and after to show that. It takes the same files as the build, since the goals and the spawns are worked out from them, and logs the navmesh's tile count and how far the map director's spawns lie from those (`the map's director has the plan's 16 district spawns, the furthest 0.00 m from the plan's`), so the check walks to the spawns the tour walks to. A map saved without navmesh tiles has them built in memory, not saved, and the log says so. A `-Kit` table is still checked; `-Dump` is ignored.

Solhaven's build map (`Content/Maps/World_Solhaven_Build.umap`, rebuilt with district 0's spawn and the entry 2 m east of a bank's toe) checks with goals 13 of 13 and district spawns 16 of 16 reached, all full paths, with the node limit hit on none. Every spawn's footing holds, the most at 0.04 m. Reloaded with `-Load`, its navmesh has 33660 tiles, and the check gives the same counts. The tour and a `-WorldDistrict=9` tour each walk all 16 districts (18 and 19 legs) with no STUCK, and the walk into district 0 finds a full path on every query.

Three optional goal-table columns name massing ids. `held` lists a goal apart: it stands inside or on a box held back for its own piece, so it is neither walked nor counted, and the summary names it by box (`goals reached <n> of <m> (listed apart, not counted: D10 on <id>, ...)`). `goal_box` and `spawn_box` mark the goal or the spawn as a temporary placement just outside that box, until the piece lands. Each is logged with its box and where it stands against it; a `spawn_box` line also gives the generator's own spawn, which stands in that box's footprint (a warning when it doesn't):

`FTBuildMassingWorld: goal D4 (363.5, 585.0, +60.0) is a temporary placement outside <id> (a hidden kind, not built, which holds the plan's own point): 7.0 m over its top.`

The summary line closes with `temporary placements: <g> goals, <s> spawns`, and `-Strict` counts only the goals not listed apart.

`-Kit=<csv>` swaps boxes for finished meshes already imported with `FTImportProp`. Each row has `box` (a massing id) and `asset` (the mesh's object path), and optionally `fit`, `turn` and `dz`. The mesh's bounds land on the box's centre at the box's rotation, with their bottom on the box's lowest corner, raised by `dz`; `turn` adds degrees for a mesh whose axes differ from the box's. `fit` is `real` by default (the mesh keeps its size) or `footprint` (scaled on each axis to the box's footprint and height). The mesh is labelled with the box's id and collides with nothing. The box stays as its hidden collider, labelled `<id>-COL`, so the lead walks on the box's shape rather than the mesh's. A missing box or mesh fails the run. The log gives each mesh's size and scale, how many boxes each asset stands in for, and the built boxes left.

`-Surfaces -Streets=<csv>` dresses the map in Solhaven's photographed surfaces, and without `-Surfaces` the build is exactly as before (`-Streets` alone does nothing). `-Surfaces` needs `-Streets`, and everything it loads is checked before anything is built: the 15 instances `/Game/Rendering/Surfaces/solhaven_<id>/MI_Surface_solhaven_<id>`, the ground master `M_WorldGround` and `M_Look_Water`. Each built box wears one of them, chosen in Core by `ftcore::massingLookSurface` from the box's `src`, `kind`, id prefix and `row` (the `row` column of the boxes table is read when it has one): kerbs and the other added walls `pole_concrete`; flights, toe stairs, ramps, landings and terraces `paving_slab`; cliff cover plates, the seawall, breakwater and quay `board_concrete`; decks, piers, boats and bridges `oak_plank`; cranes, masts, posts, barriers and gates `galv_steel`; every other built box is a building, wearing `soot_brick`, `breeze_block` or `alu_cladding` by a hash of its row, so that all the parts of one building share a finish. The Landscape wears an instance of `M_WorldGround` (`MI_Massing_Look_Ground`) in place of the flat ground tint: `solhaven_paving_slab` for the ground, `solhaven_board_concrete` for the rock, which starts 20 m over the field's highest ground so that only steep faces turn to it, and `solhaven_bitumen_roof` for the carriageway (the set has no asphalt, and bitumen is the nearest; board concrete takes the road if bitumen isn't built). The road mask, `T_Massing_RoadMask` in the map's package, is filled from the streets table `-Streets=<csv>`, whose columns `id`, `width_m` and `pts` are read (`pts` is `x:y[:z];x:y[:z];...`, plan frame, metres; every other column is skipped and never kept): `ftcore::importMassingStreets` reads it, `ftcore::massingStreetsToEngine` moves the points to the engine frame, and `ftcore::massingRoadMask` writes, for each pixel, the greatest over the streets of `255 clamp(0.5 + (width/2 - d) / (2 feather), 0, 1)`, `d` the distance from the pixel's centre to the street's polyline and the feather 2 m, which is what the ground master decodes by. The ground master reads the mask as world XY over `MaskSideCm` plus a half, so the mask is centred on the engine's origin and not on the field: it is 8192 x 8192 pixels, as wide as twice the Landscape's furthest edge from the origin plus a margin, rounded up to whole metres (about 0.43 m a pixel on a 3.5 km field), and the streets land on the carved terrain through the same plan-to-engine move as the boxes. Its outermost ring is empty. The master's shape and flow masks stay at its defaults. The sea plane made by `-Sea` wears `M_Look_Water` in place of the flat tint. One log line gives the counts: `FTBuildMassingWorld: look: boxes by surface: ...; N streets read; road mask ...; ground wears ...; sea wears ...`. `-Kit` and the night rig are not part of this look.

`-Dump=<csv>` also writes the saved map's actors to that file, so a check can read what the map holds rather than what the tables asked for. It reads them back from the actors after the save, so it changes nothing in the map. Each actor gets one row:

- `label`, empty for an actor the build leaves unlabelled, and `class`;
- location in cm and rotation in degrees, in the engine frame;
- scale, and whether the actor is hidden in game;
- `mesh_vertices_cm`, on a sloped box's row: its mesh's vertices as `x y z` relative to the actor, `;` between them, in `massingMesh`'s order (0 to 3 the bottom ring, 4 to 7 the top ring over them, 0 to 1 along the box's +u axis);
- `district_floors_cm`, on the director's row: its floors in cm, D0 first, `;` between them.

`#` lines at the top of the file repeat this. The log adds `dumped <n> actors to <csv>`, and a dump it can't write fails the run. Like the tables, keep the file outside the repo.

The walls table's blockers carry the actor tag `FTHiddenWall`, so the tour's edge legs can tell a hidden wall from a built one. WALL rows of `-Boxes` aren't built, so they have no actor to tag.

`-Tour=<csv>` (with `-World`) gives the map's director the stops its `-WorldAutoPlay` tour walks after the districts (`ftcore::importTourStops`). The table has a header row and a row per stop, in the order the tour walks them, with `#` comment rows allowed anywhere. Other columns are skipped and never kept. The columns are:

- `id`: letters A to Z, digits, `_` or `-`; unique;
- `kind`: `reach`, `edge` or `push`;
- `x`, `y`, `z`: the target in the plan frame, metres. `z` is the floor to stand on for a reach stop and the ground at the edge point for an edge stop. A push stop doesn't use it, but it must still be a number;
- `start_x`, `start_y`, `start_z`: where the lead is set down before the leg, with `start_z` the floor there. Either all three are empty or all three are numbers. An edge or push stop needs them;
- `edge`: `W`, `S`, `N` or `E` on an edge stop, empty on the others;
- `still`: `1` to take a still where the leg ends, else `0`.

The director also gets the sea's level and the plan's side, for its judging and for giving its lines in the plan frame. The build logs `FTBuildMassingWorld: tour: <n> stops after the districts: <r> reach, <e> edge, <p> push; <s> end in a still.` Under `-Load`, the saved director's stop count is checked against the table's, with a warning when they differ.

`-Coverage=<csv>` (with `-World`) checks the navmesh's islands after the reach check, in a build and with `-Load`. The table has the columns `id`, `role`, `x`, `y` and `z` (the plan frame; `z` is the floor there), and `role` is `pocket` or `on`. The navmesh's ground polys are joined where they share an edge or an off-mesh link (`ftcore::navIslands`, following a link one hop). District 0's island is the one its spawn projects to. Every other island over 50 m² must be declared by a pocket point inside it, and every on-point must lie on district 0's island (`ftcore::classifyNavIslands`). With a sea, the navmesh under it is left out before the islands are counted (`ftcore::navPolysAboveSea`), since the sea plane has no collision and the ground under it gets navmesh too: a poly goes when its highest corner is at or under the sea's level plus 0.3 m, the tables' dry margin, inland low ground included, and a point on a poly left out is off the navmesh. In a build the sea is `-Sea=<x>`'s, at the datum. Under `-Load`, `-Sea=<x>` is optional: given, it wins, at the datum; left out, the saved director's sea level is used if it has one. The summary line then gives `(under the sea at <z> m, from -Sea` or `from the saved director: <s> polys, <a> m2, left out)` after the tiles, and its poly count is all of them. It logs one summary line:

`FTBuildMassingWorld: coverage: <n> navmesh polys in <t> tiles, <i> islands, <a> m2 walkable; district 0's island #<k>, <a> m2 (<pct>% of it); <b> islands over 50 m2 besides it: <d> declared by a pocket, <x> in built boxes (left out: <r> roofs, <a> m2, and <s> interiors, <a> m2), <u> undeclared (<w> of them roofs a walk surface meets); on-points on district 0's island <o> of <m>; pockets <p> (<q> in an island over the size, <r> off the navmesh).`

Recast takes a closed box as hollow, so it lays navmesh on a built box's roof and inside it, where no one walks. An island over the size that no pocket declares is left out when it lies in built boxes (`ftcore::navIslandsInBoxes`, over the boxes and walls tables): every corner of it is within 0.3 m of a built box's footprint and within 0.5 m of its height span there. A corner on the top of a walk surface (a sloped row or a deck) lies in nothing, so the navmesh on a ramp or a quay always counts. Such an island is a roof when every corner is on a box's top, else an interior. A roof stays undeclared when a walk surface's top comes within 2 m of it in plan and 0.5 m in height, since the lead can step onto it there. Interiors are always left out; a row that is walked inside declares its island with a pocket point.

There is one more line for each of the 12 largest islands left out (`left out, the roof|inside of <id> and the boxes beside it: island #<n>, <a> m2, centroid (<x>, <y>), z <z1> to <z2> m, <p> polys.`), one for each undeclared island (its area, centroid, z range and poly count, and `; a roof a walk surface meets` when it is one) and one for each on-point off district 0's island or off the navmesh. With `-Strict`, an undeclared island or a missed on-point fails the run. With `-NoNav` the check is skipped, and the log says so.

A cover plate (a named cliff box with `cover` set, `ftcore::massingNavObstacle`) gets no navmesh at all: none on its top, in its slab or on ground buried in it. The build attaches a hidden box with no collision to the plate (`UFTPlateNavNull`), which the navmesh build takes as a dynamic obstacle of `NavArea_Null`: one strip for each 0.5 m of rise along a sloped plate, each reaching 0.3 m past the slab above and below (`ftcore::massingNullBand`), so the street under a raised plate keeps its navmesh. The plate's mesh keeps its collision, so the lead, the traces and the edge judge still meet it. The component is saved with the plate's actor, so a navmesh built again nulls the same strips. The build logs `FTBuildMassingWorld: cover plates: <n> boxes are navmesh obstacles, <s> null strips (each rising at most 0.50 m, 0.30 m past the slab); they keep their collision.`

`-ReadBack=<json>` (a build only; ignored under `-Load`, with a line saying so) reads the Landscape back once it is built, after every compile has finished, so it reads the final heightmap. The json names a height field in the same format as `-Height`'s. It must be at the built field's spacing, on its grid and inside it, so a small window of the 1 m field will do. Every sample of that window is read back from the Landscape's heightmap and must match exactly. The collision surface is then read at 4096 fixed points between the samples (`ftcore::readBackSurfacePoints`, off the samples and off both diagonals). Each must lie within 5 mm of the field's two triangles in that cell, cut north-west to south-east. The line also gives the other cut, south-west to north-east, as evidence of which one the collision uses:

`FTBuildMassingWorld: read-back: heightmap <nx> x <ny> samples from plan (<x>, <y>), Landscape vertices (<x1>, <y1>) to (<x2>, <y2>): <k> of <n> match exactly, <m> don't; the worst is <m> m at (<x>, <y>).`

`FTBuildMassingWorld: read-back: surface at 4096 points between the samples, cut north-west to south-east: <k> within 0.005 m, <o> over (<u> unread); the worst is <m> m at (<x>, <y>). Cut south-west to north-east instead: <o> over, the worst <m> m.`

With `-Strict`, a mismatch on either line fails the run.

### The Solhaven strip

`Content/Maps/World_Solhaven_Strip.umap` is a build product and is not in git (`.gitignore` lists it): build it on each machine with `FTBuildMassingWorld -World=solhaven -Map=World_Solhaven_Strip`, the massing switches above and the dressing switches the commandlet's header lists (`-Surfaces -Streets -Kit -Roads -RoadDecals -Lawn -LawnTex -TreeMeshes -Lots -KeiCars -Backdrop`, and so on). The plan's tables and the full command line stay with the production plan outside the repo. Build the world materials, surfaces and look lab first (`FTBuildWorldMaterials`, `FTBuildSurfaces`, `FTBuildLookLabWorld`; the road decals' materials are built by the strip run itself under `-RoadDecals`); `Content/Maps/Look_Lab.umap` is `FTBuildLookLabWorld`'s output.

The strip also wears content that a public clone doesn't have, all local only and never committed:
- `/Game/Fab/Lawn` (`-LawnTex`): the lawn's scanned maps. Without them the build logs one warning and the lawn wears the project's grass surface.
- `/Game/Fab/...` marketplace meshes and textures named by the kit and tree tables (`-Kit`, `-KitTable`, `-TreeMeshes`).
- `Content/Props/` (street furniture, trees, lamps, the clock tower's pieces) and `Content/Vehicles/` (the kei cars), imported with `FTImportProp` from the production plan's meshes.

A `-Kit` row whose mesh is missing fails the run and names it.

### Stills from a fixed eye

A look-lock frame, or any review frame named by its eye, is taken off-screen from the built map (`FTWorldEyeShotSubsystem.h`):

```
UnrealEditor-Cmd.exe ForgottenThird.uproject /Game/Maps/<map> -game -RenderOffScreen -unattended -nosplash -nosound -WorldEyeShot=<file.png> -WorldEye=<x,y,z> -WorldEyeAt=<x,y,z> [-WorldEyeLens=35] [-WorldEyeSide=1000] [-WorldEyeSize=2560x1440] [-WorldEyeDelay=8]
```

The eye and the point it looks at are in the plan frame, as in the tables. The lens is a focal length on a 36 mm-wide frame (35 mm sees about 54.4 degrees across). The view stays on the eye with the HUD hidden while every shader compiles and the delay passes; then the still is saved and the game quits, with 1 if it never arrives. The log gives the eye in both frames: `WorldEyeShot: eye (430.26, 387.35, 41.70) to (110.00, 500.00, 110.00) in the plan frame, 35.0 mm (54.4 degrees across), ...`. A `-NullRHI` run draws nothing.

One character can stand in the same still, looping one clip in place (`FTPlaceSkeletalSubsystem.h`). Add to the run:

```
-PlaceSkin=<mesh object path> -PlaceAt=<x,y,z> [-PlaceAnim=<clip object path>] [-PlaceFace=<x,y,z>] [-PlaceSide=1000]
```

`-PlaceAt` is where the soles go and `-PlaceFace` the point it turns to (the eye, usually), both in the plan frame. For 8 seconds after begin play, a line from 2 m above to 5 m below `-PlaceAt` looks for the ground. If it finds nothing, the plan z stands. The log names the result: `PlaceSkeletal: ground found after ...` or `PlaceSkeletal: no ground ...`. A mesh or clip that doesn't load ends the run with 1, and so does a clip on another skeleton. There is no AI, no collision and no spawner: it is for looking at.

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

  From Git Bash, run `export MSYS_NO_PATHCONV=1` first. Without it Git Bash rewrites `/Game/...` into a Windows path, no map loads, and the run still exits 0 with no STUCK, which means nothing. `tools/world_tour.ps1 -Map /Game/Maps/World_Solhaven -Log <new log path> [-District <n>]` runs the tour and checks the log for the map, the tour's start and its end. It prints `TOUR OK`, `TOUR STUCK <n>`, `TOUR FAIL <n>` (a stop leg's fail, below) or `VOID`, and exits 0, 1 or 3. With `-CheckOnly` it judges an existing log and runs nothing. `-Kinds edge,push` passes `-WorldTourKinds`. `-Stills <dir>` runs with `-RenderOffScreen` instead of `-NullRHI` and passes `-WorldTourStills=<dir>`.

  On a massing map built with `-World`, whose districts the map pins, the tour walks the navmesh between the spawns instead of the generated roads, which that map doesn't have, and says so when it starts (`tour of 18 legs on the navmesh, spawn to spawn`). Each leg ends at the district's spawn, not its plaza's centre: the builder checks every spawn for a path from the entry, while a centre can stand inside a piece that is still a box. The pilot asks the navmesh for a path to the spawn's own floor, which may be a deck or terrace above the plaza. Reaching is judged on the ground plan, so each reached line also gives the lead's feet against the spawn's floor: `reached district 9 (leg 17/18) at <t> s game time, feet at z <cm> cm (its spawn's <cm> cm)`. The first leg is the district the tour starts in, so it is reached at once when the lead starts on that district's spawn.

  The pilot gives a leg up on any of three tests, each logged as `ExploreDemo: STUCK <m> m from spot <300 + leg> (<why>; <path>).`:

  - a stall: under 1 m/s for 15 s of game time (`15 s under 1.0 m/s`);
  - no new best for 15 s of game time (`15 s without a new best`). A new best is 10 cm closer in a straight line; on a map that pins its districts it is also 0.5 m less of a full navmesh path left, so a lead walking a detour on a full path runs on. On a partial path, or none, only the straight line counts;
  - on a map that pins its districts, the leg's cap: 300 s of game time on one leg, whatever the path (`<t> s on the leg, past its cap of 300 s`), so a lead that keeps finding a shorter way along a path that circles is still caught. The longest leg of a clean tour of a pinned 3.5 km map took 167 s.

  `<path>` is `walking straight at it`, `with a full path, <m> m of it left, best <m> m`, `with a partial path` or `with no path`, with `its last search ran out of nodes` added when the pilot's last query hit the engine's node limit. With `-WorldQuitOnDone`, it then gives that leg up and logs `World solhaven: gave up on district <d> (leg i/n), <m> m short, at <t> s game time.` and a second line that places the stop: `leg i stopped with the lead at (<x>, <y>) m in the engine frame, feet at z <cm> cm; the goal asked at z <cm> cm; the last query found <no path | a partial path | a full path> (<k> points), ending <m> m from the goal at z <cm> cm[; its search ran out of nodes].` (the plan frame is (x + 500, 500 - y) on a 1 km map). It then sets the lead down on that leg's spawn, `leg i given up; the lead is set down on district <d>'s spawn for leg i+1`, so the next leg starts where the tour meant it to and one stall is given up once, not again on every leg after it. A tour with a leg given up ends with `TOUR ENDED, <k> of <n> legs given up (legs ...)` instead of `TOUR DONE`, and the run exits 1. Without the flag the lead stays where it stopped.

  Once a game second the tour also logs the lead's state, and nothing else about the tour changes: `World solhaven: trace leg i/n (spot <300 + leg>) at <t> s game time: the lead at (<x>, <y>) m in the engine frame, feet at z <cm> cm, <v> m/s; <m> m from the goal, best <m> m, <s> s without a new best; path: <never asked | no path | a partial path | a full path>[ (out of nodes)], <m> m left, best <m> m; <s> s slow; <s> s on the leg.` It gives what each of the three tests reads, so a STUCK can be told apart from the log alone: a stall, a lead still moving that got no closer, or a leg past its cap. The path left and its best are -1 when the pilot has no full path (and off a pinned map). Followers aren't traced: they have no goal of their own and walk the lead's trail.

  It logs `World solhaven: reached district <d> (leg i/n) at <t> s game time.` for each leg, then `TOUR DONE`. Solhaven takes 18 legs and 814 s of game time, about 106 s real with `-AlphaFast`; Lattice 15 legs and 803 s, Unwritten 17 legs and 992 s, each with the lead's feet 5 cm over the entry's floor.
- Tour stops. On a massing map built with `-Tour=<csv>`, the tour walks the district legs as above, then each stop in the table's order, each as a leg of its own (the legs are numbered across both). A map without stops tours as before, line for line, except on a massing map built with `-Sea`: there the dry check below runs on the district legs too, and a wet leg ends the tour with the new end line. The switches:

  - `-WorldTourKinds=<list>` picks the kinds to run: `reach`, `edge` or `push`, separated by `,` or `+`. A name that isn't a kind logs an error, and every kind runs.
  - `-WorldTourStops=only` skips the district legs.
  - `-WorldTourStills=<dir>`: with a renderer, each stop marked `still` saves `<dir>/<id>.png`.

  The start line counts what will run: `World <id>: tour of <n> legs on the navmesh: <k> district legs spawn to spawn, then <s> of the map's <m> stops (<r> reach, <e> edge, <p> push; <q> end in a still).` With `-WorldTourStops=only`, the district part reads `the district legs skipped (-WorldTourStops=only)`. A stop with a start sets the lead down there, facing the target, and the leg begins once the lead has settled on the ground (1 s, at most 3 s). A stop without a start walks on from where the last leg ended.

  - Reach: the pilot walks the navmesh to the target with a search budget of 65535 nodes instead of the engine's 2048. That is Detour's most: its node pool's index is 16 bits, so a bigger pool wraps it on a long search (the builder's reach check keeps 32768). The leg's cap is 2 s of game time a metre of the straight line from where it starts, and never under 300 s (`ftcore::tourLegCapSeconds`). The walk's first full path re-sets the cap by that path's length, never under the straight line's and still timed from the walk's start (`ftcore::tourLegBudget`; an edge stop's walk too): `World <id>: stop <id> (leg i/n): a full path of <m> m (the straight line <m> m) at <t> s on the walk; its cap is now <s> s.` A partial path, or none, keeps the straight line's cap. A full path over 5 times its straight line also logs `World <id>: INFO stop <id> (leg i/n): its full path, <m> m, is <r> times its straight line of <m> m (over 5), for the plan's owner.`, never a fail; `tools/world_tour.ps1` shows these lines after the edge summaries. A stop counts as reached within 4 m on the ground plan: `World <id>: reached stop <id> (leg i/n) at <t> s game time, <s> s on the leg, feet at z <cm> cm (the stop's <cm> cm).` A stop given up with `-WorldQuitOnDone` logs `World <id>: gave up on stop <id> (leg i/n), <m> m short, at <t> s game time.` and the located line, as on a district leg. The next leg then starts on that stop's target, unless it has a start of its own: `leg i given up; the lead is set down on stop <id>'s target for leg i+1`. Without the flag the lead stays put.
  - Edge: from its start, the lead walks the navmesh to the navmesh point nearest the target (looked for within 20 m) on the island the lead stands on, to within 1 m, so it never aims at a roof or a shelf it can't walk to. The islands are the coverage's (`FTGatherNavIslands`, the polys under the sea left out, numbered alike), gathered once a tour at its first edge stop: `World <id>: the navmesh's islands for the edge walks: <n> polys in <t> tiles, <u> of them left out under the sea, <i> islands; gathered in <s> s.` Each edge stop logs its aim: `World <id>: edge stop <id> (leg i/n): the walk aims at (<x>, <y>) m in the plan frame, z <cm> cm, on the lead's island #<k>, <m> m from the target (<m> m across the ground).` A lead on none of the islands aims at the nearest point on any island, as before, and the line says `on any island (...)`. It then pushes straight at the target for 3 s of game time, with no navmesh. Once it is within 0.25 m of the target, it keeps pushing that way. A capsule a little narrower than the lead, lifted half a step off the floor, is then swept 3 m along the push on the pawn channel, which is the one that stops the lead. Down-traces on the visibility channel, which hidden walls don't block, find the ground: 1.5 m past a hidden wall, or every 0.5 m along the probe when nothing (or only walkable ground) is in the way. The leg meets a cause (`ftcore::judgeEdge`) if it finds one of these:
    - a built wall: a visible actor, logged by its label;
    - a cliff: the Landscape steeper than the lead can walk;
    - a drop: ground over 2 m below the feet, or no ground at all;
    - water: ground under the sea's level.

    A hidden wall with dry ground past it within 1 m of the feet's level is a `BARE HIDDEN WALL`, and anything else is `UNJUDGED`; both are fails. A walk given up counts as given up, and the edge is still pushed and judged from where the lead stopped, with or without `-WorldQuitOnDone`. With no navmesh within 20 m of the target on the lead's island, the push starts where the lead stands, with a warning that names the island. One line a leg: `World <id>: edge stop <id> (leg i/n, edge <E>): <met | FAIL>, <cause>; the lead at (<x>, <y>) m in the plan frame, feet z <m> m, <m> m from the target; the sweep hit <label at <m> m | nothing within 3.0 m>; <ground at z <m> m (the sea's level <m> m) | no ground found>[; it fell <m> m on the push].` When the tour ends, each edge gets a summary: `World <id>: EDGE <E>: <k> of <n> met a cause (cliff <a>, water <b>, drop <c>, built wall <d>); bare hidden wall <f>; unjudged <u>.`
  - Push: from its start, the lead is pushed straight at the target for 8 s of game time, with no navmesh. The stop holds if the lead never comes within 3 m of the target: `World <id>: push stop <id> (leg i/n): held, <m> m from the target at the closest; ...`. Otherwise it logs `GOT THROUGH`, a fail.

  Dry: on a map with a sea, a leg on which the lead's feet go 5 cm under the sea's level logs `World <id>: WET on leg i/n at (<x>, <y>) m in the plan frame, feet z <cm> cm, under the sea's level <cm> cm.` It logs once a leg, on every leg, district legs included, and each wet leg is a fail.

  Stills: a stop marked `still` takes its still where its leg ends, with `HighResShot 1600x900`, and logs `still for stop <id> requested: <file>` and then `still for stop <id> saved: <file>` (or a warning after 5 s). Under `-NullRHI` it logs `still for stop <id> skipped: no renderer (-NullRHI).`, and without `-WorldTourStills` it logs `skipped: no -WorldTourStills=<dir>.`

  The stop legs are traced once a game second like the district legs (`trace leg i/n (stop <id>, walking | settling | pushing)`). The tour's end line adds counts by kind and the fails:

  `World <id>: TOUR DONE, <n> legs: <k> district legs, <s> stops (reach <r>, reached <a>; edge <e>, met a cause <m>; push <p>, held <h>); fails: gave up 0, bare hidden wall 0, unjudged 0, got through 0, wet 0; <t> s game time (<t> s real).`

  Any fail ends the tour with `TOUR ENDED, ...; <f> fails: gave up <g> (legs ...), bare hidden wall <b>, unjudged <u>, got through <x>, wet <w>; ...`, and the run exits 1 under `-WorldQuitOnDone`. `tools/world_tour.ps1` reads the same lines. It prints the stop counts and each edge's summary. It counts a stop given up, an edge `FAIL`, a push that got through and a wet leg as fails, and, with `-Stills`, a still that wasn't saved too. It prints `TOUR FAIL <n>` when there are fails but no STUCK.
- `-WorldDistrict=<n>`, outside a story step, starts the lead on district n's spawn and floor instead of the PlayerStart, and starts a tour there. With `-ExplorePortrait` it frames the ground from that district's pad, such as the climax's hilltop (Solhaven's district 14, Lattice's 1, Unwritten's 4).
- A second after each placing of the lead on a world map (the start of a free walk or the tour, and every story step, field or CG), the director logs its feet against the floor saved for the district it stands in: `World solhaven: the lead's feet at z 5 cm in district 0, whose floor is 0 cm.` The two agree to a few cm when the map's ground is the one the builder imported. The entry's floor is always 0, so the tour's line only shows the ground is there; a story step in another district logs that district's floor.
- The alpha gate (`tools/gate.ps1 -Alpha`) plays the whole story, with Solhaven's field steps on its map.
