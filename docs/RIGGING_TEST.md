# First rigging test

The goal is to bring one character into the demo map, see its body skin correctly in the bind pose, and watch its face morph targets move through every expression preset.

## What is already in place

- **The `Demo` map** (`Content/Maps/Demo.umap`) is built by code:
  `UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildDemoWorld -NullRHI -unattended -nopause -nosplash`.
  - It contains a floor, a sun, a sky light, sky atmosphere, height fog, a `PlayerStart` 2.5 m in front of the origin, and a **`CharacterPreview`** actor at the origin.
  - It is the game's and the editor's default map.
  - Don't edit it by hand. Change `FTBuildDemoWorldCommandlet.cpp` and run the commandlet again (it overwrites the map).
- **`AFTCharacterPreview`** holds a skeletal mesh component, `Body`, and a `UFTFaceComponent` bound to it.
  - On play, it cycles the 14 expression presets in `Data/Rig/face_presets.json`, holding each one for 2 s.
  - Its `Preview` settings control this: cycle on/off, seconds per expression, intensity, and the list of expressions.
- **The IK Rig and Control Rig plugins** are enabled for the retargeting step that follows this test.

## 1. The test model: proof_head

The first test uses `Data/Models/proof_head.glb`, the newengine proof head (from `assets/models/rig/`).
- It has a full deform skeleton (87 bones), including `head`, `jaw`, `tongue_01..03`, `eye_l` and `eye_r`.
- Its 56 morph targets are named exactly as in `face_presets.json`.
- No Blender step is needed for it.

## 2. Import and rig check (no editor clicks)

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTImportCharacter -Source=Data/Models/proof_head.glb -Dest=/Game/Characters/ProofHead -NullRHI -unattended -nopause -nosplash
```

- It imports through Interchange and saves the assets under `Content/Characters/ProofHead/proof_head/`: the mesh, skeleton, physics asset, materials and four clips (`face_jaw`, `rom`, `tpose`, `face_blink`).
- Then it checks the rig and logs `N/7 face bones` and `N/56 face shapes bound`. It exits with 1 if a face bone or a shape's morph is missing.
- The first run gave 7/7 bones and 56/56 shapes. The log also shows harmless "glTF sampler corrupt" warnings for the clips.

## 3. Put it in the demo map

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildDemoWorld -Mesh=/Game/Characters/ProofHead/proof_head/SkeletalMeshes/proof_head.proof_head -NullRHI -unattended -nopause -nosplash
```

- `-Mesh=` sets `CharacterPreview`'s `Body` mesh.
- It also turns the actor so that the eyes face the `PlayerStart` (+X). The yaw comes from where the eye bones sit relative to the head bone, which was -90° for proof_head.
- Then open the editor (it opens `Demo`) and click Play.

## Later characters (Blender export)

For the newengine Rigify characters, export to glTF (or FBX) and run the same two commandlets. Export settings:
- **Deform bones only.** Export the `DEF-` bones and leave out the `ORG-`, `MCH-` and control bones. Flatten B-Bones to single bones first, since Unreal skins with plain bones.
- **Bind pose is the A-pose** (newengine `docs/rigging_plan.md` §4): upper arms 45° down, elbows bent 10°. Apply all transforms. Scale 1.0, with +Z up.
- **Shape keys on.** The face mesh carries the ARKit-52 names plus the house shapes (`mouthSmileNoCheek`, `lipsTogetherSoft`, `eyeLidTension`, `jawOpen_x_mouthSmile`), spelled exactly as in `face_presets.json`. The import commandlet names any that are missing.
- **Face bones:** `jaw`, then `tongue_01`, `tongue_02` and `tongue_03` under it, and `eye_l` and `eye_r` under `head`.
- Leave out normal deltas on the face shapes (proxy-normal region), per the toon-normals note in rigging_plan §5.

## Result (2026-10-18, proof_head)

Passed: the expressions cycled, the lids blinked and the jaw moved in Play. The body and the tongue were not judged yet.

## What should move

- **Body:** the A-pose stands still and nothing explodes. Look for skinning errors at the shoulders, elbows, hips and knees.
- **Face:**
  - Every 2 s it crossfades over 0.4 s to the next preset: neutral, warm, amused, wry, proud, hopeful, sad, grief, afraid, anxious, angry, contempt, stunned, cold.
  - It blinks every 2 to 6 s.
  - Check that the brows, lids, cheeks and mouth corners read as each expression's name. Also check that the lids close fully on a blink and that nothing tears at the mouth corners.
- **The output log** has a line for each face shape that has no matching morph target. Expect none.
- **The jaw and tongue move.** `Body` uses the C++ anim class `UFTFaceAnimInstance` by default, so no Anim Blueprint is needed. It rotates the `jaw` and `tongue_01..03` bones from `UFTFaceComponent::GetJawRotation()` / `GetTongueRotation()`, in local space on top of the rest pose. A bone the skeleton lacks is skipped, with one warning in the log ("Face anim: bone '...' not found"). The IK Rig and Retargeter came next; see below.

## IK Rig and Retargeter (2026-10-18)

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildRig -Mesh=/Game/Characters/ProofHead/proof_head/SkeletalMeshes/proof_head.proof_head -Dest=/Game/Characters/ProofHead/Rig -NullRHI -unattended -nopause -nosplash
```

Both arguments are optional; these are the defaults. Rerunning replaces both assets.

- **Clip check.** It first logs every AnimSequence in the mesh's folder: frames, bone tracks that move, tracks off the bind pose, and float curves that change. For proof_head:
  - `rom`: 440 frames, 14.67 s, 81 bone tracks, 60 of them move. Imported correctly.
  - `tpose`, `face_jaw`, `face_blink`: 1 frame, no tracks, no curves. **Empty.**
- **Why the three clips are empty.** The 83 "glTF sampler corrupt" import warnings are exactly the 83 CUBICSPLINE samplers in the file (tpose 81, face_jaw 1, face_blink 1); `rom` uses LINEAR keys and imports fine. UE 5.8's Interchange checks `Input.Count != 3 * Output.Count` for cubic splines (`Engine/Plugins/Interchange/Runtime/Source/Import/Private/Gltf/InterchangeGltfAnimation.cpp`, `ValidateChannelSampler`). The glTF 2.0 rule is the reverse: a cubic-spline output holds three values (in-tangent, value, out-tangent) per input key. The file follows the spec (2 keys -> 6 values, 3 -> 9, blink 3 keys x 56 morphs -> 504), so this is an engine bug and every spec-valid cubic channel is dropped. The fix is on the export side: export these clips with LINEAR interpolation (or sample them) in newengine, or collapse each cubic triple to its middle value in a small glb post-process step. The rig doesn't need these clips, since it is built from the mesh's reference pose (the A-pose).
- **IK Rig** `Content/Characters/ProofHead/Rig/IK_proof_head`: the auto-characterizer matched the UE4 Mannequin template (48 bones, score 0.95, none missing). Retarget root `pelvis`, root motion bone `root`. 21 chains, all valid:
  - `Spine` spine_01-03, `Neck` neck_01-02, `Head`
  - `LeftClavicle`/`RightClavicle`, `LeftArm`/`RightArm` upperarm -> hand (goals `LeftHandIK`/`RightHandIK`)
  - `Thumb`, `Index`, `Middle`, `Ring`, `Pinky` per side, `_01` -> `_03` (the `ring_00`/`pinky_00` metacarpals stay outside the chains)
  - `LeftLeg`/`RightLeg` thigh -> foot (goals `LeftFootIK`/`RightFootIK`), `LeftFoot`/`RightFoot` ball
  - It exits with 1 if the root isn't `pelvis` or any spine, neck, head, arm, leg or finger bone has no chain. A chain the template misses is added by hand from a fixed table (none were needed for proof_head).
- **Retargeter** `Content/Characters/ProofHead/Rig/RTG_proof_head`: source rig `IK_proof_head`, source preview mesh `proof_head`. **No target yet.** Engine/Content ships no mannequin; `SKM_Manny` and `IK_Mannequin` exist only inside experimental plugins (MoverExamples, MoverTests). proof_head uses the mannequin bone names, so once a mannequin (or the game's own body rig) is in the project, setting it as the target and calling `AutoMapChains` should map every chain by name.

## Shaders

- The cook (`-run=Cook -TargetPlatform=Windows`) compiles the game's shaders for the demo map.
- **`Data/` is staged with the packaged game (proven 2026-10-18).** `RunUAT BuildCookRun -platform=Win64 -clientconfig=Development -cook -stage -unattended -stagingdirectory=<dir>` gave BUILD SUCCESSFUL with 0 cook errors. In `<dir>\Windows\ForgottenThird\`, which is `FPaths::ProjectDir()` in the packaged game, these were staged as loose NonUFS files, and `Manifest_NonUFSFiles_Win64.txt` lists them:
  - `ForgottenThird/Data/Rig/face_presets.json`
  - `ForgottenThird/Data/Rig/emotion_map.json`
  - `ForgottenThird/Data/Rig/containment_profiles.json`
  - `ForgottenThird/Data/Models/proof_head.glb`
  - `ForgottenThird/Data/Story/...`: all 99 local files (gitignored, so only on machines that have the copy)
- `$(ProjectDir)/Data/...` is expanded when the **game target is built**, and the file list goes into `Binaries/Win64/ForgottenThird.target`. Staging reads that receipt. A new or renamed file under `Data/` only stages after the game target is built again (drop `-skipbuild`).
- A fresh worktree has Git LFS pointer stubs in `Content/`, and the cook fails with "Invalid value for PACKAGE_FILE_TAG". Run `git lfs pull` first.
- The editor still compiles its own shader set the first time it opens interactively, which takes a few minutes on this PC. Later opens use the DDC.
