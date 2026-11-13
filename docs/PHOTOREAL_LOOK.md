# The per-world photoreal look

Every world shares one photoreal base (docs/SCENE_LOOK.md: the sun, sky, fog, clouds and the camera finish that
`FTScene::SpawnLook` puts in each map). Each world's own feel comes from light, air, the camera finish and one grade
after the tonemapper, never from drawn styles: no ink lines, contours, flat colour or banded shading. This layer
gives each world its own values for all of them.

| World | In light and colour |
|---|---|
| Solhaven | A present-day coastal city with dark sorcery in it: hard grey daylight or wet night, ink-black shadow, desaturated skin and cloth, gold kept as the brightest accent. |
| The Lattice | A present-day city after a catastrophe, with a cold blue game-light glow: ash and dust in the air, a grey world, blue and violet light kept saturated. |
| The Unwritten | A horror void: ruins in fog, ink-black still water, colour drained except blood, a sickly green in the shadows. |

## The pieces

- **`Data/Look/looks.json`**: one entry per world, each a set of `ftcore::SceneLook` fields (`Core/include/ftcore/SceneLook.h`).
  A field a world leaves out keeps the shared value, and every field has a range the importer holds it to
  (`ftcore::importLookTable`, `Core/include/ftcore/LookTable.h`). The doctest tests load the shipped file.
- **`ftcore::SceneLook`**: the shared look's fields plus the camera's tone curve (filmic slope, toe, shoulder, black and
  white clip), auto-exposure speed, local exposure contrast, fringe, grain by tone, and the grade's fields, then
  ground wetness, the water's absorption, scattering, waves and roughness, the fog's albedo, the practical lights'
  colour, intensity and scattering, the glow's colour and strength, the sky's stars, skyglow and flat dome, the wind, the cloud
  layer's cover, kind, height, depth, density, drift and underglow, and the sea's swell, chop, fetch and foam: 91 fields
  in all. Every default reproduces the shared look exactly (no wetness, no glow, no stars or skyglow, no wind and so no
  sea, white fog, the engine's water and light values and its own cloud layer).
- **The `render` block** of `looks.json`: engine console variables the game sets at start, such as
  `r.Lumen.HardwareRayTracing`, as `"name": number` pairs in file order (`ftcore::LookTable::render`). Each key starts
  with `r.`, uses letters, digits, `.` and `_`, and appears once; each value is a finite number. A bad entry refuses
  the whole table, with an error naming `render.<key>`.
- **The grade** (`ftcore::gradeSrgb`, `Core/include/ftcore/LookGrade.h`; on the GPU `Plugins/FTLook`): one full-screen
  pass right after the tonemapper, on the sRGB display values, in OKLab. It scales chroma by one global factor, except
  in two hue windows a world keeps (gold in Solhaven; blue and violet in the Lattice; blood red in the Unwritten), a
  window for the current fight's accent, and a floor for skin; then adds a split tone weighted to the shadows and the
  highlights. Hue is kept everywhere except by the split tone. The shader mirrors the CPU code step for step, so the
  tests on one check the other.
- **`UFTLookSubsystem`** (`Source/ForgottenThird/FTLookSubsystem.h`): in a game world it picks the world, by `-Look=` or
  the map's name (Solhaven, Lattice or Unwritten in it), and once the level's look actors exist writes the look into
  them in place (`FTScene::ApplyLook`): a field is written only where it differs from what's there, so a map built with
  the shared look changes only where its world's look does. It hands the grade its constants.

The maps keep being built with the shared look; the world's look is applied when the map starts.

## Switches

| | |
|---|---|
| `-Look=<world>` | That world's look (solhaven, lattice, unwritten, or off), whatever the map. |
| `-LookOff` | Nothing applied and no grade pass: the scene renders exactly as the shared look does. The "before" of any comparison. |
| `r.FT.Look 0` | As `-LookOff`, read when a map starts. |
| `ft.Look <world\|off>` | Switches the look while playing. With no argument, prints the one on. |
| `r.FT.LookGrade 0` | Turns the grade pass off at run time, leaving the light and camera finish. |

The grade costs no pass when it would change nothing (the shared look, `-LookOff`), and is skipped on HDR output.

## Fights and effects

- A boss camera's own post-process settings (manual exposure, depth of field, fringe, vignette, a slow-motion grade)
  sit above the level's volume and win over the world's camera finish by design. The after-tonemap grade still
  applies on top of them.
- A fight's hot accent is set on the subsystem, not as a post-process field:
  `World->GetSubsystem<UFTLookSubsystem>()->SetFightAccent(HueDegrees, WidthDegrees, Chroma)` keeps that OKLab hue's
  chroma while the world round it is desaturated; `ClearFightAccent()` ends it. An effect colour outside every window
  is desaturated with the world, so pick the accent's hue from the effect's colour.
- Effects are authored in scene colour and pass through the tonemapper and the grade like everything else. Don't use
  a post-process material at `BL_SceneColorAfterTonemapping` for a world's look; that place is the grade's.
- Check an effect in the arena demo both ways: with `-Look=<world>` and with `-LookOff`.

## Comparing before and after

Two off-screen runs with the same seed and delay, one with `-LookOff`:

```
UnrealEditor-Cmd.exe ForgottenThird.uproject /Game/Maps/Demo_Arena -game -RenderOffScreen -ResX=1600 -ResY=900
  -ArenaStage=<zone> -ArenaSeed=7 -ArenaShotDelay=4 -ArenaWorldView -ArenaShot=<dir>/after_<world> -Look=<world>
... the same with -LookOff and -ArenaShot=<dir>/before_<world>
```

`-ArenaWorldView` adds a clean wide view of the scene (`_0_world`) before the fight's views (docs/ARENA_DEMO.md).

## The surfaces and the look lab

The world's wetness, water and glow reach the materials through one material parameter collection,
`/Game/Look/MPC_Look`. `FTScene::ApplyLookParameters` fills it once the world has its instance (again when the world
begins play: the world sets up its collections after its subsystems and swaps a fresh instance, at the defaults, for
the one written during its start), and the subsystem's log line says how many parameters it wrote.
`FTBuildLookMaterials` builds the collection and five surfaces over it, each graph a Custom HLSL node:

| Material | What it shows |
|---|---|
| `M_Look_Water` | Single Layer Water: two panning wave layers that calm toward the shore (the depth behind the surface), the world's absorption and scattering (per metre in the data, per centimetre in the engine) and phase. |
| `M_Look_WetStone` | A photographed stone that soaks, darkens and glosses with `Wetness`, and holds puddles in its dips past `PuddleStart`. |
| `M_Look_Glass` | Lit translucent glass, index of refraction 1.5, with Lumen front-layer reflections (the `render` block turns them on). |
| `M_Look_Chrome` | A near-mirror metal, to read the reflections by. |
| `M_Look_Glow` | The world's glow colour times its strength: the Lattice's game-light. |
| `M_Look_Underwater` | The view from under a water surface, a post-process (`FTBuildLookMaterials -Underwater` builds it alone). Each pixel is cut at the waterline, then fogged and tinted by the same absorption and scattering over the water its ray crosses, dimmed with the eye's depth and wobbled a little; a ray that leaves through the surface sees a bright window overhead and a mirror past the critical angle. |

Water a body can be in is `ftcore::WaterBodies` (`Core/include/ftcore/WaterBodies.h`): a map's canals (the road net's
canal rows) and its sea as footprints with a surface, a bed and a flow, saved on the map's director by the builder.
`UFTWaterSubsystem` reads them: the lead wades (slowed with depth, to 0.7 m), wades deep (to 1.2 m) and swims on the
surface over that, the followers keep to the bank, and while the view's eye is in a body the underwater material is on
(through whichever camera is the view). Its `OnCameraWater` event (under or out, the depth, the body's kind) is the
hook for sound.

ApplyLook also sets the fog's albedo and the practical lights: every light component tagged `FTPractical` takes the
world's practical colour temperature, intensity and volumetric scattering.

The look lab (`/Game/Maps/Look_Lab`, built by `FTBuildLookLabWorld`, game mode `AFTLookLabGameMode`) holds a water
basin with a ramp into it, a wet-stone yard, a glass pane and ball, a chrome slab and ball in an alcove, a glow strip
and one practical lamp in the fog, seen through three cameras. With `-LookLabShot=<prefix>` it takes three stills
(`_1_wide`, `_2_water`, `_3_chrome`) and 192 frames of the water view a 24th of a second apart (`_loop_0000.png` ...)
at a fixed step, then quits:

```
UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTBuildLookMaterials -AllowCommandletRendering -unattended
UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTBuildLookLabWorld -NullRHI -unattended
UnrealEditor-Cmd.exe ForgottenThird.uproject /Game/Maps/Look_Lab -game -RenderOffScreen -ResX=1920 -ResY=1080
  -unattended -nosound -Look=<world> -LookLabShot=<dir>/<world>
```

`-AllowCommandletRendering` lets the material build compile its shaders and report errors; under `-NullRHI` it can't.

## The environment materials

The sky, the clouds and the sea read a second collection, `/Game/Look/MPC_Env`, which
`FTScene::ApplyLookParameters` fills beside `MPC_Look`. `FTBuildEnvMaterials` builds it and the materials over it.
Their shading code lives in the FTLook plugin, under `Plugins/FTLook/Shaders/Private/Env` (the virtual path
`/Plugin/FTLook/Private/Env`), so each material's Custom node is a single call into an include:

| Material | What it shows |
|---|---|
| `Env/M_Env_Sky` | An unlit sky for a sky dome (`Sky.ush`). It draws the atmosphere's own sky and its sun's disc through the engine's sky atmosphere nodes, then adds stars, a city's skyglow and a flat featureless dome on top. The stars come from a real magnitude count, dimmed by air mass and twinkling more near the horizon, and they fade as the sky brightens. The skyglow is a sodium-coloured band toward the city. With the defaults it draws the atmosphere alone. |
| `Env/M_Env_Clouds` | A volume material for the volumetric cloud layer (`Clouds.ush`). A weather map of cover drifts with the wind and sets where cloud is and how tall it grows; `cloudType` runs from flat sheets to towering heaps. The engine's tiling noise volumes give the shape and wear its edges, a cheap bound lets the engine skip empty sky, and the light is a forward lobe for the silver edge, a weaker back lobe, one octave of multiple scattering and a marched shadow. `cloudUnderglow` adds a city's warm glow to the cloud bases. `FTScene::ApplyLook` puts it on the layer when `cloudCover` is above 0, gives the engine's own material back when the cover goes, and moves the layer to `cloudBaseKm` and `cloudThickKm`. |
| `Env/M_Env_Sea` | Single-layer water for open water on a grid mesh (`Waves.ush`). The sea is eight Gerstner waves that `ftcore::seaWaves` (`Core/include/ftcore/SeaWaves.h`) builds from the look: three for a swell of `seaSwellM` at `seaSwellPeriodS` from `seaSwellDeg`, five for the sea `windMps` raises over `seaFetchKm` of water (a fetch-limited spectrum, scaled by `seaChop`). Each keeps deep-water dispersion, the set repeats exactly every 30 minutes, and the waves' shared steepness keeps the surface from ever folding over. They move the vertices (the waves shorter than four grid steps stay out of them), and each pixel takes its exact normal and whitecaps from the same waves at its rest place. The look water's ripples ride on top, its colour and medium fill it, and surf foams a `seaFoamBandCm` band where the water runs shallow (`seaFoam` scales both). `ftcore::seaHeight` gives the height anywhere, for floats and spray. |

```
UnrealEditor-Cmd.exe ForgottenThird.uproject -run=FTBuildEnvMaterials -AllowCommandletRendering -unattended
```

The sea reads `MPC_Look` too, so `FTBuildLookMaterials` runs first, once; this build stops, replacing nothing, if
`MPC_Look` is missing. No map places the sky dome or a sea mesh yet. A sea mesh needs a grid of 100 cm steps (the
material's `GridCm`) and bounds grown by the tallest swell, since the waves move its vertices.

## Later

- Halation (a red-weighted glow round bright edges), per-world wetness on the ground master (`M_WorldGround` reading
  `Wetness` from the collection), the world maps' water bodies wearing `M_Look_Water`, practical lights per district,
  and a night variant (the maps' sun angle is set when they're
  built).
- Measure each map's exposure and narrow the auto-exposure range to about 1.5 EV either side (docs/SCENE_LOOK.md).
