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

// A number in [lo, hi] (required when `required`, else optional and left alone when absent).
bool encDouble(JsonValue j, const char* key, double& out, double lo, double hi, bool required, const std::string& path,
               std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent() && !required) return true;
    const double d = v.number(-1e30);
    if (!v.isNumber() || !std::isfinite(d) || d < lo || d > hi) {
        errors.push_back(path + "." + key + ": expected a number in " + std::to_string(lo) + ".." + std::to_string(hi));
        return false;
    }
    out = d;
    return true;
}

// A skill's optional "hits" array: its attack timeline. Absent: the default single hit.
void encHits(JsonValue s, const std::string& path, AttackTimeline& out, std::vector<std::string>& errors,
             std::vector<std::string>* warnings) {
    const JsonValue hits = s["hits"];
    if (hits.isAbsent()) return;
    const std::string hp = path + ".hits";
    if (!hits.isArray() || hits.size() == 0 || hits.size() > kMaxTimelineHits) {
        errors.push_back(hp + ": expected an array of 1.." + std::to_string(kMaxTimelineHits) + " hits");
        return;
    }
    out.clear();
    double previous = 0.0;
    for (std::uint32_t i = 0; i < hits.size(); ++i) {
        const JsonValue h = hits[i];
        const std::string p = hp + "[" + std::to_string(i) + "]";
        if (!h.isObject()) {
            errors.push_back(p + ": expected an object");
            continue;
        }
        encUnknownKeys(h, p, {"at", "tell", "telegraph", "weight", "jumpable"}, warnings);
        TimelineHit hit;
        if (encDouble(h, "at", hit.at, 0.0, kMaxHitOffset, true, p, errors)) {
            if (hit.at <= previous) errors.push_back(p + ".at: expected a time after the previous hit (and above 0)");
            previous = hit.at;
        }
        encDouble(h, "tell", hit.tell, kMinTell, kMaxTell, false, p, errors);
        encDouble(h, "telegraph", hit.telegraph, 0.0, kMaxTelegraph, false, p, errors);
        if (hit.tell > hit.at) errors.push_back(p + ".tell: longer than the time from the banner to the hit");
        if (hit.telegraph > hit.at) errors.push_back(p + ".telegraph: longer than the time from the banner to the hit");
        encNum(h, "weight", hit.weight, 0.f, 100.f, p, errors);
        if (!(hit.weight > 0.0f)) errors.push_back(p + ".weight: expected a number above 0");
        const JsonValue jump = h["jumpable"];
        if (!jump.isAbsent() && !jump.isBool()) errors.push_back(p + ".jumpable: expected true or false");
        hit.jumpable = jump.boolean(false);
        out.add(hit);
    }
}

// A reward's optional "items" array: its item drops. Absent: none.
void encDrops(JsonValue rw, const std::string& path, Inventory& out, std::vector<std::string>& errors,
              std::vector<std::string>* warnings) {
    const JsonValue drops = rw["items"];
    if (drops.isAbsent()) return;
    const std::string dp = path + ".items";
    if (!drops.isArray() || drops.size() > kMaxEnemyDrops) {
        errors.push_back(dp + ": expected an array of 0.." + std::to_string(kMaxEnemyDrops) + " drops");
        return;
    }
    for (std::uint32_t i = 0; i < drops.size(); ++i) {
        const JsonValue d = drops[i];
        const std::string p = dp + "[" + std::to_string(i) + "]";
        if (!d.isObject()) {
            errors.push_back(p + ": expected an object");
            continue;
        }
        encUnknownKeys(d, p, {"id", "count"}, warnings);
        const JsonValue id = d["id"];
        int count = 1;
        const bool countOk = encInt(d, "count", count, 1, kMaxItemStack, p, errors);
        if (!id.isString() || !validItemId(id.string()))
            errors.push_back(p + ".id: expected an item id (lower-case letters, digits and _)");
        else if (out.count(id.string()) > 0)
            errors.push_back(p + ".id: duplicate \"" + encStr(id.string()) + "\"");
        else if (countOk)
            out.add(id.string(), count);
    }
}

// An enemy's optional "phases" array (its skill ids already read). Absent: no phases.
void encPhases(JsonValue j, const std::string& path, const std::vector<std::string>& skillIds, EnemyPhases& out,
               std::vector<std::string>& errors, std::vector<std::string>* warnings) {
    const JsonValue phases = j["phases"];
    if (phases.isAbsent()) return;
    const std::string pp = path + ".phases";
    if (!phases.isArray() || phases.size() == 0 || phases.size() > kMaxEnemyPhases) {
        errors.push_back(pp + ": expected an array of 1.." + std::to_string(kMaxEnemyPhases) + " phases");
        return;
    }
    out.clear();
    float previous = 1.0f;
    for (std::uint32_t i = 0; i < phases.size(); ++i) {
        const JsonValue ph = phases[i];
        const std::string p = pp + "[" + std::to_string(i) + "]";
        if (!ph.isObject()) {
            errors.push_back(p + ": expected an object");
            continue;
        }
        encUnknownKeys(ph, p, {"below", "label", "pattern", "loop", "attack", "heal"}, warnings);
        EnemyPhase phase;
        if (i == 0) {
            if (ph.has("below")) errors.push_back(p + ".below: the first phase holds from the start (no threshold)");
        } else if (!ph.has("below")) {
            errors.push_back(p + ".below: expected a fraction of max HP above 0 and below 1");
        } else if (encNum(ph, "below", phase.hpBelow, 0.f, 1.f, p, errors)) {
            if (!(phase.hpBelow > 0.0f && phase.hpBelow < previous))
                errors.push_back(p + ".below: expected a fraction above 0 and below the previous phase's");
            previous = phase.hpBelow;
        }
        const JsonValue label = ph["label"];
        if (!label.isAbsent()) {
            if (!label.isString() || label.string().size() >= kPhaseLabelCapacity)
                errors.push_back(p + ".label: expected a string of at most " + std::to_string(kPhaseLabelCapacity - 1) +
                                 " bytes");
            else phase.setLabel(label.string());
        }
        const JsonValue pattern = ph["pattern"];
        if (!pattern.isArray() || pattern.size() == 0 || pattern.size() > kMaxPatternSteps) {
            errors.push_back(p + ".pattern: expected an array of 1.." + std::to_string(kMaxPatternSteps) + " skill ids");
        } else {
            for (std::uint32_t k = 0; k < pattern.size(); ++k) {
                const std::string_view id = pattern[k].string();
                const auto it = std::find(skillIds.begin(), skillIds.end(), id);
                if (!pattern[k].isString() || it == skillIds.end())
                    errors.push_back(p + ".pattern[" + std::to_string(k) + "]: expected one of the enemy's skill ids");
                else phase.addStep(static_cast<std::size_t>(std::distance(skillIds.begin(), it)));
            }
        }
        int loop = 0;
        if (encInt(ph, "loop", loop, 0, static_cast<int>(kMaxPatternSteps) - 1, p, errors) && phase.length > 0 &&
            loop >= phase.length)
            errors.push_back(p + ".loop: expected a step of the pattern (0.." + std::to_string(phase.length - 1) + ")");
        phase.loopFrom = static_cast<std::uint8_t>(loop);
        encInt(ph, "attack", phase.attackPercent, 0, kMaxPhaseAttackPercent, p, errors);
        encInt(ph, "heal", phase.healPercent, 0, 100, p, errors);
        out.add(phase);
    }
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
    if (list.size() > kMaxLineupEnemies) {
        errors.push_back(path + ": expected at most " + std::to_string(kMaxLineupEnemies) + " enemies");
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

CombatKind EncounterTable::combatFor(std::string_view scene, std::uint32_t beat, std::uint32_t step) const {
    const EncounterEntry* e = findEntry(scene, beat, step);
    return e ? e->combat : CombatKind::Rhythm;
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
        encUnknownKeys(j, path, {"id", "name", "stats", "skills", "reward", "phases"}, warnings);
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
            encUnknownKeys(st, sp, {"maxHp", "attack", "defense", "speed", "technique", "poise"}, warnings);
            encInt(st, "maxHp", e.stats.maxHp, 1, 1000000, sp, errors);
            encInt(st, "attack", e.stats.attack, 0, 100000, sp, errors);
            encInt(st, "defense", e.stats.defense, 0, 100000, sp, errors);
            encInt(st, "speed", e.stats.speed, 1, 100000, sp, errors);
            encInt(st, "technique", e.stats.technique, 0, 100000, sp, errors);
            encInt(st, "poise", e.stats.maxPoise, 0, kMaxPoise, sp, errors);
        }

        const JsonValue rw = j["reward"];
        if (!rw.isAbsent()) {
            const std::string rp = path + ".reward";
            if (!rw.isObject()) {
                errors.push_back(rp + ": expected an object");
            } else {
                encUnknownKeys(rw, rp, {"xp", "gold", "items"}, warnings);
                encInt(rw, "xp", e.reward.xp, 0, 100000, rp, errors);
                encInt(rw, "gold", e.reward.gold, 0, 100000, rp, errors);
                encDrops(rw, rp, e.reward.items, errors, warnings);
            }
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
            encUnknownKeys(s, kp,
                           {"id", "name", "apCost", "apGain", "targeting", "power", "notes", "difficulty", "hits"},
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
            encHits(s, kp, skill.timeline, errors, warnings);
            const std::string_view targeting = s["targeting"].string("single");
            if (targeting == "single") skill.targeting = Targeting::SingleEnemy;
            else if (targeting == "all") skill.targeting = Targeting::AllEnemies;
            else errors.push_back(kp + ".targeting: expected \"single\" or \"all\"");
            e.skillIds.push_back(id);
            e.skills.push_back(std::move(skill));
        }
        encPhases(j, path, e.skillIds, e.phases, errors, warnings);
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
        encUnknownKeys(def, "default", {"enemies", "tier", "combat"}, warnings);
        if (def.has("combat")) errors.push_back("default.combat: the default line-up is a Turns fight; tag an encounter instead");
        encLineup(def["enemies"], roster, "default.enemies", table.fallback, errors);
        encInt(def, "tier", table.fallback.tier, 0, kMaxTier, "default", errors);
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
        encUnknownKeys(j, path, {"scene", "beat", "step", "tier", "enemies", "combat"}, warnings);
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
        encInt(j, "tier", e.lineup.tier, 0, kMaxTier, path, errors);
        const JsonValue combat = j["combat"];
        if (!combat.isAbsent() && !(combat.isString() && parseCombatKind(combat.string(), e.combat)))
            errors.push_back(path + ".combat: expected rhythm, arena or beat");
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
        const int tier = lineup.tier < 0 ? 0 : (lineup.tier > kMaxTier ? kMaxTier : lineup.tier);
        c.stats.maxHp += e.stats.maxHp * tier * kTierHpPercent / 100;
        c.stats.attack += e.stats.attack * tier * kTierAttackPercent / 100;
        c.hp = c.stats.maxHp;
        c.skills = e.skills;
        c.phases = e.phases;
        out.push_back(std::move(c));
    }
}

} // namespace ftcore
