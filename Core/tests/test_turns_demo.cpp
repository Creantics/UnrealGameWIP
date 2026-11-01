// The Turns demo fight (Data/Battle/encounters.json "demos": "turns") and the data it needs: enemy
// moves aimed at the back of the party ("targeting": "back"), the child flag on a roster entry, and
// the demo autopilot that uses every Turns verb (Guard, Intercept, Release, Take Half, Read).
#include "AllocCounter.h"
#include "TestAssets.h"
#include "ftcore/Battle.h"
#include "ftcore/BattleDemo.h"
#include "ftcore/Encounter.h"
#include "ftcore/WeakPoint.h"

#include <cstdint>
#include <ostream> // doctest prints std::string_view through operator<<
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

constexpr const char* kRoster = R"({"enemies": [
    {"id": "foe", "stats": {"maxHp": 100}, "skills": [
        {"id": "poke", "power": 1.0},
        {"id": "reach", "targeting": "back", "power": 1.0},
        {"id": "shove", "targeting": "front", "power": 1.0}]},
    {"id": "kid", "child": true, "stats": {"maxHp": 60}, "skills": [{"id": "wave"}]}]})";

struct DemoData {
    EnemyRoster roster;
    EncounterTable table;
    WeakPointTable weakPoints;
    std::vector<Combatant> foes;
    bool ok = false;
};

DemoData loadTurnsDemo() {
    DemoData d;
    std::string enemies, encounters, weak, err;
    if (!ftcore_test::readProjectData("Battle/enemies.json", enemies) ||
        !ftcore_test::readProjectData("Battle/encounters.json", encounters) ||
        !ftcore_test::readProjectData("Battle/weakpoints.json", weak))
        return d;
    d.ok = importEnemyRoster(enemies, d.roster, &err) && importEncounterTable(encounters, d.roster, d.table, &err) &&
           importWeakPointTable(weak, d.roster, d.weakPoints, &err);
    const EncounterLineup* lineup = d.ok ? d.table.findDemo("turns") : nullptr;
    d.ok = d.ok && lineup;
    if (!d.ok) return d;
    makeEnemies(d.roster, *lineup, d.foes);
    applyWeakPoints(d.weakPoints, d.roster, *lineup, d.foes);
    return d;
}

// The demo autopilot: Guard mode, perfect, letting the first tearing attack land (PilotSkill::showTorn).
demo::PilotSkill showcase() {
    demo::PilotSkill s;
    s.showTorn = true;
    return s;
}

} // namespace

TEST_CASE("enemy data: targeting back, the child flag, and demo line-ups load; bad ones are refused") {
    EnemyRoster roster;
    std::string err;
    REQUIRE_MESSAGE(importEnemyRoster(kRoster, roster, &err), err);
    REQUIRE(roster.enemies.size() == 2);
    CHECK(roster.enemies[0].skills[0].targeting == Targeting::SingleEnemy);
    CHECK(roster.enemies[0].skills[1].targeting == Targeting::Back);
    CHECK(roster.enemies[0].skills[2].targeting == Targeting::Front);
    CHECK_FALSE(roster.enemies[0].child);
    CHECK(roster.enemies[1].child);

    // The flag reaches the combatant.
    EncounterLineup lineup;
    lineup.enemies = {0, 1};
    std::vector<Combatant> made;
    makeEnemies(roster, lineup, made);
    REQUIRE(made.size() == 2);
    CHECK_FALSE(made[0].child);
    CHECK(made[1].child);

    // A demo line-up is found by its id and plays no story step.
    EncounterTable table;
    REQUIRE_MESSAGE(importEncounterTable(R"({"default": {"enemies": ["foe"]},
        "demos": [{"id": "show", "tier": 2, "enemies": ["foe", "foe"]}]})",
                                         roster, table, &err),
                    err);
    REQUIRE(table.findDemo("show"));
    CHECK(table.findDemo("show")->enemies.size() == 2);
    CHECK(table.findDemo("show")->tier == 2);
    CHECK(table.findDemo("nope") == nullptr);
    CHECK(table.entries.empty());

    EnemyRoster bad;
    CHECK_FALSE(importEnemyRoster(R"({"enemies": [{"id": "a", "skills": [{"id": "s", "targeting": "middle"}]}]})", bad, &err));
    CHECK(err.find("targeting") != std::string::npos);
    CHECK_FALSE(importEnemyRoster(R"({"enemies": [{"id": "a", "child": 1, "skills": [{"id": "s"}]}]})", bad, &err));
    CHECK(err.find("child") != std::string::npos);
    EncounterTable badTable;
    CHECK_FALSE(importEncounterTable(R"({"default": {"enemies": ["foe"]}, "demos": [{"id": "x", "enemies": ["ghost"]}]})",
                                     roster, badTable, &err));
    CHECK(err.find("demos[0].enemies") != std::string::npos);
    CHECK_FALSE(importEncounterTable(R"({"default": {"enemies": ["foe"]},
        "demos": [{"id": "x", "enemies": ["foe"]}, {"id": "x", "enemies": ["foe"]}]})",
                                     roster, badTable, &err));
    CHECK(err.find("duplicate") != std::string::npos);
}

TEST_CASE("a child from the roster on the party's side is never intercepted for") {
    EnemyRoster roster;
    std::string err;
    REQUIRE_MESSAGE(importEnemyRoster(kRoster, roster, &err), err);
    EncounterLineup kidOnly;
    kidOnly.enemies = {1};
    std::vector<Combatant> guests;
    makeEnemies(roster, kidOnly, guests);
    REQUIRE(guests.size() == 1);

    // The guard lead first, the child behind it; the foe's "back" move goes for the child.
    std::vector<Combatant> party = demo::party();
    party.resize(1);
    party.push_back(guests[0]);
    EncounterLineup foeOnly;
    foeOnly.enemies = {0};
    std::vector<Combatant> foes;
    makeEnemies(roster, foeOnly, foes);
    foes[0].stats.speed = 1000; // it acts first
    foes[0].skills = {foes[0].skills[1]}; // only the move at the back
    Battle b(party, foes, demo::tuning());
    b.start(0.0);
    REQUIRE(b.phase() != Phase::AwaitingCommand);
    double now = 0.0;
    while (b.phase() != Phase::EnemySkill && now < 10.0) b.update(now += 0.01);
    REQUIRE(b.phase() == Phase::EnemySkill);
    CHECK(b.defender() == 1);
    CHECK(b.combatant(1).child);
    CHECK_FALSE(b.interceptor().has_value());

    // The same line-up without the flag: the guard lead may take it.
    party[1].child = false;
    Battle c(party, foes, demo::tuning());
    c.start(0.0);
    now = 0.0;
    while (c.phase() != Phase::EnemySkill && now < 10.0) c.update(now += 0.01);
    REQUIRE(c.phase() == Phase::EnemySkill);
    CHECK(c.defender() == 1);
    REQUIRE(c.interceptor().has_value());
    CHECK(*c.interceptor() == 0);
}

TEST_CASE("targeting back and front: the move goes for the last (first) living party member, not the most wounded") {
    std::vector<Combatant> party = demo::party();
    party[1].hp = party[1].stats.maxHp;
    party[0].hp = 30; // the most wounded is the first
    Combatant foe;
    foe.name = "Foe";
    foe.stats = {200, 10, 0, 1000, 10};
    foe.hp = 200;
    Skill reach;
    reach.name = "Reach";
    reach.targeting = Targeting::Back;
    foe.skills = {reach};
    Battle b(party, {foe}, demo::tuning());
    b.start(0.0);
    double now = 0.0;
    while (b.phase() != Phase::EnemySkill && now < 10.0) b.update(now += 0.01);
    REQUIRE(b.phase() == Phase::EnemySkill);
    CHECK(b.defender() == 1);

    // With the back member down, it goes for the one left.
    party[1].hp = 0;
    Battle c(party, {foe}, demo::tuning());
    c.start(0.0);
    now = 0.0;
    while (c.phase() != Phase::EnemySkill && now < 10.0) c.update(now += 0.01);
    REQUIRE(c.phase() == Phase::EnemySkill);
    CHECK(c.defender() == 0);

    // Front: the first living member, however wounded the others are.
    party[1].hp = 20;
    party[0].hp = party[0].stats.maxHp;
    Combatant shover = foe;
    shover.skills[0].targeting = Targeting::Front;
    Battle f(party, {shover}, demo::tuning());
    f.start(0.0);
    now = 0.0;
    while (f.phase() != Phase::EnemySkill && now < 10.0) f.update(now += 0.01);
    REQUIRE(f.phase() == Phase::EnemySkill);
    CHECK(f.defender() == 0);

    // A party skill with it takes one target, as a single-target skill does.
    std::vector<Combatant> one = demo::party();
    one[0].skills[0].targeting = Targeting::Back;
    one[0].stats.speed = 1000;
    Combatant slow = foe;
    slow.stats.speed = 1;
    Battle p(one, {slow, slow}, demo::tuning());
    p.start(0.0);
    REQUIRE(p.phase() == Phase::AwaitingCommand);
    CHECK(p.command(0, 3, 0.0) == CommandError::None);
    CHECK(p.activeTargets().size() == 1);
}

TEST_CASE("the Turns demo line-up: four to five enemies, distinct tells, a move at the back and a tearing one") {
    const DemoData d = loadTurnsDemo();
    REQUIRE(d.ok);
    CHECK(d.foes.size() >= 4);
    CHECK(d.foes.size() <= kMaxLineupEnemies);
    std::vector<std::string_view> tells;
    int back = 0, front = 0, torn = 0;
    for (const Combatant& c : d.foes) {
        CHECK(c.stats.maxPoise > 0);
        CHECK(c.weakPoints.size() > 0);
        for (const Skill& s : c.skills) {
            CAPTURE(s.name);
            CHECK_FALSE(s.tellId.empty());
            for (const std::string_view t : tells) CHECK(t != tellOf(s));
            tells.push_back(tellOf(s));
            back += s.targeting == Targeting::Back ? 1 : 0;
            front += s.targeting == Targeting::Front ? 1 : 0;
            torn += s.torn ? 1 : 0;
        }
    }
    CHECK(back >= 1);  // at the second lead: the first Intercepts it
    CHECK(front >= 1); // at the first lead: it Guards
    CHECK(torn >= 1);
}

TEST_CASE("the Turns demo: the autopilot wins using every verb, at fine and coarse steps; idle loses") {
    const DemoData d = loadTurnsDemo();
    REQUIRE(d.ok);
    for (const double step : {1.0 / 120.0, 1.0 / 60.0, 0.13}) {
        CAPTURE(step);
        const demo::HeadlessResult r =
            demo::playHeadless(demo::party(), d.foes, true, step, 1200.0, DefenceInput::Guard, showcase());
        MESSAGE("step " << step << ": " << (r.outcome == Phase::Victory ? "victory" : "not won") << " in " << r.endTime
                        << " s, party hp " << r.partyHpLeft << ", releases " << r.releases << ", intercepts "
                        << r.intercepts << ", take halves " << r.takeHalves << ", reads " << r.correctReads << "/"
                        << r.reads << ", defended " << r.hitsDefended << ", load " << r.loadStored << ", aims " << r.aims);
        CHECK_FALSE(r.commandFailed);
        CHECK(r.outcome == Phase::Victory);
        CHECK(r.releases >= 1);
        CHECK(r.intercepts > 0);
        // The first lead, with Guard, holds every hit aimed at it in Guard mode: the hits defended
        // beyond the intercepted ones are its Guards.
        CHECK(r.hitsDefended > r.intercepts);
        CHECK(r.takeHalves >= 1);
        CHECK(r.reads > 0);
        CHECK(r.correctReads == r.reads);
        CHECK(r.loadStored > 0);
        CHECK(r.endTime < 240.0); // a demo, not a slog
    }
    // Without the showcase nothing tears (every hit is held perfectly), so there is no Take Half.
    const demo::HeadlessResult plain = demo::playHeadless(demo::party(), d.foes, true, 1.0 / 120.0, 1200.0, DefenceInput::Guard);
    CHECK(plain.outcome == Phase::Victory);
    CHECK(plain.takeHalves == 0);
    // The plain parry pilot wins it too, and the idle player loses.
    CHECK(demo::playHeadless(demo::party(), d.foes, true).outcome == Phase::Victory);
    const demo::HeadlessResult idle = demo::playHeadless(demo::party(), d.foes, false, 1.0 / 120.0, 1200.0, DefenceInput::Guard);
    CHECK(idle.finished);
    CHECK(idle.outcome == Phase::Defeat);
}

TEST_CASE("the Turns demo: the showcase pilot's whole fight allocates nothing per step") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    const DemoData d = loadTurnsDemo();
    REQUIRE(d.ok);
    Battle b(demo::party(), d.foes, demo::tuning());
    const demo::PilotSkill skill = showcase();
    demo::PilotState state;
    const double step = 1.0 / 120.0;
    double now = 0.0;
    b.start(now);
    std::uint64_t allocs = 0;
    for (int steps = 0; steps < 200000 && b.phase() != Phase::Victory && b.phase() != Phase::Defeat; ++steps) {
        const auto before = ftcore_test::allocStats().count;
        if (b.phase() == Phase::AwaitingCommand) {
            const CombatantId ally = demo::pilotTakeHalf(b);
            if (ally != demo::kNoAlly) REQUIRE(b.takeHalf(ally, now) == CommandError::None);
        }
        if (b.phase() == Phase::AwaitingCommand) {
            const demo::PilotChoice choice = demo::pilotChoice(b);
            const CommandError e = choice.release ? b.release(now) : b.command(choice.skill, choice.target, now);
            REQUIRE(e == CommandError::None);
        }
        demo::pilotRead(b, now, skill, state);
        demo::pilotPress(b, now, skill, state);
        demo::pilotDefend(b, now + step, DefenceInput::Guard, skill, state);
        now += step;
        b.update(now);
        b.clearEvents();
        if (now >= 20.0) allocs += ftcore_test::allocStats().count - before; // the first seconds grow the pools
    }
    CHECK(b.phase() == Phase::Victory);
    CHECK(b.stats().takeHalves >= 1);
    CHECK(b.stats().releases >= 1);
    CHECK(allocs == 0);
}
