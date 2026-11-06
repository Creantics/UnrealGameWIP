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
