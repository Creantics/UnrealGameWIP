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

TEST_CASE("road net: a junction's surface has no holes round its corners and no step where an arm meets its fill") {
    RoadBuild b;
    roadNetBuild(net(kTee), MassingGround{}, b, nullptr);
    // The top of the road's surface (asphalt, kerb, pavement) over a 36 m square round the node, every 0.2 m.
    constexpr int n = 180;
    constexpr double res = 0.2, x0 = 82.0, y0 = -18.0;
    std::vector<double> top(n * n, -1e9);
    std::vector<char> tar(n * n, 0);
    for (const RoadSolid& sd : b.solids) {
        const MassingBox& x = sd.box;
        if (!sd.collide || (sd.tint != RoadTint::Asphalt && sd.tint != RoadTint::Kerb && sd.tint != RoadTint::Pavement))
            continue;
        const double ra = x.rot * 3.14159265358979323846 / 180.0, c = std::cos(ra), sn = std::sin(ra);
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const double dx = x0 + (i + 0.5) * res - x.cx, dy = y0 + (j + 0.5) * res - x.cy;
                double z = x.z1;
                if (sd.shape == RoadShape::Cylinder) {
                    if (dx * dx + dy * dy > x.sx * x.sx * 0.25) continue;
                } else {
                    const double u = dx * c + dy * sn, w = -dx * sn + dy * c;
                    if (std::fabs(u) > x.sx * 0.5 || std::fabs(w) > x.sy * 0.5) continue;
                    z += x.riseU * u + x.riseW * w;
                }
                const std::size_t k = static_cast<std::size_t>(j * n + i);
                if (z > top[k]) {
                    top[k] = z;
                    tar[k] = sd.tint == RoadTint::Asphalt ? 1 : 0;
                }
            }
    }
    // No hole: every uncovered cell is joined to the square's edge (the blocks behind the pavements).
    std::vector<char> seen(n * n, 0);
    std::vector<int> todo;
    for (int i = 0; i < n; ++i)
        for (const int k : {i, (n - 1) * n + i, i * n, i * n + n - 1})
            if (top[static_cast<std::size_t>(k)] < -1e8 && !seen[static_cast<std::size_t>(k)]) {
                seen[static_cast<std::size_t>(k)] = 1;
                todo.push_back(k);
            }
    while (!todo.empty()) {
        const int k = todo.back();
        todo.pop_back();
        const int i = k % n, j = k / n;
        for (const int m : {i > 0 ? k - 1 : -1, i + 1 < n ? k + 1 : -1, j > 0 ? k - n : -1, j + 1 < n ? k + n : -1})
            if (m >= 0 && top[static_cast<std::size_t>(m)] < -1e8 && !seen[static_cast<std::size_t>(m)]) {
                seen[static_cast<std::size_t>(m)] = 1;
                todo.push_back(m);
            }
    }
    int holes = 0;
    for (std::size_t k = 0; k < top.size(); ++k)
        if (top[k] < -1e8 && !seen[k]) ++holes;
    CHECK(holes == 0);
    // No step on the asphalt: neighbouring cells differ by no more than the crossfall over a cell (and a little).
    double step = 0.0;
    for (int j = 0; j + 1 < n; ++j)
        for (int i = 0; i + 1 < n; ++i) {
            const std::size_t k = static_cast<std::size_t>(j * n + i);
            for (const std::size_t m : {k + 1, k + static_cast<std::size_t>(n)})
                if (tar[k] && tar[m]) step = std::max(step, std::fabs(top[k] - top[m]));
        }
    CHECK(step < 0.006);
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
    // The fill carries the arms' crown: no step where an arm meets it.
    CHECK(rep.maxStep == 0.0);
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

TEST_CASE("road net: kerbs and pavements are cut at every mouth; no faces, no steps") {
    // A T, a cross with bends, and lane mouths: no kerb or pavement over 3 cm across a lane, in a carriageway or
    // stepping against its neighbour.
    const char* nets[] = {kTee,
                          "id,kind,width_m,pts,pavement_m,parking\n"
                          "ST-H,street,18,0:0;240:0,3.6,1\n"
                          "ST-V,street,14,120:-150;120:150,2.8,0\n"
                          "ST-B,street,14,0:0;-60:60;-60:200,2.8,0\n"};
    for (const char* csv : nets) {
        const std::vector<RoadNetStreet> s = net(csv);
        RoadBuild b;
        RoadNetReport rep;
        roadNetBuild(s, MassingGround{}, b, &rep);
        RoadGraph g;
        roadGraphFromNet(s, b, g);
        REQUIRE_FALSE(g.lanes.empty());
        for (const RoadFace& f : roadKerbFaces(b, &g)) {
            INFO(f.piece << " at " << f.x << ", " << f.y << ": " << f.fault);
            CHECK(false);
        }
    }

    // Lane mouths (6 m): a dropped kerb flush with the gutter (0 to 2 cm), and the lane's centre line crosses no kerb.
    const std::vector<RoadNetStreet> s = net("id,kind,width_m,pts,pavement_m,mouths,mouth_back_m,mouth_z\n"
                                             "RD-X,road,14,0:0;0:200,3.5,LN-1@50@6@L;LN-2@150@6@L,1.5,0\n");
    RoadBuild b;
    roadNetBuild(s, MassingGround{}, b);
    RoadGraph g;
    roadGraphFromNet(s, b, g);
    for (const double y : {50.0, 150.0}) {
        RoadGraphLane lane;
        lane.id = "LN-TEST";
        lane.width = 6.0;
        lane.pts = {{-20.0, y}, {-3.0, y}};
        g.lanes.push_back(lane);
    }
    for (const RoadFace& f : roadKerbFaces(b, &g)) {
        INFO(f.piece << " at " << f.x << ", " << f.y << ": " << f.fault);
        CHECK(false);
    }

    // A kerb laid across the road is caught, named with the point where the lane crosses it.
    RoadSolid bad = b.solids.front();
    for (const RoadSolid& sd : b.solids)
        if (sd.box.kind == "kerb") bad = sd;
    bad.box.id = "TEST-kerb";
    bad.box.cx = 0.0;
    bad.box.cy = 100.0;
    bad.box.sx = 14.0;
    bad.box.sy = 0.3;
    bad.box.rot = 0.0;
    bad.box.riseU = bad.box.riseW = 0.0;
    b.solids.push_back(bad);
    bool named = false;
    for (const RoadFace& f : roadKerbFaces(b, &g))
        if (f.piece == "TEST-kerb" && std::fabs(f.y - 100.0) < 0.2 && std::fabs(f.x) < 3.5) named = true;
    CHECK(named);
    // The commandlet's -Kerbs pieces are tested the same way.
    std::vector<MassingBox> kerbs(2, bad.box);
    kerbs[1].cx = -30.0; // off the road
    const std::vector<std::size_t> in = roadKerbsInCarriageway(b, &g, kerbs);
    REQUIRE(in.size() == 1);
    CHECK(in[0] == 0);
}

TEST_CASE("road net: a bend's outer ring keeps the kerb line without a jog") {
    // Two widths meeting at a bend: the ring goes from one's kerb line to the other's, no face and no step.
    const std::vector<RoadNetStreet> s = net("id,kind,width_m,pts,pavement_m\n"
                                             "ST-A,street,18,0:0;100:0,3.6\n"
                                             "ST-B,street,14,100:0;160:80,2.8\n");
    RoadBuild b;
    RoadNetReport rep;
    roadNetBuild(s, MassingGround{}, b, &rep);
    CHECK(rep.bends >= 1);
    RoadGraph g;
    roadGraphFromNet(s, b, g);
    for (const RoadFace& f : roadKerbFaces(b, &g)) {
        INFO(f.piece << " at " << f.x << ", " << f.y << ": " << f.fault);
        CHECK(false);
    }
}

TEST_CASE("road net: parked vehicles keep out of junction boxes and crossings; the graph reads back") {
    const std::vector<RoadNetStreet> s = net("id,kind,width_m,pts,pavement_m,parking\n"
                                             "ST-H,street,18,0:0;240:0,3.6,1\n"
                                             "ST-V,street,14,120:-150;120:150,2.8,0\n");
    RoadBuild b;
    roadNetBuild(s, MassingGround{}, b);
    std::vector<RoadVehicle> cars;
    const RoadParkCounts c = roadParkVehicles(b, {}, nullptr, 11U, cars);
    INFO("bays " << b.bays.size() << " parked " << c.parked << " empty " << c.empty << " blocked " << c.blocked);
    REQUIRE_FALSE(cars.empty());
    for (const RoadVehicle& v : cars) {
        const MassingFoot f = shrunk(roadVehicleFoot(v), 0.01);
        for (const MassingFoot& k : b.keepOut) CHECK_FALSE(massingFeetOverlap(f, k, 0.0));
        for (const RoadJunction& j : b.junctions) CHECK_FALSE(massingFeetOverlap(f, j.box, 0.0));
    }
    // The written graph (its bays' ids are lower case) reads back as it was.
    RoadGraph g;
    roadGraphFromNet(s, b, g);
    REQUIRE_FALSE(g.bays.empty());
    RoadGraph back;
    std::string err;
    REQUIRE_MESSAGE(importRoadGraph(roadGraphWrite(g), back, &err), err);
    CHECK(back.lanes.size() == g.lanes.size());
    CHECK(back.bays.size() == g.bays.size());
    CHECK(back.crossings.size() == g.crossings.size());
}

TEST_CASE("road net: poles and signs on the network, laid as instances, off the carriageway") {
    // A street with poles on its left, a T, and a lane mouth on the right: speed signs past the junction, a stop sign
    // at the mouth, the centre line broken across it.
    const std::vector<RoadNetStreet> s = net(
        "id,kind,width_m,pts,pavement_m,mouths,poles\n"
        "RD-P,road,16,0:0;0:300,3.5,LN-1@220@6@R,L\n"
        "ST-E,street,14,0:100;80:100,2.8,,\n");
    REQUIRE(s[0].poleLeft);
    REQUIRE_FALSE(s[0].poleRight);
    RoadBuild b;
    roadNetBuild(s, MassingGround{}, b);
    CHECK(b.counts.poles >= 6);
    CHECK(b.counts.signs >= 3);
    int poles = 0, wires = 0, stops = 0;
    for (const RoadSolid& sd : b.solids) {
        if (sd.box.kind == "utilitypole") {
            ++poles;
            CHECK(sd.instanced);
            CHECK(sd.box.cx < -6.0); // the street's left, running north: the west side
        }
        if (sd.box.kind == "wire") ++wires;
        if (sd.tag == "FTRoadSign_stop") ++stops;
        if (sd.box.kind == "sign" || sd.box.kind == "signpost" || sd.box.kind == "crossarm" || sd.box.kind == "wire")
            CHECK(sd.instanced);
        if (sd.box.kind == "kerb" || sd.box.kind == "pavement") CHECK_FALSE(sd.instanced);
    }
    CHECK(poles == static_cast<int>(b.counts.poles));
    CHECK(wires >= 3 * (poles - 2));
    CHECK(stops == 1);
    for (const MassingFoot& f : b.furniture) CHECK_FALSE(roadInCarriageway(b, f, kRoadKerbClear));
    // No centre dash across the mouth (y 220 +- 3 m and its flares).
    for (const RoadSolid& sd : b.solids)
        if (sd.tint == RoadTint::LineWhite && std::fabs(sd.box.cx) < 0.2 && sd.box.sx < 4.0) {
            INFO(sd.box.cy);
            CHECK(std::fabs(sd.box.cy - 220.0) > 3.0);
        }
    std::vector<RoadNetStreet> out;
    CHECK_FALSE(importRoadNet("id,kind,width_m,pts,pavement_m,poles\nA,road,14,0:0;0:50,3.5,Q\n", out));
}

TEST_CASE("road net: a tilted cube's top is centred over its box, so a crown's two halves meet") {
    MassingBox x;
    x.cx = 10.0;
    x.cy = -4.0;
    x.sx = 20.0;
    x.sy = 3.5;
    x.rot = 30.0;
    x.z0 = 1.0;
    x.z1 = 4.3;
    const MassingVec level = roadCubeCentre(x, 3.3);
    CHECK(level.x == doctest::Approx(10.0));
    CHECK(level.y == doctest::Approx(-4.0));
    CHECK(level.z == doctest::Approx(2.65));
    x.riseU = 0.08;
    x.riseW = -kRoadCrossfall;
    const MassingCubePose pose = massingCubePose(x, 3.3);
    const double p = pose.pitch * 3.14159265358979323846 / 180.0, r = pose.roll * 3.14159265358979323846 / 180.0;
    const double a = x.rot * 3.14159265358979323846 / 180.0;
    // Local Z in (u, w, z), then the top's middle half the height along it from the centre.
    const double zu = -std::cos(r) * std::sin(p), zw = -std::sin(r), zz = std::cos(r) * std::cos(p);
    const MassingVec c = roadCubeCentre(x, 3.3);
    CHECK(c.x + 1.65 * (zu * std::cos(a) - zw * std::sin(a)) == doctest::Approx(10.0).epsilon(1e-9));
    CHECK(c.y + 1.65 * (zu * std::sin(a) + zw * std::cos(a)) == doctest::Approx(-4.0).epsilon(1e-9));
    CHECK(c.z + 1.65 * zz == doctest::Approx(4.3).epsilon(1e-9));
    // Off the plumb line by more than a centimetre: the slit it closes.
    CHECK(std::hypot(c.x - 10.0, c.y + 4.0) > 0.01);
}

TEST_CASE("road net: a square junction corner's pavement carries on into the block's corner as an apron") {
    RoadBuild b;
    RoadNetReport rep;
    roadNetBuild(net(kTee), MassingGround{}, b, &rep);
    // The T's two corners (north of ST-H, either side of ST-V): back lines at y 7 and x 100 -/+ 7.
    CHECK(rep.aprons == 2);
    int found = 0;
    for (const RoadSolid& sd : b.solids) {
        const MassingBox& x = sd.box;
        if (x.kind != "pavement" || std::fabs(x.sx - (kRoadCornerApron + kRoadApronLap)) > 1e-6) continue;
        CHECK(std::fabs(x.sy - (kRoadCornerApron + kRoadApronLap)) < 1e-6);
        const double mid = 7.0 + (kRoadCornerApron - kRoadApronLap) * 0.5; // back line 7 (14 m wide, pavements in it)
        CHECK(std::fabs(x.cy - mid) < 1e-6);
        CHECK(std::fabs(std::fabs(x.cx - 100.0) - mid) < 1e-6);
        CHECK(std::fabs(x.z1 - (kRoadSurfaceLift + kRoadKerbRise)) < 1e-9);
        CHECK(sd.tint == RoadTint::Pavement);
        ++found;
    }
    CHECK(found == 2);
}

TEST_CASE("road net: a scramble gets an all-walk phase and two diagonal zebras laid on its fill") {
    const char* const csv = "id,kind,width_m,pts,pavement_m,speed_kmh,one_way,deck_z,parking,scramble\n"
                            "ST-H,street,14,0:0;200:0,2.8,40,0,,0,100.5:0.5\n"
                            "ST-V,street,14,100:0;100:150,2.8,40,0,,0,\n";
    const std::vector<RoadNetStreet> s = net(csv);
    REQUIRE(s[0].scramble.size() == 1);
    RoadBuild b;
    RoadNetReport rep;
    roadNetBuild(s, MassingGround{}, b, &rep);
    CHECK(rep.junctions == 1);
    CHECK(rep.scrambles == 1);
    CHECK(b.crossings.size() == 5);
    int diag = 0;
    for (const RoadQuad& q : b.crossings) {
        if (q.id.find("-ZD") == std::string::npos) continue;
        ++diag;
        CHECK(q.junction == "JN-0001");
        const double dx = q.pts[1].x - q.pts[0].x, dy = q.pts[1].y - q.pts[0].y;
        const double a = std::atan2(std::fabs(dy), std::fabs(dx)) * 180.0 / 3.14159265358979;
        CHECK(a >= 25.0);
        CHECK(a <= 65.0);
        // Its walk line's ends inside the carriageway, near the centre.
        for (const RoadPoint& p : {RoadPoint{(q.pts[0].x + q.pts[3].x) * 0.5, (q.pts[0].y + q.pts[3].y) * 0.5},
                                   RoadPoint{(q.pts[1].x + q.pts[2].x) * 0.5, (q.pts[1].y + q.pts[2].y) * 0.5}}) {
            CHECK(p.y > -4.2);
            CHECK(std::hypot(p.x - 100.0, p.y) < 20.0);
        }
    }
    CHECK(diag == 2);
    // Every pedestrian head on the all-walk phase; vehicle phases unchanged.
    int walk = 0, other = 0, vehicle = 0;
    for (const RoadSolid& sd : b.solids) {
        if (sd.tag.rfind("FTPedSignal_JN-0001_", 0) == 0 && sd.tag.find("_red") != std::string::npos)
            (sd.tag.rfind("FTPedSignal_JN-0001_W_", 0) == 0 ? walk : other)++;
        if (sd.tag.rfind("FTSignal_JN-0001_", 0) == 0 && sd.tag.find("_red") != std::string::npos) ++vehicle;
    }
    CHECK(walk == 6);
    CHECK(other == 0);
    CHECK(vehicle == 3);
    // The diagonal bars sit on the fill: none under its top, none more than 3 cm over it.
    std::vector<const RoadSolid*> fill;
    for (const RoadSolid& sd : b.solids)
        if (sd.box.row == "JN-0001" && sd.box.kind == "fill") fill.push_back(&sd);
    int bars = 0;
    for (const RoadSolid& sd : b.solids) {
        if (sd.box.row != "JN-0001" || sd.box.kind != "mark") continue;
        ++bars;
        CHECK_FALSE(sd.collide);
        CHECK(sd.box.z1 > 0.0);
        CHECK(sd.box.z1 < kRoadSurfaceLift + kRoadCrossfall * 4.2 + 0.05);
        // Never over a kerb: south of the main road's north kerb only between the corners, never inside a corner's kerb.
        CHECK(sd.box.cy > -4.2);
        CHECK(std::hypot(sd.box.cx - 110.2, sd.box.cy - 10.2) > kRoadCornerRadius);
        CHECK(std::hypot(sd.box.cx - 89.8, sd.box.cy - 10.2) > kRoadCornerRadius);
    }
    CHECK(bars >= 40);

    RoadGraph g;
    roadGraphFromNet(s, b, g);
    REQUIRE(g.junctions.size() == 1);
    CHECK(g.junctions[0].phases.find(";W:all-walk") != std::string::npos);
    CHECK(g.crossings.size() == 5);

    // Without the column: no all-walk phase, no diagonals.
    const std::vector<RoadNetStreet> plain = net(kTee);
    RoadBuild pb;
    roadNetBuild(plain, MassingGround{}, pb);
    RoadGraph pg;
    roadGraphFromNet(plain, pb, pg);
    REQUIRE(pg.junctions.size() == 1);
    CHECK(pg.junctions[0].phases.find("W:") == std::string::npos);
    CHECK(pb.crossings.size() == 3);

    std::vector<RoadNetStreet> bad;
    CHECK_FALSE(importRoadNet("id,kind,width_m,pts,pavement_m,scramble\nST-H,street,14,0:0;200:0,2.8,100\n", bad));
}

TEST_CASE("road net: a canal between two roads gets railing spans, closing posts, masts and a footbridge") {
    const char* const csv = "id,kind,width_m,pts,pavement_m\n"
                            "ST-W,street,14,0:0;0:200,2.8\n"
                            "ST-E,street,14,30:0;30:200,2.8\n";
    const std::vector<RoadNetStreet> s = net(csv);
    RoadBuild b;
    roadNetBuild(s, MassingGround{}, b);
    RoadCanal canal;
    canal.id = "CN-T";
    canal.x0 = 7.0;
    canal.x1 = 23.0;
    canal.y0 = 20.0;
    canal.y1 = 180.0;
    canal.bridgeAt = 100.0;
    std::vector<RoadQuayPoint> pts;
    RoadQuayCounts c;
    roadQuayPoints(canal, b, MassingGround{}, pts, &c);
    CHECK(c.roadInWater == 0);
    CHECK(c.bridgeAt > 89.0);
    CHECK(c.bridgeAt < 111.0);
    double pave = 0.0;
    for (const RoadSolid& sd : b.solids)
        if (sd.box.kind == "pavement") pave = std::max(pave, sd.box.z1);
    int spans = 0, posts = 0, masts = 0, deck = 0, ramps = 0, landings = 0;
    std::vector<double> mastW, mastE;
    for (const RoadQuayPoint& p : pts) {
        CHECK(p.x >= 7.0 - 1e-6);
        CHECK(p.x <= 23.0 + 1e-6);
        CHECK(p.y >= 20.0 - 1e-6);
        CHECK(p.y <= 180.0 + 1e-6);
        if (p.kind == "railing_span") {
            ++spans;
            CHECK(p.length <= kQuaySpanMax + 1e-9);
            CHECK(p.length > 0.25);
            CHECK(p.z1 - p.z0 == doctest::Approx(kQuayRailHigh));
            if (p.group.rfind("QR-", 0) == 0) {
                CHECK((std::fabs(p.x - 7.25) < 1e-6 || std::fabs(p.x - 22.75) < 1e-6));
                CHECK(p.z0 == doctest::Approx(pave).epsilon(0.01));
                // Off the bridge's gap.
                CHECK(std::fabs(p.y - c.bridgeAt) >= kQuayBridgeWide * 0.5 + p.length * 0.5 - 1e-6);
            }
        } else if (p.kind == "bridge_pier") {
            ++posts;
            CHECK(p.length == doctest::Approx(kQuayPost));
        } else if (p.kind == "lamp_mast") {
            ++masts;
            (p.x < 15.0 ? mastW : mastE).push_back(p.y);
            CHECK(p.z1 - p.z0 == doctest::Approx(kQuayMastHigh));
            for (const MassingFoot& f : b.furniture)
                CHECK(std::hypot(p.x - f.x, p.y - f.y) - std::max(f.hx, f.hy) >= kQuayMastClear - 1e-9);
        } else if (p.kind == "bridge_deck") {
            ++deck;
            CHECK(p.z1 == doctest::Approx(pave + kQuayBridgeRise).epsilon(0.01));
            CHECK(p.length == doctest::Approx(kQuayBridgeDeck));
        } else if (p.kind == "bridge_stair") {
            ++ramps;
            CHECK(p.slope > 0.0);
            CHECK(p.slope <= 0.6);
        } else if (p.kind == "bridge_landing") {
            ++landings;
        }
    }
    CHECK(deck == 1);
    CHECK(ramps == 2);
    CHECK(landings == 2);
    CHECK(spans == static_cast<int>(c.spans));
    CHECK(masts == static_cast<int>(c.masts));
    // A closing post per run: each bank's runs (one more than its breaks: the bridge and its masts) and the bridge's two.
    CHECK(posts == static_cast<int>(c.posts));
    CHECK(posts == static_cast<int>(mastW.size() + mastE.size()) + 2 + 2 + 2);
    CHECK(mastW.size() >= 5);
    CHECK(mastE.size() >= 5);
    for (const std::vector<double>* m : {&mastW, &mastE})
        for (std::size_t i = 1; i < m->size(); ++i) {
            const double gap = (*m)[i] - (*m)[i - 1];
            if (std::fabs(gap - 25.0) > 6.0 * 2.0 + 1e-9) CHECK(((*m)[i] > c.bridgeAt && (*m)[i - 1] < c.bridgeAt));
        }
    const std::string table = roadQuayCsv(pts);
    CHECK(table.find("kind,id,group,x,y,z0,z1,rot,length_m,width_m,slope,deck\n") != std::string::npos);
    std::size_t rows = 0;
    for (const char ch : table) rows += ch == '\n';
    CHECK(rows == pts.size() + 2);

    // A canal laid over a road's pavement is reported.
    RoadCanal bad = canal;
    bad.x0 = 3.0;
    roadQuayPoints(bad, b, MassingGround{}, pts, &c);
    CHECK(c.roadInWater > 0);
}

TEST_CASE("road net: a canal row builds its channel: walls, copings at the bank top, floor and water") {
    const char* const csv = "id,kind,width_m,pts,pavement_m,canal\n"
                            "ST-W,street,14,0:0;0:200,2.8,\n"
                            "ST-E,street,14,30:0;30:200,2.8,\n"
                            "CN-T,canal,,7:20;23:180,,1.5:2.5:100\n";
    std::vector<RoadNetStreet> s;
    std::vector<RoadCanal> canals;
    REQUIRE(importRoadNet(csv, s, nullptr, &canals));
    CHECK(s.size() == 2); // the canal row is no street
    REQUIRE(canals.size() == 1);
    const RoadCanal& c = canals[0];
    CHECK(c.id == "CN-T");
    CHECK(c.x0 == doctest::Approx(7.0));
    CHECK(c.y1 == doctest::Approx(180.0));
    CHECK(c.floorZ == doctest::Approx(1.5));
    CHECK(c.waterZ == doctest::Approx(2.5));
    CHECK(c.bridgeAt == doctest::Approx(100.0));
    // Without the canals list the row is skipped; a bad canal column or water under the floor fails.
    std::vector<RoadNetStreet> s2;
    REQUIRE(importRoadNet(csv, s2));
    CHECK(s2.size() == 2);
    std::vector<RoadCanal> cx;
    CHECK_FALSE(importRoadNet("id,kind,pts,canal\nCN-X,canal,0:0;10:50,3:2\n", s2, nullptr, &cx));
    CHECK_FALSE(importRoadNet("id,kind,pts,canal\nCN-X,canal,0:0;10:50,a:2\n", s2, nullptr, &cx));
    CHECK_FALSE(importRoadNet("id,kind,pts\nCN-X,canal,0:0\n", s2, nullptr, &cx));

    RoadBuild b;
    roadNetBuild(s, MassingGround{}, b);
    const std::size_t before = b.solids.size(), collide0 = b.counts.collide;
    const std::size_t added = roadCanalBuild(c, MassingGround{}, b);
    // 16 bank pieces a side (158 m in 10 m pieces) and two ends, each a wall and a coping; the floor and the water.
    CHECK(added == (2 * 16 + 2) * 2 + 2);
    CHECK(b.solids.size() == before + added);
    CHECK(b.counts.collide == collide0 + added - 1);
    double bankTop = 0.0;
    for (const RoadSolid& r : b.solids)
        if (r.box.kind == "pavement" && std::fabs(r.box.cx - (7.0 - 0.3)) < 2.0 && r.box.cy > 50.0 && r.box.cy < 150.0) {
            bankTop = r.box.z1;
            break;
        }
    for (std::size_t i = before; i < b.solids.size(); ++i) {
        const RoadSolid& r = b.solids[i];
        const MassingBox& x = r.box;
        CHECK(x.id.rfind("CN-T-", 0) == 0);
        CHECK(x.cx - 0.5 * x.sx >= 7.0 - 1e-9);
        CHECK(x.cx + 0.5 * x.sx <= 23.0 + 1e-9);
        CHECK(x.cy - 0.5 * x.sy >= 20.0 - 1e-9);
        CHECK(x.cy + 0.5 * x.sy <= 180.0 + 1e-9);
        if (x.kind == "canal_water") {
            CHECK(r.tint == RoadTint::Water);
            CHECK_FALSE(r.collide);
            CHECK(x.z1 == doctest::Approx(2.5));
            CHECK(x.sx == doctest::Approx(16.0 - 2.0 * c.wall));
        } else if (x.kind == "canal_floor") {
            CHECK(x.z1 == doctest::Approx(1.5));
            CHECK(r.collide);
        } else if (x.kind == "canal_coping") {
            CHECK(r.tint == RoadTint::Kerb);
            CHECK(x.z1 - x.z0 == doctest::Approx(c.coping));
            if (x.cx < 10.0 && x.sy < 11.0) CHECK(x.z1 == doctest::Approx(bankTop)); // the west bank: the pavement's top
        } else {
            CHECK(x.kind == "canal_wall");
            CHECK(x.z0 == doctest::Approx(1.2));
        }
    }
    // Too small a rectangle: nothing.
    RoadCanal tiny = c;
    tiny.x1 = tiny.x0 + 2.5;
    CHECK(roadCanalBuild(tiny, MassingGround{}, b) == 0);
}

TEST_CASE("road net: tints carry the surface master's street kind; the ramps' tactile strips wear Tactile") {
    CHECK(roadTintStreet(RoadTint::Asphalt) == 1);
    CHECK(roadTintStreet(RoadTint::AsphaltPatch) == 1);
    CHECK(roadTintStreet(RoadTint::Pavement) == 2);
    CHECK(roadTintStreet(RoadTint::Kerb) == 3);
    CHECK(roadTintStreet(RoadTint::LineWhite) == 4);
    CHECK(roadTintStreet(RoadTint::LineYellow) == 4);
    CHECK(roadTintStreet(RoadTint::SharedLane) == 5);
    CHECK(roadTintStreet(RoadTint::Tactile) == kRoadStreetTactile);
    CHECK(roadTintStreet(RoadTint::Water) == 0);
    CHECK(roadTintStreet(RoadTint::Steel) == 0);
    CHECK(std::string(roadTintName(RoadTint::Tactile)) == "Tactile");
    CHECK(std::string(roadTintName(RoadTint::SharedLane)) == "SharedLane");
    CHECK(std::string(roadTintName(RoadTint::Water)) == "Water");
    for (int t = 0; t < static_cast<int>(RoadTint::Count); ++t)
        CHECK(std::string(roadTintName(static_cast<RoadTint>(t))) != "Unknown");
    CHECK(roadTint(RoadTint::Water).roughness < 0.1f);

    const std::vector<RoadNetStreet> s = net(kTee);
    RoadBuild b;
    roadNetBuild(s, MassingGround{}, b);
    std::size_t tactile = 0;
    for (const RoadSolid& r : b.solids) {
        if (r.box.kind != "tactile") continue;
        ++tactile;
        CHECK(r.tint == RoadTint::Tactile);
    }
    CHECK(tactile > 0);
}
