#include "ftcore/Cutscene.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

CutsceneShot makeShot(std::string_view spot, std::string_view subject, float duration,
                      ShotTransition transition = ShotTransition::Cut, float blend = 0.f) {
    CutsceneShot s;
    REQUIRE(s.setSpot(spot));
    REQUIRE(s.setSubject(subject));
    s.duration = duration;
    s.transition = transition;
    s.blendTime = blend;
    return s;
}

CutscenePlan threeShots() {
    CutscenePlan p;
    p.add(makeShot("spot_a", "subj_a", 2.f));
    p.add(makeShot("spot_b", "subj_b", 1.f, ShotTransition::Blend, 0.5f));
    p.add(makeShot("spot_c", "subj_a", 3.f));
    return p;
}

float flatDistance(const CutsceneVec3& a, const CutsceneVec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

float distance3(const CutsceneVec3& a, const CutsceneVec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

constexpr const char* kTable = R"J({
  "version": 1,
  "default": { "shots": [
    { "spot": "spot_a", "framing": "wide", "duration": 3, "subject": "subj_a" },
    { "spot": "spot_b", "subject": "subj_b" }
  ] },
  "cutscenes": [
    { "scene": "SC-TEST-01", "beat": 4, "shots": [
      { "spot": "spot_c", "framing": "two_shot", "subject": "subj_a", "subject2": "subj_b", "duration": 1.5,
        "move": "orbit", "transition": "blend" }
    ] },
    { "scene": "SC-TEST-01", "beat": 4, "step": 2, "shots": [
      { "spot": "spot_d", "framing": "insert", "subject": "subj_c", "move": "push_in", "amount": 2 },
      { "spot": "spot_e", "framing": "over_shoulder", "subject": "subj_a", "subject2": "subj_b", "move": "pan",
        "transition": "blend", "blend": 0.25 }
    ] }
  ]
})J";

} // namespace

TEST_CASE("cutscene names") {
    CHECK(shotFramingName(ShotFraming::Wide) == "wide");
    CHECK(shotFramingName(ShotFraming::OverShoulder) == "over_shoulder");
    CHECK(shotFramingName(ShotFraming::TwoShot) == "two_shot");
    CHECK(shotFramingName(ShotFraming::Insert) == "insert");
    CHECK(cameraMoveName(CameraMove::PushIn) == "push_in");
    CHECK(cameraMoveName(CameraMove::Orbit) == "orbit");
    CHECK(shotTransitionName(ShotTransition::Blend) == "blend");
    CHECK(shotTransitionName(ShotTransition::Cut) == "cut");
}

TEST_CASE("cutscene ids are short lower-case tokens") {
    CHECK(isCutsceneId("spot_a"));
    CHECK(isCutsceneId("subj_3"));
    CHECK(isCutsceneId("abcdefghijklmno")); // 15
    CHECK_FALSE(isCutsceneId("abcdefghijklmnop")); // 16
    CHECK_FALSE(isCutsceneId(""));
    CHECK_FALSE(isCutsceneId("Spot_A"));
    CHECK_FALSE(isCutsceneId("spot a"));
    CutsceneShot s;
    CHECK(s.setSpot("spot_a"));
    CHECK(s.spot() == "spot_a");
    CHECK_FALSE(s.setSpot("BAD"));
    CHECK(s.spot() == "spot_a"); // unchanged
    CHECK_FALSE(s.hasSubject2());
    CHECK(s.setSubject2("subj_b"));
    CHECK(s.hasSubject2());
    CHECK(s.setSubject2(""));
    CHECK_FALSE(s.hasSubject2());
    CHECK_FALSE(s.setSubject(""));
}

TEST_CASE("a cutscene plan holds a fixed number of shots and tidies their times") {
    CutscenePlan p;
    CHECK(p.empty());
    for (std::uint32_t i = 0; i < CutscenePlan::kMaxShots; ++i) CHECK(p.add(makeShot("spot_a", "subj_a", 1.f)));
    CHECK_FALSE(p.add(makeShot("spot_a", "subj_a", 1.f)));
    CHECK(p.size() == CutscenePlan::kMaxShots);
    CHECK(p.totalDuration() == doctest::Approx(16.0));

    p.clear();
    p.add(makeShot("spot_a", "subj_a", 0.f));                             // raised to the minimum
    p.add(makeShot("spot_a", "subj_a", 1.f, ShotTransition::Cut, 0.7f));  // a cut never blends
    p.add(makeShot("spot_a", "subj_a", 1.f, ShotTransition::Blend, 5.f)); // blend kept within the shot
    CHECK(p.shot(0).duration == doctest::Approx(CutsceneShot::kMinDuration));
    CHECK(p.shot(1).blendTime == doctest::Approx(0.0));
    CHECK(p.shot(2).blendTime == doctest::Approx(1.0));
}

TEST_CASE("the cutscene player advances shots on the clock") {
    CutscenePlayer player;
    CHECK(player.phase() == CutscenePhase::Idle);
    player.begin(threeShots());
    REQUIRE(player.playing());
    CHECK(player.shotCount() == 3);
    CHECK(player.currentIndex() == 0);
    CHECK(player.currentShot().spot() == "spot_a");
    CHECK(player.shotProgress() == doctest::Approx(0.0));

    player.update(1.0);
    CHECK(player.currentIndex() == 0);
    CHECK(player.shotProgress() == doctest::Approx(0.5));
    player.update(-1.0); // ignored
    player.update(1.25);
    CHECK(player.currentIndex() == 1);
    CHECK(player.shotProgress() == doctest::Approx(0.25));
    player.update(4.0); // past the end in one step
    CHECK(player.finished());
    CHECK_FALSE(player.skipped());
    CHECK(player.currentIndex() == 2);
    CHECK(player.shotProgress() == doctest::Approx(1.0));
    CHECK(player.elapsed() == doctest::Approx(6.25));
    CHECK_FALSE(player.skip()); // already done
}

TEST_CASE("one long update can cross several shots") {
    CutscenePlayer player;
    player.begin(threeShots());
    player.update(3.5); // 2 + 1, then 0.5 into the third
    CHECK(player.currentIndex() == 2);
    CHECK(player.shotTime() == doctest::Approx(0.5));
    CHECK(player.playing());
}

TEST_CASE("an empty plan is done at once") {
    CutscenePlayer player;
    player.begin(CutscenePlan{});
    CHECK(player.finished());
    CHECK_FALSE(player.skipped());
    CHECK(player.shotProgress() == doctest::Approx(1.0));
}

TEST_CASE("skip presses wait out the input grace; skip() does not") {
    CutscenePlayer player;
    player.begin(threeShots());
    CHECK_FALSE(player.press()); // the press that opened the screen
    player.tickReal(0.2);
    CHECK_FALSE(player.press());
    player.tickReal(0.1);
    CHECK(player.press());
    CHECK(player.finished());
    CHECK(player.skipped());

    player.begin(threeShots());
    CHECK_FALSE(player.skipped()); // begin resets
    CHECK(player.skip());
    CHECK(player.skipped());
}

TEST_CASE("autoplay skips after a second of real time, whatever the game clock does") {
    CutscenePlayer player;
    player.begin(threeShots(), true);
    CHECK(player.autoPlay());
    player.update(0.1);
    player.tickReal(0.6);
    CHECK(player.playing());
    player.tickReal(0.39);
    CHECK(player.playing());
    player.tickReal(0.02);
    CHECK(player.finished());
    CHECK(player.skipped());

    // Without autoplay real time alone never ends it.
    player.begin(threeShots(), false);
    player.tickReal(100.0);
    CHECK(player.playing());

    // An autoplay cutscene shorter than the cap ends by itself, not skipped.
    CutscenePlan shortPlan;
    shortPlan.add(makeShot("spot_a", "subj_a", 0.5f));
    player.begin(shortPlan, true);
    player.update(0.6);
    CHECK(player.finished());
    CHECK_FALSE(player.skipped());
}

TEST_CASE("blend shots blend in from the previous shot over their blend time") {
    CutscenePlayer player;
    player.begin(threeShots());
    CHECK_FALSE(player.blending()); // the first shot never blends
    CHECK(player.blendWeight() == doctest::Approx(1.0));
    player.update(2.0); // the start of shot 1 (blend 0.5)
    REQUIRE(player.currentIndex() == 1);
    CHECK(player.blending());
    CHECK(player.previousShot().spot() == "spot_a");
    CHECK(player.blendWeight() == doctest::Approx(0.0));
    player.update(0.25);
    CHECK(player.blendWeight() == doctest::Approx(0.5)); // eased, symmetric at the middle
    player.update(0.125);
    CHECK(player.blendWeight() > 0.75f);
    CHECK(player.blendWeight() < 1.0f);
    player.update(0.2);
    CHECK_FALSE(player.blending());
    CHECK(player.blendWeight() == doctest::Approx(1.0));
    player.update(0.5); // shot 2 is a cut
    REQUIRE(player.currentIndex() == 2);
    CHECK_FALSE(player.blending());
}

TEST_CASE("framings place the camera from the spot and subject") {
    const CutsceneVec3 subject{0.f, 0.f, 0.f};
    const CutsceneVec3 spot{10.f, 0.f, 2.f};

    CutsceneShot s = makeShot("spot_a", "subj_a", 2.f);
    s.framing = ShotFraming::Wide;
    CameraPose wide = cutsceneCamera(s, 0.f, spot, subject, nullptr);
    CHECK(wide.position.x == doctest::Approx(10.0));
    CHECK(wide.position.z == doctest::Approx(2.0));
    CHECK(wide.lookAt.z == doctest::Approx(framingRule(ShotFraming::Wide).lookHeight));

    s.framing = ShotFraming::Medium;
    CameraPose medium = cutsceneCamera(s, 0.f, spot, subject, nullptr);
    CHECK(flatDistance(medium.position, subject) == doctest::Approx(framingRule(ShotFraming::Medium).distance));
    CHECK(medium.position.x > 0.f); // on the spot's side
    CHECK(medium.position.y == doctest::Approx(0.0));

    s.framing = ShotFraming::Close;
    CameraPose close = cutsceneCamera(s, 0.f, spot, subject, nullptr);
    CHECK(flatDistance(close.position, subject) < flatDistance(medium.position, subject));
    CHECK(close.lookAt.z > medium.lookAt.z); // the face, not the chest

    s.framing = ShotFraming::Insert;
    CameraPose insert = cutsceneCamera(s, 0.f, spot, subject, nullptr);
    CHECK(flatDistance(insert.position, subject) < flatDistance(close.position, subject));
    CHECK(insert.position.z > insert.lookAt.z); // looks down at the detail

    // Seen from another side: the spot picks the direction.
    const CutsceneVec3 northSpot{0.f, -6.f, 0.f};
    CameraPose north = cutsceneCamera(s, 0.f, northSpot, subject, nullptr);
    CHECK(north.position.y < 0.f);
    CHECK(north.position.x == doctest::Approx(0.0));
}

TEST_CASE("a spot right on the subject still gives a usable camera") {
    const CutsceneVec3 subject{1.f, 1.f, 0.f};
    CutsceneShot s = makeShot("spot_a", "subj_a", 2.f);
    s.framing = ShotFraming::Medium;
    CameraPose p = cutsceneCamera(s, 0.f, subject, subject, nullptr);
    CHECK(distance3(p.position, p.lookAt) > 1.f);
    s.framing = ShotFraming::Wide;
    const CutsceneVec3 head{1.f, 1.f, framingRule(ShotFraming::Wide).lookHeight};
    p = cutsceneCamera(s, 0.f, head, subject, nullptr);
    CHECK(distance3(p.position, p.lookAt) > 1.f);
}

TEST_CASE("two-shots hold both subjects and over-the-shoulder stands behind the second") {
    const CutsceneVec3 a{0.f, -2.f, 0.f};
    const CutsceneVec3 b{0.f, 2.f, 0.f};
    const CutsceneVec3 spot{8.f, 0.f, 1.5f};
    CutsceneShot s = makeShot("spot_a", "subj_a", 2.f);
    s.framing = ShotFraming::TwoShot;
    REQUIRE(s.setSubject2("subj_b"));
    CameraPose two = cutsceneCamera(s, 0.f, spot, a, &b);
    CHECK(two.lookAt.x == doctest::Approx(0.0));
    CHECK(two.lookAt.y == doctest::Approx(0.0));
    CHECK(two.position.x >= 4.4f - 1e-3f); // 4 m apart: at least 4 * 1.1 + 1.5 back
    CHECK(two.position.y == doctest::Approx(0.0));

    // Without the second subject a two-shot falls back to medium on the first.
    CameraPose solo = cutsceneCamera(s, 0.f, spot, a, nullptr);
    CHECK(solo.lookAt.y == doctest::Approx(-2.0));

    s.framing = ShotFraming::OverShoulder;
    CameraPose ots = cutsceneCamera(s, 0.f, spot, a, &b);
    CHECK(ots.lookAt.y == doctest::Approx(-2.0)); // looks at the first subject
    CHECK(ots.position.y > b.y);                  // from behind the second one
    CHECK(ots.position.x > 0.f);                  // on the spot's shoulder
    CHECK(flatDistance(ots.position, b) < 1.5f);
    const CutsceneVec3 otherSpot{-8.f, 0.f, 1.5f};
    CameraPose other = cutsceneCamera(s, 0.f, otherSpot, a, &b);
    CHECK(other.position.x < 0.f); // the other shoulder
}

TEST_CASE("camera moves follow the shot's progress") {
    const CutsceneVec3 subject{0.f, 0.f, 0.f};
    const CutsceneVec3 spot{10.f, 0.f, 0.f};
    CutsceneShot s = makeShot("spot_a", "subj_a", 2.f);
    s.framing = ShotFraming::Medium;
    const CameraPose base = cutsceneCamera(s, 0.f, spot, subject, nullptr);
    const float baseDist = distance3(base.position, base.lookAt);

    s.move = CameraMove::PushIn;
    s.moveAmount = 1.f;
    CHECK(distance3(cutsceneCamera(s, 0.f, spot, subject, nullptr).position, base.position) == doctest::Approx(0.0));
    CameraPose in = cutsceneCamera(s, 1.f, spot, subject, nullptr);
    CHECK(distance3(in.position, in.lookAt) == doctest::Approx(baseDist - 1.f));
    CameraPose half = cutsceneCamera(s, 0.5f, spot, subject, nullptr);
    CHECK(distance3(half.position, half.lookAt) == doctest::Approx(baseDist - 0.5f));
    s.moveAmount = 100.f; // stops short of the subject
    in = cutsceneCamera(s, 1.f, spot, subject, nullptr);
    CHECK(distance3(in.position, in.lookAt) == doctest::Approx(0.3));

    s.move = CameraMove::PullOut;
    s.moveAmount = 2.f;
    CameraPose out = cutsceneCamera(s, 1.f, spot, subject, nullptr);
    CHECK(distance3(out.position, out.lookAt) == doctest::Approx(baseDist + 2.f));

    s.move = CameraMove::Orbit;
    s.moveAmount = 90.f;
    CameraPose orbit = cutsceneCamera(s, 1.f, spot, subject, nullptr);
    CHECK(distance3(orbit.position, orbit.lookAt) == doctest::Approx(baseDist));
    CHECK(orbit.position.x == doctest::Approx(0.0).epsilon(0.001));
    CHECK(std::fabs(orbit.position.y) == doctest::Approx(framingRule(ShotFraming::Medium).distance));

    s.move = CameraMove::Pan;
    s.moveAmount = 40.f;
    CameraPose start = cutsceneCamera(s, 0.f, spot, subject, nullptr);
    CameraPose mid = cutsceneCamera(s, 0.5f, spot, subject, nullptr);
    CameraPose end = cutsceneCamera(s, 1.f, spot, subject, nullptr);
    CHECK(distance3(mid.position, base.position) == doctest::Approx(0.0)); // the camera stays put
    CHECK(distance3(mid.lookAt, base.lookAt) == doctest::Approx(0.0).epsilon(0.001)); // centred mid-shot
    CHECK(start.lookAt.y * end.lookAt.y < 0.f); // sweeps across
}

TEST_CASE("lerpPose blends both points") {
    CameraPose a;
    a.position = {0.f, 0.f, 0.f};
    a.lookAt = {1.f, 0.f, 0.f};
    CameraPose b;
    b.position = {10.f, 0.f, 2.f};
    b.lookAt = {1.f, 4.f, 0.f};
    const CameraPose m = lerpPose(a, b, 0.5f);
    CHECK(m.position.x == doctest::Approx(5.0));
    CHECK(m.position.z == doctest::Approx(1.0));
    CHECK(m.lookAt.y == doctest::Approx(2.0));
    CHECK(lerpPose(a, b, 2.f).position.x == doctest::Approx(10.0));
    CHECK(lerpPose(a, b, -1.f).position.x == doctest::Approx(0.0));
}

TEST_CASE("cutscene table loads and finds by scene, beat and step") {
    CutsceneTable table;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importCutsceneTable(kTable, table, &err, &warnings), err);
    CHECK(warnings.empty());
    REQUIRE(table.fallback.size() == 2);
    CHECK(table.fallback.shot(0).framing == ShotFraming::Wide);
    CHECK(table.fallback.shot(0).duration == doctest::Approx(3.0));
    CHECK(table.fallback.shot(1).framing == ShotFraming::Medium); // defaults
    CHECK(table.fallback.shot(1).duration == doctest::Approx(2.0));
    CHECK(table.fallback.shot(1).move == CameraMove::Static);
    CHECK(table.fallback.shot(1).transition == ShotTransition::Cut);
    REQUIRE(table.entries.size() == 2);

    const CutscenePlan& step0 = table.find("SC-TEST-01", 4, 0);
    REQUIRE(step0.size() == 1);
    CHECK(step0.shot(0).framing == ShotFraming::TwoShot);
    CHECK(step0.shot(0).subject2() == "subj_b");
    CHECK(step0.shot(0).move == CameraMove::Orbit);
    CHECK(step0.shot(0).moveAmount == doctest::Approx(30.0)); // orbit default
    CHECK(step0.shot(0).transition == ShotTransition::Blend);
    CHECK(step0.shot(0).blendTime == doctest::Approx(0.5)); // blend default

    const CutscenePlan& step2 = table.find("SC-TEST-01", 4, 2);
    REQUIRE(step2.size() == 2);
    CHECK(step2.shot(0).framing == ShotFraming::Insert);
    CHECK(step2.shot(0).moveAmount == doctest::Approx(2.0));
    CHECK(step2.shot(1).move == CameraMove::Pan);
    CHECK(step2.shot(1).moveAmount == doctest::Approx(20.0)); // pan default
    CHECK(step2.shot(1).blendTime == doctest::Approx(0.25));

    CHECK(&table.find("SC-TEST-01", 4, 1) == &table.fallback);
    CHECK(&table.find("SC-TEST-02", 4, 0) == &table.fallback);
    CHECK(table.findEntry("SC-TEST-01", 5, 0) == nullptr);
}

TEST_CASE("cutscene table errors name the problem") {
    const auto fails = [](const char* json, const char* expected) {
        CutsceneTable table;
        std::string err;
        CHECK_FALSE(importCutsceneTable(json, table, &err));
        CHECK_MESSAGE(err.find(expected) != std::string::npos, err);
        CHECK(table.fallback.empty());
        CHECK(table.entries.empty());
    };
    fails("{", "cutscenes file");
    fails("[]", "expected an object");
    fails(R"({"version": 2, "default": {"shots": [{"spot": "spot_a", "subject": "subj_a"}]}})", "version");
    fails(R"({"cutscenes": []})", "default: expected an object");
    fails(R"({"default": {"shots": []}})", "default.shots: expected a non-empty array");
    fails(R"({"default": {"shots": [{"subject": "subj_a"}]}})", "default.shots[0].spot: expected an id");
    fails(R"({"default": {"shots": [{"spot": "Spot A", "subject": "subj_a"}]}})", "default.shots[0].spot");
    fails(R"({"default": {"shots": [{"spot": "spot_a"}]}})", "default.shots[0].subject");
    fails(R"({"default": {"shots": [{"spot": "spot_a", "subject": "subj_a", "framing": "dutch"}]}})", ".framing");
    fails(R"({"default": {"shots": [{"spot": "spot_a", "subject": "subj_a", "framing": "two_shot"}]}})", "needs a second subject");
    fails(R"({"default": {"shots": [{"spot": "spot_a", "subject": "subj_a", "duration": 0}]}})", ".duration");
    fails(R"({"default": {"shots": [{"spot": "spot_a", "subject": "subj_a", "move": "zoom"}]}})", ".move");
    fails(R"({"default": {"shots": [{"spot": "spot_a", "subject": "subj_a", "move": "push_in", "amount": -1}]}})", ".amount");
    fails(R"({"default": {"shots": [{"spot": "spot_a", "subject": "subj_a", "transition": "wipe"}]}})", ".transition");
    fails(R"({"default": {"shots": [{"spot": "spot_a", "subject": "subj_a", "duration": 1, "transition": "blend", "blend": 2}]}})",
          ".blend");
    fails(R"({"default": {"shots": [{"spot": "spot_a", "subject": "subj_a"}]},
              "cutscenes": [{"beat": 1, "shots": [{"spot": "spot_a", "subject": "subj_a"}]}]})", "cutscenes[0].scene");
    fails(R"({"default": {"shots": [{"spot": "spot_a", "subject": "subj_a"}]},
              "cutscenes": [{"scene": "SC-TEST-01", "shots": [{"spot": "spot_a", "subject": "subj_a"}]}]})", "cutscenes[0].beat");
    fails(R"({"default": {"shots": [{"spot": "spot_a", "subject": "subj_a"}]},
              "cutscenes": [{"scene": "SC-TEST-01", "beat": 1, "shots": [{"spot": "spot_a", "subject": "subj_a"}]},
                            {"scene": "SC-TEST-01", "beat": 1, "step": 0, "shots": [{"spot": "spot_b", "subject": "subj_a"}]}]})",
          "duplicate entry for SC-TEST-01 beat 1 step 0");

    std::string many = R"({"default": {"shots": [)";
    for (std::uint32_t i = 0; i <= CutscenePlan::kMaxShots; ++i) {
        if (i) many += ",";
        many += R"({"spot": "spot_a", "subject": "subj_a"})";
    }
    many += "]}}";
    fails(many.c_str(), "more than 16 shots");
}

TEST_CASE("cutscene table unknown keys are warnings") {
    CutsceneTable table;
    std::vector<std::string> warnings;
    REQUIRE(importCutsceneTable(R"({"default": {"shots": [{"spot": "spot_a", "subject": "subj_a", "lens": 35}]}, "extra": 1})",
                                table, nullptr, &warnings));
    CHECK(warnings.size() == 2);
}

TEST_CASE("the committed cutscenes.json loads cleanly") {
    std::string text;
    if (!ftcore_test::readProjectData("Cutscene/cutscenes.json", text)) {
        FAIL("Data/Cutscene/cutscenes.json not found");
        return;
    }
    CutsceneTable table;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importCutsceneTable(text, table, &err, &warnings), err);
    CHECK(warnings.empty());
    CHECK_FALSE(table.fallback.empty());
    // The blockout map has spot_a..spot_f and subj_a..subj_c; the data names no others.
    const auto known = [](std::string_view id, char first, char last, std::string_view prefix) {
        return id.size() == prefix.size() + 1 && id.substr(0, prefix.size()) == prefix && id.back() >= first &&
               id.back() <= last;
    };
    const auto checkPlan = [&](const CutscenePlan& plan) {
        for (std::uint32_t i = 0; i < plan.size(); ++i) {
            const CutsceneShot& s = plan.shot(i);
            CHECK(known(s.spot(), 'a', 'f', "spot_"));
            CHECK(known(s.subject(), 'a', 'c', "subj_"));
            if (s.hasSubject2()) {
                CHECK(known(s.subject2(), 'a', 'c', "subj_"));
            }
        }
    };
    checkPlan(table.fallback);
    for (const CutsceneEntry& e : table.entries) checkPlan(e.plan);
}

TEST_CASE("playing a cutscene does not allocate") {
    if (!ftcore_test::allocHookInstalled()) {
        return;
    }
    const CutscenePlan plan = threeShots();
    const CutsceneVec3 spot{5.f, 1.f, 1.f};
    const CutsceneVec3 a{0.f, 0.f, 0.f};
    const CutsceneVec3 b{0.f, 2.f, 0.f};
    const auto before = ftcore_test::allocStats().count;
    CutscenePlayer player;
    player.begin(plan, true);
    float sink = 0.f;
    for (int i = 0; i < 200 && !player.finished(); ++i) {
        player.update(0.02);
        player.tickReal(0.004);
        CameraPose pose = cutsceneCamera(player.currentShot(), player.shotProgress(), spot, a, &b);
        if (player.blending()) {
            const CameraPose from = cutsceneCamera(player.previousShot(), 1.f, spot, a, &b);
            pose = lerpPose(from, pose, player.blendWeight());
        }
        sink += pose.position.x;
    }
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(sink != 0.f);
}
