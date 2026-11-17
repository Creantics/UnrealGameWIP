#include <doctest/doctest.h>
#include "ftcore/FarSigns.h"

using namespace ftcore;

namespace {
FarSignBlock box(float x0, float y0, float x1, float y1, float top) {
    FarSignBlock b;
    b.minX = x0;
    b.minY = y0;
    b.maxX = x1;
    b.maxY = y1;
    b.topZ = top;
    return b;
}
} // namespace

TEST_CASE("far signs: nearest eligible blocks first, a card on the face toward the centre") {
    const FarSignBlock blocks[] = {
        box(30.f, -5.f, 40.f, 5.f, 15.f),    // east, 30 m: card on its -X face, looking -X
        box(-5.f, 50.f, 5.f, 60.f, 20.f),    // north, 50 m: card on its -Y face
        box(-5.f, -5.f, 5.f, 5.f, 30.f),     // contains the centre: none
        box(10.f, 0.f, 12.f, 2.f, 20.f),     // 10 m: nearer than nearM
        box(-60.f, -3.f, -50.f, 3.f, 4.f),   // too low
        box(-300.f, -300.f, 300.f, -200.f, 20.f), // the ground: too big
    };
    FarSign out[8];
    FarSignSpec spec;
    const std::size_t n = placeFarSigns(blocks, 6, 0.f, 0.f, spec, out, 8);
    REQUIRE(n == 2);
    CHECK(out[0].x == doctest::Approx(30.f - spec.outM));
    CHECK(out[0].y == doctest::Approx(0.f));
    CHECK(out[0].yawDeg == 180.f);
    CHECK(out[1].y == doctest::Approx(50.f - spec.outM));
    CHECK(out[1].yawDeg == 270.f);
    CHECK(out[0].colour == 0);
    CHECK(out[1].colour == 1);
    // The card stays under the roof.
    for (std::size_t i = 0; i < n; ++i) {
        CHECK(out[i].z + 0.5f * out[i].heightM <= 20.f - 0.5f + 1e-4f);
        CHECK(out[i].heightM > 0.f);
    }
}

TEST_CASE("far signs: the count is capped, ties keep index order, a face's ends are kept clear") {
    FarSignBlock blocks[4];
    for (int i = 0; i < 4; ++i) {
        blocks[i] = box(30.f, -40.f + 20.f * static_cast<float>(i), 36.f, -25.f + 20.f * static_cast<float>(i), 12.f);
    }
    FarSign out[3];
    FarSignSpec spec;
    spec.farM = 200.f;
    const std::size_t n = placeFarSigns(blocks, 4, 0.f, 0.f, spec, out, 3);
    CHECK(n == 3);
    for (std::size_t i = 0; i < n; ++i) {
        CHECK(out[i].x == doctest::Approx(30.f - spec.outM));
        CHECK(out[i].yawDeg == 180.f);
    }
    // Block 2 (0..15, square across, 30 m) comes first, then block 1 (-20..-5, 30.4 m); block 0 is the farthest.
    CHECK(out[0].y == doctest::Approx(0.f + spec.cornerM));
    CHECK(out[1].y == doctest::Approx(-5.f - spec.cornerM));
    CHECK(placeFarSigns(nullptr, 4, 0.f, 0.f, spec, out, 3) == 0);
    CHECK(farSignColour(3) == 2);
}
