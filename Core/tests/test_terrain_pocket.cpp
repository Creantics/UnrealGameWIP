// Stuck pockets and the plinths that close them (ftcore/TerrainPocket.h).

#include "ftcore/TerrainPocket.h"

#include <ostream>
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
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

TEST_CASE("the skirts of two buildings in one line join into one box") {
    // The trough above, with the building cut in two at y 30: each half's east skirt ends where the other's starts.
    const Field f([](double x, double y) {
        double h = 10.0;
        if (x >= 22.0) h = 11.5;
        if (y < 15.0 || y > 45.0) h = 11.5;
        return h;
    });
    const std::vector<MassingBox> boxes = {building("A", 5, 15, 20, 30, 9.0, 20.0), building("B", 5, 30, 20, 45, 9.0, 20.0)};
    const auto eastSkirts = [](const std::vector<MassingBox>& made, double& longest) {
        int n = 0;
        longest = 0.0;
        for (const MassingBox& b : made) {
            if (b.id.rfind("PL-S-", 0) != 0 || b.cx <= 20.0 || b.cx >= 22.5) continue;
            ++n;
            longest = std::max(longest, b.sy);
            CHECK(b.sx == doctest::Approx(2.0));
            CHECK(b.cx == doctest::Approx(21.0));
            CHECK(b.z1 == doctest::Approx(11.5).epsilon(0.01));
        }
        return n;
    };
    PlinthSpec apart;
    apart.joinSkirts = false;
    std::vector<MassingBox> split, joined;
    PlinthStats splitStats, joinedStats;
    REQUIRE(planPlinths(f.ground, boxes, {true, true}, kRect, {}, apart, split, splitStats));
    REQUIRE(planPlinths(f.ground, boxes, {true, true}, kRect, {}, PlinthSpec{}, joined, joinedStats));
    double splitLongest = 0.0, joinedLongest = 0.0;
    const int splitCount = eastSkirts(split, splitLongest), joinedCount = eastSkirts(joined, joinedLongest);
    CHECK(splitCount >= 2);
    CHECK(joinedCount < splitCount);
    CHECK(joinedLongest > splitLongest);
    CHECK(joinedLongest <= 30.0 + 1e-9);
    CHECK(joinedStats.skirts < splitStats.skirts);
    CHECK(joinedStats.pocketsAfter == 0);
    // The joined boxes cover what the split ones covered: the same length of east skirt in all.
    double splitSum = 0.0, joinedSum = 0.0;
    for (const MassingBox& b : split)
        if (b.id.rfind("PL-S-", 0) == 0 && b.cx > 20.0 && b.cx < 22.5) splitSum += b.sy;
    for (const MassingBox& b : joined)
        if (b.id.rfind("PL-S-", 0) == 0 && b.cx > 20.0 && b.cx < 22.5) joinedSum += b.sy;
    CHECK(joinedSum == doctest::Approx(splitSum));
}

namespace {

// A wall's foot at x 20 with a cliff right past it: the ground is 10 at the foot and climbs 40 m in the first metre
// out (4 m in the first 0.1 m), then stands at 50. Under the building (x under 19.5) it lies at 4.
double cliffPastWall(double x, double) { return x < 19.5 ? 4.0 : (x < 20.5 ? 10.0 : 50.0); }

// The skirts among `made` that float, by the float check read every 0.1 m over the buildings and the skirts alone (no
// fill or stair to hold one up).
std::vector<std::string> floatingSkirts(const Field& f, const std::vector<MassingBox>& boxes, const std::vector<MassingBox>& made) {
    std::vector<MassingBox> all(boxes);
    for (const MassingBox& b : made)
        if (b.id.rfind("PL-S-", 0) == 0) all.push_back(b);
    const std::vector<bool> include(all.size(), true);
    FloatSpec spec;
    spec.sample = 0.1;
    FloatReport r;
    REQUIRE(findFloats(f.ground, all, include, spec, r));
    std::vector<std::string> ids;
    for (const FloatBox& fb : r.floating)
        if (all[fb.box].id.rfind("PL-S-", 0) == 0) ids.push_back(all[fb.box].id + " " + std::to_string(fb.gap));
    return ids;
}

} // namespace

TEST_CASE("a skirt reaches the lowest ground under it where the ground climbs steeply off the wall") {
    const Field f(cliffPastWall);
    const std::vector<MassingBox> boxes = {building("BLD", 5, 15, 20, 45, 3.5, 20.0)};
    std::vector<MassingBox> made;
    PlinthStats stats;
    REQUIRE(planPlinths(f.ground, boxes, {true}, kRect, {}, PlinthSpec{}, made, stats));
    bool east = false;
    for (const MassingBox& b : made) {
        if (b.id.rfind("PL-S-", 0) != 0 || b.cx <= 20.0 || b.cx >= 22.5) continue;
        east = true;
        // The lowest ground under it is the foot's 10 m (the 4 m lies under the building): a sink under that, no deeper.
        CHECK(b.z0 == doctest::Approx(10.0 - 0.3));
        CHECK(b.z1 == doctest::Approx(50.0));
    }
    CHECK(east);
    const std::vector<std::string> floating = floatingSkirts(f, boxes, made);
    CHECK_MESSAGE(floating.empty(), (floating.empty() ? std::string() : floating.front()));
}

TEST_CASE("a turned building's skirts and joined skirts reach the lowest ground under them") {
    SUBCASE("turned 30 degrees, the cliff past its +u wall") {
        const double a = 30.0 * 3.14159265358979323846 / 180.0;
        const Field f([a](double x, double y) {
            const double u = (x - 30.0) * std::cos(a) + (y - 30.0) * std::sin(a);
            return u > 6.0 ? 50.0 : 10.0;
        });
        MassingBox b = building("TURNED", 24, 26, 36, 34, 9.0, 20.0);
        b.rot = 30.0;
        const std::vector<MassingBox> boxes = {b};
        std::vector<MassingBox> made;
        PlinthStats stats;
        REQUIRE(planPlinths(f.ground, boxes, {true}, kRect, {}, PlinthSpec{}, made, stats));
        CHECK(stats.skirts > 0);
        for (const MassingBox& m : made)
            if (m.id.rfind("PL-S-", 0) == 0) CHECK(m.z0 >= 10.0 - 0.3 - 1e-9); // no ground node lies under 10
        const std::vector<std::string> floating = floatingSkirts(f, boxes, made);
        CHECK_MESSAGE(floating.empty(), (floating.empty() ? std::string() : floating.front()));
    }
    SUBCASE("two buildings in one line, their skirts joined and apart") {
        const Field f(cliffPastWall);
        const std::vector<MassingBox> boxes = {building("A", 5, 15, 20, 30, 3.5, 20.0), building("B", 5, 30, 20, 45, 3.5, 20.0)};
        for (const bool join : {true, false}) {
            PlinthSpec spec;
            spec.joinSkirts = join;
            std::vector<MassingBox> made;
            PlinthStats stats;
            REQUIRE(planPlinths(f.ground, boxes, {true, true}, kRect, {}, spec, made, stats));
            CHECK(stats.skirts > 0);
            const std::vector<std::string> floating = floatingSkirts(f, boxes, made);
            CHECK_MESSAGE(floating.empty(), (std::string(join ? "joined: " : "apart: ") + (floating.empty() ? std::string() : floating.front())));
        }
    }
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

TEST_CASE("a building raised over sloping ground floats; a foundation closes the gap") {
    // Ground falls 0.1 m per m along x from 12 at x 0; the building's base sits at 10.5 over x 10..20 (ground 11..10).
    const Field f([](double x, double) { return 12.0 - 0.1 * x; });
    std::vector<MassingBox> boxes{building("B", 10.0, 10.0, 20.0, 20.0, 10.5, 16.0)};
    // A lamp post 2 m up on the flat, a building sunk 0.5 m, and a deck out over the water elsewhere stay out of the list.
    boxes.push_back(building("LAMP", 30.0, 30.0, 30.3, 30.3, 11.0, 15.0));
    boxes.back().kind = "lamp";
    boxes.push_back(building("SUNK", 40.0, 40.0, 50.0, 50.0, 12.0 - 0.1 * 50.0 - 0.5, 15.0));
    const std::vector<bool> include(boxes.size(), true);
    FloatReport r;
    REQUIRE(findFloats(f.ground, boxes, include, FloatSpec{}, r));
    CHECK(r.checked == 3);
    REQUIRE(r.floating.size() == 2);
    CHECK(r.floating[0].box == 1); // the lamp: 11 over ground 9 (largest first)
    CHECK(r.floating[0].gap == doctest::Approx(2.0).epsilon(0.02));
    CHECK_FALSE(r.floating[0].building);
    CHECK(r.floating[1].box == 0);
    CHECK(r.floating[1].gap == doctest::Approx(0.5).epsilon(0.05)); // over ground 10 at x 20
    CHECK(r.floating[1].x == doctest::Approx(20.0).epsilon(0.01));
    CHECK(r.floating[1].building);
    CHECK(r.shallow == 2);
    CHECK(r.failing == 2); // the lamp over 0.9 m, the building over 0.3 m
    CHECK(r.crouch == 0);

    FoundationPlan plan;
    FoundationStats st;
    REQUIRE(planFoundations(f.ground, boxes, include, nullptr, FloatSpec{}, plan, st));
    CHECK(st.floating == 2);
    CHECK(st.left == 0);
    REQUIRE(st.lowered == 1);
    REQUIRE(plan.edits.size() == 1);
    CHECK(plan.edits[0].box == 1); // the lamp's base goes down under the ground (8.97 at x 30.3), its top kept
    CHECK(plan.edits[0].to.z0 == doctest::Approx(8.97 - 0.3).epsilon(0.01));
    CHECK(plan.edits[0].to.z1 == doctest::Approx(15.0));
    REQUIRE(st.skirts == 1);
    REQUIRE(plan.added.size() == 1);
    CHECK(plan.added[0].id == "PL-B-1");
    CHECK(plan.added[0].kind == "plinth");
    CHECK(plan.wears[0] == 0); // in the building's own surface
    CHECK(plan.added[0].z1 == doctest::Approx(10.5));
    CHECK(plan.added[0].z0 == doctest::Approx(10.0 - 0.3).epsilon(0.01));
    CHECK(plan.added[0].cx == doctest::Approx(15.0));
    REQUIRE(plan.rows.size() == 2);
    CHECK(plan.rows[0].fix == FloatFix::Lower);
    CHECK(plan.rows[1].fix == FloatFix::Skirt);

    // With the fixes in, nothing floats.
    std::vector<MassingBox> all;
    std::vector<bool> inc2;
    applyFoundations(boxes, include, plan, all, inc2);
    CHECK(all.size() == 4);
    FloatReport r2;
    REQUIRE(findFloats(f.ground, all, inc2, FloatSpec{}, r2));
    CHECK(r2.floating.empty());
    FloatOutcomes o;
    classifyFloats(r, plan, r2, o);
    CHECK(o.fixed == 2);
    CHECK(o.spanned + o.failing + o.newFloats == 0);
}

TEST_CASE("the float check skips water and boxes held up by others; a building far up is snapped, not skirted") {
    // Water (ground 0) for x under 20, 10 m ground beyond.
    const Field f([](double x, double) { return x < 20.0 ? 0.0 : 10.0; });
    std::vector<MassingBox> boxes{building("DECK", 5.0, 5.0, 15.0, 15.0, 1.0, 1.5)};
    boxes.push_back(building("BASE", 30.0, 30.0, 40.0, 40.0, 9.5, 14.0));
    boxes.push_back(building("TOP", 32.0, 32.0, 38.0, 38.0, 14.0, 20.0)); // stands on BASE
    boxes.push_back(building("HIGH", 45.0, 45.0, 55.0, 55.0, 16.0, 22.0)); // 6 m up: too deep to found
    const std::vector<bool> include(boxes.size(), true);
    FloatReport r;
    REQUIRE(findFloats(f.ground, boxes, include, FloatSpec{}, r));
    REQUIRE(r.floating.size() == 1);
    CHECK(r.floating[0].box == 3);
    CHECK(r.floating[0].failing);
    FoundationPlan plan;
    FoundationStats st;
    REQUIRE(planFoundations(f.ground, boxes, include, nullptr, FloatSpec{}, plan, st));
    // 6 m up: a podium, 1 m of its own surface and terrace stone under that, never a tall skirt or a move.
    CHECK(plan.edits.empty());
    CHECK(st.podiums == 1);
    REQUIRE(plan.added.size() == 2);
    CHECK(plan.added[0].id == "PL-P-1");
    CHECK(plan.wears[0] == 3);
    CHECK(plan.added[0].z0 == doctest::Approx(15.0));
    CHECK(plan.added[0].z1 == doctest::Approx(16.0));
    CHECK(plan.added[1].id == "PL-T-2");
    CHECK(plan.wears[1] == kFoundationTerrace);
    CHECK(plan.added[1].z0 == doctest::Approx(10.0 - 0.3));
    CHECK(plan.added[1].z1 == doctest::Approx(15.0));
    // A building 1.1 m up is a crouch-under; it is founded.
    std::vector<MassingBox> one{building("LOW", 30.0, 30.0, 40.0, 40.0, 11.1, 16.0)};
    FloatReport r3;
    REQUIRE(findFloats(f.ground, one, std::vector<bool>(1, true), FloatSpec{}, r3));
    REQUIRE(r3.floating.size() == 1);
    CHECK(r3.floating[0].crouch);
    CHECK(r3.crouch == 1);
    FloatSpec bad;
    bad.sample = 0.0;
    CHECK_FALSE(findFloats(f.ground, boxes, include, bad, r));
}

namespace {

MassingBox piece(const char* id, const char* kind, double x0, double y0, double x1, double y1, double z0, double z1) {
    MassingBox b = building(id, x0, y0, x1, y1, z0, z1);
    b.kind = kind;
    return b;
}

FloatSpec spanSpec(double eave = 1.5) {
    FloatSpec s;
    s.spanRule = true;
    s.maxEave = eave;
    return s;
}

FloatReport floats(const Field& f, const std::vector<MassingBox>& boxes, const FloatSpec& spec) {
    FloatReport r;
    REQUIRE(findFloats(f.ground, boxes, std::vector<bool>(boxes.size(), true), spec, r));
    return r;
}

} // namespace

TEST_CASE("span rule: a deck on two piers passes; a span over 30 m, a long tail, a short pier or a floating pier fails") {
    const Field f([](double, double) { return 10.0; });
    // A 30 m deck 5 m up on 2 m piers at its ends: 26 m clear between them.
    std::vector<MassingBox> boxes{piece("D", "deck", 10, 20, 40, 24, 15.0, 15.5), piece("P1", "pier", 10, 20, 12, 24, 9.7, 15.0),
                                  piece("P2", "pier", 38, 20, 40, 24, 9.7, 15.0)};
    FloatReport plain = floats(f, boxes, FloatSpec{});
    REQUIRE(plain.floating.size() == 1); // held everywhere, as before: the deck floats 5 m
    CHECK(plain.floating[0].gap == doctest::Approx(5.0));
    FloatReport r = floats(f, boxes, spanSpec());
    CHECK(r.floating.empty());
    REQUIRE(r.spanned.size() == 1);
    CHECK(r.spanned[0].box == 0);
    CHECK(r.spanned[0].supporters == 2);
    CHECK(r.spanned[0].span == doctest::Approx(26.0).epsilon(0.05));
    CHECK(r.spanned[0].overhang == doctest::Approx(0.0));

    SUBCASE("30 m clear passes, 32 m fails") {
        boxes = {piece("D", "deck", 10, 20, 42, 24, 15.0, 15.5), piece("P1", "pier", 10, 20, 11, 24, 9.7, 15.0),
                 piece("P2", "pier", 41, 20, 42, 24, 9.7, 15.0)};
        CHECK(floats(f, boxes, spanSpec()).spanned.size() == 1);
        boxes = {piece("D", "deck", 10, 20, 44, 24, 15.0, 15.5), piece("P1", "pier", 10, 20, 11, 24, 9.7, 15.0),
                 piece("P2", "pier", 43, 20, 44, 24, 9.7, 15.0)};
        r = floats(f, boxes, spanSpec());
        REQUIRE(r.floating.size() == 1);
        CHECK(r.floating[0].span > 30.0);
    }
    SUBCASE("a tail past the outer pier: 1.4 m passes, 5 m fails") {
        boxes[0] = piece("D", "deck", 10, 20, 41.4, 24, 15.0, 15.5);
        r = floats(f, boxes, spanSpec());
        REQUIRE(r.spanned.size() == 1);
        CHECK(r.spanned[0].overhang == doctest::Approx(1.4).epsilon(0.05));
        boxes[0] = piece("D", "deck", 10, 20, 45, 24, 15.0, 15.5);
        r = floats(f, boxes, spanSpec());
        REQUIRE(r.floating.size() == 1);
        CHECK(r.floating[0].overhang == doctest::Approx(5.0).epsilon(0.05));
        CHECK(r.floating[0].x > 41.5); // its worst point is out on the tail
    }
    SUBCASE("a deck's own eave is 3.5 m; a gate or bridge reaching 3.5 m still fails at the 1.5 m eave") {
        // A wing 3.4 m out sideways past the piers (y 24..27.4), as a viaduct deck's.
        boxes[0] = piece("D", "deck", 10, 20, 40, 27.4, 15.0, 15.5);
        r = floats(f, boxes, spanSpec());
        REQUIRE(r.spanned.size() == 1);
        CHECK(r.spanned[0].overhang == doctest::Approx(3.4).epsilon(0.05));
        boxes[0] = piece("D", "deck", 10, 20, 40, 28.0, 15.0, 15.5); // 4 m out: past the deck's eave
        r = floats(f, boxes, spanSpec());
        REQUIRE(r.floating.size() == 1);
        CHECK(r.floating[0].overhang == doctest::Approx(4.0).epsilon(0.05));
        for (const char* kind : {"gate", "bridge"}) {
            boxes[0] = piece("G", kind, 10, 20, 40, 27.5, 15.0, 15.5); // 3.5 m out
            r = floats(f, boxes, spanSpec());
            REQUIRE(r.floating.size() == 1);
            CHECK(r.floating[0].overhang == doctest::Approx(3.5).epsilon(0.05));
        }
        FloatSpec none = spanSpec();
        none.kindEaves.clear(); // without the deck's own eave, the 3.4 m wing fails too
        boxes[0] = piece("D", "deck", 10, 20, 40, 27.4, 15.0, 15.5);
        CHECK(floats(f, boxes, none).floating.size() == 1);
        // The own eave holds only up to the kind's maxDepth (12 m): an 11.5 m rise keeps it, a 13 m rise is judged at 1.5.
        boxes[0] = piece("D", "deck", 10, 20, 40, 27.4, 15.0, 26.5);
        CHECK(floats(f, boxes, spanSpec()).spanned.size() == 1);
        boxes[0] = piece("D", "deck", 10, 20, 40, 27.4, 15.0, 28.0);
        r = floats(f, boxes, spanSpec());
        REQUIRE(r.floating.size() == 1);
        CHECK(r.floating[0].overhang == doctest::Approx(3.4).epsilon(0.05));
    }
    SUBCASE("a pier short of the base holds nothing; the fix pass raises it to the base") {
        boxes[2].z1 = 14.5;
        r = floats(f, boxes, spanSpec());
        REQUIRE(r.floating.size() == 1);
        CHECK(r.floating[0].supporters == 1);
        FoundationPlan plan;
        FoundationStats st;
        REQUIRE(planFoundations(f.ground, boxes, std::vector<bool>(3, true), nullptr, FloatSpec{}, plan, st));
        REQUIRE(plan.rows.size() == 1);
        CHECK(plan.rows[0].fix == FloatFix::RaisePiers);
        REQUIRE(plan.edits.size() == 1);
        CHECK(plan.edits[0].box == 2);
        CHECK(plan.edits[0].to.z1 == doctest::Approx(15.0));
        std::vector<MassingBox> all;
        std::vector<bool> inc;
        applyFoundations(boxes, std::vector<bool>(3, true), plan, all, inc);
        CHECK(floats(f, all, spanSpec()).spanned.size() == 1);
    }
    SUBCASE("a pier that floats itself holds nothing (no chains); lowered, it holds") {
        boxes[1].z0 = 10.5;
        boxes[2].z0 = 10.5;
        r = floats(f, boxes, spanSpec());
        CHECK(r.floating.size() == 3);
        CHECK(r.passes == 3); // the piers drop out, then the deck they held, then nothing changes
        FloatSpec noChain = spanSpec();
        noChain.chainCheck = false;
        CHECK(floats(f, boxes, noChain).floating.size() == 2);
        FoundationPlan plan;
        FoundationStats st;
        REQUIRE(planFoundations(f.ground, boxes, std::vector<bool>(3, true), nullptr, FloatSpec{}, plan, st));
        CHECK(st.lowered == 2); // the piers; the deck is spanned once they hold
        REQUIRE(plan.rows.size() == 2);
        std::vector<MassingBox> all;
        std::vector<bool> inc;
        applyFoundations(boxes, std::vector<bool>(3, true), plan, all, inc);
        const FloatReport after = floats(f, all, spanSpec());
        CHECK(after.floating.empty());
        FloatOutcomes o;
        classifyFloats(floats(f, boxes, FloatSpec{}), plan, after, o);
        CHECK(o.rows.size() == 3);
        CHECK(o.fixed == 2);
        CHECK(o.spanned == 1);
        CHECK(o.failing == 0);
    }
    SUBCASE("a wide deck on narrow piers: 3.5 m out sideways passes on the deck's own eave, fails at 1.5 m without it") {
        boxes = {piece("D", "deck", 10, 20, 40, 30, 15.0, 15.5), piece("P1", "pier", 10, 23.5, 13, 26.5, 9.7, 15.0),
                 piece("P2", "pier", 37, 23.5, 40, 26.5, 9.7, 15.0)};
        CHECK(floats(f, boxes, spanSpec()).spanned.size() == 1);
        FloatSpec noKind = spanSpec();
        noKind.kindEaves.clear();
        r = floats(f, boxes, noKind);
        REQUIRE(r.floating.size() == 1);
        CHECK(r.floating[0].overhang == doctest::Approx(3.5).epsilon(0.05));
        CHECK(floats(f, boxes, spanSpec(3.5)).spanned.size() == 1);
        FoundationPlan plan;
        FoundationStats st;
        FloatSpec planSpec;
        planSpec.kindEaves.clear();
        REQUIRE(planFoundations(f.ground, boxes, std::vector<bool>(3, true), nullptr, planSpec, plan, st));
        REQUIRE(plan.rows.size() == 1);
        CHECK(plan.rows[0].fix == FloatFix::Left); // no short pier to raise: left, by name
        CHECK(st.left == 1);
    }
    SUBCASE("a wall is never spanned: held everywhere, or lowered") {
        boxes[0].kind = "wall";
        CHECK(floats(f, boxes, spanSpec()).floating.size() == 1);
    }
    SUBCASE("a pier under two short decks goes up to the higher base, and counts once") {
        boxes = {piece("A", "deck", 10, 20, 30, 24, 15.0, 15.5), piece("B", "deck", 10, 24, 30, 28, 16.0, 16.5),
                 piece("P", "pier", 10, 20, 12, 28, 9.7, 14.0)};
        FoundationPlan plan;
        FoundationStats st;
        REQUIRE(planFoundations(f.ground, boxes, std::vector<bool>(3, true), nullptr, FloatSpec{}, plan, st));
        REQUIRE(plan.edits.size() == 1);
        CHECK(plan.edits[0].box == 2);
        CHECK(plan.edits[0].to.z1 == doctest::Approx(16.0));
        CHECK(st.piersRaised == 1);
    }
}

TEST_CASE("span rule: a deck resting on its bank at one end is held there, like a pier") {
    // A bank 5 m up west of x = 14; the deck lies on it, then crosses to a pier at x 38..40.
    const Field f([](double x, double) { return x < 14.0 ? 15.0 : 10.0; });
    std::vector<MassingBox> boxes{piece("D", "deck", 10, 20, 40, 24, 15.0, 15.5), piece("P", "pier", 38, 20, 40, 24, 9.7, 15.0)};
    const FloatReport r = floats(f, boxes, spanSpec());
    CHECK(r.floating.empty());
    REQUIRE(r.spanned.size() == 1);
    CHECK(r.spanned[0].span < 30.0);
    CHECK(r.spanned[0].supporters == 1);
    boxes.pop_back(); // no pier: the far end hangs 26 m past the bank
    REQUIRE(floats(f, boxes, spanSpec()).floating.size() == 1);
}

TEST_CASE("fix pass by kind: a marking snaps, a mast drops onto its slab, a bridge gets end posts, a kit-fitted post an under-box") {
    const Field f([](double, double) { return 10.0; });
    std::vector<MassingBox> boxes{piece("M", "marking", 5, 5, 9, 6, 10.2, 10.8), piece("SL", "slab", 20, 20, 30, 30, 9.7, 12.0),
                                  piece("MA", "mast", 24, 24, 25, 25, 20.0, 30.0), piece("BR", "bridge", 30, 40, 54, 43, 16.0, 17.0),
                                  piece("PO", "post", 50, 5, 50.4, 5.4, 11.0, 16.0)};
    const std::vector<bool> inc(boxes.size(), true);
    std::vector<bool> keep(boxes.size(), false);
    keep[4] = true;
    FoundationPlan plan;
    FoundationStats st;
    REQUIRE(planFoundations(f.ground, boxes, inc, &keep, FloatSpec{}, plan, st));
    CHECK(st.snapped == 1);
    CHECK(st.dropped == 1);
    CHECK(st.endPosts == 1);
    CHECK(st.underBoxes == 1);
    std::vector<MassingBox> all;
    std::vector<bool> inc2;
    applyFoundations(boxes, inc, plan, all, inc2);
    CHECK(all[0].z0 == doctest::Approx(10.0)); // flat paint on the ground, as thick as it was
    CHECK(all[0].z1 - all[0].z0 == doctest::Approx(0.6));
    CHECK(all[2].z0 == doctest::Approx(12.0)); // down 8 m onto the slab
    CHECK(all[2].z1 == doctest::Approx(22.0));
    CHECK(all[4].z0 == doctest::Approx(11.0)); // the post's shape kept
    REQUIRE(plan.added.size() == 3);
    CHECK(plan.added[0].id == "PL-E-1");
    CHECK(plan.added[0].z1 == doctest::Approx(16.0));
    CHECK(plan.wears[0] == 3);
    CHECK(plan.added[2].id == "PL-U-3");
    CHECK(plan.added[2].z1 == doctest::Approx(11.0));
    const FloatReport after = floats(f, all, spanSpec());
    CHECK(after.floating.empty());
    REQUIRE(after.spanned.size() == 1); // the bridge, 22 m clear between its posts
    CHECK(after.spanned[0].box == 3);
    CHECK(std::string(floatFixName(FloatFix::EndPosts)) == "end-posts");
}

namespace {

std::size_t countPrefix(const FoundationPlan& plan, const char* prefix) {
    return static_cast<std::size_t>(std::count_if(plan.added.begin(), plan.added.end(),
                                                  [prefix](const MassingBox& b) { return b.id.rfind(prefix, 0) == 0; }));
}

// The fix pass and the check after it, as ftcore_floatcheck runs them (no plinths).
FloatOutcomes fixAndCheck(const Field& f, const std::vector<MassingBox>& boxes, const FloatSpec& spec, FoundationPlan& plan,
                          FoundationStats& st, std::vector<MassingBox>& all) {
    const std::vector<bool> inc(boxes.size(), true);
    FloatReport before;
    REQUIRE(findFloats(f.ground, boxes, inc, FloatSpec{}, before));
    REQUIRE(planFoundations(f.ground, boxes, inc, nullptr, spec, plan, st));
    std::vector<bool> inc2;
    applyFoundations(boxes, inc, plan, all, inc2);
    FloatSpec as = spec;
    as.spanRule = true;
    as.chainCheck = true;
    FloatReport after;
    REQUIRE(findFloats(f.ground, all, inc2, as, after));
    FloatOutcomes o;
    classifyFloats(before, plan, after, o);
    return o;
}

} // namespace

TEST_CASE("deck posts: off, a deck with no pier to raise is left as before; on, it gets posts at most 6 m apart, braces, and passes") {
    const Field f([](double, double) { return 10.0; });
    // A 29.29 x 10 m deck 8 m up on one pier at its west end: its east end hangs 26 m past the pier.
    const std::vector<MassingBox> boxes{piece("D", "deck", 10, 20, 39.29, 30, 18.0, 18.5), piece("P", "pier", 10, 23, 13, 27, 9.7, 18.0)};
    FoundationPlan plan;
    FoundationStats st;
    std::vector<MassingBox> all;
    FloatOutcomes o = fixAndCheck(f, boxes, FloatSpec{}, plan, st, all);
    REQUIRE(plan.rows.size() == 1);
    CHECK(plan.rows[0].fix == FloatFix::Left);
    CHECK(st.left == 1);
    CHECK(st.posted == 0);
    CHECK(countPrefix(plan, "PL-D-") == 0);
    CHECK(countPrefix(plan, "PL-K-") == 0);
    CHECK(o.failing == 1);

    FloatSpec on;
    on.deckPosts = true;
    o = fixAndCheck(f, boxes, on, plan, st, all);
    REQUIRE(plan.rows.size() == 1);
    CHECK(plan.rows[0].fix == FloatFix::Posts);
    CHECK(std::string(floatFixName(FloatFix::Posts)) == "posts");
    CHECK(st.left == 0);
    CHECK(st.posted == 1);
    // Six bents 5.66 m apart along it, two rows 1.5 m off its middle (its 3.5 m eave less half a metre in from each side);
    // the west bent stands on the pier, so it is left out.
    CHECK(st.posts == 10);
    CHECK(countPrefix(plan, "PL-D-") == 10);
    std::vector<double> xs;
    for (std::size_t a = 0; a < plan.added.size(); ++a) {
        const MassingBox& b = plan.added[a];
        CHECK(plan.wears[a] == 0); // the deck's surface
        if (b.id.rfind("PL-D-", 0) != 0) continue;
        CHECK_FALSE(b.sloped);
        CHECK(b.sx == doctest::Approx(1.0));
        CHECK(b.z0 == doctest::Approx(9.7)); // down to sink under the ground
        CHECK(b.z1 == doctest::Approx(18.0)); // up to the deck's underside
        CHECK(std::fabs(std::fabs(b.cy - 25.0) - 1.5) < 1e-6);
        xs.push_back(b.cx);
    }
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end(), [](double a, double c) { return std::fabs(a - c) < 1e-6; }), xs.end());
    REQUIRE(xs.size() == 5);
    for (std::size_t k = 1; k < xs.size(); ++k) CHECK(xs[k] - xs[k - 1] <= 6.0);
    CHECK(xs.back() == doctest::Approx(38.79)); // flush with the east end
    // Braces: 4 bays along each row (one panel each, 7.1 m clear over 5.66 m), 5 bents across (two panels over 3 m).
    CHECK(st.braces == 18);
    for (const MassingBox& b : plan.added) {
        if (b.id.rfind("PL-K-", 0) != 0) continue;
        CHECK(b.kind == "brace");
        CHECK(b.z1 - b.z0 == doctest::Approx(0.3));
        const double deg = std::atan(std::fabs(b.riseU)) * 180.0 / 3.14159265358979;
        CHECK(deg > 30.0);
        CHECK(deg < 60.0);
    }
    CHECK(o.failing == 0);
    CHECK(o.spanned == 1);
    CHECK(o.newFloats == 0);
}

TEST_CASE("deck posts: a ramp's posts slope with its underside and its braces stay near 45 degrees") {
    const Field f([](double, double) { return 4.0; });
    // A ramp 13 -> 47.88 m over 29.29 m, 0.5 m thick, on a pier at its foot (as a deck 35 m deep: the 1.5 m eave).
    MassingBox ramp = piece("R", "deck", 10, 20, 39.29, 30, 12.5, 47.88);
    ramp.sloped = true;
    ramp.slopeFrom = 13.0;
    ramp.slopeTo = 47.88;
    ramp.slopeThick = 0.5;
    const std::vector<MassingBox> boxes{ramp, piece("P", "pier", 9, 23, 12, 27, 3.7, 12.5)};
    FoundationPlan plan;
    FoundationStats st;
    std::vector<MassingBox> all;
    FloatSpec on;
    on.deckPosts = true;
    const FloatOutcomes o = fixAndCheck(f, boxes, on, plan, st, all);
    CHECK(st.posted == 1);
    // Three rows (1 m in from each side: its 1.5 m eave less half a metre), 3.5 m apart.
    std::vector<double> ys;
    const double grade = (47.88 - 13.0) / 29.29;
    for (const MassingBox& b : plan.added) {
        if (b.id.rfind("PL-D-", 0) != 0) continue;
        REQUIRE(b.sloped);
        CHECK(b.slopeTo - b.slopeFrom == doctest::Approx(grade).epsilon(0.01));
        CHECK(0.5 * (b.slopeFrom + b.slopeTo) == doctest::Approx(12.5 + grade * (b.cx - 10.0)).epsilon(0.001));
        CHECK(b.z0 == doctest::Approx(3.7));
        ys.push_back(b.cy);
    }
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end(), [](double a, double c) { return std::fabs(a - c) < 1e-6; }), ys.end());
    REQUIRE(ys.size() == 3);
    CHECK(ys.front() == doctest::Approx(21.5));
    CHECK(ys.back() == doctest::Approx(28.5));
    CHECK(st.braces > 0);
    for (const MassingBox& b : plan.added) {
        if (b.id.rfind("PL-K-", 0) != 0) continue;
        const double deg = std::atan(std::fabs(b.riseU)) * 180.0 / 3.14159265358979;
        CHECK(deg > 9.0);
        CHECK(deg < 57.0);
    }
    CHECK(o.failing == 0);
    CHECK(o.newFloats == 0);
}

TEST_CASE("deck posts: a brace is spanned between the two posts it sits in, and floats with one end free") {
    const Field f([](double, double) { return 10.0; });
    MassingBox k = piece("K", "brace", 10.5, 20.35, 16.5, 20.65, 13.35, 13.65);
    k.riseU = 5.0 / 6.0; // 11 m at the west post up to 16 m at the east one
    std::vector<MassingBox> boxes{piece("A", "plinth", 10, 20, 11, 21, 9.7, 18.0), piece("B", "plinth", 16, 20, 17, 21, 9.7, 18.0), k};
    FloatReport r = floats(f, boxes, spanSpec());
    CHECK(r.floating.empty());
    REQUIRE(r.spanned.size() == 1);
    CHECK(r.spanned[0].box == 2);
    CHECK(r.spanned[0].supporters == 2);
    CHECK(floats(f, boxes, FloatSpec{}).floating.size() == 1); // held everywhere without the span rule
    boxes.erase(boxes.begin() + 1);
    r = floats(f, boxes, spanSpec());
    REQUIRE(r.floating.size() == 1);
    CHECK(r.floating[0].box == 1);
}
