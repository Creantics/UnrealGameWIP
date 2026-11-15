#include "ftcore/NightLook.h"

#include "ftcore/LightPicker.h"
#include "ftcore/WorldClock.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ftcore {

namespace {
float lerp(float a, float b, float t) { return a + (b - a) * t; }
} // namespace

NightFrame nightFrame(const NightLook& look, float night) {
    NightFrame f;
    const float n = std::clamp(night, 0.f, 1.f);
    const float moon = std::max(look.moonShare, 1e-4f);
    const float fill = std::max(look.skyFill, 0.f);
    f.night = n;
    f.sunScale = lerp(1.f, moon, n);
    f.skyScale = lerp(1.f, fill, n);
    f.skyScaleRealTime = lerp(1.f, fill / moon, n);
    f.tintMix = n;
    f.fogKeep = lerp(1.f, std::max(look.fogKeep, 0.f), n);
    for (std::size_t c = 0; c < 3; ++c) {
        f.fogGlow[c] = std::max(look.glow[c], 0.f) * n;
    }
    f.lockExposure = n >= kExposureLockAt;
    f.exposureEV100 = look.exposureEV100;
    const float range = std::max(look.exposureRangeEV, 0.f);
    f.exposureMinEV100 = look.exposureEV100 - range;
    f.exposureMaxEV100 = look.exposureEV100 + range;
    f.whiteMix = n;
    f.wetShare = lerp(std::clamp(look.dayWetShare, 0.f, 1.f), 1.f, n);
    f.whiteKelvin = std::clamp(look.whiteKelvin, 1500.f, 15000.f);
    return f;
}

float nightLampScale(const NightLook& look, std::size_t neighbours) {
    const float share = 1.f / (1.f + std::max(look.capShare, 0.f) * static_cast<float>(neighbours));
    return std::clamp(share, std::clamp(look.capFloor, 0.f, 1.f), 1.f);
}

void nightLampScales(const NightLook& look, const float* xy, std::size_t count, float unitsPerMetre, float* out) {
    if (count == 0 || !xy || !out) {
        return;
    }
    const float radius = std::max(look.capRadiusM, 0.f) * std::max(unitsPerMetre, 1e-6f);
    if (radius <= 0.f) {
        std::fill(out, out + count, 1.f);
        return;
    }
    LightPicker picker;
    picker.build(xy, count, radius);
    std::vector<std::uint32_t> hits(kNightCapCount + 1);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t found = picker.pick(xy[i * 2], xy[i * 2 + 1], radius, hits.size(), hits.data());
        out[i] = nightLampScale(look, found > 0 ? found - 1 : 0);
    }
}

namespace {
float smooth01(float x) {
    const float t = std::clamp(x, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
std::array<float, 3> mix3(const std::array<float, 3>& to, float t) {
    return {lerp(1.f, to[0], t), lerp(1.f, to[1], t), lerp(1.f, to[2], t)};
}
} // namespace

float duskAmount(const DuskLook& look, float sunElevationDeg, bool evening) {
    const float hi = look.startDeg, peak = look.peakDeg, lo = look.endDeg;
    if (!(hi > peak && peak > lo)) return 0.f;
    float a = 0.f;
    if (sunElevationDeg >= peak) a = smooth01((hi - sunElevationDeg) / (hi - peak));
    else a = smooth01((sunElevationDeg - lo) / (peak - lo));
    return evening ? a : a * std::clamp(look.morningShare, 0.f, 1.f);
}

DuskFrame duskFrame(const DuskLook& look, float amount) {
    DuskFrame f;
    const float a = std::clamp(amount, 0.f, 1.f);
    f.amount = a;
    f.sunMul = mix3(look.sunTint, a);
    f.skyGlow = mix3(look.skyGlow, a);
    f.fillMul = mix3(look.fillTint, a);
    f.fillScale = lerp(1.f, std::max(look.fillScale, 0.f), a);
    f.fogMul = mix3(look.fogTint, a);
    f.flareMix = a;
    f.flare = std::max(look.flare, 0.f);
    f.flareThreshold = std::max(look.flareThreshold, 0.1f);
    f.bloomAdd = std::max(look.bloomAdd, 0.f) * a;
    return f;
}

namespace {
// The hour of the evening the sun stands `deg` up: it sinks through the afternoon, so bisect 12:00..24:00.
double eveningHourAt(float deg, float latitudeDeg) {
    WorldClockSpec spec;
    spec.latitudeDeg = latitudeDeg;
    double a = 12.0, b = 24.0;
    if (sunElevationDeg(spec, a) < deg) return a;
    if (sunElevationDeg(spec, b) > deg) return b;
    for (int i = 0; i < 40; ++i) {
        const double m = 0.5 * (a + b);
        if (sunElevationDeg(spec, m) > deg) a = m; else b = m;
    }
    return 0.5 * (a + b);
}
} // namespace

double duskPeakHour(const DuskLook& look, float latitudeDeg) { return eveningHourAt(look.peakDeg, latitudeDeg); }

double duskStillHour(const DuskLook& look, float latitudeDeg) { return eveningHourAt(look.stillDeg, latitudeDeg); }

float duskLights(const DuskLook& look, float sunElevationDeg) {
    if (!(look.lightsOnDeg > look.lightsFullDeg)) return 0.f;
    return smooth01((look.lightsOnDeg - sunElevationDeg) / (look.lightsOnDeg - look.lightsFullDeg));
}

float duskDaylight(const DuskLook& look, float sunElevationDeg) {
    if (!(look.dayFullDeg > look.dayEndDeg)) return sunElevationDeg > look.dayEndDeg ? 1.f : 0.f;
    return smooth01((sunElevationDeg - look.dayEndDeg) / (look.dayFullDeg - look.dayEndDeg));
}

} // namespace ftcore
