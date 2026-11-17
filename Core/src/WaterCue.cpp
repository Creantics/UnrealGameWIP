#include "ftcore/WaterCue.h"

#include "ftcore/WaterBodies.h"

#include <algorithm>

namespace ftcore {

float waterCueStir(WaterRipples& rings, const WaterCue& cue, bool bStirred) {
    const float s = std::clamp(cue.strength, 0.f, 1.f);
    const Vec2 p{cue.at[0], cue.at[1]};
    switch (cue.kind) {
    case WaterCueKind::Entry:
        if (bStirred) return 0.f;
        rings.add(p, s, kWaterSplashLife, kWaterSplashSpeed, 0.15f);
        rings.add(p, 0.6f * s, kWaterSplashLife, 0.55f * kWaterSplashSpeed, 0.05f);
        return s;
    case WaterCueKind::CatchLeft:
    case WaterCueKind::CatchRight:
        rings.add(p, 0.1f + 0.3f * s, kWaterWakeLife, 0.5f, 0.1f);
        return 0.f;
    case WaterCueKind::Kick:
        rings.add(p, 0.1f + 0.35f * s, kWaterWakeLife, 0.6f, 0.15f);
        return 0.f;
    case WaterCueKind::Gasp:
    case WaterCueKind::HeadOut:
        rings.add(p, 0.25f, kWaterIdleLife, 0.45f, 0.12f);
        return 0.f;
    case WaterCueKind::HeadUnder:
        rings.add(p, 0.35f, kWaterIdleLife, 0.5f, 0.2f);
        return 0.f;
    case WaterCueKind::Pour:
        if (s > 0.f) rings.add(p, 0.2f * s, kWaterIdleLife, 0.35f, 0.25f);
        return 0.f;
    case WaterCueKind::Exit:
        if (bStirred) return 0.f;
        rings.add(p, 0.1f + 0.4f * s, kWaterSplashLife, 0.6f * kWaterSplashSpeed, 0.15f);
        return s / 3.f;
    case WaterCueKind::HandRest:
        return 0.f;
    }
    return 0.f;
}

} // namespace ftcore
