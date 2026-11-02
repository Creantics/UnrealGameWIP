#include "ftcore/MoveTuning.h"

#include "TestAssets.h"

#include <doctest/doctest.h>

#include <string>

using namespace ftcore;

TEST_CASE("The shipped Data/Movement/movement.json reads, and every move's section is in it") {
    std::string text;
    if (!ftcore_test::readProjectData("Movement/movement.json", text)) FAIL("Data/Movement/movement.json not found");
    MoveTuning t;
    std::string error;
    CHECK_MESSAGE(importMoveTuning(text, t, &error), error);
    CHECK(text.find("\"slide\"") != std::string::npos);
    CHECK(text.find("\"traversal\"") != std::string::npos);
    CHECK(text.find("\"landing\"") != std::string::npos);
    // The slide's jump cancel is tuned there.
    CHECK(text.find("\"jumpFrom\"") != std::string::npos);
}

TEST_CASE("A member left out keeps its default; one given is read") {
    MoveTuning t;
    std::string error;
    REQUIRE(importMoveTuning(R"({"version": 1, "slide": {"friction": 300, "jumpCarry": 0.9}, "landing": {"rollDrop": 200}})", t, &error));
    CHECK(t.slide.friction == 300.f);
    CHECK(t.slide.jumpCarry == doctest::Approx(0.9f));
    CHECK(t.slide.outSpeed == SlideTuning{}.outSpeed);
    CHECK(t.traversal.rollMinSpeed == TraversalTuning{}.rollMinSpeed);
    CHECK(t.landing.rollDrop == 200.f);
    CHECK(t.landing.rollMinAir == LandingTuning{}.rollMinAir);
    // An empty file of the right version changes nothing.
    MoveTuning same = t;
    REQUIRE(importMoveTuning(R"({"version": 1})", same));
    CHECK(same.slide.friction == 300.f);
}

TEST_CASE("A misspelt tunable, a number out of range, a wrong type or version is refused and changes nothing") {
    MoveTuning t;
    t.slide.friction = 123.f;
    std::string error;
    CHECK_FALSE(importMoveTuning(R"({"version": 1, "slide": {"friction": 300, "frction": 1}})", t, &error));
    CHECK(error == "slide.frction: not a known tunable");
    CHECK(t.slide.friction == 123.f);
    CHECK_FALSE(importMoveTuning(R"({"version": 1, "slide": {"friction": -1}})", t, &error));
    CHECK(error == "slide.friction: out of range");
    CHECK_FALSE(importMoveTuning(R"({"version": 1, "traversal": {"rollMinSpeed": "fast"}})", t, &error));
    CHECK(error == "traversal.rollMinSpeed: not a number");
    CHECK_FALSE(importMoveTuning(R"({"version": 1, "ladder": {}})", t, &error));
    CHECK(error == "ladder: not a known tunable");
    CHECK_FALSE(importMoveTuning(R"({"version": 1, "slide": 3})", t, &error));
    CHECK_FALSE(importMoveTuning(R"({"version": 2})", t, &error));
    CHECK_FALSE(importMoveTuning(R"({"version": 1, "slide": {"minEntryRate": 2, "maxEntryRate": 1}})", t, &error));
    CHECK_FALSE(importMoveTuning("{", t, &error));
    CHECK(t.slide.friction == 123.f);
}
