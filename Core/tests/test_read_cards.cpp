// Read cards (ReadCards.h) and the Turns verb Read: the deal, the pick, the Read counter and its
// stacks, the misread, and the autopilot reading through whole fights.
#include "AllocCounter.h"
#include "ftcore/Battle.h"
#include "ftcore/BattleDemo.h"
#include "ftcore/Encounter.h"
#include "ftcore/ReadCards.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <ostream> // doctest prints std::string_view through operator<<
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

std::vector<ReadCard> poolOf(std::initializer_list<std::string_view> tells) {
    std::vector<ReadCard> pool;
    std::uint32_t action = 0;
    for (std::string_view t : tells) pool.push_back({.actor = 7, .action = action++, .tell = t});
    return pool;
}

// Every card's tell is unlike every other's, and exactly one card is the right one.
void checkDeal(const ReadCards& cards, const ReadCard& right) {
    REQUIRE(cards.dealt());
    REQUIRE(cards.correct() < cards.size());
    int matches = 0;
    for (std::size_t i = 0; i < cards.size(); ++i) {
        matches += cards[i].tell == right.tell;
        for (std::size_t j = i + 1; j < cards.size(); ++j) CHECK(cards[i].tell != cards[j].tell);
    }
    CHECK(matches == 1);
    CHECK(cards[cards.correct()].tell == right.tell);
    CHECK(cards[cards.correct()].actor == right.actor);
    CHECK(cards[cards.correct()].action == right.action);
}

Combatant member(const char* name, std::uint8_t verbs) {
    Combatant c;
    c.name = name;
    c.stats = {100, 20, 0, 5, 10};
    c.hp = 100;
    c.verbs = verbs;
    c.skills = {{"Strike", 0, 1, Targeting::SingleEnemy, 1.0f, 4, 1.0f}};
    return c;
}

// A fast foe with three actions (three tells), so it attacks first and a deal has three cards.
Combatant reading(int poise = 0) {
    Combatant c;
    c.name = "Foe";
    c.stats = {500, 10, 0, 50, 10};
    c.stats.maxPoise = poise;
    c.hp = 500;
    c.skills = {{"Claw", 0, 0, Targeting::SingleEnemy, 1.0f, 4, 2.0f},
                {"Bite", 0, 0, Targeting::SingleEnemy, 1.0f, 4, 2.0f},
                {"Howl", 0, 0, Targeting::SingleEnemy, 1.0f, 4, 2.0f}};
    c.skills[2].tellId = "rears_back"; // a tell of its own; the others show their names
    return c;
}

std::vector<Combatant> readers() {
    return {member("Lead", 0), member("Reader", verbBit(Verb::Read))};
}

std::size_t wrongCard(const ReadCards& cards) { return (cards.correct() + 1) % cards.size(); }

const BattleEvent* find(const std::vector<BattleEvent>& seen, EventType type, std::string_view label = {}) {
    for (const BattleEvent& e : seen)
        if (e.type == type && (label.empty() || e.label == label)) return &e;
    return nullptr;
}

void drain(Battle& b, std::vector<BattleEvent>& seen) {
    for (const BattleEvent& e : b.events()) seen.push_back(e);
    b.clearEvents();
}

// Steps (no defence: the hits land) until the enemy attack in progress has ended.
void finishAttack(Battle& b, double& now, std::vector<BattleEvent>& seen) {
    for (int i = 0; i < 2000 && b.phase() == Phase::EnemySkill; ++i) {
        now += 1.0 / 120.0;
        b.update(now);
        drain(b, seen);
    }
}

// Steps until the next enemy attack has begun (its events in `seen`): the party strikes the first
// living enemy and plays every note, and an attack in progress lands.
bool toEnemyAttack(Battle& b, double& now, std::vector<BattleEvent>& seen) {
    seen.clear();
    finishAttack(b, now, seen);
    seen.clear();
    for (int i = 0; i < 20000; ++i) {
        if (b.phase() == Phase::Victory || b.phase() == Phase::Defeat) return false;
        if (b.phase() == Phase::AwaitingCommand && b.command(0, b.living(Team::Enemies).front(), now) != CommandError::None)
            return false;
        demo::pilotPress(b, now);
        now += 1.0 / 120.0;
        b.update(now);
        drain(b, seen);
        if (b.phase() == Phase::EnemySkill) return true;
    }
    return false;
}

int counterDamage(const Combatant& by, const Combatant& on, float power) {
    const float raw = static_cast<float>(by.stats.attack) * power * 100.f / (100.f + static_cast<float>(on.stats.defense) * 5.f);
    return std::max(1, static_cast<int>(std::lround(raw)));
}

} // namespace

TEST_CASE("read cards: a deal holds the coming action once, among decoys with other tells") {
    const std::vector<ReadCard> pool = poolOf({"lunge", "sweep", "roar", "coil", "lunge", "dive"});
    std::array<int, kReadCards> where{};
    for (std::uint64_t seed = 1; seed <= 300; ++seed) {
        for (std::size_t right = 0; right < pool.size(); ++right) {
            CAPTURE(seed);
            CAPTURE(right);
            ReadCards cards;
            REQUIRE(cards.deal(pool, right, seed) == kReadCards);
            checkDeal(cards, pool[right]);
            ++where[cards.correct()];
            // The same pool and seed deal the same cards.
            ReadCards again;
            again.deal(pool, right, seed);
            REQUIRE(again.size() == cards.size());
            CHECK(again.correct() == cards.correct());
            for (std::size_t i = 0; i < cards.size(); ++i) CHECK(again[i].action == cards[i].action);
        }
    }
    for (int n : where) CHECK(n > 300); // the right card comes in every place
}

TEST_CASE("read cards: fewer tells deal fewer cards, and one tell deals none") {
    ReadCards cards;
    const std::vector<ReadCard> two = poolOf({"lunge", "lunge", "sweep"});
    CHECK(cards.deal(two, 0, 9) == 2);
    checkDeal(cards, two[0]);
    const std::vector<ReadCard> one = poolOf({"lunge", "lunge"});
    CHECK(cards.deal(one, 1, 9) == 0);
    CHECK_FALSE(cards.dealt());
    CHECK(cards.deal(two, 3, 9) == 0); // no such action
    CHECK(cards.deal({}, 0, 9) == 0);
}

TEST_CASE("read cards: a pick is Correct for the right card, Wrong for a decoy, and comes once") {
    const std::vector<ReadCard> pool = poolOf({"lunge", "sweep", "roar"});
    ReadCards cards;
    CHECK(cards.pick(0) == ReadPick::Refused); // nothing dealt
    cards.deal(pool, 1, 42);
    CHECK(cards.pick(kReadCards) == ReadPick::Refused);
    CHECK_FALSE(cards.picked());
    CHECK(cards.pick(cards.correct()) == ReadPick::Correct);
    CHECK(cards.picked());
    CHECK(cards.pickedIndex() == cards.correct());
    CHECK(cards.pick(wrongCard(cards)) == ReadPick::Refused);
    cards.deal(pool, 1, 42); // a fresh deal is open again
    CHECK_FALSE(cards.picked());
    CHECK(cards.pick(wrongCard(cards)) == ReadPick::Wrong);
    cards.clear();
    CHECK(cards.size() == 0);
    CHECK(cards.pick(0) == ReadPick::Refused);
}

TEST_CASE("read: the reader is dealt the coming action's card as the enemy attack starts") {
    Battle b(readers(), {reading()});
    b.start(0.0);
    std::vector<BattleEvent> seen;
    drain(b, seen);
    REQUIRE(b.phase() == Phase::EnemySkill);
    REQUIRE(b.reader() == std::optional<CombatantId>(1));
    const BattleEvent* dealt = find(seen, EventType::CardsDealt);
    REQUIRE(dealt);
    CHECK(dealt->source == 2);
    CHECK(dealt->target == 1);
    CHECK(dealt->amount == 3);
    const ReadCards& cards = b.readCards();
    REQUIRE(cards.size() == 3);
    const ReadCard& right = cards[cards.correct()];
    CHECK(right.actor == b.actor());
    CHECK(right.action == 0);
    CHECK(right.tell == tellOf(*b.activeSkill()));
    CHECK(right.tell == "Claw");
    for (std::size_t i = 0; i < cards.size(); ++i) // the third action shows its own tell, not its name
        CHECK(cards[i].tell != "Howl");
    // Over the fight, the right card is always the one whose tell the attack shows.
    double now = 0.0;
    for (int attack = 0; attack < 6; ++attack) {
        REQUIRE(toEnemyAttack(b, now, seen));
        REQUIRE(b.readCards().size() == 3);
        CHECK(b.readCards()[b.readCards().correct()].tell == tellOf(*b.activeSkill()));
    }
}

TEST_CASE("read: a correct pick brings a free Read counter as the attack ends, and stacks") {
    Battle b(readers(), {reading(40)});
    b.start(0.0);
    double now = 0.0;
    std::vector<BattleEvent> seen;
    drain(b, seen);
    seen.clear();
    // Refused picks change nothing.
    CHECK(b.pickCard(kReadCards, 0.1) == CommandError::InvalidTarget);
    CHECK_FALSE(b.readCards().picked());
    REQUIRE(b.pickCard(b.readCards().correct(), 0.1) == CommandError::None);
    CHECK(b.pickCard(wrongCard(b.readCards()), 0.1) == CommandError::WrongPhase); // once per attack
    drain(b, seen);
    const BattleEvent* picked = find(seen, EventType::CardPicked);
    REQUIRE(picked);
    CHECK(picked->label == "READ");
    CHECK(picked->grade == Grade::Perfect);
    CHECK(picked->source == 1);
    CHECK(picked->target == 2);
    CHECK(picked->amount == 1);
    CHECK(b.combatant(1).readStacks == 1);
    CHECK_FALSE(find(seen, EventType::Counter)); // not before the attack ends
    finishAttack(b, now, seen);
    CHECK(find(seen, EventType::Defended, "HIT")); // no defence: the hit landed, the counter comes anyway
    const BattleEvent* counter = find(seen, EventType::Counter, "READ");
    REQUIRE(counter);
    CHECK(counter->source == 1);
    CHECK(counter->target == 2);
    CHECK(counter->amount == counterDamage(b.combatant(1), b.combatant(2), 0.5f));
    const BattleEvent* hit = find(seen, EventType::Hit, "READ");
    REQUIRE(hit);
    CHECK(hit->amount == counter->amount);
    CHECK(b.combatant(2).poise == 30); // counterPoise
    CHECK(b.stats().reads == 1);
    CHECK(b.stats().correctReads == 1);
    CHECK(b.stats().counters == 1);

    // A second correct Read in a row: two stacks, the counter at readPower x (1 + readStackPower).
    REQUIRE(toEnemyAttack(b, now, seen));
    REQUIRE(b.pickCard(b.readCards().correct(), now) == CommandError::None);
    CHECK(b.combatant(1).readStacks == 2);
    finishAttack(b, now, seen);
    counter = find(seen, EventType::Counter, "READ");
    REQUIRE(counter);
    CHECK(counter->amount == counterDamage(b.combatant(1), b.combatant(2), 0.6f));
    CHECK(b.combatant(2).poise == 20);

    // A misread: the stacks go and the attacker gets readWrongPoise back; no counter.
    REQUIRE(toEnemyAttack(b, now, seen));
    seen.clear();
    REQUIRE(b.pickCard(wrongCard(b.readCards()), now) == CommandError::None);
    drain(b, seen);
    picked = find(seen, EventType::CardPicked);
    REQUIRE(picked);
    CHECK(picked->label == "MISREAD");
    CHECK(picked->grade == Grade::Miss);
    CHECK(picked->amount == 10);
    CHECK(b.combatant(1).readStacks == 0);
    CHECK(b.combatant(2).poise == 30);
    finishAttack(b, now, seen);
    CHECK_FALSE(find(seen, EventType::Counter, "READ"));
    CHECK(b.stats().reads == 3);
    CHECK(b.stats().correctReads == 2);
}

TEST_CASE("read: stacks stop at the cap, a misread never lifts poise past its max, and a pick closes at the first impact") {
    Battle b(readers(), {reading(40)});
    b.start(0.0);
    double now = 0.0;
    std::vector<BattleEvent> seen;
    // A misread on a full stance gives nothing back.
    REQUIRE(b.pickCard(wrongCard(b.readCards()), 0.1) == CommandError::None);
    drain(b, seen);
    CHECK(find(seen, EventType::CardPicked, "MISREAD")->amount == 0);
    CHECK(b.combatant(2).poise == 40);
    for (int attack = 0; attack < kMaxReadStacks + 2; ++attack) {
        REQUIRE(toEnemyAttack(b, now, seen));
        REQUIRE(b.pickCard(b.readCards().correct(), now) == CommandError::None);
    }
    CHECK(b.combatant(1).readStacks == kMaxReadStacks);
    // Past the first hit's impact the pick is refused.
    REQUIRE(toEnemyAttack(b, now, seen));
    const double impact = b.defence().impact(0);
    CHECK(b.pickCard(b.readCards().correct(), impact + 0.01) == CommandError::WrongPhase);
    CHECK(b.pickCard(b.readCards().correct(), impact) == CommandError::None);
}

TEST_CASE("read: no reader, no deal; a pick outside an attack is refused") {
    Battle b({member("Lead", 0), member("Other", 0)}, {reading()});
    b.start(0.0);
    std::vector<BattleEvent> seen;
    drain(b, seen);
    REQUIRE(b.phase() == Phase::EnemySkill);
    CHECK_FALSE(b.reader());
    CHECK_FALSE(find(seen, EventType::CardsDealt));
    CHECK_FALSE(b.readCards().dealt());
    CHECK(b.pickCard(0, 0.1) == CommandError::WrongPhase);
    // One action, one tell: nothing to read.
    Combatant single = reading();
    single.skills.resize(1);
    Battle lone(readers(), {single});
    lone.start(0.0);
    seen.clear();
    drain(lone, seen);
    CHECK_FALSE(find(seen, EventType::CardsDealt));
    // On the party's turn there are no cards.
    double now = 0.0;
    Battle c(readers(), {reading()});
    c.start(0.0);
    for (int i = 0; i < 4000 && c.phase() != Phase::AwaitingCommand; ++i) c.update(now += 1.0 / 120.0);
    REQUIRE(c.phase() == Phase::AwaitingCommand);
    CHECK_FALSE(c.readCards().dealt());
    CHECK(c.pickCard(0, now) == CommandError::WrongPhase);
}

TEST_CASE("read: the deal follows the battle seed") {
    const auto deals = [](std::uint64_t seed) {
        BattleTuning t;
        t.seed = seed;
        Battle b(readers(), {reading(), reading()}, t);
        b.start(0.0);
        double now = 0.0;
        std::vector<BattleEvent> seen;
        std::vector<std::uint32_t> out;
        for (int attack = 0; attack < 8; ++attack) {
            const ReadCards& cards = b.readCards();
            REQUIRE(cards.size() == 3);
            out.push_back(static_cast<std::uint32_t>(cards.correct()));
            for (const ReadCard& c : cards.cards()) out.push_back(c.actor * 16 + c.action);
            if (!toEnemyAttack(b, now, seen)) break;
        }
        return out;
    };
    CHECK(deals(5) == deals(5));
    CHECK(deals(5) != deals(6));
}

TEST_CASE("read: the enemy data takes an optional skills[].tellId") {
    EnemyRoster roster;
    std::string err;
    std::vector<std::string> warnings;
    const auto load = [&](const char* skill) {
        const std::string json = std::string(R"({"enemies":[{"id":"pup","stats":{"maxHp":50},"skills":[)") + skill + "]}]}";
        err.clear();
        warnings.clear();
        return importEnemyRoster(json, roster, &err, &warnings);
    };
    REQUIRE_MESSAGE(load(R"({"id":"bite","power":1.0,"tellId":"bares_teeth"})"), err);
    CHECK(warnings.empty());
    CHECK(tellOf(roster.enemies[0].skills[0]) == "bares_teeth");
    REQUIRE_MESSAGE(load(R"({"id":"bite","name":"Bite","power":1.0})"), err);
    CHECK(tellOf(roster.enemies[0].skills[0]) == "Bite");
    for (const char* bad : {R"({"id":"bite","tellId":""})", R"({"id":"bite","tellId":3})"}) {
        CAPTURE(bad);
        CHECK_FALSE(load(bad));
        CHECK(err.find("enemies[0].skills[0].tellId") != std::string::npos);
    }
}

TEST_CASE("read pilot: the demo party wins reading every attack, and a shaky reader misreads") {
    for (const double step : {1.0 / 120.0, 0.13}) {
        CAPTURE(step);
        const demo::HeadlessResult r =
            demo::playHeadless(demo::party(4), demo::enemies(), true, step, 1200.0, DefenceInput::Guard);
        CHECK_FALSE(r.commandFailed);
        CHECK(r.outcome == Phase::Victory);
        CHECK(r.reads > 0);
        CHECK(r.correctReads == r.reads);
        CHECK(r.intercepts > 0);
    }
    for (const double step : {1.0 / 120.0, 0.13}) {
        CAPTURE(step);
        const demo::HeadlessResult r = demo::playHeadless(demo::party(4), demo::enemies(), true, step, 1200.0,
                                                          DefenceInput::Guard, {0.85, 0.75});
        CHECK_FALSE(r.commandFailed);
        CHECK(r.outcome == Phase::Victory);
        CHECK(r.correctReads > 0);
        CHECK(r.correctReads < r.reads);
    }
    // The Parry pilot never reads, and the idle player still loses.
    const demo::HeadlessResult parry = demo::playHeadless(demo::party(4), demo::enemies(), true);
    CHECK(parry.outcome == Phase::Victory);
    CHECK(parry.reads == 0);
    const demo::HeadlessResult idle =
        demo::playHeadless(demo::party(4), demo::enemies(), false, 1.0 / 120.0, 1200.0, DefenceInput::Guard);
    CHECK(idle.outcome == Phase::Defeat);
}

TEST_CASE("read pilot: a whole fight dealing and picking cards allocates nothing per step") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    Battle b(demo::party(4), demo::enemies(), demo::tuning());
    const demo::PilotSkill skill{0.85, 0.75};
    demo::PilotState state;
    const double step = 1.0 / 120.0;
    double now = 0.0;
    b.start(now);
    std::uint64_t allocs = 0;
    BattleStats warm{};
    for (int steps = 0; steps < 200000 && b.phase() != Phase::Victory && b.phase() != Phase::Defeat; ++steps) {
        const auto before = ftcore_test::allocStats().count;
        if (b.phase() == Phase::AwaitingCommand) {
            const demo::PilotChoice choice = demo::pilotChoice(b);
            REQUIRE((choice.release ? b.release(now) : b.command(choice.skill, choice.target, now)) == CommandError::None);
        }
        demo::pilotRead(b, now, skill, state);
        demo::pilotPress(b, now, skill, state);
        demo::pilotDefend(b, now + step, DefenceInput::Guard, skill, state);
        now += step;
        b.update(now);
        b.clearEvents();
        const auto spent = ftcore_test::allocStats().count - before;
        if (now < 20.0) warm = b.stats(); // the first seconds grow the pools
        else allocs += spent;
    }
    CHECK(b.phase() == Phase::Victory);
    CHECK(b.stats().reads > warm.reads);
    CHECK(b.stats().correctReads > warm.correctReads);
    CHECK(allocs == 0);
}
