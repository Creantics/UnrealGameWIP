#pragma once

// Which locomotion clips a body plays, at what weight, time and rate, from how its capsule moves.
//
//   ftcore::Locomotion loco;
//   loco.setClip(LocoClip::Walk, {1.5f, 298.f, 2, 0.1f});   // once per clip, from the asset
//   loco.update({dt, groundSpeed, turnRate, falling, verticalSpeed});   // every frame
//   for (i < loco.layerCount()) pose += sample(loco.layer(i).clip, loco.layer(i).time) * loco.layer(i).weight;
//   then bend the spine by loco.lean() (roll into the turn), loco.twist() (upper body leads the
//   turn) and loco.pitch() (forward lean at a run).
//
// On the ground it plays idle or a blend of the gaits it has (walk, run, sprint). The speed picks
// the blend: between two adjacent gaits' ground speeds it mixes them linearly, below the slowest it
// plays that one alone, above the fastest that one. The gaits share one foot phase (0 at a left
// plant), advanced at the rate that makes the blended planted foot move exactly as fast as the
// capsule: every gait plays at the point in its loop where the same foot is in the same place in
// its stride, so the blend never slides, whatever the speed. A jump plays the jump start, then the
// fall loop, then the landing, which hands back to the gaits early when the body keeps moving. A
// body moving at movingAir or faster takes off and lands with the moving clips (JumpMoving,
// LandMoving) when it has them; they play at the rate their root travel matches the capsule's,
// within limits. Landing at speed with only a standing landing skips it, since its feet would
// slide. A step down shorter than airGrace stays on the ground clips. Blends are exponential
// crossfades. Fixed size: nothing here allocates.
//
// Starts and stops, when the caller says where the body is heading (LocoInput::wantSpeed) and the
// clips have their root travel measured (measureMove): from idle, a body heading off plays the start
// clip of the gait it heads for, at the point whose root travel matches the distance the capsule has
// covered since, and hands over to the gait once the clip nears the gait's speed, at the gait's
// foot phase that best matches the clip's feet. A moving body told to stop plays the stop clip of
// its gait (the one stopping on whichever foot matches best) at the point whose remaining root
// travel matches the capsule's remaining braking distance (LocoInput::stopDistance), then its
// settle, then idle. Heading off again mid-stop goes back to the gait the same way.
//
// Speeds are cm/s, times seconds, angles degrees. turnRate is the body's yaw rate, positive one way
// (the caller's); lean() and twist() have its sign.

#include "ftcore/Api.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ftcore {

// Starts are each gait's start from standing; stops each gait's stop, planting the left or the right
// foot last.
enum class LocoClip : std::uint8_t {
    Idle, Walk, Run, Sprint, JumpStart, Fall, Land, JumpMoving, LandMoving,
    WalkStart, RunStart, SprintStart,
    WalkStopLeft, WalkStopRight, RunStopLeft, RunStopRight, SprintStopLeft, SprintStopRight,
    Count
};
inline constexpr std::size_t kLocoClipCount = static_cast<std::size_t>(LocoClip::Count);

constexpr bool isLocoGait(LocoClip c) { return c == LocoClip::Walk || c == LocoClip::Run || c == LocoClip::Sprint; }
constexpr bool isLocoLoop(LocoClip c) { return c == LocoClip::Idle || c == LocoClip::Fall || isLocoGait(c); }
constexpr bool isLocoStart(LocoClip c) { return c >= LocoClip::WalkStart && c <= LocoClip::SprintStart; }
constexpr bool isLocoStop(LocoClip c) { return c >= LocoClip::WalkStopLeft && c <= LocoClip::SprintStopRight; }
// A gait's start, and its stops.
constexpr LocoClip locoStart(LocoClip gait) {
    return static_cast<LocoClip>(static_cast<int>(LocoClip::WalkStart) + static_cast<int>(gait) - static_cast<int>(LocoClip::Walk));
}
constexpr LocoClip locoStop(LocoClip gait, bool right) {
    return static_cast<LocoClip>(static_cast<int>(LocoClip::WalkStopLeft) + 2 * (static_cast<int>(gait) - static_cast<int>(LocoClip::Walk)) + (right ? 1 : 0));
}

enum class LocoState : std::uint8_t { Ground, Rising, Falling, Landing, Starting, Stopping };

// One sample of a clip for measureGait: the root bone and the two feet (ball joints), in the
// clip's space (cm, z up).
struct LocoPoint {
    float x = 0.f, y = 0.f, z = 0.f;
};
struct GaitFrame {
    LocoPoint root, left, right;
};
inline constexpr std::size_t kMaxGaitFrames = 512;

// The feet relative to the root.
struct LocoFeet {
    LocoPoint left, right;
};
inline constexpr std::size_t kMoveSamples = 64;

// What the anim layer knows about one clip.
struct LocoClipInfo {
    float length = 0.f;      // seconds; 0 means the body has no such clip
    float groundSpeed = 0.f; // gaits and moving jumps: the capsule speed at which the feet don't slide at rate 1
    int cycles = 1;          // gaits: strides (plants of one foot) per loop
    float leftPlant = 0.f;   // gaits: normalized time (0..1) of the first left plant
    float footSpeed = 0.f;   // measureGait: the planted feet's own median speed (which groundSpeed takes)
    float footSide = 0.f;    // measureGait: their median speed across the stride; near 0 unless the feet slide sideways
    // measureMove: the time the root moves for (from 0; a stop then settles in place), how far it
    // travels, and, sampled evenly over that time, its path length from time 0 and the feet. Starts
    // and stops need the path; matching a gait to a start or stop needs the feet.
    float moveTime = 0.f;
    float travel = 0.f;
    std::array<float, kMoveSamples> path{};
    std::array<LocoFeet, kMoveSamples> feet{};
};

// Measures a gait clip from `count` frames sampled evenly over one loop (frame 0 at time 0, the
// last at `length`; at most kMaxGaitFrames are read). A foot is planted while it is within
// kPlantHeight of its lowest point relative to the root and moving back under it (against the way
// it swings), and lifted once it rises kSwingHeight above it. groundSpeed is the median speed of a
// planted foot relative to the root (with or without root motion); cycles counts the left foot's plants, and leftPlant is the first one's normalized time. A
// clip whose feet never lift gets groundSpeed 0. It measures the move too (measureMove).
inline constexpr float kPlantHeight = 2.5f;
inline constexpr float kSwingHeight = 8.f;
FTCORE_API LocoClipInfo measureGait(const GaitFrame* frames, std::size_t count, float length);
// Measures any clip's root travel and feet (the move fields), from frames sampled as for
// measureGait. The root moves until it is last more than kMoveStill from where it ends.
inline constexpr float kMoveStill = 0.5f;
FTCORE_API LocoClipInfo measureMove(const GaitFrame* frames, std::size_t count, float length);

struct LocoTuning {
    float moveStart = 25.f;     // ground speed that starts a gait from idle
    float moveStop = 10.f;      // and that drops back to idle
    float gaitFade = 0.08f;     // crossfade time constant on the ground (95% in three)
    float airFade = 0.05f;      // into the jump and the fall
    float landFade = 0.03f;     // into the landing
    float airGrace = 0.12f;     // falling shorter than this (a step down) keeps the ground clips
    float landMinAir = 0.3f;    // a shorter time in the air skips the landing
    float landHoldMoving = 0.2f; // landing plays at least this long before a moving body resumes its gait
    float movingAir = 150.f;    // at this ground speed or more, jumps and landings use the moving clips
    float minLandRate = 0.5f;   // the moving jump's and landing's rate range
    float maxLandRate = 2.f;
    float maxRate = 3.f;        // gait rate cap
    float leanPerTurn = 0.035f; // roll per deg/s of turning, at the run's ground speed (less when slower)
    float maxLean = 10.f;
    float twistPerTurn = 0.05f; // upper body yaw per deg/s of turning
    float maxTwist = 18.f;
    float runPitch = 6.f;       // forward lean added at the fastest gait's ground speed (none at the walk's)
    float bodyResponse = 10.f;  // how fast lean, twist and pitch follow (1/s)
    float moveRate = 3.f;       // starts and stops play at most this fast to catch up with the capsule
    float startHandoff = 0.9f;  // a start hands over once its root moves this share of the gait's ground speed
    float startEnd = 0.85f;     // or once it is this far through
    float stopSettle = 0.25f;   // a stop hands over to idle this long before its end
    float stopDelay = 0.05f;    // braking starts a stop once it has lasted this long (a dropped frame of input doesn't)
    float poseVelocity = 0.01f; // feet matching: weight of the feet's velocity (s^2) against their position
};

struct LocoInput {
    float dt = 0.f;
    float groundSpeed = 0.f;
    float turnRate = 0.f;
    bool falling = false;
    float verticalSpeed = 0.f; // positive up
    // Where the body is heading: the speed it is speeding up to or holding, 0 when it is braking to a
    // stop, below 0 when the caller doesn't say (no starts or stops then).
    float wantSpeed = -1.f;
    // While braking: the distance to standstill (below 0: unknown, no stop clip).
    float stopDistance = -1.f;
};

struct LocoLayer {
    LocoClip clip = LocoClip::Idle;
    float time = 0.f;
    float weight = 0.f;
};

class Locomotion {
public:
    static constexpr float kMinWeight = 0.002f; // lighter layers are dropped

    Locomotion() { reset(); }

    FTCORE_API void setClip(LocoClip clip, const LocoClipInfo& info);
    const LocoClipInfo& clip(LocoClip c) const { return info_[index(c)]; }
    LocoTuning& tuning() { return tuning_; }

    // Back to standing idle at full weight.
    FTCORE_API void reset();
    FTCORE_API void update(const LocoInput& in);

    LocoState state() const { return state_; }
    // The clip the blend is heading for (on the ground, the heaviest gait).
    LocoClip target() const { return target_; }
    // Layers with weight this frame, weights summing to 1.
    std::size_t layerCount() const { return layerCount_; }
    const LocoLayer& layer(std::size_t i) const { return layers_[i]; }
    float weight(LocoClip c) const { return weight_[index(c)]; }
    float time(LocoClip c) const { return time_[index(c)]; }
    float rate(LocoClip c) const { return rate_[index(c)]; }

    // The gaits' shared foot phase (0..1, 0 at a left plant).
    float phase() const { return phase_; }
    // The foot phase of a gait clip at `time`.
    FTCORE_API float footPhase(LocoClip c, float time) const;
    // The time in a gait clip at which its foot phase is `phase`.
    FTCORE_API float timeAtPhase(LocoClip c, float phase) const;

    float lean() const { return lean_; }
    float twist() const { return twist_; }
    float pitch() const { return pitch_; }

private:
    static constexpr std::size_t index(LocoClip c) { return static_cast<std::size_t>(c); }
    bool has(LocoClip c) const { return info_[index(c)].length > 0.f; }
    bool hasMove(LocoClip c) const { return has(c) && info_[index(c)].moveTime > 0.f; }
    void splitGaits(float speed);
    LocoClip groundTarget(float speed);
    LocoClip heaviestGait() const;
    // The ground states with starts and stops; returns the clip to head for. syncFrom is set to a
    // clip (and syncTime to its time) when the gaits should pick up from its feet.
    LocoClip moveTarget(const LocoInput& in, float speed, LocoClip& syncFrom, float& syncTime);
    bool beginStop(const LocoInput& in, float speed);
    void syncGaits(LocoClip from, float time, float speed);
    void retarget(LocoClip next);
    void advance(float dt, float speed);

    std::array<LocoClipInfo, kLocoClipCount> info_{};
    std::array<float, kLocoClipCount> weight_{1.f};
    std::array<float, kLocoClipCount> time_{};
    std::array<float, kLocoClipCount> rate_{};
    std::array<float, kLocoClipCount> share_{}; // each gait's part of the ground blend at this speed
    std::array<int, kLocoClipCount> stride_{};  // which of its loop's strides each gait is in
    std::array<LocoLayer, kLocoClipCount> layers_{};
    std::size_t layerCount_ = 1;
    LocoTuning tuning_;
    LocoState state_ = LocoState::Ground;
    LocoClip target_ = LocoClip::Idle;
    LocoClip gait_ = LocoClip::Idle; // the ground clip (the heaviest gait), kept through the air for the landing
    LocoClip jump_ = LocoClip::JumpStart; // the take-off and landing clips this jump uses
    LocoClip land_ = LocoClip::Land;
    LocoClip move_ = LocoClip::RunStart; // the start or stop playing
    float moved_ = 0.f; // starting: the distance covered since
    float braking_ = 0.f; // how long the body has been braking
    float enter_ = -1.f; // the time a start or stop just begun enters at, once it is the target
    float fade_ = 0.f;
    float phase_ = 0.f; // within the current stride
    float airTime_ = 0.f;
    float landTime_ = 0.f;
    float lean_ = 0.f;
    float twist_ = 0.f;
    float pitch_ = 0.f;
};

} // namespace ftcore
