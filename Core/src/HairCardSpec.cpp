#include "ftcore/HairCardSpec.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <utility>

namespace ftcore {

namespace {

void hairCardUnknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys,
                         std::vector<std::string>* warnings) {
    if (!warnings) return;
    for (std::uint32_t k = 0; k < j.size(); ++k) {
        const std::string_view key = j.key(k);
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            warnings->push_back((path.empty() ? std::string() : path + ": ") + "unknown key: " + std::string(key));
    }
}

// Lower-case letters, digits and '_'; not empty.
bool hairCardName(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    for (const char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

// "/Game/", then letters, digits, '_' and '-' between single slashes, with no slash at the end.
bool hairCardAssetPath(std::string_view s) {
    constexpr std::string_view kRoot = "/Game/";
    if (s.size() <= kRoot.size() || s.size() > 255 || s.substr(0, kRoot.size()) != kRoot) return false;
    char prev = '/';
    for (const char c : s.substr(kRoot.size())) {
        const bool word =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!word && !(c == '/' && prev != '/')) return false;
        prev = c;
    }
    return prev != '/';
}

// An integer in lo..hi into `out`; an optional one that's absent leaves `out` as it is. False, with an error, when
// it's anything else.
bool hairCardInt(JsonValue v, const std::string& path, int lo, int hi, bool required, int& out,
                 std::vector<std::string>& errors) {
    if (v.isAbsent() && !required) return true;
    const std::int64_t n = v.integer(-1);
    if (!v.isInteger() || n < lo || n > hi) {
        errors.push_back(path + ": expected an integer in " + std::to_string(lo) + ".." + std::to_string(hi));
        return false;
    }
    out = static_cast<int>(n);
    return true;
}

} // namespace

int HairCardSpec::find(std::string_view id) const {
    for (std::size_t i = 0; i < grooms.size(); ++i)
        if (grooms[i].id == id) return static_cast<int>(i);
    return -1;
}

int hairCardAtlasLog2(int atlasPixels) {
    for (int p = 10; p <= 13; ++p)
        if (atlasPixels == (1 << p)) return p;
    return -1;
}

bool importHairCardSpec(std::string_view json, HairCardSpec& out, std::string* error, std::vector<std::string>* warnings) {
    out = HairCardSpec{};
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
        errors.push_back("hair card file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("hair card file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !v.isAbsent() && !(v.isInteger() && v.integer() == 1))
        errors.push_back("version: expected 1");
    hairCardUnknownKeys(root, "", {"version", "grooms"}, warnings);

    HairCardSpec spec;
    const JsonValue list = root["grooms"];
    if (!list.isArray() || list.size() == 0) errors.push_back("grooms: expected a non-empty array");
    for (std::uint32_t i = 0; i < list.size() && list.isArray(); ++i) {
        const JsonValue j = list[i];
        const std::string path = "grooms[" + std::to_string(i) + "]";
        if (!j.isObject()) {
            errors.push_back(path + ": expected an object");
            continue;
        }
        hairCardUnknownKeys(j, path, {"id", "groom", "out", "group", "seed", "lods"}, warnings);
        HairCardGroom g;
        g.id = std::string(j["id"].string());
        if (!hairCardName(g.id)) errors.push_back(path + ".id: expected lower-case letters, digits and _");
        else if (spec.find(g.id) >= 0) errors.push_back(path + ".id: duplicate groom " + g.id);
        g.groom = std::string(j["groom"].string());
        if (!hairCardAssetPath(g.groom)) errors.push_back(path + ".groom: expected a package path under /Game/");
        else if (std::any_of(spec.grooms.begin(), spec.grooms.end(),
                             [&g](const HairCardGroom& o) { return o.groom == g.groom; }))
            errors.push_back(path + ".groom: listed twice");
        g.out = std::string(j["out"].string());
        if (!hairCardAssetPath(g.out)) errors.push_back(path + ".out: expected a folder under /Game/");
        hairCardInt(j["group"], path + ".group", 0, HairCardGroom::kMaxGroup, false, g.group, errors);
        hairCardInt(j["seed"], path + ".seed", 0, HairCardGroom::kMaxSeed, false, g.seed, errors);

        const JsonValue lods = j["lods"];
        if (!lods.isArray() || lods.size() == 0 || lods.size() > HairCardGroom::kMaxLods)
            errors.push_back(path + ".lods: expected an array of 1.." + std::to_string(HairCardGroom::kMaxLods) + " levels");
        for (std::uint32_t l = 0; l < lods.size() && lods.isArray(); ++l) {
            const JsonValue lj = lods[l];
            const std::string lpath = path + ".lods[" + std::to_string(l) + "]";
            if (!lj.isObject()) {
                errors.push_back(lpath + ": expected an object");
                continue;
            }
            hairCardUnknownKeys(lj, lpath,
                                {"lod", "cards", "triangles", "textures", "flyaways", "atlas", "screenSize", "useCards"},
                                warnings);
            HairCardLod lod;
            const bool lodOk = hairCardInt(lj["lod"], lpath + ".lod", 0, HairCardLod::kMaxLod, true, lod.lod, errors);
            const bool cardsOk =
                hairCardInt(lj["cards"], lpath + ".cards", 1, HairCardLod::kMaxCards, true, lod.cards, errors);
            const bool trianglesOk = hairCardInt(lj["triangles"], lpath + ".triangles", HairCardLod::kMinTriangles,
                                                 HairCardLod::kMaxTriangles, true, lod.triangles, errors);
            hairCardInt(lj["textures"], lpath + ".textures", 1, HairCardLod::kMaxTextures, true, lod.textures, errors);
            hairCardInt(lj["flyaways"], lpath + ".flyaways", 0, HairCardLod::kMaxFlyaways, false, lod.flyaways, errors);
            if (cardsOk && trianglesOk && lod.triangles < 2 * lod.cards)
                errors.push_back(lpath + ".triangles: expected at least two per card");
            const JsonValue atlas = lj["atlas"];
            const std::int64_t px = atlas.integer(0);
            if (!atlas.isInteger() || px < 1024 || px > 8192 || hairCardAtlasLog2(static_cast<int>(px)) < 0)
                errors.push_back(lpath + ".atlas: expected 1024, 2048, 4096 or 8192");
            else
                lod.atlas = static_cast<int>(px);
            if (const JsonValue size = lj["screenSize"]; !size.isAbsent()) {
                const double s = size.number(-1.0);
                if (!size.isNumber() || !std::isfinite(s) || s <= 0.0 || s > 1.0)
                    errors.push_back(lpath + ".screenSize: expected a number in (0, 1]");
                else
                    lod.screenSize = static_cast<float>(s);
            }
            if (const JsonValue use = lj["useCards"]; !use.isAbsent()) {
                if (!use.isBool()) errors.push_back(lpath + ".useCards: expected true or false");
                else lod.useCards = use.boolean(true);
            }

            // Against the levels before it. A value that failed above is left at 0 (or the screen size at -1),
            // which none of these compare.
            if (!g.lods.empty()) {
                const HairCardLod& prev = g.lods.back();
                if (lodOk && lod.lod <= prev.lod) errors.push_back(lpath + ".lod: expected levels in increasing order");
                const auto grows = [&](int was, int now, const char* key) {
                    if (was > 0 && now > was)
                        errors.push_back(lpath + "." + key + ": expected no more than the level before's " +
                                         std::to_string(was));
                };
                grows(prev.cards, lod.cards, "cards");
                grows(prev.triangles, lod.triangles, "triangles");
                grows(prev.textures, lod.textures, "textures");
                grows(prev.atlas, lod.atlas, "atlas");
                if (lod.screenSize > 0.f) {
                    for (auto it = g.lods.rbegin(); it != g.lods.rend(); ++it) {
                        if (it->screenSize <= 0.f) continue;
                        if (lod.screenSize >= it->screenSize)
                            errors.push_back(lpath + ".screenSize: expected less than the level before's");
                        break;
                    }
                }
            }
            g.lods.push_back(lod);
        }
        spec.grooms.push_back(std::move(g));
    }

    if (!errors.empty()) return finish();
    out = std::move(spec);
    return true;
}

} // namespace ftcore
