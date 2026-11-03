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
  white clip), auto-exposure speed, local exposure contrast, fringe, grain by tone, and the grade's fields. Every
  default reproduces the shared look exactly.
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

## Later

- Halation (a red-weighted glow round bright edges), per-world wetness on the ground master's flow mask, water
  shading per world, practical lights per district, and a night variant (the maps' sun angle is set when they're
  built).
- Measure each map's exposure and narrow the auto-exposure range to about 1.5 EV either side (docs/SCENE_LOOK.md).
