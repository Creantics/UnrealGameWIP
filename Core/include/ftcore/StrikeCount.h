#pragma once

// The count: one fighter calls a short shared strike window ("two, three, now"). Two blows landing
// together inside it both hit far harder; a blow thrown alone does not. See
// docs/SHARED_COMBAT_INPUTS.md section 1.
//
//   Idle --call(caller, t)--> cues on the beat --> window [now - before, now + after] --> Idle
//   the next call waits for readyAt() = window end + cooldown (or the cancel time + cooldown)
//
// * call(caller, t, voice): caller is kLeadA, kLeadB, kThirdSlot, or kNoFighter for a silenced count
//   (no caller: a tempo click only, a narrower window, a smaller boost). Cues land on the beat: cue 0 at
//   t, cue k at t + k beats, the last (index leadBeats, the "now") at t + leadBeats beats. Window edges
//   are included. Refused while a count is live or cooling down, and for the enemy (an enemy only plays
//   false counts).
// * The rule. A blow inside the window cannot know yet whether a partner will join it, so it is Held:
//   judgeBlow returns Held and a blow id, and the host waits for that blow's BlowSettled event before
//   applying its damage (the wait is at most the window's length; the hit reaction can play at once).
//   - When a second fighter lands inside the same window, the window is joined: that blow, every held
//     blow and every later blow in the window are OnCount (x onCountMultiplier, the caller's own
//     included; x silencedMultiplier on a silenced count). The joining and later blows are settled at
//     once in judgeBlow's result; the held ones settle by event.
//   - When the window closes unjoined, the held blows settle: the caller's as Unanswered
//     (x callerAloneMultiplier: a call nobody answered lands light), anyone else's as Alone (x1).
//   - Outside every window a blow is Alone (x1) at once. An enemy's blow is always Alone.
//   - There is no cap on held blows: until the window is joined every blow in it is by one fighter
//     (a second fighter joins it), so the held blows are one run of consecutive blow ids, kept as a
//     first id and a count. Each still gets its own BlowSettled event.
// * falseCall(t, shift): the enemy plays the count with its "now" moved early (shift < 0) or late
//   (shift > 0) off the beat. Its cues are flagged false. A blow inside [falseNow - before,
//   falseNow + after] and in no live true window is FalseCount at once: x falseMultiplier (0: it
//   misses) and punishable (it leaves the fighter open). A new false count replaces the last.
// * cancel(t): drops the live true count. Steps already due still play; its later cues go; a window
//   that had opened gets WindowClosed at t; its held blows settle as unjoined at t; the cooldown runs
//   from t. A count whose window has already closed (by t) is over: cancel leaves it, and its
//   cooldown, as they are. cancelFalse(t) drops the false count the same way (no window events).
// * update(now): the cues, window edges and settled blows that fell due, in time order (on a tie: the
//   true count's step, then a settled blow, then the false count's step). Steps of a count replaced or
//   cancelled before an update still play if they were due. Call order each frame: judge the frame's
//   blows (at their own times), then update(frameEnd) once; a call after update with an earlier time
//   reads as frameEnd.
// * Full buffers. Events wait for update() in a buffer of kMaxCountEvents. When it is full, a
//   BlowSettled event pushes out the earliest event that is not one (cues and window edges are
//   cosmetic; a settle carries damage), and anything pushed out or turned away is counted in the
//   `dropped` of the next CountEvents update() returns. A settle is turned away only when the buffer
//   holds nothing but settles; it is then counted in `droppedSettles` too, and the host applies every
//   blow it still holds back at x1 (the plain value).
//
// Times as CombatInputs.h (queries included): an earlier or non-finite time reads as the latest seen,
// and a forward jump is capped at kMaxTimeStep. No allocation.

#include "ftcore/CombatInputs.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ftcore {

struct CountTuning {
    double bpm = 90.0;                 // 40..240
    int leadBeats = 2;                 // beats from the first cue to the "now": 1..4
    double windowBefore = 0.12;        // seconds before the "now" that still count: (0, one beat)
    double windowAfter = 0.18;         // seconds after it: (0, one beat)
    float onCountMultiplier = 2.5f;    // every blow in a joined window: 1..10
    float silencedMultiplier = 1.6f;   // every blow in a joined silenced window: 1..10
    double silencedWindowScale = 0.6;  // a silenced count's window, times this: (0, 1]
    float callerAloneMultiplier = 0.5f; // the caller's blow in a window nobody joined: 0..1
    float falseMultiplier = 0.0f;      // a blow on a false "now": 0..1 (0: it misses)
    double cooldown = 1.5;             // seconds from the window's end (or a cancel) to the next call: 0..30

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
    if (!(t.callerAloneMultiplier >= 0.0f && t.callerAloneMultiplier <= 1.0f)) return false;
    if (!(t.falseMultiplier >= 0.0f && t.falseMultiplier <= 1.0f)) return false;
    if (!(t.cooldown >= 0.0 && t.cooldown <= 30.0)) return false;
    return true;
}

enum class CountVoice : std::uint8_t { Voice, Click }; // append only
enum class CountOutcome : std::uint8_t { Alone, Held, OnCount, Unanswered, FalseCount }; // append only
enum class CountError : std::uint8_t { None, Live, Cooling, BadCaller }; // append only

struct CountHit {
    CountOutcome outcome = CountOutcome::Alone;
    float multiplier = 1.0f;       // final unless Held (then 1 until its BlowSettled event)
    bool punishable = false;       // FalseCount: the fighter is left open
    FighterId caller = kNoFighter; // the true count's caller (kNoFighter: silenced or none)
    std::uint16_t blow = 0;        // this blow's id (never 0 for a judged fighter's blow)
};

enum class CountEventType : std::uint8_t { Cue, WindowOpened, WindowClosed, BlowSettled }; // append only

struct CountEvent {
    CountEventType type = CountEventType::Cue;
    std::uint8_t index = 0;        // Cue: 0..leadBeats (leadBeats is the "now")
    bool isNow = false;            // Cue: the last one
    bool isFalse = false;          // a false count's cue
    CountVoice voice = CountVoice::Voice;
    FighterId caller = kNoFighter; // the true count's caller; kEnemyFighter for a false count
    double at = 0.0;
    // BlowSettled: the held blow, who threw it and its final outcome and multiplier.
    std::uint16_t blow = 0;
    FighterId striker = kNoFighter;
    CountOutcome outcome = CountOutcome::Alone;
    float multiplier = 1.0f;
};

inline constexpr std::size_t kMaxCountEvents = 64;

struct CountEvents {
    std::array<CountEvent, kMaxCountEvents> items{};
    std::size_t count = 0;
    std::size_t dropped = 0;        // events pushed out or turned away for want of room since the last update
    std::size_t droppedSettles = 0; // of those, BlowSettled events (the host applies its held blows at x1)

    const CountEvent* begin() const { return items.data(); }
    const CountEvent* end() const { return items.data() + count; }
    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
    const CountEvent& operator[](std::size_t i) const { return items[i]; }
    void clear() {
        count = 0;
        dropped = 0;
        droppedSettles = 0;
    }
    // Appends (the caller pushes in time order).
    void push(const CountEvent& e) {
        if (count >= items.size() && !makeRoom(e)) return;
        items[count++] = e;
    }
    // Inserts in time order (after any event at the same time).
    void insert(const CountEvent& e) {
        if (count >= items.size() && !makeRoom(e)) return;
        std::size_t i = count++;
        while (i > 0 && items[i - 1].at > e.at) {
            items[i] = items[i - 1];
            --i;
        }
        items[i] = e;
    }

private:
    // Full: a settle pushes out the earliest event that is not a settle; anything else is turned away.
    bool makeRoom(const CountEvent& e) {
        ++dropped;
        if (e.type == CountEventType::BlowSettled) {
            for (std::size_t i = 0; i < count; ++i) {
                if (items[i].type == CountEventType::BlowSettled) continue;
                for (std::size_t j = i + 1; j < count; ++j) items[j - 1] = items[j];
                --count;
                return true;
            }
            ++droppedSettles;
        }
        return false;
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
        held_ = HeldRun{};
        joined_ = false;
        pending_.clear();
        pendingHead_ = 0;
        nextBlow_ = 1;
        events_.clear();
        return ok;
    }

    // Calls a true count at t. kNoFighter is a silenced count (always a click).
    CountError call(FighterId caller, double t, CountVoice voice = CountVoice::Voice) {
        t = advance(t);
        if (!(caller == kLeadA || caller == kLeadB || caller == kThirdSlot || caller == kNoFighter))
            return CountError::BadCaller;
        if (live()) return CountError::Live;
        if (t < readyAt_) return CountError::Cooling;
        drainDue(true_, t); // the old count's steps that fell due still play
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
        held_ = HeldRun{};
        joined_ = false;
        return CountError::None;
    }

    // The enemy plays a false count at t, its "now" moved by `shift` seconds (negative: early), within
    // one beat either way. A shift that is not finite reads as 0. Replaces the last false count.
    void falseCall(double t, double shift) {
        t = advance(t);
        if (!std::isfinite(shift)) shift = 0.0;
        if (shift > tuning_.beat()) shift = tuning_.beat();
        drainDue(false_, t);
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
        // Nothing live, or a count whose window closed before t (over, though update() has not yet
        // played its end): nothing to drop, and its cooldown stands.
        if (!true_.active || t > true_.hi) return;
        drainDue(true_, t);
        // A window that had opened closes now, so a host that opened on WindowOpened can close.
        const int last = tuning_.leadBeats;
        if (true_.step > last && true_.step <= last + 2) {
            CountEvent e = stepEvent(true_);
            e.type = CountEventType::WindowClosed;
            e.isNow = false;
            e.index = 0;
            e.at = t;
            pushPending(e);
        }
        settleLone(t);
        true_.active = false;
        readyAt_ = t + tuning_.cooldown;
    }
    void cancelFalse(double t) {
        t = advance(t);
        drainDue(false_, t);
        false_.active = false;
    }

    // Judges a blow landing at t (before its damage is applied). See the rule above.
    CountHit judgeBlow(FighterId striker, double t) {
        t = advance(t);
        CountHit hit;
        hit.caller = true_.active ? true_.caller : kNoFighter;
        if (!(striker == kLeadA || striker == kLeadB || striker == kThirdSlot)) return hit;
        hit.blow = takeBlowId();
        if (true_.active && t >= true_.lo && t <= true_.hi) {
            if (!joined_ && held_.count > 0 && held_.striker != striker) {
                joined_ = true;
                settleHeld(CountOutcome::OnCount, boost(), t); // the held blows were joined at t
            }
            if (joined_) {
                hit.outcome = CountOutcome::OnCount;
                hit.multiplier = boost();
                return hit;
            }
            // Unjoined, so every blow held so far is this fighter's, and their ids run on without a
            // gap up to this one (each judged blow takes the next id).
            if (held_.count == 0) {
                held_.striker = striker;
                held_.first = hit.blow;
            }
            ++held_.count;
            hit.outcome = CountOutcome::Held;
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
            const double ts = pendingHead_ < pending_.size() ? pending_[pendingHead_].at : kInf;
            const double tf = false_.active ? nextTime(false_) : kInf;
            if (tt <= now && tt <= ts && tt <= tf) {
                events_.push(stepEvent(true_));
            } else if (ts <= now && ts <= tf) {
                events_.push(pending_[pendingHead_++]);
            } else if (tf <= now) {
                events_.push(stepEvent(false_));
            } else {
                break;
            }
        }
        // What the waiting buffer lost since the last update reaches the host here.
        events_.dropped += pending_.dropped;
        events_.droppedSettles += pending_.droppedSettles;
        pending_.clear();
        pendingHead_ = 0;
        // A run is over once everything of it has played and its window has passed.
        if (true_.active && true_.step > static_cast<int>(last) + 2 && now > true_.hi) true_.active = false;
        if (false_.active && false_.step > static_cast<int>(last) && now > false_.hi) false_.active = false;
        return events_;
    }

    // A true count is called and its window has not closed (by the latest time seen).
    bool live() const { return true_.active && now_ <= true_.hi; }
    // t inside the true window (t read as CombatInputs.h against the latest time seen, without moving it).
    bool open(double t) const {
        const double c = inputTime(now_, t);
        return true_.active && c >= true_.lo && c <= true_.hi;
    }
    bool joined() const { return joined_; }
    std::size_t heldBlows() const { return held_.count; }
    bool falseLive() const { return false_.active && now_ <= false_.hi; }
    FighterId caller() const { return true_.caller; }
    bool silenced() const { return true_.active && true_.caller == kNoFighter; }
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

    // The held blows of an unjoined window: one fighter's, ids first, first + 1, ... (skipping 0).
    struct HeldRun {
        FighterId striker = kNoFighter;
        std::uint16_t first = 0;
        std::size_t count = 0;
    };

    double advance(double t) {
        now_ = inputTime(now_, t);
        // The window closed unjoined: its held blows settle at its end.
        if (held_.count > 0 && true_.active && now_ > true_.hi) {
            drainDue(true_, true_.hi); // its steps first, so WindowClosed comes before the settles
            settleLone(true_.hi);
        }
        return now_;
    }

    static std::uint16_t nextId(std::uint16_t id) { return static_cast<std::uint16_t>(id == 0xFFFF ? 1 : id + 1); }
    std::uint16_t takeBlowId() {
        const std::uint16_t id = nextBlow_;
        nextBlow_ = nextId(nextBlow_);
        return id;
    }

    float boost() const { return true_.caller == kNoFighter ? tuning_.silencedMultiplier : tuning_.onCountMultiplier; }
    CountOutcome loneOutcome(FighterId striker) const {
        return striker == true_.caller ? CountOutcome::Unanswered : CountOutcome::Alone;
    }
    float loneMultiplier(FighterId striker) const {
        return striker == true_.caller ? tuning_.callerAloneMultiplier : 1.0f;
    }

    // The held blows settle unjoined: the caller's light, anyone else's plain.
    void settleLone(double at) {
        settleHeld(loneOutcome(held_.striker), loneMultiplier(held_.striker), at);
    }

    // One BlowSettled event per held blow, then none held.
    void settleHeld(CountOutcome outcome, float multiplier, double at) {
        CountEvent e;
        e.type = CountEventType::BlowSettled;
        e.caller = true_.caller;
        e.voice = true_.voice;
        e.at = at;
        e.striker = held_.striker;
        e.outcome = outcome;
        e.multiplier = multiplier;
        std::uint16_t id = held_.first;
        for (std::size_t i = 0; i < held_.count; ++i, id = nextId(id)) {
            e.blow = id;
            pushPending(e);
        }
        held_ = HeldRun{};
    }

    // Keeps pending_ in time order (a later push on a tie goes after).
    void pushPending(const CountEvent& e) { pending_.insert(e); }

    // Moves the run's steps due by t into pending_ (before the run is replaced or dropped).
    void drainDue(Run& r, double t) {
        if (!r.active) return;
        while (nextTime(r) <= t) pushPending(stepEvent(r));
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

    // The run's next step as an event; advances the run.
    CountEvent stepEvent(Run& r) {
        const std::uint8_t last = static_cast<std::uint8_t>(tuning_.leadBeats);
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
        ++r.step;
        return e;
    }

    CountTuning tuning_{};
    double now_ = 0.0;
    Run true_{};
    Run false_{};
    double readyAt_ = -kInf;
    HeldRun held_{};
    bool joined_ = false;
    CountEvents pending_{}; // settled blows and drained steps waiting for the next update, in time order
    std::size_t pendingHead_ = 0;
    std::uint16_t nextBlow_ = 1;
    CountEvents events_{};
};

// True when a settled outcome is a shared blow (a joined window).
inline bool sharedOutcome(CountOutcome o) { return o == CountOutcome::OnCount; }

} // namespace ftcore
