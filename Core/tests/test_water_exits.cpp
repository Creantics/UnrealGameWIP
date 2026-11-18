#include "ftcore/WaterBodies.h"

#include "ftcore/Roads.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
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

TEST_CASE("on steps: a flight's treads are, wet or dry; its head, the bank, a ladder and the open water are not") {
    WaterBodies water;
    // A flight down a west wall whose face is at x 893.0: its head at y 718.7, its foot at y 723.1, the water east of it.
    WaterExit steps;
    steps.kind = WaterExitKind::Steps;
    steps.foot = {893.3f, 723.14f};
    steps.top = {892.f, 718.71f};
    steps.topZ = 4.27f;
    steps.out = {-1.f, 0.f};
    WaterExit ladder = steps;
    ladder.kind = WaterExitKind::Ladder;
    ladder.foot = {893.35f, 757.5f};
    ladder.top = {891.8f, 757.5f};
    CHECK_FALSE(water.onSteps({893.45f, 720.f}, 3.6f));
    REQUIRE(water.addExit(steps));
    REQUIRE(water.addExit(ladder));
    CHECK(water.onSteps({893.45f, 720.f}, 3.6f));        // a dry tread
    CHECK(water.onSteps({893.45f, 722.8f}, 2.3f));       // a wet one
    CHECK(water.onSteps({893.1f, 721.f}, 3.2f));         // by the wall
    CHECK_FALSE(water.onSteps({893.45f, 718.8f}, 4.25f)); // the head, level with the bank
    CHECK_FALSE(water.onSteps({892.5f, 720.f}, 3.6f));    // behind the wall's face
    CHECK_FALSE(water.onSteps({895.5f, 720.f}, 1.2f));    // out in the channel
    CHECK_FALSE(water.onSteps({893.45f, 725.f}, 1.6f));   // the shelf past the foot
    CHECK_FALSE(water.onSteps({893.45f, 717.f}, 3.6f));   // before the head
    CHECK_FALSE(water.onSteps({893.35f, 757.5f}, 1.6f));  // a ladder's foot
    // The same flight on the far wall, its head to the north: the run is taken the other way.
    WaterBodies east;
    WaterExit far = steps;
    far.foot = {899.7f, 760.f};
    far.top = {901.f, 764.2f};
    far.out = {1.f, 0.f};
    REQUIRE(east.addExit(far));
    CHECK(east.onSteps({899.5f, 762.f}, 3.4f));
    CHECK_FALSE(east.onSteps({899.5f, 758.f}, 1.6f));
    CHECK_FALSE(east.onSteps({898.f, 762.f}, 1.2f));
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

TEST_CASE("canal lip: no bed past the shelf between the swim's exit and the lip's depth; a swimmer still gets out") {
    // The lip's depth: the draft, the bob and the clearance; the face over a swimmer's lowest feet is a step up
    // (CharacterMovement's 45 cm MaxStepHeight, which the flying swim steps up too).
    CHECK(kWaterLipDepth == doctest::Approx(1.48f));
    CHECK(kWaterLipDepth > kWaterSwimDepth);
    CHECK(waterSectionDepth(0.6f, 1.f, 0.6f, 1.5f, 2.f, 0.48f) == doctest::Approx(1.f));
    CHECK(waterSectionDepth(0.61f, 1.f, 0.6f, 1.5f, 2.f, 0.48f) == doctest::Approx(1.4835f).epsilon(0.001));
    CHECK(waterSectionDepth(2.1f, 1.f, 0.6f, 1.5f, 2.f, 0.48f) == doctest::Approx(2.f));
    CHECK(waterSectionDepth(1.f, 1.f, 0.6f, 1.5f, 1.2f, 0.48f) == doctest::Approx(1.2f)); // no deeper than the middle
    CHECK(waterSectionDepth(1.f, 1.f, 0.6f, 0.f, 2.f, 0.48f) == doctest::Approx(1.f));    // a flat bed has none
    for (const RoadCanal& base : {canalOf("CN-N", 892.0, 664.0, 901.0, 814.0, 700.0), canalOf("CN-P", 744.0, 786.0, 892.0, 814.0, 830.0)}) {
        CAPTURE(base.id);
        CHECK(roadCanalSection(base).lip == doctest::Approx(0.0)); // off by default: today's section
        RoadCanal canal = base;
        canal.lip = true;
        const RoadCanalSection section = roadCanalSection(canal);
        CHECK(section.lip == doctest::Approx(kWaterLipDepth - 1.0));
        CHECK(section.deep == doctest::Approx(2.0));
        RoadBuild build;
        RoadBuild before;
        Banks banks;
        REQUIRE(roadCanalBuild(canal, banks.ground(), build) > 0);
        REQUIRE(roadCanalBuild(base, banks.ground(), before) > 0);
        CHECK(build.solids.size() == before.solids.size()); // same pieces, same ids
        REQUIRE(build.waters.size() == 1);
        const RoadWater& w = build.waters[0];
        CHECK(w.lip == doctest::Approx(section.lip));
        WaterBodies water;
        REQUIRE(water.add(waterBodyFromCanal(w)));
        CHECK(water.body(0).lip == doctest::Approx(static_cast<float>(section.lip)));
        for (std::size_t i = 0; i < build.exits.size(); ++i) REQUIRE(water.addExit(waterExitFromRoad(build.exits[i])));
        const bool alongY = w.sy >= w.sx;
        const double across = alongY ? w.sx : w.sy;
        int band = 0, checked = 0;
        for (double d = 0.005; d < across; d += 0.01) {
            const double x = alongY ? w.cx - 0.5 * w.sx + d : w.cx + 3.0;
            const double y = alongY ? w.cy + 3.0 : w.cy - 0.5 * w.sy + d;
            const Vec2 p{static_cast<float>(x), static_cast<float>(y)};
            const float column = water.depth(p);
            // The builder and the body share the section.
            CHECK(column == doctest::Approx(w.z - builtFloor(build, x, y)).epsilon(0.01));
            band += column > kWaterSwimExit && column < kWaterLipDepth - 1e-3f ? 1 : 0;
            ++checked;
        }
        CHECK(checked > 600);
        CHECK(band == 0);
        // Without the lip the slope crosses the band (the 1.16 m column a partner rested in).
        WaterBodies old;
        REQUIRE(old.add(waterBodyFromCanal(before.waters[0])));
        const double at = 0.6 + (1.16 - 1.0) * 1.5;
        const Vec2 q{static_cast<float>(alongY ? w.cx - 0.5 * w.sx + at : w.cx + 3.0), static_cast<float>(alongY ? w.cy + 3.0 : w.cy - 0.5 * w.sy + at)};
        CHECK(old.depth(q) == doctest::Approx(1.16f).epsilon(0.01));
        CHECK(water.depth(q) >= kWaterLipDepth - 1e-3f);
        // The face over a swimmer's lowest feet at the lip is a step up of 45 cm or less.
        const float face = kWaterSwimDraft + kWaterBobHeight - static_cast<float>(section.depth);
        CHECK(face > 0.f);
        CHECK(face <= 0.45f);
        // A swimmer held at the lip (a 34 cm capsule against the shelf's face) in front of a ladder is in its reach.
        int ladders = 0;
        for (std::size_t i = 0; i < water.exitCount(); ++i) {
            const WaterExit& e = water.exit(i);
            // The long walls' ladders (an end wall's, on water over 12 m wide, has no shelf before it).
            if (e.kind != WaterExitKind::Ladder || std::fabs(alongY ? e.out.x : e.out.y) < 0.5f) continue;
            ++ladders;
            const Vec2 face0{e.foot.x + e.out.x * 0.35f, e.foot.y + e.out.y * 0.35f}; // the wall's face behind the foot
            const float off = static_cast<float>(section.shelf) + 0.34f;
            const Vec2 held{face0.x - e.out.x * off, face0.y - e.out.y * off};
            CAPTURE(i);
            CHECK(water.depth(held) >= kWaterLipDepth - 1e-3f);
            CHECK(std::hypot(held.x - e.foot.x, held.y - e.foot.y) <= kWaterLadderReach);
            CHECK(water.nearestExit(held, kWaterLadderReach) >= 0);
            // On the shelf in front of it a body wades.
            WaterCapsule c;
            c.p = {face0.x - e.out.x * 0.4f, face0.y - e.out.y * 0.4f};
            c.feetZ = static_cast<float>(w.z - section.depth);
            c.swimming = true;
            CHECK(waterState(water, c, 0.f).mode == WaterMode::DeepWade);
        }
        CHECK(ladders > 0);
    }
    // A shelf that is no wade (its water over the swim's exit) gets no lip; nor does a flat bed.
    RoadCanal deepShelf = canalOf("CN-D", 892.0, 424.0, 901.0, 636.0, 560.0);
    deepShelf.floorZ = 1.3;
    deepShelf.lip = true;
    CHECK(roadCanalSection(deepShelf).lip == doctest::Approx(0.0));
    RoadCanal narrow = canalOf("CN-S", 0.0, 0.0, 6.0, 60.0, 0.0);
    narrow.lip = true;
    CHECK(roadCanalSection(narrow).lip == doctest::Approx(0.0));
    // A middle shallower than the lip's depth: the lip drops to it and the slope lies flat there.
    RoadCanal shallow = canalOf("CN-H", 892.0, 424.0, 901.0, 636.0, 560.0);
    shallow.deep = 1.4;
    shallow.lip = true;
    CHECK(roadCanalSection(shallow).lip == doctest::Approx(0.4));
}

namespace {

// Flat ground at 4 m from (0, 0), a sample every 50 m: banks for made-up canals.
struct FlatBanks {
    std::vector<std::uint16_t> grid = std::vector<std::uint16_t>(9u * 9u, static_cast<std::uint16_t>(32768 + 4 * 128));
    MassingGround ground() const {
        MassingGround g;
        g.grid = grid.data();
        g.samplesX = 9;
        g.samplesY = 9;
        g.spacing = 50.0;
        return g;
    }
};

// Which long wall a way out is on (+1 / -1 along the across axis), 0 for an end wall's.
int wallOf(const RoadWaterExit& e, bool alongY) {
    const double o = alongY ? e.outX : e.outY;
    return o > 0.5 ? 1 : (o < -0.5 ? -1 : 0);
}
double gapMiddle(const RoadWaterExit& e, bool alongY) { return alongY ? 0.5 * (e.gapY0 + e.gapY1) : 0.5 * (e.gapX0 + e.gapX1); }
// The stretch of its wall a way out takes along the bank: its rail gap to its foot.
std::pair<double, double> stretchOf(const RoadWaterExit& e, bool alongY) {
    const double g0 = alongY ? e.gapY0 : e.gapX0, g1 = alongY ? e.gapY1 : e.gapX1, f = alongY ? e.footY : e.footX;
    return {std::min({g0, g1, f}), std::max({g0, g1, f})};
}

} // namespace

TEST_CASE("canal openings: a flight's rail gap within 15 m along every bank, 1.2 m wide, clear of the rest; off as before") {
    const FlatBanks banks;
    const RoadCanal canals[] = {canalOf("CN-A", 100.0, 60.0, 109.0, 330.0, 200.0),  // narrow, along y
                                canalOf("CN-B", 40.0, 120.0, 300.0, 136.0, 150.0)}; // 14 m of water, along x, end ladders
    for (const RoadCanal& base : canals) {
        CAPTURE(base.id);
        RoadCanal open = base;
        open.openings = true;
        RoadBuild off, on;
        REQUIRE(roadCanalBuild(base, banks.ground(), off) > 0);
        REQUIRE(roadCanalBuild(open, banks.ground(), on) > 0);
        const bool alongY = on.waters[0].alongY;
        CHECK(alongY == (base.y1 - base.y0 > base.x1 - base.x0));
        // Today's ways out stand as they were, the new flights after them.
        REQUIRE(on.exits.size() > off.exits.size());
        for (std::size_t i = 0; i < off.exits.size(); ++i) {
            const RoadWaterExit &a = off.exits[i], &b = on.exits[i];
            CHECK(a.id == b.id);
            CHECK(a.kind == b.kind);
            CHECK(a.footX == b.footX);
            CHECK(a.footY == b.footY);
            CHECK(a.topX == b.topX);
            CHECK(a.topY == b.topY);
            CHECK(a.topZ == b.topZ);
            CHECK(a.gapX0 == b.gapX0);
            CHECK(a.gapY0 == b.gapY0);
            CHECK(a.gapX1 == b.gapX1);
            CHECK(a.gapY1 == b.gapY1);
        }
        // Off, a flight is two ways out apart on a wall: farther than the reach.
        CHECK(roadCanalOpeningWorst(base, off) > kCanalOpeningReach);
        const double worst = roadCanalOpeningWorst(open, on);
        CHECK(worst <= kCanalOpeningReach);
        // Every half metre along each bank, end to end: a flight's gap on that wall within the reach, along it.
        const double ua = alongY ? base.y0 : base.x0, ub = alongY ? base.y1 : base.x1;
        double sampled = 0.0;
        for (const int side : {-1, 1})
            for (double u = ua; u <= ub + 1e-9; u += 0.5) {
                double nearest = HUGE_VAL;
                for (const RoadWaterExit& e : on.exits)
                    if (e.kind == RoadExitKind::Steps && wallOf(e, alongY) == side) nearest = std::min(nearest, std::fabs(gapMiddle(e, alongY) - u));
                CAPTURE(side);
                CAPTURE(u);
                CHECK(nearest <= kCanalOpeningReach);
                sampled = std::max(sampled, nearest);
            }
        CHECK(worst >= sampled);
        CHECK(worst <= sampled + 0.5);
        // The added ways out: flights only, inside the end walls, each 0.5 m clear of every other way out on its wall
        // and of the footbridge's landing.
        const double sa = ua + base.wall, sb = ub - base.wall;
        for (std::size_t i = off.exits.size(); i < on.exits.size(); ++i) {
            const RoadWaterExit& e = on.exits[i];
            CAPTURE(e.id);
            CHECK(e.kind == RoadExitKind::Steps);
            const int side = wallOf(e, alongY);
            CHECK(side != 0);
            const std::pair<double, double> s = stretchOf(e, alongY);
            CHECK(s.first >= sa);
            CHECK(s.second <= sb);
            CHECK((s.second <= base.bridgeAt - 0.5 * kQuayBridgeWide - 0.5 + 1e-9 || s.first >= base.bridgeAt + 0.5 * kQuayBridgeWide + 0.5 - 1e-9));
            for (std::size_t j = 0; j < on.exits.size(); ++j) {
                if (j == i || wallOf(on.exits[j], alongY) != side) continue;
                const std::pair<double, double> r = stretchOf(on.exits[j], alongY);
                CHECK((s.second <= r.first - 0.5 + 1e-9 || s.first >= r.second + 0.5 - 1e-9));
            }
        }
        // Every flight's treads kCanalOpeningWide across with openings, 0.9 m without.
        std::size_t treadsOn = 0, treadsOff = 0;
        for (const RoadSolid& s : on.solids)
            if (s.box.kind == "canal_steps") {
                ++treadsOn;
                CHECK((alongY ? s.box.sx : s.box.sy) == doctest::Approx(kCanalOpeningWide));
            }
        for (const RoadSolid& s : off.solids)
            if (s.box.kind == "canal_steps") {
                ++treadsOff;
                CHECK((alongY ? s.box.sx : s.box.sy) == doctest::Approx(0.9));
            }
        CHECK(treadsOn > treadsOff);
    }
    // No ways out at all: infinity.
    CHECK(roadCanalOpeningWorst(canals[0], RoadBuild{}) == HUGE_VAL);
}

TEST_CASE("canal openings: bank under a long deck passed over, the flights go on past it") {
    const FlatBanks banks;
    RoadCanal canal = canalOf("CN-A", 100.0, 60.0, 109.0, 330.0, 200.0);
    canal.openings = true;
    // A road's deck 30 m long over the whole channel, from wall top to wall top.
    const double d0 = 140.0, d1 = 170.0;
    RoadSolid deck;
    deck.box.id = "DECK";
    deck.box.kind = "asphalt";
    deck.box.cx = 104.5;
    deck.box.cy = 0.5 * (d0 + d1);
    deck.box.sx = 9.0;
    deck.box.sy = d1 - d0;
    deck.box.z0 = 4.0;
    deck.box.z1 = 4.4;
    deck.collide = true;
    RoadBuild b;
    b.solids.push_back(deck);
    REQUIRE(roadCanalBuild(canal, banks.ground(), b) > 0);
    // No flight's run (rail gap to foot) under the deck.
    for (const RoadWaterExit& e : b.exits) {
        if (e.kind != RoadExitKind::Steps || wallOf(e, true) == 0) continue;
        CAPTURE(e.id);
        const std::pair<double, double> s = stretchOf(e, true);
        CHECK((s.second <= d0 || s.first >= d1));
    }
    // Every bank point off the deck, past it too: a flight's gap on that wall within the reach.
    double sampled = 0.0;
    for (const int side : {-1, 1})
        for (double u = canal.y0; u <= canal.y1 + 1e-9; u += 0.5) {
            if (u >= d0 - 1e-9 && u <= d1 + 1e-9) continue;
            double nearest = HUGE_VAL;
            for (const RoadWaterExit& e : b.exits)
                if (e.kind == RoadExitKind::Steps && wallOf(e, true) == side) nearest = std::min(nearest, std::fabs(gapMiddle(e, true) - u));
            CAPTURE(side);
            CAPTURE(u);
            CHECK(nearest <= kCanalOpeningReach);
            sampled = std::max(sampled, nearest);
        }
    const double worst = roadCanalOpeningWorst(canal, b);
    CHECK(worst <= kCanalOpeningReach);
    CHECK(worst >= sampled);
    // The deck's own bank is no edge: without it in the build, the same ways out leave that stretch out of reach.
    RoadBuild bare;
    bare.exits = b.exits;
    CHECK(roadCanalOpeningWorst(canal, bare) > kCanalOpeningReach);
}

TEST_CASE("canal openings: no added flight under a deck, not even one over only its middle") {
    const FlatBanks banks;
    RoadCanal canal = canalOf("CN-A", 100.0, 60.0, 109.0, 330.0, 200.0);
    canal.openings = true;
    RoadBuild open;
    REQUIRE(roadCanalBuild(canal, banks.ground(), open) > 0);
    // The last added flight: its run from 0.55 m before its gap's middle to its foot, along y.
    const RoadWaterExit added = open.exits.back();
    REQUIRE(added.kind == RoadExitKind::Steps);
    const int side = wallOf(added, true);
    const double top = gapMiddle(added, true) - 0.55, foot = added.footY;
    REQUIRE(foot - top > 3.0);
    // A deck 1 m long over the water at the run's middle: clear of both its ends.
    const double mid = 0.5 * (top + foot);
    RoadSolid deck;
    deck.box.id = "DECK";
    deck.box.kind = "asphalt";
    deck.box.cx = 104.5;
    deck.box.cy = mid;
    deck.box.sx = 7.0;
    deck.box.sy = 1.0;
    deck.box.z0 = 4.0;
    deck.box.z1 = 4.4;
    deck.collide = true;
    RoadBuild roofed;
    roofed.solids.push_back(deck);
    REQUIRE(roadCanalBuild(canal, banks.ground(), roofed) > 0);
    bool under = false;
    for (const RoadWaterExit& e : roofed.exits) {
        if (e.kind != RoadExitKind::Steps || wallOf(e, true) != side) continue;
        const double a = gapMiddle(e, true) - 0.55, b = e.footY;
        under = under || (b > mid - 0.5 && a < mid + 0.5);
    }
    CHECK_FALSE(under);
}
