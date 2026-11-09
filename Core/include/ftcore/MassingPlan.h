#pragma once

// A world's massing: the table of boxes a world's production plan blocks its town out of (kept
// outside the repo and read at build time), and the conversions -run=FTBuildMassingWorld needs to
// stand those boxes on the world's 1 m ground in an Unreal map. Ids, kinds, flags and numbers only:
// the table's other columns (names, notes) are skipped as each row is read and never kept.
//
//   std::vector<ftcore::MassingBox> boxes;
//   if (!ftcore::importMassingBoxes(csvText, boxes, &error)) ...;  // allocates: build time only
//   if (ftcore::massingBuilt(ftcore::massingRole(box))) ...;
//
// The table is CSV (RFC 4180 quoting) with a header row naming its columns; a row whose first field
// starts with '#' is a comment, before the header or after it. The columns read, in any order:
//   id      unique; capital letters, digits, '_' and '-' only, so it serves as an actor label as is
//   src     BLD (a row of the plan), INV (a piece the massing pass added), Z (a zone: an area the
//           plan names, not a solid), OPEN (an option the plan has not ruled), PROXY (a stand-in
//           that never ships) or WALL (an invisible wall the build adds: collision only)
//   kind    the piece's kind, lower case (slab, wall, deck, pier, interior, ...)
//   cx, cy  the footprint's centre, metres, in the plan frame (below)
//   sx, sy  the footprint's size along the box's own u and w axes, metres, > 0
//   rot     degrees counter-clockwise from the plan's +x (east) to the box's u axis
//   z0, z1  bottom and top, metres above the datum (the world's mean sea level), z1 >= z0
//   cover   True or False: True for a piece the plan names, False for the kit that fills between
//   slope   empty, or "x A to B" with an optional ", T thick": the top runs straight from height A
//           at u = -sx/2 to B at u = +sx/2. Only the u axis ("x") is allowed.
// An optional column, row, names the plan row a piece belongs to; the parts of one building share it.
// Any other column is skipped. A missing column, a malformed number, a duplicate id or a row with
// the wrong number of fields is an error naming the line.
//
// The plan frame: x east and y north in metres, origin at the playable square's south-west corner,
// the square running 0 to side on each axis. The engine frame is Unreal's, in metres: centred on the
// square, X east, Y south (Unreal's axes are left-handed, so north is -Y for a map that reads true
// from above), Z up. An engine yaw is Unreal's: degrees from +X towards +Y, clockwise seen from above.

#include "ftcore/Api.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

enum class MassingSource : std::uint8_t { Plan, Invented, Zone, Open, Proxy, Wall };

struct MassingBox {
    std::string id;
    std::string row;           // the plan row it belongs to (the optional "row" column; empty when absent)
    std::string kind;
    MassingSource source = MassingSource::Plan;
    bool cover = false;
    double cx = 0.0, cy = 0.0; // plan frame, m
    double sx = 0.0, sy = 0.0; // m
    double rot = 0.0;          // degrees, counter-clockwise from +x
    double z0 = 0.0, z1 = 0.0; // m above the datum
    bool sloped = false;
    double slopeFrom = 0.0;    // top at u = -sx/2
    double slopeTo = 0.0;      // top at u = +sx/2
    double slopeThick = -1.0;  // < 0: none given
};

// What the build does with a box, in the order it decides (the passed massing render's order):
// a zone first, then a hidden kind (interior, trigger, underpass), then an open option, a proxy, an
// added wall, then cover False as kit and the rest as named pieces.
enum class MassingRole : std::uint8_t { Zone, Hidden, Open, Proxy, Wall, Kit, Named };

// The surface a built box wears until its kit or hero mesh replaces it: plain, a wall's (kit of kind
// wall) or a deck's (a named deck, pier, bridge or quay).
enum class MassingFinish : std::uint8_t { Plain, Wall, Deck };

// A Z row's sloped plate stands this far over its stated elevation (and 1 m under it).
inline constexpr double kMassingZonePlate = 0.3;
// The thinnest box built: a flat box's height is max(z1 - z0 + nudge, this).
inline constexpr double kMassingMinHeight = 0.05;
// Two boxes whose tops are within this of each other and whose footprints overlap share a top.
inline constexpr double kMassingSharedTop = 0.01;
// Each level of a shared top sits this far under the last (massingTopNudges).
inline constexpr double kMassingNudgeStep = 0.02;

FTCORE_API MassingRole massingRole(const MassingBox& box);
FTCORE_API MassingFinish massingFinish(const MassingBox& box);
// Kit, Named and Wall boxes are built; zones, hidden kinds, open options and proxies are references.
inline bool massingBuilt(MassingRole role) {
    return role == MassingRole::Kit || role == MassingRole::Named || role == MassingRole::Wall;
}
// A built box the player sees (not an added wall).
inline bool massingVisible(MassingRole role) { return role == MassingRole::Kit || role == MassingRole::Named; }

// Reads the table into `out` (cleared first). False, with a reason naming the line, on any error.
FTCORE_API bool importMassingBoxes(std::string_view csv, std::vector<MassingBox>& out, std::string* error = nullptr);

struct MassingVec {
    double x = 0.0, y = 0.0, z = 0.0;
};

// Plan frame to engine frame, metres, for a square `side` metres across.
inline MassingVec massingEnginePoint(double side, double x, double y, double z) {
    return {x - side * 0.5, side * 0.5 - y, z};
}
// A plan rotation (counter-clockwise from east) as an engine yaw.
inline double massingEngineYaw(double rotDeg) { return -rotDeg; }

// The box's eight corners in the plan frame: the bottom four, then the top four, each ring in the
// order (-u, -w), (+u, -w), (+u, +w), (-u, +w). A flat box spans z0 to z0 + max(z1 + topNudge - z0,
// kMassingMinHeight). A sloped box's top runs from slopeFrom to slopeTo along u; its bottom lies
// slopeThick under the top when that is given, 1 m under it for a zone (whose top is raised by
// kMassingZonePlate), and at z0 otherwise. topNudge applies to flat boxes only.
FTCORE_API void massingCorners(const MassingBox& box, double topNudge, std::array<MassingVec, 8>& out);

// A cover plate (a named cliff cover: kind cliff, cover True, built as a named piece) is an obstacle to the
// navmesh. Its collision stays, but no navmesh lies on its top, inside its slab or on ground buried in it: the
// build nulls the navmesh there in strips along u. Strip i of massingNullBandCount covers u from its share of sx,
// the whole of sy, and z from the lower of the slab's bottoms at its two ends to the higher of its tops
// (massingCorners), kMassingNullMargin past each; a strip's top rises at most kMassingNullRise across it, and a
// flat box is one strip. Ground further under the slab (a street under a raised end) keeps its navmesh.
// No allocation.
inline constexpr double kMassingNullRise = 0.5;
inline constexpr double kMassingNullMargin = 0.3;
inline constexpr std::size_t kMassingNullMaxBands = 1024;
struct MassingNullBand {
    double u0 = 0.0, u1 = 0.0; // m along the box's u axis, from its centre
    double w0 = 0.0, w1 = 0.0; // m along its w axis, from its centre
    double z0 = 0.0, z1 = 0.0; // m above the datum
};
FTCORE_API bool massingNavObstacle(const MassingBox& box);
FTCORE_API std::size_t massingNullBandCount(const MassingBox& box);
// Strip `index` (clamped to the last) of massingNullBandCount(box).
FTCORE_API MassingNullBand massingNullBand(const MassingBox& box, std::size_t index);

// A box as a closed mesh in the engine frame (metres): its corners (massingCorners, moved by
// massingEnginePoint) and six quads that face outward under Unreal's winding, where (b - a) x (c - a)
// of a face's first three corners, taken right-handed in engine coordinates, points into the solid
// (the engine's own generated cube winds this way).
struct MassingMesh {
    std::array<MassingVec, 8> v{};
    std::array<std::array<std::uint8_t, 4>, 6> quads{};
};
FTCORE_API void massingMesh(const MassingBox& box, double side, double topNudge, MassingMesh& out);

// The amount (<= 0) each flat box's top comes down so that no two boxes that overlap share a top
// (coplanar faces flicker): among the boxes `include` marks (flat ones only), each pair whose tops are
// within kMassingSharedTop and whose footprints overlap is a conflict; smallest footprint first, each
// takes the lowest level no conflicting box already holds, and sits kMassingNudgeStep per level under
// its stated top. `out` gets one entry per box (0 for the rest). Allocates: build time only.
FTCORE_API void massingTopNudges(const std::vector<MassingBox>& boxes, const std::vector<bool>& include,
                                 std::vector<double>& out);

// The amount (>= 0) each flat box's top goes up, on top of its nudge, so that no top lies on the ground: a top the
// ground was levelled to (a quay's) flickers against it as two boxes' shared tops do. Among the boxes `include` marks
// (flat ones only), a box lies on the ground when its top (z1 + nudges[i], the nudges massingTopNudges gives, a missing
// entry 0) is within kMassingSharedTop of the ground (massingGroundAt) at one of its sample points: a grid over its
// footprint kMassingGroundInset inside its edges (ground meeting a side along a line shares no face with the top),
// about `spacing` apart and at most kMassingGroundSamples a side. Such a box goes up together with the boxes it shares
// a top with (massingTopNudges' conflicts, joined), so their order holds, by the fewest kMassingNudgeStep, up to
// kMassingGroundSteps of them, that leaves none of their tops within kMassingSharedTop of the ground at its sample
// points, nor of the top (nudge and raise taken) of a box outside them whose footprint overlaps it; groups go in the
// order of their first box. A group no such raise clears stays where it is. `out` gets one entry per box (0 for the
// rest; all 0 with no ground: an empty grid or a spacing that isn't positive). Returns the number of groups left on
// the ground. Allocates: build time only.
inline constexpr double kMassingGroundInset = 0.05;
inline constexpr std::size_t kMassingGroundSamples = 65;
inline constexpr int kMassingGroundSteps = 5;
FTCORE_API std::size_t massingGroundRaises(const std::uint16_t* grid, std::size_t samplesX, std::size_t samplesY,
                                           double spacing, double originX, double originY,
                                           const std::vector<MassingBox>& boxes, const std::vector<bool>& include,
                                           const std::vector<double>& nudges, std::vector<double>& out);

// Whether two boxes' footprints overlap with positive area (touching edges don't).
FTCORE_API bool massingFootprintsOverlap(const MassingBox& a, const MassingBox& b);

// How far a plan point lies outside a box's footprint, in metres: 0 on its edge or inside it.
FTCORE_API double massingFootprintGap(const MassingBox& box, double x, double y);

// How a finished mesh stands in for a box, in the engine frame: the actor's location (m), its scale on
// each of the mesh's own axes, and its engine yaw (degrees).
struct MassingKitPlace {
    MassingVec at;
    MassingVec scale{1.0, 1.0, 1.0};
    double yaw = 0.0;
};

// Places a mesh in a box's stead, whatever the mesh's own pivot, from its bounds in its own axes (m). The
// bounds' centre lands over the box's centre at the box's rotation, turned turnDeg more for a mesh whose
// long axis lies across the box's +u, and their bottom on the box's lowest corner (massingCorners), raised
// by dz. With fitFootprint the mesh is scaled on each axis to the box's footprint and height, an odd
// quarter turn trading the two footprint sides; without it the mesh keeps its size. False, leaving `out`
// alone, on bounds that are inverted or, with fitFootprint, without size on an axis.
FTCORE_API bool massingKitPlacement(const MassingBox& box, double side, const MassingVec& meshMin, const MassingVec& meshMax,
                                    bool fitFootprint, double turnDeg, double dz, MassingKitPlace& out);

// The world's 1 m height field (a raw grid of 16-bit samples, south-west corner first, rows running
// north) as an Unreal Landscape takes it: a square whose side is the smallest quadsPerComponent * n + 1
// that holds both of the grid's sides, the grid's last column and row repeated east and north to fill
// it, rows written north first (the engine frame's -Y first).
FTCORE_API std::size_t massingLandscapeSide(std::size_t samplesX, std::size_t samplesY,
                                            std::size_t quadsPerComponent);
// `out` holds side * side samples. False when a size is zero or the grid doesn't fit.
FTCORE_API bool massingLandscapeHeights(const std::uint16_t* grid, std::size_t samplesX, std::size_t samplesY,
                                        std::size_t side, std::uint16_t* out);
// The engine-frame position (metres, z 0) of the Landscape's first sample: the padded square's
// north-west corner, for a grid whose south-west sample lies at (originX, originY) in the plan frame.
FTCORE_API MassingVec massingLandscapeCorner(double squareSide, double originX, double originY, double spacing,
                                             std::size_t side);

// Heights the way Core's ground stores them (Terrain.h heightmapValue): a 16-bit Landscape at Z scale 100.
inline double massingHeightFromValue(std::uint16_t value) { return (static_cast<double>(value) - 32768.0) / 128.0; }

// The ground at a plan point (metres): bilinear between the grid's samples (south-west first, rows
// running north, `spacing` apart from (originX, originY)), clamped to the grid's edge. 0 for an empty
// grid, a spacing that isn't positive or a point that isn't finite.
FTCORE_API double massingGroundAt(const std::uint16_t* grid, std::size_t samplesX, std::size_t samplesY,
                                  double spacing, double originX, double originY, double x, double y);

// What a player dropped at a plan point stands on: the highest of `ground` and the stated tops (a
// sloped top read along u, a zone's raised by kMassingZonePlate, nudges left out) of the boxes
// `include` marks whose footprints hold the point, among those no higher than `ceiling`. `onBox`, when
// given, gets the index of the box it stands on, or -1 for the ground. A box past the end of `include`
// is left out. No allocation.
FTCORE_API double massingFloorAt(const std::vector<MassingBox>& boxes, const std::vector<bool>& include, double x,
                                 double y, double ground, double ceiling, int* onBox = nullptr);

// A box that a player of `height` standing at a plan point on `floor` would stand inside: the first of
// the boxes `include` marks whose footprint holds the point and whose solid there (bottom to top, read
// as massingFloorAt reads tops) reaches over floor + kMassingStandClear and under floor + height. -1
// for none. No allocation.
inline constexpr double kMassingStandClear = 0.05;
FTCORE_API int massingBlockingAt(const std::vector<MassingBox>& boxes, const std::vector<bool>& include, double x,
                                 double y, double floor, double height);

// Whether a spawn's floor holds a player set down on it: the largest step, in metres, between `floor` (what the player
// stands on at the plan point, massingFloorAt's answer there) and the floor at kMassingFootingPoints sample points
// around it: the point itself, then eight a half `radius` out and eight a whole `radius` out, east first and turning
// counter-clockwise. Each point reads the ground (massingGroundAt) and the tops of the boxes `include` marks no higher
// than `ceiling` (massingFloorAt). A point where such a box stands in the way of a player of `height` on `floor`
// (massingBlockingAt) is a wall the player can't step into, not a floor to stand on, so it is skipped and counted: a
// wall a quarter metre from a spawn is no fault of its footing, while the ground under that wall can lie well below.
//
// A step over kMassingFootingTolerance means the spawn sits on a steep bank's toe or a box's edge: the player slides off
// it, and the navmesh, which leaves such a slope out, has nothing under the point, so a path asked to it finds none.
// Spawns that hold in play show a few centimetres here and leave the player's feet within about a quarter metre of
// their floor; the tolerance sits over that band. The radius is about the player's own and the reach a navmesh query
// takes sideways by default: a spawn 0.7 m from its landing's edge holds in play, so a full metre would flag it.
// A grid massingGroundAt reads as empty gives a flat ground at 0. No allocation.
inline constexpr int kMassingFootingPoints = 17;
inline constexpr double kMassingFootingRadius = 0.5;
inline constexpr double kMassingFootingTolerance = 0.5;
struct MassingFooting {
    double step = 0.0;       // the largest |floor there - floor|, m (0 when every point is skipped: a spawn
                             // inside a box is massingBlockingAt's to find)
    double x = 0.0, y = 0.0; // the sample point it is at, plan frame (the spawn when there's no step)
    int skipped = 0;         // sample points a box stands in the way at
};
FTCORE_API MassingFooting massingSpawnFooting(const std::uint16_t* grid, std::size_t samplesX, std::size_t samplesY,
                                              double spacing, double originX, double originY,
                                              const std::vector<MassingBox>& boxes, const std::vector<bool>& include,
                                              double x, double y, double floor, double ceiling, double height,
                                              double radius = kMassingFootingRadius);
inline bool massingFootingHolds(const MassingFooting& footing) { return footing.step <= kMassingFootingTolerance; }

// ---- The photographed look (-Surfaces): which surface a box wears, and the road mask from a street table ----

// The class of photographed surface a built box wears. Kerbs and the other added walls are pole concrete; flights,
// toe stairs, ramps, landings and terraces paving slab; cliff cover plates, the seawall, breakwater and quay board
// concrete; decks, piers, boats and bridges oak plank; cranes, masts, posts, barriers and gates galvanised steel. Every
// other built box is a building, wearing one of three finishes (soot brick, breeze block, aluminium cladding) chosen by a
// stable hash of its row, so that the parts of one building share it. A box the build doesn't make is board concrete.
enum class MassingLookSurface : std::uint8_t {
    PoleConcrete,
    PavingSlab,
    BoardConcrete,
    OakPlank,
    GalvSteel,
    BuildingA, // soot brick
    BuildingB, // breeze block
    BuildingC, // aluminium cladding
};
inline constexpr std::size_t kMassingLookSurfaceCount = 8;
// Decided from the box's src, kind, id prefix and row alone. No allocation.
FTCORE_API MassingLookSurface massingLookSurface(const MassingBox& box);
// The surface's id without its world's prefix: "pole_concrete", "paving_slab", ..., "soot_brick".
FTCORE_API std::string_view massingLookSurfaceId(MassingLookSurface surface);

// A street: its id, its width in metres, its polyline and its kind.
struct MassingStreet {
    std::string id;
    double width = 0.0;             // m, > 0
    std::vector<MassingVec> points; // z is left 0; at least one
    std::string kind{};             // empty when the table gives none
};
// Reads a street table (CSV, a header row naming the columns; comment rows as in importMassingBoxes). The columns read:
//   id       unique; as a massing id
//   width_m  the street's width in metres, > 0
//   pts      "x:y[:z];x:y[:z];..." in the plan frame, metres (z is read and dropped)
//   kind     optional: empty, or a-z, A-Z, 0-9, '_' or '-'
// Any other column is skipped, never kept. `out` is cleared first. False, with a reason naming the line, on any error.
// Allocates: build time only.
FTCORE_API bool importMassingStreets(std::string_view csv, std::vector<MassingStreet>& out, std::string* error = nullptr);
// Moves every street's points from the plan frame of a square `side` metres across to the engine frame
// (massingEnginePoint).
FTCORE_API void massingStreetsToEngine(std::vector<MassingStreet>& streets, double side);

// The streets as a mask: size x size bytes, row iy then column ix, pixel (ix, iy) centred at (-sideM/2 + (ix + 0.5)
// sideM/size, -sideM/2 + (iy + 0.5) sideM/size) in the streets' own frame (the engine's, after massingStreetsToEngine),
// as ftcore::roadMask lays out its own. Each byte is the max over the streets of round(255 clamp(0.5 + (width/2 - d) /
// (2 feather), 0, 1)), d the distance from the pixel's centre to the street's polyline: 255 inside by feather, about 128
// at the edge, 0 from feather outside it. The outermost ring is 0, so a clamped read past the mask finds no street.
// Writes all size * size bytes; each segment touches only the pixels it reaches. No allocation.
inline constexpr double kMassingRoadFeather = 2.0; // m: what the ground master decodes its road mask by
FTCORE_API void massingRoadMask(const std::vector<MassingStreet>& streets, std::size_t size, double sideM, double feather,
                                std::uint8_t* out);

} // namespace ftcore
