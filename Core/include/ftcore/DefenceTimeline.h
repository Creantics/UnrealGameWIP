#pragma once

// Reactive defence on enemy turns, driven by a fixed per-attack timeline.
//
// Every enemy skill carries an AttackTimeline: a banner at t = 0 (the skill name caption), then one
// or more hits at fixed offsets from it. Each hit has a tell cue `tell` seconds before impact (the
// HUD flash and, later, a tell sound) and optionally a long world-space telegraph `telegraph`
// seconds before impact for slow, big attacks. The same attack always replays the same beat, so
// learning a fight means learning its offsets.
//
// The defender answers each hit with one of three inputs, judged against a window that ends at the
// impact (the impact itself is already too late, so windows are [impact - width, impact)):
//   Dodge  wide window. Safe; no lockout, so mashing it is tolerated. A dodge in the last
//          `perfectDodge` seconds is a perfect dodge and refunds an action point. Can't avoid a
//          jumpable hit (a low sweep or a shockwave).
//   Parry  narrow window. Always perfect when it lands and refunds an action point; a whole attack
//          parried enables a counter. A parry press that misses locks parry out for `parryLockout`.
//   Jump   only on hits flagged jumpable; no refund. A jump that misses locks jump out for a moment.
// A hit nobody answered in time lands (outcome Hit).
//
// Rules:
//  - A press first resolves every pending hit whose impact is at or before the press time as Hit,
//    then answers the earliest pending hit ("each press answers the next hit").
//  - Presses and update() times must be fed in time order (the same clock: battle seconds).
//  - Nothing here allocates: hits live in fixed-capacity arrays and results come back by value.
//
//   ftcore::DefenceTimeline d;
//   d.reset(skill.timeline, bannerTime);
//   for (const DefenceResult& r : d.update(now)) ...;          // hits that landed unanswered
//   for (const DefenceResult& r : d.press(DefenceInput::Parry, inputTime)) ...;
//
// Tests and the autopilot drive it with an explicit clock: update(now) or advance(dt), and
// idealPress() gives the press time that is perfect for a hit (the autopilot never listens to audio).

#include "ftcore/Api.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ftcore {

enum class DefenceInput : std::uint8_t { Dodge, Parry, Jump };
inline constexpr std::size_t kDefenceInputs = 3;

enum class DefenceOutcome : std::uint8_t { Pending, Perfect, Dodged, Jumped, Hit };
// "PERFECT", "DODGE", "JUMP", "HIT" (or "" for Pending). A literal: valid forever.
FTCORE_API const char* defenceOutcomeName(DefenceOutcome o);

inline constexpr std::size_t kMaxTimelineHits = 8;
// A skill without timeline data: one hit kDefaultImpact seconds after its banner, its tell
// kDefaultTell seconds before that.
inline constexpr double kDefaultImpact = 1.2;
inline constexpr double kDefaultTell = 0.4;
// Data limits (the loader refuses anything outside them).
inline constexpr double kMaxHitOffset = 30.0;
inline constexpr double kMaxTell = 2.0;
inline constexpr double kMaxTelegraph = 10.0;

struct TimelineHit {
    double at = kDefaultImpact; // seconds after the banner
    double tell = kDefaultTell; // tell cue this long before impact (0: none)
    double telegraph = 0.0;     // world-space marker on the target this long before impact (0: none)
    float weight = 1.0f;        // share of the skill's damage this hit carries (relative to the others)
    bool jumpable = false;      // Jump answers it; Dodge doesn't
};

// The hits of one attack, sorted by `at`. Default-constructed it is the single default hit.
struct AttackTimeline {
    std::array<TimelineHit, kMaxTimelineHits> hits{};
    std::uint8_t count = 1;

    std::size_t size() const { return count; }
    const TimelineHit& operator[](std::size_t i) const { return hits[i]; }
    void clear() { count = 0; }
    // Appends a hit; false (and nothing added) when full.
    bool add(const TimelineHit& h) {
        if (count >= kMaxTimelineHits) return false;
        hits[count++] = h;
        return true;
    }
    // Offset of the last impact (0 when empty).
    double length() const { return count == 0 ? 0.0 : hits[count - 1u].at; }
    float totalWeight() const {
        float w = 0.0f;
        for (std::size_t i = 0; i < count; ++i) w += hits[i].weight;
        return w;
    }
};

FTCORE_API bool sameTimeline(const AttackTimeline& a, const AttackTimeline& b);

// Window widths in seconds, each ending at the impact.
struct DefenceWindows {
    double dodge = 0.30;
    double perfectDodge = 0.06; // the last part of the dodge window: a perfect dodge
    double parry = 0.12;
    double jump = 0.25;
    double dodgeLockout = 0.0;  // dodge tolerates mashing
    double parryLockout = 0.40; // after a parry press that misses
    double jumpLockout = 0.30;  // after a jump press that misses
};

struct DefenceResult {
    std::uint8_t hit = 0; // index into the timeline
    DefenceOutcome outcome = DefenceOutcome::Pending;
    bool refund = false;  // a perfect parry or a perfect dodge: refunds an action point
    double time = 0.0;    // press time, or the impact for a hit that landed
};

// Hits resolved by one call, in timeline order. Fixed capacity: no allocation.
struct DefenceResults {
    std::array<DefenceResult, kMaxTimelineHits> items{};
    std::size_t count = 0;

    const DefenceResult* begin() const { return items.data(); }
    const DefenceResult* end() const { return items.data() + count; }
    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
    const DefenceResult& operator[](std::size_t i) const { return items[i]; }
};

class DefenceTimeline {
public:
    DefenceTimeline() { timeline_.clear(); } // no attack until reset()

    // Starts `timeline` with its banner at `start` (also the clock's time). An empty timeline is
    // complete at once.
    FTCORE_API void reset(const AttackTimeline& timeline, double start, const DefenceWindows& windows = {});
    // No attack: empty and complete.
    FTCORE_API void clear();

    // Moves the clock to `now`: hits whose impact is at or before it land (outcome Hit).
    FTCORE_API DefenceResults update(double now);
    // update(now() + dt).
    DefenceResults advance(double dt) { return update(now_ + dt); }
    // A press at time t (moves the clock there first). Returns every hit it resolved: the ones that
    // landed before t, then the one it answered, if any. A press that answers nothing is a miss
    // (lockout for parry and jump); a press during its input's lockout is ignored.
    FTCORE_API DefenceResults press(DefenceInput input, double t);
    DefenceResults press(DefenceInput input) { return press(input, now_); }

    double now() const { return now_; }
    double start() const { return start_; }
    std::size_t size() const { return timeline_.size(); }
    const AttackTimeline& timeline() const { return timeline_; }
    const DefenceWindows& windows() const { return windows_; }
    const TimelineHit& hit(std::size_t i) const { return timeline_[i]; }
    double impact(std::size_t i) const { return start_ + timeline_[i].at; }
    DefenceOutcome outcome(std::size_t i) const { return outcomes_[i]; }
    bool refunded(std::size_t i) const { return refunds_[i]; }
    // Earliest pending hit (size() when none).
    std::size_t nextIndex() const { return next_; }
    bool complete() const { return next_ >= timeline_.size(); }

    // The input's lockout covers t: presses of it are ignored.
    bool lockedOut(DefenceInput input, double t) const { return t < lockoutUntil_[static_cast<std::size_t>(input)]; }
    // t is inside hit i's tell (between the tell cue and the impact) / its telegraph.
    FTCORE_API bool tellActive(std::size_t i, double t) const;
    FTCORE_API bool telegraphActive(std::size_t i, double t) const;
    // Whether `input` can answer hit i at all (Jump only on jumpable hits, Dodge only on the others).
    FTCORE_API bool accepts(std::size_t i, DefenceInput input) const;
    // A press time for hit i that gets the best result `input` can: the middle of the parry
    // window, of the perfect-dodge part of the dodge window, or of the jump window.
    FTCORE_API double idealPress(std::size_t i, DefenceInput input) const;

    FTCORE_API std::size_t count(DefenceOutcome o) const;
    int refunds() const { return refundCount_; }
    int misses() const { return misses_; }   // presses that answered nothing (lockouts started)
    int ignored() const { return ignored_; } // presses dropped by a lockout
    // Every hit parried (complete, at least one hit): the defender may counter.
    FTCORE_API bool allParried() const;

private:
    void expire(double t, DefenceResults& out);
    void record(std::size_t i, DefenceOutcome o, bool refund, double t, DefenceResults& out);

    AttackTimeline timeline_{};
    DefenceWindows windows_{};
    std::array<DefenceOutcome, kMaxTimelineHits> outcomes_{};
    std::array<bool, kMaxTimelineHits> refunds_{};
    std::array<double, kDefenceInputs> lockoutUntil_{-1e30, -1e30, -1e30};
    double start_ = 0.0;
    double now_ = 0.0;
    std::size_t next_ = 0;
    int refundCount_ = 0;
    int misses_ = 0;
    int ignored_ = 0;
};

} // namespace ftcore
