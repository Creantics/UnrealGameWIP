#include "ftcore/WaterBodies.h"

#include "ftcore/Roads.h"
#include "ftcore/SeaWaves.h"
#include "ftcore/Terrain.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

using namespace ftcore;

namespace {

// A north-south canal 9 m wide and 150 m long, floor 1.5 m, water 2.5 m, running north at 0.25 m/s.
RoadWater northCanal() {
    RoadWater w;
    w.id = "CN-A";
    w.cx = 896.5;
    w.cy = 739.0;
    w.sx = 7.0;
    w.sy = 148.0;
    w.z = 2.5;
    w.depth = 1.0;
    w.flowX = 0.0;
    w.flowY = 1.0;
    w.flow = 0.25;
    w.halfWidth = 3.5;
    return w;
}

// An east-west pool that meets the canal's north end: the two overlap over x 893 to 895.
RoadWater pool() {
    RoadWater w;
    w.id = "CN-B";
    w.cx = 820.0;
    w.cy = 800.0;
    w.sx = 150.0;
    w.sy = 26.0;
    w.z = 2.5;
    w.depth = 1.0;
    w.flowX = 1.0;
    w.flowY = 0.0;
    w.flow = 0.08;
    w.halfWidth = 13.0;
    return w;
}

WaterBodies town() {
    WaterBodies water;
    REQUIRE(water.add(waterBodyFromCanal(northCanal())));
    REQUIRE(water.add(waterBodyFromCanal(pool())));
    REQUIRE(water.add(waterSea(940.f, 0.f, -33.f)));
    return water;
}

WaterCapsule standing(Vec2 p, float groundZ) {
    WaterCapsule c;
    c.p = p;
    c.feetZ = groundZ;
    c.groundZ = groundZ;
    c.hasGround = true;
    c.height = 1.8f;
    return c;
}

} // namespace

static_assert(WorldTerrain::kWadeDepth == kWaterWadeDepth, "one wade depth");

TEST_CASE("water bodies: a canal row becomes a box with its level, bed and flow") {
    const WaterBody b = waterBodyFromCanal(northCanal());
    CHECK(b.kind == WaterKind::Canal);
    CHECK(b.shape == WaterShape::Box);
    CHECK(b.level == doctest::Approx(2.5f));
    CHECK(b.bed == doctest::Approx(1.5f));
    CHECK(b.flow.x == doctest::Approx(0.f));
    CHECK(b.flow.y == doctest::Approx(0.25f));
    CHECK(b.covers({896.5f, 700.f}));
    CHECK(b.covers({893.f, 665.f}));  // its corner: the edge is in
    CHECK_FALSE(b.covers({892.9f, 700.f})); // the bank
    CHECK_FALSE(b.covers({896.5f, 813.1f}));
}

TEST_CASE("water bodies: surface and depth inside, on the bank and outside every body") {
    const WaterBodies water = town();
    float z = -1.f;
    CHECK(water.surfaceHeight({896.5f, 700.f}, 0.f, z));
    CHECK(z == doctest::Approx(2.5f));
    CHECK(water.depth({896.5f, 700.f}) == doctest::Approx(1.f));
    // The bank, a step outside the wall.
    z = -1.f;
    CHECK_FALSE(water.surfaceHeight({892.5f, 700.f}, 0.f, z));
    CHECK(z == -1.f);
    CHECK(water.depth({892.5f, 700.f}) == 0.f);
    CHECK(water.bodyAt({500.f, 500.f}) == -1);
    // The sea, flat at the datum.
    CHECK(water.surfaceHeight({960.f, 300.f}, 12.f, z));
    CHECK(z == doctest::Approx(0.f));
    CHECK(water.depth({960.f, 300.f}) == doctest::Approx(33.f));
    CHECK(water.bodyAt({939.9f, 300.f}) == -1);
    CHECK(water.bodyAt({940.f, 300.f}) == 2); // on the edge
    CHECK_FALSE(water.bodyAt({std::nanf(""), 0.f}) >= 0);
}

TEST_CASE("water bodies: where two overlap, the higher surface, then the deeper, then the first") {
    WaterBodies water = town();
    // The canal and the pool meet: same level and bed, so the first added.
    const Vec2 meet{894.f, 805.f};
    REQUIRE(water.body(0).covers(meet));
    REQUIRE(water.body(1).covers(meet));
    CHECK(water.bodyAt(meet) == 0);
    CHECK(water.depth(meet) == doctest::Approx(1.f));
    // A lock basin over the meeting point, a metre higher: it is the water there.
    WaterBody basin;
    basin.shape = WaterShape::Box;
    basin.kind = WaterKind::Pool;
    basin.centre = meet;
    basin.half = {1.f, 1.f};
    basin.level = 3.5f;
    basin.bed = 2.f;
    REQUIRE(water.add(basin));
    CHECK(water.bodyAt(meet) == 3);
    float z = 0.f;
    CHECK(water.surfaceHeight(meet, 0.f, z));
    CHECK(z == doctest::Approx(3.5f));
    CHECK(water.depth(meet) == doctest::Approx(1.5f));
    // Equal surfaces: the deeper.
    WaterBody pit = basin;
    pit.bed = 0.f;
    REQUIRE(water.add(pit));
    CHECK(water.bodyAt(meet) == 4);
    // The camera takes the body it is deepest in.
    const WaterCamera cam = water.camera(meet, 1.8f, 0.f);
    CHECK(cam.under);
    CHECK(cam.body == 4); // under the basin's bed, so the pit's (the canal's surface is only 0.7 m over the eye)
    CHECK(cam.depth == doctest::Approx(1.7f));
}

TEST_CASE("water bodies: a turned box, a polygon, and bodies that are refused") {
    WaterBodies water;
    WaterBody box;
    box.centre = {10.f, 10.f};
    box.half = {5.f, 1.f};
    box.cosYaw = std::cos(0.78539816f) * 3.f; // not unit: add() sets it right
    box.sinYaw = std::sin(0.78539816f) * 3.f;
    box.level = 1.f;
    box.bed = 0.f;
    REQUIRE(water.add(box));
    CHECK(water.bodyAt({13.f, 13.f}) == 0);  // along its long axis
    CHECK(water.bodyAt({13.f, 7.f}) == -1);  // across it
    WaterBody lake;
    lake.shape = WaterShape::Polygon;
    lake.kind = WaterKind::Lake;
    lake.pointCount = 6; // an L
    lake.points[0] = {100.f, 100.f};
    lake.points[1] = {110.f, 100.f};
    lake.points[2] = {110.f, 104.f};
    lake.points[3] = {104.f, 104.f};
    lake.points[4] = {104.f, 110.f};
    lake.level = 0.5f;
    lake.bed = -2.f;
    lake.points[5] = {100.f, 110.f};
    CHECK(water.count() == 1);
    REQUIRE(water.add(lake));
    CHECK(water.bodyAt({108.f, 102.f}) == 1);
    CHECK(water.bodyAt({102.f, 108.f}) == 1);
    CHECK(water.bodyAt({108.f, 108.f}) == -1); // the L's notch
    CHECK(water.bodyAt({100.f, 105.f}) == 1);  // on its edge
    WaterBody bad = box;
    bad.half = {0.f, 1.f};
    CHECK_FALSE(water.add(bad));
    bad = box;
    bad.bed = 2.f; // over its surface
    CHECK_FALSE(water.add(bad));
    bad = lake;
    bad.pointCount = 2;
    CHECK_FALSE(water.add(bad));
    for (std::size_t i = water.count(); i < kWaterMaxBodies; ++i) REQUIRE(water.add(box));
    CHECK_FALSE(water.add(box)); // full
}

TEST_CASE("water bodies: wade, deep wade and swim by the column, exactly at the thresholds") {
    WaterBodies water;
    REQUIRE(water.add(waterSea(0.f, 0.f, -33.f)));
    auto at = [&](float groundZ, bool swimming = false) {
        WaterCapsule c = standing({10.f, 0.f}, groundZ);
        c.swimming = swimming;
        return waterState(water, c, 0.f);
    };
    CHECK(at(0.f).mode == WaterMode::Dry); // feet on the surface: not in it
    CHECK(at(0.5f).mode == WaterMode::Dry);
    CHECK(at(-0.01f).mode == WaterMode::Wade);
    CHECK(at(-0.01f).speedFactor == doctest::Approx(1.f).epsilon(0.01));
    CHECK(at(-kWaterWadeDepth).mode == WaterMode::Wade); // the threshold is still a wade
    CHECK(at(-kWaterWadeDepth).speedFactor == doctest::Approx(kWaterWadeFactor));
    CHECK(at(-0.71f).mode == WaterMode::DeepWade);
    CHECK(at(-kWaterSwimDepth).mode == WaterMode::DeepWade); // and this one still a deep wade
    CHECK(at(-kWaterSwimDepth).speedFactor == doctest::Approx(kWaterDeepFactor));
    CHECK(at(-1.21f).mode == WaterMode::Swim);
    CHECK(at(-20.f).mode == WaterMode::Swim);
    // Slower all the way down.
    float last = 1.01f;
    for (float d = 0.05f; d < 1.3f; d += 0.05f) {
        const float f = waterWadeFactor(d);
        CHECK(f < last);
        CHECK(f >= kWaterDeepFactor);
        last = f + 1e-6f;
        if (d > 1.2f) break;
    }
    CHECK(waterWadeFactor(-1.f) == 1.f);
    CHECK(waterWadeFactor(5.f) == kWaterDeepFactor);
    // A swimmer keeps swimming down to the exit depth; a wader starts only past the swim depth.
    CHECK(at(-1.15f, true).mode == WaterMode::Swim);
    CHECK(at(-1.15f, false).mode == WaterMode::DeepWade);
    CHECK(at(-kWaterSwimExit, true).mode == WaterMode::Swim);
    CHECK(at(-1.05f, true).mode == WaterMode::DeepWade);
    // Submerged share from the feet.
    CHECK(at(-0.9f).submerged == doctest::Approx(0.5f));
    CHECK(at(-0.9f).immersion == doctest::Approx(0.9f));
    CHECK(at(-0.9f).column == doctest::Approx(0.9f));
    CHECK(at(-5.f).submerged == 1.f);
}

TEST_CASE("water bodies: a body held at the surface over deep water swims; over the water it is dry") {
    const WaterBodies water = town();
    WaterCapsule c;
    c.p = {970.f, 300.f};
    c.height = 1.8f;
    c.hasGround = true;
    c.groundZ = -12.f;
    // Falling in: the first toe under the surface is already a swim (the wrapper eases it to the draft).
    c.feetZ = -0.05f;
    CHECK(waterState(water, c, 0.f).mode == WaterMode::Swim);
    // Riding at the draft with its bob, it never leaves the swim.
    for (float t = 0.f; t < 6.f; t += 0.1f) {
        c.feetZ = waterSwimFeetZ(0.f, t);
        c.swimming = true;
        const WaterState s = waterState(water, c, t);
        CHECK(s.mode == WaterMode::Swim);
        CHECK(s.submerged > 0.65f);
        CHECK(s.submerged < 0.8f);
        CHECK(c.feetZ == doctest::Approx(-kWaterSwimDraft).epsilon(0.05));
    }
    // In the air over it.
    c.feetZ = 0.4f;
    CHECK(waterState(water, c, 0.f).mode == WaterMode::Dry);
    CHECK(waterState(water, c, 0.f).body == -1);
    // On a bridge deck over the canal: dry, whatever the canal under it.
    CHECK(waterState(water, standing({896.5f, 700.f}, 6.f), 0.f).mode == WaterMode::Dry);
    // On the canal's floor: a deep wade, never a swim (a metre of water).
    const WaterState floor = waterState(water, standing({896.5f, 700.f}, 1.5f), 0.f);
    CHECK(floor.mode == WaterMode::DeepWade);
    CHECK(floor.body == 0);
    CHECK(floor.speedFactor < kWaterWadeFactor);
    CHECK(floor.speedFactor > kWaterDeepFactor);
    // Without a ground the body's bed is the ground.
    WaterCapsule bare;
    bare.p = {896.5f, 700.f};
    bare.feetZ = 2.f;
    CHECK(waterState(water, bare, 0.f).column == doctest::Approx(1.f));
    CHECK(waterState(water, bare, 0.f).mode == WaterMode::Wade); // only its feet are in
    // A hop out of a wade: less held than the wade.
    WaterCapsule hop = standing({896.5f, 700.f}, 1.5f);
    hop.feetZ = 2.2f;
    CHECK(waterState(water, hop, 0.f).speedFactor > floor.speedFactor);
    // Off every body.
    CHECK(waterState(water, standing({500.f, 500.f}, -3.f), 0.f).mode == WaterMode::Dry);
}

TEST_CASE("water bodies: the camera is under inside a footprint and below the surface only") {
    const WaterBodies water = town();
    WaterCamera cam = water.camera({896.5f, 700.f}, 2.2f, 0.f);
    CHECK(cam.under);
    CHECK(cam.body == 0);
    CHECK(cam.depth == doctest::Approx(0.3f));
    CHECK(cam.surface == doctest::Approx(2.5f));
    CHECK_FALSE(water.camera({896.5f, 700.f}, 2.5f, 0.f).under);  // on the surface: not under
    CHECK(water.camera({896.5f, 700.f}, 2.55f, 0.f, 0.1f).under); // within the margin (the near plane)
    CHECK(water.camera({896.5f, 700.f}, 2.55f, 0.f, 0.1f).depth == 0.f);
    CHECK_FALSE(water.camera({896.5f, 700.f}, 3.f, 0.f, 0.1f).under);
    CHECK_FALSE(water.camera({892.5f, 700.f}, 2.2f, 0.f).under);  // in the bank
    CHECK_FALSE(water.camera({896.5f, 700.f}, 1.2f, 0.f).under);  // under the floor
    // The ground by the sea lies under the canal's level: an eye there, off the canal, is not under.
    CHECK_FALSE(water.camera({930.f, 300.f}, 1.f, 0.f).under);
    cam = water.camera({980.f, 300.f}, -6.f, 0.f);
    CHECK(cam.under);
    CHECK(cam.body == 2);
    CHECK(cam.depth == doctest::Approx(6.f));
}

TEST_CASE("water bodies: the flow pushes mid-channel, slackens at the walls, and is nothing outside") {
    const WaterBodies water = town();
    const Vec2 mid = water.flowAt({896.5f, 700.f});
    CHECK(mid.x == doctest::Approx(0.f));
    CHECK(mid.y == doctest::Approx(0.25f));
    const Vec2 wall = water.flowAt({893.f, 700.f});
    CHECK(wall.y == doctest::Approx(0.25f * kWaterBankSlack));
    const Vec2 between = water.flowAt({894.75f, 700.f});
    CHECK(between.y < mid.y);
    CHECK(between.y > wall.y);
    const Vec2 poolMid = water.flowAt({820.f, 800.f});
    CHECK(poolMid.x == doctest::Approx(0.08f));
    CHECK(poolMid.y == doctest::Approx(0.f));
    const Vec2 none = water.flowAt({500.f, 500.f});
    CHECK(none.x == 0.f);
    CHECK(none.y == 0.f);
    // The state carries the push.
    CHECK(waterState(water, standing({896.5f, 700.f}, 1.5f), 0.f).flow.y == doctest::Approx(0.25f));
}

TEST_CASE("water bodies: past the buoy line the sea's current turns a swimmer back") {
    const WaterBodies water = town();
    // Inside the line: no current.
    CHECK(water.flowAt({940.f + kWaterBuoyLine - 0.5f, 300.f}).x == 0.f);
    CHECK(water.flowAt({940.f + kWaterBuoyLine, 300.f}).x == 0.f);
    // Past it: back toward the shore, harder the further out, never past its cap.
    const float one = water.flowAt({940.f + kWaterBuoyLine + 1.f, 300.f}).x;
    const float three = water.flowAt({940.f + kWaterBuoyLine + 3.f, 300.f}).x;
    CHECK(one == doctest::Approx(-kWaterBuoyPull));
    CHECK(three < one);
    CHECK(water.flowAt({2000.f, 300.f}).x == doctest::Approx(-kWaterBuoyPullMax));
    CHECK(water.flowAt({2000.f, 300.f}).y == 0.f);
    // A swimmer heading straight out at full speed for five minutes never gets 3 m past the line, and is let go
    // the moment it turns back.
    Vec2 p{945.f, 300.f};
    float furthest = 0.f;
    const float dt = 1.f / 30.f;
    for (int i = 0; i < 30 * 300; ++i) {
        const Vec2 push = water.flowAt(p);
        p.x += (kWaterSwimSpeed + push.x) * dt;
        p.y += push.y * dt;
        furthest = std::max(furthest, p.x - 940.f);
    }
    CHECK(furthest > kWaterBuoyLine);
    CHECK(furthest < kWaterBuoyLine + 3.f);
    CHECK(p.x - 940.f == doctest::Approx(kWaterBuoyLine + kWaterSwimSpeed / kWaterBuoyPull).epsilon(0.01));
    // A sea with no line has no current.
    WaterBodies open;
    REQUIRE(open.add(waterSea(0.f, 0.f, -10.f, 0.f)));
    CHECK(open.flowAt({5000.f, 0.f}).x == 0.f);
}

TEST_CASE("water bodies: ground under a body's surface is the water's, for a follower's footing") {
    const WaterBodies water = town();
    CHECK(water.overWater({896.5f, 700.f}, 1.5f));        // the canal's floor
    CHECK_FALSE(water.overWater({896.5f, 700.f}, 6.f));   // a bridge deck over it
    CHECK_FALSE(water.overWater({892.5f, 700.f}, 1.5f));  // the bank (whatever its height)
    CHECK(water.overWater({960.f, 300.f}, -4.f));
    CHECK(water.overWater({896.5f, 700.f}, 2.55f, 0.1f)); // within the margin over the surface
    CHECK_FALSE(water.overWater({896.5f, 700.f}, 2.55f));
}

TEST_CASE("water bodies: a sea on waves reads the waves' height; flat without them") {
    SceneLook look;
    const SeaWaves sea = seaWaves(look);
    WaterBodies water;
    WaterBody body = waterSea(0.f, 0.f, -33.f);
    body.waves = true;
    REQUIRE(water.add(body));
    float z = 9.f;
    CHECK(water.surfaceHeight({40.f, 20.f}, 3.f, z));
    CHECK(z == 0.f); // no waves given: flat
    water.setWaves(&sea);
    CHECK(water.surfaceHeight({40.f, 20.f}, 3.f, z));
    CHECK(z == doctest::Approx(seaHeight(sea, 4000.f, 2000.f, 3.f) * 0.01f));
    // The state and the camera read the same surface.
    WaterCapsule c;
    c.p = {40.f, 20.f};
    c.feetZ = z - 0.3f;
    CHECK(waterState(water, c, 3.f).immersion == doctest::Approx(0.3f).epsilon(0.01));
    CHECK(water.camera({40.f, 20.f}, z - 0.5f, 3.f).depth == doctest::Approx(0.5f).epsilon(0.01));
}

TEST_CASE("water bodies: the plan frame to the engine's turns y, the flow and the sea's way over") {
    const float side = 1000.f;
    const WaterBody canal = waterBodyToEngine(waterBodyFromCanal(northCanal()), side);
    CHECK(canal.centre.x == doctest::Approx(396.5f));
    CHECK(canal.centre.y == doctest::Approx(-239.f));
    CHECK(canal.flow.y == doctest::Approx(-0.25f)); // north is -y there
    CHECK(canal.covers({396.5f, -200.f}));
    CHECK_FALSE(canal.covers({396.5f, 200.f}));
    const WaterBody sea = waterBodyToEngine(waterSea(940.f, 0.f, -33.f), side);
    CHECK(sea.covers({440.f, 77.f}));
    CHECK_FALSE(sea.covers({439.f, 77.f}));
    CHECK(sea.out({500.f, -300.f}) == doctest::Approx(60.f));
    WaterBody tilted = waterBodyFromCanal(northCanal());
    tilted.cosYaw = std::cos(0.5f);
    tilted.sinYaw = std::sin(0.5f);
    const Vec2 planPoint{tilted.centre.x + 3.f * tilted.cosYaw, tilted.centre.y + 3.f * tilted.sinYaw};
    const WaterBody turned = waterBodyToEngine(tilted, side);
    CHECK(tilted.covers(planPoint));
    CHECK(turned.covers({planPoint.x - 500.f, 500.f - planPoint.y}));
    const Vec2 off{tilted.centre.x - 20.f * tilted.sinYaw, tilted.centre.y + 20.f * tilted.cosYaw};
    CHECK(tilted.covers(off) == turned.covers({off.x - 500.f, 500.f - off.y}));
}

TEST_CASE("water bodies: no query allocates") {
    const WaterBodies water = town();
    const WaterCapsule c = standing({896.5f, 700.f}, 1.5f);
    const auto before = ftcore_test::allocStats();
    float z = 0.f;
    float sum = 0.f;
    for (int i = 0; i < 200; ++i) {
        const Vec2 p{880.f + static_cast<float>(i) * 0.5f, 700.f};
        sum += water.surfaceHeight(p, 1.f, z) ? z : 0.f;
        sum += water.depth(p) + water.flowAt(p).y + (water.overWater(p, 1.f) ? 1.f : 0.f);
        sum += water.camera(p, 2.f, 1.f).depth;
        sum += waterState(water, c, 1.f).speedFactor;
    }
    const auto after = ftcore_test::allocStats();
    CHECK(after.count == before.count);
    CHECK(sum > 0.f);
}

TEST_CASE("water ripples: a ring grows, fades and ages out") {
    WaterRipples rings;
    CHECK(rings.live() == 0u);
    rings.add({10.f, 20.f}, 0.8f, 2.f, 1.f, 0.1f);
    CHECK(rings.live() == 1u);
    rings.advance(0.5f);
    WaterRing r{};
    std::size_t found = 0;
    for (std::size_t i = 0; i < kWaterRippleCount; ++i) {
        if (rings.ring(i).amplitude > 0.f) {
            r = rings.ring(i);
            ++found;
        }
    }
    REQUIRE(found == 1u);
    CHECK(r.p.x == doctest::Approx(10.f));
    CHECK(r.p.y == doctest::Approx(20.f));
    CHECK(r.radius == doctest::Approx(0.6f));
    CHECK(r.amplitude > 0.3f);
    CHECK(r.amplitude < 0.8f);
    const float early = r.amplitude;
    rings.advance(1.f);
    float late = 0.f;
    for (std::size_t i = 0; i < kWaterRippleCount; ++i) late = std::max(late, rings.ring(i).amplitude);
    CHECK(late > 0.f);
    CHECK(late < early);
    rings.advance(0.6f);
    CHECK(rings.live() == 0u);
    CHECK(rings.ring(99).amplitude == 0.f);
}

TEST_CASE("water ripples: bad input adds nothing; a full ring gives up its faintest") {
    WaterRipples rings;
    rings.add({0.f, 0.f}, 0.f, 1.f, 1.f);
    rings.add({0.f, 0.f}, 1.f, 0.f, 1.f);
    rings.add({std::nanf(""), 0.f}, 1.f, 1.f, 1.f);
    CHECK(rings.live() == 0u);
    // A splash, then more wake rings than the ring holds: the splash is still there.
    rings.add({5.f, 5.f}, 1.f, kWaterSplashLife, kWaterSplashSpeed);
    for (int i = 0; i < 20; ++i) {
        rings.advance(0.05f);
        rings.add({6.f + static_cast<float>(i), 5.f}, 0.5f, kWaterWakeLife, 0.5f);
    }
    CHECK(rings.live() == kWaterRippleCount);
    bool splash = false;
    bool newest = false;
    for (std::size_t i = 0; i < kWaterRippleCount; ++i) {
        const WaterRing r = rings.ring(i);
        splash = splash || (r.amplitude > 0.f && r.p.x == doctest::Approx(5.f));
        newest = newest || rings.ring(i).p.x == doctest::Approx(24.f);
    }
    CHECK(splash);
    rings.advance(0.07f);
    for (std::size_t i = 0; i < kWaterRippleCount; ++i) newest = newest || rings.ring(i).p.x == doctest::Approx(25.f);
    CHECK(newest);
}

TEST_CASE("water stir: splash on entry by fall speed, wake while moving, idle rings, leaving") {
    CHECK(waterSplashStrength(0.f) == doctest::Approx(kWaterSplashMin));
    CHECK(waterSplashStrength(kWaterSplashFall) == doctest::Approx(1.f));
    CHECK(waterSplashStrength(100.f) == doctest::Approx(1.f));
    CHECK(waterSplashStrength(3.f) > waterSplashStrength(1.f));
    WaterRipples rings;
    WaterStir stir;
    WaterState dry;
    WaterState wade;
    wade.mode = WaterMode::DeepWade;
    wade.immersion = 1.f;
    wade.surface = 2.5f;
    WaterStirEvents e = stir.step(rings, dry, {0.f, 0.f}, {1.f, 0.f}, 0.f, 0.033f);
    CHECK_FALSE(e.entered);
    CHECK_FALSE(e.moving);
    CHECK(rings.live() == 0u);
    e = stir.step(rings, wade, {0.f, 0.f}, {0.f, 0.f}, 5.f, 0.033f);
    CHECK(e.entered);
    CHECK(e.splash == doctest::Approx(waterSplashStrength(5.f)));
    CHECK(stir.inWater());
    CHECK(rings.live() == 2u);
    // Moving at 1.2 m/s for a second: a ring about every 0.15 s, none twice in a step.
    int fed = 0;
    for (int i = 0; i < 30; ++i) {
        rings.advance(1.f / 30.f);
        e = stir.step(rings, wade, {0.04f * static_cast<float>(i), 0.f}, {1.2f, 0.f}, 0.f, 1.f / 30.f);
        CHECK_FALSE(e.entered);
        CHECK(e.moving);
        CHECK(e.speed == doctest::Approx(1.2f));
        fed += e.fed ? 1 : 0;
    }
    CHECK(fed >= 5);
    CHECK(fed <= 7);
    // The wake's rings grow slower than the body moves, so their edge is a V.
    CHECK(rings.live() <= kWaterRippleCount);
    // Standing: a faint ring now and then.
    fed = 0;
    for (int i = 0; i < 90; ++i) {
        rings.advance(1.f / 30.f);
        e = stir.step(rings, wade, {1.2f, 0.f}, {0.05f, 0.f}, 0.f, 1.f / 30.f);
        CHECK_FALSE(e.moving);
        fed += e.fed ? 1 : 0;
    }
    CHECK(fed >= 2);
    CHECK(fed <= 3);
    e = stir.step(rings, dry, {1.2f, 0.f}, {0.f, 0.f}, 0.f, 1.f / 30.f);
    CHECK(e.left);
    CHECK_FALSE(stir.inWater());
    e = stir.step(rings, dry, {1.2f, 0.f}, {0.f, 0.f}, 0.f, 1.f / 30.f);
    CHECK_FALSE(e.left);
}

TEST_CASE("water wetness: soaked to the line, dries in about 90 s, drips a few seconds") {
    WaterWetness wet;
    wet.step(0.f, 1.8f, 1.f);
    CHECK(wet.wetness == 0.f);
    wet.step(1.1f, 1.8f, 0.1f);
    CHECK(wet.wetness == doctest::Approx(1.f));
    CHECK(wet.line == doctest::Approx(1.1f));
    CHECK(wet.drip == doctest::Approx(1.f));
    // Shallower water does not lower a line that is still wet.
    wet.step(0.4f, 1.8f, 0.1f);
    CHECK(wet.line == doctest::Approx(1.1f));
    // A full dunk carries it over the head, no further.
    wet.step(5.f, 1.8f, 0.1f);
    CHECK(wet.line == doctest::Approx(2.0f));
    // Out: drips end in kWaterDripTime, the wetness in kWaterDryTime.
    for (int i = 0; i < 30; ++i) wet.step(0.f, 1.8f, 0.1f);
    CHECK(wet.drip == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(wet.wetness == doctest::Approx(1.f - 3.f / kWaterDryTime).epsilon(0.001));
    for (int i = 0; i < 40; ++i) wet.step(0.f, 1.8f, 0.1f);
    CHECK(wet.drip == 0.f);
    CHECK(wet.line == doctest::Approx(2.0f));
    for (int i = 0; i < 430; ++i) wet.step(0.f, 1.8f, 0.1f);
    CHECK(wet.wetness > 0.4f);
    CHECK(wet.wetness < 0.5f);
    // Mostly dry: stepping into the shallows wets to the new line only.
    wet.step(0.3f, 1.8f, 0.1f);
    CHECK(wet.wetness == doctest::Approx(1.f));
    CHECK(wet.line == doctest::Approx(0.3f));
    // Rain: no drying. Sun and wind: faster.
    wet.step(0.f, 1.8f, 30.f, 0.f);
    CHECK(wet.wetness == doctest::Approx(1.f));
    wet.step(0.f, 1.8f, 30.f, 2.f);
    CHECK(wet.wetness == doctest::Approx(1.f - 60.f / kWaterDryTime));
    wet.step(0.f, 1.8f, 100.f);
    CHECK(wet.wetness == 0.f);
    CHECK(wet.line == 0.f);
}

TEST_CASE("water ripples, stir and wetness: no heap") {
    WaterRipples rings;
    WaterStir stir;
    WaterWetness wet;
    WaterState wade;
    wade.mode = WaterMode::Wade;
    wade.immersion = 0.5f;
    const auto before = ftcore_test::allocStats();
    float sum = 0.f;
    for (int i = 0; i < 300; ++i) {
        const float x = 0.03f * static_cast<float>(i);
        sum += stir.step(rings, wade, {x, 0.f}, {0.9f, 0.f}, 2.f, 1.f / 30.f).speed;
        rings.advance(1.f / 30.f);
        wet.step(i < 200 ? 0.5f : 0.f, 1.8f, 1.f / 30.f);
        for (std::size_t k = 0; k < kWaterRippleCount; ++k) sum += rings.ring(k).amplitude;
        sum += static_cast<float>(rings.live()) + wet.wetness;
    }
    const auto after = ftcore_test::allocStats();
    CHECK(after.count == before.count);
    CHECK(sum > 0.f);
}
