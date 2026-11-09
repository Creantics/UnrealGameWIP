#include <doctest/doctest.h>

#include "AllocCounter.h"
#include "TestAssets.h"
#include "ftcore/LookTable.h"

#include <string>
#include <vector>

using namespace ftcore;

namespace {

bool contains(const std::string& text, const char* word) { return text.find(word) != std::string::npos; }

} // namespace

TEST_CASE("looks: Data/Look/looks.json loads clean with solhaven, lattice and unwritten, every value in range") {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Look/looks.json", text));
    LookTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importLookTable(text, t, &error, &warnings), error);
    CHECK(warnings.empty());
    CHECK(t.worlds.size() == 3);
    const char* ids[] = {"solhaven", "lattice", "unwritten"};
    for (const char* id : ids) {
        CAPTURE(id);
        const SceneLook* look = t.find(id);
        REQUIRE(look != nullptr);
        for (const SceneLookField& f : kSceneLookFields) {
            CAPTURE(f.name);
            const float v = look->*(f.member);
            CHECK(v >= f.lo);
            CHECK(v <= f.hi);
        }
        // Each world is its own look, not the default.
        CHECK(*look != SceneLook{});
    }
    CHECK(*t.find("solhaven") != *t.find("lattice"));
    CHECK(*t.find("lattice") != *t.find("unwritten"));
    // The engine's ray-traced reflections are on for every world.
    REQUIRE(t.render.size() == 2);
    CHECK(t.render[0].name == "r.Lumen.HardwareRayTracing");
    CHECK(t.render[0].value == 1.f);
    CHECK(t.render[1].name == "r.Lumen.TranslucencyReflections.FrontLayer.Enable");
    CHECK(t.render[1].value == 1.f);
    // Water, wetness, fog albedo, practical lights and glow are set per world.
    CHECK(t.find("solhaven")->wetness > 0.f);
    CHECK(t.find("lattice")->glowStrength > 0.f);
    CHECK(t.find("solhaven")->glowStrength == 0.f);
    CHECK(t.find("unwritten")->waterWaveStrength < 0.5f);
    // The town's street lamps are high-pressure sodium at 2000 K; its other practicals, interior lamps, are 2700 K.
    CHECK(t.find("solhaven")->sodiumKelvin == 2000.f);
    CHECK(t.find("solhaven")->practicalKelvin == 2700.f);
}

TEST_CASE("looks: a render block parses in order, and no block means none, with no warning") {
    LookTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importLookTable(R"({"version":1,"render":{"r.B.Two":2.5,"r.A.One":-1,"r.Z_9":0},"worlds":{"w":{}}})", t,
                                    &error, &warnings),
                    error);
    CHECK(warnings.empty());
    REQUIRE(t.render.size() == 3);
    CHECK(t.render[0].name == "r.B.Two");
    CHECK(t.render[0].value == 2.5f);
    CHECK(t.render[1].name == "r.A.One");
    CHECK(t.render[1].value == -1.f);
    CHECK(t.render[2].name == "r.Z_9");
    CHECK(t.render[2].value == 0.f);

    warnings.clear();
    REQUIRE(importLookTable(R"({"version":1,"render":{},"worlds":{"w":{}}})", t, &error, &warnings));
    CHECK(t.render.empty());
    CHECK(warnings.empty());
    REQUIRE(importLookTable(R"({"version":1,"worlds":{"w":{}}})", t, &error, &warnings));
    CHECK(t.render.empty());
    CHECK(warnings.empty());
}

TEST_CASE("looks: a bad render key or value refuses the table, naming render.<key>") {
    LookTable t;
    std::string error;
    CHECK_FALSE(importLookTable(R"({"version":1,"render":{"Lumen.X":1},"worlds":{"w":{}}})", t, &error));
    CHECK(contains(error, "render.Lumen.X"));
    CHECK(t.render.empty());
    CHECK_FALSE(importLookTable(R"({"version":1,"render":{"r.":1},"worlds":{"w":{}}})", t, &error));
    CHECK(contains(error, "render.r."));
    CHECK_FALSE(importLookTable(R"({"version":1,"render":{"r.Bad Name":1},"worlds":{"w":{}}})", t, &error));
    CHECK(contains(error, "render.r.Bad Name"));
    CHECK_FALSE(importLookTable(R"({"version":1,"render":{"r.A-B":1},"worlds":{"w":{}}})", t, &error));
    CHECK(contains(error, "render.r.A-B"));
    CHECK_FALSE(importLookTable(R"({"version":1,"render":{"r.A":1,"r.a":2},"worlds":{"w":{}}})", t, &error));
    CHECK(contains(error, "render.r.a"));
    CHECK(contains(error, "duplicate"));
    CHECK_FALSE(importLookTable(R"({"version":1,"render":{"r.A":"on"},"worlds":{"w":{}}})", t, &error));
    CHECK(contains(error, "render.r.A"));
    CHECK(contains(error, "not a number"));
    CHECK_FALSE(importLookTable(R"({"version":1,"render":{"r.A":null},"worlds":{"w":{}}})", t, &error));
    CHECK_FALSE(importLookTable(R"({"version":1,"render":{"r.A":true},"worlds":{"w":{}}})", t, &error));
    CHECK_FALSE(importLookTable(R"({"version":1,"render":{"r.A":1e999},"worlds":{"w":{}}})", t, &error));
    CHECK_FALSE(importLookTable(R"({"version":1,"render":[1],"worlds":{"w":{}}})", t, &error));
    CHECK(contains(error, "render"));
    CHECK_FALSE(importLookTable(R"({"version":1,"render":5,"worlds":{"w":{}}})", t, &error));
    // A name over 128 characters.
    const std::string longName(130, 'a');
    CHECK_FALSE(importLookTable("{\"version\":1,\"render\":{\"r." + longName + "\":1},\"worlds\":{\"w\":{}}}", t, &error));
    CHECK(t.render.empty());
    CHECK(t.worlds.empty());
}

TEST_CASE("looks: a world leaves out what it keeps at the default, and an about note is ignored") {
    LookTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importLookTable(R"({"version":1,"worlds":{"w":{"about":"a note","sunKelvin":7000}}})", t, &error,
                                    &warnings),
                    error);
    CHECK(warnings.empty());
    REQUIRE(t.worlds.size() == 1);
    SceneLook expect;
    expect.sunKelvin = 7000.f;
    CHECK(t.worlds[0].look == expect);
}

TEST_CASE("looks: an unknown key is a warning that names the world and the key") {
    LookTable t;
    std::vector<std::string> warnings;
    REQUIRE(importLookTable(R"({"version":1,"worlds":{"w":{"sunKelvin":7000,"sunnyness":3}}})", t, nullptr, &warnings));
    REQUIRE(warnings.size() == 1);
    CHECK(contains(warnings[0], "w"));
    CHECK(contains(warnings[0], "sunnyness"));
    CHECK(t.worlds.size() == 1);
}

TEST_CASE("looks: a value out of range refuses the whole table, naming the world, field, value and range") {
    LookTable t;
    std::string error;
    CHECK_FALSE(importLookTable(R"({"version":1,"worlds":{"ok":{"bloom":1},"bad":{"fogForward":1.5}}})", t, &error));
    CHECK(t.worlds.empty());
    CHECK(contains(error, "bad"));
    CHECK(contains(error, "fogForward"));
    CHECK(contains(error, "1.5"));
    CHECK(contains(error, "-0.9..0.9"));
    CHECK_FALSE(importLookTable(R"({"version":1,"worlds":{"w":{"gradeChroma":-0.1}}})", t, &error));
    CHECK_FALSE(importLookTable(R"({"version":1,"worlds":{"w":{"shadowTintChroma":0.2}}})", t, &error));
}

TEST_CASE("looks: a value that is not a number refuses the table") {
    LookTable t;
    std::string error;
    CHECK_FALSE(importLookTable(R"({"version":1,"worlds":{"w":{"bloom":"high"}}})", t, &error));
    CHECK(contains(error, "bloom"));
    CHECK(contains(error, "not a number"));
    CHECK_FALSE(importLookTable(R"({"version":1,"worlds":{"w":{"bloom":null}}})", t, &error));
    CHECK_FALSE(importLookTable(R"({"version":1,"worlds":{"w":{"bloom":[1]}}})", t, &error));
}

TEST_CASE("looks: a missing or wrong version, a bad world and bad JSON refuse the table") {
    LookTable t;
    std::string error;
    CHECK_FALSE(importLookTable(R"({"worlds":{"w":{}}})", t, &error));
    CHECK(contains(error, "version"));
    CHECK_FALSE(importLookTable(R"({"version":2,"worlds":{"w":{}}})", t, &error));
    CHECK_FALSE(importLookTable(R"({"version":"1","worlds":{"w":{}}})", t, &error));
    CHECK_FALSE(importLookTable(R"({"version":1})", t, &error));
    CHECK_FALSE(importLookTable(R"({"version":1,"worlds":{}})", t, &error));
    CHECK_FALSE(importLookTable(R"({"version":1,"worlds":{"Bad Name":{}}})", t, &error));
    CHECK_FALSE(importLookTable(R"({"version":1,"worlds":{"w":5}})", t, &error));
    CHECK_FALSE(importLookTable(R"({"version":1,"worlds":{"w":{},"w":{}}})", t, &error));
    CHECK_FALSE(importLookTable("{", t, &error));
    CHECK_FALSE(importLookTable("[]", t, &error));
    CHECK(t.worlds.empty());
}

TEST_CASE("looks: find ignores case and does not allocate") {
    LookTable t;
    REQUIRE(importLookTable(R"({"version":1,"worlds":{"solhaven":{"bloom":2},"lattice":{}}})", t));
    CHECK(t.find("solhaven") != nullptr);
    CHECK(t.find("Solhaven") == t.find("solhaven"));
    CHECK(t.find("LATTICE") == t.find("lattice"));
    CHECK(t.find("solhaven")->bloom == 2.f);
    CHECK(t.find("unwritten") == nullptr);
    CHECK(t.find("") == nullptr);
    const auto before = ftcore_test::allocStats().count;
    int hits = 0;
    for (int i = 0; i < 100; ++i) hits += t.find("SolHaven") != nullptr ? 1 : 0;
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(hits == 100);
}

TEST_CASE("looks: lookWorldForMap names the world a map belongs to") {
    CHECK(lookWorldForMap("World_Solhaven") == "solhaven");
    CHECK(lookWorldForMap("Demo_Lattice") == "lattice");
    CHECK(lookWorldForMap("/Game/Maps/World_Unwritten") == "unwritten");
    CHECK(lookWorldForMap("/Game/Maps/WORLD_SOLHAVEN.World_SOLHAVEN") == "solhaven");
    CHECK(lookWorldForMap("Demo_Arena").empty());
    CHECK(lookWorldForMap("").empty());
    CHECK(lookWorldForMap("Overworld").empty());
    const auto before = ftcore_test::allocStats().count;
    int hits = 0;
    for (int i = 0; i < 100; ++i) hits += lookWorldForMap("/Game/Maps/World_Unwritten").empty() ? 0 : 1;
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(hits == 100);
}
