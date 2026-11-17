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

TEST_CASE("Every failure is listed in order: a body without a way doesn't get overwritten") {
    WaterBodies water;
    REQUIRE(water.add(canal()));
    WaterBody pool = canal();
    pool.kind = WaterKind::Pool;
    pool.centre = {0.f, 100.f};
    REQUIRE(water.add(pool));
    WaterBody third = canal();
    third.centre = {0.f, 200.f};
    REQUIRE(water.add(third));
    WaterWayOut ways[] = {way(WaterWayKind::Steps, 0.f, -0.5f, 0.5f), way(WaterWayKind::Steps, 0.f, -0.5f, 0.5f)};
    ways[1].at = {0.f, 200.f};
    WaterStrandSpec one;
    one.levels = 1;
    const WaterStrandReport r = waterStrandCheck(water, WaterSpec{}, ways, 2, one);
    CHECK(r.stranded == 2);
    CHECK(r.bodiesWithout == 1);
    REQUIRE(r.listed == 3);
    CHECK(r.first[0].way == 0);
    CHECK(r.first[0].body == 0);
    CHECK(r.first[1].way == -1);
    CHECK(r.first[1].body == 1);
    CHECK(r.first[2].way == 1);
    CHECK(r.first[2].body == 2);
}

TEST_CASE("A way in no body fails the check; no ways at all fails it") {
    WaterBodies water;
    REQUIRE(water.add(canal()));
    WaterWayOut ways[] = {way(WaterWayKind::Ladder, 0.f, -4.f, 1.f), way(WaterWayKind::Ladder, 0.f, -4.f, 1.f)};
    ways[1].at = {0.f, 300.f};
    const WaterStrandReport r = waterStrandCheck(water, WaterSpec{}, ways, 2);
    CHECK(r.stranded == 0);
    CHECK(r.unplaced == 1);
    CHECK_FALSE(r.ok());
    const WaterStrandReport none = waterStrandCheck(water, WaterSpec{}, nullptr, 0);
    CHECK(none.bodiesWithout == 1);
    CHECK_FALSE(none.ok());
}

TEST_CASE("Coverage: an entry point exactly 25 m from a usable way passes, a hair over fails") {
    WaterBodies water;
    REQUIRE(water.add(canal()));
    const WaterWayOut ladder = way(WaterWayKind::Ladder, 0.f, -4.f, 1.f);
    const WaterEntryPoint at25[] = {{{25.f, 5.5f}, 0}};
    const WaterCoverageReport r = waterExitCoverage(water, WaterSpec{}, at25, 1, &ladder, 1);
    CHECK(r.ok());
    CHECK(r.levels == 9);
    CHECK(r.worst[0].distM == 25.f);
    const WaterEntryPoint over[] = {{{25.01f, 5.5f}, 0}};
    CHECK_FALSE(waterExitCoverage(water, WaterSpec{}, over, 1, &ladder, 1).ok());
}

TEST_CASE("Coverage: two good ladders 60 m apart pass the strand check but not coverage") {
    WaterBodies water;
    REQUIRE(water.add(canal()));
    const WaterWayOut ways[] = {way(WaterWayKind::Ladder, 0.f, -4.f, 1.f), way(WaterWayKind::Ladder, 60.f, -4.f, 1.f)};
    CHECK(waterStrandCheck(water, WaterSpec{}, ways, 2).ok());
    WaterEntryPoint bank[41];
    for (int i = 0; i < 41; ++i) bank[i] = {{-10.f + 2.f * static_cast<float>(i), 5.5f}, 0}; // every 2 m, x -10 .. 70
    const auto before = ftcore_test::allocStats().count;
    const WaterCoverageReport r = waterExitCoverage(water, WaterSpec{}, bank, 41, ways, 2);
    CHECK(ftcore_test::allocStats().count == before);
    CHECK_FALSE(r.ok());
    CHECK(r.points == 41);
    CHECK(r.uncovered > 0);
    for (int l = 0; l < r.levels; ++l) {
        CHECK(r.worst[l].point == 20); // x = 30, halfway
        CHECK(r.worst[l].distM == doctest::Approx(30.f));
    }
}

TEST_CASE("Coverage: no points proves nothing; a point naming no body fails; one level is low water") {
    WaterBodies water;
    REQUIRE(water.add(canal()));
    const WaterWayOut ladder = way(WaterWayKind::Ladder, 0.f, -4.f, 1.f);
    const WaterCoverageReport none = waterExitCoverage(water, WaterSpec{}, nullptr, 0, &ladder, 1);
    CHECK(none.points == 0);
    CHECK_FALSE(none.ok());
    const WaterEntryPoint lost[] = {{{0.f, 5.5f}, 0}, {{0.f, 5.5f}, 7}};
    const WaterCoverageReport r = waterExitCoverage(water, WaterSpec{}, lost, 2, &ladder, 1);
    CHECK(r.unplaced == 1);
    CHECK_FALSE(r.ok());
    WaterStrandSpec one;
    one.levels = 1;
    const WaterCoverageReport low = waterExitCoverage(water, WaterSpec{}, lost, 1, &ladder, 1, one);
    CHECK(low.levels == 1);
    CHECK(low.ok());
    const WaterBody& b = water.body(0);
    const WaterSpec spec{};
    float lo = 0.f, hi = 0.f;
    waterTideBand(spec.tide, b, waterWaveClass(b, spec.waves), lo, hi);
    CHECK(low.worst[0].levelM == doctest::Approx(b.level + lo));
    CHECK(lo < hi);
}

TEST_CASE("Coverage: a body with no usable way at low water reports no way") {
    WaterBodies water;
    REQUIRE(water.add(canal()));
    const WaterWayOut steps = way(WaterWayKind::Steps, 0.f, -1.f, 0.5f); // ends at mean water: never usable
    const WaterEntryPoint p[] = {{{0.f, 5.5f}, 0}};
    const WaterCoverageReport r = waterExitCoverage(water, WaterSpec{}, p, 1, &steps, 1);
    CHECK_FALSE(r.ok());
    CHECK(r.worst[0].distM == kWaterCoverageNoWayM);
}
