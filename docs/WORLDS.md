# The three big worlds

The story's field steps play in three big worlds:

- **Solhaven**: a town. Its Ink-Solhaven and Restored scenes play here under those style packs.
- **Lattice**: towers.
- **Unwritten**: ruins.

Scenes that aren't placed in a world, such as the house and the pocket, still play on the small Explore map.

The **overworld** joins them: a small walkable map with one gate per world. When the story moves from one world to another, the party walks the road between the two gates before the next field step starts.

Each world is a blockout for now: shapes, roads, landmarks and the world's style pack. It has no terrain art, streaming or biomes yet.

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

Each row of `scenes` places one scene in a district of a world. It can also set a `style` that overrides the world's own. A test checks that every scene of a world named in `story` is placed, and that no house or pocket scene is.

`ftcore::generateWorldLayout` turns a world entry into a layout. It is deterministic in the seed, does no allocation after load, and fails with a reason when the districts don't fit. The map builder and the game both call it, so they always agree. The layout contains:
- district centres placed best-candidate, and roads that form a tree plus a few shortcuts, none crossing another district
- each district's frame
- the buildings, trees and rocks

Nothing stands on a road or in a plaza.

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
   - starts the player at the district's spawn
   - switches the style controller to the scene's style
   - runs the field step as usual

   Log: `World solhaven: field step in district 3 (style inksolhaven).`

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

The style is world 0's unless `-Style=<spec>` is given. The default build logs `overworld (style solhaven): 3 worlds, 3 roads, 18 emblem blocks, 256 props, 433 instances, 38 actors; bounds 482 x 482 m, 48 m tall.` The tour walks every gate along the roads:

```
UnrealEditor-Cmd.exe ForgottenThird.uproject /Game/Maps/Overworld -game -NullRHI -OverworldAutoPlay -AlphaFast -WorldQuitOnDone -unattended -nosplash -nosound
```

It logs `Overworld: reached the <id> gate (leg i/n) at <t> s game time.` for each leg, then `OVERWORLD TOUR DONE`. On the default layout that's all 3 gates in 2 legs, 117.5 s game time (16 s real on the 8x clock). Core covers the layout in `Core/tests/test_overworld.cpp`: the loader, determinism and clearances for 1 to 8 worlds, no allocation, the open rule, and the real script's hops (when `Data/Story` is present).

## Building the maps

The maps are generated; don't edit them by hand.

```
UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTBuildExploreWorld -World=solhaven -NullRHI -unattended -nopause -nosplash
```

Run it again with `-World=lattice` and `-World=unwritten`. `-Style=<spec>` overrides the world's style. Each run writes `/Game/Maps/<map>` and contains:
- a floor and perimeter walls
- roads, plazas, buildings, trees and rocks as instanced meshes on a few holder actors (about 90 actors in all)
- the landmarks and beacons
- one large anchor shape floating 260 m over the centre
- lights, the director and the style controller
- a navmesh per district

The build fails if any field goal, from `kFieldGoals` or `goals.json`, has no full navmesh path from any district's spawn. The roads between districts have no navmesh, because they are straight and clear.

The last line logged reads like this:

`FTBuildExploreWorld: world solhaven (style solhaven): 16 districts, 19 roads, 269 buildings, 1400 props, 3001 instances, 16 landmarks, 88 actors; bounds 1002 x 1002 m, 290 m tall; 136 goals reachable from each district.`

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

  It logs `World solhaven: reached district <d> (leg i/n) at <t> s game time.` for each leg, then `TOUR DONE`. Solhaven takes 18 legs and 1108 s of game time, about 140 s real with `-AlphaFast`.
- The alpha gate (`tools/gate.ps1 -Alpha`) plays the whole story, with Solhaven's field steps on its map.
