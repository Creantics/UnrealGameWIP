# Lip-sync track format (`.nface`)

This page is the contract for the engine-independent part of the lip-sync pipeline:
- the audio→phoneme alignment and the bake (`tools/lipsync/bake_face_tracks.py`);
- the viseme set (`assets/rig/face_presets.json`);
- the `.nface` track file they produce.

A runtime in any engine can play a track from this page alone. In this repo the reader is
`ne::anim::parseFaceTrack` (`engine/src/anim/FacePlayer.cpp`), the writer is `serializeFaceTrack`, and the
bake writes the same bytes.

## Pipeline

```
line.wav + spoken text
  └─ aligner: MFA, Rhubarb or espeak    → phone intervals (ARPAbet), word intervals
       └─ phone_to_viseme (face_presets.json) → viseme intervals (15 visemes, below)
            └─ bake: dominance blend, closure windows, F3 limits, breath tags
                 └─ per-frame shape weights (ARKit names) → line.nface (+ line.events.json)
```

A track stores **blendshape weights under ARKit names**, already blended from the visemes. It does not
store viseme IDs, so a consumer never needs the viseme table to play it. The table matters only when you
re-bake or retarget.

## File layout (version 1)

All integers are little-endian. There is no padding.

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 6 | magic | `N F A C E \0` |
| 6 | u16 | version | `1`. A reader rejects anything else |
| 8 | u16 | rate | Frames per second, > 0 (the bake writes 60) |
| 10 | u32 | frames | Frame count |
| 14 | u16 | channels | Channel count, > 0 (the reference reader allows at most 72: 64 shapes + 8) |
| 16 | … | channel table | `channels` entries, each a u8 length followed by that many bytes of name (ASCII, no terminator, ≤ 255) |
| … | frames × channels | data | u8 samples, **frame-major**: frame 0 channel 0, frame 0 channel 1, … |

**Validation.** The data block must be exactly `frames × channels` bytes to the end of the file. Anything
else is an error, not a truncation to tolerate.

**Duration** is `frames / rate` seconds. Frame *f* is at time *f / rate*. Track time 0 is clip time 0 of
the line's audio.

**Sampling.** Value = sample / 255. Interpolate linearly between the two neighbouring frames. Clamp
before frame 0 and after the last frame.

## Channels

**A shape channel** is any name not starting with `@`. Its value is a blendshape weight in [0, 1]. The bake
emits only the shapes some viseme uses. For the current set (see the example below) that is `jawOpen`,
`lipsTogetherSoft`, `mouthClose`, `mouthFunnel`, `mouthLowerDownLeft/Right`, `mouthPressLeft/Right`,
`mouthPucker`, `mouthRollLower`, `mouthShrugUpper`, `mouthStretchLeft/Right`, `mouthUpperUpLeft/Right` and
`tongueOut`. A reader ignores a name its rig lacks.

**Reserved channels:**

| Name | Value → meaning |
|---|---|
| `@speech` | σ in [0, 1]. 1 while the character is articulating, ramping to 0 in pauses and at line end. Use it to fade the expression's own mouth shapes out under speech (see Layering) |
| `@tongue_01`, `@tongue_02`, `@tongue_03` | Tongue joint pitch in degrees = (value × 2 − 1) × 40. Encoded 128 ≈ 0°. It is a rotation about each tongue joint's local +X from its rest pose, root to tip |

**Unknown `@` names** are reserved for later versions. A reader must ignore them.

**Side file.** `line.events.json` (optional, written by the bake) carries non-weight events, currently
`"breaths"`: `[exhales]` / `[sighs]` intervals from the line's tags. Breath shapes are already baked into
the weights. The events are for audio or VFX hooks only.

### Example

`assets/rig/lines/5dc7c14b9fad_duy.nface` is 60 Hz, 286 frames (4.77 s) and has 20 channels: the 16 shapes
above, `@tongue_01..03` and `@speech`.

## The 15 visemes

These are the Oculus/OVR-style names, defined in `face_presets.json` → `visemes`. Weights are in ARKit
names. Tongue is the pitch of tongue_01/02/03 in degrees.

| Viseme | Phones (ARPAbet) | Shapes | Tongue |
|---|---|---|---|
| `sil` | silence, SP, SPN | (rest) | 0, 0, 0 |
| `PP` | P, B, M | mouthClose 1.0, mouthPressL/R 0.35 | 0, 0, 0 |
| `FF` | F, V | mouthRollLower 1.0, mouthUpperUpL/R 0.25 | 0, 0, 0 |
| `TH` | TH, DH | jawOpen 0.16, mouthUpperUpL/R 0.10, tongueOut 0.45 | −8, −10, −6 |
| `DD` | T, D, N, L | jawOpen 0.20, mouthStretchL/R 0.15, tongueOut 0.50 | −20, 0, 0 |
| `kk` | K, G, NG | jawOpen 0.26, mouthStretchL/R 0.20 | 8, 6, 0 |
| `CH` | CH, JH, SH, ZH | jawOpen 0.14, mouthFunnel 0.55, mouthPucker 0.20, mouthShrugUpper 0.20 | −6, −8, −4 |
| `SS` | S, Z | jawOpen 0.07, mouthStretchL/R 0.40, mouthUpperUpL/R 0.12 | −6, −8, −4 |
| `RR` | R, ER | jawOpen 0.15, mouthFunnel 0.35, mouthPucker 0.30 | −4, −12, 10 |
| `aa` | AA, AE, AH, AW, AY | jawOpen 0.62, mouthLowerDownL/R 0.30, mouthUpperUpL/R 0.12 | 4, 0, 0 |
| `E` | EH, EY | jawOpen 0.34, mouthLowerDownL/R 0.15, mouthStretchL/R 0.40 | 0, −4, 0 |
| `ih` | IH, IY, Y | jawOpen 0.24, mouthStretchL/R 0.28 | 0, −6, 0 |
| `oh` | AO, OW, OY | jawOpen 0.44, mouthFunnel 0.65, mouthPucker 0.10 | 4, 2, 0 |
| `ou` | UH, UW, W | jawOpen 0.14, mouthFunnel 0.35, mouthPucker 0.85 | 4, 2, 0 |
| `nn` | (none by default; N maps to DD) | lipsTogetherSoft 1.0, mouthClose 0.3 | −10, −18, −14 |

**HH** maps to `next`: it takes the following phone's viseme. Rhubarb's A–H, X map through
`rhubarb_to_viseme` (A→PP, B→ih, C→E, D→aa, E→oh, F→ou, G→FF, H→DD, X→sil).

**Closure floors** (`viseme_floors`): PP holds `mouthClose` ≥ 0.8 and FF holds `mouthRollLower` ≥ 0.5 for
the length of the closure. The bake guarantees this in the weights.

## Mapping onto ARKit 52 (UE5 / MetaHuman / Live Link Face)

Every shape name in a track except one is an ARKit blendshape name, spelled and cased as ARKit,
Live Link Face and the MetaHuman ARKit mapping pose asset use them. In UE, a track channel can drive the
**curve of the same name** directly, for example from an Anim Blueprint or a Live Link source fed from the
file. No renaming is needed.

**Track channels that are not one of the ARKit 52:**

| Channel | On an ARKit-only rig |
|---|---|
| `lipsTogetherSoft` | Fold into `mouthClose` (add 0.3 × value) and `mouthPressLeft/Right` (add 0.2 × value), clamped to 1. It is used only by `nn`, a soft lip seal |
| `@tongue_01..03` | ARKit has only `tongueOut`. Drive the tongue joints or MetaHuman tongue controls from the degrees, or drop them; `tongueOut` is already in the track for TH and DD |
| `@speech` | Not a shape. It is a layering input (below) |

**ARKit shapes a track never writes:**
- brows;
- eyes, blinks and gaze;
- cheeks and nose;
- `jawForward/Left/Right`, `mouthLeft/Right`;
- `mouthSmile*`, `mouthFrown*`, `mouthDimple*`;
- `mouthRollUpper`, `mouthShrugLower`.

These belong to the expression and eye layers, which stay with the character.

## Layering with an expression (recommended)

The track is the speech layer only. To combine it with an expression's weights *e* (from `face_presets.json`
→ `expressions`, scaled by intensity / 5), use the rule the engine ships and validates (F12, F13):

- closure c = clamp01(max(mouthClose / 0.8, mouthRollLower / 0.5)), taken from the track's weights;
- a = (1 − σ)(1 − c), where σ = `@speech`;
- **Mouth-articulation shapes** (jaw, mouthClose, Funnel, Pucker, Press, Roll, Shrug, UpperUp, LowerDown,
  tongueOut, lipsTogetherSoft): out = track + a · e. For `jawOpen`: out = max(track, a · e).
- **Mouth tone shapes** (mouthSmile, Frown, Dimple, Stretch, mouthLeft/Right, noseSneer): the expression's
  value only, out = e × (1 − 0.7 · max(funnel, pucker)) × (1 − 0.5 · mouthClose), using the track's funnel,
  pucker and close.
  - Known gap: `mouthStretchLeft/Right` is a tone shape in this table, so the engine drops the track's
    stretch values (SS, E, ih, DD, kk write them).
  - A new consumer should add the track's stretch, clamped to 1, rather than copy that gap.
- **Brows, eyes and cheeks:** the expression's own values. The track never writes them.

Without the closure term, an open-jawed expression (grief, stunned) parts the lips on P/B/M and F/V.
At line end, blend the mouth from its last value to the expression's rest over 120 ms (smoothstep). If you
ease the visemes and σ separately, the jaw dips below rest and rises again.

## Versioning

- Any change to the byte layout bumps `version`.
- Adding a reserved `@` channel or a new shape name does not. Readers already ignore names they do not know.
