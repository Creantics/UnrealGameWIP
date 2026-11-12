#include "ftcore/ArenaLook.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

constexpr float kDegToRad = 3.14159265358979f / 180.f;

} // namespace

float arenaClampViewPitch(const ArenaLookLimits& limits, float pitch) {
    if (!(pitch == pitch)) return 0.f;
    return pitch < limits.viewMin ? limits.viewMin : (pitch > limits.viewMax ? limits.viewMax : pitch);
}

ArenaPitchSplit arenaSplitPitch(const ArenaLookLimits& limits, float viewPitch) {
    const float view = arenaClampViewPitch(limits, viewPitch);
    ArenaPitchSplit s;
    s.boom = view > limits.boomMax ? limits.boomMax : view;
    s.tilt = view - s.boom;
    return s;
}

float arenaEyeHeight(float pivot, float arm, float up, float boomPitch) {
    const float r = boomPitch * kDegToRad;
    return pivot - arm * std::sin(r) + up * std::cos(r);
}

float arenaEyeBack(float arm, float up, float boomPitch) {
    const float r = boomPitch * kDegToRad;
    return arm * std::cos(r) + up * std::sin(r);
}

float arenaSightHeightAt(const ArenaLookLimits& limits, float pivot, float arm, float up, float away, float viewPitch) {
    const float view = arenaClampViewPitch(limits, viewPitch);
    const ArenaPitchSplit s = arenaSplitPitch(limits, view);
    const float along = arenaEyeBack(arm, up, s.boom) + away;
    return arenaEyeHeight(pivot, arm, up, s.boom) + along * std::tan(view * kDegToRad);
}

float arenaLookUpPitch(const ArenaLookLimits& limits, float pivot, float arm, float up, float away, float height,
                       float floorPitch) {
    const float lo = arenaClampViewPitch(limits, floorPitch);
    // The sight's height at `away` rises with the pitch (the lens drops as the boom swings up, but slower than the
    // line climbs over the arm's length ahead of it): bisect it.
    float a = lo, b = limits.viewMax;
    if (arenaSightHeightAt(limits, pivot, arm, up, away, a) >= height) return a;
    if (arenaSightHeightAt(limits, pivot, arm, up, away, b) <= height) return b;
    for (int i = 0; i < 40; ++i) {
        const float m = 0.5f * (a + b);
        if (arenaSightHeightAt(limits, pivot, arm, up, away, m) < height) a = m;
        else b = m;
    }
    return 0.5f * (a + b);
}

ArenaBodyCylinder arenaBodyCylinder(const ArenaEnemyKind& kind, Vec2 at, float floorZ) {
    const bool skinned = kind.look.skinned();
    const float scale = skinned && kind.look.scale > 0.f ? kind.look.scale : 1.f;
    const float hover = skinned ? std::max(0.f, kind.look.hover) : 0.f;
    ArenaBodyCylinder b;
    b.at = at;
    b.base = floorZ + hover;
    b.top = b.base + arenaEnemyHeight(kind) * scale;
    b.radius = std::max(0.f, kind.bodyRadius) * scale;
    return b;
}

bool arenaRayEntersBody(ArenaPoint3 from, ArenaPoint3 to, const ArenaBodyCylinder& body, float& t) {
    const float lx = to.x - from.x, ly = to.y - from.y, lz = to.z - from.z;
    const float ox = from.x - body.at.x, oy = from.y - body.at.y;
    const float r2 = body.radius * body.radius;
    if (!(body.radius > 0.f) || !(body.top > body.base)) return false;
    // The shares of the line inside the round (side) and inside the height (caps), each as [lo, hi].
    float sideLo, sideHi;
    const float a = lx * lx + ly * ly;
    if (a > 1e-12f) {
        const float b = 2.f * (ox * lx + oy * ly);
        const float c = ox * ox + oy * oy - r2;
        const float disc = b * b - 4.f * a * c;
        if (disc < 0.f) return false;
        const float root = std::sqrt(disc);
        sideLo = (-b - root) / (2.f * a);
        sideHi = (-b + root) / (2.f * a);
    } else {
        if (ox * ox + oy * oy > r2) return false; // straight up or down, beside the body
        sideLo = -1e30f;
        sideHi = 1e30f;
    }
    float capLo, capHi;
    if (std::fabs(lz) > 1e-9f) {
        const float ta = (body.base - from.z) / lz, tb = (body.top - from.z) / lz;
        capLo = std::min(ta, tb);
        capHi = std::max(ta, tb);
    } else {
        if (from.z < body.base || from.z > body.top) return false; // level, under or over the body
        capLo = -1e30f;
        capHi = 1e30f;
    }
    const float enter = std::max(sideLo, capLo);
    const float leave = std::min(sideHi, capHi);
    if (enter > leave) return false; // passes by
    if (enter < 0.f) return false;  // starts inside it (leave >= 0), or the body is behind (leave < 0)
    if (enter > 1.f) return false;  // beyond the segment's end
    t = enter;
    return true;
}

} // namespace ftcore
