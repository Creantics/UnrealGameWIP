// S-275 doors and S-283 street density for the crowd: the door table, doors on streets, going in and out, the
// streets' share by time of day, runners taking a door away from a fight.
#include "ftcore/Crowd.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

float dist(Vec2 a, Vec2 b) { return std::hypot(b.x - a.x, b.y - a.y); }

CrowdStreet line(Vec2 a, Vec2 b, float width = 8.f) {
    CrowdStreet s;
    s.kind = "street";
    s.width = width;
    s.pavement = 2.f;
    s.points = {a, b};
    return s;
}

CrowdGraphTuning loose() {
    CrowdGraphTuning t;
    t.minComponent = 1;
    return t;
}

CrowdTuning quiet() {
    CrowdTuning t;
    t.reactJitter = 0.f;
    t.idleChance = 0.f;
    t.walkSpeedJitter = 0.f;
    t.headingChange = 0.f;
    t.reactRadius = 0.f;
    return t;
}

std::uint32_t edgeTo(const CrowdGraph& g, std::uint32_t n, std::uint32_t m) {
    for (std::uint32_t k = 0; k < g.degree(n); ++k)
        if (g.edge(n, k).to == m) return k;
    return 0;
}

void tick(CrowdSim& sim, Vec2 player = {0.f, 0.f}) { sim.tick(1.f / 15.f, player, {0.f, 0.f}); }

std::vector<CrowdStreet> grid(int n, float spacing) {
    std::vector<CrowdStreet> out;
    const float o = -0.5f * spacing * static_cast<float>(n - 1);
    for (int r = 0; r < n; ++r) {
        CrowdStreet h, v;
        h.kind = v.kind = "street";
        h.width = v.width = 8.f;
        h.pavement = v.pavement = 2.f;
        for (int c = 0; c < n; ++c) {
            h.points.push_back({o + spacing * static_cast<float>(c), o + spacing * static_cast<float>(r)});
            v.points.push_back({o + spacing * static_cast<float>(r), o + spacing * static_cast<float>(c)});
        }
        out.push_back(h);
        out.push_back(v);
    }
    return out;
}

} // namespace

TEST_CASE("crowd doors: the door table reads doors and rooms, moved to the engine frame") {
    const std::string csv =
        "# a comment\n"
        "id,kind,width_m,pts,pavement_m\n"
        "DOOR-A,door,1.6,510:490:3;512:490:3,\n"
        "ROOM-A,interior_shop,0,510:486:3;514:486:3;514:480:3;510:480:3,\n"
        "DOOR-B,door,1.2,600:400:0;600:402:0,\n"
        "STREET-X,street,6,1:1:0;2:2:0,\n";
    std::vector<CrowdDoor> doors;
    std::string err;
    REQUIRE_MESSAGE(importCrowdDoors(csv, 1000.0, doors, &err), err);
    REQUIRE(doors.size() == 2);
    CHECK(doors[0].id == "A");
    CHECK(doors[0].at.x == doctest::Approx(11.f));
    CHECK(doors[0].at.y == doctest::Approx(10.f));
    CHECK(doors[0].inside.x == doctest::Approx(12.f));
    CHECK(doors[0].inside.y == doctest::Approx(17.f));
    CHECK(doors[1].inside.x == doctest::Approx(doors[1].at.x)); // no room row: the door itself
    // side 0: the points are already in the engine frame
    REQUIRE(importCrowdDoors(csv, 0.0, doors, &err));
    CHECK(doors[0].at.x == doctest::Approx(511.f));

    CHECK_FALSE(importCrowdDoors("id,kind,width_m,pts\nDOOR-A,door,1,1:1:0;2:2:0;3:3:0\n", 0.0, doors, &err)); // 3 ends
    CHECK_FALSE(importCrowdDoors("id,kind,width_m,pts\nDOOR-A,door,1,1:x:0;2:2:0\n", 0.0, doors, &err));
    CHECK_FALSE(importCrowdDoors("id,kind,width_m,pts\nROOM-A,room,0,1:1:0;2:2:0\n", 0.0, doors, &err)); // no doors
    CHECK_FALSE(importCrowdDoors("id,kind\nDOOR-A,door\n", 0.0, doors, &err));                         // no pts column
}

TEST_CASE("crowd doors: a door attaches to the street it opens onto; one too far off is left out") {
    CrowdGraph g;
    REQUIRE(g.build({line({-100.f, 0.f}, {100.f, 0.f})}, loose()));
    CrowdSim sim;
    REQUIRE(sim.init(g, quiet(), 1));
    std::vector<CrowdDoor> doors{{"near", {10.f, 5.f}, {10.f, 9.f}}, {"far", {10.f, 40.f}, {10.f, 44.f}}};
    CHECK(sim.setDoors(doors) == 1);
    CHECK(sim.doorCount() == 1);
    REQUIRE(sim.init(g, quiet(), 1)); // init forgets them
    CHECK(sim.doorCount() == 0);
}

TEST_CASE("crowd doors: a calm walker goes in, stays inside its dwell, comes out onto its pavement and walks on") {
    CrowdGraph g;
    REQUIRE(g.build({line({-100.f, 0.f}, {100.f, 0.f})}, loose()));
    CrowdTuning t = quiet();
    t.doorEnterChance = 1.f;
    t.doorDwellMin = t.doorDwellMax = 6.f;
    CrowdSim sim;
    REQUIRE(sim.init(g, t, 3));
    sim.setPopulationFrozen(true);
    const Vec2 door{0.f, 5.f};
    REQUIRE(sim.setDoors({{"d", door, {0.f, 9.f}}}) == 1);
    const std::uint32_t w = g.nearest({-100.f, 0.f}, 1.f), e = g.nearest({100.f, 0.f}, 1.f);
    const std::uint32_t a = sim.spawnAt(w, edgeTo(g, w, e), 0.5f, 80.f); // 20 m short of the door, walking to it
    REQUIRE(a != UINT32_MAX);
    // Out to the end of the dwell: in by about 20 m / 1.3 m/s plus the few metres to the door.
    float firstIn = -1.f, lastIn = -1.f;
    bool sawHidden = false, drawnWhileHidden = false;
    for (int s = 0; s < 15 * 45; ++s) {
        tick(sim);
        const CrowdAgentView& v = sim.agent(a);
        const float now = static_cast<float>(s + 1) / 15.f;
        if (v.state == CrowdState::Indoors) {
            if (firstIn < 0.f) firstIn = now;
            lastIn = now;
        }
        if (v.indoors) {
            sawHidden = true;
            drawnWhileHidden = drawnWhileHidden || v.drawn;
            CHECK(dist(v.pos, door) < 1e-3f);
            CHECK(sim.indoorCount() == 1);
            CHECK(sim.wallClearance(a) > 100.f);
        }
    }
    REQUIRE(firstIn > 0.f);
    CHECK(firstIn < 20.f);
    CHECK(sawHidden);
    CHECK_FALSE(drawnWhileHidden);
    CHECK(lastIn - firstIn >= 6.f);           // the dwell
    CHECK(lastIn - firstIn < 6.f + 12.f);     // plus the walks in and out
    CHECK(sim.agent(a).state == CrowdState::Walk);
    CHECK(sim.indoorCount() == 0);
    CHECK(sim.counts().wentIn == 1);
    CHECK(sim.counts().cameOut == 1);
    CHECK(sim.counts().resumed == 0); // coming out isn't a recovery from a scare
    CHECK(sim.wallClearance(a) >= t.graph.wallInset - 0.05f);
}

TEST_CASE("crowd doors: a runner takes a door ahead of it, away from the fight, and reads as moving away") {
    CrowdGraph g;
    REQUIRE(g.build({line({-100.f, 0.f}, {100.f, 0.f})}, loose()));
    CrowdTuning t = quiet();
    t.doorEnterChance = 0.f;
    t.reactRadius = 6.f; // close fight: runs at once
    CrowdSim sim;
    REQUIRE(sim.init(g, t, 5));
    sim.setPopulationFrozen(true);
    // A door 8 m behind the member (towards the fight) and one 5 m ahead of it.
    REQUIRE(sim.setDoors({{"behind", {-8.f, 5.f}, {-8.f, 9.f}}, {"ahead", {5.f, 5.f}, {5.f, 9.f}}}) == 2);
    const std::uint32_t w = g.nearest({-100.f, 0.f}, 1.f), e = g.nearest({100.f, 0.f}, 1.f);
    const std::uint32_t a = sim.spawnAt(w, edgeTo(g, w, e), 0.5f, 100.f); // at x = 0
    REQUIRE(a != UINT32_MAX);
    for (int s = 0; s < 15; ++s) tick(sim);
    const Vec2 fight{-5.f, 0.f};
    CrowdReactionProbe probe;
    probe.begin(sim, fight, 12.f);
    sim.pushEvent({CrowdEventKind::FightStart, fight});
    float closest = 1e30f;
    for (int s = 0; s < 15; ++s) {
        tick(sim);
        closest = std::min(closest, dist(sim.agent(a).pos, fight));
    }
    const CrowdReactionResult r = probe.evaluate(sim, 1.5f, 0.3f);
    CHECK(r.toward == 0);
    CHECK(r.ok());
    for (int s = 0; s < 15 * 3; ++s) tick(sim);
    CHECK(sim.counts().fledIn == 1);
    CHECK(sim.agent(a).indoors);
    CHECK(sim.agent(a).pos.x == doctest::Approx(5.f)); // the door ahead, never the one by the fight
    CHECK(closest >= 5.f - 0.05f);
}

TEST_CASE("crowd density: the streets empty into doors towards evening and refill in the morning, allocation free") {
    CrowdGraph g;
    REQUIRE(g.build(grid(20, 15.f), loose())); // 285 m across
    CrowdTuning t;                             // the shipped defaults, 300 target
    t.targetAgents = 200;
    CrowdSim sim;
    REQUIRE(sim.init(g, t, 11));
    // A door on every street between two crossings, 5 m off its centre line.
    std::vector<CrowdDoor> doors;
    for (std::uint32_t n = 0; n < g.nodeCount(); ++n)
        for (std::uint32_t k = 0; k < g.degree(n); ++k) {
            const std::uint32_t m = g.edge(n, k).to;
            if (m < n) continue;
            const Vec2 p = g.node(n), q = g.node(m);
            const Vec2 mid{(p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f};
            const bool across = std::fabs(q.x - p.x) > std::fabs(q.y - p.y);
            doors.push_back({"d", {mid.x + (across ? 0.f : 5.f), mid.y + (across ? 5.f : 0.f)}, mid});
        }
    REQUIRE(sim.setDoors(doors) == doors.size());
    for (int s = 0; s < 15 * 20; ++s) tick(sim);
    const std::uint32_t day = sim.liveCount() - sim.indoorCount();
    CHECK(day >= 190);
    CHECK(sim.counts().wentIn > 0); // calm visits by day too

    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    sim.setDensity(0.3f); // evening
    float minPair = 1e30f;
    for (int s = 0; s < 15 * 150; ++s) {
        tick(sim);
        if (s % 15 == 0) minPair = std::min(minPair, sim.minPairDistance(2.f));
    }
    if (counting) CHECK(ftcore_test::allocStats().count == before);
    const std::uint32_t evening = sim.liveCount() - sim.indoorCount();
    CHECK(evening < day * 7 / 10);
    CHECK(sim.indoorCount() > 0);
    CHECK(minPair >= t.minGap - 0.05f); // members going in never stand in each other

    sim.setDensity(1.f); // morning
    const auto out0 = sim.counts().cameOut;
    for (int s = 0; s < 15 * 60; ++s) tick(sim);
    CHECK(sim.counts().cameOut > out0);
    CHECK(sim.liveCount() - sim.indoorCount() >= 190);
}
