#include "ftcore/PartnerCloseStation.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace ftcore {

namespace {

constexpr float kDeg = 3.14159265358979f / 180.f;
constexpr float kHalfPi = 1.57079632679490f;

Vec2 sub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 add(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 mul(Vec2 a, float k) { return {a.x * k, a.y * k}; }
float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
float cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
float len(Vec2 a) { return std::sqrt(dot(a, a)); }
// a over its length; under 1e-4: false.
bool unitOf(Vec2 a, Vec2& out) {
    const float l = len(a);
    if (!(l > 1e-4f)) return false;
    out = {a.x / l, a.y / l};
    return true;
}
// v turned `degrees` counter-clockwise in x, y (as PartnerBrain's stTurn).
Vec2 turn(Vec2 v, float degrees) {
    const float c = std::cos(degrees * kDeg), s = std::sin(degrees * kDeg);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}
// The lens's level right for a level forward f (unit): up x forward with x forward and y right at yaw 0, as the
// arena's frustum test (arenaInFrustum): (-f.y, f.x).
Vec2 rightOf(Vec2 f) { return {-f.y, f.x}; }
bool fin(float v) { return std::isfinite(v); }
bool fin(Vec2 v) { return fin(v.x) && fin(v.y); }
bool within(float v, float lo, float hi) { return fin(v) && v >= lo && v <= hi; }

// The angle (radians, 0..pi) between p and b seen from the lens.
float apart(Vec2 cam, Vec2 p, Vec2 b) {
    Vec2 u, v;
    if (!unitOf(sub(p, cam), u) || !unitOf(sub(b, cam), v)) return 0.f;
    return std::atan2(std::fabs(cross(u, v)), dot(u, v));
}
// The half-angle a body of radius r at distance d fills; the lens inside it: a right angle.
float halfAngle(float d, float r) { return d <= r ? kHalfPi : std::asin(r / d); }

float distToSegment(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = sub(b, a);
    const float ll = dot(ab, ab);
    const float t = ll > 1e-8f ? std::clamp(dot(sub(p, a), ab) / ll, 0.f, 1.f) : 0.f;
    return len(sub(p, add(a, mul(ab, t))));
}
// The segments p0-p1 and q0-q1 cross (touching counts).
bool segmentsCross(Vec2 p0, Vec2 p1, Vec2 q0, Vec2 q1) {
    const Vec2 r = sub(p1, p0), s = sub(q1, q0);
    const float den = cross(r, s);
    const Vec2 qp = sub(q0, p0);
    if (std::fabs(den) < 1e-8f) return false; // parallel: the distance test catches a wall along the line
    const float t = cross(qp, s) / den, u = cross(qp, r) / den;
    return t >= 0.f && t <= 1.f && u >= 0.f && u <= 1.f;
}

struct Pick {
    bool found = false;
    float cost = 0.f;
    Vec2 point{};
    std::int8_t flank = 0;
    float angle = 0.f;
    float offEdge = 0.f;
};

bool legal(const PartnerCloseInput& in, Vec2 p, float& ndcX) {
    if (!partnerCloseNdc(in.cameraPos, in.cameraForward, in.halfAngleDegrees, p, ndcX)) return false;
    if (std::fabs(ndcX) > 1.f - 2.f * kPartnerCloseConeMargin) return false;
    const float fromLead = len(sub(p, in.lead));
    if (fromLead < in.personalSpace) return false;
    if (in.leashMeters > 0.f && fromLead > in.leashMeters) return false;
    if (in.arenaRadius > 0.f && len(sub(p, in.arenaCentre)) > in.arenaRadius) return false;
    if (partnerCloseOverlaps(in.cameraPos, p, in.bodyRadius, in.lead, in.bodyRadius)) return false;
    if (partnerCloseHiddenBehind(in.cameraPos, p, in.target, in.targetRadius)) return false;
    for (const PartnerBody& b : in.bodies)
        if (len(sub(p, b.pos)) < b.radius + in.bodyRadius) return false;
    for (const PartnerWall& w : in.walls) {
        if (distToSegment(p, w.a, w.b) < w.halfThickness + in.bodyRadius) return false;
        if (segmentsCross(in.cameraPos, p, w.a, w.b)) return false;
    }
    return true;
}

} // namespace

bool partnerCloseWhole(const PartnerCloseInput& in) {
    if (!fin(in.lead) || !fin(in.target) || !fin(in.cameraPos) || !fin(in.cameraForward) || !fin(in.partner) ||
        !fin(in.arenaCentre))
        return false;
    if (!within(in.targetRadius, 0.f, 50.f) || !within(in.halfAngleDegrees, 1.f, 85.f) ||
        !within(in.standMin, 0.f, 30.f) || !within(in.standMax, in.standMin, 30.f) ||
        !within(in.bodyRadius, 0.1f, 1.5f) || !within(in.personalSpace, 0.f, 20.f) ||
        !within(in.leashMeters, 0.f, 200.f) || !within(in.arenaRadius, 0.f, 1000.f))
        return false;
    if (in.leadSide < -1 || in.leadSide > 1) return false;
    for (const PartnerWall& w : in.walls)
        if (!fin(w.a) || !fin(w.b) || !within(w.halfThickness, 0.f, 10.f)) return false;
    for (const PartnerBody& b : in.bodies)
        if (!fin(b.pos) || !within(b.radius, 0.f, 50.f)) return false;
    return true;
}

bool partnerCloseNdc(Vec2 cameraPos, Vec2 cameraForward, float halfAngleDegrees, Vec2 p, float& ndcX) {
    Vec2 f;
    if (!unitOf(cameraForward, f) || !within(halfAngleDegrees, 1.f, 85.f) || !fin(p) || !fin(cameraPos)) return false;
    const Vec2 r = sub(p, cameraPos);
    const float depth = dot(r, f);
    if (!(depth > kPartnerCloseNear)) return false;
    ndcX = dot(r, rightOf(f)) / (depth * std::tan(halfAngleDegrees * kDeg));
    return true;
}

bool partnerCloseInCone(Vec2 cameraPos, Vec2 cameraForward, float halfAngleDegrees, float margin, Vec2 p) {
    float x = 0.f;
    if (!within(margin, 0.f, 0.49f) || !partnerCloseNdc(cameraPos, cameraForward, halfAngleDegrees, p, x)) return false;
    return std::fabs(x) <= 1.f - 2.f * margin;
}

bool partnerCloseOverlaps(Vec2 cameraPos, Vec2 p, float pr, Vec2 b, float br) {
    const float dp = len(sub(p, cameraPos)), db = len(sub(b, cameraPos));
    if (dp <= pr || db <= br) return true; // the lens inside a body: it covers the view
    return apart(cameraPos, p, b) < halfAngle(dp, pr) + halfAngle(db, br);
}

bool partnerCloseHiddenBehind(Vec2 cameraPos, Vec2 p, Vec2 b, float br) {
    const float dp = len(sub(p, cameraPos)), db = len(sub(b, cameraPos));
    if (db <= br) return true;
    return dp > db && apart(cameraPos, p, b) < halfAngle(db, br);
}

PartnerCloseStation partnerCloseStation(const PartnerCloseInput& in, PartnerCloseHold& hold) {
    PartnerCloseStation out;
    if (!partnerCloseWhole(in)) return out;
    // The lead's bearing from the target; the lead on the target: the lens's.
    Vec2 bearing;
    if (!unitOf(sub(in.lead, in.target), bearing) && !unitOf(sub(in.cameraPos, in.target), bearing)) bearing = {1.f, 0.f};
    float leadX = 0.f;
    const bool leadSeen = partnerCloseNdc(in.cameraPos, in.cameraForward, in.halfAngleDegrees, in.lead, leadX);
    const bool keep = hold.flank != 0 && (hold.heldFor < kPartnerCloseHoldSeconds || in.attacking);
    const float lo = in.standMin, hi = in.standMax;
    const float distFromLens = len(sub(in.lead, in.cameraPos));
    const float leadHalf = halfAngle(distFromLens, in.bodyRadius);
    const int steps = static_cast<int>(std::lround((kPartnerCloseAngleMax - kPartnerCloseAngleMin) / kPartnerCloseAngleStep));
    Pick best;
    for (std::int8_t flank : {std::int8_t{-1}, std::int8_t{1}}) {
        if (keep && flank != hold.flank) continue;
        for (int i = 0; i <= steps; ++i) {
            const float angle = kPartnerCloseAngleMin + static_cast<float>(i) * kPartnerCloseAngleStep;
            const Vec2 dir = turn(bearing, static_cast<float>(flank) * angle);
            const float nearest = std::clamp(dot(sub(in.partner, in.target), dir) - in.targetRadius, lo, hi);
            const float offs[4] = {nearest, lo, 0.5f * (lo + hi), hi};
            for (const float off : offs) {
                const Vec2 p = add(in.target, mul(dir, in.targetRadius + off));
                float x = 0.f;
                if (!legal(in, p, x)) continue;
                const float dp = len(sub(p, in.cameraPos));
                const float clear = apart(in.cameraPos, p, in.lead) / (halfAngle(dp, in.bodyRadius) + leadHalf);
                float cost = len(sub(p, in.partner)) - kPartnerCloseClearWeight * std::min(clear, 3.f);
                if (in.leadSide != 0 && leadSeen && !(x * in.leadSide >= leadX * in.leadSide))
                    cost += kPartnerCloseSidePenalty;
                if (flank == hold.flank && std::fabs(angle - hold.angle) < 1e-3f) cost -= kPartnerCloseKeepBonus;
                if (!best.found || cost < best.cost) best = {true, cost, p, flank, angle, off};
            }
        }
    }
    if (!best.found) {
        if (keep) { // the flank it must keep has no legal point: where it would stand, not valid
            const Vec2 dir = turn(bearing, static_cast<float>(hold.flank) * hold.angle);
            const float off = std::clamp(dot(sub(in.partner, in.target), dir) - in.targetRadius, lo, hi);
            out.point = add(in.target, mul(dir, in.targetRadius + off));
            out.flank = hold.flank;
            out.angle = hold.angle;
            out.offEdge = off;
            out.held = true;
        }
        return out;
    }
    if (best.flank != hold.flank) hold.heldFor = 0.0;
    hold.flank = best.flank;
    hold.angle = best.angle;
    out.valid = true;
    out.point = best.point;
    out.flank = best.flank;
    out.angle = best.angle;
    out.offEdge = best.offEdge;
    out.held = keep;
    return out;
}

} // namespace ftcore
