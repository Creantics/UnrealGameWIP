// Look-dev switches stay inert unless asked for: fold-layer defaults, the rim term and the -ArenaWet flag.

#include "ftcore/LookReview.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cmath>

using ftcore::look::findWetReview;
using ftcore::look::rimDirection;
using ftcore::look::rimStrength;
using ftcore::look::rimWeight;

TEST_CASE("look review: the fold-layer parameters default to off") {
    CHECK(ftcore::look::kFoldDirtStrength == 0.f);
    CHECK(ftcore::look::kFoldWetRidgeSheen == 0.f);
    CHECK(ftcore::look::kFoldWetCreaseDarken == 0.f);
    CHECK(ftcore::look::kFoldTintDrift == 0.f);
    // Dirt roughness only ever counts times DirtStrength.
    CHECK(ftcore::look::kFoldDirtRoughness * ftcore::look::kFoldDirtStrength == 0.f);
    CHECK(ftcore::look::kFoldDryRidgeRoughness >= 1.f); // min(r, 1) leaves a saturated roughness as it was
}

TEST_CASE("look review: the rim term is off at cvar 0, negative or NaN") {
    static_assert(rimStrength(0.f) == 0.f);
    CHECK(rimStrength(-1.f) == 0.f);
    CHECK(rimStrength(std::nanf("")) == 0.f);
    CHECK(rimStrength(0.25f) == doctest::Approx(0.25f));
    CHECK(rimWeight(0.f, 1.f) * rimStrength(0.f) == 0.f);
    CHECK(rimWeight(0.f, 1.f) * rimStrength(std::nanf("")) == 0.f);
}

TEST_CASE("look review: the rim term lives on the silhouette and on the rim side") {
    CHECK(rimWeight(1.f, 1.f) == 0.f);                                     // facing the camera: none
    CHECK(rimWeight(0.f, 1.f) * rimStrength(2.f) == doctest::Approx(2.f)); // grazing, toward the rim: full times strength
    CHECK(rimWeight(0.8f, 1.f) < 0.001f);                                  // the body's interior stays dark
    // A two-sided garment's back face (a collar's inside, a flipped face) has a vertex normal pointing away from the
    // camera: taken as it is, it clamps to grazing and glows whole. The masters turn N to the face drawn first.
    const float backN = -0.9f;
    CHECK(rimWeight(backN, 0.9f) == doctest::Approx(1.f)); // the fault, as the term alone would see it
    CHECK(rimWeight(-backN, -0.9f) == 0.f);                // the same face, N turned to it: dark
}

TEST_CASE("look review: one rim writer; the arena's figure wins over an explore pawn") {
    using ftcore::look::rimMayWrite;
    CHECK(rimMayWrite(false, false, 0, 10, 0, 0));    // a free slot
    CHECK(rimMayWrite(true, true, 10, 10, 0, 1));     // the holder keeps it
    CHECK_FALSE(rimMayWrite(false, true, 10, 10, 0, 1)); // the pawn never takes it from the arena figure
    CHECK_FALSE(rimMayWrite(false, true, 9, 10, 0, 0));  // nor from a pawn that wrote last frame
    CHECK(rimMayWrite(false, true, 10, 10, 1, 0));    // the arena figure takes it from a pawn
    CHECK(rimMayWrite(false, true, 8, 10, 0, 1));     // a holder that stopped writing frees it
}

TEST_CASE("look review: at ft.Rim 0 the rim tick is a branch and allocates nothing") {
    using ftcore::look::rimIdle;
    static_assert(rimIdle(0.f, 0.f));
    CHECK(rimIdle(rimStrength(-1.f), 0.f));
    CHECK_FALSE(rimIdle(0.f, 0.04f)); // switched off: one write of 0 first
    CHECK_FALSE(rimIdle(0.04f, 0.f));
    CHECK(rimIdle(std::nanf(""), std::nanf("")));
    // The tick's Core part at strength 0, many frames: no heap allocation.
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    float side = 1.f, written = 0.f;
    int idle = 0;
    for (int frame = 0; frame < 1000; ++frame) {
        const float strength = rimStrength(0.f);
        const float target = ftcore::look::rimSideTarget(frame % 2 ? -1.f : 0.f);
        if (rimIdle(strength, written)) {
            side = target;
            ++idle;
            continue;
        }
        written = strength;
    }
    const auto allocs = ftcore_test::allocStats().count - before;
    CHECK(idle == 1000);
    CHECK(side == ftcore::look::rimSideTarget(-1.f)); // snapped to the last frame's target (frame 999)
    if (counting) {
        CHECK(allocs == 0);
    }
    // At strength, the per-frame Core work allocates nothing either.
    const auto before2 = ftcore_test::allocStats().count;
    float acc = 0.f;
    for (int frame = 0; frame < 1000; ++frame) {
        side = ftcore::look::rimSideStep(side, -1.f, 1.f / 60.f);
        const auto l = rimDirection({0.f, 0.f, 170.f}, {500.f, static_cast<float>(frame), 100.f}, side);
        acc += rimWeight(0.1f, l.x);
    }
    CHECK(acc >= 0.f);
    if (counting) {
        CHECK(ftcore_test::allocStats().count - before2 == 0);
    }
}

TEST_CASE("look review: the rim direction is behind the lead, raised 30 degrees, turned 35 to the camera's right") {
    const auto l = rimDirection({0.f, 0.f, 170.f}, {500.f, 0.f, 100.f}); // camera looking along +x
    CHECK(std::sqrt(l.x * l.x + l.y * l.y + l.z * l.z) == doctest::Approx(1.f));
    CHECK(l.z == doctest::Approx(0.5f));                                // sin 30
    CHECK(l.x > 0.f);                                                   // away from the camera
    CHECK(l.y > 0.f);                                                   // to its right (+y)
    CHECK(std::atan2(l.y, l.x) * 180.f / 3.14159265f == doctest::Approx(35.f).epsilon(0.001));
    const auto m = rimDirection({500.f, 300.f, 0.f}, {500.f, 0.f, 0.f}); // camera looking along -y: its right is +x
    CHECK(m.y < 0.f);
    CHECK(m.x > 0.f);
    const auto top = rimDirection({10.f, 20.f, 500.f}, {10.f, 20.f, 0.f}); // straight above: +x bearing
    CHECK(top.x > 0.f);
    CHECK(top.z == doctest::Approx(0.5f));
}

TEST_CASE("look review: the rim weight lights only the edge facing the rim direction") {
    using ftcore::look::rimWeight;
    CHECK(rimWeight(0.f, 1.f) == doctest::Approx(1.f));             // grazing edge, facing the rim: full
    CHECK(rimWeight(1.f, 1.f) == 0.f);                               // facing the camera: none
    CHECK(rimWeight(0.f, 0.1f) == 0.f);                              // at the low side edge: none
    CHECK(rimWeight(0.f, -1.f) == 0.f);                              // the far silhouette: none
    CHECK(rimWeight(0.f, 0.6f) == doctest::Approx(1.f));             // full from the high side edge up
    CHECK(rimWeight(0.f, 0.35f) == doctest::Approx(0.5f));           // smoothstep midpoint
    CHECK(rimWeight(0.5f, 1.f) == doctest::Approx(1.f / 32.f));      // exponent 5
    CHECK(rimWeight(-0.2f, 1.f) == doctest::Approx(1.f));            // back-facing N.V clamps to 0
    CHECK(rimWeight(0.2f, 0.3f) < rimWeight(0.2f, 0.4f));            // rises with N.L
    CHECK(rimWeight(std::nanf(""), 1.f) == 0.f);
    CHECK(rimWeight(0.f, std::nanf("")) == 0.f);
}

TEST_CASE("look review: the lamp side turns the rim to the camera's left, eased over 0.4 s") {
    using ftcore::look::rimSideStep;
    using ftcore::look::rimSideTarget;
    CHECK(rimSideTarget(1.f) == 1.f);
    CHECK(rimSideTarget(0.3f) == 1.f);
    CHECK(rimSideTarget(-0.2f) == -1.f);
    CHECK(rimSideTarget(0.f) == 1.f);           // 0: the default, camera-right
    CHECK(rimSideTarget(std::nanf("")) == 1.f); // NaN: the default
    const auto left = rimDirection({0.f, 0.f, 170.f}, {500.f, 0.f, 100.f}, -1.f);
    CHECK(std::atan2(left.y, left.x) * 180.f / 3.14159265f == doctest::Approx(-35.f).epsilon(0.001));
    CHECK(left.z == doctest::Approx(0.5f));
    const auto mid = rimDirection({0.f, 0.f, 170.f}, {500.f, 0.f, 100.f}, 0.f); // halfway through a flip: straight behind
    CHECK(mid.y == doctest::Approx(0.f));
    const auto odd = rimDirection({0.f, 0.f, 170.f}, {500.f, 0.f, 100.f}, std::nanf("")); // NaN side: the default
    CHECK(std::atan2(odd.y, odd.x) * 180.f / 3.14159265f == doctest::Approx(35.f).epsilon(0.001));
    const auto wide = rimDirection({0.f, 0.f, 170.f}, {500.f, 0.f, 100.f}, -7.f); // clamped
    CHECK(wide.y == doctest::Approx(left.y));
    // A full flip at 60 frames a second takes 0.4 s (24 frames), never jumps more than one frame's step, ends exactly.
    float side = 1.f;
    int frames = 0;
    while (side > -1.f && frames < 1000) {
        const float next = rimSideStep(side, -1.f, 1.f / 60.f);
        CHECK(side - next <= 2.f / 60.f / 0.4f + 1e-5f);
        side = next;
        ++frames;
    }
    CHECK(side == -1.f);
    CHECK(frames >= 23);
    CHECK(frames <= 25);
    CHECK(rimSideStep(-1.f, -1.f, 0.016f) == -1.f);      // at the target, stays
    CHECK(rimSideStep(0.5f, 1.f, 0.f) == 0.5f);          // no time, holds
    CHECK(rimSideStep(0.5f, 1.f, std::nanf("")) == 0.5f); // NaN dt, holds
    CHECK(rimSideStep(std::nanf(""), -1.f, 0.016f) == -1.f); // NaN side restarts at the target
    CHECK(rimSideStep(1.f, -1.f, 10.f) == -1.f);          // a long frame lands on the target, no overshoot
}

TEST_CASE("look review: hair and face take less rim, glasses none") {
    CHECK(ftcore::look::kRimHairScale < ftcore::look::kRimFaceScale);
    CHECK(ftcore::look::kRimFaceScale < 1.f);
    CHECK(ftcore::look::kRimHairCap * ftcore::look::kRimHairScale < 0.6f); // hair rim at most 0.6x its base colour
}

TEST_CASE("look review: no -ArenaWet, no review") {
    CHECK_FALSE(findWetReview("", "ArenaWet").has_value());
    CHECK_FALSE(findWetReview("/Game/Maps/Demo_Arena -game -ArenaLead=Duy -ExploreWet=1,130,0", "ArenaWet").has_value());
    CHECK_FALSE(findWetReview("-NotArenaWet=1,50", "ArenaWet").has_value());
    CHECK_FALSE(findWetReview("x-ArenaWet=1,50", "ArenaWet").has_value());
    CHECK_FALSE(findWetReview("-ArenaWetter=1,50", "ArenaWet").has_value());
    CHECK_FALSE(findWetReview("-ArenaWet", "ArenaWet").has_value());
    CHECK_FALSE(findWetReview("-ArenaWet=1", "").has_value());
}

TEST_CASE("look review: -ArenaWet reads wetness and line, with defaults for parts left out") {
    auto w = findWetReview("-game -ArenaWet=0.6,90 -ArenaSeed=1", "ArenaWet");
    REQUIRE(w.has_value());
    CHECK(w->wetness == doctest::Approx(0.6f));
    CHECK(w->lineCm == doctest::Approx(90.f));

    w = findWetReview("-ArenaWet=0.5", "ArenaWet");
    REQUIRE(w.has_value());
    CHECK(w->wetness == doctest::Approx(0.5f));
    CHECK(w->lineCm == doctest::Approx(130.f));

    w = findWetReview("-ArenaWet=", "ArenaWet");
    REQUIRE(w.has_value());
    CHECK(w->wetness == doctest::Approx(1.f));
    CHECK(w->lineCm == doctest::Approx(130.f));

    w = findWetReview("-ArenaWet=wet,abc", "ArenaWet");
    REQUIRE(w.has_value());
    CHECK(w->wetness == doctest::Approx(1.f));
    CHECK(w->lineCm == doctest::Approx(130.f));
}

TEST_CASE("look review: the dry matte floor holds each family in its band and leaves wet alone") {
    namespace lk = ftcore::look;
    // Old vinyl-gloss leather (about 0.3) comes up into the leather band, by the map's own G.
    CHECK(lk::dryRoughness(0.3f, lk::kDryLeather, lk::kDryMetal, 0.f, 0.f) == doctest::Approx(0.45f));
    CHECK(lk::dryRoughness(0.3f, lk::kDryLeather, lk::kDryMetal, 1.f, 0.f) == doctest::Approx(0.60f));
    CHECK(lk::dryRoughness(0.3f, lk::kDryLeather, lk::kDryMetal, 0.49f, 0.f) > lk::dryRoughness(0.3f, lk::kDryLeather, lk::kDryMetal, 0.2f, 0.f));
    // Rougher than the floor stays as it is; cloth sits in 0.80..0.95.
    CHECK(lk::dryRoughness(0.97f, lk::kDryCloth, lk::kDryMetal, 0.5f, 0.f) == doctest::Approx(0.97f));
    CHECK(lk::dryRoughness(0.5f, lk::kDryCloth, lk::kDryMetal, 0.5f, 0.f) == doctest::Approx(0.875f));
    // Polished hardware (0.28) comes up into the metal band by the metal mask.
    CHECK(lk::dryRoughness(0.28f, lk::kDryCloth, lk::kDryMetal, 0.f, 1.f) == doctest::Approx(0.45f));
    // The bands are the ruling's.
    CHECK(lk::kDryWaxed.lo == doctest::Approx(0.55f));
    CHECK(lk::kDryWaxed.hi == doctest::Approx(0.70f));
    CHECK(lk::dryFloor(lk::kDryLeather, -1.f) == doctest::Approx(0.45f));
    CHECK(lk::dryFloor(lk::kDryLeather, 2.f) == doctest::Approx(0.60f));
}
