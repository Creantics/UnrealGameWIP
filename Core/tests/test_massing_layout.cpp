// Lanes laid out as streets (ftcore/MassingLayout.h): build-to lines, infill, flush lane dressing, the road mask's lanes
// and the paving pads dropped from them. Made-up lanes and boxes only.

#include "ftcore/MassingLayout.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

MassingStreet lane(const std::string& id, double w, double x0, double y0, double x1, double y1) {
    MassingStreet s;
    s.id = id;
    s.width = w;
    s.points = {{x0, y0, 0.0}, {x1, y1, 0.0}};
    s.kind = "core_lane";
    return s;
}

MassingBox house(const std::string& id, double x0, double y0, double x1, double y1, double z1 = 8.0) {
    MassingBox b;
    b.id = id;
    b.row = id;
    b.kind = "kit";
    b.cover = false;
    b.cx = 0.5 * (x0 + x1);
    b.cy = 0.5 * (y0 + y1);
    b.sx = x1 - x0;
    b.sy = y1 - y0;
    b.z0 = -0.1;
    b.z1 = z1;
    return b;
}

struct Bounds {
    double x0, y0, x1, y1;
};
Bounds boundsOf(const MassingBox& b) {
    const bool q = std::fabs(std::fmod(std::fabs(b.rot), 180.0) - 90.0) < 1e-6;
    const double hx = 0.5 * (q ? b.sy : b.sx), hy = 0.5 * (q ? b.sx : b.sy);
    return {b.cx - hx, b.cy - hy, b.cx + hx, b.cy + hy};
}

bool overlap(const Bounds& a, const Bounds& b) {
    return a.x0 < b.x1 - 0.01 && b.x0 < a.x1 - 0.01 && a.y0 < b.y1 - 0.01 && b.y0 < a.y1 - 0.01;
}

bool validId(const std::string& id) {
    if (id.empty()) return false;
    for (const char c : id)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

// A ground rising 0.08 m a metre to the east from 4 m at x = 0, flat along y, 1 m samples from (-10, -10).
struct Slope {
    std::vector<std::uint16_t> grid;
    MassingGround ground;
    Slope() {
        const std::size_t n = 200;
        grid.resize(n * n);
        for (std::size_t iy = 0; iy < n; ++iy)
            for (std::size_t ix = 0; ix < n; ++ix) {
                const double x = -10.0 + static_cast<double>(ix);
                grid[iy * n + ix] = static_cast<std::uint16_t>(std::lround(32768.0 + (4.0 + 0.08 * x) * 128.0));
            }
        ground = {grid.data(), n, n, 1.0, -10.0, -10.0};
    }
};

} // namespace

TEST_CASE("massingBuildTo pulls set-back rows to the lane's lines and keeps the lane clear") {
    // An east-west lane at y 50, 6 m wide; a row of houses each side, set back 6 m and 4 m from its edges.
    std::vector<MassingStreet> streets{lane("CL-1", 6.0, 0.0, 50.0, 60.0, 50.0)};
    std::vector<MassingBox> boxes;
    for (int i = 0; i < 6; ++i) {
        const double x0 = 2.0 + 9.0 * i;
        boxes.push_back(house("N" + std::to_string(i), x0, 59.0, x0 + 7.0, 70.0));
        boxes.push_back(house("S" + std::to_string(i), x0, 33.0, x0 + 7.0, 43.0));
    }
    MassingBuildTo opts;
    opts.area = {0.0, 0.0, 60.0, 100.0};
    const MassingGround flat{};
    const MassingBuildToCounts c = massingBuildTo(boxes, streets, flat, opts);
    CHECK(c.lanes == 1);
    CHECK(c.pulled == 12);
    for (int i = 0; i < 12; ++i) {
        const Bounds b = boundsOf(boxes[static_cast<std::size_t>(i)]);
        if (boxes[static_cast<std::size_t>(i)].id[0] == 'N') {
            CHECK(b.y0 == doctest::Approx(53.0));
            CHECK(b.y1 == doctest::Approx(70.0)); // the back stays
        } else {
            CHECK(b.y1 == doctest::Approx(47.0));
            CHECK(b.y0 == doctest::Approx(33.0));
        }
        CHECK(boxes[static_cast<std::size_t>(i)].z0 <= -0.1);
    }
    // Wall to wall along the lane: 6 m wherever both sides stand; nothing in the lane.
    const MassingLaneWidth w = massingLaneWidth(streets[0], boxes, opts.area, 12.0, 1.0);
    CHECK(w.bothSides > 0);
    CHECK(w.minWall == doctest::Approx(6.0));
    CHECK(w.maxWall == doctest::Approx(6.0));
    for (const MassingBox& b : boxes) CHECK_FALSE(overlap(boundsOf(b), {0.0, 47.0, 60.0, 53.0}));
}

TEST_CASE("massingBuildTo fills the gaps along a frontage, out of keep-clear rects and crossing streets") {
    // A lane at y 50 from x 0 to 100; one house north at x 10..17 on the line; a crossing lane at x 40 (6 m) and a
    // keep-clear corridor at x 80..94.
    std::vector<MassingStreet> streets{lane("CL-2", 6.0, 0.0, 50.0, 100.0, 50.0), lane("CL-3", 6.0, 40.0, 50.0, 40.0, 90.0)};
    std::vector<MassingBox> boxes{house("H1", 10.0, 53.0, 17.0, 63.0)};
    MassingBuildTo opts;
    opts.area = {0.0, 0.0, 100.0, 100.0};
    opts.keepClear.push_back({80.0, 0.0, 94.0, 100.0});
    const MassingGround flat{};
    const MassingBuildToCounts c = massingBuildTo(boxes, streets, flat, opts);
    CHECK(c.infill > 0);
    CHECK(boxes.size() == 1 + c.infill);
    const Bounds corridor{80.0, 0.0, 94.0, 100.0};
    for (std::size_t i = 1; i < boxes.size(); ++i) {
        const MassingBox& b = boxes[i];
        const Bounds r = boundsOf(b);
        CHECK(validId(b.id));
        CHECK(b.id.rfind("CL-", 0) == 0);
        CHECK(b.kind == "kit");
        CHECK(b.source == MassingSource::Invented);
        CHECK(massingVisible(massingRole(b)));
        CHECK(massingLookIsBuilding(massingLookSurface(b)));
        // A lot is at most lotMax along its lane and depthMin..depthMax deep (the crossing lane's lots run along y).
        const bool alongX = std::fabs(r.y0 - 53.0) < 1e-9 || std::fabs(r.y1 - 47.0) < 1e-9;
        const double along = alongX ? r.x1 - r.x0 : r.y1 - r.y0, deep = alongX ? r.y1 - r.y0 : r.x1 - r.x0;
        CHECK(along <= opts.lotMax + 1e-9);
        CHECK(deep >= opts.depthMin - 1e-9);
        CHECK(deep <= opts.depthMax + 1e-9);
        CHECK(b.z0 <= -opts.sink + 1e-9);
        CHECK(b.z1 >= 2.0 * opts.storey);
        CHECK_FALSE(overlap(r, corridor));
        CHECK_FALSE(overlap(r, {37.0, 47.0, 43.0, 90.0}));  // the crossing lane
        CHECK_FALSE(overlap(r, {0.0, 47.0, 100.0, 53.0})); // the lane itself
        for (std::size_t j = 0; j < i; ++j) CHECK_FALSE(overlap(r, boundsOf(boxes[j])));
        // Each stands on a line.
        CHECK((std::fabs(r.y0 - 53.0) < 1e-9 || std::fabs(r.y1 - 47.0) < 1e-9 || std::fabs(r.x0 - 43.0) < 1e-9 ||
               std::fabs(r.x1 - 37.0) < 1e-9));
    }
    // The house's side of the lane now reads 6 m wall to wall outside the crossing and the corridor.
    const MassingLaneWidth w = massingLaneWidth(streets[0], boxes, opts.area, 12.0, 1.0);
    CHECK(w.minWall == doctest::Approx(6.0));
    CHECK(w.maxWall == doctest::Approx(6.0));
}

TEST_CASE("massingBuildTo leaves a face with something between it and the line, and cuts a building out of a lane") {
    std::vector<MassingStreet> streets{lane("CL-4", 6.0, 0.0, 50.0, 40.0, 50.0)};
    std::vector<MassingBox> boxes{house("BACK", 5.0, 60.0, 12.0, 70.0), house("WALLED", 20.0, 60.0, 27.0, 70.0),
                                  house("INTO", 30.0, 49.0, 37.0, 60.0)};
    MassingBox kiosk = house("KIOSK", 21.0, 55.0, 26.0, 57.0, 3.0);
    kiosk.kind = "barrier";
    boxes.push_back(kiosk);
    MassingBuildTo opts;
    opts.area = {0.0, 0.0, 40.0, 100.0};
    opts.minGap = 1000.0; // no infill here
    const MassingGround flat{};
    const MassingBuildToCounts c = massingBuildTo(boxes, streets, flat, opts);
    CHECK(c.trimmed == 1);
    CHECK(boundsOf(boxes[2]).y0 == doctest::Approx(53.0));
    CHECK(boundsOf(boxes[0]).y0 == doctest::Approx(53.0)); // pulled
    CHECK(boundsOf(boxes[1]).y0 == doctest::Approx(60.0)); // the kiosk is in the way
    CHECK(c.blocked >= 1);
    CHECK(c.infill == 0);
}

TEST_CASE("massingBuildTo lowers a pulled building's bottom to the ground under its new footprint") {
    Slope s;
    // A north-south lane at x 50: the ground falls to the west, so pulling a west house east lifts nothing but
    // pulling an east house west reaches lower ground.
    std::vector<MassingStreet> streets{lane("CL-5", 6.0, 50.0, 0.0, 50.0, 60.0)};
    std::vector<MassingBox> boxes{house("E", 60.0, 10.0, 70.0, 17.0)};
    boxes[0].z0 = 4.0 + 0.08 * 60.0 - 0.1;
    MassingBuildTo opts;
    opts.area = {0.0, 0.0, 100.0, 60.0};
    opts.minGap = 1000.0;
    massingBuildTo(boxes, streets, s.ground, opts);
    CHECK(boundsOf(boxes[0]).x0 == doctest::Approx(53.0));
    CHECK(boxes[0].z0 == doctest::Approx(4.0 + 0.08 * 53.0 - opts.sink).epsilon(1e-3));
}

TEST_CASE("massingLaneDress lays a gutter, drains, patches and mouth bars flush with a sloping ground") {
    Slope s;
    // An east-west lane at y 50 from x 0 to 120, meeting a 14 m road at its east end.
    std::vector<MassingStreet> streets{lane("CL-6", 6.0, 0.0, 50.0, 120.0, 50.0), lane("RD-9", 14.0, 128.0, 0.0, 128.0, 100.0)};
    MassingLaneDress opts;
    opts.area = {0.0, 0.0, 140.0, 100.0};
    std::vector<MassingBox> out;
    const MassingLaneDressCounts c = massingLaneDress(streets, s.ground, opts, out);
    CHECK(c.gutters > 0);
    CHECK(c.drains > 0);
    CHECK(c.patches > 0);
    CHECK(c.marks == 1);
    CHECK(c.uneven == 0);
    CHECK(out.size() == c.gutters + c.drains + c.patches + c.marks);
    for (const MassingBox& b : out) {
        CHECK(validId(b.id));
        const Bounds r = boundsOf(b);
        CHECK(r.y0 >= 47.0 - 1e-9); // in the lane
        CHECK(r.y1 <= 53.0 + 1e-9);
        CHECK(r.x0 >= 1.0 - 1e-9);
        CHECK(r.x1 <= 120.0 + 1e-9);
        CHECK(massingVisible(massingRole(b)));
        CHECK_FALSE(massingLookIsBuilding(massingLookSurface(b)));
        // Flush: its top kMassingLaneDressTop over the ground at both ends.
        const double g0 = 4.0 + 0.08 * r.x0, g1 = 4.0 + 0.08 * r.x1;
        REQUIRE(b.sloped);
        CHECK(std::fabs(b.slopeFrom - (g0 + kMassingLaneDressTop)) < 0.01);
        CHECK(std::fabs(b.slopeTo - (g1 + kMassingLaneDressTop)) < 0.01);
        CHECK(b.z0 <= g0 - kMassingLaneDressSink + 0.01); // the r16's 1/128 m steps
    }
    // No two pieces overlap (no coplanar fighting).
    for (std::size_t i = 0; i < out.size(); ++i)
        for (std::size_t j = 0; j < i; ++j) CHECK_FALSE(overlap(boundsOf(out[i]), boundsOf(out[j])));
}

TEST_CASE("massingLaneDress leaves out a piece over a step and keeps out of keep-clear rects") {
    // A ground with a 0.5 m step at x 30.
    const std::size_t n = 100;
    std::vector<std::uint16_t> grid(n * n);
    for (std::size_t iy = 0; iy < n; ++iy)
        for (std::size_t ix = 0; ix < n; ++ix) grid[iy * n + ix] = static_cast<std::uint16_t>(32768 + (ix >= 30 ? 64 : 0));
    const MassingGround g{grid.data(), n, n, 1.0, 0.0, 0.0};
    std::vector<MassingStreet> streets{lane("CL-7", 6.0, 0.0, 50.0, 90.0, 50.0)};
    MassingLaneDress opts;
    opts.area = {0.0, 0.0, 100.0, 100.0};
    opts.keepClear.push_back({60.0, 0.0, 70.0, 100.0});
    std::vector<MassingBox> out;
    const MassingLaneDressCounts c = massingLaneDress(streets, g, opts, out);
    CHECK(c.uneven > 0);
    for (const MassingBox& b : out) {
        const Bounds r = boundsOf(b);
        CHECK_FALSE((r.x0 < 29.0 && r.x1 > 29.0)); // nothing spans the step's foot or its top
        CHECK_FALSE((r.x0 < 30.0 && r.x1 > 30.0));
        CHECK_FALSE(overlap(r, {59.0, 0.0, 71.0, 100.0}));
    }
}

TEST_CASE("massingLanePave widens the lanes in the road mask and massingDropLanePads drops the pads on them") {
    std::vector<MassingStreet> streets{lane("CL-8", 6.0, 0.0, 50.0, 60.0, 50.0), lane("ST-1", 14.0, 0.0, 0.0, 60.0, 0.0)};
    streets[0].pavement = 1.5;
    streets[1].pavement = 3.0;
    CHECK(massingLanePave(streets, 6.0, 0.5) == 1);
    CHECK(streets[0].width == doctest::Approx(7.0));
    CHECK(streets[0].pavement == 0.0);
    CHECK(massingCarriageway(streets[0]) == doctest::Approx(7.0));
    CHECK(streets[1].width == doctest::Approx(14.0));
    CHECK(massingLanePave(streets, 0.0, 0.5) == 0);

    std::vector<MassingStreet> plan{lane("CL-8", 6.0, 0.0, 50.0, 60.0, 50.0), lane("ST-1", 14.0, 0.0, 0.0, 60.0, 0.0)};
    std::vector<MassingBox> pads{house("A-FP1", 10.0, 52.0, 12.0, 53.0), house("B-FP1", 10.0, 6.0, 12.0, 7.0),
                                house("C-FP1", 10.0, 80.0, 12.0, 81.0)};
    CHECK(massingDropLanePads(pads, plan, 6.0, 0.5) == 1);
    REQUIRE(pads.size() == 2);
    CHECK(pads[0].id == "B-FP1");
    CHECK(pads[1].id == "C-FP1");
}

TEST_CASE("massingBuildTo and massingLaneDress take only the lane kinds asked for; other narrow streets stay obstacles") {
    // A lane at y 50 and a 4 m stair at y 20, both from x 0 to 60; a house 5 m behind each.
    std::vector<MassingStreet> streets{lane("CL-7", 6.0, 0.0, 50.0, 60.0, 50.0), lane("SR-1", 4.0, 0.0, 20.0, 60.0, 20.0)};
    streets[1].kind = "stair";
    std::vector<MassingBox> boxes{house("A", 10.0, 58.0, 20.0, 68.0), house("B", 10.0, 27.0, 20.0, 37.0)};
    MassingBuildTo opts;
    opts.area = {0.0, 0.0, 60.0, 100.0};
    opts.kinds = {"core_lane", "lane"};
    const MassingGround flat{};
    const MassingBuildToCounts c = massingBuildTo(boxes, streets, flat, opts);
    CHECK(c.lanes == 1);
    CHECK(boundsOf(boxes[0]).y0 == doctest::Approx(53.0)); // pulled to the lane
    CHECK(boundsOf(boxes[1]).y0 == doctest::Approx(27.0)); // the stair's house stays
    for (std::size_t i = 2; i < boxes.size(); ++i) {
        CHECK(boxes[i].id.rfind("CL-7-", 0) == 0);
        CHECK_FALSE(overlap(boundsOf(boxes[i]), {0.0, 18.0, 60.0, 22.0})); // nothing on the stair
    }
    MassingLaneDress d;
    d.area = opts.area;
    d.kinds = opts.kinds;
    std::vector<MassingBox> out;
    massingLaneDress(streets, flat, d, out);
    CHECK_FALSE(out.empty());
    for (const MassingBox& b : out) CHECK(b.id.rfind("CL-7-", 0) == 0);
}

namespace {

// Separating axes for two turned footprints; true when they overlap by more than 1 cm.
bool turnedOverlap(const MassingBox& a, const MassingBox& b) {
    const double ta = a.rot * 3.14159265358979323846 / 180.0, tb = b.rot * 3.14159265358979323846 / 180.0;
    const double au[2] = {std::cos(ta), std::sin(ta)}, bu[2] = {std::cos(tb), std::sin(tb)};
    const double axes[4][2] = {{au[0], au[1]}, {-au[1], au[0]}, {bu[0], bu[1]}, {-bu[1], bu[0]}};
    for (const auto& ax : axes) {
        const double ra = 0.5 * a.sx * std::fabs(au[0] * ax[0] + au[1] * ax[1]) + 0.5 * a.sy * std::fabs(-au[1] * ax[0] + au[0] * ax[1]);
        const double rb = 0.5 * b.sx * std::fabs(bu[0] * ax[0] + bu[1] * ax[1]) + 0.5 * b.sy * std::fabs(-bu[1] * ax[0] + bu[0] * ax[1]);
        if (std::fabs((b.cx - a.cx) * ax[0] + (b.cy - a.cy) * ax[1]) >= ra + rb - 0.01) return false;
    }
    return true;
}

} // namespace

TEST_CASE("massingBuildTo with oblique fills a lane at an angle with turned infill, clear of streets and keep-clear rects") {
    // A 6 m lane at 45 degrees from (0, 0) to (70, 70), a 10 m street across it at y 35, a keep-clear rect beside it.
    std::vector<MassingStreet> streets{lane("TR-1", 6.0, 0.0, 0.0, 70.0, 70.0), lane("ST-9", 10.0, -20.0, 35.0, 100.0, 35.0)};
    streets[1].kind = "street";
    MassingBuildTo opts;
    opts.area = {-20.0, -20.0, 100.0, 100.0};
    opts.kinds = {"core_lane"};
    opts.keepClear.push_back({25.0, 15.0, 35.0, 25.0});
    const MassingGround flat{};

    std::vector<MassingBox> plain;
    const MassingBuildToCounts c0 = massingBuildTo(plain, streets, flat, opts);
    CHECK(c0.infill == 0); // without oblique a lane at an angle gets nothing
    CHECK(plain.empty());

    opts.oblique = true;
    std::vector<MassingBox> boxes;
    const MassingBuildToCounts c = massingBuildTo(boxes, streets, flat, opts);
    CHECK(c.oblique > 0);
    CHECK(c.oblique == c.infill);
    REQUIRE(boxes.size() == c.oblique);
    MassingBox keep;
    keep.cx = 30.0;
    keep.cy = 20.0;
    keep.sx = 10.0;
    keep.sy = 10.0;
    bool bothSides[2] = {false, false};
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        const MassingBox& b = boxes[i];
        CHECK(b.id.rfind("TR-1-IN", 0) == 0);
        CHECK(validId(b.id));
        CHECK(b.rot == doctest::Approx(45.0));
        CHECK(b.sx >= opts.minGap - 1e-9);
        CHECK(b.sy >= opts.depthMin - 1e-9);
        // The near face on the lane's line: the centre 3 m plus half the depth off the centre line.
        const double off = (b.cy - b.cx) / std::sqrt(2.0);
        CHECK(std::fabs(off) == doctest::Approx(3.0 + 0.5 * b.sy).epsilon(1e-6));
        bothSides[off > 0.0 ? 0 : 1] = true;
        // Clear of the street across (y 30..40) and the keep-clear rect.
        const double ex = 0.5 * (b.sx + b.sy) / std::sqrt(2.0); // the y half extent of a 45-degree footprint
        CHECK((b.cy + ex <= 30.0 + 0.03 || b.cy - ex >= 40.0 - 0.03));
        CHECK_FALSE(turnedOverlap(b, keep));
        for (std::size_t j = i + 1; j < boxes.size(); ++j) CHECK_FALSE(turnedOverlap(b, boxes[j]));
    }
    CHECK(bothSides[0]);
    CHECK(bothSides[1]);
}

TEST_CASE("massingBuildTo with oblique keeps a turned lot's ground within obliqueRise") {
    // The 45-degree lane on a ground rising 0.08 m a metre to the east: a 1 m rise allows 12.5 m of east-west extent.
    std::vector<MassingStreet> streets{lane("TR-2", 6.0, 0.0, 0.0, 70.0, 70.0)};
    MassingBuildTo opts;
    opts.area = {-10.0, -10.0, 180.0, 180.0};
    opts.oblique = true;
    opts.obliqueRise = 1.0;
    const Slope slope;
    std::vector<MassingBox> boxes;
    const MassingBuildToCounts c = massingBuildTo(boxes, streets, slope.ground, opts);
    CHECK(c.oblique > 0);
    for (const MassingBox& b : boxes) {
        const double ex = (b.sx + b.sy) / std::sqrt(2.0); // the east-west extent of a 45-degree footprint
        CHECK(0.08 * ex <= 1.0 + 0.02);
    }
}

TEST_CASE("massingBuildTo numbers new infill after the lane's infill already there") {
    std::vector<MassingStreet> streets{lane("CL-8", 6.0, 0.0, 50.0, 60.0, 50.0)};
    std::vector<MassingBox> boxes{house("CL-8-IN3", 50.0, 53.0, 58.0, 63.0)};
    MassingBuildTo opts;
    opts.area = {0.0, 0.0, 60.0, 100.0};
    const MassingGround flat{};
    const MassingBuildToCounts c = massingBuildTo(boxes, streets, flat, opts);
    REQUIRE(c.infill > 0);
    for (std::size_t i = 1; i < boxes.size(); ++i) {
        CHECK(boxes[i].id != "CL-8-IN1");
        CHECK(boxes[i].id.rfind("CL-8-IN", 0) == 0);
        CHECK(std::stoi(boxes[i].id.substr(7)) > 3);
    }
}

TEST_CASE("massingBuildTo puts no infill on ground under groundMin") {
    // The slope is 4 m at x = 0 and 0.08 m higher a metre east: ground under 4.8 m lies west of x = 10.
    std::vector<MassingStreet> streets{lane("TR-3", 6.0, 0.0, 0.0, 70.0, 70.0), lane("CL-9", 6.0, 0.0, 120.0, 100.0, 120.0)};
    MassingBuildTo opts;
    opts.area = {-10.0, -10.0, 180.0, 180.0};
    opts.oblique = true;
    opts.groundMin = 4.8;
    const Slope slope;
    std::vector<MassingBox> boxes;
    const MassingBuildToCounts c = massingBuildTo(boxes, streets, slope.ground, opts);
    CHECK(c.oblique > 0);
    CHECK(c.infill > c.oblique);
    for (const MassingBox& b : boxes) {
        const double ex = std::fabs(b.rot) < 1e-9 ? 0.5 * b.sx : 0.5 * (b.sx + b.sy) / std::sqrt(2.0);
        CHECK(b.cx - ex >= 10.0 - 0.6); // the ground is read every metre or so
    }
}
