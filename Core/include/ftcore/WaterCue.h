#pragma once

// What a body in the water tells the water, so the surface answers it (UFTWaterSubsystem): the swimmer's movement
// layer (WaterLoco) fills these for the lead and every follower, an enemy or a dropped object's host for theirs.
//
//   WaterCueEvent an event this step: an entry (strength 0..1 from the entry speed), a hand's catch, a kick, a gasp, the
//                head going under or coming out. Where is the hand's, the feet's or the head's place.
//   WaterToucher a body that cuts the surface this step (sent every tick while it is in the water): its place, velocity,
//                its radius at the waterline and how much of it is under. The surface draws its displacement, the rings
//                while it treads and the wake while it moves from these.
//
// Metres, metres a second, the engine's frame (Z up). Plain data.

#include "ftcore/Api.h"

#include <cstdint>

namespace ftcore {

// HandRest: a hand set on a ledge while treading (a damp patch at `at`). Pour: water running off the body on a climb out
// (`at` the waist, strength how wet). Exit: leaving by steps or wading out (a small splash).
enum class WaterCueKind : std::uint8_t { Entry, CatchLeft, CatchRight, Kick, Gasp, HeadUnder, HeadOut, HandRest, Pour, Exit };

struct WaterCueEvent {
    WaterCueKind kind = WaterCueKind::Entry;
    float strength = 0.f; // 0..1
    float at[3] = {};     // m
    float velocity[3] = {}; // m/s: the hand's, the feet's, the body's at entry
};

struct WaterToucher {
    float at[3] = {};       // m: the body's middle at the waterline
    float velocity[3] = {}; // m/s
    float radius = 0.3f;    // m at the waterline
    float immersion = 0.f;  // 0..1 of its height under
    float massKg = 70.f;    // sizes its splash and wake
};

class WaterRipples; // ftcore/WaterBodies.h

// The rings a cue sends out on the surface (a catch and a kick small and quick, the head's going under or coming out a
// slow ring, an exit a splash's pair) and the splash effect it wants, 0..1 (an entry its strength, an exit a third of
// it; 0 for the rest). The lead's own entry and exit are WaterStir's: its host passes bStirred so they are not sent
// twice. A hand at rest sends nothing yet. No allocation.
FTCORE_API float waterCueStir(WaterRipples& rings, const WaterCueEvent& cue, bool bStirred);

} // namespace ftcore
