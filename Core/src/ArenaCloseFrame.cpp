#include "ftcore/ArenaCloseFrame.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {
constexpr float kRad = 3.14159265358979f / 180.f;

bool finite(float v) { return std::isfinite(v); }

float clamp01(float v) { return std::min(1.f, std::max(0.f, v)); }

float lerp(float a, float b, float t) { return a + (b - a) * t; }

// A share or blend as given, or 0 when it is NaN, held to 0..1.
float shareOf(float v) { return finite(v) ? clamp01(v) : 0.f; }
} // namespace

float arenaCloseMeleeWant(const ArenaCloseRules& rules, float distance) {
    if (!finite(distance) || distance < 0.f) return 0.f;
    if (distance <= rules.meleeTo) return 1.f;
    if (distance >= rules.farFrom || !(rules.farFrom > rules.meleeTo)) return 0.f;
    const float t = (rules.farFrom - distance) / (rules.farFrom - rules.meleeTo);
    return t * t * (3.f - 2.f * t);
}

float arenaCloseMeleeStep(const ArenaCloseRules& rules, float now, float want, float dt, bool hold) {
    if (!finite(now)) now = 0.f;
    if (hold || !finite(want) || !finite(dt) || !(dt > 0.f)) return now;
    if (!(rules.smoothSeconds > 0.f)) return want;
    const float k = 1.f - std::exp(-dt / rules.smoothSeconds);
    return now + (want - now) * k;
}

bool arenaCloseDodgeHolds(const ArenaCloseRules& rules, float dodgeAge) {
    return finite(dodgeAge) && dodgeAge >= 0.f && dodgeAge < rules.dodgeHold;
}

float arenaCloseTallness(const ArenaCloseRules& rules, float targetHeight) {
    if (!finite(targetHeight) || !(rules.tallTo > rules.tallFrom)) return 0.f;
    return clamp01((targetHeight - rules.tallFrom) / (rules.tallTo - rules.tallFrom));
}

ArenaCloseShape arenaCloseShape(const ArenaCloseRules& rules, const CameraPreset& base, float share, float tallness, float shoulder,
                                float leadHead) {
    const float m = shareOf(share);
    const float tall = shareOf(tallness);
    const float s = finite(shoulder) ? std::min(1.f, std::max(-1.f, shoulder)) : 1.f;
    ArenaCloseShape out;
    out.pivot = base.rig.pivot + m * rules.pivotUp;
    out.side = base.rig.side * s;
    out.fov = lerp(base.rig.fov, rules.fovMelee, m);
    out.screenX = 0.5f + s * (lerp(base.targetX, rules.targetXMelee, m) - 0.5f);
    const float cut = cameraPresetCut(base, leadHead);
    // Raised toward the shoulders on a tall target, never over them, never under the preset's own cut.
    const float raised = std::min(cut + m * tall * rules.cutUpTall, finite(leadHead) ? leadHead - rules.shoulderRoom : 0.f);
    out.cutAt = std::max(cut, raised);
    out.cutMin = lerp(base.cutMin, rules.cutMinMelee, m);
    return out;
}

float arenaCloseMeleePitch(const ArenaCloseRules& rules, const ArenaLookLimits& limits, float pivot, float arm, float up, float distance,
                           float targetHeight, float share, float farPitch) {
    if (!finite(share) || !(share > 0.f) || !finite(farPitch)) return farPitch;
    if (!finite(pivot) || !finite(arm) || !finite(up) || !finite(distance) || !finite(targetHeight)) return farPitch;
    const float look = arenaLookUpPitch(limits, pivot, arm, up, distance, targetHeight, farPitch);
    if (!finite(look)) return farPitch;
    const float cap = lerp(rules.pitchMelee, rules.pitchMeleeTall, arenaCloseTallness(rules, targetHeight));
    const float want = std::max(farPitch, std::min(look, cap));
    return lerp(farPitch, want, clamp01(share));
}

float arenaCloseShoulderPick(const ArenaCloseRules& rules, float roomRight, float roomLeft, bool partnerRight, bool partnerLeft,
                             float current) {
    const float keep = current < 0.f ? -1.f : 1.f;
    const float r = finite(roomRight) ? roomRight : 0.f;
    const float l = finite(roomLeft) ? roomLeft : 0.f;
    const bool rOpen = r >= rules.wallRoom, lOpen = l >= rules.wallRoom;
    if (rOpen != lOpen) return rOpen ? 1.f : -1.f;
    if (partnerRight != partnerLeft) return partnerRight ? 1.f : -1.f;
    if (r >= l + rules.roomFlip) return 1.f;
    if (l >= r + rules.roomFlip) return -1.f;
    return keep;
}

float arenaCloseShoulderStep(const ArenaCloseRules& rules, float now, float want, float dt) {
    const float to = want < 0.f ? -1.f : 1.f;
    if (!finite(now)) return to;
    if (!finite(dt) || !(dt > 0.f)) return now;
    if (!(rules.swapSeconds > 0.f)) return to;
    const float step = 2.f * dt / rules.swapSeconds;
    return now < to ? std::min(to, now + step) : std::max(to, now - step);
}

bool arenaCloseSwapping(float now, float want) {
    const float to = want < 0.f ? -1.f : 1.f;
    return finite(now) && now != to;
}

ArenaStickBasis arenaCloseStickBasis(const ArenaCloseRules& rules, ArenaStickBasis now, float viewYaw, bool swapping,
                                     bool stickHeld, float dt) {
    if (!finite(viewYaw)) return now;
    const ArenaStickBasis follow{viewYaw, 0.f, true, false};
    if (!now.set || !finite(now.yaw) || !stickHeld) return follow;
    if (swapping) return ArenaStickBasis{now.yaw, 0.f, true, true};
    if (!now.lag) return follow;
    const float gap = std::remainder(viewYaw - now.yaw, 360.f); // the short way round
    if (std::fabs(gap) < 1e-3f || !(rules.stickEase > 0.f)) return follow;
    const float rate = std::max(finite(now.rate) ? now.rate : 0.f, std::fabs(gap) / rules.stickEase);
    const float move = finite(dt) && dt > 0.f ? rate * dt : 0.f;
    if (move >= std::fabs(gap)) return follow;
    return ArenaStickBasis{now.yaw + (gap < 0.f ? -move : move), rate, true, true};
}

float arenaClosePartnerYaw(const ArenaCloseRules& rules, const ArenaLeadFrameRules& leadRules, const ArenaCameraPose& pose, float aspect,
                           const ArenaLeadBody& body, ArenaPoint3 partner, ArenaPoint3 target, float shoulder, float margin) {
    if (!finite(partner.x) || !finite(partner.y) || !finite(partner.z) || !finite(target.x) || !finite(target.y) || !finite(target.z))
        return 0.f;
    if (std::sqrt(partner.x * partner.x + partner.y * partner.y) > rules.partnerRange) return 0.f;
    const float lo = margin, hi = 1.f - margin;
    const ArenaFramePoint now = arenaFramePoint(pose, aspect, partner);
    if (now.ahead && now.x >= lo && now.x <= hi) return 0.f;
    const bool right = !(shoulder < 0.f);
    const float tMin = right ? rules.targetMin : 1.f - rules.targetMax;
    const float tMax = right ? rules.targetMax : 1.f - rules.targetMin;
    const float first = arenaLensGround(pose, partner).right < 0.f ? -1.f : 1.f;
    const float limit = std::max(0.f, rules.partnerYawMax);
    for (float d = 0.25f; d <= limit + 1e-4f; d += 0.25f) {
        for (const float sign : {first, -first}) {
            ArenaCameraPose p = pose;
            p.yaw = pose.yaw + sign * d;
            const ArenaFramePoint f = arenaFramePoint(p, aspect, partner);
            if (!f.ahead || f.x < lo || f.x > hi) continue;
            const ArenaFramePoint t = arenaFramePoint(p, aspect, target);
            if (!t.ahead || t.x < tMin || t.x > tMax) continue;
            if (!arenaLeadInFrame(leadRules, arenaLeadShares(p, aspect, body))) continue;
            return sign * d;
        }
    }
    return 0.f;
}

float arenaClosePartnerYawCap(const ArenaCloseRules& rules, bool ranged, bool leadBusy, float yawNow) {
    const float base = std::max(0.f, rules.partnerYawMax);
    if (!ranged) return base;
    const float most = std::max(base, rules.partnerYawRangedMax);
    if (!leadBusy) return most;
    const float held = finite(yawNow) ? std::fabs(yawNow) : 0.f;
    return std::clamp(held, base, most);
}

float arenaCloseContactWant(const ArenaCloseRules& rules, float distance, float targetHeight, float targetRadius) {
    if (!finite(distance) || distance < 0.f) return 0.f;
    const bool big = (finite(targetHeight) && targetHeight > rules.contactTall) || (finite(targetRadius) && targetRadius >= rules.contactWide);
    if (!big) return 0.f;
    const float body = distance - (finite(targetRadius) ? std::max(0.f, targetRadius) : 0.f);
    if (body <= rules.contactTo) return 1.f;
    if (body >= rules.contactFrom || !(rules.contactFrom > rules.contactTo)) return 0.f;
    const float t = (rules.contactFrom - body) / (rules.contactFrom - rules.contactTo);
    return t * t * (3.f - 2.f * t);
}

float arenaCloseContactStep(const ArenaCloseRules& rules, float now, float want, float dt) {
    if (!finite(now)) now = 0.f;
    if (!finite(want) || !finite(dt) || !(dt > 0.f)) return now;
    const float to = clamp01(want);
    if (!(rules.contactSeconds > 0.f)) return to;
    const float step = dt / rules.contactSeconds;
    return now < to ? std::min(to, now + step) : std::max(to, now - step);
}

bool arenaCloseContactBlending(float now, float want) {
    return finite(now) && finite(want) && now != clamp01(want);
}

namespace {
// How far (degrees) the swing m (the right shoulder's, the view turned left by m) leaves the target's centre and its
// near outline past where they are wanted (both >= 0: the swing is enough), on the floor plane from the lens.
float contactShort(float m, float side, float distance, float radius, float back, float centreAt, float edgeAt) {
    const float y = -m * kRad;
    const float ahead = distance * std::cos(y) + back;
    const float right = -distance * std::sin(y) - side;
    const float a = std::atan2(right, ahead) / kRad;
    const float dist = std::sqrt(ahead * ahead + right * right);
    const float half = dist > radius && dist > 0.f ? std::asin(radius / dist) / kRad : 90.f;
    return std::min(a - centreAt, a - half - edgeAt);
}
} // namespace

float arenaCloseContactSwing(const ArenaCloseRules& rules, float shoulder, float side, float distance, float radius, float fov,
                             float back) {
    const float s = finite(shoulder) ? std::min(1.f, std::max(-1.f, shoulder)) : 1.f;
    const float lo = std::max(0.f, rules.swingMin), hi = std::max(lo, rules.swingMax);
    float size = lo;
    if (finite(side) && finite(distance) && finite(radius) && finite(fov) && finite(back) && fov > 0.f && fov < 180.f) {
        const float t = std::tan(0.5f * fov * kRad);
        const float centreAt = std::atan((2.f * rules.contactX - 1.f) * t) / kRad;
        const float edgeAt = std::atan((2.f * rules.contactEdgeX - 1.f) * t) / kRad;
        const float sd = std::fabs(side), r = std::max(0.f, radius);
        if (contactShort(lo, sd, distance, r, back, centreAt, edgeAt) >= 0.f) {
            size = lo;
        } else if (contactShort(hi, sd, distance, r, back, centreAt, edgeAt) < 0.f) {
            size = hi;
        } else {
            float a = lo, b = hi; // short at a, enough at b
            for (int i = 0; i < 24; ++i) {
                const float mid = 0.5f * (a + b);
                if (contactShort(mid, sd, distance, r, back, centreAt, edgeAt) >= 0.f) b = mid;
                else a = mid;
            }
            size = b;
        }
    }
    return -size * s;
}

ArenaBodySpan arenaBodySpan(const ArenaCameraPose& pose, float aspect, ArenaPoint3 centre, float radius) {
    ArenaBodySpan out;
    if (!finite(centre.x) || !finite(centre.y) || !finite(centre.z) || !finite(radius)) return out;
    const ArenaLensAt lens = arenaCameraLens(pose);
    const float vx = centre.x - lens.x, vy = centre.y - lens.y;
    const float d = std::sqrt(vx * vx + vy * vy);
    const float r = std::max(0.f, radius);
    const ArenaFramePoint c = arenaFramePoint(pose, aspect, centre);
    out.ahead = c.ahead;
    if (!(d > r)) { // the lens inside the body: it fills the width
        out.left = -1.f;
        out.right = 2.f;
        return out;
    }
    // The two tangent points from the lens: from the centre, back toward the lens turned by acos(r / d) each way.
    const float b = std::acos(r / d);
    const float ux = -vx / d, uy = -vy / d;
    const float cb = std::cos(b), sb = std::sin(b);
    const ArenaPoint3 t1{centre.x + r * (ux * cb - uy * sb), centre.y + r * (ux * sb + uy * cb), centre.z};
    const ArenaPoint3 t2{centre.x + r * (ux * cb + uy * sb), centre.y + r * (-ux * sb + uy * cb), centre.z};
    const ArenaFramePoint f1 = arenaFramePoint(pose, aspect, t1), f2 = arenaFramePoint(pose, aspect, t2);
    const float x1 = f1.ahead ? f1.x : (f1.x < 0.5f ? -1.f : 2.f);
    const float x2 = f2.ahead ? f2.x : (f2.x < 0.5f ? -1.f : 2.f);
    out.left = std::min(x1, x2);
    out.right = std::max(x1, x2);
    return out;
}

float arenaCloseCutTallness(const ArenaCloseRules& rules, float targetHeight, float contact) {
    return std::max(arenaCloseTallness(rules, targetHeight), shareOf(contact));
}

float arenaCloseContactYaw(float frameTurn, float swing, float share) {
    if (!finite(share) || !(share > 0.f) || !finite(frameTurn) || !finite(swing)) return 0.f;
    const float c = clamp01(share);
    return (swing - frameTurn) * (c * c * (3.f - 2.f * c));
}

float arenaCloseContactPitch(const ArenaCloseRules& rules, float pitch, float share) {
    return arenaCloseContactPitch(rules, pitch, pitch, share);
}

float arenaCloseContactPitch(const ArenaCloseRules& rules, float offPitch, float contactPitch, float share) {
    if (!finite(offPitch)) return offPitch;
    const float standOff = std::min(offPitch, rules.standOffPitchMax);
    if (!finite(share) || !(share > 0.f) || !finite(contactPitch)) return standOff;
    const float held = std::min(rules.contactPitchMax, std::max(rules.contactPitchMin, contactPitch));
    return lerp(standOff, held, clamp01(share));
}

float arenaCloseContactLow(const ArenaCloseRules& rules, float contact, float targetHeight) {
    if (!finite(targetHeight) || !(targetHeight >= rules.contactLowTall)) return 0.f;
    return shareOf(contact);
}

ArenaLowRules arenaCloseContactLowRules(const ArenaCloseRules& rules, float side) {
    ArenaLowRules r = kArenaLow;
    r.eye = rules.contactLowEye;
    r.side = finite(side) ? side : 0.f;
    return r;
}

float arenaCloseContactLowPitch(const ArenaCloseRules& rules, float pitch, float low) {
    if (!finite(pitch) || !finite(low) || !(low > 0.f) || pitch <= rules.contactLowPitchMax) return pitch;
    return lerp(pitch, rules.contactLowPitchMax, clamp01(low));
}

float arenaCloseContactLowCut(const ArenaCloseRules& rules, float cut, float head, float low) {
    if (!finite(cut) || !finite(head) || !finite(low) || !(low > 0.f)) return cut;
    return lerp(cut, rules.contactLowWaist * head, clamp01(low));
}

ArenaLeadFrameRules arenaCloseContactLowLeadRules(const ArenaLeadFrameRules& leadRules, float low) {
    ArenaLeadFrameRules r = leadRules;
    if (finite(low) && low > 0.f) r.feetMin = lerp(leadRules.feetMin, kArenaLeadFrame.feetMin, clamp01(low));
    return r;
}

float arenaCloseWallPitch(const ArenaCloseRules& rules, float rulePitch, float asked) {
    if (!finite(rulePitch)) return rulePitch;
    const float floor = finite(asked) ? std::min(asked, rules.wallPitchMin) : rules.wallPitchMin;
    return std::max(rulePitch, floor);
}

float arenaCloseStandOffPitch(const ArenaCloseRules& rules, const ArenaLookLimits& limits, const ArenaCameraPose& rig, float aspect,
                              ArenaPoint3 point, float floorPitch) {
    const float cap = arenaClampViewPitch(limits, rules.standOffPitchMax);
    const float lo = std::min(cap, arenaClampViewPitch(limits, finite(floorPitch) ? floorPitch : 0.f));
    if (!finite(aspect) || !(aspect > 0.f) || !finite(point.x) || !finite(point.y) || !finite(point.z)) return lo;
    // The point's height in the frame falls as the view pitches up (the lens moving with the boom): bisect it.
    const auto pointY = [&](float p, bool& ahead) {
        const ArenaFramePoint q = arenaFramePoint(arenaPoseAtPitch(limits, rig, p), aspect, point);
        ahead = q.ahead;
        return q.y;
    };
    bool ahead = false;
    if (pointY(lo, ahead) <= rules.standOffTopY || !ahead) return lo;
    if (pointY(cap, ahead) > rules.standOffTopY && ahead) return cap;
    float a = lo, b = cap;
    for (int i = 0; i < 24; ++i) {
        const float m = 0.5f * (a + b);
        if (pointY(m, ahead) > rules.standOffTopY && ahead) a = m;
        else b = m;
    }
    return b;
}

bool arenaCloseHeadHidden(const ArenaCameraPose& pose, float aspect, const ArenaLeadBody& body, ArenaPoint3 head, float margin) {
    if (!finite(aspect) || !(aspect > 0.f) || !finite(head.x) || !finite(head.y) || !finite(head.z)) return false;
    const ArenaFramePoint p = arenaFramePoint(pose, aspect, head);
    if (!p.ahead || !finite(p.x) || !finite(p.y)) return false;
    const ArenaLeadShares s = arenaLeadShares(pose, aspect, body);
    if (!s.ahead) return false;
    const float m = finite(margin) ? std::max(0.f, margin) : 0.f;
    return p.x >= s.left - m && p.x <= s.right + m && p.y >= s.feet - m && p.y <= s.head + m;
}

ArenaLeadBody arenaCloseClearBody(const ArenaCloseRules& rules, ArenaLeadBody body) {
    if (finite(rules.clearRadius) && (!finite(body.radius) || body.radius < rules.clearRadius)) body.radius = rules.clearRadius;
    return body;
}

ArenaCloseClear arenaCloseStandOffClear(const ArenaCloseRules& rules, const ArenaCameraPose& pose, float aspect,
                                        const ArenaLeadBody& leadBody, ArenaPoint3 head, ArenaCloseClear held) {
    ArenaCloseClear out;
    if (!finite(pose.side) || !finite(pose.pivot) || !finite(rules.clearMargin)) return out;
    const ArenaLeadBody body = arenaCloseClearBody(rules, leadBody);
    const auto hiddenBy = [&](const ArenaCameraPose& q, float margin) { return arenaCloseHeadHidden(q, aspect, body, head, margin); };
    const auto hidden = [&](const ArenaCameraPose& q) { return hiddenBy(q, rules.clearMargin); };
    const float sign = pose.side < 0.f ? -1.f : 1.f;
    const float room = finite(rules.clearSideMax) ? std::max(0.f, rules.clearSideMax - std::fabs(pose.side)) : 0.f;
    const float rise = finite(rules.clearPivotUp) ? std::max(0.f, rules.clearPivotUp) : 0.f;
    // The hold: the last clearance is kept while the head is not clear of the bare view by the hold's more margin and
    // the last still clears it by the hold's less.
    const float hold = finite(rules.clearHold) ? std::max(0.f, rules.clearHold) : 0.f;
    if (finite(held.side) && finite(held.pivot) && (held.side != 0.f || held.pivot != 0.f) &&
        hiddenBy(pose, rules.clearMargin + hold)) {
        ArenaCameraPose kept = pose;
        const float add = std::min(room, std::fabs(held.side));
        const float up = std::min(rise, std::max(0.f, held.pivot));
        kept.side = pose.side + sign * add;
        kept.pivot = pose.pivot + up;
        if (!hiddenBy(kept, std::max(0.f, rules.clearMargin - hold))) {
            out.side = sign * add;
            out.pivot = up;
            return out;
        }
    }
    if (!hidden(pose)) return out;
    const float step = finite(rules.clearStep) && rules.clearStep > 0.01f ? rules.clearStep : 0.05f;
    // Each side step (the last landing on the most) with its least pivot step that clears; the least sum kept. At most
    // 18 x 7 projections of the head and the lead.
    const int sideSteps = static_cast<int>(std::ceil(room / step - 1e-4f));
    const int upSteps = static_cast<int>(std::ceil(rise / step - 1e-4f));
    ArenaCameraPose p = pose;
    float best = 1e9f;
    for (int ks = 0; ks <= sideSteps; ++ks) {
        const float add = std::min(room, static_cast<float>(ks) * step);
        if (add >= best) break;
        p.side = pose.side + sign * add;
        for (int kp = ks == 0 ? 1 : 0; kp <= upSteps; ++kp) {
            const float up = std::min(rise, static_cast<float>(kp) * step);
            if (add + up >= best) break;
            p.pivot = pose.pivot + up;
            if (hidden(p)) continue;
            best = add + up;
            out.side = sign * add;
            out.pivot = up;
            break;
        }
    }
    return out; // 0 and 0 when no pair cleared the widened box: a move that does not clear it is not asked
}

ArenaCloseFrameCheck arenaCloseFrameCheck(const ArenaCloseRules& rules, float shoulder, float leadX, float targetX,
                                          float distance, bool partnerOn, ArenaFramePoint partner) {
    ArenaCloseFrameCheck out;
    const bool left = shoulder < 0.f;
    // Mirrored onto the right shoulder: the lead on the left, the target right of centre.
    const float lead = left ? 1.f - leadX : leadX;
    const float target = left ? 1.f - targetX : targetX;
    const float slack = finite(rules.frameLeadSlack) ? std::max(0.f, rules.frameLeadSlack) : 0.f;
    out.lead = finite(lead) && lead >= 0.f && lead <= 1.f / 3.f + slack;
    const bool nearBand = finite(distance) && finite(rules.frameNearDistance) && finite(rules.frameNearTargetMin) &&
                          distance < rules.frameNearDistance;
    const float bandMin = nearBand ? rules.frameNearTargetMin : rules.targetMin;
    out.target = finite(target) && target >= bandMin && target <= rules.targetMax;
    out.partner = !partnerOn || (partner.ahead && finite(partner.x) && finite(partner.y) && partner.x >= 0.f && partner.x <= 1.f &&
                                 partner.y >= 0.f && partner.y <= 1.f);
    out.all = out.lead && out.target && out.partner;
    return out;
}

float arenaCloseClearStep(const ArenaCloseRules& rules, float now, float want, float dt) {
    const float at = finite(now) ? now : 0.f;
    if (!finite(want) || !finite(dt) || !(dt > 0.f)) return at;
    const float tau = finite(rules.clearEase) && rules.clearEase > 1e-4f ? rules.clearEase : 1e-4f;
    return at + (want - at) * (1.f - std::exp(-dt / tau));
}

ArenaLensGround arenaLensGround(const ArenaCameraPose& pose, ArenaPoint3 at) {
    const ArenaLensAt lens = arenaCameraLens(pose);
    const float y = pose.yaw * kRad;
    const float cy = std::cos(y), sy = std::sin(y);
    const float dx = at.x - lens.x, dy = at.y - lens.y;
    ArenaLensGround out;
    out.ahead = dx * cy + dy * sy;
    out.right = -dx * sy + dy * cy;
    return out;
}

} // namespace ftcore
