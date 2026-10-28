#pragma once

// Items: what a consumable does (Data/Battle/items.json) and the stacks a party carries.
//
//   ftcore::ItemTable table;
//   std::string error;
//   if (!ftcore::importItemTable(itemsText, table, &error)) ...;   // load time: allocates
//   const ftcore::ItemDef* def = table.find("bandage_roll");        // null when the id is unknown
//
//   ftcore::Inventory bag;                  // fixed capacity: copying and every call below never allocate
//   bag.add("bandage_roll", 3);             // returns how many fit
//   bag.remove("bandage_roll", 1);          // returns how many were taken out
//   bag.count("bandage_roll");              // 2
//
// An inventory is a list of stacks, each an item id and a count of 1..kMaxItemStack. A new id goes
// to the end of the list, so stacks keep their pickup order; a stack that is emptied leaves the
// list and the ones after it close up. At most kMaxInventoryStacks different ids fit: a full
// inventory refuses a new id but still tops up the stacks it has. The inventory does not look ids
// up in an ItemTable: an id the table no longer knows (the table may change between versions)
// stays in the bag until it is sold or lost. Ids are lower-case letters, digits and _ (see
// validItemId); the inventory refuses any other text.
//
// Data/Battle/items.json (ids and numbers only; the display text is in Data/Loc/en/ui.json as
// item.<id>.name and item.<id>.desc):
//   { "version": 1, "items": [
//       { "id": "bandage_roll", "effect": "heal", "scope": "ally", "hp": 30, "hpPercent": 10,
//         "buyPrice": 30, "sellPrice": 15 } ] }
//   `id` is required and unique (validItemId). `effect` is required: "heal" (HP to a living ally),
//   "restore_ap" (AP to a living ally) or "revive" (a downed ally gets up with HP). `scope` is
//   "ally" (one member, the default) or "party" (every eligible member). A heal or revive needs
//   `hp` (0..100000, flat HP) or `hpPercent` (0..100, percent of the recipient's max HP) above 0,
//   and takes no `ap`; restore_ap needs `ap` (1..99) and takes neither. `buyPrice` and `sellPrice`
//   (0..100000) are required, and `sellPrice` is at most `buyPrice`. 1..kMaxItemDefs items;
//   unknown keys are warnings.

#include "ftcore/Api.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

inline constexpr std::size_t kItemIdCapacity = 24;     // bytes, terminator included
inline constexpr std::size_t kMaxItemDefs = 32;        // items an ItemTable holds
inline constexpr std::size_t kMaxInventoryStacks = 24; // different items an Inventory holds
inline constexpr int kMaxItemStack = 99;               // of one item
inline constexpr std::size_t kMaxEnemyDrops = 4;       // items one enemy's reward may list

// 1..kItemIdCapacity - 1 lower-case letters, digits and _.
FTCORE_API bool validItemId(std::string_view id);

// An item id in a fixed buffer, so copying one never allocates.
struct ItemId {
    std::array<char, kItemIdCapacity> chars{};

    std::string_view view() const { return std::string_view(chars.data()); }
    bool empty() const { return chars[0] == '\0'; }
    // Copies at most kItemIdCapacity - 1 bytes.
    void set(std::string_view text) {
        chars.fill('\0');
        const std::size_t n = text.size() < kItemIdCapacity ? text.size() : kItemIdCapacity - 1;
        for (std::size_t i = 0; i < n; ++i) chars[i] = text[i];
    }
};

enum class ItemEffect : std::uint8_t {
    Heal,      // HP to a living ally, up to its max HP
    RestoreAp, // AP to a living ally, up to its max AP
    Revive,    // a downed ally gets up with HP (at least 1) and rejoins the turn queue
};
enum class ItemScope : std::uint8_t {
    Ally,  // the one member the player picks
    Party, // every member the effect applies to
};

// "heal", "restore_ap", "revive" and "ally", "party": the names items.json uses. Literals.
FTCORE_API const char* itemEffectName(ItemEffect effect);
FTCORE_API const char* itemScopeName(ItemScope scope);

struct ItemDef {
    ItemId id;
    ItemEffect effect = ItemEffect::Heal;
    ItemScope scope = ItemScope::Ally;
    int hp = 0;        // heal, revive: flat HP
    int hpPercent = 0; // heal, revive: plus this percent of the recipient's max HP
    int ap = 0;        // restore_ap: AP
    int buyPrice = 0;
    int sellPrice = 0; // at most buyPrice

    // The HP a heal or revive gives a member with `maxHp` max HP, before it is capped.
    int hpFor(int maxHp) const { return hp + (maxHp > 0 ? maxHp * hpPercent / 100 : 0); }
};

// Every item the game knows (fixed capacity).
struct ItemTable {
    std::array<ItemDef, kMaxItemDefs> items{};
    std::uint8_t count = 0;

    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
    const ItemDef& operator[](std::size_t i) const { return items[i]; }
    const ItemDef* begin() const { return items.data(); }
    const ItemDef* end() const { return items.data() + count; }
    void clear() { count = 0; }
    // The definition with that id, or null. No allocation.
    FTCORE_API const ItemDef* find(std::string_view id) const;
    // Appends a definition; false (and nothing added) when the table is full or the id is empty
    // or already listed.
    FTCORE_API bool add(const ItemDef& def);
};

// On failure `out` is empty and *error lists each problem with its path, one per line.
FTCORE_API bool importItemTable(std::string_view json, ItemTable& out, std::string* error = nullptr,
                                std::vector<std::string>* warnings = nullptr);

struct ItemStack {
    ItemId id;
    int count = 0; // 1..kMaxItemStack
};

class Inventory {
public:
    // Stacks, in pickup order (slot 0 is the oldest).
    std::size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    const ItemStack& operator[](std::size_t slot) const { return stacks_[slot]; }
    const ItemStack* begin() const { return stacks_.data(); }
    const ItemStack* end() const { return stacks_.data() + count_; }
    void clear() { *this = Inventory{}; }

    // How many of `id` are carried (0 for an id that isn't, or isn't valid).
    FTCORE_API int count(std::string_view id) const;
    // How many more of `id` fit: what is left of its stack, or kMaxItemStack when it would start a
    // new one and there is room, else 0.
    FTCORE_API int space(std::string_view id) const;
    // Adds up to `n` of `id` (n below 1 and an invalid id add nothing) and returns how many fit.
    FTCORE_API int add(std::string_view id, int n);
    // Takes out up to `n` of `id` and returns how many were taken. An emptied stack leaves the list.
    FTCORE_API int remove(std::string_view id, int n);

    // The same stacks, in the same order.
    friend bool operator==(const Inventory& a, const Inventory& b) {
        if (a.count_ != b.count_) return false;
        for (std::size_t i = 0; i < a.count_; ++i)
            if (a.stacks_[i].count != b.stacks_[i].count || a.stacks_[i].id.view() != b.stacks_[i].id.view()) return false;
        return true;
    }

private:
    std::array<ItemStack, kMaxInventoryStacks> stacks_{};
    std::uint8_t count_ = 0;
};

} // namespace ftcore
