#include "ftcore/Shop.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cstdint>

namespace ftcore {

namespace {

bool shopFinish(const std::vector<std::string>& errors, std::string* error) {
    if (errors.empty()) return true;
    if (error) {
        error->clear();
        for (const std::string& e : errors) {
            if (!error->empty()) *error += '\n';
            *error += e;
        }
    }
    return false;
}

} // namespace

bool ShopStock::contains(std::string_view id) const {
    for (std::size_t i = 0; i < count; ++i)
        if (ids[i].view() == id) return true;
    return false;
}

bool ShopStock::add(std::string_view id) {
    if (!validItemId(id) || count >= kMaxShopStock || contains(id)) return false;
    ids[count++].set(id);
    return true;
}

ShopResult buy(PartyProgress& progress, const ItemTable& items, const ShopStock& stock, std::string_view id, int count) {
    if (count < 1) return ShopResult::InvalidCount;
    const ItemDef* def = items.find(id);
    if (!def) return ShopResult::UnknownItem;
    if (!stock.contains(id)) return ShopResult::NotStocked;
    // 64-bit: a price of 100000 times a huge count must not wrap around into an affordable one.
    const std::int64_t cost = static_cast<std::int64_t>(def->buyPrice) * count;
    if (cost > progress.gold) return ShopResult::NotEnoughGold;
    if (progress.items.space(id) < count) return ShopResult::InventoryFull;
    progress.gold -= static_cast<int>(cost);
    progress.items.add(id, count);
    return ShopResult::Ok;
}

ShopResult sell(PartyProgress& progress, const ItemTable& items, std::string_view id, int count) {
    if (count < 1) return ShopResult::InvalidCount;
    const ItemDef* def = items.find(id);
    if (!def) return ShopResult::UnknownItem;
    if (progress.items.count(id) < count) return ShopResult::NotCarried;
    progress.items.remove(id, count);
    const std::int64_t gold = static_cast<std::int64_t>(progress.gold) + static_cast<std::int64_t>(def->sellPrice) * count;
    progress.gold = static_cast<int>(std::min<std::int64_t>(gold, kMaxGold));
    return ShopResult::Ok;
}

bool importShopStock(std::string_view json, const ItemTable& items, ShopStock& out, std::string* error,
                     std::vector<std::string>* warnings) {
    out = ShopStock{};
    std::vector<std::string> errors;
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) {
        errors.push_back("shops file: " + parseError);
        return shopFinish(errors, error);
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("shops file: expected an object");
        return shopFinish(errors, error);
    }
    if (const JsonValue v = root["version"]; !v.isAbsent() && !(v.isInteger() && v.integer() == 1))
        errors.push_back("version: expected 1");
    if (warnings) {
        for (std::uint32_t k = 0; k < root.size(); ++k) {
            const std::string_view key = root.key(k);
            if (key != "version" && key != "default") warnings->push_back("unknown key: " + std::string(key));
        }
    }

    const JsonValue list = root["default"];
    if (!list.isArray() || list.size() == 0 || list.size() > kMaxShopStock)
        errors.push_back("default: expected an array of 1.." + std::to_string(kMaxShopStock) + " item ids");
    ShopStock stock;
    for (std::uint32_t i = 0; i < list.size() && list.isArray(); ++i) {
        const JsonValue entry = list[i];
        const std::string path = "default[" + std::to_string(i) + "]";
        if (!entry.isString() || !validItemId(entry.string()))
            errors.push_back(path + ": expected an item id (lower-case letters, digits and _)");
        else if (!items.find(entry.string()))
            errors.push_back(path + ": unknown item \"" + std::string(entry.string()) + "\"");
        else if (stock.contains(entry.string()))
            errors.push_back(path + ": duplicate \"" + std::string(entry.string()) + "\"");
        else
            stock.add(entry.string()); // a full stock was already reported
    }
    if (!shopFinish(errors, error)) return false;
    out = stock;
    return true;
}

} // namespace ftcore
