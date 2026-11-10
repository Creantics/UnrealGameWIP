#include "ftcore/Crowd.h"

#include "CsvRead.h"
#include "ftcore/MassingPlan.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace ftcore {

using detail::isComment;
using detail::parseNumber;
using detail::Record;
using detail::splitCsv;
using detail::trim;

bool importCrowdStreets(std::string_view csv, double side, std::vector<CrowdStreet>& out, std::string* error) {
    out.clear();
    std::vector<MassingStreet> geo;
    if (!importMassingStreets(csv, geo, error)) return false;
    massingStreetsToEngine(geo, side);

    // The optional kind and pavement_m columns, row for row with importMassingStreets (same comment and header rules).
    std::vector<Record> records;
    if (!splitCsv(csv, records, error)) return false;
    std::size_t kindCol = SIZE_MAX, paveCol = SIZE_MAX;
    bool haveHeader = false;
    std::size_t row = 0;
    out.resize(geo.size());
    for (const Record& r : records) {
        if (isComment(r)) continue;
        if (!haveHeader) {
            for (std::size_t f = 0; f < r.fields.size(); ++f) {
                const std::string_view name = trim(r.fields[f]);
                if (name == "kind") kindCol = f;
                if (name == "pavement_m") paveCol = f;
            }
            haveHeader = true;
            continue;
        }
        if (row >= geo.size()) break;
        CrowdStreet& s = out[row];
        const MassingStreet& g = geo[row];
        ++row;
        s.width = static_cast<float>(g.width);
        s.points.reserve(g.points.size());
        for (const MassingVec& p : g.points) s.points.push_back({static_cast<float>(p.x), static_cast<float>(p.y)});
        if (kindCol < r.fields.size()) s.kind = std::string(trim(r.fields[kindCol]));
        if (paveCol < r.fields.size()) {
            const std::string_view v = trim(r.fields[paveCol]);
            double d = 0.0;
            if (!v.empty()) {
                if (!parseNumber(v, d) || d < 0.0 || d * 2.0 > g.width)
                    return detail::fail(error, r.line, g.id + ": pavement_m must be a number >= 0, at most half the width");
                s.pavement = static_cast<float>(d);
            }
        }
    }
    return true;
}

namespace {

float dist(Vec2 a, Vec2 b) { return std::hypot(b.x - a.x, b.y - a.y); }

struct BuildEdge {
    std::uint32_t a = 0, b = 0;
    float lane = 0.f, laneHalf = 0.f, half = 0.f; // half: the street's half width
    bool dead = false;
};

struct Split {
    std::uint32_t edge;
    float t;
    std::uint32_t node;
};

std::int64_t cellKey(std::int64_t x, std::int64_t y) { return (x << 32) ^ (y & 0xffffffffLL); }

class NodeMaker {
public:
    NodeMaker(std::vector<Vec2>& pos, float snap) : pos_(pos), snap_(std::max(snap, 0.01f)) {}
    std::uint32_t at(Vec2 p) {
        const std::int64_t cx = static_cast<std::int64_t>(std::floor(p.x / snap_));
        const std::int64_t cy = static_cast<std::int64_t>(std::floor(p.y / snap_));
        std::uint32_t best = UINT32_MAX;
        float bestD = snap_;
        for (std::int64_t dy = -1; dy <= 1; ++dy)
            for (std::int64_t dx = -1; dx <= 1; ++dx) {
                const auto it = cells_.find(cellKey(cx + dx, cy + dy));
                if (it == cells_.end()) continue;
                for (const std::uint32_t n : it->second) {
                    const float d = dist(pos_[n], p);
                    if (d < bestD) bestD = d, best = n;
                }
            }
        if (best != UINT32_MAX) return best;
        pos_.push_back(p);
        const auto n = static_cast<std::uint32_t>(pos_.size() - 1);
        cells_[cellKey(cx, cy)].push_back(n);
        return n;
    }
    // A node exactly here, never merged (crossings and junction feet).
    std::uint32_t fresh(Vec2 p) {
        pos_.push_back(p);
        const auto n = static_cast<std::uint32_t>(pos_.size() - 1);
        cells_[cellKey(static_cast<std::int64_t>(std::floor(p.x / snap_)), static_cast<std::int64_t>(std::floor(p.y / snap_)))]
            .push_back(n);
        return n;
    }

private:
    std::vector<Vec2>& pos_;
    float snap_;
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> cells_;
};

// Closest point on segment ab to p: its parameter t in [0, 1].
float project(Vec2 a, Vec2 b, Vec2 p) {
    const float ux = b.x - a.x, uy = b.y - a.y;
    const float l2 = ux * ux + uy * uy;
    if (l2 <= 1e-12f) return 0.f;
    return std::clamp(((p.x - a.x) * ux + (p.y - a.y) * uy) / l2, 0.f, 1.f);
}

Vec2 lerp(Vec2 a, Vec2 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

float cross(Vec2 o, Vec2 a, Vec2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

} // namespace

bool CrowdGraph::build(const std::vector<CrowdStreet>& streets, const CrowdGraphTuning& tuning, std::string* error) {
    pos_.clear();
    first_.clear();
    edges_.clear();
    spawnable_.clear();
    spawnableCount_ = 0;
    components_ = 0;

    std::vector<BuildEdge> edges;
    NodeMaker nodes(pos_, tuning.snap);
    for (const CrowdStreet& s : streets) {
        if (!s.kind.empty() && std::find(tuning.walkKinds.begin(), tuning.walkKinds.end(), s.kind) == tuning.walkKinds.end())
            continue;
        BuildEdge proto;
        proto.half = s.width * 0.5f;
        if (s.pavement > 0.f) {
            proto.lane = proto.half - s.pavement * 0.5f;
            proto.laneHalf = std::max(0.1f, s.pavement * 0.5f - 0.2f);
        } else {
            proto.lane = 0.f;
            proto.laneHalf = std::max(0.f, proto.half - tuning.edgeMargin);
        }
        std::uint32_t prev = UINT32_MAX;
        for (const Vec2& p : s.points) {
            const std::uint32_t n = nodes.at(p);
            if (prev != UINT32_MAX && prev != n) {
                BuildEdge e = proto;
                e.a = prev;
                e.b = n;
                edges.push_back(e);
            }
            prev = n;
        }
    }
    if (edges.empty()) {
        if (error) *error = "no walkable street segment";
        return false;
    }

    // Segment grid for the junction and crossing joins.
    constexpr float kCell = 16.f;
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> segCells;
    auto cellsOf = [&](std::uint32_t e, float pad, auto&& fn) {
        const Vec2 a = pos_[edges[e].a], b = pos_[edges[e].b];
        const auto x0 = static_cast<std::int64_t>(std::floor((std::min(a.x, b.x) - pad) / kCell));
        const auto x1 = static_cast<std::int64_t>(std::floor((std::max(a.x, b.x) + pad) / kCell));
        const auto y0 = static_cast<std::int64_t>(std::floor((std::min(a.y, b.y) - pad) / kCell));
        const auto y1 = static_cast<std::int64_t>(std::floor((std::max(a.y, b.y) + pad) / kCell));
        for (std::int64_t y = y0; y <= y1; ++y)
            for (std::int64_t x = x0; x <= x1; ++x) fn(x, y);
    };
    const auto baseEdges = static_cast<std::uint32_t>(edges.size());
    for (std::uint32_t e = 0; e < baseEdges; ++e)
        cellsOf(e, 0.f, [&](std::int64_t x, std::int64_t y) { segCells[cellKey(x, y)].push_back(e); });

    std::vector<std::uint32_t> deg0(pos_.size(), 0);
    for (const BuildEdge& e : edges) ++deg0[e.a], ++deg0[e.b];

    std::vector<Split> splits;
    std::vector<BuildEdge> joins;
    // A street's loose end that stops on another street joins it.
    const auto baseNodes = static_cast<std::uint32_t>(pos_.size());
    for (std::uint32_t n = 0; n < baseNodes; ++n) {
        if (deg0[n] != 1) continue;
        const Vec2 p = pos_[n];
        std::uint32_t best = UINT32_MAX;
        float bestD = 1e30f, bestT = 0.f;
        const auto cx = static_cast<std::int64_t>(std::floor(p.x / kCell));
        const auto cy = static_cast<std::int64_t>(std::floor(p.y / kCell));
        for (std::int64_t dy = -2; dy <= 2; ++dy)
            for (std::int64_t dx = -2; dx <= 2; ++dx) {
                const auto it = segCells.find(cellKey(cx + dx, cy + dy));
                if (it == segCells.end()) continue;
                for (const std::uint32_t e : it->second) {
                    const BuildEdge& be = edges[e];
                    if (be.a == n || be.b == n) continue;
                    const float t = project(pos_[be.a], pos_[be.b], p);
                    const float d = dist(lerp(pos_[be.a], pos_[be.b], t), p);
                    if (d <= be.half + tuning.junctionReach && d < bestD) bestD = d, best = e, bestT = t;
                }
            }
        if (best == UINT32_MAX) continue;
        const BuildEdge& be = edges[best];
        const float segLen = dist(pos_[be.a], pos_[be.b]);
        std::uint32_t foot;
        if (bestT * segLen <= tuning.snap) foot = be.a;
        else if ((1.f - bestT) * segLen <= tuning.snap) foot = be.b;
        else {
            foot = nodes.fresh(lerp(pos_[be.a], pos_[be.b], bestT));
            splits.push_back({best, bestT, foot});
        }
        if (foot == n) continue;
        BuildEdge j = be;
        j.a = n;
        j.b = foot;
        j.dead = false;
        joins.push_back(j);
    }
    // Streets crossing without a shared point join where they cross (once, in the cell holding the crossing).
    for (const auto& [key, list] : segCells) {
        for (std::size_t i = 0; i < list.size(); ++i)
            for (std::size_t k = i + 1; k < list.size(); ++k) {
                const BuildEdge& e1 = edges[list[i]];
                const BuildEdge& e2 = edges[list[k]];
                if (e1.a == e2.a || e1.a == e2.b || e1.b == e2.a || e1.b == e2.b) continue;
                const Vec2 a = pos_[e1.a], b = pos_[e1.b], c = pos_[e2.a], d = pos_[e2.b];
                const float d1 = cross(c, d, a), d2 = cross(c, d, b), d3 = cross(a, b, c), d4 = cross(a, b, d);
                if (!(((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0))) || d1 == 0 || d2 == 0 || d3 == 0 || d4 == 0) continue;
                const float t = d1 / (d1 - d2);
                const float u = d3 / (d3 - d4);
                const Vec2 x = lerp(a, b, t);
                if (cellKey(static_cast<std::int64_t>(std::floor(x.x / kCell)), static_cast<std::int64_t>(std::floor(x.y / kCell))) != key)
                    continue;
                const std::uint32_t m = nodes.fresh(x);
                splits.push_back({list[i], t, m});
                splits.push_back({list[k], u, m});
            }
    }
    // Apply the splits: each split edge becomes a chain through its split nodes.
    std::sort(splits.begin(), splits.end(), [](const Split& l, const Split& r) {
        return l.edge != r.edge ? l.edge < r.edge : l.t < r.t;
    });
    for (std::size_t i = 0; i < splits.size();) {
        const std::uint32_t e = splits[i].edge;
        BuildEdge proto = edges[e];
        edges[e].dead = true;
        std::uint32_t prev = proto.a;
        for (; i < splits.size() && splits[i].edge == e; ++i) {
            if (splits[i].node == prev) continue;
            BuildEdge part = proto;
            part.a = prev;
            part.b = splits[i].node;
            edges.push_back(part);
            prev = splits[i].node;
        }
        if (prev != proto.b) {
            BuildEdge part = proto;
            part.a = prev;
            part.b = proto.b;
            edges.push_back(part);
        }
    }
    edges.insert(edges.end(), joins.begin(), joins.end());

    // CSR, both directions, duplicates dropped.
    struct Dir {
        std::uint32_t from, to;
        float lane, laneHalf;
    };
    std::vector<Dir> dirs;
    dirs.reserve(edges.size() * 2);
    for (const BuildEdge& e : edges) {
        if (e.dead || e.a == e.b) continue;
        dirs.push_back({e.a, e.b, e.lane, e.laneHalf});
        dirs.push_back({e.b, e.a, e.lane, e.laneHalf});
    }
    std::sort(dirs.begin(), dirs.end(), [](const Dir& l, const Dir& r) { return l.from != r.from ? l.from < r.from : l.to < r.to; });
    dirs.erase(std::unique(dirs.begin(), dirs.end(), [](const Dir& l, const Dir& r) { return l.from == r.from && l.to == r.to; }),
               dirs.end());
    const auto n = static_cast<std::uint32_t>(pos_.size());
    first_.assign(n + 1, 0);
    for (const Dir& d : dirs) ++first_[d.from + 1];
    for (std::uint32_t i = 0; i < n; ++i) first_[i + 1] += first_[i];
    edges_.resize(dirs.size());
    for (std::size_t i = 0; i < dirs.size(); ++i) {
        const Dir& d = dirs[i];
        edges_[i] = {d.to, std::max(0.01f, dist(pos_[d.from], pos_[d.to])), d.lane, d.laneHalf};
    }

    // Components: islands smaller than minComponent are never spawned on.
    std::vector<std::uint32_t> comp(n, UINT32_MAX), stack, size;
    for (std::uint32_t s = 0; s < n; ++s) {
        if (comp[s] != UINT32_MAX || degree(s) == 0) continue;
        const auto c = static_cast<std::uint32_t>(size.size());
        size.push_back(0);
        stack.push_back(s);
        comp[s] = c;
        while (!stack.empty()) {
            const std::uint32_t v = stack.back();
            stack.pop_back();
            ++size[c];
            for (std::uint32_t k = first_[v]; k < first_[v + 1]; ++k) {
                const std::uint32_t w = edges_[k].to;
                if (comp[w] == UINT32_MAX) comp[w] = c, stack.push_back(w);
            }
        }
    }
    components_ = static_cast<std::uint32_t>(size.size());
    spawnable_.assign(n, 0);
    for (std::uint32_t v = 0; v < n; ++v)
        if (comp[v] != UINT32_MAX && size[comp[v]] >= tuning.minComponent) spawnable_[v] = 1, ++spawnableCount_;

    // Node grid.
    min_ = max_ = pos_[0];
    for (const Vec2& p : pos_) {
        min_.x = std::min(min_.x, p.x), min_.y = std::min(min_.y, p.y);
        max_.x = std::max(max_.x, p.x), max_.y = std::max(max_.y, p.y);
    }
    gw_ = static_cast<std::uint32_t>((max_.x - min_.x) / cell_) + 1;
    gh_ = static_cast<std::uint32_t>((max_.y - min_.y) / cell_) + 1;
    cellFirst_.assign(static_cast<std::size_t>(gw_) * gh_ + 1, 0);
    auto cellOf = [&](Vec2 p) {
        const auto x = std::min(gw_ - 1, static_cast<std::uint32_t>((p.x - min_.x) / cell_));
        const auto y = std::min(gh_ - 1, static_cast<std::uint32_t>((p.y - min_.y) / cell_));
        return static_cast<std::size_t>(y) * gw_ + x;
    };
    for (std::uint32_t v = 0; v < n; ++v)
        if (degree(v) > 0) ++cellFirst_[cellOf(pos_[v]) + 1];
    for (std::size_t i = 1; i < cellFirst_.size(); ++i) cellFirst_[i] += cellFirst_[i - 1];
    cellNodes_.assign(cellFirst_.back(), 0);
    std::vector<std::uint32_t> fill(cellFirst_.begin(), cellFirst_.end() - 1);
    for (std::uint32_t v = 0; v < n; ++v)
        if (degree(v) > 0) cellNodes_[fill[cellOf(pos_[v])]++] = v;
    return true;
}

const CrowdEdge* CrowdGraph::find(std::uint32_t a, std::uint32_t b) const {
    for (std::uint32_t k = first_[a]; k < first_[a + 1]; ++k)
        if (edges_[k].to == b) return &edges_[k];
    return nullptr;
}

std::uint32_t CrowdGraph::nearest(Vec2 p, float within) const {
    if (pos_.empty()) return UINT32_MAX;
    const auto r = static_cast<std::int64_t>(std::ceil(within / cell_));
    const auto cx = static_cast<std::int64_t>(std::floor((p.x - min_.x) / cell_));
    const auto cy = static_cast<std::int64_t>(std::floor((p.y - min_.y) / cell_));
    std::uint32_t best = UINT32_MAX;
    float bestD = within;
    for (std::int64_t y = std::max<std::int64_t>(0, cy - r); y <= std::min<std::int64_t>(gh_ - 1, cy + r); ++y)
        for (std::int64_t x = std::max<std::int64_t>(0, cx - r); x <= std::min<std::int64_t>(gw_ - 1, cx + r); ++x) {
            const std::size_t c = static_cast<std::size_t>(y) * gw_ + static_cast<std::size_t>(x);
            for (std::uint32_t i = cellFirst_[c]; i < cellFirst_[c + 1]; ++i) {
                const float d = dist(pos_[cellNodes_[i]], p);
                if (d <= bestD) bestD = d, best = cellNodes_[i];
            }
        }
    return best;
}

} // namespace ftcore
