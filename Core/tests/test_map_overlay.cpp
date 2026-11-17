#include "ftcore/MapOverlay.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

// A 400 x 300 m model (engine m, y south): a main road east-west at y 150, a street running north-east from (50, 250)
// to (350, 50) in two rows that share one name, a lane north-south at x 200, and a street with no name row.
// Names here are invented fixtures.
MiniMapModel model() {
    MiniMapModel m;
    m.minX = 0.f;
    m.minY = 0.f;
    m.maxX = 400.f;
    m.maxY = 300.f;
    auto road = [&](const char* id, MapRoadClass cls, std::initializer_list<Vec2> pts) {
        MapRoad r;
        r.id = id;
        r.cls = cls;
        r.width = 8.f;
        r.first = static_cast<std::uint32_t>(m.points.size());
        r.count = static_cast<std::uint32_t>(pts.size());
        for (const Vec2& p : pts) m.points.push_back(p);
        m.roads.push_back(r);
    };
    road("RD-A", MapRoadClass::Main, {{0.f, 150.f}, {400.f, 150.f}});
    road("ST-A1", MapRoadClass::Street, {{50.f, 250.f}, {200.f, 150.f}});
    road("ST-A2", MapRoadClass::Street, {{200.f, 150.f}, {350.f, 50.f}});
    road("LN-A", MapRoadClass::Lane, {{200.f, 0.f}, {200.f, 300.f}});
    road("ST-B", MapRoadClass::Street, {{20.f, 20.f}, {120.f, 20.f}});
    mapBuildGrid(m);
    return m;
}

const char* kNames = "id,kind,name,sign_form,district\n"
                     "D1,district,Pebblewick,Pebi,D1\n"
                     "RD-A,road,Ferrow Road,Feru,D1\n"
                     "ST-A1,street,Ashlit Street,,D1\n"
                     "ST-A2,street,Ashlit Street,,D1\n"
                     "LN-A,core_lane,Thimble Lane,,D1\n"
                     "ZZ-9,street,Nowhere Street,,D1\n";

} // namespace

TEST_CASE("map overlay: zoom levels show main roads always, streets from mid, lanes and stairs only near") {
    CHECK(mapRoadShown(MapRoadClass::Main, MapZoom::Far));
    CHECK_FALSE(mapRoadShown(MapRoadClass::Street, MapZoom::Far));
    CHECK(mapRoadShown(MapRoadClass::Street, MapZoom::Mid));
    CHECK_FALSE(mapRoadShown(MapRoadClass::Lane, MapZoom::Mid));
    CHECK_FALSE(mapRoadShown(MapRoadClass::Stair, MapZoom::Mid));
    CHECK(mapRoadShown(MapRoadClass::Lane, MapZoom::Near));
    CHECK(mapRoadShown(MapRoadClass::Stair, MapZoom::Near));
    CHECK(mapZoomIn(MapZoom::Near) == MapZoom::Near);
    CHECK(mapZoomIn(MapZoom::Far) == MapZoom::Mid);
    CHECK(mapZoomOut(MapZoom::Far) == MapZoom::Far);
    CHECK(mapZoomOut(MapZoom::Near) == MapZoom::Mid);
}

TEST_CASE("map overlay: the view maps world to screen and back, north up, y down, scaled by screen height") {
    const MapOverlayView v = mapOverlayView({200.f, 150.f}, MapZoom::Mid, 1920.f, 1080.f);
    CHECK(v.mPerPx == doctest::Approx(kMapZoomMPerPx[1]));
    const Vec2 c = mapOverlayToScreen(v, {200.f, 150.f});
    CHECK(c.x == doctest::Approx(960.f));
    CHECK(c.y == doctest::Approx(540.f));
    const Vec2 south = mapOverlayToScreen(v, {200.f, 164.f}); // 14 m south: 10 px down
    CHECK(south.y == doctest::Approx(550.f));
    const Vec2 back = mapOverlayFromScreen(v, {123.f, 456.f});
    const Vec2 again = mapOverlayToScreen(v, back);
    CHECK(again.x == doctest::Approx(123.f));
    CHECK(again.y == doctest::Approx(456.f));
    const MapOverlayView k4 = mapOverlayView({}, MapZoom::Mid, 3840.f, 2160.f);
    CHECK(k4.mPerPx == doctest::Approx(kMapZoomMPerPx[1] * 0.5f));
}

TEST_CASE("map overlay: the centre is held inside the area, or centred on a side smaller than the screen") {
    const MiniMapModel m = model();
    MapOverlayView v = mapOverlayView({-500.f, 9000.f}, MapZoom::Near, 1920.f, 1080.f); // 0.6 m/px: 1152 x 648 m
    const Vec2 c = mapOverlayClampCentre(m, v);
    CHECK(c.x == doctest::Approx(200.f)); // the area (400 m) is narrower than the view: centred
    CHECK(c.y == doctest::Approx(150.f));
    MiniMapModel big = m;
    big.maxX = 4000.f;
    big.maxY = 3000.f;
    const Vec2 h = mapOverlayClampCentre(big, v);
    CHECK(h.x == doctest::Approx(1920.f * 0.6f * 0.5f));
    CHECK(h.y == doctest::Approx(3000.f - 1080.f * 0.6f * 0.5f));
}

TEST_CASE("map overlay: names join to roads by id, once per name; districts listed; unknown ids counted") {
    const MiniMapModel m = model();
    MapNames n;
    std::string err;
    REQUIRE(mapLoadNames(m, kNames, n, &err));
    CHECK(n.names.size() == 5); // Pebblewick, Ferrow Road, Ashlit Street, Thimble Lane, Nowhere Street
    REQUIRE(n.roadName.size() == m.roads.size());
    CHECK(n.names[mapRoadName(n, 0)] == "Ferrow Road");
    CHECK(mapRoadName(n, 1) == mapRoadName(n, 2)); // two rows, one name
    CHECK(n.names[mapRoadName(n, 1)] == "Ashlit Street");
    CHECK(mapRoadName(n, 4) == kMapNoName);        // no row: no name
    CHECK(mapRoadName(n, -1) == kMapNoName);
    CHECK(mapRoadName(n, 99) == kMapNoName);
    REQUIRE(n.districts.size() == 1);
    CHECK(n.districts[0].id == "D1");
    CHECK(n.signForms[n.districts[0].name] == "Pebi");
    CHECK(n.unmatched == 1);
    MapNames bad;
    CHECK_FALSE(mapLoadNames(m, "id,kind\nRD-A,road\n", bad, &err));
    CHECK_FALSE(mapLoadNames(m, "id,kind,name\nRD-A,road,\n", bad, &err));
}

TEST_CASE("map overlay: upright angles stay in [-90, 90)") {
    for (float d = -720.f; d <= 720.f; d += 7.5f) {
        const float u = mapUprightDeg(d);
        CHECK(u >= -90.f);
        CHECK(u < 90.f);
        CHECK(std::fmod(std::abs(u - d), 180.f) == doctest::Approx(0.f).epsilon(1e-3)); // turned by half turns only
    }
    CHECK(mapUprightDeg(90.f) == doctest::Approx(-90.f)); // standing labels read bottom to top
    CHECK(mapUprightDeg(180.f) == doctest::Approx(0.f));
    CHECK(mapUprightDeg(NAN) == 0.f);
}

TEST_CASE("map overlay: the cursor's road names all its rows; the label sits on the longest stretch, upright, rising for a north-east road") {
    const MiniMapModel m = model();
    MapNames n;
    REQUIRE(mapLoadNames(m, kNames, n));
    const MapOverlayView v = mapOverlayView({200.f, 150.f}, MapZoom::Far, 1920.f, 1080.f); // 3 m/px: all on screen
    const int under = mapRoadAt(m, {125.f, 200.f}); // on ST-A1
    REQUIRE(under == 1);
    const std::uint16_t name = mapRoadName(n, under);
    MapLabel l;
    REQUIRE(mapLabelAlong(m, n, name, v, 40.f, 14.f, l));
    CHECK(l.name == name);
    CHECK(l.angleDeg >= -90.f);
    CHECK(l.angleDeg < 90.f);
    // North-east in the world is up and right on screen: the text rises, a negative screen angle.
    CHECK(l.angleDeg < 0.f);
    CHECK(l.angleDeg == doctest::Approx(-std::atan2(100.f, 150.f) * 180.f / 3.14159265f).epsilon(1e-3));
    CHECK(l.fits);
    // A name with no road on screen, or no name, places nothing.
    const MapOverlayView away = mapOverlayView({5000.f, 5000.f}, MapZoom::Near, 1920.f, 1080.f);
    CHECK_FALSE(mapLabelAlong(m, n, name, away, 40.f, 14.f, l));
    CHECK_FALSE(mapLabelAlong(m, n, kMapNoName, v, 40.f, 14.f, l));
    // A label longer than its stretch is still placed, flagged as not fitting.
    REQUIRE(mapLabelAlong(m, n, name, v, 900.f, 14.f, l));
    CHECK_FALSE(l.fits);
}

TEST_CASE("map overlay: the stretch used is the part inside the screen's margin") {
    const MiniMapModel m = model();
    MapNames n;
    REQUIRE(mapLoadNames(m, kNames, n));
    // Near zoom (0.6 m/px) centred 250 m east of the main road's east end: its west end (x 0, 1083 px left of the
    // centre) is cut off at the margin; its east end sits at 960 - 250 / 0.6 px.
    const MapOverlayView v = mapOverlayView({650.f, 150.f}, MapZoom::Near, 1920.f, 1080.f);
    MapLabel l;
    REQUIRE(mapLabelAlong(m, n, mapRoadName(n, 0), v, 40.f, 14.f, l, 24.f));
    CHECK(l.angleDeg == doctest::Approx(0.f));
    CHECK(l.at.y == doctest::Approx(540.f));
    CHECK(l.at.x == doctest::Approx((24.f + (960.f - 250.f / 0.6f)) * 0.5f));
}

TEST_CASE("map overlay: the placer keeps higher priority first and never two overlapping labels") {
    MapLabel a{{500.f, 500.f}, 0.f, 200.f, 20.f, 1, 1, true};  // a district
    MapLabel b{{520.f, 505.f}, 30.f, 120.f, 16.f, 2, 0, true}; // the cursor's road: wins over a
    MapLabel c{{900.f, 300.f}, -45.f, 120.f, 16.f, 3, 2, true};
    MapLabel d{{905.f, 300.f}, 10.f, 80.f, 16.f, 4, 2, true};  // over c, same priority: c came first
    MapLabel e{{100.f, 100.f}, 0.f, 0.f, 16.f, 5, 0, true};    // no size: never placed
    const MapLabel in[] = {a, b, c, d, e};
    MapLabelSet out;
    mapPlaceLabels(in, 5, out);
    REQUIRE(out.count == 2);
    CHECK(out.labels[0].name == 2);
    CHECK(out.labels[1].name == 3);
    for (std::size_t i = 0; i < out.count; ++i)
        for (std::size_t k = i + 1; k < out.count; ++k) CHECK_FALSE(mapLabelsOverlap(out.labels[i], out.labels[k]));
    mapPlaceLabels(nullptr, 3, out);
    CHECK(out.count == 0);
    mapPlaceLabels(in, 0, out);
    CHECK(out.count == 0);
}

TEST_CASE("map overlay: overlap is exact for turned boxes (no false hit across a diagonal gap)") {
    // Two thin labels side by side along a 45 degree line: their axis boxes overlap, the turned boxes don't.
    const MapLabel a{{0.f, 0.f}, 45.f, 200.f, 10.f, 1, 0, true};
    const MapLabel b{{20.f, -20.f}, 45.f, 200.f, 10.f, 2, 0, true}; // 28 px apart across the line
    CHECK_FALSE(mapLabelsOverlap(a, b, 6.f));
    const MapLabel c{{5.f, -5.f}, 45.f, 200.f, 10.f, 3, 0, true};   // 7 px apart: inside 10 + 6
    CHECK(mapLabelsOverlap(a, c, 6.f));
}

TEST_CASE("map overlay: a dense field of candidates places without overlaps and without allocating") {
    const MiniMapModel m = model();
    MapNames n;
    REQUIRE(mapLoadNames(m, kNames, n));
    MapLabel cand[96];
    for (int i = 0; i < 96; ++i)
        cand[i] = MapLabel{{80.f + static_cast<float>(i % 12) * 150.f, 80.f + static_cast<float>(i / 12) * 110.f},
                           static_cast<float>((i * 37) % 180) - 90.f, 140.f, 18.f, static_cast<std::uint16_t>(i),
                           static_cast<std::uint8_t>(i % 3), true};
    const MapOverlayView v = mapOverlayView({200.f, 150.f}, MapZoom::Far, 1920.f, 1080.f);
    MapLabelSet out;
    MapLabel along;
    const bool hooked = ftcore_test::allocHookInstalled();
    const std::uint64_t before = ftcore_test::allocStats().count;
    mapPlaceLabels(cand, 96, out);
    const bool placed = mapLabelAlong(m, n, mapRoadName(n, 1), v, 40.f, 14.f, along);
    const Vec2 c = mapOverlayClampCentre(m, v);
    if (hooked) CHECK(ftcore_test::allocStats().count == before);
    CHECK(placed);
    CHECK(c.x == doctest::Approx(200.f));
    CHECK(out.count > 0);
    CHECK(out.count <= kMaxMapLabels);
    for (std::size_t i = 0; i < out.count; ++i) {
        CHECK(out.labels[i].angleDeg >= -90.f);
        CHECK(out.labels[i].angleDeg < 90.f);
        for (std::size_t k = i + 1; k < out.count; ++k) CHECK_FALSE(mapLabelsOverlap(out.labels[i], out.labels[k]));
    }
}

TEST_CASE("map overlay: a building with a door on its wall or inside it is enterable; turned footprints respected") {
    MiniMapModel m = model();
    m.buildings.push_back(MapRect{100.f, 100.f, 10.f, 5.f, 0.f});           // door on its south wall
    m.buildings.push_back(MapRect{300.f, 100.f, 10.f, 5.f, 0.f});           // no door near
    m.buildings.push_back(MapRect{200.f, 250.f, 20.f, 2.f, 3.14159265f / 2.f}); // turned a quarter: long side north-south
    m.doors.push_back({104.f, 105.5f});
    m.doors.push_back({200.f, 268.f}); // 18 m south of the turned one's centre: inside it only because it is turned
    m.doors.push_back({316.f, 100.f}); // 6 m past the second's east wall: too far
    std::vector<std::uint8_t> e;
    mapEnterableBuildings(m, e);
    REQUIRE(e.size() == 3);
    CHECK(e[0] == 1);
    CHECK(e[1] == 0);
    CHECK(e[2] == 1);
    m.doors.clear();
    mapEnterableBuildings(m, e);
    CHECK(e == std::vector<std::uint8_t>{0, 0, 0});
}
