#pragma once

// The feet held on the floor while a strike's clip has the legs, and the pelvis sunk under them.
//
// While the lock holds, each foot on the floor stays where it stood in the world as the body moves over it. One held
// more than holdReach from where the clip has it steps there: one foot at a time, the farther first, lifted on an arc
// over stepTime. Let go, a held foot steps back to the clip's place (at once when within letGoWithin). A foot the clip
// lifts goes where the clip takes it and is held again where it comes down.
//
// The clips stand on nearly straight legs, so a held foot is out of the leg's reach: the host measures how far the
// pelvis must sink and easePelvis eases toward it (in faster than out), never below 0 nor past maxPelvisDrop.
//
// The legs need solving while the lock holds, a foot is held or stepping, or the pelvis is still sunk: legsNeeded.
// reset() drops every hold and the sink at once, for a body put somewhere else (a teleport, a respawn, a death, a
// hand-off); update() does the same itself when the body moves more than teleportJump in one frame.

#include "ftcore/Api.h"

namespace ftcore {

struct FootPoint {
    float x = 0.f, y = 0.f, z = 0.f; // cm, world (z up)
};

struct FootHoldTuning {
    float holdReach = 26.f;     // cm
    float stepTime = 0.16f;     // s
    float stepLift = 6.f;       // cm at the arc's top
    float letGoWithin = 2.f;    // cm: a held foot this near its clip place is let go at once
    float maxPelvisDrop = 9.f;  // cm
    float pelvisDropIn = 18.f;  // 1/s, sinking
    float pelvisDropOut = 8.f;  // 1/s, rising
    float pelvisSnapBelow = 0.05f; // cm: a rising sink under this ends at 0
    float teleportJump = 150.f; // cm moved in one frame: the body was put somewhere else
};

class FTCORE_API FootHold {
public:
    struct Foot {
        bool held = false;
        bool stepping = false;
        FootPoint anchor;  // the ball's place, world
        FootPoint from;    // a step's start, world
        float t = 0.f;     // through the step, 0..1
    };

    explicit FootHold(const FootHoldTuning& tuning = {}) : tuning_(tuning) {}

    // One frame: the balls where the clip has them (world), whether the clip lifts each, the lock asked, the frame's
    // seconds and how far the body moved this frame (cm). Fills `by`: how far each foot is moved from the clip's place
    // (world; z is a step's lift, 0 for a held foot).
    void update(const FootPoint ball[2], const bool lifted[2], bool lock, float dt, float bodyMoved, FootPoint by[2]);

    // Eases the pelvis's sink toward `want` cm (clamped to 0..maxPelvisDrop) and returns it.
    float easePelvis(float want, float dt);

    void reset();

    bool feetActive() const { return feet_[0].held || feet_[0].stepping || feet_[1].held || feet_[1].stepping; }
    bool legsNeeded(bool lock) const { return lock || feetActive() || pelvisDrop_ > 0.f; }
    float pelvisDrop() const { return pelvisDrop_; }
    const Foot& foot(int i) const { return feet_[i & 1]; }
    const FootHoldTuning& tuning() const { return tuning_; }

private:
    FootHoldTuning tuning_;
    Foot feet_[2];
    float pelvisDrop_ = 0.f;
};

} // namespace ftcore
