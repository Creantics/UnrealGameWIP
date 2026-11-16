#pragma once
// Fills the underwater HUD's input (UiSwimIn) from the lead's real Swim step, so DIVE, RISE, the ring and the hints read
// the swim rule itself and cannot disagree with it.
//
//   each frame in the water:  const SwimStep& st = swim.step(input, env, dt);
//                             UiSwimIn in = uiSwimInFrom(swim, st, teach);
//   out of it:                UiSwimIn in = uiSwimInDry(teach);
//
// Header-only, plain values, zero allocation, no state of its own: the presses' edges are Swim's.
#include "ftcore/Swim.h"
#include "ftcore/UiSwim.h"

namespace ftcore {

// Out of the water: nothing hidden, no prompt.
inline UiSwimIn uiSwimInDry(ShowBasicControls teach) {
    UiSwimIn in;
    in.teach = teach;
    return in;
}

// After swim.step(...): `step` is its result.
inline UiSwimIn uiSwimInFrom(const Swim& swim, const SwimStep& step, ShowBasicControls teach) {
    UiSwimIn in;
    in.swimming = true;
    in.teach = teach;
    in.rescued = step.state == SwimState::Rescued;
    // Swim's own dive test; on the step a press dived the lead is already Diving, where a press would not dive again.
    in.canDive = step.canDive && step.state == SwimState::Surface;
    in.canRise = step.state == SwimState::Diving || step.state == SwimState::Under;
    in.dived = step.dived;
    in.rose = step.rose;
    in.warning = step.warning;
    in.draining = step.draining;
    in.attack = step.flags.attack;
    in.guard = step.flags.guard;
    in.dodge = step.flags.dodge;
    in.weaponsOut = step.flags.weaponsOut;
    in.breath = step.breath;
    in.warningSeconds = swim.tuning().warningSeconds;
    in.vitality = step.vitality;
    return in;
}

} // namespace ftcore
