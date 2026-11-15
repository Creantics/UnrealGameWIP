#include "ftcore/ArenaLockFrame.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {
constexpr float kRad = 3.14159265358979f / 180.f;

bool finite(float v) { return std::isfinite(v); }

// One pose's view, its basis worked out once for the many points a frame test projects.
struct View {
    bool ok = false;
    float lx = 0.f, ly = 0.f, lz = 0.f; // the lens, from the floor point under the pivot
    float fx = 0.f, fy = 0.f, fz = 0.f; // forward
    float rx = 0.f, ry = 0.f;           // right (level)
    float ux = 0.f, uy = 0.f, uz = 0.f; // up
    float tanH = 0.f, tanV = 0.f;
};

View viewOf(const ArenaCameraPose& pose, float aspect) {
    View v;
    if (!(aspect > 0.f) || !finite(aspect) || !(pose.fov > 0.f) || !(pose.fov < 179.f)) return v;
    const ArenaLensAt lens = arenaCameraLens(pose);
    const float p = (pose.boomPitch + pose.tilt) * kRad;
    const float y = pose.yaw * kRad;
    const float cp = std::cos(p), sp = std::sin(p), cy = std::cos(y), sy = std::sin(y);
    v.lx = lens.x;
    v.ly = lens.y;
    v.lz = pose.pivot + lens.z;
    v.fx = cp * cy;
    v.fy = cp * sy;
    v.fz = sp;
    v.rx = -sy;
    v.ry = cy;
    v.ux = -sp * cy;
    v.uy = -sp * sy;
    v.uz = cp;
    v.tanH = std::tan(0.5f * pose.fov * kRad);
    v.tanV = v.tanH / aspect;
    v.ok = finite(v.lx) && finite(v.ly) && finite(v.lz) && finite(v.tanH) && v.tanH > 0.f;
    return v;
}

ArenaFramePoint project(const View& v, ArenaPoint3 at) {
    ArenaFramePoint out;
    if (!v.ok) return out;
    const float dx = at.x - v.lx, dy = at.y - v.ly, dz = at.z - v.lz;
    const float fwd = dx * v.fx + dy * v.fy + dz * v.fz;
    if (!(fwd > 1e-4f)) return out;
    const float right = dx * v.rx + dy * v.ry;
    const float up = dx * v.ux + dy * v.uy + dz * v.uz;
    out.x = 0.5f * (1.f + right / (fwd * v.tanH));
    out.y = 0.5f * (1.f + up / (fwd * v.tanV));
    out.ahead = finite(out.x) && finite(out.y);
    if (!out.ahead) out.x = out.y = 0.f;
    return out;
}

ArenaLeadShares sharesOf(const View& v, const ArenaLeadBody& body) {
    // The body's sides along the view's right.
    const float rx = v.rx * body.radius, ry = v.ry * body.radius;
    const ArenaPoint3 pts[6] = {
        {0.f, 0.f, 0.f}, {0.f, 0.f, body.head}, {rx, ry, 0.f}, {-rx, -ry, 0.f}, {rx, ry, body.head}, {-rx, -ry, body.head},
    };
    ArenaLeadShares s;
    s.ahead = true;
    s.feet = 1e9f;
    s.head = -1e9f;
    s.left = 1e9f;
    s.right = -1e9f;
    for (const ArenaPoint3& q : pts) {
        const ArenaFramePoint f = project(v, q);
        if (!f.ahead) return ArenaLeadShares{};
        s.feet = std::min(s.feet, f.y);
        s.head = std::max(s.head, f.y);
        s.left = std::min(s.left, f.x);
        s.right = std::max(s.right, f.x);
    }
    return s;
}

float lowShare(float low) { return finite(low) ? std::clamp(low, 0.f, 1.f) : 0.f; }

// arenaPoseAtPitch with the low boom worked out already (it depends on the rig alone, not on the pitch).
ArenaCameraPose poseAt(const ArenaLookLimits& limits, const ArenaCameraPose& rig, float viewPitch, float w, float lowBoom,
                       float lowSide) {
    ArenaCameraPose p = rig;
    const float view = arenaClampViewPitch(limits, viewPitch);
    const ArenaPitchSplit split = arenaSplitPitch(limits, view);
    if (w <= 0.f) {
        p.boomPitch = split.boom;
        p.tilt = split.tilt;
        return p;
    }
    p.boomPitch = split.boom + w * (lowBoom - split.boom);
    p.tilt = view - p.boomPitch;
    p.side = rig.side + w * (lowSide - rig.side);
    return p;
}

// One rig's lead test at many pitches: the low boom hoisted, each pitch's verdict and how far the body is out.
struct LeadTest {
    const ArenaLookLimits& limits;
    const ArenaLeadFrameRules& rules;
    const ArenaCameraPose& rig;
    float aspect;
    const ArenaLeadBody& body;
    float w;
    float lowBoom;
    float lowSide;
    int evals = 0;

    struct At {
        int level = 0;        // 2: the rule, 1: visible only, 0: neither
        float outside = 1e3f; // how far the body's extremes pass the frame's edges (shares, the feet's weighted); 1e3 behind
        ArenaLeadShares s{};
    };

    At at(float pitch) {
        ++evals;
        At a;
        a.s = sharesOf(viewOf(poseAt(limits, rig, pitch, w, lowBoom, lowSide), aspect), body);
        const ArenaLeadShares& s = a.s;
        if (!s.ahead) return a;
        a.outside = kArenaFeetGive * std::max(0.f, -s.feet) + std::max(0.f, s.head - 1.f) + std::max(0.f, -s.left) + std::max(0.f, s.right - 1.f);
        a.level = arenaLeadInFrame(rules, s) ? 2 : (arenaLeadVisible(s) ? 1 : 0);
        return a;
    }
};

float lowBoomOf(const ArenaCameraPose& rig, float w, const ArenaLowRules& lowRules) {
    return w > 0.f ? arenaLowBoom(rig.pivot, rig.arm, rig.up, lowRules.eye) : 0.f;
}
} // namespace

ArenaFramePoint arenaFramePoint(const ArenaCameraPose& pose, float aspect, ArenaPoint3 at) { return project(viewOf(pose, aspect), at); }

ArenaLeadShares arenaLeadShares(const ArenaCameraPose& pose, float aspect, const ArenaLeadBody& body) {
    return sharesOf(viewOf(pose, aspect), body);
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
    const float w = lowShare(low);
    return poseAt(limits, rig, viewPitch, w, lowBoomOf(rig, w, lowRules), lowRules.side);
}

ArenaLeadPitch arenaLeadPitch(const ArenaLookLimits& limits, const ArenaLeadFrameRules& rules, const ArenaCameraPose& rig,
                              float aspect, const ArenaLeadBody& body, float want, float low, const ArenaLowRules& lowRules) {
    const float w = lowShare(low);
    LeadTest t{limits, rules, rig, aspect, body, w, lowBoomOf(rig, w, lowRules), lowRules.side};
    ArenaLeadPitch out;
    const float goal = arenaClampViewPitch(limits, finite(want) ? want : 0.f);
    out.pitch = goal;
    const LeadTest::At here = t.at(goal);
    if (here.level == 2) {
        out.kept = 2; // the asked pitch keeps the rule: it stands as asked (an ordinary lock is never moved)
        out.evals = t.evals;
        return out;
    }
    // The lead's shares fall as the view pitches up (the lead sinks in the frame), so the pitches that keep a level are
    // one band: its edge nearest the goal is a bisection from the goal toward the end of the range that fixes what the
    // goal breaks. The goal breaking both the feet and the head (the frame too small for the lead) has no band.
    const auto band = [&](float feetMin, float headMax, int level, float& got) {
        // A lead behind the lens (the view pitched far up) counts as under the frame: its feet and head below it.
        const bool feetBad = !here.s.ahead || here.s.feet < feetMin;
        const bool headBad = here.s.ahead && here.s.head > headMax;
        if (feetBad && headBad) return false;
        const auto inBand = [&](const LeadTest::At& a) { return a.s.ahead && a.s.feet >= feetMin && a.s.head <= headMax; };
        // The edge between a pitch that keeps the level (good) and one nearer the goal that does not (bad).
        const auto refine = [&](float good, float bad) {
            for (int i = 0; i < 14; ++i) {
                const float m = 0.5f * (good + bad);
                if (t.at(m).level >= level) good = m;
                else bad = m;
            }
            return good;
        };
        if (!feetBad && !headBad) {
            // The feet and the head fit at the goal, the sides do not: walk out each way in turn while the feet and the
            // head still fit, 2.5 degrees a step; the first pitch that keeps the level is refined back toward the goal.
            constexpr float kWalk = 2.5f;
            bool openHi = true, openLo = true;
            for (int k = 1; (openHi || openLo) && k <= 54; ++k) {
                for (const float sign : {1.f, -1.f}) {
                    bool& open = sign > 0.f ? openHi : openLo;
                    if (!open) continue;
                    const float q = goal + sign * kWalk * static_cast<float>(k);
                    if (q > limits.viewMax || q < limits.viewMin) {
                        open = false;
                        continue;
                    }
                    const LeadTest::At a = t.at(q);
                    if (a.level >= level) {
                        got = refine(q, q - sign * kWalk);
                        return true;
                    }
                    if (!inBand(a)) open = false;
                }
            }
            return false;
        }
        const float end = feetBad ? limits.viewMin : limits.viewMax;
        const auto fixed = [&](const LeadTest::At& a) {
            if (!a.s.ahead) return headBad;
            return feetBad ? a.s.feet >= feetMin : a.s.head <= headMax;
        };
        if (!fixed(t.at(end))) return false;
        float in = end, outP = goal;
        for (int i = 0; i < 18; ++i) {
            const float m = 0.5f * (in + outP);
            if (fixed(t.at(m))) in = m;
            else outP = m;
        }
        if (t.at(in).level >= level) {
            got = in;
            return true;
        }
        // The feet and the head fit there but the sides do not (a narrow frame: the body's width moves with the pitch).
        // Walk on into the band, away from the goal, a few samples as far as the end of the range; the first that keeps
        // the level is refined back toward the goal.
        const float span = end - in;
        float prev = in;
        for (int k = 1; k <= 12; ++k) {
            const float q = in + span * static_cast<float>(k) / 12.f;
            const LeadTest::At a = t.at(q);
            if (a.level >= level) {
                got = refine(q, prev);
                return true;
            }
            if (!inBand(a)) break; // past the band's far edge
            prev = q;
        }
        return false;
    };
    float got = goal;
    if (band(rules.feetMin, rules.headMax, 2, got)) {
        out.pitch = got;
        out.kept = 2;
        out.evals = t.evals;
        return out;
    }
    if (here.level >= 1 || band(0.f, 1.f, 1, got)) {
        // No pitch keeps the rule (a lens pulled in close by a wall): the nearest that keeps the whole body visible.
        out.pitch = here.level >= 1 ? goal : got;
        out.kept = 1;
        out.evals = t.evals;
        return out;
    }
    // Not even that: the pitch where the body passes the frame's edges least over the whole range (the feet given up
    // first, kArenaFeetGive), never the asked one as it is unless nothing does better.
    float lo = limits.viewMin, hi = limits.viewMax;
    for (int i = 0; i < 24; ++i) {
        const float m1 = lo + (hi - lo) / 3.f, m2 = hi - (hi - lo) / 3.f;
        if (t.at(m1).outside <= t.at(m2).outside) hi = m2;
        else lo = m1;
    }
    const float mid = 0.5f * (lo + hi);
    out.pitch = t.at(mid).outside <= here.outside ? mid : goal;
    out.kept = 0;
    out.evals = t.evals;
    return out;
}

float arenaLowFit(const ArenaLookLimits& limits, const ArenaLeadFrameRules& rules, const ArenaCameraPose& rig, float aspect,
                  const ArenaLeadBody& body, const ArenaLowRules& lowRules) {
    // A low share fits when some pitch keeps the rule there: as the lead's shares fall with the pitch, the pitch with
    // the feet on their line (the highest the feet allow) must keep the head under its own.
    const auto fits = [&](float w) {
        LeadTest t{limits, rules, rig, aspect, body, w, lowBoomOf(rig, w, lowRules), lowRules.side};
        const auto feetOk = [&](float p) {
            const LeadTest::At a = t.at(p);
            return a.s.ahead && a.s.feet >= rules.feetMin;
        };
        float lo = limits.viewMin, hi = limits.viewMax;
        if (!feetOk(lo)) return false;
        if (!feetOk(hi)) {
            for (int i = 0; i < 14; ++i) {
                const float m = 0.5f * (lo + hi);
                if (feetOk(m)) lo = m;
                else hi = m;
            }
        } else {
            lo = hi;
        }
        return t.at(lo).level == 2;
    };
    if (fits(1.f)) return 1.f;
    if (!fits(0.f)) return 0.f;
    float a = 0.f, b = 1.f;
    for (int i = 0; i < 7; ++i) {
        const float m = 0.5f * (a + b);
        if (fits(m)) a = m;
        else b = m;
    }
    return a;
}

float arenaLockAimHeight(float base, float top, float telegraph, float weakPoint) {
    if (telegraph >= 0.f && finite(telegraph)) return telegraph;
    if (weakPoint >= 0.f && finite(weakPoint)) return weakPoint;
    const float lo = finite(base) ? base : 0.f;
    const float hi = finite(top) && top > lo ? top : lo;
    return lo + kArenaChestShare * (hi - lo);
}

float arenaLockAimPitch(const ArenaLookLimits& limits, const ArenaCameraPose& rig, float aspect, ArenaPoint3 aim, float floorPitch,
                        float low, const ArenaLowRules& lowRules) {
    const float w = lowShare(low);
    const float lowBoom = lowBoomOf(rig, w, lowRules);
    // The pitch that puts it on the frame's centre line: its height in the frame falls as the view pitches up (the
    // lens moving with the boom), so bisect it over [floorPitch, viewMax].
    const auto aimY = [&](float p) {
        const ArenaFramePoint q = project(viewOf(poseAt(limits, rig, p, w, lowBoom, lowRules.side), aspect), aim);
        return q.ahead ? q.y : 0.f;
    };
    float a = arenaClampViewPitch(limits, finite(floorPitch) ? floorPitch : 0.f), b = limits.viewMax;
    if (aimY(a) <= 0.5f) return a;
    if (aimY(b) >= 0.5f) return b;
    for (int i = 0; i < 24; ++i) {
        const float m = 0.5f * (a + b);
        if (aimY(m) > 0.5f) a = m;
        else b = m;
    }
    return 0.5f * (a + b);
}

ArenaLockFrame arenaLockFrame(const ArenaLookLimits& limits, const ArenaLeadFrameRules& rules, const ArenaCameraPose& rig,
                              float aspect, const ArenaLeadBody& body, float aimAway, float aimHeight, float floorPitch,
                              float low, const ArenaLowRules& lowRules) {
    ArenaLockFrame f;
    // The aim point straight ahead of the pivot along the view's heading.
    const float y = rig.yaw * kRad;
    const ArenaPoint3 aim{aimAway * std::cos(y), aimAway * std::sin(y), aimHeight};
    f.want = arenaLockAimPitch(limits, rig, aspect, aim, floorPitch, low, lowRules);
    const ArenaLeadPitch held = arenaLeadPitch(limits, rules, rig, aspect, body, f.want, low, lowRules);
    f.pitch = held.pitch;
    f.kept = held.kept;
    const ArenaFramePoint at = arenaFramePoint(arenaPoseAtPitch(limits, rig, f.pitch, low, lowRules), aspect, aim);
    f.aimAbove = at.ahead && at.y > 1.f;
    f.aimX = at.ahead ? std::clamp(at.x, 0.f, 1.f) : 0.5f;
    return f;
}

float arenaLowSide(const ArenaLookLimits& limits, const ArenaCameraPose& rig, float viewPitch, float aspect, const ArenaLeadBody& body,
                   ArenaPoint3 aim, float current, const ArenaLowSideRules& sideRules, const ArenaLowRules& lowRules) {
    const float cur = current < 0.f ? -1.f : 1.f;
    const float mag = std::fabs(lowRules.side);
    if (!(mag > 1e-3f) || !finite(mag)) return cur;
    const float lowBoom = lowBoomOf(rig, 1.f, lowRules);
    // How far the aim point stands clear of the lead's box in the frame, from the full low lens on shoulder `s`: 1 when
    // it is above or below the body (never hidden), 0 when it is inside the body's width.
    const auto clear = [&](float s) {
        const View v = viewOf(poseAt(limits, rig, viewPitch, 1.f, lowBoom, s * mag), aspect);
        const ArenaFramePoint a = project(v, aim);
        if (!a.ahead) return 1.f;
        const ArenaLeadShares sh = sharesOf(v, body);
        if (!sh.ahead || a.y > sh.head || a.y < sh.feet) return 1.f;
        return std::max(0.f, std::max(sh.left - a.x, a.x - sh.right));
    };
    const float here = clear(cur);
    if (here >= sideRules.clear) return cur;
    return clear(-cur) >= here + sideRules.flip ? -cur : cur;
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
            const View v = viewOf(p, aspect);
            const ArenaFramePoint f = project(v, partner);
            if (!f.ahead || f.x < lo || f.x > hi) continue;
            if (!arenaLeadInFrame(rules, sharesOf(v, body))) continue;
            return sign * d;
        }
    }
    return 0.f;
}

bool arenaHiddenBehind(ArenaPoint3 eye, ArenaPoint3 point, Vec2 at, float radius, float baseZ, float topZ) {
    if (!(radius > 0.f) || !(topZ > baseZ) || !finite(eye.x) || !finite(eye.y) || !finite(eye.z) || !finite(point.x) ||
        !finite(point.y) || !finite(point.z) || !finite(at.x) || !finite(at.y))
        return false;
    const auto within = [&](float z) { return z >= baseZ && z <= topZ; };
    const float ex = eye.x - at.x, ey = eye.y - at.y;
    const float dx = point.x - eye.x, dy = point.y - eye.y, dz = point.z - eye.z;
    const float c = ex * ex + ey * ey - radius * radius;
    if (c <= 0.f && within(eye.z)) return false; // the eye inside the body
    // Where the line is over the body's footprint, t in [t0, t1] of the way from the eye to the point.
    const float a = dx * dx + dy * dy;
    float t0 = 0.f, t1 = 1.f;
    if (a < 1e-8f) {
        if (c > 0.f) return false; // straight up or down, beside the body
    } else {
        const float b = 2.f * (ex * dx + ey * dy);
        const float disc = b * b - 4.f * a * c;
        if (disc < 0.f) return false;
        const float root = std::sqrt(disc);
        t0 = std::max(0.f, (-b - root) / (2.f * a));
        t1 = std::min(1.f, (-b + root) / (2.f * a));
        if (t0 > t1) return false;
    }
    // Over the footprint, the line's height runs from z(t0) to z(t1): hidden when that span meets the body's.
    const float z0 = eye.z + dz * t0, z1 = eye.z + dz * t1;
    return std::max(z0, z1) >= baseZ && std::min(z0, z1) <= topZ;
}

ArenaFrustum arenaPoseFrustum(const ArenaCameraPose& pose, Vec2 at, float aspect) {
    ArenaFrustum f;
    const View v = viewOf(pose, aspect);
    if (!v.ok) return f;
    f.pos = ArenaPoint3{at.x + v.lx, at.y + v.ly, v.lz};
    f.forward = ArenaPoint3{v.fx, v.fy, v.fz};
    f.up = ArenaPoint3{v.ux, v.uy, v.uz};
    f.aspect = aspect;
    f.vfov = 2.f * std::atan(v.tanV) / kRad;
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

PartnerCameraView arenaPoseCamera(const ArenaCameraPose& pose, Vec2 at, float aspect, float bodyHeight) {
    PartnerCameraView v;
    const ArenaFrustum f = arenaPoseFrustum(pose, at, aspect);
    if (!f.on) return v;
    v.given = true;
    v.pos = PartnerVec3{f.pos.x, f.pos.y, f.pos.z};
    v.forward = PartnerVec3{f.forward.x, f.forward.y, f.forward.z};
    v.up = PartnerVec3{f.up.x, f.up.y, f.up.z};
    v.verticalFovDegrees = f.vfov;
    v.aspect = f.aspect;
    v.groundZ = 0.f;
    v.bodyHeight = bodyHeight;
    return v;
}


} // namespace ftcore
