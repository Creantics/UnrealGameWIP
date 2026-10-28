#include "ftcore/ArenaFight.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 60.f;

// ---- kits ----

// An attack for the rule tests: it reaches all round, so geometry never gets in the way of a timing test.
ArenaAttack swing(float startup, float active, float recovery, float range, int damage, float stamina = 0.f) {
    ArenaAttack a;
    a.startup = startup;
    a.active = active;
    a.recovery = recovery;
    a.range = range;
    a.arcCos = -1.f;
    a.damage = damage;
    a.poiseDamage = 0.f;
    a.guardDamage = 0.f;
    a.lunge = 0.f;
    a.tracking = 0.f;
    a.armor = 0.f;
    a.stamina = stamina;
    a.cancel = 0.5f;
    return a;
}

// A plain kit with round numbers: light 0.2 + 0.1 + 0.3 s (another action from 0.45 s), a dodge of 0.6 s with
// i-frames 0.1 .. 0.4 s, a parry window of 0.2 s, a heal of 0.5 + 0.3 s.
ArenaFighter ruleFighter() {
    ArenaFighter f;
    f.hp = 100;
    f.stamina = 100.f;
    f.staminaRegen = 50.f;
    f.staminaDelay = 0.5f;
    f.poise = 30.f;
    f.poiseDelay = 1.f;
    f.hitstun = 0.4f;
    f.moveSpeed = 4.f;
    f.turnRate = 20.f;
    f.bodyRadius = 0.4f;
    f.bufferTime = 0.2f;
    f.lightCount = 3;
    f.light[0] = swing(0.2f, 0.1f, 0.3f, 2.f, 10, 10.f);
    f.light[1] = swing(0.2f, 0.1f, 0.3f, 2.f, 11, 10.f);
    f.light[2] = swing(0.2f, 0.1f, 0.3f, 2.f, 12, 10.f);
    f.chainGap = 0.3f;
    f.heavy = swing(0.4f, 0.2f, 0.5f, 2.f, 30, 25.f);
    f.riposte = swing(0.1f, 0.1f, 0.4f, 2.5f, 50, 0.f);
    f.dodgeTime = 0.6f;
    f.dodgeDistance = 3.f;
    f.dodgeInvulnStart = 0.1f;
    f.dodgeInvulnEnd = 0.4f;
    f.dodgeStamina = 20.f;
    f.dodgeCancel = 0.75f;
    f.guardArcCos = 0.f;
    f.guardAbsorb = 0.75f;
    f.guardMoveScale = 0.5f;
    f.guardBreakTime = 1.f;
    f.parryWindow = 0.2f;
    f.parryRearm = 0.5f;
    f.riposteReach = 2.2f;
    f.healCharges = 2;
    f.healAmount = 40;
    f.healTime = 0.5f;
    f.healRecovery = 0.3f;
    f.healMoveScale = 0.35f;
    f.lockRange = 15.f;
    f.lockBreak = 20.f;
    return f;
}

// An enemy that never asks for a token: a body for the player's rules to act on.
ArenaEnemyKind dummyKind() {
    ArenaEnemyKind k;
    k.hp = 100;
    k.poise = 30.f;
    k.poiseDelay = 1.f;
    k.staggerTime = 1.f;
    k.openTime = 1.5f;
    k.downTime = 1.f;
    k.attackDelay = 1000.f;
    k.bodyRadius = 0.45f;
    return k;
}

// An enemy that asks at once and has one move, picked within `maxRange`.
ArenaEnemyKind hitterKind(const ArenaAttack& attack, float maxRange = 3.f) {
    ArenaEnemyKind k = dummyKind();
    k.attackDelay = 0.f;
    k.moveCount = 1;
    ArenaEnemyMove& m = k.moves[0];
    m.attack = attack;
    m.minRange = 0.f;
    m.maxRange = maxRange;
    m.cooldown = 0.f;
    m.weight = 1.f;
    m.chain = -1;
    m.unblockable = false;
    m.parryable = true;
    return k;
}

// ---- a rig with the bodies pinned where the test puts them ----

struct Logged {
    int tick = 0; // 1-based: the tick the event came out of
    ArenaEvent event;
};

struct Rig {
    ArenaDef def;
    ArenaFight fight;
    ArenaBodies bodies;
    ArenaInput in;
    std::vector<Logged> log;
    int ticks = 0;

    Rig(const ArenaFighter& fighter, std::initializer_list<ArenaEnemyKind> kinds, std::uint8_t attackers = 1,
        ArenaRing ring = ArenaRing{Vec2{0.f, 0.f}, 30.f}, std::uint32_t seed = 1) {
        def.fighter = fighter;
        std::size_t n = 0;
        for (const ArenaEnemyKind& k : kinds) def.enemies[n++] = k;
        def.enemyCount = static_cast<std::uint8_t>(n);
        def.attackers = attackers;
        fight.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, seed);
        for (std::size_t i = 0; i < n; ++i) bodies.enemies[i] = Vec2{1.5f, 0.f};
    }

    void put(std::size_t i, float x, float y) { bodies.enemies[i] = Vec2{x, y}; }

    // One tick; the edge inputs clear after it, the stick, the view and the guard flag stay.
    const ArenaOutput& tick(float dt = kDt) {
        const ArenaOutput& out = fight.tick(dt, in, bodies);
        ++ticks;
        for (const ArenaEvent& e : out.events) {
            Logged l;
            l.tick = ticks;
            l.event = e;
            log.push_back(l);
        }
        in.light = in.heavy = in.dodge = in.heal = in.lock = false;
        in.lockSwitch = 0;
        return out;
    }
    void run(int n) {
        for (int i = 0; i < n; ++i) tick();
    }
    void runFor(float seconds) { run(static_cast<int>(std::lround(seconds / kDt))); }

    const ArenaOutput& out() const { return fight.output(); }

    int count(ArenaEventType type) const {
        int n = 0;
        for (const Logged& l : log)
            if (l.event.type == type) ++n;
        return n;
    }
    // The nth event of a type, or nullptr.
    const Logged* find(ArenaEventType type, int nth = 0) const {
        for (const Logged& l : log)
            if (l.event.type == type && nth-- == 0) return &l;
        return nullptr;
    }
};

float distanceOf(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

// ---- loader fixtures ----

bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

std::string tableJson(const std::string& fighters, const std::string& enemies, const std::string& fights = "[]",
                      const std::string& def = R"({ "enemies": ["dummy"] })") {
    return std::string(R"({ "version": 1, "fighters": )") + fighters + R"(, "enemies": )" + enemies +
           R"(, "default": )" + def + R"(, "fights": )" + fights + " }";
}

const char* const kFightersOk = R"([ { "id": "default" } ])";
const char* const kEnemiesOk = R"([ { "id": "dummy" } ])";

// The error text of loading `json` (empty on success); out comes back empty on failure.
std::string loadError(const std::string& json, ArenaTable* table = nullptr, std::vector<std::string>* warnings = nullptr) {
    ArenaTable local;
    ArenaTable& t = table ? *table : local;
    std::string err;
    if (importArenaTable(json, t, &err, warnings)) return std::string();
    CHECK_FALSE(err.empty());
    CHECK(t.fighters.empty());
    CHECK(t.enemies.empty());
    CHECK(t.entries.empty());
    return err;
}

ArenaTable loadShipped() {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/arena.json", text)) FAIL("Data/Battle/arena.json not found");
    ArenaTable t;
    std::string err;
    REQUIRE_MESSAGE(importArenaTable(text, t, &err), err);
    return t;
}

} // namespace

// ---- loader ----

TEST_CASE("arena table: Data/Battle/arena.json loads without warnings") {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/arena.json", text)) FAIL("Data/Battle/arena.json not found");
    ArenaTable t;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importArenaTable(text, t, &err, &warnings), err);
    CHECK(warnings.empty());
    REQUIRE(!t.fighterIds.empty());
    CHECK(t.fighterIds[0] == "default");
    CHECK(t.fighters.size() == t.fighterIds.size());
    CHECK(t.enemies.size() == t.enemyIds.size());
    CHECK(t.enemies.size() >= 2);
    CHECK(t.enemies.size() <= 3);
    CHECK(t.fallback.enemyCount == 1);
    CHECK(t.entries.empty());
    // The default fight's slot says which table kind it holds.
    REQUIRE(t.fallback.enemyKind[0] < t.enemyIds.size());
    CHECK(t.enemyIds[t.fallback.enemyKind[0]] == "brute");
    CHECK(t.fallback.enemyKind[1] == kArenaNoKind);
    // Real acceleration values ship: the fighter's and every kind's.
    CHECK(t.fighters[0].moveAccel > 0.f);
    CHECK(t.fighters[0].moveBrake > t.fighters[0].moveAccel); // it stops faster than it starts
    bool unblockable = false;
    for (const ArenaEnemyKind& k : t.enemies) {
        CHECK(k.accel > 0.f);
        for (std::size_t m = 0; m < k.moveCount; ++m) {
            CHECK(k.moves[m].attack.startup >= 0.35f); // every attack telegraphs
            if (k.moves[m].unblockable) unblockable = true;
        }
    }
    CHECK(unblockable);
}

TEST_CASE("arena data: every enemy attack telegraphs for at least 0.35 s, and a heavier one telegraphs longer") {
    const ArenaTable table = loadShipped();
    std::vector<ArenaEnemyKind> kinds = table.enemies;
    kinds.push_back(defaultArenaDef().enemies[0]); // the built-in fight's enemy follows the same rule
    for (const ArenaEnemyKind& k : kinds) {
        for (std::size_t a = 0; a < k.moveCount; ++a) {
            CHECK(k.moves[a].attack.startup >= 0.35f);
            for (std::size_t b = 0; b < k.moveCount; ++b)
                if (k.moves[a].attack.damage > k.moves[b].attack.damage)
                    CHECK(k.moves[a].attack.startup > k.moves[b].attack.startup);
        }
    }
}

TEST_CASE("arena table: an unknown enemy id, a duplicate id and a duplicate entry are errors") {
    {
        const std::string err = loadError(tableJson(
            kFightersOk, kEnemiesOk, R"([ { "scene": "S", "beat": 0, "enemies": ["dummy", "ghost"] } ])"));
        CHECK(contains(err, "fights[0].enemies[1]: unknown enemy id: ghost"));
    }
    {
        const std::string err = loadError(tableJson(kFightersOk, R"([ { "id": "dummy" }, { "id": "dummy" } ])"));
        CHECK(contains(err, "enemies[1].id: duplicate id: dummy"));
    }
    {
        const std::string err = loadError(tableJson(R"([ { "id": "default" }, { "id": "default" } ])", kEnemiesOk));
        CHECK(contains(err, "fighters[1].id: duplicate id: default"));
    }
    {
        // The same (scene, beat, step): a missing step is step 0.
        const std::string err = loadError(tableJson(kFightersOk, kEnemiesOk,
                                                    R"([ { "scene": "S", "beat": 3, "enemies": ["dummy"] },
                                                         { "scene": "S", "beat": 3, "step": 0, "enemies": ["dummy"] } ])"));
        CHECK(contains(err, "fights[1]: duplicate entry for S beat 3 step 0"));
    }
    {
        // A different step, beat or scene is fine.
        ArenaTable t;
        const std::string err = loadError(tableJson(kFightersOk, kEnemiesOk,
                                                    R"([ { "scene": "S", "beat": 3, "enemies": ["dummy"] },
                                                         { "scene": "S", "beat": 3, "step": 1, "enemies": ["dummy"] },
                                                         { "scene": "S", "beat": 4, "enemies": ["dummy"] },
                                                         { "scene": "T", "beat": 3, "enemies": ["dummy"] } ])"),
                                          &t);
        CHECK(err.empty());
        CHECK(t.entries.size() == 4);
    }
}

TEST_CASE("arena table: a chain to a missing move and a looping string are errors") {
    {
        const std::string err = loadError(tableJson(kFightersOk, R"([ { "id": "dummy", "moves": [ { "chain": 5 } ] } ])"));
        CHECK(contains(err, "enemies[0].moves[0].chain"));
    }
    {
        const std::string err = loadError(tableJson(kFightersOk, R"([ { "id": "dummy", "moves": [ { "chain": 0 } ] } ])"));
        CHECK(contains(err, "enemies[0].moves[0].chain: a move cannot chain to itself"));
    }
    {
        const std::string err = loadError(tableJson(
            kFightersOk, R"([ { "id": "dummy", "moves": [ { "chain": 1 }, { "chain": 2 }, { "chain": 0 } ] } ])"));
        CHECK(contains(err, "the string loops"));
    }
    {
        // A string that ends is fine, and a move reached only by a chain may have weight 0.
        ArenaTable t;
        const std::string err = loadError(
            tableJson(kFightersOk, R"([ { "id": "dummy", "moves": [ { "chain": 1 }, { "chain": 2, "weight": 0 }, { "weight": 0 } ] } ])"),
            &t);
        CHECK(err.empty());
        REQUIRE(t.enemies.size() == 1);
        CHECK(t.enemies[0].moveCount == 3);
        CHECK(t.enemies[0].moves[0].chain == 1);
        CHECK(t.enemies[0].moves[2].chain == -1);
    }
    {
        // Something has to be pickable.
        const std::string err = loadError(tableJson(kFightersOk, R"([ { "id": "dummy", "moves": [ { "weight": 0 } ] } ])"));
        CHECK(contains(err, "enemies[0].moves"));
    }
}

TEST_CASE("arena table: a missing default fighter is an error") {
    CHECK(contains(loadError(tableJson(R"([ { "id": "lead" } ])", kEnemiesOk)), "default"));
    CHECK(contains(loadError(R"({ "version": 1, "enemies": [ { "id": "dummy" } ], "default": { "enemies": ["dummy"] } })"),
                   "default"));
}

TEST_CASE("arena table: a number out of its range is an error with its path") {
    CHECK(contains(loadError(tableJson(R"([ { "id": "default", "hp": 0 } ])", kEnemiesOk)), "fighters[0].hp"));
    CHECK(contains(loadError(tableJson(R"([ { "id": "default", "hp": 100001 } ])", kEnemiesOk)), "fighters[0].hp"));
    CHECK(contains(loadError(tableJson(R"([ { "id": "default", "light": [ { "startup": 11 } ] } ])", kEnemiesOk)),
                   "fighters[0].light[0].startup"));
    CHECK(contains(loadError(tableJson(R"([ { "id": "default", "heavy": { "arcCos": 2 } } ])", kEnemiesOk)),
                   "fighters[0].heavy.arcCos"));
    CHECK(contains(loadError(tableJson(R"([ { "id": "default", "bodyRadius": 0 } ])", kEnemiesOk)), "fighters[0].bodyRadius"));
    CHECK(contains(loadError(tableJson(R"([ { "id": "default", "hp": "many" } ])", kEnemiesOk)), "fighters[0].hp"));
    CHECK(contains(loadError(tableJson(R"([ { "id": "default", "light": [] } ])", kEnemiesOk)), "fighters[0].light"));
    CHECK(contains(loadError(tableJson(R"([ { "id": "default", "dodgeInvulnEnd": 0.9 } ])", kEnemiesOk)),
                   "fighters[0].dodgeInvulnEnd"));
    CHECK(contains(loadError(tableJson(kFightersOk, R"([ { "id": "dummy", "poise": -1 } ])")), "enemies[0].poise"));
    CHECK(contains(loadError(tableJson(kFightersOk, R"([ { "id": "dummy", "moves": [ { "maxRange": 0 } ] } ])")),
                   "enemies[0].moves[0].maxRange"));
    CHECK(contains(loadError(tableJson(kFightersOk, R"([ { "id": "dummy", "moves": [ { "minRange": 5, "maxRange": 3 } ] } ])")),
                   "enemies[0].moves[0].minRange"));
    CHECK(contains(loadError(tableJson(kFightersOk, R"([ { "id": "dummy", "moves": [ { "unblockable": 1 } ] } ])")),
                   "enemies[0].moves[0].unblockable"));
    CHECK(contains(loadError(tableJson(kFightersOk, kEnemiesOk, "[]", R"({ "enemies": ["dummy"], "attackers": 7 })")),
                   "default.attackers"));
    CHECK(contains(loadError(tableJson(kFightersOk, kEnemiesOk, "[]", R"({ "enemies": ["dummy"], "ring": 0 })")), "default.ring"));
    CHECK(contains(loadError(tableJson(kFightersOk, kEnemiesOk, "[]", R"({ "enemies": [] })")), "default.enemies"));
    CHECK(contains(loadError(tableJson(kFightersOk, kEnemiesOk, R"([ { "scene": "S", "beat": -1, "enemies": ["dummy"] } ])")),
                   "fights[0].beat"));
}

TEST_CASE("arena table: errors come one per line, each with its path, and out stays empty") {
    ArenaTable t;
    const std::string err = loadError(
        tableJson(R"([ { "id": "default", "hp": 0, "stamina": -4 } ])", kEnemiesOk,
                  R"([ { "scene": "S", "beat": 0, "enemies": ["ghost"] } ])"),
        &t);
    CHECK(contains(err, "fighters[0].hp"));
    CHECK(contains(err, "fighters[0].stamina"));
    CHECK(contains(err, "fights[0].enemies[0]"));
    CHECK(std::count(err.begin(), err.end(), '\n') == 2);
    std::string text;
    ArenaTable bad;
    std::string e2;
    CHECK_FALSE(importArenaTable("{ not json", bad, &e2));
    CHECK(contains(e2, "arena file"));
    CHECK_FALSE(importArenaTable("[]", bad, &e2));
    CHECK(contains(e2, "expected an object"));
}

TEST_CASE("arena table: an unknown key is a warning, a missing key keeps the struct default") {
    ArenaTable t;
    std::vector<std::string> warnings;
    const std::string err = loadError(
        R"({ "version": 1, "extra": 1,
             "fighters": [ { "id": "default", "hp": 90, "bogus": true, "light": [ { "range": 3, "wobble": 1 } ] } ],
             "enemies": [ { "id": "dummy", "hp": 55, "moves": [ { "startup": 0.5, "mystery": 2 } ] } ],
             "default": { "enemies": ["dummy"], "colour": "red" },
             "fights": [] })",
        &t, &warnings);
    CHECK(err.empty());
    CHECK(warnings.size() == 5);
    bool sawFighter = false, sawLight = false, sawMove = false, sawDefault = false, sawRoot = false;
    for (const std::string& w : warnings) {
        sawFighter = sawFighter || contains(w, "fighters[0]: unknown key: bogus");
        sawLight = sawLight || contains(w, "fighters[0].light[0]: unknown key: wobble");
        sawMove = sawMove || contains(w, "enemies[0].moves[0]: unknown key: mystery");
        sawDefault = sawDefault || contains(w, "default: unknown key: colour");
        sawRoot = sawRoot || contains(w, "unknown key: extra");
    }
    CHECK(sawFighter);
    CHECK(sawLight);
    CHECK(sawMove);
    CHECK(sawDefault);
    CHECK(sawRoot);
    // What is missing keeps the struct's default.
    REQUIRE(t.fighters.size() == 1);
    const ArenaFighter standard;
    CHECK(t.fighters[0].hp == 90);
    CHECK(t.fighters[0].stamina == standard.stamina);
    CHECK(t.fighters[0].parryWindow == standard.parryWindow);
    CHECK(t.fighters[0].lightCount == 1); // the light array's length
    CHECK(t.fighters[0].light[0].range == 3.f);
    CHECK(t.fighters[0].light[0].damage == standard.light[0].damage);
    CHECK(t.fighters[0].heavy.startup == standard.heavy.startup);
    REQUIRE(t.enemies.size() == 1);
    const ArenaEnemyKind kind;
    CHECK(t.enemies[0].hp == 55);
    CHECK(t.enemies[0].poise == kind.poise);
    CHECK(t.enemies[0].preferredRange == kind.preferredRange);
    CHECK(t.enemies[0].moveCount == 1);
    CHECK(t.enemies[0].moves[0].attack.startup == 0.5f);
    CHECK(t.enemies[0].moves[0].attack.damage == kind.moves[0].attack.damage);
    CHECK(t.enemies[0].moves[0].parryable);
    CHECK(t.fallback.ringRadius == ArenaDef{}.ringRadius);
    CHECK(t.fallback.attackers == 1);
}

TEST_CASE("arena table: the default fighter comes first, fighter() and find() fall back") {
    ArenaTable t;
    const std::string err = loadError(
        R"({ "version": 1,
             "fighters": [ { "id": "lead", "hp": 80 }, { "id": "default", "hp": 100 } ],
             "enemies": [ { "id": "dummy" } ],
             "default": { "enemies": ["dummy"] },
             "fights": [ { "scene": "SC-T", "beat": 4, "step": 2, "enemies": ["dummy", "dummy"], "attackers": 2, "ring": 9, "spawn": 5 } ] })",
        &t);
    CHECK(err.empty());
    REQUIRE(t.fighterIds.size() == 2);
    CHECK(t.fighterIds[0] == "default");
    CHECK(t.fighterIds[1] == "lead");
    CHECK(t.fighter("lead").hp == 80);
    CHECK(t.fighter("default").hp == 100);
    CHECK(t.fighter("nobody").hp == 100);
    CHECK(t.fighter("").hp == 100);
    // The entry is keyed by scene, beat and step; its fighter is the default one.
    const ArenaFightEntry* e = t.findEntry("SC-T", 4, 2);
    REQUIRE(e);
    CHECK(e->def.enemyCount == 2);
    CHECK(e->def.attackers == 2);
    CHECK(e->def.ringRadius == 9.f);
    CHECK(e->def.spawnDistance == 5.f);
    CHECK(e->def.fighter.hp == 100);
    CHECK(t.findEntry("SC-T", 4, 1) == nullptr);
    CHECK(t.findEntry("SC-X", 4, 2) == nullptr);
    CHECK(&t.find("SC-T", 4, 2) == &e->def);
    CHECK(&t.find("SC-X", 0, 0) == &t.fallback);
    CHECK(t.fallback.enemyCount == 1);
    // An empty table still answers.
    const ArenaTable empty;
    CHECK(empty.fighter("lead").hp == ArenaFighter{}.hp);
    CHECK(&empty.find("S", 0, 0) == &empty.fallback);
}

TEST_CASE("arena table: each fight records the table kind of its enemy slots") {
    ArenaTable t;
    const std::string err = loadError(
        R"({ "version": 1,
             "fighters": [ { "id": "default" } ],
             "enemies": [ { "id": "alpha", "hp": 11 }, { "id": "beta", "hp": 22 }, { "id": "gamma", "hp": 33 } ],
             "default": { "enemies": ["beta"] },
             "fights": [ { "scene": "S", "beat": 1, "enemies": ["gamma", "alpha", "gamma"] } ] })",
        &t);
    CHECK(err.empty());
    CHECK(t.fallback.enemyKind[0] == 1);
    for (std::size_t i = 1; i < kMaxArenaEnemies; ++i) CHECK(t.fallback.enemyKind[i] == kArenaNoKind);
    REQUIRE(t.entries.size() == 1);
    const ArenaDef& def = t.entries[0].def;
    CHECK(def.enemyKind[0] == 2);
    CHECK(def.enemyKind[1] == 0);
    CHECK(def.enemyKind[2] == 2);
    for (std::size_t i = 3; i < kMaxArenaEnemies; ++i) CHECK(def.enemyKind[i] == kArenaNoKind);
    // A slot indexes the table's enemies: it names the kind the slot holds.
    for (std::size_t i = 0; i < def.enemyCount; ++i) {
        REQUIRE(def.enemyKind[i] < t.enemies.size());
        CHECK(t.enemies[def.enemyKind[i]].hp == def.enemies[i].hp);
    }
    CHECK(t.enemyIds[def.enemyKind[0]] == "gamma");
    CHECK(t.enemyIds[def.enemyKind[1]] == "alpha");
    // A started fight keeps them. A def built in code means "no table kind", defaultArenaDef() included.
    ArenaFight fight;
    fight.begin(def, ArenaRing{Vec2{4.f, 0.f}, 12.f}, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
    CHECK(fight.def().enemyKind[0] == 2);
    CHECK(fight.def().enemyKind[1] == 0);
    const ArenaDef plain;
    for (const std::uint8_t kind : plain.enemyKind) CHECK(kind == kArenaNoKind);
    const ArenaDef builtin = defaultArenaDef();
    for (const std::uint8_t kind : builtin.enemyKind) CHECK(kind == kArenaNoKind);
    // The index is a byte with 255 taken: 254 kinds fit, one more is an error.
    const auto manyKinds = [](int count) {
        std::string enemies = "[ ";
        for (int i = 0; i < count; ++i) enemies += std::string(i ? ", " : "") + "{ \"id\": \"k" + std::to_string(i) + "\" }";
        return tableJson(kFightersOk, enemies + " ]", R"([ { "scene": "S", "beat": 0, "enemies": ["k253"] } ])",
                         R"({ "enemies": ["k0"] })");
    };
    ArenaTable big;
    CHECK(loadError(manyKinds(254), &big).empty());
    REQUIRE(big.entries.size() == 1);
    CHECK(big.entries[0].def.enemyKind[0] == 253);
    CHECK(contains(loadError(manyKinds(255)), "at most 254 kinds"));
}

TEST_CASE("arena table: moveAccel, moveBrake, accel and maxTargets are read and checked, and default to nothing") {
    ArenaTable t;
    const std::string err = loadError(
        tableJson(R"([ { "id": "default", "moveAccel": 25, "moveBrake": 40, "light": [ { "maxTargets": 2 } ],
                         "heavy": { "maxTargets": 3 } } ])",
                  R"([ { "id": "dummy", "accel": 8, "moves": [ { "maxTargets": 1 } ] } ])"),
        &t);
    CHECK(err.empty());
    REQUIRE(t.fighters.size() == 1);
    CHECK(t.fighters[0].moveAccel == 25.f);
    CHECK(t.fighters[0].moveBrake == 40.f);
    CHECK(t.fighters[0].light[0].maxTargets == 2);
    CHECK(t.fighters[0].heavy.maxTargets == 3);
    CHECK(t.fighters[0].riposte.maxTargets == 0);
    REQUIRE(t.enemies.size() == 1);
    CHECK(t.enemies[0].accel == 8.f);
    CHECK(t.enemies[0].moves[0].attack.maxTargets == 1);
    // Left out, they keep their defaults: instant, and every target in reach.
    ArenaTable plain;
    CHECK(loadError(tableJson(kFightersOk, kEnemiesOk), &plain).empty());
    CHECK(plain.fighters[0].moveAccel == 0.f);
    CHECK(plain.fighters[0].moveBrake == 0.f);
    CHECK(plain.fighters[0].light[0].maxTargets == 0);
    CHECK(plain.enemies[0].accel == 0.f);
    CHECK(contains(loadError(tableJson(R"([ { "id": "default", "moveAccel": -1 } ])", kEnemiesOk)), "fighters[0].moveAccel"));
    CHECK(contains(loadError(tableJson(R"([ { "id": "default", "moveBrake": 1001 } ])", kEnemiesOk)), "fighters[0].moveBrake"));
    CHECK(contains(loadError(tableJson(kFightersOk, R"([ { "id": "dummy", "accel": -2 } ])")), "enemies[0].accel"));
    CHECK(contains(loadError(tableJson(R"([ { "id": "default", "light": [ { "maxTargets": 7 } ] } ])", kEnemiesOk)),
                   "fighters[0].light[0].maxTargets"));
    CHECK(contains(loadError(tableJson(R"([ { "id": "default", "heavy": { "maxTargets": 1.5 } } ])", kEnemiesOk)),
                   "fighters[0].heavy.maxTargets"));
}

// ---- the ring ----

TEST_CASE("arena ring: centred ahead of the player, bounds of radius <= 0 ignored") {
    const ArenaRing none{Vec2{0.f, 0.f}, 0.f};
    const ArenaRing a = arenaRingAt(Vec2{10.f, 5.f}, Vec2{0.f, 1.f}, 12.f, none);
    CHECK(a.radius == 12.f);
    CHECK(a.centre.x == doctest::Approx(10.f));
    CHECK(a.centre.y == doctest::Approx(5.f + 12.f * 0.35f));
    // The facing is a direction: its length doesn't matter.
    const ArenaRing b = arenaRingAt(Vec2{0.f, 0.f}, Vec2{3.f, 4.f}, 10.f, ArenaRing{Vec2{1.f, 1.f}, -5.f});
    CHECK(b.radius == 10.f);
    CHECK(b.centre.x == doctest::Approx(2.1f));
    CHECK(b.centre.y == doctest::Approx(2.8f));
}

TEST_CASE("arena ring: moved and shrunk to fit inside the bounds") {
    const ArenaRing plaza{Vec2{0.f, 0.f}, 36.f};
    {   // room to spare: untouched
        const ArenaRing r = arenaRingAt(Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 12.f, plaza);
        CHECK(r.radius == 12.f);
        CHECK(r.centre.x == doctest::Approx(4.2f));
        CHECK(r.centre.y == doctest::Approx(0.f));
    }
    {   // near the edge: moved back toward the middle, same size
        const ArenaRing r = arenaRingAt(Vec2{30.f, 0.f}, Vec2{1.f, 0.f}, 12.f, plaza);
        CHECK(r.radius == 12.f);
        CHECK(r.centre.x == doctest::Approx(24.f)); // 36 - 12
        CHECK(r.centre.y == doctest::Approx(0.f));
    }
    {   // bigger than the bounds: shrunk to them
        const ArenaRing r = arenaRingAt(Vec2{5.f, 5.f}, Vec2{1.f, 0.f}, 50.f, plaza);
        CHECK(r.radius == doctest::Approx(36.f));
        CHECK(r.centre.x == doctest::Approx(0.f));
        CHECK(r.centre.y == doctest::Approx(0.f));
    }
    {   // bounds that aren't at the origin
        const ArenaRing off{Vec2{100.f, -40.f}, 20.f};
        const ArenaRing r = arenaRingAt(Vec2{118.f, -40.f}, Vec2{1.f, 0.f}, 12.f, off);
        CHECK(distanceOf(r.centre, off.centre) + r.radius <= off.radius + 1e-3f);
    }
    // Wherever the player stands in the plaza, the ring is inside it.
    for (int x = -30; x <= 30; x += 10)
        for (int y = -30; y <= 30; y += 10)
            for (int angle = 0; angle < 360; angle += 45) {
                const float a = static_cast<float>(angle) * 3.14159265f / 180.f;
                const Vec2 at{static_cast<float>(x), static_cast<float>(y)};
                if (distanceOf(at, plaza.centre) > plaza.radius) continue;
                const ArenaRing r = arenaRingAt(at, Vec2{std::cos(a), std::sin(a)}, 12.f, plaza);
                CHECK(distanceOf(r.centre, plaza.centre) + r.radius <= plaza.radius + 1e-3f);
            }
}

TEST_CASE("arena park spots: outside the ring, on the player's side, spread apart") {
    const ArenaRing ring{Vec2{4.2f, 0.f}, 12.f};
    const Vec2 player{0.f, 0.f};
    Vec2 spots[4];
    for (std::size_t i = 0; i < 4; ++i) {
        spots[i] = arenaParkSpot(ring, player, i);
        CHECK(distanceOf(spots[i], ring.centre) > ring.radius);
        // On the player's side of the ring.
        const float side = (spots[i].x - ring.centre.x) * (player.x - ring.centre.x) +
                           (spots[i].y - ring.centre.y) * (player.y - ring.centre.y);
        CHECK(side > 0.f);
    }
    for (std::size_t i = 0; i < 4; ++i)
        for (std::size_t j = i + 1; j < 4; ++j) CHECK(distanceOf(spots[i], spots[j]) >= 1.9f);
    // Just outside the edge, not far off it.
    CHECK(distanceOf(spots[0], ring.centre) < ring.radius + 3.f);
    // The same ring and player give the same spots, and a ring elsewhere moves them with it.
    const Vec2 again = arenaParkSpot(ring, player, 2);
    CHECK(again.x == spots[2].x);
    CHECK(again.y == spots[2].y);
    const ArenaRing moved{Vec2{104.2f, 50.f}, 12.f};
    const Vec2 shifted = arenaParkSpot(moved, Vec2{100.f, 50.f}, 1);
    CHECK(shifted.x - 100.f == doctest::Approx(spots[1].x).epsilon(0.001));
    CHECK(shifted.y - 50.f == doctest::Approx(spots[1].y).epsilon(0.001));
}

// ---- rules ----

TEST_CASE("arena attack: it connects only in its active part, and only once per target") {
    Rig rig(ruleFighter(), {dummyKind()});
    rig.put(0, 1.5f, 0.f);
    rig.in.light = true;
    std::vector<ArenaPart> parts;
    int firstHit = 0;
    {
        const ArenaOutput& out = rig.tick();
        CHECK(out.player.action == ArenaAction::Light);
        CHECK(out.player.move == 0);
        CHECK(rig.count(ArenaEventType::AttackStarted) == 1);
        parts.push_back(out.player.part);
    }
    for (int i = 0; i < 70; ++i) {
        const ArenaOutput& out = rig.tick();
        parts.push_back(out.player.part);
        if (!firstHit && rig.count(ArenaEventType::Hit) > 0) firstHit = rig.ticks;
    }
    // The startup is 0.2 s: nothing before it ends, a hit as the tick reaches past it.
    CHECK(firstHit >= 12);
    CHECK(firstHit <= 13);
    CHECK(rig.count(ArenaEventType::Hit) == 1); // though the active part spans six ticks
    const Logged* hit = rig.find(ArenaEventType::Hit);
    REQUIRE(hit);
    CHECK(hit->event.source == kArenaPlayer);
    CHECK(hit->event.target == 0);
    CHECK(hit->event.amount == 10);
    CHECK(rig.fight.enemyHp(0) == 90);
    CHECK(rig.fight.stats().hits == 1);
    CHECK(rig.fight.stats().damageDealt == 10);
    // The parts run Startup, Active, Recovery, then none: in that order.
    CHECK(parts.front() == ArenaPart::Startup);
    int stage = 0; // 0 startup, 1 active, 2 recovery, 3 none
    for (ArenaPart p : parts) {
        const int s = p == ArenaPart::Startup ? 0 : p == ArenaPart::Active ? 1 : p == ArenaPart::Recovery ? 2 : 3;
        CHECK(s >= stage);
        stage = std::max(stage, s);
    }
    CHECK(stage == 3);
    CHECK(rig.out().player.action == ArenaAction::Idle);
}

TEST_CASE("arena attack: nothing connects when the target is out of reach during the active part") {
    Rig rig(ruleFighter(), {dummyKind()});
    rig.put(0, 5.f, 0.f); // beyond range 2 + radius 0.45
    rig.in.light = true;
    rig.tick();
    rig.run(20); // through the startup and the active part
    rig.put(0, 1.5f, 0.f); // in reach, but only in the recovery
    rig.run(30);
    CHECK(rig.count(ArenaEventType::Hit) == 0);
    // The arc counts too: a narrow attack misses a target at its side.
    ArenaFighter narrow = ruleFighter();
    narrow.light[0].arcCos = 0.9f;
    Rig side(narrow, {dummyKind()});
    side.put(0, 0.f, 1.5f); // a quarter turn from the facing
    side.in.light = true;
    side.run(40);
    CHECK(side.count(ArenaEventType::Hit) == 0);
}

TEST_CASE("arena attack: a long frame that spans a short active part still hits") {
    ArenaFighter f = ruleFighter();
    f.light[0] = swing(0.2f, 0.02f, 0.3f, 2.f, 10, 10.f);
    Rig rig(f, {dummyKind()});
    rig.put(0, 1.5f, 0.f);
    rig.in.light = true;
    rig.tick();
    rig.tick(0.1f); // 0.1167 s: still startup
    CHECK(rig.count(ArenaEventType::Hit) == 0);
    rig.tick(0.5f); // spans 0.2 .. 0.22 s whole
    CHECK(rig.count(ArenaEventType::Hit) == 1);
    CHECK(rig.out().player.action == ArenaAction::Idle); // and the attack is over
    // An enemy's attack is no different.
    Rig foe(ruleFighter(), {hitterKind(swing(0.4f, 0.02f, 0.4f, 2.f, 20))});
    foe.put(0, 1.5f, 0.f);
    foe.tick();
    foe.tick(0.2f);
    CHECK(foe.count(ArenaEventType::Hit) == 0);
    foe.tick(0.5f);
    CHECK(foe.count(ArenaEventType::Hit) == 1);
    CHECK(foe.fight.hp() == 80);
}

TEST_CASE("arena attack: maxTargets caps how many distinct targets one attack connects with, the nearest first") {
    // Three in a line within one swing's reach (2 m + 0.45): the indexes are not in the order of the distances.
    const auto hitsOn = [](int maxTargets) {
        ArenaFighter f = ruleFighter();
        f.light[0].maxTargets = static_cast<std::uint8_t>(maxTargets);
        Rig rig(f, {dummyKind(), dummyKind(), dummyKind()});
        rig.put(0, 1.8f, 0.f);
        rig.put(1, 2.4f, 0.f);
        rig.put(2, 1.0f, 0.f);
        rig.in.light = true;
        rig.run(40);
        std::vector<int> hit;
        for (const Logged& l : rig.log)
            if (l.event.type == ArenaEventType::Hit) hit.push_back(l.event.target);
        std::sort(hit.begin(), hit.end());
        return hit;
    };
    const std::vector<int> everyone{0, 1, 2};
    const std::vector<int> nearest{2};
    const std::vector<int> nearestTwo{0, 2};
    CHECK(hitsOn(0) == everyone); // 0: every target in reach, as before
    CHECK(hitsOn(1) == nearest);
    CHECK(hitsOn(2) == nearestTwo);
    CHECK(hitsOn(3) == everyone);
    {   // the slots last the whole active part: only the first two distinct targets ever connect
        ArenaFighter f = ruleFighter();
        f.light[0].maxTargets = 2;
        Rig rig(f, {dummyKind(), dummyKind(), dummyKind()});
        rig.put(0, 1.0f, 0.f);
        rig.put(1, 9.f, 0.f);
        rig.put(2, 9.f, 0.f);
        rig.in.light = true;
        int guard = 0;
        while (rig.count(ArenaEventType::Hit) == 0 && guard++ < 60) rig.tick();
        REQUIRE(rig.count(ArenaEventType::Hit) == 1); // enemy 0, on the first tick of the active part
        rig.put(1, 1.5f, 0.f);
        rig.tick(); // the next tick of the active part
        CHECK(rig.count(ArenaEventType::Hit) == 2); // enemy 1 takes the second slot
        rig.put(2, 1.2f, 0.f);
        rig.run(30);
        CHECK(rig.count(ArenaEventType::Hit) == 2); // none left for enemy 2
        CHECK(rig.fight.enemyHp(2) == 100);
        CHECK(rig.fight.enemyHp(1) == 90);
    }
    {   // a tie by distance goes to the lower index
        ArenaFighter f = ruleFighter();
        f.light[0].maxTargets = 1;
        Rig rig(f, {dummyKind(), dummyKind()});
        rig.put(0, 0.f, 1.5f);
        rig.put(1, 1.5f, 0.f);
        rig.in.light = true;
        rig.run(40);
        REQUIRE(rig.count(ArenaEventType::Hit) == 1);
        CHECK(rig.find(ArenaEventType::Hit)->event.target == 0);
    }
}

TEST_CASE("arena input buffer: a press waits for bufferTime, and only the latest is kept") {
    // light[0]: 0.2 + 0.1 + 0.3 s with cancel 0.5: another action may start from 0.45 s.
    {   // pressed 0.3 s in: held, then started as soon as the recovery allows
        Rig rig(ruleFighter(), {dummyKind()});
        rig.put(0, 1.5f, 0.f);
        rig.in.light = true;
        rig.tick();
        rig.run(17);
        rig.in.light = true;
        rig.tick(); // t = 0.3: can't start yet
        CHECK(rig.count(ArenaEventType::AttackStarted) == 1);
        rig.run(20);
        REQUIRE(rig.count(ArenaEventType::AttackStarted) == 2);
        const Logged* second = rig.find(ArenaEventType::AttackStarted, 1);
        REQUIRE(second);
        CHECK(second->event.move == 1); // the chain went on
        CHECK(second->tick >= 27);
        CHECK(second->tick <= 30);
    }
    {   // pressed 0.1 s in: the buffer runs out before the recovery lets go
        Rig rig(ruleFighter(), {dummyKind()});
        rig.put(0, 1.5f, 0.f);
        rig.in.light = true;
        rig.tick();
        rig.run(5);
        rig.in.light = true;
        rig.tick();
        rig.run(100);
        CHECK(rig.count(ArenaEventType::AttackStarted) == 1);
    }
    {   // a heavy pressed after the light replaces it
        Rig rig(ruleFighter(), {dummyKind()});
        rig.put(0, 1.5f, 0.f);
        rig.in.light = true;
        rig.tick();
        rig.run(17);
        rig.in.light = true;
        rig.tick();
        rig.in.heavy = true;
        rig.tick();
        rig.run(25);
        REQUIRE(rig.count(ArenaEventType::AttackStarted) == 2);
        CHECK(rig.find(ArenaEventType::AttackStarted, 1)->event.move == kArenaHeavyMove);
        CHECK(rig.out().player.action == ArenaAction::Heavy);
    }
}

TEST_CASE("arena light chain: the next attack within chainGap, the first after it, and the wrap") {
    {   // pressed all the time: 0, 1, 2, then round again
        Rig rig(ruleFighter(), {dummyKind()});
        rig.put(0, 1.5f, 0.f);
        for (int i = 0; i < 160; ++i) {
            rig.in.light = true;
            rig.tick();
        }
        std::vector<int> moves;
        for (const Logged& l : rig.log)
            if (l.event.type == ArenaEventType::AttackStarted) moves.push_back(l.event.move);
        REQUIRE(moves.size() >= 5);
        CHECK(moves[0] == 0);
        CHECK(moves[1] == 1);
        CHECK(moves[2] == 2);
        CHECK(moves[3] == 0);
        CHECK(moves[4] == 1);
    }
    for (const float wait : {0.2f, 0.45f}) {   // chainGap 0.3 s after the light ends
        Rig rig(ruleFighter(), {dummyKind()});
        rig.put(0, 1.5f, 0.f);
        rig.in.light = true;
        rig.tick();
        int guard = 0;
        while (rig.out().player.action != ArenaAction::Idle && guard++ < 200) rig.tick();
        rig.runFor(wait);
        rig.in.light = true;
        rig.tick();
        REQUIRE(rig.count(ArenaEventType::AttackStarted) == 2);
        CHECK(rig.find(ArenaEventType::AttackStarted, 1)->event.move == (wait < 0.3f ? 1 : 0));
    }
    {   // another action in between breaks the chain
        Rig rig(ruleFighter(), {dummyKind()});
        rig.put(0, 1.5f, 0.f);
        rig.in.light = true;
        rig.tick();
        rig.run(35);
        rig.in.heavy = true;
        rig.tick();
        int guard = 0;
        while (rig.out().player.action != ArenaAction::Idle && guard++ < 200) rig.tick();
        rig.in.light = true;
        rig.tick();
        REQUIRE(rig.count(ArenaEventType::AttackStarted) == 3);
        CHECK(rig.find(ArenaEventType::AttackStarted, 2)->event.move == 0);
    }
}

TEST_CASE("arena stamina: the cost, needing some to start, StaminaEmpty, and regeneration after a delay") {
    {   // the cost, then back after staminaDelay at staminaRegen per second
        Rig rig(ruleFighter(), {dummyKind()});
        rig.in.heavy = true;
        rig.tick();
        CHECK(rig.fight.stamina() == doctest::Approx(75.f)); // 25
        rig.runFor(0.4f);
        CHECK(rig.fight.stamina() == doctest::Approx(75.f)); // inside the 0.5 s delay
        rig.runFor(0.2f);                                    // t = 0.62: 0.12 s of regeneration
        CHECK(rig.fight.stamina() > 78.f);
        CHECK(rig.fight.stamina() < 83.f);
        rig.runFor(1.0f);
        CHECK(rig.fight.stamina() == doctest::Approx(100.f));
    }
    {   // the dodge costs too
        Rig rig(ruleFighter(), {dummyKind()});
        rig.in.dodge = true;
        rig.tick();
        CHECK(rig.fight.stamina() == doctest::Approx(80.f));
    }
    {   // an action needs more than 0 and may take it to 0
        ArenaFighter f = ruleFighter();
        f.stamina = 5.f;
        f.staminaDelay = 1.f;
        Rig rig(f, {dummyKind()});
        rig.put(0, 1.5f, 0.f);
        rig.in.light = true;
        rig.tick();
        CHECK(rig.out().player.action == ArenaAction::Light);
        CHECK(rig.fight.stamina() == 0.f);
        CHECK(rig.count(ArenaEventType::StaminaEmpty) == 1);
        // With none left a press starts nothing, however long it is held.
        for (int i = 0; i < 50; ++i) {
            rig.in.light = true;
            rig.tick();
        }
        CHECK(rig.count(ArenaEventType::AttackStarted) == 1);
        CHECK(rig.fight.stamina() == 0.f);
        // Once it regenerates, the pressed light starts.
        for (int i = 0; i < 40; ++i) {
            rig.in.light = true;
            rig.tick();
        }
        REQUIRE(rig.count(ArenaEventType::AttackStarted) == 2);
        CHECK(rig.find(ArenaEventType::AttackStarted, 1)->tick >= 60);
        CHECK(rig.find(ArenaEventType::AttackStarted, 1)->tick <= 66);
        CHECK(rig.count(ArenaEventType::StaminaEmpty) == 2);
    }
}

TEST_CASE("arena dodge: a roll along the stick, a backstep with the stick idle") {
    const auto roll = [](Vec2 stick, Vec2& sum, Vec2& firstVelocity, Vec2& facing) {
        Rig rig(ruleFighter(), {dummyKind()});
        rig.put(0, 20.f, 0.f);
        rig.in.move = stick;
        rig.in.dodge = true;
        const ArenaOutput& out = rig.tick();
        CHECK(out.player.action == ArenaAction::Dodge);
        firstVelocity = out.player.velocity;
        facing = out.player.facing;
        sum = Vec2{out.player.velocity.x * kDt, out.player.velocity.y * kDt};
        rig.in.move = Vec2{}; // the roll's direction was taken when it started; the stick can go
        for (int i = 0; i < 45; ++i) {
            rig.tick();
            sum.x += rig.out().player.velocity.x * kDt;
            sum.y += rig.out().player.velocity.y * kDt;
        }
        CHECK(rig.out().player.action != ArenaAction::Dodge);
    };
    Vec2 sum, v, facing;
    roll(Vec2{0.f, 0.f}, sum, v, facing); // idle stick: straight back, away from the facing (+x)
    CHECK(v.x == doctest::Approx(-5.f)); // 3 m in 0.6 s
    CHECK(std::fabs(v.y) < 1e-3f);
    CHECK(facing.x == doctest::Approx(1.f)); // a backstep doesn't turn
    CHECK(sum.x == doctest::Approx(-3.f).epsilon(0.02));
    roll(Vec2{0.f, 0.3f}, sum, v, facing); // a light push still rolls the whole distance, the way it points
    CHECK(std::fabs(v.x) < 1e-3f);
    CHECK(v.y == doctest::Approx(5.f));
    CHECK(facing.y == doctest::Approx(1.f));
    CHECK(sum.y == doctest::Approx(3.f).epsilon(0.02));
}

TEST_CASE("arena dodge: the i-frames give Evaded once per attack, a hit lands outside them") {
    const ArenaAttack bite = swing(0.4f, 0.15f, 0.5f, 2.f, 20); // hits at 0.4 .. 0.55 s
    {   // dodge pressed at 0.25 s: i-frames cover 0.35 .. 0.65 s
        Rig rig(ruleFighter(), {hitterKind(bite)});
        rig.put(0, 1.5f, 0.f);
        rig.run(15);
        rig.in.dodge = true;
        rig.tick();
        rig.run(55); // t = 1.25: before the enemy's next attack can land
        CHECK(rig.count(ArenaEventType::Evaded) == 1); // though its active part spans nine ticks
        CHECK(rig.count(ArenaEventType::Hit) == 0);
        CHECK(rig.fight.hp() == 100);
        CHECK(rig.fight.stats().evades == 1);
        const Logged* evaded = rig.find(ArenaEventType::Evaded);
        REQUIRE(evaded);
        CHECK(evaded->event.source == 0);
        CHECK(evaded->event.target == kArenaPlayer);
    }
    {   // dodge pressed at once: its i-frames (0.1 .. 0.4 s) are over before a slower bite lands at 0.6 s
        Rig rig(ruleFighter(), {hitterKind(swing(0.6f, 0.15f, 0.5f, 2.f, 20))});
        rig.put(0, 1.5f, 0.f);
        rig.in.dodge = true;
        rig.tick();
        rig.run(55);
        CHECK(rig.count(ArenaEventType::Evaded) == 0);
        CHECK(rig.count(ArenaEventType::Hit) == 1);
        CHECK(rig.fight.hp() == 80);
        CHECK(rig.fight.stats().hitsTaken == 1);
    }
    {   // and with no dodge at all
        Rig rig(ruleFighter(), {hitterKind(bite)});
        rig.put(0, 1.5f, 0.f);
        rig.run(40);
        CHECK(rig.count(ArenaEventType::Hit) == 1);
        CHECK(rig.fight.hp() == 80);
        CHECK(rig.find(ArenaEventType::Hit)->event.target == kArenaPlayer);
        CHECK(rig.find(ArenaEventType::Hit)->event.amount == 20);
    }
}

TEST_CASE("arena dodge: the direction and the backstep flag are set once, when a dodge starts") {
    const ArenaRing ring{Vec2{0.f, 0.f}, 6.f};
    Rig rig(ruleFighter(), {dummyKind()}, 1, ring);
    rig.put(0, -20.f, 0.f);
    // Before any dodge: the defaults.
    CHECK(rig.out().player.dodgeDir.x == 1.f);
    CHECK(rig.out().player.dodgeDir.y == 0.f);
    CHECK_FALSE(rig.out().player.backstep);
    // A roll along the stick, started at the ring's edge: the roll slides along it, the direction stays the stick's.
    rig.bodies.player = Vec2{5.5f, 0.f};
    rig.in.move = Vec2{0.6f, 0.8f};
    rig.in.dodge = true;
    bool always = true;
    float widest = 0.f;
    for (int i = 0; i < 60; ++i) {
        const ArenaOutput& out = rig.tick();
        rig.bodies.player.x += out.player.velocity.x * kDt;
        rig.bodies.player.y += out.player.velocity.y * kDt;
        always = always && std::fabs(out.player.dodgeDir.x - 0.6f) < 1e-4f && std::fabs(out.player.dodgeDir.y - 0.8f) < 1e-4f &&
                 !out.player.backstep;
        const float speed = std::hypot(out.player.velocity.x, out.player.velocity.y);
        if (out.player.action == ArenaAction::Dodge && speed > 1.f)
            widest = std::max(widest, std::acos(std::min(1.f, (out.player.velocity.x * 0.6f + out.player.velocity.y * 0.8f) / speed)));
    }
    CHECK(always);         // set when it started, and kept after it ended
    CHECK(widest > 0.3f);  // while the velocity itself turned along the edge
    // With the stick idle it is a backstep: away from the facing (which the roll turned to the stick), and it stays so.
    rig.in.move = Vec2{};
    rig.in.dodge = true;
    const ArenaOutput& back = rig.tick();
    CHECK(back.player.action == ArenaAction::Dodge);
    CHECK(back.player.backstep);
    CHECK(back.player.dodgeDir.x == doctest::Approx(-0.6f));
    CHECK(back.player.dodgeDir.y == doctest::Approx(-0.8f));
    rig.run(50);
    CHECK(rig.out().player.action != ArenaAction::Dodge);
    CHECK(rig.out().player.backstep);
    CHECK(rig.out().player.dodgeDir.x == doctest::Approx(-0.6f));
    // The next roll clears it.
    rig.in.move = Vec2{0.f, 1.f};
    rig.in.dodge = true;
    const ArenaOutput& roll = rig.tick();
    CHECK_FALSE(roll.player.backstep);
    CHECK(roll.player.dodgeDir.y == doctest::Approx(1.f));
    CHECK(std::hypot(roll.player.dodgeDir.x, roll.player.dodgeDir.y) == doctest::Approx(1.f));
}

TEST_CASE("arena acceleration: the player's velocity ramps by moveAccel and moveBrake") {
    ArenaFighter f = ruleFighter(); // moveSpeed 4
    f.moveAccel = 20.f;             // 0.2 s from standing to full speed
    f.moveBrake = 40.f;             // 0.1 s from full speed to standing
    {
        Rig rig(f, {dummyKind()});
        rig.put(0, 20.f, 0.f);
        rig.in.move = Vec2{1.f, 0.f};
        float previous = 0.f;
        int toSpeed = 0;
        bool smooth = true;
        for (int i = 1; i <= 30; ++i) {
            const float v = rig.tick().player.velocity.x;
            smooth = smooth && v - previous <= 20.f * kDt + 1e-4f && v >= previous - 1e-5f;
            if (toSpeed == 0 && v >= 4.f - 1e-4f) toSpeed = i;
            previous = v;
        }
        CHECK(smooth);
        CHECK(toSpeed >= 11);
        CHECK(toSpeed <= 13); // moveSpeed / moveAccel = 0.2 s: 12 ticks
        CHECK(previous == doctest::Approx(4.f));
        rig.in.move = Vec2{};
        int toStop = 0;
        smooth = true;
        for (int i = 1; i <= 20; ++i) {
            const float v = rig.tick().player.velocity.x;
            smooth = smooth && previous - v <= 40.f * kDt + 1e-4f && v <= previous + 1e-5f;
            if (toStop == 0 && v <= 1e-4f) toStop = i;
            previous = v;
        }
        CHECK(smooth);
        CHECK(toStop >= 5);
        CHECK(toStop <= 7); // moveSpeed / moveBrake = 0.1 s: 6 ticks
        CHECK(previous == 0.f);
    }
    {   // A turn is one vector moving toward the wanted one. The rate is moveAccel while the wanted speed is above the
        // current one, and a reversal asks for 4 m/s of a body that has dropped below it after the first tick: so it
        // brakes for one tick and then goes at moveAccel, 8 m/s in all (23 ticks), not 8 / moveBrake (12).
        Rig rig(f, {dummyKind()});
        rig.put(0, 20.f, 0.f);
        rig.in.move = Vec2{1.f, 0.f};
        rig.run(20);
        REQUIRE(rig.out().player.velocity.x == doctest::Approx(4.f));
        rig.in.move = Vec2{-1.f, 0.f};
        int turned = 0;
        float previous = 4.f;
        bool smooth = true;
        for (int i = 1; i <= 40 && turned == 0; ++i) {
            const float v = rig.tick().player.velocity.x;
            smooth = smooth && std::fabs(v - previous) <= 40.f * kDt + 1e-4f;
            previous = v;
            if (v <= -4.f + 1e-4f) turned = i;
        }
        CHECK(smooth);
        CHECK(turned >= 21);
        CHECK(turned <= 25);
    }
    {   // guard and heal ramp too, toward their scaled speeds
        Rig guard(f, {dummyKind()});
        guard.put(0, 20.f, 0.f);
        guard.in.guard = true;
        guard.in.move = Vec2{1.f, 0.f};
        const ArenaOutput& g = guard.tick();
        CHECK(g.player.action == ArenaAction::Guard);
        CHECK(g.player.velocity.x == doctest::Approx(20.f * kDt));
        guard.run(30);
        CHECK(guard.out().player.velocity.x == doctest::Approx(2.f)); // 4 * guardMoveScale
        Rig heal(f, {dummyKind()});
        heal.put(0, 20.f, 0.f);
        heal.in.heal = true;
        heal.in.move = Vec2{1.f, 0.f};
        const ArenaOutput& h = heal.tick();
        CHECK(h.player.action == ArenaAction::Heal);
        CHECK(h.player.velocity.x == doctest::Approx(20.f * kDt));
    }
}

TEST_CASE("arena acceleration: 0 is instant, in either direction on its own") {
    const ArenaFighter standard;
    CHECK(standard.moveAccel == 0.f);
    CHECK(standard.moveBrake == 0.f);
    CHECK(ArenaEnemyKind{}.accel == 0.f);
    {   // both 0: full speed on the first tick, standing on the next
        Rig rig(ruleFighter(), {dummyKind()});
        rig.put(0, 20.f, 0.f);
        rig.in.move = Vec2{1.f, 0.f};
        CHECK(rig.tick().player.velocity.x == doctest::Approx(4.f));
        rig.in.move = Vec2{};
        CHECK(rig.tick().player.velocity.x == 0.f);
    }
    {   // moveAccel 0 with a moveBrake: gaining speed snaps, losing it ramps
        ArenaFighter f = ruleFighter();
        f.moveBrake = 40.f;
        Rig rig(f, {dummyKind()});
        rig.put(0, 20.f, 0.f);
        rig.in.move = Vec2{1.f, 0.f};
        CHECK(rig.tick().player.velocity.x == doctest::Approx(4.f));
        rig.in.move = Vec2{};
        CHECK(rig.tick().player.velocity.x == doctest::Approx(4.f - 40.f * kDt));
    }
    {   // moveBrake 0 with a moveAccel: gaining speed ramps, losing it snaps
        ArenaFighter f = ruleFighter();
        f.moveAccel = 20.f;
        Rig rig(f, {dummyKind()});
        rig.put(0, 20.f, 0.f);
        rig.in.move = Vec2{1.f, 0.f};
        CHECK(rig.tick().player.velocity.x == doctest::Approx(20.f * kDt));
        rig.run(20);
        rig.in.move = Vec2{};
        CHECK(rig.tick().player.velocity.x == 0.f);
    }
}

TEST_CASE("arena acceleration: lunges and dodges are never ramped, and the ramp resumes from them") {
    ArenaFighter f = ruleFighter();
    f.moveAccel = 2.f; // a crawl: any ramp would show at once
    f.moveBrake = 5.f;
    f.light[0].lunge = 1.f; // 1 m over the startup and active parts (0.3 s)
    {   // the lunge is written at once, and held while it lasts
        Rig rig(f, {dummyKind()});
        rig.put(0, 20.f, 0.f);
        rig.in.light = true;
        const ArenaOutput& out = rig.tick();
        CHECK(out.player.action == ArenaAction::Light);
        CHECK(out.player.velocity.x == doctest::Approx(1.f / 0.3f));
        rig.run(10);
        CHECK(rig.out().player.velocity.x == doctest::Approx(1.f / 0.3f));
    }
    {   // a dodge too, from a standing start
        Rig rig(f, {dummyKind()});
        rig.put(0, 20.f, 0.f);
        rig.in.dodge = true;
        const ArenaOutput& out = rig.tick();
        CHECK(out.player.action == ArenaAction::Dodge);
        CHECK(out.player.velocity.x == doctest::Approx(-5.f)); // 3 m in 0.6 s, from the first tick
    }
    {   // The dodge is cancelled into Guard at 0.45 s (the guard was pressed with it): the ramp starts from the roll's
        // velocity and brakes it by moveBrake, it doesn't start from standing or snap.
        Rig rig(f, {dummyKind()});
        rig.put(0, 20.f, 0.f);
        rig.in.guard = true;
        rig.in.dodge = true;
        rig.tick();
        int guardTick = 0;
        float first = 0.f, second = 0.f;
        for (int i = 0; i < 60 && guardTick == 0; ++i) {
            const ArenaOutput& out = rig.tick();
            if (out.player.action == ArenaAction::Guard) {
                guardTick = rig.ticks;
                first = out.player.velocity.x;
                second = rig.tick().player.velocity.x;
            }
        }
        REQUIRE(guardTick > 0);
        CHECK(guardTick >= 28);
        CHECK(guardTick <= 30); // dodgeCancel 0.75 of 0.6 s
        CHECK(first == doctest::Approx(-5.f + 5.f * kDt).epsilon(0.002));
        CHECK(second == doctest::Approx(-5.f + 10.f * kDt).epsilon(0.002));
    }
}

TEST_CASE("arena acceleration: an enemy ramps by its kind's accel, both ways, and 0 is instant") {
    for (const float accel : {0.f, 6.f}) {
        ArenaEnemyKind k = dummyKind(); // asks for no token: it only approaches and strafes
        k.accel = accel;
        Rig rig(ruleFighter(), {k});
        rig.put(0, 6.f, 0.f); // beyond preferredRange: Approach, walking at 1.8 m/s toward the player
        float previous = 0.f;
        int toSpeed = 0;
        bool smooth = true;
        for (int i = 1; i <= 40; ++i) {
            const Vec2 v = rig.tick().enemies[0].velocity;
            const float speed = std::hypot(v.x, v.y);
            smooth = smooth && (accel <= 0.f || speed - previous <= accel * kDt + 1e-4f);
            if (toSpeed == 0 && speed >= 1.8f - 1e-4f) toSpeed = i;
            previous = speed;
        }
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Approach);
        CHECK(previous == doctest::Approx(1.8f));
        CHECK(rig.out().enemies[0].velocity.x < 0.f);
        CHECK(smooth);
        if (accel > 0.f) {
            CHECK(toSpeed >= 17);
            CHECK(toSpeed <= 19); // 1.8 / 6 = 0.3 s: 18 ticks
        } else {
            CHECK(toSpeed == 1);
        }
        // Turning into the strafe (within preferredRange) turns the velocity round: by accel at most per tick, as a
        // vector, with no jump; instantly with accel 0.
        rig.put(0, 3.f, 0.f);
        Vec2 last = rig.out().enemies[0].velocity;
        float biggestStep = 0.f;
        for (int i = 0; i < 60; ++i) {
            const Vec2 v = rig.tick().enemies[0].velocity;
            biggestStep = std::max(biggestStep, std::hypot(v.x - last.x, v.y - last.y));
            last = v;
        }
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Strafe);
        if (accel > 0.f) CHECK(biggestStep <= accel * kDt + 1e-4f);
        else CHECK(biggestStep > 2.f);
        CHECK(std::hypot(last.x, last.y) == doctest::Approx(1.4f).epsilon(0.01)); // strafeSpeed
    }
    {   // a lunge is not ramped by the enemy's accel either, and the ramp goes on from it afterwards
        ArenaAttack lunge = swing(0.3f, 0.1f, 0.3f, 2.f, 0);
        lunge.lunge = 0.8f;
        ArenaEnemyKind k = hitterKind(lunge);
        k.accel = 1.f; // a crawl
        Rig rig(ruleFighter(), {k});
        rig.put(0, 1.5f, 0.f);
        const ArenaOutput& out = rig.tick();
        CHECK(out.enemies[0].brain == ArenaBrain::Attack);
        CHECK(std::hypot(out.enemies[0].velocity.x, out.enemies[0].velocity.y) == doctest::Approx(0.8f / 0.4f));
    }
}

TEST_CASE("arena guard: a block takes damage * (1 - guardAbsorb) and stamina, the guard breaks when it empties") {
    ArenaAttack bite = swing(0.4f, 0.15f, 0.5f, 2.f, 20);
    bite.guardDamage = 30.f;
    {   // held from the start: the parry window is long over when the bite lands
        Rig rig(ruleFighter(), {hitterKind(bite)});
        rig.put(0, 1.5f, 0.f);
        rig.in.guard = true;
        rig.runFor(0.7f);
        CHECK(rig.count(ArenaEventType::Blocked) == 1);
        CHECK(rig.count(ArenaEventType::Parried) == 0);
        CHECK(rig.count(ArenaEventType::GuardBroken) == 0);
        CHECK(rig.find(ArenaEventType::Blocked)->event.amount == 30);
        CHECK(rig.fight.stamina() == doctest::Approx(70.f));
        CHECK(rig.fight.hp() == 95); // 20 * (1 - 0.75)
        REQUIRE(rig.count(ArenaEventType::Hit) == 1);
        CHECK(rig.find(ArenaEventType::Hit)->event.amount == 5);
        CHECK(rig.fight.stats().blocks == 1);
        CHECK(rig.fight.stats().hitsTaken == 0);
        CHECK(rig.out().player.action == ArenaAction::Guard);
    }
    {   // a block that empties the stamina breaks the guard
        ArenaFighter f = ruleFighter();
        f.stamina = 25.f;
        Rig rig(f, {hitterKind(bite)});
        rig.put(0, 1.5f, 0.f);
        rig.in.guard = true;
        rig.runFor(0.5f);
        rig.put(0, 30.f, 0.f); // no second attack
        rig.runFor(0.2f);
        CHECK(rig.count(ArenaEventType::GuardBroken) == 1);
        CHECK(rig.find(ArenaEventType::Blocked)->event.amount == 25); // all it had
        CHECK(rig.out().player.action == ArenaAction::GuardBreak);
        CHECK(rig.fight.stats().guardBreaks == 1);
        rig.runFor(1.2f); // guardBreakTime is 1 s
        CHECK(rig.out().player.action != ArenaAction::GuardBreak);
    }
    {   // an unblockable move ignores the guard
        ArenaEnemyKind k = hitterKind(bite);
        k.moves[0].unblockable = true;
        Rig rig(ruleFighter(), {k});
        rig.put(0, 1.5f, 0.f);
        rig.in.guard = true;
        rig.runFor(0.7f);
        CHECK(rig.count(ArenaEventType::Blocked) == 0);
        CHECK(rig.fight.hp() == 80);
        CHECK(rig.fight.stamina() == doctest::Approx(100.f));
    }
    {   // the arc: the guard covers the front half
        Rig rig(ruleFighter(), {hitterKind(bite)});
        rig.put(0, -1.5f, 0.f); // behind the player, who faces +x
        rig.in.guard = true;
        rig.runFor(0.7f);
        CHECK(rig.count(ArenaEventType::Blocked) == 0);
        CHECK(rig.fight.hp() == 80);
    }
    {   // moving while guarding is scaled
        Rig rig(ruleFighter(), {dummyKind()});
        rig.put(0, 20.f, 0.f);
        rig.in.guard = true;
        rig.in.move = Vec2{1.f, 0.f};
        const ArenaOutput& out = rig.tick();
        CHECK(out.player.action == ArenaAction::Guard);
        CHECK(out.player.velocity.x == doctest::Approx(2.f)); // 4 m/s * 0.5
    }
}

TEST_CASE("arena parry: a window after the press, no window on a mashed press, and Open afterwards") {
    ArenaAttack bite = swing(0.4f, 0.15f, 0.5f, 2.f, 20);
    bite.guardDamage = 30.f;
    {   // pressed 0.3 s in: the window (0.3 .. 0.5 s) holds the hit at 0.42 s
        Rig rig(ruleFighter(), {hitterKind(bite)});
        rig.put(0, 1.5f, 0.f);
        rig.run(18);
        rig.in.guard = true;
        const ArenaOutput& out = rig.tick();
        CHECK(out.player.parrying);
        rig.run(19);
        CHECK(rig.count(ArenaEventType::Parried) == 1);
        CHECK(rig.count(ArenaEventType::Blocked) == 0);
        CHECK(rig.count(ArenaEventType::Hit) == 0);
        CHECK(rig.fight.hp() == 100);
        CHECK(rig.fight.stamina() == doctest::Approx(100.f)); // no cost
        CHECK(rig.fight.stats().parries == 1);
        const Logged* parried = rig.find(ArenaEventType::Parried);
        REQUIRE(parried);
        CHECK(parried->event.source == 0);
        CHECK(parried->event.target == kArenaPlayer);
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Open);
        CHECK_FALSE(rig.out().enemies[0].token);
        rig.runFor(1.0f);
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Open); // openTime is 1.5 s
        rig.runFor(0.6f);
        CHECK(rig.out().enemies[0].brain != ArenaBrain::Open);
        CHECK(rig.out().player.parrying == false);
    }
    {   // a second press 0.45 s after the first (parryRearm 0.5 s) opens no window: the bite at 0.52 s is blocked
        Rig rig(ruleFighter(), {hitterKind(swing(0.5f, 0.15f, 0.5f, 2.f, 20))});
        rig.put(0, 1.5f, 0.f);
        rig.in.guard = true;
        rig.tick(); // press at t = 0
        rig.in.guard = false;
        rig.run(26);
        rig.in.guard = true;
        rig.tick(); // press at t = 0.45
        rig.run(20);
        CHECK(rig.count(ArenaEventType::Parried) == 0);
        CHECK(rig.count(ArenaEventType::Blocked) == 1);
    }
    {   // 0.55 s after the first it opens one: the bite at 0.62 s is parried
        Rig rig(ruleFighter(), {hitterKind(swing(0.6f, 0.15f, 0.5f, 2.f, 20))});
        rig.put(0, 1.5f, 0.f);
        rig.in.guard = true;
        rig.tick();
        rig.in.guard = false;
        rig.run(32);
        rig.in.guard = true;
        rig.tick(); // press at t = 0.55
        rig.run(20);
        CHECK(rig.count(ArenaEventType::Parried) == 1);
        CHECK(rig.count(ArenaEventType::Blocked) == 0);
    }
    {   // a move that isn't parryable is blocked, whatever the timing
        ArenaEnemyKind k = hitterKind(bite);
        k.moves[0].parryable = false;
        Rig rig(ruleFighter(), {k});
        rig.put(0, 1.5f, 0.f);
        rig.run(18);
        rig.in.guard = true;
        rig.run(20);
        CHECK(rig.count(ArenaEventType::Parried) == 0);
        CHECK(rig.count(ArenaEventType::Blocked) == 1);
        CHECK(rig.out().enemies[0].brain != ArenaBrain::Open);
    }
    {   // the attacker has to be inside the guard arc
        Rig rig(ruleFighter(), {hitterKind(bite)});
        rig.put(0, -1.5f, 0.f);
        rig.run(18);
        rig.in.guard = true;
        rig.run(20);
        CHECK(rig.count(ArenaEventType::Parried) == 0);
        CHECK(rig.fight.hp() == 80);
    }
}

TEST_CASE("arena riposte: only in reach and in front, the enemy goes Down, invulnerable throughout") {
    const ArenaAttack bite = swing(0.4f, 0.15f, 0.5f, 2.f, 20);
    // Leaves enemy 0 open at (x, y): parry its bite, then let go of the guard (t = 0.48 s).
    const auto parry = [](Rig& rig, float x, float y) {
        rig.put(0, x, y);
        rig.run(18);
        rig.in.guard = true;
        rig.run(10);
        rig.in.guard = false;
        rig.tick();
        REQUIRE(rig.out().enemies[0].brain == ArenaBrain::Open);
    };
    {   // in reach and in front
        Rig rig(ruleFighter(), {hitterKind(bite)});
        parry(rig, 1.5f, 0.f);
        rig.in.light = true;
        const ArenaOutput& out = rig.tick();
        CHECK(out.player.action == ArenaAction::Riposte);
        CHECK(out.player.move == kArenaRiposteMove);
        CHECK(out.player.invulnerable);
        CHECK(rig.find(ArenaEventType::AttackStarted, 1)->event.move == kArenaRiposteMove);
        bool alwaysInvulnerable = true, sawEnd = false;
        for (int i = 0; i < 50; ++i) {
            const ArenaOutput& o = rig.tick();
            if (o.player.action == ArenaAction::Riposte) alwaysInvulnerable = alwaysInvulnerable && o.player.invulnerable;
            else sawEnd = sawEnd || !o.player.invulnerable;
        }
        CHECK(alwaysInvulnerable);
        CHECK(sawEnd);
        const Logged* hit = rig.find(ArenaEventType::Hit);
        REQUIRE(hit);
        CHECK(hit->event.move == kArenaRiposteMove);
        CHECK(hit->event.amount == 50);
        CHECK(rig.fight.enemyHp(0) == 50);
        CHECK(rig.fight.stats().ripostes == 1);
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Down); // t = 1.3 s: the riposte landed at about 0.6 s
        rig.runFor(0.1f);
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Down); // downTime is 1 s from the hit
        rig.runFor(1.0f);
        CHECK(rig.out().enemies[0].brain != ArenaBrain::Down);
    }
    {   // out of reach: an ordinary light, and the enemy stays open
        Rig rig(ruleFighter(), {hitterKind(bite)});
        parry(rig, 1.5f, 0.f);
        rig.put(0, 3.f, 0.f);
        rig.in.light = true;
        const ArenaOutput& out = rig.tick();
        CHECK(out.player.action == ArenaAction::Light);
        CHECK(out.player.move == 0);
        rig.run(30);
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Open);
        CHECK(rig.fight.stats().ripostes == 0);
    }
    {   // not in front: the enemy at the player's side
        Rig rig(ruleFighter(), {hitterKind(bite)});
        parry(rig, 1.5f, 0.f);
        rig.put(0, 0.f, 1.5f);
        rig.in.light = true;
        const ArenaOutput& out = rig.tick();
        CHECK(out.player.action == ArenaAction::Light);
        CHECK(out.player.move == 0);
    }
    {   // with maxTargets 1 the riposte goes to the enemy it is aimed at, not to a nearer one
        ArenaFighter f = ruleFighter();
        f.riposte.maxTargets = 1;
        Rig rig(f, {hitterKind(bite), dummyKind()});
        rig.put(1, 20.f, 0.f);
        parry(rig, 2.f, 0.f);
        rig.put(1, 1.f, 0.f); // nearer, in front, and in reach of the swing
        rig.in.light = true;
        const ArenaOutput& out = rig.tick();
        CHECK(out.player.action == ArenaAction::Riposte);
        rig.run(30);
        CHECK(rig.fight.enemyHp(0) == 50);
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Down);
        CHECK(rig.fight.enemyHp(1) == 100);
        CHECK(rig.fight.stats().ripostes == 1);
    }
    {   // an enemy's attack that lands during the riposte does nothing
        Rig rig(ruleFighter(), {hitterKind(bite), hitterKind(swing(0.9f, 0.15f, 0.5f, 2.f, 20))}, 2);
        rig.put(1, 0.f, 1.5f);
        parry(rig, 1.5f, 0.f);
        rig.in.light = true;
        rig.tick();
        rig.run(60);
        CHECK(rig.count(ArenaEventType::Evaded) == 1);
        CHECK(rig.find(ArenaEventType::Evaded)->event.source == 1);
        CHECK(rig.fight.hp() == 100);
    }
}

TEST_CASE("arena heal: it applies after healTime, a hitstun before that cancels it and keeps the charge") {
    {   // applies after healTime, and the charge goes then
        const ArenaAttack bite = swing(0.2f, 0.1f, 0.5f, 2.f, 40);
        Rig rig(ruleFighter(), {hitterKind(bite)});
        rig.put(0, 1.5f, 0.f);
        rig.runFor(0.4f); // hit at 0.2 s: hp 60
        REQUIRE(rig.fight.hp() == 60);
        rig.put(0, 30.f, 0.f); // out of the way
        rig.in.heal = true;
        const ArenaOutput& out = rig.tick();
        CHECK(out.player.action == ArenaAction::Heal);
        CHECK(rig.fight.healCharges() == 2);
        rig.runFor(0.3f);
        CHECK(rig.fight.hp() == 60);
        CHECK(rig.count(ArenaEventType::Healed) == 0);
        CHECK(rig.fight.healCharges() == 2);
        rig.runFor(0.3f);
        CHECK(rig.fight.hp() == 100);
        REQUIRE(rig.count(ArenaEventType::Healed) == 1);
        CHECK(rig.find(ArenaEventType::Healed)->event.amount == 40);
        CHECK(rig.fight.healCharges() == 1);
        CHECK(rig.fight.stats().heals == 1);
        rig.runFor(0.3f); // the recovery is over
        CHECK(rig.out().player.action == ArenaAction::Idle);
    }
    {   // a heal can't heal past the maximum: only what was missing is restored
        const ArenaAttack bite = swing(0.2f, 0.1f, 0.5f, 2.f, 15);
        Rig rig(ruleFighter(), {hitterKind(bite)});
        rig.put(0, 1.5f, 0.f);
        rig.runFor(0.4f);
        rig.put(0, 30.f, 0.f);
        rig.in.heal = true;
        rig.runFor(1.f);
        CHECK(rig.fight.hp() == 100);
        CHECK(rig.find(ArenaEventType::Healed)->event.amount == 15);
    }
    {   // a hit that breaks the poise during the heal cancels it, and the charge stays
        ArenaAttack bite = swing(0.2f, 0.1f, 0.5f, 2.f, 40);
        bite.poiseDamage = 40.f; // more than the poise (30)
        ArenaFighter f = ruleFighter();
        f.healTime = 0.8f;
        Rig rig(f, {hitterKind(bite)});
        rig.put(0, 1.5f, 0.f);
        rig.in.heal = true;
        rig.tick();
        CHECK(rig.out().player.action == ArenaAction::Heal);
        rig.run(14); // the hit lands at 0.2 s
        CHECK(rig.out().player.action == ArenaAction::Hitstun);
        CHECK(rig.fight.hp() == 60);
        rig.put(0, 30.f, 0.f);
        rig.runFor(1.5f);
        CHECK(rig.count(ArenaEventType::Healed) == 0);
        CHECK(rig.fight.healCharges() == 2);
        CHECK(rig.fight.hp() == 60);
        CHECK(rig.out().player.action == ArenaAction::Idle);
    }
    {   // no charge, no heal
        ArenaFighter f = ruleFighter();
        f.healCharges = 0;
        Rig rig(f, {dummyKind()});
        rig.in.heal = true;
        const ArenaOutput& out = rig.tick();
        CHECK(out.player.action == ArenaAction::Idle);
        rig.runFor(1.5f);
        CHECK(rig.count(ArenaEventType::Healed) == 0);
        // and with one charge, the second press does nothing
        ArenaFighter one = ruleFighter();
        one.healCharges = 1;
        Rig again(one, {dummyKind()});
        again.in.heal = true;
        again.runFor(1.2f);
        CHECK(again.count(ArenaEventType::Healed) == 1);
        CHECK(again.fight.healCharges() == 0);
        again.in.heal = true;
        const ArenaOutput& out2 = again.tick();
        CHECK(out2.player.action == ArenaAction::Idle);
    }
}

TEST_CASE("arena poise: a hit that doesn't break it staggers nothing, the second does, it refills after a delay") {
    // One string of two hits 0.8 s apart, 20 poise each.
    ArenaAttack hit = swing(0.2f, 0.1f, 0.5f, 2.f, 10);
    hit.poiseDamage = 20.f;
    ArenaEnemyKind two = hitterKind(hit);
    two.moveCount = 2;
    two.moves[0].chain = 1;
    two.moves[1] = two.moves[0];
    two.moves[1].chain = -1;
    two.moves[1].weight = 0.f;
    {   // poiseDelay 1 s: the poise hasn't come back by the second hit
        Rig rig(ruleFighter(), {two}); // poise 30
        rig.put(0, 1.5f, 0.f);
        rig.run(20);
        CHECK(rig.count(ArenaEventType::Hit) == 1);
        CHECK(rig.fight.poise() == doctest::Approx(10.f));
        CHECK(rig.out().player.action != ArenaAction::Hitstun);
        CHECK(rig.count(ArenaEventType::Staggered) == 0);
        rig.run(55); // the second hit at about 1.0 s
        CHECK(rig.count(ArenaEventType::Hit) == 2);
        CHECK(rig.count(ArenaEventType::Staggered) == 1);
        const Logged* staggered = rig.find(ArenaEventType::Staggered);
        REQUIRE(staggered);
        CHECK(staggered->event.target == kArenaPlayer);
        CHECK(rig.fight.poise() == doctest::Approx(30.f)); // refilled with the break
        CHECK(rig.fight.stats().staggers == 0);            // that counts the enemies' only
        // Hitstun lasts 0.4 s.
        Rig also(ruleFighter(), {two});
        also.put(0, 1.5f, 0.f);
        bool stunned = false;
        int stunTicks = 0;
        for (int i = 0; i < 90; ++i) {
            const ArenaOutput& out = also.tick();
            if (out.player.action == ArenaAction::Hitstun) {
                stunned = true;
                ++stunTicks;
            }
        }
        CHECK(stunned);
        CHECK(stunTicks >= 23);
        CHECK(stunTicks <= 25);
    }
    {   // poiseDelay 0.5 s: back to full before the second hit
        ArenaFighter f = ruleFighter();
        f.poiseDelay = 0.5f;
        Rig rig(f, {two});
        rig.put(0, 1.5f, 0.f);
        rig.run(75);
        CHECK(rig.count(ArenaEventType::Hit) == 2);
        CHECK(rig.count(ArenaEventType::Staggered) == 0);
        CHECK(rig.fight.poise() == doctest::Approx(10.f));
    }
    {   // hitstun cancels the action in progress
        ArenaAttack big = swing(0.2f, 0.1f, 0.5f, 2.f, 10);
        big.poiseDamage = 40.f;
        Rig rig(ruleFighter(), {hitterKind(big)});
        rig.put(0, 1.5f, 0.f);
        rig.in.heavy = true; // 0.4 s of startup: the hit lands first
        rig.tick();
        rig.run(20);
        CHECK(rig.out().player.action == ArenaAction::Hitstun);
        rig.put(0, 30.f, 0.f);
        rig.run(60);
        bool playerHit = false;
        for (const Logged& l : rig.log) playerHit = playerHit || (l.event.type == ArenaEventType::Hit && l.event.source == kArenaPlayer);
        CHECK_FALSE(playerHit);
    }
    {   // an enemy's poise: a hit past it staggers, and the enemy is out for staggerTime
        ArenaFighter f = ruleFighter();
        f.light[0].poiseDamage = 40.f;
        Rig rig(f, {dummyKind()}); // poise 30
        rig.put(0, 1.5f, 0.f);
        rig.in.light = true;
        rig.run(20);
        CHECK(rig.count(ArenaEventType::Staggered) == 1);
        CHECK(rig.find(ArenaEventType::Staggered)->event.target == 0);
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Staggered);
        CHECK(rig.fight.stats().staggers == 1);
        CHECK(rig.fight.enemyPoise(0) == doctest::Approx(30.f)); // refilled with the break
        rig.runFor(1.2f); // staggerTime is 1 s
        CHECK(rig.out().enemies[0].brain != ArenaBrain::Staggered);
    }
}

TEST_CASE("arena Victory and Defeat: the phase, the events, and nothing happens afterwards") {
    {   // Victory: the last enemy dies
        ArenaEnemyKind weak = dummyKind();
        weak.hp = 20;
        Rig rig(ruleFighter(), {weak});
        rig.put(0, 1.5f, 0.f);
        for (int i = 0; i < 200 && !rig.fight.over(); ++i) {
            rig.in.light = true;
            rig.tick();
        }
        CHECK(rig.fight.phase() == ArenaPhase::Victory);
        CHECK(rig.fight.over());
        CHECK_FALSE(rig.fight.enemyAlive(0));
        CHECK(rig.fight.enemyHp(0) == 0);
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Dead);
        const Logged* downed = rig.find(ArenaEventType::Downed);
        const Logged* victory = rig.find(ArenaEventType::Victory);
        REQUIRE(downed);
        REQUIRE(victory);
        CHECK(downed->event.target == 0);
        CHECK(downed->event.source == kArenaPlayer);
        CHECK(downed->tick == victory->tick);
        CHECK(rig.count(ArenaEventType::Defeat) == 0);
        // Afterwards tick() only reports the phase.
        const float time = rig.fight.time();
        const std::size_t events = rig.log.size();
        const ArenaStats stats = rig.fight.stats();
        rig.in.light = true;
        rig.in.move = Vec2{1.f, 0.f};
        const ArenaOutput& out = rig.tick();
        rig.run(20);
        CHECK(out.events.empty());
        CHECK(out.player.velocity.x == 0.f);
        CHECK(rig.fight.time() == time);
        CHECK(rig.log.size() == events);
        CHECK(rig.fight.stats().time == stats.time);
        CHECK(rig.fight.stats().hits == stats.hits);
        CHECK(rig.fight.phase() == ArenaPhase::Victory);
        const ArenaInput idle = rig.fight.autopilot();
        CHECK_FALSE(idle.light);
        CHECK_FALSE(idle.dodge);
        CHECK(idle.move.x == 0.f);
    }
    {   // Defeat: the player goes down
        const ArenaAttack killer = swing(0.2f, 0.1f, 0.5f, 2.f, 100);
        Rig rig(ruleFighter(), {hitterKind(killer)});
        rig.put(0, 1.5f, 0.f);
        rig.runFor(0.4f);
        CHECK(rig.fight.phase() == ArenaPhase::Defeat);
        CHECK(rig.fight.over());
        CHECK(rig.fight.hp() == 0);
        CHECK(rig.out().player.action == ArenaAction::Downed);
        const Logged* hit = rig.find(ArenaEventType::Hit);
        const Logged* downed = rig.find(ArenaEventType::Downed);
        const Logged* defeat = rig.find(ArenaEventType::Defeat);
        REQUIRE(hit);
        REQUIRE(downed);
        REQUIRE(defeat);
        CHECK(hit->event.amount == 100);
        CHECK(downed->event.target == kArenaPlayer);
        // In the order they happened, within one tick.
        const auto indexOf = [&](const Logged* l) { return static_cast<std::size_t>(l - rig.log.data()); };
        CHECK(indexOf(hit) < indexOf(downed));
        CHECK(indexOf(downed) < indexOf(defeat));
        CHECK(rig.count(ArenaEventType::Victory) == 0);
        const float time = rig.fight.time();
        const std::size_t events = rig.log.size();
        rig.in.light = true;
        rig.in.dodge = true;
        rig.run(30);
        CHECK(rig.fight.time() == time);
        CHECK(rig.log.size() == events);
        CHECK(rig.fight.phase() == ArenaPhase::Defeat);
        CHECK(rig.out().player.action == ArenaAction::Downed);
        // A retry puts it all back.
        rig.fight.restart();
        CHECK(rig.fight.phase() == ArenaPhase::Fighting);
        CHECK(rig.fight.hp() == 100);
        CHECK(rig.fight.time() == 0.f);
    }
}

TEST_CASE("arena enemy tokens: at most `attackers` at once, the longest waiting then the nearest") {
    const ArenaAttack slow = swing(0.3f, 0.1f, 0.3f, 2.f, 0); // 0.7 s, harmless
    const auto heldNow = [](const Rig& rig, std::size_t count) {
        int held = 0;
        for (std::size_t e = 0; e < count; ++e)
            if (rig.out().enemies[e].token) ++held;
        return held;
    };
    {   // equal waiting: the nearest
        Rig rig(ruleFighter(), {hitterKind(slow), hitterKind(slow)});
        rig.put(0, 2.2f, 0.f);
        rig.put(1, 1.5f, 0.f);
        rig.tick();
        const Logged* first = rig.find(ArenaEventType::AttackStarted);
        REQUIRE(first);
        CHECK(first->event.source == 1);
        CHECK(rig.count(ArenaEventType::AttackStarted) == 1);
    }
    {   // the longest waiting gets it when it comes back, though another is nearer
        ArenaEnemyKind a = hitterKind(slow);
        ArenaEnemyKind b = hitterKind(slow);
        b.attackDelay = 0.3f;
        ArenaEnemyKind c = hitterKind(slow);
        c.attackDelay = 0.5f;
        Rig rig(ruleFighter(), {a, b, c});
        rig.put(0, 1.5f, 0.f);
        rig.put(1, 2.4f, 0.f);
        rig.put(2, 0.f, 1.5f); // the nearest
        int most = 0;
        for (int i = 0; i < 300; ++i) {
            rig.tick();
            most = std::max(most, heldNow(rig, 3));
        }
        CHECK(most == 1);
        std::vector<int> order;
        for (const Logged& l : rig.log)
            if (l.event.type == ArenaEventType::AttackStarted) order.push_back(l.event.source);
        REQUIRE(order.size() >= 4);
        CHECK(order[0] == 0);
        CHECK(order[1] == 1);
        CHECK(order[2] == 2);
        CHECK(order[3] == 0);
    }
    {   // two tokens, two at once
        Rig rig(ruleFighter(), {hitterKind(slow), hitterKind(slow), hitterKind(slow)}, 2);
        rig.put(0, 1.5f, 0.f);
        rig.put(1, 0.f, 1.5f);
        rig.put(2, -1.5f, 0.f);
        rig.tick();
        CHECK(rig.count(ArenaEventType::AttackStarted) == 2);
        int most = 0;
        for (int i = 0; i < 300; ++i) {
            rig.tick();
            most = std::max(most, heldNow(rig, 3));
        }
        CHECK(most == 2);
    }
    {   // kept through a whole string: nobody else attacks until it ends
        ArenaAttack leg = swing(0.2f, 0.1f, 0.2f, 2.f, 0);
        ArenaEnemyKind a = hitterKind(leg);
        a.moveCount = 2;
        a.moves[0].chain = 1;
        a.moves[1] = a.moves[0];
        a.moves[1].chain = -1;
        a.moves[1].weight = 0.f;
        ArenaEnemyKind b = hitterKind(slow);
        b.attackDelay = 0.1f;
        Rig rig(ruleFighter(), {a, b});
        rig.put(0, 1.5f, 0.f);
        rig.put(1, 0.f, 1.5f);
        bool kept = true;
        for (int i = 0; i < 56; ++i) { // a's string runs for 1 s
            rig.tick();
            kept = kept && rig.out().enemies[0].token && !rig.out().enemies[1].token;
        }
        CHECK(kept);
        rig.run(12);
        REQUIRE(rig.count(ArenaEventType::AttackStarted) >= 3);
        CHECK(rig.find(ArenaEventType::AttackStarted, 0)->event.source == 0);
        CHECK(rig.find(ArenaEventType::AttackStarted, 1)->event.source == 0);
        CHECK(rig.find(ArenaEventType::AttackStarted, 1)->event.move == 1);
        CHECK(rig.find(ArenaEventType::AttackStarted, 2)->event.source == 1);
    }
    {   // returned on a stagger and on a death: the one waiting gets it at once
        ArenaFighter f = ruleFighter();
        f.light[0].poiseDamage = 40.f;
        for (int death = 0; death < 2; ++death) {
            ArenaEnemyKind a = hitterKind(swing(1.f, 0.1f, 0.5f, 2.f, 0));
            if (death) a.hp = 5;
            ArenaEnemyKind b = hitterKind(slow);
            b.attackDelay = 0.2f;
            Rig rig(f, {a, b});
            rig.put(0, 1.5f, 0.f);
            rig.put(1, 3.f, 0.f); // out of the player's reach
            rig.run(18);
            CHECK(rig.out().enemies[0].token);
            rig.in.light = true;
            rig.run(20);
            CHECK(rig.count(death ? ArenaEventType::Downed : ArenaEventType::Staggered) == 1);
            CHECK_FALSE(rig.out().enemies[0].token);
            CHECK(rig.out().enemies[1].token);
        }
    }
    {   // returned on a parry
        const ArenaAttack bite = swing(0.4f, 0.15f, 0.5f, 2.f, 20);
        ArenaEnemyKind b = hitterKind(slow);
        b.attackDelay = 0.2f;
        Rig rig(ruleFighter(), {hitterKind(bite), b});
        rig.put(0, 1.5f, 0.f);
        rig.put(1, 3.f, 0.f);
        rig.run(18);
        CHECK(rig.out().enemies[0].token);
        rig.in.guard = true;
        rig.run(14);
        CHECK(rig.count(ArenaEventType::Parried) == 1);
        CHECK_FALSE(rig.out().enemies[0].token);
        CHECK(rig.out().enemies[1].token);
    }
}

TEST_CASE("arena enemy moves: chosen by range, weight and cooldown") {
    const ArenaAttack harmless = swing(0.3f, 0.1f, 0.3f, 2.f, 0);
    // Two moves: move 0 within [0, 2.5], move 1 within [3.5, 6].
    const auto twoBands = [&](float weight0, float weight1) {
        ArenaEnemyKind k = hitterKind(harmless, 2.5f);
        k.moveCount = 2;
        k.moves[1] = k.moves[0];
        k.moves[1].minRange = 3.5f;
        k.moves[1].maxRange = 6.f;
        k.moves[0].weight = weight0;
        k.moves[1].weight = weight1;
        k.preferredRange = 5.f;
        return k;
    };
    const auto moves = [](const Rig& rig) {
        std::vector<int> picked;
        for (const Logged& l : rig.log)
            if (l.event.type == ArenaEventType::AttackStarted) picked.push_back(l.event.move);
        return picked;
    };
    for (const float distance : {2.f, 4.5f}) {   // the player's distance picks the band
        Rig rig(ruleFighter(), {twoBands(1.f, 1.f)});
        rig.put(0, distance, 0.f);
        rig.runFor(10.f);
        const std::vector<int> picked = moves(rig);
        REQUIRE(picked.size() >= 8);
        for (const int m : picked) CHECK(m == (distance < 3.f ? 0 : 1));
    }
    {   // both fit: weighted, 1 to 4
        Rig rig(ruleFighter(), {[&] {
                    ArenaEnemyKind k = twoBands(1.f, 4.f);
                    k.moves[1].minRange = 0.f;
                    k.moves[1].maxRange = 3.f;
                    k.moves[0].maxRange = 3.f;
                    return k;
                }()});
        rig.put(0, 2.f, 0.f);
        rig.runFor(60.f);
        const std::vector<int> picked = moves(rig);
        int zero = 0, one = 0;
        for (const int m : picked) (m == 0 ? zero : one) += 1;
        CHECK(zero > 0);
        CHECK(one > 2 * zero);
    }
    {   // a cooldown keeps a move from being picked again too soon
        ArenaEnemyKind k = twoBands(10.f, 1.f);
        k.moves[0].maxRange = 3.f;
        k.moves[1].minRange = 0.f;
        k.moves[1].maxRange = 3.f;
        k.moves[0].cooldown = 2.f;
        Rig rig(ruleFighter(), {k});
        rig.put(0, 2.f, 0.f);
        rig.runFor(40.f);
        std::vector<int> ticksOfZero;
        int ones = 0;
        for (const Logged& l : rig.log) {
            if (l.event.type != ArenaEventType::AttackStarted) continue;
            if (l.event.move == 0) ticksOfZero.push_back(l.tick);
            else ++ones;
        }
        REQUIRE(ticksOfZero.size() >= 5);
        for (std::size_t i = 1; i < ticksOfZero.size(); ++i) CHECK(ticksOfZero[i] - ticksOfZero[i - 1] >= 118); // 2 s
        CHECK(ones > 0);
    }
}

TEST_CASE("arena enemy moves: with none fitting, the enemy walks in to the nearest move's range") {
    ArenaEnemyKind k = hitterKind(swing(0.3f, 0.1f, 0.3f, 2.f, 0), 2.5f);
    Rig rig(ruleFighter(), {k});
    rig.bodies.enemies[0] = Vec2{6.f, 0.f};
    bool approached = false;
    float distanceAtAttack = 0.f;
    for (int i = 0; i < 600 && rig.count(ArenaEventType::AttackStarted) == 0; ++i) {
        const ArenaOutput& out = rig.tick();
        arenaStep(rig.fight, rig.bodies, kDt);
        if (out.enemies[0].brain == ArenaBrain::Approach && out.enemies[0].velocity.x < -1.f) approached = true;
        if (rig.count(ArenaEventType::AttackStarted) == 1 && distanceAtAttack == 0.f)
            distanceAtAttack = distanceOf(rig.bodies.enemies[0], rig.bodies.player);
    }
    CHECK(approached);
    REQUIRE(rig.count(ArenaEventType::AttackStarted) == 1);
    CHECK(distanceAtAttack <= 2.51f);
    CHECK(distanceAtAttack >= 2.4f); // it stopped at the move's maxRange
    // An enemy with every move cooling down waits where it is, holding the token.
    ArenaEnemyKind slowBoth = hitterKind(swing(0.3f, 0.1f, 0.3f, 2.f, 0), 3.f);
    slowBoth.moves[0].cooldown = 5.f;
    Rig wait(ruleFighter(), {slowBoth});
    wait.put(0, 2.f, 0.f);
    wait.runFor(3.f);
    CHECK(wait.count(ArenaEventType::AttackStarted) == 1);
    CHECK(wait.out().enemies[0].token); // asks again, picks nothing for 5 s
    CHECK(wait.out().enemies[0].brain == ArenaBrain::Strafe);
}

TEST_CASE("arena armor: a share of the poise damage is ignored during an attack's startup and active parts") {
    ArenaFighter f = ruleFighter();
    for (ArenaAttack& l : f.light) l.poiseDamage = 40.f;
    ArenaEnemyKind k = hitterKind(swing(1.f, 0.1f, 0.6f, 2.f, 0)); // startup 1 s, active to 1.1 s, recovery to 1.7 s
    k.poise = 30.f;
    k.poiseDelay = 5.f;
    k.moves[0].attack.armor = 0.5f;
    Rig rig(f, {k});
    rig.put(0, 1.5f, 0.f);
    rig.run(18);
    rig.in.light = true;
    rig.tick(); // pressed at 0.3 s: it connects at about 0.5 s, in the enemy's startup
    rig.run(25);
    CHECK(rig.count(ArenaEventType::Staggered) == 0);
    CHECK(rig.fight.enemyPoise(0) == doctest::Approx(10.f)); // 40 * (1 - 0.5) lost
    rig.runFor(0.5f);
    rig.in.light = true;
    rig.tick(); // at about 1.23 s: it connects in the recovery, with no armor
    rig.run(30);
    CHECK(rig.count(ArenaEventType::Staggered) == 1);
    CHECK(rig.out().enemies[0].brain == ArenaBrain::Staggered);
    // The player's heavy has armor too: it is not staggered by a hit that would break its poise bare.
    ArenaAttack big = swing(0.2f, 0.1f, 0.5f, 2.f, 10);
    big.poiseDamage = 40.f;
    for (const float armor : {0.f, 0.5f}) {
        ArenaFighter g = ruleFighter();
        g.heavy.armor = armor;
        Rig duel(g, {hitterKind(big)});
        duel.put(0, 1.5f, 0.f);
        duel.in.heavy = true; // startup 0.4 s: the 40-poise hit lands at 0.2 s
        duel.tick();
        duel.run(20);
        CHECK((duel.out().player.action == ArenaAction::Hitstun) == (armor == 0.f));
    }
}

TEST_CASE("arena lock-on: the pick, nearest wins ties, switch left and right") {
    Rig rig(ruleFighter(), {dummyKind(), dummyKind(), dummyKind(), dummyKind()});
    rig.put(0, 5.f, 0.f);   // ahead
    rig.put(1, 4.f, 4.f);   // 45 degrees to the right (the ground plane has y to the right)
    rig.put(2, 4.f, -4.f);  // 45 degrees to the left
    rig.put(3, 30.f, 0.f);  // beyond lockRange
    CHECK(rig.fight.lock() == kArenaNobody);
    rig.in.view = Vec2{1.f, 0.f};
    rig.in.lock = true;
    rig.tick();
    CHECK(rig.fight.lock() == 0);
    CHECK(rig.out().player.lock == 0);
    REQUIRE(rig.count(ArenaEventType::LockChanged) == 1);
    CHECK(rig.find(ArenaEventType::LockChanged)->event.target == 0);
    // Locked, the player faces the target.
    rig.run(3);
    CHECK(rig.out().player.facing.x == doctest::Approx(1.f));
    // The switch goes round: right, right (wrapping), right, then left back.
    const auto flick = [&](int side, int want) {
        rig.in.lockSwitch = static_cast<std::int8_t>(side);
        rig.tick();
        CHECK(rig.fight.lock() == want);
    };
    flick(1, 1);
    flick(1, 2);
    flick(1, 0);
    flick(-1, 2);
    flick(-1, 1);
    flick(-1, 0);
    // Pressing lock again lets go.
    rig.in.lock = true;
    rig.tick();
    CHECK(rig.fight.lock() == kArenaNobody);
    CHECK(rig.find(ArenaEventType::LockChanged, 7) != nullptr);
    // The view decides: looking at the one on the right picks it.
    rig.in.view = Vec2{0.70710678f, 0.70710678f};
    rig.in.lock = true;
    rig.tick();
    CHECK(rig.fight.lock() == 1);
    // A switch with nothing locked does nothing.
    rig.in.lock = true;
    rig.tick();
    CHECK(rig.fight.lock() == kArenaNobody);
    rig.in.lockSwitch = 1;
    rig.tick();
    CHECK(rig.fight.lock() == kArenaNobody);
}

TEST_CASE("arena lock-on: out of range can't be locked, and the nearer of two in line wins") {
    {
        Rig rig(ruleFighter(), {dummyKind()});
        rig.put(0, 30.f, 0.f);
        rig.in.lock = true;
        rig.tick();
        CHECK(rig.fight.lock() == kArenaNobody);
        CHECK(rig.count(ArenaEventType::LockChanged) == 0);
    }
    {   // the same direction: the nearer scores better
        Rig rig(ruleFighter(), {dummyKind(), dummyKind()});
        rig.put(0, 9.f, 0.f);
        rig.put(1, 3.f, 0.f);
        rig.in.lock = true;
        rig.tick();
        CHECK(rig.fight.lock() == 1);
    }
    {   // an exact tie (mirror images): the first
        Rig rig(ruleFighter(), {dummyKind(), dummyKind()});
        rig.put(0, 4.f, -4.f);
        rig.put(1, 4.f, 4.f);
        rig.in.lock = true;
        rig.tick();
        CHECK(rig.fight.lock() == 0);
    }
}

TEST_CASE("arena lock-on: it moves on when the target dies, and breaks beyond lockBreak") {
    {   // the target dies: the nearest other living enemy in range
        ArenaEnemyKind frail = dummyKind();
        frail.hp = 10;
        Rig rig(ruleFighter(), {frail, dummyKind(), dummyKind()});
        rig.put(0, 2.f, 0.f);
        rig.put(1, 4.f, -4.f); // 5.66 m
        rig.put(2, 3.f, 3.f);  // 4.24 m: the nearest of the two
        rig.in.lock = true;
        rig.tick();
        REQUIRE(rig.fight.lock() == 0);
        rig.in.light = true;
        rig.run(20);
        REQUIRE_FALSE(rig.fight.enemyAlive(0));
        CHECK(rig.fight.lock() == 2);
        REQUIRE(rig.count(ArenaEventType::LockChanged) == 2);
        CHECK(rig.find(ArenaEventType::LockChanged, 1)->event.target == 2);
    }
    {   // none in range: it lets go
        ArenaEnemyKind frail = dummyKind();
        frail.hp = 10;
        Rig rig(ruleFighter(), {frail, dummyKind()});
        rig.put(0, 2.f, 0.f);
        rig.put(1, 30.f, 0.f);
        rig.in.lock = true;
        rig.tick();
        REQUIRE(rig.fight.lock() == 0);
        rig.in.light = true;
        rig.run(20);
        REQUIRE_FALSE(rig.fight.enemyAlive(0));
        CHECK(rig.fight.lock() == kArenaNobody);
        CHECK(rig.find(ArenaEventType::LockChanged, 1)->event.target == kArenaNobody);
    }
    {   // lockRange is 15 and lockBreak 20: between them the lock holds
        Rig rig(ruleFighter(), {dummyKind()});
        rig.put(0, 5.f, 0.f);
        rig.in.lock = true;
        rig.tick();
        REQUIRE(rig.fight.lock() == 0);
        rig.put(0, 18.f, 0.f);
        rig.run(5);
        CHECK(rig.fight.lock() == 0);
        rig.put(0, 25.f, 0.f);
        rig.tick();
        CHECK(rig.fight.lock() == kArenaNobody);
        CHECK(rig.find(ArenaEventType::LockChanged, 1)->event.target == kArenaNobody);
        // and it doesn't come back by itself
        rig.put(0, 5.f, 0.f);
        rig.run(5);
        CHECK(rig.fight.lock() == kArenaNobody);
    }
    {   // a locked player strafes: the stick moves it, the facing stays on the target
        Rig rig(ruleFighter(), {dummyKind()});
        rig.put(0, 5.f, 0.f);
        rig.in.lock = true;
        rig.in.move = Vec2{0.f, 1.f};
        rig.tick();
        rig.run(10);
        CHECK(rig.out().player.velocity.y == doctest::Approx(4.f));
        CHECK(rig.out().player.facing.x == doctest::Approx(1.f));
    }
}

TEST_CASE("arena ring: no body leaves it, and a roll along the edge slides") {
    const ArenaRing ring{Vec2{0.f, 0.f}, 6.f}; // bodies may reach 5.6 m (the player) from the middle
    {   // pushing outward all the time
        Rig rig(ruleFighter(), {dummyKind()}, 1, ring);
        rig.put(0, 20.f, 0.f);
        rig.in.move = Vec2{1.f, 0.f};
        float farthest = 0.f;
        for (int i = 0; i < 240; ++i) {
            const ArenaOutput& out = rig.tick();
            rig.bodies.player.x += out.player.velocity.x * kDt; // no arenaStep: only Core's own cut
            rig.bodies.player.y += out.player.velocity.y * kDt;
            farthest = std::max(farthest, distanceOf(rig.bodies.player, ring.centre));
        }
        CHECK(farthest <= 5.6f + 1e-3f);
        CHECK(farthest >= 5.5f); // and it did reach the edge
    }
    {   // a roll along the edge slides
        Rig rig(ruleFighter(), {dummyKind()}, 1, ring);
        rig.put(0, -20.f, 0.f);
        rig.bodies.player = Vec2{5.5f, 0.f};
        rig.in.move = Vec2{0.f, 1.f};
        rig.in.dodge = true;
        float farthest = 0.f;
        for (int i = 0; i < 50; ++i) {
            const ArenaOutput& out = rig.tick();
            rig.bodies.player.x += out.player.velocity.x * kDt;
            rig.bodies.player.y += out.player.velocity.y * kDt;
            farthest = std::max(farthest, distanceOf(rig.bodies.player, ring.centre));
        }
        CHECK(farthest <= 5.6f + 1e-3f);
        CHECK(rig.bodies.player.y > 2.5f); // it went most of the way round
    }
    {   // a roll straight at the edge goes nowhere
        Rig rig(ruleFighter(), {dummyKind()}, 1, ring);
        rig.put(0, -20.f, 0.f);
        rig.bodies.player = Vec2{5.5f, 0.f};
        rig.in.move = Vec2{1.f, 0.f};
        rig.in.dodge = true;
        for (int i = 0; i < 50; ++i) {
            const ArenaOutput& out = rig.tick();
            rig.bodies.player.x += out.player.velocity.x * kDt;
            rig.bodies.player.y += out.player.velocity.y * kDt;
        }
        CHECK(distanceOf(rig.bodies.player, ring.centre) <= 5.6f + 1e-3f);
        CHECK(rig.bodies.player.x <= 5.6f + 1e-3f);
        CHECK(std::fabs(rig.bodies.player.y) < 0.2f);
    }
    {   // a whole fight in a small ring, the bodies moved by hand: nobody leaves
        ArenaDef def = defaultArenaDef();
        def.enemyCount = 2;
        def.enemies[1] = def.enemies[0];
        def.attackers = 2;
        def.ringRadius = 6.f;
        def.spawnDistance = 5.f;
        ArenaFight fight;
        fight.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 3);
        ArenaBodies bodies;
        bodies.player = fight.playerStart();
        for (std::size_t i = 0; i < 2; ++i) bodies.enemies[i] = fight.enemyStart(i);
        float worstPlayer = 0.f, worstEnemy = 0.f;
        for (int i = 0; i < 3000 && !fight.over(); ++i) {
            const ArenaOutput& out = fight.tick(kDt, fight.autopilot(), bodies);
            bodies.player.x += out.player.velocity.x * kDt;
            bodies.player.y += out.player.velocity.y * kDt;
            worstPlayer = std::max(worstPlayer, distanceOf(bodies.player, ring.centre) - (ring.radius - def.fighter.bodyRadius));
            for (std::size_t e = 0; e < 2; ++e) {
                bodies.enemies[e].x += out.enemies[e].velocity.x * kDt;
                bodies.enemies[e].y += out.enemies[e].velocity.y * kDt;
                worstEnemy = std::max(worstEnemy, distanceOf(bodies.enemies[e], ring.centre) - (ring.radius - def.enemies[e].bodyRadius));
            }
        }
        CHECK(worstPlayer <= 1e-3f);
        CHECK(worstEnemy <= 1e-3f);
    }
}

TEST_CASE("arena step: bodies move at their velocities, overlaps are pushed apart, the ring holds") {
    const ArenaRing ring{Vec2{0.f, 0.f}, 10.f};
    ArenaDef def = defaultArenaDef();
    def.enemyCount = 2;
    def.enemies[1] = def.enemies[0];
    def.spawnDistance = 4.f;
    ArenaFight fight;
    fight.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
    ArenaBodies bodies;
    bodies.player = Vec2{0.f, 0.f};
    bodies.enemies[0] = Vec2{0.3f, 0.f}; // on top of the player
    bodies.enemies[1] = Vec2{0.3f, 0.1f}; // and on top of the other enemy
    ArenaBodies before = bodies;
    arenaStep(fight, bodies, kDt);
    CHECK(distanceOf(bodies.player, bodies.enemies[0]) >= 0.4f + 0.45f - 0.2f);
    CHECK(distanceOf(bodies.enemies[0], bodies.enemies[1]) > distanceOf(before.enemies[0], before.enemies[1]));
    // The ring holds everyone.
    bodies.player = Vec2{50.f, 0.f};
    bodies.enemies[0] = Vec2{0.f, -50.f};
    bodies.enemies[1] = Vec2{-50.f, 0.f};
    arenaStep(fight, bodies, kDt);
    CHECK(distanceOf(bodies.player, ring.centre) <= 10.f - 0.4f + 1e-4f);
    CHECK(distanceOf(bodies.enemies[0], ring.centre) <= 10.f - 0.45f + 1e-4f);
    CHECK(distanceOf(bodies.enemies[1], ring.centre) <= 10.f - 0.45f + 1e-4f);
    // Bodies move at their output velocities.
    Rig mover(ruleFighter(), {dummyKind()});
    mover.put(0, 20.f, 0.f);
    mover.in.move = Vec2{1.f, 0.f};
    mover.tick();
    const Vec2 moverStart = mover.bodies.player;
    const Vec2 foeStart = mover.bodies.enemies[0];
    arenaStep(mover.fight, mover.bodies, kDt);
    CHECK(mover.bodies.player.x - moverStart.x == doctest::Approx(4.f * kDt));
    CHECK(mover.bodies.player.y == moverStart.y);
    CHECK(mover.bodies.enemies[0].x - foeStart.x == doctest::Approx(mover.out().enemies[0].velocity.x * kDt));
    // A dead enemy doesn't block.
    ArenaEnemyKind weak = dummyKind();
    weak.hp = 5;
    Rig rig(ruleFighter(), {weak});
    rig.put(0, 1.5f, 0.f);
    rig.in.light = true;
    rig.run(20);
    REQUIRE_FALSE(rig.fight.enemyAlive(0));
    ArenaBodies corpse;
    corpse.player = Vec2{1.f, 1.f};
    corpse.enemies[0] = Vec2{1.f, 1.f};
    arenaStep(rig.fight, corpse, kDt);
    CHECK(corpse.player.x == 1.f);
    CHECK(corpse.player.y == 1.f);
}

// ---- begin, restart, refresh ----

TEST_CASE("arena begin: the enemies spread on an arc in front, inside the ring, facing the player") {
    ArenaDef def = defaultArenaDef();
    def.enemyCount = 3;
    def.enemies[1] = def.enemies[0];
    def.enemies[2] = def.enemies[0];
    def.spawnDistance = 7.f;
    const ArenaRing ring{Vec2{4.2f, 0.f}, 12.f};
    ArenaFight fight;
    CHECK(fight.phase() == ArenaPhase::NotStarted);
    fight.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 5);
    CHECK(fight.phase() == ArenaPhase::Fighting);
    CHECK(fight.enemyCount() == 3);
    CHECK(fight.hp() == def.fighter.hp);
    CHECK(fight.stamina() == doctest::Approx(100.f));
    CHECK(fight.poise() == doctest::Approx(25.f));
    CHECK(fight.healCharges() == 3);
    CHECK(fight.lock() == kArenaNobody);
    CHECK(fight.time() == 0.f);
    CHECK(fight.playerStart().x == 0.f);
    CHECK(fight.playerStartFacing().x == doctest::Approx(1.f));
    for (std::size_t i = 0; i < 3; ++i) {
        const Vec2 at = fight.enemyStart(i);
        CHECK(distanceOf(at, Vec2{0.f, 0.f}) == doctest::Approx(7.f).epsilon(0.01));
        CHECK(at.x > 0.f);
        CHECK(ring.contains(at, def.enemies[i].bodyRadius));
        const Vec2 face = fight.enemyStartFacing(i);
        CHECK(std::hypot(face.x, face.y) == doctest::Approx(1.f));
        CHECK(face.x * -at.x + face.y * -at.y == doctest::Approx(distanceOf(at, Vec2{0.f, 0.f})).epsilon(0.001));
        CHECK(fight.enemyHp(i) == def.enemies[i].hp);
        CHECK(fight.enemyAlive(i));
        for (std::size_t j = i + 1; j < 3; ++j) CHECK(distanceOf(at, fight.enemyStart(j)) >= 1.5f);
    }
    CHECK_FALSE(fight.enemyAlive(3));
    CHECK(fight.output().enemies[0].brain == ArenaBrain::Approach);
    CHECK(fight.output().enemies[3].brain == ArenaBrain::Dead);
    // A ring too small to hold the arc pulls the enemies in.
    const ArenaRing cramped{Vec2{0.f, 0.f}, 5.f};
    fight.begin(def, cramped, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 5);
    for (std::size_t i = 0; i < 3; ++i) CHECK(cramped.contains(fight.enemyStart(i), def.enemies[i].bodyRadius - 1e-3f));
    // The facing may be any length, and any way.
    fight.begin(def, ring, Vec2{0.f, 0.f}, Vec2{0.f, -5.f}, 5);
    CHECK(fight.playerStartFacing().y == doctest::Approx(-1.f));
    CHECK(fight.enemyStart(1).y < -6.f);
    // Capacities hold: a def that says too much is held to them.
    ArenaDef wild = def;
    wild.enemyCount = 40;
    wild.attackers = 0;
    wild.fighter.lightCount = 0;
    fight.begin(wild, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 5);
    CHECK(fight.enemyCount() == kMaxArenaEnemies);
    CHECK(fight.def().attackers == 1);
    CHECK(fight.def().fighter.lightCount == 1);
}

TEST_CASE("arena restart: back to the state begin left, the stats keep counting") {
    Rig rig(ruleFighter(), {hitterKind(swing(0.2f, 0.1f, 0.5f, 2.f, 40))});
    rig.put(0, 1.5f, 0.f);
    rig.in.light = true;
    rig.in.guard = false;
    rig.runFor(1.f);
    CHECK(rig.fight.stats().hitsTaken >= 1);
    CHECK(rig.fight.hp() < 100);
    rig.in.lock = true;
    rig.tick();
    const ArenaStats stats = rig.fight.stats();
    rig.fight.restart();
    CHECK(rig.fight.phase() == ArenaPhase::Fighting);
    CHECK(rig.fight.hp() == 100);
    CHECK(rig.fight.stamina() == doctest::Approx(100.f));
    CHECK(rig.fight.healCharges() == 2);
    CHECK(rig.fight.lock() == kArenaNobody);
    CHECK(rig.fight.time() == 0.f);
    CHECK(rig.fight.enemyHp(0) == 100);
    CHECK(rig.fight.output().player.action == ArenaAction::Idle);
    CHECK(rig.fight.output().events.empty());
    CHECK(rig.fight.stats().restarts == 1);
    CHECK(rig.fight.stats().hitsTaken == stats.hitsTaken); // not reset
    CHECK(rig.fight.stats().time >= stats.time);
    rig.fight.restart();
    CHECK(rig.fight.stats().restarts == 2);
    // begin() starts the stats over.
    rig.fight.begin(rig.def, ArenaRing{Vec2{0.f, 0.f}, 30.f}, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
    CHECK(rig.fight.stats().restarts == 0);
    CHECK(rig.fight.stats().time == 0.f);
}

TEST_CASE("arena tick: dt <= 0 only refreshes the output") {
    Rig rig(ruleFighter(), {dummyKind()});
    rig.put(0, 1.5f, 0.f);
    rig.in.move = Vec2{1.f, 0.f};
    rig.run(10);
    const float time = rig.fight.time();
    const Vec2 velocity = rig.out().player.velocity;
    rig.in.light = true;
    for (const float dt : {0.f, -1.f}) {
        const ArenaOutput& out = rig.tick(dt);
        CHECK(rig.fight.time() == time);
        CHECK(out.events.empty());
        CHECK(out.player.action == ArenaAction::Move);
        CHECK(out.player.velocity.x == velocity.x);
        rig.in.light = true;
    }
    CHECK(rig.count(ArenaEventType::AttackStarted) == 0); // the presses were not taken
    rig.tick();
    CHECK(rig.count(ArenaEventType::AttackStarted) == 1);
}

TEST_CASE("arena names and the default def") {
    const ArenaAction actions[] = {ArenaAction::Idle,    ArenaAction::Move,    ArenaAction::Light,
                                   ArenaAction::Heavy,   ArenaAction::Riposte, ArenaAction::Dodge,
                                   ArenaAction::Guard,   ArenaAction::Heal,    ArenaAction::Hitstun,
                                   ArenaAction::GuardBreak, ArenaAction::Downed};
    for (const ArenaAction a : actions) CHECK(std::string(arenaActionName(a)).size() > 2);
    CHECK(std::string(arenaActionName(ArenaAction::Idle)) == "Idle");
    CHECK(std::string(arenaActionName(ArenaAction::GuardBreak)) == "GuardBreak");
    const ArenaBrain brains[] = {ArenaBrain::Approach, ArenaBrain::Strafe, ArenaBrain::Attack, ArenaBrain::Staggered,
                                 ArenaBrain::Open,     ArenaBrain::Down,   ArenaBrain::Dead};
    for (const ArenaBrain b : brains) CHECK(std::string(arenaBrainName(b)).size() > 2);
    CHECK(std::string(arenaBrainName(ArenaBrain::Strafe)) == "Strafe");
    const ArenaDef def = defaultArenaDef();
    CHECK(def.enemyCount == 1);
    CHECK(def.enemies[0].moveCount >= 2);
    CHECK(def.fighter.lightCount >= 1);
    CHECK(def.enemies[0].moves[0].attack.startup >= 0.35f);
    // A quick strike and a string of two.
    bool string = false;
    for (std::size_t m = 0; m < def.enemies[0].moveCount; ++m) string = string || def.enemies[0].moves[m].chain >= 0;
    CHECK(string);
}

// ---- determinism ----

namespace {

struct Trace {
    std::vector<ArenaEvent> events;
    std::vector<int> hp;
    std::vector<Vec2> where;
    ArenaPhase phase = ArenaPhase::NotStarted;
};

// The autopilot plays `fight` (with arenaStep) for up to `ticks` ticks.
Trace playAutopilot(ArenaFight& fight, int ticks) {
    Trace t;
    ArenaBodies bodies;
    bodies.player = fight.playerStart();
    for (std::size_t i = 0; i < fight.enemyCount(); ++i) bodies.enemies[i] = fight.enemyStart(i);
    for (int i = 0; i < ticks && !fight.over(); ++i) {
        const ArenaOutput& out = fight.tick(kDt, fight.autopilot(), bodies);
        arenaStep(fight, bodies, kDt);
        for (const ArenaEvent& e : out.events) t.events.push_back(e);
        t.hp.push_back(fight.hp());
        t.where.push_back(bodies.player);
    }
    t.phase = fight.phase();
    return t;
}

bool sameEvent(const ArenaEvent& a, const ArenaEvent& b) {
    return a.type == b.type && a.source == b.source && a.target == b.target && a.move == b.move && a.amount == b.amount &&
           a.at.x == b.at.x && a.at.y == b.at.y;
}

bool sameTrace(const Trace& a, const Trace& b) {
    if (a.events.size() != b.events.size() || a.hp != b.hp || a.where.size() != b.where.size() || a.phase != b.phase)
        return false;
    for (std::size_t i = 0; i < a.events.size(); ++i)
        if (!sameEvent(a.events[i], b.events[i])) return false;
    for (std::size_t i = 0; i < a.where.size(); ++i)
        if (a.where[i].x != b.where[i].x || a.where[i].y != b.where[i].y) return false;
    return true;
}

ArenaDef pairDef() {
    ArenaDef def = defaultArenaDef();
    def.enemyCount = 2;
    def.enemies[1] = def.enemies[0];
    def.attackers = 1;
    return def;
}

} // namespace

TEST_CASE("arena determinism: the same seed, inputs and dts give the same fight, and restart() replays it") {
    const ArenaDef def = pairDef();
    const ArenaRing ring = arenaRingAt(Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, def.ringRadius, ArenaRing{Vec2{0.f, 0.f}, 36.f});
    ArenaFight a, b;
    a.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 77);
    b.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 77);
    const Trace first = playAutopilot(a, 2400);
    const Trace second = playAutopilot(b, 2400);
    CHECK(first.events.size() > 20);
    CHECK(sameTrace(first, second));
    CHECK(a.stats().hits == b.stats().hits);
    CHECK(a.stats().damageTaken == b.stats().damageTaken);
    CHECK(a.stats().time == b.stats().time);
    CHECK(a.time() == b.time());
    // restart() replays the first run exactly.
    a.restart();
    CHECK(a.stats().restarts == 1);
    const Trace replay = playAutopilot(a, 2400);
    CHECK(sameTrace(first, replay));
    // A copy of a fight in the middle of it carries on the same way (the numbers it draws are part of its state).
    ArenaFight c;
    c.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 77);
    ArenaBodies bodies;
    bodies.player = c.playerStart();
    for (std::size_t i = 0; i < 2; ++i) bodies.enemies[i] = c.enemyStart(i);
    for (int i = 0; i < 300; ++i) {
        c.tick(kDt, c.autopilot(), bodies);
        arenaStep(c, bodies, kDt);
    }
    ArenaFight d = c;
    ArenaBodies bodiesD = bodies;
    bool same = true;
    for (int i = 0; i < 600 && same; ++i) {
        const ArenaOutput& oc = c.tick(kDt, c.autopilot(), bodies);
        const ArenaOutput& od = d.tick(kDt, d.autopilot(), bodiesD);
        arenaStep(c, bodies, kDt);
        arenaStep(d, bodiesD, kDt);
        same = oc.events.size() == od.events.size() && bodies.player.x == bodiesD.player.x &&
               bodies.enemies[0].x == bodiesD.enemies[0].x && bodies.enemies[1].y == bodiesD.enemies[1].y;
    }
    CHECK(same);
    // Different seeds are different fights (the strafe and the move picks come from the seed).
    ArenaFight e;
    e.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 78);
    const Trace other = playAutopilot(e, 2400);
    CHECK_FALSE(sameTrace(first, other));
}

// ---- balance ----

namespace {

struct Outcome {
    ArenaPhase phase = ArenaPhase::NotStarted;
    float time = 0.f;
    int hp = 0;
    int heals = 0;
    int parries = 0;
    int evades = 0;
    int hitsTaken = 0;
    int blocks = 0;
    int ripostes = 0;
    int guardBreaks = 0;
    int damageTaken = 0;
};

// Plays `def` at a fixed 1/60 s with arenaStep, the autopilot or a player who presses nothing.
Outcome playOut(const ArenaDef& def, std::uint32_t seed, bool autopilot) {
    const ArenaRing ring = arenaRingAt(Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, def.ringRadius, ArenaRing{Vec2{0.f, 0.f}, 36.f});
    ArenaFight fight;
    fight.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, seed);
    ArenaBodies bodies;
    bodies.player = fight.playerStart();
    for (std::size_t i = 0; i < fight.enemyCount(); ++i) bodies.enemies[i] = fight.enemyStart(i);
    while (!fight.over() && fight.time() < 120.f) {
        const ArenaInput in = autopilot ? fight.autopilot() : ArenaInput{};
        fight.tick(kDt, in, bodies);
        arenaStep(fight, bodies, kDt);
    }
    Outcome o;
    o.phase = fight.phase();
    o.time = fight.time();
    o.hp = fight.hp();
    o.heals = fight.stats().heals;
    o.parries = fight.stats().parries;
    o.evades = fight.stats().evades;
    o.hitsTaken = fight.stats().hitsTaken;
    o.blocks = fight.stats().blocks;
    o.ripostes = fight.stats().ripostes;
    o.guardBreaks = fight.stats().guardBreaks;
    o.damageTaken = fight.stats().damageTaken;
    return o;
}

// Each fight is played with several seeds: the autopilot has to win every one, and doing nothing has to lose.
void checkBalance(const std::string& name, const ArenaDef& def) {
    float worstWin = 0.f, worstLoss = 0.f, firstWin = 0.f, firstLoss = 0.f;
    int lowestHp = 1000;
    for (std::uint32_t seed = 1; seed <= 8; ++seed) {
        const Outcome win = playOut(def, seed, true);
        const Outcome lose = playOut(def, seed, false);
        CHECK_MESSAGE(win.phase == ArenaPhase::Victory,
                      "Error: autopilot did not win " << name << " seed " << seed << " in 120 s (hp " << win.hp << ", time " << win.time
                                                      << ", hits taken " << win.hitsTaken << ", blocks " << win.blocks << ", parries "
                                                      << win.parries << ", evades " << win.evades << ", ripostes " << win.ripostes
                                                      << ", heals " << win.heals << ", guard breaks " << win.guardBreaks << ")");
        CHECK_MESSAGE(win.time <= 120.f, "Error: autopilot too slow in " << name << " seed " << seed);
        CHECK_MESSAGE(lose.phase == ArenaPhase::Defeat, "Error: a player pressing nothing survived " << name << " seed " << seed);
        CHECK_MESSAGE(lose.time <= 120.f, "Error: idle loss too slow in " << name << " seed " << seed);
        if (seed == 1) {
            firstWin = win.time;
            firstLoss = lose.time;
        }
        worstWin = std::max(worstWin, win.time);
        worstLoss = std::max(worstLoss, lose.time);
        lowestHp = std::min(lowestHp, win.hp);
        if (seed == 1)
            MESSAGE("arena balance: " << name << " seed 1: autopilot won in " << win.time << " s with " << win.hp
                                                  << " hp (heals " << win.heals << ", parries " << win.parries << ", evades "
                                                  << win.evades << ", hits taken " << win.hitsTaken << "), idle lost in " << lose.time
                                                  << " s");
    }
    MESSAGE("arena balance: " << name << " over 8 seeds: slowest win " << worstWin << " s, lowest hp left " << lowestHp
                                         << ", slowest idle loss " << worstLoss << " s (seed 1: " << firstWin << " / " << firstLoss << ")");
}

// A fight of the shipped enemies, by id.
ArenaDef shippedFight(const ArenaTable& table, std::initializer_list<const char*> ids, std::uint8_t attackers) {
    ArenaDef def;
    def.fighter = table.fighter("default");
    def.enemyCount = 0;
    for (const char* id : ids)
        for (std::size_t e = 0; e < table.enemyIds.size(); ++e)
            if (table.enemyIds[e] == id) def.enemies[def.enemyCount++] = table.enemies[e];
    def.attackers = attackers;
    def.ringRadius = 12.f;
    def.spawnDistance = 7.f;
    REQUIRE(def.enemyCount == ids.size());
    return def;
}

} // namespace

TEST_CASE("arena balance: the built-in fight") { checkBalance("default def", defaultArenaDef()); }

TEST_CASE("arena balance: every fight in Data/Battle/arena.json") {
    const ArenaTable table = loadShipped();
    checkBalance("shipped default fight", table.fallback);
    for (const ArenaFightEntry& e : table.entries) checkBalance(e.scene, e.def);
}

TEST_CASE("arena balance: three enemies, one attacking at a time") {
    const ArenaTable table = loadShipped();
    checkBalance("three kinds, attackers 1", shippedFight(table, {"brute", "skirmisher", "breaker"}, 1));
    checkBalance("two brutes, attackers 2", shippedFight(table, {"brute", "brute"}, 2));
    checkBalance("three skirmishers, attackers 1", shippedFight(table, {"skirmisher", "skirmisher", "skirmisher"}, 1));
}

TEST_CASE("arena balance: the shipped enemies each lose to the autopilot alone") {
    const ArenaTable table = loadShipped();
    for (std::size_t e = 0; e < table.enemyIds.size(); ++e) {
        ArenaDef def;
        def.fighter = table.fighter("default");
        def.enemyCount = 1;
        def.enemies[0] = table.enemies[e];
        def.attackers = 1;
        checkBalance(table.enemyIds[e], def);
    }
}

// ---- allocation ----

TEST_CASE("arena: tick, autopilot and arenaStep allocate nothing over a whole fight") {
    const ArenaDef def = pairDef();
    const ArenaRing ring = arenaRingAt(Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, def.ringRadius, ArenaRing{Vec2{0.f, 0.f}, 36.f});
    ArenaFight fight;
    ArenaBodies bodies;
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    fight.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 9);
    int ticks = 0;
    for (int run = 0; run < 2; ++run) { // the autopilot, then nobody at the controls; each fight to its end
        bodies.player = fight.playerStart();
        for (std::size_t i = 0; i < fight.enemyCount(); ++i) bodies.enemies[i] = fight.enemyStart(i);
        while (!fight.over() && ticks < 20000) {
            const ArenaInput in = run == 0 ? fight.autopilot() : ArenaInput{};
            fight.tick(kDt, in, bodies);
            arenaStep(fight, bodies, kDt);
            ++ticks;
        }
        fight.restart();
    }
    const ArenaFight copy = fight; // copying never allocates either
    (void)copy;
    const auto allocs = ftcore_test::allocStats().count - before;
    if (counting) CHECK(allocs == 0);
    CHECK(ticks > 500);
}
