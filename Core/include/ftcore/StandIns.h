#pragma once

// Stand-in meshes for the strip's new kinds of piece, as compositions of the engine's basic shapes (a cube, a sphere and a
// cylinder, each 1 unit a side and centred) and the build's own tree canopies (standInCanopyMesh: a trunk cylinder plus
// one canopy a tree), until kit meshes replace them: street and park trees, park shrubs, quay
// railing spans, lamp masts, footbridges (whole, or as their deck, landing, stair and pier pieces), pitched house roofs
// (a simpler one for back houses no street sees; standInGableMesh closes their gables, and nothing of a roof reaches
// under its eaves), glass towers and the landmark tower. standInCorridorCheck is the build's reject for a stand-in
// standing in a street's or an alley's way. The build instances each part per 250 m cell (FTBuildStandIns.h).
//
// Frame: the plan's (x east, y north, z up, metres, right-handed; rot counter-clockwise from +x, degrees);
// ftcore::massingEnginePoint takes a point to the engine and the engine's yaw is -yaw. A part's roll turns it about its own
// x axis by the right-hand rule (its +y edge up for a positive roll) and its pitch lifts its +x end; both carry to the
// engine's FRotator unchanged (roll as Roll, pitch as Pitch).
//
// Plain C++20. importStandIns allocates (build time only); standInParts doesn't.

#include "ftcore/Api.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

enum class StandInKind : std::uint8_t {
    StreetTree, // on a pavement: 5-7 m crown on a clear trunk (also a young cherry, kind "cherry")
    ParkTree,   // in a park, forecourt or precinct: bigger, fuller (also an old cherry, kind "cherry_old")
    Shrub,      // park planting: a low clump
    Railing,    // a quay railing span, 1.1 m high, len long along rot
    LampMast,   // a quay lamp mast, 8 m, its head reaching along rot
    Footbridge, // a whole beam footbridge, len its span along rot, wid its deck; z its deck's top
    BridgeDeck,    // a footbridge's walking slab: z its underside, top its walking top
    BridgeLanding, // a flat slab at a stair's head or between flights: z, top as the deck's
    BridgeStair,   // one straight flight as a ramp slab rising slope per metre along rot: top its walk at the centre
    BridgePier,    // a square column from z (the ground) up to top (the slab it carries)
    House,      // a pitched roof on a house box: len along the ridge (rot), wid across; z the eaves
    HouseBack,  // the same roof for a back house no street sees: one gable prism, its ridge along the longer side
    GlassTower, // a glass tower: len x wid footprint, floors (30-50) or h
    Landmark,   // the landmark tower: h (250-300) tall, slender
    Count
};
constexpr std::size_t kStandInKinds = static_cast<std::size_t>(StandInKind::Count);
FTCORE_API const char* standInKindName(StandInKind kind);

// Canopy: a tree's crown and its forking limbs as one mesh (standInCanopyMesh), in the unit box around its fork.
// Gable: a pitched roof's solid as one mesh (standInGableMesh), the unit box's floor up to a ridge along x.
enum class StandInShape : std::uint8_t { Cube, Sphere, Cylinder, Canopy, Gable, Count };

enum class StandInPaint : std::uint8_t {
    Bark,
    Leaf,
    LeafLight,
    LeafDark,
    Steel,
    LampHead,
    Deck,
    Concrete,
    Glass,
    GlassDark,
    Spandrel,
    RoofTile,
    RoofSlate,
    RoofGlazed,
    Shell,   // the landmark's pale skin
    Lantern,  // the landmark's warm lantern
    LeafWarm, // a yellow-olive leaf, the fourth of the leaf greens
    Count
};
constexpr std::size_t kStandInPaints = static_cast<std::size_t>(StandInPaint::Count);

enum class StandInCollide : std::uint8_t { None, Solid, Walk };

// A row of a stand-in point table, in the plan frame.
struct StandInPoint {
    StandInKind kind = StandInKind::StreetTree;
    double x = 0.0, y = 0.0; // the piece's centre (a span's or bridge's middle, a roof's or tower's footprint centre)
    double z = 0.0;          // its foot: the ground, a railing's or mast's quay top, a bridge's deck top, a roof's eaves
    bool hasZ = false;       // false: the build takes the ground's height at (x, y)
    double rot = 0.0;        // degrees, counter-clockwise from +x
    double len = 0.0;        // along rot (0: the kind's own)
    double wid = 0.0;        // across rot (0: the kind's own)
    double h = 0.0;          // a tower's (over its z) or tree's height (0: from floors, or the kind's own)
    double top = 0.0;        // a quay piece's top (a mast's head, a slab's walking top, a pier's head)
    bool hasTop = false;
    double slope = 0.0;      // a railing span's or stair's rise per metre along rot
    double pitch = 0.0;      // a roof's pitch, degrees (0: 25-35 by seed)
    double canopy = 0.0;     // a tree's crown width (0: by seed)
    int floors = 0;          // a glass tower's floors (0: the kind's own)
    float scale = 1.f;       // a tree's or shrub's size, a share of its own (0.6 .. 1.6)
    std::string id;          // the row's id, when the table has one
    std::string tag;         // the row's kind as written, lower case ("street", "cherry_old"); empty without a kind column
};

// One basic shape of a stand-in: its centre from the point (plan metres, already turned by the point's rot), its full
// size along its own axes (metres), its yaw (plan degrees, counter-clockwise), roll and pitch (see the frame above).
struct StandInPart {
    StandInShape shape = StandInShape::Cube;
    StandInPaint paint = StandInPaint::Concrete;
    StandInCollide collide = StandInCollide::None;
    float dx = 0.f, dy = 0.f, dz = 0.f;
    float sx = 1.f, sy = 1.f, sz = 1.f;
    float yaw = 0.f, roll = 0.f, pitch = 0.f;
    std::uint8_t variant = 0; // a Canopy's mesh (0 .. kStandInCanopyVariants - 1)
};
constexpr std::size_t kStandInPartsMax = 16;
// A roof part (House, HouseBack) reaches no further under its eaves than this: its halves' overhang, never a gable.
constexpr double kStandInEaveDrop = 0.5; // m

// The tree canopies: a crown of small, offset, lumpy clumps with gaps between them, and the limbs that fork up into
// them from the trunk's top. Variant 0 is the street tree's (5 clumps, one fork), 1 and 2 the park tree's (7 clumps,
// two forks).
constexpr std::size_t kStandInCanopyVariants = 3;

// A canopy mesh in the unit box: z from -0.5 (the fork, where the trunk ends, at x = y = 0) to 0.5 (the crown's top);
// x and y in [-0.5, 0.5] around the fork, reaching 0.5 in size on each axis on at least one side. One outward normal
// per triangle (normals), and one per vertex (vertexNormals: the area-weighted mean of its triangles', unit length) to
// shade a clump or a limb round: no clump shares a vertex with another, so the crown's clumps stay apart. Slot 0 the
// leaves, 1 the limbs' bark. Build time only (it allocates).
struct StandInCanopyMesh {
    std::vector<float> positions;         // x, y, z per vertex
    std::vector<std::uint32_t> triangles; // three vertex indices per triangle
    std::vector<float> normals;           // x, y, z per triangle, outward
    std::vector<std::uint8_t> slots;      // per triangle: 0 leaves, 1 bark
    std::vector<float> vertexNormals;     // x, y, z per vertex, outward; empty: a flat-shaded mesh
};
// Fills out (cleared first); false for a variant out of range.
FTCORE_API bool standInCanopyMesh(std::size_t variant, StandInCanopyMesh& out);
// The gable mesh in the unit box: a triangular prism, its floor the box's (z -0.5, x and y in [-0.5, 0.5]) and its
// ridge along x at y 0, z 0.5; two slopes, two gable ends and the floor, 8 triangles, slot 0, flat (vertexNormals left
// empty). Scaled (len, span, rise) it is a roof of any pitch standing on its eaves. Build time only (it allocates).
FTCORE_API void standInGableMesh(StandInCanopyMesh& out);
// The share of a canopy's height under its crown (its bare limbs, from the fork up to its lowest leaf); 0 out of range.
FTCORE_API double standInCanopyForkShare(std::size_t variant);

// Writes the point's parts into out (room for kStandInPartsMax) and returns how many; 0 for a null out. Seeded by the
// point's position and kind (FNV-1a), so repeatable. No allocation.
FTCORE_API std::size_t standInParts(const StandInPoint& p, StandInPart* out);

// The kind of a table's kind text (case-insensitive): tree / street_tree / street, park_tree / park / forecourt /
// precinct, shrub / planting, railing / rail / railing_span, mast / lamp / lamp_mast, footbridge / bridge, bridge_deck,
// bridge_landing, bridge_stair, bridge_pier, house / house_roof, house_back / back_house, tower / glass_tower / glass,
// landmark. False for anything else.
FTCORE_API bool parseStandInKind(std::string_view text, StandInKind& out);

struct StandInImportStats {
    std::size_t rows = 0;     // data rows read
    std::size_t kept = 0;     // rows made points
    std::size_t skipped = 0;  // rows of a kind this doesn't place (left for others)
};

// Reads a point table (CSV, '#' comment lines, a header row; columns found by name in any order, unknown ones ignored):
//   kind (or type)                 the row's kind; without the column every row is defaultKind
//   x, y (or cx, cy)               its centre; or x0, y0, x1, y1 for a span (its centre, len and rot from the ends)
//   z (or z0, ground)              its foot (see StandInPoint); empty or absent: the ground's
//   z1 (or eave, eave_z, top)      its top; a roof's eaves, taken as its z
//   rot (or yaw, rot_deg, heading) degrees
//   len (or length, length_m, sx, span), wid (or width, width_m, sy, w), h (or height, height_m), floors, scale, id,
//   slope, pitch (or pitch_deg), canopy (or canopy_m)
//   ridge                          a roof's ridge: u along len (the default) or w along wid (then len and wid swap, rot + 90)
//   back (or hidden)               1 / true: a house is a back house
// Appends to out. False with a line-numbered error on a bad header, a short row or a bad number; rows of a kind it
// doesn't know are skipped (counted). defaultKind StandInKind::Count makes the kind column required.
FTCORE_API bool importStandIns(std::string_view csv, StandInKind defaultKind, std::vector<StandInPoint>& out, std::string* error = nullptr,
                               StandInImportStats* stats = nullptr);

// ---- The corridor rule: no stand-in stands in a street's or an alley's way. ----

// One straight piece of a street's clear way, in the plan frame: its centre line a to b and the half width kept clear
// (half the carriageway where the street has pavements, half its width where it has none).
struct StandInCorridor {
    double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0;
    double half = 0.0;
    std::uint32_t street = 0; // which street of its table (the data rows this read kept, from 0)
    double median = 0.0;      // m: the planted median island along its centre line (0 none)
};
// Reads the clear ways from a street or a road table (CSV, '#' comment lines, a header row naming the columns):
//   width_m     the street's width, wall to wall; a row without one is left out (a canal's row in a road table)
//   pts         "x:y[:z];x:y[:z];..." in the plan frame, its centre line (z dropped; a one-point street gives none)
//   pavement_m  optional: each pavement's width; over 0 and under half the width, only the carriageway is kept clear
//   median_m    optional: the planted median island's width (empty or 0: none)
//   id, kind    optional
// Other columns are ignored, and a row may be shorter than the header as long as it reaches width_m and pts. Rows of
// skipKind (not empty) are left out. Appends a corridor a segment to out and, when ids is given, its street's id a
// corridor to ids. False with a line-numbered error on a header without width_m or pts, or a bad number or point.
// Allocates: build time only.
FTCORE_API bool importStandInCorridors(std::string_view csv, std::vector<StandInCorridor>& out, std::vector<std::string>* ids = nullptr,
                                       std::string_view skipKind = {}, std::string* error = nullptr);

struct StandInCorridorSpec {
    double clearance = 2.4; // m: a part whose underside is this far over the ground hangs clear (eaves, a bridge's deck)
    double slack = 0.05;    // m: a part may reach into a corridor's edge by this much
    double medianMin = 1.5; // m: a tree's trunk may stand on a corridor's median island at least this wide
};

// A part's bounds: a rectangle round it in the plan frame, turned by yaw (a rolled or pitched part's is the one round
// its turned box), and its underside and top over the point's foot.
struct StandInBounds {
    double cx = 0.0, cy = 0.0; // plan metres
    double hx = 0.0, hy = 0.0; // half sizes along and across yaw
    double yaw = 0.0;          // degrees, counter-clockwise from +x
    double bottom = 0.0, top = 0.0;
};
FTCORE_API StandInBounds standInBounds(const StandInPoint& p, const StandInPart& part);
// How far (m) the bounds reach inside the corridor: its half width less the plan distance from the bounds' rectangle to
// its centre line; 0 or less when they stay outside. No allocation.
FTCORE_API double standInCorridorDepth(const StandInBounds& b, const StandInCorridor& c);

struct StandInCorridorHit {
    std::size_t point = 0, part = 0, corridor = 0;
    double depth = 0.0; // m inside the corridor
};
// Checks every point's parts against every corridor. footOverGround: per point, its foot (StandInPoint::z, or the
// ground's where it has none) less the ground's height under it; null takes 0 for all. A part is left out when it is a
// tree's canopy (a crown may spread over a street; its trunk may not stand in one) or when its underside is
// spec.clearance over the ground. A tree's trunk (StreetTree, ParkTree) whose whole bounds stand on a corridor's median
// island (StandInCorridor::median, at least spec.medianMin wide: within median / 2 of its centre line, between its ends)
// is clear of that corridor; the island's gaps at the zebras are not known here. Each part counts once, at the corridor it reaches deepest into, when that is over
// spec.slack. Writes the first hitCap hits (hits may be null with hitCap 0) and returns how many there are in all: the
// build fails on any. No allocation.
FTCORE_API std::size_t standInCorridorCheck(const StandInPoint* points, const double* footOverGround, std::size_t count,
                                            const StandInCorridor* corridors, std::size_t corridorCount, const StandInCorridorSpec& spec,
                                            StandInCorridorHit* hits, std::size_t hitCap);

} // namespace ftcore
