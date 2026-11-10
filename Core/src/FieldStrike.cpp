#include "ftcore/FieldStrike.h"

#include <cmath>

namespace ftcore {

namespace {
constexpr std::uint8_t kNoMove = 0xFF;

Vec2 unit(Vec2 v) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y);
    if (!(len > 1e-6f)) return Vec2{1.f, 0.f};
    return Vec2{v.x / len, v.y / len};
}
} // namespace

bool fieldStrikeReaches(const ArenaAttack& attack, Vec2 from, Vec2 facing, Vec2 at, float radius) {
    const Vec2 f = unit(facing);
    const float dx = at.x - from.x;
    const float dy = at.y - from.y;
    const float reach = attack.range + radius;
    if (attack.width > 0.f) {
        const float along = dx * f.x + dy * f.y;
        const float off = std::fabs(dx * f.y - dy * f.x);
        return along >= 0.f && along <= reach && off <= attack.width * 0.5f + radius;
    }
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (dist > reach) return false;
    if (dist < 1e-4f) return true;
    return (dx * f.x + dy * f.y) / dist >= attack.arcCos;
}

Vec2 fieldHitPoint(Vec2 from, Vec2 at, float radius) {
    const float dx = from.x - at.x;
    const float dy = from.y - at.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (!(dist > radius) || !(dist > 1e-6f)) return at;
    return Vec2{at.x + dx / dist * radius, at.y + dy / dist * radius};
}

void FieldStrike::takeAim(Vec2 pos, const FieldTarget* targets, std::size_t n) {
    aim_.aimed = false;
    aim_.target = 0xFF;
    std::size_t best = n;
    float bestD = 0.f;
    if (attack_ && targets) {
        for (std::size_t i = 0; i < n; ++i) {
            const FieldTarget& t = targets[i];
            if (!t.alive) continue;
            const float dx = t.at.x - pos.x;
            const float dy = t.at.y - pos.y;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d > attack_->range + t.radius) continue;
            if (best == n || d < bestD) {
                best = i;
                bestD = d;
            }
        }
    }
    if (best == n) {
        const float range = attack_ ? attack_->range : 1.f;
        aim_.at = Vec2{pos.x + facing_.x * range, pos.y + facing_.y * range};
        aim_.height = chest_;
        return;
    }
    const FieldTarget& t = targets[best];
    if (bestD > 1e-4f) facing_ = unit(Vec2{t.at.x - pos.x, t.at.y - pos.y});
    aim_.aimed = true;
    aim_.target = static_cast<std::uint8_t>(best);
    aim_.at = fieldHitPoint(pos, t.at, t.radius);
    aim_.height = t.chest;
}

void FieldStrike::begin(const ArenaFighter& kit) {
    kit_ = kit;
    out_ = ArenaOutput{};
    attack_ = nullptr;
    move_ = 0;
    actionTime_ = 0.f;
    time_ = 0.f;
    lastLightEnd_ = -1e9f;
    lastLight_ = 0;
    lastWasLight_ = false;
    buffered_ = kNoMove;
    bufferedAt_ = 0.f;
    facing_ = Vec2{1.f, 0.f};
    aim_ = FieldAim{};
    for (bool& h : hit_) h = false;
    hitCount_ = 0;
    hits_ = 0;
    damage_ = 0;
}

void FieldStrike::push(ArenaEventType type, std::uint8_t target, int amount, Vec2 at) {
    ArenaEvents& ev = out_.events;
    if (ev.count >= ev.items.size()) return;
    ArenaEvent& e = ev.items[ev.count++];
    e.type = type;
    e.source = kArenaPlayer;
    e.target = target;
    e.move = move_;
    e.amount = amount;
    e.at = at;
}

bool FieldStrike::start(std::uint8_t move, Vec2 facing) {
    std::uint8_t m = move;
    if (m != kArenaHeavyMove) {
        // The chain: the next light during the last light's recovery or within chainGap after it.
        const bool chained = (attack_ != nullptr && lastWasLight_) || (time_ - lastLightEnd_ <= kit_.chainGap);
        const std::uint8_t count = kit_.lightCount == 0 ? 1 : kit_.lightCount;
        m = chained ? static_cast<std::uint8_t>((lastLight_ + 1) % count) : 0;
    }
    const ArenaAttack* a = arenaPlayerAttack(kit_, m);
    if (!a) return false;
    if (attack_ && lastWasLight_) lastLightEnd_ = time_;
    attack_ = a;
    move_ = m;
    actionTime_ = 0.f;
    facing_ = unit(facing);
    lastWasLight_ = m != kArenaHeavyMove;
    if (lastWasLight_) lastLight_ = m;
    for (bool& h : hit_) h = false;
    hitCount_ = 0;
    push(ArenaEventType::AttackStarted, kArenaNobody, 0, Vec2{});
    return true;
}

const ArenaOutput& FieldStrike::tick(float dt, bool light, bool heavy, Vec2 pos, Vec2 facing,
                                     const FieldTarget* targets, std::size_t targetCount) {
    out_.events.count = 0;
    if (!(dt > 0.f)) dt = 0.f;
    time_ += dt;

    if (heavy || light) {
        buffered_ = heavy ? kArenaHeavyMove : 0;
        bufferedAt_ = time_;
    }
    if (buffered_ != kNoMove && time_ - bufferedAt_ > kit_.bufferTime) buffered_ = kNoMove;

    // The attack under way runs on.
    if (attack_) {
        actionTime_ += dt;
        if (actionTime_ >= attack_->total()) {
            if (lastWasLight_) lastLightEnd_ = time_;
            attack_ = nullptr;
        }
    }
    // A press starts once the attack is free or past the cancel share of its recovery.
    if (buffered_ != kNoMove) {
        const bool canStart = !attack_
            || actionTime_ >= attack_->startup + attack_->active + attack_->recovery * attack_->cancel;
        if (canStart && start(buffered_, facing)) buffered_ = kNoMove;
    }
    const std::size_t n = targets ? (targetCount < kMaxFieldTargets ? targetCount : kMaxFieldTargets) : 0;
    // Aimed as it starts and through its startup (the target may move); held from active on.
    if (attack_ && actionTime_ < attack_->startup + 1e-6f) takeAim(pos, targets, n);

    ArenaPlayerOut& p = out_.player;
    p.velocity = Vec2{};
    if (attack_) {
        p.action = move_ == kArenaHeavyMove ? ArenaAction::Heavy : ArenaAction::Light;
        p.move = move_;
        p.actionTime = actionTime_;
        p.facing = facing_;
        if (actionTime_ < attack_->startup) {
            p.part = ArenaPart::Startup;
        } else if (actionTime_ < attack_->startup + attack_->active) {
            p.part = ArenaPart::Active;
        } else {
            p.part = ArenaPart::Recovery;
        }
    } else {
        p.action = ArenaAction::Idle;
        p.part = ArenaPart::None;
        p.actionTime = 0.f;
        p.facing = unit(facing);
    }

    // Active: connect with the targets in reach, nearest first, each once.
    if (attack_ && p.part == ArenaPart::Active && targets) {
        for (;;) {
            if (attack_->maxTargets > 0 && hitCount_ >= attack_->maxTargets) break;
            std::size_t best = n;
            float bestD = 0.f;
            for (std::size_t i = 0; i < n; ++i) {
                const FieldTarget& t = targets[i];
                if (!t.alive || hit_[i] || !fieldStrikeReaches(*attack_, pos, facing_, t.at, t.radius)) continue;
                const float dx = t.at.x - pos.x;
                const float dy = t.at.y - pos.y;
                const float d = dx * dx + dy * dy;
                if (best == n || d < bestD) {
                    best = i;
                    bestD = d;
                }
            }
            if (best == n) break;
            hit_[best] = true;
            ++hitCount_;
            ++hits_;
            damage_ += attack_->damage;
            push(ArenaEventType::Hit, static_cast<std::uint8_t>(best), attack_->damage, targets[best].at);
        }
    }
    return out_;
}

} // namespace ftcore
