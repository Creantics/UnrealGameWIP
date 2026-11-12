// A fight step from the runner's side: the loss rule checked at load, the result resumed into the beat's
// `sets` flag, a retry's story writes rolled back, and a reload in the middle of a fight. Invented ids.
#include "ftcore/GameplayPlan.h"
#include "ftcore/StoryChain.h"
#include "ftcore/StoryDatabase.h"
#include "ftcore/StoryRunner.h"

#include <ostream>
#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace ftcore;

namespace {
// One scene: a line, a fight beat with the given params, then a line. The fight is beat 1.
bool build(StoryDatabase& db, std::string& error, std::string_view kind, std::string_view lose, std::string_view sets,
           std::vector<std::string>* warnings = nullptr) {
    StoryBuilder b;
    b.setStart("SC-F");
    b.addFlag("F_WON", FlagValue::boolean(false));
    b.addFlag("F_SCORE", FlagValue::integer(7));
    b.addFlag("F_NAME", b.stringValue("NONE"));
    b.beginChapter("01", "One");
    b.beginScene("SC-F", "Fight");
    b.line("VO-F-001", "SPK1", "Before.");
    b.gameplay(kind, "[GAMEPLAY]");
    if (!lose.empty()) b.param("lose", lose);
    if (!sets.empty()) b.param("sets", sets);
    b.line("VO-F-002", "SPK1", "After.");
    b.end();
    const bool ok = b.finish(db, &error);
    if (warnings) *warnings = b.warnings();
    return ok;
}

// Runs to the fight beat.
void toFight(StoryRunner& r) {
    REQUIRE(r.start());
    REQUIRE(r.advance());
    REQUIRE(r.waitingForGameplay());
}

// What the director does with a judged last step.
bool finishFight(StoryRunner& r, bool won, std::uint32_t retries, std::uint32_t cap, StepResume& out) {
    LossRule rule = LossRule::Retry;
    lossRuleFromName(r.param("lose"), rule);
    const bool sets = !r.param("sets").empty();
    out = stepResumeFor(judgeStep(true, won, rule, sets, retries, cap), true, rule, sets);
    switch (out) {
    case StepResume::Won: return r.resume(FlagValue::integer(1));
    case StepResume::Lost: return r.resume(FlagValue::integer(0));
    case StepResume::Plain: return r.resume();
    case StepResume::Next:
    case StepResume::Retry:
    case StepResume::GiveUp: break;
    }
    return false;
}
} // namespace

TEST_CASE("fight step: the load refuses a branch beat it could not resume") {
    StoryDatabase db;
    std::string error;
    CHECK(build(db, error, "CMB", "branch", "F_WON"));
    CHECK(build(db, error, "CMB", "branch", "F_SCORE")); // an integer takes 0 and 1
    CHECK_FALSE(build(db, error, "CMB", "branch", "F_NAME"));
    CHECK(error.find("string flag") != std::string::npos);
    CHECK_FALSE(build(db, error, "CMB", "branch", ""));
    CHECK(error.find("without a sets flag") != std::string::npos);
    CHECK_FALSE(build(db, error, "PLAY", "branch", "F_WON")); // no fight step to lose
    CHECK(error.find("without a fight step") != std::string::npos);
    CHECK(build(db, error, "CMB", "retry", "F_NAME")); // only a branch resumes with a value
}

TEST_CASE("fight step: every lose=never beat is listed at load") {
    StoryDatabase db;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE(build(db, error, "CMB", "never", "", &warnings));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("1 fight beat(s) marked lose=never") != std::string::npos);
    CHECK(warnings[0].find("scene 'SC-F' beat 1") != std::string::npos);
    REQUIRE(build(db, error, "CMB", "retry", "", &warnings));
    CHECK(warnings.empty());
}

TEST_CASE("fight step: on a branch beat a win resumes with 1 and a loss with 0") {
    StoryDatabase db;
    std::string error;
    for (const char* flag : {"F_WON", "F_SCORE"}) {
        CAPTURE(flag);
        REQUIRE(build(db, error, "CMB", "branch", flag));
        StepResume how{};
        {
            StoryRunner r(&db);
            toFight(r);
            REQUIRE(finishFight(r, true, 0, 0, how));
            CHECK(how == StepResume::Won);
            CHECK(r.flag(flag).value == 1);
            CHECK(r.state() == StoryRunner::State::Line); // went on past the beat
        }
        {
            StoryRunner r(&db);
            toFight(r);
            REQUIRE(finishFight(r, false, 0, 0, how));
            CHECK(how == StepResume::Lost);
            CHECK(r.flag(flag).value == 0);
            CHECK(r.state() == StoryRunner::State::Line);
        }
        {
            StoryRunner r(&db); // the autopilot past its cap still takes the branch
            toFight(r);
            REQUIRE(finishFight(r, false, 9, 2, how));
            CHECK(how == StepResume::Lost);
        }
    }
}

TEST_CASE("fight step: a lost retry fight stays on the beat until it is won, and counts once") {
    StoryDatabase db;
    std::string error;
    REQUIRE(build(db, error, "CMB", "", ""));
    StoryRunner r(&db);
    toFight(r);
    StepResume how{};
    CHECK_FALSE(finishFight(r, false, 0, 0, how));
    CHECK(how == StepResume::Retry);
    CHECK(r.waitingForGameplay()); // never a resume as a win
    CHECK_FALSE(finishFight(r, false, 2, 2, how));
    CHECK(how == StepResume::GiveUp);
    CHECK(r.waitingForGameplay());
    REQUIRE(finishFight(r, true, 1, 0, how));
    CHECK(how == StepResume::Plain);
    CHECK(r.state() == StoryRunner::State::Line);
    CHECK(r.flag("F_WON").value == 0); // a plain resume sets nothing
}

TEST_CASE("fight step: the verdict says what the step's story writes do") {
    CHECK(stepEndFor(StepVerdict::Advance) == StepEnd::Keep);
    CHECK(stepEndFor(StepVerdict::LossBranch) == StepEnd::Keep); // what the story says the loss keeps
    CHECK(stepEndFor(StepVerdict::Retry) == StepEnd::Retry);
    CHECK(stepEndFor(StepVerdict::GiveUp) == StepEnd::Drop);
    CHECK(stepResumeFor(StepVerdict::Advance, false, LossRule::Branch, true) == StepResume::Next);
    CHECK(stepResumeFor(StepVerdict::Advance, true, LossRule::Branch, false) == StepResume::Plain);
    CHECK(stepResumeFor(StepVerdict::Advance, true, LossRule::Never, true) == StepResume::Plain);
}

// The runner's half of a reload: its position comes back on the fight beat. The state's half (the save
// written as of the step's start) is in test_story_state.cpp.
TEST_CASE("fight step: a reload in the middle of a fight puts the runner back on the fight") {
    StoryDatabase db;
    std::string error;
    REQUIRE(build(db, error, "CMB", "branch", "F_WON"));
    StoryRunner r(&db);
    toFight(r);
    const StoryPosition saved = r.position();
    StoryRunner back(&db);
    REQUIRE(back.restore(saved));
    CHECK(back.waitingForGameplay()); // the fight opens again
    CHECK(back.param("lose") == "branch");
    StepResume how{};
    REQUIRE(finishFight(back, true, 0, 0, how));
    CHECK(back.flag("F_WON").value == 1);
}
