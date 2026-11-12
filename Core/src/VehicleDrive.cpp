#include "ftcore/VehicleDrive.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace ftcore {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.f;
constexpr float kKmh = 100000.f / 3600.f; // cm/s in one km/h
constexpr int kMaxSubsteps = 32;

// Moves `v` toward `target` by at most `by`.
float approach(float v, float target, float by) {
    return v < target ? std::min(v + by, target) : std::max(v - by, target);
}

// Adds `dv` to `v` but stops at zero rather than crossing it (brakes and drag stop a car, they don't reverse it).
float slowBy(float v, float dv) {
    if (v > 0.f) return std::max(v - dv, 0.f);
    if (v < 0.f) return std::min(v + dv, 0.f);
    return 0.f;
}

// An angle in (-pi, pi].
float wrapAngle(float a) {
    if (!std::isfinite(a)) return 0.f;
    if (std::abs(a) > 64.f) a = std::remainder(a, 2.f * kPi); // a huge heading: one step, not a long loop
    while (a > kPi) a -= 2.f * kPi;
    while (a <= -kPi) a += 2.f * kPi;
    return a;
}

float finiteOr(float v, float d) { return std::isfinite(v) ? v : d; }

// Input as the model can use it: anything not finite reads as nothing.
VehicleInput cleaned(const VehicleInput& in) {
    VehicleInput c = in;
    c.throttle = finiteOr(in.throttle, 0.f);
    c.brake = finiteOr(in.brake, 0.f);
    c.steer = finiteOr(in.steer, 0.f);
    c.limit = std::max(finiteOr(in.limit, 0.f), 0.f);
    c.limitDecel = std::max(finiteOr(in.limitDecel, 0.f), 0.f);
    c.clamp = std::max(finiteOr(in.clamp, 0.f), 0.f);
    if (!std::isfinite(in.guide.dirX) || !std::isfinite(in.guide.dirY) || !std::isfinite(in.guide.offset)) c.guide.present = false;
    return c;
}

// A state the model can step: the motion values that are not finite are reset (the position is left as it is).
void heal(VehicleState& s) {
    s.forward = finiteOr(s.forward, 0.f);
    s.side = finiteOr(s.side, 0.f);
    s.steer = finiteOr(s.steer, 0.f);
    s.heading = finiteOr(s.heading, 0.f);
    s.yawRate = finiteOr(s.yawRate, 0.f);
    s.carry = finiteOr(s.carry, 0.f);
}

} // namespace

VehicleState VehicleDrive::parked(float x, float y, float heading) const {
    VehicleState s;
    s.x = x;
    s.y = y;
    s.heading = heading;
    return s;
}

float VehicleDrive::topSpeed(const VehicleState& s, const VehicleInput& in) const {
    const float own = s.latched && tuning_.guideSpeedCap > 0.f ? tuning_.guideSpeedCap : tuning_.speedCap;
    return in.limit > 0.f ? std::min(own, in.limit) : own;
}

void VehicleDrive::step(VehicleState& s, const VehicleInput& in, float dt) const {
    const float h = tuning_.substep > 0.f ? tuning_.substep : 1.f / 120.f;
    heal(s);
    s.carry = std::min(s.carry + std::max(finiteOr(dt, 0.f), 0.f), h * static_cast<float>(kMaxSubsteps));
    // The guide's offset moves with the vehicle between substeps (the line taken as straight over a frame).
    VehicleInput cur = cleaned(in);
    const float glen = std::sqrt(cur.guide.dirX * cur.guide.dirX + cur.guide.dirY * cur.guide.dirY);
    const float px = glen > 0.f ? -cur.guide.dirY / glen : 0.f;
    const float py = glen > 0.f ? cur.guide.dirX / glen : 0.f;
    for (int n = 0; n < kMaxSubsteps && s.carry >= h - 1e-6f; ++n) {
        const float x = s.x;
        const float y = s.y;
        substep(s, cur, h);
        cur.guide.offset += (s.x - x) * px + (s.y - y) * py;
        s.carry = std::max(s.carry - h, 0.f);
    }
}

void VehicleDrive::substep(VehicleState& s, const VehicleInput& raw, float h) const {
    if (!(h > 0.f) || !std::isfinite(h)) return;
    heal(s);
    const VehicleInput in = cleaned(raw);
    const VehicleTuning& t = tuning_;
    const float throttle = std::clamp(in.throttle, 0.f, 1.f);
    const float brake = std::clamp(in.brake, 0.f, 1.f);
    float v = s.forward;

    // The guide: let go when it ends, drifts away or the host detaches; latch when close and lined up.
    const VehicleGuide& g = in.guide;
    const float glen = std::sqrt(g.dirX * g.dirX + g.dirY * g.dirY);
    const bool usable = t.guideSpeedCap > 0.f && g.present && glen > 0.f && !in.detach;
    float guideHeading = 0.f;
    if (usable) {
        guideHeading = std::atan2(g.dirY, g.dirX);
        float off = wrapAngle(s.heading - guideHeading);
        if (std::abs(off) > kPi * 0.5f) { // running the other way along it
            guideHeading = wrapAngle(guideHeading + kPi);
            off = wrapAngle(s.heading - guideHeading);
        }
        if (s.latched) {
            if (std::abs(g.offset) > 2.f * t.latchDistance) s.latched = false;
        } else if (std::abs(g.offset) <= t.latchDistance && std::abs(off) <= t.latchAngle * kDeg) {
            s.latched = true;
        }
    } else {
        s.latched = false;
    }
    const float top = topSpeed(s, in);

    // Steering: toward the asked angle, the lock narrowing with speed; back toward straight at returnRate.
    const float fade = t.steerFadeSpeed > 0.f ? 1.f / (1.f + std::abs(v) / t.steerFadeSpeed) : 1.f;
    const float target = s.latched ? 0.f : std::clamp(in.steer, -1.f, 1.f) * t.maxSteer * kDeg * fade;
    const bool easing = std::abs(target) < std::abs(s.steer) || target * s.steer < 0.f;
    s.steer = approach(s.steer, target, (easing ? t.returnRate : t.steerRate) * kDeg * h);

    // Along the heading: drive, brake, reverse, the governor and drag.
    if (v > t.restSpeed) {
        if (v > top) {
            const bool limitBinds = in.limit > 0.f && top >= in.limit && in.limitDecel > 0.f;
            v = std::max(v - (limitBinds ? in.limitDecel : t.governorDecel) * h, top);
        } else {
            const float taper = std::clamp((top - v) / (0.1f * top), 0.f, 1.f); // the engine fades in the last tenth
            v = std::min(v + throttle * t.engineAccel * taper * h, top);
        }
        v = slowBy(v, brake * t.brakeDecel * h);
    } else if (v < -t.restSpeed) {
        if (v < -t.reverseSpeed) {
            v = std::min(v + t.governorDecel * h, -t.reverseSpeed); // pushed back faster: reined in, not snapped
        } else {
            const float taper = std::clamp((t.reverseSpeed + v) / (0.1f * t.reverseSpeed), 0.f, 1.f);
            v = std::max(v - brake * t.reverseAccel * taper * h, -t.reverseSpeed);
        }
        v = slowBy(v, throttle * t.brakeDecel * h);
    } else if (throttle > 0.f && throttle >= brake) {
        v = top >= t.restSpeed ? std::min(v + throttle * t.engineAccel * h, std::max(top, v)) : 0.f; // held: limited to a stop
    } else if (brake > 0.f) {
        v = std::max(v - brake * t.reverseAccel * h, std::min(-t.reverseSpeed, v));
    }
    if (in.handbrake) v = slowBy(v, t.handbrakeDecel * h);
    if (throttle <= 0.f && brake <= 0.f) {
        v = slowBy(v, (t.rollDrag + t.airDrag * v * v) * h);
        if (std::abs(v) < t.restSpeed) v = 0.f;
    }
    if (in.clamp > 0.f) v = std::clamp(v, -in.clamp, in.clamp);
    s.forward = v;

    if (s.latched) {
        // On the guide: the heading turns onto it, the vehicle is drawn onto its line, nothing slides.
        const float most = t.guideTurn * kDeg * h;
        const float turn = std::clamp(wrapAngle(guideHeading - s.heading), -most, most);
        s.heading = wrapAngle(s.heading + turn);
        s.yawRate = turn / h;
        s.side = 0.f;
        const float pull = std::clamp(-g.offset, -t.guidePull * h, t.guidePull * h);
        s.x += s.velX() * h - g.dirY / glen * pull;
        s.y += s.velY() * h + g.dirX / glen * pull;
        return;
    }

    // Turning: the bicycle model's yaw, held under the tyres' grip; the heading turns under the car's momentum.
    const float yawBoost = in.handbrake ? t.handbrakeYaw : 1.f;
    float yaw = t.wheelbase > 0.f ? v * std::tan(s.steer) / t.wheelbase * yawBoost : 0.f;
    const float maxYaw = t.gripLat * yawBoost / std::max(std::abs(v), 1.f);
    yaw = std::clamp(yaw, -maxYaw, maxYaw);
    s.yawRate = yaw;
    const float d = yaw * h;
    s.heading = wrapAngle(s.heading + d);
    const float c = std::cos(d);
    const float sn = std::sin(d);
    const float f = s.forward * c + s.side * sn;
    const float sd = -s.forward * sn + s.side * c;
    s.forward = f;
    // The tyres pull the sideways speed back to nothing; the handbrake lets most of it slide.
    s.side = approach(sd, 0.f, t.gripLat * (in.handbrake ? t.handbrakeGrip : 1.f) * h);
    // The hard clamp holds the ground speed, so a slide into the zone is held to it too.
    if (in.clamp > 0.f) {
        const float sp = std::sqrt(s.forward * s.forward + s.side * s.side);
        if (sp > in.clamp) {
            s.forward *= in.clamp / sp;
            s.side *= in.clamp / sp;
        }
    }

    s.x += s.velX() * h;
    s.y += s.velY() * h;
}

float VehicleDrive::collide(VehicleState& s, float nx, float ny) const {
    const float len = std::sqrt(nx * nx + ny * ny);
    if (!(len > 0.f) || !std::isfinite(len)) return 0.f;
    heal(s);
    nx /= len;
    ny /= len;
    float vx = s.velX();
    float vy = s.velY();
    const float into = vx * nx + vy * ny;
    if (into >= 0.f) return 0.f;
    float tx = vx - into * nx;
    float ty = vy - into * ny;
    const float along = std::sqrt(tx * tx + ty * ty);
    if (along > 0.f) {
        const float keep = std::max(along - tuning_.scrape * -into, 0.f) / along;
        tx *= keep;
        ty *= keep;
    }
    vx = tx - tuning_.restitution * into * nx;
    vy = ty - tuning_.restitution * into * ny;
    const float c = std::cos(s.heading);
    const float sn = std::sin(s.heading);
    s.forward = vx * c + vy * sn;
    s.side = -vx * sn + vy * c;
    return -into;
}

bool VehicleDrive::mountKerb(VehicleState& s, float height) const {
    if (!std::isfinite(height)) return false;
    if (height <= 0.f) return true;
    if (height > tuning_.kerbMax) return false;
    const float sp = s.speed();
    if (sp > 0.f) {
        const float keep = std::max(sp - tuning_.kerbLoss * height, 0.f) / sp;
        s.forward *= keep;
        s.side *= keep;
    }
    return true;
}

namespace {

struct Reader {
    std::string* error;
    bool ok = true;

    void fail(std::string_view name, const char* why) {
        if (!ok) return;
        ok = false;
        if (error) *error = "drive." + std::string(name) + ": " + why;
    }
    bool has(const JsonValue& obj, const char* name) const { return !obj[name].isAbsent(); }
    void number(const JsonValue& obj, const char* name, float& v, double lo, double hi, double scale = 1.0) {
        if (!ok) return;
        const JsonValue m = obj[name];
        if (m.isAbsent()) return;
        if (!m.isNumber()) return fail(name, "not a number");
        const double d = m.number();
        if (!(d >= lo && d <= hi)) return fail(name, "out of range");
        v = static_cast<float>(d * scale);
    }
    void only(const JsonValue& obj, std::initializer_list<std::string_view> known) {
        for (std::uint32_t i = 0; ok && i < obj.size(); ++i) {
            const std::string_view k = obj.key(i);
            bool found = false;
            for (const std::string_view n : known) found = found || n == k;
            if (!found) fail(k, "not a known tunable");
        }
    }
};

} // namespace

bool importVehicleTuning(std::string_view json, VehicleTuning& out, std::string* error) {
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) {
        if (error) *error = parseError;
        return false;
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        if (error) *error = "the file is not an object";
        return false;
    }
    if (root["version"].integer(0) != 1) {
        if (error) *error = "version is not 1";
        return false;
    }
    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        if (k != "version" && k != "drive" && k != "name") {
            if (error) *error = std::string(k) + ": not a known section";
            return false;
        }
    }
    const JsonValue s = root["drive"];
    if (!s.isAbsent() && !s.isObject()) {
        if (error) *error = "drive: not an object";
        return false;
    }
    VehicleTuning t = out;
    Reader rd{error};
    rd.only(s, {"wheelbase", "maxSteer", "steerRate", "returnRate", "steerFadeSpeed", "engineAccel", "brakeDecel",
                "reverseAccel", "reverseSpeed", "reverseKmh", "speedCap", "speedCapKmh", "governorDecel", "rollDrag",
                "airDrag", "restSpeed", "gripLat", "handbrakeGrip", "handbrakeYaw", "handbrakeDecel", "restitution",
                "scrape", "kerbMax", "kerbLoss", "substep", "guideSpeedCap", "guideKmh", "latchDistance", "latchAngle",
                "guideTurn", "guidePull"});
    if (rd.ok && rd.has(s, "guideSpeedCap") && rd.has(s, "guideKmh")) rd.fail("guideKmh", "guideSpeedCap given too");
    if (rd.ok && rd.has(s, "speedCap") && rd.has(s, "speedCapKmh")) rd.fail("speedCapKmh", "speedCap given too");
    if (rd.ok && rd.has(s, "reverseSpeed") && rd.has(s, "reverseKmh")) rd.fail("reverseKmh", "reverseSpeed given too");
    rd.number(s, "wheelbase", t.wheelbase, 50.0, 2000.0);
    rd.number(s, "maxSteer", t.maxSteer, 1.0, 60.0);
    rd.number(s, "steerRate", t.steerRate, 1.0, 2000.0);
    rd.number(s, "returnRate", t.returnRate, 1.0, 2000.0);
    rd.number(s, "steerFadeSpeed", t.steerFadeSpeed, 0.0, 10000.0);
    rd.number(s, "engineAccel", t.engineAccel, 0.0, 5000.0);
    rd.number(s, "brakeDecel", t.brakeDecel, 0.0, 5000.0);
    rd.number(s, "reverseAccel", t.reverseAccel, 0.0, 5000.0);
    rd.number(s, "reverseSpeed", t.reverseSpeed, 0.0, 5000.0);
    rd.number(s, "reverseKmh", t.reverseSpeed, 0.0, 180.0, kKmh);
    rd.number(s, "speedCap", t.speedCap, 1.0, 10000.0);
    rd.number(s, "speedCapKmh", t.speedCap, 1.0, 360.0, kKmh);
    rd.number(s, "governorDecel", t.governorDecel, 0.0, 5000.0);
    rd.number(s, "rollDrag", t.rollDrag, 0.0, 1000.0);
    rd.number(s, "airDrag", t.airDrag, 0.0, 0.01);
    rd.number(s, "restSpeed", t.restSpeed, 0.0, 200.0);
    rd.number(s, "gripLat", t.gripLat, 0.0, 5000.0);
    rd.number(s, "handbrakeGrip", t.handbrakeGrip, 0.0, 1.0);
    rd.number(s, "handbrakeYaw", t.handbrakeYaw, 1.0, 4.0);
    rd.number(s, "handbrakeDecel", t.handbrakeDecel, 0.0, 5000.0);
    rd.number(s, "restitution", t.restitution, 0.0, 1.0);
    rd.number(s, "scrape", t.scrape, 0.0, 2.0);
    rd.number(s, "kerbMax", t.kerbMax, 0.0, 100.0);
    rd.number(s, "kerbLoss", t.kerbLoss, 0.0, 200.0);
    rd.number(s, "substep", t.substep, 1.0 / 1000.0, 1.0 / 30.0);
    rd.number(s, "guideSpeedCap", t.guideSpeedCap, 0.0, 10000.0);
    rd.number(s, "guideKmh", t.guideSpeedCap, 0.0, 360.0, kKmh);
    rd.number(s, "latchDistance", t.latchDistance, 0.0, 1000.0);
    rd.number(s, "latchAngle", t.latchAngle, 0.0, 90.0);
    rd.number(s, "guideTurn", t.guideTurn, 1.0, 2000.0);
    rd.number(s, "guidePull", t.guidePull, 0.0, 5000.0);
    if (!rd.ok) return false;
    out = t;
    return true;
}

} // namespace ftcore
