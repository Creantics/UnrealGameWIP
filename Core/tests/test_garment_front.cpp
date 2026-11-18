// Open and closed garments: a look's jacket or coat is worn closed or open (its two fitted meshes).

#include "ftcore/GarmentFront.h"

#include <doctest/doctest.h>

using ftcore::look::Front;
using ftcore::look::FrontAsk;
using ftcore::look::FrontFor;
using ftcore::look::FrontPair;
using ftcore::look::ParseFront;
using ftcore::look::ParseFrontAsks;
using ftcore::look::WornPiece;

namespace {
const std::vector<FrontPair> kPairs = {{"lead_jacket_closed", "lead_jacket_open", Front::Open},
                                       {"lead_coat", "lead_coat_open", Front::Closed}};
}

TEST_CASE("garment front: a piece with no pair is worn as listed in either state") {
    const std::string tee = "lead_tee";
    CHECK(WornPiece(tee, kPairs, {}) == "lead_tee");
    CHECK(WornPiece(tee, kPairs, {{"", Front::Open}}) == "lead_tee");
    CHECK(WornPiece(tee, {}, {{"", Front::Closed}}) == "lead_tee");
}

TEST_CASE("garment front: a pair wears its default unless asked") {
    CHECK(WornPiece("lead_jacket_closed", kPairs, {}) == "lead_jacket_open");
    CHECK(WornPiece("lead_jacket_open", kPairs, {}) == "lead_jacket_open");
    CHECK(WornPiece("lead_coat", kPairs, {}) == "lead_coat");
}

TEST_CASE("garment front: asks pick the state, the last one naming a pair wins") {
    CHECK(WornPiece("lead_jacket_closed", kPairs, {{"", Front::Closed}}) == "lead_jacket_closed");
    CHECK(WornPiece("lead_coat", kPairs, {{"coat", Front::Open}}) == "lead_coat_open");
    CHECK(WornPiece("lead_jacket_open", kPairs, {{"coat", Front::Open}}) == "lead_jacket_open");
    CHECK(FrontFor(kPairs[0], {{"", Front::Closed}, {"jacket", Front::Open}}) == Front::Open);
    CHECK(FrontFor(kPairs[0], {{"jacket", Front::Open}, {"", Front::Closed}}) == Front::Closed);
}

TEST_CASE("garment front: a pair without an open mesh stays closed") {
    const std::vector<FrontPair> pairs = {{"lead_jacket", "", Front::Open}};
    CHECK(WornPiece("lead_jacket", pairs, {{"", Front::Open}}) == "lead_jacket");
}

TEST_CASE("garment front: an open mesh not made yet gives the closed one, whichever half is listed") {
    const std::vector<FrontPair> pairs = {
        {"/Game/Out/SK_Jacket.SK_Jacket", "/Game/Out/SK_Jacket_open.SK_Jacket_open", Front::Open, false}};
    CHECK(WornPiece("/Game/Out/SK_Jacket.SK_Jacket", pairs, {}) == pairs[0].closed);
    CHECK(WornPiece("/Game/Out/SK_Jacket_open.SK_Jacket_open", pairs, {}) == pairs[0].closed);
    CHECK(WornPiece("/Game/Out/SK_Jacket_open", pairs, {{"", Front::Open}}) == pairs[0].closed);
    CHECK(FrontFor(pairs[0], {{"jacket", Front::Open}}) == Front::Closed);
}

TEST_CASE("garment front: short and long object paths match, in any case") {
    using ftcore::look::PiecePathKey;
    using ftcore::look::WornChoice;
    CHECK(PiecePathKey("/Game/A/SK_B.SK_B") == "/game/a/sk_b");
    CHECK(PiecePathKey("/Game/A/SK_B") == "/game/a/sk_b");
    CHECK(PiecePathKey("/Game/A/SK_B.Other") == "/game/a/sk_b.other");
    const std::vector<FrontPair> shortForm = {{"/Game/A/SK_Coat", "/Game/A/SK_Coat_open", Front::Open}};
    CHECK(WornPiece("/Game/A/SK_Coat.SK_Coat", shortForm, {}) == "/Game/A/SK_Coat_open");
    CHECK(WornChoice("/game/a/sk_coat_open.SK_Coat_open", shortForm, {}).pair == 0);
    const std::vector<FrontPair> longForm = {{"/Game/A/SK_Coat.SK_Coat", "/Game/A/SK_Coat_open.SK_Coat_open", Front::Closed}};
    CHECK(WornPiece("/Game/A/SK_Coat_open", longForm, {}) == "/Game/A/SK_Coat.SK_Coat");
    CHECK(WornChoice("/Game/B/SK_Coat", longForm, {}).pair == -1);
}

TEST_CASE("garment front: a word names asset names in any case, not folders; unmatched words are listed") {
    using ftcore::look::AskNames;
    using ftcore::look::UnmatchedAskWords;
    const std::vector<FrontPair> pairs = {{"/Game/Jackets/SK_Duy_Coat", "/Game/Jackets/SK_Duy_Coat_open", Front::Closed}};
    CHECK(AskNames(pairs[0], "COAT"));
    CHECK_FALSE(AskNames(pairs[0], "jackets"));
    CHECK(FrontFor(pairs[0], {{"Coat", Front::Open}}) == Front::Open);
    const auto words = UnmatchedAskWords(pairs, {{"jackets", Front::Open}, {"coat", Front::Open}, {"", Front::Open},
                                                 {"jackets", Front::Closed}});
    REQUIRE(words.size() == 1);
    CHECK(words[0] == "jackets");
}

TEST_CASE("garment front: parsing states and asks") {
    Front f = Front::Closed;
    CHECK(ParseFront("Open", f));
    CHECK(f == Front::Open);
    CHECK(ParseFront("CLOSED", f));
    CHECK(f == Front::Closed);
    CHECK_FALSE(ParseFront("ajar", f));
    CHECK(f == Front::Closed);

    std::vector<FrontAsk> asks;
    CHECK(ParseFrontAsks("open", asks));
    REQUIRE(asks.size() == 1);
    CHECK(asks[0].word.empty());
    CHECK(asks[0].state == Front::Open);
    CHECK(ParseFrontAsks("jacket:closed+coat:open", asks));
    REQUIRE(asks.size() == 3);
    CHECK(asks[1].word == "jacket");
    CHECK(asks[1].state == Front::Closed);
    CHECK(asks[2].word == "coat");
    CHECK(asks[2].state == Front::Open);
    CHECK_FALSE(ParseFrontAsks("jacket:ajar", asks));
    CHECK_FALSE(ParseFrontAsks(":open", asks));
    CHECK_FALSE(ParseFrontAsks("", asks));
    CHECK_FALSE(ParseFrontAsks("open+", asks));
    CHECK(asks.size() == 3);
}
