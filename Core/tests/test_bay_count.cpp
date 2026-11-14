// Shop bays per street (ftcore/BayCount.h). Made-up tables only: the real ones are kept outside the repo.

#include "ftcore/BayCount.h"
#include "ftcore/WindowFacade.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

MassingBox building(const std::string& id, double cx, double cy, double sx, double sy, double z1) {
    MassingBox b;
    b.id = id;
    b.kind = "bld";
    b.cover = true;
    b.cx = cx;
    b.cy = cy;
    b.sx = sx;
    b.sy = sy;
    b.z0 = 0.0;
    b.z1 = z1;
    return b;
}

MassingStreet street(const std::string& id, double width, double pavement, double y) {
    MassingStreet s;
    s.id = id;
    s.kind = "street";
    s.width = width;
    s.pavement = pavement;
    s.points = {{-60.0, y, 0.0}, {60.0, y, 0.0}};
    return s;
}

std::vector<MassingFrontRule> oneRule() {
    MassingFrontRule r;
    r.id = "R-1";
    r.asset = "/Game/R.R";
    r.width = 3.0;
    r.depth = 1.0;
    r.height = 3.0;
    r.out = 1.0;
    return {r};
}

// What the facade cuts on a face `length` wide and `height` high over its street floor, from the box alone.
std::size_t expectedBays(const MassingBox& box, double length, double height) {
    std::uint8_t front[4] = {1, 1, 1, 1};  // a street face, as the type picks by it
    const MassingWindowData data = massingWindowData(box, MassingGround{}, front);
    const WindowType type = massingFacadeData(box, data).type;
    const WindowFacadeSpec spec;
    const WindowFaceGrid g = windowFaceGrid(windowGridFor(spec, type), static_cast<float>(length), static_cast<float>(height),
                                            windowWidthFor(spec, type, data.seed, spec.base.windowWidth));
    return g.storeys > 0u ? g.bays : 0u;
}

struct Fixture {
    // S-A: a 10 m street with 2 m footways along y = 0 (walked); S-B: an 8 m street without, along y = 60.
    std::vector<MassingStreet> streets{street("S-A", 10.0, 2.0, 0.0), street("S-B", 8.0, 0.0, 60.0)};
    // H-1 (20 x 8 m, 8 m high) faces S-A with its -w face; H-2 (30 x 8 m, 20 m high) faces S-B with its +w face; H-3 stands
    // far from both; H-4 would face S-A but a kit mesh stands in for it (replaced).
    std::vector<MassingBox> boxes{building("H-1", 0.0, 10.0, 20.0, 8.0, 8.0), building("H-2", 0.0, 50.0, 30.0, 8.0, 20.0),
                                  building("H-3", 0.0, 30.0, 10.0, 6.0, 8.0), building("H-4", 40.0, 10.0, 12.0, 8.0, 8.0)};
    std::vector<bool> replaced{false, false, false, true};
    BayCountResult run(const BayCountConfig& config = {}) const {
        BayCountResult r;
        std::string error;
        REQUIRE(bayCount(boxes, replaced, streets, streets, oneRule(), MassingGround{}, config, r, &error));
        return r;
    }
};

const BayStreet* row(const BayCountResult& r, const std::string& id) {
    for (const BayStreet& s : r.streets)
        if (s.id == id) return &s;
    return nullptr;
}

} // namespace

TEST_CASE("bay count: each street gets the bays of the faces that front it") {
    const Fixture f;
    const BayCountResult r = f.run();
    const BayStreet* a = row(r, "S-A");
    const BayStreet* b = row(r, "S-B");
    REQUIRE(a);
    REQUIRE(b);
    const std::size_t bayH1 = expectedBays(f.boxes[0], 20.0, 8.0);
    const std::size_t bayH2 = expectedBays(f.boxes[1], 30.0, 20.0);
    REQUIRE(bayH1 > 0);
    REQUIRE(bayH2 > 0);
    CHECK(a->bays == bayH1);
    CHECK(a->faces == 1);
    CHECK(b->bays == bayH2);
    CHECK(b->faces == 1);
    CHECK(std::fabs(a->length - 120.0) < 1e-9);
    CHECK(a->kind == "street");
    CHECK(a->pavement == doctest::Approx(2.0));
    // H-3 is a host with no street, H-4 is hidden under a kit mesh.
    CHECK(r.boxes == 4);
    CHECK(r.hosts == 3);
    CHECK(r.windowed == 3);
    CHECK(r.streetFaces == 2);
    CHECK(r.unattributed == 0);
    CHECK(a->litBays <= a->bays);
    // One spill light per 8 m of H-1's 20 m face.
    CHECK(a->lights == 3);
}

TEST_CASE("bay count: the walked total is the streets with a footway") {
    const Fixture f;
    const BayCountResult r = f.run();
    CHECK(r.all.streets == 2);
    CHECK(r.walked.streets == 1);
    CHECK(r.all.bays == row(r, "S-A")->bays + row(r, "S-B")->bays);
    CHECK(r.walked.bays == row(r, "S-A")->bays);
    CHECK(r.all.length == doctest::Approx(240.0));
    CHECK(r.walked.length == doctest::Approx(120.0));
    CHECK(r.walked.withBays == 1);
}

TEST_CASE("bay count: the sweep counts every bay within reach of a walked centreline") {
    const Fixture f;
    BayCountConfig wide;
    wide.reach = 500.0;
    const BayCountResult all = f.run(wide);
    REQUIRE(all.worst.found);
    CHECK(all.worst.street == "S-A");
    CHECK(all.worst.bays == all.all.bays); // both streets' bays, though only S-A is walked

    BayCountConfig narrow;
    narrow.reach = 15.0;
    const BayCountResult close = f.run(narrow);
    REQUIRE(close.worst.found);
    CHECK(close.worst.street == "S-A");
    CHECK(close.worst.bays > 0);
    CHECK(close.worst.bays <= row(close, "S-A")->bays); // S-B's bays are 50 m off
    CHECK(std::fabs(close.worst.x) <= 20.0);
}

TEST_CASE("bay count: a street without a footway alone leaves nothing to sweep") {
    Fixture f;
    f.streets[0].pavement = 0.0;
    const BayCountResult r = f.run();
    CHECK(r.walked.streets == 0);
    CHECK(r.walked.bays == 0);
    CHECK_FALSE(r.worst.found);
    CHECK(r.all.bays > 0);
}

TEST_CASE("bay count: a front street missing from the table is appended; bad input is refused") {
    Fixture f;
    std::vector<MassingStreet> table{f.streets[0]};
    BayCountResult r;
    std::string error;
    REQUIRE(bayCount(f.boxes, f.replaced, table, f.streets, oneRule(), MassingGround{}, BayCountConfig{}, r, &error));
    REQUIRE(r.streets.size() == 2);
    CHECK(r.streets[1].id == "S-B");
    CHECK(r.streets[1].bays > 0);

    CHECK_FALSE(bayCount(f.boxes, f.replaced, f.streets, f.streets, {}, MassingGround{}, BayCountConfig{}, r, &error));
    CHECK_FALSE(error.empty());
    BayCountConfig noStep;
    noStep.step = 0.0;
    CHECK_FALSE(bayCount(f.boxes, f.replaced, f.streets, f.streets, oneRule(), MassingGround{}, noStep, r, &error));
}
