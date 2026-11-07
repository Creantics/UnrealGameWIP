// The city building placer (ftcore/CityPlacer.h): the walk's count and spacing, every reject reason,
// the roof cap's lower-variant fallback, envelope growth, determinism, the variety and landmark checks,
// coverage, the readers and the example job under tests/data/place_example. Made-up grids and plans
// only; ids, class names and metres.

#include "ftcore/CityPlacer.h"

#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

// One class of one variant: w along the street, d deep, its top at top.
PlacerConfig boxConfig(double w, double d, double top, double gap, double front = 0.0, double rear = 0.0) {
    PlacerConfig c;
    c.seed = 7;
    c.setbackMargin = 1.0;
    c.endMargin = 0.0;
    c.rejectStep = 4.0;
    c.benchMaxSlopeDeg = 30.0;
    PlacerClass k;
    k.name = "box";
    k.frontGrowth = front;
    k.rearGrowth = rear;
    k.gap = gap;
    k.jitter = 0.0;
    k.benchOk = true;
    PlacerVariant v;
    v.name = "v";
    v.width = w;
    v.depth = d;
    v.storeys = 3;
    v.height = top;
    v.top = top;
    v.weight = 1.0;
    k.variants.push_back(v);
    c.classes.push_back(k);
    return c;
}

// x from x0, y from y0, one metre a sample.
PlacerGrid flatGrid(double x0, double y0, std::size_t nx, std::size_t ny, float h) {
    PlacerGrid g;
    g.x0 = x0;
    g.y0 = y0;
    g.cell = 1.0;
    g.nx = nx;
    g.ny = ny;
    g.height.assign(nx * ny, h);
    return g;
}

// The default ground: x -10..120, y -50..50, flat at 0.
PlacerGrid plainGrid() { return flatGrid(-10.0, -50.0, 131, 101, 0.0f); }

PlacerZone zoneFor(const PlacerConfig& c, double slopeMax = 20.0, bool bench = false) {
    PlacerZone z;
    z.id = "z";
    z.share = 1.0;
    z.slopeMaxDeg = slopeMax;
    z.bench = bench;
    z.buildableM2 = 8000.0;
    z.classWeights.assign(c.classes.size(), 1.0);
    return z;
}

PlacerFrontage street(const char* id, double x0, double y0, double x1, double y1, double halfWidth, bool left, bool right) {
    PlacerFrontage f;
    f.id = id;
    f.zone = "z";
    f.x0 = x0;
    f.y0 = y0;
    f.x1 = x1;
    f.y1 = y1;
    f.halfWidth = halfWidth;
    f.left = left;
    f.right = right;
    return f;
}

std::uint32_t rejects(const PlacerResult& r, PlaceReject why) { return r.zones.at(0).rejects[static_cast<std::size_t>(why)]; }

PlacerConfig dataConfig() {
    std::string text, error;
    REQUIRE(ftcore_test::readProjectData("Field/city_placer.json", text));
    PlacerConfig c;
    const bool ok = importPlacerConfig(text, c, &error);
    INFO(error);
    REQUIRE(ok);
    return c;
}

std::size_t classIndex(const PlacerConfig& c, const std::string& name) {
    for (std::size_t i = 0; i < c.classes.size(); ++i)
        if (c.classes[i].name == name) return i;
    FAIL("no class " << name);
    return 0;
}

std::size_t variantIndex(const PlacerClass& c, const std::string& name) {
    for (std::size_t i = 0; i < c.variants.size(); ++i)
        if (c.variants[i].name == name) return i;
    FAIL("no variant " << name);
    return 0;
}

// A zone that builds only the named class.
PlacerZone onlyClass(const PlacerConfig& c, const std::string& name, bool bench = false) {
    PlacerZone z = zoneFor(c, 20.0, bench);
    z.classWeights.assign(c.classes.size(), 0.0);
    z.classWeights[classIndex(c, name)] = 1.0;
    return z;
}

bool readBytes(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::string examplePath(const std::string& name) {
#if defined(FTCORE_CORE_TEST_DATA_DIR)
    return std::string(FTCORE_CORE_TEST_DATA_DIR) + "/place_example/" + name;
#else
    return "tests/data/place_example/" + name;
#endif
}

struct Example {
    PlacerConfig config;
    PlacerGrid grid;
    std::vector<PlacerZone> zones;
    std::vector<PlacerFrontage> frontages;
    std::vector<PlacerPoint> landmarks;
};

void loadExample(Example& ex) {
    ex.config = dataConfig();
    std::string text, error;
    INFO(error);
    REQUIRE(readBytes(examplePath("ground.json"), text));
    REQUIRE(importPlacerGridMeta(text, ex.grid, &error));
    const auto bytes = [](const std::string& s) { return reinterpret_cast<const std::uint8_t*>(s.data()); };
    REQUIRE(readBytes(examplePath("ground.u16"), text));
    REQUIRE(placerDecodeHeights(bytes(text), text.size(), ex.grid, &error));
    REQUIRE(readBytes(examplePath("roof.u16"), text));
    REQUIRE(placerDecodeMaxRoof(bytes(text), text.size(), ex.grid, &error));
    REQUIRE(readBytes(examplePath("mask.u8"), text));
    REQUIRE(placerDecodeNoBuild(bytes(text), text.size(), ex.grid, &error));
    REQUIRE(readBytes(examplePath("zones.csv"), text));
    REQUIRE(importPlacerZones(text, ex.config, ex.zones, &error));
    REQUIRE(readBytes(examplePath("frontages.csv"), text));
    REQUIRE(importPlacerFrontages(text, ex.frontages, &error));
    REQUIRE(readBytes(examplePath("landmarks.csv"), text));
    REQUIRE(importPlacerPoints(text, ex.landmarks, &error));
}

} // namespace

TEST_CASE("Placer: reject reasons have stable names") {
    const char* names[] = {"off_grid", "water", "band", "shoulder", "carriageway", "hold", "other", "slope", "roof_cap", "overlap",
                           "variety"};
    for (std::size_t i = 0; i < kPlaceRejectCount; ++i) CHECK(placeRejectName(static_cast<PlaceReject>(i)) == names[i]);
}

TEST_CASE("Placer: a straight frontage takes its count at width plus gap, set back and facing the street") {
    const PlacerConfig c = boxConfig(10.0, 10.0, 9.0, 2.0);
    const PlacerGrid g = plainGrid();
    PlacerResult r;
    std::string error;
    REQUIRE(placeCity(c, g, {zoneFor(c)}, {street("s", 0, 0, 100, 0, 5.0, true, true)}, r, &error));
    // Slots at 0, 12, ..., 84 (the next, 96..106, would run past 100): eight a side.
    REQUIRE(r.placed.size() == 16);
    CHECK(r.zones[0].placed == 16);
    CHECK(r.zones[0].candidates == 16);
    CHECK(r.zones[0].rejected() == 0);
    std::vector<double> leftX, rightX;
    for (const PlacedBuilding& b : r.placed) {
        if (b.side > 0) {
            CHECK(near(b.cy, 5.0 + 1.0 + 5.0)); // half-width + margin + half the depth
            CHECK(near(b.rot, -90.0));          // facing south, at the street
            leftX.push_back(b.cx);
        } else {
            CHECK(near(b.cy, -11.0));
            CHECK(near(b.rot, 90.0));
            rightX.push_back(b.cx);
        }
        CHECK(near(b.groundZ, 0.0));
        CHECK(near(b.roofZ, 9.0));
        CHECK(b.frontage == 0);
    }
    REQUIRE(leftX.size() == 8);
    REQUIRE(rightX.size() == 8);
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(near(leftX[i], 5.0 + 12.0 * static_cast<double>(i)));
        CHECK(near(rightX[i], leftX[i]));
    }
    CHECK(r.placed[0].id == "z_1");
    CHECK(r.placed[15].id == "z_16");
}

TEST_CASE("Placer: the zone's and the frontage's axis offsets turn each building") {
    PlacerConfig c = boxConfig(10.0, 10.0, 9.0, 4.0);
    PlacerZone z = zoneFor(c);
    z.axisDeg = 10.0;
    PlacerFrontage f = street("s", 0, 0, 100, 0, 5.0, true, false);
    f.axisDeg = 5.0;
    PlacerResult r;
    REQUIRE(placeCity(c, plainGrid(), {z}, {f}, r));
    REQUIRE(!r.placed.empty());
    for (const PlacedBuilding& b : r.placed) CHECK(near(b.rot, -75.0));
    CHECK(near(placerFootprint(c, r.placed[0], true).rot, -75.0));
}

TEST_CASE("Placer: slope rejects, and benches where the zone and class allow") {
    const PlacerConfig c = boxConfig(10.0, 10.0, 9.0, 2.0);
    const auto ramp = [](double deg) {
        PlacerGrid g = plainGrid();
        const double t = std::tan(deg * 3.14159265358979323846 / 180.0);
        for (std::size_t j = 0; j < g.ny; ++j)
            for (std::size_t i = 0; i < g.nx; ++i) g.height[j * g.nx + i] = static_cast<float>((g.x0 + static_cast<double>(i)) * t);
        return g;
    };
    const PlacerGrid g25 = ramp(25.0);
    CHECK(near(g25.slopeDegAt(40, 40), 25.0, 1e-3));
    const std::vector<PlacerFrontage> fs = {street("s", 0, 0, 100, 0, 5.0, true, false)};

    PlacerResult r;
    REQUIRE(placeCity(c, g25, {zoneFor(c, 20.0, false)}, fs, r));
    CHECK(r.placed.empty());
    CHECK(r.zones[0].candidates == 23); // 0, 4, ..., 88
    CHECK(rejects(r, PlaceReject::Slope) == 23);

    REQUIRE(placeCity(c, g25, {zoneFor(c, 20.0, true)}, fs, r));
    CHECK(r.placed.size() == 8);
    CHECK(r.zones[0].benched == 8);
    for (const PlacedBuilding& b : r.placed) {
        CHECK(b.bench);
        CHECK(b.slopeDeg > 20.0);
        CHECK(b.groundZ > b.groundMinZ); // set on the high side of its bench
        CHECK(near(b.roofZ, b.groundZ + 9.0));
    }

    PlacerConfig noBench = c;
    noBench.classes[0].benchOk = false;
    REQUIRE(placeCity(noBench, g25, {zoneFor(c, 20.0, true)}, fs, r));
    CHECK(rejects(r, PlaceReject::Slope) == 23);

    REQUIRE(placeCity(c, ramp(35.0), {zoneFor(c, 20.0, true)}, fs, r)); // past bench_max_slope_deg
    CHECK(r.placed.empty());
    CHECK(rejects(r, PlaceReject::Slope) == 23);
}

TEST_CASE("Placer: each no-build bit rejects with its own reason; the lowest bit names a mixed cell") {
    const PlacerConfig c = boxConfig(10.0, 10.0, 9.0, 2.0);
    const std::vector<PlacerFrontage> fs = {street("s", 0, 0, 100, 0, 5.0, true, false)};
    const struct {
        std::uint8_t bits;
        PlaceReject why;
    } cases[] = {{1, PlaceReject::Water},     {2, PlaceReject::Band},       {4, PlaceReject::Shoulder},
                 {8, PlaceReject::Carriageway}, {16, PlaceReject::Hold},      {32, PlaceReject::OtherMask},
                 {64, PlaceReject::OtherMask}, {1 | 16, PlaceReject::Water}, {8 | 32, PlaceReject::Carriageway}};
    for (const auto& k : cases) {
        CAPTURE(static_cast<int>(k.bits));
        PlacerGrid g = plainGrid();
        g.noBuild.assign(g.nx * g.ny, 0);
        // One masked sample under every candidate's envelope (y 6..16), none on the street.
        for (std::size_t i = 0; i < g.nx; ++i) g.noBuild[static_cast<std::size_t>(50 + 10) * g.nx + i] = k.bits;
        PlacerResult r;
        REQUIRE(placeCity(c, g, {zoneFor(c)}, fs, r));
        CHECK(r.placed.empty());
        CHECK(r.zones[0].candidates == 23);
        CHECK(rejects(r, k.why) == 23);
        CHECK(r.zones[0].rejected() == 23);
    }
    // A mask that misses the envelopes (on the carriageway) turns nothing down.
    PlacerGrid g = plainGrid();
    g.noBuild.assign(g.nx * g.ny, 0);
    for (std::size_t j = 50 - 4; j <= 50 + 4; ++j)
        for (std::size_t i = 0; i < g.nx; ++i) g.noBuild[j * g.nx + i] = 8;
    PlacerResult r;
    REQUIRE(placeCity(c, g, {zoneFor(c)}, fs, r));
    CHECK(r.placed.size() == 8);
}

TEST_CASE("Placer: the roof cap rejects a top over it, within a millimetre's tolerance") {
    const PlacerConfig c = boxConfig(10.0, 10.0, 9.0, 2.0);
    const std::vector<PlacerFrontage> fs = {street("s", 0, 0, 100, 0, 5.0, true, false)};
    PlacerGrid g = plainGrid();
    PlacerResult r;
    g.maxRoof.assign(g.nx * g.ny, 8.0f);
    REQUIRE(placeCity(c, g, {zoneFor(c)}, fs, r));
    CHECK(r.placed.empty());
    CHECK(rejects(r, PlaceReject::RoofCap) == 23);
    g.maxRoof.assign(g.nx * g.ny, 8.998f);
    REQUIRE(placeCity(c, g, {zoneFor(c)}, fs, r));
    CHECK(rejects(r, PlaceReject::RoofCap) == 23);
    g.maxRoof.assign(g.nx * g.ny, 9.0005f);
    REQUIRE(placeCity(c, g, {zoneFor(c)}, fs, r));
    CHECK(r.placed.size() == 8);
    g.maxRoof.assign(g.nx * g.ny, std::numeric_limits<float>::infinity());
    REQUIRE(placeCity(c, g, {zoneFor(c)}, fs, r));
    CHECK(r.placed.size() == 8);
}

TEST_CASE("Placer: an envelope corner past the grid's samples is off_grid") {
    const PlacerConfig c = boxConfig(10.0, 10.0, 9.0, 2.0);
    const PlacerGrid g = flatGrid(-10.0, -50.0, 131, 66, 0.0f); // y up to 15; the buildings reach 16
    PlacerResult r;
    REQUIRE(placeCity(c, g, {zoneFor(c)}, {street("s", 0, 0, 100, 0, 5.0, true, true)}, r));
    CHECK(rejects(r, PlaceReject::OffGrid) == 23); // the north side
    CHECK(r.placed.size() == 8);                   // the south side stands
    for (const PlacedBuilding& b : r.placed) CHECK(b.side < 0);
}

TEST_CASE("Placer: an envelope nearer than the gap to one already placed is overlap; exactly the gap passes") {
    const PlacerConfig c = boxConfig(10.0, 10.0, 9.0, 2.0);
    const PlacerGrid g = plainGrid();
    PlacerResult r;
    // The same street twice: the second walk finds every slot taken.
    REQUIRE(placeCity(c, g, {zoneFor(c)}, {street("a", 0, 0, 100, 0, 5.0, true, false), street("b", 0, 0, 100, 0, 5.0, true, false)}, r));
    CHECK(r.placed.size() == 8);
    CHECK(rejects(r, PlaceReject::Overlap) == 23);
    // A street to the north whose buildings back onto the first row: 1 m apart (under the gap) ...
    REQUIRE(placeCity(c, g, {zoneFor(c)}, {street("a", 0, 0, 100, 0, 5.0, true, false), street("b", 0, 33, 100, 33, 5.0, false, true)}, r));
    CHECK(r.placed.size() == 8);
    CHECK(rejects(r, PlaceReject::Overlap) == 23);
    // ... and 2 m apart, exactly the gap.
    REQUIRE(placeCity(c, g, {zoneFor(c)}, {street("a", 0, 0, 100, 0, 5.0, true, false), street("b", 0, 34, 100, 34, 5.0, false, true)}, r));
    CHECK(r.placed.size() == 16);
    CHECK(rejects(r, PlaceReject::Overlap) == 0);
}

TEST_CASE("Placer: under a roof cap a class tries its lower variants, weight-0 ones last") {
    PlacerConfig c = boxConfig(10.0, 10.0, 20.0, 2.0);
    PlacerVariant mid = c.classes[0].variants[0];
    mid.name = "mid";
    mid.top = mid.height = 12.0;
    PlacerVariant low = mid;
    low.name = "low";
    low.top = low.height = 6.0;
    low.weight = 0.0;
    c.classes[0].variants[0].name = "tall";
    c.classes[0].variants.push_back(mid);
    c.classes[0].variants.push_back(low);
    const std::vector<PlacerFrontage> fs = {street("s", 0, 0, 100, 0, 5.0, true, false)};
    PlacerGrid g = plainGrid();
    PlacerResult r;

    g.maxRoof.assign(g.nx * g.ny, 13.0f);
    REQUIRE(placeCity(c, g, {zoneFor(c)}, fs, r));
    REQUIRE(r.placed.size() == 8);
    std::uint32_t lowered = 0;
    for (const PlacedBuilding& b : r.placed) {
        CHECK(c.classes[0].variants[b.variant].name == "mid"); // mid weighs, so it goes before low
        CHECK(b.roofZ <= 13.0 + kPlacerRoofTolerance);
        lowered += b.lowered ? 1u : 0u;
    }
    CHECK(lowered > 0);
    CHECK(lowered == r.zones[0].lowered);

    g.maxRoof.assign(g.nx * g.ny, 7.0f);
    REQUIRE(placeCity(c, g, {zoneFor(c)}, fs, r));
    REQUIRE(r.placed.size() == 8);
    for (const PlacedBuilding& b : r.placed) {
        CHECK(c.classes[0].variants[b.variant].name == "low");
        CHECK(b.lowered);
    }

    g.maxRoof.assign(g.nx * g.ny, 5.0f); // nothing fits: the candidate counts as roof_cap
    REQUIRE(placeCity(c, g, {zoneFor(c)}, fs, r));
    CHECK(r.placed.empty());
    CHECK(rejects(r, PlaceReject::RoofCap) == 23);
}

TEST_CASE("Placer: under a +12 cap on +4 ground only the 7 m and 8 m lane_house variants stand") {
    const PlacerConfig c = dataConfig();
    const std::size_t lane = classIndex(c, "lane_house");
    const std::vector<PlacerFrontage> fs = {street("s", 0, 0, 100, 0, 5.0, true, true)};
    PlacerGrid g = plainGrid();
    std::fill(g.height.begin(), g.height.end(), 4.0f);
    g.maxRoof.assign(g.nx * g.ny, 12.0f);
    PlacerResult r;
    REQUIRE(placeCity(c, g, {onlyClass(c, "lane_house")}, fs, r));
    REQUIRE(!r.placed.empty());
    std::set<std::string> seen;
    for (const PlacedBuilding& b : r.placed) {
        REQUIRE(b.cls == lane);
        const std::string& name = c.classes[lane].variants[b.variant].name;
        CHECK((name == "v5" || name == "v6"));
        CHECK(b.roofZ <= 12.0 + kPlacerRoofTolerance);
        seen.insert(name);
    }
    CHECK(seen.size() == 2);
    CHECK(r.zones[0].lowered > 0);
    CHECK(rejects(r, PlaceReject::RoofCap) == 0);

    g.maxRoof.assign(g.nx * g.ny, 11.5f); // the 8 m one no longer fits
    REQUIRE(placeCity(c, g, {onlyClass(c, "lane_house")}, fs, r));
    REQUIRE(!r.placed.empty());
    for (const PlacedBuilding& b : r.placed) CHECK(c.classes[lane].variants[b.variant].name == "v5");

    // No apartment block fits under the cap at all.
    g.maxRoof.assign(g.nx * g.ny, 12.0f);
    REQUIRE(placeCity(c, g, {onlyClass(c, "apartment_block")}, fs, r));
    CHECK(r.placed.empty());
    CHECK(rejects(r, PlaceReject::RoofCap) == r.zones[0].candidates);
    CHECK(r.zones[0].candidates > 0);
}

TEST_CASE("Placer: the Data classes grow their envelopes front and rear") {
    const PlacerConfig c = dataConfig();
    REQUIRE(c.classes.size() == 3);
    CHECK(c.variantCount() == 27 + 1 + 6);
    const auto box = [&](const char* cls, const char* var, bool envelope) {
        PlacedBuilding b;
        b.cls = static_cast<std::uint16_t>(classIndex(c, cls));
        b.variant = static_cast<std::uint16_t>(variantIndex(c.classes[b.cls], var));
        b.rot = 0.0; // facing +x: the front is the +x side
        return placerFootprint(c, b, envelope);
    };
    // Apartment: 16 deep + 1.2 front + 2.0 rear = 19.2, its centre 0.4 behind the body's.
    CHECK(near(box("apartment_block", "w12_d16_s4", false).sx, 16.0));
    CHECK(near(box("apartment_block", "w12_d16_s4", true).sx, 19.2));
    CHECK(near(box("apartment_block", "w12_d16_s4", true).cx, -0.4));
    CHECK(near(box("apartment_block", "w12_d16_s4", true).sy, 12.0));
    // Walkway block: 14 deep grows to 17.2.
    CHECK(near(box("walkway_block", "w27_d14", true).sx, 17.2));
    CHECK(near(box("walkway_block", "w27_d14", true).cx, 0.4));
    // lane_house: front +0.9 only.
    CHECK(near(box("lane_house", "v1", true).sx, 10.9));
    CHECK(near(box("lane_house", "v1", true).cx, 0.45));

    const PlacerClass& apt = c.classes[classIndex(c, "apartment_block")];
    CHECK(near(apt.variants[variantIndex(apt, "w12_d16_s4")].height, 12.0));
    CHECK(near(apt.variants[variantIndex(apt, "w12_d16_s4")].top, 13.2));
    CHECK(near(apt.variants[variantIndex(apt, "w20_d40_s12")].top, 37.2));
    const PlacerClass& walk = c.classes[classIndex(c, "walkway_block")];
    CHECK(near(walk.variants[0].height, 27.0));
    CHECK(near(walk.variants[0].top, 30.9));
    const PlacerClass& lane = c.classes[classIndex(c, "lane_house")];
    CHECK(near(lane.variants[variantIndex(lane, "v1")].top, 9.9));
    CHECK(near(lane.variants[variantIndex(lane, "v5")].top, 7.0));
    CHECK(near(lane.variants[variantIndex(lane, "v6")].top, 8.0));

    // Placed, each body's front wall stands half-width + margin + front growth off the street.
    PlacerResult r;
    REQUIRE(placeCity(c, plainGrid(), {onlyClass(c, "apartment_block")}, {street("s", 0, 0, 100, 0, 5.0, true, false)}, r));
    REQUIRE(!r.placed.empty());
    for (const PlacedBuilding& b : r.placed) {
        const PlacerVariant& v = apt.variants[b.variant];
        CHECK(near(b.cy, 5.0 + c.setbackMargin + 1.2 + v.depth * 0.5));
        const MassingBox env = placerFootprint(c, b, true);
        CHECK(near(env.sx, v.depth + 3.2));
        CHECK(near(env.cy, b.cy + 0.4));                            // 0.4 behind the body, away from the street
        CHECK(near(env.cy - env.sx * 0.5, 5.0 + c.setbackMargin)); // its front edge, at the margin
    }
}

TEST_CASE("Placer: a seed gives the same city every time; another seed another city") {
    Example ex;
    loadExample(ex);
    PlacerResult a, b;
    std::string error;
    REQUIRE(placeCity(ex.config, ex.grid, ex.zones, ex.frontages, a, &error));
    REQUIRE(placeCity(ex.config, ex.grid, ex.zones, ex.frontages, b, &error));
    const std::string csvA = placerCsv(ex.config, ex.zones, ex.frontages, a);
    CHECK(csvA == placerCsv(ex.config, ex.zones, ex.frontages, b));
    CHECK(placerZoneLines(ex.zones, a) == placerZoneLines(ex.zones, b));
    PlacerConfig other = ex.config;
    other.seed += 1;
    REQUIRE(placeCity(other, ex.grid, ex.zones, ex.frontages, b, &error));
    CHECK(csvA != placerCsv(other, ex.zones, ex.frontages, b));
}

TEST_CASE("Placer: the example job runs, counting what it turns down") {
    Example ex;
    loadExample(ex);
    REQUIRE(ex.zones.size() == 2);
    REQUIRE(ex.frontages.size() == 2);
    PlacerResult r;
    std::string error;
    REQUIRE(placeCity(ex.config, ex.grid, ex.zones, ex.frontages, r, &error));
    REQUIRE(!r.placed.empty());
    const PlacerZoneStats& a = r.zones[0];
    const PlacerZoneStats& b = r.zones[1];
    CHECK(a.placed > 0);
    CHECK(a.lowered > 0);                                                      // lane_house variants under the +12 cap
    CHECK(a.rejects[static_cast<std::size_t>(PlaceReject::RoofCap)] > 0);       // the blocks under it
    CHECK(a.rejects[static_cast<std::size_t>(PlaceReject::Water)] > 0);         // the east end
    CHECK(b.rejects[static_cast<std::size_t>(PlaceReject::OffGrid)] == b.candidates); // past the north edge
    CHECK(a.candidates == a.placed + a.rejected());
    for (const PlacedBuilding& p : r.placed) {
        CHECK(p.id.rfind(ex.zones[p.zone].id + "_", 0) == 0);
        if (ex.grid.maxRoof.empty()) continue;
        CHECK(std::isfinite(p.roofZ));
    }
    // The CSV: a header and one row a building.
    const std::string csv = placerCsv(ex.config, ex.zones, ex.frontages, r);
    std::size_t lines = 0;
    for (const char ch : csv) lines += ch == '\n' ? 1u : 0u;
    CHECK(lines == r.placed.size() + 1);
    CHECK(csv.rfind("id,zone,class,variant,frontage,", 0) == 0);
    const std::string zl = placerZoneLines(ex.zones, r);
    CHECK(zl.find("zone id=a ") != std::string::npos);
    CHECK(zl.find("coverage id=b ") != std::string::npos);
}

TEST_CASE("Placer: variety fails a disc where one footprint-and-storey class passes 25% or under 6 classes stand") {
    VarietyParams p; // 300 m discs, 25%, 6 classes, 20 buildings
    const auto at = [](std::uint16_t cls, std::uint16_t var, double x, double y, double slope = 0.0) {
        PlacedBuilding b;
        b.cls = cls;
        b.variant = var;
        b.cx = x;
        b.cy = y;
        b.slopeDeg = slope;
        return b;
    };
    const std::vector<PlacerPoint> centre = {{0.0, 0.0}};
    VarietyResult v;

    std::vector<PlacedBuilding> list; // one class at 30%: 6 of 20, the rest 2 each over 7 more
    for (int i = 0; i < 6; ++i) list.push_back(at(0, 0, i, 0));
    for (std::uint16_t k = 1; k <= 7; ++k)
        for (int i = 0; i < 2; ++i) list.push_back(at(1, k, 10.0 * k, i));
    checkVariety(list, centre, p, v);
    REQUIRE(v.discs.size() == 1);
    CHECK(v.discs[0].judged);
    CHECK(v.discs[0].buildings == 20);
    CHECK(v.discs[0].classes == 8);
    CHECK(v.discs[0].topCount == 6);
    CHECK_FALSE(v.discs[0].pass);
    CHECK(v.failedShare == 1);
    CHECK_FALSE(v.pass);

    list.clear(); // exactly 25% passes: 5 of 20, 15 over 5 more (3 each)
    for (int i = 0; i < 5; ++i) list.push_back(at(0, 0, i, 0));
    for (std::uint16_t k = 1; k <= 5; ++k)
        for (int i = 0; i < 3; ++i) list.push_back(at(1, k, 10.0 * k, i));
    checkVariety(list, centre, p, v);
    CHECK(v.discs[0].classes == 6);
    CHECK(v.pass);

    list.clear(); // 5 classes at 20% each
    for (std::uint16_t k = 0; k < 5; ++k)
        for (int i = 0; i < 4; ++i) list.push_back(at(2, k, 10.0 * k, i));
    checkVariety(list, centre, p, v);
    CHECK(v.discs[0].classes == 5);
    CHECK_FALSE(v.discs[0].pass);
    CHECK(v.failedClasses == 1);
    CHECK(v.failedShare == 0);
    CHECK_FALSE(v.pass);

    list.clear(); // 8 classes x 3
    for (std::uint16_t k = 0; k < 8; ++k)
        for (int i = 0; i < 3; ++i) list.push_back(at(k, 0, 10.0 * k, i));
    checkVariety(list, centre, p, v);
    CHECK(v.discs[0].pass);
    CHECK(v.pass);

    // Steep buildings are not counted, far ones are outside the disc, and a thin disc is not judged.
    list.push_back(at(0, 0, 1, 1, 10.0));
    list.push_back(at(0, 0, 400, 0));
    checkVariety(list, centre, p, v);
    CHECK(v.counted == 25);
    CHECK(v.discs[0].buildings == 24);
    checkVariety(list, {{2000.0, 0.0}}, p, v);
    CHECK_FALSE(v.discs[0].judged);
    CHECK(v.judged == 0);
    CHECK_FALSE(v.pass); // nothing judged is not a pass
}

namespace {

// One 10 x 10 class of n variants (v0..), v0 weighing `heavy` and the rest 1; gap 2, no jitter.
PlacerConfig variedConfig(std::size_t n, double heavy) {
    PlacerConfig c = boxConfig(10.0, 10.0, 9.0, 2.0);
    PlacerVariant base = c.classes[0].variants[0];
    c.classes[0].variants.clear();
    for (std::size_t k = 0; k < n; ++k) {
        PlacerVariant v = base;
        v.name = "v" + std::to_string(k);
        v.storeys = static_cast<std::uint16_t>(k + 1);
        v.weight = k == 0 ? heavy : 1.0;
        c.classes[0].variants.push_back(v);
    }
    c.variety.radius = 200.0;   // one disc holds the whole street
    c.variety.maxShare = 0.25;
    c.variety.minClasses = 1;
    c.variety.minBuildings = 8;
    c.variety.maxSlopeDeg = 4.0;
    return c;
}

std::vector<PlacerFrontage> guardStreet() { return {street("s", 0, 0, 100, 0, 5.0, true, true)}; }

PlacerVarietyGuard oneDisc(bool held = true) {
    PlacerVarietyGuard g;
    g.centres = {{50.0, 0.0}};
    g.zones = {held};
    return g;
}

VarietyResult varietyOf(const PlacerConfig& c, const PlacerResult& r) {
    VarietyResult v;
    checkVariety(r.placed, {{50.0, 0.0}}, c.variety, v);
    return v;
}

} // namespace

TEST_CASE("Placer: the variety guard holds a disc within the share, standing in the class's other variants") {
    const PlacerConfig c = variedConfig(8, 20.0);
    const PlacerGrid g = plainGrid();
    PlacerResult free, held;
    REQUIRE(placeCity(c, g, {zoneFor(c)}, guardStreet(), free));
    CHECK_FALSE(varietyOf(c, free).pass); // v0 crowds the disc unguarded
    const PlacerVarietyGuard guard = oneDisc();
    std::string error;
    REQUIRE(placeCity(c, g, {zoneFor(c)}, guardStreet(), held, &error, &guard));
    const VarietyResult v = varietyOf(c, held);
    REQUIRE(v.discs.size() == 1);
    CHECK(v.discs[0].judged);
    CHECK(v.pass);
    CHECK(static_cast<double>(v.discs[0].topCount) <= 0.25 * static_cast<double>(v.discs[0].buildings));
    // Every variant is the same size, so the slots are the same; only the variants change.
    CHECK(held.placed.size() == free.placed.size());
    CHECK(held.zones[0].substituted > 0);
    CHECK(rejects(held, PlaceReject::Variety) == 0);
    // Same inputs, same city.
    PlacerResult again;
    REQUIRE(placeCity(c, g, {zoneFor(c)}, guardStreet(), again, &error, &guard));
    REQUIRE(again.placed.size() == held.placed.size());
    for (std::size_t i = 0; i < held.placed.size(); ++i) CHECK(again.placed[i].variant == held.placed[i].variant);
}

TEST_CASE("Placer: the variety guard stands in another class the zone weighs") {
    PlacerConfig c = variedConfig(1, 1.0); // class 0: one variant
    PlacerClass other = variedConfig(7, 1.0).classes[0];
    other.name = "other";
    c.classes.push_back(other);
    PlacerZone z = zoneFor(c);
    z.classWeights = {20.0, 1.0};
    const PlacerGrid g = plainGrid();
    PlacerResult free, held;
    REQUIRE(placeCity(c, g, {z}, guardStreet(), free));
    CHECK_FALSE(varietyOf(c, free).pass);
    const PlacerVarietyGuard guard = oneDisc();
    REQUIRE(placeCity(c, g, {z}, guardStreet(), held, nullptr, &guard));
    CHECK(varietyOf(c, held).pass);
    std::size_t first = 0;
    for (const PlacedBuilding& b : held.placed) first += b.cls == 0 ? 1 : 0;
    CHECK(first <= held.placed.size() / 4);
    CHECK(held.zones[0].substituted > 0);
}

TEST_CASE("Placer: with too few variants the guard stops the disc, and the rest are variety rejects") {
    // Four classes can fill 8 buildings at 2 each; a ninth would be 3 of 9, over floor(0.25 * 9) = 2.
    const PlacerConfig c = variedConfig(4, 20.0);
    const PlacerVarietyGuard guard = oneDisc();
    PlacerResult r;
    REQUIRE(placeCity(c, plainGrid(), {zoneFor(c)}, guardStreet(), r, nullptr, &guard));
    CHECK(r.placed.size() == 8);
    CHECK(rejects(r, PlaceReject::Variety) > 0);
    const VarietyResult v = varietyOf(c, r);
    CHECK(v.discs[0].topCount == 2);
    CHECK(v.pass);
}

TEST_CASE("Placer: the guard holds only its zones and the ground under the variety slope") {
    PlacerConfig c = variedConfig(8, 20.0);
    const PlacerGrid g = plainGrid();
    PlacerResult free, r;
    REQUIRE(placeCity(c, g, {zoneFor(c)}, guardStreet(), free));
    const auto same = [&](const PlacerResult& a) {
        REQUIRE(a.placed.size() == free.placed.size());
        for (std::size_t i = 0; i < a.placed.size(); ++i) CHECK(a.placed[i].variant == free.placed[i].variant);
        CHECK(a.zones[0].substituted == 0);
    };
    const PlacerVarietyGuard off = oneDisc(false);
    REQUIRE(placeCity(c, g, {zoneFor(c)}, guardStreet(), r, nullptr, &off));
    same(r);
    c.variety.maxSlopeDeg = 0.0; // flat ground is not under 0 degrees: nothing is counted
    const PlacerVarietyGuard on = oneDisc();
    REQUIRE(placeCity(c, g, {zoneFor(c)}, guardStreet(), r, nullptr, &on));
    same(r);
    // A guard must flag every zone.
    PlacerVarietyGuard bad = oneDisc();
    bad.zones.clear();
    std::string error;
    CHECK_FALSE(placeCity(c, g, {zoneFor(c)}, guardStreet(), r, &error, &bad));
    CHECK(error.find("variety guard") != std::string::npos);
}

TEST_CASE("Placer: the landmark reach finds the point farthest from every landmark") {
    const std::vector<PlacerPoint> points = {{0, 0}, {400, 0}, {600, 0}};
    LandmarkReach lr;
    checkLandmarkReach(points, {{0, 0}}, 500.0, lr);
    CHECK(lr.points == 3);
    CHECK(lr.uncovered == 1);
    CHECK(near(lr.worst, 600.0));
    CHECK(near(lr.worstAt.x, 600.0));
    CHECK_FALSE(lr.pass);
    checkLandmarkReach(points, {{0, 0}, {300, 0}}, 500.0, lr);
    CHECK(lr.uncovered == 0);
    CHECK(near(lr.worst, 300.0));
    CHECK(lr.pass);
    checkLandmarkReach(points, {}, 500.0, lr);
    CHECK_FALSE(lr.pass);
}

TEST_CASE("Placer: the variety discs stand on flat, dry samples") {
    PlacerGrid g = flatGrid(0.0, 0.0, 301, 301, 0.0f);
    g.noBuild.assign(g.nx * g.ny, 0);
    g.noBuild[100 * g.nx + 100] = static_cast<std::uint8_t>(NoBuild::Water);
    g.noBuild[200 * g.nx + 100] = static_cast<std::uint8_t>(NoBuild::Hold); // only water is skipped
    std::vector<PlacerPoint> pts;
    placerFlatPoints(g, 0.0, 0.0, 300.0, 300.0, 100.0, 4.0, pts);
    CHECK(pts.size() == 15);
    for (std::size_t j = 0; j < g.ny; ++j)
        for (std::size_t i = 0; i < 150; ++i) g.height[j * g.nx + i] = static_cast<float>(static_cast<double>(i) * 0.2); // ~11 degrees
    placerFlatPoints(g, 0.0, 0.0, 300.0, 300.0, 100.0, 4.0, pts);
    CHECK(pts.size() == 8); // x 200 and 300 only (x 100 and 0 are on the slope)
}

TEST_CASE("Placer: coverage per zone, of the bodies and of the grown envelopes") {
    const PlacerConfig c = boxConfig(10.0, 10.0, 9.0, 2.0, 1.0, 1.0);
    PlacerResult r;
    PlacerZone z = zoneFor(c);
    REQUIRE(placeCity(c, plainGrid(), {z}, {street("s", 0, 0, 100, 0, 5.0, true, false)}, r));
    REQUIRE(r.placed.size() == 8);
    CHECK(near(r.zones[0].bodyArea, 800.0));
    CHECK(near(r.zones[0].envelopeArea, 960.0));
    CHECK(near(placerCoverage(r.zones[0], z, false), 0.1));
    CHECK(near(placerCoverage(r.zones[0], z, true), 0.12));
    z.buildableM2 = 0.0;
    CHECK(placerCoverage(r.zones[0], z, false) == 0.0);
}

TEST_CASE("Placer: the zone share thins slots into empty lots") {
    const PlacerConfig c = boxConfig(10.0, 10.0, 9.0, 2.0);
    PlacerZone z = zoneFor(c);
    z.share = 0.0;
    PlacerResult r;
    REQUIRE(placeCity(c, plainGrid(), {z}, {street("s", 0, 0, 100, 0, 5.0, true, false)}, r));
    CHECK(r.placed.empty());
    CHECK(r.zones[0].candidates == 0);
    CHECK(r.zones[0].thinned == 10); // empty 10 m lots at 0, 10, ..., 90
    z.share = 0.5;
    REQUIRE(placeCity(c, plainGrid(), {z}, {street("s", 0, 0, 100, 0, 5.0, true, false)}, r));
    CHECK(r.zones[0].thinned > 0);
    CHECK(r.zones[0].placed > 0);
}

TEST_CASE("Placer: placeCity refuses inputs that don't agree") {
    const PlacerConfig c = boxConfig(10.0, 10.0, 9.0, 2.0);
    PlacerResult r;
    std::string error;
    PlacerGrid bad = plainGrid();
    bad.height.pop_back();
    CHECK_FALSE(placeCity(c, bad, {zoneFor(c)}, {}, r, &error));
    PlacerFrontage f = street("s", 0, 0, 100, 0, 5.0, true, false);
    f.zone = "nowhere";
    CHECK_FALSE(placeCity(c, plainGrid(), {zoneFor(c)}, {f}, r, &error));
    CHECK(error.find("nowhere") != std::string::npos);
    PlacerZone z = zoneFor(c);
    z.classWeights.push_back(1.0);
    CHECK_FALSE(placeCity(c, plainGrid(), {z}, {}, r, &error));
    CHECK_FALSE(placeCity(c, plainGrid(), {zoneFor(c), zoneFor(c)}, {}, r, &error));
}

TEST_CASE("Placer: the config reader checks its members") {
    std::string text, error;
    REQUIRE(ftcore_test::readProjectData("Field/city_placer.json", text));
    PlacerConfig c;
    REQUIRE(importPlacerConfig(text, c, &error));
    CHECK(c.classes[0].name == "apartment_block");
    CHECK(c.classes[0].variants.front().name == "w12_d16_s4");
    CHECK(c.classes[0].variants.back().name == "w20_d40_s12");
    CHECK(c.variety.minClasses == 6);
    CHECK(near(c.variety.maxShare, 0.25));
    CHECK(near(c.variety.radius, 300.0));
    CHECK(near(c.variety.landmarkReach, 500.0));
    const auto refused = [&](const std::string& from, const std::string& to) {
        std::string t = text;
        const std::size_t at = t.find(from);
        REQUIRE(at != std::string::npos);
        t.replace(at, from.size(), to);
        PlacerConfig out;
        const bool ok = importPlacerConfig(t, out, &error);
        return !ok && out.classes.empty();
    };
    CHECK(refused("\"version\": 1", "\"version\": 2"));
    CHECK(refused("\"seed\": 4242", "\"seed\": -1"));
    CHECK(refused("\"reject_step_m\": 4.0", "\"reject_step_m\": 0.0"));
    CHECK(refused("\"reject_step_m\"", "\"reject_stride_m\""));
    CHECK(refused("\"name\": \"walkway_block\"", "\"name\": \"apartment_block\""));
    CHECK(refused("\"name\": \"walkway_block\"", "\"name\": \"Walkway\""));
    CHECK(refused("\"storeys\": [4, 12] },", "\"storeys\": [12, 4] },"));
    CHECK(refused("\"max_share\": 0.25", "\"max_share\": 1.5"));
    CHECK(refused("\"width_m\": 5.0", "\"width_m\": 0.0"));
}

TEST_CASE("Placer: the zone, frontage and point readers") {
    const PlacerConfig c = dataConfig();
    std::vector<PlacerZone> zones;
    std::string error;
    REQUIRE(importPlacerZones("# zones\nid,share,slope_max_deg,bench,buildable_m2,weights\n"
                              "z1,0.5,15,1,1000,apartment_block:2;lane_house:1\n"
                              "z2,1,20,false,0,\n",
                              c, zones, &error));
    REQUIRE(zones.size() == 2);
    CHECK(near(zones[0].share, 0.5));
    CHECK(zones[0].bench);
    CHECK(near(zones[0].classWeights[0], 2.0));
    CHECK(near(zones[0].classWeights[1], 0.0));
    CHECK(near(zones[0].classWeights[2], 1.0));
    CHECK_FALSE(zones[1].bench);
    CHECK(near(zones[1].axisDeg, 0.0));
    const std::string head = "id,share,slope_max_deg,bench,buildable_m2,weights\n";
    CHECK_FALSE(importPlacerZones(head + "z1,0.5,15,1,1000,tower:1\n", c, zones, &error));
    CHECK(error.find("tower") != std::string::npos);
    CHECK_FALSE(importPlacerZones(head + "z1,1.5,15,1,1000,lane_house:1\n", c, zones, &error));
    CHECK_FALSE(importPlacerZones(head + "z1,1,15,1,1000,lane_house:1\nz1,1,15,1,1000,lane_house:1\n", c, zones, &error));
    CHECK_FALSE(importPlacerZones(head + "z1,1,15,maybe,1000,lane_house:1\n", c, zones, &error));
    CHECK_FALSE(importPlacerZones("id,share\nz1,1\n", c, zones, &error));
    REQUIRE(importPlacerZones("id,share,slope_max_deg,bench,buildable_m2,weights,axis_deg,target_per_km2\n"
                              "z1,1,15,0,1000,lane_house:1,12.5,300\n",
                              c, zones, &error));
    CHECK(near(zones[0].axisDeg, 12.5));
    CHECK(near(zones[0].targetPerKm2, 300.0));

    std::vector<PlacerFrontage> fs;
    REQUIRE(importPlacerFrontages("id,zone,x0,y0,x1,y1,half_width,sides,axis_deg\n"
                                  "f1,z1,0,0,10,0,4,both,\n"
                                  "\"f2\",z1,0,0,0,10,0,right,-3\n",
                                  fs, &error));
    REQUIRE(fs.size() == 2);
    CHECK((fs[0].left && fs[0].right));
    CHECK((!fs[1].left && fs[1].right));
    CHECK(near(fs[1].axisDeg, -3.0));
    const std::string fhead = "id,zone,x0,y0,x1,y1,half_width,sides\n";
    CHECK_FALSE(importPlacerFrontages(fhead + "f1,z1,0,0,10,0,4,up\n", fs, &error));
    CHECK_FALSE(importPlacerFrontages(fhead + "f1,z1,3,3,3,3,4,left\n", fs, &error));
    CHECK_FALSE(importPlacerFrontages(fhead + "f1,z1,0,0,10,0,-1,left\n", fs, &error));
    CHECK_FALSE(importPlacerFrontages(fhead + "f1,z1,0,0,10,0,4\n", fs, &error));

    std::vector<PlacerPoint> pts;
    REQUIRE(importPlacerPoints("id,x,y\na,1,2\nb,-3.5,4\n", pts, &error));
    REQUIRE(pts.size() == 2);
    CHECK(near(pts[1].x, -3.5));
    CHECK_FALSE(importPlacerPoints("id,x,y\na,1,2\na,3,4\n", pts, &error));
    CHECK_FALSE(importPlacerPoints("id,x,y\na,1,inf\n", pts, &error));
}

TEST_CASE("Placer: the grid sidecar and the rasters decode") {
    PlacerGrid g;
    std::string error;
    REQUIRE(importPlacerGridMeta("{\"samples_x\": 3, \"samples_y\": 2, \"spacing_m\": 2.0,"
                                 " \"origin_of_sample_0_0_m\": [-10.0, 5.0], \"anything_else\": [1, 2]}",
                                 g, &error));
    CHECK(g.nx == 3);
    CHECK(g.ny == 2);
    CHECK(near(g.cell, 2.0));
    CHECK(near(g.x0, -10.0));
    CHECK(near(g.y0, 5.0));
    // Heights: 32768 + 128 h, little-endian; 4.0 m, -1.0 m, 0.5 m, ...
    const std::uint16_t hv[6] = {33280, 32640, 32832, 32768, 32768, 32768};
    std::uint8_t bytes[12];
    for (int k = 0; k < 6; ++k) {
        bytes[2 * k] = static_cast<std::uint8_t>(hv[k] & 0xff);
        bytes[2 * k + 1] = static_cast<std::uint8_t>(hv[k] >> 8);
    }
    REQUIRE(placerDecodeHeights(bytes, 12, g, &error));
    CHECK(near(g.height[0], 4.0));
    CHECK(near(g.height[1], -1.0));
    CHECK(near(g.height[2], 0.5));
    CHECK_FALSE(placerDecodeHeights(bytes, 10, g, &error));
    bytes[0] = bytes[1] = 0xff; // 65535: no cap
    REQUIRE(placerDecodeMaxRoof(bytes, 12, g, &error));
    CHECK(std::isinf(g.maxRoof[0]));
    CHECK(near(g.maxRoof[1], -1.0));
    const std::uint8_t mask[6] = {0, 1, 8, 16, 32, 0};
    REQUIRE(placerDecodeNoBuild(mask, 6, g, &error));
    CHECK(g.noBuild[3] == 16);
    CHECK_FALSE(placerDecodeNoBuild(mask, 5, g, &error));
    CHECK(g.valid());
    CHECK_FALSE(importPlacerGridMeta("{\"samples_x\": 1, \"samples_y\": 2, \"spacing_m\": 2.0, \"origin_of_sample_0_0_m\": [0, 0]}", g,
                                     &error));
    CHECK_FALSE(importPlacerGridMeta("{\"samples_x\": 3, \"samples_y\": 2, \"spacing_m\": 0, \"origin_of_sample_0_0_m\": [0, 0]}", g,
                                     &error));
}
