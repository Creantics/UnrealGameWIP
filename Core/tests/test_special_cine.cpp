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
CutsceneVec3 add3(const CutsceneVec3& a, const CutsceneVec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
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

// Whether `p` stands inside `h` of the half-frame each way, in front of the lens, by the test's own sums.
bool inFrameBox(const SpecialCineFrame& f, const CutsceneVec3& p, float h);

// Whether `p` stands inside the frame's safe box (SpecialCineFraming::kSafe of the half-frame), by its own sums.
bool inSafeFrame(const SpecialCineFrame& f, const CutsceneVec3& p) { return inFrameBox(f, p, SpecialCineFraming::kSafe); }

bool inFrameBox(const SpecialCineFrame& f, const CutsceneVec3& p, float h) {
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
    return std::fabs(x) <= h && std::fabs(y) <= h;
}

// The lead's head and striking hand (kHandReach from its chest toward `contact`) inside kInFrame, by the test's sums.
bool leadInFrame(const SpecialCineFrame& f, const CutsceneVec3& feet, float height, const CutsceneVec3& contact) {
    const CutsceneVec3 head{feet.x, feet.y, feet.z + SpecialCineFraming::kHeadHeight * height};
    const CutsceneVec3 chest{feet.x, feet.y, feet.z + 0.6f * height};
    const CutsceneVec3 d = sub3(contact, chest);
    const float n = length3(d);
    const float k = n > SpecialCineFraming::kHandReach ? SpecialCineFraming::kHandReach / n : 1.f;
    const CutsceneVec3 hand{chest.x + d.x * k, chest.y + d.y * k, chest.z + d.z * k};
    return inFrameBox(f, head, SpecialCineFraming::kInFrame) && inFrameBox(f, hand, SpecialCineFraming::kInFrame);
}

// The same for a posed lead (its head, and its measured fist, else its hand kHandReach from its chest toward `contact`).
bool poseInFrame(const SpecialCineFrame& f, const SpecialCineLeadPose& p, const CutsceneVec3& contact) {
    const CutsceneVec3 d = sub3(contact, p.chest);
    const float n = length3(d);
    const float k = n > SpecialCineFraming::kHandReach ? SpecialCineFraming::kHandReach / n : 1.f;
    const CutsceneVec3 model{p.chest.x + d.x * k, p.chest.y + d.y * k, p.chest.z + d.z * k};
    return inFrameBox(f, p.head, SpecialCineFraming::kInFrame) && inFrameBox(f, p.fist ? p.hand : model, SpecialCineFraming::kInFrame);
}

// The least distance from the segment `eye`..`to` to the lead's `hips`..`head` line, by sampling both (an upper bound
// on the exact one, within half a step).
float sampledGap(const CutsceneVec3& eye, const CutsceneVec3& to, const CutsceneVec3& hips, const CutsceneVec3& head) {
    float best = 1e9f;
    const CutsceneVec3 a = sub3(to, eye), b = sub3(head, hips);
    for (int i = 0; i <= 200; ++i) {
        const float u = static_cast<float>(i) / 200.f;
        const CutsceneVec3 q{eye.x + a.x * u, eye.y + a.y * u, eye.z + a.z * u};
        for (int j = 0; j <= 40; ++j) {
            const float w = static_cast<float>(j) / 40.f;
            best = std::min(best, length3(sub3(q, {hips.x + b.x * w, hips.y + b.y * w, hips.z + b.z * w})));
        }
    }
    return best;
}

// The sight from `eye` to `contact`, its last kTorso metres left out (a lead pressed into its blow holds its chest that
// near), against the lead's line, by the test's sums.
float stoppedSightGap(const CutsceneVec3& eye, const CutsceneVec3& contact, const CutsceneVec3& hips, const CutsceneVec3& head) {
    const CutsceneVec3 full = sub3(contact, eye);
    const float n = length3(full), keep = n > SpecialCineFraming::kTorso ? (n - SpecialCineFraming::kTorso) / n : 0.f;
    return sampledGap(eye, {eye.x + full.x * keep, eye.y + full.y * keep, eye.z + full.z * keep}, hips, head);
}

// The camera's sight to `contact` against the lead's hips-to-head line (stoppedSightGap).
float torsoGap(const SpecialCineFrame& f, const CutsceneVec3& contact, const SpecialCineLeadPose& p) {
    return stoppedSightGap(f.pose.position, contact, p.hips, p.head);
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
// written, with a fix, or skipped under a cover that passes (the host's two passes); `reframed` counts the shots moved
// for the contact or the lead alone, `skipped` the shots covered (`skippedShot` the last).
void framedOnStage(const SpecialCineMove& m, const SpecialCineStage& st, bool& oldOk, bool& fixedOk, int& reframed,
                   std::uint32_t& failedShot, int& skipped, std::uint32_t& skippedShot) {
    const SpecialCineBody body = bodyOf(st);
    oldOk = fixedOk = true;
    SpecialCineShotFix fixes[SpecialCineMove::kMaxShots]{};
    bool blocked[SpecialCineMove::kMaxShots]{};
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
        fixes[i] = c.fix;
        blocked[i] = !c.ok;
    }
    for (std::uint32_t i = 0; i < m.size(); ++i) {
        if (!blocked[i]) continue;
        const SpecialCineCover cover = specialCineCoverShot(m, i, st, fixes, &body, 1);
        if (cover == SpecialCineCover::None) {
            fixedOk = false;
            failedShot = i;
            continue;
        }
        fixes[i].cover = cover;
        ++skipped;
        skippedShot = i;
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
    int kinds = 0, reframed = 0, skips = 0;
    float firstSkip = 1e9f;
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
            std::uint32_t failed = 99, skippedShot = 99;
            bool farOk = true, farFixed = true;
            int moved = 0, skipped = 0;
            framedOnStage(*far, st, farOk, farFixed, moved, failed, skipped, skippedShot);
            if (!farOk) lastFarFail = std::max(lastFarFail, d);
            if (d >= within) {
                CHECK(picked == far);
                CAPTURE(failed);
                CHECK(farOk);
                reframed += moved;
                // No shot is left failing: each is framed as written, moved, or skipped under a neighbour that frames
                // over its window.
                CHECK(farFixed);
                if (skipped > 0) {
                    // Measured: only the smallest kind (0.9 m tall) from 13.35 m, only shot 6. From beyond it the
                    // contact is hidden; from over the lead's shoulder, even pulled 2 m in, it is under the minimum
                    // figure. Shot 7 starts early over its window (the cut list logs it).
                    skips += skipped;
                    firstSkip = std::min(firstSkip, d);
                    CHECK(skipped == 1);
                    CHECK(arenaEnemyHeight(kind) < 1.f);
                    CHECK(d > 13.3f);
                    CHECK(skippedShot == 5);
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
    // (but for the smallest kind far out, pulled in or skipped, above).
    CHECK(reframed > 0);
    MESSAGE("the far set past the switch point: " << reframed << " shots reframed for a hidden contact or lead; "
            << skips << " skipped under a cover, from " << firstSkip << " m; none left failing");
    CHECK(skips > 0);
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

TEST_CASE("special cine: the shot shown at the hit passes only with the lead's head and striking hand in frame") {
    // The lead at the origin (1.8 m), a target 4 m off; the hit at 0.5 s of a one-shot move.
    SpecialCineStage st = stageAt(0.f, 0.f, 4.f, 0.f);
    st.targetRadius = 0.45f;
    // Wide and side on: both whole in frame.
    const SpecialCineMove wide = oneShot({2.f, -6.f, 1.2f}, {2.f, 0.f, 0.9f}, SpecialCineSubject::Both);
    const SpecialCineHit wideHit = specialCineHit(wide, st, 0.5f, 0.45f);
    CHECK(specialCineFrameProblem(wide, 0.5f, st, nullptr, nullptr, 0) == SpecialCineProblem::None);
    CHECK(specialCineHitProblem(wide, st, wideHit) == SpecialCineProblem::None);
    // Low behind the lead's shoulder, level at the target's knees: the lead's legs fill the frame's side and its head
    // and hand stand above the top edge (the round 4 r2b_big still). The frame passes; the hit in it does not.
    const SpecialCineMove low = oneShot({-1.f, -0.3f, 0.5f}, {4.f, 0.f, 0.3f}, SpecialCineSubject::Both);
    const SpecialCineHit hit = specialCineHit(low, st, 0.5f, 0.45f);
    CHECK(specialCineFrameProblem(low, 0.5f, st, nullptr, nullptr, 0) == SpecialCineProblem::None);
    CHECK(specialCineHitProblem(low, st, hit) == SpecialCineProblem::LeadCropped);
    CHECK(std::string(specialCineProblemName(SpecialCineProblem::LeadCropped)) == "the lead cropped");
    // Without the hit the fix search keeps the shot as written; with it, it moves the shot until the lead reads, by the
    // test's own sums, at the hit and at every still.
    CHECK(specialCineFixShot(low, 0, st, nullptr, 0).first == SpecialCineProblem::None);
    const SpecialCineShotCheck c = specialCineFixShot(low, 0, st, nullptr, 0, nullptr, nullptr, &hit);
    CHECK(c.first == SpecialCineProblem::LeadCropped);
    CHECK(near(c.firstAt, hit.time));
    REQUIRE(c.ok);
    CHECK(specialCineHitProblem(low, st, hit, &c.fix) == SpecialCineProblem::None);
    float stills[SpecialCineHit::kStills] = {};
    const std::uint32_t n = specialCineHitStills(low, hit, stills, SpecialCineHit::kStills);
    CHECK(n >= 3);
    CHECK(leadInFrame(specialCineFrameAt(low, hit.time, st, &c.fix), {0.f, 0.f, 0.f}, st.leadHeight, hit.at));
    for (std::uint32_t k = 0; k < n; ++k) {
        CAPTURE(stills[k]);
        CHECK(leadInFrame(specialCineFrameAt(low, stills[k], st, &c.fix), {0.f, 0.f, 0.f}, st.leadHeight, hit.at));
        CHECK(inSafeFrame(specialCineFrameAt(low, stills[k], st, &c.fix), hit.at));
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
    CHECK(c.tries == 185); // every fix tried: as written, 36 of the old fixes, 32 orbits, 4 reverses, 80 reverse swings,
                           // 32 reverses pulled in
    CHECK(c.alike == 0);
    CHECK_FALSE(c.fix.moved());
    // A move of one shot has no neighbour to cover it: played as written.
    CHECK(specialCineCoverShot(m, 0, st, nullptr, ring, 8) == SpecialCineCover::None);
    if (ftcore_test::allocHookInstalled()) CHECK(ftcore_test::allocStats().count == before);
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
    // The partner's angle round the target's axis from the contact (which faces the lead's feet at the hit).
    const auto bearingOf = [](const SpecialCineHit& h, const SpecialCineStage& sg, const CutsceneVec3& p) {
        const float cx = h.at.x - sg.target.x, cy = h.at.y - sg.target.y, px = p.x - sg.target.x, py = p.y - sg.target.y;
        return std::acos(std::clamp((cx * px + cy * py) / (std::sqrt(cx * cx + cy * cy) * std::sqrt(px * px + py * py)), -1.f,
                                    1.f)) * 180.f / 3.14159265f;
    };
    const auto before = ftcore_test::allocStats().count;
    // A partner on the lead's side of the target, at any range: its blow lands on the surface toward it, near the
    // contact; within the cone whose surface point stays 0.15 m off the line, on it.
    for (const float range : {1.f, 2.f, 3.5f, 6.f}) {
        for (float deg = -60.f; deg <= 60.f + 1e-3f; deg += 5.f) {
            const float a = (180.f + deg) * 3.14159265f / 180.f;
            const CutsceneVec3 partner{6.59f + range * std::cos(a), range * std::sin(a), chest};
            const SpecialCinePartnerStrike s = specialCinePartnerStrike(*far, st, hit, partner, 0.45f);
            // The lead's contact rule: kContactOut off the surface, at the partner's chest (under the brute's).
            CHECK(near(std::sqrt((s.at.x - 6.59f) * (s.at.x - 6.59f) + s.at.y * s.at.y), 0.55f, 1e-3f));
            CHECK(near(s.at.z, chest));
            CHECK(near(s.bearing, bearingOf(hit, st, partner), 0.5f));
            CHECK(near(s.bearing, std::fabs(deg), 1.5f));
            // The contact line runs from the lead (about 6 m back) to the contact, near the target's axis: the strike
            // point's distance from it is the chord, about (radius + kContactOut) x sin(bearing).
            const float expect = 0.55f * std::sin(std::fabs(deg) * 3.14159265f / 180.f);
            CHECK(std::fabs(s.offLine - expect) < 0.06f);
            if (std::fabs(deg) <= 10.f) CHECK(s.onLine());
            if (std::fabs(deg) >= 30.f) CHECK_FALSE(s.onLine());
            CHECK(s.along > 0.9f);
        }
    }
    // Beside the target, square to the lead's way: the blow is the radius and the fist's depth off the line.
    const SpecialCinePartnerStrike side = specialCinePartnerStrike(*far, st, hit, CutsceneVec3{6.59f, 2.f, chest}, 0.45f);
    CHECK(near(side.offLine, 0.55f, 0.06f));
    CHECK(near(side.bearing, 90.f, 1.f));
    CHECK_FALSE(side.onLine());
    // Standing in the target (no direction): its centre, about on the line.
    const SpecialCinePartnerStrike inside = specialCinePartnerStrike(*far, st, hit, CutsceneVec3{6.59f, 0.f, chest}, 0.45f);
    CHECK(inside.offLine < SpecialCinePartnerStrike::kOnLine);
    if (ftcore_test::allocHookInstalled()) CHECK(ftcore_test::allocStats().count == before);
}

TEST_CASE("special cine: the smallest kind far out: shot 6 pulled in, past that skipped under shot 7; never a failing shot shown") {
    SpecialCineTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(loadCommitted(t, warnings, error), error);
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Battle/arena.json", text));
    ArenaTable arena;
    REQUIRE_MESSAGE(importArenaTable(text, arena, &error), error);
    const ArenaFighter& lead = arena.fighter("devin");
    const ArenaEnemyKind* pup = nullptr;
    for (std::size_t k = 0; k < arena.enemies.size(); ++k)
        if (arena.enemyIds[k] == "maw_pup") pup = &arena.enemies[k];
    REQUIRE(pup != nullptr);
    REQUIRE(arenaEnemyHeight(*pup) < 1.f);
    const SpecialCineMove* far = t.find("devin_special_01");
    REQUIRE(far != nullptr);
    const std::uint32_t six = far->shotAt(7.16f), seven = six + 1, eight = six + 2;
    REQUIRE(six == 5);
    const SpecialCineShot& s6 = far->shot(six);
    const CutsceneVec3 end = far->endFeet();
    const float walk = std::sqrt(end.x * end.x + end.y * end.y);
    const float contact = lead.bodyRadius + pup->bodyRadius;
    const float length = far->length();
    int pulled = 0, skipped = 0;
    float firstPulled = 1e9f, lastPulled = -1.f, firstSkip = 1e9f, leastTurn = 360.f, worst = -1e9f;
    for (int k = 0;; ++k) {
        const float d = 12.35f + 0.05f * static_cast<float>(k);
        if (d > lead.lockRange + 1e-3f) break;
        CAPTURE(d);
        SpecialCineStage st = stageAt(0.f, 0.f, d, 0.f);
        st.targetRadius = pup->bodyRadius;
        st.targetHeight = arenaEnemyHeight(*pup);
        st.travel = travelAt(d, contact, walk);
        st.bone = {0.3f, 0.25f, 1.1f};
        REQUIRE(t.find("devin_special_01", specialCineDistance(st)) == far);
        const SpecialCineBody body = bodyOf(st);
        // The host's two passes: every shot's fix, then the cover for a shot none frames.
        const auto before = ftcore_test::allocStats().count;
        SpecialCineShotFix fixes[SpecialCineMove::kMaxShots]{};
        SpecialCineShotCheck c;
        bool othersOk = true;
        for (std::uint32_t i = 0; i < far->size(); ++i) {
            if (far->shot(i).subject == SpecialCineSubject::None) continue;
            const SpecialCineShotCheck ci = specialCineFixShot(*far, i, st, &body, 1);
            if (i != six && !ci.ok) othersOk = false;
            fixes[i] = ci.fix;
            if (i == six) c = ci;
        }
        const SpecialCineCover cover = c.ok ? SpecialCineCover::None : specialCineCoverShot(*far, six, st, fixes, &body, 1);
        SpecialCineCut cuts[SpecialCineMove::kMaxShots];
        if (!c.ok) fixes[six].cover = cover;
        const std::uint32_t n = specialCineCutList(*far, fixes, cuts, SpecialCineMove::kMaxShots);
        if (ftcore_test::allocHookInstalled()) CHECK(ftcore_test::allocStats().count == before);
        CHECK(othersOk);
        if (c.ok) {
            // Framed by a fix up to 13.3 m. From 12.4 m it is the reverse pulled in, mirrored, off shot 8 by 30 degrees
            // or more, and no farther from its look-at than as written (the cap).
            CHECK(d < 13.32f);
            if (c.fix.pull > 0.f) {
                ++pulled;
                firstPulled = std::min(firstPulled, d);
                lastPulled = std::max(lastPulled, d);
                CHECK(c.fix.orbit == doctest::Approx(SpecialCineFraming::kOrbitReverse));
                CHECK(c.fix.mirror);
                const SpecialCineShotView mine = specialCineShotView(*far, six, st, fixes);
                const SpecialCineShotView last = specialCineShotView(*far, eight, st);
                CHECK_FALSE(specialCineViewsAlike(mine, last));
                float turn = std::fabs(mine.azimuth - last.azimuth);
                if (turn > 180.f) turn = 360.f - turn;
                CHECK(turn >= SpecialCineShotView::kAlikeAzimuth);
                leastTurn = std::min(leastTurn, turn);
                SpecialCineShotFix written[SpecialCineMove::kMaxShots]{};
                written[six].mirror = c.fix.mirror;
                written[six].orbit = c.fix.orbit;
                for (int q = 0; q <= 4; ++q) {
                    const float at = s6.in + (s6.out - s6.in) * (0.02f + 0.96f * static_cast<float>(q) / 4.f);
                    const SpecialCineFrame a = specialCineFrameAt(*far, at, st, written);
                    const SpecialCineFrame b = specialCineFrameAt(*far, at, st, fixes);
                    const float authored = length3(sub3(a.pose.lookAt, a.pose.position));
                    const float now = length3(sub3(a.pose.lookAt, b.pose.position));
                    const float raise = std::isfinite(c.fix.raise) && c.fix.raise > 0.f ? c.fix.raise : 0.f;
                    CHECK(now <= authored + raise + 1e-3f);
                    worst = std::max(worst, now - authored - raise);
                }
            }
            CHECK(n == far->size());
            continue;
        }
        // Past 13.3 m no fix inside the cap frames it: skipped, shot 7 starting early over its window.
        ++skipped;
        firstSkip = std::min(firstSkip, d);
        CHECK(d > 13.3f);
        CHECK(c.last != SpecialCineProblem::None); // the last try, pulled 2 m, ends in the lead
        REQUIRE(cover == SpecialCineCover::Next);
        CHECK(specialCineShownShot(*far, six, fixes) == seven);
        CHECK(fixes[six].moved());
        // Every frame over the window, at 120 per second: shot 7 shown, passing every check.
        int frames = 0, failing = 0, notSeven = 0;
        for (int q = 0;; ++q) {
            const float at = s6.in + static_cast<float>(q) / 120.f;
            if (at >= s6.out) break;
            ++frames;
            if (specialCineFrameAt(*far, at, st, fixes).shot != seven) ++notSeven;
            if (specialCineFrameProblem(*far, at, st, fixes, &body, 1) != SpecialCineProblem::None) ++failing;
        }
        CHECK(frames >= 29);
        CHECK(notSeven == 0);
        CHECK(failing == 0);
        // The cut list: one cut fewer, from 0 to the move's end without a gap, no shot 6; shot 7 from shot 6's in.
        REQUIRE(n == far->size() - 1);
        CHECK(cuts[0].in == 0.f);
        CHECK(cuts[n - 1].out == doctest::Approx(length));
        for (std::uint32_t q = 0; q < n; ++q) {
            CHECK(cuts[q].shot != six);
            if (q > 0) CHECK(cuts[q].in == doctest::Approx(cuts[q - 1].out));
            if (cuts[q].shot == seven) {
                CHECK(cuts[q].in == doctest::Approx(s6.in));
                CHECK(cuts[q].out == doctest::Approx(far->shot(seven).out));
                CHECK(cuts[q].skipped == 1);
            } else {
                CHECK(cuts[q].skipped == 0);
            }
        }
        // The hit lands in shot 6's window; its shot is the one shown.
        CHECK(specialCineHit(*far, st, s6.in + 0.1f, 0.f, fixes).shot == seven);
        CHECK(specialCineHit(*far, st, s6.in + 0.1f).shot == six); // as written
    }
    MESSAGE("maw_pup: shot 6 pulled in from ", firstPulled, " to ", lastPulled, " m (", pulled, " ranges, at least ", leastTurn,
            " deg from shot 8, at most ", worst, " m past the cap); skipped under shot 7 from ", firstSkip, " m (", skipped,
            " ranges)");
    CHECK(pulled > 0);
    CHECK(skipped > 0);
    CHECK(firstPulled < 12.45f);
    CHECK(firstSkip < 13.45f);
    CHECK(worst <= 1e-3f);
}

TEST_CASE("special cine: a skipped shot as played: the cut lands on its in, the release still fires once, its shake plays") {
    SpecialCineTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(loadCommitted(t, warnings, error), error);
    const SpecialCineMove* far = t.find("devin_special_01");
    REQUIRE(far != nullptr);
    const std::uint32_t six = 5, seven = 6;
    const SpecialCineShot& s6 = far->shot(six);
    REQUIRE(s6.shake);
    REQUIRE_FALSE(far->shot(seven).shake);
    const float release = far->release();
    REQUIRE(release == doctest::Approx(s6.in));
    SpecialCineStage st = stageAt(0.f, 0.f, 14.f, 0.f);
    st.targetRadius = 0.35f;
    st.targetHeight = 0.9f;
    st.bone = {0.3f, 0.25f, 1.1f};
    for (const SpecialCineCover cover : {SpecialCineCover::Next, SpecialCineCover::Previous}) {
        CAPTURE(static_cast<int>(cover));
        const std::uint32_t shown = cover == SpecialCineCover::Next ? seven : six - 1;
        SpecialCinePlayer player;
        player.begin(far);
        SpecialCineShotFix skip;
        skip.cover = cover;
        REQUIRE(player.setFix(six, skip));
        int releases = 0, cutsInWindow = 0, shakes = 0, wrongShot = 0;
        bool sawSix = false, cutOnIn = false;
        float releaseAt = -1.f;
        const float dt = 1.f / 60.f;
        while (player.active()) {
            const SpecialCineFrame f = player.frame(st);
            const float now = player.time();
            if (f.shot == six) sawSix = true;
            if (f.release) {
                ++releases;
                releaseAt = now;
            }
            if (now >= s6.in && now < s6.out) {
                if (f.shot != shown) ++wrongShot;
                if (f.cut) ++cutsInWindow;
                if (f.shake.x != 0.f || f.shake.y != 0.f || f.shake.z != 0.f) ++shakes;
                if (f.cut && f.shot == seven && now < s6.in + dt + 1e-4f) cutOnIn = true;
            }
            player.step(dt);
        }
        CHECK_FALSE(sawSix);
        CHECK(wrongShot == 0);
        CHECK(releases == 1);
        CHECK(releaseAt >= release - 1e-4f);
        CHECK(releaseAt < release + dt + 1e-4f);
        CHECK(shakes > 0);
        if (cover == SpecialCineCover::Next) {
            CHECK(cutsInWindow == 1); // shot 7 cuts in on shot 6's in, and nothing cuts at its out
            CHECK(cutOnIn);
        } else {
            CHECK(cutsInWindow == 0); // the shot before holds through; shot 7 cuts in on its own in
        }
    }
}

TEST_CASE("special cine: the partner's blow by the lead's contact rule at any radius and height; off the line by its bearing") {
    SpecialCineTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(loadCommitted(t, warnings, error), error);
    const SpecialCineMove* far = t.find("devin_special_01");
    REQUIRE(far != nullptr);
    const float pi = 3.14159265f;
    const float radii[6] = {0.2f, 0.35f, 0.6f, 0.9f, 1.2f, 1.5f};
    float widestOnLine[6] = {};
    int cases = 0;
    const auto before = ftcore_test::allocStats().count;
    for (int r = 0; r < 6; ++r) {
        const float radius = radii[r];
        const float out = radius + SpecialCineFraming::kContactOut;
        for (const float height : {0.5f, 0.9f, 1.4f, 1.97f, 3.f}) {
            CAPTURE(radius);
            CAPTURE(height);
            SpecialCineStage st = stageAt(0.f, 0.f, 6.f + radius, 0.f);
            st.targetRadius = radius;
            st.targetHeight = height;
            const SpecialCineHit hit = specialCineHit(*far, st, 7.1f + 6.f / 40.f, radius);
            const float side = std::atan2(hit.at.y - st.target.y, hit.at.x - st.target.x); // the contact's side
            // The lead's chest capped at the target's: the contact's height, and the blow's (the partner as tall).
            const float z = std::min(0.6f * 1.8f, 0.6f * height);
            CHECK(near(hit.at.z, z, 1e-3f));
            for (float b = 0.f; b <= 90.f + 1e-3f; b += 5.f) {
                for (const float range : {1.f, 3.f}) {
                    const float a = side + b * pi / 180.f;
                    const CutsceneVec3 partner{st.target.x + (radius + range) * std::cos(a), (radius + range) * std::sin(a),
                                               0.6f * 1.8f};
                    const SpecialCinePartnerStrike s = specialCinePartnerStrike(*far, st, hit, partner, radius);
                    ++cases;
                    // On the surface toward the partner, the fist's depth out, at the contact's height.
                    const float dx = s.at.x - st.target.x, dy = s.at.y - st.target.y;
                    CHECK(near(std::sqrt(dx * dx + dy * dy), out, 1e-3f));
                    CHECK(near(s.at.z, z, 1e-3f));
                    CHECK(near(s.bearing, b, 0.5f));
                    // Off the line by the chord square to it; on a short target the line slopes down past the contact,
                    // so a blow round the far side sits a little above it.
                    const float lateral = out * std::sin(b * pi / 180.f);
                    CHECK(s.offLine >= lateral - 0.06f);
                    CHECK(s.offLine <= lateral + 0.06f + 0.12f * out * (1.f - std::cos(b * pi / 180.f)));
                    if (s.onLine()) widestOnLine[r] = std::max(widestOnLine[r], b);
                    // Straight on the lead's side, at any radius, height and range: the blow is the contact.
                    if (b == 0.f) {
                        CHECK(s.onLine());
                        CHECK(s.offLine < 0.06f);
                    }
                }
            }
        }
    }
    if (ftcore_test::allocHookInstalled()) CHECK(ftcore_test::allocStats().count == before);
    CHECK(cases > 1000);
    MESSAGE("widest bearing on the line (0.15 m), deg: ", widestOnLine[0], " at 0.2 m, ", widestOnLine[1], " at 0.35 m, ",
            widestOnLine[2], " at 0.6 m, ", widestOnLine[3], " at 0.9 m, ", widestOnLine[4], " at 1.2 m, ", widestOnLine[5],
            " at 1.5 m");
    // asin(0.15 / (R + 0.1)): about 30 degrees at 0.2 m, under 7 at 1.2 m.
    CHECK(widestOnLine[0] >= 25.f);
    CHECK(widestOnLine[4] <= 10.f);
    CHECK(widestOnLine[5] <= 5.f);

    // r2b_big (round 3): a 1.2 m target, the partner 65 degrees round from the lead's side. No blow on its surface is on
    // the line: (1.2 + 0.1) x sin 65 = 1.18 m. The partner's place, not the blow's rule, keeps it off.
    {
        SpecialCineStage st = stageAt(0.f, 0.f, 2.97f, 0.f);
        st.targetRadius = 1.2f;
        st.targetHeight = 3.f;
        st.leadHeight = 1.4f;
        const SpecialCineHit hit = specialCineHit(*far, st, 7.156f, 1.2f);
        const float a = std::atan2(hit.at.y, hit.at.x - 2.97f) + 65.f * pi / 180.f;
        const SpecialCinePartnerStrike s =
            specialCinePartnerStrike(*far, st, hit, CutsceneVec3{2.97f + 4.f * std::cos(a), 4.f * std::sin(a), 0.84f}, 1.2f);
        CHECK(near(s.bearing, 65.f, 0.5f));
        CHECK(near(s.offLine, 1.3f * std::sin(65.f * pi / 180.f), 0.12f));
        CHECK_FALSE(s.onLine());
    }
    // maw_pup (round 3): 0.9 m tall, its chest (0.54 m) caps the contact; the partner (chest 0.84 m) behind the lead on
    // its line. The old blow at the partner's chest stood 0.30 m off; by the contact rule it is the contact.
    {
        SpecialCineStage st = stageAt(0.f, 0.f, 13.f, 0.f);
        st.targetRadius = 0.35f;
        st.targetHeight = 0.9f;
        st.leadHeight = 1.4f;
        const SpecialCineHit hit = specialCineHit(*far, st, 7.4f, 0.35f);
        CHECK(near(hit.at.z, 0.54f, 1e-3f));
        const SpecialCinePartnerStrike s = specialCinePartnerStrike(*far, st, hit, CutsceneVec3{8.f, 0.04f, 0.84f}, 0.35f);
        CHECK(near(s.at.z, 0.54f, 1e-3f));
        CHECK(s.offLine < 0.05f);
        CHECK(s.onLine());
    }
    // At the target's axis: its centre, at the capped height; a partner taller than the target's chest is capped, a
    // shorter one keeps its own height.
    const CutsceneVec3 axis = specialCinePartnerBlow({3.f, 1.f, 0.f}, 0.5f, 0.9f, {3.f, 1.f, 1.2f});
    CHECK(near(axis.x, 3.f));
    CHECK(near(axis.y, 1.f));
    CHECK(near(axis.z, 0.54f));
    const CutsceneVec3 low = specialCinePartnerBlow({3.f, 1.f, 0.f}, 0.5f, 3.f, {0.f, 1.f, 1.f});
    CHECK(near(low.x, 2.4f));
    CHECK(near(low.y, 1.f));
    CHECK(near(low.z, 1.f));
    // Standing on a raised target: its feet's height counts.
    const CutsceneVec3 raised = specialCinePartnerBlow({3.f, 1.f, 0.5f}, 0.5f, 0.9f, {0.f, 1.f, 1.6f});
    CHECK(near(raised.z, 0.5f + 0.54f));
}

TEST_CASE("special cine: the hit's stills: its first frames to its fade, inside the move") {
    SpecialCineTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(loadCommitted(t, warnings, error), error);
    const SpecialCineMove* far = t.find("devin_special_01");
    REQUIRE(far != nullptr);
    SpecialCineStage st = stageAt(0.f, 0.f, 6.59f, 0.f);
    st.targetRadius = 0.45f;
    const SpecialCineHit hit = specialCineHit(*far, st, 7.1f + 6.59f / 40.f, 0.45f);
    float at[8] = {};
    REQUIRE(specialCineHitStills(*far, hit, at, 8) == SpecialCineHit::kStills);
    CHECK(at[0] == hit.stillAt);
    for (std::uint32_t k = 0; k < SpecialCineHit::kStills; ++k) {
        CHECK(near(at[k], hit.time + SpecialCineHit::kStillLags[k]));
        CHECK(at[k] < far->length());
        if (k > 0) CHECK(at[k] > at[k - 1]);
    }
    CHECK(near(at[3] - hit.time, 0.25f));
    CHECK(specialCineHitStills(*far, hit, at, 2) == 2);
    CHECK(specialCineHitStills(*far, hit, nullptr, 8) == 0);
    // Near the move's end: only those inside it.
    const SpecialCineHit late = specialCineHit(*far, st, far->length() - 0.1f, 0.45f);
    CHECK(specialCineHitStills(*far, late, at, 8) == 2);
}

TEST_CASE("special cine: the hit's contact in the shown shot: it reads at 6.6 m; out of frame on the smallest kind far out") {
    SpecialCineTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(loadCommitted(t, warnings, error), error);
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Battle/arena.json", text));
    ArenaTable arena;
    REQUIRE_MESSAGE(importArenaTable(text, arena, &error), error);
    REQUIRE(ftcore_test::readProjectData("Battle/arena_special.json", text));
    ArenaSpecialTable specials;
    REQUIRE_MESSAGE(importArenaSpecials(text, specials, &error), error);
    const ArenaSpecialRules* rules = specials.forLead("devin");
    REQUIRE(rules != nullptr);
    const ArenaFighter& lead = arena.fighter("devin");
    const ArenaEnemyKind* pup = nullptr;
    for (std::size_t k = 0; k < arena.enemies.size(); ++k)
        if (arena.enemyIds[k] == "maw_pup") pup = &arena.enemies[k];
    REQUIRE(pup != nullptr);
    const SpecialCineMove* far = t.find("devin_special_01");
    REQUIRE(far != nullptr);
    const CutsceneVec3 end = far->endFeet();
    const float walk = std::sqrt(end.x * end.x + end.y * end.y);
    // The host's two passes (every shot's fix, then the cover for a shot none frames), then the hit as played. With
    // `withHit` the hit (its time and contact, known before any fix) is given to the fix search and the cover, which
    // then test the shot shown at the hit and at its stills; without it, round 4's search.
    const auto played = [&](const SpecialCineMove& mv, const SpecialCineStage& st, float d, float radius, bool withHit,
                            SpecialCineShotFix* fixes, SpecialCineHit& hit, const SpecialCineHit* posed = nullptr) {
        const SpecialCineBody body = bodyOf(st);
        SpecialCineHit first = specialCineHit(mv, st, arenaSpecialImpactAt(*rules, d), radius);
        if (posed != nullptr) {
            first.posed = posed->posed;
            for (std::uint32_t k = 0; k <= SpecialCineHit::kStills; ++k) first.pose[k] = posed->pose[k];
        }
        const SpecialCineHit* given = withHit ? &first : nullptr;
        bool failing[SpecialCineMove::kMaxShots] = {};
        for (std::uint32_t i = 0; i < mv.size(); ++i) {
            if (mv.shot(i).subject == SpecialCineSubject::None) continue;
            const SpecialCineShotCheck c = specialCineFixShot(mv, i, st, &body, 1, nullptr, nullptr, given);
            fixes[i] = c.fix;
            failing[i] = !c.ok;
        }
        for (std::uint32_t i = 0; i < mv.size(); ++i)
            if (failing[i]) fixes[i].cover = specialCineCoverShot(mv, i, st, fixes, &body, 1, nullptr, nullptr, given);
        hit = specialCineHit(mv, st, arenaSpecialImpactAt(*rules, d), radius, fixes);
        hit.posed = first.posed;
        for (std::uint32_t k = 0; k <= SpecialCineHit::kStills; ++k) hit.pose[k] = first.pose[k];
        CHECK(near(hit.time, first.time));
        return specialCineHitProblem(mv, st, hit, fixes, &body, 1);
    };
    // The lead's pose at moment k of the hit, by the test's own sums: the measured one, else upright at its walk's end.
    const auto poseAt = [&](const SpecialCineMove& mv, const SpecialCineStage& st, const SpecialCineHit& hit, std::uint32_t k) {
        if (hit.posed) return hit.pose[k];
        const CutsceneVec3 e = mv.endFeet();
        const float a = std::clamp(st.travel, 0.f, 1.f), h = st.leadHeight;
        const CutsceneVec3 feet = specialCinePoint(st, SpecialCineAnchor::Lead, {e.x * a, e.y * a, e.z * a});
        return SpecialCineLeadPose{{feet.x, feet.y, feet.z + 0.5f * h}, {feet.x, feet.y, feet.z + 0.6f * h},
                                   {feet.x, feet.y, feet.z + SpecialCineFraming::kHeadHeight * h}};
    };
    // At the hit and every still (shot "none" frames excepted): Core's verdict none, and by the test's own sums the
    // lead's head and hand in frame and the contact line clear of its body.
    const auto hitReads = [&](const SpecialCineMove& mv, const SpecialCineStage& st, const SpecialCineShotFix* fixes,
                              const SpecialCineHit& hit) {
        float at[1 + SpecialCineHit::kStills] = {hit.time};
        const std::uint32_t n = 1 + specialCineHitStills(mv, hit, at + 1, SpecialCineHit::kStills);
        const SpecialCineBody body = bodyOf(st);
        bool ok = true;
        for (std::uint32_t k = 0; k < n; ++k) {
            const SpecialCineFrame f = specialCineFrameAt(mv, at[k], st, fixes);
            if (mv.shot(f.shot).subject == SpecialCineSubject::None) continue;
            SpecialCineHit still = hit;
            still.time = at[k];
            still.pose[0] = hit.pose[k];
            const SpecialCineProblem p = specialCineHitProblem(mv, st, still, fixes, &body, 1);
            const SpecialCineLeadPose q = poseAt(mv, st, hit, k);
            const bool reads = poseInFrame(f, q, hit.at), clear = torsoGap(f, hit.at, q) >= SpecialCineFraming::kTorso;
            if (p != SpecialCineProblem::None || !reads || !clear)
                MESSAGE("  at ", at[k], " s (shot ", f.shot + 1, "): ", specialCineProblemName(p), "; by the test's sums the lead reads ",
                        reads, ", the contact line clear of it ", clear);
            ok = ok && p == SpecialCineProblem::None && reads && clear;
        }
        return ok;
    };
    // r2b_far: a brute 6.59 m off, the hit in shot 6: the contact reads.
    {
        SpecialCineStage st = stageAt(0.f, 0.f, 6.59f, 0.f);
        st.targetRadius = 0.45f;
        st.targetHeight = 1.97f;
        st.travel = travelAt(6.59f, lead.bodyRadius + 0.45f, walk);
        st.bone = {0.3f, 0.25f, 1.1f};
        SpecialCineShotFix fixes[SpecialCineMove::kMaxShots]{};
        SpecialCineHit hit;
        CHECK(played(*far, st, 6.59f, 0.45f, true, fixes, hit) == SpecialCineProblem::None);
        CHECK(hit.shot == 5);
        CHECK(hitReads(*far, st, fixes, hit));
        // Round 4's shot 6 here passed the ruling: the hit given, its fix is the same.
        SpecialCineShotFix old[SpecialCineMove::kMaxShots]{};
        SpecialCineHit oldHit;
        played(*far, st, 6.59f, 0.45f, false, old, oldHit);
        CHECK(fixes[5].mirror == old[5].mirror);
        CHECK(fixes[5].aim == old[5].aim);
        CHECK(near(fixes[5].raise, old[5].raise));
        CHECK(near(fixes[5].pull, old[5].pull));
        CHECK(near(fixes[5].orbit, old[5].orbit));
        CHECK(fixes[5].cover == old[5].cover);
    }
    // r2b_big (round 4's log): a hulk (1.2 m, 3.5 m tall) 2.41 m off at the press, so the near set; the lead (1.4 m)
    // walks its whole 0.73 m. Round 4 showed shot 6 (release over the shoulder) with the lead a cropped back at the
    // bottom edge. Given the hit, the search frames the lead's head and striking hand at the hit and at every still.
    {
        const ArenaEnemyKind* hulk = nullptr;
        for (std::size_t k = 0; k < arena.enemies.size(); ++k)
            if (arena.enemyIds[k] == "hulk") hulk = &arena.enemies[k];
        REQUIRE(hulk != nullptr);
        SpecialCineStage st = stageAt(0.f, 0.f, 2.41f, -0.09f);
        st.targetRadius = hulk->bodyRadius;
        st.targetHeight = arenaEnemyHeight(*hulk);
        st.leadHeight = 1.4f;
        st.travel = 1.f;
        st.bone = {0.3f, 0.25f, 1.1f};
        const SpecialCineMove* mv = t.find("devin_special_01", specialCineDistance(st));
        REQUIRE(mv == &t.nearMove(0));
        SpecialCineShotFix fixes[SpecialCineMove::kMaxShots]{};
        SpecialCineHit hit;
        const SpecialCineProblem p = played(*mv, st, 2.41f, hulk->bodyRadius, true, fixes, hit);
        CHECK(p == SpecialCineProblem::None);
        CHECK(hitReads(*mv, st, fixes, hit));
        // The lead lunges into the blow: its pose fitted to round 4's still r2b_big_sp_07.30 on Core's camera there (the
        // head at pixel 860,560, the shoulders' top at 800,655 and the back at 790,800 of 1600x900; the body right of its
        // left fist), held through the stills; the pose the next block logs replaces it. Round 4's game fix for shot 6
        // (aimed, orbited -30 degrees) looks at the contact over the lead's back: behind the lead.
        SpecialCineHit lunge = specialCineHit(*mv, st, 7.145f, hulk->bodyRadius);
        lunge.posed = true;
        for (SpecialCineLeadPose& q : lunge.pose) q = {{0.52f, 0.08f, 0.64f}, {0.74f, 0.03f, 0.86f}, {0.91f, 0.04f, 1.10f}};
        SpecialCineShotFix game[SpecialCineMove::kMaxShots]{};
        game[5] = SpecialCineShotFix{false, 0.f, 0.f, true, -30.f};
        const SpecialCineBody body = bodyOf(st);
        const SpecialCineFrame gf = specialCineFrameAt(*mv, lunge.time, st, game);
        MESSAGE("r2b_big, round 4's game fix on the lunge: ", specialCineProblemName(specialCineHitProblem(*mv, st, lunge, game, &body, 1)),
                "; the contact line ", torsoGap(gf, lunge.at, lunge.pose[0]), " m from the lead's hips-to-head line");
        CHECK(specialCineHitProblem(*mv, st, lunge, game, &body, 1) == SpecialCineProblem::ContactBehindLead);
        CHECK(torsoGap(gf, lunge.at, lunge.pose[0]) < SpecialCineFraming::kTorso);
        // Given the posed hit, the search frames the blow clear of the lead's back, its head and hand in frame.
        SpecialCineShotFix posedFixes[SpecialCineMove::kMaxShots]{};
        SpecialCineHit posedHit;
        const SpecialCineProblem q = played(*mv, st, 2.41f, hulk->bodyRadius, true, posedFixes, posedHit, &lunge);
        MESSAGE("r2b_big on the lunge: shot 6 ", posedFixes[5].mirror ? std::string("mirrored, ") : std::string(), posedFixes[5].aim ? std::string("aimed, ") : std::string(),
                "orbited ", posedFixes[5].orbit, ", raised ", posedFixes[5].raise, ", pulled ", posedFixes[5].pull, ", cover ",
                static_cast<int>(posedFixes[5].cover), "; problem ", specialCineProblemName(q));
        CHECK(q == SpecialCineProblem::None);
        CHECK(hitReads(*mv, st, posedFixes, posedHit));
    }
    // maw_pup at 12.49 and 13.69 m (round 3's 13.0 and 14.2 spawns at the press): the hit falls in shot 7, which frames
    // the lead; round 4 left its contact out of frame. The fix search, given the hit, moves the shot or covers it.
    for (const float d : {12.49f, 13.69f}) {
        CAPTURE(d);
        SpecialCineStage st = stageAt(0.f, 0.f, d, 0.f);
        st.targetRadius = pup->bodyRadius;
        st.targetHeight = arenaEnemyHeight(*pup);
        st.travel = travelAt(d, lead.bodyRadius + pup->bodyRadius, walk);
        st.bone = {0.3f, 0.25f, 1.1f};
        SpecialCineShotFix fixes[SpecialCineMove::kMaxShots]{};
        SpecialCineHit hit;
        const auto before = ftcore_test::allocStats().count;
        const SpecialCineProblem p = played(*far, st, d, pup->bodyRadius, true, fixes, hit);
        if (ftcore_test::allocHookInstalled()) CHECK(ftcore_test::allocStats().count == before);
        MESSAGE("maw_pup at ", d, " m: the hit at ", hit.time, " s in shot ", hit.shot + 1, " (shot 7's cover ",
                static_cast<int>(fixes[6].cover), "), problem ", specialCineProblemName(p));
        CHECK(p == SpecialCineProblem::None);
        CHECK(hitReads(*far, st, fixes, hit));
    }
}

TEST_CASE("special cine: the partner's mark puts its blow on the lead's line, behind the lead, inside its reach") {
    SpecialCineTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(loadCommitted(t, warnings, error), error);
    const SpecialCineMove* far = t.find("devin_special_01");
    REQUIRE(far != nullptr);
    // r2b_big: the hulk (1.2 m) 2.97 m off; the Third (light range 3.6, reach 8.5, standoff 5) chest 0.84 m.
    SpecialCineStage st = stageAt(0.f, 0.f, 2.97f, 0.f);
    st.targetRadius = 1.2f;
    st.targetHeight = 3.f;
    st.leadHeight = 1.4f;
    const SpecialCineHit hit = specialCineHit(*far, st, 7.156f, 1.2f);
    const SpecialCinePartnerMark m = specialCinePartnerMark(*far, st, hit, 0.35f, 0.35f, 8.5f, 5.f);
    REQUIRE(m.found);
    MESSAGE("r2b_big mark: ", m.fromSurface, " m from the surface, ", m.behindLead, " m behind the lead");
    CHECK(near(m.fromSurface, 5.f, 1e-3f)); // the standoff, farther than the lead's reach behind it
    CHECK(m.behindLead >= 1.1f + 0.35f + SpecialCinePartnerMark::kGap - 1e-3f);
    const SpecialCinePartnerStrike s = specialCinePartnerStrike(*far, st, hit, CutsceneVec3{m.feet.x, m.feet.y, m.feet.z + 0.84f}, 1.2f);
    CHECK(s.bearing < 1.f);
    CHECK(s.onLine());
    // Too short a reach: not found, and the numbers say why.
    const SpecialCinePartnerMark shortReach = specialCinePartnerMark(*far, st, hit, 0.35f, 0.35f, 3.6f, 5.f);
    CHECK_FALSE(shortReach.found);
    CHECK(shortReach.fromSurface > 3.6f);
    // No standoff: just clear of the lead's staff and body.
    const SpecialCinePartnerMark close = specialCinePartnerMark(*far, st, hit, 0.35f, 0.35f, 8.5f);
    CHECK(near(close.behindLead, 1.1f + 0.35f + SpecialCinePartnerMark::kGap, 1e-3f));
    // Every radius and distance: the blow from the mark is on the line, the mark clear of the lead, within reach when found.
    int cases = 0;
    for (const float r : {0.2f, 0.35f, 0.6f, 0.9f, 1.2f, 1.5f})
        for (const float d : {2.f, 3.f, 6.6f, 13.f})
            for (const float side : {0.f, 1.5f}) {
                CAPTURE(r);
                CAPTURE(d);
                CAPTURE(side);
                SpecialCineStage sw = stageAt(0.f, 0.f, d, side);
                sw.targetRadius = r;
                sw.targetHeight = 1.8f;
                sw.leadHeight = 1.4f; // the lead's chest and the partner's at one height (0.84 m), as on r2b
                const SpecialCineHit h = specialCineHit(*far, sw, 7.2f, r);
                const SpecialCinePartnerMark k = specialCinePartnerMark(*far, sw, h, 0.35f, 0.35f, 8.5f, 5.f);
                CHECK(k.fromSurface >= 5.f - 1e-3f);
                CHECK(k.behindLead >= 0.35f + 0.35f + SpecialCinePartnerMark::kGap - 1e-3f);
                CHECK(k.found == (k.fromSurface <= 8.5f));
                const SpecialCinePartnerStrike ks =
                    specialCinePartnerStrike(*far, sw, h, CutsceneVec3{k.feet.x, k.feet.y, k.feet.z + 0.84f}, r);
                CHECK(ks.onLine());
                ++cases;
            }
    CHECK(cases == 48);
}

// Round 5's r2b_big log (4be6a0ba): the hulk (1.2 m, 3.5 m tall) 2.41 m off, the partner on its mark (-3.22, -0.29) m,
// the lead's pose read from the clip at the hit and its stills (the log's metres less the stage's origin (0.56, -0.01)).
// The game played shots 6 and 7 as written: +80 ms the contact behind the lead, +150 ms off frame, +250 ms (shot 7)
// behind the lead.
TEST_CASE("special cine: round 5's r2b_big as logged: the hit's shots frame the lead's head and hand, the contact clear") {
    SpecialCineTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(loadCommitted(t, warnings, error), error);
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Battle/arena.json", text));
    ArenaTable arena;
    REQUIRE_MESSAGE(importArenaTable(text, arena, &error), error);
    const ArenaFighter& lead = arena.fighter("devin");
    // The fight's view along +x (its end less its origin: endTravel 0.73, endSide -0.11); the hulk where the contact
    // (1.67, -0.08) and the mark (on the target's line through the lead) put it.
    SpecialCineStage st = stageAt(0.f, 0.f, 2.41f, -0.02f);
    st.forward = {1.f, 0.f, 0.f};
    st.targetRadius = 1.2f;
    st.targetHeight = 3.5f;
    st.leadHeight = 1.4f;
    st.travel = 1.f;
    st.bone = {0.f, 0.f, 1.2f};
    const SpecialCineMove* mv = t.find("devin_special_01", specialCineDistance(st));
    REQUIRE(mv == &t.nearMove(0));
    SpecialCineBody bodies[2];
    bodies[0] = bodyOf(st);
    bodies[1].feet = {-3.22f - 0.56f, -0.29f + 0.01f, 0.f};
    bodies[1].radius = lead.bodyRadius;
    bodies[1].height = 1.68f;
    SpecialCineHit hit = specialCineHit(*mv, st, 7.145f, 1.2f);
    MESSAGE("contact (", hit.at.x, ", ", hit.at.y, ", ", hit.at.z, "), shot ", hit.shot + 1);
    CHECK(near(hit.at.x, 1.67f - 0.56f, 0.03f));
    CHECK(near(hit.at.y, -0.08f + 0.01f, 0.03f));
    CHECK(near(hit.at.z, 0.84f, 0.03f));
    hit.posed = true;
    const float logged[1 + SpecialCineHit::kStills][9] = {
        {1.36f, -0.17f, 0.39f, 1.58f, -0.27f, 0.69f, 1.82f, -0.40f, 0.92f},
        {1.35f, -0.16f, 0.39f, 1.56f, -0.25f, 0.70f, 1.80f, -0.36f, 0.94f},
        {1.33f, -0.15f, 0.40f, 1.54f, -0.22f, 0.72f, 1.77f, -0.32f, 0.97f},
        {1.34f, -0.15f, 0.40f, 1.54f, -0.23f, 0.71f, 1.78f, -0.34f, 0.96f},
        {1.34f, -0.15f, 0.40f, 1.54f, -0.23f, 0.71f, 1.78f, -0.34f, 0.96f}};
    for (std::uint32_t k = 0; k <= SpecialCineHit::kStills; ++k) {
        const float* q = logged[k];
        hit.pose[k] = {{q[0] - 0.56f, q[1] + 0.01f, q[2]}, {q[3] - 0.56f, q[4] + 0.01f, q[5]}, {q[6] - 0.56f, q[7] + 0.01f, q[8]}};
    }
    float at[1 + SpecialCineHit::kStills] = {hit.time};
    const std::uint32_t n = 1 + specialCineHitStills(*mv, hit, at + 1, SpecialCineHit::kStills);
    // As written (round 5 played these unmoved): shot 6 loses the contact off frame by +150 ms; shot 7, its eye on the
    // lead's far side (-y), has the lead's body before the contact at +250 ms.
    REQUIRE(n == 1 + SpecialCineHit::kStills);
    SpecialCineShotFix none[SpecialCineMove::kMaxShots]{};
    SpecialCineProblem written[1 + SpecialCineHit::kStills]{};
    for (std::uint32_t k = 0; k < n; ++k) {
        SpecialCineHit still = hit;
        still.time = at[k];
        still.pose[0] = hit.pose[k];
        const SpecialCineFrame f = specialCineFrameAt(*mv, at[k], st, none);
        written[k] = specialCineHitProblem(*mv, st, still, none, bodies, 2);
        MESSAGE("as written at ", at[k], " (shot ", f.shot + 1, "): ", specialCineProblemName(written[k]));
    }
    CHECK(written[0] == SpecialCineProblem::None);
    CHECK(written[3] == SpecialCineProblem::OffFrame);
    CHECK(written[4] == SpecialCineProblem::ContactBehindLead);
    // The lead pressed into its blow: the contact itself inside kTorso of its hips-to-head line from +80 ms on, so the
    // full sight to it fails from every camera; the sight stopped kTorso short of it (specialCineSightGap) does not.
    const auto toLine = [](const CutsceneVec3& c, const SpecialCineLeadPose& q) {
        const CutsceneVec3 b = sub3(q.head, q.hips), r = sub3(c, q.hips);
        const float bb = b.x * b.x + b.y * b.y + b.z * b.z;
        const float u = std::clamp((r.x * b.x + r.y * b.y + r.z * b.z) / bb, 0.f, 1.f);
        return length3(sub3(c, {q.hips.x + b.x * u, q.hips.y + b.y * u, q.hips.z + b.z * u}));
    };
    for (std::uint32_t k = 0; k <= SpecialCineHit::kStills; ++k) MESSAGE("the contact ", toLine(hit.at, hit.pose[k]), " m from the lead's line at moment ", k);
    CHECK(toLine(hit.at, hit.pose[2]) < SpecialCineFraming::kTorso);
    CHECK(toLine(hit.at, hit.pose[3]) < SpecialCineFraming::kTorso);
    CHECK(toLine(hit.at, hit.pose[4]) < SpecialCineFraming::kTorso);
    // The host's two passes with both bodies: every shot's fix, then the cover for one still failing; then the hit and
    // each still read none, and by the test's own sums the lead's head and hand are in frame, the contact line clear.
    const auto playedHere = [&](const SpecialCineStage& stg, const SpecialCineHit& posedHit, SpecialCineShotFix* fixes,
                                const char* name) {
        SpecialCineBody bs[2] = {bodyOf(stg), bodies[1]};
        SpecialCineHit h = specialCineHit(*mv, stg, 7.145f, 1.2f);
        h.posed = true;
        for (std::uint32_t k = 0; k <= SpecialCineHit::kStills; ++k) h.pose[k] = posedHit.pose[k];
        bool failing[SpecialCineMove::kMaxShots] = {};
        for (std::uint32_t i = 0; i < mv->size(); ++i) {
            if (mv->shot(i).subject == SpecialCineSubject::None) continue;
            const SpecialCineShotCheck c = specialCineFixShot(*mv, i, stg, bs, 2, nullptr, nullptr, &h);
            fixes[i] = c.fix;
            failing[i] = !c.ok;
        }
        for (std::uint32_t i = 0; i < mv->size(); ++i)
            if (failing[i]) fixes[i].cover = specialCineCoverShot(*mv, i, stg, fixes, bs, 2, nullptr, nullptr, &h);
        float when[1 + SpecialCineHit::kStills] = {h.time};
        const std::uint32_t m = 1 + specialCineHitStills(*mv, h, when + 1, SpecialCineHit::kStills);
        bool ok = true;
        for (std::uint32_t k = 0; k < m; ++k) {
            SpecialCineHit still = h;
            still.time = when[k];
            still.pose[0] = h.pose[k];
            const SpecialCineFrame f = specialCineFrameAt(*mv, when[k], stg, fixes);
            if (mv->shot(f.shot).subject == SpecialCineSubject::None) continue;
            const SpecialCineProblem p = specialCineHitProblem(*mv, stg, still, fixes, bs, 2);
            const bool reads = poseInFrame(f, h.pose[k], h.at), clear = torsoGap(f, h.at, h.pose[k]) >= SpecialCineFraming::kTorso;
            if (p != SpecialCineProblem::None || !reads || !clear)
                MESSAGE(name, " at ", when[k], " (shot ", f.shot + 1, "): ", specialCineProblemName(p), ", reads ", reads, ", clear ", clear);
            ok = ok && p == SpecialCineProblem::None && reads && clear;
        }
        return ok;
    };
    {
        SpecialCineShotFix fixes[SpecialCineMove::kMaxShots]{};
        CHECK(playedHere(st, hit, fixes, "as logged"));
        for (std::uint32_t i : {5u, 6u})
            MESSAGE("shot ", i + 1, ": mirror ", fixes[i].mirror, ", raise ", fixes[i].raise, ", pull ", fixes[i].pull, ", aim ",
                    fixes[i].aim, ", orbit ", fixes[i].orbit, ", cover ", static_cast<int>(fixes[i].cover));
        for (std::uint32_t i : {5u, 6u}) {
            const SpecialCineShotCheck c = specialCineFixShot(*mv, i, st, bodies, 2, nullptr, nullptr, &hit);
            CAPTURE(i);
            CHECK(c.ok);
            CHECK(c.last == SpecialCineProblem::None);
        }
    }
    // Round 4's criterion (the full sight) fails the framing found at +80 ms: the sight ends at the contact, under
    // kTorso from the lead's line.
    {
        SpecialCineShotFix fixes[SpecialCineMove::kMaxShots]{};
        playedHere(st, hit, fixes, "as logged");
        const SpecialCineFrame f = specialCineFrameAt(*mv, at[2], st, fixes);
        CHECK(sampledGap(f.pose.position, hit.at, hit.pose[2].hips, hit.pose[2].head) < SpecialCineFraming::kTorso);
        CHECK(torsoGap(f, hit.at, hit.pose[2]) >= SpecialCineFraming::kTorso);
    }
    // The striking fist measured (the host reads it from the clip): one the framing found leaves out, at arm's reach from
    // the chest (kHandReach), reads LeadCropped there; the search given it frames it.
    {
        SpecialCineShotFix fixes[SpecialCineMove::kMaxShots]{};
        REQUIRE(playedHere(st, hit, fixes, "as logged"));
        const SpecialCineFrame f = specialCineFrameAt(*mv, hit.time, st, fixes);
        const SpecialCineLeadPose& q = hit.pose[0];
        CutsceneVec3 off{};
        bool found = false;
        for (int e = -4; e <= 4 && !found; ++e)
            for (int a = 0; a < 24 && !found; ++a) {
                const float az = static_cast<float>(a) * 15.f * kDeg, el = static_cast<float>(e) * 20.f * kDeg;
                const float r = SpecialCineFraming::kHandReach;
                off = {r * std::cos(el) * std::cos(az), r * std::cos(el) * std::sin(az), r * std::sin(el)};
                found = q.chest.z + off.z > 0.05f && !inFrameBox(f, add3(q.chest, off), SpecialCineFraming::kInFrame);
            }
        MESSAGE("a fist out of the found framing at the hit (shot ", f.shot + 1, "): ", found, ", chest + (", off.x, ", ", off.y, ", ", off.z, ")");
        REQUIRE(found);
        SpecialCineHit fisted = hit;
        for (SpecialCineLeadPose& p : fisted.pose) {
            p.hand = add3(p.chest, off);
            p.fist = true;
        }
        SpecialCineBody bs[2] = {bodyOf(st), bodies[1]};
        CHECK(specialCineHitProblem(*mv, st, fisted, fixes, bs, 2) == SpecialCineProblem::LeadCropped);
        CHECK(specialCineHitProblem(*mv, st, hit, fixes, bs, 2) == SpecialCineProblem::None); // the model's hand: in frame
        SpecialCineShotFix refixed[SpecialCineMove::kMaxShots]{};
        CHECK(playedHere(st, fisted, refixed, "with the fist"));
        MESSAGE("with the fist, shot ", f.shot + 1, ": mirror ", refixed[f.shot].mirror, ", raise ", refixed[f.shot].raise, ", aim ",
                refixed[f.shot].aim, ", orbit ", refixed[f.shot].orbit, ", cover ", static_cast<int>(refixed[f.shot].cover));
    }
    // The verdict holds off the logged stage: the hulk 5 cm either side, the pose a few cm off each way.
    std::uint32_t variants = 0, held = 0;
    for (const float dy : {-0.05f, -0.025f, 0.f, 0.025f, 0.05f}) {
        for (const CutsceneVec3 off : {CutsceneVec3{0.f, 0.f, 0.f}, CutsceneVec3{0.03f, 0.f, 0.f}, CutsceneVec3{-0.03f, 0.f, 0.f},
                                       CutsceneVec3{0.f, 0.03f, 0.f}, CutsceneVec3{0.f, -0.03f, 0.f}, CutsceneVec3{0.f, 0.f, 0.03f}}) {
            SpecialCineStage stg = st;
            stg.target.y += dy;
            SpecialCineHit h = hit;
            for (SpecialCineLeadPose& q : h.pose) {
                q.hips = add3(q.hips, off);
                q.chest = add3(q.chest, off);
                q.head = add3(q.head, off);
            }
            SpecialCineShotFix fixes[SpecialCineMove::kMaxShots]{};
            CAPTURE(dy);
            CAPTURE(off.x);
            CAPTURE(off.y);
            CAPTURE(off.z);
            const bool ok = playedHere(stg, h, fixes, "swept");
            CHECK(ok);
            ++variants;
            held += ok ? 1u : 0u;
        }
    }
    MESSAGE("swept: ", held, " of ", variants, " hold");
}

TEST_CASE("special cine: the sight gap leaves out the contact's own kTorso, so only a body before it counts") {
    // An upright lead (hips 0.4, head 1.3 m) with the contact 0.18 m in front of it, inside kTorso.
    const CutsceneVec3 hips{0.f, 0.f, 0.4f}, head{0.f, 0.f, 1.3f}, contact{0.18f, 0.f, 0.9f};
    // From the open side the sight stops kTorso short: 0.40 m off the line, clear (the full sight: 0.18, not).
    const float open = specialCineSightGap({3.f, 0.f, 0.9f}, contact, hips, head);
    CHECK(near(open, 0.40f, 1e-3f));
    CHECK(open >= SpecialCineFraming::kTorso);
    CHECK(sampledGap({3.f, 0.f, 0.9f}, contact, hips, head) < SpecialCineFraming::kTorso);
    // From behind the lead the sight stops 0.04 m short of its line, inside its body: hidden. (From straight behind, a
    // contact d under kTorso in front leaves kTorso - d: hidden for any contact off the line.)
    CHECK(near(specialCineSightGap({-3.f, 0.f, 0.9f}, contact, hips, head), 0.04f, 1e-3f));
    // Seen from the side at a slant past its chest: the distance from its line, as sampled.
    const CutsceneVec3 side{0.6f, 3.f, 1.2f};
    const float slant = specialCineSightGap(side, contact, hips, head);
    CHECK(near(slant, stoppedSightGap(side, contact, hips, head), 3e-3f));
    // An eye inside kTorso of the contact: its own distance from the line.
    CHECK(near(specialCineSightGap({0.3f, 0.f, 0.9f}, contact, hips, head), 0.3f, 1e-4f));
}
