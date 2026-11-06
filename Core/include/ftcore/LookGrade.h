#pragma once

// The after-tonemap grade of a world's look, in OKLab: the reference the HLSL pass mirrors line by line.
// It scales chroma (colour strength) globally, then per hue window, keeps skin tones from going grey, and
// leans the shadows and highlights toward a hue (split tone). Lightness is never touched, so the picture's
// exposure and contrast stay the tonemapper's; only how coloured it is, and which way, changes.
//
//   ftcore::FightAccent accent;                                  // a fight sets this at runtime; width 0 = off
//   const ftcore::GradeConstants g = ftcore::gradeConstants(look, accent);   // once per look change
//   if (!ftcore::gradeIsIdentity(g)) ...;                        // skip the pass when it would change nothing
//   ftcore::gradeSrgb(g, rgb);                                   // per pixel: sRGB-encoded display values in/out
//
// The steps of gradeSrgb (OKLab = Bjorn Ottosson's published sRGB matrices, see LookGrade.cpp):
//   1. Decode sRGB (the exact EOTF) and convert to OKLab: L, a, b; C = |(a, b)|, h = atan2(b, a).
//   2. A hue window's weight is smoothstep(1 - d / width) for d, the hue's distance from the window's centre
//      (wrapped to 0..pi); 0 when its width is 0. Every window is multiplied by a neutral weight, C / 0.02
//      clamped to 0..1, because a near-grey has no stable hue.
//   3. The chroma scale starts at gradeChroma and blends toward each window's own (keep A, keep B, the fight
//      accent) by its weight. Skin tones (the skin window, between L 0.25 and 0.95 with 0.1 ramps) then raise
//      the scale to at least skinFloor.
//   4. a and b are scaled, then moved by the split tone: the shadow tint (1 - L)^2, the highlight tint L^2.
//   5. Back to linear sRGB, clamped to 0..1, and encoded.
//
// Plain value maths, no allocation: safe to call every frame (the GPU does the per-pixel work).

#include "ftcore/Api.h"
#include "ftcore/SceneLook.h"

namespace ftcore {

// What a fight adds to the grade: one more hue window with its own chroma scale (width 0 = off).
struct FightAccent {
    float hueDeg = 0.f, widthDeg = 0.f, chroma = 1.f;
};

// The grade as the shader takes it: five float4s, hues and widths in radians.
//   c[0] = { gradeChroma, skinFloor, skinHue, skinWidth }
//   c[1] = { keepAHue, keepAWidth, keepAChroma, 0 }
//   c[2] = { keepBHue, keepBWidth, keepBChroma, 0 }
//   c[3] = { accent hue, accent width, accent chroma, 0 }
//   c[4] = { shA, shB, hiA, hiB }: the tint offsets, tintChroma * cos(hue) and * sin(hue), shadows then highlights
struct GradeConstants {
    float c[5][4] = {};
};

FTCORE_API GradeConstants gradeConstants(const SceneLook& look, const FightAccent& accent);

// True when the pass would change nothing: gradeChroma is 1, every window has width 0 or chroma 1, skinFloor
// is no more than the least scale any pixel can get, and all four tint offsets are 0.
FTCORE_API bool gradeIsIdentity(const GradeConstants& g);

// In place; sRGB-encoded display values in 0..1 in and out.
FTCORE_API void gradeSrgb(const GradeConstants& g, float rgb[3]);

// The OKLab hue (degrees, 0 to 360) and chroma of an sRGB-encoded colour, for tests and tooling.
FTCORE_API float oklabHueDeg(const float srgb[3]);
FTCORE_API float oklabChroma(const float srgb[3]);

} // namespace ftcore
