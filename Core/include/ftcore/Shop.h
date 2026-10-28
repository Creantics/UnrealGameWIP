#pragma once

// The shop: the party spends its gold on items and sells them back (Progression.h holds the gold
// and the inventory; Inventory.h the item prices).
//
//   ftcore::ShopStock stock;
//   std::string error;
//   if (!ftcore::importShopStock(shopsText, items, stock, &error)) ...;   // load time: allocates
//   ftcore::ShopResult r = ftcore::buy(progress, items, stock, "bandage_roll", 2);
//   // Ok: 2 bandage rolls in progress.items and 2 x their buy price out of progress.gold
//   r = ftcore::sell(progress, items, "bandage_roll", 1);
//   // Ok: 1 bandage roll out of progress.items and its sell price into progress.gold
//
// Both change `progress` only when they return Ok: a refused deal leaves the gold and the
// inventory exactly as they were, and a deal is all or nothing (there is no part of 5 bought).
// The checks run in this order and the first that fails is the result:
//   buy:  InvalidCount (count below 1), UnknownItem (not in the item table), NotStocked (the shop
//         does not sell it), NotEnoughGold (count x buy price is more than the gold), InventoryFull
//         (not all `count` fit: a new id needs a free stack, and a stack holds kMaxItemStack).
//   sell: InvalidCount, UnknownItem (the table has no price for it), NotCarried (the party carries
//         fewer than `count`). Selling needs no stock: any carried item can be sold.
// Gold stays within 0..kMaxGold: a sale that would pass the cap leaves it at kMaxGold. Nothing
// here allocates (importShopStock does, at load time).
//
// Data/Battle/shops.json (ids only):
//   { "version": 1, "default": [ "bandage_roll", "field_kit", "wake_bell" ] }
//   `default` is the stock of the shop: 1..kMaxShopStock distinct item ids, each one in
//   Data/Battle/items.json (an unknown id is an error). Unknown keys are warnings.

#include "ftcore/Api.h"
#include "ftcore/Inventory.h"
#include "ftcore/Progression.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

inline constexpr std::size_t kMaxShopStock = 16; // different items one shop sells

// The items a shop sells, by id (fixed capacity).
struct ShopStock {
    std::array<ItemId, kMaxShopStock> ids{};
    std::uint8_t count = 0;

    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
    const ItemId& operator[](std::size_t i) const { return ids[i]; }
    void clear() { count = 0; }
    FTCORE_API bool contains(std::string_view id) const;
    // Appends an id; false (and nothing added) when the stock is full, the id is not valid
    // (validItemId) or is already listed.
    FTCORE_API bool add(std::string_view id);
};

enum class ShopResult : std::uint8_t {
    Ok,
    UnknownItem,   // the item table has no such id
    NotStocked,    // buy: the shop does not sell it
    NotEnoughGold, // buy: count x buy price is more than the party's gold
    InventoryFull, // buy: not all of them fit in the inventory
    NotCarried,    // sell: the party carries fewer than `count`
    InvalidCount,  // count below 1
};

FTCORE_API ShopResult buy(PartyProgress& progress, const ItemTable& items, const ShopStock& stock, std::string_view id,
                          int count);
FTCORE_API ShopResult sell(PartyProgress& progress, const ItemTable& items, std::string_view id, int count);

// On failure `out` is empty and *error lists each problem with its path, one per line. Item ids
// are resolved against `items`.
FTCORE_API bool importShopStock(std::string_view json, const ItemTable& items, ShopStock& out, std::string* error = nullptr,
                                std::vector<std::string>* warnings = nullptr);

} // namespace ftcore
