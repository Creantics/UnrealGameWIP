// The hair card spec: Data/Grooms/hair_cards.json (loader), and that the committed file gives every groom a level 0
// and keeps the cards it builds next to their groom, in the local MetaHuman folders.

#include "ftcore/HairCardSpec.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr const char* kSpec = R"J({
  "version": 1,
  "grooms": [
    { "id": "lead_hair", "groom": "/Game/Characters/Lead/MH/Grooms/lead_hair",
      "out": "/Game/Characters/Lead/MH/Grooms/Cards", "seed": 7,
      "lods": [
        { "lod": 0, "cards": 2000, "triangles": 40000, "textures": 75, "atlas": 4096 },
        { "lod": 1, "cards": 1000, "triangles": 16000, "textures": 50, "atlas": 2048, "screenSize": 0.5, "flyaways": 4 },
        { "lod": 3, "cards": 400, "triangles": 6000, "textures": 30, "atlas": 1024, "screenSize": 0.125, "useCards": false }
      ] },
    { "id": "lead_brows", "groom": "/Game/Characters/Lead/MH/Game/lead-brows_2", "out": "/Game/Characters/Lead/MH/Game/Cards",
      "group": 1, "lods": [ { "lod": 0, "cards": 300, "triangles": 600, "textures": 20, "atlas": 1024 } ] }
  ]
})J";

constexpr const char* kLevel = R"({"lod":0,"cards":10,"triangles":100,"textures":5,"atlas":1024})";

// A file of these groom entries.
std::string grooms(const std::string& entries) { return R"({"grooms":[)" + entries + "]}"; }

// One groom entry with these levels (each an object's text) and `more` (members, each with its leading comma).
std::string groom(const std::string& id, const std::string& asset, const std::string& out, const std::string& more,
                  const std::string& levels) {
    return R"({"id":")" + id + R"(","groom":")" + asset + R"(","out":")" + out + "\"" + more + R"(,"lods":[)" +
           levels + "]}";
}

// The minimal valid file with these levels as its one groom's.
std::string withLevels(const std::string& levels) { return grooms(groom("a", "/Game/G/a", "/Game/G/Cards", "", levels)); }

// The minimal valid file with `more` added to its one groom.
std::string withGroomKeys(const std::string& more) {
    return grooms(groom("a", "/Game/G/a", "/Game/G/Cards", more, kLevel));
}

// A level with these members (each with its leading comma) after a valid level 0's.
std::string level(const std::string& lod, const std::string& more) {
    return R"({"lod":)" + lod + R"(,"cards":10,"triangles":100,"textures":5,"atlas":1024)" + more + "}";
}

} // namespace

TEST_CASE("hair cards: atlas sizes map to the generator's setting") {
    CHECK(hairCardAtlasLog2(1024) == 10);
    CHECK(hairCardAtlasLog2(2048) == 11);
    CHECK(hairCardAtlasLog2(4096) == 12);
    CHECK(hairCardAtlasLog2(8192) == 13);
    CHECK(hairCardAtlasLog2(512) == -1);
    CHECK(hairCardAtlasLog2(16384) == -1);
    CHECK(hairCardAtlasLog2(3000) == -1);
    CHECK(hairCardAtlasLog2(0) == -1);
    CHECK(hairCardAtlasLog2(-1024) == -1);
}

TEST_CASE("hair cards: the loader reads each groom and its levels") {
    HairCardSpec s;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importHairCardSpec(kSpec, s, &error, &warnings), error);
    CHECK(warnings.empty());
    REQUIRE(s.grooms.size() == 2);
    CHECK(s.find("lead_brows") == 1);
    CHECK(s.find("lead") == -1);

    const HairCardGroom& hair = s.grooms[0];
    CHECK(hair.id == "lead_hair");
    CHECK(hair.groom == "/Game/Characters/Lead/MH/Grooms/lead_hair");
    CHECK(hair.out == "/Game/Characters/Lead/MH/Grooms/Cards");
    CHECK(hair.group == 0); // absent: 0
    CHECK(hair.seed == 7);
    REQUIRE(hair.lods.size() == 3);
    CHECK(hair.lods[0].lod == 0);
    CHECK(hair.lods[0].cards == 2000);
    CHECK(hair.lods[0].triangles == 40000);
    CHECK(hair.lods[0].textures == 75);
    CHECK(hair.lods[0].atlas == 4096);
    CHECK(hair.lods[0].flyaways == 10);     // absent: 10
    CHECK(hair.lods[0].screenSize < 0.f);   // absent: the groom's own
    CHECK(hair.lods[0].useCards);           // absent: true
    CHECK(hair.lods[1].screenSize == doctest::Approx(0.5f));
    CHECK(hair.lods[1].flyaways == 4);
    CHECK(hair.lods[2].lod == 3); // levels may skip one
    CHECK(hair.lods[2].screenSize == doctest::Approx(0.125f));
    CHECK_FALSE(hair.lods[2].useCards);

    const HairCardGroom& brows = s.grooms[1];
    CHECK(brows.groom == "/Game/Characters/Lead/MH/Game/lead-brows_2");
    CHECK(brows.group == 1);
    CHECK(brows.seed == 0); // absent: 0
    REQUIRE(brows.lods.size() == 1);
    CHECK(brows.lods[0].triangles == 600); // exactly two per card
}

TEST_CASE("hair cards: the loader refuses bad files and warns on unknown keys") {
    HairCardSpec s;
    std::string error;
    CHECK_FALSE(importHairCardSpec("{", s, &error));
    CHECK_FALSE(importHairCardSpec("[]", s, &error));
    CHECK_FALSE(importHairCardSpec(R"({"grooms":[]})", s, &error));
    CHECK_FALSE(importHairCardSpec(grooms("1"), s, &error));
    REQUIRE_MESSAGE(importHairCardSpec(withLevels(kLevel), s, &error), error);
    CHECK_FALSE(importHairCardSpec(R"({"version":2,"grooms":[)" + groom("a", "/Game/G/a", "/Game/G/Cards", "", kLevel) + "]}",
                                   s, &error));

    // ids: lower-case, unique
    CHECK_FALSE(importHairCardSpec(grooms(groom("A b", "/Game/G/a", "/Game/G/Cards", "", kLevel)), s, &error));
    CHECK_FALSE(importHairCardSpec(grooms(groom("", "/Game/G/a", "/Game/G/Cards", "", kLevel)), s, &error));
    CHECK_FALSE(importHairCardSpec(grooms(groom("a", "/Game/G/a", "/Game/G/Cards", "", kLevel) + "," +
                                          groom("a", "/Game/G/b", "/Game/G/Cards", "", kLevel)),
                                   s, &error));
    CHECK(error.find("duplicate") != std::string::npos);
    // a groom is listed once
    CHECK_FALSE(importHairCardSpec(grooms(groom("a", "/Game/G/a", "/Game/G/Cards", "", kLevel) + "," +
                                          groom("b", "/Game/G/a", "/Game/G/Cards", "", kLevel)),
                                   s, &error));
    CHECK(error.find("grooms[1].groom: listed twice") != std::string::npos);

    // paths: under /Game/, single slashes, no dots, spaces or slash at the end
    for (const char* bad : {"/Game/", "/Game/G/", "/Game//a", "/Game/G/../a", "/Game/G/a.a", "/Game/G/a b", "/Engine/G/a",
                            "Game/G/a", ""}) {
        CHECK_FALSE_MESSAGE(importHairCardSpec(grooms(groom("a", bad, "/Game/G/Cards", "", kLevel)), s, &error), bad);
        CHECK_FALSE_MESSAGE(importHairCardSpec(grooms(groom("a", "/Game/G/a", bad, "", kLevel)), s, &error), bad);
    }
    CHECK(error.find("grooms[0].out") != std::string::npos);
    REQUIRE_MESSAGE(importHairCardSpec(grooms(groom("a", "/Game/G-2/a_b", "/Game/G-2/Cards", "", kLevel)), s, &error), error);

    // group and seed
    REQUIRE_MESSAGE(importHairCardSpec(withGroomKeys(R"(,"group":63,"seed":1000000)"), s, &error), error);
    CHECK(s.grooms[0].group == 63);
    CHECK_FALSE(importHairCardSpec(withGroomKeys(R"(,"group":-1)"), s, &error));
    CHECK_FALSE(importHairCardSpec(withGroomKeys(R"(,"group":64)"), s, &error));
    CHECK_FALSE(importHairCardSpec(withGroomKeys(R"(,"group":"1")"), s, &error));
    CHECK_FALSE(importHairCardSpec(withGroomKeys(R"(,"seed":1.5)"), s, &error));
    CHECK_FALSE(importHairCardSpec(withGroomKeys(R"(,"seed":1000001)"), s, &error));
    CHECK(error.find("grooms[0].seed") != std::string::npos);

    // levels: one to eight objects
    CHECK_FALSE(importHairCardSpec(R"({"grooms":[{"id":"a","groom":"/Game/G/a","out":"/Game/G/Cards"}]})", s, &error));
    CHECK_FALSE(importHairCardSpec(withLevels(""), s, &error));
    CHECK_FALSE(importHairCardSpec(withLevels("1"), s, &error));
    std::string eight = level("0", ""), nine;
    for (int l = 1; l < 8; ++l) eight += "," + level(std::to_string(l), "");
    nine = eight + "," + level("8", "");
    REQUIRE_MESSAGE(importHairCardSpec(withLevels(eight), s, &error), error);
    CHECK(s.grooms[0].lods.back().lod == 7);
    CHECK_FALSE(importHairCardSpec(withLevels(nine), s, &error));

    // each level's numbers, within the generator's limits
    CHECK_FALSE(importHairCardSpec(withLevels(R"({"cards":10,"triangles":100,"textures":5,"atlas":1024})"), s, &error));
    CHECK_FALSE(importHairCardSpec(withLevels(level("-1", "")), s, &error));
    CHECK_FALSE(importHairCardSpec(withLevels(level("8", "")), s, &error));
    CHECK_FALSE(importHairCardSpec(withLevels(R"({"lod":0,"triangles":100,"textures":5,"atlas":1024})"), s, &error));
    for (const char* bad : {"0", "100001", "2.5", "\"10\""}) {
        CHECK_FALSE_MESSAGE(importHairCardSpec(withLevels(R"({"lod":0,"cards":)" + std::string(bad) +
                                                          R"(,"triangles":300000,"textures":5,"atlas":1024})"),
                                               s, &error),
                            bad);
    }
    CHECK_FALSE(importHairCardSpec(withLevels(R"({"lod":0,"cards":1,"triangles":1,"textures":5,"atlas":1024})"), s, &error));
    CHECK_FALSE(importHairCardSpec(withLevels(R"({"lod":0,"cards":1,"triangles":1000001,"textures":5,"atlas":1024})"), s, &error));
    CHECK_FALSE(importHairCardSpec(withLevels(R"({"lod":0,"cards":10,"triangles":100,"textures":0,"atlas":1024})"), s, &error));
    CHECK_FALSE(importHairCardSpec(withLevels(R"({"lod":0,"cards":10,"triangles":100,"textures":301,"atlas":1024})"), s, &error));
    CHECK_FALSE(importHairCardSpec(withLevels(R"({"lod":0,"cards":10,"triangles":100,"atlas":1024})"), s, &error));
    REQUIRE_MESSAGE(importHairCardSpec(withLevels(level("0", R"(,"flyaways":0)")), s, &error), error);
    CHECK(s.grooms[0].lods[0].flyaways == 0);
    CHECK_FALSE(importHairCardSpec(withLevels(level("0", R"(,"flyaways":-1)")), s, &error));
    CHECK_FALSE(importHairCardSpec(withLevels(level("0", R"(,"flyaways":1001)")), s, &error));
    // at least two triangles per card
    CHECK_FALSE(importHairCardSpec(withLevels(R"({"lod":0,"cards":10,"triangles":19,"textures":5,"atlas":1024})"), s, &error));
    CHECK(error.find("lods[0].triangles: expected at least two per card") != std::string::npos);
    REQUIRE_MESSAGE(importHairCardSpec(withLevels(R"({"lod":0,"cards":10,"triangles":20,"textures":5,"atlas":1024})"), s, &error),
                    error);
    // atlas sizes the generator has
    for (const char* bad : {"512", "3000", "16384", "\"4096\"", "4096.5"}) {
        CHECK_FALSE_MESSAGE(importHairCardSpec(withLevels(R"({"lod":0,"cards":10,"triangles":100,"textures":5,"atlas":)" +
                                                          std::string(bad) + "}"),
                                               s, &error),
                            bad);
    }
    CHECK_FALSE(importHairCardSpec(withLevels(R"({"lod":0,"cards":10,"triangles":100,"textures":5})"), s, &error));
    // screen size and useCards
    REQUIRE_MESSAGE(importHairCardSpec(withLevels(level("0", R"(,"screenSize":1,"useCards":false)")), s, &error), error);
    CHECK(s.grooms[0].lods[0].screenSize == doctest::Approx(1.f));
    CHECK_FALSE(s.grooms[0].lods[0].useCards);
    for (const char* bad : {"0", "1.5", "-0.5", "\"half\""}) {
        CHECK_FALSE_MESSAGE(importHairCardSpec(withLevels(level("0", R"(,"screenSize":)" + std::string(bad))), s, &error), bad);
    }
    CHECK_FALSE(importHairCardSpec(withLevels(level("0", R"(,"useCards":"yes")")), s, &error));

    // from level to level: increasing levels, nothing grows, screen sizes shrink
    CHECK_FALSE(importHairCardSpec(withLevels(level("0", "") + "," + level("0", "")), s, &error));
    CHECK(error.find("lods[1].lod: expected levels in increasing order") != std::string::npos);
    CHECK_FALSE(importHairCardSpec(withLevels(level("1", "") + "," + level("0", "")), s, &error));
    REQUIRE_MESSAGE(importHairCardSpec(withLevels(level("0", "") + "," + level("2", "")), s, &error), error); // equal is fine
    const std::string big0 = R"({"lod":0,"cards":100,"triangles":1000,"textures":50,"atlas":2048})";
    for (const char* grown : {R"({"lod":1,"cards":101,"triangles":1000,"textures":50,"atlas":2048})",
                              R"({"lod":1,"cards":100,"triangles":1001,"textures":50,"atlas":2048})",
                              R"({"lod":1,"cards":100,"triangles":1000,"textures":51,"atlas":2048})",
                              R"({"lod":1,"cards":100,"triangles":1000,"textures":50,"atlas":4096})"}) {
        CHECK_FALSE_MESSAGE(importHairCardSpec(withLevels(big0 + "," + grown), s, &error), grown);
        CHECK(error.find("lods[1].") != std::string::npos);
    }
    REQUIRE_MESSAGE(importHairCardSpec(withLevels(level("0", R"(,"screenSize":0.5)") + "," + level("1", "") + "," +
                                                  level("2", R"(,"screenSize":0.25)")),
                                       s, &error),
                    error);
    CHECK_FALSE(importHairCardSpec(withLevels(level("0", R"(,"screenSize":0.5)") + "," + level("1", "") + "," +
                                              level("2", R"(,"screenSize":0.5)")),
                                   s, &error));
    CHECK(error.find("lods[2].screenSize") != std::string::npos);
    REQUIRE_MESSAGE(importHairCardSpec(withLevels(level("0", "") + "," + level("1", R"(,"screenSize":0.9)")), s, &error),
                    error); // the level before gives none
    CHECK(s.grooms[0].lods[1].screenSize == doctest::Approx(0.9f));
    CHECK_FALSE(importHairCardSpec(
        withLevels(level("0", "") + R"(,{"lod":1,"cards":11,"triangles":100,"textures":5,"atlas":1024})"), s, &error));
    CHECK(error.find("lods[1].cards: expected no more than the level before's 10") != std::string::npos);
    CHECK(s.grooms.empty());

    // unknown keys at the top, in a groom and in a level
    std::vector<std::string> warnings;
    REQUIRE(importHairCardSpec(R"({"x":1,"grooms":[)" + groom("a", "/Game/G/a", "/Game/G/Cards", R"(,"y":2)", level("0", R"(,"z":3)")) +
                                   "]}",
                               s, &error, &warnings));
    CHECK(warnings.size() == 3);
}

TEST_CASE("hair cards: lookups allocate nothing") {
    HairCardSpec s;
    REQUIRE(importHairCardSpec(kSpec, s));
    int found = 0;
    const std::uint64_t before = ftcore_test::allocStats().count;
    found += s.find("lead_brows");
    found += s.find("lead_hair");
    found += s.find("nobody");
    found += hairCardAtlasLog2(s.grooms[0].lods[0].atlas);
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(found == 1 + 0 - 1 + 12);
}

TEST_CASE("hair cards: Data/Grooms/hair_cards.json gives every groom a level 0 and keeps its cards beside it") {
    std::string text;
    if (!ftcore_test::readProjectData("Grooms/hair_cards.json", text)) {
        MESSAGE("Grooms/hair_cards.json not readable; skipping");
        return;
    }
    HairCardSpec s;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importHairCardSpec(text, s, &error, &warnings), error);
    CHECK(warnings.empty());
    for (const HairCardGroom& g : s.grooms) {
        REQUIRE_MESSAGE(!g.lods.empty(), g.id);
        // The nearest level is the one a close-up sees.
        CHECK_MESSAGE(g.lods.front().lod == 0, g.id);
        // 4096 is the most a groom's nearest level needs; 8192 costs four times the memory for close-ups only.
        CHECK_MESSAGE(g.lods.front().atlas <= 4096, g.id);
        // The cards go in a Cards folder beside the groom, inside the MetaHuman content, which stays local.
        CHECK_MESSAGE(g.out == g.groom.substr(0, g.groom.rfind('/')) + "/Cards", g.id);
        CHECK_MESSAGE(g.groom.find("/MH/") != std::string::npos, g.id);
    }
}
