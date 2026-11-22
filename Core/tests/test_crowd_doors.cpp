// S-275 doors and S-283 street density for the crowd: the door table, doors on streets, going in and out, the
// streets' share by time of day, runners taking a door away from a fight.
#include "ftcore/CrowdV2.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace ftcore;
using namespace ftcore::crowdv2;

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

TEST_CASE("crowd doors: at a walk-in room a walker goes in through the doorway to a spot, stays in sight, and comes back out") {
    CrowdGraph g;
    REQUIRE(g.build({line({-100.f, 0.f}, {100.f, 0.f})}, loose()));
    CrowdTuning t = quiet();
    t.doorEnterChance = 1.f;
    t.doorDwellMin = t.doorDwellMax = 6.f;
    CrowdSim sim;
    REQUIRE(sim.init(g, t, 3));
    sim.setPopulationFrozen(true);
    CrowdDoor room{"room", {0.f, 5.f}, {0.f, 10.f}};
    room.through = {0.f, 6.5f};
    room.dwell = {{-1.f, 9.f}, {1.f, 9.f}};
    REQUIRE(sim.setDoors({room}) == 1);
    const std::uint32_t w = g.nearest({-100.f, 0.f}, 1.f), e = g.nearest({100.f, 0.f}, 1.f);
    const std::uint32_t a = sim.spawnAt(w, edgeTo(g, w, e), 0.5f, 80.f);
    REQUIRE(a != UINT32_MAX);
    bool hidden = false, passedDoorway = false;
    float atSpotFor = 0.f, firstIn = -1.f, lastIn = -1.f;
    // The view's phases: a room visit runs 1, 4, 5, 6, 7, 8, 3 (by rank, never back); seen, as bits.
    const int rank[9] = {0, 1, -1, 7, 2, 3, 4, 5, 6};
    int lastRank = 0;
    std::uint8_t phaseSeen = 0;
    CHECK(sim.doorCount() == 1);
    CHECK(dist(sim.doorAt(0), room.at) < 1e-6f);
    for (int s = 0; s < 15 * 50; ++s) {
        tick(sim);
        const CrowdAgentView& v = sim.agent(a);
        const float now = static_cast<float>(s + 1) / 15.f;
        if (v.state != CrowdState::Indoors) {
            CHECK(v.door == -1);
            CHECK(v.doorPhase == 0);
            continue;
        }
        CHECK(v.door == 0);
        REQUIRE(v.doorPhase <= 8);
        CHECK(rank[v.doorPhase] >= lastRank); // never back to an earlier phase in one visit
        lastRank = rank[v.doorPhase];
        if (v.doorPhase >= 1 && v.doorPhase <= 8) phaseSeen |= static_cast<std::uint8_t>(1u << (v.doorPhase - 1));
        if (v.doorPhase == 6) CHECK((dist(v.pos, room.dwell[0]) < 1e-3f || dist(v.pos, room.dwell[1]) < 1e-3f));
        if (firstIn < 0.f) firstIn = now;
        lastIn = now;
        hidden = hidden || v.indoors;
        passedDoorway = passedDoorway || dist(v.pos, room.through) < 0.05f;
        const bool atSpot = dist(v.pos, room.dwell[0]) < 1e-3f || dist(v.pos, room.dwell[1]) < 1e-3f;
        if (atSpot) {
            atSpotFor += 1.f / 15.f;
            CHECK(v.anim == CrowdAnim::Idle);
            CHECK(sim.indoorCount() == 1);
        }
    }
    REQUIRE(firstIn > 0.f);
    CHECK_FALSE(hidden);        // never hidden at the door: walks in and stands in sight
    CHECK((phaseSeen & (1u << 5)) != 0); // the view showed it standing at its spot (phase 6)
    CHECK((phaseSeen & (1u << 1)) == 0); // and never the plain door's hidden phase
    CHECK(sim.doorAt(7).x == 0.f);       // a slot past the end reads as the origin
    CHECK(passedDoorway);
    CHECK(atSpotFor >= 3.f - 0.1f); // the 6 s dwell, run at twice the pace while the streets hold under their share
    CHECK(lastIn - firstIn < 6.f + 14.f);
    CHECK(sim.agent(a).state == CrowdState::Walk);
    CHECK(sim.indoorCount() == 0);
    CHECK(sim.counts().wentIn == 1);
    CHECK(sim.counts().cameOut == 1);
    CHECK(sim.wallClearance(a) >= t.graph.wallInset - 0.05f);
}

TEST_CASE("crowd doors: a walk-in room's spots are never shared; a full room is passed by; a plain door still hides") {
    CrowdGraph g;
    REQUIRE(g.build({line({-100.f, 0.f}, {100.f, 0.f})}, loose()));
    CrowdTuning t = quiet();
    t.doorEnterChance = 1.f;
    t.doorDwellMin = t.doorDwellMax = 200.f; // still in when b comes by
    CrowdSim sim;
    REQUIRE(sim.init(g, t, 9));
    sim.setPopulationFrozen(true);
    CrowdDoor room{"room", {0.f, 5.f}, {0.f, 10.f}};
    room.through = {0.f, 6.5f};
    room.dwell = {{0.f, 9.f}}; // one spot
    CrowdDoor plain{"plain", {20.f, 5.f}, {20.f, 9.f}};
    REQUIRE(sim.setDoors({room, plain}) == 2);
    const std::uint32_t w = g.nearest({-100.f, 0.f}, 1.f), e = g.nearest({100.f, 0.f}, 1.f);
    const std::uint32_t a = sim.spawnAt(w, edgeTo(g, w, e), 0.5f, 90.f);
    const std::uint32_t b = sim.spawnAt(w, edgeTo(g, w, e), 0.5f, 80.f); // 10 m behind
    REQUIRE(a != UINT32_MAX);
    REQUIRE(b != UINT32_MAX);
    for (int s = 0; s < 15 * 45; ++s) tick(sim);
    // a took the room's one spot; b passed it by and went in at the plain door, hidden there.
    CHECK(dist(sim.agent(a).pos, room.dwell[0]) < 1e-3f);
    CHECK_FALSE(sim.agent(a).indoors);
    CHECK(sim.agent(b).state == CrowdState::Indoors);
    CHECK(sim.agent(b).indoors);
    CHECK(dist(sim.agent(b).pos, plain.at) < 1e-3f);
    CHECK(sim.counts().wentIn == 2);
}

TEST_CASE("crowd doors: a walk-in doorway takes one way at a time; two visitors never walk through each other or lock") {
    int together = 0, passedBy = 0;
    for (int off = 50; off <= 78; off += 2) {
        CrowdGraph g;
        REQUIRE(g.build({line({-100.f, 0.f}, {100.f, 0.f})}, loose()));
        CrowdTuning t = quiet();
        t.doorEnterChance = 1.f;
        t.doorDwellMin = t.doorDwellMax = 6.f;
        CrowdSim sim;
        REQUIRE(sim.init(g, t, 5));
        sim.setPopulationFrozen(true);
        CrowdDoor room{"room", {0.f, 5.f}, {0.f, 10.f}};
        room.through = {0.f, 6.5f};
        room.dwell = {{-1.f, 9.f}, {1.f, 9.f}};
        REQUIRE(sim.setDoors({room}) == 1);
        const std::uint32_t w = g.nearest({-100.f, 0.f}, 1.f), e = g.nearest({100.f, 0.f}, 1.f);
        const std::uint32_t a = sim.spawnAt(w, edgeTo(g, w, e), 0.5f, 80.f);
        const std::uint32_t b = sim.spawnAt(w, edgeTo(g, w, e), 0.5f, static_cast<float>(off));
        REQUIRE(a != UINT32_MAX);
        REQUIRE(b != UINT32_MAX);
        float closest = 1e9f;
        bool both = false;
        for (int s = 0; s < 15 * 70; ++s) {
            tick(sim);
            const CrowdAgentView &va = sim.agent(a), &vb = sim.agent(b);
            if (va.state != CrowdState::Indoors || vb.state != CrowdState::Indoors) continue;
            both = true;
            closest = std::min(closest, dist(va.pos, vb.pos));
        }
        CAPTURE(off);
        CHECK(closest >= 0.6f);
        CHECK(sim.agent(a).state == CrowdState::Walk); // nobody left waiting inside
        CHECK(sim.agent(b).state == CrowdState::Walk);
        CHECK(sim.indoorCount() == 0);
        CHECK(sim.counts().wentIn == sim.counts().cameOut);
        together += both ? 1 : 0;
        passedBy += sim.counts().wentIn == 1 ? 1 : 0;
    }
    CHECK(together > 0); // the sweep holds cases with both in the room at once
    CHECK(passedBy > 0); // and cases where the second walked past a busy doorway
}

TEST_CASE("crowd doors: in a deep walk-in room only one visitor moves at a time and none walks through one standing") {
    // Round 4 faults A and B: mixed walk speeds and runners from a fight (one could catch a slower one up on a long
    // way in), and spots in line behind each other (the way to the far one runs through the near one).
    int together = 0, fled = 0, farSpot = 0;
    for (std::uint32_t seed = 1; seed <= 8; ++seed) {
        for (int off = 62; off <= 74; off += 4) {
            const bool fight = seed % 2 == 0;
            CrowdGraph g;
            REQUIRE(g.build({line({-100.f, 0.f}, {100.f, 0.f})}, loose()));
            CrowdTuning t = quiet();
            t.walkSpeedJitter = 0.45f;
            t.doorEnterChance = 1.f;
            t.doorDwellMin = 4.f;
            t.doorDwellMax = 9.f;
            if (fight) {
                t.reactRadius = 40.f;
                t.doorFleeAhead = 100.f;
            }
            CrowdSim sim;
            REQUIRE(sim.init(g, t, seed));
            sim.setPopulationFrozen(true);
            CrowdDoor room{"room", {0.f, 5.f}, {0.f, 16.f}};
            room.through = {0.f, 6.5f};
            room.dwell = {{0.f, 10.f}, {0.f, 14.f}, {-2.f, 12.f}, {2.f, 12.f}}; // 0 and 1 in line from the doorway
            REQUIRE(sim.setDoors({room}) == 1);
            const std::uint32_t w = g.nearest({-100.f, 0.f}, 1.f), e = g.nearest({100.f, 0.f}, 1.f);
            const std::uint32_t ids[3] = {sim.spawnAt(w, edgeTo(g, w, e), 0.5f, 80.f),
                                          sim.spawnAt(w, edgeTo(g, w, e), 0.5f, static_cast<float>(off)),
                                          sim.spawnAt(w, edgeTo(g, w, e), 0.5f, static_cast<float>(off) - 7.f)};
            for (std::uint32_t id : ids) REQUIRE(id != UINT32_MAX);
            float closest = 1e9f;
            int movingMax = 0;
            bool both = false;
            for (int s = 0; s < 15 * 240; ++s) {
                if (fight && s == 15 * 2) sim.pushEvent({CrowdEventKind::FightStart, {-60.f, 0.f}});
                tick(sim);
                int inRoom = 0, moving = 0;
                for (int i = 0; i < 3; ++i) {
                    const CrowdAgentView& vi = sim.agent(ids[i]);
                    if (vi.state != CrowdState::Indoors || vi.pos.y <= room.at.y + 0.05f) continue;
                    ++inRoom;
                    if (vi.speed > 0.f) ++moving;
                    farSpot += dist(vi.pos, room.dwell[1]) < 1e-3f ? 1 : 0;
                    for (int j = i + 1; j < 3; ++j) {
                        const CrowdAgentView& vj = sim.agent(ids[j]);
                        if (vj.state != CrowdState::Indoors || vj.pos.y <= room.at.y + 0.05f) continue;
                        closest = std::min(closest, dist(vi.pos, vj.pos));
                    }
                }
                movingMax = std::max(movingMax, moving);
                both = both || inRoom >= 2;
            }
            CAPTURE(seed);
            CAPTURE(off);
            CHECK(movingMax <= 1);             // one on the move inside at a time: nobody catches another up
            CHECK(closest >= t.minGap - 1e-3f); // never through, or up against, one standing at its spot
            CHECK(sim.indoorCount() == 0);      // nobody left waiting inside: it never locks
            CHECK(sim.counts().wentIn + sim.counts().fledIn == sim.counts().cameOut);
            together += both ? 1 : 0;
            fled += static_cast<int>(sim.counts().fledIn);
        }
    }
    CHECK(together > 0); // the sweep holds visitors in the room together
    CHECK(fled > 0);     // and runners taking the room from a fight
    CHECK(farSpot > 0);  // the far spot is used (only while the near one is free)
}
