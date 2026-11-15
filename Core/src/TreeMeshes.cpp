#include "ftcore/TreeMeshes.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>

namespace ftcore {

namespace {

std::string_view trimmed(std::string_view v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '"')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '"')) v.remove_suffix(1);
    return v;
}

std::string lower(std::string_view v) {
    std::string s;
    s.reserve(v.size());
    for (const char c : v) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return s;
}

void split(std::string_view line, std::vector<std::string_view>& fields) {
    fields.clear();
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            fields.push_back(trimmed(line.substr(start, i - start)));
            start = i + 1;
        }
    }
}

bool fail(std::string* error, std::size_t line, const std::string& what) {
    if (error) *error = "line " + std::to_string(line) + ": " + what;
    return false;
}

std::uint32_t fnv(std::uint32_t h, std::uint32_t v) {
    for (int b = 0; b < 4; ++b) {
        h ^= (v >> (8 * b)) & 0xffu;
        h *= 16777619u;
    }
    return h;
}

// A repeatable share in [0, 1) of the seed and a salt.
double share(std::uint32_t seed, std::uint32_t salt) {
    std::uint32_t h = fnv(seed, salt);
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    return static_cast<double>(h >> 8) / 16777216.0;
}

std::uint32_t seedOf(const StandInPoint& p) {
    std::uint32_t h = 2166136261u;
    if (!p.id.empty()) {
        for (const char c : p.id) {
            h ^= static_cast<unsigned char>(c);
            h *= 16777619u;
        }
        return h;
    }
    h = fnv(h, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(p.x * 10.0))));
    return fnv(h, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(p.y * 10.0))));
}

bool serves(const TreeModel& m, std::string_view kind) { return m.weight > 0.0 && m.height > 0.0 && m.kind == kind; }

} // namespace

bool parseTreeModels(std::string_view csv, std::vector<TreeModel>& out, std::string* error) {
    constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    std::size_t colKind = kNone, colAsset = kNone, colWeight = kNone, width = 0;
    bool header = false;
    std::vector<std::string_view> f;
    std::size_t lineNo = 0, pos = 0;
    while (pos < csv.size()) {
        std::size_t end = csv.find('\n', pos);
        if (end == std::string_view::npos) end = csv.size();
        const std::string_view line = csv.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;
        const std::string_view t = trimmed(line);
        if (t.empty() || t.front() == '#') continue;
        split(line, f);
        if (!header) {
            for (std::size_t i = 0; i < f.size(); ++i) {
                const std::string name = lower(f[i]);
                if (name == "kind" && colKind == kNone) colKind = i;
                if ((name == "asset" || name == "mesh" || name == "model") && colAsset == kNone) colAsset = i;
                if (name == "weight" && colWeight == kNone) colWeight = i;
            }
            if (colKind == kNone || colAsset == kNone) return fail(error, lineNo, "the header needs kind and asset columns");
            width = std::max(colKind, colAsset) + 1;
            header = true;
            continue;
        }
        if (f.size() < width) return fail(error, lineNo, "a short row");
        TreeModel m;
        m.kind = lower(f[colKind]);
        m.asset = std::string(f[colAsset]);
        if (m.kind.empty() || m.asset.empty()) return fail(error, lineNo, "an empty kind or asset");
        if (colWeight != kNone && colWeight < f.size() && !f[colWeight].empty()) {
            const std::string_view w = f[colWeight];
            const auto r = std::from_chars(w.data(), w.data() + w.size(), m.weight);
            if (r.ec != std::errc() || r.ptr != w.data() + w.size() || !(m.weight >= 0.0)) return fail(error, lineNo, "the weight is not a number >= 0");
        }
        out.push_back(std::move(m));
    }
    if (!header) return fail(error, lineNo, "no header row");
    return true;
}

std::string_view treeKindOf(const StandInPoint& point) {
    if (!point.tag.empty()) return point.tag;
    return point.kind == StandInKind::StreetTree ? std::string_view("street") : std::string_view("park");
}

TreePick pickTreeModel(const StandInPoint& point, const TreeModel* models, std::size_t count, const TreePickSpec& spec) {
    TreePick pick;
    std::string_view kind = treeKindOf(point);
    bool any = false;
    for (std::size_t i = 0; i < count && !any; ++i) any = serves(models[i], kind);
    if (!any) {
        kind = point.kind == StandInKind::StreetTree ? std::string_view("street") : std::string_view("park");
        for (std::size_t i = 0; i < count && !any; ++i) any = serves(models[i], kind);
    }
    if (!any) return pick;

    const std::uint32_t seed = seedOf(point);
    const auto wanted = [&](const TreeModel& m) { return point.h > 0.0 ? point.h : m.height * (point.scale > 0.f ? point.scale : 1.0); };
    double total = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        if (!serves(models[i], kind)) continue;
        const double s = wanted(models[i]) / models[i].height;
        if (s >= spec.minScale && s <= spec.maxScale) total += models[i].weight;
    }
    if (total > 0.0) {
        double u = share(seed, 1) * total;
        for (std::size_t i = 0; i < count; ++i) {
            if (!serves(models[i], kind)) continue;
            const double s = wanted(models[i]) / models[i].height;
            if (s < spec.minScale || s > spec.maxScale) continue;
            pick.model = static_cast<int>(i);
            u -= models[i].weight;
            if (u < 0.0) break;
        }
    } else {
        double best = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            if (!serves(models[i], kind)) continue;
            const double off = std::fabs(std::log(wanted(models[i]) / models[i].height));
            if (pick.model < 0 || off < best) {
                best = off;
                pick.model = static_cast<int>(i);
            }
        }
        pick.clamped = true;
    }

    const TreeModel& m = models[pick.model];
    const double jitter = 1.0 + spec.jitter * (2.0 * share(seed, 2) - 1.0);
    pick.scaleZ = std::clamp(wanted(m) / m.height, spec.minScale, spec.maxScale) * jitter;
    double xy = pick.scaleZ * (1.0 + 0.5 * spec.crownSpread * (2.0 * share(seed, 3) - 1.0));
    if (point.canopy > 0.0 && m.crown > 0.0) xy = point.canopy / m.crown;
    pick.scaleXY = std::clamp(xy, pick.scaleZ * (1.0 - spec.crownSpread), pick.scaleZ * (1.0 + spec.crownSpread));
    pick.yawDeg = 360.0 * share(seed, 4);
    return pick;
}

} // namespace ftcore
