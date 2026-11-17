#pragma once

// What the drawn water surface needs from the water's maths: the tiles that cover the bodies, and each draw group's
// parameters for the material's copy of the waves (Plugins/FTLook/Shaders/Private/Water/Surface.ush).
//
// Draw groups. A body draws with one group's material instance; a group shares one wave class and one tide:
//   Sea      a sea                       (Sea waves, the sea's tide)
//   River    a flowing canal row         (River waves, the river's tide)
//   Canal    a still canal row           (Canal waves, the canal's tide)
//   Lake     a lake                      (River waves, no tide)
//   Pool     a pool                      (Canal waves, no tide)
// So the material reads only its own eight waves and one tide offset, and the host (UFTWaterSubsystem) pushes them when
// the look, the storm or the clock's hour moves, never per vertex.
//
// Tiles. Each body is covered by square-ish tiles of about tileM, laid in the body's own frame: a box's tiles fit it
// exactly (its size split into whole tiles); a half-plane's run from landOverlapM inland out to seaReachM, and along its
// shore across the bounds given; a polygon gets none (it stays as the world draws it). Every tile carries its body's
// edge line (waterEdgeLine), which the material uses for the wall fade.
//
// Units: metres, but the edge line, which is centimetres as the material's world positions. No allocation.

#include "ftcore/Api.h"
#include "ftcore/WaterBodies.h"
#include "ftcore/WaterWaves.h"

#include <cstddef>
#include <cstdint>

namespace ftcore {

enum class WaterDrawGroup : std::uint8_t { Sea, River, Canal, Lake, Pool };
inline constexpr int kWaterDrawGroups = 5;

FTCORE_API const char* waterDrawGroupName(WaterDrawGroup g);
FTCORE_API WaterDrawGroup waterDrawGroup(const WaterBody& body, const WaterWaveSpec& spec);
FTCORE_API WaterWaveClass waterDrawGroupClass(WaterDrawGroup g);

// A group's material parameters: the eight waves as packSeaWave vectors, the shared steepness, the edge fade (cm), the
// height kept at a wall, and the tide's offset from each body's level (m) at the model's hours.
struct WaterDrawParams {
    float waves[kSeaWaveCount][4] = {};
    float q = 0.f;
    float fadeCm = 150.f;
    float lapShare = 0.35f;
    float tideM = 0.f;
};

FTCORE_API WaterDrawParams waterDrawParams(const WaterSurfaceModel& model, WaterDrawGroup g);

// A body's edge line for the material: {centre x, centre y (cm), axis x, axis y, half across (cm), mode}; mode 0 a box's
// long walls (edge = half - |d|), 1 a half-plane's shore (edge = d), 2 none. d = (p - centre) . axis.
inline constexpr int kWaterEdgeFloats = 6;
FTCORE_API void waterEdgeLine(const WaterBody& body, float out[kWaterEdgeFloats]);
// The same distance from the line (cm in, cm out), as the material's FTWaterEdgeCm reads it.
FTCORE_API float waterEdgeLineCm(const float line[kWaterEdgeFloats], float x, float y);

struct WaterTileSpec {
    float tileM = 16.f;         // a tile's side, about
    float seaReachM = 600.f;    // how far out to sea the tiles run; beyond, the world's far sea plane
    float landOverlapM = 20.f;  // how far inland of a shore the tiles start, under the beach
};

struct WaterTileBounds {
    Vec2 min{}, max{};          // the area a half-plane's tiles are laid across (the map's)
};

struct WaterTile {
    std::uint16_t body = 0;
    WaterDrawGroup group = WaterDrawGroup::Sea;
    Vec2 centre{};              // m
    float cosYaw = 1.f, sinYaw = 0.f; // the tile's x axis
    Vec2 size{};                // m, along its x and y axes
    float z = 0.f;              // the body's level, m
};

// Lays the tiles of every body into out (up to cap) and returns how many the bodies need (more than cap: the rest
// were dropped).
FTCORE_API std::size_t waterTileLayout(const WaterBodies& water, const WaterWaveSpec& waves, const WaterTileSpec& spec,
                                       const WaterTileBounds& bounds, WaterTile* out, std::size_t cap);

// The draw's cap on tiles, all bodies together (a far start or body can't grow the sea's strip past it). A 1 km plan: the sea
// is about 1.8 km along (with the 400 m margins) by 620 m, some 4 400 tiles; 8192 holds a plan to about 2.4 km.
inline constexpr std::size_t kWaterMaxTiles = 8192;

// The same, a body at a time and whole: a body whose tiles don't all fit in what is left of cap is laid not at all (its
// bit in *dropped, the bodies after it still tried). Returns the tiles laid. perBody (kWaterMaxBodies long, may be null):
// each body's need. Bodies past kWaterMaxBodies are not laid.
FTCORE_API std::size_t waterTileLayoutWhole(const WaterBodies& water, const WaterWaveSpec& waves, const WaterTileSpec& spec,
                                            const WaterTileBounds& bounds, WaterTile* out, std::size_t cap,
                                            std::uint32_t* dropped, std::uint32_t* perBody = nullptr);

} // namespace ftcore
