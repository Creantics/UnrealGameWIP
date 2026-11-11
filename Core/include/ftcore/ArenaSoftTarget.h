#pragma once

// Soft target and homing for an arena attack that starts with no lock: pure queries, no fight state.
//
// * Soft target (arenaSoftTarget): of the living bodies whose near edge (centre distance less its radius) is
//   within `range` and whose direction from the attacker is within the arc (dot(dir, to body) >= arcCos), the
//   one with the best score 0.7 * alignment + 0.3 * nearness (1 - distance / range); scores within a tie go to
//   the nearer body, then the lower index. `dir` is the stick when it is pushed, else the facing
//   (arenaSoftDir). range 0 turns it off.
// * Homing (arenaHoming): the facing an attack on that body starts with (straight at it) and how far the
//   attacker closes in on top of the attack's own lunge so it lands: what is missing from the attack's reach
//   (range + the body's radius) after the lunge, capped at `homing` and never past the two bodies touching.
//   0 when the attack already reaches. arenaHomingStep spreads it over startup and active like the lunge, so
//   the distance covered is the same at any frame rate.
// * Precedence (arenaAimSource), for the host that wires it into the fight: a riposte's own enemy, then the
//   aim, then the lock, then the soft target, then the pushed stick, then the facing it has. The soft target is
//   never asked for while aiming or locked.
//
// Each lead's numbers are its kit's softRange, softArcCos and homingReach (arena.json, a lead inheriting the
// default's). Nothing here allocates.

#include "ftcore/Api.h"
#include "ftcore/ArenaFight.h"

#include <cstddef>
#include <cstdint>

namespace ftcore {

struct ArenaSoftAim {
    float range = 0.f;   // metres to a body's near edge; 0: no soft target
    float arcCos = 0.5f; // the arc either side of the direction (dot)
    float homing = 0.f;  // most metres an attack closes in on top of its lunge; 0: no homing
};

struct ArenaSoftBody {
    Vec2 pos{};
    float radius = 0.f;
    bool alive = false;
};

struct ArenaHoming {
    Vec2 facing{1.f, 0.f}; // the facing the attack starts with
    float close = 0.f;     // metres closed in on top of the lunge, over startup and active
    bool reaches = false;  // with that, the attack's reach covers the body
};

enum class ArenaAimSource : std::uint8_t { Riposte, Aim, Lock, Soft, Stick, Facing };

FTCORE_API ArenaSoftAim arenaSoftAimOf(const ArenaFighter& kit);

// The stick when it is pushed (a length above the rest dead zone), else the facing; unit.
FTCORE_API Vec2 arenaSoftDir(Vec2 stick, Vec2 facing);

// The soft target's index in `bodies`, or kArenaNobody.
FTCORE_API std::uint8_t arenaSoftTarget(Vec2 from, Vec2 dir, const ArenaSoftBody* bodies, std::size_t count,
                                        const ArenaSoftAim& aim);

// An attack with `range` and `lunge` from a body of `fromRadius` at `from`, on `target`.
FTCORE_API ArenaHoming arenaHoming(Vec2 from, float fromRadius, Vec2 facing, const ArenaSoftBody& target, float range,
                                   float lunge, const ArenaSoftAim& aim);

// The share of `close` covered between attack times a0 and a1 (s) of an attack whose startup and active last `span`.
FTCORE_API float arenaHomingStep(float close, float span, float a0, float a1);

FTCORE_API ArenaAimSource arenaAimSource(bool riposte, bool aiming, bool locked, bool softFound, Vec2 stick);

} // namespace ftcore
