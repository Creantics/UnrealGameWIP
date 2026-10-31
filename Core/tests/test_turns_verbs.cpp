// The Turns battle's verbs beyond the arrow combos: poise and stagger.
#include "ftcore/Battle.h"
#include "ftcore/Encounter.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <string>
#include <vector>

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

Combatant lead(int speed = 5) {
    Combatant c;
    c.name = "Lead";
    c.stats = {100, 20, 0, speed, 10};
    c.hp = 100;
    c.skills = {{"Strike", 0, 1, Targeting::SingleEnemy, 1.0f, 4, 1.0f}};
    return c;
}

Combatant foe(int poise, int speed = 50) {
    Combatant c;
    c.name = "Foe";
    c.stats = {500, 10, 0, speed, 10};
    c.stats.maxPoise = poise;
    c.hp = 500;
    c.skills = {{"Claw", 0, 0, Targeting::SingleEnemy, 1.0f, 4, 2.0f}};
    return c;
}

BattleTuning tuning() {
    BattleTuning t;
    t.leadIn = 0.5;
    t.recovery = 0.5;
    return t;
}

int count(const std::vector<BattleEvent>& events, EventType type) {
    int n = 0;
    for (const BattleEvent& e : events) n += e.type == type;
    return n;
}

// Steps to the next enemy turn, collecting the events on the way; false if none came.
bool toEnemyAttack(Battle& b, double& now, std::vector<BattleEvent>& seen) {
    for (int i = 0; i < 2000 && b.phase() != Phase::EnemySkill; ++i) {
        now += 0.05;
        b.update(now);
        for (const BattleEvent& e : b.drainEvents()) seen.push_back(e);
    }
    return b.phase() == Phase::EnemySkill;
}

// Parries every hit of the current enemy attack at its ideal time.
void parryAll(Battle& b, std::vector<BattleEvent>& seen) {
    while (b.phase() == Phase::EnemySkill && !b.defence().complete()) {
        const DefenceTimeline& d = b.defence();
        b.defend(DefenceInput::Parry, d.idealPress(d.nextIndex(), DefenceInput::Parry));
    }
    for (const BattleEvent& e : b.drainEvents()) seen.push_back(e);
}

} // namespace

TEST_CASE("poise: counters wear it down, and at 0 the enemy is staggered and skips its next turn") {
    Battle b({lead()}, {foe(25)}, tuning());
    REQUIRE(b.combatant(1).poise == 25);
    double now = 0.0;
    b.start(now);
    std::vector<BattleEvent> seen;
    for (const BattleEvent& e : b.drainEvents()) seen.push_back(e);

    // Two counters: 25 - 10 - 10 = 5, still standing.
    for (int i = 0; i < 2; ++i) {
        REQUIRE(toEnemyAttack(b, now, seen));
        parryAll(b, seen);
    }
    CHECK(b.combatant(1).poise == 5);
    CHECK_FALSE(b.combatant(1).staggered);
    CHECK(count(seen, EventType::PoiseLost) == 2);
    CHECK(count(seen, EventType::Staggered) == 0);

    // The third takes the last 5 and breaks it.
    seen.clear();
    REQUIRE(toEnemyAttack(b, now, seen));
    parryAll(b, seen);
    CHECK(b.combatant(1).poise == 0);
    CHECK(b.combatant(1).staggered);
    REQUIRE(count(seen, EventType::PoiseLost) == 1);
    for (const BattleEvent& e : seen)
        if (e.type == EventType::PoiseLost) {
            CHECK(e.amount == 5);
            CHECK(e.source == 0);
            CHECK(e.target == 1);
        }
    CHECK(count(seen, EventType::Staggered) == 1);
    CHECK(b.stats().staggers == 1);

    // Its next turn passes: Steadied with the poise restored, no attack; the turn after, it attacks.
    seen.clear();
    REQUIRE(toEnemyAttack(b, now, seen));
    int steadied = 0;
    for (const BattleEvent& e : seen)
        if (e.type == EventType::Steadied) {
            ++steadied;
            CHECK(e.source == 1);
            CHECK(e.amount == 25);
        }
    CHECK(steadied == 1);
    CHECK(count(seen, EventType::SkillStarted) == 1); // only the attack this loop stopped on
    CHECK(count(seen, EventType::TurnStarted) == 2);  // the skipped turn and this one
    CHECK(b.combatant(1).poise == 25);
    CHECK_FALSE(b.combatant(1).staggered);
}

TEST_CASE("poise: an enemy without poise is never staggered") {
    Battle b({lead()}, {foe(0)}, tuning());
    double now = 0.0;
    b.start(now);
    std::vector<BattleEvent> seen;
    for (int i = 0; i < 4; ++i) {
        REQUIRE(toEnemyAttack(b, now, seen));
        parryAll(b, seen);
    }
    CHECK(count(seen, EventType::Counter) == 4);
    CHECK(count(seen, EventType::PoiseLost) == 0);
    CHECK(count(seen, EventType::Staggered) == 0);
    CHECK(b.stats().staggers == 0);
}

TEST_CASE("poise: counterPoise sets what a counter costs, 0 for nothing") {
    BattleTuning t = tuning();
    t.counterPoise = 30;
    Battle b({lead()}, {foe(30)}, t);
    double now = 0.0;
    b.start(now);
    std::vector<BattleEvent> seen;
    REQUIRE(toEnemyAttack(b, now, seen));
    parryAll(b, seen);
    CHECK(b.combatant(1).staggered);

    t.counterPoise = 0;
    Battle none({lead()}, {foe(30)}, t);
    now = 0.0;
    none.start(now);
    seen.clear();
    REQUIRE(toEnemyAttack(none, now, seen));
    parryAll(none, seen);
    CHECK(none.combatant(1).poise == 30);
    CHECK(count(seen, EventType::PoiseLost) == 0);
}

TEST_CASE("poise: the enemy data takes an optional stats.poise") {
    EnemyRoster roster;
    std::string err;
    std::vector<std::string> warnings;
    const auto load = [&](const char* stats) {
        const std::string json = std::string(R"({"enemies":[{"id":"pup","stats":)") + stats +
                                 R"(,"skills":[{"id":"snap","power":1.0}]}]})";
        err.clear();
        warnings.clear();
        return importEnemyRoster(json, roster, &err, &warnings);
    };
    REQUIRE_MESSAGE(load(R"({"maxHp":50,"attack":9,"defense":2,"speed":6,"technique":7,"poise":20})"), err);
    CHECK(warnings.empty());
    CHECK(roster.enemies[0].stats.maxPoise == 20);
    std::vector<Combatant> made;
    makeEnemies(roster, EncounterLineup{.enemies = {0}, .tier = 4}, made);
    REQUIRE(made.size() == 1);
    CHECK(made[0].stats.maxPoise == 20); // tiers leave poise alone

    REQUIRE_MESSAGE(load(R"({"maxHp":50,"attack":9,"defense":2,"speed":6,"technique":7})"), err);
    CHECK(roster.enemies[0].stats.maxPoise == 0);

    CHECK_FALSE(load(R"({"maxHp":50,"poise":-1})"));
    CHECK(err.find("enemies[0].stats.poise") != std::string::npos);
    CHECK_FALSE(load(R"({"maxHp":50,"poise":1001})"));
    CHECK_FALSE(load(R"({"maxHp":50,"poise":"high"})"));
}
