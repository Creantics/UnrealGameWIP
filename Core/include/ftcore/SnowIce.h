#pragma once

// Snow and ice underfoot (winter weather): what a lead's step reads from the ground under it. Stateless rules, no
// allocation; the host's movement asks them each step.
//   - Trudge: snow slows a lead by its depth, not at all below trudgeStartM, to trudgeMinScale at trudgeFullM and deeper.
//   - Frozen water: ice at least iceHoldM thick holds a lead (thinner is open water: swim). It has little grip, and a
//     sharp turn or a stop at speed slips: a short skid with no steering. A slip is a skid, never a fall or a hurt.
//   - Ice patches: a thin skin on pavement lowers grip a little and never slips, hurts or knocks a lead down.
// Snow deep enough to trudge in wins over the ice under it (snow on ice walks like snow).

#include <cmath>
#include <cstdint>

namespace ftcore {

enum class Footing : std::uint8_t { Ground, Snow, IcePatch, FrozenWater, OpenWater };

struct SnowIceTuning {
    float trudgeStartM = 0.05f;   // snow shallower than this doesn't slow a lead
    float trudgeFullM = 0.60f;    // at this depth and deeper, the slowest
    float trudgeMinScale = 0.45f; // speed scale at trudgeFullM
    float iceHoldM = 0.08f;       // frozen water at least this thick holds a lead
    float snowGrip = 0.85f;       // share of dry ground's grip (braking and turning acceleration)
    float patchGrip = 0.60f;
    float iceGrip = 0.15f;
    float slipSpeedMS = 3.0f;     // at or above this ground speed on frozen water ...
    float slipTurnDeg = 60.f;     // ... a turn this sharp in one step, or a stop, slips
    float slipS = 0.6f;           // the skid's length, no steering
};

// Finite, ordered and in range: 0 <= start < full; 0 < minScale <= 1; iceHold > 0; 0 < iceGrip <= patchGrip <= 1
// (a patch never grips worse than frozen water: it stays harmless); 0 < snowGrip <= 1; slip values above 0.
inline bool validSnowIceTuning(const SnowIceTuning& t) {
    const float all[] = {t.trudgeStartM, t.trudgeFullM, t.trudgeMinScale, t.iceHoldM, t.snowGrip,
                         t.patchGrip,    t.iceGrip,     t.slipSpeedMS,    t.slipTurnDeg, t.slipS};
    for (const float v : all) {
        if (!std::isfinite(v)) return false;
    }
    return t.trudgeStartM >= 0.f && t.trudgeStartM < t.trudgeFullM && t.trudgeMinScale > 0.f && t.trudgeMinScale <= 1.f &&
           t.iceHoldM > 0.f && t.iceGrip > 0.f && t.iceGrip <= t.patchGrip && t.patchGrip <= 1.f && t.snowGrip > 0.f &&
           t.snowGrip <= 1.f && t.slipSpeedMS > 0.f && t.slipTurnDeg > 0.f && t.slipS > 0.f;
}

// Speed scale in snow this deep (m): 1 below trudgeStartM, falling linearly to trudgeMinScale at trudgeFullM, flat
// beyond. A non-finite or negative depth reads as no snow.
inline float trudgeScale(const SnowIceTuning& t, float depthM) {
    if (!std::isfinite(depthM) || depthM <= t.trudgeStartM) return 1.f;
    if (depthM >= t.trudgeFullM) return t.trudgeMinScale;
    const float k = (depthM - t.trudgeStartM) / (t.trudgeFullM - t.trudgeStartM);
    return 1.f + (t.trudgeMinScale - 1.f) * k;
}

// Frozen water this thick (m) holds a lead. Non-finite reads as open water.
inline bool iceHolds(const SnowIceTuning& t, float iceThicknessM) {
    return std::isfinite(iceThicknessM) && iceThicknessM >= t.iceHoldM;
}

// The footing under a step. onWater: the step is over a water body (iceThicknessM is its ice, 0 when none); icePatch:
// a patch decal or volume on dry ground. Trudge-deep snow wins over ice and patches.
inline Footing footingAt(const SnowIceTuning& t, float snowDepthM, bool onWater, float iceThicknessM, bool icePatch) {
    const bool deepSnow = std::isfinite(snowDepthM) && snowDepthM > t.trudgeStartM;
    if (onWater) {
        if (!iceHolds(t, iceThicknessM)) return Footing::OpenWater;
        return deepSnow ? Footing::Snow : Footing::FrozenWater;
    }
    if (deepSnow) return Footing::Snow;
    return icePatch ? Footing::IcePatch : Footing::Ground;
}

// Share of dry ground's grip on this footing. Open water has none (the lead swims).
inline float footingGrip(const SnowIceTuning& t, Footing f) {
    switch (f) {
    case Footing::Ground: return 1.f;
    case Footing::Snow: return t.snowGrip;
    case Footing::IcePatch: return t.patchGrip;
    case Footing::FrozenWater: return t.iceGrip;
    case Footing::OpenWater: return 0.f;
    }
    return 1.f;
}

// Whether this step slips: frozen water only, at or above slipSpeedMS, with a turn of at least slipTurnDeg (either
// way) or a stop. Never on an ice patch, in snow or on ground.
inline bool stepSlips(const SnowIceTuning& t, Footing f, float speedMS, float turnDeg, bool stopping) {
    if (f != Footing::FrozenWater || !std::isfinite(speedMS) || speedMS < t.slipSpeedMS) return false;
    return stopping || (std::isfinite(turnDeg) && std::fabs(turnDeg) >= t.slipTurnDeg);
}

} // namespace ftcore
