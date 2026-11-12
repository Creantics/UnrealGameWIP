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
// A fight step can be lost. judgeStep says what a carried step that came back done leads to; the
// beat's `lose` param picks the rule for a lost fight:
//   "retry" (or no param): the same step opens again; never a resume as a win
//   "branch": the beat ends at once and resume(0) sets its `sets` flag, so the scene can branch on it
//             (a won fight on such a beat resumes with 1). The load refuses a "branch" beat without a fight
//             step or with a `sets` flag that cannot take 0 and 1; judgeStep still plays one without `sets` as "retry"
//   "never":  the fight cannot be lost (a spar): a loss counts as a win
//
// AlphaCounters keeps the tallies of an alpha run (the whole script played by autoplay) across map
// changes and formats the closing line, and the line of its fights by combat system. The lines
// carry counts only.

#include "ftcore/CombatKind.h"
#include "ftcore/Dialogue.h"
#include "ftcore/GameplayPlan.h"
#include "ftcore/StoryState.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

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

// A gameplay beat's `lose` param.
enum class LossRule : std::uint8_t { Retry, Branch, Never };

// "" or "retry", "branch", "never". False (out unchanged) for any other text.
inline bool lossRuleFromName(std::string_view name, LossRule& out) {
    if (name.empty() || name == "retry") out = LossRule::Retry;
    else if (name == "branch") out = LossRule::Branch;
    else if (name == "never") out = LossRule::Never;
    else return false;
    return true;
}

// What a step that came back done leads to.
enum class StepVerdict : std::uint8_t {
    Advance,    // counted; the chain moves on (or the beat resumes after its last step)
    Retry,      // a lost fight: the same step opens again, not counted
    LossBranch, // a lost fight on a "branch" beat: the beat ends with resume(0)
    GiveUp,     // a lost fight past the retry cap (the autopilot): not counted, the beat stays
};

// fight: the step routes to a battle (only fights can be lost). won: the screen's result.
// hasSets: the beat names a `sets` flag. retries: losses of this step already retried.
// retryCap: the most retries before GiveUp; 0 means no cap (a player retries as often as they like).
inline StepVerdict judgeStep(bool fight, bool won, LossRule rule, bool hasSets, std::uint32_t retries, std::uint32_t retryCap) {
    if (!fight || won || rule == LossRule::Never) return StepVerdict::Advance;
    if (rule == LossRule::Branch && hasSets) return StepVerdict::LossBranch;
    if (retryCap != 0 && retries >= retryCap) return StepVerdict::GiveUp;
    return StepVerdict::Retry;
}

// What the step's story writes do (StoryState.h storyEndStep): a win and a loss branch keep them, a retry
// goes back to the step's start and counts a retry, a give-up drops them uncounted.
inline StepEnd stepEndFor(StepVerdict v) {
    switch (v) {
    case StepVerdict::Retry: return StepEnd::Retry;
    case StepVerdict::GiveUp: return StepEnd::Drop;
    case StepVerdict::Advance:
    case StepVerdict::LossBranch: break;
    }
    return StepEnd::Keep;
}

// How the beat goes on after a judged step. Plain: resume(). Won / Lost: resume(1) / resume(0) into the
// beat's `sets` flag (a "branch" beat only). Next: the chain's next step. Retry: the same step again.
// GiveUp: the beat stays (the autopilot skips it).
enum class StepResume : std::uint8_t { Next, Retry, GiveUp, Plain, Won, Lost };
// lastStep: the judged step was the chain's last (or there is no chain).
inline StepResume stepResumeFor(StepVerdict v, bool lastStep, LossRule rule, bool hasSets) {
    switch (v) {
    case StepVerdict::Retry: return StepResume::Retry;
    case StepVerdict::GiveUp: return StepResume::GiveUp;
    case StepVerdict::LossBranch: return StepResume::Lost;
    case StepVerdict::Advance: break;
    }
    if (!lastStep) return StepResume::Next;
    return rule == LossRule::Branch && hasSets ? StepResume::Won : StepResume::Plain;
}

struct AlphaCounters {
    std::uint32_t scenes = 0;
    std::uint32_t battle = 0, field = 0, stub = 0; // screens played to the end; stub includes choice, qte, cutscene, banter and mini
    std::uint32_t stuck = 0;
    std::uint32_t warnings = 0; // plan unknown or dropped tokens, screens that came back not done
    std::uint32_t hops = 0;     // overworld walks between two worlds' field steps
    // The fights played to the end, by the system their encounter names (EncounterTable::combatFor)
    // and by the one that really played (a system with no screen yet plays Turns).
    std::uint32_t tagged[kCombatKinds] = {};
    std::uint32_t played[kCombatKinds] = {};

    void addFight(CombatKind wanted, CombatKind ran) {
        const auto w = static_cast<std::size_t>(wanted), r = static_cast<std::size_t>(ran);
        if (w < kCombatKinds) ++tagged[w];
        if (r < kCombatKinds) ++played[r];
    }
    std::uint32_t fights() const {
        std::uint32_t n = 0;
        for (const std::uint32_t k : played) n += k;
        return n;
    }

    void addScreen(GameplayScreen screen) {
        switch (screenRoutesTo(screen)) { // the screen that was really played
        case GameplayScreen::Battle: ++battle; break;
        case GameplayScreen::Field: ++field; break;
        case GameplayScreen::Stub:
        // The choice, quick-time event, cutscene, banter and mini-game screens count in the stub
        // bucket, so the FINISHED line keeps its fields and totals (docs/CHOICE_SCREEN.md,
        // QTE_SCREEN.md, CUTSCENE_SCREEN.md, BANTER_SCREEN.md, MINI_SCREEN.md).
        case GameplayScreen::Choice:
        case GameplayScreen::Qte:
        case GameplayScreen::Cutscene:
        case GameplayScreen::Banter:
        case GameplayScreen::Mini: ++stub; break;
        case GameplayScreen::Box: break;
        }
    }
    std::uint32_t screens() const { return battle + field + stub; }
    bool clean() const { return stuck == 0 && warnings == 0; }
    // 0 for a clean run, 1 otherwise.
    int exitCode() const { return clean() ? 0 : 1; }

    // "Alpha: FINISHED <S> scenes, <N> screens (<b> battle, <f> field, <s> stub), <k> STUCK, <w> warnings,
    // <h> overworld hops".
    // Returns the length written (the text is cut to fit `capacity`, always null-terminated).
    std::size_t formatFinished(char* buffer, std::size_t capacity) const {
        if (!buffer || capacity == 0) return 0;
        const int n = std::snprintf(buffer, capacity,
                                    "Alpha: FINISHED %u scenes, %u screens (%u battle, %u field, %u stub), %u STUCK, %u warnings, %u overworld hops",
                                    static_cast<unsigned>(scenes), static_cast<unsigned>(screens()),
                                    static_cast<unsigned>(battle), static_cast<unsigned>(field), static_cast<unsigned>(stub),
                                    static_cast<unsigned>(stuck), static_cast<unsigned>(warnings), static_cast<unsigned>(hops));
        if (n < 0) {
            buffer[0] = '\0';
            return 0;
        }
        return static_cast<std::size_t>(n) < capacity ? static_cast<std::size_t>(n) : capacity - 1;
    }

    // "Alpha: FIGHTS <n> (tagged <t> turns, <a> arena, <b> beat; played <t> turns, <a> arena, <b> beat)".
    // Turns is CombatKind::Rhythm. Returns the length written, as formatFinished.
    std::size_t formatFights(char* buffer, std::size_t capacity) const {
        if (!buffer || capacity == 0) return 0;
        const auto at = [](const std::uint32_t (&k)[kCombatKinds], CombatKind c) {
            return static_cast<unsigned>(k[static_cast<std::size_t>(c)]);
        };
        const int n = std::snprintf(buffer, capacity,
                                    "Alpha: FIGHTS %u (tagged %u turns, %u arena, %u beat; played %u turns, %u arena, %u beat)",
                                    static_cast<unsigned>(fights()), at(tagged, CombatKind::Rhythm), at(tagged, CombatKind::Arena),
                                    at(tagged, CombatKind::Beat), at(played, CombatKind::Rhythm), at(played, CombatKind::Arena),
                                    at(played, CombatKind::Beat));
        if (n < 0) {
            buffer[0] = '\0';
            return 0;
        }
        return static_cast<std::size_t>(n) < capacity ? static_cast<std::size_t>(n) : capacity - 1;
    }
};

} // namespace ftcore
