#pragma once

// A massing world's build checks (-run=FTBuildMassingWorld) and the tour that walks it: the tour stops a
// -WorldAutoPlay tour visits after its districts, the coverage points that say which navmesh islands are meant to
// be there, the islands themselves, and the read-back of the built Landscape against the height field it was
// made from. Ids, kinds and numbers only: a table's other columns are skipped as each row is read and never kept.
//
//   std::vector<ftcore::TourStop> stops;
//   if (!ftcore::importTourStops(csvText, stops, &error)) ...;         // allocates: build time only
//   ftcore::navIslands(verts, polys, links, islands);                  // allocates: build time only
//   ftcore::classifyNavIslands(islands, homePoly, points, pointPolys, ftcore::kNavIslandBig, summary);
//   ftcore::readBackWindow(...); ftcore::compareHeightsExact(...);     // the heightmap, sample for sample
//   ftcore::readBackSurfacePoints(...); ftcore::compareSurface(...);   // the collision surface between samples
//
// The tour's own judging (judgeEdge, EdgeTally, tourLegCapSeconds) allocates nothing and is safe on a tick.

#include "ftcore/Api.h"
#include "ftcore/MassingPlan.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

// ---- Tour stops ------------------------------------------------------------------------------------------------

// What a tour stop asks of the lead. Reach: walk there on the navmesh. Edge: from a start inside the map, walk to the
// navmesh point nearest a point on the map's edge, then push straight at it to find what stops the lead there. Push:
// from a start, push straight at the target with no navmesh, to see that something holds the lead off it.
enum class TourStopKind : std::uint8_t { Reach = 0, Edge = 1, Push = 2 };
inline constexpr std::size_t kTourStopKinds = 3;

// "reach", "edge" or "push".
FTCORE_API const char* tourStopKindName(TourStopKind kind);

struct TourStop {
    std::string id;                                   // A-Z, 0-9, '_' or '-'
    TourStopKind kind = TourStopKind::Reach;
    double x = 0.0, y = 0.0, z = 0.0;                 // the target, plan frame, m; z is the floor to stand on (reach),
                                                      // the ground at the edge point (edge), unused (push)
    bool hasStart = false;                            // the lead is set down at the start before the leg
    double startX = 0.0, startY = 0.0, startZ = 0.0;  // plan frame, m; startZ is the floor there
    char edge = 0;                                    // 'W', 'S', 'N' or 'E' on an edge stop, 0 otherwise
    bool still = false;                               // a screenshot at the leg's end
};

// Reads a tour table: a header row naming the columns id, kind, x, y, z, start_x, start_y, start_z, edge and still
// (in any order; any other column is skipped), '#' comment rows anywhere, then a row per stop in the order the tour
// walks them. kind is reach, edge or push; x, y, z and the start are plan-frame metres (z must be a number even
// where it is unused); start_x, start_y and start_z are all empty or all numbers, and an edge or push stop needs
// them; edge is W, S, N or E on an edge stop and empty on the others; still is 0 or 1. Ids are unique. False with
// `error` naming the line ("line N: ...") on the first fault; `out` is cleared first. Allocates: build time only.
FTCORE_API bool importTourStops(std::string_view csv, std::vector<TourStop>& out, std::string* error = nullptr);

// A -WorldTourKinds list, kind names separated by ',' or '+': sets bit (1 << kind) for each. An empty list means
// every kind. False on a name that isn't a kind (the mask is then left as it was). No allocation.
FTCORE_API bool parseTourKinds(std::string_view list, std::uint8_t& mask);
inline constexpr std::uint8_t kTourKindsAll = 0x7;
inline bool tourKindIn(std::uint8_t mask, TourStopKind kind) { return (mask >> static_cast<unsigned>(kind)) & 1u; }

// The game time a tour leg may take before it is given up: kTourLegSecondsPerMetre for each metre of the straight
// line from where the leg starts to its target, and never under kTourLegCapFloorSeconds. The lead walks at 4.5 m/s,
// so two seconds a metre leaves room for a path several times the straight line and its stalls. No allocation.
inline constexpr double kTourLegSecondsPerMetre = 2.0;
inline constexpr double kTourLegCapFloorSeconds = 300.0;
inline double tourLegCapSeconds(double straightMetres) {
    const double cap = straightMetres * kTourLegSecondsPerMetre;
    return cap > kTourLegCapFloorSeconds ? cap : kTourLegCapFloorSeconds;
}

// ---- Edge stops: what stopped the lead -------------------------------------------------------------------------

// What a sweep from the lead toward an edge stop's target first hit, within kEdgeProbeMetres.
enum class EdgeHit : std::uint8_t {
    None,       // nothing
    HiddenWall, // an actor tagged FTHiddenWall (the walls table's blockers)
    Built,      // any other actor: a box, a kit piece or its collider
    Landscape,  // the ground itself
};

// The verdict on an edge leg. The first four are causes the edge is meant to have; the last two fail the tour.
enum class EdgeCause : std::uint8_t { Cliff, Water, Drop, BuiltWall, BareHiddenWall, Unjudged };

// "cliff", "water", "drop", "built wall", "BARE HIDDEN WALL" or "UNJUDGED".
FTCORE_API const char* edgeCauseName(EdgeCause cause);
inline bool edgeCauseMet(EdgeCause cause) { return cause != EdgeCause::BareHiddenWall && cause != EdgeCause::Unjudged; }

inline constexpr double kEdgeProbeMetres = 3.0; // how far ahead of the lead the sweep looks
inline constexpr double kEdgeDryBand = 1.0;     // ground past a hidden wall this close to the feet is walkable on
inline constexpr double kEdgeDropMetres = 2.0;  // ground this far under the feet is a drop

struct EdgeProbe {
    EdgeHit hit = EdgeHit::None;
    double hitNormalZ = 1.0;      // the hit's surface normal's z (a Landscape hit)
    double walkableNormalZ = 0.0; // the lead's walkable floor: a Landscape hit steeper than this is a cliff
    bool groundFound = false;     // a down-trace found ground: just past a hidden wall, or (no hit) along the probe
    double groundZ = 0.0;         // the lowest ground those down-traces found, m
    double feetZ = 0.0;           // the lead's feet, m
    bool hasSea = false;
    double seaLevel = 0.0;        // m
};

// The cause: a Landscape hit steeper than walkable is a cliff, an actor that isn't a hidden wall is a built wall. A
// hidden wall, or no hit at all, is judged by the ground past it: under the sea is water, more than kEdgeDropMetres
// under the feet (or no ground found) is a drop. A hidden wall with dry ground within kEdgeDryBand of the feet past
// it is a bare hidden wall, which nothing in the world explains. Anything else is unjudged. No allocation.
FTCORE_API EdgeCause judgeEdge(const EdgeProbe& probe);

// The count of one edge's legs by cause. No allocation.
struct EdgeTally {
    int legs = 0, cliff = 0, water = 0, drop = 0, built = 0, bare = 0, unjudged = 0;
    void add(EdgeCause cause) {
        ++legs;
        switch (cause) {
        case EdgeCause::Cliff: ++cliff; break;
        case EdgeCause::Water: ++water; break;
        case EdgeCause::Drop: ++drop; break;
        case EdgeCause::BuiltWall: ++built; break;
        case EdgeCause::BareHiddenWall: ++bare; break;
        case EdgeCause::Unjudged: ++unjudged; break;
        }
    }
    int met() const { return cliff + water + drop + built; }
};

// ---- Coverage points and navmesh islands -----------------------------------------------------------------------

// A coverage point: a pocket marks a navmesh island apart from district 0's as meant; an on-point must lie on
// district 0's island.
enum class CoverageRole : std::uint8_t { Pocket = 0, On = 1 };

struct CoveragePoint {
    std::string id;                   // A-Z, 0-9, '_' or '-'
    CoverageRole role = CoverageRole::On;
    double x = 0.0, y = 0.0, z = 0.0; // plan frame, m; z the floor there
};

// Reads a coverage table: a header row naming the columns id, role, x, y and z (any order; any other column is
// skipped and never kept), '#' comment rows anywhere, then a row per point. role is pocket or on. Ids are unique.
// False with `error` naming the line on the first fault; `out` is cleared first. Allocates: build time only.
FTCORE_API bool importCoveragePoints(std::string_view csv, std::vector<CoveragePoint>& out,
                                     std::string* error = nullptr);

// A navmesh poly as the builder gathers it: `count` corners from `first` in a shared array of plan-frame points (m).
struct NavPolyRef {
    std::uint32_t first = 0, count = 0;
};
// Two polys that touch (a shared edge or an off-mesh link), indices into the poly array.
struct NavLink {
    std::uint32_t a = 0, b = 0;
};

// A poly's plan area (m², by the shoelace rule, either winding) and its area-weighted centroid; the corners' mean
// when the area is nil. No allocation.
FTCORE_API void navPolyArea(const MassingVec* corners, std::size_t count, double& area, double& cx, double& cy);

struct NavIsland {
    double area = 0.0;           // m², plan
    double cx = 0.0, cy = 0.0;   // plan frame, area-weighted
    double zMin = 0.0, zMax = 0.0;
    std::size_t polys = 0;
    std::size_t firstPoly = 0;   // its lowest poly index
};
struct NavIslandSet {
    std::vector<NavIsland> islands;      // largest first; equal areas by firstPoly
    std::vector<std::uint32_t> islandOf; // each poly's island
    double area = 0.0;                   // every poly's, m²
};

// The navmesh's connected parts (union-find over `links`; a link naming a poly past the end is skipped), each with
// its area, centroid and z range. A poly whose corners run past `corners` counts with no area. Allocates: build time
// only.
FTCORE_API void navIslands(const std::vector<MassingVec>& corners, const std::vector<NavPolyRef>& polys,
                           const std::vector<NavLink>& links, NavIslandSet& out);

// An island over this many square metres that isn't district 0's must be declared by a pocket point.
inline constexpr double kNavIslandBig = 50.0;

enum class NavIslandClass : std::uint8_t {
    Home,       // the island district 0's spawn projects into
    Declared,   // over the size, with a pocket point in it
    Undeclared, // over the size, with no pocket point
    Small,      // the size or under
};

struct CoverageSummary {
    std::vector<NavIslandClass> classes; // one per island
    std::int64_t home = -1;              // district 0's island, -1 when its spawn is off the navmesh
    double homeArea = 0.0;               // m²
    double homePercent = 0.0;            // of the whole walkable area
    std::size_t big = 0;                 // islands over the size besides district 0's
    std::size_t bigDeclared = 0, bigUndeclared = 0;
    std::size_t onPoints = 0, onHome = 0;
    std::size_t pockets = 0;             // pocket rows
    std::size_t pocketsMarking = 0;      // of them, those in an island over the size besides district 0's
    std::size_t pocketsOffMesh = 0;
    std::vector<std::size_t> onMissed;   // on-points (indices into the points) off district 0's island or the mesh
    std::vector<std::size_t> undeclared; // island indices, largest first
};

// Classifies `set`'s islands: `homePoly` is the poly district 0's spawn projects to and `pointPolys[i]` the one
// points[i] projects to (-1: off the navmesh; a list shorter than the points leaves the rest off it). Allocates:
// build time only.
FTCORE_API void classifyNavIslands(const NavIslandSet& set, std::int64_t homePoly,
                                   const std::vector<CoveragePoint>& points,
                                   const std::vector<std::int64_t>& pointPolys, double bigArea, CoverageSummary& out);

// ---- Read-back of the built Landscape --------------------------------------------------------------------------

// Where a height field (the one read back) lies in the Landscape built from another (massingLandscapeHeights' rows:
// north first, the built field's last column and row repeated east and north): the Landscape's vertex rectangle
// (x1, y1)-(x2, y2), inclusive, with x east and y south from the north edge, and the read-back field's first sample
// within the built field. Both fields are grids of samples, south-west first with rows running north.
struct ReadBackWindow {
    std::size_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    std::size_t nx = 0, ny = 0;   // the read-back field's samples
    std::size_t ix0 = 0, iy0 = 0; // its sample (0, 0) in the built field
};

// False with `error` saying why when a size is zero, the spacings differ, the read-back field's origin isn't on the
// built field's grid, or it doesn't lie inside the built field (or the built field inside the Landscape's `landSide`
// square). Allocates only `error`.
FTCORE_API bool readBackWindow(std::size_t landSide, std::size_t builtX, std::size_t builtY, double builtSpacing,
                               double builtOriginX, double builtOriginY, std::size_t readX, std::size_t readY,
                               double readSpacing, double readOriginX, double readOriginY, ReadBackWindow& out,
                               std::string* error = nullptr);

// Fills `land` (ny rows of nx, the north row first, as the Landscape gives the window back) with each sample's
// expected value inverted, so a cell the Landscape read leaves unwritten can never pass for a match. No allocation.
FTCORE_API void readBackSentinel(const std::uint16_t* field, std::size_t nx, std::size_t ny, std::uint16_t* land);

struct HeightCompare {
    std::size_t compared = 0, mismatched = 0;
    double worst = 0.0;                // m, the largest |Landscape - field|
    double worstX = 0.0, worstY = 0.0; // plan frame of the worst sample (the first, on a tie)
};

// Compares every sample of the read-back `field` (nx by ny, south-west first, `spacing` apart from its origin in the
// plan frame) with `land`, the Landscape's window (ny rows of nx, the north row first). No allocation.
FTCORE_API void compareHeightsExact(const std::uint16_t* field, std::size_t nx, std::size_t ny, double spacing,
                                    double originX, double originY, const std::uint16_t* land, HeightCompare& out);

// How a grid cell is cut into two triangles: along (ix, iy)-(ix+1, iy+1), south-west to north-east, or along
// (ix, iy+1)-(ix+1, iy), north-west to south-east. A Landscape's collision cuts each quad along its own local
// (x, y)-(x+1, y+1), and with its rows running south that is north-west to south-east in the plan frame.
enum class CellDiagonal : std::uint8_t { SouthWestNorthEast, NorthWestSouthEast };

// The height (m) at a plan point on the grid's two-triangle surface, cut along `diagonal`, clamped to the grid's
// edge. 0 for an empty grid, a spacing that isn't positive or a point that isn't finite. No allocation.
FTCORE_API double heightOnTriangles(const std::uint16_t* grid, std::size_t samplesX, std::size_t samplesY,
                                    double spacing, double originX, double originY, double x, double y,
                                    CellDiagonal diagonal);

struct SurfacePoint {
    double x = 0.0, y = 0.0; // plan frame, m
};

// A fixed set of `count` points inside the grid's cells, off both diagonals and off the samples: the cells by a
// Halton sequence (bases 2 and 3) over the grid, each point at one of six cell fractions in turn, (0.2, 0.7),
// (0.7, 0.2), (0.35, 0.9), (0.9, 0.35), (0.6, 0.15), (0.15, 0.6), at least 0.1 cell off either diagonal. The same
// grid gives the same points. Empty for a grid under 2 by 2. Allocates: build time only.
inline constexpr std::size_t kReadBackSurfacePoints = 4096;
inline constexpr double kReadBackSurfaceTolerance = 0.005; // m
FTCORE_API void readBackSurfacePoints(std::size_t samplesX, std::size_t samplesY, double spacing, double originX,
                                      double originY, std::size_t count, std::vector<SurfacePoint>& out);

struct SurfaceCompare {
    std::size_t compared = 0, unread = 0, over = 0; // over the tolerance, the unread among them
    double worst = 0.0;                             // m, among those read
    double worstX = 0.0, worstY = 0.0;              // plan frame
};

// Compares `measured[i]` (the built surface's height at points[i], m; not finite when the query found none) with the
// grid's two-triangle surface there. A missing or unread point counts over the tolerance. No allocation.
FTCORE_API void compareSurface(const std::vector<SurfacePoint>& points, const std::vector<double>& measured,
                               const std::uint16_t* grid, std::size_t samplesX, std::size_t samplesY, double spacing,
                               double originX, double originY, CellDiagonal diagonal, double tolerance,
                               SurfaceCompare& out);

} // namespace ftcore
