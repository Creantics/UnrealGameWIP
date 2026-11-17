#include "ftcore/SpecialCine.h"

#include "ftcore/Json.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

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

TEST_CASE("special cine: the framing check: the captured release frame at 7.33 s shows nothing, a mirror cannot fix it, the aim does") {
    SpecialCineTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(loadCommitted(t, warnings, error), error);
    const SpecialCineMove* m = t.find("devin_special_01");
    REQUIRE(m != nullptr);
    const SpecialCineStage st = capturedStage();
    const SpecialCineBody body = bodyOf(st);
    const std::uint32_t release = 5;
    const SpecialCineShot& s = m->shot(release);
    REQUIRE(s.subject == SpecialCineSubject::Both);
    // 7.33 s in the captured 7.10..7.35 shot: 92% in. Its camera stands beside the target and its look-at rides the
    // wave out past the camera: it faces away from both figures.
    const float at = s.in + 0.92f * (s.out - s.in);
    SpecialCineShotFix fixes[SpecialCineMove::kMaxShots]{};
    CHECK(specialCineFrameProblem(*m, at, st, fixes, &body, 1) == SpecialCineProblem::OffFrame);
    // Mirrored: both figures stand on the stage's axis, so the mirror swaps the camera and the look-at to the other
    // side and frames nothing more. Raised: no better.
    fixes[release] = fixOf(true, 0.f, false);
    CHECK(specialCineFrameProblem(*m, at, st, fixes, &body, 1) == SpecialCineProblem::OffFrame);
    fixes[release] = fixOf(false, 0.8f, false);
    CHECK(specialCineFrameProblem(*m, at, st, fixes, &body, 1) == SpecialCineProblem::OffFrame);
    // Aimed at the two figures: both in the safe frame, the target's own body not counted against the two-shot.
    fixes[release] = fixOf(false, 0.f, true);
    CHECK(specialCineFrameProblem(*m, at, st, fixes, &body, 1) == SpecialCineProblem::None);

    // The search: the mirror and the raises are tried first, then the aim.
    const SpecialCineShotCheck c = specialCineFixShot(*m, release, st, &body, 1);
    CHECK(c.first == SpecialCineProblem::OffFrame);
    CHECK(c.ok);
    CHECK(c.fix.aim);
    CHECK_FALSE(c.fix.mirror);
    CHECK(c.fix.pull == 0.f);
    CHECK(c.tries == 9); // as written, mirrored, 3 raises, 3 mirrored raises, aimed

    // Every shot passes on that stage, moved or not; the last two show the lead as written (the capture's host test
    // pulled them off a body that was not in their line).
    for (std::uint32_t i = 0; i < m->size(); ++i) {
        CAPTURE(i);
        CHECK(specialCineFixShot(*m, i, st, &body, 1).ok);
    }
    CHECK(specialCineFixShot(*m, 6, st, &body, 1).first == SpecialCineProblem::None);
    const SpecialCineShotCheck last = specialCineFixShot(*m, 7, st, &body, 1);
    CHECK(last.first == SpecialCineProblem::None);
    CHECK_FALSE(last.fix.moved());
    // At 2 m shot 1 starts in the brute's body and shot 2 has it in front of the lead: both pulled in.
    CHECK(specialCineFixShot(*m, 0, st, &body, 1).fix.pull > 0.f);
    CHECK(specialCineFixShot(*m, 1, st, &body, 1).fix.pull > 0.f);
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

    // Beyond the target looking back: the target stands in the line to the lead, but not in its own.
    m = oneShot({8.f, 0.f, 1.2f}, {0.f, 0.f, 1.f}, SpecialCineSubject::Lead);
    CHECK(specialCineFrameProblem(m, 0.5f, st, nullptr, &target, 1) == SpecialCineProblem::BodyInLine);
    m = oneShot({8.f, 0.f, 1.2f}, {5.f, 0.f, 1.f}, SpecialCineSubject::Target);
    CHECK(specialCineFrameProblem(m, 0.5f, st, nullptr, &target, 1) == SpecialCineProblem::None);
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
}
