# Particles v2: a Shuriken-style particle system for NewEngine

**Status:** research, gap analysis and implementation plan. No code has changed yet.
**Scope:** `ne::vfx` (CPU simulation, `ParticleRenderer`, the JSON loader), `games/examples/vfx_demo`, the
battle demo's effects, and a particle inspector for `tools/level_editor`.
**Reference:** the Unity Manual's Particle System module pages (Unity 6). Every module section below cites
its page.

---

## 0. Summary

Today `ne::vfx` is a small, clean, deterministic CPU system. It has PCG32 seeding, a fixed SoA pool per emitter,
zero allocations per frame, five spawn shapes, linear over-life curves and gradients, and four procedural
sprites. It covers roughly Unity's Main, Emission and Shape basics plus Color/Size over Lifetime. It has none of
the following: the MinMaxCurve value model (each value can be a constant, a curve, or random between two constants
or two curves), velocity, force and limit modules, noise, texture-sheet animation, sub-emitters, trails, collision,
lights, render-mode options, or an editor.

The plan:

1. **Architecture first.** Replace `Range` and `Curve` with one `MinMaxCurve` and add a `MinMaxGradient`, keeping
   source and JSON compatibility. Then turn `update()` into one SoA pass per *enabled* module.
2. **P1:** the value model, the motion modules (velocity, force, limit, inherit, noise), the by-speed modules,
   texture sheets, the loader, tests and vfx_demo effects. All of this stays in `engine/*/vfx`, tests and
   vfx_demo.
3. **P2:** render modes, sorting, trails, sub-emitters, plane collision, lights and the style-pack
   treatment.
4. **P3:** an ImGui particle inspector with curve and gradient widgets, a live preview, and saving to JSON.

Constraints that hold in every phase:

- zero heap allocations per frame;
- PCG32 determinism;
- planes are the only colliders;
- particle lights live within the forward renderer's cap of 8 point lights;
- particles must read correctly in all four world style packs.

---

## 1. Research: Unity's Particle System, module by module

All URLs are `https://docs.unity3d.com/Manual/<page>`. Properties are listed with what they do. **Bold**
marks the properties we will build. The rest are noted for completeness.

### 1.1 The value model: MinMaxCurve and MinMaxGradient

- **MinMaxCurve** ([ScriptReference/ParticleSystem.MinMaxCurve](https://docs.unity3d.com/ScriptReference/ParticleSystem.MinMaxCurve.html),
  [ParticleSystemCurveMode](https://docs.unity3d.com/ScriptReference/ParticleSystemCurveMode.html)). Four modes:
  - **Constant**: one value.
  - **Curve**: one curve.
  - **TwoConstants**: random between two constants.
  - **TwoCurves**: random between two curves.

  Fields: `constant`, `constantMin/Max`, `curve`, `curveMin/Max`, and `curveMultiplier`. The curve is authored in
  the range −1..1 and scaled by the multiplier.

  The curve's x axis depends on the property:
  - for "start" values in the Main module, x is the emitter's cycle time (0..Duration);
  - for "over lifetime" values, x is normalised particle age;
  - for "by speed" values, x is speed remapped through a Speed Range.

  In the random modes, the random factor is fixed per particle, so a particle stays on "its" curve between
  the two.
- **MinMaxGradient** ([ScriptReference/ParticleSystem.MinMaxGradient](https://docs.unity3d.com/ScriptReference/ParticleSystem.MinMaxGradient.html),
  [ParticleSystemGradientMode](https://docs.unity3d.com/ScriptReference/ParticleSystemGradientMode.html)).
  Five modes:
  - **Color**;
  - **Gradient**;
  - **TwoColors**;
  - **TwoGradients**;
  - **RandomColor**: picks a colour from a gradient's keys.

  Unity gradients keep colour keys and alpha keys separately.

### 1.2 Modules

| Module (page) | Properties and behaviour |
|---|---|
| **Main** ([PartSysMainModule](https://docs.unity3d.com/Manual/PartSysMainModule.html)) | **Duration**, **Looping**, **Prewarm** (start as if one cycle had already run; looping only), **Start Delay**, **Start Lifetime / Speed / Size** (each a MinMaxCurve), **3D Start Size**, **Start Rotation**, **3D Start Rotation**, **Flip Rotation** (a share of particles spin the other way), **Start Color** (a MinMaxGradient), **Gravity Modifier** (scales project gravity), **Simulation Space** (Local / World / Custom transform), **Simulation Speed** (one multiplier on the whole system's dt), Delta Time (Scaled / Unscaled time), **Scaling Mode** (Hierarchy = all parent scale; Local = own scale only; Shape = scale moves spawn positions but not particle size), **Play on Awake**, **Emitter Velocity** (Rigidbody or Transform delta; feeds Inherit Velocity and rate over distance), **Max Particles**, **Auto Random Seed / Random Seed**, **Stop Action** (None / Disable / Destroy / Callback when the system finishes), **Culling Mode** (Automatic: looping systems pause offscreen; Pause and Catch-up; Pause; Always Simulate), Ring Buffer Mode (expired particles are kept and recycled when the pool is full). |
| **Emission** ([PartSysEmissionModule](https://docs.unity3d.com/Manual/PartSysEmissionModule.html)) | **Rate over Time** (MinMaxCurve over Duration), **Rate over Distance** (particles per unit the emitter moves), **Bursts**: Time, Count (MinMaxCurve), **Cycles**, **Interval**, **Probability**. |
| **Shape** ([PartSysShapeModule](https://docs.unity3d.com/Manual/PartSysShapeModule.html)) | Shapes: **Sphere, Hemisphere, Cone, Donut, Box, Circle, Edge, Rectangle**, Mesh, MeshRenderer, SkinnedMeshRenderer, Sprite, SpriteRenderer. Properties: **Radius**, **Donut Radius**, **Angle**, **Length**, **Emit from** (cone: Base / Volume; box: Volume / Shell / Edge), **Radius Thickness** (0 = surface, 1 = whole volume), **Arc**, **Arc Mode** (Random / Loop / Ping-Pong / Burst Spread), **Spread** (snap to discrete arc steps), **Arc Speed**, **Position / Rotation / Scale** of the shape, **Align to Direction**, **Randomize Direction**, **Spherize Direction**, **Randomize Position**. Also mesh Type / Normal Offset / Use Mesh Colors and a Texture used to tint or discard particles. |
| **Velocity over Lifetime** ([PartSysVelOverLifeModule](https://docs.unity3d.com/Manual/PartSysVelOverLifeModule.html)) | **Linear XYZ** plus **Space** (Local / World), **Orbital XYZ** (angular speed about each axis), **Offset XYZ** (the orbit centre), **Radial** (push away from or pull toward the centre), **Speed Modifier** (scales speed along the current direction). All are MinMaxCurves over age. |
| **Limit Velocity over Lifetime** ([PartSysLimitVelOverLifeModule](https://docs.unity3d.com/Manual/PartSysLimitVelOverLifeModule.html)) | **Separate Axes**, **Speed** (the limit, a curve over age), **Space**, **Dampen** (the fraction of the excess removed each frame), **Drag** (linear drag), **Multiply by Size**, **Multiply by Velocity** (quadratic drag). |
| **Inherit Velocity** ([PartSysInheritVelocity](https://docs.unity3d.com/Manual/PartSysInheritVelocity.html)) | **Mode**: **Initial** (the emitter velocity is added once, at birth) or **Current** (the live emitter velocity is applied every frame). **Multiplier** (a curve over age). |
| **Lifetime by Emitter Speed** ([PartSysLifetimeByEmitterSpeedModule](https://docs.unity3d.com/Manual/PartSysLifetimeByEmitterSpeedModule.html)) | **Multiplier** on the start lifetime, sampled at the emitter speed remapped through **Speed Range**. The constant modes ignore the range. |
| **Force over Lifetime** ([PartSysForceOverLifeModule](https://docs.unity3d.com/Manual/PartSysForceOverLifeModule.html)) | **XYZ** acceleration curves, **Space**, and **Randomize** (in the random modes, a new value is picked every frame instead of a fixed per-particle one, which gives jitter). |
| **Color over Lifetime** ([PartSysColorOverLifeModule](https://docs.unity3d.com/Manual/PartSysColorOverLifeModule.html)) | **Color**: a gradient over age, multiplied with Start Color. |
| **Color by Speed** ([PartSysColorBySpeedModule](https://docs.unity3d.com/Manual/PartSysColorBySpeedModule.html)) | **Color** gradient over **Speed Range**. Speeds outside the range clamp to the end colours. |
| **Size over Lifetime** ([PartSysSizeOverLifeModule](https://docs.unity3d.com/Manual/PartSysSizeOverLifeModule.html)) | **Separate Axes** and **Size** (a curve over age). |
| **Size by Speed** ([PartSysSizeBySpeedModule](https://docs.unity3d.com/Manual/PartSysSizeBySpeedModule.html)) | **Separate Axes**, **Size** curve and **Speed Range**. |
| **Rotation over Lifetime** ([PartSysRotOverLifeModule](https://docs.unity3d.com/Manual/PartSysRotOverLifeModule.html)) | **Separate Axes** and **Angular Velocity** in degrees per second. |
| **Rotation by Speed** ([PartSysRotBySpeedModule](https://docs.unity3d.com/Manual/PartSysRotBySpeedModule.html)) | **Separate Axes**, **Angular Velocity** and **Speed Range**. |
| **External Forces** ([PartSysExtForceModule](https://docs.unity3d.com/Manual/PartSysExtForceModule.html)) | **Multiplier** on wind-zone forces, and an **Influence Filter** (a layer mask or an explicit list of Force Fields). |
| **Noise** ([PartSysNoiseModule](https://docs.unity3d.com/Manual/PartSysNoiseModule.html)) | **Separate Axes**, **Strength** (a curve over age), **Frequency** (low = smooth and slow turns, high = abrupt ones), **Scroll Speed** (moves the field over time), **Damping** (strength proportional to frequency, so the field can be rescaled without changing its character), **Octaves**, **Octave Multiplier** (strength per extra octave), **Octave Scale** (frequency per extra octave), **Quality** ([enum](https://docs.unity3d.com/ScriptReference/ParticleSystemNoiseQuality.html): Low = 1D, Medium = 2D, High = 3D; lower is much cheaper), **Remap** plus **Remap Curve** (reshape the final −1..1 noise value), and **Position / Rotation / Size Amount** (how much the noise drives each channel). |
| **Collision** ([PartSysCollisionModule](https://docs.unity3d.com/Manual/PartSysCollisionModule.html)) | Type **Planes** (a list of transforms) or World (the physics scene, with Quality High / Medium / Low through a voxel cache, Collides With, Max Collision Shapes, Dynamic Colliders, Voxel Size, Collider Force). Shared: **Dampen** (speed lost), **Bounce** (speed kept along the normal), **Lifetime Loss**, **Min / Max Kill Speed**, **Radius Scale**, Send Collision Messages, Visualize Bounds. |
| **Triggers** ([PartSysTriggersModule](https://docs.unity3d.com/Manual/PartSysTriggersModule.html)) | **Inside / Outside / Enter / Exit**, each Ignore / Kill / Callback. Also Collider Query Mode, Radius Scale and Visualize Bounds. |
| **Sub Emitters** ([PartSysSubEmitModule](https://docs.unity3d.com/Manual/PartSysSubEmitModule.html)) | Per entry: a child system, a **condition** and **Inherit** flags ([enum](https://docs.unity3d.com/ScriptReference/ParticleSystemSubEmitterProperties.html): Color tints, Size scales, Rotation adds, Lifetime shortens the child as the parent nears death, Duration), plus **Emit Probability**. Conditions: **Birth** (emits throughout the parent's life, so a trail of children), **Collision** (on each parent collision), **Death** (when the parent expires), **Trigger** (on trigger-volume interaction), **Manual** (only when script calls `TriggerSubEmitter`). Birth children use the full Emission module; the event conditions use a single burst. |
| **Texture Sheet Animation** ([PartSysTexSheetAnimModule](https://docs.unity3d.com/Manual/PartSysTexSheetAnimModule.html)) | Mode **Grid** (Tiles X×Y) or Sprites. **Animation**: Whole Sheet or Single Row, with **Row Mode** Custom / Random / Mesh Index. **Time Mode**: **Lifetime** (Frame over Time curve) / **Speed** (frame from a speed range) / **FPS**. **Start Frame**, **Cycles**, Affected UV Channels. |
| **Lights** ([PartSysLightsModule](https://docs.unity3d.com/Manual/PartSysLightsModule.html)) | Light prefab, **Ratio** (the share of particles that get a light), **Random Distribution** (random, or every Nth particle), **Use Particle Color**, **Size Affects Range**, **Alpha Affects Intensity**, **Range / Intensity Multiplier** (curves), **Maximum Lights** (the manual's performance advice: cap it). |
| **Trails** ([PartSysTrailsModule](https://docs.unity3d.com/Manual/PartSysTrailsModule.html)) | Mode **Particles** (each particle leaves a trail) or **Ribbon** (connects particles in age order; Ribbon Count, Split Sub Emitter Ribbons). Also **Ratio**, **Lifetime**, **Minimum Vertex Distance**, **World Space**, **Die with Particles**, **Texture Mode** (Stretch / Tile / Repeat / Distribute per segment), **Size affects Width / Lifetime**, **Inherit Particle Color**, **Color over Lifetime**, **Width over Trail**, **Color over Trail**, Generate Lighting Data, Shadow Bias. |
| **Custom Data** ([PartSysCustomDataModule](https://docs.unity3d.com/Manual/PartSysCustomDataModule.html)) | Two streams. Each is Disabled, Vector (1–4 MinMaxCurve components) or Color (a MinMaxGradient). They reach custom shaders through Custom Vertex Streams. |
| **Renderer** ([PartSysRendererModule](https://docs.unity3d.com/Manual/PartSysRendererModule.html)) | **Render Mode**: **Billboard** (faces the Render Alignment); **Stretched Billboard** (Camera Scale, **Velocity Scale**, **Length Scale**, Freeform Stretching, Rotate with Stretch); **Horizontal Billboard** (flat on XZ); **Vertical Billboard** (upright on Y, turns to face the camera); **Mesh**; **None** (trails only). **Sort Mode**: **None / By Distance / Oldest in Front / Youngest in Front / By Depth** (camera-plane depth). **Sorting Fudge** (system-level bias), **Min / Max Particle Size** (as a fraction of the viewport), **Render Alignment**: **View / World / Local / Facing / Velocity**. **Flip**, Allow Roll, **Pivot** (in multiples of size), Normal Direction, Material / **Trail Material**, Custom Vertex Streams, shadows, motion vectors, sorting layer / order, probes. |

### 1.3 Inspector UX

- **Modules as foldouts.** The component is a column of collapsible modules. Each has an enable checkbox in its
  header, and a disabled module is greyed out and costs nothing
  ([PartSysUsage](https://docs.unity3d.com/Manual/PartSysUsage.html)).
- **Value fields** carry a small dropdown arrow that switches between the four MinMaxCurve modes. A
  Curve or Two Curves field shows a thumbnail. Clicking it opens the **Particle System Curves** editor at the
  bottom of the Inspector. That editor has a y-axis scale field (the multiplier), preset shapes, and draggable
  keys with tangents. Two Curves shades the band between the curves
  ([Curve editor, Unity 3.5 manual](https://docs.unity3d.com/352/Documentation/Manual/ParticleSystemCurveEditor.html)).
- **Colours** open the **Gradient Editor**: colour keys under the bar, alpha keys above it, Blend or Fixed
  interpolation, presets
  ([Gradient editor, Unity 3.5 manual](https://docs.unity3d.com/351/Documentation/Manual/GradientEditor.html)).
- **The Scene-view "Particle Effect" overlay**, shown while a system is selected, has:
  - **Play / Pause**, **Restart**, **Stop**;
  - **Playback Speed**;
  - **Playback Time**, which can be scrubbed by dragging its label;
  - **Particles**, the live count.

  ([PartSysUsage](https://docs.unity3d.com/Manual/PartSysUsage.html)). The overlay also has these toggles
  ([class-ParticleSystem](https://docs.unity3d.com/Manual/class-ParticleSystem.html)):
  - **Simulate Layers**: preview unselected systems;
  - **Resimulate**: edits re-run the existing particles;
  - **Show Bounds**;
  - **Show Only Selected**.

### 1.4 Out of scope: the VFX Graph

Unity's Visual Effect Graph is a separate, node-based **GPU** system. It needs compute shaders and SSBOs
([System Requirements](https://docs.unity3d.com/Packages/com.unity.visualeffectgraph@17.0/manual/System-Requirements.html)).
We are leaving it out for three reasons:

- **No compute.** Our window asks for an OpenGL **3.3** core context (`engine/src/platform/Window.cpp:45-46`) and
  every shader is `#version 330 core`, so there are no compute shaders.
- **No determinism.** GPU simulation gives up CPU determinism, which our tests depend on.
- **We don't need the scale.** Our effects are hundreds to tens of thousands of particles, which a CPU SoA
  simulation handles.

A GPU path would mean a GL 4.3 or Vulkan backend first.

### 1.5 Open-source references

- **Curve and gradient widgets for Dear ImGui.** [CedricGuillemet/ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo)
  (MIT) ships `src/ImCurveEdit.h/.cpp` and `src/ImGradient.h/.cpp`.
  - ImCurveEdit: a `Delegate` interface (point count, points, bounds, curve type
    discrete / linear / smooth / bezier, edit / add hooks) plus `ImCurveEdit::Edit(delegate, size, id)`.
  - ImGradient: a `Delegate` plus `ImGradient::Edit(delegate, size, selection)`.

  Both are draw-list widgets of about 300–700 lines. We pin **ImGui v1.92.9** (`cmake/Dependencies.cmake`).
  The plan is to *write our own* widgets of about 400 lines in the same delegate style rather than add a
  dependency. The editor already writes custom draw-list widgets (the gizmo, `EditorUi.cpp:270`).
- **CPU particle architecture.** Bartłomiej Filipek's "Flexible particle system" series:
  - [start](https://cppstories.com/2014/04/flexible-particle-system-start);
  - [the SoA container](https://www.cppstories.com/2014/04/flexible-particle-system-container/);
  - [summary](https://www.cppstories.com/2015/04/flexible-particle-system-summary/).

  The series covers SoA pools, swap-remove kill, separate **generators** (spawn modules) and **updaters**
  (one pass each), and the SIMD gains. That is the shape proposed in §3.3.
- **Effekseer** ([github.com/effekseer/Effekseer](https://github.com/effekseer/Effekseer), MIT) is a complete
  open-source effect editor and runtime. It is a useful UX reference for a standalone tool, but too big to embed.

---

## 2. Gap analysis

### 2.1 What we have

- `Particles.h/.cpp` (273 lines):
  - `Rng` (PCG32);
  - `Range{min,max}`;
  - piecewise-linear `Curve{keys}` and `Gradient{stops}`;
  - `EmitterDesc`, a flat struct of about 35 fields;
  - `ParticleEmitter`: a fixed SoA pool (`pos_, vel_` as `vector<vec3>`; `age_, life_, size_, rot_, angVel_,
    seed_` as `vector<float>`), swap-remove kill, a sub-frame-accurate rate and bursts, `gather()` producing
    world-space `ParticleInstance`s;
  - `ParticleSystem`: a bundle of emitters.

  `update()` is one loop that does gravity, drag and orbit for every particle.
- `EffectLoader` reads JSON through `data::Reader`, which gives path-aware diagnostics. Range fields accept a
  number or `[min,max]`. `sizeOverLife:[{t,v}]` and `colorOverLife:[{t,color}]` are supported.
- `ParticleRenderer` (412 lines):
  - instanced quads, with the quad corner taken from `gl_VertexID`;
  - procedural sprites Soft / Puff / Spark;
  - velocity stretch, soft particles and HDR linearisation;
  - batches keyed by look;
  - alpha batches sorted back to front with `std::sort`;
  - additive batches unsorted.

  Reservations: 16 batches × 2048 instances (`kReservedBatches`, `kReservedInstances`). The VBO grows on demand.
- `ParticleComponent`: ECS follow-transform and `destroyWhenFinished`.
- Users:
  - `vfx_demo`: fountain, swirl, smoke and a pool of 4 spark entities;
  - `battle_demo` (`BattleGame.cpp:194-259`): 8 pooled spark systems and embers. Both **build `EmitterDesc` in
    C++**.
- The four `assets/vfx/*.json` files (heal, hit_spark, perfect_burst, smoke) are loaded **only by tests**:
  - `test_particles.cpp:371`;
  - the mutation fuzz in `test_loader_robustness.cpp:408`.
- There is no `docs/vfx*.md`; the only docs are `docs/vfx_demo.png` and `STATUS.md`'s "Particles / VFX: Done".

### 2.2 Module → current field → gap → priority → cost

Cost: **S** < 1 day, **M** 1–3 days, **L** > 3 days.

| Unity module / property | What we have now (field) | Gap | Pri | Cost |
|---|---|---|---|---|
| MinMaxCurve (4 modes) | `Range` (two constants), `Curve` (one linear curve, over life only) | No curve-over-duration for start values, no two-curves mode, no multiplier, no per-particle curve factor | **P1** | M |
| MinMaxGradient (5 modes) | `startColor` (vec4), `Gradient` | No random two colours / two gradients / random key; start colour is not random | **P1** | S |
| Main: duration, loop, max particles | `duration`, `loop`, `maxParticles` | — | — | — |
| Main: start lifetime / speed / size / rotation | `lifetime`, `speed`, `startSize`, `startRotation` (Range) | MinMaxCurve over cycle time; 3D size and 3D rotation | P1 (MMC) / P3 (3D) | S |
| Main: flip rotation | — | Fraction of particles with negated rotation and angular velocity | P1 | S |
| Main: gravity modifier | `gravity` (vec3 acceleration) | Keep the vec3; add `gravityModifier` MinMaxCurve over duration | P1 | S |
| Main: simulation space | `space` World / Local | Custom space (a transform pointer) | P3 | S |
| Main: simulation speed | — | `simulationSpeed` multiplier on dt | **P1** | S |
| Main: start delay, prewarm | — (vfx_demo and battle warm up by hand with 180 / 240 `update()` calls) | `startDelay`; `prewarm` run at `play()` with fixed substeps | **P1** | S |
| Main: scaling mode | Transform scale used "as-is" (`ParticleComponent.h`) | Hierarchy / Local / Shape | P2 | S |
| Main: stop action | `ParticleComponent::destroyWhenFinished` | `stopAction` None / Disable / Destroy / Callback (a function pointer, not `std::function`) | P2 | S |
| Main: culling | — | Bounds per system, `cullingMode`; the renderer skips culled systems | P2 | M |
| Main: emitter velocity | — | Track the transform delta in `setTransform` (with a teleport flag) | **P1** | S |
| Main: random seed | Explicit seed at construction; replays reuse the RNG stream | `reseed(seed)` on `play()` so pooled hits vary deterministically | P1 | S |
| Main: ring buffer mode | Spawns are dropped when full | Optional recycle-oldest | P3 | S |
| Emission: rate over time, bursts | `rate` (float), `bursts{time,count}` | Rate as a MinMaxCurve over duration; bursts with count MinMaxCurve, cycles, interval, probability | **P1** | S |
| Emission: rate over distance | — | Needs emitter velocity | **P1** | S |
| Shape: sphere, cone, box, ring, point | `shape`, `radius`, `innerRadius` (ring only), `coneAngle`, `boxHalfExtents` | Hemisphere, circle, edge, rectangle, donut; `radiusThickness` for every round shape; box shell / edge; cone length and volume | P1 (cheap ones) / P2 | M |
| Shape: arc, arc mode, spread | — | Arc with random / loop / ping-pong / burst spread | P2 | S |
| Shape: transform, align / randomize / spherize direction, randomize position | — | Shape TRS; three direction blends; position jitter | **P1** | S |
| Shape: mesh / skinned / sprite | — | Emit from `MeshData` triangles | P3 | M |
| Velocity over Lifetime | `velocityMin/Max` (per-axis random start velocity), `orbit` (Y axis only, rad/s) | Linear XYZ over age plus space; orbital XYZ plus offset; radial; speed modifier | **P1** | M |
| Limit Velocity | `drag` (exponential, constant) | Speed limit curve, dampen, separate axes, size- and velocity-scaled drag | **P1** | S |
| Inherit Velocity | — | Initial / Current × multiplier | **P1** | S |
| Lifetime by Emitter Speed | — | Multiplier over speed range at spawn | P2 | S |
| Force over Lifetime | `gravity` (constant) | XYZ MinMaxCurve over age, space, randomize | **P1** | S |
| Color over Lifetime | `colorOverLife` (Gradient) × `startColor` | MinMaxGradient (two gradients); colour and alpha keys stay merged (fine) | **P1** | S |
| Color by Speed | — | Gradient over speed range (applied in `gather`) | **P1** | S |
| Size over Lifetime | `sizeOverLife` (Curve) | MinMaxCurve; separate axes need non-square quads | P1 / P2 (axes) | S |
| Size by Speed | — | Curve over speed range | **P1** | S |
| Rotation over Lifetime | `angularVelocity` (constant per particle) | MinMaxCurve over age | **P1** | S |
| Rotation by Speed | — | Curve over speed range | **P1** | S |
| External Forces | — | Runtime force fields (directional wind, point attractor, vortex) passed to `ParticleSystem::setForceFields(span)`; no physics module | P3 | M |
| Noise | Puff's `turbulence` is **shader-only** (it erodes the sprite outline; motion is unaffected) | CPU noise field: strength, frequency, scroll, damping, octaves, quality 1D / 2D / 3D, remap; position / rotation / size amounts | **P1** | M |
| Collision (planes) | — | Plane list plus dampen / bounce / lifetime loss / kill speeds / radius scale; collision events | P2 | M |
| Collision (world) | — | No physics module (`ne::collision` has AABB queries but no particle API) | out | — |
| Triggers | — | Volumes (spheres / boxes) passed at runtime, Inside / Outside / Enter / Exit = ignore / kill / event | P3 | M |
| Sub Emitters | — (`perfect_burst` fakes it with two emitters in the same frame) | Birth / collision / death / manual with inherit and probability | P2 | L |
| Texture Sheet Animation | `texture` (one image per emitter, UV 0..1) | Grid tiles, whole sheet / single row, random row, lifetime / speed / FPS, start frame, cycles; per-instance frame | **P1** | M |
| Lights | — (demos place static `env.pointLights` by hand) | Per-system budget into `Environment::pointLights`, ≤ 8 in total | P2 | M |
| Trails | — (stretched billboards fake short streaks) | Particle trails (ring buffer per particle), ribbon mode, width / colour over trail | P2 | L |
| Custom Data | `seed` (per-particle float to the shader) | Two vec4 streams | P3 | S |
| Renderer: billboard / stretched | `stretch`, `stretchMax` (≈ Unity Velocity Scale and a max) | `lengthScale`, `cameraScale`; stretched rotation | P2 | S |
| Renderer: horizontal / vertical billboard | — | Two vertex-shader branches | P2 | S |
| Renderer: mesh mode | — | A second instanced program over `Mesh` | P3 | M |
| Renderer: sort modes | Alpha: by view depth (≈ By Depth) per batch; additive: none | None / ByDistance / ByDepth / Oldest / Youngest per emitter; a cheaper sort | P2 | M |
| Renderer: alignment, pivot, flip, min / max size | — | View / World / Local / Facing / Velocity; pivot vec3; flip fraction; screen-size clamp | P2 | S |
| Renderer: material | `blend` Alpha / Additive, `sprite` Soft / Puff / Spark, `softness`, `coreShade`, `turbulence` | Premultiplied / multiply blend; **Ink** sprite; per-pack treatment | P2 | M |
| Inspector, curve / gradient editors, preview | — | A particle inspector in `level_editor` | P3 | L |

### 2.3 Top 10 gaps, by value

1. **MinMaxCurve / MinMaxGradient** everywhere, with curves over duration, two-curves mode and a multiplier. Every
   other module depends on it.
2. **Noise.** Smoke, magic and embers move in straight lines today. Puff turbulence is only on the sprite.
3. **Velocity over Lifetime.** Linear, orbital on all axes, radial and speed modifier. Today orbit is Y-only and constant.
4. **Texture Sheet Animation.** Flipbook fire, smoke and impacts. We can only show one image or a procedural sprite.
5. **Sub-emitters**, death and collision first: fireworks, sparks that pop, impact debris.
6. **Trails.** Sword swipes, magic ribbons, comet tails.
7. **Renderer modes and sort modes.** Horizontal ground decals (heal rings, shockwaves), vertical billboards
   (pillars of light), age-based sorting.
8. **Force over Lifetime plus Limit Velocity.** Rising heat, decelerating bursts, air resistance.
9. **Plane collision.** Sparks bouncing on the arena floor.
10. **An editor with a live preview.** Effects are hand-tuned as C++ constants today, and the JSON assets are not
    used at runtime.

---

## 3. Architecture

### 3.1 The value model

```cpp
// Particles.h — replaces the role of Range (Range stays, see compatibility)
enum class CurveMode : std::uint8_t { Constant, Curve, TwoConstants, TwoCurves };

struct MinMaxCurve {
    CurveMode mode = CurveMode::Constant;
    float min = 0.f, max = 0.f;          // Constant uses max (== min); TwoConstants picks in [min, max]
    std::vector<Curve::Key> keys;        // Curve, or the upper curve of TwoCurves; empty = constant 1
    std::vector<Curve::Key> keysMin;     // lower curve of TwoCurves
    float multiplier = 1.f;              // scales both curves (Unity's curveMultiplier)
    bool smooth = false;                 // false: linear (today's behaviour); true: monotone cubic (Fritsch-Carlson)

    MinMaxCurve() = default;
    MinMaxCurve(float v);                // NOLINT implicit: `d.lifetime = 2.f`
    MinMaxCurve(Range r);                // NOLINT implicit: `d.lifetime = Range(0.3f, 0.7f)` keeps compiling
    static MinMaxCurve curve(std::vector<Curve::Key> k, float mul = 1.f);
    static MinMaxCurve between(std::vector<Curve::Key> lo, std::vector<Curve::Key> hi, float mul = 1.f);

    // x: normalised time (cycle time, age or remapped speed); r: the particle's fixed [0,1) factor.
    float evaluate(float x, float r) const;
    float sample(float x) const { return evaluate(x, 0.f); } // test convenience, like Curve::sample
    bool isConstant() const;
};

enum class GradientMode : std::uint8_t { Color, Gradient, TwoColors, TwoGradients, RandomColor };

struct MinMaxGradient {
    GradientMode mode = GradientMode::Gradient;
    glm::vec4 colorMin{1.f}, colorMax{1.f};
    std::vector<Gradient::Stop> stops;     // Gradient / upper; empty = white (today's behaviour)
    std::vector<Gradient::Stop> stopsMin;  // lower gradient of TwoGradients
    glm::vec4 evaluate(float x, float r) const;
    glm::vec4 sample(float x) const { return evaluate(x, 0.f); }
};
```

**Compatibility rule: battle_demo, vfx_demo and the tests compile unchanged.** They use these forms:

- `Range(a,b)` and `Range(v)` assigned to `lifetime`, `speed`, `startSize`, `startRotation` and
  `angularVelocity`;
- `d.lifetime.min` / `.max` (`test_particles.cpp:325`);
- `sizeOverLife.keys = {{t,v}}`;
- `colorOverLife.stops = {{t,{rgba}}}`.

Hence:

- `Range` stays as a type, and `MinMaxCurve` converts from it implicitly.
- `min`, `max`, `keys` and `stops` keep their names.
- **Over-lifetime fields default to `mode = Curve` with empty keys, meaning constant 1.** So assigning `.keys`
  alone works just as it does now.
- The standalone `Curve` and `Gradient` stay as types, used by tests and as key storage.

**Per-particle random factors without extra columns.** In the random modes, Unity keeps a fixed per-particle
factor for each property. Instead of one float column per property:

- Store one `std::uint32_t rand_[i]` per particle, drawn from the emitter's PCG32 at spawn.
- Derive each property's factor as `hash01(rand_[i] ^ kSalt[property])`, where `hash01` is a 32-bit integer
  mix such as `lowbias32` mapped to [0,1).
- This is deterministic, costs one SoA column whatever the number of modules, and makes the editor's
  *Resimulate* trivial.
- Force over Lifetime's "Randomize" mixes in the frame counter.
- Today's `seed_` float becomes `hash01(rand_, kSaltShader)`.

**Sampling cost: bake a LUT.** At `instantiate()`, each non-constant curve is baked into a 64-entry `float` LUT,
and each gradient into a 64-entry `vec4` LUT. Both live in the emitter's arena, allocated once. Evaluating is then
one multiply, one floor and one lerp. The lookup is branch-free and the same on every path. The key data stays in
the desc for the editor and for saving.

### 3.2 JSON schema (back-compatible)

Every value field accepts any of these forms:

```jsonc
"startSize": 0.2                                   // Constant          (existing form)
"startSize": [0.1, 0.2]                            // TwoConstants      (existing form)
"startSize": {"curve": [{"t":0,"v":0},{"t":1,"v":1}], "mul": 0.3, "smooth": true}          // Curve
"startSize": {"min": [{"t":0,"v":0.5}], "max": [{"t":0,"v":1}], "mul": 0.3}                 // TwoCurves
"sizeOverLife": [{"t":0,"v":0},{"t":1,"v":1}]      // bare key array = Curve (existing form)
"colorOverLife": [{"t":0,"color":[1,1,1,1]}]       // bare stop array = Gradient (existing form)
"startColor": [1,0.5,0.2,1]                         // Color (existing form)
"startColor": {"between": [[1,0,0,1],[1,1,0,1]]}    // TwoColors
"startColor": {"min": [stops], "max": [stops]}      // TwoGradients
"startColor": {"random": [stops]}                   // RandomColor
```

New modules are optional objects. A module that is present is enabled unless it says `"enabled": false`. The
existing flat keys (`rate`, `gravity`, `drag`, `orbit`, `velocityMin/Max`, `stretch` and so on) keep their meaning
and are never renamed.

```jsonc
{ "name": "fire", "emitters": [{
    "name": "flames", "rate": 40, "loop": true, "maxParticles": 128, "lifetime": [0.6, 1.0],
    "simulationSpeed": 1.0, "startDelay": 0, "prewarm": true, "flipRotation": 0.5,
    "shape": "cone", "radius": 0.2, "radiusThickness": 1, "shapeTransform": {"position": [0,0.1,0]},
    "velocityOverLifetime": {"linear": [0, {"curve":[{"t":0,"v":0},{"t":1,"v":1}],"mul":1.5}, 0], "space": "world",
                              "orbital": [0, 2, 0], "radial": -0.3, "speedModifier": 1},
    "limitVelocity": {"speed": {"curve": [{"t":0,"v":4},{"t":1,"v":1}]}, "dampen": 0.3, "drag": 0.5},
    "inheritVelocity": {"mode": "initial", "multiplier": 0.5},
    "forceOverLifetime": {"force": [0, 2.0, 0], "space": "world", "randomize": false},
    "noise": {"strength": 0.6, "frequency": 0.8, "scrollSpeed": 0.3, "octaves": 2, "octaveMultiplier": 0.5,
              "octaveScale": 2, "quality": "medium", "damping": true, "remap": [{"t":0,"v":-1},{"t":1,"v":1}],
              "positionAmount": 1, "rotationAmount": 0, "sizeAmount": 0},
    "colorBySpeed": {"gradient": [...], "range": [0, 5]},
    "sizeBySpeed": {"curve": [...], "range": [0, 5]},
    "rotationOverLifetime": {"angularVelocity": [-90, 90]},     // degrees/s, like Unity
    "textureSheet": {"tiles": [4, 4], "animation": "wholeSheet", "timeMode": "lifetime",
                     "frameOverTime": {"curve": [{"t":0,"v":0},{"t":1,"v":1}]}, "startFrame": 0, "cycles": 1},
    "texture": "flame_sheet", "blend": "additive"
}]}
```

Loader rules:

- The schema is dispatched by JSON type: a number, an array of numbers, an array of objects, or an object.
- Unknown keys stay errors, through `r.finish()`.
- Angles are in degrees for new keys, matching Unity, and the loader converts them. The existing
  `startRotation` / `angularVelocity` stay in radians so the shipped assets don't change meaning.
- Every module's error goes through `data::Diagnostics` with its JSON path.

### 3.3 Module organisation: disabled modules cost nothing

`EmitterDesc` keeps its flat core (the Main, Emission, Shape and look fields) and gains one plain struct per
module (`VelocityOverLifetime velocityOverLifetime;` and so on). Each struct has an `enabled` flag and only POD or
MinMaxCurve members.

At construction, `ParticleEmitter` does three things:

- builds a `std::uint32_t modules_` bitmask from the enabled flags;
- bakes the LUTs;
- allocates any extra SoA columns the enabled modules need, such as `noiseSize_` and `frame_`.

A column a module doesn't use is never allocated.

`update(dt)` becomes a fixed sequence of **tight passes over `[0, count)`**, each skipped as a whole by one
bitmask test:

```
dt *= simulationSpeed; handle startDelay
0. age:      age += dt; t01 = age * invLife            (scratch column t01_, sized at instantiate)
1. forces:   v += (gravity*gravityMod(cycle) + force(t01)) * dt          [if FORCE]
2. noise:    v += noise(p, time) * strength(t01) * dt                     [if NOISE]
3. velocity: vLin = linear(t01); orbital/radial about offset              [if VOL]
4. inherit:  v += emitterVel * mult(t01) (Current mode)                   [if INHERIT]
5. limit:    clamp |v| to speed(t01), dampen, drag                        [if LIMIT]
6. drag/orbit (existing fields, unchanged maths)
7. integrate p += (v + vLin) * dt; rot += angVel(t01)*dt (+ by-speed)
8. collide:  planes → bounce/dampen/lifeLoss; push events                 [if COLLISION]
9. trails:   append ring-buffer vertex if moved > minVertexDistance       [if TRAILS]
10. kill:    single compaction pass (age >= life or killed flag); push death events
11. emit:    rate/bursts/distance → spawn(); then sub-emitter events → children
```

The rules that keep this fast and correct:

- **Kill happens only in pass 10.** Today `killAt()` runs inside the integrate loop. Moving it to one compaction
  at the end means no module pass sees indices shift under it.
- **Each pass is branch-free.** The mode switch (Constant / LUT / two LUTs) is hoisted out of the loop through a
  small template on the evaluator, so the inner loop has no per-particle mode branch.
- **The layout is SIMD-friendly.** `pos_` and `vel_` change from `vector<vec3>` to separate `x/y/z` float columns
  (SoA proper), 16-byte aligned and padded to a multiple of 8, so GCC and clang can auto-vectorise passes 0–7.
  This is internal. The `positionAt()` / `velocityAt()` accessors keep their signatures.
- **Render-only modules are evaluated in `gather()`**, not in `update()`: Color over Lifetime, Color by Speed,
  Size over Lifetime, Size by Speed, and the texture-sheet frame. This is what happens today, and it means a
  system that is simulated but culled pays nothing for them.

### 3.4 Determinism

- All randomness comes from the emitter's PCG32, plus `hash01` of the per-particle `rand_`.
- Spawns consume the RNG in a fixed order. Sub-emitter children consume their own RNG, in parent-index order of
  the events.
- The noise permutation table is built from the system seed at instantiate.
- **Guarantee:** the same `(desc, seed, dt sequence)` gives bit-identical particles within one build. Across
  compilers it holds only to a tolerance, because `std::exp`, `std::sin` and FMA contraction differ.
- Keep `-ffp-contract=off` on the vfx sources, so GCC and mingw agree with clang in practice.
- **Test:** run two systems through an irregular dt list (1/60, 1/144, 0.05, 1/30 and so on) and require identical
  `positionAt` / `velocityAt` / `ageAt` for every particle, for every module enabled.

### 3.5 No allocation per frame

- **Emitters:** everything is sized in `ParticleEmitter`'s constructor, which runs from `instantiate()`.
  - SoA columns: `maxParticles`.
  - Sub-emitter event buffer: `maxParticles + maxSpawnsPerFrame`.
  - Trail rings: `maxParticles × trailVerts`.
  - Noise table: 256 entries (or 1 KB for the 3D gradients).
  - Curve LUTs.
- **The renderer, which is today's real risk.**
  - The VBO grows on demand (`draw()`).
  - `batches_` can `emplace_back` past 16.
  - `order_` / `sorted_` are reserved for 2048.

  Add `ParticleRenderer::reserve(std::size_t instances, std::size_t batches)` and call it once after the effects
  are built. A helper sums `maxParticles` over a span of systems to size it. The demos already warm up by hand;
  `reserve()` makes that explicit and testable.
- **Lights:** see §4.4. **Callbacks** are function pointers plus `void*`, never `std::function`.

---

## 4. Constraints in detail

### 4.1 Collision: planes only

There is no physics module, so we follow Unity's Planes mode:

- **Source of planes:** `CollisionModule{ planes[8] (normal, d), space, dampen, bounce, lifetimeLoss, minKillSpeed,
  maxKillSpeed, radiusScale }`, plus `ParticleSystem::setCollisionPlanes(std::span<const Plane>)` for planes from
  the game. For example, the battle arena floor at y = 0.
- **The test:** a sphere of radius `0.5 × size × radiusScale`.
- **The response:**
  - reflect the normal component times `bounce`;
  - scale the tangent component by `(1 − dampen)`;
  - push the particle out of the plane;
  - add `lifetimeLoss × life` to its age;
  - kill it by speed.
- **Events:** each collision goes into the event buffer for sub-emitters.

### 4.2 Lights within the forward renderer's cap

The cap is **`Renderer::kMaxPointLights = 8`** (`engine/include/ne/render/Renderer.h:144`).

- The GLSL array is `#define MAX_POINT_LIGHTS 8` (`engine/src/render/Renderer.cpp:67`).
- `Renderer.cpp:951` silently clamps `env.pointLights` to the first 8.
- `Environment::pointLights` is a `std::vector`, and the editor reserves `kMaxPointLights` for exactly this
  reason (`EditorApp.cpp:54`).

The plan:

- `LightsModule{ ratio, randomDistribution, useParticleColor, sizeAffectsRange, alphaAffectsIntensity,
  rangeMul (MMC), intensityMul (MMC), maxLights (≤ 4, default 1) }`.
- `appendParticleLights(std::span<const ParticleSystem* const>, Environment& env, int budget, glm::vec3 camPos)`.
  It adds at most `budget` lights in total. The budget is `kMaxPointLights − scene lights`, with a default
  ceiling of 3 for all particle lights together.
  - Within a system it takes the brightest particles first, scoring `intensity × alpha / distance²`.
  - It works through a fixed-size candidate array on the stack.
  - It writes into an `env.pointLights` vector the caller reserved to 8. That is asserted in debug builds:
    `capacity() >= kMaxPointLights`.
- Light flicker between frames, as the brightest particle changes, is damped by keeping last frame's choices when
  they score within 20%.
- This needs no change to `Renderer.cpp`.

### 4.3 Style packs: particles have to read in every world

**Why fire looks wrong in the Unwritten today.** Particles draw in the transparent pass (`docs/style_packs.md`,
pipeline placement), *before* bloom and the tonemap and style stage. The Unwritten's style stage then applies:

- `monochrome = 1` and `saturation = 0`: luminance is remapped from ink to paper;
- bloom ×0.35;
- hatching, which lives in the **lit** shader, so particles never get it.

The result is that an additive spark (display `2.2, 1.9, 1.1`, linearised to about 5.7) becomes a near-paper-white,
soft grey smear with no line. That is the opposite of "ink on paper", where fire should be *drawn*: dark strokes
with paper showing through.

**Proposal: two layers.**

1. **An `Ink` sprite shape** (`SpriteShape::Ink`, P2, in the particle fragment shader only):
   - Alpha-blended, **dark** marks: a stroke-edged blob whose interior is hatched.
   - It samples a copy of `hatchTonalArtMap()`. That function is header-only and needs no GL, so
     `ParticleRenderer` builds its own 256² texture and **does not touch `Renderer.cpp`**.
   - The particle's luminance picks the TAM level through `hatchWeights()`, so bright fire becomes sparse strokes
     and its fading smoke becomes dense cross-hatch.
   - The outline is a 1–1.5 px ink rim, from `fwidth` on the disc distance, with the per-particle `seed` driving an
     8 fps tremble that matches `outlineJitter`.
   - Sparks (`Spark` plus stretch) become single tapered pen strokes. That is the same quad with a hard, tapered
     ink core and no halo.
2. **A per-pack particle treatment** (`ParticleStyle`, P2):
   - `ParticleRenderer::setStyle(const StyleSettings&)` is called each frame with
     `renderer.postSettings().style`. It only reads a struct and needs no PostProcess change.
   - It derives:
     - `inkAmount = monochrome × hatch`;
     - the ink and paper colours;
     - `jitterFrames`.
   - With `inkAmount > 0`, emitters whose desc says `"inkable": true` (the default for additive Soft / Spark)
     switch to the following, cross-faded by `inkAmount` so the Style Dial blends smoothly:
     - the Ink shape;
     - alpha blending instead of additive (additive light on paper is invisible after the mono remap);
     - colours remapped to ink.
   - Inside a colour-authority island, nothing changes: the post stage restores colour.
   - An emitter can opt out with `"inkable": false`, for example the Third's own warm light.

**The Lattice's neon glow and its bloom threshold.**

- **The numbers today:**
  - bloom prefilter uses `max(r,g,b)` against `PostSettings::bloomThreshold = 1` (`PostProcess.cpp:36-40`);
  - `art_direction_v2.md` §7 item 6 proposes `bloomThresholdScale = 2.5` for the Lattice, with skills authored
    at ≥ 3.0 scene-referred;
  - particle colours are display values and are linearised with `pow(c, 2.2)` when the target is HDR.
- **The threshold in authored units.** The maths gives:
  - linear 2.5 is display ≈ **1.52**;
  - linear 3.0 is display ≈ **1.65**.

  The current sparks and embers (2.0–2.2, so linear 4.6–5.7) still bloom under the Lattice. Heal's 1.5 (linear
  2.44) would **stop** blooming.
- **Proposal:**
  - a per-emitter `glow` field (default 1), a scene-referred multiplier applied *after* linearisation;
  - the convention that "this emitter is a Rule, skill or eye effect" sets `glow` so that its peak colour reaches
    ≥ 3.0 linear;
  - a pack-side `particleGlowScale` read from the style, for example 1.0 in the Lattice and 0.5 in the Unwritten;
  - a unit test of the threshold arithmetic: given the style and the emitter, does the peak colour exceed
    `bloomThreshold × bloomThresholdScale`?
- **What waits.** The `bloomThresholdScale` field itself belongs to the post and StylePack owner, and lands
  after the agent working on PostProcess. Until then particles read `PostSettings::bloomThreshold` alone.

---

## 5. Editor: a particle inspector (P3)

`tools/level_editor` is Dear ImGui (v1.92.9, `ne_imgui`). `EditorApp` is an `Application` with an ImGui layer.
Panels live in `engine/src/editor/EditorUi.cpp` (about 340 ImGui calls). The inspector is a list of
collapsible component sections with an undo `CommandStack` (`docs/editor.md`). The editor's idle frame is
allocation-budgeted.

**The scope is its own phase and its own panel, "Effect Editor"**, opened from the Assets panel by
double-clicking a `*.json` under `assets/vfx/`, or with File > New Effect.

- **Emitter list** on the left: add, duplicate, delete, rename, reorder, and an enable checkbox for each emitter.
- **Module foldouts** on the right, in Unity's order. Each `CollapsingHeader` has an enable checkbox; a disabled
  module is drawn greyed out and is skipped by the bitmask.
- **`MinMaxCurveField`**: a value field with a mode dropdown (●, ⤳, ⇄, ≋). It shows a constant, two constants,
  or a 64×20 thumbnail.
  - Clicking the thumbnail opens a **curve editor** (about 400 lines, ImDrawList) with:
    - keys you can drag;
    - add a key with a double-click, delete one with Del;
    - snapping to 0.05;
    - a y-range and multiplier field;
    - a lin / smooth toggle;
    - the band between two curves shaded;
    - presets: linear up and down, ease, bell.
- **`MinMaxGradientField`**: a gradient bar with draggable stops, a `ColorEdit4` (HDR) for the selected stop,
  and the two-gradient and random modes.
- **Live preview** in the 3D viewport.
  - The effect plays at a preview anchor in front of the camera, or on the selected node.
  - An overlay matching Unity's Particle Effect panel offers **Play / Pause / Restart / Stop**, **Playback
    Speed** (0.1–4×), a **Playback Time** scrubber (scrubbing re-simulates from 0 with fixed 1/60 steps, which
    is deterministic and therefore exact), the **particle count**, **Show bounds**, and **Resimulate** (on:
    restart the effect after every edit).
- **Save to JSON** through a new `writeEffect(const EffectDesc&) → std::string`. It emits the *shortest* form
  of each value (a bare number, `[min,max]`, a bare key array), so a saved file diffs cleanly against a
  hand-written one.
  - A round-trip test: parse, then write, then parse again gives an equal desc.
  - Undo uses `CommandStack` and merges a drag into one step.
  - Hot reload reuses `FileWatcher`.

The editing widgets live in a reusable `ne_editor_ui` header (`ne/editor/CurveWidgets.h`). The debug UI can then
use them for tuning at runtime.

---

## 6. Phasing

### Gate for every phase

1. Builds with **GCC `-Werror`**, **clang** and **mingw**, in the existing `build`, `build-clang` and
   `build-mingw` trees.
2. **`ctest`** passes, including `test_loader_robustness` (the "robustness: assets/vfx effects" mutation fuzz
   runs over the four assets and must not crash or hang the new parser).
3. **Determinism test** per phase: same seed and same irregular dt list give bit-identical particles with every
   module of that phase enabled.
4. `vfx_demo --hidden --no-vsync --fixed-dt 0.0166667 --frames 600 --perf-json` and the same for `battle_demo`
   both report **`allocsPerFrameMax: 0`**, checked with `tools/perf_check.py` against `bench/baseline.json`.
   Both already have budget entries there.

### P1: the value model and motion modules (vfx only)

**Files:**

- `engine/include/ne/vfx/Particles.h` and `engine/src/vfx/Particles.cpp`:
  - MinMaxCurve and MinMaxGradient;
  - LUT baking;
  - the module structs;
  - the SoA x/y/z split;
  - the pass pipeline;
  - `rand_`;
  - simulation speed, start delay, prewarm, flip rotation, gravity modifier;
  - emitter velocity and `reseed`.
- New `engine/include/ne/vfx/Noise.h` and `engine/src/vfx/Noise.cpp`: seeded value or gradient noise,
  1D / 2D / 3D, octaves.
- `EffectLoader.h/.cpp`: the schema in §3.2 and a new `writeEffect()`.
- `ParticleRenderer.h/.cpp` (vfx, allowed):
  - the texture-sheet frame in `ParticleInstance`;
  - attribute 3 becomes `vec4(age01, seed, frame, 0)`, and the `static_assert`s are updated;
  - tile UVs in the fragment shader, with optional frame blending;
  - `reserve()`.

  **No change to `Renderer.cpp`, `PostProcess.*` or `StylePack.h`.**
- `tests/test_particles.cpp`, plus a new `tests/test_particles_modules.cpp`.
- `games/examples/vfx_demo/main.cpp`. New effects:
  - a **noisy wisp swirl**;
  - a **flipbook fire**, with the 4×4 sheet generated procedurally at start, so no binary asset is needed;
  - a **decelerating burst ring** with limit velocity;
  - a **rising-heat** force curve.

  vfx_demo also loads one effect from `assets/vfx/` so the loader is exercised at runtime.
- New `assets/vfx/fire.json`, `wisp.json`.
- `bench/main.cpp`: a `particles_update_20k` benchmark (`allocsPerOp 0`).
- New `docs/vfx.md`, the module reference.

**Modules:** Main additions, Emission (rate curve, burst cycles / interval / probability, rate over distance),
Shape (radius thickness, hemisphere / circle / edge / rectangle, shape TRS, direction blends), Velocity over
Lifetime, Limit Velocity, Inherit Velocity, Force over Lifetime, Color over Lifetime and by Speed, Size over
Lifetime and by Speed, Rotation over Lifetime and by Speed, Noise, and Texture Sheet.

**Tests:**

- MinMaxCurve evaluates correctly in all four modes and with the multiplier.
- Smooth curves are monotone between keys.
- The LUT matches the exact evaluation within 1e-3.
- The per-particle factor is stable over a particle's life.
- **Golden back-compat:** the four `assets/vfx/*.json` parse to the same `EmitterDesc` values as before. The
  pre-change values are captured in the test.
- Every JSON value form parses, and bad forms report the right path.
- `writeEffect` round-trips.
- Physics:
  - force integration matches the analytic result;
  - limit velocity caps the speed;
  - orbital motion keeps the radius (within tolerance);
  - radial moves outward;
  - inherit (initial and current) works with a moving transform.
- Noise: deterministic, bounded by strength, zero when strength is 0, and quality levels differ.
- Texture sheet: lifetime, FPS and speed modes give the expected frame indices; a random row is stable per
  particle.
- **Disabled modules cost nothing:** with every module disabled the result is identical to the v1 behaviour, and
  the benchmark is within 5% of today's.
- The determinism test with all P1 modules on.

**Acceptance:**

- `battle_demo` and `vfx_demo` **compile without source changes** (see the compatibility rule) and render the same
  as before. Spot-check the screenshots; the existing effects use no new module, so they should be
  pixel-identical.
- The new vfx_demo effects are visible in a `--screenshot`.
- Everything in the gate.

### P2: rendering, events and the world

**Files:** the vfx sources, plus `ParticleComponent` (stop action, culling). Renderer and PostProcess are read
only, through `postSettings().style`. Changes:

- **Render modes:** Horizontal / Vertical billboard, `lengthScale` / `cameraScale` stretch, alignment
  View / World / Local / Facing / Velocity, `pivot`, `flip`, min / max screen size. Mesh mode is P3.
- **Sorting:**
  - per-emitter `sortMode` None / ByDistance / ByDepth / OldestInFront / YoungestInFront;
  - `std::sort` is replaced by a 16-bit-key **radix sort** into preallocated index arrays (O(n), no
    comparator);
  - a system-level `sortingFudge`.
- **Trails:**
  - a ring buffer per particle (`maxParticles × trailVerts`, holding position and birth time);
  - a ribbon mode;
  - width and colour over trail;
  - a new triangle-strip draw path in `ParticleRenderer` with its own pre-sized VBO.
- **Sub-emitters:**
  - `subEmitters: [{emitter: "debris", on: "death" | "birth" | "collision" | "manual", count: [2,4],
    probability: 0.8, inherit: ["color","size"]}]`;
  - a child is another emitter in the same `EffectDesc`, with `rate 0`, its own fixed pool and a topological
    update order;
  - `ParticleSystem::triggerSubEmitter(emitter, particle)` for the manual condition.
- **Plane collision** (§4.1), **lights** (§4.2), and **Lifetime by Emitter Speed**.
- **Style treatment** (§4.3): the `Ink` sprite, `setStyle()`, the `glow` field, and the Lattice threshold test.
- **Stop action and culling:** each system keeps bounds (min / max over the gather pass); culling skips the
  update of looping systems that are offscreen.
- `battle_demo` adopts the following, which needs a battle_demo source change; this is P2's first use of it:
  - sparks with a death sub-emitter;
  - floor collision;
  - one light per hit, with a budget of 2;
  - a sword-swipe trail.

**Tests:**

- Sort order for each mode.
- The radix sort agrees with `std::sort`.
- Trails: the ring wraps, minimum vertex distance is respected, and vertices die with the particle.
- Sub-emitters: the event count is correct, probability holds over a seeded sample, inheritance works, the child
  pool caps, and the whole is deterministic.
- Collision: bounce, dampen, lifetime loss and kill speeds.
- Lights: never more than the budget, and `env.pointLights.size() ≤ 8`.
- Ink: `setStyle(Unwritten)` switches inkable emitters; `None` is pixel-identical to before. Same contract as the
  style packs: "off is free and identical".
- **Allocation:** sub-emitter and trail pools are full at instantiate.

**Acceptance:**

- `post_demo` or `battle_demo --style unwritten` screenshots show ink-drawn sparks and fire.
- `--style lattice` shows the glow on skills.
- Everything in the gate.

### P3: editor and long tail

**Files:**

- `engine/src/editor/EffectEditor.cpp`, `engine/include/ne/editor/EffectEditor.h`;
- `ne/editor/CurveWidgets.h/.cpp`;
- hooks in `EditorApp` / `EditorUi`;
- `tests/test_editor.cpp` for the headless parts: the effect document, undo, and `writeEffect` round trip.

**Features:**

- everything in §5;
- Mesh render mode;
- emitting from a mesh shape (`MeshData`);
- triggers (runtime volumes);
- external force fields;
- custom data;
- 3D start size and rotation;
- ring buffer mode;
- custom simulation space;
- separate-axes size.

**Acceptance:**

- `level_editor --open-effect assets/vfx/fire.json --frames 300 --perf-json` reports `allocsPerFrameMax 0`
  when idle, with the preview playing.
- Editing a curve and saving gives a file that loads in vfx_demo.
- Everything in the gate.

---

## 7. Performance budget

**Target:** **20,000 CPU-simulated particles at 60 fps**, spending **≤ 2.0 ms** of update and gather on one core
of a desktop CPU from 2020 or later, with P1's typical modules on (force, noise at medium quality, colour and size
over life, texture sheet). Also ≤ 1.0 ms for upload and draw on a GPU. llvmpipe CI numbers are tracked, not gated.

| Item | Cost estimate | How |
|---|---|---|
| Core passes (age, force, integrate, kill) | ~10 ns/particle → 0.2 ms | SoA float columns, auto-vectorised (SSE2, 4-wide; AVX2 optional), no branches in the passes |
| Curve and gradient evaluation | ~2 ns per curve per particle | 64-entry LUTs, a hoisted mode switch |
| Noise, medium (2D), 2 octaves | ~25 ns/particle → 0.5 ms | Value noise with a seeded permutation table; Low quality (1D) is ~4× cheaper; 3D is reserved for hero effects |
| Gather (colour, size, frame, to `ParticleInstance`, 64 B) | ~8 ns/particle → 0.16 ms, 1.3 MB | Writes straight into the batch's pre-reserved array |
| Alpha sort | 20k: `std::sort` ≈ 1.2 ms → radix ≈ 0.15 ms | P2 radix on a 16-bit depth key |
| GPU upload | 1.3 MB per frame | Orphaned `glBufferData` plus `SubData`, as today |

**Memory per emitter:**

- **Core:** about 64 B per particle, which is 1.3 MB for 20k.
- **Trails:** add `trailVerts × 16 B` per particle. For example, 256 particles × 16 vertices is 64 KB.
- **Sub-emitter children:** sized by their own `maxParticles`.
- **Event buffer:** 8 B × the parent's `maxParticles`.

**How to get there, in order:**

1. The SoA x/y/z split and per-module passes (P1).
2. The LUTs (P1).
3. `reserve()` so the renderer never grows (P1).
4. The radix sort (P2).
5. Distance and visibility culling of whole systems (P2).
6. **Later:** multithreading *over emitters*. Each emitter is independent, apart from the sub-emitter order,
   which becomes a dependency level. Use a fixed job list with no allocation, deterministic because each
   emitter has its own RNG and pool. Do not split one emitter across threads.

**Measurement:**

- `bench/main.cpp`:
  - `particles_update_20k`: one emitter of 20k with P1 modules, plus 10 emitters of 2k;
  - `particles_gather_20k`.
- Both go in `bench/baseline.json` with `allocsPerOp 0`.
- A `vfx_demo --stress 20000` flag gives a frame-time check with `--perf-json`.

---

## 8. Risks and notes

- **Stale docs.** `docs/PERFORMANCE.md` says battle_demo is "tracked but not budgeted". But
  `bench/baseline.json` budgets `battle_demo` at `allocsPerFrameMax 0`, and `STATUS.md` agrees with the
  baseline. The gate relies on the baseline. Fix the doc sentence in P1.
- **The JSON assets are unused at runtime.** Both games build effects in C++. P1 makes vfx_demo load JSON. P2
  moves battle_demo's sparks to `assets/vfx/hit_spark.json` (which already exists and nearly matches), so the
  editor in P3 edits what the game ships.
- **Angle units.** The existing `startRotation` and `angularVelocity` are radians. New Unity-style keys are
  degrees. The docs and the loader comment must say so per key.
- **Changing `ParticleInstance`** in P1 changes the vertex layout. The `static_assert`s catch packing mistakes,
  and the renderer has one VAO, so the change is local.
- **Cross-compiler determinism** holds to a tolerance only (§3.4). Tests compare within one binary.
