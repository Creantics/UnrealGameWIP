#include <doctest/doctest.h>

#include "AllocCounter.h"
#include "ftcore/WaterStrand.h"

using namespace ftcore;

namespace {

// A still canal 400 m by 12 m, mean water at -1 m, bed at -4 m: its tide runs -1.175 .. -0.825 m (canal gain 0.35).
WaterBody canal() {
    WaterBody b;
    b.kind = WaterKind::Canal;
    b.shape = WaterShape::Box;
    b.half = {200.f, 6.f};
    b.level = -1.f;
    b.bed = -4.f;
    return b;
}

WaterWayOut way(WaterWayKind kind, float x, float bottomZ, float topZ) {
    WaterWayOut w;
    w.kind = kind;
    w.at = {x, 5.5f};
    w.bottomZ = bottomZ;
    w.topZ = topZ;
    return w;
}

} // namespace

TEST_CASE("A way out is usable by its kind: a rung under the water, a wall top within the climb") {
    const WaterStrandSpec s;
    CHECK(waterWayUsable(way(WaterWayKind::Ladder, 0.f, -4.f, 1.f), -1.f, s));
    CHECK_FALSE(waterWayUsable(way(WaterWayKind::Steps, 0.f, -1.f, 1.f), -1.f, s));
    CHECK(waterWayUsable(way(WaterWayKind::Steps, 0.f, -1.3f, 1.f), -1.f, s));
    CHECK(waterWayUsable(way(WaterWayKind::Mantle, 0.f, 0.f, 1.f), -1.f, s));
    CHECK_FALSE(waterWayUsable(way(WaterWayKind::Mantle, 0.f, 0.f, 1.1f), -1.f, s));
}

TEST_CASE("No tide strands a swimmer: a failing way passes with a usable one within 25 m") {
    WaterBodies water;
    REQUIRE(water.add(canal()));
    const WaterSpec spec;
    // A wall 2.1 m over low water fails at low tide; the ladder 20 m on covers it.
    WaterWayOut ways[] = {way(WaterWayKind::Mantle, 0.f, 0.f, 1.0f), way(WaterWayKind::Ladder, 20.f, -4.f, 1.f)};
    WaterStrandReport r = waterStrandCheck(water, spec, ways, 2);
    CHECK(r.ok());
    CHECK(r.ways == 2);
    CHECK(r.checks == 18);
    // 40 m on, it doesn't: the wall strands at the low levels only.
    ways[1].at.x = 40.f;
    r = waterStrandCheck(water, spec, ways, 2);
    CHECK_FALSE(r.ok());
    CHECK(r.stranded > 0);
    CHECK(r.stranded < 9);
    CHECK(r.first[0].way == 0);
    CHECK(r.first[0].levelM == doctest::Approx(-1.175f));
}

TEST_CASE("Steps that end at mean water fail at every level of the band; a body with no way out fails") {
    WaterBodies water;
    REQUIRE(water.add(canal()));
    WaterBody pool = canal();
    pool.kind = WaterKind::Pool;
    pool.centre = {0.f, 100.f};
    REQUIRE(water.add(pool));
    const WaterWayOut ways[] = {way(WaterWayKind::Steps, 0.f, -1.f, 0.5f), way(WaterWayKind::Steps, 0.f, -1.f, 0.5f)};
    const WaterWayOut far = way(WaterWayKind::Ladder, 0.f, -4.f, 1.f);
    WaterWayOut out[] = {ways[0], far};
    out[1].at = {0.f, 300.f}; // in no body
    const WaterStrandReport r = waterStrandCheck(water, WaterSpec{}, out, 2);
    CHECK(r.stranded == 9);
    CHECK(r.bodiesWithout == 1);
    CHECK(r.unplaced == 1);
}

TEST_CASE("The map's exits run to the bed, so they never strand") {
    WaterBodies water;
    REQUIRE(water.add(canal()));
    WaterExit exit;
    exit.foot = {10.f, 5.5f};
    exit.top = {10.f, 6.5f};
    exit.topZ = 0.5f;
    exit.out = {0.f, 1.f};
    const WaterWayOut w = waterWayOutFromExit(water, exit);
    CHECK(w.kind == WaterWayKind::Ladder);
    CHECK(w.bottomZ == doctest::Approx(-4.f));
    const auto before = ftcore_test::allocStats().count;
    const WaterStrandReport r = waterStrandCheck(water, WaterSpec{}, &w, 1);
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(r.ok());
}
