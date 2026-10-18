# Rigging and faces: tools, runtime, what is measured

The plan, with the criteria (R1–R16 body, F1–F16 face), is `docs/rigging_plan.md`. This page covers what P1a
built, how to run it, and which numbers have actually been measured. Blender authoring rules are in
`docs/blender.md` §5.

**Status: P1a is tooling, not gate-complete.** The face gate's F1, F2 and F4 have **NOT** been measured
against the plan's hand-marked gold set: no hand-marked TextGrids and no recorded VO exist yet, so every
F1/F2/F4 number below is against a machine reference (espeak's own phoneme events) on TTS audio. F3 is
measured and still fails on holds (§7). Body ROM has not run on a real body (P1b).

**P1b (faces) added:**
- expressions that are distinct at full and at half weight, with a doctest (§3);
- GPU morph targets in the skinning shaders, matched against the CPU reference (§6);
- a speaking proof head in `dialogue_demo --face` (§6);
- a minimum-hold step in the lip-sync bake (§7).

**P1c (faces in every world style) added:** face roles on materials and face fields in the style packs, so
expressions and speech read in every world, with a measured readability check and contact sheet
(§6, "Faces in every world style").

## 1. Running Blender headless

Every tool under `tools/blender/` runs on the `bpy` wheel (Blender 5.0.1 as a Python module). The wheel needs
Python 3.11. `uv` provides both without a system install:

```
uv run --offline --python 3.11 --with bpy==5.0.1 python -I tools/blender/<script>.py [args]
```

Drop `--offline` the first time so uv can fetch the wheel (about 400 MB, cached in `~/.cache/uv`). `-I` keeps
the working directory off `sys.path`. Each script adds its own directory to the path, so the shared modules
(`rig_spec`, `face_spec`, `rig_common`, `ne_blender`) resolve.

**Rendering.** Only Cycles on the CPU renders in this build. Workbench and EEVEE need a GPU context, which the
headless module does not have. The contact sheets therefore use Cycles at 16 samples with no denoiser, about
1 s per cell at 192 px.

**Rigify.** Rigify loads headless, but `bpy` must be imported first. A factory reset unloads add-ons, so enable
Rigify after any reset:

```
import bpy, addon_utils
addon_utils.enable("rigify", default_set=True)
```

## 2. Files

| File | What it is |
|---|---|
| `assets/rig/canonical_humanoid.json` | Canonical skeleton: 87 joints in a fixed order, rest transforms, x-axis hints, the ROM poses, thresholds, hinge list, translation-key joints. Written from `tools/blender/rig_spec.py`. |
| `assets/rig/face_presets.json` | Face shapes: ARKit 52 plus the house shapes (56 total). Also 15 visemes (with tongue angles), 14 expressions, one corrective, and the layer of each shape. Written from `tools/blender/face_spec.py`. |
| `assets/models/rig/proof_head.glb` | Proof head on the canonical skeleton, with all 56 shapes, a blink clip and `rom`. Kept small (0.74 MB). |
| `assets/models/rig/proof_head.expected.json` | Blender's evaluated shapes and skinning cases, used by `tests/test_morph.cpp`. |
| `assets/models/rig/proof_head.rigexpect.json` | Joints and sample vertices at rest and at 12 `rom` keys, used by `ne_rig_check`. |
| `tools/blender/make_rig_template.py` | Writes all of the above. `--blend X` also saves the template `.blend`, which is not committed. |
| `tools/blender/rig_validate.py` | Validator: body and face checks, ROM pass, contact sheets, optional export plus rigexpect. |
| `tools/blender/fit_canonical.py` | Path 1: fit the canonical skeleton to an unrigged mesh from landmarks, then auto-weight it and run the post-pass. |
| `tools/blender/bake_to_canonical.py` | Path 3: bake a control-rig (Rigify) character onto the canonical skeleton. |
| `tools/blender/rig_fit.py` | Landmark → joint fit. Pure Python, no bpy; `python3 rig_fit.py` runs its self-check. |
| `tools/blender/retarget_maps/rigify.json` | Mapping from Rigify DEF/ORG bones to canonical joints. |
| `tools/lipsync/bake_face_tracks.py` | VO line → `.nface` viseme track, plus F1–F4 scoring. |
| `tools/lipsync/gold_set.json` | The 20-line gold set. |
| `tools/ne_rig_check/` | Engine round-trip check (R10, R13) of a rigged `.glb` against Blender. |
| `engine/include/ne/geometry/Morph.h` | CPU reference for morph and skinning: `applyMorphs`, `skinPositions`. |
| `engine/include/ne/anim/FacePlayer.h` | Shape set, presets, `composeFace` (§7.3), `.nface` I/O, `FacePlayer`. |

### Regenerating the assets

```
uv run --offline --python 3.11 --with bpy==5.0.1 python -I tools/blender/make_rig_template.py --out assets \
    --blend /tmp/template.blend
uv run ... tools/blender/rig_validate.py /tmp/template.blend --template assets/rig/canonical_humanoid.json \
    --out /tmp/report [--no-sheets] [--export /tmp/report/template.glb]
```

## 3. The validator (`rig_validate.py`)

Each check prints PASS, FAIL, WARN or SKIP. A SKIP always states its reason. The report goes to
`<out>/<asset>.rigreport.json`.

- **Body, static**
  - `R12.order`, `R12.parents`, `R12.hinges`: the hinge axis uses the asset's own `ne_x_hints` if present,
    orthogonalised against the actual bone direction.
  - `R9`, `R15.joints`, `R15.bake`, `R11.transforms`, `R11.height`, `R16.soles`, `R16.root`.
  - `MOD.preserve_volume`, `MOD.others`.
  - `R7`, `R8`, `R6`.
  - `ACT.start`, `ACT.face_joints`, `ACT.translation`.
- **Face, static**
  - `F11.names`, `F11.normals`, `F15`: shapes, entries per vertex, texels.
- **Face, dynamic**
  - `F12`: lips–teeth, lips–lips and lids–eyes penetration, over every viseme, expression and layered case.
  - `F13`.
- **ROM**
  - `ROM.stretch`, `ROM.flips`, `ROM.self_intersection`.
  - Depth is measured relative to the bind pose, so contact that already exists at rest does not count.
- **Sheets**
  - `_visemes.png`, `_expressions.png`, `_expressions_closeup.png` (every expression at its full preset
    weights and at 50 %, 512 px per head) and `_turntable.png`.
  - Before posing, the validator stashes the head's shape-key action (`face_blink` on the proof head). Left
    in place, the action overrode `eyeBlink` on every frame, so expressions with tense lids rendered with open eyes. A body ROM sheet is P1b: the ROM pass measures the poses
    but does not draw them yet.

**Always SKIP for now:**
- `ROM.volume`: segment volume between cut planes is not implemented. It needs a closed limb mesh, meaning the
  pilot body, which P1a does not have.
- `R11.height` and `R16.soles` on a head-only asset.
- `F11.normals` on a `.blend`: it checks exported normals and runs only on `.glb` input.

**Template result.** Proof head with the template armature: every check PASSES except those SKIPs.
- F12: all three pairs at 0.000 mm over 295 cases.
- F15: 56 shapes, at most 25 entries per vertex, 15,847 entries (31,694 texels).
- Expression shapes, max displacement on the 23.6 cm head (floors asserted in `test_morph`): browInnerUp
  13.2 mm, browOuterUp 11.8, browDown 11.1, cheekSquint 7.4 (it also lifts the lower lid), eyeSquint 4.3,
  eyeWide 5.5, noseSneer 4.8, eyeBlink 6.7, mouthSmile 12.7, mouthFrown 9.8.
- **Expression distinctness** (`test_morph`, "the 14 expressions are pairwise distinct"). For every pair of
  expressions it measures the mean vertex distance over the face-active vertices (those any expression shape
  moves), using the full preset weights and again at 50 %. Closest pair: neutral / anxious, 0.65 mm at 100 %
  and 0.33 mm at 50 %. The thresholds are 0.45 and 0.22 mm, about 70 % of those values, so a regression that
  collapses two presets fails. The metric is geometric, not perceptual. On the close-up sheet at 50 %, warm,
  wry, proud, sad, anxious, angry, contempt and cold are still subtle.
- Hinges: worst 0.012°.
- ROM: stretch 0.05%, 0 flips, 0 new overlaps.

## 4. Fitting a mesh: `fit_canonical.py` (path 1)

```
uv run ... tools/blender/fit_canonical.py MESH.(glb|obj|fbx|blend) --landmarks lm.json --out x.blend \
    [--export x.glb] [--height 1.75] [--no-symmetrize] [--no-mirror-weights]
uv run ... tools/blender/fit_canonical.py MESH --landmarks-from other_rig.glb --map map.json --out x.blend
uv run ... tools/blender/fit_canonical.py --selftest --out DIR
```

`lm.json` lists 14 required landmarks in metres, Blender axes: `pelvis`, `neck_01`, and `upperarm`,
`lowerarm`, `hand`, `thigh`, `calf`, `foot` for each of `_l` and `_r`. Any other canonical joint may also be
given.

**Fitting the joints.** Each joint gets a similarity transform from an anchor segment:
- One end is the joint itself or its nearest landmarked ancestor.
- The other end is the first landmark in its subtree.
- Tails and roll hints go through the same transform.
- The root sits on the floor under the pelvis.

**Weights.**
1. Heat weights (`ARMATURE_AUTO`). Sockets, helpers, tongue and eyes are left out. Vertices heat leaves empty
   fall back to the nearest bone.
2. The post-pass (`rig_common.weight_post_pass`):
   1. twist-band redistribution;
   2. mirror average;
   3. prune below 0.01;
   4. symmetric limit-4;
   5. normalise.

**Selftest.** A capsule mannequin re-proportioned from the template, with ±4 mm landmark noise.
- Joint error vs truth: 7.05 mm. Heat fallback: 0 vertices.
- Every static check PASSES: R6 0.0000, R7, R8, R11, R16, hinges 0.014°.
- `ne_rig_check` PASSES:
  - R10: 7.2e-7;
  - R13: 0.0014 mm and 0.0001°;
  - vertices: 0.0015 mm.
- The ROM checks **FAIL** on this mannequin: stretch 350%, 3,167 flips, overlaps up to 52 mm. It is built from
  separate, overlapping capsules, which tear and interpenetrate at every joint. Body ROM thresholds have not
  been exercised on a real body; that needs the pilot body (P1b).

## 5. Baking a control rig: `bake_to_canonical.py` (path 3)

```
uv run ... tools/blender/bake_to_canonical.py SOURCE.blend --out x.blend [--export x.glb] \
    [--map tools/blender/retarget_maps/rigify.json] [--rig RIG] [--actions a,b] [--mirror-weights]
uv run ... tools/blender/bake_to_canonical.py --selftest [--mirror-weights] --out DIR
```

**Fitting.** The canonical skeleton is fitted to the mapped deform-bone heads.

**Following.** Each canonical joint follows its mapped bone rigidly:

    M_joint(f) = M_bone(f) · M_bone(rest)⁻¹ · M_joint(rest)

Linear blend skinning only sees `M · M_rest⁻¹`, so a vertex weighted to a 1:1-mapped bone deforms exactly as
before, wherever the joint sits.

**Weights.** Unmapped deform groups (face detail, palms, breasts, pelvis wings) are routed to the nearest
mapped ancestor. Then the post-pass runs (prune, limit 4, normalise), without twist re-banding and **without
the mirror average**: a bake must reproduce the authored deformation, and mirroring changes it wherever the
source weights are not exactly symmetric. `--mirror-weights` opts in. The bake then evaluates every frame of
every baked clip with and without the mirror pass and **fails** if mirroring moves any vertex more than
0.1 mm, naming the mesh, clip, frame and vertex.

**Keys.**
- Actions are baked every frame with linear keys.
- Rotation is keyed on every joint. Translation is keyed only on `root` and `pelvis`.
- Face joints are keyed in a body clip only if that clip moves them.
- Scale and stretch are dropped.
- Control-rig actions, widgets and the rig itself are removed.

**Selftest.** Rigify human metarig, a capsule mannequin heat-weighted to the DEF bones, and an FK clip on the
controls. Vertex distance against Rigify's own deformation over 5 frames, 3,630 vertices:

| Reference | No mirror (default): max / p95 | `--mirror-weights` (measured before the flip) |
|---|---|---|
| limited to 4 influences, 1-segment bones (the bake alone) | **0.001 / 0.001 mm** | 50.4 / 0.75 mm |
| limited to 4, Rigify B-bones as authored | 32.3 / 4.2 mm | 50.4 / 5.6 mm |
| heat weights as authored, B-bones as authored | 101.7 / 23.2 mm | 101.7 / 23.2 mm |

`--selftest --mirror-weights` now refuses: "moves vertex 3556 of 'mannequin' by 50.373 mm on clip
'rigify_fk_test' frame 24 (limit 0.1 mm)".

How to read the table:
- With identical weights the bake is exact to 1 µm.
- The rest is the expected loss:
  - the 4-influence limit: up to 102 mm, because heat weights on overlapping capsules reach across the legs;
  - Rigify's B-bone curvature, which the engine does not have;
  - (only with `--mirror-weights`) the mirror pass, which is why it is opt-in and bounded at 0.1 mm.
- A real character weighted symmetrically within the budget loses only the B-bone term.

Validator on the bake output (default, no mirror): every static check PASSES, including `ACT.face_joints`
and `ACT.translation`, **except R6, which FAILS** (worst mirrored-pair weight difference 0.355, limit 0.02):
heat weights on the mannequin are asymmetric and the bake now keeps them. Symmetric source weights are the
author's job; mirror only when the source is already within 0.1 mm (`--mirror-weights` checks it). ROM FAILS
for the capsule reason above. `ne_rig_check` PASSES: R10 1.2e-6, R13 0.0017 mm and 0.0000°, vertices
0.0024 mm.

## 6. Engine runtime

**Loader.** `loadGltf` reads morph targets: sparse accessors, names from `mesh.extras.targetNames`, default
weights and NORMAL deltas. Morph-weight animation channels (glTF path `weights`) load as `MorphWeightChannel`s of the clip (`sampleWeights`).
`geometry::applyMorphs` and `skinPositions` form the CPU reference and fallback for the GPU path below.

**GPU morph targets (P1b).**
- **Upload.** `render::Mesh` uploads a skinned mesh's sparse morph deltas (at most `kMaxMorphTargets` = 64
  targets) to an RGBA32F buffer texture, two texels (position delta, normal delta) per entry. Each vertex gets
  a `(first, count)` integer attribute pointing at its entries.
- **Shaders.** The skinned vertex shaders of the shadow, lit and hull passes add the weighted deltas before
  skinning. Weights arrive in a fixed `uniform vec4 uMorphWeights[16]`, so per frame there is no buffer
  upload and no allocation.
- **Platform.** The shaders use GLSL 330 on a GL 3.3 core context, with no SSBOs or compute, so the path is
  within macOS GL 4.1. It has not been run on a Mac.
- **Missing pieces.** A mesh without morphs binds a 1-texel dummy texture. `geometry::deriveMorphNormals`
  derives normal deltas for targets that ship without them.
- **API.** `Renderer::submitMorphed(mesh, model, material, joints, weights)` draws a morphed mesh.
  `Renderer::captureMorphSkin` runs the same morph and skinning code under transform feedback and reads the
  positions and normals back. It is a test and debug tool that compiles a program and allocates.
- **GPU vs CPU, measured** with `dialogue_demo --gpu-morph-check` (ctest `dialogue_face_gpu`; Mesa llvmpipe
  under xvfb):
  - 10 poses: 5 expressions, 4 visemes, and every shape at 0.35 with a 14° head turn, with jaw and skinning;
  - 42,990 vertex samples, worst position 0.000243 mm (float precision at 1.7 m), worst normal 0.0000°;
  - pass limits 0.01 mm and 0.1°; the poses move vertices by 1.7–14 mm.
  - Not measured on a hardware GPU driver or on macOS.

**Speaking head: `dialogue_demo --face`.** The proof head speaks gold line SC-P2-01#005: Duy, "It's math. I'm
calculating how much of your confidence is load-bearing.", deadpan, with the emphasis on "load-bearing".
- **Lip-sync.** The track is the MFA bake `assets/rig/lines/5dc7c14b9fad_duy.nface`, played by `FacePlayer`.
- **Expressions.** Cold at intensity 4 from the start of the line, turning wry (4.5) at 2.9 s, on "your confidence".
- **Blinks** are procedural.
- **Gaze.** The eyes look away and down from 1.15 to 2.2 s, with lid follow through the `eyeLook*` shapes,
  plus small saccades.
- **Head.** It sways slightly, nods at the start of the line and on "load-bearing" (3.73 s), and rolls a
  little while the eyes look away.
- **Subtitle.** It reveals whole words at their MFA word times. The line loops every 7.4 s.
- **Flags.** `--style <pack>` applies a world style pack, for example `inksolhaven`. The usual `--hidden`,
  `--no-vsync`, `--fixed-dt`, `--frames`, `--screenshot` and `--perf-json` flags work.
- **Allocations.** ctest `dialogue_face_speak` runs 300 frames and requires `allocsPerFrameMax` 0.
- **What it shows.** The wry turn reads clearly in every style pack, including `inksolhaven`. Before P1c,
  the hatching washed over the brows, lids and mouth under `inksolhaven` (open question 6 in the plan). See
  "Faces in every world style" below.

### Faces in every world style (P1c)

The ruling (plan §5.4, §14 Q5): every speaking character keeps a full 3D morph face in every world. A drawn
look comes only from StylePack shading, never from flattening the face. P1c makes that hold for the hatching
packs.

**Why faces washed out.** The hatch shading (`docs/style_packs.md` "Hatching") replaces a surface with a
paper base, `albedo / luma(albedo)`. That normalises a dark brow or a pink lip to paper-bright, and leaves
its value to pen strokes that are far coarser than the feature. Under the Unwritten, or `inksolhaven`, the
brows, lids and lips went the colour of the skin, and expressions were carried only by the outline.

**The fix.**
- **Face roles.** `Material::faceRole` is `None` (default), `Skin` or `Feature`. The app tags its face
  materials; the engine never guesses from names. `dialogue_demo` tags the proof head by its material
  names: `ph_skin` is Skin, and every other material (lips, brows, sclera, iris, pupil, teeth, tongue,
  mouth interior) is a Feature. It warns if no `*skin` material exists.
- **Style fields** (`StyleSettings`, identity by default, so packs without them draw faces as before):
  - `faceFeatures` (Solhaven 1, Unwritten 1): features keep their own lit value. No paper base, no strokes.
  - `faceContrast` (Unwritten 1): feature value pushed apart, `× (luma(albedo) / 0.4)^c` (clamped to 1.25).
    Brows and the open mouth go towards ink, lips to a mid grey, teeth and eye whites to paper. It is
    scaled by `faceFeatures × hatch`.
  - `faceHatch` 0.6 and `faceHatchMaxLevel` 2 (Unwritten): face skin is hatched sparser, one stroke
    direction at most, so strokes do not swamp the features.
  - `faceSkinLit` 0.3 (Unwritten): the skin's paper base keeps 0.3 of its lit shading. Half of a smile is
    cheek shading, which a flat paper base erases.
  - `faceSnap` (0 in every pack): morph weights snap to n steps (`snapFaceWeight`), for held-drawing faces.
    It is applied by the app (`dialogue_demo` honours it) and is off by default.
- **Blends.** `lerp` and `styleFromDial` weigh the face fields by each side's `hatch` as well as by blend
  weight. A third of the Unwritten carries a third of its hatching. With plain weights it also carried a
  third of the face treatment, and the dial `L1 U1 R1` measured 0.54. Weighted by hatch, it keeps the full
  Unwritten face treatment (0.87). Every face field acts through the hatch, so blends stay continuous.
- **Unchanged elsewhere.** Only materials with a face role are affected, so nothing else a pack draws
  changes. The `post_demo` screenshots of every pack, the dials and a blend are identical before and after
  (`compare -metric AE` = 0; see the commit report). The lit shader gains one int and one vec4 and one
  float uniform. No allocations are added: `dialogue_face_speak` still requires 0 per frame.

**The readability check: `dialogue_demo --face-read-check` (ctest `dialogue_face_read`).**
- **Looks (rows).** default, solhaven, lattice, unwritten, restored, inksolhaven, dial S2 U2, dial L1 U1 R1.
- **Faces (columns).** neutral, warm, sad, grief, angry, afraid, wry, cold (intensity 5), plus three frames
  of the speaking line (cold at 4, the MFA track at 0.62 s, 1.53 s and 1.95 s). Blinks are off and every
  pose starts from a fresh `FacePlayer`, so a pose is the same in every row.
- **Mask.** The neutral head rendered on black with no style and no post, thresholded and eroded by
  max(2, H/135) px, so the silhouette edge (which every pack outlines differently) does not count.
- **Metric.** Per 4×4-pixel block (12 of its 16 pixels inside the mask), the mean |luma difference| from the
  row's own neutral. The row's noise floor (neutral rendered again at the end of the row) is subtracted. The
  ratio is a face's change in a look divided by the same face's change in the default look.
- **Pass.** Every ratio is at least the threshold, `--face-read-threshold` (default 0.70). Written with
  `--face-sheet PATH`: a contact sheet (rows = looks, columns = faces, cells cropped to the head).

**Measured** (worst ratio per look, with its column; Mesa llvmpipe under xvfb; the noise floor was 0.0 in every row; "before" is the parent commit under the same check):

| Look | Before P1c, 960×540 | After, 640×360 (ctest) | After, 960×540 |
|---|---|---|---|
| default | 1.00 | 1.00 | 1.00 |
| solhaven | 0.94 | 0.92 (wry) | 0.91 (wry) | |
| lattice | 1.04 | 1.06 (wry) | 1.04 (wry) | |
| unwritten | **0.22** | **0.76 (warm)** | **0.77 (warm)** | |
| restored | 0.84 | 0.85 (sad) | 0.84 (angry) | |
| inksolhaven | **0.26** | 0.85 (warm) | 0.86 (warm) | |
| dial S2 U2 | **0.31** | 0.78 (warm) | 0.80 (warm) | |
| dial L1 U1 R1 | **0.51** | 0.87 (warm) | 0.86 (warm) | |

- **Why 0.70.** The check is there to catch the wash-out, which measured 0.22–0.51. The best that the
  paper-skinned worlds reach is about 0.76–0.80, so 0.70 leaves headroom for driver noise while still
  catching a 2.5× regression. Parity with a lit face (1.0) is not the goal: in a hatched world the skin is
  meant to be paper.
- **`warm` is the floor column in the paper-skinned looks.** A smile is half cheek shading, and paper
  flattens it by design. A sweep showed that `faceHatch`, `faceHatchMaxLevel` and `faceContrast` hardly move
  it (0.69–0.76); only `faceSkinLit` does (0.71 → 0.76 at 0.3). Higher values start to read as smooth grey
  skin instead of hatched paper.
- **Contact sheets** (in the commit report, not in the repo): before and after, all looks × all faces.
- **Not measured:** a hardware GPU driver, and how the screen-locked strokes look on a moving, talking face
  (the "shower door", `docs/style_packs.md`). Object-space hatching remains deferred.

**Importer facts, both checked by doctests:**
- **E1.** `JOINTS_1`/`WEIGHTS_1` are dropped and the remaining set-0 weights are renormalised. The exporter must
  limit to 4.
- **E2.** A vertex whose weights are all zero binds with weight 1 to the **first joint listed in its own
  `JOINTS_0` slot**, not to the skeleton's first joint. The plan's audit had this wrong. R7 makes it unreachable
  for validated assets.

**Morph accuracy, measured by `test_morph`.** CPU morph vs Blender's evaluation of the proof head, over 77 cases:
- morph only: worst 0.000354 mm;
- morph plus skinning: worst 0.000302 mm.

The tolerance is 0.1 mm.

**`FacePlayer`.**
- Inputs: one expression with crossfade (intensity 0–5), an optional `.nface` track, procedural blinks from a
  seeded xorshift (deterministic), and a micro layer.
- `composeFace` applies the §7.3 layering. A doctest compares it against `face_spec.layer_weights`.
- Outputs: one weight per shape, `jawDeg()` and `tongueDeg()`, `applyJoints` (rotation about the joint's local
  +X from rest) and `writeMeshWeights` through a name map built once by `bindMesh`.
- After construction, `setExpression`, `play`, `update`, `setMicro` and the output calls make **zero
  allocations**. A doctest checks this with the allocation hook.

**`ne_rig_check`.**

```
ne_rig_check model.glb [--expect model.rigexpect.json] [--json r.json] [--quiet]
```

- R10: `world · inverseBind = I` at rest.
- R13: joints and CPU-skinned sample vertices vs Blender at the expectation's key frames.
- Only key frames are compared. Between keys the engine uses nlerp and Blender uses slerp. The `rig` profile
  samples every frame, so in-game the difference is below a frame's motion.
- It runs in ctest as `ne_rig_check_proof_head`. On the proof head: R10 4.8e-7, R13 0.0012 mm and under
  0.0001°, vertices 0.0010 mm.

**The `rig` export profile.** `ne_blender.export_rig` / `RIG_PROFILE`:
- shape keys kept (`export_apply=False`), with morph normals and sparse targets;
- deform bones only;
- every frame sampled;
- 4 influences;
- armature exported at its rest position.

## 7. Lip-sync bake (`tools/lipsync/bake_face_tracks.py`)

**Pipeline.**
1. Phones come from a pluggable backend.
2. They map to the 15 visemes.
3. A dominance-blend coarticulation solver turns them into about 20 mouth, jaw and tongue channels at 60 Hz.
4. The result is stored as a u8 `.nface` plus a sidecar JSON.

The subcommands are `gold`, `synth`, `align-mfa`, `align-rhubarb`, `bake` and `eval`; usage is in the script
header.

**Gold set.** 20 lines from the Draft 56 VO table, including `CHILD SC-P0-01#001` and
`UNKNOWN VOICE SC-P0-01#002`. No VO has been recorded, so the audio is **espeak-ng TTS**.

**The reference is a machine reference.** espeak's own phoneme events for the audio it synthesised. The plan's
F1/F2/F4 gate asks for hand-marked TextGrids, which do not exist yet. The `textgrid` backend reads them once
someone marks them. Two consequences:
- The `espeak` row measures the solver given perfect timing.
- The `mfa` and `rhubarb` rows measure aligner plus solver against espeak's timing, on synthetic speech.

**Metric definitions.**
- **F1 onset:** the first frame where the mouth is closer to the new viseme's preset than to the previous one's.
- **F3 holds:** a frame's displayed viseme is its dominant class (nearest preset), and a hold is a run of one
  class lasting at least 2 frames. A shorter interior run is a **flash** (A-B-A, a pop), a **missed hold**
  (the class is the viseme of a reference phone within ±1 frame, so that phone's viseme showed for one frame),
  or **transit** (a class the mouth passes through on the way from A to C that is no nearby phone's viseme).
  Violations are flash + missed. Transit is not a hold, so it is not counted: with |Δw| ≤ 0.35 per frame and
  presets 0.15–0.4 apart, a blend A → C has to spend a frame inside some third preset's nearest region, and
  counting those frames would make the hold rule unsatisfiable under F3's own velocity limit. The first
  version counted every 1-frame run, transit included, which gave 92 / 91 / 119.
- **F4:** the rounding lead is at least 100 ms before a rounded vowel; phones shorter than 50 ms are reduced but not dropped.
- **`tongueOut`:** used only by TH, at 0.45. The tongue joints carry the rest of the tongue articulation.

**Results.** 20 lines, 3,431 frames. These come from `eval --report`; the report is not committed.

| Backend | F1 med / p95 | F2 in-class | bilabials closed | labiodentals | F3 dw/d²w viol | F3 hold viol (flash + missed) / transit | F4 rounding | F4 short |
|---|---|---|---|---|---|---|---|---|
| espeak (oracle) | 1.0 / 9.6 ms | 93.2% | 33/33 | 14/14 | 0 / 0 | 1 (0 + 1) / 82 | 10/10 | 60/61 |
| MFA | 15.0 / 71.4 ms | 81.2% | 27/33 | 13/14 | 0 / 0 | 1 (0 + 1) / 81 | 8/10 | 40/61 |
| Rhubarb | 37.7 / 117.7 ms | 39.3% | 4/33 | 2/14 | 0 / 0 | 9 (0 + 9) / 110 | 4/10 | 5/58 |
| ElevenLabs | not run: no API key / no alignment JSON | | | | | | | |
| hand TextGrid | not run: no hand-marked data | | | | | | | |

**Measured against the plan's gate:**
- **F1, F2, F4: NOT measured against the plan's hand-marked gold set.** The gate needs hand-marked
  references on recorded VO, which do not exist. The numbers above are against a machine reference on TTS
  and say nothing about whether the gate passes.
- **F3: measured, and it still FAILS on every backend, by fewer holds.** It does not need a reference.
  Velocity and acceleration: 0 violations everywhere (max |Δw| 0.21, closures 0.27, max |Δ²w| 0.24 / 0.24 /
  0.16). Holds: 1 / 1 / 9 violations (espeak / MFA / Rhubarb), down from 11 / 4 / 11 before the minimum-hold
  step; F1, F2 and F4 did not change (F2 espeak 93.1 → 93.2%).
  - **Minimum-hold step** (`enforce_hold` in the bake, after the limiter). A one-frame run of a viseme that one
    of the backend's own segments asks for (a 17 ms /d/, 43–66 ms /t k l z/, an E between two ih) is extended
    into the neighbouring frame closer to it, by blending that frame toward it. Short phones are reinforced, not
    dropped, so F4 holds. A one-frame A-B-A pop that no segment asks for is blended toward its neighbours. A
    repair is kept only if every channel stays inside the F3 velocity and acceleration limits with a 0.012
    quantisation margin, no closure floor is lost, and nearby hold violations go down. Transit frames are left alone.
  - **What is left** (traced through the repair):
    - **espeak:** one /ah/ squeezed between an /m/ closure and a /w/. Every blend step that shows it for a second frame
      pushes `mouthClose`'s second difference to 0.26–0.40, above the 0.238 the margin allows.
    - **MFA:** one kk frame. The reference puts that /g/ at 2.91 s, but MFA aligns it at 2.96–3.02 s after a
      pause, so in MFA's own segments the frame is transit and the bake has nothing to repair.
    - **Rhubarb:** 9 transit frames (kk, RR) within ±1 frame of a /k g ng r/ in the espeak reference, where no
      Rhubarb cue asks for those visemes.
    - The MFA and Rhubarb leftovers are aligner timing disagreeing with the reference. The bake only knows the
      backend's own segments, so it cannot repair them.
- **F12, F15: measured** on the proof head (section 3).
- **F5–F10, F13, F14, F16: not measured.** The engine GPU path exists now (§6), but they need perception reviews or the
  pilot.

**Third-party tools.** MFA, Rhubarb and the `espeakng_loader` wheel were installed into a scratch venv outside
the repo, as untrusted downloads. None are committed or needed to build the engine.
