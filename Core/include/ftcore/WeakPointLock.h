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
// * onDeferred(t): a deferred refusal (it counts as accepted). Counted; it relocks an open lock when
//   relockOnDefer.
// * While open() the host keeps the boss's CritWindow (ArenaBoss.h) open until until(); locked, closed.
//
// Times as CombatInputs.h (queries included). No allocation.

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
    WeakPointLock() { reset({}, 0.0); }

    // Starts over with its clock at t (CombatInputs.h startTime).
    bool reset(const LockTuning& tuning, double t) {
        const bool ok = validLockTuning(tuning);
        tuning_ = ok ? tuning : LockTuning{};
        now_ = startTime(t);
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

    // The per-frame tick for a host that drives this lock directly: moves the clock to t (CombatInputs.h
    // rules) and closes a window that has run out. LockedPoint's owners need not call it.
    void update(double t) { advance(t); }

    // Open at t (t read as CombatInputs.h against the latest time seen, without moving it).
    bool open(double t) const { return open_ && inputTime(now_, t) < until_; }

    // The end of the open time (+inf while open with openSeconds 0; meaningless while locked).
    double until() const { return until_; }
    int opens() const { return opens_; }
    int deferrals() const { return deferrals_; }
    const LockTuning& tuning() const { return tuning_; }

private:
    friend class LockedPoint;
    // For LockedPoint, whose owner keeps its own clock: moves the clock to t with no jump bound (an
    // earlier or non-finite t does nothing), so the lock never falls behind its owner.
    void follow(double t) {
        if (std::isfinite(t) && t > now_) advance(t, std::numeric_limits<double>::infinity());
    }
    // Open at the owner's time t, with no jump bound.
    bool openAt(double t) const { return open_ && (std::isfinite(t) && t > now_ ? t : now_) < until_; }

    static constexpr double kInf = std::numeric_limits<double>::infinity();
    static constexpr int kIntMax = std::numeric_limits<int>::max(); // counters saturate
    double advance(double t, double maxJump = kMaxTimeJump) {
        now_ = inputTime(now_, t, maxJump);
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

// The one adapter every boss machine uses over a WeakPointLock (ArenaBossRun, BossScript): a boss whose
// weak point is not locked is always open, and offers to it are BadKey. Every call takes the owner's
// clock: the lock follows it with no jump bound, so a long quiet stretch before the first offer cannot
// leave the lock behind. BossScript bounds its clock by kMaxTimeJump; ArenaBossRun only refuses earlier
// and non-finite times. Ask the owner whether the point is open (weakPointOpen, unlocked), not
// lock().open, which reads against the lock's own clock and lags between offers.
class LockedPoint {
public:
    // Starts shut at t (or, not locked, always open).
    void reset(bool locked, const LockTuning& tuning, double t) {
        locked_ = locked;
        lock_.reset(locked ? tuning : LockTuning{}, t);
    }
    bool locked() const { return locked_; }
    bool open(double t) const { return !locked_ || lock_.openAt(t); }
    UnlockResult offer(UnlockKey key, double t, std::uint16_t clause = 0) {
        if (!locked_) return UnlockResult::BadKey;
        lock_.follow(t);
        return lock_.offer(key, t, clause);
    }
    static bool opened(UnlockResult u) { return u == UnlockResult::Unlocked || u == UnlockResult::Refreshed; }
    // True when the deferral shut an open lock.
    bool onDeferred(double t) {
        if (!locked_) return false;
        lock_.follow(t);
        return lock_.onDeferred(t);
    }
    // True when it shut an open lock.
    bool relock(double t) {
        if (!locked_) return false;
        lock_.follow(t);
        if (!lock_.open(t)) return false;
        lock_.relock(t);
        return true;
    }
    double until() const { return lock_.until(); }
    const WeakPointLock& lock() const { return lock_; }

private:
    bool locked_ = false;
    WeakPointLock lock_{};
};

} // namespace ftcore
