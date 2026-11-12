#include "ftcore/FootHold.h"
#include <doctest/doctest.h>

#include <cmath>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 60.f;

struct Feet {
    FootPoint ball[2] = {{0.f, -10.f, 0.f}, {0.f, 10.f, 0.f}};
    bool lifted[2] = {false, false};
    FootPoint by[2];
    void move(float dx) {
        ball[0].x += dx;
        ball[1].x += dx;
    }
};

} // namespace

TEST_CASE("foot hold: a locked foot stays in the world while the body moves, then steps when too far") {
    FootHold h;
    Feet f;
    h.update(f.ball, f.lifted, true, kDt, 0.f, f.by);
    CHECK(h.foot(0).held);
    CHECK(h.foot(1).held);
    CHECK(h.legsNeeded(false));
    f.move(10.f);
    h.update(f.ball, f.lifted, true, kDt, 10.f, f.by);
    CHECK(f.by[0].x == doctest::Approx(-10.f)); // kept back where it stood
    CHECK(f.by[0].z == 0.f);
    // Past the reach: one foot steps (the farther; equal here, so the first), lifted on an arc, landing on the clip's.
    f.move(20.f);
    h.update(f.ball, f.lifted, true, kDt, 20.f, f.by);
    CHECK(h.foot(0).stepping);
    CHECK_FALSE(h.foot(1).stepping); // one at a time
    CHECK(f.by[0].z > 0.f);
    for (int i = 0; i < 20; ++i) h.update(f.ball, f.lifted, true, kDt, 0.f, f.by);
    CHECK_FALSE(h.foot(0).stepping);
    CHECK(h.foot(0).held); // held again where it came down
    CHECK(f.by[0].x == doctest::Approx(0.f));
}

TEST_CASE("foot hold: let go, a held foot steps back; a lifted one follows the clip") {
    FootHold h;
    Feet f;
    h.update(f.ball, f.lifted, true, kDt, 0.f, f.by);
    f.move(10.f);
    h.update(f.ball, f.lifted, true, kDt, 10.f, f.by);
    h.update(f.ball, f.lifted, false, kDt, 0.f, f.by); // released: 10 cm off, so both step back, one at a time
    CHECK((h.foot(0).stepping || h.foot(1).stepping));
    for (int i = 0; i < 60 && h.feetActive(); ++i) h.update(f.ball, f.lifted, false, kDt, 0.f, f.by);
    CHECK_FALSE(h.feetActive());
    CHECK(f.by[0].x == 0.f);
    CHECK(f.by[1].x == 0.f);
    // Lifted by the clip: not held, whatever the lock.
    f.lifted[1] = true;
    h.update(f.ball, f.lifted, true, kDt, 0.f, f.by);
    CHECK(h.foot(0).held);
    CHECK_FALSE(h.foot(1).held);
}

TEST_CASE("foot hold: the pelvis sinks fast, rises slower, never below the floor or past its most") {
    FootHold h;
    const FootHoldTuning& t = h.tuning();
    float down = 0.f;
    for (int i = 0; i < 3; ++i) down = h.easePelvis(5.f, kDt);
    CHECK(down > 0.f);
    CHECK(down < 5.f); // eased, not at once
    CHECK(down == doctest::Approx(5.f * (1.f - std::pow(1.f - t.pelvisDropIn * kDt, 3.f)))); // the in rate
    for (int i = 0; i < 120; ++i) h.easePelvis(5.f, kDt);
    CHECK(h.pelvisDrop() == doctest::Approx(5.f));
    // Out at the slower rate: after the same frames less of the sink is gone than came in.
    float out = 0.f;
    for (int i = 0; i < 3; ++i) out = h.easePelvis(0.f, kDt);
    CHECK(5.f - out < down);
    CHECK(out == doctest::Approx(5.f * std::pow(1.f - t.pelvisDropOut * kDt, 3.f))); // the out rate
    CHECK(h.legsNeeded(false)); // still sunk with no foot held: the legs stay solved, no pop
    for (int i = 0; i < 600 && h.pelvisDrop() > 0.f; ++i) h.easePelvis(0.f, kDt);
    CHECK(h.pelvisDrop() == 0.f); // ends at exactly the floor
    CHECK_FALSE(h.legsNeeded(false));
    // Never below 0 nor past the most; nonsense asks nothing.
    CHECK(h.easePelvis(-20.f, kDt) == 0.f);
    for (int i = 0; i < 600; ++i) h.easePelvis(100.f, kDt);
    CHECK(h.pelvisDrop() == doctest::Approx(t.maxPelvisDrop));
    CHECK(h.easePelvis(100.f, 10.f) == doctest::Approx(t.maxPelvisDrop)); // a long frame lands, never overshoots
    const float before = h.pelvisDrop();
    const float nan = h.easePelvis(NAN, kDt); // a NaN ask is no sink: eased toward the floor
    CHECK(nan < before);
    CHECK(nan > 0.f);
}

TEST_CASE("foot hold: a held foot near its clip place is let go at once; a step finishes whatever the clip lifts") {
    FootHold h;
    Feet f;
    h.update(f.ball, f.lifted, true, kDt, 0.f, f.by);
    f.move(1.f); // within letGoWithin
    h.update(f.ball, f.lifted, false, kDt, 1.f, f.by);
    CHECK_FALSE(h.feetActive());
    CHECK(f.by[0].x == 0.f);
    // A stepping foot keeps stepping though the clip lifts it, and lands not held while lifted.
    h.update(f.ball, f.lifted, true, kDt, 0.f, f.by);
    f.move(30.f);
    h.update(f.ball, f.lifted, true, kDt, 30.f, f.by);
    REQUIRE(h.foot(0).stepping);
    f.lifted[0] = true;
    h.update(f.ball, f.lifted, true, kDt, 0.f, f.by);
    CHECK(h.foot(0).stepping);
    for (int i = 0; i < 20; ++i) h.update(f.ball, f.lifted, true, kDt, 0.f, f.by);
    CHECK_FALSE(h.foot(0).stepping);
    CHECK_FALSE(h.foot(0).held);
}

TEST_CASE("foot hold: a move of exactly teleportJump keeps the holds, just over drops them") {
    FootHold h;
    Feet f;
    const float jump = h.tuning().teleportJump;
    h.update(f.ball, f.lifted, true, kDt, 0.f, f.by);
    h.update(f.ball, f.lifted, true, kDt, jump, f.by);
    CHECK(h.foot(0).held);
    f.move(10.f);
    h.update(f.ball, f.lifted, true, kDt, jump, f.by);
    CHECK(f.by[0].x == doctest::Approx(-10.f)); // still where it stood
    h.update(f.ball, f.lifted, true, kDt, jump + 1.f, f.by);
    CHECK(f.by[0].x == 0.f); // held anew where the clip has it
}

TEST_CASE("foot hold: a reset or a jump in one frame drops every hold and the sink") {
    FootHold h;
    Feet f;
    h.update(f.ball, f.lifted, true, kDt, 0.f, f.by);
    for (int i = 0; i < 30; ++i) h.easePelvis(6.f, kDt);
    REQUIRE(h.feetActive());
    REQUIRE(h.pelvisDrop() > 0.f);
    h.reset();
    CHECK_FALSE(h.feetActive());
    CHECK(h.pelvisDrop() == 0.f);
    // A teleport: the old anchors would stretch the legs across the arena; held anew where the body now stands.
    h.update(f.ball, f.lifted, true, kDt, 0.f, f.by);
    f.move(2000.f);
    h.update(f.ball, f.lifted, true, kDt, 2000.f, f.by);
    CHECK_FALSE(h.foot(0).stepping);
    CHECK(h.foot(0).held);
    CHECK(f.by[0].x == 0.f);
    CHECK(f.by[1].x == 0.f);
}
