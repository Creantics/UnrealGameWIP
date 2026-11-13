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
// * The count's Load and the Lift (def.lift; off by default, and off nothing below happens). The host feeds each
//   lead's Load (setLoad; a lead never fed has no gauge) before it judges blows. When a window becomes joined, each
//   lead joiner with a gauge stores countLoad, at most once per countLoadEvery per lead; a Lift stores none and each
//   lead joiner spends min(liftCost, its Load) instead. These come as the next tick's `loads`; the host applies them
//   to its gauges (its own capped store and spend). A kThirdSlot caller joins without a gauge (StrikeCount.h).
// * Marks: markEnemy(enemy, bonus, seconds) marks an enemy slot (a Release's chair effect). The joined blows of the
//   next joined window judged on that enemy (judgeHeldBlow, held ones at their settle) take x (1 + bonus); the mark
//   is used up once that window is over, or ends unused at its time. begin clears every mark.
// * Story: with a story state bound, every press of the gift button aimed at a lead is one press in it
//   (storyCountPress), given or refused (cooling, a refused count, a hostile or vacant slot): the side is the
//   target lead (kLeadA 0, kLeadB 1), the kind is the selected gift (Heal 0, Guard 1, Count 2). A press at a
//   non-lead has no side and writes nothing.
//   The per-kind press counters (counter wides) roll back with the fight step (storyEndStep: the story marks the
//   step as it opens and goes back to the mark on a retry or a quit), so a retried fight counts afresh. The
//   binding enforces it: it lasts one fight (begin unbinds) and is refused unless the state is still at the
//   step's mark, so a screen that retries a fight itself must roll the last attempt back (storyEndStep with
//   Retry), mark the step again and bind anew, or the new attempt writes nothing. Each attempt thrown away counts
//   one story retry, whoever retries it: a screen's own "fight again" adds 1 (its Retry), and a lost fight the
//   story retries adds 1 more (the director's Retry verdict).
// * The session keeps its own copy of the boss def; it cannot be copied or moved (the script points into it).
// * No allocation after construction (the UE wrapper allocates its session once, when it is made).

#include "ftcore/BossScript.h"
#include "ftcore/CombatKind.h"
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
#include <initializer_list>
#include <span>

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
    bool lift = false; // the count's Load and the Lift (see above)
    LiftTuning liftTuning{};
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
    bool lift = true;  // false: the lift tuning was refused (the Lift stays off)
    bool start = true; // false: the start was moved: past kMaxFightStart or infinite (clamped to it), or NaN (0)
    // Every piece took its tuning. A moved start is reported in `start` but is not a refusal: the fight began.
    bool ok() const { return count && gift && places && table && lock && boss && lift; }
};

// A Load change the count asks of a lead's gauge (def.lift): the host applies it, store or spend. Append only.
enum class CountLoadKind : std::uint8_t { CountStore, LiftSpend };
struct CountLoadChange {
    FighterId lead = kNoFighter;
    int amount = 0; // CountStore: the Load to store (above 0); LiftSpend: the Load to spend (above 0)
    CountLoadKind kind = CountLoadKind::CountStore;
};
// Two joiners per window: a tick between joins holds at most a few.
inline constexpr std::size_t kMaxCountLoadChanges = 8;

// A Release's mark on an enemy slot (markEnemy).
struct EnemyMark {
    float bonus = 0.f;
    double until = -1.0e300;
    bool used = false; // a joined window's blow on the enemy took it; it ends with that window
};
inline constexpr std::size_t kMaxMarkedEnemies = 8;

// A held blow's settle above x1: the extra damage its enemy takes now (the blow itself landed at base).
struct HeldBonus {
    std::uint8_t enemy = 0;
    std::uint8_t move = 0;
    int extraDamage = 0; // round(baseDamage * (multiplier - 1)), above 0
};

// Held blows the session can wait on at once (judgeHeldBlow); one more lands at base with no bonus.
inline constexpr std::size_t kMaxHeldBlows = 16;

// One tick's results. The event buffers are the pieces' own (no copy): read them before the next session call,
// which may refill them; the pointers stay valid until the next tick or begin.
struct FightTick {
    double now = 0.0;
    double step = 0.0; // the fight time this tick added (0 during hit-stop)
    const CountEvents* count = nullptr;
    const TableEvents* table = nullptr;
    const BossEvents* boss = nullptr; // empty when there is no boss script
    int sharedBlows = 0;              // shared blows the settled count blows dealt the boss or the lock
    std::span<const HeldBonus> bonuses; // this tick's held-blow bonuses (the session's own buffer)
    int heldDropped = 0;  // held blows judged since the last tick with no room to wait (they stay at base)
    int heldExpired = 0;  // held blows whose settle never came (a dropped settle): they stay at base
    std::span<const CountLoadChange> loads; // def.lift: the gauges' changes since the last tick (the session's own buffer)
    int loadsDropped = 0;                   // changes with no room in the buffer (never with two joiners a tick)
};

// What a press did, for the generic press(); the typed calls return the pieces' own results.
struct SharedReply {
    bool accepted = false;
    CountError count = CountError::None;
    GiftResult gift{};
    RefusalInput refusal = RefusalInput::Ignored;
    PlaceError place = PlaceError::None;
};

// The story's hand-off to a fight, as the host sees it when the fight begins or restarts: whether the story handed
// this fight over (a valid carry), whether that carry is already settled (won or lost), and the combat kind it named.
struct FightHandOff {
    bool fromStory = false;
    bool settled = false;
    CombatKind kind = CombatKind::Arena;
};

// Whether a fight's session takes the story's writes: only when the story handed off this kind of fight and it is not
// settled yet. A fight with no hand-off (standalone, demo, a practice run) or a stale one for another kind is unbound.
constexpr bool fightTakesStory(const FightHandOff& h, CombatKind fight) {
    return h.fromStory && !h.settled && h.kind == fight;
}

class FightSession {
public:
    FightSession() { begin(FightSessionDef{}, 0.0); }
    FightSession(const FightSession&) = delete;
    FightSession& operator=(const FightSession&) = delete;

    // Starts a fight at `start` on the fight clock (not bound by kMaxTimeJump). A start past +-kMaxFightStart, or
    // an infinite one, is clamped to +-kMaxFightStart; a NaN start is 0. Either way the fight begins (check.start).
    FightSessionCheck begin(const FightSessionDef& def, double start) {
        FightSessionCheck c;
        c.start = std::isfinite(start) && start >= -kMaxFightStart && start <= kMaxFightStart;
        now_ = startTime(start < -kMaxFightStart ? -kMaxFightStart : (start > kMaxFightStart ? kMaxFightStart : start));
        def_ = def;
        c.count = count_.reset(def_.count, now_);
        if (!c.count) count_.reset(CountTuning{}, now_);
        c.lift = count_.setLift(def_.lift, def_.liftTuning);
        lastStore_.fill(-1.0e300);
        pendingLoads_ = 0;
        pendingLoadsDropped_ = 0;
        marks_.fill(EnemyMark{});
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
        story_ = nullptr; // a binding lasts one fight: a restart binds again, at a fresh mark
        reg_ = nullptr;
        tick_ = FightTick{};
        heldCount_ = 0; // a held blow from the last fight never pays out
        heldDropped_ = 0;
        tick_.now = now_;
        tick_.count = &kNoCount;
        tick_.table = &kNoTable;
        tick_.boss = &kNoBoss;
        return c;
    }

    // The story state presses are written to, from now until the next begin. Bind only a fight the story handed off
    // (fightTakesStory); a standalone, demo or practice fight stays unbound and writes nothing. False (and unbound)
    // unless the state is at the step's mark (storyAtMark): an attempt whose writes were not rolled back never
    // lets the next one count on top of it.
    bool bindStory(StoryState& story, const FactRegistry& reg, const StoryStepMark& mark) {
        const bool ok = storyAtMark(story, reg, mark);
        story_ = ok ? &story : nullptr;
        reg_ = ok ? &reg : nullptr;
        return ok;
    }
    void unbindStory() {
        story_ = nullptr;
        reg_ = nullptr;
    }
    bool storyBound() const { return story_ != nullptr; }
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
        tick_.heldDropped = heldDropped_;
        heldDropped_ = 0;
        tick_.heldExpired = 0;
        std::size_t bonuses = 0;
        for (const CountEvent& e : *tick_.count) {
            if (sharedBlow(e) && shareBlow()) ++tick_.sharedBlows;
            if (e.type == CountEventType::BlowSettled) settleHeld(e, bonuses);
        }
        // Every held blow has settled or its settle was dropped: what is still waiting never will.
        if (count_.heldBlows() == 0) {
            tick_.heldExpired = static_cast<int>(heldCount_);
            heldCount_ = 0;
        }
        tick_.bonuses = std::span<const HeldBonus>(bonuses_.data(), bonuses);
        // The gauges' changes since the last tick, then the marks: a used one ends with its window, an unused one at its time.
        tickLoads_ = pendingLoads_;
        for (std::size_t i = 0; i < pendingLoads_; ++i) loadsOut_[i] = loadsIn_[i];
        tick_.loads = std::span<const CountLoadChange>(loadsOut_.data(), tickLoads_);
        tick_.loadsDropped = pendingLoadsDropped_;
        pendingLoads_ = 0;
        pendingLoadsDropped_ = 0;
        for (EnemyMark& m : marks_) {
            if ((m.used && !count_.live()) || (!m.used && now_ > m.until)) m = EnemyMark{};
        }
        gift_.update(now_);
        tick_.table = &table_.update(now_);
        lock_.update(now_);
        tick_.boss = hasBoss_ ? &boss_.update(now_) : &kNoBoss;
        return tick_;
    }

    double now() const { return now_; }

    // The lead the player's figure fills (kLeadA until set): the host aims its gift presses at it and reports its
    // blows and calls for it. The host sets it when the fight begins and whenever the player switches lead; it
    // stays across begin (a party fact, not a fight's). False (unchanged) for a non-lead.
    bool setActiveLead(FighterId lead) {
        if (!isLead(lead)) return false;
        activeLead_ = lead;
        return true;
    }
    FighterId activeLead() const { return activeLead_; }

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
    // "Not yet" (a yes): the deferral relocks an unlocked point, the boss script's or the session's own lock, when
    // that lock's relockOnDefer says so. An offer that ends unheld (endRefusal) is a deferral too.
    RefusalInput deferRefusal() {
        const RefusalInput r = refusal_.defer();
        if (r == RefusalInput::Resolved) deferLocks();
        return r;
    }
    // Ends the host's prompt; a refusal kept to the end offers the refusal key: to the boss script when there is one
    // (it opens the point only when refusal is its key kind and a counter has the point exposed), else to the
    // session's own lock. Only the call that resolves the refusal offers it: ending an ended prompt again offers nothing.
    RefusalInput endRefusal() {
        const RefusalInput r = refusal_.offerEnded();
        if (r == RefusalInput::Resolved && refusal_.refused()) {
            if (hasBoss_) boss_.offerKey(kUnlockRefusal, now_);
            else if (def_.locked) lock_.offer(kUnlockRefusal, now_);
        } else if (r == RefusalInput::Resolved && refusal_.deferred()) {
            deferLocks();
        }
        return r;
    }

    // A blow by `striker`, judged on the count before its damage; an OnCount (or Lift) blow is a shared blow at once.
    CountHit judgeBlow(FighterId striker) {
        const CountHit h = count_.judgeBlow(striker, now_);
        takeJoin();
        if (sharedBlow(h)) shareBlow();
        return h;
    }
    // A fighter's Load now, for the Lift (def.lift); negative: no gauge. Feed it before judging its blows.
    void setLoad(FighterId f, int load) { count_.setLoad(f, load); }
    // Marks enemy slot `enemy`: its next joined window's blows take x (1 + bonus), bonus clamped to
    // 0..kMaxReleaseMark, for `seconds` (0..15) from now. A new mark replaces the old. False (nothing) for a slot
    // past kMaxMarkedEnemies, a bonus of 0 or less, or seconds of 0 or less (or not finite).
    bool markEnemy(std::uint8_t enemy, float bonus, double seconds) {
        if (enemy >= kMaxMarkedEnemies || !(bonus > 0.f) || !(seconds > 0.0) || !std::isfinite(seconds)) return false;
        EnemyMark m;
        m.bonus = bonus < kMaxReleaseMark ? bonus : kMaxReleaseMark;
        m.until = now_ + (seconds < 15.0 ? seconds : 15.0);
        marks_[enemy] = m;
        return true;
    }
    EnemyMark mark(std::uint8_t enemy) const { return enemy < kMaxMarkedEnemies ? marks_[enemy] : EnemyMark{}; }
    // judgeBlow for a blow the host lands at `baseDamage` on `enemy` now. A Held blow lands at base and the session
    // waits on it: when it settles above x1, a later tick's `bonuses` lists the extra damage to add (the host deals
    // it outside a blow). A settle at x1 or below, a dropped settle (heldExpired) or no room to wait (heldDropped)
    // adds nothing. Other outcomes are final: the host multiplies as with judgeBlow.
    CountHit judgeHeldBlow(FighterId striker, std::uint8_t enemy, int baseDamage, std::uint8_t move) {
        CountHit h = judgeBlow(striker);
        if (sharedOutcome(h.outcome)) h.multiplier *= markFactor(enemy);
        if (h.outcome != CountOutcome::Held || baseDamage <= 0) return h;
        if (heldCount_ == kMaxHeldBlows) {
            ++heldDropped_;
            return h;
        }
        held_[heldCount_++] = HeldRecord{h.blow, enemy, move, baseDamage};
        return h;
    }
    // An unlock key (a read clause with its id, a refusal, a shared blow) offered to the boss script's point when
    // there is one (BossScript::offerKey: only its key kind, only while exposed), else to the session's own lock. A shared
    // blow offered here is the host's own: it skips the count (tick().sharedBlows does not see it).
    UnlockResult offerKey(UnlockKey key, std::uint16_t clause = 0) {
        if (hasBoss_) return boss_.offerKey(key, now_, clause);
        if (!def_.locked) return UnlockResult::BadKey;
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
    struct HeldRecord {
        std::uint16_t blow = 0;
        std::uint8_t enemy = 0;
        std::uint8_t move = 0;
        int baseDamage = 0;
    };
    // Frees the settled blow's record; above x1 it adds a bonus (at most one per record, so the buffer fits).
    void settleHeld(const CountEvent& e, std::size_t& bonuses) {
        for (std::size_t i = 0; i < heldCount_; ++i) {
            if (held_[i].blow != e.blow) continue;
            const HeldRecord r = held_[i];
            held_[i] = held_[--heldCount_];
            const float mult = sharedOutcome(e.outcome) ? e.multiplier * markFactor(r.enemy) : e.multiplier;
            if (!(mult > 1.0f) || !std::isfinite(mult)) return;
            const double extra = std::round(static_cast<double>(r.baseDamage) * (static_cast<double>(mult) - 1.0));
            if (extra < 1.0) return;
            const int add = extra > 1e9 ? 1000000000 : static_cast<int>(extra);
            bonuses_[bonuses++] = HeldBonus{r.enemy, r.move, add};
            return;
        }
    }
    // A joined blow on `enemy`: its mark's factor (1 + bonus), using the mark; 1 with no live mark.
    float markFactor(std::uint8_t enemy) {
        if (enemy >= kMaxMarkedEnemies) return 1.0f;
        EnemyMark& m = marks_[enemy];
        if (!(m.bonus > 0.f) || (!m.used && now_ > m.until)) return 1.0f;
        m.used = true;
        return 1.0f + m.bonus;
    }
    void pushLoad(FighterId lead, int amount, CountLoadKind kind) {
        if (pendingLoads_ == loadsIn_.size()) {
            ++pendingLoadsDropped_;
            return;
        }
        loadsIn_[pendingLoads_++] = CountLoadChange{lead, amount, kind};
    }
    // A window just joined (def.lift): each lead joiner with a gauge stores countLoad (rate-limited) or, on a Lift,
    // spends min(liftCost, its Load).
    void takeJoin() {
        CountJoin j;
        if (!count_.takeJoin(j)) return;
        const LiftTuning& l = count_.liftTuning();
        for (const FighterId f : {j.first, j.second}) {
            if (!isLead(f) || !count_.hasGauge(f)) continue;
            if (j.lift) {
                const int held = count_.loadOf(f);
                const int spend = l.liftCost < held ? l.liftCost : held;
                if (spend > 0) {
                    pushLoad(f, spend, CountLoadKind::LiftSpend);
                    count_.setLoad(f, held - spend);
                }
            } else if (l.countLoad > 0 && (l.countLoadEvery <= 0.0 || now_ - lastStore_[f] >= l.countLoadEvery - 1e-9)) {
                pushLoad(f, l.countLoad, CountLoadKind::CountStore);
                lastStore_[f] = now_;
            }
        }
    }
    void deferLocks() {
        if (hasBoss_) boss_.onDeferred(now_);
        else if (def_.locked) lock_.onDeferred(now_);
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
    FighterId activeLead_ = kLeadA;
    StoryState* story_ = nullptr;
    const FactRegistry* reg_ = nullptr;
    SetResult lastStory_ = SetResult::Unchanged;
    FightTick tick_{};
    std::array<HeldRecord, kMaxHeldBlows> held_{};
    std::size_t heldCount_ = 0;
    int heldDropped_ = 0;
    std::array<HeldBonus, kMaxHeldBlows> bonuses_{};
    std::array<double, kMaxFighters> lastStore_{};
    std::array<CountLoadChange, kMaxCountLoadChanges> loadsIn_{};
    std::array<CountLoadChange, kMaxCountLoadChanges> loadsOut_{};
    std::size_t pendingLoads_ = 0;
    std::size_t tickLoads_ = 0;
    int pendingLoadsDropped_ = 0;
    std::array<EnemyMark, kMaxMarkedEnemies> marks_{};
    // The empty buffers a tick points at before the first tick and when there is no boss script.
    static inline const CountEvents kNoCount{};
    static inline const TableEvents kNoTable{};
    static inline const BossEvents kNoBoss{};
};

} // namespace ftcore
