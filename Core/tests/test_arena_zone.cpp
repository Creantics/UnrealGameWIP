#include "ftcore/ArenaZone.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 60.f;

ArenaTable loadArena() {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/arena.json", text)) FAIL("Data/Battle/arena.json not found");
    ArenaTable t;
    std::string err;
    REQUIRE_MESSAGE(importArenaTable(text, t, &err), err);
    return t;
}

WorldTable loadWorlds() {
    std::string text;
    if (!ftcore_test::readProjectData("Field/worlds.json", text)) FAIL("Data/Field/worlds.json not found");
    WorldTable t;
    std::string err;
    REQUIRE_MESSAGE(importWorldTable(text, t, &err), err);
    return t;
}

ArenaZoneTable loadZones(const ArenaTable& arena, const WorldTable& worlds, std::vector<std::string>* warnings = nullptr) {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/arenas.json", text)) FAIL("Data/Battle/arenas.json not found");
    ArenaZoneTable t;
    std::string err;
    REQUIRE_MESSAGE(importArenaZoneTable(text, arena, worlds, t, &err, warnings), err);
    return t;
}

std::string zoneError(const ArenaTable& arena, const WorldTable& worlds, const std::string& json) {
    ArenaZoneTable t;
    std::string err;
    CHECK_FALSE(importArenaZoneTable(json, arena, worlds, t, &err));
    CHECK(t.zones.empty());
    return err;
}

bool contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

ArenaDef fightOf(const ArenaTable& arena, const std::vector<std::string>& bosses, int attackers = 1) {
    ArenaDef def = arena.fallback;
    def.enemyCount = static_cast<std::uint8_t>(bosses.size());
    for (std::size_t i = 0; i < bosses.size(); ++i) {
        for (std::size_t e = 0; e < arena.enemyIds.size(); ++e) {
            if (arena.enemyIds[e] == bosses[i]) {
                def.enemies[i] = arena.enemies[e];
                def.enemyKind[i] = static_cast<std::uint8_t>(e);
            }
        }
    }
    def.attackers = static_cast<std::uint8_t>(attackers);
    return def;
}

// Everything a test needs about one zone of the shipped file, fitted to its real world layout.
struct Fitted {
    ArenaZoneSpec spec;
    WorldLayout layout;
    bool hasLayout = false;
    ArenaDef def;
    ArenaZoneFit fit;
    std::vector<std::string> warnings;
};

void fitShipped(const ArenaTable& arena, const WorldTable& worlds, const ArenaZoneSpec& spec, Fitted& out) {
    out.spec = spec;
    out.def = fightOf(arena, spec.bosses);
    out.hasLayout = !spec.world.empty();
    if (out.hasLayout) {
        std::string err;
        REQUIRE_MESSAGE(generateWorldLayout(worlds.worlds[spec.worldIndex], out.layout, &err), err);
    }
    std::string err;
    REQUIRE_MESSAGE(fitArenaZone(spec, out.hasLayout ? &out.layout : nullptr, out.def, out.fit, &err, &out.warnings), err);
    out.fit.apply(out.def);
}

struct Play {
    ArenaPhase phase = ArenaPhase::NotStarted;
    float time = 0.f;
    bool held = true; // every body stayed inside the walls and out of the obstacles after every step
};

// Plays a fitted zone's fight at a fixed 1/60 s with the zone-aware step, the autopilot or a player pressing nothing.
Play playIn(const Fitted& f, std::uint32_t seed, bool autopilot) {
    ArenaFight fight;
    fight.begin(f.def, f.fit.ring, f.fit.playerStart, f.fit.playerFacing, seed);
    ArenaBodies bodies;
    bodies.player = fight.playerStart();
    for (std::size_t i = 0; i < fight.enemyCount(); ++i) bodies.enemies[i] = fight.enemyStart(i);
    Play play;
    const ArenaZone& zone = f.fit.zone;
    while (!fight.over() && fight.time() < 120.f) {
        fight.tick(kDt, autopilot ? fight.autopilot() : ArenaInput{}, bodies);
        arenaStep(fight, bodies, kDt, zone);
        const auto holds = [&](Vec2 p, float r) { return zone.contains(p, r - 0.02f) && zone.obstacleDistance(p) >= r - 0.02f; };
        if (!holds(bodies.player, f.def.fighter.bodyRadius)) play.held = false;
        for (std::size_t i = 0; i < fight.enemyCount(); ++i)
            if (!holds(bodies.enemies[i], f.def.enemies[i].bodyRadius)) play.held = false;
    }
    play.phase = fight.phase();
    play.time = fight.time();
    return play;
}

ArenaZone circleZone(Vec2 c, float r) {
    ArenaZone z;
    z.shape = ZoneShape::Circle;
    z.centre = c;
    z.radius = r;
    return z;
}

ArenaZone squareZone(float half) {
    ArenaZone z;
    z.shape = ZoneShape::Polygon;
    z.verts[0] = Vec2{-half, -half};
    z.verts[1] = Vec2{half, -half};
    z.verts[2] = Vec2{half, half};
    z.verts[3] = Vec2{-half, half};
    z.vertCount = 4;
    z.centre = Vec2{0.f, 0.f};
    z.radius = half * 1.4142136f;
    return z;
}

} // namespace

// ---- the walls and the obstacles ----

TEST_CASE("arena zone shapes: contains and clamp, with a margin, for a circle and a polygon") {
    for (const ArenaZone& z : {circleZone(Vec2{3.f, -2.f}, 10.f), squareZone(10.f)}) {
        const Vec2 c = z.centre;
        CHECK(z.contains(c));
        CHECK(z.contains(Vec2{c.x + 8.f, c.y}, 0.f));
        CHECK(z.contains(Vec2{c.x + 8.f, c.y}, 1.f));
        CHECK_FALSE(z.contains(Vec2{c.x + 8.f, c.y}, 3.f));
        CHECK_FALSE(z.contains(Vec2{c.x + 12.f, c.y}));
        // Inside, clamp is the identity.
        const Vec2 in{c.x + 4.f, c.y - 3.f};
        CHECK(z.clamp(in, 0.5f).x == in.x);
        CHECK(z.clamp(in, 0.5f).y == in.y);
        // Outside: pulled to the shrunk edge, and then inside.
        const Vec2 out = z.clamp(Vec2{c.x + 30.f, c.y + 1.f}, 1.f);
        CHECK(z.contains(out, 1.f - 1e-3f));
        CHECK_FALSE(z.contains(out, 1.5f));
        // Thinner than the margin: the centre.
        const Vec2 mid = z.clamp(Vec2{c.x + 30.f, c.y}, 50.f);
        CHECK(mid.x == doctest::Approx(c.x).epsilon(1e-3).scale(1.f));
        CHECK(z.area() > 100.f);
    }
    CHECK(circleZone({}, 10.f).area() == doctest::Approx(314.159f));
    CHECK(squareZone(10.f).area() == doctest::Approx(400.f));
    CHECK(squareZone(10.f).inradius() == doctest::Approx(10.f));
    // A corner slides: a point outside the square's corner lands on the shrunk corner.
    const Vec2 corner = squareZone(10.f).clamp(Vec2{50.f, 50.f}, 1.f);
    CHECK(corner.x == doctest::Approx(9.f));
    CHECK(corner.y == doctest::Approx(9.f));
}

TEST_CASE("arena zone obstacles: a circle and a box push a body out to its radius, from outside and from inside") {
    ArenaZone z = squareZone(30.f);
    ZoneObstacle tree;
    tree.at = Vec2{5.f, 0.f};
    tree.radius = 1.f;
    ZoneObstacle house;
    house.box = true;
    house.at = Vec2{-10.f, 4.f};
    house.half = Vec2{4.f, 2.f};
    house.yaw = 0.5f;
    z.obstacles[0] = tree;
    z.obstacles[1] = house;
    z.obstacleCount = 2;

    CHECK(z.obstacleDistance(Vec2{8.f, 0.f}) == doctest::Approx(2.f));
    CHECK(z.obstacleDistance(Vec2{5.f, 0.f}) == doctest::Approx(-1.f));
    CHECK(z.obstacleDistance(Vec2{-10.f, 4.f}) < 0.f);
    CHECK(z.obstacleDistance(Vec2{0.f, -20.f}) > 5.f);

    for (const Vec2 p : {Vec2{5.2f, 0.1f}, Vec2{5.f, 0.f}, Vec2{6.f, 0.f}, Vec2{4.f, 0.5f}}) {
        const Vec2 q = z.pushOut(p, 0.5f);
        CHECK(z.obstacleDistance(q) >= 0.5f - 1e-3f);
        CHECK(std::sqrt((q.x - 5.f) * (q.x - 5.f) + q.y * q.y) == doctest::Approx(1.5f));
    }
    for (const Vec2 p : {Vec2{-10.f, 4.f}, Vec2{-9.f, 5.f}, Vec2{-12.f, 3.f}, Vec2{-6.5f, 5.f}, Vec2{-14.5f, 0.f}}) {
        const Vec2 q = z.pushOut(p, 0.5f);
        CHECK_MESSAGE(z.obstacleDistance(q) >= 0.5f - 1e-3f, "from (" << p.x << ", " << p.y << ") to (" << q.x << ", " << q.y << ")");
    }
    // A body far from everything stays where it is.
    const Vec2 free{-20.f, -20.f};
    CHECK(z.pushOut(free, 0.5f).x == free.x);
    // constrain: held in the walls and out of the obstacles at once, from anywhere.
    for (float x = -60.f; x <= 60.f; x += 7.5f) {
        for (float y = -60.f; y <= 60.f; y += 7.5f) {
            const Vec2 q = z.constrain(Vec2{x, y}, 0.5f);
            CHECK(z.contains(q, 0.5f - 1e-2f));
            CHECK(z.obstacleDistance(q) >= 0.5f - 1e-2f);
        }
    }
}

TEST_CASE("arena zone step: with a zone that holds the whole ring and no obstacle, the fight is the old fight, step for step") {
    const ArenaTable arena = loadArena();
    const ArenaDef def = fightOf(arena, {"brute", "skirmisher", "breaker"});
    ArenaDef d = def;
    d.ringRadius = 12.f;
    d.spawnDistance = 7.f;
    const ArenaRing ring{Vec2{0.f, 0.f}, 12.f};
    const ArenaZone zone = circleZone(Vec2{0.f, 0.f}, 12.f);
    ArenaFight a, b;
    a.begin(d, ring, Vec2{-4.2f, 0.f}, Vec2{1.f, 0.f}, 1u);
    b.begin(d, ring, Vec2{-4.2f, 0.f}, Vec2{1.f, 0.f}, 1u);
    ArenaBodies pa, pb;
    pa.player = pb.player = a.playerStart();
    for (std::size_t i = 0; i < a.enemyCount(); ++i) pa.enemies[i] = pb.enemies[i] = a.enemyStart(i);
    int steps = 0;
    while (!a.over() && a.time() < 120.f) {
        a.tick(kDt, a.autopilot(), pa);
        b.tick(kDt, b.autopilot(), pb);
        arenaStep(a, pa, kDt);
        arenaStep(b, pb, kDt, zone);
        REQUIRE(pa.player.x == pb.player.x);
        REQUIRE(pa.player.y == pb.player.y);
        for (std::size_t i = 0; i < a.enemyCount(); ++i) {
            REQUIRE(pa.enemies[i].x == pb.enemies[i].x);
            REQUIRE(pa.enemies[i].y == pb.enemies[i].y);
        }
        ++steps;
    }
    CHECK(a.phase() == ArenaPhase::Victory);
    CHECK(b.time() == a.time());
    CHECK(steps > 600);
}

TEST_CASE("arena zone step: allocates nothing over a whole fight in a zone with obstacles") {
    const ArenaTable arena = loadArena();
    const WorldTable worlds = loadWorlds();
    const ArenaZoneTable zones = loadZones(arena, worlds);
    Fitted f;
    fitShipped(arena, worlds, *zones.find("sol_facade_a"), f);
    ArenaFight fight;
    ArenaBodies bodies;
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    fight.begin(f.def, f.fit.ring, f.fit.playerStart, f.fit.playerFacing, 3u);
    int ticks = 0;
    for (int run = 0; run < 2; ++run) {
        bodies.player = fight.playerStart();
        for (std::size_t i = 0; i < fight.enemyCount(); ++i) bodies.enemies[i] = fight.enemyStart(i);
        while (!fight.over() && ticks < 20000) {
            fight.tick(kDt, run == 0 ? fight.autopilot() : ArenaInput{}, bodies);
            arenaStep(fight, bodies, kDt, f.fit.zone);
            ++ticks;
        }
        fight.restart();
    }
    if (counting) CHECK(ftcore_test::allocStats().count - before == 0);
    CHECK(ticks > 300);
}

// ---- the file ----

TEST_CASE("arena zones: Data/Battle/arenas.json loads against arena.json and worlds.json without warnings") {
    const ArenaTable arena = loadArena();
    const WorldTable worlds = loadWorlds();
    std::vector<std::string> warnings;
    const ArenaZoneTable t = loadZones(arena, worlds, &warnings);
    CHECK(warnings.empty());
    REQUIRE(t.defaultZone());
    CHECK(t.defaultZone()->id == "sand_ring");
    CHECK(t.defaultZone()->world.empty());
    CHECK(t.find("nope") == nullptr);
    // At least a human-sized zone and a big one in each of the three worlds.
    for (const char* world : {"solhaven", "lattice", "unwritten"}) {
        int small = 0, large = 0;
        for (const ArenaZoneSpec& z : t.zones) {
            if (z.world != world) continue;
            const ArenaZoneSizing s = arenaZoneSizing(fightOf(arena, z.bosses));
            (s.k <= 1.f ? small : large)++;
        }
        CHECK_MESSAGE(small >= 1, world);
        CHECK_MESSAGE(large >= 1, world);
    }
}

TEST_CASE("arena zones loader: refusals name their path") {
    const ArenaTable arena = loadArena();
    const WorldTable worlds = loadWorlds();
    const auto one = [](const std::string& body) { return R"({ "zones": [ { "id": "x", "bosses": ["brute"], )" + body + " } ] }"; };
    CHECK(contains(zoneError(arena, worlds, R"({ "zones": [] })"), "zones: expected a non-empty array"));
    CHECK(contains(zoneError(arena, worlds, R"({ "zones": [ { "id": "x", "bosses": ["ghost"] } ] })"), "zones[0].bosses[0]: unknown enemy id: ghost"));
    CHECK(contains(zoneError(arena, worlds, R"({ "zones": [ { "id": "x" } ] })"), "zones[0].bosses"));
    CHECK(contains(zoneError(arena, worlds, one(R"("world": "mars")")), "zones[0].world"));
    CHECK(contains(zoneError(arena, worlds, one(R"("world": "solhaven")")), "needs a scene, a district or at"));
    CHECK(contains(zoneError(arena, worlds, one(R"("scene": "SC-NOPE")")), "zones[0].scene"));
    CHECK(contains(zoneError(arena, worlds, one(R"("world": "lattice", "scene": "SC-03-D1")")), "another world"));
    CHECK(contains(zoneError(arena, worlds, one(R"("world": "solhaven", "district": 99)")), "zones[0].district"));
    CHECK(contains(zoneError(arena, worlds, one(R"("shape": { "kind": "hexagon" })")), "zones[0].shape.kind"));
    CHECK(contains(zoneError(arena, worlds, one(R"("shape": { "kind": "polygon", "points": [[1,0],[0,1]] })")), "zones[0].shape.points"));
    CHECK(contains(zoneError(arena, worlds, one(R"("shape": { "kind": "polygon", "points": [[1,0],[0,1],[1,1],[0.1,0.1]] })")), "convex"));
    CHECK(contains(zoneError(arena, worlds, one(R"("ring": 2)")), "zones[0].ring"));
    CHECK(contains(zoneError(arena, worlds, one(R"("offset": [1])")), "zones[0].offset"));
    CHECK(contains(zoneError(arena, worlds, one(R"("at": [5000, 0], "world": "solhaven")")), "zones[0].at"));
    CHECK(contains(zoneError(arena, worlds, R"({ "zones": [ { "id": "x", "bosses": ["brute"] }, { "id": "x", "bosses": ["brute"] } ] })"), "duplicate id: x"));
    CHECK(contains(zoneError(arena, worlds, R"({ "default": "y", "zones": [ { "id": "x", "bosses": ["brute"] } ] })"), "default: expected the id of a zone"));
    CHECK(contains(zoneError(arena, worlds, "{ nope"), "arenas file"));

    ArenaZoneTable t;
    std::vector<std::string> warnings;
    REQUIRE(importArenaZoneTable(R"({ "zones": [ { "id": "x", "bosses": ["brute"], "colour": 1, "shape": { "kind": "circle", "fill": 2 } } ] })", arena,
                                 worlds, t, nullptr, &warnings));
    CHECK(warnings.size() == 2);
    CHECK(t.defaultId == "x");
    // A scene names its world and district.
    REQUIRE(importArenaZoneTable(R"({ "zones": [ { "id": "x", "scene": "SC-03-D2", "bosses": ["brute"] } ] })", arena, worlds, t));
    CHECK(t.zones[0].world == "solhaven");
    CHECK(t.zones[0].district == 3);
}

// ---- the size ----

TEST_CASE("arena zone size: a human gets a tight ring, a giant a huge one, and the ranges are held") {
    const ArenaTable arena = loadArena();
    ArenaZoneSpec spec;
    const ArenaDef duel = fightOf(arena, {"duelist"}), brute = fightOf(arena, {"brute"}), hulk = fightOf(arena, {"hulk"}),
                   titan = fightOf(arena, {"titan"});
    CHECK(arenaZoneSizing(duel).k == 1.f);
    CHECK(arenaZoneSizing(brute).k == 1.f);
    CHECK(arenaZoneSizing(hulk).k > 2.f);
    CHECK(arenaZoneSizing(titan).k > arenaZoneSizing(hulk).k);
    CHECK(arenaZoneSizing(titan).k <= 6.f);
    CHECK(arenaZoneAskedRing(spec, duel) == doctest::Approx(8.5f));
    CHECK(arenaZoneAskedRing(spec, hulk) > 1.6f * 8.5f);
    CHECK(arenaZoneAskedRing(spec, titan) > 1.5f * arenaZoneAskedRing(spec, hulk));
    // More enemies and more attackers widen it; it is held to 7..60 m.
    ArenaDef three = duel;
    three.enemyCount = 3;
    three.enemies[1] = three.enemies[2] = three.enemies[0];
    CHECK(arenaZoneAskedRing(spec, three) > arenaZoneAskedRing(spec, duel));
    ArenaDef threeAtOnce = three;
    threeAtOnce.attackers = 3;
    CHECK(arenaZoneAskedRing(spec, threeAtOnce) > arenaZoneAskedRing(spec, three));
    ArenaDef crowd = titan;
    crowd.enemyCount = 6;
    for (int i = 1; i < 6; ++i) crowd.enemies[static_cast<std::size_t>(i)] = crowd.enemies[0];
    crowd.attackers = 6;
    CHECK(arenaZoneAskedRing(spec, crowd) == 60.f);
    spec.ring = 20.f;
    CHECK(arenaZoneAskedRing(spec, duel) == 20.f);
    // The camera grows with the boss, in held ranges.
    float last = 0.f;
    for (const ArenaDef* d : {&duel, &hulk, &titan}) {
        const ZoneCamera c = arenaZoneCamera(arenaZoneSizing(*d));
        CHECK(c.arm >= last);
        CHECK(c.arm >= 6.4f);
        CHECK(c.arm <= 6.4f * 4.2f + 1e-3f);
        CHECK(c.lockPitch >= -20.f);
        CHECK(c.lockPitch <= -8.f);
        last = c.arm;
    }
    CHECK(arenaZoneCamera(arenaZoneSizing(duel)).arm == doctest::Approx(6.4f));
    CHECK(arenaZoneCamera(arenaZoneSizing(duel)).lockPitch == doctest::Approx(-20.f));
}

// ---- the fit ----

TEST_CASE("arena zone fit: the default zone is the old ring, the same ring, spawn and start") {
    const ArenaTable arena = loadArena();
    const WorldTable worlds = loadWorlds();
    const ArenaZoneTable zones = loadZones(arena, worlds);
    Fitted f;
    fitShipped(arena, worlds, *zones.defaultZone(), f);
    CHECK(f.fit.zone.shape == ZoneShape::Circle);
    CHECK(f.fit.zone.obstacleCount == 0);
    CHECK(f.fit.scale == 1.f);
    CHECK(f.fit.ring.radius == arena.fallback.ringRadius);
    CHECK(f.fit.spawnDistance == arena.fallback.spawnDistance);
    CHECK(f.fit.ring.centre.x == 0.f);
    CHECK(f.fit.ring.centre.y == 0.f);
    CHECK(f.fit.playerStart.x == doctest::Approx(-0.35f * 12.f));
    CHECK(f.fit.playerStart.y == doctest::Approx(0.f));
    CHECK(f.fit.playerFacing.x == doctest::Approx(1.f));
    CHECK(f.def.ringRadius == arena.fallback.ringRadius);
    CHECK(f.warnings.empty());
}

TEST_CASE("arena zone fit: every shipped zone fits its real layout, passes its checks and says what it is made of") {
    const ArenaTable arena = loadArena();
    const WorldTable worlds = loadWorlds();
    const ArenaZoneTable zones = loadZones(arena, worlds);
    for (const ArenaZoneSpec& spec : zones.zones) {
        Fitted f;
        fitShipped(arena, worlds, spec, f);
        std::string err;
        CHECK_MESSAGE(checkArenaZone(f.fit, f.def, f.hasLayout ? &f.layout : nullptr, &err), err);
        const ArenaZone& z = f.fit.zone;
        CHECK(f.fit.ring.radius == doctest::Approx(z.radius));
        CHECK(f.fit.scale >= 0.65f);
        CHECK(f.fit.scale <= 1.f);
        CHECK(z.area() >= 0.5f * 3.14159f * f.fit.askedRing * f.fit.scale * f.fit.askedRing * f.fit.scale - 1.f);
        CHECK(f.fit.spawnDistance <= 12.f);
        // A zone in a world is the world's own space: cut by its buildings or holding some of its things.
        MESSAGE(spec.id << ": " << std::string(z.shape == ZoneShape::Circle ? "a circle" : "a polygon") << " of " << int(z.vertCount) << " verts, " << int(z.obstacleCount)
                         << " obstacles, ring " << f.fit.ring.radius << " m (asked " << f.fit.askedRing << ", scale " << f.fit.scale
                         << "), area " << z.area() << " m2, inscribed radius " << z.inradius() << " m, spawn " << f.fit.spawnDistance
                         << " m, " << int(f.fit.mouthCount) << " road mouths, " << int(f.fit.waterCount) << " water sheets, camera blocked "
                         << f.fit.cameraBlocked << std::string(f.warnings.empty() ? "" : "; " + f.warnings[0]));
    }
}

TEST_CASE("arena zone fit: deterministic, and the walls keep clear of every building they don't hold") {
    const ArenaTable arena = loadArena();
    const WorldTable worlds = loadWorlds();
    const ArenaZoneTable zones = loadZones(arena, worlds);
    for (const ArenaZoneSpec& spec : zones.zones) {
        Fitted a, b;
        fitShipped(arena, worlds, spec, a);
        fitShipped(arena, worlds, spec, b);
        CHECK(a.fit.zone.vertCount == b.fit.zone.vertCount);
        CHECK(a.fit.zone.obstacleCount == b.fit.zone.obstacleCount);
        CHECK(a.fit.zone.radius == b.fit.zone.radius);
        CHECK(a.fit.playerStart.x == b.fit.playerStart.x);
        CHECK(a.fit.playerStart.y == b.fit.playerStart.y);
        if (!a.hasLayout) continue;
        // No building's footprint stands inside the zone unless it is one of its obstacles.
        const ArenaZone& z = a.fit.zone;
        for (std::size_t i = 0; i < a.layout.blockCount(); ++i) {
            const WorldBlock& blk = a.layout.block(i);
            bool obstacle = false;
            for (std::size_t o = 0; o < z.obstacleCount; ++o)
                obstacle = obstacle || (z.obstacles[o].box && std::fabs(z.obstacles[o].at.x - blk.pos.x) < 1e-3f && std::fabs(z.obstacles[o].at.y - blk.pos.y) < 1e-3f);
            if (obstacle) continue;
            // Sample the footprint: its centre and corners must lie outside the walls.
            const float c = std::cos(blk.yaw), s = std::sin(blk.yaw);
            for (const Vec2 k : {Vec2{0.f, 0.f}, Vec2{-1.f, -1.f}, Vec2{1.f, -1.f}, Vec2{1.f, 1.f}, Vec2{-1.f, 1.f}}) {
                const Vec2 lx{k.x * blk.width * 0.5f, k.y * blk.depth * 0.5f};
                const Vec2 p{blk.pos.x + lx.x * c - lx.y * s, blk.pos.y + lx.x * s + lx.y * c};
                CHECK_MESSAGE(!z.contains(p, 0.f), spec.id << ": a building's footprint lies inside the walls");
            }
        }
    }
}

TEST_CASE("arena zone fit: a crowded spot shrinks with a warning, a hopeless one is refused") {
    const ArenaTable arena = loadArena();
    const WorldTable worlds = loadWorlds();
    WorldLayout layout;
    std::string err;
    REQUIRE_MESSAGE(generateWorldLayout(worlds.worlds[0], layout, &err), err);

    ArenaZoneSpec spec;
    spec.id = "crowded";
    spec.world = "solhaven";
    spec.district = 2;
    spec.bosses = {"hulk"};
    spec.offset = Vec2{-50.f, -12.f}; // right at the building ring, with the landmark beside it
    const ArenaDef def = fightOf(arena, spec.bosses);
    ArenaZoneFit fit;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(fitArenaZone(spec, &layout, def, fit, &err, &warnings), err);
    CHECK(fit.scale < 1.f);
    CHECK(fit.scale >= 0.65f);
    REQUIRE_FALSE(warnings.empty());
    CHECK(contains(warnings[0], "shrunk"));
    CHECK(checkArenaZone(fit, def, &layout, &err));

    // A spot off the district's flat pad has no room at any size.
    spec.id = "off the pad";
    spec.offset = Vec2{-30.f, 60.f};
    CHECK_FALSE(fitArenaZone(spec, &layout, def, fit, &err, &warnings));
    CHECK(contains(err, "off the pad"));
    CHECK(contains(err, "no room"));
    // So is a spot in the middle of the building ring for a titan.
    spec.id = "titan in the ring";
    spec.offset = Vec2{-49.f, 0.f};
    CHECK_FALSE(fitArenaZone(spec, &layout, fightOf(arena, {"titan"}), fit, &err, &warnings));
    // A bigger boss at the same place gets a bigger zone than a smaller one.
    spec.id = "same place";
    spec.offset = Vec2{-46.f, -12.f};
    ArenaZoneFit dFit, hFit;
    REQUIRE(fitArenaZone(spec, &layout, fightOf(arena, {"duelist"}), dFit, &err));
    REQUIRE(fitArenaZone(spec, &layout, fightOf(arena, {"hulk"}), hFit, &err));
    CHECK(hFit.ring.radius > dFit.ring.radius);
}

TEST_CASE("arena zone fit: the pinned ring, spawn and attackers of a spec stand, and a polygon scales with the ring") {
    const ArenaTable arena = loadArena();
    ArenaZoneSpec spec;
    spec.id = "pinned";
    spec.bosses = {"duelist"};
    spec.ring = 20.f;
    spec.spawn = 9.f;
    ArenaZoneFit fit;
    std::string err;
    REQUIRE_MESSAGE(fitArenaZone(spec, nullptr, fightOf(arena, spec.bosses), fit, &err), err);
    CHECK(fit.ring.radius == doctest::Approx(20.f));
    CHECK(fit.spawnDistance == doctest::Approx(9.f));
    // A hexagon in multiples of the ring radius.
    spec.kind = ZoneKind::Polygon;
    spec.points = {Vec2{1.f, 0.f}, Vec2{0.5f, 0.87f}, Vec2{-0.5f, 0.87f}, Vec2{-1.f, 0.f}, Vec2{-0.5f, -0.87f}, Vec2{0.5f, -0.87f}};
    spec.ring = 0.f;
    spec.spawn = 0.f;
    REQUIRE_MESSAGE(fitArenaZone(spec, nullptr, fightOf(arena, spec.bosses), fit, &err), err);
    CHECK(fit.zone.shape == ZoneShape::Polygon);
    CHECK(fit.zone.vertCount == 6);
    CHECK(fit.zone.radius == doctest::Approx(8.5f * fit.scale).epsilon(0.05));
    CHECK(checkArenaZone(fit, fightOf(arena, spec.bosses), nullptr, &err));
}

TEST_CASE("arena zone check: a start in a wall, a lane through an obstacle and a wrong ring are refused") {
    const ArenaTable arena = loadArena();
    const WorldTable worlds = loadWorlds();
    const ArenaZoneTable zones = loadZones(arena, worlds);
    Fitted f;
    fitShipped(arena, worlds, *zones.find("sol_facade_a"), f);
    std::string err;
    REQUIRE(checkArenaZone(f.fit, f.def, &f.layout, &err));
    {
        ArenaZoneFit bad = f.fit;
        bad.ring.radius += 1.f;
        CHECK_FALSE(checkArenaZone(bad, f.def, &f.layout, &err));
        CHECK(contains(err, "bounding circle"));
    }
    {
        ArenaZoneFit bad = f.fit;
        bad.playerStart = Vec2{bad.zone.centre.x + 100.f, bad.zone.centre.y};
        CHECK_FALSE(checkArenaZone(bad, f.def, &f.layout, &err));
        CHECK(contains(err, "outside the walls"));
    }
    {
        // An obstacle on the lane between the starts.
        ArenaZoneFit bad = f.fit;
        ArenaFight fight;
        fight.begin(f.def, bad.ring, bad.playerStart, bad.playerFacing, 1u);
        ZoneObstacle o;
        o.at = Vec2{(fight.playerStart().x + fight.enemyStart(0).x) * 0.5f, (fight.playerStart().y + fight.enemyStart(0).y) * 0.5f};
        o.radius = 0.8f;
        bad.zone.obstacles[bad.zone.obstacleCount++] = o;
        CHECK_FALSE(checkArenaZone(bad, f.def, &f.layout, &err));
        CHECK(contains(err, "lane"));
    }
    {
        // Leaving the district's flat pad.
        ArenaZoneFit bad = f.fit;
        bad.district = 0;
        CHECK_FALSE(checkArenaZone(bad, f.def, &f.layout, &err));
        CHECK(contains(err, "flat pad"));
    }
}

// ---- the fights the zones hold ----

TEST_CASE("arena zone fights: the autopilot wins every shipped zone inside its walls, and a player pressing nothing loses") {
    const ArenaTable arena = loadArena();
    const WorldTable worlds = loadWorlds();
    const ArenaZoneTable zones = loadZones(arena, worlds);
    for (const ArenaZoneSpec& spec : zones.zones) {
        Fitted f;
        fitShipped(arena, worlds, spec, f);
        float slowest = 0.f;
        for (std::uint32_t seed = 1; seed <= 6; ++seed) {
            const Play win = playIn(f, seed, true);
            const Play lose = playIn(f, seed, false);
            CHECK_MESSAGE(win.phase == ArenaPhase::Victory, spec.id << " seed " << seed << ": autopilot did not win in 120 s");
            CHECK_MESSAGE(lose.phase == ArenaPhase::Defeat, spec.id << " seed " << seed << ": a player pressing nothing survived");
            CHECK_MESSAGE(win.held, spec.id << " seed " << seed << ": a body left the walls or entered an obstacle");
            CHECK_MESSAGE(lose.held, spec.id << " seed " << seed << ": a body left the walls or entered an obstacle");
            slowest = std::max(slowest, win.time);
        }
        MESSAGE(spec.id << ": ring " << f.def.ringRadius << " m, spawn " << f.def.spawnDistance << " m, slowest win " << slowest << " s");
    }
}

// ---- the level designer's zones, in the district frame ----

namespace {

// p (world metres) in district d's plaza-centred frame: +x the way its spawn faces.
Vec2 inDistrictFrame(const WorldLayout& layout, std::size_t d, Vec2 p) {
    const WorldDistrict& dist = layout.district(d);
    const float dx = p.x - dist.centre.x, dy = p.y - dist.centre.y;
    return Vec2{dx * dist.forward.x + dy * dist.forward.y, -dx * dist.forward.y + dy * dist.forward.x};
}

} // namespace

TEST_CASE("arena zone frame: every district's plaza centre and facing, from generateWorldLayout (for checking the level notes)") {
    const WorldTable worlds = loadWorlds();
    for (const WorldSpec& w : worlds.worlds) {
        WorldLayout layout;
        std::string err;
        REQUIRE_MESSAGE(generateWorldLayout(w, layout, &err), err);
        std::ostringstream out;
        out << w.id << " (" << layout.districtCount() << " districts, " << layout.roadCount() << " roads, " << layout.blockCount() << " buildings):";
        for (std::size_t d = 0; d < layout.districtCount(); ++d) {
            const WorldDistrict& dist = layout.district(d);
            const Vec2 lm = inDistrictFrame(layout, d, dist.landmark);
            out << "\n  district " << d << ": plaza centre (" << dist.centre.x << ", " << dist.centre.y << "), faces "
                << std::atan2(dist.forward.y, dist.forward.x) * 57.2958f << " deg, landmark at (" << lm.x << ", " << lm.y << ") in its frame";
            // The frame itself: the plaza centre is the origin and the spawn lies on the -x side of it.
            CHECK(inDistrictFrame(layout, d, dist.centre).x == doctest::Approx(0.f).epsilon(1e-3).scale(1.f));
            CHECK(inDistrictFrame(layout, d, dist.origin).x == doctest::Approx(-WorldLayout::kPlazaCentreX).epsilon(1e-3));
            CHECK(inDistrictFrame(layout, d, dist.origin).y == doctest::Approx(0.f).epsilon(1e-3).scale(1.f));
        }
        MESSAGE(out.str());
    }
}

TEST_CASE("arena zone table: the nine on-pad zones are in the file, with the level notes' anchors, shapes and pins") {
    const ArenaTable arena = loadArena();
    const WorldTable worlds = loadWorlds();
    const ArenaZoneTable zones = loadZones(arena, worlds);
    struct Row {
        const char* id;
        const char* world;
        int district;
        float x, y, yaw;
        ZoneKind kind;
        float ring;
    };
    const Row rows[] = {
        {"sol_plaza_a", "solhaven", 3, 0, 0, 180, ZoneKind::Circle, 0},   {"sol_facade_a", "solhaven", 2, 0, 34, 90, ZoneKind::Rect, 0},
        {"sol_climax_a", "solhaven", 14, 22, 0, 0, ZoneKind::Circle, 36}, {"lat_plaza_a", "lattice", 6, 0, 0, 180, ZoneKind::Circle, 0},
        {"lat_facade_a", "lattice", 5, 0, -34, -90, ZoneKind::Rect, 0},   {"lat_climax_a", "lattice", 1, 22, 0, 0, ZoneKind::Circle, 36},
        {"unw_plaza_a", "unwritten", 3, 0, 0, 180, ZoneKind::Circle, 0},  {"unw_court_a", "unwritten", 3, 0, 0, 180, ZoneKind::Circle, 54},
        {"unw_climax_a", "unwritten", 4, 22, 0, 0, ZoneKind::Circle, 36},
    };
    for (const Row& r : rows) {
        const ArenaZoneSpec* z = zones.find(r.id);
        REQUIRE_MESSAGE(z, r.id);
        CHECK(z->world == r.world);
        CHECK(z->district == r.district);
        CHECK(z->offset.x == r.x);
        CHECK(z->offset.y == r.y);
        CHECK(z->yaw == r.yaw);
        CHECK((z->kind == r.kind));
        CHECK(z->ring == r.ring);
    }
    // Every world has water in its zones.
    for (const char* world : {"solhaven", "lattice", "unwritten"}) {
        std::size_t sheets = 0;
        for (const ArenaZoneSpec& z : zones.zones)
            if (z.world == world) sheets += z.water.size();
        CHECK_MESSAGE(sheets > 0, world);
    }
}

TEST_CASE("arena zone fit: the lane starts are the level notes' (district frame), and nothing is shrunk or moved") {
    const ArenaTable arena = loadArena();
    const WorldTable worlds = loadWorlds();
    const ArenaZoneTable zones = loadZones(arena, worlds);
    struct Row {
        const char* id;
        float px, py, bx, by;
    };
    // The plazas with the large boss, and the climax zones with the giant.
    const Row rows[] = {{"sol_plaza_a", 5.5f, 0.f, -3.6f, 0.f}, {"lat_plaza_a", 5.5f, 0.f, -3.6f, 0.f}, {"sol_climax_a", 9.4f, 0.f, 21.4f, 0.f},
                        {"lat_climax_a", 9.4f, 0.f, 21.4f, 0.f}, {"unw_climax_a", 9.4f, 0.f, 21.4f, 0.f}};
    for (const Row& r : rows) {
        Fitted f;
        fitShipped(arena, worlds, *zones.find(r.id), f);
        CHECK(f.fit.scale == 1.f);
        ArenaFight fight;
        fight.begin(f.def, f.fit.ring, f.fit.playerStart, f.fit.playerFacing, 1u);
        const Vec2 p = inDistrictFrame(f.layout, static_cast<std::size_t>(f.spec.district), fight.playerStart());
        const Vec2 b = inDistrictFrame(f.layout, static_cast<std::size_t>(f.spec.district), fight.enemyStart(0));
        CHECK_MESSAGE(std::fabs(p.x - r.px) < 0.15f, r.id << " player x " << p.x);
        CHECK_MESSAGE(std::fabs(p.y - r.py) < 0.15f, r.id << " player y " << p.y);
        CHECK_MESSAGE(std::fabs(b.x - r.bx) < 0.15f, r.id << " boss x " << b.x);
        CHECK_MESSAGE(std::fabs(b.y - r.by) < 0.15f, r.id << " boss y " << b.y);
    }
    // The facades are pressed on the building faces: cut by them, and at their full size.
    for (const char* id : {"sol_facade_a", "lat_facade_a"}) {
        Fitted f;
        fitShipped(arena, worlds, *zones.find(id), f);
        CHECK_MESSAGE((f.fit.zone.shape == ZoneShape::Polygon), id);
        CHECK(f.fit.scale == 1.f);
        CHECK(f.fit.mouthCount == 0); // the facade's walls meet no road
    }
}

TEST_CASE("arena zone fit: the court's ruin walls become cover and its landmark an obstacle; a plaza holds nothing") {
    const ArenaTable arena = loadArena();
    const WorldTable worlds = loadWorlds();
    const ArenaZoneTable zones = loadZones(arena, worlds);
    Fitted court, plaza;
    fitShipped(arena, worlds, *zones.find("unw_court_a"), court);
    fitShipped(arena, worlds, *zones.find("unw_plaza_a"), plaza);
    int boxes = 0, circles = 0;
    for (std::size_t i = 0; i < court.fit.zone.obstacleCount; ++i) (court.fit.zone.obstacles[i].box ? boxes : circles)++;
    CHECK(boxes >= 5);
    CHECK(circles >= 1); // the landmark
    CHECK(plaza.fit.zone.obstacleCount == 0);
}

TEST_CASE("arena zone fit: road mouths lie on the wall and on a road, and a plain zone has none") {
    const ArenaTable arena = loadArena();
    const WorldTable worlds = loadWorlds();
    const ArenaZoneTable zones = loadZones(arena, worlds);
    int total = 0;
    for (const ArenaZoneSpec& spec : zones.zones) {
        Fitted f;
        fitShipped(arena, worlds, spec, f);
        if (!f.hasLayout) {
            CHECK(f.fit.mouthCount == 0);
            continue;
        }
        for (std::size_t m = 0; m < f.fit.mouthCount; ++m) {
            const ZoneMouth& mouth = f.fit.mouths[m];
            ++total;
            CHECK_MESSAGE(std::fabs(f.fit.zone.wallDistance(mouth.at)) < 1.5f, spec.id << ": a mouth is off the wall");
            CHECK_MESSAGE(f.layout.roadDistance(mouth.at) <= WorldLayout::kRoadWidth * 0.5f + 1.f, spec.id << ": a mouth is off the road");
            CHECK(mouth.width >= WorldLayout::kRoadWidth);
            CHECK(std::sqrt(mouth.out.x * mouth.out.x + mouth.out.y * mouth.out.y) == doctest::Approx(1.f));
        }
    }
    CHECK(total >= 3); // the plazas are crossed by their roads
}

TEST_CASE("arena zone fit: the camera room of a giant: a climax half is open, a tower plaza is not") {
    const ArenaTable arena = loadArena();
    const WorldTable worlds = loadWorlds();
    const ArenaZoneTable zones = loadZones(arena, worlds);
    // Every shipped zone keeps the camera clear of the buildings for most of its edge.
    for (const ArenaZoneSpec& spec : zones.zones) {
        Fitted f;
        fitShipped(arena, worlds, spec, f);
        CHECK_MESSAGE(f.fit.cameraBlocked <= 0.3f, spec.id << " camera blocked " << f.fit.cameraBlocked);
    }
    // The same plaza with the giant, in the tower world, has the camera in the wall along much of its edge.
    ArenaZoneSpec spec = *zones.find("lat_plaza_a");
    spec.bosses = {"titan"};
    Fitted f;
    fitShipped(arena, worlds, spec, f);
    MESSAGE("lat_plaza_a with the titan: camera blocked " << f.fit.cameraBlocked << ", ring " << f.fit.ring.radius);
    CHECK(f.fit.cameraBlocked > 0.1f);
    bool said = false;
    for (const std::string& w : f.warnings) said = said || contains(w, "camera");
    CHECK(said);
}

TEST_CASE("arena zone water and cover: the file's lists are read, put in the world, and kept off the lanes") {
    const ArenaTable arena = loadArena();
    const WorldTable worlds = loadWorlds();
    ArenaZoneTable t;
    std::string err;
    const std::string json = R"({ "zones": [ { "id": "x", "world": "solhaven", "district": 3, "bosses": ["duelist"], "yaw": 180,
        "water": [ { "at": [-6, 8], "radius": 2 } ],
        "cover": [ { "kind": "box", "at": [4, 6], "half": [1.5, 0.8], "yaw": 30 }, { "kind": "circle", "at": [-4, -6], "radius": 0.8 },
                   { "kind": "circle", "at": [90, 90], "radius": 1 } ] } ] })";
    REQUIRE_MESSAGE(importArenaZoneTable(json, arena, worlds, t, &err), err);
    REQUIRE(t.zones[0].water.size() == 1);
    REQUIRE(t.zones[0].cover.size() == 3);
    CHECK(t.zones[0].cover[0].box);
    CHECK(t.zones[0].cover[0].yaw == 30.f);

    WorldLayout layout;
    REQUIRE(generateWorldLayout(worlds.worlds[0], layout, &err));
    ArenaZoneFit fit;
    REQUIRE_MESSAGE(fitArenaZone(t.zones[0], &layout, fightOf(arena, {"duelist"}), fit, &err), err);
    CHECK(fit.waterCount == 1);
    int authored = 0;
    for (std::size_t i = 0; i < fit.zone.obstacleCount; ++i) authored += fit.zone.obstacles[i].authored ? 1 : 0;
    CHECK(authored == 2); // the third piece is far outside the zone
    const Vec2 sheet = inDistrictFrame(layout, 3, fit.water[0].at);
    CHECK(sheet.x == doctest::Approx(-6.f).epsilon(1e-3));
    CHECK(sheet.y == doctest::Approx(8.f).epsilon(1e-3));
    CHECK(checkArenaZone(fit, fightOf(arena, {"duelist"}), &layout, &err));
    // A sheet on the starts is refused.
    ArenaZoneFit bad = fit;
    bad.water[0].at = fit.playerStart;
    CHECK_FALSE(checkArenaZone(bad, fightOf(arena, {"duelist"}), &layout, &err));
    CHECK(contains(err, "water sheet"));

    // Refusals.
    CHECK(contains(zoneError(arena, worlds, R"({ "zones": [ { "id": "x", "bosses": ["brute"], "water": [ { "at": [1], "radius": 2 } ] } ] })"), "zones[0].water[0].at"));
    CHECK(contains(zoneError(arena, worlds, R"({ "zones": [ { "id": "x", "bosses": ["brute"], "cover": [ { "kind": "cone", "at": [1, 1] } ] } ] })"), "zones[0].cover[0].kind"));
}

TEST_CASE("arena zone step: a zone in a world brakes a body at its wall to 60% and slides it along; the plain zone does not") {
    ArenaZone z = squareZone(20.f);
    CHECK(z.wallDistance(Vec2{0.f, 0.f}) == doctest::Approx(20.f));
    CHECK(z.wallDistance(Vec2{19.f, 0.f}) == doctest::Approx(1.f));
    CHECK(z.wallDistance(Vec2{25.f, 0.f}) < 0.f);
    CHECK(circleZone(Vec2{}, 10.f).wallDistance(Vec2{4.f, 0.f}) == doctest::Approx(6.f));

    const ArenaTable arena = loadArena();
    ArenaDef def = fightOf(arena, {"duelist"});
    def.ringRadius = 40.f;
    def.spawnDistance = 6.f;
    float moved[2] = {0.f, 0.f};
    for (int soft = 0; soft < 2; ++soft) {
        z.softWall = soft == 1;
        ArenaFight fight;
        fight.begin(def, ArenaRing{Vec2{0.f, 0.f}, 40.f}, Vec2{-3.f, 0.f}, Vec2{1.f, 0.f}, 1u);
        ArenaBodies bodies;
        bodies.player = Vec2{17.5f, 0.f}; // 2.1 m from touching the wall
        for (std::size_t i = 0; i < fight.enemyCount(); ++i) bodies.enemies[i] = Vec2{-10.f, 10.f};
        ArenaInput in;
        in.move = Vec2{1.f, 0.f};
        for (int i = 0; i < 60; ++i) {
            fight.tick(kDt, in, bodies);
            arenaStep(fight, bodies, kDt, z);
            CHECK(z.contains(bodies.player, def.fighter.bodyRadius - 0.02f));
            if (i == 24) moved[soft] = bodies.player.x - 17.5f; // a short way in: the braked body is behind
        }
        // Either way it ends at the wall.
        CHECK(bodies.player.x == doctest::Approx(20.f - def.fighter.bodyRadius).epsilon(0.01));
    }
    CHECK(moved[1] < moved[0]);
}
