// The slice's hard rejects (ftcore/BlockCheck.h).

#include "ftcore/BlockCheck.h"

#include <ostream>
#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace ftcore;

namespace {

MassingBox box(const char* id, double x0, double y0, double x1, double y1, double z0 = 0.0, double z1 = 9.0) {
    MassingBox b;
    b.id = id;
    b.kind = "kit";
    b.source = MassingSource::Invented;
    b.cx = (x0 + x1) * 0.5, b.cy = (y0 + y1) * 0.5, b.sx = x1 - x0, b.sy = y1 - y0;
    b.z0 = z0, b.z1 = z1;
    return b;
}

MassingStreet street(const char* id, const char* kind, double w, double pav, double x0, double y0, double x1, double y1) {
    MassingStreet s;
    s.id = id;
    s.kind = kind;
    s.width = w;
    s.pavement = pav;
    s.points = {MassingVec{x0, y0, 0}, MassingVec{x1, y1, 0}};
    return s;
}

// Two rows facing two 6 m lanes 30 m apart (block 26 m deep: 2 rows of 13), a 14 m road to the east.
struct Fixture {
    std::vector<MassingBox> boxes;
    BlockCheckInput in;
    Fixture() {
        for (int i = 0; i < 8; ++i) {
            const double x = 10.0 + 8.0 * i;
            boxes.push_back(box(("S" + std::to_string(i)).c_str(), x, 0, x + 8, 13));
            boxes.push_back(box(("N" + std::to_string(i)).c_str(), x, 13, x + 8, 26));
        }
        boxes.push_back(box("E0", 74, 0, 88, 26)); // the road's frontage, x 74..88 (wall at 88)
        in.boxes = &boxes;
        in.streets.push_back(street("LS", "core_lane", 6, 0, 0, -3, 88, -3));
        in.streets.push_back(street("LN", "core_lane", 6, 0, 0, 29, 88, 29));
        in.streets.push_back(street("RD", "road", 14, 3.5, 95, -40, 95, 60));
        in.rect = {0, -5, 92, 31};
    }
};

} // namespace

TEST_CASE("blockCheck passes a well-formed block") {
    Fixture f;
    BlockCheckReport r;
    CHECK(blockCheck(f.in, r));
    CHECK(r.buildings == f.boxes.size());
    CHECK(r.minDepth == doctest::Approx(26.0));
    CHECK(blockCheckText(r).find("BLOCKCHECK OK") != std::string::npos);
}

TEST_CASE("blockCheck rejects a lone building, a shallow block and deep sink") {
    Fixture f;
    f.boxes.push_back(box("LONE", 40, 60, 48, 70, 30.0, 40.0)); // far off on its own
    f.in.rect.y1 = 80;
    BlockCheckReport r;
    CHECK_FALSE(blockCheck(f.in, r));
    CHECK(r.lone == 1);

    Fixture g;
    g.in.streets[1] = street("LN", "core_lane", 6, 0, 0, 23, 88, 23); // 20 m between lanes
    BlockCheckReport r2;
    blockCheck(g.in, r2);
    CHECK(r2.depth > 0);

    Fixture h;
    BlockGround ground; // flat at 0
    h.in.ground = &ground;
    h.boxes[0].z0 = -5.0;
    BlockCheckReport r3;
    CHECK_FALSE(blockCheck(h.in, r3));
    CHECK(r3.sink == 1);
    CHECK(r3.maxSink == doctest::Approx(5.0));
}

TEST_CASE("blockCheck rejects a kerbed side without frontage and paving over the limit") {
    Fixture f;
    f.boxes.pop_back(); // the road's frontage gone
    BlockCheckReport r;
    CHECK_FALSE(blockCheck(f.in, r));
    CHECK(r.frontage > 0);

    Fixture h;
    for (int i = 0; i < 8; ++i) {
        const double x = 10.0 + 8.0 * i;
        h.boxes.push_back(box(("X" + std::to_string(i)).c_str(), x, 32.0 + 6.0, x + 8, 50)); // across LN, 9 m back
    }
    BlockCheckReport r3;
    CHECK_FALSE(blockCheck(h.in, r3));
    CHECK(r3.paving > 0);
    h.in.exempt.push_back({0, 26, 92, 38});
    BlockCheckReport r4;
    blockCheck(h.in, r4);
    CHECK(r4.paving == 0);
}

namespace {
// Mains at y 0 and y 100 (14 m, kerbed, 300 m straight), a short-cut lane between them, built strips along both mains.
struct Routes {
    std::vector<MassingBox> boxes;
    BlockCheckInput in;
    Routes() {
        in.routes = true;
        in.rect = {0, 0, 200, 100};
        in.streets = {street("M1", "road", 14, 3.0, -50, 0, 250, 0), street("M2", "road", 14, 3.0, -50, 100, 250, 100),
                      street("LN", "core_lane", 6, 1.5, 50, 7, 50, 93)};
        in.boxes = &boxes;
    }
    BlockCheckReport run() {
        BlockCheckReport r;
        blockCheck(in, r);
        return r;
    }
};
} // namespace

TEST_CASE("blockCheck route rules: a short-cut lane passes, chains and dead ends fail") {
    Routes f;
    BlockCheckReport r = f.run();
    CHECK(r.mains == 2);
    CHECK(r.lanes == 1);
    CHECK(r.mainShort + r.chain + r.deadEnd + r.walk + r.sight + r.section == 0);
    CHECK(r.maxWalk == doctest::Approx(49.0).epsilon(0.05)); // the lane's middle: 42 m to its mouth, 7 m to the centreline
    CHECK(blockCheckText(r).find("routes: mains 2") != std::string::npos);

    // a dead-end lane off M1, then the same with a park at its end
    f.in.streets.push_back(street("DE", "core_lane", 6, 0, 120, 7, 120, 40));
    r = f.run();
    CHECK(r.deadEnd == 1);
    f.in.reasons.push_back({110, 40, 130, 60});
    r = f.run();
    CHECK(r.deadEnd == 0);
    // a lane ending on another lane: a chain; a lane crossing the lane: a chain
    f.in.streets.push_back(street("CH", "core_lane", 6, 0, 53, 50, 100, 50));
    f.in.streets.push_back(street("X", "core_lane", 6, 0, 20, 70, 80, 70));
    r = f.run();
    CHECK(r.chain >= 2);
    CHECK_FALSE(r.ok());
}

TEST_CASE("blockCheck route rules: walk over 80 m, a bent main street, the landmark's sight line") {
    Routes f;
    // a long main stretch of lane far from any main: an alley 150 m north of M2, joined only at its west end by a lane
    f.in.rect = {0, 0, 200, 260};
    f.in.streets.push_back(street("L2", "core_lane", 6, 0, 50, 107, 50, 250));
    f.in.streets.push_back(street("AL", "alley", 2.5, 0, 50, 250, 190, 250));
    BlockCheckReport r = f.run();
    CHECK(r.walk >= 1);
    CHECK(r.maxWalk > 200.0);

    Routes g;
    MassingStreet bent = street("M1", "road", 14, 3.5, 0, 0, 100, 0);
    bent.points.push_back(MassingVec{150, 50, 0});
    g.in.streets[0] = bent;
    r = g.run();
    CHECK(r.mainShort == 1);

    // a landmark north of M2: seen over low roofs, hidden behind a tall wall of buildings
    Routes h;
    h.in.sights.push_back({"LM", 100, 300, 200});
    for (int i = 0; i < 10; ++i) h.boxes.push_back(box("LOW", -60 + 32.0 * i, 110, -30 + 32.0 * i, 130, 0, 9));
    r = h.run();
    CHECK(r.sight == 0);
    CHECK(r.sightRays >= 2);
    h.boxes.clear();
    for (int i = 0; i < 12; ++i) h.boxes.push_back(box("HI", -80 + 30.0 * i, 110, -50 + 30.0 * i, 130, 0, 400));
    for (int i = 0; i < 12; ++i) h.boxes.push_back(box("HS", -80 + 30.0 * i, -30, -50 + 30.0 * i, -10, 0, 400));
    r = h.run();
    CHECK(r.sight == 2);
    // the sight rule is skipped without targets; the route rules are off by default
    h.in.sights.clear();
    r = h.run();
    CHECK(r.sight == 0);
    h.in.routes = false;
    r = h.run();
    CHECK_FALSE(r.routes);
}

TEST_CASE("blockCheck route rules: street sections by class (no kerbless lanes)") {
    Routes f;
    f.in.streets.push_back(street("SH", "core_lane", 6, 0, 150, 7, 150, 93));  // a kerbless shared lane
    f.in.streets.push_back(street("WL", "core_lane", 8, 1.5, 100, 7, 100, 93)); // a lane too wide
    f.in.streets.push_back(street("AW", "alley", 4, 0, 20, 7, 20, 93));        // an alley over 3 m
    f.in.streets.push_back(street("AK", "alley", 3, 0.5, 180, 7, 180, 93));    // an alley with a kerb
    f.in.streets[0].pavement = 3.5;                                            // a road's pavement over 3 m
    BlockCheckReport r = f.run();
    CHECK(r.section == 5);
    std::size_t kerbless = 0;
    for (const BlockCheckIssue& i : r.issues) kerbless += i.rule == "section_kerbless" && i.id == "SH" ? 1 : 0;
    CHECK(kerbless == 1);
    CHECK_FALSE(r.ok());
    CHECK(blockCheckText(r).find("section 5") != std::string::npos);
}

TEST_CASE("blockCheck paving counts open paving beyond the footway edges") {
    Routes f;
    f.in.routes = false;
    f.in.rect = {40, 20, 60, 80};
    // facades 7 m and 4 m off LN's centre line: 11 m across a 6 m lane, 5 m open (at the limit)
    f.boxes.push_back(box("W", 33, 10, 43, 90));
    f.boxes.push_back(box("E", 54, 10, 64, 90));
    BlockCheckReport r = f.run();
    CHECK(r.paving == 0);
    CHECK(r.maxPaveLane == doctest::Approx(kBlockOpenLane).epsilon(0.02));
    f.boxes[1] = box("E", 55, 10, 65, 90); // 6 m open
    r = f.run();
    CHECK(r.paving > 0);
    // a 2 m yard wall on the east side's front closes it again; a 1.2 m one does not
    MassingBox w = box("YW", 54, 10, 54.3, 90);
    w.kind = "wall";
    w.z1 = w.z0 + 2.0;
    f.boxes.push_back(w);
    r = f.run();
    CHECK(r.paving == 0);
    f.boxes.back().z1 = f.boxes.back().z0 + 1.2;
    r = f.run();
    CHECK(r.paving > 0);
}
