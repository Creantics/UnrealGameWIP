// A surface swimmer's feet height (WaterSurfaceFeet.h): the draft and bob in deep water, the bed floor in a shallow
// column, the splash's sink. The 11-18 rejoin case: a partner placed by the one rule starts where its swim holds it
// (the old placement at the draft alone bobbed up 19 cm over a 1.16 m column). The ease's lag on the bob. The open
// sea's case (11-18, 3.3 cm): the ease alone trails a wave and the bob; the carry rides them and eases only a gap.
#include "ftcore/WaterSurfaceFeet.h"
#include "ftcore/WaterBodies.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

using ftcore::kWaterBedFloat;
using ftcore::waterFeetCarry;
using ftcore::waterFeetEase;
using ftcore::waterFeetOnRide;
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

// A sea with one wave under a swimmer treading in place: its height (m) at time t.
struct Sea {
    float level = 2.46f;
    float amp = 0.f;      // m
    float period = 4.f;   // s
    float phase = 0.f;    // rad
};

float seaAt(const Sea& sea, float t)
{
    return sea.level + sea.amp * std::sin(6.2831853f * t / sea.period + sea.phase);
}

struct SeaRun {
    float maxFromTarget = 0.f; // m: after each frame's step, the feet off that frame's target
    float maxBeforeStep = 0.f; // m: the feet before each frame's step off its target (the -SwimProof judge's reading)
    float lastGap = 0.f;       // m: the last frame's feet off its target
    float firstGap = 0.f;      // m: the first frame's
    float lowest = 1e9f;       // m: the lowest feet
};

// FTPartyFollower's Surface step treading in place on the sea: placed on the target at t0 (no sink), then frames at
// 60 fps with a sink from the first frame. carry: the host's rule (the ride's move carried while the target is on the
// ride both frames); else the ease alone (-SwimFeetEaseAll, and the step before 11-18).
SeaRun swimOnSea(const Sea& sea, float bedZ, float t0, float sink, bool carry, int frames)
{
    float rideLast = waterSwimFeetZ(seaAt(sea, t0), t0);
    bool onLast = waterFeetOnRide(rideLast, 0.f, bedZ);
    float z = waterSurfaceFeetZ(rideLast, 0.f, bedZ);
    SeaRun r;
    for (int i = 1; i <= frames; ++i) {
        const float t = t0 + static_cast<float>(i) * kDt;
        const float ride = waterSwimFeetZ(seaAt(sea, t), t);
        const bool on = waterFeetOnRide(ride, sink, bedZ);
        const float want = waterSurfaceFeetZ(ride, sink, bedZ);
        r.maxBeforeStep = std::max(r.maxBeforeStep, std::fabs(z - want));
        const float move = carry && on && onLast ? ride - rideLast : 0.f;
        z = waterFeetCarry(z, move, want, kDt, kRiseEase);
        r.maxFromTarget = std::max(r.maxFromTarget, std::fabs(z - want));
        r.lowest = std::min(r.lowest, z);
        if (i == 1) {
            r.firstGap = std::fabs(z - want);
        }
        r.lastGap = std::fabs(z - want);
        rideLast = ride;
        onLast = on;
    }
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

TEST_CASE("surface feet: on the open sea the ease alone trails a wave and the bob; the carry holds the target")
{
    Sea sea;
    sea.amp = 0.06f; // a 12 cm swell at 4 s
    const float bed = -4.f;
    float oldWorst = 0.f;
    for (int k = 0; k < 13; ++k) {
        const float t0 = static_cast<float>(k) * ftcore::kWaterBobPeriod / 13.f;
        CAPTURE(t0);
        const SeaRun old = swimOnSea(sea, bed, t0, 0.f, false, 240);
        oldWorst = std::max(oldWorst, old.maxFromTarget);
        const SeaRun now = swimOnSea(sea, bed, t0, 0.f, true, 240);
        CHECK(now.maxFromTarget < 1e-3f);
        // The judge reads the feet before the frame's step: one frame of the ride's move, well under 3 cm.
        CHECK(now.maxBeforeStep < 0.01f);
    }
    // The 11-18 open-sea reading (3.3 cm off): the ease alone lags the moving target past the 3 cm line.
    CHECK(oldWorst > 0.03f);
}

TEST_CASE("surface feet: the carry still eases a splash's sink, and the bed floor holds over a shallow swell")
{
    Sea sea;
    sea.amp = 0.06f;
    // A splash's 30 cm sink from the first frame: eased, not jumped, and closed within 1.5 s.
    const SeaRun sunk = swimOnSea(sea, -4.f, 0.7f, -0.3f, true, 90);
    CHECK(sunk.firstGap > 0.25f);
    CHECK(sunk.lastGap < 0.02f);
    // A column just under the draft: the swell's troughs reach the floor. The feet never go under it, and stay within
    // 3 cm of the target as it moves on and off the floor.
    const float bed = sea.level - ftcore::kWaterSwimDraft - kWaterBedFloat + 0.03f;
    for (int k = 0; k < 13; ++k) {
        const float t0 = static_cast<float>(k) * sea.period / 13.f;
        CAPTURE(t0);
        const SeaRun r = swimOnSea(sea, bed, t0, 0.f, true, 240);
        CHECK(r.lowest >= bed + kWaterBedFloat - 1e-4f);
        CHECK(r.maxFromTarget < 0.03f);
    }
}
