#pragma once

// Shared pieces of the arena's shared combat inputs (docs/SHARED_COMBAT_INPUTS.md): fighter ids and
// the clock rule. Header-only, no allocation.
//
// Fighters are small indices the host maps its pawns to: two leads, the third party slot, and the
// enemy side. Core never knows who they are.
//
// Time is the fight clock in seconds. Each input object keeps the latest time it has seen; a time that
// is earlier, or not finite, reads as that latest time (a bad clock stalls a piece, never hangs it).

#include <cmath>
#include <cstdint>

namespace ftcore {

using FighterId = std::uint8_t;
inline constexpr std::uint8_t kMaxFighters = 4;
inline constexpr FighterId kLeadA = 0;
inline constexpr FighterId kLeadB = 1;
inline constexpr FighterId kThirdSlot = 2;
inline constexpr FighterId kEnemyFighter = 3;
inline constexpr FighterId kNoFighter = 0xFF;

inline constexpr bool isLead(FighterId f) { return f == kLeadA || f == kLeadB; }
inline constexpr bool isFighter(FighterId f) { return f < kMaxFighters; }
inline constexpr std::uint8_t fighterBit(FighterId f) { return isFighter(f) ? static_cast<std::uint8_t>(1u << f) : 0u; }

// `t` when it is finite and not before `now`, else `now`.
inline double inputTime(double now, double t) { return std::isfinite(t) && t > now ? t : now; }

} // namespace ftcore
