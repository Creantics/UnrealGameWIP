// Drivable roads (ftcore/Roads.h): the road table, the build's clearances, bays and parking, doors, lane lamps,
// two-wheelers and cleared boxes. Made-up tables only.

#include "ftcore/RoadGraph.h"
#include "ftcore/Roads.h"

#include <ostream>
#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

const char* const kRoadCsv =
    "# a test road\n"
    "id,kind,width_m,pts,pavement_m,lane_m,speed_kmh,one_way,ground_z,mouth_back_m,mouth_z,mouths,bus_stops,parking,"
    "rail,poles,ends\n"
    "RD-01,road,14,885:410;885:650,3.5,3.5,40,0,4.0,1.5,4.12,"
    "CL-046@10.5@6@L;CL-047@60.5@6@L;CL-048@118.5@6@L@18;CL-049@160.5@6@L;CL-051@215.5@6@L,64@R,LR,R,R,closed\n";

RoadSpec theRoad() {
    std::vector<RoadSpec> roads;
    std::string err;
    REQUIRE_MESSAGE(importRoads(kRoadCsv, roads, &err), err);
    REQUIRE(roads.size() == 1);
    return roads[0];
}

} // namespace

TEST_CASE("roads: the table reads every column, mouths and bus stops") {
    const RoadSpec r = theRoad();
    CHECK(r.id == "RD-01");
    CHECK(r.width == doctest::Approx(14.0));
    CHECK(r.pavement == doctest::Approx(3.5));
    CHECK(r.y1 == doctest::Approx(650.0));
    CHECK(r.ground == doctest::Approx(4.0));
    CHECK(r.mouthZ == doctest::Approx(4.12));
    REQUIRE(r.mouths.size() == 5);
    CHECK(r.mouths[2].lane == "CL-048");
    CHECK(r.mouths[2].stub == doctest::Approx(18.0));
    CHECK(r.mouths[0].left);
    REQUIRE(r.busStops.size() == 1);
    CHECK(!r.busStops[0].left);
    CHECK(r.parkLeft);
    CHECK(r.parkRight);
    CHECK(!r.railLeft);
    CHECK(r.railRight);
    CHECK(r.closedEnds);
    CHECK(!r.oneWay);
}

TEST_CASE("roads: table faults") {
    std::vector<RoadSpec> roads;
    std::string err;
    const std::string head = "id,kind,width_m,pts,pavement_m,lane_m,ground_z,mouths\n";
    CHECK(!importRoads(head + "RD-1,road,10,0:0;0:100,3.5,3.5,0,\n", roads, &err)); // 3 m carriageway
    CHECK(err.find("line 2") != std::string::npos);
    CHECK(!importRoads(head + "RD-1,road,14,0:0;0:100,3.5,2.5,0,\n", roads, &err)); // lane under 3 m
    CHECK(!importRoads(head + "RD-1,road,14,0:0;0:5,3.5,3.5,0,\n", roads, &err));   // too short
    CHECK(!importRoads(head + "RD-1,road,14,0:0,3.5,3.5,0,\n", roads, &err));       // one point
    CHECK(!importRoads(head + "RD-1,road,14,0:0;0:100,3.5,3.5,0,L1@5@6@X\n", roads, &err));
    CHECK(!importRoads(head + "rd 1,road,14,0:0;0:100,3.5,3.5,0,\n", roads, &err));
    CHECK(importRoads(head + "RD-1,road,14,0:0;0:100,3.5,3.5,0,\n", roads, &err));
    CHECK(roads.size() == 1);
}

TEST_CASE("roads: the build keeps the carriageway clear and the kerbs mountable") {
    const RoadSpec r = theRoad();
    RoadBuild b;
    roadBuild(r, b);
    CHECK(b.counts.solids == b.solids.size());
    CHECK(b.counts.junctions == 1);
    CHECK(b.counts.zebras == 3);
    CHECK(b.counts.mouths == 5);
    CHECK(b.counts.lamps >= 18);
    CHECK(b.counts.signals >= 4 + 6);
    CHECK(b.counts.poles >= 6);
    CHECK(b.crossings.size() == 3);
    // Nothing of the furniture nearer the kerb face than kRoadKerbClear, save the end barriers, which close the road.
    std::size_t near = 0;
    for (const MassingFoot& f : b.furniture) {
        const bool barrier = f.hx < 0.2 && f.hy > 3.0;
        if (!barrier && roadInCarriageway(b, f, kRoadKerbClear - 1e-6)) {
            ++near;
            MESSAGE(f.x << "," << f.y << " " << f.hx << "x" << f.hy << " r" << f.rot);
        }
    }
    CHECK(near == 0);
    // Lamps stand at least kRoadKerbClear off the carriageway.
    for (const MassingProp& l : b.lamps) {
        MassingFoot f;
        f.x = l.x;
        f.y = l.y;
        f.hx = 0.1;
        f.hy = 0.1;
        CHECK(!roadInCarriageway(b, f, kRoadKerbClear - 0.1 - 1e-6));
    }
    // Kerbs: no kerb's top more than kRoadKerbRise over the gutter; the crown 2 % over 3.5 m above it.
    const double gutter = r.ground + kRoadSurfaceLift;
    for (const RoadSolid& s : b.solids) {
        if (s.box.kind == "kerb") CHECK(s.box.z1 <= gutter + kRoadKerbRise + 1e-9);
        if (s.box.kind == "crossover" || s.box.kind == "ramp") {
            // A slope no steeper than 1 in 10 across, from the gutter.
            CHECK(std::fabs(s.box.riseW) <= 0.1 + 1e-9);
        }
    }
}

TEST_CASE("roads: bays sit off the carriageway, clear of the junction, zebras, bus stop and ends") {
    const RoadSpec r = theRoad();
    RoadBuild b;
    roadBuild(r, b);
    REQUIRE(b.bays.size() >= 8);
    for (const RoadBay& bay : b.bays) {
        CHECK(!roadInCarriageway(b, bay.foot, -1e-6));
        // Its centre lies on the pavement side of the kerb line.
        CHECK(std::fabs(bay.foot.x - 885.0) == doctest::Approx(3.5 + kRoadBayDeep * 0.5));
        CHECK(bay.at >= kRoadBayKeepEnd - 1e-9);
        CHECK(bay.at <= 240.0 - kRoadBayKeepEnd + 1e-9);
        for (const MassingFoot& k : b.keepOut) {
            INFO(bay.at << " " << bay.left << " keep " << k.x << "," << k.y << " " << k.hx << "x" << k.hy);
            CHECK(!massingFeetOverlap(bay.foot, k, -1e-6));
        }
        // Not within kRoadBayKeepBus of the bus pole on its side.
        if (!bay.left) CHECK(std::fabs(bay.at - 64.0) >= kRoadBayKeepBus + kRoadBayLong * 0.5 - 1e-6);
    }
}

TEST_CASE("roads: a vehicle fits a bay only wholly inside it") {
    RoadBay bay;
    bay.foot.x = 10.0;
    bay.foot.y = 0.0;
    bay.foot.hx = 3.0;
    bay.foot.hy = 1.0;
    MassingFoot kei;
    kei.x = 10.0;
    kei.hx = 1.7;
    kei.hy = 0.74;
    CHECK(roadFitsBay(bay, kei));
    MassingFoot bus = kei;
    bus.hx = 5.25;
    bus.hy = 1.25;
    CHECK(!roadFitsBay(bay, bus));
    kei.y = 0.4; // over its edge
    CHECK(!roadFitsBay(bay, kei));
    kei.y = 0.0;
    kei.rot = 90.0; // across it
    CHECK(!roadFitsBay(bay, kei));
    // Every car kind fits a bay.
    bay.foot.hx = kRoadBayLong * 0.5;
    bay.foot.hy = kRoadBayDeep * 0.5;
    for (RoadVehicleKind k : {RoadVehicleKind::Kei, RoadVehicleKind::Sedan, RoadVehicleKind::Taxi, RoadVehicleKind::Van,
                              RoadVehicleKind::KeiTruck}) {
        RoadVehicle v;
        v.kind = k;
        v.x = bay.foot.x;
        CHECK_MESSAGE(roadFitsBay(bay, roadVehicleFoot(v)), roadVehicleName(k));
    }
}

TEST_CASE("roads: parked cars stay in their bays, off doors and the carriageway") {
    const RoadSpec r = theRoad();
    RoadBuild b;
    roadBuild(r, b);
    // A door at the back of the east pavement facing a bay.
    REQUIRE(!b.bays.empty());
    const RoadBay& first = b.bays.front();
    std::vector<MassingFoot> doors;
    MassingFoot d = first.foot; // a door foot right on the first bay
    d.hx = 1.0;
    d.hy = 1.0;
    doors.push_back(d);
    std::vector<RoadVehicle> cars;
    const RoadParkCounts c = roadParkVehicles(b, doors, nullptr, 7U, cars);
    CHECK(c.bays == b.bays.size());
    CHECK(c.parked + c.empty + c.blocked == c.bays);
    CHECK(c.parked == cars.size());
    CHECK(c.parked > c.bays / 2);
    for (const RoadVehicle& v : cars) {
        const MassingFoot f = roadVehicleFoot(v);
        CHECK(!roadInCarriageway(b, f, 0.0));
        CHECK(!massingFeetOverlap(f, d, 0.0));
        bool inBay = false;
        for (const RoadBay& bay : b.bays)
            if (roadFitsBay(bay, f)) inBay = true;
        CHECK(inBay);
        CHECK(v.where.find("-bay-") != std::string::npos);
    }
    // Same seed, same cars.
    std::vector<RoadVehicle> again;
    roadParkVehicles(b, doors, nullptr, 7U, again);
    REQUIRE(again.size() == cars.size());
    for (std::size_t i = 0; i < cars.size(); ++i) CHECK(again[i].kind == cars[i].kind);
    // Solids: tagged by kind, wheels not colliding.
    std::vector<RoadSolid> parts;
    roadVehicleSolids(cars.front(), parts);
    REQUIRE(parts.size() >= 7);
    for (const RoadSolid& s : parts) {
        CHECK(s.tag == std::string("FTVehicle_") + roadVehicleName(cars.front().kind));
        if (s.shape == RoadShape::Cylinder) CHECK(!s.collide);
    }
}

TEST_CASE("roads: doors table reads door rows, skips rooms") {
    const std::string csv = "id,kind,width_m,pts,pavement_m\n"
                            "DOOR-A,door,1.6,568.49:171.20:15.62;566.49:171.20:15.62,\n"
                            "ROOM-A,room,0,1:1:0;2:2:0;3:1:0,\n";
    std::vector<MassingFoot> doors;
    std::string err;
    REQUIRE_MESSAGE(importRoadDoors(csv, 0.5, doors, &err), err);
    REQUIRE(doors.size() == 1);
    CHECK(doors[0].x == doctest::Approx(567.49));
    CHECK(doors[0].hx == doctest::Approx(1.5));
    CHECK(doors[0].hy == doctest::Approx(1.3));
    CHECK(!importRoadDoors("id,kind,width_m,pts\nD,door,0,1:1:1;2:2:2\n", 0.0, doors, &err));
}

TEST_CASE("roads: lane lamps overlap along a lane, keep off existing lamps and doors") {
    std::vector<MassingStreet> lanes(2);
    lanes[0].id = "CL-1";
    lanes[0].width = 6.0;
    lanes[0].points = {{0.0, 0.0, 0.0}, {100.0, 0.0, 0.0}};
    lanes[1].id = "ST-1";
    lanes[1].width = 14.0; // too wide
    lanes[1].points = {{0.0, 50.0, 0.0}, {100.0, 50.0, 0.0}};
    RoadLaneLampOptions o;
    o.x0 = -10;
    o.y0 = -10;
    o.x1 = 200;
    o.y1 = 200;
    o.edgeIn = 0.9;
    const MassingGround ground;
    std::vector<MassingProp> lamps;
    const std::vector<RoadPoint> existing = {{50.0, 2.0}};
    MassingFoot door;
    door.x = 30.0;
    door.y = 3.0;
    door.hx = 1.0;
    door.hy = 1.0;
    const std::size_t n = roadLaneLamps(lanes, o, ground, existing, {door}, nullptr, lamps);
    CHECK(n == lamps.size());
    CHECK(n >= 10);
    for (const MassingProp& l : lamps) {
        CHECK(std::fabs(l.y) == doctest::Approx(3.0 - 0.9));
        CHECK(std::hypot(l.x - 50.0, l.y - 2.0) >= o.keep);
        CHECK(std::fabs(l.y) < 10.0);
    }
    for (std::size_t i = 0; i < lamps.size(); ++i)
        for (std::size_t j = i + 1; j < lamps.size(); ++j)
            CHECK(std::hypot(lamps[i].x - lamps[j].x, lamps[i].y - lamps[j].y) >= o.keep - 1e-9);
}

TEST_CASE("roads: two-wheelers stay off the carriageway, doors and each other") {
    const RoadSpec r = theRoad();
    RoadBuild b;
    roadBuild(r, b);
    std::vector<MassingStreet> lanes(1);
    lanes[0].id = "CL-048";
    lanes[0].width = 6.0;
    lanes[0].points = {{820.0, 528.5, 0.0}, {876.5, 528.5, 0.0}};
    RoadTwoWheelOptions o;
    o.x0 = 800;
    o.y0 = 400;
    o.x1 = 950;
    o.y1 = 660;
    o.seed = 3;
    const MassingGround ground;
    std::vector<RoadVehicle> bikes;
    const std::size_t n = roadTwoWheelers(lanes, o, &b, &r, ground, {}, nullptr, bikes);
    CHECK(n == bikes.size());
    CHECK(n >= 4);
    for (std::size_t i = 0; i < bikes.size(); ++i) {
        const MassingFoot f = roadVehicleFoot(bikes[i]);
        CHECK(!roadInCarriageway(b, f, kRoadKerbClear - 1e-6));
        CHECK((bikes[i].kind == RoadVehicleKind::Scooter || bikes[i].kind == RoadVehicleKind::Bicycle));
        for (std::size_t j = i + 1; j < bikes.size(); ++j) CHECK(!massingFeetOverlap(f, roadVehicleFoot(bikes[j]), 0.1));
    }
}

TEST_CASE("roads: cleared boxes are the decks and piers over the band") {
    const RoadSpec r = theRoad();
    RoadBuild b;
    roadBuild(r, b);
    std::vector<MassingBox> boxes(4);
    boxes[0].id = "D1";
    boxes[0].kind = "deck";
    boxes[0].cx = 885.0;
    boxes[0].cy = 500.0;
    boxes[0].sx = 10.0;
    boxes[0].sy = 30.0;
    boxes[0].z0 = 12.5;
    boxes[0].z1 = 13.0;
    boxes[1] = boxes[0];
    boxes[1].id = "D2";
    boxes[1].cy = 395.0; // touches at y 410 only
    boxes[1].sy = 30.0 - 1e-3;
    boxes[2] = boxes[0];
    boxes[2].id = "B1";
    boxes[2].kind = "lowrise"; // not a deck
    boxes[3] = boxes[0];
    boxes[3].id = "P1";
    boxes[3].kind = "pier";
    boxes[3].sx = 3.0;
    boxes[3].sy = 3.0;
    boxes[3].z0 = 3.5;
    const std::vector<std::size_t> gone = roadClearedBoxes(b, boxes, 0.0);
    REQUIRE(gone.size() == 2);
    CHECK(gone[0] == 0);
    CHECK(gone[1] == 3);
}

TEST_CASE("roads: box rows in the carriageway clash unless hidden or a footbridge 4.8 m clear") {
    const RoadSpec r = theRoad();
    RoadBuild b;
    roadBuild(r, b);
    std::vector<MassingBox> boxes(5);
    boxes[0].id = "SLAB-LOW"; // a footbridge slab 3 m over the road
    boxes[0].kind = "lowrise";
    boxes[0].cx = 885.0;
    boxes[0].cy = 450.0;
    boxes[0].sx = 30.0;
    boxes[0].sy = 3.0;
    boxes[0].z0 = 7.0;
    boxes[0].z1 = 7.4;
    boxes[1] = boxes[0];
    boxes[1].id = "SLAB-HIGH"; // its underside 5 m over the road
    boxes[1].z0 = 9.2;
    boxes[1].z1 = 9.6;
    boxes[2] = boxes[0];
    boxes[2].id = "SLAB-HIDDEN";
    boxes[2].kind = "underpass";
    boxes[3] = boxes[0];
    boxes[3].id = "PAD"; // a pad on the road
    boxes[3].z0 = 3.0;
    boxes[3].z1 = 4.4;
    boxes[4] = boxes[3];
    boxes[4].id = "LOT"; // the same pad in the lot behind
    boxes[4].cx = 930.0;
    const std::vector<std::size_t> clash = roadBoxClashes(b, boxes);
    REQUIRE(clash.size() == 2);
    CHECK(clash[0] == 0);
    CHECK(clash[1] == 3);
}

TEST_CASE("roads: the junction and the lane mouths leave no kerb or pavement face, no step") {
    const RoadSpec r = theRoad();
    RoadBuild b;
    roadBuild(r, b);
    RoadGraph g;
    roadGraphFromRoad(r, b, g);
    REQUIRE_FALSE(g.lanes.empty());
    // The lanes (6 m) meeting the road: centre lines out to the road's kerb line, through the dropped kerbs.
    for (const RoadMouth& m : r.mouths) {
        RoadGraphLane lane;
        lane.id = m.lane;
        lane.width = m.width;
        lane.pts = {{885.0 - 30.0, 410.0 + m.at}, {885.0 - 3.6, 410.0 + m.at}};
        g.lanes.push_back(lane);
    }
    for (const RoadFace& f : roadKerbFaces(b, &g)) {
        INFO(f.piece << " at " << f.x << ", " << f.y << ": " << f.fault);
        CHECK(false);
    }
    // Nothing parked in the junction box or on its crossings.
    std::vector<RoadVehicle> cars;
    roadParkVehicles(b, {}, nullptr, 7U, cars);
    for (const RoadVehicle& v : cars) {
        MassingFoot f = roadVehicleFoot(v);
        f.hx -= 0.01;
        f.hy -= 0.01;
        for (const MassingFoot& k : b.keepOut) CHECK_FALSE(massingFeetOverlap(f, k, 0.0));
        for (const RoadJunction& j : b.junctions) CHECK_FALSE(massingFeetOverlap(f, j.box, 0.0));
    }
}
