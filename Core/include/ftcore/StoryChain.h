#pragma once

// The step sequencing of a multi-screen gameplay beat, and the alpha run's counters. Header-only,
// no allocation.
//
//   ftcore::StoryChain chain;
//   chain.begin(plan);                 // the beat's lowered ftcore::GameplayPlan
//   open chain.current().screen        // step chain.step() of chain.count()
//   ... the screen comes back ...
//   switch (chain.finishStep(done)) {
//   case Next::Open:   open chain.current().screen   // the next step
//   case Next::Finish: resume the story past the beat  // every step was done
//   case Next::Fail:   leave the beat where it is      // a screen came back not done
//   }
//
// The chain holds a copy of the plan, so it outlives the string the plan was lowered from and the
// map changes between steps (the owner keeps it on the game instance).
//
// AlphaCounters keeps the tallies of an alpha run (the whole script played by autoplay) across map
// changes and formats the closing line. The line carries counts only.

#include "ftcore/Dialogue.h"
#include "ftcore/GameplayPlan.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace ftcore {

class StoryChain {
public:
    enum class Next : std::uint8_t { Open, Finish, Fail };

    // Starts the chain at step 0. A plan with no step leaves it inactive.
    void begin(const GameplayPlan& plan) {
        plan_ = plan;
        step_ = 0;
        active_ = !plan_.empty();
    }
    // Drops the chain (a failed screen, or the story moved on).
    void reset() {
        active_ = false;
        step_ = 0;
    }

    bool active() const { return active_; }
    std::uint32_t step() const { return step_; }
    std::uint32_t count() const { return plan_.size(); }
    // The step to play now; only meaningful while active().
    const GameplayStep& current() const { return plan_.step(step_); }
    const GameplayPlan& plan() const { return plan_; }

    // The screen of the current step came back. done: Open the next step, or Finish after the last
    // one (the chain ends). Not done, or no active chain: Fail (the chain ends).
    Next finishStep(bool done) {
        if (!active_ || !done) {
            active_ = false;
            return Next::Fail;
        }
        ++step_;
        if (step_ >= plan_.size()) {
            active_ = false;
            return Next::Finish;
        }
        return Next::Open;
    }

private:
    GameplayPlan plan_;
    std::uint32_t step_ = 0;
    bool active_ = false;
};

struct AlphaCounters {
    std::uint32_t scenes = 0;
    std::uint32_t battle = 0, field = 0, stub = 0; // screens played to the end (stub includes choice)
    std::uint32_t stuck = 0;
    std::uint32_t warnings = 0; // plan unknown or dropped tokens, screens that came back not done

    void addScreen(GameplayScreen screen) {
        switch (screenRoutesTo(screen)) { // the screen that was really played
        case GameplayScreen::Battle: ++battle; break;
        case GameplayScreen::Field: ++field; break;
        case GameplayScreen::Stub:
        // The choice screen (the dialogue loop) counts in the stub bucket, so the FINISHED line keeps
        // its fields and totals (docs/CHOICE_SCREEN.md).
        case GameplayScreen::Choice: ++stub; break;
        case GameplayScreen::Box:
        case GameplayScreen::Qte:
        case GameplayScreen::Cutscene: break;
        }
    }
    std::uint32_t screens() const { return battle + field + stub; }
    bool clean() const { return stuck == 0 && warnings == 0; }
    // 0 for a clean run, 1 otherwise.
    int exitCode() const { return clean() ? 0 : 1; }

    // "Alpha: FINISHED <S> scenes, <N> screens (<b> battle, <f> field, <s> stub), <k> STUCK, <w> warnings".
    // Returns the length written (the text is cut to fit `capacity`, always null-terminated).
    std::size_t formatFinished(char* buffer, std::size_t capacity) const {
        if (!buffer || capacity == 0) return 0;
        const int n = std::snprintf(buffer, capacity,
                                    "Alpha: FINISHED %u scenes, %u screens (%u battle, %u field, %u stub), %u STUCK, %u warnings",
                                    static_cast<unsigned>(scenes), static_cast<unsigned>(screens()),
                                    static_cast<unsigned>(battle), static_cast<unsigned>(field), static_cast<unsigned>(stub),
                                    static_cast<unsigned>(stuck), static_cast<unsigned>(warnings));
        if (n < 0) {
            buffer[0] = '\0';
            return 0;
        }
        return static_cast<std::size_t>(n) < capacity ? static_cast<std::size_t>(n) : capacity - 1;
    }
};

} // namespace ftcore
