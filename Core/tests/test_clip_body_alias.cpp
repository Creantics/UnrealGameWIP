#include <doctest/doctest.h>

#include "AllocCounter.h"
#include "TestAssets.h"
#include "ftcore/ClipBodyAlias.h"

#include <string>
#include <vector>

using namespace ftcore;

namespace {

bool contains(const std::string& text, const char* word) { return text.find(word) != std::string::npos; }

bool load(const char* json, ClipBodyAliases& t, std::string& error) {
    std::vector<std::string> warnings;
    return importClipBodyAliases(json, t, &error, &warnings);
}

} // namespace

TEST_CASE("clip body aliases: Data/Anim/clip_body_aliases.json gives each Real4 lead body its Real3 clip set") {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Anim/clip_body_aliases.json", text));
    ClipBodyAliases t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importClipBodyAliases(text, t, &error, &warnings), error);
    CHECK(warnings.empty());
    const char* leads[] = {"Devin", "Duy", "Third"};
    for (const char* lead : leads) {
        CAPTURE(lead);
        const std::string r4 = std::string("SKM_MHC_") + lead + "Real4_BodyMesh";
        const std::string r3 = std::string("SKM_MHC_") + lead + "Real3_BodyMesh";
        CHECK(t.resolve(r4) == r3);
        CHECK(t.resolve(r3) == r3); // a body with its own clips stays itself
    }
}

TEST_CASE("clip body aliases: resolve follows chains, ignores case, leaves unknown bodies alone, allocates nothing") {
    ClipBodyAliases t;
    std::string error;
    REQUIRE_MESSAGE(load(R"({"version": 1, "aliases": {"B5": "B4", "B4": "B3"}})", t, error), error);
    CHECK(t.resolve("B5") == "B3");
    CHECK(t.resolve("b4") == "B3");
    CHECK(t.resolve("Other") == "Other");
    const auto before = ftcore_test::allocStats().count;
    std::string_view r;
    for (int i = 0; i < 100; ++i) r = t.resolve("B5");
    CHECK(r == "B3");
    CHECK(ftcore_test::allocStats().count == before);
}

TEST_CASE("clip body aliases: bad tables are refused whole, with the path named") {
    ClipBodyAliases t;
    std::string error;
    CHECK_FALSE(load(R"({"version": 2, "aliases": {}})", t, error));
    CHECK(contains(error, "version"));
    CHECK_FALSE(load(R"({"version": 1, "aliases": {"A": "A"}})", t, error));
    CHECK(contains(error, "aliases.A"));
    CHECK_FALSE(load(R"({"version": 1, "aliases": {"A": "B", "B": "A"}})", t, error));
    CHECK(contains(error, "loops"));
    CHECK_FALSE(load(R"({"version": 1, "aliases": {"A": "B", "a": "C"}})", t, error));
    CHECK(contains(error, "twice"));
    CHECK_FALSE(load(R"({"version": 1, "aliases": {"A": 3}})", t, error));
    CHECK_FALSE(load(R"({"version": 1, "aliases": {"A/B": "C"}})", t, error));
    CHECK_FALSE(load(R"({"version": 1, "aliases": {"A": "B"}, )", t, error));
    CHECK(t.entries.empty());
    std::vector<std::string> warnings;
    CHECK(importClipBodyAliases(R"({"version": 1, "aliases": {}, "extra": 1})", t, &error, &warnings));
    CHECK(warnings.size() == 1);
}
