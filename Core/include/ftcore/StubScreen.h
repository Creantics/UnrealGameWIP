#pragma once

// The rules of the stub screen: the placeholder a gameplay step with no real screen yet opens
// (GameplayScreen::Stub). It names the step, waits for a press and continues; with autoplay it
// continues by itself after a second of game time.
//
//   ftcore::StubScreen screen;
//   screen.begin(hasStep, autoPlay);
//   every frame: screen.tick(deltaSeconds * ftcore::StubScreen::speedFactor(fast, worldDilation));
//   on a confirm press: screen.press();
//   when screen.finished(): mark the carry done and open the return map
//
// Plain state, no allocation.

#include "ftcore/Api.h"

#include <cstdint>
#include <string_view>

namespace ftcore {

// The big human title of a gameplay token as written in the script: CHC "Choice", BNT "Banter",
// CG "Cutscene", HE "Heart-to-heart", QTE and `co-op QTE` "Quick-time event", MINI "Mini-game".
// Matching is ASCII case-insensitive. Any other token comes back as given; an empty one gives "Step".
FTCORE_API std::string_view stubTitle(std::string_view token);

enum class StubPhase : std::uint8_t {
    NoStep,  // no story step to show: the screen does nothing
    Waiting, // showing the step
    Done     // finished: continue to the return map
};

class StubScreen {
public:
    // Autoplay continues after this much game time.
    static constexpr double kAutoPlayDelay = 1.0;
    // A press counts only after this much game time, so the press that opened the screen can't skip it.
    static constexpr double kInputGrace = 0.25;
    // The time dilation of an -AlphaFast run.
    static constexpr double kFastDilation = 8.0;

    // How much faster than the world clock the screen's timers should run: the world's own
    // dilation already speeds a fast run up, so this only makes up what it lacks (1 when the
    // world already runs at the fast rate, or when `fast` is off).
    FTCORE_API static double speedFactor(bool fast, double worldDilation);

    // hasStep false: a screen opened without a valid story carry (NoStep, inert).
    FTCORE_API void begin(bool hasStep, bool autoPlay);
    FTCORE_API void tick(double gameSeconds);
    // A confirm press (Space, Enter, pad A). True when it finished the screen.
    FTCORE_API bool press();

    StubPhase phase() const { return phase_; }
    bool waiting() const { return phase_ == StubPhase::Waiting; }
    bool finished() const { return phase_ == StubPhase::Done; }
    bool autoPlay() const { return autoPlay_; }
    double elapsed() const { return elapsed_; }

private:
    StubPhase phase_ = StubPhase::NoStep;
    bool autoPlay_ = false;
    double elapsed_ = 0.0;
};

} // namespace ftcore
