#include "ftcore/MapLook.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

// A model of w x h m from (0, 0) with a 4 m water grid set by wet(x, y) at each cell's centre. No roads.
MiniMapModel waterModel(float w, float h, const std::function<bool(float, float)>& wet) {
    MiniMapModel m;
    m.maxX = w;
    m.maxY = h;
    m.waterCell = 4.f;
    m.waterW = static_cast<std::uint32_t>(std::ceil(w / 4.f));
    m.waterH = static_cast<std::uint32_t>(std::ceil(h / 4.f));
    m.water.assign(static_cast<std::size_t>(m.waterW) * m.waterH, 0);
    for (std::uint32_t y = 0; y < m.waterH; ++y)
        for (std::uint32_t x = 0; x < m.waterW; ++x)
            m.water[static_cast<std::size_t>(y) * m.waterW + x] = wet(static_cast<float>(x) * 4.f + 2.f, static_cast<float>(y) * 4.f + 2.f) ? 1 : 0;
    return m;
}

// The strip: 2000 x 1200 m, land west of x 1500, the sea east of it (and on past the model's east edge).
MiniMapModel strip() {
    return waterModel(2000.f, 1200.f, [](float x, float) { return x >= 1500.f; });
}

float centreDist(const MapOverlayView& v, Vec2 p, float& sx, float& sy) {
    const Vec2 s = mapOverlayToScreen(v, p);
    sx = s.x;
    sy = s.y;
    return std::hypot(s.x - v.screenW * 0.5f, s.y - v.screenH * 0.5f);
}

} // namespace

TEST_CASE("map look: the sea is the water that reaches the sea-side edge; land bounds and share count it alone") {
    // A canal-like pond inland (x 200..240) is water but not sea: it counts as land.
    const MiniMapModel m = waterModel(2000.f, 1200.f, [](float x, float y) { return x >= 1500.f || (x >= 200.f && x < 240.f && y >= 400.f && y < 800.f); });
    MapLook look;
    mapBuildLook(m, nullptr, look);
    CHECK(look.seaSide == 1);
    CHECK(mapIsSea(m, look, {1700.f, 600.f}));
    CHECK_FALSE(mapIsSea(m, look, {220.f, 600.f})); // the pond
    CHECK_FALSE(mapIsSea(m, look, {1000.f, 600.f}));
    CHECK(mapIsSea(m, look, {2500.f, 600.f}));       // off the model, sea side
    CHECK_FALSE(mapIsSea(m, look, {-500.f, 600.f})); // off the model, the other side: land
    CHECK(look.landX0 == doctest::Approx(0.f));
    CHECK(look.landX1 == doctest::Approx(1500.f));
    CHECK(look.landY0 == doctest::Approx(0.f));
    CHECK(look.landY1 == doctest::Approx(1200.f));
    // The pond's edge is drawn as a canal's, never as coast.
    bool pondEdge = false;
    mapEachStroke(look, MapStrokeKind::Canal, {190.f, 390.f}, {250.f, 810.f}, [&](const MapStroke& s) {
        if (std::abs(s.a.x - 200.f) < 1.f || std::abs(s.a.x - 240.f) < 1.f) pondEdge = true;
    });
    CHECK(pondEdge);
    bool coastInland = false;
    mapEachStroke(look, MapStrokeKind::Coast, {0.f, 0.f}, {1000.f, 1200.f}, [&](const MapStroke& s) { coastInland = coastInland || s.a.x < 1400.f; });
    CHECK_FALSE(coastInland);
}

TEST_CASE("map look: the land share is the model's own ground; the sea and the ground past the model's edges are not land") {
    const MiniMapModel m = strip();
    MapLook look;
    mapBuildLook(m, nullptr, look);
    // Near (0.6 m/px at 1080p: 1152 x 648 m) centred on the east edge: 500 m of model sea and 576 m off the model.
    MapOverlayView v = mapOverlayView({2000.f, 600.f}, MapZoom::Near, 1920.f, 1080.f);
    CHECK(mapLandShare(m, look, v) == doctest::Approx(76.f / 1152.f).epsilon(1e-3));
    // On the west edge: half the frame off the model, where nothing is mapped: not land.
    v.centre = Vec2{0.f, 600.f};
    CHECK(mapLandShare(m, look, v) == doctest::Approx(0.5f).epsilon(1e-4));
    // Wholly off the model: no land at all.
    v.centre = Vec2{-2000.f, 600.f};
    CHECK(mapLandShare(m, look, v) == doctest::Approx(0.f));
    // A share is exact for a part cell: the frame's edge 2 m into the first sea cell column.
    v.centre = Vec2{1502.f - 576.f, 600.f};
    CHECK(mapLandShare(m, look, v) == doctest::Approx(1.f - 2.f / 1152.f).epsilon(1e-4));
    // North and south of the model the edge rows run on as sea and ground, but neither is land: at the north-east corner
    // (frame x 924..2076, y -324..324) the land is the model's ground alone, x 924..1500, y 0..324.
    CHECK(mapIsSea(m, look, {1700.f, -50.f}));
    CHECK_FALSE(mapIsSea(m, look, {1000.f, -50.f}));
    CHECK(mapIsSea(m, look, {1700.f, 1250.f}));
    CHECK_FALSE(mapIsSea(m, look, {1000.f, 1250.f}));
    v.centre = Vec2{1500.f, 0.f};
    CHECK(mapLandShare(m, look, v) == doctest::Approx(0.25f).epsilon(1e-4));
    v.centre = Vec2{1500.f, 1200.f};
    CHECK(mapLandShare(m, look, v) == doctest::Approx(0.25f).epsilon(1e-4));
}

TEST_CASE("map look: a dry margin of up to four rows at the south or north edge takes the sea; a deeper dry band is a shore") {
    // The sea east of x 1500 stops 12 m short of the south edge (three dry rows, the bake's margin past its height field).
    const MiniMapModel m = waterModel(2000.f, 1200.f, [](float x, float y) { return x >= 1500.f && y < 1188.f; });
    MapLook look;
    mapBuildLook(m, nullptr, look);
    CHECK(mapIsSea(m, look, {1700.f, 1195.f}));
    CHECK(mapIsSea(m, look, {1700.f, 1250.f})); // and on past the edge
    CHECK_FALSE(mapIsSea(m, look, {1000.f, 1195.f}));
    bool edgeCoast = false;
    mapEachStroke(look, MapStrokeKind::Coast, {0.f, 1100.f}, {2000.f, 1200.f}, [&](const MapStroke& s) {
        edgeCoast = edgeCoast || (s.a.x > 1510.f && s.a.y > 1150.f && std::abs(s.b.y - s.a.y) < 1.f);
    });
    CHECK_FALSE(edgeCoast);
    // The same at the north edge.
    const MiniMapModel north = waterModel(2000.f, 1200.f, [](float x, float y) { return x >= 1500.f && y >= 8.f; });
    MapLook nl;
    mapBuildLook(north, nullptr, nl);
    CHECK(mapIsSea(north, nl, {1700.f, 2.f}));
    CHECK(mapIsSea(north, nl, {1700.f, -50.f}));
    // Forty metres of dry ground (ten rows) is a shore: it stays land, its coast drawn, and the land runs on past the edge.
    const MiniMapModel shore = waterModel(2000.f, 1200.f, [](float x, float y) { return x >= 1500.f && y < 1160.f; });
    MapLook sl;
    mapBuildLook(shore, nullptr, sl);
    CHECK_FALSE(mapIsSea(shore, sl, {1700.f, 1180.f}));
    CHECK_FALSE(mapIsSea(shore, sl, {1700.f, 1250.f}));
    bool shoreCoast = false;
    mapEachStroke(sl, MapStrokeKind::Coast, {1500.f, 1100.f}, {2000.f, 1200.f}, [&](const MapStroke& s) {
        shoreCoast = shoreCoast || (s.a.x > 1510.f && std::abs(s.a.y - 1160.f) < 3.f);
    });
    CHECK(shoreCoast);
}

TEST_CASE("map look: a coast-side player is framed with land at three quarters, by the least shift, still on screen") {
    const MiniMapModel m = strip();
    MapLook look;
    mapBuildLook(m, nullptr, look);
    const Vec2 player{1480.f, 600.f}; // 20 m off the coast
    const MapFrame f = mapOverlayFrame(m, look, player, MapZoom::Near, 1920.f, 1080.f);
    CHECK(f.reached);
    CHECK(f.land >= kMapLandShareMin);
    CHECK(f.land == doctest::Approx(kMapLandShareMin).epsilon(2e-3)); // the least shift: just enough
    // 1152 m wide: the sea (east of 1500) may take 288 m, so the frame's east edge sits at 1788, its centre at 1212.
    CHECK(f.view.centre.x == doctest::Approx(1212.f).epsilon(1e-3));
    CHECK(f.view.centre.y == doctest::Approx(600.f));
    float sx = 0.f, sy = 0.f;
    centreDist(f.view, player, sx, sy);
    CHECK(sx >= 0.1f * 1920.f);
    CHECK(sx <= 0.9f * 1920.f);
    CHECK(sy >= 0.1f * 1080.f);
    CHECK(sy <= 0.9f * 1080.f);
    // Mid (1.4 m/px: 2688 m) is wider than the model: centred on it, then shifted west as far as the player allows.
    const MapFrame mid = mapOverlayFrame(m, look, player, MapZoom::Mid, 1920.f, 1080.f);
    centreDist(mid.view, player, sx, sy);
    CHECK(sx <= 0.9f * 1920.f + 0.5f);
    CHECK(sx >= 0.1f * 1920.f - 0.5f);
}

TEST_CASE("map look: a pan is held to the model, widened to take in the framed centre, which is never pulled back") {
    const MiniMapModel m = strip(); // 2000 x 1200 m
    MapLook look;
    mapBuildLook(m, nullptr, look);
    // Near (0.6 m/px: 1152 x 648 m): the centre's room is x 576..1424, y 324..876.
    MapOverlayView v = mapOverlayView({1000.f, 600.f}, MapZoom::Near, 1920.f, 1080.f);
    const Vec2 inside{1000.f, 600.f};
    v.centre = {1000.f, 600.f};
    CHECK(mapOverlayHold(m, v, inside).x == doctest::Approx(1000.f));
    v.centre = {1900.f, 100.f};
    Vec2 c = mapOverlayHold(m, v, inside);
    CHECK(c.x == doctest::Approx(1424.f));
    CHECK(c.y == doctest::Approx(324.f));
    // Mid (1.4 m/px: 2688 x 1512 m) is wider and taller than the model: the room is its middle alone, widened to a
    // framed centre shifted west to x 700, and no further.
    v = mapOverlayView({1000.f, 600.f}, MapZoom::Mid, 1920.f, 1080.f);
    const Vec2 west{700.f, 600.f};
    v.centre = {400.f, 600.f};
    CHECK(mapOverlayHold(m, v, west).x == doctest::Approx(700.f));
    v.centre = {1200.f, 600.f};
    CHECK(mapOverlayHold(m, v, west).x == doctest::Approx(1000.f));
    v.centre = {850.f, 900.f};
    c = mapOverlayHold(m, v, west);
    CHECK(c.x == doctest::Approx(850.f));
    CHECK(c.y == doctest::Approx(600.f));
    // A real frame off the coast is held where it is, and its land is the share of its own view, at least the minimum.
    const MapFrame f = mapOverlayFrame(m, look, {1480.f, 600.f}, MapZoom::Mid, 1920.f, 1080.f);
    const Vec2 held = mapOverlayHold(m, f.view, f.view.centre);
    CHECK(held.x == f.view.centre.x);
    CHECK(held.y == f.view.centre.y);
    CHECK(mapLandShare(m, look, f.view) == f.land);
    if (f.reached) {
        CHECK(f.land >= kMapLandShareMin);
    }
    const MapFrame n = mapOverlayFrame(m, look, {1480.f, 600.f}, MapZoom::Near, 1920.f, 1080.f);
    REQUIRE(n.reached);
    CHECK(n.land >= kMapLandShareMin);
    CHECK(mapLandShare(m, look, n.view) == n.land);
}

TEST_CASE("map look: an inland player is not moved") {
    const MiniMapModel m = strip();
    MapLook look;
    mapBuildLook(m, nullptr, look);
    const MapFrame f = mapOverlayFrame(m, look, {600.f, 600.f}, MapZoom::Near, 1920.f, 1080.f);
    CHECK(f.reached);
    CHECK(f.land == doctest::Approx(1.f));
    CHECK(f.view.centre.x == doctest::Approx(600.f));
    CHECK(f.view.centre.y == doctest::Approx(600.f));
}

TEST_CASE("map look: when three quarters can't be reached, the frame takes the best share by the least shift") {
    // An island: land only for x 700..1400, y 300..900, one sea all round it (and east past the model). (Water cut off
    // from the sea-side edge would be a lake, which counts as land; so would the ground past the model's west edge, which
    // no frame here reaches.)
    const MiniMapModel m = waterModel(2400.f, 1200.f, [](float x, float y) { return x < 700.f || x >= 1400.f || y < 300.f || y >= 900.f; });
    MapLook look;
    mapBuildLook(m, nullptr, look);
    REQUIRE(look.seaSide == 1);
    CHECK(mapIsSea(m, look, {300.f, 600.f}));
    const MapFrame f = mapOverlayFrame(m, look, {1400.f, 600.f}, MapZoom::Near, 1920.f, 1080.f);
    CHECK_FALSE(f.reached);
    // The whole island (700 x 600 m of 1152 x 648) is the best any frame holding the player can do; it first fits when
    // the frame's west edge passes 700 (centre 1276), found within one scan step (460.8 / 40 m).
    CHECK(f.land == doctest::Approx(700.f * 600.f / (1152.f * 648.f)).epsilon(2e-3));
    CHECK(f.view.centre.x <= 1276.f);
    CHECK(f.view.centre.x >= 1276.f - 460.8f / 40.f - 0.01f);
}

TEST_CASE("map look: Far fits the land and keeps it whole; its share is honest, so no shift reaches three quarters") {
    const MiniMapModel m = strip();
    MapLook look;
    mapBuildLook(m, nullptr, look);
    const MapFrame f = mapOverlayFrame(m, look, {1400.f, 600.f}, MapZoom::Far, 1920.f, 1080.f);
    // The scale fits the land (1500 x 1200 m in 92% of the screen: 1.21 m/px) but is never nearer than 1.25 x Mid.
    CHECK(f.view.mPerPx == doctest::Approx(kMapZoomMPerPx[1] * kMapFarOverMid));
    // 3360 x 1890 m holding all 1500 x 1200 m of land: no frame in the room holds more, so it stays on the player.
    CHECK_FALSE(f.reached);
    CHECK(f.land == doctest::Approx(1500.f * 1200.f / (3360.f * 1890.f)).epsilon(1e-3));
    CHECK(f.view.centre.x == doctest::Approx(1400.f));
    CHECK(f.view.centre.y == doctest::Approx(600.f));
    // Every land cell is in the frame.
    const Vec2 lo = mapOverlayFromScreen(f.view, {0.f, 0.f}), hi = mapOverlayFromScreen(f.view, {1920.f, 1080.f});
    CHECK(lo.x <= look.landX0);
    CHECK(hi.x >= look.landX1);
    CHECK(lo.y <= look.landY0);
    CHECK(hi.y >= look.landY1);
    // Far over a model whose land is wider than the frame: the scale fits the land.
    const MiniMapModel wide = waterModel(8000.f, 600.f, [](float x, float) { return x >= 7000.f; });
    MapLook wl;
    mapBuildLook(wide, nullptr, wl);
    const MapFrame w = mapOverlayFrame(wide, wl, {6900.f, 300.f}, MapZoom::Far, 1920.f, 1080.f);
    CHECK(w.view.mPerPx == doctest::Approx(7000.f / (1920.f * (1.f - 2.f * kMapFarMargin))));
    CHECK_FALSE(w.reached);
    CHECK(w.land == doctest::Approx(7000.f * 600.f / (1920.f * 1080.f * w.view.mPerPx * w.view.mPerPx)).epsilon(1e-3));
}

TEST_CASE("map look: the coast is a smooth line at the water's edge, the waves at 12, 24 and 40 m out") {
    const MiniMapModel m = waterModel(400.f, 300.f, [](float x, float) { return x >= 200.f; });
    MapLook look;
    mapBuildLook(m, nullptr, look);
    const MapStrokeKind kinds[4] = {MapStrokeKind::Coast, MapStrokeKind::Wave12, MapStrokeKind::Wave24, MapStrokeKind::Wave40};
    const float at[4] = {200.f, 212.f, 224.f, 240.f};
    for (int k = 0; k < 4; ++k) {
        float length = 0.f, worst = 0.f;
        std::size_t n = 0;
        mapEachStroke(look, kinds[k], {-100.f, -100.f}, {500.f, 400.f}, [&](const MapStroke& s) {
            ++n;
            length += std::hypot(s.b.x - s.a.x, s.b.y - s.a.y);
            worst = std::max(worst, std::max(std::abs(s.a.x - at[k]), std::abs(s.b.x - at[k])));
            CHECK(s.level == 0);
        });
        CAPTURE(k);
        CHECK(n > 0);
        CHECK(worst < 0.25f);
        CHECK(length == doctest::Approx(296.f).epsilon(0.02)); // cell centre to cell centre, top to bottom
    }
    // A diagonal coast runs as one straight line, not in cell steps.
    const MiniMapModel d = waterModel(400.f, 400.f, [](float x, float y) { return x + y >= 400.f; });
    MapLook dl;
    mapBuildLook(d, nullptr, dl);
    float lo = 1e9f, hi = -1e9f;
    std::size_t n = 0;
    mapEachStroke(dl, MapStrokeKind::Coast, {40.f, 40.f}, {360.f, 360.f}, [&](const MapStroke& s) {
        if (s.a.x < 40.f || s.a.x > 360.f) return;
        ++n;
        lo = std::min(lo, s.a.x + s.a.y);
        hi = std::max(hi, s.a.x + s.a.y);
    });
    CHECK(n > 0);
    CHECK((hi - lo) / std::sqrt(2.f) < 0.5f); // straight: the 4 m cells' stair (2.8 m tread to nose) is gone
    CHECK((hi + lo) * 0.5f == doctest::Approx(398.f).epsilon(2e-3)); // between the last land and first sea centres
}

TEST_CASE("map look: strokes are culled by tile with no allocation, and none is missed") {
    const MiniMapModel m = waterModel(400.f, 300.f, [](float x, float) { return x >= 200.f; });
    MapLook look;
    mapBuildLook(m, nullptr, look);
    std::size_t all = 0;
    for (const MapStroke& s : look.strokes[static_cast<std::size_t>(MapStrokeKind::Coast)])
        if (std::max(s.a.y, s.b.y) >= 100.f && std::min(s.a.y, s.b.y) <= 140.f) ++all;
    const bool hooked = ftcore_test::allocHookInstalled();
    const std::uint64_t before = ftcore_test::allocStats().count;
    std::size_t seen = 0;
    mapEachStroke(look, MapStrokeKind::Coast, {150.f, 100.f}, {250.f, 140.f}, [&](const MapStroke& s) {
        if (std::max(s.a.y, s.b.y) >= 100.f && std::min(s.a.y, s.b.y) <= 140.f) ++seen;
    });
    MapOverlayView v = mapOverlayView({200.f, 150.f}, MapZoom::Near, 1920.f, 1080.f);
    const float share = mapLandShare(m, look, v);
    const MapFrame f = mapOverlayFrame(m, look, {190.f, 150.f}, MapZoom::Mid, 1920.f, 1080.f);
    Vec2 at{};
    const bool sea = mapSeaLabelAt(m, look, v, 120.f, 24.f, at);
    const bool isSea = mapIsSea(m, look, {300.f, 150.f});
    if (hooked) CHECK(ftcore_test::allocStats().count == before);
    CHECK(seen == all);
    CHECK(share < 1.f);
    CHECK(f.land > 0.f);
    CHECK(sea);
    CHECK(isSea);
}

TEST_CASE("map look: the sea's name goes over open sea, wholly on it and on screen, or nowhere") {
    const MiniMapModel m = waterModel(400.f, 300.f, [](float x, float) { return x >= 200.f; });
    MapLook look;
    mapBuildLook(m, nullptr, look);
    const MapOverlayView v{{200.f, 150.f}, 0.25f, 1600.f, 1200.f}; // 400 x 300 m: the whole model
    Vec2 at{};
    REQUIRE(mapSeaLabelAt(m, look, v, 200.f, 30.f, at));
    const float hx = 100.f * 0.25f, hy = 15.f * 0.25f;
    CHECK(mapIsSea(m, look, at));
    CHECK(mapIsSea(m, look, {at.x - hx, at.y - hy}));
    CHECK(mapIsSea(m, look, {at.x - hx, at.y + hy}));
    CHECK(at.x - hx >= 200.f);
    CHECK(at.x + hx <= 400.f);
    // All land on screen: no place for it.
    const MapOverlayView inland{{60.f, 150.f}, 0.25f, 400.f, 400.f};
    CHECK_FALSE(mapSeaLabelAt(m, look, inland, 200.f, 30.f, at));
    // Past the model's sea-side edge, the open sea off the model takes it.
    const MapOverlayView off{{700.f, 150.f}, 0.25f, 1600.f, 1200.f};
    REQUIRE(mapSeaLabelAt(m, look, off, 200.f, 30.f, at));
    CHECK(at.x > 400.f);
}

TEST_CASE("map look: blocks are the empty ground between roads, buildings and water: parks hatched, yards dotted") {
    // A 200 x 200 m model, no water: a ring of main roads (x 20 and 180, y 20 and 180) cut in two by one at x 100.
    // The west block holds a walled court of 8 x 8 m (a sliver); the east block holds a park's point.
    MiniMapModel m;
    m.maxX = 200.f;
    m.maxY = 200.f;
    auto road = [&](Vec2 a, Vec2 b) {
        MapRoad r;
        r.cls = MapRoadClass::Main;
        r.width = 8.f;
        r.first = static_cast<std::uint32_t>(m.points.size());
        r.count = 2;
        m.points.push_back(a);
        m.points.push_back(b);
        m.roads.push_back(r);
    };
    road({20.f, 20.f}, {180.f, 20.f});
    road({20.f, 180.f}, {180.f, 180.f});
    road({20.f, 20.f}, {20.f, 180.f});
    road({180.f, 20.f}, {180.f, 180.f});
    road({100.f, 20.f}, {100.f, 180.f});
    m.buildings.push_back(MapRect{52.f, 100.f, 2.f, 9.f, 0.f});
    m.buildings.push_back(MapRect{68.f, 100.f, 2.f, 9.f, 0.f});
    m.buildings.push_back(MapRect{60.f, 92.5f, 10.f, 1.5f, 0.f});
    m.buildings.push_back(MapRect{60.f, 107.5f, 10.f, 1.5f, 0.f});
    mapBuildGrid(m);
    MapNames n;
    std::string err;
    REQUIRE(mapLoadNames(m, "id,kind,name,sign_form,district,x,y\nP1,park,Kettle Green,,,140,100\nP2,shrine,Pinch Shrine,,,60,140\n", n, &err));
    REQUIRE(n.places.size() == 2);
    CHECK(n.places[0].green);
    CHECK_FALSE(n.places[1].green);
    MapLook look;
    mapBuildLook(m, &n, look);
    CHECK(look.parks == 1);
    CHECK(look.yards == 1);
    CHECK(look.slivers == 1);
    CHECK(look.open == 1); // the ground outside the ring reaches the edge
    // The hatch lies in the east block only, on the lines x + y = 6k.
    std::size_t hatch = 0;
    mapEachStroke(look, MapStrokeKind::ParkHatch, {0.f, 0.f}, {200.f, 200.f}, [&](const MapStroke& s) {
        ++hatch;
        for (const Vec2 p : {s.a, s.b}) {
            CHECK(p.x >= 104.f);
            CHECK(p.x <= 176.f);
            CHECK(p.y >= 24.f);
            CHECK(p.y <= 176.f);
        }
        const float k = (s.a.x + s.a.y) / kMapHatchStepM;
        CHECK(std::abs(k - std::round(k)) < 1e-3f);
        CHECK(std::abs((s.b.x + s.b.y) - (s.a.x + s.a.y)) < 1e-2f);
        CHECK(std::hypot(s.b.x - s.a.x, s.b.y - s.a.y) <= kMapStrokeMaxM + 1e-3f);
    });
    CHECK(hatch > 0);
    // At Far only every 4th line; at Mid every 2nd.
    std::size_t atFar = 0, atMid = 0, atNear = 0;
    mapEachStroke(look, MapStrokeKind::ParkHatch, {0.f, 0.f}, {200.f, 200.f}, [&](const MapStroke& s) {
        atFar += mapStrokeShown(s, MapZoom::Far) ? 1 : 0;
        atMid += mapStrokeShown(s, MapZoom::Mid) ? 1 : 0;
        atNear += mapStrokeShown(s, MapZoom::Near) ? 1 : 0;
    });
    CHECK(atFar > 0);
    CHECK(atFar < atMid);
    CHECK(atMid < atNear);
    // The yard's dots: points, in the west block, none at Far.
    std::size_t dots = 0;
    mapEachStroke(look, MapStrokeKind::YardDot, {0.f, 0.f}, {200.f, 200.f}, [&](const MapStroke& s) {
        ++dots;
        CHECK(s.a.x == s.b.x);
        CHECK(s.a.y == s.b.y);
        CHECK(s.a.x > 24.f);
        CHECK(s.a.x < 96.f);
        CHECK_FALSE(mapStrokeShown(s, MapZoom::Far));
    });
    CHECK(dots > 0);
    // No water grid: the model is all land (the ground past it none); no coast.
    CHECK(look.strokes[static_cast<std::size_t>(MapStrokeKind::Coast)].empty());
    CHECK(look.edgeCoast.empty());
    CHECK(mapLandShare(m, look, mapOverlayView({100.f, 100.f}, MapZoom::Near, 1920.f, 1080.f)) ==
          doctest::Approx(200.f * 200.f / (1152.f * 648.f)).epsilon(1e-4));
}

TEST_CASE("map look: ground that opens onto the sea is shore, not a block, even with a park's point on it") {
    // 200 x 200 m, the sea from x 160 (and past the east edge). Main roads at y 20 and 180 run into the sea, one at x 20
    // and one at x 100: the west block is a yard; the east one, between the road at x 100 and the sea, is a quay.
    MiniMapModel m = waterModel(200.f, 200.f, [](float x, float) { return x >= 160.f; });
    auto road = [&](Vec2 a, Vec2 b) {
        MapRoad r;
        r.cls = MapRoadClass::Main;
        r.width = 8.f;
        r.first = static_cast<std::uint32_t>(m.points.size());
        r.count = 2;
        m.points.push_back(a);
        m.points.push_back(b);
        m.roads.push_back(r);
    };
    road({20.f, 20.f}, {165.f, 20.f});
    road({20.f, 180.f}, {165.f, 180.f});
    road({20.f, 20.f}, {20.f, 180.f});
    road({100.f, 20.f}, {100.f, 180.f});
    mapBuildGrid(m);
    MapNames n;
    std::string err;
    REQUIRE(mapLoadNames(m, "id,kind,name,sign_form,district,x,y\nQ1,square,Fixture Quay Square,,,130,100\n", n, &err));
    REQUIRE(n.places.size() == 1);
    CHECK(n.places[0].green);
    MapLook look;
    mapBuildLook(m, &n, look);
    REQUIRE(look.seaSide == 1);
    CHECK(look.parks == 0);
    CHECK(look.yards == 1);
    CHECK(look.open == 2); // the ground outside the roads (it reaches the edge) and the quay (it reaches the sea)
    CHECK(look.strokes[static_cast<std::size_t>(MapStrokeKind::ParkHatch)].empty());
    std::size_t dots = 0;
    mapEachStroke(look, MapStrokeKind::YardDot, {0.f, 0.f}, {200.f, 200.f}, [&](const MapStroke& s) {
        ++dots;
        CHECK(s.a.x < 96.f); // the west block's only
    });
    CHECK(dots > 0);
}

TEST_CASE("map look: a canal rectangle is edged on its long sides, and open where another canal meets it") {
    MiniMapModel m;
    m.maxX = 300.f;
    m.maxY = 300.f;
    m.waterRects.push_back(MapRect{150.f, 150.f, 100.f, 5.f, 0.f}); // east-west, x 50..250
    m.waterRects.push_back(MapRect{150.f, 100.f, 5.f, 50.f, 0.f});  // north-south, y 50..150, meeting it at the middle
    MapLook look;
    mapBuildLook(m, nullptr, look);
    bool northGap = true;
    float southLen = 0.f;
    mapEachStroke(look, MapStrokeKind::Canal, {0.f, 0.f}, {300.f, 300.f}, [&](const MapStroke& s) {
        if (std::abs(s.a.y - 145.f) < 1e-3f && std::abs(s.b.y - 145.f) < 1e-3f) { // the east-west canal's north side
            const float lo = std::min(s.a.x, s.b.x), hi = std::max(s.a.x, s.b.x);
            if (hi > 146.f && lo < 154.f) northGap = false; // must stay open where the north-south canal joins
        }
        if (std::abs(s.a.y - 155.f) < 1e-3f && std::abs(s.b.y - 155.f) < 1e-3f) southLen += std::abs(s.b.x - s.a.x);
        CHECK(std::hypot(s.b.x - s.a.x, s.b.y - s.a.y) <= kMapStrokeMaxM + 1e-3f);
    });
    CHECK(northGap);
    CHECK(southLen == doctest::Approx(200.f));
}

// ---- Edges found in turns 8 and 9: a second build into one look, no roads, no water grid, an empty model ----

namespace {

// The strip with a ring of streets inland, buildings in its west half and a park's point in its east half.
MiniMapModel stripWithRing(MapNames& n) {
    MiniMapModel m = strip();
    auto road = [&](Vec2 a, Vec2 b) {
        MapRoad r;
        r.cls = MapRoadClass::Main;
        r.width = 8.f;
        r.first = static_cast<std::uint32_t>(m.points.size());
        r.count = 2;
        m.points.push_back(a);
        m.points.push_back(b);
        m.roads.push_back(r);
    };
    road({1220.f, 420.f}, {1380.f, 420.f});
    road({1220.f, 580.f}, {1380.f, 580.f});
    road({1220.f, 420.f}, {1220.f, 580.f});
    road({1380.f, 420.f}, {1380.f, 580.f});
    road({1300.f, 420.f}, {1300.f, 580.f});
    m.buildings.push_back(MapRect{1252.f, 500.f, 2.f, 9.f, 0.f});
    m.buildings.push_back(MapRect{1268.f, 500.f, 2.f, 9.f, 0.f});
    mapBuildGrid(m);
    std::string err;
    REQUIRE_MESSAGE(mapLoadNames(m, "id,kind,name,sign_form,district,x,y\nP1,park,Kettle Green,,,1340,500\n", n, &err), err);
    return m;
}

void checkSameLook(const MapLook& a, const MapLook& b) {
    CHECK(a.sea == b.sea);
    CHECK(a.seaSum == b.seaSum);
    CHECK(a.cell == b.cell);
    CHECK(a.x0 == b.x0);
    CHECK(a.y0 == b.y0);
    CHECK(a.w == b.w);
    CHECK(a.h == b.h);
    CHECK(a.seaSide == b.seaSide);
    CHECK(a.landX0 == b.landX0);
    CHECK(a.landY0 == b.landY0);
    CHECK(a.landX1 == b.landX1);
    CHECK(a.landY1 == b.landY1);
    for (int k = 0; k < kMapStrokeKinds; ++k) {
        CAPTURE(k);
        const std::vector<MapStroke>& sa = a.strokes[static_cast<std::size_t>(k)];
        const std::vector<MapStroke>& sb = b.strokes[static_cast<std::size_t>(k)];
        REQUIRE(sa.size() == sb.size());
        std::size_t differ = 0;
        for (std::size_t i = 0; i < sa.size(); ++i)
            differ += sa[i].a.x != sb[i].a.x || sa[i].a.y != sb[i].a.y || sa[i].b.x != sb[i].b.x || sa[i].b.y != sb[i].b.y ||
                              sa[i].level != sb[i].level
                          ? 1
                          : 0;
        CHECK(differ == 0);
        CHECK(a.tileFirst[static_cast<std::size_t>(k)] == b.tileFirst[static_cast<std::size_t>(k)]);
    }
    REQUIRE(a.edgeCoast.size() == b.edgeCoast.size());
    for (std::size_t i = 0; i < a.edgeCoast.size(); ++i) {
        CHECK(a.edgeCoast[i].a.x == b.edgeCoast[i].a.x);
        CHECK(a.edgeCoast[i].a.y == b.edgeCoast[i].a.y);
        CHECK(a.edgeCoast[i].b.x == b.edgeCoast[i].b.x);
        CHECK(a.edgeCoast[i].b.y == b.edgeCoast[i].b.y);
    }
    CHECK(a.tilesW == b.tilesW);
    CHECK(a.tilesH == b.tilesH);
    CHECK(a.tileX0 == b.tileX0);
    CHECK(a.tileY0 == b.tileY0);
    REQUIRE(a.seaSpots.size() == b.seaSpots.size());
    for (std::size_t i = 0; i < a.seaSpots.size(); ++i) {
        CHECK(a.seaSpots[i].at.x == b.seaSpots[i].at.x);
        CHECK(a.seaSpots[i].at.y == b.seaSpots[i].at.y);
        CHECK(a.seaSpots[i].depth == b.seaSpots[i].depth);
    }
    CHECK(a.parks == b.parks);
    CHECK(a.yards == b.yards);
    CHECK(a.slivers == b.slivers);
    CHECK(a.open == b.open);
}

bool finiteStrokes(const MapLook& look) {
    for (const std::vector<MapStroke>& kind : look.strokes)
        for (const MapStroke& s : kind)
            if (!std::isfinite(s.a.x) || !std::isfinite(s.a.y) || !std::isfinite(s.b.x) || !std::isfinite(s.b.y)) return false;
    return true;
}

} // namespace

TEST_CASE("map look edges: building into a look that held another model gives the same look as a fresh one") {
    MapNames n;
    const MiniMapModel m = stripWithRing(n);
    MapLook fresh;
    mapBuildLook(m, &n, fresh);
    REQUIRE(fresh.parks == 1);
    // A second Init: the same look object first built from another model (a west sea, a different grid), then this one.
    MapLook reused;
    const MiniMapModel west = waterModel(800.f, 600.f, [](float x, float) { return x < 200.f; });
    mapBuildLook(west, nullptr, reused);
    REQUIRE(reused.seaSide == -1);
    mapBuildLook(m, &n, reused);
    checkSameLook(reused, fresh);
    // And again on itself.
    mapBuildLook(m, &n, reused);
    checkSameLook(reused, fresh);
    // The frames from it are the same at every zoom, by the coast and inland.
    for (const Vec2 focus : {Vec2{1450.f, 600.f}, Vec2{600.f, 600.f}})
        for (int z = 0; z < kMapZooms; ++z) {
            CAPTURE(z);
            const MapFrame a = mapOverlayFrame(m, fresh, focus, static_cast<MapZoom>(z), 1920.f, 1080.f);
            const MapFrame b = mapOverlayFrame(m, reused, focus, static_cast<MapZoom>(z), 1920.f, 1080.f);
            CHECK(a.view.centre.x == b.view.centre.x);
            CHECK(a.view.centre.y == b.view.centre.y);
            CHECK(a.view.mPerPx == b.view.mPerPx);
            CHECK(a.land == b.land);
            CHECK(a.reached == b.reached);
        }
}

TEST_CASE("map look edges: no roads and no water grid; an empty model; a one-point road") {
    // No roads, no water grid: no sea, no coast, no waves, no sea name; the model is all land, but every frame is bigger
    // than it and the ground past it is not land, so no zoom reaches three quarters (with no sea, nothing to move from).
    MiniMapModel m;
    m.maxX = 300.f;
    m.maxY = 200.f;
    mapBuildGrid(m);
    MapLook look;
    mapBuildLook(m, nullptr, look);
    CHECK(look.sea.empty());
    CHECK(look.seaSide == 0);
    CHECK(look.seaSpots.empty());
    for (int k = 0; k <= static_cast<int>(MapStrokeKind::Canal); ++k) CHECK(look.strokes[static_cast<std::size_t>(k)].empty());
    CHECK(look.parks == 0);
    CHECK(look.landX0 == doctest::Approx(0.f));
    CHECK(look.landX1 == doctest::Approx(300.f));
    CHECK(look.landY1 == doctest::Approx(200.f));
    for (int z = 0; z < kMapZooms; ++z) {
        CAPTURE(z);
        const MapFrame f = mapOverlayFrame(m, look, Vec2{150.f, 100.f}, static_cast<MapZoom>(z), 1920.f, 1080.f);
        CHECK_FALSE(f.reached);
        CHECK(f.land == doctest::Approx(300.f * 200.f / (1920.f * 1080.f * f.view.mPerPx * f.view.mPerPx)).epsilon(1e-4));
        Vec2 at;
        CHECK_FALSE(mapSeaLabelAt(m, look, f.view, 120.f, 20.f, at));
    }
    // An empty model (a load that found no blob): the look is empty and its frames are finite.
    const MiniMapModel empty{};
    MapLook none;
    mapBuildLook(empty, nullptr, none);
    CHECK(none.seaSpots.empty());
    CHECK(finiteStrokes(none));
    for (int z = 0; z < kMapZooms; ++z) {
        CAPTURE(z);
        const MapFrame f = mapOverlayFrame(empty, none, Vec2{0.f, 0.f}, static_cast<MapZoom>(z), 1920.f, 1080.f);
        CHECK(std::isfinite(f.view.centre.x));
        CHECK(std::isfinite(f.view.centre.y));
        CHECK(std::isfinite(f.view.mPerPx));
        CHECK(f.view.mPerPx > 0.f);
        CHECK(f.land >= 0.f);
        CHECK(f.land <= 1.f);
    }
    // A one-point road among others: its blocks and strokes are finite, and the ring's blocks still come out.
    MapNames n;
    MiniMapModel ring = stripWithRing(n);
    MapRoad dot;
    dot.cls = MapRoadClass::Street;
    dot.width = 6.f;
    dot.first = static_cast<std::uint32_t>(ring.points.size());
    dot.count = 1;
    ring.points.push_back(Vec2{1340.f, 460.f});
    ring.roads.push_back(dot);
    mapBuildGrid(ring);
    MapLook withDot;
    mapBuildLook(ring, &n, withDot);
    CHECK(finiteStrokes(withDot));
    CHECK(withDot.parks == 1);
}

namespace {

// A 600 x 400 m model with no water grid: a canal 9 m wide running north-south (x 195.5..204.5, y 50..250) with a 14 m
// main road along each bank (their edges on the banks), and a basin 148 x 28 m (x 226..374, y 286..314) with a road
// 67 m north of it.
MiniMapModel waterSeatModel() {
    MiniMapModel m;
    m.maxX = 600.f;
    m.maxY = 400.f;
    m.waterRects.push_back(MapRect{200.f, 150.f, 4.5f, 100.f, 0.f});
    m.waterRects.push_back(MapRect{300.f, 300.f, 74.f, 14.f, 0.f});
    auto road = [&](Vec2 a, Vec2 b) {
        MapRoad r;
        r.cls = MapRoadClass::Main;
        r.width = 14.f;
        r.first = static_cast<std::uint32_t>(m.points.size());
        r.count = 2;
        m.points.push_back(a);
        m.points.push_back(b);
        m.roads.push_back(r);
    };
    road({188.5f, 40.f}, {188.5f, 260.f});
    road({211.5f, 40.f}, {211.5f, 260.f});
    road({220.f, 233.f}, {400.f, 233.f});
    return m;
}

// A seated label's box is all water at its corners and centre, and its grown box meets no road.
void checkSeat(const MiniMapModel& m, const MapNames& n, const MapLook& look, const MapOverlayView& v, MapZoom z, const MapLabel& l, float grow) {
    Vec2 k[4];
    mapLabelCorners(l, -0.01f, k);
    for (const Vec2& q : k) {
        const Vec2 w = mapOverlayFromScreen(v, q);
        CHECK((mapIsWater(m, w) || mapIsSea(m, look, w)));
    }
    CHECK(mapIsWater(m, mapOverlayFromScreen(v, l.at)));
    CHECK(mapLabelOnAnyRoad(m, n, v, z, MapRoadInk{}, l, grow) == -1);
}

} // namespace

TEST_CASE("map look: a water name sits on its own water, nearest its point; on a canal too narrow for it, it is left out") {
    const MiniMapModel m = waterSeatModel();
    MapLook look;
    mapBuildLook(m, nullptr, look);
    const MapNames n;
    const MapRoadInk ink;
    const float halo = kMapLabelHaloPx;
    const MapOverlayView nearV = mapOverlayView({300.f, 200.f}, MapZoom::Near, 1920.f, 1080.f); // 0.6 m per px
    REQUIRE(nearV.mPerPx == doctest::Approx(0.6f));
    // The basin's middle: seated right there, level.
    MapLabel l;
    REQUIRE(mapWaterLabelAt(m, n, look, nearV, MapZoom::Near, ink, {300.f, 300.f}, 70.f, 19.f, halo, l) == MapWaterSeat::Seated);
    CHECK(l.at.x == doctest::Approx(mapOverlayToScreen(nearV, {300.f, 300.f}).x));
    CHECK(l.at.y == doctest::Approx(mapOverlayToScreen(nearV, {300.f, 300.f}).y));
    CHECK(l.angleDeg == 0.f);
    checkSeat(m, n, look, nearV, MapZoom::Near, l, halo);
    // Near the basin's west end the box would hang over the bank: it moves east the least (its centre 19.5 m in from
    // x 226, on a 3 m lattice from x 230), and stays on the basin's line.
    REQUIRE(mapWaterLabelAt(m, n, look, nearV, MapZoom::Near, ink, {230.f, 300.f}, 65.f, 19.f, halo, l) == MapWaterSeat::Seated);
    const Vec2 seat = mapOverlayFromScreen(nearV, l.at);
    CHECK(seat.x >= 245.5f);
    CHECK(seat.x <= 248.6f);
    CHECK(seat.y == doctest::Approx(300.f).epsilon(0.001));
    checkSeat(m, n, look, nearV, MapZoom::Near, l, halo);
    // The canal is 15 px wide at Near: a 19 px name fits it neither level nor along it, so it is left out.
    CHECK(mapWaterLabelAt(m, n, look, nearV, MapZoom::Near, ink, {200.f, 150.f}, 60.f, 19.f, halo, l) == MapWaterSeat::NoSeat);
    // Close up (0.2 m per px: 45 px wide) it is seated along the canal, standing, its halo clear of both banks' roads.
    MapOverlayView closeV = nearV;
    closeV.centre = {200.f, 150.f};
    closeV.mPerPx = 0.2f;
    REQUIRE(mapWaterLabelAt(m, n, look, closeV, MapZoom::Near, ink, {200.f, 150.f}, 60.f, 19.f, halo, l) == MapWaterSeat::Seated);
    CHECK(l.angleDeg == -90.f);
    CHECK(l.at.x == doctest::Approx(960.f));
    checkSeat(m, n, look, closeV, MapZoom::Near, l, halo);
    // A name whose point is on land is never set: Dry.
    CHECK(mapWaterLabelAt(m, n, look, nearV, MapZoom::Near, ink, {100.f, 100.f}, 60.f, 19.f, halo, l) == MapWaterSeat::Dry);
    // A lit road through the basin keeps the name off it: the cursor's road drawn over water still counts.
    MiniMapModel lit = m;
    MapRoad bridge;
    bridge.cls = MapRoadClass::Lane; // not drawn at Mid but for the cursor
    bridge.width = 6.f;
    bridge.first = static_cast<std::uint32_t>(lit.points.size());
    bridge.count = 2;
    lit.points.push_back({300.f, 280.f});
    lit.points.push_back({300.f, 320.f});
    lit.roads.push_back(bridge);
    const int b = static_cast<int>(lit.roads.size()) - 1;
    MapOverlayView midV = nearV;
    midV.mPerPx = 0.6f;
    REQUIRE(mapWaterLabelAt(lit, n, look, midV, MapZoom::Mid, ink, {300.f, 300.f}, 70.f, 19.f, halo, l) == MapWaterSeat::Seated);
    CHECK(l.at.x == doctest::Approx(960.f)); // unlit at Mid: not drawn, not in the way
    REQUIRE(mapWaterLabelAt(lit, n, look, midV, MapZoom::Mid, ink, {300.f, 300.f}, 70.f, 19.f, halo, l, b) == MapWaterSeat::Seated);
    CHECK(std::abs(l.at.x - 960.f) >= 35.f + halo); // lit: the name moves off it, its halo clear of the lit line
    CHECK(mapLabelOnAnyRoad(lit, n, midV, MapZoom::Mid, ink, l, halo, b) == -1);
}

TEST_CASE("map look: a water name on the grid's water stays within reach of its point; zero allocation") {
    // A lake of radius 40 m at (200, 200) in a 400 x 400 m grid.
    const MiniMapModel m = waterModel(400.f, 400.f, [](float x, float y) { return std::hypot(x - 200.f, y - 200.f) <= 40.f; });
    MapLook look;
    mapBuildLook(m, nullptr, look);
    const MapNames n;
    const MapRoadInk ink;
    const MapOverlayView v = mapOverlayView({200.f, 200.f}, MapZoom::Mid, 1920.f, 1080.f);
    MapOverlayView one = v;
    one.mPerPx = 1.f;
    MapLabel l;
    REQUIRE(mapWaterLabelAt(m, n, look, one, MapZoom::Mid, ink, {200.f, 200.f}, 60.f, 19.f, kMapLabelHaloPx, l) == MapWaterSeat::Seated);
    CHECK(l.at.x == doctest::Approx(960.f));
    // From the lake's west shore the name moves in to fit (25 m or more: its west end on a wet cell), within the 40 m
    // reach; with 10 m, no seat.
    const bool hooked = ftcore_test::allocHookInstalled();
    const std::uint64_t before = ftcore_test::allocStats().count;
    const MapWaterSeat shore = mapWaterLabelAt(m, n, look, one, MapZoom::Mid, ink, {165.f, 200.f}, 60.f, 19.f, kMapLabelHaloPx, l);
    if (hooked) CHECK(ftcore_test::allocStats().count == before);
    REQUIRE(shore == MapWaterSeat::Seated);
    const Vec2 seat = mapOverlayFromScreen(one, l.at);
    CHECK(seat.x - 165.f >= 25.f);
    CHECK(std::hypot(seat.x - 165.f, seat.y - 200.f) <= kMapWaterReachM);
    checkSeat(m, n, look, one, MapZoom::Mid, l, kMapLabelHaloPx);
    CHECK(mapWaterLabelAt(m, n, look, one, MapZoom::Mid, ink, {165.f, 200.f}, 60.f, 19.f, kMapLabelHaloPx, l, -1, kMapNoName, 10.f) == MapWaterSeat::NoSeat);
}

namespace {

// Whether an edge-coast line stands upright at some x in (x0, x1] across height y.
bool edgeLineBetween(const MapLook& look, float x0, float x1, float y) {
    for (const MapStroke& s : look.edgeCoast)
        if (s.a.x == s.b.x && s.a.x > x0 && s.a.x <= x1 && y >= std::min(s.a.y, s.b.y) && y <= std::max(s.a.y, s.b.y)) return true;
    return false;
}

// The edge coast agrees with mapIsSea past the model: along the lines 60 m north and south of it, and across its west
// and east edges at every metre, sea and ground differ exactly where a line stands; and every line has sea on one side.
void checkEdgeCoast(const MiniMapModel& m, const MapLook& look) {
    for (const float y : {m.minY - 60.f, m.maxY + 60.f}) {
        CAPTURE(y);
        std::size_t wrong = 0;
        for (float x = m.minX - 500.5f; x < m.maxX + 500.f; x += 1.f) {
            const bool differ = mapIsSea(m, look, {x, y}) != mapIsSea(m, look, {x + 1.f, y});
            wrong += differ != edgeLineBetween(look, x, x + 1.f, y) ? 1 : 0;
        }
        CHECK(wrong == 0);
    }
    std::size_t wrongSides = 0;
    for (float y = m.minY + 0.5f; y < m.maxY; y += 1.f) {
        const bool w = mapIsSea(m, look, {m.minX - 0.5f, y}) != mapIsSea(m, look, {m.minX + 0.5f, y});
        const bool e = mapIsSea(m, look, {m.maxX - 0.5f, y}) != mapIsSea(m, look, {m.maxX + 0.5f, y});
        wrongSides += w != edgeLineBetween(look, m.minX - 0.5f, m.minX + 0.5f, y) ? 1 : 0;
        wrongSides += e != edgeLineBetween(look, m.maxX - 0.5f, m.maxX + 0.5f, y) ? 1 : 0;
    }
    CHECK(wrongSides == 0);
    for (const MapStroke& s : look.edgeCoast) {
        CHECK((s.a.x == s.b.x || s.a.y == s.b.y));
        const Vec2 mid{0.5f * (s.a.x + s.b.x), 0.5f * (s.a.y + s.b.y)};
        const Vec2 off = s.a.x == s.b.x ? Vec2{0.5f, 0.f} : Vec2{0.f, 0.5f};
        CAPTURE(mid.x);
        CAPTURE(mid.y);
        CHECK(mapIsSea(m, look, {mid.x - off.x, mid.y - off.y}) != mapIsSea(m, look, {mid.x + off.x, mid.y + off.y}));
    }
}

} // namespace

TEST_CASE("map look: a coast is drawn wherever the sea past the model's edges meets ground, and nowhere else") {
    // The strip: the coast at x 1500 runs on north and south from the edge rows' middles; none along the east edge (sea
    // both sides) or the west (ground both sides).
    const MiniMapModel m = strip();
    MapLook look;
    mapBuildLook(m, nullptr, look);
    REQUIRE(look.edgeCoast.size() == 2);
    for (const MapStroke& s : look.edgeCoast) {
        CHECK(s.a.x == doctest::Approx(1500.f));
        CHECK(s.b.x == doctest::Approx(1500.f));
        CHECK(s.level == 0);
    }
    CHECK(look.edgeCoast[0].a.y == doctest::Approx(2.f));
    CHECK(look.edgeCoast[0].b.y == doctest::Approx(-kMapEdgeRunM));
    CHECK(look.edgeCoast[1].a.y == doctest::Approx(1198.f));
    CHECK(look.edgeCoast[1].b.y == doctest::Approx(1200.f + kMapEdgeRunM));
    checkEdgeCoast(m, look);
    // Land against the east edge (the sea past it): a spit at y 400..600, its line on the edge joined to the contour at
    // both ends; and a corner at the north-east, whose line runs on north from the corner itself.
    const MiniMapModel spit = waterModel(2000.f, 1200.f, [](float x, float y) {
        return x >= 1500.f && !(x >= 1800.f && y >= 400.f && y < 600.f) && !(x >= 1800.f && y < 100.f);
    });
    MapLook sl;
    mapBuildLook(spit, nullptr, sl);
    bool side = false, corner = false, joins = false;
    for (const MapStroke& s : sl.edgeCoast) {
        side = side || (s.a.x == 2000.f && s.b.x == 2000.f && std::abs(s.a.y - 400.f) < 0.01f && std::abs(s.b.y - 600.f) < 0.01f);
        corner = corner || (s.a.x == 2000.f && s.b.x == 2000.f && std::abs(s.a.y) < 0.01f && std::abs(s.b.y + kMapEdgeRunM) < 0.01f);
        joins = joins || (std::abs(s.a.y - 400.f) < 0.01f && std::abs(s.a.x - 1998.f) < 0.01f && s.b.x == 2000.f);
    }
    CHECK(side);
    CHECK(corner);
    CHECK(joins);
    checkEdgeCoast(spit, sl);
    // A sea on the west: its coast runs on at x 200; the ground past the east edge meets no sea.
    const MiniMapModel west = waterModel(800.f, 600.f, [](float x, float) { return x < 200.f; });
    MapLook wl;
    mapBuildLook(west, nullptr, wl);
    REQUIRE(wl.seaSide == -1);
    REQUIRE(wl.edgeCoast.size() == 2);
    checkEdgeCoast(west, wl);
    // No water grid: no sea side, no edge coast.
    MiniMapModel bare;
    bare.maxX = 300.f;
    bare.maxY = 200.f;
    MapLook bl;
    mapBuildLook(bare, nullptr, bl);
    CHECK(bl.edgeCoast.empty());
}

TEST_CASE("map look: roads that run off the model are found by their ends, and fade out there") {
    MiniMapModel m;
    m.maxX = 250.f;
    m.maxY = 150.f;
    auto road = [&](std::initializer_list<Vec2> pts) {
        MapRoad r;
        r.first = static_cast<std::uint32_t>(m.points.size());
        r.count = static_cast<std::uint32_t>(pts.size());
        for (const Vec2 p : pts) m.points.push_back(p);
        m.roads.push_back(r);
    };
    // The roads' bounds: x 0..200, y 10..100.
    road({{0.f, 50.f}, {100.f, 50.f}});    // runs off west at its first end
    road({{100.f, 50.f}, {100.f, 100.f}}); // runs off south at its last end
    road({{100.f, 50.f}, {193.f, 50.f}});  // a dead end 7 m short of the east bound: not off
    road({{200.f, 10.f}, {200.f, 98.f}});  // both ends on the east bound
    road({{0.f, 52.f}});                   // one point on the bound: both ends
    std::vector<std::uint8_t> off;
    mapRoadOffEnds(m, off);
    REQUIRE(off.size() == 5);
    CHECK(off[0] == kMapRoadOffFirst);
    CHECK(off[1] == kMapRoadOffLast);
    CHECK(off[2] == 0);
    CHECK(off[3] == (kMapRoadOffFirst | kMapRoadOffLast));
    CHECK(off[4] == (kMapRoadOffFirst | kMapRoadOffLast));
    // A wider touch takes the dead end too.
    mapRoadOffEnds(m, off, 8.f);
    CHECK(off[2] == kMapRoadOffLast);
    // A road whose points run past the list is skipped, not read.
    MapRoad bad;
    bad.first = static_cast<std::uint32_t>(m.points.size());
    bad.count = 3;
    m.roads.push_back(bad);
    mapRoadOffEnds(m, off);
    REQUIRE(off.size() == 6);
    CHECK(off[5] == 0);
    // The fade: 0 at an off end, straight up to 1 at fadeM, 1 elsewhere.
    CHECK(mapRoadFade(0.f, 100.f, kMapRoadOffFirst, 10.f) == doctest::Approx(0.f));
    CHECK(mapRoadFade(5.f, 100.f, kMapRoadOffFirst, 10.f) == doctest::Approx(0.5f));
    CHECK(mapRoadFade(10.f, 100.f, kMapRoadOffFirst, 10.f) == doctest::Approx(1.f));
    CHECK(mapRoadFade(100.f, 100.f, kMapRoadOffFirst, 10.f) == doctest::Approx(1.f));
    CHECK(mapRoadFade(100.f, 100.f, kMapRoadOffLast, 10.f) == doctest::Approx(0.f));
    CHECK(mapRoadFade(95.f, 100.f, kMapRoadOffLast, 10.f) == doctest::Approx(0.5f));
    CHECK(mapRoadFade(0.f, 100.f, kMapRoadOffLast, 10.f) == doctest::Approx(1.f));
    CHECK(mapRoadFade(5.f, 10.f, kMapRoadOffFirst | kMapRoadOffLast, 10.f) == doctest::Approx(0.5f));
    CHECK(mapRoadFade(0.f, 100.f, 0, 10.f) == doctest::Approx(1.f));
    CHECK(mapRoadFade(0.f, 100.f, kMapRoadOffFirst, 0.f) == doctest::Approx(1.f));
    CHECK(mapRoadFade(-3.f, 100.f, kMapRoadOffFirst, 10.f) == doctest::Approx(0.f));
}

TEST_CASE("map look edges: a screen of no size, or not a number, still gives a finite frame") {
    const MiniMapModel m = strip();
    MapLook look;
    mapBuildLook(m, nullptr, look);
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    struct Case {
        float w, h, ui;
        Vec2 focus;
    };
    const Vec2 p{1400.f, 600.f};
    const Case cases[] = {{0.f, 0.f, 1.f, p},       {-5.f, 100.f, 1.f, p},     {nan, 1080.f, 1.f, p},       {inf, 1080.f, 1.f, p},
                          {1920.f, 1e-40f, 1.f, p}, {1920.f, inf, 1.f, p},     {1920.f, 1080.f, nan, p},    {1920.f, 1080.f, inf, p},
                          {1920.f, 1080.f, 0.f, p}, {1920.f, 1080.f, 1e-30f, p}, {1920.f, 1080.f, 1.f, {nan, 600.f}},
                          {1920.f, 1080.f, 1.f, {1400.f, inf}}};
    std::size_t bad = 0;
    for (const Case& k : cases)
        for (int z = 0; z < kMapZooms; ++z) {
            const MapFrame f = mapOverlayFrame(m, look, k.focus, static_cast<MapZoom>(z), k.w, k.h, k.ui);
            const bool ok = std::isfinite(f.view.centre.x) && std::isfinite(f.view.centre.y) && std::isfinite(f.view.mPerPx) &&
                            f.view.mPerPx > 0.f && std::isfinite(f.view.screenW) && f.view.screenW >= 1.f &&
                            std::isfinite(f.view.screenH) && f.view.screenH >= 1.f && f.land >= 0.f && f.land <= 1.f;
            bad += ok ? 0 : 1;
        }
    CHECK(bad == 0);
    // A screen of no size frames as the default screen does.
    for (int z = 0; z < kMapZooms; ++z) {
        CAPTURE(z);
        const MapFrame a = mapOverlayFrame(m, look, p, static_cast<MapZoom>(z), 0.f, 0.f);
        const MapFrame b = mapOverlayFrame(m, look, p, static_cast<MapZoom>(z), 1920.f, 1080.f);
        CHECK(a.view.centre.x == b.view.centre.x);
        CHECK(a.view.centre.y == b.view.centre.y);
        CHECK(a.view.mPerPx == b.view.mPerPx);
        CHECK(a.land == b.land);
    }
}
