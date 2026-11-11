#pragma once

// The count: one fighter calls a short shared strike window ("two, three, now"). A partner's blow that
// lands inside it hits far harder; the caller's own blow, and a blow thrown alone, do not. See
// docs/SHARED_COMBAT_INPUTS.md section 1.
//
//   Idle --call(caller, t)--> Counting --(Now - before)--> Open --(Now + after)--> Cooldown --> Idle
//
// * call(caller, t, voice): caller is kLeadA, kLeadB, kThirdSlot, or kNoFighter for a silenced count
//   (no caller: a tempo click only). Cues land on the beat: cue 0 at t, cue k at t + k beats, the last
//   (index leadBeats, the "now") at t + leadBeats beats. The window is [now - before, now + after],
//   edges included. Refused while a count is live or cooling down, and for the enemy (an enemy only
//   plays false counts).
// * judgeBlow(striker, t): a lead's or the third slot's blow. In the window: the caller's own blow is
//   CallersOwn (x1); each other fighter's first perFighterBoosts blows are OnCount (x onCountMultiplier;
//   a silenced count: x silencedMultiplier on a window scaled by silencedWindowScale); later ones Spent
//   (x1). Outside every window: Alone (x1). An enemy's blow is always Alone.
// * falseCall(t, shift): the enemy plays the count with its "now" moved early (shift < 0) or late
//   (shift > 0) off the beat. Its cues are flagged false. A blow inside [falseNow - before,
//   falseNow + after] and in no live true window is FalseCount: x falseMultiplier and punishable. A new
//   false count replaces the last (its cues not yet played are dropped).
// * cancel(t): drops the live true count (its pending cues and window go, no event); the cooldown runs
//   from t. cancelFalse(t) drops the false count.
// * update(now): the cues and window edges that fell due, in time order (true before false on a tie).
//   Call it every frame: a count replaced (a new call after the cooldown, a new false count) before an
//   update has played its last steps drops them.
//
// Times as CombatInputs.h: an earlier or non-finite time reads as the latest seen. No allocation.

#include "ftcore/CombatInputs.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ftcore {

struct CountTuning {
    double bpm = 90.0;                // 40..240
    int leadBeats = 2;                // beats from the first cue to the "now": 1..4
    double windowBefore = 0.12;       // seconds before the "now" that still count: (0, one beat)
    double windowAfter = 0.18;        // seconds after it: (0, one beat)
    float onCountMultiplier = 2.5f;   // a partner's blow on a called count
    float silencedMultiplier = 1.6f;  // any blow on a silenced count
    double silencedWindowScale = 0.6; // a silenced count's window, times this: (0, 1]
    float falseMultiplier = 0.75f;    // a blow on a false count
    int perFighterBoosts = 1;         // boosted blows per fighter per count: 1..4
    double cooldown = 1.5;            // seconds from the window's end (or a cancel) to the next call: 0..30

    double beat() const { return 60.0 / bpm; }
};

inline bool validCountTuning(const CountTuning& t) {
    if (!(t.bpm >= 40.0 && t.bpm <= 240.0)) return false;
    if (t.leadBeats < 1 || t.leadBeats > 4) return false;
    const double b = t.beat();
    if (!(t.windowBefore > 0.0 && t.windowBefore < b)) return false;
    if (!(t.windowAfter > 0.0 && t.windowAfter < b)) return false;
    if (!(t.onCountMultiplier >= 1.0f && t.onCountMultiplier <= 10.0f)) return false;
    if (!(t.silencedMultiplier >= 1.0f && t.silencedMultiplier <= 10.0f)) return false;
    if (!(t.silencedWindowScale > 0.0 && t.silencedWindowScale <= 1.0)) return false;
    if (!(t.falseMultiplier >= 0.0f && t.falseMultiplier <= 1.0f)) return false;
    if (t.perFighterBoosts < 1 || t.perFighterBoosts > 4) return false;
    if (!(t.cooldown >= 0.0 && t.cooldown <= 30.0)) return false;
    return true;
}

enum class CountVoice : std::uint8_t { Voice, Click }; // append only
enum class CountOutcome : std::uint8_t { Alone, OnCount, CallersOwn, Spent, FalseCount }; // append only
enum class CountError : std::uint8_t { None, Live, Cooling, BadCaller }; // append only

struct CountHit {
    CountOutcome outcome = CountOutcome::Alone;
    float multiplier = 1.0f;
    bool punishable = false;       // FalseCount: the boss may answer it
    FighterId caller = kNoFighter; // the count's caller (kNoFighter: silenced or none)
};

enum class CountEventType : std::uint8_t { Cue, WindowOpened, WindowClosed }; // append only

struct CountEvent {
    CountEventType type = CountEventType::Cue;
    std::uint8_t index = 0;        // Cue: 0..leadBeats (leadBeats is the "now")
    bool isNow = false;            // Cue: the last one
    bool isFalse = false;          // a false count's cue
    CountVoice voice = CountVoice::Voice;
    FighterId caller = kNoFighter; // the true count's caller; kEnemyFighter for a false count
    double at = 0.0;
};

inline constexpr std::size_t kMaxCountEvents = 16;

struct CountEvents {
    std::array<CountEvent, kMaxCountEvents> items{};
    std::size_t count = 0;
    std::size_t dropped = 0;

    const CountEvent* begin() const { return items.data(); }
    const CountEvent* end() const { return items.data() + count; }
    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
    const CountEvent& operator[](std::size_t i) const { return items[i]; }
    void clear() { count = 0; dropped = 0; }
    void push(const CountEvent& e) {
        if (count < items.size()) items[count++] = e;
        else ++dropped;
    }
};

class StrikeCount {
public:
    StrikeCount() { reset(); }

    // Starts over with `tuning` (the defaults when it is not valid: returns false then).
    bool reset(const CountTuning& tuning = {}) {
        const bool ok = validCountTuning(tuning);
        tuning_ = ok ? tuning : CountTuning{};
        now_ = 0.0;
        true_ = Run{};
        false_ = Run{};
        readyAt_ = -kInf;
        boosts_.fill(0);
        return ok;
    }

    // Calls a true count at t. kNoFighter is a silenced count (always a click).
    CountError call(FighterId caller, double t, CountVoice voice = CountVoice::Voice) {
        t = advance(t);
        if (!(caller == kLeadA || caller == kLeadB || caller == kThirdSlot || caller == kNoFighter))
            return CountError::BadCaller;
        if (live()) return CountError::Live;
        if (t < readyAt_) return CountError::Cooling;
        const bool silenced = caller == kNoFighter;
        const double scale = silenced ? tuning_.silencedWindowScale : 1.0;
        true_ = Run{};
        true_.active = true;
        true_.start = t;
        true_.nowAt = t + tuning_.beat() * static_cast<double>(tuning_.leadBeats);
        true_.lo = true_.nowAt - tuning_.windowBefore * scale;
        true_.hi = true_.nowAt + tuning_.windowAfter * scale;
        true_.caller = caller;
        true_.voice = silenced ? CountVoice::Click : voice;
        readyAt_ = true_.hi + tuning_.cooldown;
        boosts_.fill(0);
        return CountError::None;
    }

    // The enemy plays a false count at t, its "now" moved by `shift` seconds (negative: early). A shift
    // that is not finite reads as 0. Replaces the last false count.
    void falseCall(double t, double shift) {
        t = advance(t);
        if (!std::isfinite(shift)) shift = 0.0;
        false_ = Run{};
        false_.active = true;
        false_.start = t;
        false_.nowAt = t + tuning_.beat() * static_cast<double>(tuning_.leadBeats) + shift;
        // Never before the cue ahead of it (the cues stay in order).
        const double prev = t + tuning_.beat() * static_cast<double>(tuning_.leadBeats - 1);
        if (false_.nowAt < prev) false_.nowAt = prev;
        false_.lo = false_.nowAt - tuning_.windowBefore;
        false_.hi = false_.nowAt + tuning_.windowAfter;
        false_.caller = kEnemyFighter;
        false_.voice = CountVoice::Voice;
        false_.isFalse = true;
    }

    void cancel(double t) {
        t = advance(t);
        if (!true_.active) return;
        true_.active = false;
        readyAt_ = t + tuning_.cooldown;
    }
    void cancelFalse(double t) {
        advance(t);
        false_.active = false;
    }

    // Judges a blow landing at t (before its damage is applied).
    CountHit judgeBlow(FighterId striker, double t) {
        t = advance(t);
        CountHit hit;
        hit.caller = true_.active ? true_.caller : kNoFighter;
        if (!(striker == kLeadA || striker == kLeadB || striker == kThirdSlot)) return hit;
        if (true_.active && t >= true_.lo && t <= true_.hi) {
            if (striker == true_.caller) {
                hit.outcome = CountOutcome::CallersOwn;
                return hit;
            }
            if (boosts_[striker] >= tuning_.perFighterBoosts) {
                hit.outcome = CountOutcome::Spent;
                return hit;
            }
            ++boosts_[striker];
            hit.outcome = CountOutcome::OnCount;
            hit.multiplier = true_.caller == kNoFighter ? tuning_.silencedMultiplier : tuning_.onCountMultiplier;
            return hit;
        }
        if (false_.active && t >= false_.lo && t <= false_.hi) {
            hit.outcome = CountOutcome::FalseCount;
            hit.multiplier = tuning_.falseMultiplier;
            hit.punishable = true;
        }
        return hit;
    }

    // What fell due by `now`.
    const CountEvents& update(double now) {
        now = advance(now);
        events_.clear();
        const std::uint8_t last = static_cast<std::uint8_t>(tuning_.leadBeats);
        for (;;) {
            const double tt = true_.active ? nextTime(true_) : kInf;
            const double tf = false_.active ? nextTime(false_) : kInf;
            Run* r = nullptr;
            if (tt <= now && tt <= tf) r = &true_;
            else if (tf <= now) r = &false_;
            if (r == nullptr) break;
            emit(*r, last);
        }
        // A run is over once everything of it has played and its window has passed.
        if (true_.active && true_.step > static_cast<int>(last) + 2 && now > true_.hi) true_.active = false;
        if (false_.active && false_.step > static_cast<int>(last) && now > false_.hi) false_.active = false;
        return events_;
    }

    // A true count is called and its window has not closed (by the latest time seen).
    bool live() const { return true_.active && now_ <= true_.hi; }
    bool open(double t) const { return true_.active && t >= true_.lo && t <= true_.hi; }
    bool falseLive() const { return false_.active && now_ <= false_.hi; }
    FighterId caller() const { return true_.caller; }
    bool silenced() const { return true_.caller == kNoFighter; }
    double nowAt() const { return true_.nowAt; }
    double windowStart() const { return true_.lo; }
    double windowEnd() const { return true_.hi; }
    double falseNowAt() const { return false_.nowAt; }
    double readyAt() const { return readyAt_; }
    double now() const { return now_; }
    const CountTuning& tuning() const { return tuning_; }

private:
    static constexpr double kInf = std::numeric_limits<double>::infinity();

    struct Run {
        bool active = false;
        bool isFalse = false;
        double start = 0.0;
        double nowAt = 0.0;
        double lo = 0.0;
        double hi = 0.0;
        FighterId caller = kNoFighter;
        CountVoice voice = CountVoice::Voice;
        // The next thing to play. A true count: cues 0..last-1, WindowOpened, the "now" cue,
        // WindowClosed (steps 0..last+2). A false count: cues 0..last (steps 0..last).
        int step = 0;
    };

    double advance(double t) {
        now_ = inputTime(now_, t);
        return now_;
    }

    // The time of the run's next step (inf when it has played everything).
    double nextTime(const Run& r) const {
        const int last = tuning_.leadBeats;
        const double beat = tuning_.beat();
        if (r.isFalse) {
            if (r.step > last) return kInf;
            return r.step < last ? r.start + beat * r.step : r.nowAt;
        }
        if (r.step < last) return r.start + beat * r.step;
        if (r.step == last) return r.lo;
        if (r.step == last + 1) return r.nowAt;
        if (r.step == last + 2) return r.hi;
        return kInf;
    }

    void emit(Run& r, std::uint8_t last) {
        CountEvent e;
        e.at = nextTime(r);
        e.isFalse = r.isFalse;
        e.voice = r.voice;
        e.caller = r.caller;
        const int s = r.step;
        if (r.isFalse) {
            e.type = CountEventType::Cue;
            e.index = static_cast<std::uint8_t>(s);
            e.isNow = s == last;
        } else if (s < last) {
            e.type = CountEventType::Cue;
            e.index = static_cast<std::uint8_t>(s);
        } else if (s == last) {
            e.type = CountEventType::WindowOpened;
        } else if (s == last + 1) {
            e.type = CountEventType::Cue;
            e.index = last;
            e.isNow = true;
        } else {
            e.type = CountEventType::WindowClosed;
        }
        events_.push(e);
        ++r.step;
    }

    CountTuning tuning_{};
    double now_ = 0.0;
    Run true_{};
    Run false_{};
    double readyAt_ = -kInf;
    std::array<int, kMaxFighters> boosts_{};
    CountEvents events_{};
};

} // namespace ftcore
