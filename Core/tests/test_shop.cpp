#include "ftcore/Inventory.h"
#include "ftcore/Progression.h"
#include "ftcore/Shop.h"

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

// "small" and "big" are for sale, "gem" is for sale and sells for a fortune, "unstocked" is in the
// item table but not in the shop.
constexpr const char* kItems = R"J({ "items": [
    { "id": "small", "effect": "heal", "hp": 30, "buyPrice": 30, "sellPrice": 15 },
    { "id": "big", "effect": "heal", "hp": 100, "buyPrice": 90, "sellPrice": 45 },
    { "id": "gem", "effect": "heal", "hp": 1, "buyPrice": 100000, "sellPrice": 100000 },
    { "id": "unstocked", "effect": "restore_ap", "ap": 1, "buyPrice": 10, "sellPrice": 5 } ]})J";

struct Shopfront {
    ItemTable items;
    ShopStock stock;
};

Shopfront makeShop() {
    Shopfront s;
    std::string err;
    REQUIRE_MESSAGE(importItemTable(kItems, s.items, &err), err);
    REQUIRE(s.stock.add("small"));
    REQUIRE(s.stock.add("big"));
    REQUIRE(s.stock.add("gem"));
    return s;
}

PartyProgress withGold(int gold) {
    PartyProgress p;
    p.gold = gold;
    return p;
}

// A buy that is refused with `want` and leaves the party as it was.
void refusedBuy(const Shopfront& s, PartyProgress& p, std::string_view id, int count, ShopResult want) {
    const PartyProgress before = p;
    CAPTURE(id);
    CAPTURE(count);
    CHECK(buy(p, s.items, s.stock, id, count) == want);
    CHECK(p.gold == before.gold);
    CHECK(p.xp == before.xp);
    CHECK(p.items == before.items);
}

void refusedSell(const Shopfront& s, PartyProgress& p, std::string_view id, int count, ShopResult want) {
    const PartyProgress before = p;
    CAPTURE(id);
    CAPTURE(count);
    CHECK(sell(p, s.items, id, count) == want);
    CHECK(p.gold == before.gold);
    CHECK(p.items == before.items);
}

} // namespace

TEST_CASE("shop: buying spends the gold and adds the items") {
    const Shopfront s = makeShop();
    PartyProgress p = withGold(300);
    CHECK(buy(p, s.items, s.stock, "small", 2) == ShopResult::Ok);
    CHECK(p.gold == 300 - 60);
    CHECK(p.items.count("small") == 2);
    CHECK(buy(p, s.items, s.stock, "small", 1) == ShopResult::Ok); // tops up the stack
    CHECK(p.gold == 300 - 90);
    CHECK(p.items.count("small") == 3);
    CHECK(buy(p, s.items, s.stock, "big", 1) == ShopResult::Ok);
    CHECK(p.gold == 300 - 90 - 90);
    CHECK(p.items.count("big") == 1);
    CHECK(p.items.size() == 2);
    // The exact price is enough.
    p = withGold(30);
    CHECK(buy(p, s.items, s.stock, "small", 1) == ShopResult::Ok);
    CHECK(p.gold == 0);
    // Experience is not touched.
    p = withGold(100);
    p.xp = 77;
    CHECK(buy(p, s.items, s.stock, "small", 1) == ShopResult::Ok);
    CHECK(p.xp == 77);
}

TEST_CASE("shop: every refusal leaves the party as it was") {
    const Shopfront s = makeShop();
    PartyProgress p = withGold(1000);
    p.items.add("small", 3);

    refusedBuy(s, p, "ghost", 1, ShopResult::UnknownItem);     // not in the item table
    refusedBuy(s, p, "", 1, ShopResult::UnknownItem);
    refusedBuy(s, p, "unstocked", 1, ShopResult::NotStocked);  // in the table, not in the shop
    refusedBuy(s, p, "small", 0, ShopResult::InvalidCount);
    refusedBuy(s, p, "small", -5, ShopResult::InvalidCount);   // a negative count must not pay out
    refusedBuy(s, p, "ghost", 0, ShopResult::InvalidCount);    // the count is checked first
    refusedBuy(s, p, "gem", 11, ShopResult::NotEnoughGold);    // 1,100,000
    p.gold = 59;
    refusedBuy(s, p, "small", 2, ShopResult::NotEnoughGold);   // 60 is one gold more
    p.gold = 0;
    refusedBuy(s, p, "small", 1, ShopResult::NotEnoughGold);

    // Not all of them fit: nothing is bought, not even the part that would.
    p.gold = 100000;
    p.items.add("small", 94); // 97 carried
    refusedBuy(s, p, "small", 3, ShopResult::InventoryFull);
    refusedBuy(s, p, "small", 100, ShopResult::InventoryFull); // more than one stack holds
    CHECK(buy(p, s.items, s.stock, "small", 2) == ShopResult::Ok); // exactly what fits
    CHECK(p.items.count("small") == kMaxItemStack);
    refusedBuy(s, p, "small", 1, ShopResult::InventoryFull);

    // Every stack in use: a new id has no room, a stack that has room still sells.
    PartyProgress full = withGold(100000);
    for (std::size_t i = 0; i < kMaxInventoryStacks - 1; ++i) full.items.add("item_" + std::to_string(i), 1);
    full.items.add("small", 1);
    REQUIRE(full.items.size() == kMaxInventoryStacks);
    refusedBuy(s, full, "big", 1, ShopResult::InventoryFull);
    CHECK(buy(full, s.items, s.stock, "small", 1) == ShopResult::Ok);
    CHECK(full.items.count("small") == 2);
}

TEST_CASE("shop: a buy checks the count, the item, the stock, the gold and the room, in that order") {
    const Shopfront s = makeShop();
    PartyProgress p = withGold(0);
    p.items.add("small", kMaxItemStack);
    refusedBuy(s, p, "ghost", 0, ShopResult::InvalidCount);
    refusedBuy(s, p, "ghost", 1, ShopResult::UnknownItem);
    refusedBuy(s, p, "unstocked", 1, ShopResult::NotStocked);        // no gold either
    refusedBuy(s, p, "small", 1, ShopResult::NotEnoughGold);         // no gold and no room
    p.gold = 1000;
    refusedBuy(s, p, "small", 1, ShopResult::InventoryFull);
}

TEST_CASE("shop: a price too big for an int is just too expensive") {
    const Shopfront s = makeShop();
    PartyProgress p = withGold(kMaxGold);
    refusedBuy(s, p, "gem", 100000, ShopResult::NotEnoughGold);      // 10 billion
    refusedBuy(s, p, "gem", 2147483647, ShopResult::NotEnoughGold);  // INT_MAX gems
}

TEST_CASE("shop: selling earns the gold and takes the items out") {
    const Shopfront s = makeShop();
    PartyProgress p = withGold(5);
    p.items.add("small", 3);
    p.items.add("big", 1);
    CHECK(sell(p, s.items, "small", 2) == ShopResult::Ok);
    CHECK(p.gold == 5 + 30);
    CHECK(p.items.count("small") == 1);
    CHECK(sell(p, s.items, "small", 1) == ShopResult::Ok);
    CHECK(p.gold == 5 + 45);
    CHECK(p.items.count("small") == 0);
    CHECK(p.items.size() == 1); // the emptied stack left
    // An item the shop does not stock sells all the same.
    p.items.add("unstocked", 2);
    CHECK(sell(p, s.items, "unstocked", 2) == ShopResult::Ok);
    CHECK(p.gold == 5 + 45 + 10);
}

TEST_CASE("shop: a sale is refused for an unknown item, too few carried or a bad count") {
    const Shopfront s = makeShop();
    PartyProgress p = withGold(10);
    p.items.add("small", 2);
    p.items.add("a_future_item", 5); // carried, but the table has no price for it
    refusedSell(s, p, "ghost", 1, ShopResult::UnknownItem);
    refusedSell(s, p, "a_future_item", 1, ShopResult::UnknownItem);
    CHECK(p.items.count("a_future_item") == 5);
    refusedSell(s, p, "big", 1, ShopResult::NotCarried);   // not carried at all
    refusedSell(s, p, "small", 3, ShopResult::NotCarried); // fewer than that
    refusedSell(s, p, "small", 0, ShopResult::InvalidCount);
    refusedSell(s, p, "small", -1, ShopResult::InvalidCount);
    refusedSell(s, p, "ghost", 0, ShopResult::InvalidCount);
}

TEST_CASE("shop: gold never passes kMaxGold") {
    const Shopfront s = makeShop();
    PartyProgress p = withGold(kMaxGold - 10);
    p.items.add("gem", 2);
    CHECK(sell(p, s.items, "gem", 1) == ShopResult::Ok); // 100,000 for gold with room for 10
    CHECK(p.gold == kMaxGold);
    CHECK(sell(p, s.items, "gem", 1) == ShopResult::Ok); // nothing more fits, the item still goes
    CHECK(p.gold == kMaxGold);
    CHECK(p.items.count("gem") == 0);

    p = withGold(500000);
    p.items.add("gem", kMaxItemStack); // 9,900,000
    CHECK(sell(p, s.items, "gem", kMaxItemStack) == ShopResult::Ok);
    CHECK(p.gold == kMaxGold);

    // Buying at the cap spends from it.
    p = withGold(kMaxGold);
    CHECK(buy(p, s.items, s.stock, "gem", 1) == ShopResult::Ok);
    CHECK(p.gold == kMaxGold - 100000);
}

TEST_CASE("shop: buying and selling back never earns gold") {
    const Shopfront s = makeShop();
    for (const ItemId& id : s.stock.ids) {
        if (id.empty()) continue;
        PartyProgress p = withGold(200000);
        const int before = p.gold;
        CHECK(buy(p, s.items, s.stock, id.view(), 1) == ShopResult::Ok);
        CHECK(sell(p, s.items, id.view(), 1) == ShopResult::Ok);
        CHECK(p.gold <= before);
        CHECK(p.items.empty());
    }
}

TEST_CASE("shop: buying and selling do not allocate") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    const Shopfront s = makeShop();
    PartyProgress p = withGold(5000);
    const auto before = ftcore_test::allocStats().count;
    int ok = 0, refused = 0;
    for (int i = 0; i < 40; ++i) {
        const ShopResult results[] = {
            buy(p, s.items, s.stock, "small", 3),     buy(p, s.items, s.stock, "ghost", 1),
            buy(p, s.items, s.stock, "unstocked", 1), buy(p, s.items, s.stock, "gem", 50),
            sell(p, s.items, "small", 2),             sell(p, s.items, "big", 1),
            sell(p, s.items, "ghost", 1),             sell(p, s.items, "small", 0),
        };
        for (const ShopResult r : results) (r == ShopResult::Ok ? ok : refused) += 1;
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(after == before);
    CHECK(ok > 0);
    CHECK(refused > 0);
}

TEST_CASE("shop stock: add, contains and the limits") {
    ShopStock stock;
    CHECK(stock.empty());
    CHECK(stock.add("small"));
    CHECK(stock.add("big"));
    CHECK_FALSE(stock.add("small")); // already listed
    CHECK_FALSE(stock.add(""));
    CHECK_FALSE(stock.add("Bad Id"));
    CHECK(stock.size() == 2);
    CHECK(stock.contains("big"));
    CHECK_FALSE(stock.contains("gem"));
    CHECK_FALSE(stock.contains(""));
    CHECK(stock[0].view() == "small");
    for (std::size_t i = stock.size(); i < kMaxShopStock; ++i) CHECK(stock.add("item_" + std::to_string(i)));
    CHECK_FALSE(stock.add("one_more")); // full
    CHECK(stock.size() == kMaxShopStock);
    stock.clear();
    CHECK(stock.empty());
    CHECK_FALSE(stock.contains("small"));
}

TEST_CASE("shop stock: a shops file loads against the item table; bad ones are refused with a reason") {
    ItemTable items;
    std::string err;
    REQUIRE_MESSAGE(importItemTable(kItems, items, &err), err);

    ShopStock stock;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importShopStock(R"({"version": 1, "default": ["big", "small"]})", items, stock, &err, &warnings), err);
    CHECK(warnings.empty());
    REQUIRE(stock.size() == 2);
    CHECK(stock[0].view() == "big"); // the file's order
    CHECK(stock.contains("small"));

    struct Bad {
        std::string json;
        const char* expected;
    };
    std::string tooMany = R"({"default": [)";
    ItemTable big;
    for (std::size_t i = 0; i <= kMaxShopStock; ++i) {
        ItemDef d;
        d.id.set("item_" + std::to_string(i));
        d.hp = 1;
        REQUIRE(big.add(d));
        if (i) tooMany += ',';
        tooMany += "\"item_" + std::to_string(i) + "\"";
    }
    tooMany += "]}";
    const std::vector<Bad> bad = {
        {"{", "shops file"},
        {"[]", "shops file"},
        {R"({"version": 2, "default": ["small"]})", "version"},
        {R"({})", "default"},
        {R"({"default": []})", "default"},
        {R"({"default": "small"})", "default"},
        {R"({"default": [3]})", "default[0]"},
        {R"({"default": ["ghost"]})", "ghost"},       // not in the item table
        {R"({"default": ["small", "small"]})", "duplicate"},
        {R"({"default": ["small", "Bad Id"]})", "default[1]"},
    };
    for (const Bad& b : bad) {
        INFO(b.json);
        err.clear();
        CHECK_FALSE(importShopStock(b.json, items, stock, &err));
        CHECK(err.find(b.expected) != std::string::npos);
        CHECK(stock.empty());
    }
    CHECK_FALSE(importShopStock(tooMany, big, stock, &err));
    CHECK(err.find("default") != std::string::npos);
    CHECK(importShopStock(tooMany.substr(0, tooMany.rfind(',')) + "]}", big, stock, &err)); // one fewer: exactly the limit
    CHECK(stock.size() == kMaxShopStock);
    CHECK_FALSE(importShopStock("{", items, stock)); // no error sink: must not crash

    warnings.clear();
    CHECK(importShopStock(R"({"default": ["small"], "sale": 1})", items, stock, &err, &warnings));
    CHECK(warnings.size() == 1);
}

TEST_CASE("Data/Battle/shops.json loads against items.json, and every item in it can be bought and sold") {
    std::string itemsText, shopsText;
    if (!ftcore_test::readProjectData("Battle/items.json", itemsText) || !ftcore_test::readProjectData("Battle/shops.json", shopsText)) {
        FAIL("Data/Battle/items.json or shops.json not found");
        return;
    }
    ItemTable items;
    ShopStock stock;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importItemTable(itemsText, items, &err), err);
    REQUIRE_MESSAGE(importShopStock(shopsText, items, stock, &err, &warnings), err);
    CHECK(warnings.empty());
    CHECK_FALSE(stock.empty());
    for (std::size_t i = 0; i < stock.size(); ++i) {
        const ItemDef* def = items.find(stock[i].view());
        REQUIRE(def != nullptr);
        PartyProgress p = withGold(def->buyPrice);
        CHECK(buy(p, items, stock, def->id.view(), 1) == ShopResult::Ok);
        CHECK(p.gold == 0);
        CHECK(p.items.count(def->id.view()) == 1);
        CHECK(sell(p, items, def->id.view(), 1) == ShopResult::Ok);
        CHECK(p.gold == def->sellPrice);
    }
}
