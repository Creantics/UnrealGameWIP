#pragma once

// The weather's rain: the one source of rain wetness for the whole game (world-build-3 W4 step 3, section 1).
//
// A RainState is stepped once a frame by its owner (the world clock, before it writes the ground's Wetness):
// - intensity follows its target linearly, rising over rampUpS and falling over rampDownS;
// - wetness moves toward the soak this intensity holds (intensity / soakAtIntensity, at most 1), rising at wetRisePerS
//   and drying at dryPerS. Asphalt dries slower than it wets, so a shower leaves the street wet for minutes;
// - puddles move toward the share of wetness over puddleFrom, filling at puddleRisePerS and drying at puddleDryPerS. On the
//   way up they lag the wetness; on the way down they outlast it (water stands after the ground has dried).
//
// Readers:
// - weatherGroundWetness: the ground's Wetness (MPC_Look), the night look's damp floor or the rain, whichever is wetter.
//   With no rain it is today's look.wetness * wetShare exactly.
// - leadSurfaceWetness: skin, hair and cloth on a lead. It is the rain's wetness (scaled by how exposed the lead stands) or
//   the lead's own water state (WaterWetness::wetness, from wading and swimming), whichever is wetter. The night look's
//   damp floor is the ground's art direction and never wets a lead.
// - rainBand: the streak density tier 0..3 (0 exactly when no rain falls).
//
// Plain C++20, no allocation, no state outside RainState.

#include "ftcore/Api.h"

namespace ftcore {

struct RainSpec {
    float rampUpS = 8.f;           // s for the intensity to rise from 0 to 1
    float rampDownS = 20.f;        // s to fall from 1 to 0
    float soakAtIntensity = 0.5f;  // rain this heavy or heavier soaks the ground fully; lighter holds a share of it
    float wetRisePerS = 1.f / 90.f;
    float dryPerS = 1.f / 600.f;
    float puddleFrom = 0.35f;      // wetness under which no puddle stands
    float puddleRisePerS = 1.f / 180.f;
    float puddleDryPerS = 1.f / 1200.f;
    float mistAtFull = 1.6f;       // local mist density scale at full rain (1 when dry)
    float streakAlphaMin = 0.15f;  // the lightest rain's streak opacity (the heaviest's is 1)
};

struct RainState {
    float target = 0.f;     // 0..1, where the intensity is heading (the switch: -Rain, ft.Rain)
    float intensity = 0.f;  // 0..1 now
    float wetness = 0.f;    // 0..1, the soak the rain has left on open ground
    float puddle = 0.f;     // 0..1, the standing water
};

// The longest step rainStep takes (s): a longer dt (a hitch, a load) is clamped to it.
inline constexpr float kRainMaxStepS = 0.5f;

// One step of dtS seconds. A dt of 0 or less, or not finite, returns s as it was; target and all values are kept in 0..1.
FTCORE_API RainState rainStep(const RainSpec& spec, RainState s, float dtS);
// The state the rain at `target` holds once it has settled (intensity at target, wetness and puddles at their soak), for
// -Rain=<x> starts and stills that must not wait for the ramp.
FTCORE_API RainState rainSettled(const RainSpec& spec, float target);
// The wetness the rain holds on open ground at this intensity: min(1, intensity / soakAtIntensity).
FTCORE_API float rainSoakTarget(const RainSpec& spec, float intensity);
// The ground's Wetness: max(lookWet * wetShare, rain wetness), each clamped to 0..1.
FTCORE_API float weatherGroundWetness(float lookWet, float wetShare, const RainState& rain);
// Surface wetness on a lead now: max(rain wetness * exposure, ownWetness). exposure 0..1: 1 out in the open, 0 under cover
// or indoors; pass 1 until the shelter map exists. ownWetness is the lead's WaterWetness::wetness (0..1).
FTCORE_API float leadSurfaceWetness(const RainState& rain, float ownWetness, float exposure = 1.f);
// The streak density tier: 0 at intensity 0, then 1 below 1/3, 2 below 2/3, 3 from there.
FTCORE_API int rainBand(float intensity);
// Streak opacity: 0 at intensity 0, else streakAlphaMin..1 along the intensity.
FTCORE_API float rainStreakAlpha(const RainSpec& spec, float intensity);
// Local mist density scale: 1 + (mistAtFull - 1) * intensity.
FTCORE_API float rainMistScale(const RainSpec& spec, float intensity);

}  // namespace ftcore
