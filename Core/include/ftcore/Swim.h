// Underwater free swim for an explore lead: dive, swim in 3D along the look, breath, the rescue and the outer edge.
// Plain C++20, no Unreal headers, no allocation in step(). Header-only (inline), so it needs no wrapper in the Unreal
// module. The host calls step() once a frame while the lead is in deep water, moves the body to the step's position
// and reads the flags. Metres, seconds, z up; Unreal's axes (x forward, y right). Data/Swim/swim.json is read by
// importSwimTuning.
//
// The rules:
// - Surface: the host's own surface swim moves the body (Swim passes the position through, no edge push); breath
//   refills. A crouch dives (Diving) where the env allows it: deepAllowed and a column of at least kSwimDeepMin;
//   else diveRefused.
// - Diving: straight down at swimSpeed to diveDepth under the surface (or the bed's clearance), then Under.
// - Under: along the look in full 3D, the move axes on it (forward on the look, right on its horizontal right) at
//   swimSpeed, sprintSpeed while sprinting with a move input; with no move input it drifts up at buoyancy. Reaching
//   the surface hands back to Surface (surfaced).
// - A jump in Diving or Under: Surfacing, straight up at swimSpeed to the surface, then Surface (surfaced).
// - Every state: held bedClearance off the bed and stopped at the surface.
// - Under water (Diving, Under, Surfacing) the outer edge pushes back toward edgeInward: 0 from edgeRampMetres
//   inside it, rising to edgePushSpeed at the edge and past it, so a sprint cannot cross it. No wall, no message.
// - Breath: used 1 s a second under water, sprintBreathRate while sprinting; refilled at the surface or in an air
//   pocket (empty to full in refillSeconds). warning from warningSeconds left while it is being used; at 0 the
//   vitality drains drainPerSecond (for the part of the step past empty).
// - Vitality 0 under water: rescue. The host carries the lead to the nearest exit; Swim is reset (Surface, full
//   breath) at rescueVitality.
#pragma once

#include "ftcore/Json.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

enum class SwimState : std::uint8_t { Surface, Diving, Under, Surfacing };

inline constexpr float kSwimDeepMin = 3.0f;  // a dive needs a water column at least this deep (m)
inline constexpr float kSwimMaxStep = 0.25f; // a longer step is clamped to this (s)

struct SwimVec {
    float x = 0.f, y = 0.f, z = 0.f;
};

// The tunables (Data/Swim/swim.json); the ranges are validSwimTuning's.
struct SwimTuning {
    float swimSpeed = 2.2f;       // m/s (0.5..6)
    float sprintSpeed = 3.2f;     // m/s (swimSpeed..8)
    float buoyancy = 0.2f;        // m/s up under water with no move input (0..2)
    float bedClearance = 0.3f;    // held this far off the bed (0..2)
    float diveDepth = 1.0f;       // a dive ends this far under the surface (0.2..3)
    float breathSeconds = 45.f;   // a full breath (5..300)
    float sprintBreathRate = 2.f; // breath used per second while sprinting (1..5)
    float warningSeconds = 10.f;  // the warning from this much breath left (0..breathSeconds)
    float drainPerSecond = 0.05f; // vitality share lost per second with no breath (0.01..1)
    float refillSeconds = 2.f;    // empty to full at the surface or in an air pocket (0.1..30)
    float rescueVitality = 0.25f; // the vitality share a rescued lead is left at (0.01..1)
    float edgeRampMetres = 8.f;   // the edge's push rises over this distance (0.5..50)
    float edgePushSpeed = 4.f;    // m/s inward at the edge (above sprintSpeed..12)
};

namespace swim_detail {

struct Range {
    std::string_view name;
    float SwimTuning::*field;
    float lo, hi;
};

// The fixed ranges; the cross-field ones (sprintSpeed, warningSeconds, edgePushSpeed) are in validSwimTuning.
inline constexpr Range kRanges[] = {
    {"swimSpeed", &SwimTuning::swimSpeed, 0.5f, 6.f},
    {"sprintSpeed", &SwimTuning::sprintSpeed, 0.5f, 8.f},
    {"buoyancy", &SwimTuning::buoyancy, 0.f, 2.f},
    {"bedClearance", &SwimTuning::bedClearance, 0.f, 2.f},
    {"diveDepth", &SwimTuning::diveDepth, 0.2f, 3.f},
    {"breathSeconds", &SwimTuning::breathSeconds, 5.f, 300.f},
    {"sprintBreathRate", &SwimTuning::sprintBreathRate, 1.f, 5.f},
    {"warningSeconds", &SwimTuning::warningSeconds, 0.f, 300.f},
    {"drainPerSecond", &SwimTuning::drainPerSecond, 0.01f, 1.f},
    {"refillSeconds", &SwimTuning::refillSeconds, 0.1f, 30.f},
    {"rescueVitality", &SwimTuning::rescueVitality, 0.01f, 1.f},
    {"edgeRampMetres", &SwimTuning::edgeRampMetres, 0.5f, 50.f},
    {"edgePushSpeed", &SwimTuning::edgePushSpeed, 0.5f, 12.f},
};

inline bool inRange(float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; }
inline bool finite(const SwimVec& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline std::string text(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", double(v));
    return buf;
}

} // namespace swim_detail

inline bool validSwimTuning(const SwimTuning& t) {
    for (const swim_detail::Range& r : swim_detail::kRanges)
        if (!swim_detail::inRange(t.*r.field, r.lo, r.hi)) return false;
    return t.sprintSpeed >= t.swimSpeed && t.warningSeconds <= t.breathSeconds && t.edgePushSpeed > t.sprintSpeed;
}

// What the player asks this frame. A bad value is replaced (SwimStep::inputOk false), never trusted.
struct SwimInput {
    SwimVec look{1.f, 0.f, 0.f}; // the camera's look (normalised here; zero or not finite: the last good one)
    float forward = 0.f;         // move axes, -1..1 (outside: clamped; not finite: 0); longer than 1 together: scaled
    float right = 0.f;
    bool sprint = false;
    bool crouch = false; // at the surface: dive
    bool jump = false;   // diving or under: rise to the surface
};

// The water at the lead this frame (the world lane's WaterBody rows and the character).
struct SwimEnv {
    SwimVec position;            // the lead's body reference point
    float surfaceZ = 0.f;        // the water surface over the lead
    float bedZ = -10.f;          // the bed under it (below surfaceZ)
    float column = 10.f;         // the water column's depth here (a dive needs kSwimDeepMin)
    bool deepAllowed = false;    // the body is swimDeep and the point is inside its dive area
    bool airPocket = false;      // the lead's head is in an air pocket (breath refills under water)
    float edgeDistance = 1.0e9f; // metres inside the outer edge (0 or less: at or past it; not finite: far)
    SwimVec edgeInward;          // horizontal, back into the basin (zero or not finite: no push)
};

// What the lead may do while swimming, in every state: the character reads them.
struct SwimFlags {
    bool attack = false;     // no attacks
    bool dodge = false;      // no dodge roll
    bool weaponsOut = false; // weapons stowed
    bool interact = true;    // interact allowed
};

struct SwimStep {
    SwimState state = SwimState::Surface; // after this step
    SwimState from = SwimState::Surface;  // before it
    bool dived = false;       // Surface to Diving this step
    bool diveRefused = false; // a crouch at the surface where no dive is allowed
    bool surfaced = false;    // to Surface this step (not by a rescue)
    SwimVec position;         // where the body goes (clamped to the bed's clearance and the surface)
    SwimVec velocity;         // m/s: the move this step (the asked one when dt is 0)
    float dt = 0.f;           // the step used (0..kSwimMaxStep)
    bool sprinting = false;   // under, sprint held with a move input
    bool refilling = false;   // at the surface or in an air pocket
    float breath = 0.f;       // seconds left
    float breathShare = 1.f;  // breath / breathSeconds
    bool warning = false;     // breath in use with warningSeconds or less left
    bool draining = false;    // no breath left under water: vitality drains
    float vitality = 1.f;     // the share left (0..1)
    bool rescue = false;      // vitality reached 0 under water: carry the lead to the nearest exit (Swim is reset)
    float pushBack = 0.f;     // the outer edge's share applied this step (0..1)
    SwimFlags flags;
    bool inputOk = true;      // false: an input value was replaced (not finite, out of range)
    bool envOk = true;        // false: the env was unusable (not finite, bed not under the surface): nothing changed
};

class Swim {
public:
    // A tuning validSwimTuning refuses is replaced by the defaults (tuningOk() false).
    explicit Swim(const SwimTuning& t = SwimTuning{}) { reset(t); }
    // Surface, a full breath and full vitality.
    void reset(const SwimTuning& t) {
        tuningOk_ = validSwimTuning(t);
        tuning_ = tuningOk_ ? t : SwimTuning{};
        reset();
    }
    void reset() {
        state_ = SwimState::Surface;
        breath_ = tuning_.breathSeconds;
        vitality_ = 1.f;
        look_ = {1.f, 0.f, 0.f};
        right_ = {0.f, 1.f, 0.f};
        step_ = SwimStep{};
        step_.breath = breath_;
    }
    // dt: seconds (negative or not finite: 0; over kSwimMaxStep: clamped).
    const SwimStep& step(const SwimInput& in, const SwimEnv& env, float dt);

    SwimState state() const { return state_; }
    float breath() const { return breath_; }
    float vitality() const { return vitality_; }
    // The party's health takes vitality over once it exists: the host may set it (clamped 0..1; not finite: kept).
    void setVitality(float share) {
        if (std::isfinite(share)) vitality_ = swim_detail::clampf(share, 0.f, 1.f);
    }
    const SwimTuning& tuning() const { return tuning_; }
    bool tuningOk() const { return tuningOk_; }
    const SwimStep& last() const { return step_; }

private:
    SwimTuning tuning_{};
    bool tuningOk_ = true;
    SwimState state_ = SwimState::Surface;
    float breath_ = 45.f;
    float vitality_ = 1.f;
    SwimVec look_{1.f, 0.f, 0.f};  // the last good look, unit
    SwimVec right_{0.f, 1.f, 0.f}; // the last good horizontal right (up x look), unit
    SwimStep step_{};
};

inline const SwimStep& Swim::step(const SwimInput& in, const SwimEnv& env, float dt) {
    using swim_detail::clampf;
    using swim_detail::finite;
    const SwimTuning& t = tuning_;
    SwimStep s;
    s.from = state_;
    s.flags = SwimFlags{};
    if (!std::isfinite(dt) || dt < 0.f) dt = 0.f;
    if (dt > kSwimMaxStep) dt = kSwimMaxStep;
    s.dt = dt;

    // The env: nothing changes on a bad one (the last position stays).
    if (!finite(env.position) || !std::isfinite(env.surfaceZ) || !std::isfinite(env.bedZ) ||
        !std::isfinite(env.column) || env.bedZ >= env.surfaceZ) {
        s.envOk = false;
        s.state = state_;
        s.position = step_.position;
        s.breath = breath_;
        s.breathShare = breath_ / t.breathSeconds;
        s.vitality = vitality_;
        step_ = s;
        return step_;
    }

    // The input.
    if (finite(in.look)) {
        const float len = std::sqrt(in.look.x * in.look.x + in.look.y * in.look.y + in.look.z * in.look.z);
        if (len > 1e-4f) {
            look_ = {in.look.x / len, in.look.y / len, in.look.z / len};
            const float h = std::sqrt(look_.x * look_.x + look_.y * look_.y);
            if (h > 1e-4f) right_ = {-look_.y / h, look_.x / h, 0.f}; // straight up or down: the last right
        } else {
            s.inputOk = false;
        }
    } else {
        s.inputOk = false;
    }
    const auto axis = [&s](float v) {
        if (!std::isfinite(v)) {
            s.inputOk = false;
            return 0.f;
        }
        if (v < -1.f || v > 1.f) s.inputOk = false;
        return clampf(v, -1.f, 1.f);
    };
    float fwd = axis(in.forward);
    float rgt = axis(in.right);
    if (const float m = fwd * fwd + rgt * rgt; m > 1.f) {
        const float r = std::sqrt(m);
        fwd /= r;
        rgt /= r;
    }
    const bool moving = fwd != 0.f || rgt != 0.f;

    // The asked state changes.
    if (state_ == SwimState::Surface && in.crouch) {
        if (env.deepAllowed && env.column >= kSwimDeepMin) {
            state_ = SwimState::Diving;
            s.dived = true;
        } else {
            s.diveRefused = true;
        }
    } else if ((state_ == SwimState::Diving || state_ == SwimState::Under) && in.jump) {
        state_ = SwimState::Surfacing;
    }

    // The move.
    SwimVec v;
    switch (state_) {
    case SwimState::Surface:
        break;
    case SwimState::Diving:
        v.z = -t.swimSpeed;
        break;
    case SwimState::Surfacing:
        v.z = t.swimSpeed;
        break;
    case SwimState::Under:
        if (moving) {
            s.sprinting = in.sprint;
            const float sp = s.sprinting ? t.sprintSpeed : t.swimSpeed;
            v = {(look_.x * fwd + right_.x * rgt) * sp, (look_.y * fwd + right_.y * rgt) * sp,
                 (look_.z * fwd + right_.z * rgt) * sp};
        } else {
            v.z = t.buoyancy;
        }
        break;
    }
    if (state_ != SwimState::Surface) {
        float share = 0.f;
        if (!std::isfinite(env.edgeDistance)) share = 0.f;
        else if (env.edgeDistance <= 0.f) share = 1.f;
        else if (env.edgeDistance < t.edgeRampMetres) share = 1.f - env.edgeDistance / t.edgeRampMetres;
        const float h = finite(env.edgeInward)
                            ? std::sqrt(env.edgeInward.x * env.edgeInward.x + env.edgeInward.y * env.edgeInward.y)
                            : 0.f;
        if (share > 0.f && h > 1e-4f) {
            s.pushBack = share;
            v.x += env.edgeInward.x / h * share * t.edgePushSpeed;
            v.y += env.edgeInward.y / h * share * t.edgePushSpeed;
        }
    }
    s.position = {env.position.x + v.x * dt, env.position.y + v.y * dt, env.position.z + v.z * dt};
    const float floorZ = env.bedZ + t.bedClearance < env.surfaceZ ? env.bedZ + t.bedClearance : env.surfaceZ;
    s.position.z = clampf(s.position.z, floorZ, env.surfaceZ);
    s.velocity = dt > 0.f ? SwimVec{(s.position.x - env.position.x) / dt, (s.position.y - env.position.y) / dt,
                                    (s.position.z - env.position.z) / dt}
                          : v;

    // The state changes the position brings.
    constexpr float kEps = 1e-4f;
    if (state_ == SwimState::Diving) {
        const float target = env.surfaceZ - t.diveDepth > floorZ ? env.surfaceZ - t.diveDepth : floorZ;
        if (s.position.z <= target + kEps) state_ = SwimState::Under;
    }
    if ((state_ == SwimState::Under || state_ == SwimState::Surfacing) && s.position.z >= env.surfaceZ - kEps &&
        s.position.z <= env.surfaceZ + kEps) {
        state_ = SwimState::Surface;
        s.surfaced = true;
    }

    // Breath and vitality.
    s.refilling = state_ == SwimState::Surface || env.airPocket;
    if (s.refilling) {
        breath_ += t.breathSeconds / t.refillSeconds * dt;
        if (breath_ > t.breathSeconds) breath_ = t.breathSeconds;
    } else {
        const float rate = s.sprinting ? t.sprintBreathRate : 1.f;
        const float use = rate * dt;
        float drainTime = 0.f;
        if (use >= breath_) {
            drainTime = dt - breath_ / rate;
            breath_ = 0.f;
        } else {
            breath_ -= use;
        }
        s.warning = breath_ <= t.warningSeconds;
        s.draining = breath_ <= 0.f;
        if (drainTime > 0.f) vitality_ = clampf(vitality_ - t.drainPerSecond * drainTime, 0.f, 1.f);
    }
    if (state_ != SwimState::Surface && vitality_ <= 0.f) {
        s.rescue = true;
        s.position = env.position;
        s.velocity = SwimVec{};
        s.pushBack = 0.f;
        s.sprinting = false;
        state_ = SwimState::Surface;
        breath_ = t.breathSeconds;
        vitality_ = t.rescueVitality;
        s.warning = false;
        s.draining = false;
    }
    s.state = state_;
    s.breath = breath_;
    s.breathShare = breath_ / t.breathSeconds;
    s.vitality = vitality_;
    step_ = s;
    return step_;
}

// Reads Data/Swim/swim.json: {"version":1, and any SwimTuning field by name}; each field is optional (its default
// kept). On any error `out` is the defaults, false is returned and `error` lists every problem.
inline bool importSwimTuning(std::string_view json, SwimTuning& out, std::string* error = nullptr) {
    out = SwimTuning{};
    std::vector<std::string> errors;
    const auto finish = [&]() {
        if (errors.empty()) return true;
        out = SwimTuning{};
        if (error) {
            error->clear();
            for (const std::string& e : errors) {
                if (!error->empty()) *error += '\n';
                *error += e;
            }
        }
        return false;
    };
    JsonDocument doc;
    std::string err;
    if (!doc.parse(json, &err)) {
        errors.push_back("swim file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("swim file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !(v.isInteger() && v.integer() == 1)) errors.push_back("version: expected 1");
    SwimTuning t;
    for (const swim_detail::Range& r : swim_detail::kRanges) {
        const JsonValue v = root[r.name];
        if (v.isAbsent()) continue;
        const float f = v.isNumber() ? float(v.number()) : 0.f;
        if (!v.isNumber() || !swim_detail::inRange(f, r.lo, r.hi))
            errors.push_back(std::string(r.name) + ": expected a number in " + swim_detail::text(r.lo) + ".." +
                             swim_detail::text(r.hi));
        else
            t.*r.field = f;
    }
    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        bool known = k == "version";
        for (const swim_detail::Range& r : swim_detail::kRanges) known = known || k == r.name;
        if (!known) errors.push_back(std::string(k) + ": unknown field");
    }
    if (errors.empty()) {
        if (t.sprintSpeed < t.swimSpeed) errors.push_back("sprintSpeed: expected at least swimSpeed");
        if (t.warningSeconds > t.breathSeconds) errors.push_back("warningSeconds: expected at most breathSeconds");
        if (t.edgePushSpeed <= t.sprintSpeed) errors.push_back("edgePushSpeed: expected above sprintSpeed");
    }
    if (!errors.empty()) return finish();
    out = t;
    return true;
}

} // namespace ftcore
