#include "ftcore/LedgeHang.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

constexpr float kStep = 1.f / 60.f;

void run(const LedgeHang& l, LedgeState& s, const LedgeInput& in, float seconds) {
    for (float t = 0.f; t < seconds - 1e-4f; t += kStep) l.step(s, in, kStep);
}

LedgeInput stickAt(float off, float push = 1.f) {
    LedgeInput in;
    in.stick = push;
    in.stickOff = off;
    return in;
}

} // namespace

TEST_CASE("At the catch a body hangs, unless it holds jump and pushes into the face (or hanging is off)") {
    LedgeHang l;
    LedgeState s;
    CHECK(l.canCatch(s));
    CHECK(l.reach(s, LedgeInput{}) == LedgePhase::Hanging);
    CHECK_FALSE(s.through);
    CHECK_FALSE(l.canCatch(s)); // on one already

    LedgeInput held = stickAt(10.f);
    held.jumpHeld = true;
    LedgeState t;
    CHECK(l.reach(t, held) == LedgePhase::Climbing);
    CHECK(t.through);
    // Jump held with no direction, or a direction without jump: it hangs.
    LedgeInput noStick;
    noStick.jumpHeld = true;
    CHECK(l.reach(t, noStick) == LedgePhase::Hanging);
    CHECK(l.reach(t, stickAt(0.f)) == LedgePhase::Hanging);

    LedgeTuning off;
    off.hang = 0.f;
    l.setTuning(off);
    CHECK(l.reach(t, LedgeInput{}) == LedgePhase::Climbing);
    CHECK(t.through);
}

TEST_CASE("A hang climbs on a jump press or the stick into the face, once it has hung minHang") {
    LedgeHang l;
    LedgeState s;
    l.reach(s, LedgeInput{});
    LedgeInput jump;
    jump.jumpPressed = true;
    l.step(s, jump, kStep); // too soon: ignored
    CHECK(s.phase == LedgePhase::Hanging);
    run(l, s, LedgeInput{}, 1.f); // it hangs as long as nothing is asked
    CHECK(s.phase == LedgePhase::Hanging);
    l.step(s, jump, kStep);
    CHECK(s.phase == LedgePhase::Climbing);
    run(l, s, jump, 0.5f);
    CHECK(s.phase == LedgePhase::Climbing); // it climbs on until the host says the clip is done
    l.finish(s);
    CHECK(s.phase == LedgePhase::None);
    CHECK(l.canCatch(s)); // no regrab after a climb

    // The stick: within intoFace of the way in, and pushed far enough.
    LedgeState t;
    l.reach(t, LedgeInput{});
    run(l, t, LedgeInput{}, 0.25f);
    l.step(t, stickAt(60.f), kStep); // too far off the way in
    CHECK(t.phase == LedgePhase::Hanging);
    l.step(t, stickAt(-20.f, 0.3f), kStep); // not pushed far enough
    CHECK(t.phase == LedgePhase::Hanging);
    l.step(t, stickAt(-45.f), kStep);
    CHECK(t.phase == LedgePhase::Climbing);
}

TEST_CASE("A hang drops on a crouch press or the stick pulled away, and no ledge is caught for regrab after") {
    LedgeHang l;
    LedgeState s;
    l.reach(s, LedgeInput{});
    run(l, s, LedgeInput{}, 0.25f);
    l.step(s, stickAt(90.f), kStep); // sideways: neither in nor away
    CHECK(s.phase == LedgePhase::Hanging);
    l.step(s, stickAt(150.f), kStep);
    CHECK(s.phase == LedgePhase::Dropping); // seen once by the host...
    CHECK_FALSE(l.canCatch(s));
    l.step(s, LedgeInput{}, kStep);
    CHECK(s.phase == LedgePhase::None); // ...and handed back the next step
    CHECK_FALSE(l.canCatch(s));
    run(l, s, LedgeInput{}, 0.45f);
    CHECK_FALSE(l.canCatch(s));
    run(l, s, LedgeInput{}, 0.06f);
    CHECK(l.canCatch(s));

    // Crouch drops; asked with a climb in the same step, the drop wins.
    LedgeState t;
    l.reach(t, LedgeInput{});
    run(l, t, LedgeInput{}, 0.25f);
    LedgeInput both = stickAt(0.f);
    both.jumpPressed = true;
    both.dropPressed = true;
    l.step(t, both, kStep);
    CHECK(t.phase == LedgePhase::Dropping);
    CHECK(t.regrab == doctest::Approx(0.5f));
}

TEST_CASE("Hanging doesn't allocate") {
    LedgeHang l;
    LedgeState s;
    const auto before = ftcore_test::allocStats().count;
    for (int i = 0; i < 3000; ++i) {
        LedgeInput in = stickAt(static_cast<float>(i % 360) - 180.f, (i % 7) / 6.f);
        in.jumpPressed = i % 11 == 0;
        in.dropPressed = i % 13 == 0;
        in.jumpHeld = i % 2 == 0;
        if (l.canCatch(s)) l.reach(s, in);
        l.step(s, in, kStep);
        if (s.phase == LedgePhase::Climbing && s.time > 0.5f) l.finish(s);
    }
    CHECK(ftcore_test::allocStats().count == before);
}
