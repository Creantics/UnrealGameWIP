#pragma once
// Fills the underwater HUD's input (UiSwimIn) from the lead's real Swim step, so DIVE, RISE, the ring and the hints read
// the swim rule itself and cannot disagree with it.
//
//   UiSwimFeed feed;   // one per lead, beside its Swim
//   each frame in the water:  const SwimStep& st = swim.step(input, env, dt);
//                             UiSwimIn in = uiSwimInFrom(feed, swim, st, input, env, teach);
//   out of it:                UiSwimIn in = uiSwimInDry(feed, input, teach);
//
// Header-only, plain values, zero allocation.
#include "ftcore/Swim.h"
#include "ftcore/UiSwim.h"

#include <cmath>

namespace ftcore {

struct UiSwimFeed {
    bool jumpHeld = false; // the jump last frame, for its press (the same edge rule as Swim's)
};

// The column a dive is judged on, guarded as Swim::step guards it.
inline float uiSwimFeedColumn(const SwimEnv& env) {
    const float span = env.surfaceZ - env.bedZ;
    const float column = env.column;
    if (!std::isfinite(column) || column < 0.f || std::fabs(column - span) > kSwimColumnSlack) return span;
    return column;
}

// Out of the water: nothing hidden, no prompt. Keeps the jump's edge so the first frame in the water reads it right.
inline UiSwimIn uiSwimInDry(UiSwimFeed& feed, const SwimInput& input, ShowBasicControls teach) {
    feed.jumpHeld = input.jump;
    UiSwimIn in;
    in.teach = teach;
    return in;
}

// After swim.step(input, env, dt): `step` is its result, `input` and `env` what it was given.
inline UiSwimIn uiSwimInFrom(UiSwimFeed& feed, const Swim& swim, const SwimStep& step, const SwimInput& input,
                             const SwimEnv& env, ShowBasicControls teach) {
    const bool jumpPress = input.jump && !feed.jumpHeld;
    feed.jumpHeld = input.jump;
    const bool fromUnder = step.from == SwimState::Diving || step.from == SwimState::Under;

    UiSwimIn in;
    in.swimming = true;
    in.teach = teach;
    in.rescued = step.state == SwimState::Rescued;
    // TODO(core swim canDive): read step.canDive once Core's tip adds it; until then the same test as Swim::step's dive.
    in.canDive = step.state == SwimState::Surface && env.deepAllowed && uiSwimFeedColumn(env) >= kSwimDeepMin;
    in.canRise = step.state == SwimState::Diving || step.state == SwimState::Under;
    in.dived = step.dived;
    // A rise: Diving or Under to Surfacing, or straight to the surface on the press's own step (only a press does that
    // from Diving; from Under the lead may also swim up through it, which is no rise).
    in.rose = fromUnder && (step.state == SwimState::Surfacing ||
                            (step.state == SwimState::Surface && (step.from == SwimState::Diving || jumpPress)));
    in.warning = step.warning;
    in.draining = step.draining;
    in.attack = step.flags.attack;
    in.dodge = step.flags.dodge;
    in.weaponsOut = step.flags.weaponsOut;
    // TODO(core swim guard): read step.flags.guard once Core's tip adds it; no guard in the water until then.
    in.guard = false;
    in.breath = step.breath;
    in.warningSeconds = swim.tuning().warningSeconds;
    in.vitality = step.vitality;
    return in;
}

} // namespace ftcore
