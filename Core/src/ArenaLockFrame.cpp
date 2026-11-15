#include "ftcore/ArenaLockFrame.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {
constexpr float kRad = 3.14159265358979f / 180.f;

bool finite(float v) { return std::isfinite(v); }
} // namespace

ArenaFramePoint arenaFramePoint(const ArenaCameraPose& pose, float aspect, ArenaPoint3 at) {
    ArenaFramePoint out;
    if (!(aspect > 0.f) || !finite(aspect) || !(pose.fov > 0.f) || !(pose.fov < 179.f)) return out;
    const ArenaLensAt lens = arenaCameraLens(pose);
    const float p = (pose.boomPitch + pose.tilt) * kRad;
    const float y = pose.yaw * kRad;
    const float cp = std::cos(p), sp = std::sin(p), cy = std::cos(y), sy = std::sin(y);
    const float dx = at.x - lens.x;
    const float dy = at.y - lens.y;
    const float dz = at.z - (pose.pivot + lens.z);
    const float fwd = dx * cp * cy + dy * cp * sy + dz * sp;
    if (!(fwd > 1e-4f)) return out;
    const float right = -dx * sy + dy * cy;
    const float up = -dx * sp * cy - dy * sp * sy + dz * cp;
    const float tanH = std::tan(0.5f * pose.fov * kRad);
    const float tanV = tanH / aspect;
    out.x = 0.5f * (1.f + right / (fwd * tanH));
    out.y = 0.5f * (1.f + up / (fwd * tanV));
    out.ahead = finite(out.x) && finite(out.y);
    if (!out.ahead) out.x = out.y = 0.f;
    return out;
}

ArenaLeadShares arenaLeadShares(const ArenaCameraPose& pose, float aspect, const ArenaLeadBody& body) {
    ArenaLeadShares s;
    const float y = pose.yaw * kRad;
    const float rx = -std::sin(y) * body.radius;
    const float ry = std::cos(y) * body.radius;
    const ArenaPoint3 pts[6] = {
        {0.f, 0.f, 0.f}, {0.f, 0.f, body.head}, {rx, ry, 0.f}, {-rx, -ry, 0.f}, {rx, ry, body.head}, {-rx, -ry, body.head},
    };
    s.ahead = true;
    s.feet = 1e9f;
    s.head = -1e9f;
    s.left = 1e9f;
    s.right = -1e9f;
    for (const ArenaPoint3& q : pts) {
        const ArenaFramePoint f = arenaFramePoint(pose, aspect, q);
        if (!f.ahead) {
            s = ArenaLeadShares{};
            return s;
        }
        s.feet = std::min(s.feet, f.y);
        s.head = std::max(s.head, f.y);
        s.left = std::min(s.left, f.x);
        s.right = std::max(s.right, f.x);
    }
    return s;
}

bool arenaLeadInFrame(const ArenaLeadFrameRules& rules, const ArenaLeadShares& s) {
    const float edge = 0.5f * (1.f - rules.widthMid);
    return s.ahead && s.feet >= rules.feetMin && s.head <= rules.headMax && s.left >= edge && s.right <= 1.f - edge;
}

bool arenaLeadVisible(const ArenaLeadShares& s) {
    return s.ahead && s.feet >= 0.f && s.head <= 1.f && s.left >= 0.f && s.right <= 1.f;
}

float arenaLowBoom(float pivot, float arm, float up, float eye) {
    if (!(arenaEyeHeight(pivot, arm, up, 0.f) > eye)) return 0.f;
    float a = 0.f, b = 60.f;
    if (arenaEyeHeight(pivot, arm, up, b) >= eye) return b;
    for (int i = 0; i < 40; ++i) {
        const float m = 0.5f * (a + b);
        if (arenaEyeHeight(pivot, arm, up, m) > eye) a = m;
        else b = m;
    }
    return 0.5f * (a + b);
}

ArenaCameraPose arenaPoseAtPitch(const ArenaLookLimits& limits, const ArenaCameraPose& rig, float viewPitch, float low,
                                 const ArenaLowRules& lowRules) {
    ArenaCameraPose p = rig;
    const float view = arenaClampViewPitch(limits, viewPitch);
    const ArenaPitchSplit split = arenaSplitPitch(limits, view);
    const float w = finite(low) ? std::clamp(low, 0.f, 1.f) : 0.f;
    if (w <= 0.f) {
        p.boomPitch = split.boom;
        p.tilt = split.tilt;
        return p;
    }
    const float lowBoom = arenaLowBoom(rig.pivot, rig.arm, rig.up, lowRules.eye);
    p.boomPitch = split.boom + w * (lowBoom - split.boom);
    p.tilt = view - p.boomPitch;
    p.side = rig.side + w * (lowRules.side - rig.side);
    return p;
}

namespace {
// 2: the rule, 1: visible only, 0: neither.
int keptAt(const ArenaLookLimits& limits, const ArenaLeadFrameRules& rules, const ArenaCameraPose& rig, float aspect,
           const ArenaLeadBody& body, float pitch, float low) {
    const ArenaLeadShares s = arenaLeadShares(arenaPoseAtPitch(limits, rig, pitch, low), aspect, body);
    if (arenaLeadInFrame(rules, s)) return 2;
    return arenaLeadVisible(s) ? 1 : 0;
}
} // namespace

ArenaLeadPitch arenaLeadPitch(const ArenaLookLimits& limits, const ArenaLeadFrameRules& rules, const ArenaCameraPose& rig,
                              float aspect, const ArenaLeadBody& body, float want, float low) {
    ArenaLeadPitch out;
    const float goal = arenaClampViewPitch(limits, want);
    out.pitch = goal;
    const float step = rules.step > 0.01f ? rules.step : 0.25f;
    for (int level = 2; level >= 1; --level) {
        if (keptAt(limits, rules, rig, aspect, body, goal, low) >= level) {
            out.kept = level;
            return out;
        }
        // The nearest sampled pitch that keeps this level, then its edge toward the goal refined.
        float best = 0.f, bestGap = 1e9f;
        bool found = false;
        for (float p = limits.viewMin; p <= limits.viewMax + 1e-4f; p += step) {
            if (keptAt(limits, rules, rig, aspect, body, p, low) < level) continue;
            const float gap = std::fabs(p - goal);
            if (gap < bestGap) {
                bestGap = gap;
                best = p;
                found = true;
            }
        }
        if (!found) continue;
        float in = best;
        float outP = goal > best ? std::min(best + step, goal) : std::max(best - step, goal);
        for (int i = 0; i < 24; ++i) {
            const float m = 0.5f * (in + outP);
            if (keptAt(limits, rules, rig, aspect, body, m, low) >= level) in = m;
            else outP = m;
        }
        out.pitch = in;
        out.kept = level;
        return out;
    }
    out.kept = 0;
    return out;
}

float arenaLockAimHeight(float base, float top, float telegraph, float weakPoint) {
    if (telegraph >= 0.f && finite(telegraph)) return telegraph;
    if (weakPoint >= 0.f && finite(weakPoint)) return weakPoint;
    const float lo = finite(base) ? base : 0.f;
    const float hi = finite(top) && top > lo ? top : lo;
    return lo + kArenaChestShare * (hi - lo);
}

ArenaLockFrame arenaLockFrame(const ArenaLookLimits& limits, const ArenaLeadFrameRules& rules, const ArenaCameraPose& rig,
                              float aspect, const ArenaLeadBody& body, float aimAway, float aimHeight, float floorPitch,
                              float low) {
    ArenaLockFrame f;
    // The aim point straight ahead of the pivot along the view's heading.
    const float y = rig.yaw * kRad;
    const ArenaPoint3 aim{aimAway * std::cos(y), aimAway * std::sin(y), aimHeight};
    // The pitch that puts it on the frame's centre line: its height in the frame falls as the view pitches up (the
    // lens moving with the boom), so bisect it over [floorPitch, viewMax].
    const auto aimY = [&](float p) {
        const ArenaFramePoint q = arenaFramePoint(arenaPoseAtPitch(limits, rig, p, low), aspect, aim);
        return q.ahead ? q.y : 0.f;
    };
    float a = arenaClampViewPitch(limits, floorPitch), b = limits.viewMax;
    if (aimY(a) <= 0.5f) f.want = a;
    else if (aimY(b) >= 0.5f) f.want = b;
    else {
        for (int i = 0; i < 40; ++i) {
            const float m = 0.5f * (a + b);
            if (aimY(m) > 0.5f) a = m;
            else b = m;
        }
        f.want = 0.5f * (a + b);
    }
    const ArenaLeadPitch held = arenaLeadPitch(limits, rules, rig, aspect, body, f.want, low);
    f.pitch = held.pitch;
    f.kept = held.kept;
    const ArenaFramePoint at = arenaFramePoint(arenaPoseAtPitch(limits, rig, f.pitch, low), aspect, aim);
    f.aimAbove = at.ahead && at.y > 1.f;
    return f;
}

float arenaPartnerYaw(const ArenaPartnerYawRules& yawRules, const ArenaLeadFrameRules& rules, const ArenaCameraPose& pose,
                      float aspect, const ArenaLeadBody& body, ArenaPoint3 partner) {
    const ArenaFramePoint now = arenaFramePoint(pose, aspect, partner);
    const float lo = yawRules.margin, hi = 1.f - yawRules.margin;
    if (now.ahead && now.x >= lo && now.x <= hi) return 0.f;
    // The smallest turn (each way, at 0.25 degree steps) that brings it in and keeps the lead's rule.
    const float limit = std::max(0.f, yawRules.maxYaw);
    for (float d = 0.25f; d <= limit + 1e-4f; d += 0.25f) {
        for (const float sign : {1.f, -1.f}) {
            ArenaCameraPose p = pose;
            p.yaw = pose.yaw + sign * d;
            const ArenaFramePoint f = arenaFramePoint(p, aspect, partner);
            if (!f.ahead || f.x < lo || f.x > hi) continue;
            if (!arenaLeadInFrame(rules, arenaLeadShares(p, aspect, body))) continue;
            return sign * d;
        }
    }
    return 0.f;
}

ArenaFrustum arenaPoseFrustum(const ArenaCameraPose& pose, Vec2 at, float aspect) {
    ArenaFrustum f;
    if (!(aspect > 0.f) || !finite(aspect) || !(pose.fov > 0.f) || !(pose.fov < 179.f)) return f;
    const ArenaLensAt lens = arenaCameraLens(pose);
    const float p = (pose.boomPitch + pose.tilt) * kRad;
    const float y = pose.yaw * kRad;
    const float cp = std::cos(p), sp = std::sin(p), cy = std::cos(y), sy = std::sin(y);
    f.pos = ArenaPoint3{at.x + lens.x, at.y + lens.y, pose.pivot + lens.z};
    f.forward = ArenaPoint3{cp * cy, cp * sy, sp};
    f.up = ArenaPoint3{-sp * cy, -sp * sy, cp};
    f.aspect = aspect;
    f.vfov = 2.f * std::atan(std::tan(0.5f * pose.fov * kRad) / aspect) / kRad;
    f.on = finite(f.pos.x) && finite(f.pos.y) && finite(f.pos.z) && finite(f.vfov);
    return f;
}

bool arenaInFrustum(const ArenaFrustum& f, ArenaPoint3 p, float margin) {
    if (!f.on || !(f.vfov > 0.f) || !(f.vfov < 179.f) || !(f.aspect > 0.f)) return false;
    const ArenaPoint3& w = f.forward;
    const ArenaPoint3& u = f.up;
    // right = up x forward (x forward, y right at yaw 0, as arenaFramePoint)
    const ArenaPoint3 r{u.y * w.z - u.z * w.y, u.z * w.x - u.x * w.z, u.x * w.y - u.y * w.x};
    const float dx = p.x - f.pos.x, dy = p.y - f.pos.y, dz = p.z - f.pos.z;
    const float fwd = dx * w.x + dy * w.y + dz * w.z;
    if (!(fwd > 1e-4f)) return false;
    const float tanV = std::tan(0.5f * f.vfov * kRad);
    const float tanH = tanV * f.aspect;
    const float keep = 1.f - 2.f * std::clamp(finite(margin) ? margin : 0.f, 0.f, 0.49f);
    const float nx = (dx * r.x + dy * r.y + dz * r.z) / (fwd * tanH);
    const float ny = (dx * u.x + dy * u.y + dz * u.z) / (fwd * tanV);
    return std::fabs(nx) <= keep && std::fabs(ny) <= keep;
}

} // namespace ftcore
