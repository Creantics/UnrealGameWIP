#include "ftcore/StubScreen.h"

#include "AllocCounter.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

using namespace ftcore;

TEST_CASE("stub titles name the known tokens and pass others through") {
    CHECK(stubTitle("CHC") == "Choice");
    CHECK(stubTitle("BNT") == "Banter");
    CHECK(stubTitle("CG") == "Cutscene");
    CHECK(stubTitle("HE") == "Dialogue loop");
    CHECK(stubTitle("QTE") == "Quick-time event");
    CHECK(stubTitle("co-op QTE") == "Quick-time event");
    CHECK(stubTitle("MINI") == "Mini-game");
    CHECK(stubTitle("chc") == "Choice");
    CHECK(stubTitle("ZZZ") == "ZZZ");
    CHECK(stubTitle("") == "Step");
}

TEST_CASE("stub screen without a step is inert") {
    StubScreen s;
    s.begin(false, true);
    CHECK(s.phase() == StubPhase::NoStep);
    s.tick(5.0);
    CHECK_FALSE(s.press());
    CHECK_FALSE(s.finished());
}

TEST_CASE("stub screen waits for a press after the grace time") {
    StubScreen s;
    s.begin(true, false);
    CHECK(s.waiting());
    CHECK_FALSE(s.press()); // too early
    s.tick(10.0);
    CHECK(s.waiting());     // no autoplay: never finishes by itself
    CHECK(s.press());
    CHECK(s.finished());
    CHECK_FALSE(s.press()); // only once
}

TEST_CASE("stub screen autoplay finishes after a second of game time") {
    StubScreen s;
    s.begin(true, true);
    s.tick(0.5);
    CHECK(s.waiting());
    s.tick(0.49);
    CHECK(s.waiting());
    s.tick(0.02);
    CHECK(s.finished());
    s.begin(true, true); // begin again restarts
    CHECK(s.waiting());
    CHECK(s.elapsed() == 0.0);
}

TEST_CASE("stub screen speed factor makes up the fast dilation the world lacks") {
    CHECK(StubScreen::speedFactor(false, 1.0) == doctest::Approx(1.0));
    CHECK(StubScreen::speedFactor(true, 1.0) == doctest::Approx(8.0));
    CHECK(StubScreen::speedFactor(true, 8.0) == doctest::Approx(1.0));
    CHECK(StubScreen::speedFactor(true, 16.0) == doctest::Approx(1.0));
    CHECK(StubScreen::speedFactor(true, 0.0) >= 1.0);
}

TEST_CASE("stub screen does not allocate") {
    if (!ftcore_test::allocHookInstalled()) {
        return;
    }
    const auto before = ftcore_test::allocStats().count;
    StubScreen s;
    s.begin(true, true);
    for (int i = 0; i < 100; ++i) {
        s.tick(0.02);
    }
    (void)stubTitle("QTE");
    CHECK(ftcore_test::allocStats().count == before);
}
