#pragma once

// A boss weak point that is locked until one of its keys unlocks it. See docs/SHARED_COMBAT_INPUTS.md
// section 6.
//
// * Keys (a mask, UnlockKey bits): SharedBlow (a blow in a joined count window: CountOutcome::OnCount
//   at once or by a BlowSettled event, or a hit a lead took half of), ReadClause (the lock's hidden
//   clause read: the id must match), Refusal (a HeldRefusal resolved Refused). A boss's data lists
//   the keys that open it.
// * offer(key, t, clause): unlocks when the key is in the mask (and the clause matches for ReadClause).
//   Offered while open, it restarts the open time. Open for openSeconds, or until relock() when 0.
// * onDeferred(t): a deferred refusal ("not yet" is a yes). Counted; it relocks an open lock when
//   relockOnDefer.
// * While open() the host keeps the boss's CritWindow (ArenaBoss.h) open until until(); locked, closed.
//
// Times as CombatInputs.h. No allocation.

#include "ftcore/CombatInputs.h"
#include "ftcore/StrikeCount.h"

#include <cstdint>
#include <limits>

namespace ftcore {

enum UnlockKey : std::uint8_t {
    kUnlockSharedBlow = 1,
    kUnlockReadClause = 2,
    kUnlockRefusal = 4,
};
inline constexpr std::uint8_t kUnlockAll = kUnlockSharedBlow | kUnlockReadClause | kUnlockRefusal;

enum class UnlockResult : std::uint8_t { Unlocked, Refreshed, WrongKey, WrongClause, BadKey }; // append only

struct LockTuning {
    std::uint8_t keys = kUnlockSharedBlow; // which keys open it: 1..kUnlockAll
    std::uint16_t clause = 0;              // ReadClause: the hidden clause's id (0: none set)
    double openSeconds = 3.0;              // 0: open until relock(); else 0.1..30
    bool relockOnDefer = true;
};

inline bool validLockTuning(const LockTuning& t) {
    if (t.keys == 0 || (t.keys & ~kUnlockAll) != 0) return false;
    if ((t.keys & kUnlockReadClause) && t.clause == 0) return false;
    return t.openSeconds == 0.0 || (t.openSeconds >= 0.1 && t.openSeconds <= 30.0);
}

// True when a judged blow counts as a shared blow.
inline bool sharedBlow(const CountHit& hit) { return sharedOutcome(hit.outcome); }
// True when a held blow settled as a shared blow (StrikeCount's BlowSettled event).
inline bool sharedBlow(const CountEvent& e) {
    return e.type == CountEventType::BlowSettled && sharedOutcome(e.outcome);
}

class WeakPointLock {
public:
    WeakPointLock() { reset(); }

    bool reset(const LockTuning& tuning = {}) {
        const bool ok = validLockTuning(tuning);
        tuning_ = ok ? tuning : LockTuning{};
        now_ = 0.0;
        open_ = false;
        until_ = 0.0;
        opens_ = 0;
        deferrals_ = 0;
        return ok;
    }

    UnlockResult offer(UnlockKey key, double t, std::uint16_t clause = 0) {
        t = advance(t);
        if (key != kUnlockSharedBlow && key != kUnlockReadClause && key != kUnlockRefusal) return UnlockResult::BadKey;
        if ((tuning_.keys & key) == 0) return UnlockResult::WrongKey;
        if (key == kUnlockReadClause && clause != tuning_.clause) return UnlockResult::WrongClause;
        const bool was = open(t);
        open_ = true;
        until_ = tuning_.openSeconds > 0.0 ? t + tuning_.openSeconds : kInf;
        if (was) return UnlockResult::Refreshed;
        if (opens_ < kIntMax) ++opens_;
        return UnlockResult::Unlocked;
    }

    // True when it relocked an open lock.
    bool onDeferred(double t) {
        t = advance(t);
        if (deferrals_ < kIntMax) ++deferrals_;
        if (!tuning_.relockOnDefer || !open(t)) return false;
        open_ = false;
        return true;
    }

    void relock(double t) {
        advance(t);
        open_ = false;
    }

    bool open(double t) const { return open_ && t < until_; }
    // The end of the open time (+inf while open with openSeconds 0; meaningless while locked).
    double until() const { return until_; }
    int opens() const { return opens_; }
    int deferrals() const { return deferrals_; }
    const LockTuning& tuning() const { return tuning_; }

private:
    static constexpr double kInf = std::numeric_limits<double>::infinity();
    static constexpr int kIntMax = std::numeric_limits<int>::max(); // counters saturate
    double advance(double t) {
        now_ = inputTime(now_, t);
        if (open_ && now_ >= until_) open_ = false;
        return now_;
    }

    LockTuning tuning_{};
    double now_ = 0.0;
    bool open_ = false;
    double until_ = 0.0;
    int opens_ = 0;
    int deferrals_ = 0;
};

} // namespace ftcore
