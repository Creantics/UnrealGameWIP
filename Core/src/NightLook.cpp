#include "ftcore/NightLook.h"

#include "ftcore/LightPicker.h"
#include "ftcore/WorldClock.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ftcore {

namespace {
float lerp(float a, float b, float t) { return a + (b - a) * t; }
// From a to b in ratio (both above 0), so each step of t dims by the same share; straight when either end is 0.
float lerpRatio(float a, float b, float t) {
    return a > 0.f && b > 0.f ? a * std::pow(b / a, t) : lerp(a, b, t);
}
} // namespace

float nightDeep(const NightLook& look, float sunElevationDeg) {
    const float span = look.deepHighDeg - look.deepLowDeg;
    if (!(span > 0.f)) return sunElevationDeg < look.deepHighDeg ? 1.f : 0.f;
    const float t = std::clamp((look.deepHighDeg - sunElevationDeg) / span, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

NightFrame nightFrame(const NightLook& look, float night) { return nightFrame(look, night, night); }

NightFrame nightFrame(const NightLook& look, float night, float deep) {
    NightFrame f;
    const float n = std::clamp(night, 0.f, 1.f);
    const float d = std::clamp(deep, 0.f, 1.f);
    const float moon = std::max(look.moonShare, 1e-4f);
    // The fill as twilight ends, then down in ratio to the deep night's; the dome from the day's down to its own.
    const float fill = lerpRatio(std::max(look.twilightFill, 0.f), std::max(look.skyFill, 0.f), d);
    f.night = n;
    f.deep = d;
    f.sunScale = lerp(1.f, moon, n);
    f.skyScale = lerp(1.f, fill, n);
    f.skyLuminance = lerpRatio(1.f, std::clamp(look.skyLuminance, 1e-4f, 1.f), d);
    // The capture also sees the dome's own dimming; lift it by that too, so the fill stands where it did.
    f.skyScaleRealTime = lerp(1.f, fill / moon, n) / f.skyLuminance;
    f.tintMix = n;
    f.fogKeep = lerp(1.f, std::max(look.fogKeep, 0.f), n);
    for (std::size_t c = 0; c < 3; ++c) {
        f.fogGlow[c] = std::max(look.glow[c], 0.f) * n;
    }
    // A still (no sun given): the switch-on and the exposure follow the night alone.
    f.lockExposure = n >= kExposureLockAt;
    f.exposureBlend = f.lockExposure ? 1.f : 0.f;
    f.lamps = n;
    f.exposureEV100 = look.exposureEV100;
    const float range = std::max(look.exposureRangeEV, 0.f);
    f.exposureMinEV100 = look.exposureEV100 - range;
    f.exposureMaxEV100 = look.exposureEV100 + range;
    f.whiteMix = n;
    f.wetShare = lerp(std::clamp(look.dayWetShare, 0.f, 1.f), 1.f, n);
    f.whiteKelvin = std::clamp(look.whiteKelvin, 1500.f, 15000.f);
    return f;
}

namespace {
// 0 at hi, 1 at lo (hi above lo), eased; a step at hi when the span is empty.
float easeDown(float hi, float lo, float x) {
    if (!(hi > lo)) return x < hi ? 1.f : 0.f;
    const float t = std::clamp((hi - x) / (hi - lo), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
} // namespace

float nightLampFraction(const NightLook& look, float sunElevationDeg) {
    return easeDown(look.lampsOnHighDeg, look.lampsOnLowDeg, sunElevationDeg);
}

bool nightLampOn(float fraction, std::uint32_t key) {
    if (fraction <= 0.f) return false;
    if (fraction >= 1.f) return true;
    // A 32-bit finaliser (murmur3's): neighbouring keys land far apart.
    std::uint32_t h = key;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    const float at = 0.05f + 0.9f * static_cast<float>(h & 0xFFFFu) / 65535.f;
    return fraction >= at;
}

float nightExposureBlend(const NightLook& look, float sunElevationDeg) {
    return easeDown(look.exposureHighDeg, look.exposureLowDeg, sunElevationDeg);
}

float nightMoonScale(const NightLook& look, float sunElevationDeg) {
    if (sunElevationDeg >= 0.f) return 1.f;
    const float rise = std::clamp(look.moonRiseShare, 0.f, 1.f);
    // The moon's elevation is the sun's depth: eased up from moonRiseDeg to moonFullDeg.
    return lerp(rise, 1.f, easeDown(-look.moonRiseDeg, -look.moonFullDeg, sunElevationDeg));
}

NightFrame nightFrameAt(const NightLook& look, float night, float sunElevationDeg) {
    NightFrame f = nightFrame(look, night, nightDeep(look, sunElevationDeg));
    const float n = f.night;
    const float moon = std::max(look.moonShare, 1e-4f);
    f.lamps = nightLampFraction(look, sunElevationDeg);
    f.exposureBlend = nightExposureBlend(look, sunElevationDeg);
    f.lockExposure = f.exposureBlend > 0.f;
    f.moonScale = nightMoonScale(look, sunElevationDeg);
    // The sun dims with the night while it is up; once down, the light is the moon at its elevation's share.
    f.sunScale = sunElevationDeg >= 0.f ? lerp(1.f, moon, n) : std::max(moon * f.moonScale, 1e-4f);
    // The real-time capture sees the atmosphere lit by that light and the dome's own dimming: lift by both, so the
    // fill stands at skyScale's share of the day's on either side of the horizon.
    f.skyScaleRealTime = f.skyScale / f.sunScale / f.skyLuminance;
    return f;
}

float nightSceneKey(const NightLook& look, const NightKeyModel& model, float night, float sunElevationDeg) {
    const NightFrame f = nightFrameAt(look, night, sunElevationDeg);
    const float lit = std::max(model.fillWeight, 0.f) * f.skyScale + std::max(model.lampWeight, 0.f) * f.lamps;
    // The day's range opens the camera dayOpenEV past the night's; it closes to the night's by the exposure blend.
    const float open = std::exp2((1.f - f.exposureBlend) * std::max(model.dayOpenEV, 0.f));
    return std::min(std::max(model.dayKey, 0.f), lit * open);
}

float nightMoonKey(const NightLook& look, const NightKeyModel& model, float sunElevationDeg) {
    if (sunElevationDeg >= 0.f) return 0.f;
    const float up = std::sin(std::min(-sunElevationDeg, 90.f) * 0.017453293f);
    return std::max(model.moonWeight, 0.f) * std::max(look.moonShare, 0.f) * nightMoonScale(look, sunElevationDeg) * up;
}

float nightCharacterFillLux(const NightLook& look, float night) {
    const float ref = std::max(look.charFillRefM, 0.1f);
    const float key = std::max(look.lampCandela, 0.f) / (ref * ref);
    return key * std::exp2(-std::max(look.charFillStops, 0.f)) * std::clamp(night, 0.f, 1.f);
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
