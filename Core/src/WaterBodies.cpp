#include "ftcore/WaterBodies.h"

#include "ftcore/Roads.h"
#include "ftcore/SeaWaves.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// p in a box's own axes.
Vec2 boxLocal(const WaterBody& body, Vec2 p) {
    const float dx = p.x - body.centre.x;
    const float dy = p.y - body.centre.y;
    return {dx * body.cosYaw + dy * body.sinYaw, -dx * body.sinYaw + dy * body.cosYaw};
}

} // namespace

const char* waterKindName(WaterKind kind) {
    switch (kind) {
    case WaterKind::Canal: return "canal";
    case WaterKind::Pool: return "pool";
    case WaterKind::Lake: return "lake";
    case WaterKind::Sea: return "sea";
    }
    return "water";
}

const char* waterModeName(WaterMode mode) {
    switch (mode) {
    case WaterMode::Dry: return "dry";
    case WaterMode::Wade: return "wade";
    case WaterMode::DeepWade: return "deep wade";
    case WaterMode::Swim: return "swim";
    }
    return "dry";
}

bool WaterBody::covers(Vec2 p) const {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
    switch (shape) {
    case WaterShape::Box: {
        const Vec2 l = boxLocal(*this, p);
        return std::fabs(l.x) <= half.x && std::fabs(l.y) <= half.y;
    }
    case WaterShape::HalfPlane:
        return out(p) >= 0.f;
    case WaterShape::Polygon: {
        if (pointCount < 3) return false;
        bool inside = false;
        for (std::size_t i = 0, j = pointCount - 1u; i < pointCount; j = i++) {
            const Vec2 a = points[i];
            const Vec2 b = points[j];
            // On an edge counts as in.
            const float cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
            if (cross == 0.f && p.x >= std::min(a.x, b.x) && p.x <= std::max(a.x, b.x) && p.y >= std::min(a.y, b.y) &&
                p.y <= std::max(a.y, b.y))
                return true;
            if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
        }
        return inside;
    }
    }
    return false;
}

float WaterBody::out(Vec2 p) const {
    if (shape != WaterShape::HalfPlane) return 0.f;
    return (p.x - centre.x) * cosYaw + (p.y - centre.y) * sinYaw;
}

float WaterBody::bedAt(Vec2 p) const {
    if (shape != WaterShape::Box || !(batter > 0.f) || !(shelfBed > bed)) return bed;
    const Vec2 l = boxLocal(*this, p);
    // Across the shorter side, in from the nearer long wall.
    const float fromWall = half.y <= half.x ? half.y - std::fabs(l.y) : half.x - std::fabs(l.x);
    if (!(fromWall >= 0.f)) return bed;
    return level - waterSectionDepth(fromWall, level - shelfBed, shelf, batter, level - bed);
}

WaterBody waterBodyFromCanal(const RoadWater& canal) {
    WaterBody body;
    body.kind = WaterKind::Canal;
    body.shape = WaterShape::Box;
    body.level = static_cast<float>(canal.z);
    const bool cut = canal.batter > 0.0 && canal.deep > canal.depth;
    body.bed = static_cast<float>(canal.z - (cut ? canal.deep : canal.depth));
    body.shelfBed = static_cast<float>(canal.z - canal.depth);
    body.shelf = cut ? static_cast<float>(canal.shelf) : 0.f;
    body.batter = cut ? static_cast<float>(canal.batter) : 0.f;
    body.centre = {static_cast<float>(canal.cx), static_cast<float>(canal.cy)};
    body.half = {static_cast<float>(canal.sx * 0.5), static_cast<float>(canal.sy * 0.5)};
    body.flow = {static_cast<float>(canal.flowX * canal.flow), static_cast<float>(canal.flowY * canal.flow)};
    return body;
}

WaterBody waterSea(float edgeX, float level, float bed, float buoyLine) {
    WaterBody body;
    body.kind = WaterKind::Sea;
    body.shape = WaterShape::HalfPlane;
    body.level = level;
    body.bed = bed;
    body.centre = {edgeX, 0.f};
    body.cosYaw = 1.f;
    body.sinYaw = 0.f;
    body.buoyLine = std::max(buoyLine, 0.f);
    return body;
}

WaterExit waterExitFromRoad(const RoadWaterExit& exit) {
    WaterExit e;
    e.kind = exit.kind == RoadExitKind::Steps ? WaterExitKind::Steps : WaterExitKind::Ladder;
    e.foot = {static_cast<float>(exit.footX), static_cast<float>(exit.footY)};
    e.top = {static_cast<float>(exit.topX), static_cast<float>(exit.topY)};
    e.topZ = static_cast<float>(exit.topZ);
    e.out = {static_cast<float>(exit.outX), static_cast<float>(exit.outY)};
    return e;
}

WaterExit waterExitToEngine(const WaterExit& planExit, float side) {
    WaterExit e = planExit;
    e.foot = {planExit.foot.x - side * 0.5f, side * 0.5f - planExit.foot.y};
    e.top = {planExit.top.x - side * 0.5f, side * 0.5f - planExit.top.y};
    e.out.y = -planExit.out.y;
    return e;
}

WaterBody waterBodyToEngine(const WaterBody& planBody, float side) {
    WaterBody body = planBody;
    auto point = [side](Vec2 p) { return Vec2{p.x - side * 0.5f, side * 0.5f - p.y}; };
    body.centre = point(planBody.centre);
    body.sinYaw = -planBody.sinYaw; // y turned over: an angle turns the other way
    body.flow.y = -planBody.flow.y;
    for (std::size_t i = 0; i < planBody.pointCount; ++i) body.points[i] = point(planBody.points[i]);
    return body;
}

bool WaterBodies::add(const WaterBody& body) {
    if (count_ >= kWaterMaxBodies) return false;
    if (!std::isfinite(body.level) || !std::isfinite(body.bed) || !(body.bed <= body.level)) return false;
    switch (body.shape) {
    case WaterShape::Box:
        if (!(body.half.x > 0.f) || !(body.half.y > 0.f)) return false;
        break;
    case WaterShape::Polygon:
        if (body.pointCount < 3 || body.pointCount > kWaterMaxPoints) return false;
        break;
    case WaterShape::HalfPlane:
        break;
    }
    if (!std::isfinite(body.shelfBed) || !std::isfinite(body.shelf) || !std::isfinite(body.batter) || body.shelf < 0.f ||
        body.batter < 0.f)
        return false;
    const float axis = std::hypot(body.cosYaw, body.sinYaw);
    if (!(axis > 0.f)) return false;
    bodies_[count_] = body;
    bodies_[count_].cosYaw = body.cosYaw / axis;
    bodies_[count_].sinYaw = body.sinYaw / axis;
    ++count_;
    return true;
}

int WaterBodies::bodyAt(Vec2 p) const {
    int best = -1;
    for (std::size_t i = 0; i < count_; ++i) {
        const WaterBody& b = bodies_[i];
        if (!b.covers(p)) continue;
        if (best < 0) {
            best = static_cast<int>(i);
            continue;
        }
        const WaterBody& a = bodies_[static_cast<std::size_t>(best)];
        if (b.level > a.level || (b.level == a.level && b.bed < a.bed)) best = static_cast<int>(i);
    }
    return best;
}

float WaterBodies::surfaceOf(std::size_t i, Vec2 p, float t) const {
    const WaterBody& b = bodies_[i];
    if (b.waves && waves_) return b.level + seaHeight(*waves_, p.x * 100.f, p.y * 100.f, t) * 0.01f;
    return b.level;
}

bool WaterBodies::surfaceHeight(Vec2 p, float t, float& out) const {
    const int i = bodyAt(p);
    if (i < 0) return false;
    out = surfaceOf(static_cast<std::size_t>(i), p, t);
    return true;
}

float WaterBodies::depth(Vec2 p) const {
    const int i = bodyAt(p);
    if (i < 0) return 0.f;
    const WaterBody& b = bodies_[static_cast<std::size_t>(i)];
    return b.level - b.bedAt(p);
}

bool WaterBodies::addExit(const WaterExit& exit) {
    if (exitCount_ >= kWaterMaxExits) return false;
    const float len = std::hypot(exit.out.x, exit.out.y);
    if (!(len > 0.f) || !std::isfinite(len) || !std::isfinite(exit.foot.x) || !std::isfinite(exit.foot.y) ||
        !std::isfinite(exit.top.x) || !std::isfinite(exit.top.y) || !std::isfinite(exit.topZ))
        return false;
    exits_[exitCount_] = exit;
    exits_[exitCount_].out = {exit.out.x / len, exit.out.y / len};
    ++exitCount_;
    return true;
}

int WaterBodies::nearestExit(Vec2 p, float within) const {
    int best = -1;
    float bestSq = within < 0.f ? 3.0e38f : within * within;
    for (std::size_t i = 0; i < exitCount_; ++i) {
        const float dx = exits_[i].foot.x - p.x, dy = exits_[i].foot.y - p.y;
        const float sq = dx * dx + dy * dy;
        if (sq <= bestSq) {
            bestSq = sq;
            best = static_cast<int>(i);
        }
    }
    return best;
}

Vec2 WaterBodies::flowAt(Vec2 p) const {
    const int i = bodyAt(p);
    if (i < 0) return {};
    const WaterBody& b = bodies_[static_cast<std::size_t>(i)];
    Vec2 push = b.flow;
    if (b.shape == WaterShape::Box) {
        const float speed = std::hypot(b.flow.x, b.flow.y);
        if (speed > 0.f) {
            // Slack toward the walls either side of the flow: across the box, from its middle line.
            const Vec2 l = boxLocal(b, p);
            const float fx = (b.flow.x * b.cosYaw + b.flow.y * b.sinYaw) / speed;
            const float fy = (-b.flow.x * b.sinYaw + b.flow.y * b.cosYaw) / speed;
            const float across = std::fabs(-l.x * fy + l.y * fx);
            const float halfAcross = std::fabs(b.half.x * fy) + std::fabs(b.half.y * fx);
            const float u = halfAcross > 0.f ? std::min(across / halfAcross, 1.f) : 0.f;
            const float share = 1.f - (1.f - kWaterBankSlack) * u * u;
            push = {b.flow.x * share, b.flow.y * share};
        }
    } else if (b.shape == WaterShape::HalfPlane && b.buoyLine > 0.f) {
        const float past = b.out(p) - b.buoyLine;
        if (past > 0.f) {
            const float pull = std::min(past * kWaterBuoyPull, kWaterBuoyPullMax);
            push.x -= b.cosYaw * pull;
            push.y -= b.sinYaw * pull;
        }
    }
    return push;
}

bool WaterBodies::overWater(Vec2 p, float groundZ, float margin) const {
    for (std::size_t i = 0; i < count_; ++i) {
        const WaterBody& b = bodies_[i];
        if (b.covers(p) && !(groundZ > b.level + margin)) return true;
    }
    return false;
}

bool WaterBodies::onSteps(Vec2 p, float groundZ, float slack, float margin) const {
    for (std::size_t i = 0; i < exitCount_; ++i) {
        const WaterExit& e = exits_[i];
        if (e.kind != WaterExitKind::Steps || !(groundZ < e.topZ - slack)) continue;
        // Across the wall: 0 at the wall's face, growing into the water. Along it: 0 at the head, `run` at the foot.
        const float across = kWaterStepsFootIn - ((p.x - e.foot.x) * e.out.x + (p.y - e.foot.y) * e.out.y);
        if (across < -margin || across > kWaterStepsWidth + margin) continue;
        const Vec2 wall{-e.out.y, e.out.x};
        const float run = (e.foot.x - e.top.x) * wall.x + (e.foot.y - e.top.y) * wall.y;
        const float along = ((p.x - e.top.x) * wall.x + (p.y - e.top.y) * wall.y) * (run < 0.f ? -1.f : 1.f);
        if (along >= -margin && along <= std::fabs(run) + margin) return true;
    }
    return false;
}

WaterCamera WaterBodies::camera(Vec2 p, float z, float t, float margin) const {
    WaterCamera cam;
    for (std::size_t i = 0; i < count_; ++i) {
        const WaterBody& b = bodies_[i];
        if (!b.covers(p)) continue;
        const float surface = surfaceOf(i, p, t);
        if (!(z < surface + margin) || z < b.bed - margin) continue;
        const float depth = std::max(surface - z, 0.f);
        if (!cam.under || depth > cam.depth) {
            cam.under = true;
            cam.body = static_cast<int>(i);
            cam.depth = depth;
            cam.surface = surface;
        }
    }
    return cam;
}

float waterWadeFactor(float depth) {
    if (!(depth > 0.f)) return 1.f;
    if (depth <= kWaterWadeDepth) return 1.f - (1.f - kWaterWadeFactor) * (depth / kWaterWadeDepth);
    if (depth >= kWaterSwimDepth) return kWaterDeepFactor;
    return kWaterWadeFactor -
           (kWaterWadeFactor - kWaterDeepFactor) * ((depth - kWaterWadeDepth) / (kWaterSwimDepth - kWaterWadeDepth));
}

WaterState waterState(const WaterBodies& water, const WaterCapsule& capsule, float t) {
    WaterState s;
    const int i = water.bodyAt(capsule.p);
    if (i < 0) return s;
    const WaterBody& b = water.body(static_cast<std::size_t>(i));
    const float surface = water.surfaceOf(static_cast<std::size_t>(i), capsule.p, t);
    const float immersion = surface - capsule.feetZ;
    if (!(immersion > 0.f)) return s; // over the water: on a bridge, in the air
    const float ground = capsule.hasGround && std::isfinite(capsule.groundZ) ? capsule.groundZ : b.bedAt(capsule.p);
    const float column = std::max(surface - ground, 0.f);
    s.body = i;
    s.surface = surface;
    s.column = column;
    s.immersion = immersion;
    s.submerged = capsule.height > 0.f ? std::clamp(immersion / capsule.height, 0.f, 1.f) : 1.f;
    s.flow = water.flowAt(capsule.p);
    const bool swim = column > kWaterSwimDepth || (capsule.swimming && column >= kWaterSwimExit);
    if (swim) {
        s.mode = WaterMode::Swim;
        s.speedFactor = kWaterDeepFactor;
        return s;
    }
    // Wading: as deep as the feet are in it (a hop out of a wade is less held than the wade).
    const float wet = std::min(column, immersion);
    s.mode = wet > kWaterWadeDepth ? WaterMode::DeepWade : WaterMode::Wade;
    s.speedFactor = waterWadeFactor(wet);
    return s;
}

float waterSwimFeetZ(float surface, float t) {
    return surface - kWaterSwimDraft + kWaterBobHeight * std::sin(kTwoPi * t / kWaterBobPeriod);
}

float WaterRipples::amplitudeOf(const Slot& s) {
    if (!(s.life > 0.f) || s.age >= s.life) return 0.f;
    const float t = s.age / s.life;
    const float rise = std::min(s.age / 0.06f, 1.f);
    const float fall = 1.f - t;
    return s.strength * rise * fall * std::sqrt(fall);
}

void WaterRipples::add(Vec2 p, float strength, float life, float speed, float radius) {
    if (!(strength > 0.f) || !(life > 0.f) || !std::isfinite(p.x) || !std::isfinite(p.y)) return;
    std::size_t pick = 0;
    float faintest = 2.f;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        // An empty or spent slot first; else the faintest ring now. A ring just added has not risen yet, so its
        // full strength stands for it.
        const Slot& s = slots_[i];
        const bool dead = !(s.life > 0.f) || s.age >= s.life;
        const float now = dead ? -1.f : (s.age < 0.06f ? s.strength : amplitudeOf(s));
        if (now < faintest) {
            faintest = now;
            pick = i;
        }
    }
    Slot& s = slots_[pick];
    s.p = p;
    s.age = 0.f;
    s.life = life;
    s.strength = std::min(strength, 1.f);
    s.speed = std::max(speed, 0.f);
    s.radius = std::max(radius, 0.f);
}

void WaterRipples::advance(float dt) {
    if (!(dt > 0.f)) return;
    for (Slot& s : slots_) {
        if (!(s.life > 0.f)) continue;
        s.age += dt;
        if (s.age >= s.life) s = Slot{};
    }
}

WaterRing WaterRipples::ring(std::size_t i) const {
    WaterRing r;
    if (i >= slots_.size()) return r;
    const Slot& s = slots_[i];
    r.amplitude = amplitudeOf(s);
    if (r.amplitude > 0.f) {
        r.p = s.p;
        r.radius = s.radius + s.speed * s.age;
    }
    return r;
}

std::size_t WaterRipples::live() const {
    std::size_t n = 0;
    for (const Slot& s : slots_) n += (amplitudeOf(s) > 0.f || (s.life > 0.f && s.age < 0.06f)) ? 1u : 0u;
    return n;
}

float waterSplashStrength(float fallSpeed) {
    const float share = std::isfinite(fallSpeed) ? std::clamp(fallSpeed / kWaterSplashFall, 0.f, 1.f) : 0.f;
    return kWaterSplashMin + (1.f - kWaterSplashMin) * share;
}

WaterStirEvents WaterStir::step(WaterRipples& rings, const WaterState& state, Vec2 p, Vec2 velocity, float fallSpeed, float dt) {
    WaterStirEvents e;
    const bool in = state.mode != WaterMode::Dry && state.immersion > 0.f;
    if (in && !in_) {
        e.entered = true;
        e.splash = waterSplashStrength(fallSpeed);
        rings.add(p, e.splash, kWaterSplashLife, kWaterSplashSpeed, 0.15f);
        rings.add(p, 0.6f * e.splash, kWaterSplashLife, 0.55f * kWaterSplashSpeed, 0.05f);
        sinceRing_ = 0.f;
    }
    e.left = !in && in_;
    in_ = in;
    if (!in) {
        moving_ = false;
        return e;
    }
    const float speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);
    e.speed = std::isfinite(speed) ? speed : 0.f;
    moving_ = e.speed > (moving_ ? kWaterWakeStop : kWaterWakeSpeed);
    e.moving = moving_;
    sinceRing_ += std::max(dt, 0.f);
    if (e.moving && sinceRing_ >= kWaterWakeEvery) {
        sinceRing_ = 0.f;
        // The deeper in, the more water it pushes; a swimmer's stroke as much as a deep wade.
        const float push = std::clamp(0.35f + 0.5f * state.immersion, 0.35f, 0.85f);
        rings.add(p, push * std::clamp(e.speed / 1.5f, 0.4f, 1.f), kWaterWakeLife, std::clamp(e.speed / 3.f, 0.3f, 0.9f), 0.2f);
        e.fed = true;
    } else if (!e.moving && sinceRing_ >= kWaterIdleEvery) {
        sinceRing_ = 0.f;
        rings.add(p, 0.22f, kWaterIdleLife, 0.45f, 0.22f);
        e.fed = true;
    }
    return e;
}

void WaterWetness::step(float immersion, float height, float dt, float dryScale) {
    const float passed = std::max(dt, 0.f);
    if (immersion > 0.f && std::isfinite(immersion)) {
        // A full dunk carries the line over the head.
        const float reach = std::min(immersion, std::max(height, 0.f) + 0.2f);
        // What was wet higher up stays in the line while it is still more wet than dry.
        line = wetness >= 0.5f ? std::max(line, reach) : reach;
        wetness = 1.f;
        drip = 1.f;
        return;
    }
    drip = std::max(drip - passed / kWaterDripTime, 0.f);
    wetness = std::max(wetness - passed * std::max(dryScale, 0.f) / kWaterDryTime, 0.f);
    if (!(wetness > 0.f)) {
        line = 0.f;
        drip = 0.f;
    }
}

} // namespace ftcore
