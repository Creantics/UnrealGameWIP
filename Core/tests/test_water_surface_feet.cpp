// A surface swimmer's feet height (WaterSurfaceFeet.h): the draft and bob in deep water, the bed floor in a shallow
// column, the splash's sink. The 11-18 rejoin case: a partner placed by the one rule starts where its swim holds it
// (the old placement at the draft alone bobbed up 19 cm over a 1.16 m column). The ease's lag on the bob.
#include "ftcore/WaterSurfaceFeet.h"
#include "ftcore/WaterBodies.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

using ftcore::kWaterBedFloat;
using ftcore::waterFeetEase;
using ftcore::waterSurfaceFeetZ;
using ftcore::waterSwimFeetZ;

namespace {

constexpr float kDt = 1.f / 60.f;
constexpr float kRiseEase = 0.35f; // the partner's surface ease (FTPartyFollower kWaterRiseEase)

struct Run {
    float maxFromFinal = 0.f;  // m: the largest gap to the last frame's feet
    float maxFromTarget = 0.f; // m: the largest gap to that frame's target
    float rise = 0.f;          // m: the last frame's feet minus the placed feet
};

// A partner placed at placedZ at time t0, then swum at the surface for 2 s (treading in place: the target moves only
// with the bob), as FTPartyFollower's Surface phase does.
Run swimAfterPlacement(float placedZ, float surface, float bedZ, float t0)
{
    constexpr int kFrames = 120;
    float feet[kFrames + 1] = {};
    float target[kFrames + 1] = {};
    feet[0] = placedZ;
    target[0] = waterSurfaceFeetZ(waterSwimFeetZ(surface, t0), 0.f, bedZ);
    for (int i = 1; i <= kFrames; ++i) {
        const float t = t0 + static_cast<float>(i) * kDt;
        target[i] = waterSurfaceFeetZ(waterSwimFeetZ(surface, t), 0.f, bedZ);
        feet[i] = waterFeetEase(feet[i - 1], target[i], kDt, kRiseEase);
    }
    Run r;
    for (int i = 0; i <= kFrames; ++i) {
        r.maxFromFinal = std::max(r.maxFromFinal, std::fabs(feet[i] - feet[kFrames]));
        r.maxFromTarget = std::max(r.maxFromTarget, std::fabs(feet[i] - target[i]));
    }
    r.rise = feet[kFrames] - placedZ;
    return r;
}

} // namespace

TEST_CASE("surface feet: the draft in deep water, the bed floor in a shallow column, the sink")
{
    const float swim = waterSwimFeetZ(2.f, 0.f); // surface 2 m, bob at its zero
    CHECK(swim == doctest::Approx(2.f - ftcore::kWaterSwimDraft));
    // Deep: the bed far under the feet.
    CHECK(waterSurfaceFeetZ(swim, 0.f, -3.f) == doctest::Approx(swim));
    // Shallow: a 1.16 m column (bed 0.84) under the 1.28 m draft: the feet ride just over the bed.
    CHECK(waterSurfaceFeetZ(swim, 0.f, 0.84f) == doctest::Approx(0.84f + kWaterBedFloat));
    // A splash's sink goes down, but not into the bed.
    CHECK(waterSurfaceFeetZ(swim, -0.5f, -3.f) == doctest::Approx(swim - 0.5f));
    CHECK(waterSurfaceFeetZ(swim, -2.f, -1.f) == doctest::Approx(-1.f + kWaterBedFloat));
}

TEST_CASE("surface feet: the ease")
{
    CHECK(waterFeetEase(0.f, 1.f, 0.f, kRiseEase) == doctest::Approx(1.f));
    CHECK(waterFeetEase(0.f, 1.f, kDt, 0.f) == doctest::Approx(1.f));
    const float one = waterFeetEase(0.f, 1.f, kDt, kRiseEase);
    CHECK(one > 0.f);
    CHECK(one < 0.1f);
    // One time constant covers 1 - 1/e of the gap.
    float z = 0.f;
    for (int i = 0; i < 21; ++i) {
        z = waterFeetEase(z, 1.f, kRiseEase / 21.f, kRiseEase);
    }
    CHECK(z == doctest::Approx(1.f - std::exp(-1.f)).epsilon(0.01));
}

TEST_CASE("surface feet: the 11-18 rejoin over a shallow column starts where the swim holds it")
{
    // The land capture's slot: feet placed at 1.18 m (the draft alone), the bed 1.298 m, so the column (1.16 m) is
    // under the draft and the swim holds the feet at 1.378 m.
    const float surface = 1.18f + ftcore::kWaterSwimDraft;
    const float bed = 1.298f;
    const float t0 = 0.f; // the bob at its zero, so the old placement is exactly 1.18
    // The old placement: the draft alone. The feet then rise ~19 cm over the first second (the bob-up seen).
    const Run old = swimAfterPlacement(waterSwimFeetZ(surface, t0), surface, bed, t0);
    CHECK(old.rise > 0.15f);
    CHECK(old.maxFromFinal > 0.15f);
    // The one rule: placed at the swim's own height, within 3 cm of the final value from the first frame.
    const float placed = waterSurfaceFeetZ(waterSwimFeetZ(surface, t0), 0.f, bed);
    CHECK(placed == doctest::Approx(bed + kWaterBedFloat));
    const Run now = swimAfterPlacement(placed, surface, bed, t0);
    CHECK(now.maxFromFinal < 0.03f);
    CHECK(now.maxFromTarget < 0.03f);
    CHECK(std::fabs(now.rise) < 0.03f);
}

TEST_CASE("surface feet: in deep water the placed feet follow the bob's target within 3 cm")
{
    const float surface = 2.46f;
    const float bed = -4.f;
    // Over a range of bob phases at the placement.
    for (int k = 0; k < 13; ++k) {
        const float t0 = static_cast<float>(k) * ftcore::kWaterBobPeriod / 13.f;
        const float placed = waterSurfaceFeetZ(waterSwimFeetZ(surface, t0), 0.f, bed);
        const Run r = swimAfterPlacement(placed, surface, bed, t0);
        CAPTURE(t0);
        CHECK(r.maxFromTarget < 0.03f);
        // The bob alone moves the feet; the gap to the final frame stays within its swing (2 x kWaterBobHeight).
        CHECK(r.maxFromFinal <= 2.f * ftcore::kWaterBobHeight + 1e-3f);
    }
}
