#include "ftcore/ReadyStance.h"
#include "TestAssets.h"
#include <ostream> // doctest prints std::string through operator<<
#include <doctest/doctest.h>
#include <string>

using namespace ftcore;

namespace {
const ReadyTurn& turnOf(const ReadyStance& s, ReadyBone b) { return s.bones[static_cast<std::size_t>(b)]; }
} // namespace

TEST_CASE("ready stance: bone names and sides follow the skeleton") {
    CHECK(std::string(readyBoneName(ReadyBone::ClavicleL)) == "clavicle_l");
    CHECK(std::string(readyBoneName(ReadyBone::HandR)) == "hand_r");
    CHECK(std::string(readyBoneName(ReadyBone::ThighL)) == "thigh_l");
    CHECK(std::string(readyBoneName(ReadyBone::FootR)) == "foot_r");
    CHECK(readyBoneSide(ReadyBone::HandL) == 0);
    CHECK(readyBoneSide(ReadyBone::ClavicleR) == 1);
    CHECK(readyBoneSide(ReadyBone::FootL) == 0);
    CHECK(readyBoneSide(ReadyBone::ThighR) == 1);
}

TEST_CASE("ready stance: the defaults raise the hands and bend the knees with the feet flat") {
    const ReadyStanceSet set = readyStanceDefaults();
    REQUIRE(set.stanceCount == 3);
    for (std::size_t i = 0; i < set.stanceCount; ++i) {
        const ReadyStance& s = set.stances[i].stance;
        CAPTURE(set.stances[i].id);
        CHECK(turnOf(s, ReadyBone::UpperArmL).fwd > 20.f);
        CHECK(turnOf(s, ReadyBone::LowerArmL).fwd > 40.f);
        CHECK(turnOf(s, ReadyBone::ThighL).fwd > 0.f);
        // The leg chain adds up to no turn of the foot: the sole stays flat.
        for (ReadyBone thigh : {ReadyBone::ThighL, ReadyBone::ThighR}) {
            const auto t = static_cast<std::size_t>(thigh);
            CHECK(s.bones[t].fwd + s.bones[t + 1].fwd + s.bones[t + 2].fwd == doctest::Approx(0.f));
        }
    }
    // The bladed stance has its lead hand higher than the gun hand.
    const ReadyStance& bladed = set.stances[1].stance;
    CHECK(turnOf(bladed, ReadyBone::UpperArmL).fwd > turnOf(bladed, ReadyBone::UpperArmR).fwd);
    CHECK(bladed.turn > 0.f);
    // The unsure stance has its weight back.
    CHECK(set.stances[2].stance.lean < 0.f);
    CHECK(set.stances[2].stance.pelvisForward < 0.f);
}

TEST_CASE("ready stance: a lead gets its mapped stance, case-insensitive, else the first") {
    ReadyStanceSet set = readyStanceDefaults();
    CHECK(readyStanceFor(set, "anyone") == &set.stances[0].stance);
    std::string error;
    REQUIRE(importReadyStances(R"({"leads": {"alpha": "bladed", "beta": "average"}})", set, &error));
    CHECK(readyStanceFor(set, "Alpha") == &set.stances[1].stance);
    CHECK(readyStanceFor(set, "BETA") == &set.stances[2].stance);
    CHECK(readyStanceFor(set, "gamma") == &set.stances[0].stance);
    CHECK(readyStanceFor(ReadyStanceSet{}, "alpha") == nullptr);
}

TEST_CASE("ready stance: import reads stances over the defaults") {
    ReadyStanceSet set = readyStanceDefaults();
    std::string error;
    REQUIRE(importReadyStances(R"({"version": 1, "about": "x", "stances": {
        "bladed": {"lean": 4, "bones": {"hand_r": {"fwd": -20, "twist": 5}}},
        "other": {"turn": -10, "easeIn": 0.5, "fadeFrom": 1.0, "fadeTo": 3.0}
    }, "leads": {"Alpha": "other"}})",
                               set, &error));
    CHECK(error.empty());
    REQUIRE(set.stanceCount == 2);
    CHECK(set.stances[0].id == "bladed");
    const ReadyStance& b = set.stances[0].stance;
    CHECK(b.lean == doctest::Approx(4.f));
    CHECK(b.turn == doctest::Approx(readyStanceDefaults().stances[1].stance.turn)); // kept from the default
    CHECK(turnOf(b, ReadyBone::HandR).fwd == doctest::Approx(-20.f));
    CHECK(turnOf(b, ReadyBone::HandR).twist == doctest::Approx(5.f));
    CHECK(turnOf(b, ReadyBone::UpperArmL).fwd == doctest::Approx(55.f)); // kept from the default
    const ReadyStance& o = set.stances[1].stance;
    CHECK(o.turn == doctest::Approx(-10.f));
    CHECK(o.easeIn == doctest::Approx(0.5f));
    CHECK(turnOf(o, ReadyBone::UpperArmL).fwd == doctest::Approx(0.f)); // a new stance starts relaxed
    REQUIRE(set.leadCount == 1);
    CHECK(set.leads[0].id == "alpha");
    CHECK(readyStanceFor(set, "alpha") == &o);
}

TEST_CASE("ready stance: import refuses a bad file and leaves the set alone") {
    const char* bad[] = {
        R"([])",
        R"({"stance": {}})",
        R"({"stances": {}})",
        R"({"stances": {"a": {"lean": 31}}})",
        R"({"stances": {"a": {"turn": -46}}})",
        R"({"stances": {"a": {"pelvisForward": 21}}})",
        R"({"stances": {"a": {"easeIn": 0.01}}})",
        R"({"stances": {"a": {"easeOut": 3.5}}})",
        R"({"stances": {"a": {"fadeFrom": 2, "fadeTo": 2}}})",
        R"({"stances": {"a": {"lean": "x"}}})",
        R"({"stances": {"a": {"tilt": 1}}})",
        R"({"stances": {"a": {"bones": {"spine_01": {"fwd": 1}}}}})",
        R"({"stances": {"a": {"bones": {"hand_l": {"roll": 1}}}}})",
        R"({"stances": {"a": {"bones": {"hand_l": {"fwd": 151}}}}})",
        R"({"stances": {"a": {}, "A": {}}})",
        R"({"stances": {"a": {}, "b": {}, "c": {}, "d": {}, "e": {}, "f": {}, "g": {}}})",
        R"({"leads": {"x": "nothing"}})",
        R"({"leads": {"x": 3}})",
        R"({"leads": {"x": "expert", "X": "expert"}})",
    };
    for (const char* json : bad) {
        CAPTURE(json);
        ReadyStanceSet set = readyStanceDefaults();
        std::string error;
        CHECK_FALSE(importReadyStances(json, set, &error));
        CHECK_FALSE(error.empty());
        CHECK(set.stanceCount == 3);
        CHECK(set.leadCount == 0);
    }
    // Dropping a stance a lead still uses, with no new leads.
    ReadyStanceSet set = readyStanceDefaults();
    std::string error;
    REQUIRE(importReadyStances(R"({"leads": {"x": "average"}})", set, &error));
    CHECK_FALSE(importReadyStances(R"({"stances": {"expert": {}}})", set, &error));
    CHECK(error.find("drops") != std::string::npos);
    CHECK(set.stanceCount == 3);
}

TEST_CASE("ready stance: the clock eases in over easeIn and out over easeOut") {
    ReadyStance s;
    s.easeIn = 0.25f;
    s.easeOut = 0.4f;
    float a = 0.f;
    for (int i = 0; i < 4; ++i) a = readyStanceStep(a, true, 0.05f, s);
    CHECK(a == doctest::Approx(0.8f));
    a = readyStanceStep(a, true, 0.05f, s);
    CHECK(a == doctest::Approx(1.f));
    a = readyStanceStep(a, true, 0.5f, s);
    CHECK(a == 1.f);
    a = readyStanceStep(a, false, 0.2f, s);
    CHECK(a == doctest::Approx(0.5f));
    a = readyStanceStep(a, false, 1.f, s);
    CHECK(a == 0.f);
    CHECK(readyStanceStep(0.3f, true, 0.f, s) == 0.3f);
    CHECK(readyStanceStep(0.3f, true, -1.f, s) == 0.3f);
}

TEST_CASE("ready stance: the share is smoothstep of the clock times the speed fade") {
    ReadyStance s;
    s.fadeFrom = 0.6f;
    s.fadeTo = 2.0f;
    CHECK(readyStanceShare(0.f, 0.f, s) == 0.f);
    CHECK(readyStanceShare(1.f, 0.f, s) == doctest::Approx(1.f));
    CHECK(readyStanceShare(0.5f, 0.f, s) == doctest::Approx(0.5f));
    CHECK(readyStanceShare(0.25f, 0.f, s) == doctest::Approx(0.15625f));
    CHECK(readyStanceShare(1.f, 0.6f, s) == doctest::Approx(1.f));
    CHECK(readyStanceShare(1.f, 1.3f, s) == doctest::Approx(0.5f));
    CHECK(readyStanceShare(1.f, 2.0f, s) == 0.f);
    CHECK(readyStanceShare(1.f, -1.3f, s) == doctest::Approx(0.5f));
    CHECK(readyStanceShare(2.f, 0.f, s) == doctest::Approx(1.f));
}

TEST_CASE("ready stance: the shipped file imports and maps the three leads") {
    std::string text;
    if (!ftcore_test::readProjectData("Anim/ready_stance.json", text)) FAIL("Data/Anim/ready_stance.json not found");
    ReadyStanceSet set = readyStanceDefaults();
    std::string error;
    REQUIRE_MESSAGE(importReadyStances(text, set, &error), error);
    CHECK(set.stanceCount == 3);
    CHECK(set.leadCount == 3);
    const ReadyStance* a = readyStanceFor(set, "devin");
    const ReadyStance* b = readyStanceFor(set, "duy");
    const ReadyStance* c = readyStanceFor(set, "third");
    CHECK(a != b);
    CHECK(b != c);
    CHECK(a != c);
}
