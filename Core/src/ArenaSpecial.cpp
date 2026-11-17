#include "ftcore/ArenaSpecial.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>

namespace ftcore {

namespace {

bool spId(std::string_view id) {
    if (id.empty() || id.size() > kMaxArenaSpecialId) return false;
    for (const char c : id)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    return true;
}

template <std::size_t N>
void spCopy(std::string_view s, std::array<char, N>& out) {
    out.fill('\0');
    std::copy_n(s.begin(), std::min(s.size(), N - 1), out.begin());
}

bool inRange(float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; }

void spUnknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys,
                   std::vector<std::string>* warnings) {
    if (!warnings) return;
    for (std::uint32_t k = 0; k < j.size(); ++k) {
        const std::string_view key = j.key(k);
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            warnings->push_back((path.empty() ? std::string() : path + ": ") + "unknown key: " + std::string(key));
    }
}

// An optional number in [lo, hi] into `out`.
void spNumber(JsonValue j, const char* key, const std::string& path, float lo, float hi, float& out,
              std::vector<std::string>& errors) {
    const JsonValue v = j[key];
    if (v.isAbsent()) return;
    const double d = v.number(-1.0e30);
    if (!v.isNumber() || !std::isfinite(d) || d < static_cast<double>(lo) || d > static_cast<double>(hi)) {
        errors.push_back(path + "." + key + ": expected a number in " + std::to_string(lo) + ".." + std::to_string(hi));
        return;
    }
    out = static_cast<float>(d);
}

void spMove(JsonValue j, const std::string& path, ArenaSpecialRules& r, std::vector<std::string>& errors,
            std::vector<std::string>* warnings) {
    if (!j.isObject()) {
        errors.push_back(path + ": expected an object");
        return;
    }
    spUnknownKeys(j, path,
                  {"id", "lead", "duration", "impactAt", "range", "laneLength", "laneWidth", "damage", "otherShare",
                   "bossPhaseShare", "knockBack", "knockTime", "skippable", "cues"},
                  warnings);
    for (const char* key : {"id", "lead"}) {
        const JsonValue v = j[key];
        if (!v.isString() || !spId(v.string())) {
            errors.push_back(path + "." + key + ": expected an id of 1.." + std::to_string(kMaxArenaSpecialId) +
                             " letters, digits, _ and -");
            continue;
        }
        spCopy(v.string(), key[0] == 'i' ? r.id : r.lead);
    }
    spNumber(j, "duration", path, 0.5f, 30.f, r.duration, errors);
    spNumber(j, "impactAt", path, 0.f, 30.f, r.impactAt, errors);
    spNumber(j, "range", path, 1.f, 200.f, r.range, errors);
    spNumber(j, "laneLength", path, 0.5f, 100.f, r.laneLength, errors);
    spNumber(j, "laneWidth", path, 0.5f, 50.f, r.laneWidth, errors);
    if (const JsonValue v = j["damage"]; !v.isAbsent()) {
        if (!v.isInteger() || v.integer() < 1 || v.integer() > 100000)
            errors.push_back(path + ".damage: expected an integer in 1..100000");
        else
            r.damage = static_cast<int>(v.integer());
    }
    spNumber(j, "otherShare", path, 0.f, 1.f, r.otherShare, errors);
    spNumber(j, "bossPhaseShare", path, 0.f, 1.f, r.bossPhaseShare, errors);
    spNumber(j, "knockBack", path, 0.f, 20.f, r.knockBack, errors);
    spNumber(j, "knockTime", path, 0.05f, 5.f, r.knockTime, errors);
    if (const JsonValue v = j["skippable"]; !v.isAbsent()) {
        if (!v.isBool())
            errors.push_back(path + ".skippable: expected true or false");
        else
            r.skippable = v.boolean();
    }
    if (!(r.impactAt > 0.f && r.impactAt <= r.duration))
        errors.push_back(path + ".impactAt: expected a time in (0, duration]");

    const JsonValue cues = j["cues"];
    if (cues.isAbsent()) return;
    if (!cues.isArray() || cues.size() > kMaxArenaSpecialCues) {
        errors.push_back(path + ".cues: expected an array of 0.." + std::to_string(kMaxArenaSpecialCues) + " cues");
        return;
    }
    float last = 0.f;
    for (std::uint32_t i = 0; i < cues.size(); ++i) {
        const std::string cp = path + ".cues[" + std::to_string(i) + "]";
        const JsonValue c = cues[i];
        if (!c.isObject()) {
            errors.push_back(cp + ": expected an object");
            continue;
        }
        spUnknownKeys(c, cp, {"at", "cue"}, warnings);
        ArenaSpecialCue& out = r.cues[r.cueCount++];
        const JsonValue at = c["at"];
        const double t = at.number(-1.0);
        if (!at.isNumber() || !std::isfinite(t) || t < 0.0 || t > static_cast<double>(r.duration) ||
            static_cast<float>(t) < last)
            errors.push_back(cp + ".at: expected a time in [0, duration], not before the cue above");
        else
            out.at = last = static_cast<float>(t);
        const JsonValue name = c["cue"];
        if (!name.isString() || !spId(name.string()))
            errors.push_back(cp + ".cue: expected a name of 1.." + std::to_string(kMaxArenaSpecialId) +
                             " letters, digits, _ and -");
        else
            spCopy(name.string(), out.name);
    }
}

} // namespace

bool validArenaSpecialRules(const ArenaSpecialRules& r) {
    if (!spId(r.idView()) || !spId(r.leadView())) return false;
    if (!inRange(r.duration, 0.5f, 30.f) || !(r.impactAt > 0.f && r.impactAt <= r.duration)) return false;
    if (!inRange(r.range, 1.f, 200.f) || !inRange(r.laneLength, 0.5f, 100.f) || !inRange(r.laneWidth, 0.5f, 50.f))
        return false;
    if (r.damage < 1 || r.damage > 100000) return false;
    if (!inRange(r.otherShare, 0.f, 1.f) || !inRange(r.bossPhaseShare, 0.f, 1.f)) return false;
    if (!inRange(r.knockBack, 0.f, 20.f) || !inRange(r.knockTime, 0.05f, 5.f)) return false;
    if (r.cueCount > kMaxArenaSpecialCues) return false;
    float last = 0.f;
    for (std::size_t i = 0; i < r.cueCount; ++i) {
        const ArenaSpecialCue& c = r.cues[i];
        if (!inRange(c.at, last, r.duration) || !spId(c.view())) return false;
        last = c.at;
    }
    return true;
}

bool validArenaSpecialGauge(const ArenaSpecialGaugeRules& g) {
    return inRange(g.full, 1.f, 1.0e6f) && inRange(g.perDamageDealt, 0.f, 1000.f) && inRange(g.perParry, 0.f, 1.0e5f) &&
           inRange(g.perBlock, 0.f, 1.0e5f) && inRange(g.perDamageTaken, 0.f, 1000.f);
}

const ArenaSpecialRules* ArenaSpecialTable::find(std::string_view id) const {
    for (std::size_t i = 0; i < count && i < moves.size(); ++i)
        if (moves[i].idView() == id) return &moves[i];
    return nullptr;
}

const ArenaSpecialRules* ArenaSpecialTable::forLead(std::string_view leadId) const {
    if (leadId.empty()) return nullptr;
    for (std::size_t i = 0; i < count && i < moves.size(); ++i)
        if (moves[i].leadView() == leadId) return &moves[i];
    return nullptr;
}

LeadSpecialGauges::Lead* LeadSpecialGauges::entry(std::string_view leadId) {
    if (leadId.empty() || leadId.size() > kMaxArenaSpecialId) return nullptr;
    for (std::size_t i = 0; i < count && i < leads.size(); ++i)
        if (std::string_view(leads[i].id.data()) == leadId) return &leads[i];
    if (count >= leads.size()) return nullptr;
    Lead& l = leads[count++];
    l = Lead{};
    spCopy(leadId, l.id);
    return &l;
}

const LeadSpecialGauges::Lead* LeadSpecialGauges::find(std::string_view leadId) const {
    if (leadId.empty()) return nullptr;
    for (std::size_t i = 0; i < count && i < leads.size(); ++i)
        if (std::string_view(leads[i].id.data()) == leadId) return &leads[i];
    return nullptr;
}

const char* arenaSpecialRefusalName(ArenaSpecialRefusal r) {
    switch (r) {
    case ArenaSpecialRefusal::Ok: return "ok";
    case ArenaSpecialRefusal::NoMove: return "no_move";
    case ArenaSpecialRefusal::NotReady: return "not_ready";
    case ArenaSpecialRefusal::NoTarget: return "no_target";
    case ArenaSpecialRefusal::NoSight: return "no_sight";
    case ArenaSpecialRefusal::Down: return "down";
    case ArenaSpecialRefusal::Running: return "running";
    case ArenaSpecialRefusal::Over: return "over";
    }
    return "?";
}

bool importArenaSpecials(std::string_view json, ArenaSpecialTable& out, std::string* error,
                         std::vector<std::string>* warnings) {
    out = ArenaSpecialTable{};
    std::vector<std::string> errors;
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) {
        errors.push_back("arena special: " + parseError);
    } else if (!doc.root().isObject()) {
        errors.push_back("arena special: expected an object");
    } else {
        const JsonValue root = doc.root();
        if (const JsonValue v = root["version"]; !v.isAbsent() && !(v.isInteger() && v.integer() == 1))
            errors.push_back("version: expected 1");
        spUnknownKeys(root, "", {"version", "gauge", "moves"}, warnings);
        const JsonValue g = root["gauge"];
        if (g.isObject()) {
            spUnknownKeys(g, "gauge", {"full", "perDamageDealt", "perParry", "perBlock", "perDamageTaken"}, warnings);
            spNumber(g, "full", "gauge", 1.f, 1.0e6f, out.gauge.full, errors);
            spNumber(g, "perDamageDealt", "gauge", 0.f, 1000.f, out.gauge.perDamageDealt, errors);
            spNumber(g, "perParry", "gauge", 0.f, 1.0e5f, out.gauge.perParry, errors);
            spNumber(g, "perBlock", "gauge", 0.f, 1.0e5f, out.gauge.perBlock, errors);
            spNumber(g, "perDamageTaken", "gauge", 0.f, 1000.f, out.gauge.perDamageTaken, errors);
        } else if (!g.isAbsent()) {
            errors.push_back("gauge: expected an object");
        }
        const JsonValue moves = root["moves"];
        if (!moves.isArray() || moves.size() > kMaxArenaSpecials) {
            errors.push_back("moves: expected an array of 0.." + std::to_string(kMaxArenaSpecials) + " moves");
        } else {
            for (std::uint32_t i = 0; i < moves.size(); ++i) {
                const std::string path = "moves[" + std::to_string(i) + "]";
                ArenaSpecialRules r;
                spMove(moves[i], path, r, errors, warnings);
                if (!r.idView().empty() && out.find(r.idView())) errors.push_back(path + ".id: duplicate move id");
                if (!r.leadView().empty() && out.forLead(r.leadView()))
                    errors.push_back(path + ".lead: that lead already has a move");
                out.moves[out.count++] = r;
            }
        }
    }
    if (errors.empty()) return true;
    out = ArenaSpecialTable{};
    if (error) {
        error->clear();
        for (const std::string& e : errors) {
            if (!error->empty()) *error += '\n';
            *error += e;
        }
    }
    return false;
}

} // namespace ftcore
