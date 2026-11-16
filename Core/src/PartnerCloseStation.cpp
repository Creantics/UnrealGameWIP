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

// How far an ndc y falls outside the lower two thirds of the frame (0 inside).
float verticalOut(float ndcY) {
    const float bottom = -(1.f - 2.f * kPartnerCloseConeMargin);
    return ndcY < bottom ? bottom - ndcY : (ndcY > kPartnerCloseTopNdc ? ndcY - kPartnerCloseTopNdc : 0.f);
}

bool legal(const PartnerCloseInput& in, Vec2 p, float& ndcX, float& ndcY) {
    if (!partnerCloseLens(in, p, ndcX, ndcY)) return false;
    if (in.sees != nullptr && !in.sees(in.seesContext, p)) return false;
    if (std::fabs(ndcX) > 1.f - 2.f * kPartnerCloseConeMargin) return false;
    Vec2 f;
    if (!unitOf(in.cameraForward, f)) return false;
    const Vec2 r = sub(p, in.cameraPos);
    const float ahead = dot(r, f);
    if (ahead < in.minAhead) return false;
    if (in.aimConeDegrees > 0.f && std::fabs(dot(r, rightOf(f))) > ahead * std::tan(in.aimConeDegrees * kDeg))
        return false;
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
        !within(in.leashMeters, 0.f, 200.f) || !within(in.arenaRadius, 0.f, 1000.f) ||
        !within(in.aimConeDegrees, 0.f, 85.f) || !within(in.minAhead, 0.f, 20.f) ||
        !within(in.pitchDegrees, -85.f, 85.f) || !within(in.verticalHalfDegrees, 0.f, 85.f) ||
        !within(in.lensHeight, -20.f, 50.f) || !within(in.aimHeight, 0.f, 5.f))
        return false;
    if (in.verticalHalfDegrees > 0.f && in.verticalHalfDegrees < 1.f) return false;
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

bool partnerCloseLens(const PartnerCloseInput& in, Vec2 p, float& ndcX, float& ndcY) {
    ndcY = 0.f;
    if (!(in.verticalHalfDegrees > 0.f))
        return partnerCloseNdc(in.cameraPos, in.cameraForward, in.halfAngleDegrees, p, ndcX);
    Vec2 f;
    if (!unitOf(in.cameraForward, f) || !within(in.halfAngleDegrees, 1.f, 85.f) ||
        !within(in.verticalHalfDegrees, 1.f, 85.f) || !within(in.pitchDegrees, -85.f, 85.f) || !fin(p) ||
        !fin(in.cameraPos) || !std::isfinite(in.lensHeight) || !std::isfinite(in.aimHeight))
        return false;
    const Vec2 r = sub(p, in.cameraPos);
    const float ground = dot(r, f), up = in.aimHeight - in.lensHeight;
    const float c = std::cos(in.pitchDegrees * kDeg), s = std::sin(in.pitchDegrees * kDeg);
    const float depth = ground * c + up * s;
    if (!(depth > kPartnerCloseNear)) return false;
    ndcX = dot(r, rightOf(f)) / (depth * std::tan(in.halfAngleDegrees * kDeg));
    ndcY = (up * c - ground * s) / (depth * std::tan(in.verticalHalfDegrees * kDeg));
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
    float leadX = 0.f, leadY = 0.f;
    const bool leadSeen = partnerCloseLens(in, in.lead, leadX, leadY);
    const bool keep = hold.flank != 0 && (hold.heldFor < kPartnerCloseHoldSeconds || in.attacking);
    const float lo = in.standMin, hi = in.standMax;
    const float distFromLens = len(sub(in.lead, in.cameraPos));
    const float leadHalf = halfAngle(distFromLens, in.bodyRadius);
    const bool wide = in.targetRadius >= kPartnerCloseWideRadius;
    const float angleMin = wide ? kPartnerCloseWideAngleMin : kPartnerCloseAngleMin;
    const float angleMax = wide ? kPartnerCloseWideAngleMax : kPartnerCloseAngleMax;
    constexpr int kSteps = 8; // 5 degrees either way
    // A legal p's score: the walk, the lead clearance, the lower two thirds and the lead's side (no hold terms).
    const auto score = [&](Vec2 p, float& cost) {
        float x = 0.f, y = 0.f;
        if (!legal(in, p, x, y)) return false;
        const float dp = len(sub(p, in.cameraPos));
        const float clear = apart(in.cameraPos, p, in.lead) / (halfAngle(dp, in.bodyRadius) + leadHalf);
        cost = len(sub(p, in.partner)) - kPartnerCloseClearWeight * std::min(clear, 3.f) +
               kPartnerCloseVerticalWeight * verticalOut(y);
        if (in.leadSide != 0 && leadSeen && !(x * in.leadSide >= leadX * in.leadSide)) cost += kPartnerCloseSidePenalty;
        return true;
    };
    // Tiers 1-2 on one flank: the band's best, else the farthest legal point short of the band.
    const auto onFlank = [&](std::int8_t flank) {
        Pick band, shortPick;
        for (int i = 0; i <= kSteps; ++i) {
            const float angle = angleMin + (angleMax - angleMin) * static_cast<float>(i) / static_cast<float>(kSteps);
            const Vec2 dir = turn(bearing, static_cast<float>(flank) * angle);
            const float keepBonus =
                !hold.ring && flank == hold.flank && std::fabs(angle - hold.angle) < 1e-3f ? kPartnerCloseKeepBonus : 0.f;
            const float nearest = std::clamp(dot(sub(in.partner, in.target), dir) - in.targetRadius, lo, hi);
            const float offs[4] = {nearest, lo, 0.5f * (lo + hi), hi};
            for (const float off : offs) {
                const Vec2 p = add(in.target, mul(dir, in.targetRadius + off));
                float cost = 0.f;
                if (!score(p, cost)) continue;
                cost -= keepBonus;
                if (!band.found || cost < band.cost) band = {true, cost, p, flank, angle, off};
            }
            if (band.found || !(lo > 0.f)) continue; // the short pass only while the band has none on this flank
            for (int j = 1; j <= kSteps; ++j) {
                const float off = lo * (1.f - static_cast<float>(j) / static_cast<float>(kSteps));
                const Vec2 p = add(in.target, mul(dir, in.targetRadius + off));
                float cost = 0.f;
                if (!score(p, cost)) continue;
                cost += kPartnerCloseShortPenalty - keepBonus;
                if (!shortPick.found || off > shortPick.offEdge + 1e-5f ||
                    (off > shortPick.offEdge - 1e-5f && cost < shortPick.cost))
                    shortPick = {true, cost, p, flank, angle, off};
                break; // the farthest legal one at this angle
            }
        }
        if (band.found) return band;
        return shortPick;
    };
    Pick picks[2];
    for (const std::int8_t flank : {std::int8_t{-1}, std::int8_t{1}})
        if (!keep || flank == hold.flank) picks[flank > 0 ? 1 : 0] = onFlank(flank);
    Pick best;
    std::uint8_t tier = 0;
    if (picks[0].found && picks[1].found && hold.flank != 0) {
        const Pick& held = picks[hold.flank > 0 ? 1 : 0];
        const Pick& other = picks[hold.flank > 0 ? 0 : 1];
        best = other.cost + kPartnerCloseFlipMargin < held.cost ? other : held;
    } else {
        for (const Pick& q : picks)
            if (q.found && (!best.found || q.cost < best.cost)) best = q;
    }
    if (best.found) tier = best.offEdge >= lo - 1e-5f ? 1 : 2;
    bool ring = false;
    if (!best.found && in.leashMeters > 0.f) { // tier 3: within the leash, round the lead toward the target's flanks
        Vec2 toTarget;
        if (!unitOf(sub(in.target, in.lead), toTarget)) toTarget = mul(bearing, -1.f);
        const float radii[2] = {in.leashMeters - 0.25f, 0.5f * (in.leashMeters + in.personalSpace)};
        for (const std::int8_t flank : {std::int8_t{-1}, std::int8_t{1}}) {
            if (keep && flank != hold.flank) continue;
            for (int i = 0; i <= kSteps; ++i) {
                const float angle =
                    kPartnerCloseRingMin + (kPartnerCloseRingMax - kPartnerCloseRingMin) * static_cast<float>(i) / 8.f;
                const Vec2 dir = turn(toTarget, -static_cast<float>(flank) * angle); // flank +1 as round the target
                for (const float r : radii) {
                    if (!(r > 0.f)) continue;
                    const Vec2 p = add(in.lead, mul(dir, r));
                    float cost = 0.f;
                    if (!score(p, cost)) continue;
                    if (hold.flank != 0 && flank != hold.flank) cost += kPartnerCloseFlipMargin;
                    if (!best.found || cost < best.cost)
                        best = {true, cost, p, flank, angle, len(sub(p, in.target)) - in.targetRadius};
                }
            }
        }
        if (best.found) {
            tier = 3;
            ring = true;
        }
    }
    if (!best.found) {
        if (keep && !hold.ring) { // the flank it must keep has no legal point: where it would stand, not valid
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
    hold.ring = ring;
    out.valid = true;
    out.tier = tier;
    out.point = best.point;
    out.flank = best.flank;
    out.angle = best.angle;
    out.offEdge = best.offEdge;
    out.held = keep;
    return out;
}

} // namespace ftcore
