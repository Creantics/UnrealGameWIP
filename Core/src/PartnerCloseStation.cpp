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

// The body tests every tier shares: outside the target, the lead's personal space kept, within the leash and the
// arena, clear of the other bodies and the walls.
bool roomFor(const PartnerCloseInput& in, Vec2 p) {
    // Never inside the target's own body (the lead-ring points of tier 3 are placed round the lead, not its edge).
    if (len(sub(p, in.target)) < in.targetRadius + in.bodyRadius) return false;
    const float fromLead = len(sub(p, in.lead));
    if (fromLead < in.personalSpace) return false;
    if (in.leashMeters > 0.f && fromLead > in.leashMeters) return false;
    if (in.arenaRadius > 0.f && len(sub(p, in.arenaCentre)) > in.arenaRadius) return false;
    for (const PartnerBody& b : in.bodies)
        if (len(sub(p, b.pos)) < b.radius + in.bodyRadius) return false;
    for (const PartnerWall& w : in.walls)
        if (distToSegment(p, w.a, w.b) < w.halfThickness + in.bodyRadius) return false;
    return true;
}

// The near field's rules 1 and 2 (rule 2 only with capNow); off without nearField.
bool nearFieldOk(const PartnerCloseInput& in, Vec2 p, bool capNow) {
    if (!in.nearField) return true;
    if (!partnerCloseNearOk(in.cameraPos, in.cameraForward, in.lead, p)) return false;
    return !capNow || !(in.verticalHalfDegrees > 0.f) ||
           partnerCloseProjectedHeight(in.cameraPos, in.cameraForward, in.verticalHalfDegrees, p) <= kPartnerCloseSizeCap;
}

// p's silhouette clear of the lead's, with gapBodies of p's own body widths of screen gap between them (0: only clear).
bool clearOfLead(const PartnerCloseInput& in, Vec2 p, float gapBodies) {
    return partnerCloseClearOfLead(in.cameraPos, p, in.lead, in.bodyRadius, gapBodies);
}

bool legal(const PartnerCloseInput& in, Vec2 p, bool capNow, float gap, float& ndcX, float& ndcY) {
    if (!partnerCloseLens(in, p, ndcX, ndcY)) return false;
    if (!nearFieldOk(in, p, capNow)) return false;
    if (in.sees != nullptr && !in.sees(in.seesContext, p)) return false;
    if (std::fabs(ndcX) > 1.f - 2.f * kPartnerCloseConeMargin) return false;
    Vec2 f;
    if (!unitOf(in.cameraForward, f)) return false;
    const Vec2 r = sub(p, in.cameraPos);
    const float ahead = dot(r, f);
    if (ahead < in.minAhead) return false;
    if (in.aimConeDegrees > 0.f && std::fabs(dot(r, rightOf(f))) > ahead * std::tan(in.aimConeDegrees * kDeg))
        return false;
    if (!roomFor(in, p)) return false;
    if (!clearOfLead(in, p, gap)) return false;
    if (partnerCloseHiddenBehind(in.cameraPos, p, in.target, in.targetRadius)) return false;
    for (const PartnerWall& w : in.walls)
        if (segmentsCross(in.cameraPos, p, w.a, w.b)) return false;
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
        !within(in.lensHeight, -20.f, 50.f) || !within(in.aimHeight, 0.f, 5.f) || !within(in.leadGapBodies, 0.f, 3.f))
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
    if (!(dp > 0.f) || !(db > 0.f)) return true; // a body at the lens (a zero radius too): it covers the view
    if (dp <= pr || db <= br) return true; // the lens inside a body: it covers the view
    return apart(cameraPos, p, b) < halfAngle(dp, pr) + halfAngle(db, br);
}

bool partnerCloseHiddenBehind(Vec2 cameraPos, Vec2 p, Vec2 b, float br) {
    const float dp = len(sub(p, cameraPos)), db = len(sub(b, cameraPos));
    if (db <= br) return true;
    return dp > db && apart(cameraPos, p, b) < halfAngle(db, br);
}

bool partnerCloseClearOfLead(Vec2 cameraPos, Vec2 p, Vec2 lead, float bodyRadius, float gapBodies) {
    if (partnerCloseOverlaps(cameraPos, p, bodyRadius, lead, bodyRadius)) return false;
    if (!(gapBodies > 0.f)) return true;
    const float dp = len(sub(p, cameraPos)), dl = len(sub(lead, cameraPos));
    if (!(dp > 0.f) || !(dl > 0.f)) return false; // a body at the lens: it covers the view, never clear (review fix 5)
    return apart(cameraPos, p, lead) >= halfAngle(dl, bodyRadius) + (1.f + 2.f * gapBodies) * halfAngle(dp, bodyRadius);
}

bool partnerCloseNearOk(Vec2 cameraPos, Vec2 cameraForward, Vec2 lead, Vec2 p) {
    Vec2 f;
    if (!unitOf(cameraForward, f) || !fin(cameraPos) || !fin(lead) || !fin(p)) return false;
    if (dot(sub(p, cameraPos), f) < dot(sub(lead, cameraPos), f) - kPartnerCloseNearBehindLead) return false;
    return len(sub(p, cameraPos)) >= kPartnerCloseNearLens;
}

float partnerCloseProjectedHeight(Vec2 cameraPos, Vec2 cameraForward, float verticalHalfDegrees, Vec2 p) {
    constexpr float kHuge = 1000.f;
    Vec2 f;
    if (!unitOf(cameraForward, f) || !within(verticalHalfDegrees, 1.f, 85.f) || !fin(cameraPos) || !fin(p)) return kHuge;
    const float depth = dot(sub(p, cameraPos), f);
    if (!(depth > kPartnerCloseNear)) return kHuge;
    return std::min(kHuge, kPartnerCloseBodyTall / (2.f * depth * std::tan(verticalHalfDegrees * kDeg)));
}

PartnerCloseStation partnerCloseStation(const PartnerCloseInput& in, PartnerCloseHold& hold) {
    PartnerCloseStation out;
    if (!partnerCloseWhole(in)) return out;
    // The lead's bearing from the target; the lead on the target: the lens's.
    Vec2 bearing;
    if (!unitOf(sub(in.lead, in.target), bearing) && !unitOf(sub(in.cameraPos, in.target), bearing)) bearing = {1.f, 0.f};
    float leadX = 0.f, leadY = 0.f;
    const bool leadSeen = partnerCloseLens(in, in.lead, leadX, leadY);
    // A melee partner holds a flank longer (kPartnerCloseMeleeHoldSeconds): its far flank is the side with the frame's
    // room, and a shorter hold let it flip as the lead's screen x wavered about the centre.
    const double holdFor = in.farFlankFirst ? kPartnerCloseMeleeHoldSeconds : kPartnerCloseHoldSeconds;
    const bool keep = hold.flank != 0 && (hold.heldFor < holdFor || in.attacking);
    const float lo = in.standMin, hi = in.standMax;
    // A melee partner in a string: no new pick, the held angle where it stands off the target's edge.
    if (in.keepWhileAttacking && in.attacking && hold.flank != 0 && !hold.ring) {
        const Vec2 dir = turn(bearing, static_cast<float>(hold.flank) * hold.angle);
        const float off =
            std::clamp(dot(sub(in.partner, in.target), dir) - in.targetRadius, in.bodyRadius, std::max(hi, in.bodyRadius));
        const Vec2 p = add(in.target, mul(dir, in.targetRadius + off));
        // In the frame too, with the cone's margin (the lead's turn or the target's step can carry the held angle out
        // of it): else a new pick below, the hold kept where that flank has one.
        float hx = 0.f, hy = 0.f;
        const bool framed = partnerCloseLens(in, p, hx, hy) && std::fabs(hx) <= 1.f - 2.f * kPartnerCloseConeMargin &&
                            (in.sees == nullptr || in.sees(in.seesContext, p));
        if (framed && roomFor(in, p) && clearOfLead(in, p, 0.f)) {
            out.valid = true;
            out.tier = 1;
            out.point = p;
            out.flank = hold.flank;
            out.angle = hold.angle;
            out.offEdge = off;
            out.held = true;
            out.gapped = in.leadGapBodies > 0.f && clearOfLead(in, p, in.leadGapBodies);
            return out;
        }
    }
    const float distFromLens = len(sub(in.lead, in.cameraPos));
    const float leadHalf = halfAngle(distFromLens, in.bodyRadius);
    const bool wide = in.targetRadius >= kPartnerCloseWideRadius;
    const float angleMin = wide ? kPartnerCloseWideAngleMin : kPartnerCloseAngleMin;
    const float angleMax = wide ? kPartnerCloseWideAngleMax : kPartnerCloseAngleMax;
    constexpr int kSteps = 8; // 5 degrees either way
    // Tier 4's two extra tests: the far point is clear of the target's whole silhouette from the lens, and the straight
    // walk to it never enters the lead's or the target's shadow (deeper than the body, over its silhouette) from a
    // start clear of both.
    const float targetFromLens = len(sub(in.target, in.cameraPos));
    const float targetHalf = halfAngle(targetFromLens, in.targetRadius);
    const auto shadowed = [&](Vec2 q) {
        const float dq = len(sub(q, in.cameraPos)), own = halfAngle(dq, in.bodyRadius);
        return (dq > distFromLens && apart(in.cameraPos, q, in.lead) < own + leadHalf) ||
               (dq > targetFromLens && apart(in.cameraPos, q, in.target) < own + targetHalf);
    };
    const bool startShadowed = shadowed(in.partner);
    bool farArc = false;
    // The near field: rule 2 held in the first pass (capNow), the lead's depth for rule 3's preference.
    const bool capGiven = in.nearField && in.verticalHalfDegrees > 0.f;
    bool capNow = capGiven;
    Vec2 lensForward{1.f, 0.f};
    const bool forwardOk = unitOf(in.cameraForward, lensForward);
    const float leadDepth = dot(sub(in.lead, in.cameraPos), lensForward);
    const auto shallow = [&](Vec2 p) { return dot(sub(p, in.cameraPos), lensForward) <= leadDepth; };
    // A melee partner: the far flank, across the lens's axis from the lead (its lateral offset, else leadSide).
    float gapNow = 0.f;
    std::int8_t farFlank = 0;
    if (in.farFlankFirst && forwardOk) {
        const Vec2 right = rightOf(lensForward);
        // The frame's room: the lead left of the centre on screen, the right flank; the reverse (its screen x, else
        // its lateral offset from the axis, else leadSide).
        const float leadLateral = leadSeen ? leadX : dot(sub(in.lead, in.cameraPos), right);
        const float leadEps = leadSeen ? 0.02f : 0.05f;
        const float leadSign =
            std::fabs(leadLateral) > leadEps ? (leadLateral > 0.f ? 1.f : -1.f) : static_cast<float>(in.leadSide);
        if (leadSign != 0.f) {
            const float mid = 0.5f * (angleMin + angleMax), reachOut = in.targetRadius + 0.5f * (lo + hi);
            const float minus = dot(sub(add(in.target, mul(turn(bearing, -mid), reachOut)), in.cameraPos), right);
            const float plus = dot(sub(add(in.target, mul(turn(bearing, mid), reachOut)), in.cameraPos), right);
            farFlank = minus * leadSign < plus * leadSign ? std::int8_t{-1} : std::int8_t{1};
        }
    }
    const auto farClear = [&](Vec2 p, bool crossing) {
        if (partnerCloseOverlaps(in.cameraPos, p, in.bodyRadius, in.target, in.targetRadius)) return false;
        if (startShadowed || crossing) return true; // crossing: a melee partner's far flank (no walk test)
        constexpr int kWalkSamples = 8;
        for (int k = 1; k <= kWalkSamples; ++k)
            if (shadowed(add(in.partner, mul(sub(p, in.partner), static_cast<float>(k) / static_cast<float>(kWalkSamples)))))
                return false;
        return true;
    };
    // The near field's gapped pass: a walk that would cross the lead's silhouette (the partner clear of it now) is no
    // station; the partner keeps its own side.
    const bool startHiddenByLead = !partnerCloseClearOfLead(in.cameraPos, in.partner, in.lead, in.bodyRadius, 0.f);
    const auto walkCrossesLead = [&](Vec2 p) {
        if (startHiddenByLead) return false;
        constexpr int kWalkSamples = 8;
        for (int k = 1; k < kWalkSamples; ++k) {
            const Vec2 q = add(in.partner, mul(sub(p, in.partner), static_cast<float>(k) / static_cast<float>(kWalkSamples)));
            if (!partnerCloseClearOfLead(in.cameraPos, q, in.lead, in.bodyRadius, 0.f)) return true;
        }
        return false;
    };
    // A legal p's score: the walk, the lead clearance, the lower two thirds and the lead's side (no hold terms).
    const auto score = [&](Vec2 p, std::int8_t flank, float& cost) {
        float x = 0.f, y = 0.f;
        if (!legal(in, p, capNow, gapNow, x, y)) return false;
        if (in.nearField && gapNow > 0.f && walkCrossesLead(p)) return false;
        if (farArc && !farClear(p, farFlank != 0 && flank == farFlank)) return false;
        const float dp = len(sub(p, in.cameraPos));
        const float clear = apart(in.cameraPos, p, in.lead) / (halfAngle(dp, in.bodyRadius) + leadHalf);
        cost = len(sub(p, in.partner)) - kPartnerCloseClearWeight * std::min(clear, 3.f) +
               kPartnerCloseVerticalWeight * verticalOut(y);
        if (in.leadSide != 0 && leadSeen && !(x * in.leadSide >= leadX * in.leadSide)) cost += kPartnerCloseSidePenalty;
        if (in.nearField) {
            if (shallow(p)) cost += kPartnerCloseShallowPenalty;
            cost += kPartnerCloseEdgeWeight * std::max(0.f, std::fabs(x) - kPartnerCloseEdgeNdc);
        }
        return true;
    };
    // Tiers 1-2 (or 4) on one flank between aMin and aMax degrees: the band's best, else the farthest legal point
    // short of the band.
    bool bandOnly = false; // the near field's gapped pass: band points only, no short pick
    const auto onFlank = [&](std::int8_t flank, float aMin, float aMax) {
        Pick band, shortPick;
        for (int i = 0; i <= kSteps; ++i) {
            const float angle = aMin + (aMax - aMin) * static_cast<float>(i) / static_cast<float>(kSteps);
            const Vec2 dir = turn(bearing, static_cast<float>(flank) * angle);
            const float keepBonus =
                !hold.ring && flank == hold.flank && std::fabs(angle - hold.angle) < 1e-3f ? kPartnerCloseKeepBonus : 0.f;
            const float nearest = std::clamp(dot(sub(in.partner, in.target), dir) - in.targetRadius, lo, hi);
            const float offs[4] = {nearest, lo, 0.5f * (lo + hi), hi};
            for (const float off : offs) {
                const Vec2 p = add(in.target, mul(dir, in.targetRadius + off));
                float cost = 0.f;
                if (!score(p, flank, cost)) continue;
                cost -= keepBonus;
                if (!band.found || cost < band.cost) band = {true, cost, p, flank, angle, off};
            }
            if (band.found || !(lo > 0.f) || bandOnly) continue; // the short pass only while the band has none here
            for (int j = 1; j <= kSteps; ++j) {
                const float off = lo * (1.f - static_cast<float>(j) / static_cast<float>(kSteps));
                if (off < in.bodyRadius) break; // never inside the target's own body
                const Vec2 p = add(in.target, mul(dir, in.targetRadius + off));
                float cost = 0.f;
                if (!score(p, flank, cost)) continue;
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
    // Both flanks' picks between aMin and aMax (the held flank only while it is kept), and the better of the two.
    const auto sweep = [&](float aMin, float aMax, bool kept) {
        Pick picks[2];
        for (const std::int8_t flank : {std::int8_t{-1}, std::int8_t{1}})
            if (!kept || flank == hold.flank) picks[flank > 0 ? 1 : 0] = onFlank(flank, aMin, aMax);
        if (!kept && farFlank != 0 && picks[farFlank > 0 ? 1 : 0].found) return picks[farFlank > 0 ? 1 : 0];
        Pick chosen;
        if (picks[0].found && picks[1].found && hold.flank != 0) {
            const Pick& held = picks[hold.flank > 0 ? 1 : 0];
            const Pick& other = picks[hold.flank > 0 ? 0 : 1];
            chosen = other.cost + kPartnerCloseFlipMargin < held.cost ? other : held;
        } else {
            for (const Pick& q : picks)
                if (q.found && (!chosen.found || q.cost < chosen.cost)) chosen = q;
        }
        return chosen;
    };
    // Tier 5 (the near field): round the whole target, no screen test; the frame, the clear view and the depth past
    // the lead scored in that order.
    const auto outOfFrame = [&]() {
        Pick o;
        if (!forwardOk) return o;
        constexpr int kOutSteps = static_cast<int>(180.f / kPartnerCloseOutAngleStep + 0.5f);
        const float inside = 1.f - 2.f * kPartnerCloseConeMargin;
        for (const std::int8_t flank : {std::int8_t{-1}, std::int8_t{1}}) {
            for (int i = 0; i <= kOutSteps; ++i) {
                const float angle = kPartnerCloseOutAngleStep * static_cast<float>(i);
                const Vec2 dir = turn(bearing, static_cast<float>(flank) * angle);
                const float nearest = std::clamp(dot(sub(in.partner, in.target), dir) - in.targetRadius, lo, hi);
                const float offs[4] = {nearest, lo, 0.5f * (lo + hi), hi};
                for (const float off : offs) {
                    const Vec2 p = add(in.target, mul(dir, in.targetRadius + off));
                    if (!roomFor(in, p) || !nearFieldOk(in, p, capNow)) continue;
                    float x = 0.f, y = 0.f;
                    const bool lensOk = partnerCloseLens(in, p, x, y);
                    const bool framed =
                        in.sees != nullptr ? in.sees(in.seesContext, p) : lensOk && std::fabs(x) <= inside;
                    bool hidden = !clearOfLead(in, p, gapNow) ||
                                  partnerCloseHiddenBehind(in.cameraPos, p, in.target, in.targetRadius);
                    for (const PartnerWall& w : in.walls)
                        if (segmentsCross(in.cameraPos, p, w.a, w.b)) hidden = true;
                    const float outBy = lensOk ? std::max(0.f, std::fabs(x) - inside) + verticalOut(y) : 3.f;
                    float cost = (framed ? 0.f : 100.f) + (hidden ? 50.f : 0.f) + (shallow(p) ? 20.f : 0.f) +
                                 10.f * std::min(outBy, 3.f) + len(sub(p, in.partner));
                    if (hold.flank != 0 && flank != hold.flank) cost += kPartnerCloseFlipMargin;
                    if (!o.found || cost < o.cost) o = {true, cost, p, flank, angle, off};
                }
            }
        }
        return o;
    };
    Pick best;
    std::uint8_t tier = 0;
    bool ring = false;
    // Without the near field one pass, as before; with it, the cap held, then (nothing found) the soft pass. A lead
    // gap given: those with it, then (nothing found) those without it.
    const int passes = in.leadGapBodies > 0.f ? 4 : 2;
    for (int pass = 0; pass < passes && !best.found; ++pass) {
        if (pass % 2 == 1 && !capGiven) continue;
        capNow = capGiven && pass % 2 == 0;
        gapNow = pass < 2 ? in.leadGapBodies : 0.f;
        farArc = false;
        // The near field with the lead gap: the band's points only (tier 1); none, and the passes without the gap
        // follow (a short or a far-arc point with the gap stood the partner behind the lead as the lead circled).
        const bool gappedNear = in.nearField && gapNow > 0.f;
        bandOnly = gappedNear;
        best = sweep(angleMin, angleMax, keep);
        bandOnly = false;
        if (best.found) tier = best.offEdge >= lo - 1e-5f ? 1 : 2;
        if (gappedNear) continue;
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
                        if (!score(p, std::int8_t{0}, cost)) continue;
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
        if (!best.found && !wide) { // tier 4: the lead hugging its target, the near flanks all fail: the far arc
            farArc = true;
            best = sweep(kPartnerCloseFarAngleMin, kPartnerCloseFarAngleMax, keep);
            // The kept flank's far arc has nothing either (the partner held on the side the lead's body closes): the
            // hold gives way to the open flank, since the fallback would stand it in the lead's shadow.
            if (!best.found && keep) best = sweep(kPartnerCloseFarAngleMin, kPartnerCloseFarAngleMax, false);
            if (best.found) tier = 4;
        }
        if (!best.found && in.nearField) {
            best = outOfFrame();
            if (best.found) tier = 5;
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
    const std::int8_t heldFlank = hold.flank;
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
    out.held = keep && best.flank == heldFlank; // a released hold (tier 4) did not keep it
    out.capped = capNow;
    out.gapped = gapNow > 0.f && clearOfLead(in, best.point, gapNow);
    return out;
}

} // namespace ftcore
