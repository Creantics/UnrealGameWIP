// The big worlds: Data/Field/worlds.json (loader), the layout each world generates (spacing, roads,
// clear plazas and roads, walk times) and, when the real script is there, that the table puts each
// field step of a world's scenes in that world. Ids and counts only; no story text is read or printed.

#include "ftcore/Dialogue.h"
#include "ftcore/FieldGoals.h"
#include "ftcore/GameplayPlan.h"
#include "ftcore/Json.h"
#include "ftcore/StoryRunner.h"
#include "ftcore/WorldLayout.h"

#include "AllocCounter.h"
#include "RealStory.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr const char* kWorlds = R"J({
  "version": 1,
  "worlds": [
    { "id": "town_a", "map": "World_A", "style": "solhaven", "kind": "town", "side": 1000, "seed": 3, "districts": 12,
      "story": ["Alpha", "Alpha Night"] },
    { "id": "spires", "map": "World_B", "style": "lattice", "kind": "towers", "side": 900, "districts": 6 }
  ],
  "scenes": [
    { "scene": "SC-T1", "world": "town_a", "district": 11 },
    { "scene": "SC-T2", "world": "spires", "district": 0, "style": "inksolhaven" }
  ]
})J";

constexpr float kRunSpeed = 6.2f; // m/s, the player's run

float dist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

WorldSpec spec(std::uint32_t districts, std::uint32_t seed, WorldKind kind = WorldKind::Town, float side = 1000.f) {
    WorldSpec s;
    s.id = "test";
    s.map = "World_Test";
    s.style = "solhaven";
    s.kind = kind;
    s.side = side;
    s.seed = seed;
    s.districts = districts;
    return s;
}

// Everything a layout promises, checked for one generated world.
void checkLayout(const WorldLayout& w) {
    const float half = w.side() * 0.5f;
    const std::size_t n = w.districtCount();
    REQUIRE(n > 0);
    for (std::size_t i = 0; i < n; ++i) {
        CAPTURE(i);
        const WorldDistrict& d = w.district(i);
        CHECK(std::fabs(d.centre.x) <= half - WorldLayout::kEdgeMargin + 0.01f);
        CHECK(std::fabs(d.centre.y) <= half - WorldLayout::kEdgeMargin + 0.01f);
        for (std::size_t k = i + 1; k < n; ++k) CHECK(dist(d.centre, w.district(k).centre) >= WorldLayout::kMinSpacing);
        CHECK(std::hypot(d.forward.x, d.forward.y) == doctest::Approx(1.f));
        // The plaza's centre is kPlazaCentreX ahead of the spawn, and the frame round-trips.
        const Vec2 c = w.toWorld(i, {WorldLayout::kPlazaCentreX, 0.f});
        CHECK(dist(c, d.centre) < 0.01f);
        const Vec2 back = w.toLocal(i, w.toWorld(i, {12.f, -7.f}));
        CHECK(back.x == doctest::Approx(12.f).epsilon(1e-4));
        CHECK(back.y == doctest::Approx(-7.f).epsilon(1e-4));
        // goals.json's whole box lies inside the plaza.
        for (const Vec2 corner : {Vec2{WorldLayout::kPlazaMinX, -WorldLayout::kPlazaMaxAbsY},
                                  Vec2{WorldLayout::kPlazaMinX, WorldLayout::kPlazaMaxAbsY},
                                  Vec2{WorldLayout::kPlazaMaxX, -WorldLayout::kPlazaMaxAbsY},
                                  Vec2{WorldLayout::kPlazaMaxX, WorldLayout::kPlazaMaxAbsY}})
            CHECK(dist(w.toWorld(i, corner), d.centre) < WorldLayout::kPlazaRadius);
        CHECK(w.districtAt(d.centre) == static_cast<int>(i));
        // The landmark stands in the ring, off the roads, and is tall.
        const float lr = dist(d.landmark, d.centre);
        CHECK(lr > WorldLayout::kPlazaRadius);
        CHECK(lr < WorldLayout::kDistrictRadius);
        CHECK(w.roadDistance(d.landmark) > WorldLayout::kLandmarkRadius + WorldLayout::kRoadWidth * 0.5f);
        CHECK(d.landmarkHeight >= 40.f);
    }
    // Roads join every district and keep clear of the districts they don't join.
    CHECK(w.roadCount() >= n - 1);
    std::vector<std::uint8_t> walk(2 * n + 2);
    const std::size_t steps = w.tour(0, walk.data(), walk.size());
    std::set<std::uint8_t> reached(walk.begin(), walk.begin() + static_cast<std::ptrdiff_t>(steps));
    CHECK(reached.size() == n);
    for (std::size_t s = 1; s < steps; ++s) CHECK(w.joined(walk[s - 1], walk[s]));
    for (std::size_t r = 0; r < w.roadCount(); ++r) {
        const WorldRoad& road = w.road(r);
        CHECK(road.a != road.b);
        for (std::size_t k = 0; k < n; ++k) {
            if (k == road.a || k == road.b) continue;
            // the road's line is outside district k's ring
            const Vec2 a = w.district(road.a).centre, b = w.district(road.b).centre, p = w.district(k).centre;
            const float abx = b.x - a.x, aby = b.y - a.y;
            const float t = std::clamp(((p.x - a.x) * abx + (p.y - a.y) * aby) / (abx * abx + aby * aby), 0.f, 1.f);
            CHECK(dist(p, {a.x + abx * t, a.y + aby * t}) >= WorldLayout::kDistrictRadius);
        }
    }
    // Buildings sit in their district's ring, clear of the plaza, the roads and the landmark.
    CHECK(w.blockCount() > 0);
    for (std::size_t b = 0; b < w.blockCount(); ++b) {
        const WorldBlock& k = w.block(b);
        const WorldDistrict& d = w.district(k.district);
        const float foot = 0.5f * std::hypot(k.width, k.depth);
        CHECK(dist(k.pos, d.centre) - foot >= WorldLayout::kPlazaRadius - 0.01f);
        CHECK(dist(k.pos, d.centre) + foot <= WorldLayout::kDistrictRadius + 0.01f);
        CHECK(w.roadDistance(k.pos) >= foot + WorldLayout::kRoadWidth * 0.5f);
        CHECK(dist(k.pos, d.landmark) >= foot + WorldLayout::kLandmarkRadius);
        CHECK(k.height > 0.f);
    }
    // Props stand on open ground inside the walls.
    CHECK(w.propCount() > 0);
    for (std::size_t p = 0; p < w.propCount(); ++p) {
        const WorldProp& q = w.prop(p);
        CHECK(std::fabs(q.pos.x) < half);
        CHECK(std::fabs(q.pos.y) < half);
        CHECK(w.districtAt(q.pos) < 0);
        CHECK(w.roadDistance(q.pos) >= WorldLayout::kRoadWidth * 0.5f + 2.f);
    }
}

bool loadWorlds(WorldTable& t) {
    std::string text;
    if (!ftcore_test::readProjectData("Field/worlds.json", text)) {
        MESSAGE("Field/worlds.json not readable; skipping");
        return false;
    }
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importWorldTable(text, t, &error, &warnings), error);
    CHECK(warnings.empty());
    return true;
}

} // namespace

TEST_CASE("worlds: the loader reads worlds and scenes") {
    WorldTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importWorldTable(kWorlds, t, &error, &warnings), error);
    CHECK(warnings.empty());
    REQUIRE(t.worlds.size() == 2);
    CHECK(t.worlds[0].kind == WorldKind::Town);
    CHECK(t.worlds[0].seed == 3);
    CHECK(t.worlds[0].story.size() == 2);
    CHECK(t.worlds[1].kind == WorldKind::Towers);
    CHECK(t.worlds[1].side == doctest::Approx(900.f));
    CHECK(t.worlds[1].seed == 1);
    CHECK(t.findWorld("spires") == 1);
    CHECK(t.findWorld("World_A") == 0);
    CHECK(t.findWorld("nowhere") == -1);
    const WorldScene* a = t.findScene("SC-T1");
    REQUIRE(a);
    CHECK(a->world == 0);
    CHECK(a->district == 11);
    CHECK(a->style.empty());
    const WorldScene* b = t.findScene("SC-T2");
    REQUIRE(b);
    CHECK(b->world == 1);
    CHECK(b->style == "inksolhaven");
    CHECK(t.findScene("SC-T3") == nullptr);
}

TEST_CASE("worlds: the loader refuses bad files and warns on unknown keys") {
    WorldTable t;
    std::string error;
    std::vector<std::string> warnings;
    const char* w = R"("worlds":[{"id":"a","map":"M","style":"s","side":1000,"districts":4}])";
    const auto with = [&](const std::string& body) { return "{" + body + "}"; };
    CHECK_FALSE(importWorldTable("{", t, &error));
    CHECK_FALSE(importWorldTable(R"({"version":2,)" + std::string(w) + "}", t, &error));
    CHECK_FALSE(importWorldTable(R"({"worlds":[]})", t, &error));
    REQUIRE_MESSAGE(importWorldTable(with(w), t, &error), error);
    // bad id, side, districts, kind; duplicates
    CHECK_FALSE(importWorldTable(R"({"worlds":[{"id":"A b","map":"M","style":"s","side":1000,"districts":4}]})", t, &error));
    CHECK_FALSE(importWorldTable(R"({"worlds":[{"id":"a","map":"M","style":"s","side":100,"districts":4}]})", t, &error));
    CHECK_FALSE(importWorldTable(R"({"worlds":[{"id":"a","map":"M","style":"s","side":1000,"districts":17}]})", t, &error));
    CHECK_FALSE(
        importWorldTable(R"({"worlds":[{"id":"a","map":"M","style":"s","side":1000,"districts":4,"kind":"x"}]})", t, &error));
    CHECK_FALSE(importWorldTable(
        R"({"worlds":[{"id":"a","map":"M","style":"s","side":1000,"districts":4},{"id":"a","map":"N","style":"s","side":1000,"districts":4}]})",
        t, &error));
    CHECK(error.find("duplicate") != std::string::npos);
    CHECK(t.worlds.empty());
    // scenes: unknown world, district out of range, duplicate
    CHECK_FALSE(importWorldTable(with(std::string(w) + R"(,"scenes":[{"scene":"S","world":"b","district":0}])"), t, &error));
    CHECK_FALSE(importWorldTable(with(std::string(w) + R"(,"scenes":[{"scene":"S","world":"a","district":4}])"), t, &error));
    CHECK_FALSE(importWorldTable(
        with(std::string(w) + R"(,"scenes":[{"scene":"S","world":"a","district":0},{"scene":"S","world":"a","district":1}])"),
        t, &error));
    REQUIRE(importWorldTable(with(std::string(w) + R"(,"x":1,"scenes":[{"scene":"S","world":"a","district":3,"y":2}])"), t,
                             &error, &warnings));
    CHECK(warnings.size() == 2);
}

TEST_CASE("worlds: a layout is deterministic in its seed and keeps every promise") {
    auto a = std::make_unique<WorldLayout>();
    auto b = std::make_unique<WorldLayout>();
    std::string error;
    REQUIRE_MESSAGE(generateWorldLayout(spec(12, 7), *a, &error), error);
    REQUIRE_MESSAGE(generateWorldLayout(spec(12, 7), *b, &error), error);
    REQUIRE(a->districtCount() == 12);
    CHECK(a->blockCount() == b->blockCount());
    CHECK(a->propCount() == b->propCount());
    CHECK(a->roadCount() == b->roadCount());
    for (std::size_t i = 0; i < 12; ++i) {
        CHECK(a->district(i).centre.x == b->district(i).centre.x);
        CHECK(a->district(i).centre.y == b->district(i).centre.y);
    }
    checkLayout(*a);
    REQUIRE(generateWorldLayout(spec(12, 8), *b, &error));
    CHECK(a->district(0).centre.x != b->district(0).centre.x);
}

TEST_CASE("worlds: every kind and size from 1 to 16 districts lays out (seeds 1..6)") {
    auto w = std::make_unique<WorldLayout>();
    std::string error;
    for (const WorldKind kind : {WorldKind::Town, WorldKind::Towers, WorldKind::Ruins})
        for (std::uint32_t n : {1u, 2u, 5u, 10u, 12u, 16u})
            for (std::uint32_t seed = 1; seed <= 6; ++seed) {
                CAPTURE(n);
                CAPTURE(seed);
                REQUIRE_MESSAGE(generateWorldLayout(spec(n, seed, kind), *w, &error), error);
                CHECK(w->districtCount() == n);
                checkLayout(*w);
            }
    // Too many districts for a small world says so.
    CHECK_FALSE(generateWorldLayout(spec(16, 1, WorldKind::Town, 600.f), *w, &error));
    CHECK(error.find("can't fit") != std::string::npos);
    CHECK(w->districtCount() == 0);
}

TEST_CASE("worlds: the frame, the tour and the queries don't allocate") {
    auto w = std::make_unique<WorldLayout>();
    REQUIRE(generateWorldLayout(spec(10, 2), *w));
    std::uint8_t walk[32];
    float sink = 0.f;
    const std::uint64_t before = ftcore_test::allocStats().count;
    for (std::size_t i = 0; i < w->districtCount(); ++i) {
        const Vec2 p = w->toWorld(i, {20.f, 5.f});
        sink += w->toLocal(i, p).x + w->roadDistance(p) + static_cast<float>(w->districtAt(p)) + w->spawnYaw(i);
    }
    sink += static_cast<float>(w->tour(3, walk, 32));
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(sink != 0.f);
}

TEST_CASE("worlds: the committed worlds lay out, are big, and neighbours are 20-40 s apart at a run") {
    WorldTable t;
    if (!loadWorlds(t)) return;
    REQUIRE(t.worlds.size() == 3);
    auto w = std::make_unique<WorldLayout>();
    for (const WorldSpec& s : t.worlds) {
        CAPTURE(s.id);
        std::string error;
        REQUIRE_MESSAGE(generateWorldLayout(s, *w, &error), error);
        checkLayout(*w);
        CHECK(s.side >= 1000.f); // a square kilometre at least
        // Each district's nearest neighbour by road: the median is a 20-40 s run.
        std::vector<float> nearest;
        for (std::size_t i = 0; i < w->districtCount(); ++i) {
            float best = 1e30f;
            for (std::size_t r = 0; r < w->roadCount(); ++r) {
                const WorldRoad& road = w->road(r);
                if (road.a == i || road.b == i) best = std::min(best, dist(w->district(road.a).centre, w->district(road.b).centre));
            }
            if (best < 1e30f) nearest.push_back(best / kRunSpeed);
        }
        if (nearest.empty()) continue;
        std::sort(nearest.begin(), nearest.end());
        const float median = nearest[nearest.size() / 2];
        MESSAGE(s.id << ": " << w->districtCount() << " districts, " << w->roadCount() << " roads, " << w->blockCount()
                     << " buildings, " << w->propCount() << " props, median neighbour run " << median << " s");
        CHECK(median >= 20.f);
        CHECK(median <= 40.f);
    }
}

TEST_CASE("worlds: the climax's building ring opens over the half facing the world's centre, and nothing else moves") {
    WorldTable t;
    if (!loadWorlds(t)) return;
    auto open = std::make_unique<WorldLayout>();
    auto ringed = std::make_unique<WorldLayout>();
    for (const WorldSpec& s : t.worlds) {
        CAPTURE(s.id);
        std::size_t climax = s.districts;
        for (const WorldSite& site : s.sites)
            if (site.tier == SiteTier::Climax) climax = site.district;
        REQUIRE(climax < s.districts);
        WorldSpec plain = s;
        plain.sites.clear();
        REQUIRE(generateWorldLayout(s, *open));
        REQUIRE(generateWorldLayout(plain, *ringed));
        // Without its sites the climax keeps its whole ring. With them it loses the buildings ahead of its plaza
        // and keeps those behind; every other building and every prop stands where it did.
        const WorldDistrict& d = ringed->district(climax);
        std::size_t ahead = 0, behind = 0, k = 0;
        for (std::size_t b = 0; b < ringed->blockCount(); ++b) {
            const WorldBlock& was = ringed->block(b);
            if (was.district == climax &&
                (was.pos.x - d.centre.x) * d.forward.x + (was.pos.y - d.centre.y) * d.forward.y > 0.f) {
                ++ahead;
                continue;
            }
            behind += was.district == climax ? 1 : 0;
            REQUIRE(k < open->blockCount());
            CHECK(open->block(k).district == was.district);
            CHECK(open->block(k).pos.x == was.pos.x);
            CHECK(open->block(k).pos.y == was.pos.y);
            CHECK(open->block(k).height == was.height);
            ++k;
        }
        CHECK(k == open->blockCount());
        MESSAGE(s.id << ": the climax (district " << climax << ") opens " << ahead << " buildings ahead and keeps "
                     << behind << " behind");
        CHECK(ahead >= 3);
        CHECK(behind >= 3);
        REQUIRE(open->propCount() == ringed->propCount());
        for (std::size_t p = 0; p < open->propCount(); ++p) {
            CHECK(open->prop(p).pos.x == ringed->prop(p).pos.x);
            CHECK(open->prop(p).pos.y == ringed->prop(p).pos.y);
        }
        CHECK(open->district(climax).landmark.x == d.landmark.x);
        CHECK(open->district(climax).landmark.y == d.landmark.y);
    }
}

TEST_CASE("worlds: real script: every field step of a world's scenes plays in that world") {
    FTCORE_REQUIRE_REAL_STORY(s);
    WorldTable t;
    if (!loadWorlds(t)) return;
    const StoryDatabase& db = s.db;
    std::set<std::string> aliases;
    for (const WorldSpec& w : t.worlds) aliases.insert(w.story.begin(), w.story.end());
    std::size_t listed = 0, fieldScenes = 0;
    for (std::uint32_t i = 0; i < db.sceneCount(); ++i) {
        const Scene& sc = db.scene(i);
        const std::string id(db.view(sc.id));
        const std::string world(db.view(sc.world));
        CAPTURE(id);
        const WorldScene* e = t.findScene(id);
        if (aliases.count(world) == 0) {
            CHECK(e == nullptr); // House, pockets: the Explore map
            continue;
        }
        REQUIRE(e != nullptr);
        ++listed;
        const WorldSpec& ws = t.worlds[e->world];
        CHECK(std::find(ws.story.begin(), ws.story.end(), world) != ws.story.end());
        CHECK(e->district < ws.districts);
        if (sc.mode.length > 0) ++fieldScenes;
    }
    CHECK(listed == t.scenes.size());
    CHECK(fieldScenes > 0);
}

// ---- combat routing: each world names the combat system its fights play ----

namespace {

// One world per combat kind, plus one with no `combat` key.
constexpr const char* kCombatWorlds = R"J({
  "version": 1,
  "worlds": [
    { "id": "town_a", "map": "World_A", "style": "solhaven", "side": 1000, "districts": 4, "combat": "arena" },
    { "id": "spires", "map": "World_B", "style": "lattice", "kind": "towers", "side": 900, "districts": 4, "combat": "rhythm" },
    { "id": "ruins_c", "map": "World_C", "style": "unwritten", "kind": "ruins", "side": 900, "districts": 4, "combat": "beat" },
    { "id": "plain", "map": "World_D", "style": "solhaven", "side": 900, "districts": 4 }
  ],
  "scenes": [
    { "scene": "SC-A1", "world": "town_a", "district": 0 },
    { "scene": "SC-B1", "world": "spires", "district": 1 },
    { "scene": "SC-C1", "world": "ruins_c", "district": 2 },
    { "scene": "SC-D1", "world": "plain", "district": 3 }
  ]
})J";

// A one-world file whose `combat` value is `value` (raw JSON text).
std::string oneWorldWithCombat(const std::string& value) {
    return std::string(R"J({"worlds":[{"id":"a","map":"M","style":"s","side":1000,"districts":4,"combat":)J") + value + "}]}";
}

} // namespace

TEST_CASE("worlds: the combat key parses, and a world without it plays rhythm") {
    WorldTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importWorldTable(kCombatWorlds, t, &error, &warnings), error);
    CHECK(warnings.empty()); // `combat` is a known key
    REQUIRE(t.worlds.size() == 4);
    CHECK(t.worlds[0].combat == CombatKind::Arena);
    CHECK(t.worlds[1].combat == CombatKind::Rhythm);
    CHECK(t.worlds[2].combat == CombatKind::Beat);
    CHECK(t.worlds[3].combat == CombatKind::Rhythm); // no key
    CHECK(WorldSpec{}.combat == CombatKind::Rhythm);

    // The older fixture has no `combat` anywhere: every world reads as rhythm.
    REQUIRE_MESSAGE(importWorldTable(kWorlds, t, &error, &warnings), error);
    for (const WorldSpec& w : t.worlds) CHECK(w.combat == CombatKind::Rhythm);
}

TEST_CASE("worlds: an unknown combat value is a load error with its path") {
    WorldTable t;
    std::string error;
    CHECK_FALSE(importWorldTable(oneWorldWithCombat(R"J("melee")J"), t, &error));
    CHECK(error.find("worlds[0].combat") != std::string::npos);
    CHECK(t.worlds.empty()); // failure leaves it empty
    CHECK_FALSE(importWorldTable(oneWorldWithCombat(R"J("Arena")J"), t, &error)); // the spelling is exact
    CHECK_FALSE(importWorldTable(oneWorldWithCombat(R"J("")J"), t, &error));
    CHECK_FALSE(importWorldTable(oneWorldWithCombat("3"), t, &error)); // not a string
    CHECK_FALSE(importWorldTable(oneWorldWithCombat("null"), t, &error));
    for (const char* good : {R"J("rhythm")J", R"J("arena")J", R"J("beat")J"}) {
        error.clear();
        CHECK_MESSAGE(importWorldTable(oneWorldWithCombat(good), t, &error), error);
    }
}

TEST_CASE("worlds: combatForWorld and combatForScene answer, fall back to rhythm and allocate nothing") {
    WorldTable t;
    std::string error;
    REQUIRE_MESSAGE(importWorldTable(kCombatWorlds, t, &error), error);

    const bool counting = ftcore_test::allocHookInstalled();
    const std::uint64_t before = ftcore_test::allocStats().count;
    const CombatKind byId = t.combatForWorld("town_a");
    const CombatKind byMap = t.combatForWorld("World_C"); // the map name works too
    const CombatKind spires = t.combatForWorld("spires");
    const CombatKind plain = t.combatForWorld("plain");
    const CombatKind unknownWorld = t.combatForWorld("nowhere");
    const CombatKind emptyWorld = t.combatForWorld("");
    const CombatKind sceneA = t.combatForScene("SC-A1");
    const CombatKind sceneB = t.combatForScene("SC-B1");
    const CombatKind sceneC = t.combatForScene("SC-C1");
    const CombatKind sceneD = t.combatForScene("SC-D1");
    const CombatKind unknownScene = t.combatForScene("SC-NOPE");
    const CombatKind emptyScene = t.combatForScene("");
    const std::uint64_t allocs = ftcore_test::allocStats().count - before;
    if (counting) CHECK(allocs == 0);

    CHECK(byId == CombatKind::Arena);
    CHECK(byMap == CombatKind::Beat);
    CHECK(spires == CombatKind::Rhythm);
    CHECK(plain == CombatKind::Rhythm);
    CHECK(unknownWorld == CombatKind::Rhythm);
    CHECK(emptyWorld == CombatKind::Rhythm);
    CHECK(sceneA == CombatKind::Arena);
    CHECK(sceneB == CombatKind::Rhythm);
    CHECK(sceneC == CombatKind::Beat);
    CHECK(sceneD == CombatKind::Rhythm);       // its world has no key
    CHECK(unknownScene == CombatKind::Rhythm); // a scene with no world
    CHECK(emptyScene == CombatKind::Rhythm);

    // An empty table answers rhythm for everything.
    const WorldTable none{};
    CHECK(none.combatForWorld("town_a") == CombatKind::Rhythm);
    CHECK(none.combatForScene("SC-A1") == CombatKind::Rhythm);
}

TEST_CASE("worlds: every map opens by its long name under /Game/Maps") {
    CHECK(mapPackage("World_A") == "/Game/Maps/World_A");
    CHECK(mapPackage("Overworld") == "/Game/Maps/Overworld");
    CHECK(mapPackage("/Game/Maps/World_A") == "/Game/Maps/World_A");
    CHECK(mapPackage("").empty());
    // The loader takes short names only, so a world's map can't point anywhere but /Game/Maps.
    WorldTable t;
    std::string error;
    CHECK_FALSE(importWorldTable(
        R"({"worlds":[{"id":"a","map":"/Game/Trial/LookBefore/M","style":"s","side":1000,"districts":4}]})", t, &error));
    CHECK(error.find(".map") != std::string::npos);
    CHECK_FALSE(importWorldTable(
        R"({"worlds":[{"id":"a","map":"M","style":"s","side":1000,"districts":4}],"overworld":{"map":"Trial/O"}})", t,
        &error));
    CHECK(error.find("overworld.map") != std::string::npos);
    // Each world in Data/Field/worlds.json, and the overworld, opens under /Game/Maps.
    if (!loadWorlds(t)) return;
    REQUIRE(t.worlds.size() == 3);
    for (const WorldSpec& w : t.worlds) {
        CAPTURE(w.id);
        CHECK(w.map.find('/') == std::string::npos);
        CHECK(mapPackage(w.map) == "/Game/Maps/" + w.map);
    }
    REQUIRE(t.overworld.present);
    CHECK(mapPackage(t.overworld.map) == "/Game/Maps/" + t.overworld.map);
}

TEST_CASE("worlds: Data/Field/worlds.json names solhaven arena, lattice rhythm and unwritten beat") {
    std::string text;
    if (!ftcore_test::readProjectData("Field/worlds.json", text)) {
        MESSAGE("Field/worlds.json not readable; skipping");
        return;
    }
    // The file spells the key out for every world (rhythm is also the default, so read the text).
    JsonDocument doc;
    std::string parseError;
    REQUIRE_MESSAGE(doc.parse(text, &parseError), parseError);
    const JsonValue worlds = doc.root()["worlds"];
    REQUIRE(worlds.size() == 3);
    for (std::uint32_t i = 0; i < worlds.size(); ++i) {
        const std::string id(worlds[i]["id"].string());
        CAPTURE(id);
        const std::string_view combat = worlds[i]["combat"].string();
        if (id == "solhaven") CHECK(combat == "arena");
        else if (id == "lattice") CHECK(combat == "rhythm");
        else if (id == "unwritten") CHECK(combat == "beat");
        else FAIL("unexpected world " << id);
    }

    WorldTable t;
    if (!loadWorlds(t)) return;
    CHECK(t.combatForWorld("solhaven") == CombatKind::Arena);
    CHECK(t.combatForWorld("lattice") == CombatKind::Rhythm);
    CHECK(t.combatForWorld("unwritten") == CombatKind::Beat);
    CHECK(t.combatForWorld("World_Unwritten") == CombatKind::Beat);
    // Scenes follow their world; one that isn't placed plays rhythm.
    CHECK(t.combatForScene("SC-01-D1") == CombatKind::Arena);
    CHECK(t.combatForScene("SC-13-HE1") == CombatKind::Arena); // an Ink-Solhaven scene
    CHECK(t.combatForScene("SC-01-U1") == CombatKind::Rhythm);
    CHECK(t.combatForScene("SC-14-Q1") == CombatKind::Beat);
    CHECK(t.combatForScene("SC-P0-01") == CombatKind::Beat);
    CHECK(t.combatForScene("SC-not-placed") == CombatKind::Rhythm);
    // Every placed scene answers its world's system.
    for (const WorldScene& s : t.scenes) {
        CAPTURE(s.scene);
        CHECK(t.combatForScene(s.scene) == t.worlds[s.world].combat);
    }
}
