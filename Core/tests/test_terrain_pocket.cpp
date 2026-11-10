// Stuck pockets and the plinths that close them (ftcore/TerrainPocket.h).

#include "ftcore/TerrainPocket.h"

#include <ostream>
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

using namespace ftcore;

namespace {

// A 60 x 60 m field, 1 m samples from (0, 0), heights from a function of the sample's plan point.
struct Field {
    std::vector<std::uint16_t> grid;
    MassingGround ground;
    explicit Field(const std::function<double(double, double)>& h, std::size_t n = 61) : grid(n * n) {
        for (std::size_t j = 0; j < n; ++j)
            for (std::size_t i = 0; i < n; ++i)
                grid[j * n + i] = static_cast<std::uint16_t>(std::lround(32768.0 + h(static_cast<double>(i), static_cast<double>(j)) * 128.0));
        ground = MassingGround{grid.data(), n, n, 1.0, 0.0, 0.0};
    }
};

MassingBox building(const char* id, double x0, double y0, double x1, double y1, double z0, double z1) {
    MassingBox b;
    b.id = id;
    b.kind = "kit";
    b.source = MassingSource::Invented;
    b.cx = (x0 + x1) * 0.5, b.cy = (y0 + y1) * 0.5, b.sx = x1 - x0, b.sy = y1 - y0;
    b.z0 = z0, b.z1 = z1;
    return b;
}

const PocketRect kRect{0.0, 0.0, 60.0, 60.0};

PocketReport check(const Field& f, const std::vector<MassingBox>& boxes, const PocketSpec& spec = PocketSpec{}) {
    PocketReport r;
    const std::vector<bool> include(boxes.size(), true);
    REQUIRE(findPockets(f.ground, boxes, include, kRect, {}, spec, r));
    return r;
}

// A pit one sample wide (x 20) from y 20 to 30, `depth` under the 10 m ground, its sides steeper than walkable.
double pit(double x, double y, double depth) { return (x == 20.0 && y >= 20.0 && y <= 30.0) ? 10.0 - depth : 10.0; }

} // namespace

TEST_CASE("flat ground and a building on it leave no pocket") {
    const Field f([](double, double) { return 10.0; });
    const PocketReport r = check(f, {building("B1", 20, 20, 30, 30, 9.5, 20)});
    CHECK(r.pockets.empty());
    CHECK(r.cellsX == 120);
    CHECK(r.reached > 0);
}

TEST_CASE("a narrow pit with steep sides is a pocket, and a fill closes it") {
    const Field f([](double x, double y) { return pit(x, y, 2.0); });
    const PocketReport r = check(f, {});
    REQUIRE(r.pockets.size() == 1);
    const Pocket& p = r.pockets[0];
    CHECK(p.cx == doctest::Approx(20.0).epsilon(0.02));
    CHECK(p.width <= 2.0);
    CHECK(p.hasSpill);
    // The spill: the pit's rim as the 0.5 m cells sample it (bilinear), under the field's 10 m.
    CHECK(p.spill > 9.5);
    CHECK(p.spill <= 10.0);
    CHECK(p.floorMin < 9.0);

    std::vector<MassingBox> made;
    PlinthStats stats;
    REQUIRE(planPlinths(f.ground, {}, {}, kRect, {}, PlinthSpec{}, made, stats));
    CHECK(stats.pocketsBefore == 1);
    CHECK(stats.fills > 0);
    CHECK(stats.skirts == 0);
    CHECK(stats.pocketsAfter == 0);
    for (const MassingBox& b : made) {
        CHECK(b.kind == "plinth");
        CHECK(b.z1 == doctest::Approx(p.spill));
        CHECK(b.z0 < p.spill - 0.5);
    }
}

TEST_CASE("a shallow pit is walked out of; a jump gets out of a deeper one") {
    CHECK(check(Field([](double x, double y) { return pit(x, y, 0.3); }), {}).pockets.empty());
    const Field deeper([](double x, double y) { return x >= 20.0 && x <= 23.0 && y >= 20.0 && y <= 30.0 ? 9.2 : 10.0; });
    // The 0.8 m sides over 1 m are walkable slopes here: no pocket either way.
    CHECK(check(deeper, {}).pockets.empty());
    // A box rim 0.8 m high round a flat yard entered by a drop: a pocket on foot, none with a 0.9 m jump.
    const Field flat([](double, double) { return 10.0; });
    std::vector<MassingBox> rim = {building("N", 20, 30, 30, 31, 9.0, 10.8), building("S", 20, 19, 30, 20, 9.0, 10.8),
                                   building("W", 19, 19, 20, 31, 9.0, 10.8), building("E", 30, 19, 31, 31, 9.0, 10.8),
                                   building("STEP", 18, 24, 19, 26, 9.0, 10.4)};
    for (MassingBox& b : rim) b.kind = "wall";
    const PocketReport walk = check(flat, rim);
    // The rim's top is reached from the step (0.4 m, then 0.4 m), the yard by a drop off it.
    REQUIRE(walk.pockets.size() == 1);
    CHECK(walk.pockets[0].width > 2.0);
    PocketSpec jump;
    jump.jumpRise = 0.9;
    CHECK(check(flat, rim, jump).pockets.empty());
}

TEST_CASE("a slot narrower than the capsule isn't entered") {
    // Two walls 0.5 m apart on flat ground, a drop between them from a step: the slot is squeezed.
    const Field flat([](double, double) { return 10.0; });
    std::vector<MassingBox> boxes = {building("A", 20, 20, 22, 30, 9.0, 10.4), building("B", 22.5, 20, 24.5, 30, 9.0, 10.4),
                                     building("C", 22, 19.5, 22.5, 20, 9.0, 10.4), building("D", 22, 30, 22.5, 30.5, 9.0, 10.4)};
    CHECK(check(flat, boxes).pockets.empty());
}

TEST_CASE("water is out of play") {
    // A cliff into the sea: its face is entered from the top and left by dropping into the water.
    const Field f([](double x, double) { return x < 30.0 ? -2.0 : 10.0; });
    const PocketReport r = check(f, {});
    CHECK(r.pockets.empty());
    CHECK(r.inPlay < r.cellsX * r.cellsY);
}

TEST_CASE("a trough between a facade and a steep bank gets a skirt and is closed") {
    // A building's east face at x 20; past it a 1 m strip at 10 m, then a bank rising 1.5 m in 1 m to a walkable terrace
    // at 11.5 m. The strip's ends rise as steeply: a trough entered by sliding down the bank.
    const Field f([](double x, double y) {
        double h = 10.0;
        if (x >= 22.0) h = 11.5;
        if (y < 15.0 || y > 45.0) h = 11.5;
        return h;
    });
    const std::vector<MassingBox> boxes = {building("BLD", 5, 15, 20, 45, 9.0, 20.0)};
    const PocketReport before = check(f, boxes);
    REQUIRE_FALSE(before.pockets.empty());
    CHECK(before.pockets[0].nearBox == 0);

    std::vector<MassingBox> made;
    PlinthStats stats;
    REQUIRE(planPlinths(f.ground, boxes, {true}, kRect, {}, PlinthSpec{}, made, stats));
    CHECK(stats.slopedBuildings == 1);
    CHECK(stats.skirts > 0);
    CHECK(stats.pocketsAfter == 0);
    bool eastSkirt = false;
    for (const MassingBox& b : made) {
        CHECK(b.z0 < b.z1);
        if (b.id.rfind("PL-S-", 0) == 0 && b.cx > 20.0 && b.cx < 22.5 && b.sy > 10.0) {
            eastSkirt = true;
            CHECK(b.z1 == doctest::Approx(11.5).epsilon(0.01));
            CHECK(b.sx == doctest::Approx(2.0));
        }
    }
    CHECK(eastSkirt);
}

TEST_CASE("a wide pit gets a stair out, not a fill; an exempt rect drops it from the report") {
    const Field f([](double x, double y) { return (x >= 20.0 && x <= 30.0 && y >= 20.0 && y <= 30.0) ? 7.0 : 10.0; });
    std::vector<MassingBox> made;
    PlinthStats stats;
    REQUIRE(planPlinths(f.ground, {}, {}, kRect, {}, PlinthSpec{}, made, stats));
    CHECK(stats.pocketsBefore == 1);
    CHECK(stats.fills == 0);
    CHECK(stats.stairedPockets == 1);
    CHECK(stats.stairs >= 10); // 3 m in 0.3 m risers
    CHECK(stats.pocketsAfter == 0);
    double prev = 10.3; // the first step is level with the rim
    for (const MassingBox& b : made) {
        CHECK(b.id.rfind("PL-X-", 0) == 0);
        CHECK(b.z1 < prev); // each step lower, by a riser
        CHECK(prev - b.z1 <= 0.45 + 1e-9);
        prev = b.z1;
    }
    // Without stairs it stays, as wide.
    PlinthSpec none;
    none.stairs = false;
    made.clear();
    REQUIRE(planPlinths(f.ground, {}, {}, kRect, {}, none, made, stats));
    CHECK(stats.pocketsAfter == 1);
    CHECK(stats.wideLeft == 1);
    // Exempt: not reported.
    PocketSpec spec;
    REQUIRE(parsePocketRects("19,19,31,31", spec.exempt));
    const PocketReport r = check(f, {}, spec);
    CHECK(r.pockets.empty());
    CHECK(r.exempted == 1);
    // A rect that holds only part of it doesn't.
    REQUIRE(parsePocketRects("19,19,25,31;0,0,1,1", spec.exempt));
    CHECK(spec.exempt.size() == 2);
    CHECK(check(f, {}, spec).pockets.size() == 1);
    std::vector<PocketRect> bad;
    CHECK_FALSE(parsePocketRects("1,2,3", bad));
    CHECK_FALSE(parsePocketRects("1,2,3,x", bad));
}

TEST_CASE("pocketRectOf grows the boxes' bounds and cuts them to the field") {
    const Field f([](double, double) { return 10.0; });
    const std::vector<MassingBox> boxes = {building("A", 10, 10, 20, 20, 9, 15), building("B", 50, 40, 58, 50, 9, 15)};
    const PocketRect r = pocketRectOf(f.ground, boxes, {true, true}, 5.0);
    CHECK(r.minX == doctest::Approx(5.0));
    CHECK(r.minY == doctest::Approx(5.0));
    CHECK(r.maxX == doctest::Approx(60.0));
    CHECK(r.maxY == doctest::Approx(55.0));
    CHECK(pocketBuilding(boxes[0]));
    MassingBox low = boxes[0];
    low.z1 = 10.0;
    CHECK_FALSE(pocketBuilding(low));
}
