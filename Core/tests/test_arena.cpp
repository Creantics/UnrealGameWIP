#include "ftcore/ArenaFight.h"
#include "ftcore/BeatChoreo.h" // the skinned boss's clip table
#include "ftcore/ComboSequence.h" // comboSequencesBuilt: the fence test

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
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
    f.release = swing(0.1f, 0.1f, 0.4f, 2.5f, 20, 0.f);
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
        in.light = in.heavy = in.dodge = in.heal = in.lock = in.release = in.pair = false;
        in.lockSwitch = 0;
        in.read = -1;
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
    CHECK(t.enemies.size() <= 9); // the wave's three, a human-sized duelist, the sized placeholders (hulk, titan), the two skinned bosses and the ranged spitter
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
    for (std::size_t e = 0; e < kinds.size(); ++e) {
        const ArenaEnemyKind& k = kinds[e];
        // The skinned boss's timings are authored move by move (to its animations), not by that ladder.
        const bool authored = e < table.enemyIds.size() && table.enemyIds[e] == "kite_boss";
        for (std::size_t a = 0; a < k.moveCount; ++a) {
            CHECK(k.moves[a].attack.startup >= 0.35f);
            for (std::size_t b = 0; b < k.moveCount && !authored; ++b)
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

TEST_CASE("arena table: a lead's kit starts from the default fighter, and lists only what differs") {
    ArenaTable t;
    const std::string err = loadError(
        R"({ "version": 1,
             "fighters": [ { "id": "lead", "hp": 80, "light": [ { "damage": 30 } ], "heavy": { "range": 9 } },
                           { "id": "default", "hp": 150, "moveAccel": 25, "parryWindow": 0.2,
                             "light": [ { "damage": 12, "range": 2.4 }, { "damage": 14 } ], "heavy": { "damage": 40 } } ],
             "enemies": [ { "id": "dummy" } ],
             "default": { "enemies": ["dummy"] } })",
        &t);
    CHECK(err.empty());
    const ArenaFighter& lead = t.fighter("lead");
    const ArenaFighter& def = t.fighter("default");
    CHECK(lead.hp == 80);                      // its own
    CHECK(lead.moveAccel == 25.f);             // the default's, not the struct's
    CHECK(lead.parryWindow == doctest::Approx(0.2f));
    CHECK(lead.lightCount == 1);               // a listed chain is the whole chain
    CHECK(lead.light[0].damage == 30);         // its own, on top of the default's light[0]
    CHECK(lead.light[0].range == doctest::Approx(2.4f));
    CHECK(lead.heavy.range == doctest::Approx(9.f));
    CHECK(lead.heavy.damage == 40);
    // The default itself still starts from the struct's values, whichever order the fighters come in.
    CHECK(def.hp == 150);
    CHECK(def.lightCount == 2);
    CHECK(t.fighter("lead").heavy.startup == doctest::Approx(ArenaFighter{}.heavy.startup));
}

TEST_CASE("arena data: each lead ships its own kit and its own blocks") {
    const ArenaTable t = loadShipped();
    const ArenaFighter& base = t.fighter("default");
    const ArenaFighter& devin = t.fighter("devin");
    const ArenaFighter& duy = t.fighter("duy");
    const ArenaFighter& third = t.fighter("third");
    for (const char* id : {"devin", "duy", "third"}) {
        bool found = false;
        for (const std::string& f : t.fighterIds) found = found || f == id;
        CHECK_MESSAGE(found, "Error: no fighter kit for " << id);
    }
    // Devin, the expert blade: a longer chain, a wider parry than anyone.
    CHECK(devin.lightCount > base.lightCount);
    CHECK(devin.parryWindow > base.parryWindow);
    CHECK(devin.parryWindow > duy.parryWindow);
    CHECK(devin.parryWindow > third.parryWindow);
    // Duy: kicks that reach further and break poise, a gun for the heavy (long, narrow, one target, no lunge).
    CHECK(duy.light[0].range > base.light[0].range);
    CHECK(duy.light[0].poiseDamage > base.light[0].poiseDamage);
    CHECK(duy.heavy.range >= 8.f);
    CHECK(duy.heavy.arcCos >= 0.95f);
    CHECK(duy.heavy.maxTargets == 1);
    CHECK(duy.heavy.lunge == 0.f);
    // The Third: spells at range with no lunge, a ward all round, and an even-handed parry.
    CHECK(third.light[0].range > base.light[0].range);
    CHECK(third.light[0].lunge == 0.f);
    CHECK(third.heavy.arcCos <= -1.f);
    CHECK(third.guardArcCos <= -1.f);
    // Each lead blocks its own way.
    CHECK(devin.guardArcCos != duy.guardArcCos);
    CHECK(duy.guardArcCos != third.guardArcCos);
    CHECK(devin.guardAbsorb > duy.guardAbsorb);
    CHECK(duy.guardAbsorb > third.guardAbsorb);
    // Every lead keeps the default's handling where it says nothing.
    CHECK(duy.moveBrake == base.moveBrake);
    CHECK(third.moveAccel == base.moveAccel);
    CHECK(third.hp == base.hp);
}

TEST_CASE("arena attack: Duy's gun heavy connects far off, with the nearest body only, and not past its range") {
    const ArenaTable t = loadShipped();
    const ArenaFighter duy = t.fighter("duy");
    {   // 8 m out, straight ahead, with another body behind it
        Rig rig(duy, {dummyKind(), dummyKind()});
        rig.put(0, 8.f, 0.f);
        rig.put(1, 8.8f, 0.f);
        rig.in.heavy = true;
        rig.runFor(1.5f);
        REQUIRE(rig.count(ArenaEventType::Hit) == 1);
        CHECK(rig.find(ArenaEventType::Hit)->event.target == 0);
        CHECK(rig.fight.enemyHp(1) == 100);
    }
    {   // well past its range: nothing
        Rig rig(duy, {dummyKind()});
        rig.put(0, 12.f, 0.f);
        rig.in.heavy = true;
        rig.runFor(1.5f);
        CHECK(rig.count(ArenaEventType::Hit) == 0);
    }
    {   // a body well off the line of the shot is missed, where a kick's wide arc would not care
        Rig rig(duy, {dummyKind()});
        rig.put(0, 6.f, 4.f);
        rig.in.heavy = true;
        rig.runFor(1.5f);
        CHECK(rig.count(ArenaEventType::Hit) == 0);
    }
}

namespace {

// A shot: a lane half a metre wide out to 9 m, told for 0.8 s, turning slowly while it is told.
ArenaAttack laneShot() {
    ArenaAttack a = swing(0.8f, 0.08f, 0.6f, 9.f, 12);
    a.width = 0.5f;
    a.tracking = 0.3f;
    return a;
}

// A shooter that asks for the token from anywhere in its lane's reach (its bodies are pinned, so it never walks in).
ArenaEnemyKind laneKind(const ArenaAttack& attack = laneShot(), float maxRange = 9.f) {
    ArenaEnemyKind k = hitterKind(attack, maxRange);
    k.preferredRange = maxRange;
    k.runRange = maxRange + 3.f;
    return k;
}

// Ticks until the first enemy attack starts (or `most` ticks pass); the tick it started on, or -1.
int runToAttack(Rig& rig, int most = 400) {
    for (int i = 0; i < most; ++i) {
        rig.tick();
        if (const Logged* l = rig.find(ArenaEventType::AttackStarted)) return l->tick;
    }
    return -1;
}

} // namespace

TEST_CASE("arena lane: a shot hits down its lane, close or far, and misses off it where an arc of its width would not") {
    for (const float x : {1.5f, 4.f, 8.f}) {
        Rig rig(ruleFighter(), {laneKind()});
        rig.put(0, x, 0.f);
        REQUIRE(runToAttack(rig) > 0);
        rig.runFor(1.f);
        CHECK_MESSAGE(rig.count(ArenaEventType::Hit) == 1, "at " << x << " m");
        CHECK(rig.fight.hp() == 88);
    }
    {   // Past its range plus the body: nothing.
        Rig rig(ruleFighter(), {laneKind(laneShot(), 12.f)});
        rig.put(0, 10.f, 0.f);
        REQUIRE(runToAttack(rig) > 0);
        rig.runFor(1.f);
        CHECK(rig.count(ArenaEventType::Hit) == 0);
    }
}

TEST_CASE("arena lane: a late sidestep out of the lane makes the shot miss; standing still it lands") {
    // 7 m off, the shooter turns 0.3 rad/s while it tells: stepping 1.5 m aside 0.2 s before the shot leaves the
    // lane (0.25 m + the body's 0.4 m off its line) far behind.
    for (const bool step : {false, true}) {
        Rig rig(ruleFighter(), {laneKind()});
        rig.put(0, 7.f, 0.f);
        REQUIRE(runToAttack(rig) > 0);
        rig.runFor(0.6f);
        CHECK(rig.count(ArenaEventType::Hit) == 0); // still told
        if (step) rig.bodies.player = Vec2{0.f, 1.5f};
        rig.runFor(0.6f);
        CHECK(rig.count(ArenaEventType::Hit) == (step ? 0 : 1));
        CHECK(rig.count(ArenaEventType::Evaded) == 0); // no roll: it simply missed
    }
}

TEST_CASE("arena lane: a guard blocks a shot, an unblockable volley goes through it, and a roll's i-frames evade it") {
    {   // guard held: blocked
        Rig rig(ruleFighter(), {laneKind()});
        rig.put(0, 6.f, 0.f);
        rig.in.guard = true;
        REQUIRE(runToAttack(rig) > 0);
        rig.runFor(1.f);
        CHECK(rig.count(ArenaEventType::Blocked) == 1);
        CHECK(rig.fight.hp() > 88); // at most the guard's chip of the shot's 12
    }
    ArenaEnemyKind volley = laneKind();
    volley.moves[0].attack.width = 1.6f;
    volley.moves[0].attack.damage = 20;
    volley.moves[0].unblockable = true;
    volley.moves[0].parryable = false;
    {   // the same guard against the volley: hit
        Rig rig(ruleFighter(), {volley});
        rig.put(0, 6.f, 0.f);
        rig.in.guard = true;
        REQUIRE(runToAttack(rig) > 0);
        rig.runFor(1.f);
        CHECK(rig.count(ArenaEventType::Blocked) == 0);
        CHECK(rig.count(ArenaEventType::Hit) == 1);
        CHECK(rig.fight.hp() == 80);
    }
    {   // a roll pressed 0.65 s into the 0.8 s tell: its i-frames cover the shot
        Rig rig(ruleFighter(), {volley});
        rig.put(0, 6.f, 0.f);
        REQUIRE(runToAttack(rig) > 0);
        rig.runFor(0.65f);
        rig.in.dodge = true;
        rig.tick();
        rig.runFor(0.6f);
        CHECK(rig.count(ArenaEventType::Evaded) == 1);
        CHECK(rig.count(ArenaEventType::Hit) == 0);
    }
}

TEST_CASE("arena data: the spitter shoots down lanes, told well before they land, and keeps a shove for close in") {
    const ArenaTable t = loadShipped();
    const ArenaEnemyKind* spitter = nullptr;
    for (std::size_t e = 0; e < t.enemyIds.size(); ++e)
        if (t.enemyIds[e] == "spitter") spitter = &t.enemies[e];
    REQUIRE(spitter);
    int lanes = 0, unblockableLanes = 0, close = 0;
    for (std::size_t m = 0; m < spitter->moveCount; ++m) {
        const ArenaEnemyMove& mv = spitter->moves[m];
        if (mv.attack.width > 0.f) {
            ++lanes;
            CHECK(mv.attack.startup >= 0.8f); // two beats: the lane reads with the sound off
            CHECK(mv.attack.range >= 8.f);
            CHECK(mv.attack.lunge == 0.f);
            CHECK(mv.attack.tracking <= 0.5f); // slow enough that a late sidestep beats it
            CHECK(mv.minRange >= 3.f);         // it shoots from afar only
            if (mv.unblockable) ++unblockableLanes;
        } else if (mv.maxRange <= 2.5f) {
            ++close;
        }
    }
    CHECK(lanes >= 2);
    CHECK(unblockableLanes >= 1);
    CHECK(close >= 1);
    // It backs off from a lead closing in, but slower than any lead moves, so it can be caught.
    for (const ArenaFighter& f : t.fighters) {
        CHECK(spitter->walkSpeed < f.moveSpeed);
        CHECK(spitter->runSpeed < f.moveSpeed);
    }
    // Every shipped lane, the spitter's or a lead's, tells for at least 0.4 s in an enemy.
    for (const ArenaEnemyKind& k : t.enemies)
        for (std::size_t m = 0; m < k.moveCount; ++m)
            if (k.moves[m].attack.width > 0.f) CHECK(k.moves[m].attack.startup >= 0.4f);
}

TEST_CASE("arena table: an attack's width is a lane, 0..20 m") {
    ArenaTable t;
    CHECK(loadError(tableJson(kFightersOk, R"([ { "id": "dummy", "moves": [ { "tell": "shot", "width": 0.5, "range": 9 } ] } ])"), &t).empty());
    REQUIRE(t.enemies.size() == 1);
    CHECK(t.enemies[0].moves[0].attack.width == doctest::Approx(0.5f));
    CHECK(t.enemies[0].moves[0].tell == ArenaTell::Shot);
    CHECK(contains(loadError(tableJson(kFightersOk, R"([ { "id": "dummy", "moves": [ { "width": -1 } ] } ])")), "moves[0].width"));
    CHECK(contains(loadError(tableJson(kFightersOk, R"([ { "id": "dummy", "moves": [ { "width": 21 } ] } ])")), "moves[0].width"));
    CHECK(std::string_view(arenaTellName(ArenaTell::Shot)) == "shot");
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
                                   ArenaAction::GuardBreak, ArenaAction::Downed, ArenaAction::Release};
    for (const ArenaAction a : actions) CHECK(std::string(arenaActionName(a)).size() > 2);
    CHECK(std::string(arenaActionName(ArenaAction::Idle)) == "Idle");
    CHECK(std::string(arenaActionName(ArenaAction::GuardBreak)) == "GuardBreak");
    CHECK(std::string(arenaActionName(ArenaAction::Release)) == "Release");
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

// ---- Load, Release, the paired counter and the beat ----

namespace {

// A hitter's bite parried: guard pressed on tick 19 (t = 0.3 s), the hit at about 0.42 s. `pairAt` and `pairAgain`
// press the partner's input on those ticks (1-based, 0 for none); `ticks` is how many to run.
void parryRun(Rig& rig, int ticks, int pairAt = 0, int pairAgain = 0) {
    rig.put(0, 1.5f, 0.f);
    for (int k = 1; k <= ticks; ++k) {
        if (k == 19) rig.in.guard = true;
        if (k == pairAt || k == pairAgain) rig.in.pair = true;
        rig.tick();
    }
}

} // namespace

TEST_CASE("arena Load: it never rises without a parry, and never refills or drains on its own") {
    ArenaAttack bite = swing(0.4f, 0.15f, 0.5f, 2.f, 20);
    bite.guardDamage = 10.f;
    {   // a hit taken
        Rig rig(ruleFighter(), {hitterKind(bite)});
        rig.put(0, 1.5f, 0.f);
        rig.run(40);
        CHECK(rig.count(ArenaEventType::Hit) >= 1);
        CHECK(rig.fight.load() == 0);
    }
    {   // a block: the guard held from the start, the window long gone when the bite lands
        Rig rig(ruleFighter(), {hitterKind(bite)});
        rig.put(0, 1.5f, 0.f);
        rig.in.guard = true;
        rig.run(40);
        CHECK(rig.count(ArenaEventType::Blocked) == 1);
        CHECK(rig.fight.load() == 0);
    }
    {   // a roll through it
        Rig rig(ruleFighter(), {hitterKind(bite)});
        rig.put(0, 1.5f, 0.f);
        rig.run(15);
        rig.in.dodge = true;
        rig.run(30);
        CHECK(rig.count(ArenaEventType::Evaded) == 1);
        CHECK(rig.fight.load() == 0);
    }
    {   // the player's own hits store nothing
        Rig rig(ruleFighter(), {dummyKind()});
        for (int i = 0; i < 3; ++i) {
            rig.in.light = true;
            rig.runFor(0.6f);
        }
        rig.in.heavy = true;
        rig.runFor(1.2f);
        CHECK(rig.count(ArenaEventType::Hit) >= 3);
        CHECK(rig.fight.load() == 0);
        CHECK(rig.count(ArenaEventType::LoadStored) == 0);
    }
    {   // a parry stores the parried damage; after that only another parry moves it
        Rig rig(ruleFighter(), {hitterKind(bite)});
        parryRun(rig, 40);
        REQUIRE(rig.count(ArenaEventType::Parried) == 1);
        CHECK(rig.fight.load() == 20);
        CHECK(rig.out().player.load == 20);
        rig.in.guard = false;
        rig.runFor(5.f);
        CHECK(rig.fight.load() == 20 * rig.count(ArenaEventType::Parried));
    }
}

TEST_CASE("arena Load: a parry stores damage x loadShare and says so") {
    const ArenaAttack bite = swing(0.4f, 0.15f, 0.5f, 2.f, 20);
    {
        Rig rig(ruleFighter(), {hitterKind(bite)});
        parryRun(rig, 40);
        const Logged* stored = rig.find(ArenaEventType::LoadStored);
        REQUIRE(stored);
        CHECK(stored->event.amount == 20);
        CHECK(stored->event.source == 0);
        CHECK(stored->event.target == kArenaPlayer);
        REQUIRE(rig.find(ArenaEventType::Parried));
        CHECK(stored->tick == rig.find(ArenaEventType::Parried)->tick);
        CHECK(rig.fight.stats().loadStored == 20);
    }
    {
        ArenaFighter f = ruleFighter();
        f.loadShare = 0.5f;
        Rig rig(f, {hitterKind(bite)});
        parryRun(rig, 40);
        CHECK(rig.fight.load() == 10);
    }
    {
        ArenaFighter f = ruleFighter();
        f.loadShare = 0.f;
        Rig rig(f, {hitterKind(bite)});
        parryRun(rig, 40);
        CHECK(rig.count(ArenaEventType::Parried) == 1);
        CHECK(rig.fight.load() == 0);
        CHECK(rig.count(ArenaEventType::LoadStored) == 0);
    }
}

TEST_CASE("arena Load: stamina and Load are apart; a Release costs no stamina and works at none") {
    // Ten stamina, a light that costs ten and no regeneration: one swing empties it for good.
    ArenaFighter f = ruleFighter();
    f.stamina = 10.f;
    f.staminaRegen = 0.f;
    Rig rig(f, {hitterKind(swing(1.f, 0.15f, 0.5f, 2.f, 40))});
    rig.put(0, 1.5f, 0.f);
    rig.in.light = true;
    rig.tick();
    CHECK(rig.fight.stamina() == doctest::Approx(0.f));
    CHECK(rig.fight.load() == 0); // spending stamina moves no Load
    rig.runFor(0.85f);
    rig.in.guard = true; // pressed at about 0.87 s: the window holds the hit at about 1.02 s
    rig.run(20);
    REQUIRE(rig.count(ArenaEventType::Parried) == 1);
    CHECK(rig.fight.load() == 40);
    CHECK(rig.fight.stamina() == doctest::Approx(0.f)); // storing Load gives no stamina
    rig.in.guard = false;
    rig.tick();
    rig.in.release = true;
    rig.tick();
    CHECK(rig.count(ArenaEventType::Released) == 1);
    CHECK(rig.out().player.action == ArenaAction::Release);
    CHECK(rig.fight.load() == 0);
    CHECK(rig.fight.stamina() == doctest::Approx(0.f));
}

TEST_CASE("arena Release: refused at 33 Load and under, open at 34; it spends it all as poise damage") {
    {   // 33 stored: refused, nothing spent
        Rig rig(ruleFighter(), {hitterKind(swing(0.4f, 0.15f, 0.5f, 2.f, 33))});
        parryRun(rig, 40);
        REQUIRE(rig.fight.load() == 33);
        rig.in.guard = false;
        rig.tick();
        rig.in.release = true;
        rig.tick();
        CHECK(rig.count(ArenaEventType::Released) == 0);
        CHECK(rig.out().player.action != ArenaAction::Release);
        CHECK(rig.fight.load() == 33);
        CHECK(rig.fight.stats().releases == 0);
    }
    {   // nothing stored: refused
        Rig rig(ruleFighter(), {dummyKind()});
        rig.in.release = true;
        rig.tick();
        CHECK(rig.count(ArenaEventType::Released) == 0);
    }
    {   // 34: the Release goes, spends all 34, is invulnerable, and an upright enemy loses 34 poise
        ArenaEnemyKind upright = dummyKind();
        upright.poise = 40.f;
        Rig rig(ruleFighter(), {hitterKind(swing(0.4f, 0.15f, 0.5f, 2.f, 34)), upright});
        rig.put(1, 0.f, 1.5f);
        parryRun(rig, 40);
        REQUIRE(rig.fight.load() == 34);
        rig.in.guard = false;
        rig.tick();
        rig.in.release = true;
        rig.tick();
        const Logged* released = rig.find(ArenaEventType::Released);
        REQUIRE(released);
        CHECK(released->event.amount == 34);
        CHECK(released->event.source == kArenaPlayer);
        CHECK(rig.fight.load() == 0);
        CHECK(rig.fight.stats().releases == 1);
        CHECK(rig.out().player.action == ArenaAction::Release);
        CHECK(rig.out().player.invulnerable);
        rig.runFor(0.3f);
        CHECK(rig.fight.enemyPoise(1) == doctest::Approx(6.f));
        CHECK(rig.fight.enemyHp(1) == 80); // the Release's own damage
    }
    {   // releasePoise scales it; enough breaks the poise
        ArenaFighter f = ruleFighter();
        f.releasePoise = 2.f;
        ArenaEnemyKind upright = dummyKind();
        upright.poise = 40.f;
        Rig rig(f, {hitterKind(swing(0.4f, 0.15f, 0.5f, 2.f, 34)), upright});
        rig.put(1, 0.f, 1.5f);
        parryRun(rig, 40);
        rig.in.guard = false;
        rig.tick();
        rig.in.release = true;
        rig.runFor(0.3f);
        bool staggered = false;
        for (const Logged& l : rig.log)
            if (l.event.type == ArenaEventType::Staggered && l.event.target == 1) staggered = true;
        CHECK(staggered);
    }
}

TEST_CASE("arena Overload: at full Load attacks and heals are refused until a Release; the defence stays open") {
    // A bite of 120 parried: only the 100 that fit are stored, and Overloaded comes once. It bites only once.
    ArenaEnemyKind once = hitterKind(swing(0.4f, 0.15f, 0.5f, 2.f, 120));
    once.moves[0].cooldown = 60.f;
    Rig rig(ruleFighter(), {once});
    parryRun(rig, 40);
    REQUIRE(rig.count(ArenaEventType::Parried) == 1);
    CHECK(rig.fight.load() == kMaxLoad);
    CHECK(rig.fight.overloaded());
    CHECK(rig.out().player.overloaded);
    REQUIRE(rig.find(ArenaEventType::LoadStored));
    CHECK(rig.find(ArenaEventType::LoadStored)->event.amount == kMaxLoad);
    CHECK(rig.count(ArenaEventType::Overloaded) == 1);
    CHECK(rig.fight.stats().overloads == 1);
    rig.in.guard = false;
    rig.tick();
    const int hp = rig.fight.hp();
    rig.in.light = true; // the enemy is open, so this would be a riposte: refused all the same
    rig.tick();
    CHECK(rig.out().player.action != ArenaAction::Riposte);
    CHECK(rig.out().player.action != ArenaAction::Light);
    rig.runFor(0.3f); // the buffer runs out
    rig.in.heavy = true;
    rig.tick();
    CHECK(rig.out().player.action != ArenaAction::Heavy);
    rig.runFor(0.3f);
    rig.in.heal = true;
    rig.tick();
    CHECK(rig.out().player.action != ArenaAction::Heal);
    CHECK(rig.fight.healCharges() == 2);
    CHECK(rig.fight.hp() == hp);
    rig.runFor(0.3f);
    rig.in.guard = true; // the guard goes up
    rig.tick();
    CHECK(rig.out().player.action == ArenaAction::Guard);
    rig.in.guard = false;
    rig.tick();
    rig.in.dodge = true; // and the roll
    rig.tick();
    CHECK(rig.out().player.action == ArenaAction::Dodge);
    rig.runFor(0.7f);
    CHECK(rig.count(ArenaEventType::Overloaded) == 1);
    rig.in.release = true; // the Release lifts it
    rig.tick();
    CHECK(rig.count(ArenaEventType::Released) == 1);
    CHECK_FALSE(rig.fight.overloaded());
    CHECK(rig.fight.load() == 0);
    rig.runFor(0.7f);
    rig.in.light = true;
    rig.tick();
    const ArenaAction after = rig.out().player.action;
    CHECK((after == ArenaAction::Light || after == ArenaAction::Riposte));
}

TEST_CASE("arena beat: arenaWholeBeats, and every authored startup is a whole number of beats") {
    CHECK(arenaWholeBeats(0.4f, 150.f) == 1);
    CHECK(arenaWholeBeats(0.8f, 150.f) == 2);
    CHECK(arenaWholeBeats(1.2f, 150.f) == 3);
    CHECK(arenaWholeBeats(0.4f + kArenaBeatTolerance * 0.5f, 150.f) == 1);
    CHECK(arenaWholeBeats(0.45f, 150.f) == -1);
    CHECK(arenaWholeBeats(0.1f, 150.f) == -1); // under one beat
    CHECK(arenaWholeBeats(0.5f, 120.f) == 1);
    CHECK(arenaWholeBeats(0.4f, 0.f) == -1);
    CHECK(arenaWholeBeats(0.f, 150.f) == -1);

    const ArenaTable table = loadShipped();
    REQUIRE(table.bpm > 0.f);
    CHECK(table.fallback.bpm == table.bpm);
    for (const ArenaFightEntry& e : table.entries) CHECK(e.def.bpm == table.bpm);
    for (std::size_t i = 0; i < table.enemies.size(); ++i) {
        const float bpm = table.enemies[i].bpm > 0.f ? table.enemies[i].bpm : table.bpm; // a kind may keep its own beat
        for (std::size_t m = 0; m < table.enemies[i].moveCount; ++m)
            CHECK_MESSAGE(arenaWholeBeats(table.enemies[i].moves[m].attack.startup, bpm) > 0, table.enemyIds[i] << " move " << m);
    }
    const ArenaDef def = defaultArenaDef();
    REQUIRE(def.bpm > 0.f);
    for (std::size_t m = 0; m < def.enemies[0].moveCount; ++m)
        CHECK(arenaWholeBeats(def.enemies[0].moves[m].attack.startup, def.bpm) > 0);
}

TEST_CASE("arena beat: the loader refuses an off-beat startup and a bpm out of range") {
    const auto withBpm = [](const std::string& bpm, const char* startup) {
        std::string json = tableJson(kFightersOk, std::string(R"([ { "id": "dummy", "moves": [ { "startup": )") + startup +
                                                      R"(, "active": 0.1, "range": 2, "maxRange": 2, "weight": 1 } ] } ])");
        const std::string version = R"("version": 1,)";
        json.replace(json.find(version), version.size(), version + R"( "bpm": )" + bpm + ",");
        return json;
    };
    ArenaTable t;
    CHECK(loadError(withBpm("150", "0.4"), &t).empty());
    CHECK(t.bpm == 150.f);
    CHECK(t.fallback.bpm == 150.f);
    const std::string off = loadError(withBpm("150", "0.45"));
    CHECK(contains(off, "enemies[0].moves[0].startup"));
    CHECK(contains(off, "whole number of beats"));
    CHECK_FALSE(loadError(withBpm("120", "0.45")).empty());
    CHECK(loadError(withBpm("120", "1.0")).empty());
    CHECK(contains(loadError(withBpm("0", "0.4")), "bpm"));
    CHECK(contains(loadError(withBpm("301", "0.4")), "bpm"));
    CHECK(contains(loadError(withBpm(R"("fast")", "0.4")), "bpm"));
    // Without a bpm nothing is checked.
    ArenaTable plain;
    const std::string noBpm =
        tableJson(kFightersOk, R"([ { "id": "dummy", "moves": [ { "startup": 0.45, "active": 0.1, "range": 2, "maxRange": 2, "weight": 1 } ] } ])");
    CHECK(loadError(noBpm, &plain).empty());
    CHECK(plain.bpm == 0.f);
}

TEST_CASE("arena kinds: a bpm of their own, a skinned look and a clip per move load, and a bad one is refused") {
    const auto withKind = [](const char* extra, const char* moveExtra = "") {
        return tableJson(kFightersOk, std::string(R"([ { "id": "dummy", )") + extra +
                                          R"(, "moves": [ { "startup": 1.667, "active": 0.1, "range": 2, "maxRange": 2, "weight": 1)" +
                                          moveExtra + " } ] } ]");
    };
    const auto withFileBpm = [](std::string json) {
        const std::string version = R"("version": 1,)";
        json.replace(json.find(version), version.size(), version + R"( "bpm": 150,)");
        return json;
    };
    const char* const look = R"("look": { "clips": "Data/Anim/x.json", "hover": 0.9, "scale": 2, "yawOffset": 90 })";
    // The file's beat (0.4 s) does not fit 1.667 s; the kind's own (72 bpm, 0.833 s) does.
    CHECK(contains(loadError(withFileBpm(withKind(R"("hp": 5)"))), "whole number of beats at 150 bpm"));
    ArenaTable t;
    CHECK(loadError(withFileBpm(withKind(R"("bpm": 72)")), &t).empty());
    REQUIRE(t.enemies.size() == 1);
    CHECK(t.enemies[0].bpm == 72.f);
    CHECK(contains(loadError(withFileBpm(withKind(R"("bpm": 90)"))), "whole number of beats at 90 bpm"));
    CHECK(contains(loadError(withKind(R"("bpm": 0)")), "enemies[0].bpm"));
    CHECK(contains(loadError(withKind(R"("bpm": 301)")), "enemies[0].bpm"));

    CHECK(loadError(withKind(look, R"(, "clip": "swing_big")"), &t).empty());
    CHECK(t.enemies[0].look.skinned());
    CHECK(t.enemies[0].look.clips() == "Data/Anim/x.json");
    CHECK(t.enemies[0].look.hover == 0.9f);
    CHECK(t.enemies[0].look.scale == 2.f);
    CHECK(t.enemies[0].look.yawOffset == 90.f);
    CHECK(t.enemies[0].moves[0].clip() == "swing_big");
    // No look and no clip: the shapes, as before.
    CHECK(loadError(withKind(R"("hp": 5)"), &t).empty());
    CHECK_FALSE(t.enemies[0].look.skinned());
    CHECK(t.enemies[0].look.scale == 1.f);
    CHECK(t.enemies[0].moves[0].clip().empty());

    CHECK(contains(loadError(withKind(R"("look": 3)")), "enemies[0].look: expected an object"));
    CHECK(contains(loadError(withKind(R"("look": {})")), "enemies[0].look.clips"));
    CHECK(contains(loadError(withKind(R"("look": { "clips": "../x.json" })")), "enemies[0].look.clips"));
    CHECK(contains(loadError(withKind(R"("look": { "clips": "C:/x.json" })")), "enemies[0].look.clips"));
    CHECK(contains(loadError(withKind(R"("look": { "clips": "Data/x.json", "hover": -1 })")), "enemies[0].look.hover"));
    CHECK(contains(loadError(withKind(R"("look": { "clips": "Data/x.json", "scale": 0 })")), "enemies[0].look.scale"));
    CHECK(contains(loadError(withKind(R"("look": { "clips": "Data/x.json", "yawOffset": 400 })")), "enemies[0].look.yawOffset"));
    CHECK(contains(loadError(withKind(look, R"(, "clip": "has space")")), "moves[0].clip"));
    CHECK(contains(loadError(withKind(look, R"(, "clip": 4)")), "moves[0].clip"));
    CHECK(contains(loadError(withKind(look, R"(, "clip": "this_clip_id_is_much_too_long_for_it")")), "moves[0].clip"));
    CHECK(contains(loadError(withKind(R"("hp": 5)", R"(, "clip": "swing_big")")), "moves[0].clip: expected the enemy to have a look"));
    std::vector<std::string> warnings;
    CHECK(loadError(withKind(R"("look": { "clips": "Data/x.json", "colour": 1 })"), nullptr, &warnings).empty());
    REQUIRE(warnings.size() == 1);
    CHECK(contains(warnings[0], "enemies[0].look: unknown key: colour"));
    // Two moves of one enemy cannot share a clip.
    const std::string twice = tableJson(kFightersOk, std::string(R"([ { "id": "dummy", )") + look +
                                                        R"(, "moves": [ { "clip": "a" }, { "clip": "a", "weight": 0 } ] } ])");
    CHECK(contains(loadError(twice), "moves[1].clip: the same as moves[0]'s"));
    // Up to kMaxArenaMoves moves: the first nine name every tell once (a named tell tells its move apart), the rest
    // leave it out; one more is refused.
    static_assert(kMaxArenaMoves >= kArenaTellCount, "the test names every tell once");
    std::string full = R"([ { "id": "dummy", "moves": [)";
    for (std::size_t m = 0; m < kMaxArenaMoves; ++m) {
        full += m ? "," : "";
        full += m < kArenaTellCount
                    ? std::string(R"({ "tell": ")") + arenaTellName(static_cast<ArenaTell>(m)) + R"(" })"
                    : std::string("{}");
    }
    CHECK(loadError(tableJson(kFightersOk, full + "] } ]")).empty());
    const std::string tooMany = "moves: expected an array of 1.." + std::to_string(kMaxArenaMoves) + " moves";
    CHECK(contains(loadError(tableJson(kFightersOk, full + R"(, {}] } ])")), tooMany.c_str()));
}

namespace {

// The shipped skinned boss (arena.json's kite_boss) and its clip table.
struct KiteData {
    ArenaTable table;
    BeatClips clips;
    std::size_t kind = 0;
    const ArenaEnemyKind& boss() const { return table.enemies[kind]; }
};

KiteData loadKite() {
    KiteData k;
    k.table = loadShipped();
    for (std::size_t e = 0; e < k.table.enemyIds.size(); ++e)
        if (k.table.enemyIds[e] == "kite_boss") k.kind = e;
    REQUIRE(k.table.enemyIds[k.kind] == "kite_boss");
    std::string text, err;
    std::vector<std::string> warnings;
    if (!ftcore_test::readProjectData("Anim/kite_clips.json", text)) FAIL("Data/Anim/kite_clips.json not found");
    REQUIRE_MESSAGE(importBeatClips(text, k.clips, &err, &warnings), err);
    CHECK(warnings.empty());
    return k;
}

} // namespace

TEST_CASE("arena kite boss: its moves name clips of its table, with impacts that land at the end of the startup") {
    const KiteData k = loadKite();
    const ArenaEnemyKind& boss = k.boss();
    CHECK(boss.hp == 1100);
    CHECK(boss.height == 9.1f);
    CHECK(boss.look.skinned());
    CHECK(boss.look.clips() == "Data/Anim/kite_clips.json");
    CHECK(boss.look.hover > 0.f);
    CHECK(boss.bpm == 72.f);
    CHECK(k.clips.mesh().rfind("/Game/", 0) == 0);
    // The clips the host plays for a state, not a move.
    for (const char* id : {"idle", "idle_p2", "stagger", "face_down", "defeat", "hit_light", "hit_heavy"})
        CHECK_MESSAGE(k.clips.find(id) >= 0, id);
    CHECK(k.clips[static_cast<std::size_t>(k.clips.find("idle"))].loop);
    CHECK(k.clips[static_cast<std::size_t>(k.clips.find("face_down"))].loop);
    REQUIRE(boss.moveCount == 8);
    int parryable = 0;
    for (std::size_t m = 0; m < boss.moveCount; ++m) {
        const ArenaEnemyMove& mv = boss.moves[m];
        const int clip = k.clips.find(mv.clip());
        REQUIRE_MESSAGE(clip >= 0, "move " << m << " clip " << mv.clip());
        const BeatClip& c = k.clips[static_cast<std::size_t>(clip)];
        CHECK_MESSAGE(c.impact > 0.0, mv.clip());
        // The clip's blow lands on the end of the startup at a play rate near 1, and the clip is as long as the move.
        const double rate = c.impact / static_cast<double>(mv.attack.startup);
        CHECK_MESSAGE(std::fabs(rate - 1.0) < 0.01, mv.clip() << " rate " << rate);
        CHECK_MESSAGE(std::fabs(c.length - static_cast<double>(mv.attack.total())) < 0.01, mv.clip());
        CHECK(mv.attack.lunge == 0.f);
        if (mv.parryable && !mv.unblockable) ++parryable;
    }
    // Swoop is the marked move: the only big one a parry opens the boss on (the two basic strikes are parryable too).
    for (std::size_t m = 0; m < boss.moveCount; ++m)
        if (boss.moves[m].attack.damage >= 8)
            CHECK_MESSAGE(boss.moves[m].parryable == (boss.moves[m].clip() == "swoop"), boss.moves[m].clip());
    CHECK(parryable == 3);
}

TEST_CASE("arena kite boss: about one swoop in ten seconds") {
    const KiteData k = loadKite();
    std::size_t swoop = 0;
    for (std::size_t m = 0; m < k.boss().moveCount; ++m)
        if (k.boss().moves[m].clip() == "swoop") swoop = m;
    // A lead who cannot die and presses nothing: the boss attacks without a pause.
    ArenaDef def;
    def.fighter = k.table.fighter("default");
    def.fighter.hp = 1000000;
    def.enemyCount = 1;
    def.enemies[0] = k.boss();
    def.ringRadius = 13.4f;
    def.spawnDistance = 7.7f;
    int swoops = 0;
    float seconds = 0.f;
    for (std::uint32_t seed = 1; seed <= 6; ++seed) {
        const ArenaRing ring{Vec2{0.f, 0.f}, def.ringRadius};
        ArenaFight fight;
        fight.begin(def, ring, Vec2{-4.f, 0.f}, Vec2{1.f, 0.f}, seed);
        ArenaBodies bodies;
        bodies.player = fight.playerStart();
        bodies.enemies[0] = fight.enemyStart(0);
        while (fight.time() < 120.f) {
            const ArenaOutput& out = fight.tick(kDt, ArenaInput{}, bodies);
            arenaStep(fight, bodies, kDt);
            for (const ArenaEvent& e : out.events)
                if (e.type == ArenaEventType::AttackStarted && e.move == swoop) ++swoops;
        }
        seconds += fight.time();
    }
    const float every = seconds / static_cast<float>(std::max(swoops, 1));
    MESSAGE("arena kite boss: one swoop every " << every << " s (" << swoops << " in " << seconds << " s)");
    CHECK(every > 8.f);
    CHECK(every < 13.f);
}

TEST_CASE("arena paired counter: the partner's press within pairWindow of a parry, either first, counter-staggers") {
    // Where the parry lands.
    Rig probe(ruleFighter(), {hitterKind(swing(0.4f, 0.15f, 0.5f, 2.f, 20))});
    parryRun(probe, 40);
    const Logged* parried = probe.find(ArenaEventType::Parried);
    REQUIRE(parried);
    const int p = parried->tick;
    const int inside = 6;  // 0.1 s at 60 Hz
    const int outside = 9; // 0.15 s
    const auto check = [&](int pairAt, bool pairs) {
        Rig rig(ruleFighter(), {hitterKind(swing(0.4f, 0.15f, 0.5f, 2.f, 20))});
        parryRun(rig, p + 15, pairAt);
        CHECK(rig.count(ArenaEventType::Parried) == 1);
        if (pairs) {
            const Logged* counter = rig.find(ArenaEventType::CounterStagger);
            REQUIRE(counter);
            CHECK(counter->event.amount == 3 * ruleFighter().light[0].damage);
            CHECK(counter->event.source == kArenaPlayer);
            CHECK(counter->event.target == 0);
            CHECK(rig.fight.enemyHp(0) == 100 - 30);
            CHECK(rig.fight.stats().counterStaggers == 1);
            const Logged* staggered = rig.find(ArenaEventType::Staggered);
            REQUIRE(staggered);
            CHECK(staggered->tick == counter->tick);
            CHECK(rig.out().enemies[0].brain == ArenaBrain::Staggered);
        } else {
            CHECK(rig.count(ArenaEventType::CounterStagger) == 0);
            CHECK(rig.fight.enemyHp(0) == 100);
            CHECK(rig.out().enemies[0].brain == ArenaBrain::Open);
        }
    };
    check(p, true);          // the same tick
    check(p + inside, true); // after the parry
    check(p - inside, true); // before it
    check(p + outside, false);
    check(p - outside, false);
    {   // one press per parry: a second press pairs with nothing
        Rig rig(ruleFighter(), {hitterKind(swing(0.4f, 0.15f, 0.5f, 2.f, 20))});
        parryRun(rig, p + 15, p + 1, p + 3);
        CHECK(rig.count(ArenaEventType::CounterStagger) == 1);
    }
    {   // pairHits sets the worth, and the window is the fighter's
        ArenaFighter f = ruleFighter();
        f.pairHits = 5;
        f.pairWindow = 0.2f;
        Rig rig(f, {hitterKind(swing(0.4f, 0.15f, 0.5f, 2.f, 20))});
        parryRun(rig, p + 15, p + outside);
        const Logged* counter = rig.find(ArenaEventType::CounterStagger);
        REQUIRE(counter);
        CHECK(counter->event.amount == 50);
    }
    {   // no parry, no counter
        Rig rig(ruleFighter(), {hitterKind(swing(0.4f, 0.15f, 0.5f, 2.f, 20))});
        rig.put(0, 1.5f, 0.f);
        for (int k = 1; k <= 40; ++k) {
            rig.in.pair = k == 20;
            rig.tick();
        }
        CHECK(rig.count(ArenaEventType::CounterStagger) == 0);
    }
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

// ---- Read ----

namespace {

// An enemy that asks at once and swings one of three moves alike in all but their tells.
ArenaEnemyKind readKind() {
    ArenaEnemyKind k = hitterKind(swing(0.4f, 0.15f, 0.5f, 2.f, 20));
    k.moveCount = 3;
    k.moves[1] = k.moves[0];
    k.moves[2] = k.moves[0];
    k.moves[0].tell = ArenaTell::Thrust;
    k.moves[1].tell = ArenaTell::Sweep;
    k.moves[2].tell = ArenaTell::Slam;
    return k;
}

// The rule kit with hp to stand there and be hit for as long as a test needs, and round Read numbers.
ArenaFighter sturdyFighter() {
    ArenaFighter f = ruleFighter();
    f.hp = 100000;
    f.readPower = 1.5f; // round numbers: a counter of 15 at one stack, 3 more per stack
    f.readStackPower = 0.2f;
    f.readPoise = 10.f;
    f.readWrongPoise = 10.f;
    return f;
}

// Ticks until a tick deals Read cards (true), or `most` ticks pass.
bool untilDealt(Rig& rig, int most = 600) {
    for (int t = 0; t < most; ++t) {
        const ArenaOutput& out = rig.tick();
        for (const ArenaEvent& e : out.events)
            if (e.type == ArenaEventType::CardsDealt) return true;
    }
    return false;
}

// Ticks until the nth ReadCounter is logged (its log entry), or `most` ticks pass.
const Logged* untilCounter(Rig& rig, int nth, int most = 120) {
    for (int t = 0; t < most && !rig.find(ArenaEventType::ReadCounter, nth); ++t) rig.tick();
    return rig.find(ArenaEventType::ReadCounter, nth);
}

} // namespace

TEST_CASE("arena Read: every enemy move deals three cards as it starts, and the right card is always that move") {
    Rig rig(sturdyFighter(), {readKind(), readKind()});
    rig.put(1, -1.5f, 0.f);
    int starts = 0, deals = 0;
    bool at[kReadCards] = {};
    for (int t = 0; t < 1800; ++t) { // 30 s: both enemies, every move, many times
        const ArenaOutput& out = rig.tick();
        for (const ArenaEvent& e : out.events) {
            if (e.type == ArenaEventType::AttackStarted) ++starts;
            if (e.type != ArenaEventType::CardsDealt) continue;
            ++deals;
            const ReadCards& cards = rig.fight.readCards();
            REQUIRE(cards.size() == kReadCards);
            CHECK(e.amount == static_cast<int>(kReadCards));
            CHECK(e.target == kArenaPlayer);
            CHECK(rig.fight.readFoe() == e.source);
            CHECK(rig.fight.readMove() == e.move);
            CHECK(rig.fight.readOpen());
            const ReadCard& right = cards[cards.correct()];
            CHECK(right.actor == e.source); // the coming move ...
            CHECK(right.action == e.move);
            CHECK(right.tell == std::string_view(arenaTellName(rig.def.enemies[e.source].moves[e.move].tell))); // ... and its tell
            CHECK(cards[0].tell != cards[1].tell);
            CHECK(cards[0].tell != cards[2].tell);
            CHECK(cards[1].tell != cards[2].tell);
            at[cards.correct()] = true;
        }
        // The cards stay only while the move is in its startup; nothing is ever paused for them.
        if (rig.fight.readCards().dealt()) CHECK(out.enemies[rig.fight.readFoe()].part == ArenaPart::Startup);
    }
    CHECK(rig.fight.time() == doctest::Approx(1800 * kDt));
    CHECK(starts > 20);
    CHECK(deals == starts); // one attacker at a time: every move gets its deal
    CHECK(rig.fight.stats().readsDealt == deals);
    CHECK(rig.fight.stats().reads == 0); // nobody picked: nothing happened
    CHECK(rig.count(ArenaEventType::Read) == 0);
    CHECK(rig.count(ArenaEventType::Misread) == 0);
    CHECK(rig.count(ArenaEventType::ReadCounter) == 0);
    CHECK((at[0] && at[1] && at[2])); // the right card turns up in every place
}

TEST_CASE("arena Read: the decoys come from every enemy's moves; fewer tells, fewer cards; one tell, none") {
    {   // a lone enemy with two tells: two cards
        ArenaEnemyKind k = readKind();
        k.moveCount = 2;
        Rig rig(sturdyFighter(), {k});
        REQUIRE(untilDealt(rig));
        CHECK(rig.fight.readCards().size() == 2);
    }
    {   // every move with the same tell: nothing to read, nothing dealt
        Rig rig(sturdyFighter(), {hitterKind(swing(0.4f, 0.15f, 0.5f, 2.f, 20))});
        CHECK_FALSE(untilDealt(rig, 300));
        CHECK(rig.count(ArenaEventType::AttackStarted) > 0);
        CHECK(rig.fight.stats().readsDealt == 0);
        CHECK(rig.fight.readFoe() == kArenaNobody);
    }
    {   // the decoys may be another enemy's moves: a one-tell hitter beside a three-tell enemy that never attacks
        ArenaEnemyKind still = readKind();
        still.attackDelay = 1000.f;
        Rig rig(sturdyFighter(), {hitterKind(swing(0.4f, 0.15f, 0.5f, 2.f, 20)), still});
        rig.put(1, 0.f, 1.5f);
        REQUIRE(untilDealt(rig));
        const ReadCards& cards = rig.fight.readCards();
        REQUIRE(cards.size() == kReadCards);
        for (std::size_t c = 0; c < cards.size(); ++c) CHECK(cards[c].actor == (c == cards.correct() ? 0u : 1u));
        CHECK(cards[cards.correct()].tell == std::string_view("strike"));
    }
}

TEST_CASE("arena Read: a pick before the active part counts; right stacks and counters, wrong resets and steadies") {
    ArenaEnemyKind k = readKind();
    k.poiseDelay = 100.f; // the poise stays where the reads leave it
    Rig rig(sturdyFighter(), {k});

    // A right pick a moment into the startup; the fight ran on while the cards were up.
    REQUIRE(untilDealt(rig));
    const int dealt = rig.ticks;
    const float time0 = rig.fight.time(), state0 = rig.out().enemies[0].stateTime;
    rig.run(6);
    CHECK(rig.fight.time() == doctest::Approx(time0 + 6 * kDt));
    CHECK(rig.out().enemies[0].stateTime == doctest::Approx(state0 + 6 * kDt));
    REQUIRE(rig.fight.readOpen());
    const std::size_t right = rig.fight.readCards().correct();
    rig.in.read = static_cast<std::int8_t>(right);
    rig.tick();
    const Logged* read = rig.find(ArenaEventType::Read);
    REQUIRE(read);
    CHECK(read->event.source == 0);
    CHECK(read->event.amount == 1);
    CHECK(rig.fight.readStacks() == 1);
    CHECK(rig.fight.stats().reads == 1);
    CHECK(rig.fight.stats().correctReads == 1);
    CHECK(rig.fight.readCards().picked());
    CHECK_FALSE(rig.fight.readOpen());
    // A second pick of the same deal does nothing.
    rig.in.read = static_cast<std::int8_t>((right + 1) % kReadCards);
    rig.tick();
    CHECK(rig.count(ArenaEventType::Misread) == 0);
    CHECK(rig.fight.stats().reads == 1);
    // The partner's counter as the move's active part ends (0.55 s in): 1.5 x light[0]'s 10, and readPoise.
    const Logged* first = untilCounter(rig, 0);
    REQUIRE(first);
    CHECK(first->tick - dealt >= 32);
    CHECK(first->tick - dealt <= 34);
    CHECK(first->event.amount == 15);
    CHECK(first->event.source == kArenaPlayer);
    CHECK(first->event.target == 0);
    CHECK(first->event.move == 1);
    CHECK(rig.fight.enemyHp(0) == 85);
    CHECK(rig.fight.enemyPoise(0) == doctest::Approx(20.f));
    CHECK(rig.fight.stats().readCounters == 1);
    CHECK(rig.fight.stats().readDamage == 15);

    // Another right pick: two stacks, a counter 20% stronger.
    REQUIRE(untilDealt(rig));
    rig.in.read = static_cast<std::int8_t>(rig.fight.readCards().correct());
    rig.tick();
    CHECK(rig.fight.readStacks() == 2);
    const Logged* second = untilCounter(rig, 1);
    REQUIRE(second);
    CHECK(second->event.amount == 18);
    CHECK(rig.fight.enemyHp(0) == 67);
    CHECK(rig.fight.enemyPoise(0) == doctest::Approx(10.f));

    // A wrong pick: the stacks are gone, the attacker gets readWrongPoise back, and no counter comes.
    REQUIRE(untilDealt(rig));
    rig.in.read = static_cast<std::int8_t>((rig.fight.readCards().correct() + 1) % kReadCards);
    rig.tick();
    const Logged* misread = rig.find(ArenaEventType::Misread);
    REQUIRE(misread);
    CHECK(misread->event.amount == 10);
    CHECK(rig.fight.readStacks() == 0);
    CHECK(rig.fight.enemyPoise(0) == doctest::Approx(20.f));
    CHECK(rig.fight.stats().reads == 3);
    CHECK(rig.fight.stats().correctReads == 2);
    rig.run(60);
    CHECK(rig.count(ArenaEventType::ReadCounter) == 2);
    CHECK(rig.fight.enemyHp(0) == 67);

    // A pick on the last tick of the startup still counts ...
    REQUIRE(untilDealt(rig));
    const float startup = rig.def.enemies[0].moves[0].attack.startup;
    while (rig.out().enemies[0].stateTime + kDt < startup) rig.tick();
    REQUIRE(rig.fight.readOpen());
    rig.in.read = static_cast<std::int8_t>(rig.fight.readCards().correct());
    rig.tick();
    CHECK(rig.count(ArenaEventType::Read) == 3);
    CHECK(rig.fight.readStacks() == 1);
    // ... and one once the active part has begun is too late: the cards are gone.
    REQUIRE(untilDealt(rig));
    while (rig.out().enemies[0].part == ArenaPart::Startup) rig.tick();
    CHECK_FALSE(rig.fight.readCards().dealt());
    CHECK_FALSE(rig.fight.readOpen());
    rig.in.read = 0;
    rig.tick();
    CHECK(rig.fight.stats().reads == 4);
    CHECK(rig.count(ArenaEventType::Read) == 3);
    CHECK(rig.count(ArenaEventType::Misread) == 1);
}

TEST_CASE("arena Read: the stacks top out at kMaxReadStacks; restart() clears them") {
    ArenaEnemyKind k = readKind();
    k.hp = 1000;
    Rig rig(sturdyFighter(), {k});
    const int want[] = {15, 18, 21, 24, 27, 27, 27};
    for (int r = 0; r < 7; ++r) {
        REQUIRE(untilDealt(rig));
        rig.in.read = static_cast<std::int8_t>(rig.fight.readCards().correct());
        rig.tick();
        CHECK(rig.fight.readStacks() == std::min(r + 1, kMaxReadStacks));
        const Logged* counter = untilCounter(rig, r);
        REQUIRE(counter);
        CHECK(counter->event.amount == want[r]);
    }
    rig.fight.restart();
    CHECK(rig.fight.readStacks() == 0);
    CHECK_FALSE(rig.fight.readCards().dealt());
    CHECK(rig.fight.readFoe() == kArenaNobody);
}

TEST_CASE("arena Read: a read move that is parried brings the counter at once, without poise on the open enemy") {
    Rig rig(sturdyFighter(), {readKind()});
    rig.put(0, 1.5f, 0.f);
    for (int t = 1; t <= 40; ++t) {
        if (t == 19) rig.in.guard = true;
        if (rig.fight.readOpen() && !rig.fight.readCards().picked())
            rig.in.read = static_cast<std::int8_t>(rig.fight.readCards().correct());
        rig.tick();
    }
    const Logged* parried = rig.find(ArenaEventType::Parried);
    const Logged* counter = rig.find(ArenaEventType::ReadCounter);
    REQUIRE(parried);
    REQUIRE(counter);
    CHECK(counter->tick == parried->tick);
    CHECK(counter->event.amount == 15);
    CHECK(rig.fight.enemyHp(0) == 85);
    CHECK(rig.fight.enemyPoise(0) == doctest::Approx(30.f)); // Open: not on its feet, no poise lost
    CHECK(rig.out().enemies[0].brain == ArenaBrain::Open);
    CHECK(rig.count(ArenaEventType::ReadCounter) == 1);
}

TEST_CASE("arena Read: the autopilot picks the right card a moment into the startup") {
    Rig rig(sturdyFighter(), {readKind()});
    int picks = 0;
    for (int t = 0; t < 600; ++t) {
        const ArenaInput pilot = rig.fight.autopilot();
        if (pilot.read >= 0) {
            ++picks;
            CHECK(rig.fight.readOpen());
            CHECK(static_cast<std::size_t>(pilot.read) == rig.fight.readCards().correct());
            CHECK(rig.out().enemies[rig.fight.readFoe()].stateTime >= 0.2f - 1e-4f);
        }
        rig.in.read = pilot.read;
        rig.tick();
    }
    CHECK(picks > 3);
    CHECK(rig.fight.stats().correctReads == picks);
    CHECK(rig.count(ArenaEventType::Misread) == 0);
}

TEST_CASE("arena Read loader: tells by name, an unknown one and two alike are errors; the read numbers") {
    {
        ArenaTable t;
        const std::string err = loadError(
            tableJson(R"([ { "id": "default", "readPower": 2, "readStackPower": 0.5, "readPoise": 12, "readWrongPoise": 7 } ])",
                      R"([ { "id": "dummy", "moves": [ { "tell": "sweep" }, { "tell": "slam", "weight": 0 }, { "weight": 0 } ] } ])"),
            &t);
        CHECK(err.empty());
        REQUIRE(t.enemies.size() == 1);
        CHECK(t.enemies[0].moves[0].tell == ArenaTell::Sweep);
        CHECK(t.enemies[0].moves[1].tell == ArenaTell::Slam);
        CHECK(t.enemies[0].moves[2].tell == ArenaTell::Strike); // the default
        CHECK(t.fighters[0].readPower == doctest::Approx(2.f));
        CHECK(t.fighters[0].readStackPower == doctest::Approx(0.5f));
        CHECK(t.fighters[0].readPoise == doctest::Approx(12.f));
        CHECK(t.fighters[0].readWrongPoise == doctest::Approx(7.f));
    }
    CHECK(contains(loadError(tableJson(kFightersOk, R"([ { "id": "dummy", "moves": [ { "tell": "wobble" } ] } ])")),
                   "moves[0].tell: expected one of"));
    CHECK(contains(loadError(tableJson(kFightersOk, R"([ { "id": "dummy", "moves": [ { "tell": 3 } ] } ])")),
                   "moves[0].tell: expected one of"));
    CHECK(contains(loadError(tableJson(kFightersOk,
                                       R"([ { "id": "dummy", "moves": [ { "tell": "sweep" }, { "tell": "sweep" } ] } ])")),
                   "moves[1].tell: the same as moves[0]'s"));
    CHECK(contains(loadError(tableJson(R"([ { "id": "default", "readPower": 101 } ])", kEnemiesOk)), "readPower"));
    CHECK(contains(loadError(tableJson(R"([ { "id": "default", "readWrongPoise": -1 } ])", kEnemiesOk)), "readWrongPoise"));
    // Moves that name no tell all read "strike": allowed, they just deal no cards on their own.
    CHECK(loadError(tableJson(kFightersOk, R"([ { "id": "dummy", "moves": [ {}, { "weight": 0 } ] } ])")).empty());
    for (std::size_t t = 0; t < kArenaTellCount; ++t) CHECK(std::string_view(arenaTellName(static_cast<ArenaTell>(t))).size() > 0);
}

TEST_CASE("arena Read: every shipped enemy names a different tell for each move, and the built-in one too") {
    const ArenaTable table = loadShipped();
    REQUIRE_FALSE(table.enemies.empty());
    const auto distinct = [](const ArenaEnemyKind& k) {
        for (std::size_t m = 1; m < k.moveCount; ++m)
            for (std::size_t o = 0; o < m; ++o)
                if (k.moves[m].tell == k.moves[o].tell) return false;
        return true;
    };
    for (std::size_t e = 0; e < table.enemies.size(); ++e) {
        CHECK_MESSAGE(distinct(table.enemies[e]), table.enemyIds[e]);
        CHECK(table.enemies[e].moveCount >= 2); // so a lone one is dealt at least two cards
    }
    CHECK(distinct(defaultArenaDef().enemies[0]));
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
    int releases = 0;
    int readsDealt = 0;
    int reads = 0;
    int correctReads = 0;
    int readCounters = 0;
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
    o.releases = fight.stats().releases;
    o.readsDealt = fight.stats().readsDealt;
    o.reads = fight.stats().reads;
    o.correctReads = fight.stats().correctReads;
    o.readCounters = fight.stats().readCounters;
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
        // The autopilot reads for the partner, always right; nobody at the controls reads nothing.
        CHECK_MESSAGE(win.readsDealt > 0, "Error: no Read cards dealt in " << name << " seed " << seed);
        CHECK(win.correctReads == win.reads);
        CHECK(win.correctReads > 0);
        CHECK(win.readCounters > 0);
        CHECK(lose.reads == 0);
        CHECK(lose.readsDealt > 0);
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
                                                  << win.evades << ", releases " << win.releases << ", reads " << win.correctReads << "/" << win.readsDealt << ", read counters " << win.readCounters << ", hits taken " << win.hitsTaken << "), idle lost in " << lose.time
                                                  << " s");
    }
    MESSAGE("arena balance: " << name << " over 8 seeds: slowest win " << worstWin << " s, lowest hp left " << lowestHp
                                         << ", slowest idle loss " << worstLoss << " s (seed 1: " << firstWin << " / " << firstLoss << ")");
}

// A fight of the shipped enemies, by id, played with one fighter's kit.
ArenaDef shippedFight(const ArenaTable& table, std::initializer_list<const char*> ids, std::uint8_t attackers,
                      const char* fighterId = "default") {
    ArenaDef def;
    def.fighter = table.fighter(fighterId);
    def.enemyCount = 0;
    for (const char* id : ids)
        for (std::size_t e = 0; e < table.enemyIds.size(); ++e)
            if (table.enemyIds[e] == id) {
                def.enemyKind[def.enemyCount] = static_cast<std::uint8_t>(e);
                def.enemies[def.enemyCount++] = table.enemies[e];
            }
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
    checkBalance("a brute and a spitter, attackers 2", shippedFight(table, {"brute", "spitter"}, 2));
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

TEST_CASE("arena balance: every lead's kit plays the default's fights") {
    const ArenaTable table = loadShipped();
    REQUIRE(table.fighterIds.size() >= 4); // default, devin, duy, third
    for (std::size_t f = 1; f < table.fighterIds.size(); ++f) {
        const std::string& id = table.fighterIds[f];
        ArenaDef fallback = table.fallback;
        fallback.fighter = table.fighters[f];
        checkBalance(id + ": shipped default fight", fallback);
        checkBalance(id + ": three kinds, attackers 1", shippedFight(table, {"brute", "skirmisher", "breaker"}, 1, id.c_str()));
        checkBalance(id + ": a brute and a spitter, attackers 2", shippedFight(table, {"brute", "spitter"}, 2, id.c_str()));
        for (std::size_t e = 0; e < table.enemyIds.size(); ++e) {
            ArenaDef def;
            def.fighter = table.fighters[f];
            def.enemyCount = 1;
            def.enemies[0] = table.enemies[e];
            def.attackers = 1;
            checkBalance(id + ": " + table.enemyIds[e], def);
        }
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

TEST_CASE("arena: a whole autopilot fight that stores Load and releases it allocates nothing") {
    const ArenaTable table = loadShipped();
    const ArenaDef def = shippedFight(table, {"brute", "skirmisher", "breaker"}, 1);
    const ArenaRing ring = arenaRingAt(Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, def.ringRadius, ArenaRing{Vec2{0.f, 0.f}, 36.f});
    ArenaFight fight;
    ArenaBodies bodies;
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    fight.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
    bodies.player = fight.playerStart();
    for (std::size_t i = 0; i < fight.enemyCount(); ++i) bodies.enemies[i] = fight.enemyStart(i);
    int fullDeals = 0, deals = 0;
    while (!fight.over() && fight.time() < 120.f) {
        const ArenaOutput& out = fight.tick(kDt, fight.autopilot(), bodies);
        for (const ArenaEvent& e : out.events) {
            if (e.type != ArenaEventType::CardsDealt) continue;
            ++deals;
            if (e.amount == static_cast<int>(kReadCards)) ++fullDeals;
        }
        arenaStep(fight, bodies, kDt);
    }
    const auto allocs = ftcore_test::allocStats().count - before;
    if (counting) CHECK(allocs == 0); // every Read deal and pick included
    INFO("load stored " << fight.stats().loadStored << ", load " << fight.load() << ", time " << fight.time());
    CHECK(fight.phase() == ArenaPhase::Victory);
    CHECK(fight.stats().loadStored > 0); // the fight went through the Load and the Release
    CHECK(fight.stats().releases > 0);
    CHECK(deals == fight.stats().readsDealt);
    CHECK(deals > 5);
    CHECK(fullDeals > 0); // three kinds alive: three cards (fewer only once two of them are down)
    CHECK(fight.stats().correctReads > 0);
}

// ---- the arrow combos stay in the Turns battle ----

TEST_CASE("arena fence: a whole autopilot fight and an idle one build no arrow combo") {
    const std::uint64_t before = comboSequencesBuilt();
    const Outcome won = playOut(defaultArenaDef(), 2, true);
    const Outcome lost = playOut(defaultArenaDef(), 2, false);
    CHECK(won.phase == ArenaPhase::Victory);
    CHECK(lost.phase == ArenaPhase::Defeat);
    CHECK(comboSequencesBuilt() == before);
}

// ---- log ----

namespace {

// Plays def to its end under the autopilot (or nobody), then formats its result line.
std::string resultLineOf(const ArenaDef& def, const ArenaTable& table, bool autopilot, const ArenaLogIds& ids, std::size_t cap = kArenaResultLineMax) {
    const ArenaRing ring = arenaRingAt(Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, def.ringRadius, ArenaRing{Vec2{0.f, 0.f}, 36.f});
    ArenaFight fight;
    fight.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, ids.seed);
    ArenaBodies bodies;
    bodies.player = fight.playerStart();
    for (std::size_t i = 0; i < fight.enemyCount(); ++i) bodies.enemies[i] = fight.enemyStart(i);
    while (!fight.over() && fight.time() < 120.f) {
        fight.tick(kDt, autopilot ? fight.autopilot() : ArenaInput{}, bodies);
        arenaStep(fight, bodies, kDt);
    }
    std::array<char, kArenaResultLineMax> buf{};
    const std::size_t n = arenaResultLine(buf.data(), cap, fight, table, ids);
    CHECK(n < cap);
    CHECK(buf[n] == '\0');
    return std::string(buf.data(), n);
}

} // namespace

TEST_CASE("arena log: one line per fight, ids and counts only, like the beat screen's") {
    const ArenaTable table = loadShipped();
    ArenaLogIds ids;
    ids.zone = "sand_ring";
    ids.kit = "devin";
    ids.pilot = "autoplay";
    ids.seed = 3;
    const ArenaDef wave = shippedFight(table, {"brute", "skirmisher", "breaker"}, 1, "devin");
    const std::string won = resultLineOf(wave, table, true, ids);
    MESSAGE(won);
    CHECK(won.rfind("Arena: VICTORY at standalone beat 0 step 0 after ", 0) == 0);
    CHECK(contains(won, " s, zone sand_ring, kit devin, enemies brute+skirmisher+breaker, autoplay seed 3, hp "));
    CHECK(contains(won, "/150, "));
    CHECK(contains(won, " reads right, dealt "));
    CHECK(contains(won, ", 0 restarts."));
    CHECK(won.back() == '.');
    // A story step names its scene, beat and step; a lost fight says DEFEAT; a built-in slot says so.
    ids.scene = "SC-07";
    ids.beat = 4;
    ids.step = 2;
    ids.pilot = "idle";
    const std::string lost = resultLineOf(wave, table, false, ids);
    CHECK(lost.rfind("Arena: DEFEAT at SC-07 beat 4 step 2 after ", 0) == 0);
    CHECK(contains(lost, "idle seed 3, hp 0/150, 0 hits"));
    const std::string builtIn = resultLineOf(defaultArenaDef(), ArenaTable{}, true, ids);
    CHECK(contains(builtIn, ", enemies built-in, "));
    // An id with a space or a comma can't break the line apart; a fight still running is a TIMEOUT.
    ids.zone = "two words,here";
    ArenaFight fresh;
    fresh.begin(wave, arenaRingAt(Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 12.f, ArenaRing{Vec2{0.f, 0.f}, 36.f}), Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
    std::array<char, kArenaResultLineMax> buf{};
    const std::size_t n = arenaResultLine(buf.data(), buf.size(), fresh, table, ids);
    const std::string running(buf.data(), n);
    CHECK(running.rfind("Arena: TIMEOUT at SC-07 beat 4 step 2 after 0.0 s, zone two_words_here, kit devin, ", 0) == 0);
}

TEST_CASE("arena log: a short buffer is cut and zero-ended, and writing the line allocates nothing") {
    const ArenaTable table = loadShipped();
    const ArenaDef wave = shippedFight(table, {"brute", "skirmisher", "breaker"}, 1);
    ArenaLogIds ids;
    ids.zone = "sand_ring";
    ids.kit = "default";
    ids.pilot = "autoplay";
    ids.seed = 1;
    const std::string cut = resultLineOf(wave, table, true, ids, 20);
    CHECK(cut == "Arena: VICTORY at s");
    ArenaFight fight;
    fight.begin(wave, arenaRingAt(Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 12.f, ArenaRing{Vec2{0.f, 0.f}, 36.f}), Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
    char one[1] = {'x'};
    CHECK(arenaResultLine(one, 1, fight, table, ids) == 0);
    CHECK(one[0] == '\0');
    CHECK(arenaResultLine(nullptr, 0, fight, table, ids) == 0);
    std::array<char, kArenaResultLineMax> buf{};
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    const std::size_t n = arenaResultLine(buf.data(), buf.size(), fight, table, ids);
    const auto allocs = ftcore_test::allocStats().count - before;
    CHECK(n > 0);
    if (counting) CHECK(allocs == 0);
}

// ---- host hooks ----

namespace {

// Records every blow the fight asks about and rules on it: `ruling` for the blows `only` picks (kArenaNobody: every
// blow; kArenaPlayer: the player's; an enemy: that enemy's), none for the rest.
struct BlowRecorder {
    std::vector<ArenaBlow> blows;
    ArenaBlowRuling ruling;
    std::uint8_t only = kArenaNobody;

    static ArenaBlowRuling hook(void* user, const ArenaBlow& b) {
        BlowRecorder& r = *static_cast<BlowRecorder*>(user);
        r.blows.push_back(b);
        return r.only == kArenaNobody || r.only == b.source ? r.ruling : ArenaBlowRuling{};
    }
    // The first recorded blow of `type` from `source`, or nullptr.
    const ArenaBlow* find(ArenaEventType type, std::uint8_t source) const {
        for (const ArenaBlow& b : blows)
            if (b.type == type && b.source == source) return &b;
        return nullptr;
    }
};

ArenaBlowRuling ruling(float damage, float poise, bool veto = false) {
    ArenaBlowRuling r;
    r.damage = damage;
    r.poise = poise;
    r.veto = veto;
    return r;
}

constexpr float kForever = std::numeric_limits<float>::infinity();

// The tick parryRun's parry lands on.
int parryTick() {
    Rig probe(ruleFighter(), {hitterKind(swing(0.4f, 0.15f, 0.5f, 2.f, 20))});
    parryRun(probe, 40);
    const Logged* parried = probe.find(ArenaEventType::Parried);
    REQUIRE(parried);
    return parried->tick;
}

} // namespace

TEST_CASE("arena hooks: a hold cuts a live move at the next tick, lasts its seconds, and a release ends it") {
    const ArenaAttack bite = swing(0.4f, 0.15f, 0.5f, 2.f, 20);
    {   // Open for 0.5 s, asked 10 ticks into the bite's startup
        Rig rig(ruleFighter(), {hitterKind(bite)});
        rig.put(0, 1.5f, 0.f);
        rig.run(10);
        REQUIRE(rig.out().enemies[0].brain == ArenaBrain::Attack);
        REQUIRE(rig.out().enemies[0].token);
        CHECK(rig.fight.holdEnemy(0, ArenaBrain::Open, 0.5f));
        CHECK_FALSE(rig.fight.enemyHeld(0)); // it waits for the next tick ...
        rig.tick(0.f);                        // ... that advances
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Attack);
        rig.tick();
        CHECK(rig.fight.enemyHeld(0));
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Open);
        CHECK_FALSE(rig.out().enemies[0].token);
        const int started = rig.count(ArenaEventType::AttackStarted);
        rig.run(27); // 28 ticks held: 0.47 s
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Open);
        CHECK(rig.count(ArenaEventType::AttackStarted) == started);
        rig.run(3); // 31 ticks: 0.52 s
        CHECK(rig.out().enemies[0].brain != ArenaBrain::Open);
        CHECK_FALSE(rig.fight.enemyHeld(0));
        CHECK(rig.count(ArenaEventType::Hit) == 0); // the cut bite never landed
        CHECK(rig.fight.hp() == 100);
    }
    {   // Down until released
        Rig rig(ruleFighter(), {hitterKind(bite)});
        rig.put(0, 1.5f, 0.f);
        rig.run(10);
        CHECK(rig.fight.holdEnemy(0, ArenaBrain::Down, kForever));
        rig.tick();
        const int started = rig.count(ArenaEventType::AttackStarted);
        rig.runFor(5.f);
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Down);
        CHECK(rig.fight.enemyHeld(0));
        CHECK(rig.count(ArenaEventType::AttackStarted) == started);
        rig.fight.releaseEnemy(0);
        CHECK(rig.fight.enemyHeld(0)); // the release waits for the next tick too
        rig.tick();
        CHECK_FALSE(rig.fight.enemyHeld(0));
        CHECK(rig.out().enemies[0].brain != ArenaBrain::Down);
        CHECK(rig.fight.hp() == 100);
    }
    {   // before a tick the latest request wins
        Rig rig(ruleFighter(), {dummyKind()});
        CHECK(rig.fight.holdEnemy(0, ArenaBrain::Down, 1.f));
        rig.fight.releaseEnemy(0);
        rig.tick();
        CHECK(rig.out().enemies[0].brain != ArenaBrain::Down);
        rig.fight.releaseEnemy(0);
        CHECK(rig.fight.holdEnemy(0, ArenaBrain::Down, 1.f));
        rig.tick();
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Down);
    }
    {   // a release ends only a host's hold: a parry's Open runs its own openTime
        Rig rig(ruleFighter(), {hitterKind(bite)});
        parryRun(rig, 30);
        REQUIRE(rig.out().enemies[0].brain == ArenaBrain::Open);
        CHECK_FALSE(rig.fight.enemyHeld(0));
        rig.fight.releaseEnemy(0);
        rig.tick();
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Open);
    }
    {   // a staggered enemy can be held; holding again starts the hold over
        ArenaFighter f = ruleFighter();
        f.light[0].poiseDamage = 40.f;
        Rig rig(f, {dummyKind()});
        rig.in.light = true;
        rig.run(20);
        REQUIRE(rig.out().enemies[0].brain == ArenaBrain::Staggered);
        CHECK(rig.fight.holdEnemy(0, ArenaBrain::Down, 0.5f));
        rig.tick();
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Down);
        rig.run(20); // 21 ticks held
        CHECK(rig.fight.holdEnemy(0, ArenaBrain::Down, 0.5f));
        rig.run(25); // 25 ticks into the second: 46 since the first, which would have ended at 30
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Down);
        rig.run(10);
        CHECK(rig.out().enemies[0].brain != ArenaBrain::Down);
    }
    {   // a riposte on a held Open enemy ends the hold: Down for the kind's downTime
        Rig rig(ruleFighter(), {dummyKind()});
        CHECK(rig.fight.holdEnemy(0, ArenaBrain::Open, kForever));
        rig.tick();
        rig.in.light = true;
        CHECK(rig.tick().player.action == ArenaAction::Riposte);
        rig.run(12);
        CHECK(rig.fight.stats().ripostes == 1);
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Down);
        CHECK_FALSE(rig.fight.enemyHeld(0));
        rig.runFor(1.1f);
        CHECK(rig.out().enemies[0].brain != ArenaBrain::Down); // downTime 1 s
    }
    {   // asked from inside the blow hook, a hold waits for the next tick; a release from outside ends it
        struct Holder {
            ArenaFight* fight = nullptr;
            static ArenaBlowRuling hook(void* user, const ArenaBlow& b) {
                if (b.source == kArenaPlayer)
                    static_cast<Holder*>(user)->fight->holdEnemy(b.target, ArenaBrain::Down, kForever);
                return ArenaBlowRuling{};
            }
        };
        Rig rig(ruleFighter(), {dummyKind()});
        Holder holder;
        holder.fight = &rig.fight;
        rig.fight.setBlowHook(&Holder::hook, &holder);
        rig.in.light = true;
        for (int t = 0; t < 30 && !rig.find(ArenaEventType::Hit); ++t) rig.tick();
        REQUIRE(rig.find(ArenaEventType::Hit));
        CHECK(rig.out().enemies[0].brain != ArenaBrain::Down);
        rig.tick();
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Down);
        CHECK(rig.fight.enemyHeld(0));
        rig.fight.setBlowHook(nullptr, nullptr);
        rig.runFor(2.f); // past downTime
        CHECK(rig.fight.enemyHeld(0));
        rig.fight.releaseEnemy(0);
        rig.tick();
        CHECK_FALSE(rig.fight.enemyHeld(0));
        CHECK(rig.out().enemies[0].brain != ArenaBrain::Down);
    }
    {   // a hold asked for an enemy the same blow kills is dropped; the other enemy fights on
        struct Holder {
            ArenaFight* fight = nullptr;
            static ArenaBlowRuling hook(void* user, const ArenaBlow& b) {
                if (b.source == kArenaPlayer)
                    static_cast<Holder*>(user)->fight->holdEnemy(b.target, ArenaBrain::Open, 1.f);
                return ArenaBlowRuling{};
            }
        };
        ArenaEnemyKind frail = dummyKind();
        frail.hp = 5;
        Rig rig(ruleFighter(), {frail, dummyKind()});
        rig.put(1, -10.f, 0.f);
        Holder holder;
        holder.fight = &rig.fight;
        rig.fight.setBlowHook(&Holder::hook, &holder);
        rig.in.light = true;
        for (int t = 0; t < 30 && !rig.find(ArenaEventType::Hit); ++t) rig.tick();
        REQUIRE_FALSE(rig.fight.enemyAlive(0));
        rig.tick();
        CHECK_FALSE(rig.fight.enemyHeld(0));
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Dead);
        CHECK_FALSE(rig.fight.enemyHeld(1));
        CHECK(rig.fight.phase() == ArenaPhase::Fighting);
    }
}

TEST_CASE("arena hooks: a hold is refused for a missing or dead enemy, another brain, or no time") {
    Rig rig(ruleFighter(), {dummyKind()});
    CHECK_FALSE(rig.fight.holdEnemy(0, ArenaBrain::Staggered, 1.f));
    CHECK_FALSE(rig.fight.holdEnemy(0, ArenaBrain::Attack, 1.f));
    CHECK_FALSE(rig.fight.holdEnemy(0, ArenaBrain::Dead, 1.f));
    CHECK_FALSE(rig.fight.holdEnemy(0, ArenaBrain::Open, 0.f));
    CHECK_FALSE(rig.fight.holdEnemy(0, ArenaBrain::Open, -1.f));
    CHECK_FALSE(rig.fight.holdEnemy(0, ArenaBrain::Open, std::numeric_limits<float>::quiet_NaN()));
    CHECK_FALSE(rig.fight.holdEnemy(1, ArenaBrain::Open, 1.f)); // the fight has one enemy
    CHECK_FALSE(rig.fight.holdEnemy(kMaxArenaEnemies, ArenaBrain::Open, 1.f));
    rig.fight.releaseEnemy(kMaxArenaEnemies);
    rig.tick();
    CHECK(rig.out().enemies[0].brain != ArenaBrain::Open);
    CHECK(rig.out().enemies[0].brain != ArenaBrain::Down);
    CHECK_FALSE(rig.fight.enemyHeld(0));
    CHECK_FALSE(rig.fight.enemyHeld(kMaxArenaEnemies));

    ArenaEnemyKind frail = dummyKind();
    frail.hp = 5;
    Rig kill(ruleFighter(), {frail});
    kill.in.light = true;
    kill.run(20);
    REQUIRE_FALSE(kill.fight.enemyAlive(0));
    CHECK_FALSE(kill.fight.holdEnemy(0, ArenaBrain::Open, 1.f));
    CHECK_FALSE(kill.fight.enemyHeld(0));

    ArenaFight unstarted; // no enemies before begin()
    CHECK_FALSE(unstarted.holdEnemy(0, ArenaBrain::Open, 1.f));
    unstarted.setEnemyHpFloor(0, 5);
    CHECK(unstarted.enemyHpFloor(0) == 0);
}

TEST_CASE("arena hooks: the blow hook scales the player's blows or vetoes them before they apply") {
    ArenaFighter f = ruleFighter();
    f.light[0].poiseDamage = 10.f;
    {   // half the damage, twice the poise
        Rig rig(f, {dummyKind()});
        BlowRecorder rec;
        rec.ruling = ruling(0.5f, 2.f);
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        rig.in.light = true;
        rig.run(20);
        REQUIRE(rec.blows.size() == 1); // once per attack and target
        const ArenaBlow& b = rec.blows[0];
        CHECK(b.type == ArenaEventType::Hit);
        CHECK(b.source == kArenaPlayer);
        CHECK(b.target == 0);
        CHECK(b.move == 0);
        CHECK(b.damage == 10);
        CHECK(b.poise == doctest::Approx(10.f));
        CHECK(b.from.x == doctest::Approx(0.f));
        CHECK(b.from.y == doctest::Approx(0.f));
        CHECK(b.facing.x == doctest::Approx(1.f));
        CHECK(b.at.x == doctest::Approx(1.5f));
        const Logged* hit = rig.find(ArenaEventType::Hit);
        REQUIRE(hit);
        CHECK(b.time == doctest::Approx(static_cast<float>(hit->tick) * kDt).epsilon(1e-4));
        CHECK(hit->event.amount == 5);
        CHECK(rig.fight.enemyHp(0) == 95);
        CHECK(rig.fight.enemyPoise(0) == doctest::Approx(10.f)); // 30 - 2 x 10
        CHECK(rig.fight.stats().damageDealt == 5);
        CHECK(rig.fight.stats().hits == 1);
    }
    {   // vetoed: the attack is spent on that enemy and nothing else happens
        Rig rig(f, {dummyKind()});
        BlowRecorder rec;
        rec.ruling = ruling(1.f, 1.f, true);
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        rig.in.light = true;
        rig.run(40);
        CHECK(rec.blows.size() == 1); // the active part's later ticks don't ask again
        CHECK(rig.count(ArenaEventType::Hit) == 0);
        CHECK(rig.fight.enemyHp(0) == 100);
        CHECK(rig.fight.enemyPoise(0) == doctest::Approx(30.f));
        CHECK(rig.fight.stats().hits == 0);
        CHECK(rig.fight.stats().damageDealt == 0);
    }
    {   // NaN and negative scales count as 0: the blow lands for nothing
        Rig rig(f, {dummyKind()});
        BlowRecorder rec;
        rec.ruling = ruling(std::numeric_limits<float>::quiet_NaN(), -1.f);
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        rig.in.light = true;
        rig.run(20);
        const Logged* hit = rig.find(ArenaEventType::Hit);
        REQUIRE(hit);
        CHECK(hit->event.amount == 0);
        CHECK(rig.fight.enemyHp(0) == 100);
        CHECK(rig.fight.enemyPoise(0) == doctest::Approx(30.f));
    }
    {   // a huge scale takes what hp there is
        Rig rig(f, {dummyKind()});
        BlowRecorder rec;
        rec.ruling = ruling(1.0e30f, 1.f);
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        rig.in.light = true;
        rig.run(20);
        const Logged* hit = rig.find(ArenaEventType::Hit);
        REQUIRE(hit);
        CHECK(hit->event.amount == 100);
        CHECK_FALSE(rig.fight.enemyAlive(0));
        CHECK(rig.fight.phase() == ArenaPhase::Victory);
    }
    {   // a vetoed riposte leaves the enemy Open, and held
        Rig rig(f, {dummyKind()});
        BlowRecorder rec;
        rec.ruling = ruling(1.f, 1.f, true);
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        CHECK(rig.fight.holdEnemy(0, ArenaBrain::Open, kForever));
        rig.tick();
        rig.in.light = true;
        CHECK(rig.tick().player.action == ArenaAction::Riposte);
        rig.run(12);
        CHECK(rec.blows.size() == 1);
        CHECK(rig.fight.stats().ripostes == 0);
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Open);
        CHECK(rig.fight.enemyHeld(0));
    }
}

TEST_CASE("arena hooks: the blow hook scales an enemy's blow, landed, blocked or parried alike, or vetoes it") {
    ArenaAttack bite = swing(0.4f, 0.15f, 0.5f, 2.f, 20);
    bite.poiseDamage = 20.f;
    {   // landed: 1.5 x the damage, 2 x the poise (the player's 30 breaks)
        Rig rig(ruleFighter(), {hitterKind(bite)});
        rig.put(0, 1.5f, 0.f);
        BlowRecorder rec;
        rec.ruling = ruling(1.5f, 2.f);
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        rig.run(40);
        REQUIRE(rec.blows.size() == 1);
        const ArenaBlow& b = rec.blows[0];
        CHECK(b.type == ArenaEventType::Hit);
        CHECK(b.source == 0);
        CHECK(b.target == kArenaPlayer);
        CHECK(b.move == 0);
        CHECK(b.damage == 20);
        CHECK(b.poise == doctest::Approx(20.f));
        CHECK(b.from.x == doctest::Approx(1.5f));
        CHECK(b.at.x == doctest::Approx(0.f));
        CHECK(b.facing.x == doctest::Approx(-1.f));
        const Logged* hit = rig.find(ArenaEventType::Hit);
        REQUIRE(hit);
        CHECK(hit->event.amount == 30);
        CHECK(rig.fight.hp() == 70);
        CHECK(rig.fight.stats().damageTaken == 30);
        const Logged* broke = rig.find(ArenaEventType::Staggered);
        REQUIRE(broke);
        CHECK(broke->event.target == kArenaPlayer);
    }
    {   // scaled past anything: the player loses what hp it has, and a broken poise comes back whole
        Rig rig(ruleFighter(), {hitterKind(bite)});
        rig.put(0, 1.5f, 0.f);
        BlowRecorder rec;
        rec.ruling = ruling(1.0e30f, std::numeric_limits<float>::infinity());
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        rig.run(40);
        const Logged* hit = rig.find(ArenaEventType::Hit);
        REQUIRE(hit);
        CHECK(hit->event.amount == 100);
        CHECK(rig.fight.hp() == 0);
        CHECK(rig.fight.stats().damageTaken == 100);
        CHECK(rig.fight.phase() == ArenaPhase::Defeat);
    }
    {   // an endless poise scale breaks the player's poise, which comes back whole (no inf or NaN left behind)
        Rig rig(ruleFighter(), {hitterKind(bite)});
        rig.put(0, 1.5f, 0.f);
        BlowRecorder rec;
        rec.ruling = ruling(0.f, std::numeric_limits<float>::infinity());
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        rig.run(40);
        REQUIRE(rig.find(ArenaEventType::Staggered));
        CHECK(rig.fight.poise() == doctest::Approx(30.f));
    }
    {   // blocked: the chip is the ruled damage's, 30 x (1 - 0.75) = 7.5, rounded to 8
        Rig rig(ruleFighter(), {hitterKind(bite)});
        rig.put(0, 1.5f, 0.f);
        BlowRecorder rec;
        rec.ruling = ruling(1.5f, 1.f);
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        rig.in.guard = true; // pressed at once: the parry window is long gone by the bite
        rig.run(40);
        CHECK(rig.count(ArenaEventType::Blocked) == 1);
        CHECK(rig.fight.hp() == 92);
    }
    {   // parried: the Load is the ruled damage's
        Rig rig(ruleFighter(), {hitterKind(bite)});
        BlowRecorder rec;
        rec.ruling = ruling(1.5f, 1.f);
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        parryRun(rig, 40);
        CHECK(rig.count(ArenaEventType::Parried) == 1);
        const Logged* stored = rig.find(ArenaEventType::LoadStored);
        REQUIRE(stored);
        CHECK(stored->event.amount == 30);
        CHECK(rig.fight.load() == 30);
    }
    {   // vetoed, unguarded: no hit, no poise lost, no stagger
        Rig rig(ruleFighter(), {hitterKind(bite)});
        rig.put(0, 1.5f, 0.f);
        BlowRecorder rec;
        rec.ruling = ruling(1.f, 1.f, true);
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        rig.run(40);
        CHECK(rec.blows.size() == 1);
        CHECK(rig.count(ArenaEventType::Hit) == 0);
        CHECK(rig.count(ArenaEventType::Staggered) == 0);
        CHECK(rig.fight.poise() == doctest::Approx(30.f));
        CHECK(rig.fight.hp() == 100);
        CHECK(rig.fight.stats().damageTaken == 0);
    }
    {   // vetoed against a raised guard: no block, no chip, no stamina
        Rig rig(ruleFighter(), {hitterKind(bite)});
        rig.put(0, 1.5f, 0.f);
        BlowRecorder rec;
        rec.ruling = ruling(1.f, 1.f, true);
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        rig.in.guard = true;
        rig.run(40);
        CHECK(rec.blows.size() == 1);
        CHECK(rig.count(ArenaEventType::Blocked) == 0);
        CHECK(rig.fight.hp() == 100);
        CHECK(rig.fight.stamina() == doctest::Approx(100.f));
        CHECK(rig.fight.stats().blocks == 0);
    }
    {   // vetoed inside the parry window: no parry, no block, no hit, and it is spent
        Rig rig(ruleFighter(), {hitterKind(bite)});
        BlowRecorder rec;
        rec.ruling = ruling(1.f, 1.f, true);
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        parryRun(rig, 40);
        CHECK(rec.blows.size() == 1);
        CHECK(rig.count(ArenaEventType::Parried) == 0);
        CHECK(rig.count(ArenaEventType::Blocked) == 0);
        CHECK(rig.count(ArenaEventType::Hit) == 0);
        CHECK(rig.fight.hp() == 100);
        CHECK(rig.fight.stamina() == doctest::Approx(100.f));
        CHECK(rig.fight.load() == 0);
        CHECK(rig.fight.stats().parries == 0);
        CHECK(rig.fight.stats().hitsTaken == 0);
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Attack); // the bite plays out
    }
    {   // the dodge's frames come first: an evaded blow never reaches the hook
        Rig rig(ruleFighter(), {hitterKind(bite)});
        rig.put(0, 1.5f, 0.f);
        BlowRecorder rec;
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        rig.run(15); // pressed at 0.25 s: the i-frames cover the bite's 0.4 .. 0.55 s
        rig.in.dodge = true;
        rig.tick();
        rig.run(55);
        CHECK(rig.count(ArenaEventType::Evaded) == 1);
        CHECK(rec.blows.empty());
    }
}

TEST_CASE("arena hooks: the partner's counters go through the blow hook too") {
    const int p = parryTick();
    const ArenaEnemyKind hitter = hitterKind(swing(0.4f, 0.15f, 0.5f, 2.f, 20));
    {   // the paired counter: twice the damage; the kind's poise is its poise damage
        Rig rig(ruleFighter(), {hitter});
        BlowRecorder rec;
        rec.only = kArenaPlayer;
        rec.ruling = ruling(2.f, 1.f);
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        parryRun(rig, p + 15, p);
        const ArenaBlow* b = rec.find(ArenaEventType::CounterStagger, kArenaPlayer);
        REQUIRE(b);
        CHECK(b->target == 0);
        CHECK(b->damage == 30);
        CHECK(b->poise == doctest::Approx(30.f));
        const Logged* counter = rig.find(ArenaEventType::CounterStagger);
        REQUIRE(counter);
        CHECK(counter->event.amount == 60);
        CHECK(rig.fight.enemyHp(0) == 40);
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Staggered);
    }
    {   // a poise scale below 1 keeps the open enemy from staggering: the whole poise or nothing
        Rig rig(ruleFighter(), {hitter});
        BlowRecorder rec;
        rec.only = kArenaPlayer;
        rec.ruling = ruling(1.f, 0.9f);
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        parryRun(rig, p + 15, p);
        REQUIRE(rig.find(ArenaEventType::CounterStagger));
        CHECK(rig.find(ArenaEventType::CounterStagger)->event.amount == 30);
        CHECK(rig.count(ArenaEventType::Staggered) == 0);
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Open);
    }
    {   // vetoed: the pair is spent for nothing
        Rig rig(ruleFighter(), {hitter});
        BlowRecorder rec;
        rec.only = kArenaPlayer;
        rec.ruling = ruling(1.f, 1.f, true);
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        parryRun(rig, p + 15, p);
        CHECK(rec.find(ArenaEventType::CounterStagger, kArenaPlayer));
        CHECK(rig.count(ArenaEventType::CounterStagger) == 0);
        CHECK(rig.fight.stats().counterStaggers == 0);
        CHECK(rig.fight.enemyHp(0) == 100);
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Open);
    }
    // The Read counter: a read move parried brings it at once (15 at one stack, readPoise 10).
    const auto readRun = [](Rig& rig) {
        rig.put(0, 1.5f, 0.f);
        for (int t = 1; t <= 40; ++t) {
            if (t == 19) rig.in.guard = true;
            if (rig.fight.readOpen() && !rig.fight.readCards().picked())
                rig.in.read = static_cast<std::int8_t>(rig.fight.readCards().correct());
            rig.tick();
        }
        REQUIRE(rig.find(ArenaEventType::Parried));
    };
    {
        Rig rig(sturdyFighter(), {readKind()});
        BlowRecorder rec;
        rec.only = kArenaPlayer;
        rec.ruling = ruling(2.f, 1.f);
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        readRun(rig);
        const ArenaBlow* b = rec.find(ArenaEventType::ReadCounter, kArenaPlayer);
        REQUIRE(b);
        CHECK(b->target == 0);
        CHECK(b->move == 1); // the stacks
        CHECK(b->damage == 15);
        CHECK(b->poise == doctest::Approx(10.f));
        const Logged* counter = rig.find(ArenaEventType::ReadCounter);
        REQUIRE(counter);
        CHECK(counter->event.amount == 30);
        CHECK(rig.fight.enemyHp(0) == 70);
        CHECK(rig.fight.stats().readDamage == 30);
        CHECK(rig.fight.stats().readCounters == 1);
    }
    {   // vetoed: no counter, no count
        Rig rig(sturdyFighter(), {readKind()});
        BlowRecorder rec;
        rec.only = kArenaPlayer;
        rec.ruling = ruling(1.f, 1.f, true);
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        readRun(rig);
        CHECK(rec.find(ArenaEventType::ReadCounter, kArenaPlayer));
        CHECK(rig.count(ArenaEventType::ReadCounter) == 0);
        CHECK(rig.fight.stats().readCounters == 0);
        CHECK(rig.fight.stats().readDamage == 0);
        CHECK(rig.fight.enemyHp(0) == 100);
    }
}

TEST_CASE("arena hooks: an hp floor keeps an enemy alive through any blow until the host lowers it") {
    {
        ArenaEnemyKind k = dummyKind();
        k.hp = 15;
        Rig rig(ruleFighter(), {k});
        rig.fight.setEnemyHpFloor(0, 10);
        CHECK(rig.fight.enemyHpFloor(0) == 10);
        rig.in.light = true;
        rig.run(40);
        REQUIRE(rig.find(ArenaEventType::Hit));
        CHECK(rig.find(ArenaEventType::Hit)->event.amount == 5);
        CHECK(rig.fight.enemyHp(0) == 10);
        rig.in.light = true;
        rig.run(40);
        REQUIRE(rig.find(ArenaEventType::Hit, 1));
        CHECK(rig.find(ArenaEventType::Hit, 1)->event.amount == 0); // it connects, for nothing
        CHECK(rig.fight.enemyHp(0) == 10);
        CHECK(rig.fight.enemyAlive(0));
        CHECK(rig.count(ArenaEventType::Downed) == 0);
        CHECK(rig.fight.phase() == ArenaPhase::Fighting);
        rig.fight.setEnemyHpFloor(0, 0);
        rig.in.light = true;
        rig.run(40);
        CHECK_FALSE(rig.fight.enemyAlive(0));
        CHECK(rig.count(ArenaEventType::Downed) == 1);
        CHECK(rig.fight.phase() == ArenaPhase::Victory);
    }
    {   // a floor above the hp lets nothing through; below 0 counts as 0; a missing enemy has none
        Rig rig(ruleFighter(), {dummyKind()});
        rig.fight.setEnemyHpFloor(0, 500);
        rig.in.light = true;
        rig.run(40);
        CHECK(rig.fight.enemyHp(0) == 100);
        rig.fight.setEnemyHpFloor(0, -3);
        CHECK(rig.fight.enemyHpFloor(0) == 0);
        rig.fight.setEnemyHpFloor(1, 50);
        CHECK(rig.fight.enemyHpFloor(1) == 0);
    }
    {   // a riposte stops at the floor, and still downs the open enemy
        Rig rig(ruleFighter(), {dummyKind()});
        rig.fight.setEnemyHpFloor(0, 90);
        CHECK(rig.fight.holdEnemy(0, ArenaBrain::Open, kForever));
        rig.tick();
        rig.in.light = true;
        CHECK(rig.tick().player.action == ArenaAction::Riposte);
        rig.run(12);
        REQUIRE(rig.find(ArenaEventType::Hit));
        CHECK(rig.find(ArenaEventType::Hit)->event.amount == 10);
        CHECK(rig.fight.enemyHp(0) == 90);
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Down);
    }
    {   // the partner's counter stops at the floor too
        Rig rig(ruleFighter(), {hitterKind(swing(0.4f, 0.15f, 0.5f, 2.f, 20))});
        rig.fight.setEnemyHpFloor(0, 90);
        const int p = parryTick();
        parryRun(rig, p + 15, p);
        REQUIRE(rig.find(ArenaEventType::CounterStagger));
        CHECK(rig.find(ArenaEventType::CounterStagger)->event.amount == 10);
        CHECK(rig.fight.enemyHp(0) == 90);
    }
}

TEST_CASE("arena hooks: restart and begin clear holds and floors, and keep the blow hook") {
    const ArenaAttack bite = swing(0.4f, 0.15f, 0.5f, 2.f, 20);
    Rig rig(ruleFighter(), {hitterKind(bite)});
    rig.put(0, 1.5f, 0.f);
    BlowRecorder rec;
    rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
    rig.fight.setEnemyHpFloor(0, 50);
    CHECK(rig.fight.holdEnemy(0, ArenaBrain::Down, kForever));
    rig.tick();
    REQUIRE(rig.fight.enemyHeld(0));
    rig.fight.restart();
    CHECK_FALSE(rig.fight.enemyHeld(0));
    CHECK(rig.fight.enemyHpFloor(0) == 0);
    CHECK(rig.fight.holdEnemy(0, ArenaBrain::Down, kForever)); // asked, then a restart: forgotten
    rig.fight.restart();
    rig.run(40);
    CHECK(rig.out().enemies[0].brain != ArenaBrain::Down);
    CHECK(rec.blows.size() == 1); // the bite: the hook stayed
    rig.fight.setEnemyHpFloor(0, 50);
    CHECK(rig.fight.holdEnemy(0, ArenaBrain::Down, kForever));
    rig.tick();
    REQUIRE(rig.fight.enemyHeld(0));
    CHECK(rig.fight.holdEnemy(0, ArenaBrain::Open, kForever)); // a pending one too
    rig.fight.begin(rig.def, rig.fight.ring(), Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
    CHECK_FALSE(rig.fight.enemyHeld(0));
    CHECK(rig.fight.enemyHpFloor(0) == 0);
    rig.run(40);
    CHECK(rig.out().enemies[0].brain != ArenaBrain::Open);
    CHECK(rig.out().enemies[0].brain != ArenaBrain::Down);
    CHECK(rec.blows.size() == 2);
    rig.fight.setBlowHook(nullptr, &rec); // none: every blow as it is
    rig.fight.restart();
    rig.run(40);
    CHECK(rec.blows.size() == 2);
    CHECK(rig.count(ArenaEventType::Hit) == 3);
}

TEST_CASE("arena: a kind of kMaxArenaMoves moves plays a string of all of them") {
    // A move of n impacts is n chained moves, so a long string needs more than the old 8.
    static_assert(kMaxArenaMoves >= 20, "room for the chained impacts of an arena boss");
    ArenaEnemyKind k = hitterKind(swing(0.05f, 0.05f, 0.f, 2.f, 1));
    k.moveCount = static_cast<std::uint8_t>(kMaxArenaMoves);
    for (std::size_t m = 0; m < kMaxArenaMoves; ++m) {
        k.moves[m] = k.moves[0];
        k.moves[m].chain = m + 1 < kMaxArenaMoves ? static_cast<std::int8_t>(m + 1) : static_cast<std::int8_t>(-1);
        k.moves[m].weight = m == 0 ? 1.f : 0.f;
    }
    Rig rig(sturdyFighter(), {k});
    rig.put(0, 1.5f, 0.f);
    BlowRecorder rec;
    rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
    rig.runFor(2.5f);
    for (std::size_t m = 0; m < kMaxArenaMoves; ++m) {
        const Logged* s = rig.find(ArenaEventType::AttackStarted, static_cast<int>(m));
        REQUIRE(s);
        CHECK(s->event.source == 0);
        CHECK(s->event.move == m);
    }
    CHECK(rig.fight.stats().hitsTaken >= static_cast<int>(kMaxArenaMoves));
    REQUIRE(rec.blows.size() >= kMaxArenaMoves); // the hook sees every move of the string, the last one too
    for (std::size_t m = 0; m < kMaxArenaMoves; ++m) CHECK(rec.blows[m].move == m);
}

TEST_CASE("arena hooks: a whole fight with a blow hook, a floor and holds allocates nothing") {
    struct Counter {
        int blows = 0;
        int vetoes = 0;
        static ArenaBlowRuling hook(void* user, const ArenaBlow&) {
            Counter& c = *static_cast<Counter*>(user);
            ArenaBlowRuling r;
            r.damage = 1.25f; // every blow scaled, every seventh vetoed
            r.poise = 0.8f;
            r.veto = ++c.blows % 7 == 0;
            if (r.veto) ++c.vetoes;
            return r;
        }
    };
    const ArenaTable table = loadShipped();
    const ArenaDef def = shippedFight(table, {"brute"}, 1);
    const ArenaRing ring = arenaRingAt(Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, def.ringRadius, ArenaRing{Vec2{0.f, 0.f}, 36.f});
    ArenaFight fight;
    ArenaBodies bodies;
    Counter counter;
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    fight.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 3);
    fight.setBlowHook(&Counter::hook, &counter);
    fight.setEnemyHpFloor(0, 1);
    bodies.player = fight.playerStart();
    bodies.enemies[0] = fight.enemyStart(0);
    int ticks = 0, holds = 0;
    while (!fight.over() && ticks < 7200) {
        if (ticks % 600 == 300 && fight.holdEnemy(0, ArenaBrain::Down, 1.f)) ++holds;
        if (ticks == 1800) fight.setEnemyHpFloor(0, 0);
        fight.tick(kDt, fight.autopilot(), bodies);
        arenaStep(fight, bodies, kDt);
        ++ticks;
    }
    const auto allocs = ftcore_test::allocStats().count - before;
    if (counting) CHECK(allocs == 0);
    CHECK(counter.blows > 0);
    CHECK(counter.vetoes > 0);
    CHECK(holds > 0);
    CHECK(fight.phase() == ArenaPhase::Victory);
}

TEST_CASE("arena hooks: the hook may set a floor for the blow it rules, release, swap itself, and can't re-enter") {
    struct Host {
        ArenaFight* fight = nullptr;
        int floor = -1;          // set this floor while ruling the player's blow (-1: none)
        bool release = false;    // release enemy 0 while ruling it
        bool reenter = false;    // try tick, begin and restart from inside
        int calls = 0;
        static ArenaBlowRuling hook(void* user, const ArenaBlow& b) {
            Host& h = *static_cast<Host*>(user);
            ++h.calls;
            if (b.source != kArenaPlayer) return ArenaBlowRuling{};
            if (h.floor >= 0) h.fight->setEnemyHpFloor(b.target, h.floor);
            if (h.release) h.fight->releaseEnemy(b.target);
            if (h.reenter) {
                const float before = h.fight->time();
                h.fight->tick(kDt, ArenaInput{}, ArenaBodies{});
                h.fight->restart();
                h.fight->begin(h.fight->def(), h.fight->ring(), Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 9);
                CHECK(h.fight->time() == before);
            }
            return ArenaBlowRuling{};
        }
        static ArenaBlowRuling swap(void* user, const ArenaBlow&) {
            Host& h = *static_cast<Host*>(user);
            ++h.calls;
            h.fight->setBlowHook(nullptr, nullptr);
            return ruling(0.f, 1.f);
        }
    };
    {   // a floor set while ruling counts for that blow
        Rig rig(ruleFighter(), {dummyKind()});
        Host host;
        host.fight = &rig.fight;
        host.floor = 95;
        rig.fight.setBlowHook(&Host::hook, &host);
        rig.in.light = true;
        rig.run(20);
        REQUIRE(rig.find(ArenaEventType::Hit));
        CHECK(rig.find(ArenaEventType::Hit)->event.amount == 5);
        CHECK(rig.fight.enemyHp(0) == 95);
    }
    {   // a release asked while ruling ends the hold at the next tick
        ArenaFighter f = ruleFighter();
        f.light[0].damage = 1;
        Rig rig(f, {dummyKind()});
        Host host;
        host.fight = &rig.fight;
        host.release = true;
        rig.fight.setBlowHook(&Host::hook, &host);
        CHECK(rig.fight.holdEnemy(0, ArenaBrain::Down, kForever));
        rig.tick();
        rig.in.light = true;
        for (int t = 0; t < 30 && !rig.find(ArenaEventType::Hit); ++t) rig.tick();
        REQUIRE(rig.find(ArenaEventType::Hit));
        CHECK(rig.fight.enemyHeld(0));
        rig.tick();
        CHECK_FALSE(rig.fight.enemyHeld(0));
        CHECK(rig.out().enemies[0].brain != ArenaBrain::Down);
    }
    {   // tick, begin and restart from inside do nothing: the fight goes on as it was
        Rig rig(ruleFighter(), {dummyKind()});
        Host host;
        host.fight = &rig.fight;
        host.reenter = true;
        rig.fight.setBlowHook(&Host::hook, &host);
        rig.in.light = true;
        rig.run(20);
        CHECK(host.calls == 1);
        REQUIRE(rig.find(ArenaEventType::Hit));
        CHECK(rig.fight.enemyHp(0) == 90);
        CHECK(rig.fight.stats().restarts == 0);
        CHECK(rig.fight.time() == doctest::Approx(20.f * kDt));
    }
    {   // a hook that clears itself still rules the blow it was asked about, and no blow after
        Rig rig(ruleFighter(), {dummyKind()});
        Host host;
        host.fight = &rig.fight;
        rig.fight.setBlowHook(&Host::swap, &host);
        rig.in.light = true;
        rig.run(40);
        rig.in.light = true;
        rig.run(40);
        CHECK(host.calls == 1);
        REQUIRE(rig.count(ArenaEventType::Hit) == 2);
        CHECK(rig.find(ArenaEventType::Hit, 0)->event.amount == 0);
        CHECK(rig.find(ArenaEventType::Hit, 1)->event.amount >= 10); // as it is
    }
}

TEST_CASE("arena hooks: one attack on two enemies asks in index order, and a veto uses up a target slot") {
    {
        Rig rig(ruleFighter(), {dummyKind(), dummyKind()});
        BlowRecorder rec;
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        rig.in.light = true;
        rig.run(20);
        REQUIRE(rec.blows.size() == 2);
        CHECK(rec.blows[0].target == 0);
        CHECK(rec.blows[1].target == 1);
        CHECK(rec.blows[0].time == rec.blows[1].time);
    }
    {
        ArenaFighter f = ruleFighter();
        f.light[0].maxTargets = 1;
        Rig rig(f, {dummyKind(), dummyKind()});
        BlowRecorder rec;
        rec.ruling = ruling(1.f, 1.f, true);
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        rig.in.light = true;
        rig.run(40);
        REQUIRE(rec.blows.size() == 1); // the vetoed blow took the one slot: enemy 1 is never asked about
        CHECK(rec.blows[0].target == 0);
        CHECK(rig.count(ArenaEventType::Hit) == 0);
        CHECK(rig.fight.enemyHp(1) == 100);
    }
}

TEST_CASE("arena hooks: a hold that cuts a read move brings the partner's counter, under the floor, and may kill") {
    const auto readThenHold = [](Rig& rig) {
        rig.put(0, 1.5f, 0.f);
        REQUIRE(untilDealt(rig));
        rig.in.read = static_cast<std::int8_t>(rig.fight.readCards().correct());
        rig.tick();
        REQUIRE(rig.find(ArenaEventType::Read));
        REQUIRE(rig.fight.readOpen() == false);
        REQUIRE(rig.out().enemies[0].brain == ArenaBrain::Attack);
        REQUIRE(rig.count(ArenaEventType::Hit) == 0);
        CHECK(rig.fight.holdEnemy(0, ArenaBrain::Open, 1.f));
        rig.tick();
    };
    {   // 15 at one stack, cut to 5 by a floor of 95; the enemy stays held
        Rig rig(sturdyFighter(), {readKind()});
        rig.fight.setEnemyHpFloor(0, 95);
        readThenHold(rig);
        const Logged* counter = rig.find(ArenaEventType::ReadCounter);
        REQUIRE(counter);
        CHECK(counter->event.amount == 5);
        CHECK(rig.fight.enemyHp(0) == 95);
        CHECK(rig.fight.stats().readDamage == 5);
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Open);
        CHECK(rig.fight.enemyHeld(0));
    }
    {   // the counter kills a frail enemy: no hold on the dead
        ArenaEnemyKind frail = readKind();
        frail.hp = 10;
        Rig rig(sturdyFighter(), {frail});
        readThenHold(rig);
        REQUIRE(rig.find(ArenaEventType::ReadCounter));
        CHECK_FALSE(rig.fight.enemyAlive(0));
        CHECK_FALSE(rig.fight.enemyHeld(0));
        CHECK(rig.fight.phase() == ArenaPhase::Victory);
    }
}

TEST_CASE("arena hooks: a counter-stagger ends a host's hold, and a ruled parry's Load stops at the gauge") {
    const ArenaAttack bite = swing(0.4f, 0.15f, 0.5f, 2.f, 20);
    const int p = parryTick();
    {
        Rig rig(ruleFighter(), {hitterKind(bite)});
        parryRun(rig, p);
        REQUIRE(rig.out().enemies[0].brain == ArenaBrain::Open);
        CHECK(rig.fight.holdEnemy(0, ArenaBrain::Open, kForever));
        rig.in.pair = true; // the hold applies first, then the pair lands in the same tick
        rig.tick();
        REQUIRE(rig.find(ArenaEventType::CounterStagger));
        CHECK(rig.out().enemies[0].brain == ArenaBrain::Staggered);
        CHECK_FALSE(rig.fight.enemyHeld(0));
    }
    {   // a parried blow scaled past any share fills the gauge, no more
        Rig rig(ruleFighter(), {hitterKind(bite)});
        BlowRecorder rec;
        rec.ruling = ruling(1.0e30f, 1.f);
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        parryRun(rig, 40);
        REQUIRE(rig.count(ArenaEventType::Parried) == 1);
        CHECK(rig.fight.load() == kMaxLoad);
        CHECK(rig.fight.overloaded());
    }
}

TEST_CASE("arena hooks: a hold the hook asks while holds apply waits a tick, and a copy made in the hook is live") {
    {   // enemy 0's read move is cut by a hold; ruling on the counter, the hook holds enemy 1: that waits a tick
        struct Host {
            ArenaFight* fight = nullptr;
            static ArenaBlowRuling hook(void* user, const ArenaBlow& b) {
                if (b.type == ArenaEventType::ReadCounter)
                    static_cast<Host*>(user)->fight->holdEnemy(1, ArenaBrain::Down, kForever);
                return ArenaBlowRuling{};
            }
        };
        Rig rig(sturdyFighter(), {readKind(), dummyKind()});
        rig.put(0, 1.5f, 0.f);
        rig.put(1, -10.f, 0.f);
        Host host;
        host.fight = &rig.fight;
        rig.fight.setBlowHook(&Host::hook, &host);
        REQUIRE(untilDealt(rig));
        REQUIRE(rig.fight.readFoe() == 0);
        rig.in.read = static_cast<std::int8_t>(rig.fight.readCards().correct());
        rig.tick();
        CHECK(rig.fight.holdEnemy(0, ArenaBrain::Open, kForever));
        rig.tick();
        REQUIRE(rig.find(ArenaEventType::ReadCounter));
        CHECK(rig.fight.enemyHeld(0));
        CHECK_FALSE(rig.fight.enemyHeld(1));
        CHECK(rig.out().enemies[1].brain != ArenaBrain::Down);
        rig.tick();
        CHECK(rig.fight.enemyHeld(1));
        CHECK(rig.out().enemies[1].brain == ArenaBrain::Down);
    }
    {   // a fight copied or assigned inside the hook is not stuck in it
        struct Host {
            ArenaFight* fight = nullptr;
            ArenaFight* assigned = nullptr;
            bool copyTicked = false;
            static ArenaBlowRuling hook(void* user, const ArenaBlow& b) {
                Host& h = *static_cast<Host*>(user);
                if (b.source != kArenaPlayer) return ArenaBlowRuling{};
                ArenaFight copy(*h.fight);
                const float before = copy.time();
                copy.tick(kDt, ArenaInput{}, ArenaBodies{});
                h.copyTicked = copy.time() > before;
                *h.assigned = *h.fight;
                return ArenaBlowRuling{};
            }
        };
        Rig rig(ruleFighter(), {dummyKind()});
        ArenaFight assigned;
        Host host;
        host.fight = &rig.fight;
        host.assigned = &assigned;
        rig.fight.setBlowHook(&Host::hook, &host);
        rig.in.light = true;
        rig.run(20);
        REQUIRE(rig.find(ArenaEventType::Hit));
        CHECK(host.copyTicked);
        assigned.setBlowHook(nullptr, nullptr);
        const float before = assigned.time();
        assigned.tick(kDt, ArenaInput{}, ArenaBodies{});
        CHECK(assigned.time() > before);
        assigned.restart();
        CHECK(assigned.stats().restarts == 1);
    }
}

// The bigger move cap is fixed storage: these ceilings make the next bump a deliberate one (MSVC x64 now: ArenaDef
// 17824 bytes, ArenaFight 23592).
static_assert(sizeof(ArenaDef) <= 20 * 1024, "ArenaDef grew: check kMaxArenaMoves and the enemy kinds");
static_assert(sizeof(ArenaFight) <= 28 * 1024, "ArenaFight grew: check kMaxArenaMoves and the Read pool");

TEST_CASE("arena hooks: a Release's spent Load is in the poise the hook sees and scales; no hold after a Defeat") {
    {   // 34 Load spent: 34 poise damage on the upright enemy, halved by the ruling
        ArenaEnemyKind upright = dummyKind();
        upright.poise = 40.f;
        Rig rig(ruleFighter(), {hitterKind(swing(0.4f, 0.15f, 0.5f, 2.f, 34)), upright});
        rig.put(1, 0.f, 1.5f);
        BlowRecorder rec;
        rec.only = kArenaPlayer;
        rec.ruling = ruling(1.f, 0.5f);
        rig.fight.setBlowHook(&BlowRecorder::hook, &rec);
        parryRun(rig, 40);
        REQUIRE(rig.fight.load() == 34);
        rig.in.guard = false;
        rig.tick();
        rig.in.release = true;
        rig.tick();
        REQUIRE(rig.find(ArenaEventType::Released));
        rig.runFor(0.3f);
        const ArenaBlow* b = nullptr;
        for (const ArenaBlow& x : rec.blows)
            if (x.source == kArenaPlayer && x.target == 1) b = &x;
        REQUIRE(b);
        CHECK(b->poise == doctest::Approx(34.f));
        CHECK(rig.fight.enemyPoise(1) == doctest::Approx(23.f)); // 40 - 34 x 0.5
    }
    {   // Defeat: the enemy lives, but the fight is over
        Rig rig(ruleFighter(), {hitterKind(swing(0.4f, 0.15f, 0.5f, 2.f, 200))});
        rig.put(0, 1.5f, 0.f);
        rig.run(40);
        REQUIRE(rig.fight.phase() == ArenaPhase::Defeat);
        REQUIRE(rig.fight.enemyAlive(0));
        CHECK_FALSE(rig.fight.holdEnemy(0, ArenaBrain::Open, 1.f));
    }
}
