#include "ftcore/ArenaLook.h"

#include <cmath>

namespace ftcore {

namespace {

constexpr float kDegToRad = 3.14159265358979f / 180.f;

} // namespace

float arenaClampViewPitch(const ArenaLookLimits& limits, float pitch) {
    if (!(pitch == pitch)) return 0.f;
    return pitch < limits.viewMin ? limits.viewMin : (pitch > limits.viewMax ? limits.viewMax : pitch);
}

ArenaPitchSplit arenaSplitPitch(const ArenaLookLimits& limits, float viewPitch) {
    const float view = arenaClampViewPitch(limits, viewPitch);
    ArenaPitchSplit s;
    s.boom = view > limits.boomMax ? limits.boomMax : view;
    s.tilt = view - s.boom;
    return s;
}

float arenaEyeHeight(float pivot, float arm, float up, float boomPitch) {
    const float r = boomPitch * kDegToRad;
    return pivot - arm * std::sin(r) + up * std::cos(r);
}

float arenaEyeBack(float arm, float up, float boomPitch) {
    const float r = boomPitch * kDegToRad;
    return arm * std::cos(r) + up * std::sin(r);
}

float arenaSightHeightAt(const ArenaLookLimits& limits, float pivot, float arm, float up, float away, float viewPitch) {
    const float view = arenaClampViewPitch(limits, viewPitch);
    const ArenaPitchSplit s = arenaSplitPitch(limits, view);
    const float along = arenaEyeBack(arm, up, s.boom) + away;
    return arenaEyeHeight(pivot, arm, up, s.boom) + along * std::tan(view * kDegToRad);
}

float arenaLookUpPitch(const ArenaLookLimits& limits, float pivot, float arm, float up, float away, float height,
                       float floorPitch) {
    const float lo = arenaClampViewPitch(limits, floorPitch);
    // The sight's height at `away` rises with the pitch (the lens drops as the boom swings up, but slower than the
    // line climbs over the arm's length ahead of it): bisect it.
    float a = lo, b = limits.viewMax;
    if (arenaSightHeightAt(limits, pivot, arm, up, away, a) >= height) return a;
    if (arenaSightHeightAt(limits, pivot, arm, up, away, b) <= height) return b;
    for (int i = 0; i < 40; ++i) {
        const float m = 0.5f * (a + b);
        if (arenaSightHeightAt(limits, pivot, arm, up, away, m) < height) a = m;
        else b = m;
    }
    return 0.5f * (a + b);
}

} // namespace ftcore
