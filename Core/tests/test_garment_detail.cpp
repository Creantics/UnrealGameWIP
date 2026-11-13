#include "ftcore/GarmentDetail.h"
#include "ftcore/SkinDetail.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace ftcore;

namespace {

const GarmentTile kTiles[] = {GarmentTile::Leather, GarmentTile::Twill, GarmentTile::Knit};

GarmentTileSettings small(GarmentTile tile) {
    GarmentTileSettings s = garmentTileDefaults(tile);
    s.size = 128;
    s.repeats = tile == GarmentTile::Twill ? 12 : 10;
    return s;
}

// Mean absolute step between columns x and x + 1 (wrapping) over every row.
double columnStep(const std::vector<float>& h, uint32_t n, uint32_t x) {
    double sum = 0.0;
    for (uint32_t y = 0; y < n; ++y) {
        sum += std::fabs(h[y * n + (x + 1) % n] - h[y * n + x]);
    }
    return sum / n;
}

double rowStep(const std::vector<float>& h, uint32_t n, uint32_t y) {
    double sum = 0.0;
    for (uint32_t x = 0; x < n; ++x) {
        sum += std::fabs(h[((y + 1) % n) * n + x] - h[y * n + x]);
    }
    return sum / n;
}

// The largest step between neighbouring columns (rows) inside the tile, not across its edge. Weaves and
// knits have hard steps at every yarn or loop border, so the seam is compared against the largest one.
double maxColumnStep(const std::vector<float>& h, uint32_t n) {
    double most = 0.0;
    for (uint32_t x = 0; x + 1 < n; ++x) {
        most = std::max(most, columnStep(h, n, x));
    }
    return most;
}

double maxRowStep(const std::vector<float>& h, uint32_t n) {
    double most = 0.0;
    for (uint32_t y = 0; y + 1 < n; ++y) {
        most = std::max(most, rowStep(h, n, y));
    }
    return most;
}

} // namespace

TEST_CASE("garment tiles fill [0, 1] and repeat the same for the same settings") {
    for (GarmentTile tile : kTiles) {
        const GarmentTileSettings s = small(tile);
        std::vector<float> a, b;
        REQUIRE(makeGarmentHeight(tile, s, a));
        REQUIRE(makeGarmentHeight(tile, s, b));
        REQUIRE(a.size() == static_cast<size_t>(s.size) * s.size);
        CHECK(a == b);
        float lo = 1.f, hi = 0.f;
        for (float h : a) {
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
        CHECK(lo == doctest::Approx(0.f));
        CHECK(hi == doctest::Approx(1.f));
    }
}

TEST_CASE("garment tiles wrap: the step across the tile edge is like any other step") {
    for (GarmentTile tile : kTiles) {
        const GarmentTileSettings s = small(tile);
        std::vector<float> h;
        REQUIRE(makeGarmentHeight(tile, s, h));
        const uint32_t n = s.size;
        // The seam step is no larger than the largest step inside the tile in that direction (a little slack:
        // the edge falls exactly on a loop border, where inside the tile a border usually splits a pixel).
        CHECK(columnStep(h, n, n - 1) <= 1.15 * maxColumnStep(h, n) + 1e-3);
        CHECK(rowStep(h, n, n - 1) <= 1.15 * maxRowStep(h, n) + 1e-3);
    }
}

TEST_CASE("a different seed gives a different leather grain") {
    GarmentTileSettings s = small(GarmentTile::Leather);
    std::vector<float> a, b;
    REQUIRE(makeGarmentHeight(GarmentTile::Leather, s, a));
    s.seed += 1;
    REQUIRE(makeGarmentHeight(GarmentTile::Leather, s, b));
    CHECK(a != b);
}

TEST_CASE("leather grain has grooves: a share of the tile sits low") {
    const GarmentTileSettings s = small(GarmentTile::Leather);
    std::vector<float> h;
    REQUIRE(makeGarmentHeight(GarmentTile::Leather, s, h));
    size_t low = 0;
    for (float v : h) {
        low += v < 0.35f ? 1 : 0;
    }
    const double share = static_cast<double>(low) / static_cast<double>(h.size());
    CHECK(share > 0.03);
    CHECK(share < 0.5);
}

TEST_CASE("twill runs on the diagonal") {
    // The weave repeats where yarn column + yarn row stays the same, so a shift of one yarn right and one
    // up lands on the same weave, and one right and one down does not. Few, wide yarns keep the rounding of
    // the shift to whole pixels small.
    GarmentTileSettings s = small(GarmentTile::Twill);
    s.repeats = 6;
    std::vector<float> h;
    REQUIRE(makeGarmentHeight(GarmentTile::Twill, s, h));
    const uint32_t n = s.size;
    const uint32_t yarn = static_cast<uint32_t>(std::lround(static_cast<double>(n) / s.repeats));
    double along = 0.0, across = 0.0;
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            const float v = h[y * n + x];
            along += std::fabs(v - h[((y + n - yarn) % n) * n + (x + yarn) % n]);
            across += std::fabs(v - h[((y + yarn) % n) * n + (x + yarn) % n]);
        }
    }
    CHECK(along < 0.5 * across);
}

TEST_CASE("knit loops repeat once per column") {
    const GarmentTileSettings s = small(GarmentTile::Knit);
    std::vector<float> h;
    REQUIRE(makeGarmentHeight(GarmentTile::Knit, s, h));
    const uint32_t n = s.size;
    // n / repeats is not a whole number of pixels (128 / 10), so compare against a shift of one column rounded.
    const uint32_t col = static_cast<uint32_t>(std::lround(static_cast<double>(n) / s.repeats));
    double same = 0.0, half = 0.0;
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            const float v = h[y * n + x];
            same += std::fabs(v - h[y * n + (x + col) % n]);
            half += std::fabs(v - h[y * n + (x + col / 2) % n]);
        }
    }
    CHECK(same < half);
}

TEST_CASE("garment tile settings out of range are refused") {
    std::vector<float> h{1.f};
    GarmentTileSettings s = small(GarmentTile::Leather);
    s.size = 100;
    CHECK_FALSE(makeGarmentHeight(GarmentTile::Leather, s, h));
    CHECK(h.empty());
    s = small(GarmentTile::Leather);
    s.repeats = 0;
    CHECK_FALSE(makeGarmentHeight(GarmentTile::Leather, s, h));
    s.repeats = s.size;
    CHECK_FALSE(makeGarmentHeight(GarmentTile::Leather, s, h));
    s = small(GarmentTile::Twill);
    s.repeats = 10;  // not a multiple of the three-yarn repeat
    CHECK_FALSE(makeGarmentHeight(GarmentTile::Twill, s, h));
}

TEST_CASE("garment tiles go through the skin texture packers") {
    const GarmentTileSettings s = small(GarmentTile::Knit);
    std::vector<float> h;
    REQUIRE(makeGarmentHeight(GarmentTile::Knit, s, h));
    std::vector<uint8_t> normal, mask;
    skinNormalBgra(h, s.size, s.normalStrength, normal);
    skinMaskBgra(h, s.size, mask);
    CHECK(normal.size() == static_cast<size_t>(s.size) * s.size * 4);
    CHECK(mask.size() == normal.size());
}
