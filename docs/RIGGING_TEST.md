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

## 1. Blender export

Use the newengine Rigify character and export to FBX (or glTF), set as follows:
- **Deform bones only.** Export the `DEF-` bones and leave out the `ORG-`, `MCH-` and control bones. Flatten B-Bones to single bones first, since Unreal skins with plain bones.
- **Bind pose is the A-pose** (newengine `docs/rigging_plan.md` §4): upper arms 45° down, elbows bent 10°. Apply all transforms. Scale 1.0, with +Z up.
- **Shape keys on.** The face mesh carries the ARKit-52 names plus the house shapes (`mouthSmileNoCheek`, `lipsTogetherSoft`, `eyeLidTension`, `jawOpen_x_mouthSmile`), spelled exactly as in `face_presets.json`. Anything spelled differently is skipped (there is a log line per unmatched morph).
- **Face bones:** `jaw`, then `tongue_01`, `tongue_02` and `tongue_03` under it, and `eye_l` and `eye_r` under `head`.
- Leave out normal deltas on the face shapes (proxy-normal region), per the toon-normals note in rigging_plan §5.

## 2. Import into Unreal

These steps need the interactive editor, at the keyboard.

1. Drag the file into `Content/Characters/<Name>/`.
2. Import settings:
   - Skeletal Mesh on, a new skeleton, and **Import Morph Targets on**.
   - Import Animations off.
   - Normals: Import Normals and Tangents.
   - Use T0 As Reference Pose off (the file's bind pose is the A-pose).
3. Check that the asset's Morph Target preview lists the shapes by their ARKit names.

## 3. Put it in the demo map

1. Open `Demo` (the editor opens it at startup).
2. Select **CharacterPreview**. In Details, set **Body → Skeletal Mesh Asset** to the imported mesh.
3. Rotate the actor if the character doesn't face the `PlayerStart` (+X).
4. Save the map. This is the one hand edit. Then click Play.

## What should move

- **Body:** the A-pose stands still and nothing explodes. Look for skinning errors at the shoulders, elbows, hips and knees.
- **Face:**
  - Every 2 s it crossfades over 0.4 s to the next preset: neutral, warm, amused, wry, proud, hopeful, sad, grief, afraid, anxious, angry, contempt, stunned, cold.
  - It blinks every 2 to 6 s.
  - Check that the brows, lids, cheeks and mouth corners read as each expression's name. Also check that the lids close fully on a blink and that nothing tears at the mouth corners.
- **The output log** has a line for each face shape that has no matching morph target. Expect none.
- **The jaw and tongue move.** `Body` uses the C++ anim class `UFTFaceAnimInstance` by default, so no Anim Blueprint is needed. It rotates the `jaw` and `tongue_01..03` bones from `UFTFaceComponent::GetJawRotation()` / `GetTongueRotation()`, in local space on top of the rest pose. A bone the skeleton lacks is skipped, with one warning in the log ("Face anim: bone '...' not found"). Next after this test: the IK Rig and Retargeter from the A-pose skeleton.

## Shaders

- The cook (`-run=Cook -TargetPlatform=Windows`) compiles the game's shaders for the demo map and stages `Data/`.
- The editor still compiles its own shader set the first time it opens interactively, which takes a few minutes on this PC. Later opens use the DDC.
