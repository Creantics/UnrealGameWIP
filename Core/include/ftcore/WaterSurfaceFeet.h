#pragma once

// Where a partner's feet ride while it swims at the surface, and how it eases there. A single rule covers both
// cases below, so a partner put back in the water starts where its swim already holds it (no bob-up after a rejoin):
// - the surface swim each frame;
// - the placement at a rejoin.
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

} // namespace ftcore
