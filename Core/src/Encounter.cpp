#include "ftcore/Encounter.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iterator>

namespace ftcore {

namespace {

std::string encStr(std::string_view s) { return std::string(s); }

void encUnknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys,
                    std::vector<std::string>* warnings) {
    if (!warnings) return;
    for (std::uint32_t k = 0; k < j.size(); ++k) {
        const std::string_view key = j.key(k);
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            warnings->push_back((path.empty() ? std::string() : path + ": ") + "unknown key: " + encStr(key));
    }
}

bool encId(std::string_view id) {
    if (id.empty()) return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

// An optional integer in [lo, hi]; false (and an error) when present but not one.
bool encInt(JsonValue j, const char* key, int& out, int lo, int hi, const std::string& path,
            std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return true;
    if (!v.isInteger() || v.integer() < lo || v.integer() > hi) {
        errors.push_back(path + "." + key + ": expected an integer in " + std::to_string(lo) + ".." + std::to_string(hi));
        return false;
    }
    out = static_cast<int>(v.integer());
    return true;
}

bool encNum(JsonValue j, const char* key, float& out, float lo, float hi, const std::string& path,
            std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return true;
    const double d = v.number(-1e30);
    if (!v.isNumber() || !std::isfinite(d) || d < lo || d > hi) {
        errors.push_back(path + "." + key + ": expected a number in " + std::to_string(lo) + ".." + std::to_string(hi));
        return false;
    }
    out = static_cast<float>(d);
    return true;
}

bool encRoot(std::string_view json, JsonDocument& doc, const char* what, std::vector<std::string>& errors) {
    std::string err;
    if (!doc.parse(json, &err)) {
        errors.push_back(std::string(what) + ": " + err);
        return false;
    }
    if (!doc.root().isObject()) {
        errors.push_back(std::string(what) + ": expected an object");
        return false;
    }
    if (const JsonValue v = doc.root()["version"]; !v.isAbsent() && !(v.isInteger() && v.integer() == 1))
        errors.push_back("version: expected 1");
    return true;
}

bool encFinish(std::vector<std::string>& errors, std::string* error) {
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

bool encLineup(JsonValue list, const EnemyRoster& roster, const std::string& path, EncounterLineup& out,
               std::vector<std::string>& errors) {
    if (!list.isArray() || list.size() == 0) {
        errors.push_back(path + ": expected a non-empty array of enemy ids");
        return false;
    }
    bool ok = true;
    for (std::uint32_t i = 0; i < list.size(); ++i) {
        const int e = roster.find(list[i].string());
        if (e < 0) {
            errors.push_back(path + "[" + std::to_string(i) + "]: unknown enemy \"" + encStr(list[i].string()) + "\"");
            ok = false;
        } else {
            out.enemies.push_back(static_cast<std::uint32_t>(e));
        }
    }
    return ok;
}

} // namespace

int EnemyRoster::find(std::string_view id) const {
    for (std::size_t i = 0; i < enemies.size(); ++i)
        if (enemies[i].id == id) return static_cast<int>(i);
    return -1;
}

const EncounterEntry* EncounterTable::findEntry(std::string_view scene, std::uint32_t beat, std::uint32_t step) const {
    for (const EncounterEntry& e : entries)
        if (e.beat == beat && e.step == step && e.scene == scene) return &e;
    return nullptr;
}

const EncounterLineup& EncounterTable::find(std::string_view scene, std::uint32_t beat, std::uint32_t step) const {
    const EncounterEntry* e = findEntry(scene, beat, step);
    return e ? e->lineup : fallback;
}

bool importEnemyRoster(std::string_view json, EnemyRoster& out, std::string* error, std::vector<std::string>* warnings) {
    out = EnemyRoster{};
    std::vector<std::string> errors;
    JsonDocument doc;
    if (!encRoot(json, doc, "enemies file", errors)) return encFinish(errors, error);
    const JsonValue root = doc.root();
    encUnknownKeys(root, "", {"version", "enemies"}, warnings);
    const JsonValue list = root["enemies"];
    if (!list.isArray() || list.size() == 0) errors.push_back("enemies: expected a non-empty array");

    EnemyRoster roster;
    for (std::uint32_t i = 0; i < list.size(); ++i) {
        const JsonValue j = list[i];
        const std::string path = "enemies[" + std::to_string(i) + "]";
        if (!j.isObject()) {
            errors.push_back(path + ": expected an object");
            continue;
        }
        encUnknownKeys(j, path, {"id", "name", "stats", "skills"}, warnings);
        EnemyDef e;
        e.id = encStr(j["id"].string());
        if (!encId(e.id)) errors.push_back(path + ".id: expected lower-case letters, digits and _");
        else if (roster.find(e.id) >= 0) errors.push_back(path + ".id: duplicate \"" + e.id + "\"");
        e.name = j.has("name") ? encStr(j["name"].string()) : e.id;
        if (j.has("name") && !j["name"].isString()) errors.push_back(path + ".name: expected a string");

        const JsonValue st = j["stats"];
        const std::string sp = path + ".stats";
        if (!st.isObject()) {
            errors.push_back(sp + ": expected an object");
        } else {
            encUnknownKeys(st, sp, {"maxHp", "attack", "defense", "speed", "technique"}, warnings);
            encInt(st, "maxHp", e.stats.maxHp, 1, 1000000, sp, errors);
            encInt(st, "attack", e.stats.attack, 0, 100000, sp, errors);
            encInt(st, "defense", e.stats.defense, 0, 100000, sp, errors);
            encInt(st, "speed", e.stats.speed, 1, 100000, sp, errors);
            encInt(st, "technique", e.stats.technique, 0, 100000, sp, errors);
        }

        const JsonValue skills = j["skills"];
        if (!skills.isArray() || skills.size() == 0) errors.push_back(path + ".skills: expected a non-empty array");
        for (std::uint32_t k = 0; k < skills.size(); ++k) {
            const JsonValue s = skills[k];
            const std::string kp = path + ".skills[" + std::to_string(k) + "]";
            if (!s.isObject()) {
                errors.push_back(kp + ": expected an object");
                continue;
            }
            encUnknownKeys(s, kp, {"id", "name", "apCost", "apGain", "targeting", "power", "notes", "difficulty"},
                           warnings);
            Skill skill;
            const std::string id = encStr(s["id"].string());
            if (!encId(id)) errors.push_back(kp + ".id: expected lower-case letters, digits and _");
            else if (std::find(e.skillIds.begin(), e.skillIds.end(), id) != e.skillIds.end())
                errors.push_back(kp + ".id: duplicate \"" + id + "\"");
            skill.name = s.has("name") ? encStr(s["name"].string()) : id;
            encInt(s, "apCost", skill.apCost, 0, 99, kp, errors);
            encInt(s, "apGain", skill.apGain, 0, 99, kp, errors);
            encNum(s, "power", skill.power, 0.f, 100.f, kp, errors);
            encInt(s, "notes", skill.notes, 1, 64, kp, errors);
            encNum(s, "difficulty", skill.difficulty, 0.f, 10.f, kp, errors);
            const std::string_view targeting = s["targeting"].string("single");
            if (targeting == "single") skill.targeting = Targeting::SingleEnemy;
            else if (targeting == "all") skill.targeting = Targeting::AllEnemies;
            else errors.push_back(kp + ".targeting: expected \"single\" or \"all\"");
            e.skillIds.push_back(id);
            e.skills.push_back(std::move(skill));
        }
        roster.enemies.push_back(std::move(e));
    }
    if (!encFinish(errors, error)) return false;
    out = std::move(roster);
    return true;
}

bool importEncounterTable(std::string_view json, const EnemyRoster& roster, EncounterTable& out, std::string* error,
                          std::vector<std::string>* warnings) {
    out = EncounterTable{};
    std::vector<std::string> errors;
    JsonDocument doc;
    if (!encRoot(json, doc, "encounters file", errors)) return encFinish(errors, error);
    const JsonValue root = doc.root();
    encUnknownKeys(root, "", {"version", "default", "encounters"}, warnings);

    EncounterTable table;
    const JsonValue def = root["default"];
    if (!def.isObject()) {
        errors.push_back("default: expected an object");
    } else {
        encUnknownKeys(def, "default", {"enemies"}, warnings);
        encLineup(def["enemies"], roster, "default.enemies", table.fallback, errors);
    }

    const JsonValue list = root["encounters"];
    if (!list.isAbsent() && !list.isArray()) errors.push_back("encounters: expected an array");
    for (std::uint32_t i = 0; i < list.size() && list.isArray(); ++i) {
        const JsonValue j = list[i];
        const std::string path = "encounters[" + std::to_string(i) + "]";
        if (!j.isObject()) {
            errors.push_back(path + ": expected an object");
            continue;
        }
        encUnknownKeys(j, path, {"scene", "beat", "step", "enemies"}, warnings);
        EncounterEntry e;
        e.scene = encStr(j["scene"].string());
        if (e.scene.empty()) errors.push_back(path + ".scene: expected a scene id");
        int beat = -1, step = 0;
        if (!j.has("beat")) errors.push_back(path + ".beat: expected an integer in 0..1000000");
        encInt(j, "beat", beat, 0, 1000000, path, errors);
        encInt(j, "step", step, 0, 1000, path, errors);
        e.beat = beat < 0 ? 0u : static_cast<std::uint32_t>(beat);
        e.step = static_cast<std::uint32_t>(step);
        encLineup(j["enemies"], roster, path + ".enemies", e.lineup, errors);
        if (beat >= 0 && !e.scene.empty() && table.findEntry(e.scene, e.beat, e.step))
            errors.push_back(path + ": duplicate entry for " + e.scene + " beat " + std::to_string(e.beat) + " step " +
                             std::to_string(e.step));
        table.entries.push_back(std::move(e));
    }
    if (!encFinish(errors, error)) return false;
    out = std::move(table);
    return true;
}

void makeEnemies(const EnemyRoster& roster, const EncounterLineup& lineup, std::vector<Combatant>& out) {
    out.clear();
    out.reserve(lineup.enemies.size());
    for (const std::uint32_t index : lineup.enemies) {
        if (index >= roster.enemies.size()) continue;
        const EnemyDef& e = roster.enemies[index];
        Combatant c;
        c.name = e.name;
        c.team = Team::Enemies;
        c.stats = e.stats;
        c.hp = e.stats.maxHp;
        c.skills = e.skills;
        out.push_back(std::move(c));
    }
}

} // namespace ftcore
