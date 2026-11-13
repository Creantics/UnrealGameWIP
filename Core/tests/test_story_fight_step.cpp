// A fight step from the runner's side: the loss rule checked at load, the result resumed into the beat's
// `sets` flag, a retry's story writes rolled back, and a reload in the middle of a fight. Invented ids.
#include "ftcore/GameplayPlan.h"
#include "ftcore/StoryChain.h"
#include "ftcore/StoryDatabase.h"
#include "ftcore/StoryRunner.h"
#include "ftcore/StoryState.h"
#include "ftcore/StoryFactReader.h"

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

TEST_CASE("fight step: the load lists every lose=never beat, and only those") {
    StoryBuilder b;
    b.setStart("SC-N");
    b.beginChapter("01", "One");
    b.beginScene("SC-N", "Never");
    b.gameplay("CMB", "[GAMEPLAY]"); // beat 0
    b.param("lose", "never");
    b.line("VO-N-001", "SPK1", "Between.");
    b.gameplay("CMB", "[GAMEPLAY]"); // beat 2
    b.param("lose", "never");
    b.gameplay("CMB", "[GAMEPLAY]"); // beat 3: a retry beat, not listed
    b.param("lose", "retry");
    b.end();
    StoryDatabase db;
    std::string error;
    REQUIRE(b.finish(db, &error));
    REQUIRE(b.warnings().size() == 1);
    const std::string& w = b.warnings()[0];
    CHECK(w.find("2 fight beat(s) marked lose=never") != std::string::npos);
    CHECK(w.find("scene 'SC-N' beat 0") != std::string::npos);
    CHECK(w.find("scene 'SC-N' beat 2") != std::string::npos);
    CHECK(w.find("beat 3") == std::string::npos);
}

namespace {
// The story state's half of a fight: the press counters the fight writes (side 0 kind 0 is w_a0).
const char* kFlowCsv = "slot,id,kind,max,default,mut,set_scenes,gate,read_scenes,fallback,derive\n"
                       "0,w_a0,CTR,65535,0,-,any,-,-,-,-\n"
                       "1,w_a1,CTR,65535,0,-,any,-,-,-,-\n"
                       "2,w_a2,CTR,65535,0,-,any,-,-,-,-\n"
                       "3,w_b0,CTR,65535,0,-,any,-,-,-,-\n"
                       "4,w_b1,CTR,65535,0,-,any,-,-,-,-\n"
                       "5,w_b2,CTR,65535,0,-,any,-,-,-,-\n";
const char* kFlowJson = R"({"version": 2, "scenes": ["s1"],
  "roles": {"pressCounters": [["w_a0", "w_a1", "w_a2"], ["w_b0", "w_b1", "w_b2"]]}})";

// One attempt at the fight beat as the director plays it: mark the step, the fight presses once, judge it,
// end the step by the verdict, then resume the runner by it.
StepResume attempt(StoryRunner& r, StoryState& st, const FactRegistry& reg, bool won, std::uint32_t retries) {
    StoryStepMark mark;
    REQUIRE(storyMarkStep(st, mark));
    REQUIRE(storyCountPress(st, reg, 0, 0) == SetResult::Ok);
    LossRule rule = LossRule::Retry;
    lossRuleFromName(r.param("lose"), rule);
    const bool sets = !r.param("sets").empty();
    const StepVerdict v = judgeStep(true, won, rule, sets, retries, 0);
    REQUIRE(storyEndStep(st, reg, mark, stepEndFor(v)));
    const StepResume how = stepResumeFor(v, true, rule, sets);
    if (how == StepResume::Won) REQUIRE(r.resume(FlagValue::integer(1)));
    if (how == StepResume::Lost) REQUIRE(r.resume(FlagValue::integer(0)));
    if (how == StepResume::Plain) REQUIRE(r.resume());
    return how;
}
} // namespace

TEST_CASE("fight step: verdict, story state and runner resume together") {
    FactRegistry reg;
    std::string err;
    REQUIRE_MESSAGE(importStoryRegistry(kFlowCsv, kFlowJson, reg, &err), err);
    const std::uint8_t presses = reg.roles.pressCounters[0][0];
    StoryDatabase db;
    std::string error;

    SUBCASE("retry: two losses roll back, the win counts once and goes on") {
        REQUIRE(build(db, error, "CMB", "retry", ""));
        StoryRunner r(&db);
        StoryState st;
        storyReset(st);
        REQUIRE(storyBeginScene(st, reg, storySceneOrdinal(reg, "s1")));
        toFight(r);
        CHECK(attempt(r, st, reg, false, 0) == StepResume::Retry);
        CHECK(r.waitingForGameplay());
        CHECK(attempt(r, st, reg, false, 1) == StepResume::Retry);
        CHECK(storyWide(st, presses) == 0);
        CHECK(attempt(r, st, reg, true, 2) == StepResume::Plain);
        CHECK(storyWide(st, presses) == 1);
        CHECK(r.state() == StoryRunner::State::Line);
    }
    SUBCASE("branch: the loss keeps its writes and resumes with 0") {
        REQUIRE(build(db, error, "CMB", "branch", "F_WON"));
        StoryRunner r(&db);
        StoryState st;
        storyReset(st);
        REQUIRE(storyBeginScene(st, reg, storySceneOrdinal(reg, "s1")));
        toFight(r);
        CHECK(attempt(r, st, reg, false, 0) == StepResume::Lost);
        CHECK(storyWide(st, presses) == 1);
        CHECK(r.flag("F_WON").value == 0);
        CHECK(r.state() == StoryRunner::State::Line);
    }
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
