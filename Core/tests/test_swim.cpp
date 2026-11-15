// Underwater free swim (Swim.h): states, speeds, buoyancy, clamps, breath, drain, rescue, the edge and the guards.
#include "ftcore/Swim.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kDt = 0.1f;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// A deep basin: surface at 0, bed at -10, the lead at the surface, far from the edge.
SwimEnv deep(float z = 0.f) {
    SwimEnv e;
    e.position = {0.f, 0.f, z};
    e.surfaceZ = 0.f;
    e.bedZ = -10.f;
    e.column = 10.f;
    e.deepAllowed = true;
    return e;
}

SwimInput crouch() {
    SwimInput in;
    in.crouch = true;
    return in;
}

SwimInput jump() {
    SwimInput in;
    in.jump = true;
    return in;
}

SwimInput go(SwimVec look, float fwd = 1.f, float rgt = 0.f, bool sprint = false) {
    SwimInput in;
    in.look = look;
    in.forward = fwd;
    in.right = rgt;
    in.sprint = sprint;
    return in;
}

// Dives and steps on until Under; the env follows the step's position. Returns the env at the end.
SwimEnv diveToUnder(Swim& s, SwimEnv e) {
    REQUIRE(s.step(crouch(), e, kDt).dived);
    e.position = s.last().position;
    for (int i = 0; i < 100 && s.state() != SwimState::Under; ++i) {
        s.step(SwimInput{}, e, kDt);
        e.position = s.last().position;
    }
    REQUIRE(s.state() == SwimState::Under);
    return e;
}

float len(const SwimVec& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

} // namespace

TEST_CASE("swim: the defaults are valid and match Data/Swim/swim.json") {
    CHECK(validSwimTuning(SwimTuning{}));
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Swim/swim.json", text));
    SwimTuning t;
    t.swimSpeed = 9.f;
    std::string err;
    REQUIRE_MESSAGE(importSwimTuning(text, t, &err), err);
    const SwimTuning d;
    CHECK(t.swimSpeed == d.swimSpeed);
    CHECK(t.sprintSpeed == d.sprintSpeed);
    CHECK(t.buoyancy == d.buoyancy);
    CHECK(t.bedClearance == d.bedClearance);
    CHECK(t.diveDepth == d.diveDepth);
    CHECK(t.breathSeconds == d.breathSeconds);
    CHECK(t.sprintBreathRate == d.sprintBreathRate);
    CHECK(t.warningSeconds == d.warningSeconds);
    CHECK(t.drainPerSecond == d.drainPerSecond);
    CHECK(t.refillSeconds == d.refillSeconds);
    CHECK(t.rescueVitality == d.rescueVitality);
    CHECK(t.edgeRampMetres == d.edgeRampMetres);
    CHECK(t.edgePushSpeed == d.edgePushSpeed);
    CHECK(d.swimSpeed == 2.2f);
    CHECK(d.sprintSpeed == 3.2f);
    CHECK(d.buoyancy == 0.2f);
    CHECK(d.bedClearance == 0.3f);
    CHECK(d.breathSeconds == 45.f);
    CHECK(d.sprintBreathRate == 2.f);
    CHECK(d.warningSeconds == 10.f);
    CHECK(d.drainPerSecond == 0.05f);
    CHECK(d.refillSeconds == 2.f);
    CHECK(d.rescueVitality == 0.25f);
    CHECK(d.edgeRampMetres == 8.f);
    CHECK(kSwimDeepMin == 3.f);
}

TEST_CASE("swim: the importer reads fields; missing, bad and out-of-range ones take their defaults and are noted") {
    SwimTuning t;
    std::string err;
    std::vector<std::string> notes;
    const auto noted = [&](const char* what) {
        for (const std::string& n : notes)
            if (n.find(what) != std::string::npos) return true;
        return false;
    };
    REQUIRE(importSwimTuning(R"({"version":1,"swimSpeed":2.5,"breathSeconds":60})", t, &err, &notes));
    CHECK(t.swimSpeed == 2.5f);
    CHECK(t.breathSeconds == 60.f);
    CHECK(t.sprintSpeed == 3.2f); // missing: the default, and noted
    CHECK(notes.size() == 11u);
    CHECK(noted("sprintSpeed: missing, the default 3.2 used"));
    CHECK(noted("edgePushSpeed: missing, the default 4 used"));
    CHECK_FALSE(noted("swimSpeed"));
    // Out of range or not a number: its default, noted; an unknown key skipped, noted; still ok.
    REQUIRE(importSwimTuning(R"({"version":1,"swimSpeed":"fast","buoyancy":-1,"breathSeconds":60,"gills":true})", t, &err, &notes));
    CHECK(err.empty());
    CHECK(t.swimSpeed == 2.2f);
    CHECK(t.buoyancy == 0.2f);
    CHECK(t.breathSeconds == 60.f);
    CHECK(noted("swimSpeed: expected a number in 0.5..6, the default 2.2 used"));
    CHECK(noted("buoyancy: expected a number in 0..2, the default 0.2 used"));
    CHECK(noted("gills: unknown field, skipped"));
    // The whole file from Data: no notes.
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Swim/swim.json", text));
    REQUIRE(importSwimTuning(text, t, &err, &notes));
    CHECK(notes.empty());
    // Only a file that cannot be read fails, with the defaults.
    t.swimSpeed = 1.f;
    CHECK_FALSE(importSwimTuning(R"({"version":2,"swimSpeed":3})", t, &err, &notes));
    CHECK(t.swimSpeed == SwimTuning{}.swimSpeed);
    CHECK(err.find("version: expected 1") != std::string::npos);
    CHECK_FALSE(importSwimTuning("[1]", t, &err));
    CHECK(err.find("expected an object") != std::string::npos);
    CHECK_FALSE(importSwimTuning("{", t, &err));
    CHECK(err.find("swim file") != std::string::npos);
    // The cross-field rules: the later field back to its default, noted.
    REQUIRE(importSwimTuning(R"({"version":1,"swimSpeed":3,"sprintSpeed":2.5})", t, &err, &notes));
    CHECK(t.swimSpeed == 3.f);
    CHECK(t.sprintSpeed == 3.2f);
    CHECK(noted("sprintSpeed: expected at least swimSpeed"));
    REQUIRE(importSwimTuning(R"({"version":1,"breathSeconds":8,"warningSeconds":9})", t, &err, &notes));
    CHECK(t.warningSeconds == 10.f);
    CHECK(noted("warningSeconds: expected at most breathSeconds"));
    CHECK(noted("every default used")); // the default 10 s is still over 8 s
    CHECK(t.breathSeconds == 45.f);
    REQUIRE(importSwimTuning(R"({"version":1,"edgePushSpeed":3.2})", t, &err, &notes));
    CHECK(t.edgePushSpeed == 4.f);
    CHECK(noted("edgePushSpeed: expected above sprintSpeed"));
    CHECK(validSwimTuning(t));
}

TEST_CASE("swim: a refused tuning becomes the defaults; validSwimTuning checks every range") {
    SwimTuning bad;
    bad.breathSeconds = kNaN;
    CHECK_FALSE(validSwimTuning(bad));
    Swim s(bad);
    CHECK_FALSE(s.tuningOk());
    CHECK(s.tuning().breathSeconds == 45.f);
    CHECK(s.breath() == 45.f);
    for (float SwimTuning::*f : {&SwimTuning::swimSpeed, &SwimTuning::buoyancy, &SwimTuning::refillSeconds,
                                 &SwimTuning::drainPerSecond, &SwimTuning::edgeRampMetres}) {
        SwimTuning t;
        t.*f = -1.f;
        CHECK_FALSE(validSwimTuning(t));
        t.*f = kInf;
        CHECK_FALSE(validSwimTuning(t));
    }
    SwimTuning ok;
    ok.breathSeconds = 60.f;
    Swim u(ok);
    CHECK(u.tuningOk());
    CHECK(u.breath() == 60.f);
}

TEST_CASE("swim: a crouch at the surface dives where the water is deep, else it is refused") {
    {
        Swim s;
        const SwimStep& r = s.step(crouch(), deep(), kDt);
        CHECK(r.from == SwimState::Surface);
        CHECK(r.state == SwimState::Diving);
        CHECK(r.dived);
        CHECK_FALSE(r.diveRefused);
        CHECK(r.velocity.z == doctest::Approx(-2.2f));
        CHECK(r.position.z == doctest::Approx(-0.22f));
    }
    {
        Swim s; // not deep water
        SwimEnv e = deep();
        e.deepAllowed = false;
        const SwimStep& r = s.step(crouch(), e, kDt);
        CHECK(r.state == SwimState::Surface);
        CHECK(r.diveRefused);
        CHECK_FALSE(r.dived);
    }
    for (const float column : {2.99f, 3.f}) {
        CAPTURE(column);
        Swim s; // the column's minimum
        SwimEnv e = deep();
        e.bedZ = -column;
        e.column = column;
        const SwimStep& r = s.step(crouch(), e, kDt);
        CHECK(r.envOk);
        CHECK(r.dived == (column >= kSwimDeepMin));
        CHECK(r.diveRefused == (column < kSwimDeepMin));
    }
    Swim s; // no crouch: nothing; a jump at the surface: nothing
    CHECK(s.step(SwimInput{}, deep(), kDt).state == SwimState::Surface);
    const SwimStep& r = s.step(jump(), deep(), kDt);
    CHECK(r.state == SwimState::Surface);
    CHECK_FALSE(r.diveRefused);
    CHECK_FALSE(r.surfaced);
}

TEST_CASE("swim: a dive goes straight down to diveDepth, then Under; or to the bed's clearance in a shallow column") {
    Swim s;
    SwimEnv e = deep();
    s.step(crouch(), e, kDt);
    e.position = s.last().position;
    int steps = 1;
    while (s.state() == SwimState::Diving && steps < 100) {
        const SwimStep& r = s.step(go({1.f, 0.f, 0.f}), e, kDt); // the move axes do not steer a dive
        CHECK(r.position.x == 0.f);
        e.position = r.position;
        ++steps;
    }
    CHECK(s.state() == SwimState::Under);
    CHECK(steps == 5); // 1.0 m at 2.2 m/s in 0.1 s steps
    CHECK(e.position.z <= -1.f + 1e-4f);
    // A 3.2 m column, bed clearance 2 and diveDepth 3: the floor (-1.2) is above the dive depth, the dive ends there.
    SwimTuning t;
    t.bedClearance = 2.f;
    t.diveDepth = 3.f;
    Swim u(t);
    SwimEnv f = deep();
    f.bedZ = -3.2f;
    f.column = 3.2f;
    u.step(crouch(), f, kDt);
    f.position = u.last().position;
    for (int i = 0; i < 20 && u.state() == SwimState::Diving; ++i) {
        u.step(SwimInput{}, f, kDt);
        f.position = u.last().position;
    }
    CHECK(u.state() == SwimState::Under);
    CHECK(f.position.z == doctest::Approx(-1.2f)); // held at the bed's clearance
}

TEST_CASE("swim: under water the lead swims along the look in 3D at 2.2 m/s, sprint 3.2 m/s") {
    Swim s;
    SwimEnv e = diveToUnder(s, deep());
    e.position.z = -5.f;
    const float r2 = std::sqrt(0.5f);
    {
        const SwimStep& r = s.step(go({1.f, 0.f, -1.f}), e, kDt); // down at 45 degrees
        CHECK(r.state == SwimState::Under);
        CHECK(len(r.velocity) == doctest::Approx(2.2f));
        CHECK(r.velocity.x == doctest::Approx(2.2f * r2));
        CHECK(r.velocity.z == doctest::Approx(-2.2f * r2));
        CHECK(r.position.z == doctest::Approx(-5.f - 0.22f * r2));
        CHECK_FALSE(r.sprinting);
    }
    {
        const SwimStep& r = s.step(go({0.f, 2.f, 0.f}, 1.f, 0.f, true), e, kDt); // not unit: normalised
        CHECK(r.inputOk);
        CHECK(r.sprinting);
        CHECK(r.velocity.y == doctest::Approx(3.2f));
        CHECK(len(r.velocity) == doctest::Approx(3.2f));
    }
    {
        const SwimStep& r = s.step(go({1.f, 0.f, 0.f}, 0.f, 1.f), e, kDt); // right of +x is +y (up x look)
        CHECK(r.velocity.y == doctest::Approx(2.2f));
        CHECK(r.velocity.x == doctest::Approx(0.f));
        CHECK(r.velocity.z == doctest::Approx(0.f));
    }
    {
        const SwimStep& r = s.step(go({1.f, 0.f, 0.f}, 1.f, 1.f), e, kDt); // both axes: no faster
        CHECK(len(r.velocity) == doctest::Approx(2.2f));
        const SwimStep& b = s.step(go({1.f, 0.f, 0.f}, -1.f, 0.f), e, kDt); // back
        CHECK(b.velocity.x == doctest::Approx(-2.2f));
    }
    {
        const SwimStep& r = s.step(go({0.f, 0.f, -1.f}, 0.f, 1.f), e, kDt); // straight down: the last good right
        CHECK(r.velocity.y == doctest::Approx(2.2f));
    }
    CHECK(s.step(SwimInput{}, e, kDt).sprinting == false);
    SwimInput idle;
    idle.sprint = true; // sprint with no move input is not a sprint
    CHECK_FALSE(s.step(idle, e, kDt).sprinting);
}

TEST_CASE("swim: with no move input the lead drifts up at 0.2 m/s and reaching the surface hands back to Surface") {
    Swim s;
    SwimEnv e = diveToUnder(s, deep());
    e.position.z = -1.f;
    const SwimStep& r = s.step(SwimInput{}, e, kDt);
    CHECK(r.velocity.x == 0.f);
    CHECK(r.velocity.z == doctest::Approx(0.2f));
    CHECK(r.position.z == doctest::Approx(-0.98f));
    CHECK(r.state == SwimState::Under);
    e.position.z = -0.01f;
    const SwimStep& u = s.step(SwimInput{}, e, kDt);
    CHECK(u.position.z == 0.f); // stopped at the surface
    CHECK(u.velocity.z == doctest::Approx(0.1f));
    CHECK(u.state == SwimState::Surface);
    CHECK(u.surfaced);
    CHECK(u.from == SwimState::Under);
}

TEST_CASE("swim: held 0.3 m off the bed and stopped at the surface in every state") {
    Swim s;
    SwimEnv e = diveToUnder(s, deep());
    e.position.z = -9.6f;
    const SwimStep& r = s.step(go({0.f, 0.f, -1.f}), e, kDt);
    CHECK(r.position.z == doctest::Approx(-9.7f));
    CHECK(r.velocity.z == doctest::Approx(-1.f));
    e.position = r.position;
    const SwimStep& u = s.step(go({0.f, 0.f, -1.f}), e, kDt);
    CHECK(u.position.z == doctest::Approx(-9.7f));
    CHECK(u.velocity.z == doctest::Approx(0.f));
    e.position.z = -12.f; // the host put the body under the bed: lifted to the clearance
    CHECK(s.step(SwimInput{}, e, kDt).position.z == doctest::Approx(-9.7f));
    e.position.z = 0.5f; // over the surface: stopped there (and so at the surface)
    const SwimStep& w = s.step(go({0.f, 0.f, 1.f}), e, kDt);
    CHECK(w.position.z == 0.f);
    CHECK(w.state == SwimState::Surface);
    // At the surface the host's surface swim moves the body: Swim passes it through.
    SwimEnv f = deep();
    f.position = {3.f, 4.f, 0.f};
    const SwimStep& p = s.step(go({1.f, 0.f, 0.f}, 1.f, 0.f, true), f, kDt);
    CHECK(p.position.x == 3.f);
    CHECK(p.position.y == 4.f);
    CHECK(p.position.z == 0.f);
    CHECK(len(p.velocity) == 0.f);
    CHECK_FALSE(p.sprinting);
}

TEST_CASE("swim: a jump rises straight up at swimSpeed to the surface, then Surface") {
    for (const bool fromDive : {false, true}) {
        CAPTURE(fromDive);
        Swim s;
        SwimEnv e = deep();
        if (fromDive) {
            s.step(crouch(), e, kDt);
            e.position = s.last().position;
            s.step(SwimInput{}, e, kDt); // 0.44 m down, still diving
            e.position = s.last().position;
            REQUIRE(s.state() == SwimState::Diving);
        } else {
            e = diveToUnder(s, e);
            e.position.z = -2.f;
        }
        SwimInput in = go({1.f, 0.f, 0.f});
        in.jump = true;
        const SwimStep& r = s.step(in, e, kDt);
        CHECK(r.state == SwimState::Surfacing);
        CHECK(r.velocity.x == 0.f);
        CHECK(r.velocity.z == doctest::Approx(2.2f));
        e.position = r.position;
        int steps = 0;
        while (s.state() == SwimState::Surfacing && steps < 100) {
            const SwimStep& q = s.step(crouch(), e, kDt); // a crouch does not dive while surfacing
            CHECK_FALSE(q.dived);
            e.position = q.position;
            ++steps;
        }
        CHECK(s.state() == SwimState::Surface);
        CHECK(s.last().surfaced);
        CHECK(s.last().from == SwimState::Surfacing);
        CHECK(e.position.z == 0.f);
    }
}

TEST_CASE("swim: the flags: no attacks, no dodge, weapons stowed, interact allowed, in every state") {
    Swim s;
    SwimEnv e = deep();
    for (int i = 0; i < 12; ++i) {
        const SwimInput in = i == 0 ? crouch() : (i == 8 ? jump() : go({1.f, 0.f, -0.2f}));
        const SwimStep& r = s.step(in, e, kDt);
        e.position = r.position;
        CAPTURE(int(r.state));
        CHECK_FALSE(r.flags.attack);
        CHECK_FALSE(r.flags.dodge);
        CHECK_FALSE(r.flags.weaponsOut);
        CHECK(r.flags.interact);
    }
}

TEST_CASE("swim: breath lasts 45 s under water, sprint at double cost; the warning from 10 s left") {
    Swim s;
    SwimEnv e = diveToUnder(s, deep());
    e.position.z = -5.f;
    const float start = s.breath();
    CHECK(start == doctest::Approx(45.f - 0.5f).epsilon(0.01)); // the dive's 5 steps
    const SwimStep& r = s.step(SwimInput{}, e, kDt);
    CHECK(r.breath == doctest::Approx(start - 0.1f));
    CHECK_FALSE(r.refilling);
    CHECK_FALSE(r.warning);
    const SwimStep& q = s.step(go({1.f, 0.f, 0.f}, 1.f, 0.f, true), e, kDt);
    CHECK(q.sprinting);
    CHECK(q.breath == doctest::Approx(start - 0.3f));
    CHECK(q.breathShare == doctest::Approx((start - 0.3f) / 45.f));
    // Down to the warning.
    for (int i = 0; i < 1000 && s.breath() > 10.05f; ++i) s.step(SwimInput{}, e, kDt);
    CHECK_FALSE(s.last().warning);
    const SwimStep& w = s.step(SwimInput{}, e, kDt);
    CHECK(w.breath <= 10.f);
    CHECK(w.warning);
    CHECK_FALSE(w.draining);
    CHECK(w.vitality == 1.f);
}

TEST_CASE("swim: at no breath vitality drains 5% a second, only for the part of the step past empty") {
    Swim s;
    SwimEnv e = diveToUnder(s, deep());
    e.position.z = -5.f;
    for (int i = 0; i < 1000 && s.breath() > 0.15f; ++i) s.step(SwimInput{}, e, kDt);
    s.step(SwimInput{}, e, 0.05f);
    const float left = s.breath(); // about 0.05 s
    REQUIRE(left > 0.03f);
    REQUIRE(left < 0.07f);
    const SwimStep& r = s.step(SwimInput{}, e, kDt);
    CHECK(r.breath == 0.f);
    CHECK(r.draining);
    CHECK(r.warning);
    CHECK(r.vitality == doctest::Approx(1.f - 0.05f * (kDt - left)));
    const float v = r.vitality;
    const SwimStep& u = s.step(SwimInput{}, e, 0.2f);
    CHECK(u.vitality == doctest::Approx(v - 0.01f));
    // Sprinting drains no faster: the drain is on time, not on breath.
    const SwimStep& w = s.step(go({1.f, 0.f, 0.f}, 1.f, 0.f, true), e, 0.2f);
    CHECK(w.vitality == doctest::Approx(v - 0.02f));
}

TEST_CASE("swim: breath refills in 2 s at the surface or in an air pocket") {
    Swim s;
    SwimEnv e = diveToUnder(s, deep());
    e.position.z = -5.f;
    for (int i = 0; i < 1000 && s.breath() > 0.f; ++i) s.step(SwimInput{}, e, 0.25f); // empty
    REQUIRE(s.breath() == 0.f);
    {
        SwimEnv p = e; // an air pocket under water: refill there
        p.airPocket = true;
        const SwimStep& r = s.step(SwimInput{}, p, 0.25f);
        CHECK(r.refilling);
        CHECK(r.state == SwimState::Under);
        CHECK(r.breath == doctest::Approx(45.f / 2.f * 0.25f));
        CHECK_FALSE(r.warning);
        CHECK_FALSE(r.draining);
        const float v = r.vitality;
        for (int i = 0; i < 8; ++i) s.step(SwimInput{}, p, 0.25f);
        CHECK(s.breath() == 45.f); // full in 2 s, no more
        CHECK(s.vitality() == v);  // vitality is not refilled
    }
    for (int i = 0; i < 1000 && s.breath() > 0.f; ++i) s.step(SwimInput{}, e, 0.25f);
    s.step(jump(), e, kDt);
    e.position = s.last().position;
    for (int i = 0; i < 100 && s.state() != SwimState::Surface; ++i) {
        s.step(SwimInput{}, e, kDt);
        e.position = s.last().position;
    }
    const float b = s.breath();
    CHECK(s.last().refilling);
    const SwimStep& r = s.step(SwimInput{}, e, 0.25f);
    CHECK(r.breath == doctest::Approx(b + 45.f / 2.f * 0.25f));
    CHECK_FALSE(r.draining);
}

TEST_CASE("swim: vitality 0 under water is a rescue: Rescued, left in place, until the host's reset at the exit") {
    Swim s;
    SwimEnv e = diveToUnder(s, deep());
    e.position = {2.f, 3.f, -5.f};
    for (int i = 0; i < 1000 && s.breath() > 0.f; ++i) s.step(SwimInput{}, e, 0.25f);
    REQUIRE(s.breath() == 0.f);
    REQUIRE_FALSE(s.last().rescue);
    s.setVitality(0.004f); // 0.25 s of drain takes 0.0125
    const SwimStep& r = s.step(go({1.f, 0.f, 0.f}), e, 0.25f);
    CHECK(r.rescue);
    CHECK(r.state == SwimState::Rescued);
    CHECK(r.from == SwimState::Under);
    CHECK_FALSE(r.surfaced);
    CHECK(r.vitality == 0.f);
    CHECK_FALSE(r.draining);
    CHECK(r.position.x == 2.f); // the host carries the lead; Swim does not move it
    CHECK(r.position.z == -5.f);
    CHECK(len(r.velocity) == 0.f);
    // Left in place: no refill (not even at the surface or in an air pocket), no dive, no move from input.
    SwimEnv pocket = e;
    pocket.airPocket = true;
    for (int i = 0; i < 20; ++i) {
        CAPTURE(i);
        SwimInput in = go({0.f, 0.f, 1.f}, 1.f, 0.5f, true);
        in.crouch = i % 2 == 0;
        in.jump = i % 3 == 0;
        const SwimStep& q = s.step(in, i % 2 == 0 ? pocket : deep(), 0.25f);
        CHECK(q.state == SwimState::Rescued);
        CHECK_FALSE(q.rescue); // raised once
        CHECK_FALSE(q.dived);
        CHECK_FALSE(q.diveRefused);
        CHECK_FALSE(q.refilling);
        CHECK(q.breath == 0.f);
        CHECK(q.vitality == 0.f);
        CHECK(len(q.velocity) == 0.f);
        CHECK(q.position.x == (i % 2 == 0 ? 2.f : 0.f)); // where the env puts it
    }
    // The host's reset at the exit: Surface, full breath, the rescue's vitality.
    s.reset();
    CHECK(s.state() == SwimState::Surface);
    CHECK(s.breath() == 45.f);
    CHECK(s.vitality() == 0.25f);
    CHECK(s.step(SwimInput{}, deep(), kDt).state == SwimState::Surface);
    s.reset(); // a later reset (not ending a rescue): full vitality
    CHECK(s.vitality() == 1.f);
    // Vitality set to 0 at the surface is no rescue; the first step under water is.
    Swim u;
    u.setVitality(0.f);
    CHECK_FALSE(u.step(SwimInput{}, deep(), kDt).rescue);
    CHECK(u.step(crouch(), deep(), kDt).rescue);
    CHECK(u.state() == SwimState::Rescued);
    // setVitality guards.
    u.setVitality(kNaN);
    CHECK(u.vitality() == 0.f);
    u.setVitality(3.f);
    CHECK(u.vitality() == 1.f);
    u.setVitality(-1.f);
    CHECK(u.vitality() == 0.f);
}

TEST_CASE("swim: the outer edge pushes back under water, rising over the last 8 m, so a sprint cannot pass") {
    Swim s;
    SwimEnv e = diveToUnder(s, deep());
    e.position.z = -5.f;
    e.edgeInward = {-2.f, 0.f, 0.f}; // the edge is toward +x; normalised
    const SwimInput out = go({1.f, 0.f, 0.f}, 1.f, 0.f, true);
    for (const float d : {9.f, 8.f, 6.f, 2.f, 0.f, -3.f}) {
        CAPTURE(d);
        e.edgeDistance = d;
        const SwimStep& r = s.step(out, e, kDt);
        const float share = d >= 8.f ? 0.f : (d <= 0.f ? 1.f : 1.f - d / 8.f);
        CHECK(r.pushBack == doctest::Approx(share));
        CHECK(r.velocity.x == doctest::Approx(3.2f - 4.f * share));
    }
    e.edgeDistance = 0.f;
    CHECK(s.step(out, e, kDt).velocity.x < 0.f); // at the edge a sprint is pushed back in
    // Steps from inside the ramp never reach the edge.
    SwimEnv f = e;
    f.position.x = 0.f;
    for (int i = 0; i < 200; ++i) {
        f.edgeDistance = 4.f - f.position.x;
        f.position = s.step(out, f, kDt).position;
    }
    CHECK(4.f - f.position.x > 1.5f); // stays about 1.6 m inside (4 * share == 3.2)
    // No direction, or a bad one: no push. Not finite distance: far.
    e.edgeInward = {0.f, 0.f, 0.f};
    CHECK(s.step(out, e, kDt).pushBack == 0.f);
    e.edgeInward = {kNaN, 1.f, 0.f};
    CHECK(s.step(out, e, kDt).pushBack == 0.f);
    e.edgeInward = {-1.f, 0.f, 0.f};
    e.edgeDistance = kNaN;
    CHECK(s.step(out, e, kDt).pushBack == 0.f);
    // At the surface: no push (the host's surface swim).
    Swim u;
    SwimEnv g = deep();
    g.edgeInward = {-1.f, 0.f, 0.f};
    g.edgeDistance = 0.f;
    const SwimStep& p = u.step(out, g, kDt);
    CHECK(p.pushBack == 0.f);
    CHECK(len(p.velocity) == 0.f);
    // Diving and surfacing are pushed too.
    const SwimStep& q = u.step(crouch(), g, kDt);
    CHECK(q.state == SwimState::Diving);
    CHECK(q.pushBack == 1.f);
    CHECK(q.velocity.x == doctest::Approx(-4.f));
}

TEST_CASE("swim: bad inputs are replaced, never trusted") {
    Swim s;
    SwimEnv e = diveToUnder(s, deep());
    e.position.z = -5.f;
    s.step(go({0.f, 1.f, 0.f}), e, kDt); // the last good look: +y
    for (const SwimVec look : {SwimVec{0.f, 0.f, 0.f}, SwimVec{kNaN, 0.f, 0.f}, SwimVec{kInf, 1.f, 0.f}}) {
        const SwimStep& r = s.step(go(look), e, kDt);
        CHECK_FALSE(r.inputOk);
        CHECK(r.velocity.y == doctest::Approx(2.2f));
        CHECK(r.velocity.x == doctest::Approx(0.f));
    }
    for (const float bad : {kNaN, kInf, -kInf}) {
        CAPTURE(bad);
        const SwimStep& r = s.step(go({1.f, 0.f, 0.f}, bad, 0.f), e, kDt);
        CHECK_FALSE(r.inputOk);
        CHECK(r.velocity.x == 0.f);           // no move input: buoyancy
        CHECK(r.velocity.z == doctest::Approx(0.2f));
        CHECK(std::isfinite(r.position.x));
    }
    {
        const SwimStep& r = s.step(go({1.f, 0.f, 0.f}, 5.f, 0.f), e, kDt); // out of range: clamped
        CHECK_FALSE(r.inputOk);
        CHECK(r.velocity.x == doctest::Approx(2.2f));
        CHECK(s.step(go({1.f, 0.f, 0.f}, 0.5f, 0.f), e, kDt).inputOk);
        CHECK(s.last().velocity.x == doctest::Approx(1.1f));
    }
    // dt: negative or not finite is 0 (nothing moves, no breath used); a long step is clamped.
    for (const float dt : {-1.f, kNaN, kInf}) {
        CAPTURE(dt);
        const float b = s.breath();
        const SwimStep& r = s.step(go({1.f, 0.f, 0.f}), e, dt);
        CHECK(r.dt == 0.f);
        CHECK(r.position.x == e.position.x);
        CHECK(r.breath == b);
        CHECK(r.velocity.x == doctest::Approx(2.2f)); // the asked move
    }
    CHECK(s.step(SwimInput{}, e, 10.f).dt == kSwimMaxStep);
    CHECK(s.last().position.z == doctest::Approx(-5.f + 0.2f * kSwimMaxStep));
}

TEST_CASE("swim: a bad env changes nothing; the env's position if finite, else the last") {
    Swim s;
    SwimEnv e = diveToUnder(s, deep());
    e.position.z = -5.f;
    const SwimStep& good = s.step(SwimInput{}, e, kDt);
    const SwimVec at = good.position;
    const float b = s.breath();
    SwimEnv bads[5];
    for (SwimEnv& x : bads) x = e;
    bads[0].position.x = kNaN;
    bads[1].surfaceZ = kInf;
    bads[2].bedZ = kNaN;
    bads[3].bedZ = 0.f; // bed at the surface
    bads[4].bedZ = 1.f; // bed over it
    for (int i = 0; i < 5; ++i) {
        CAPTURE(i);
        SwimInput in = go({0.f, 0.f, 1.f});
        in.jump = i % 2 == 0;
        const SwimStep& r = s.step(in, bads[i], kDt);
        CHECK_FALSE(r.envOk);
        CHECK(r.state == SwimState::Under);
        CHECK(r.position.z == (i == 0 ? at.z : e.position.z));
        CHECK(r.position.x == (i == 0 ? at.x : e.position.x));
        CHECK(r.breath == b);
        CHECK(s.state() == SwimState::Under);
        CHECK(s.breath() == b);
    }
    CHECK(s.step(SwimInput{}, e, kDt).envOk);
}

TEST_CASE("swim: a bad column is reported and replaced by surface minus bed; the step goes on") {
    for (const float col : {kNaN, kInf, -1.f, 9.9f, 10.06f, 2.f}) {
        CAPTURE(col);
        // At the surface: a dive goes by surface minus bed (10 m), not the column.
        Swim s;
        SwimEnv e = deep();
        e.column = col;
        const SwimStep& r = s.step(crouch(), e, kDt);
        CHECK_FALSE(r.envOk);
        CHECK(r.dived);
        CHECK(r.state == SwimState::Diving);
        CHECK(r.position.z == doctest::Approx(-0.22f));
        // A shallow pool (2 m) whatever the column claims: refused.
        Swim u;
        SwimEnv p = deep();
        p.bedZ = -2.f;
        p.column = col == 2.f ? 10.f : col;
        const SwimStep& q = u.step(crouch(), p, kDt);
        CHECK_FALSE(q.envOk);
        CHECK(q.diveRefused);
    }
    // Within 0.05 m it is fine.
    Swim s;
    SwimEnv e = deep();
    e.column = 10.04f;
    CHECK(s.step(crouch(), e, kDt).envOk);
    e.column = 9.96f;
    CHECK(s.step(SwimInput{}, e, kDt).envOk);
}

TEST_CASE("swim: edge inputs: NaN or +inf distance, or a bad inward, is no push and reported; 0 or less (-inf too) is the full push") {
    Swim s;
    SwimEnv e = diveToUnder(s, deep());
    e.position.z = -5.f;
    e.edgeInward = {-1.f, 0.f, 0.f};
    for (const float d : {kNaN, kInf}) {
        e.edgeDistance = d;
        const SwimStep& r = s.step(SwimInput{}, e, kDt);
        CHECK_FALSE(r.envOk);
        CHECK(r.pushBack == 0.f);
        CHECK(r.position.x == 0.f);
        CHECK(r.state == SwimState::Under);
        CHECK(r.position.z == doctest::Approx(-5.f + 0.2f * kDt)); // the step went on
    }
    for (const SwimVec in : {SwimVec{kNaN, 0.f, 0.f}, SwimVec{-1.f, kInf, 0.f}}) {
        e.edgeDistance = -1.f;
        e.edgeInward = in;
        const SwimStep& r = s.step(SwimInput{}, e, kDt);
        CHECK_FALSE(r.envOk);
        CHECK(r.pushBack == 0.f);
        CHECK(r.position.x == 0.f);
    }
    e.edgeInward = {-1.f, 0.f, 0.f};
    for (const float d : {-kInf, -1e30f, -3.f, 0.f}) {
        CAPTURE(d);
        e.edgeDistance = d;
        const SwimStep& r = s.step(SwimInput{}, e, kDt);
        CHECK(r.envOk);
        CHECK(r.pushBack == 1.f);
        CHECK(r.position.x == doctest::Approx(-4.f * kDt));
    }
    e.edgeInward = {}; // zero inward: no push, not an error
    e.edgeDistance = 0.f;
    const SwimStep& z = s.step(SwimInput{}, e, kDt);
    CHECK(z.envOk);
    CHECK(z.pushBack == 0.f);
}

TEST_CASE("swim: a huge or tiny finite look normalises; never a silent zero") {
    struct Case {
        SwimVec look;
        SwimVec unit;
    };
    const float h = 0.70710678f;
    for (const Case c : {Case{{1e30f, 0.f, -1e30f}, {h, 0.f, -h}}, Case{{3e38f, 3e38f, 0.f}, {h, h, 0.f}},
                         Case{{1e-30f, 0.f, -1e-30f}, {h, 0.f, -h}}, Case{{-1e38f, 1.f, 0.f}, {-1.f, 0.f, 0.f}}}) {
        CAPTURE(c.look.x);
        Swim s;
        SwimEnv e = diveToUnder(s, deep());
        e.position.z = -5.f;
        const SwimStep& r = s.step(go(c.look), e, kDt);
        CHECK(r.inputOk);
        CHECK(len(r.velocity) == doctest::Approx(2.2f));
        CHECK(r.velocity.x == doctest::Approx(2.2f * c.unit.x));
        CHECK(r.velocity.y == doctest::Approx(2.2f * c.unit.y));
        CHECK(r.velocity.z == doctest::Approx(2.2f * c.unit.z));
    }
    // Zero or not finite: the last good look, inputOk false.
    Swim s;
    SwimEnv e = diveToUnder(s, deep());
    e.position.z = -5.f;
    s.step(go({0.f, 1.f, 0.f}), e, kDt);
    for (const SwimVec bad : {SwimVec{}, SwimVec{kInf, 0.f, 0.f}, SwimVec{kNaN, 1.f, 0.f}}) {
        const SwimStep& r = s.step(go(bad), e, kDt);
        CHECK_FALSE(r.inputOk);
        CHECK(r.velocity.y == doctest::Approx(2.2f));
        CHECK(len(r.velocity) == doctest::Approx(2.2f));
    }
}

TEST_CASE("swim: crouch and jump are edge-triggered; both down at the surface: the jump wins") {
    { // A held refused crouch: diveRefused once per press.
        Swim s;
        SwimEnv e = deep();
        e.deepAllowed = false;
        CHECK(s.step(crouch(), e, kDt).diveRefused);
        for (int i = 0; i < 5; ++i) CHECK_FALSE(s.step(crouch(), e, kDt).diveRefused);
        s.step(SwimInput{}, e, kDt); // released
        CHECK(s.step(crouch(), e, kDt).diveRefused);
    }
    { // A crouch held from the dive through the surfacing does not dive again.
        Swim s;
        SwimEnv e = deep();
        REQUIRE(s.step(crouch(), e, kDt).dived);
        e.position = s.last().position;
        for (int i = 0; i < 10; ++i) e.position = s.step(crouch(), e, kDt).position;
        REQUIRE(s.state() == SwimState::Under);
        SwimInput up = crouch();
        up.jump = true;
        e.position = s.step(up, e, kDt).position;
        REQUIRE(s.state() == SwimState::Surfacing);
        up.jump = false;
        for (int i = 0; i < 30 && s.state() != SwimState::Surface; ++i) e.position = s.step(up, e, kDt).position;
        REQUIRE(s.state() == SwimState::Surface);
        for (int i = 0; i < 5; ++i) {
            const SwimStep& r = s.step(crouch(), e, kDt);
            CHECK_FALSE(r.dived);
            CHECK(r.state == SwimState::Surface);
        }
        s.step(SwimInput{}, e, kDt);
        CHECK(s.step(crouch(), e, kDt).dived); // a new press dives
    }
    { // Both pressed on the same step at the surface: no dive, no refusal.
        Swim s;
        SwimEnv e = deep();
        SwimInput both = crouch();
        both.jump = true;
        const SwimStep& r = s.step(both, e, kDt);
        CHECK_FALSE(r.dived);
        CHECK_FALSE(r.diveRefused);
        CHECK(r.state == SwimState::Surface);
        // A jump held, then a crouch pressed: the jump still down, so it still wins.
        s.step(SwimInput{}, e, kDt);
        s.step(jump(), e, kDt);
        CHECK_FALSE(s.step(both, e, kDt).dived);
    }
    { // A jump press rises once; held, it changes nothing more; held at the surface it beats a crouch press.
        Swim s;
        SwimEnv e = deep();
        REQUIRE(s.step(crouch(), e, kDt).dived);
        e.position = s.last().position;
        for (int i = 0; i < 10; ++i) e.position = s.step(crouch(), e, kDt).position; // held: to Under at 1 m
        REQUIRE(s.state() == SwimState::Under);
        SwimInput c = crouch();
        c.jump = true; // pressed now
        e.position = s.step(c, e, kDt).position;
        CHECK(s.state() == SwimState::Surfacing);
        e.position = s.step(c, e, kDt).position; // held: no new change
        CHECK(s.state() == SwimState::Surfacing);
        for (int i = 0; i < 30 && s.state() != SwimState::Surface; ++i) e.position = s.step(c, e, kDt).position;
        REQUIRE(s.state() == SwimState::Surface);
        SwimInput j = jump(); // the jump still held, the crouch released and pressed again: the jump wins
        s.step(j, e, kDt);
        CHECK_FALSE(s.step(c, e, kDt).dived);
        s.step(SwimInput{}, e, kDt);
        REQUIRE(s.step(crouch(), e, kDt).dived); // a clean press dives
        e.position = s.last().position;
        e.position = s.step(crouch(), e, kDt).position;
        REQUIRE(s.state() == SwimState::Diving);
        e.position = s.step(c, e, kDt).position; // a jump pressed while diving: surfacing
        CHECK(s.state() == SwimState::Surfacing);
    }
}

TEST_CASE("swim: once under water deepAllowed false or a shallow column never ejects; only a new dive needs them") {
    Swim s;
    SwimEnv e = diveToUnder(s, deep());
    e.position.z = -1.5f;
    e.deepAllowed = false;
    for (int i = 0; i < 10; ++i) {
        const SwimStep& r = s.step(go({1.f, 0.f, 0.f}), e, kDt);
        CHECK(r.state == SwimState::Under);
        CHECK(r.envOk);
    }
    SwimEnv shallow = e; // the column shrinks under kSwimDeepMin
    shallow.bedZ = -2.f;
    shallow.column = 2.f;
    shallow.deepAllowed = true;
    for (int i = 0; i < 10; ++i) CHECK(s.step(go({1.f, 0.f, 0.f}), shallow, kDt).state == SwimState::Under);
    // Up to the surface there; a new dive is refused.
    e = shallow;
    for (int i = 0; i < 20 && s.state() != SwimState::Surface; ++i) {
        SwimInput in;
        in.jump = i == 0;
        e.position = s.step(in, e, kDt).position;
    }
    REQUIRE(s.state() == SwimState::Surface);
    const SwimStep& r = s.step(crouch(), e, kDt);
    CHECK(r.diveRefused);
    CHECK_FALSE(r.dived);
}

TEST_CASE("swim: one huge step from just under the surface going up, or just above the bed going down, ends clamped") {
    for (const float dt : {10.f, 1e6f}) {
        CAPTURE(dt);
        {
            Swim s;
            SwimEnv e = diveToUnder(s, deep());
            e.position.z = -0.05f;
            const SwimStep& r = s.step(go({0.f, 0.f, 1.f}, 1.f, 0.f, true), e, dt);
            CHECK(r.dt == kSwimMaxStep);
            CHECK(r.position.z == 0.f);
            CHECK(r.state == SwimState::Surface); // the hand-over
            CHECK(r.surfaced);
        }
        {
            Swim s;
            SwimEnv e = diveToUnder(s, deep());
            e.position.z = -9.65f;
            const float b = s.breath();
            const SwimStep& r = s.step(go({0.f, 0.f, -1.f}, 1.f, 0.f, true), e, dt);
            CHECK(r.position.z == doctest::Approx(-9.7f)); // 0.3 m off the bed, never through
            CHECK(r.position.z >= -9.7f - 1e-5f);
            CHECK(r.state == SwimState::Under);
            CHECK(r.breath == doctest::Approx(b - 2.f * kSwimMaxStep)); // the rest of the step dropped
        }
        { // A dive on one huge step stops at its target.
            Swim s;
            const SwimStep& r = s.step(crouch(), deep(), dt);
            CHECK(r.position.z == doctest::Approx(-0.55f)); // 0.25 s at 2.2 m/s
            CHECK(r.position.z >= -1.f);
        }
    }
}

TEST_CASE("swim: the dive stops at its target, no overshoot carried on") {
    for (const float dt : {0.1f, 0.25f, 1.f / 30.f}) {
        CAPTURE(dt);
        Swim s;
        SwimEnv e = deep();
        for (int i = 0; i < 100 && s.state() != SwimState::Under; ++i) {
            SwimInput in;
            in.crouch = i == 0;
            e.position = s.step(in, e, dt).position;
        }
        REQUIRE(s.state() == SwimState::Under);
        CHECK(e.position.z == -1.f);
    }
}

TEST_CASE("swim: the same 10 s script at 30, 60 and 144 steps a second ends within 0.05 m and 0.05 s of breath") {
    struct End {
        SwimVec pos;
        float breath;
        SwimState state;
    };
    const auto run = [](int rate) {
        Swim s;
        SwimEnv e = deep();
        const float dt = 1.f / float(rate);
        for (int k = 0; k < 10 * rate; ++k) {
            const float t = float(k) * dt + 0.5f * dt; // the middle of the step
            SwimInput in;
            if (t < 0.5f) in.crouch = true;                                 // dive (0.45 s to 1 m)
            else if (t < 1.f) in = SwimInput{};                             // settle at the dive's end
            else if (t < 4.f) in = go({1.f, 0.4f, -0.5f});                  // along a slanted look, down
            else if (t < 6.f) in = go({0.3f, 1.f, -0.2f}, 1.f, 0.f, true); // sprint
            else if (t < 8.f) in = SwimInput{};                             // idle: drift up
            else in = go({-1.f, 0.f, 0.1f}, 0.7f, 0.5f);                    // swim on, both axes
            e.position = s.step(in, e, dt).position;
        }
        return End{s.last().position, s.breath(), s.state()};
    };
    const End a = run(30), b = run(60), c = run(144);
    for (const End* x : {&a, &b}) {
        CHECK(x->state == SwimState::Under);
        CHECK(std::fabs(x->pos.x - c.pos.x) <= 0.05f);
        CHECK(std::fabs(x->pos.y - c.pos.y) <= 0.05f);
        CHECK(std::fabs(x->pos.z - c.pos.z) <= 0.05f);
        CHECK(std::fabs(x->breath - c.breath) <= 0.05f);
    }
    CHECK(c.state == SwimState::Under);
    CHECK(c.pos.z < -1.f); // the script stayed under water
    MESSAGE("30 Hz " << a.pos.x << "," << a.pos.y << "," << a.pos.z << " breath " << a.breath << "; 144 Hz " << c.pos.x
                     << "," << c.pos.y << "," << c.pos.z << " breath " << c.breath);
}

TEST_CASE("swim: reset brings back the surface, a full breath and full vitality") {
    Swim s;
    SwimEnv e = diveToUnder(s, deep());
    s.setVitality(0.5f);
    s.step(SwimInput{}, e, 0.25f);
    s.reset();
    CHECK(s.state() == SwimState::Surface);
    CHECK(s.breath() == 45.f);
    CHECK(s.vitality() == 1.f);
    SwimTuning t;
    t.breathSeconds = 30.f;
    t.warningSeconds = 5.f;
    s.reset(t);
    CHECK(s.breath() == 30.f);
    CHECK(s.tuningOk());
}

TEST_CASE("swim: a step allocates nothing") {
    Swim s;
    SwimEnv e = deep();
    REQUIRE(ftcore_test::allocHookInstalled());
    const std::uint64_t before = ftcore_test::allocStats().count;
    for (int i = 0; i < 400; ++i) {
        SwimInput in = i == 0 ? crouch() : go({1.f, 0.3f, -0.1f}, 1.f, 0.2f, i % 3 == 0);
        in.jump = i == 300;
        e.edgeDistance = 20.f - float(i) * 0.05f;
        e.edgeInward = {-1.f, 0.f, 0.f};
        e.position = s.step(in, e, kDt).position;
    }
    CHECK(ftcore_test::allocStats().count == before);
}
