#pragma once

// Water locomotion: the animation state and the surface movement of a body in the water (a lead or a party follower).
// It sits over what already exists and duplicates no physics: waterState() (WaterBodies.h) says Dry / Wade / DeepWade /
// Swim, and ftcore::Swim keeps moving the body under water and keeps breath and rescue. WaterLoco owns the surface
// movement (tread, the surface swim, the splash sink and recover) and, for every water case, which clips play at which
// phase and weight, the blend time of each state change, the stroke cues and the hand IK target of an exit.
//
//   ftcore::WaterLoco loco;              // tuning from Data/Swim/water_loco.json (importWaterLocoTuning)
//   const ftcore::WaterLocoPose& p = loco.step(in, dt);
//   // surface states: p.velocity is the planar velocity to move at, p.yaw the facing, p.sink the depth offset
//   // (negative: under the draft) to add to the float height; the anim layer plays p.layers with p.blendSeconds.
//
// Metres, seconds, degrees, Z up, in the engine's frame: +Y is to the right of +X seen from above, and yaw is degrees
// about Z, 0 along +X, 90 along +Y (a rising yaw turns right). step() never allocates.
// Breath and rescue stay Swim's: WaterLoco only reads breathShare for the breach gasp.

#include "ftcore/Json.h"
#include "ftcore/Swim.h"
#include "ftcore/WaterBodies.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ftcore {

enum class WaterLocoState : std::uint8_t {
    Dry,         // out of the water: locomotion plays
    Wade,        // Wade or DeepWade: locomotion's walk plus wadeArms
    EnterWalk,   // walked off a slope into swimming depth
    EnterSplash, // fell in: the sink and the rise
    Recover,     // the head broke the surface after a splash: a gasp, no input yet
    Tread,       // at the surface, no forward intent: strafes
    TreadTurn,   // treading, turning toward a strong input more than treadTurnAngle off the facing
    SurfaceSwim, // the surface stroke
    Dive,        // Swim dived: DiveStart
    Under,       // Swim's Under or Surfacing
    Breach,      // Swim surfaced: Breach, then Tread
    Exit,        // the host's placed climb out (mantle, ladder, ladder top)
    Rescued,     // Swim's Rescued: limp in the host's carry
};
inline constexpr int kWaterLocoStateCount = 13;

// The clip set (production/animation/swim_1117): every clip in place, root at the capsule centre.
enum class WaterClip : std::uint8_t {
    None,
    TreadIdle, TreadFwd, TreadBack, TreadL, TreadR,
    TreadTurnL90, TreadTurnR90,
    SwimSlow, SwimFast,
    DiveStart,
    UnderIdle, UnderSlow, UnderFast,
    PitchUp, PitchDown,
    Breach,
    EnterWalk, EnterSplash, EnterDive,
    ExitMantle, ExitLadder, ExitLadderTop,
};
inline constexpr int kWaterClipCount = 23; // None included

// Each clip's length in seconds (the polish pass r2's 30 fps frame counts). The anim layer samples a clip at phase x
// length.
inline constexpr float kWaterClipSeconds[kWaterClipCount] = {
    0.f,                                     // None
    3.f, 3.f, 3.f, 3.f, 3.f,                 // TreadIdle, Fwd, Back, L, R (90)
    1.2f, 1.2f,                              // TreadTurnL90, R90 (36)
    1.6f, 3.2f,                              // SwimSlow (48: stroke and glide), SwimFast (96: six strokes, a breath each three)
    0.9f,                                    // DiveStart (27)
    2.4f, 2.4f, 0.8f,                        // UnderIdle (72), UnderSlow (72: pull and glide), UnderFast (24)
    20.f / 30.f, 20.f / 30.f,                // PitchUp, PitchDown (20)
    0.8f,                                    // Breach (24)
    1.f, 85.f / 30.f, 1.8f,                  // EnterWalk (30), EnterSplash (85), EnterDive (54)
    2.4f, 1.f, 80.f / 30.f,                  // ExitMantle (72), ExitLadder (30), ExitLadderTop (80)
};
inline constexpr float waterClipSeconds(WaterClip c) { return kWaterClipSeconds[int(c)]; }

// Each clip's name: its asset is A_<name> (the host's per-body Swim folder) and the blocking's clip of that name.
inline constexpr const char* kWaterClipNames[kWaterClipCount] = {
    "None",
    "TreadIdle", "TreadFwd", "TreadBack", "TreadL", "TreadR",
    "TreadTurnL90", "TreadTurnR90",
    "SwimSlow", "SwimFast",
    "DiveStart",
    "UnderIdle", "UnderSlow", "UnderFast",
    "PitchUp", "PitchDown",
    "Breach",
    "EnterWalk", "EnterSplash", "EnterDive",
    "ExitMantle", "ExitLadder", "ExitLadderTop",
};
inline constexpr const char* waterClipName(WaterClip c) {
    return int(c) >= 0 && int(c) < kWaterClipCount ? kWaterClipNames[int(c)] : "?";
}

inline constexpr const char* kWaterLocoStateNames[kWaterLocoStateCount] = {
    "Dry", "Wade", "EnterWalk", "EnterSplash", "Recover", "Tread", "TreadTurn", "SurfaceSwim",
    "Dive", "Under", "Breach", "Exit", "Rescued",
};
inline constexpr const char* waterLocoStateName(WaterLocoState s) {
    return int(s) >= 0 && int(s) < kWaterLocoStateCount ? kWaterLocoStateNames[int(s)] : "?";
}

// The host's placed climb out, if one is running (it moves the body; WaterLoco only animates it).
enum class WaterExitMove : std::uint8_t { None, Mantle, Ladder, LadderTop };

// Cue bits for one step: the water subsystem plays splashes, ripples and the gasp from them.
enum WaterCue : std::uint32_t {
    kWaterCueStrokeL = 1u << 0, // a left hand catches the water (a stroke's catch point)
    kWaterCueStrokeR = 1u << 1, // a right hand catches
    kWaterCueKick = 1u << 2,    // a kick (tread and the under-water loops)
    kWaterCueSplash = 1u << 3,  // an entry (strength: the fall's share) or a breach
    kWaterCueGasp = 1u << 4,    // the head came up short of breath (recover, a breach on low breath)
    kWaterCueWaterIn = 1u << 5, // the body went from dry into any water
    kWaterCueWaterOut = 1u << 6, // the body left the water to dry ground
};

// The tunables (Data/Swim/water_loco.json); the ranges are validWaterLocoTuning's.
struct WaterLocoTuning {
    float headClear = 0.12f;       // m: the head held this far over the surface (0..0.5)
    float treadSpeed = 0.6f;       // m/s strafing at the tread (0.1..2)
    float treadAccel = 1.5f;       // m/s^2 (0.1..20)
    float treadDrag = 1.2f;        // m/s^2 with no input (0.1..20)
    float treadTurnRate = 120.f;   // deg/s (10..720)
    float treadTurnAngle = 60.f;   // deg: a strong input further off the facing than this turns the treader (10..170)
    float swimSpeed = 1.5f;        // m/s on the surface (0.3..5)
    float swimSprintSpeed = 2.4f;  // m/s sprinting (swimSpeed..8)
    float swimAccel = 2.f;         // m/s^2 (0.1..20)
    float swimDrag = 1.2f;         // m/s^2 gliding out (0.1..20)
    float swimTurnRate = 150.f;    // deg/s toward the input (10..720)
    float swimStopSpeed = 0.4f;    // m/s: under this with no input the swim settles to a tread (0.05..swimSpeed)
    float intentMin = 0.5f;        // input length that counts as an intent to swim (0.05..1)
    float splashMinSpeed = 3.f;    // m/s down: a slower entry is not a splash (0.5..20)
    float splashSpeedSpan = 6.f;   // m/s over splashMinSpeed to the full splash (0.5..30)
    float sinkMin = 0.3f;          // m sunk by the smallest splash (0..3)
    float sinkSpan = 1.2f;         // m more at the full splash (0..5)
    float sinkOmega = 4.f;         // 1/s: the critically damped sink and rise (0.5..20)
    float bedClearance = 0.3f;     // m: a sink never brings the feet nearer the bed than this (0..2)
    float recoverMin = 0.3f;       // s: the recover beat after the smallest splash (0..3)
    float recoverSpan = 0.2f;      // s more after the full splash (0..3)
    float outHold = 0.2f;          // s out of swimming depth before the surface states give way (0..2)
    float leanPerTurnSpeed = 0.06f; // deg of roll per (deg/s x m/s) (0..1)
    float leanMax = 20.f;          // deg (0..60)
    float pitchMax = 70.f;         // deg under water (0..89)
    float pitchRate = 180.f;       // deg/s the pitch follows at (10..1440)
    float strokeSwimSlow = 2.4f;   // m travelled per SwimSlow cycle (0.2..10): swimSpeed over its 1.6 s
    float strokeSwimFast = 7.7f;   // m per SwimFast cycle (0.2..10): swimSprintSpeed over its 3.2 s (six strokes)
    float strokeUnderSlow = 4.f;   // m per UnderSlow cycle (0.2..10): a pull and its glide
    float strokeUnderFast = 2.8f;  // m per UnderFast cycle (0.2..10)
    float underSlowSpeed = 2.2f;   // m/s at which UnderSlow plays alone (Swim's swimSpeed) (0.3..8)
    float underFastSpeed = 3.2f;   // m/s at which UnderFast plays alone (Swim's sprintSpeed) (underSlowSpeed..10)
    float ladderCycle = 0.6f;      // m climbed per ExitLadder cycle (0.1..3)
    float gaspBreath = 0.3f;       // breath share under which a breach gasps (0..1)
    float diveEntrySpeed = 4.5f;   // m/s level at a fall's entry from which it goes in head first: EnterDive (0..30)
};

namespace water_loco_detail {

struct Range {
    std::string_view name;
    float WaterLocoTuning::*field;
    float lo, hi;
};

inline constexpr Range kRanges[] = {
    {"headClear", &WaterLocoTuning::headClear, 0.f, 0.5f},
    {"treadSpeed", &WaterLocoTuning::treadSpeed, 0.1f, 2.f},
    {"treadAccel", &WaterLocoTuning::treadAccel, 0.1f, 20.f},
    {"treadDrag", &WaterLocoTuning::treadDrag, 0.1f, 20.f},
    {"treadTurnRate", &WaterLocoTuning::treadTurnRate, 10.f, 720.f},
    {"treadTurnAngle", &WaterLocoTuning::treadTurnAngle, 10.f, 170.f},
    {"swimSpeed", &WaterLocoTuning::swimSpeed, 0.3f, 5.f},
    {"swimSprintSpeed", &WaterLocoTuning::swimSprintSpeed, 0.3f, 8.f},
    {"swimAccel", &WaterLocoTuning::swimAccel, 0.1f, 20.f},
    {"swimDrag", &WaterLocoTuning::swimDrag, 0.1f, 20.f},
    {"swimTurnRate", &WaterLocoTuning::swimTurnRate, 10.f, 720.f},
    {"swimStopSpeed", &WaterLocoTuning::swimStopSpeed, 0.05f, 5.f},
    {"intentMin", &WaterLocoTuning::intentMin, 0.05f, 1.f},
    {"splashMinSpeed", &WaterLocoTuning::splashMinSpeed, 0.5f, 20.f},
    {"splashSpeedSpan", &WaterLocoTuning::splashSpeedSpan, 0.5f, 30.f},
    {"sinkMin", &WaterLocoTuning::sinkMin, 0.f, 3.f},
    {"sinkSpan", &WaterLocoTuning::sinkSpan, 0.f, 5.f},
    {"sinkOmega", &WaterLocoTuning::sinkOmega, 0.5f, 20.f},
    {"bedClearance", &WaterLocoTuning::bedClearance, 0.f, 2.f},
    {"recoverMin", &WaterLocoTuning::recoverMin, 0.f, 3.f},
    {"recoverSpan", &WaterLocoTuning::recoverSpan, 0.f, 3.f},
    {"outHold", &WaterLocoTuning::outHold, 0.f, 2.f},
    {"leanPerTurnSpeed", &WaterLocoTuning::leanPerTurnSpeed, 0.f, 1.f},
    {"leanMax", &WaterLocoTuning::leanMax, 0.f, 60.f},
    {"pitchMax", &WaterLocoTuning::pitchMax, 0.f, 89.f},
    {"pitchRate", &WaterLocoTuning::pitchRate, 10.f, 1440.f},
    {"strokeSwimSlow", &WaterLocoTuning::strokeSwimSlow, 0.2f, 10.f},
    {"strokeSwimFast", &WaterLocoTuning::strokeSwimFast, 0.2f, 10.f},
    {"strokeUnderSlow", &WaterLocoTuning::strokeUnderSlow, 0.2f, 10.f},
    {"strokeUnderFast", &WaterLocoTuning::strokeUnderFast, 0.2f, 10.f},
    {"underSlowSpeed", &WaterLocoTuning::underSlowSpeed, 0.3f, 8.f},
    {"underFastSpeed", &WaterLocoTuning::underFastSpeed, 0.3f, 10.f},
    {"ladderCycle", &WaterLocoTuning::ladderCycle, 0.1f, 3.f},
    {"gaspBreath", &WaterLocoTuning::gaspBreath, 0.f, 1.f},
    {"diveEntrySpeed", &WaterLocoTuning::diveEntrySpeed, 0.f, 30.f},
};

// The cross-field pairs: a broken one puts both its fields back to their defaults (the importer).
struct Pair {
    float WaterLocoTuning::*a;
    float WaterLocoTuning::*b;
    std::string_view names; // "a, b"
    std::string_view rule;
    bool (*ok)(const WaterLocoTuning&);
};
inline constexpr Pair kPairs[] = {
    {&WaterLocoTuning::swimSpeed, &WaterLocoTuning::swimSprintSpeed, "swimSpeed, swimSprintSpeed",
     "swimSprintSpeed at least swimSpeed", [](const WaterLocoTuning& t) { return t.swimSprintSpeed >= t.swimSpeed; }},
    {&WaterLocoTuning::treadSpeed, &WaterLocoTuning::swimSpeed, "treadSpeed, swimSpeed", "treadSpeed at most swimSpeed",
     [](const WaterLocoTuning& t) { return t.treadSpeed <= t.swimSpeed; }},
    {&WaterLocoTuning::swimStopSpeed, &WaterLocoTuning::swimSpeed, "swimStopSpeed, swimSpeed",
     "swimStopSpeed under swimSpeed", [](const WaterLocoTuning& t) { return t.swimStopSpeed < t.swimSpeed; }},
    {&WaterLocoTuning::underSlowSpeed, &WaterLocoTuning::underFastSpeed, "underSlowSpeed, underFastSpeed",
     "underFastSpeed above underSlowSpeed", [](const WaterLocoTuning& t) { return t.underFastSpeed > t.underSlowSpeed; }},
};

inline constexpr float kPi = 3.14159265358979f;
inline constexpr float kDeg = kPi / 180.f;
inline bool inRange(float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float absf(float v) { return v < 0.f ? -v : v; }
inline float sat(float v) { return clampf(v, 0.f, 1.f); }
// Wrapped to -180..180.
inline float wrapDeg(float a) {
    a = std::fmod(a + 180.f, 360.f);
    if (a < 0.f) a += 360.f;
    return a - 180.f;
}
inline float frac(float v) { return v - std::floor(v); }
// The phase moved from p0 by d (d >= 0, under 1) crossed c (all 0..1, wrapped): c in (p0, p0 + d].
inline bool crossed(float p0, float d, float c) {
    if (!(d > 0.f)) return false;
    float x = c - p0;
    if (x <= 0.f) x += 1.f;
    return x <= d;
}
inline std::string text(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", double(v));
    return buf;
}

} // namespace water_loco_detail

inline bool validWaterLocoTuning(const WaterLocoTuning& t) {
    for (const water_loco_detail::Range& r : water_loco_detail::kRanges)
        if (!water_loco_detail::inRange(t.*r.field, r.lo, r.hi)) return false;
    for (const water_loco_detail::Pair& p : water_loco_detail::kPairs)
        if (!p.ok(t)) return false;
    return true;
}

// The body this frame. A bad value is replaced (WaterLocoPose::inputOk false), never trusted.
struct WaterLocoInput {
    WaterState water;                  // waterState() at the body
    SwimState swim = SwimState::Surface; // the lead's Swim after its step (a follower: WaterFollow's say)
    bool dived = false;                // Swim's dived this step
    bool surfaced = false;             // Swim's surfaced this step
    float breathShare = 1.f;           // Swim's breathShare (only the breach gasp reads it)
    SwimVec swimVelocity;              // under water: Swim's velocity (drives the pitch and the stroke)
    Vec2 move;                         // the wanted move, world planar, length 0..1 (longer: scaled to 1)
    bool sprint = false;
    float yaw = 0.f;                   // the body's facing now (deg)
    SwimVec velocity;                  // the body's velocity now (m/s)
    bool falling = false;              // airborne (only read on the step the body enters the water)
    WaterExitMove exitMove = WaterExitMove::None;
    float exitProgress = 0.f;          // 0..1 of the placed move (Mantle, LadderTop)
    float ladderClimbed = 0.f;         // m climbed so far (Ladder)
    SwimVec exitHand;                  // the ledge top the hands reach for (Mantle, LadderTop)
};

// Up to four clips at once; weights sum to 1 over the used ones. Phase is 0..1 of the clip.
struct WaterClipLayer {
    WaterClip clip = WaterClip::None;
    float weight = 0.f;
    float phase = 0.f;
};

struct WaterLocoPose {
    WaterLocoState state = WaterLocoState::Dry; // after this step
    WaterLocoState from = WaterLocoState::Dry;  // the state before the last change
    bool changed = false;     // the state changed this step
    float stateTime = 0.f;    // s in the state
    float blendSeconds = 0.f; // the last change's blend into this state
    float waterWeight = 0.f;  // 1: the water layer plays (layers); 0: locomotion (Dry, Wade)
    WaterClipLayer layers[4];
    int layerCount = 0;
    Vec2 strafe;              // the tread's local move (x forward, y right), for the anim's own 2D blend if it has one
    Vec2 velocity;            // surface states: the planar velocity to move at (m/s); else the input's, passed on
    float yaw = 0.f;          // the facing to hold (deg); under water: the input's
    float speed = 0.f;        // m/s (planar at the surface, 3D under water)
    float turnRate = 0.f;     // deg/s, + to the right (the yaw rising)
    float lean = 0.f;         // deg of roll into the turn (+ into a right turn: the turn rate's sign)
    float pitch = 0.f;        // deg under water: + nose up
    float strokePhase = 0.f;  // 0..1 of the playing stroke or kick cycle
    float sink = 0.f;         // m: add to the float height (negative: under the draft); the splash only
    float headTargetZ = 0.f;  // surface states: the surface plus headClear
    float wadeArms = 0.f;     // 0..1: the arms raised by the wade's depth (an additive on the walk)
    bool handIk = false;      // Exit: reach both hands to handTarget
    SwimVec handTarget;
    bool lockMove = false;    // EnterSplash, Recover, Exit, Rescued: the move input is not used
    bool lockActions = false; // and crouch (dive) and jump (climb, rise) must not be acted on
    std::uint32_t cue = 0;    // WaterCue bits of this step
    float cueStrength = 0.f;  // 0..1: the speed share, or the splash's
    float splashStrength = 0.f; // the entry splash's k (0..1), kept while the splash plays
    bool inputOk = true;      // false: an input value was replaced
};

class WaterLoco {
public:
    explicit WaterLoco(const WaterLocoTuning& t = WaterLocoTuning{}) { reset(t); }
    void reset(const WaterLocoTuning& t) {
        tuningOk_ = validWaterLocoTuning(t);
        tuning_ = tuningOk_ ? t : WaterLocoTuning{};
        reset();
    }
    // Dry, at rest; the tuning kept.
    void reset() {
        pose_ = WaterLocoPose{};
        vel_ = {};
        yaw_ = 0.f;
        yawSet_ = false;
        outTime_ = 0.f;
        phase_ = 0.f;
        kickPhase_ = 0.f;
        turnPhase_ = 0.f;
        pitch_ = 0.f;
        sinkK_ = 0.f;
        sinkDepth_ = 0.f;
        diveEntry_ = false;
        recoverLen_ = 0.f;
        lastFallVz_ = 0.f;
        lastFalling_ = false;
        mode_ = WaterMode::Dry;
    }
    bool tuningOk() const { return tuningOk_; }
    const WaterLocoTuning& tuning() const { return tuning_; }
    const WaterLocoPose& pose() const { return pose_; }
    WaterLocoState state() const { return pose_.state; }

    // One step. dt is clamped to 0..0.25 s (not finite: 0).
    const WaterLocoPose& step(const WaterLocoInput& raw, float dt) {
        namespace d = water_loco_detail;
        const WaterLocoTuning& T = tuning_;
        WaterLocoInput in = raw;
        bool ok = sanitize(in);
        if (!std::isfinite(dt)) {
            dt = 0.f;
            ok = false;
        }
        dt = d::clampf(dt, 0.f, 0.25f);
        if (!yawSet_) {
            yaw_ = in.yaw;
            yawSet_ = true;
        }

        WaterLocoPose& p = pose_;
        p.changed = false;
        p.cue = 0;
        p.cueStrength = 0.f;
        p.inputOk = ok;
        p.stateTime += dt;

        const WaterLocoState was = p.state;
        const bool swimMode = in.water.mode == WaterMode::Swim;
        const bool wadeMode = in.water.mode == WaterMode::Wade || in.water.mode == WaterMode::DeepWade;
        const bool dryMode = in.water.mode == WaterMode::Dry;
        const bool underSwim = in.swim == SwimState::Diving || in.swim == SwimState::Under ||
                               in.swim == SwimState::Surfacing;
        const float moveLen = std::sqrt(in.move.x * in.move.x + in.move.y * in.move.y);
        mode_ = in.water.mode;
        // Fell in: airborne on this step or the last (the host may have caught the fall already). The entry speed is the
        // faster of this step's vertical speed and the last airborne one.
        const bool fellIn = in.falling || lastFalling_;
        const float entryVz = lastFalling_ && lastFallVz_ < in.velocity.z ? lastFallVz_ : in.velocity.z;

        // --- The state machine: the external events first, then each state's own rule.
        if (in.swim == SwimState::Rescued) {
            enter(WaterLocoState::Rescued);
        } else if (in.exitMove != WaterExitMove::None && was != WaterLocoState::Dry) {
            enter(WaterLocoState::Exit);
        } else if (in.dived) {
            enter(WaterLocoState::Dive);
        } else if (in.surfaced && (isUnder(was) || was == WaterLocoState::Rescued)) {
            enter(WaterLocoState::Breach);
        } else {
            switch (was) {
            case WaterLocoState::Dry:
            case WaterLocoState::Wade:
                if (underSwim) {
                    enter(WaterLocoState::Under);
                } else if (swimMode) {
                    const float k = splashK(entryVz);
                    if (fellIn && -entryVz > T.splashMinSpeed) {
                        // Going in fast and level (a running jump): head first.
                        const float level = std::sqrt(in.velocity.x * in.velocity.x + in.velocity.y * in.velocity.y);
                        startSplash(k, in.water, level >= T.diveEntrySpeed);
                    } else if (was == WaterLocoState::Wade && !fellIn) {
                        enter(WaterLocoState::EnterWalk);
                    } else {
                        enter(WaterLocoState::Tread);
                    }
                } else if (wadeMode && was == WaterLocoState::Dry) {
                    // A fall into wading depth: a splash cue, no sink (the bed is too near).
                    if (fellIn && -entryVz > T.splashMinSpeed) {
                        p.cue |= kWaterCueSplash;
                        p.cueStrength = splashK(entryVz);
                    }
                    enter(WaterLocoState::Wade);
                } else if (dryMode && was == WaterLocoState::Wade) {
                    enter(WaterLocoState::Dry);
                }
                break;
            case WaterLocoState::EnterWalk:
                if (leftSwim(swimMode, dt)) break;
                if (p.stateTime >= waterClipSeconds(WaterClip::EnterWalk))
                    enter(forwardIntent(in.move, moveLen, yaw_) ? WaterLocoState::SurfaceSwim : WaterLocoState::Tread);
                break;
            case WaterLocoState::EnterSplash: {
                // The head breaks the surface on the way back up (after the deepest point): the recover beat.
                const float t = p.stateTime;
                if (t > 1.f / T.sinkOmega && (sinkAt(t) > -T.headClear || t > 3.f)) {
                    enter(WaterLocoState::Recover);
                    recoverLen_ = T.recoverMin + T.recoverSpan * sinkK_;
                    p.cue |= kWaterCueGasp;
                    p.cueStrength = sinkK_;
                }
                break;
            }
            case WaterLocoState::Recover:
                if (p.stateTime >= recoverLen_) enter(WaterLocoState::Tread);
                break;
            case WaterLocoState::Tread:
            case WaterLocoState::TreadTurn:
            case WaterLocoState::SurfaceSwim:
                if (underSwim) {
                    enter(WaterLocoState::Under);
                    break;
                }
                if (leftSwim(swimMode, dt)) break;
                surfaceRule(in, moveLen);
                break;
            case WaterLocoState::Dive:
                if (in.swim == SwimState::Under || in.swim == SwimState::Surfacing) enter(WaterLocoState::Under);
                else if (in.swim == SwimState::Surface && p.stateTime > waterClipSeconds(WaterClip::DiveStart))
                    enter(WaterLocoState::Tread); // the dive came to nothing
                break;
            case WaterLocoState::Under:
                if (in.swim == SwimState::Surface) enter(WaterLocoState::Breach); // surfaced without the flag
                break;
            case WaterLocoState::Breach:
                if (p.stateTime >= waterClipSeconds(WaterClip::Breach))
                    enter(forwardIntent(in.move, moveLen, yaw_) ? WaterLocoState::SurfaceSwim : WaterLocoState::Tread);
                break;
            case WaterLocoState::Exit:
                if (in.exitMove == WaterExitMove::None)
                    enter(swimMode ? WaterLocoState::Tread : (wadeMode ? WaterLocoState::Wade : WaterLocoState::Dry));
                break;
            case WaterLocoState::Rescued:
                enter(swimMode ? WaterLocoState::Tread : (wadeMode ? WaterLocoState::Wade : WaterLocoState::Dry));
                break;
            }
        }

        // Water in and out.
        const bool wasDry = was == WaterLocoState::Dry;
        const bool isDry = p.state == WaterLocoState::Dry;
        if (wasDry && !isDry) p.cue |= kWaterCueWaterIn;
        if (!wasDry && isDry) p.cue |= kWaterCueWaterOut;

        // --- The state's movement and pose.
        p.lockMove = false;
        p.lockActions = false;
        p.handIk = false;
        p.sink = 0.f;
        p.wadeArms = 0.f;
        p.strafe = {};
        p.layerCount = 0;
        p.headTargetZ = in.water.surface + T.headClear;
        const float prevYaw = yaw_;
        float speedNow = 0.f;

        switch (p.state) {
        case WaterLocoState::Dry:
        case WaterLocoState::Wade:
            p.waterWeight = 0.f;
            vel_ = {in.velocity.x, in.velocity.y};
            yaw_ = in.yaw;
            if (p.state == WaterLocoState::Wade)
                p.wadeArms = d::sat((in.water.column - 0.35f) / (kWaterSwimDepth - 0.35f));
            speedNow = len(vel_);
            pitch_ = 0.f;
            break;
        case WaterLocoState::EnterWalk: {
            p.waterWeight = 1.f;
            glide(T.swimDrag, dt);
            speedNow = len(vel_);
            layer(WaterClip::EnterWalk, 1.f, d::sat(p.stateTime / waterClipSeconds(WaterClip::EnterWalk)));
            break;
        }
        case WaterLocoState::EnterSplash:
        case WaterLocoState::Recover: {
            p.waterWeight = 1.f;
            p.lockMove = true;
            p.lockActions = true;
            glide(T.swimDrag, dt);
            speedNow = len(vel_);
            float ph;
            WaterClip clip = WaterClip::EnterSplash;
            if (p.state == WaterLocoState::EnterSplash) {
                p.sink = sinkAt(p.stateTime);
                // The sink and rise play the clip to 0.75; the recover beat holds its tail. Head first, the dive's
                // clip plays through at its own pace, and the recover is the splash's tail (the head breaking out).
                ph = 0.75f * d::sat(p.stateTime * T.sinkOmega / 3.f);
                if (diveEntry_) {
                    clip = WaterClip::EnterDive;
                    ph = d::sat(p.stateTime / waterClipSeconds(WaterClip::EnterDive));
                }
            } else {
                p.sink = 0.f;
                ph = 0.75f + 0.25f * (recoverLen_ > 0.f ? d::sat(p.stateTime / recoverLen_) : 1.f);
            }
            p.splashStrength = sinkK_;
            layer(clip, 1.f, ph);
            break;
        }
        case WaterLocoState::Tread:
        case WaterLocoState::TreadTurn: {
            p.waterWeight = 1.f;
            const float c = std::cos(yaw_ * d::kDeg), s = std::sin(yaw_ * d::kDeg);
            // Local: x forward, y right (the right of the facing (c, s) is (-s, c) in this frame).
            const float lx = in.move.x * c + in.move.y * s;
            const float ly = -in.move.x * s + in.move.y * c;
            p.strafe = {lx, ly};
            if (p.state == WaterLocoState::TreadTurn) {
                const float want = std::atan2(in.move.y, in.move.x) / d::kDeg;
                const float off = d::wrapDeg(want - yaw_);
                const float stepDeg = d::clampf(off, -T.treadTurnRate * dt, T.treadTurnRate * dt);
                yaw_ = d::wrapDeg(yaw_ + stepDeg);
                turnPhase_ = d::frac(turnPhase_ + d::absf(stepDeg) / 90.f);
                accelerate(Vec2{}, T.treadDrag, T.treadDrag, dt);
                layer(off >= 0.f ? WaterClip::TreadTurnR90 : WaterClip::TreadTurnL90, 1.f, turnPhase_);
            } else {
                const Vec2 want{in.move.x * T.treadSpeed, in.move.y * T.treadSpeed};
                accelerate(want, T.treadAccel, T.treadDrag, dt);
                treadLayers(lx, ly);
            }
            speedNow = len(vel_);
            kick(dt, waterClipSeconds(WaterClip::TreadIdle), d::sat(speedNow / T.treadSpeed));
            break;
        }
        case WaterLocoState::SurfaceSwim: {
            p.waterWeight = 1.f;
            const float top = in.sprint ? T.swimSprintSpeed : T.swimSpeed;
            if (moveLen > 0.05f) {
                const float want = std::atan2(in.move.y, in.move.x) / d::kDeg;
                const float off = d::wrapDeg(want - yaw_);
                yaw_ = d::wrapDeg(yaw_ + d::clampf(off, -T.swimTurnRate * dt, T.swimTurnRate * dt));
                // The stroke drives along the facing; a sideways input only turns the swimmer.
                const float c = std::cos(yaw_ * d::kDeg), s = std::sin(yaw_ * d::kDeg);
                const float drive = top * moveLen * d::sat(std::cos(off * d::kDeg));
                accelerate(Vec2{c * drive, s * drive}, T.swimAccel, T.swimDrag, dt);
            } else {
                glide(T.swimDrag, dt);
            }
            speedNow = len(vel_);
            const float fast = d::sat((speedNow - T.swimSpeed) / (T.swimSprintSpeed - T.swimSpeed));
            const float swim = d::sat(speedNow / (0.5f * T.swimSpeed));
            const float strokeLen = T.strokeSwimSlow + (T.strokeSwimFast - T.strokeSwimSlow) * fast;
            stroke(speedNow * dt / strokeLen, fast, d::sat(speedNow / T.swimSprintSpeed));
            if (swim < 1.f) layer(WaterClip::TreadIdle, 1.f - swim, kickPhase_);
            if (swim * (1.f - fast) > 0.f) layer(WaterClip::SwimSlow, swim * (1.f - fast), phase_);
            if (swim * fast > 0.f) layer(WaterClip::SwimFast, swim * fast, phase_);
            break;
        }
        case WaterLocoState::Dive:
        case WaterLocoState::Under:
        case WaterLocoState::Rescued: {
            p.waterWeight = 1.f;
            vel_ = {in.swimVelocity.x, in.swimVelocity.y};
            yaw_ = in.yaw;
            const SwimVec v = in.swimVelocity;
            const float horiz = std::sqrt(v.x * v.x + v.y * v.y);
            speedNow = std::sqrt(horiz * horiz + v.z * v.z);
            const float wantPitch = speedNow > 0.3f && p.state != WaterLocoState::Rescued
                                        ? d::clampf(std::atan2(v.z, horiz) / d::kDeg, -T.pitchMax, T.pitchMax)
                                        : 0.f;
            pitch_ += d::clampf(wantPitch - pitch_, -T.pitchRate * dt, T.pitchRate * dt);
            if (p.state == WaterLocoState::Rescued) {
                p.lockMove = true;
                p.lockActions = true;
                kickPhase_ = d::frac(kickPhase_ + dt / waterClipSeconds(WaterClip::UnderIdle));
                layer(WaterClip::UnderIdle, 1.f, kickPhase_);
                break;
            }
            if (p.state == WaterLocoState::Dive) {
                layer(WaterClip::DiveStart, 1.f, d::sat(p.stateTime / waterClipSeconds(WaterClip::DiveStart)));
                break;
            }
            underLayers(speedNow, dt);
            break;
        }
        case WaterLocoState::Breach: {
            p.waterWeight = 1.f;
            glide(T.swimDrag, dt);
            speedNow = len(vel_);
            pitch_ += d::clampf(0.f - pitch_, -T.pitchRate * dt, T.pitchRate * dt);
            if (p.changed) {
                p.cue |= kWaterCueSplash;
                p.cueStrength = 0.3f;
                if (in.breathShare < T.gaspBreath) p.cue |= kWaterCueGasp;
            }
            layer(WaterClip::Breach, 1.f, d::sat(p.stateTime / waterClipSeconds(WaterClip::Breach)));
            break;
        }
        case WaterLocoState::Exit: {
            p.waterWeight = 1.f;
            p.lockMove = true;
            p.lockActions = true;
            vel_ = {};
            yaw_ = in.yaw;
            pitch_ = 0.f;
            if (in.exitMove == WaterExitMove::Ladder) {
                layer(WaterClip::ExitLadder, 1.f, d::frac(in.ladderClimbed / T.ladderCycle));
            } else {
                layer(in.exitMove == WaterExitMove::LadderTop ? WaterClip::ExitLadderTop : WaterClip::ExitMantle, 1.f,
                      d::sat(in.exitProgress));
                p.handIk = true;
                p.handTarget = in.exitHand;
            }
            break;
        }
        }

        p.velocity = vel_;
        p.yaw = yaw_;
        p.speed = speedNow;
        p.turnRate = dt > 0.f ? d::wrapDeg(yaw_ - prevYaw) / dt : 0.f;
        p.lean = d::clampf(p.turnRate * p.speed * T.leanPerTurnSpeed, -T.leanMax, T.leanMax);
        p.pitch = pitch_;
        p.strokePhase = isUnder(p.state) || p.state == WaterLocoState::SurfaceSwim ? phase_ : kickPhase_;
        if (p.state != WaterLocoState::EnterSplash && p.state != WaterLocoState::Recover) p.splashStrength = 0.f;

        lastFalling_ = in.falling;
        lastFallVz_ = in.velocity.z;
        return p;
    }

private:
    static float len(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
    static bool isUnder(WaterLocoState s) { return s == WaterLocoState::Dive || s == WaterLocoState::Under; }

    // Replaces each bad value; false if any was.
    static bool sanitize(WaterLocoInput& in) {
        namespace d = water_loco_detail;
        bool ok = true;
        const auto fix = [&ok](float& v, float to) {
            if (!std::isfinite(v)) {
                v = to;
                ok = false;
            }
        };
        fix(in.water.surface, 0.f);
        fix(in.water.column, 0.f);
        if (in.water.column < 0.f) {
            in.water.column = 0.f;
            ok = false;
        }
        fix(in.breathShare, 1.f);
        fix(in.swimVelocity.x, 0.f);
        fix(in.swimVelocity.y, 0.f);
        fix(in.swimVelocity.z, 0.f);
        fix(in.move.x, 0.f);
        fix(in.move.y, 0.f);
        const float l = std::sqrt(in.move.x * in.move.x + in.move.y * in.move.y);
        if (l > 1.f) {
            in.move.x /= l;
            in.move.y /= l;
        }
        fix(in.yaw, 0.f);
        in.yaw = d::wrapDeg(in.yaw);
        fix(in.velocity.x, 0.f);
        fix(in.velocity.y, 0.f);
        fix(in.velocity.z, 0.f);
        fix(in.exitProgress, 0.f);
        fix(in.ladderClimbed, 0.f);
        fix(in.exitHand.x, 0.f);
        fix(in.exitHand.y, 0.f);
        fix(in.exitHand.z, 0.f);
        return ok;
    }

    // The blend into `to` from `from` (seconds), per the design's transition table.
    static float blendFor(WaterLocoState from, WaterLocoState to) {
        using S = WaterLocoState;
        switch (to) {
        case S::EnterWalk: return 0.25f;
        case S::EnterSplash: return 0.1f;
        case S::Recover: return 0.1f;
        case S::Dive: return 0.15f;
        case S::Breach: return 0.15f;
        case S::Exit: return 0.1f;
        case S::Under: return 0.3f;
        case S::TreadTurn: return 0.2f;
        case S::SurfaceSwim: return from == S::Tread ? 0.35f : 0.3f;
        case S::Tread:
            if (from == S::SurfaceSwim) return 0.5f;
            if (from == S::TreadTurn) return 0.2f;
            return 0.3f;
        case S::Wade: return from == S::Dry ? 0.2f : 0.4f;
        case S::Dry: return 0.2f;
        case S::Rescued: return 0.3f;
        }
        return 0.3f;
    }

    void enter(WaterLocoState s) {
        if (pose_.state == s) return;
        pose_.from = pose_.state;
        pose_.blendSeconds = blendFor(pose_.state, s);
        pose_.state = s;
        pose_.stateTime = 0.f;
        pose_.changed = true;
        outTime_ = 0.f;
    }

    // Surface states: out of swimming depth for outHold, they give way to the wade or dry ground.
    bool leftSwim(bool swimMode, float dt) {
        if (swimMode) {
            outTime_ = 0.f;
            return false;
        }
        outTime_ += dt;
        if (outTime_ < tuning_.outHold) return false;
        // waterState says wade or dry; enter() resets outTime_.
        enter(mode_ == WaterMode::Dry ? WaterLocoState::Dry : WaterLocoState::Wade);
        return true;
    }

    bool forwardIntent(Vec2 move, float moveLen, float yaw) const {
        namespace d = water_loco_detail;
        if (moveLen < tuning_.intentMin) return false;
        const float want = std::atan2(move.y, move.x) / d::kDeg;
        return d::absf(d::wrapDeg(want - yaw)) <= tuning_.treadTurnAngle;
    }

    void surfaceRule(const WaterLocoInput& in, float moveLen) {
        namespace d = water_loco_detail;
        const WaterLocoTuning& T = tuning_;
        const float want = moveLen > 0.f ? std::atan2(in.move.y, in.move.x) / d::kDeg : yaw_;
        const float off = d::absf(d::wrapDeg(want - yaw_));
        switch (pose_.state) {
        case WaterLocoState::Tread:
            if (moveLen >= T.intentMin) enter(off <= T.treadTurnAngle ? WaterLocoState::SurfaceSwim : WaterLocoState::TreadTurn);
            break;
        case WaterLocoState::TreadTurn:
            if (moveLen < T.intentMin) enter(WaterLocoState::Tread);
            else if (off <= T.treadTurnAngle) enter(WaterLocoState::SurfaceSwim);
            break;
        case WaterLocoState::SurfaceSwim:
            if (moveLen < 0.05f && len(vel_) < T.swimStopSpeed) enter(WaterLocoState::Tread);
            break;
        default: break;
        }
    }

    float splashK(float vz) const {
        return water_loco_detail::sat((-vz - tuning_.splashMinSpeed) / tuning_.splashSpeedSpan);
    }

    void startSplash(float k, const WaterState& w, bool headFirst) {
        const WaterLocoTuning& T = tuning_;
        sinkK_ = k;
        diveEntry_ = headFirst;
        // The sink never brings the feet nearer the bed than bedClearance.
        const float room = w.column - kWaterSwimDraft - T.bedClearance;
        sinkDepth_ = water_loco_detail::clampf(T.sinkMin + T.sinkSpan * k, 0.f, room > 0.f ? room : 0.f);
        enter(WaterLocoState::EnterSplash);
        pose_.cue |= kWaterCueSplash;
        pose_.cueStrength = k;
    }

    // The critically damped sink: 0 at the entry, -sinkDepth_ at 1/omega, back toward 0.
    float sinkAt(float t) const {
        const float w = tuning_.sinkOmega;
        return -sinkDepth_ * 2.718281828f * w * t * std::exp(-w * t);
    }

    // Toward `want` at `accel`; with a zero want, slows at `drag`.
    void accelerate(Vec2 want, float accel, float drag, float dt) {
        const float rate = (want.x == 0.f && want.y == 0.f) ? drag : accel;
        const Vec2 dv{want.x - vel_.x, want.y - vel_.y};
        const float l = len(dv);
        const float stepLen = rate * dt;
        if (l <= stepLen || l <= 0.f) {
            vel_ = want;
        } else {
            vel_.x += dv.x / l * stepLen;
            vel_.y += dv.y / l * stepLen;
        }
    }
    void glide(float drag, float dt) { accelerate(Vec2{}, drag, drag, dt); }

    void layer(WaterClip c, float w, float phase) {
        if (pose_.layerCount >= 4 || !(w > 0.f)) return;
        pose_.layers[pose_.layerCount++] = WaterClipLayer{c, w, phase};
    }

    // Tread: TreadIdle under the two directional clips the local move leans toward, weighted by its length.
    void treadLayers(float lx, float ly) {
        namespace d = water_loco_detail;
        const float m = d::sat(std::sqrt(lx * lx + ly * ly));
        const float ax = d::absf(lx), ay = d::absf(ly);
        const float sum = ax + ay;
        layer(WaterClip::TreadIdle, 1.f - m, kickPhase_);
        if (sum > 0.f) {
            layer(lx >= 0.f ? WaterClip::TreadFwd : WaterClip::TreadBack, m * ax / sum, kickPhase_);
            layer(ly >= 0.f ? WaterClip::TreadR : WaterClip::TreadL, m * ay / sum, kickPhase_);
        }
        if (pose_.layerCount == 0) layer(WaterClip::TreadIdle, 1.f, kickPhase_);
    }

    // The tread's kick loop at its own rate; a kick cue at each of the left leg's eggbeater in-sweeps (the r2 clips'
    // frames 13, 36, 58 and 81 of 90; the right leg's sit between and stay quiet, so the cue is not doubled).
    void kick(float dt, float cycle, float strength) {
        namespace d = water_loco_detail;
        static constexpr float kKicks[] = {13.f / 90.f, 36.f / 90.f, 58.f / 90.f, 81.f / 90.f};
        const float dp = dt / cycle;
        for (float c : kKicks) {
            if (d::crossed(kickPhase_, dp, c)) {
                pose_.cue |= kWaterCueKick;
                pose_.cueStrength = pose_.cueStrength > strength ? pose_.cueStrength : strength;
            }
        }
        kickPhase_ = d::frac(kickPhase_ + dp);
    }

    // The surface stroke: SwimSlow's two-handed catch at 0; SwimFast's cycle is six strokes, the r2 clip's catches
    // left at frames 4, 36 and 68 of 96 and right at 20, 52 and 84.
    void stroke(float dp, float fast, float strength) {
        namespace d = water_loco_detail;
        static constexpr float kLeft[] = {4.f / 96.f, 36.f / 96.f, 68.f / 96.f};
        static constexpr float kRight[] = {20.f / 96.f, 52.f / 96.f, 84.f / 96.f};
        if (dp > 0.99f) dp = 0.99f;
        bool any = false;
        if (fast < 0.5f) {
            if (d::crossed(phase_, dp, 0.f)) {
                pose_.cue |= kWaterCueStrokeL | kWaterCueStrokeR;
                any = true;
            }
        } else {
            for (int i = 0; i < 3; ++i) {
                if (d::crossed(phase_, dp, kLeft[i])) {
                    pose_.cue |= kWaterCueStrokeL;
                    any = true;
                }
                if (d::crossed(phase_, dp, kRight[i])) {
                    pose_.cue |= kWaterCueStrokeR;
                    any = true;
                }
            }
        }
        if (any) pose_.cueStrength = pose_.cueStrength > strength ? pose_.cueStrength : strength;
        phase_ = d::frac(phase_ + dp);
    }

    // Under water: UnderIdle (the hover) to UnderSlow to UnderFast by speed, and the pitch pose by elevation.
    void underLayers(float speed, float dt) {
        namespace d = water_loco_detail;
        const WaterLocoTuning& T = tuning_;
        const float slow = d::sat(speed / T.underSlowSpeed);
        const float fast = d::sat((speed - T.underSlowSpeed) / (T.underFastSpeed - T.underSlowSpeed));
        const float strokeLen = T.strokeUnderSlow + (T.strokeUnderFast - T.strokeUnderSlow) * fast;
        // Hovering, the loop runs at its own rate; moving, it follows the travel.
        const float dp = slow < 1.f ? (1.f - slow) * dt / waterClipSeconds(WaterClip::UnderIdle) + slow * speed * dt / strokeLen
                                    : speed * dt / strokeLen;
        const float clamped = dp > 0.99f ? 0.99f : dp;
        if (d::crossed(phase_, clamped, 0.f)) {
            pose_.cue |= kWaterCueKick;
            pose_.cueStrength = d::sat(speed / T.underFastSpeed);
        }
        phase_ = d::frac(phase_ + clamped);
        const float pw = T.pitchMax > 0.f ? 0.6f * d::sat(d::absf(pitch_) / T.pitchMax) : 0.f;
        const float body = 1.f - pw;
        layer(WaterClip::UnderIdle, body * (1.f - slow), phase_);
        layer(WaterClip::UnderSlow, body * slow * (1.f - fast), phase_);
        layer(WaterClip::UnderFast, body * slow * fast, phase_);
        if (pw > 0.f) layer(pitch_ >= 0.f ? WaterClip::PitchUp : WaterClip::PitchDown, pw, 1.f);
    }

    WaterLocoTuning tuning_;
    bool tuningOk_ = true;
    WaterLocoPose pose_;
    Vec2 vel_;
    float yaw_ = 0.f;
    bool yawSet_ = false;
    float outTime_ = 0.f;
    float phase_ = 0.f;     // the stroke (surface swim, under water)
    float kickPhase_ = 0.f; // the tread's loop
    float turnPhase_ = 0.f;
    float pitch_ = 0.f;
    float sinkK_ = 0.f;
    float sinkDepth_ = 0.f;
    bool diveEntry_ = false; // the splash went in head first (EnterDive)
    float recoverLen_ = 0.f;
    float lastFallVz_ = 0.f;
    bool lastFalling_ = false;
    WaterMode mode_ = WaterMode::Dry; // this step's water mode (leftSwim chooses wade or dry ground by it)
};

// Reads Data/Swim/water_loco.json: {"version":1, "_comment": "...", and any WaterLocoTuning field by name}. Lenient
// like importSwimTuning: a missing, non-numeric or out-of-range field takes its default (noted); an unknown field is
// skipped (noted); a broken cross-field pair puts both its fields back to their defaults (noted). Only a file that
// does not parse, is not an object or is not version 1 fails: `out` is the defaults, false is returned and `error`
// says why.
inline bool importWaterLocoTuning(std::string_view json, WaterLocoTuning& out, std::string* error = nullptr,
                                  std::vector<std::string>* notes = nullptr) {
    namespace d = water_loco_detail;
    out = WaterLocoTuning{};
    if (error) error->clear();
    if (notes) notes->clear();
    const auto fail = [&](std::string why) {
        out = WaterLocoTuning{};
        if (error) *error = std::move(why);
        return false;
    };
    const auto note = [&](std::string n) {
        if (notes) notes->push_back(std::move(n));
    };
    JsonDocument doc;
    std::string err;
    if (!doc.parse(json, &err)) return fail("water loco file: " + err);
    const JsonValue root = doc.root();
    if (!root.isObject()) return fail("water loco file: expected an object");
    if (const JsonValue v = root["version"]; !(v.isInteger() && v.integer() == 1)) return fail("version: expected 1");
    const WaterLocoTuning def;
    WaterLocoTuning t;
    for (const d::Range& r : d::kRanges) {
        const JsonValue v = root[r.name];
        const std::string fallback = ", the default " + d::text(def.*r.field) + " used";
        if (v.isAbsent()) {
            note(std::string(r.name) + ": missing" + fallback);
            continue;
        }
        const float f = v.isNumber() ? float(v.number()) : 0.f;
        if (!v.isNumber() || !d::inRange(f, r.lo, r.hi))
            note(std::string(r.name) + ": expected a number in " + d::text(r.lo) + ".." + d::text(r.hi) + fallback);
        else
            t.*r.field = f;
    }
    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        bool known = k == "version" || k == "_comment";
        for (const d::Range& r : d::kRanges) known = known || k == r.name;
        if (!known) note(std::string(k) + ": unknown field, skipped");
    }
    bool again = true;
    for (std::size_t pass = 0; again && pass <= std::size(d::kPairs); ++pass) {
        again = false;
        for (const d::Pair& p : d::kPairs) {
            if (p.ok(t)) continue;
            note(std::string(p.names) + ": expected " + std::string(p.rule) + ", both defaults used");
            t.*p.a = def.*p.a;
            t.*p.b = def.*p.b;
            again = true;
        }
    }
    out = t;
    return true;
}

} // namespace ftcore
