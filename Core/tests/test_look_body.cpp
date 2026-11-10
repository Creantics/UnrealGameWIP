// Which body a look wears: the cut (BodyHide) body only while every covering garment is worn.

#include "ftcore/LookBody.h"

#include <doctest/doctest.h>

using ftcore::look::NeedsFullBody;
using ftcore::look::PieceLeftOff;

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
