#include "ftcore/Weather.h"

#include <algorithm>
#include <cmath>

namespace ftcore {
namespace {

float rainUnit(float v) { return std::isfinite(v) ? std::clamp(v, 0.f, 1.f) : 0.f; }

// v moved toward goal by at most up (rising) or down (falling).
float rainToward(float v, float goal, float up, float down) {
    if (v < goal) return std::min(goal, v + up);
    return std::max(goal, v - down);
}

float rainRate(float span) { return span > 0.f && std::isfinite(span) ? 1.f / span : 1e9f; }
float rainPerS(float r) { return std::isfinite(r) ? std::max(r, 0.f) : 0.f; }

}  // namespace

float rainSoakTarget(const RainSpec& spec, float intensity) {
    const float i = rainUnit(intensity);
    if (i <= 0.f) return 0.f;
    return spec.soakAtIntensity > 0.f ? std::min(1.f, i / spec.soakAtIntensity) : 1.f;
}

static float rainPuddleTarget(const RainSpec& spec, float wetness) {
    const float from = std::clamp(spec.puddleFrom, 0.f, 0.999f);
    return std::max(0.f, (rainUnit(wetness) - from) / (1.f - from));
}

RainState rainStep(const RainSpec& spec, RainState s, float dtS) {
    s.target = rainUnit(s.target);
    s.intensity = rainUnit(s.intensity);
    s.wetness = rainUnit(s.wetness);
    s.puddle = rainUnit(s.puddle);
    if (!(dtS > 0.f) || !std::isfinite(dtS)) return s;
    const float dt = std::min(dtS, kRainMaxStepS);
    s.intensity = rainToward(s.intensity, s.target, dt * rainRate(spec.rampUpS), dt * rainRate(spec.rampDownS));
    s.wetness = rainToward(s.wetness, rainSoakTarget(spec, s.intensity), dt * rainPerS(spec.wetRisePerS), dt * rainPerS(spec.dryPerS));
    s.puddle = rainToward(s.puddle, rainPuddleTarget(spec, s.wetness), dt * rainPerS(spec.puddleRisePerS), dt * rainPerS(spec.puddleDryPerS));
    return s;
}

RainState rainSettled(const RainSpec& spec, float target) {
    RainState s;
    s.target = s.intensity = rainUnit(target);
    s.wetness = rainSoakTarget(spec, s.intensity);
    s.puddle = rainPuddleTarget(spec, s.wetness);
    return s;
}

float weatherGroundWetness(float lookWet, float wetShare, const RainState& rain) {
    return std::max(rainUnit(lookWet) * rainUnit(wetShare), rainUnit(rain.wetness));
}

float leadSurfaceWetness(const RainState& rain, float ownWetness, float exposure) {
    return std::max(rainUnit(rain.wetness) * rainUnit(exposure), rainUnit(ownWetness));
}

int rainBand(float intensity) {
    const float i = rainUnit(intensity);
    if (i <= 0.f) return 0;
    if (i < 1.f / 3.f) return 1;
    if (i < 2.f / 3.f) return 2;
    return 3;
}

float rainStreakAlpha(const RainSpec& spec, float intensity) {
    const float i = rainUnit(intensity);
    if (i <= 0.f) return 0.f;
    const float lo = rainUnit(spec.streakAlphaMin);
    return lo + (1.f - lo) * i;
}

float rainMistScale(const RainSpec& spec, float intensity) {
    return 1.f + (spec.mistAtFull - 1.f) * rainUnit(intensity);
}

}  // namespace ftcore
