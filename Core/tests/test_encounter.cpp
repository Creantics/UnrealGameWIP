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
           a.power == b.power && a.notes == b.notes && a.difficulty == b.difficulty && sameTimeline(a.timeline, b.timeline);
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
                                               { "at": 1.5, "tell": 0.3, "telegraph": 1.2, "weight": 2, "jumpable": true } ] } ] } ]
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
    CHECK(t[1].tell == 0.3);
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
        CHECK(lineup.enemies.size() <= 3); // the HUD stacks three enemy panels

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

TEST_CASE("every listed encounter is a real battle step of the script") {
    FTCORE_REQUIRE_REAL_STORY(s);
    const LoadedData d = loadBattleData();
    REQUIRE(d.ok);

    struct Key {
        std::string scene;
        std::uint32_t beat, step;
    };
    std::vector<Key> real;
    StoryRunner r(&s.db);
    DialoguePresenter p(&r);
    p.setRevealRate(0.f);
    GameplayPlan plan;
    REQUIRE(r.start());
    int moves = 0;
    while (!r.finished() && moves < 200000) {
        if (r.waitingForGameplay()) {
            REQUIRE(plan.lower(r));
            for (std::uint32_t i = 0; i < plan.size(); ++i)
                if (plan.step(i).screen == GameplayScreen::Battle)
                    real.push_back({std::string(r.position().scene), static_cast<std::uint32_t>(r.position().beat), i});
            REQUIRE(r.resume());
            ++moves;
            continue;
        }
        p.update(1.f / 60.f);
        const DialogueCommand c = p.autoplay();
        if (c.none() || !applyDialogueCommand(r, c)) break;
        ++moves;
    }
    CHECK(r.finished());
    CHECK(real.size() >= d.table.entries.size());
    for (const EncounterEntry& e : d.table.entries) {
        CAPTURE(e.scene);
        CAPTURE(e.beat);
        CAPTURE(e.step);
        bool found = false;
        for (const Key& k : real) found = found || (k.scene == e.scene && k.beat == e.beat && k.step == e.step);
        CHECK(found);
    }
}

TEST_CASE("every battle step of the script has its own line-up") {
    FTCORE_REQUIRE_REAL_STORY(s);
    const LoadedData d = loadBattleData();
    REQUIRE(d.ok);

    std::size_t steps = 0, missing = 0, outOfOrder = 0;
    StoryRunner r(&s.db);
    DialoguePresenter p(&r);
    p.setRevealRate(0.f);
    GameplayPlan plan;
    REQUIRE(r.start());
    int moves = 0;
    while (!r.finished() && moves < 200000) {
        if (r.waitingForGameplay()) {
            REQUIRE(plan.lower(r));
            for (std::uint32_t i = 0; i < plan.size(); ++i) {
                if (plan.step(i).screen != GameplayScreen::Battle) continue;
                ++steps;
                const std::string scene(r.position().scene);
                const auto beat = static_cast<std::uint32_t>(r.position().beat);
                const EncounterEntry* entry = d.table.findEntry(scene, beat, i);
                const bool listed = entry != nullptr;
                if (listed) {
                    // The table lists the fights in the order the script reaches them.
                    const auto index = static_cast<std::size_t>(entry - d.table.entries.data());
                    if (index != steps - 1) {
                        ++outOfOrder;
                        MESSAGE("encounter " << scene << " beat " << beat << " step " << i << " is entry " << index
                                             << ", but the script reaches it as fight " << steps - 1);
                    }
                }
                if (!listed) {
                    ++missing;
                    MESSAGE("no encounter for " << scene << " beat " << beat << " step " << i);
                }
            }
            REQUIRE(r.resume());
            ++moves;
            continue;
        }
        p.update(1.f / 60.f);
        const DialogueCommand c = p.autoplay();
        if (c.none() || !applyDialogueCommand(r, c)) break;
        ++moves;
    }
    CHECK(r.finished());
    CHECK(steps > 0);
    CHECK(missing == 0);
    CHECK(outOfOrder == 0);
    CHECK(d.table.entries.size() == steps); // no entry for a step that does not exist
}
