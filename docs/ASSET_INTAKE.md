# Asset intake: art, voice and music

What the user generates and hands over, and what the code accepts today. Each section says what is **defined** (read from the importers and Core), what is **not defined yet**, and gives a **proposal** for the gaps. A proposal is a default to confirm, not a rule the code checks.

Ids and formats only. Story text, names and line text never go in this repo (it is public). Examples use `SC-TEST-01` and `VO-TEST-01-001`.

| Asset | Importer today | Where it goes | Status |
|---|---|---|---|
| Skinned character (`.glb`) | `-run=FTImportCharacter`, then `-run=FTBuildRig` | `Data/Models/` | Works (proof head: 7/7 face bones, 56/56 shapes) |
| Character textures | None. Materials import as flat factors only | not defined yet | Gap, see 2 |
| Static mesh / prop | `FTImportProp` (section 2) | `Data/Models/` | Defined |
| Voice line (`.wav`) + lip-sync track (`.nface`) | `FTImportAudio -Voice` (section 3); `UFTAudioSubsystem` plays a line's voice by line id. Track: `UFTFaceComponent::PlayTrack` | `Data/Voice/<LineId>.wav` + `.nface` (gitignored) | Defined; face hookup to come |
| Music and sound cues (`.wav`) | `FTImportAudio` (section 4); `UFTAudioSubsystem` plays them by cue id | `Data/Audio/<Bus>/<CueId>.wav` | Defined |
| Concept art | Not imported | outside the repo | Convention only |

### What's left to deliver: `FTValidateAssets`

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTValidateAssets [-Category=audio|voice|models] [-Csv=Saved/AssetStatus.csv] -NullRHI -unattended -nopause -nosplash
```

It lists every asset the game expects by id and runs each delivered file through its importer's checks. Each asset ends up in one of four states:

- **missing:** no file yet.
- **failed:** the file breaks a rule.
- **delivered:** the file passes but isn't imported yet.
- **imported:** the asset is under `/Game`.

It prints one summary line per category, for example `audio: 92 expected: 0 imported, 0 delivered, 92 missing, 0 failed`. It exits 1 only when a file fails or a manifest is bad. `-Csv` writes `category,id,state` for every asset; keep that file under `Saved/`, because its voice rows carry line ids.

| Category | Expected list | File | Imported at |
|---|---|---|---|
| audio | `Data/Audio/cues.json` | `Data/Audio/<file>` | `/Game/Audio/<Bus>/<CueId>` |
| voice | every line id in the local `Data/Story` (nothing committed) | `Data/Voice/<LineId>.wav` + `.nface` | `/Game/Voice/<LineId>` |
| models | `Data/Assets/models.json` (ids, kind, lead/supporting, rule pack) | `Data/Models/<ModelId>.glb` | anything under `/Game/Characters/<ModelId>` or `/Game/Props/<ModelId>` (use that as the importer's `-Dest`) |

`models.json` is generated from the private asset tracker with `python -I tools/make_models_json.py <ASSET_TRACKER.xlsx> --leads "<name>,..."` (the start of each lead row's name, typed on the command line so no name is stored in the repo). It writes only ids and rule fields. Hand edits to `file`, `role` or `pack` survive re-runs.

Everything binary goes through Git LFS (`.gitattributes`: `glb fbx blend png jpg tga exr psd hdr wav ogg flac mp3 mp4 mov obj abc ttf otf zip nface uasset umap`). Never commit `Binaries/`, `Intermediate/`, `Saved/` or `DerivedDataCache/`.

## 1. Characters and faces

### Format

| Item | Rule | Source |
|---|---|---|
| File | glTF 2.0 binary, `.glb`. Every shipped model is `.glb`; FBX is only mentioned as an option in RIGGING_TEST.md. | `RIGGING_TEST.md`, `Data/Models/` |
| Location | `Data/Models/<stem>.glb` | `RIGGING_TEST.md` |
| One file | One skinned mesh, one skeleton, the clips, the face shape keys. | `LEADS.md` |
| Up axis, scale | glTF is +Y up, metres. Apply all transforms, scale 1.0. The canonical body is built with the skull top at 1.808 m, then scaled uniformly to the character's height. | `RIGGING_TEST.md`, `make_leads.py` |
| Facing | Any. `FTMeshFacing::FacingYaw` reads the direction from the head bone to between the eye bones and turns the preview to face +X. So the eye bones must sit in front of the head bone. | `FTMeshFacing.h` |
| Bind pose | A-pose: upper arms 45 degrees down, elbows bent 10 degrees. | `RIGGING_TEST.md` |
| Weights | 4 influences per vertex at most (export profile `export_influence_nb=4`). | `ne_blender.py` |
| Textures | Not read by the importer (see 2). Shipped models have none. | `FTImportCharacterCommandlet.cpp` |

### Skeleton: the mannequin-style bone names

The importer only hard-checks the seven face bones. The IK Rig step (`FTBuildRig`) auto-characterizes against the UE4 Mannequin template and needs the names below to match (proof head: 48 bones matched, score 0.95, none missing). The proof head's 87 joints, in order of the file:

| Group | Bones |
|---|---|
| Root, spine | `root`, `pelvis`, `spine_01`, `spine_02`, `spine_03` |
| Neck, head | `neck_01`, `neck_02`, `head` |
| Face (required) | `jaw`, `tongue_01`, `tongue_02`, `tongue_03`, `eye_l`, `eye_r` |
| Arm, per side `_l` / `_r` | `clavicle`, `upperarm`, `upperarm_twist_01`, `upperarm_twist_02`, `lowerarm`, `lowerarm_twist_01`, `lowerarm_twist_02`, `hand` |
| Fingers, per side | `thumb_01..03`, `index_01..03`, `middle_01..03`, `ring_00..03`, `pinky_00..03` |
| Leg, per side | `thigh`, `thigh_twist_01`, `calf`, `calf_twist_01`, `foot`, `ball` |
| Helpers, per side | `weapon`, `elbow_helper`, `deltoid_helper`, `knee_helper`, `glute_helper`; one `prop_back` |

Rules:
- Side suffix is `_l` / `_r`, lower case.
- `FTBuildRig` exits 1 if the retarget root is not `pelvis`, or any spine, neck, head, arm, leg or finger bone has no chain. `root` becomes the root-motion bone.
- Deform bones only. Export the `DEF-` bones, leave out `ORG-`, `MCH-` and control bones, and flatten B-Bones first.
- The canonical skeleton, with its rest data, is `assets/rig/canonical_humanoid.json` in newengine, built by `tools/blender/rig_spec.py`. Fitting scripts: `rig_fit.py`, `fit_canonical.py`, `bake_to_canonical.py`.

### Face bones

Required, and logged as `N/7 face bones`: `head`, `jaw`, `tongue_01`, `tongue_02`, `tongue_03`, `eye_l`, `eye_r`. Hierarchy: `tongue_01..03` chain under `jaw`; `eye_l` and `eye_r` under `head`. `UFTFaceAnimInstance` rotates `jaw` and `tongue_01..03` about each bone's **local +X** from its rest pose (`Face.h`), so that must be the pitch axis. Jaw range is `jaw_max_deg` = 25. A missing bone is skipped with one log warning at run time, and fails the import check.

### Face shape keys (morph targets)

Spelled and cased exactly as in `Data/Rig/face_presets.json`. All 56 are required (`N/56 face shapes bound`). The shape budget per mesh is 64 (`kMaxFaceShapes`). The importer logs one error per missing shape and exits 1.

- **ARKit-52:** `browDownLeft`, `browDownRight`, `browInnerUp`, `browOuterUpLeft`, `browOuterUpRight`, `cheekPuff`, `cheekSquintLeft`, `cheekSquintRight`, `eyeBlinkLeft`, `eyeBlinkRight`, `eyeLookDown*`, `eyeLookIn*`, `eyeLookOut*`, `eyeLookUp*`, `eyeSquintLeft`, `eyeSquintRight`, `eyeWideLeft`, `eyeWideRight`, `jawForward`, `jawLeft`, `jawOpen`, `jawRight`, `mouthClose`, `mouthDimple*`, `mouthFrown*`, `mouthFunnel`, `mouthLeft`, `mouthLowerDown*`, `mouthPress*`, `mouthPucker`, `mouthRight`, `mouthRollLower`, `mouthRollUpper`, `mouthShrugLower`, `mouthShrugUpper`, `mouthSmile*`, `mouthStretch*`, `mouthUpperUp*`, `noseSneer*`, `tongueOut` (`*` = `Left` and `Right`).
- **House shapes (4):** `mouthSmileNoCheek`, `lipsTogetherSoft`, `eyeLidTension`, `jawOpen_x_mouthSmile`.
- `jawOpen_x_mouthSmile` is a corrective. Its weight is computed by the face code as `jawOpen * mouthSmile`, so a name of the form `a_x_b` must name two shapes that exist. Don't drive it by hand.
- Leave normal deltas out of the face shapes (proxy-normal region, newengine `docs/rigging_plan.md` section 5).
- Extra shapes beyond the 56 are not checked and get weight 0 (`bindMesh`). To add one, add it to `face_presets.json` first (the file is generated by newengine `tools/blender/face_spec.py`).

### Animation clips: the CUBICSPLINE caveat

UE 5.8 Interchange rejects every spec-valid CUBICSPLINE sampler. Its check is `Input.Count != 3 * Output.Count`, the reverse of the glTF 2.0 rule (`InterchangeGltfAnimation.cpp`, `ValidateChannelSampler`). The log says "glTF sampler corrupt" and the clip imports empty (1 frame, no tracks, no curves).

- **Export LINEAR samplers only.** Bake Bezier curves to one key per frame (`rig_common.bake_linear`).
- Check the exported file: `make_rig_template.check_glb` asserts that no sampler is CUBICSPLINE.
- Shipped models already comply (every sampler in `Data/Models/*.glb` is LINEAR).
- Clip names in use: `idle`, `walk`, `rom` (characters); `face_jaw`, `rom`, `tpose`, `face_blink` (proof head). The clips land beside the mesh as `<stem>idle`, `<stem>walk` and so on. No clip names are enforced.
- newengine's `tools/blender` still writes CUBICSPLINE and needs the same change.

### Materials (as imported)

Interchange gives each glTF material an instance of a plugin master. `FTImportCharacter` then reparents each one onto a project master (skeletal-mesh and morph-target usage on, two-sided) and copies five glTF values: base colour factor, metallic factor, roughness factor, emissive factor and the `KHR_materials_emissive_strength` value.

- A material with no texture goes on `/Game/Characters/Shared/Materials/M_FTCharacter`. Emissive output is `Emissive * EmissiveStrength`.
- A material with any texture goes on `M_FTCharacterTex`, made on first use next to it with its 4x4 defaults `T_FTWhite`, `T_FTWhiteMasks` and `T_FTFlatNormal`. Interchange names its texture parameters `BaseColorTexture`, `MetallicRoughnessTexture`, `NormalTexture`, `OcclusionTexture` and `EmissiveTexture`; the importer logs each one it finds and binds it to the slot of the same name. The graph follows glTF: base colour = map RGB × factor; roughness = metal-rough map G × factor; metallic = map B × factor; ambient occlusion = occlusion map R; emissive = map RGB × emissive factor × strength. A missing map reads as white (flat normal for the normal slot), so factors alone still work.
- Each bound texture is set up for its slot: base colour and emissive sRGB, metal-rough and occlusion linear masks (`TC_Masks`), normal `TC_Normalmap`, virtual texturing off. The import fails if a texture's sampler type still doesn't match its slot.
- Not checked headless: the textured master's shader compile. Commandlets run with `-NullRHI`, which has no render resources, so the first attended editor open is where a compile error would show.

Slots: one glTF material per region (skin, lips, cloth, hair, glasses and so on) is how the current models get colour. Keep material names meaningful, as they become the instance names.

### Intake checklist: character

- [ ] `.glb` in `Data/Models/`, LFS-tracked.
- [ ] One skinned mesh, 87-joint skeleton, bone names as in the table, `_l` / `_r` lower case.
- [ ] Bind pose is the A-pose, scale 1.0, transforms applied, character upright.
- [ ] `head`, `jaw`, `tongue_01..03`, `eye_l`, `eye_r` present with the hierarchy above.
- [ ] 56 shape keys, names exact, no extra normal deltas.
- [ ] Every animation sampler LINEAR.
- [ ] Emissive materials follow the style rules (section 2).
- [ ] Textures, if any, follow section 2 (the importer checks them).

Commands (PowerShell, from the repo root, one at a time; the exe is `E:\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe`):

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTImportCharacter -Source=Data/Models/<stem>.glb -Dest=/Game/Characters/<Name> -NullRHI -unattended -nopause -nosplash
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildRig -Mesh=/Game/Characters/<Name>/<stem>/SkeletalMeshes/<stem>.<stem> -Dest=/Game/Characters/<Name>/Rig -NullRHI -unattended -nopause -nosplash
UnrealEditor-Cmd ForgottenThird.uproject -run=FTBuildDemoWorld -Mesh=/Game/Characters/<Name>/<stem>/SkeletalMeshes/<stem>.<stem> -NullRHI -unattended -nopause -nosplash
```

- **Import** (`-Source=` relative to the project, `-Dest=` default `/Game/Characters/ProofHead`, `-Face=0` skips the shape check, `-Pack=` and `-Role=` pick the asset rules in section 2). It checks the `.glb` against the asset rules first and imports nothing if any rule fails. Pass means exit 0 with `0 errors` on the asset-rules line, `7/7 face bones` and `56/56 face shapes bound`. Exit 1 names the broken rule, missing bone or shape.
- **Rig** logs every clip's frames, moving tracks and curves, then builds `IK_<stem>` and `RTG_<stem>`. Pass means exit 0. A clip with 1 frame and no tracks means a CUBICSPLINE sampler got through.
- **Demo map** puts the character on `Demo` (it overwrites the map).
- Blender-side check, before export (needs `bpy`, see `docs/BLENDER.md`): `rig_validate.py <file.glb|.blend> --template <newengine>/assets/rig/canonical_humanoid.json --out <dir> --presets Data/Rig/face_presets.json`. Accepted only on PASS.
- The jaw and tongue were not judged yet on the proof head, and the retargeter has no target (no mannequin in the project).

## 2. Style and textures

Rules from the art direction (pack names are public: Solhaven, Lattice, Unwritten, Restored, Ink Solhaven). The look is mostly the post-process style pass (`docs/STYLE_PACKS_UE.md`), which is switched per scene and applied to every mesh. So assets are authored **neutral**:

- Flat, even light in the model sheet, no baked shadows or drawn hatching in textures, no outlines. Outlines, hatching, cel bands, hulls and rims are a style-pass job (some still deferred: cel bands, lit hatching, hulls, pack rim).
- **Black pockets and load maps go in textures, not vertex colour.** Flat-black pockets (under jaw, arm, fold, ledge) are painted into the base-colour texture. Vertex colour is not read by any material here (`M_FTCharacter` has no vertex-colour input), and the art direction says the same of the engine.
- **Lattice emissives:** skills, eyes and rules at **3.0 or more** (scene-referred); everything else **below 2.5**. Set it as emissive factor times `KHR_materials_emissive_strength` in the glTF. Bloom for the pack is `bloomScale`; `bloomThresholdScale` is only applied when the base threshold is set (UE's default is off), so bloom does not split them by itself; the importer enforces the split by material name (see below).
- A character may cross between packs. Don't author a pack's look into its colours.
- Shape language and proportion rules per character are in the private design docs, not here.

**What the code accepts for textures.** `FTImportCharacter` checks every `.glb` against these rules before it imports (`ftcore::checkGlb`, `Core/include/ftcore/Glb.h`), and fails on any error. Pass `-Pack=<Solhaven|Lattice|Unwritten|Restored|InkSolhaven>` and `-Role=<lead|supporting>`; without them the pack is none (no emissive rule) and the role is supporting.

| Rule | Code | Level |
|---|---|---|
| Maps: base colour (sRGB), normal (+Y up as glTF), metal-rough packed (G roughness, B metal), occlusion (R), emissive (sRGB). Packing occlusion into the metal-rough map's R (ORM) is fine | — | how the master reads them |
| Images embedded in the `.glb` (a buffer view, not a `uri`) | `image-external` | error |
| PNG or JPEG only (all glTF allows). Emissive above 1.0 comes from `KHR_materials_emissive_strength`, not from an HDR image | `image-format` | error |
| At most 2048 px for a lead, 1024 for supporting cast and props | `image-too-big` | error |
| Square power of two | `image-not-square-pot` | warning |
| One UV set (`TEXCOORD_0`), no overlap on the face, texel density matched within a character | `uv-sets` | error (overlap and density are by eye) |
| No vertex colour (`COLOR_0`); pockets and load maps go in textures | `vertex-color` | error |
| Lattice only: a material whose name contains Glow, Eye, Skill or Rule (any case) glows at 3.0 or more | `emissive-glow-low` | error |
| Lattice only: every other emissive material stays below 2.5 | `emissive-plain-high` | error |
| A character has a skin (armature) and a mesh | `no-skin`, `no-mesh` | error |
| Pockets: painted flat black in base colour, hex from the pack's `outlineColor` (see `StylePack.h`) | — | by eye |
| Load map: a separate greyscale map, linear, named `<stem>_load.png` | — | proposal; no material reads it yet |

The emissive level is max(emissive factor) × emissive strength. Every message names the material or image by index and name.

Checklist, texture-ready: [ ] no baked light, shadow or hatch; [ ] pockets in base colour, no vertex colour; [ ] Glow/Eye/Skill/Rule in the names of the materials that must glow; [ ] import passes with the right `-Pack=` and `-Role=`.

### Props and static meshes

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTImportProp -Source=Data/Models/<stem>.glb -Dest=/Game/Props/<Name> -Pack=<pack> -NullRHI -unattended -nopause -nosplash
```

The same rules as above with the prop limits: textures up to 1024 px, no skin needed, the Lattice emissive split with `-Pack=Lattice`. A skinned file is refused (import it with `FTImportCharacter`). Materials go onto the same two masters as characters. The log gives each static mesh's triangle count, material slots and simple collision shapes; a mesh with no simple collision is a warning (Interchange adds a box unless the file carries its own `UCX_` shapes). Pass means exit 0 and `ok` on the last line.

Validation: the import's asset-rules line. Look at the result with `FTBuildDemoWorld -Style=<pack>` (see `STYLE_PACKS_UE.md`); nobody has seen the style picture rendered yet.

## 3. Voice

### Lookup by line id

A line's id is the `id` on a story line beat (`StoryRunner::lineId()`, `BeatType::Line`), taken from the local `Data/Story/scenes/<scene>.json`. The id style is `VO-<scene>-NNN`, for example `VO-TEST-01-001` in scene `SC-TEST-01`; some camp lines carry the script's own `VO-` id. Ids are what the importer reads; no text is needed to name files.

**Playback (defined):** on every story line `UFTAudioSubsystem` looks for `/Game/Voice/<LineId>` and plays it on the voice slider, stopping the previous line's voice when the beat moves on. A line without one shows text only (logged once). `OnVoiceLine(LineId, Seconds)` fires when a voice starts, and `UFTAudioSubsystem::VoiceTrackFile(LineId)` gives `Data/Voice/<LineId>.nface` for `UFTFaceComponent::PlayTrack(TrackFile, StartSeconds)`. **Not wired yet:** the speaking character's face playing that track, and auto-advance waiting for the voice to end (both belong to the story director).

**Naming (defined):** `Data/Voice/<LineId>.wav` and `Data/Voice/<LineId>.nface` side by side, for example `VO-TEST-01-001.wav` and `VO-TEST-01-001.nface`. **Both `Data/Voice/` and `Content/Voice/` are gitignored:** the audio speaks the story, and this repo is public. Note the bake tool currently keys its output as `<text_hash>_<speaker>` (a hash of the words plus the speaker label), which is not the story id. Rename on intake, or change the bake to key on the line id. Do not mix the two schemes.

### Audio file

| Item | Defined? | Value |
|---|---|---|
| Container | Defined (checked) | Uncompressed WAV: PCM 16/24/32-bit or 32-bit float (the import). The bake reads audio with Python's `wave` module, which takes PCM only. |
| Channels | Defined (checked) | Mono (`voice-not-mono` is an error). |
| Sample rate | Defined (checked) | 44100 or 48000 Hz; 48000 preferred, the project rate (`Config/DefaultEngine.ini`). |
| Loudness | Defined (checked) | -23 LUFS integrated +-2, true peak at most -1 dBTP, one level for every line. A line under 0.4 s can't be gated and gets a warning. |
| Silence | Not defined | The track time 0 is clip time 0. **Proposal:** 50 to 100 ms of lead-in, 150 ms or more of tail, since the face blends back to rest over 120 ms at line end. Holds before or after a line are done in the edit. |
| Bus | Defined | The voice slider (master x voice), applied per sound by `UFTAudioSubsystem`. No Unreal sound class or submix. |

### Lip-sync track (`.nface`)

The contract is newengine `docs/lipsync_track_format.md` (branch `feature/engine-foundation`). The reader here is `ftcore::parseFaceTrack` (`Core/include/ftcore/Face.h`). Summary:

- Little-endian. Header: magic `NFACE\0` (6 bytes), `u16` version (1), `u16` rate (frames per second, the bake writes 60), `u32` frame count, `u16` channel count. Then per channel a `u8` name length and the name bytes. Then the data: `u8` samples, frame-major, exactly `frames * channels` bytes to the end of the file (anything else is an error).
- Channels are shape names (ARKit names, as in `face_presets.json`) and the reserved ones `@speech` (0 to 1) and `@tongue_01..03` (value 0..255 maps to -40..+40 degrees). Unknown `@` names are ignored. A shape name the rig lacks is ignored. The player takes at most 72 channels (64 shapes + 8).
- Value = sample / 255, linear between frames, clamped at the ends. Time 0 is the audio's time 0, and duration is `frames / rate`.
- A track holds only the speech layer (jaw, lips, tongue). Brows, eyes, cheeks and tone shapes come from the expression and blink layers, which `FacePlayer` composes.
- Optional side file `<key>.events.json` (breath events, for audio or VFX hooks only; nothing reads it here).

### Intake checklist: voice

- [ ] `Data/Voice/<LineId>.wav`: mono, 48 kHz, one line per file, -23 LUFS, id exists in `Data/Story`. `FTImportAudio -Voice -Check` measures them all.
- [ ] `<LineId>.nface` baked from that wav at 60 fps, 20 or so channels (16 shapes, `@tongue_01..03`, `@speech` in the reference bake).
- [ ] No two files for one id; ids in `Data/Story` with no file are listed, not guessed.
- [ ] Nothing from `Data/Voice/` or `Content/Voice/` is committed (both gitignored). Never commit a file whose name or content carries line text.

Import (from `Data/Voice/`, onto `/Game/Voice/<LineId>`):

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTImportAudio -Voice [-Cue=<LineId>] [-Check] -NullRHI -unattended -nopause -nosplash
```

Bake commands (Python 3.12, `python -I`; the tools were carried over unchanged and **have not been run from this repo yet**):

```
python -I tools/lipsync/bake_face_tracks.py bake --work <W> --backend <espeak|mfa|rhubarb|elevenlabs> --out <W>/face/<backend>
python -I tools/lipsync/bake_face_tracks.py eval --work <W> --backends <backend> --report <W>/report.json
```

- `bake` reads `<W>/wav/<key>.wav` and writes `<key>.nface` and `<key>.events.json`. `eval` measures the tracks against the lip-sync metrics (F1 to F4). Alignment backends: `align-mfa` and `align-rhubarb` subcommands produce the phone data first (see the tool's `--help`).
- `FTImportAudio -Voice` parses each line's `.nface` (an error when it doesn't parse) and warns (`nface-length`) when it is more than 0.25 s longer or shorter than the audio. Other checks: `parseFaceTrack` (the face tests in `ctest --test-dir Core/build -C Debug`) and the load at run time, which logs the path and the parse error on failure.
- The reference voice-direction pipeline (directions CSV, ElevenLabs settings) is `tools/vo_direction/` and newengine `design/forgotten_third/voice/`. It is private design data and is not summarised here.

## 4. Music

### What the code needs

`ftcore::Conductor` turns an audio cursor into a smooth song clock. `UFTConductorComponent` supplies it with the cursor of a playing `UAudioComponent` (`BindAudio`; the cursor is playback percent times `USoundWave::Duration`) or from `ObserveCursor`. `BindMusic` binds a music cue's component instead: it takes `Bpm` and `OffsetSeconds` from the cue (`ftcore::conductorConfigFor`) and unwraps the cue's loop points (`ftcore::LoopUnwrapper`), so the song time keeps rising through every loop. The battle screen binds to the music the story is playing when that cue has a `bpm` (not under `-AlphaFast`, whose game clock runs 8x); otherwise it uses the typed-in values. The beat grid is one **constant** tempo:

| Field | Meaning | Default | Set by |
|---|---|---|---|
| `Bpm` | beats per minute, constant for the whole track | 120 | the cue's `bpm` when bound with `BindMusic`, else the property |
| `OffsetSeconds` | song time of beat 0 (the lead-in; the music still plays from the file's start) | 0 | the cue's `offset` when bound with `BindMusic`, else the property |
| `LatencyOffset` | the player's input latency, from calibration (`BeginCalibration`, `RecordTap`, `EndCalibration`) | 0 | calibration |

`beat = (songTime - offset) * bpm / 60`. Battle charts (`MusicConfig`) take `bpm` and `offset` from the bound conductor, and a combo's first note waits for the next beat or bar. Tracking: a cursor jump of more than 0.1 s counts as a seek and snaps the clock.

### Format

| Item | Defined? | Value |
|---|---|---|
| Track | Defined (checked) | One `.wav` per cue: PCM 16/24/32-bit or 32-bit float, 44.1 or 48 kHz (48 kHz preferred, the project rate), mono or stereo. Ogg or mp3 add encoder padding that shifts beat 0, so they aren't read. |
| Stems | **Not defined yet** | The conductor follows one audio component. **Proposal:** deliver stems (drums, bass, harmony, lead) of equal length, sample-aligned, plus the full mix. Only the full mix drives the clock. |
| BPM, offset | Defined as inputs | Per cue, constant: `bpm` and `offset` in `cues.json` (filled where the cue sheet gives one plain tempo). `BindMusic` reads them. |
| Tempo changes | **Not supported** | One `Bpm` per conductor. A piece with tempo changes needs a code change, or separate files. |
| Loop points | Defined (played) | `loopStart` / `loopEnd` in seconds. The intro before `loopStart` plays once; when the cursor reaches `loopEnd`, `UFTAudioSubsystem` seeks back to `loopStart` plus the overshoot, so the beat phase holds. The jump is checked once a frame, so it can land up to a frame late and isn't sample-accurate. Audio after `loopEnd` is never heard. A cue without loop points loops the whole file. The loop should be a whole number of bars; `importAudioCues` warns when it isn't a whole number of beats at the cue's `bpm`. |
| Calibration track | **Not defined yet** | `BeginCalibration` needs a steady beat. **Proposal:** a click track at 100 BPM or slower, as the code advises a slower tempo if the offset might be large. |
| Loudness | Defined (checked) | Integrated loudness after BS.1770-4 (`ftcore::measureLoudness`), true peak by 4x oversampling. Music -18 LUFS +-2, ambience -24 +-3, voice -23 +-2; sfx have no loudness target. Every bus: true peak at most -1 dBTP. A clip too short or quiet to gate (under 0.4 s) gets a warning, not an error. |
| Cue ids | Defined | `Data/Audio/cues.json` (ids and numbers only) maps every cue id the story fires to its bus (`music`, `sfx`, `ambience`), playback (`loop`, `stinger`, `oneshot`), file (default `<Bus>/<CueId>.wav`), and optional `bpm`, `offset`, `loopStart`/`loopEnd`, `fadeIn`/`fadeOut` and `gainDb` (format in `Core/include/ftcore/AudioCues.h`). `tools/make_cues_json.py <cues_final.csv>` regenerates it from the private cue sheet and keeps hand edits. A ctest checks that every cue in the story has an entry. A cue id ending in `-NONE` means silence and has no file. |
| Playback | Defined | `UFTAudioSubsystem` listens to the story's cues. A music loop cross-fades from the current music (Play), and Stop or a `-NONE` cue fades it out. Stingers and one-shots play once over everything. An ambience loop runs until a Stop for its id. Volume = master x bus slider (sfx and ambience share the sfx slider) x `gainDb`, and held sounds follow the sliders. Sounds persist across the story, battle and field maps. A cue without a sound is silent and logged once at Display. |

### Intake checklist: music

- [ ] File is `Data/Audio/<Bus>/<CueId>.wav` (or the cue's `file`), and the cue is in `Data/Audio/cues.json`.
- [ ] One constant tempo, measured: `bpm` to 2 decimals, `offset_seconds` to the millisecond.
- [ ] Loop length is a whole number of bars (proposal).
- [ ] 48 kHz, level and true peak as above (`FTImportAudio -Check` measures them), mix and stems the same length.
- [ ] A drum-only or click stem exists for checking the grid by ear and for calibration.
- [ ] Files LFS-tracked.

Commands:

```
UnrealEditor-Cmd ForgottenThird.uproject -run=FTImportAudio [-Cue=<CueId>] [-Check] -NullRHI -unattended -nopause -nosplash
```

It looks for every manifest cue's file (or just `-Cue`), decodes and measures each one it finds, and holds it to the rules above. A file that passes is imported as `/Game/Audio/<Bus>/<CueId>`, with looping set from the playback. `-Check` measures without importing. Missing files are listed, not errors. It exits 1 when a file breaks a rule (issue codes: `sample-rate`, `channels`, `voice-not-mono`, `empty`, `loudness`, `true-peak`, `loop-range`; `loudness-unmeasured` is a warning) or an import fails. The headless alpha run uses `-nosound`, so it only proves that missing audio stays silent and clean: loop playback and the conductor's binding are not exercised by the gate.

Placeholder: `MUS-KNK` (60 BPM, a one-bar intro, a four-bar loop from 4 s to 20 s, then a tail bar that should never play) comes from `python tools/audio/make_placeholder_music.py --bpm 60 --out Data/Audio/Music/MUS-KNK.wav`. Tones only. Replace it when the real track arrives.

## 5. Concept art

Concept art is reference only; no importer reads it, and it is not shipped. The rules:

- **White background**, flat, no gradient, no scene.
- **One subject per sheet.** A character, a creature or a prop, not a group.
- **Several angles** on the sheet (front, three-quarter, side, back for a character; at least front and three-quarter for a prop).
- Neutral light, no baked shadow, no rim, no hatching, no speed lines. A pose a modeller can read: A-pose for a character (arms about 30 to 45 degrees down).
- Reference photos of real people stay outside the repo.
- Technique only from reference games and art: no names, designs, costumes or creatures, and no franchise or artist names in file names, metadata or shipped text.

**Not defined yet:** a folder and naming scheme. **Proposal:** keep the sheets outside the repo (`D:\projects\` next to the reference images), named `<AssetId>_sheet_v<NN>.png`, in sRGB PNG at 2048 px on the long side or more. Only commit a sheet if it has no real likeness and the user says so (PNG is LFS-tracked).

Checklist: [ ] white background; [ ] one subject; [ ] two or more angles; [ ] neutral light; [ ] no names or text on the sheet; [ ] file named by asset id.

## Gaps, in one list

1. The textured master's shader compile is only seen in an attended editor (commandlets run with `-NullRHI`).
2. The lip-sync bake keys files by text hash, not by the story id (rename on intake). A line's face track and voice-aware auto-advance aren't wired into the story director yet.
3. Loudness targets are checked at import, but there are no Unreal sound classes or submixes: bus volume is applied per sound by `UFTAudioSubsystem`.
4. The lip-sync tools have not been run from this repo (`FTImportAudio -Voice` validates the tracks they write).
5. Music loop points are played by a seek, checked once a frame, so they aren't sample-accurate. There's no tempo map or stems playback. A music change during a battle leaves the conductor on the old, fading component. Combat was tuned at 120 BPM, so a cue with a much slower tempo changes how combos feel. None of this is heard in the headless gate (`-nosound`).
6. No folder or naming scheme for textures or concept art (audio: `Data/Audio/<Bus>/<CueId>.wav`).
7. The Lattice emissive thresholds (3.0 and 2.5) are enforced at import by material name; `bloomThresholdScale` is still off by default.
8. The rig's retargeter has no target mannequin, and the proof head's body, tongue and style picture have not been judged.
