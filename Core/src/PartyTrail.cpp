#include "ftcore/PartyTrail.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

float distance(Vec2 a, Vec2 b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}

Vec2 lerp(Vec2 a, Vec2 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

} // namespace

void PartyTrail::add(Vec2 p, float speed) {
    newest_ = (newest_ + 1) % kCapacity;
    ring_[newest_] = p;
    speed_[newest_] = speed;
    if (count_ < kCapacity) ++count_;
    ++laid_;
}

void PartyTrail::reset(Vec2 lead, Vec2 facing, float seedLength) {
    const float len = std::sqrt(facing.x * facing.x + facing.y * facing.y);
    const Vec2 dir = len > 1e-6f ? Vec2{facing.x / len, facing.y / len} : Vec2{1.f, 0.f};
    std::size_t n = seedLength > 0.f ? static_cast<std::size_t>(std::ceil(seedLength / kSampleSpacing)) + 1u : 1u;
    if (n > kCapacity) n = kCapacity;
    count_ = 0;
    newest_ = kCapacity - 1u;
    for (std::size_t k = n; k-- > 0;) {
        const float back = static_cast<float>(k) * kSampleSpacing;
        add({lead.x - dir.x * back, lead.y - dir.y * back}, -1.f);
    }
    lead_ = lead;
    leadSpeed_ = -1.f;
    turnBase_ = turnCount_;
    jumpBase_ = jumpCount_;
}

void PartyTrail::push(Vec2 lead, float speed) {
    if (count_ == 0) {
        reset(lead, {}, 0.f);
        leadSpeed_ = speed;
        return;
    }
    Vec2 from = sample(0);
    float d = distance(from, lead);
    if (d > static_cast<float>(kCapacity) * kSampleSpacing) {
        reset(lead, {lead.x - from.x, lead.y - from.y}, 0.f);
        leadSpeed_ = speed;
        return;
    }
    // Lay samples at even spacing along the move, so a long frame still leaves a usable path.
    while (d >= kSampleSpacing) {
        const Vec2 next = lerp(from, lead, kSampleSpacing / d);
        add(next, speed);
        from = next;
        d -= kSampleSpacing;
    }
    lead_ = lead;
    leadSpeed_ = speed;
}

Vec2 PartyTrail::pointBehind(float arc) const {
    if (arc <= 0.f || count_ == 0) return lead_;
    Vec2 a = lead_;
    for (std::size_t k = 0; k < count_; ++k) {
        const Vec2 b = sample(k);
        const float seg = distance(a, b);
        if (seg >= arc && seg > 0.f) return lerp(a, b, arc / seg);
        arc -= seg;
        a = b;
    }
    return a;
}

float PartyTrail::odometer() const {
    if (count_ == 0) return 0.f;
    return static_cast<float>(laid_) * kSampleSpacing + distance(sample(0), lead_);
}

void PartyTrail::markTurn(float delta) {
    turns_[turnCount_ % kTurns] = {odometer(), delta};
    ++turnCount_;
}

float PartyTrail::turnsReached(float arc, std::uint32_t& next) const {
    if (next < turnBase_) next = turnBase_;
    if (turnCount_ - next > kTurns) next = turnCount_ - static_cast<std::uint32_t>(kTurns);
    const float at = odometer() - arc;
    float delta = 0.f;
    while (next < turnCount_ && turns_[next % kTurns].at <= at) {
        delta += turns_[next % kTurns].delta;
        ++next;
    }
    return delta;
}

void PartyTrail::markJump() {
    jumps_[jumpCount_ % kJumps] = odometer();
    ++jumpCount_;
}

int PartyTrail::jumpsReached(float arc, std::uint32_t& next) const {
    if (next < jumpBase_) next = jumpBase_;
    if (jumpCount_ - next > kJumps) next = jumpCount_ - static_cast<std::uint32_t>(kJumps);
    const float at = odometer() - arc;
    int n = 0;
    while (next < jumpCount_ && jumps_[next % kJumps] <= at) {
        ++n;
        ++next;
    }
    return n;
}

float PartyTrail::speedBehind(float arc) const {
    if (arc <= 0.f || count_ == 0) return leadSpeed_;
    // Between the lead and the newest sample the lead's own speed holds; past that, the speed the
    // sample was laid at, unknown (below 0) on either side wins.
    Vec2 a = lead_;
    float va = leadSpeed_;
    for (std::size_t k = 0; k < count_; ++k) {
        const Vec2 b = sample(k);
        const float vb = sampleSpeed(k);
        const float seg = distance(a, b);
        if (seg >= arc && seg > 0.f) {
            if (va < 0.f || vb < 0.f) return -1.f;
            return va + (vb - va) * (arc / seg);
        }
        arc -= seg;
        a = b;
        va = vb;
    }
    return va;
}

float PartyTrail::length() const {
    float total = 0.f;
    Vec2 a = lead_;
    for (std::size_t k = 0; k < count_; ++k) {
        const Vec2 b = sample(k);
        total += distance(a, b);
        a = b;
    }
    return total;
}

Vec2 PartySlot::sideOffset(const PartyTrail& trail, float arc) const {
    // Left of the way the path runs here, from a chord a metre long, so a single sample's kink
    // doesn't swing it.
    const Vec2 ahead = trail.pointBehind(arc - 0.5f), behind = trail.pointBehind(arc + 0.5f);
    const float dx = ahead.x - behind.x, dy = ahead.y - behind.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return offset_;
    return {-dy / len * tuning_.side, dx / len * tuning_.side};
}

void PartySlot::reset(const PartyTrail& trail) {
    arc_ = std::min(tuning_.spacing, trail.length());
    at_ = trail.odometer() - arc_;
    speed_ = leadLag_ = want_ = 0.f;
    offset_ = {};
    offset_ = sideOffset(trail, arc_);
    const Vec2 base = trail.pointBehind(arc_);
    pos_ = {base.x + offset_.x, base.y + offset_.y};
}

float PartySlot::stopDistance() const {
    if (want_ > kStanding || speed_ <= kStanding) return -1.f;
    // It eases in at the braking it plans for, and never past its place nearest the lead.
    return std::min(speed_ * speed_ / (2.f * tuning_.settle), std::max(arc_ - tuning_.minArc, 0.f));
}

void PartySlot::step(const PartyTrail& trail, float leadSpeed, float dt) {
    if (dt <= 0.f) return;
    const PartySlotTuning& k = tuning_;
    const float odo = trail.odometer();
    // A reset trail (or one that overflowed) moves the odometer on without us: stay on what's kept.
    at_ = std::clamp(at_, odo - trail.length(), odo);
    arc_ = odo - at_;

    // It follows the lead's speed a beat late, and closes a gap that has opened or shut.
    leadLag_ += (std::max(leadSpeed, 0.f) - leadLag_) * (1.f - std::exp(-dt / k.react));
    float want = leadLag_ + k.gain * std::max(arc_ - k.spacing - k.slack, 0.f);
    // It comes into its place no faster than it can stop there, as the lead stops ahead.
    want = std::min(want, std::max(leadSpeed, 0.f) + std::sqrt(2.f * k.settle * std::max(arc_ - k.spacing, 0.f)));
    // Through this stretch no faster than the lead went (plus the catch-up)...
    const float there = trail.speedBehind(arc_);
    float cap = k.maxSpeed;
    if (there >= 0.f) cap = std::min(cap, there * (1.f + k.catchUp) + k.catchUpMin);
    // ...and able to stop short of the lead.
    const float room = std::max(arc_ - k.minArc, 0.f);
    cap = std::min(cap, std::sqrt(2.f * k.decel * room));
    want = std::clamp(want, 0.f, cap);
    want_ = want;
    if (want > speed_) speed_ = std::min(want, speed_ + k.accel * dt);
    else speed_ = std::max(want, speed_ - k.decel * dt);
    // A braking limit it can't meet (the lead turned back on it) is met by the hard limit below.

    float next = std::min(at_ + speed_ * dt, odo - k.minArc);
    next = std::max(next, at_);
    float nextArc = odo - next;
    const Vec2 lead = trail.lead();
    const Vec2 want0 = sideOffset(trail, nextArc);
    const float ease = 1.f - std::exp(-dt / k.sideTime);
    const Vec2 off{offset_.x + (want0.x - offset_.x) * ease, offset_.y + (want0.y - offset_.y) * ease};
    const Vec2 base = trail.pointBehind(nextArc);
    Vec2 pos{base.x + off.x, base.y + off.y};
    // Where the path doubles back past the lead, it holds its place on the path rather than walk
    // into it, keeping its speed to carry on as the path clears...
    const float gapNow = distance(pos_, lead), gapNext = distance(pos, lead);
    if (gapNext < k.minGap && gapNext < gapNow && next > at_) {
        next = at_;
        nextArc = odo - next;
        const Vec2 stay = trail.pointBehind(nextArc);
        pos = {stay.x + off.x, stay.y + off.y};
    }
    // ...and a lead coming back past it pushes it aside, minGap away.
    const float gap = distance(pos, lead);
    if (gap < k.minGap) {
        Vec2 away{pos.x - lead.x, pos.y - lead.y};
        float len = gap;
        if (len < 1e-4f) {
            // Right on the lead: back along the path.
            const Vec2 behind = trail.pointBehind(k.minArc);
            away = {behind.x - lead.x, behind.y - lead.y};
            len = distance(behind, lead);
        }
        if (len >= 1e-4f) pos = {lead.x + away.x / len * k.minGap, lead.y + away.y / len * k.minGap};
    }
    at_ = next;
    arc_ = nextArc;
    offset_ = off;
    pos_ = pos;
}

} // namespace ftcore
