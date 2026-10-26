#pragma once

// The overworld: a small walkable map that joins the big worlds (WorldLayout.h). Each world is one
// node, a reduced-scale emblem of its kind (a cluster of houses, towers or ruins round a tall
// landmark) with a gate in front of it; straight roads join the gates. The player is full size.
//
//   ftcore::OverworldLayout map;
//   if (!ftcore::generateOverworldLayout(table, map, &error)) ...;   // deterministic per seed
//   ftcore::OverworldRoute route;
//   ftcore::overworldRoute(map, fromWorld, toWorld, route);           // no allocation
//
// Node i is world i of the table. The nodes lie on a ring round the centre in order, so neighbours'
// gates are kMinHop..kMaxHop metres apart by road: a 25-40 s run. Each emblem stands behind its gate,
// away from the centre, so every road and every gate is straight and clear.
//
// When the story's next field step is in another world than the last one played, the player walks
// from the last world's gate to the next one's (overworldRoute) and enters there. A world is open
// once the script's first scene placed in it has started (worldFirstScenes, worldsOpen).

#include "ftcore/Api.h"
#include "ftcore/Explore.h"
#include "ftcore/WorldLayout.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

struct OverworldNode {
    Vec2 centre;  // the emblem's centre
    Vec2 gate;    // the roads end here; Use here enters the world
    Vec2 outward; // unit, from the map's centre toward the node (the gate faces the other way)
    std::uint8_t world = 0;
    WorldKind kind = WorldKind::Town;
    float landmarkHeight = 0.f; // the emblem's landmark, at `centre`
};

struct OverworldRoad {
    std::uint8_t a = 0, b = 0; // node indices
};

class OverworldLayout {
public:
    static constexpr std::size_t kMaxNodes = 8;
    static constexpr std::size_t kMaxRoads = 16;
    static constexpr std::size_t kMaxBlocks = 96;
    static constexpr std::size_t kMaxProps = 512;

    static constexpr float kEdgeMargin = 40.f;   // emblems stay this far inside the edge
    static constexpr float kGateOffset = 26.f;   // gate to emblem centre
    static constexpr float kEmblemRadius = 16.f; // the emblem's blocks lie inside this
    static constexpr float kNodeClear = 34.f;    // nothing else within this of an emblem's centre
    static constexpr float kGateClear = 8.f;     // nothing within this of a gate
    static constexpr float kGateReach = 4.f;     // a walker this near a gate is at it
    static constexpr float kRoadWidth = 6.f;
    static constexpr float kHopTarget = 210.f;   // gate to neighbouring gate
    static constexpr float kMinHop = 150.f, kMaxHop = 250.f;

    float side() const { return side_; }
    std::size_t nodeCount() const { return nodeCount_; }
    std::size_t roadCount() const { return roadCount_; }
    std::size_t blockCount() const { return blockCount_; }
    std::size_t propCount() const { return propCount_; }
    const OverworldNode& node(std::size_t i) const { return nodes_[i]; }
    const OverworldRoad& road(std::size_t i) const { return roads_[i]; }
    // The emblems' blocks; `district` is the node index.
    const WorldBlock& block(std::size_t i) const { return blocks_[i]; }
    const WorldProp& prop(std::size_t i) const { return props_[i]; }

    // The gate's facing in radians: toward the map's centre.
    FTCORE_API float gateYaw(std::size_t node) const;
    // Distance from p to the nearest road's centre line (a large number with no roads).
    FTCORE_API float roadDistance(Vec2 p) const;
    // The node whose gate is within kGateReach of p, or -1.
    FTCORE_API int nodeAt(Vec2 p) const;
    // Whether road a-b exists (either way round).
    FTCORE_API bool joined(std::size_t a, std::size_t b) const;
    // The fewest-roads way from node `from` to node `to`, both included. Writes at most `capacity`
    // node indices; returns how many (0 when either is out of range or `to` can't be reached).
    // No allocation.
    FTCORE_API std::size_t path(std::size_t from, std::size_t to, std::uint8_t* out, std::size_t capacity) const;

private:
    friend FTCORE_API bool generateOverworldLayout(const WorldTable&, OverworldLayout&, std::string*);

    float side_ = 0.f;
    std::size_t nodeCount_ = 0, roadCount_ = 0, blockCount_ = 0, propCount_ = 0;
    std::array<OverworldNode, kMaxNodes> nodes_{};
    std::array<OverworldRoad, kMaxRoads> roads_{};
    std::array<WorldBlock, kMaxBlocks> blocks_{};
    std::array<WorldProp, kMaxProps> props_{};
};

// Deterministic in table.overworld and the worlds' kinds. Fails (and says why) when the table has no
// overworld, no worlds or more than kMaxNodes, or the nodes don't fit in the side.
FTCORE_API bool generateOverworldLayout(const WorldTable& table, OverworldLayout& out, std::string* error = nullptr);

// A walk from one world's gate to another's along the roads.
struct OverworldRoute {
    std::uint8_t from = 0, to = 0; // node (= world) indices
    Vec2 start;                    // from's gate
    float startYaw = 0.f;          // facing the first point
    // The gates walked to in turn; the last is to's gate.
    std::array<Vec2, OverworldLayout::kMaxNodes> points{};
    std::size_t pointCount = 0;

    Vec2 goal() const { return points[pointCount - 1]; }
};

// False when either world is out of range, they're the same, or no road joins them. No allocation.
FTCORE_API bool overworldRoute(const OverworldLayout& map, std::size_t fromWorld, std::size_t toWorld,
                               OverworldRoute& out);

// For each world of the table, the position in `sceneOrder` (the script's scene ids in play order) of
// the first scene placed in it; sceneOrder.size() when none is. Allocates `out`: at load.
FTCORE_API void worldFirstScenes(const WorldTable& table, const std::vector<std::string_view>& sceneOrder,
                                 std::vector<std::size_t>& out);
// Bit w set: world w is open once the scene at position `reached` in that order has started.
// No allocation.
FTCORE_API std::uint32_t worldsOpen(const std::vector<std::size_t>& firstScenes, std::size_t reached);

} // namespace ftcore
