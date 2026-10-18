#include "ftcore/Battle.h"
#include "AllocCounter.h"

#include <array>
#include <cmath>
#include <ostream> // doctest prints std::string_view through operator<<
#include <span>

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

Skill basicAttack() { return {"Strike", 0, 1, Targeting::SingleEnemy, 1.0f, 4, 1.0f}; }
Skill heavy() { return {"Heavy", 2, 0, Targeting::SingleEnemy, 2.0f, 6, 5.0f}; }
Skill sweep() { return {"Sweep", 1, 0, Targeting::AllEnemies, 1.0f, 4, 1.0f}; }

Combatant hero(int speed = 20, int hp = 100) {
    Combatant c;
    c.name = "Hero";
    c.stats = {hp, 20, 0, speed, 10};
    c.hp = hp;
    c.skills = {basicAttack(), heavy(), sweep()};
    return c;
}

Combatant monster(int speed = 10, int hp = 200) {
    Combatant c;
    c.name = "Monster";
    c.stats = {hp, 10, 0, speed, 10};
    c.hp = hp;
    c.skills = {{"Claw", 0, 0, Targeting::SingleEnemy, 1.0f, 4, 2.0f}};
    return c;
}

BattleTuning tuning() {
    BattleTuning t;
    t.leadIn = 0.5;
    t.windup = 1.0;
    t.recovery = 0.5;
    return t;
}

std::vector<BattleEvent> eventsOf(const std::vector<BattleEvent>& events, EventType type) {
    std::vector<BattleEvent> out;
    for (auto& e : events)
        if (e.type == type) out.push_back(e);
    return out;
}

int total(const std::vector<BattleEvent>& events, EventType type) {
    int sum = 0;
    for (auto& e : eventsOf(events, type)) sum += e.amount;
    return sum;
}

// Steps through the first `count` notes of the active combo, `offset` seconds off the beat.
void play(Battle& b, std::size_t count = 1000, double offset = 0.0) {
    const auto notes = b.combo().notes();
    for (std::size_t i = 0; i < notes.size() && i < count; ++i) b.press(notes[i].arrow, notes[i].time + offset);
}

Arrow other(Arrow a) { return a == Arrow::Left ? Arrow::Right : Arrow::Left; }

// Advance a battle until an enemy starts attacking.
void runToEnemyAttack(Battle& b, double& now) {
    while (b.phase() != Phase::EnemySkill) {
        if (b.phase() == Phase::AwaitingCommand)
            REQUIRE(b.command(0, b.living(Team::Enemies).front(), now) == CommandError::None);
        now += 0.05;
        b.update(now);
        REQUIRE(now < 100.0);
    }
}

} // namespace

TEST_CASE("a perfect attack combo deals full damage plus the full-combo bonus") {
    Battle b({hero()}, {monster()}, tuning());
    b.start(0.0);
    REQUIRE(b.phase() == Phase::AwaitingCommand);
    CHECK(b.actor() == 0);
    REQUIRE(b.command(0, 1, 0.0) == CommandError::None);
    CHECK(b.phase() == Phase::PartySkill);
    CHECK(b.combatant(0).ap == 3); // starting 2 + 1 from basic attack
    REQUIRE(b.combo().size() == 4);
    CHECK(b.combo().note(0).time == doctest::Approx(0.5));
    b.drainEvents();

    play(b);
    const auto events = b.drainEvents();
    CHECK(eventsOf(events, EventType::NoteJudged).size() == 4);
    CHECK(eventsOf(events, EventType::Hit).size() == 5); // one per note + bonus
    CHECK(total(events, EventType::Hit) == 25);          // 20 atk * 100% + 25% bonus
    const auto done = eventsOf(events, EventType::ComboFinished);
    REQUIRE(done.size() == 1);
    CHECK(done[0].amount == 100);
    CHECK(done[0].label == "FULL COMBO");
    CHECK(b.combatant(1).hp == 175);
    CHECK(b.phase() == Phase::Recovery);
}

TEST_CASE("attack damage is the percentage of the combo performed") {
    SUBCASE("ignored combo deals nothing") {
        Battle b({hero()}, {monster()}, tuning());
        b.start(0.0);
        REQUIRE(b.command(0, 1, 0.0) == CommandError::None);
        b.drainEvents();
        b.update(5.0);
        const auto events = b.drainEvents();
        CHECK(eventsOf(events, EventType::Hit).empty());
        CHECK(eventsOf(events, EventType::ComboFinished).at(0).amount == 0);
        CHECK(b.combatant(1).hp == 200);
    }
    SUBCASE("half the notes for half damage, no bonus") {
        Battle b({hero()}, {monster()}, tuning());
        b.start(0.0);
        REQUIRE(b.command(0, 1, 0.0) == CommandError::None);
        play(b, 2);
        b.update(5.0);
        CHECK(b.combatant(1).hp == 190);
    }
    SUBCASE("sloppy timing for partial credit") {
        Battle b({hero()}, {monster()}, tuning());
        b.start(0.0);
        REQUIRE(b.command(0, 1, 0.0) == CommandError::None);
        const double good = b.combo().windows().good;
        play(b, 1000, good * 0.98); // every note graded Good (weight 0.5), but no misses
        CHECK(b.combatant(1).hp == 200 - 10 - 5); // 50% damage + full-combo bonus
    }
    SUBCASE("wrong arrows miss") {
        Battle b({hero()}, {monster()}, tuning());
        b.start(0.0);
        REQUIRE(b.command(0, 1, 0.0) == CommandError::None);
        b.drainEvents();
        const ComboNote first = b.combo().note(0);
        b.press(other(first.arrow), first.time);
        const auto judged = eventsOf(b.drainEvents(), EventType::NoteJudged);
        REQUIRE(judged.size() == 1);
        CHECK(judged[0].grade == Grade::Miss);
        CHECK(judged[0].label == "WRONG");
    }
}

TEST_CASE("command validation") {
    Battle b({hero()}, {monster()}, tuning());
    CHECK(b.command(0, 1, 0.0) == CommandError::WrongPhase);
    b.start(0.0);
    CHECK(b.command(9, 1, 0.0) == CommandError::NoSuchSkill);
    CHECK(b.command(0, 0, 0.0) == CommandError::InvalidTarget); // can't attack self
    b = Battle({hero()}, {monster()}, [] { auto t = tuning(); t.startingAp = 0; return t; }());
    b.start(0.0);
    CHECK(b.command(1, 1, 0.0) == CommandError::NotEnoughAp);
}

TEST_CASE("area skills hit every living enemy") {
    Battle b({hero()}, {monster(), monster()}, tuning());
    b.start(0.0);
    REQUIRE(b.command(2, 0 /* ignored */, 0.0) == CommandError::None);
    play(b);
    CHECK(b.combatant(1).hp == 175);
    CHECK(b.combatant(2).hp == 175);
}

TEST_CASE("combo difficulty scales with the move and both sides' stats") {
    Battle b({hero(10)}, {monster(10)}, tuning());
    const Skill& strike = b.combatant(0).skills[0];
    const Skill& big = b.combatant(0).skills[1];
    const ComboParams easy = b.previewCombo(0, 1, strike);
    const ComboParams hard = b.previewCombo(0, 1, big);
    CHECK(hard.rating > easy.rating);
    CHECK(hard.steps > easy.steps);
    CHECK(hard.interval < easy.interval);

    auto slick = monster(10);
    slick.stats.technique = 25;
    Battle vsSlick({hero(10)}, {slick}, tuning());
    CHECK(vsSlick.previewCombo(0, 1, strike).rating > easy.rating);

    auto expert = hero(10);
    expert.stats.technique = 25;
    Battle asExpert({expert}, {monster(10)}, tuning());
    CHECK(asExpert.previewCombo(0, 1, big).rating < hard.rating);
}

TEST_CASE("defense: blocking the whole combo takes no damage and counters") {
    Battle b({hero(5)}, {monster(50)}, tuning());
    double now = 0.0;
    b.start(now);
    runToEnemyAttack(b, now);
    REQUIRE(b.activeTargets().front() == 0);
    const int apBefore = b.combatant(0).ap;
    b.drainEvents();

    play(b);
    const auto events = b.drainEvents();
    CHECK(total(events, EventType::Blocked) == 10);
    CHECK(eventsOf(events, EventType::Counter).size() == 1);
    CHECK(b.combatant(0).hp == 100);
    CHECK(b.combatant(0).ap == apBefore + 1);
    CHECK(b.combatant(1).hp == 200 - 20); // counter: 20 atk * 1.0
    CHECK(b.phase() == Phase::Recovery);
}

TEST_CASE("defense: damage taken is the share of the combo not blocked") {
    SUBCASE("no input takes the full blow") {
        Battle b({hero(5)}, {monster(50)}, tuning());
        double now = 0.0;
        b.start(now);
        runToEnemyAttack(b, now);
        b.update(now + 10.0);
        CHECK(b.combatant(0).hp == 90);
    }
    SUBCASE("half-credit timing blocks half and no counter") {
        Battle b({hero(5)}, {monster(50)}, tuning());
        double now = 0.0;
        b.start(now);
        runToEnemyAttack(b, now);
        b.drainEvents();
        play(b, 1000, b.combo().windows().good * 0.98);
        const auto events = b.drainEvents();
        CHECK(total(events, EventType::Blocked) == 5);
        CHECK(total(events, EventType::Hit) == 5);
        CHECK(eventsOf(events, EventType::Counter).empty());
        CHECK(b.combatant(0).hp == 95);
    }
}

TEST_CASE("enemies focus the most wounded party member") {
    auto weak = hero(1, 100);
    weak.hp = 10;
    weak.name = "Weak";
    Battle b({hero(1), weak}, {monster(50)}, tuning());
    double now = 0.0;
    b.start(now);
    runToEnemyAttack(b, now);
    REQUIRE(b.activeTargets().size() == 1);
    CHECK(b.activeTargets().front() == 1);
}

TEST_CASE("battle ends in victory and defeat") {
    SUBCASE("victory") {
        Battle b({hero()}, {monster(10, 15)}, tuning());
        b.start(0.0);
        REQUIRE(b.command(0, 1, 0.0) == CommandError::None);
        play(b);
        CHECK(b.phase() == Phase::Victory);
        CHECK(b.living(Team::Enemies).empty());
    }
    SUBCASE("defeat") {
        Battle b({hero(1, 5)}, {monster(50)}, tuning());
        double now = 0.0;
        b.start(now);
        runToEnemyAttack(b, now);
        b.update(now + 10.0);
        CHECK(b.phase() == Phase::Defeat);
    }
}

TEST_CASE("full battle runs to completion with deterministic play") {
    Battle b({hero(12), hero(9)}, {monster(10, 150), monster(8, 150)}, tuning());
    double now = 0.0;
    b.start(now);
    int turns = 0;
    while (b.phase() != Phase::Victory && b.phase() != Phase::Defeat) {
        if (b.phase() == Phase::AwaitingCommand) {
            ++turns;
            const auto& me = b.combatant(b.actor());
            const std::size_t skill = me.ap >= 2 ? 1 : 0;
            REQUIRE(b.command(skill, b.living(Team::Enemies).front(), now) == CommandError::None);
        }
        // Step on every note as it arrives.
        if (b.phase() == Phase::PartySkill || b.phase() == Phase::EnemySkill) {
            const ComboSequence& c = b.combo();
            for (std::size_t i = c.nextIndex(); i < c.size() && c.note(i).time <= now; ++i)
                if (c.grade(i) == Grade::Pending) b.press(c.note(i).arrow, c.note(i).time);
        }
        now += 1.0 / 120.0;
        b.update(now);
        REQUIRE(now < 600.0);
    }
    CHECK(b.phase() == Phase::Victory);
    CHECK(turns > 2);
    CHECK(b.combatant(0).hp == 100); // flawless defense never takes damage
}

// --- music sync ---

namespace {
BattleTuning musicTuning(bool bar = false) {
    BattleTuning t = tuning();
    t.music.bpm = 120.0;
    t.music.offset = 0.3;
    t.music.startOnBar = bar;
    return t;
}
bool onBeat(double beat, double unit) {
    const double q = beat / unit;
    return std::abs(q - std::round(q)) < 1e-9;
}
} // namespace

TEST_CASE("with music the first note waits for the next beat after the lead-in") {
    Battle b({hero()}, {monster()}, musicTuning());
    b.start(0.0);
    REQUIRE(b.command(0, 1, 0.1) == CommandError::None);
    // leadIn 0.5 -> earliest 0.6 s = beat 0.6 on a 0.5 s beat starting at 0.3 -> beat 1 at 0.8 s
    const ComboNote& first = b.combo().note(0);
    CHECK(first.beat == doctest::Approx(1.0));
    CHECK(first.time == doctest::Approx(0.8));
    CHECK(b.comboParams().bpm == 120.0);
    for (const ComboNote& n : b.combo().notes()) {
        CHECK(onBeat(n.beat, 0.25));
        CHECK(n.time == doctest::Approx(0.3 + n.beat * 0.5));
    }
}

TEST_CASE("with music a combo can start on a bar line") {
    Battle b({hero()}, {monster()}, musicTuning(true));
    b.start(0.0);
    REQUIRE(b.command(0, 1, 0.1) == CommandError::None);
    CHECK(b.combo().note(0).beat == doctest::Approx(4.0)); // 0.6 s is beat 0.6 -> next bar at beat 4
    CHECK(b.combo().note(0).time == doctest::Approx(2.3));
}

TEST_CASE("a note exactly on the earliest time starts there") {
    Battle b({hero()}, {monster()}, musicTuning());
    b.start(0.0);
    REQUIRE(b.command(0, 1, 0.3) == CommandError::None); // earliest 0.8 s == beat 1
    CHECK(b.combo().note(0).time == doctest::Approx(0.8));
}

TEST_CASE("perfect presses on a grid chart still deal full damage") {
    Battle b({hero()}, {monster()}, musicTuning());
    b.start(0.0);
    REQUIRE(b.command(0, 1, 0.0) == CommandError::None);
    b.drainEvents();
    play(b);
    const auto events = b.drainEvents();
    CHECK(total(events, EventType::Hit) == 25);
    CHECK(eventsOf(events, EventType::ComboFinished).at(0).label == "FULL COMBO");
    CHECK(b.combatant(1).hp == 175);
}

TEST_CASE("enemy attacks are beat-aligned and blockable with music on") {
    Battle b({hero()}, {monster()}, musicTuning());
    b.start(0.0);
    double now = 0.0;
    runToEnemyAttack(b, now);
    for (const ComboNote& n : b.combo().notes()) CHECK(onBeat(n.beat, 0.25));
    CHECK(b.combo().note(0).time >= now + 1.0 - 1e-9);
    b.drainEvents();
    play(b);
    CHECK(b.combatant(0).hp == 100);
}

TEST_CASE("music off keeps the plain seconds chart") {
    Battle b({hero()}, {monster()}, tuning());
    b.start(0.0);
    REQUIRE(b.command(0, 1, 0.1) == CommandError::None);
    CHECK(b.combo().note(0).time == doctest::Approx(0.6));
    CHECK(b.combo().note(0).beat == kNoBeat);
}

TEST_CASE("events() and clearEvents() expose the same events as drainEvents()") {
    Battle a({hero()}, {monster()}, tuning());
    Battle b({hero()}, {monster()}, tuning());
    a.start(0.0);
    b.start(0.0);
    REQUIRE(a.command(0, 1, 0.0) == CommandError::None);
    REQUIRE(b.command(0, 1, 0.0) == CommandError::None);
    play(a);
    play(b);
    const std::vector<BattleEvent> drained = a.drainEvents();
    const std::span<const BattleEvent> viewed = b.events();
    REQUIRE(viewed.size() == drained.size());
    for (std::size_t i = 0; i < drained.size(); ++i) {
        CHECK(viewed[i].type == drained[i].type);
        CHECK(viewed[i].amount == drained[i].amount);
        CHECK(viewed[i].label == drained[i].label);
    }
    b.clearEvents();
    CHECK(b.events().empty());
    CHECK(a.drainEvents().empty());
}

TEST_CASE("living(out), livingCount() and turnOrder(span) match the allocating forms") {
    Battle b({hero(20), hero(15)}, {monster(10), monster(5)}, tuning());
    b.start(0.0);
    std::vector<CombatantId> buf;
    for (Team t : {Team::Party, Team::Enemies}) {
        b.living(t, buf);
        CHECK(buf == b.living(t));
        CHECK(b.livingCount(t) == buf.size());
    }
    std::array<CombatantId, 7> order{};
    const std::size_t n = b.turnOrder(std::span<CombatantId>(order.data(), order.size()));
    const auto expected = b.turnOrder(7);
    REQUIRE(n == expected.size());
    for (std::size_t i = 0; i < n; ++i) CHECK(order[i] == expected[i]);
}

TEST_CASE("a running battle does not allocate once it has warmed up") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    Battle b({hero(), hero(18)}, {monster(12, 4000), monster(8, 4000)}, musicTuning());
    b.start(0.0);
    std::vector<CombatantId> scratch;
    std::array<CombatantId, 8> order{};
    double now = 0.0;
    std::uint64_t before = 0;
    int skills = 0, enemyTurns = 0;
    // Plays whole turns (attacks and defences) with press()/update() and the in-place event API.
    for (int step = 0; step < 20000 && skills < 14; ++step) {
        now += 0.02;
        if (b.phase() == Phase::AwaitingCommand) {
            b.living(Team::Enemies, scratch);
            const int ap = b.combatant(b.actor()).ap; // mix of strike / heavy (2 AP) / sweep (1 AP)
            const std::size_t skill = skills % 3 == 1 && ap >= 2 ? 1 : skills % 3 == 2 && ap >= 1 ? 2 : 0;
            REQUIRE(b.command(skill, scratch.front(), now) == CommandError::None);
            ++skills;
        }
        if (b.phase() == Phase::EnemySkill && b.combo().judged() == 0 && now < b.combo().note(0).time) ++enemyTurns;
        if (b.phase() == Phase::PartySkill || b.phase() == Phase::EnemySkill) {
            const ComboSequence& c = b.combo();
            for (std::size_t i = 0; i < c.size(); ++i)
                if (c.grade(i) == Grade::Pending && c.note(i).time <= now) b.press(c.note(i).arrow, c.note(i).time);
        }
        b.update(now);
        (void)b.turnOrder(std::span<CombatantId>(order.data(), 7));
        b.clearEvents();
        if (skills == 6 && before == 0) before = ftcore_test::allocStats().count; // warmed up: both pools grown
    }
    REQUIRE(skills >= 14);
    CHECK(enemyTurns > 0);
    CHECK(ftcore_test::allocStats().count == before);
}

TEST_CASE("Battle::reset starts over like a new battle and a same-shaped reset does not allocate") {
    const std::vector<Combatant> party = {hero()};
    const std::vector<Combatant> foes = {monster()};
    Battle b(party, foes, tuning());
    b.start(0.0);
    REQUIRE(b.phase() != Phase::NotStarted);
    b.command(0, 1, 0.1);
    b.update(0.2);

    b.reset(party, foes, tuning()); // warm up the reuse path
    const std::uint64_t before = ftcore_test::allocStats().count;
    b.reset(party, foes, tuning());
    CHECK(ftcore_test::allocStats().count == before);

    CHECK(b.phase() == Phase::NotStarted);
    CHECK(b.events().empty());
    CHECK(b.combo().size() == 0);
    REQUIRE(b.combatants().size() == 2);
    CHECK(b.combatant(0).hp == 100);
    CHECK(b.combatant(0).team == Team::Party);
    CHECK(b.combatant(1).team == Team::Enemies);
    b.start(5.0);
    Battle fresh(party, foes, tuning());
    fresh.start(5.0);
    CHECK(b.phase() == fresh.phase());
    CHECK(b.actor() == fresh.actor());
}
