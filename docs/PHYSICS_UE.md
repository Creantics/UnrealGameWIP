# Physics in Unreal (Chaos)

How newengine's physics plan (physical materials, the ragdoll map and profiles, and the feel maths for
knockback, KO, hit reactions, hit-stop and settling) runs under Chaos in UE 5.8. Core holds the data
model, the JSON loaders and the maths (`Core/include/ftcore/Physics.h`). Unreal only writes the data into
assets and calls the maths at run time. No physics asset is made by hand.

Status (2026-10-18): the six physical materials and the proof_head ragdoll build and reload. The demo
map has a physics corner, and a headless 4-second simulation of it passes (see below). The Windows cook of the demo map has 0 errors and 0 warnings.
Nobody has watched any of it in a viewport yet, so the feel is unchecked.

| Piece | File | Job |
|---|---|---|
| Data model, loaders, maths | `Core/include/ftcore/Physics.h`, `Core/src/Physics.cpp` | Materials, the humanoid ragdoll map, profiles, the bone binding, Chaos joint limits, knockback planning, the KO impulse split, hit-reaction weight, heavy damping, `SettleTracker`, `HitStop`. |
| Tests | `Core/tests/test_physics.cpp` | Loads the real `Data/Physics` files and checks the loaders' errors and the maths. |
| Data | `Data/Physics/materials.json`, `Data/Physics/ragdoll/*.json` | Copied from newengine: six materials, `humanoid_map`, and the `default`, `gentle` and `minor` profiles. |
| Builder | `Source/ForgottenThird/FTBuildPhysicsCommandlet.{h,cpp}` | `-run=FTBuildPhysics` writes `/Game/Physics/PM_<name>` for each material and `/Game/Physics/PHYS_<mesh>_Ragdoll`. |
| Runtime | `Source/ForgottenThird/FTPhysicsComponent.{h,cpp}` | `UFTPhysicsComponent`: knockback, KO, hit reactions, hit-stop, settling, the speed cap. |
| Demo map | `FTBuildDemoWorldCommandlet` | A physics corner of seven simulating props. `-NoPhysics` leaves it out. |
| Check | `Source/ForgottenThird/FTPhysicsSimCommandlet.{h,cpp}` | `-run=FTPhysicsSim` simulates the demo map headless and fails if something falls through or won't settle. |

## Units

newengine works in metres, kilograms and radians with Y up. Unreal works in centimetres and degrees with Z up.

| Quantity | newengine | Unreal | Conversion |
|---|---|---|---|
| Length, position | m | cm | × 100 |
| Speed | m/s | cm/s | × 100 |
| Impulse | N·s (kg·m/s) | kg·cm/s | × 100 |
| Density | kg/m³ | g/cm³ | ÷ 1000 |
| Angle, angular limit | rad | deg | × 180/π |
| Angular speed | rad/s | deg/s (`MaxAngularVelocity`) | × 180/π |
| Mass | kg | kg | none |
| Gravity | 9.81 m/s² | the project's -980 cm/s² | no change made |

## Physical materials

`materials.json` becomes one `UPhysicalMaterial` per entry: `PM_Wood`, `PM_Stone`, `PM_Metal`, `PM_Sack`,
`PM_Rubber` and `PM_Ragdoll`. Friction, static friction, restitution, density and both combine modes are
copied (`default` keeps Unreal's project setting). Each sets `SurfaceType` from `ftcore::PhysSurface`, whose
order matches the six surfaces named in `Config/DefaultEngine.ini`
(`[/Script/Engine.PhysicsSettings]`): SurfaceType1..6 = Wood, Stone, Metal, Cloth, Rubber, Flesh.
`-Verify` reloads every material and compares it with the JSON.

## The ragdoll asset

`FTBuildPhysics` binds `humanoid_map.json` to the mesh's bones (`ftcore::RagdollBinding`) and writes a
separate physics asset, `PHYS_<mesh>_Ragdoll`. The mesh keeps its own physics asset (proof_head keeps
`proof_head_PhysicsAsset`), so the face rig and anything else using that asset is untouched. The
component swaps the ragdoll asset in on KO.

For proof_head, the build makes 12 bodies, 11 joints and 19 no-collide pairs, and the masses total
70 kg. The map's mass fractions sum to 0.99, so `-Verify` reads back 69.3 kg.

- **Bodies.** Each part is one capsule (`FKSphylElem`, along local Z) from its bone to its tip bone, or to
  the map's length when there is no tip. The radius comes from the map, and the length is
  `max(span − 2r, 2 cm)`. A body's mass is the map's fraction of the total, set as a mass override.
  Bodies also get the profile's damping, its `inertiaScale`, its solver iteration counts
  (`positionSteps`, `velocitySteps`) and its `maxAngularVelocity`. They use `PM_Ragdoll`.
- **Joints.** Linear motion is locked. In the constraint frame X runs along the child bone (twist), Z is
  the body's lateral axis (swing1, the hinge axis), and Y is swing2. The lateral axis is the
  right-to-left thigh direction in the reference pose. For proof_head it is (1, 0, 0) in component space.
  Each joint disables collision between its two bodies.
- **Limits.** `ftcore::ragdollChaosLimits` turns each joint's min/max into a symmetric Chaos limit plus an
  offset: a hinge of -2.0..0.05 rad becomes a ±58.7° swing1 with a -55.9° offset. Chaos ignores
  `AngularRotationOffset` (only `UPhysicsConstraintComponent` reads it), so the builder bakes the offset
  into the parent frame (Frame2) as `Rot(Z, swing1 offset) · Rot(X, twist offset)`.
- **Hinge sign (unverified).** Whether a negative hinge angle bends the elbow and knee the natural way
  depends on the sign of the lateral axis. Nobody has looked at it in a viewport. If limbs bend
  backwards, flip `PhysLateral` in the builder.

## UFTPhysicsComponent

Add the component to an actor and set `RagdollAsset` and `Profile` (default `gentle`). The profile is read
once in `BeginPlay`, where buffers are reserved. The component ticks in `TG_PostPhysics` and does not
allocate per frame.

| Feature | Calls | What it does |
|---|---|---|
| Knockback | `Knockback(ImpulseNs, GroundFriction)` | One sphere sweep against WorldStatic along the planned slide. `ftcore::planKnockback` then cuts the impulse so the body stops short of a wall. Returns the slide in metres. |
| KO | `Knockout(ImpulseNs, StruckBone)` | Swaps in the ragdoll asset, simulates every body, and applies `ftcore::splitKoImpulse`: a whole-body velocity change plus a capped push at the struck bone. Refuses when the profile has `useAuthoredKo`. |
| Going heavy | (tick) | After `koHeavyAfter` seconds, bodies slower than `koHeavyBelow` are damped by `heavyDampingFactor`. |
| Settle | (tick) | `ftcore::SettleTracker` puts the ragdoll to sleep once it has been quiet for `settleTime`. |
| Speed cap | (tick) | Chaos has no per-body linear cap, so `maxLinearVelocity` is clamped each tick. The angular cap is written into the asset. |
| Hit react | `HitReact(ImpulseNs, Bone)` | Simulates the bodies below the bone and blends them out with `hitReactionWeight`. The body indices are cached once per reaction, because `SetAllBodiesBelowPhysicsBlendWeight` allocates. |
| Hit-stop | `HitStop(Frames)` | An approximation: the frozen frames zero the bodies' velocities and gravity, then restore both. |

## Demo corner

Unless `-NoPhysics` is given, `FTBuildDemoWorld` places seven simulating props around (-450, 550), each using
one of the materials:

| Prop | Shape | Material |
|---|---|---|
| `Phys_CrateLow`, `Phys_CrateMid`, `Phys_CrateTop` | 50 cm cubes, stacked | `PM_Wood` |
| `Phys_StoneBlock` | cube | `PM_Stone` |
| `Phys_MetalDrum` | cylinder | `PM_Metal` |
| `Phys_Sack` | flat cube | `PM_Sack` |
| `Phys_RubberBall` | sphere, dropped from 150 cm | `PM_Rubber` |

The builder errors if a material is missing ("run FTBuildPhysics first (or pass -NoPhysics)").

## The simulation check

`-run=FTPhysicsSim` loads the demo as a game world with a physics scene, begins play, and ticks at 60 Hz for
`-Seconds` (default 4). It also spawns proof_head away from the props, knocks it out at `spine_03`
with an impulse of (-120, 30, 40) N·s, and calls `HitStop(4)` at frame 30. The check fails when:

- the map has no `Phys_*` props, or a prop isn't simulating;
- a prop's centre goes more than 5 cm below the floor at any point, or the prop is still awake and moving faster than 5 cm/s at the end;
- the ragdoll has no bodies, or a ragdoll body ends more than 15 cm below the floor;
- a ragdoll body is still awake and moving faster than 20 cm/s;
- the KO fails.

The hit-stop is logged ("frozen" or "not frozen" at frame 31) but does not fail the check.

Result on 2026-10-18 (exit 0):

- **Props.** All seven are simulating with their materials and come to rest asleep. The crate stack stays
  standing; the crates settle by 1, 3 and 5 cm.
- **Rubber ball.** Falls from 150 cm, bounces 6 times (first rebound 80.7 cm) and rests at 20 cm.
- **Ragdoll.** The hit-stop froze it at frame 31. After 4 s its 12 bodies rest 4–21 cm above the floor
  with none awake. The settle assist fired once.
- **Speed cap.** An earlier run, in which the floor had no collision body, showed the cap holding falling
  bodies at 500 cm/s.

## Deferred

- **Joint friction and motors.** `jointFriction` and `motor` are loaded and tested in Core but not built.
  The joints have no drives, so there is no active ragdoll or animated pose matching.
- **Hit-stop** is approximate (see above). Chaos has no exact per-body pause.
- **Friction.** Ragdoll bodies use `PM_Ragdoll` (friction 0.9, restitution 0). A profile's own friction and
  restitution are only compared with it, and the build logs when they differ.
- **One profile per asset.** The damping, iterations and angular cap of one profile (`-Profile=`) are baked
  into the asset. Switching profiles at run time changes only what the component does (impulses, settle,
  heavy damping, speed cap, hit reactions).
- **Knockback sweep.** One sphere sweep whose radius is half the target's smallest bounds extent, not a sweep of the real shape.
- **ChaosCloth and GeometryCollection** are not used, so neither plugin is enabled.
- **Feel and the hinge sign** are unchecked in a viewport.

## Rerunning

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildPhysics -NullRHI -unattended -nopause -nosplash
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildPhysics -Verify -NullRHI -unattended -nopause -nosplash
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildDemoWorld -Mesh=/Game/Characters/ProofHead/proof_head/SkeletalMeshes/proof_head.proof_head -Style=solhaven -NullRHI -unattended -nopause -nosplash
UnrealEditor-Cmd ForgottenThird.uproject -run=FTPhysicsSim -NullRHI -unattended -nopause -nosplash
UnrealEditor-Cmd ForgottenThird.uproject -run=Cook -TargetPlatform=Windows -Map=/Game/Maps/Demo -unattended -nopause -nosplash
```

`FTBuildPhysics` options: `-Mesh=<object path>` (default proof_head), `-Profile=<name>` (default `gentle`),
`-NoRagdoll` (materials only), and `-Verify` (reload and compare, without writing). A rerun rewrites every
`PM_*` asset and the ragdoll. Run it before `FTBuildDemoWorld`, which needs the materials.
`FTPhysicsSim` options: `-Seconds=<n>` and `-NoRagdoll`.
