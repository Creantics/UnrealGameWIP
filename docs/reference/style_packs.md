# World style packs (v1, retuned by art direction v2)

The Forgotten Third is drawn in four worlds that share one renderer (NDD 8.1.1, 8.15.1). A **style
pack** is the parameter set for one of them: how lines are drawn, how light is banded, and how colour
and paper are treated. v1 has four packs plus `None`, and any blend of them:

![The four packs in post_demo: Solhaven, Lattice / Unwritten, Restored](style_packs.png)

*`post_demo --style solhaven | lattice | unwritten | restored`. In the Unwritten, the pale sphere
stands inside a colour-authority island.*

The pack values follow **art direction v2** (`design/forgotten_third/art_direction_v2.md`, §3 and §7),
which retuned Solhaven, the Lattice and the Unwritten and left Restored as it was:

![Solhaven and the Lattice before (left) and after (right) art direction v2](style_v2_solhaven_lattice.png)

*Left: v1. Right: v2. Solhaven is more saturated, inks parallel hatching into its shadow band only and
gives the pawns a gold rim. The Lattice gets a hard two-band cel with near-black indigo shadows, a thin
indigo hull and a cyan rim, and a raised bloom threshold, so only the emissives (authored at 3.0 or
more) bloom.*

![A 50% blend of Solhaven and the Unwritten, and the dial S3 U1](style_blend.png)

*Left: `--style solhaven --style-to unwritten --blend 0.5`. Right: `--style S3,U1` (a Solhaven scene
with a little of the Unwritten coming through).*

## Using it

```cpp
#include "ne/render/StylePack.h"   // header-only, no GL: usable from tools and tests

PostSettings ps = renderer.postSettings();
ps.style = stylePack(StylePack::Unwritten);                     // one pack
ps.style = lerp(stylePack(StylePack::Solhaven), stylePack(StylePack::Lattice), t); // any two
ps.style = styleFromDial({3.f, 0.f, 1.f, 0.f});                 // Style Dial needles S, L, U, R
renderer.setPostSettings(ps);
```

- `StyleSettings` is a plain struct of floats and colours that lives in `PostSettings::style`. It is
  defined by an X-macro (`NE_STYLE_FIELDS`), so `==`, `lerp` and the dial blend cover every field
  automatically. Its defaults are the identity, `None`.
- `lerp(a, b, t)` blends field by field. `t` is clamped, and `t = 0` / `t = 1` return `a` / `b`
  exactly. Island placement (centre and radius) is not blended: it follows whichever side has an
  island.
- `styleFromDial(StyleDial)` implements the Style Dial (NDD 8.1.2). Each needle is 0..3 and has
  weight `needle / max(sum, 3)`; whatever weight is left goes to `None`. So:
  - a pure scene (one needle at 3) is exactly that pack;
  - a lone needle at 1 is a third of its pack;
  - S3 with U1 is three quarters Solhaven and one quarter Unwritten.
- `parseStyleSpec("unwritten")` / `parseStyleSpec("S3,U0.5")` / `parseStyleSpec("3,0,0.5,0")` is the
  command-line form. Pack names are case-insensitive. A dial is either named needles in any order,
  or exactly four bare numbers in S,L,U,R order.
- `parseStyleSpec("inksolhaven")` (any case) returns `inkSolhavenStyle()`, the **Ink Solhaven** preset
  of art direction v2 §5 (Solhaven cel colour under Unwritten ink). It is the dial S1 U3 with
  `saturation` 1, `monochrome` 0.8 (about a fifth of the colour comes back: writer decision D6, v2
  option), a hard two-band cel (`cel` 1, `celSteps` 2) and the paper sky. It is a preset, not a
  `StylePack`.
- Packs need the HDR post chain (`PostSettings::enabled`). With post off they do nothing.
- **Off is free and identical.** When `style.active()` is false (`None`, the default), nothing changes:
  - the tonemap pass uses the same shader program as before;
  - the depth copy is skipped;
  - the lit shader's cel, hatch and pack-rim branches are skipped through uniforms that are never
    changed;
  - the bloom threshold is the unscaled `PostSettings::bloomThreshold`;
  - the hull pass draws nothing (its programs are primed once with a draw that rasterises nothing).

  `post_demo` with no `--style` is pixel-identical (ImageMagick AE = 0) to the build before this
  change.

### Command line

| Program | Flag | Notes |
|---|---|---|
| `post_demo` | `--style PACK\|inksolhaven\|DIAL`, `--style-to PACK`, `--blend T`, `--auto-blend`, `--no-island`, `--no-hull`, `--no-hatch`, `--hull-static W`, `--ink-void FAR` | Keys: `1`-`5` choose None, Solhaven, Lattice, Unwritten, Restored. `Tab` moves to the next pack. `Left`/`Right` blend toward the next pack in steps of 10%. `B` toggles an automatic ping-pong blend. `I` toggles the island. `H` toggles the hull silhouette. A styled run also gets a light rig that suits the pack and a few extra props (steps, an arch with a banner, crates, and two swaying skinned pawns for the hull). Without a style the scene is unchanged. The Lattice rig authors the glows at 1.4x so the rule and skill emissives sit above the raised bloom threshold (v2 §2). `--ink-void FAR` adds an ink void from 0.4 × FAR to FAR world units. |
| `rpg` | `--style PACK\|inksolhaven\|DIAL` | Applied once at start. Settings re-applies keep it. Default `none`. |
| `battle_demo` | `--style PACK\|inksolhaven\|DIAL` | Ignored with `--no-post`. Default `none`. |

## What each pack does

All widths are in pixels at 1080p and scale with the output height, so the line looks the same at
720p and at 4K.

| | Solhaven | Lattice | Unwritten | Restored |
|---|---|---|---|---|
| **Outline** | Hatch-black `#14110F`, 2.4 px, **tapered**: heavier near the camera, thinner in the distance, with a slow brush-pressure wobble | Night-indigo `#12072B`, 1.0 px, **constant**, at 90% strength (crisp grid line, not a coloured line) | Ink `#0B0B0C`, 1.3 px, **8 fps tremble**: a low-frequency noise offset of about 1 px, redrawn every 8 frames, with a small width wobble | Restored ink `#17151A`, 1.6 px, **constant and steady** (ligne claire) |
| **Light** | Hard 2-step cel on the sun and point lights | Hard 2-step cel at 90% | Smooth (the tone comes from the hatching) | Soft 3-step cel at 85% |
| **Colour** | Saturation ×1.25. Highlights are tinted with trophy gold `#FFC93C`; shadows are warmed with bronze `#B07A35` | Saturation ×1.3. Shadows are multiplied by near-black indigo (0.46, 0.38, 0.59); highlights get a touch of cyan `#19E3F0` | **Monochrome.** Luminance is remapped from ink to paper `#F2EFE8` with gamma 1.15 (the darks stay ink and the hatching carries the tone) | Saturation ×1.08. Shadows get Lattice violet (12%, NDD 8.2.5). Highlights are morning daylight `#FFF4DD` with a breath of dawn rose `#F2A3B3` |
| **Halftone** | – | – | Off: the hatching carries the tone | – |
| **Hatching** | **Shadow hatching:** parallel lines only (`hatchMaxLevel` 2), inked over the lit colour (`hatchOverlay` 1) and only in the sun's lowest cel band (`hatchShadowOnly` 1); 240 px tile, density 1.0, ink `#14110F` | – | **Ink drawing:** a tonal art map of nested pen strokes, 280 px tile (at 1080p), density 1.4, tone curve 1.8 (one direction in the mid-tones, the cross only in the darks), ink `#0B0B0C` | – |
| **Hull silhouette** (characters) | Hatch-black `#14110F`, 2.6 px | Night-indigo `#12072B`, 1.2 px, at 80% | Ink `#0B0B0C`, 2.4 px | Restored ink `#17151A`, 1.7 px |
| **Rim** (characters) | Gold `#FFC93C` at 1 (blooms) | Cyan `#19E3F0` at 0.35 (stays under the bloom threshold) | – | – |
| **Sky** | Scene sky | Scene sky | **Blank page:** the sky is replaced by paper | Scene sky |
| **Paper grain** (NDD 8.3.4) | 2% | 0 | 7% | 3% |
| **Finish** | No chromatic aberration | Bloom ×1.8, bloom threshold ×2.5 (only emissives bloom), vignette ×1.5 | Bloom ×0.35, vignette ×0.4, no film grain or chromatic aberration | No chromatic aberration |

Every value is a field of `stylePack()` in `engine/include/ne/render/StylePack.h`, and the NDD and
art direction v2 sources are commented next to each one. The **ink void** (`inkVoid`, `inkVoidNear`,
`inkVoidFar`) is 0 in every pack: a scene sets it (v2 §3: the Theater, the Stairwell, the Undertow, the Library stacks, Hush 75 and up; never in rooms).

The v2 fields and what they do:

| Field | Identity | Packs | Effect |
|---|---|---|---|
| `hatchOverlay` | 0 | Solhaven 1 | 0: strokes over a flat paper base; 1: strokes over the lit colour itself |
| `hatchMaxLevel` | 5 | Solhaven 2 | Highest TAM level used (2 = parallel lines, never a cross or solid ink) |
| `hatchShadowOnly` | 0 | Solhaven 1 | Coverage is masked to the sun's lowest cel band (`celSteps` bands, anti-aliased), so a lit surface is never inked |
| `hatchCurve` | 1 | Unwritten 1.8 | Darkness `(1 - tone)^curve × density` picks the level, so the cross starts at tone 0.50, is full at 0.375, solid ink starts at 0.27 and is complete at 0.17 |
| `rimScale`, `rimTint` | 0, white | Solhaven 1 / `#FFC93C`, Lattice 0.35 / `#19E3F0` | A fresnel rim, `(1 - N·V)^3 × rimScale × linear(rimTint)`, scene-referred, on skinned meshes (the cast) only |
| `bloomThresholdScale` | 1 | Lattice 2.5 | Multiplies `PostSettings::bloomThreshold` for the bright pass |
| `inkVoid`, `inkVoidNear`, `inkVoidFar` | 0, 8, 30 | – (per scene) | The style stage mixes to the ink colour from `inkVoidNear` to `inkVoidFar` world units of view distance; the sky is inked too. `lerp` keeps the distances of the side that has a void |

The face fields (rigging P1c) act only on materials an app tags with a `FaceRole` (`Material::faceRole`:
`Skin` or `Feature`), so they never change anything else a pack draws. They do not make a pack `active()`
on their own. See [rigging.md "Faces in every world style"](rigging.md#faces-in-every-world-style).

| Field | Identity | Packs | Effect |
|---|---|---|---|
| `faceFeatures` | 0 | Solhaven 1, Unwritten 1 | Face features (brows, lips, eyes, mouth interior) keep their own lit value under hatching: no paper base, no strokes |
| `faceContrast` | 0 | Unwritten 1 | Feature value pushed apart: colour × `(luma(albedo) / 0.4)^faceContrast` (clamped to 1.25), scaled by `faceFeatures × hatch` |
| `faceHatch` | 1 | Unwritten 0.6 | Hatch density multiplier on face skin |
| `faceHatchMaxLevel` | 5 | Unwritten 2 | Stroke level cap on face skin (2 = one direction, no cross, no solid ink) |
| `faceSkinLit` | 0 | Unwritten 0.3 | Share of the face skin's lit shading kept in its paper base, so cheek shading (half of a smile) still moves |
| `faceSnap` | 0 | – | > 0: the app snaps morph weights to this many steps (held drawings, `snapFaceWeight`); 0 = smooth |

Blends weigh the face fields by each side's `hatch` as well as by blend weight (`style_detail::blendFaceFields`).
A third of the Unwritten carries a third of its hatching, and with plain weights only a third of its face
treatment, which measured 0.53 of the default face change. Weighted by hatch, it keeps the Unwritten face
treatment in full. Every face field acts through the hatch, so blends stay continuous. `faceSnap` blends
linearly.

### Pipeline placement

```
sky  ->  lit pass, opaque (cel bands: uCel / uCelSteps; hatching: uHatch*; pack rim: uStyleRim)
  ->  inverted hulls  ->  lit pass, blended  ->  transparent pass  ->  depth copy (only if styled)
  ->  bloom (threshold × bloomThresholdScale)  ->  tonemap + grade  ->  [STYLE: tint/saturation ->
      mono + halftone -> paper sky -> ink void -> island -> outline -> paper tooth]
  ->  vignette / film grain  ->  UI (Renderer2D)
```

- **Cel bands** live in the forward lit shader (`Renderer.cpp`), not in post.
  - The light amount for the sun (`N·L` × shadow) and for each point light (`N·L`) is quantised into
    `celSteps` bands.
  - Band edges are anti-aliased with `fwidth`.
  - The result is mixed back by `cel`. Albedo, ambient and emissive are not affected.
  - Banding luminance after tonemapping would posterise textures and look like a cheap photo filter,
    so it is not done that way.
- **The style stage** is a compile-time permutation (`#define STYLE`) of the tonemap shader. It sits
  in the same full-screen pass:
  - after bloom, so grain shows through colour blooms (NDD 8.15.4);
  - before vignette, film grain and the HUD, so the UI never looks printed.
- **Edges** come from the depth texture (a copy of the scene depth, already used by soft particles):
  - The shader reconstructs reciprocal view distance, `1/z`. On any plane, `1/z` is affine in screen
    space.
  - Its second difference over a 3×3 ring (radius = line width) is therefore zero on flat surfaces.
    It is large at silhouettes (depth jumps) and at creases (where the slope changes, which is where
    the normal changes).
  - The measure is relative to the centre value and passed through `smoothstep` between `threshold`
    and 2.5 × `threshold`.
  - So "normals" are used implicitly through the depth curvature. There is no separate normal buffer
    (see Deferred).
- **The colour-authority island** (NDD 8.15.5, the stretch goal) is a single world-space disc.
  - The pixel's position is rebuilt from depth and the inverse view-projection. The disc is tested in
    the XZ plane, so anything standing on it (spheres, props) is inside it too.
  - The radius is slightly noisy along the rim, so it reads as drawn rather than as a compass circle.
  - Inside, the graded scene colour comes back through the monochrome page. The rim is an ink ring
    2.5 px wide at 1080p (NDD: "at least 2 px at 1080p").
  - Fields: `island` (strength), `islandCenter`, `islandRadius`, `islandEdge`, `islandInk`.
  - `post_demo` puts one at the pale sphere while the Unwritten has any weight.

### Inverted-hull silhouettes (`hull`, `hullWidth`, `hullColor`, `hullStatic`)

![Solhaven and the Lattice in post_demo without and with the hull silhouette](style_outline_hull.png)

*Top: `post_demo --style solhaven --no-hull` (left) and `--style solhaven` (right). Bottom: the same for
`--style lattice`, whose hull is a thin night-indigo band (v2). The two pawns are skinned and get the
hull band, including where the base meets the ground and the post outline finds no depth jump. The
spheres and the standing stone keep only the depth-curvature outline.*

A second geometry pass, `Renderer::drawHulls`, runs right after the opaque lit draws, into the same
HDR target:

- Every eligible mesh is drawn again with **front faces culled**. Each vertex is pushed out along its
  normal by a constant number of **pixels**, not world units:
  - The shader takes the normal's direction on screen, `clipN.xy * clip.w - clip.xy * clipN.w`, which
    is d/dt of the projection of `p + t n`.
  - It adds `normalize(dir_px) * 2 * width / viewport * clip.w` to `clip.xy`.
  - So the band is `hullWidth` px at 1080p (scaled by the output height) whatever the distance.
  - `hullClipOffset()` in `StylePack.h` is the same maths, and is unit-tested.
- **Skinned meshes** use a SKINNED permutation that reads the same joint UBO range as the lit pass, so
  the band follows the animated pose.
- **Colour.** `hullColor` is flat and authored as a display colour. It is placed in scene-referred space
  through `acesFittedInverse` (and exposure), so it comes out of the tonemap as authored, before the
  pack's grade and monochrome remap. Fog is applied like the lit pass.
- **Skipped items:** blended, double-sided (no closed volume) and alpha-cutout items.
- **Opt-in.** `hull = 0` (None) skips the pass. `hullStatic` is a width multiplier for static
  meshes; it is 0 in every pack, so only characters get hulls unless a caller opts in
  (`post_demo --hull-static 1`).

**Why characters only: complementing the post outline, not duplicating it.** The post-process outline
(depth curvature) already draws environment silhouettes *and* creases at a width that tapers or
trembles per pack. Adding a hull to every crate would only draw the same silhouette twice. What the post
outline does badly is characters:

- thin limbs a few pixels wide, where the 3x3 depth ring straddles the whole limb;
- a character standing against a wall or another character at a similar depth, where the depth jump
  is too small relative to the distance to pass the threshold;
- the outline weakening with distance or taper on the figures the player most needs to read
  (NDD 8.15.8 readability).

So the hull gives skinned meshes a solid, constant-pixel silhouette that does not depend on what is
behind them, and the environment keeps the post line.

**Depth writes are off in the hull pass.** The post outline reads the opaque depth, so it traces the
character's true silhouette, and that line falls *inside* the hull band. The two merge into one heavier
contour instead of a double ring. The hull also never occludes anything drawn later (blended items,
particles).

**Cost.** The hull pass draws each opaque character once more (front faces, a flat-colour fragment shader,
no lighting or textures), and is counted in `RenderStats::hullDrawCalls`. Measured with `--perf-json` on llvmpipe (software GL, 4 cores):

| `post_demo --style solhaven`, 300 frames, frame-time p50 | hull on | `--no-hull` | paired difference (4 interleaved pairs) |
|---|---|---|---|
| 480x270 (the CI autoplay size) | 66.4 ms | 64.2 ms | +2.2 ms (+3%), pairs from -0.3 to +5.0 ms |
| 1280x720 | 197.3 ms | 197.2 ms | +0.1 ms, within the run-to-run noise (-0.8 to +2.0 ms) |

The scene has two skinned pawns, so the pass adds two draw calls. The cost scales with the number of
characters on screen, not with resolution: hull fragments are a thin band and the shader is a single
fog mix. On llvmpipe the run-to-run noise on a shared machine is several ms, so treat this as an upper
bound and re-measure on a GPU. A pack with `hull = 0` skips the pass entirely once both hull programs
have been primed (one collapsed draw each, which rasterises nothing).

### Hatching (`hatch`, `hatchScale`, `hatchDensity`, `hatchInk`)

![The Unwritten in post_demo: an ink drawing with hatched tone and a colour island](style_unwritten_hatch.png)

*`post_demo --style unwritten`. Tone comes from nested pen strokes: the mid-tones keep clean paper or one
stroke direction, and the cross appears only in the darks (the crates). The colour island keeps the red and
blue spheres in colour. The two pawns are skinned and carry the ink hull.*

The Unwritten is drawn as an ink drawing: a **tonal art map** (TAM) of pen strokes replaces the
halftone dots.

- **The map.** `hatchTonalArtMap()` (header-only, unit-tested) generates the map once at renderer
  start: 256x256 RGBA8, tileable, with mipmaps.
  - R holds sparse 45-degree strokes, and G doubles them.
  - B adds the cross direction, and A doubles that plus near-horizontal strokes.
  - The levels are **nested**: each darker level contains every stroke of the lighter ones, so a
    tone change adds strokes and never moves them.
  - Strokes have a hash-jittered offset, a slow wobble, a varying width and gaps, so they read as drawn
    rather than ruled.
- **In the lit shader.**
  - The tone is the lit colour's luminance pushed through the same ACES curve and gamma the tonemap
    pass will apply (`uExposure`), so hatching follows the picture the player sees.
  - The darkness `(1 - tone)^hatchCurve * hatchDensity` picks among paper, the four TAM levels and
    solid ink with hat-function weights (`hatchWeights()`, unit-tested), capped at `hatchMaxLevel`.
    Neighbouring levels cross-fade.
  - The surface becomes a flat, paper-bright base that keeps the albedo's hue (plus emissive), with
    `hatchInk` laid over it by stroke coverage. With `hatchOverlay` 1 the base is the lit colour itself
    instead, and with `hatchShadowOnly` 1 the coverage is multiplied by a mask of the sun's lowest cel
    band. That is Solhaven's shadow hatching (v2 §1): ink lines in the shadow band, the lit colour
    untouched.
  - Ink and paper are placed with `acesFittedInverse`, like the hull colour. The pack's monochrome remap
    turns the base into paper, and inside a colour island the hue comes back under the strokes.
- **Screen-locked.** The strokes are sampled at `gl_FragCoord / tile`, with a tile of `hatchScale` px
  at 1080p. This matches the halftone and the paper tooth: the paper is the screen, and lines stay a
  constant pen width at any distance.
  - The trade-off is the "shower door": strokes slide over surfaces when the camera moves.
  - Triplanar object-space strokes (NDD H0-H5) stay deferred. They need per-object scale and
    stroke-width compensation to avoid thick/thin strokes with distance.
- **Halftone is 0 in the Unwritten.** Hatching carries the tone, and dots under strokes read as noise.
  `halftone` is still a field, so a dial or blend can bring it back.

### Driver variants and warm-up

On Mesa llvmpipe a shader is compiled again for every new combination of bound textures and certain
state. That compilation allocates, and the CI gates require zero allocations after warm-up. So:

- **The hatch texture is always bound** on its own unit, `Renderer::kHatchTextureUnit` (5), for every
  lit draw in every style, including `None`.
  - `uHatch = 0` skips the branch, but the bound-texture set never changes with the pack.
  - Units 1-4 keep the existing white-texture rule.
- **Hull programs are primed.** Each hull program (static, skinned) issues one collapsed draw on the
  first HDR frame that has a mesh for it, even when the style has no hull.
  - `uHullWidth < 0` moves every vertex outside the clip volume, so nothing is rasterised.
  - The pipeline state is the same as a real hull draw.
  - So switching from Lattice to Solhaven mid-game, or turning the Style Dial, compiles nothing.
- The new lit uniforms are uploaded only when the computed `LitStyle` changes. The hull pass uses
  fixed-size state only, so steady-state frames allocate nothing.

## NDD 8.15 coverage

| NDD item | Status in v1 |
|---|---|
| 8.15.1 One shader stack, four packs; a pack is a parameter set plus permutations | **Done**: `StyleSettings`, `stylePack()`, the STYLE permutation, lerp, and the dial (8.1.2) |
| 8.15.1 Contour curve, taper, jitter, shading model (cel steps / flat), grain, halftone strength | **Done** as listed in the table above |
| 8.15.1 Palette LUT | Partly: split tint, saturation and monochrome remap. No 3D LUT. |
| 8.15.2 Environment outline: screen-space from depth and normals, width by distance and by pack | **Done** from depth curvature. Width scales by pack and resolution. Solhaven's taper adds the distance falloff. |
| 8.15.2 Constant width for the Lattice and Restored | **Done** |
| 8.15.2 Hand-drawn jitter at 8 fps (Unwritten) | **Done** (`kStyleJitterFrames = 8` frames, i.e. 7.5 fps at 60 Hz) |
| 8.15.3 Halftone: screen-space dots at 45° and a fixed pixel pitch, resolution-locked | **Done** (field `halftone`); the Unwritten now uses hatching instead |
| 8.15.3 Tonal-art-map hatching (H1-H4 stroke levels) | **Done**, screen-locked, procedural nested TAM, for the Unwritten. Object-space strokes, irregularity map and H0/H5 rules deferred. |
| 8.15.2 Inverted-hull character outlines | **Done** for skinned meshes in all four packs: constant pixel width, flat colour, opt-in for static meshes (`hullStatic`). Per-vertex weight maps deferred. |
| 8.5.1 / 8.5.2 Rim on every figure (v2: gold in Solhaven; a non-blooming cyan rim in the Lattice) | **Done** as a pack term on skinned meshes (`rimScale`, `rimTint`), plus `bloomThresholdScale` so Lattice bloom is kept for emissives |
| 8.15.4 Paper grain: screen-locked multiply, two scales of tooth, per pack, after bloom and before UI | **Done** |
| 8.15.5 Colour authority: monochrome scene, colour granted by a mask, inked edge ≥ 2 px | **Done** for one disc with a noisy radius and an ink ring |
| 8.15.10 Same budget for all packs | One full-screen pass in all cases. See [PERFORMANCE.md](PERFORMANCE.md#style-packs). |

## Deferred

These are not in v1. They are listed roughly in the order the NDD leans on them:

- **Object-space hatching** (8.15.3, H0-H5): triplanar stroke stacks that stick to surfaces, the
  irregularity map, and the Hush edge-in vignette. v1 hatching is screen-locked (see Hatching).
- **Painted per-vertex weight / load maps** for the hull outline (8.15.2). The v1 hull is a constant
  width.
- **Per-draw style override** (v2 §7 #10): hull width and colour and rim tint per renderer item, for a
  character crossing worlds and for the hero **silhouette boost** (8.15.8: +30% contour weight and a
  bright rim). The pack rim and hull are per pack only.
- **Object-space hatching moves up** (v2 §7 #11): Solhaven's coloured shadow strokes make the
  screen-locked "shower door" more visible than the Unwritten's paper did.
- **Core-shadow ink** (`celInk`, v2 §7 #8, optional) and a **black-point clip** for the Lattice
  (`blackPoint`, v2 §7 #9, optional, only if the `shadowTint` multiply muddies it). Not needed by the
  v2 screenshots: the Lattice shadows read as near-black indigo with the multiply alone.
- **Pack rim on static figures.** The rim is applied to skinned meshes only, so cast members drawn
  from static part meshes (`battle_demo`'s figures) get no pack rim.
- **Object-ID buffer** and a real **normal buffer** (MRT) for edges (8.15.2). Normals are implicit in
  the depth curvature, so a crease between two faces at the same depth slope is missed, and
  coplanar touching objects get no line between them.
- **The Third's own line** as a separate, jitter-exempt layer (8.15.2).
- **Ink bleed** (blur-and-threshold on an edge buffer).
- **The full colour-authority buffer** (8.15.5):
  - RGBA mask channels for gold, violet, overlap rose and blank;
  - emitters and island growth persisted per level;
  - the separate rose shader;
  - the **warmth mask** material flag ("made with love" objects keep colour).

  v1 has one disc, passed as a uniform.
- **Ink-Solhaven transcription** (8.15.6): material swaps, the ruled-page sky with panel borders, and
  a layout pass. The look itself exists as the `inksolhaven` preset; these scene-level parts do not.
- **Page-turn transitions** (8.15.7). The dial / `lerp` cross-blend is the only transition.
- **Combat readability masks** (8.15.8): the hatch- and FX-free cue field, the enemy white halo, and
  greyscale / colour-blind QA sweeps.
- **Accessibility switches** (8.15.9): Reduce Pattern, Calm Ink, High Contrast, and so on. Most are
  one-field overrides of a pack (`halftone = 0`, `outlineJitter = 0`, `islandEdge *= 2`). There is
  no settings UI for them yet.
- **Per-pack UI skin, FX library and camera rules** (8.15.1).
- **3D palette LUTs** per pack.
