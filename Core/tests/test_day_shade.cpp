#include <doctest/doctest.h>
#include "ftcore/DayShade.h"
#include "ftcore/NightLook.h"
#include "ftcore/WorldClock.h"
#include "AllocCounter.h"

#include <array>
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
    DayShadeBase base;
    base.filmToe = 0.62f;
    base.highlightContrast = 0.9f;
    base.minEV100 = -3.f;
    const DayShadeFrame f = dayShadeFrame(DayShadeLook{}, base, 0.f);
    CHECK(f.skyScale == 1.f);
    CHECK(f.shadowContrast == 1.f);
    CHECK(f.diffuseBoost == 1.f);
    CHECK(f.filmToe == 0.62f);
    CHECK(f.highlightContrast == 0.9f);
    CHECK(f.minEV100 == -3.f);
    CHECK(f.fogMaxOpacity == 1.f);
    CHECK(f.fogSky == std::array<float, 3>{1.f, 1.f, 1.f});
}

TEST_CASE("dayShadeFrame: full day holds the exposure's floor, the highlights and the fog") {
    const DayShadeLook look;
    DayShadeBase base;
    const DayShadeFrame f = dayShadeFrame(look, base, 1.f);
    CHECK(f.minEV100 == doctest::Approx(look.minEV100));
    CHECK(f.highlightContrast == doctest::Approx(look.highlightContrast));
    CHECK(f.highlightContrast < 1.f);
    CHECK(f.fogMaxOpacity == doctest::Approx(look.fogMaxOpacity));
    CHECK(f.fogMaxOpacity < 1.f);
    // A dimmer haze that leans blue, never brighter than the sky gives it.
    CHECK(f.fogSky[2] > f.fogSky[0]);
    CHECK(f.fogSky[2] < 1.f);
    // A world whose own floor is higher, or whose highlights are already pulled further, keeps them.
    base.minEV100 = 0.f;
    base.highlightContrast = 0.5f;
    const DayShadeFrame dark = dayShadeFrame(look, base, 1.f);
    CHECK(dark.minEV100 == 0.f);
    CHECK(dark.highlightContrast == 0.5f);
    // A NaN floor leaves the world's own.
    DayShadeLook odd;
    odd.minEV100 = std::numeric_limits<float>::quiet_NaN();
    CHECK(dayShadeFrame(odd, DayShadeBase{}, 1.f).minEV100 == DayShadeBase{}.minEV100);
}

TEST_CASE("dayShadeFrame: full day lifts the shade") {
    DayShadeBase base;
    base.filmToe = 0.62f;
    const DayShadeFrame f = dayShadeFrame(DayShadeLook{}, base, 1.f);
    CHECK(f.skyScale > 1.f);
    CHECK(f.shadowContrast < 1.f);
    CHECK(f.diffuseBoost > 1.f);
    CHECK(f.filmToe < 0.62f);
    const DayShadeFrame half = dayShadeFrame(DayShadeLook{}, base, 0.5f);
    CHECK(half.skyScale == doctest::Approx((1.f + f.skyScale) * 0.5f));
}

TEST_CASE("dayShadeFrame: clamps to the engine's ranges") {
    DayShadeLook wild;
    wild.skyScale = -1.f;
    wild.shadowContrast = 2.f;
    wild.diffuseBoost = 9.f;
    wild.filmToe = -1.f;
    wild.highlightContrast = -2.f;
    wild.minEV100 = 99.f;
    wild.fogMaxOpacity = 4.f;
    wild.fogSky = {-1.f, 0.5f, 3.f};
    const DayShadeFrame f = dayShadeFrame(wild, DayShadeBase{}, 3.f);
    CHECK(f.highlightContrast == 0.f);
    CHECK(f.minEV100 == 25.f);
    CHECK(f.fogMaxOpacity == 1.f);
    CHECK(f.fogSky == std::array<float, 3>{0.f, 0.5f, 1.f});
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
