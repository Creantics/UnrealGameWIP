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
