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

TEST_CASE("pore pits are dense: 0.6 to 1.0 mm apart, one in five larger") {
    const GarmentTileSettings s = garmentTileDefaults(GarmentTile::Pore);
    std::vector<GarmentPore> pores;
    REQUIRE(garmentPores(s, pores));
    const double tile = 10.0 * garmentTileCentimetres(GarmentTile::Pore);
    REQUIRE(pores.size() > 200);
    size_t inBand = 0, large = 0;
    double rLarge = 0.0, rSmall = 0.0;
    for (size_t i = 0; i < pores.size(); ++i) {
        double nearest = 1e9;
        for (size_t j = 0; j < pores.size(); ++j) {
            if (i == j) {
                continue;
            }
            double dx = std::fabs(pores[i].x - pores[j].x), dy = std::fabs(pores[i].y - pores[j].y);
            dx = std::min(dx, tile - dx);  // the tile wraps
            dy = std::min(dy, tile - dy);
            nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy));
        }
        inBand += nearest >= 0.6 - 1e-4 && nearest <= 1.0 ? 1 : 0;
        if (pores[i].large) {
            ++large;
            rLarge += pores[i].radius;
        } else {
            rSmall += pores[i].radius;
        }
    }
    const double n = static_cast<double>(pores.size());
    rLarge /= std::max(static_cast<double>(large), 1.0);
    rSmall /= std::max(n - static_cast<double>(large), 1.0);
    MESSAGE("pore tile: " << pores.size() << " pits, " << inBand / n << " with the nearest 0.6 to 1.0 mm away, "
                          << large / n << " large (mean radius " << rLarge << " mm against " << rSmall << " mm)");
    CHECK(inBand / n >= 0.9);
    CHECK(large / n >= 0.15);
    CHECK(large / n <= 0.25);
    CHECK(rLarge > 1.15 * rSmall);
}

TEST_CASE("pore pits are separate dimples in one skin") {
    // Thresholded at mid-height, every pit is its own low region (no two merge, none lost) and the skin between
    // them one high region, wrapping. The old Leather tile is the other way round: its grooves wall the high set
    // off into cells.
    const GarmentTileSettings s = garmentTileDefaults(GarmentTile::Pore);
    REQUIRE(s.size == 512);
    std::vector<float> h;
    REQUIRE(makeGarmentHeight(GarmentTile::Pore, s, h));
    std::vector<GarmentPore> pores;
    REQUIRE(garmentPores(s, pores));
    const std::vector<size_t> pits = regions(below(h, 0.5f, true), s.size);
    const std::vector<size_t> skin = regions(below(h, 0.5f, false), s.size);
    size_t low = 0;
    for (float v : h) {
        low += v < 0.5f ? 1 : 0;
    }
    const double lowShare = static_cast<double>(low) / static_cast<double>(h.size());
    MESSAGE("pore tile: " << pits.size() << " pits at mid-height of " << pores.size() << " sites, skin regions "
                          << skin.size() << ", " << lowShare << " of the tile below mid-height");
    CHECK(static_cast<double>(pits.size()) >= 0.95 * static_cast<double>(pores.size()));
    CHECK(static_cast<double>(pits.size()) <= 1.02 * static_cast<double>(pores.size()));
    CHECK(skin.size() == 1);
    CHECK(lowShare > 0.04);  // dense enough that the skin between pits reads as pebbles
    CHECK(lowShare < 0.3);

    const GarmentTileSettings old = garmentTileDefaults(GarmentTile::Leather);
    std::vector<float> l;
    REQUIRE(makeGarmentHeight(GarmentTile::Leather, old, l));
    const std::vector<size_t> cells = regions(below(l, 0.5f, false), old.size);
    CHECK(cells.size() > 20);
}

TEST_CASE("creases are 5 to 30 mm long, within 30 degrees of u, some branching") {
    const GarmentTileSettings s = garmentTileDefaults(GarmentTile::Crease);
    std::vector<GarmentCrease> creases;
    REQUIRE(garmentCreases(s, creases));
    size_t mains = 0, branches = 0, shortOnes = 0;
    float longest = 0.f, shortest = 1e9f, widest = 0.f;
    for (const GarmentCrease& c : creases) {
        if (c.branch) {
            ++branches;
            continue;
        }
        ++mains;
        longest = std::max(longest, c.length);
        shortest = std::min(shortest, c.length);
        widest = std::max(widest, c.maxAngle);
        shortOnes += c.length < 15.f ? 1 : 0;
    }
    MESSAGE("crease tile: " << mains << " creases, " << branches << " branches, " << shortest << " to " << longest
                            << " mm, widest turn " << widest << " degrees");
    CHECK(mains == s.repeats);
    CHECK(shortest >= 5.f);
    CHECK(longest <= 30.f);
    CHECK(widest <= 30.f + 1e-3f);
    CHECK(shortOnes > mains / 2);  // more short than long
    CHECK(branches > mains / 5);
    CHECK(branches < mains / 2);
}

TEST_CASE("crease tile: grooves run along u, sparse, each pixel ranked") {
    const GarmentTileSettings s = garmentTileDefaults(GarmentTile::Crease);
    REQUIRE(s.size == 1024);
    std::vector<float> h, rank;
    REQUIRE(makeGarmentHeight(GarmentTile::Crease, s, h));
    REQUIRE(garmentCreaseRank(s, rank));
    REQUIRE(rank.size() == h.size());
    const uint32_t n = s.size;
    // Across v the height steps at every groove; along u it barely changes.
    double du = 0.0, dv = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        du += columnStep(h, n, i) / n;
        dv += rowStep(h, n, i) / n;
    }
    size_t cut = 0, rankedCut = 0, ranked = 0;
    for (size_t i = 0; i < h.size(); ++i) {
        CHECK_FALSE(rank[i] < 0.f);
        CHECK_FALSE(rank[i] > 1.f);
        ranked += rank[i] < 1.f ? 1 : 0;
        if (h[i] < 0.7f) {
            ++cut;
            rankedCut += rank[i] < 1.f ? 1 : 0;
        }
    }
    const double share = static_cast<double>(cut) / static_cast<double>(h.size());
    MESSAGE("crease tile: step across v " << dv << " against along u " << du << ", " << share << " of the tile cut");
    CHECK(dv > 1.5 * du);
    CHECK(share > 0.005);
    CHECK(share < 0.12);  // every crease shown, as at the elbow; the fold map hides most of them elsewhere
    CHECK(rankedCut == cut);
    // the rank reaches past each groove (its soft edge keeps its crease's rank) but leaves most of the skin unranked
    CHECK(ranked > cut + cut / 2);
    CHECK(static_cast<double>(ranked) < 0.5 * static_cast<double>(h.size()));
}

TEST_CASE("crease tiles take any count; the tile sizes") {
    GarmentTileSettings s = small(GarmentTile::Crease);
    s.repeats = 7;
    std::vector<float> h;
    CHECK(makeGarmentHeight(GarmentTile::Crease, s, h));
    s.repeats = 0;
    CHECK_FALSE(makeGarmentHeight(GarmentTile::Crease, s, h));
    std::vector<float> rank;
    CHECK_FALSE(garmentCreaseRank(s, rank));
    CHECK(rank.empty());
    CHECK(garmentTileCentimetres(GarmentTile::Pore) == doctest::Approx(1.6f));
    CHECK(garmentTileCentimetres(GarmentTile::Crease) == doctest::Approx(6.f));
    CHECK(garmentTileCentimetres(GarmentTile::Leather) == 0.f);
}
