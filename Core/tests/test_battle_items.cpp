#include "ftcore/Battle.h"
#include "ftcore/BattleDemo.h"
#include "ftcore/Inventory.h"

#include "AllocCounter.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <ostream> // doctest prints std::string_view through operator<<
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

Skill basicAttack() { return {"Strike", 0, 1, Targeting::SingleEnemy, 1.0f, 4, 1.0f}; }

Combatant hero(int speed = 20, int hp = 100) {
    Combatant c;
    c.name = "Hero";
    c.stats = {hp, 20, 0, speed, 10};
    c.hp = hp;
    c.skills = {basicAttack()};
    return c;
}

// A party member who is down when the battle starts.
Combatant fallen(int speed = 10) {
    Combatant c = hero(speed);
    c.hp = 0;
    return c;
}

Combatant monster(int speed = 10, int hp = 200, int attack = 10) {
    Combatant c;
    c.name = "Monster";
    c.stats = {hp, attack, 0, speed, 10};
    c.hp = hp;
    c.skills = {{"Claw", 0, 0, Targeting::SingleEnemy, 1.0f, 4, 2.0f}};
    return c;
}

// Its blows deal nothing, so HP and AP only change by items.
Combatant harmless() { return monster(10, 200, 0); }

BattleTuning tuning() {
    BattleTuning t;
    t.leadIn = 0.5;
    t.recovery = 0.5;
    return t;
}

// Max HP is 100 in these tests unless a hero says otherwise.
constexpr const char* kItems = R"J({ "items": [
    { "id": "small", "effect": "heal", "hp": 30, "hpPercent": 10, "buyPrice": 10, "sellPrice": 5 },
    { "id": "big", "effect": "heal", "hp": 500, "buyPrice": 10, "sellPrice": 5 },
    { "id": "rations", "effect": "heal", "scope": "party", "hp": 20, "hpPercent": 10, "buyPrice": 10, "sellPrice": 5 },
    { "id": "bar", "effect": "restore_ap", "ap": 2, "buyPrice": 10, "sellPrice": 5 },
    { "id": "mix", "effect": "restore_ap", "scope": "party", "ap": 1, "buyPrice": 10, "sellPrice": 5 },
    { "id": "bell", "effect": "revive", "hpPercent": 30, "buyPrice": 10, "sellPrice": 5 },
    { "id": "chime", "effect": "revive", "scope": "party", "hpPercent": 20, "buyPrice": 10, "sellPrice": 5 } ] })J";

ItemTable itemTable() {
    ItemTable t;
    std::string err;
    REQUIRE_MESSAGE(importItemTable(kItems, t, &err), err);
    return t;
}

Inventory bag(std::initializer_list<std::pair<const char*, int>> stacks) {
    Inventory b;
    for (const auto& s : stacks) REQUIRE(b.add(s.first, s.second) == s.second);
    return b;
}

// Loads the items into a battle that was just built and starts it at time 0. The start events are
// cleared, not drained: drainEvents() hands the battle's reserved event buffer away, and the
// allocation tests need it.
void begin(Battle& b, const Inventory& carried) {
    b.setItems(itemTable(), carried);
    b.start(0.0);
    b.clearEvents();
}

// A battle with the party carrying `carried`, started at time 0 and with its start events cleared.
Battle started(std::vector<Combatant> party, const Inventory& carried, std::vector<Combatant> foes = {harmless()}) {
    Battle b(std::move(party), std::move(foes), tuning());
    begin(b, carried);
    return b;
}

std::size_t slotOf(const Battle& b, std::string_view id) {
    const Inventory& carried = b.carriedItems();
    for (std::size_t i = 0; i < carried.size(); ++i)
        if (carried[i].id.view() == id) return i;
    return carried.size();
}

// Runs the clock, with no presses and no defence, until a party member has to choose again.
void toNextCommand(Battle& b, double& now) {
    while (b.phase() != Phase::AwaitingCommand) {
        REQUIRE(b.phase() != Phase::Victory);
        REQUIRE(b.phase() != Phase::Defeat);
        now += 0.05;
        REQUIRE(now < 1000.0);
        b.update(now);
        b.clearEvents();
    }
}

std::vector<BattleEvent> itemEvents(Battle& b) {
    std::vector<BattleEvent> out;
    for (const BattleEvent& e : b.drainEvents())
        if (e.type == EventType::ItemUsed) out.push_back(e);
    return out;
}

} // namespace

TEST_CASE("items: a heal restores HP, spends the turn, and the next turn follows like after a skill") {
    Combatant wounded = hero(15);
    wounded.hp = 50;
    Battle b = started({hero(20), wounded}, bag({{"small", 2}}));
    REQUIRE(b.phase() == Phase::AwaitingCommand);
    REQUIRE(b.actor() == 0);
    const int apBefore = b.combatant(0).ap;

    CHECK(b.useItem(slotOf(b, "small"), 1, 0.0) == CommandError::None);
    CHECK(b.combatant(1).hp == 90); // 30 plus 10 percent of 100
    CHECK(b.combatant(0).ap == apBefore); // an item costs the turn, not AP
    CHECK(b.phase() == Phase::Recovery);
    const std::vector<BattleEvent> events = b.drainEvents();
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == EventType::ItemUsed);
    CHECK(events[0].source == 0);
    CHECK(events[0].target == 1);
    CHECK(events[0].amount == 40);
    CHECK(events[0].label == "small");
    CHECK(b.carriedItems().count("small") == 1);

    // After the recovery pause the next member in the turn order is up, as after a skill.
    b.update(0.4);
    CHECK(b.phase() == Phase::Recovery);
    b.update(0.5);
    CHECK(b.phase() == Phase::AwaitingCommand);
    CHECK(b.actor() == 1);
    CHECK(b.useItem(slotOf(b, "small"), 1, 0.5) == CommandError::None); // and it can use one too
    CHECK(b.combatant(1).hp == 100);
    const std::vector<BattleEvent> second = itemEvents(b);
    REQUIRE(second.size() == 1);
    CHECK(second[0].source == 1); // the member whose turn it was
    CHECK(second[0].amount == 10);
}

TEST_CASE("items: HP and AP stop at the maximum, and a target at the maximum still uses the item up") {
    Combatant nearly = hero(15);
    nearly.hp = 95;
    Combatant lowAp = hero(10);
    lowAp.maxAp = 3; // starts with 2
    Battle b = started({hero(20), nearly, lowAp}, bag({{"small", 1}, {"big", 1}, {"bar", 2}}));
    double now = 0.0;

    REQUIRE(b.combatant(2).ap == 2);
    CHECK(b.useItem(slotOf(b, "small"), 1, now) == CommandError::None); // 40 would pass the maximum
    CHECK(b.combatant(1).hp == 100);
    std::vector<BattleEvent> events = itemEvents(b);
    REQUIRE(events.size() == 1);
    CHECK(events[0].amount == 5);

    toNextCommand(b, now);
    CHECK(b.useItem(slotOf(b, "big"), 0, now) == CommandError::None); // already at full HP
    CHECK(b.combatant(0).hp == 100);
    events = itemEvents(b);
    REQUIRE(events.size() == 1);
    CHECK(events[0].amount == 0);
    CHECK(b.carriedItems().count("big") == 0); // used up all the same

    toNextCommand(b, now);
    CHECK(b.useItem(slotOf(b, "bar"), 2, now) == CommandError::None); // 2 AP offered, 1 fits
    CHECK(b.combatant(2).ap == 3);
    events = itemEvents(b);
    REQUIRE(events.size() == 1);
    CHECK(events[0].amount == 1);

    toNextCommand(b, now);
    CHECK(b.useItem(slotOf(b, "bar"), 0, now) == CommandError::None); // plenty of room
    CHECK(b.combatant(0).ap == 4);
}

TEST_CASE("items: a restore is a percent of the recipient's max HP, not of its current HP") {
    Combatant tough = hero(15, 300);
    tough.hp = 10;
    Battle b = started({hero(20), tough, fallen()}, bag({{"small", 1}, {"bell", 1}}));
    double now = 0.0;
    CHECK(b.useItem(slotOf(b, "small"), 1, now) == CommandError::None);
    CHECK(b.combatant(1).hp == 10 + 30 + 30); // 30 flat plus 10 percent of 300
    toNextCommand(b, now);
    CHECK(b.useItem(slotOf(b, "bell"), 2, now) == CommandError::None);
    CHECK(b.combatant(2).hp == 30); // 30 percent of 100
}

TEST_CASE("items: wrong targets are refused and change nothing") {
    Combatant wounded = hero(15);
    wounded.hp = 40;
    Battle b = started({hero(20), wounded, fallen()}, bag({{"small", 1}, {"bar", 1}, {"bell", 1}}));
    const std::size_t small = slotOf(b, "small"), bar = slotOf(b, "bar"), bell = slotOf(b, "bell");
    const int enemy = 3;
    REQUIRE(b.combatant(enemy).team == Team::Enemies);

    CHECK(b.useItem(small, 2, 0.0) == CommandError::InvalidTarget);   // a heal on a downed ally
    CHECK(b.useItem(bar, 2, 0.0) == CommandError::InvalidTarget);     // AP to a downed ally
    CHECK(b.useItem(small, enemy, 0.0) == CommandError::InvalidTarget);
    CHECK(b.useItem(bar, enemy, 0.0) == CommandError::InvalidTarget);
    CHECK(b.useItem(bell, enemy, 0.0) == CommandError::InvalidTarget);
    CHECK(b.useItem(bell, 1, 0.0) == CommandError::InvalidTarget);    // a revive on a living ally
    CHECK(b.useItem(bell, 0, 0.0) == CommandError::InvalidTarget);    // even the one using it
    CHECK(b.useItem(small, 99, 0.0) == CommandError::InvalidTarget);  // no such combatant

    // Nothing happened: same phase, same turn, same HP, no events, every item still there.
    CHECK(b.phase() == Phase::AwaitingCommand);
    CHECK(b.actor() == 0);
    CHECK(b.combatant(1).hp == 40);
    CHECK(b.combatant(2).hp == 0);
    CHECK(b.events().empty());
    CHECK(b.carriedItems().size() == 3);
    CHECK(b.stats().itemsUsed == 0);
    CHECK(b.livingCount(Team::Party) == 2);

    // The right targets work.
    CHECK(b.useItem(small, 1, 0.0) == CommandError::None);
    CHECK(b.combatant(1).hp == 80);
}

TEST_CASE("items: an empty slot, a slot past the end and an item the table does not know are NoSuchItem") {
    // Nothing loaded at all.
    {
        Battle b({hero()}, {harmless()}, tuning());
        b.start(0.0);
        CHECK(b.carriedItems().empty());
        CHECK(b.itemTable().empty());
        CHECK(b.useItem(0, 0, 0.0) == CommandError::NoSuchItem);
    }
    Battle b = started({hero(20), hero(15)}, bag({{"small", 1}, {"mystery", 3}}));
    CHECK(b.useItem(2, 0, 0.0) == CommandError::NoSuchItem); // one past the last stack
    CHECK(b.useItem(1000, 0, 0.0) == CommandError::NoSuchItem);
    CHECK(b.useItem(static_cast<std::size_t>(-1), 0, 0.0) == CommandError::NoSuchItem);
    CHECK(b.useItem(slotOf(b, "mystery"), 0, 0.0) == CommandError::NoSuchItem); // carried, but the table has no entry
    CHECK(b.carriedItems().count("mystery") == 3); // and it stays in the bag for the write-back
    CHECK(b.phase() == Phase::AwaitingCommand);

    // The slot is checked before the target.
    CHECK(b.useItem(5, 99, 0.0) == CommandError::NoSuchItem);

    // A stack that has been used up is gone: its slot is empty now.
    CHECK(b.useItem(slotOf(b, "small"), 1, 0.0) == CommandError::None);
    CHECK(b.carriedItems().size() == 1);
    CHECK(slotOf(b, "small") == b.carriedItems().size());
}

TEST_CASE("items: only a party member choosing a command can use one") {
    Battle b({hero(20), hero(15)}, {monster(10, 100000)}, tuning());
    b.setItems(itemTable(), bag({{"small", 9}}));
    CHECK(b.phase() == Phase::NotStarted);
    CHECK(b.useItem(0, 0, 0.0) == CommandError::WrongPhase);
    b.start(0.0);
    b.drainEvents();
    REQUIRE(b.phase() == Phase::AwaitingCommand);

    // A skill is under way.
    REQUIRE(b.command(0, 2, 0.0) == CommandError::None);
    REQUIRE(b.phase() == Phase::PartySkill);
    CHECK(b.useItem(0, 0, 0.1) == CommandError::WrongPhase);

    // The same battle, in the recovery after an item.
    Battle c({hero(20), hero(15)}, {monster(10, 100000)}, tuning());
    c.setItems(itemTable(), bag({{"small", 9}}));
    c.start(0.0);
    REQUIRE(c.useItem(0, 1, 0.0) == CommandError::None);
    REQUIRE(c.phase() == Phase::Recovery);
    CHECK(c.useItem(0, 1, 0.1) == CommandError::WrongPhase); // one item per turn
    CHECK(c.carriedItems().count("small") == 8);

    // Aiming.
    Battle d({hero(20), hero(15)}, {monster(10, 100000)}, tuning());
    d.setItems(itemTable(), bag({{"small", 9}}));
    d.start(0.0);
    REQUIRE(d.aim(2, 0.0) == CommandError::None);
    REQUIRE(d.phase() == Phase::PartyAim);
    CHECK(d.useItem(0, 0, 0.1) == CommandError::WrongPhase);

    // An enemy's turn.
    Battle e({hero(1), hero(1)}, {monster(50, 100000)}, tuning());
    e.setItems(itemTable(), bag({{"small", 9}}));
    e.start(0.0);
    REQUIRE(e.phase() == Phase::EnemySkill);
    CHECK(e.useItem(0, 0, 0.1) == CommandError::WrongPhase);
    CHECK(e.carriedItems().count("small") == 9);

    // And after the battle.
    Battle f({hero(20)}, {monster(10, 1)}, tuning());
    f.setItems(itemTable(), bag({{"small", 9}}));
    f.start(0.0);
    REQUIRE(f.aim(1, 0.0) == CommandError::None);
    f.setReticle(0.0f, 0.5f);
    REQUIRE(f.fire(0.1));
    REQUIRE(f.phase() == Phase::Victory);
    CHECK(f.useItem(0, 0, 0.2) == CommandError::WrongPhase);
}

TEST_CASE("items: a revive brings a downed ally back at a percent of its max HP, and it rejoins the turn queue") {
    Combatant big = fallen(15);
    big.stats.maxHp = 200;
    Battle b = started({hero(20), big}, bag({{"bell", 1}}));
    const auto queued = [&](CombatantId id) {
        const std::vector<CombatantId> order = b.turnOrder(8);
        return std::find(order.begin(), order.end(), id) != order.end();
    };
    REQUIRE(b.combatant(1).hp == 0);
    CHECK_FALSE(queued(1));
    CHECK(b.livingCount(Team::Party) == 1);

    CHECK(b.useItem(slotOf(b, "bell"), 1, 0.0) == CommandError::None);
    CHECK(b.combatant(1).hp == 60); // 30 percent of 200
    CHECK(b.combatant(1).alive());
    CHECK(queued(1));
    CHECK(b.livingCount(Team::Party) == 2);
    const std::vector<BattleEvent> events = itemEvents(b);
    REQUIRE(events.size() == 1);
    CHECK(events[0].source == 0);
    CHECK(events[0].target == 1);
    CHECK(events[0].amount == 60);
    CHECK(events[0].label == "bell");
    CHECK(b.stats().itemsUsed == 1);
}

TEST_CASE("items: a revived member takes turns again") {
    Battle b = started({hero(20), fallen(15)}, bag({{"bell", 1}}));
    double now = 0.0;
    REQUIRE(b.useItem(0, 1, now) == CommandError::None);

    // The others strike until the revived member's turn comes round.
    int guard = 0;
    while (!(b.phase() == Phase::AwaitingCommand && b.actor() == 1)) {
        REQUIRE(++guard < 20000);
        if (b.phase() == Phase::AwaitingCommand) REQUIRE(b.command(0, 2, now) == CommandError::None);
        now += 0.05;
        b.update(now);
        b.clearEvents();
    }
    CHECK(b.command(0, 2, now) == CommandError::None);
    CHECK(b.phase() == Phase::PartySkill);
    CHECK(b.activeSkill() != nullptr);
}

TEST_CASE("items: enemies pick on a revived member like on any other, the most wounded one") {
    Battle b = started({hero(20), fallen(15)}, bag({{"bell", 1}}), {monster(5, 100000)});
    REQUIRE(b.useItem(0, 1, 0.0) == CommandError::None);
    REQUIRE(b.combatant(1).hp == 30); // the other member is at 100
    double now = 0.0;
    int guard = 0;
    while (b.phase() != Phase::EnemySkill) {
        REQUIRE(++guard < 20000);
        if (b.phase() == Phase::AwaitingCommand) REQUIRE(b.command(0, 2, now) == CommandError::None);
        now += 0.05;
        b.update(now);
        b.clearEvents();
    }
    REQUIRE(b.activeTargets().size() == 1);
    CHECK(b.activeTargets().front() == 1);
}

TEST_CASE("items: a whole-party heal reaches every living member and leaves the downed ones down") {
    Combatant wounded = hero(15);
    wounded.hp = 50;
    Battle b = started({hero(20), wounded, fallen()}, bag({{"rations", 1}}));
    CHECK(b.useItem(slotOf(b, "rations"), 99 /* ignored */, 0.0) == CommandError::None);
    CHECK(b.combatant(0).hp == 100); // 30 offered, already full
    CHECK(b.combatant(1).hp == 80);  // 20 plus 10 percent of 100
    CHECK(b.combatant(2).hp == 0);   // still down
    CHECK_FALSE(b.combatant(2).alive());
    CHECK(b.combatant(3).hp == 200); // an enemy is not touched
    const std::vector<BattleEvent> events = itemEvents(b);
    REQUIRE(events.size() == 2); // one per recipient, in combatant order
    CHECK(events[0].target == 0);
    CHECK(events[0].amount == 0);
    CHECK(events[1].target == 1);
    CHECK(events[1].amount == 30);
    for (const BattleEvent& e : events) {
        CHECK(e.source == 0);
        CHECK(e.label == "rations");
    }
    CHECK(b.stats().itemsUsed == 1); // one item, however many it reached
    CHECK(b.carriedItems().empty());
    CHECK(b.phase() == Phase::Recovery);
}

TEST_CASE("items: a whole-party AP restore reaches every living member") {
    Combatant capped = hero(15);
    capped.maxAp = 2; // already full
    Battle b = started({hero(20), capped, fallen()}, bag({{"mix", 1}}));
    CHECK(b.useItem(slotOf(b, "mix"), 0, 0.0) == CommandError::None);
    CHECK(b.combatant(0).ap == 3);
    CHECK(b.combatant(1).ap == 2);
    CHECK(b.combatant(2).ap == 2); // the downed one keeps its AP (startingAp) untouched
    const std::vector<BattleEvent> events = itemEvents(b);
    REQUIRE(events.size() == 2);
    CHECK(events[0].amount == 1);
    CHECK(events[1].amount == 0);
}

TEST_CASE("items: a whole-party revive gets up every downed ally; with nobody down it is refused") {
    Battle b = started({hero(20), fallen(15), fallen(10)}, bag({{"chime", 2}}));
    CHECK(b.livingCount(Team::Party) == 1);
    CHECK(b.useItem(slotOf(b, "chime"), 0, 0.0) == CommandError::None);
    CHECK(b.livingCount(Team::Party) == 3);
    CHECK(b.combatant(1).hp == 20); // 20 percent of 100
    CHECK(b.combatant(2).hp == 20);
    const std::vector<BattleEvent> events = itemEvents(b);
    REQUIRE(events.size() == 2);
    CHECK(events[0].target == 1);
    CHECK(events[1].target == 2);
    const std::vector<CombatantId> order = b.turnOrder(12);
    CHECK(std::count(order.begin(), order.end(), 1u) > 0);
    CHECK(std::count(order.begin(), order.end(), 2u) > 0);

    // Everyone is up now: a second one has nobody to get up and is not spent.
    double now = 0.0;
    toNextCommand(b, now);
    CHECK(b.useItem(slotOf(b, "chime"), 0, now) == CommandError::InvalidTarget);
    CHECK(b.carriedItems().count("chime") == 1);
    CHECK(b.stats().itemsUsed == 1);
}

TEST_CASE("items: the counts go down by one a use, and what is left is what the game writes back") {
    Battle b = started({hero(20), hero(15)}, bag({{"small", 2}, {"mystery", 3}, {"bar", 1}}));
    double now = 0.0;
    CHECK(b.carriedItems().count("small") == 2);
    CHECK(b.useItem(slotOf(b, "small"), 0, now) == CommandError::None);
    CHECK(b.carriedItems().count("small") == 1);
    CHECK(b.stats().itemsUsed == 1);
    toNextCommand(b, now);
    CHECK(b.useItem(slotOf(b, "bar"), 1, now) == CommandError::None);
    CHECK(b.carriedItems().count("bar") == 0);
    CHECK(b.carriedItems().size() == 2); // the used-up stack left
    toNextCommand(b, now);
    CHECK(b.useItem(slotOf(b, "small"), 0, now) == CommandError::None);
    CHECK(b.stats().itemsUsed == 3);

    // Only the unknown item is left, and written back it is as it was carried.
    Inventory writeBack;
    writeBack = b.carriedItems();
    CHECK(writeBack.size() == 1);
    CHECK(writeBack.count("mystery") == 3);
    PartyProgress progress;
    progress.items = b.carriedItems(); // what the game does after the fight
    CHECK(progress.items == writeBack);

    // The slots close up: the one after a used-up stack moves into its place.
    Battle c = started({hero(20), hero(15)}, bag({{"bar", 1}, {"small", 2}}));
    CHECK(slotOf(c, "small") == 1);
    CHECK(c.useItem(0, 0, 0.0) == CommandError::None); // the bar is gone
    CHECK(slotOf(c, "small") == 0);
    double later = 0.0;
    toNextCommand(c, later);
    CHECK(c.useItem(1, 0, later) == CommandError::NoSuchItem); // slot 1 is empty now
    CHECK(c.useItem(0, 0, later) == CommandError::None);
}

TEST_CASE("items: the stats count the items used, not the members they reached") {
    Combatant wounded = hero(15);
    wounded.hp = 10;
    Battle b = started({hero(20), wounded, hero(10)}, bag({{"rations", 2}, {"small", 1}}));
    double now = 0.0;
    CHECK(b.stats().itemsUsed == 0);
    CHECK(b.useItem(slotOf(b, "rations"), 0, now) == CommandError::None); // three members
    CHECK(b.stats().itemsUsed == 1);
    toNextCommand(b, now);
    CHECK(b.useItem(slotOf(b, "small"), 2, now) == CommandError::None);
    CHECK(b.stats().itemsUsed == 2);
    toNextCommand(b, now);
    CHECK(b.useItem(slotOf(b, "rations"), 0, now) == CommandError::None);
    CHECK(b.stats().itemsUsed == 3);
    CHECK(b.stats().partyTurns >= 3);
}

TEST_CASE("items: the battle keeps its own copy, and an event's label points into it") {
    ItemTable table = itemTable();
    Inventory carried = bag({{"small", 2}});
    Battle b({hero(20), hero(15)}, {harmless()}, tuning());
    b.setItems(table, carried);
    table = ItemTable{}; // the caller's copies can go away
    carried.clear();
    b.start(0.0);
    b.drainEvents();
    CHECK(b.carriedItems().count("small") == 2);
    REQUIRE(b.itemTable().find("small") != nullptr);

    REQUIRE(b.useItem(0, 0, 0.0) == CommandError::None);
    REQUIRE(b.events().size() == 1);
    const BattleEvent& e = b.events()[0];
    CHECK(e.label == "small");
    CHECK(e.label.data() == b.itemTable().find("small")->id.view().data()); // battle-owned data
}

TEST_CASE("items: reset empties the battle's items, and the carried ones have to be loaded again") {
    const std::vector<Combatant> party = {hero(20), hero(15)};
    const std::vector<Combatant> foes = {harmless()};
    Battle b(party, foes, tuning());
    b.setItems(itemTable(), bag({{"small", 2}}));
    b.start(0.0);
    REQUIRE(b.useItem(0, 0, 0.0) == CommandError::None);
    REQUIRE(b.stats().itemsUsed == 1);

    b.reset(party, foes, tuning());
    CHECK(b.carriedItems().empty());
    CHECK(b.itemTable().empty());
    CHECK(b.stats().itemsUsed == 0);
    b.start(0.0);
    CHECK(b.useItem(0, 0, 0.0) == CommandError::NoSuchItem);

    // Loading them again before the next start works, with the counts the game holds.
    b.reset(party, foes, tuning());
    b.setItems(itemTable(), bag({{"small", 2}}));
    b.start(0.0);
    CHECK(b.useItem(0, 0, 0.0) == CommandError::None);
    CHECK(b.carriedItems().count("small") == 1);
}

TEST_CASE("items: useItem does not allocate, for any effect or scope") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    Combatant wounded = hero(15);
    wounded.hp = 20;
    Battle b({hero(20), wounded, fallen(12)}, {harmless()}, tuning());
    begin(b, bag({{"small", 1}, {"rations", 1}, {"bar", 1}, {"mix", 1}, {"bell", 1}, {"big", 1}}));
    double now = 0.0;
    struct Use {
        const char* id;
        CombatantId target;
    };
    const Use uses[] = {{"small", 1}, {"bar", 0}, {"bell", 2}, {"rations", 0}, {"mix", 0}, {"big", 1}};
    std::vector<CommandError> results;
    std::vector<std::uint64_t> spent;
    results.reserve(std::size(uses));
    spent.reserve(std::size(uses));
    for (const Use& u : uses) {
        toNextCommand(b, now);
        const std::size_t slot = slotOf(b, u.id);
        const std::uint64_t before = ftcore_test::allocStats().count;
        const CommandError e = b.useItem(slot, u.target, now);
        const std::uint64_t after = ftcore_test::allocStats().count;
        results.push_back(e);
        spent.push_back(after - before);
        b.clearEvents();
    }
    for (std::size_t i = 0; i < std::size(uses); ++i) {
        CAPTURE(uses[i].id);
        CHECK(results[i] == CommandError::None);
        CHECK(spent[i] == 0);
    }
    CHECK(b.stats().itemsUsed == 6);
}

TEST_CASE("items: reviving a member the enemy downed mid-fight does not allocate") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    Combatant frail = hero(5);
    frail.hp = 5;
    Battle b({hero(20), frail}, {monster(10, 100000)}, tuning());
    begin(b, bag({{"bell", 1}}));
    double now = 0.0;
    int guard = 0;
    while (b.combatant(1).alive() || b.phase() != Phase::AwaitingCommand) {
        REQUIRE(++guard < 20000);
        if (b.phase() == Phase::AwaitingCommand) REQUIRE(b.command(0, 2, now) == CommandError::None);
        now += 0.02;
        b.update(now);
        b.clearEvents();
    }
    REQUIRE_FALSE(b.combatant(1).alive());
    REQUIRE(b.actor() == 0);
    const std::size_t slot = slotOf(b, "bell");
    const std::uint64_t before = ftcore_test::allocStats().count;
    const CommandError e = b.useItem(slot, 1, now);
    const std::uint64_t after = ftcore_test::allocStats().count;
    CHECK(e == CommandError::None);
    CHECK(after == before);
    CHECK(b.combatant(1).alive());
}

TEST_CASE("items: reviving a member who started the battle down does not allocate either") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    Battle b({hero(20), fallen(10)}, {harmless()}, tuning());
    begin(b, bag({{"bell", 1}}));
    const std::size_t slot = slotOf(b, "bell");
    const std::uint64_t before = ftcore_test::allocStats().count;
    const CommandError e = b.useItem(slot, 1, 0.0);
    const std::uint64_t after = ftcore_test::allocStats().count;
    CHECK(e == CommandError::None);
    CHECK(after == before);
}

TEST_CASE("pilot: a member who is low gets the smallest heal that covers what is missing, else the biggest") {
    const auto choiceFor = [](int hp, int maxHp, std::initializer_list<std::pair<const char*, int>> carried, float below) {
        Combatant low = hero(15, maxHp);
        low.hp = hp;
        Battle b = started({hero(20), low}, bag(carried));
        const demo::PilotItemChoice c = demo::pilotItemChoice(b, below);
        return std::make_pair(c, c.use ? std::string(b.carriedItems()[c.slot].id.view()) : std::string());
    };
    // Missing 30: the small heal (40 at max HP 100) covers it, and the big one (500) does too: the small one.
    const auto covered = choiceFor(70, 100, {{"big", 1}, {"small", 1}}, 0.8f);
    CHECK(covered.first.use);
    CHECK(covered.second == "small");
    CHECK(covered.first.target == 1);
    // Missing 80: only the big one covers it.
    CHECK(choiceFor(20, 100, {{"small", 1}, {"big", 1}}, 0.8f).second == "big");
    // Missing 900 of 1000: neither covers it, so the biggest.
    CHECK(choiceFor(100, 1000, {{"small", 1}, {"big", 1}}, 0.8f).second == "big");
    // Nobody is low enough (70 percent is above 0.4).
    CHECK_FALSE(choiceFor(70, 100, {{"small", 1}}, 0.4f).first.use);
    // No heal carried: AP items and ids the table does not know are no use here.
    CHECK_FALSE(choiceFor(10, 100, {{"bar", 1}, {"mystery", 2}, {"bell", 1}}, 0.4f).first.use);
    CHECK_FALSE(choiceFor(10, 100, {}, 0.4f).first.use);
    // A downed member is not the one to heal, even when the rest are fine.
    Battle d = started({hero(20), fallen(15)}, bag({{"small", 1}}));
    CHECK_FALSE(demo::pilotItemChoice(d, 0.9f).use);
}

TEST_CASE("pilot: given items it heals and uses them up; given none it plays the same fight as before") {
    const demo::HeadlessResult plain = demo::playHeadless(demo::party(), demo::enemies(), true);
    demo::PilotItems none; // an empty table and an empty bag
    const demo::HeadlessResult same = demo::playHeadless(demo::party(), demo::enemies(), true, none);
    CHECK(same.itemsUsed == 0);
    CHECK(same.outcome == plain.outcome);
    CHECK(same.partyTurns == plain.partyTurns);
    CHECK(same.presses == plain.presses);
    CHECK(same.aims == plain.aims);
    CHECK(same.partyHpLeft == plain.partyHpLeft);
    CHECK(same.endTime == plain.endTime);

    // A sloppy pilot lets hits land, so the party gets low; with heals in the bag it uses them.
    demo::PilotItems items;
    items.table = itemTable();
    REQUIRE(items.carried.add("small", 3) == 3);
    items.healBelow = 0.9f;
    const demo::PilotSkill sloppy{0.5, 0.3};
    const demo::HeadlessResult with =
        demo::playHeadless(demo::party(), demo::enemies(), true, items, 1.0 / 120.0, 1200.0, DefenceInput::Parry, sloppy);
    const demo::HeadlessResult again =
        demo::playHeadless(demo::party(), demo::enemies(), true, items, 1.0 / 120.0, 1200.0, DefenceInput::Parry, sloppy);
    CHECK_FALSE(with.commandFailed);
    CHECK(with.finished);
    CHECK(with.itemsUsed >= 1);
    CHECK(with.itemsUsed <= 3);
    CHECK(with.itemsUsed == again.itemsUsed); // the same play twice
    CHECK(with.endTime == again.endTime);
    CHECK(with.partyHpLeft == again.partyHpLeft);
    // Without the items the same pilot is unchanged.
    const demo::HeadlessResult without =
        demo::playHeadless(demo::party(), demo::enemies(), true, 1.0 / 120.0, 1200.0, DefenceInput::Parry, sloppy);
    CHECK(without.itemsUsed == 0);
}
