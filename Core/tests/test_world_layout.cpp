// The big worlds: Data/Field/worlds.json (loader), the layout each world generates (spacing, roads,
// clear plazas and roads, walk times) and, when the real script is there, that the table puts each
// field step of a world's scenes in that world. Ids and counts only; no story text is read or printed.

#include "ftcore/Dialogue.h"
#include "ftcore/FieldGoals.h"
#include "ftcore/GameplayPlan.h"
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
