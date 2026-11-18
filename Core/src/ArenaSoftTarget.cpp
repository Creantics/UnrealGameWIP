#include "ftcore/ArenaSoftTarget.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

constexpr float kSoftEps = 1.0e-4f;    // a stick shorter than this is at rest; bodies closer are in one place
constexpr float kSoftTie = 1.0e-5f;    // scores closer than this tie (the nearer body wins)
constexpr float kSoftAlignWeight = 0.7f;
constexpr float kSoftNearWeight = 0.3f;

float lengthOf(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

Vec2 unitOr(Vec2 v, Vec2 fallback) {
    const float length = lengthOf(v);
    if (!(length > kSoftEps) || !std::isfinite(length)) return fallback;
    return Vec2{v.x / length, v.y / length};
}

} // namespace

ArenaSoftAim arenaSoftAimOf(const ArenaFighter& kit) {
    ArenaSoftAim aim;
    aim.range = kit.softRange;
    aim.arcCos = kit.softArcCos;
    aim.homing = kit.homingReach;
    return aim;
}

Vec2 arenaSoftDir(Vec2 stick, Vec2 facing) { return unitOr(stick, unitOr(facing, Vec2{1.f, 0.f})); }

std::uint8_t arenaSoftTarget(Vec2 from, Vec2 dir, const ArenaSoftBody* bodies, std::size_t count,
                             const ArenaSoftAim& aim) {
    if (!bodies || !(aim.range > 0.f)) return kArenaNobody;
    const Vec2 way = unitOr(dir, Vec2{1.f, 0.f});
    const std::size_t limit = std::min<std::size_t>(count, kArenaNobody);
    std::uint8_t best = kArenaNobody;
    float bestScore = 0.f, bestDist = 0.f;
    for (std::size_t i = 0; i < limit; ++i) {
        const ArenaSoftBody& b = bodies[i];
        if (!b.alive) continue;
        const Vec2 d{b.pos.x - from.x, b.pos.y - from.y};
        const float distance = lengthOf(d);
        if (!std::isfinite(distance)) continue;
        const float edge = std::max(0.f, distance - std::max(0.f, b.radius));
        if (edge > aim.range) continue;
        // A body on top of the attacker lies straight ahead.
        const Vec2 to = unitOr(d, way);
        const float align = std::clamp(way.x * to.x + way.y * to.y, -1.f, 1.f);
        if (align < aim.arcCos - kSoftTie) continue; // an arc at -1 or 1 takes what rounding puts on its edge
        const float score = kSoftAlignWeight * align + kSoftNearWeight * (1.f - edge / aim.range);
        const bool better = best == kArenaNobody || score > bestScore + kSoftTie ||
                            (score >= bestScore - kSoftTie && distance < bestDist);
        if (better) {
            // A nearer body winning a tie keeps the higher score, so a run of near-ties can't drift.
            bestScore = best == kArenaNobody ? score : std::max(score, bestScore);
            best = static_cast<std::uint8_t>(i);
            bestDist = distance;
        }
    }
    return best;
}

ArenaHoming arenaHoming(Vec2 from, float fromRadius, Vec2 facing, const ArenaSoftBody& target, float range,
                        float lunge, const ArenaSoftAim& aim) {
    ArenaHoming h;
    const Vec2 d{target.pos.x - from.x, target.pos.y - from.y};
    const float distance = lengthOf(d);
    h.facing = unitOr(d, unitOr(facing, Vec2{1.f, 0.f}));
    if (!std::isfinite(distance)) return h;
    const float reach = std::max(0.f, range) + std::max(0.f, target.radius);
    const float moved = std::max(0.f, lunge);
    const float missing = distance - reach - moved;
    // Never past the two bodies touching, the lunge included.
    const float room = distance - std::max(0.f, fromRadius) - std::max(0.f, target.radius) - moved;
    const float most = std::max(0.f, aim.homing);
    h.close = std::max(0.f, std::min({missing, most, room}));
    h.reaches = missing - h.close <= kSoftEps;
    return h;
}

float arenaHomingStep(float close, float span, float a0, float a1) {
    if (!(close > 0.f) || !(a1 > a0)) return 0.f;
    if (!(span > kSoftEps)) return a0 <= 0.f && a1 > 0.f ? close : 0.f;
    const float covered = std::max(0.f, std::min(a1, span) - std::max(a0, 0.f));
    return close * covered / span;
}

ArenaAimSource arenaAimSource(bool riposte, bool aiming, bool locked, bool softFound, Vec2 stick) {
    if (riposte) return ArenaAimSource::Riposte;
    if (aiming) return ArenaAimSource::Aim;
    if (locked) return ArenaAimSource::Lock;
    if (softFound) return ArenaAimSource::Soft;
    if (lengthOf(stick) > kSoftEps) return ArenaAimSource::Stick;
    return ArenaAimSource::Facing;
}

} // namespace ftcore
