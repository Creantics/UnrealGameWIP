#pragma once

// Sliding along the floor out of a sprint: which clip a body drops into its slide with, how fast it goes as
// the slide wears off, which clip it gets up with, and when it is back on its feet.
//
//   ftcore::SlideSet set;
//   set.setInto(i, ftcore::measureSlideInto(frames, count, length));  // each drop into it, once at load
//   set.setLoop(loopLength);                                         // the slide itself, looping
//   set.setOut(ftcore::SlideOut::Run, ftcore::measureSlideOut(frames, count, length));  // each get-up
//   ftcore::SlideState s = set.start(speed, foot);  // crouch pressed in a sprint
//   if (s.sliding()) every frame: set.step(s, input, dt); play clip s.clip at s.clipTime (looping while s.loop),
//                    turn the body by s.yawStep and move it s.speed along the way it faces;
//                    once it is no longer sliding, hand it back to its locomotion.
//
// An Into runs into the slide and drops down into it; its tail is the slide itself, ending in the Loop's
// first pose, and the Loop goes on from there. measureSlideInto reads when the pelvis is down, the foot the
// body takes off from (the last at rest on the floor before the down) and when it planted (the take-off),
// and the root's speed in from there. An Out gets up from the slide: measureSlideOut reads when a foot first
// plants, when the pelvis is back up, when the body has settled (a get-up to stand or crouch still: the
// pelvis at its end height and the root still), and the root's speed all through it.
//
// start() takes the Into that costs least by entry speed and take-off foot and plays it from its take-off,
// the body keeping its speed and the clip easing from the body's pace to its own by the down (as
// TraversalSet::rate). From the down the slide slows by friction, and by gravity along the slope (so it
// speeds up down a steep one), and the body gets up once it is slower than outSpeed, the crouch is let go
// or it runs into something; any of those before the down waits for it, as the get-ups start from the slide
// pose. The get-up is picked from what the player asks then: sprinting on, it stands into the sprint;
// crouched, a crouch walk or a crouch still; otherwise a walk or a run by the speed asked, or a stand (a
// missing clip falls back to the nearest there is). Its speed eases from the slide's to the clip's own by
// the first plant and is the clip's from there, so the feet hold from the first step. A body left still is
// handed back once it has settled; one that wants to move exitAfter past being up, or once settled if that
// comes first (asked live, so pushing the stick mid get-up hands back then); never past the clip's end.
//
// Space: x forward, y to the left, z up; cm and seconds. Fixed size: nothing allocates.

#include "ftcore/Api.h"
#include "ftcore/Traversal.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ftcore {

struct SlideIntoInfo {
    float length = 0.f;     // seconds; 0: no such clip (or its pelvis does not go down)
    float takeoff = 0.f;    // when the take-off foot plants: its last rest before the down begins
    float down = 0.f;       // when the pelvis is down: within kSlideDownShare of its way down to the slide
    float entrySpeed = 0.f; // the root's forward speed over kEntryTime from the take-off
    float slideSpeed = 0.f; // its forward speed from the down to the end
    int takeoffFoot = -1;   // 0 left, 1 right (-1: none at rest before the down)
};

inline constexpr std::size_t kSlideSpeedSamples = 128;

struct SlideOutInfo {
    float length = 0.f;   // seconds; 0: no such clip (or its pelvis does not come up)
    float plant = 0.f;    // when a foot first plants on the floor it ends on (none: the up)
    float up = 0.f;       // when the pelvis has come kTopShare of the way up to its end height
    float settle = -1.f;  // from when, once up, the pelvis stays within kSettle of its end height and the
                          // root still (-1: never, a get-up into a walk or a run)
    float endSpeed = 0.f; // the root's ground speed over the last kEntryTime
    std::array<float, kSlideSpeedSamples> speed{}; // the root's ground speed, evenly over the clip
};

inline constexpr float kMinSlideDrop = 20.f;   // a pelvis going down (or up) less than this is no slide (no get-up)
inline constexpr float kSlideDownShare = 0.1f; // the pelvis within this share of its way down is down
inline constexpr float kSlideStill = 5.f;      // cm/s: a root slower than this is still

// Measures an Into from `count` frames sampled evenly over it (frame 0 at time 0, the last at `length`; at
// most kMaxTraversalFrames are read), the pelvis included. length stays 0 when the pelvis ends less than
// kMinSlideDrop below where it starts.
FTCORE_API SlideIntoInfo measureSlideInto(const TraversalFrame* frames, std::size_t count, float length);
// Measures an Out the same way. length stays 0 when the pelvis ends less than kMinSlideDrop above where it
// starts.
FTCORE_API SlideOutInfo measureSlideOut(const TraversalFrame* frames, std::size_t count, float length);
// An Out's root ground speed at clip time t, from its samples.
FTCORE_API float slideOutSpeed(const SlideOutInfo& info, float t);

// The get-ups: to stand or crouch still, or into a walk, a run, a sprint or a crouch walk.
enum class SlideOut : std::uint8_t { IdleStand, IdleCrouch, Walk, Run, Sprint, CrouchWalk };
inline constexpr std::size_t kSlideOuts = 6;
inline constexpr bool crouchedOut(SlideOut o) { return o == SlideOut::IdleCrouch || o == SlideOut::CrouchWalk; }

// The clips a slide plays, as SlideState::clip numbers them: the Intos from 0, then the Loop, then the
// Outs in SlideOut's order.
inline constexpr std::size_t kSlideIntos = 4;
inline constexpr int kSlideLoop = static_cast<int>(kSlideIntos);
inline constexpr std::size_t kSlideClips = kSlideIntos + 1 + kSlideOuts;
inline constexpr int slideOutClip(SlideOut o) { return kSlideLoop + 1 + static_cast<int>(o); }

struct SlideTuning {
    float minEntrySpeed = 350.f;   // a body slower than this does not slide
    float friction = 400.f;        // cm/s^2 the slide slows by on the flat...
    float gravity = 980.f;         // ...and this times the slope (speeding up where the slope is below 0)
    float outSpeed = 250.f;        // slower than this, the body gets up
    float maxSpeed = 900.f;        // the fastest a slide speeds up to downhill
    float steerRate = 45.f;        // degrees/s the body turns toward the stick until it gets up
    float exitAfter = 0.3f;        // a body that wants to move is handed back this long after it is up
    float speedCost = 1.f / 200.f; // the cost per cm/s of entry speed between the body and an Into
    float footCost = 0.3f;         // an Into taking off from the other foot
    float minEntryRate = 0.8f;     // the play rate at the entry keeps the body's speed within these
    float maxEntryRate = 1.5f;
};

// What the player asks for and the floor under the body, each frame.
struct SlideInput {
    float slope = 0.f;     // the sine of the floor's rise along the way the body faces (below 0 downhill)
    float wantSpeed = 0.f; // the speed the stick asks for (0: none)
    float turn = 0.f;      // degrees from the way the body faces to the way the stick points
    bool sprint = false;   // sprint held
    bool crouch = true;    // the crouch still on (let go, the body gets up)
    bool blocked = false;  // the body ran into something it can't go on along
};

enum class SlidePhase : std::uint8_t { Into, Down, Out, Done };

struct SlideState {
    SlidePhase phase = SlidePhase::Done;
    int into = -1;            // the Into it went in with
    SlideOut out = SlideOut::IdleStand; // the get-up, from the Out on
    int clip = -1;            // the clip playing, numbered as kSlideLoop and slideOutClip say
    float clipTime = 0.f;
    bool loop = false;        // the clip loops (the Loop)
    float speed = 0.f;        // the body's ground speed
    float yawStep = 0.f;      // degrees the body turns this frame (toward the stick)
    float start = 0.f;        // the Into time it went in at
    float entryRate = 1.f;    // the Into's play rate there
    float outFrom = 0.f;      // the speed it got up at
    bool getUp = false;       // asked to get up before the down: it does at the down
    bool sliding() const { return phase != SlidePhase::Done; }
};

class SlideSet {
public:
    void setTuning(const SlideTuning& t) { tuning_ = t; }
    const SlideTuning& tuning() const { return tuning_; }
    // False when i is out of range.
    FTCORE_API bool setInto(std::size_t i, const SlideIntoInfo& info);
    const SlideIntoInfo& into(std::size_t i) const { return intos_[i]; }
    // The Loop's length (0: none, the Into's last pose holds).
    void setLoop(float length) { loop_ = length; }
    float loopLength() const { return loop_; }
    void setOut(SlideOut o, const SlideOutInfo& info) { outs_[static_cast<std::size_t>(o)] = info; }
    const SlideOutInfo& out(SlideOut o) const { return outs_[static_cast<std::size_t>(o)]; }

    // A slide for a body at ground speed `speed` with `foot` planted (0 left, 1 right, -1 not known): the Into
    // that costs least by entry speed and take-off foot (in index order when they cost the same), from its
    // take-off. Not sliding below minEntrySpeed or without an Into.
    FTCORE_API SlideState start(float speed, int foot) const;
    // Moves a slide on by dt: its clip, speed, turn and phase.
    FTCORE_API void step(SlideState& s, const SlideInput& in, float dt) const;
    // The get-up a slide ending now takes, from what the player asks: the nearest there is.
    FTCORE_API SlideOut chooseOut(const SlideInput& in) const;
    // The Into's play rate at the slide's clip time: its entryRate at the take-off, easing (smoothstep) to 1
    // by the down; 1 from there.
    FTCORE_API float rate(const SlideState& s) const;

private:
    void beginOut(SlideState& s, const SlideInput& in) const;

    SlideTuning tuning_;
    std::array<SlideIntoInfo, kSlideIntos> intos_{};
    float loop_ = 0.f;
    std::array<SlideOutInfo, kSlideOuts> outs_{};
};

} // namespace ftcore
