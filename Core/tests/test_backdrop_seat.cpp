#include <doctest/doctest.h>
#include "ftcore/Backdrop.h"
#include "AllocCounter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace ftcore;

namespace {

// A field 10 m a sample whose every row holds the same heights (metres) along x.
std::vector<std::uint16_t> rowsOf(const std::vector<double>& heights, std::size_t rows) {
    std::vector<std::uint16_t> grid(heights.size() * rows);
    for (std::size_t iy = 0; iy < rows; ++iy) {
        for (std::size_t ix = 0; ix < heights.size(); ++ix) {
            grid[iy * heights.size() + ix] = static_cast<std::uint16_t>(32768.0 + heights[ix] * 128.0);
        }
    }
    return grid;
}

} // namespace

TEST_CASE("skylineSeat: strips keep to the land, under the edge ground, touching; none in the sea") {
    // x 0..990: a hill falling from 120 m to 4 m by x 600, flat to x 790, then the sea bed.
    std::vector<double> heights(100);
    for (std::size_t i = 0; i < heights.size(); ++i) {
        const double x = 10.0 * static_cast<double>(i);
        heights[i] = x <= 600.0 ? 120.0 - 116.0 * x / 600.0 : x <= 790.0 ? 4.0 : -20.0;
    }
    const std::vector<std::uint16_t> grid = rowsOf(heights, 8);
    const HeightField field{grid.data(), heights.size(), 8, 10.0, 0.0, 0.0};
    SkylineSeatSpec spec;
    spec.x0 = 0.0;
    spec.x1 = 990.0;
    spec.edgeY = 0.0;
    spec.stripM = 100.0;
    SkylineStrip strips[kSkylineStripMax];
    const std::size_t count = skylineSeat(field, spec, strips);
    REQUIRE(count == 8);
    CHECK(strips[0].x0 == doctest::Approx(0.f));
    CHECK(strips[count - 1].x1 <= 800.f);
    CHECK(strips[count - 1].x1 >= 780.f);
    for (std::size_t s = 0; s < count; ++s) {
        CHECK(strips[s].x1 > strips[s].x0);
        if (s > 0) {
            CHECK(strips[s].x0 == doctest::Approx(strips[s - 1].x1));
            CHECK(strips[s].topM <= strips[s - 1].topM);
        }
        // Under the edge ground all along, over the sea.
        for (double x = strips[s].x0; x <= strips[s].x1; x += 5.0) {
            CHECK(static_cast<double>(strips[s].topM) < field.at(x, 0.0));
        }
        CHECK(strips[s].topM > 0.f);
    }
    CHECK(skylineSeatAt(strips, count, 300.0) == strips[3].topM);
    CHECK(skylineSeatAt(strips, count, -50.0) == strips[0].topM);
    CHECK(skylineSeatAt(strips, count, 5000.0) == strips[count - 1].topM);
}

TEST_CASE("skylineSeat: the longest land run wins, the strip cap holds, open water or no field gives none") {
    // Land 0..190, water 200..290, land 300..990.
    std::vector<double> heights(100);
    for (std::size_t i = 0; i < heights.size(); ++i) {
        heights[i] = i < 20 ? 6.0 : i < 30 ? -5.0 : 9.0;
    }
    const std::vector<std::uint16_t> grid = rowsOf(heights, 4);
    const HeightField field{grid.data(), heights.size(), 4, 10.0, 0.0, 0.0};
    SkylineSeatSpec spec;
    spec.x0 = 0.0;
    spec.x1 = 990.0;
    spec.stripM = 10.0;
    spec.maxStrips = 5;
    SkylineStrip strips[kSkylineStripMax];
    const std::size_t count = skylineSeat(field, spec, strips);
    REQUIRE(count == 5);
    CHECK(strips[0].x0 >= 295.f);
    CHECK(strips[4].x1 == doctest::Approx(990.f));
    CHECK(strips[2].topM == doctest::Approx(8.5f));
    spec.maxStrips = 100;
    CHECK(skylineSeat(field, spec, strips) == kSkylineStripMax);
    // All water.
    spec.x0 = 205.0;
    spec.x1 = 285.0;
    CHECK(skylineSeat(field, spec, strips) == 0);
    CHECK(skylineSeat(HeightField{}, spec, strips) == 0);
    CHECK(skylineSeat(field, spec, nullptr) == 0);
    CHECK(skylineSeatAt(strips, 0, 10.0) == 0.f);
}

TEST_CASE("skylineSeat: no allocation") {
    const std::vector<double> heights(50, 12.0);
    const std::vector<std::uint16_t> grid = rowsOf(heights, 4);
    const HeightField field{grid.data(), heights.size(), 4, 10.0, 0.0, 0.0};
    SkylineSeatSpec spec;
    spec.x1 = 490.0;
    SkylineStrip strips[kSkylineStripMax];
    const auto before = ftcore_test::allocStats().count;
    const std::size_t count = skylineSeat(field, spec, strips);
    const float top = skylineSeatAt(strips, count, 100.0);
    const auto after = ftcore_test::allocStats().count;
    if (ftcore_test::allocHookInstalled()) CHECK(after == before);
    CHECK(count == 3);
    CHECK(top == doctest::Approx(11.5f));
}

TEST_CASE("hillNightAlbedo: day keeps the hill's colour; full night darkens it to the albedo share and greys it") {
    const HillNightLook look;
    const float green[3] = {0.36f, 0.66f, 0.27f};
    const auto lum = [](const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
    float out[3] = {};
    const auto before = ftcore_test::allocStats().count;
    hillNightAlbedo(look, green, 0.f, 1.f, out);
    for (int k = 0; k < 3; ++k) CHECK(out[k] == doctest::Approx(green[k]));
    hillNightAlbedo(look, green, 1.f, 0.f, out); // the town's own ground, under the hill's foot
    for (int k = 0; k < 3; ++k) CHECK(out[k] == doctest::Approx(green[k]));
    hillNightAlbedo(look, green, 1.f, 1.f, out);
    const auto after = ftcore_test::allocStats().count;
    if (ftcore_test::allocHookInstalled()) CHECK(after == before);
    CHECK(look.albedo >= 0.25f);
    CHECK(look.albedo <= 0.35f);
    CHECK(lum(out) == doctest::Approx(look.albedo * lum(green)).epsilon(0.01));
    // The green's lead over blue shrinks by albedo x saturation: a blue-grey-green, not a dim lawn.
    CHECK(out[1] - out[2] == doctest::Approx(look.albedo * look.saturation * (green[1] - green[2])).epsilon(0.01));
    CHECK(look.saturation < 0.5f);
    // Halfway through dusk, halfway there.
    float half[3] = {};
    hillNightAlbedo(look, green, 0.5f, 1.f, half);
    CHECK(half[1] == doctest::Approx(0.5f * (green[1] + out[1])));
}

TEST_CASE("hillNightShare: none under the hill's foot, all from the ramp's top, smooth between") {
    CHECK(hillNightShare(5.0, 14.0, 8.0) == doctest::Approx(0.f));
    CHECK(hillNightShare(14.0, 14.0, 8.0) == doctest::Approx(0.f));
    CHECK(hillNightShare(18.0, 14.0, 8.0) == doctest::Approx(0.5f));
    CHECK(hillNightShare(22.0, 14.0, 8.0) == doctest::Approx(1.f));
    CHECK(hillNightShare(200.0, 14.0, 8.0) == doctest::Approx(1.f));
    CHECK(hillNightShare(14.5, 14.0, 0.0) == doctest::Approx(1.f)); // no ramp: a step
}

namespace {

// A 2 km square: the town flat at 0 m for x < 400, then a hill rising 0.126 to 200 m at x 1990.
struct HillTown {
    std::vector<double> heights = std::vector<double>(200);
    std::vector<std::uint16_t> grid;
    HeightField field;
    HillTown() {
        for (std::size_t i = 0; i < heights.size(); ++i) {
            const double x = 10.0 * static_cast<double>(i);
            heights[i] = x < 400.0 ? 0.0 : 200.0 * (x - 400.0) / 1590.0;
        }
        grid = rowsOf(heights, 200);
        field = HeightField{grid.data(), heights.size(), 200, 10.0, 0.0, 0.0};
    }
};

bool insideBox(const MassingBox& b, double x, double y, double margin) {
    const double a = b.rot * 3.14159265358979323846 / 180.0, c = std::cos(a), s = std::sin(a);
    const double lx = c * (x - b.cx) + s * (y - b.cy), ly = -s * (x - b.cx) + c * (y - b.cy);
    return std::abs(lx) <= 0.5 * b.sx + margin && std::abs(ly) <= 0.5 * b.sy + margin;
}

} // namespace

TEST_CASE("hillWindows: a few dozen to 150 lit windows in hamlets, all in the low band, none on a box, repeatable") {
    const HillTown town;
    HillWindowSpec spec;
    spec.nearX = 200.0;
    spec.nearY = 1000.0;
    std::vector<MassingBox> boxes(2);
    boxes[0].cx = 700.0; boxes[0].cy = 1000.0; boxes[0].sx = 120.0; boxes[0].sy = 120.0; boxes[0].rot = 30.0;
    boxes[1].cx = 620.0; boxes[1].cy = 400.0; boxes[1].sx = 60.0; boxes[1].sy = 200.0; boxes[1].rot = 0.0;
    const auto houses = hillWindows(town.field, spec, 0.0, 0.0, 1990.0, 1990.0, boxes.data(), boxes.size());
    const float bandTop = hillWindowBandTop(town.field, spec, 0.0, 0.0, 1990.0, 1990.0);
    CHECK(bandTop == doctest::Approx(14.0 + 0.25 * (200.0 - 14.0)).epsilon(0.01));
    std::size_t windows = 0;
    for (const HillHouse& h : houses) {
        CHECK(h.windows >= 1);
        CHECK(h.windows <= spec.windowsMax);
        windows += h.windows;
        const double ground = town.field.at(h.x, h.y);
        CHECK(ground >= spec.footM);
        CHECK(ground <= bandTop);
        CHECK(h.frontZ >= spec.footM);
        CHECK(h.frontZ <= bandTop);
        CHECK(h.z0 < h.frontZ);
        CHECK(h.z1 > h.frontZ + spec.sillM + spec.windowH);
        CHECK(std::cos(h.yaw * 3.14159265358979323846 / 180.0) < 0.0); // the front turns down the hill, to the town
        const double half = 0.5 * std::sqrt(h.w * h.w + h.d * h.d);
        for (const MassingBox& b : boxes) CHECK_FALSE(insideBox(b, h.x, h.y, spec.clearM + half - 0.01));
        for (std::size_t k = 0; k < h.windows; ++k) {
            const HillWindow w = hillHouseWindow(h, spec, k);
            CHECK(w.z > h.z0);
            CHECK(w.z < h.z1);
            for (const MassingBox& b : boxes) CHECK_FALSE(insideBox(b, w.x, w.y, spec.clearM));
            // On the front: half the depth out from the centre, within the front's width.
            const double dx = w.x - h.x, dy = w.y - h.y;
            const double a = h.yaw * 3.14159265358979323846 / 180.0;
            CHECK(dx * std::cos(a) + dy * std::sin(a) == doctest::Approx(0.5 * (h.d + spec.windowDepthM)).epsilon(1e-3));
            CHECK(std::abs(-dx * std::sin(a) + dy * std::cos(a)) + 0.5 * spec.windowW <= 0.5 * h.w + 1e-3);
        }
    }
    CHECK(windows >= 30);
    CHECK(windows <= spec.maxWindows);
    // Hamlets: most houses have a neighbour within two hamlet radii.
    std::size_t clustered = 0;
    for (std::size_t i = 0; i < houses.size(); ++i) {
        for (std::size_t j = 0; j < houses.size(); ++j) {
            const double dx = houses[i].x - houses[j].x, dy = houses[i].y - houses[j].y;
            if (i != j && dx * dx + dy * dy < 4.0 * spec.hamletRadiusM * spec.hamletRadiusM) {
                ++clustered;
                break;
            }
        }
    }
    CHECK(clustered * 10 >= houses.size() * 7);
    const auto again = hillWindows(town.field, spec, 0.0, 0.0, 1990.0, 1990.0, boxes.data(), boxes.size());
    REQUIRE(again.size() == houses.size());
    for (std::size_t i = 0; i < houses.size(); ++i) {
        CHECK(again[i].x == houses[i].x);
        CHECK(again[i].y == houses[i].y);
        CHECK(again[i].windows == houses[i].windows);
    }
}

TEST_CASE("hillWindows: the cap holds and spreads; flat ground or no field gives none") {
    const HillTown town;
    HillWindowSpec spec;
    spec.nearX = 200.0;
    spec.nearY = 1000.0;
    spec.maxWindows = 20;
    const auto houses = hillWindows(town.field, spec, 0.0, 0.0, 1990.0, 1990.0, nullptr, 0);
    std::size_t windows = 0;
    float yMin = 1e9f, yMax = -1e9f;
    for (const HillHouse& h : houses) {
        windows += h.windows;
        yMin = std::min(yMin, h.y);
        yMax = std::max(yMax, h.y);
    }
    CHECK(windows == 20);
    CHECK(yMax - yMin > 500.f); // a hashed order, not the scan's first rows

    const std::vector<double> flat(50, 8.0);
    const std::vector<std::uint16_t> grid = rowsOf(flat, 50);
    const HeightField low{grid.data(), flat.size(), 50, 10.0, 0.0, 0.0};
    CHECK(hillWindows(low, spec, 0.0, 0.0, 490.0, 490.0, nullptr, 0).empty());
    CHECK(hillWindows(HeightField{}, spec, 0.0, 0.0, 490.0, 490.0, nullptr, 0).empty());
}
