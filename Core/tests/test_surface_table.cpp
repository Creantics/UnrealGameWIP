// The world surface library: Data/Field/surfaces.json (loader), what each world kind's roles wear, and that the
// committed file dresses every role of a town, and the ground, road and rock of the other kinds, in a CC0 surface at
// its real scale.

#include "ftcore/SurfaceTable.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr const char* kSurfaces = R"J({
  "version": 1,
  "surfaces": [
    { "id": "tiles", "tileCm": 250, "res": "2k", "source": "https://example.org/a/tiles", "license": "CC0" },
    { "id": "bark", "tileCm": 100, "res": "1k", "roughness": 0.9, "source": "https://example.org/a/bark", "license": "CC0" },
    { "id": "grass", "tileCm": 251.5, "source": "https://example.org/a/grass", "license": "CC0" }
  ],
  "kinds": {
    "town": { "roof": { "surface": "tiles", "tint": [1, 0.5, 0.25] }, "wood": { "surface": "bark" },
              "ground": { "surface": "grass" } },
    "ruins": { "ground": { "surface": "grass", "tint": [0.8, 0.8, 0.8] } }
  }
})J";

// The minimal valid file with `role` set to `use` (an object's text) under kind `kind`.
std::string withUse(const std::string& kind, const std::string& role, const std::string& use) {
    return R"({"surfaces":[{"id":"a","tileCm":200,"source":"https://example.org/a","license":"CC0"}],"kinds":{")" + kind +
           R"(":{")" + role + R"(":)" + use + "}}}";
}

} // namespace

TEST_CASE("surfaces: role names round-trip") {
    for (std::size_t i = 0; i < kSurfaceRoleCount; ++i) {
        const SurfaceRole role = static_cast<SurfaceRole>(i);
        SurfaceRole back{};
        REQUIRE(parseSurfaceRole(surfaceRoleName(role), back));
        CHECK(back == role);
    }
    SurfaceRole r{};
    CHECK_FALSE(parseSurfaceRole("roofs", r));
    CHECK_FALSE(parseSurfaceRole("", r));
    CHECK(surfaceRoleName(SurfaceRole::Landmark) == "landmark");
}

TEST_CASE("surfaces: the loader reads the library and what each kind's roles wear") {
    SurfaceTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importSurfaceTable(kSurfaces, t, &error, &warnings), error);
    CHECK(warnings.empty());
    REQUIRE(t.surfaces.size() == 3);
    CHECK(t.find("bark") == 1);
    CHECK(t.find("stone") == -1);
    CHECK(t.surfaces[0].resK == 2);
    CHECK(t.surfaces[1].resK == 1);
    CHECK(t.surfaces[2].resK == 2); // absent: 2k
    CHECK(t.surfaces[2].tileCm == doctest::Approx(251.5f));
    CHECK(t.surfaces[1].source == "https://example.org/a/bark");
    CHECK(t.surfaces[1].roughness == doctest::Approx(0.9f));
    CHECK(t.surfaces[0].roughness < 0.f); // absent: the packed map's

    const SurfaceSpec* roof = t.surfaceFor(WorldKind::Town, SurfaceRole::Roof);
    REQUIRE(roof != nullptr);
    CHECK(roof->id == "tiles");
    CHECK(t.use(WorldKind::Town, SurfaceRole::Roof).tint[1] == doctest::Approx(0.5f));
    CHECK(t.use(WorldKind::Town, SurfaceRole::Wood).tint[0] == doctest::Approx(1.f)); // absent: white
    CHECK(t.surfaceFor(WorldKind::Ruins, SurfaceRole::Ground)->id == "grass");
    CHECK(t.use(WorldKind::Ruins, SurfaceRole::Ground).tint[2] == doctest::Approx(0.8f));
    // Roles and kinds the file doesn't list keep the builder's tint.
    CHECK(t.surfaceFor(WorldKind::Town, SurfaceRole::Building) == nullptr);
    CHECK(t.use(WorldKind::Town, SurfaceRole::Building).surface == -1);
    CHECK(t.surfaceFor(WorldKind::Towers, SurfaceRole::Ground) == nullptr);
}

TEST_CASE("surfaces: a kind may dress only the roles its ground draws") {
    SurfaceTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importSurfaceTable(
                        R"({"surfaces":[{"id":"gravel","tileCm":200,"source":"https://example.org/gravel","license":"CC0"},)"
                        R"({"id":"tar","tileCm":210,"source":"https://example.org/tar","license":"CC0"},)"
                        R"({"id":"slate","tileCm":242,"source":"https://example.org/slate","license":"CC0"}],)"
                        R"("kinds":{"towers":{"ground":{"surface":"gravel"},"road":{"surface":"tar"},)"
                        R"("rock":{"surface":"slate","tint":[0.9,0.9,1]}}}})",
                        t, &error, &warnings),
                    error);
    CHECK(warnings.empty());
    CHECK(t.surfaceFor(WorldKind::Towers, SurfaceRole::Ground)->id == "gravel");
    CHECK(t.surfaceFor(WorldKind::Towers, SurfaceRole::Road)->id == "tar");
    CHECK(t.surfaceFor(WorldKind::Towers, SurfaceRole::Rock)->id == "slate");
    CHECK(t.use(WorldKind::Towers, SurfaceRole::Rock).tint[2] == doctest::Approx(1.f));
    // The roles it leaves out keep the builder's tint, and so does every role of a kind the file doesn't list.
    for (const SurfaceRole role : {SurfaceRole::Plaza, SurfaceRole::Building, SurfaceRole::Roof, SurfaceRole::Landmark,
                                   SurfaceRole::Wood})
        CHECK(t.use(WorldKind::Towers, role).surface == -1);
    for (std::size_t r = 0; r < kSurfaceRoleCount; ++r)
        CHECK(t.surfaceFor(WorldKind::Town, static_cast<SurfaceRole>(r)) == nullptr);
}

TEST_CASE("surfaces: the loader refuses bad files and warns on unknown keys") {
    SurfaceTable t;
    std::string error;
    CHECK_FALSE(importSurfaceTable("{", t, &error));
    CHECK_FALSE(importSurfaceTable("[]", t, &error));
    CHECK_FALSE(importSurfaceTable(R"({"surfaces":[]})", t, &error));
    const std::string good = R"({"id":"a","tileCm":200,"source":"https://example.org/a","license":"CC0"})";
    const auto lib = [](const std::string& entries) { return R"({"surfaces":[)" + entries + "]}"; };
    REQUIRE_MESSAGE(importSurfaceTable(lib(good), t, &error), error);
    CHECK_FALSE(importSurfaceTable(R"({"version":2,"surfaces":[)" + good + "]}", t, &error));
    // id, duplicate, tile size, resolution, source, licence
    CHECK_FALSE(importSurfaceTable(lib(R"({"id":"A b","tileCm":200,"source":"https://example.org/a","license":"CC0"})"), t, &error));
    CHECK_FALSE(importSurfaceTable(lib(good + "," + good), t, &error));
    CHECK(error.find("duplicate") != std::string::npos);
    CHECK_FALSE(importSurfaceTable(lib(R"({"id":"a","tileCm":5,"source":"https://example.org/a","license":"CC0"})"), t, &error));
    CHECK_FALSE(importSurfaceTable(lib(R"({"id":"a","source":"https://example.org/a","license":"CC0"})"), t, &error));
    CHECK_FALSE(
        importSurfaceTable(lib(R"({"id":"a","tileCm":200,"res":"8k","source":"https://example.org/a","license":"CC0"})"), t, &error));
    CHECK_FALSE(importSurfaceTable(lib(R"({"id":"a","tileCm":200,"source":"http://example.org/a","license":"CC0"})"), t, &error));
    CHECK_FALSE(importSurfaceTable(lib(R"({"id":"a","tileCm":200,"source":"https://example.org/a","license":"CC-BY"})"), t, &error));
    CHECK(error.find("surfaces[0].license") != std::string::npos);
    CHECK(t.surfaces.empty());
    // roughness: 0..1 when present
    REQUIRE_MESSAGE(
        importSurfaceTable(lib(R"({"id":"a","tileCm":200,"roughness":0,"source":"https://example.org/a","license":"CC0"})"), t, &error),
        error);
    CHECK(t.surfaces[0].roughness == doctest::Approx(0.f));
    CHECK_FALSE(importSurfaceTable(
        lib(R"({"id":"a","tileCm":200,"roughness":1.5,"source":"https://example.org/a","license":"CC0"})"), t, &error));
    CHECK(error.find("surfaces[0].roughness") != std::string::npos);
    CHECK_FALSE(importSurfaceTable(
        lib(R"({"id":"a","tileCm":200,"roughness":-0.1,"source":"https://example.org/a","license":"CC0"})"), t, &error));
    CHECK_FALSE(importSurfaceTable(
        lib(R"({"id":"a","tileCm":200,"roughness":"matte","source":"https://example.org/a","license":"CC0"})"), t, &error));
    // kinds: an unknown kind or role, a surface not in the library, a bad tint
    REQUIRE_MESSAGE(importSurfaceTable(withUse("town", "roof", R"({"surface":"a"})"), t, &error), error);
    CHECK_FALSE(importSurfaceTable(withUse("city", "roof", R"({"surface":"a"})"), t, &error));
    CHECK_FALSE(importSurfaceTable(withUse("town", "roofs", R"({"surface":"a"})"), t, &error));
    CHECK(error.find("kinds.town.roofs") != std::string::npos);
    CHECK_FALSE(importSurfaceTable(withUse("town", "roof", R"({"surface":"b"})"), t, &error));
    CHECK_FALSE(importSurfaceTable(withUse("town", "roof", R"({"surface":"a","tint":[1,1]})"), t, &error));
    CHECK_FALSE(importSurfaceTable(withUse("town", "roof", R"({"surface":"a","tint":[1,1,3]})"), t, &error));
    CHECK_FALSE(importSurfaceTable(withUse("town", "roof", R"({"surface":"a","tint":"red"})"), t, &error));
    CHECK_FALSE(importSurfaceTable(withUse("town", "roof", R"("a")"), t, &error));
    // unknown keys at the top, in a surface and in a use
    std::vector<std::string> warnings;
    REQUIRE(importSurfaceTable(
        R"({"x":1,"surfaces":[{"id":"a","tileCm":200,"source":"https://example.org/a","license":"CC0","y":2}],)"
        R"("kinds":{"town":{"roof":{"surface":"a","z":3}}}})",
        t, &error, &warnings));
    CHECK(warnings.size() == 3);
}

TEST_CASE("surfaces: lookups allocate nothing") {
    SurfaceTable t;
    REQUIRE(importSurfaceTable(kSurfaces, t));
    int found = 0;
    const std::uint64_t before = ftcore_test::allocStats().count;
    for (std::size_t k = 0; k < kWorldKindCount; ++k)
        for (std::size_t r = 0; r < kSurfaceRoleCount; ++r)
            found += t.surfaceFor(static_cast<WorldKind>(k), static_cast<SurfaceRole>(r)) != nullptr ? 1 : 0;
    found += t.find("grass");
    found += static_cast<int>(surfaceRoleName(SurfaceRole::Rock).size());
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(found == 4 + 2 + 4);
}

TEST_CASE("surfaces: Data/Field/surfaces.json dresses every town role, and every world's ground, in a CC0 surface at "
          "its real scale") {
    std::string text;
    if (!ftcore_test::readProjectData("Field/surfaces.json", text)) {
        MESSAGE("Field/surfaces.json not readable; skipping");
        return;
    }
    SurfaceTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importSurfaceTable(text, t, &error, &warnings), error);
    CHECK(warnings.empty());
    const auto dressed = [&t](WorldKind kind, SurfaceRole role) {
        const SurfaceSpec* s = t.surfaceFor(kind, role);
        CHECK_MESSAGE(s != nullptr, surfaceRoleName(role));
        if (s == nullptr) return;
        // A photographed surface covers somewhere between a hand's width and a few metres.
        CHECK(s->tileCm >= 50.f);
        CHECK(s->tileCm <= 500.f);
    };
    for (std::size_t r = 0; r < kSurfaceRoleCount; ++r) dressed(WorldKind::Town, static_cast<SurfaceRole>(r));
    // The ground material draws each world's ground, its roads and its steep ground.
    for (const WorldKind kind : {WorldKind::Towers, WorldKind::Ruins})
        for (const SurfaceRole role : {SurfaceRole::Ground, SurfaceRole::Road, SurfaceRole::Rock}) dressed(kind, role);
    // Every surface in the library is worn somewhere.
    for (std::size_t i = 0; i < t.surfaces.size(); ++i) {
        bool worn = false;
        for (const auto& kind : t.uses)
            for (const SurfaceUse& use : kind) worn = worn || use.surface == static_cast<int>(i);
        CHECK_MESSAGE(worn, t.surfaces[i].id);
    }
}
