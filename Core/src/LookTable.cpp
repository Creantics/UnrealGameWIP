#include "ftcore/LookTable.h"

#include "ftcore/Json.h"

#include <cmath>
#include <cstdio>
#include <utility>

namespace ftcore {

namespace {

char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
    return true;
}

bool containsNoCase(std::string_view text, std::string_view word) {
    if (word.empty() || text.size() < word.size()) return false;
    for (std::size_t i = 0; i + word.size() <= text.size(); ++i)
        if (equalsNoCase(text.substr(i, word.size()), word)) return true;
    return false;
}

// Lower-case letters, digits and '_'; not empty.
bool worldName(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    for (const char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

// "r." then letters, digits, '.' and '_'; 3..128 characters.
bool cvarName(std::string_view s) {
    if (s.size() < 3 || s.size() > 128 || s.substr(0, 2) != "r.") return false;
    for (const char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_'))
            return false;
    return true;
}

std::string shortFloat(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
    return buf;
}

} // namespace

const SceneLook* LookTable::find(std::string_view id) const {
    for (const Entry& e : worlds)
        if (equalsNoCase(e.id, id)) return &e.look;
    return nullptr;
}

const LookTable::DuskRow* LookTable::findDusk(std::string_view world) const {
    for (const DuskRow& d : dusk)
        if (equalsNoCase(d.world, world)) return &d;
    return nullptr;
}

float LookTable::glassValue(std::string_view name, float fallback) const {
    for (const GlassValue& g : glass)
        if (g.name == name) return g.value;
    return fallback;
}

const LookTable::RenderTier* LookTable::findTier(std::string_view name) const {
    for (const RenderTier& t : renderTiers)
        if (equalsNoCase(t.name, name)) return &t;
    return nullptr;
}

std::string_view lookWorldForMap(std::string_view mapName) {
    for (const std::string_view world : {std::string_view("solhaven"), std::string_view("lattice"),
                                         std::string_view("unwritten")})
        if (containsNoCase(mapName, world)) return world;
    return {};
}

bool importLookTable(std::string_view json, LookTable& out, std::string* error, std::vector<std::string>* warnings) {
    out = LookTable{};
    std::vector<std::string> errors;
    const auto finish = [&]() {
        if (errors.empty()) return true;
        if (error) {
            error->clear();
            for (const std::string& e : errors) {
                if (!error->empty()) *error += '\n';
                *error += e;
            }
        }
        return false;
    };
    const auto warn = [&](std::string message) {
        if (warnings) warnings->push_back(std::move(message));
    };

    JsonDocument doc;
    std::string err;
    if (!doc.parse(json, &err)) {
        errors.push_back("look file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("look file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !(v.isInteger() && v.integer() == 1))
        errors.push_back("version: expected 1");
    for (std::uint32_t k = 0; k < root.size(); ++k) {
        const std::string_view key = root.key(k);
        if (key != "version" && key != "worlds" && key != "render" && key != "renderTiers" && key != "dusk" &&
            key != "glass")
            warn("unknown key: " + std::string(key));
    }

    LookTable table;
    // An object of console variables (name -> number) at `path` into `out`, in file order.
    const auto cvars = [&](const JsonValue& obj, const std::string& path, std::vector<LookTable::RenderSetting>& out) {
        if (!obj.isObject()) {
            errors.push_back(path + ": expected an object");
            return;
        }
        for (std::uint32_t k = 0; k < obj.size(); ++k) {
            const std::string name(obj.key(k));
            const std::string at = path + "." + name;
            if (!cvarName(name)) {
                errors.push_back(at + ": expected a console variable name: r. then letters, digits, . and _ (3..128 chars)");
                continue;
            }
            bool duplicate = false;
            for (const LookTable::RenderSetting& r : out)
                if (equalsNoCase(r.name, name)) duplicate = true;
            if (duplicate) {
                errors.push_back(at + ": duplicate console variable");
                continue;
            }
            const JsonValue v = obj.value(k);
            const double d = v.number(0.0);
            if (!v.isNumber() || !std::isfinite(d) || d > 3e38 || d < -3e38) {
                errors.push_back(at + ": " + std::string(v.raw()) + " is not a number a float holds");
                continue;
            }
            out.push_back({name, static_cast<float>(d)});
        }
    };
    if (root.has("render")) cvars(root["render"], "render", table.render);
    if (root.has("renderTiers")) {
        const JsonValue tiers = root["renderTiers"];
        if (!tiers.isObject()) errors.push_back("renderTiers: expected an object");
        for (std::uint32_t k = 0; k < tiers.size() && tiers.isObject(); ++k) {
            const std::string name(tiers.key(k));
            const JsonValue v = tiers.value(k);
            if (name == "about") continue;
            if (name == "play" || name == "cutscene") {
                if (!v.isString() || v.string().empty()) {
                    errors.push_back("renderTiers." + name + ": expected a tier name");
                    continue;
                }
                (name == "play" ? table.playTier : table.cutsceneTier) = std::string(v.string());
                continue;
            }
            if (!worldName(name) || table.findTier(name)) {
                errors.push_back("renderTiers." + name + ": expected one tier name of lower-case letters, digits and _");
                continue;
            }
            LookTable::RenderTier tier;
            tier.name = name;
            cvars(v, "renderTiers." + name, tier.settings);
            table.renderTiers.push_back(std::move(tier));
        }
        if (tiers.isObject()) {
            if (table.renderTiers.empty()) errors.push_back("renderTiers: expected at least one tier");
            for (const std::string* pick : {&table.playTier, &table.cutsceneTier})
                if (!table.renderTiers.empty() && !table.findTier(*pick))
                    errors.push_back("renderTiers." + std::string(pick == &table.playTier ? "play" : "cutscene") +
                                     ": '" + *pick + "' is not a tier");
            // Every tier sets the same console variables, so switching tiers leaves none of another tier's behind.
            for (std::size_t t = 1; t < table.renderTiers.size(); ++t) {
                const auto& a = table.renderTiers[0].settings;
                const auto& b = table.renderTiers[t].settings;
                bool same = a.size() == b.size();
                for (const LookTable::RenderSetting& s : a) {
                    bool found = false;
                    for (const LookTable::RenderSetting& r : b) found = found || equalsNoCase(r.name, s.name);
                    same = same && found;
                }
                if (!same)
                    errors.push_back("renderTiers." + table.renderTiers[t].name + ": expected the same console variables as " +
                                     table.renderTiers[0].name);
            }
        }
    }
    // A number at `at` within lo..hi into `value`; false (and an error) when it isn't one.
    const auto number = [&](const JsonValue& v, const std::string& at, float lo, float hi, float& value) {
        const double d = v.number(0.0);
        const std::string range = shortFloat(lo) + ".." + shortFloat(hi);
        if (!v.isNumber() || !std::isfinite(d)) {
            errors.push_back(at + ": " + std::string(v.raw()) + " is not a number (expected " + range + ")");
            return false;
        }
        const float f = static_cast<float>(d);
        if (!(f >= lo && f <= hi)) {
            errors.push_back(at + ": " + std::string(v.raw()) + " is out of range (expected " + range + ")");
            return false;
        }
        value = f;
        return true;
    };
    if (root.has("dusk")) {
        const JsonValue dusk = root["dusk"];
        if (!dusk.isObject()) errors.push_back("dusk: expected an object");
        for (std::uint32_t w = 0; w < dusk.size() && dusk.isObject(); ++w) {
            LookTable::DuskRow row;
            row.world = std::string(dusk.key(w));
            const std::string path = "dusk." + row.world;
            const JsonValue j = dusk.value(w);
            if (!worldName(row.world) || !j.isObject() || table.findDusk(row.world)) {
                errors.push_back(path + ": expected one object a world id (lower-case letters, digits and _)");
                continue;
            }
            DuskLook& d = row.look;
            struct Field {
                const char* name;
                float* value;
                float lo, hi;
            };
            const Field fields[] = {
                {"stillDeg", &d.stillDeg, -10.f, 30.f},       {"startDeg", &d.startDeg, -10.f, 45.f},
                {"peakDeg", &d.peakDeg, -15.f, 30.f},         {"endDeg", &d.endDeg, -30.f, 10.f},
                {"morningShare", &d.morningShare, 0.f, 1.f},  {"sunTintR", &d.sunTint[0], 0.f, 4.f},
                {"sunTintG", &d.sunTint[1], 0.f, 4.f},        {"sunTintB", &d.sunTint[2], 0.f, 4.f},
                {"skyGlowR", &d.skyGlow[0], 0.f, 4.f},        {"skyGlowG", &d.skyGlow[1], 0.f, 4.f},
                {"skyGlowB", &d.skyGlow[2], 0.f, 4.f},        {"fillTintR", &d.fillTint[0], 0.f, 4.f},
                {"fillTintG", &d.fillTint[1], 0.f, 4.f},      {"fillTintB", &d.fillTint[2], 0.f, 4.f},
                {"fillScale", &d.fillScale, 0.f, 4.f},        {"fogTintR", &d.fogTint[0], 0.f, 4.f},
                {"fogTintG", &d.fogTint[1], 0.f, 4.f},        {"fogTintB", &d.fogTint[2], 0.f, 4.f},
                {"flare", &d.flare, 0.f, 10.f},               {"flareThreshold", &d.flareThreshold, 0.1f, 32.f},
                {"bloomAdd", &d.bloomAdd, 0.f, 4.f},          {"exposureBias", &d.exposureBias, -6.f, 6.f},
                {"lightsOnDeg", &d.lightsOnDeg, -90.f, 45.f}, {"lightsFullDeg", &d.lightsFullDeg, -90.f, 45.f},
                {"dayEndDeg", &d.dayEndDeg, -30.f, 30.f},     {"dayFullDeg", &d.dayFullDeg, -30.f, 60.f},
            };
            for (std::uint32_t k = 0; k < j.size(); ++k) {
                const std::string_view key = j.key(k);
                const JsonValue v = j.value(k);
                if (key == "about") continue;
                if (key == "status") {
                    row.status = std::string(v.string());
                    continue;
                }
                const Field* field = nullptr;
                for (const Field& f : fields)
                    if (key == f.name) field = &f;
                if (!field) {
                    warn(path + ": unknown key: " + std::string(key));
                    continue;
                }
                number(v, path + "." + std::string(key), field->lo, field->hi, *field->value);
            }
            if (!(d.startDeg > d.peakDeg && d.peakDeg > d.endDeg))
                errors.push_back(path + ": expected startDeg > peakDeg > endDeg");
            table.dusk.push_back(std::move(row));
        }
    }
    if (root.has("glass")) {
        const JsonValue glass = root["glass"];
        if (!glass.isObject()) errors.push_back("glass: expected an object");
        for (std::uint32_t k = 0; k < glass.size() && glass.isObject(); ++k) {
            const std::string name(glass.key(k));
            const JsonValue v = glass.value(k);
            if (name == "about") continue;
            if (name == "status") {
                table.glassStatus = std::string(v.string());
                continue;
            }
            bool good = name.size() >= 2 && name.size() <= 64;
            for (const char ch : name)
                good = good && ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'));
            bool duplicate = false;
            for (const LookTable::GlassValue& g : table.glass) duplicate = duplicate || g.name == name;
            if (!good || duplicate) {
                errors.push_back("glass." + name + ": expected a name of letters and digits, once");
                continue;
            }
            float value = 0.f;
            if (number(v, "glass." + name, -1e6f, 1e6f, value)) table.glass.push_back({name, value});
        }
    }
    const JsonValue worlds = root["worlds"];
    if (!worlds.isObject() || worlds.size() == 0) errors.push_back("worlds: expected a non-empty object");
    for (std::uint32_t w = 0; w < worlds.size() && worlds.isObject(); ++w) {
        const std::string id(worlds.key(w));
        const std::string path = "worlds." + id;
        const JsonValue j = worlds.value(w);
        if (!worldName(id)) {
            errors.push_back(path + ": expected a world id of lower-case letters, digits and _");
            continue;
        }
        if (table.find(id)) {
            errors.push_back(path + ": duplicate world");
            continue;
        }
        if (!j.isObject()) {
            errors.push_back(path + ": expected an object");
            continue;
        }
        LookTable::Entry entry;
        entry.id = id;
        for (std::uint32_t k = 0; k < j.size(); ++k) {
            const std::string_view key = j.key(k);
            if (key == "about") continue;
            const SceneLookField* field = nullptr;
            for (const SceneLookField& f : kSceneLookFields)
                if (key == f.name) field = &f;
            if (!field) {
                warn(path + ": unknown key: " + std::string(key));
                continue;
            }
            const JsonValue v = j.value(k);
            const std::string at = path + "." + std::string(key);
            const double d = v.number(0.0);
            const std::string range = shortFloat(field->lo) + ".." + shortFloat(field->hi);
            if (!v.isNumber() || !std::isfinite(d)) {
                errors.push_back(at + ": " + std::string(v.raw()) + " is not a number (expected " + range + ")");
                continue;
            }
            const float f = static_cast<float>(d);
            if (!(f >= field->lo && f <= field->hi)) {
                errors.push_back(at + ": " + std::string(v.raw()) + " is out of range (expected " + range + ")");
                continue;
            }
            entry.look.*(field->member) = f;
        }
        table.worlds.push_back(std::move(entry));
    }

    if (!errors.empty()) return finish();
    out = std::move(table);
    return true;
}

} // namespace ftcore
