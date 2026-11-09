#include "ftcore/LookGrade.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.f;

float clamp01(float x) { return std::clamp(x, 0.f, 1.f); }
float mix(float a, float b, float t) { return a + (b - a) * t; }

float srgbDecode(float x) { return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f); }
float srgbEncode(float x) { return x <= 0.0031308f ? x * 12.92f : 1.055f * std::pow(x, 1.f / 2.4f) - 0.055f; }

struct Lab {
    float L, a, b;
};

// Bjorn Ottosson's OKLab (2020): linear sRGB -> LMS (M1), cube root, -> Lab (M2); and back.
Lab toOklab(float r, float g, float b) {
    const float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
            1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
            0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}

void fromOklab(const Lab& c, float rgb[3]) {
    const float l = c.L + 0.3963377774f * c.a + 0.2158037573f * c.b;
    const float m = c.L - 0.1055613458f * c.a - 0.0638541728f * c.b;
    const float s = c.L - 0.0894841775f * c.a - 1.2914855480f * c.b;
    const float l3 = l * l * l, m3 = m * m * m, s3 = s * s * s;
    rgb[0] = +4.0767416621f * l3 - 3.3077115913f * m3 + 0.2309699292f * s3;
    rgb[1] = -1.2684380046f * l3 + 2.6097574011f * m3 - 0.3413193965f * s3;
    rgb[2] = -0.0041960863f * l3 - 0.7034186147f * m3 + 1.7076147010f * s3;
}

Lab srgbToOklab(const float srgb[3]) {
    return toOklab(srgbDecode(srgb[0]), srgbDecode(srgb[1]), srgbDecode(srgb[2]));
}

// A hue window's weight: 1 at its centre, 0 at its width (smoothstep between); 0 for a width of 0.
float windowWeight(float hue, float centre, float width) {
    if (width <= 0.f) return 0.f;
    float d = hue - centre;
    d -= 2.f * kPi * std::floor((d + kPi) / (2.f * kPi)); // wrapped to [-pi, pi)
    const float t = clamp01(1.f - std::fabs(d) / width);
    return t * t * (3.f - 2.f * t);
}

} // namespace

GradeConstants gradeConstants(const SceneLook& look, const FightAccent& accent) {
    GradeConstants g;
    g.c[0][0] = look.gradeChroma;
    g.c[0][1] = look.skinFloor;
    g.c[0][2] = look.skinHue * kDegToRad;
    g.c[0][3] = look.skinWidth * kDegToRad;
    g.c[1][0] = look.keepAHue * kDegToRad;
    g.c[1][1] = look.keepAWidth * kDegToRad;
    g.c[1][2] = look.keepAChroma;
    g.c[2][0] = look.keepBHue * kDegToRad;
    g.c[2][1] = look.keepBWidth * kDegToRad;
    g.c[2][2] = look.keepBChroma;
    g.c[3][0] = accent.hueDeg * kDegToRad;
    g.c[3][1] = accent.widthDeg * kDegToRad;
    g.c[3][2] = accent.chroma;
    const float shHue = look.shadowTintHue * kDegToRad, hiHue = look.highlightTintHue * kDegToRad;
    g.c[4][0] = look.shadowTintChroma * std::cos(shHue);
    g.c[4][1] = look.shadowTintChroma * std::sin(shHue);
    g.c[4][2] = look.highlightTintChroma * std::cos(hiHue);
    g.c[4][3] = look.highlightTintChroma * std::sin(hiHue);
    return g;
}

bool gradeIsIdentity(const GradeConstants& g) {
    if (g.c[0][0] != 1.f) return false;
    float least = g.c[0][0];
    for (int w = 1; w <= 3; ++w) {
        if (g.c[w][1] <= 0.f) continue; // width 0: off
        if (g.c[w][2] != 1.f) return false;
        least = std::min(least, g.c[w][2]);
    }
    if (g.c[0][1] > least) return false;
    for (const float t : g.c[4])
        if (t != 0.f) return false;
    return true;
}

void gradeSrgb(const GradeConstants& g, float rgb[3]) {
    const Lab in = srgbToOklab(rgb);
    const float chroma = std::sqrt(in.a * in.a + in.b * in.b);
    const float hue = std::atan2(in.b, in.a);
    const float neutral = clamp01(chroma / 0.02f);

    float s = g.c[0][0];
    s = mix(s, g.c[1][2], windowWeight(hue, g.c[1][0], g.c[1][1]) * neutral);
    s = mix(s, g.c[2][2], windowWeight(hue, g.c[2][0], g.c[2][1]) * neutral);
    s = mix(s, g.c[3][2], windowWeight(hue, g.c[3][0], g.c[3][1]) * neutral);
    const float skin = windowWeight(hue, g.c[0][2], g.c[0][3]) * neutral * clamp01((in.L - 0.25f) / 0.1f) *
                       clamp01((0.95f - in.L) / 0.1f);
    s = mix(s, std::max(s, g.c[0][1]), skin);

    const float dark = (1.f - in.L) * (1.f - in.L), light = in.L * in.L;
    const Lab out{in.L, in.a * s + g.c[4][0] * dark + g.c[4][2] * light,
                  in.b * s + g.c[4][1] * dark + g.c[4][3] * light};
    float lin[3];
    fromOklab(out, lin);
    for (int i = 0; i < 3; ++i) rgb[i] = clamp01(srgbEncode(clamp01(lin[i])));
}

float oklabHueDeg(const float srgb[3]) {
    const Lab c = srgbToOklab(srgb);
    float deg = std::atan2(c.b, c.a) / kDegToRad;
    if (deg < 0.f) deg += 360.f;
    return deg;
}

float oklabChroma(const float srgb[3]) {
    const Lab c = srgbToOklab(srgb);
    return std::sqrt(c.a * c.a + c.b * c.b);
}

} // namespace ftcore
