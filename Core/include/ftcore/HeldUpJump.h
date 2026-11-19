#pragma once

// A scripted walk (the dev captures' LeadGo) held up by something it can't walk over, such as a quay's railing or a
// low wall: the jump that tries to get past it, and whether each try cleared it. The jump rise helpers say how high a
// jump takes the feet, from the movement's own values.
//
// Each host frame, tick() gets:
// - walking: the walk is pushing the body on;
// - speed: the body's ground speed (cm/s);
// - distance: its distance to the walk's point (cm);
// - busy: in the air, mid-traversal, climbing or hanging (a try is judged once it is back on its feet);
// - inWater: a try that ends in the water cleared it.
// Held up (walking, under stuckSpeed for stuckTime): pressed() that frame; the host presses jump and keeps its forward
// push on. holdTime later: released(). Then the try is judged (verdict()) once not busy and at least settleTime after
// the release, or at judgeMax after the press whatever the state. It cleared if the body is in the water or came
// minProgress nearer the point since the press. pointReached() ends a try as cleared (the walk got to its point and
// took the next). failed() counts the tries that didn't clear since the last reset(). No heap use.

#include <cstdint>

namespace ftcore {

struct HeldUpJumpRules {
    float stuckSpeed = 15.f;  // cm/s: slower than this is held up...
    float stuckTime = 1.f;    // ...for this long (s) before a try
    float holdTime = 0.25f;   // s the jump is held
    float settleTime = 0.3f;  // s after the release before a try is judged
    float judgeMax = 3.f;     // s after the press: judged then, whatever the state
    float minProgress = 30.f; // cm nearer the point: cleared
};

enum class HeldUpJumpVerdict : std::uint8_t { None, Cleared, NotCleared };

class HeldUpJump {
public:
    void setRules(const HeldUpJumpRules& r) { rules_ = r; }
    const HeldUpJumpRules& rules() const { return rules_; }

    // Forgets the tries, the counts and any try under way (a new walk). A held jump is not released by this: the host
    // releases it if holding() was true.
    void reset()
    {
        const HeldUpJumpRules kept = rules_;
        *this = HeldUpJump{};
        rules_ = kept;
    }

    // One frame. dt <= 0 changes nothing but this frame's events (cleared).
    void tick(float dt, bool walking, float speed, float distance, bool busy, bool inWater)
    {
        pressed_ = false;
        released_ = false;
        verdict_ = HeldUpJumpVerdict::None;
        if (dt <= 0.f) {
            return;
        }
        switch (phase_) {
        case Phase::Walking:
            stuckFor_ = walking && speed < rules_.stuckSpeed ? stuckFor_ + dt : 0.f;
            if (stuckFor_ > rules_.stuckTime) {
                stuckFor_ = 0.f;
                phase_ = Phase::Holding;
                ++tries_;
                pressed_ = true;
                sinceRelease_ = 0.f;
                sincePress_ = 0.f;
                startDistance_ = distance;
            }
            return;
        case Phase::Holding:
            sincePress_ += dt;
            if (sincePress_ >= rules_.holdTime) {
                phase_ = Phase::Judging;
                released_ = true;
            }
            return;
        case Phase::Judging: {
            sincePress_ += dt;
            sinceRelease_ += dt;
            const bool cleared = inWater || startDistance_ - distance >= rules_.minProgress;
            // Past it and down (or in the water): cleared at once. Otherwise judged once settled, or at judgeMax.
            if ((cleared && (inWater || !busy)) || (!busy && sinceRelease_ >= rules_.settleTime) ||
                sincePress_ >= rules_.judgeMax) {
                judge(cleared);
            }
            return;
        }
        }
    }

    // The walk reached its point and took the next: a try under way cleared (released() too when it was held).
    void pointReached()
    {
        if (phase_ == Phase::Walking) {
            stuckFor_ = 0.f;
            return;
        }
        if (phase_ == Phase::Holding) {
            released_ = true;
        }
        judge(true);
    }

    // This frame's events.
    bool pressed() const { return pressed_; }
    bool released() const { return released_; }
    HeldUpJumpVerdict verdict() const { return verdict_; }
    // The jump is held now (between pressed() and released()).
    bool holding() const { return phase_ == Phase::Holding; }
    // A try is under way (pressed, not yet judged).
    bool trying() const { return phase_ != Phase::Walking; }
    int tries() const { return tries_; }
    int failed() const { return failed_; }
    // How much nearer the point the body is than at the last press (cm).
    float progress(float distance) const { return startDistance_ - distance; }

private:
    enum class Phase : std::uint8_t { Walking, Holding, Judging };

    void judge(bool cleared)
    {
        verdict_ = cleared ? HeldUpJumpVerdict::Cleared : HeldUpJumpVerdict::NotCleared;
        if (!cleared) {
            ++failed_;
        }
        phase_ = Phase::Walking;
        stuckFor_ = 0.f;
    }

    HeldUpJumpRules rules_{};
    Phase phase_ = Phase::Walking;
    float stuckFor_ = 0.f;
    float sincePress_ = 0.f;
    float sinceRelease_ = 0.f;
    float startDistance_ = 0.f;
    int tries_ = 0;
    int failed_ = 0;
    bool pressed_ = false;
    bool released_ = false;
    HeldUpJumpVerdict verdict_ = HeldUpJumpVerdict::None;
};

// How high a jump takes the feet (cm): the take-off speed jumpZ (cm/s) is kept for holdTime (s; the movement's jump
// hold, 0 for none), then the body rises under gravity (cm/s^2, positive). 0 for a non-positive speed or gravity.
inline float jumpRise(float jumpZ, float gravity, float holdTime = 0.f)
{
    if (jumpZ <= 0.f || gravity <= 0.f) {
        return 0.f;
    }
    const float hold = holdTime > 0.f ? holdTime : 0.f;
    return jumpZ * hold + jumpZ * jumpZ / (2.f * gravity);
}

// Whether a jump rising `rise` cm takes the feet over a top `height` cm above them, with `margin` cm to spare.
inline bool jumpClears(float rise, float height, float margin = 0.f) { return rise >= height + margin; }

} // namespace ftcore
