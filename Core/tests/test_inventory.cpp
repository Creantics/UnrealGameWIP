#include "ftcore/Encounter.h"
#include "ftcore/Inventory.h"
#include "ftcore/Progression.h"
#include "ftcore/StorySave.h"
#include "ftcore/UiText.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

using namespace ftcore;

namespace {

// Three items, one of each effect; `rise` leaves the scope out (the default is one ally).
constexpr const char* kItems = R"J({
  "version": 1,
  "items": [
    { "id": "small", "effect": "heal", "scope": "ally", "hp": 30, "hpPercent": 10, "buyPrice": 30, "sellPrice": 15 },
    { "id": "party_ap", "effect": "restore_ap", "scope": "party", "ap": 1, "buyPrice": 70, "sellPrice": 35 },
    { "id": "rise", "effect": "revive", "hpPercent": 30, "buyPrice": 120, "sellPrice": 60 }
  ]
})J";

// The ids of an inventory in slot order, comma separated.
std::string order(const Inventory& bag) {
    std::string out;
    for (const ItemStack& s : bag) {
        if (!out.empty()) out += ',';
        out += std::string(s.id.view());
    }
    return out;
}

bool readData(const char* rel, std::string& text) {
    if (ftcore_test::readProjectData(rel, text)) return true;
    FAIL("Data file not found: " << rel);
    return false;
}

} // namespace

TEST_CASE("inventory: add, count and remove") {
    Inventory bag;
    CHECK(bag.empty());
    CHECK(bag.add("small", 3) == 3);
    CHECK(bag.add("rise", 1) == 1);
    CHECK(bag.add("small", 2) == 2); // tops up the stack it has
    CHECK(bag.count("small") == 5);
    CHECK(bag.count("rise") == 1);
    CHECK(bag.count("nothing") == 0);
    REQUIRE(bag.size() == 2);

    CHECK(bag.remove("small", 2) == 2);
    CHECK(bag.count("small") == 3);
    CHECK(bag.remove("small", 10) == 3); // takes what is there
    CHECK(bag.count("small") == 0);
    CHECK(bag.size() == 1); // an emptied stack leaves the list
    CHECK(bag.remove("small", 1) == 0); // not carried
    CHECK(bag.remove("rise", 0) == 0);
    CHECK(bag.remove("rise", -4) == 0);
    CHECK(bag.count("rise") == 1);

    // Nothing to add: a count below 1, an id that is not valid.
    CHECK(bag.add("small", 0) == 0);
    CHECK(bag.add("small", -3) == 0);
    CHECK(bag.add("", 1) == 0);
    CHECK(bag.add("Bad Id", 1) == 0);
    CHECK(bag.add("an_id_that_is_far_too_long_for_the_buffer", 1) == 0);
    CHECK(bag.size() == 1);
    CHECK(bag.count("") == 0);

    bag.clear();
    CHECK(bag.empty());
    CHECK(bag.count("rise") == 0);
}

TEST_CASE("inventory: a stack holds 99 and a full inventory refuses a new id") {
    Inventory bag;
    CHECK(bag.add("small", 90) == 90);
    CHECK(bag.add("small", 20) == 9); // only 9 fit
    CHECK(bag.count("small") == kMaxItemStack);
    CHECK(bag.add("small", 1) == 0);
    CHECK(bag.space("small") == 0);
    CHECK(bag.space("other") == kMaxItemStack);
    CHECK(bag.space("Bad Id") == 0);
    CHECK(bag.add("big", 500) == kMaxItemStack); // a new stack holds at most 99 too

    for (std::size_t i = bag.size(); i < kMaxInventoryStacks; ++i) CHECK(bag.add("item_" + std::to_string(i), 1) == 1);
    REQUIRE(bag.size() == kMaxInventoryStacks);
    CHECK(bag.add("one_too_many", 1) == 0); // a new id is refused
    CHECK(bag.space("one_too_many") == 0);
    CHECK(bag.count("one_too_many") == 0);
    CHECK(bag.size() == kMaxInventoryStacks);

    // The stacks it has still take what fits.
    CHECK(bag.remove("big", 10) == 10);
    CHECK(bag.space("big") == 10);
    CHECK(bag.add("big", 20) == 10);
    CHECK(bag.add("item_7", 5) == 5);
    // Emptying a stack makes room for a new id.
    CHECK(bag.remove("item_5", 1) == 1);
    CHECK(bag.space("one_too_many") == kMaxItemStack);
    CHECK(bag.add("one_too_many", 4) == 4);
    CHECK(bag.size() == kMaxInventoryStacks);
}

TEST_CASE("inventory: stacks keep their pickup order") {
    Inventory bag;
    bag.add("c", 1);
    bag.add("a", 2);
    bag.add("b", 3);
    CHECK(order(bag) == "c,a,b");
    bag.add("c", 4); // topping up does not move a stack
    CHECK(order(bag) == "c,a,b");
    CHECK(bag[0].count == 5);
    CHECK(bag[1].count == 2);
    CHECK(bag.remove("a", 1) == 1);
    CHECK(order(bag) == "c,a,b");
    CHECK(bag.remove("a", 1) == 1); // a leaves; the later stacks close up
    CHECK(order(bag) == "c,b");
    CHECK(bag[1].id.view() == "b");
    CHECK(bag[1].count == 3);
    bag.add("a", 1); // a new pickup goes to the end
    CHECK(order(bag) == "c,b,a");
    CHECK(bag.remove("c", kMaxItemStack) == 5);
    CHECK(order(bag) == "b,a");
}

TEST_CASE("inventory: copies and comparison") {
    Inventory a;
    a.add("small", 2);
    a.add("rise", 1);
    Inventory b = a;
    CHECK(a == b);
    b.add("small", 1);
    CHECK_FALSE(a == b);
    b.remove("small", 1);
    CHECK(a == b);
    Inventory reversed;
    reversed.add("rise", 1);
    reversed.add("small", 2);
    CHECK_FALSE(a == reversed); // the order is part of it
    // A cleared inventory equals a new one however it got there.
    b.clear();
    CHECK(b == Inventory{});
}

TEST_CASE("inventory: nothing here allocates") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    const auto before = ftcore_test::allocStats().count;
    int sum = 0;
    Inventory bag;
    for (int round = 0; round < 20; ++round) {
        sum += bag.add("small", 7) + bag.add("rise", 2) + bag.add("party_ap", 120);
        sum += bag.count("small") + bag.space("rise");
        sum += bag.remove("rise", 1) + bag.remove("small", 50) + bag.remove("party_ap", 300) + bag.remove("small", 1);
        Inventory copy = bag;
        sum += static_cast<int>(copy.size());
        copy.clear();
        sum += copy == bag ? 1 : 0;
    }
    // A full inventory refusing new ids (ids "aa_x", "ba_x", ...).
    for (std::size_t i = 0; i < kMaxInventoryStacks + 3; ++i) {
        const char id[4] = {static_cast<char>('a' + i % 26), static_cast<char>('a' + i / 26), '_', 'x'};
        sum += bag.add(std::string_view(id, sizeof id), 1);
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(after == before);
    CHECK(sum > 0);
}

TEST_CASE("item table: a fixture loads and finds its items") {
    ItemTable table;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importItemTable(kItems, table, &err, &warnings), err);
    CHECK(warnings.empty());
    REQUIRE(table.size() == 3);

    const ItemDef* small = table.find("small");
    REQUIRE(small != nullptr);
    CHECK(small->id.view() == "small");
    CHECK(small->effect == ItemEffect::Heal);
    CHECK(small->scope == ItemScope::Ally);
    CHECK(small->hp == 30);
    CHECK(small->hpPercent == 10);
    CHECK(small->ap == 0);
    CHECK(small->buyPrice == 30);
    CHECK(small->sellPrice == 15);
    CHECK(small->hpFor(140) == 30 + 14);
    CHECK(small->hpFor(0) == 30);

    const ItemDef* ap = table.find("party_ap");
    REQUIRE(ap != nullptr);
    CHECK(ap->effect == ItemEffect::RestoreAp);
    CHECK(ap->scope == ItemScope::Party);
    CHECK(ap->ap == 1);

    const ItemDef* rise = table.find("rise");
    REQUIRE(rise != nullptr);
    CHECK(rise->effect == ItemEffect::Revive);
    CHECK(rise->scope == ItemScope::Ally); // the default
    CHECK(rise->hpFor(100) == 30);

    CHECK(table.find("nope") == nullptr);
    CHECK(table.find("") == nullptr);
    CHECK(table.find("smal") == nullptr);
    CHECK(&table[0] == small);
    CHECK(std::string(itemEffectName(ItemEffect::RestoreAp)) == "restore_ap");
    CHECK(std::string(itemScopeName(ItemScope::Party)) == "party");

    if (ftcore_test::allocHookInstalled()) {
        const auto before = ftcore_test::allocStats().count;
        std::size_t hits = 0;
        for (int i = 0; i < 50; ++i) hits += table.find("rise") != nullptr ? 1u : 0u;
        CHECK(ftcore_test::allocStats().count == before);
        CHECK(hits == 50);
    }
}

TEST_CASE("item table: bad files are refused with a reason") {
    const auto item = [](const char* body) {
        return std::string(R"({"items": [{"id": "x", )") + body + "}]}";
    };
    struct Bad {
        std::string json;
        const char* expected; // a piece of the error
    };
    const std::vector<Bad> bad = {
        {"{", "items file"},
        {"[]", "items file"},
        {R"({"items": []})", "items:"},
        {R"({"items": {}})", "items:"},
        {R"({"version": 2, "items": [{"id": "x", "effect": "heal", "hp": 1, "buyPrice": 1, "sellPrice": 1}]})", "version"},
        {R"({"items": [3]})", "items[0]"},
        {R"({"items": [{"id": "x", "effect": "heal", "hp": 1, "buyPrice": 2, "sellPrice": 1},
                       {"id": "x", "effect": "heal", "hp": 1, "buyPrice": 2, "sellPrice": 1}]})", "duplicate"},
        {R"({"items": [{"id": "Bad Id", "effect": "heal", "hp": 1, "buyPrice": 1, "sellPrice": 1}]})", "items[0].id"},
        {R"({"items": [{"effect": "heal", "hp": 1, "buyPrice": 1, "sellPrice": 1}]})", "items[0].id"},
        {R"({"items": [{"id": "an_id_that_is_far_too_long_for_it", "effect": "heal", "hp": 1, "buyPrice": 1, "sellPrice": 1}]})",
         "items[0].id"},
        {item(R"("effect": "smite", "hp": 1, "buyPrice": 1, "sellPrice": 1)"), "items[0].effect"},
        {item(R"("hp": 1, "buyPrice": 1, "sellPrice": 1)"), "items[0].effect"}, // no effect at all
        {item(R"("effect": "heal", "scope": "row", "hp": 1, "buyPrice": 1, "sellPrice": 1)"), "items[0].scope"},
        {item(R"("effect": "heal", "scope": 3, "hp": 1, "buyPrice": 1, "sellPrice": 1)"), "items[0].scope"},
        {item(R"("effect": "heal", "hp": -1, "hpPercent": 5, "buyPrice": 1, "sellPrice": 1)"), "items[0].hp"},
        {item(R"("effect": "heal", "hp": 1.5, "buyPrice": 1, "sellPrice": 1)"), "items[0].hp"},
        {item(R"("effect": "heal", "hp": 100001, "buyPrice": 1, "sellPrice": 1)"), "items[0].hp"},
        {item(R"("effect": "heal", "hpPercent": 101, "buyPrice": 1, "sellPrice": 1)"), "items[0].hpPercent"},
        {item(R"("effect": "heal", "hp": 1, "buyPrice": -5, "sellPrice": 0)"), "items[0].buyPrice"},
        {item(R"("effect": "heal", "hp": 1, "buyPrice": 10)"), "items[0].sellPrice"},    // no sell price
        {item(R"("effect": "heal", "hp": 1, "sellPrice": 10)"), "items[0].buyPrice"},    // no buy price
        {item(R"("effect": "heal", "hp": 1, "buyPrice": "10", "sellPrice": 5)"), "items[0].buyPrice"},
        {item(R"("effect": "heal", "hp": 1, "buyPrice": 10, "sellPrice": 11)"), "sellPrice"}, // sells for more than it costs
        {item(R"("effect": "heal", "buyPrice": 10, "sellPrice": 5)"), "hp or hpPercent"},    // does nothing
        {item(R"("effect": "revive", "hp": 0, "hpPercent": 0, "buyPrice": 10, "sellPrice": 5)"), "hp or hpPercent"},
        {item(R"("effect": "heal", "hp": 5, "ap": 1, "buyPrice": 10, "sellPrice": 5)"), "items[0].ap"},
        {item(R"("effect": "restore_ap", "buyPrice": 10, "sellPrice": 5)"), "items[0].ap"},
        {item(R"("effect": "restore_ap", "ap": 100, "buyPrice": 10, "sellPrice": 5)"), "items[0].ap"},
        {item(R"("effect": "restore_ap", "ap": 1, "hp": 5, "buyPrice": 10, "sellPrice": 5)"), "ap only"},
    };
    ItemTable table;
    for (const Bad& b : bad) {
        INFO(b.json);
        std::string err;
        CHECK_FALSE(importItemTable(b.json, table, &err));
        CHECK(err.find(b.expected) != std::string::npos);
        CHECK(table.empty());
    }
    CHECK_FALSE(importItemTable("{", table)); // no error sink: must not crash

    // One more item than the table holds.
    std::string many = R"({"items": [)";
    for (std::size_t i = 0; i <= kMaxItemDefs; ++i) {
        if (i) many += ',';
        many += R"({"id": "item_)" + std::to_string(i) + R"(", "effect": "heal", "hp": 1, "buyPrice": 1, "sellPrice": 1})";
    }
    many += "]}";
    std::string err;
    CHECK_FALSE(importItemTable(many, table, &err));
    CHECK(err.find("items:") != std::string::npos);

    // Unknown keys are warnings, and the item still loads.
    std::vector<std::string> warnings;
    CHECK(importItemTable(R"({"items": [{"id": "x", "effect": "heal", "hp": 1, "buyPrice": 1, "sellPrice": 0, "colour": 3}], "extra": 1})",
                          table, &err, &warnings));
    CHECK(warnings.size() == 2);
    CHECK(table.size() == 1);
}

TEST_CASE("item table: add refuses a full table, a duplicate and an empty id") {
    ItemTable table;
    ItemDef d;
    CHECK_FALSE(table.add(d)); // no id
    for (std::size_t i = 0; i < kMaxItemDefs; ++i) {
        d.id.set("item_" + std::to_string(i));
        CHECK(table.add(d));
    }
    CHECK(table.size() == kMaxItemDefs);
    d.id.set("one_more");
    CHECK_FALSE(table.add(d)); // full
    table.clear();
    CHECK(table.empty());
    d.id.set("a");
    CHECK(table.add(d));
    CHECK_FALSE(table.add(d)); // already listed
    CHECK(table.size() == 1);
}

TEST_CASE("Data/Battle/items.json loads: every kind of item, each with its texts in ui.json") {
    std::string itemsText, uiText;
    if (!readData("Battle/items.json", itemsText) || !readData("Loc/en/ui.json", uiText)) return;
    ItemTable table;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importItemTable(itemsText, table, &err, &warnings), err);
    CHECK(warnings.empty());
    CHECK(table.size() >= 5);

    bool healOne = false, healParty = false, apOne = false, revive = false;
    for (const ItemDef& d : table) {
        healOne = healOne || (d.effect == ItemEffect::Heal && d.scope == ItemScope::Ally);
        healParty = healParty || (d.effect == ItemEffect::Heal && d.scope == ItemScope::Party);
        apOne = apOne || d.effect == ItemEffect::RestoreAp;
        revive = revive || d.effect == ItemEffect::Revive;
    }
    CHECK(healOne);
    CHECK(healParty);
    CHECK(apOne);
    CHECK(revive);

    // The display text is in the interface table: item.<id>.name and item.<id>.desc, and nothing
    // under item. for an id the table does not have (CheckUiTextKeys.cmake checks the same).
    UiTextTable en;
    REQUIRE_MESSAGE(importUiText(uiText, en, &err), err);
    for (const ItemDef& d : table) {
        const std::string base = "item." + std::string(d.id.view());
        CAPTURE(base);
        const std::string* name = en.find(base + ".name");
        const std::string* desc = en.find(base + ".desc");
        REQUIRE(name != nullptr);
        REQUIRE(desc != nullptr);
        CHECK_FALSE(name->empty());
        CHECK(name->size() < 40);
        CHECK_FALSE(desc->empty());
        CHECK(desc->size() < 160);
    }
    std::size_t itemKeys = 0;
    for (const UiTextEntry& e : en.entries) {
        if (e.key.rfind("item.", 0) != 0) continue;
        ++itemKeys;
        CAPTURE(e.key);
        const std::size_t dot = e.key.rfind('.');
        REQUIRE(dot > 5);
        const std::string suffix = e.key.substr(dot + 1);
        CHECK((suffix == "name" || suffix == "desc"));
        CHECK(table.find(e.key.substr(5, dot - 5)) != nullptr);
    }
    CHECK(itemKeys == table.size() * 2);
}

namespace {

constexpr const char* kDropRoster = R"J({ "enemies": [
    { "id": "a", "stats": {}, "reward": { "xp": 20, "gold": 10, "items": [ { "id": "small" } ] }, "skills": [ { "id": "s" } ] },
    { "id": "b", "stats": {}, "reward": { "xp": 5, "items": [ { "id": "small", "count": 2 }, { "id": "rise", "count": 1 } ] },
      "skills": [ { "id": "s" } ] },
    { "id": "c", "stats": {}, "skills": [ { "id": "s" } ] } ]})J";

} // namespace

TEST_CASE("enemy rewards list item drops") {
    EnemyRoster roster;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importEnemyRoster(kDropRoster, roster, &err, &warnings), err);
    CHECK(warnings.empty());
    CHECK(roster.enemies[0].reward.items.count("small") == 1); // the count defaults to 1
    CHECK(roster.enemies[1].reward.items.count("small") == 2);
    CHECK(roster.enemies[1].reward.items.count("rise") == 1);
    CHECK(order(roster.enemies[1].reward.items) == "small,rise");
    CHECK(roster.enemies[2].reward.items.empty());

    // Unknown keys warn.
    CHECK(importEnemyRoster(R"({"enemies": [{"id": "a", "stats": {}, "skills": [{"id": "s"}],
                                "reward": {"items": [{"id": "small", "chance": 5}]}}]})", roster, &err, &warnings));
    CHECK(warnings.size() == 1);
    CHECK(roster.enemies[0].reward.items.count("small") == 1);
}

TEST_CASE("bad item drops are refused with a reason") {
    const auto drops = [](const char* list) {
        return std::string(R"({"enemies": [{"id": "a", "stats": {}, "skills": [{"id": "s"}], "reward": {"items": )") + list +
               "}}]}";
    };
    const char* bad[] = {
        R"(3)",
        R"({})",
        R"([3])",
        R"([{"count": 1}])",                                                     // no id
        R"([{"id": 4}])",
        R"([{"id": "Bad Id"}])",
        R"([{"id": "an_id_that_is_far_too_long_for_it"}])",
        R"([{"id": "small", "count": 0}])",
        R"([{"id": "small", "count": 100}])",
        R"([{"id": "small", "count": 1.5}])",
        R"([{"id": "small", "count": "2"}])",
        R"([{"id": "small"}, {"id": "small", "count": 2}])",                     // an id once
        R"([{"id": "a"}, {"id": "b"}, {"id": "c"}, {"id": "d"}, {"id": "e"}])",  // more than kMaxEnemyDrops
    };
    for (const char* list : bad) {
        INFO(list);
        EnemyRoster roster;
        std::string err;
        CHECK_FALSE(importEnemyRoster(drops(list), roster, &err));
        CHECK(err.find("reward.items") != std::string::npos);
    }
    EnemyRoster roster;
    CHECK(importEnemyRoster(drops("[]"), roster)); // an empty list is fine
    CHECK(importEnemyRoster(drops(R"([{"id": "a"}, {"id": "b"}, {"id": "c"}, {"id": "d"}])"), roster)); // exactly the limit
    CHECK(roster.enemies[0].reward.items.size() == kMaxEnemyDrops);
}

TEST_CASE("an encounter sums its drops, the tier leaves them alone, and award puts them in the inventory") {
    EnemyRoster roster;
    std::string err;
    REQUIRE_MESSAGE(importEnemyRoster(kDropRoster, roster, &err), err);
    EncounterLineup lineup;
    lineup.enemies = {0, 1, 2, 0};
    const Reward r = encounterReward(roster, lineup);
    CHECK(r.gold == 20);
    CHECK(r.items.count("small") == 4); // 1 + 2 + 1: one stack
    CHECK(r.items.count("rise") == 1);
    CHECK(order(r.items) == "small,rise");

    lineup.tier = 5;
    const Reward tough = encounterReward(roster, lineup);
    CHECK(tough.xp > r.xp);
    CHECK(tough.gold > r.gold);
    CHECK(tough.items == r.items); // the tier raises xp and gold only
    lineup.enemies.push_back(99);  // a bad index drops nothing
    CHECK(encounterReward(roster, lineup).items == r.items);

    PartyProgress p;
    p.items.add("rise", 2);
    AwardResult a = award(p, r);
    CHECK(a.itemsLost == 0);
    CHECK(p.items.count("small") == 4);
    CHECK(p.items.count("rise") == 3);
    CHECK(order(p.items) == "rise,small"); // the older stack stays first
    CHECK(p.gold == 20);
    const int xpBefore = p.xp;
    a = award(p, Reward{3, 4}); // a reward without drops
    CHECK(a.itemsLost == 0);
    CHECK(p.items.count("small") == 4);
    CHECK(p.xp == xpBefore + 3);
    CHECK(p.gold == 24);

    // Past the stack cap the surplus is lost, and counted: small had room for 95 of the 99.
    Reward big;
    big.items.add("small", kMaxItemStack);
    big.items.add("rise", 5);
    a = award(p, big);
    CHECK(p.items.count("small") == kMaxItemStack);
    CHECK(p.items.count("rise") == 8);
    CHECK(a.itemsLost == 4);

    // A full inventory refuses a new id but takes the rest of the drops.
    PartyProgress full;
    for (std::size_t i = 0; i < kMaxInventoryStacks; ++i) full.items.add("item_" + std::to_string(i), 1);
    Reward drops;
    drops.items.add("item_3", 2);
    drops.items.add("a_new_one", 6);
    a = award(full, drops);
    CHECK(a.itemsLost == 6);
    CHECK(full.items.count("item_3") == 3);
    CHECK(full.items.count("a_new_one") == 0);

    if (ftcore_test::allocHookInstalled()) {
        const auto before = ftcore_test::allocStats().count;
        PartyProgress q;
        int sum = 0;
        for (int i = 0; i < 50; ++i) sum += award(q, encounterReward(roster, lineup)).itemsLost;
        CHECK(ftcore_test::allocStats().count == before);
        CHECK(sum > 0); // 50 fights of 4 small heals overflow the stack of 99
        CHECK(q.items.count("small") == kMaxItemStack);
    }
}

TEST_CASE("every drop in enemies.json is an item in items.json, and a few enemies drop something") {
    std::string enemiesText, itemsText;
    if (!readData("Battle/enemies.json", enemiesText) || !readData("Battle/items.json", itemsText)) return;
    EnemyRoster roster;
    ItemTable table;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importEnemyRoster(enemiesText, roster, &err, &warnings), err);
    REQUIRE_MESSAGE(importItemTable(itemsText, table, &err, &warnings), err);
    CHECK(warnings.empty());
    std::size_t droppers = 0;
    for (const EnemyDef& e : roster.enemies) {
        CAPTURE(e.id);
        if (!e.reward.items.empty()) ++droppers;
        for (const ItemStack& drop : e.reward.items) {
            CAPTURE(drop.id.view());
            CHECK(table.find(drop.id.view()) != nullptr);
            CHECK(drop.count >= 1);
        }
    }
    CHECK(droppers >= 3);
    CHECK(droppers < roster.enemies.size()); // most enemies drop nothing
}

TEST_CASE("the cheapest heal costs about one or two typical fights' gold") {
    std::string enemiesText, encountersText, itemsText;
    if (!readData("Battle/enemies.json", enemiesText) || !readData("Battle/encounters.json", encountersText) ||
        !readData("Battle/items.json", itemsText))
        return;
    EnemyRoster roster;
    EncounterTable encounters;
    ItemTable table;
    std::string err;
    REQUIRE_MESSAGE(importEnemyRoster(enemiesText, roster, &err), err);
    REQUIRE_MESSAGE(importEncounterTable(encountersText, roster, encounters, &err), err);
    REQUIRE_MESSAGE(importItemTable(itemsText, table, &err), err);
    REQUIRE_FALSE(encounters.entries.empty());

    long long total = 0;
    for (const EncounterEntry& e : encounters.entries) total += encounterReward(roster, e.lineup).gold;
    const double typicalFight = static_cast<double>(total) / static_cast<double>(encounters.entries.size());
    int cheapest = 1 << 30;
    for (const ItemDef& d : table)
        if (d.effect == ItemEffect::Heal) cheapest = d.buyPrice < cheapest ? d.buyPrice : cheapest;
    MESSAGE("a typical fight pays " << typicalFight << " gold; the cheapest heal costs " << cheapest);
    CHECK(cheapest >= typicalFight * 0.75);
    CHECK(cheapest <= typicalFight * 2.0);
}

namespace {

StorySave sampleSave() {
    StorySave s;
    s.scene = "SC-A";
    s.beat = 3;
    s.party.xp = 240;
    s.party.gold = 130;
    s.party.items.add("small", 3);
    s.party.items.add("a_future_item", 7); // the item table does not have it: it loads as it is
    s.party.items.add("rise", 1);
    return s;
}

} // namespace

TEST_CASE("save codec: items round-trip in order, ids the game no longer knows load as they are") {
    const StorySave s = sampleSave();
    const std::string text = encodeStorySave(s);
    CHECK(text.find(R"("party":{"xp":240,"gold":130,"items":[{"id":"small","count":3},{"id":"a_future_item","count":7},{"id":"rise","count":1}]})") !=
          std::string::npos);
    CHECK(text.find("\"format\":2") != std::string::npos); // still format 2

    StorySave out;
    out.party.items.add("stale", 9); // whatever was there is replaced
    std::string err;
    REQUIRE_MESSAGE(decodeStorySave(text, out, &err), err);
    CHECK(out.party.xp == 240);
    CHECK(out.party.gold == 130);
    CHECK(out.party.items == s.party.items);
    CHECK(order(out.party.items) == "small,a_future_item,rise");
    CHECK(out.party.items.count("a_future_item") == 7);
    CHECK(out.party.items.count("stale") == 0);

    // An empty inventory is written as an empty list and reads back empty.
    StorySave empty;
    const std::string emptyText = encodeStorySave(empty);
    CHECK(emptyText.find(R"("items":[])") != std::string::npos);
    out = s;
    REQUIRE_MESSAGE(decodeStorySave(emptyText, out, &err), err);
    CHECK(out.party.items.empty());

    // A full inventory round-trips too.
    StorySave full;
    for (std::size_t i = 0; i < kMaxInventoryStacks; ++i) full.party.items.add("item_" + std::to_string(i), kMaxItemStack);
    REQUIRE_MESSAGE(decodeStorySave(encodeStorySave(full), out, &err), err);
    CHECK(out.party.items == full.party.items);
}

TEST_CASE("save codec: a save from before items loads with an empty inventory") {
    StorySave out = sampleSave();
    std::string err;
    // The old format 2 text: a party with xp and gold only.
    REQUIRE_MESSAGE(decodeStorySave(R"({"format":2,"scene":"A","beat":0,"flags":[],"seen":[],"party":{"xp":240,"gold":130}})", out, &err), err);
    CHECK(out.party.xp == 240);
    CHECK(out.party.gold == 130);
    CHECK(out.party.items.empty());
    // Before progression there was no party at all, and format 1 had no "seen".
    out = sampleSave();
    REQUIRE_MESSAGE(decodeStorySave(R"({"format":1,"scene":"A","beat":2,"flags":[]})", out, &err), err);
    CHECK(out.party.xp == 0);
    CHECK(out.party.items.empty());
    // Items with no other change to the party.
    REQUIRE_MESSAGE(decodeStorySave(R"({"format":2,"scene":"A","beat":0,"flags":[],"seen":[],"party":{"xp":1,"gold":2,"items":[]}})", out, &err), err);
    CHECK(out.party.items.empty());
}

TEST_CASE("save codec: bad items are refused and leave the output alone; what does not fit is dropped") {
    const auto withItems = [](const std::string& items) {
        return R"({"format":2,"scene":"A","beat":0,"flags":[],"seen":[],"party":{"xp":1,"gold":2,"items":)" + items + "}}";
    };
    const StorySave marker = [] {
        StorySave m;
        m.scene = "keep";
        m.party.items.add("kept", 1);
        return m;
    }();
    const char* bad[] = {
        R"(3)",
        R"({})",
        R"([3])",
        R"([{"count":1}])",
        R"([{"id":"small"}])",
        R"([{"id":3,"count":1}])",
        R"([{"id":"","count":1}])",
        R"([{"id":"Bad Id","count":1}])",
        R"([{"id":"an_id_that_is_far_too_long_for_it","count":1}])",
        R"([{"id":"small","count":0}])",
        R"([{"id":"small","count":100}])",
        R"([{"id":"small","count":-1}])",
        R"([{"id":"small","count":1.5}])",
        R"([{"id":"small","count":"1"}])",
        R"([{"id":"small","count":1},{"id":"rise"}])", // a bad entry after a good one
    };
    for (const char* items : bad) {
        INFO(items);
        StorySave out = marker;
        std::string err;
        CHECK_FALSE(decodeStorySave(withItems(items), out, &err));
        CHECK(err.find("party.items") != std::string::npos);
        CHECK(out.scene == "keep");
        CHECK(out.party.items == marker.party.items);
    }

    // Duplicate entries add up to the stack cap; the rest of the surplus is dropped.
    StorySave out;
    std::string err;
    REQUIRE_MESSAGE(decodeStorySave(withItems(R"([{"id":"small","count":90},{"id":"rise","count":1},{"id":"small","count":90}])"), out, &err), err);
    CHECK(out.party.items.count("small") == kMaxItemStack);
    CHECK(out.party.items.count("rise") == 1);
    CHECK(order(out.party.items) == "small,rise");

    // More different ids than the inventory holds: the first ones load, the later ones are dropped.
    std::string many = "[";
    for (std::size_t i = 0; i < kMaxInventoryStacks + 5; ++i) {
        if (i) many += ',';
        many += R"({"id":"item_)" + std::to_string(i) + R"(","count":2})";
    }
    many += "]";
    REQUIRE_MESSAGE(decodeStorySave(withItems(many), out, &err), err);
    CHECK(out.party.items.size() == kMaxInventoryStacks);
    CHECK(out.party.items.count("item_0") == 2);
    CHECK(out.party.items.count("item_" + std::to_string(kMaxInventoryStacks - 1)) == 2);
    CHECK(out.party.items.count("item_" + std::to_string(kMaxInventoryStacks)) == 0);
}
