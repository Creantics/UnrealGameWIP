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

The world maps' ground, roads, plazas, walls, roofs, landmarks, tree trunks and rocks wear photographed
surfaces: colour, normal and ambient occlusion/roughness/metal maps of real ground, stone, plaster, tiles
and bark. They are all CC0 and come from one library. `Data/Field/surfaces.json` lists:

- each surface: its `id` (the library's own name), `tileCm` (the real width one copy of the photo
  covers, from the source page), `res` (1k, 2k or 4k), the source URL and the licence. The loader
  refuses any licence but CC0 and any source that isn't https. An optional `roughness` (0..1) stands in
  for the packed map's roughness when the file's roughness channel is empty. The gravel road's is: without
  it the road rendered as a mirror.
- per world kind (town, towers, ruins), the surface each role wears and an optional `tint`, a
  multiplier on the photo's colour. Solhaven's walls are tinted warm this way.

Only the town kind (Solhaven) is dressed so far. Lattice and Unwritten keep the flat tints of
`M_WorldSurface`. So does any role the file doesn't list, and any surface that hasn't been built yet,
which also logs a warning.

`M_WorldSurfaceTex` projects each photo three ways, along each shape's own axes, and blends the three
by how squarely the face meets each axis. An instanced shape uses its own instance's axes, so:

- a texture keeps its real size however the shape is scaled. A 30 m wall and a 3 m wall show the same
  plaster grain, and nothing reads the meshes' UVs.
- on a side face the photo's top is up, and on a tilted top it runs down the slope, so a pitched roof's
  tiles run down it.
- the normal map is turned onto the same axes, so the tiles and stones catch the light on the side
  facing it.

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

The textures keep their JPEG as their source, so each asset costs about its file's size: 24 textures,
62.0 MB in all. They are loaded whole rather than streamed, because the streamer judges a texture's
need from the mesh's UVs, which a projection doesn't use. Compressed with their mips, the six 2k surfaces and
the two 1k ones take about 70 MB of video memory.

One known flaw: the blockout roofs are cubes turned 45° about one axis, so their gable ends show the
tiles turned 45°. The building kit will replace those cubes.

## Building

Build the materials first, then the maps:

```
UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTBuildWorldMaterials -NullRHI -unattended -nopause -nosplash
python -I tools/fetch_surfaces.py --out <dir>
UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTBuildSurfaces -Source=<dir> -AllowCommandletRendering -unattended -nopause -nosplash
```

`FTBuildWorldMaterials` replaces its file, and saves nothing if the material reports compile errors. A map builder that
needs the material logs `/Game/Rendering/M_WorldSurface is missing; run FTBuildWorldMaterials first.`
when it hasn't been built.

`fetch_surfaces.py` downloads the photos into `<dir>/<id>/`. It keeps a file that's already there with
the right checksum, and refuses any source that isn't CC0. The photos aren't committed; the built
textures are, through LFS. `FTBuildSurfaces` checks that every file is there before it changes anything,
replaces the master and every surface's assets, and saves nothing if the master doesn't compile.
`-AllowCommandletRendering` without `-NullRHI` makes that compile check real. It ends with
`FTBuildSurfaces: saved 8 surfaces (24 textures, 8 instances and /Game/Rendering/M_WorldSurfaceTex) in 62.0 MB.`

Before it changes anything, it also reads the roughness channel of each surface's packed map. A channel that
averages under 0.05 is empty. No surface here is that smooth all over, and the shape would render as a mirror.
So the build refuses an empty channel unless the surface has a `roughness` in surfaces.json, and it refuses a
`roughness` on a map whose channel isn't empty, which would hide the map once its file is fixed. The gravel
road's channel is empty, so the log says
`FTBuildSurfaces: gravel_road's arm map has an empty roughness channel; the surface takes 0.95 from Data/Field/surfaces.json.`

That check misses errors in the ray-tracing hit shaders. If one of those fails, the game draws every shape that wears
the master in the engine's grey default material, and its log says
`Failed to compile Material /Game/Rendering/M_WorldSurfaceTex.M_WorldSurfaceTex for platform PCD3D_SM6`. So after any
change to the master's code, look for that line in the first game run's log. The code samples through the engine's
`Texture2DSample` for this reason: a plain `Tex.Sample` needs derivatives, which a hit shader doesn't have.

Without `-Style=` the builders log the look as photoreal, for example
`FTBuildExploreWorld: world solhaven (look photoreal): ...` and `FTBuildDemoWorld: photoreal look, no style controller.`
On a world map the director logs `World solhaven: field step in district 3 (photoreal look).`

## Later

- Measure each map's EV100 and narrow the exposure range to about ±1.5 EV round it.
- One `SceneLook` per world, blended with `lerp` where the story crosses from one to another.
- Photographed surfaces for the towers and ruins kinds.
- Terrain in place of the flat floor, with scattered grass, stones and decals that hide the repeats.
- A building kit in place of the blockout cubes, which also fixes the roofs' gable ends.
