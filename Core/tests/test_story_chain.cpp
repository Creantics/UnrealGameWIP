#include "ftcore/GameplayPlan.h"
#include "ftcore/StoryChain.h"

#include "AllocCounter.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cstring>

using namespace ftcore;

// Every mode below is invented notation in the script's shapes.

TEST_CASE("chain walks every step and finishes after the last") {
    GameplayPlan plan;
    REQUIRE(plan.lower("ENG → CMB (pair) → CHC"));
    StoryChain chain;
    CHECK_FALSE(chain.active());
    chain.begin(plan);
    REQUIRE(chain.active());
    CHECK(chain.count() == 3);
    CHECK(chain.step() == 0);
    CHECK(chain.current().screen == GameplayScreen::Field);
    CHECK(chain.finishStep(true) == StoryChain::Next::Open);
    CHECK(chain.step() == 1);
    CHECK(chain.current().screen == GameplayScreen::Battle);
    CHECK(chain.current().label() == "pair");
    CHECK(chain.finishStep(true) == StoryChain::Next::Open);
    CHECK(chain.current().screen == GameplayScreen::Choice);
    CHECK(screenRoutesTo(chain.current().screen) == GameplayScreen::Stub);
    CHECK(chain.current().tokenName() == "CHC");
    CHECK(chain.finishStep(true) == StoryChain::Next::Finish);
    CHECK_FALSE(chain.active());
    CHECK(chain.finishStep(true) == StoryChain::Next::Fail); // nothing left to finish
}

TEST_CASE("a single-step plan finishes at once") {
    GameplayPlan plan;
    REQUIRE(plan.lower("CMB"));
    StoryChain chain;
    chain.begin(plan);
    CHECK(chain.count() == 1);
    CHECK(chain.finishStep(true) == StoryChain::Next::Finish);
}

TEST_CASE("a step that comes back not done fails and ends the chain") {
    GameplayPlan plan;
    REQUIRE(plan.lower("CMB, ENG"));
    StoryChain chain;
    chain.begin(plan);
    CHECK(chain.finishStep(false) == StoryChain::Next::Fail);
    CHECK_FALSE(chain.active());
    chain.begin(plan); // a new beat starts over
    CHECK(chain.step() == 0);
    CHECK(chain.active());
    chain.reset();
    CHECK_FALSE(chain.active());
    CHECK(chain.finishStep(true) == StoryChain::Next::Fail);
}

TEST_CASE("an empty plan leaves the chain inactive") {
    GameplayPlan plan;
    StoryChain chain;
    chain.begin(plan);
    CHECK_FALSE(chain.active());
}

TEST_CASE("the chain keeps its own copy of the plan") {
    StoryChain chain;
    {
        GameplayPlan plan;
        REQUIRE(plan.lower("ENG → bogusToken"));
        chain.begin(plan);
    }
    CHECK(chain.plan().unknownTokens() == 1);
    CHECK(chain.finishStep(true) == StoryChain::Next::Open);
    CHECK(chain.current().screen == GameplayScreen::Stub);
}

TEST_CASE("chaining allocates nothing") {
    GameplayPlan plan;
    REQUIRE(plan.lower("ENG → CMB → CHC"));
    StoryChain chain;
    AlphaCounters counters;
    char line[160];
    const auto before = ftcore_test::allocStats().count;
    chain.begin(plan);
    while (chain.active()) {
        counters.addScreen(chain.current().screen);
        chain.finishStep(true);
    }
    counters.formatFinished(line, sizeof line);
    const auto allocs = ftcore_test::allocStats().count - before;
    if (ftcore_test::allocHookInstalled()) CHECK(allocs == 0);
    CHECK(counters.screens() == 3);
}

TEST_CASE("alpha counters and the closing line") {
    AlphaCounters c;
    c.scenes = 73;
    c.addScreen(GameplayScreen::Battle);
    c.addScreen(GameplayScreen::Battle);
    c.addScreen(GameplayScreen::Field);
    c.addScreen(GameplayScreen::Stub);
    c.addScreen(GameplayScreen::Choice); // routed: counts as a stub
    c.addScreen(GameplayScreen::Qte);
    c.addScreen(GameplayScreen::Cutscene);
    c.addScreen(GameplayScreen::Box); // not a screen
    CHECK(c.stub == 4);
    CHECK(c.screens() == 7);
    CHECK(c.clean());
    CHECK(c.exitCode() == 0);

    char line[160];
    const std::size_t n = c.formatFinished(line, sizeof line);
    CHECK(std::strcmp(line, "Alpha: FINISHED 73 scenes, 7 screens (2 battle, 1 field, 4 stub), 0 STUCK, 0 warnings") == 0);
    CHECK(n == std::strlen(line));

    c.warnings = 2;
    CHECK_FALSE(c.clean());
    CHECK(c.exitCode() == 1);
    c.warnings = 0;
    c.stuck = 1;
    CHECK(c.exitCode() == 1);
    c.formatFinished(line, sizeof line);
    CHECK(std::strstr(line, "1 STUCK, 0 warnings") != nullptr);

    char tiny[8];
    CHECK(c.formatFinished(tiny, sizeof tiny) == 7);
    CHECK(tiny[7] == '\0');
    CHECK(c.formatFinished(nullptr, 0) == 0);
}
