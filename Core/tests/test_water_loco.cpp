// Water locomotion (WaterLoco.h): entries, the splash sink and recover, tread, surface swim, dive, under, breach,
// exits, cues, the tuning file and the guards.
#include "ftcore/WaterLoco.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 30.f;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

WaterState water(WaterMode mode, float column = 4.f) {
    WaterState w;
    w.mode = mode;
    w.surface = 0.f;
    w.column = column;
    w.immersion = mode == WaterMode::Dry ? 0.f : 1.f;
    return w;
}

WaterLocoInput swimIn(Vec2 move = {}, float yaw = 0.f) {
    WaterLocoInput in;
    in.water = water(WaterMode::Swim);
    in.move = move;
    in.yaw = yaw;
    return in;
}

// Steps `in` for `seconds`, feeding the facing back; returns the last pose.
const WaterLocoPose& run(WaterLoco& w, WaterLocoInput in, float seconds) {
    const WaterLocoPose* p = &w.pose();
    for (float t = 0.f; t < seconds; t += kDt) {
        p = &w.step(in, kDt);
        in.yaw = p->yaw;
    }
    return *p;
}

// At the surface, treading, settled.
WaterLoco treading() {
    WaterLoco w;
    run(w, swimIn(), 0.5f);
    return w;
}

float weightOf(const WaterLocoPose& p, WaterClip c) {
    float s = 0.f;
    for (int i = 0; i < p.layerCount; ++i)
        if (p.layers[i].clip == c) s += p.layers[i].weight;
    return s;
}

float weightSum(const WaterLocoPose& p) {
    float s = 0.f;
    for (int i = 0; i < p.layerCount; ++i) s += p.layers[i].weight;
    return s;
}

// A fall into the water at `vz` m/s over a column `column` m deep: one airborne step, then the water.
WaterLoco fallInto(float vz, float column = 4.f, WaterMode mode = WaterMode::Swim) {
    WaterLoco w;
    WaterLocoInput air;
    air.falling = true;
    air.velocity = {0.f, 0.f, vz};
    w.step(air, kDt);
    WaterLocoInput in;
    in.water = water(mode, column);
    in.velocity = {0.f, 0.f, vz};
    w.step(in, kDt);
    return w;
}

} // namespace

TEST_CASE("water loco: the defaults are valid and match Data/Swim/water_loco.json") {
    CHECK(validWaterLocoTuning(WaterLocoTuning{}));
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Swim/water_loco.json", text));
    WaterLocoTuning t;
    t.swimSpeed = 4.f;
    std::string err;
    std::vector<std::string> notes;
    REQUIRE_MESSAGE(importWaterLocoTuning(text, t, &err, &notes), err);
    CHECK(notes.empty());
    const WaterLocoTuning d;
    for (const auto& r : water_loco_detail::kRanges) CHECK_MESSAGE(t.*r.field == d.*r.field, r.name);
    CHECK(d.swimSpeed == kWaterSwimSpeed);
    CHECK(d.headClear == 0.12f);
    CHECK(d.sinkOmega == 4.f);
}

TEST_CASE("water loco: the importer is lenient and notes what it replaced") {
    WaterLocoTuning t;
    std::string err;
    std::vector<std::string> notes;
    const auto noted = [&](const char* what) {
        for (const std::string& n : notes)
            if (n.find(what) != std::string::npos) return true;
        return false;
    };
    REQUIRE(importWaterLocoTuning(R"({"version":1,"treadSpeed":0.8,"swimSpeed":"quick","gills":1})", t, &err, &notes));
    CHECK(t.treadSpeed == 0.8f);
    CHECK(t.swimSpeed == 1.5f);
    CHECK(noted("swimSpeed: expected a number in 0.3..5, the default 1.5 used"));
    CHECK(noted("gills: unknown field, skipped"));
    CHECK(noted("headClear: missing, the default 0.12 used"));
    // A broken pair: both fields back to their defaults.
    REQUIRE(importWaterLocoTuning(R"({"version":1,"swimSpeed":3,"swimSprintSpeed":2})", t, &err, &notes));
    CHECK(t.swimSpeed == 1.5f);
    CHECK(t.swimSprintSpeed == 2.4f);
    CHECK(noted("swimSpeed, swimSprintSpeed: expected swimSprintSpeed at least swimSpeed, both defaults used"));
    // Only an unreadable file fails.
    CHECK_FALSE(importWaterLocoTuning(R"({"version":2})", t, &err, &notes));
    CHECK(err == "version: expected 1");
    CHECK_FALSE(importWaterLocoTuning("{", t, &err, &notes));
    // A bad tuning handed to the constructor is replaced by the defaults.
    WaterLocoTuning bad;
    bad.sinkOmega = -1.f;
    WaterLoco w(bad);
    CHECK_FALSE(w.tuningOk());
    CHECK(w.tuning().sinkOmega == 4.f);
}

TEST_CASE("water loco: dry and wading play locomotion; the arms rise with the wade's depth") {
    WaterLoco w;
    WaterLocoInput in;
    CHECK(w.step(in, kDt).state == WaterLocoState::Dry);
    CHECK(w.pose().waterWeight == 0.f);
    in.water = water(WaterMode::Wade, 0.3f);
    const WaterLocoPose& p = w.step(in, kDt);
    CHECK(p.state == WaterLocoState::Wade);
    CHECK(p.changed);
    CHECK((p.cue & kWaterCueWaterIn) != 0u);
    CHECK(p.waterWeight == 0.f);
    CHECK(p.wadeArms == 0.f);
    in.water = water(WaterMode::DeepWade, 1.0f);
    const float deep = w.step(in, kDt).wadeArms;
    CHECK(deep > 0.5f);
    CHECK(deep < 1.f);
    in.water = water(WaterMode::Dry, 0.f);
    CHECK(w.step(in, kDt).state == WaterLocoState::Dry);
    CHECK((w.pose().cue & kWaterCueWaterOut) != 0u);
}

TEST_CASE("water loco: walking off a slope into swimming depth plays EnterWalk, then treads or swims") {
    WaterLoco w;
    WaterLocoInput in;
    in.water = water(WaterMode::DeepWade, 1.f);
    in.velocity = {1.2f, 0.f, 0.f};
    w.step(in, kDt);
    in.water = water(WaterMode::Swim);
    const WaterLocoPose& p = w.step(in, kDt);
    CHECK(p.state == WaterLocoState::EnterWalk);
    CHECK(p.blendSeconds == doctest::Approx(0.25f));
    CHECK(p.velocity.x > 1.f); // glides in at the walk's speed
    CHECK(weightOf(p, WaterClip::EnterWalk) == 1.f);
    in.move = {1.f, 0.f};
    CHECK(run(w, in, 1.1f).state == WaterLocoState::SurfaceSwim);
    WaterLoco still;
    WaterLocoInput s2;
    s2.water = water(WaterMode::Wade, 0.5f);
    still.step(s2, kDt);
    s2.water = water(WaterMode::Swim);
    still.step(s2, kDt);
    CHECK(run(still, s2, 1.1f).state == WaterLocoState::Tread);
}

TEST_CASE("water loco: a fast level entry goes in head first (EnterDive), then the splash's recover") {
    auto entry = [](float vx) {
        WaterLoco w;
        WaterLocoInput air;
        air.falling = true;
        air.velocity = {vx, 0.f, -6.f};
        w.step(air, kDt);
        WaterLocoInput in;
        in.water = water(WaterMode::Swim, 6.f);
        in.velocity = {vx, 0.f, -6.f};
        w.step(in, kDt);
        return w;
    };
    WaterLoco dive = entry(5.f);
    REQUIRE(dive.state() == WaterLocoState::EnterSplash);
    CHECK(dive.pose().layers[0].clip == WaterClip::EnterDive);
    WaterLoco drop = entry(1.f);
    REQUIRE(drop.state() == WaterLocoState::EnterSplash);
    CHECK(drop.pose().layers[0].clip == WaterClip::EnterSplash);
    // The recover after a head-first entry is the splash's tail: the head breaking out.
    WaterLocoInput still;
    still.water = water(WaterMode::Swim, 6.f);
    for (int i = 0; i < 300 && dive.state() == WaterLocoState::EnterSplash; ++i) dive.step(still, kDt);
    REQUIRE(dive.state() == WaterLocoState::Recover);
    CHECK(dive.pose().layers[0].clip == WaterClip::EnterSplash);
    CHECK(dive.pose().layers[0].phase >= 0.75f);
}

TEST_CASE("water loco: a slow drop is no splash; it treads at once") {
    WaterLoco w = fallInto(-2.f);
    CHECK(w.state() == WaterLocoState::Tread);
    CHECK(w.pose().blendSeconds == doctest::Approx(0.3f));
    CHECK((w.pose().cue & kWaterCueSplash) == 0u);
}

TEST_CASE("water loco: a fall sinks and rises by its speed, then a recover beat with a gasp, then the tread") {
    // 1 m (vz -4.4) to 4 m (vz -8.9): the sink grows with the speed, 0.3 m up to 1.5 m.
    float lastDepth = 0.f;
    for (float vz : {-4.4f, -6.3f, -7.7f, -8.9f}) {
        CAPTURE(vz);
        WaterLoco w = fallInto(vz, 6.f);
        const WaterLocoPose& p0 = w.pose();
        REQUIRE(p0.state == WaterLocoState::EnterSplash);
        CHECK((p0.cue & kWaterCueSplash) != 0u);
        const float k = (-vz - 3.f) / 6.f;
        CHECK(p0.cueStrength == doctest::Approx(k).epsilon(0.01));
        CHECK(p0.lockMove);
        CHECK(p0.lockActions);
        float deepest = 0.f, recoverAt = -1.f, tread = -1.f;
        bool gasp = false;
        WaterLocoInput in = swimIn({1.f, 0.f});
        for (int i = 0; i < 120; ++i) {
            const WaterLocoPose& p = w.step(in, kDt);
            deepest = std::fmin(deepest, p.sink);
            if (p.state == WaterLocoState::Recover && recoverAt < 0.f) {
                recoverAt = float(i) * kDt;
                gasp = (p.cue & kWaterCueGasp) != 0u;
                CHECK(p.sink == 0.f);
            }
            if (p.state == WaterLocoState::Recover) {
                CHECK(p.lockMove);
                CHECK(p.lockActions);
                CHECK(p.velocity.x == 0.f); // the move input is ignored
            }
            if (p.state != WaterLocoState::EnterSplash && p.state != WaterLocoState::Recover && tread < 0.f)
                tread = float(i) * kDt;
        }
        CHECK(-deepest == doctest::Approx(0.3f + 1.2f * k).epsilon(0.03));
        CHECK(-deepest > lastDepth);
        lastDepth = -deepest;
        CHECK(gasp);
        REQUIRE(recoverAt > 0.f);
        // The beat lasts 0.3 + 0.2 k s (one step of slack each side).
        CHECK(tread - recoverAt == doctest::Approx(0.3f + 0.2f * k).epsilon(0.15));
    }
}

TEST_CASE("water loco: a splash's sink stops short of the bed") {
    // A column of 2 m: the feet hang kWaterSwimDraft under the surface, so at most 2 - 1.28 - 0.3 = 0.42 m of sink.
    WaterLoco w = fallInto(-8.9f, 2.f);
    REQUIRE(w.state() == WaterLocoState::EnterSplash);
    float deepest = 0.f;
    for (int i = 0; i < 60; ++i) deepest = std::fmin(deepest, w.step(swimIn(), kDt).sink);
    CHECK(-deepest <= 2.f - kWaterSwimDraft - 0.3f + 1e-3f);
    CHECK(-deepest > 0.3f);
    // A column barely deep enough to swim: no room, no sink, and the splash still ends.
    WaterLoco shallow = fallInto(-8.9f, 1.3f);
    for (int i = 0; i < 90; ++i) CHECK(shallow.step(swimIn(), kDt).sink == 0.f);
    CHECK(shallow.state() == WaterLocoState::Tread);
}

TEST_CASE("water loco: a fall into wading depth is a splash cue with no sink") {
    for (WaterMode m : {WaterMode::Wade, WaterMode::DeepWade}) {
        WaterLoco w = fallInto(-7.f, 0.9f, m);
        CHECK(w.state() == WaterLocoState::Wade);
        CHECK((w.pose().cue & kWaterCueSplash) != 0u);
        CHECK(w.pose().cueStrength == doctest::Approx(4.f / 6.f));
        CHECK(w.pose().sink == 0.f);
        CHECK_FALSE(w.pose().lockMove);
    }
}

TEST_CASE("water loco: the tread strafes slowly and blends the directional clips by the local move") {
    WaterLoco w = treading();
    REQUIRE(w.state() == WaterLocoState::Tread);
    const WaterLocoPose& idle = w.pose();
    CHECK(weightOf(idle, WaterClip::TreadIdle) == doctest::Approx(1.f));
    CHECK(idle.headTargetZ == doctest::Approx(0.12f));
    // A gentle push to the right (yaw 0 faces +X, and right is +Y in the engine's frame): strafes, keeps the facing.
    WaterLocoInput in = swimIn({0.f, 0.4f});
    const WaterLocoPose& p = run(w, in, 2.f);
    CHECK(p.state == WaterLocoState::Tread);
    CHECK(p.yaw == doctest::Approx(0.f));
    CHECK(p.velocity.y == doctest::Approx(0.4f * 0.6f).epsilon(0.02));
    CHECK(p.strafe.y == doctest::Approx(0.4f));
    CHECK(weightOf(p, WaterClip::TreadR) == doctest::Approx(0.4f));
    CHECK(weightOf(p, WaterClip::TreadIdle) == doctest::Approx(0.6f));
    CHECK(weightSum(p) == doctest::Approx(1.f));
    // Back and left at once: both directional clips.
    const WaterLocoPose& q = run(w, swimIn({-0.3f, -0.3f}), 1.f);
    CHECK(weightOf(q, WaterClip::TreadBack) > 0.f);
    CHECK(weightOf(q, WaterClip::TreadL) > 0.f);
    CHECK(weightSum(q) == doctest::Approx(1.f));
    // Let go: the drag stops it.
    CHECK(run(w, swimIn(), 1.f).speed == doctest::Approx(0.f));
}

TEST_CASE("water loco: a strong push behind the treader turns it, then it swims") {
    WaterLoco w = treading();
    WaterLocoInput in = swimIn({-1.f, 0.f});
    const WaterLocoPose& p = w.step(in, kDt);
    CHECK(p.state == WaterLocoState::TreadTurn);
    CHECK(p.blendSeconds == doctest::Approx(0.2f));
    CHECK(std::fabs(p.turnRate) == doctest::Approx(120.f).epsilon(0.02));
    bool swam = false;
    for (int i = 0; i < 60 && !swam; ++i) {
        in.yaw = w.pose().yaw;
        swam = w.step(in, kDt).state == WaterLocoState::SurfaceSwim;
    }
    CHECK(swam);
    CHECK(std::fabs(water_loco_detail::wrapDeg(w.pose().yaw)) >= 120.f - 1.f);
}

TEST_CASE("water loco: the surface swim reaches its speeds, turns at its rate, leans, and glides out to a tread") {
    WaterLoco w = treading();
    WaterLocoInput in = swimIn({1.f, 0.f});
    CHECK(w.step(in, kDt).state == WaterLocoState::SurfaceSwim);
    CHECK(w.pose().blendSeconds == doctest::Approx(0.35f));
    const WaterLocoPose& p = run(w, in, 2.f);
    CHECK(p.speed == doctest::Approx(1.5f).epsilon(0.01));
    CHECK(weightOf(p, WaterClip::SwimSlow) == doctest::Approx(1.f));
    in.sprint = true;
    const WaterLocoPose& f = run(w, in, 2.f);
    CHECK(f.speed == doctest::Approx(2.4f).epsilon(0.01));
    CHECK(weightOf(f, WaterClip::SwimFast) == doctest::Approx(1.f));
    // Turning right (toward +Y): 150 deg/s, a lean into the turn.
    in.sprint = false;
    in.move = {0.f, 1.f};
    in.yaw = f.yaw;
    const WaterLocoPose& t = w.step(in, kDt);
    CHECK(t.turnRate == doctest::Approx(150.f).epsilon(0.02));
    CHECK(t.lean > 0.f);
    CHECK(t.lean <= 20.f);
    // Let go: it glides on, then treads once under 0.4 m/s.
    const WaterLocoPose& g = run(w, swimIn({}, t.yaw), 0.3f);
    CHECK(g.state == WaterLocoState::SurfaceSwim);
    CHECK(g.speed > 0.4f);
    const WaterLocoPose& h = run(w, swimIn({}, g.yaw), 3.f);
    CHECK(h.state == WaterLocoState::Tread);
    CHECK(h.from == WaterLocoState::SurfaceSwim);
    CHECK(h.blendSeconds == doctest::Approx(0.5f));
}

TEST_CASE("water loco: the stroke phase follows the travel and fires cues at the catch points") {
    WaterLoco w = treading();
    WaterLocoInput in = swimIn({1.f, 0.f});
    run(w, in, 2.f);
    // At 1.5 m/s and 2.4 m a cycle: one catch (both hands) every 1.6 s.
    int catches = 0;
    float phase0 = w.pose().strokePhase;
    for (int i = 0; i < 96; ++i) {
        const WaterLocoPose& p = w.step(in, kDt);
        if ((p.cue & kWaterCueStrokeL) && (p.cue & kWaterCueStrokeR)) ++catches;
    }
    CHECK(catches == 2);
    // Two whole cycles: back where it started.
    const float dp = std::fabs(w.pose().strokePhase - phase0);
    CHECK(std::fmin(dp, 1.f - dp) < 0.02f);
    // Sprinting (the crawl): three catches an arm a cycle, left and right apart.
    in.sprint = true;
    run(w, in, 2.f);
    int left = 0, right = 0;
    for (int i = 0; i < 90; ++i) {
        const WaterLocoPose& p = w.step(in, kDt);
        CHECK_FALSE(((p.cue & kWaterCueStrokeL) && (p.cue & kWaterCueStrokeR)));
        left += (p.cue & kWaterCueStrokeL) ? 1 : 0;
        right += (p.cue & kWaterCueStrokeR) ? 1 : 0;
        if (p.cue & (kWaterCueStrokeL | kWaterCueStrokeR)) CHECK(p.cueStrength == doctest::Approx(1.f));
    }
    CHECK(left >= 2);
    CHECK(std::abs(left - right) <= 1);
    // Treading: four kicks a 3 s cycle.
    WaterLoco t = treading();
    int kicks = 0;
    for (int i = 0; i < 90; ++i) kicks += (t.step(swimIn(), kDt).cue & kWaterCueKick) ? 1 : 0;
    CHECK(kicks == 4);
}

TEST_CASE("water loco: dive, under by speed and pitch, breach with a gasp on low breath, then the tread") {
    WaterLoco w = treading();
    WaterLocoInput in = swimIn();
    in.dived = true;
    in.swim = SwimState::Diving;
    const WaterLocoPose& d = w.step(in, kDt);
    CHECK(d.state == WaterLocoState::Dive);
    CHECK(d.blendSeconds == doctest::Approx(0.15f));
    CHECK(weightOf(d, WaterClip::DiveStart) == 1.f);
    in.dived = false;
    in.swimVelocity = {0.f, 0.f, -1.f};
    w.step(in, kDt);
    CHECK(w.state() == WaterLocoState::Dive);
    in.swim = SwimState::Under;
    // Hovering: UnderIdle.
    in.swimVelocity = {};
    const WaterLocoPose& h = run(w, in, 1.f);
    CHECK(h.state == WaterLocoState::Under);
    CHECK(weightOf(h, WaterClip::UnderIdle) == doctest::Approx(1.f));
    CHECK(h.pitch == doctest::Approx(0.f));
    // Level at Swim's speed: UnderSlow; at its sprint: UnderFast.
    in.swimVelocity = {2.2f, 0.f, 0.f};
    CHECK(weightOf(run(w, in, 0.5f), WaterClip::UnderSlow) == doctest::Approx(1.f));
    in.swimVelocity = {3.2f, 0.f, 0.f};
    const WaterLocoPose& fast = run(w, in, 0.5f);
    CHECK(weightOf(fast, WaterClip::UnderFast) == doctest::Approx(1.f));
    CHECK(fast.speed == doctest::Approx(3.2f));
    // Swimming steeply up: the pitch follows at its rate, clamped at 70, and the PitchUp pose takes a share.
    in.swimVelocity = {0.2f, 0.f, 2.2f};
    const WaterLocoPose& up1 = w.step(in, kDt);
    CHECK(up1.pitch == doctest::Approx(180.f * kDt).epsilon(0.01));
    const WaterLocoPose& up = run(w, in, 1.f);
    CHECK(up.pitch == doctest::Approx(70.f));
    CHECK(weightOf(up, WaterClip::PitchUp) == doctest::Approx(0.6f));
    CHECK(weightSum(up) == doctest::Approx(1.f));
    in.swimVelocity = {0.2f, 0.f, -2.2f};
    CHECK(weightOf(run(w, in, 1.f), WaterClip::PitchDown) == doctest::Approx(0.6f));
    // Surfacing short of breath: the breach, a splash and a gasp.
    in.swim = SwimState::Surface;
    in.surfaced = true;
    in.breathShare = 0.2f;
    const WaterLocoPose& b = w.step(in, kDt);
    CHECK(b.state == WaterLocoState::Breach);
    CHECK((b.cue & kWaterCueSplash) != 0u);
    CHECK((b.cue & kWaterCueGasp) != 0u);
    in.surfaced = false;
    CHECK(run(w, in, 1.f).state == WaterLocoState::Tread);
    CHECK(w.pose().pitch == doctest::Approx(0.f));
    // With breath to spare: no gasp.
    WaterLoco w2 = treading();
    WaterLocoInput u = swimIn();
    u.swim = SwimState::Under;
    run(w2, u, 0.5f);
    u.swim = SwimState::Surface;
    u.surfaced = true;
    u.breathShare = 0.8f;
    CHECK((w2.step(u, kDt).cue & kWaterCueGasp) == 0u);
}

TEST_CASE("water loco: under water follows Swim's state even without its flags") {
    WaterLoco w = treading();
    WaterLocoInput in = swimIn();
    in.swim = SwimState::Under; // no dived flag (a reset, a load)
    CHECK(w.step(in, kDt).state == WaterLocoState::Under);
    in.swim = SwimState::Surface; // no surfaced flag
    CHECK(w.step(in, kDt).state == WaterLocoState::Breach);
    // A dive that comes to nothing goes back to the tread.
    WaterLoco v = treading();
    WaterLocoInput d = swimIn();
    d.dived = true;
    v.step(d, kDt);
    d.dived = false;
    CHECK(run(v, d, 1.f).state == WaterLocoState::Tread);
}

TEST_CASE("water loco: rescued is limp and locked; after it, the tread") {
    WaterLoco w = treading();
    WaterLocoInput in = swimIn({1.f, 0.f});
    in.swim = SwimState::Rescued;
    const WaterLocoPose& r = w.step(in, kDt);
    CHECK(r.state == WaterLocoState::Rescued);
    CHECK(r.lockMove);
    CHECK(r.lockActions);
    CHECK(weightOf(r, WaterClip::UnderIdle) == 1.f);
    in.swim = SwimState::Surface;
    in.move = {};
    CHECK(w.step(in, kDt).state == WaterLocoState::Tread);
}

TEST_CASE("water loco: exits play the climb at the host's progress, with the hands on the ledge; then dry ground") {
    WaterLoco w = treading();
    WaterLocoInput in = swimIn();
    in.exitMove = WaterExitMove::Mantle;
    in.exitProgress = 0.4f;
    in.exitHand = {1.f, 0.f, 0.9f};
    const WaterLocoPose& m = w.step(in, kDt);
    CHECK(m.state == WaterLocoState::Exit);
    CHECK(m.blendSeconds == doctest::Approx(0.1f));
    CHECK(m.layers[0].clip == WaterClip::ExitMantle);
    CHECK(m.layers[0].phase == doctest::Approx(0.4f));
    CHECK(m.handIk);
    CHECK(m.handTarget.z == doctest::Approx(0.9f));
    CHECK(m.lockMove);
    CHECK(m.lockActions);
    in.exitMove = WaterExitMove::None;
    in.water = water(WaterMode::Dry, 0.f);
    const WaterLocoPose& out = w.step(in, kDt);
    CHECK(out.state == WaterLocoState::Dry);
    CHECK((out.cue & kWaterCueWaterOut) != 0u);
    // The ladder: its loop by the height climbed (0.6 m a cycle), then the top-out by progress.
    WaterLoco l = treading();
    WaterLocoInput lin = swimIn();
    lin.exitMove = WaterExitMove::Ladder;
    lin.ladderClimbed = 0.9f;
    const WaterLocoPose& lp = l.step(lin, kDt);
    CHECK(lp.layers[0].clip == WaterClip::ExitLadder);
    CHECK(lp.layers[0].phase == doctest::Approx(0.5f));
    CHECK_FALSE(lp.handIk);
    lin.exitMove = WaterExitMove::LadderTop;
    lin.exitProgress = 0.25f;
    const WaterLocoPose& top = l.step(lin, kDt);
    CHECK(top.state == WaterLocoState::Exit);
    CHECK(top.layers[0].clip == WaterClip::ExitLadderTop);
    CHECK(top.handIk);
    // An exit cancelled in the water: back to the tread.
    lin.exitMove = WaterExitMove::None;
    CHECK(l.step(lin, kDt).state == WaterLocoState::Tread);
}

TEST_CASE("water loco: the surface gives way to the wade only after outHold (no flicker)") {
    WaterLoco w = treading();
    WaterLocoInput in = swimIn();
    in.water = water(WaterMode::DeepWade, 1.1f);
    CHECK(w.step(in, kDt).state == WaterLocoState::Tread);
    CHECK(w.step(swimIn(), kDt).state == WaterLocoState::Tread); // back in: the hold restarts
    for (int i = 0; i < 5; ++i) CHECK(w.step(in, kDt).state == WaterLocoState::Tread);
    const WaterLocoPose& p = run(w, in, 0.1f);
    CHECK(p.state == WaterLocoState::Wade);
    CHECK(p.blendSeconds == doctest::Approx(0.4f));
}

TEST_CASE("water loco: bad input is replaced and reported; dt is clamped") {
    WaterLoco w = treading();
    WaterLocoInput in = swimIn({kNaN, 0.f});
    in.yaw = kNaN;
    const WaterLocoPose& p = w.step(in, kDt);
    CHECK_FALSE(p.inputOk);
    CHECK(std::isfinite(p.yaw));
    CHECK(std::isfinite(p.velocity.x));
    CHECK(w.step(swimIn(), kNaN).inputOk == false);
    CHECK(w.step(swimIn(), kDt).inputOk);
    // A move longer than 1 is scaled to 1.
    WaterLoco s = treading();
    const WaterLocoPose& q = run(s, swimIn({3.f, 0.f}), 3.f);
    CHECK(q.speed == doctest::Approx(1.5f).epsilon(0.01));
    // A huge dt is clamped to 0.25 s.
    WaterLoco t = treading();
    const float before = t.pose().stateTime;
    t.step(swimIn(), 10.f);
    CHECK(t.pose().stateTime == doctest::Approx(before + 0.25f));
}

TEST_CASE("water loco: every water state plays weights that sum to 1") {
    WaterLoco w;
    WaterLocoInput in = swimIn();
    const auto check = [&](const WaterLocoPose& p) {
        if (p.waterWeight > 0.f) {
            CHECK(p.layerCount >= 1);
            CHECK(weightSum(p) == doctest::Approx(1.f));
        }
    };
    for (int i = 0; i < 400; ++i) {
        in.move = {std::cos(float(i) * 0.05f), std::sin(float(i) * 0.03f)};
        in.sprint = (i / 50) % 2 == 1;
        in.swim = i >= 200 && i < 300 ? SwimState::Under : SwimState::Surface;
        in.dived = i == 200;
        in.surfaced = i == 300;
        in.swimVelocity = {std::cos(float(i) * 0.1f) * 3.f, 0.f, std::sin(float(i) * 0.07f) * 2.f};
        check(w.step(in, kDt));
        in.yaw = w.pose().yaw;
    }
}

TEST_CASE("water loco: a step allocates nothing") {
    WaterLoco w;
    REQUIRE(ftcore_test::allocHookInstalled());
    const std::uint64_t before = ftcore_test::allocStats().count;
    WaterLocoInput air;
    air.falling = true;
    air.velocity = {0.f, 0.f, -8.f};
    w.step(air, kDt);
    WaterLocoInput in = swimIn({1.f, 0.2f});
    for (int i = 0; i < 600; ++i) {
        in.sprint = i % 90 > 45;
        in.swim = i >= 300 && i < 420 ? SwimState::Under : SwimState::Surface;
        in.dived = i == 300;
        in.surfaced = i == 420;
        in.swimVelocity = {2.f, 0.f, i < 360 ? -1.f : 1.f};
        in.exitMove = i >= 540 ? WaterExitMove::Mantle : WaterExitMove::None;
        in.exitProgress = float(i - 540) / 60.f;
        w.step(in, kDt);
        in.yaw = w.pose().yaw;
    }
    CHECK(ftcore_test::allocStats().count == before);
}

TEST_CASE("water loco: clip and state names are unique and cover every value") {
    using namespace ftcore;
    for (int i = 0; i < kWaterClipCount; ++i)
        for (int j = i + 1; j < kWaterClipCount; ++j)
            CHECK(std::string_view(kWaterClipNames[i]) != std::string_view(kWaterClipNames[j]));
    CHECK(std::string_view(waterClipName(WaterClip::ExitLadderTop)) == "ExitLadderTop");
    CHECK(std::string_view(waterClipName(WaterClip::SwimFast)) == "SwimFast");
    CHECK(std::string_view(waterLocoStateName(WaterLocoState::Rescued)) == "Rescued");
    CHECK(std::string_view(waterLocoStateName(WaterLocoState::SurfaceSwim)) == "SurfaceSwim");
    CHECK(std::string_view(waterLocoStateName(WaterLocoState::Dry)) == "Dry");
}

TEST_CASE("water loco: the engine's frame: a push toward +Y strafes and turns to the right") {
    WaterLoco w = treading();
    // Toward +Y and a little back: a turn to the right (the yaw rising) on TreadTurnR90.
    const WaterLocoPose& p = w.step(swimIn({-0.2f, 1.f}), kDt);
    REQUIRE(p.state == WaterLocoState::TreadTurn);
    CHECK(weightOf(p, WaterClip::TreadTurnR90) == doctest::Approx(1.f));
    CHECK(p.turnRate > 0.f);
    // Toward -Y: TreadTurnL90, the yaw falling.
    WaterLoco v = treading();
    const WaterLocoPose& q = v.step(swimIn({-0.2f, -1.f}), kDt);
    REQUIRE(q.state == WaterLocoState::TreadTurn);
    CHECK(weightOf(q, WaterClip::TreadTurnL90) == doctest::Approx(1.f));
    CHECK(q.turnRate < 0.f);
}
