#include "ftcore/SkinDetail.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <vector>

using namespace ftcore;

namespace {

SkinDetailSettings small() {
    SkinDetailSettings s;
    s.size = 128;
    s.poreCells = 12;
    return s;
}

// Mean absolute step between neighbouring columns (x, x + 1) over every row.
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

} // namespace

TEST_CASE("skin detail: the height field is in range, has pores and is the same for the same seed") {
    const SkinDetailSettings s = small();
    std::vector<float> a, b;
    REQUIRE(makeSkinHeight(s, a));
    REQUIRE(makeSkinHeight(s, b));
    REQUIRE(a.size() == size_t{128} * 128);
    CHECK(a == b);
    int open = 0, pit = 0;
    for (float h : a) {
        CHECK(h >= 0.f);
        CHECK(h <= 1.f);
        open += h > 0.99f;
        pit += h < 0.8f;
    }
    // Mostly open skin, with a real share of pore pixels.
    CHECK(open > static_cast<int>(a.size()) / 2);
    CHECK(pit > static_cast<int>(a.size()) / 100);

    SkinDetailSettings other = s;
    other.seed = 2;
    REQUIRE(makeSkinHeight(other, b));
    CHECK(a != b);
}

TEST_CASE("skin detail: the tile wraps without a seam") {
    const SkinDetailSettings s = small();
    std::vector<float> h;
    REQUIRE(makeSkinHeight(s, h));
    const uint32_t n = s.size;
    double inner = 0.0;
    for (uint32_t i = 0; i + 1 < n; ++i) {
        inner += columnStep(h, n, i) + rowStep(h, n, i);
    }
    inner /= 2.0 * (n - 1);
    // Across the wrap the step is no bigger than an average step inside the tile.
    CHECK(columnStep(h, n, n - 1) < inner * 2.0);
    CHECK(rowStep(h, n, n - 1) < inner * 2.0);
}

TEST_CASE("skin detail: normals are unit length, point out, and tilt towards a pore's centre") {
    SkinDetailSettings s = small();
    s.grooveDepth = 0.f;
    std::vector<float> h;
    REQUIRE(makeSkinHeight(s, h));
    std::vector<uint8_t> normal;
    skinNormalBgra(h, s.size, s.normalStrength, normal);
    REQUIRE(normal.size() == h.size() * 4);
    for (size_t i = 0; i < h.size(); ++i) {
        const float x = normal[i * 4 + 2] / 127.5f - 1.f, y = normal[i * 4 + 1] / 127.5f - 1.f,
                    z = normal[i * 4 + 0] / 127.5f - 1.f;
        CHECK(std::fabs(std::sqrt(x * x + y * y + z * z) - 1.f) < 0.02f);
        CHECK(z > 0.f);
        CHECK(normal[i * 4 + 3] == 255);
    }
    // Find the deepest pixel, then check the slope on its left faces right (+X) and the one on its right faces left.
    size_t deepest = 0;
    for (size_t i = 1; i < h.size(); ++i) {
        if (h[i] < h[deepest]) {
            deepest = i;
        }
    }
    const size_t n = s.size, x = deepest % n, y = deepest / n;
    const size_t left = y * n + (x + n - 2) % n, right = y * n + (x + 2) % n;
    CHECK(normal[left * 4 + 2] > 128);
    CHECK(normal[right * 4 + 2] < 128);
}

TEST_CASE("skin detail: masks carry cavity in R and micro roughness round 128 in G") {
    const SkinDetailSettings s = small();
    std::vector<float> h;
    REQUIRE(makeSkinHeight(s, h));
    std::vector<uint8_t> mask;
    skinMaskBgra(h, s.size, mask);
    REQUIRE(mask.size() == h.size() * 4);
    double g = 0.0;
    for (size_t i = 0; i < h.size(); ++i) {
        CHECK(std::abs(static_cast<int>(mask[i * 4 + 2]) - static_cast<int>(std::lround(h[i] * 255.f))) <= 1);
        g += mask[i * 4 + 1];
        CHECK(mask[i * 4 + 1] >= 80);
        CHECK(mask[i * 4 + 1] <= 176);
    }
    CHECK(std::fabs(g / h.size() - 128.0) < 12.0);
}

TEST_CASE("skin detail: bad settings give no tile") {
    std::vector<float> h{1.f};
    SkinDetailSettings s = small();
    s.size = 100;
    CHECK_FALSE(makeSkinHeight(s, h));
    CHECK(h.empty());
    s = small();
    s.poreCells = 0;
    CHECK_FALSE(makeSkinHeight(s, h));
    s = small();
    s.poreRadius = 0.7f;
    CHECK_FALSE(makeSkinHeight(s, h));
}
