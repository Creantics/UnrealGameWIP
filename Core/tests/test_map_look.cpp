#include "ftcore/MapLook.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cmath>
#include <functional>
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

TEST_CASE("map look: off-model sea counts as sea, the other sides as land, the edge rows run on north and south") {
    const MiniMapModel m = strip();
    MapLook look;
    mapBuildLook(m, nullptr, look);
    // Near (0.6 m/px at 1080p: 1152 x 648 m) centred on the east edge: 500 m of model sea and 576 m off the model.
    MapOverlayView v = mapOverlayView({2000.f, 600.f}, MapZoom::Near, 1920.f, 1080.f);
    CHECK(mapLandShare(m, look, v) == doctest::Approx(76.f / 1152.f).epsilon(1e-3));
    // On the west edge: half the frame off the model, all of it land.
    v.centre = Vec2{0.f, 600.f};
    CHECK(mapLandShare(m, look, v) == doctest::Approx(1.f));
    // A share is exact for a part cell: the frame's edge 2 m into the first sea cell column.
    v.centre = Vec2{1502.f - 576.f, 600.f};
    CHECK(mapLandShare(m, look, v) == doctest::Approx(1.f - 2.f / 1152.f).epsilon(1e-4));
    // North and south of the model the edge rows run on, so the share counts the sea that is drawn there: at the
    // north-east corner (frame x 924..2076, y -324..324) the sea is x 1500..2076 at the frame's full height.
    CHECK(mapIsSea(m, look, {1700.f, -50.f}));
    CHECK_FALSE(mapIsSea(m, look, {1000.f, -50.f}));
    CHECK(mapIsSea(m, look, {1700.f, 1250.f}));
    CHECK_FALSE(mapIsSea(m, look, {1000.f, 1250.f}));
    v.centre = Vec2{1500.f, 0.f};
    CHECK(mapLandShare(m, look, v) == doctest::Approx(576.f / 1152.f).epsilon(1e-4));
    v.centre = Vec2{1500.f, 1200.f};
    CHECK(mapLandShare(m, look, v) == doctest::Approx(576.f / 1152.f).epsilon(1e-4));
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

TEST_CASE("map look: Far fits the land, keeps it whole, and holds the sea to a quarter with the sea on one side") {
    const MiniMapModel m = strip();
    MapLook look;
    mapBuildLook(m, nullptr, look);
    const MapFrame f = mapOverlayFrame(m, look, {1400.f, 600.f}, MapZoom::Far, 1920.f, 1080.f);
    CHECK(f.reached);
    CHECK(f.land >= kMapLandShareMin);
    // The scale fits the land (1500 x 1200 m in 92% of the screen: 1.21 m/px) but is never nearer than 1.25 x Mid.
    CHECK(f.view.mPerPx == doctest::Approx(kMapZoomMPerPx[1] * kMapFarOverMid));
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
    CHECK(w.land >= kMapLandShareMin);
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
    // No water grid: everything is land; no coast.
    CHECK(look.strokes[static_cast<std::size_t>(MapStrokeKind::Coast)].empty());
    CHECK(mapLandShare(m, look, mapOverlayView({100.f, 100.f}, MapZoom::Near, 1920.f, 1080.f)) == doctest::Approx(1.f));
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
