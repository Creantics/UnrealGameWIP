#include "ftcore/DayShade.h"

#include <algorithm>
#include <cstddef>

namespace ftcore {

namespace {
float mix(float a, float b, float t) { return a + (b - a) * t; }
float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }
} // namespace

float dayShadeAmount(const DayShadeFade& fade, float night, float sunElevationDeg) {
    // NaN reads as full night (no lift).
    if (!(night == night) || !(sunElevationDeg == sunElevationDeg)) return 0.f;
    float up = sunElevationDeg >= fade.highDeg ? 1.f : 0.f;
    if (fade.highDeg > fade.lowDeg) {
        const float t = clamp01((sunElevationDeg - fade.lowDeg) / (fade.highDeg - fade.lowDeg));
        up = t * t * (3.f - 2.f * t);
    }
    return (1.f - clamp01(night)) * up;
}

DayShadeFrame dayShadeFrame(const DayShadeLook& look, const DayShadeBase& base, float amount) {
    const float a = (amount == amount) ? clamp01(amount) : 0.f;
    DayShadeFrame f;
    f.amount = a;
    f.skyScale = mix(1.f, std::max(look.skyScale, 0.f), a);
    f.shadowContrast = mix(base.shadowContrast, clamp01(look.shadowContrast), a);
    f.diffuseBoost = mix(base.diffuseBoost, std::clamp(look.diffuseBoost, 1.f, 4.f), a);
    f.filmToe = mix(base.filmToe, clamp01(look.filmToe), a);
    f.highlightContrast = mix(base.highlightContrast, std::min(base.highlightContrast, clamp01(look.highlightContrast)), a);
    const float minEV = (look.minEV100 == look.minEV100) ? std::clamp(look.minEV100, -15.f, 25.f) : base.minEV100;
    f.minEV100 = mix(base.minEV100, std::max(base.minEV100, minEV), a);
    f.fogMaxOpacity = mix(1.f, clamp01(look.fogMaxOpacity), a);
    for (std::size_t i = 0; i < 3; ++i) {
        f.fogSky[i] = mix(1.f, clamp01(look.fogSky[i]), a);
    }
    return f;
}

} // namespace ftcore
