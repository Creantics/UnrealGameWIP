#include <doctest/doctest.h>
#include "ftcore/LightPicker.h"
#include "AllocCounter.h"

#include <cmath>
#include <cstdint>
#include <vector>

using ftcore::LightPicker;

namespace {

// The reference answer: every point within radius, by (squared distance, index), first maxCount.
std::vector<std::uint32_t> bruteNearest(const std::vector<float>& xy, float x, float y, float r, std::size_t maxCount) {
    std::vector<std::uint32_t> all;
    for (std::uint32_t i = 0; i < xy.size() / 2; ++i) {
        const double dx = static_cast<double>(xy[i * 2]) - x, dy = static_cast<double>(xy[i * 2 + 1]) - y;
        if (dx * dx + dy * dy <= static_cast<double>(r) * r) all.push_back(i);
    }
    auto d2 = [&](std::uint32_t i) {
        const double dx = static_cast<double>(xy[i * 2]) - x, dy = static_cast<double>(xy[i * 2 + 1]) - y;
        return dx * dx + dy * dy;
    };
    for (std::size_t a = 1; a < all.size(); ++a) {
        for (std::size_t b = a; b > 0 && (d2(all[b]) < d2(all[b - 1]) || (d2(all[b]) == d2(all[b - 1]) && all[b] < all[b - 1])); --b) {
            std::swap(all[b], all[b - 1]);
        }
    }
    if (all.size() > maxCount) all.resize(maxCount);
    return all;
}

} // namespace

TEST_CASE("light picker: empty or unbuilt picks nothing") {
    LightPicker p;
    std::uint32_t out[4] = {};
    CHECK(p.pick(0.f, 0.f, 100.f, 4, out) == 0);
    p.build(nullptr, 0, 10.f);
    CHECK(p.size() == 0);
    CHECK(p.pick(0.f, 0.f, 100.f, 4, out) == 0);
}

TEST_CASE("light picker: one point, in and out of reach; bad arguments") {
    LightPicker p;
    const float xy[] = {5.f, 5.f};
    p.build(xy, 1, 10.f);
    std::uint32_t out[2] = {9, 9};
    CHECK(p.pick(5.f, 8.f, 3.f, 2, out) == 1); // exactly on the radius counts
    CHECK(out[0] == 0);
    CHECK(p.pick(5.f, 8.1f, 3.f, 2, out) == 0);
    CHECK(p.pick(5.f, 5.f, 3.f, 0, out) == 0);
    CHECK(p.pick(5.f, 5.f, 3.f, 2, nullptr) == 0);
    CHECK(p.pick(5.f, 5.f, -1.f, 2, out) == 0);
}

TEST_CASE("light picker: nearest first, ties by lower index, maxCount keeps the nearest") {
    LightPicker p;
    // Index 3 and 1 sit at the same distance as each other; 0 duplicates 2's spot.
    const float xy[] = {10.f, 0.f, 4.f, 0.f, 10.f, 0.f, -4.f, 0.f, 1.f, 0.f};
    p.build(xy, 5, 3.f);
    std::uint32_t out[5] = {};
    REQUIRE(p.pick(0.f, 0.f, 20.f, 5, out) == 5);
    CHECK(out[0] == 4);
    CHECK(out[1] == 1);
    CHECK(out[2] == 3);
    CHECK(out[3] == 0);
    CHECK(out[4] == 2);
    REQUIRE(p.pick(0.f, 0.f, 20.f, 2, out) == 2);
    CHECK(out[0] == 4);
    CHECK(out[1] == 1);
    REQUIRE(p.pick(0.f, 0.f, 20.f, 4, out) == 4);
    CHECK(out[3] == 0); // the tie at 10 goes to the lower index
}

TEST_CASE("light picker: points on cell edges and a query off the grid") {
    LightPicker p;
    const float xy[] = {0.f, 0.f, 10.f, 0.f, 20.f, 0.f, 20.f, 20.f};
    p.build(xy, 4, 10.f);
    std::uint32_t out[4] = {};
    REQUIRE(p.pick(10.f, 0.f, 0.f, 4, out) == 1);
    CHECK(out[0] == 1);
    REQUIRE(p.pick(20.f, 20.f, 0.5f, 4, out) == 1);
    CHECK(out[0] == 3);
    // Well outside the points' box but in reach of the corner.
    REQUIRE(p.pick(-30.f, -40.f, 50.f, 4, out) == 1);
    CHECK(out[0] == 0);
    CHECK(p.pick(500.f, 500.f, 50.f, 4, out) == 0);
}

TEST_CASE("light picker: matches brute force over a scattered city, and a wide spread caps the grid") {
    std::vector<float> xy;
    std::uint32_t seed = 12345u;
    auto next = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    for (int i = 0; i < 3000; ++i) {
        // Whole metres in cm, so many points share a distance.
        xy.push_back(std::floor(next() * 300.f) * 100.f);
        xy.push_back(std::floor(next() * 300.f) * 100.f);
    }
    LightPicker p;
    p.build(xy.data(), xy.size() / 2, 2000.f);
    CHECK(p.size() == 3000);
    std::uint32_t out[64] = {};
    for (int q = 0; q < 200; ++q) {
        const float x = next() * 32000.f - 1000.f, y = next() * 32000.f - 1000.f;
        const std::size_t n = p.pick(x, y, 6000.f, 64, out);
        const std::vector<std::uint32_t> want = bruteNearest(xy, x, y, 6000.f, 64);
        REQUIRE(n == want.size());
        for (std::size_t i = 0; i < n; ++i) CHECK(out[i] == want[i]);
    }

    const float far[] = {0.f, 0.f, 1.0e7f, 1.0e7f};
    LightPicker wide;
    wide.build(far, 2, 1.f);
    CHECK(wide.cellsX() <= LightPicker::kMaxCellsPerAxis);
    CHECK(wide.cellsY() <= LightPicker::kMaxCellsPerAxis);
    REQUIRE(wide.pick(1.0e7f, 1.0e7f, 10.f, 2, out) == 1);
    CHECK(out[0] == 1);
}

TEST_CASE("light picker: pick does not allocate") {
    std::vector<float> xy;
    for (int i = 0; i < 2000; ++i) {
        xy.push_back(static_cast<float>((i * 37) % 400) * 50.f);
        xy.push_back(static_cast<float>((i * 91) % 400) * 50.f);
    }
    LightPicker p;
    p.build(xy.data(), xy.size() / 2, 2000.f);
    std::uint32_t out[96] = {};
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    std::size_t total = 0;
    for (int q = 0; q < 500; ++q) {
        total += p.pick(static_cast<float>(q * 40), static_cast<float>(q * 33), 6000.f, 96, out);
    }
    const auto after = ftcore_test::allocStats().count;
    if (counting) CHECK(after == before);
    CHECK(total > 0);
}
