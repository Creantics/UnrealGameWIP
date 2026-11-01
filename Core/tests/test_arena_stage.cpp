#include "ftcore/ArenaStage.h"

#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
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

ArenaStageTable loadStages(const ArenaTable& arena, std::vector<std::string>* warnings = nullptr) {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/arenas.json", text)) FAIL("Data/Battle/arenas.json not found");
    ArenaStageTable t;
    std::string err;
    REQUIRE_MESSAGE(importArenaStageTable(text, arena, t, &err, warnings), err);
    return t;
}

// The fight a stage holds: its bosses against the default fighter, fitted to the stage.
ArenaDef stageFight(const ArenaTable& arena, const ArenaStageSpec& spec) {
    ArenaDef def = arena.fallback;
    def.enemyCount = static_cast<std::uint8_t>(spec.bosses.size());
    for (std::size_t i = 0; i < spec.bosses.size(); ++i) {
        for (std::size_t e = 0; e < arena.enemyIds.size(); ++e) {
            if (arena.enemyIds[e] == spec.bosses[i]) {
                def.enemies[i] = arena.enemies[e];
                def.enemyKind[i] = static_cast<std::uint8_t>(e);
            }
        }
    }
    def.attackers = 1;
    arenaStageFit(spec, def);
    return def;
}

ArenaDef oneEnemy(const ArenaTable& arena, const char* id) {
    ArenaStageSpec spec;
    spec.bosses = {id};
    return stageFight(arena, spec);
}

std::string loadError(const ArenaTable& arena, const std::string& json) {
    ArenaStageTable t;
    std::string err;
    CHECK_FALSE(importArenaStageTable(json, arena, t, &err));
    CHECK(t.stages.empty());
    return err;
}

bool contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

struct Play {
    ArenaPhase phase = ArenaPhase::NotStarted;
    float time = 0.f;
};

// Plays a fight on its stage's ring at a fixed 1/60 s with arenaStep, the autopilot or a player pressing nothing.
Play playOn(const ArenaDef& def, std::uint32_t seed, bool autopilot) {
    ArenaFight fight;
    fight.begin(def, arenaStageRing(def.ringRadius), arenaStagePlayerStart(def.ringRadius), Vec2{1.f, 0.f}, seed);
    ArenaBodies bodies;
    bodies.player = fight.playerStart();
    for (std::size_t i = 0; i < fight.enemyCount(); ++i) bodies.enemies[i] = fight.enemyStart(i);
    while (!fight.over() && fight.time() < 120.f) {
        fight.tick(kDt, autopilot ? fight.autopilot() : ArenaInput{}, bodies);
        arenaStep(fight, bodies, kDt);
    }
    return Play{fight.phase(), fight.time()};
}

float tallest(const ArenaStageLayout& l, StageGroup group) {
    float h = 0.f;
    for (const StagePiece& p : l.pieces)
        if (p.group == group) h = std::max(h, p.z + p.sz);
    return h;
}

const std::string kArenaJson = R"({ "version": 1,
  "enemies": [ { "id": "a", "moves": [ { "tell": "overhead", "startup": 0.4 }, { "tell": "thrust", "startup": 0.4 } ] } ],
  "fighters": [ { "id": "default" } ],
  "default": { "enemies": ["a"] } })";

ArenaTable tinyArena() {
    ArenaTable t;
    std::string err;
    REQUIRE_MESSAGE(importArenaTable(kArenaJson, t, &err), err);
    return t;
}

} // namespace

// ---- the data ----

TEST_CASE("arena stage: Data/Battle/arenas.json loads against arena.json without warnings") {
    const ArenaTable arena = loadArena();
    std::vector<std::string> warnings;
    const ArenaStageTable t = loadStages(arena, &warnings);
    CHECK(warnings.empty());
    CHECK(t.stages.size() >= 7);
    REQUIRE(t.defaultStage());
    CHECK(t.defaultStage()->id == t.defaultId);
    CHECK(t.find("nope") == nullptr);
    // Two stages in each of the three worlds, one human-sized and one large.
    for (StylePack world : {StylePack::Solhaven, StylePack::Lattice, StylePack::Unwritten}) {
        int small = 0, large = 0;
        for (const ArenaStageSpec& s : t.stages) {
            if (s.world != world) continue;
            const ArenaStageSizing z = arenaStageSizing(stageFight(arena, s));
            (z.k <= 1.f ? small : large)++;
        }
        CHECK_MESSAGE(small >= 1, stylePackName(world));
        CHECK_MESSAGE(large >= 1, stylePackName(world));
    }
}

TEST_CASE("arena stage loader: refusals name their path") {
    const ArenaTable arena = tinyArena();
    CHECK(contains(loadError(arena, R"({ "stages": [] })"), "stages: expected a non-empty array"));
    CHECK(contains(loadError(arena, R"({ "stages": [ { "id": "x", "bosses": ["ghost"] } ] })"), "stages[0].bosses[0]: unknown enemy id: ghost"));
    CHECK(contains(loadError(arena, R"({ "stages": [ { "id": "x" } ] })"), "stages[0].bosses"));
    CHECK(contains(loadError(arena, R"({ "stages": [ { "id": "x", "bosses": ["a"], "floor": "hexagon" } ] })"), "stages[0].floor: expected one of circle"));
    CHECK(contains(loadError(arena, R"({ "stages": [ { "id": "x", "bosses": ["a"], "bound": 3 } ] })"), "stages[0].bound"));
    CHECK(contains(loadError(arena, R"({ "stages": [ { "id": "x", "bosses": ["a"], "world": "mars" } ] })"), "stages[0].world"));
    CHECK(contains(loadError(arena, R"({ "stages": [ { "id": "x", "bosses": ["a"], "ring": 2 } ] })"), "stages[0].ring"));
    CHECK(contains(loadError(arena, R"({ "stages": [ { "id": "x", "bosses": ["a"], "features": { "pillars": 99 } } ] })"), "stages[0].features.pillars"));
    CHECK(contains(loadError(arena, R"({ "stages": [ { "id": "x", "bosses": ["a"] }, { "id": "x", "bosses": ["a"] } ] })"), "duplicate id: x"));
    CHECK(contains(loadError(arena, R"({ "default": "y", "stages": [ { "id": "x", "bosses": ["a"] } ] })"), "default: expected the id of a stage"));
    CHECK(contains(loadError(arena, R"({ "version": 2, "stages": [ { "id": "x", "bosses": ["a"] } ] })"), "version"));
    CHECK(contains(loadError(arena, "{ not json"), "arenas file"));

    ArenaStageTable t;
    std::vector<std::string> warnings;
    REQUIRE(importArenaStageTable(R"({ "stages": [ { "id": "x", "bosses": ["a"], "colour": 3, "features": { "mist": 1 } } ] })", arena, t,
                                  nullptr, &warnings));
    CHECK(warnings.size() == 2);
    CHECK(t.defaultId == "x"); // without "default", the first
    CHECK(t.stages[0].floor == StageFloor::Circle);
    CHECK(t.stages[0].lightIntensity == 1.f);
}

TEST_CASE("arena stage names round trip") {
    CHECK(std::string(stageFloorName(StageFloor::BrokenRing)) == "broken_ring");
    CHECK(std::string(stageBoundName(StageBound::FogWall)) == "fog_wall");
    CHECK(std::string(stageThemeName(StageTheme::Void)) == "void");
    const ArenaTable arena = tinyArena();
    ArenaStageTable t;
    REQUIRE(importArenaStageTable(R"({ "stages": [ { "id": "x", "bosses": ["a"], "floor": "broken_ring", "bound": "fog_wall", "theme": "void" } ] })",
                                  arena, t));
    CHECK(t.stages[0].floor == StageFloor::BrokenRing);
    CHECK(t.stages[0].bound == StageBound::FogWall);
    CHECK(t.stages[0].theme == StageTheme::Void);
}

TEST_CASE("arena enemy height: the key, and the derived default") {
    const ArenaTable arena = loadArena();
    REQUIRE(arena.enemyIds.size() >= 5);
    for (std::size_t e = 0; e < arena.enemies.size(); ++e) {
        const float h = arenaEnemyHeight(arena.enemies[e]);
        CHECK_MESSAGE(h >= 1.4f, arena.enemyIds[e]);
        CHECK_MESSAGE(h <= 30.f, arena.enemyIds[e]);
    }
    ArenaEnemyKind kind;
    kind.bodyRadius = 0.4f;
    CHECK(arenaEnemyHeight(kind) == doctest::Approx(1.75f));
    kind.bodyRadius = 0.45f;
    CHECK(arenaEnemyHeight(kind) == doctest::Approx(1.75f * 0.45f / 0.4f));
    kind.bodyRadius = 5.f;
    CHECK(arenaEnemyHeight(kind) == doctest::Approx(1.75f * 1.6f)); // held to 1.6x
    kind.height = 7.f;
    CHECK(arenaEnemyHeight(kind) == 7.f);

    ArenaTable t;
    std::string err;
    const std::string bad = R"({ "enemies": [ { "id": "a", "height": 0, "moves": [ { "tell": "overhead" } ] } ], "fighters": [ { "id": "default" } ], "default": { "enemies": ["a"] } })";
    CHECK_FALSE(importArenaTable(bad, t, &err));
    CHECK(contains(err, "enemies[0].height"));
    // A giant in the file is as tall as it says.
    const auto titan = std::find(arena.enemyIds.begin(), arena.enemyIds.end(), "titan");
    REQUIRE(titan != arena.enemyIds.end());
    CHECK(arenaEnemyHeight(arena.enemies[static_cast<std::size_t>(titan - arena.enemyIds.begin())]) >= 6.5f);
}

// ---- scale ----

TEST_CASE("arena stage scale: a human gets a tight ring, a giant a huge one, and the ranges are held") {
    const ArenaTable arena = loadArena();
    const ArenaDef duel = oneEnemy(arena, "duelist");
    const ArenaDef brute = oneEnemy(arena, "brute");
    const ArenaDef hulk = oneEnemy(arena, "hulk");
    const ArenaDef titan = oneEnemy(arena, "titan");

    CHECK(arenaStageSizing(duel).k == 1.f);
    CHECK(arenaStageSizing(brute).k == 1.f);
    CHECK(arenaStageSizing(hulk).k > 2.f);
    CHECK(arenaStageSizing(titan).k > arenaStageSizing(hulk).k);
    CHECK(arenaStageSizing(titan).k <= 6.f);
    CHECK(arenaStageSizing(titan).propScale > arenaStageSizing(hulk).propScale);

    CHECK(duel.ringRadius == doctest::Approx(8.5f));
    CHECK(hulk.ringRadius > 1.6f * duel.ringRadius);
    CHECK(titan.ringRadius > 1.5f * hulk.ringRadius);
    for (const ArenaDef* d : {&duel, &brute, &hulk, &titan}) {
        CHECK(d->ringRadius >= 7.f);
        CHECK(d->ringRadius <= 60.f);
        CHECK(d->spawnDistance <= 12.f); // the lock reaches 15 m
        CHECK(d->spawnDistance >= d->enemies[0].bodyRadius + 4.f);
        CHECK(d->spawnDistance <= 0.9f * d->ringRadius);
    }

    // More enemies and more attackers widen the ring.
    ArenaStageSpec spec;
    ArenaDef three = duel;
    three.enemyCount = 3;
    three.enemies[1] = three.enemies[2] = three.enemies[0];
    arenaStageFit(spec, three);
    CHECK(three.ringRadius > duel.ringRadius);
    ArenaDef threeAtOnce = three;
    threeAtOnce.attackers = 3;
    arenaStageFit(spec, threeAtOnce);
    CHECK(threeAtOnce.ringRadius > three.ringRadius);

    // Six giants: held to the top of the range.
    ArenaDef crowd = titan;
    crowd.enemyCount = 6;
    for (int i = 1; i < 6; ++i) crowd.enemies[static_cast<std::size_t>(i)] = crowd.enemies[0];
    crowd.attackers = 6;
    arenaStageFit(spec, crowd);
    CHECK(crowd.ringRadius == 60.f);
    CHECK(crowd.spawnDistance == 12.f);

    // A pinned ring and spawn stand; a stage's attackers apply but never exceed the enemies.
    ArenaStageSpec pinned;
    pinned.ring = 20.f;
    pinned.spawn = 9.f;
    pinned.attackers = 5;
    ArenaDef d = duel;
    arenaStageFit(pinned, d);
    CHECK(d.ringRadius == 20.f);
    CHECK(d.spawnDistance == 9.f);
    CHECK(d.attackers == 1); // one enemy
}

TEST_CASE("arena stage: the default stage keeps today's ring, spawn and wave") {
    const ArenaTable arena = loadArena();
    const ArenaStageTable stages = loadStages(arena);
    const ArenaStageSpec* spec = stages.defaultStage();
    REQUIRE(spec);
    CHECK(spec->id == "sand_ring");
    ArenaDef def = stageFight(arena, *spec);
    CHECK(def.ringRadius == arena.fallback.ringRadius);
    CHECK(def.spawnDistance == arena.fallback.spawnDistance);
    CHECK(def.attackers == arena.fallback.attackers);
    REQUIRE(spec->bosses.size() == 3);
    CHECK(spec->bosses[0] == "brute");
    CHECK(spec->bosses[1] == "skirmisher");
    CHECK(spec->bosses[2] == "breaker");
    // The shipped wave is the camera's reference: the old rig exactly.
    ArenaStageLayout l;
    std::string err;
    REQUIRE_MESSAGE(generateArenaStage(*spec, def, l, &err), err);
    CHECK(l.camera.arm == doctest::Approx(6.4f));
    CHECK(l.camera.pivot == doctest::Approx(1.5f));
    CHECK(l.camera.lockPitch == doctest::Approx(-20.f));
    CHECK(l.sizing.propScale == 1.f);
    CHECK(l.count(StageGroup::Pillar) == 8);
    CHECK(l.lights.size() == 8);
}

// ---- the generator ----

TEST_CASE("arena stage: every shipped stage generates, passes its checks and has water in a world") {
    const ArenaTable arena = loadArena();
    const ArenaStageTable stages = loadStages(arena);
    for (const ArenaStageSpec& spec : stages.stages) {
        const ArenaDef def = stageFight(arena, spec);
        ArenaStageLayout l;
        std::string err;
        CHECK_MESSAGE(generateArenaStage(spec, def, l, &err), err);
        CHECK_MESSAGE(checkArenaStage(l, def, &err, false), err);
        CHECK(l.id == spec.id);
        CHECK(l.ringRadius == def.ringRadius);
        CHECK(l.spawnDistance == def.spawnDistance);
        CHECK_FALSE(l.pieces.empty());
        CHECK(l.pieces.size() < 1200);
        CHECK(l.lights.size() == static_cast<std::size_t>(spec.lights));
        CHECK(l.count(StageGroup::Pillar) >= static_cast<std::size_t>(spec.pillars));
        if (spec.world != StylePack::None) CHECK_MESSAGE(l.hasWater(), spec.id);
        if (spec.landmarks > 0) CHECK(l.count(StageGroup::Landmark) >= static_cast<std::size_t>(spec.landmarks));
        CHECK(l.count(StageGroup::Rim) >= 24);
        CHECK(l.floorRadius > l.ringRadius);
    }
}

TEST_CASE("arena stage: the same spec gives the same layout, another seed another one") {
    const ArenaTable arena = loadArena();
    const ArenaStageTable stages = loadStages(arena);
    for (const ArenaStageSpec& spec : stages.stages) {
        const ArenaDef def = stageFight(arena, spec);
        ArenaStageLayout a, b, c;
        REQUIRE(generateArenaStage(spec, def, a));
        REQUIRE(generateArenaStage(spec, def, b));
        REQUIRE(a.pieces.size() == b.pieces.size());
        for (std::size_t i = 0; i < a.pieces.size(); ++i) {
            CHECK((a.pieces[i].at.x == b.pieces[i].at.x && a.pieces[i].at.y == b.pieces[i].at.y));
            CHECK(a.pieces[i].sz == b.pieces[i].sz);
            CHECK(a.pieces[i].yaw == b.pieces[i].yaw);
        }
        ArenaStageSpec other = spec;
        other.seed = spec.seed + 100;
        REQUIRE(generateArenaStage(other, def, c));
        bool differs = c.pieces.size() != a.pieces.size();
        for (std::size_t i = 0; !differs && i < a.pieces.size(); ++i) differs = a.pieces[i].at.x != c.pieces[i].at.x || a.pieces[i].at.y != c.pieces[i].at.y || a.pieces[i].yaw != c.pieces[i].yaw;
        if (spec.cover + spec.cracks + spec.pools + spec.landmarks > 0) CHECK_MESSAGE(differs, spec.id);
    }
}

TEST_CASE("arena stage: props, camera and fog grow with the boss, in held ranges") {
    const ArenaTable arena = loadArena();
    const ArenaStageTable stages = loadStages(arena);
    ArenaStageSpec spec = *stages.find("solhaven_canal"); // pillars, landmarks, a wall: a bit of everything
    spec.bosses = {"duelist"};
    float lastArm = 0.f, lastRing = 0.f, lastWall = 0.f, lastPillar = 0.f, lastLandmark = 0.f, lastFog = 1e9f;
    for (const char* id : {"duelist", "hulk", "titan"}) {
        spec.bosses = {id};
        const ArenaDef def = stageFight(arena, spec);
        ArenaStageLayout l;
        std::string err;
        REQUIRE_MESSAGE(generateArenaStage(spec, def, l, &err), err);
        const float wall = l.wallHeight, pillar = tallest(l, StageGroup::Pillar), landmark = tallest(l, StageGroup::Landmark);
        MESSAGE(id << ": ring " << l.ringRadius << " m, camera arm " << l.camera.arm << " m, lock pitch " << l.camera.lockPitch << ", wall "
                   << wall << " m, pillar " << pillar << " m, tallest landmark " << landmark << " m, fog " << l.look.fogDensity);
        CHECK(l.camera.arm >= lastArm);
        CHECK(l.ringRadius > lastRing);
        CHECK(wall >= lastWall);
        CHECK(pillar >= lastPillar);
        CHECK(landmark > lastLandmark);
        CHECK(l.look.fogDensity <= lastFog); // thinner with scale, so the boss stays in view
        CHECK(l.camera.arm >= 6.4f);
        CHECK(l.camera.arm <= 6.4f * 4.2f + 1e-3f);
        CHECK(l.camera.lockPitch >= -20.f);
        CHECK(l.camera.lockPitch <= -8.f);
        lastArm = l.camera.arm;
        lastRing = l.ringRadius;
        lastWall = wall;
        lastPillar = pillar;
        lastLandmark = landmark;
        lastFog = l.look.fogDensity;
    }
    // The camera clears the boss: its arm puts the camera above the tallest enemy's head at the lock pitch for a
    // player standing a ring-radius-share from it, for every shipped boss.
    for (const char* id : {"duelist", "brute", "hulk", "titan"}) {
        spec.bosses = {id};
        const ArenaDef def = stageFight(arena, spec);
        ArenaStageLayout l;
        REQUIRE(generateArenaStage(spec, def, l));
        const float pitch = -l.camera.lockPitch / 57.2958f;
        const float cameraHeight = l.camera.pivot + l.camera.up + l.camera.arm * std::sin(pitch);
        const float height = arenaStageSizing(def).height;
        CHECK_MESSAGE(cameraHeight * 2.f >= height, id << ": camera height " << cameraHeight << " m for a " << height << " m boss");
    }
}

TEST_CASE("arena stage: every floor, bound and theme combination at every size keeps the fight's rules") {
    const ArenaTable arena = loadArena();
    for (const char* boss : {"duelist", "hulk", "titan"}) {
        for (std::size_t f = 0; f < kStageFloorCount; ++f) {
            for (std::size_t b = 0; b < kStageBoundCount; ++b) {
                for (std::size_t t = 0; t < kStageThemeCount; ++t) {
                    for (StylePack world : {StylePack::None, StylePack::Solhaven, StylePack::Lattice, StylePack::Unwritten}) {
                        ArenaStageSpec spec;
                        spec.id = "combo";
                        spec.world = world;
                        spec.floor = static_cast<StageFloor>(f);
                        spec.bound = static_cast<StageBound>(b);
                        spec.theme = static_cast<StageTheme>(t);
                        spec.bosses = {boss};
                        spec.cover = 16;
                        spec.pillars = 24;
                        spec.landmarks = 8;
                        spec.pools = 6;
                        spec.cracks = 12;
                        spec.lights = 24;
                        spec.seed = static_cast<std::uint32_t>(f * 100 + b * 10 + t);
                        const ArenaDef def = stageFight(arena, spec);
                        ArenaStageLayout l;
                        std::string err;
                        // Water only where the stage brings some; the rest of the rules always.
                        const bool ok = generateArenaStage(spec, def, l, &err);
                        CHECK_MESSAGE((ok || contains(err, "no water")), boss << " " << stageFloorName(spec.floor) << " " << stageBoundName(spec.bound)
                                                                          << " " << stageThemeName(spec.theme) << ": " << err);
                        CHECK_MESSAGE(checkArenaStage(l, def, &err, false), err);
                    }
                }
            }
        }
    }
}

TEST_CASE("arena stage: three enemies, six at once, and the crowded rings keep the rules") {
    const ArenaTable arena = loadArena();
    ArenaStageSpec spec;
    spec.id = "crowd";
    spec.world = StylePack::Solhaven;
    spec.floor = StageFloor::Circle;
    spec.bound = StageBound::Water;
    spec.cover = 10;
    spec.pools = 4;
    spec.cracks = 8;
    for (int count = 1; count <= 6; ++count) {
        spec.bosses.assign(static_cast<std::size_t>(count), "hulk");
        spec.attackers = count;
        const ArenaDef def = stageFight(arena, spec);
        CHECK(def.enemyCount == count);
        ArenaStageLayout l;
        std::string err;
        CHECK_MESSAGE(generateArenaStage(spec, def, l, &err), "count " << count << ": " << err);
        // The enemies start apart from the player and each other, and inside the ring.
        ArenaFight fight;
        fight.begin(def, arenaStageRing(def.ringRadius), arenaStagePlayerStart(def.ringRadius), Vec2{1.f, 0.f}, 1u);
        const ArenaRing ring = arenaStageRing(def.ringRadius);
        for (std::size_t i = 0; i < fight.enemyCount(); ++i) {
            CHECK(ring.contains(fight.enemyStart(i), def.enemies[i].bodyRadius - 0.01f));
            const Vec2 d{fight.enemyStart(i).x - fight.playerStart().x, fight.enemyStart(i).y - fight.playerStart().y};
            CHECK(std::sqrt(d.x * d.x + d.y * d.y) >= def.enemies[i].bodyRadius + 3.f);
        }
    }
}

TEST_CASE("arena stage ring: centred on the origin with the player behind it") {
    for (float r : {7.f, 8.5f, 12.f, 25.f, 60.f}) {
        const ArenaRing ring = arenaStageRing(r);
        CHECK(ring.centre.x == doctest::Approx(0.f).epsilon(1e-4).scale(1.f));
        CHECK(ring.centre.y == doctest::Approx(0.f).epsilon(1e-4).scale(1.f));
        CHECK(ring.radius == doctest::Approx(r));
        CHECK(arenaStagePlayerStart(r).x == doctest::Approx(-0.35f * r));
        CHECK(ring.contains(arenaStagePlayerStart(r), 0.4f));
    }
}

// ---- the rules catch a bad layout ----

TEST_CASE("arena stage check: a solid piece in the ring, a decal in a spawn disc and a missing light are refused") {
    const ArenaTable arena = loadArena();
    const ArenaStageTable stages = loadStages(arena);
    const ArenaStageSpec& spec = *stages.find("solhaven_alley");
    const ArenaDef def = stageFight(arena, spec);
    ArenaStageLayout good;
    REQUIRE(generateArenaStage(spec, def, good));
    std::string err;
    REQUIRE(checkArenaStage(good, def, &err));

    {
        ArenaStageLayout l = good;
        StagePiece p;
        p.group = StageGroup::Cover;
        p.at = Vec2{1.f, 1.f};
        l.pieces.push_back(p);
        CHECK_FALSE(checkArenaStage(l, def, &err));
        CHECK(contains(err, "stands inside the ring"));
    }
    {
        ArenaStageLayout l = good;
        StagePiece p;
        p.group = StageGroup::Pool;
        p.shape = StageShape::Cylinder;
        p.role = StageRole::Water;
        p.at = arenaStagePlayerStart(def.ringRadius);
        p.sx = p.sy = 2.f;
        p.sz = 0.04f;
        l.pieces.push_back(p);
        CHECK_FALSE(checkArenaStage(l, def, &err));
        CHECK(contains(err, "spawn disc"));
    }
    {
        // A crack across the lane between the starts.
        ArenaStageLayout l = good;
        ArenaFight fight;
        fight.begin(def, arenaStageRing(def.ringRadius), arenaStagePlayerStart(def.ringRadius), Vec2{1.f, 0.f}, 1u);
        StagePiece p;
        p.group = StageGroup::Crack;
        p.at = Vec2{(fight.playerStart().x + fight.enemyStart(0).x) * 0.5f, (fight.playerStart().y + fight.enemyStart(0).y) * 0.5f};
        p.sx = 3.f;
        p.sy = 0.1f;
        p.sz = 0.03f;
        l.pieces.push_back(p);
        CHECK_FALSE(checkArenaStage(l, def, &err));
        CHECK(contains(err, "lane"));
    }
    {
        ArenaStageLayout l = good;
        l.ringRadius += 1.f;
        CHECK_FALSE(checkArenaStage(l, def, &err));
        CHECK(contains(err, "ring is not the fight's"));
    }
    {
        ArenaStageLayout l = good;
        l.camera.arm = 100.f;
        CHECK_FALSE(checkArenaStage(l, def, &err));
        CHECK(contains(err, "camera"));
    }
    {
        ArenaStageLayout l = good;
        l.pieces.erase(std::remove_if(l.pieces.begin(), l.pieces.end(), [](const StagePiece& p) { return p.role == StageRole::Water; }), l.pieces.end());
        l.bound = StageBound::Wall;
        l.floor = StageFloor::Circle;
        CHECK_FALSE(checkArenaStage(l, def, &err));
        CHECK(contains(err, "no water"));
        CHECK(checkArenaStage(l, def, &err, false));
    }
    {
        // A pinned ring too small for the spawn rule: the fit holds the spawn at 0.9 of the ring.
        ArenaStageSpec tiny = spec;
        tiny.ring = 6.f;
        tiny.spawn = 12.f;
        ArenaDef d = stageFight(arena, tiny);
        CHECK(d.spawnDistance <= 0.9f * d.ringRadius + 1e-4f);
    }
}

// ---- the fights the stages hold ----

TEST_CASE("arena stage fights: the autopilot wins every shipped stage, and a player pressing nothing loses") {
    const ArenaTable arena = loadArena();
    const ArenaStageTable stages = loadStages(arena);
    for (const ArenaStageSpec& spec : stages.stages) {
        const ArenaDef def = stageFight(arena, spec);
        float slowest = 0.f;
        for (std::uint32_t seed = 1; seed <= 6; ++seed) {
            const Play win = playOn(def, seed, true);
            const Play lose = playOn(def, seed, false);
            CHECK_MESSAGE(win.phase == ArenaPhase::Victory, spec.id << " seed " << seed << ": autopilot did not win in 120 s");
            CHECK_MESSAGE(lose.phase == ArenaPhase::Defeat, spec.id << " seed " << seed << ": a player pressing nothing survived");
            slowest = std::max(slowest, win.time);
        }
        MESSAGE(spec.id << ": ring " << def.ringRadius << " m, spawn " << def.spawnDistance << " m, slowest win " << slowest << " s");
    }
}
