# The three big worlds

The story's field steps play in three big worlds:

- **Solhaven**: a town. Its Ink-Solhaven and Restored scenes play here.
- **Lattice**: towers.
- **Unwritten**: ruins.

Scenes that aren't placed in a world, such as the house and the pocket, still play on the small Explore map.

The **overworld** joins them: a small walkable map with one gate per world. When the story moves from one world to another, the party walks the road between the two gates before the next field step starts.

Each world is a blockout for now: shapes, roads and landmarks on the world's own hills under the shared photoreal look ([SCENE_LOOK.md](SCENE_LOOK.md)). The ground comes from Core (see [Ground](#ground)) and is imported as a Landscape. The ground wears photographed ground, rock on its steep faces and its high ground, and the roads, but the trees and rocks are still simple shapes, nothing grows on the ground yet, and there is no streaming and there are no biomes.

## Size

Each world is 1 km a side. Districts sit at least 150 m apart, joined by straight roads. A district is a stone plaza 72 m across, ringed by buildings, with a tall landmark topped by a gold beacon.

| World | Districts | Kind | Roads | Buildings | Props | Median road between neighbours |
|---|---|---|---|---|---|---|
| solhaven | 16 | town | 19 | 269 | 1400 | ~185 m: 30 s running, about a minute walking |
| lattice | 12 | towers | 14 | 163 | 700 | ~210 m: 34 s running |
| unwritten | 10 | ruins | 10 | 165 | 1600 | ~236 m: 38 s running |

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

`ftcore::generateWorldTerrain` (`Core/include/ftcore/Terrain.h`) shapes each world's ground from its entry, its layout and its `sites`. The result is one height field 1.5 km across, the 1 km playable square and 250 m round it, with a sample every 1.98 m. It is deterministic in the seed and allocates only at load. The map builder imports it as the world's Landscape (see [Building the maps](#building-the-maps)).

- Every district stands on a flat pad. A road climbs in a straight line between its two pads, and the ground beside it banks at 0.65 at most.
- The climax is the world's highest pad, 36 m up where the roads allow. It stays in sight from the entry: the ground near the line between them keeps under it.
- Between the pads the town rolls in hills, the towers stand among broader, higher hills, and the ruins lie on a low plain broken by mesas and gullies. Off the sheer walls no slope is steeper than 1.8 (61 degrees).
- Then rain wears that ground. 200,000 droplets run downhill over the square and 60 m round it, cutting where they run fast and laying down what they carry where they slow, so the hills take branching rills and valleys. Steep ground wears less, as rock. The pads, the roads' shoulders, the ruins' mesas and gullies and the ground just round each don't wear, and worn ground stays within the roads' banks and under the climax and the sightline. The mountains, the coast, the abyss and the features come after it.
- Past the edge, mountains rise round the towers and round the town's three dry sides, their foot wandering up to 60 m out. Their noise comes from the world's seed and its kind, so two kinds of world on one seed, as the town and the towers are, raise different ranges. The town's fourth side is its sea, behind a quay. The ruins run on 16 to 114 m in bays and headlands, then drop sheer into an abyss.
- Rain wears the mountains too, once they stand, but never inside the square, which it leaves exactly as it was. It wears them twice: first on a field four times coarser, as it would wear mountains four times smaller, so that 200,000 droplets cut gullies tens of metres across down their slopes and lay fans at their feet over the broken band of rock there; then on the full field, so that rills run down the gullies' sides. Their steepest faces wear less and the steepest not at all, as bare rock. The field's outer 20 m, the horizon, stays exactly as raised, and neither the town's coast nor the ruins wear. Between 60 and 220 m past the edge the mountains move 3.8 to 4.1 m on average: their foot rises 1 to 1.6 m under the fans and their slopes come down 2 to 6 m. All the wear makes generation four to five times as long as it is unworn: 2.3 s for the town, 2.6 s for the towers and 1.6 s for the ruins in an optimized build, 7 to 10 s in Debug.
- The town has its sea, the towers up to two trenches and three shallow pools, and the ruins one or two pits. Each keeps 25 m from the roads' edges, the pads' blends and the others, and 30 m inside the walls.
- `Core/tests/test_terrain.cpp` checks all of this on the three worlds, that the open ground has no terraces and few flats, that the wear leaves the open ground curving about 1.5 times as sharply as it does unworn (1.25 times at least), and that the mountains' wear moves them 1.5 m on average at least and touches nothing it may not.

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

The ground wears the ground master: the kind's ground surface, rock wherever the ground is steeper than about 0.85 in 1 (40 degrees) and on the mountains' upper sides from 10 m over the square's highest ground (45.6 m in the town and 46.0 m among the towers; nothing in the ruins stands that high), meeting the ground by height so that the stone breaks through first at its high points, and the road surface along every road, drawn from a mask of the roads the build makes from the layout ([SCENE_LOOK.md](SCENE_LOOK.md#the-ground)). The plazas, buildings, roofs, landmarks, trunks and rocks wear the photographed surfaces that `Data/Field/surfaces.json` gives the world's kind, and the rest keep flat tints. The build logs what the ground wears and how many roles are dressed:

| World | Ground | Rock | Road | On a road | Roles dressed |
|---|---|---|---|---|---|
| solhaven | leafy_grass (tinted) | rock_wall | gravel_road | 2.5% | 8 of 8 |
| lattice | gravel_floor_02 (tinted) | worn_rock_natural_01 (tinted) | clean_asphalt | 2.1% | 3 of 8 |
| unwritten | gravelly_sand | worn_rock_natural_01 | dry_mud_field_001 | 1.7% | 3 of 8 |

The tints are the ones surfaces.json gives: the town's grass greener, (0.5, 0.9, 0.35), because the photo's dry grass showed as khaki from any distance; the towers' gravel a touch cooler, (0.85, 0.9, 1.0); and their rock, the ruins' worn rock, grey, (0.45, 0.6, 0.9). The towers first wore a dark rock photo, but even at the tint's cap of 2 it stayed darker than the shadows beside it, and the mountains' steep faces read as holes from afar; the grey worn rock reads lighter than those shadows and darker than the sand.

For Solhaven: `FTBuildExploreWorld: the ground wears /Game/Rendering/M_WorldGround (Ground leafy_grass, Rock rock_wall, Road gravel_road), its roads from a 2048 x 2048 mask (2.5% of the square on a road), rock on the high ground from 45.6 m up.` then `FTBuildExploreWorld: 8 of 8 surface roles wear photographed surfaces.` The mask adds 56 to 72 KB to a map.

The build logs the ground's numbers, then the map's. For Solhaven:

`FTBuildExploreWorld: world solhaven's ground: -33.0 to 145.7 m (pads -19.7 to 35.6 m), 0.0% of the square steeper than 1 in 1; 0 cuts walled in by 0 hidden walls; 1 water planes; 0 props left off steep, wet or cut ground.`

`FTBuildExploreWorld: world solhaven (look photoreal): 16 districts, 19 roads, 269 buildings, 1400 props, 2982 instances, 16 landmarks, 89 actors; bounds 990 x 990 m, 290 m tall; 136 goals reachable from each district.`

| World | Ground | Pads | Steeper than 1 in 1 | Cuts, hidden walls | Water planes | Props left off |
|---|---|---|---|---|---|---|
| solhaven | -33.0 to 145.7 m | -19.7 to 35.6 m | 0.0% | 0, 0 | 1 | 0 |
| lattice | -38.6 to 146.7 m | -12.1 to 36.0 m | 1.0% | 2, 8 | 3 | 25 |
| unwritten | -185.5 to 36.0 m | -18.0 to 36.0 m | 2.8% | 2, 64 | 0 | 78 |

## Headless checks

- Core: `Core/tests/test_world_layout.cpp` covers:
  - the loader
  - determinism and layout invariants for every kind and district count
  - no allocation
  - the committed worlds' median runs
  - the real script's placements (when `Data/Story` is present)
- The tour walks the roads through every district:

  ```
  UnrealEditor-Cmd.exe ForgottenThird.uproject /Game/Maps/World_Solhaven -game -NullRHI -WorldAutoPlay -AlphaFast -WorldQuitOnDone -unattended -nosplash -nosound
  ```

  It logs `World solhaven: reached district <d> (leg i/n) at <t> s game time.` for each leg, then `TOUR DONE`. Solhaven takes 18 legs and 814 s of game time, about 106 s real with `-AlphaFast`; Lattice 15 legs and 803 s, Unwritten 17 legs and 992 s, each with the lead's feet 5 cm over the entry's floor.
- `-WorldDistrict=<n>`, outside a story step, starts the lead on district n's spawn and floor instead of the PlayerStart, and starts a tour there. With `-ExplorePortrait` it frames the ground from that district's pad, such as the climax's hilltop (Solhaven's district 14, Lattice's 1, Unwritten's 4).
- A second after each placing of the lead on a world map (the start of a free walk or the tour, and every story step, field or CG), the director logs its feet against the floor saved for the district it stands in: `World solhaven: the lead's feet at z 5 cm in district 0, whose floor is 0 cm.` The two agree to a few cm when the map's ground is the one the builder imported. The entry's floor is always 0, so the tour's line only shows the ground is there; a story step in another district logs that district's floor.
- The alpha gate (`tools/gate.ps1 -Alpha`) plays the whole story, with Solhaven's field steps on its map.
