// The overworld that joins the big worlds: worlds.json's "overworld" entry, the layout it generates
// (gates on a ring, roads round it, emblems behind the gates), the routes between worlds and the rule
// that opens a world. When the real script is there, how often the story's field steps change world
// (counts only; no story text is read or printed).

#include "ftcore/Dialogue.h"
#include "ftcore/GameplayPlan.h"
#include "ftcore/Overworld.h"
#include "ftcore/WorldLayout.h"

#include "AllocCounter.h"
#include "RealStory.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kOverRunSpeed = 6.2f; // m/s, the player's run

float dist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

// A table of `count` worlds (kinds in turn) with an overworld.
WorldTable table(std::size_t count, std::uint32_t seed, float side = 480.f) {
    WorldTable t;
    for (std::size_t i = 0; i < count; ++i) {
        WorldSpec w;
        w.id = "w" + std::to_string(i);
        w.map = "World_" + std::to_string(i);
        w.style = "solhaven";
        w.kind = static_cast<WorldKind>(i % 3);
        t.worlds.push_back(w);
    }
    t.overworld.present = true;
    t.overworld.map = "Overworld";
    t.overworld.side = side;
    t.overworld.seed = seed;
    return t;
}

// Everything an overworld layout promises.
void checkOverworld(const OverworldLayout& m, std::size_t count) {
    const float half = m.side() * 0.5f;
    REQUIRE(m.nodeCount() == count);
    for (std::size_t i = 0; i < count; ++i) {
        CAPTURE(i);
        const OverworldNode& n = m.node(i);
        CHECK(n.world == i);
        CHECK(std::fabs(n.centre.x) + OverworldLayout::kEmblemRadius <= half - OverworldLayout::kEdgeMargin + 0.01f);
        CHECK(std::fabs(n.centre.y) + OverworldLayout::kEmblemRadius <= half - OverworldLayout::kEdgeMargin + 0.01f);
        CHECK(std::hypot(n.outward.x, n.outward.y) == doctest::Approx(1.f));
        CHECK(dist(n.gate, n.centre) == doctest::Approx(OverworldLayout::kGateOffset));
        CHECK(m.nodeAt(n.gate) == static_cast<int>(i));
        CHECK(n.landmarkHeight >= 14.f);
        // The gate faces the centre.
        const float yaw = m.gateYaw(i);
        CHECK(std::cos(yaw) * n.outward.x + std::sin(yaw) * n.outward.y == doctest::Approx(-1.f));
        for (std::size_t k = i + 1; k < count; ++k) CHECK(dist(n.centre, m.node(k).centre) > 2.f * OverworldLayout::kNodeClear);
    }
    // Roads round the ring: neighbouring gates a 25-40 s run apart, clear of the emblems.
    CHECK(m.roadCount() == (count >= 3 ? count : count - 1));
    for (std::size_t r = 0; r < m.roadCount(); ++r) {
        const OverworldRoad& road = m.road(r);
        const float len = dist(m.node(road.a).gate, m.node(road.b).gate);
        CHECK(len >= OverworldLayout::kMinHop);
        CHECK(len <= OverworldLayout::kMaxHop);
    }
    // Each road leaves its gate away from the emblem behind it.
    for (std::size_t i = 0; i < count; ++i) CHECK(m.roadDistance(m.node(i).centre) >= OverworldLayout::kGateOffset - 0.01f);
    // Emblem blocks lie inside their emblem, off the roads and away from the gate.
    CHECK(m.blockCount() >= count * 5);
    for (std::size_t b = 0; b < m.blockCount(); ++b) {
        const WorldBlock& k = m.block(b);
        const OverworldNode& n = m.node(k.district);
        const float foot = 0.5f * std::hypot(k.width, k.depth);
        CHECK(dist(k.pos, n.centre) + foot <= OverworldLayout::kEmblemRadius + 0.01f);
        CHECK(dist(k.pos, n.gate) >= OverworldLayout::kGateClear + foot);
        CHECK(m.roadDistance(k.pos) >= foot + OverworldLayout::kRoadWidth * 0.5f);
        CHECK(k.height > 0.f);
    }
    // Props on open ground inside the walls.
    CHECK(m.propCount() > 0);
    for (std::size_t p = 0; p < m.propCount(); ++p) {
        const WorldProp& q = m.prop(p);
        CHECK(std::fabs(q.pos.x) < half);
        CHECK(std::fabs(q.pos.y) < half);
        CHECK(m.roadDistance(q.pos) >= OverworldLayout::kRoadWidth * 0.5f + 2.f);
        CHECK(m.nodeAt(q.pos) < 0);
        for (std::size_t i = 0; i < count; ++i) CHECK(dist(q.pos, m.node(i).centre) >= OverworldLayout::kNodeClear);
    }
    // A route joins every ordered pair, gate to gate along roads.
    for (std::size_t a = 0; a < count; ++a)
        for (std::size_t b = 0; b < count; ++b) {
            CAPTURE(a);
            CAPTURE(b);
            OverworldRoute route;
            if (a == b) {
                CHECK_FALSE(overworldRoute(m, a, b, route));
                continue;
            }
            REQUIRE(overworldRoute(m, a, b, route));
            CHECK(route.from == a);
            CHECK(route.to == b);
            CHECK(dist(route.start, m.node(a).gate) < 0.01f);
            CHECK(dist(route.goal(), m.node(b).gate) < 0.01f);
            CHECK(route.pointCount <= count / 2 + 1);
            Vec2 at = route.start;
            for (std::size_t p = 0; p < route.pointCount; ++p) {
                const int from = m.nodeAt(at), to = m.nodeAt(route.points[p]);
                REQUIRE(from >= 0);
                REQUIRE(to >= 0);
                CHECK(m.joined(static_cast<std::size_t>(from), static_cast<std::size_t>(to)));
                at = route.points[p];
            }
            const Vec2 first = route.points[0];
            CHECK(std::atan2(first.y - route.start.y, first.x - route.start.x) == doctest::Approx(route.startYaw));
        }
}

} // namespace

TEST_CASE("overworld: the loader reads the overworld entry") {
    WorldTable t;
    std::string error;
    std::vector<std::string> warnings;
    const std::string w = R"("worlds":[{"id":"a","map":"M","style":"s","side":1000,"districts":4}])";
    REQUIRE_MESSAGE(importWorldTable("{" + w + "}", t, &error), error);
    CHECK_FALSE(t.overworld.present);
    REQUIRE_MESSAGE(importWorldTable("{" + w + R"(,"overworld":{"map":"Over","side":500,"seed":9}})", t, &error, &warnings),
                    error);
    CHECK(warnings.empty());
    CHECK(t.overworld.present);
    CHECK(t.overworld.map == "Over");
    CHECK(t.overworld.side == doctest::Approx(500.f));
    CHECK(t.overworld.seed == 9);
    REQUIRE(importWorldTable("{" + w + R"(,"overworld":{"map":"Over","z":1}})", t, &error, &warnings));
    CHECK(t.overworld.side == doctest::Approx(480.f));
    CHECK(t.overworld.seed == 1);
    CHECK(warnings.size() == 1);
    // not an object, a bad or taken map, a side out of range, a bad seed
    CHECK_FALSE(importWorldTable("{" + w + R"(,"overworld":3})", t, &error));
    CHECK_FALSE(importWorldTable("{" + w + R"(,"overworld":{"map":"a b"}})", t, &error));
    CHECK_FALSE(importWorldTable("{" + w + R"(,"overworld":{"map":"M"}})", t, &error));
    CHECK(error.find("overworld.map") != std::string::npos);
    CHECK_FALSE(importWorldTable("{" + w + R"(,"overworld":{"map":"Over","side":100}})", t, &error));
    CHECK_FALSE(importWorldTable("{" + w + R"(,"overworld":{"map":"Over","seed":-1}})", t, &error));
    CHECK(t.worlds.empty());
}

TEST_CASE("overworld: a layout is deterministic in its seed and keeps every promise") {
    auto a = std::make_unique<OverworldLayout>();
    auto b = std::make_unique<OverworldLayout>();
    std::string error;
    REQUIRE_MESSAGE(generateOverworldLayout(table(3, 7), *a, &error), error);
    REQUIRE_MESSAGE(generateOverworldLayout(table(3, 7), *b, &error), error);
    CHECK(a->blockCount() == b->blockCount());
    CHECK(a->propCount() == b->propCount());
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(a->node(i).gate.x == b->node(i).gate.x);
        CHECK(a->node(i).gate.y == b->node(i).gate.y);
    }
    checkOverworld(*a, 3);
    REQUIRE(generateOverworldLayout(table(3, 8), *b, &error));
    CHECK(a->node(0).gate.x != b->node(0).gate.x);
}

TEST_CASE("overworld: 1 to 8 worlds lay out (seeds 1..6), and what doesn't fit says so") {
    auto m = std::make_unique<OverworldLayout>();
    std::string error;
    for (std::size_t n = 1; n <= OverworldLayout::kMaxNodes; ++n)
        for (std::uint32_t seed = 1; seed <= 6; ++seed) {
            CAPTURE(n);
            CAPTURE(seed);
            REQUIRE_MESSAGE(generateOverworldLayout(table(n, seed, n <= 4 ? 480.f : 1200.f), *m, &error), error);
            checkOverworld(*m, n);
        }
    CHECK_FALSE(generateOverworldLayout(table(8, 1, 480.f), *m, &error));
    CHECK(error.find("can't fit") != std::string::npos);
    CHECK(m->nodeCount() == 0);
    CHECK_FALSE(generateOverworldLayout(table(9, 1, 1200.f), *m, &error));
    WorldTable none = table(3, 1);
    none.overworld.present = false;
    CHECK_FALSE(generateOverworldLayout(none, *m, &error));
    CHECK(error.find("has none") != std::string::npos);
}

TEST_CASE("overworld: routes, paths and queries don't allocate") {
    auto m = std::make_unique<OverworldLayout>();
    REQUIRE(generateOverworldLayout(table(6, 2, 1200.f), *m));
    OverworldRoute route;
    std::uint8_t way[8];
    float sink = 0.f;
    const std::uint64_t before = ftcore_test::allocStats().count;
    for (std::size_t a = 0; a < 6; ++a)
        for (std::size_t b = 0; b < 6; ++b) {
            if (overworldRoute(*m, a, b, route)) sink += route.goal().x + route.startYaw;
            sink += static_cast<float>(m->path(a, b, way, 8));
        }
    for (std::size_t i = 0; i < 6; ++i)
        sink += m->roadDistance(m->node(i).centre) + static_cast<float>(m->nodeAt(m->node(i).gate)) + m->gateYaw(i);
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(sink != 0.f);
    // Six on a ring: the far side is three roads away either way round.
    CHECK(m->path(0, 3, way, 8) == 4);
    CHECK(m->path(0, 3, way, 2) == 0); // too little room
    CHECK(m->path(0, 6, way, 8) == 0);
}

TEST_CASE("overworld: a world opens when its first placed scene starts") {
    WorldTable t = table(3, 1);
    t.scenes = {{"S-B1", 1, 0, ""}, {"S-A1", 0, 0, ""}, {"S-A2", 0, 1, ""}, {"S-C1", 2, 0, ""}};
    const std::vector<std::string_view> order = {"S-H", "S-A1", "S-B1", "S-A2", "S-H2", "S-C1"};
    std::vector<std::size_t> first;
    worldFirstScenes(t, order, first);
    REQUIRE(first.size() == 3);
    CHECK(first[0] == 1);
    CHECK(first[1] == 2);
    CHECK(first[2] == 5);
    CHECK(worldsOpen(first, 0) == 0u);
    CHECK(worldsOpen(first, 1) == 1u);
    CHECK(worldsOpen(first, 4) == 3u);
    CHECK(worldsOpen(first, 5) == 7u);
    // A world no scene of the order is placed in never opens.
    worldFirstScenes(t, {"S-A1", "S-C1"}, first);
    CHECK(first[1] == 2);
    CHECK(worldsOpen(first, 1) == 5u);
}

TEST_CASE("overworld: the committed overworld joins the three worlds with 25-40 s runs") {
    std::string text;
    if (!ftcore_test::readProjectData("Field/worlds.json", text)) {
        MESSAGE("Field/worlds.json not readable; skipping");
        return;
    }
    WorldTable t;
    std::string error;
    REQUIRE_MESSAGE(importWorldTable(text, t, &error), error);
    REQUIRE(t.overworld.present);
    auto m = std::make_unique<OverworldLayout>();
    REQUIRE_MESSAGE(generateOverworldLayout(t, *m, &error), error);
    checkOverworld(*m, t.worlds.size());
    for (std::size_t r = 0; r < m->roadCount(); ++r) {
        const float run = dist(m->node(m->road(r).a).gate, m->node(m->road(r).b).gate) / kOverRunSpeed;
        CHECK(run >= 24.f);
        CHECK(run <= 41.f);
    }
    MESSAGE("overworld: " << m->nodeCount() << " nodes, " << m->roadCount() << " roads, " << m->blockCount()
                          << " emblem blocks, " << m->propCount() << " props");
}

TEST_CASE("overworld: real script: every world opens, and the field steps change world a bounded number of times") {
    FTCORE_REQUIRE_REAL_STORY(s);
    std::string text;
    if (!ftcore_test::readProjectData("Field/worlds.json", text)) return;
    WorldTable t;
    REQUIRE(importWorldTable(text, t));
    const StoryDatabase& db = s.db;
    std::vector<std::string_view> order;
    for (std::uint32_t i = 0; i < db.sceneCount(); ++i) order.push_back(db.view(db.scene(i).id));
    std::vector<std::size_t> first;
    worldFirstScenes(t, order, first);
    for (std::size_t w = 0; w < first.size(); ++w) {
        CAPTURE(w);
        CHECK(first[w] < order.size());
    }
    CHECK(worldsOpen(first, order.size() - 1) == (1u << t.worlds.size()) - 1u);
    // Hops: consecutive field steps in different worlds, as the story director counts them (a step
    // whose screen routes to the field, in a scene placed in a world). Each is one overworld walk.
    std::size_t hops = 0;
    int last = -1;
    GameplayPlan plan;
    for (std::uint32_t i = 0; i < db.sceneCount(); ++i) {
        const Scene& sc = db.scene(i);
        const WorldScene* e = t.findScene(db.view(sc.id));
        if (!e) continue;
        bool field = false;
        for (std::uint32_t b = 0; b < sc.beatCount && !field; ++b) {
            const Beat& beat = db.beat(sc, b);
            if (beat.type != BeatType::Gameplay || !plan.lower(db, beat)) continue;
            for (std::uint32_t k = 0; k < plan.size(); ++k)
                if (screenRoutesTo(plan.step(k).screen) == GameplayScreen::Field) field = true;
        }
        if (!field) continue;
        if (last >= 0 && static_cast<int>(e->world) != last) ++hops;
        last = static_cast<int>(e->world);
        CHECK(((worldsOpen(first, i) >> e->world) & 1u) == 1u);
    }
    MESSAGE("real script: " << hops << " overworld hops between field steps");
    CHECK(hops > 0);
    CHECK(hops <= 30); // the alpha gate's time budget
}
