// Road networks (ftcore/Roads.h roadNetBuild): the network table, junction fills and corners, the carriageway kept
// clear, bays, steps, grades, decks and the junction ids the road graph shares. Made-up tables only.

#include "ftcore/RoadGraph.h"
#include "ftcore/Roads.h"

#include <ostream>
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

// A T: a 14 m street east-west, parking, and a 14 m street north from its middle.
const char* const kTee =
    "# a test net\n"
    "id,kind,width_m,pts,pavement_m,speed_kmh,one_way,deck_z,parking\n"
    "ST-H,street,14,0:0;200:0,2.8,40,0,,1\n"
    "ST-V,street,14,100:0;100:150,2.8,40,0,,0\n";

std::vector<RoadNetStreet> net(const char* csv) {
    std::vector<RoadNetStreet> s;
    std::string err;
    REQUIRE_MESSAGE(importRoadNet(csv, s, &err), err);
    return s;
}

MassingFoot shrunk(MassingFoot f, double by) {
    f.hx = std::max(0.0, f.hx - by);
    f.hy = std::max(0.0, f.hy - by);
    return f;
}

MassingFoot solidFoot(const RoadSolid& s) {
    MassingFoot f;
    f.x = s.box.cx;
    f.y = s.box.cy;
    f.hx = s.box.sx * 0.5;
    f.hy = s.box.sy * 0.5;
    f.rot = s.box.rot;
    return f;
}

} // namespace

TEST_CASE("road net: the table reads, and refuses bad rows") {
    const std::vector<RoadNetStreet> s = net(kTee);
    REQUIRE(s.size() == 2);
    CHECK(s[0].pts.size() == 2);
    CHECK(s[0].park);
    CHECK_FALSE(s[1].park);
    CHECK_FALSE(s[0].deck);
    CHECK(roadNetTable(kTee));
    CHECK_FALSE(roadNetTable("id,kind,width_m,pts,pavement_m,lane_m,ground_z\n"));

    std::vector<RoadNetStreet> out;
    const char* head = "id,kind,width_m,pts,pavement_m,deck_z\n";
    CHECK_FALSE(importRoadNet(std::string(head) + "A,street,14,0:0,2.8,\n", out));          // one point
    CHECK_FALSE(importRoadNet(std::string(head) + "A,street,14,0:0;50:0,0,\n", out));       // no pavement
    CHECK_FALSE(importRoadNet(std::string(head) + "A,street,10,0:0;50:0,2.5,\n", out));     // 5 m carriageway
    CHECK_FALSE(importRoadNet(std::string(head) + "A,street,14,0:0;50:0,2.8,\nA,street,14,0:9;50:9,2.8,\n", out));
    CHECK_FALSE(importRoadNet(std::string(head) + "A,street,14,0:0;50:0,2.8,x\n", out));    // bad deck
    REQUIRE(importRoadNet(std::string(head) + "A,street,14,0:0;50:0;50:60,2.8,9\n", out));
    CHECK(out[0].deck);
    CHECK(out[0].deckZ == doctest::Approx(9.0));
    CHECK(out[0].pts.size() == 3);
}

TEST_CASE("road net: a T is signalised, its corners kerbed at 6 m, its ids the road graph's") {
    const std::vector<RoadNetStreet> s = net(kTee);
    RoadBuild b;
    RoadNetReport rep;
    roadNetBuild(s, MassingGround{}, b, &rep);
    CHECK(rep.junctions == 1);
    CHECK(rep.ends == 3);
    CHECK(rep.edges == 3);
    CHECK(rep.steep == 0);
    CHECK(rep.pokes == 0);
    REQUIRE(b.junctions.size() == 1);
    CHECK(b.junctions[0].id == "JN-0001");
    CHECK(b.crossings.size() == 3);
    for (const RoadQuad& q : b.crossings) CHECK(q.junction == "JN-0001");

    // The road graph numbers the same junction the same way, and keeps the build's zebras and bays.
    RoadGraph g;
    roadGraphFromNet(s, b, g);
    REQUIRE(g.junctions.size() == 1);
    CHECK(g.junctions[0].id == "JN-0001");
    CHECK_FALSE(g.junctions[0].phases.empty());
    CHECK(g.crossings.size() == 3);
    CHECK(g.bays.size() == b.bays.size());

    // The north-east corner: kerb pieces on a 6 m circle about (100 + 4.2 + 6, 4.2 + 6).
    int onCircle = 0;
    for (const RoadSolid& sd : b.solids) {
        if (sd.box.kind != "kerb" || sd.box.row != "JN-0001") continue;
        const double r = std::hypot(sd.box.cx - 110.2, sd.box.cy - 10.2);
        if (std::fabs(r - (kRoadCornerRadius - kRoadKerbWide * 0.5)) < 0.15) ++onCircle;
    }
    CHECK(onCircle >= 4);

    // Signals: a vehicle signal per arm and a pedestrian signal at each end of each zebra, tagged with the junction.
    int vehicle = 0, ped = 0;
    for (const RoadSolid& sd : b.solids) {
        if (sd.tag.rfind("FTSignal_JN-0001_", 0) == 0 && sd.tag.find("_red") != std::string::npos) ++vehicle;
        if (sd.tag.rfind("FTPedSignal_JN-0001_", 0) == 0 && sd.tag.find("_red") != std::string::npos) ++ped;
    }
    CHECK(vehicle == 3);
    CHECK(ped == 6);
}

TEST_CASE("road net: nothing solid stands in the carriageway; furniture keeps off the kerb; bays stay off it") {
    const std::vector<RoadNetStreet> s = net(
        "id,kind,width_m,pts,pavement_m,parking\n"
        "ST-H,street,18,0:0;240:0,3.6,1\n"
        "ST-V,street,14,120:-150;120:150,2.8,0\n"
        "ST-B,street,14,0:0;-60:60;-60:200,2.8,0\n");
    RoadBuild b;
    RoadNetReport rep;
    roadNetBuild(s, MassingGround{}, b, &rep);
    CHECK(rep.junctions == 1);
    CHECK(rep.bends == 2);
    CHECK(b.crossings.size() == 4);
    CHECK(b.bays.size() > 4);
    for (const RoadSolid& sd : b.solids) {
        if (!sd.collide || sd.tint == RoadTint::Asphalt || sd.box.kind == "deck" || sd.box.kind == "pier") continue;
        INFO(sd.box.id << " " << sd.box.kind);
        CHECK_FALSE(roadInCarriageway(b, shrunk(solidFoot(sd), 0.1), 0.0));
    }
    for (const MassingFoot& f : b.furniture) {
        INFO(f.x << ", " << f.y);
        CHECK_FALSE(roadInCarriageway(b, f, kRoadKerbClear));
    }
    for (const RoadBay& bay : b.bays) {
        CHECK(bay.street == "ST-H");
        CHECK_FALSE(roadInCarriageway(b, shrunk(bay.foot, 0.02), 0.0));
    }
    // The step where a crowned arm meets the level fill.
    CHECK(rep.maxStep <= kRoadCrossfall * 0.5 * 5.4 + 1e-9);
}

TEST_CASE("road net: pieces follow the ground and report what is too steep; decks stand on piers") {
    // The ground rises 10 % eastwards from x = 100 to x = 200, flat elsewhere (5 m samples).
    const std::size_t nx = 81, ny = 21;
    std::vector<std::uint16_t> grid(nx * ny);
    for (std::size_t r = 0; r < ny; ++r)
        for (std::size_t c = 0; c < nx; ++c) {
            const double xm = static_cast<double>(c) * 5.0;
            const double h = xm < 100.0 ? 0.0 : (xm > 200.0 ? 10.0 : (xm - 100.0) * 0.1);
            grid[r * nx + c] = static_cast<std::uint16_t>(32768 + std::lround(h * 128.0));
        }
    MassingGround ground;
    ground.grid = grid.data();
    ground.samplesX = nx;
    ground.samplesY = ny;
    ground.spacing = 5.0;
    ground.originX = 0.0;
    ground.originY = -50.0;

    RoadBuild b;
    RoadNetReport rep;
    roadNetBuild(net("id,kind,width_m,pts,pavement_m\nST-R,street,14,10:0;390:0,2.8\n"), ground, b, &rep);
    CHECK(rep.steep >= 1);
    CHECK(rep.maxGrade == doctest::Approx(0.1).epsilon(0.05));
    CHECK(rep.pieces >= 3);
    CHECK(rep.pokes == 0);
    CHECK_FALSE(rep.notes.empty());
    // The asphalt over x = 150 sits on the ground there (5 m) plus the lift and crown.
    bool found = false;
    for (const RoadSolid& sd : b.solids)
        if (sd.box.kind == "asphalt" && std::fabs(sd.box.cx - 150.0) < 30.0) found = true;
    CHECK(found);

    RoadBuild d;
    RoadNetReport drep;
    roadNetBuild(net("id,kind,width_m,pts,pavement_m,deck_z,parking\nBR-1,bridge,14,0:0;0:200,2.8,12,1\n"),
                 MassingGround{}, d, &drep);
    CHECK(drep.piers >= 10);
    CHECK(drep.steep == 0);
    CHECK(d.bays.empty());
    int rails = 0;
    for (const RoadSolid& sd : d.solids)
        if (sd.box.kind == "rail") ++rails;
    CHECK(rails == 2);
    for (const RoadSolid& sd : d.solids)
        if (sd.box.kind == "asphalt") CHECK(sd.box.z1 > 12.0);
}

TEST_CASE("road net: lane mouths drop the kerb on their side, across edges") {
    // A street north with a T at y 100 (east) and lane mouths west at 50 and 150 m along it.
    const std::vector<RoadNetStreet> s = net(
        "id,kind,width_m,pts,pavement_m,mouths,mouth_back_m,mouth_z\n"
        "RD-X,road,14,0:0;0:200,3.5,LN-1@50@6@L;LN-2@150@6@L,1.5,0\n"
        "ST-E,street,14,0:100;80:100,2.8,,,\n");
    REQUIRE(s[0].mouths.size() == 2);
    RoadBuild b;
    roadNetBuild(s, MassingGround{}, b);
    int cross = 0;
    for (const RoadSolid& sd : b.solids)
        if (sd.box.kind == "crossover") {
            ++cross;
            CHECK(sd.box.cx < -3.5); // the west side
        }
    CHECK(cross == 2);
    std::vector<RoadNetStreet> out;
    CHECK_FALSE(importRoadNet("id,kind,width_m,pts,pavement_m,mouths\nA,road,14,0:0;0:50,3.5,LN@5@6@Q\n", out));
}
