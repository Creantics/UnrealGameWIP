// A massing world's build checks and tour judging (ftcore/WorldChecks.h): the tour and coverage tables, the edge
// verdicts, the navmesh islands and their classes, and the Landscape read-back. Made-up tables only. Ids and numbers.

#include "ftcore/WorldChecks.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace ftcore;

namespace {

constexpr const char* kTourHeader = "id,kind,x,y,z,start_x,start_y,start_z,edge,still\n";

std::string tour(const std::string& rows) { return std::string("# tour stops\n") + kTourHeader + rows; }

// The error importTourStops gives for `csv`, or "" when it reads.
std::string tourError(const std::string& csv) {
    std::vector<TourStop> stops;
    std::string error;
    if (importTourStops(csv, stops, &error)) return "";
    return error;
}

std::string coverageError(const std::string& csv) {
    std::vector<CoveragePoint> points;
    std::string error;
    if (importCoveragePoints(csv, points, &error)) return "";
    return error;
}

bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

std::uint16_t raw(double metres) { return static_cast<std::uint16_t>(std::lround(metres * 128.0 + 32768.0)); }

// A unit square's corners, from (x, y), counter-clockwise, at height z.
void square(std::vector<MassingVec>& corners, std::vector<NavPolyRef>& polys, double x, double y, double s, double z) {
    polys.push_back({static_cast<std::uint32_t>(corners.size()), 4});
    corners.push_back({x, y, z});
    corners.push_back({x + s, y, z});
    corners.push_back({x + s, y + s, z});
    corners.push_back({x, y + s, z});
}

} // namespace

// ---- Tour stops ------------------------------------------------------------------------------------------------

TEST_CASE("world checks: a tour table reads in file order, comments and other columns skipped") {
    const std::string csv = "# a comment before the header\n"
                            "id,kind,x,y,z,start_x,start_y,start_z,edge,still,note\n"
                            "\"# a quoted comment\"\n"
                            "R1,reach,100,200.5,3.25,,,,,1,first\n"
                            "  # an indented comment\n"
                            "E-W,edge, -10 ,500,0,50,500,1.5,W,0,\"a, quoted, note\"\n"
                            "P_1,push,990,10,0,950,10,0.5,,0,\n";
    std::vector<TourStop> stops;
    std::string error;
    REQUIRE(importTourStops(csv, stops, &error));
    REQUIRE(stops.size() == 3);
    CHECK(stops[0].id == "R1");
    CHECK(stops[0].kind == TourStopKind::Reach);
    CHECK(near(stops[0].x, 100.0));
    CHECK(near(stops[0].y, 200.5));
    CHECK(near(stops[0].z, 3.25));
    CHECK_FALSE(stops[0].hasStart);
    CHECK(stops[0].edge == 0);
    CHECK(stops[0].still);
    CHECK(stops[1].id == "E-W");
    CHECK(stops[1].kind == TourStopKind::Edge);
    CHECK(near(stops[1].x, -10.0));
    CHECK(stops[1].hasStart);
    CHECK(near(stops[1].startX, 50.0));
    CHECK(near(stops[1].startY, 500.0));
    CHECK(near(stops[1].startZ, 1.5));
    CHECK(stops[1].edge == 'W');
    CHECK_FALSE(stops[1].still);
    CHECK(stops[2].kind == TourStopKind::Push);
    CHECK(stops[2].hasStart);
    CHECK(stops[2].edge == 0);
}

TEST_CASE("world checks: a tour table with a reach stop given a start, and a header only") {
    std::vector<TourStop> stops;
    REQUIRE(importTourStops(tour("A,reach,1,2,3,4,5,6,,0\n"), stops));
    REQUIRE(stops.size() == 1);
    CHECK(stops[0].hasStart);
    CHECK(near(stops[0].startZ, 6.0));
    REQUIRE(importTourStops(tour(""), stops));
    CHECK(stops.empty());
    // CRLF line ends and a byte-order mark read the same.
    REQUIRE(importTourStops("\xEF\xBB\xBFid,kind,x,y,z,start_x,start_y,start_z,edge,still\r\nA,reach,1,2,3,,,,,0\r\n",
                            stops));
    CHECK(stops.size() == 1);
}

TEST_CASE("world checks: every tour table fault names its line") {
    CHECK(tourError("# only a comment\n") == "line 1: no header row");
    CHECK(tourError("") == "line 1: no header row");
    CHECK(tourError("id,kind,x,y,z,start_x,start_y,start_z,edge\n") == "line 1: no column still");
    CHECK(tourError("# c\nid,kind,x,x,y,z,start_x,start_y,start_z,edge,still\n") == "line 2: column x twice");
    CHECK(tourError(tour("A,reach,1,2,3,,,,,0,extra\n")) == "line 3: 11 fields where the header has 10");
    CHECK(tourError(tour("a,reach,1,2,3,,,,,0\n")) == "line 3: id must be A-Z, 0-9, '_' or '-'");
    CHECK(tourError(tour(",reach,1,2,3,,,,,0\n")) == "line 3: id must be A-Z, 0-9, '_' or '-'");
    CHECK(tourError(tour("A,reach,1,2,3,,,,,0\nA,reach,1,2,3,,,,,0\n")) == "line 4: id A twice");
    CHECK(tourError(tour("A,walk,1,2,3,,,,,0\n")) == "line 3: A: kind must be reach, edge or push");
    CHECK(tourError(tour("A,Reach,1,2,3,,,,,0\n")) == "line 3: A: kind must be reach, edge or push");
    CHECK(tourError(tour("A,reach,one,2,3,,,,,0\n")) == "line 3: A: x is not a number");
    CHECK(tourError(tour("A,reach,1,2,,,,,,0\n")) == "line 3: A: z is not a number");
    CHECK(tourError(tour("A,push,1,2,,0,0,0,,0\n")) == "line 3: A: z is not a number");
    CHECK(tourError(tour("A,reach,1,2,3,4,,6,,0\n")) ==
          "line 3: A: start_x, start_y and start_z must be all empty or all numbers");
    CHECK(tourError(tour("A,reach,1,2,3,4,y,6,,0\n")) == "line 3: A: start_y is not a number");
    CHECK(tourError(tour("A,edge,1,2,3,,,,W,0\n")) == "line 3: A: an edge stop needs a start");
    CHECK(tourError(tour("A,push,1,2,3,,,,,0\n")) == "line 3: A: a push stop needs a start");
    CHECK(tourError(tour("A,edge,1,2,3,4,5,6,,0\n")) == "line 3: A: edge must be W, S, N or E on an edge stop");
    CHECK(tourError(tour("A,edge,1,2,3,4,5,6,w,0\n")) == "line 3: A: edge must be W, S, N or E on an edge stop");
    CHECK(tourError(tour("A,edge,1,2,3,4,5,6,NE,0\n")) == "line 3: A: edge must be W, S, N or E on an edge stop");
    CHECK(tourError(tour("A,reach,1,2,3,,,,W,0\n")) == "line 3: A: edge must be empty on a reach stop");
    CHECK(tourError(tour("A,push,1,2,3,4,5,6,E,0\n")) == "line 3: A: edge must be empty on a push stop");
    CHECK(tourError(tour("A,reach,1,2,3,,,,,2\n")) == "line 3: A: still must be 0 or 1");
    CHECK(tourError(tour("A,reach,1,2,3,,,,,\n")) == "line 3: A: still must be 0 or 1");
    CHECK(tourError(tour("A,reach,1,2,3,,,,,0\n\"open")) == "line 4: a quoted field is never closed");
    CHECK(tourError(tour("\"A\"x,reach,1,2,3,,,,,0\n")) == "line 3: text after a closing quote");
    // A fault leaves nothing behind.
    std::vector<TourStop> stops = {TourStop{}};
    CHECK_FALSE(importTourStops(tour("A,walk,1,2,3,,,,,0\n"), stops));
    CHECK(stops.empty());
}

TEST_CASE("world checks: a tour kinds list picks kinds") {
    std::uint8_t mask = 0;
    REQUIRE(parseTourKinds("", mask));
    CHECK(mask == kTourKindsAll);
    REQUIRE(parseTourKinds("edge", mask));
    CHECK(tourKindIn(mask, TourStopKind::Edge));
    CHECK_FALSE(tourKindIn(mask, TourStopKind::Reach));
    CHECK_FALSE(tourKindIn(mask, TourStopKind::Push));
    REQUIRE(parseTourKinds(" reach , push ", mask));
    CHECK(tourKindIn(mask, TourStopKind::Reach));
    CHECK(tourKindIn(mask, TourStopKind::Push));
    CHECK_FALSE(tourKindIn(mask, TourStopKind::Edge));
    REQUIRE(parseTourKinds("edge+push", mask));
    CHECK(mask == 6); // edge and push
    mask = 5;
    CHECK_FALSE(parseTourKinds("reach,walk", mask));
    CHECK(mask == 5);
    CHECK(std::string(tourStopKindName(TourStopKind::Reach)) == "reach");
    CHECK(std::string(tourStopKindName(TourStopKind::Edge)) == "edge");
    CHECK(std::string(tourStopKindName(TourStopKind::Push)) == "push");
}

TEST_CASE("world checks: a tour leg's cap scales with its straight distance, never under the floor") {
    CHECK(near(tourLegCapSeconds(0.0), kTourLegCapFloorSeconds));
    CHECK(near(tourLegCapSeconds(100.0), kTourLegCapFloorSeconds));
    CHECK(near(tourLegCapSeconds(150.0), kTourLegCapFloorSeconds));
    CHECK(near(tourLegCapSeconds(600.0), 1200.0));
    CHECK(near(tourLegCapSeconds(1400.0), 1400.0 * kTourLegSecondsPerMetre));
}

// ---- Edge verdicts ---------------------------------------------------------------------------------------------

TEST_CASE("world checks: an edge leg's cause") {
    EdgeProbe p;
    p.walkableNormalZ = 0.71;
    p.feetZ = 5.0;
    p.hasSea = true;
    p.seaLevel = 0.0;

    // A built actor is a built wall whatever lies past it.
    p.hit = EdgeHit::Built;
    CHECK(judgeEdge(p) == EdgeCause::BuiltWall);

    // The ground too steep to walk is a cliff; a walkable slope is judged by the ground ahead.
    p.hit = EdgeHit::Landscape;
    p.hitNormalZ = 0.5;
    CHECK(judgeEdge(p) == EdgeCause::Cliff);
    p.hitNormalZ = 0.9;
    p.groundFound = true;
    p.groundZ = 4.8;
    CHECK(judgeEdge(p) == EdgeCause::Unjudged);
    p.groundZ = 2.0;
    CHECK(judgeEdge(p) == EdgeCause::Drop);

    // A hidden wall: dry ground at the feet past it is bare; under the sea is water; a big drop is a drop.
    p.hit = EdgeHit::HiddenWall;
    p.groundZ = 5.6;
    CHECK(judgeEdge(p) == EdgeCause::BareHiddenWall);
    p.groundZ = 4.1;
    CHECK(judgeEdge(p) == EdgeCause::BareHiddenWall);
    p.groundZ = -0.5;
    CHECK(judgeEdge(p) == EdgeCause::Water);
    p.groundZ = 2.5;
    CHECK(judgeEdge(p) == EdgeCause::Drop);
    p.groundZ = 3.5; // 1.5 m down: neither walkable on nor a drop
    CHECK(judgeEdge(p) == EdgeCause::Unjudged);
    p.groundZ = 7.0; // a rise past the wall
    CHECK(judgeEdge(p) == EdgeCause::Unjudged);
    p.groundFound = false; // nothing under the far side at all
    CHECK(judgeEdge(p) == EdgeCause::Drop);

    // No hit: the ground along the probe.
    p.hit = EdgeHit::None;
    p.groundFound = true;
    p.groundZ = -0.01;
    CHECK(judgeEdge(p) == EdgeCause::Water);
    p.hasSea = false;
    CHECK(judgeEdge(p) == EdgeCause::Drop);
    p.groundZ = 4.9;
    CHECK(judgeEdge(p) == EdgeCause::Unjudged);
    p.groundZ = 2.9; // just over the drop
    CHECK(judgeEdge(p) == EdgeCause::Drop);
    p.groundZ = 3.0; // exactly the drop is not over it
    CHECK(judgeEdge(p) == EdgeCause::Unjudged);

    CHECK(std::string(edgeCauseName(EdgeCause::BareHiddenWall)) == "BARE HIDDEN WALL");
    CHECK(std::string(edgeCauseName(EdgeCause::Unjudged)) == "UNJUDGED");
    CHECK(std::string(edgeCauseName(EdgeCause::BuiltWall)) == "built wall");
    CHECK(edgeCauseMet(EdgeCause::Cliff));
    CHECK(edgeCauseMet(EdgeCause::BuiltWall));
    CHECK_FALSE(edgeCauseMet(EdgeCause::BareHiddenWall));
    CHECK_FALSE(edgeCauseMet(EdgeCause::Unjudged));
}

TEST_CASE("world checks: an edge's tally") {
    EdgeTally t;
    const EdgeCause causes[] = {EdgeCause::Cliff,     EdgeCause::Water,          EdgeCause::Water,   EdgeCause::Drop,
                                EdgeCause::BuiltWall, EdgeCause::BareHiddenWall, EdgeCause::Unjudged};
    for (const EdgeCause c : causes) t.add(c);
    CHECK(t.legs == 7);
    CHECK(t.cliff == 1);
    CHECK(t.water == 2);
    CHECK(t.drop == 1);
    CHECK(t.built == 1);
    CHECK(t.bare == 1);
    CHECK(t.unjudged == 1);
    CHECK(t.met() == 5);
}

// ---- Coverage points -------------------------------------------------------------------------------------------

TEST_CASE("world checks: a coverage table reads its points and skips every other column") {
    const std::string csv = "# coverage\n"
                            "role,note,id,x,y,z\n"
                            "on,\"some words, kept out\",C1,10,20,1.5\n"
                            "# between\n"
                            "pocket,,P-2,300,400,-2\n";
    std::vector<CoveragePoint> points;
    std::string error;
    REQUIRE(importCoveragePoints(csv, points, &error));
    REQUIRE(points.size() == 2);
    CHECK(points[0].id == "C1");
    CHECK(points[0].role == CoverageRole::On);
    CHECK(near(points[0].x, 10.0));
    CHECK(near(points[0].y, 20.0));
    CHECK(near(points[0].z, 1.5));
    CHECK(points[1].id == "P-2");
    CHECK(points[1].role == CoverageRole::Pocket);
    CHECK(near(points[1].z, -2.0));
}

TEST_CASE("world checks: every coverage table fault names its line, and none quotes another column") {
    CHECK(coverageError("# nothing\n") == "line 1: no header row");
    CHECK(coverageError("id,role,x,y\n") == "line 1: no column z");
    CHECK(coverageError("id,role,role,x,y,z\n") == "line 1: column role twice");
    CHECK(coverageError("id,role,x,y,z\nA,on,1,2\n") == "line 2: 4 fields where the header has 5");
    CHECK(coverageError("id,role,x,y,z\nA b,on,1,2,3\n") == "line 2: id must be A-Z, 0-9, '_' or '-'");
    CHECK(coverageError("id,role,x,y,z\nA,on,1,2,3\nA,pocket,1,2,3\n") == "line 3: id A twice");
    CHECK(coverageError("id,role,x,y,z,note\nA,in,1,2,3,secret\n") == "line 2: A: role must be pocket or on");
    CHECK(coverageError("id,role,x,y,z,note\nA,on,1,two,3,secret\n") == "line 2: A: y is not a number");
    CHECK(coverageError("id,role,x,y,z\nA,on,1,2,\n") == "line 2: A: z is not a number");
}

// ---- Navmesh islands -------------------------------------------------------------------------------------------

TEST_CASE("world checks: a poly's area and centroid, either winding") {
    const MassingVec ccw[] = {{0, 0, 0}, {4, 0, 0}, {4, 2, 0}, {0, 2, 0}};
    const MassingVec cw[] = {{0, 0, 0}, {0, 2, 0}, {4, 2, 0}, {4, 0, 0}};
    double area = 0, cx = 0, cy = 0;
    navPolyArea(ccw, 4, area, cx, cy);
    CHECK(near(area, 8.0));
    CHECK(near(cx, 2.0));
    CHECK(near(cy, 1.0));
    navPolyArea(cw, 4, area, cx, cy);
    CHECK(near(area, 8.0));
    CHECK(near(cx, 2.0));
    CHECK(near(cy, 1.0));
    const MassingVec tri[] = {{900, 900, 5}, {903, 900, 5}, {900, 903, 5}};
    navPolyArea(tri, 3, area, cx, cy);
    CHECK(near(area, 4.5));
    CHECK(near(cx, 901.0));
    CHECK(near(cy, 901.0));
    const MassingVec line[] = {{0, 0, 0}, {2, 0, 0}, {4, 0, 0}};
    navPolyArea(line, 3, area, cx, cy);
    CHECK(area == 0.0);
    CHECK(near(cx, 2.0));
    CHECK(near(cy, 0.0));
    navPolyArea(nullptr, 0, area, cx, cy);
    CHECK(area == 0.0);
}

TEST_CASE("world checks: navmesh islands by their links, largest first") {
    std::vector<MassingVec> corners;
    std::vector<NavPolyRef> polys;
    // Island X: one 2 m square (poly 0). Island Y: three 10 m squares in a row (polys 1, 2, 3), one of them higher.
    // Island Z: a 5 m square (poly 4) linked only by an off-mesh link to poly 5, another 5 m square.
    square(corners, polys, 500, 500, 2, 1);
    square(corners, polys, 0, 0, 10, 0);
    square(corners, polys, 10, 0, 10, 2);
    square(corners, polys, 20, 0, 10, 4);
    square(corners, polys, 100, 100, 5, -1);
    square(corners, polys, 105, 100, 5, -1);
    const std::vector<NavLink> links = {{1, 2}, {2, 1}, {3, 2}, {4, 5}, {0, 99}};
    NavIslandSet set;
    navIslands(corners, polys, links, set);
    REQUIRE(set.islands.size() == 3);
    CHECK(near(set.area, 4.0 + 300.0 + 50.0));
    CHECK(near(set.islands[0].area, 300.0));
    CHECK(set.islands[0].polys == 3);
    CHECK(set.islands[0].firstPoly == 1);
    CHECK(near(set.islands[0].cx, 15.0));
    CHECK(near(set.islands[0].cy, 5.0));
    CHECK(near(set.islands[0].zMin, 0.0));
    CHECK(near(set.islands[0].zMax, 4.0));
    CHECK(near(set.islands[1].area, 50.0));
    CHECK(near(set.islands[1].cx, 105.0));
    CHECK(near(set.islands[1].cy, 102.5));
    CHECK(near(set.islands[2].area, 4.0));
    REQUIRE(set.islandOf.size() == 6);
    CHECK(set.islandOf[0] == 2);
    CHECK(set.islandOf[1] == 0);
    CHECK(set.islandOf[2] == 0);
    CHECK(set.islandOf[3] == 0);
    CHECK(set.islandOf[4] == 1);
    CHECK(set.islandOf[5] == 1);

    // A poly whose corners run past the array counts with no area.
    polys.push_back({static_cast<std::uint32_t>(corners.size()) - 2, 4});
    navIslands(corners, polys, {}, set);
    CHECK(set.islands.size() == 7);
    CHECK(set.islands.back().area == 0.0);
    CHECK(set.islands.back().polys == 1);

    navIslands({}, {}, {}, set);
    CHECK(set.islands.empty());
    CHECK(set.area == 0.0);
}

TEST_CASE("world checks: the navmesh under the sea is left out, the rest kept in order") {
    std::vector<MassingVec> corners;
    std::vector<NavPolyRef> polys;
    // Poly 0: under the sea, its highest corner on the line itself (sea level 0 plus the 0.3 m margin), 2 m square.
    polys.push_back({static_cast<std::uint32_t>(corners.size()), 4});
    corners.push_back({600, 100, -2.0});
    corners.push_back({602, 100, -1.5});
    corners.push_back({602, 102, 0.3});
    corners.push_back({600, 102, -0.4});
    // Poly 1: across the waterline, one corner over the line. Poly 2: dry ground. Poly 3: just over the line.
    polys.push_back({static_cast<std::uint32_t>(corners.size()), 3});
    corners.push_back({590, 100, -1.0});
    corners.push_back({595, 100, 0.5});
    corners.push_back({590, 105, 2.0});
    square(corners, polys, 100, 100, 10, 5.0);
    square(corners, polys, 610, 100, 3, 0.31);
    // Poly 4: a 4 m square wholly under the sea. Poly 5: corners past the array, kept as navIslands keeps it.
    square(corners, polys, 620, 100, 4, -3.0);
    polys.push_back({static_cast<std::uint32_t>(corners.size()) - 2, 4});

    std::vector<std::uint32_t> keep = {99};
    SeaLeftOut out;
    out.polys = 7;
    navPolysAboveSea(corners, polys, 0.0, kNavSeaMargin, keep, out);
    CHECK(keep == std::vector<std::uint32_t>{1, 2, 3, 5});
    CHECK(out.polys == 2);
    CHECK(near(out.area, 4.0 + 16.0));

    // A higher sea leaves the poly across the waterline out too; the dry ground stays.
    navPolysAboveSea(corners, polys, 2.0, kNavSeaMargin, keep, out);
    CHECK(keep == std::vector<std::uint32_t>{2, 5});
    CHECK(out.polys == 4);

    // No sea: nothing is left out, even the polys under the datum.
    navPolysAboveSea(corners, polys, kNoSea, kNavSeaMargin, keep, out);
    CHECK(keep == std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5});
    CHECK(out.polys == 0);
    CHECK(out.area == 0.0);

    navPolysAboveSea({}, {}, 0.0, kNavSeaMargin, keep, out);
    CHECK(keep.empty());
    CHECK(out.polys == 0);
}

TEST_CASE("world checks: islands classed by district 0's spawn and the coverage points") {
    std::vector<MassingVec> corners;
    std::vector<NavPolyRef> polys;
    square(corners, polys, 0, 0, 30, 0);     // poly 0: 900 m², district 0's
    square(corners, polys, 200, 0, 10, 0);   // poly 1: 100 m², a pocket point in it
    square(corners, polys, 400, 0, 8, 0);    // poly 2: 64 m², nothing in it
    square(corners, polys, 600, 0, 7, 0);    // poly 3: 49 m², small
    square(corners, polys, 800, 0, 10, 0);   // poly 4: 100 m², only an on-point in it
    NavIslandSet set;
    navIslands(corners, polys, {}, set);
    const std::vector<CoveragePoint> points = {
        {"ON1", CoverageRole::On, 1, 1, 0},       {"ON2", CoverageRole::On, 801, 1, 0},
        {"ON3", CoverageRole::On, 5000, 5000, 0}, {"PK1", CoverageRole::Pocket, 201, 1, 0},
        {"PK2", CoverageRole::Pocket, 2, 2, 0},   {"PK3", CoverageRole::Pocket, 9000, 0, 0},
        {"PK4", CoverageRole::Pocket, 601, 1, 0},
    };
    const std::vector<std::int64_t> pointPolys = {0, 4, -1, 1, 0, -1, 3};
    CoverageSummary s;
    classifyNavIslands(set, 0, points, pointPolys, kNavIslandBig, s);
    REQUIRE(s.classes.size() == 5);
    CHECK(s.home == set.islandOf[0]);
    CHECK(s.classes[set.islandOf[0]] == NavIslandClass::Home);
    CHECK(s.classes[set.islandOf[1]] == NavIslandClass::Declared);
    CHECK(s.classes[set.islandOf[2]] == NavIslandClass::Undeclared);
    CHECK(s.classes[set.islandOf[3]] == NavIslandClass::Small);
    CHECK(s.classes[set.islandOf[4]] == NavIslandClass::Undeclared);
    CHECK(near(s.homeArea, 900.0));
    CHECK(near(s.homePercent, 100.0 * 900.0 / (900.0 + 100.0 + 64.0 + 49.0 + 100.0)));
    CHECK(s.big == 3);
    CHECK(s.bigDeclared == 1);
    CHECK(s.bigUndeclared == 2);
    CHECK(s.onPoints == 3);
    CHECK(s.onHome == 1);
    CHECK(s.onMissed == std::vector<std::size_t>{1, 2});
    CHECK(s.pockets == 4);
    CHECK(s.pocketsMarking == 1);
    CHECK(s.pocketsOffMesh == 1);
    // Largest first: the 100 m² island with the lower poly index before the other.
    REQUIRE(s.undeclared.size() == 2);
    CHECK(s.undeclared[0] == set.islandOf[4]);
    CHECK(s.undeclared[1] == set.islandOf[2]);

    // District 0's spawn off the mesh: no home island, every on-point missed, the big islands all judged.
    classifyNavIslands(set, -1, points, {}, kNavIslandBig, s);
    CHECK(s.home == -1);
    CHECK(s.homeArea == 0.0);
    CHECK(s.onHome == 0);
    CHECK(s.onMissed.size() == 3);
    CHECK(s.big == 4);
    CHECK(s.bigUndeclared == 4);
    CHECK(s.pocketsOffMesh == 4);
}

namespace {

MassingBox massingRow(const char* kind, MassingSource source, bool cover, double cx, double cy, double sx, double sy,
                      double z0, double z1) {
    MassingBox b;
    b.id = "B";
    b.kind = kind;
    b.source = source;
    b.cover = cover;
    b.cx = cx;
    b.cy = cy;
    b.sx = sx;
    b.sy = sy;
    b.z0 = z0;
    b.z1 = z1;
    return b;
}

// A rectangle's corners, (x0, y0) to (x1, y1), counter-clockwise, each at its own height.
void rect(std::vector<MassingVec>& corners, std::vector<NavPolyRef>& polys, double x0, double y0, double x1, double y1,
          double z0, double z1, double z2, double z3) {
    polys.push_back({static_cast<std::uint32_t>(corners.size()), 4});
    corners.push_back({x0, y0, z0});
    corners.push_back({x1, y0, z1});
    corners.push_back({x1, y1, z2});
    corners.push_back({x0, y1, z3});
}

} // namespace

TEST_CASE("world checks: islands on built boxes' roofs and inside them") {
    std::vector<MassingBox> boxes;
    boxes.push_back(massingRow("kit", MassingSource::Plan, false, 50, 50, 20, 20, 0, 10));    // 0: a building
    boxes.push_back(massingRow("slab", MassingSource::Plan, true, 50, 150, 20, 20, 0, 10));   // 1, 2: a building of two
    boxes.push_back(massingRow("slab", MassingSource::Plan, true, 70, 150, 20, 20, 0, 8));
    boxes.push_back(massingRow("kit", MassingSource::Plan, false, 200, 50, 20, 20, 0, 10));   // 3: a stair row reaches it
    MassingBox ramp = massingRow("ramp", MassingSource::Invented, false, 220, 50, 20, 4, 0, 10); // 4: 10 m at x 210, 0 at 230
    ramp.sloped = true;
    ramp.slopeFrom = 10.0;
    ramp.slopeTo = 0.0;
    boxes.push_back(ramp);
    boxes.push_back(massingRow("zone", MassingSource::Zone, true, 400, 50, 20, 20, 0, 10));   // 5: not built
    boxes.push_back(massingRow("interior", MassingSource::Plan, true, 500, 50, 20, 20, 0, 10)); // 6: hidden, not built
    boxes.push_back(massingRow("deck", MassingSource::Plan, true, 600, 50, 20, 20, 0, 2));    // 7: a walk surface
    boxes.push_back(massingRow("wall", MassingSource::Wall, false, 700, 50, 20, 20, -5, 3));  // 8: an added wall

    std::vector<MassingVec> corners;
    std::vector<NavPolyRef> polys;
    square(corners, polys, 42, 42, 16, 10.0);                         // 0: box 0's roof
    square(corners, polys, 42, 42, 16, 0.1);                          // 1: the ground inside box 0
    square(corners, polys, 70, 42, 16, 0.0);                          // 2: the ground beside it
    square(corners, polys, 52, 42, 16, 10.0);                         // 3: over box 0's roof edge, partly outside it
    rect(corners, polys, 45, 145, 75, 155, 10.0, 8.0, 8.0, 10.0);     // 4: across both roofs of boxes 1 and 2
    rect(corners, polys, 193, 48, 209, 56, 10.0, 10.0, 10.0, 10.0);   // 5: box 3's roof, 1 m from the ramp's top
    rect(corners, polys, 212, 49, 218, 51, 0.1, 0.1, 0.1, 0.1);       // 6: the ground under the ramp
    rect(corners, polys, 212, 49, 218, 51, 9.0, 6.0, 6.0, 9.0);       // 7: on the ramp's top
    square(corners, polys, 392, 42, 16, 10.0);                        // 8: in the zone
    square(corners, polys, 492, 42, 16, 0.1);                         // 9: in the hidden interior
    square(corners, polys, 592, 42, 16, 2.0);                         // 10: on the deck
    square(corners, polys, 592, 42, 16, 0.5);                         // 11: under the deck
    square(corners, polys, 692, 42, 16, 3.0);                         // 12: on the added wall's top
    square(corners, polys, 42, 42, 16, 10.4);                         // 13: 0.4 m over box 0's roof
    square(corners, polys, 2, 2, 30, 0.0);                            // 14: district 0's ground
    polys.push_back({static_cast<std::uint32_t>(corners.size()), 0}); // 15: no corners
    NavIslandSet set;
    navIslands(corners, polys, {}, set);
    REQUIRE(set.islands.size() == polys.size());

    std::vector<NavIslandInBoxes> in;
    navIslandsInBoxes(corners, polys, set, boxes, in);
    REQUIRE(in.size() == set.islands.size());
    const auto of = [&](std::size_t poly) { return in[set.islandOf[poly]]; };
    CHECK(of(0).part == NavBoxPart::Roof);
    CHECK(of(0).box == 0);
    CHECK_FALSE(of(0).met);
    CHECK(of(1).part == NavBoxPart::Interior);
    CHECK(of(1).box == 0);
    CHECK(of(2).part == NavBoxPart::None);
    CHECK(of(2).box == -1);
    CHECK(of(3).part == NavBoxPart::None);
    CHECK(of(4).part == NavBoxPart::Roof);
    CHECK(of(4).box == 1);
    CHECK(of(5).part == NavBoxPart::Roof);
    CHECK(of(5).met);
    CHECK(of(6).part == NavBoxPart::Interior);
    CHECK(of(6).box == 4);
    CHECK(of(7).part == NavBoxPart::None);
    CHECK(of(8).part == NavBoxPart::None);
    CHECK(of(9).part == NavBoxPart::None);
    CHECK(of(10).part == NavBoxPart::None);
    CHECK(of(11).part == NavBoxPart::Interior);
    CHECK(of(12).part == NavBoxPart::Roof);
    CHECK(of(12).box == 8);
    CHECK(of(13).part == NavBoxPart::Roof); // within the rise margin of the top
    CHECK(of(13).box == 0);
    CHECK(of(14).part == NavBoxPart::None);
    CHECK(of(15).part == NavBoxPart::None);

    // The classes: in-box islands over the size are Roof or Interior, a roof the ramp meets stays undeclared, and a
    // pocket declares an island whatever it lies in.
    const std::vector<CoveragePoint> points = {{"PK1", CoverageRole::Pocket, 600, 50, 2.0}};
    const std::vector<std::int64_t> pointPolys = {10};
    CoverageSummary s;
    classifyNavIslands(set, 14, points, pointPolys, kNavIslandBig, s, &in);
    CHECK(s.classes[set.islandOf[0]] == NavIslandClass::Roof);
    CHECK(s.classes[set.islandOf[1]] == NavIslandClass::Interior);
    CHECK(s.classes[set.islandOf[2]] == NavIslandClass::Undeclared);
    CHECK(s.classes[set.islandOf[5]] == NavIslandClass::Undeclared);
    CHECK(s.classes[set.islandOf[6]] == NavIslandClass::Small); // 12 m²
    CHECK(s.classes[set.islandOf[10]] == NavIslandClass::Declared);
    CHECK(s.classes[set.islandOf[14]] == NavIslandClass::Home);
    // Roofs: polys 0, 4, 12, 13; interiors: 1, 11. Undeclared: 2, 3, 5, 8, 9.
    CHECK(s.bigRoof == 4);
    CHECK(s.bigInterior == 2);
    CHECK(near(s.roofArea, 256.0 + 300.0 + 256.0 + 256.0));
    CHECK(near(s.interiorArea, 512.0));
    CHECK(s.roofsMet == 1);
    CHECK(s.bigUndeclared == 5);
    CHECK(s.bigDeclared == 1);
    CHECK(s.big == 12);
    REQUIRE(s.inBoxes.size() == 6);
    CHECK(s.inBoxes[0] == set.islandOf[4]); // the largest first
    for (std::size_t i = 1; i < s.inBoxes.size(); ++i) {
        CHECK(set.islands[s.inBoxes[i - 1]].area >= set.islands[s.inBoxes[i]].area);
    }

    // Without the list, or with a short one, nothing lies in a box.
    classifyNavIslands(set, 14, points, pointPolys, kNavIslandBig, s);
    CHECK(s.bigRoof == 0);
    CHECK(s.bigInterior == 0);
    CHECK(s.bigUndeclared == 11);
    const std::vector<NavIslandInBoxes> none;
    classifyNavIslands(set, 14, points, pointPolys, kNavIslandBig, s, &none);
    CHECK(s.bigUndeclared == 11);

    // No boxes: nothing lies in one. No islands: an empty list.
    navIslandsInBoxes(corners, polys, set, {}, in);
    REQUIRE(in.size() == set.islands.size());
    for (const NavIslandInBoxes& r : in) CHECK(r.part == NavBoxPart::None);
    navIslandsInBoxes({}, {}, NavIslandSet{}, boxes, in);
    CHECK(in.empty());
}

// ---- Read-back -------------------------------------------------------------------------------------------------

TEST_CASE("world checks: the read-back window in the Landscape the build makes") {
    // A built field 5 by 4 at 2 m from (-4, -6); the Landscape's square is 7 (3 quads a component).
    const std::size_t bx = 5, by = 4;
    std::vector<std::uint16_t> built(bx * by);
    for (std::size_t j = 0; j < by; ++j)
        for (std::size_t i = 0; i < bx; ++i) built[j * bx + i] = static_cast<std::uint16_t>(30000 + j * 100 + i);
    const std::size_t side = massingLandscapeSide(bx, by, 3);
    REQUIRE(side == 7);
    std::vector<std::uint16_t> land(side * side);
    REQUIRE(massingLandscapeHeights(built.data(), bx, by, side, land.data()));

    // The read-back field: 3 by 2 from the built field's sample (1, 1).
    const std::size_t rx = 3, ry = 2;
    std::vector<std::uint16_t> field(rx * ry);
    for (std::size_t j = 0; j < ry; ++j)
        for (std::size_t i = 0; i < rx; ++i) field[j * rx + i] = built[(j + 1) * bx + (i + 1)];
    ReadBackWindow w;
    std::string error;
    REQUIRE(readBackWindow(side, bx, by, 2.0, -4.0, -6.0, rx, ry, 2.0, -2.0, -4.0, w, &error));
    CHECK(w.ix0 == 1);
    CHECK(w.iy0 == 1);
    CHECK(w.x1 == 1);
    CHECK(w.x2 == 3);
    CHECK(w.y1 == 4);
    CHECK(w.y2 == 5);
    CHECK(w.nx == 3);
    CHECK(w.ny == 2);

    // What the Landscape gives back for the window, the north row first, as its edit data does.
    std::vector<std::uint16_t> got(rx * ry);
    readBackSentinel(field.data(), rx, ry, got.data());
    HeightCompare c;
    compareHeightsExact(field.data(), rx, ry, 2.0, -2.0, -4.0, got.data(), c);
    CHECK(c.compared == 6);
    CHECK(c.mismatched == 6); // unwritten: every cell stands out
    for (std::size_t y = w.y1; y <= w.y2; ++y)
        for (std::size_t x = w.x1; x <= w.x2; ++x) got[(y - w.y1) * rx + (x - w.x1)] = land[y * side + x];
    compareHeightsExact(field.data(), rx, ry, 2.0, -2.0, -4.0, got.data(), c);
    CHECK(c.compared == 6);
    CHECK(c.mismatched == 0);
    CHECK(c.worst == 0.0);

    // One sample off by 3 units, at the field's (2, 0): plan (2, -4).
    got[(ry - 1) * rx + 2] = static_cast<std::uint16_t>(got[(ry - 1) * rx + 2] + 3);
    compareHeightsExact(field.data(), rx, ry, 2.0, -2.0, -4.0, got.data(), c);
    CHECK(c.mismatched == 1);
    CHECK(near(c.worst, 3.0 / 128.0));
    CHECK(near(c.worstX, 2.0));
    CHECK(near(c.worstY, -4.0));

    // The whole built field reads back as itself.
    REQUIRE(readBackWindow(side, bx, by, 2.0, -4.0, -6.0, bx, by, 2.0, -4.0, -6.0, w, &error));
    CHECK(w.x1 == 0);
    CHECK(w.x2 == 4);
    CHECK(w.y1 == 3);
    CHECK(w.y2 == 6);
}

TEST_CASE("world checks: a read-back window that can't be read says why") {
    ReadBackWindow w;
    std::string error;
    CHECK_FALSE(readBackWindow(7, 5, 4, 2.0, 0, 0, 0, 2, 2.0, 0, 0, w, &error));
    CHECK(error == "a size is zero");
    CHECK_FALSE(readBackWindow(7, 5, 4, 2.0, 0, 0, 3, 2, 0.0, 0, 0, w, &error));
    CHECK(error == "a spacing isn't a positive number");
    CHECK_FALSE(readBackWindow(7, 5, 4, 2.0, 0, 0, 3, 2, 2.0, std::numeric_limits<double>::quiet_NaN(), 0, w, &error));
    CHECK(error == "an origin isn't a number");
    CHECK_FALSE(readBackWindow(7, 5, 4, 2.0, 0, 0, 3, 2, 1.0, 0, 0, w, &error));
    CHECK(error.find("spacing") != std::string::npos);
    CHECK_FALSE(readBackWindow(7, 5, 4, 2.0, 0, 0, 3, 2, 2.0, 1.0, 0, w, &error));
    CHECK(error == "the read-back field's origin isn't on the built field's grid");
    CHECK_FALSE(readBackWindow(7, 5, 4, 2.0, 0, 0, 3, 2, 2.0, 6.0, 0, w, &error));
    CHECK(error.find("doesn't lie inside the built field") != std::string::npos);
    CHECK_FALSE(readBackWindow(7, 5, 4, 2.0, 0, 0, 3, 2, 2.0, -2.0, 0, w, &error));
    CHECK(error.find("doesn't lie inside the built field") != std::string::npos);
    CHECK_FALSE(readBackWindow(4, 5, 4, 2.0, 0, 0, 3, 2, 2.0, 0, 0, w, &error));
    CHECK(error == "the built field doesn't fit the Landscape's square");
}

TEST_CASE("world checks: the two-triangle surface on either diagonal") {
    // A tilted plane is the same on both diagonals and at the samples.
    const std::size_t nx = 4, ny = 3;
    std::vector<std::uint16_t> plane(nx * ny);
    for (std::size_t j = 0; j < ny; ++j)
        for (std::size_t i = 0; i < nx; ++i)
            plane[j * nx + i] = static_cast<std::uint16_t>(32768 + 128 * i + 64 * j); // 1 m east, 0.5 m north a sample
    for (const CellDiagonal d : {CellDiagonal::SouthWestNorthEast, CellDiagonal::NorthWestSouthEast}) {
        CHECK(near(heightOnTriangles(plane.data(), nx, ny, 2.0, 10.0, 20.0, 10.0, 20.0, d), 0.0));
        CHECK(near(heightOnTriangles(plane.data(), nx, ny, 2.0, 10.0, 20.0, 12.4, 22.6, d), 1.2 + 0.65));
        CHECK(near(heightOnTriangles(plane.data(), nx, ny, 2.0, 10.0, 20.0, 16.0, 24.0, d), 3.0 + 1.0));
        CHECK(near(heightOnTriangles(plane.data(), nx, ny, 2.0, 10.0, 20.0, 99.0, 99.0, d), 3.0 + 1.0)); // clamped
        CHECK(near(heightOnTriangles(plane.data(), nx, ny, 2.0, 10.0, 20.0, 0.0, 0.0, d), 0.0));
    }
    // One raised corner tells the diagonals apart: the north-east one, 1 m up.
    const std::uint16_t cell[4] = {raw(0.0), raw(0.0), raw(0.0), raw(1.0)};
    const auto sw = CellDiagonal::SouthWestNorthEast, nw = CellDiagonal::NorthWestSouthEast;
    CHECK(near(heightOnTriangles(cell, 2, 2, 1.0, 0, 0, 0.7, 0.2, sw), 0.2));
    CHECK(near(heightOnTriangles(cell, 2, 2, 1.0, 0, 0, 0.7, 0.2, nw), 0.0));
    CHECK(near(heightOnTriangles(cell, 2, 2, 1.0, 0, 0, 0.2, 0.7, sw), 0.2));
    CHECK(near(heightOnTriangles(cell, 2, 2, 1.0, 0, 0, 0.2, 0.7, nw), 0.0));
    CHECK(near(heightOnTriangles(cell, 2, 2, 1.0, 0, 0, 0.9, 0.35, sw), 0.35));
    CHECK(near(heightOnTriangles(cell, 2, 2, 1.0, 0, 0, 0.9, 0.35, nw), 0.25));
    CHECK(near(heightOnTriangles(cell, 2, 2, 1.0, 0, 0, 1.0, 1.0, nw), 1.0));
    CHECK(near(heightOnTriangles(cell, 2, 2, 1.0, 0, 0, 0.5, 0.5, sw), 0.5));
    CHECK(near(heightOnTriangles(cell, 2, 2, 1.0, 0, 0, 0.5, 0.5, nw), 0.0));
    CHECK(heightOnTriangles(nullptr, 2, 2, 1.0, 0, 0, 0.5, 0.5, sw) == 0.0);
    CHECK(heightOnTriangles(cell, 2, 2, 0.0, 0, 0, 0.5, 0.5, sw) == 0.0);
}

TEST_CASE("world checks: the surface points sit inside cells, off both diagonals, the same every time") {
    std::vector<SurfacePoint> a, b;
    readBackSurfacePoints(1501, 1501, 1.0, -250.0, -250.0, kReadBackSurfacePoints, a);
    readBackSurfacePoints(1501, 1501, 1.0, -250.0, -250.0, kReadBackSurfacePoints, b);
    REQUIRE(a.size() == kReadBackSurfacePoints);
    std::set<std::pair<long, long>> cells;
    for (std::size_t k = 0; k < a.size(); ++k) {
        CHECK(a[k].x == b[k].x);
        CHECK(a[k].y == b[k].y);
        const double u = a[k].x + 250.0, v = a[k].y + 250.0;
        REQUIRE(u > 0.0);
        REQUIRE(v > 0.0);
        REQUIRE(u < 1500.0);
        REQUIRE(v < 1500.0);
        const double fu = u - std::floor(u), fv = v - std::floor(v);
        CHECK(std::fabs(fu - fv) >= 0.1 - 1e-9);
        CHECK(std::fabs(fu + fv - 1.0) >= 0.1 - 1e-9);
        CHECK(fu > 0.1 - 1e-9);
        CHECK(fv > 0.1 - 1e-9);
        cells.insert({static_cast<long>(std::floor(u)), static_cast<long>(std::floor(v))});
    }
    CHECK(cells.size() == a.size()); // no cell twice
    // Spread over the field: points in each quarter.
    int quarters[4] = {};
    for (const SurfacePoint& p : a) ++quarters[(p.x > 500.0 ? 1 : 0) + (p.y > 500.0 ? 2 : 0)];
    for (const int q : quarters) CHECK(q > 800);

    readBackSurfacePoints(1, 5, 1.0, 0, 0, 10, a);
    CHECK(a.empty());
    readBackSurfacePoints(2, 2, 1.0, 0, 0, 7, a);
    CHECK(a.size() == 7); // a single cell takes every point
}

TEST_CASE("world checks: the surface compare counts the worst and those over the tolerance") {
    const std::size_t nx = 3, ny = 3;
    std::vector<std::uint16_t> grid(nx * ny);
    for (std::size_t k = 0; k < grid.size(); ++k) grid[k] = raw(static_cast<double>((k * 7) % 5) * 0.25);
    std::vector<SurfacePoint> points;
    readBackSurfacePoints(nx, ny, 1.0, 0, 0, 12, points);
    REQUIRE(points.size() == 12);
    const auto d = CellDiagonal::NorthWestSouthEast;
    std::vector<double> measured;
    for (const SurfacePoint& p : points) measured.push_back(heightOnTriangles(grid.data(), nx, ny, 1.0, 0, 0, p.x, p.y, d));
    SurfaceCompare c;
    compareSurface(points, measured, grid.data(), nx, ny, 1.0, 0, 0, d, kReadBackSurfaceTolerance, c);
    CHECK(c.compared == 12);
    CHECK(c.over == 0);
    CHECK(c.unread == 0);
    CHECK(c.worst < 1e-12);

    measured[3] += 0.004; // inside the tolerance
    measured[5] -= 0.02;  // over it
    measured[7] = std::numeric_limits<double>::quiet_NaN();
    measured.pop_back(); // the last point never read
    compareSurface(points, measured, grid.data(), nx, ny, 1.0, 0, 0, d, kReadBackSurfaceTolerance, c);
    CHECK(c.compared == 12);
    CHECK(c.unread == 2);
    CHECK(c.over == 3);
    CHECK(near(c.worst, 0.02, 1e-12));
    CHECK(c.worstX == points[5].x);
    CHECK(c.worstY == points[5].y);
}
