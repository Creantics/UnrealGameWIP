#include "ftcore/SurfaceTable.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace ftcore {

namespace {

constexpr std::array<std::string_view, kSurfaceRoleCount> kRoleNames{"ground",   "road",     "plaza", "building",
                                                                     "roof",     "landmark", "wood",  "rock"};
constexpr std::array<std::string_view, kWorldKindCount> kKindNames{"town", "towers", "ruins"};

void surfaceUnknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys,
                        std::vector<std::string>* warnings) {
    if (!warnings) return;
    for (std::uint32_t k = 0; k < j.size(); ++k) {
        const std::string_view key = j.key(k);
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            warnings->push_back((path.empty() ? std::string() : path + ": ") + "unknown key: " + std::string(key));
    }
}

// Lower-case letters, digits and '_'; not empty.
bool surfaceName(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    for (const char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

std::size_t kindIndex(WorldKind kind) { return static_cast<std::size_t>(kind); }
std::size_t roleIndex(SurfaceRole role) { return static_cast<std::size_t>(role); }

} // namespace

std::string_view surfaceRoleName(SurfaceRole role) {
    const std::size_t i = roleIndex(role);
    return i < kSurfaceRoleCount ? kRoleNames[i] : std::string_view("?");
}

bool parseSurfaceRole(std::string_view name, SurfaceRole& out) {
    for (std::size_t i = 0; i < kSurfaceRoleCount; ++i) {
        if (kRoleNames[i] == name) {
            out = static_cast<SurfaceRole>(i);
            return true;
        }
    }
    return false;
}

int SurfaceTable::find(std::string_view id) const {
    for (std::size_t i = 0; i < surfaces.size(); ++i)
        if (surfaces[i].id == id) return static_cast<int>(i);
    return -1;
}

const SurfaceUse& SurfaceTable::use(WorldKind kind, SurfaceRole role) const {
    static const SurfaceUse kNone{};
    const std::size_t k = kindIndex(kind), r = roleIndex(role);
    return k < kWorldKindCount && r < kSurfaceRoleCount ? uses[k][r] : kNone;
}

const SurfaceSpec* SurfaceTable::surfaceFor(WorldKind kind, SurfaceRole role) const {
    const int s = use(kind, role).surface;
    return s >= 0 && static_cast<std::size_t>(s) < surfaces.size() ? &surfaces[static_cast<std::size_t>(s)] : nullptr;
}

bool importSurfaceTable(std::string_view json, SurfaceTable& out, std::string* error, std::vector<std::string>* warnings) {
    out = SurfaceTable{};
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

    JsonDocument doc;
    std::string err;
    if (!doc.parse(json, &err)) {
        errors.push_back("surface file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("surface file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !v.isAbsent() && !(v.isInteger() && v.integer() == 1))
        errors.push_back("version: expected 1");
    surfaceUnknownKeys(root, "", {"version", "surfaces", "kinds"}, warnings);

    SurfaceTable table;
    const JsonValue list = root["surfaces"];
    if (!list.isArray() || list.size() == 0) errors.push_back("surfaces: expected a non-empty array");
    for (std::uint32_t i = 0; i < list.size() && list.isArray(); ++i) {
        const JsonValue j = list[i];
        const std::string path = "surfaces[" + std::to_string(i) + "]";
        if (!j.isObject()) {
            errors.push_back(path + ": expected an object");
            continue;
        }
        surfaceUnknownKeys(j, path, {"id", "tileCm", "res", "source", "license", "roughness"}, warnings);
        SurfaceSpec s;
        s.id = std::string(j["id"].string());
        if (!surfaceName(s.id)) errors.push_back(path + ".id: expected lower-case letters, digits and _");
        else if (table.find(s.id) >= 0) errors.push_back(path + ".id: duplicate surface " + s.id);
        const JsonValue tile = j["tileCm"];
        const double cm = tile.number(-1.0);
        if (!tile.isNumber() || !std::isfinite(cm) || cm < SurfaceSpec::kMinTileCm || cm > SurfaceSpec::kMaxTileCm)
            errors.push_back(path + ".tileCm: expected a number in " + std::to_string(static_cast<int>(SurfaceSpec::kMinTileCm)) +
                             ".." + std::to_string(static_cast<int>(SurfaceSpec::kMaxTileCm)));
        else
            s.tileCm = static_cast<float>(cm);
        if (const JsonValue res = j["res"]; !res.isAbsent()) {
            const std::string_view r = res.string();
            if (r == "1k") s.resK = 1;
            else if (r == "2k") s.resK = 2;
            else if (r == "4k") s.resK = 4;
            else errors.push_back(path + ".res: expected 1k, 2k or 4k");
        }
        s.source = std::string(j["source"].string());
        if (s.source.rfind("https://", 0) != 0 || s.source.size() <= 8)
            errors.push_back(path + ".source: expected an https page");
        if (j["license"].string() != "CC0") errors.push_back(path + ".license: expected CC0");
        if (const JsonValue rough = j["roughness"]; !rough.isAbsent()) {
            const double r = rough.number(-1.0);
            if (!rough.isNumber() || !std::isfinite(r) || r < 0.0 || r > 1.0)
                errors.push_back(path + ".roughness: expected a number in 0..1");
            else
                s.roughness = static_cast<float>(r);
        }
        table.surfaces.push_back(std::move(s));
    }

    const JsonValue kinds = root["kinds"];
    if (!kinds.isAbsent() && !kinds.isObject()) errors.push_back("kinds: expected an object");
    for (std::uint32_t k = 0; k < kinds.size() && kinds.isObject(); ++k) {
        const std::string_view kindName = kinds.key(k);
        const std::string kindPath = "kinds." + std::string(kindName);
        const auto kindAt = std::find(kKindNames.begin(), kKindNames.end(), kindName);
        if (kindAt == kKindNames.end()) {
            errors.push_back(kindPath + ": expected town, towers or ruins");
            continue;
        }
        const JsonValue roles = kinds.value(k);
        if (!roles.isObject()) {
            errors.push_back(kindPath + ": expected an object");
            continue;
        }
        std::array<SurfaceUse, kSurfaceRoleCount>& uses =
            table.uses[static_cast<std::size_t>(kindAt - kKindNames.begin())];
        for (std::uint32_t r = 0; r < roles.size(); ++r) {
            const std::string path = kindPath + "." + std::string(roles.key(r));
            SurfaceRole role{};
            if (!parseSurfaceRole(roles.key(r), role)) {
                errors.push_back(path + ": expected ground, road, plaza, building, roof, landmark, wood or rock");
                continue;
            }
            const JsonValue j = roles.value(r);
            if (!j.isObject()) {
                errors.push_back(path + ": expected an object");
                continue;
            }
            surfaceUnknownKeys(j, path, {"surface", "tint"}, warnings);
            SurfaceUse use;
            use.surface = table.find(j["surface"].string());
            if (use.surface < 0) errors.push_back(path + ".surface: expected a surface id from surfaces");
            if (const JsonValue tint = j["tint"]; !tint.isAbsent()) {
                bool ok = tint.isArray() && tint.size() == 3;
                for (std::uint32_t c = 0; ok && c < 3; ++c) {
                    const double v = tint[c].number(-1.0);
                    ok = tint[c].isNumber() && std::isfinite(v) && v >= 0.0 && v <= 2.0;
                    if (ok) use.tint[c] = static_cast<float>(v);
                }
                if (!ok) errors.push_back(path + ".tint: expected three numbers in 0..2");
            }
            uses[roleIndex(role)] = use;
        }
    }

    if (!errors.empty()) return finish();
    out = std::move(table);
    return true;
}

} // namespace ftcore
