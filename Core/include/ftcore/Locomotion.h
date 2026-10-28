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
// The forward lean also leads a start (more while there is speed to gain), leans back into a stop,
// bobs forward as the stop comes to a stand, tucks in the air and dips into a landing (both on top of
// the gait's lean at the speed, the dip easing out from touchdown even once the body has gone back to
// its gait); the head keeps level against all of it. At a run the elbows are held bent (elbowHold()).
//
// On the ground it plays idle or a blend of the gaits it has (walk, run, sprint). The speed picks
// the blend: between two adjacent gaits it mixes them linearly from the slower one's ground speed up
// to gaitBand short of the faster one's, where that one plays alone; below the slowest it plays that
// one alone, above the fastest that one. The gaits share one foot phase (0 at a left
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
// Crouching (LocoInput::crouch, when the body has CrouchIdle and CrouchWalk): the crouch walk is a gait
// on the same foot phase, so a moving body eases between its standing gaits and the crouch walk over
// crouchFade in step. A standing body gets down (CrouchDown) or up (CrouchUp) before its idle, unless it
// moves off first. Crouched there are no starts, stops or running turns; turns on the spot use the
// crouch turns.
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
// Turns, when the caller steers (LocoInput::steer) and the clips have their root's yaw measured
// (measureMove): the body faces the way it moves, easing toward it (yawStep). Heading at least turnMin
// away from its facing while running plays a running turn: the clip whose turn is nearest (each
// clip's measured turn picks it, not its name), on the foot and from the point before the turn
// whose feet best match the gait's, its yaw scaled to the angle asked for, and back to the gait in
// step once the turn is done. Setting off at least refaceMin away from the facing plays a turning
// start; standing, LocoInput::face turns on the spot. A caller that moves the body by the turn clip's
// root motion (LocoInput::rootMotion) plays it at rate 1 and moves the body by turnFrom..turnTo of
// it; otherwise (a follower placed on the lead's path) it plays at the point whose root path matches
// the distance the body has come, and the caller turns the body by turnYaw. A follower replays a
// running turn with LocoInput::turnBy.
//
// The air pose (LocoClip::AirPose), when the body has one: a pose held over the limbs in the air,
// since the mocap jumps fling the arms wide and leave the legs hanging. limbs() gives the arms' and
// legs' weights over whatever the layers blend, sampled at the clip's end. Both ease in at take-off;
// at touchdown the legs ease out into the landing's crouch, the arms landArms after it (the landing's
// first moments throw the arms out too), even when a moving body has gone back to its gait sooner.
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
    // Running turns by 45, 90, 135 and 180 degrees each way, planting the left (L) or right (R) foot
    // as they turn; starts from standing that turn 90 or 180 degrees as they set off; turns on the
    // spot. Left and right as the clips are named: Locomotion picks them by their measured turn.
    RunTurnLeft45L, RunTurnLeft45R, RunTurnLeft90L, RunTurnLeft90R, RunTurnLeft135L, RunTurnLeft135R, RunTurnLeft180L, RunTurnLeft180R,
    RunTurnRight45L, RunTurnRight45R, RunTurnRight90L, RunTurnRight90R, RunTurnRight135L, RunTurnRight135R, RunTurnRight180L, RunTurnRight180R,
    RefaceLeft90, RefaceLeft180, RefaceRight90, RefaceRight180,
    StandTurnLeft45, StandTurnLeft90, StandTurnLeft135, StandTurnLeft180,
    StandTurnRight45, StandTurnRight90, StandTurnRight135, StandTurnRight180,
    AirPose,
    // Crouching: the idle and the walk loops, getting down and up from a stand, and turns on the spot.
    CrouchIdle, CrouchWalk, CrouchDown, CrouchUp,
    CrouchTurnLeft45, CrouchTurnLeft90, CrouchTurnLeft135, CrouchTurnLeft180,
    CrouchTurnRight45, CrouchTurnRight90, CrouchTurnRight135, CrouchTurnRight180,
    Count
};
inline constexpr std::size_t kLocoClipCount = static_cast<std::size_t>(LocoClip::Count);

constexpr bool isLocoGait(LocoClip c) {
    return c == LocoClip::Walk || c == LocoClip::Run || c == LocoClip::Sprint || c == LocoClip::CrouchWalk;
}
constexpr bool isLocoLoop(LocoClip c) { return c == LocoClip::Idle || c == LocoClip::CrouchIdle || c == LocoClip::Fall || isLocoGait(c); }
constexpr bool isLocoStart(LocoClip c) { return c >= LocoClip::WalkStart && c <= LocoClip::SprintStart; }
constexpr bool isLocoStop(LocoClip c) { return c >= LocoClip::WalkStopLeft && c <= LocoClip::SprintStopRight; }
constexpr bool isLocoRunTurn(LocoClip c) { return c >= LocoClip::RunTurnLeft45L && c <= LocoClip::RunTurnRight180R; }
constexpr bool isLocoReface(LocoClip c) { return c >= LocoClip::RefaceLeft90 && c <= LocoClip::RefaceRight180; }
constexpr bool isLocoStandTurn(LocoClip c) { return c >= LocoClip::StandTurnLeft45 && c <= LocoClip::StandTurnRight180; }
constexpr bool isLocoCrouchTurn(LocoClip c) { return c >= LocoClip::CrouchTurnLeft45 && c <= LocoClip::CrouchTurnRight180; }
constexpr bool isLocoTurn(LocoClip c) { return isLocoRunTurn(c) || isLocoReface(c) || isLocoStandTurn(c) || isLocoCrouchTurn(c); }
// The turns by angle: running turns and turns on the spot by kLocoTurnAngles[angle], turning starts
// by kLocoRefaceAngles[angle].
inline constexpr int kLocoTurnAngles[] = {45, 90, 135, 180};
inline constexpr int kLocoRefaceAngles[] = {90, 180};
constexpr LocoClip locoRunTurn(bool right, int angle, bool rightFoot) {
    return static_cast<LocoClip>(static_cast<int>(LocoClip::RunTurnLeft45L) + (right ? 8 : 0) + 2 * angle + (rightFoot ? 1 : 0));
}
constexpr LocoClip locoReface(bool right, int angle) {
    return static_cast<LocoClip>(static_cast<int>(LocoClip::RefaceLeft90) + (right ? 2 : 0) + angle);
}
constexpr LocoClip locoStandTurn(bool right, int angle) {
    return static_cast<LocoClip>(static_cast<int>(LocoClip::StandTurnLeft45) + (right ? 4 : 0) + angle);
}
constexpr LocoClip locoCrouchTurn(bool right, int angle) {
    return static_cast<LocoClip>(static_cast<int>(LocoClip::CrouchTurnLeft45) + (right ? 4 : 0) + angle);
}
// A standing gait's start, and its stops (the crouch walk has none).
constexpr LocoClip locoStart(LocoClip gait) {
    return static_cast<LocoClip>(static_cast<int>(LocoClip::WalkStart) + static_cast<int>(gait) - static_cast<int>(LocoClip::Walk));
}
constexpr LocoClip locoStop(LocoClip gait, bool right) {
    return static_cast<LocoClip>(static_cast<int>(LocoClip::WalkStopLeft) + 2 * (static_cast<int>(gait) - static_cast<int>(LocoClip::Walk)) + (right ? 1 : 0));
}

// StanceChange: standing still, getting down into a crouch or up from one.
enum class LocoState : std::uint8_t { Ground, Rising, Falling, Landing, Starting, Stopping, Turning, StanceChange };

// One sample of a clip for measureGait: the root bone and the two feet (ball joints), in the
// clip's space (cm, z up), and the root's yaw (degrees, positive turning +x toward +y).
struct LocoPoint {
    float x = 0.f, y = 0.f, z = 0.f;
};
struct GaitFrame {
    LocoPoint root, left, right;
    float yaw = 0.f;
};
inline constexpr std::size_t kMaxGaitFrames = 512;

// The feet relative to the root, turned with it (by its yaw since the clip's start).
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
    // measureMove: the root's yaw from its start (degrees, unwrapped), sampled like the path; how far
    // it turns in all; and the times it has done 5% and 95% of that (all zero for a clip turning less
    // than kTurnStill).
    std::array<float, kMoveSamples> yaw{};
    float turn = 0.f;
    float turnStart = 0.f;
    float turnEnd = 0.f;
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
inline constexpr float kTurnStill = 5.f;
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
    float phaseGain = 0.4f;     // a follower's stride rate trim per stride of phase error (LocoInput::leadPhase), on top of
                                // its stride against the lead's (LocoInput::leadStride: the rate that keeps step on its own)...
    float phaseTrim = 0.03f;    // ...the two at most this share either way (the feet slip by speed x trim; Duy's run stride is
                                // 2.6% over the Third's)
    float phaseUpBias = 1.5f;   // it steps faster to the next quarter stride unless that is this many times further than slowing
                                // to the last one: trimming down adds to the forward slip of the plants, trimming up takes from it
    float gaitDeadBand = 0.03f; // a gait with less of the mix than this drops out, the nearer gait playing alone (the
                                // blend steps by this much in a frame where it crosses)
    float gaitBand = 0.12f;     // the faster of two gaits plays alone from this share short of its own ground speed (a
                                // little slowed: the stride rate follows the speed); the MetaHuman bodies' run (483-489)
                                // would hold 13% walk at the 450 jog, which takes from its arm pump and knee lift
    float leanPerTurn = 0.035f; // roll per deg/s of turning, at the run's ground speed (less when slower)
    float maxLean = 14.f;
    float twistPerTurn = 0.05f; // upper body yaw per deg/s of turning
    float twistLead = 0.25f;    // in a turn clip: upper body yaw per degree of the turn still to come
    float maxTwist = 22.f;
    float runPitch = -6.f;      // lean added at the run's ground speed (none at the walk's): the GASP run leans ~25 deg alone,
                                // and -6 lifts the head clear of the shoulders in a rear three-quarter view...
    float sprintPitch = 3.f;    // ...and this much more at the sprint's
    float startPitch = 8.f;     // more forward lean while speeding up, with a run's speed still to gain...
    float brakePitch = -4.f;    // ...and lean back while braking, on top of the gait's lean, by how hard it slows: all of it at
    float brakeDecel = 1030.f;  // this deceleration (cm/s^2: the lead's mean stopping a run, 400 to a stand in 0.39 s)...
    float brakeSmooth = 0.1f;   // ...smoothed over this long (s): the GASP stops lurch forward mid-stop, and this holds them back
    float airPitch = 6.f;       // forward tuck in the air, on top of the gait's lean at the speed
    float landPitch = 10.f;     // forward dip into a landing, on top of it too...
    float landDip = 0.3f;       // ...easing out over this long from touchdown (on past a moving body's landHoldMoving)
    float elbowHold = 1.f;      // at a run the elbows bend at least elbowBend, by this weight (none at the walk)
    float elbowBend = 90.f;
    float bodyResponse = 10.f;  // how fast lean, twist and pitch follow (1/s)
    float headLevel = 1.f;      // the head takes back this share of the forward lean...
    float headLevelRoll = 0.6f; // ...and this share of the bank
    float headLead = 0.3f;      // in a turn clip: head yaw past the upper body's per degree still to come
    float maxHeadYaw = 30.f;
    float headResponse = 14.f;  // how fast the head follows (1/s)
    float moveRate = 3.f;       // starts and stops play at most this fast to catch up with the capsule
    float startHandoff = 0.9f;  // a start hands over once its root moves this share of the gait's ground speed
    float startEnd = 0.85f;     // or once it is this far through
    float stopSettle = 0.25f;   // a stop hands over to idle this long before its end
    float stopDelay = 0.05f;    // braking starts a stop once it has lasted this long (a dropped frame of input doesn't)
    float poseVelocity = 0.01f; // feet matching: weight of the feet's velocity (s^2) against their position
    float faceTime = 0.15f;     // steering: time constant of the facing's ease toward the way the body moves
    float turnMin = 30.f;       // heading this far from the facing while running plays a running turn
    float turnSpeed = 0.6f;     // at this share of the run's ground speed or more
    float replaySettle = 0.5f;  // a follower's replayed running turn plays only this long (s) into a steady run...
    float replayWeight = 0.9f;  // ...with the run (or sprint) this much of the blend; else it is dropped
    float refaceMin = 60.f;     // setting off this far from the facing plays a turning start
    float faceMin = 40.f;       // standing, LocoInput::face this far off turns on the spot
    float turnEarly = 0.3f;     // a running turn enters between this long and turnLate before its turn begins
    float turnLate = 0.1f;
    float turnTail = 0.1f;      // and hands back this long after its turn ends (a turn on the spot too)
    float turnRateMin = 0.7f;   // a turn matched to the distance the body comes plays within these rates
    float turnRateMax = 1.4f;
    float minWarp = 0.7f;       // a turn's yaw scales to the angle asked for, within this range
    float maxWarp = 1.3f;
    float limbFade = 0.05f;     // the air pose's ease in and out (95% in three)
    float landArms = 0.5f;      // the arms keep the air pose this long from a landing's touchdown
    float crouchFade = 0.2f;    // moving: the gaits ease between standing and the crouch walk (time constant)
    float crouchSettle = 0.15f; // getting down or up hands over to the idle this long before its end
    float rollMinAir = 0.4f;    // a moving body touching down after this long in the air may roll out of it (landRoll)
};

// A party follower's brake lean and gaits. The party trail brings it to a stand more gently than the lead's
// movement component (485 cm/s^2 on average, about 0.95 s from a run), so all of its lean comes at its own rate;
// and its stop clip leans back less than the lead's on its own, so the lean tops it up by more. Just below where its
// run plays alone (gaitBand) it would hold a few percent of walk under the run, which slips the planted foot
// mid-stance: a wider dead band drops it, and the stride rate (the lead's stride against its own, plus the phase
// trim; up to 3%) keeps step instead.
inline void tuneForFollower(LocoTuning& t) {
    t.brakeDecel = 485.f;
    t.brakePitch = -12.f;
    t.gaitDeadBand = 0.08f;
}

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
    // Steering (degrees, the turnRate's convention): the body's facing, the way it moves (meaningful
    // while groundSpeed is at least moveStop) and the way it heads (while wantSpeed is above 0). face
    // asks a standing body to turn on the spot from yaw to faceYaw (without steer too: a follower).
    bool steer = false;
    float yaw = 0.f;
    float moveYaw = 0.f;
    float wantYaw = 0.f;
    bool face = false;
    float faceYaw = 0.f;
    // The caller moves the body by the turn clips' root motion.
    bool rootMotion = false;
    // A follower: replay a running turn by this many degrees now (0: none).
    float turnBy = 0.f;
    // A follower: the lead's foot phase (0..1) while the lead holds a gait, below 0 otherwise, as this
    // frame begins (before the lead's update, like the follower's own). On the ground loop the follower
    // trims its stride rate to keep a quarter stride from it, so their feet don't land together.
    float leadPhase = -1.f;
    // With it, the lead's stride (cm from one left plant to the next, Locomotion::strideLength): the follower
    // steps at the rate a body with that stride would at its speed, and trims its phase from there. 0: unknown.
    float leadStride = 0.f;
    // Crouch (a follower: as its lead does). Ignored without CrouchIdle and CrouchWalk.
    bool crouch = false;
    // A roll asked for (a press the caller holds on to for a moment): touching down, a moving body rolls out of
    // the landing, as with crouch held (landRoll).
    bool roll = false;
};

struct LocoLayer {
    LocoClip clip = LocoClip::Idle;
    float time = 0.f;
    float weight = 0.f;
};

// A pose laid over the limbs after the layers blend: each arm (upper arm, forearm, hand) and each
// leg (thigh, calf, foot) turned toward the clip's pose by its weight.
struct LocoLimbs {
    LocoClip clip = LocoClip::AirPose;
    float time = 0.f;
    float arms = 0.f;
    float legs = 0.f;
};

class Locomotion {
public:
    static constexpr float kMinWeight = 0.002f; // lighter layers are dropped

    Locomotion() { reset(); }

    FTCORE_API void setClip(LocoClip clip, const LocoClipInfo& info);
    const LocoClipInfo& clip(LocoClip c) const { return info_[index(c)]; }
    LocoTuning& tuning() { return tuning_; }
    const LocoTuning& tuning() const { return tuning_; }

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
    // A follower's stride rate trim this frame (a share: 0.02 steps 2% faster), its stride against the lead's
    // and its phase error together.
    float phaseTrim() const { return trim_; }
    // The gaits' stride at this blend (cm from one left plant to the next; 0 with no gait).
    float strideLength() const { return strideLength_; }
    // How hard the body slows, smoothed (cm/s^2, speeding up below 0): LocoTuning::brakePitch leans back by it.
    float decel() const { return decel_; }
    // The foot phase of a gait clip at `time`.
    FTCORE_API float footPhase(LocoClip c, float time) const;
    // The time in a gait clip at which its foot phase is `phase`.
    FTCORE_API float timeAtPhase(LocoClip c, float phase) const;

    // Steering: the yaw (degrees) to turn the body by this frame, outside a turn. An update with no
    // time passed keeps it, so take it once per frame that time passes.
    float yawStep() const { return yawStep_; }
    // While Turning: the turn clip, the stretch of it played this frame (from, to), the scale on its
    // root's yaw, and how far it has turned the body since it began (degrees, scaled).
    bool turning() const { return state_ == LocoState::Turning; }
    LocoClip turnClip() const { return move_; }
    float turnFrom() const { return turnFrom_; }
    float turnTo() const { return turnTo_; }
    float turnWarp() const { return warp_; }
    bool turnRootDriven() const { return rootDriven_; }
    FTCORE_API float turnYaw() const;
    // How far the turn playing turns the body in all, from where it entered (degrees, scaled), and
    // how many turns have begun so far (a caller sees a new one when this changes).
    FTCORE_API float turnAngle() const;
    std::uint32_t turnsBegun() const { return turnsBegun_; }

    // The body layer over the clips (degrees): bank into turns (roll, positive with a positive
    // turnRate), the upper body's yaw lead into them, and its forward lean with speed.
    float lean() const { return lean_; }
    float twist() const { return twist_; }
    float pitch() const { return pitch_; }
    // The head on top of that (degrees): pitch and roll that keep it level against the lean and the
    // bank, and yaw that looks into a turn ahead of the upper body.
    float headPitch() const { return headPitch_; }
    float headRoll() const { return headRoll_; }
    float headYaw() const { return headYaw_; }
    // The weight (0..1) on holding the elbows bent at least tuning().elbowBend.
    float elbowHold() const { return elbowHold_; }
    const LocoLimbs& limbs() const { return limbs_; }
    // Crouched (asked to, with the clips for it), and how far the gaits have eased into the crouch walk (0..1).
    bool crouched() const { return crouched_; }
    float crouchWeight() const { return crouch_; }
    // A moving body (movingAir or faster) touching down after rollMinAir in the air with crouch held or a roll
    // asked for: the caller rolls out of the landing. True from the update that touches down until the next one
    // that time passes in, so a second pose with none keeps it. The landing goes on beneath it all the same,
    // for a caller with no roll to play.
    bool landRoll() const { return landRoll_; }

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
    // Starts the turn among first..last whose measured turn is nearest `angle` and the same way: with
    // matchFeet, on the clip and from the point before its turn whose feet best match the gaits';
    // otherwise from its start. False when there is none.
    bool beginTurn(const LocoInput& in, LocoClip first, LocoClip last, float angle, bool matchFeet);
    void syncGaits(LocoClip from, float time, float speed);
    void retarget(LocoClip next);
    void advance(float dt, float speed, float leadPhase, float leadStride);
    // Takes up the stance asked for: getting down or up when standing still.
    void changeStance(bool crouch, float speed);
    LocoClip idleClip() const { return crouched_ ? LocoClip::CrouchIdle : LocoClip::Idle; }

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
    float enter_ = -1.f; // the time a start, stop or turn just begun enters at, once it is the target
    float turnEnter_ = 0.f; // the time the turn playing entered at
    float warp_ = 1.f;
    bool rootDriven_ = false; // the turn playing moves the body by its root motion
    float turnFrom_ = 0.f, turnTo_ = 0.f;
    float yawStep_ = 0.f;
    std::uint32_t turnsBegun_ = 0;
    float fade_ = 0.f;
    float phase_ = 0.f; // within the current stride
    float trim_ = 0.f; // a follower's stride rate trim this frame
    float strideLength_ = 0.f;
    float airTime_ = 0.f;
    float landTime_ = -1.f; // since a landing touched down, on through the hand-back to the gaits (below 0: none)
    float groundTime_ = 0.f; // seconds in Ground since a start, a turn or a landing
    float lean_ = 0.f;
    float twist_ = 0.f;
    float pitch_ = 0.f;
    float headPitch_ = 0.f;
    float headRoll_ = 0.f;
    float headYaw_ = 0.f;
    float elbowHold_ = 0.f;
    float decel_ = 0.f; // smoothed deceleration (cm/s^2, speeding up below 0)
    float lastSpeed_ = 0.f;
    LocoLimbs limbs_;
    bool crouched_ = false;
    float crouch_ = 0.f; // the crouch walk's share of the gaits
    bool landRoll_ = false;
};

} // namespace ftcore
