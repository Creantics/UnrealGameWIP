// Look-dev switches stay inert unless asked for: fold-layer defaults, the rim light scale and the -ArenaWet flag.

#include "ftcore/LookReview.h"

#include <doctest/doctest.h>

#include <cmath>

using ftcore::look::findWetReview;
using ftcore::look::rimLightScale;
using ftcore::look::rimLightWanted;

TEST_CASE("look review: the fold-layer parameters default to off") {
    CHECK(ftcore::look::kFoldDirtStrength == 0.f);
    CHECK(ftcore::look::kFoldWetRidgeSheen == 0.f);
    CHECK(ftcore::look::kFoldWetCreaseDarken == 0.f);
    CHECK(ftcore::look::kFoldTintDrift == 0.f);
    // Dirt roughness only ever counts times DirtStrength.
    CHECK(ftcore::look::kFoldDirtRoughness * ftcore::look::kFoldDirtStrength == 0.f);
}

TEST_CASE("look review: the rim light is off at cvar 0, negative, NaN, or on a figure that doesn't carry it") {
    static_assert(!rimLightWanted(0.f, true));
    CHECK_FALSE(rimLightWanted(0.f, true));
    CHECK_FALSE(rimLightWanted(-1.f, true));
    CHECK_FALSE(rimLightWanted(std::nanf(""), true));
    CHECK_FALSE(rimLightWanted(0.25f, false));
    CHECK(rimLightScale(0.25f, false) == 0.f);
    CHECK(rimLightWanted(0.25f, true));
    CHECK(rimLightScale(0.25f, true) == doctest::Approx(0.25f));
}

TEST_CASE("look review: the rim light leaves the face piece and glasses off its channel, hair on") {
    using ftcore::look::rimLightSkipsPiece;
    static_assert(!rimLightSkipsPiece("", true));
    CHECK_FALSE(rimLightSkipsPiece("Hair_S_Casual", true)); // hair carries the head's outline
    CHECK(rimLightSkipsPiece("SKM_MHC_DuyReal4_FaceMesh", true));
    CHECK(rimLightSkipsPiece("SKM_MHC_DuyReal4_FaceMesh", false));
    CHECK(rimLightSkipsPiece("SK_Duy_Glasses_b", false));
    CHECK_FALSE(rimLightSkipsPiece("SK_Duy_JacketOpen_rr1", false));
    CHECK_FALSE(rimLightSkipsPiece("SKM_MHC_DuyReal4_BodyMesh", false));
    CHECK_FALSE(rimLightSkipsPiece("", false));
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
