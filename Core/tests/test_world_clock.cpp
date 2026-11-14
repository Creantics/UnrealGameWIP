#include "ftcore/StorySave.h"
#include "ftcore/WorldClock.h"

#include "TestAssets.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace ftcore;

TEST_CASE("world clock: a day takes dayMinutes of real time and counts days") {
    WorldClock c;
    c.hours = 23.0;
    c.dayMinutes = 48.0f;
    worldClockTick(c, 2.0f * 60.0f);  // 2 real minutes = 1 game hour
    CHECK(c.hours == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(c.day == 1u);
    for (int i = 0; i < 48 * 60 + 60; ++i) worldClockTick(c, 1.0f);  // a day and half an hour more in 1 s steps
    CHECK(c.day == 2u);
    CHECK(c.hours == doctest::Approx(0.5).epsilon(1e-4));
    worldClockSet(c, 0.0);
    worldClockTick(c, 3.0f * 48.0f * 60.0f + 60.0f);  // three days and half an hour in one step
    CHECK(c.day == 5u);
    CHECK(c.hours == doctest::Approx(0.5).epsilon(1e-6));
}

TEST_CASE("world clock: pause, resume, bad dt, set and advance") {
    WorldClock c;
    worldClockPause(c);
    worldClockTick(c, 600.0f);
    CHECK(c.hours == 12.0);
    worldClockResume(c);
    worldClockTick(c, -5.0f);
    worldClockTick(c, std::nanf(""));
    CHECK(c.hours == 12.0);
    worldClockSet(c, 30.5);  // wraps; no day added
    CHECK(c.hours == doctest::Approx(6.5));
    CHECK(c.day == 0u);
    worldClockSet(c, -1.0);
    CHECK(c.hours == doctest::Approx(23.0));
    worldClockSet(c, std::nan(""));
    CHECK(c.hours == doctest::Approx(23.0));
    worldClockAdvanceTo(c, 7.0);  // past midnight: a new day
    CHECK(c.hours == doctest::Approx(7.0));
    CHECK(c.day == 1u);
    worldClockAdvanceTo(c, 7.0);  // already there: stays
    CHECK(c.day == 1u);
    worldClockSetDayMinutes(c, 0.0f);
    CHECK(c.dayMinutes == kWorldClockMinDayMinutes);
    worldClockSetDayMinutes(c, 1e9f);
    CHECK(c.dayMinutes == kWorldClockMaxDayMinutes);
}

TEST_CASE("world clock: sun and night amount") {
    const WorldClockSpec spec;
    CHECK(sunElevationDeg(spec, 12.0) == doctest::Approx(90.0f - spec.latitudeDeg).epsilon(1e-4));
    CHECK(sunElevationDeg(spec, 0.0) == doctest::Approx(-(90.0f - spec.latitudeDeg)).epsilon(1e-4));
    CHECK(sunElevationDeg(spec, 6.0) == doctest::Approx(0.0f).epsilon(1e-4));
    const SunDirection morning = sunDirection(spec, 8.0), evening = sunDirection(spec, 16.0), noon = sunDirection(spec, 12.0);
    CHECK(morning.east > 0.0f);
    CHECK(evening.east < 0.0f);
    CHECK(noon.north < 0.0f);  // the sun stands to the south north of the tropics
    CHECK(std::sqrt(morning.east * morning.east + morning.north * morning.north + morning.up * morning.up) ==
          doctest::Approx(1.0f));
    // The start hour's sun: low (25..30 degrees) and turned off the plan's north-south axis by about 20 degrees (from
    // the south-south-east), so it rakes along a north-south street.
    const float startUp = sunElevationDeg(spec, kWorldClockStartHours);
    CHECK(startUp > 25.0f);
    CHECK(startUp < 30.0f);
    const SunDirection start = sunDirection(spec, kWorldClockStartHours);
    const float azimuth = std::atan2(start.east, start.north) * 180.0f / 3.14159265f;  // from north towards east
    CHECK(azimuth == doctest::Approx(160.0f).epsilon(0.02));
    WorldClockSpec plain;
    plain.azimuthOffsetDeg = 0.0f;
    const SunDirection eastern = sunDirection(plain, 6.0);  // no offset: it rises due east
    CHECK(eastern.east == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(sunElevationDeg(plain, 8.0) == doctest::Approx(startUp));  // the offset turns, never lifts
    CHECK(nightAmount(spec, 12.0) == 0.0f);
    CHECK(nightAmount(spec, 0.0) == 1.0f);
    CHECK(nightAmount(spec, 6.0) == doctest::Approx(0.5f));
    CHECK(nightAmount(spec, 18.0) == doctest::Approx(0.5f));
    float last = -1.0f;
    for (double h = 12.0; h <= 24.0; h += 0.25) {  // falls monotonically through dusk
        const float n = nightAmount(spec, h);
        CHECK(n >= last);
        last = n;
    }
}

TEST_CASE("time curves: parse, wrap and the shipped table") {
    TimeCurves t;
    std::string err;
    REQUIRE(parseTimeCurves("# c\ncurve,0,6,12,18\nCrowdDensity,0.2,0.4,1,0.6\nDanger,1,0.5,0,0.5\n", t, &err));
    CHECK(timeCurveAt(t.crowd, 3.0) == doctest::Approx(0.3f));
    CHECK(timeCurveAt(t.crowd, 21.0) == doctest::Approx(0.4f));   // 18 -> 24(=0): 0.6 .. 0.2
    CHECK(timeCurveAt(t.crowd, 24.0) == doctest::Approx(0.2f));
    CHECK(timeCurveAt(t.danger, -6.0) == doctest::Approx(0.5f));  // wraps to 18
    TimeCurves late;
    REQUIRE(parseTimeCurves("curve,4,20\nCrowdDensity,0,1\nDanger,1,0\n", late));
    CHECK(timeCurveAt(late.crowd, 2.0) == doctest::Approx(0.25f));  // between 20 and 4 (+24): 1 .. 0, 6 of 8 h
    CHECK(timeCurveAt(TimeCurve{}, 5.0) == 0.0f);

    CHECK_FALSE(parseTimeCurves("curve,0,6\nCrowdDensity,0,1\n", t, &err));                       // no Danger
    CHECK(err.find("Danger") != std::string::npos);
    CHECK_FALSE(parseTimeCurves("curve,6,0\nCrowdDensity,0,1\nDanger,0,1\n", t));                 // hours fall
    CHECK_FALSE(parseTimeCurves("curve,0,24\nCrowdDensity,0,1\nDanger,0,1\n", t));                // hour 24
    CHECK_FALSE(parseTimeCurves("curve,0,6\nCrowdDensity,0,1.5\nDanger,0,1\n", t));               // value > 1
    CHECK_FALSE(parseTimeCurves("curve,0,6\nCrowdDensity,0\nDanger,0,1\n", t));                   // short row
    CHECK_FALSE(parseTimeCurves("curve,0,6\nCrowdDensity,0,1\nDanger,0,1\nRain,0,1\n", t));       // unknown
    CHECK_FALSE(parseTimeCurves("curve,0,6\nCrowdDensity,0,1\nCrowdDensity,0,1\nDanger,0,1\n", t));

    std::string text;
    REQUIRE(ftcore_test::readProjectData("time_curves.csv", text));
    REQUIRE(parseTimeCurves(text, t, &err));
    CHECK(timeCurveAt(t.crowd, 12.0) == doctest::Approx(1.0f));
    CHECK(timeCurveAt(t.crowd, 4.0) == doctest::Approx(0.05f));
    CHECK(timeCurveAt(t.danger, 4.0) == doctest::Approx(1.0f));
    CHECK(timeCurveAt(t.danger, 12.0) == doctest::Approx(0.1f));
    // Busier and safer by day, quieter and more dangerous at night.
    CHECK(timeCurveAt(t.crowd, 12.0) > timeCurveAt(t.crowd, 0.0));
    CHECK(timeCurveAt(t.danger, 0.0) > timeCurveAt(t.danger, 12.0));
}

TEST_CASE("world clock: save round trip and sanitising") {
    StorySave s;
    s.scene = "a";
    s.hasClock = true;
    s.clock.hours = 22.123456789012345;
    s.clock.paused = true;
    s.clock.dayMinutes = 36.5f;
    s.clock.day = 41;
    StorySave back;
    std::string err;
    REQUIRE(decodeStorySave(encodeStorySave(s), back, &err));
    CHECK(back.hasClock);
    CHECK(back.clock.hours == s.clock.hours);  // exact
    CHECK(back.clock.paused);
    CHECK(back.clock.dayMinutes == s.clock.dayMinutes);
    CHECK(back.clock.day == 41u);

    StorySave noClock;
    noClock.scene = "a";
    REQUIRE(decodeStorySave(encodeStorySave(noClock), back));
    CHECK_FALSE(back.hasClock);
    CHECK(back.clock.hours == 12.0);

    CHECK_FALSE(decodeStorySave(R"({"format":2,"scene":"a","beat":0,"flags":[],"seen":[],"clock":{"hours":"x"}})", back));
    REQUIRE(decodeStorySave(
        R"({"format":2,"scene":"a","beat":0,"flags":[],"seen":[],"clock":{"hours":30,"paused":false,"dayMinutes":-4,"day":3}})",
        back, &err));
    CHECK(back.clock.hours == doctest::Approx(6.0));
    CHECK(back.clock.dayMinutes == kWorldClockMinDayMinutes);
}

TEST_CASE("clock hands: hour and minute angles clockwise from twelve") {
    using ftcore::clockHands;
    CHECK(clockHands(0.0).hourDeg == doctest::Approx(0.0));
    CHECK(clockHands(0.0).minuteDeg == doctest::Approx(0.0));
    CHECK(clockHands(3.0).hourDeg == doctest::Approx(90.0));
    CHECK(clockHands(15.0).hourDeg == doctest::Approx(90.0));
    CHECK(clockHands(15.0).minuteDeg == doctest::Approx(0.0));
    CHECK(clockHands(10.5).hourDeg == doctest::Approx(315.0));
    CHECK(clockHands(10.5).minuteDeg == doctest::Approx(180.0));
    CHECK(clockHands(21.25).hourDeg == doctest::Approx(277.5));
    CHECK(clockHands(21.25).minuteDeg == doctest::Approx(90.0));
    CHECK(clockHands(-1.0).hourDeg == doctest::Approx(330.0));
    CHECK(clockHands(std::nan("")).hourDeg == doctest::Approx(0.0));
    CHECK(clockHands(std::nan("")).minuteDeg == doctest::Approx(0.0));
}
