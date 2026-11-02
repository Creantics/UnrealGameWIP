#include "ftcore/DodgeRoll.h"

#include <algorithm>

namespace ftcore {

DodgeState DodgeRoll::fresh() const {
    DodgeState s;
    s.stamina = tuning_.staminaMax;
    s.sinceSpent = tuning_.regenDelay;
    return s;
}

bool DodgeRoll::canStart(const DodgeState& s, bool hasDirection) const {
    return !s.active() && s.cooldown <= 0.f && (hasDirection || tuning_.needsDirection < 0.5f);
}

DodgeKind DodgeRoll::kindFor(const DodgeState& s, float speed, bool hasDirection) const {
    if (!canStart(s, hasDirection)) return DodgeKind::None;
    if (tuning_.staminaCost > 0.f && s.stamina < tuning_.staminaCost) return DodgeKind::Tired;
    return speed >= tuning_.longFrom ? DodgeKind::Long : DodgeKind::Short;
}

bool DodgeRoll::start(DodgeState& s, float speed, bool hasDirection, float length) const {
    const DodgeKind kind = kindFor(s, speed, hasDirection);
    if (kind == DodgeKind::None) return false;
    s.kind = kind;
    s.time = 0.f;
    s.length = length > 0.f ? length : tuning_.length;
    // A Long roll keeps a sprint's momentum: the body's speed, but no slower than longSpeed.
    s.speed = kind == DodgeKind::Long ? std::max(speed, tuning_.longSpeed) : kind == DodgeKind::Short ? tuning_.shortSpeed : tuning_.tiredSpeed;
    if (tuning_.staminaCost > 0.f) {
        s.stamina = std::max(s.stamina - tuning_.staminaCost, 0.f);
        s.sinceSpent = 0.f;
    }
    return true;
}

void DodgeRoll::tick(DodgeState& s, float dt) const {
    dt = std::max(dt, 0.f);
    if (s.active()) {
        s.time += dt;
        if (s.time >= s.length) stop(s);
    } else {
        s.cooldown = std::max(s.cooldown - dt, 0.f);
    }
    // Stamina comes back once regenDelay has gone since it was spent, dodging or not.
    s.sinceSpent += dt;
    if (s.sinceSpent >= tuning_.regenDelay) {
        const float over = std::min(s.sinceSpent - tuning_.regenDelay, dt);
        s.stamina = std::min(s.stamina + tuning_.staminaRegen * over, tuning_.staminaMax);
    }
}

void DodgeRoll::stop(DodgeState& s) const {
    if (!s.active()) return;
    s.kind = DodgeKind::None;
    s.time = 0.f;
    s.cooldown = tuning_.cooldown;
}

bool DodgeRoll::invulnerable(const DodgeState& s) const {
    if (!s.active()) return false;
    float end = tuning_.invulnEnd;
    if (s.kind == DodgeKind::Tired) {
        end = tuning_.invulnStart + (tuning_.invulnEnd - tuning_.invulnStart) * std::clamp(tuning_.tiredInvuln, 0.f, 1.f);
    }
    return s.time >= tuning_.invulnStart && s.time < end;
}

} // namespace ftcore
