#include "ftcore/ArenaKindCrit.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>

namespace ftcore {

// ---- The run ---------------------------------------------------------------------------------------

void ArenaKindCrit::reset(const ArenaKindCritDef& def) {
    window_.reset(def.weakPoints);
    opensAfter_ = def.opensAfterChars;
    opens_ = def.opens;
    showHitFor_ = def.showHitFor;
    opensAt_ = -1.0e300;
    crits_ = 0;
    lastPoint_ = kMaxWeakPoints;
    lastMultiplier_ = 1.0f;
    lastDamage_ = 0;
    lastCritAt_ = -1.0e300;
}

bool ArenaKindCrit::onAttackStarted(std::uint8_t move, double t, const ArenaEnemyKind& kind) {
    if (!std::isfinite(t) || move >= kind.moveCount || opensAfter().empty()) return false;
    const ArenaEnemyMove& m = kind.moves[move];
    if (m.clip() != opensAfter()) return false;
    opensAt_ = t + static_cast<double>(m.attack.startup) + static_cast<double>(m.attack.active);
    window_.open(opensAt_, opens_);
    return true;
}

ArenaBlowRuling ArenaKindCrit::rule(const ArenaBlow& blow, std::uint8_t slot, Vec2 bossPos, Vec2 bossFacing) {
    ArenaBlowRuling r;
    if (blow.source != kArenaPlayer || blow.target != slot) return r;
    const double t = static_cast<double>(blow.time);
    const CritResult c = window_.judgeLockOn(blow.from, bossPos, bossFacing, t);
    if (!c.crit) return r;
    r.damage = r.poise = c.multiplier;
    ++crits_;
    lastPoint_ = c.point;
    lastMultiplier_ = c.multiplier;
    lastDamage_ = static_cast<int>(std::lround(static_cast<double>(blow.damage) * static_cast<double>(c.multiplier)));
    lastCritAt_ = t;
    return r;
}

bool ArenaKindCrit::opensOn(const ArenaEnemyKind& kind) const {
    if (opensAfter().empty()) return false;
    for (std::size_t i = 0; i < kind.moveCount && i < kind.moves.size(); ++i)
        if (kind.moves[i].clip() == opensAfter()) return true;
    return false;
}

// ---- The loader ------------------------------------------------------------------------------------

namespace {

// 1..maxLen letters, digits, _ and - (arena.json's ids and clip ids).
bool kcId(std::string_view id, std::size_t maxLen) {
    if (id.empty() || id.size() > maxLen) return false;
    for (const char c : id)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    return true;
}

void kcUnknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys,
                   std::vector<std::string>* warnings) {
    if (!warnings) return;
    for (std::uint32_t k = 0; k < j.size(); ++k) {
        const std::string_view key = j.key(k);
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            warnings->push_back((path.empty() ? std::string() : path + ": ") + "unknown key: " + std::string(key));
    }
}

template <std::size_t N>
void kcCopy(std::string_view s, std::array<char, N>& out) {
    out.fill('\0');
    std::copy_n(s.begin(), std::min(s.size(), N - 1), out.begin());
}

void kcEntry(JsonValue j, const std::string& path, ArenaKindCritDef& d, std::vector<std::string>& errors,
             std::vector<std::string>* warnings) {
    if (!j.isObject()) {
        errors.push_back(path + ": expected an object");
        return;
    }
    kcUnknownKeys(j, path, {"opensAfter", "opens", "showHitFor", "weakPoints", "effects"}, warnings);

    const JsonValue clip = j["opensAfter"];
    if (!clip.isString() || !kcId(clip.string(), kMaxArenaClipId))
        errors.push_back(path + ".opensAfter: expected a clip id of 1.." + std::to_string(kMaxArenaClipId) +
                         " letters, digits, _ and -");
    else
        kcCopy(clip.string(), d.opensAfterChars);

    const JsonValue opens = j["opens"];
    const double s = opens.number(-1.0);
    if (!opens.isNumber() || !std::isfinite(s) || s < 0.1 || s > 10.0)
        errors.push_back(path + ".opens: expected a number in 0.1..10");
    else
        d.opens = s;

    const JsonValue hitFor = j["showHitFor"];
    if (!hitFor.isAbsent()) {
        const double h = hitFor.number(-1.0);
        if (!hitFor.isNumber() || !std::isfinite(h) || h < 0.5 || h > 10.0)
            errors.push_back(path + ".showHitFor: expected a number in 0.5..10");
        else
            d.showHitFor = h;
    }

    const JsonValue fx = j["effects"];
    if (fx.isObject()) {
        const std::string fp = path + ".effects";
        kcUnknownKeys(fx, fp, {"socket", "tell", "open", "crit", "hit", "hitFloor", "critFloor"}, warnings);
        const auto name = [&](const char* key, std::array<char, kMaxArenaKindCritId + 1>& outName) {
            const JsonValue v = fx[key];
            if (v.isAbsent()) return;
            if (!v.isString() || !kcId(v.string(), kMaxArenaKindCritId))
                errors.push_back(fp + "." + key + ": expected a name of 1.." + std::to_string(kMaxArenaKindCritId) +
                                 " letters, digits, _ and -");
            else
                kcCopy(v.string(), outName);
        };
        name("socket", d.effects.socketChars);
        name("tell", d.effects.tellChars);
        name("open", d.effects.openChars);
        name("crit", d.effects.critChars);
        name("hit", d.effects.hitChars);
        name("hitFloor", d.effects.hitFloorChars);
        name("critFloor", d.effects.critFloorChars);
    } else if (!fx.isAbsent()) {
        errors.push_back(path + ".effects: expected an object");
    }

    const std::string wp = path + ".weakPoints";
    if (importArenaWeakPoints(j["weakPoints"], wp, d.weakPoints, errors, warnings) && d.weakPoints.points.empty())
        errors.push_back(wp + ".points: expected 1.." + std::to_string(kMaxWeakPoints) + " points");
}

} // namespace

bool importArenaKindCrits(std::string_view json, ArenaKindCritTable& out, std::string* error,
                          std::vector<std::string>* warnings) {
    out = ArenaKindCritTable{};
    std::vector<std::string> errors;
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) {
        errors.push_back("arena crit: " + parseError);
    } else if (!doc.root().isObject()) {
        errors.push_back("arena crit: expected an object");
    } else {
        const JsonValue root = doc.root();
        if (const JsonValue v = root["version"]; !v.isAbsent() && !(v.isInteger() && v.integer() == 1))
            errors.push_back("version: expected 1");
        kcUnknownKeys(root, "", {"version", "kinds"}, warnings);
        const JsonValue kinds = root["kinds"];
        if (!kinds.isObject() || kinds.size() > kMaxArenaKindCrits) {
            errors.push_back("kinds: expected an object of 0.." + std::to_string(kMaxArenaKindCrits) + " kinds");
        } else {
            for (std::uint32_t i = 0; i < kinds.size(); ++i) {
                const std::string_view id = kinds.key(i);
                const std::string path = "kinds." + std::string(id);
                if (!kcId(id, kMaxArenaKindCritId))
                    errors.push_back(path + ": expected a kind id of 1.." + std::to_string(kMaxArenaKindCritId) +
                                     " letters, digits, _ and -");
                if (out.find(id)) errors.push_back(path + ": duplicate kind id");
                ArenaKindCritDef& d = out.kinds[out.count++];
                kcCopy(id, d.kindChars);
                kcEntry(kinds.value(i), path, d, errors, warnings);
            }
        }
    }
    if (errors.empty()) return true;
    out = ArenaKindCritTable{};
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
