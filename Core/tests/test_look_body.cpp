// Which body a look wears: the cut (BodyHide) body only while every covering garment is worn.

#include "ftcore/LookBody.h"

#include <doctest/doctest.h>

using ftcore::look::NeedsFullBody;
using ftcore::look::PieceLeftOff;
using ftcore::look::UnderChoice;
using ftcore::look::UnderLayer;

TEST_CASE("look body: no cover words keeps the look's body") {
    CHECK_FALSE(NeedsFullBody({}, {}));
    CHECK_FALSE(NeedsFullBody({}, {"devin_tunic_b6c"}));
}

TEST_CASE("look body: every covering garment worn keeps the cut body") {
    CHECK_FALSE(NeedsFullBody({"tunic", "trousers"}, {"devin_r3e_trousers_s5", "devin_r4_tunic_b6c", "SKM_Face"}));
}

TEST_CASE("look body: a covering garment off needs the full body") {
    CHECK(NeedsFullBody({"tunic", "trousers"}, {"devin_r3e_trousers_s5", "SKM_Face"}));
    CHECK(NeedsFullBody({"jacket"}, {}));
}

TEST_CASE("look body: pieces left off by the command-line words") {
    CHECK(PieceLeftOff("duy_r4_jacket_k3b_g7c", true, {}));
    CHECK(PieceLeftOff("duy_r4_jacket_k3b_g7c", false, {"hair", "jacket"}));
    CHECK_FALSE(PieceLeftOff("duy_r4_jacket_k3b_g7c", false, {"hair"}));
    CHECK_FALSE(PieceLeftOff("duy_r4_jacket_k3b_g7c", false, {""}));
}

TEST_CASE("look body: words match pieces in any case") {
    CHECK(PieceLeftOff("SKM_Face", false, {"face"}));
    CHECK(PieceLeftOff("skm_face", false, {"FACE"}));
    CHECK_FALSE(NeedsFullBody({"Jacket"}, {"SK_Duy_jacket_r5"}));
    CHECK(NeedsFullBody({"JACKET"}, {"SK_Duy_Tee_r3"}));
}

namespace {
const std::string kTee = "/Game/Characters/Duy/Tee/SK_Duy_Tee.SK_Duy_Tee";
const std::string kTeeOpen = "/Game/Characters/Duy/Tee/SK_Duy_Tee_uJO.SK_Duy_Tee_uJO";
const std::string kTeeClosed = "/Game/Characters/Duy/Tee/SK_Duy_Tee_uJC.SK_Duy_Tee_uJC";
const std::string kJacketOpen = "/Game/Characters/Duy/Jacket/SK_Duy_JacketOpen_rr2.SK_Duy_JacketOpen_rr2";
const std::string kJacketClosed = "/Game/Characters/Duy/Jacket/SK_Duy_Jacket_r5.SK_Duy_Jacket_r5";
const std::string kTrousers = "/Game/Characters/Duy/Trousers/SK_Duy_Trousers.SK_Duy_Trousers";
const std::vector<UnderLayer> kLayers = {{kTee, "JacketOpen_rr2", kTeeOpen}, {kTee, "Jacket_r5", kTeeClosed}};
} // namespace

TEST_CASE("under layer: the cut tee is worn while its jacket is") {
    CHECK(UnderChoice(kTee, kLayers, {kTee, kJacketOpen, kTrousers}) == kTeeOpen);
}

TEST_CASE("under layer: no over piece worn keeps the whole tee") {
    CHECK(UnderChoice(kTee, kLayers, {kTee, kTrousers}) == kTee);
    CHECK(UnderChoice(kTee, kLayers, {}) == kTee);
    CHECK(UnderChoice(kTee, {}, {kTee, kJacketOpen}) == kTee);
}

TEST_CASE("under layer: a cut whose jacket came off goes back to the whole tee") {
    CHECK(UnderChoice(kTeeOpen, kLayers, {kTeeOpen, kTrousers}) == kTee);
    CHECK(UnderChoice(kTeeOpen, kLayers, {kTeeOpen, kJacketOpen}) == kTeeOpen);
}

TEST_CASE("under layer: the open and the closed jacket each pick their own cut") {
    CHECK(UnderChoice(kTee, kLayers, {kTee, kJacketClosed}) == kTeeClosed);
    CHECK(UnderChoice(kTeeOpen, kLayers, {kTeeOpen, kJacketClosed}) == kTeeClosed);
    CHECK(UnderChoice(kTeeClosed, kLayers, {kTeeClosed, kJacketOpen}) == kTeeOpen);
}

TEST_CASE("under layer: the word is matched on asset names only, in any case") {
    const std::string inJacketFolder = "/Game/Characters/Duy/JacketOpen_rr2/SK_Duy_Trousers.SK_Duy_Trousers";
    CHECK(UnderChoice(kTee, kLayers, {kTee, inJacketFolder}) == kTee);
    CHECK(UnderChoice(kTee, {{kTee, "jacketopen_RR2", kTeeOpen}}, {kTee, kJacketOpen}) == kTeeOpen);
    CHECK(UnderChoice("/game/characters/duy/tee/sk_duy_tee.sk_duy_tee", kLayers, {kJacketOpen}) == kTeeOpen);
    CHECK(UnderChoice(kTee, kLayers, {kTee, "SK_Duy_JacketOpen_rr2"}) == kTeeOpen);
}

TEST_CASE("under layer: the under piece and its cuts are never over pieces") {
    const std::vector<UnderLayer> self = {{kTee, "Tee", kTeeOpen}};
    CHECK(UnderChoice(kTee, self, {kTee}) == kTee);
    CHECK(UnderChoice(kTeeOpen, self, {kTeeOpen}) == kTee);
}

TEST_CASE("under layer: empty words and cuts never apply; other pieces pass through") {
    CHECK(UnderChoice(kTee, {{kTee, "", kTeeOpen}}, {kTee, kJacketOpen}) == kTee);
    CHECK(UnderChoice(kTee, {{kTee, "JacketOpen", ""}}, {kTee, kJacketOpen}) == kTee);
    CHECK(UnderChoice(kTrousers, kLayers, {kTrousers, kJacketOpen}) == kTrousers);
    CHECK(UnderChoice("", kLayers, {kJacketOpen}).empty());
}

TEST_CASE("under layer: the first matching layer wins") {
    const std::vector<UnderLayer> both = {{kTee, "Jacket", kTeeClosed}, {kTee, "JacketOpen_rr2", kTeeOpen}};
    CHECK(UnderChoice(kTee, both, {kTee, kJacketOpen}) == kTeeClosed);
}
