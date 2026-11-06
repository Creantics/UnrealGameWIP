#include "ftcore/KneelGetUp.h"

#include <doctest/doctest.h>

#include <limits>

using namespace ftcore;

namespace {

constexpr double kIn = 2.5; // the go-down
constexpr double kUp = 1.4; // the get-up

// One slot through a spell of `length` seconds (+infinity: a hold) from t = 0, the clip last drawn fed back as the
// runner does.
struct Slot {
    KneelTrack k;
    double since = 0.0;
    double upAt = 0.0;
    double length = 0.0;
    KneelClip last = KneelClip::Hold;
    bool drawn = false;

    explicit Slot(double spell, bool track = false) : length(spell) {
        k.track = track;
        kneelStart(k, since, upAt, 0.0);
    }
    KneelFrame at(double now, bool hasIn = true) {
        KneelClips c;
        c.inLength = hasIn ? kIn : 0.0;
        c.upLength = kUp;
        c.hasIn = hasIn;
        c.hasUp = true;
        c.goingDown = drawn && last == KneelClip::GoDown;
        const KneelFrame f = kneelShow(k, since, upAt, now, length - now, c);
        last = f.clip;
        drawn = true;
        return f;
    }
};

} // namespace

TEST_CASE("kneel: a go-down cut by the spell's end gets up from the same pose and plays out in real time") {
    // A 1.5 s down with a 2.5 s go-down and a 1.4 s get-up (an untracked skin): the get-up is owed from 0.1 s in.
    Slot s(1.5);
    KneelFrame f = s.at(0.0);
    CHECK(f.clip == KneelClip::GoDown);
    CHECK(f.time == doctest::Approx(0.0));
    f = s.at(0.2);
    REQUIRE(f.clip == KneelClip::GetUp);
    CHECK(f.cut);
    CHECK(f.cutAt == doctest::Approx(0.2 / kIn));
    CHECK(f.time == doctest::Approx((1.0 - 0.2 / kIn) * kUp)); // the same pose: the go-down 8 % in is the get-up 92 % in
    double prev = f.time;
    // From there it runs on in real time, never jumping back to upLength - left (0.15 s at 0.25 s in).
    f = s.at(0.25);
    REQUIRE(f.clip == KneelClip::GetUp);
    CHECK_FALSE(f.cut);
    CHECK(f.time == doctest::Approx(prev + 0.05));
    CHECK(s.upAt == doctest::Approx(f.time));
    // It ends 0.112 s after the cut and stands out the 1.19 s left.
    f = s.at(0.32);
    CHECK(f.clip == KneelClip::Stand);
    CHECK(f.standsOut);
    CHECK(s.upAt == doctest::Approx(kUp));
    f = s.at(0.5);
    CHECK(f.clip == KneelClip::Stand);
    CHECK_FALSE(f.standsOut);
    // The next spell starts clean.
    kneelEnd(s.k);
    kneelStart(s.k, s.since, s.upAt, 2.0);
    CHECK_FALSE(s.k.stood);
    CHECK(s.k.upStart < kKneelNone * 0.1);
}

TEST_CASE("kneel: the cut maps the go-down's progress p to (1 - p) of the get-up") {
    // A 2.65 s down: the get-up is owed at 1.25 s in, half way through the go-down.
    Slot s(1.25 + kUp);
    CHECK(s.at(1.0).clip == KneelClip::GoDown);
    const KneelFrame f = s.at(1.26);
    REQUIRE(f.cut);
    CHECK(f.cutAt == doctest::Approx(1.26 / kIn));
    CHECK(f.time == doctest::Approx((1.0 - 1.26 / kIn) * kUp));
}

TEST_CASE("kneel: an uncut get-up of an untracked skin is timed to end with the spell") {
    Slot s(5.0);
    CHECK(s.at(1.0).clip == KneelClip::GoDown);
    CHECK(s.at(2.6).clip == KneelClip::Hold);
    CHECK(s.at(3.5).clip == KneelClip::Hold);
    const KneelFrame f = s.at(4.0);
    REQUIRE(f.clip == KneelClip::GetUp);
    CHECK_FALSE(f.cut);
    CHECK(f.time == doctest::Approx(kUp - 1.0));
    CHECK(s.k.upStart < kKneelNone * 0.1); // not tracked: placed by the time left each frame
    CHECK(s.at(4.9).time == doctest::Approx(kUp - 0.1));
}

TEST_CASE("kneel: a tracked get-up never sinks back when the down runs on; it kneels again or stands it out") {
    Slot s(5.0, true);
    s.at(3.0);
    KneelFrame f = s.at(3.7);
    REQUIRE(f.clip == KneelClip::GetUp);
    CHECK(f.time == doctest::Approx(0.1));
    // A riposte makes it a 9 s down: the get-up plays on.
    s.length = 9.0;
    f = s.at(3.8);
    REQUIRE(f.clip == KneelClip::GetUp);
    CHECK(f.time == doctest::Approx(0.2));
    // Up at 5.0 with 4 s left (more than a go-down and a get-up): it kneels again from there.
    f = s.at(5.05);
    CHECK(f.kneelsAgain);
    CHECK(f.clip == KneelClip::GoDown);
    CHECK(f.time == doctest::Approx(0.0));
    CHECK(s.since == doctest::Approx(5.05));
    CHECK(s.upAt == doctest::Approx(0.0));
    // A down that runs on by less stands it out.
    Slot t(5.0, true);
    t.at(3.7);
    t.length = 7.0;
    f = t.at(5.05);
    CHECK(f.standsOut);
    CHECK(f.clip == KneelClip::Stand);
}

TEST_CASE("kneel: a hold never gets up early") {
    Slot s(std::numeric_limits<double>::infinity());
    CHECK(s.at(1.0).clip == KneelClip::GoDown);
    for (double t = 2.6; t < 30.0; t += 1.0) {
        CHECK(s.at(t).clip == KneelClip::Hold);
    }
}

TEST_CASE("kneel: without a go-down clip there is nothing to cut") {
    Slot s(1.5);
    const KneelFrame f = s.at(0.2, false);
    REQUIRE(f.clip == KneelClip::GetUp);
    CHECK_FALSE(f.cut);
    CHECK(f.time == doctest::Approx(kUp - 1.3));
}
