#include <doctest/doctest.h>
#include "ftcore/ParkedCarLook.h"

#include <string>

using namespace ftcore;

TEST_CASE("parkedCarLook: every paint shows up, dirt stays in range, no lamps by default") {
    int seen[kParkedCarPaints] = {};
    float lo = 2.f, hi = -1.f;
    for (int i = 0; i < 400; ++i) {
        const std::string id = "bay-" + std::to_string(i) + "-car";
        const ParkedCarLook look = parkedCarLook(id, 1110U);
        REQUIRE(look.paint >= 0);
        REQUIRE(look.paint < kParkedCarPaints);
        ++seen[look.paint];
        CHECK(look.dirt >= kParkedCarDirtMin);
        CHECK(look.dirt <= kParkedCarDirtMax);
        CHECK(look.lamp == 0.f);
        lo = look.dirt < lo ? look.dirt : lo;
        hi = look.dirt > hi ? look.dirt : hi;
    }
    for (int p = 0; p < kParkedCarPaints; ++p) CHECK(seen[p] > 20); // about 50 each
    CHECK(lo < 0.3f);
    CHECK(hi > 0.85f);
}

TEST_CASE("parkedCarLook: the same id and seed give the same car; the seed changes the street") {
    const ParkedCarLook a = parkedCarLook("R1-bay-7-car", 1110U);
    const ParkedCarLook b = parkedCarLook("R1-bay-7-car", 1110U);
    CHECK(a.paint == b.paint);
    CHECK(a.dirt == b.dirt);
    int differ = 0;
    for (int i = 0; i < 64; ++i) {
        const std::string id = "R1-bay-" + std::to_string(i) + "-car";
        differ += parkedCarLook(id, 1110U).paint != parkedCarLook(id, 1111U).paint ? 1 : 0;
    }
    CHECK(differ > 32);
}

TEST_CASE("parkedCarLook: lampShare lights about that share") {
    int lit = 0;
    for (int i = 0; i < 1000; ++i) lit += parkedCarLook("car-" + std::to_string(i), 7U, 0.1f).lamp > 0.f ? 1 : 0;
    CHECK(lit > 60);
    CHECK(lit < 140);
    CHECK(parkedCarLook("car-1", 7U, 1.f).lamp == 1.f);
}

TEST_CASE("parkedCarCustomData: paint rgb, dirt, lamp in slots 0..4") {
    ParkedCarLook look;
    look.paint = 5;
    look.color = parkedCarPaint(5);
    look.dirt = 0.4f;
    look.lamp = 1.f;
    float d[kParkedCarCustomFloats] = {};
    parkedCarCustomData(look, d);
    CHECK(d[0] == doctest::Approx(0.42f));
    CHECK(d[1] == doctest::Approx(0.07f));
    CHECK(d[2] == doctest::Approx(0.05f));
    CHECK(d[3] == doctest::Approx(0.4f));
    CHECK(d[4] == 1.f);
    CHECK(parkedCarPaint(-1).r == doctest::Approx(0.255f));
    CHECK(parkedCarPaint(kParkedCarPaints).b == doctest::Approx(0.366f));
}
