#include "ftcore/DayShade.h"

#include <algorithm>

namespace ftcore {

namespace {
float mix(float a, float b, float t) { return a + (b - a) * t; }
float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }
} // namespace

float dayShadeAmount(float night, float dusk) {
    // NaN reads as full night (no lift).
    if (!(night == night) || !(dusk == dusk)) return 0.f;
    // Gone by a fifth of the dusk, so the dusk's own dimmed fill (its whole ramp, not only its peak) stands.
    const float d = 1.f - clamp01(dusk / kDayShadeDuskCut);
    return (1.f - clamp01(night)) * d * d;
}

DayShadeFrame dayShadeFrame(const DayShadeLook& look, const DayShadeBase& base, float amount) {
    const float a = (amount == amount) ? clamp01(amount) : 0.f;
    DayShadeFrame f;
    f.amount = a;
    f.skyScale = mix(1.f, std::max(look.skyScale, 0.f), a);
    f.shadowContrast = mix(base.shadowContrast, clamp01(look.shadowContrast), a);
    f.diffuseBoost = mix(base.diffuseBoost, std::clamp(look.diffuseBoost, 1.f, 4.f), a);
    f.filmToe = mix(base.filmToe, clamp01(look.filmToe), a);
    return f;
}

} // namespace ftcore
