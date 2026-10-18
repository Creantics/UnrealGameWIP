# Physics plan: making the world feel real

Status: P1 delivered (Jolt-backed `PhysicsWorld`, §4.2). P2a delivered: joints, ragdolls and the battle helpers (§4.3.1, with measured numbers). The rest of P2 and P3 are planned.
Owner: engine. Readers: engine and gameplay programmers, design.

The brief is "extremely good physics, the game needs to feel real". Solhaven's law is "deeds have weight", so the rpg needs physics that players can *feel*: heavy things are heavy, hits land, bodies fall believably (and gently; no gore), cloth and lanterns move, and nothing jitters or explodes. Today `ne/physics` is a kinematic toolkit: static shapes in an XZ grid (`CollisionWorld`) plus a move-and-slide `CharacterController`. It has no rigid-body dynamics, no joints, no ragdolls and no cloth. This document sets out what "feels real" means as testable moments (§1), which physics library to use (§2), how it fits the engine (§3), and the phases and gates (§4).

Evidence labels used below:

- **[verified]**: measured or read directly by me in this session, either from the library sources at the pinned tag or from a throwaway spike (described in §2.4).
- **[documented]**: stated in the library's own docs or README; I did not reproduce it.
- **[estimate]**: judgement.

---

## 1. Feel spec

Each moment is something a player would notice if it were wrong. Each has a one-line acceptance test that can run headless (doctest or a `physics_demo --autoplay` scenario with `--fixed-dt 1/60`). "Phase" is the phase that delivers it (§4).

| # | Moment | What the player feels | Acceptance test (one line) | Phase |
|---|---|---|---|---|
| F1 | Ground contact and stairs | Feet stay planted on slopes and steps; no hovering, no stutter going downstairs | Controller walking down a 0.3 m stair run at 5 m/s keeps `isGrounded` true on ≥ 98% of frames and its foot never ends a frame > 0.02 m above the surface | P1 (controller unchanged) |
| F2 | Stacks that stand | A stack of crates rests still; no creeping or jitter | A 10-box stack settles and every box sleeps within 2.0 s; max drift after sleep is < 1 mm over 10 s | P1 |
| F3 | Momentum chains | A domino run tips over in order and nothing tunnels | 40-domino run: every domino falls, in order, and the last falls within 6 s of the push | P1 |
| F4 | Mass you can feel when pushing | A flour sack gives way; a loaded cart barely moves | Player pushing at walk speed moves a 10 kg sack at ≥ 70% of walk speed and a 200 kg cart at ≤ 25% | P2 |
| F5 | Carrying | Picked-up objects lag slightly and sway; heavy loads slow you (the Load > 70 "shoulders drop") | A carried body tracks its hold point through a spring (≤ 0.15 m error at walk, settles in ≤ 0.3 s after a stop); move speed scales with carried mass per the design table | P2 |
| F6 | Battle hits with knockback | The hit lands, the target is shoved along the hit direction, and they never get pushed through walls | Knockback impulse of N·s moves a 70 kg target within ±10% of the expected distance on flat ground, and never past a wall (penetration < 0.02 m) | P2 |
| F7 | Hit-stop | Impact frames freeze 4–6 frames (NDD), then motion resumes with no pop | During hit-stop the battle world performs 0 steps for exactly the configured frames; post-resume body velocities equal pre-freeze velocities (bit-identical) | P2 |
| F8 | KO ragdoll (gentle) | A KO'd foe crumples and settles; it never flails, stretches, or clips into the floor | Ragdoll from KO pose settles (all parts asleep) within 2.5 s; no joint exceeds its limit by > 5°; no part goes below the floor; no part speed exceeds 8 m/s | P2 |
| F9 | Partial hit reactions | Hit in the shoulder, the upper body snaps back and recovers into the animation | Powered upper-body ragdoll blends back to the animated pose in 0.35–0.45 s; the lower body (feet) stays on the animated pose throughout (error < 1 cm) | P2 |
| F10 | Battle debris and props | Barrels, crates and bricks scatter and come to rest quickly | 50 debris bodies spawned by a hit all sleep within 3.0 s; 0 heap allocations per frame while they do | P2 |
| F11 | Secondary motion | Paper lanterns swing on their cords, banners and capes ripple in wind, hair settles | A lantern pushed 20° returns to within 2° of rest in 3–5 s (damped, not stiff); cloth stays within its collision capsules (no penetration > 1 cm) | P2 (lanterns), P3 (cloth, hair) |
| F12 | Camera collision | The camera never sees through walls and never snaps | Orbit camera around a wall corner: near plane never inside geometry; camera distance changes at most 0.5 m per frame when unblocked (existing camera behaviour kept, moved to PhysicsWorld queries) | P2 |
| F13 | Destructibles | The lifting-club shed lifts at one corner, tilts and goes up into the seam (P1-03); brick walls break into pooled pieces | Rift-suction field breaks the shed's hinge at the scripted force, and the shed rises into the seam within the cue window; pre-fractured pieces come from a pool (0 allocs) | P2 |
| F14 | The tram (moving platform) | Standing on the Lattice tram, you ride it smoothly; props on it stay put | Player and a crate on the tram keep their offset within 2 cm over 30 s of motion including the stop and start | P3 |
| F15 | Water | Crates bob and drift; the player feels resistance wading | A 0.5 m crate at density 600 kg/m³ floats with ~60% submerged, ±5%, and stops bobbing within 4 s | P3 |
| F16 | Determinism and replay | The same inputs give the same scene, so autoplay, replays and bug reports are reproducible | Golden-hash test: 600 steps of the demo scene give the same hash on GCC, Clang and MinGW, and when run twice in one process | P1 |

Tone constraint for F8 and F9 (from the Bible): KO is stylised and gentle. Ragdolls get damped joints, low max speeds and soft limits. Minors (Pia, Lumi, Toby) are never ragdolled into distorted poses: their profile uses tighter limits, or the KO plays an authored animation instead. That is a content rule enforced by the ragdoll profile files, not a physics feature.

---

## 2. Decision: integrate Jolt Physics v5.6.0

### 2.1 Recommendation

**Integrate [Jolt Physics](https://github.com/jrouwe/JoltPhysics) v5.6.0** (MIT; tag commit `e77f1755`, 2026-07-11), statically, behind `ne::physics::PhysicsWorld`. Build it with `CROSS_PLATFORM_DETERMINISTIC=ON`, an SSE4.2 SIMD baseline, compute backends off, and a pooled allocator plugged into its allocation hooks.

Why:

- **Constraints 1 to 3 are met, and I proved it in a spike [verified].** Jolt builds and links on GCC 13.3, Clang 18.1.3 and our MinGW 13-posix toolchain. Its headers compile under our full warning set with `-Werror`. A 500-body scene with two 11-part joint chains gives a bit-identical hash on all three compilers, and with a pooled allocator reaches 0 allocations per step in steady state (one first-run size-class miss remains; pre-seeding removes it by design, to be measured in P1).
- **It covers the whole feature list.** Ragdolls with powered motors (`Ragdoll::DriveToPoseUsingMotors`), `SkeletonMapper`, `CharacterVirtual`, soft bodies (cloth), hair, buoyancy (`Body::ApplyBuoyancyImpulse`), CCD, mesh and heightfield shapes, and constraint types including 6DOF.
- **It is shipped and active.** Used by Horizon Forbidden West and Death Stranding 2 [documented]; v5.6.0 is from July 2026 [verified].

### 2.2 Constraint scorecard

| Constraint (priority order) | Jolt 5.6.0 | Bullet 3.25 | PhysX 5.9 | Box3D 0.1.0 | In-house |
|---|---|---|---|---|---|
| 1. MinGW + GCC + Clang, `-Werror`, FetchContent | **Pass** [verified]: all three build; headers clean under our flags; upstream CI has an MSYS2 MinGW job | Builds historically; has many warnings; no SYSTEM-scoping story | **Fail/high risk**: no MinGW support; large CMake/packman build | Likely (C17, small) [not verified] | Pass (our code) |
| 2. Zero per-frame heap allocs after warm-up | **Pass with a fix** [verified]: 2 allocs/step stock, 0 with a pooled `JPH::Allocate` | Poor (`btAlignedObjectArray` growth, pair cache) [estimate] | Allocator callbacks; per-frame scratch is configurable [documented] | "World capacities" designed to avoid runtime allocs [documented] | Pass if designed for it |
| 3. Determinism at fixed dt | **Pass** [verified]: same hash on GCC, Clang, MinGW (Wine) | Same-binary only | Same-binary "enhanced determinism" mode [documented] | Cross-platform deterministic math [documented] | Possible; we would own the whole proof |
| 4. Binary size, static link | **+1.5 MB (Linux) / +2.3 MB (Win)** stripped spike [verified] | Similar or larger | Large (multi-lib) | Small | Small |
| 5. License | MIT | zlib | BSD-3 | MIT | n/a |
| Feature fit (ragdoll, cloth, CCD, character) | All | Most, aging | All | Rigid, joints, CCD, mover; no soft body | Years of work |
| Activity | v5.6.0, 2026-07 | Last tag 3.25, 2022-09 | 5.9.0 (`110.1-omni-and-physx-5.9.0`) | v0.1.0, 2026-06-30 (very early) | n/a |

Non-Jolt cells other than version, date and license are from prior knowledge, not verified in this session.

Sources (versions and dates checked with `git ls-remote` and shallow clones, 2026-10-17):

- Jolt: https://github.com/jrouwe/JoltPhysics, tag `v5.6.0`.
- Bullet: https://github.com/bulletphysics/bullet3, last tag `3.25`, 2022-09-24.
- PhysX: https://github.com/NVIDIA-Omniverse/PhysX, tag `110.1-omni-and-physx-5.9.0`.
- Box3D: https://github.com/erincatto/box3d, tag `v0.1.0`.

**Box3D** is the one to watch. It is from Erin Catto (Box2D v3), it is C17, it is designed for capacity-bounded allocation and cross-platform determinism, and it has a Soft Step solver. At v0.1.0 it is too young to bet the game on, and it has no soft bodies or ragdoll tooling. `PhysicsWorld` keeps the library private (§3.1), so switching to it later is a contained job if Box3D matures.

**PhysX** is the most capable, but it fails our top constraint: it is not built for MinGW and depends on NVIDIA's packman/preset build. **Bullet** is effectively unmaintained (no release in four years) and is not cross-platform deterministic.

### 2.3 The honest in-house cost

[estimate] To reach "works", for one experienced physics programmer:

| Subsystem | Weeks |
|---|---|
| Dynamic broadphase (BVH/SAP, sleeping islands) | 2 |
| GJK/EPA, convex hull support functions | 3 |
| Contact manifolds, persistence, warm starting | 4 |
| Sequential-impulse or soft-step solver, friction, restitution | 4 |
| Sleeping, islands, stability tuning | 2 |
| Joints (hinge, swing-twist, 6DOF, motors, limits) | 4 |
| CCD (speculative contacts or time of impact) | 3 |
| Triangle mesh and heightfield shapes, mesh contacts | 3 |
| Ragdoll building, powered ragdoll, pose mapping | 3 |
| Soft body or cloth | 4 |
| Cross-compiler determinism, SIMD, threading, tuning | 4+ |
| **Total** | **36–40+ engineer-weeks** |

That gets a working engine; Jolt-level stability (stacks that sleep, ragdolls that never explode, no tunnelling) took years of shipped titles. In-house would buy allocation control and determinism, which Jolt already gives us, at the cost of most of a year. **Do not build the solver.** Do build the wrapper, the ECS sync, the ragdoll and animation glue, and the gameplay feel layer. That is where the "feels real" work actually lives.

### 2.4 Spike evidence

The spike was a throwaway program outside the repo (not committed); its sources can seed the P1 determinism test.

The spike drops 500 dynamic bodies (boxes, spheres, capsules, hulls) onto a floor and a 20° slope, plus two 11-capsule swing-twist chains as stand-in ragdolls. It runs 600 single-threaded steps at 1/60 s (60 warm-up) twice in one process, hashes all positions and rotations (FNV), and counts `JPH::Allocate` and global `operator new` calls separately.

| Finding | Result | Label |
|---|---|---|
| GCC 13.3 / Clang 18.1.3 / MinGW 13-posix (our toolchain file) build Jolt + `-Werror` spike | All pass; Jolt ≈ 3 min on 4 cores | verified |
| Jolt headers under `-Wall -Wextra -Wpedantic -Wshadow -Werror` (`warn.cpp` includes PhysicsSystem, all shapes, Ragdoll, CharacterVirtual, SoftBody, SixDOF, SkeletonMapper, DebugRenderer, JobSystemThreadPool) | Clean on all three, even as non-SYSTEM includes | verified |
| Determinism hash (det ON, SSE4.2 baseline) | `91d692653b6c4ca3` on GCC, Clang, and MinGW exe under Wine; identical across the two in-process runs | verified (Windows run under Wine, not real Windows) |
| Stock allocator, allocs per step after warm-up | **2 every step** (1080 over 540 steps): `QuadTree::UpdatePrepare` (`new NodeID[mNumBodies]`, QuadTree.cpp:300) and `QuadTree::BuildTree` (`new Vec3[inNumber]`, QuadTree.cpp:544), from the broadphase rebuild | verified |
| Global `operator new` after warm-up | 0 | verified |
| Pooled recycling allocator on the `JPH::Allocate` hooks | 1 miss in run 1 (new size class at step 156), 0 in run 2; hash unchanged | verified |
| Step cost, GCC RelWithDebInfo, single thread, shared VM | mean 1.56–2.6 ms/step (≈ 522 active), worst step 6.6–13 ms while the pile settles; Clang 2.4 ms; MinGW-under-Wine 2.5 ms | verified, noisy |
| Cost of `CROSS_PLATFORM_DETERMINISTIC` | Not measurable on this VM: det ON 1.73–2.28 ms mean vs OFF 1.97–2.01 ms over 3 runs each (noise exceeds the gap). Jolt documents ≈ 8% | verified (noisy), documented |
| Stripped binary size of the spike | 1.55 MB Linux, 2.29 MB Windows. rpg.exe today is 5.99 MB stripped, so expect +2–3 MB with ragdoll, soft body and character code linked | verified (spike), estimate (rpg) |

### 2.5 Risks that could break the plan

1. **Per-step allocations (constraint 2).** Stock Jolt allocates twice every step in the broadphase rebuild. The fix is ours, not upstream's: route `JPH::Allocate / Reallocate / Free / AlignedAllocate / AlignedFree` into a pooled allocator (§3.6), and pre-seed it at init. Any pool miss after warm-up is counted by the same counter the perf gate reads, so a regression fails CI rather than hiding. We can also offer upstream a patch to use the `TempAllocator` in `UpdatePrepare`. Without this fix the rpg gate (0 allocs before frame 2307) and every demo's `allocsPerFrameMax 0` would fail as soon as anything moves.
2. **Jolt's SIMD flags are PUBLIC compile options.** By default Jolt adds `-mavx2 -mbmi -mpopcnt -mlzcnt -mf16c -mfma -mfpmath=sse` to *every target that links it*, and that would change codegen across the engine and break Windows players without AVX2. Mitigations: set the SSE4.2 baseline (`-msse4.2 -mpopcnt -mfpmath=sse`), and link Jolt **PRIVATE** from a dedicated `ne_physics` library so the flags stop there.
3. **MinGW-hostile defaults.** `JPH_USE_DX12` links MSVC-style `dxgi.lib`, and `JPH_USE_VK` calls `find_package(Vulkan)` and needs `dxc`. Both default ON. Turn off all four compute backends (`JPH_USE_DX12/VK/MTL/CPU_COMPUTE`). The consequence is that Jolt's 5.6 GPU hair is unavailable, so hair is a P3 decision (§4).
4. **Debug renderer and config defines.** `JPH_DEBUG_RENDERER` and the profiler are defined only for Debug and Release, and our default RelWithDebInfo gets neither. Set `DEBUG_RENDERER_IN_DISTRIBUTION=ON` so the debug view exists in all configs, and leave the profiler off. Jolt's `JPH_*` defines are PUBLIC compile definitions on the Jolt target. Every translation unit that includes Jolt headers must see the same set, otherwise there are ODR or layout mismatches. Only `ne_physics` sources include Jolt, which makes that automatic.
5. **Determinism scope.**
   - Bit-identical results need the same Jolt version, the same defines, the same SIMD baseline, the same API call order, and single-threaded stepping (or Jolt's documented deterministic threading path, to be verified in P2).
   - Broadphase query order, narrowphase result order and `GetActiveBodies` order are not deterministic, so our code sorts anything it acts on.
   - Contact listeners can fire from multiple threads, so events go into a ring and are sorted after the step.
   - Our own code that feeds physics should use `-ffp-contract=off`. Clang defaults to `on`; GCC in ISO mode is already off.
6. **Build time.** About 3 minutes of cold compile per build tree on 4 cores; cached afterwards.

---

## 3. Architecture

### 3.1 Library boundary: `ne_physics`

A new static library, `ne_physics` (engine/src/physics3d/ or a split of src/physics), owns everything that touches Jolt.

- **Public headers** (`engine/include/ne/physics/PhysicsWorld.h`, `Body.h`, `Ragdoll.h`) include only glm and std. No Jolt type and no `JPH` namespace appears. In the spike, `Result` collided with `JPH::Result` as soon as `using namespace JPH` was present, so this boundary is necessary, not cosmetic.
- **Implementation** uses a pimpl (`struct PhysicsWorld::Impl`), compiled with `-ffp-contract=off`, and is the only code that includes `<Jolt/...>`.
- **Linking:** `target_link_libraries(ne_physics PRIVATE Jolt)` and `target_link_libraries(ne_physics PUBLIC ne_core)`. Games link `ne_physics` only if they use it. In P1 only `physics_demo` and the tests link it, so the rpg binary and its gates do not change.

The Jolt FetchContent block for `cmake/Dependencies.cmake`, as validated by the spike:

```cmake
# Jolt Physics: rigid bodies, joints, ragdolls, soft bodies (MIT). Linked PRIVATE
# from ne_physics only; its SIMD flags and JPH_* defines must not leak further.
set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
foreach(_o JPH_USE_DX12 JPH_USE_VK JPH_USE_MTL JPH_USE_CPU_COMPUTE
           ENABLE_INSTALL ENABLE_OBJECT_STREAM INTERPROCEDURAL_OPTIMIZATION
           ENABLE_ALL_WARNINGS JPH_BUILD_SHARED_LIBS OVERRIDE_CXX_FLAGS
           PROFILER_IN_DEBUG_AND_RELEASE FLOATING_POINT_EXCEPTIONS_ENABLED
           GENERATE_DEBUG_SYMBOLS
           USE_AVX USE_AVX2 USE_AVX512 USE_FMADD USE_F16C USE_LZCNT USE_TZCNT)
    set(${_o} OFF CACHE BOOL "" FORCE)
endforeach()
set(DEBUG_RENDERER_IN_DISTRIBUTION ON CACHE BOOL "" FORCE)   # RelWithDebInfo gets JPH_DEBUG_RENDERER
FetchContent_Declare(JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.6.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR Build)
FetchContent_MakeAvailable(JoltPhysics)
get_target_property(_jolt_inc Jolt INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(Jolt PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_jolt_inc}")
```

Notes on these options:

- `ENABLE_ALL_WARNINGS=OFF` stops Jolt adding its own `-Werror` in its directory. Its sources are third-party, and like imgui and lua they are not built with `ne_warnings`. Our code that includes Jolt is still built with `-Werror` and is clean.
- `OVERRIDE_CXX_FLAGS=OFF` keeps our `CMAKE_CXX_FLAGS_*`.
- `GENERATE_DEBUG_SYMBOLS=OFF` stops Jolt forcing `-g` into Release.
- Jolt sets `-fno-rtti -fno-exceptions` in its own directory only, so that does not affect us.

### 3.2 `PhysicsWorld` API (P1 shape)

```cpp
namespace ne::physics {
struct BodyId { std::uint32_t v; };  struct ShapeHandle { std::uint32_t v; };  struct ConstraintId { std::uint32_t v; };
struct PhysicsConfig {   // fixed capacities, all allocated at construction
    std::uint32_t maxBodies = 4096, maxBodyPairs = 8192, maxContacts = 8192, tempBytes = 16u << 20,
                  eventCapacity = 1024, workerThreads = 0;
    float fixedDt = 1.f / 60.f;  int maxSubsteps = 4;
};
class PhysicsWorld {
public:
    explicit PhysicsWorld(const PhysicsConfig&);
    // + shape factories (box, sphere, capsule, hull, static mesh, compound), createBody/destroyBody,
    //   setTransform, addImpulse, moveKinematic, raycast/sweepSphere/sweepCapsule (RayHit, SweepHit, LayerMask)
    int   step(float frameDt);   // accumulator; returns fixed steps taken (<= maxSubsteps)
    void  stepFixed();           // exactly one step (tests, hit-stop control)
    float alpha() const;         // accumulator / fixedDt
    std::span<const ContactEvent> contactEvents() const;  // this step's events, sorted, fixed ring
    std::uint64_t stateHash() const;
    void  debugDraw(debug::DebugDraw&, DebugFlags) const;
};
}
```

Queries reuse the existing `RayHit` and `SweepHit` types and `LayerMask`, so gameplay code that uses `CollisionWorld` today can move over in P2 with little churn. Nothing in the API returns a container that allocates. `contactEvents()` is a span over a fixed ring.

### 3.3 ECS components and sync

| Component | Contents | Notes |
|---|---|---|
| `RigidBody` | `BodyId id; Motion motion; glm::vec3 prevPos, currPos; glm::quat prevRot, currRot; float mass; LayerMask layer, mask;` | prev/curr are written once per fixed step, from Jolt |
| `Collider` | `ShapeHandle shape; glm::vec3 offset; glm::quat offsetRot; float friction, restitution;` | shapes are shared and created at load |
| `Joint` | `ConstraintId id; JointType type; entt::entity a, b; ...limits, motor...; float breakImpulse;` | breakable joints for F13 |
| `PhysicsMaterial` (optional) | friction, restitution, density, audio/vfx surface id | feeds contact hooks |

Sync rules:

- **Before step:** kinematic bodies take their `Transform` through `moveKinematic` (so contacts get velocity, which matters for the tram). Dynamic bodies are never written from `Transform` except by an explicit `teleport`.
- **After each fixed step:** for every *active* dynamic body (Jolt gives the active list), `prev = curr; curr = jolt`. The active list is sorted by `BodyId` before use, because its order is not deterministic. Sleeping bodies cost nothing.
- **Render:** `Transform` = `mix`/`slerp` of prev and curr by `alpha`; visual only, gameplay reads `curr`.
- **Hierarchy:** physics-driven entities must be root-level (`Transform.parent == entt::null`), checked by a debug assert.
- **Prefabs:** `ColliderData{shape, dynamic, ...}` from `Prefab.h` maps directly to Collider plus RigidBody. This also fixes a LevelRuntime known issue: rotated boxes become AABBs today, and in Jolt they are real oriented boxes.

### 3.4 Fixed step, interpolation and hit-stop

- `step(frameDt)` accumulates time and runs at most `maxSubsteps` (4) fixed steps of `fixedDt` (1/60). Any excess is dropped (with a counter), so a hitch cannot cause a spiral of death.
- In `--fixed-dt` runs (autoplay, CI) the frame dt equals the fixed dt, so it is exactly one step per frame and fully reproducible.
- **Hit-stop (F7):** the battle state simply does not call `step` for the configured 4–6 frames, while the simulated clock and the presentation layer continue (camera shake, VFX). Because nothing is stepped, resumption is bit-identical. The animation system freezes the same frames, so the ragdoll and animation stay in sync.
- **Worlds:** one `PhysicsWorld` per scene (explore, battle), created and pre-seeded at state load.

### 3.5 Layers and filtering

- Our `LayerMask` (uint32) maps onto Jolt's `ObjectLayerPairFilterMask` (group bits plus mask bits in the `ObjectLayer`). This needs `OBJECT_LAYER_BITS=32` if we want 16 group bits and 16 mask bits; otherwise it is 8+8 with the default 16. Decide in P1: the default 8+8 is enough for the current layers.
- **Broadphase layers** (fixed, 4): `Static`, `Moving`, `Debris` (cheap, does not collide with other debris; F10) and `Sensor` (triggers, water volumes, the Rift field).
- `kLevelStaticLayer` and `kLevelDynamicLayer` from LevelRuntime keep their meaning.

### 3.6 Memory: zero allocations after warm-up

- **Hooks.** At `ne_physics` init, set `JPH::Allocate`, `Reallocate`, `Free`, `AlignedAllocate` and `AlignedFree` to `ne::physics::detail::PoolAllocator`. This is a power-of-two bucketed recycler with a 64-byte header holding the size class, as in the spike (verified to reach 0 misses).
- **Pre-seeding.** At `PhysicsWorld` construction, pre-seed the buckets for the broadphase rebuild arrays, whose sizes are bounded by `maxBodies` (`4 × maxBodies` and `16 × maxBodies` bytes plus the header), plus a small count of each common class. A miss after warm-up calls the system allocator **and bumps `ne::core::AllocCounter`**, so the perf JSON reports it and the gate fails. Jolt's malloc-backed default allocations would otherwise be invisible to our `NE_ALLOC_HOOK` counter.
- **Temp memory.** A `TempAllocatorImpl` of `tempBytes` is allocated once per world.
- **Shapes.** Shapes are created at load (`Ref<Shape>` is ref-counted and allocates). The game never creates shapes per frame. Debris uses a preallocated body pool: bodies are created at load, kept outside the world, and added and removed with `AddBody`/`RemoveBody`. These calls are expected not to allocate once the broadphase has capacity [not verified; P1 test].
- **Contact events.** A fixed `std::array`-backed ring. The `ContactListener` writes into it under a spinlock when threaded; after the step the ring is sorted by `(BodyId a, BodyId b)`. On overflow the newest events are dropped and `droppedEvents()` is incremented; there is no growth.
- **Our job system is not used.** `ne::jobs::JobSystem` allocates once per job. P1 uses Jolt's `JobSystemSingleThreaded`, which made 0 allocations in the spike. P2 evaluates `JobSystemThreadPool` (§3.10).

### 3.7 Coexistence with `CollisionWorld` and `CharacterController`

**Phase 1 does not touch them.** The rpg keeps `CollisionWorld collision` and `CharacterController player_` exactly as now. Behaviour and API stay the same, so `rpg_autoplay_full` (0 allocs before frame 2307) does not move. Only `physics_demo` and the tests link `ne_physics`.

**Phase 2 options**, decided by a parity test:

- **A (preferred).** Keep our `CharacterController` and its tuned feel (step-up, ground snap, slope limit), but give it a query backend interface implemented by both `CollisionWorld` and `PhysicsWorld`. The controller pushes dynamic bodies with mass-scaled impulses (F4) and is pushed by kinematic platforms (F14).
- **B.** Implement the same `CharacterController` API on `JPH::CharacterVirtual`. This gives better moving-platform and dynamic-body interaction for free, at the risk of changing feel.

Either option ships only if the existing controller tests pass unchanged, the rpg autoplay reaches the same story marks, and alloc counts stay at 0. `CollisionWorld` is retired in its own change once nothing uses it; moving level dynamics off it also fixes the allocating `syncDynamicColliders` known issue.

The follow camera already collides against `CollisionWorld`; F12 moves it to a `PhysicsWorld` sphere sweep against the Static layer only, with smoothing unchanged.

### 3.8 Ragdolls and animation blending

- **Build.** At character load, build a `JPH::Skeleton` from the `ne/anim` skeleton (joint names and parents), and a `RagdollSettings` from a **ragdoll profile** (JSON next to the model: which joints get bodies, capsule radii, swing-twist limits, masses, motor strengths, the "gentle" damping preset). A `SkeletonMapper` maps the full animation skeleton to the reduced ragdoll skeleton. The settings are created once per character type, instances are pooled per battle, and a battle needs at most 2–4 at once.
- **KO (F8).** `SetPose` from the animated pose, carry over joint velocities, apply the hit impulse at the struck part, disable motors and let it settle; later freeze to a static pose to free the instance.
- **Partial or powered reactions (F9).** The ragdoll stays active with motors on, driven each step by `DriveToPoseUsingMotors(animatedPose)`. The hit impulse goes to the struck body. The result is written back into a `Pose` and blended over the animation with `Pose::blend` and a mask from `makeSubtreeMask(spine)`. The blend weight ramps 1 → 0 over about 0.4 s, and the lower body stays animated. Jolt 5.6's velocity variant of `DriveToPoseUsingMotors` can be used for smoother tracking.
- **Write-back.** After each step, map ragdoll bodies back through `SkeletonMapper` into the full `Pose`; unmapped joints (fingers, face) stay animated.

### 3.9 Debug draw

- **P1:** `PhysicsWorld::debugDraw` walks bodies and draws our own primitives through `ne::debug::DebugDraw` (box, sphere, capsule, oriented box, contact arrows, sleeping bodies dimmed). This is cheap and needs no Jolt renderer.
- **P2:** implement `JPH::DebugRendererSimple` (lines and triangles) on top of `DebugDraw` for hulls, meshes, constraints and soft bodies. It is available in every config because of `DEBUG_RENDERER_IN_DISTRIBUTION=ON`, and is toggled from the existing debug overlay. It must not allocate when off.

### 3.10 Threading

- **P1:** single-threaded `JobSystemSingleThreaded`. Determinism comes first, and the budget fits on one core.
- **P2:** try `JobSystemThreadPool` (`workerThreads = 2–3`). This is accepted only if:
  1. 0-thread and 3-thread runs give the same `stateHash` over 600 steps;
  2. 0 allocs per frame after warm-up;
  3. ≥ 1.5× speed-up on the 500-body scene.

  Otherwise stay single-threaded.

---

## 4. Phases and gates

### 4.1 Gates common to every phase

Every phase ships only when all of these pass:

1. **Builds:** GCC with `-Werror`, Clang with `-Werror`, and MinGW (cmake/toolchains/mingw-w64.cmake) with `-Werror`.
2. **ctest** fully green.
3. **Perf:** `physics_demo --autoplay --fixed-dt 1/60 --perf-json` reports `allocsPerFrameMax 0` after warm-up, and the pool-miss counter is 0.
4. **The rpg gate is unchanged:** `rpg_autoplay_full` with `allocsPerFrameMaxBeforeMark: 0` (0 allocs before frame 2307) in bench/baseline.json via tools/ci/local_ci.sh. Same story marks, same frame counts.
5. **Determinism test:** a golden `stateHash` after 600 steps of the reference scene, checked in. It must match on GCC, Clang and MinGW (under Wine in CI), and across two runs in one process. The golden value is bound to the pinned Jolt tag and option set (the spike's hash changed from `91d692653b6c4ca3` to `5110297ed39743c8` with determinism off), so changing either bumps it deliberately.
6. **Performance budget** (measured in `ne_bench`, Release, one core, mean / p95 per fixed step):

| Scenario | Mean | p95 |
|---|---|---|
| 500 active bodies + 2 ragdolls | ≤ 2.0 ms | ≤ 4.0 ms |
| rpg explore scene (typical) | ≤ 1.0 ms | ≤ 2.0 ms |
| rpg battle with 2 KO ragdolls and 50 debris bodies | ≤ 1.5 ms | ≤ 3.0 ms |

   No frame runs more than 4 substeps. The spike's 1.56–2.6 ms mean on a noisy shared VM in RelWithDebInfo suggests the 2.0 ms budget is reachable in Release, but it is not yet proven. P1 must measure it on the bench machine before freezing the number.
7. **Size check:** stripped rpg.exe growth is reported in the PR; the expectation is ≤ 3 MB.
8. **Docs:** a `docs/STATUS.md` row is added or updated.

### 4.2 P1: integration and rigid bodies

Scope:

- Jolt via FetchContent (§3.1); the `ne_physics` library; the pool allocator and counters (§3.6).
- `PhysicsWorld` with box, sphere, capsule, convex hull, static mesh and compound shapes; bodies; impulses; kinematic moves; raycast and sweeps; the contact event ring; `stateHash`; own-primitive debug draw.
- ECS components `RigidBody` and `Collider`; prefab `ColliderData` mapping; the fixed-step accumulator; alpha interpolation.
- **`physics_demo` scenes:**
  - a 10-high box stack and a pyramid;
  - slopes at 15°, 30° and 45° with friction variants;
  - a 40-domino run;
  - a ramp launching spheres into a wall (CCD check);
  - a pile of 200 convex hulls;
  - a scripted autoplay that pushes the dominoes and fires a ball at the stack.
  - `--perf-json`, `--fixed-dt` and `--autoplay` flags like the other demos.
- **Tests (doctest):**
  - body lifecycle, with no leaks after destroy;
  - shape creation;
  - interpolation (alpha 0 and 1 give exactly prev and curr);
  - accumulator caps at `maxSubsteps`;
  - layer filtering (a masked pair never produces a contact event);
  - contact-event order is sorted and overflow is counted;
  - stack sleeps (F2) and dominoes complete (F3);
  - **bit-identical determinism:** two worlds built from the same script give equal `stateHash` after 600 steps, and the hash equals the checked-in golden value;
  - allocation tests: 0 `AllocCounter` increments over 300 steps after warm-up, and while adding/removing 50 pooled bodies per step.
- **STATUS.md row:** `| Physics dynamics (Jolt: rigid bodies, shapes, ECS sync) | ne/physics (ne_physics) | P1 done | Jolt v5.6.0, 0 allocs/frame, golden hash on GCC/Clang/MinGW |`.

Explicitly *not* in P1: any change to `CollisionWorld`, `CharacterController`, the rpg, or the camera.

Feel moments delivered: F2, F3, F16 (F1 is unchanged).

### 4.3 P2: gameplay physics in the rpg

Scope:

- **Joints:** hinge, swing-twist, 6DOF, distance; motors and limits; breakable joints.
- **Ragdolls:** profiles; KO ragdolls (F8); powered partial hit reactions blended into animation (F9); a gentle preset; content rule for minors.
- **Battle:** knockback through `addImpulse` with wall-safe resolution (F6); hit-stop that skips steps (F7); a debris pool (F10).
- **Explore:**
  - push and carry (F4, F5): flour sacks, crates and the wheelbarrow, with `CharacterController` choosing option A or B from §3.7 behind its unchanged API;
  - momentum and turn inertia as a controller feel layer (acceleration curves, and leaning into turns);
  - pendulum lanterns, Wim's lamp-post sign and the hanging banner rope as jointed chains (the lantern half of F11).
- **Destructibles (F13):** pre-fractured pooled pieces and a breakable-hinge shed; a Rift suction force field (a Sensor volume that applies a force toward the seam).
- **Camera collision** through `PhysicsWorld` sweeps (F12).
- **Contact hooks:** events above an impulse threshold drive audio (material pair → sound, volume from impulse) and particles (dust, splinters), rate-limited, 0 allocations.
- **Tooling:** the Jolt `DebugRendererSimple` bridge; the thread-pool evaluation (§3.10).
- **Level runtime:** level dynamics move from `CollisionWorld` into `PhysicsWorld`, which fixes the rotated-box and `syncDynamicColliders` known issues.

Extra gates for P2:

- the controller parity tests are unchanged;
- the rpg autoplay reaches the same marks with 0 allocations;
- the 500 bodies + 2 ragdolls budget;
- a ragdoll stability soak (1,000 random KOs; no NaN, no joint past its limit by > 5°, no part under the floor);
- the 0-thread versus N-thread hash test, if threading ships.

Feel moments delivered: F4–F10, the lantern half of F11, F12, F13.

#### 4.3.1 P2a delivered: joints, ragdolls, battle helpers

P2 is split. **P2a** (this section) adds joints, ragdolls and the battle helpers to `ne_physics` with their gates. The rpg and battle_demo are **not wired** yet: neither links `ne_physics`, and linking it would pull Jolt into rpg.exe (gate 7, the size check). Wiring them is P2b, together with push/carry, the explore chains, the destructibles, the camera and the contact hooks.

**What shipped (no Jolt type in any public header):**

- **Joints** on `PhysicsWorld` through handles (`ConstraintId`): hinge, swing-twist, 6DOF and distance, with limits, limit springs, friction, motors (velocity and position, with a force/torque cap) and breakable joints. A joint whose impulse goes over `breakImpulse` is disabled and reported once in `brokenJoints()`; `mendJoint()` re-enables it. `collideConnected = false` filters the joined pair through a pair filter, not through collision groups.
- **Ragdolls**, in `ne/physics/Ragdoll.h`:
  - The data is the bone→part map `assets/physics/ragdoll/humanoid_map.json`. Each part lists candidate joint names, so one map serves the engine's `rigged_humanoid` and canonical humanoid names.
  - There are three profiles, `default`, `gentle` and `minor` (`assets/physics/ragdoll/*.json`). `minor` sets `useAuthoredKo`, so `beginKo()` refuses and the caller plays the authored KO animation. It also has tighter limits and lower velocity caps.
  - The states are Animated (kinematic), HitReaction (F9: the upper body is dynamic and motor-driven, the pelvis and legs stay kinematic on the animated pose) and Ko (F8).
  - It is built on `PhysicsWorld` capsules and swing-twist joints rather than `JPH::Ragdoll` (§3.8). This keeps Jolt out of the header and makes the map data-driven.
- **Battle helpers**, in `ne/physics/BattlePhysics.h`:
  - `applyKnockback` (F6) applies the knockback through `addImpulse`. A sweep along the predicted slide cuts the impulse so the target stops short of a wall.
  - `HitStop` (F7) does not call `world.step` for N frames (no time is banked), so the motion resumes bit-identically.
  - `DebrisPool` (F10) holds pre-created bodies and switches them in and out of the world with `setInWorld`. It reuses the oldest piece when the pool is full.
- **Assists.** KO ragdolls and debris use a settle assist and a "go heavy" damping ramp: once every part has been slow for a short while, the body is put to sleep. Jolt's own sleep test alone misses the 2.5 s and 3 s windows. This means the sleep in F8 and F10 is *assisted*, not natural.
- **Ragdoll world settings.** Worlds that hold ragdolls use `speculativeContactDistance 0.08` and `penetrationSlop 0.01`. Ragdoll parts use 4–24 position iterations (`positionStepsOverride`). The defaults are unchanged, so the P1 golden hash `0x99611397fdbfa1ef` is unchanged.
- **Pool allocator.** The allocator now refills its size classes after every shape, body and constraint is created. A world with a few hundred joints had used up the 128-byte class that the broadphase rebuild recycles, which caused one allocation per run in `physics_demo`.
- **`physics_demo`:**
  - Station 6 is a KO line-up (default and gentle profiles, plus a minor character that plays the authored KO) and a powered dummy taking hit reactions, with a 5-frame hit-stop and a 24-piece debris burst.
  - Station 7 is a motorised hinge door hit by balls, a 12-link swing-twist chain with a distance-jointed weight, and a 10-plank bridge with breakable hinges that an 800 kg ball breaks.
  - The ragdoll assets are found through `--assets DIR`, then `NE_ASSET_DIR`, then the source tree.
- **`physics_bench`** has three cases, each printing one JSON line:
  - `reference` (P1);
  - `ragdoll`: 500 bodies + 2 ragdolls, with a hit reaction and a double KO every 3 s;
  - `battle`: 2 KO ragdolls + 50 debris every 4 s.
  
  Their budgets are in the `physics` section of `bench/baseline.json`. `tools/perf_check.py --physics-bench build/bench/physics_bench` checks them. `local_ci.sh alloc_budgets` runs that with `--allocs-only`, so it checks allocations, pool misses and determinism but not timings.

**Measured gates** (doctest in `tests/test_physics_joints.cpp`, `tests/test_ragdoll.cpp` and `tests/test_battle_physics.cpp`; GCC RelWithDebInfo):

| Gate | Requirement | Measured |
|---|---|---|
| F8 KO (default profile) | asleep ≤ 2.5 s; ≤ 5° past a limit; nothing below the floor; ≤ 8 m/s | asleep at 1.85 s; 2.16° worst; lowest part −0.017 m (capsule skin inside the 0.01 m slop + speculative margin); peak 6.39 m/s. Angular speed reaches the 20 rad/s cap |
| F8 KO (gentle profile) | same | 2.22 s; 2.15°; −0.015 m; 5.0 m/s |
| 1,000-KO soak (random impulse, part and facing) | no NaN, ≤ 5° past a limit, no part through the floor | no NaN; 4.78° worst; lowest −0.032 m (63 of 1,000 KOs dip below −2 cm for a frame; none passes through); peak 7.26 m/s; 12 not yet asleep at 2.5 s (the soak checks stability, not the sleep time) |
| F9 powered hit reaction | blend back in 0.35–0.45 s; feet within 1 cm | blend-back 0.40 s; peak upper-body deflection 6.6°, residual 2.6° at the end; foot error 0 cm (the lower body stays kinematic) |
| F6 knockback | ±10% of the expected slide on flat ground; < 0.02 m into a wall | flat −4.0% to −1.6%; wall: never reached. The capsule stops 0.032 m short of the wall with the impulse cut, and 0.021 m short with `guardKnockback` alone (a raw 12 m/s shove 0.65 m from the wall) |
| F7 hit-stop | 0 steps for N frames; bit-identical resume | 5 frames skipped; state hash with hit-stop equals the hash without it (`0xbaadbc39efe5844b`) |
| F10 debris | 50 pieces asleep ≤ 3.0 s; 0 allocs | all asleep at 2.32 s; 0 allocations |
| Joints | limits, motors, breakable | hinge limit held under a hard spin; motors reach their targets; a breakable joint breaks once and can be mended; bridge holds itself (peak 70 N·s) and an 80 kg crate (104 N·s), and breaks under an 800 kg ball (147 N·s against a 130 N·s threshold) |
| 0 allocs per step | 500 bodies + 2 ragdolls, through KO/hit/reset transitions | 0 allocations, 0 pool misses (576 bodies) |
| Golden hash | unchanged | `0x99611397fdbfa1ef` |
| `physics_demo` | `allocsPerFrameMax 0` | 0 over 900 frames (all stations, and stations 6 and 7 on their own), 0 pool misses |
| rpg gate | unchanged | `rpg --start explore` 600 frames, autoplay to battle 1500 frames, full autoplay 4000 frames (0 before frame 2307): all 0 allocs/frame; `perf_check.py` passes |
| Builds | `-Werror` GCC, Clang, MinGW | clean; ctest 10/10 on GCC and Clang (532 doctest cases) |

**Bench** (`physics_bench`, 600 steps per case, mean / p95 per fixed step, GCC, single thread). These were measured on a shared 4-core VM with a load average of 9–11 while they ran, so they are upper bounds, not bench-machine numbers:

| Case | Budget mean / p95 | Release (3 runs) mean / p95 | RelWithDebInfo (3 runs) mean / p95 |
|---|---|---|---|
| `reference` (500 bodies + 2 chains, P1) | ≤ 2.0 / ≤ 4.0 ms | 1.44–1.58 / 2.02–2.41 ms | 1.39–1.47 / 2.02–2.21 ms |
| `ragdoll` (500 bodies + 2 ragdolls) | ≤ 2.0 / ≤ 4.0 ms | 1.87–2.00 / 2.47–2.76 ms | 1.87–2.09 / 2.50–3.19 ms |
| `battle` (2 KO ragdolls + 50 debris) | ≤ 1.5 / ≤ 3.0 ms | 0.29–0.35 / 0.48–0.62 ms | 0.32–0.37 / 0.48–0.67 ms |

The `ragdoll` mean sits **at** the 2.0 ms budget on this VM: it is within budget in Release, and over it in one of three RelWithDebInfo runs (2.09 ms). It has not been measured on an idle bench machine. The hashes match between Release and RelWithDebInfo: `reference` `99611397fdbfa1ef`, `ragdoll` `2b1f65c2a341a819`, `battle` `a8a3ccf3d13b4cb4`.

The ragdoll case costs about 0.3–0.4 ms over the reference case. Most of that is the 24 position iterations the ragdoll parts get. Lowering them is the first lever if the budget tightens. All three cases make 0 allocations, have 0 pool misses and are deterministic (two runs per process).

### 4.4 P3: soft bodies, vehicles, water

Scope:

- **Cloth (rest of F11):** banners, crests and capes as Jolt soft bodies pinned to skinned joints, colliding against the character's ragdoll capsules, with LOD (cloth sleeps off-screen) and wind.
- **Hair:** Jolt 5.6's strand hair needs the compute backends we turned off. Choose either a CPU verlet strand system of our own (small, deterministic) or turning on `JPH_USE_CPU_COMPUTE` after a MinGW check. Decide with a spike at P3 start.
- **The tram (F14):** a kinematic body following a spline, with the tram bells; riders get platform velocity; props on board stay put.
- **Water (F15):** buoyancy volumes (Sensor layer) with `ApplyBuoyancyImpulse`; wading drag on the controller.
- **The Unwritten's page turns:** a small soft-body sheet or an authored blend, whichever reads better.

Extra gates for P3:

- cloth budget ≤ 0.5 ms for 4 on-screen capes and banners;
- soft-body determinism: the same hash check, including soft bodies;
- the soft-body vertex buffers are created at load (0 allocations).

Feel moments delivered: the cloth and hair half of F11, F14, F15.

### 4.5 Summary

| Phase | Delivers | Main risk | Gate beyond the common set |
|---|---|---|---|
| P1 | Jolt in the build, `PhysicsWorld`, ECS sync, interpolation, physics_demo, determinism test | Allocation pool coverage; Release perf numbers | Golden hash on 3 compilers; 0 allocs in demo |
| P2 | Ragdolls, joints, push/carry, knockback, hit-stop, destructibles, camera, contact audio/VFX | Controller feel parity; ragdoll stability | Controller parity tests; ragdoll soak; rpg marks unchanged |
| P3 | Cloth, hair, tram, water, page turns | Soft-body cost; hair compute on MinGW | Cloth ≤ 0.5 ms; soft-body determinism |

---

## Appendix: open questions

1. `OBJECT_LAYER_BITS` 16 (8+8) or 32 (16+16): decide in P1 from the real layer list.
2. Run the MinGW golden hash under Wine in local_ci.sh? The spike shows it works.
3. Offer upstream a patch so the QuadTree rebuild uses the `TempAllocator`; keep our pool regardless.
4. If `CROSS_PLATFORM_DETERMINISTIC` costs too much on the bench machine, fall back to same-binary determinism (enough for autoplay and replays).
