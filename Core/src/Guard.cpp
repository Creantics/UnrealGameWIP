#include "ftcore/Guard.h"

#include <cmath>

namespace ftcore {

namespace {

float distance(Vec2 a, Vec2 b) { return std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y)); }

// Walks `pos` up to `maxStep` toward `goal`, facing the way it walks. True once it is there.
bool moveToward(Vec2& pos, Vec2& facing, Vec2 goal, float maxStep) {
    const float d = distance(pos, goal);
    if (d <= maxStep || d < 1e-5f) {
        if (d >= 1e-5f) facing = {(goal.x - pos.x) / d, (goal.y - pos.y) / d};
        pos = goal;
        return true;
    }
    facing = {(goal.x - pos.x) / d, (goal.y - pos.y) / d};
    pos.x += facing.x * maxStep;
    pos.y += facing.y * maxStep;
    return false;
}

void face(Vec2& facing, Vec2 from, Vec2 to) {
    const float d = distance(from, to);
    if (d >= 1e-5f) facing = {(to.x - from.x) / d, (to.y - from.y) / d};
}

} // namespace

const char* guardStateName(GuardState s) {
    switch (s) {
    case GuardState::Idle:
        return "idle";
    case GuardState::Patrol:
        return "patrol";
    case GuardState::Notice:
        return "notice";
    case GuardState::Chase:
        return "chase";
    case GuardState::Return:
        return "return";
    }
    return "?";
}

int GuardSet::add(const GuardSpec& spec) {
    if (count_ >= kMaxGuards || spec.routeCount == 0) return -1;
    GuardSpec s = spec;
    if (s.routeCount > GuardSpec::kMaxWaypoints) s.routeCount = static_cast<std::uint8_t>(GuardSpec::kMaxWaypoints);
    const float flen = std::sqrt(s.facing.x * s.facing.x + s.facing.y * s.facing.y);
    s.facing = flen > 1e-5f ? Vec2{s.facing.x / flen, s.facing.y / flen} : Vec2{1.f, 0.f};
    specs_[count_] = s;
    Guard g;
    g.pos = s.route[0];
    g.facing = s.facing;
    guards_[count_] = g;
    return static_cast<int>(count_++);
}

bool GuardSet::active(int id) const {
    return id >= 0 && static_cast<std::size_t>(id) < count_ && !guards_[static_cast<std::size_t>(id)].defeated;
}

bool GuardSet::sees(const Guard& g, const GuardTuning& t, Vec2 target, bool clear) const {
    if (!clear || g.cooldown > 0.f) return false;
    const float d = distance(g.pos, target);
    if (g.state == GuardState::Chase) return d <= t.loseRange;
    if (d <= t.senseRadius) return true;
    if (d > t.visionRange) return false;
    return ((target.x - g.pos.x) * g.facing.x + (target.y - g.pos.y) * g.facing.y) >= t.visionHalfAngleCos * d;
}

bool GuardSet::inSenseRange(int id, Vec2 target) const {
    if (!active(id)) return false;
    const std::size_t i = static_cast<std::size_t>(id);
    return sees(guards_[i], specs_[i].tuning, target, true);
}

int GuardSet::step(Vec2 target, std::uint32_t sight, float dt, GuardEvents* events) {
    if (events) events->bits = {};
    if (dt < 0.f) dt = 0.f;
    int touched = -1;
    for (std::size_t i = 0; i < count_; ++i) {
        Guard& g = guards_[i];
        if (g.defeated) continue;
        const GuardSpec& s = specs_[i];
        const GuardTuning& t = s.tuning;
        std::uint8_t bits = 0;
        g.cooldown = g.cooldown > dt ? g.cooldown - dt : 0.f;
        const bool seen = sees(g, t, target, (sight >> i) & 1u);
        const Vec2 home = s.route[static_cast<std::size_t>(g.waypoint)];

        switch (g.state) {
        case GuardState::Idle:
        case GuardState::Patrol:
        case GuardState::Return:
            if (seen) {
                g.state = GuardState::Notice;
                g.timer = 0.f;
                g.lastSeen = target;
                face(g.facing, g.pos, target);
                bits |= kGuardNoticed;
            } else if (g.state == GuardState::Idle) {
                g.timer += dt;
                if (s.routeCount > 1 && g.timer >= t.pauseTime) {
                    g.waypoint = (g.waypoint + 1) % s.routeCount;
                    g.state = GuardState::Patrol;
                }
            } else if (moveToward(g.pos, g.facing, home, t.walkSpeed * dt)) {
                g.state = GuardState::Idle;
                g.timer = 0.f;
                if (s.routeCount == 1) g.facing = s.facing;
            }
            break;
        case GuardState::Notice:
            if (seen) {
                g.lastSeen = target;
                face(g.facing, g.pos, target);
            }
            g.timer += dt;
            if (g.timer >= t.reactionTime) {
                if (seen) {
                    g.state = GuardState::Chase;
                    g.lostTimer = 0.f;
                    bits |= kGuardChaseStarted;
                } else {
                    g.state = GuardState::Return;
                    bits |= kGuardLost;
                }
            }
            break;
        case GuardState::Chase:
            if (seen) {
                g.lastSeen = target;
                g.lostTimer = 0.f;
            } else {
                g.lostTimer += dt;
            }
            if (g.lostTimer >= t.loseTime) {
                g.state = GuardState::Return;
                bits |= kGuardLost;
            } else {
                moveToward(g.pos, g.facing, g.lastSeen, t.chaseSpeed * dt);
            }
            break;
        }

        if (touched < 0 && g.cooldown <= 0.f && distance(g.pos, target) < t.contactRadius) {
            touched = static_cast<int>(i);
            g.state = GuardState::Return;
            g.cooldown = t.contactCooldown;
            bits = static_cast<std::uint8_t>((bits & ~kGuardLost) | kGuardContact);
        }
        if (events) events->bits[i] = bits;
    }
    return touched;
}

void GuardSet::defeat(int id) {
    if (active(id)) guards_[static_cast<std::size_t>(id)].defeated = true;
}

void GuardSet::reset(int id) {
    if (!active(id)) return;
    const std::size_t i = static_cast<std::size_t>(id);
    Guard g;
    g.pos = specs_[i].route[0];
    g.facing = specs_[i].facing;
    g.cooldown = guards_[i].cooldown;
    guards_[i] = g;
}

void GuardSet::holdOff(float seconds) {
    for (std::size_t i = 0; i < count_; ++i) {
        if (guards_[i].cooldown < seconds) guards_[i].cooldown = seconds;
    }
}

GuardSave GuardSet::save() const {
    GuardSave out;
    for (std::size_t i = 0; i < count_; ++i) {
        if (guards_[i].defeated) out.defeated |= 1u << i;
    }
    return out;
}

void GuardSet::load(const GuardSave& save) {
    for (std::size_t i = 0; i < count_; ++i) guards_[i].defeated = ((save.defeated >> i) & 1u) != 0;
}

} // namespace ftcore
