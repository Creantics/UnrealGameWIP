#pragma once

// A data-driven boss state machine for the shared-input bosses (docs/BOSS_SCRIPT.md). One machine,
// many bosses: each boss is a BossScriptDef, loaded from Data/Battle/boss_script JSON (docs/BOSS_SCRIPT.md).
//
// * Moves (at most kMaxScriptMoves): each is a tell, a window and a recovery. The host reads the Tell
//   event to play the move's tell; the counter is accepted only inside the window. A window that runs
//   out uncountered lands (Landed: the host applies the move's damage). A countered move goes straight
//   to its recovery. Counters a lone fighter cannot make (StrikeOnCount, TakeHalf, CarryTogether) need
//   the shared input (`onCount`: StrikeCount's OnCount, or a hit a lead took half of); CarryTogether
//   needs two different fighters in the same window.
// * Phases (at most kMaxScriptPhases): each has its own move rotation, played in order and repeated. A
//   phase starts when the boss's gauge falls to its `enterAt`: the hp share (Hp gauge) or a resource
//   count (Resource gauge, e.g. something in the arena the boss consumes). Phases never go back.
// * The locked weak point (WeakPointLock.h). Countering a move with exposeSeconds exposes the point
//   for that long; the point's key kind (`key`: a shared blow, a read clause or a
//   refusal) offered while exposed (sharedBlow, offerKey) unlocks it, and any other key
//   opens nothing. A move with unlocksOnCounter unlocks it on its counter. Blows on the weak point crit (critMultiplier) only while it is unlocked.
//   A move with needsWatch does either only while the host says the point is watched (setWatched).
// * The end is an answered offer, never zero health: hp never falls below 1. The last phase plays the
//   def's `offer` first and between its moves; countering the offer ends the fight (Ended). An offer
//   with needsAnswer takes its counter only after the host calls answer().
// * A ward (wardLimit above 0): someone the party keeps alive. Each landed move with harmsWard harms
//   it; at the limit the fight is lost (Lost).
//
// Times as CombatInputs.h (an earlier or non-finite time reads as the latest seen). Calls between
// updates queue their events; update(now) plays everything due and returns them all in time order.
// Deterministic; no allocation.

#include "ftcore/CombatInputs.h"
#include "ftcore/WeakPointLock.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace ftcore {

inline constexpr std::size_t kMaxScriptMoves = 5;
inline constexpr std::size_t kMaxScriptPhases = 3;
inline constexpr std::size_t kBossIdCapacity = 16; // bytes, terminator included
inline constexpr std::uint8_t kBossNoMove = 0xFF;
inline constexpr std::uint8_t kBossOffer = 0xFE; // the offer, in events and current()
inline constexpr int kMaxBossResource = 32;

enum class BossCounter : std::uint8_t { // append only
    Guard,
    Dodge,
    Strike,
    StrikeOnCount, // shared
    TakeHalf,      // shared
    Listen,
    GetClear,
    Recover,
    CarryTogether, // shared, two fighters
    Answer,
};
inline constexpr std::uint8_t kBossCounterKinds = 10;

// The one key kind that opens a script's weak point (each a WeakPointLock key).
enum class BossKey : std::uint8_t { Shared, Read, Refusal }; // append only
inline constexpr std::uint8_t kBossKeyKinds = 3;
inline UnlockKey bossUnlockKey(BossKey k) {
    return k == BossKey::Read ? kUnlockReadClause : (k == BossKey::Refusal ? kUnlockRefusal : kUnlockSharedBlow);
}
inline bool sharedCounter(BossCounter c) {
    return c == BossCounter::StrikeOnCount || c == BossCounter::TakeHalf || c == BossCounter::CarryTogether;
}

enum class BossGauge : std::uint8_t { Hp, Resource }; // append only

struct BossMoveDef {
    std::array<char, kBossIdCapacity> idChars{};
    BossCounter counter = BossCounter::Guard;
    float tell = 1.0f;          // seconds, 0.2..5
    float window = 0.5f;        // seconds, 0.1..10
    float recover = 1.0f;       // seconds, 0..5
    float exposeSeconds = 0.0f; // 0..10: countered, the weak point is exposed this long
    bool unlocksOnCounter = false;
    bool harmsWard = false;
    bool needsAnswer = false;        // the counter waits for answer()
    bool needsWatch = false;         // its counter exposes or unlocks only while watched (setWatched)
    std::int8_t resourceOnLand = 0;  // -8..8: the resource changes when it lands
    std::int8_t resourceOnCounter = 0; // -8..8: ... when it is countered

    std::string_view id() const { return std::string_view(idChars.data()); }
    void setId(std::string_view id) {
        idChars.fill('\0');
        const std::size_t n = id.size() < kBossIdCapacity ? id.size() : kBossIdCapacity - 1;
        for (std::size_t i = 0; i < n; ++i) idChars[i] = id[i];
    }
};

struct BossPhaseDef {
    float enterAt = 1.0f; // phase 0: ignored; later: the gauge value at or below which it starts
    std::array<std::uint8_t, kMaxScriptMoves> moves{};
    std::uint8_t moveCount = 0;
};

struct BossScriptDef {
    std::array<char, kBossIdCapacity> idChars{};
    int hp = 1000;              // 1..100000
    BossGauge gauge = BossGauge::Hp;
    int resourceMax = 0;        // Resource gauge: 1..kMaxBossResource
    int resourceStart = 0;      // Resource gauge: 1..resourceMax
    int wardLimit = 0;          // 0 (no ward)..10
    float critMultiplier = 2.0f; // 1..4
    float restSeconds = 1.0f;   // between moves, 0..10
    LockTuning lock{};
    BossKey key = BossKey::Shared; // the one key kind that opens the point; its bit must be in lock.keys
    std::array<BossMoveDef, kMaxScriptMoves> moves{};
    std::uint8_t moveCount = 0;
    BossMoveDef offer{};
    std::array<BossPhaseDef, kMaxScriptPhases> phases{};
    std::uint8_t phaseCount = 0;

    std::string_view id() const { return std::string_view(idChars.data()); }
    void setId(std::string_view id) {
        idChars.fill('\0');
        const std::size_t n = id.size() < kBossIdCapacity ? id.size() : kBossIdCapacity - 1;
        for (std::size_t i = 0; i < n; ++i) idChars[i] = id[i];
    }
    // Index of the move with that id, or kBossNoMove.
    std::uint8_t find(std::string_view id) const {
        for (std::uint8_t i = 0; i < moveCount; ++i)
            if (moves[i].id() == id) return i;
        return kBossNoMove;
    }
};

namespace detail {
inline bool bossIdOk(std::string_view id) {
    if (id.empty() || id.size() >= kBossIdCapacity) return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}
inline bool bossIn(float v, float lo, float hi) { return v >= lo && v <= hi; } // false for NaN
inline bool bossMoveOk(const BossMoveDef& m, float maxWindow) {
    return bossIdOk(m.id()) && static_cast<std::uint8_t>(m.counter) < kBossCounterKinds &&
           bossIn(m.tell, 0.2f, 5.0f) && bossIn(m.window, 0.1f, maxWindow) && bossIn(m.recover, 0.0f, 5.0f) &&
           bossIn(m.exposeSeconds, 0.0f, 10.0f) && m.resourceOnLand >= -8 && m.resourceOnLand <= 8 &&
           m.resourceOnCounter >= -8 && m.resourceOnCounter <= 8;
}
} // namespace detail

// Every range above, plus: unique move ids (the offer's too); each phase's moves known and not
// repeated, 1..kMaxScriptMoves of them (the last phase may have none: it plays the offer alone); later
// phases' enterAt strictly falling (Hp: in (0, 1); Resource: whole numbers in [0, resourceStart)); a
// lock that takes the point's key kind when any move exposes or unlocks it (always, for a kind other
// than Shared); a ward only with a move that
// harms it.
inline bool validBossScript(const BossScriptDef& d) {
    using detail::bossIn;
    if (!detail::bossIdOk(d.id())) return false;
    if (d.hp < 1 || d.hp > 100000 || !bossIn(d.critMultiplier, 1.0f, 4.0f) || !bossIn(d.restSeconds, 0.0f, 10.0f))
        return false;
    if (static_cast<std::uint8_t>(d.gauge) > static_cast<std::uint8_t>(BossGauge::Resource)) return false;
    if (d.gauge == BossGauge::Resource &&
        (d.resourceMax < 1 || d.resourceMax > kMaxBossResource || d.resourceStart < 1 || d.resourceStart > d.resourceMax))
        return false;
    if (d.wardLimit < 0 || d.wardLimit > 10 || !validLockTuning(d.lock)) return false;
    if (d.moveCount < 1 || d.moveCount > kMaxScriptMoves || d.phaseCount < 1 || d.phaseCount > kMaxScriptPhases)
        return false;
    if (static_cast<std::uint8_t>(d.key) >= kBossKeyKinds) return false;
    bool needsKey = d.key != BossKey::Shared;
    bool harms = false;
    for (std::size_t i = 0; i < d.moveCount; ++i) {
        const BossMoveDef& m = d.moves[i];
        if (!detail::bossMoveOk(m, 10.0f) || m.id() == d.offer.id()) return false;
        for (std::size_t j = 0; j < i; ++j)
            if (d.moves[j].id() == m.id()) return false;
        needsKey = needsKey || m.exposeSeconds > 0.0f || m.unlocksOnCounter;
        harms = harms || m.harmsWard;
    }
    if (!detail::bossMoveOk(d.offer, 30.0f) || d.offer.harmsWard) return false;
    needsKey = needsKey || d.offer.exposeSeconds > 0.0f || d.offer.unlocksOnCounter;
    if (needsKey && (d.lock.keys & bossUnlockKey(d.key)) == 0) return false;
    if (d.wardLimit > 0 && !harms) return false;
    for (std::size_t p = 0; p < d.phaseCount; ++p) {
        const BossPhaseDef& ph = d.phases[p];
        const bool last = p + 1 == d.phaseCount;
        if (ph.moveCount > kMaxScriptMoves || (ph.moveCount == 0 && !last)) return false;
        for (std::size_t i = 0; i < ph.moveCount; ++i) {
            if (ph.moves[i] >= d.moveCount) return false;
            for (std::size_t j = 0; j < i; ++j)
                if (ph.moves[j] == ph.moves[i]) return false;
        }
        if (p == 0) continue;
        const float prev = p == 1 ? (d.gauge == BossGauge::Hp ? 1.0f : static_cast<float>(d.resourceStart))
                                  : d.phases[p - 1].enterAt;
        if (!(ph.enterAt < prev)) return false;
        if (d.gauge == BossGauge::Hp && !(ph.enterAt > 0.0f)) return false;
        if (d.gauge == BossGauge::Resource && (!(ph.enterAt >= 0.0f) || ph.enterAt != std::floor(ph.enterAt)))
            return false;
    }
    return true;
}

enum class BossState : std::uint8_t { Rest, Tell, Window, Recover, Ended, Lost }; // append only

enum class BossEventType : std::uint8_t { // append only
    PhaseChanged,    // value = the phase now
    Tell,            // `move` begins its tell; until = when its window opens
    WindowOpened,    // until = when it closes
    Landed,          // the window ran out uncountered: the host lands the move
    Countered,       // value = the BossCounter that answered it
    Recovered,       // the move is over
    Exposed,         // the weak point is exposed until `until`
    ExposeEnded,
    Unlocked,        // the weak point is unlocked until `until` (+inf: until relocked)
    ResourceChanged, // value = the resource now, amount = the change
    WardHarmed,      // value = harms so far
    Crit,            // value = the damage dealt
    Ended,           // the offer was answered: the fight is over
    Lost,            // the ward fell
    Relocked,        // a deferred refusal shut the unlocked point (the lock's relockOnDefer)
};

struct BossEvent {
    BossEventType type = BossEventType::Tell;
    std::uint8_t move = kBossNoMove;
    int value = 0;
    int amount = 0;
    double at = 0.0;
    double until = 0.0;
};

inline constexpr std::size_t kMaxScriptEvents = 32;

struct BossEvents {
    std::array<BossEvent, kMaxScriptEvents> items{};
    std::size_t count = 0;
    std::size_t dropped = 0;

    const BossEvent* begin() const { return items.data(); }
    const BossEvent* end() const { return items.data() + count; }
    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
    const BossEvent& operator[](std::size_t i) const { return items[i]; }
    void clear() { count = dropped = 0; }
    void push(const BossEvent& e) {
        if (count < kMaxScriptEvents) items[count++] = e;
        else ++dropped;
    }
};

enum class BossInput : std::uint8_t { // append only
    Countered,
    Early,        // during the tell
    Late,         // after the window (recovering or resting)
    WrongCounter, // not this move's counter
    NeedsShared,  // a shared counter without the shared input
    NeedsPartner, // CarryTogether: one fighter so far
    NeedsAnswer,  // the offer waits for answer()
    Ignored,      // a bad fighter or kind, or no valid def
    Over,         // the fight has ended or been lost
};

struct ScriptHit {
    int damage = 0;
    bool crit = false;
};

class BossScript {
public:
    // Starts `def` (kept by reference: it must outlive the script) at t, phase 0, resting. False (and
    // inert: every call is ignored) when the def is not valid.
    bool reset(const BossScriptDef& def, double t) {
        def_ = &def;
        valid_ = validBossScript(def);
        now_ = startTime(t);
        clock_ = now_;
        pending_.clear();
        out_.clear();
        state_ = valid_ ? BossState::Rest : BossState::Ended;
        stepAt_ = now_ + (valid_ ? def.restSeconds : 0.0);
        phase_ = 0;
        rot_ = 0;
        current_ = kBossNoMove;
        hp_ = valid_ ? def.hp : 0;
        resource_ = valid_ && def.gauge == BossGauge::Resource ? def.resourceStart : 0;
        ward_ = 0;
        carriers_ = 0;
        answered_ = false;
        watched_ = true;
        exposedOpen_ = false;
        exposedUntil_ = 0.0;
        lock_.reset(valid_, def.lock, now_);
        return valid_;
    }

    BossInput counter(BossCounter kind, FighterId who, double t, bool onCount) {
        if (!valid_ || static_cast<std::uint8_t>(kind) >= kBossCounterKinds || who >= kEnemyFighter)
            return BossInput::Ignored;
        advance(t);
        if (over()) return BossInput::Over;
        if (state_ == BossState::Tell) return BossInput::Early;
        if (state_ != BossState::Window) return BossInput::Late;
        const BossMoveDef& m = move(current_);
        if (kind != m.counter) return BossInput::WrongCounter;
        if (m.needsAnswer && !answered_) return BossInput::NeedsAnswer;
        if (sharedCounter(kind) && !onCount) return BossInput::NeedsShared;
        if (kind == BossCounter::CarryTogether) {
            carriers_ = static_cast<std::uint8_t>(carriers_ | fighterBit(who));
            if (carriers_ == fighterBit(who)) return BossInput::NeedsPartner;
        }
        push(BossEventType::Countered, current_, static_cast<int>(kind), 0, now_);
        if (m.resourceOnCounter != 0) changeResource(m.resourceOnCounter);
        const bool seen = !m.needsWatch || watched_;
        if (m.exposeSeconds > 0.0f && seen) {
            exposedOpen_ = true;
            exposedUntil_ = now_ + m.exposeSeconds;
            pending_.push({BossEventType::Exposed, current_, 0, 0, now_, exposedUntil_});
        }
        if (m.unlocksOnCounter && seen) unlock(def_->lock.clause);
        if (current_ == kBossOffer) {
            state_ = BossState::Ended;
            push(BossEventType::Ended, kBossOffer, 0, 0, now_);
            return BossInput::Countered;
        }
        state_ = BossState::Recover;
        stepAt_ = now_ + m.recover;
        checkPhase();
        return BossInput::Countered;
    }

    // A shared blow landed on the boss (StrikeCount's OnCount, or a hit a lead took half of). True when
    // it unlocked the weak point (or restarted its open time): only with the Shared key kind, while the
    // point is exposed.
    bool sharedBlow(double t) { return LockedPoint::opened(offerKey(kUnlockSharedBlow, t)); }

    // A key offered to the weak point: a read clause (with its id), a refusal, or a shared blow. It opens
    // the point (Unlocked, or Refreshed while open) only when it is the point's key kind (else WrongKey)
    // and a counter has the point exposed (else NotExposed: also after the end); a read clause needs the
    // lock's clause (WrongClause). BadKey: an invalid script, or not one of the three keys.
    UnlockResult offerKey(UnlockKey key, double t, std::uint16_t clause = 0) {
        if (!valid_) return UnlockResult::BadKey;
        advance(t);
        if (key != kUnlockSharedBlow && key != kUnlockReadClause && key != kUnlockRefusal) return UnlockResult::BadKey;
        if (key != bossUnlockKey(def_->key)) return UnlockResult::WrongKey;
        if (over() || !exposedOpen_) return UnlockResult::NotExposed;
        return unlock(clause);
    }

    // A deferred refusal ("not yet" counts as a yes): it shuts an unlocked point when the lock's relockOnDefer says
    // so, with a Relocked event. True when it shut one. After the end, or without a lock, it does nothing.
    bool onDeferred(double t) {
        if (!valid_) return false;
        advance(t);
        if (over() || !lock_.onDeferred(now_)) return false;
        push(BossEventType::Relocked, current_, 0, 0, now_);
        return true;
    }

    // A blow of `amount` (0 or more) on the boss; on the weak point it crits while the point is unlocked.
    // Hp never falls below 1: the fight ends only on the answered offer.
    ScriptHit hit(int amount, bool onWeakPoint, double t) {
        ScriptHit r;
        if (!valid_) return r;
        advance(t);
        if (over() || amount <= 0) return r;
        r.crit = onWeakPoint && lock_.open(now_);
        const double raw = r.crit ? std::round(static_cast<double>(amount) * def_->critMultiplier) : amount;
        const int want = raw > static_cast<double>(hp_) ? hp_ : static_cast<int>(raw);
        const int before = hp_;
        hp_ = before - want < 1 ? 1 : before - want;
        r.damage = before - hp_;
        if (r.crit) push(BossEventType::Crit, current_, r.damage, 0, now_);
        checkPhase();
        return r;
    }

    // The host's go-ahead an offer with needsAnswer waits for. Only in the last phase; true when taken.
    bool answer(double t) {
        if (!valid_) return false;
        advance(t);
        if (over() || phase_ + 1u != def_->phaseCount) return false;
        answered_ = true;
        return true;
    }

    // Whether the weak point is watched (needsWatch moves). True after reset.
    void setWatched(bool watched, double t) {
        if (!valid_) return;
        advance(t);
        watched_ = watched;
    }

    // Adds `n` (negative takes away) to the resource, within 0..resourceMax. Returns the change made.
    int addResource(int n, double t) {
        if (!valid_ || def_->gauge != BossGauge::Resource) return 0;
        advance(t);
        if (over()) return 0;
        const int change = changeResource(n);
        checkPhase();
        return change;
    }

    // Plays everything due by `now` and returns it with every event the calls since the last update made,
    // in time order.
    const BossEvents& update(double now) {
        if (valid_) advance(now);
        out_ = pending_;
        pending_.clear();
        return out_;
    }

    BossState state() const { return state_; }
    bool ended() const { return state_ == BossState::Ended && valid_; }
    bool lost() const { return state_ == BossState::Lost; }
    bool valid() const { return valid_; }
    std::size_t phase() const { return phase_; }
    std::uint8_t current() const { return current_; }
    int hp() const { return hp_; }
    float hpShare() const { return valid_ ? static_cast<float>(hp_) / static_cast<float>(def_->hp) : 0.0f; }
    int resource() const { return resource_; }
    int wardHarm() const { return ward_; }
    bool answered() const { return answered_; }
    bool watched() const { return watched_; }
    bool exposed() const { return exposedOpen_; }
    double exposedUntil() const { return exposedUntil_; }
    bool unlocked() const { return valid_ && lock_.open(now_); }
    const WeakPointLock& lock() const { return lock_.lock(); }
    // The step position. It can lag the latest time seen when one update hits the step cap (the lock
    // and the queries judge at this time too); the next updates catch it up.
    double now() const { return now_; }
    // When the current step ends (the tell, the window, the recovery or the rest).
    double stepEnds() const { return stepAt_; }
    const BossScriptDef& def() const { return *def_; }
    // The move with that index (kBossOffer: the offer); an empty move for anything else.
    const BossMoveDef& move(std::uint8_t m) const {
        static const BossMoveDef kEmptyMove{};
        if (!def_) return kEmptyMove;
        if (m == kBossOffer) return def_->offer;
        return m < def_->moveCount ? def_->moves[m] : kEmptyMove;
    }

private:
    static constexpr double kInf = std::numeric_limits<double>::infinity();
    static constexpr int kMaxBossSteps = 4096;

    bool over() const { return state_ == BossState::Ended || state_ == BossState::Lost; }

    void push(BossEventType type, std::uint8_t mv, int value, int amount, double at) {
        pending_.push({type, mv, value, amount, at, 0.0});
    }

    // Offers the point's own key (a counter with unlocksOnCounter passes the lock's clause).
    UnlockResult unlock(std::uint16_t clause) {
        const UnlockResult u = lock_.offer(bossUnlockKey(def_->key), now_, clause);
        if (LockedPoint::opened(u)) pending_.push({BossEventType::Unlocked, current_, 0, 0, now_, lock_.until()});
        return u;
    }

    int changeResource(int n) {
        if (def_->gauge != BossGauge::Resource) return 0;
        int next = resource_ + n;
        next = next < 0 ? 0 : (next > def_->resourceMax ? def_->resourceMax : next);
        const int change = next - resource_;
        if (change == 0) return 0;
        resource_ = next;
        push(BossEventType::ResourceChanged, current_, resource_, change, now_);
        return change;
    }

    void checkPhase() {
        std::size_t target = phase_;
        for (std::size_t p = phase_ + 1; p < def_->phaseCount; ++p) {
            const float v = def_->gauge == BossGauge::Hp ? hpShare() : static_cast<float>(resource_);
            if (v <= def_->phases[p].enterAt) target = p;
        }
        if (target == phase_) return;
        phase_ = target;
        rot_ = 0;
        push(BossEventType::PhaseChanged, kBossNoMove, static_cast<int>(phase_), 0, now_);
    }

    // The next move of the phase's rotation; the last phase plays the offer first and between rounds.
    std::uint8_t nextMove() {
        const BossPhaseDef& ph = def_->phases[phase_];
        const bool last = phase_ + 1u == def_->phaseCount;
        const std::size_t len = ph.moveCount + (last ? 1u : 0u);
        const std::size_t i = rot_ % len;
        rot_ = (rot_ + 1) % len;
        if (last) return i == 0 ? kBossOffer : ph.moves[i - 1];
        return ph.moves[i];
    }

    void step(double at) {
        now_ = at;
        switch (state_) {
        case BossState::Rest: {
            current_ = nextMove();
            carriers_ = 0;
            state_ = BossState::Tell;
            stepAt_ = at + move(current_).tell;
            pending_.push({BossEventType::Tell, current_, 0, 0, at, stepAt_});
            break;
        }
        case BossState::Tell:
            state_ = BossState::Window;
            stepAt_ = at + move(current_).window;
            pending_.push({BossEventType::WindowOpened, current_, 0, 0, at, stepAt_});
            break;
        case BossState::Window: {
            const BossMoveDef& m = move(current_);
            push(BossEventType::Landed, current_, 0, 0, at);
            if (m.resourceOnLand != 0) changeResource(m.resourceOnLand);
            if (m.harmsWard) {
                ++ward_;
                push(BossEventType::WardHarmed, current_, ward_, 0, at);
                if (def_->wardLimit > 0 && ward_ >= def_->wardLimit) {
                    state_ = BossState::Lost;
                    push(BossEventType::Lost, current_, 0, 0, at);
                    return;
                }
            }
            state_ = BossState::Recover;
            stepAt_ = at + m.recover;
            checkPhase();
            break;
        }
        case BossState::Recover:
            push(BossEventType::Recovered, current_, 0, 0, at);
            state_ = BossState::Rest;
            stepAt_ = at + def_->restSeconds;
            break;
        case BossState::Ended:
        case BossState::Lost:
            break;
        }
    }

    void advance(double t) {
        // clock_ is the latest time seen (the jump rule reads against it); now_ may lag it when a call
        // hits the step bound, and the rest play on the next call.
        clock_ = inputTime(clock_, t);
        t = clock_;
        for (int guard = 0; guard < kMaxBossSteps; ++guard) {
            const double due = over() ? kInf : stepAt_;
            const double expose = exposedOpen_ ? exposedUntil_ : kInf;
            const double first = due < expose ? due : expose;
            if (!(first <= t)) {
                now_ = t;
                return;
            }
            if (expose <= due) {
                now_ = expose;
                exposedOpen_ = false;
                push(BossEventType::ExposeEnded, kBossNoMove, 0, 0, expose);
                continue;
            }
            step(due);
        }
    }

    const BossScriptDef* def_ = nullptr;
    bool valid_ = false;
    double now_ = 0.0;
    double clock_ = 0.0; // the latest time seen (CombatInputs.h); now_ <= clock_
    BossEvents pending_{};
    BossEvents out_{};
    BossState state_ = BossState::Ended;
    double stepAt_ = 0.0;
    std::size_t phase_ = 0;
    std::size_t rot_ = 0;
    std::uint8_t current_ = kBossNoMove;
    int hp_ = 0;
    int resource_ = 0;
    int ward_ = 0;
    std::uint8_t carriers_ = 0;
    bool answered_ = false;
    bool watched_ = true;
    bool exposedOpen_ = false;
    double exposedUntil_ = 0.0;
    LockedPoint lock_{};
};

} // namespace ftcore
