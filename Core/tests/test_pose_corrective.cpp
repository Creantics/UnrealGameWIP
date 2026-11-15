#include <doctest/doctest.h>

#include "AllocCounter.h"
#include "TestAssets.h"
#include "ftcore/PoseCorrective.h"

#include <cmath>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

bool contains(const std::string& text, const char* word) { return text.find(word) != std::string::npos; }

bool load(const char* json, PoseCorrectiveTable& t, std::string& error, std::vector<std::string>* warnings = nullptr) {
    return importPoseCorrectives(json, t, &error, warnings);
}

Dir3 thighAt(float deg) { // hip flexed `deg` about +x: straight down turns towards +y (forward)
    const float r = deg * 3.14159265f / 180.f;
    return {0.f, std::sin(r), -std::cos(r)};
}

} // namespace

TEST_CASE("pose correctives: Data/Anim/pose_correctives.json loads clean with a crouch morph per side") {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Anim/pose_correctives.json", text));
    PoseCorrectiveTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importPoseCorrectives(text, t, &error, &warnings), error);
    CHECK(warnings.empty());
    REQUIRE(t.entries.size() >= 2);
    bool left = false, right = false;
    for (const PoseCorrective& e : t.entries) {
        CAPTURE(e.morph);
        CHECK(e.startDeg < e.endDeg);
        left = left || (e.side == 'L' && e.bone == "thigh_l" && e.child == "calf_l");
        right = right || (e.side == 'R' && e.bone == "thigh_r" && e.child == "calf_r");
    }
    CHECK(left);
    CHECK(right);
}

TEST_CASE("pose correctives: flexion is the signed angle about the lateral axis, other turns ignored") {
    const Dir3 x{1.f, 0.f, 0.f};
    const Dir3 down = thighAt(0.f);
    CHECK(flexionDegrees(down, down, x) == doctest::Approx(0.f).epsilon(1e-4));
    CHECK(flexionDegrees(down, thighAt(30.f), x) == doctest::Approx(30.f).epsilon(1e-3));
    CHECK(flexionDegrees(down, thighAt(100.f), x) == doctest::Approx(100.f).epsilon(1e-3));
    CHECK(flexionDegrees(down, thighAt(-20.f), x) == doctest::Approx(-20.f).epsilon(1e-3)); // leg behind
    CHECK(flexionDegrees(down, thighAt(30.f), Dir3{-1.f, 0.f, 0.f}) == doctest::Approx(-30.f).epsilon(1e-3));
    // A sideways (abduction) swing has no part about x.
    CHECK(flexionDegrees(down, Dir3{0.5f, 0.f, -0.866f}, x) == doctest::Approx(0.f).epsilon(1e-4));
    // A forward swing with some abduction still reads its forward part; length does not matter.
    CHECK(flexionDegrees(Dir3{0.f, 0.f, -40.f}, Dir3{0.3f, 0.70710678f, -0.70710678f}, x) ==
          doctest::Approx(45.f).epsilon(1e-3));
    // Degenerate input reads 0, never NaN.
    CHECK(flexionDegrees(down, Dir3{1.f, 0.f, 0.f}, x) == 0.f);
    CHECK(flexionDegrees(down, thighAt(30.f), Dir3{}) == 0.f);
}

TEST_CASE("pose correctives: weight is 0 to start, 1 from end, smoothstep and monotonic between") {
    CHECK(correctiveWeight(-10.f, 35.f, 95.f) == 0.f);
    CHECK(correctiveWeight(35.f, 35.f, 95.f) == 0.f);
    CHECK(correctiveWeight(65.f, 35.f, 95.f) == doctest::Approx(0.5f));
    CHECK(correctiveWeight(95.f, 35.f, 95.f) == 1.f);
    CHECK(correctiveWeight(170.f, 35.f, 95.f) == 1.f);
    float prev = 0.f;
    for (int d = 35; d <= 95; ++d) {
        const float w = correctiveWeight(static_cast<float>(d), 35.f, 95.f);
        CHECK(w >= prev);
        prev = w;
    }
    CHECK(correctiveWeight(10.f, 20.f, 20.f) == 0.f); // degenerate range is a step
    CHECK(correctiveWeight(20.f, 20.f, 20.f) == 1.f);
}

TEST_CASE("pose correctives: the per-frame functions allocate nothing") {
    const auto before = ftcore_test::allocStats().count;
    float sum = 0.f;
    for (int i = 0; i < 200; ++i) {
        const float deg = flexionDegrees(thighAt(0.f), thighAt(static_cast<float>(i % 120)), Dir3{1.f, 0.f, 0.f});
        sum += correctiveWeight(deg, 35.f, 95.f);
    }
    CHECK(sum > 0.f);
    CHECK(ftcore_test::allocStats().count == before);
}

TEST_CASE("pose correctives: a good table keeps file order, optional lateral, and warns on unknown keys") {
    PoseCorrectiveTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(load(R"({"version": 1, "about": "x", "extra": 1, "correctives": [
        {"morph": "A_L", "side": "L", "bone": "thigh_l", "child": "calf_l", "start_deg": 30, "end_deg": 90, "about": "n"},
        {"morph": "B", "side": "mid", "bone": "spine_01", "child": "spine_02", "lateral": "-y", "start_deg": -20, "end_deg": 15.5, "odd": 2}]})",
                         t, error, &warnings),
                    error);
    REQUIRE(t.entries.size() == 2);
    CHECK(t.entries[0].morph == "A_L");
    CHECK(t.entries[0].lateral.x == 1.f);
    CHECK(t.entries[1].side == 'M');
    CHECK(t.entries[1].lateral.y == -1.f);
    CHECK(t.entries[1].startDeg == doctest::Approx(-20.f));
    CHECK(t.entries[1].endDeg == doctest::Approx(15.5f));
    REQUIRE(warnings.size() == 2);
    CHECK(contains(warnings[0], "extra"));
    CHECK(contains(warnings[1], "odd"));
}

TEST_CASE("pose correctives: bad tables are refused whole, with the path named") {
    PoseCorrectiveTable t;
    std::string error;
    const char* ok = R"({"version": 1, "correctives": [{"morph": "A", "side": "L", "bone": "b", "child": "c", "start_deg": 1, "end_deg": 2}]})";
    REQUIRE(load(ok, t, error));
    REQUIRE(t.entries.size() == 1);

    CHECK_FALSE(load(R"({"version": 2, "correctives": []})", t, error));
    CHECK(contains(error, "version"));
    CHECK(t.entries.empty());
    CHECK_FALSE(load(R"({"version": 1})", t, error));
    CHECK(contains(error, "correctives"));
    CHECK_FALSE(load("not json", t, error));
    CHECK(contains(error, "json"));
    CHECK_FALSE(load(R"([1])", t, error));
    CHECK(contains(error, "root"));
    CHECK_FALSE(load(R"({"version": 1, "correctives": [{"morph": "A", "side": "up", "bone": "b", "child": "c", "start_deg": 1, "end_deg": 2}]})", t, error));
    CHECK(contains(error, "correctives[0].side"));
    CHECK_FALSE(load(R"({"version": 1, "correctives": [{"morph": "A B", "side": "L", "bone": "b", "child": "c", "start_deg": 1, "end_deg": 2}]})", t, error));
    CHECK(contains(error, "correctives[0].morph"));
    CHECK_FALSE(load(R"({"version": 1, "correctives": [{"morph": "A", "side": "L", "bone": "b", "child": "c", "lateral": "w", "start_deg": 1, "end_deg": 2}]})", t, error));
    CHECK(contains(error, "lateral"));
    CHECK_FALSE(load(R"({"version": 1, "correctives": [{"morph": "A", "side": "L", "bone": "b", "child": "c", "start_deg": 50, "end_deg": 50}]})", t, error));
    CHECK(contains(error, "below"));
    CHECK_FALSE(load(R"({"version": 1, "correctives": [{"morph": "A", "side": "L", "bone": "b", "child": "c", "start_deg": 1, "end_deg": 200}]})", t, error));
    CHECK(contains(error, "end_deg"));
    CHECK_FALSE(load(R"({"version": 1, "correctives": [{"morph": "A", "side": "L", "bone": "Hip", "child": "hip", "start_deg": 1, "end_deg": 2}]})", t, error));
    CHECK(contains(error, "same bone"));
    CHECK_FALSE(load(R"({"version": 1, "correctives": [
        {"morph": "A", "side": "L", "bone": "b", "child": "c", "start_deg": 1, "end_deg": 2},
        {"morph": "a", "side": "R", "bone": "d", "child": "e", "start_deg": 1, "end_deg": 2}]})", t, error));
    CHECK(contains(error, "correctives[1].morph"));
    CHECK(contains(error, "twice"));
    CHECK_FALSE(load(R"({"version": 1, "correctives": [7]})", t, error));
    CHECK(contains(error, "correctives[0]"));
}
