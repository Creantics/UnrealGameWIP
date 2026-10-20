#include "ftcore/BattleDemo.h"
#include "ftcore/Dialogue.h"
#include "ftcore/Encounter.h"
#include "ftcore/GameplayPlan.h"
#include "ftcore/StoryRunner.h"

#include "AllocCounter.h"
#include "RealStory.h"
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
    return d;
}

} // namespace

TEST_CASE("balance gate: every encounter is won with perfect presses and lost with none") {
    const LoadedData d = loadBattleData();
    REQUIRE(d.ok);
    REQUIRE_FALSE(d.table.entries.empty());

    // The default line-up first, then every listed entry at its own tier.
    for (std::size_t i = 0; i <= d.table.entries.size(); ++i) {
        const EncounterLineup& lineup = i == 0 ? d.table.fallback : d.table.entries[i - 1].lineup;
        const std::string name = i == 0 ? std::string("default")
                                        : d.table.entries[i - 1].scene + " beat " + std::to_string(d.table.entries[i - 1].beat) +
                                              " step " + std::to_string(d.table.entries[i - 1].step);
        CAPTURE(name);
        CHECK(lineup.enemies.size() >= 1);
        CHECK(lineup.enemies.size() <= 3); // the HUD stacks three enemy panels

        std::vector<Combatant> foes;
        makeEnemies(d.roster, lineup, foes);
        REQUIRE(foes.size() == lineup.enemies.size());

        const demo::HeadlessResult win = demo::playHeadless(demo::party(), foes, true);
        CHECK_FALSE(win.commandFailed);
        CHECK(win.finished);
        CHECK(win.outcome == Phase::Victory);
        CHECK(win.partyTurns >= 1);
        CHECK(win.presses > 0);
        MESSAGE(name << ": tier " << lineup.tier << ", victory in " << win.endTime << " s, " << win.partyTurns
                     << " turns, " << win.enemyAttacks << " defences, party HP left " << win.partyHpLeft);

        // Long frames (130 ms of battle time per frame, as under time dilation) still win: the autopilot
        // presses at each note's own time before the update that would expire it.
        const demo::HeadlessResult coarse = demo::playHeadless(demo::party(), foes, true, 0.13);
        CHECK(coarse.outcome == Phase::Victory);
        // Perfect dodges (jumps on jumpable hits) instead of parries: no counters, still a win.
        const demo::HeadlessResult dodged =
            demo::playHeadless(demo::party(), foes, true, 1.0 / 120.0, 1200.0, DefenceInput::Dodge);
        CHECK(dodged.outcome == Phase::Victory);
        const demo::HeadlessResult dodgedCoarse =
            demo::playHeadless(demo::party(), foes, true, 0.13, 1200.0, DefenceInput::Dodge);
        CHECK(dodgedCoarse.outcome == Phase::Victory);

        const demo::HeadlessResult loss = demo::playHeadless(demo::party(), foes, false);
        CHECK_FALSE(loss.commandFailed);
        CHECK(loss.finished);
        CHECK(loss.outcome == Phase::Defeat);
        CHECK(loss.presses == 0);
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
        CHECK(tough[i].stats.maxHp == base[i].stats.maxHp + base[i].stats.maxHp * 5 * kTierPercent / 100);
        CHECK(tough[i].hp == tough[i].stats.maxHp);
        CHECK(tough[i].stats.attack > base[i].stats.attack);
        CHECK(tough[i].stats.defense == base[i].stats.defense);
    }
    lineup.tier = 99; // clamped
    std::vector<Combatant> clamped;
    makeEnemies(d.roster, lineup, clamped);
    CHECK(clamped[0].stats.maxHp == base[0].stats.maxHp + base[0].stats.maxHp * kMaxTier * kTierPercent / 100);

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
    CHECK(real.size() > d.table.entries.size());
    for (const EncounterEntry& e : d.table.entries) {
        CAPTURE(e.scene);
        CAPTURE(e.beat);
        CAPTURE(e.step);
        bool found = false;
        for (const Key& k : real) found = found || (k.scene == e.scene && k.beat == e.beat && k.step == e.step);
        CHECK(found);
    }
}
