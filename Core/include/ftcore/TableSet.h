#pragma once

// Setting the table: at full Load, a short state in which everyone seated shares every blow, and a
// seated enemy shares what it takes. It needs at least minSeats places named or deliberately left open
// (Places.h), and it can fail. See docs/SHARED_COMBAT_INPUTS.md section 4.
//
//   Ready --begin--> Setting --(setTime)--> Set --(setDuration, or cancel)--> Cooling --> Ready
//                       +--interrupt--> Cooling (failed, failCooldown)
//
// * begin(setter, load, places, seated, t): needs places.full(load) and places.tableSeats() >=
//   minSeats. `seated` is the mask of fighters at the table (fighterBit; the enemy bit is ignored, the
//   setter is always seated). On success begin spends the setter's Load itself (load.release()).
// * interrupt(t) while Setting (the setter staggered or downed): the table fails. The Load stays spent.
// * checkSeats(places, t) while Setting or Set: with fewer than minSeats named or left open, the table
//   gutters out: Guttered, failed(), and failCooldown. An open chair is a seat; an emptied one is not.
//   TableSet keeps no reference to the PlaceTable, so the host must call checkSeats after every change
//   to the setter's places while the table is Setting or Set.
// * beginAt(..., spot, others, count, t): begin with a place on the ground (TableSpot). Refused with Occupied
//   (nothing spent, no cooldown) while another table in `others` is Set and the two rings overlap: the centres
//   closer than the two radii added. A table still Setting does not block. One table per place.
// * checkBodies(bodies, t) while Set: with fewer than minBodies at the table (the host counts them: TableRing.h),
//   the table gutters out as checkSeats does. The host calls it at Set and after every stand-up or death.
// * seatEnemy(t) while Set: the enemy sits until the table clears.
// * split(target, amount, t) while Set: an enemy's blow on a seated fighter (enemy blows only: never a lead's
//   blow, a Release, a hazard or a heal) is shared evenly by everyone at the table, the seated enemy included (its share, times enemyShare, is real damage to it: this is how
//   it is hurt). A blow on the enemy itself is not shared back: it takes it all. Outside Set, or on a
//   fighter not seated, the target takes it all.
// * update(now): the timed changes since the last update, and the ones the calls made, in order.
//   Past kMaxTableEvents waiting, later events are turned away and counted in the returned `dropped`.
//
// Times as CombatInputs.h. No allocation.

#include "ftcore/CombatInputs.h"
#include "ftcore/Places.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ftcore {

enum class TableState : std::uint8_t { Ready, Setting, Set, Cooling }; // append only
enum class TableError : std::uint8_t { None, NotFull, TooFewSeats, Busy, Cooling, BadSetter, Occupied, Lone }; // append only
enum class TableEventType : std::uint8_t { Setting, Set, Failed, EnemySeated, Cleared, Ready, Guttered }; // append only

struct TableTuning {
    int minSeats = 3;          // 1..kMaxPlaces
    double setTime = 0.8;      // seconds from begin to Set: 0..5
    double setDuration = 6.0;  // seconds the table stays set: 0.1..60
    double cooldown = 20.0;    // after it clears: 0..300
    double failCooldown = 8.0; // after a failure: 0..300
    float enemyShare = 1.0f;   // a seated enemy's share of a blow, times this: 0..4
    int minBodies = 1;         // bodies at a set table below which it gutters (checkBodies): 1..16. 1: never
};

// Where a table sits: its ring's centre on the ground and radius, fixed when it begins.
struct TableSpot {
    float x = 0.f;
    float y = 0.f;
    float radius = 0.f;
};

// Two rings overlap: the centres are closer than the radii added (touching is not overlapping).
inline bool tableSpotsOverlap(const TableSpot& a, const TableSpot& b) {
    const double dx = static_cast<double>(a.x) - b.x;
    const double dy = static_cast<double>(a.y) - b.y;
    const double r = static_cast<double>(a.radius) + b.radius;
    return dx * dx + dy * dy < r * r;
}

inline bool validTableTuning(const TableTuning& t) {
    return t.minSeats >= 1 && t.minSeats <= static_cast<int>(kMaxPlaces) && t.setTime >= 0.0 && t.setTime <= 5.0 &&
           t.setDuration >= 0.1 && t.setDuration <= 60.0 && t.cooldown >= 0.0 && t.cooldown <= 300.0 &&
           t.failCooldown >= 0.0 && t.failCooldown <= 300.0 && t.enemyShare >= 0.0f && t.enemyShare <= 4.0f &&
           t.minBodies >= 1 && t.minBodies <= 16;
}

struct TableEvent {
    TableEventType type = TableEventType::Setting;
    double at = 0.0;
};

inline constexpr std::size_t kMaxTableEvents = 16;

struct TableEvents {
    std::array<TableEvent, kMaxTableEvents> items{};
    std::size_t count = 0;
    std::size_t dropped = 0;

    const TableEvent* begin() const { return items.data(); }
    const TableEvent* end() const { return items.data() + count; }
    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
    const TableEvent& operator[](std::size_t i) const { return items[i]; }
    void clear() { count = 0; dropped = 0; }
    void push(TableEventType type, double at) {
        if (count < items.size()) items[count++] = TableEvent{type, at};
        else ++dropped;
    }
};

// Who takes what of one blow: per FighterId (kEnemyFighter is the seated enemy).
struct TableSplit {
    std::array<float, kMaxFighters> share{};
    bool shared = false;
};

class TableSet {
public:
    TableSet() { reset({}, 0.0); }

    // Starts over with its clock at t (CombatInputs.h startTime).
    bool reset(const TableTuning& tuning, double t) {
        const bool ok = validTableTuning(tuning);
        tuning_ = ok ? tuning : TableTuning{};
        state_ = TableState::Ready;
        now_ = startTime(t);
        until_ = 0.0;
        setter_ = kNoFighter;
        seated_ = 0;
        enemySeated_ = false;
        failed_ = false;
        spot_ = TableSpot{};
        pending_.clear();
        out_.clear();
        return ok;
    }

    // Begins setting the table and spends `load` (the setter's) on success; on an error `load` is untouched.
    TableError begin(FighterId setter, Load& load, const PlaceTable& places, std::uint8_t seated, double t) {
        t = advance(t);
        if (!(setter == kLeadA || setter == kLeadB || setter == kThirdSlot)) return TableError::BadSetter;
        if (state_ == TableState::Setting || state_ == TableState::Set) return TableError::Busy;
        if (state_ == TableState::Cooling) return TableError::Cooling;
        if (!places.full(load)) return TableError::NotFull;
        if (places.tableSeats() < tuning_.minSeats) return TableError::TooFewSeats;
        load.release();
        setter_ = setter;
        seated_ = static_cast<std::uint8_t>((seated | fighterBit(setter)) & ~fighterBit(kEnemyFighter) & 0x0F);
        enemySeated_ = false;
        failed_ = false;
        spot_ = TableSpot{};
        state_ = TableState::Setting;
        until_ = t + tuning_.setTime;
        pending_.push(TableEventType::Setting, t);
        advance(t); // a setTime of 0 sets it at once
        return TableError::None;
    }

    // Begins at `spot`, unless one of `others` (count of them; null ones and this one are skipped) is Set on an
    // overlapping spot: then Occupied, and `load` and the cooldown are untouched.
    TableError beginAt(FighterId setter, Load& load, const PlaceTable& places, std::uint8_t seated, TableSpot spot,
                       const TableSet* const* others, std::size_t count, double t) {
        for (std::size_t i = 0; others && i < count; ++i) {
            const TableSet* o = others[i];
            if (o && o != this && o->set() && tableSpotsOverlap(o->spot(), spot)) {
                advance(t);
                return TableError::Occupied;
            }
        }
        const TableError e = begin(setter, load, places, seated, t);
        if (e == TableError::None) spot_ = spot;
        return e;
    }

    // The bodies at a set table changed (or it just set). True when they are too few and it guttered out.
    bool checkBodies(int bodies, double t) {
        t = advance(t);
        if (state_ != TableState::Set || bodies >= tuning_.minBodies) return false;
        failed_ = true;
        enemySeated_ = false;
        pending_.push(TableEventType::Guttered, t);
        startCooling(t, tuning_.failCooldown);
        return true;
    }

    // The setter is interrupted. True when it failed the table (only while Setting).
    bool interrupt(double t) {
        t = advance(t);
        if (state_ != TableState::Setting) return false;
        failed_ = true;
        pending_.push(TableEventType::Failed, t);
        startCooling(t, tuning_.failCooldown);
        return true;
    }

    // The seats under a setting or set table changed. True when the table guttered out.
    bool checkSeats(const PlaceTable& places, double t) {
        t = advance(t);
        if (state_ != TableState::Setting && state_ != TableState::Set) return false;
        if (places.tableSeats() >= tuning_.minSeats) return false;
        failed_ = true;
        enemySeated_ = false;
        pending_.push(TableEventType::Guttered, t);
        startCooling(t, tuning_.failCooldown);
        return true;
    }

    // Clears a set table early. True when one was set.
    bool cancel(double t) {
        t = advance(t);
        if (state_ != TableState::Set) return false;
        clear(t);
        return true;
    }

    // Seats the enemy. True when the table is set and it was not seated yet.
    bool seatEnemy(double t) {
        t = advance(t);
        if (state_ != TableState::Set || enemySeated_) return false;
        enemySeated_ = true;
        pending_.push(TableEventType::EnemySeated, t);
        return true;
    }

    TableSplit split(FighterId target, float amount, double t) {
        advance(t);
        TableSplit r;
        if (!isFighter(target) || !(amount > 0.0f)) return r;
        const bool ally = target != kEnemyFighter && (seated_ & fighterBit(target)) != 0;
        if (state_ != TableState::Set || !ally) {
            r.share[target] = amount;
            return r;
        }
        int n = enemySeated_ ? 1 : 0;
        for (FighterId f = 0; f < kEnemyFighter; ++f)
            if (seated_ & fighterBit(f)) ++n;
        const float each = amount / static_cast<float>(n);
        for (FighterId f = 0; f < kEnemyFighter; ++f)
            if (seated_ & fighterBit(f)) r.share[f] = each;
        if (enemySeated_) r.share[kEnemyFighter] = each * tuning_.enemyShare;
        r.shared = n > 1;
        return r;
    }

    const TableEvents& update(double now) {
        advance(now);
        out_ = pending_;
        pending_.clear();
        return out_;
    }

    TableState state() const { return state_; }
    bool set() const { return state_ == TableState::Set; }
    bool failed() const { return failed_; }
    bool enemySeated() const { return enemySeated_; }
    std::uint8_t seated() const { return seated_; }
    FighterId setter() const { return setter_; }
    double until() const { return until_; }
    const TableTuning& tuning() const { return tuning_; }
    const TableSpot& spot() const { return spot_; } // the last beginAt's (radius 0 after begin or reset)

private:
    double advance(double t) {
        now_ = inputTime(now_, t);
        // Play every timed change up to now, each at its own time.
        for (;;) {
            if (state_ == TableState::Setting && until_ <= now_) {
                state_ = TableState::Set;
                pending_.push(TableEventType::Set, until_);
                until_ = until_ + tuning_.setDuration;
            } else if (state_ == TableState::Set && until_ <= now_) {
                clear(until_);
            } else if (state_ == TableState::Cooling && until_ <= now_) {
                state_ = TableState::Ready;
                pending_.push(TableEventType::Ready, until_);
            } else {
                break;
            }
        }
        return now_;
    }
    void clear(double t) {
        enemySeated_ = false;
        pending_.push(TableEventType::Cleared, t);
        startCooling(t, tuning_.cooldown);
    }
    void startCooling(double t, double seconds) {
        state_ = TableState::Cooling;
        until_ = t + seconds;
        if (seconds <= 0.0) {
            state_ = TableState::Ready;
            pending_.push(TableEventType::Ready, t);
        }
    }

    TableTuning tuning_{};
    TableState state_ = TableState::Ready;
    double now_ = 0.0;
    double until_ = 0.0;
    FighterId setter_ = kNoFighter;
    std::uint8_t seated_ = 0;
    bool enemySeated_ = false;
    bool failed_ = false;
    TableSpot spot_{};
    TableEvents pending_{};
    TableEvents out_{};
};

} // namespace ftcore
