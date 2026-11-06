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

TEST_CASE("worlds: a map pins its own districts; their frames move and the rest stays as generated") {
    auto w = std::make_unique<WorldLayout>();
    REQUIRE(generateWorldLayout(spec(4, 3), *w));
    // The generator's rule is spawnFor: every generated spawn is it.
    for (std::size_t i = 0; i < w->districtCount(); ++i) {
        CAPTURE(i);
        const Vec2 s = WorldLayout::spawnFor(w->district(i).centre);
        CHECK(s.x == doctest::Approx(w->district(i).origin.x));
        CHECK(s.y == doctest::Approx(w->district(i).origin.y));
    }
    CHECK(WorldLayout::spawnFor({0.f, 0.f}).x == doctest::Approx(-WorldLayout::kPlazaCentreX)); // faces +x from the centre

    const std::size_t roads = w->roadCount();
    const WorldRoad firstRoad = w->road(0);
    const Vec2 firstBlock = w->block(0).pos;
    const std::unique_ptr<WorldLayout> before = std::make_unique<WorldLayout>(*w);

    // District 0 by the rule; district 1 at a spawn the map names, 60 m east of its centre; 2 and 3 only
    // 20 m apart, so their rings overlap.
    DistrictPin pins[4];
    pins[0].centre = {-65.f, 105.f};
    pins[1].centre = {-50.f, -245.f};
    pins[1].spawn = {15.f, -245.f};
    pins[1].hasSpawn = true;
    pins[2].centre = {290.f, 100.f};
    pins[3].centre = {300.f, 82.f};
    std::string error;
    REQUIRE(w->pinDistricts(pins, 4, &error));

    // By the rule: 9 m behind the centre, facing the world's centre, so the plaza's centre is 9 m ahead.
    const WorldDistrict& d0 = w->district(0);
    CHECK(d0.centre.x == doctest::Approx(-65.f));
    CHECK(d0.centre.y == doctest::Approx(105.f));
    const float len0 = std::hypot(65.f, 105.f);
    CHECK(d0.forward.x == doctest::Approx(65.f / len0));
    CHECK(d0.forward.y == doctest::Approx(-105.f / len0));
    CHECK(dist(d0.origin, d0.centre) == doctest::Approx(WorldLayout::kPlazaCentreX));
    const Vec2 ahead = w->toWorld(0, {WorldLayout::kPlazaCentreX, 0.f});
    CHECK(ahead.x == doctest::Approx(-65.f));
    CHECK(ahead.y == doctest::Approx(105.f));

    // A named spawn stands where the map says and faces its centre: due west here.
    const WorldDistrict& d1 = w->district(1);
    CHECK(d1.origin.x == doctest::Approx(15.f));
    CHECK(d1.origin.y == doctest::Approx(-245.f));
    CHECK(d1.forward.x == doctest::Approx(-1.f));
    CHECK(d1.forward.y == doctest::Approx(0.f).epsilon(1e-6));
    CHECK(std::fabs(w->spawnYaw(1)) == doctest::Approx(3.14159265f));
    const Vec2 local = w->toLocal(1, {-50.f, -245.f});
    CHECK(local.x == doctest::Approx(65.f));

    // Overlapping rings: the nearest centre wins, whichever comes first.
    CHECK(w->districtAt({290.f, 100.f}) == 2);
    CHECK(w->districtAt({300.f, 82.f}) == 3);
    CHECK(w->districtAt({299.f, 85.f}) == 3);
    CHECK(w->districtAt({0.f, 0.f}) == -1);

    // The roads, blocks and the road graph are untouched.
    CHECK(w->roadCount() == roads);
    CHECK(w->road(0).a == firstRoad.a);
    CHECK(w->road(0).b == firstRoad.b);
    CHECK(w->block(0).pos.x == doctest::Approx(firstBlock.x));
    std::uint8_t walk[32];
    std::uint8_t walkBefore[32];
    const std::size_t steps = w->tour(0, walk, 32);
    REQUIRE(steps == before->tour(0, walkBefore, 32));
    for (std::size_t i = 0; i < steps; ++i) CHECK(walk[i] == walkBefore[i]);

    // Refused, and the layout left as it was: a wrong count, a centre or a spawn outside the square, a
    // spawn on its centre.
    const std::unique_ptr<WorldLayout> pinned = std::make_unique<WorldLayout>(*w);
    const auto unchanged = [&] {
        for (std::size_t i = 0; i < w->districtCount(); ++i) {
            if (w->district(i).origin.x != pinned->district(i).origin.x || w->district(i).centre.y != pinned->district(i).centre.y)
                return false;
        }
        return true;
    };
    CHECK_FALSE(w->pinDistricts(pins, 3, &error));
    CHECK(error.find("3 districts") != std::string::npos);
    CHECK(unchanged());
    DistrictPin bad[4] = {pins[0], pins[1], pins[2], pins[3]};
    bad[3].centre = {501.f, 0.f};
    CHECK_FALSE(w->pinDistricts(bad, 4, &error));
    CHECK(error.find("district 3's centre") != std::string::npos);
    CHECK(unchanged());
    bad[3] = pins[3];
    bad[1].spawn = {15.f, -501.f};
    CHECK_FALSE(w->pinDistricts(bad, 4, &error));
    CHECK(error.find("district 1's spawn lies outside") != std::string::npos);
    bad[1].spawn = {-50.5f, -245.f};
    CHECK_FALSE(w->pinDistricts(bad, 4, &error));
    CHECK(error.find("under 1 m") != std::string::npos);
    CHECK(unchanged());
    CHECK_FALSE(w->pinDistricts(nullptr, 4, &error));
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

// A one-world file whose `combat` value is `value` (raw JSON text).
std::string oneWorldWithCombat(const std::string& value) {
    return std::string(R"J({"worlds":[{"id":"a","map":"M","style":"s","side":1000,"districts":4,"combat":)J") + value + "}]}";
}

} // namespace

TEST_CASE("worlds: the retired combat key loads with a warning, whatever its value") {
    // A fight's system is its encounter's "combat" (Data/Battle/encounters.json), not its world's.
    for (const char* value : {R"J("arena")J", R"J("rhythm")J", R"J("beat")J", R"J("melee")J", "3", "null"}) {
        WorldTable t;
        std::string error;
        std::vector<std::string> warnings;
        CHECK_MESSAGE(importWorldTable(oneWorldWithCombat(value), t, &error, &warnings), error);
        CHECK(t.worlds.size() == 1);
        REQUIRE(warnings.size() == 1);
        CHECK(warnings[0].find("worlds[0].combat") != std::string::npos);
        CHECK(warnings[0].find("encounters.json") != std::string::npos);
    }
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

TEST_CASE("worlds: Data/Field/worlds.json no longer names a combat system") {
    std::string text;
    if (!ftcore_test::readProjectData("Field/worlds.json", text)) {
        MESSAGE("Field/worlds.json not readable; skipping");
        return;
    }
    // Each fight names its own (Data/Battle/encounters.json): the worlds' retired key is gone.
    JsonDocument doc;
    std::string parseError;
    REQUIRE_MESSAGE(doc.parse(text, &parseError), parseError);
    const JsonValue worlds = doc.root()["worlds"];
    REQUIRE(worlds.size() == 3);
    for (std::uint32_t i = 0; i < worlds.size(); ++i) CHECK(worlds[i]["combat"].isAbsent());
}
