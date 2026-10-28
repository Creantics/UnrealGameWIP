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
| Map builders | every `FTBuild*Commandlet` that writes a map, and `FTWorldMapBuilder` | Each one calls `SpawnLook` with its own sun angle, keeps its own local lamps, and tints `M_WorldSurface`. |

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

## Building

Build the material first, then the maps:

```
UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTBuildWorldMaterials -NullRHI -unattended -nopause -nosplash
```

It replaces the file, and saves nothing if the material reports compile errors. A map builder that
needs the material logs `/Game/Rendering/M_WorldSurface is missing; run FTBuildWorldMaterials first.`
when it hasn't been built.

Without `-Style=` the builders log the look as photoreal, for example
`FTBuildExploreWorld: world solhaven (look photoreal): ...` and `FTBuildDemoWorld: photoreal look, no style controller.`
On a world map the director logs `World solhaven: field step in district 3 (photoreal look).`

## Later

- Measure each map's EV100 and narrow the exposure range to about ±1.5 EV round it.
- One `SceneLook` per world, blended with `lerp` where the story crosses from one to another.
- Real surfaces in place of the blockout tints.
