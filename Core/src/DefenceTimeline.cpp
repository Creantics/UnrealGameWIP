#include "ftcore/DefenceTimeline.h"

namespace ftcore {

const char* defenceOutcomeName(DefenceOutcome o) {
    switch (o) {
    case DefenceOutcome::Perfect: return "PERFECT";
    case DefenceOutcome::Dodged: return "DODGE";
    case DefenceOutcome::Jumped: return "JUMP";
    case DefenceOutcome::Hit: return "HIT";
    default: return "";
    }
}

bool sameTimeline(const AttackTimeline& a, const AttackTimeline& b) {
    if (a.count != b.count) return false;
    for (std::size_t i = 0; i < a.count; ++i) {
        const TimelineHit& x = a.hits[i];
        const TimelineHit& y = b.hits[i];
        if (x.at != y.at || x.tell != y.tell || x.telegraph != y.telegraph || x.weight != y.weight ||
            x.jumpable != y.jumpable)
            return false;
    }
    return true;
}

void DefenceTimeline::reset(const AttackTimeline& timeline, double start, const DefenceWindows& windows) {
    timeline_ = timeline;
    if (timeline_.count > kMaxTimelineHits) timeline_.count = static_cast<std::uint8_t>(kMaxTimelineHits);
    windows_ = windows;
    outcomes_.fill(DefenceOutcome::Pending);
    refunds_.fill(false);
    late_.fill(false);
    lockoutUntil_.fill(-1e30);
    start_ = start;
    now_ = start;
    next_ = 0;
    refundCount_ = 0;
    lateCount_ = 0;
    lastLate_ = 0;
    misses_ = 0;
    ignored_ = 0;
}

void DefenceTimeline::clear() {
    AttackTimeline none;
    none.clear();
    reset(none, now_, windows_);
}

void DefenceTimeline::record(std::size_t i, DefenceOutcome o, bool refund, double t, DefenceResults& out) {
    outcomes_[i] = o;
    refunds_[i] = refund;
    refundCount_ += refund ? 1 : 0;
    if (out.count < out.items.size()) {
        DefenceResult& r = out.items[out.count++];
        r.hit = static_cast<std::uint8_t>(i);
        r.outcome = o;
        r.refund = refund;
        r.time = t;
    }
    while (next_ < timeline_.size() && outcomes_[next_] != DefenceOutcome::Pending) ++next_;
}

void DefenceTimeline::expire(double t, DefenceResults& out) {
    if (t > now_) now_ = t;
    while (next_ < timeline_.size() && impact(next_) <= t) record(next_, DefenceOutcome::Hit, false, impact(next_), out);
}

DefenceResults DefenceTimeline::update(double now) {
    DefenceResults out;
    expire(now, out);
    return out;
}

DefenceResults DefenceTimeline::press(DefenceInput input, double t) {
    DefenceResults out;
    expire(t, out);
    if (lockedOut(input, t)) {
        ++ignored_;
        return out;
    }
    if (!complete()) {
        const std::size_t i = next_;
        const double before = impact(i) - t; // > 0: the hit is still pending
        bool answered = false;
        if (accepts(i, input)) {
            switch (input) {
            case DefenceInput::Parry:
                if (before <= windows_.parry) {
                    record(i, DefenceOutcome::Perfect, true, t, out);
                    answered = true;
                }
                break;
            case DefenceInput::Dodge:
                if (before <= windows_.dodge) {
                    record(i, DefenceOutcome::Dodged, before <= windows_.perfectDodge, t, out);
                    answered = true;
                }
                break;
            case DefenceInput::Jump:
                if (before <= windows_.jump) {
                    record(i, DefenceOutcome::Jumped, false, t, out);
                    answered = true;
                }
                break;
            }
        }
        if (answered) return out;
    }
    if (markLate(input, t)) return out;
    // A miss: too early, the wrong input for this hit, or nothing incoming.
    ++misses_;
    const double lockout = input == DefenceInput::Parry  ? windows_.parryLockout
                           : input == DefenceInput::Jump ? windows_.jumpLockout
                                                         : windows_.dodgeLockout;
    if (lockout > 0.0) lockoutUntil_[static_cast<std::size_t>(input)] = t + lockout;
    return out;
}

// Hits resolve in order, so the one that landed last is the one just before next_.
bool DefenceTimeline::markLate(DefenceInput input, double t) {
    if (next_ == 0) return false;
    const std::size_t i = next_ - 1;
    if (outcomes_[i] != DefenceOutcome::Hit || late_[i] || !accepts(i, input)) return false;
    const double after = t - impact(i);
    if (after < 0.0 || after >= windows_.late) return false;
    late_[i] = true;
    ++lateCount_;
    lastLate_ = i;
    return true;
}

bool DefenceTimeline::tellActive(std::size_t i, double t) const {
    if (i >= timeline_.size() || timeline_[i].tell <= 0.0) return false;
    const double at = impact(i);
    return t >= at - timeline_[i].tell && t < at;
}

bool DefenceTimeline::telegraphActive(std::size_t i, double t) const {
    if (i >= timeline_.size() || timeline_[i].telegraph <= 0.0) return false;
    const double at = impact(i);
    return t >= at - timeline_[i].telegraph && t < at;
}

bool DefenceTimeline::accepts(std::size_t i, DefenceInput input) const {
    if (i >= timeline_.size()) return false;
    switch (input) {
    case DefenceInput::Jump: return timeline_[i].jumpable;
    case DefenceInput::Dodge: return !timeline_[i].jumpable;
    default: return true;
    }
}

double DefenceTimeline::idealPress(std::size_t i, DefenceInput input) const {
    const double at = impact(i);
    switch (input) {
    case DefenceInput::Parry: return at - windows_.parry * 0.5;
    case DefenceInput::Dodge: return at - windows_.perfectDodge * 0.5;
    default: return at - windows_.jump * 0.5;
    }
}

std::size_t DefenceTimeline::count(DefenceOutcome o) const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < timeline_.size(); ++i) n += outcomes_[i] == o ? 1u : 0u;
    return n;
}

bool DefenceTimeline::allParried() const {
    return complete() && timeline_.size() > 0 && count(DefenceOutcome::Perfect) == timeline_.size();
}

} // namespace ftcore
