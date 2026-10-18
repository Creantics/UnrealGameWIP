# Rigging plan: bodies that bend right, faces that act

Status: plan (research, decision, phases). Nothing in the engine has changed yet.
Owner: engine + art pipeline. Readers: engine and tools programmers, the art owner, design.

The brief: rig every model "perfectly", and, at higher priority, "mouth movement and facial expressions. If our characters lack these, the emotion is lost throughout everything." The 2,328 directed VO lines already carry emotion, intensity, pace and volume (`design/forgotten_third/voice/line_direction_schema.md`); the face system turns those columns into a performance.

"Perfect" means measurable criteria: R1–R16 for the body (§2), F1–F16 for the face (§3). A headless validator checks them (§10); no asset enters `assets/` without a PASS.

Evidence labels: **[verified]** read in this repo or run in this session; **[documented]** a tool's own docs, licence or vendor page, not reproduced; **[secondary]** third-party source; **[estimate]** judgement.

---

## 0. Decisions at a glance

| Question | Decision | Section |
|---|---|---|
| Face technique | **Morph targets** (a 52-shape facial-action set plus tongue and jaw bones) for every speaking named character, in every world; a world's look comes from StylePack shading, never from flattening the face (Q5). **2D decal faces** (atlas swap) for the Gathered, the Staff, crowds and the Third's porcelain oval | §5 |
| Visemes | 15 visemes, authored as **presets over the 52 shapes** (no extra sculpts unless the validator asks for a corrective) | §5.2 |
| Lip-sync source | **Forced alignment** of VO wav + `spoken_text` with Montreal Forced Aligner, run offline at build time. Rhubarb Lip Sync is the fallback when alignment fails. An RMS envelope covers unbaked lines at runtime | §6 |
| Expression | Runtime mapping from the direction columns to expression poses, layered by face region with the baked mouth track | §7 |
| GPU morphs | **Texture buffer (TBO), sparse per-vertex delta lists**, applied before skinning in the lit, depth and hull shaders | §8.1 |
| Sync | Face time = the voice's **audio playback cursor**, not frame time (a new `SoundHandle` cursor) | §8.3 |
| Body skinning | **Stay LBS.** Fix candy-wrapper twist with twist bones and elbow/knee volume with helper bones. No DQS | §4, §11 |
| Skeleton | One canonical humanoid of about 90 joints (128 max), with snake_case `_l`/`_r` names, an A-pose bind, and Rigify used only as a control rig that is baked onto it | §4 |
| Tools per path | Unrigged mesh → **AccuRIG for landmarks, then fit the canonical template** (UniRig for creatures). Foreign rig → **Auto-Rig Pro Full retarget** plus a rename table. Own rig → **Rigify + bake script** | §9 |
| Gate | `tools/blender/rig_validate.py` (headless bpy) + `ne_rig_check` (engine round-trip, no GL) → `*.rigreport.json`, pass/fail | §10 |

---

## 1. Engine facts (audit, read in this session)

Line references are as of commit `719a7bc`; `Renderer.cpp` and `Mesh.cpp` are being edited by other agents, so their lines may drift.

| # | Fact | Where | Consequence |
|---|---|---|---|
| E1 | Only attribute set 0 is read. `JOINTS_1`/`WEIGHTS_1` are **silently dropped**, and the remaining 4 weights are renormalised | `Gltf.cpp:381`, `:441` | Max 4 influences; the validator rejects a 5th |
| E2 | A vertex with zero total weight is bound to joint 0 with weight 1, **with no warning** | `Gltf.cpp:442` | The validator must reject unweighted vertices |
| E3 | `inverseBindMatrices` are read when present, otherwise identity (spec default). Joints are re-sorted parent-first and the IBMs follow the permutation | `Gltf.cpp:299–320` | Bind must equal rest; `tests/test_blender_assets.cpp:245–251` already checks `world × IBM = I` |
| E4 | **Morph targets are not read at all**, and `weights` animation channels are dropped with a warning | `Gltf.h:137`, `Animation.h:14`, `Gltf.cpp:511` | The face is an engine item (§8) |
| E5 | Hard bone budget: **128 joints per skin** (`std140 mat4[128]`). Extra joints are ignored with a warning, and indices are clamped to 127 in GLSL | `Renderer.h:146`, `Renderer.cpp:33,283,322,663` | Canonical skeleton ≤ 128 including face and secondary bones |
| E6 | Skinning is **linear blend (LBS)**: the 4 matrices are weighted and summed. Normals use `mat3(skin)` with no inverse-transpose | `Renderer.cpp:34–36,52` | Twist bones needed. Non-uniform scale skews normals, so scale keys on deform bones are rejected (except declared squash bones) |
| E7 | Attribute locations 0–5 are in use (pos, normal, uv, joints, weights, tangent) | `Mesh.cpp:30–54` | 10 of 16 free: too few for attribute morphs (§8.1) |
| E8 | Blending: crossfade, masked override layers and additive layers exist. **No IK, no retargeting, no root motion** ("Future work") | `Pose.h`, `AnimationPlayer.h:65` | Look-at and foot IK are engine items (§12) |
| E9 | A clip binds to joint **indices** of the skeleton in the same file (`channel.joint` is resolved by node at load time) | `Gltf.cpp:519–523`, `Pose.cpp:22–31` | Clips are shared only across identical joint order: P1 enforces an order hash; P3 adds name binding |
| E10 | The `AnimationPlayer` palette path allocates nothing. The convenience `computeSkinningMatrices(skeleton, pose, out)` **allocates per call**, and `sampleClip` reallocates on a size mismatch; neither runs per frame today | `Pose.cpp:125–129`, `Pose.cpp:22`, `AnimationPlayer.cpp:282` | Trap: face code uses the member-scratch overloads |
| E11 | `SoundHandle` has **no playback position**. `Music` has `clock()`/`audibleClock()` and the device reports `outputLatency()` | `AudioDevice.h:52–72,115`, `Music.h:13–57` | Audio-clock sync needs a cursor on voice plays (§8.3) |
| E12 | Story beats carry `id` (VO id) and `speaker`. The rpg `StoryState` **does not play VO yet**, and no direction columns reach the runtime | `StoryDatabase.h:79–80`, `games/rpg/StoryState.cpp` | New wiring (§8.4) |
| E13 | A per-draw `Material::baseUv` transform exists | `Renderer.h:76` | 2D decal faces need **no renderer change** |
| E14 | The exporter wrapper sets `export_influence_nb=4`, `export_morph=False`, `export_force_sampling=False`, `export_def_bones=False` | `tools/blender/ne_blender.py:274–294` | A `rig` export profile (§11) |
| E15 | Blender headless: the **`blender` binary is not installed**. **`bpy` 5.0.1 runs headless** from the uv cache (`uv run --offline --python 3.11 --with bpy==5.0.1 python -I`), and `rigify` and `io_scene_gltf2` are present [verified] | this session | The validator runs in CI with no Blender install |

The 21-bone test hero (`tools/blender/make_character.py`: `upper_arm.L`, T-pose) and the 12-joint sample (`tools/gen_rigged_sample.py`) stay as test fixtures; neither is the canonical skeleton.

---

## 2. Body spec: what "perfect" means (R1–R16)

"ROM clip" is the canonical range-of-motion clip (§10.2), measured at the poses listed. "Stretch" is per-edge length change against bind, and "volume" is the signed volume of a closed limb segment between two cut planes. Measurement is in bpy with the Armature modifier's **Preserve Volume off**, so Blender evaluates the same LBS as the engine (E6).

| # | Criterion | Threshold | How it is measured | Phase |
|---|---|---|---|---|
| R1 | Elbow and knee at 140° flexion | Segment volume loss ≤ 10%. Inner-crease self-intersection ≤ 2 mm depth. Outer-point edge stretch ≤ 15% | ROM poses `elbow_140`, `knee_140`. Volume between cut planes 8 cm either side of the joint; BVH tri-tri test for intersection | P1 |
| R2 | Shoulder raise to 170° (abduction) and forward to 160° | No triangle flips (normal reversal) in the deltoid/armpit band. Volume loss ≤ 12%. Clavicle takes 20–35% of the raise | `shoulder_up`, `shoulder_fwd`. Face normals compared to bind; clavicle angle ratio | P1 |
| R3 | Forearm twist ±90°, upper arm twist ±60°, thigh twist ±45° | No candy-wrapper: wrist-ring cross-section area ≥ 85% of bind, and no ring self-intersection | `twist_*` poses; ring area at 3 cut planes along the forearm | P1 |
| R4 | Spine and neck bend: ±45° flexion/extension, ±30° side, ±45° twist (summed over the chain) | Torso volume loss ≤ 8%. No belly/back self-intersection. The neck seam never opens (shared verts) | `spine_*`, `neck_*` | P1 |
| R5 | Fingers: full fist and full spread | No finger-finger penetration > 1 mm in the spread. Fist knuckles keep ≥ 90% edge length. Thumb opposition reaches the index tip ≤ 5 mm | `fist`, `spread`, `oppose` | P1 |
| R6 | L/R weight symmetry | For each vertex with a mirror partner within 0.5 mm, \|w − w_mirror(bone_mirror)\| ≤ 0.02 for every bone | KD-tree mirror match on X; skipped for meshes tagged `ne_asymmetric` (§4.4) | P1 |
| R7 | No unweighted vertices; weights normalised | 0 vertices with Σw < 0.999. 0 weights < 0.01 (pruned) | Vertex groups of deform bones only | P1 |
| R8 | Influences per vertex | ≤ 4 after pruning; **hard fail at 5** (E1) | Count of non-zero deform groups | P1 |
| R9 | Bones per skin | ≤ 128 (E5); canonical humanoid ≤ 100, leaving room for secondary motion | glTF skin joint count | P1 |
| R10 | Bind = rest | Every joint's `world × IBM` = I within 1e-4 in the engine | `ne_rig_check` (§10.4), the same check as `test_blender_assets.cpp:245` | P1 |
| R11 | Unit scale, no negative scale, applied transforms | Armature and mesh objects: scale (1,1,1), rotation 0; scene unit scale 1.0; no negative determinant anywhere in the hierarchy; height within ±2% of the character sheet | bpy object and scene inspection | P1 |
| R12 | Canonical names, hierarchy, order and orientation | Joint names, parents and order match `canonical_humanoid.json` exactly (order hash). Hinge axes within 2° of spec (§4.2) | Compared to the template | P1 |
| R13 | Engine round-trip | Joint world matrices in the engine match Blender at rest and at 12 ROM frames within 1 mm / 0.1° | `ne_rig_check` vs `*.rigexpect.json` (§10.4) | P1 |
| R14 | Clip plays in-engine at 0 allocs | `skinned_demo --model <asset> --clip rom --fixed-dt 1/60 --frames 600 --perf-json` gives `allocsPerFrameMax 0` | Existing perf JSON (`docs/PERFORMANCE.md`) | P1 (needs the `--model` flag) |
| R15 | Deform-only export | No control or MCH bones in the skin. No constraints or drivers left unbaked (§11) | glTF joint names ⊆ canonical set + declared extras | P1 |
| R16 | Feet planted at rest | Both soles at y = 0 ± 2 mm. `root` at the origin; `pelvis` above it | Rest-pose bounds | P1 |

Thresholds are first estimates [estimate]; P1 calibrates them on the pilot (Devin) and freezes them in `canonical_humanoid.json`.

---

## 3. Face & Performance spec (F1–F16)

All face timing is at **60 Hz**, the game's fixed frame. Forced alignment uses a 10 ms frame shift [documented], so ±1 frame is reachable.

| # | Criterion | Threshold | How it is measured | Phase |
|---|---|---|---|---|
| F1 | Viseme timing | Mouth-shape onset vs hand-marked phone onset: median \|err\| ≤ 10 ms, p95 ≤ 16.7 ms (1 frame). Never late by more than 1 frame. Lead allowed up to 2 frames (anticipation) | 20-line gold set: hand-marked boundaries in Praat-style TextGrids, compared with the baked track | P1 |
| F2 | Viseme accuracy | ≥ 90% of frames inside a phone show that phone's viseme class (or its coarticulated neighbour); 100% of bilabials (/p b m/) reach `mouthClose ≥ 0.8` inside the phone; 100% of /f v/ reach `mouthRollLower ≥ 0.5` | Gold set, computed from the track | P1 |
| F3 | No popping | Per-channel \|Δw\| ≤ 0.35 per 60 Hz frame, except closures (≤ 0.6); second difference ≤ 0.25. Minimum viseme hold 2 frames | Scan of every baked track (all 2,328 lines) in CI | P1 |
| F4 | Coarticulation | Lip rounding for /uw ow w/ starts ≥ 100 ms before the vowel when the preceding consonants allow it; a viseme shorter than 50 ms is reduced, not dropped, unless it is a closure | Gold set | P1 |
| F5 | Jaw, lips, tongue | Jaw opens 0–25° (character-tunable), driven by the bone. Tongue tip reaches the teeth for /th/ and the alveolar ridge for /t d n l/. Teeth never pass through lips at any viseme | Viseme contact sheet + intersection test (F12) | P1 |
| F6 | Silence and breaths | Mouth returns to the expression's rest within 150 ms of line end. Breath tags (`[exhales]`, `[sighs]`) play their canned shape at the aligned time | Track inspection | P1 |
| F7 | Blinks | Baseline 12–20/min (character-tunable). Blink 100–150 ms with a fast close and slower open. Forced blink on gaze shifts > 20° and at `//` pauses ≥ 50% of the time. No blink lands on an emphasis word | 10-minute soak, event log | P1 |
| F8 | Saccades and look-at | Listening saccades of 2–8° at 1–3 Hz. Gaze reaches its target within 1–2 frames (saccade) plus head follow over 200–400 ms. Eye limits ±30° yaw, +20°/−25° pitch | Soak + clamp check | P1 (eyes), P2 (head) |
| F9 | Lid follow | Upper lid follows eye pitch with gain 0.6–0.8; no sclera gap above the iris at neutral | Contact sheet at ±20° pitch | P1 |
| F10 | Brows and cheeks | Every base expression (§7.1) at intensity 3 is recognisable: ≥ 4 of 5 reviewers name the right family from a still | Blind review of the expression contact sheet | P1 |
| F11 | Shape set complete and symmetric | All 52 shapes present with exact names. L/R pairs mirror within 0.5 mm per vertex. Each shape's max delta lies within its declared region mask | Validator (§10.1) | P1 |
| F12 | No intersection at extremes | Every shape at 1.0, and every viseme at 1.0, and each expression at intensity 5 combined with each viseme: lips do not cross each other or the teeth by > 0.5 mm; lids do not pass through the eyeball | Validator BVH tests | P1 |
| F13 | Layering never fights | For every (expression, viseme) pair, the combined weights stay ≤ 1 per shape after layering, and bilabial closures still reach 0.8 under any smile | Validator runs the §7.3 math on all pairs | P1 |
| F14 | Zero allocations | Face update + morph upload: `allocsPerFrameMax 0` in a dialogue scene with 6 faces | perf JSON | P2 (runtime) |
| F15 | GPU morph budget | ≤ 64 shapes per face mesh; ≤ 32 non-zero entries per vertex; ≤ 65,536 texels per face mesh (the GL 3.3 minimum `GL_MAX_TEXTURE_BUFFER_SIZE`); face pass ≤ 0.3 ms GPU for 6 faces at 1080p on the target GPU | Validator counts; GPU timer query | P1 counts, P2 timing |
| F16 | Audio sync at runtime | Face clock vs audible audio clock: \|err\| ≤ 1 frame across a 30 s line, including after a pause/resume | Instrumented `dialogue_demo` run | P2 |

---

## 4. Canonical skeleton

### 4.1 Humanoid joints (`canonical_humanoid.json`)

Names are lower snake_case with `_l`/`_r` suffixes. Blender's mirror tools recognise this suffix form, and the names do not depend on Rigify. Joint order is depth-first in the order below, so every character exports identical joint indices (E9).

| Group | Joints | Count |
|---|---|---|
| Core | `root` (ground, carries root motion later) → `pelvis` → `spine_01` → `spine_02` → `spine_03` → `neck_01` → `neck_02` → `head` | 8 |
| Face | `jaw` → `tongue_01` → `tongue_02` → `tongue_03`; `eye_l`, `eye_r` (children of `head`) | 6 |
| Arm (×2) | `clavicle` → `upperarm` → (`upperarm_twist_01`, `upperarm_twist_02`) → `lowerarm` → (`lowerarm_twist_01`, `lowerarm_twist_02`) → `hand` | 16 |
| Hand (×2) | `thumb_01..03`; `index_01..03`; `middle_01..03`; `ring_00` (metacarpal, for cupping) → `ring_01..03`; `pinky_00` → `pinky_01..03` | 34 |
| Leg (×2) | `thigh` → `thigh_twist_01` → `calf` → `calf_twist_01` → `foot` → `ball` | 12 |
| Helpers (×2) | `elbow_helper`, `knee_helper`, `deltoid_helper`, `glute_helper` (volume keepers, driven by joint rules, §11) | 8 |
| Sockets | `weapon_r`, `weapon_l`, `prop_back` (non-deforming; `findJoint` targets) | 3 |
| **Total** | | **87** |

That leaves 41 slots for secondary bones (hair, capes, coat tails: ≤ 30 per character by budget) and keeps a margin below 128.

### 4.2 Conventions

- **Bind pose: A-pose.** Upper arms 45° below horizontal, elbows bent 10°, palms facing down/in at 45°, legs straight with feet hip-width, toes forward. An A-pose deforms the shoulder better than a T-pose, and it is the pose the art pipeline already asks image-to-3D for (`docs/art_pipeline.md` §1.5). A stored `tpose` action supplies the T reference for retargeting.
- **Axes:** Blender bones point +Y down the chain. The roll is set so that **flexion is a positive rotation about local +X** on every hinge (elbow, knee, finger), mirrored on both sides. That makes mirrored clips and the validator's hinge checks simple.
- **Units and placement:** metres, the character faces Blender −Y (engine +Z), soles at z = 0, `root` at the origin, everything at scale 1 (R11, R16).
- **Twist split:** the forearm twist bones take 1/3 and 2/3 of the hand's twist, and the upper-arm twist bones take 1/3 and 2/3 of the upper arm's counter-twist. The thigh twist takes 1/2.

### 4.3 Proportion variants

One skeleton serves every humanoid, from Toby to Devin's 1.3× hands to the 12-year-olds. Clips are authored on Devin. Rotations transfer unchanged; translation is keyed only on `root`/`pelvis` and scaled by the pelvis-height ratio.

### 4.4 Non-humanoids and the Unwritten

- **Creatures and constructs** (28 enemy rows) get their own templates (`canonical_quadruped.json`, `canonical_construct.json`) with ROM clips and per-template thresholds.
- **The Third, true form:** a humanoid core plus a declared **extra-hands chain** (≤ 6 arm/hand rigs × 12 joints; the face is 2D, §5.4), about 128 total. Over budget, it splits into two skins on one hierarchy.
- **Distortion, uncanny and tender, never gore** (art direction D1): elongation via scale-free **length bones** (translation-keyed segment bones declared in the template), too-perfect posture via clips, and "a hand that runs into the tablecloth" via a shared mesh skinned to both. No splitting, wounds or exposed anatomy shapes are authored. The validator rejects any shape key or bone in an `ne_tone_forbidden` list.
- **Minors** (Pia, Lumi, Toby, the Child) are never distorted. Their rigs carry `ne_minor = 1`, which (a) forbids length bones and asymmetric shapes, (b) caps expression intensity at 4 (§7), and (c) is read by the physics ragdoll profile (`docs/physics_plan.md` §1 tone constraint).
- **Asymmetry is opt-in**: the Third's slightly asymmetric porcelain face and Granite's broken nose are tagged `ne_asymmetric` with the affected region, so R6/F11 skip only that region.

---

## 5. Face rig

### 5.1 Why morphs, not a bone face

A bone face avoids engine work (E4) but fails the brief: sealed lips for /m b p/, cheek raise in a real smile, an "oo" funnel and brow corrugation need surface shapes that 4-influence LBS cannot hold, and 40–60 face bones would eat half the 128 budget (E5). **Decision: morph targets, with bones only for rigid parts: jaw, tongue, eyes.** Morph support becomes a P1 engine item (§13).

### 5.2 The shape set

**52 facial-action shapes**, named in the de-facto camelCase convention used by face-capture tools (it lets us drive the rig from a phone capture app later without a rename table):

- Brows: `browDownLeft/Right`, `browInnerUp`, `browOuterUpLeft/Right`
- Eyes: `eyeBlink`, `eyeLookDown`, `eyeLookIn`, `eyeLookOut`, `eyeLookUp`, `eyeSquint`, `eyeWide` (each `Left/Right`)
- Cheeks/nose: `cheekPuff`, `cheekSquintLeft/Right`, `noseSneerLeft/Right`
- Jaw: `jawOpen` (corrective, follows the `jaw` bone), `jawForward`, `jawLeft`, `jawRight`
- Mouth: `mouthClose`, `mouthFunnel`, `mouthPucker`, `mouthLeft/Right`, `mouthSmile`, `mouthFrown`, `mouthDimple`, `mouthStretch`, `mouthPress`, `mouthLowerDown`, `mouthUpperUp` (each `Left/Right` where sided), `mouthRollLower`, `mouthRollUpper`, `mouthShrugLower`, `mouthShrugUpper`
- Tongue: `tongueOut` (the 53rd; the tongue is otherwise posed with bones)

Plus up to **11 house shapes** to reach the 64 cap (F15): `mouthSmileNoCheek` (the Third's "smile that does not reach the eyes"), `lipsTogetherSoft`, `eyeLidTension`, `jawOpen_x_mouthSmile` and other combination correctives, added only when F12/F13 fail.

**Visemes (15):** `sil`, `PP` (p b m), `FF` (f v), `TH`, `DD` (t d n l), `kk` (k g ng), `CH` (ch j sh zh), `SS` (s z), `RR` (r), `aa`, `E`, `ih`, `oh`, `ou`, `nn` (nasal hum, lips together soft). Each viseme is a **preset**: a sparse vector of shape weights plus jaw-bone and tongue-bone angles, kept in `face_presets.json`. Character variation (Duy's flat mouth, Devin's wide one) lives in the sculpted shapes, not in the presets, so the presets are shared by the whole cast. A character may override a preset entry if review demands it.

### 5.3 Eyes, lids, jaw

- **Eyes** are bones (`eye_l/r`), rotated by look-at, with spherical eyeballs and an iris offset for the webtoon look. The lids are morphs, driven by `eyeLookUp/Down` (lid follow, F9) and `eyeBlink`.
- **The jaw** is a bone: the `jawOpen` curve drives `jaw` rotation (0–25°) and the `jawOpen` corrective morph together. Teeth and tongue meshes are rigidly skinned to `head`/`jaw`/`tongue_*`, so they cannot drift through the lips (F12).

### 5.4 Stylisation per world

Characters cross worlds with the same mesh (art direction §4), so the face technique is chosen **per character**; the world changes only shading.

**Ruling (owner, Q5).** Every speaking named character has a full 3D morph face in every world. Without mouth movement and expressions the emotion is lost. Stylisation comes from StylePack shading (hatch, ink, rim, optionally snapped morph weights), never from flattening the face. 2D decals remain only for the faces in the last three rows.

| Who | Face | Why |
|---|---|---|
| Party and named cast (Devin, Duy, Mina, Torrin, Nil, Granite, Odette, the minors, every speaking named character) | **Full 3D morph face** in all worlds, Solhaven included | They carry the 2,328 lines. In Solhaven (heroic cel + hatch) and the Lattice (sharp webtoon cel) a sculpted face with proxy normals (below) gives clean cel shadows. In the Unwritten the same face is drawn in ink |
| The Gathered, the Staff | **2D decal face**: a face material with an atlas of eye and mouth cells, selected by `Material::baseUv` offset (E13). Mouths use a 9-cell mouth set (A–H + rest, the classic limited-animation set) mapped from the same viseme track | "Two lines and a mouth" (art direction §4). The **unison mouth** is one track driving many decals |
| The Third, true form | 2D decal on the porcelain oval, plus a few morphs for slight asymmetry when it "draws itself" | The art direction calls for a flat, drawn face; morphs would read as flesh |
| Crowds, minor NPCs | 2D decal or none | Budget |

**Toon normals.** Cel shading turns noisy face normals into shadow islands. Every 3D face gets **custom normals transferred from a smooth proxy** (head ellipsoid + nose wedge, Data Transfer modifier); the loader keeps them (`Gltf.cpp:419`). Face morphs carry **no NORMAL deltas** in the proxy region, so the cel terminator stays stable while the mouth moves; the jaw still rotates chin normals via skinning, and lips/mouth interior keep real deltas (a vertex mask the validator checks). Hatching on Unwritten faces is open (§14).

---

## 6. Lip-sync source

### 6.1 Options

| Tool | Licence / cost | Input → output | Notes |
|---|---|---|---|
| **Montreal Forced Aligner 3.4.1** | Code MIT [documented: LICENSE read]; English dictionary CC BY 4.0; model licence unconfirmed [secondary] | wav + transcript → phone intervals; headless CLI | **Uses our `spoken_text`**, so no recognition errors. Invented names go in a project dictionary |
| **Rhubarb Lip Sync** | MIT; output data "belongs to you alone" [documented: LICENSE.md read] | wav (+ dialog text) → 6–9 mouth cues | English, 2D-oriented, coarse but robust |
| **ElevenLabs `with-timestamps`** | Part of the TTS plan we already use | **Per-character** start/end times [documented] | Needs grapheme-to-phoneme; v3 support **not stated**; misses human-recorded lines and post edits |
| **WhisperX** | BSD-2-Clause [documented: LICENSE read] | Recognition + word alignment | Mishears invented names; only for lines without text |

### 6.2 Recommendation: bake once, offline

**Pipeline:** `VO wav + spoken_text → MFA → phones → coarticulation solver → mouth track → build asset`. `tools/lipsync/bake_face_tracks.py` runs at asset-build time, keyed by `vo_id`, else `text_hash + speaker` (`line_id` is unstable across drafts; `text_hash` changes exactly when a re-bake is needed).

1. **Align:** MFA with the project dictionary. If any word is < 30 ms, coverage is incomplete, or RMS outside words is > 20% of inside, fall back to **Rhubarb with the dialog text** and flag the line.
2. **Map** phones → the 15 visemes (a fixed table in `face_presets.json`).
3. **Coarticulate** (dominance blending): each viseme is a target with an attack/release of 40–80 ms. Closures (/p b m/) and labiodentals (/f v/) are hard constraints. Anticipatory rounding looks up to 150 ms ahead (F4). Volume scales the jaw aperture (§7.2). Output: about 20 mouth/jaw/tongue channels at 60 Hz, quantised to u8.
4. **Events:** word starts, emphasis words, `/` `//` pauses and nonverbal tags at their aligned times. An ElevenLabs alignment, where present, cross-checks words (> 50 ms disagreement flags the line).
5. **Write** `assets/vo/face/<key>.nface`: a binary header (version, 60 Hz, frame count, channel ids), u8 curves, the event list, and the resolved direction (emotion ids, intensity, volume, pace, contour). Estimated size: 2,328 lines × ~3 s × 60 × 20 B ≈ 8.4 MB [estimate].

The engine only samples curves. **Runtime fallback** for unbaked lines: `FacePlayer` reads the clip's PCM RMS in a 20 ms window at the audio cursor, driving `jawOpen` plus an `aa`/`E`/`oh` cycle at the syllable rate. It is allocation-free and plainly worse; a debug HUD marks such lines and CI counts them.

---

## 7. Expression from the VO direction

### 7.1 Emotion vocabulary → base poses

The schema's ~50 emotion words map to **14 base poses** (presets over the 52 shapes): `neutral`, `warm` (soft Duchenne smile), `amused`, `wry` (one-sided, no cheek), `proud`, `hopeful/eager` (lifted brows, open eyes), `sad`, `grief`, `afraid`, `anxious` (inner brow up, lip press), `angry`, `contempt` (one-sided sneer), `stunned` (wide eyes, slack jaw, no blink), `cold/hollow/uncanny` (neutral mouth, reduced blink, `mouthSmileNoCheek` for `uncanny`).

`emotion_map.json` gives each word a base, weight and modifier, e.g. `tender` = warm 0.7 + `browInnerUp` 0.3.

### 7.2 Direction fields → parameters

| Field | Drives |
|---|---|
| `emotion_primary` | Base pose, **all regions** |
| `emotion_secondary` | Base pose at 0.35, **upper face only** (brows, lids, cheeks), plus micro-expression flashes. What leaks through shows in the eyes |
| `intensity` 1–5 | Weight 0.2–1.0, then a per-character **containment profile** (Devin: cap 0.7 + `mouthPress`; Duy: cap 0.6, blink × 0.5; Odette: cap 0.5, still head). Above the cap, micro-expression rate rises instead. Minors cap at 4 |
| `volume` | Baked jaw aperture scale: whisper 0.5 … shout 1.3, plus `mouthStretch` at raised/shout |
| `pace` / `pace_wpm` | Blink rate × 0.8–1.2, blend time 400–150 ms, head-motion amplitude |
| `pitch_contour` | Brows: `rising` lifts, `falling` settles, `breaking` lid flutter, `clipped` `mouthPress` |
| `emphasis` | At each emphasis word's aligned time: a brow flick (120 ms) + a small head nod (an additive layer on `neck_02`) |
| `pauses` (`/`, `//`) | A preferred blink slot (F7); micro-expression slot for the secondary emotion |
| `breath_nonverbal` / tags | Canned face clips (sigh, laugh, swallow) at the aligned time |
| `hold_before_s` / `hold_after_s` | Listening face (§7.4) during holds |
| `subtext` | Not machine-read. It is the review note for the face pass |

### 7.3 Layering math

Each shape belongs to one region: **upper** (brows, lids, cheeks, nose), **eyes** (look, blink), **mouth-articulation** (jaw, close, funnel, pucker, roll, press, lower/upper lip, tongue) or **mouth-tone** (smile, frown, dimple, stretch, sneer, sideways). Per frame, with expression weights E (the eased current emotion pose × intensity), viseme weights V (sampled from the track), micro-expressions M, speech activity σ ∈ [0,1] (track envelope, 80 ms ease), and rounding ρ = max(V.funnel, V.pucker):

- **upper:** w = clamp(E + M, 0, 1). Additive: brows act freely while talking.
- **eyes:** `eyeBlink` = max(blink(t), E.blink); look shapes come from look-at.
- **mouth-articulation:** w = V + (1 − σ)·E. The **viseme overrides** while speaking, and the expression returns in silence. `jawOpen` = max(V.jaw, (1 − σ)·E.jaw).
- **mouth-tone:** w = E·(1 − 0.7·ρ)·(1 − 0.5·V.close). A smile shrinks under "oo" and under a closure. It does not fight them, and a smile still reads between syllables.
- **combination correctives:** w(corr_ab) = w(a)·w(b) (pose-space correction by product; cheap, in-engine).
- Final clamp to [0,1]. F13 checks this function over all expression × viseme pairs in the validator.

### 7.4 Listeners and idle faces

Non-speakers run a **listening state**: their last emotion decaying to rest (half-life 4 s) plus 0.3× the speaker's emotion mirrored (warmth and sadness mirror; contempt does not); gaze on the speaker with saccades and a 20% glance-away per phrase; a nod on `//` pauses when warm. Blinks are seeded per character, so autoplay is deterministic. With nobody speaking, an idle face breathes, blinks slowly and drifts gaze.

---

## 8. Engine runtime

### 8.1 Morphs on GL 3.3: the texture-buffer path

| Option | Verdict |
|---|---|
| Vertex attributes | 10 free locations (E7) ÷ 2 (pos + normal) = **5 shapes**. Fails F15 |
| CPU morph + streaming VBO | Re-uploads every face each frame; kept only as the **test reference** |
| **Texture buffer (`samplerBuffer`, core since GL 3.1)** | **Chosen.** One TBO per face mesh, **sparse per-vertex lists**: a `uvec2(offset, count)` attribute (location 6); each entry is two RGBA16F texels (Δpos + shape index, Δnormal); weights are a `float[64]` uniform. About 2.5k vertices × 10 shapes ≈ 50k texels, under the 65,536 minimum |

The morph runs **before skinning** in one GLSL block shared by the lit, depth and hull shaders, so shadows and outlines follow the mouth. Weights live in the per-frame pool (like `jointPool_`).

### 8.2 Loader and animation

- `Gltf.cpp`: read `targets` (POSITION/NORMAL deltas), the mesh's `extras.targetNames`, and default `weights`. Build the sparse lists at load time in `MeshData::morphs`. Load `weights` animation channels as `AnimationPath::Weights` (fixes E4).
- `anim::FacePlayer` (new, `ne/anim/FacePlayer.h`): owns `float weights[64]`, the eye/jaw/tongue joint overrides, a blink/saccade generator (a seeded deterministic RNG), the expression easing state and the current `.nface` track. `update(audioTime, dt)` writes the weights and then **overrides the face joints in the `Pose`** produced by `AnimationPlayer` (body clips never key face joints; the validator enforces this). Then `computeWorldMatrices` runs as usual. Everything is sized at setup, so per-frame work allocates nothing (F14).
- Emphasis nods use the existing **additive layer** of `AnimationPlayer` on a neck mask (`makeSubtreeMask(neck_01)`).

### 8.3 Sync to audio time

Add `SoundHandle::cursorSeconds()` (consumed frames ÷ rate, like `Music::clock()`) and `audibleSeconds() = cursor − outputLatency()`. `FacePlayer` samples at `audibleSeconds()`, holds when the voice pauses, and goes to `sil` if the voice is stolen. Frame time is never the clock, so a hitch cannot desync the mouth (F16).

### 8.4 Who triggers it

The rpg `StoryState`, on entering a `Line` beat (`StoryRunner::State::Line`):

1. Resolves `Beat.id` (VO id) → voice `Clip` + `.nface` track + resolved direction (both baked at import, keyed as in §6.2).
2. Calls `audio.playVoice(clip)` and keeps the `SoundHandle`.
3. Binds the track to the speaker entity's `FacePlayer` (speaker label → entity via the scene cast table) and sets every other cast member to listening, looking at the speaker.
4. On `advance()` or skip, stops the voice; the face eases to listening within 150 ms.

`dialogue_demo` gets this wiring first. `tools/story_import` joins the direction columns by `vo_id`/`text_hash` into `story.json` (E12).

---

## 9. Input paths and tools

### 9.1 Tools compared (checked 2026-10-17)

| Tool | Licence / cost | Scriptable / headless | Quality (for us) | LBS-friendliness |
|---|---|---|---|---|
| **Rigify** (bundled; Blender 5.x) | GPL, free [documented] | Yes: present in headless bpy 5.0.1 [verified]; generate is a bpy operator | Best humanoid control rig; DEF bones are parented under ORG/MCH, not game-friendly [documented] | Fine **after baking** onto our skeleton |
| **Auto-Rig Pro** 3.7x | Paid: Lite $25, **Full $50** (needed for export and retarget) [secondary]; Blender 2.93–5.1 [secondary] | bpy operators (add-on) | Good auto-placement; **best remap/retarget in Blender** | Deform-only game export |
| **AccuRIG 2** (Reallusion) | Free; commercial use allowed for your own content per a vendor forum reply [secondary]; Windows only, account needed | **No** (GUI) | Good joint placement incl. fingers [documented] | Landmarks only |
| **Mixamo** | Free with an Adobe ID; **maintenance mode, outages since 2025-06** [secondary] | No (web) | Adequate; big clip library | Clip source only |
| **UniRig** (VAST-AI) | **MIT** [documented: LICENSE read]; local, needs ≥ 8 GB VRAM (`art_pipeline.md` §3.4) | Yes (Python CLI) | Only option for creatures; weights need cleanup | Fitted to creature templates |
| **Meshy / Tripo auto-rig** | Per-call credits (`art_pipeline.md` §3.4) | API | Humanoid only | Landmarks only |

### 9.2 Recommendation per path

1. **Unrigged mesh (AI-generated or artist-made) → `fit_canonical.py`.** Run **AccuRIG** (humanoids) or **UniRig** (creatures) only to obtain joint **landmarks**. `fit_canonical.py` snaps our template to them (or to 14 manual clicks), sets rolls (§4.2), heat-map weights, then the **weight post-pass** (prune < 0.01, limit 4, normalise, mirror, smooth twist bands) and the validator. This revises `art_pipeline.md` §3.4: service skeletons are never shipped.
2. **Foreign rig (Mixamo-style, service rigs, library characters) → Auto-Rig Pro Full remap** to the canonical skeleton, driven by a checked-in bone map (`retarget_maps/<source>.json`). Vertex groups are renamed/split through the same map, then post-pass and validator. Without ARP: a homemade Copy-Rotation retarget + bake (slower, free).
3. **Our own authored rig → Rigify as the control rig + `bake_to_canonical.py`.** The canonical armature follows Rigify DEF bones via Copy Transforms (a name table; no `DEF-` names). Export bakes actions onto it (visual keying, every frame), strips constraints and exports only canonical armature + mesh. The Rigify file stays the source.

---

## 10. Validator and ROM test: the product

Two tools produce one `<asset>.rigreport.json` plus contact sheets. **Accepted only on PASS**; CI runs both on every character; a WARN needs a reviewer waiver.

### 10.1 `tools/blender/rig_validate.py`: static checks (headless bpy)

`uv run --offline --python 3.11 --with bpy==5.0.1 python -I tools/blender/rig_validate.py <file.blend|.glb> --template canonical_humanoid.json --out report/`

It checks R6–R9, R11, R12, R15 and R16 from object, armature and vertex-group data. It also checks:
- the Armature modifier's Preserve Volume is off;
- no unapplied modifiers except Armature;
- every action starts at frame 0;
- no body clip keys face joints;
- no translation keys outside `root`/`pelvis`/declared bones.

Face checks (F11, F12 static part, F15):
- **completeness**: the exact 52 names (+ declared house shapes);
- **symmetry**: each L/R pair mirrored within 0.5 mm;
- **region masks**: each shape's deltas stay inside its region;
- **normals**: no NORMAL deltas in the proxy-normal region;
- **budget**: the per-vertex entry count and TBO texel count.

### 10.2 ROM pass: dynamic checks (b)

The template's `rom` action (about 40 s) steps through every R1–R5 pose and a combined reach + twist + crouch. Per pose, on the evaluated LBS mesh:
- **per-vertex stretch**: max |edge length / bind − 1| per vertex, with a heat-map vertex colour for the sheet;
- **segment volume** between cut planes;
- **self-intersection** (`mathutils.bvhtree` overlap on the evaluated mesh, excluding adjacent faces).

The **face ROM** does the same for every shape at 1.0, every viseme, each base expression at intensity 5, and the expression × viseme grid through the §7.3 math (F12, F13).

### 10.3 Contact sheets (c)

Rendered headless with Workbench or EEVEE (no GPU needed for Workbench):
- **turntable**: 8 views at bind;
- **ROM sheet**: one tile per R pose, with stretch heat;
- **viseme sheet**: 15 visemes × front/¾;
- **expression sheet**: 14 bases × intensity 2/4, plus 4 combined with `aa`/`ou`/`PP`.

The sheets are the human review artefact (F10).

### 10.4 Engine round-trip (d): `ne_rig_check`

The validator exports with the rig profile (§11) and writes `<asset>.rigexpect.json` (joint matrices at rest and 12 ROM frames, 64 sample vertices; the `hero.expected.json` pattern). `ne_rig_check`, a new CLI linking `ne_asset` + `ne_anim` with **no GL**:
- checks R10 (`world × IBM = I`);
- samples `rom` through `sampleClip`, compares joint matrices (1 mm / 0.1°, R13) and the CPU-skinned sample vertices (1 mm);
- with morphs (P2), applies the CPU reference morph and compares the face vertices.

Then `skinned_demo --model` (new flag; `main.cpp:109` hard-codes the asset today) runs R14.

---

## 11. The bake rule

glTF carries no constraints, drivers or IK: each feature becomes keys/morphs at export, or engine code.

| Feature | Decision | Cost |
|---|---|---|
| Twist bones (authored clips) | **Bake** in Blender (export sampling on for constrained rigs; `export_force_sampling=True` in the rig profile) | Bigger clips (linear keys every frame) [estimate: about 2× for arm channels]. No engine cost |
| Twist + helper bones after runtime changes (IK, look-at, blends of clips with different twist) | **Engine "joint rules"** (P3): twist = a fraction of the swing-twist decomposition of a driver joint, helper = a fraction of a driver's swing. Data in the template JSON | About 12 rules per character, under 2 µs [estimate]. Allocation-free |
| Body corrective shapes (pose-space deformation) | **Engine** (P2, once morphs exist): weight = a remapped joint angle (e.g. elbow 90→140° → 0→1) from the template | A few multiplies; uses the same morph path |
| Face combination correctives | **Engine**: product of the two weights (§7.3) | Negligible |
| Blender IK (foot plants, hand contacts) | **Bake** into FK keys at export | None at runtime. Runtime IK is separate (§12) |
| DQS / Preserve Volume | **Not used**; the validator rejects it (it would differ from the engine) | Avoids a second skinning shader in locked files |
| Shape-key drivers (face rig UI) | **Bake** to `weights` channels for authored face clips (sigh, laugh); the live face is runtime (§8) | Loader must read `weights` channels (P1b) |

`ne_blender.py` gets a **`rig` export profile**: `export_morph=True`, `export_morph_normal=True` (the masked regions are zeroed by the validator first), `export_def_bones=True`, `export_force_sampling=True`, `export_influence_nb=4`. The existing profile stays as it is, so the sample assets and their tests do not change.

---

## 12. Animation side (brief)

- **Retargeting:** every clip enters through ARP remap (or the constraint retarget) in Blender, then is baked. Identical joint order (E9, R12) lets every canonical clip play on every humanoid; P3 adds a **name-bound clip table** built at load, for skeletons with extra bones.
- **Foot IK and look-at:** two-bone analytic IK on thigh/calf/foot with physics ground probes (P3). Look-at chains `spine_03 → neck_01 → neck_02 → head` at 0.1/0.2/0.3/0.4 with clamps (P2, needed by listeners).
- **Ragdoll mapping** (`physics_plan.md` §3.8): one profile for every humanoid, 15 bodies: `pelvis`, `spine_02`, `spine_03`, `head`, `upperarm`, `lowerarm`, `hand`, `thigh`, `calf`, `foot` (sided ×2). Twist, helper, finger and face joints stay animated. `ne_minor` selects the tighter profile.

---

## 13. Phases and gates

### 13.1 Gates common to every phase

1. GCC, Clang and MinGW (`cmake/toolchains/mingw-w64.cmake`) build with `-Werror`.
2. `ctest` fully green.
3. **0 allocs per frame**: every demo touched reports `allocsPerFrameMax 0` with `--fixed-dt 1/60 --perf-json`.
4. **The rpg gate is unchanged**: `rpg_autoplay_full` with `allocsPerFrameMaxBeforeMark: 0` (0 allocs before frame 2307) in `bench/baseline.json` via `tools/ci/local_ci.sh`. Same marks, same frame counts.
5. Existing asset tests (`test_blender_assets.cpp`, `test_gltf.cpp`, `test_animation.cpp`) unchanged and green.
6. `docs/STATUS.md` row updated; `docs/blender.md` §5 updated to point here.

### 13.2 P1: canonical body + face spec + the gate

**P1a (no locked files; can start now):**
- `canonical_humanoid.json` (§4) and a script-built template (`make_rig_template.py`): canonical armature, aligned Rigify metarig, `rom`/`tpose` actions, the face shape-set **spec** (names, regions, 15 viseme and 14 expression presets) and a **proof head** with all 52 shapes.
- `rig_validate.py` (§10.1–10.3) with the body and face checks, and the report and contact sheets.
- `fit_canonical.py`, `bake_to_canonical.py`, the `rig` export profile in `ne_blender.py`.
- **Engine-caps audit** (§1) pinned by doctests on E1/E2 behaviour.
- **Loader + anim side of morphs (no renderer):** read `targets`/`weights` into `MeshData`/`AnimationClip`; `anim::FacePlayer` with the CPU reference morph. **Proof:** a doctest morphs the proof head on the CPU and matches Blender's evaluated shapes from `proof_head.expected.json` within 0.1 mm (the `test_blender_assets.cpp` pattern).
- **Viseme pipeline proven:** `bake_face_tracks.py` on a **20-line gold set** (from `draft56_04a.csv`, incl. a minor's human line and an `UNKNOWN` speaker) meets F1–F4 against hand-marked TextGrids, F3 on every line with audio, with MFA, Rhubarb and ElevenLabs alignment reported side by side.
- `ne_rig_check` (§10.4).
- Docs: `docs/rigging.md` (artist how-to), `docs/blender.md` update.

**P1b (first PR after the art-direction and particles agents merge `Renderer.cpp`/`Mesh.cpp`):**
- the TBO morph path (§8.1) in the lit/depth/hull skinned shaders;
- `SoundHandle::cursorSeconds()`;
- `skinned_demo --model/--clip`;
- **`dialogue_demo` speaks**: one line with a baked track, a listener, blinks and look-at, on the proof head.

Extra P1 gates:
- the pilot character (Devin) passes all R and F11–F13, F15 counts;
- the gold set passes F1–F4;
- `dialogue_demo --autoplay` at 0 allocs with 2 faces;
- the GPU morph pass ≤ 0.3 ms for 6 faces measured on the bench GPU (F15).

### 13.3 P2: performance at scale + engine gaps

- Expression mapping and layering (§7) in `FacePlayer`; listeners; containment profiles; the emotion map reviewed against the expression sheet (F10).
- Story import of direction columns; rpg `StoryState` VO + face wiring (§8.4) behind a setting until all of Act I is baked.
- Body pose-space correctives (§11) on the morph path; head look-at.
- Face for the whole party; 2D decal faces for the Gathered/Staff.
- **>4 influences: not added.** The validator enforces 4, and nothing in the spec needs more [estimate]. Revisit only if R1–R4 fail on a hero after helpers and correctives.
- **DQS: not added** (§11).

Extra P2 gates:
- F7–F10, F14, F16 pass;
- the rpg gate is unchanged, both with the face setting off **and** on (the face path must not allocate before frame 2307);
- every Act I line is baked or reported as unbaked.

### 13.4 P3: batch retarget, mocap, runtime rules

- `retarget_batch.py`: library/mocap FBX → canonical clips with a per-clip report (foot slide ≤ 2 cm, joints within limits).
- Engine joint rules (§11), the name-bound clip table (§12), foot IK with physics ground probes, root motion.
- Creature templates and UniRig fitting for the 28 enemy rows.
- Optional: a phone face-capture import (the 52 names already match).

Extra P3 gates:
- 20 library clips play on 3 different humanoids with no per-character fixes;
- foot IK keeps soles within 1 cm of a 15° slope;
- 0 allocs with IK and joint rules on.

---

## 14. Open questions

1. **Auto-Rig Pro Full ($50)**: buy it for the retarget path, or accept the slower free constraint-retarget fallback?
2. **AccuRIG is Windows-only and GUI-only.** Is the art owner on Windows (the 3080 Ti box)? If not, use manual landmarks or UniRig for humanoids too.
3. **ElevenLabs v3 timestamps:** should someone run one test call to confirm `with-timestamps` works with `eleven_v3`? It is a cross-check only, not on the critical path.
4. **Face capture:** is a phone face-capture pass for hero lines wanted later? It would replace the procedural layer for marquee scenes; the shape names already allow it.
5. **Decal vs morph boundary: RESOLVED (owner + advisor).** Devin, the party and every speaking named character use full 3D morph faces in every world, including Solhaven. A drawn look comes from StylePack shading (hatch, ink, rim, optionally snapped morph weights), never from flattening the face. 2D decal faces stay limited to the Gathered, the Staff, crowds and the Third's true form.
6. **Unwritten faces under hatching: RESOLVED (P1c).** No mask: face materials carry a role (skin or feature), and the style packs' face fields keep features at their own value (no paper base, no strokes) and hatch the skin sparser, with some lit shading kept. Measured by `dialogue_demo --face-read-check` (ctest `dialogue_face_read`): every look keeps at least 0.70 of the default face change (it was 0.22 in the Unwritten and 0.26 in `inksolhaven`). See `docs/rigging.md` "Faces in every world style". Still open: the screen-locked strokes slide on a moving face ("shower door"); object-space hatching stays deferred.
7. **Distortion limits (art direction D1):** this plan assumes "uncanny and tender". If the writer chooses otherwise, the forbidden-shape list in §4.4 changes.
