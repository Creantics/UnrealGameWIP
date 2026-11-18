#include <doctest/doctest.h>
#include "ftcore/StillCam.h"

#include <string>

using namespace ftcore;

TEST_CASE("-StillCam reads six numbers or is off as a whole")
{
    SUBCASE("good input, spaces and a leading + allowed")
    {
        const StillCam c = parseStillCam("857.5, -262.0,5.7,90,-8.5,+60");
        CHECK(c.on);
        CHECK(c.x == doctest::Approx(857.5));
        CHECK(c.y == doctest::Approx(-262.0));
        CHECK(c.z == doctest::Approx(5.7));
        CHECK(c.yawDeg == doctest::Approx(90.0));
        CHECK(c.pitchDeg == doctest::Approx(-8.5));
        CHECK(c.fovDeg == doctest::Approx(60.0));
    }
    SUBCASE("missing or extra parts fall back to off")
    {
        CHECK_FALSE(parseStillCam("").on);
        CHECK_FALSE(parseStillCam("1,2,3,4,5").on);
        CHECK_FALSE(parseStillCam("1,2,3,4,5,60,7").on);
        CHECK_FALSE(parseStillCam("1,2,3,4,5,").on);
        CHECK_FALSE(parseStillCam("1,2,,4,5,60").on);
        CHECK_FALSE(parseStillCam(",1,2,3,4,5").on);
    }
    SUBCASE("bad input is off, never half read")
    {
        CHECK_FALSE(parseStillCam("1,2,3,4,5,sixty").on);
        CHECK_FALSE(parseStillCam("1,2,3m,4,5,60").on);
        CHECK_FALSE(parseStillCam("inf,2,3,4,5,60").on);
        CHECK_FALSE(parseStillCam("1,2,3,nan,5,60").on);
        CHECK_FALSE(parseStillCam("1;2;3;4;5;60").on);
        const StillCam off = parseStillCam("1,2,3,4,5");
        CHECK(off.x == 0.0);
        CHECK(off.fovDeg == 0.0);
    }
    SUBCASE("pitch and fov out of range are off")
    {
        CHECK(parseStillCam("0,0,0,0,-89,10").on);
        CHECK(parseStillCam("0,0,0,0,89,150").on);
        CHECK_FALSE(parseStillCam("0,0,0,0,-90,60").on);
        CHECK_FALSE(parseStillCam("0,0,0,0,0,9.9").on);
        CHECK_FALSE(parseStillCam("0,0,0,0,0,151").on);
    }
}

TEST_CASE("-Weather= knows dry only; a rain look's dry sibling")
{
    bool known = false;
    CHECK(parseWeatherForce("dry", &known) == WeatherForce::dry);
    CHECK(known);
    CHECK(parseWeatherForce(" DRY ") == WeatherForce::dry);
    CHECK(parseWeatherForce("", &known) == WeatherForce::none);
    CHECK(known);
    CHECK(parseWeatherForce("sunny", &known) == WeatherForce::none);
    CHECK_FALSE(known);
    CHECK(parseWeatherForce("dryish", &known) == WeatherForce::none);
    CHECK_FALSE(known);

    CHECK(std::string(dryLookFor("solhaven_rain")) == "solhaven");
    CHECK(std::string(dryLookFor("solhaven")) == "solhaven");
    CHECK(std::string(dryLookFor("lattice")) == "lattice");
    CHECK(std::string(dryLookFor("_rain")) == "_rain");
    CHECK(std::string(dryLookFor("")) == "");
}
