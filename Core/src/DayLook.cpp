#include "ftcore/DayLook.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {
constexpr float kPi = 3.14159265358979f;
float mix(float a, float b, float t) { return a + (b - a) * t; }
float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }
float amountOf(float amount) { return (amount == amount) ? clamp01(amount) : 0.f; }

// A tint (hue in degrees, OKLab chroma) blended toward another as a vector in a/b, so the hue takes the short way and
// a tint at zero chroma has no hue to jump from.
void mixTint(float& hueDeg, float& chroma, float toHueDeg, float toChroma, float t) {
    const float r0 = hueDeg * kPi / 180.f;
    const float r1 = toHueDeg * kPi / 180.f;
    const float a = mix(chroma * std::cos(r0), toChroma * std::cos(r1), t);
    const float b = mix(chroma * std::sin(r0), toChroma * std::sin(r1), t);
    chroma = std::sqrt(a * a + b * b);
    if (chroma > 0.f) {
        float h = std::atan2(b, a) * 180.f / kPi;
        if (h < 0.f) h += 360.f;
        hueDeg = h;
    }
}
} // namespace

DayShadeLook dayLookShade(const DayShadeLook& defaults, const DayLookSpec& spec) {
    // Step 1 .. 3 of the bracket: sky light, shadow contrast, highlight contrast, diffuse boost.
    static constexpr float kSteps[kDayLookShadeSteps][4] = {
        {2.0f, 0.6f, 0.85f, 1.6f},
        {1.7f, 0.7f, 0.825f, 1.45f},
        {1.4f, 0.8f, 0.8f, 1.3f},
    };
    DayShadeLook l = defaults;
    if (spec.shadeStep >= 1 && spec.shadeStep <= kDayLookShadeSteps) {
        const float* s = kSteps[spec.shadeStep - 1];
        l.skyScale = s[0];
        l.shadowContrast = s[1];
        l.highlightContrast = s[2];
        l.diffuseBoost = s[3];
    }
    if (spec.fogMaxOpacity >= 0.f) l.fogMaxOpacity = clamp01(spec.fogMaxOpacity);
    return l;
}

SceneLook dayLookScene(const SceneLook& row, const DayLookSpec& spec, float amount, bool heldDay) {
    SceneLook l = row;
    // Held at every hour (see the header): the wind never changes speed, the cloud layer never changes material. A look
    // set at night (heldDay false) keeps the row's own all session.
    if (heldDay && spec.windMps >= 0.f) l.windMps = std::max(row.windMps, std::min(spec.windMps, 40.f));
    if (heldDay && !(row.cloudCover > 0.f) && spec.cloudCover >= 0.f) l.cloudCover = clamp01(spec.cloudCover);
    const float a = amountOf(amount);
    if (a <= 0.f) return l;
    if (spec.fogDensity >= 0.f) l.fogDensity = mix(row.fogDensity, std::min(spec.fogDensity, 1.f), a);
    if (spec.sunKelvin > 0.f) l.sunKelvin = mix(row.sunKelvin, std::clamp(spec.sunKelvin, 1500.f, 15000.f), a);
    if (spec.shadowTintChroma >= 0.f) {
        mixTint(l.shadowTintHue, l.shadowTintChroma, spec.shadowTintHue, std::min(spec.shadowTintChroma, 0.05f), a);
    }
    if (spec.highlightTintChroma >= 0.f) {
        mixTint(l.highlightTintHue, l.highlightTintChroma, spec.highlightTintHue, std::min(spec.highlightTintChroma, 0.05f),
                a);
    }
    return l;
}

bool dayLookCloudsOn(float night) { return (night == night) && night < 1.f; }

float dayLookSignStrength(const DayLookSpec& spec, float nightStrength, float sunLux, float sunElevationDeg,
                          float lights) {
    const float n = std::max(nightStrength, 0.f);
    const float lit = (lights == lights) ? clamp01(lights) : 1.f;
    if (lit >= 1.f || spec.signWhiteRatio < 0.f) return n;
    const float elev = (sunElevationDeg == sunElevationDeg) ? sunElevationDeg : -90.f;
    const float lux = (sunLux == sunLux) ? std::max(sunLux, 0.f) : 0.f;
    // A white face's luminance under the sun and the sky (a Lambert face: illuminance over pi).
    const float white = lux * (std::max(std::sin(elev * kPi / 180.f), 0.f) + std::max(spec.signSkyShare, 0.f)) / kPi;
    const float day = spec.signWhiteRatio * white;
    return mix(day, n, lit);
}

float dayLookContactShadow(const DayLookSpec& spec, float own, float amount) {
    const float a = amountOf(amount);
    if (a <= 0.f || spec.contactShadowLength < 0.f) return own;
    return mix(own, std::min(spec.contactShadowLength, 1.f), a);
}

} // namespace ftcore
