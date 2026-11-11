#include "ftcore/FieldStrike.h"

#include "ftcore/AttackMove.h"

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

namespace {
std::uint32_t fieldKey(const FieldTarget& t, std::size_t index) {
    return t.id != 0 ? t.id : (0x80000000u | static_cast<std::uint32_t>(index));
}

bool usable(const FieldTarget& t) {
    return t.alive && std::isfinite(t.at.x) && std::isfinite(t.at.y) && std::isfinite(t.radius) && t.radius >= 0.f
        && std::isfinite(t.chest);
}
} // namespace

void FieldStrike::gather(Vec2 pos, const FieldTarget* targets, std::size_t count) {
    nearCount_ = 0;
    if (!targets) return;
    const std::size_t n = count < 255 ? count : 255;
    float dist[kMaxFieldTargets] = {};
    for (std::size_t i = 0; i < n; ++i) {
        const FieldTarget& t = targets[i];
        if (!usable(t)) continue;
        const float dx = t.at.x - pos.x;
        const float dy = t.at.y - pos.y;
        const float d = dx * dx + dy * dy;
        // Insert in distance order; past capacity the farthest falls off.
        std::size_t k = nearCount_;
        if (k == kMaxFieldTargets) {
            if (!(d < dist[k - 1])) continue;
            --k;
        } else {
            ++nearCount_;
        }
        while (k > 0 && d < dist[k - 1]) {
            dist[k] = dist[k - 1];
            near_[k] = near_[k - 1];
            --k;
        }
        dist[k] = d;
        near_[k] = static_cast<std::uint8_t>(i);
    }
}

void FieldStrike::takeAim(Vec2 pos, const FieldTarget* targets) {
    aim_.aimed = false;
    aim_.target = 0xFF;
    aim_.id = 0;
    if (reticleOn_) {
        const float dx = reticleAt_.x - pos.x;
        const float dy = reticleAt_.y - pos.y;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d > 1e-4f) facing_ = unit(Vec2{dx, dy});
        aim_.aimed = true;
        aim_.at = reticleAt_;
        aim_.height = reticleHeight_;
        for (std::uint8_t k = 0; k < nearCount_; ++k) {
            const FieldTarget& t = targets[near_[k]];
            const float ox = t.at.x - reticleAt_.x;
            const float oy = t.at.y - reticleAt_.y;
            if (std::sqrt(ox * ox + oy * oy) > t.radius + kFieldReticleSlack) continue;
            aim_.target = near_[k];
            aim_.id = t.id;
            break;
        }
        const float reach = attack_ ? attack_->range * lungeStop_ : 0.f;
        const float room = d - reach;
        const float want = attack_ ? attack_->lunge * lungeScale_ : 0.f;
        lunge_ = room > 0.f ? (want < room ? want : room) : 0.f;
        return;
    }
    lunge_ = 0.f;
    for (std::uint8_t k = 0; attack_ && k < nearCount_; ++k) {
        const FieldTarget& t = targets[near_[k]];
        const float dx = t.at.x - pos.x;
        const float dy = t.at.y - pos.y;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d > attack_->range + t.radius) continue;
        if (d > 1e-4f) facing_ = unit(Vec2{dx, dy});
        aim_.aimed = true;
        aim_.target = near_[k];
        aim_.id = t.id;
        aim_.at = fieldHitPoint(pos, t.at, t.radius);
        aim_.height = t.chest;
        return;
    }
    const float range = attack_ ? attack_->range : 1.f;
    aim_.at = Vec2{pos.x + facing_.x * range, pos.y + facing_.y * range};
    aim_.height = chest_;
}

void FieldStrike::strikeHits(Vec2 pos, const FieldTarget* targets) {
    // nearCount_ > 0 only with a list; near_ is nearest first.
    for (std::uint8_t k = 0; k < nearCount_; ++k) {
        if (attack_->maxTargets > 0 && hitCount_ >= attack_->maxTargets) return;
        if (hitCount_ >= kMaxFieldTargets) return;
        const std::size_t i = near_[k];
        const FieldTarget& t = targets[i];
        const std::uint32_t key = fieldKey(t, i);
        bool done = false;
        for (std::uint8_t h = 0; h < hitCount_; ++h) done = done || hitKeys_[h] == key;
        if (done || !fieldStrikeReaches(*attack_, pos, facing_, t.at, t.radius)) continue;
        hitKeys_[hitCount_++] = key;
        ++hits_;
        damage_ += attack_->damage;
        push(ArenaEventType::Hit, static_cast<std::uint8_t>(i), attack_->damage, t.at);
    }
}

void FieldStrike::cancel() {
    if (attack_ && lastWasLight_) lastLightEnd_ = time_;
    attack_ = nullptr;
    chainLive_ = false;
    hitCount_ = 0;
    actionTime_ = 0.f;
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
    chainLive_ = false;
    body_ = FieldBody::Free;
    handBackLeft_ = 0.f;
    buffered_ = kNoMove;
    bufferedAt_ = 0.f;
    facing_ = Vec2{1.f, 0.f};
    aim_ = FieldAim{};
    lunge_ = 0.f;
    hitCount_ = 0;
    nearCount_ = 0;
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
        // The chain: the next light during the last light's recovery or within chainGap after it. A heavy (or a cut)
        // in between ends it: the next light opens a new chain.
        const bool chained = chainLive_
            && ((attack_ != nullptr && lastWasLight_) || (time_ - lastLightEnd_ <= kit_.chainGap));
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
    chainLive_ = lastWasLight_;
    if (lastWasLight_) lastLight_ = m;
    hitCount_ = 0;
    push(ArenaEventType::AttackStarted, kArenaNobody, 0, Vec2{});
    return true;
}

const ArenaOutput& FieldStrike::tick(float dt, bool light, bool heavy, Vec2 pos, Vec2 facing,
                                     const FieldTarget* targets, std::size_t targetCount) {
    out_.events.count = 0;
    if (!(dt > 0.f) || !std::isfinite(dt)) dt = 0.f;
    time_ += dt;
    ArenaPlayerOut& p = out_.player;
    p.velocity = Vec2{};

    // The body is busy with a move of its own, or handing back from one: no attack, nothing kept for later.
    bool held = fieldBodyBusy(body_);
    if (!held && handBackLeft_ > 0.f) {
        handBackLeft_ -= dt;
        held = true;
    }
    if (held) {
        buffered_ = kNoMove;
        if (attack_) cancel();
        nearCount_ = 0;
        p.action = ArenaAction::Idle;
        p.part = ArenaPart::None;
        p.actionTime = 0.f;
        p.facing = unit(facing);
        return out_;
    }

    gather(pos, targets, targetCount);
    // A kit with no attacks of its own (ArenaFighter::attacks) drops every press.
    if ((heavy || light) && kit_.attacks) {
        buffered_ = heavy ? kArenaHeavyMove : 0;
        bufferedAt_ = time_;
    }
    if (buffered_ != kNoMove && time_ - bufferedAt_ > kit_.bufferTime) buffered_ = kNoMove;

    // On the move with nothing to face: the attack under way turns with the body, capped, before its hit test. Aimed at
    // a target it holds on it only until its hit is over: through the recovery it turns with the body too.
    const bool pastHit = attack_ && actionTime_ >= attack_->startup + attack_->active;
    if (attack_ && steerRate_ > 0.f && (!aim_.aimed || pastHit) && !reticleOn_ && dt > 0.f) {
        facing_ = turnCapped(facing_, steerWant_, steerRate_ * dt);
        // An aimed attack keeps its hit point (the arm eases off it through the recovery).
        if (!aim_.aimed) aim_.at = Vec2{pos.x + facing_.x * attack_->range, pos.y + facing_.y * attack_->range};
    }
    // The attack under way runs on. A frame that reaches into its active part, or passes over it whole, tests hits.
    if (attack_) {
        const float from = actionTime_;
        actionTime_ += dt;
        const float on = attack_->startup;
        const float off = attack_->startup + attack_->active;
        if (actionTime_ >= on && from < off && attack_->active > 0.f) strikeHits(pos, targets);
        if (actionTime_ >= attack_->total()) {
            if (lastWasLight_) lastLightEnd_ = time_;
            attack_ = nullptr;
        }
    }
    // A press starts once the attack is free or past the cancel share of its recovery.
    bool started = false;
    if (buffered_ != kNoMove) {
        const bool canStart = !attack_
            || actionTime_ >= attack_->startup + attack_->active + attack_->recovery * attack_->cancel;
        if (canStart && start(buffered_, facing)) {
            buffered_ = kNoMove;
            started = true;
        }
    }
    // Aimed as it starts and through its startup (the target may move); held from active on.
    if (attack_ && actionTime_ < attack_->startup + 1e-6f) takeAim(pos, targets);
    // An attack with no startup is active on the frame it starts.
    if (started && attack_->startup <= 0.f && attack_->active > 0.f) strikeHits(pos, targets);

    if (attack_) {
        p.action = move_ == kArenaHeavyMove ? ArenaAction::Heavy : ArenaAction::Light;
        p.move = move_;
        p.actionTime = actionTime_;
        p.facing = facing_;
        // The aimed lunge, spread over startup and active.
        const float span = attack_->startup + attack_->active;
        if (lunge_ > 0.f && span > 1e-4f && dt > 0.f) {
            const float before = actionTime_ - dt;
            const float lo = before > 0.f ? before : 0.f;
            const float hi = actionTime_ < span ? actionTime_ : span;
            const float moving = hi > lo ? hi - lo : 0.f;
            const float speed = lunge_ / span * moving / dt;
            p.velocity = Vec2{facing_.x * speed, facing_.y * speed};
        }
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
    return out_;
}

} // namespace ftcore
