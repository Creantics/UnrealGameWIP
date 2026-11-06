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

// Whether two boxes' footprints overlap with positive area (touching edges don't).
FTCORE_API bool massingFootprintsOverlap(const MassingBox& a, const MassingBox& b);

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

} // namespace ftcore
