#pragma once

// The arena's speed curves: one shape and one evaluator for every body travel Core plays against a clock (ruling
// 6681r item 5): a lunge move's window (arena.json move "lungeSpeed"), a kind's flinch push ("flinchSpeed") and a
// special's knock-back ("knockSpeed"). Plain data, no allocation.

#include "ftcore/Api.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ftcore {

// A speed curve: the body's speed against its own clock, linear between knots (each segment constant acceleration,
// its distance the exact trapezoid), 0 before the first knot and after the last. A knot may repeat the one before's
// time (a step). The whole curve is scaled by its distance / its integral (`scale`), so the travel stays one knob.
//
// A move's lunge window (arena.json move "lungeSpeed": [[t, v], ...], at most one move per kind): its clock is
// seconds since the move began, less an unseen start's kArenaUnseenWindup, which the clip holds on its first frame.
// The window may run past startup + active (a landing skid in the recovery). With no knots a move keeps the linear
// spread (lunge over startup + active), as does a boss's break move (its clock is not the clip's).
//   - lungeToContact kind: only the lead stops it, and only in the air (the startup's end until the ending is
//     decided, ArenaEnemyOut::lungeEnding); the startup's frames and the landing always carry their travel.
inline constexpr std::size_t kMaxArenaLungeKnots = 8;
struct ArenaLungeCurve {
    std::array<float, kMaxArenaLungeKnots> t{}; // s on the curve's clock, not decreasing
    std::array<float, kMaxArenaLungeKnots> v{}; // m/s, at least 0 (before the scale)
    float scale = 1.f;                         // distance / the knots' integral (arenaLungeScale; the loader sets it)
    std::uint8_t count = 0;                    // knots; 0: none
};
// The curve's metres from its start to `t` on its clock, scaled; 0 at or before the first knot, with no knots, or for
// NaN; the whole (scaled) integral from the last knot on.
FTCORE_API float arenaLungeDistance(const ArenaLungeCurve& c, float t);
// distance / the unscaled integral (1 when that is 0).
FTCORE_API float arenaLungeScale(const ArenaLungeCurve& c, float lunge);
// The curve's scaled speed at `t` on its clock (the knots' line; at a step, the later knot's): 0 before the first
// knot, after the last, with no knots, or for NaN.
// A body it moves takes its travel between two clocks over each step (arenaLungeDistance's difference), so it covers
// the curve's distance exactly.
FTCORE_API float arenaLungeSpeed(const ArenaLungeCurve& c, float t);

} // namespace ftcore
