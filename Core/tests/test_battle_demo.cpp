#include "ftcore/BattleDemo.h"

#include <cmath>
#include <doctest/doctest.h>

using namespace ftcore;

namespace {

struct RunResult {
    Phase outcome = Phase::NotStarted;
    int partyTurns = 0;
    int enemyCombos = 0;
    int presses = 0;
    bool allOnGrid = true;
    double endTime = 0.0;
};

// Plays the demo fight on a 120 Hz clock. With `press` the pilot steps on every note on time;
// without it the party still chooses skills but never presses an arrow.
RunResult run(bool press) {
    Battle b(demo::party(), demo::enemies(), demo::tuning());
    RunResult r;
    double now = 0.0;
    b.start(now);
    Phase last = b.phase();
    while (b.phase() != Phase::Victory && b.phase() != Phase::Defeat && now < 1200.0) {
        if (b.phase() == Phase::AwaitingCommand) {
            const demo::PilotChoice choice = demo::pilotChoice(b);
            REQUIRE(b.command(choice.skill, choice.target, now) == CommandError::None);
            ++r.partyTurns;
        }
        if (b.phase() != last && (b.phase() == Phase::PartySkill || b.phase() == Phase::EnemySkill)) {
            r.enemyCombos += b.phase() == Phase::EnemySkill;
            for (const ComboNote& n : b.combo().notes()) r.allOnGrid = r.allOnGrid && n.beat != kNoBeat;
        }
        last = b.phase();
        if (press) r.presses += demo::pilotPress(b, now);
        now += 1.0 / 120.0;
        b.update(now);
        b.clearEvents();
    }
    r.outcome = b.phase();
    r.endTime = now;
    return r;
}

} // namespace

TEST_CASE("the demo line-up is two against two and fits the HUD") {
    const auto party = demo::party();
    const auto foes = demo::enemies();
    REQUIRE(party.size() == 2);
    REQUIRE(foes.size() == 2);
    for (const Combatant& c : party) {
        CHECK(!c.skills.empty());
        CHECK(c.skills.size() <= 4); // skill keys 1-4
        CHECK(c.hp == c.stats.maxHp);
        CHECK(c.skills.front().apCost == 0); // always something to do
    }
    for (const Combatant& c : foes) CHECK(!c.skills.empty());
    CHECK(demo::tuning().music.enabled());
}

TEST_CASE("pilot choice: strongest affordable skill on the weakest living enemy") {
    Battle b(demo::party(), demo::enemies(), demo::tuning());
    b.start(0.0);
    // Run until a party member has to choose.
    double now = 0.0;
    while (b.phase() != Phase::AwaitingCommand) {
        demo::pilotPress(b, now);
        now += 0.01;
        b.update(now);
        REQUIRE(now < 60.0);
    }
    const demo::PilotChoice choice = demo::pilotChoice(b);
    const Combatant& me = b.combatant(b.actor());
    REQUIRE(choice.skill < me.skills.size());
    CHECK(me.skills[choice.skill].apCost <= me.ap);
    CHECK(b.combatant(choice.target).team == Team::Enemies);
    CHECK(b.combatant(choice.target).alive());
    for (const Combatant& c : b.combatants())
        if (c.team == Team::Enemies && c.alive()) CHECK(b.combatant(choice.target).hp <= c.hp);
}

TEST_CASE("headless: the demo fight played perfectly ends in victory") {
    const RunResult r = run(true);
    CHECK(r.outcome == Phase::Victory);
    CHECK(r.partyTurns >= 4);
    CHECK(r.enemyCombos >= 1); // the enemies got to attack and were defended against
    CHECK(r.presses > 20);
    CHECK(r.allOnGrid); // every chart sat on the 120 BPM beat grid
    MESSAGE("victory after " << r.partyTurns << " party turns, " << r.enemyCombos << " defences, "
                             << r.presses << " presses, " << r.endTime << " s");

    // Same seed, same play: the same fight.
    const RunResult again = run(true);
    CHECK(again.partyTurns == r.partyTurns);
    CHECK(again.presses == r.presses);
    CHECK(std::abs(again.endTime - r.endTime) < 1e-9);
}

TEST_CASE("headless: the demo fight with no arrows pressed ends in defeat") {
    const RunResult r = run(false);
    CHECK(r.outcome == Phase::Defeat);
    CHECK(r.presses == 0);
}
