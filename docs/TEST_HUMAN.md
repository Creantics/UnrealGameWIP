# Test human

`Data/Models/test_human.glb` is a full-body stand-in on the game's canonical skeleton. It lets the body side of the pipeline be tested before the real characters exist: import, skinning, clips, IK Rig and retargeting. [RIGGING_TEST.md](RIGGING_TEST.md) covers the same for proof_head, which is a head on that skeleton.

![test_human, three-quarter view](img/test_human.png)

## What it is

- **Size:** a 1.82 m adult in the canonical A-pose, with the soles at z = 0.
- **Mesh:** 20,814 vertices and 20,608 faces, built from tubes and ellipsoids in six materials (skin, shirt, shorts, hair, eye, shoe).
  - It has a torso, neck, head, jaw, nose, brow, ears, eyes and a hair cap.
  - It has arms, palms, five fingers per hand, legs and shoes.
  - It has no face morphs.
- **Skeleton:** the 87 joints of `rig_spec.py`, in the same order and with the same names as proof_head, including the face bones (`jaw`, `tongue_01..03`, `eye_l`, `eye_r`).
- **Weights:** procedural.
  - Each tube's weights blend smoothly along its bone chain.
  - `rig_common.weight_post_pass` adds the twist bands, mirrors left to right, prunes, and limits each vertex to 4 influences.
  - Result: 0 unweighted vertices, 0 unpaired mirror vertices, at most 3 influences per vertex.
- **Clips:** three, all with LINEAR keys. UE 5.8 Interchange imports CUBICSPLINE glTF clips empty; see RIGGING_TEST.md.

| Clip | Frames (30 fps) | Content |
|---|---|---|
| `idle` | 60 | arms lowered from the A-pose, breathing |
| `walk` | 32 | an in-place walk cycle with a pelvis bob |
| `rom` | 440 | the standard range-of-motion sweep from `rig_common.make_rom_action` |

## Rebuild it

Blender runs headless:

```
"C:\Program Files\Blender Foundation\Blender 5.0\blender.exe" -b --factory-startup --python-exit-code 1 ^
    --python tools/blender/make_test_human.py -- --out Data/Models/test_human.glb [--blend <path>.blend]
```

- The script checks its own output and fails if any of these is wrong:
  - the joints differ from `rig_spec.JOINT_NAMES`
  - the clips are not exactly `idle`, `rom` and `walk`
  - any sampler is not LINEAR
- The output is deterministic: running it again gives a byte-identical file.
- To get preview renders, run `tools/blender/ft_preview.py` on the glb.
- To check the rig, run `tools/blender/rig_validate.py` on the saved `.blend` with `--template <newengine>/assets/rig/canonical_humanoid.json`.
  - All static checks pass, including mirror symmetry (worst difference 0) and the action checks.
  - The face-shape check is skipped because the model has no morphs.
  - At the extreme ROM poses, three checks fail: stretch, triangle flips and self-intersection (about 21 mm deep). This is expected for a capsule-built body with procedural weights. The ordinary poses (walk, knee at 140 degrees, shoulder forward, fist) deform cleanly. A sculpted character with painted weights will need these checks to pass.

Then import it and build its rig, with no editor clicks:

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTImportCharacter -Source=Data/Models/test_human.glb ^
    -Dest=/Game/Characters/TestHuman -Face=0 -NullRHI -unattended -nopause -nosplash
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildRig ^
    -Mesh=/Game/Characters/TestHuman/test_human/SkeletalMeshes/test_human.test_human ^
    -Dest=/Game/Characters/TestHuman/Rig ^
    -RetargetFrom=/Game/Characters/ProofHead/Rig/IK_proof_head.IK_proof_head ^
    -Clips=/Game/Characters/ProofHead/proof_head/SkeletalMeshes/proof_headrom.proof_headrom ^
    -NullRHI -unattended -nopause -nosplash
```

- `-Face=0` is new.
  - It skips the 56-face-shape check, which a body without face morphs can't pass.
  - The face bone check still runs.
  - The default is unchanged: the check stays on.
- `-RetargetFrom=` and `-Clips=` are new on FTBuildRig.
  - Together they write `RTG_<source>_to_<mesh>`, using the default op stack and chains mapped by exact name.
  - They retarget the listed clips into `<Dest>/Retargeted/` and log each result.
  - The run exits with 1 if a target chain is unmapped or a result clip is empty.

## Results (2026-10-18, UE 5.8)

- **Import:** 0 errors.
  - It brought in the mesh, skeleton, physics asset, 6 materials and 3 clips under `Content/Characters/TestHuman/test_human/`.
  - Rig check: 7/7 face bones, 87 bones.
- **Clip check:** every clip has frames and moving tracks.
  - `idle`: 60 frames, 12 tracks move.
  - `walk`: 32 frames, 15 tracks move.
  - `rom`: 440 frames, 60 tracks move.
- **IK Rig `IK_test_human`:**
  - Auto-characterized from the UE4 Mannequin template: 48 bones matched, score 0.95, nothing missing.
  - Retarget root `pelvis`.
  - The same 21 chains as `IK_proof_head`, all valid, with IK goals on the hands and feet.
  - `RTG_test_human` is source-only, like `RTG_proof_head`.
- **Retarget proof_head `rom` to test_human:**
  - `RTG_proof_head_to_test_human` has 5 ops, and 21/21 target chains are mapped.
  - The result, `Rig/Retargeted/proof_headrom_to_test_human`, has 440 frames and 87 bone tracks, of which 50 move.
  - The commandlet ran with 0 errors.
  - The only warning is that proof_head's rom has no float curves to carry over.
  - The UE5 Mannequin isn't installed, so it was not a target.

## One character from a head and a body

proof_head and test_human have the same 87-joint skeleton with the same names and bind pose. A character can therefore be put together from a separately made head and body without retargeting:

- Make the body mesh the **leader**.
- Make the head mesh a **follower** that copies its pose: `SetLeaderPoseComponent` on the head's skeletal mesh component, or a Copy Pose From Mesh node in the head's AnimBP.
- The body's clips then drive the head's bones, and the face code keeps driving the head's morphs.

When a full character is modelled in one piece, nothing changes on the Unreal side. It imports, rigs and retargets as above, and only `-Face=0` is dropped.
