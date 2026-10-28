# World style packs in Unreal

> **Off by default since 2026-10-28.** The maps now share one photoreal look
> ([SCENE_LOOK.md](SCENE_LOOK.md)). A builder adds the style controller below only when it's given
> `-Style=<spec>` (and not `-Style=none`). The packs, the material and the controller still build and
> work. The worlds will get their own looks later, as tuned light, air, camera and shader values on the
> photoreal base.

How the style packs from newengine (`docs/reference/style_packs.md`) run in UE 5.8. The rules live in
Core (`Core/include/ftcore/StylePack.h`, ported from newengine's `StylePack.h`): the four packs, the
Ink Solhaven preset, `lerp`, the Style Dial and `parseStyleSpec`. Unreal only carries the values to
the screen.

| Piece | File | Job |
|---|---|---|
| `ftcore::StyleSettings` and friends | `Core/include/ftcore/StylePack.h`, `Core/src/StylePack.cpp` | Pack values, blends, the dial, spec parsing. Thirteen doctest cases. |
| Parameter mapping | `Source/ForgottenThird/FTStyleParameters.{h,cpp}` | `FTStyle::VisitParameters`: one call per material parameter, with unit conversion. The commandlet and the controller both use it, so they always agree. |
| Shader | `Source/ForgottenThird/FTStyleShader.inl` | The HLSL body of the Custom node. A port of newengine's tonemap STYLE stage, plus a screen-space hatching. |
| Material builder | `FTBuildStyleMaterialsCommandlet` | Writes `/Game/Rendering/PP_Style` and the instances `MI_Style_Solhaven`, `_Lattice`, `_Unwritten`, `_Restored` and `_InkSolhaven`. |
| Runtime | `AFTStyleController` | Owns an unbound post-process component with a dynamic instance of `PP_Style`. `SetStyle`, `BlendTo`, `SetPack`, `BlendToPack`, `SetStyleSpec`, `BlendToSpec`, `SetDial`. |
| Demo map | `FTBuildDemoWorldCommandlet` | Adds an unbound post-process volume, the controller and a few BasicShapes props. `-Style=` picks the style. |

## Rebuilding and switching packs

Run from PowerShell. Never use the interactive editor for these.

```powershell
$ue = 'E:\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$proj = '"D:\projects\unrealgamewip\ForgottenThird.uproject"'

# 1. The material and the pack instances. Run again after changing FTStyleShader.inl,
#    FTStyleParameters.cpp or a pack's values in Core. It replaces the six assets.
$p = Start-Process $ue -ArgumentList "$proj -run=FTBuildStyleMaterials -NullRHI -unattended -nopause -nosplash -log -abslog=$env:TEMP\ft_style.log" -PassThru -NoNewWindow
$null = $p.Handle; $p.WaitForExit(); $p.ExitCode

# 2. The demo map with a style.
$p = Start-Process $ue -ArgumentList "$proj -run=FTBuildDemoWorld -Mesh=/Game/Characters/ProofHead/proof_head/SkeletalMeshes/proof_head.proof_head -Style=solhaven -NullRHI -unattended -nopause -nosplash -log -abslog=$env:TEMP\ft_demo.log" -PassThru -NoNewWindow
$null = $p.Handle; $p.WaitForExit(); $p.ExitCode

# 3. One map per pack: -Map=<name> writes /Game/Maps/<name> instead of /Game/Maps/Demo.
foreach ($pack in 'solhaven','lattice','unwritten','restored') {
  $name = 'Demo_' + $pack.Substring(0,1).ToUpper() + $pack.Substring(1)
  $p = Start-Process $ue -ArgumentList "$proj -run=FTBuildDemoWorld -Map=$name -Mesh=/Game/Characters/ProofHead/proof_head/SkeletalMeshes/proof_head.proof_head -Style=$pack -NullRHI -unattended -nopause -nosplash -log -abslog=$env:TEMPt_$name.log" -PassThru -NoNewWindow
  $null = $p.Handle; $p.WaitForExit(); "$name $($p.ExitCode)"
}
```

The maps are `Demo_Solhaven`, `Demo_Lattice`, `Demo_Unwritten` and `Demo_Restored`. They are rebuilt
outputs, so rerun step 3 after changing the demo commandlet rather than editing them by hand.

`-Style=` takes anything `ftcore::parseStyleSpec` takes:

- **Packs:** `none`, `solhaven`, `lattice`, `unwritten`, `restored`. Without `-Style=` a builder adds no controller.
- **The preset:** `inksolhaven`.
- **A dial:** `S3,U0.5` or `3,0,0.5,0` (needles 0..3 in S, L, U, R order). Commas are kept. A spec with
  spaces needs quotes.

A spec that doesn't parse makes the commandlet exit 1 before it touches the map. If `PP_Style` is
missing, the commandlet also exits 1 and asks you to run `FTBuildStyleMaterials` first.

For a named pack or `inksolhaven`, the commandlet also puts that pack's static instance on the
controller's post-process component, so the editor viewport shows the style without playing. A dial
has no static instance, so it only shows in play. The same applies to `none`.

At run time, call `SetPack` or `BlendToPack(Pack, Seconds)` on the `StyleController` actor (from
Blueprint or C++), `SetDial(S, L, U, R, Seconds)`, or `BlendTo(ftcore::StyleSettings, Seconds)` from
C++.

In play, the console command `ft.Style <spec>` sets every `StyleController` in the world to that
spec, for example `ft.Style lattice` or `ft.Style S3,U1`. It takes the same specs as `-Style=`.

Hatching is off for now, until the shader pass: every pack's `Hatch` reaches the material as 0. Launch
with `-StyleHatch` to see it again. The packs still carry their hatch values.

Launching the game with `-StyleOff` turns every controller's post-process off at BeginPlay, so the scene
renders with Unreal's plain physically based look (default finish, no hatching or ink). Use it to judge
characters and props under natural light, for example in portrait captures.

## Blendable location: after tonemapping

`PP_Style` uses `BL_SceneColorAfterTonemapping`. The choice follows from where the stage sits in
newengine: inside the tonemap pass, after bloom and the grade, working on display-space colour.

- **The pack values are display-space sRGB.** That covers the tints, paper, ink and outline colours,
  and the tone thresholds in the hatching and mono remap. After tonemapping, they apply as authored,
  with no conversion. Before tonemapping, every threshold would shift with exposure.
- **Bloom is already in the image**, so the paper grain shows through colour blooms, as the spec asks
  (NDD 8.15.4).
- **The cost:** in UE, vignette, film grain and chromatic aberration run in the tonemapper, so they are
  already in the input. newengine applies them after the style stage. The finish multipliers therefore
  can't be material parameters. `AFTStyleController` writes them to its `FPostProcessSettings`
  instead (see the mapping below). The grain and vignette get styled along with the scene rather than
  laid on top of it. On these packs that barely shows, because UE's default grain is 0.
- **The UI** is drawn after post-processing, so it never looks printed, as in newengine.

## Where the effect lives

| Option | Verdict |
|---|---|
| Unbound post-process component on `AFTStyleController` (used) | One actor holds the style for the level. Priority 10, above the level's own unbound `PostProcessVolume` (priority 0, no overrides), so the level volume stays free for scene grading. |
| Camera post-process settings | Ties the style to a pawn. It would have to move with possession and cutscene cameras. Not used. |

The controller's `BeginPlay`:

1. Parses `StyleSpec`.
2. Removes the static instance from the component.
3. Creates a dynamic instance of `StyleMaterial` and adds it as the only blendable.
4. Writes every parameter once.

Later writes update existing parameter entries. A blend ticks `ftcore::lerp` on plain
`StyleSettings` values, and the controller ticks only while a blend runs. So nothing in the
per-frame path allocates. Unreal's own render-command queue is outside our control.

## Field to parameter mapping

Material parameters are in the group "Style". Units convert from newengine's metres with Y up to
Unreal's centimetres with Z up: (x, y, z) m becomes (x, z, y) × 100 cm. The values go to the
material instance unless the "Goes to" column says otherwise.

| StyleSettings field | Material parameter | Goes to / notes |
|---|---|---|
| `outline`, `outlineWidth`, `outlineThreshold`, `outlineTaper`, `outlineJitter` | same names, capitalised | Width is in pixels at 1080p. Jitter redraws at 8 fps from `Time`. |
| `outlineColor` | `OutlineColor` | sRGB |
| `celSteps` | `CelSteps` | Used only for the hatching's shadow-only mask (see Deferred) |
| `saturation`, `shadowTint`, `highlightTint` | `Saturation`, `ShadowTint`, `HighlightTint` | |
| `blackPoint` | `BlackPoint` | Display values below it clip to black and the rest re-stretch. 0 in every pack (see below). |
| `monochrome`, `monoGamma`, `paperColor`, `inkColor` | `Monochrome`, `MonoGamma`, `PaperColor`, `InkColor` | |
| `halftone`, `halftonePitch` | `Halftone`, `HalftonePitch` | |
| `paperSky`, `paperGrain` | `PaperSky`, `PaperGrain` | Sky is scene depth ≥ `SkyDepth` (group "Scene", 60000 cm) |
| `island`, `islandEdge`, `islandInk` | `Island`, `IslandEdge`, `IslandInk` | |
| `islandCenter` | `IslandCenter` | Position converted to cm with Z up. The disc is tested in the XY ground plane. |
| `islandRadius` | `IslandRadius` | × 100 (cm) |
| `hatch`, `hatchScale`, `hatchDensity`, `hatchInk`, `hatchOverlay`, `hatchMaxLevel`, `hatchShadowOnly`, `hatchCurve` | same names, capitalised | Screen-space hatching in post |
| `inkVoid` | `InkVoid` | |
| `inkVoidNear`, `inkVoidFar` | `InkVoidNear`, `InkVoidFar` | × 100 (cm) |
| `rimScale`, `rimTint` | `RimScale`, `RimTint` | Post approximation of the pack rim (below) |
| `bloomScale` | none | Controller: `BloomIntensity = BaseBloomIntensity (0.675) × bloomScale` |
| `bloomThresholdScale` | none | Controller: `BloomThreshold = BaseBloomThreshold × scale`. Only applied when the base is ≥ 0, and UE's default is −1, so it is off by default. |
| `vignetteScale` | none | Controller: `VignetteIntensity = 0.4 × scale` |
| `filmGrainScale` | none | Controller: `FilmGrainIntensity = BaseFilmGrainIntensity (0) × scale` |
| `chromaScale` | none | Controller: `SceneFringeIntensity = BaseFringeIntensity (0) × scale` |
| `cel`, `hull*`, `face*` | none | Deferred (below) |

Pipeline order inside the shader, the same as newengine:

1. Tint and saturation.
2. Black point.
3. Pack rim.
4. Hatching.
5. Mono remap with halftone.
6. Paper sky.
7. Ink void.
8. Island.
9. Outline.
10. Paper tooth.

The rim and the black point are Unreal-side steps: in newengine the rim is part of the lit colour, so it
reaches the style stage already in the image, and the black point is an optional extra (below).

Edges use the second difference of 1/depth over a 3×3 ring, as in newengine. The shader reads
depth through `SceneTextureLookup(…, PPI_SceneDepth)`, and the SceneTexture nodes wired into the
Custom node make the material bind the scene textures it reads (colour, depth and the GBuffer world
normal). A `CameraVectorWS` node feeds the rim's view direction.

## Art direction engine deltas

Art direction v2 section 7 lists the engine work behind the v2 pack values. Where each item stands here:

| # | Item | Fields | Unreal |
|---|---|---|---|
| – | Parameter-only values | Solhaven saturation; the Lattice's outline, cel, bands, shadow tint, bloom, vignette and hull; the Unwritten's tile, gamma and density | In `stylePack()`. The cel and hull values wait for the deferred lit-pass and geometry work. |
| 1 | Hatch overlay mode | `hatchOverlay` | Post hatching: strokes over the graded colour instead of a paper-bright base. |
| 2 | Hatch level cap | `hatchMaxLevel` | Post hatching: the level is clamped before the stroke lookup. |
| 3 | Shadow-band mask | `hatchShadowOnly` | Post hatching. Approximated by display tone below `0.8 / CelSteps` (no light buffer in post). |
| 4 | Hatch tone curve | `hatchCurve` | Post hatching and `hatchWeights()`. Tested: with density 1.4 and curve 1.8 the cross begins at tone 0.50 and is full at 0.375. |
| 5 | Pack rim | `rimScale`, `rimTint` | Post approximation (below). |
| 6 | Bloom threshold multiplier | `bloomThresholdScale` | Controller, on `BloomThreshold`. Off until `BaseBloomThreshold` is set to 0 or more. |
| 7 | Ink void | `inkVoid`, `inkVoidNear`, `inkVoidFar` | Post: depth fades to ink, and the sky goes to ink too. 0 in every pack; a scene sets it. |
| 8 | Core-shadow ink (optional) | – | Not ported. It is for the environment only, which needs the character/environment mask that post lacks. |
| 9 | Black-point clip (optional) | `blackPoint` | Post, right after the tints. 0 in every pack: v2 proposes 0.06 for the Lattice only if its shadow tint muddies the darks, which needs a look at the picture first. |
| 10, 11 | Per-draw override, object-space strokes | – | Not ported. They belong to the lit pass and the hull, which are deferred. |

Every new field defaults to the identity, so `lerp` and the dial cover it and the Restored pack, which
sets none of them, draws as before.

### The rim in post

newengine adds `(1 − N·V)³ × rimScale × linear(rimTint)` to the lit colour of skinned meshes, before
bloom and tonemapping. The post version:

- reads `N` from the GBuffer world normal and `V` from `CameraVectorWS`, and adds
  `(1 − N·V)³ × RimScale × RimTint` to the display-space colour;
- masks it to silhouettes: the rim only shows where a depth sample 4 px away (at 1080p) is at least 4–15 %
  farther than the pixel, so a floor or wall seen edge-on does not glow;
- applies to every foreground surface, not only the cast, since post has no mesh type. Custom depth isn't
  used as a cast mask because the explore spots already use it for focus;
- runs before the hatching, so strokes thin out along the rim, and after bloom, so the rim never blooms.
  That is what the Lattice wants anyway (a cyan rim kept below the bloom threshold);
- is skipped in the base-pass permutation UE also compiles for post materials.

## Deferred, and the plan for each

| Item | Why it isn't in post | Plan |
|---|---|---|
| **Cel bands** (`cel`, `celSteps`) | They quantise `N·L × shadow` in the lit shader. Banding tone in post would posterise textures, which the spec rules out. | A material function for the character and world master materials that bands the light term. Alternatively, a Substrate or custom shading model once the master materials exist. Feed `cel` and `celSteps` through a Material Parameter Collection the controller writes. |
| **Lit hatching** (tonal art map in the lit pass) | Post only sees display tone, not the light amount. The post version is a screen-space stand-in. Its `hatchShadowOnly` mask is display tone below `0.8 / CelSteps`, not the real lowest cel band. | Port newengine's procedural TAM generator to a Core function or a texture-building commandlet. Sample it in the master materials from the banded light term, keyed by the same MPC. Drop the post hatching when that lands. |
| **Inverted-hull silhouettes** (`hull`, `hullWidth`, `hullColor`, `hullStatic`) | It is a geometry pass. | A second material slot or overlay material on skeletal meshes: a front-face-culled shell pushed out along normals to a constant pixel width, with the colour from the MPC. Static meshes opt in with `hullStatic`. |
| **Pack rim, lit version** (`rimScale`, `rimTint`) | Post has a stand-in (above). The real term is scene-referred, in the lit pass, on skinned meshes only. | A Fresnel term in the character master material, from the MPC. Drop the post rim when it lands. |
| **Face fields** (`face*`) | They apply to materials with a face role, inside the lit hatching. | Comes with lit hatching. `faceSnap` belongs to `UFTFaceComponent`. |
| **Full colour-authority buffer** (8.15.5) | v1 is one disc, as in newengine. | A render-target mask (gold, violet, rose, blank channels) written by emitters and sampled here in place of the disc test. |
| **Static instance for dial specs** | A dial has no saved instance. | Only needed if the editor viewport must show a dial without playing. The commandlet could save an `MI_Style_Custom`. |

## Verification status

- **Gate:** `tools/gate.ps1 -Ubt` passes: 139 doctest cases, ctest 3/3, and UBT "Result: Succeeded".
- **Commandlets:** under `-NullRHI`, `FTBuildStyleMaterials` and `FTBuildDemoWorld -Style=…` exit 0.
  A bad `-Style=` exits 1.
- **Cook:** the Windows cook of `/Game/Maps/Demo` reports 0 errors and 0 warnings.
  - The first cook failed. UE also compiles a base-pass pixel shader for a post-process material, and
    `PostProcessOutput_ViewportSizeInverse` doesn't exist there.
  - The shader now falls back to `View.ViewSizeAndInvSize` outside `POST_PROCESS_MATERIAL`.
- **Not yet built:** the black point and the post rim (new `SceneNormalIn` and `CameraVec` inputs)
  haven't been through UBT or `FTBuildStyleMaterials` yet. Rerun the commandlet before judging them.
- **Not yet seen:** nobody has looked at the picture. Everything above ran without a renderer, so the
  look of each pack still needs an eye in the editor or a packaged build. Things to check:
  - outline threshold and width against UE's depth precision;
  - hatching density at 1080p;
  - that the sky reads as sky (`SkyDepth`);
  - the rim's silhouette mask: rim width and whether props pick up too much of it.
