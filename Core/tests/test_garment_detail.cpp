#include "ftcore/GarmentDetail.h"
#include "ftcore/SkinDetail.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace ftcore;

namespace {

const GarmentTile kTiles[] = {GarmentTile::Leather, GarmentTile::Twill, GarmentTile::Knit, GarmentTile::Pore, GarmentTile::Crease};

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

// Sizes of the 4-connected regions of `in` on an n x n grid that wraps at its edges (a flood fill).
std::vector<size_t> regions(const std::vector<bool>& in, uint32_t n) {
    std::vector<size_t> sizes;
    std::vector<bool> seen(in.size(), false);
    std::vector<uint32_t> stack;
    for (uint32_t start = 0; start < in.size(); ++start) {
        if (!in[start] || seen[start]) {
            continue;
        }
        size_t count = 0;
        seen[start] = true;
        stack.push_back(start);
        while (!stack.empty()) {
            const uint32_t i = stack.back();
            stack.pop_back();
            ++count;
            const uint32_t x = i % n, y = i / n;
            const uint32_t next[4] = {y * n + (x + 1) % n, y * n + (x + n - 1) % n, ((y + 1) % n) * n + x, ((y + n - 1) % n) * n + x};
            for (uint32_t j : next) {
                if (in[j] && !seen[j]) {
                    seen[j] = true;
                    stack.push_back(j);
                }
            }
        }
        sizes.push_back(count);
    }
    return sizes;
}

std::vector<bool> below(const std::vector<float>& h, float level, bool want) {
    std::vector<bool> out(h.size());
    for (size_t i = 0; i < h.size(); ++i) {
        out[i] = (h[i] < level) == want;
    }
    return out;
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

TEST_CASE("pore grain is dimples in one skin, where the old leather grain is walled cells") {
    // Thresholded at mid-height, the pore tile's low set is many small separate pits and its high set (the skin) one
    // region, wrapping. The old Leather tile is the other way round: its grooves wall the high set off into cells
    // (the closed-cell crackle the pore tile replaces).
    const GarmentTileSettings s = garmentTileDefaults(GarmentTile::Pore);
    REQUIRE(s.size == 512);
    std::vector<float> h;
    REQUIRE(makeGarmentHeight(GarmentTile::Pore, s, h));
    const std::vector<size_t> pits = regions(below(h, 0.5f, true), s.size);
    const std::vector<size_t> skin = regions(below(h, 0.5f, false), s.size);
    // The largest pit outline: radius 0.45 cells, 1.2 times that on its long axis.
    const double cell = static_cast<double>(s.size) / s.repeats;
    const double maxPitArea = 3.14159265 * (0.45 * 1.2 * cell) * (0.45 / 1.2 * cell);
    const size_t largest = *std::max_element(pits.begin(), pits.end());
    MESSAGE("pore tile: " << pits.size() << " pits at mid-height, largest " << largest << " px (cap " << maxPitArea
                          << "), skin regions " << skin.size());
    CHECK(pits.size() > 100);
    CHECK(static_cast<double>(largest) <= maxPitArea);
    CHECK(skin.size() == 1);

    const GarmentTileSettings old = garmentTileDefaults(GarmentTile::Leather);
    std::vector<float> l;
    REQUIRE(makeGarmentHeight(GarmentTile::Leather, old, l));
    const std::vector<size_t> cells = regions(below(l, 0.5f, false), old.size);
    MESSAGE("old leather tile: high set in " << cells.size() << " separate regions");
    CHECK(cells.size() > 20);
}

TEST_CASE("pore pits are sparse and the skin between them stays near the top") {
    const GarmentTileSettings s = garmentTileDefaults(GarmentTile::Pore);
    std::vector<float> h;
    REQUIRE(makeGarmentHeight(GarmentTile::Pore, s, h));
    size_t skin = 0, low = 0;
    for (float v : h) {
        skin += v > 0.85f ? 1 : 0;
        low += v < 0.5f ? 1 : 0;
    }
    const double n = static_cast<double>(h.size());
    CHECK(skin / n > 0.6);  // mostly skin: dimples, not a cratered field
    CHECK(low / n > 0.01);
    CHECK(low / n < 0.15);
}

TEST_CASE("crease lines run along v, 2 to 4 mm apart, broken by gaps") {
    const GarmentTileSettings s = garmentTileDefaults(GarmentTile::Crease);
    REQUIRE(s.size == 512);
    std::vector<float> h;
    REQUIRE(makeGarmentHeight(GarmentTile::Crease, s, h));
    const uint32_t n = s.size;
    // Along v the height barely changes; across u it steps at every line.
    double du = 0.0, dv = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        du += columnStep(h, n, i) / n;
        dv += rowStep(h, n, i) / n;
    }
    CHECK(du > 4.0 * dv);
    // Lines cut below 0.7 in each row: how many, and how far apart (mm, the tile drawn at 2.4 cm).
    const double mmPerPx = 10.0 * garmentTileCentimetres(GarmentTile::Crease) / n;
    std::vector<double> spacing;
    double lines = 0.0;
    size_t cut = 0;
    for (uint32_t y = 0; y < n; ++y) {
        std::vector<double> centres;
        uint32_t start = 0;
        while (start < n && h[y * n + start] < 0.7f) {
            ++start;  // begin outside a line, so no run is split by the tile edge
        }
        REQUIRE(start < n);
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t x = (start + k) % n;
            if (h[y * n + x] >= 0.7f) {
                continue;
            }
            uint32_t len = 0;
            while (k < n && h[y * n + (start + k) % n] < 0.7f) {
                ++len;
                ++k;
            }
            cut += len;
            centres.push_back(static_cast<double>(start + k) - 0.5 * (len + 1));
        }
        lines += static_cast<double>(centres.size());
        for (size_t c = 0; c < centres.size(); ++c) {
            const double next = c + 1 < centres.size() ? centres[c + 1] : centres[0] + n;
            spacing.push_back((next - centres[c]) * mmPerPx);
        }
    }
    lines /= n;
    std::sort(spacing.begin(), spacing.end());
    const double median = spacing[spacing.size() / 2];
    const double share = static_cast<double>(cut) / static_cast<double>(h.size());
    MESSAGE("crease tile: " << lines << " lines a row of " << s.repeats << ", median spacing " << median << " mm, "
                            << share << " of the tile cut");
    CHECK(lines > 0.5 * s.repeats);  // the gaps take some lines out of a row
    CHECK(lines <= s.repeats + 1.0);  // a connector adds at most one
    CHECK(median >= 2.0);
    CHECK(median <= 5.0);
    CHECK(share > 0.01);
    CHECK(share < 0.1);  // sparse: fine lines, not a grid
}

TEST_CASE("crease tiles need an even line count") {
    GarmentTileSettings s = garmentTileDefaults(GarmentTile::Crease);
    s.repeats = 7;
    std::vector<float> h;
    CHECK_FALSE(makeGarmentHeight(GarmentTile::Crease, s, h));
    CHECK(garmentTileCentimetres(GarmentTile::Pore) == doctest::Approx(1.6f));
    CHECK(garmentTileCentimetres(GarmentTile::Crease) == doctest::Approx(2.4f));
    CHECK(garmentTileCentimetres(GarmentTile::Leather) == 0.f);
}
