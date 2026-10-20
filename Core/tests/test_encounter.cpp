#include "ftcore/BattleDemo.h"
#include "ftcore/Encounter.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr const char* kRoster = R"J({
  "version": 1,
  "enemies": [
    { "id": "brute", "stats": { "maxHp": 100, "attack": 10 },
      "skills": [ { "id": "hit", "power": 1.0, "notes": 3, "difficulty": 1.0 } ] },
    { "id": "shade", "name": "Shade", "stats": { "maxHp": 50, "speed": 20 },
      "skills": [ { "id": "zap", "targeting": "all", "apCost": 1, "power": 2.0, "notes": 4, "difficulty": 3.0 } ] }
  ]
})J";

constexpr const char* kTable = R"J({
  "version": 1,
  "default": { "enemies": ["brute", "shade"] },
  "encounters": [
    { "scene": "SC-T1", "beat": 3, "enemies": ["shade"] },
    { "scene": "SC-T1", "beat": 3, "step": 2, "enemies": ["brute", "brute", "brute"] }
  ]
})J";

bool sameSkill(const Skill& a, const Skill& b) {
    return a.name == b.name && a.apCost == b.apCost && a.apGain == b.apGain && a.targeting == b.targeting &&
           a.power == b.power && a.notes == b.notes && a.difficulty == b.difficulty;
}

} // namespace

TEST_CASE("enemy roster and encounter table load a fixture") {
    EnemyRoster roster;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importEnemyRoster(kRoster, roster, &err, &warnings), err);
    CHECK(warnings.empty());
    REQUIRE(roster.enemies.size() == 2);
    CHECK(roster.find("shade") == 1);
    CHECK(roster.find("nobody") == -1);
    const EnemyDef& brute = roster.enemies[0];
    CHECK(brute.name == "brute"); // the id when no name is given
    CHECK(brute.stats.maxHp == 100);
    CHECK(brute.stats.defense == Stats{}.defense); // defaults
    const EnemyDef& shade = roster.enemies[1];
    REQUIRE(shade.skills.size() == 1);
    CHECK(shade.skillIds[0] == "zap");
    CHECK(shade.skills[0].targeting == Targeting::AllEnemies);
    CHECK(shade.skills[0].apCost == 1);

    EncounterTable table;
    REQUIRE_MESSAGE(importEncounterTable(kTable, roster, table, &err, &warnings), err);
    CHECK(warnings.empty());
    CHECK(table.fallback.enemies == std::vector<std::uint32_t>{0, 1});
    CHECK(table.find("SC-T1", 3, 0).enemies == std::vector<std::uint32_t>{1});
    CHECK(table.find("SC-T1", 3, 2).enemies.size() == 3);
    CHECK(&table.find("SC-T1", 3, 1) == &table.fallback);
    CHECK(&table.find("SC-T2", 3, 0) == &table.fallback);
    CHECK(table.findEntry("SC-T1", 4, 0) == nullptr);

    std::vector<Combatant> enemies;
    makeEnemies(roster, table.find("SC-T1", 3, 2), enemies);
    REQUIRE(enemies.size() == 3);
    CHECK(enemies[0].team == Team::Enemies);
    CHECK(enemies[0].hp == 100);
}

TEST_CASE("encounter lookups do not allocate") {
    EnemyRoster roster;
    EncounterTable table;
    REQUIRE(importEnemyRoster(kRoster, roster));
    REQUIRE(importEncounterTable(kTable, roster, table));
    if (!ftcore_test::allocHookInstalled()) return;
    const auto before = ftcore_test::allocStats().count;
    std::size_t n = 0;
    for (std::uint32_t step = 0; step < 4; ++step) n += table.find("SC-T1", 3, step).enemies.size();
    n += static_cast<std::size_t>(roster.find("shade"));
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(n > 0);
}

TEST_CASE("bad enemy and encounter files are refused with a reason") {
    EnemyRoster roster;
    std::string err;
    CHECK_FALSE(importEnemyRoster("{", roster, &err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(importEnemyRoster(R"({"enemies": []})", roster, &err));
    CHECK_FALSE(importEnemyRoster(R"({"enemies": [{"id": "Bad Id", "stats": {}, "skills": [{"id": "a"}]}]})", roster, &err));
    CHECK(err.find("enemies[0].id") != std::string::npos);
    CHECK_FALSE(importEnemyRoster(
        R"({"enemies": [{"id": "a", "stats": {}, "skills": [{"id": "s"}]}, {"id": "a", "stats": {}, "skills": [{"id": "s"}]}]})",
        roster, &err));
    CHECK(err.find("duplicate") != std::string::npos);
    CHECK_FALSE(importEnemyRoster(R"({"enemies": [{"id": "a", "stats": {"maxHp": 0}, "skills": [{"id": "s"}]}]})", roster, &err));
    CHECK_FALSE(importEnemyRoster(R"({"enemies": [{"id": "a", "stats": {}, "skills": [{"id": "s", "targeting": "row"}]}]})",
                                  roster, &err));
    CHECK(roster.enemies.empty());
    std::vector<std::string> warnings;
    CHECK(importEnemyRoster(R"({"enemies": [{"id": "a", "stats": {}, "skills": [{"id": "s"}], "extra": 1}]})", roster,
                            &err, &warnings));
    CHECK(warnings.size() == 1);

    REQUIRE(importEnemyRoster(kRoster, roster));
    EncounterTable table;
    CHECK_FALSE(importEncounterTable(R"({"encounters": []})", roster, table, &err)); // no default
    CHECK_FALSE(importEncounterTable(R"({"default": {"enemies": []}})", roster, table, &err));
    CHECK_FALSE(importEncounterTable(R"({"default": {"enemies": ["ghost"]}})", roster, table, &err));
    CHECK(err.find("ghost") != std::string::npos);
    CHECK_FALSE(importEncounterTable(
        R"({"default": {"enemies": ["brute"]}, "encounters": [{"scene": "S", "beat": 1, "enemies": ["brute"]}, {"scene": "S", "beat": 1, "step": 0, "enemies": ["shade"]}]})",
        roster, table, &err));
    CHECK(err.find("duplicate") != std::string::npos);
    CHECK_FALSE(importEncounterTable(R"({"default": {"enemies": ["brute"]}, "encounters": [{"scene": "S", "enemies": ["brute"]}]})",
                                     roster, table, &err)); // no beat
    CHECK(table.fallback.enemies.empty());
    CHECK(importEncounterTable(R"({"default": {"enemies": ["brute"]}})", roster, table, &err)); // no list is fine
    CHECK(table.entries.empty());
}

TEST_CASE("Data/Battle loads and its default line-up is the demo's") {
    std::string enemiesText, encountersText;
    if (!ftcore_test::readProjectData("Battle/enemies.json", enemiesText) ||
        !ftcore_test::readProjectData("Battle/encounters.json", encountersText)) {
        FAIL("Data/Battle/enemies.json or encounters.json not found");
        return;
    }
    EnemyRoster roster;
    EncounterTable table;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importEnemyRoster(enemiesText, roster, &err, &warnings), err);
    REQUIRE_MESSAGE(importEncounterTable(encountersText, roster, table, &err, &warnings), err);
    CHECK(warnings.empty());

    std::vector<Combatant> enemies;
    makeEnemies(roster, table.fallback, enemies);
    const std::vector<Combatant> demo = demo::enemies();
    REQUIRE(enemies.size() == demo.size());
    for (std::size_t i = 0; i < demo.size(); ++i) {
        CAPTURE(i);
        CHECK(enemies[i].name == demo[i].name);
        CHECK(enemies[i].hp == demo[i].hp);
        CHECK(enemies[i].stats.maxHp == demo[i].stats.maxHp);
        CHECK(enemies[i].stats.attack == demo[i].stats.attack);
        CHECK(enemies[i].stats.defense == demo[i].stats.defense);
        CHECK(enemies[i].stats.speed == demo[i].stats.speed);
        CHECK(enemies[i].stats.technique == demo[i].stats.technique);
        REQUIRE(enemies[i].skills.size() == demo[i].skills.size());
        for (std::size_t k = 0; k < demo[i].skills.size(); ++k) CHECK(sameSkill(enemies[i].skills[k], demo[i].skills[k]));
    }

    // Every listed entry resolves to a real line-up.
    for (const EncounterEntry& e : table.entries) {
        CHECK_FALSE(e.lineup.enemies.empty());
        CHECK(&table.find(e.scene, e.beat, e.step) == &e.lineup);
    }
}
