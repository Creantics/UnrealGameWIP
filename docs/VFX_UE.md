# Particle effects in Unreal (Niagara)

How newengine's particle effects (`Data/Vfx/*.json`) run in UE 5.8. Core holds the effect model,
the JSON loader and a plan that maps each effect to Niagara Lightweight (stateless) emitters
(`Core/include/ftcore/Vfx.h`). Unreal only writes that plan into assets. No Niagara asset is made
by hand.

Status (2026-10-18): all 12 systems build, keep their bindings after a reload, and cook for Windows
with 0 errors. Nobody has looked at them in a viewport yet, so sizes, colours and speeds are unchecked.

| Piece | File | Job |
|---|---|---|
| Effect model and loader | `Core/include/ftcore/Vfx.h`, `Core/src/Vfx.cpp` | `VfxEffect`, `importVfxEffect`. Bad input fails with a path and a reason, such as `emitters[0].noise.octaves: expected 1..8`. Unknown keys are warnings. |
| Niagara plan | the same files | `planVfxNiagara` gives Lightweight module values in centimetres, Z up and degrees. Each emitter lists its `gaps`: what the plan drops or approximates. |
| User parameters | the same files | `kVfxUserSpeedScale` and `kVfxUserShapeScale`. `sanitizeVfxUserParameters` clamps each to [0, 10] and turns non-finite values into 1. |
| Tests | `Core/tests/test_vfx.cpp` | Loads all 12 real `Data/Vfx` files and checks the loader errors and the plan. |
| Builder | `Source/ForgottenThird/FTBuildVfxCommandlet.{h,cpp}` | `-run=FTBuildVfx` writes `/Game/VFX/NS_<file>` for every JSON file, plus four sprite materials in `/Game/VFX/Materials`. |
| Runtime | `Source/ForgottenThird/FTVfxComponent.{h,cpp}` | `UFTVfxComponent` plays a named effect and fills its user parameters. `AFTVfxActor` holds one component, for placing effects in a map. |
| Demo map | `FTBuildDemoWorldCommandlet` | Places `ember_swirl`, `heal_ring` and `smoke` beside the character. `-NoVfx` leaves them out. |

## How a system is built

Each system is a copy of the engine template
`/Niagara/DefaultAssets/Templates/Systems/FountainLightweight`, with one Lightweight emitter per
spec emitter. The builder:

- duplicates the template's emitter handle;
- turns off every module except InitializeParticle and SolveVelocitiesAndForces;
- turns on and fills the modules the plan uses;
- sets the sprite renderer and the fixed bounds;
- compiles the system and saves it.

A rerun deletes and rewrites every `NS_*` asset and the sprite materials.

Coordinates: a newengine point (x, y, z) in metres with Y up becomes 100 × (x, z, y) in Unreal.
Swapping two axes mirrors the space, so a turn one way about newengine's Y is a turn the other way
about Unreal's Z.

| Spec field | Niagara Lightweight |
|---|---|
| `duration`, `loop` | EmitterState: loop behaviour and loop duration |
| `rate`, `bursts` | Spawn infos: one Rate, and one Burst per burst (count range, time) |
| `lifetime`, `startSize`, `startRotation`, `startColor` | InitializeParticle: lifetime, sprite size, sprite rotation, colour. Several colour stops become a random pick. |
| `shape` sphere, box, ring, cone | ShapeLocation: sphere, box, or ring with a disc coverage. A cone spawns on its base disc. |
| `speed`, `velocityMin/Max` | AddVelocity. Speed from a point or ring is FromPoint, speed from a cone is InCone (full angle = 2 × the spec's half angle), and a box of velocities is Linear. |
| `gravity`, `drag`, `forceOverLifetime` | GravityForce, Drag, AccelerationForce. A force curve is replaced by its mean. |
| `noise` | CurlNoiseForce: strength and frequency |
| `sizeOverLifetime`, `colorOverLifetime` | ScaleSpriteSize and ScaleColor curves |
| `angularVelocity` | SpriteRotationRate |
| `stretch` / speed scaling | ScaleSpriteSizeBySpeed plus velocity-aligned sprites |
| `renderMode`, `alignment` | Sprite renderer: facing mode, alignment and sort mode |
| `blend`, `sprite` | One of four materials: `M_VfxSoft`, `M_VfxSoftAdditive`, `M_VfxSpark` or `M_VfxSparkAdditive`. They are unlit, procedural round masks with no textures, and take the particle colour and alpha. |
| sub-emitters on death | A burst in the child at the parent's mean lifetime, placed at the parent's expected death position |

### User parameters

Every system exposes two user parameters, and both default to 1:

| Parameter | Type | Bound to |
|---|---|---|
| `User.SpeedScale` | float | the AddVelocity scale (linear, point or cone) |
| `User.ShapeScale` | vector | ShapeLocation's shape scale |

`-run=FTBuildVfx -Verify` reloads the saved systems and lists each emitter's modules and bindings. The
bindings survive the save. `UFTVfxComponent::SetParameters` pushes them through
`UNiagaraComponent::SetVariableFloat` and `SetVariableVec3`. It does this when the effect starts and
when the values change, never per frame.

## What is not mapped

The build log prints every gap as `FTBuildVfx: gap <effect>/<emitter>: ...`. The main ones:

- **Orbits.** `RotateAroundPoint` is a module class in 5.8, but it isn't in the default Lightweight
  template, and the include for it is commented out of the Lightweight simulation shader
  (`NiagaraStatelessSimulationDefault.usf`). So `velocityOverLifetime.orbital` and `orbit` are dropped.
  `heal_ring`, `heal` and `magic_ribbon` spawn in place and don't circle.
- **Trails and ribbons.** Lightweight emitters have no ribbon renderer, so `fireworks` and
  `magic_ribbon` have no trails.
- **Per-particle sub-emitters.** Death bursts fire once per loop, at the expected time and place, not
  at each dying particle. Inherit flags are dropped.
- **Textures.** `flipbook_fire` has no flipbook sheet, the leaf mesh is drawn as sprites, and `puff`
  uses the soft round mask.
- **Smaller losses.** No soft-particle depth fade. No speed limit (`limitVelocity`). No colour or size
  by speed, except the stretch. Noise is one octave and doesn't scroll. A box of start velocities
  keeps only its mean.

Closing most of these needs a full (stateful) Niagara emitter built from the
`/Niagara/DefaultAssets/Templates/Emitters` templates. That means editing Niagara scripts and the
stack graph from code, which is a bigger job than this pass.

## Rebuilding

Run from PowerShell, with no editor open. Never use the interactive editor for these.

```powershell
$ue = 'E:\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$proj = '"D:\projects\unrealgamewip\ForgottenThird.uproject"'

# 1. All systems and the sprite materials. Run again after changing Data/Vfx, the Core plan or the builder.
$p = Start-Process $ue -ArgumentList "$proj -run=FTBuildVfx -NullRHI -unattended -nopause -nosplash -log -abslog=$env:TEMP\ft_vfx.log" -PassThru -NoNewWindow
$null = $p.Handle; $p.WaitForExit(); $p.ExitCode

# 2. Optional: list modules and user-parameter bindings of the saved systems.
$p = Start-Process $ue -ArgumentList "$proj -run=FTBuildVfx -Verify -NullRHI -unattended -nopause -nosplash -log -abslog=$env:TEMP\ft_vfx_verify.log" -PassThru -NoNewWindow
$null = $p.Handle; $p.WaitForExit(); $p.ExitCode

# 3. The demo map, with the effects (add -NoVfx to leave them out).
$p = Start-Process $ue -ArgumentList "$proj -run=FTBuildDemoWorld -Mesh=/Game/Characters/ProofHead/proof_head/SkeletalMeshes/proof_head.proof_head -Style=solhaven -NullRHI -unattended -nopause -nosplash -log -abslog=$env:TEMP\ft_demo.log" -PassThru -NoNewWindow
$null = $p.Handle; $p.WaitForExit(); $p.ExitCode
```

`FTBuildDemoWorld` fails if a placed effect's `NS_*` asset is missing, so run step 1 first.

## Using an effect

```cpp
AFTVfxActor* Fx = World->SpawnActor<AFTVfxActor>(Location, FRotator::ZeroRotator);
Fx->GetVfx()->Effect = TEXT("hit_spark");    // loads /Game/VFX/NS_hit_spark on Play when System is empty
Fx->GetVfx()->SetParameters(1.5f, 0.5f);     // faster, tighter
Fx->GetVfx()->Play();
```

Set `System` directly if the asset has to be cooked: the demo map does this. Otherwise the cook can't
see a system that is only loaded by name. `ReplayInterval` restarts a one-shot effect on a timer. The
three effects in the demo loop, so the map leaves it at 0.
