#include "ftcore/ArenaFight.h"
#include "ftcore/ArenaZone.h"
#include "ftcore/BattleDemo.h"
#include "ftcore/Dialogue.h"
#include "ftcore/Encounter.h"
#include "ftcore/GameplayPlan.h"
#include "ftcore/Progression.h"
#include "ftcore/StoryRunner.h"

#include "AllocCounter.h"
#include "RealStory.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cstdio>
#include <initializer_list>
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
           a.power == b.power && a.notes == b.notes && a.difficulty == b.difficulty && a.tellId == b.tellId &&
           sameTimeline(a.timeline, b.timeline);
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

TEST_CASE("skill hits load as the attack timeline; skills without them get the default hit") {
    constexpr const char* kHits = R"J({
      "enemies": [ { "id": "a", "stats": {},
        "skills": [ { "id": "legacy" },
                    { "id": "combo", "hits": [ { "at": 0.8 },
                                               { "at": 1.5, "tell": 0.5, "telegraph": 1.2, "weight": 2, "jumpable": true } ] } ] } ]
    })J";
    EnemyRoster roster;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importEnemyRoster(kHits, roster, &err, &warnings), err);
    CHECK(warnings.empty());
    const std::vector<Skill>& skills = roster.enemies[0].skills;
    REQUIRE(skills.size() == 2);
    CHECK(sameTimeline(skills[0].timeline, AttackTimeline{})); // legacy: the default single hit
    REQUIRE(skills[0].timeline.size() == 1);
    CHECK(skills[0].timeline[0].at == kDefaultImpact);
    CHECK(skills[0].timeline[0].tell == kDefaultTell);
    const AttackTimeline& t = skills[1].timeline;
    REQUIRE(t.size() == 2);
    CHECK(t[0].at == 0.8);
    CHECK(t[0].tell == kDefaultTell);
    CHECK(t[0].telegraph == 0.0);
    CHECK(t[0].weight == 1.0f);
    CHECK_FALSE(t[0].jumpable);
    CHECK(t[1].at == 1.5);
    CHECK(t[1].tell == 0.5);
    CHECK(t[1].telegraph == 1.2);
    CHECK(t[1].weight == 2.0f);
    CHECK(t[1].jumpable);

    // Unknown hit keys warn; bad hits are refused with a reason.
    CHECK(importEnemyRoster(R"({"enemies": [{"id": "a", "stats": {}, "skills": [{"id": "s", "hits": [{"at": 1, "x": 0}]}]}]})",
                            roster, &err, &warnings));
    CHECK(warnings.size() == 1);
    const char* bad[] = {
        R"({"enemies": [{"id": "a", "stats": {}, "skills": [{"id": "s", "hits": []}]}]})",
        R"({"enemies": [{"id": "a", "stats": {}, "skills": [{"id": "s", "hits": {}}]}]})",
        R"({"enemies": [{"id": "a", "stats": {}, "skills": [{"id": "s", "hits": [{}]}]}]})",
        R"({"enemies": [{"id": "a", "stats": {}, "skills": [{"id": "s", "hits": [{"at": 0}]}]}]})",
        R"({"enemies": [{"id": "a", "stats": {}, "skills": [{"id": "s", "hits": [{"at": 31}]}]}]})",
        R"({"enemies": [{"id": "a", "stats": {}, "skills": [{"id": "s", "hits": [{"at": 1.0}, {"at": 1.0}]}]}]})",
        R"({"enemies": [{"id": "a", "stats": {}, "skills": [{"id": "s", "hits": [{"at": 0.3, "tell": 0.4}]}]}]})",
        R"({"enemies": [{"id": "a", "stats": {}, "skills": [{"id": "s", "hits": [{"at": 1, "tell": 0.3}]}]}]})", // under the 0.4 s floor
        R"({"enemies": [{"id": "a", "stats": {}, "skills": [{"id": "s", "hits": [{"at": 1, "tell": 0}]}]}]})",
        R"({"enemies": [{"id": "a", "stats": {}, "skills": [{"id": "s", "hits": [{"at": 1, "telegraph": 2}]}]}]})",
        R"({"enemies": [{"id": "a", "stats": {}, "skills": [{"id": "s", "hits": [{"at": 1, "weight": 0}]}]}]})",
        R"({"enemies": [{"id": "a", "stats": {}, "skills": [{"id": "s", "hits": [{"at": 1, "jumpable": 1}]}]}]})",
        R"({"enemies": [{"id": "a", "stats": {}, "skills": [{"id": "s", "hits": [{"at": 1}, {"at": 2}, {"at": 3}, {"at": 4}, {"at": 5}, {"at": 6}, {"at": 7}, {"at": 8}, {"at": 9}]}]}]})",
    };
    for (const char* json : bad) {
        CAPTURE(json);
        err.clear();
        CHECK_FALSE(importEnemyRoster(json, roster, &err));
        CHECK(err.find("hits") != std::string::npos);
    }
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

TEST_CASE("an encounter's combat tag says which system it plays; untagged fights are Turns") {
    EnemyRoster roster;
    REQUIRE(importEnemyRoster(kRoster, roster));
    EncounterTable table;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importEncounterTable(R"({"default": {"enemies": ["brute"]}, "encounters": [
        {"scene": "S", "beat": 1, "enemies": ["brute"]},
        {"scene": "S", "beat": 2, "combat": "arena", "enemies": ["brute"]},
        {"scene": "S", "beat": 3, "combat": "beat", "enemies": ["shade"]},
        {"scene": "S", "beat": 4, "combat": "rhythm", "enemies": ["shade"]}]})",
                                         roster, table, &err, &warnings),
                    err);
    CHECK(warnings.empty()); // `combat` is a known key
    CHECK(EncounterEntry{}.combat == CombatKind::Rhythm);

    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    const CombatKind plain = table.combatFor("S", 1, 0);
    const CombatKind arena = table.combatFor("S", 2, 0);
    const CombatKind beat = table.combatFor("S", 3, 0);
    const CombatKind spelled = table.combatFor("S", 4, 0);
    const CombatKind otherStep = table.combatFor("S", 2, 1); // not listed: the default line-up
    const CombatKind unknown = table.combatFor("NOPE", 2, 0);
    const CombatKind empty = table.combatFor("", 0, 0);
    if (counting) CHECK(ftcore_test::allocStats().count == before);
    CHECK(plain == CombatKind::Rhythm);
    CHECK(arena == CombatKind::Arena);
    CHECK(beat == CombatKind::Beat);
    CHECK(spelled == CombatKind::Rhythm);
    CHECK(otherStep == CombatKind::Rhythm);
    CHECK(unknown == CombatKind::Rhythm);
    CHECK(empty == CombatKind::Rhythm);
    CHECK(EncounterTable{}.combatFor("S", 2, 0) == CombatKind::Rhythm);

    // A bad value is a load error with its path; the spelling is exact.
    for (const char* bad : {R"("melee")", R"("Arena")", R"("turns")", R"("")", "3", "null"}) {
        CAPTURE(bad);
        const std::string text = std::string(R"({"default": {"enemies": ["brute"]}, "encounters": [{"scene": "S", "beat": 1, "combat": )") +
                                 bad + R"(, "enemies": ["brute"]}]})";
        CHECK_FALSE(importEncounterTable(text, roster, table, &err));
        CHECK(err.find("encounters[0].combat") != std::string::npos);
    }
    // The default line-up is always a Turns fight.
    CHECK_FALSE(importEncounterTable(R"({"default": {"enemies": ["brute"], "combat": "arena"}})", roster, table, &err));
    CHECK(err.find("default.combat") != std::string::npos);
}

TEST_CASE("an arena encounter may name its stage; only arena fights take one") {
    EnemyRoster roster;
    REQUIRE(importEnemyRoster(kRoster, roster));
    EncounterTable table;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importEncounterTable(R"({"default": {"enemies": ["brute"]}, "encounters": [
        {"scene": "S", "beat": 1, "combat": "arena", "stage": "sand_ring", "enemies": ["brute", "brute", "shade"]},
        {"scene": "S", "beat": 2, "combat": "arena", "enemies": ["brute"]}]})",
                                         roster, table, &err, &warnings),
                    err);
    CHECK(warnings.empty()); // `stage` is a known key
    REQUIRE(table.findEntry("S", 1, 0));
    CHECK(table.findEntry("S", 1, 0)->stage == "sand_ring");
    CHECK(table.findEntry("S", 1, 0)->lineup.enemies.size() == 3);
    CHECK(table.findEntry("S", 2, 0)->stage.empty()); // no stage: the fight plays in its world
    CHECK(EncounterEntry{}.stage.empty());

    for (const char* bad : {R"("combat": "arena", "stage": "Sand Ring")", R"("combat": "arena", "stage": "")",
                            R"("combat": "arena", "stage": 3)", R"("stage": "sand_ring")",
                            R"("combat": "beat", "stage": "sand_ring")"}) {
        CAPTURE(bad);
        const std::string text = std::string(R"({"default": {"enemies": ["brute"]}, "encounters": [{"scene": "S", "beat": 1, )") + bad +
                                 R"(, "enemies": ["brute"]}]})";
        CHECK_FALSE(importEncounterTable(text, roster, table, &err));
        CHECK(err.find("encounters[0].stage") != std::string::npos);
    }
}

TEST_CASE("enemy phases load with their patterns, thresholds and bonuses; bad ones are refused") {
    constexpr const char* kPhased = R"J({ "enemies": [ { "id": "boss", "stats": { "maxHp": 400, "attack": 20 },
      "skills": [ { "id": "a" }, { "id": "b" }, { "id": "c" } ],
      "phases": [ { "pattern": ["a", "b"] },
                  { "below": 0.5, "label": "Fury", "pattern": ["c", "a", "b"], "loop": 1, "attack": 25, "heal": 5 } ] } ] })J";
    EnemyRoster roster;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importEnemyRoster(kPhased, roster, &err, &warnings), err);
    CHECK(warnings.empty());
    const EnemyPhases& phases = roster.enemies[0].phases;
    REQUIRE(phases.size() == 2);
    CHECK(phases[0].hpBelow == 1.0f);
    CHECK(phases[0].length == 2);
    CHECK(phases[0].pattern[1] == 1);
    CHECK(phases[0].label().empty());
    CHECK(phases[1].hpBelow == 0.5f);
    CHECK(phases[1].label() == "Fury");
    CHECK(phases[1].length == 3);
    CHECK(phases[1].pattern[0] == 2);
    CHECK(phases[1].loopFrom == 1);
    CHECK(phases[1].attackPercent == 25);
    CHECK(phases[1].healPercent == 5);

    EncounterLineup lineup;
    lineup.enemies = {0};
    std::vector<Combatant> foes;
    makeEnemies(roster, lineup, foes);
    REQUIRE(foes.size() == 1);
    CHECK(foes[0].phases.size() == 2);
    CHECK(foes[0].phases[1].label() == "Fury");

    const auto refused = [&](const char* phasesJson, const char* where) {
        const std::string json = std::string(R"({"enemies": [{"id": "e", "stats": {}, "skills": [{"id": "a"}, {"id": "b"}], "phases": )") +
                                 phasesJson + "}]}";
        EnemyRoster r;
        std::string e;
        CAPTURE(phasesJson);
        CHECK_FALSE(importEnemyRoster(json, r, &e));
        CHECK(e.find(where) != std::string::npos);
    };
    refused("[]", ".phases");
    refused(R"([{"pattern": ["a"]}, {"pattern": ["b"]}])", "phases[1].below");                   // no threshold
    refused(R"([{"below": 0.5, "pattern": ["a"]}])", "phases[0].below");                         // first has one
    refused(R"([{"pattern": ["a"]}, {"below": 0.5, "pattern": ["b"]}, {"below": 0.6, "pattern": ["a"]}])", "phases[2].below");
    refused(R"([{"pattern": ["a"]}, {"below": 1.0, "pattern": ["b"]}])", "phases[1].below");
    refused(R"([{"pattern": ["zz"]}])", "phases[0].pattern[0]");                                 // unknown skill
    refused(R"([{"pattern": []}])", "phases[0].pattern");
    refused(R"([{"pattern": ["a", "b"], "loop": 2}])", "phases[0].loop");
    refused(R"([{"pattern": ["a"], "attack": 999}])", "phases[0].attack");
    refused(R"([{"pattern": ["a"], "heal": 101}])", "phases[0].heal");
    refused(R"([{"pattern": ["a"], "label": "a label much too long for the buffer"}])", "phases[0].label");
    refused(R"([{"pattern": ["a"]}, {"below": 0.8, "pattern": ["a"]}, {"below": 0.6, "pattern": ["a"]},
               {"below": 0.4, "pattern": ["a"]}, {"below": 0.2, "pattern": ["a"]}])", ".phases");
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

TEST_CASE("Data/Battle: the bosses' fights are tagged, the finals play Beat and every other fight is Turns") {
    std::string enemiesText, encountersText;
    if (!ftcore_test::readProjectData("Battle/enemies.json", enemiesText) ||
        !ftcore_test::readProjectData("Battle/encounters.json", encountersText)) {
        FAIL("Data/Battle/enemies.json or encounters.json not found");
        return;
    }
    EnemyRoster roster;
    EncounterTable table;
    std::string err;
    REQUIRE_MESSAGE(importEnemyRoster(enemiesText, roster, &err), err);
    REQUIRE_MESSAGE(importEncounterTable(encountersText, roster, table, &err), err);
    // A boss is an enemy of kBossHp base max HP or more; a fight with one is a boss fight.
    constexpr int kBossHp = 340;
    std::uint32_t kinds[kCombatKinds] = {};
    for (const EncounterEntry& e : table.entries) {
        CAPTURE(e.scene);
        CAPTURE(e.beat);
        CAPTURE(e.step);
        bool boss = false;
        for (const std::uint32_t i : e.lineup.enemies) boss = boss || roster.enemies[i].stats.maxHp >= kBossHp;
        const CombatKind kind = table.combatFor(e.scene, e.beat, e.step);
        CHECK(kind == e.combat);
        ++kinds[static_cast<std::size_t>(kind)];
        // Minor enemies are Turns, except a staged arena fight (a pack on an arena stage, 11-12 ruling).
        if (e.stage.empty()) CHECK(boss == (kind != CombatKind::Rhythm));
        else CHECK(kind == CombatKind::Arena);
        if (kind == CombatKind::Beat) CHECK(e.lineup.enemies.size() == 1); // a world's final: one boss
    }
    CHECK(kinds[static_cast<std::size_t>(CombatKind::Beat)] == 3); // one final per world
    CHECK(kinds[static_cast<std::size_t>(CombatKind::Arena)] == 15); // 13 in-world, 2 on stages (p04)
    CHECK(kinds[static_cast<std::size_t>(CombatKind::Rhythm)] == 24);
    CHECK(table.combatFor("SC-NOPE", 0, 0) == CombatKind::Rhythm); // an unlisted fight plays the default, Turns
}

namespace {

struct LoadedData {
    EnemyRoster roster;
    EncounterTable table;
    WeakPointTable weakPoints;
    bool ok = false;
};

LoadedData loadBattleData() {
    LoadedData d;
    std::string enemiesText, encountersText, err;
    if (!ftcore_test::readProjectData("Battle/enemies.json", enemiesText) ||
        !ftcore_test::readProjectData("Battle/encounters.json", encountersText))
        return d;
    d.ok = importEnemyRoster(enemiesText, d.roster, &err) &&
           importEncounterTable(encountersText, d.roster, d.table, &err);
    // Weak points are optional data: the gate plays with them when the file is there.
    std::string weakText;
    if (d.ok && ftcore_test::readProjectData("Battle/weakpoints.json", weakText))
        d.ok = importWeakPointTable(weakText, d.roster, d.weakPoints, &err);
    return d;
}

// The party level at each listed encounter (before its fight), for a party that has won every
// earlier one. The table lists the story's fights in script order (a real-story test checks it).
std::vector<int> levelsInTableOrder(const LoadedData& d) {
    std::vector<int> levels;
    PartyProgress progress;
    for (const EncounterEntry& e : d.table.entries) {
        levels.push_back(levelForXp(progress.xp));
        award(progress, encounterReward(d.roster, e.lineup));
    }
    return levels;
}

} // namespace

TEST_CASE("winning the story's fights in order levels the party about every four fights to the cap") {
    const LoadedData d = loadBattleData();
    REQUIRE(d.ok);
    for (const EnemyDef& e : d.roster.enemies) {
        CAPTURE(e.id);
        CHECK(e.reward.xp > 0);
        CHECK(e.reward.gold > 0);
    }
    PartyProgress progress;
    int prevLevel = 1, lastGain = 0, capAt = -1;
    const int n = static_cast<int>(d.table.entries.size());
    REQUIRE(n >= 30);
    for (int i = 0; i < n; ++i) {
        const AwardResult r = award(progress, encounterReward(d.roster, d.table.entries[static_cast<std::size_t>(i)].lineup));
        CAPTURE(i);
        CHECK(r.levelAfter - r.levelBefore <= 1); // never two levels from one fight
        if (r.levelAfter > prevLevel) {
            CHECK(i - lastGain <= 6); // no long stretch without a level
            CHECK(i - lastGain >= 2); // and no level every fight
            lastGain = i;
        }
        if (r.levelAfter == kMaxLevel && capAt < 0) capAt = i;
        prevLevel = r.levelAfter;
    }
    MESSAGE("PROGRESSION " << n << " fights: " << progress.xp << " xp, " << progress.gold << " gold, cap at fight " << capAt + 1);
    CHECK(levelForXp(progress.xp) == kMaxLevel);
    CHECK(capAt >= n - 6); // the cap comes near the finale, not before
    // The xp left over after the cap is small: the curve isn't far too cheap.
    CHECK(progress.xp - xpForLevel(kMaxLevel) < xpForLevel(kMaxLevel) / 4);
}

TEST_CASE("balance gate: every encounter is won with perfect presses and lost with none") {
    const LoadedData d = loadBattleData();
    REQUIRE(d.ok);
    REQUIRE_FALSE(d.table.entries.empty());

    // The default line-up first (at level 1), then every listed entry at its own tier, with the
    // party at the level it has reached by then.
    const std::vector<int> levels = levelsInTableOrder(d);
    for (std::size_t i = 0; i <= d.table.entries.size(); ++i) {
        const EncounterLineup& lineup = i == 0 ? d.table.fallback : d.table.entries[i - 1].lineup;
        const int level = i == 0 ? 1 : levels[i - 1];
        const std::string name = i == 0 ? std::string("default")
                                        : d.table.entries[i - 1].scene + " beat " + std::to_string(d.table.entries[i - 1].beat) +
                                              " step " + std::to_string(d.table.entries[i - 1].step);
        CAPTURE(name);
        CHECK(lineup.enemies.size() >= 1);
        CHECK(lineup.enemies.size() <= kMaxLineupEnemies);

        std::vector<Combatant> foes;
        makeEnemies(d.roster, lineup, foes);
        REQUIRE(foes.size() == lineup.enemies.size());
        applyWeakPoints(d.weakPoints, d.roster, lineup, foes);

        const demo::HeadlessResult win = demo::playHeadless(demo::party(level), foes, true);
        CHECK_FALSE(win.commandFailed);
        CHECK(win.finished);
        CHECK(win.outcome == Phase::Victory);
        CHECK(win.partyTurns >= 1);
        CHECK(win.presses > 0);
        MESSAGE(name << ": tier " << lineup.tier << ", level " << level << ", victory in " << win.endTime << " s, " << win.partyTurns
                     << " turns (" << win.aims << " aimed, " << win.weakHits << " on a weak point), " << win.enemyAttacks << " defences, party HP left " << win.partyHpLeft);

        // Long frames (130 ms of battle time per frame, as under time dilation) still win: the autopilot
        // presses at each note's own time before the update that would expire it.
        const demo::HeadlessResult coarse = demo::playHeadless(demo::party(level), foes, true, 0.13);
        CHECK(coarse.outcome == Phase::Victory);
        // Perfect dodges (jumps on jumpable hits) instead of parries: no counters, still a win.
        const demo::HeadlessResult dodged =
            demo::playHeadless(demo::party(level), foes, true, 1.0 / 120.0, 1200.0, DefenceInput::Dodge);
        CHECK(dodged.outcome == Phase::Victory);
        const demo::HeadlessResult dodgedCoarse =
            demo::playHeadless(demo::party(level), foes, true, 0.13, 1200.0, DefenceInput::Dodge);
        CHECK(dodgedCoarse.outcome == Phase::Victory);

        const demo::HeadlessResult loss = demo::playHeadless(demo::party(level), foes, false);
        CHECK_FALSE(loss.commandFailed);
        CHECK(loss.finished);
        CHECK(loss.outcome == Phase::Defeat);
        CHECK(loss.presses == 0);
    }
}

TEST_CASE("line-ups of up to five load, and five enemies run the whole balance gate") {
    const LoadedData d = loadBattleData();
    REQUIRE(d.ok);
    // The loader takes five and refuses six, naming the path.
    EncounterTable table;
    std::string err;
    REQUIRE_MESSAGE(importEncounterTable(R"({"default": {"enemies": ["wisp"]}, "encounters": [
        {"scene": "SC-T5", "beat": 1, "enemies": ["maw_pup", "maw_pup", "maw_pup", "maw_pup", "maw_pup"]}]})",
                                         d.roster, table, &err),
                    err);
    CHECK(table.find("SC-T5", 1, 0).enemies.size() == kMaxLineupEnemies);
    CHECK_FALSE(importEncounterTable(R"({"default": {"enemies": ["wisp"]}, "encounters": [
        {"scene": "SC-T6", "beat": 1, "enemies": ["wisp", "wisp", "wisp", "wisp", "wisp", "wisp"]}]})",
                                     d.roster, table, &err));
    CHECK(err.find("encounters[0].enemies: expected at most 5 enemies") != std::string::npos);
    CHECK_FALSE(importEncounterTable(R"({"default": {"enemies": ["wisp", "wisp", "wisp", "wisp", "wisp", "wisp"]}})", d.roster, table, &err));
    CHECK(err.find("default.enemies: expected at most 5 enemies") != std::string::npos);

    // A pack of five weak enemies at the start of the game, and a mixed five later on.
    struct Five {
        const char* ids[kMaxLineupEnemies];
        int tier, level;
    };
    for (const Five& five : {Five{{"maw_pup", "maw_pup", "maw_pup", "maw_pup", "maw_pup"}, 0, 1},
                             Five{{"wisp", "hound", "maw_pup", "rust_drone", "lurker"}, 2, 4}}) {
        CAPTURE(five.ids[0]);
        EncounterLineup lineup;
        lineup.tier = five.tier;
        for (const char* id : five.ids) {
            const int e = d.roster.find(id);
            REQUIRE(e >= 0);
            lineup.enemies.push_back(static_cast<std::uint32_t>(e));
        }
        std::vector<Combatant> foes;
        makeEnemies(d.roster, lineup, foes);
        REQUIRE(foes.size() == kMaxLineupEnemies);
        applyWeakPoints(d.weakPoints, d.roster, lineup, foes);

        const demo::HeadlessResult win = demo::playHeadless(demo::party(five.level), foes, true);
        CHECK_FALSE(win.commandFailed);
        CHECK(win.outcome == Phase::Victory);
        MESSAGE("five: victory in " << win.endTime << " s, " << win.partyTurns << " turns, " << win.enemyAttacks
                                    << " defences, party HP left " << win.partyHpLeft);
        CHECK(demo::playHeadless(demo::party(five.level), foes, true, 0.13).outcome == Phase::Victory);
        CHECK(demo::playHeadless(demo::party(five.level), foes, true, 1.0 / 120.0, 1200.0, DefenceInput::Dodge).outcome ==
              Phase::Victory);
        const demo::HeadlessResult loss = demo::playHeadless(demo::party(five.level), foes, false);
        CHECK_FALSE(loss.commandFailed);
        CHECK(loss.outcome == Phase::Defeat);
    }
}

// The set-piece fight: the Brass Regent (SC-P2-01 beat 12) goes through all three of its phases
// against every pilot, each phase announced once and in order, and its turns follow each phase's
// pattern from the top.
TEST_CASE("the Brass Regent fight plays its phases and patterns for every pilot") {
    const LoadedData d = loadBattleData();
    REQUIRE(d.ok);
    const EncounterEntry* entry = d.table.findEntry("SC-P2-01", 12, 0);
    REQUIRE(entry != nullptr);
    std::vector<Combatant> foes;
    makeEnemies(d.roster, entry->lineup, foes);
    applyWeakPoints(d.weakPoints, d.roster, entry->lineup, foes);
    REQUIRE(foes.size() == 1);
    REQUIRE(foes[0].phases.size() == 3);
    CHECK(foes[0].phases[1].label() == "Overdrive");
    CHECK(foes[0].phases[2].label() == "Last Stand");

    for (const demo::PilotSkill& pilot : {demo::PilotSkill{}, demo::PilotSkill{0.75, 0.6}, demo::PilotSkill{0.5, 0.3}}) {
        CAPTURE(pilot.press);
        Battle b(demo::party(1), foes, demo::tuning());
        demo::PilotState state;
        const CombatantId regent = 2;
        std::vector<std::size_t> phasesEntered;
        std::vector<std::vector<std::string>> used(3); // enemy skills per phase
        double now = 0.0;
        const double step = 1.0 / 120.0;
        b.start(now);
        while (b.phase() != Phase::Victory && b.phase() != Phase::Defeat && now < 1200.0) {
            if (b.phase() == Phase::AwaitingCommand) {
                const demo::PilotChoice choice = demo::pilotChoice(b);
                if (choice.aim) {
                    REQUIRE(b.aim(choice.target, now) == CommandError::None);
                    demo::pilotShoot(b, choice.weakPoint, now);
                } else {
                    REQUIRE(b.command(choice.skill, choice.target, now) == CommandError::None);
                }
            }
            demo::pilotPress(b, now, pilot, state);
            demo::pilotDefend(b, now + step, DefenceInput::Parry, pilot, state);
            now += step;
            b.update(now);
            for (const BattleEvent& e : b.events()) {
                if (e.type == EventType::PhaseChanged) phasesEntered.push_back(e.beat);
                if (e.type == EventType::SkillStarted && e.source == regent)
                    used[b.enemyPhase(regent)].emplace_back(e.label);
            }
            b.clearEvents();
        }
        CHECK(b.phase() == Phase::Victory);
        CHECK(phasesEntered == std::vector<std::size_t>{1, 2});
        CHECK(b.stats().phaseChanges == 2);
        const auto starts = [](const std::vector<std::string>& got, std::initializer_list<const char*> want) {
            std::size_t i = 0;
            for (const char* w : want) {
                if (i >= got.size()) return true; // the phase ended before more turns
                if (got[i++] != w) return false;
            }
            return true;
        };
        CHECK(starts(used[0], {"Hammer", "Decree", "Hammer", "Decree"}));
        CHECK(starts(used[1], {"Rally Strike", "Hammer", "Rally Strike", "Decree", "Rally Strike"}));
        CHECK(starts(used[2], {"Verdict", "Onslaught", "Rally Strike", "Onslaught"}));
        CHECK_FALSE(used[0].empty());
        CHECK_FALSE(used[1].empty());
        CHECK_FALSE(used[2].empty()); // the Last Stand opener always gets its turn
        MESSAGE("Regent: " << used[0].size() << " / " << used[1].size() << " / " << used[2].size() << " turns per phase, "
                           << b.stats().endTime << " s");
    }
}

TEST_CASE("an imperfect pilot misses evenly and deterministically") {
    int hits = 0;
    for (unsigned n = 0; n < 100; ++n) hits += demo::pilotSucceeds(n, 0.75) ? 1 : 0;
    CHECK(hits == 75);
    CHECK(demo::pilotSucceeds(0, 0.75)); // the first action lands
    CHECK_FALSE(demo::pilotSucceeds(2, 0.75)); // one in four misses
    for (unsigned n = 0; n < 8; ++n) {
        CHECK(demo::pilotSucceeds(n, 1.0));
        CHECK_FALSE(demo::pilotSucceeds(n, 0.0));
    }

    // Against the demo line-up a weaker pilot lands fewer presses and takes more damage, and the
    // same skill plays the same fight twice.
    const demo::PilotSkill typical{0.75, 0.6};
    const demo::HeadlessResult perfect = demo::playHeadless(demo::party(), demo::enemies(), true);
    const demo::HeadlessResult a =
        demo::playHeadless(demo::party(), demo::enemies(), true, 1.0 / 120.0, 1200.0, DefenceInput::Parry, typical);
    const demo::HeadlessResult b =
        demo::playHeadless(demo::party(), demo::enemies(), true, 1.0 / 120.0, 1200.0, DefenceInput::Parry, typical);
    CHECK(a.finished);
    CHECK(a.endTime == b.endTime);
    CHECK(a.partyHpLeft == b.partyHpLeft);
    CHECK(a.partyHpLeft < perfect.partyHpLeft);
    CHECK(a.endTime > perfect.endTime);
}

// Prints a BAL line per encounter with -s (perfect, typical and novice: outcome, seconds, party HP, aims).
TEST_CASE("balance: typical and novice pilots against every encounter") {
    const LoadedData d = loadBattleData();
    REQUIRE(d.ok);
    const demo::PilotSkill typical{0.75, 0.6}, novice{0.5, 0.3};
    const std::vector<int> levels = levelsInTableOrder(d);
    for (std::size_t i = 0; i <= d.table.entries.size(); ++i) {
        const EncounterLineup& lineup = i == 0 ? d.table.fallback : d.table.entries[i - 1].lineup;
        const int level = i == 0 ? 1 : levels[i - 1];
        const std::string name = i == 0 ? std::string("default")
                                        : d.table.entries[i - 1].scene + " b" + std::to_string(d.table.entries[i - 1].beat) +
                                              " s" + std::to_string(d.table.entries[i - 1].step);
        std::vector<Combatant> foes;
        makeEnemies(d.roster, lineup, foes);
        applyWeakPoints(d.weakPoints, d.roster, lineup, foes);
        const auto run = [&](const demo::PilotSkill& p) {
            return demo::playHeadless(demo::party(level), foes, true, 1.0 / 120.0, 1200.0, DefenceInput::Parry, p);
        };
        const demo::HeadlessResult perfect = run({}), t = run(typical), n = run(novice);
        const auto out = [](const demo::HeadlessResult& r) {
            return std::string(r.outcome == Phase::Victory ? "W " : "L ") + std::to_string(static_cast<int>(r.endTime)) +
                   "s hp" + std::to_string(r.partyHpLeft) + " aim" + std::to_string(r.aims);
        };
        std::string ids;
        for (std::uint32_t e : lineup.enemies) ids += d.roster.enemies[e].id + " ";
        // A typical player wins every fight inside its time budget; a novice wins the early ones.
        CAPTURE(name);
        CHECK(t.outcome == Phase::Victory);
        CHECK(t.endTime <= 60.0 + 8.0 * lineup.tier);
        if (lineup.tier <= 1) CHECK(n.outcome == Phase::Victory);
        MESSAGE("BAL " << name << " t" << lineup.tier << " L" << level << " [" << ids << "] perfect " << out(perfect) << " | typical "
                       << out(t) << " | novice " << out(n));
    }
}

TEST_CASE("tiers toughen a line-up and stay within bounds") {
    const LoadedData d = loadBattleData();
    REQUIRE(d.ok);
    EncounterLineup lineup = d.table.fallback;
    std::vector<Combatant> base, tough;
    makeEnemies(d.roster, lineup, base);
    lineup.tier = 5;
    makeEnemies(d.roster, lineup, tough);
    REQUIRE(base.size() == tough.size());
    for (std::size_t i = 0; i < base.size(); ++i) {
        CHECK(tough[i].stats.maxHp == base[i].stats.maxHp + base[i].stats.maxHp * 5 * kTierHpPercent / 100);
        CHECK(tough[i].hp == tough[i].stats.maxHp);
        CHECK(tough[i].stats.attack > base[i].stats.attack);
        CHECK(tough[i].stats.defense == base[i].stats.defense);
    }
    lineup.tier = 99; // clamped
    std::vector<Combatant> clamped;
    makeEnemies(d.roster, lineup, clamped);
    CHECK(clamped[0].stats.maxHp == base[0].stats.maxHp + base[0].stats.maxHp * kMaxTier * kTierHpPercent / 100);

    EnemyRoster roster;
    EncounterTable table;
    REQUIRE(importEnemyRoster(kRoster, roster));
    CHECK(importEncounterTable(R"({"default": {"enemies": ["brute"], "tier": 2}, "encounters": [{"scene": "S", "beat": 1, "tier": 4, "enemies": ["shade"]}]})",
                               roster, table));
    CHECK(table.fallback.tier == 2);
    CHECK(table.find("S", 1, 0).tier == 4);
    std::string err;
    CHECK_FALSE(importEncounterTable(R"({"default": {"enemies": ["brute"], "tier": 10}})", roster, table, &err));
    CHECK(err.find("tier") != std::string::npos);
}

namespace {
struct BattleStep {
    std::string scene;
    std::uint32_t beat, step;
};

// Walks the script from scene `from` (empty: the default start) on autoplay, appending each battle step reached. With
// `onlyThat`, the walk keeps to `from` and stops when the story leaves it. Returns whether the walk ended cleanly.
bool walkBattleSteps(const StoryDatabase& db, std::string_view from, bool onlyThat, std::vector<BattleStep>& out) {
    StoryRunner r(&db);
    DialoguePresenter p(&r);
    p.setRevealRate(0.f);
    GameplayPlan plan;
    if (!r.start(from)) return false;
    int moves = 0;
    while (!r.finished() && moves < 200000) {
        if (onlyThat && r.position().scene != from) return true;
        if (r.waitingForGameplay()) {
            if (!plan.lower(r)) return false;
            for (std::uint32_t i = 0; i < plan.size(); ++i)
                if (plan.step(i).screen == GameplayScreen::Battle)
                    out.push_back({std::string(r.position().scene), static_cast<std::uint32_t>(r.position().beat), i});
            if (!r.resume()) return false;
            ++moves;
            continue;
        }
        p.update(1.f / 60.f);
        const DialogueCommand c = p.autoplay();
        if (c.none() || !applyDialogueCommand(r, c)) break;
        ++moves;
    }
    return r.finished();
}

// Every battle step the script reaches, in the order the table lists fights: the walk from the default start, then
// each listed scene that walk never enters (a scene played on its own, e.g. a later chapter's scene), walked alone.
// A listed scene the loaded script does not have at all is put in `absent` (one per table row) and not walked;
// it must be one of kNewerDraftScenes, or the walk fails (a typo or a deleted scene must not slip by).
// The scenes the newer draft (the game's Data/Story) has and this test script does not: the game's own walk
// checks their fights. Add a scene here only when its rows are new in the newer draft.
constexpr std::string_view kNewerDraftScenes[] = {"p04"};

bool newerDraftScene(std::string_view scene) {
    for (const std::string_view s : kNewerDraftScenes)
        if (s == scene) return true;
    return false;
}

bool scriptBattleSteps(const StoryDatabase& db, const EncounterTable& table, std::vector<BattleStep>& out,
                       std::vector<std::string>& absent) {
    absent.clear();
    if (!walkBattleSteps(db, {}, false, out)) return false;
    for (const EncounterEntry& e : table.entries) {
        if (db.findScene(e.scene) == kNone) {
            absent.push_back(e.scene + " beat " + std::to_string(e.beat) + " step " + std::to_string(e.step));
            if (!newerDraftScene(e.scene)) return false; // not a known newer-draft scene: a typo or a deletion
            continue;
        }
        bool seen = false;
        for (const BattleStep& k : out) seen = seen || k.scene == e.scene;
        if (!seen) {
            const std::size_t before = out.size();
            if (!walkBattleSteps(db, e.scene, true, out)) return false;
            if (out.size() == before) return false; // a listed scene with no battle step at all
        }
    }
    return true;
}
} // namespace

TEST_CASE("every listed encounter is a real battle step of the script") {
    FTCORE_REQUIRE_REAL_STORY(s);
    const LoadedData d = loadBattleData();
    REQUIRE(d.ok);
    std::vector<BattleStep> real;
    std::vector<std::string> absent;
    CHECK(scriptBattleSteps(s.db, d.table, real, absent));
    std::string absentList;
    for (const std::string& a : absent) absentList += (absentList.empty() ? "" : ", ") + a;
    // A plain line, so it shows in a ctest run (a doctest MESSAGE shows only with -s or on a failure).
    std::printf("ENCOUNTERS %zu listed in newer-draft scenes, not checked here: %s\n", absent.size(),
                absentList.empty() ? "none" : absentList.c_str());
    CHECK(real.size() + absent.size() >= d.table.entries.size());
    for (const EncounterEntry& e : d.table.entries) {
        if (s.db.findScene(e.scene) == kNone) continue;
        CAPTURE(e.scene);
        CAPTURE(e.beat);
        CAPTURE(e.step);
        bool found = false;
        for (const BattleStep& k : real) found = found || (k.scene == e.scene && k.beat == e.beat && k.step == e.step);
        CHECK(found);
    }
}

TEST_CASE("every battle step of the script has its own line-up") {
    FTCORE_REQUIRE_REAL_STORY(s);
    const LoadedData d = loadBattleData();
    REQUIRE(d.ok);
    std::vector<BattleStep> real;
    std::vector<std::string> absent;
    CHECK(scriptBattleSteps(s.db, d.table, real, absent));
    std::size_t steps = 0, missing = 0, outOfOrder = 0;
    for (const BattleStep& k : real) {
        ++steps;
        const EncounterEntry* entry = d.table.findEntry(k.scene, k.beat, k.step);
        if (!entry) {
            ++missing;
            MESSAGE("no encounter for " << k.scene << " beat " << k.beat << " step " << k.step);
            continue;
        }
        // The table lists the fights in the order the script reaches them.
        const auto index = static_cast<std::size_t>(entry - d.table.entries.data());
        if (index != steps - 1) {
            ++outOfOrder;
            MESSAGE("encounter " << k.scene << " beat " << k.beat << " step " << k.step << " is entry " << index
                                 << ", but the script reaches it as fight " << steps - 1);
        }
    }
    CHECK(steps > 0);
    CHECK(missing == 0);
    CHECK(outOfOrder == 0);
    CHECK(d.table.entries.size() == steps + absent.size()); // no entry for a step that does not exist
}

TEST_CASE("a staged line-up becomes the roster's ids; a bad one is refused") {
    EnemyRoster roster;
    REQUIRE(importEnemyRoster(kRoster, roster));
    EncounterLineup lineup;
    std::string ids = "stale";
    CHECK_FALSE(lineupIds(roster, lineup, ids)); // empty: no fight
    CHECK(ids.empty());
    lineup.enemies = {0, 0, 1};
    REQUIRE(lineupIds(roster, lineup, ids));
    CHECK(ids == roster.enemies[0].id + "," + roster.enemies[0].id + "," + roster.enemies[1].id);
    lineup.enemies.push_back(static_cast<std::uint32_t>(roster.enemies.size()));
    CHECK_FALSE(lineupIds(roster, lineup, ids)); // one index outside the roster: no fight
    CHECK(ids.empty());
}

TEST_CASE("every staged encounter names a stage in arenas.json and arena kinds in arena.json") {
    const LoadedData d = loadBattleData();
    REQUIRE(d.ok);
    std::string text, err;
    REQUIRE(ftcore_test::readProjectData("Battle/arena.json", text));
    ArenaTable arena;
    REQUIRE_MESSAGE(importArenaTable(text, arena, &err), err);
    REQUIRE(ftcore_test::readProjectData("Field/worlds.json", text));
    WorldTable worlds;
    REQUIRE_MESSAGE(importWorldTable(text, worlds, &err), err);
    REQUIRE(ftcore_test::readProjectData("Battle/arenas.json", text));
    ArenaZoneTable zones;
    REQUIRE_MESSAGE(importArenaZoneTable(text, arena, worlds, zones, &err), err);
    std::size_t staged = 0;
    for (const EncounterEntry& e : d.table.entries) {
        if (e.stage.empty()) continue;
        ++staged;
        CAPTURE(e.scene);
        CAPTURE(e.beat);
        CAPTURE(e.step);
        CHECK_MESSAGE(zones.find(e.stage) != nullptr, "no stage " << e.stage << " in arenas.json");
        std::string ids;
        REQUIRE(lineupIds(d.roster, e.lineup, ids));
        CHECK(e.lineup.enemies.size() <= kMaxArenaEnemies);
        for (const std::uint32_t index : e.lineup.enemies) {
            const std::string& id = d.roster.enemies[index].id;
            bool kind = false;
            for (const std::string& k : arena.enemyIds) kind = kind || k == id;
            CHECK_MESSAGE(kind, "no arena kind " << id << " in arena.json");
        }
    }
    CHECK(staged >= 2); // p04's two stages
}
