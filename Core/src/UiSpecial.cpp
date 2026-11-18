#include "ftcore/UiSpecial.h"

#include <cmath>

namespace ftcore {

UiSpecialView uiSpecialStep(UiSpecialState& s, bool reserve, const UiSpecialInput& in, float dt, bool reduceMotion, bool reduceFlashes) {
    UiSpecialView v;
    v.row = reserve;
    if (!reserve) {
        s = UiSpecialState{};
        return v;
    }
    const float step = std::isfinite(dt) && dt > 0.f ? dt : 0.f;
    if (!in.ready && !in.thirdReady) {
        // Nothing ready (used, or the rules' unready): the row leaves as last shown, with no pulse; the next ready
        // arrives with the pulse again. Reduce motion: a cut.
        if ((!s.ready && !s.leaving) || reduceMotion) {
            s = UiSpecialState{};
            return v;
        }
        s.since = s.leaving ? s.since + step : 0.f;
        s.leaving = true;
        s.ready = false;
        s.pulsed = false;
        const float a = s.alphaWas * (1.f - s.since / kUiSpecialFadeOut);
        if (a <= 0.f) {
            s = UiSpecialState{};
            return v;
        }
        v.drawn = true;
        v.live = s.live;
        v.call = s.call;
        v.why = s.why;
        v.alpha = a;
        return v;
    }
    if (!s.ready) {
        // An arrival: from nothing at 0, or back mid-leave climbing from the alpha it had (no dip to 0).
        const float had = s.leaving ? s.alphaWas * (1.f - s.since / kUiSpecialFadeOut) : 0.f; // the leave's last alpha
        s.since = (had > 0.f ? had : 0.f) * kUiSpecialFadeIn;
        s.leaving = false;
        s.pulsed = false;
    } else {
        s.since += step;
    }
    s.ready = true;
    v.drawn = true;
    v.call = !in.ready; // only the Third's: CALL and his name
    const UiSpecialWhy why = in.ready ? in.why : UiSpecialWhy::None;
    v.live = why == UiSpecialWhy::None;
    v.why = why;
    // The pulse is the first live moment's (a special that arrives with no target pulses when it turns live), once.
    if (v.live && !s.pulsed) {
        s.pulsed = true;
        s.pulseAt = s.since;
    }
    s.live = v.live;
    s.call = v.call;
    s.why = v.why;
    if (reduceMotion) {
        v.alpha = 1.f;
        s.alphaWas = 1.f;
        return v;
    }
    v.alpha = s.since >= kUiSpecialFadeIn ? 1.f : s.since / kUiSpecialFadeIn;
    s.alphaWas = v.alpha;
    const float p = s.since - s.pulseAt;
    if (v.live && s.pulsed && !reduceFlashes && p < kUiSpecialPulse) v.pulse = p / kUiSpecialPulse;
    return v;
}

} // namespace ftcore
