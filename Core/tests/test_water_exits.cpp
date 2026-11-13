#include "ftcore/WaterBodies.h"

#include "ftcore/Roads.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

RoadCanal canalOf(const char* id, double x0, double y0, double x1, double y1, double bridgeAt) {
    RoadCanal c;
    c.id = id;
    c.x0 = x0;
    c.y0 = y0;
    c.x1 = x1;
    c.y1 = y1;
    c.bridgeAt = bridgeAt;
    return c;
}

// Flat ground at 4 m over the town's canals (a sample every 50 m from (700, 350)): the banks' tops.
struct Banks {
    std::vector<std::uint16_t> grid = std::vector<std::uint16_t>(8u * 12u, static_cast<std::uint16_t>(32768 + 4 * 128));
    MassingGround ground() const {
        MassingGround g;
        g.grid = grid.data();
        g.samplesX = 8;
        g.samplesY = 12;
        g.spacing = 50.0;
        g.originX = 700.0;
        g.originY = 350.0;
        return g;
    }
};

// The top of the canal's built floor under a plan point: the highest canal_floor piece that covers it (a tilted
// piece's by its rise). The pieces are axis-aligned.
double builtFloor(const RoadBuild& build, double x, double y) {
    double top = -1e9;
    for (const RoadSolid& s : build.solids) {
        const MassingBox& b = s.box;
        if (b.kind != "canal_floor") continue;
        const double u = x - b.cx, w = y - b.cy;
        if (std::fabs(u) > 0.5 * b.sx || std::fabs(w) > 0.5 * b.sy) continue;
        top = std::max(top, b.z1 + b.riseU * u + b.riseW * w);
    }
    return top;
}

// The furthest any point of the canal's water is from its nearest exit's foot (a half-metre grid).
float furthestFromExit(const RoadBuild& build, const WaterBodies& water) {
    float worst = 0.f;
    for (const RoadWater& w : build.waters)
        for (double x = w.cx - 0.5 * w.sx; x <= w.cx + 0.5 * w.sx + 1e-9; x += 0.5)
            for (double y = w.cy - 0.5 * w.sy; y <= w.cy + 0.5 * w.sy + 1e-9; y += 0.5) {
                const Vec2 p{static_cast<float>(x), static_cast<float>(y)};
                const int e = water.nearestExit(p);
                REQUIRE(e >= 0);
                const WaterExit& exit = water.exit(static_cast<std::size_t>(e));
                worst = std::max(worst, std::hypot(exit.foot.x - p.x, exit.foot.y - p.y));
            }
    return worst;
}

} // namespace

TEST_CASE("water section: a shelf at the wall, a slope, a deep middle; flat without a batter") {
    CHECK(waterSectionDepth(0.f, 1.f, 0.6f, 1.5f, 2.f) == doctest::Approx(1.f));
    CHECK(waterSectionDepth(0.6f, 1.f, 0.6f, 1.5f, 2.f) == doctest::Approx(1.f));
    CHECK(waterSectionDepth(1.35f, 1.f, 0.6f, 1.5f, 2.f) == doctest::Approx(1.5f));
    CHECK(waterSectionDepth(2.1f, 1.f, 0.6f, 1.5f, 2.f) == doctest::Approx(2.f));
    CHECK(waterSectionDepth(3.5f, 1.f, 0.6f, 1.5f, 2.f) == doctest::Approx(2.f));
    CHECK(waterSectionDepth(3.5f, 1.f, 0.6f, 0.f, 2.f) == doctest::Approx(1.f));
    CHECK(waterSectionDepth(3.5f, 1.f, 0.6f, 1.5f, 0.8f) == doctest::Approx(1.f));
    // The shelf is a deep wade, the middle a swim.
    CHECK(waterSectionDepth(0.3f, 1.f, 0.6f, 1.5f, 2.f) <= kWaterSwimDepth);
    CHECK(waterSectionDepth(3.5f, 1.f, 0.6f, 1.5f, 2.f) > kWaterSwimDepth);
}

TEST_CASE("canal section: the built floor and the body's depth agree across the channel") {
    // North-south (9 m between the banks' backs, 7 m of water) and east-west (a pool 26 m wide).
    for (const RoadCanal& canal : {canalOf("CN-N", 892.0, 664.0, 901.0, 814.0, 700.0), canalOf("CN-P", 744.0, 786.0, 892.0, 814.0, 830.0)}) {
        CAPTURE(canal.id);
        const RoadCanalSection section = roadCanalSection(canal);
        CHECK(section.depth == doctest::Approx(1.0));
        CHECK(section.deep == doctest::Approx(2.0));
        CHECK(section.shelf == doctest::Approx(0.6));
        CHECK(section.batter == doctest::Approx(1.5));
        RoadBuild build;
        REQUIRE(roadCanalBuild(canal, MassingGround{}, build) > 0);
        REQUIRE(build.waters.size() == 1);
        const RoadWater& w = build.waters[0];
        CHECK(w.depth == doctest::Approx(1.0));
        CHECK(w.deep == doctest::Approx(2.0));
        WaterBodies water;
        REQUIRE(water.add(waterBodyFromCanal(w)));
        CHECK(water.body(0).bed == doctest::Approx(0.5f));
        CHECK(water.body(0).shelfBed == doctest::Approx(1.5f));
        const bool alongY = w.sy >= w.sx;
        const double across = alongY ? w.sx : w.sy;
        int checked = 0;
        for (double d = 0.05; d < across; d += 0.1) {
            const double x = alongY ? w.cx - 0.5 * w.sx + d : w.cx + 3.0;
            const double y = alongY ? w.cy + 3.0 : w.cy - 0.5 * w.sy + d;
            const Vec2 p{static_cast<float>(x), static_cast<float>(y)};
            const double floor = builtFloor(build, x, y);
            REQUIRE(floor > -1e8);
            CHECK(water.depth(p) == doctest::Approx(w.z - floor).epsilon(0.01));
            // A body with no ground given stands in the section's column.
            WaterCapsule c;
            c.p = p;
            c.feetZ = static_cast<float>(w.z) - kWaterSwimDraft;
            const WaterState s = waterState(water, c, 0.f);
            CHECK(s.column == doctest::Approx(water.depth(p)));
            ++checked;
        }
        CHECK(checked > 60);
        // At the wall a step in is a wade (deep), the middle is a swim.
        const Vec2 wall{static_cast<float>(alongY ? w.cx - 0.5 * w.sx + 0.3 : w.cx), static_cast<float>(alongY ? w.cy : w.cy - 0.5 * w.sy + 0.3)};
        const Vec2 mid{static_cast<float>(w.cx), static_cast<float>(w.cy)};
        CHECK(water.depth(wall) == doctest::Approx(1.f));
        CHECK(water.depth(mid) == doctest::Approx(2.f));
        WaterCapsule in;
        in.p = wall;
        in.feetZ = 1.5f;
        CHECK(waterState(water, in, 0.f).mode == WaterMode::DeepWade);
        in.p = mid;
        in.feetZ = 0.5f;
        CHECK(waterState(water, in, 0.f).mode == WaterMode::Swim);
        // The slope is one the lead walks (under 40 degrees).
        CHECK((section.deep - section.depth) / section.batter < 0.84);
    }
    // Too narrow for a shelf and a slope each side, or no deeper asked for: the flat floor as before.
    RoadCanal narrow = canalOf("CN-S", 0.0, 0.0, 6.0, 60.0, 0.0);
    CHECK(roadCanalSection(narrow).deep == doctest::Approx(1.0));
    CHECK(roadCanalSection(narrow).batter == doctest::Approx(0.0));
    RoadCanal flat = canalOf("CN-F", 0.0, 0.0, 9.0, 60.0, 0.0);
    flat.deep = 0.0;
    CHECK(roadCanalSection(flat).deep == doctest::Approx(1.0));
    RoadBuild b;
    REQUIRE(roadCanalBuild(flat, MassingGround{}, b) > 0);
    WaterBodies w2;
    REQUIRE(w2.add(waterBodyFromCanal(b.waters[0])));
    CHECK(w2.depth({4.5f, 30.f}) == doctest::Approx(1.f));
    CHECK(w2.depth({1.2f, 30.f}) == doctest::Approx(1.f));
}

TEST_CASE("canal exits: ladders and steps in turn on both walls, never more than about 20 m of swimming away") {
    const RoadCanal canals[] = {canalOf("CN-01", 892.0, 424.0, 901.0, 636.0, 560.0), canalOf("CN-02", 892.0, 664.0, 901.0, 814.0, 700.0),
                                canalOf("CN-03", 744.0, 786.0, 892.0, 814.0, 830.0)};
    const Banks banks;
    for (const RoadCanal& canal : canals) {
        CAPTURE(canal.id);
        RoadBuild build;
        REQUIRE(roadCanalBuild(canal, banks.ground(), build) > 0);
        REQUIRE(build.exits.size() >= 8);
        WaterBodies water;
        REQUIRE(water.add(waterBodyFromCanal(build.waters[0])));
        std::size_t ladders = 0, steps = 0;
        for (const RoadWaterExit& e : build.exits) {
            CHECK(e.canal == canal.id);
            (e.kind == RoadExitKind::Ladder ? ladders : steps) += 1;
            const WaterExit x = waterExitFromRoad(e);
            // The foot is in the water, the top on the bank outside it and behind the foot along the way out.
            CHECK(water.body(0).covers(x.foot));
            CHECK_FALSE(water.body(0).covers(x.top));
            CHECK(std::hypot(x.out.x, x.out.y) == doctest::Approx(1.f));
            if (e.kind == RoadExitKind::Ladder) CHECK((x.top.x - x.foot.x) * x.out.x + (x.top.y - x.foot.y) * x.out.y > 1.f);
            // The rail's gap is a stretch of the wall's edge at least a metre long.
            CHECK(std::hypot(e.gapX1 - e.gapX0, e.gapY1 - e.gapY0) >= 1.0);
            REQUIRE(water.addExit(x));
            // Clear of the footbridge.
            const bool alongY = build.waters[0].alongY;
            if (e.outX * (alongY ? 1.0 : 0.0) + e.outY * (alongY ? 0.0 : 1.0) != 0.0)
                CHECK(std::fabs((alongY ? 0.5 * (e.gapY0 + e.gapY1) : 0.5 * (e.gapX0 + e.gapX1)) - canal.bridgeAt) > 1.5);
        }
        CHECK(ladders >= 4);
        CHECK(steps >= 4);
        CHECK(furthestFromExit(build, water) <= 21.f);
        // Built pieces: rungs in the wall for each ladder, treads for each flight, the treads walked on.
        std::size_t rungs = 0, treads = 0;
        for (const RoadSolid& s : build.solids) {
            rungs += s.box.kind == "canal_ladder" ? 1u : 0u;
            if (s.box.kind == "canal_steps") {
                ++treads;
                CHECK(s.collide);
                CHECK(s.box.z1 > canal.floorZ);
            }
        }
        CHECK(rungs >= ladders * 12);
        CHECK(treads >= steps * 8);
    }
    // The north run's second way out on the west wall is a flight of steps, on the east wall a ladder, by y 720.5.
    RoadBuild build;
    REQUIRE(roadCanalBuild(canals[1], banks.ground(), build) > 0);
    bool westSteps = false, eastLadder = false;
    for (const RoadWaterExit& e : build.exits) {
        const double y = 0.5 * (e.gapY0 + e.gapY1);
        westSteps = westSteps || (e.kind == RoadExitKind::Steps && e.outX < 0.0 && std::fabs(e.gapX0 - 893.0) < 1e-6 && y > 716.0 && y < 723.0);
        eastLadder = eastLadder || (e.kind == RoadExitKind::Ladder && e.outX > 0.0 && std::fabs(y - 720.5) < 1e-6);
    }
    CHECK(westSteps);
    CHECK(eastLadder);
    // exitEvery 0: none.
    RoadCanal none = canals[1];
    none.exitEvery = 0.0;
    RoadBuild b2;
    REQUIRE(roadCanalBuild(none, banks.ground(), b2) > 0);
    CHECK(b2.exits.empty());
    // No bank over the water (no ground given: its top reads 0): no way out is built.
    RoadBuild b3;
    REQUIRE(roadCanalBuild(canals[1], MassingGround{}, b3) > 0);
    CHECK(b3.exits.empty());
    const std::string csv = roadExitCsv(build.exits);
    CHECK(csv.find("CN-02-EXIT-01,CN-02,ladder,") != std::string::npos);
}

TEST_CASE("canal exits: under a road's deck a flight gives way to a ladder, and no ladder is set under one") {
    const Banks banks;
    const RoadCanal canal = canalOf("CN-02", 892.0, 664.0, 901.0, 814.0, 700.0);
    RoadBuild open;
    REQUIRE(roadCanalBuild(canal, banks.ground(), open) > 0);
    // The west wall's second way out is a flight about y 720.5 (718.3 to 722.7), its first a ladder at y 683.5.
    auto deck = [](double y0, double y1) {
        RoadSolid s;
        s.box.id = "DECK";
        s.box.kind = "asphalt";
        s.box.cx = 896.5;
        s.box.cy = 0.5 * (y0 + y1);
        s.box.sx = 14.0;
        s.box.sy = y1 - y0;
        s.box.z0 = 4.0;
        s.box.z1 = 4.4;
        s.collide = true;
        return s;
    };
    RoadBuild roofedOver;
    roofedOver.solids.push_back(deck(721.5, 727.0)); // over the flight's lower end
    roofedOver.solids.push_back(deck(680.0, 687.0)); // over the first ladder
    REQUIRE(roadCanalBuild(canal, banks.ground(), roofedOver) > 0);
    // The ladder under the deck is gone on both walls' sides it covers; the flight is a ladder now.
    CHECK(roofedOver.exits.size() < open.exits.size());
    bool flight = false, ladderThere = false, ladderUnder = false;
    for (const RoadWaterExit& e : open.exits) flight = flight || (e.kind == RoadExitKind::Steps && e.outX < 0.0 && std::fabs(e.topY - 718.7) < 1.0);
    for (const RoadWaterExit& e : roofedOver.exits) {
        ladderThere = ladderThere || (e.kind == RoadExitKind::Ladder && e.outX < 0.0 && std::fabs(e.footY - 720.5) < 1e-6);
        ladderUnder = ladderUnder || (e.footY > 680.0 && e.footY < 687.0);
        CHECK_FALSE((e.kind == RoadExitKind::Steps && e.outX < 0.0 && std::fabs(e.topY - 718.7) < 1.0));
    }
    CHECK(flight);
    CHECK(ladderThere);
    CHECK_FALSE(ladderUnder);
}

TEST_CASE("water exits: the nearest within reach, the frame turned over, a full set and bad exits refused") {
    WaterBodies water;
    WaterExit a;
    a.foot = {10.f, 5.f};
    a.top = {8.5f, 5.f};
    a.topZ = 4.f;
    a.out = {-2.f, 0.f};
    WaterExit b = a;
    b.kind = WaterExitKind::Steps;
    b.foot = {10.f, 45.f};
    b.top = {8.5f, 45.f};
    CHECK(water.nearestExit({10.f, 5.f}) == -1);
    REQUIRE(water.addExit(a));
    REQUIRE(water.addExit(b));
    CHECK(water.exit(0).out.x == doctest::Approx(-1.f));
    CHECK(water.nearestExit({11.f, 20.f}) == 0);
    CHECK(water.nearestExit({11.f, 30.f}) == 1);
    CHECK(water.nearestExit({10.5f, 5.f}, kWaterLadderReach) == 0);
    CHECK(water.nearestExit({12.f, 5.f}, kWaterLadderReach) == -1);
    WaterExit bad = a;
    bad.out = {0.f, 0.f};
    CHECK_FALSE(water.addExit(bad));
    const WaterExit e = waterExitToEngine(a, 1000.f);
    CHECK(e.foot.x == doctest::Approx(-490.f));
    CHECK(e.foot.y == doctest::Approx(495.f));
    CHECK(e.top.y == doctest::Approx(495.f));
    WaterExit north = a;
    north.out = {0.f, 1.f};
    CHECK(waterExitToEngine(north, 1000.f).out.y == doctest::Approx(-1.f));
    while (water.exitCount() < kWaterMaxExits) REQUIRE(water.addExit(a));
    CHECK_FALSE(water.addExit(a));
    const auto before = ftcore_test::allocStats();
    int sum = 0;
    for (int i = 0; i < 100; ++i) sum += water.nearestExit({static_cast<float>(i), 5.f}, 2.f);
    CHECK(ftcore_test::allocStats().count == before.count);
    CHECK(sum != 12345);
}

TEST_CASE("water stir: moving holds between the stop and the start speed") {
    WaterRipples rings;
    WaterStir stir;
    WaterState wade;
    wade.mode = WaterMode::Wade;
    wade.immersion = 0.5f;
    wade.surface = 2.5f;
    auto at = [&](float speed) { return stir.step(rings, wade, {0.f, 0.f}, {speed, 0.f}, 0.f, 1.f / 30.f).moving; };
    CHECK_FALSE(at(0.2f));  // under the start speed: standing
    CHECK_FALSE(at(0.25f)); // the line itself is not over it
    CHECK(at(0.27f));
    // Hovering about the start speed no longer flickers.
    CHECK(at(0.24f));
    CHECK(at(0.26f));
    CHECK(at(0.16f));
    CHECK_FALSE(at(0.14f)); // under the stop speed
    CHECK_FALSE(at(0.2f));
    CHECK(at(0.3f));
    // Out of the water and back in: standing again until it is over the start speed.
    WaterState dry;
    stir.step(rings, dry, {0.f, 0.f}, {1.f, 0.f}, 0.f, 1.f / 30.f);
    CHECK_FALSE(at(0.2f));
}

TEST_CASE("sea slip: a ramp from the quay's top to under the sea, its fill under it, buoys along the line") {
    RoadSeaSlip slip;
    slip.edgeX = 955.0;
    slip.y = 687.0;
    slip.width = 5.0;
    slip.topZ = 4.0;
    slip.endZ = -1.8;
    slip.fall = 0.5;
    slip.buoyX = 1000.0;
    slip.penY0 = 657.0;
    slip.penY1 = 717.0;
    RoadBuild build;
    const std::size_t added = roadSeaSlipBuild(slip, build);
    REQUIRE(added > 0);
    CHECK(build.solids.size() == added);
    const RoadSolid* ramp = nullptr;
    std::size_t buoys = 0, onLine = 0, floats = 0;
    for (const RoadSolid& s : build.solids) {
        if (s.box.kind == "sea_slip") ramp = &s;
        if (s.box.kind == "sea_buoy" || s.box.kind == "sea_float") {
            CHECK_FALSE(s.collide);
            CHECK(s.shape == RoadShape::Cylinder);
            CHECK(s.box.cx <= 1000.0 + 1e-6);
            CHECK(s.box.cy >= 657.0 - 1e-6);
            CHECK(s.box.cy <= 717.0 + 1e-6);
            floats += s.box.kind == "sea_float" ? 1u : 0u;
            if (s.box.kind == "sea_buoy" && s.box.sx > 0.5) {
                ++buoys;
                onLine += std::fabs(s.box.cx - 1000.0) < 1e-6 ? 1u : 0u;
                // It floats: part under the sea's level, part over.
                CHECK(s.box.z0 < slip.seaZ);
                CHECK(s.box.z1 > slip.seaZ);
            }
        }
    }
    REQUIRE(ramp);
    CHECK(ramp->collide);
    const MassingBox& r = ramp->box;
    const double run = (slip.topZ - slip.endZ) / slip.fall;
    CHECK(r.sx == doctest::Approx(run));
    CHECK(r.z1 + r.riseU * (-0.5 * r.sx) == doctest::Approx(slip.topZ));
    CHECK(r.z1 + r.riseU * (0.5 * r.sx) == doctest::Approx(slip.endZ));
    // A slope the lead walks, ending deeper than a swim begins.
    CHECK(slip.fall < 0.84);
    CHECK(slip.seaZ - slip.endZ > kWaterSwimDepth);
    for (const RoadSolid& s : build.solids) {
        if (s.box.kind != "sea_slip_fill") continue;
        // Under the ramp's top everywhere along the slice.
        const double high = r.z1 + r.riseU * (s.box.cx - 0.5 * s.box.sx - r.cx);
        const double low = r.z1 + r.riseU * (s.box.cx + 0.5 * s.box.sx - r.cx);
        CHECK(s.box.z1 < low);
        CHECK(s.box.z1 < high);
    }
    // 60 m of outer line: a buoy every 10 m (7), and each side line's too.
    CHECK(onLine == 7);
    CHECK(buoys >= 7 + 2 * 4);
    CHECK(floats > 40);
    // No pen: the slip alone. No fall: nothing.
    RoadSeaSlip bare = slip;
    bare.penY1 = bare.penY0;
    RoadBuild b2;
    CHECK(roadSeaSlipBuild(bare, b2) > 0);
    for (const RoadSolid& s : b2.solids) CHECK(s.box.kind.rfind("sea_slip", 0) == 0);
    bare.fall = 0.0;
    CHECK(roadSeaSlipBuild(bare, b2) == 0);
}
