# The scene look

Since 2026-10-28 every map has the same photoreal base. The stylized post-process packs
([STYLE_PACKS_UE.md](STYLE_PACKS_UE.md)) are off unless a map is built with `-Style=<spec>`. Each world
gets its own look later as tuned values on the same base: light, air, camera finish and shaders, not a
drawn style.

## Pieces

| Piece | File | Job |
|---|---|---|
| `ftcore::SceneLook` | `Core/include/ftcore/SceneLook.h`, `Core/src/SceneLook.cpp` | The look as 17 plain values: sun, sky light, fog, clouds, exposure, white balance and grade. `lerp` blends two looks, so a later world-to-world blend is one call. Tests: `Core/tests/test_scene_look.cpp`. |
| `FTScene::SpawnLook` | `Source/ForgottenThird/FTSceneLook.{h,cpp}` | Spawns a look into a map: a movable sun (colour from its temperature), a real-time sky light, the sky atmosphere, exponential height fog (volumetric when the look has it), the engine's volumetric clouds, and an unbound post-process volume at priority 0 for the camera finish. |
| `FTScene::LoadSurface` | same | Loads the surface master that the builders tint for their blocks and props. |
| Surface master | `FTBuildWorldMaterialsCommandlet` | Writes `/Game/Rendering/M_WorldSurface`. See below. |
| `ftcore::SurfaceTable` | `Core/include/ftcore/SurfaceTable.h`, `Core/src/SurfaceTable.cpp` | Loads `Data/Field/surfaces.json`: the photographed surfaces and which one each world kind's shapes wear. Tests: `Core/tests/test_surface_table.cpp`. |
| `FTScene::LoadSurfaceTable`, `FTScene::LoadSurfaceFor` | `Source/ForgottenThird/FTSceneLook.{h,cpp}` | Read the table in the editor and load the surface a shape's role wears. |
| Photographed surfaces | `FTBuildSurfacesCommandlet` | Writes `/Game/Rendering/M_WorldSurfaceTex` and each surface's textures and instance under `/Game/Rendering/Surfaces/<id>/`. See below. |
| Map builders | every `FTBuild*Commandlet` that writes a map, and `FTWorldMapBuilder` | Each one calls `SpawnLook` with its own sun angle, keeps its own local lamps, and tints `M_WorldSurface`. A world map's shapes wear the photographed surfaces instead where the table gives their role one. |

The renderer settings come from `Config/DefaultEngine.ini`: Lumen global illumination and reflections,
Substrate, and the extended auto-exposure range.

## The default look

| Field | Default | Meaning |
|---|---|---|
| `sunLux` | 6 | Sun intensity. The local lamps and the effects' emissive are tuned against a sun this bright under auto exposure, so this is an artistic scale, not daylight lux. A physical-units retune would move every light together. |
| `sunKelvin` | 5800 | Sun temperature: the warmth is in the light. |
| `skyLight` | 1 | Sky light intensity (real-time capture of the atmosphere). |
| `fogDensity`, `fogFalloff` | 0.03, 0.2 | Height fog. |
| `volumetricFog` | 1 | Volumetric fog extinction scale; 0 turns volumetric fog off. |
| `fogForward` | 0.6 | Scattering distribution: light scattered forward, so the air glows towards the sun. |
| `clouds` | 1 | The volumetric cloud layer: on at 0.5 or more. |
| `exposureBias` | 1 | Auto-exposure bias, EV. |
| `exposureMinEV100`, `exposureMaxEV100` | -10, 20 | The engine's extended-range defaults until the scenes are measured. |
| `whiteBalanceKelvin` | 6500 | Neutral camera white balance. |
| `saturation`, `contrast` | 1, 1 | Grade, neutral. |
| `bloom`, `vignette`, `filmGrain` | 0.675, 0.4, 0 | Lens. |

## The surface master

`M_WorldSurface` is an opaque lit material built from code. Substrate converts its classic inputs.
Its parameters, all in the "Surface" group:

- `Color`: albedo. It keeps the basic shape material's parameter name, so tints carry over. The
  shader holds the albedo between 0.04 and 0.8. A brighter tint is scaled down with its hue kept.
- `Roughness` (0.8) and `Metallic` (0).
- `Variation` (0.12) and `VariationScale` (350 cm): two octaves of value noise on the world position.
  They move the albedo by up to `Variation` and the roughness by half that, in patches about a
  `VariationScale` across, so large flat blocks don't read as one flat colour.

It has the instanced static mesh usage, which the world maps' roads, buildings and trees need. The
Explore map's fountain water is a dark tint at roughness 0.05, so it carries the sky's reflection.
The runtime markers (explore spots, field guards, the battle demo's figures) still use the engine's
basic shape material.

## Photographed surfaces

The world maps' ground, plazas, walls, roofs, landmarks, tree trunks and rocks wear photographed
surfaces: colour, normal and ambient occlusion/roughness/metal maps of real ground, stone, plaster, tiles
and bark. They are all CC0 and come from one library. `Data/Field/surfaces.json` lists:

- each surface: its `id` (the library's own name), `tileCm` (the real width one copy of the photo
  covers, from the source page), `res` (1k, 2k or 4k), the source URL and the licence. The loader
  refuses any licence but CC0 and any source that isn't https. An optional `roughness` (0..1) stands in
  for the packed map's roughness when the file's roughness channel is empty. The gravel road's is: without
  it the road rendered as a mirror.
- per world kind (town, towers, ruins), the surface each role wears and an optional `tint`, a
  multiplier on the photo's colour (0 to 2 per channel). Solhaven's walls are tinted warm this way, and its
  grass greener, (0.5, 0.9, 0.35): the photo's grass is dry and showed as khaki from any distance. The towers'
  ground is tinted a touch cooler, (0.85, 0.9, 1.0), and their rock, the ruins' worn rock, grey, (0.45, 0.6,
  0.9). The towers first wore a dark rock photo, but even at the cap of 2 it stayed darker than the shadows
  beside it, and the mountains' steep faces read as holes from afar.

The town kind (Solhaven) dresses all eight roles. The towers and ruins kinds (Lattice and Unwritten) dress
only the ground, the roads and the rock, the roles their ground draws (see [The ground](#the-ground)); their
buildings, plazas, roofs, landmarks and tree trunks keep the flat tints of `M_WorldSurface`. So does any role
the file doesn't list, and any surface that hasn't been built yet, which also logs a warning.

`M_WorldSurfaceTex` projects each photo three ways, along each shape's own axes, and blends the three
by how squarely the face meets each axis. An instanced shape uses its own instance's axes, so:

- a texture keeps its real size however the shape is scaled. A 30 m wall and a 3 m wall show the same
  plaster grain, and nothing reads the meshes' UVs.
- on a side face the photo's top is up, and on a tilted top it runs down the slope, so a pitched roof's
  tiles run down it.
- the normal map is turned onto the same axes, so the tiles and stones catch the light on the side
  facing it.

A world map's ground wears a master of its own, `M_WorldGround` (see [The ground](#the-ground)).

Its parameters, in the "Surface" group:

- `Color` (white): the tint. White keeps the photo's colour.
- `AlbedoTex`, `NormalTex`, `ArmTex`: the three maps. Each surface's instance sets them.
- `TileCm` (200): the real width of one copy, set from `tileCm`.
- `NormalStrength` (1) and `RoughnessScale` (1).
- `RoughnessOverride` (-1): a constant roughness in place of the map's, set from `roughness`. Below zero
  the shader uses the map's roughness times `RoughnessScale`.
- `Variation` (0.12) and `VariationScale` (350 cm): the same mottle as `M_WorldSurface`.
- `MacroVariation` (0.12) and `MacroScale` (2500 cm): a broad mottle that keeps the repeats from lining
  up over open ground.
- `BlendSharpness` (4): how quickly a rounded face hands from one projection to the next.

The albedo is held between 0.03 and 0.8 like the surface master's.

The textures keep their JPEG as their source, so each asset costs about its file's size: 36 textures for
12 surfaces, about 117 MB in all. They are loaded whole rather than streamed, because the streamer judges a
texture's need from the mesh's UVs, which a projection doesn't use. Compressed with their mips, a 2k surface
takes about 11 MB of video memory and a 1k one about 3 MB. A map loads only its own kind's: Solhaven's seven
2k surfaces and one 1k take about 81 MB, Lattice's and Unwritten's three 2k ones about 34 MB each, and each
map's road mask about 6 MB more and its shape mask about 1.4 MB.

One known flaw: the blockout roofs are cubes turned 45° about one axis, so their gable ends show the
tiles turned 45°. The building kit will replace those cubes.

## The ground

A world map's ground is a Landscape ([WORLDS.md](WORLDS.md)). It wears `M_WorldGround`, which blends three
photographed surfaces, the ones surfaces.json gives the world's kind for these roles:

- **ground**: laid flat along the world's X and Y and drawn twice. The second copy is turned 0.6 radians and
  1.31 times the size, and a broad mottle (about 18 m across) chooses between them, so neither copy's repeat
  lines up across open ground.
- **rock**: projected along the world's three axes like `M_WorldSurfaceTex`, wherever the ground is steeper
  than about 0.75 to 0.95 (37 to 44 degrees), with the line wandering by noise. High up it takes gentler
  ground too, by the ground's shape. From `RockAltitudeCm` up, over `RockAltitudeRunCm`, the slope it needs
  falls by 0.4, by `ShapeRock` more on a ridge and as much less in a hollow, and a ridge stands `ShapeLift` of
  the run further into that band than its height alone puts it, a hollow as much less far. So the mountains'
  ridges and upper sides turn to stone first, their gullies keep the ground longest, and only their gentle
  shoulders keep it at the top. Each map sets `RockAltitudeCm` 10 m over the highest ground inside its
  playable square, so this rock stays on the mountains round it: from 45.6 m in the town and 46.0 m among the
  towers. Nothing in the ruins stands that high, so their rock keeps to the steep ground.
- **road**: laid flat inside each road's edge, 3 m from its centre line give or take 40 cm by noise, and
  softened over `RoadEdgeCm`.

The road comes over the rock, and the rock over the ground. Each layer takes its tint and its roughness, and
then all three take the same mottles and a drift between warm and cool over broad patches.

The rock and the ground meet by height, not by a straight fade. Each surface's occlusion map, taken about its
own mean (read from its last mip), stands for its height, up to `RockBlendHeight`; wherever one stands
`RockBlendDepth` clear of the other, it alone shows. So where the slope starts to call for rock, the stone
breaks through at its high points first and the ground keeps to its cracks and hollows longest. Centring
each map on its own mean keeps a bright gravel meeting a dark rock as evenly as two of a kind, and the height
is held under 0.95 less the depth, so the ends of the blend stay pure ground and pure rock.

Far off, the detail that would tile or glitter gives way, from `FarStartCm` (30 m from the eye) to
`FarEndCm` (120 m). A third copy of the ground, `FarScale` (5) times the size and turned 1.9 radians, takes
`FarBlend` (0.6) of its colour and packed maps, so the near copies' repeats don't line up into a pattern seen
from high up. A second copy of the rock, `RockFarScale` (15) times its size, does the same for the rock, so
the mountains keep the photo's cracks and ledges, grown to their size, instead of fading to its mean colour.
Its bumps are left out. The bumps ease to `FarNormalStrength` (0.4) of their strength, and the fine mottle
fades out, leaving the broad mottles and the drift.

The roads come from a mask. `ftcore::roadMask` fills one byte for each half metre of the playable square: 255
within a metre of a road's centre line, falling in a straight line to 0 five metres out, so a filtered read
gives back the distance to the nearest road. The outermost ring is 0, and the mask is clamped, so no road shows
past the square. The builder keeps it in the map's package as `T_RoadMask`: 2048 x 2048, uncompressed, loaded
whole, and stored as a PNG of 56 to 72 KB.

The ground's shape comes from a second mask. At each point `ftcore::shapeMask` measures how far the ground
stands over two rings round it, 3 and 8 samples out (6 and 16 m), per metre of each ring's radius, and averages
the two. The byte is 128 where the ground is flat or slopes evenly, such as a pad's core; more on ridges,
crests and knolls; less in gullies, hollows and valleys; and 255 (or 0) where the ground stands half a metre
over (or under) its rings for each metre out. The builder keeps it in the map's package as `T_ShapeMask`:
1024 x 1024 over the whole field, about 1.5 m a pixel, so a ridge or a gully keeps its shape. It adds 0.3 to
0.6 MB to a map. The master alone wears `T_GroundFlatShape`, which reads 128 everywhere.

`ftcore::flowMask` makes a third mask, of where the rain gathers. It resamples the ground's `flow()` (see
[WORLDS.md](WORLDS.md#ground)) over the whole field, a byte a pixel: 0 on the pads and the roads, low on the
crests and the open slopes, rising along the gullies and the valleys' floors, and 255 where a hectare's rain
runs through, which almost nothing reaches. Neither the builder nor the master uses it yet; it is meant to make
the ground richer and darker where the water gathers and paler on the crests.

Each map gets one instance of the master, `MI_World_Ground`, in its own package. It carries the three
surfaces' textures, tints, tile sizes and roughness from surfaces.json, the two masks and their widths, and
`HueDrift`: 0.08 in the town, 0.02 among the towers and 0.05 in the ruins. When the master or one of the three
surfaces hasn't been built, the builder warns and the ground wears its ground surface alone through
`M_WorldSurfaceTex` (or the flat tint), with no rock and no roads.

Its parameters:

- `GroundAlbedo`, `GroundNormal` and `GroundArm`, and the same for `Rock` and `Road`: the maps. The defaults
  are the town's surfaces.
- `GroundTint` (white), `GroundTileCm` (200) and `GroundRoughness` (-1), and the same for `Rock` and `Road`.
  A roughness below zero keeps the packed map's.
- `RoadMask` (the empty `T_GroundNoRoads`, so the master alone draws no road), `MaskSideCm` (100000, the
  mask's width) and `RoadEdgeCm` (35).
- `RockBlendHeight` (0.5), `RockBlendDepth` (0.2), `RockAltitudeCm` (100000, over any ground, so the master
  alone puts no rock on the high ground) and `RockAltitudeRunCm` (4000).
- `ShapeMask` (the flat `T_GroundFlatShape`), `ShapeSideCm` (150000, the field's width), `ShapeLift` (0.5)
  and `ShapeRock` (0.4).
- `FarStartCm` (3000), `FarEndCm` (12000), `FarScale` (5), `RockFarScale` (15), `FarBlend` (0.6) and
  `FarNormalStrength` (0.4).
- `NormalStrength` (1), `Variation` (0.12), `VariationScale` (400 cm), `MacroVariation` (0.14),
  `MacroScale` (3000 cm) and `HueDrift` (0.05).

The albedo is held between 0.03 and 0.8. Every texture is read through one of the view's shared samplers,
which filter as the world's textures do. Ten textures with a sampler each, plus the Landscape's own, would
pass the sixteen samplers a shader may bind.

How it looks, from offscreen renders at the entry, close behind the lead, 25 m up and from 60 m up and
120 m back, and at eye height on each world's climax (`-WorldDistrict`, [WORLDS.md](WORLDS.md)):

- Solhaven's grass reads a lime green rather than a lawn's. From a road the gravel and the plazas' cobbles
  show at their real size, and the far view shows the roads running between the districts. From afar the
  grass no longer shows its tile's grain, though it stays one even green. From the hilltop the plaza looks
  out through the open half of its ring on the town's other districts, the sea and the mountains. The
  mountains read as grey stone worn into ridges and gullies, with the grass climbing the gullies in tongues
  and filling the slopes' feet, and their rock keeps a grain from 300 m instead of one flat colour.
- Lattice's ground is pale, cool gravel, and its asphalt roads read from the far view. From the hilltop the
  plaza looks out through the open half of its ring over the towers to the mountains. Their ridges and
  spurs wear grey-brown stone with a grain of its own, and the pale gravel fills the valleys and the gullies
  between them.
- Unwritten's sand shows no repeat, and none of the speckle it used to, from 25 m up or from afar. Its mud
  roads stand out against it, its worn open ground rolls in soft folds, and its mesas and a pit's funnel
  show. From afar the mesas' faces keep the rock's grain, streaked from top to foot, instead of one flat tan.
  The plain keeps one even tone to the horizon. On the climax the plaza fills the butte's flat top, so even
  with its ring open the plain stays hidden below the rim, and only the far landmarks show over it.

Not yet: nothing grows on the ground, so from afar the town's grass is one even green and the ruins' plain one
tone, and the trees and rocks are still simple shapes. Each pad is a flat disc out to the district's radius
(62 m), so from a climax's plaza the ground below stays hidden under the pad's rim even where the ring is open;
to look down on the world, the pad would have to fall away from the plaza's edge on that side. The mountains
wear one rock photo throughout, the quay runs straight, and the sea is a still, flat plane with no waves or
foam where it meets the quay. The ground doesn't read where the rain gathers yet, so along a gully or a
valley's floor it wears the same tint as on the slopes round it.

## Building

Build the materials first, then the maps:

```
UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTBuildWorldMaterials -NullRHI -unattended -nopause -nosplash
python -I tools/fetch_surfaces.py --out <dir>
UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTBuildSurfaces -Source=<dir> -AllowCommandletRendering -unattended -nopause -nosplash
UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTBuildGround -AllowCommandletRendering -unattended -nopause -nosplash
```

`FTBuildWorldMaterials` replaces its file, and saves nothing if the material reports compile errors. A map builder that
needs the material logs `/Game/Rendering/M_WorldSurface is missing; run FTBuildWorldMaterials first.`
when it hasn't been built.

`fetch_surfaces.py` downloads the photos into `<dir>/<id>/`. It keeps a file that's already there with
the right checksum, and refuses any source that isn't CC0. The photos aren't committed; the built
textures are, through LFS. `FTBuildSurfaces` checks that every file is there before it changes anything,
replaces the master and every surface's assets, and saves nothing if the master doesn't compile.
`-AllowCommandletRendering` without `-NullRHI` makes that compile check real. It ends with a line like
`FTBuildSurfaces: saved 12 surfaces (36 textures, 12 instances and /Game/Rendering/M_WorldSurfaceTex) in <size> MB.`

`-Only=<id>,<id>,...` rebuilds just those surfaces and keeps the others' assets as they are. The master keeps
its graph, but its default textures follow the table's first surface, so it's saved only when they move. Building
the ground's surfaces this way moved the master's defaults to leafy_grass:
`FTBuildSurfaces: saved 7 surfaces (21 textures, 7 instances and /Game/Rendering/M_WorldSurfaceTex) in 69.9 MB; the others are as they were.`
When the first surface stays put, the master isn't saved:
`FTBuildSurfaces: saved 2 surfaces (6 textures, 2 instances) in 21.6 MB; the others are as they were.`

`FTBuildGround` replaces `M_WorldGround`, `T_GroundNoRoads` and `T_GroundFlatShape`. It reads the town's
ground, rock and road surfaces from surfaces.json for the master's defaults, and saves nothing if one of their
textures is missing or the master doesn't compile. It ends with
`FTBuildGround: saved /Game/Rendering/M_WorldGround, /Game/Rendering/T_GroundNoRoads and /Game/Rendering/T_GroundFlatShape (56.7 KB), wearing leafy_grass, worn_rock_natural_01 and gravel_road until a map's instance says otherwise.`
Each world map's build then makes the map's instance and masks, and logs, for example,
`FTBuildExploreWorld: the ground wears /Game/Rendering/M_WorldGround (Ground leafy_grass, Rock worn_rock_natural_01, Road gravel_road), its roads from a 2048 x 2048 mask (2.5% of the square on a road), rock on the high ground from 45.6 m up, keeping to the ridges by a 1024 x 1024 shape mask.`

Before it changes anything, it also reads the roughness channel of each surface's packed map. A channel that
averages under 0.05 is empty. No surface here is that smooth all over, and the shape would render as a mirror.
So the build refuses an empty channel unless the surface has a `roughness` in surfaces.json, and it refuses a
`roughness` on a map whose channel isn't empty, which would hide the map once its file is fixed. The gravel
road's channel is empty, so the log says
`FTBuildSurfaces: gravel_road's arm map has an empty roughness channel; the surface takes 0.95 from Data/Field/surfaces.json.`

That check misses errors in the ray-tracing hit shaders. If one of those fails, the game draws every shape that wears
the master in the engine's grey default material, and its log says
`Failed to compile Material /Game/Rendering/M_WorldSurfaceTex.M_WorldSurfaceTex for platform PCD3D_SM6`. The same goes
for `M_WorldGround`, whose failure turns the whole ground grey. So after any change to either master's code, look for
that line in the first game run's log, and for `too many samplers`, which would mean the ground master's textures
took samplers of their own again. The code samples through the engine's `Texture2DSample` for this reason: a plain
`Tex.Sample` needs derivatives, which a hit shader doesn't have.

Without `-Style=` the builders log the look as photoreal, for example
`FTBuildExploreWorld: world solhaven (look photoreal): ...` and `FTBuildDemoWorld: photoreal look, no style controller.`
On a world map the director logs `World solhaven: field step in district 3 (photoreal look).`

## Later

- Measure each map's EV100 and narrow the exposure range to about ±1.5 EV round it.
- One `SceneLook` per world, blended with `lerp` where the story crosses from one to another.
- Photographed surfaces for the towers' and ruins' buildings, plazas, roofs, landmarks and trees.
- On the terrain: scattered grass, stones and decals that hide the repeats; a second ground surface per
  world, such as dirt or moss in patches; and a mask from the erosion that lays rock or scree along the
  rills and gullies the rain cut, rather than by slope and height alone.
- Cliff meshes, or a fog pass, on the sheer walls (the quay, the abyss's fall and the pits' shafts), which
  the Landscape can only draw as long, thin triangles 2 m apart.
- A building kit in place of the blockout cubes, which also fixes the roofs' gable ends.
