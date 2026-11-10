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
