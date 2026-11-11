#pragma once

// The HUD minimap's model: roads by class, building footprints, water, doors and the landmark of one world map, all in
// engine metres (x east, y south: the engine's own X and Y over 100), so a north-up map reads like the engine's top view
// and the map image's rows run north to south.
//
// The model is built once, from a world's layout tables (buildMiniMap, at bake time or under a dev switch), saved as a
// byte blob on the level (saveMiniMap / loadMiniMap), drawn once into an image (rasterMiniMap) and then only read. The
// per-frame queries (the view transform, pins, zoom rules and the street under the player) allocate nothing.

#include "ftcore/Explore.h" // Vec2
#include "ftcore/Api.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

// Road classes, highest drawing and lookup priority first.
enum class MapRoadClass : std::uint8_t { Main, Street, Lane, Stair };
inline constexpr int kMapRoadClasses = 4;
// The class of a road or street kind (coast_road, road, street, lane, stair, ...). False for a kind the map leaves out
// (canal: water, not a road) or doesn't know.
FTCORE_API bool mapRoadClass(std::string_view kind, MapRoadClass& out);
FTCORE_API const char* mapRoadClassName(MapRoadClass c);
// A road or street row with width_m left blank takes its class's width (m); a width that isn't a positive number fails.
inline constexpr double kMapRoadDefaultWidth[kMapRoadClasses] = {10.0, 6.0, 3.0, 2.0};
// Whether a box kind is a building (drawn as a block): kit, box, slab, shrine, terrace.
FTCORE_API bool mapIsBuildingKind(std::string_view kind);
// Boxes lower than this (m, z1 - z0) aren't buildings on the map (kerbs, pads, low walls).
inline constexpr double kMapBuildingMinHeight = 3.0;

struct MapRoad {
    std::string id;
    MapRoadClass cls = MapRoadClass::Street;
    float width = 0.f;         // m, wall to wall
    std::uint32_t first = 0;   // into MiniMapModel::points
    std::uint32_t count = 0;   // at least one
};
// A rotated rectangle: centre, half sizes along its own axes, and the angle of its x axis (radians, from engine +x
// toward engine +y).
struct MapRect {
    float cx = 0.f, cy = 0.f, hx = 0.f, hy = 0.f, rot = 0.f;
};

struct MiniMapModel {
    float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f; // the area shown, engine m
    std::vector<Vec2> points;
    std::vector<MapRoad> roads;
    std::vector<MapRect> buildings;
    std::vector<MapRect> waterRects;  // canals and basins given as rectangles
    float waterCell = 0.f;            // m; 0: no water grid
    std::uint32_t waterW = 0, waterH = 0;
    std::vector<std::uint8_t> water;  // waterW * waterH cells from (minX, minY), row by row; 1: water
    std::vector<Vec2> doors;
    bool hasLandmark = false;
    Vec2 landmark{};
    // The lookup grid (rebuilt by loadMiniMap and buildMiniMap, not saved): the road over each cell, by class priority.
    float gridCell = 0.f;
    std::uint32_t gridW = 0, gridH = 0;
    std::vector<std::int32_t> grid;   // road index, -1: none
};

// What buildMiniMap reads: the layout tables' text (CSV) and the terrain. Plan frame in, engine metres out.
struct MiniMapSource {
    std::string_view roads;    // id,kind,width_m,pts (a canal row, width empty, gives its pts' bounding rectangle as water)
    std::string_view streets;  // id,kind,width_m,pts
    std::string_view boxes;    // importMassingBoxes' table; building kinds of kMapBuildingMinHeight or more
    std::string_view towers;   // id,kind,cx,cy,sx,sy,rot; kind "landmark" is the landmark, the rest buildings
    std::string_view doors;    // importRoadDoors' table (rows of kind door)
    const std::uint16_t* height = nullptr; // value = 32768 + h * 128; row iy (north), column ix (east)
    std::size_t samplesX = 0, samplesY = 0;
    double spacing = 1.0, originX = 0.0, originY = 0.0; // plan frame, the south-west sample
    double seaFromX = 0.0;     // plan x from which ground under sea level is sea
    double side = 0.0;         // the plan's square (massingEnginePoint)
    // Plan frame, the area shown. Left empty (x1 <= x0 or y1 <= y0): the roads' and streets' bounds, kMapAutoAreaPad
    // around them, so the map covers every street the level was built with.
    double areaX0 = 0.0, areaY0 = 0.0, areaX1 = 0.0, areaY1 = 0.0;
    float waterCell = 4.f;     // m
};
inline constexpr float kMapAutoAreaPad = 50.f; // m
// skipped, when given: the road and street kinds the map left out, as "kind xN" joined by ", " in first-seen order
// (canals are water, not skipped), so a new kind in a table shows up in the build log instead of vanishing.
FTCORE_API bool buildMiniMap(const MiniMapSource& src, MiniMapModel& out, std::string* error = nullptr,
                             std::string* skipped = nullptr);
// Rebuilds the lookup grid (gridCell m cells) from the roads.
FTCORE_API void mapBuildGrid(MiniMapModel& m, float gridCell = 2.f);

// A versioned byte blob of the model (the lookup grid is left out and rebuilt on load).
FTCORE_API void saveMiniMap(const MiniMapModel& m, std::vector<std::uint8_t>& out);
FTCORE_API bool loadMiniMap(const std::uint8_t* data, std::size_t size, MiniMapModel& out, std::string* error = nullptr);

// The map drawn once: BGRA8 pixels (B first in memory), row 0 at minY (north), column 0 at minX (west).
struct MapImage {
    std::uint32_t w = 0, h = 0;
    float mPerPx = 1.f;
    float x0 = 0.f, y0 = 0.f; // engine m at the image's top-left corner
    std::vector<std::uint32_t> px; // 0xAARRGGBB
};
inline constexpr std::uint32_t kMapGround = 0xFF1A1E22u;
inline constexpr std::uint32_t kMapWater = 0xFF1F3F5Cu;
inline constexpr std::uint32_t kMapBuilding = 0xFF2E3338u;
inline constexpr std::uint32_t kMapRoadColor[kMapRoadClasses] = {0xFFE2DCC8u, 0xFFB4B2A8u, 0xFF8A8C88u, 0xFF6E7A70u};
// Road widths on the map (px) never thinner than these, so a lane stays a visible line at any scale.
inline constexpr float kMapRoadMinPx[kMapRoadClasses] = {3.f, 2.f, 1.5f, 1.f};
// Draws the model at mPerPx (grown as needed so neither side passes maxSide px): water, buildings, then roads from
// stairs to main so main roads lie on top. powerOfTwo: each side rounded up to a power of two (for a mipped texture),
// the scale grown so the area still fits, the spare east and south strips left as ground.
FTCORE_API void rasterMiniMap(const MiniMapModel& m, float mPerPx, MapImage& out, std::uint32_t maxSide = 4096,
                              bool powerOfTwo = false);

// --- Run time: nothing below allocates. ---

// Which zoom the map is at: on foot, driving, or the large overlay of the whole map.
enum class MapMode : std::uint8_t { OnFoot, InVan, Overlay };
inline constexpr float kMapRadiusOnFoot = 120.f; // m, centre to rim
inline constexpr float kMapRadiusInVan = 250.f;
// The radius for a mode; the overlay's covers the whole model.
FTCORE_API float mapZoomRadius(MapMode mode, const MiniMapModel& m);
// What shows at a zoom: doors only on foot; stairs on foot and on the overlay; the rest always.
struct MapLayers {
    bool doors = false;
    bool stairs = false;
};
FTCORE_API MapLayers mapLayers(MapMode mode);
// Whether the map turns with the heading: on foot and in the van yes, the overlay stays north-up.
FTCORE_API bool mapRotates(MapMode mode);

// A view: the world point at the centre, the radius shown (m), and the heading (engine yaw, degrees: 0 east, 90 south).
// Rotating, the heading points up; otherwise north does.
struct MapView {
    Vec2 centre{};
    float radius = kMapRadiusOnFoot;
    float yawDeg = 0.f;
    bool rotate = true;
};
// A world point (engine m) in view space: the unit disc, x right, y down (screen), 1 at the rim.
FTCORE_API Vec2 mapToView(const MapView& v, Vec2 world);
FTCORE_API Vec2 mapFromView(const MapView& v, Vec2 view);
// North's direction in view space (a unit vector).
FTCORE_API Vec2 mapNorth(const MapView& v);
// A heading (engine yaw) as a unit direction in view space.
FTCORE_API Vec2 mapHeadingDir(const MapView& v, float yawDeg);
// A marker kept on the map: inside `edge` it is where it is; past it, pinned to the rim at `edge` in its direction.
struct MapPin {
    Vec2 at{};
    bool pinned = false;
    float distance = 0.f; // m from the view's centre
};
FTCORE_API MapPin mapPin(const MapView& v, Vec2 world, float edge = 0.9f);
// A world point as image UVs (0..1).
FTCORE_API Vec2 mapImageUv(const MapImage& img, Vec2 world);

// The road over a point (index into roads), -1 for none. Sticky: while the point stays within the previous road's half
// width plus kMapRoadHold, that road is kept, so the street line doesn't flicker at junctions.
inline constexpr float kMapRoadHold = 2.f;
FTCORE_API int mapRoadAt(const MiniMapModel& m, Vec2 world);
FTCORE_API int mapRoadAtSticky(const MiniMapModel& m, Vec2 world, int previous);
// The distance from a point to a road's centre line (m).
FTCORE_API float mapRoadDistance(const MiniMapModel& m, int road, Vec2 world);

} // namespace ftcore
