#include <doctest/doctest.h>
#include "ftcore/DayShade.h"
#include "ftcore/NightLook.h"
#include "ftcore/WorldClock.h"
#include "AllocCounter.h"

#include <cmath>
#include <limits>

using namespace ftcore;

TEST_CASE("dayShadeAmount: full by day, none at night or full dusk") {
    CHECK(dayShadeAmount(0.f, 0.f) == doctest::Approx(1.f));
    CHECK(dayShadeAmount(1.f, 0.f) == doctest::Approx(0.f));
    CHECK(dayShadeAmount(0.f, 1.f) == doctest::Approx(0.f));
    CHECK(dayShadeAmount(0.5f, 0.f) == doctest::Approx(0.5f));
    CHECK(dayShadeAmount(0.f, 0.1f) == doctest::Approx(0.25f));
    CHECK(dayShadeAmount(0.f, kDayShadeDuskCut) == 0.f);
    CHECK(dayShadeAmount(0.f, 0.26f) == 0.f);
    CHECK(dayShadeAmount(-1.f, 2.f) == doctest::Approx(0.f));
    CHECK(dayShadeAmount(std::numeric_limits<float>::quiet_NaN(), 0.f) == 0.f);
}

TEST_CASE("dayShadeFrame: the night keeps the world's own values exactly") {
    const DayShadeBase base{1.f, 1.f, 0.62f};
    const DayShadeFrame f = dayShadeFrame(DayShadeLook{}, base, 0.f);
    CHECK(f.skyScale == 1.f);
    CHECK(f.shadowContrast == 1.f);
    CHECK(f.diffuseBoost == 1.f);
    CHECK(f.filmToe == 0.62f);
}

TEST_CASE("dayShadeFrame: full day lifts the shade") {
    const DayShadeBase base{1.f, 1.f, 0.62f};
    const DayShadeFrame f = dayShadeFrame(DayShadeLook{}, base, 1.f);
    CHECK(f.skyScale > 1.f);
    CHECK(f.shadowContrast < 1.f);
    CHECK(f.diffuseBoost > 1.f);
    CHECK(f.filmToe < 0.62f);
    const DayShadeFrame half = dayShadeFrame(DayShadeLook{}, base, 0.5f);
    CHECK(half.skyScale == doctest::Approx((1.f + f.skyScale) * 0.5f));
}

TEST_CASE("dayShadeFrame: clamps to the engine's ranges") {
    const DayShadeFrame f = dayShadeFrame(DayShadeLook{-1.f, 2.f, 9.f, -1.f}, DayShadeBase{}, 3.f);
    CHECK(f.amount == 1.f);
    CHECK(f.skyScale == 0.f);
    CHECK(f.shadowContrast == 1.f);
    CHECK(f.diffuseBoost == 4.f);
    CHECK(f.filmToe == 0.f);
}

TEST_CASE("day shade: full at 08, 12 and 16 h, none at midnight, and allocation-free") {
    const WorldClockSpec spec;
    const DuskLook dusk;
    const auto amountAt = [&](double h) {
        const float e = sunElevationDeg(spec, h);
        return dayShadeAmount(nightAmount(spec, h), duskAmount(dusk, e, h >= 12.0));
    };
    const ftcore_test::AllocStats before = ftcore_test::allocStats();
    const float a8 = amountAt(8.0), a12 = amountAt(12.0), a16 = amountAt(16.0), a0 = amountAt(0.0);
    const DayShadeFrame f = dayShadeFrame(DayShadeLook{}, DayShadeBase{}, a12);
    const ftcore_test::AllocStats after = ftcore_test::allocStats();
    CHECK(a8 == doctest::Approx(1.f));
    CHECK(a12 == doctest::Approx(1.f));
    CHECK(a16 == doctest::Approx(1.f));
    CHECK(a0 == doctest::Approx(0.f));
    CHECK(f.amount == doctest::Approx(1.f));
    CHECK(after.count == before.count);
}
