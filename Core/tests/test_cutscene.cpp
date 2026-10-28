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

TEST_CASE("a cutscene plan keeps its in-place blend times in range") {
    CutscenePlan p;
    CHECK(p.blendIn() == doctest::Approx(CutscenePlan::kDefaultBlend));
    CHECK(p.blendOut() == doctest::Approx(CutscenePlan::kDefaultBlend));
    p.setBlends(-1.f, 99.f);
    CHECK(p.blendIn() == doctest::Approx(0.0));
    CHECK(p.blendOut() == doctest::Approx(CutscenePlan::kMaxBlend));
    p.setBlends(std::nanf(""), 1.5f);
    CHECK(p.blendIn() == doctest::Approx(0.0));
    CHECK(p.blendOut() == doctest::Approx(1.5));
    p.clear();
    CHECK(p.blendIn() == doctest::Approx(CutscenePlan::kDefaultBlend));
    CHECK(p.blendOut() == doctest::Approx(CutscenePlan::kDefaultBlend));
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

TEST_CASE("in place, the player eases in, plays the shots and eases out") {
    CutscenePlan plan = threeShots(); // 2 + 1 + 3 s
    plan.setBlends(0.5f, 0.25f);
    CutscenePlayer player;
    player.begin(plan, false, true);
    CHECK(player.inPlace());
    REQUIRE(player.blendingIn());
    CHECK(player.active());
    CHECK_FALSE(player.playing());
    CHECK(player.blendInTime() == doctest::Approx(0.5));
    CHECK(player.blendOutTime() == doctest::Approx(0.25));
    CHECK(player.currentIndex() == 0);
    CHECK(player.blendInWeight() == doctest::Approx(0.0));
    CHECK(player.presence() == doctest::Approx(0.0));

    player.update(0.25);
    CHECK(player.blendingIn());
    CHECK(player.blendInWeight() == doctest::Approx(0.5)); // eased, symmetric at the middle
    CHECK(player.presence() == doctest::Approx(0.5));
    CHECK(player.shotTime() == doctest::Approx(0.0)); // the shot clock holds still
    CHECK(player.shotProgress() == doctest::Approx(0.0));

    player.update(0.375); // 0.125 past the blend in
    REQUIRE(player.playing());
    CHECK(player.blendInWeight() == doctest::Approx(1.0));
    CHECK(player.presence() == doctest::Approx(1.0));
    CHECK(player.shotTime() == doctest::Approx(0.125));

    player.update(5.875); // the end of the last shot, exactly
    REQUIRE(player.blendingOut());
    CHECK(player.active());
    CHECK_FALSE(player.skipped());
    CHECK(player.currentIndex() == 2);
    CHECK(player.shotProgress() == doctest::Approx(1.0));
    CHECK(player.blendOutWeight() == doctest::Approx(0.0));
    CHECK(player.presence() == doctest::Approx(1.0));

    player.update(0.125);
    CHECK(player.blendOutWeight() == doctest::Approx(0.5));
    CHECK(player.presence() == doctest::Approx(0.5));
    CHECK(player.shotProgress() == doctest::Approx(1.0)); // held
    player.update(0.125);
    CHECK(player.finished());
    CHECK_FALSE(player.active());
    CHECK(player.blendOutWeight() == doctest::Approx(1.0));
    CHECK(player.presence() == doctest::Approx(0.0));
    CHECK(player.elapsed() == doctest::Approx(6.75));

    // One long update runs the whole of it.
    player.begin(plan, false, true);
    player.update(6.5);
    CHECK(player.blendingOut());
    player.begin(plan, false, true);
    player.update(100.0);
    CHECK(player.finished());
    CHECK(player.elapsed() == doctest::Approx(100.0));
}

TEST_CASE("a skip in place blends out from wherever the camera got to") {
    CutscenePlan plan = threeShots();
    plan.setBlends(0.5f, 0.25f);
    CutscenePlayer player;

    // While the shots play: the shot on screen holds and the whole blend out follows.
    player.begin(plan, false, true);
    player.update(3.0); // 0.5 in, 2 of shot 0, half of shot 1
    REQUIRE(player.currentIndex() == 1);
    CHECK(player.skip());
    CHECK(player.skipped());
    REQUIRE(player.blendingOut());
    CHECK(player.currentIndex() == 1);
    CHECK(player.shotProgress() == doctest::Approx(0.5)); // held
    CHECK(player.presence() == doctest::Approx(1.0));
    CHECK_FALSE(player.skip()); // already on its way out
    player.tickReal(1.0);
    CHECK_FALSE(player.press());
    player.update(0.125);
    CHECK(player.presence() == doctest::Approx(0.5));
    player.update(0.125);
    CHECK(player.finished());
    CHECK(player.skipped());

    // While blending in: the input grace counts from begin, and the fall starts where the rise got
    // to, over that share of the blend out (0.5 of 0.25 s).
    player.begin(plan, false, true);
    CHECK_FALSE(player.press());
    player.tickReal(0.3);
    player.update(0.25);
    REQUIRE(player.blendingIn());
    CHECK(player.press());
    REQUIRE(player.blendingOut());
    CHECK(player.currentIndex() == 0);
    CHECK(player.presence() == doctest::Approx(0.5));
    player.update(0.0625);
    CHECK(player.presence() == doctest::Approx(0.25));
    player.update(0.0625);
    CHECK(player.finished());

    // A skip on the very first frame has nothing to blend out of.
    player.begin(plan, false, true);
    CHECK(player.skip());
    CHECK(player.finished());
}

TEST_CASE("blend times count only in place, without autoplay") {
    CutscenePlan plan = threeShots(); // default blend times
    CutscenePlayer player;

    // Autoplay in place: no blends, and the real-time cap ends it at once.
    player.begin(plan, true, true);
    CHECK(player.playing());
    CHECK(player.blendInTime() == doctest::Approx(0.0));
    CHECK(player.blendOutTime() == doctest::Approx(0.0));
    player.tickReal(1.0);
    CHECK(player.finished());
    CHECK(player.skipped());

    // Not in place: the plan's blend times don't count.
    player.begin(plan);
    CHECK(player.playing());
    CHECK(player.blendInTime() == doctest::Approx(0.0));
    player.update(6.0);
    CHECK(player.finished());
    CHECK(player.presence() == doctest::Approx(0.0));

    // A zero time skips its phase.
    plan.setBlends(0.f, 0.f);
    player.begin(plan, false, true);
    CHECK(player.playing());
    player.update(6.0);
    CHECK(player.finished());
    plan.setBlends(0.f, 0.5f);
    player.begin(plan, false, true);
    CHECK(player.playing());
    player.update(6.0);
    CHECK(player.blendingOut());

    // An empty plan is done at once, in place too.
    player.begin(CutscenePlan{}, false, true);
    CHECK(player.finished());
    CHECK_FALSE(player.active());
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

TEST_CASE("the stage has the blockout's spots and subject marks") {
    REQUIRE(cutsceneStageMarkCount() == 9);
    std::uint32_t subjects = 0;
    for (std::uint32_t i = 0; i < cutsceneStageMarkCount(); ++i) {
        const CutsceneStageMark& m = cutsceneStageMark(i);
        CHECK(isCutsceneId(m.id));
        CHECK(findCutsceneStageMark(m.id) == &m); // ids are unique
        if (m.subject) {
            ++subjects;
            CHECK(m.position.z == doctest::Approx(0.0)); // on the floor
        } else {
            CHECK(m.position.x > 0.f); // in front of the lead
        }
    }
    CHECK(subjects == 3);
    const CutsceneStageMark* lead = findCutsceneStageMark(kCutsceneLeadMark);
    REQUIRE(lead != nullptr);
    CHECK(lead->subject);
    CHECK(findCutsceneStageMark("spot_z") == nullptr);
    CHECK(&cutsceneStageMark(1000) == &cutsceneStageMark(cutsceneStageMarkCount() - 1));
}

TEST_CASE("the stage lands on the lead: its mark on the feet, its front along the facing") {
    const CutsceneVec3 feet{10.f, -4.f, 3.f};
    const CutsceneStageMark* lead = findCutsceneStageMark(kCutsceneLeadMark);
    const CutsceneStageMark* spotA = findCutsceneStageMark("spot_a");
    const CutsceneStageMark* subjB = findCutsceneStageMark("subj_b");
    REQUIRE(lead != nullptr);
    REQUIRE(spotA != nullptr);
    REQUIRE(subjB != nullptr);
    const CutsceneVec3 ahead{lead->position.x + 1.f, lead->position.y, lead->position.z};
    for (const float yaw : {0.f, 90.f, -135.f, 270.f, 33.f}) {
        CAPTURE(yaw);
        const StageFrame frame = anchorForLead(feet, yaw);
        CHECK(distance3(stageToWorld(frame, lead->position), feet) == doctest::Approx(0.0));
        const CutsceneVec3 front = stageToWorld(frame, ahead);
        const float rad = yaw * 3.14159265f / 180.f;
        CHECK(front.x - feet.x == doctest::Approx(std::cos(rad)));
        CHECK(front.y - feet.y == doctest::Approx(std::sin(rad)));
        CHECK(front.z == doctest::Approx(feet.z));
        // Rigid: distances between marks stay as they were.
        CHECK(distance3(stageToWorld(frame, spotA->position), stageToWorld(frame, subjB->position)) ==
              doctest::Approx(distance3(spotA->position, subjB->position)));
    }
    // Facing +x, spot_a stands 10 m ahead, 1.2 m toward +y and 2.5 m up from the lead's feet...
    CutsceneVec3 a = stageToWorld(anchorForLead(feet, 0.f), spotA->position);
    CHECK(a.x == doctest::Approx(20.0));
    CHECK(a.y == doctest::Approx(-2.8));
    CHECK(a.z == doctest::Approx(5.5));
    // ...and facing +y, ahead is +y.
    a = stageToWorld(anchorForLead(feet, 90.f), spotA->position);
    CHECK(a.x == doctest::Approx(8.8));
    CHECK(a.y == doctest::Approx(6.0));
    CHECK(a.z == doctest::Approx(5.5));

    const StageFrame turned = anchorForLead(feet, 170.f);
    CHECK(stageYawToWorld(turned, 0.f) == doctest::Approx(170.0));
    CHECK(stageYawToWorld(turned, 30.f) == doctest::Approx(-160.0));
    CHECK(anchorForLead(feet, -90.f).turn == doctest::Approx(-90.0));
    CHECK(std::fabs(anchorForLead(feet, 540.f).turn) == doctest::Approx(180.0));
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
    fails(R"({"version": 3, "default": {"shots": [{"spot": "spot_a", "subject": "subj_a"}]}})", "version");
    fails(R"({"version": 2, "default": {"blend_in": 6, "shots": [{"spot": "spot_a", "subject": "subj_a"}]}})",
          "default.blend_in");
    fails(R"({"version": 2, "default": {"shots": [{"spot": "spot_a", "subject": "subj_a"}]},
              "cutscenes": [{"scene": "SC-TEST-01", "beat": 1, "blend_out": -1, "shots": [{"spot": "spot_a", "subject": "subj_a"}]}]})",
          "cutscenes[0].blend_out");
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

TEST_CASE("version 2 tables carry the in-place blend times") {
    CutsceneTable table;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importCutsceneTable(R"({"version": 2,
        "default": {"blend_in": 1.25, "blend_out": 0, "shots": [{"spot": "spot_a", "subject": "subj_a"}]},
        "cutscenes": [
          {"scene": "SC-TEST-01", "beat": 1, "blend_out": 2.5, "shots": [{"spot": "spot_b", "subject": "subj_a"}]},
          {"scene": "SC-TEST-01", "beat": 2, "shots": [{"spot": "spot_b", "subject": "subj_a"}]}]})",
                                        table, &err, &warnings),
                    err);
    CHECK(warnings.empty());
    CHECK(table.fallback.blendIn() == doctest::Approx(1.25));
    CHECK(table.fallback.blendOut() == doctest::Approx(0.0));
    const CutscenePlan& one = table.find("SC-TEST-01", 1, 0);
    CHECK(one.blendIn() == doctest::Approx(CutscenePlan::kDefaultBlend)); // not the default entry's
    CHECK(one.blendOut() == doctest::Approx(2.5));
    CHECK(table.find("SC-TEST-01", 2, 0).blendIn() == doctest::Approx(CutscenePlan::kDefaultBlend));

    // Version 1 has no blend keys: a warning, and the time stays the default.
    warnings.clear();
    REQUIRE_MESSAGE(importCutsceneTable(R"({"version": 1,
        "default": {"blend_in": 2, "shots": [{"spot": "spot_a", "subject": "subj_a"}]}})",
                                        table, &err, &warnings),
                    err);
    CHECK(warnings.size() == 1);
    CHECK(table.fallback.blendIn() == doctest::Approx(CutscenePlan::kDefaultBlend));
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
    // Every id names a mark of the blockout stage: spots for spots, subject marks for subjects.
    const auto isMark = [](std::string_view id, bool subject) {
        const CutsceneStageMark* m = findCutsceneStageMark(id);
        return m != nullptr && m->subject == subject;
    };
    const auto checkPlan = [&](const CutscenePlan& plan) {
        for (std::uint32_t i = 0; i < plan.size(); ++i) {
            const CutsceneShot& s = plan.shot(i);
            CHECK(isMark(s.spot(), false));
            CHECK(isMark(s.subject(), true));
            if (s.hasSubject2()) {
                CHECK(isMark(s.subject2(), true));
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
    // In place: both blends, a skip, and the stage laid over the map.
    CutscenePlan blended = plan;
    blended.setBlends(0.5f, 0.5f);
    CutscenePlayer inPlace;
    inPlace.begin(blended, false, true);
    const StageFrame frame = anchorForLead(a, 45.f);
    const CutsceneStageMark* spotA = findCutsceneStageMark("spot_a");
    for (int i = 0; i < 400 && !inPlace.finished(); ++i) {
        inPlace.update(0.02);
        inPlace.tickReal(0.02);
        if (i == 100) inPlace.press();
        sink += inPlace.presence() + inPlace.blendInWeight() + inPlace.blendOutWeight();
        if (spotA) sink += stageToWorld(frame, spotA->position).x + stageYawToWorld(frame, 10.f);
    }
    CHECK(inPlace.finished());
    // A follower walking round the lead onto its mark.
    MarkWalk walk;
    walk.begin({-2.f, 0.f, 0.f}, {2.f, 0.f, 0.f}, a);
    for (int i = 0; i < 600 && !walk.arrived(); ++i) {
        walk.step(1.f / 60.f);
        sink += walk.position().x + walk.wantSpeed() + walk.stopDistance();
    }
    CHECK(walk.arrived());
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(sink != 0.f);
}

namespace {

// Walks `walk` to its mark at 60 frames a second (at most `seconds`), checking on every frame that it
// keeps to its walking speed, never walks back, and never comes nearer `avoid` than `clearance`.
// Returns the seconds it took (or `seconds` when it never arrived).
float walkToMark(MarkWalk& walk, const CutsceneVec3& avoid, float clearance, float seconds = 20.f) {
    const float dt = 1.f / 60.f;
    float t = 0.f;
    float lastRemaining = walk.remaining();
    float lastStop = -1.f;
    bool braked = false;
    while (!walk.arrived() && t < seconds) {
        walk.step(dt);
        t += dt;
        CHECK(walk.speed() <= walk.tuning().speed + 1e-4f);
        CHECK(walk.remaining() <= lastRemaining + 1e-5f);
        lastRemaining = walk.remaining();
        CHECK(flatDistance(walk.position(), avoid) >= clearance - 1e-3f);
        if (walk.stopDistance() >= 0.f) {
            // Braking: it says so, and the distance left only shrinks.
            CHECK(walk.wantSpeed() == 0.f);
            if (braked) CHECK(walk.stopDistance() <= lastStop + 1e-5f);
            braked = true;
            lastStop = walk.stopDistance();
        } else if (!walk.arrived()) {
            CHECK_FALSE(braked); // it never speeds up again once braking
            CHECK(walk.wantSpeed() == doctest::Approx(walk.tuning().speed));
        }
    }
    return t;
}

} // namespace

TEST_CASE("a mark walk to a clear mark goes straight and stands on it") {
    MarkWalk walk;
    const CutsceneVec3 from{0.f, 0.f, 0.f};
    const CutsceneVec3 mark{3.f, 0.f, 0.f};
    const CutsceneVec3 lead{0.f, -5.f, 0.f};
    walk.begin(from, mark, lead);
    CHECK(walk.pointCount() == 2);
    CHECK(walk.remaining() == doctest::Approx(3.0));
    CHECK_FALSE(walk.arrived());
    CHECK(walk.wantSpeed() == doctest::Approx(1.4));
    CHECK(walk.stopDistance() == -1.f);
    const float took = walkToMark(walk, lead, 0.f);
    CHECK(walk.arrived());
    // 3 m at a 1.4 m/s walk, with the start and the stop: about 2.8 s.
    CHECK(took == doctest::Approx(2.8).epsilon(0.1));
    CHECK(distance3(walk.position(), mark) == doctest::Approx(0.0));
    CHECK(walk.speed() == 0.f);
    CHECK(walk.wantSpeed() == 0.f);
    CHECK(walk.stopDistance() == -1.f);
    // Standing on the mark, more frames change nothing.
    walk.step(0.1f);
    CHECK(distance3(walk.position(), mark) == doctest::Approx(0.0));
}

TEST_CASE("a mark walk past the lead goes round it, touching the clearance") {
    const CutsceneVec3 from{-2.f, 0.f, 0.f};
    const CutsceneVec3 mark{2.f, 0.f, 0.f};
    MarkWalk walk;
    // The straight line passes 0.1 m beside the lead: it goes round the side the line passes on.
    const CutsceneVec3 lead{0.f, 0.1f, 0.f};
    walk.begin(from, mark, lead);
    REQUIRE(walk.pointCount() == 3);
    CHECK(walk.point(1).y < 0.f);
    CHECK(distance3(walk.point(0), from) == doctest::Approx(0.0));
    CHECK(distance3(walk.point(2), mark) == doctest::Approx(0.0));
    CHECK(distance3(walk.point(9), mark) == doctest::Approx(0.0)); // clamped to the mark
    CHECK(walk.remaining() > 4.f);
    walkToMark(walk, lead, walk.tuning().clearance);
    CHECK(walk.arrived());
    CHECK(distance3(walk.position(), mark) == doctest::Approx(0.0));

    // Straight through the lead, it goes round a fixed side, and still keeps its distance.
    MarkWalk through;
    through.begin(from, mark, {0.f, 0.f, 0.f});
    REQUIRE(through.pointCount() == 3);
    CHECK(std::fabs(through.point(1).y) > 0.5f);
    walkToMark(through, {0.f, 0.f, 0.f}, through.tuning().clearance);
    CHECK(through.arrived());

    // A wider clearance from the tuning.
    MarkWalk wide;
    wide.tuning().clearance = 1.5f;
    wide.begin({-4.f, 1.f, 0.f}, {4.f, 0.f, 0.f}, {0.f, 0.f, 0.f});
    REQUIRE(wide.pointCount() == 3);
    walkToMark(wide, {0.f, 0.f, 0.f}, 1.5f);
    CHECK(wide.arrived());
}

TEST_CASE("a mark walk whose turns round the lead nearly line up keeps its corner near") {
    // Both ends just outside the clearance, on either side: the touching lines are nearly parallel and
    // would meet far off, so the corner stands no more than three clearances from the lead.
    MarkWalk walk;
    const CutsceneVec3 lead{0.f, 0.f, 0.f};
    walk.begin({-0.85f, 0.f, 0.f}, {0.85f, 0.f, 0.f}, lead);
    REQUIRE(walk.pointCount() == 3);
    CHECK(flatDistance(walk.point(1), lead) <= 3.f * walk.tuning().clearance + 1e-3f);
    walkToMark(walk, lead, 0.f);
    CHECK(walk.arrived());
}

TEST_CASE("a mark walk starting or ending inside the clearance walks straight") {
    const CutsceneVec3 lead{0.f, 0.f, 0.f};
    MarkWalk out;
    out.begin({0.3f, 0.f, 0.f}, {-3.f, 0.1f, 0.f}, lead);
    CHECK(out.pointCount() == 2);
    MarkWalk in;
    in.begin({-3.f, 0.f, 0.f}, {0.5f, 0.f, 0.f}, lead);
    CHECK(in.pointCount() == 2);
    walkToMark(in, lead, 0.f);
    CHECK(in.arrived());
    // A mark behind it: the lead isn't on the way.
    MarkWalk behind;
    behind.begin({2.f, 0.f, 0.f}, {4.f, 0.f, 0.f}, lead);
    CHECK(behind.pointCount() == 2);
}

TEST_CASE("a mark walk on the spot has arrived, and a step of no time moves nothing") {
    MarkWalk walk;
    CHECK(walk.arrived()); // never begun
    const CutsceneVec3 at{1.f, 2.f, 0.5f};
    walk.begin(at, at, {0.f, 0.f, 0.f});
    CHECK(walk.arrived());
    CHECK(distance3(walk.position(), at) == doctest::Approx(0.0));
    CHECK(walk.wantSpeed() == 0.f);

    MarkWalk still;
    still.begin({0.f, 0.f, 0.f}, {2.f, 0.f, 0.f}, {0.f, 5.f, 0.f});
    still.step(0.f);
    still.step(-1.f);
    CHECK(distance3(still.position(), {0.f, 0.f, 0.f}) == doctest::Approx(0.0));
    CHECK_FALSE(still.arrived());
    CHECK(still.speed() == 0.f);
}

TEST_CASE("a mark walk carries the height along and ends at the mark's") {
    MarkWalk walk;
    const CutsceneVec3 mark{2.f, 0.f, 0.4f};
    walk.begin({0.f, 0.f, 0.f}, mark, {0.f, 5.f, 0.f});
    // The height plays no part in how far it is.
    CHECK(walk.remaining() == doctest::Approx(2.0));
    walkToMark(walk, {0.f, 5.f, 0.f}, 0.f);
    CHECK(walk.position().z == doctest::Approx(0.4));
}
