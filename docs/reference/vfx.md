# Particles (ne::vfx)

CPU-simulated, deterministic billboard particles. `ne_core` holds the headless simulation and the JSON
loader; `ne_runtime` holds the GL renderer. The roadmap is in [particles_plan.md](particles_plan.md).
This page describes what is built: v1, Phase 1 (Unity-style values and modules) and Phase 2 (render modes,
sorting, trails and sub-emitters; see [Phase 2](#phase-2-render-modes-trails-sub-emitters)).

![vfx_demo](vfx_modules.png)

*`vfx_demo --post`. Back row: the v1 effects (fountain, sparks, smoke, swirl). Front row, from left to right,
the module showcases in `assets/vfx`: `ember_swirl` (noise, force, limit), `flipbook_fire` (4×4 texture sheet
with frame blending, rotation over lifetime), `heal_ring` (orbital, linear and radial velocity) and
`speed_sparks` (colour and size by speed, limit velocity).*

## Types

| Type | Header | Role |
|---|---|---|
| `EmitterDesc` | `ne/vfx/Particles.h` | Authoring data for one emitter: emission, shape, start values, over-life values, look, modules. |
| `ParticleEmitter` | `ne/vfx/Particles.h` | A fixed-capacity SoA pool. `update(dt)` never allocates. |
| `ParticleSystem` | `ne/vfx/Particles.h` | One effect: emitters that share a transform and a seed. |
| `EffectDesc` | `ne/vfx/EffectLoader.h` | `{name, emitters}` from JSON. `instantiate(seed)` builds a `ParticleSystem`. |
| `ParticleRenderer` | `ne/vfx/ParticleRenderer.h` | Instanced billboards, grouped by look (blend and texture). |
| `ParticleComponent` | `ne/vfx/ParticleComponent.h` | ECS hook: `updateParticles(scene, dt)` and `collectParticleSystems`. |

The same `(desc, seed, dt sequence)` always gives the same particles. All randomness comes from a seeded
PCG32 and from per-particle hashes, never from `std::` distributions.

## Values: MinMaxCurve and MinMaxGradient

Scalar fields such as `lifetime`, `speed`, `startSize`, `sizeOverLife`, `startRotation`, `angularVelocity`
and every module scalar are `MinMaxCurve`s. `x` is the emitter cycle position for start values, the
particle's age (0..1) for over-lifetime values, and the remapped speed for by-speed values. `r` is a fixed
per-particle random factor.

| Mode | Value | C++ | JSON |
|---|---|---|---|
| Constant | `min` | `d.speed = 2.f;` | `2` |
| TwoConstants | `min + (max − min)·r` | `d.speed = Range(1.f, 3.f);` | `[1, 3]` |
| Curve | `keys(x)·multiplier` | `MinMaxCurve::curve({{0,0},{1,1}}, 2.f)` | `[{"t":0,"v":0},{"t":1,"v":1}]` or `{"curve":[…],"mul":2}` |
| TwoCurves | `mix(keysMin(x), keys(x), r)·multiplier` | `MinMaxCurve::between(lo, hi, m)` | `{"min":[…],"max":[…],"mul":m}` |

Colour fields (`startColor`, `colorOverLife`, `colorBySpeed.color`) are `MinMaxGradient`s.

| Mode | C++ | JSON |
|---|---|---|
| Color | `d.startColor = glm::vec4(1, .5f, 0, 1);` | `[1, 0.5, 0, 1]` |
| Gradient | `d.colorOverLife.stops = {…}` | `[{"t":0,"color":[…]}, …]` |
| TwoColors | `MinMaxGradient::twoColors(a, b)` | `{"between": [[…], […]]}` |
| TwoGradients | `mode = TwoGradients`, `stopsMin`, `stops` | `{"min": [stops], "max": [stops]}` |
| RandomColor | `mode = RandomColor`, `stops` | `{"random": [stops]}` |

**Back-compat.** v1 code and files compile and load unchanged:

- `Range` converts implicitly to `MinMaxCurve` (Constant when `min == max`, otherwise TwoConstants).
- `glm::vec4` converts implicitly to `MinMaxGradient`.
- `d.sizeOverLife.keys = {…}` and `d.colorOverLife.stops = {…}` still work. The defaults are Curve and
  Gradient modes with empty keys, which mean 1 and white.

With no module enabled, the simulation is bit-identical to v1. `tests/test_particles_modules.cpp` checks this
against a reference recorded from the v1 code.

**Baking.** At construction each curve is baked to a 64-entry LUT (`kLutSize`), or kept as one or two
constants. Each gradient is baked to a 64-entry `vec4` LUT. The per-particle loops then do a clamp, an index
and a lerp, with no key search. A key that sits on the grid (`k/63`) is reproduced exactly. A key between
grid points is smoothed by at most `|slope change|·step/4`, within one step of the key.

**Per-particle randomness.** Each particle stores one 32-bit `rand_` drawn at spawn (in the slot of v1's
`seed` draw). Each property hashes it with its own salt (`hash01(rand ^ salt)`), so properties are
decorrelated and still stable over the particle's life.

## Modules

Each module is a plain struct on `EmitterDesc` with an `enabled` flag. `ParticleEmitter::modules()` returns
the bitmask (`ModuleBit`) of enabled modules. Each pass is skipped by one bit test, and a disabled module
allocates no column (`memoryBytes()` shows this). Rotation angles in modules are in **degrees**, as in Unity.
Orbital velocity is in rad/s, also as in Unity. The core `startRotation`, `angularVelocity` and `orbit` stay
in radians, as in v1.

| Module | Fields | Notes |
|---|---|---|
| `velocityOverLifetime` | `linear[3]`, `space`, `orbital[3]` (rad/s), `offset`, `radial` | Extra velocity that is not accumulated. Orbital is an exact rotation about `origin + offset`, so the radius is kept. Radial is positive outwards. |
| `limitVelocity` | `speed`, `dampen` (fraction of the excess removed per 1/60 s, 1 = hard clamp), `drag` | Gravity and forces are applied before the clamp. |
| `forceOverLifetime` | `force[3]`, `space`, `randomize` | `randomize` picks a new random factor every frame. |
| `inheritVelocity` | `mode` (`initial` / `current`), `multiplier` | Emitter velocity comes from the transform's motion. World space only. `resetMotion()` after a teleport. |
| `noise` | `strength`, `frequency`, `scrollSpeed`, `damping`, `octaves`, `octaveMultiplier`, `octaveScale`, `quality` (low = 1D, medium = 2D, high = 3D), `remap`, `positionAmount`, `rotationAmount`, `sizeAmount` | Seeded value noise (`ne/vfx/Noise.h`). `damping` divides strength by frequency. |
| `colorBySpeed` | `color`, `range` | Multiplies the over-life colour. |
| `sizeBySpeed` | `size`, `range` | Multiplies the size. |
| `rotationOverLifetime` | `angularVelocity` (deg/s) | |
| `rotationBySpeed` | `angularVelocity` (deg/s), `range` | |
| `textureSheet` | `tiles`, `animation` (`wholeSheet` / `singleRow`), `rowMode` (`custom` / `random`), `row`, `timeMode` (`lifetime` / `speed` / `fps`), `frameOverTime`, `startFrame`, `cycles`, `fps`, `speedRange`, `frameBlend` | Tile 0 is the first column of texture row 0 (v = 0). `frameBlend` cross-fades to the next frame in the shader. |

The update order is: age → force (plus gravity, when force or limit is on) → extra velocity (noise, linear,
radial, inherit current) → limit → v1 drag, orbit and integration → orbital → rotation → kill → emit.

## JSON

`loadEffectFile(path, diag)` and `parseEffect(text, diag)` read an effect, `writeEffect(effect)` writes one,
and `toJson` returns the JSON object. Emitter keys mirror `EmitterDesc`; the full list is in
`EffectLoader.h`. A module object that is present is enabled, unless it says `"enabled": false`. Unknown
keys are warnings with their JSON path. Bad values are errors with their path, for example
`emitters[0].noise.octaves`.

```json
{
  "name": "heal_ring",
  "emitters": [{
    "rate": 70, "loop": true, "maxParticles": 200, "lifetime": [1.4, 2.0],
    "shape": "ring", "radius": 1.0, "innerRadius": 0.95, "speed": 0,
    "colorOverLife": [{"t": 0, "color": [0.5, 2, 0.8, 0]}, {"t": 1, "color": [0.3, 1.2, 0.5, 0]}],
    "blend": "additive",
    "velocityOverLifetime": {"linear": [0, 0.6, 0], "orbital": [0, 2.5, 0],
                             "radial": [{"t": 0, "v": 0}, {"t": 1, "v": -0.5}]}
  }]
}
```

The writer emits the shortest form of each value and only the fields that differ from the defaults.
`parseEffect(writeEffect(e)) == e` holds for canonical values: keys sorted by `t`, and no `multiplier` on a
Constant or TwoConstants curve (the multiplier only scales the curve modes, so the writer drops it there).

## Rendering and allocation

```cpp
ParticleRenderer particles;
particles.registerTexture("flipbook", &sheet);   // before reserveFor: the texture is part of the look
particles.reserveFor(allSystems);                // sizes batches, sort scratch and the GPU buffer once
// per frame, inside Renderer's transparent pass:
particles.renderInPass(visibleSystems, view, proj);
```

`reserve(instancesPerBatch, batches)` is the manual form. With it, a steady-state frame allocates nothing,
even the first time a large burst plays. vfx_demo and battle_demo run with `allocsPerFrameMax` 0.

## Performance

`ne_bench --filter particles` runs three cases:

- `particles_update_20k`: one 20k emitter with force, limit, linear and orbital velocity, medium noise,
  colour and size by speed, rotation and a texture sheet, plus a moving transform;
- `particles_update_20k_v1`: the same 20k particles with no module on;
- `particles_gather_20k`: building the 20k `ParticleInstance`s.

The plan's target is ≤ 2 ms for update plus gather on a 2020 desktop core. Noise dominates the module cost.
Low quality is cheaper than medium.

## Phase 2: render modes, trails, sub-emitters

`vfx_demo --p2` shows the four P2 assets on the same stage: `fireworks` (rockets with trails and two death
sub-emitters), `stretched_sparks` (stretched billboards over a horizontal-billboard shockwave),
`falling_leaves` (mesh particles, sorted by distance) and `magic_ribbon` (a ribbon trail through orbiting
particles, with vertical-billboard glints).

### Render modes and sorting

| Key | Values | Notes |
|---|---|---|
| `renderMode` | `billboard` (default), `stretched`, `horizontal`, `vertical`, `mesh` | |
| `lengthScale`, `speedScale` | floats, defaults 2 and 0 | Stretched: length = size × (lengthScale + speed × speedScale). |
| `mesh` | a name given to `ParticleRenderer::registerMesh` | Required for `mesh`. An unregistered name draws billboards. |
| `sortMode` | `auto` (default), `none`, `distance`, `oldest`, `youngest` | `auto` keeps v1: alpha by view depth, additive unsorted. |

- Flipbook frames, colour, size and rotation work in every mode. Mesh particles are instanced, get a simple
  two-sided directional shade, and rotate by the particle's rotation about a stable per-particle axis
  (from its seed).
- The legacy `stretch` / `stretchMax` keys still apply to `billboard` only.
- The new modes use their own GL programs. The default billboard path issues the same GL calls as before, so
  existing effects render pixel-identically.
- `sortForDrawing` lives in `ne_core` so the tests can check every mode headless. It is still `std::sort`;
  the radix sort in the plan is not built.
- Soft particles (`softness`) fade against the scene depth in every mode, mesh particles and trails
  included, whenever the renderer has the depth texture.

### Trails

```json
"trails": {"mode": "perParticle", "ratio": 1, "lifetime": 0.3, "minVertexDistance": 0.05, "maxPoints": 16,
           "widthOverTrail": [{"t": 0, "v": 1}, {"t": 1, "v": 0}], "colorOverTrail": "...",
           "inheritParticleColor": true, "sizeAffectsWidth": true}
```

- **perParticle:** each trailed particle owns a ring of `maxPoints` (2..256) points. A point is added when
  the particle has moved `minVertexDistance`, and expires after `lifetime` (a MinMaxCurve over the particle's
  life). When the ring is full the oldest point is overwritten and counted (`trailPointsOverwritten`).
  `ratio` picks a stable share of particles by their hash. Trails die with their particle.
- **ribbon:** one camera-facing strip through the live particles in spawn order. `ribbonCount` splits them
  into that many strips.
- Width and colour are evaluated along `u` (0 at the head, 1 at the tail). The trail colour is multiplied by
  the particle's colour when `inheritParticleColor` is set, and the width by its size when
  `sizeAffectsWidth` is set.
- Storage is `maxParticles × maxPoints` points, allocated at instantiate. `gatherTrails` writes into a
  caller-reserved vector (`trailVertexCapacity()`); the renderer draws it as one triangle strip per look,
  from its own pre-sized buffer.

### Sub-emitters

```json
"subEmitters": [{"emitter": "burst", "on": "death", "count": [70, 90], "probability": 1,
                 "inherit": ["color", "size", "rotation", "velocity"], "velocityScale": 1}]
```

- `emitter` names another emitter in the same effect. `on` is `birth` or `death`. Collision events need P2
  collision, which is not built, so `collision` and `manual` are not accepted.
- The child spawns its shape centred on the event position, in world space even when the parent simulates
  in local space. Inherited colour and size multiply the child's start values; inherited rotation and velocity
  are added, the velocity scaled by `velocityScale`.
- A sub-emitter target ignores its own `rate` and `bursts` and does not `play()`. The system updates parents
  before children (a topological order built once), so a child spawned this frame is aged in the same frame.
  Unknown emitters, self-links and cycles are load errors with their JSON path.
- Pools: every child has its fixed `maxParticles`, and every parent has a birth and a death event buffer of
  `maxParticles` events. A spawn into a full child pool is dropped and counted (`droppedSpawns`); an event
  past a full buffer is dropped and counted (`droppedEvents`). Nothing allocates after instantiate.
- `writeEffect` writes every P2 field, and only those that differ from the defaults.

### Measured (P2)

On the shared, loaded development VM (GCC, RelWithDebInfo, `ne_bench --filter particles`):

| Case | min | median |
|---|---|---|
| `particles_trails_2k`: 2000 particles with 16-point trails, update + `gatherTrails` | 1.26 ms | 1.33 ms |
| `particles_subemit_fireworks`: 64 rockets, death bursts into a 4000-spark child with trails, update + gather + `gatherTrails` | 0.64 ms | 0.69 ms |
| `particles_update_20k_v1` (no modules), before and after P2 | 95–96 µs → 85–86 µs | |
| `particles_update_20k` (modules) | 1.79 ms | 1.97 ms (unchanged within noise) |

Death events are recorded in a separate pass before the kill compaction. A first version recorded them inside
it and made the no-module 20k update about 20 % slower (A/B: 96 µs → 115 µs, same instruction count under
callgrind); the separate pass removed that cost.

All allocation counters stay at 0: the P2 tests run all four assets for 600 frames under `NE_ALLOC_HOOK`.

### Deferred from P2

- plane collision and collision events, `manual` sub-emitter triggers;
- particle lights;
- the `Ink` sprite and per-pack treatment (owned by the style-pack work);
- stop action, bounds and culling;
- Lifetime by Emitter Speed;
- the radix sort and `sortingFudge`;
- alignment (World / Local / Facing / Velocity), pivot, flip, min / max screen size, `cameraScale`;
- battle_demo adoption (sparks with death sub-emitters, sword-swipe trail).

## Not built yet

Deferred from Phase 1 (see the plan for P2 and P3):

- `simulationSpeed`, `startDelay`, `prewarm`, `flipRotation`, `gravityModifier`;
- emission rate over time as a curve, burst cycles and probability, and rate over distance;
- new spawn shapes and a shape transform;
- smooth (Hermite) curves, since keys are linear;
- splitting the SoA columns into x/y/z floats;
- `-ffp-contract=off` for cross-compiler bit-exactness;
- `vfx_demo --stress`.
