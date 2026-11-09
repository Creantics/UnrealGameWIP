// A world's massing table (ftcore/MassingPlan.h): the CSV reader, the roles and finishes the build
// gives each box, the corners and engine-frame meshes, the shared-top nudges and the Landscape grid.
// Made-up tables only: the real one is kept outside the repo. Ids and numbers only.

#include "ftcore/MassingPlan.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace ftcore;

namespace {

constexpr const char* kHeader = "id,row,src,name,kind,cx,cy,sx,sy,rot,z0,z1,ground,cover,note,slope\n";

std::string table(const std::string& rows) {
    return std::string("\"# a comment line, quoted\"\n") + kHeader + rows;
}

bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

MassingBox flatBox(double cx, double cy, double sx, double sy, double rot, double z0, double z1) {
    MassingBox b;
    b.id = "B";
    b.kind = "slab";
    b.cover = true;
    b.cx = cx;
    b.cy = cy;
    b.sx = sx;
    b.sy = sy;
    b.rot = rot;
    b.z0 = z0;
    b.z1 = z1;
    return b;
}

MassingVec sub(const MassingVec& a, const MassingVec& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
MassingVec cross(const MassingVec& a, const MassingVec& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double dot(const MassingVec& a, const MassingVec& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Every quad's (b - a) x (c - a) points into the solid, and every edge is walked once each way.
void checkMeshWinding(const MassingMesh& m) {
    MassingVec centroid;
    for (const MassingVec& p : m.v) {
        centroid.x += p.x / 8.0;
        centroid.y += p.y / 8.0;
        centroid.z += p.z / 8.0;
    }
    std::map<std::pair<int, int>, int> edges;
    for (const auto& q : m.quads) {
        const MassingVec n = cross(sub(m.v[q[1]], m.v[q[0]]), sub(m.v[q[2]], m.v[q[0]]));
        MassingVec mid;
        for (const std::uint8_t k : q) {
            mid.x += m.v[k].x / 4.0;
            mid.y += m.v[k].y / 4.0;
            mid.z += m.v[k].z / 4.0;
        }
        CHECK(dot(n, sub(mid, centroid)) < 0.0);
        for (std::size_t k = 0; k < 4; ++k) ++edges[{q[k], q[(k + 1) % 4]}];
    }
    CHECK(edges.size() == 24);
    for (const auto& [e, count] : edges) {
        CHECK(count == 1);
        CHECK(edges.count({e.second, e.first}) == 1);
    }
}

} // namespace

TEST_CASE("massing: the table is read by column name, names and notes skipped") {
    const std::string csv = table(
        "BLD-001_1,BLD-001,BLD,\"Tower (\"\"the top\"\"), north\",slab,110,500,24,60,0.0,79.5,102,80,True,a note,\n"
        "K-7,BLD-009,BLD,kit,deck,10.5,-20,8,4,30,13,13.5,13,False,\"two\nlines\",\"x 13.00 to 16.33, 0.5 thick\"\r\n"
        "\n"
        "# a comment between rows\n"
        "W1,,WALL,,barrier,0,500,1000,0.5,90,-10,60,0,False,,\n");
    std::vector<MassingBox> boxes;
    std::string error;
    REQUIRE_MESSAGE(importMassingBoxes(csv, boxes, &error), error);
    REQUIRE(boxes.size() == 3);

    const MassingBox& a = boxes[0];
    CHECK(a.id == "BLD-001_1");
    CHECK(a.kind == "slab");
    CHECK(a.source == MassingSource::Plan);
    CHECK(a.cover);
    CHECK(a.cx == 110.0);
    CHECK(a.cy == 500.0);
    CHECK(a.sx == 24.0);
    CHECK(a.sy == 60.0);
    CHECK(a.z0 == 79.5);
    CHECK(a.z1 == 102.0);
    CHECK_FALSE(a.sloped);

    const MassingBox& k = boxes[1];
    CHECK(k.id == "K-7");
    CHECK_FALSE(k.cover);
    CHECK(k.rot == 30.0);
    CHECK(k.cy == -20.0);
    CHECK(k.sloped);
    CHECK(k.slopeFrom == 13.0);
    CHECK(near(k.slopeTo, 16.33));
    CHECK(k.slopeThick == 0.5);

    CHECK(boxes[2].source == MassingSource::Wall);
    CHECK(massingRole(boxes[2]) == MassingRole::Wall);
}

TEST_CASE("massing: a slope without a thickness, and the sources") {
    const std::string csv = table("R,,INV,,ramp,0,0,10,4,0,0,10,0,True,,x 10.00 to 4.00\n"
                                  "Z1,,Z,,zone_slope,0,0,10,4,0,0,1,0,True,,x 1 to 2\n"
                                  "O1,,OPEN,,box,0,0,1,1,0,0,1,0,True,,\n"
                                  "P1,,PROXY,,crane,0,0,1,1,0,0,1,0,True,,\n");
    std::vector<MassingBox> boxes;
    std::string error;
    REQUIRE_MESSAGE(importMassingBoxes(csv, boxes, &error), error);
    REQUIRE(boxes.size() == 4);
    CHECK(boxes[0].source == MassingSource::Invented);
    CHECK(boxes[0].slopeFrom == 10.0);
    CHECK(boxes[0].slopeTo == 4.0);
    CHECK(boxes[0].slopeThick < 0.0);
    CHECK(boxes[1].source == MassingSource::Zone);
    CHECK(boxes[2].source == MassingSource::Open);
    CHECK(boxes[3].source == MassingSource::Proxy);
}

TEST_CASE("massing: errors name the line") {
    struct Bad {
        std::string csv;
        const char* says;
    };
    const Bad bad[] = {
        {"id,src,kind,cx,cy,sx,sy,rot,z0,z1,cover\nA,BLD,slab,0,0,1,1,0,0,1,True\n", "no column slope"},
        {"id,src,kind,cx,cy,sx,sy,rot,z0,z1,cover,slope,id\nA,BLD,slab,0,0,1,1,0,0,1,True,,A\n", "column id twice"},
        {table("A,,BLD,,slab,0,0,1,1,0,0,1,0,True,,\nA,,BLD,,slab,0,0,1,1,0,0,1,0,True,,\n"), "line 4: id A twice"},
        {table("A,,BLD,,slab,0,zero,1,1,0,0,1,0,True,,\n"), "line 3: A: cy is not a number"},
        {table("A,,BLD,,slab,0,,1,1,0,0,1,0,True,,\n"), "cy is not a number"},
        {table("A,,BLD,,slab,0,0,1,1,0,0,1,0,True\n"), "line 3: 14 fields where the header has 16"},
        {table("A,,BLD,,slab,0,0,1,1,0,0,1,0,True,,y 1 to 2\n"), "A: slope must be"},
        {table("A,,BLD,,slab,0,0,1,1,0,0,1,0,True,,x 1 to 2 thick\n"), "A: slope must be"},
        {table("A,,BLD,,slab,0,0,1,1,0,0,1,0,True,,\"x 1 to 2, 0 thick\"\n"), "A: slope must be"},
        {table("a1,,BLD,,slab,0,0,1,1,0,0,1,0,True,,\n"), "id must be"},
        {table("A,,XYZ,,slab,0,0,1,1,0,0,1,0,True,,\n"), "A: unknown src"},
        {table("A,,BLD,,Slab,0,0,1,1,0,0,1,0,True,,\n"), "A: kind must be"},
        {table("A,,BLD,,slab,0,0,0,1,0,0,1,0,True,,\n"), "A: sx and sy must be > 0"},
        {table("A,,BLD,,slab,0,0,1,1,0,2,1,0,True,,\n"), "A: z1 under z0"},
        {table("A,,BLD,,slab,0,0,1,1,0,0,1,0,yes,,\n"), "A: cover must be True or False"},
        {table("A,,BLD,\"open,slab,0,0,1,1,0,0,1,0,True,,\n"), "line 3: a quoted field is never closed"},
        {table("A,,BLD,\"x\"y,slab,0,0,1,1,0,0,1,0,True,,\n"), "line 3: text after a closing quote"},
        {"# only a comment\n", "no header row"},
    };
    for (const Bad& b : bad) {
        CAPTURE(b.csv);
        std::vector<MassingBox> boxes;
        std::string error;
        CHECK_FALSE(importMassingBoxes(b.csv, boxes, &error));
        CHECK_MESSAGE(error.find(b.says) != std::string::npos, error);
    }
}

TEST_CASE("massing: roles in the build's order, and finishes") {
    MassingBox b = flatBox(0, 0, 1, 1, 0, 0, 1);
    b.kind = "interior";
    b.source = MassingSource::Zone;
    CHECK(massingRole(b) == MassingRole::Zone); // a zone wins over a hidden kind
    b.source = MassingSource::Open;
    CHECK(massingRole(b) == MassingRole::Hidden); // a hidden kind wins over an open option
    for (const char* kind : {"trigger", "underpass"}) {
        b.kind = kind;
        CHECK(massingRole(b) == MassingRole::Hidden);
    }
    b.kind = "box";
    CHECK(massingRole(b) == MassingRole::Open);
    b.source = MassingSource::Proxy;
    CHECK(massingRole(b) == MassingRole::Proxy);
    b.source = MassingSource::Wall;
    CHECK(massingRole(b) == MassingRole::Wall);
    CHECK(massingBuilt(MassingRole::Wall));
    CHECK_FALSE(massingVisible(MassingRole::Wall));

    b.source = MassingSource::Plan;
    b.cover = false;
    CHECK(massingRole(b) == MassingRole::Kit);
    CHECK(massingFinish(b) == MassingFinish::Plain);
    b.kind = "wall";
    CHECK(massingFinish(b) == MassingFinish::Wall);
    b.kind = "deck";
    CHECK(massingFinish(b) == MassingFinish::Plain); // a kit deck stays plain
    b.cover = true;
    CHECK(massingRole(b) == MassingRole::Named);
    for (const char* kind : {"deck", "pier", "bridge", "quay"}) {
        b.kind = kind;
        CHECK(massingFinish(b) == MassingFinish::Deck);
    }
    b.kind = "wall";
    CHECK(massingFinish(b) == MassingFinish::Plain); // a named wall stays plain
    b.source = MassingSource::Invented;
    CHECK(massingRole(b) == MassingRole::Named);

    for (const MassingRole r : {MassingRole::Zone, MassingRole::Hidden, MassingRole::Open, MassingRole::Proxy})
        CHECK_FALSE(massingBuilt(r));
    CHECK(massingVisible(MassingRole::Kit));
    CHECK(massingVisible(MassingRole::Named));
}

TEST_CASE("massing: a flat box's corners, its nudge and its least height") {
    const MassingBox b = flatBox(100, 200, 20, 10, 90, 5, 15);
    std::array<MassingVec, 8> c;
    massingCorners(b, 0.0, c);
    // rot 90: u runs north, w runs west.
    CHECK(near(c[0].x, 105, 1e-9));
    CHECK(near(c[0].y, 190, 1e-9));
    CHECK(near(c[1].x, 105, 1e-9));
    CHECK(near(c[1].y, 210, 1e-9));
    CHECK(near(c[2].x, 95, 1e-9));
    CHECK(near(c[2].y, 210, 1e-9));
    for (std::size_t k = 0; k < 4; ++k) {
        CHECK(c[k].z == 5.0);
        CHECK(c[k + 4].z == 15.0);
        CHECK(near(c[k + 4].x, c[k].x));
        CHECK(near(c[k + 4].y, c[k].y));
    }
    massingCorners(b, -0.04, c);
    CHECK(near(c[4].z, 14.96));
    const MassingBox thin = flatBox(0, 0, 1, 1, 0, 3, 3);
    massingCorners(thin, -0.02, c);
    CHECK(near(c[4].z, 3.0 + kMassingMinHeight));
}

TEST_CASE("massing: a sloped box's top runs along u, its bottom by its kind") {
    MassingBox b = flatBox(0, 0, 10, 4, 0, 2, 9);
    b.sloped = true;
    b.slopeFrom = 13.0;
    b.slopeTo = 16.0;
    b.slopeThick = 0.5;
    std::array<MassingVec, 8> c;
    massingCorners(b, -1.0, c); // the nudge leaves sloped boxes alone
    CHECK(near(c[4].x, -5.0));
    CHECK(near(c[4].z, 13.0));
    CHECK(near(c[5].z, 16.0));
    CHECK(near(c[6].z, 16.0));
    CHECK(near(c[7].z, 13.0));
    CHECK(near(c[0].z, 12.5));
    CHECK(near(c[1].z, 15.5));

    b.slopeThick = -1.0;
    massingCorners(b, 0.0, c);
    for (std::size_t k = 0; k < 4; ++k) CHECK(c[k].z == 2.0);

    b.source = MassingSource::Zone;
    b.slopeThick = 0.5; // a zone's plate ignores the thickness
    massingCorners(b, 0.0, c);
    CHECK(near(c[4].z, 13.0 + kMassingZonePlate));
    CHECK(near(c[0].z, 12.0));
    CHECK(near(c[5].z, 16.0 + kMassingZonePlate));
}

TEST_CASE("massing: a named cliff cover is a navigation obstacle; nothing else is") {
    MassingBox b = flatBox(0, 0, 10, 4, 0, 2, 9);
    b.kind = "cliff";
    b.source = MassingSource::Invented;
    CHECK(massingNavObstacle(b));
    b.source = MassingSource::Plan;
    CHECK(massingNavObstacle(b));
    b.cover = false; // kit
    CHECK_FALSE(massingNavObstacle(b));
    b.cover = true;
    for (const MassingSource s : {MassingSource::Zone, MassingSource::Open, MassingSource::Proxy, MassingSource::Wall}) {
        b.source = s;
        CHECK_FALSE(massingNavObstacle(b));
    }
    b.source = MassingSource::Plan;
    for (const char* kind : {"deck", "plate", "ramp", "slab", "interior"}) {
        b.kind = kind;
        CHECK_FALSE(massingNavObstacle(b));
    }
}

TEST_CASE("massing: an obstacle's null strips tile its footprint, each rising at most the strip rise") {
    // A flat box is one strip over its whole footprint, the margin past its bottom and top.
    MassingBox f = flatBox(0, 0, 10, 4, 30, 2, 9);
    REQUIRE(massingNullBandCount(f) == 1);
    MassingNullBand n = massingNullBand(f, 0);
    CHECK(near(n.u0, -5.0));
    CHECK(near(n.u1, 5.0));
    CHECK(near(n.w0, -2.0));
    CHECK(near(n.w1, 2.0));
    CHECK(near(n.z0, 2.0 - kMassingNullMargin));
    CHECK(near(n.z1, 9.0 + kMassingNullMargin));

    // A 2 m slab rising 3 m over 12 m: six strips of 2 m, each covering its own share of the slab.
    MassingBox s = flatBox(0, 0, 12, 4, 0, 0, 0);
    s.sloped = true;
    s.slopeFrom = 10.0;
    s.slopeTo = 13.0;
    s.slopeThick = 2.0;
    REQUIRE(massingNullBandCount(s) == 6);
    double u = -6.0;
    for (std::size_t i = 0; i < 6; ++i) {
        n = massingNullBand(s, i);
        CHECK(near(n.u0, u, 1e-12));
        CHECK(near(n.u1, u + 2.0, 1e-12));
        CHECK(near(n.w0, -2.0));
        CHECK(near(n.w1, 2.0));
        const double top0 = 10.0 + 0.5 * static_cast<double>(i), top1 = top0 + 0.5;
        CHECK(near(n.z0, top0 - 2.0 - kMassingNullMargin, 1e-12));
        CHECK(near(n.z1, top1 + kMassingNullMargin, 1e-12));
        CHECK(n.z1 - n.z0 <= 2.0 + kMassingNullRise + 2.0 * kMassingNullMargin + 1e-12);
        u = n.u1;
    }
    CHECK(near(u, 6.0));
    // An index past the last is the last.
    CHECK(near(massingNullBand(s, 99).u0, massingNullBand(s, 5).u0));

    // Falling the other way: the same strips, the high end first.
    s.slopeFrom = 13.0;
    s.slopeTo = 10.0;
    REQUIRE(massingNullBandCount(s) == 6);
    n = massingNullBand(s, 0);
    CHECK(near(n.z1, 13.0 + kMassingNullMargin));
    CHECK(near(n.z0, 12.5 - 2.0 - kMassingNullMargin));

    // A rise that isn't a whole number of strips rounds up; one within a strip is one strip.
    s.slopeTo = 10.0 + 3.1;
    s.slopeFrom = 10.0;
    CHECK(massingNullBandCount(s) == 7);
    s.slopeTo = 10.4;
    CHECK(massingNullBandCount(s) == 1);
    n = massingNullBand(s, 0);
    CHECK(near(n.z0, 10.0 - 2.0 - kMassingNullMargin));
    CHECK(near(n.z1, 10.4 + kMassingNullMargin));

    // Without a thickness the slab reaches down to z0: each strip runs from z0.
    s.slopeTo = 13.0;
    s.slopeThick = -1.0;
    s.z0 = 4.0;
    n = massingNullBand(s, 3);
    CHECK(near(n.z0, 4.0 - kMassingNullMargin));
    CHECK(near(n.z1, 12.0 + kMassingNullMargin));

    // A cliff steep enough to want more strips than the cap gets the cap; a NaN slope gets one.
    s.slopeTo = 10.0 + kMassingNullRise * 5000.0;
    CHECK(massingNullBandCount(s) == kMassingNullMaxBands);
    s.slopeTo = std::nan("");
    CHECK(massingNullBandCount(s) == 1);
}

TEST_CASE("massing: the engine frame reads true from above") {
    // The square's centre is the origin; north is -Y; east is +X.
    const MassingVec centre = massingEnginePoint(1000, 500, 500, 7);
    CHECK(centre.x == 0.0);
    CHECK(centre.y == 0.0);
    CHECK(centre.z == 7.0);
    const MassingVec north = massingEnginePoint(1000, 500, 900, 0);
    CHECK(north.y == -400.0);
    const MassingVec east = massingEnginePoint(1000, 950, 500, 0);
    CHECK(east.x == 450.0);

    // A box turned 30 degrees counter-clockwise points its u axis where Unreal's yaw of -30 points X.
    const MassingBox b = flatBox(500, 500, 20, 2, 30, 0, 1);
    std::array<MassingVec, 8> c;
    massingCorners(b, 0.0, c);
    const MassingVec p1 = massingEnginePoint(1000, c[1].x, c[1].y, 0), p2 = massingEnginePoint(1000, c[2].x, c[2].y, 0);
    const double yaw = massingEngineYaw(b.rot) * 3.14159265358979323846 / 180.0;
    CHECK(near((p1.x + p2.x) / 2, 10.0 * std::cos(yaw), 1e-9));
    CHECK(near((p1.y + p2.y) / 2, 10.0 * std::sin(yaw), 1e-9));
}

TEST_CASE("massing: meshes wind the way the engine's own cube does") {
    // Unreal's generated cube (UStaticMeshDescription::CreateCube): corners and its +Z and +X faces.
    const MassingVec cube[8] = {{1, -1, 1}, {1, 1, 1}, {-1, 1, 1}, {-1, -1, 1}, {-1, 1, -1}, {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}};
    const int plusZ[3] = {1, 0, 3}, plusX[3] = {0, 1, 7};
    CHECK(cross(sub(cube[plusZ[1]], cube[plusZ[0]]), sub(cube[plusZ[2]], cube[plusZ[0]])).z < 0.0);
    CHECK(cross(sub(cube[plusX[1]], cube[plusX[0]]), sub(cube[plusX[2]], cube[plusX[0]])).x < 0.0);

    MassingMesh m;
    massingMesh(flatBox(100, 200, 20, 10, 0, 5, 15), 1000, 0.0, m);
    checkMeshWinding(m);
    massingMesh(flatBox(100, 200, 20, 10, 137.5, -3, 9), 1000, -0.02, m);
    checkMeshWinding(m);
    MassingBox s = flatBox(300, 700, 30, 6, -20.3, 0, 1);
    s.sloped = true;
    s.slopeFrom = 13.0;
    s.slopeTo = 47.88;
    s.slopeThick = 0.5;
    massingMesh(s, 1000, 0.0, m);
    checkMeshWinding(m);
    s.slopeThick = -1.0;
    s.slopeFrom = 10.0;
    s.slopeTo = 4.0;
    massingMesh(s, 1000, 0.0, m);
    checkMeshWinding(m);
    // The mesh sits where its corners do, moved into the engine frame.
    std::array<MassingVec, 8> c;
    massingCorners(s, 0.0, c);
    for (std::size_t k = 0; k < 8; ++k) {
        const MassingVec e = massingEnginePoint(1000, c[k].x, c[k].y, c[k].z);
        CHECK(near(m.v[k].x, e.x));
        CHECK(near(m.v[k].y, e.y));
        CHECK(near(m.v[k].z, e.z));
    }
}

TEST_CASE("massing: footprints overlap only with area") {
    const MassingBox a = flatBox(0, 0, 10, 10, 0, 0, 1);
    CHECK(massingFootprintsOverlap(a, flatBox(5, 5, 10, 10, 0, 0, 1)));
    CHECK_FALSE(massingFootprintsOverlap(a, flatBox(10, 0, 10, 10, 0, 0, 1))); // sharing an edge
    CHECK_FALSE(massingFootprintsOverlap(a, flatBox(20, 0, 10, 10, 0, 0, 1)));
    // Bounding boxes overlap, the shapes don't: a diamond off the square's corner.
    CHECK_FALSE(massingFootprintsOverlap(a, flatBox(9.5, 9.5, 4, 4, 45, 0, 1)));
    CHECK(massingFootprintsOverlap(a, flatBox(6, 6, 4, 4, 45, 0, 1)));
}

TEST_CASE("massing: how far a point lies off a footprint") {
    const MassingBox a = flatBox(0, 0, 10, 4, 0, 0, 1);
    CHECK(massingFootprintGap(a, 0, 0) == 0.0);
    CHECK(massingFootprintGap(a, 5, 2) == 0.0); // the corner itself
    CHECK(near(massingFootprintGap(a, 6.5, 0), 1.5));
    CHECK(near(massingFootprintGap(a, -8, -6), 5.0)); // 3 and 4 past the corner
    // A quarter turn: u runs north, so the box is 4 m across and 10 m along.
    const MassingBox b = flatBox(0, 0, 10, 4, 90, 0, 1);
    CHECK(near(massingFootprintGap(b, 0, 6.5), 1.5));
    CHECK(near(massingFootprintGap(b, 3.5, 0), 1.5));
    CHECK(near(massingFootprintGap(b, -6.5, 0), 4.5));
    CHECK(massingFootprintGap(b, 1.9, 4.9) == 0.0);
}

namespace {

// Where a point of the mesh, in its own axes, lands in the engine frame.
MassingVec placedPoint(const MassingKitPlace& p, const MassingVec& m) {
    const double a = p.yaw * 3.14159265358979323846 / 180.0;
    const double x = m.x * p.scale.x, y = m.y * p.scale.y;
    return {p.at.x + x * std::cos(a) - y * std::sin(a), p.at.y + x * std::sin(a) + y * std::cos(a), p.at.z + m.z * p.scale.z};
}

} // namespace

TEST_CASE("massing: a mesh stands in a box's stead at its own size") {
    // Plan (100, 200) in a 1000 m square is engine (-400, 300).
    const MassingBox box = flatBox(100, 200, 10, 20, 0, 10, 30);
    MassingKitPlace p;
    REQUIRE(massingKitPlacement(box, 1000.0, {-1, -2, 0}, {1, 2, 5}, false, 0.0, 0.0, p));
    CHECK(near(p.at.x, -400.0));
    CHECK(near(p.at.y, 300.0));
    CHECK(near(p.at.z, 10.0));
    CHECK(p.scale.x == 1.0);
    CHECK(p.scale.y == 1.0);
    CHECK(p.scale.z == 1.0);
    CHECK(p.yaw == 0.0);

    // A pivot at the mesh's corner, under its base: the bounds still land on the box.
    REQUIRE(massingKitPlacement(box, 1000.0, {0, 0, -1}, {4, 2, 3}, false, 0.0, 0.0, p));
    CHECK(near(p.at.x, -402.0));
    CHECK(near(p.at.y, 299.0));
    CHECK(near(p.at.z, 11.0));

    // dz raises it off the box's bottom.
    REQUIRE(massingKitPlacement(box, 1000.0, {-1, -2, 0}, {1, 2, 5}, false, 0.0, 2.5, p));
    CHECK(near(p.at.z, 12.5));

    // A flat mesh has no size to fit, and at its own size that is fine.
    CHECK(massingKitPlacement(box, 1000.0, {-1, -2, 0}, {1, 2, 0}, false, 0.0, 0.0, p));
}

TEST_CASE("massing: a mesh takes the box's rotation, and its turn on top") {
    const MassingBox box = flatBox(100, 200, 10, 20, 90, 0, 5);
    const MassingVec lo{0, 0, 0}, hi{4, 2, 3};
    const MassingVec mid{2, 1, 1.5};
    for (const double turn : {0.0, 90.0, -90.0, 180.0, 30.0}) {
        MassingKitPlace p;
        REQUIRE(massingKitPlacement(box, 1000.0, lo, hi, false, turn, 0.0, p));
        CHECK(near(p.yaw, -90.0 + turn));
        const MassingVec at = placedPoint(p, mid);
        CHECK(near(at.x, -400.0, 1e-9));
        CHECK(near(at.y, 300.0, 1e-9));
    }
    // By the numbers at no turn: yaw -90 turns the bounds' centre (2, 1) to (1, -2).
    MassingKitPlace p;
    REQUIRE(massingKitPlacement(box, 1000.0, lo, hi, false, 0.0, 0.0, p));
    CHECK(near(p.at.x, -401.0, 1e-9));
    CHECK(near(p.at.y, 302.0, 1e-9));

    // The mesh's +X runs along the box's +u: plan north at rot 90, which is engine -Y.
    const MassingVec end = placedPoint(p, {4, 1, 0});
    CHECK(near(end.x, -400.0, 1e-9));
    CHECK(near(end.y, 298.0, 1e-9));
}

TEST_CASE("massing: a mesh fitted to a box's footprint and height") {
    const MassingBox box = flatBox(0, 0, 10, 20, 0, 5, 15);
    MassingKitPlace p;
    REQUIRE(massingKitPlacement(box, 100.0, {-1, -2, 0}, {1, 2, 5}, true, 0.0, 0.0, p));
    CHECK(near(p.scale.x, 5.0));
    CHECK(near(p.scale.y, 5.0));
    CHECK(near(p.scale.z, 2.0));
    CHECK(near(placedPoint(p, {1, 2, 5}).z, 15.0));

    // An odd quarter turn lays the mesh's X along the box's w, either way round.
    for (const double turn : {90.0, -90.0, 270.0}) {
        REQUIRE(massingKitPlacement(box, 100.0, {0, 0, 0}, {4, 2, 5}, true, turn, 0.0, p));
        CHECK(near(p.scale.x, 5.0));
        CHECK(near(p.scale.y, 5.0));
    }
    REQUIRE(massingKitPlacement(box, 100.0, {0, 0, 0}, {4, 2, 5}, true, 180.0, 0.0, p));
    CHECK(near(p.scale.x, 2.5));
    CHECK(near(p.scale.y, 10.0));

    // Scale moves an off-centre pivot too: the bounds' centre still lands on the box.
    REQUIRE(massingKitPlacement(box, 100.0, {0, 0, -1}, {4, 2, 3}, true, 0.0, 0.0, p));
    const MassingVec at = placedPoint(p, {2, 1, 1});
    CHECK(near(at.x, -50.0, 1e-9));
    CHECK(near(at.y, 50.0, 1e-9));
    CHECK(near(placedPoint(p, {2, 1, -1}).z, 5.0, 1e-9));
}

TEST_CASE("massing: a sloped box's lowest and highest corners bound a mesh") {
    MassingBox box = flatBox(0, 0, 10, 4, 0, 2, 3);
    box.sloped = true;
    box.slopeFrom = 8.0;
    box.slopeTo = 4.0;
    box.slopeThick = 1.0;
    // The top runs 8 to 4 and the underside 1 m under it: 3 at the bottom, 8 at the top.
    MassingKitPlace p;
    REQUIRE(massingKitPlacement(box, 100.0, {-1, -1, 0}, {1, 1, 1}, true, 0.0, 0.0, p));
    CHECK(near(p.at.z, 3.0));
    CHECK(near(p.scale.z, 5.0));
}

TEST_CASE("massing: a mesh that can't be placed") {
    const MassingBox box = flatBox(0, 0, 10, 20, 0, 0, 10);
    MassingKitPlace p;
    p.yaw = 77.0;
    CHECK_FALSE(massingKitPlacement(box, 100.0, {1, 0, 0}, {0, 1, 1}, false, 0.0, 0.0, p)); // inverted
    CHECK_FALSE(massingKitPlacement(box, 100.0, {0, 0, 0}, {0, 1, 1}, true, 0.0, 0.0, p));  // no X to fit
    CHECK_FALSE(massingKitPlacement(box, 100.0, {0, 0, 0}, {1, 1, 0}, true, 0.0, 0.0, p));  // no Z to fit
    CHECK(p.yaw == 77.0); // left alone
}

TEST_CASE("massing: shared tops step down, smallest first") {
    std::vector<MassingBox> boxes = {
        flatBox(0, 0, 30, 30, 0, 0, 10),   // 0: big
        flatBox(0, 0, 10, 10, 0, 0, 10),   // 1: small, same top
        flatBox(5, 0, 20, 20, 0, 0, 10.005), // 2: medium, within 1 cm
        flatBox(0, 0, 5, 5, 0, 0, 12),     // 3: a different top
        flatBox(100, 0, 5, 5, 0, 0, 10),   // 4: same top, apart
        flatBox(0, 0, 8, 8, 0, 0, 10),     // 5: excluded
    };
    MassingBox sloped = flatBox(0, 0, 9, 9, 0, 0, 10);
    sloped.sloped = true;
    sloped.slopeFrom = sloped.slopeTo = 10.0;
    boxes.push_back(sloped); // 6: sloped, never nudged
    const std::vector<bool> include = {true, true, true, true, true, false, true};
    std::vector<double> nudge;
    massingTopNudges(boxes, include, nudge);
    REQUIRE(nudge.size() == boxes.size());
    CHECK(nudge[1] == 0.0);
    CHECK(near(nudge[2], -kMassingNudgeStep));
    CHECK(near(nudge[0], -2 * kMassingNudgeStep));
    CHECK(nudge[3] == 0.0);
    CHECK(nudge[4] == 0.0);
    CHECK(nudge[5] == 0.0);
    CHECK(nudge[6] == 0.0);
}

TEST_CASE("massing: the 1 m grid as a Landscape") {
    CHECK(massingLandscapeSide(1501, 1501, 126) == 1513);
    CHECK(massingLandscapeSide(127, 100, 126) == 127);
    CHECK(massingLandscapeSide(128, 2, 126) == 253);
    CHECK(massingLandscapeSide(1, 1, 126) == 127);
    CHECK(massingLandscapeSide(10, 10, 0) == 0);

    // 3 columns, 2 rows, south row first.
    const std::uint16_t grid[6] = {1, 2, 3, 4, 5, 6};
    std::uint16_t out[16];
    REQUIRE(massingLandscapeHeights(grid, 3, 2, 4, out));
    const std::uint16_t expect[16] = {
        4, 5, 6, 6, // the padding north repeats the north row
        4, 5, 6, 6, //
        4, 5, 6, 6, // the north row
        1, 2, 3, 3, // the south row, last
    };
    for (std::size_t i = 0; i < 16; ++i) CHECK(out[i] == expect[i]);
    CHECK_FALSE(massingLandscapeHeights(grid, 3, 2, 2, out));
    CHECK_FALSE(massingLandscapeHeights(nullptr, 3, 2, 4, out));

    // The 1501-sample grid from (-250, -250), padded to 1513, for a 1000 m square.
    const MassingVec corner = massingLandscapeCorner(1000, -250, -250, 1.0, 1513);
    CHECK(corner.x == -750.0);
    CHECK(corner.y == -762.0);

    CHECK(massingHeightFromValue(32768) == 0.0);
    CHECK(massingHeightFromValue(32768 + 4 * 128) == 4.0);
    CHECK(massingHeightFromValue(32768 - 33 * 128) == -33.0);
}

TEST_CASE("massing: the ground between samples, and the floor a player lands on") {
    // 3 columns, 2 rows from (10, 20), 2 m apart, south row first: heights 0, 2, 4 m south and 1, 3, 5 m north.
    const auto v = [](double m) { return static_cast<std::uint16_t>(32768 + m * 128); };
    const std::uint16_t grid[6] = {v(0), v(2), v(4), v(1), v(3), v(5)};
    CHECK(near(massingGroundAt(grid, 3, 2, 2.0, 10, 20, 10, 20), 0.0));
    CHECK(near(massingGroundAt(grid, 3, 2, 2.0, 10, 20, 14, 22), 5.0));
    CHECK(near(massingGroundAt(grid, 3, 2, 2.0, 10, 20, 11, 21), 1.5)); // the middle of the first cell
    CHECK(near(massingGroundAt(grid, 3, 2, 2.0, 10, 20, 13, 20), 3.0));
    CHECK(near(massingGroundAt(grid, 3, 2, 2.0, 10, 20, -50, -50), 0.0)); // clamped to the edge
    CHECK(near(massingGroundAt(grid, 3, 2, 2.0, 10, 20, 99, 21), 4.5));
    CHECK(massingGroundAt(nullptr, 3, 2, 2.0, 10, 20, 11, 21) == 0.0);
    CHECK(massingGroundAt(grid, 3, 2, 0.0, 10, 20, 11, 21) == 0.0);
    CHECK(massingGroundAt(grid, 3, 2, 2.0, 10, 20, std::nan(""), 21) == 0.0);

    // A deck at 5 m, a roof at 12 m over part of it, a ramp from 0 to 4 m turned 90 degrees (its u runs
    // north), and a box left out.
    std::vector<MassingBox> boxes = {flatBox(0, 0, 10, 10, 0, 0, 5), flatBox(2, 0, 4, 4, 0, 5, 12),
                                     flatBox(30, 0, 8, 2, 90, 0, 0), flatBox(-20, 0, 4, 4, 0, 0, 50)};
    boxes[2].sloped = true;
    boxes[2].slopeFrom = 0.0;
    boxes[2].slopeTo = 4.0;
    const std::vector<bool> include = {true, true, true, false};
    CHECK(near(massingFloorAt(boxes, include, -3, 0, 1.0, 100), 5.0));
    CHECK(near(massingFloorAt(boxes, include, 2, 0, 1.0, 100), 12.0));
    CHECK(near(massingFloorAt(boxes, include, 2, 0, 1.0, 6.0), 5.0)); // the roof is over the ceiling
    CHECK(near(massingFloorAt(boxes, include, 2, 0, 7.0, 6.0), 7.0)); // the ground is never lowered
    CHECK(near(massingFloorAt(boxes, include, 30, 0, -1.0, 100), 2.0));
    CHECK(near(massingFloorAt(boxes, include, 30, 3, -1.0, 100), 3.5));
    CHECK(near(massingFloorAt(boxes, include, 30, -4, -1.0, 100), 0.0));
    CHECK(near(massingFloorAt(boxes, include, 31.5, 0, -1.0, 100), -1.0)); // off the ramp's 2 m width
    CHECK(near(massingFloorAt(boxes, include, -20, 0, 1.0, 100), 1.0));    // left out
    CHECK(near(massingFloorAt(boxes, {true}, 2, 0, 1.0, 100), 5.0));       // include shorter than the table
    int on = 99;
    CHECK(near(massingFloorAt(boxes, include, 2, 0, 1.0, 6.0, &on), 5.0));
    CHECK(on == 0);
    CHECK(near(massingFloorAt(boxes, include, 50, 50, 1.0, 100, &on), 1.0));
    CHECK(on == -1);

    // Standing on the deck under the roof (which starts at 5 m): inside the roof's box. On the roof, or
    // on the ground beside the deck, nothing is in the way; on the ground under the deck, the deck is.
    CHECK(massingBlockingAt(boxes, include, 2, 0, 5.0, 1.8) == 1);
    CHECK(massingBlockingAt(boxes, include, 2, 0, 12.0, 1.8) == -1);
    CHECK(massingBlockingAt(boxes, include, -3, 0, 5.0, 1.8) == -1);
    CHECK(massingBlockingAt(boxes, include, 8, 0, 0.0, 1.8) == -1);
    CHECK(massingBlockingAt(boxes, include, -3, 0, 0.0, 1.8) == 0);
    // On the ramp at 2 m its top is the floor; a metre lower the ramp is in the way.
    CHECK(massingBlockingAt(boxes, include, 30, 0, 2.0, 1.8) == -1);
    CHECK(massingBlockingAt(boxes, include, 30, 0, 1.0, 1.8) == 2);
    CHECK(massingBlockingAt(boxes, include, -20, 0, 1.0, 1.8) == -1); // left out
}

TEST_CASE("massing: a spawn's footing") {
    // 21 columns by 5 rows from (0, 0), 1 m apart: the ground is 3 m high up to x = 4 and drops in one cell to 0 m
    // from x = 5 on.
    const auto v = [](double m) { return static_cast<std::uint16_t>(32768 + m * 128); };
    std::vector<std::uint16_t> grid(21 * 5);
    for (std::size_t r = 0; r < 5; ++r)
        for (std::size_t c = 0; c < 21; ++c) grid[r * 21 + c] = v(c <= 4 ? 3.0 : 0.0);
    const auto footing = [&](const std::vector<MassingBox>& boxes, const std::vector<bool>& include, double x,
                             double y, double floor) {
        return massingSpawnFooting(grid.data(), 21, 5, 1.0, 0.0, 0.0, boxes, include, x, y, floor, floor + 6.0, 1.8);
    };
    const std::vector<MassingBox> none;
    const std::vector<bool> noneIncluded;

    // On the drop's top sample: half a metre east is half way down it.
    MassingFooting f = footing(none, noneIncluded, 4.0, 2.0, 3.0);
    CHECK(near(f.step, 1.5));
    CHECK(near(f.x, 4.5));
    CHECK(near(f.y, 2.0));
    CHECK(f.skipped == 0);
    CHECK_FALSE(massingFootingHolds(f));
    // Two metres past the drop the ground is flat.
    f = footing(none, noneIncluded, 7.0, 2.0, 0.0);
    CHECK(near(f.step, 0.0));
    CHECK(near(f.x, 7.0));
    CHECK(near(f.y, 2.0));
    CHECK(massingFootingHolds(f));
    // A floor that isn't the ground's steps at the spawn itself.
    f = footing(none, noneIncluded, 7.0, 2.0, 1.0);
    CHECK(near(f.step, 1.0));
    CHECK(near(f.x, 7.0));
    CHECK_FALSE(massingFootingHolds(f));
    // Just inside the tolerance, and just over it.
    CHECK(massingFootingHolds(footing(none, noneIncluded, 7.0, 2.0, kMassingFootingTolerance)));
    CHECK_FALSE(massingFootingHolds(footing(none, noneIncluded, 7.0, 2.0, kMassingFootingTolerance + 0.01)));

    // A 2 m square box on the flat with its top at 5 m: at its centre the footing holds; 0.3 m from its east
    // edge, half a metre east is off the box and 5 m down.
    std::vector<MassingBox> boxes = {flatBox(15, 2, 2, 2, 0, 0, 5)};
    std::vector<bool> include = {true};
    f = footing(boxes, include, 15.0, 2.0, 5.0);
    CHECK(near(f.step, 0.0));
    CHECK(f.skipped == 0);
    CHECK(massingFootingHolds(f));
    f = footing(boxes, include, 15.7, 2.0, 5.0);
    CHECK(near(f.step, 5.0));
    CHECK(near(f.x, 16.2));
    CHECK(near(f.y, 2.0));
    CHECK_FALSE(massingFootingHolds(f));
    // Standing inside the box on the ground under it, every point is in the way: none is measured.
    f = footing(boxes, include, 15.0, 2.0, 0.0);
    CHECK(f.skipped == kMassingFootingPoints);
    CHECK(near(f.step, 0.0));

    // A box with its top at 4 m (under the ceiling) whose west face stands 0.2 m east of a ground spawn: the
    // four points on it are a wall in the way, skipped, not a 4 m step. Left out, it isn't there at all.
    boxes = {flatBox(12.2, 2, 2, 2, 0, 0, 4)};
    f = footing(boxes, include, 11.0, 2.0, 0.0);
    CHECK(f.skipped == 4);
    CHECK(near(f.step, 0.0));
    CHECK(massingFootingHolds(f));
    f = footing(boxes, {false}, 11.0, 2.0, 0.0);
    CHECK(f.skipped == 0);
    CHECK(near(f.step, 0.0));

    // No radius (zero, negative or not a number) reads the spawn alone: on the drop's top sample, nothing steps.
    for (const double radius : {0.0, -1.0, std::nan("")}) {
        f = massingSpawnFooting(grid.data(), 21, 5, 1.0, 0.0, 0.0, none, noneIncluded, 4.0, 2.0, 3.0, 9.0, 1.8, radius);
        CHECK(near(f.step, 0.0));
    }
    // A wider radius reaches further down the drop.
    f = massingSpawnFooting(grid.data(), 21, 5, 1.0, 0.0, 0.0, none, noneIncluded, 4.0, 2.0, 3.0, 9.0, 1.8, 1.0);
    CHECK(near(f.step, 3.0));
    CHECK(near(f.x, 5.0));

    // An empty grid reads a flat ground at 0.
    f = massingSpawnFooting(nullptr, 0, 0, 1.0, 0.0, 0.0, none, noneIncluded, 3.0, 3.0, 0.0, 6.0, 1.8);
    CHECK(near(f.step, 0.0));
    CHECK(massingFootingHolds(f));
    f = massingSpawnFooting(nullptr, 0, 0, 1.0, 0.0, 0.0, none, noneIncluded, 3.0, 3.0, 2.0, 8.0, 1.8);
    CHECK(near(f.step, 2.0));
}
