#include "ftcore/LedgeHang.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

bool LedgeHang::canCatch(const LedgeState& s) const {
    return !s.holding() && s.regrab <= 0.f;
}

bool LedgeHang::stickInto(const LedgeInput& in) const {
    return in.stick >= tuning_.stickShare && std::fabs(in.stickOff) <= tuning_.intoFace;
}

bool LedgeHang::stickAway(const LedgeInput& in) const {
    return in.stick >= tuning_.stickShare && std::fabs(in.stickOff) >= 180.f - tuning_.intoFace;
}

LedgePhase LedgeHang::reach(LedgeState& s, const LedgeInput& in) const {
    s.through = tuning_.hang < 0.5f || (in.jumpHeld && stickInto(in));
    s.phase = s.through ? LedgePhase::Climbing : LedgePhase::Hanging;
    s.time = 0.f;
    return s.phase;
}

void LedgeHang::step(LedgeState& s, const LedgeInput& in, float dt) const {
    dt = std::max(dt, 0.f);
    s.regrab = std::max(s.regrab - dt, 0.f);
    switch (s.phase) {
    case LedgePhase::Hanging:
        s.time += dt;
        if (s.time < tuning_.minHang) break;
        // A drop asked wins over a climb asked in the same frame: letting go is the safe way out.
        if (in.dropPressed || stickAway(in)) {
            s.phase = LedgePhase::Dropping;
            s.time = 0.f;
            s.regrab = tuning_.regrab;
        } else if (in.jumpPressed || stickInto(in)) {
            s.phase = LedgePhase::Climbing;
            s.time = 0.f;
        }
        break;
    case LedgePhase::Climbing:
        s.time += dt;
        break;
    case LedgePhase::Dropping:
        // Dropping lasts the one frame it is asked in, so the host sees it once and lets go; the regrab runs
        // from the drop.
        s.phase = LedgePhase::None;
        s.time = 0.f;
        break;
    case LedgePhase::None:
        break;
    }
}

void LedgeHang::finish(LedgeState& s) const {
    if (s.phase == LedgePhase::Climbing || s.phase == LedgePhase::Hanging) {
        s.phase = LedgePhase::None;
        s.time = 0.f;
    }
}

} // namespace ftcore
