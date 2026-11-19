// The scripted walk's held-up jump (HeldUpJump.h): when it presses and releases, how a try is judged (cleared by
// progress or the water, not cleared when nothing moved), the busy wait and its cap, the counts, the point reached
// mid-try, the reset; and the jump rise from the movement's values.
#include "ftcore/HeldUpJump.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

using ftcore::HeldUpJump;
using ftcore::HeldUpJumpVerdict;

namespace {

constexpr float kDt = 1.f / 60.f;

// Ticks held up (walking, speed 0) at `distance` until the press; returns the frames it took.
int tickToPress(HeldUpJump& j, float distance)
{
    for (int i = 1; i < 600; ++i) {
        j.tick(kDt, true, 0.f, distance, false, false);
        if (j.pressed()) {
            return i;
        }
    }
    return -1;
}

// Ticks the hold out; returns the frames from the press to the release.
int tickToRelease(HeldUpJump& j, float distance, bool busy)
{
    for (int i = 1; i < 600; ++i) {
        j.tick(kDt, true, 0.f, distance, busy, false);
        if (j.released()) {
            return i;
        }
    }
    return -1;
}

} // namespace

TEST_CASE("HeldUpJump presses after stuckTime held up, holds holdTime, then releases")
{
    HeldUpJump j;
    const int toPress = tickToPress(j, 300.f);
    CHECK((toPress == 60 || toPress == 61)); // over 1 s at 60 fps (float sums)
    CHECK(j.tries() == 1);
    CHECK(j.holding());
    CHECK(j.trying());
    CHECK_FALSE(j.released());
    const int toRelease = tickToRelease(j, 300.f, false);
    CHECK((toRelease == 15 || toRelease == 16)); // 0.25 s
    CHECK_FALSE(j.holding());
    CHECK(j.trying());
}

TEST_CASE("HeldUpJump does not count moving or not walking as held up")
{
    HeldUpJump j;
    for (int i = 0; i < 300; ++i) {
        j.tick(kDt, true, 50.f, 300.f, false, false);
        CHECK_FALSE(j.pressed());
    }
    for (int i = 0; i < 300; ++i) {
        j.tick(kDt, false, 0.f, 300.f, false, false);
        CHECK_FALSE(j.pressed());
    }
    // A moment of speed starts the count over.
    for (int i = 0; i < 50; ++i) {
        j.tick(kDt, true, 0.f, 300.f, false, false);
    }
    j.tick(kDt, true, 40.f, 300.f, false, false);
    for (int i = 0; i < 50; ++i) {
        j.tick(kDt, true, 0.f, 300.f, false, false);
        CHECK_FALSE(j.pressed());
    }
    CHECK(j.tries() == 0);
}

TEST_CASE("HeldUpJump: a try that moves nothing is not cleared after settleTime; the count goes up")
{
    HeldUpJump j;
    REQUIRE(tickToPress(j, 300.f) > 0);
    REQUIRE(tickToRelease(j, 300.f, false) > 0);
    int frames = 0;
    for (; frames < 600; ++frames) {
        j.tick(kDt, true, 0.f, 300.f, false, false);
        if (j.verdict() != HeldUpJumpVerdict::None) {
            break;
        }
    }
    CHECK(j.verdict() == HeldUpJumpVerdict::NotCleared);
    CHECK((frames + 1 == 18 || frames + 1 == 19)); // 0.3 s after the release
    CHECK(j.failed() == 1);
    CHECK_FALSE(j.trying());
    // Three such tries, three fails.
    for (int t = 0; t < 2; ++t) {
        REQUIRE(tickToPress(j, 300.f) > 0);
        REQUIRE(tickToRelease(j, 300.f, false) > 0);
        for (int i = 0; i < 600 && j.verdict() == HeldUpJumpVerdict::None; ++i) {
            j.tick(kDt, true, 0.f, 300.f, false, false);
        }
    }
    CHECK(j.tries() == 3);
    CHECK(j.failed() == 3);
}

TEST_CASE("HeldUpJump: past it by minProgress and down is cleared at once")
{
    HeldUpJump j;
    REQUIRE(tickToPress(j, 300.f) > 0);
    REQUIRE(tickToRelease(j, 300.f, true) > 0);
    // In the air over it: nearer, but busy, so not judged yet.
    j.tick(kDt, true, 200.f, 250.f, true, false);
    CHECK(j.verdict() == HeldUpJumpVerdict::None);
    // Down beyond it.
    j.tick(kDt, true, 200.f, 240.f, false, false);
    CHECK(j.verdict() == HeldUpJumpVerdict::Cleared);
    CHECK(j.failed() == 0);
    CHECK(j.progress(240.f) == doctest::Approx(60.f));
}

TEST_CASE("HeldUpJump: falling into the water clears the try even while busy")
{
    HeldUpJump j;
    REQUIRE(tickToPress(j, 300.f) > 0);
    REQUIRE(tickToRelease(j, 300.f, true) > 0);
    j.tick(kDt, true, 100.f, 290.f, true, true);
    CHECK(j.verdict() == HeldUpJumpVerdict::Cleared);
    CHECK(j.failed() == 0);
}

TEST_CASE("HeldUpJump: a busy body is judged no later than judgeMax after the press")
{
    HeldUpJump j;
    REQUIRE(tickToPress(j, 300.f) > 0);
    int frames = 0;
    for (; frames < 600; ++frames) {
        j.tick(kDt, true, 0.f, 300.f, true, false);
        if (j.verdict() != HeldUpJumpVerdict::None) {
            break;
        }
    }
    CHECK(j.verdict() == HeldUpJumpVerdict::NotCleared);
    CHECK((frames + 1) * kDt == doctest::Approx(3.f).epsilon(0.02));
}

TEST_CASE("HeldUpJump: the point reached mid-hold releases and clears the try")
{
    HeldUpJump j;
    REQUIRE(tickToPress(j, 300.f) > 0);
    j.pointReached();
    CHECK(j.released());
    CHECK(j.verdict() == HeldUpJumpVerdict::Cleared);
    CHECK_FALSE(j.trying());
    CHECK(j.failed() == 0);
    // Walking, it only starts the held-up count over.
    for (int i = 0; i < 50; ++i) {
        j.tick(kDt, true, 0.f, 300.f, false, false);
    }
    j.pointReached();
    CHECK_FALSE(j.released());
    CHECK(j.verdict() == HeldUpJumpVerdict::None);
    for (int i = 0; i < 50; ++i) {
        j.tick(kDt, true, 0.f, 300.f, false, false);
        CHECK_FALSE(j.pressed());
    }
}

TEST_CASE("HeldUpJump: dt <= 0 changes nothing; reset forgets the tries and keeps the rules")
{
    HeldUpJump j;
    ftcore::HeldUpJumpRules r;
    r.stuckTime = 0.5f;
    j.setRules(r);
    const int toPress = tickToPress(j, 300.f);
    REQUIRE((toPress == 30 || toPress == 31));
    j.tick(0.f, true, 0.f, 300.f, false, false);
    CHECK(j.holding());
    CHECK_FALSE(j.pressed());
    j.reset();
    CHECK(j.tries() == 0);
    CHECK(j.failed() == 0);
    CHECK_FALSE(j.trying());
    CHECK(j.rules().stuckTime == doctest::Approx(0.5f));
}

TEST_CASE("HeldUpJump ticks without heap use")
{
    HeldUpJump j;
    REQUIRE(ftcore_test::allocHookInstalled());
    const std::uint64_t before = ftcore_test::allocStats().count;
    for (int i = 0; i < 2000; ++i) {
        j.tick(kDt, true, 0.f, 300.f, (i / 40) % 2 == 0, false);
    }
    CHECK(j.tries() > 0);
    CHECK(ftcore_test::allocStats().count == before);
}

TEST_CASE("jumpRise from the movement's values, and jumpClears")
{
    // The lead's movement: JumpZVelocity 428 cm/s, gravity 980 cm/s^2, no jump hold.
    CHECK(ftcore::jumpRise(428.f, 980.f) == doctest::Approx(93.46f).epsilon(0.001));
    CHECK(ftcore::jumpRise(428.f, 980.f, 0.1f) == doctest::Approx(136.26f).epsilon(0.001));
    CHECK(ftcore::jumpRise(428.f, 980.f, -1.f) == doctest::Approx(93.46f).epsilon(0.001));
    CHECK(ftcore::jumpRise(0.f, 980.f) == 0.f);
    CHECK(ftcore::jumpRise(428.f, 0.f) == 0.f);
    CHECK(ftcore::jumpClears(93.46f, 90.f));
    CHECK_FALSE(ftcore::jumpClears(93.46f, 110.f));
    CHECK_FALSE(ftcore::jumpClears(93.46f, 90.f, 10.f));
}
