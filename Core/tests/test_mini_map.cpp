#include "ftcore/MiniMap.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

bool closeTo(Vec2 a, Vec2 b, float eps = 1e-4f) { return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps; }

// A 40 x 40 m plan square at side 1000 (plan 900..940 east, 420..460 north): a main road along plan y 440 from x 905
// to 935, a lane north from it at x 920, a canal at the east edge, a tall house, a low kerb and a wall, the landmark,
// one door, and sea east of plan x 930 where the ground is under the datum.
struct Fixture {
    std::vector<std::uint16_t> height;
    MiniMapSource src;
    Fixture() {
        // 50 x 50 samples at 1 m from plan (895, 415): ground 2 m up, east of x 930 under the datum.
        height.resize(50 * 50);
        for (int iy = 0; iy < 50; ++iy)
            for (int ix = 0; ix < 50; ++ix)
                height[static_cast<std::size_t>(iy) * 50 + static_cast<std::size_t>(ix)] =
                    static_cast<std::uint16_t>(32768 + (895 + ix >= 930 ? -3 : 2) * 128);
        src.roads = "id,kind,width_m,pts,canal\n"
                    "RD-1,coast_road,10,905:440;935:440,\n"
                    "CN-1,canal,,936:425;939:455,1:2\n";
        src.streets = "# PRIVATE\n"
                      "id,kind,width_m,pts,pavement_m\n"
                      "LN-1,core_lane,3,920:440;920:458,\n"
                      "XX-1,quay_edge,3,901:421;902:422,\n";
        src.boxes = "id,row,src,kind,cx,cy,sx,sy,rot,z0,z1,cover,slope\n"
                    "B-1,R,BLD,kit,910,450,8,6,30,0,9,False,\n"
                    "B-2,R,BLD,kerb,910,430,8,1,0,0,0.2,False,\n"
                    "B-3,R,BLD,wall,930,450,1,8,0,0,4,False,\n";
        src.towers = "id,kind,cx,cy,z0,sx,sy,rot,floors,height_m,note\n"
                     "T-1,landmark,926,452,0,4,4,0,0,80,placed\n";
        src.doors = "id,kind,width_m,pts,pavement_m\n"
                    "D-1,door,1.6,913:446:2;913:444:2,\n";
        src.height = height.data();
        src.samplesX = 50;
        src.samplesY = 50;
        src.originX = 895.0;
        src.originY = 415.0;
        src.seaFromX = 925.0;
        src.side = 1000.0;
        src.areaX0 = 900.0;
        src.areaY0 = 420.0;
        src.areaX1 = 940.0;
        src.areaY1 = 460.0;
        src.waterCell = 1.f;
    }
};

} // namespace

TEST_CASE("minimap: road classes by kind") {
    MapRoadClass c{};
    CHECK((mapRoadClass("coast_road", c) && c == MapRoadClass::Main));
    CHECK((mapRoadClass("bridge", c) && c == MapRoadClass::Main));
    CHECK((mapRoadClass("street", c) && c == MapRoadClass::Street));
    CHECK((mapRoadClass("core_lane", c) && c == MapRoadClass::Lane));
    CHECK((mapRoadClass("alley", c) && c == MapRoadClass::Lane));
    CHECK((mapRoadClass("link_stair", c) && c == MapRoadClass::Stair));
    CHECK_FALSE(mapRoadClass("canal", c));
    CHECK_FALSE(mapRoadClass("door", c));
    CHECK(std::string(mapRoadClassName(MapRoadClass::Lane)) == "lane");
    CHECK(mapIsBuildingKind("kit"));
    CHECK_FALSE(mapIsBuildingKind("wall"));
}

TEST_CASE("minimap: builds from the tables in engine metres") {
    Fixture f;
    MiniMapModel m;
    std::string err;
    REQUIRE_MESSAGE(buildMiniMap(f.src, m, &err), err);
    // Plan (900, 460) is the north-west corner: engine (400, 40); plan (940, 420) the south-east: (440, 80).
    CHECK(m.minX == doctest::Approx(400.f));
    CHECK(m.minY == doctest::Approx(40.f));
    CHECK(m.maxX == doctest::Approx(440.f));
    CHECK(m.maxY == doctest::Approx(80.f));
    REQUIRE(m.roads.size() == 2); // the quay edge kind is left out, the canal is water
    CHECK(m.roads[0].id == "RD-1");
    CHECK(m.roads[0].cls == MapRoadClass::Main);
    CHECK(m.roads[1].cls == MapRoadClass::Lane);
    // Plan (910, 440) at side 1000 is engine (410, 60).
    CHECK(closeTo(m.points[m.roads[0].first], Vec2{405.f, 60.f}));
    REQUIRE(m.waterRects.size() == 1);
    CHECK(m.waterRects[0].cx == doctest::Approx(437.5f));
    CHECK(m.waterRects[0].hy == doctest::Approx(15.f));
    // The tall house and the landmark; not the kerb (too low) or the wall (not a building kind).
    REQUIRE(m.buildings.size() == 2);
    CHECK(m.buildings[0].rot == doctest::Approx(-30.f * 3.14159265f / 180.f));
    REQUIRE(m.hasLandmark);
    CHECK(closeTo(m.landmark, Vec2{426.f, 48.f}));
    REQUIRE(m.doors.size() == 1);
    CHECK(closeTo(m.doors[0], Vec2{413.f, 55.f}, 1e-3f));
    // Sea east of plan x 930 (engine 430), dry ground west of it.
    REQUIRE(m.waterW == 40);
    CHECK(m.water[20 * 40 + 35] == 1);
    CHECK(m.water[20 * 40 + 20] == 0);
}

TEST_CASE("minimap: a bad table fails with a reason") {
    Fixture f;
    MiniMapModel m;
    std::string err;
    f.src.roads = "id,kind,width_m,pts\nRD-1,road,wide,1:1;2:2\n";
    CHECK_FALSE(buildMiniMap(f.src, m, &err));
    CHECK(err.find("RD-1") != std::string::npos);
    // A blank width takes the class's.
    f.src.roads = "id,kind,width_m,pts\nRD-1,road,,901:430;930:430\n";
    REQUIRE_MESSAGE(buildMiniMap(f.src, m, &err), err);
    CHECK(m.roads[0].width == doctest::Approx(10.f));
    f.src.roads = "id,kind,pts\nRD-1,road,1:1;2:2\n";
    CHECK_FALSE(buildMiniMap(f.src, m, &err));
    // No area and no roads or streets to take one from.
    f.src.roads = "";
    f.src.streets = "";
    f.src.areaX1 = f.src.areaX0;
    CHECK_FALSE(buildMiniMap(f.src, m, &err));
    f.src.side = 0.0;
    CHECK_FALSE(buildMiniMap(f.src, m, &err));
}

TEST_CASE("minimap: no area given covers every road and street; left-out kinds are named") {
    Fixture f;
    f.src.areaX0 = f.src.areaX1 = 0.0;
    f.src.streets = "id,kind,width_m,pts\n"
                    "LN-1,alley,3,920:440;920:458\n"
                    "XX-1,quay_edge,3,901:421;902:422\n"
                    "XX-2,quay_edge,3,901:421;902:422\n"
                    "XX-3,tunnel,3,901:421;902:422\n";
    f.src.towers = "id,kind,cx,cy,z0,sx,sy,rot,floors,height_m,note\n"
                   "T-1,landmark,926,452,0,4,4,0,0,80,placed\n"
                   "T-2,glass,100,100,0,30,30,0,40,140,far off\n"
                   "T-3,glass,920,450,0,10,10,0,40,140,near\n";
    MiniMapModel m;
    std::string err, skipped = "stale";
    REQUIRE_MESSAGE(buildMiniMap(f.src, m, &err, &skipped), err);
    CHECK(skipped == "quay_edge x2, tunnel x1");
    // Road RD-1 runs plan x 905..935 on y 440 and the lane y 440..458 at x 920: engine x 405..435, y 42..60, padded.
    CHECK(m.minX == doctest::Approx(405.f - kMapAutoAreaPad));
    CHECK(m.maxX == doctest::Approx(435.f + kMapAutoAreaPad));
    CHECK(m.minY == doctest::Approx(42.f - kMapAutoAreaPad));
    CHECK(m.maxY == doctest::Approx(60.f + kMapAutoAreaPad));
    REQUIRE(m.roads.size() == 2);
    CHECK(m.roads[1].cls == MapRoadClass::Lane);
    // The house, the landmark and the near tower; the far tower is off the map.
    CHECK(m.buildings.size() == 3);
    CHECK(m.hasLandmark);
    // Nothing left out: the list comes back empty.
    f.src.streets = "id,kind,width_m,pts\nLN-1,lane,3,920:440;920:458\n";
    REQUIRE(buildMiniMap(f.src, m, &err, &skipped));
    CHECK(skipped.empty());
}

TEST_CASE("minimap: the street under a point, main roads first, held at a junction") {
    Fixture f;
    MiniMapModel m;
    REQUIRE(buildMiniMap(f.src, m));
    CHECK(mapRoadAt(m, {410.f, 60.f}) == 0);  // on the main road
    CHECK(mapRoadAt(m, {420.f, 50.f}) == 1);  // up the lane
    CHECK(mapRoadAt(m, {420.f, 60.f}) == 0);  // the junction: the main road wins
    CHECK(mapRoadAt(m, {405.f, 45.f}) == -1); // between
    CHECK(mapRoadAt(m, {-5.f, 60.f}) == -1);  // off the map
    // Walking down the lane into the junction keeps the lane while within its half width plus the hold.
    CHECK(mapRoadAtSticky(m, {420.f, 60.f}, 1) == 1);
    CHECK(mapRoadAtSticky(m, {424.f, 60.f}, 1) == 0); // 4 m off the lane's line: past 1.5 + 2
    CHECK(mapRoadAtSticky(m, {420.f, 60.f}, -1) == 0);
    CHECK(mapRoadDistance(m, 1, {423.f, 50.f}) == doctest::Approx(3.f));
}

TEST_CASE("minimap: the view turns the heading up, north follows") {
    MapView v;
    v.centre = {100.f, 100.f};
    v.radius = 100.f;
    v.yawDeg = 0.f; // facing east
    CHECK(closeTo(mapToView(v, {150.f, 100.f}), Vec2{0.f, -0.5f})); // ahead is up
    CHECK(closeTo(mapNorth(v), Vec2{-1.f, 0.f}));                   // north at nine o'clock
    CHECK(closeTo(mapHeadingDir(v, 0.f), Vec2{0.f, -1.f}));
    v.yawDeg = -90.f; // facing north
    CHECK(closeTo(mapNorth(v), Vec2{0.f, -1.f}));
    CHECK(closeTo(mapToView(v, {150.f, 100.f}), Vec2{0.5f, 0.f})); // east on the right
    v.yawDeg = 90.f; // facing south
    CHECK(closeTo(mapNorth(v), Vec2{0.f, 1.f}));
    v.yawDeg = 37.f;
    const Vec2 w{163.f, 41.f};
    CHECK(closeTo(mapFromView(v, mapToView(v, w)), w, 1e-3f));
    v.rotate = false;
    CHECK(closeTo(mapNorth(v), Vec2{0.f, -1.f}));
    CHECK(closeTo(mapToView(v, {100.f, 50.f}), Vec2{0.f, -0.5f}));
    CHECK(closeTo(mapHeadingDir(v, 0.f), Vec2{1.f, 0.f}));
}

TEST_CASE("minimap: off-map markers pin to the rim with their distance") {
    MapView v;
    v.centre = {0.f, 0.f};
    v.radius = 120.f;
    v.yawDeg = -90.f;
    const MapPin in = mapPin(v, {60.f, 0.f});
    CHECK_FALSE(in.pinned);
    CHECK(closeTo(in.at, Vec2{0.5f, 0.f}));
    CHECK(in.distance == doctest::Approx(60.f));
    const MapPin out = mapPin(v, {0.f, -400.f}); // 400 m north
    CHECK(out.pinned);
    CHECK(closeTo(out.at, Vec2{0.f, -0.9f}));
    CHECK(out.distance == doctest::Approx(400.f));
}

TEST_CASE("minimap: zoom and what shows at each") {
    Fixture f;
    MiniMapModel m;
    REQUIRE(buildMiniMap(f.src, m));
    CHECK(mapZoomRadius(MapMode::OnFoot, m) == doctest::Approx(120.f));
    CHECK(mapZoomRadius(MapMode::InVan, m) == doctest::Approx(250.f));
    CHECK(mapZoomRadius(MapMode::Overlay, m) == doctest::Approx(20.f));
    CHECK(mapLayers(MapMode::OnFoot).doors);
    CHECK_FALSE(mapLayers(MapMode::InVan).doors);
    CHECK_FALSE(mapLayers(MapMode::Overlay).doors);
    CHECK_FALSE(mapLayers(MapMode::InVan).stairs);
    CHECK(mapRotates(MapMode::InVan));
    CHECK_FALSE(mapRotates(MapMode::Overlay));
}

TEST_CASE("minimap: the blob round-trips, a cut blob is refused") {
    Fixture f;
    MiniMapModel m;
    REQUIRE(buildMiniMap(f.src, m));
    std::vector<std::uint8_t> blob;
    saveMiniMap(m, blob);
    MiniMapModel back;
    std::string err;
    REQUIRE_MESSAGE(loadMiniMap(blob.data(), blob.size(), back, &err), err);
    CHECK(back.roads.size() == m.roads.size());
    CHECK(back.roads[1].id == "LN-1");
    CHECK(back.buildings.size() == m.buildings.size());
    CHECK(back.water == m.water);
    CHECK(back.doors.size() == 1);
    CHECK(back.hasLandmark);
    CHECK(back.grid == m.grid); // rebuilt on load
    for (std::size_t cut : {std::size_t{0}, std::size_t{7}, blob.size() / 2, blob.size() - 1})
        CHECK_FALSE(loadMiniMap(blob.data(), cut, back, &err));
    std::vector<std::uint8_t> bad = blob;
    bad[4] = 99; // version
    CHECK_FALSE(loadMiniMap(bad.data(), bad.size(), back, &err));
}

TEST_CASE("minimap: drawn once, water, blocks and roads in their colours") {
    Fixture f;
    MiniMapModel m;
    REQUIRE(buildMiniMap(f.src, m));
    MapImage img;
    rasterMiniMap(m, 0.5f, img);
    CHECK(img.w == 80);
    CHECK(img.h == 80);
    auto at = [&](float x, float y) {
        const Vec2 uv = mapImageUv(img, {x, y});
        return img.px[static_cast<std::size_t>(uv.y * static_cast<float>(img.h)) * img.w +
                      static_cast<std::size_t>(uv.x * static_cast<float>(img.w))];
    };
    CHECK(at(410.2f, 60.2f) == kMapRoadColor[0]);
    CHECK(at(420.2f, 50.2f) == kMapRoadColor[2]);
    CHECK(at(434.2f, 45.2f) == kMapWater);
    CHECK(at(410.2f, 50.2f) == kMapBuilding); // the tall house at plan (910, 450)
    CHECK(at(402.2f, 75.2f) == kMapGround);
    // The edge ring is ground, so the clamped texture does not smear the sea (or a road) past the area.
    CHECK(img.px[20 * 80 + 78] == kMapWater);
    CHECK(img.px[20 * 80 + 79] == kMapGround);
    // Capped to maxSide: the scale grows to fit.
    rasterMiniMap(m, 0.01f, img, 64);
    CHECK(img.w <= 64);
    CHECK(img.mPerPx == doctest::Approx(40.f / 64.f));
    // Power-of-two sides: 40 m at 0.3 m/px is 134 px; the scale stays, the image grows to 256 px and the area fills
    // its north-west 134 px.
    rasterMiniMap(m, 0.3f, img, 4096, true);
    CHECK(img.w == 256);
    CHECK(img.h == 256);
    CHECK(img.mPerPx == doctest::Approx(0.3f));
    CHECK(img.px[255 * 256 + 255] == kMapGround);
    const Vec2 uv = mapImageUv(img, {440.f, 80.f});
    CHECK(uv.x == doctest::Approx(40.f / (256.f * 0.3f)));
}

TEST_CASE("minimap: the per-frame queries allocate nothing") {
    Fixture f;
    MiniMapModel m;
    REQUIRE(buildMiniMap(f.src, m));
    MapImage img;
    rasterMiniMap(m, 1.f, img);
    const bool hooked = ftcore_test::allocHookInstalled();
    const std::uint64_t before = ftcore_test::allocStats().count;
    MapView v;
    v.centre = {415.f, 60.f};
    v.radius = mapZoomRadius(MapMode::OnFoot, m);
    float sum = 0.f;
    int road = -1;
    for (int i = 0; i < 200; ++i) {
        v.yawDeg = static_cast<float>(i);
        const Vec2 p{400.f + static_cast<float>(i % 40), 60.f};
        road = mapRoadAtSticky(m, p, road);
        const MapPin pin = mapPin(v, m.landmark);
        const Vec2 uv = mapImageUv(img, mapFromView(v, mapNorth(v)));
        sum += pin.at.x + uv.x + mapHeadingDir(v, 10.f).y + static_cast<float>(mapLayers(MapMode::OnFoot).doors);
    }
    CHECK(std::isfinite(sum));
    if (hooked) CHECK(ftcore_test::allocStats().count == before);
}
