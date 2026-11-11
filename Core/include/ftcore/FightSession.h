#pragma once

// The fight session: one object that owns every shared combat input of a fight (the count, the gift slot, each
// fighter's places, the table, the held refusal, the weak-point lock and an optional boss script) and moves them all
// on one fight clock. See docs/SHARED_COMBAT_INPUTS.md section 7.
//
// * One clock. tick(fightDt) is the only time input: the session adds the step to its clock and updates every piece
//   with it, each frame. No call takes a time; presses and blows are stamped with the session's now(). The host
//   passes 0 while its hit-stop holds, so the shared inputs see fight time that does not advance during hit-stop.
// * A step that is negative or not finite counts as 0, and one step is at most kMaxFightStep, so the clock can never
//   jump far enough for a piece to refuse it (kMaxTimeJump).
// * Events: tick() returns what each piece emitted since the last tick (calls between ticks buffer theirs).
// * Glue: a blow judged on the count (a shared outcome, at once or when a held blow settles) is a shared blow on the
//   boss script, or an offer of the shared-blow key to the session's own lock when there is no boss script. A settle
//   the count's full event buffer lost (droppedSettles) is not a shared blow. A change to
//   the table setter's places re-checks the table's seats.
// * Story: with a story state bound, every press of the gift button aimed at a lead is one press in it
//   (storyCountPress), given or refused (cooling, a refused count, a hostile or vacant slot): the side is the
//   target lead (kLeadA 0, kLeadB 1), the kind is the selected gift (Heal 0, Guard 1, Count 2). A press at a
//   non-lead has no side and writes nothing.
//   The per-kind press counters (counter wides) roll back with a scene restart, so a retried fight counts afresh.
// * The session keeps its own copy of the boss def; it cannot be copied or moved (the script points into it).
// * No allocation after construction (the UE wrapper allocates its session once, when it is made).

#include "ftcore/BossScript.h"
#include "ftcore/CombatInputs.h"
#include "ftcore/GiftSlot.h"
#include "ftcore/HeldRefusal.h"
#include "ftcore/Load.h"
#include "ftcore/Places.h"
#include "ftcore/StoryState.h"
#include "ftcore/StrikeCount.h"
#include "ftcore/TableSet.h"
#include "ftcore/WeakPointLock.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace ftcore {

// One tick's fight time at most (seconds). A host frame longer than this loses the rest, as a paused game would.
inline constexpr double kMaxFightStep = 0.25;
// The furthest a fight may start from 0 on the fight clock (about 31 years in seconds). Further out a double's
// spacing grows past a frame's step, so `now += step` would stop moving the clock; begin clamps to it.
inline constexpr double kMaxFightStart = 1.0e9;

// The shared inputs a press can make, for a host's binding table to map its actions onto. Append only.
enum class SharedAction : std::uint8_t {
    Count,      // call the count
    Gift,       // give the selected gift to `target`
    RefuseDown, // the refusal button went down (the host maps its held refusal button here)
    RefuseUp,   // and came up
    SetTable,   // set the table (needs the setter's Load: use setTable)
    LeaveOpen,  // leave a place open
};

struct FightSessionDef {
    CountTuning count{};
    GiftTuning gift{};
    PlaceTuning places{};
    int seats = 3;
    TableTuning table{};
    // The session's own lock, used when there is no boss script (a boss script has its own, from its def).
    bool locked = false;
    LockTuning lock{};
    bool hasBoss = false;
    BossScriptDef boss{};
};

// What begin() refused: each refused piece keeps its defaults, except a refused boss def, which leaves no boss script.
struct FightSessionCheck {
    bool count = true;
    bool gift = true;
    bool places = true;
    bool table = true;
    bool lock = true;
    bool boss = true;
    bool start = true; // false: the start was past kMaxFightStart (clamped) or not finite (0)
    bool ok() const { return count && gift && places && table && lock && boss && start; }
};

// What one tick emitted. The pointers stay valid until the next tick or begin.
// One tick's results. The event buffers are the pieces' own (no copy): read them before the next session call,
// which may refill them.
struct FightTick {
    double now = 0.0;
    double step = 0.0; // the fight time this tick added (0 during hit-stop)
    const CountEvents* count = nullptr;
    const TableEvents* table = nullptr;
    const BossEvents* boss = nullptr; // empty when there is no boss script
    int sharedBlows = 0;              // shared blows the settled count blows dealt the boss or the lock
};

// What a press did, for the generic press(); the typed calls return the pieces' own results.
struct SharedReply {
    bool accepted = false;
    CountError count = CountError::None;
    GiftResult gift{};
    RefusalInput refusal = RefusalInput::Ignored;
    PlaceError place = PlaceError::None;
};

class FightSession {
public:
    FightSession() { begin(FightSessionDef{}, 0.0); }
    FightSession(const FightSession&) = delete;
    FightSession& operator=(const FightSession&) = delete;

    // Starts a fight at `start` on the fight clock (not bound by kMaxTimeJump; clamped to +-kMaxFightStart, a
    // start that is not finite is 0).
    FightSessionCheck begin(const FightSessionDef& def, double start) {
        FightSessionCheck c;
        c.start = std::isfinite(start) && start >= -kMaxFightStart && start <= kMaxFightStart;
        now_ = startTime(start < -kMaxFightStart ? -kMaxFightStart : (start > kMaxFightStart ? kMaxFightStart : start));
        def_ = def;
        c.count = count_.reset(def_.count, now_);
        if (!c.count) count_.reset(CountTuning{}, now_);
        c.gift = gift_.reset(def_.gift, now_);
        if (!c.gift) gift_.reset(GiftTuning{}, now_);
        for (PlaceTable& p : places_) {
            if (!p.reset(def_.seats, def_.places)) {
                c.places = false;
                p.reset();
            }
        }
        c.table = table_.reset(def_.table, now_);
        if (!c.table) table_.reset(TableTuning{}, now_);
        c.lock = lock_.reset(def_.lock, now_);
        if (!c.lock) lock_.reset(LockTuning{}, now_);
        refusal_ = HeldRefusal{};
        // A refused boss def leaves no boss script: the weak point follows the session's lock, as without one.
        c.boss = !def_.hasBoss || boss_.reset(def_.boss, now_);
        hasBoss_ = def_.hasBoss && c.boss;
        lastStory_ = SetResult::Unchanged;
        tick_ = FightTick{};
        tick_.now = now_;
        tick_.count = &kNoCount;
        tick_.table = &kNoTable;
        tick_.boss = &kNoBoss;
        return c;
    }

    // The story state presses are written to (both or neither; null unbinds).
    void bindStory(StoryState* story, const FactRegistry* reg) {
        story_ = story && reg ? story : nullptr;
        reg_ = story && reg ? reg : nullptr;
    }
    // What the last gift's story write returned (Unchanged when none was made).
    SetResult lastStoryWrite() const { return lastStory_; }

    // Moves the fight clock by `fightDt` (0 during hit-stop) and every piece with it.
    const FightTick& tick(double fightDt) {
        const double step = std::isfinite(fightDt) && fightDt > 0.0 ? (fightDt < kMaxFightStep ? fightDt : kMaxFightStep) : 0.0;
        now_ += step;
        tick_.now = now_;
        tick_.step = step;
        tick_.sharedBlows = 0;
        tick_.count = &count_.update(now_);
        for (const CountEvent& e : *tick_.count)
            if (sharedBlow(e) && shareBlow()) ++tick_.sharedBlows;
        gift_.update(now_);
        tick_.table = &table_.update(now_);
        lock_.update(now_);
        tick_.boss = hasBoss_ ? &boss_.update(now_) : &kNoBoss;
        return tick_;
    }

    double now() const { return now_; }

    // ---- Presses (stamped with now()) ----

    SharedReply press(SharedAction action, FighterId who, FighterId target = kNoFighter) {
        SharedReply r;
        switch (action) {
        case SharedAction::Count:
            r.count = callCount(who);
            r.accepted = r.count == CountError::None;
            break;
        case SharedAction::Gift:
            r.gift = giveGift(target);
            r.accepted = r.gift.given;
            break;
        case SharedAction::RefuseDown:
            r.refusal = refusal_.holdDown();
            r.accepted = r.refusal != RefusalInput::Ignored;
            break;
        case SharedAction::RefuseUp:
            r.refusal = refusal_.holdUp();
            r.accepted = r.refusal != RefusalInput::Ignored;
            break;
        case SharedAction::LeaveOpen:
            r.place = leaveOpen(who);
            r.accepted = r.place == PlaceError::None;
            break;
        case SharedAction::SetTable: // needs a Load: setTable()
            break;
        }
        return r;
    }

    CountError callCount(FighterId caller, CountVoice voice = CountVoice::Voice) { return count_.call(caller, now_, voice); }
    void falseCount(double shift) { count_.falseCall(now_, shift); }
    void cancelCount() { count_.cancel(now_); }
    void cancelFalseCount() { count_.cancelFalse(now_); }

    bool selectGift(Gift g) { return gift_.select(g); }
    GiftResult giveGift(FighterId target) {
        const GiftResult r = gift_.press(target, now_, &count_);
        lastStory_ = SetResult::Unchanged;
        if (story_ && isLead(r.target) && r.gift != Gift::None) // the slot never selects None
            lastStory_ = storyCountPress(*story_, *reg_, r.target, static_cast<std::uint8_t>(r.gift));
        return r;
    }

    PlaceError layPlace(FighterId who, PersonId person) { return placeChanged(who, isFighter(who) ? places_[who].layPlace(person) : PlaceError::BadPerson); }
    PlaceError leaveOpen(FighterId who) { return placeChanged(who, isFighter(who) ? places_[who].leaveOpen() : PlaceError::BadPerson); }
    PlaceError clearPlace(FighterId who, int chair) { return placeChanged(who, isFighter(who) ? places_[who].clearPlace(chair) : PlaceError::BadPerson); }
    PlaceEmptied emptyPlace(FighterId who, PersonId person, Load* load = nullptr) {
        if (!isFighter(who)) return PlaceEmptied{};
        const PlaceEmptied r = places_[who].emptyPlace(person, load);
        placeChanged(who, PlaceError::None);
        return r;
    }

    TableError setTable(FighterId setter, Load& load, std::uint8_t seated) {
        if (!isFighter(setter)) return TableError::BadSetter;
        return table_.begin(setter, load, places_[setter], seated, now_);
    }
    bool interruptTable() { return table_.interrupt(now_); }
    bool cancelTable() { return table_.cancel(now_); }
    bool seatEnemy() { return table_.seatEnemy(now_); }
    TableSplit split(FighterId target, float amount) { return table_.split(target, amount, now_); }

    void offerRefusal() { refusal_.offer(); }
    RefusalInput acceptRefusal() { return refusal_.accept(); }
    RefusalInput deferRefusal() { return refusal_.defer(); }
    // Ends the host's prompt; a refusal kept to the end offers the refusal key to the session's own lock. A boss
    // script's weak point opens only through its counters and the shared blow, so with one the refusal opens nothing.
    RefusalInput endRefusal() {
        const RefusalInput r = refusal_.offerEnded();
        if (refusal_.refused() && !hasBoss_ && def_.locked) lock_.offer(kUnlockRefusal, now_);
        return r;
    }

    // A blow by `striker`, judged on the count before its damage; an OnCount blow is a shared blow at once.
    CountHit judgeBlow(FighterId striker) {
        const CountHit h = count_.judgeBlow(striker, now_);
        if (sharedBlow(h)) shareBlow();
        return h;
    }
    // An unlock key offered to the session's own lock (no boss script; a boss script takes its keys itself).
    UnlockResult offerKey(UnlockKey key, std::uint16_t clause = 0) {
        if (hasBoss_ || !def_.locked) return UnlockResult::BadKey;
        return lock_.offer(key, now_, clause);
    }
    // The weak point is open: always without a lock, else while the lock or the boss script says so.
    bool weakPointOpen() const {
        if (hasBoss_) return boss_.unlocked();
        return !def_.locked || lock_.open(now_);
    }

    // The boss script's inputs (Ignored, or nothing, without one). `shared`: the counter carries the shared input, a
    // blow judged a shared outcome (sharedBlow(judgeBlow(...))) or a hit the lead took half of; a shared counter
    // kind without it is refused (NeedsShared).
    BossInput counter(BossCounter kind, FighterId who, bool shared = false) {
        if (!hasBoss_) return BossInput::Ignored;
        return boss_.counter(kind, who, now_, shared);
    }
    ScriptHit bossHit(int amount, bool onWeakPoint) { return hasBoss_ ? boss_.hit(amount, onWeakPoint, now_) : ScriptHit{}; }
    bool answer() { return hasBoss_ && boss_.answer(now_); }
    void setWatched(bool watched) {
        if (hasBoss_) boss_.setWatched(watched, now_);
    }
    int addResource(int n) { return hasBoss_ ? boss_.addResource(n, now_) : 0; }

    // ---- Reads ----

    const StrikeCount& strikeCount() const { return count_; }
    const GiftSlot& giftSlot() const { return gift_; }
    GiftSlot& giftSlot() { return gift_; } // setState, save and load
    // A fighter's places (kLeadA's for an id that is not a fighter). The mutable one is for loads: call recheckTable after.
    const PlaceTable& places(FighterId who) const { return places_[isFighter(who) ? who : 0]; }
    PlaceTable& places(FighterId who) { return places_[isFighter(who) ? who : 0]; }
    const TableSet& table() const { return table_; }
    const HeldRefusal& refusal() const { return refusal_; }
    const WeakPointLock& lock() const { return lock_; }
    bool hasBoss() const { return hasBoss_; }
    const BossScript& boss() const { return boss_; } // meaningful only while hasBoss()
    const FightSessionDef& def() const { return def_; }

    // Re-checks the table's seats against its setter's places (after a host changed them directly).
    bool recheckTable() {
        const TableState s = table_.state();
        if ((s != TableState::Setting && s != TableState::Set) || !isFighter(table_.setter())) return false;
        return table_.checkSeats(places_[table_.setter()], now_);
    }

private:
    PlaceError placeChanged(FighterId who, PlaceError e) {
        if (e == PlaceError::None && who == table_.setter()) recheckTable();
        return e;
    }
    bool shareBlow() {
        if (hasBoss_) return boss_.sharedBlow(now_);
        if (!def_.locked) return false;
        const UnlockResult u = lock_.offer(kUnlockSharedBlow, now_);
        return u == UnlockResult::Unlocked || u == UnlockResult::Refreshed;
    }

    double now_ = 0.0;
    FightSessionDef def_{};
    StrikeCount count_{};
    GiftSlot gift_{};
    std::array<PlaceTable, kMaxFighters> places_{};
    TableSet table_{};
    HeldRefusal refusal_{};
    WeakPointLock lock_{};
    bool hasBoss_ = false;
    BossScript boss_{};
    StoryState* story_ = nullptr;
    const FactRegistry* reg_ = nullptr;
    SetResult lastStory_ = SetResult::Unchanged;
    FightTick tick_{};
    // The empty buffers a tick points at before the first tick and when there is no boss script.
    static inline const CountEvents kNoCount{};
    static inline const TableEvents kNoTable{};
    static inline const BossEvents kNoBoss{};
};

} // namespace ftcore
