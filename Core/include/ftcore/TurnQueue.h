#pragma once

#include "ftcore/Api.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ftcore {

using CombatantId = std::uint32_t;

// Conditional-turn-based initiative: each combatant fills a gauge at a rate equal to
// its speed; whoever reaches the threshold first acts. Faster units act more often.
// Fully deterministic; ties go to the higher speed, then the lower id.
class TurnQueue {
public:
    static constexpr double kThreshold = 100.0;

    FTCORE_API void add(CombatantId id, double speed, double initialGauge = 0.0);
    FTCORE_API void remove(CombatantId id);
    FTCORE_API void setSpeed(CombatantId id, double speed);
    FTCORE_API bool contains(CombatantId id) const;
    bool empty() const { return entries_.empty(); }
    void clear() { entries_.clear(); }
    // Room for `count` combatants, so add() never allocates for up to that many (a downed member who
    // is revived mid-battle rejoins with add()).
    void reserve(std::size_t count) { entries_.reserve(count); }

    // Advances time to the next actor, resets their gauge and returns them.
    FTCORE_API CombatantId next();
    // Upcoming actors without mutating state (for a turn-order timeline UI).
    FTCORE_API std::vector<CombatantId> preview(std::size_t count) const;
    // Same, into a caller buffer of `out.size()` ids; returns how many were written (0 when empty).
    // Allocation-free for up to kPreviewStack combatants.
    FTCORE_API std::size_t preview(std::span<CombatantId> out) const;
    static constexpr std::size_t kPreviewStack = 32;

private:
    struct Entry {
        CombatantId id;
        double speed;
        double gauge;
    };
    static std::size_t pickNext(const Entry* entries, std::size_t n, double& outTime);
    static CombatantId advance(Entry* entries, std::size_t n);

    std::vector<Entry> entries_;
};

} // namespace ftcore
