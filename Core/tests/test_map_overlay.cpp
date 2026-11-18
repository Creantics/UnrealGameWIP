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
    // Roads join their district; the district's anchor is the mean of its named roads' midpoints.
    CHECK(mapRoadDistrict(n, 0) == 0);
    CHECK(mapRoadDistrict(n, 3) == 0);
    CHECK(mapRoadDistrict(n, 4) == kMapNoName); // no row
    REQUIRE(n.districts[0].hasAnchor);
    CHECK(n.districts[0].anchor.x == doctest::Approx((200.f + 125.f + 275.f + 200.f) / 4.f));
    CHECK(n.districts[0].anchor.y == doctest::Approx((150.f + 200.f + 100.f + 150.f) / 4.f));
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

TEST_CASE("map overlay: a door lights only the building nearest it; turned footprints respected") {
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
    // Terraced neighbours sharing a wall: a door just off the first one's front, near the shared wall, lights only it.
    m.buildings.push_back(MapRect{50.f, 50.f, 5.f, 5.f, 0.f});
    m.buildings.push_back(MapRect{60.f, 50.f, 5.f, 5.f, 0.f});
    m.doors.push_back({54.6f, 55.3f}); // 0.3 m south of the first's front, 0.4 m from the shared wall
    mapEnterableBuildings(m, e);
    REQUIRE(e.size() == 5);
    CHECK(e[3] == 1);
    CHECK(e[4] == 0);
    m.doors.clear();
    mapEnterableBuildings(m, e);
    CHECK(e == std::vector<std::uint8_t>{0, 0, 0, 0, 0});
}

TEST_CASE("map overlay: the cursor picks the nearest road shown at the zoom, within reach") {
    const MiniMapModel m = model();
    // 3 m north of the main road, 3 m west of the lane: the lane is hidden at mid zoom, so the main road is picked.
    CHECK(mapRoadNear(m, {197.f, 147.f}, 10.f, MapZoom::Mid) == 0);
    CHECK(mapRoadNear(m, {197.f, 20.f}, 10.f, MapZoom::Near) == 3); // only the lane is near
    CHECK(mapRoadNear(m, {197.f, 20.f}, 10.f, MapZoom::Mid) == -1);
    CHECK(mapRoadNear(m, {70.f, 23.f}, 10.f, MapZoom::Mid) == 4);   // the unnamed street
    CHECK(mapRoadNear(m, {70.f, 23.f}, 10.f, MapZoom::Far) == -1);  // streets hidden at far
    CHECK(mapRoadNear(m, {70.f, 23.f}, 2.f, MapZoom::Mid) == -1);   // out of reach
}

TEST_CASE("map overlay: the water grid merges into one rectangle per run of a row, after the given rectangles") {
    MiniMapModel m = model();
    m.waterRects.push_back(MapRect{10.f, 10.f, 2.f, 2.f, 0.3f});
    m.waterCell = 2.f;
    m.waterW = 5;
    m.waterH = 2;
    m.water = {1, 1, 0, 1, 0,
               0, 0, 0, 0, 1};
    std::vector<MapRect> w;
    mapWaterRuns(m, w);
    REQUIRE(w.size() == 4);
    CHECK(w[0].rot == doctest::Approx(0.3f));
    CHECK(w[1].cx == doctest::Approx(2.f)); // cells 0-1 of row 0: x 0..4
    CHECK(w[1].hx == doctest::Approx(2.f));
    CHECK(w[1].cy == doctest::Approx(1.f));
    CHECK(w[1].hy == doctest::Approx(1.f));
    CHECK(w[2].cx == doctest::Approx(7.f)); // cell 3
    CHECK(w[3].cx == doctest::Approx(9.f)); // row 1, cell 4
    CHECK(w[3].cy == doctest::Approx(3.f));
    m.water.resize(3); // short grid: only the rectangles
    mapWaterRuns(m, w);
    CHECK(w.size() == 1);
}

TEST_CASE("map overlay: far zoom fits the whole area, letterboxed, and is never nearer than mid") {
    MiniMapModel m = model(); // 400 x 300 m: small, so far is held off mid
    const MapOverlayView mid = mapOverlayViewIn(m, {200.f, 150.f}, MapZoom::Mid, 1920.f, 1080.f);
    const MapOverlayView far = mapOverlayViewIn(m, {200.f, 150.f}, MapZoom::Far, 1920.f, 1080.f);
    CHECK(mid.mPerPx == doctest::Approx(1.4f));
    CHECK(far.mPerPx == doctest::Approx(1.4f * kMapFarOverMid));
    // A tall strip 1300 x 3500 m: height binds; the whole of it is on screen, the width letterboxed.
    m.maxX = 1300.f;
    m.maxY = 3500.f;
    const MapOverlayView tall = mapOverlayViewIn(m, {650.f, 1750.f}, MapZoom::Far, 1920.f, 1080.f);
    CHECK(tall.mPerPx == doctest::Approx(3500.f / (1080.f * (1.f - 2.f * kMapFarMargin))));
    const Vec2 c = mapOverlayClampCentre(m, tall);
    CHECK(c.x == doctest::Approx(650.f));
    CHECK(c.y == doctest::Approx(1750.f));
    const Vec2 top = mapOverlayToScreen(tall, {0.f, 0.f}), bottom = mapOverlayToScreen(tall, {1300.f, 3500.f});
    CHECK(top.y == doctest::Approx(1080.f * kMapFarMargin).epsilon(0.001));
    CHECK(bottom.y == doctest::Approx(1080.f * (1.f - kMapFarMargin)).epsilon(0.001));
    CHECK(bottom.x - top.x < 1920.f * 0.5f);
    // Near and mid stay the table's.
    CHECK(mapOverlayViewIn(m, c, MapZoom::Near, 1920.f, 1080.f).mPerPx == doctest::Approx(0.6f));
}

TEST_CASE("map overlay: the sea side comes from the water grid's edge columns") {
    MiniMapModel m = model();
    m.water.clear();
    m.waterW = 0;
    m.waterH = 0;
    CHECK(mapSeaSide(m) == 0); // no grid
    m.waterCell = 2.f;
    m.waterW = 3;
    m.waterH = 2;
    m.water = {0, 0, 1,
               0, 0, 1};
    CHECK(mapSeaSide(m) == 1);
    m.water = {1, 0, 0,
               1, 0, 1};
    CHECK(mapSeaSide(m) == -1);
    m.water = {0, 1, 0,
               0, 1, 0};
    CHECK(mapSeaSide(m) == 0);
}

TEST_CASE("map overlay: water and place rows: water along a canal or at its point, places at theirs, the rest unmatched") {
    const MiniMapModel m = model();
    const char* csv = "id,kind,name,sign_form,district,x,y\n"
                      "D1,district,Pebblewick,Pebi,D1,,\n"
                      "RD-A,road,Ferrow Road,Feru,D1,,\n"
                      "LN-A,canal,Ladle Cut,,D1,,\n"
                      "RV,river,the Wend,Wen,D1,300,40\n"
                      "SQ-1,square,Tinker Square,,D1,120,200\n"
                      "QY-1,quay,Mud Quay,,D1,,\n";
    MapNames n;
    REQUIRE(mapLoadNames(m, csv, n));
    REQUIRE(n.names.size() == 6);
    REQUIRE(n.nameKind.size() == 6);
    CHECK(n.nameKind[0] == MapNameKind::Road); // the district
    CHECK(n.nameKind[1] == MapNameKind::Road);
    CHECK(n.nameKind[2] == MapNameKind::Water); // a canal on a road's geometry
    CHECK(n.nameKind[3] == MapNameKind::Water);
    CHECK(n.nameKind[4] == MapNameKind::Place);
    REQUIRE(n.places.size() == 2);
    CHECK(n.places[0].name == 3);
    CHECK(n.places[0].at.x == doctest::Approx(300.f));
    CHECK(n.places[1].at.y == doctest::Approx(200.f));
    CHECK(n.unmatched == 1); // the quay: no road and no point
    CHECK(mapRoadName(n, 3) == 2);
}

TEST_CASE("map overlay: label picks by zoom: cursor first, districts and water points always, streets from mid, lanes and places near") {
    const MiniMapModel m = model();
    const char* csv = "id,kind,name,sign_form,district,x,y\n"
                      "D1,district,Pebblewick,Pebi,D1,,\n"
                      "RD-A,road,Ferrow Road,Feru,D1,,\n"
                      "ST-A1,street,Ashlit Street,,D1,,\n"
                      "ST-A2,street,Ashlit Street,,D1,,\n"
                      "LN-A,core_lane,Thimble Lane,,D1,,\n"
                      "RV,river,the Wend,,D1,300,40\n"
                      "SQ-1,square,Tinker Square,,D1,120,200\n";
    MapNames n;
    REQUIRE(mapLoadNames(m, csv, n));
    REQUIRE(n.districts.size() == 1);
    REQUIRE(n.districts[0].hasAnchor);
    std::vector<std::uint8_t> seen(n.names.size(), 7);
    MapLabelPick out[16];
    auto names = [&](MapZoom z, int picked) {
        const MapOverlayView v = mapOverlayView({200.f, 150.f}, z, 1920.f, 1080.f);
        const std::size_t k = mapLabelPicks(m, n, v, z, picked, out, 16, seen.data());
        std::string s;
        for (std::size_t i = 0; i < k; ++i) s += n.names[out[i].name] + (out[i].along ? "" : "@") + ";";
        return s;
    };
    CHECK(names(MapZoom::Far, -1) == "Pebblewick@;the Wend@;");
    CHECK(names(MapZoom::Mid, -1) == "Pebblewick@;the Wend@;Ferrow Road;Ashlit Street;");
    CHECK(names(MapZoom::Near, -1) == "Pebblewick@;the Wend@;Ferrow Road;Ashlit Street;Tinker Square@;Thimble Lane;");
    // The cursor's road first, once; a lane under the cursor is named at mid.
    CHECK(names(MapZoom::Mid, 3) == "Thimble Lane;Pebblewick@;the Wend@;Ferrow Road;Ashlit Street;");
    CHECK(out[0].rank == MapLabelRank::Cursor);
    CHECK(out[1].rank == MapLabelRank::District);
    CHECK(out[2].rank == MapLabelRank::Water);
    // Off screen: a view far east of the model picks nothing; the cap holds.
    const MapOverlayView away = mapOverlayView({5000.f, 150.f}, MapZoom::Near, 1920.f, 1080.f);
    CHECK(mapLabelPicks(m, n, away, MapZoom::Near, -1, out, 16, seen.data()) == 0);
    const MapOverlayView v = mapOverlayView({200.f, 150.f}, MapZoom::Near, 1920.f, 1080.f);
    CHECK(mapLabelPicks(m, n, v, MapZoom::Near, -1, out, 2, seen.data()) == 2);
    // No allocation.
    const bool hooked = ftcore_test::allocHookInstalled();
    const std::uint64_t before = ftcore_test::allocStats().count;
    const std::size_t k = mapLabelPicks(m, n, v, MapZoom::Near, 1, out, 16, seen.data());
    const MapOverlayView f = mapOverlayViewIn(m, {200.f, 150.f}, MapZoom::Far, 1920.f, 1080.f);
    if (hooked) CHECK(ftcore_test::allocStats().count == before);
    CHECK(k == 6);
    CHECK(f.mPerPx > 0.f);
    CHECK_FALSE(mapRoadLabelled(MapRoadClass::Stair, MapZoom::Near));
}

TEST_CASE("map overlay: plan-metre points turn to engine metres, and a district's own point is its anchor") {
    const MiniMapModel m = model();
    const char* csv = "id,kind,name,sign_form,district,x,y\n"
                      "D1,district,Pebblewick,Pebi,D1,100,900\n"
                      "D2,district,Hobcross,,D2,,\n"
                      "RD-A,road,Ferrow Road,Feru,D2,,\n"
                      "SQ-1,square,Tinker Square,,D1,250,500\n"
                      "ST-9,stair,Crook Steps,,D1,300,300\n" // a line off the model: never a point name
                      "LS-9,link_stair,Gull Steps,,D1,310,300\n";
    MapNames plan;
    REQUIRE(mapLoadNames(m, csv, plan, nullptr, 1000.0));
    REQUIRE(plan.places.size() == 1);
    CHECK(plan.unmatched == 2);
    CHECK(plan.places[0].at.x == doctest::Approx(-250.f)); // 250 - 500
    CHECK(plan.places[0].at.y == doctest::Approx(0.f));    // 500 - 500
    REQUIRE(plan.districts.size() == 2);
    CHECK(plan.districts[0].hasAnchor);
    CHECK(plan.districts[0].anchor.x == doctest::Approx(-400.f));
    CHECK(plan.districts[0].anchor.y == doctest::Approx(-400.f));
    CHECK(plan.districts[1].hasAnchor); // no point: from its road
    // No plan side: read as engine metres.
    MapNames eng;
    REQUIRE(mapLoadNames(m, csv, eng));
    CHECK(eng.places[0].at.x == doctest::Approx(250.f));
    CHECK(eng.districts[0].anchor.y == doctest::Approx(900.f));
}

TEST_CASE("map overlay: the names table's own anchors land where plan metres say (Lanternfall, the Hearth, Crumb Lane)") {
    // Rows as the world lane's table has them (plan m); a 1300 m plan turns plan (x, y) to engine (x - 650, 650 - y).
    const MiniMapModel m = model();
    const char* csv = "id,kind,name,sign_form,district,x,y\n"
                      "D2,district,Lanternfall,Tavren,D2,747.8,479.8\n"
                      "Z22,square,the Hearth,,D2,730.0,495.0\n"
                      "Z23,lane,Crumb Lane,,D2,790.0,410.0\n"
                      "Z25,street,Lantern Row,,D2,865.0,500.0\n";
    MapNames n;
    REQUIRE(mapLoadNames(m, csv, n, nullptr, 1300.0));
    REQUIRE(n.districts.size() == 1);
    CHECK(n.districts[0].anchor.x == doctest::Approx(97.8f));
    CHECK(n.districts[0].anchor.y == doctest::Approx(170.2f));
    REQUIRE(n.places.size() == 3); // zone rows are points whatever their kind
    CHECK(n.names[n.places[0].name] == "the Hearth");
    CHECK(n.places[0].at.x == doctest::Approx(80.f));
    CHECK(n.places[0].at.y == doctest::Approx(155.f));
    CHECK(n.names[n.places[1].name] == "Crumb Lane");
    CHECK(n.places[1].at.x == doctest::Approx(140.f));
    CHECK(n.places[1].at.y == doctest::Approx(240.f));
    CHECK(n.names[n.places[2].name] == "Lantern Row");
    CHECK(n.places[2].at.x == doctest::Approx(215.f));
    CHECK(n.places[2].at.y == doctest::Approx(150.f));
    CHECK(n.unmatched == 0);
}

TEST_CASE("map overlay: a bridge or a *_walk row off the model stays unmatched, never a point name") {
    const MiniMapModel m = model();
    const char* csv = "id,kind,name,sign_form,district,x,y\n"
                      "BR-01,bridge,Tide Bridge,,,700.0,600.0\n"
                      "W-01,harbour_walk,Quay Walk,,,710.0,610.0\n"
                      "P-01,park,Kiln Green,,,720.0,620.0\n";
    MapNames n;
    REQUIRE(mapLoadNames(m, csv, n, nullptr, 1300.0));
    REQUIRE(n.places.size() == 1);
    CHECK(n.names[n.places[0].name] == "Kiln Green");
    CHECK(n.unmatched == 2);
}

namespace {

// One road named "Seam Run" from the points given (engine m), viewed 1 m per px centred on `centre`.
struct AlongFixture {
    MiniMapModel m;
    MapNames n;
    MapOverlayView v;
};

AlongFixture alongFixture(std::initializer_list<Vec2> pts, Vec2 centre) {
    AlongFixture f;
    f.m.minX = -2000.f;
    f.m.minY = -2000.f;
    f.m.maxX = 4000.f;
    f.m.maxY = 2000.f;
    MapRoad r;
    r.id = "AL-1";
    r.cls = MapRoadClass::Lane;
    r.width = 3.f;
    r.count = static_cast<std::uint32_t>(pts.size());
    for (const Vec2& p : pts) f.m.points.push_back(p);
    f.m.roads.push_back(r);
    mapBuildGrid(f.m);
    REQUIRE(mapLoadNames(f.m, "id,kind,name,sign_form,district\nAL-1,alley,Seam Run,,\n", f.n));
    f.v = MapOverlayView{centre, 1.f, 1920.f, 1080.f};
    return f;
}

} // namespace

TEST_CASE("map overlay: a lane drawn in short straight pieces is one stretch for its name") {
    // Three 10 m pieces in a line: the name (25 px, 2 px pad each end) fits the 30 m run, not any one piece.
    const AlongFixture f = alongFixture({{100.f, 100.f}, {110.f, 100.f}, {120.f, 100.f}, {130.f, 100.f}}, {115.f, 100.f});
    MapLabel l;
    REQUIRE(mapLabelAlong(f.m, f.n, mapRoadName(f.n, 0), f.v, 25.f, 10.f, l, 24.f, 2.f));
    CHECK(l.fits);
    CHECK(l.at.x == doctest::Approx(960.f));
    CHECK(l.at.y == doctest::Approx(540.f));
    CHECK(l.angleDeg == doctest::Approx(0.f));
    REQUIRE(mapLabelAlong(f.m, f.n, mapRoadName(f.n, 0), f.v, 27.f, 10.f, l, 24.f, 2.f));
    CHECK_FALSE(l.fits);
}

TEST_CASE("map overlay: a corner splits the stretch; a gentle curve joins up to the bend limit") {
    // An L of two 10 m legs: two stretches of 10, so a 15 px name does not fit.
    const AlongFixture corner = alongFixture({{100.f, 100.f}, {110.f, 100.f}, {110.f, 110.f}}, {105.f, 105.f});
    MapLabel l;
    REQUIRE(mapLabelAlong(corner.m, corner.n, mapRoadName(corner.n, 0), corner.v, 15.f, 10.f, l, 24.f, 0.f));
    CHECK_FALSE(l.fits);
    // Pieces turning 5 degrees each (headings 0, 5, 10, 15): the first three stay within the limit of the first
    // heading, the fourth starts a new stretch. The label sits on the first three's chord.
    const float rad = 3.14159265f / 180.f;
    Vec2 p[5] = {{100.f, 100.f}};
    for (int i = 0; i < 4; ++i) {
        const float a = 5.f * static_cast<float>(i) * rad;
        p[i + 1] = Vec2{p[i].x + 10.f * std::cos(a), p[i].y + 10.f * std::sin(a)};
    }
    REQUIRE(kMapAlongBendDeg > 10.f);
    REQUIRE(kMapAlongBendDeg < 15.f);
    const AlongFixture curve = alongFixture({p[0], p[1], p[2], p[3], p[4]}, {100.f, 100.f});
    const float chord = std::hypot(p[3].x - p[0].x, p[3].y - p[0].y);
    REQUIRE(mapLabelAlong(curve.m, curve.n, mapRoadName(curve.n, 0), curve.v, chord - 1.f, 10.f, l, 24.f, 0.f));
    CHECK(l.fits);
    CHECK(l.at.x == doctest::Approx(960.f + (p[0].x + p[3].x) * 0.5f - 100.f));
    CHECK(l.at.y == doctest::Approx(540.f + (p[0].y + p[3].y) * 0.5f - 100.f));
    CHECK(l.angleDeg == doctest::Approx(std::atan2(p[3].y - p[0].y, p[3].x - p[0].x) / rad).epsilon(1e-3));
}

TEST_CASE("map overlay: a stretch of pieces stops at the screen's margin") {
    // Pieces every 10 m from x 0 to x 2000; on screen (1 m/px, centre x 500) x runs -460..1460, the margin cuts at
    // 1436, so the stretch is screen px 460..1896.
    std::vector<Vec2> pts;
    for (int i = 0; i <= 200; ++i) pts.push_back(Vec2{10.f * static_cast<float>(i), 100.f});
    AlongFixture f = alongFixture({{0.f, 100.f}}, {500.f, 100.f});
    f.m.points = pts;
    f.m.roads[0].count = static_cast<std::uint32_t>(pts.size());
    MapLabel l;
    REQUIRE(mapLabelAlong(f.m, f.n, mapRoadName(f.n, 0), f.v, 1400.f, 10.f, l, 24.f, 0.f));
    CHECK(l.fits);
    CHECK(l.at.x == doctest::Approx((460.f + 1896.f) * 0.5f));
    REQUIRE(mapLabelAlong(f.m, f.n, mapRoadName(f.n, 0), f.v, 1500.f, 10.f, l, 24.f, 0.f));
    CHECK_FALSE(l.fits);
}
