#pragma once

// The three big worlds' blockout layouts (Data/Field/worlds.json, WorldTable) and the map each one
// generates (WorldLayout), shared by the Unreal map builder and the field screen at run time.
//
//   ftcore::WorldTable table;
//   if (!ftcore::importWorldTable(text, table, &error)) ...;               // allocates: at load
//   ftcore::WorldLayout layout;
//   if (!ftcore::generateWorldLayout(table.worlds[0], layout, &error)) ...; // deterministic per seed
//   const ftcore::Vec2 at = layout.toWorld(district, goal);                // no allocation
//
// A world is a square of `side` metres centred on the origin. It holds districts (a clear plaza
// ringed by blockout buildings, with a tall landmark), straight roads between district centres that
// join every district, and scattered trees and rocks on the open ground between them. Nothing on
// the map stands on a road or in a plaza, so a walk along roads, or anywhere inside a plaza, is
// straight and clear.
//
// Each district has its own frame, the frame of Data/Field/goals.json: the origin is the spawn,
// +x is the way the spawn faces (toward the world's centre) and +y is to its left. Every goal and
// waypoint of goals.json (x -12..30, y -26..26) lies inside the plaza, so a story field step plays
// the same in any district as on the Explore map.
//
// worlds.json (ids and numbers only, no story text):
//   { "version": 1,
//     "worlds": [ { "id": "solhaven", "map": "World_Solhaven", "style": "solhaven", "kind": "town",
//                   "side": 1000, "seed": 3, "districts": 16, "story": ["Solhaven"] } ],
//     "scenes": [ { "scene": "SC-X", "world": "solhaven", "district": 2, "style": "inksolhaven" } ],
//     "overworld": { "map": "Overworld", "side": 480, "seed": 7 } }
//   `kind` is town, towers or ruins (the buildings' shape). `story` lists the script's scene world
//   names the world plays (checked by a test against the script). A scene's field steps play in its
//   district; `style` optionally overrides the world's style for that scene. A scene not listed
//   plays on the Explore map. `overworld` (optional) is the small map that joins the worlds
//   (ftcore/Overworld.h); without it the story goes straight from one world to the next. Unknown
//   keys are warnings; anything else wrong is an error.

#include "ftcore/Api.h"
#include "ftcore/Explore.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

enum class WorldKind : std::uint8_t {
    Town,   // houses with pitched roofs
    Towers, // tall flat-topped blocks
    Ruins,  // low broken walls and stubs, sparse
};

struct WorldSpec {
    static constexpr float kMinSide = 600.f;
    static constexpr float kMaxSide = 4000.f;

    std::string id;    // "solhaven": lower-case letters, digits and '_'
    std::string map;   // the Unreal map under /Game/Maps
    std::string style; // the style controller's spec ("solhaven")
    WorldKind kind = WorldKind::Town;
    float side = 1000.f;
    std::uint32_t seed = 1;
    std::uint32_t districts = 8;
    std::vector<std::string> story; // the script's world names this world plays
};

struct WorldScene {
    std::string scene;
    std::uint32_t world = 0;    // index into WorldTable::worlds
    std::uint32_t district = 0; // < that world's districts
    std::string style;          // empty: the world's own
};

struct OverworldSpec {
    static constexpr float kMinSide = 300.f;
    static constexpr float kMaxSide = 1200.f;

    bool present = false;
    std::string map; // the Unreal map under /Game/Maps
    float side = 480.f;
    std::uint32_t seed = 1;
};

struct WorldTable {
    std::vector<WorldSpec> worlds;
    std::vector<WorldScene> scenes;
    OverworldSpec overworld;

    // No allocation. Null when the scene plays on the Explore map.
    FTCORE_API const WorldScene* findScene(std::string_view scene) const;
    // The world with this id or map name, or -1.
    FTCORE_API int findWorld(std::string_view idOrMap) const;
};

// On failure `out` is empty and *error lists each problem with its path, one per line.
FTCORE_API bool importWorldTable(std::string_view json, WorldTable& out, std::string* error = nullptr,
                                 std::vector<std::string>* warnings = nullptr);

struct WorldDistrict {
    Vec2 centre;    // the plaza's centre
    Vec2 origin;    // the spawn (goals.json's origin)
    Vec2 forward;   // unit: the spawn's facing (local +x)
    Vec2 landmark;  // base of the landmark, in the building ring
    float landmarkHeight = 0.f;
    std::uint8_t landmarkShape = 0; // 0 a pillar, 1 a pillar with a ring, 2 a stepped spire
};

struct WorldRoad {
    std::uint8_t a = 0, b = 0; // district indices
};

struct WorldBlock {
    Vec2 pos;
    float yaw = 0.f; // radians
    float width = 0.f, depth = 0.f, height = 0.f;
    std::uint8_t district = 0;
    bool roof = false;
};

struct WorldProp {
    Vec2 pos;
    float scale = 1.f;
    std::uint8_t kind = 0; // 0 a tree, 1 a rock
};

class WorldLayout {
public:
    static constexpr std::size_t kMaxDistricts = 16;
    static constexpr std::size_t kMaxRoads = 32;
    static constexpr std::size_t kMaxBlocks = 512;
    static constexpr std::size_t kMaxProps = 2048;

    // goals.json's box (district frame) and the plaza circle that covers it.
    static constexpr float kPlazaMinX = -12.f, kPlazaMaxX = 30.f, kPlazaMaxAbsY = 26.f;
    static constexpr float kPlazaCentreX = 9.f;  // the plaza's centre in the district frame
    static constexpr float kPlazaRadius = 36.f;  // > half the box's diagonal (33.4)
    static constexpr float kDistrictRadius = 62.f; // plaza + building ring
    static constexpr float kRoadWidth = 6.f;
    static constexpr float kEdgeMargin = 80.f;   // district centres stay this far inside the edge
    static constexpr float kMinSpacing = 150.f;  // between district centres
    static constexpr float kLandmarkRadius = 4.f;

    float side() const { return side_; }
    WorldKind kind() const { return kind_; }
    std::size_t districtCount() const { return districtCount_; }
    std::size_t roadCount() const { return roadCount_; }
    std::size_t blockCount() const { return blockCount_; }
    std::size_t propCount() const { return propCount_; }
    const WorldDistrict& district(std::size_t i) const { return districts_[i]; }
    const WorldRoad& road(std::size_t i) const { return roads_[i]; }
    const WorldBlock& block(std::size_t i) const { return blocks_[i]; }
    const WorldProp& prop(std::size_t i) const { return props_[i]; }

    // District frame <-> world (metres). No allocation.
    Vec2 toWorld(std::size_t district, Vec2 local) const;
    Vec2 toLocal(std::size_t district, Vec2 world) const;
    // The spawn's yaw in radians (atan2 of forward).
    FTCORE_API float spawnYaw(std::size_t district) const;

    // Distance from p to the nearest road's centre line (a large number with no roads).
    FTCORE_API float roadDistance(Vec2 p) const;
    // The district whose ring contains p, or -1.
    FTCORE_API int districtAt(Vec2 p) const;
    // Whether roads a-b exist (either way round).
    FTCORE_API bool joined(std::size_t a, std::size_t b) const;

    // A walk over the roads from district `from` that reaches every district: each consecutive pair
    // is joined by a road (a depth-first tour, walking back along roads already taken). Writes at
    // most `capacity` district indices; returns how many. No allocation.
    FTCORE_API std::size_t tour(std::size_t from, std::uint8_t* out, std::size_t capacity) const;

private:
    friend FTCORE_API bool generateWorldLayout(const WorldSpec&, WorldLayout&, std::string*);

    float side_ = 0.f;
    WorldKind kind_ = WorldKind::Town;
    std::size_t districtCount_ = 0, roadCount_ = 0, blockCount_ = 0, propCount_ = 0;
    std::array<WorldDistrict, kMaxDistricts> districts_{};
    std::array<WorldRoad, kMaxRoads> roads_{};
    std::array<WorldBlock, kMaxBlocks> blocks_{};
    std::array<WorldProp, kMaxProps> props_{};
};

inline Vec2 WorldLayout::toWorld(std::size_t district, Vec2 local) const {
    const WorldDistrict& d = districts_[district];
    return {d.origin.x + d.forward.x * local.x - d.forward.y * local.y,
            d.origin.y + d.forward.y * local.x + d.forward.x * local.y};
}

inline Vec2 WorldLayout::toLocal(std::size_t district, Vec2 world) const {
    const WorldDistrict& d = districts_[district];
    const float dx = world.x - d.origin.x;
    const float dy = world.y - d.origin.y;
    return {dx * d.forward.x + dy * d.forward.y, -dx * d.forward.y + dy * d.forward.x};
}

// Deterministic in the spec. Fails (and says why) when the districts don't fit at kMinSpacing or
// the roads can't join them all without crossing another district.
FTCORE_API bool generateWorldLayout(const WorldSpec& spec, WorldLayout& out, std::string* error = nullptr);

} // namespace ftcore
