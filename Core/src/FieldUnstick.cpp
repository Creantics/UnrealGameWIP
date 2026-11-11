#include "ftcore/FieldUnstick.h"

#include "ftcore/Json.h"

#include <cmath>

namespace ftcore {

bool importUnstickRules(std::string_view json, UnstickRules& out, std::string* error) {
    JsonDocument doc;
    if (!doc.parse(json, error)) return false;
    const JsonValue root = doc.root();
    const auto fail = [&](std::string_view key, const char* why) {
        if (error) *error = "unstick" + (key.empty() ? std::string() : "." + std::string(key)) + ": " + why;
        return false;
    };
    if (!root.isObject()) return fail("", "not an object");
    UnstickRules r = out;
    struct Field {
        const char* name;
        float* value;
        double lo, hi;
    };
    const Field fields[] = {
        {"walkableZ", &r.walkableZ, 0.1, 1.0},   {"slideSpeed", &r.slideSpeed, 0.0, 30.0},
        {"stuckTime", &r.stuckTime, 0.1, 30.0},  {"stuckMove", &r.stuckMove, 0.01, 10.0},
        {"fallTime", &r.fallTime, 0.5, 60.0},    {"ringSpacing", &r.ringSpacing, 0.1, 20.0},
        {"backoff", &r.backoff, 0.0, 50.0},      {"fadeTime", &r.fadeTime, 0.0, 3.0},
        {"slideAccel", &r.slideAccel, 0.1, 100.0}, {"teleportJump", &r.teleportJump, 0.5, 1000.0},
    };
    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        if (k == "version") continue;
        bool known = false;
        for (const Field& f : fields) known = known || k == f.name;
        if (!known) return fail(k, "not a known tunable");
    }
    for (const Field& f : fields) {
        const JsonValue v = root[f.name];
        if (v.isAbsent()) continue;
        if (!v.isNumber()) return fail(f.name, "not a number");
        const double d = v.number();
        if (!(d >= f.lo && d <= f.hi)) return fail(f.name, "out of range");
        *f.value = static_cast<float>(d);
    }
    out = r;
    return true;
}

void FieldUnstick::reset() {
    head_ = 0;
    count_ = 0;
    stuckFor_ = 0.f;
    fallFor_ = 0.f;
    anchored_ = false;
    sliding_ = false;
    slideDir_ = Vec2{};
    fadeLeft_ = 0.f;
    hasLast_ = false;
}

void FieldUnstick::push(Vec2 at, float z) {
    if (count_ > 0) {
        const UnstickPoint& last = ring_[(head_ + kRing - 1) % kRing];
        const float dx = at.x - last.at.x, dy = at.y - last.at.y, dz = z - last.z;
        if (dx * dx + dy * dy + dz * dz < rules_.ringSpacing * rules_.ringSpacing) return;
    }
    ring_[head_] = UnstickPoint{at, z};
    head_ = static_cast<std::uint8_t>((head_ + 1) % kRing);
    if (count_ < kRing) ++count_;
}

bool FieldUnstick::rescue(Vec2 at) {
    if (count_ == 0) return false;
    // The newest safe point at least backoff away (the last one is often the lip the lead fell off), else the oldest.
    std::uint8_t pick = static_cast<std::uint8_t>((head_ + kRing - count_) % kRing);
    for (std::uint8_t n = 1; n <= count_; ++n) {
        const std::uint8_t i = static_cast<std::uint8_t>((head_ + kRing - n) % kRing);
        const float dx = ring_[i].at.x - at.x, dy = ring_[i].at.y - at.y;
        if (dx * dx + dy * dy >= rules_.backoff * rules_.backoff) {
            pick = i;
            break;
        }
    }
    rescueTo_ = ring_[pick];
    lastAt_ = rescueTo_.at; // the host puts the feet there: not a teleport
    lastZ_ = rescueTo_.z;
    stuckFor_ = 0.f;
    fallFor_ = 0.f;
    anchored_ = false;
    sliding_ = false;
    fadeLeft_ = rules_.fadeTime;
    return true;
}

bool FieldUnstick::tick(float dt, Vec2 at, float z, bool onGround, Vec2 floorNormalXY, float floorNormalZ, bool inAir,
                        int floorWalkable) {
    if (!(dt > 0.f) || !std::isfinite(dt)) return false; // a paused or zero frame: nothing moves, nothing counts
    if (hasLast_) {
        const float jx = at.x - lastAt_.x, jy = at.y - lastAt_.y, jz = z - lastZ_;
        if (jx * jx + jy * jy + jz * jz > rules_.teleportJump * rules_.teleportJump) {
            const float fade = fadeLeft_;
            reset(); // a teleport (a scene, a map's spawn, a swap): the old safe points are somewhere else
            fadeLeft_ = fade;
        }
    }
    lastAt_ = at;
    lastZ_ = z;
    hasLast_ = true;
    fadeLeft_ = fadeLeft_ > dt ? fadeLeft_ - dt : 0.f;
    const bool walkable = onGround && (floorWalkable >= 0 ? floorWalkable > 0 : floorNormalZ >= rules_.walkableZ);
    // Too steep to stand: down the slope's fall line (the normal's ground part points downhill).
    const float nl = std::sqrt(floorNormalXY.x * floorNormalXY.x + floorNormalXY.y * floorNormalXY.y);
    sliding_ = onGround && !walkable && nl > 1e-4f && floorNormalZ < 0.999f;
    slideDir_ = sliding_ ? Vec2{floorNormalXY.x / nl, floorNormalXY.y / nl} : Vec2{};

    if (walkable) {
        push(at, z);
        stuckFor_ = 0.f;
        fallFor_ = 0.f;
        anchored_ = false;
        return false;
    }
    fallFor_ = inAir && !onGround ? fallFor_ + dt : 0.f;
    if (!anchored_) {
        anchor_ = at;
        anchorZ_ = z;
        anchored_ = true;
        stuckFor_ = 0.f;
    }
    const float dx = at.x - anchor_.x, dy = at.y - anchor_.y, dz = z - anchorZ_;
    if (dx * dx + dy * dy + dz * dz > rules_.stuckMove * rules_.stuckMove) {
        anchor_ = at; // still getting somewhere (a long drop too): count again from here
        anchorZ_ = z;
        stuckFor_ = 0.f;
    } else {
        stuckFor_ += dt;
    }
    if (stuckFor_ >= rules_.stuckTime || fallFor_ > rules_.fallTime) return rescue(at);
    return false;
}

Vec2 FieldUnstick::slideVelocity(Vec2 vel, float dt) const {
    if (!sliding_ || !(dt > 0.f)) return vel;
    const float along = vel.x * slideDir_.x + vel.y * slideDir_.y;
    if (along >= rules_.slideSpeed) return vel;
    const float k = dt * rules_.slideAccel < 1.f ? dt * rules_.slideAccel : 1.f;
    const float add = (rules_.slideSpeed - along) * k;
    return Vec2{vel.x + slideDir_.x * add, vel.y + slideDir_.y * add};
}

} // namespace ftcore
