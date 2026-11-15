#include "ftcore/WaterBodies.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cmath>
#include <initializer_list>

using namespace ftcore;

namespace {

// The wall basin as the strip builder sets it (plan frame): the sea past x 940, dived in x 940..1000, y 620..720.
WaterBody basinSea() {
    WaterBody sea = waterSea(940.f, 0.f, -33.f);
    sea.swimDeep = true;
    sea.hasDiveArea = true;
    sea.diveMin = {940.f, 620.f};
    sea.diveMax = {1000.f, 720.f};
    return sea;
}

} // namespace

TEST_CASE("water dive: a body with no swimDeep allows no dive and has no edge") {
    WaterBodies water;
    REQUIRE(water.add(waterSea(940.f, 0.f, -33.f)));
    const WaterDive d = water.dive({960.f, 650.f});
    CHECK(d.body == 0);
    CHECK_FALSE(d.allowed);
    CHECK(d.edgeDistance == doctest::Approx(1.0e9f));
    CHECK(d.edgeInward.x == 0.f);
    CHECK(d.edgeInward.y == 0.f);
}

TEST_CASE("water dive: no body at p is no dive") {
    WaterBodies water;
    REQUIRE(water.add(basinSea()));
    const WaterDive d = water.dive({900.f, 650.f}); // on the land, west of the wall
    CHECK(d.body == -1);
    CHECK_FALSE(d.allowed);
    CHECK(d.edgeDistance == doctest::Approx(1.0e9f));
}

TEST_CASE("water dive: allowed inside the dive area, edge included, and not past it") {
    const WaterBody sea = basinSea();
    CHECK(sea.divesAt({960.f, 650.f}));
    CHECK(sea.divesAt({940.f, 620.f}));
    CHECK(sea.divesAt({1000.f, 720.f}));
    CHECK_FALSE(sea.divesAt({1000.5f, 650.f}));
    CHECK_FALSE(sea.divesAt({960.f, 619.f}));
    CHECK_FALSE(sea.divesAt({960.f, 721.f}));
    CHECK_FALSE(sea.divesAt({939.f, 650.f})); // not in the footprint at all
}

TEST_CASE("water dive: no dive area is the whole body") {
    WaterBody sea = waterSea(940.f, 0.f, -33.f);
    sea.swimDeep = true;
    CHECK(sea.divesAt({960.f, 10.f}));
    CHECK(sea.divesAt({2000.f, -500.f}));
    CHECK_FALSE(sea.divesAt({930.f, 10.f}));
}

TEST_CASE("water dive: the wall side pushes nothing; the open sides are the outer edge") {
    WaterBodies water;
    REQUIRE(water.add(basinSea()));
    SUBCASE("near the wall, the nearest open side is the edge, not the wall") {
        const WaterDive d = water.dive({941.f, 670.f}); // 1 m off the wall, 50 m from each long side, 59 m from x 1000
        CHECK(d.allowed);
        CHECK(d.edgeDistance == doctest::Approx(50.f));
        // The two long sides tie at 50 m: the south side is tried first and a tie keeps it, so the push is north.
        CHECK(d.edgeInward.y == 1.f);
        CHECK(d.edgeInward.x == 0.f);
    }
    SUBCASE("either side of the tie, the nearer long side wins") {
        const WaterDive south = water.dive({941.f, 669.f});
        CHECK(south.edgeDistance == doctest::Approx(49.f));
        CHECK(south.edgeInward.y == 1.f);
        const WaterDive north = water.dive({941.f, 671.f});
        CHECK(north.edgeDistance == doctest::Approx(49.f));
        CHECK(north.edgeInward.y == -1.f);
        CHECK(north.edgeInward.x == 0.f);
    }
    SUBCASE("exactly on an open side: allowed, zero distance, pushed in") {
        const WaterDive east = water.dive({1000.f, 670.f});
        CHECK(east.body == 0);
        CHECK(east.allowed);
        CHECK(east.edgeDistance == 0.f);
        CHECK(east.edgeInward.x == -1.f);
        CHECK(east.edgeInward.y == 0.f);
        const WaterDive north = water.dive({960.f, 720.f});
        CHECK(north.allowed);
        CHECK(north.edgeDistance == 0.f);
        CHECK(north.edgeInward.y == -1.f);
    }
    SUBCASE("a NaN point is no body, no dive and no edge") {
        for (const Vec2 p : {Vec2{std::nanf(""), 670.f}, Vec2{960.f, std::nanf("")}}) {
            const WaterDive d = water.dive(p);
            CHECK(d.body == -1);
            CHECK_FALSE(d.allowed);
            CHECK(d.edgeDistance == doctest::Approx(1.0e9f));
            CHECK(d.edgeInward.x == 0.f);
            CHECK(d.edgeInward.y == 0.f);
        }
    }
    SUBCASE("near the deep end, the buoy side pushes back west") {
        const WaterDive d = water.dive({995.f, 670.f});
        CHECK(d.edgeDistance == doctest::Approx(5.f));
        CHECK(d.edgeInward.x == doctest::Approx(-1.f));
        CHECK(d.edgeInward.y == 0.f);
    }
    SUBCASE("near the south side, it pushes north") {
        const WaterDive d = water.dive({960.f, 623.f});
        CHECK(d.edgeDistance == doctest::Approx(3.f));
        CHECK(d.edgeInward.y == doctest::Approx(1.f));
    }
    SUBCASE("past a side under water: negative distance, still pushed in, no dive allowed") {
        const WaterDive d = water.dive({1004.f, 670.f});
        CHECK(d.body == 0);
        CHECK_FALSE(d.allowed);
        CHECK(d.edgeDistance == doctest::Approx(-4.f));
        CHECK(d.edgeInward.x == doctest::Approx(-1.f));
    }
}

TEST_CASE("water dive: a half-plane with no dive area takes its buoy line as the edge") {
    WaterBodies water;
    WaterBody sea = waterSea(940.f, 0.f, -33.f, 60.f);
    sea.swimDeep = true;
    REQUIRE(water.add(sea));
    const WaterDive d = water.dive({990.f, 5.f}); // 50 m out, 10 m short of the line
    CHECK(d.allowed);
    CHECK(d.edgeDistance == doctest::Approx(10.f));
    CHECK(d.edgeInward.x == doctest::Approx(-1.f));
    CHECK(d.edgeInward.y == doctest::Approx(0.f));
}

TEST_CASE("water dive: a box body's dive area on its walls has no edge there") {
    WaterBodies water;
    WaterBody pool;
    pool.kind = WaterKind::Pool;
    pool.shape = WaterShape::Box;
    pool.level = 0.f;
    pool.bed = -5.f;
    pool.centre = {0.f, 0.f};
    pool.half = {10.f, 5.f};
    pool.swimDeep = true;
    pool.hasDiveArea = true;
    pool.diveMin = {-10.f, -5.f}; // the whole box: every side a wall
    pool.diveMax = {10.f, 5.f};
    REQUIRE(water.add(pool));
    const WaterDive d = water.dive({0.f, 0.f});
    CHECK(d.allowed);
    CHECK(d.edgeDistance == doctest::Approx(1.0e9f));
}

TEST_CASE("water dive: add refuses a dive area that is no box") {
    WaterBodies water;
    WaterBody sea = basinSea();
    sea.diveMax.x = sea.diveMin.x; // no width
    CHECK_FALSE(water.add(sea));
    sea = basinSea();
    sea.diveMin.y = std::nanf("");
    CHECK_FALSE(water.add(sea));
    CHECK(water.count() == 0);
    sea = basinSea();
    sea.hasDiveArea = false; // the corners are not read without an area
    sea.diveMin.y = std::nanf("");
    CHECK(water.add(sea));
}

TEST_CASE("water dive: the engine frame turns the area over with y") {
    const float side = 2000.f;
    const WaterBody sea = waterBodyToEngine(basinSea(), side);
    // Plan (x, y) -> engine (x - side/2, side/2 - y).
    CHECK(sea.swimDeep);
    CHECK(sea.hasDiveArea);
    CHECK(sea.diveMin.x == doctest::Approx(940.f - 1000.f));
    CHECK(sea.diveMax.x == doctest::Approx(1000.f - 1000.f));
    CHECK(sea.diveMin.y == doctest::Approx(1000.f - 720.f));
    CHECK(sea.diveMax.y == doctest::Approx(1000.f - 620.f));
    // A plan point and its engine twin agree.
    const Vec2 plan{995.f, 650.f};
    const Vec2 engine{plan.x - side * 0.5f, side * 0.5f - plan.y};
    CHECK(basinSea().divesAt(plan) == sea.divesAt(engine));
    WaterBodies planWater, engineWater;
    REQUIRE(planWater.add(basinSea()));
    REQUIRE(engineWater.add(sea));
    const WaterDive a = planWater.dive({960.f, 623.f});
    const WaterDive b = engineWater.dive({960.f - 1000.f, 1000.f - 623.f});
    CHECK(a.edgeDistance == doctest::Approx(b.edgeDistance));
    CHECK(a.edgeInward.x == doctest::Approx(b.edgeInward.x));
    CHECK(a.edgeInward.y == doctest::Approx(-b.edgeInward.y));
}

TEST_CASE("water dive: dive() does not allocate") {
    WaterBodies water;
    REQUIRE(water.add(basinSea()));
    const ftcore_test::AllocStats before = ftcore_test::allocStats();
    float sum = 0.f;
    for (int i = 0; i < 100; ++i) sum += water.dive({940.f + static_cast<float>(i) * 0.7f, 670.f}).edgeDistance;
    const ftcore_test::AllocStats after = ftcore_test::allocStats();
    CHECK(after.count == before.count);
    CHECK(sum > 0.f);
}
