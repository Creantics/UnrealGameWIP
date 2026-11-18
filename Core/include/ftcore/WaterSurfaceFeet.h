#pragma once

// Where a partner's feet ride while it swims at the surface, and how it eases there. A single rule covers both
// cases below, so a partner put back in the water starts where its swim already holds it (no bob-up after a rejoin):
// - the surface swim each frame;
// - the placement at a rejoin.
// Each frame the feet ride with the surface's own move (waterFeetCarry) and ease only the gap off it, so a wave or the
// bob is not lagged (the ease alone trailed the moving target by 3.3 cm on the open sea, 11-18).
// The feet ride at the swim's draft under the surface plus the bob (waterSwimFeetZ), plus a splash's sink. They are
// never nearer the bed than kWaterBedFloat: in a column shallower than the draft, the feet stay just over the bed.
// Heights in metres (the host converts). No heap use.

#include <algorithm>
#include <cmath>

namespace ftcore {

inline constexpr float kWaterBedFloat = 0.08f; // m: a surface swimmer's feet stay at least this far over the bed

// The feet's height a surface swimmer holds: swimFeetZ (waterSwimFeetZ at its place and time) plus sink (the splash's
// depth offset, 0 when none), but never under bedZ + kWaterBedFloat.
inline float waterSurfaceFeetZ(float swimFeetZ, float sink, float bedZ)
{
    return std::max(swimFeetZ + sink, bedZ + kWaterBedFloat);
}

// One frame's ease of the feet toward want: a first-order lag with time constant ease (s). dt <= 0 or ease <= 0
// puts the feet at want.
inline float waterFeetEase(float z, float want, float dt, float ease)
{
    if (!(dt > 0.f) || !(ease > 0.f)) {
        return want;
    }
    return z + (want - z) * (1.f - std::exp(-dt / ease));
}

// Whether a surface swimmer's target is on the ride (the swim's draft and bob over the waves, plus the sink), not held
// up by the bed floor.
inline bool waterFeetOnRide(float swimFeetZ, float sink, float bedZ)
{
    return swimFeetZ + sink >= bedZ + kWaterBedFloat;
}

// One frame of a surface swimmer's feet: carried first by the ride's own move since the last frame (rideMove, m: the
// change of waterSwimFeetZ under it, the waves and the bob), then eased toward want the rest of the way. The ride's
// motion is then not lagged; only a gap off it eases (a splash's sink, the bed floor, a placement off the ride). The
// host passes rideMove 0 when the feet were not on the ride last frame or are not now (waterFeetOnRide).
inline float waterFeetCarry(float z, float rideMove, float want, float dt, float ease)
{
    return waterFeetEase(z + rideMove, want, dt, ease);
}

} // namespace ftcore
