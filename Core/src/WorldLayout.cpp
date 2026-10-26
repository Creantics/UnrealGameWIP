#include "ftcore/WorldLayout.h"

#include "SeedMix.h"
#include "ftcore/Json.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace ftcore {

namespace {

constexpr float kWorldPi = 3.14159265358979f;

void worldUnknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys,
                      std::vector<std::string>* warnings) {
    if (!warnings) return;
    for (std::uint32_t k = 0; k < j.size(); ++k) {
        const std::string_view key = j.key(k);
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            warnings->push_back((path.empty() ? std::string() : path + ": ") + "unknown key: " + std::string(key));
    }
}

// Letters, digits and '_' only (lower-case letters only when `lower`); not empty.
bool worldName(std::string_view s, bool lower) {
    if (s.empty() || s.size() > 64) return false;
    for (const char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || (!lower && c >= 'A' && c <= 'Z');
        if (!ok) return false;
    }
    return true;
}

struct WorldRng {
    std::uint32_t s;
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float unit() { return static_cast<float>(next() >> 8) * (1.f / 16777216.f); } // [0, 1)
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
};

float worldDist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

float segmentDistance(Vec2 p, Vec2 a, Vec2 b) {
    const float abx = b.x - a.x, aby = b.y - a.y;
    const float len2 = abx * abx + aby * aby;
    float t = len2 > 0.f ? ((p.x - a.x) * abx + (p.y - a.y) * aby) / len2 : 0.f;
    t = std::clamp(t, 0.f, 1.f);
    return std::hypot(p.x - (a.x + abx * t), p.y - (a.y + aby * t));
}

float cross2(Vec2 o, Vec2 a, Vec2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

// Proper crossing of two segments (sharing an end point doesn't count).
bool segmentsCross(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
    const float d1 = cross2(c, d, a), d2 = cross2(c, d, b), d3 = cross2(a, b, c), d4 = cross2(a, b, d);
    return ((d1 > 0.f) != (d2 > 0.f)) && ((d3 > 0.f) != (d4 > 0.f)) && d1 != 0.f && d2 != 0.f && d3 != 0.f &&
           d4 != 0.f;
}

} // namespace

const WorldScene* WorldTable::findScene(std::string_view scene) const {
    for (const WorldScene& s : scenes)
        if (s.scene == scene) return &s;
    return nullptr;
}

int WorldTable::findWorld(std::string_view idOrMap) const {
    for (std::size_t i = 0; i < worlds.size(); ++i)
        if (worlds[i].id == idOrMap || worlds[i].map == idOrMap) return static_cast<int>(i);
    return -1;
}

bool importWorldTable(std::string_view json, WorldTable& out, std::string* error, std::vector<std::string>* warnings) {
    out = WorldTable{};
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
        errors.push_back("world file: " + err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        errors.push_back("world file: expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !v.isAbsent() && !(v.isInteger() && v.integer() == 1))
        errors.push_back("version: expected 1");
    worldUnknownKeys(root, "", {"version", "worlds", "scenes"}, warnings);

    WorldTable table;
    const JsonValue list = root["worlds"];
    if (!list.isArray() || list.size() == 0) errors.push_back("worlds: expected a non-empty array");
    for (std::uint32_t i = 0; i < list.size() && list.isArray(); ++i) {
        const JsonValue j = list[i];
        const std::string path = "worlds[" + std::to_string(i) + "]";
        if (!j.isObject()) {
            errors.push_back(path + ": expected an object");
            continue;
        }
        worldUnknownKeys(j, path, {"id", "map", "style", "kind", "side", "seed", "districts", "story"}, warnings);
        WorldSpec w;
        w.id = std::string(j["id"].string());
        w.map = std::string(j["map"].string());
        w.style = std::string(j["style"].string());
        if (!worldName(w.id, true)) errors.push_back(path + ".id: expected lower-case letters, digits and _");
        else if (table.findWorld(w.id) >= 0) errors.push_back(path + ".id: duplicate world " + w.id);
        if (!worldName(w.map, false)) errors.push_back(path + ".map: expected letters, digits and _");
        else if (table.findWorld(w.map) >= 0) errors.push_back(path + ".map: duplicate map " + w.map);
        if (!worldName(w.style, false)) errors.push_back(path + ".style: expected letters, digits and _");
        const JsonValue kind = j["kind"];
        if (!kind.isAbsent()) {
            const std::string_view k = kind.string();
            if (k == "town") w.kind = WorldKind::Town;
            else if (k == "towers") w.kind = WorldKind::Towers;
            else if (k == "ruins") w.kind = WorldKind::Ruins;
            else errors.push_back(path + ".kind: expected town, towers or ruins");
        }
        const JsonValue side = j["side"];
        const double sd = side.number(-1.0);
        if (!side.isNumber() || !std::isfinite(sd) || sd < WorldSpec::kMinSide || sd > WorldSpec::kMaxSide)
            errors.push_back(path + ".side: expected a number in " + std::to_string(static_cast<int>(WorldSpec::kMinSide)) +
                             ".." + std::to_string(static_cast<int>(WorldSpec::kMaxSide)));
        else
            w.side = static_cast<float>(sd);
        const JsonValue seed = j["seed"];
        if (!seed.isAbsent() && (!seed.isInteger() || seed.integer() < 0 || seed.integer() > 0xffffffffll))
            errors.push_back(path + ".seed: expected an integer in 0..4294967295");
        else
            w.seed = static_cast<std::uint32_t>(seed.integer(1));
        const JsonValue count = j["districts"];
        if (!count.isInteger() || count.integer() < 1 ||
            count.integer() > static_cast<std::int64_t>(WorldLayout::kMaxDistricts))
            errors.push_back(path + ".districts: expected an integer in 1.." +
                             std::to_string(WorldLayout::kMaxDistricts));
        else
            w.districts = static_cast<std::uint32_t>(count.integer());
        const JsonValue story = j["story"];
        if (!story.isAbsent() && !story.isArray()) errors.push_back(path + ".story: expected an array of names");
        for (std::uint32_t k = 0; k < story.size() && story.isArray(); ++k) {
            const std::string_view name = story[k].string();
            if (name.empty()) errors.push_back(path + ".story[" + std::to_string(k) + "]: expected a name");
            else w.story.emplace_back(name);
        }
        table.worlds.push_back(std::move(w));
    }

    const JsonValue scenes = root["scenes"];
    if (!scenes.isAbsent() && !scenes.isArray()) errors.push_back("scenes: expected an array");
    for (std::uint32_t i = 0; i < scenes.size() && scenes.isArray(); ++i) {
        const JsonValue j = scenes[i];
        const std::string path = "scenes[" + std::to_string(i) + "]";
        if (!j.isObject()) {
            errors.push_back(path + ": expected an object");
            continue;
        }
        worldUnknownKeys(j, path, {"scene", "world", "district", "style"}, warnings);
        WorldScene s;
        s.scene = std::string(j["scene"].string());
        if (s.scene.empty()) errors.push_back(path + ".scene: expected a scene id");
        else if (table.findScene(s.scene)) errors.push_back(path + ": duplicate scene " + s.scene);
        const int world = table.findWorld(j["world"].string());
        if (world < 0) errors.push_back(path + ".world: expected a world id from worlds");
        else s.world = static_cast<std::uint32_t>(world);
        const JsonValue d = j["district"];
        const std::int64_t districts = world < 0 ? static_cast<std::int64_t>(WorldLayout::kMaxDistricts)
                                                 : static_cast<std::int64_t>(table.worlds[world].districts);
        if (!d.isInteger() || d.integer() < 0 || d.integer() >= districts)
            errors.push_back(path + ".district: expected an integer in 0.." + std::to_string(districts - 1));
        else
            s.district = static_cast<std::uint32_t>(d.integer());
        if (j.has("style")) {
            s.style = std::string(j["style"].string());
            if (!worldName(s.style, false)) errors.push_back(path + ".style: expected letters, digits and _");
        }
        table.scenes.push_back(std::move(s));
    }
    if (!finish()) return false;
    out = std::move(table);
    return true;
}

float WorldLayout::spawnYaw(std::size_t district) const {
    const WorldDistrict& d = districts_[district];
    return std::atan2(d.forward.y, d.forward.x);
}

float WorldLayout::roadDistance(Vec2 p) const {
    float best = 1e30f;
    for (std::size_t i = 0; i < roadCount_; ++i)
        best = std::min(best, segmentDistance(p, districts_[roads_[i].a].centre, districts_[roads_[i].b].centre));
    return best;
}

int WorldLayout::districtAt(Vec2 p) const {
    for (std::size_t i = 0; i < districtCount_; ++i)
        if (worldDist(p, districts_[i].centre) <= kDistrictRadius) return static_cast<int>(i);
    return -1;
}

bool WorldLayout::joined(std::size_t a, std::size_t b) const {
    for (std::size_t i = 0; i < roadCount_; ++i)
        if ((roads_[i].a == a && roads_[i].b == b) || (roads_[i].a == b && roads_[i].b == a)) return true;
    return false;
}

std::size_t WorldLayout::tour(std::size_t from, std::uint8_t* out, std::size_t capacity) const {
    if (from >= districtCount_ || capacity == 0) return 0;
    std::array<bool, kMaxDistricts> seen{};
    std::array<std::uint8_t, kMaxDistricts> stack{};
    std::size_t depth = 0, n = 0;
    stack[depth++] = static_cast<std::uint8_t>(from);
    seen[from] = true;
    out[n++] = static_cast<std::uint8_t>(from);
    while (depth > 0 && n < capacity) {
        const std::size_t at = stack[depth - 1];
        std::size_t next = kMaxDistricts;
        for (std::size_t k = 0; k < districtCount_ && next == kMaxDistricts; ++k)
            if (!seen[k] && joined(at, k)) next = k;
        if (next != kMaxDistricts) {
            seen[next] = true;
            stack[depth++] = static_cast<std::uint8_t>(next);
            out[n++] = static_cast<std::uint8_t>(next);
            continue;
        }
        --depth;
        bool left = false;
        for (std::size_t k = 0; k < districtCount_; ++k) left = left || !seen[k];
        if (!left) break; // every district reached: no need to walk back
        if (depth > 0) out[n++] = stack[depth - 1];
    }
    return n;
}

namespace {

// Whether a road a-b keeps clear of every other district's ring.
bool roadClear(const std::array<WorldDistrict, WorldLayout::kMaxDistricts>& d, std::size_t count, std::size_t a,
               std::size_t b) {
    for (std::size_t k = 0; k < count; ++k)
        if (k != a && k != b &&
            segmentDistance(d[k].centre, d[a].centre, d[b].centre) < WorldLayout::kDistrictRadius + WorldLayout::kRoadWidth)
            return false;
    return true;
}

} // namespace

bool generateWorldLayout(const WorldSpec& spec, WorldLayout& out, std::string* error) {
    const auto fail = [&](const std::string& why) {
        out.districtCount_ = out.roadCount_ = out.blockCount_ = out.propCount_ = 0;
        if (error) *error = "world " + spec.id + ": " + why;
        return false;
    };
    if (!(spec.side >= WorldSpec::kMinSide && spec.side <= WorldSpec::kMaxSide)) return fail("side out of range");
    if (spec.districts < 1 || spec.districts > WorldLayout::kMaxDistricts) return fail("district count out of range");

    out.side_ = spec.side;
    out.kind_ = spec.kind;
    out.districtCount_ = out.roadCount_ = out.blockCount_ = out.propCount_ = 0;
    const std::size_t count = spec.districts;
    const float half = spec.side * 0.5f;
    const float span = half - WorldLayout::kEdgeMargin;

    // Districts (best candidate of 40 per district) and roads; a later attempt reseeds.
    constexpr int kAttempts = 64;
    bool placed = false;
    WorldRng rng{1};
    for (int attempt = 0; attempt < kAttempts && !placed; ++attempt) {
        rng.s = detail::mixSeed(spec.seed * 0x9e3779b9u + static_cast<std::uint32_t>(attempt) * 0x85ebca6bu + 1u);
        out.districtCount_ = 0;
        out.roadCount_ = 0;
        bool fits = true;
        for (std::size_t i = 0; i < count && fits; ++i) {
            Vec2 best{};
            float bestGap = -1.f;
            for (int c = 0; c < 40; ++c) {
                const Vec2 p{rng.range(-span, span), rng.range(-span, span)};
                float gap = 1e30f;
                for (std::size_t k = 0; k < i; ++k) gap = std::min(gap, worldDist(p, out.districts_[k].centre));
                if (gap > bestGap) {
                    bestGap = gap;
                    best = p;
                }
            }
            if (bestGap < WorldLayout::kMinSpacing) fits = false;
            out.districts_[i] = WorldDistrict{};
            out.districts_[i].centre = best;
            out.districtCount_ = i + 1;
        }
        if (!fits) continue;

        // Roads: a minimum spanning tree over the roads that keep clear of other districts (Prim)...
        std::array<bool, WorldLayout::kMaxDistricts> inTree{};
        inTree[0] = true;
        bool joinedAll = true;
        for (std::size_t added = 1; added < count; ++added) {
            float bestLen = 1e30f;
            std::size_t ba = 0, bb = 0;
            for (std::size_t a = 0; a < count; ++a) {
                if (!inTree[a]) continue;
                for (std::size_t b = 0; b < count; ++b) {
                    if (inTree[b]) continue;
                    const float len = worldDist(out.districts_[a].centre, out.districts_[b].centre);
                    if (len < bestLen && roadClear(out.districts_, count, a, b)) {
                        bestLen = len;
                        ba = a;
                        bb = b;
                    }
                }
            }
            if (bestLen >= 1e30f) {
                joinedAll = false;
                break;
            }
            inTree[bb] = true;
            out.roads_[out.roadCount_++] = {static_cast<std::uint8_t>(ba), static_cast<std::uint8_t>(bb)};
        }
        if (!joinedAll) continue;

        // ...plus shortcuts: a short clear road that crosses none and saves a long way round.
        const std::size_t extras = count / 4;
        for (std::size_t e = 0; e < extras && out.roadCount_ < WorldLayout::kMaxRoads; ++e) {
            // Shortest ways over the roads so far (Floyd-Warshall, at most 16 districts).
            std::array<std::array<float, WorldLayout::kMaxDistricts>, WorldLayout::kMaxDistricts> way{};
            for (std::size_t a = 0; a < count; ++a)
                for (std::size_t b = 0; b < count; ++b) way[a][b] = a == b ? 0.f : 1e30f;
            for (std::size_t r = 0; r < out.roadCount_; ++r) {
                const std::size_t a = out.roads_[r].a, b = out.roads_[r].b;
                way[a][b] = way[b][a] = worldDist(out.districts_[a].centre, out.districts_[b].centre);
            }
            for (std::size_t k = 0; k < count; ++k)
                for (std::size_t a = 0; a < count; ++a)
                    for (std::size_t b = 0; b < count; ++b) way[a][b] = std::min(way[a][b], way[a][k] + way[k][b]);
            float bestSaving = 0.f;
            std::size_t ba = 0, bb = 0;
            for (std::size_t a = 0; a < count; ++a)
                for (std::size_t b = a + 1; b < count; ++b) {
                    if (out.joined(a, b)) continue;
                    const Vec2 pa = out.districts_[a].centre, pb = out.districts_[b].centre;
                    const float len = worldDist(pa, pb);
                    if (len > 2.f * WorldLayout::kMinSpacing + 100.f || way[a][b] < 1.8f * len) continue;
                    if (!roadClear(out.districts_, count, a, b)) continue;
                    bool crosses = false;
                    for (std::size_t r = 0; r < out.roadCount_ && !crosses; ++r)
                        crosses = segmentsCross(pa, pb, out.districts_[out.roads_[r].a].centre,
                                                out.districts_[out.roads_[r].b].centre);
                    if (!crosses && way[a][b] - len > bestSaving) {
                        bestSaving = way[a][b] - len;
                        ba = a;
                        bb = b;
                    }
                }
            if (bestSaving <= 0.f) break;
            out.roads_[out.roadCount_++] = {static_cast<std::uint8_t>(ba), static_cast<std::uint8_t>(bb)};
        }
        placed = true;
    }
    if (!placed)
        return fail("can't fit " + std::to_string(count) + " districts " +
                    std::to_string(static_cast<int>(WorldLayout::kMinSpacing)) + " m apart, joined by roads, in a " +
                    std::to_string(static_cast<int>(spec.side)) + " m world");

    // District frames: the spawn faces the world's centre, with the plaza's centre kPlazaCentreX ahead.
    for (std::size_t i = 0; i < count; ++i) {
        WorldDistrict& d = out.districts_[i];
        const float len = std::hypot(d.centre.x, d.centre.y);
        d.forward = len > 1.f ? Vec2{-d.centre.x / len, -d.centre.y / len} : Vec2{1.f, 0.f};
        d.origin = {d.centre.x - d.forward.x * WorldLayout::kPlazaCentreX,
                    d.centre.y - d.forward.y * WorldLayout::kPlazaCentreX};
    }

    // Landmarks: in the building ring, behind the spawn as near as the roads allow, so a walker sees it
    // on the way in from anywhere.
    const float ringR = (WorldLayout::kPlazaRadius + WorldLayout::kDistrictRadius) * 0.5f;
    for (std::size_t i = 0; i < count; ++i) {
        WorldDistrict& d = out.districts_[i];
        const float back = std::atan2(-d.forward.y, -d.forward.x);
        bool found = false;
        for (int k = 0; k < 18 && !found; ++k) {
            const float off = static_cast<float>((k + 1) / 2) * (kWorldPi / 9.f) * ((k % 2) ? 1.f : -1.f);
            const Vec2 p{d.centre.x + std::cos(back + off) * ringR, d.centre.y + std::sin(back + off) * ringR};
            if (out.roadDistance(p) >= WorldLayout::kLandmarkRadius + WorldLayout::kRoadWidth * 0.5f + 2.f) {
                d.landmark = p;
                found = true;
            }
        }
        if (!found) return fail("no room for district " + std::to_string(i) + "'s landmark");
        d.landmarkShape = static_cast<std::uint8_t>(i % 3);
        d.landmarkHeight = spec.kind == WorldKind::Towers ? rng.range(80.f, 130.f) : rng.range(40.f, 80.f);
    }

    // Buildings: one ring per district, facing the plaza, clear of roads and the landmark.
    for (std::size_t i = 0; i < count; ++i) {
        const WorldDistrict& d = out.districts_[i];
        float angle = rng.range(0.f, 2.f * kWorldPi);
        const float end = angle + 2.f * kWorldPi;
        while (out.blockCount_ < WorldLayout::kMaxBlocks) {
            WorldBlock b;
            b.district = static_cast<std::uint8_t>(i);
            switch (spec.kind) {
            case WorldKind::Town:
                b.width = rng.range(8.f, 12.f), b.depth = rng.range(7.f, 10.f), b.height = rng.range(5.f, 9.f);
                b.roof = true;
                break;
            case WorldKind::Towers:
                b.width = rng.range(10.f, 14.f), b.depth = rng.range(10.f, 14.f), b.height = rng.range(18.f, 42.f);
                break;
            case WorldKind::Ruins:
                b.width = rng.range(4.f, 11.f), b.depth = rng.range(1.f, 3.f), b.height = rng.range(1.5f, 5.f);
                break;
            }
            const float gap = spec.kind == WorldKind::Ruins ? rng.range(4.f, 12.f) : rng.range(2.f, 5.f);
            const float step = (b.width + gap) / ringR;
            const float at = angle + step * 0.5f;
            angle += step;
            if (angle > end) break;
            const float r = ringR + rng.range(-2.f, 2.f);
            b.pos = {d.centre.x + std::cos(at) * r, d.centre.y + std::sin(at) * r};
            b.yaw = at + kWorldPi * 0.5f; // width runs along the ring
            const float foot = 0.5f * std::hypot(b.width, b.depth);
            const bool clear = out.roadDistance(b.pos) >= foot + WorldLayout::kRoadWidth * 0.5f + 1.f &&
                               worldDist(b.pos, d.landmark) >= foot + WorldLayout::kLandmarkRadius + 2.f &&
                               r - foot >= WorldLayout::kPlazaRadius && r + foot <= WorldLayout::kDistrictRadius;
            if (clear) out.blocks_[out.blockCount_++] = b;
        }
    }

    // Trees and rocks on the open ground: off roads, outside district rings, inside the walls.
    const std::size_t target = spec.kind == WorldKind::Towers ? 700 : spec.kind == WorldKind::Ruins ? 1600 : 1400;
    const float treeShare = spec.kind == WorldKind::Towers ? 0.2f : spec.kind == WorldKind::Ruins ? 0.35f : 0.75f;
    for (std::size_t tries = 0; tries < target * 4 && out.propCount_ < std::min(target, WorldLayout::kMaxProps);
         ++tries) {
        const Vec2 p{rng.range(-half + 6.f, half - 6.f), rng.range(-half + 6.f, half - 6.f)};
        const float scale = rng.range(0.7f, 1.6f);
        const bool tree = rng.unit() < treeShare;
        bool clear = out.roadDistance(p) >= WorldLayout::kRoadWidth * 0.5f + 3.f;
        for (std::size_t k = 0; k < count && clear; ++k)
            clear = worldDist(p, out.districts_[k].centre) >= WorldLayout::kDistrictRadius + 4.f;
        if (!clear) continue;
        WorldProp& prop = out.props_[out.propCount_++];
        prop.pos = p;
        prop.scale = scale;
        prop.kind = tree ? 0 : 1;
    }
    return true;
}

} // namespace ftcore
