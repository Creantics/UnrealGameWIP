// Look-dev switches stay inert unless asked for: fold-layer defaults, the rim term and the -ArenaWet flag.

#include "ftcore/LookReview.h"

#include <doctest/doctest.h>

#include <cmath>

using ftcore::look::findWetReview;
using ftcore::look::rimDirection;
using ftcore::look::rimStrength;
using ftcore::look::rimTerm;

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
    CHECK(rimTerm(0.f, 1.f, 0.f) == 0.f);
    CHECK(rimTerm(0.f, 1.f, std::nanf("")) == 0.f);
}

TEST_CASE("look review: the rim term lives on the silhouette and on the rim side") {
    CHECK(rimTerm(1.f, 1.f, 1.f) == 0.f);                     // facing the camera: none
    CHECK(rimTerm(0.f, 1.f, 1.f) == doctest::Approx(1.f));    // grazing, toward the rim direction: full
    CHECK(rimTerm(0.f, -1.f, 1.f) == doctest::Approx(0.f));   // grazing, away from it: none
    CHECK(rimTerm(0.f, 0.f, 2.f) == doctest::Approx(1.f));    // half side weight times strength 2
    CHECK(rimTerm(0.5f, 1.f, 1.f) < 0.07f);                   // 60 degrees off the view: about 6%
    CHECK(rimTerm(0.8f, 1.f, 1.f) < 0.002f);                  // the body's interior stays dark
    CHECK(rimTerm(-0.3f, 1.f, 1.f) == doctest::Approx(1.f));  // a back-facing normal clamps to grazing
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
