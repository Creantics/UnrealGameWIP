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

float norm(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// No longer than `most`, the same way.
Vec2 clampLength(Vec2 v, float most) {
    const float len = norm(v);
    return len > most ? Vec2{v.x / len * most, v.y / len * most} : v;
}

// A stray this near its place (metres) is back on it: with no limit on its rate, within a centimetre; with one, within a
// hundredth of a millimetre (its landing brings it right onto it) and slower than kStrayStill (m/s), so the last step
// onto its place doesn't jolt the body's speed.
constexpr float kStrayNear = 0.01f;
constexpr float kStrayNearRamp = 1e-5f;
constexpr float kStrayStill = 0.1f;

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

float PartyTrail::arcNearest(Vec2 p) const {
    float best = 0.f;
    float bestD2 = (p.x - lead_.x) * (p.x - lead_.x) + (p.y - lead_.y) * (p.y - lead_.y);
    float arc = 0.f;
    Vec2 a = lead_;
    for (std::size_t k = 0; k < count_; ++k) {
        const Vec2 b = sample(k);
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float seg2 = dx * dx + dy * dy;
        if (seg2 > 0.f) {
            const float u = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / seg2, 0.f, 1.f);
            const float qx = a.x + dx * u - p.x, qy = a.y + dy * u - p.y;
            const float seg = std::sqrt(seg2);
            if (qx * qx + qy * qy < bestD2) {
                bestD2 = qx * qx + qy * qy;
                best = arc + seg * u;
            }
            arc += seg;
        }
        a = b;
    }
    return best;
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
    // Right of the way the path runs here, from a chord a metre long, so a single sample's kink
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
    speed_ = leadLag_ = want_ = clock_ = heardMark_ = stopDecel_ = stopRoom_ = 0.f;
    stopping_ = false;
    heardNewest_ = heardCount_ = 0;
    offset_ = stray_ = {};
    offset_ = sideOffset(trail, arc_);
    const Vec2 base = trail.pointBehind(arc_);
    pos_ = {base.x + offset_.x, base.y + offset_.y};
}

void PartySlot::resetAt(const PartyTrail& trail, Vec2 standing) {
    reset(trail);
    arc_ = trail.arcNearest(standing);
    at_ = trail.odometer() - arc_;
    offset_ = sideOffset(trail, arc_);
    const Vec2 base = trail.pointBehind(arc_);
    stray_ = {standing.x - base.x - offset_.x, standing.y - base.y - offset_.y};
    pos_ = standing;
}

float PartySlot::stopDistance() const {
    if (!stopping_ || speed_ <= kStanding) return -1.f;
    // It brakes at the rate it planned, and never past its place nearest the lead.
    return std::min(std::min(speed_ * speed_ / (2.f * stopDecel_), stopRoom_ + 0.05f), std::max(arc_ - tuning_.minArc, 0.f));
}

PartySlot::Heard PartySlot::heard(float odometer, float leadSpeed, float leadStop, float dt) {
    clock_ += dt;
    // A new sample at least delay/24 after the last one began, so the ring spans the delay whatever
    // the frame rate; between those the newest is this frame's.
    const float gapAt = tuning_.delay / static_cast<float>(kHeard - 8);
    if (heardCount_ == 0 || clock_ - heardMark_ >= gapAt) {
        heardNewest_ = heardCount_ == 0 ? 0 : (heardNewest_ + 1) % kHeard;
        heardCount_ = std::min(heardCount_ + 1, kHeard);
        heardMark_ = clock_;
    }
    heardAt_[heardNewest_] = clock_;
    heardOdo_[heardNewest_] = odometer;
    heardSpeed_[heardNewest_] = leadSpeed;
    heardStop_[heardNewest_] = leadStop;
    const float when = clock_ - tuning_.delay;
    // Newest back to the first sample at or before `when`, between it and the one after.
    std::size_t later = heardNewest_;
    for (std::size_t k = 1; k < heardCount_; ++k) {
        const std::size_t i = (heardNewest_ + kHeard - k) % kHeard;
        if (heardAt_[i] <= when) {
            const float span = heardAt_[later] - heardAt_[i];
            const float u = span > 0.f ? std::clamp((when - heardAt_[i]) / span, 0.f, 1.f) : 0.f;
            // A stop is heard from the sample before (the distance left is only meaningful there).
            return {heardOdo_[i] + (heardOdo_[later] - heardOdo_[i]) * u, heardSpeed_[i] + (heardSpeed_[later] - heardSpeed_[i]) * u,
                heardStop_[i] < 0.f ? -1.f : heardStop_[i] - (heardOdo_[later] - heardOdo_[i]) * u};
        }
        later = i;
    }
    // Not that much kept yet (just reset): the oldest.
    return {heardOdo_[later], heardSpeed_[later], heardStop_[later]};
}

void PartySlot::step(const PartyTrail& trail, float leadSpeed, float dt, float leadStop) {
    if (dt <= 0.f) return;
    const PartySlotTuning& k = tuning_;
    const float odo = trail.odometer();
    // A reset trail (or one that overflowed) moves the odometer on without us: stay on what's kept.
    at_ = std::clamp(at_, odo - trail.length(), odo);
    arc_ = odo - at_;

    // It keeps its place behind the lead as it was a beat ago: follows its speed then, closes a gap
    // that has opened or shut since.
    const Heard then = heard(odo, std::max(leadSpeed, 0.f), leadStop, dt);
    const float back = std::max(then.odometer - at_, 0.f);
    leadLag_ += (then.speed - leadLag_) * (1.f - std::exp(-dt / k.react));
    float want = leadLag_ + k.gain * std::max(back - k.spacing - k.slack, 0.f);
    // It comes into its place no faster than it can stop there, as the lead stops ahead.
    want = std::min(want, then.speed + std::sqrt(2.f * k.settle * std::max(back - k.spacing, 0.f)));
    // Through this stretch no faster than the lead went (plus the catch-up)...
    const float there = trail.speedBehind(arc_);
    float cap = k.maxSpeed;
    if (there >= 0.f) cap = std::min(cap, there * (1.f + k.catchUp) + k.catchUpMin);
    // ...and able to stop short of the lead.
    const float room = std::max(arc_ - k.minArc, 0.f);
    cap = std::min(cap, std::sqrt(2.f * k.decel * room));
    want = std::clamp(want, 0.f, cap);
    // The lead braking to a stand (or standing), as it heard a beat ago: it plans its own stop to stand
    // `spacing` behind where the lead will, at a steady braking it picks as it begins.
    const bool stopping = then.stop >= 0.f || then.speed <= kStanding;
    if (stopping) {
        const float place = then.odometer + std::max(then.stop, 0.f) - k.spacing;
        stopRoom_ = std::max(place - at_, 0.f);
        if (!stopping_) stopDecel_ = std::clamp(speed_ * speed_ / (2.f * std::max(stopRoom_, 1e-3f)), k.settle, k.decel);
        want = std::min(want, std::sqrt(2.f * stopDecel_ * stopRoom_));
    }
    stopping_ = stopping;
    want_ = stopping ? 0.f : want;
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
    // Left standing off its place (resetAt), it walks back at `rejoin`, easing in at the end.
    Vec2 stray = stray_;
    if (const float len = std::sqrt(stray.x * stray.x + stray.y * stray.y); len > 0.f) {
        const float left = len - std::min(len * ease, k.rejoin * dt);
        stray = left > 1e-3f ? Vec2{stray.x / len * left, stray.y / len * left} : Vec2{};
    }
    const Vec2 base = trail.pointBehind(nextArc);
    Vec2 pos{base.x + off.x + stray.x, base.y + off.y + stray.y};
    // Where the path doubles back past the lead, it holds its place on the path rather than walk
    // into it, keeping its speed to carry on as the path clears...
    const float gapNow = distance(pos_, lead), gapNext = distance(pos, lead);
    if (gapNext < k.minGap && gapNext < gapNow && next > at_) {
        next = at_;
        nextArc = odo - next;
        const Vec2 stay = trail.pointBehind(nextArc);
        pos = {stay.x + off.x + stray.x, stay.y + off.y + stray.y};
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
    stray_ = stray;
    pos_ = pos;
}

void easeStray(Vec2& stray, Vec2& rate, Vec2 from, Vec2 way, float dt, const PartyStrayTuning& k) {
    if (dt <= 0.f) return;
    const bool ramp = k.accel > 0.f;
    if (stray.x == 0.f && stray.y == 0.f && (!ramp || norm(rate) < kStrayStill)) {
        rate = {};
        return;
    }
    // Where it would be next: back toward its place with time constant `time` (near it, no slower than a body braking at
    // `land` closes to stop on it, onto it at the last), no faster than `speed`...
    const float decay = std::exp(-dt / k.time) - 1.f;
    Vec2 change{stray.x * decay, stray.y * decay};
    const float off = norm(stray);
    if (ramp && k.land > 0.f && off > 0.f) {
        const float least = std::min(off, std::sqrt(2.f * k.land * off) * dt);
        if (norm(change) < least) change = {-stray.x / off * least, -stray.y / off * least};
    }
    change = clampLength(change, k.speed * dt);
    Vec2 next{stray.x + change.x, stray.y + change.y};
    // ...but ahead of its place the way it was going, not back toward it: it waits for its place to come on.
    const float back = (next.x - from.x) * way.x + (next.y - from.y) * way.y;
    if (back < 0.f && next.x * way.x + next.y * way.y > 0.f && norm(next) < k.wait) next = {next.x - way.x * back, next.y - way.y * back};
    const Vec2 want{(next.x - stray.x) / dt, (next.y - stray.y) / dt};
    if (ramp) {
        // Its rate goes there no faster than a person's speed changes, from the one it was let go with.
        const Vec2 turn = clampLength({want.x - rate.x, want.y - rate.y}, k.accel * dt);
        rate = {rate.x + turn.x, rate.y + turn.y};
        next = {stray.x + rate.x * dt, stray.y + rate.y * dt};
    } else {
        rate = want;
    }
    if (ramp ? norm(next) < kStrayNearRamp && norm(rate) < kStrayStill : norm(next) < kStrayNear) {
        stray = rate = {};
        return;
    }
    stray = next;
}

} // namespace ftcore
