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

#include "ftcore/ArenaFight.h"
#include "ftcore/Hallucination.h"

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

} // namespace ftcore
