#include "ftcore/Overworld.h"

#include "SeedMix.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

constexpr float kOverPi = 3.14159265358979f;

struct OverRng {
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

float overDist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

float overSegmentDistance(Vec2 p, Vec2 a, Vec2 b) {
    const float abx = b.x - a.x, aby = b.y - a.y;
    const float len2 = abx * abx + aby * aby;
    float t = len2 > 0.f ? ((p.x - a.x) * abx + (p.y - a.y) * aby) / len2 : 0.f;
    t = std::clamp(t, 0.f, 1.f);
    return std::hypot(p.x - (a.x + abx * t), p.y - (a.y + aby * t));
}

} // namespace

float OverworldLayout::gateYaw(std::size_t node) const {
    const OverworldNode& n = nodes_[node];
    return std::atan2(-n.outward.y, -n.outward.x);
}

float OverworldLayout::roadDistance(Vec2 p) const {
    float best = 1e30f;
    for (std::size_t i = 0; i < roadCount_; ++i)
        best = std::min(best, overSegmentDistance(p, nodes_[roads_[i].a].gate, nodes_[roads_[i].b].gate));
    return best;
}

int OverworldLayout::nodeAt(Vec2 p) const {
    for (std::size_t i = 0; i < nodeCount_; ++i)
        if (overDist(p, nodes_[i].gate) <= kGateReach) return static_cast<int>(i);
    return -1;
}

bool OverworldLayout::joined(std::size_t a, std::size_t b) const {
    for (std::size_t i = 0; i < roadCount_; ++i)
        if ((roads_[i].a == a && roads_[i].b == b) || (roads_[i].a == b && roads_[i].b == a)) return true;
    return false;
}

std::size_t OverworldLayout::path(std::size_t from, std::size_t to, std::uint8_t* out, std::size_t capacity) const {
    if (from >= nodeCount_ || to >= nodeCount_ || capacity == 0) return 0;
    // Breadth-first from `to`, so following `toward` from `from` walks the way in order.
    std::array<std::uint8_t, kMaxNodes> toward{};
    std::array<bool, kMaxNodes> seen{};
    std::array<std::uint8_t, kMaxNodes> queue{};
    std::size_t head = 0, tail = 0;
    queue[tail++] = static_cast<std::uint8_t>(to);
    seen[to] = true;
    while (head < tail) {
        const std::size_t at = queue[head++];
        for (std::size_t k = 0; k < nodeCount_; ++k)
            if (!seen[k] && joined(at, k)) {
                seen[k] = true;
                toward[k] = static_cast<std::uint8_t>(at);
                queue[tail++] = static_cast<std::uint8_t>(k);
            }
    }
    if (!seen[from]) return 0;
    std::size_t n = 0;
    std::size_t at = from;
    out[n++] = static_cast<std::uint8_t>(at);
    while (at != to && n < capacity) {
        at = toward[at];
        out[n++] = static_cast<std::uint8_t>(at);
    }
    return at == to ? n : 0;
}

bool generateOverworldLayout(const WorldTable& table, OverworldLayout& out, std::string* error) {
    const auto fail = [&](const std::string& why) {
        out.nodeCount_ = out.roadCount_ = out.blockCount_ = out.propCount_ = 0;
        if (error) *error = "overworld: " + why;
        return false;
    };
    out.nodeCount_ = out.roadCount_ = out.blockCount_ = out.propCount_ = 0;
    const OverworldSpec& spec = table.overworld;
    if (!spec.present) return fail("the world table has none");
    if (!(spec.side >= OverworldSpec::kMinSide && spec.side <= OverworldSpec::kMaxSide)) return fail("side out of range");
    const std::size_t count = table.worlds.size();
    if (count < 1 || count > OverworldLayout::kMaxNodes) return fail("expected 1.." + std::to_string(OverworldLayout::kMaxNodes) + " worlds");

    out.side_ = spec.side;
    const float half = spec.side * 0.5f;
    const float reach = half - OverworldLayout::kEdgeMargin - OverworldLayout::kEmblemRadius;
    OverRng rng{1};

    // Nodes: gates on a ring, in order, so neighbouring gates are about kHopTarget apart; each emblem
    // stands kGateOffset behind its gate. A later attempt reseeds.
    const float ring = count >= 3 ? OverworldLayout::kHopTarget / (2.f * std::sin(kOverPi / static_cast<float>(count)))
                                  : OverworldLayout::kHopTarget * 0.5f;
    constexpr int kAttempts = 64;
    bool placed = false;
    for (int attempt = 0; attempt < kAttempts && !placed; ++attempt) {
        rng.s = detail::mixSeed(spec.seed * 0x9e3779b9u + static_cast<std::uint32_t>(attempt) * 0x85ebca6bu + 7u);
        const float turn = rng.range(0.f, 2.f * kOverPi);
        const float jitter = count >= 2 ? 0.2f * kOverPi / static_cast<float>(count) : 0.f;
        bool fits = true;
        for (std::size_t i = 0; i < count; ++i) {
            OverworldNode& n = out.nodes_[i];
            n = OverworldNode{};
            n.world = static_cast<std::uint8_t>(i);
            n.kind = table.worlds[i].kind;
            if (count == 1) {
                n.outward = {1.f, 0.f};
                n.gate = {0.f, 0.f};
            } else {
                const float a = turn + 2.f * kOverPi * static_cast<float>(i) / static_cast<float>(count) +
                                rng.range(-jitter, jitter);
                const float r = ring * rng.range(0.92f, 1.08f);
                n.outward = {std::cos(a), std::sin(a)};
                n.gate = {n.outward.x * r, n.outward.y * r};
            }
            n.centre = {n.gate.x + n.outward.x * OverworldLayout::kGateOffset,
                        n.gate.y + n.outward.y * OverworldLayout::kGateOffset};
            fits = fits && std::fabs(n.centre.x) <= reach && std::fabs(n.centre.y) <= reach;
        }
        out.nodeCount_ = count;
        // Roads: round the ring (one road for two nodes).
        out.roadCount_ = 0;
        const std::size_t roads = count >= 3 ? count : count - 1;
        for (std::size_t i = 0; i < roads; ++i)
            out.roads_[out.roadCount_++] = {static_cast<std::uint8_t>(i), static_cast<std::uint8_t>((i + 1) % count)};
        for (std::size_t r = 0; r < out.roadCount_ && fits; ++r) {
            const OverworldRoad& road = out.roads_[r];
            const float len = overDist(out.nodes_[road.a].gate, out.nodes_[road.b].gate);
            fits = len >= OverworldLayout::kMinHop && len <= OverworldLayout::kMaxHop;
            for (std::size_t k = 0; k < count && fits; ++k)
                fits = k == road.a || k == road.b ||
                       overSegmentDistance(out.nodes_[k].centre, out.nodes_[road.a].gate, out.nodes_[road.b].gate) >=
                       OverworldLayout::kNodeClear + OverworldLayout::kRoadWidth * 0.5f;
        }
        placed = fits;
    }
    if (!placed)
        return fail("can't fit " + std::to_string(count) + " nodes " + std::to_string(static_cast<int>(OverworldLayout::kMinHop)) +
                    ".." + std::to_string(static_cast<int>(OverworldLayout::kMaxHop)) + " m apart in a " +
                    std::to_string(static_cast<int>(spec.side)) + " m overworld");

    // Emblems: a small cluster of the world's kind round a tall landmark at the centre, open toward
    // the gate.
    for (std::size_t i = 0; i < count; ++i) {
        OverworldNode& n = out.nodes_[i];
        n.landmarkHeight = n.kind == WorldKind::Towers ? rng.range(36.f, 46.f)
                         : n.kind == WorldKind::Ruins  ? rng.range(14.f, 20.f)
                                                       : rng.range(20.f, 26.f);
        const float front = std::atan2(-n.outward.y, -n.outward.x);
        const int pieces = n.kind == WorldKind::Ruins ? 7 : n.kind == WorldKind::Towers ? 5 : 6;
        for (int p = 0; p < pieces && out.blockCount_ < OverworldLayout::kMaxBlocks; ++p) {
            // Spread over the back 260 degrees; the 100 in front of the landmark stay open.
            const float at = front + kOverPi * (50.f / 180.f) +
                             (kOverPi * (260.f / 180.f)) * (static_cast<float>(p) + 0.5f) / static_cast<float>(pieces);
            WorldBlock b;
            b.district = static_cast<std::uint8_t>(i);
            float r = 10.f;
            switch (n.kind) {
            case WorldKind::Town:
                b.width = rng.range(4.f, 6.f), b.depth = rng.range(4.f, 5.f), b.height = rng.range(3.f, 5.f);
                b.roof = true;
                r = rng.range(9.f, 11.f);
                break;
            case WorldKind::Towers:
                b.width = rng.range(3.5f, 5.f), b.depth = b.width, b.height = rng.range(14.f, 26.f);
                r = rng.range(8.f, 10.f);
                break;
            case WorldKind::Ruins:
                b.width = rng.range(3.f, 6.f), b.depth = rng.range(0.8f, 1.6f), b.height = rng.range(1.f, 4.f);
                r = rng.range(9.f, 12.f);
                break;
            }
            b.pos = {n.centre.x + std::cos(at) * r, n.centre.y + std::sin(at) * r};
            b.yaw = at + kOverPi * 0.5f;
            out.blocks_[out.blockCount_++] = b;
        }
    }

    // Trees and rocks: off roads, clear of emblems and gates, inside the walls.
    const std::size_t target = std::min(OverworldLayout::kMaxProps, static_cast<std::size_t>(spec.side * spec.side / 900.f));
    for (std::size_t tries = 0; tries < target * 4 && out.propCount_ < target; ++tries) {
        const Vec2 p{rng.range(-half + 6.f, half - 6.f), rng.range(-half + 6.f, half - 6.f)};
        const float scale = rng.range(0.7f, 1.5f);
        const bool tree = rng.unit() < 0.6f;
        bool clear = out.roadDistance(p) >= OverworldLayout::kRoadWidth * 0.5f + 3.f;
        for (std::size_t k = 0; k < count && clear; ++k)
            clear = overDist(p, out.nodes_[k].centre) >= OverworldLayout::kNodeClear &&
                    overDist(p, out.nodes_[k].gate) >= OverworldLayout::kGateClear + 2.f;
        if (!clear) continue;
        WorldProp& prop = out.props_[out.propCount_++];
        prop.pos = p;
        prop.scale = scale;
        prop.kind = tree ? 0 : 1;
    }
    return true;
}

bool overworldRoute(const OverworldLayout& map, std::size_t fromWorld, std::size_t toWorld, OverworldRoute& out) {
    out = OverworldRoute{};
    if (fromWorld == toWorld) return false;
    std::array<std::uint8_t, OverworldLayout::kMaxNodes> way{};
    const std::size_t n = map.path(fromWorld, toWorld, way.data(), way.size());
    if (n < 2) return false;
    out.from = static_cast<std::uint8_t>(fromWorld);
    out.to = static_cast<std::uint8_t>(toWorld);
    out.start = map.node(fromWorld).gate;
    for (std::size_t i = 1; i < n; ++i) out.points[out.pointCount++] = map.node(way[i]).gate;
    out.startYaw = std::atan2(out.points[0].y - out.start.y, out.points[0].x - out.start.x);
    return true;
}

void worldFirstScenes(const WorldTable& table, const std::vector<std::string_view>& sceneOrder, std::vector<std::size_t>& out) {
    out.assign(table.worlds.size(), sceneOrder.size());
    for (std::size_t i = 0; i < sceneOrder.size(); ++i)
        if (const WorldScene* s = table.findScene(sceneOrder[i]); s && s->world < out.size())
            out[s->world] = std::min(out[s->world], i);
}

std::uint32_t worldsOpen(const std::vector<std::size_t>& firstScenes, std::size_t reached) {
    std::uint32_t mask = 0;
    for (std::size_t w = 0; w < firstScenes.size() && w < 32; ++w)
        if (firstScenes[w] <= reached) mask |= 1u << w;
    return mask;
}

} // namespace ftcore
