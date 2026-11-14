#pragma once

// A hallucination in an arena fight: what of the lead's input reaches the fight while an offer is open.
//
// * Held: nothing. The refusal button (the guard) went to the refusal (HallucinationInput::refuse) and never reaches
//   the fight as a guard; the stick, the buttons, the lock, the aim, the Read and the take-half are all cleared, and
//   an attack under way roots. Only the camera's forward stays, which picks nothing with no lock asked for.
// * A hold over (refused, pulled back or torn) with the guard still down from it: that press stays the refusal's until
//   let go (Hallucination::refuseKept), so the fight sees neither a guard nor the fresh press that would open a parry.
// * Holding: the stick is slowed by the hold (x (1 - holdLevel)); everything else goes through.
// * No offer open, or only shown: the input goes through as it is.
//
// The host builds HallucinationInput from the raw input first, ticks the Hallucination, then sends the fight this.
//
// The price (Hallucination.h): after the tick, a Refused with weakPointOpenFor() above 0 opens the boss's weak point
// for that long; while weakPointShutFor() is above 0 it stays shut; hallucinationHeldDamage() is the lead's hp to take.

#include "ftcore/ArenaFight.h"
#include "ftcore/Hallucination.h"

#include <cmath>

namespace ftcore {

inline ArenaInput hallucinationArenaInput(const Hallucination& h, const ArenaInput& in) {
    if (h.held()) {
        ArenaInput out;
        out.view = in.view;
        return out;
    }
    ArenaInput out = in;
    if (h.refuseKept()) out.guard = false;
    const float keep = 1.f - h.holdLevel();
    if (h.active() && keep < 1.f) out.move = Vec2{in.move.x * keep, in.move.y * keep};
    return out;
}

// Held's health cost in hit points for a lead of `maxHp`, on the step Held began (0 on every other step), to the
// nearest point.
inline int hallucinationHeldDamage(const Hallucination& h, int maxHp) {
    if (!(h.heldCost() > 0.f) || maxHp <= 0) return 0;
    return static_cast<int>(std::lround(static_cast<double>(maxHp) * static_cast<double>(h.heldCost())));
}

} // namespace ftcore
