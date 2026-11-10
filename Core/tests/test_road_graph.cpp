// The road graph (ftcore/RoadGraph.h): lanes, junctions and phases, the CSV round trip and the drivable loop.
// Made-up streets only.

#include "ftcore/RoadGraph.h"

#include <ostream>
#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

MassingStreet street(const char* id, double w, std::vector<MassingVec> pts) {
    MassingStreet s;
    s.id = id;
    s.width = w;
    s.points = std::move(pts);
    return s;
}

} // namespace

TEST_CASE("road graph: a two-way street gets one lane each way on its left") {
    RoadGraph g;
    MassingStreet s = street("ST-1", 14.0, {{0, 0, 0}, {100, 0, 0}});
    s.pavement = 3.5;
    roadGraphStreetLanes(s, 40.0, false, 0.2, g);
    REQUIRE(g.lanes.size() == 2);
    CHECK(g.lanes[0].dir == 1);
    CHECK(g.lanes[0].width == doctest::Approx(3.5));
    // Left-hand traffic: eastbound keeps to the north (its left).
    CHECK(g.lanes[0].pts.front().y == doctest::Approx(1.75));
    CHECK(g.lanes[0].pts.back().x == doctest::Approx(100.0));
    CHECK(g.lanes[1].dir == -1);
    CHECK(g.lanes[1].pts.front().x == doctest::Approx(100.0));
    CHECK(g.lanes[1].pts.front().y == doctest::Approx(-1.75));
    // An 18 m street with no pavements: 0.2 each side leaves 10.8 m, one lane each way of 3.5 m.
    RoadGraph g2;
    roadGraphStreetLanes(street("ST-2", 18.0, {{0, 0, 0}, {0, 50, 0}, {50, 50, 0}}), 50.0, false, 0.2, g2);
    REQUIRE(g2.lanes.size() == 2);
    // The bend is mitred: the northbound lane's corner sits 1.75 m off both legs.
    CHECK(g2.lanes[0].pts[1].x == doctest::Approx(-1.75));
    CHECK(g2.lanes[0].pts[1].y == doctest::Approx(51.75));
    // A one-way 10 m street with 3 m pavements: one lane along its points.
    RoadGraph g3;
    MassingStreet ow = street("ST-3", 10.0, {{0, 0, 0}, {30, 0, 0}});
    ow.pavement = 3.0;
    roadGraphStreetLanes(ow, 30.0, true, 0.2, g3);
    REQUIRE(g3.lanes.size() == 1);
    CHECK(g3.lanes[0].oneWay);
    CHECK(g3.lanes[0].pts[0].y == doctest::Approx(0.0));
}

TEST_CASE("road graph: junctions at shared ends, Ts and crossings, two phases at three arms or more") {
    std::vector<MassingStreet> s = {
        street("A", 14.0, {{0, 0, 0}, {100, 0, 0}}),      // east-west
        street("B", 14.0, {{50, -50, 0}, {50, 50, 0}}),   // crosses A at (50, 0)
        street("C", 10.0, {{100, 1.5, 0}, {100, 60, 0}}), // its end within 2 m of A's end: a corner
        street("D", 10.0, {{20, 1.0, 0}, {20, 40, 0}}),   // a T onto A
    };
    RoadGraph g;
    roadGraphJunctions(s, kRoadGraphSnap, 0.2, g);
    REQUIRE(g.junctions.size() == 3);
    int signals = 0;
    for (const RoadGraphJunction& j : g.junctions) {
        if (!j.phases.empty()) ++signals;
        if (std::fabs(j.at.x - 50.0) < 1e-6) {
            CHECK(j.streets.size() == 2);
            CHECK(j.phases == "A:A;B:B");
        }
        if (std::fabs(j.at.x - 100.0) < 1e-6) CHECK(j.phases.empty()); // two arms
    }
    CHECK(signals == 2);
    // Four zebras at the crossing, three at the T.
    CHECK(g.crossings.size() == 7);
}

TEST_CASE("road graph: the CSV round trip keeps every row") {
    RoadGraph g;
    MassingStreet s = street("ST-1", 14.0, {{0, 0, 0}, {100, 0, 0}});
    s.pavement = 3.5;
    roadGraphStreetLanes(s, 40.0, false, 0.2, g);
    RoadGraphJunction j;
    j.id = "JN-1";
    j.at = {50, 0};
    j.streets = {"ST-1", "CL-048"};
    j.phases = "A:ST-1;B:CL-048";
    g.junctions.push_back(j);
    RoadGraphCrossing c;
    c.id = "Z-1";
    c.street = "ST-1";
    c.junction = "JN-1";
    c.pts = {RoadPoint{40, -3.5}, RoadPoint{44, -3.5}, RoadPoint{44, 3.5}, RoadPoint{40, 3.5}};
    g.crossings.push_back(c);
    RoadGraphBay b;
    b.id = "BAY-1";
    b.street = "ST-1";
    b.dir = -1;
    b.width = 2.0;
    b.pts = {RoadPoint{10, -5.5}, RoadPoint{16, -5.5}, RoadPoint{16, -3.5}, RoadPoint{10, -3.5}};
    g.bays.push_back(b);
    const std::string text = roadGraphWrite(g);
    CHECK(text.rfind("# Road graph", 0) == 0);
    RoadGraph r;
    std::string err;
    REQUIRE_MESSAGE(importRoadGraph(text, r, &err), err);
    REQUIRE(r.lanes.size() == 2);
    CHECK(r.lanes[1].dir == -1);
    CHECK(r.lanes[1].speedKmh == doctest::Approx(40.0));
    CHECK(r.lanes[0].pts.size() == 2);
    REQUIRE(r.junctions.size() == 1);
    CHECK(r.junctions[0].streets.size() == 2);
    CHECK(r.junctions[0].phases == "A:ST-1;B:CL-048");
    REQUIRE(r.crossings.size() == 1);
    CHECK(r.crossings[0].pts[2].y == doctest::Approx(3.5));
    CHECK(r.crossings[0].junction == "JN-1");
    REQUIRE(r.bays.size() == 1);
    CHECK(r.bays[0].dir == -1);
    CHECK(roadGraphWrite(r) == text);
    CHECK(!importRoadGraph("type,id\n", r, &err));
    CHECK(!importRoadGraph(std::string(roadGraphHeader()) + "type,id,street,dir,width_m,speed_kmh,one_way,pts,refs,phases\n"
                                                           "lane,L1,ST,2,3.5,40,0,0:0;1:0,,\n",
                           r, &err));
    CHECK(err.find("line") != std::string::npos);
}

TEST_CASE("road graph: the loop of a grid is its outer ring; a spur and an island are left out") {
    std::vector<MassingStreet> s = {
        street("S", 14.0, {{0, 0, 0}, {200, 0, 0}}),
        street("E", 14.0, {{200, 0, 0}, {200, 100, 0}}),
        street("N", 14.0, {{200, 100, 0}, {0, 100, 0}}),
        street("W", 14.0, {{0, 100, 0}, {0, 0, 0}}),
        street("M", 10.0, {{100, 0, 0}, {100, 100, 0}}),   // splits the ring in two
        street("P", 10.0, {{200, 50, 0}, {260, 50, 0}}),   // a spur: a bridge
        street("I", 10.0, {{500, 0, 0}, {520, 0, 0}}),     // an island
    };
    const RoadLoop loop = roadGraphLoop(s, kRoadGraphSnap);
    CHECK(loop.length == doctest::Approx(600.0));
    CHECK(loop.netLength == doctest::Approx(700.0));
    CHECK(loop.bridges == 2);
    CHECK(loop.ring.size() >= 4);
    // No loop at all: a single street.
    const RoadLoop none = roadGraphLoop({street("S", 14.0, {{0, 0, 0}, {200, 0, 0}})}, kRoadGraphSnap);
    CHECK(none.length == doctest::Approx(0.0));
}
