#include "ftcore/NightLook.h"

#include "ftcore/LightPicker.h"

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

} // namespace ftcore
