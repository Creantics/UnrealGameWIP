#include <doctest/doctest.h>
#include "ftcore/Backdrop.h"
#include "AllocCounter.h"

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
