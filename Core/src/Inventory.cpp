#include "ftcore/Inventory.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <initializer_list>

namespace ftcore {

namespace {

void itmUnknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys,
                    std::vector<std::string>* warnings) {
    if (!warnings) return;
    for (std::uint32_t k = 0; k < j.size(); ++k) {
        const std::string_view key = j.key(k);
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            warnings->push_back((path.empty() ? std::string() : path + ": ") + "unknown key: " + std::string(key));
    }
}

// An integer in [lo, hi]. Absent is fine unless `required`; present but not one is an error.
bool itmInt(JsonValue j, const char* key, int& out, int lo, int hi, bool required, const std::string& path,
            std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent() && !required) return true;
    if (!v.isInteger() || v.integer() < lo || v.integer() > hi) {
        errors.push_back(path + "." + key + ": expected an integer in " + std::to_string(lo) + ".." + std::to_string(hi));
        return false;
    }
    out = static_cast<int>(v.integer());
    return true;
}

bool itmFinish(const std::vector<std::string>& errors, std::string* error) {
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

bool validItemId(std::string_view id) {
    if (id.empty() || id.size() >= kItemIdCapacity) return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

const char* itemEffectName(ItemEffect effect) {
    switch (effect) {
    case ItemEffect::Heal: return "heal";
    case ItemEffect::RestoreAp: return "restore_ap";
    case ItemEffect::Revive: return "revive";
    }
    return "heal";
}

const char* itemScopeName(ItemScope scope) {
    switch (scope) {
    case ItemScope::Ally: return "ally";
    case ItemScope::Party: return "party";
    }
    return "ally";
}

const ItemDef* ItemTable::find(std::string_view id) const {
    for (std::size_t i = 0; i < count; ++i)
        if (items[i].id.view() == id) return &items[i];
    return nullptr;
}

bool ItemTable::add(const ItemDef& def) {
    if (def.id.empty() || count >= kMaxItemDefs || find(def.id.view())) return false;
    items[count++] = def;
    return true;
}

bool importItemTable(std::string_view json, ItemTable& out, std::string* error, std::vector<std::string>* warnings) {
    out = ItemTable{};
    std::vector<std::string> errors;
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) {
        errors.push_back("items file: " + parseError);
        return itmFinish(errors, error);
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("items file: expected an object");
        return itmFinish(errors, error);
    }
    if (const JsonValue v = root["version"]; !v.isAbsent() && !(v.isInteger() && v.integer() == 1))
        errors.push_back("version: expected 1");
    itmUnknownKeys(root, "", {"version", "items"}, warnings);
    const JsonValue list = root["items"];
    if (!list.isArray() || list.size() == 0 || list.size() > kMaxItemDefs)
        errors.push_back("items: expected an array of 1.." + std::to_string(kMaxItemDefs) + " items");

    ItemTable table;
    for (std::uint32_t i = 0; i < list.size() && list.isArray(); ++i) {
        const JsonValue j = list[i];
        const std::string path = "items[" + std::to_string(i) + "]";
        if (!j.isObject()) {
            errors.push_back(path + ": expected an object");
            continue;
        }
        itmUnknownKeys(j, path, {"id", "effect", "scope", "hp", "hpPercent", "ap", "buyPrice", "sellPrice"}, warnings);
        ItemDef d;
        const JsonValue id = j["id"];
        if (!id.isString() || !validItemId(id.string()))
            errors.push_back(path + ".id: expected lower-case letters, digits and _, at most " +
                             std::to_string(kItemIdCapacity - 1) + " bytes");
        else if (table.find(id.string()))
            errors.push_back(path + ".id: duplicate \"" + std::string(id.string()) + "\"");
        else
            d.id.set(id.string());

        const std::string_view effect = j["effect"].string();
        const bool effectOk = effect == "heal" || effect == "restore_ap" || effect == "revive";
        if (effect == "heal") d.effect = ItemEffect::Heal;
        else if (effect == "restore_ap") d.effect = ItemEffect::RestoreAp;
        else if (effect == "revive") d.effect = ItemEffect::Revive;
        else errors.push_back(path + ".effect: expected \"heal\", \"restore_ap\" or \"revive\"");

        if (j.has("scope")) {
            const std::string_view scope = j["scope"].string();
            if (scope == "ally") d.scope = ItemScope::Ally;
            else if (scope == "party") d.scope = ItemScope::Party;
            else errors.push_back(path + ".scope: expected \"ally\" or \"party\"");
        }

        const bool hpOk = itmInt(j, "hp", d.hp, 0, 100000, false, path, errors);
        const bool percentOk = itmInt(j, "hpPercent", d.hpPercent, 0, 100, false, path, errors);
        const bool apOk = itmInt(j, "ap", d.ap, 0, 99, false, path, errors);
        const bool buyOk = itmInt(j, "buyPrice", d.buyPrice, 0, 100000, true, path, errors);
        const bool sellOk = itmInt(j, "sellPrice", d.sellPrice, 0, 100000, true, path, errors);
        if (buyOk && sellOk && d.sellPrice > d.buyPrice)
            errors.push_back(path + ".sellPrice: expected at most buyPrice (" + std::to_string(d.buyPrice) + ")");
        if (effectOk && hpOk && percentOk && apOk) {
            const std::string kind = std::string(itemEffectName(d.effect)) + " item";
            if (d.effect == ItemEffect::RestoreAp) {
                if (d.ap < 1) errors.push_back(path + ".ap: expected an integer in 1..99 for a " + kind);
                if (d.hp != 0 || d.hpPercent != 0) errors.push_back(path + ": a " + kind + " takes ap only, not hp or hpPercent");
            } else {
                if (d.hp == 0 && d.hpPercent == 0) errors.push_back(path + ": expected hp or hpPercent above 0 for a " + kind);
                if (d.ap != 0) errors.push_back(path + ".ap: a " + kind + " takes hp and hpPercent, not ap");
            }
        }
        if (!d.id.empty()) table.add(d);
    }
    if (!itmFinish(errors, error)) return false;
    out = table;
    return true;
}

int Inventory::count(std::string_view id) const {
    for (std::size_t i = 0; i < count_; ++i)
        if (stacks_[i].id.view() == id) return stacks_[i].count;
    return 0;
}

int Inventory::space(std::string_view id) const {
    if (!validItemId(id)) return 0;
    for (std::size_t i = 0; i < count_; ++i)
        if (stacks_[i].id.view() == id) return kMaxItemStack - stacks_[i].count;
    return count_ < kMaxInventoryStacks ? kMaxItemStack : 0;
}

int Inventory::add(std::string_view id, int n) {
    if (n < 1 || !validItemId(id)) return 0;
    for (std::size_t i = 0; i < count_; ++i) {
        ItemStack& stack = stacks_[i];
        if (stack.id.view() != id) continue;
        const int put = std::min(n, kMaxItemStack - stack.count);
        stack.count += put;
        return put;
    }
    if (count_ >= kMaxInventoryStacks) return 0;
    ItemStack& fresh = stacks_[count_++];
    fresh.id.set(id);
    fresh.count = std::min(n, kMaxItemStack);
    return fresh.count;
}

int Inventory::remove(std::string_view id, int n) {
    if (n < 1) return 0;
    for (std::size_t i = 0; i < count_; ++i) {
        ItemStack& stack = stacks_[i];
        if (stack.id.view() != id) continue;
        const int taken = std::min(n, stack.count);
        stack.count -= taken;
        if (stack.count == 0) {
            for (std::size_t k = i + 1; k < count_; ++k) stacks_[k - 1] = stacks_[k]; // the later stacks close up
            stacks_[--count_] = ItemStack{};
        }
        return taken;
    }
    return 0;
}

} // namespace ftcore
