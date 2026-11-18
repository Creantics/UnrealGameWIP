#include "ftcore/SpecialCine.h"

#include "ftcore/ArenaFight.h"
#include "ftcore/Json.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kDeg = 3.14159265358979f / 180.f;

CutsceneVec3 sub3(const CutsceneVec3& a, const CutsceneVec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float length3(const CutsceneVec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

SpecialCineShot shotOf(float in, float out, CutsceneVec3 from = {2.f, 0.f, 1.5f}) {
    SpecialCineShot s;
    s.in = in;
    s.out = out;
    s.cameraFrom = from;
    s.cameraTo = from;
    s.lookFrom = s.lookTo = {0.f, 0.f, 1.f};
    return s;
}

SpecialCineMove threeShots() {
    SpecialCineMove m;
    REQUIRE(m.setId("test_move"));
    REQUIRE(m.add(shotOf(0.f, 1.f)));
    REQUIRE(m.add(shotOf(1.f, 2.5f, {0.f, 3.f, 2.f})));
    REQUIRE(m.add(shotOf(2.5f, 4.f, {-2.f, 0.5f, 1.f})));
    return m;
}

SpecialCineStage stageAt(float ox, float oy, float tx, float ty) {
    SpecialCineStage st;
    st.origin = {ox, oy, 0.f};
    st.target = {tx, ty, 0.f};
    st.forward = {tx - ox, ty - oy, 0.f};
    return st;
}

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// The committed move, its table entry and its shot file both read.
bool loadCommitted(SpecialCineTable& t, std::vector<std::string>& warnings, std::string& error) {
    std::string text;
    if (!ftcore_test::readProjectData("Cutscene/specials.json", text)) {
        error = "Data/Cutscene/specials.json not found";
        return false;
    }
    if (!importSpecialCineTable(text, t, &error, &warnings)) return false;
    for (std::uint32_t i = 0; i < t.size(); ++i) {
        std::string shots;
        if (!ftcore_test::readProjectData("Cutscene/" + std::string(t.move(i).shotFile()) + ".json", shots)) {
            error = "shot file " + std::string(t.move(i).shotFile()) + " not found";
            return false;
        }
        if (!importSpecialCineShots(shots, t.edit(i), &error, &warnings)) return false;
        const std::string_view nearFile = t.move(i).nearFile();
        if (nearFile.empty()) continue;
        if (!ftcore_test::readProjectData("Cutscene/" + std::string(nearFile) + ".json", shots)) {
            error = "near shot file " + std::string(nearFile) + " not found";
            return false;
        }
        if (!importSpecialCineShots(shots, t.editNear(i), &error, &warnings)) return false;
    }
    return true;
}

const char* kShots = R"({"version": 1, "duration_s": 2, "end_feet": [1, 0, 0], "cuts": [1],
    "shake": {"profile": "decaying", "amp_m": [0.03, 0.05, 0.04], "hz": [21, 28, 25], "decay_s": 0.2},
    "shots": [
      {"id": 1, "name": "a", "in": 0, "out": 1, "anchor": "lead", "offset": {"from": [2, 0, 1], "to": [3, 0, 1]},
       "look_anchor": "lead", "look_at": {"from": [0, 0, 1]}, "ease": "linear", "focal_mm": {"from": 40, "to": 20},
       "focus_m": {"from": 2, "to": 4}, "shake": false},
      {"id": 2, "name": "b", "in": 1, "out": 2, "anchor": "target", "offset": {"from": [1, 0, 1]},
       "look_anchor": "lead", "look_at": {"from": [0, 0, 1]}, "focal_mm": 35, "focus_m": 0, "shake": true}]})";

} // namespace

TEST_CASE("special cine: the stage frame is fixed at the press, right of its x Unreal's way") {
    const SpecialCineStage st = stageAt(1.f, 2.f, 1.f, 7.f); // the target due +y
    const CutsceneVec3 p = specialCinePoint(st, SpecialCineAnchor::Lead, {2.f, 1.f, 0.5f});
    // forward (0,1): right is (-1,0)
    CHECK(near(p.x, 1.f - 1.f));
    CHECK(near(p.y, 2.f + 2.f));
    CHECK(near(p.z, 0.5f));
    const CutsceneVec3 w = specialCinePoint(st, SpecialCineAnchor::World, {2.f, 1.f, 0.5f});
    CHECK(near(w.x, p.x));
    CHECK(near(w.y, p.y));
    // No forward: +x.
    SpecialCineStage flat = stageAt(0.f, 0.f, 0.f, 0.f);
    CHECK(near(specialCinePoint(flat, SpecialCineAnchor::Lead, {1.f, 0.f, 0.f}).x, 1.f));
    // A target's anchor stands outside its body and scales its height.
    SpecialCineStage tall = stageAt(0.f, 0.f, 6.f, 0.f);
    tall.targetRadius = 1.5f;
    tall.targetHeight = 7.2f;
    const CutsceneVec3 t = specialCinePoint(tall, SpecialCineAnchor::Target, {0.f, -2.f, 1.f});
    CHECK(near(t.x, 6.f));
    CHECK(near(t.y, -3.5f)); // 2 m to the left, pushed out by the radius
    CHECK(near(t.z, 4.f));
    // A bone anchor rides the fed bone point, in the stage's axes.
    SpecialCineStage b = stageAt(0.f, 0.f, 0.f, 5.f);
    b.bone = {1.f, 1.f, 1.2f};
    const CutsceneVec3 q = specialCinePoint(b, SpecialCineAnchor::Bone, {0.5f, 0.f, 0.f});
    CHECK(near(q.x, 1.f));
    CHECK(near(q.y, 1.5f));
    CHECK(near(q.z, 1.2f));
}

TEST_CASE("special cine: lens to fov on a 36 mm sensor") {
    CHECK(near(specialCineFov(18.f), 90.f, 1e-3f));
    CHECK(near(specialCineFov(36.f), 53.130f, 1e-2f));
    CHECK(near(specialCineFov(31.18f), 60.f, 0.02f));
    CHECK(specialCineFov(85.f) < specialCineFov(24.f));
    CHECK(near(specialCineFov(1.f), specialCineFov(10.f)));
    CHECK(std::isfinite(specialCineFov(std::numeric_limits<float>::quiet_NaN())));
}

TEST_CASE("special cine: shots must touch, last long enough and stay in range") {
    SpecialCineMove m;
    CHECK_FALSE(m.setId("Bad Id"));
    CHECK_FALSE(m.setId("a_move_id_longer_than_twenty_three"));
    CHECK(m.setId("lead_special_01"));
    CHECK(m.setClip("A_Lead_Special_01"));
    CHECK_FALSE(m.setClip("a clip"));
    CHECK_FALSE(m.add(shotOf(0.2f, 1.f))); // the first starts at 0
    CHECK(m.add(shotOf(0.f, 1.f)));
    CHECK_FALSE(m.add(shotOf(1.1f, 2.f))); // a gap
    CHECK_FALSE(m.add(shotOf(0.9f, 2.f))); // an overlap
    CHECK_FALSE(m.add(shotOf(1.f, 1.05f))); // under 0.1 s
    CHECK_FALSE(m.add(shotOf(1.f, 21.f))); // past 20 s
    SpecialCineShot wide = shotOf(1.f, 2.f);
    wide.lensTo = 5.f;
    CHECK_FALSE(m.add(wide));
    SpecialCineShot nan = shotOf(1.f, 2.f);
    nan.lookTo.x = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(m.add(nan));
    CHECK(m.add(shotOf(1.0005f, 2.f))); // within 1 ms: snapped to touch
    CHECK(m.shot(1).in == 1.f);
    CHECK(m.size() == 2);
    CHECK(m.length() == 2.f);
}

TEST_CASE("special cine: the shot on screen, the eased move within it and the hard cuts") {
    const SpecialCineMove m = threeShots();
    CHECK(m.shotAt(-1.f) == 0);
    CHECK(m.shotAt(0.99f) == 0);
    CHECK(m.shotAt(1.f) == 1);
    CHECK(m.shotAt(3.f) == 2);
    CHECK(m.shotAt(99.f) == 2);
    const SpecialCineStage st = stageAt(0.f, 0.f, 5.f, 0.f);
    // A moving shot eases from `from` to `to`, smoothstep or linear.
    SpecialCineMove mv;
    REQUIRE(mv.setId("moving"));
    SpecialCineShot s = shotOf(0.f, 2.f, {4.f, 0.f, 1.f});
    s.cameraTo = {2.f, 0.f, 1.f};
    s.lookTo = {0.f, 2.f, 1.f};
    s.lensFrom = 40.f;
    s.lensTo = 20.f;
    REQUIRE(mv.add(s));
    CHECK(near(specialCineFrameAt(mv, 0.f, st).pose.position.x, 4.f));
    CHECK(near(specialCineFrameAt(mv, 0.5f, st).pose.position.x, 4.f - 2.f * 0.15625f)); // smoothstep(0.25)
    CHECK(near(specialCineFrameAt(mv, 1.f, st).pose.position.x, 3.f));
    CHECK(near(specialCineFrameAt(mv, 1.f, st).pose.lookAt.y, 1.f));
    CHECK(near(specialCineFrameAt(mv, 1.f, st).fovDegrees, specialCineFov(30.f), 1e-3f));
    CHECK(near(specialCineFrameAt(mv, 2.f, st).pose.position.x, 2.f));
    SpecialCineMove lin = mv;
    lin.clearShots();
    s.ease = SpecialCineEase::Linear;
    REQUIRE(lin.add(s));
    CHECK(near(specialCineFrameAt(lin, 0.5f, st).pose.position.x, 3.5f));
    // No blend in: the first frame is a cut, then one cut on the first frame of each new shot.
    SpecialCinePlayer p;
    p.begin(&m);
    REQUIRE(p.active());
    int frames = 0;
    std::vector<std::uint32_t> cutTo;
    while (p.active()) {
        const SpecialCineFrame f = p.frame(st);
        if (f.cut) cutTo.push_back(f.shot);
        ++frames;
        p.step(1.f / 60.f);
    }
    REQUIRE(cutTo.size() == 3);
    CHECK(cutTo[0] == 0);
    CHECK(cutTo[1] == 1);
    CHECK(cutTo[2] == 2);
    CHECK(frames >= 239);
    CHECK(p.finished());
    CHECK(near(p.time(), 4.f));
    // With a blend in, the first frame is not a cut.
    SpecialCineMove soft = threeShots();
    soft.setBlends(0.15f, -1.f);
    p.begin(&soft);
    CHECK_FALSE(p.frame(st).cut);
}

TEST_CASE("special cine: a hard cut in, and the last shot eased into the play camera with no step at the end") {
    SpecialCineMove m = threeShots();
    const SpecialCineStage st = stageAt(0.f, 0.f, 5.f, 0.f);
    CHECK(m.blendIn() == 0.f);
    CHECK(near(m.blendOut(), 1.5f)); // default: the whole last shot
    CHECK(specialCineFrameAt(m, 0.f, st).playWeight == 0.f);   // the shot's camera from the press
    CHECK(specialCineFrameAt(m, 2.5f, st).playWeight == 0.f);
    CHECK(near(specialCineFrameAt(m, 3.25f, st).playWeight, 0.5f));
    CHECK(specialCineFrameAt(m, 4.f, st).playWeight == 1.f);   // the play camera at the end
    m.setBlends(0.15f, 0.25f);
    CHECK(specialCineFrameAt(m, 0.f, st).playWeight == 1.f);
    CHECK(specialCineFrameAt(m, 0.15f, st).playWeight == 0.f);
    CHECK(specialCineFrameAt(m, 3.75f, st).playWeight == 0.f);
    // Monotonic and smooth at 60 Hz.
    float most = 0.f, prev = 1.f;
    for (int i = 1; i <= 240; ++i) {
        const float w = specialCineFrameAt(m, static_cast<float>(i) / 60.f, st).playWeight;
        most = std::max(most, std::fabs(w - prev));
        prev = w;
    }
    CHECK(most <= 1.5f / (0.15f * 60.f) + 1e-4f); // smoothstep's steepest slope is 1.5 / blend
    // NaN or negative blend in: none; a blend out longer than the move: the whole move.
    m.setBlends(std::numeric_limits<float>::quiet_NaN(), 5.f);
    CHECK(m.blendIn() == 0.f);
    CHECK(near(m.blendOut(), m.length()));
    // Longer than the last shot (1.5 s): it starts in the shot before and runs over the cut, still 1 at the end.
    m.setBlends(0.f, 2.f);
    CHECK(near(m.blendOut(), 2.f));
    CHECK(specialCineFrameAt(m, 1.99f, st).playWeight == 0.f);
    const SpecialCineFrame over = specialCineFrameAt(m, 2.25f, st);
    CHECK(over.shot == 1);
    CHECK(over.playWeight > 0.f);
    CHECK(specialCineFrameAt(m, 4.f, st).playWeight == 1.f);
}

TEST_CASE("special cine: a shot moved off a body: mirrored, raised, pulled toward its look-at, cleared by a begin") {
    const SpecialCineMove m = threeShots();
    const SpecialCineStage st = stageAt(0.f, 0.f, 5.f, 0.f); // forward +x, its right +y
    const SpecialCineFrame plain = specialCineFrameAt(m, 1.5f, st);
    CHECK(near(plain.pose.position.y, 3.f));
    SpecialCineShotFix fixes[SpecialCineMove::kMaxShots]{};
    CHECK_FALSE(fixes[1].moved());
    fixes[1].mirror = true;
    CHECK(fixes[1].moved());
    const SpecialCineFrame mirrored = specialCineFrameAt(m, 1.5f, st, fixes);
    CHECK(near(mirrored.pose.position.x, plain.pose.position.x));
    CHECK(near(mirrored.pose.position.y, -3.f));
    CHECK(near(mirrored.pose.position.z, plain.pose.position.z));
    CHECK(near(mirrored.pose.lookAt.y, -plain.pose.lookAt.y));
    fixes[1].raise = 0.5f;
    CHECK(near(specialCineFrameAt(m, 1.5f, st, fixes).pose.position.z, plain.pose.position.z + 0.5f));
    // Pulled along its own axis: on the line to the look-at, the distance shortened by the pull.
    fixes[1] = {};
    fixes[1].pull = 1.f;
    const SpecialCineFrame pulled = specialCineFrameAt(m, 1.5f, st, fixes);
    const float was = length3(sub3(plain.pose.lookAt, plain.pose.position));
    const float now = length3(sub3(pulled.pose.lookAt, pulled.pose.position));
    CHECK(near(now, was - 1.f, 1e-3f));
    CHECK(near(pulled.pose.lookAt.z, plain.pose.lookAt.z));
    CHECK(near(pulled.focus, now, 1e-3f)); // the auto focus follows the camera
    fixes[1].pull = 50.f;                  // never closer than the gap
    const SpecialCineFrame most = specialCineFrameAt(m, 1.5f, st, fixes);
    CHECK(near(length3(sub3(most.pose.lookAt, most.pose.position)), SpecialCineShotFix::kMinPullGap, 1e-3f));
    fixes[1].pull = std::numeric_limits<float>::quiet_NaN(); // none
    CHECK(near(specialCineFrameAt(m, 1.5f, st, fixes).pose.position.y, 3.f));
    // Other shots untouched.
    CHECK(near(specialCineFrameAt(m, 0.5f, st, fixes).pose.position.y, specialCineFrameAt(m, 0.5f, st).pose.position.y));
    // The player: a fix lasts the play, a begin clears it, out of range refused.
    SpecialCinePlayer p;
    SpecialCineShotFix up;
    CHECK_FALSE(p.setFix(0, up)); // idle
    p.begin(&m);
    up.raise = 1.f;
    CHECK(p.setFix(1, up));
    CHECK_FALSE(p.setFix(3, up));
    CHECK_FALSE(p.setFix(SpecialCineMove::kMaxShots, up));
    p.step(1.5f);
    CHECK(near(p.frame(st).pose.position.z, plain.pose.position.z + 1.f));
    p.begin(&m);
    CHECK_FALSE(p.fix(1).moved());
    p.step(1.5f);
    CHECK(near(p.frame(st).pose.position.z, plain.pose.position.z));
}

TEST_CASE("special cine: focus, lens, f-stop and the positional shake; the release beat flagged once") {
    SpecialCineMove m;
    REQUIRE(m.setId("beats"));
    SpecialCineShot a = shotOf(0.f, 1.f, {3.f, 4.f, 1.f});
    a.lensFrom = a.lensTo = 85.f;
    a.fStop = 1.4f;
    SpecialCineShot b = shotOf(1.f, 2.f);
    b.shake = true;
    b.focusFrom = b.focusTo = 7.f;
    REQUIRE(m.add(a));
    REQUIRE(m.add(b));
    SpecialCineShake k;
    k.amplitude = {0.03f, 0.05f, 0.04f};
    k.hz = {21.f, 28.f, 25.f};
    k.decay = 0.2f;
    m.setShake(k);
    const SpecialCineStage st = stageAt(0.f, 0.f, 5.f, 0.f);
    const SpecialCineFrame fa = specialCineFrameAt(m, 0.5f, st);
    CHECK(near(fa.focus, std::sqrt(9.f + 16.f), 1e-3f)); // auto: the distance to the look-at point
    CHECK(fa.fStop == 1.4f);
    CHECK(near(fa.fovDegrees, specialCineFov(85.f)));
    CHECK(fa.shake.x == 0.f);
    CHECK(fa.shake.z == 0.f);
    float most = 0.f;
    for (float u = 1.f; u < 2.f; u += 0.004f) {
        const SpecialCineFrame f = specialCineFrameAt(m, u, st);
        most = std::max(most, std::fabs(f.shake.y));
        CHECK(std::fabs(f.shake.x) <= 0.03f + 1e-5f);
        CHECK(std::fabs(f.shake.z) <= 0.04f + 1e-5f);
    }
    CHECK(most > 0.02f);
    const SpecialCineFrame late = specialCineFrameAt(m, 1.99f, st);
    CHECK(std::fabs(late.shake.y) < 0.05f * std::exp(-0.99f / 0.2f) + 1e-5f); // it dies away
    CHECK(specialCineFrameAt(m, 1.5f, st).focus == 7.f);
    // The release: the first shaking shot's in by default, else as set; flagged once.
    CHECK(m.release() == 1.f);
    m.setRelease(1.2f);
    SpecialCinePlayer p;
    p.begin(&m);
    int releases = 0;
    float at = -1.f;
    while (p.active()) {
        if (p.frame(st).release) {
            ++releases;
            at = p.time();
        }
        p.step(1.f / 60.f);
    }
    CHECK(releases == 1);
    CHECK(at >= 1.2f);
    CHECK(at < 1.2f + 1.f / 60.f + 1e-4f);
}

TEST_CASE("special cine: the player is idle without a move, stops on a skip and allocates nothing") {
    SpecialCinePlayer p;
    p.begin(nullptr);
    CHECK_FALSE(p.active());
    CHECK(p.frame(stageAt(0.f, 0.f, 1.f, 0.f)).playWeight == 1.f);
    SpecialCineMove empty;
    p.begin(&empty);
    CHECK_FALSE(p.active());
    const SpecialCineMove m = threeShots();
    p.begin(&m);
    const SpecialCineStage st = stageAt(0.f, 0.f, 5.f, 0.f);
    {
        const auto before = ftcore_test::allocStats().count;
        for (int i = 0; i < 60; ++i) {
            (void)p.frame(st);
            p.step(1.f / 60.f);
        }
        if (ftcore_test::allocHookInstalled()) CHECK(ftcore_test::allocStats().count == before);
    }
    p.step(std::numeric_limits<float>::quiet_NaN());
    p.step(-3.f);
    CHECK(near(p.time(), 1.f, 1e-3f));
    p.stop();
    CHECK(p.finished());
    CHECK_FALSE(p.active());
    p.begin(&m); // again from the top
    CHECK(p.active());
    CHECK(p.time() == 0.f);
}

TEST_CASE("special cine: the table's and the shot file's errors") {
    SpecialCineTable t;
    std::string error;
    CHECK_FALSE(importSpecialCineTable("{", t, &error));
    CHECK_FALSE(importSpecialCineTable(R"({"version": 2, "moves": []})", t, &error));
    CHECK(error.find("version") != std::string::npos);
    CHECK_FALSE(importSpecialCineTable(R"({"moves": [{"id": "m", "shots": "a b"}]})", t, &error));
    CHECK_FALSE(importSpecialCineTable(R"({"moves": [{"id": "m", "shots": "f", "fstop": 99}]})", t, &error));
    CHECK_FALSE(importSpecialCineTable(R"({"moves": [{"id": "m", "shots": "f"}, {"id": "m", "shots": "g"}]})", t, &error));
    CHECK(error.find("duplicate") != std::string::npos);
    CHECK(t.size() == 0);
    std::vector<std::string> warnings;
    REQUIRE(importSpecialCineTable(R"({"moves": [{"id": "m", "shots": "f", "colour": 1, "release": 1.5}]})", t, &error, &warnings));
    CHECK(warnings.size() == 1);
    REQUIRE(t.find("m") != nullptr);
    CHECK(t.find("m")->shotFile() == "f");
    CHECK(t.find("nope") == nullptr);

    // The shot file.
    SpecialCineMove& m = t.edit(0);
    warnings.clear();
    REQUIRE_MESSAGE(importSpecialCineShots(kShots, m, &error, &warnings), error);
    CHECK(warnings.empty());
    CHECK(m.size() == 2);
    CHECK(m.release() == 1.5f); // the table's, kept
    CHECK(m.endFeet().x == 1.f);
    CHECK(m.shot(0).ease == SpecialCineEase::Linear);
    CHECK(m.shot(0).lensFrom == 40.f);
    CHECK(m.shot(0).lensTo == 20.f);
    CHECK(m.shot(0).focusTo == 4.f);
    CHECK(m.shot(1).cameraAnchor == SpecialCineAnchor::Target);
    CHECK(m.shot(1).cameraTo.x == 1.f); // "to" defaults to "from"
    CHECK(m.shot(1).fStop == SpecialCineMove::kDefaultFStop);
    CHECK(m.shake().hz.y == 28.f);
    auto bad = [&](const std::string& from, const std::string& to) {
        std::string text = kShots;
        const std::size_t at = text.find(from);
        REQUIRE(at != std::string::npos);
        text.replace(at, from.size(), to);
        const bool ok = importSpecialCineShots(text, m, &error);
        CHECK(m.size() == (ok ? 2u : 0u));
        return !ok;
    };
    CHECK(bad(R"("in": 1, "out": 2)", R"("in": 1.2, "out": 2)"));  // a gap
    CHECK(bad(R"("duration_s": 2)", R"("duration_s": 2.5)"));      // the duration is not the last out
    CHECK(bad(R"("anchor": "target")", R"("anchor": "boss")"));
    CHECK(bad(R"("ease": "linear")", R"("ease": "bounce")"));
    CHECK(bad(R"("focal_mm": 35)", R"("focal_mm": 5)"));
    CHECK(bad(R"("anchor": "target")", R"("anchor": "bone")"));    // a bone anchor without follow_bone
    CHECK(bad(R"("decay_s": 0.2)", R"("decay_s": 0)"));
    CHECK(bad(R"("end_feet": [1, 0, 0])", R"("end_feet": [9, 0, 0])"));
    // Cuts that do not match the shots: a warning, the shots win.
    std::string cuts = kShots;
    cuts.replace(cuts.find(R"("cuts": [1])"), 11, R"("cuts": [0.8])");
    warnings.clear();
    CHECK(importSpecialCineShots(cuts, m, &error, &warnings));
    CHECK(warnings.size() == 1);
}

TEST_CASE("special cine: the committed move loads, ends on the close camera over its end feet, keeps clear of the target") {
    SpecialCineTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(loadCommitted(t, warnings, error), error);
    for (const std::string& w : warnings) MESSAGE(w);
    CHECK(warnings.empty());
    const SpecialCineMove* m = t.find("devin_special_01");
    REQUIRE(m != nullptr);
    CHECK(m->size() == 8);
    CHECK(near(m->length(), 8.5f, 1e-3f));
    CHECK(m->blendIn() == 0.f);                              // a hard cut in
    CHECK(near(m->blendOut(), 0.25f, 1e-3f));                // eased into the play camera over the spec's 0.25 s
    CHECK(m->endFeet().x > 0.5f);                            // it travels toward the target
    int shaking = 0;
    for (std::uint32_t i = 0; i < m->size(); ++i) shaking += m->shot(i).shake ? 1 : 0;
    CHECK(shaking == 1);
    CHECK(m->shot(5).shake);
    CHECK(m->shot(5).cameraAnchor == SpecialCineAnchor::Target);
    CHECK(near(m->release(), m->shot(5).in));
    CHECK(m->shot(3).followBone() == "hand_r");

    // The last shot's end: the close preset at rest over the end feet (Data/Battle/camera_presets.json), the boom's
    // socket offset turned by the pitch: within 1 cm, its view within half a degree, its fov within 0.1 degrees.
    std::string presets;
    REQUIRE(ftcore_test::readProjectData("Battle/camera_presets.json", presets));
    JsonDocument doc;
    REQUIRE(doc.parse(presets, nullptr));
    const JsonValue close = doc.root()["close"];
    REQUIRE(close.isObject());
    const float arm = static_cast<float>(close["arm"].number()), pivot = static_cast<float>(close["pivot"].number());
    const float side = static_cast<float>(close["side"].number()), up = static_cast<float>(close["up"].number());
    const float pitch = static_cast<float>(close["lockPitch"].number()) * kDeg, fov = static_cast<float>(close["fov"].number());
    const CutsceneVec3 feet = m->endFeet();
    const float ex = feet.x - arm * std::cos(pitch) - up * std::sin(pitch);
    const float ez = pivot - arm * std::sin(pitch) + up * std::cos(pitch);
    const SpecialCineStage st = stageAt(0.f, 0.f, 5.f, 0.f);
    const SpecialCineShot& last = m->shot(m->size() - 1);
    const CutsceneVec3 eye = specialCinePoint(st, last.cameraAnchor, last.cameraTo);
    const CutsceneVec3 look = specialCinePoint(st, last.lookAnchor, last.lookTo);
    CHECK(near(eye.x, ex, 0.01f));
    CHECK(near(eye.y, feet.y + side, 0.01f));
    CHECK(near(eye.z, ez, 0.01f));
    const float lookPitch = std::atan2(look.z - eye.z, std::sqrt((look.x - eye.x) * (look.x - eye.x) + (look.y - eye.y) * (look.y - eye.y)));
    CHECK(near(lookPitch, pitch, 0.5f * kDeg));
    CHECK(near(std::atan2(look.y - eye.y, look.x - eye.x), 0.f, 0.5f * kDeg));
    CHECK(near(specialCineFov(last.lensTo), fov, 0.1f));

    // Against a person 6 m off (the design's nearest) and a titan 8 m off: no shot's camera inside the target's body
    // or under the floor. (A titan at 6 m has shot 2's camera inside it: the host pushes it out.)
    for (const float radius : {0.4f, 1.6f}) {
        SpecialCineStage s = stageAt(0.f, 0.f, radius > 1.f ? 8.f : 6.f, 0.f);
        s.targetRadius = radius;
        s.targetHeight = radius > 1.f ? 7.f : 1.8f;
        s.bone = {0.9f, 0.3f, 1.2f};
        for (float tt = 0.f; tt <= m->length(); tt += 0.05f) {
            const SpecialCineFrame f = specialCineFrameAt(*m, tt, s);
            const float dx = f.pose.position.x - s.target.x, dy = f.pose.position.y - s.target.y;
            CHECK(std::sqrt(dx * dx + dy * dy) > radius);
            CHECK(f.pose.position.z > 0.1f);
        }
    }
}

namespace {

// The capture's stage at the press: a brute 1.98 m off, 0.45 m wide, 1.97 m tall, straight ahead.
SpecialCineStage capturedStage() {
    SpecialCineStage st = stageAt(0.f, 0.f, 1.98f, 0.f);
    st.targetRadius = 0.45f;
    st.targetHeight = 1.97f;
    st.bone = {0.3f, 0.25f, 1.1f};
    return st;
}

SpecialCineBody bodyOf(const SpecialCineStage& st) {
    SpecialCineBody b;
    b.feet = st.target;
    b.radius = st.targetRadius;
    b.height = st.targetHeight;
    b.target = true;
    return b;
}

SpecialCineShotFix fixOf(bool mirror, float raise, bool aim) {
    SpecialCineShotFix f;
    f.mirror = mirror;
    f.raise = raise;
    f.aim = aim;
    return f;
}

// One shot, 0..1 s, a world-anchored camera at `eye` looking at `look`; it shows `subject`.
SpecialCineMove oneShot(CutsceneVec3 eye, CutsceneVec3 look, SpecialCineSubject subject) {
    SpecialCineMove m;
    REQUIRE(m.setId("framing"));
    SpecialCineShot s = shotOf(0.f, 1.f, eye);
    s.cameraAnchor = s.lookAnchor = SpecialCineAnchor::World;
    s.lookFrom = s.lookTo = look;
    s.subject = subject;
    REQUIRE(m.add(s));
    return m;
}

// A wall on the stage's right: anything at y over 1 m is in it.
bool rightWall(const CutsceneVec3& from, const CutsceneVec3& to, void*) { return from.y > 1.f || to.y > 1.f; }

} // namespace

namespace {

// Whether `p` stands inside the frame's safe box (SpecialCineFraming::kSafe of the half-frame), by its own sums.
bool inSafeFrame(const SpecialCineFrame& f, const CutsceneVec3& p) {
    CutsceneVec3 fwd = sub3(f.pose.lookAt, f.pose.position);
    const float n = length3(fwd);
    fwd = {fwd.x / n, fwd.y / n, fwd.z / n};
    CutsceneVec3 right{-fwd.y, fwd.x, 0.f}; // right of the view on the ground, Unreal's way
    const float rn = length3(right);
    right = {right.x / rn, right.y / rn, 0.f};
    const CutsceneVec3 up{right.y * fwd.z - right.z * fwd.y, right.z * fwd.x - right.x * fwd.z, right.x * fwd.y - right.y * fwd.x};
    const CutsceneVec3 d = sub3(p, f.pose.position);
    const float z = d.x * fwd.x + d.y * fwd.y + d.z * fwd.z;
    if (z <= 0.f) return false;
    const float tanX = std::tan(0.5f * f.fovDegrees * kDeg), tanY = tanX / SpecialCineFraming::kAspect;
    const float x = (d.x * right.x + d.y * right.y + d.z * right.z) / (z * tanX);
    const float y = (d.x * up.x + d.y * up.y + d.z * up.z) / (z * tanY);
    return std::fabs(x) <= SpecialCineFraming::kSafe && std::fabs(y) <= SpecialCineFraming::kSafe;
}

// The lead's travel on a stage `d` metres off with no room behind it: the host's clamp shortens the walk to stop at
// body contact (with room behind, it starts back instead and the stage is then at least contact + travel off, which
// is a full walk at a larger distance).
float travelAt(float d, float contact, float walk) {
    return walk > 0.f ? std::clamp((d - contact - 0.01f) / walk, 0.f, 1.f) : 1.f;
}

// Whether every shot that frames someone passes as written (no fix) on `st`, the target its only body.
bool passesAsWritten(const SpecialCineMove& m, const SpecialCineStage& st, std::uint32_t& failedShot) {
    SpecialCineBody body;
    body.feet = st.target;
    body.radius = st.targetRadius;
    body.height = st.targetHeight;
    body.target = true;
    for (std::uint32_t i = 0; i < m.size(); ++i) {
        if (m.shot(i).subject == SpecialCineSubject::None) continue;
        if (specialCineFixShot(m, i, st, &body, 1).first != SpecialCineProblem::None) {
            failedShot = i;
            return false;
        }
    }
    return true;
}

// The same with the target its only body, split: `oldOk` whether every shot that frames someone passes as written
// every check before the contact and lead ones (the switch point's measure), `fixedOk` whether every one passes as
// written or with a fix; `reframed` counts the shots moved for the contact or the lead alone.
void framedOnStage(const SpecialCineMove& m, const SpecialCineStage& st, bool& oldOk, bool& fixedOk, int& reframed,
                   std::uint32_t& failedShot) {
    const SpecialCineBody body = bodyOf(st);
    oldOk = fixedOk = true;
    for (std::uint32_t i = 0; i < m.size(); ++i) {
        if (m.shot(i).subject == SpecialCineSubject::None) continue;
        const SpecialCineShotCheck c = specialCineFixShot(m, i, st, &body, 1);
        if (c.first == SpecialCineProblem::None) continue;
        const bool hidden = c.first == SpecialCineProblem::ContactHidden || c.first == SpecialCineProblem::LeadHidden;
        if (!hidden) {
            oldOk = false;
            failedShot = i;
        } else if (c.ok) {
            ++reframed;
        }
        if (!c.ok) {
            fixedOk = false;
            failedShot = i;
        }
    }
}

} // namespace

TEST_CASE("special cine: the aim keeps both figures of a two-shot in frame (the release shot as captured at 7.33 s)") {
    // The release shot as it was captured (round 1): a camera beside and beyond the target, its look-at riding out
    // 2.4..6 m ahead of the lead. On the captured stage (a brute 1.98 m off) it faces away from both figures by 92%.
    SpecialCineMove m;
    REQUIRE(m.setId("captured_release"));
    SpecialCineShot s = shotOf(0.f, 1.f, {2.2f, -1.05f, 1.6f});
    s.cameraAnchor = SpecialCineAnchor::Target;
    s.lookAnchor = SpecialCineAnchor::Lead;
    s.lookFrom = {2.4f, 0.f, 0.45f};
    s.lookTo = {6.f, 0.f, 0.55f};
    s.lensFrom = s.lensTo = 40.f;
    s.subject = SpecialCineSubject::Both;
    REQUIRE(m.add(s));
    const SpecialCineStage st = capturedStage();
    const SpecialCineBody body = bodyOf(st);
    const float at = 0.92f;
    SpecialCineShotFix fixes[SpecialCineMove::kMaxShots]{};
    CHECK(specialCineFrameProblem(m, at, st, fixes, &body, 1) == SpecialCineProblem::OffFrame);
    // Mirrored: both figures stand on the stage's axis, so the mirror frames nothing more. Raised: no better.
    fixes[0] = fixOf(true, 0.f, false);
    CHECK(specialCineFrameProblem(m, at, st, fixes, &body, 1) == SpecialCineProblem::OffFrame);
    fixes[0] = fixOf(false, 0.8f, false);
    CHECK(specialCineFrameProblem(m, at, st, fixes, &body, 1) == SpecialCineProblem::OffFrame);
    // Aimed: by sums of this test's own both chests and both waists stand in the safe frame at every sample of the
    // shot, and every check up to the contact passes; but the camera stands beyond the target looking back, so the
    // target hides the contact on its far side (the contact check is tested last).
    fixes[0] = fixOf(false, 0.f, true);
    CHECK(specialCineFrameProblem(m, at, st, fixes, &body, 1) == SpecialCineProblem::ContactHidden);
    for (float u = 0.02f; u <= 0.99f; u += 0.24f) {
        CAPTURE(u);
        const SpecialCineFrame f = specialCineFrameAt(m, u, st, fixes);
        CHECK(inSafeFrame(f, {0.f, 0.f, 1.1f}));                   // the lead's chest (no travel in this move)
        CHECK(inSafeFrame(f, {0.f, 0.f, 0.9f}));                   // its waist
        CHECK(inSafeFrame(f, {st.target.x, 0.f, 0.6f * st.targetHeight}));
        CHECK(inSafeFrame(f, {st.target.x, 0.f, 0.5f * st.targetHeight}));
    }
    // The search: as written the contact is hidden from the first sample on; the mirror, the raises, the aims and the
    // pulls all keep the camera on the target's line, so the reframe finds it: swung 30 degrees to the stage's left
    // (beside the target, the contact in view past its edge) and aimed; every plain swing before it fails.
    const SpecialCineShotCheck c = specialCineFixShot(m, 0, st, &body, 1);
    CHECK(c.first == SpecialCineProblem::ContactHidden);
    CHECK(c.ok);
    CHECK(c.fix.orbit == doctest::Approx(-30.f));
    CHECK(c.fix.aim);
    CHECK_FALSE(c.fix.mirror);
    CHECK(c.fix.raise == 0.f);
    CHECK(c.fix.pull == 0.f);
    CHECK(c.tries == 45); // as written, 36 of the old fixes, then 15 and 30 degrees each way, each plain and aimed
    fixes[0] = c.fix;
    for (float u = 0.02f; u <= 0.99f; u += 0.24f) {
        CAPTURE(u);
        CHECK(specialCineFrameProblem(m, u, st, fixes, &body, 1) == SpecialCineProblem::None);
        const SpecialCineFrame f = specialCineFrameAt(m, u, st, fixes);
        CHECK(inSafeFrame(f, {0.f, 0.f, 1.1f}));
        CHECK(inSafeFrame(f, {st.target.x, 0.f, 0.6f * st.targetHeight}));
    }
}

TEST_CASE("special cine: the near set: picked under near_within, the same clock as the far set, every shot clean at the capture") {
    SpecialCineTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(loadCommitted(t, warnings, error), error);
    for (const std::string& w : warnings) MESSAGE(w);
    CHECK(warnings.empty());
    const SpecialCineMove* far = t.find("devin_special_01");
    REQUIRE(far != nullptr);
    const SpecialCineMove& nearSet = t.nearMove(0);
    REQUIRE(far->nearWithin() > 0.f);
    CHECK(nearSet.shotFile() == far->nearFile());
    CHECK(nearSet.id() == far->id());
    // Picked by distance: under near_within the near set, at it and past it the far one; the far-only find unchanged.
    CHECK(t.find("devin_special_01", far->nearWithin() - 0.01f) == &nearSet);
    CHECK(t.find("devin_special_01", 0.f) == &nearSet);
    CHECK(t.find("devin_special_01", far->nearWithin()) == far);
    CHECK(t.find("devin_special_01", 15.f) == far);
    CHECK(t.find("devin_special_01", std::numeric_limits<float>::quiet_NaN()) == far);
    CHECK(t.find("nope", 1.f) == nullptr);
    // The same clip clock: shot count, cuts, release, blends, f-stop, travel, shake beat; the last shot's end the same.
    REQUIRE(nearSet.size() == far->size());
    CHECK(nearSet.clip() == far->clip());
    for (std::uint32_t i = 0; i < far->size(); ++i) {
        CAPTURE(i);
        CHECK(nearSet.shot(i).in == far->shot(i).in);
        CHECK(nearSet.shot(i).out == far->shot(i).out);
        CHECK(nearSet.shot(i).shake == far->shot(i).shake);
        CHECK(nearSet.shot(i).subject == far->shot(i).subject);
    }
    CHECK(nearSet.release() == far->release());
    CHECK(nearSet.blendIn() == far->blendIn());
    CHECK(nearSet.blendOut() == far->blendOut());
    CHECK(nearSet.fStop() == far->fStop());
    CHECK(nearSet.endFeet().x == far->endFeet().x);
    const SpecialCineShot& a = nearSet.shot(nearSet.size() - 1);
    const SpecialCineShot& b = far->shot(far->size() - 1);
    CHECK(a.cameraTo.x == b.cameraTo.x);
    CHECK(a.lookTo.z == b.lookTo.z);
    CHECK(a.lensTo == b.lensTo);

    // The capture's stage (a brute 1.98 m off) picks the near set, and every shot that frames someone passes there as
    // written: nothing to move.
    const SpecialCineStage st = capturedStage();
    REQUIRE(t.find("devin_special_01", specialCineDistance(st)) == &nearSet);
    std::uint32_t failed = 99;
    CHECK(passesAsWritten(nearSet, st, failed));
    CAPTURE(failed);
    // Its release shot (the strike) is a two-shot whose look-at ends on the target from the target's own anchor.
    const SpecialCineShot& release = nearSet.shot(5);
    CHECK(release.subject == SpecialCineSubject::Both);
    CHECK(release.lookToAnchor() == SpecialCineAnchor::Target);
    CHECK(far->shot(5).lookToAnchor() == SpecialCineAnchor::Target);
}

TEST_CASE("special cine: the range sweep: the far set from the switch point to lock range, the near set from contact to it") {
    SpecialCineTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(loadCommitted(t, warnings, error), error);
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Battle/arena.json", text));
    ArenaTable arena;
    REQUIRE_MESSAGE(importArenaTable(text, arena, &error), error);
    const ArenaFighter& lead = arena.fighter("devin");
    const SpecialCineMove* far = t.find("devin_special_01");
    REQUIRE(far != nullptr);
    const SpecialCineMove& nearSet = t.nearMove(0);
    const float within = far->nearWithin();
    REQUIRE(within > 0.f);
    REQUIRE(lead.lockRange > within);
    const CutsceneVec3 end = far->endFeet();
    const float walk = std::sqrt(end.x * end.x + end.y * end.y);
    constexpr float kStep = 0.05f;

    // Every person-sized kind (body radius up to 0.5 m: the bosses and big kinds are not on this list, see below),
    // every 5 cm from body contact out to lock range, both from arena.json.
    // The far set is measured by the checks before the contact and lead ones (the switch point's measure); a shot it
    // loses only to those is reframed, and must be at every range past the switch point.
    int kinds = 0, reframed = 0, unfixed = 0;
    float firstUnfixed = 1e9f;
    float lastFarFail = -1.f; // the far set's last failing distance over all of them: the switch point sits just past
    for (std::size_t k = 0; k < arena.enemies.size(); ++k) {
        const ArenaEnemyKind& kind = arena.enemies[k];
        if (kind.bodyRadius > 0.5f) continue;
        ++kinds;
        CAPTURE(arena.enemyIds[k]);
        const float contact = lead.bodyRadius + kind.bodyRadius;
        for (int i = 0;; ++i) {
            const float d = contact + kStep * static_cast<float>(i);
            if (d > lead.lockRange + 1e-3f) break;
            CAPTURE(d);
            SpecialCineStage st = stageAt(0.f, 0.f, d, 0.f);
            st.targetRadius = kind.bodyRadius;
            st.targetHeight = arenaEnemyHeight(kind);
            st.travel = travelAt(d, contact, walk);
            st.bone = {0.3f, 0.25f, 1.1f};
            const SpecialCineMove* picked = t.find("devin_special_01", specialCineDistance(st));
            REQUIRE(picked != nullptr);
            std::uint32_t failed = 99;
            bool farOk = true, farFixed = true;
            int moved = 0;
            framedOnStage(*far, st, farOk, farFixed, moved, failed);
            if (!farOk) lastFarFail = std::max(lastFarFail, d);
            if (d >= within) {
                CHECK(picked == far);
                CAPTURE(failed);
                CHECK(farOk);
                reframed += moved;
                if (!farFixed) {
                    // Measured: only the smallest kind (0.9 m tall) past 12.3 m, only shot 6. From beyond it the
                    // contact is hidden; from over the lead's shoulder it is under the minimum figure; no swing to 60
                    // degrees frames both. Counted as still failing (the host plays it as written and logs it).
                    ++unfixed;
                    firstUnfixed = std::min(firstUnfixed, d);
                    CHECK(arenaEnemyHeight(kind) < 1.f);
                    CHECK(d > 12.3f);
                    CHECK(failed == 5);
                }
            } else {
                CHECK(picked == &nearSet);
                CHECK(passesAsWritten(nearSet, st, failed));
            }
        }
    }
    CHECK(kinds >= 6);
    // The switch point is where the far set stops failing: near_within is the first 5 cm step past its last failure.
    CHECK(lastFarFail < within);
    CHECK(lastFarFail > within - 2.f * kStep);
    // Past it the far set's release shot (6) loses the contact behind the target at every range, and is reframed
    // (but for the smallest kind far out, above).
    CHECK(reframed > 0);
    MESSAGE("the far set past the switch point: " << reframed << " shots reframed for a hidden contact or lead; "
            << unfixed << " with no fix, from " << firstUnfixed << " m");
    // The big kinds (radius over 0.5 m: hulk, stone bearer, titan, the kite boss) are left out: at contact both sets
    // put a camera in or behind such a body, and the far set needs 6 to 7.3 m. They wait for a boss rule.
}

TEST_CASE("special cine: the held reach keeps the camera out over its window, not a bone shot's, and frames nothing") {
    // A lead-anchored camera 1.2 m beside the lead, on its chest: clear of the lead itself, inside a 1.1 m reach.
    SpecialCineMove m;
    REQUIRE(m.setId("reach"));
    SpecialCineShot s = shotOf(0.f, 8.5f, {0.f, 1.2f, 1.2f});
    s.cameraAnchor = s.lookAnchor = SpecialCineAnchor::Lead;
    s.lookFrom = s.lookTo = {0.f, 0.f, 1.f};
    s.subject = SpecialCineSubject::Lead;
    REQUIRE(m.add(s));
    SpecialCineStage st = stageAt(0.f, 0.f, 6.f, 0.f);
    st.targetRadius = 0.4f;
    st.bone = {0.3f, 0.25f, 1.1f};
    const auto at = [&](const SpecialCineMove& mv, float t) { return specialCineFrameProblem(mv, t, st, nullptr, nullptr, 0); };
    for (const float t : {0.3f, 4.f, 8.f}) CHECK(at(m, t) == SpecialCineProblem::None); // no reach: it passes
    m.setReach({1.1f, 0.55f, 7.8f});
    CHECK(m.reach().radius == doctest::Approx(1.1f));
    CHECK(at(m, 0.3f) == SpecialCineProblem::None); // before the conjure
    CHECK(at(m, 0.55f) == SpecialCineProblem::InsideBody);
    CHECK(at(m, 4.f) == SpecialCineProblem::InsideBody);
    CHECK(at(m, 7.8f) == SpecialCineProblem::InsideBody);
    CHECK(at(m, 8.f) == SpecialCineProblem::None); // dismissed
    // Out of it: 1.4 m off passes in the window.
    {
        SpecialCineMove w = m;
        w.clearShots();
        w.setReach({1.1f, 0.55f, 7.8f});
        SpecialCineShot o = s;
        o.cameraFrom = o.cameraTo = {0.f, 1.4f, 1.2f};
        REQUIRE(w.add(o));
        CHECK(at(w, 4.f) == SpecialCineProblem::None);
    }
    // The fix search finds a way out of the reach (raised over it).
    const SpecialCineShotCheck fix = specialCineFixShot(m, 0, st, nullptr, 0);
    CHECK(fix.first == SpecialCineProblem::InsideBody);
    CHECK(fix.ok);
    CHECK(fix.fix.moved());
    CHECK(specialCineFrameProblem(m, 4.f, st, &fix.fix, nullptr, 0) == SpecialCineProblem::None);
    // A bone shot is framed on a bone on purpose: the reach does not move it.
    {
        SpecialCineMove b = m;
        b.clearShots();
        b.setReach({1.1f, 0.55f, 7.8f});
        SpecialCineShot o = s;
        o.cameraAnchor = SpecialCineAnchor::Bone;
        o.cameraFrom = o.cameraTo = {0.f, 0.6f, 0.1f};
        REQUIRE(b.add(o));
        CHECK(at(b, 4.f) != SpecialCineProblem::InsideBody);
    }
    // The reach frames nothing: a long lens on a point of the staff 1 m beside the lead, the lead itself off frame.
    {
        SpecialCineMove o = m;
        o.clearShots();
        o.setReach({1.1f, 0.55f, 7.8f});
        SpecialCineShot q = s;
        q.cameraFrom = q.cameraTo = {0.f, 5.f, 1.f};
        q.lookFrom = q.lookTo = {1.f, 0.f, 1.f};
        q.lensFrom = q.lensTo = 200.f;
        REQUIRE(o.add(q));
        CHECK(at(o, 4.f) == SpecialCineProblem::OffFrame);
    }
    // Bad reaches are none.
    SpecialCineMove bad = m;
    bad.setReach({-1.f, 0.f, 1.f});
    CHECK(bad.reach().radius == 0.f);
    bad.setReach({1.f, 2.f, 1.f});
    CHECK(bad.reach().radius == 0.f);
}

TEST_CASE("special cine: the committed shots carry the staff's reach from the conjure to the dismiss") {
    SpecialCineTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(loadCommitted(t, warnings, error), error);
    const SpecialCineMove* far = t.find("devin_special_01");
    REQUIRE(far != nullptr);
    for (const SpecialCineMove* m : {far, &t.nearMove(0)}) {
        CHECK(m->reach().radius == doctest::Approx(1.1f));
        CHECK(m->reach().from == doctest::Approx(0.55f));
        CHECK(m->reach().to == doctest::Approx(7.8f));
    }
}

TEST_CASE("special cine: a big target frames the lead and its strike point; an aimed shot tilts up over it") {
    // A hulk-sized target (1.2 m wide, 3.5 m tall) 2.8 m off: its strike point is its near side at the lead's chest.
    SpecialCineStage st = stageAt(0.f, 0.f, 2.8f, 0.f);
    st.targetRadius = 1.2f;
    st.targetHeight = 3.5f;
    const float chest = 0.6f * st.leadHeight;
    // A camera that sees the strike point but not the hulk's whole figure passes: cropping it is accepted.
    const SpecialCineMove crop = oneShot({1.6f, -1.6f, chest}, {1.6f, 0.f, chest}, SpecialCineSubject::Target);
    CHECK(specialCineFrameProblem(crop, 0.5f, st, nullptr, nullptr, 0) == SpecialCineProblem::None);
    // Looking away from the strike point fails, however much of the hulk is in frame.
    const SpecialCineMove away = oneShot({1.6f, -1.6f, chest}, {4.f, 0.f, 3.f}, SpecialCineSubject::Target);
    CHECK(specialCineFrameProblem(away, 0.5f, st, nullptr, nullptr, 0) == SpecialCineProblem::OffFrame);
    // Aimed: the look-at is the strike point, tilted up kBigTilt.
    SpecialCineShotFix aim;
    aim.aim = true;
    const SpecialCineFrame f = specialCineFrameAt(away, 0.5f, st, &aim);
    CHECK(f.pose.lookAt.x == doctest::Approx(2.8f - 1.2f));
    CHECK(f.pose.lookAt.y == doctest::Approx(0.f));
    CHECK(f.pose.lookAt.z == doctest::Approx(chest + SpecialCineFraming::kBigTilt));
    // A target of 0.5 m or less is not big: its chest, untilted.
    st.targetRadius = 0.5f;
    const SpecialCineFrame g = specialCineFrameAt(away, 0.5f, st, &aim);
    CHECK(g.pose.lookAt.x == doctest::Approx(2.8f));
    CHECK(g.pose.lookAt.z == doctest::Approx(0.6f * st.targetHeight));
}

TEST_CASE("special cine: on a big target no fix ends farther from the authored look-at than the shot plus its raise, or wider") {
    SpecialCineTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(loadCommitted(t, warnings, error), error);
    const SpecialCineMove* far = t.find("devin_special_01");
    REQUIRE(far != nullptr);
    const SpecialCineMove& nearSet = t.nearMove(0);
    float worst = 0.f;     // the most a fix's eye ended farther from the authored look-at (metres)
    float worstRaise = 0.f;
    int fixed = 0, failed = 0;
    for (const float radius : {0.6f, 1.2f, 2.f}) {
        for (float d = 0.4f + radius + 0.01f; d <= 12.f; d += 0.25f) {
            SpecialCineStage st = stageAt(0.f, 0.f, d, 0.f);
            st.targetRadius = radius;
            st.targetHeight = 3.5f;
            st.bone = {0.3f, 0.25f, 1.1f};
            const SpecialCineBody body = bodyOf(st);
            for (const SpecialCineMove* m : {far, &nearSet}) {
                for (std::uint32_t i = 0; i < m->size(); ++i) {
                    if (m->shot(i).subject == SpecialCineSubject::None) continue;
                    const SpecialCineShotCheck c = specialCineFixShot(*m, i, st, &body, 1);
                    if (!c.ok) {
                        ++failed;
                        continue;
                    }
                    if (!c.fix.moved()) continue;
                    ++fixed;
                    std::array<SpecialCineShotFix, 16> fixes{}, written{};
                    REQUIRE(m->size() <= fixes.size());
                    fixes[i] = c.fix;
                    written[i].mirror = c.fix.mirror; // the mirror moves nothing nearer or farther
                    written[i].orbit = c.fix.orbit;   // nor the orbit: camera and look-at turned together
                    const SpecialCineShot& s = m->shot(i);
                    for (int k = 0; k <= 4; ++k) {
                        const float at = s.in + (s.out - s.in) * static_cast<float>(k) / 4.f;
                        const SpecialCineFrame a = specialCineFrameAt(*m, at, st, written.data());
                        const SpecialCineFrame b = specialCineFrameAt(*m, at, st, fixes.data());
                        if (a.shot != i || b.shot != i) continue;
                        const float authored = length3(sub3(a.pose.lookAt, a.pose.position));
                        const float now = length3(sub3(a.pose.lookAt, b.pose.position));
                        const float raise = std::isfinite(c.fix.raise) && c.fix.raise > 0.f ? c.fix.raise : 0.f;
                        CAPTURE(radius);
                        CAPTURE(d);
                        CAPTURE(i);
                        CHECK(now <= authored + raise + 1e-3f);
                        CHECK(b.fovDegrees == doctest::Approx(a.fovDegrees));
                        if (now - authored > worst) {
                            worst = now - authored;
                            worstRaise = raise;
                        }
                    }
                }
            }
        }
    }
    MESSAGE("big targets: " << fixed << " shots fixed, " << failed << " with no fix; the farthest a fix moved the camera from "
            "the authored look-at: " << worst << " m (raise " << worstRaise << " m)");
    CHECK(fixed > 0);
}

TEST_CASE("special cine: the near keys, the look-at's own end anchor, and the stage's travel") {
    // The table's near keys: both or neither, a file of its own, a range in 0.1..50 m.
    SpecialCineTable t;
    std::string error;
    REQUIRE(importSpecialCineTable(R"({"moves": [{"id": "m", "shots": "f", "near": "g", "near_within": 4}]})", t, &error));
    REQUIRE(t.find("m") != nullptr);
    CHECK(t.find("m")->nearFile() == "g");
    CHECK(t.find("m")->nearWithin() == 4.f);
    CHECK(t.nearMove(0).shotFile() == "g");
    CHECK(t.nearMove(0).nearWithin() == 0.f);
    // The near copy has no shots until its file is read: the far set plays at any range till then.
    CHECK(t.find("m", 1.f) == t.find("m"));
    CHECK_FALSE(importSpecialCineTable(R"({"moves": [{"id": "m", "shots": "f", "near": "g"}]})", t, &error));
    CHECK(error.find("near") != std::string::npos);
    CHECK_FALSE(importSpecialCineTable(R"({"moves": [{"id": "m", "shots": "f", "near_within": 4}]})", t, &error));
    CHECK_FALSE(importSpecialCineTable(R"({"moves": [{"id": "m", "shots": "f", "near": "f", "near_within": 4}]})", t, &error));
    CHECK_FALSE(importSpecialCineTable(R"({"moves": [{"id": "m", "shots": "f", "near": "g", "near_within": 0}]})", t, &error));
    CHECK_FALSE(importSpecialCineTable(R"({"moves": [{"id": "m", "shots": "f", "near": "g h", "near_within": 4}]})", t, &error));
    REQUIRE(importSpecialCineTable(R"({"moves": [{"id": "m", "shots": "f"}]})", t, &error));
    CHECK(t.find("m")->nearFile().empty());
    CHECK(t.find("m", 0.5f) == t.find("m"));

    // "look_anchor_to": the look-at starts from its own anchor and ends from the target's, eased between in the world.
    SpecialCineMove m;
    REQUIRE(m.setId("m"));
    std::string text = kShots;
    const std::string look = R"("look_anchor": "lead", "look_at": {"from": [0, 0, 1]}, "ease": "linear")";
    const std::size_t at = text.find(look);
    REQUIRE(at != std::string::npos);
    text.replace(at, look.size(),
                 R"("look_anchor": "lead", "look_at": {"from": [0.5, 0, 1], "to": [-0.2, 0, 1]}, "look_anchor_to": "target", "ease": "linear")");
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importSpecialCineShots(text, m, &error, &warnings), error);
    CHECK(warnings.empty());
    CHECK(m.shot(0).lookToAnchor() == SpecialCineAnchor::Target);
    CHECK(m.shot(1).lookToAnchor() == SpecialCineAnchor::Lead); // not given: its look_anchor
    SpecialCineStage st = stageAt(0.f, 0.f, 6.f, 0.f);
    st.targetRadius = 0.5f;
    const CutsceneVec3 a = specialCinePoint(st, SpecialCineAnchor::Lead, {0.5f, 0.f, 1.f});
    const CutsceneVec3 b = specialCinePoint(st, SpecialCineAnchor::Target, {-0.2f, 0.f, 1.f});
    CHECK(near(b.x, 5.3f, 1e-4f)); // 0.2 m short of the target's front, pushed out by its radius
    CHECK(near(specialCineFrameAt(m, 0.f, st).pose.lookAt.x, a.x, 1e-3f));
    CHECK(near(specialCineFrameAt(m, 0.5f, st).pose.lookAt.x, 0.5f * (a.x + b.x), 1e-3f));
    CHECK(near(specialCineFrameAt(m, 0.999f, st).pose.lookAt.x, b.x, 0.01f));
    // The same shot on a nearer stage ends on the nearer target.
    st = stageAt(0.f, 0.f, 2.f, 0.f);
    st.targetRadius = 0.5f;
    CHECK(near(specialCineFrameAt(m, 0.999f, st).pose.lookAt.x, 1.3f, 0.01f));
    // A bone end needs a follow bone; an unknown anchor is refused.
    std::string boneEnd = text;
    boneEnd.replace(boneEnd.find(R"("look_anchor_to": "target")"), 26, R"("look_anchor_to": "bone")");
    CHECK_FALSE(importSpecialCineShots(boneEnd, m, &error));
    std::string unknown = text;
    unknown.replace(unknown.find(R"("look_anchor_to": "target")"), 26, R"("look_anchor_to": "boss")");
    CHECK_FALSE(importSpecialCineShots(unknown, m, &error));

    // The stage's travel: the share of end_feet the lead walks, as the framing check sees it. A long lens on the
    // lead's end feet (1 m ahead, kShots's end_feet) from 3 m to the side: the full walk is in frame, none of it is not.
    SpecialCineMove side = oneShot({1.f, -3.f, 1.f}, {1.f, 0.f, 1.f}, SpecialCineSubject::Lead);
    side.setEndFeet({1.f, 0.f, 0.f});
    SpecialCineStage walk = stageAt(0.f, 0.f, 6.f, 0.f);
    walk.travel = 1.f;
    CHECK(specialCineFrameProblem(side, 0.999f, walk, nullptr, nullptr, 0) == SpecialCineProblem::None);
    walk.travel = std::numeric_limits<float>::quiet_NaN(); // NaN: the whole walk
    CHECK(specialCineFrameProblem(side, 0.999f, walk, nullptr, nullptr, 0) == SpecialCineProblem::None);
    walk.travel = 0.f;
    SpecialCineMove narrow = side;
    SpecialCineShot s = side.shot(0);
    s.lensFrom = s.lensTo = 200.f; // about a 10 degree view: the lead 1 m back is out of it
    narrow.clearShots();
    REQUIRE(narrow.add(s));
    narrow.setEndFeet({1.f, 0.f, 0.f}); // clearShots clears the travel too
    CHECK(specialCineFrameProblem(narrow, 0.999f, walk, nullptr, nullptr, 0) == SpecialCineProblem::OffFrame);
    walk.travel = 1.f;
    CHECK(specialCineFrameProblem(narrow, 0.999f, walk, nullptr, nullptr, 0) != SpecialCineProblem::OffFrame);
}

TEST_CASE("special cine: the framing check: in frame, too small, behind; a mirror off a side body or wall; the target's own body") {
    SpecialCineStage st = stageAt(0.f, 0.f, 5.f, 0.f);
    st.targetRadius = 0.45f;
    st.targetHeight = 1.8f;
    const SpecialCineBody target = bodyOf(st);

    // A full figure 4 m off at 35 mm: in frame. 40 m off: under 12% of the frame. Looking away: off frame.
    SpecialCineMove m = oneShot({-4.f, 0.f, 1.2f}, {0.f, 0.f, 1.f}, SpecialCineSubject::Lead);
    CHECK(specialCineFrameProblem(m, 0.5f, st, nullptr, nullptr, 0) == SpecialCineProblem::None);
    m = oneShot({-40.f, 0.f, 1.2f}, {0.f, 0.f, 1.f}, SpecialCineSubject::Lead);
    CHECK(specialCineFrameProblem(m, 0.5f, st, nullptr, nullptr, 0) == SpecialCineProblem::TooSmall);
    m = oneShot({-4.f, 0.f, 1.2f}, {-8.f, 0.f, 1.f}, SpecialCineSubject::Lead);
    CHECK(specialCineFrameProblem(m, 0.5f, st, nullptr, nullptr, 0) == SpecialCineProblem::OffFrame);
    // Off to the side, past the safe frame (10% in from the edge): off frame though the lead is just on screen.
    m = oneShot({-4.f, 0.f, 1.2f}, {0.f, 1.85f, 1.f}, SpecialCineSubject::Lead);
    CHECK(specialCineFrameProblem(m, 0.5f, st, nullptr, nullptr, 0) == SpecialCineProblem::OffFrame);
    // "none": nothing to frame (a ground shot), only the bodies and the world.
    m = oneShot({0.f, 0.f, 8.f}, {0.01f, 0.f, 0.f}, SpecialCineSubject::None);
    CHECK(specialCineFrameProblem(m, 0.5f, st, nullptr, nullptr, 0) == SpecialCineProblem::None);

    // A body off the axis between the camera and the lead: the mirror clears it, before any raise or pull.
    m = oneShot({-3.f, 1.5f, 1.5f}, {0.f, 0.f, 1.f}, SpecialCineSubject::Lead);
    SpecialCineBody side;
    side.feet = {-1.5f, 0.75f, 0.f};
    side.radius = 0.3f;
    CHECK(specialCineFrameProblem(m, 0.5f, st, nullptr, &side, 1) == SpecialCineProblem::BodyInLine);
    SpecialCineShotCheck c = specialCineFixShot(m, 0, st, &side, 1);
    CHECK(c.ok);
    CHECK(c.first == SpecialCineProblem::BodyInLine);
    CHECK(c.fix.mirror);
    CHECK(c.fix.raise == 0.f);
    CHECK(c.tries == 2);
    // The same with a wall on the right instead of a body.
    c = specialCineFixShot(m, 0, st, nullptr, 0, rightWall, nullptr);
    CHECK(c.first == SpecialCineProblem::World);
    CHECK(c.fix.mirror);
    // Inside a body (its margin included).
    m = oneShot({4.9f, 0.5f, 1.f}, {0.f, 0.f, 1.f}, SpecialCineSubject::Lead);
    CHECK(specialCineFrameProblem(m, 0.5f, st, nullptr, &target, 1) == SpecialCineProblem::InsideBody);

    // Beyond the target looking back: the target stands in the line to the lead, but not in its own; its contact,
    // though, is on its side toward the lead, behind it from here: every other check passes and the contact fails it.
    m = oneShot({8.f, 0.f, 1.2f}, {0.f, 0.f, 1.f}, SpecialCineSubject::Lead);
    CHECK(specialCineFrameProblem(m, 0.5f, st, nullptr, &target, 1) == SpecialCineProblem::BodyInLine);
    m = oneShot({8.f, 0.f, 1.2f}, {5.f, 0.f, 1.f}, SpecialCineSubject::Target);
    CHECK(specialCineFrameProblem(m, 0.5f, st, nullptr, &target, 1) == SpecialCineProblem::ContactHidden);
    // From the lead's side the same target shot sees its contact.
    m = oneShot({2.f, 0.f, 1.2f}, {5.f, 0.f, 1.f}, SpecialCineSubject::Target);
    CHECK(specialCineFrameProblem(m, 0.5f, st, nullptr, &target, 1) == SpecialCineProblem::None);
    m = oneShot({8.f, 0.f, 1.2f}, {5.f, 0.f, 1.f}, SpecialCineSubject::Target);
    SpecialCineBody other = target;
    other.target = false; // the same body as a bystander: it is in the line
    CHECK(specialCineFrameProblem(m, 0.5f, st, nullptr, &other, 1) == SpecialCineProblem::BodyInLine);

    // The check and the search allocate nothing.
    m = oneShot({-3.f, 1.5f, 1.5f}, {0.f, 0.f, 1.f}, SpecialCineSubject::Lead);
    const auto before = ftcore_test::allocStats().count;
    c = specialCineFixShot(m, 0, st, &side, 1, rightWall, nullptr);
    if (ftcore_test::allocHookInstalled()) CHECK(ftcore_test::allocStats().count == before);
}

TEST_CASE("special cine: a shot's subject: read, defaulted from its look-at, refused when unknown; the aim looks at it") {
    SpecialCineMove m;
    REQUIRE(m.setId("m"));
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE(importSpecialCineShots(kShots, m, &error, &warnings));
    CHECK(warnings.empty());
    CHECK(m.shot(0).subject == SpecialCineSubject::Lead);
    std::string text = kShots;
    const std::string name = R"("name": "b")";
    text.replace(text.find(name), name.size(), R"("name": "b", "subject": "both")");
    REQUIRE(importSpecialCineShots(text, m, &error, &warnings));
    CHECK(m.shot(1).subject == SpecialCineSubject::Both);
    CHECK(specialCineSubjectName(m.shot(1).subject) == "both");
    const std::string both = R"("subject": "both")";
    text.replace(text.find(both), both.size(), R"("subject": "boss")");
    CHECK_FALSE(importSpecialCineShots(text, m, &error, &warnings));
    CHECK(error.find("subject") != std::string::npos);
    // No subject and a target-anchored look-at: the target.
    text = kShots;
    const std::string look = R"("look_anchor": "lead", "look_at": {"from": [0, 0, 1]}, "focal_mm": 35)";
    text.replace(text.find(look), std::string(R"("look_anchor": "lead")").size(), R"("look_anchor": "target")");
    REQUIRE(importSpecialCineShots(text, m, &error, &warnings));
    CHECK(m.shot(1).subject == SpecialCineSubject::Target);

    // Aimed: the look-at is the subject's chest (the target's, 60% of its height), and the focus its distance.
    SpecialCineStage st = stageAt(0.f, 0.f, 5.f, 0.f);
    st.targetHeight = 2.f;
    SpecialCineShotFix fixes[SpecialCineMove::kMaxShots]{};
    fixes[1].aim = true;
    const SpecialCineFrame f = specialCineFrameAt(m, 1.5f, st, fixes);
    CHECK(near(f.pose.lookAt.x, 5.f, 1e-3f));
    CHECK(near(f.pose.lookAt.z, 1.2f, 1e-3f));
    CHECK(near(f.focus, length3(sub3(f.pose.lookAt, f.pose.position)), 1e-3f));
    CHECK(specialCineProblemName(SpecialCineProblem::OffFrame) == "off frame");
    CHECK(specialCineProblemName(SpecialCineProblem::ContactHidden) == "the contact hidden");
    CHECK(specialCineProblemName(SpecialCineProblem::LeadHidden) == "the lead hidden");
}

TEST_CASE("special cine: round 2b's far shot 6 rebuilt: the old checks passed it with the contact behind the target; the reverse angle frames it") {
    SpecialCineTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(loadCommitted(t, warnings, error), error);
    const SpecialCineMove* far = t.find("devin_special_01");
    REQUIRE(far != nullptr);
    // The press as the r2b_far log gives it: the lead's feet at (-3.89, -0.04) m, its walk to (-3.18, -0.24) m along
    // end_feet (travel share 1), so the stage's forward is about (0.9954, -0.0960); a brute 6.59 m ahead, 0.45 m wide,
    // 1.97 m tall. The other brute's and the partner's places are not in the log: the target is the one body here.
    SpecialCineStage st;
    st.origin = {-3.89f, -0.04f, 0.f};
    const float fx = 0.9954f, fy = -0.0960f;
    st.forward = {fx, fy, 0.f};
    st.target = {st.origin.x + 6.59f * fx, st.origin.y + 6.59f * fy, 0.f};
    st.targetRadius = 0.45f;
    st.targetHeight = 1.97f;
    st.travel = 1.f;
    st.bone = {st.origin.x, st.origin.y, 1.2f}; // the host's: the chest
    REQUIRE(specialCineDistance(st) == doctest::Approx(6.59f).epsilon(1e-3));
    REQUIRE(t.find("devin_special_01", specialCineDistance(st)) == far);
    const SpecialCineBody body = bodyOf(st);
    const std::uint32_t six = far->shotAt(7.16f);
    REQUIRE(far->shotAt(7.23f) == six);
    REQUIRE(far->shot(six).in == doctest::Approx(7.1f));
    REQUIRE(far->shot(six).out == doctest::Approx(7.35f));
    REQUIRE(far->shot(six).subject == SpecialCineSubject::Both);
    // How far past the target's centre the eye stands, along the stage's forward (the lead is on the minus side).
    const auto past = [&](const SpecialCineFrame& f) {
        return (f.pose.position.x - st.target.x) * fx + (f.pose.position.y - st.target.y) * fy;
    };

    // As written and as the host moved it (mirrored, try 2): at both stills every check before the contact passes
    // (the contact and lead checks are tested last, so ContactHidden says so), the camera stands past the target's
    // far side, and the target hides the contact.
    SpecialCineShotFix fixes[SpecialCineMove::kMaxShots]{};
    for (const bool mirrored : {false, true}) {
        fixes[six] = fixOf(mirrored, 0.f, false);
        for (const float at : {7.16f, 7.23f}) {
            CAPTURE(mirrored);
            CAPTURE(at);
            CHECK(specialCineFrameProblem(*far, at, st, fixes, &body, 1) == SpecialCineProblem::ContactHidden);
            CHECK(past(specialCineFrameAt(*far, at, st, fixes)) > st.targetRadius);
        }
    }

    // The search: the mirror, the raises, the aims, the pulls and every orbit to 60 degrees fail; the reverse angle,
    // over the lead's right shoulder, frames both at every sample but stands where shot 8 stands (round 2c's stills:
    // shot 6 read like shot 8), so it is refused, plain, aimed, raised and both, as is the reverse swung 5 degrees
    // (closer still to shot 8); mirrored and swung 5 degrees it frames both over the lead's left shoulder.
    const SpecialCineShotCheck c = specialCineFixShot(*far, six, st, &body, 1);
    CHECK(c.first == SpecialCineProblem::ContactHidden);
    CHECK(c.ok);
    CHECK(c.fix.orbit == doctest::Approx(SpecialCineFraming::kOrbitReverse + SpecialCineFraming::kReverseSwings[0]));
    CHECK(c.fix.mirror);
    CHECK_FALSE(c.fix.aim);
    CHECK(c.fix.raise == 0.f);
    CHECK(c.fix.pull == 0.f);
    CHECK(c.tries == 76); // as written, 36 of the old fixes, 32 orbits, 4 reverses, 2 swung, then swung and mirrored
    CHECK(c.alike == 6);  // the 4 reverses and the 2 swung as written
    // Row 4's pass: shot 6 as moved and shot 8 differ by 30 degrees or more about the lead, or 40% or more of distance.
    const std::uint32_t eight = far->shotAt(8.4f);
    REQUIRE(eight == six + 2);
    fixes[six] = c.fix;
    const SpecialCineShotView moved = specialCineShotView(*far, six, st, fixes);
    const SpecialCineShotView last = specialCineShotView(*far, eight, st);
    SpecialCineShotFix reversed[SpecialCineMove::kMaxShots]{};
    reversed[six].orbit = SpecialCineFraming::kOrbitReverse;
    const SpecialCineShotView before = specialCineShotView(*far, six, st, reversed);
    const auto turn = [](const SpecialCineShotView& a, const SpecialCineShotView& b) {
        const float d = std::fabs(a.azimuth - b.azimuth);
        return d > 180.f ? 360.f - d : d;
    };
    const auto apart = [](const SpecialCineShotView& a, const SpecialCineShotView& b) {
        return std::fabs(a.distance - b.distance) / std::max(a.distance, b.distance);
    };
    MESSAGE("far shot 6 reversed (round 2c): azimuth ", before.azimuth, " deg, ", before.distance, " m; shot 8: ", last.azimuth,
            " deg, ", last.distance, " m: ", turn(before, last), " deg and ", 100.f * apart(before, last), "% apart");
    MESSAGE("far shot 6 now: azimuth ", moved.azimuth, " deg, ", moved.distance, " m: ", turn(moved, last), " deg and ",
            100.f * apart(moved, last), "% from shot 8");
    CHECK(specialCineViewsAlike(before, last)); // what the stills showed
    CHECK(turn(before, last) < 10.f);
    CHECK_FALSE(specialCineViewsAlike(moved, last));
    CHECK((turn(moved, last) >= SpecialCineShotView::kAlikeAzimuth || apart(moved, last) >= SpecialCineShotView::kAlikeDistance));
    CHECK(turn(moved, last) >= 30.f);
    for (const float at : {7.11f, 7.16f, 7.23f, 7.34f}) {
        CAPTURE(at);
        CHECK(specialCineFrameProblem(*far, at, st, fixes, &body, 1) == SpecialCineProblem::None);
        CHECK(past(specialCineFrameAt(*far, at, st, fixes)) < -st.targetRadius); // on the lead's side now
    }
    // Every other shot that frames someone passes as written on this stage: shot 6 is the only one moved.
    for (std::uint32_t i = 0; i < far->size(); ++i) {
        if (i == six || far->shot(i).subject == SpecialCineSubject::None) continue;
        CAPTURE(i);
        CHECK(specialCineFixShot(*far, i, st, &body, 1).first == SpecialCineProblem::None);
    }
}

TEST_CASE("special cine: two views are alike under 30 degrees about the lead and 40% of distance; the turn wraps") {
    const auto view = [](float azimuth, float distance) {
        SpecialCineShotView v;
        v.azimuth = azimuth;
        v.distance = distance;
        return v;
    };
    CHECK(specialCineViewsAlike(view(10.f, 3.f), view(10.f, 3.f)));
    CHECK(specialCineViewsAlike(view(0.f, 3.f), view(29.f, 3.f)));
    CHECK_FALSE(specialCineViewsAlike(view(0.f, 3.f), view(30.f, 3.f)));
    CHECK_FALSE(specialCineViewsAlike(view(0.f, 3.f), view(-30.5f, 3.f)));
    CHECK(specialCineViewsAlike(view(0.f, 3.f), view(0.f, 1.85f)));      // 38% nearer
    CHECK_FALSE(specialCineViewsAlike(view(0.f, 3.f), view(0.f, 1.8f))); // 40% nearer
    CHECK_FALSE(specialCineViewsAlike(view(0.f, 2.f), view(0.f, 3.4f))); // 41% farther, measured on the farther
    CHECK(specialCineViewsAlike(view(170.f, 3.f), view(-170.f, 3.f)));   // 20 degrees across the back
    CHECK(specialCineViewsAlike(view(355.f, 3.f), view(5.f, 3.f)));
    CHECK_FALSE(specialCineViewsAlike(view(160.f, 3.f), view(-160.f, 3.f)));
    CHECK(specialCineViewsAlike(view(0.f, 0.f), view(10.f, 0.f))); // both on the lead: the turn decides
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(specialCineViewsAlike(view(nan, 3.f), view(0.f, 3.f)));
    CHECK_FALSE(specialCineViewsAlike(view(0.f, 3.f), view(0.f, nan)));
    // A shot out of range has no view, and so is like nothing.
    SpecialCineTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(loadCommitted(t, warnings, error), error);
    const SpecialCineMove* far = t.find("devin_special_01");
    REQUIRE(far != nullptr);
    const SpecialCineStage st = stageAt(0.f, 0.f, 6.6f, 0.f);
    const SpecialCineShotView none = specialCineShotView(*far, far->size(), st);
    CHECK(std::isnan(none.azimuth));
    CHECK(std::isnan(none.distance));
    CHECK_FALSE(specialCineViewsAlike(none, none));
    // The committed move's shots on a plain stage: every framing shot has a view, finite and off the lead.
    for (std::uint32_t i = 0; i < far->size(); ++i) {
        if (far->shot(i).subject == SpecialCineSubject::None) continue;
        CAPTURE(i);
        const SpecialCineShotView v = specialCineShotView(*far, i, st);
        CHECK(std::isfinite(v.azimuth));
        CHECK(v.distance > 0.5f);
    }
}

TEST_CASE("special cine: a clear two-shot passes untouched; a low body hides the lead's legs though its chest is in view") {
    SpecialCineStage st = stageAt(0.f, 0.f, 5.f, 0.f);
    st.targetRadius = 0.45f;
    st.targetHeight = 1.8f;
    const SpecialCineBody target = bodyOf(st);
    // From the side, 7 m out at 35 mm: both figures in frame, the contact (the target's side toward the lead) in view.
    const SpecialCineMove side = oneShot({2.5f, -7.f, 1.4f}, {2.5f, 0.f, 1.f}, SpecialCineSubject::Both);
    CHECK(specialCineFrameProblem(side, 0.5f, st, nullptr, &target, 1) == SpecialCineProblem::None);
    const SpecialCineShotCheck c = specialCineFixShot(side, 0, st, &target, 1);
    CHECK(c.first == SpecialCineProblem::None);
    CHECK(c.ok);
    CHECK(c.tries == 1);
    CHECK_FALSE(c.fix.moved());

    // A bystander 1 m tall, 1 m in front of the lead on the camera's line: the line to the lead's chest passes over
    // it (no body in the line), but it hides the lead's feet to its knees, 4 of its 11 points: under 75% seen.
    const SpecialCineMove lead = oneShot({-4.f, 0.f, 1.5f}, {0.f, 0.f, 1.f}, SpecialCineSubject::Lead);
    SpecialCineBody low;
    low.feet = {-1.f, 0.f, 0.f};
    low.radius = 0.3f;
    low.height = 1.f;
    CHECK(specialCineFrameProblem(lead, 0.5f, st, nullptr, &low, 1) == SpecialCineProblem::LeadHidden);
    const SpecialCineShotCheck h = specialCineFixShot(lead, 0, st, &low, 1);
    CHECK(h.first == SpecialCineProblem::LeadHidden);
    CHECK(h.ok);
    CHECK(h.fix.moved());
    SpecialCineShotFix fixes[SpecialCineMove::kMaxShots]{};
    fixes[0] = h.fix;
    CHECK(specialCineFrameProblem(lead, 0.5f, st, fixes, &low, 1) == SpecialCineProblem::None);
}

TEST_CASE("special cine: a shot no fix clears is counted as still failing, its fix left empty, and the search allocates nothing") {
    SpecialCineStage st = stageAt(0.f, 0.f, 5.f, 0.f);
    st.targetRadius = 0.45f;
    // The lead walled in: eight bystanders 4 m tall in a ring 1 m round it, touching. No raise sees over them, no
    // orbit finds a gap, and a pull that would clear them puts the camera inside one.
    SpecialCineBody ring[8];
    for (int k = 0; k < 8; ++k) {
        const float a = static_cast<float>(k) * 45.f * kDeg;
        ring[k].feet = {std::cos(a), std::sin(a), 0.f};
        ring[k].radius = 0.45f;
        ring[k].height = 4.f;
    }
    const SpecialCineMove m = oneShot({-4.f, 0.f, 1.2f}, {0.f, 0.f, 1.f}, SpecialCineSubject::Lead);
    const auto before = ftcore_test::allocStats().count;
    const SpecialCineShotCheck c = specialCineFixShot(m, 0, st, ring, 8);
    if (ftcore_test::allocHookInstalled()) CHECK(ftcore_test::allocStats().count == before);
    CHECK(c.first == SpecialCineProblem::BodyInLine);
    CHECK(c.last != SpecialCineProblem::None);
    CHECK_FALSE(c.ok);
    CHECK(c.tries == 153); // every fix tried: as written, 36 of the old fixes, 32 orbits, 4 reverses, 80 reverse swings
    CHECK(c.alike == 0);
    CHECK_FALSE(c.fix.moved());
    // Counted as the host counts it: one shot still failing.
    int stillFailing = 0;
    for (std::uint32_t i = 0; i < m.size(); ++i)
        if (!specialCineFixShot(m, i, st, ring, 8).ok) ++stillFailing;
    CHECK(stillFailing == 1);
}

TEST_CASE("special cine: the hit at any range: Core's impact time, the contact the framing tests, the shot and its still") {
    SpecialCineTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(loadCommitted(t, warnings, error), error);
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Battle/arena_special.json", text));
    ArenaSpecialTable specials;
    REQUIRE_MESSAGE(importArenaSpecials(text, specials, &error), error);
    const ArenaSpecialRules* rules = specials.forLead("devin");
    REQUIRE(rules != nullptr);
    const SpecialCineMove* far = t.find("devin_special_01");
    REQUIRE(far != nullptr);
    const SpecialCineMove& nearSet = t.nearMove(0);
    const float strike = rules->strikeAt, reach = rules->waveSpeed * rules->maxTravel;
    REQUIRE(near(far->release(), strike)); // the shots' release is Core's strike
    // The run that showed no effect: a brute (0.45 m, 1.97 m tall) 6.59 m off, full travel; the hit lands in shot 6.
    {
        SpecialCineStage st = stageAt(0.f, 0.f, 6.59f, 0.f);
        st.targetRadius = 0.45f;
        st.targetHeight = 1.97f;
        const float impact = arenaSpecialImpactAt(*rules, 6.59f);
        CHECK(near(impact, strike + 6.59f / rules->waveSpeed));
        const SpecialCineHit hit = specialCineHit(*far, st, impact, 0.45f);
        CHECK(near(hit.time, impact));
        CHECK(hit.shot == 5);
        CHECK(far->shotAt(hit.stillAt) == 5);
        CHECK(near(hit.at.x, 6.59f - 0.45f - SpecialCineFraming::kContactOut, 1e-3f));
        CHECK(std::fabs(hit.at.y) < 0.05f); // toward the feet, which end a little to the side
        CHECK(near(hit.at.z, std::min(0.6f * 1.8f, 0.6f * 1.97f)));
    }
    // Every 0.25 m from body contact to 30 m, both sets, a person and a big body: the time is the strike plus the
    // wave's travel (capped), the point on the target's surface toward the lead (kContactOut out, chest height capped
    // at the target's), in shot 6 while the wave is under 10 m and shot 7 past it; the still kStillLag after it.
    const auto before = ftcore_test::allocStats().count;
    int cases = 0;
    for (const float radius : {0.35f, 0.45f, 1.2f}) {
        const float height = radius > 1.f ? 3.5f : (radius < 0.4f ? 0.9f : 1.97f);
        for (float d = radius + 1.f; d <= 30.f + 1e-3f; d += 0.25f) {
            SpecialCineStage st = stageAt(0.f, 0.f, d, 0.f);
            st.targetRadius = radius;
            st.targetHeight = height;
            const SpecialCineMove& m = d < far->nearWithin() ? nearSet : *far;
            const float impact = arenaSpecialImpactAt(*rules, d);
            const SpecialCineHit hit = specialCineHit(m, st, impact, radius);
            ++cases;
            CHECK(near(impact, strike + std::min(d, reach) / rules->waveSpeed, 1e-4f));
            CHECK(near(hit.time, impact));
            const CutsceneVec3 toLead = sub3(hit.at, st.target);
            const float ground = std::sqrt(toLead.x * toLead.x + toLead.y * toLead.y);
            CHECK(near(ground, std::min(radius + SpecialCineFraming::kContactOut, d), 1e-3f));
            CHECK(toLead.x < 0.f); // on the lead's side
            CHECK(hit.at.z <= 0.6f * height + 1e-4f);
            CHECK(hit.shot == (impact < 7.35f ? 5u : 6u));
            CHECK(near(hit.stillAt, impact + SpecialCineHit::kStillLag));
            CHECK(m.shotAt(hit.stillAt) >= hit.shot);
        }
    }
    if (ftcore_test::allocHookInstalled()) CHECK(ftcore_test::allocStats().count == before);
    CHECK(cases > 300);
    // NaN: the release; past the move: its end.
    SpecialCineStage st = stageAt(0.f, 0.f, 6.f, 0.f);
    CHECK(near(specialCineHit(*far, st, std::numeric_limits<float>::quiet_NaN()).time, far->release()));
    CHECK(near(specialCineHit(*far, st, 99.f).time, far->length()));
}

TEST_CASE("special cine: the partner's strike point against the contact line at the hit") {
    SpecialCineTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(loadCommitted(t, warnings, error), error);
    const SpecialCineMove* far = t.find("devin_special_01");
    REQUIRE(far != nullptr);
    // r2b_far: a brute (0.45 m, 1.97 m) 6.59 m off; the hit at Core's impact.
    SpecialCineStage st = stageAt(0.f, 0.f, 6.59f, 0.f);
    st.targetRadius = 0.45f;
    st.targetHeight = 1.97f;
    const SpecialCineHit hit = specialCineHit(*far, st, 7.1f + 6.59f / 40.f, 0.45f);
    const float chest = 0.6f * 1.8f;
    const auto before = ftcore_test::allocStats().count;
    // A partner on the lead's side of the target, at any range: its blow lands on the surface toward it, near the
    // contact; within the cone whose surface point stays 0.15 m off the line, on it.
    for (const float range : {1.f, 2.f, 3.5f, 6.f}) {
        for (float deg = -60.f; deg <= 60.f + 1e-3f; deg += 5.f) {
            const float a = (180.f + deg) * 3.14159265f / 180.f;
            const CutsceneVec3 partner{6.59f + range * std::cos(a), range * std::sin(a), chest};
            const SpecialCinePartnerStrike s = specialCinePartnerStrike(*far, st, hit, partner, 0.45f);
            CHECK(near(std::sqrt((s.at.x - 6.59f) * (s.at.x - 6.59f) + s.at.y * s.at.y), 0.45f, 1e-3f));
            CHECK(near(s.at.z, chest));
            // The contact line runs from the lead (about 6 m back) to the contact, near the target's axis: the strike
            // point's distance from it is the surface chord, about radius x sin(angle).
            const float expect = 0.45f * std::sin(std::fabs(deg) * 3.14159265f / 180.f);
            CHECK(std::fabs(s.offLine - expect) < 0.06f);
            if (std::fabs(deg) <= 10.f) CHECK(s.onLine());
            if (std::fabs(deg) >= 30.f) CHECK_FALSE(s.onLine());
            CHECK(s.along > 0.9f);
        }
    }
    // Beside the target, square to the lead's way: the blow is the radius off the line.
    const SpecialCinePartnerStrike side = specialCinePartnerStrike(*far, st, hit, CutsceneVec3{6.59f, 2.f, chest}, 0.45f);
    CHECK(near(side.offLine, 0.45f, 0.06f));
    CHECK_FALSE(side.onLine());
    // Standing in the target (no direction): its centre, about on the line.
    const SpecialCinePartnerStrike inside = specialCinePartnerStrike(*far, st, hit, CutsceneVec3{6.59f, 0.f, chest}, 0.45f);
    CHECK(inside.offLine < SpecialCinePartnerStrike::kOnLine);
    if (ftcore_test::allocHookInstalled()) CHECK(ftcore_test::allocStats().count == before);
}
