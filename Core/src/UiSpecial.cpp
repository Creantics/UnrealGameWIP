#include "ftcore/UiSpecial.h"

#include <cmath>

namespace ftcore {

UiSpecialView uiSpecialStep(UiSpecialState& s, bool reserve, const UiSpecialInput& in, float dt, bool reduceMotion) {
    UiSpecialView v;
    v.row = reserve;
    if (!reserve || !in.ready) {
        s = UiSpecialState{};
        return v;
    }
    const float step = std::isfinite(dt) && dt > 0.f ? dt : 0.f;
    s.since = s.ready ? s.since + step : 0.f;
    s.ready = true;
    v.drawn = true;
    v.live = in.why == UiSpecialWhy::None;
    v.why = v.live ? UiSpecialWhy::None : in.why;
    // The pulse is the first live moment's (a special that arrives with no target pulses when it turns live), once.
    if (v.live && !s.pulsed) {
        s.pulsed = true;
        s.pulseAt = s.since;
    }
    if (reduceMotion) {
        v.alpha = 1.f;
        return v;
    }
    v.alpha = s.since >= kUiSpecialFadeIn ? 1.f : s.since / kUiSpecialFadeIn;
    const float p = s.since - s.pulseAt;
    if (v.live && s.pulsed && p < kUiSpecialPulse) v.pulse = p / kUiSpecialPulse;
    return v;
}

} // namespace ftcore
