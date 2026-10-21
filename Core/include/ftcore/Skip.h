#pragma once

// The reading aids: skip (a held key fast-forwards) and auto-advance (the box moves on by itself
// after a delay). Both only produce DialogueCommands for the owner to apply, like the presenter;
// neither allocates.
//
// SkipController, while held, reveals each line or direction at once and moves on every `interval`
// seconds (0.08 by default). It stops at choices, gameplay beats and the end, and passes holds with
// SkipHold (as autoplay does). In SkipMode::Seen it also stops at a line the player has not seen
// (SeenLines), which then types out as usual; it carries on once that line has been read and the
// key is still held. The owner marks beats seen when the player leaves them.
//
//   skip.setHeld(keyDown);
//   apply(skip.update(dt, presenter, seen));    // after presenter.update(dt); None most frames
//
// AutoAdvancer waits `delay` seconds once a line or direction is fully out, then answers Advance.
// It never answers on a choice, a hold or a gameplay beat. Skip takes priority: the owner should
// not run both in one frame. When the line is voiced the owner passes voiceBusy while the clip
// plays: nothing moves until it ends, then the box waits only `voiceTail` (the voice already gave
// the player time to read).

#include "ftcore/Api.h"
#include "ftcore/Dialogue.h"
#include "ftcore/Seen.h"

#include <cstdint>

namespace ftcore {

enum class SkipMode : std::uint8_t { All, Seen };

class SkipController {
public:
    static constexpr float kDefaultInterval = 0.08f;

    void setMode(SkipMode mode) { mode_ = mode; }
    SkipMode mode() const { return mode_; }
    void setInterval(float seconds) { interval_ = seconds < 0.f ? 0.f : seconds; }
    float interval() const { return interval_; }
    // Releasing the key resets the timer.
    void setHeld(bool held) {
        if (!held) timer_ = 0.f;
        held_ = held;
    }
    bool held() const { return held_; }
    // True while the last update() was actually skipping (held, on a skippable beat): what a
    // "SKIP" indicator shows.
    bool skipping() const { return skipping_; }

    // One step of dt seconds. Reveals the text of a skippable beat at once (hence the non-const
    // presenter) and returns the move to make now: Advance, SkipHold or None.
    FTCORE_API DialogueCommand update(float dt, DialoguePresenter& presenter, const SeenLines& seen);

private:
    SkipMode mode_ = SkipMode::Seen;
    float interval_ = kDefaultInterval;
    float timer_ = 0.f;
    bool held_ = false;
    bool skipping_ = false;
};

class AutoAdvancer {
public:
    void setEnabled(bool on) {
        enabled_ = on;
        if (!on) timer_ = 0.f;
    }
    bool enabled() const { return enabled_; }
    void setDelay(float seconds) { delay_ = seconds < 0.f ? 0.f : seconds; }
    float delay() const { return delay_; }

    static constexpr float kDefaultVoiceTail = 0.4f;
    void setVoiceTail(float seconds) { voiceTail_ = seconds < 0.f ? 0.f : seconds; }
    float voiceTail() const { return voiceTail_; }

    // One step of dt seconds: Advance once the delay has passed since the text was fully out, or
    // voiceTail after the line's voice ended. voiceBusy: the line's voice is still playing.
    FTCORE_API DialogueCommand update(float dt, const DialoguePresenter& presenter, bool voiceBusy = false);

private:
    bool enabled_ = false;
    float delay_ = 1.5f;
    float voiceTail_ = kDefaultVoiceTail;
    float timer_ = 0.f;
    bool voiced_ = false; // this beat's voice played
    std::uint32_t epoch_ = kNone;
};

} // namespace ftcore
