# Lead characters

Three stylised stand-ins for the leads, Devin, Duy and Third, built in code on the game's canonical skeleton. They are blockouts in an anime/comic style, not final art. Each is one skinned mesh with:

- the full face rig: 56 face shapes and the 7 face bones
- the 87-joint skeleton from `rig_spec.py`
- `idle`, `walk` and `rom` clips

So they work everywhere proof_head and test_human do: import, IK Rig, retargeting, and the face code.

| Devin | Duy | Third |
|---|---|---|
| ![Devin](img/leads_devin.png) | ![Duy](img/leads_duy.png) | ![Third](img/leads_third.png) |

The Devin render shows the `amused` preset; the other two are neutral.

## What to open in the morning

1. **`Content/Maps/Leads.umap`.** The three stand side by side, each looping its idle clip:
   - Duy, Devin and Third, left to right.
   - Devin holds the `amused` face; the other two are neutral.
   - The map has a camera (`LeadsCamera`, 4.2 m out, 50 degree FOV) set to take over Player 0's view on Play, so Play shows all three. Without Play, pilot `LeadsCamera` in the viewport.
   - The project's default map is unchanged. Open this one from the Content Browser (Maps/Leads), or start the editor on it:

     ```
     UnrealEditor ForgottenThird.uproject /Game/Maps/Leads
     ```

2. **The characters.** Each is in `Content/Characters/{Devin,Duy,Third}/<name>/SkeletalMeshes/<name>`, with its clips `<name>idle`, `<name>walk` and `<name>rom` beside it.
3. **The rigs.** `Content/Characters/<Name>/Rig/IK_<name>` and `RTG_<name>`.

## The three

All three share the same head mesh and face rig. They differ in:

- how the lower face is warped
- facial hair
- hair
- colours
- body proportions

| | Devin | Duy | Third |
|---|---|---|---|
| Height (top of skull) | 1.88 m | 1.70 m | 1.80 m |
| Build | powerlifter: broad chest and shoulders, thick arms and neck, solid belly | lean, narrow shoulders | medium |
| Face | round, wide lower face; bald; thin moustache and short chin beard | soft oval, narrower jaw | full short dark beard and moustache with a clean cheek line |
| Hair | none | thick black with a fringe, as a shell over the skull | short dark sides, bleached platinum top swept to one side |
| Outfit | orange tunic with blue side panels and cuffs, gold chest emblem, orange sash, wrapped forearms, dark trousers, oxblood boots, round gold shield on the back | midnight-blue fitted jacket with violet trim, gold shoulder badge, glowing violet cuff on the left wrist, dark trousers, black rectangular glasses | off-white long coat, dark trousers |
| Vertices / faces | 25,353 / 25,400 | 37,196 / 33,509 | 45,205 / 34,660 |

How the parts are built:

- **Hair, glasses, badges, the shield and the cuff** are separate pieces in the same mesh.
  - Each is weighted rigidly to one bone: hair and glasses to `head`, the emblem and shield to the spine, the badge to `upperarm_l` and the cuff to `lowerarm_twist_02_l`.
  - They carry zero face-shape deltas.
- **Facial hair** is a region of the face skin, pushed out a few mm and given its own material. It moves with the jaw and the mouth shapes.
- **Clothing** is material regions on the body tubes, plus a flared skirt tube for Devin's tunic and Third's coat.
- **Materials** are flat colours named `<code>_<part>`, where the code is `dv`, `dy` or `tr` (for example `tr_coat`).
  - They import as material instances.
  - Duy's `dy_glow` is emissive in Blender. Check it in Unreal; it may need its emissive input wired up after import.

## Rebuild

Everything is generated; nothing is made by hand.

**1. The models** (headless Blender, about 1 minute for all three):

```
"C:\Program Files\Blender Foundation\Blender 5.0\blender.exe" -b --factory-startup --python-exit-code 1 ^
    --python tools/blender/make_leads.py -- [--only devin,duy,third] [--outdir Data/Models] [--blend-dir <dir>]
```

- The script checks each glb and fails if any of these is wrong:
  - the 56 shape names and their order
  - a primitive is missing its targets
  - the joints differ from the canonical order
  - the clips are not exactly `idle`, `rom` and `walk`
  - any sampler is not LINEAR
- Per-character settings are in the `CHARS` table at the top of the script: height, torso/arm/leg thickness tables, face warp, colours and outfit functions.
- For preview renders, run `tools/blender/ft_preview.py -- Data/Models/<name>.glb <outdir>`. Devin's render above adds `--shape` for each weight of the `amused` preset in `face_spec.py`.

**2. Import and rig** (Unreal commandlets, no editor window). Leave out `-Face=0`, so the 56-shape check runs:

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTImportCharacter -Source=Data/Models/devin.glb ^
    -Dest=/Game/Characters/Devin -NullRHI -unattended -nopause -nosplash
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildRig ^
    -Mesh=/Game/Characters/Devin/devin/SkeletalMeshes/devin.devin -Dest=/Game/Characters/Devin/Rig ^
    -NullRHI -unattended -nopause -nosplash
```

Do the same for `duy` into `/Game/Characters/Duy` and for `third` into `/Game/Characters/Third`. Keep the glb file names lower case: the asset names come from them, and the map commandlet looks for `<name>` and `<name>idle`.

**3. The map:**

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildLeadsWorld -NullRHI -unattended -nopause -nosplash
```

- It is the same floor, sun, sky and fog as the demo map, with three `AFTCharacterPreview` actors and the camera.
- It fails before touching the old map if a mesh or idle clip is missing.

## Results (2026-10-18, UE 5.8)

- **Import:** 0 errors for all three.
  - 56 morph targets, 56/56 face shapes bound, 7/7 face bones, 87 bones.
  - Each brings in its mesh, skeleton, physics asset, materials and 3 clips.
- **Clips** (same for all three):

  | Clip | Frames | Moving tracks |
  |---|---|---|
  | idle | 60 | 12 |
  | walk | 32 | 15 |
  | rom | 440 | 60 |

- **IK Rigs:** auto-characterized from the UE4 Mannequin template.
  - 48 bones matched, score 0.95, nothing missing.
  - Retarget root `pelvis`, and the same 21 chains as test_human, with IK goals on the hands and feet.
- **Weights:**
  - Body: `rig_common.weight_post_pass` gives 0 unweighted vertices, 0 unpaired mirror vertices and at most 4 influences.
  - Head: proof_head's own weights.
- **Map:** `FTBuildLeadsWorld` saved `Leads.umap` with the three facing the camera (yaw -90 from the eye/head bones).
  - This was checked from the commandlet log only.
  - Nobody has looked at the map in a running editor yet.

## Known limits

- **Map:**
  - In the map the bodies play their clips in single-node mode. The face morphs still work, but `UFTFaceAnimInstance` isn't running, so the jaw and tongue bones stay at rest. Expressions that open the jaw by bone, rather than by shape, won't show it.
  - If the project's game mode or player controller sets its own view target after the camera activates, Play shows the pawn's view instead; pilot `LeadsCamera` then.
- **Proxy quality:**
  - The bodies are tubes with procedural weights, like test_human. Expect stretching and some intersection at extreme ROM poses.
  - Hair and beard edges follow the mesh grid, so they are stepped.
  - The skirt tubes are weighted pelvis-to-thighs, so they swing through the legs in a wide stride.
- **Scale:** the height is the top of the skull. Hair adds to it: Third reaches 1.82 m.
- **Duy:** the optional notebook under his arm was left out.
