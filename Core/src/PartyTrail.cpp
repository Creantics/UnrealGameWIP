#include "ftcore/PartyTrail.h"

#include <cmath>

namespace ftcore {

namespace {

float distance(Vec2 a, Vec2 b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}

Vec2 lerp(Vec2 a, Vec2 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

} // namespace

void PartyTrail::add(Vec2 p) {
    newest_ = (newest_ + 1) % kCapacity;
    ring_[newest_] = p;
    if (count_ < kCapacity) ++count_;
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
        add({lead.x - dir.x * back, lead.y - dir.y * back});
    }
    lead_ = lead;
}

void PartyTrail::push(Vec2 lead) {
    if (count_ == 0) {
        reset(lead, {}, 0.f);
        return;
    }
    Vec2 from = sample(0);
    float d = distance(from, lead);
    if (d > static_cast<float>(kCapacity) * kSampleSpacing) {
        reset(lead, {lead.x - from.x, lead.y - from.y}, 0.f);
        return;
    }
    // Lay samples at even spacing along the move, so a long frame still leaves a usable path.
    while (d >= kSampleSpacing) {
        const Vec2 next = lerp(from, lead, kSampleSpacing / d);
        add(next);
        from = next;
        d -= kSampleSpacing;
    }
    lead_ = lead;
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

} // namespace ftcore
