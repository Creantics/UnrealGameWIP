#pragma once

// The map overlay's look (MapOverlay.h), derived once at load from the baked minimap model and the names table, with
// no change to the model or its blob: the sea (the water that reaches the sea-side edge) and its coast, three wave
// lines off the coast, the canals' edges, the empty blocks between buildings, roads and water (parks hatched, yards
// edged in dots), the framing that keeps the sea to a quarter of the frame, and where the sea's name goes.
//
// mapBuildLook allocates (once, at the overlay's load); everything else here only reads and allocates nothing.

#include "ftcore/Api.h"
#include "ftcore/MapOverlay.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ftcore {

// The kinds of stroke the look draws, each kept apart so the host draws each in its own weight and colour.
enum class MapStrokeKind : std::uint8_t { Coast, Wave12, Wave24, Wave40, Canal, ParkHatch, YardDot };
inline constexpr int kMapStrokeKinds = 7;
// The wave lines' distances from the coast, out to sea (m): Wave12, Wave24, Wave40.
inline constexpr float kMapWaveM[3] = {12.f, 24.f, 40.f};
// A stroke: the line a-b (world m), or a dot at a when a == b. level: the farthest zoom it is drawn at, as the
// MapZoom's index (0 every zoom, 1 Mid and Near, 2 Near only).
struct MapStroke {
    Vec2 a{}, b{};
    std::uint8_t level = 0;
};
inline bool mapStrokeShown(const MapStroke& s, MapZoom z) { return s.level <= static_cast<std::uint8_t>(z); }
// No stroke is longer than this (m); strokes are kept by the kMapStrokeTileM tile their midpoint is in, so the tiles
// one beyond a box hold every stroke that meets it.
inline constexpr float kMapStrokeMaxM = 32.f;
inline constexpr float kMapStrokeTileM = 64.f;

// Blocks: the empty ground left between buildings (grown by half a cell), roads (their width, plus three quarters of
// a cell) and water, on a kMapBlockCellM grid, in 4-connected pieces. A piece of at least kMapBlockMinM2 that neither
// touches the model's edge or the sea (a shore or a quay) nor exceeds kMapBlockMaxM2 (open ground, not a block) is a
// block: a park when a place of
// open public ground (mapGreenKind) stands in it, else a yard. Smaller pieces are slivers between buildings, too small
// to mark. Parks: a fine diagonal hatch every kMapHatchStepM (every 2nd at Mid, every 4th at Far). Yards: a dotted
// edge a cell inside the block, a dot every kMapYardDotStepM (every 2nd at Mid; none at Far, where yards are too small
// to read).
inline constexpr float kMapBlockCellM = 2.f;
inline constexpr float kMapBlockMinM2 = 150.f;
inline constexpr float kMapBlockMaxM2 = 60000.f;
inline constexpr float kMapHatchStepM = 6.f;
inline constexpr float kMapYardDotStepM = 3.f;

// Framing: the land's share of a frame never under this where a shift can reach it, and the player kept inside the
// middle kMapKeepInner of the frame (each axis) at Mid and Near.
inline constexpr float kMapLandShareMin = 0.75f;
inline constexpr float kMapKeepInner = 0.8f;

struct MapLook {
    // The sea, per cell of the model's water grid (same size and origin): 1 for the sea. Water the sea never reaches
    // (basins, canals) is not sea; it counts as land in the framing. Up to four dry rows at the north or south edge
    // before the first row with sea (the bake's margin past its height field) take that row's sea.
    std::vector<std::uint8_t> sea;
    std::vector<std::uint32_t> seaSum; // (w + 1) * (h + 1): the sea cells above and left of each grid corner
    float cell = 0.f, x0 = 0.f, y0 = 0.f;
    std::uint32_t w = 0, h = 0;
    int seaSide = 0; // mapSeaSide: +1 east, -1 west, 0 none (the sea is then the largest body of water)
    // The bounds of the land (every cell that isn't sea; the model's bounds when it has no water grid).
    float landX0 = 0.f, landY0 = 0.f, landX1 = 0.f, landY1 = 0.f;
    // Strokes per kind, by tile (tileFirst per kind: tilesW * tilesH + 1 offsets).
    std::array<std::vector<MapStroke>, kMapStrokeKinds> strokes;
    std::array<std::vector<std::uint32_t>, kMapStrokeKinds> tileFirst;
    std::uint32_t tilesW = 0, tilesH = 0;
    float tileX0 = 0.f, tileY0 = 0.f;
    // The sea's deepest cell per 8 x 8 cells (its distance to land, m): where its name can go.
    struct SeaSpot {
        Vec2 at{};
        float depth = 0.f;
    };
    std::vector<SeaSpot> seaSpots;
    // What the blocks came to, for the log.
    std::uint32_t parks = 0, yards = 0, slivers = 0, open = 0;
};

// Builds the look of model m. names (may be null: no parks) gives the places of open public ground.
FTCORE_API void mapBuildLook(const MiniMapModel& m, const MapNames* names, MapLook& out);

// Whether a world point is sea as the overlay draws it: a sea cell in the model, off the model on its sea side, or
// north or south of the model over a sea cell of its edge row (the edge rows run on past the edge).
FTCORE_API bool mapIsSea(const MiniMapModel& m, const MapLook& look, Vec2 world);
// The land's share of view v's frame (0..1): everything but the sea as drawn (mapIsSea: the model's sea cells, the
// sea side's strip at the frame's full height, and the edge rows' sea run on north and south). Canals, basins and the
// hills off the model's other sides are land.
FTCORE_API float mapLandShare(const MiniMapModel& m, const MapLook& look, const MapOverlayView& v);

// The frame for a player (or cursor) at `focus`. Mid and Near: centred on the focus (held inside the model as
// mapOverlayClampCentre does), then, when the sea takes more than a quarter of it, moved straight away from the sea
// (west for an east sea) by the least amount that brings the land to kMapLandShareMin, never so far that the focus
// leaves the middle kMapKeepInner of the frame. Far: the scale fits the land's bounds (kMapFarMargin all round, at
// least kMapFarOverMid times Mid's), centred as near the focus as keeps the land's bounds in the frame, then moved away
// from the sea in the same way inside that room. When no allowed shift reaches the share, the frame takes the best
// share found (the least shift among equals) and `reached` is false.
struct MapFrame {
    MapOverlayView view;
    float land = 1.f;
    bool reached = true;
};
FTCORE_API MapFrame mapOverlayFrame(const MiniMapModel& m, const MapLook& look, Vec2 focus, MapZoom z, float screenW,
                                    float screenH, float uiScale = 1.f);
// A panned or zoomed view's centre held to the model (mapOverlayClampCentre's room, per axis), widened to take in the
// framed centre `framed`, so a pan can always come back to the frame and the frame itself is never pulled back to sea.
FTCORE_API Vec2 mapOverlayHold(const MiniMapModel& m, const MapOverlayView& v, Vec2 framed);

// Where the sea's name goes at view v, level, w x h px: over the sea's deepest point on screen whose label box lies
// wholly on the sea and on screen (the open sea off the model too). False when none does.
FTCORE_API bool mapSeaLabelAt(const MiniMapModel& m, const MapLook& look, const MapOverlayView& v, float w, float h, Vec2& at);

// Where a water name goes at view v: a label of w x h px (the text, its lean included) seated on its own water, the
// body that holds `at` (world m): inside the water rectangle that holds it, else within reachM of it. Seats lie on a
// lattice (3 px apart, coarser for a big body), tried nearest `at` first, level and, in a rectangle whose long side is
// not level, turned along that side too. A seat is taken when the box itself is all water (mapIsWater or mapIsSea at
// probes no more than 4 px apart, corners included), and the box grown by `grow` px is whole on the screen and meets no
// road (mapLabelOnAnyRoad, with the cursor's lit roads). Dry: `at` is not on water; NoSeat: no seat passed. Either way the
// name is left out, never set over land or a road. Zero allocation.
enum class MapWaterSeat : std::uint8_t { Seated, Dry, NoSeat };
inline constexpr float kMapWaterReachM = 40.f;
FTCORE_API MapWaterSeat mapWaterLabelAt(const MiniMapModel& m, const MapNames& n, const MapLook& look, const MapOverlayView& v,
                                        MapZoom z, const MapRoadInk& ink, Vec2 at, float w, float h, float grow, MapLabel& out,
                                        int picked = -1, std::uint16_t pickedName = kMapNoName, float reachM = kMapWaterReachM);

// Calls f(const MapStroke&) for every stroke of `kind` that may meet the world box lo-hi (and some just beyond it:
// the caller still culls). Zero allocation.
template <class F>
void mapEachStroke(const MapLook& look, MapStrokeKind kind, Vec2 lo, Vec2 hi, F&& f) {
    const std::size_t k = static_cast<std::size_t>(kind);
    const std::vector<MapStroke>& list = look.strokes[k];
    const std::vector<std::uint32_t>& first = look.tileFirst[k];
    const std::size_t tiles = static_cast<std::size_t>(look.tilesW) * look.tilesH;
    if (list.empty() || tiles == 0 || first.size() != tiles + 1) return;
    const auto tileOf = [](float at, float origin, std::uint32_t count) {
        const float t = (at - origin) / kMapStrokeTileM;
        if (!(t > 0.f)) return 0;
        const float top = static_cast<float>(count - 1);
        return static_cast<int>(t < top ? t : top);
    };
    const int tx0 = tileOf(lo.x, look.tileX0, look.tilesW) - 1, tx1 = tileOf(hi.x, look.tileX0, look.tilesW) + 1;
    const int ty0 = tileOf(lo.y, look.tileY0, look.tilesH) - 1, ty1 = tileOf(hi.y, look.tileY0, look.tilesH) + 1;
    const int tw = static_cast<int>(look.tilesW), th = static_cast<int>(look.tilesH);
    for (int ty = ty0 < 0 ? 0 : ty0; ty <= ty1 && ty < th; ++ty) {
        for (int tx = tx0 < 0 ? 0 : tx0; tx <= tx1 && tx < tw; ++tx) {
            const std::size_t t = static_cast<std::size_t>(ty) * look.tilesW + static_cast<std::size_t>(tx);
            for (std::uint32_t i = first[t]; i < first[t + 1]; ++i) f(list[i]);
        }
    }
}

} // namespace ftcore
