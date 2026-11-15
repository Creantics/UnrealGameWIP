// Underwater free swim for an explore lead: dive, swim in 3D along the look, breath, the rescue and the outer edge.
// Units: metres and seconds, Z up (Unreal's axes: x forward, y right). The host converts from centimetres both ways.
// Plain C++20, no Unreal headers, no allocation in step(). Header-only (inline), so it needs no wrapper in the Unreal
// module. The host calls step() once a frame while the lead is in deep water, moves the body to the step's position
// and reads the flags. Data/Swim/swim.json is read by importSwimTuning.
//
// The rules:
// - Surface: the host's own surface swim moves the body (Swim passes the position through untouched: no clamp, no
//   edge push); breath refills. A crouch press dives (Diving) where the env allows it: deepAllowed and a column of
//   at least kSwimDeepMin; else diveRefused (once per press). canDive says, every step, whether a press would dive
//   (the same test), so the HUD asks it and never copies the rule.
// - Crouch and jump are edge-triggered: a state change needs a press (up last step, down this one), so a crouch held
//   through a surfacing does not dive again and a held refused crouch reports diveRefused once. Both down on the same
//   step at the surface: the jump wins (no dive; the jump is the host's).
// - Diving: straight down at swimSpeed to diveDepth under the surface (or the bed's clearance), clamped there (the
//   rest of that step is not carried on), then Under.
// - Under: along the look in full 3D, the move axes on it (forward on the look, right on its horizontal right) at
//   swimSpeed, sprintSpeed while sprinting with a move input; with no move input it drifts up at buoyancy. Reaching
//   the surface hands back to Surface (surfaced).
// - A jump press in Diving or Under: Surfacing, straight up at swimSpeed to the surface, then Surface (surfaced).
// - Diving, Under and Surfacing: held bedClearance off the bed and stopped at the surface. The swimmer's own move
//   is clamped there at once; a body the env puts outside the column (the surface or the bed jumped) is brought
//   back in at columnCatchUpSpeed (4 m/s) at most, or at once (snapped) when it is more than kSwimSnapOutside (2 m)
//   outside it.
// - Once under water the lead is never ejected: deepAllowed turning false, or the column shrinking under
//   kSwimDeepMin, changes nothing for Diving, Under or Surfacing; only a new dive needs them.
// - Under water (Diving, Under, Surfacing) the outer edge pushes back toward edgeInward: 0 from edgeRampMetres
//   inside it, rising to edgePushSpeed at the edge and past it (edgeDistance 0 or less, -inf included), so a sprint
//   cannot cross it. No wall, no message. An edgeInward that is zero (or not finite) where the edge pushes: the
//   last good inward, else straight toward the dive area's centre when the host gives one; with neither, the body
//   holds its x and y (envOk false). Never no push. Any finite edgeInward, however long, is scaled by its largest
//   part before it is normalised (as the look is), so it never overflows to a zero direction.
// - Breath: used 1 s a second under water, sprintBreathRate while sprinting; refilled at the surface or in an air
//   pocket (empty to full in refillSeconds). warning from warningSeconds left while it is being used; at 0 the
//   vitality drains drainPerSecond (for the part of the step past empty).
// - Vitality 0 under water: rescue (raised on that step) and the state is Rescued: the body is left where the env
//   puts it, no breath refill, no dive, no move from input, until the host has carried the lead to the nearest exit
//   and calls reset() there (Surface, full breath, vitality rescueVitality).
// - The step: dt over kSwimMaxStep is clamped to it; the time past it is dropped (no move, breath or drain for it),
//   so one huge step moves at most a quarter second's worth and the clamps still hold. So the results of steps
//   longer than kSwimMaxStep depend on the frame rate (the dropped time is lost).
// - Guards: a bad env (position, surface or bed not finite, the bed not under the surface) moves nothing and changes
//   no state, but time passes: breath is used (or refills) as on a good step, except that its airPocket is not
//   trusted (under water it does not refill on a bad step), and under water kSwimBadEnvRescue
//   (2 s) of it without a break is a rescue. The position is the env's if finite, else the last valid one since the
//   last reset; with none, the env's own, untouched, and positionValid false (the host must not move the body). A
//   crouch or jump pressed on a bad step is kept until a good step uses it. A column not finite, negative, or off
//   surface minus bed by
//   more than 0.05 m, and an edgeDistance that is NaN or +inf or an edgeInward not finite, set envOk false: the
//   column is taken as surface minus bed, the edge gives no push, and the step goes on. A bad input is replaced.
#pragma once

#include "ftcore/Json.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ftcore {

enum class SwimState : std::uint8_t { Surface, Diving, Under, Surfacing, Rescued };

inline constexpr float kSwimDeepMin = 3.0f;       // a dive needs a water column at least this deep (m)
inline constexpr float kSwimMaxStep = 0.25f;      // a longer step is clamped to this (s); the rest is dropped
inline constexpr float kSwimColumnSlack = 0.05f;  // the column may differ from surface minus bed by this (m)
inline constexpr float kSwimSnapOutside = 2.0f;   // a body further outside the column than this is put in at once (m)
inline constexpr float kSwimBadEnvRescue = 2.0f;  // under water this long on a bad env without a break: a rescue (s)

struct SwimVec {
    float x = 0.f, y = 0.f, z = 0.f;
};

// The tunables (Data/Swim/swim.json); the ranges are validSwimTuning's.
struct SwimTuning {
    float swimSpeed = 2.2f;       // m/s (0.5..6)
    float sprintSpeed = 3.2f;     // m/s (swimSpeed..8)
    float buoyancy = 0.2f;        // m/s up under water with no move input (0..2)
    float bedClearance = 0.3f;    // m held off the bed (0..2)
    float diveDepth = 1.0f;       // m: a dive ends this far under the surface (0.2..3)
    float breathSeconds = 45.f;   // s: a full breath (5..300)
    float sprintBreathRate = 2.f; // breath seconds used per second while sprinting (1..5)
    float warningSeconds = 10.f;  // s: the warning from this much breath left (0..breathSeconds)
    float drainPerSecond = 0.05f; // vitality share lost per second with no breath (0.01..1)
    float refillSeconds = 2.f;    // s: empty to full at the surface or in an air pocket (0.1..30)
    float rescueVitality = 0.25f; // the vitality share a rescued lead is left at (0.01..1)
    float edgeRampMetres = 8.f;   // m: the edge's push rises over this distance (0.5..50)
    float edgePushSpeed = 4.f;    // m/s inward at the edge (above sprintSpeed..12)
    float columnCatchUpSpeed = 4.f; // m/s: a body outside the column comes back in at most this fast (0.5..50)
};

namespace swim_detail {

struct Range {
    std::string_view name;
    float SwimTuning::*field;
    float lo, hi;
};

// The fixed ranges; the cross-field pairs (kPairs) are in validSwimTuning too.
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
    {"columnCatchUpSpeed", &SwimTuning::columnCatchUpSpeed, 0.5f, 50.f},
};

// The cross-field pairs: a broken one puts both its fields back to their defaults (the importer).
struct Pair {
    float SwimTuning::*a;
    float SwimTuning::*b;
    std::string_view names; // "a, b"
    std::string_view rule;
    bool (*ok)(const SwimTuning&);
};
inline constexpr Pair kPairs[] = {
    {&SwimTuning::swimSpeed, &SwimTuning::sprintSpeed, "swimSpeed, sprintSpeed", "sprintSpeed at least swimSpeed",
     [](const SwimTuning& t) { return t.sprintSpeed >= t.swimSpeed; }},
    {&SwimTuning::breathSeconds, &SwimTuning::warningSeconds, "breathSeconds, warningSeconds",
     "warningSeconds at most breathSeconds", [](const SwimTuning& t) { return t.warningSeconds <= t.breathSeconds; }},
    {&SwimTuning::sprintSpeed, &SwimTuning::edgePushSpeed, "sprintSpeed, edgePushSpeed",
     "edgePushSpeed above sprintSpeed", [](const SwimTuning& t) { return t.edgePushSpeed > t.sprintSpeed; }},
};

inline bool inRange(float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; }
inline bool finite(const SwimVec& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float absf(float v) { return v < 0.f ? -v : v; }
inline std::string text(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", double(v));
    return buf;
}

} // namespace swim_detail

inline bool validSwimTuning(const SwimTuning& t) {
    for (const swim_detail::Range& r : swim_detail::kRanges)
        if (!swim_detail::inRange(t.*r.field, r.lo, r.hi)) return false;
    for (const swim_detail::Pair& p : swim_detail::kPairs)
        if (!p.ok(t)) return false;
    return true;
}

// What the player asks this frame. A bad value is replaced (SwimStep::inputOk false), never trusted.
struct SwimInput {
    SwimVec look{1.f, 0.f, 0.f}; // the camera's look (normalised here, any finite length; zero or not finite: the
                                 // last good one)
    float forward = 0.f;         // move axes, -1..1 (outside: clamped; not finite: 0); longer than 1 together: scaled
    float right = 0.f;
    bool sprint = false;
    bool crouch = false; // held; its press at the surface dives
    bool jump = false;   // held; its press diving or under rises to the surface
};

// The water at the lead this frame (the host's WaterBody rows and the character), in metres, Z up.
struct SwimEnv {
    SwimVec position;            // the lead's body reference point
    float surfaceZ = 0.f;        // the water surface over the lead
    float bedZ = -10.f;          // the bed under it (below surfaceZ)
    float column = 10.f;         // the water column's depth here: surfaceZ - bedZ (a dive needs kSwimDeepMin)
    bool deepAllowed = false;    // the body is swimDeep and the point is inside its dive area
    bool airPocket = false;      // the lead's head is in an air pocket (breath refills under water)
    float edgeDistance = 1.0e9f; // metres inside the outer edge (0 or less, -inf included: at or past it)
    SwimVec edgeInward;          // horizontal, back into the basin (zero: the last good one, else toward centre)
    bool hasCentre = false;      // the host knows the dive area's centre
    SwimVec centre;              // with hasCentre: the dive area's centre (its x and y are used)
};

// What the lead may do while swimming, in every state: the character reads them, the HUD asks them. At the surface
// too (the host's own surface swim moves the body there): attack false, no attack starts; guard false, no guard or
// block; dodge false, no dodge roll; weaponsOut false, weapons stay stowed (none drawn); interact true, the interact
// prompt works. Under water (Diving, Under, Surfacing) and Rescued: the same.
struct SwimFlags {
    bool attack = false;     // no attacks
    bool guard = false;      // no guard
    bool dodge = false;      // no dodge roll
    bool weaponsOut = false; // weapons stowed
    bool interact = true;    // interact allowed
};

struct SwimStep {
    SwimState state = SwimState::Surface; // after this step
    SwimState from = SwimState::Surface;  // before it
    bool dived = false;       // Surface to Diving this step
    bool diveRefused = false; // a crouch press at the surface where no dive is allowed
    // A crouch press on this step would start a dive (the dive's own test, diveOpen): Surface, no jump down, a good
    // env, deepAllowed and a column of at least kSwimDeepMin. Never in Rescued. On the step a press dives, true.
    bool canDive = false;
    bool surfaced = false;    // to Surface this step (not by a rescue)
    SwimVec position;         // where the body goes (under water: clamped to the bed's clearance and the surface)
    bool positionValid = true; // false: no valid position yet since the reset (position is the env's own): do not move
    bool snapped = false;     // the body was more than kSwimSnapOutside outside the column: put back in at once
    SwimVec velocity;         // m/s: the move this step (the asked one when dt is 0)
    float dt = 0.f;           // the step used (0..kSwimMaxStep)
    bool sprinting = false;   // under, sprint held with a move input
    bool refilling = false;   // at the surface or in an air pocket
    float breath = 0.f;       // seconds left
    float breathShare = 1.f;  // breath / breathSeconds
    bool warning = false;     // breath in use with warningSeconds or less left
    bool draining = false;    // no breath left under water: vitality drains
    float vitality = 1.f;     // the share left (0..1)
    bool rescue = false;      // vitality reached 0 under water this step: carry the lead to the nearest exit, then reset
    float pushBack = 0.f;     // the outer edge's share applied this step (0..1)
    SwimFlags flags;
    bool inputOk = true;      // false: an input value was replaced (not finite, out of range)
    bool envOk = true;        // false: an env value was unusable (see the guards at the top)
};

class Swim {
public:
    // A tuning validSwimTuning refuses is replaced by the defaults (tuningOk() false).
    explicit Swim(const SwimTuning& t = SwimTuning{}) { reset(t); }
    // Surface, a full breath; vitality full, or rescueVitality when it ends a rescue (state Rescued). The body's
    // position is the env's on the next step. Held buttons are kept (a crouch held through it is not a press).
    void reset(const SwimTuning& t) {
        tuningOk_ = validSwimTuning(t);
        tuning_ = tuningOk_ ? t : SwimTuning{};
        reset();
    }
    void reset() {
        vitality_ = state_ == SwimState::Rescued ? tuning_.rescueVitality : 1.f;
        state_ = SwimState::Surface;
        breath_ = tuning_.breathSeconds;
        look_ = {1.f, 0.f, 0.f};
        right_ = {0.f, 1.f, 0.f};
        hasValid_ = false;
        badFor_ = 0.f;
        pendCrouch_ = pendJump_ = false;
        hasInward_ = false;
        step_ = SwimStep{};
        step_.breath = breath_;
        step_.vitality = vitality_;
    }
    // dt: seconds (negative or not finite: 0; over kSwimMaxStep: clamped, the rest dropped).
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
    // Whether a crouch press dives: the one test for the dive and for SwimStep::canDive.
    static bool diveOpen(SwimState st, bool jumpDown, const SwimEnv& env, float column) {
        return st == SwimState::Surface && !jumpDown && env.deepAllowed && column >= kSwimDeepMin;
    }
    // A finite v's horizontal part as a unit vector (z 0), scaled by its largest part first so a huge v cannot
    // overflow to infinity (and a zero direction). False when that part is under 1e-4 m long.
    static bool unitXY(SwimVec v, SwimVec& out) {
        const float m = std::fmax(absf(v.x), absf(v.y));
        if (!(m > 0.f)) return false;
        const float qx = v.x / m, qy = v.y / m;
        const float len = std::sqrt(qx * qx + qy * qy); // 1..sqrt(2)
        if (!(m * len > 1e-4f)) return false;          // m * len may round to +inf: still long enough
        out = {qx / len, qy / len, 0.f};
        return true;
    }
    SwimTuning tuning_{};
    bool tuningOk_ = true;
    SwimState state_ = SwimState::Surface;
    float breath_ = 45.f;
    float vitality_ = 1.f;
    SwimVec look_{1.f, 0.f, 0.f};  // the last good look, unit
    SwimVec right_{0.f, 1.f, 0.f}; // the last good horizontal right (up x look), unit
    bool crouchHeld_ = false;      // the buttons last step, for the presses
    bool jumpHeld_ = false;
    bool pendCrouch_ = false;      // a press on a bad-env step, kept for the next good one
    bool pendJump_ = false;
    SwimVec lastValid_{};          // the last valid position put out since the reset
    bool hasValid_ = false;
    float badFor_ = 0.f;           // seconds of bad env without a break, under water
    SwimVec lastInward_{};         // the last good edge inward (horizontal, unit)
    bool hasInward_ = false;
    SwimStep step_{};
};

inline const SwimStep& Swim::step(const SwimInput& in, const SwimEnv& env, float dt) {
    using swim_detail::absf;
    using swim_detail::clampf;
    using swim_detail::finite;
    const SwimTuning& t = tuning_;
    SwimStep s;
    s.from = state_;
    s.flags = SwimFlags{};
    if (!std::isfinite(dt) || dt < 0.f) dt = 0.f;
    if (dt > kSwimMaxStep) dt = kSwimMaxStep;
    s.dt = dt;
    bool crouchPress = in.crouch && !crouchHeld_;
    bool jumpPress = in.jump && !jumpHeld_;
    crouchHeld_ = in.crouch;
    jumpHeld_ = in.jump;
    const auto finish = [&]() -> const SwimStep& {
        s.state = state_;
        s.breath = breath_;
        s.breathShare = breath_ / t.breathSeconds;
        s.vitality = vitality_;
        if (s.positionValid && finite(s.position)) {
            lastValid_ = s.position;
            hasValid_ = true;
        }
        step_ = s;
        return step_;
    };
    // Breath and vitality for this step's time (refilling, or used at rate).
    const auto breathe = [&](bool refill) {
        s.refilling = refill;
        if (refill) {
            breath_ += t.breathSeconds / t.refillSeconds * dt;
            if (breath_ > t.breathSeconds) breath_ = t.breathSeconds;
            return;
        }
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
    };
    const auto rescue = [&]() {
        s.rescue = true;
        s.velocity = SwimVec{};
        s.pushBack = 0.f;
        s.sprinting = false;
        s.warning = false;
        s.draining = false;
        state_ = SwimState::Rescued;
    };

    // A bad env: nothing moves and no state changes, but time passes (breath, its air pocket not trusted; under
    // water, the rescue after kSwimBadEnvRescue). The position: the env's if finite, else the last valid, else the
    // env's own (not valid).
    if (!finite(env.position) || !std::isfinite(env.surfaceZ) || !std::isfinite(env.bedZ) ||
        !(env.bedZ < env.surfaceZ)) {
        s.envOk = false;
        if (finite(env.position)) {
            s.position = env.position;
        } else if (hasValid_) {
            s.position = lastValid_;
        } else {
            s.position = env.position;
            s.positionValid = false;
        }
        pendCrouch_ = pendCrouch_ || crouchPress;
        pendJump_ = pendJump_ || jumpPress;
        if (state_ == SwimState::Rescued) return finish();
        const bool under = state_ != SwimState::Surface;
        breathe(!under);
        if (under) {
            badFor_ += dt;
            if (badFor_ + 1e-6f >= kSwimBadEnvRescue || vitality_ <= 0.f) rescue();
        }
        return finish();
    }
    badFor_ = 0.f;
    crouchPress = crouchPress || pendCrouch_;
    jumpPress = jumpPress || pendJump_;
    pendCrouch_ = pendJump_ = false;
    const float span = env.surfaceZ - env.bedZ;
    float column = env.column;
    if (!std::isfinite(column) || column < 0.f || absf(column - span) > kSwimColumnSlack) {
        s.envOk = false;
        column = span;
    }
    const bool edgeUsable =
        !std::isnan(env.edgeDistance) && env.edgeDistance != std::numeric_limits<float>::infinity();
    const bool inwardOk = finite(env.edgeInward);
    if (!edgeUsable || !inwardOk) s.envOk = false;
    SwimVec inward;
    const bool inwardHas = inwardOk && unitXY(env.edgeInward, inward);
    if (inwardHas) {
        lastInward_ = inward;
        hasInward_ = true;
    }

    // Rescued: left where the env puts it until the host's reset.
    if (state_ == SwimState::Rescued) {
        s.position = env.position;
        return finish();
    }

    // The input.
    if (finite(in.look)) {
        // Scaled by its largest part first, so a huge (or tiny) finite look normalises without overflow.
        const float m = std::fmax(absf(in.look.x), std::fmax(absf(in.look.y), absf(in.look.z)));
        if (m > 0.f) {
            const SwimVec q{in.look.x / m, in.look.y / m, in.look.z / m};
            const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z); // 1..sqrt(3)
            look_ = {q.x / len, q.y / len, q.z / len};
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

    // The asked state changes: presses only; at the surface a jump down wins over a crouch press.
    const bool jumpDown = in.jump || jumpPress;
    s.canDive = diveOpen(state_, jumpDown, env, column);
    if (state_ == SwimState::Surface && crouchPress && !jumpDown) {
        if (s.canDive) {
            state_ = SwimState::Diving;
            s.dived = true;
        } else {
            s.diveRefused = true;
        }
    } else if ((state_ == SwimState::Diving || state_ == SwimState::Under) && jumpPress) {
        state_ = SwimState::Surfacing;
    }

    // The move.
    SwimVec v;
    switch (state_) {
    case SwimState::Surface:
    case SwimState::Rescued:
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
    if (state_ == SwimState::Surface) {
        s.position = env.position; // the host's surface swim: passed through untouched
        s.velocity = SwimVec{};
    } else {
        bool holdXY = false;
        if (edgeUsable) {
            float share = 0.f;
            if (env.edgeDistance <= 0.f) share = 1.f;
            else if (env.edgeDistance < t.edgeRampMetres) share = 1.f - env.edgeDistance / t.edgeRampMetres;
            if (share > 0.f) {
                // This step's inward, else the last good one, else toward the dive area's centre, else hold x, y.
                SwimVec dir;
                bool have = false;
                if (inwardHas) {
                    dir = inward;
                    have = true;
                } else if (hasInward_) {
                    dir = lastInward_;
                    have = true;
                } else if (env.hasCentre && std::isfinite(env.centre.x) && std::isfinite(env.centre.y)) {
                    const float cx = env.centre.x - env.position.x, cy = env.centre.y - env.position.y;
                    const float c = std::sqrt(cx * cx + cy * cy);
                    if (std::isfinite(c) && c > 1e-4f) {
                        dir = {cx / c, cy / c, 0.f};
                        have = true;
                    }
                }
                if (have) {
                    s.pushBack = share;
                    v.x += dir.x * share * t.edgePushSpeed;
                    v.y += dir.y * share * t.edgePushSpeed;
                } else {
                    holdXY = true;
                    s.envOk = false;
                }
            }
        }
        const float floorZ = env.bedZ + t.bedClearance < env.surfaceZ ? env.bedZ + t.bedClearance : env.surfaceZ;
        s.position = {env.position.x + v.x * dt, env.position.y + v.y * dt, env.position.z + v.z * dt};
        if (holdXY) {
            s.position.x = env.position.x;
            s.position.y = env.position.y;
        }
        // The column: the swimmer's own move is clamped at once; a body the env put outside it comes back at
        // columnCatchUpSpeed at most (its own move may take it in faster), or at once past kSwimSnapOutside.
        const float z0 = env.position.z, zMove = s.position.z;
        if (z0 > env.surfaceZ || z0 < floorZ) {
            const bool over = z0 > env.surfaceZ;
            const float out = over ? z0 - env.surfaceZ : floorZ - z0;
            if (out > kSwimSnapOutside) {
                s.position.z = clampf(zMove, floorZ, env.surfaceZ);
                s.snapped = true;
            } else if (over) {
                const float cap = std::fmax(env.surfaceZ, z0 - t.columnCatchUpSpeed * dt);
                s.position.z = zMove < cap ? std::fmax(zMove, floorZ) : cap;
            } else {
                const float cap = std::fmin(floorZ, z0 + t.columnCatchUpSpeed * dt);
                s.position.z = zMove > cap ? std::fmin(zMove, env.surfaceZ) : cap;
            }
        } else {
            s.position.z = clampf(zMove, floorZ, env.surfaceZ);
        }
        if (state_ == SwimState::Diving) { // clamped at its target, the rest of the step not carried on
            const float target = env.surfaceZ - t.diveDepth > floorZ ? env.surfaceZ - t.diveDepth : floorZ;
            if (s.position.z <= target + 1e-4f) {
                if (env.position.z >= target) s.position.z = target; // from above it: stop there, no overshoot
                state_ = SwimState::Under;
            }
        }
        s.velocity = dt > 0.f ? SwimVec{(s.position.x - env.position.x) / dt, (s.position.y - env.position.y) / dt,
                                        (s.position.z - env.position.z) / dt}
                              : v;
        if ((state_ == SwimState::Under || state_ == SwimState::Surfacing) &&
            s.position.z >= env.surfaceZ - 1e-4f) {
            state_ = SwimState::Surface;
            s.surfaced = true;
        }
    }

    // Breath and vitality.
    breathe(state_ == SwimState::Surface || env.airPocket);
    if (state_ != SwimState::Surface && vitality_ <= 0.f) {
        s.position = env.position;
        s.snapped = false;
        rescue();
    }
    return finish();
}

// Reads Data/Swim/swim.json: {"version":1, "_comment": "...", and any SwimTuning field by name}, metres and
// seconds. A field left out takes its default; a value that is not a number or is out of its range takes its
// default; an unknown key is skipped; each is reported in `notes` and the import still succeeds. A cross-field
// pair broken (sprintSpeed under swimSpeed, warningSeconds over breathSeconds, edgePushSpeed not above
// sprintSpeed) puts only that pair's two fields back to their defaults (noted), until none is broken; every other
// field is kept. Only a file that does not parse, is not an object or is not version 1 fails: `out` is the
// defaults, false is returned and `error` says why.
inline bool importSwimTuning(std::string_view json, SwimTuning& out, std::string* error = nullptr,
                             std::vector<std::string>* notes = nullptr) {
    out = SwimTuning{};
    if (error) error->clear();
    if (notes) notes->clear();
    const auto fail = [&](std::string why) {
        out = SwimTuning{};
        if (error) *error = std::move(why);
        return false;
    };
    const auto note = [&](std::string n) {
        if (notes) notes->push_back(std::move(n));
    };
    JsonDocument doc;
    std::string err;
    if (!doc.parse(json, &err)) return fail("swim file: " + err);
    const JsonValue root = doc.root();
    if (!root.isObject()) return fail("swim file: expected an object");
    if (const JsonValue v = root["version"]; !(v.isInteger() && v.integer() == 1)) return fail("version: expected 1");
    const SwimTuning d;
    SwimTuning t;
    for (const swim_detail::Range& r : swim_detail::kRanges) {
        const JsonValue v = root[r.name];
        const std::string fallback = ", the default " + swim_detail::text(d.*r.field) + " used";
        if (v.isAbsent()) {
            note(std::string(r.name) + ": missing" + fallback);
            continue;
        }
        const float f = v.isNumber() ? float(v.number()) : 0.f;
        if (!v.isNumber() || !swim_detail::inRange(f, r.lo, r.hi))
            note(std::string(r.name) + ": expected a number in " + swim_detail::text(r.lo) + ".." +
                 swim_detail::text(r.hi) + fallback);
        else
            t.*r.field = f;
    }
    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        bool known = k == "version" || k == "_comment";
        for (const swim_detail::Range& r : swim_detail::kRanges) known = known || k == r.name;
        if (!known) note(std::string(k) + ": unknown field, skipped");
    }
    // The pairs, to a fixpoint (a field in two pairs may break the other when reset). A broken pair always has a field
    // off its default and the defaults agree, so it ends within one pass per pair; the bound is a guard.
    bool again = true;
    for (std::size_t pass = 0; again && pass <= std::size(swim_detail::kPairs); ++pass) {
        again = false;
        for (const swim_detail::Pair& p : swim_detail::kPairs) {
            if (p.ok(t)) continue;
            note(std::string(p.names) + ": expected " + std::string(p.rule) + ", both defaults used");
            t.*p.a = d.*p.a;
            t.*p.b = d.*p.b;
            again = true;
        }
    }
    out = t;
    return true;
}

} // namespace ftcore
