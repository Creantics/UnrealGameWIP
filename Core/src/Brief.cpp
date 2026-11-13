#include "ftcore/Brief.h"

#include "ftcore/Json.h"

#include <vector>

namespace ftcore {

const char* briefName(BriefId b) {
    switch (b) {
    case BriefId::Counsel: return "counsel";
    case BriefId::Bailiff: return "bailiff";
    case BriefId::Notary: return "notary";
    case BriefId::Proxy: return "proxy";
    case BriefId::Escrow: return "escrow";
    default: return "none";
    }
}

bool briefFromName(std::string_view name, BriefId& out) {
    for (std::size_t i = 0; i < kBriefCount; ++i) {
        const BriefId b = static_cast<BriefId>(i);
        if (name == briefName(b)) {
            out = b;
            return true;
        }
    }
    return false;
}

const BriefDef* BriefTable::find(BriefId b) const {
    const std::size_t i = static_cast<std::size_t>(b);
    return i < kBriefCount && briefs[i].present ? &briefs[i] : nullptr;
}

namespace {

// Five whole numbers, each lo..hi, summing to `sum`.
bool readFive(JsonValue j, const std::string& path, int lo, int hi, int sum, std::array<int, kLicenceStats>& out,
              std::vector<std::string>& errors) {
    if (!j.isArray() || j.size() != kLicenceStats) {
        errors.push_back(path + ": expected an array of 5 whole numbers");
        return false;
    }
    int total = 0;
    for (std::uint32_t i = 0; i < kLicenceStats; ++i) {
        const JsonValue v = j[i];
        if (!v.isInteger() || v.integer() < lo || v.integer() > hi) {
            errors.push_back(path + "[" + std::to_string(i) + "]: expected a whole number in [" + std::to_string(lo) +
                             ", " + std::to_string(hi) + "]");
            return false;
        }
        out[i] = static_cast<int>(v.integer());
        total += out[i];
    }
    if (total != sum) {
        errors.push_back(path + ": expected a sum of " + std::to_string(sum) + ", got " + std::to_string(total));
        return false;
    }
    return true;
}

void unknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys,
                 std::vector<std::string>& errors) {
    for (std::uint32_t k = 0; k < j.size(); ++k) {
        const std::string_view key = j.key(k);
        bool known = false;
        for (const std::string_view w : keys) known = known || key == w;
        if (!known) errors.push_back((path.empty() ? std::string() : path + ": ") + "unknown key: " + std::string(key));
    }
}

// Stat-driven fighter keys (BriefKit.h): a Brief's kit may not set them; its change scales them instead.
void statKeys(JsonValue j, const std::string& path, std::vector<std::string>& errors) {
    if (!j.isObject()) return;
    static constexpr std::string_view top[] = {"hp", "poise", "stamina", "staminaRegen", "guardAbsorb", "parryWindow",
                                               "dodgeInvulnEnd", "dodgeDistance", "moveSpeed", "readPower",
                                               "readStackPower", "readPoise", "loadShare"};
    for (const std::string_view k : top)
        if (j.has(k)) errors.push_back(path + "." + std::string(k) + ": a stat drives it; use change");
    const auto attack = [&](JsonValue a, const std::string& at, bool poise) {
        if (!a.isObject()) return;
        if (a.has("damage")) errors.push_back(at + ".damage: a stat drives it; use change");
        if (poise && a.has("poiseDamage")) errors.push_back(at + ".poiseDamage: a stat drives it; use change");
    };
    if (const JsonValue light = j["light"]; light.isArray())
        for (std::uint32_t i = 0; i < light.size(); ++i)
            attack(light[i], path + ".light[" + std::to_string(i) + "]", true);
    attack(j["heavy"], path + ".heavy", true);
    attack(j["shot"], path + ".shot", true);
    attack(j["riposte"], path + ".riposte", false);
    attack(j["release"], path + ".release", false);
}

void readChange(JsonValue j, const std::string& path, BriefChange& c, std::vector<std::string>& errors) {
    if (!j.isObject()) {
        errors.push_back(path + ": expected an object");
        return;
    }
    struct Key {
        std::string_view name;
        float BriefChange::*field;
        double lo, hi;
    };
    static constexpr Key keys[] = {
        {"hp", &BriefChange::hp, 0.5, 2.0},
        {"poise", &BriefChange::poise, 0.5, 2.0},
        {"guardLeak", &BriefChange::guardLeak, 0.5, 2.0},
        {"parryWindowAdd", &BriefChange::parryWindowAdd, -0.05, 0.05},
        {"kickDamage", &BriefChange::kickDamage, 0.5, 2.0},
        {"kickPoiseDamage", &BriefChange::kickPoiseDamage, 0.5, 2.0},
        {"releaseDamage", &BriefChange::releaseDamage, 0.5, 2.0},
        {"shotDamage", &BriefChange::shotDamage, 0.5, 2.0},
        {"shotPoiseDamage", &BriefChange::shotPoiseDamage, 0.5, 2.0},
        {"termDuration", &BriefChange::termDuration, 0.5, 2.0},
        {"readPower", &BriefChange::readPower, 0.5, 2.0},
        {"readStackPower", &BriefChange::readStackPower, 0.5, 2.0},
        {"loadShare", &BriefChange::loadShare, 0.5, 2.0},
        {"dodgeDistance", &BriefChange::dodgeDistance, 0.5, 2.0},
        {"invuln", &BriefChange::invuln, 0.5, 2.0},
    };
    for (std::uint32_t k = 0; k < j.size(); ++k) {
        const std::string_view name = j.key(k);
        const Key* key = nullptr;
        for (const Key& w : keys)
            if (w.name == name) key = &w;
        if (!key) {
            errors.push_back(path + ": unknown key: " + std::string(name));
            continue;
        }
        const JsonValue v = j[name];
        if (!v.isNumber() || v.number() < key->lo || v.number() > key->hi) {
            errors.push_back(path + "." + std::string(name) + ": expected a number in [" + std::to_string(key->lo) +
                             ", " + std::to_string(key->hi) + "]");
            continue;
        }
        c.*(key->field) = static_cast<float>(v.number());
    }
}

// Lays a fighter object over `kit`; its unknown keys refuse the file like any other problem.
void overlay(JsonValue j, const std::string& path, ArenaFighter& kit, std::vector<std::string>& errors) {
    if (j.isObject() && (j.has("id") || j.has("weapon"))) errors.push_back(path + ": may not name an id or a weapon");
    std::vector<std::string> warnings;
    arenaOverlayFighter(j, path, kit, errors, &warnings);
    for (const std::string& w : warnings) errors.push_back(w);
}

} // namespace

bool importBriefTable(std::string_view json, const ArenaFighter& lead, BriefTable& out, std::string* error) {
    out = BriefTable{};
    std::vector<std::string> errors;
    const auto finish = [&]() {
        if (errors.empty()) return true;
        out = BriefTable{};
        if (error) {
            error->clear();
            for (const std::string& e : errors) {
                if (!error->empty()) *error += '\n';
                *error += e;
            }
        }
        return false;
    };

    JsonDocument doc;
    std::string err;
    if (!doc.parse(json, &err)) {
        errors.push_back("briefs file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("briefs file: expected an object");
        return finish();
    }
    unknownKeys(root, "", {"version", "common", "briefs"}, errors);
    if (const JsonValue v = root["version"]; !(v.isInteger() && v.integer() == 1)) errors.push_back("version: expected 1");

    ArenaFighter common = lead;
    if (root.has("common")) overlay(root["common"], "common", common, errors);

    const JsonValue list = root["briefs"];
    if (!list.isArray()) {
        errors.push_back("briefs: expected an array");
        return finish();
    }
    BriefTable table;
    for (std::uint32_t i = 0; i < list.size(); ++i) {
        const JsonValue row = list[i];
        const std::string path = "briefs[" + std::to_string(i) + "]";
        if (!row.isObject()) {
            errors.push_back(path + ": expected an object");
            continue;
        }
        unknownKeys(row, path, {"id", "base", "growth", "change", "kit"}, errors);
        BriefId id = BriefId::Count;
        if (!row["id"].isString() || !briefFromName(row["id"].string(), id)) {
            errors.push_back(path + ".id: expected counsel, bailiff, notary, proxy or escrow");
            continue;
        }
        BriefDef& d = table.briefs[static_cast<std::size_t>(id)];
        if (d.present) {
            errors.push_back(path + ".id: duplicate brief: " + briefName(id));
            continue;
        }
        d.present = true;
        readFive(row["base"], path + ".base", 1, 50, kBriefBaseSum, d.base, errors);
        readFive(row["growth"], path + ".growth", 0, 100, kBriefGrowthSum, d.growth, errors);
        if (row.has("change")) readChange(row["change"], path + ".change", d.change, errors);
        d.kit = common;
        if (row.has("kit")) {
            statKeys(row["kit"], path + ".kit", errors);
            overlay(row["kit"], path + ".kit", d.kit, errors);
        }
    }
    if (!errors.empty()) return finish();
    out = table;
    return true;
}

} // namespace ftcore
