#include "ftcore/RainFall.h"

#include <algorithm>
#include <cmath>

namespace ftcore {
namespace {

float rainFallUnit(float v) { return std::isfinite(v) ? std::clamp(v, 0.f, 1.f) : (v > 0.f ? 1.f : 0.f); }
float rainFallRate(float perS, float i) { return std::isfinite(perS) ? std::max(perS, 0.f) * i : 0.f; }

}  // namespace

RainFallFrame rainFallFrame(const RainSpec& rain, const RainFallSpec& spec, float intensity) {
    const float i = rainFallUnit(intensity);
    RainFallFrame f;
    if (i <= 0.f) return f;
    f.streakRate = rainFallRate(spec.streaksPerSAtFull, i);
    f.ringRate = rainFallRate(spec.ringsPerSAtFull, i);
    f.dropRate = rainFallRate(spec.dropsPerSAtFull, i);
    f.streakAlpha = rainStreakAlpha(rain, i);
    return f;
}

float rainColumnTop(const RainFallSpec& spec, float camZ, bool hasFloor, float floorZ) {
    const float top = camZ + spec.aboveCamM;
    if (!hasFloor || !std::isfinite(floorZ)) return top;
    return std::min(top, floorZ + spec.columnM);
}

bool rainSplashOn(const RainFallSpec& spec, float camZ, bool hasFloor, float floorZ) {
    if (!hasFloor || !std::isfinite(camZ) || !std::isfinite(floorZ)) return false;
    const float over = camZ - floorZ;
    return over >= -spec.splashBelowM && over <= spec.splashMaxDropM;
}

}  // namespace ftcore
