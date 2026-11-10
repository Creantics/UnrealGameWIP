#include "ftcore/AimStance.h"

#include "ftcore/Json.h"

#include <cmath>

namespace ftcore {

bool importAimRules(std::string_view json, AimRules& out, std::string* error) {
    JsonDocument doc;
    if (!doc.parse(json, error)) return false;
    const JsonValue root = doc.root();
    const auto fail = [&](std::string_view key, const char* why) {
        if (error) *error = "aim" + (key.empty() ? std::string() : "." + std::string(key)) + ": " + why;
        return false;
    };
    if (!root.isObject()) return fail("", "not an object");
    AimRules r = out;
    struct Field {
        const char* name;
        float* value;
        double lo, hi;
    };
    const Field fields[] = {
        {"blendIn", &r.blendIn, 0.0, 2.0},         {"blendOut", &r.blendOut, 0.0, 2.0},
        {"speedScale", &r.speedScale, 0.05, 1.0},  {"turnRate", &r.turnRate, 0.5, 60.0},
        {"armScale", &r.armScale, 0.1, 1.0},       {"shoulderRight", &r.shoulderRight, -200.0, 200.0},
        {"shoulderUp", &r.shoulderUp, -200.0, 200.0}, {"fov", &r.fov, 20.0, 120.0},
        {"reticleRange", &r.reticleRange, 1.0, 500.0}, {"lungeScale", &r.lungeScale, 0.0, 10.0},
        {"lungeStop", &r.lungeStop, 0.0, 1.5},     {"heavyHold", &r.heavyHold, 0.05, 2.0},
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

AimPhase AimStance::tick(float dt, bool held, FieldBody body) {
    if (!(dt > 0.f) || !std::isfinite(dt)) dt = 0.f;
    const bool on = held && aimAllowed(body);
    if (on) {
        alpha_ = rules_.blendIn > 0.f ? alpha_ + dt / rules_.blendIn : 1.f;
        if (alpha_ >= 1.f) alpha_ = 1.f;
        phase_ = alpha_ >= 1.f ? AimPhase::Held : AimPhase::In;
    } else {
        alpha_ = rules_.blendOut > 0.f ? alpha_ - dt / rules_.blendOut : 0.f;
        if (alpha_ <= 0.f) alpha_ = 0.f;
        phase_ = alpha_ <= 0.f ? AimPhase::Off : AimPhase::Out;
    }
    return phase_;
}

float AimStance::alpha() const {
    // Smoothstep: the camera eases in and out of the shoulder.
    const float a = alpha_;
    return a * a * (3.f - 2.f * a);
}

PressHold::Out PressHold::tick(float dt, bool down, float hold) {
    Out o;
    if (!(dt > 0.f) || !std::isfinite(dt)) dt = 0.f;
    if (down) {
        if (downFor_ < 0.f) {
            downFor_ = 0.f;
            heavyDone_ = false;
        } else {
            downFor_ += dt;
        }
        if (!heavyDone_ && downFor_ >= hold) {
            heavyDone_ = true;
            o.heavy = true;
        }
    } else if (downFor_ >= 0.f) {
        o.light = !heavyDone_;
        downFor_ = -1.f;
        heavyDone_ = false;
    }
    return o;
}

} // namespace ftcore
