#pragma once

// Stand-in meshes for the strip's new kinds of piece, as compositions of the engine's basic shapes (a cube, a sphere and a
// cylinder, each 1 unit a side and centred), until kit meshes replace them: street and park trees, park shrubs, quay
// railing spans, lamp masts, footbridges (whole, or as their deck, landing, stair and pier pieces), pitched house roofs
// (a simpler one for back houses no street sees), glass towers and the landmark tower. The build instances each part per 250 m cell (FTBuildStandIns.h).
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
    StreetTree, // on a pavement: 5-7 m crown on a clear trunk
    ParkTree,   // in a park, forecourt or precinct: bigger, fuller
    Shrub,      // park planting: a low clump
    Railing,    // a quay railing span, 1.1 m high, len long along rot
    LampMast,   // a quay lamp mast, 8 m, its head reaching along rot
    Footbridge, // a whole beam footbridge, len its span along rot, wid its deck; z its deck's top
    BridgeDeck,    // a footbridge's walking slab: z its underside, top its walking top
    BridgeLanding, // a flat slab at a stair's head or between flights: z, top as the deck's
    BridgeStair,   // one straight flight as a ramp slab rising slope per metre along rot: top its walk at the centre
    BridgePier,    // a square column from z (the ground) up to top (the slab it carries)
    House,      // a pitched roof on a house box: len along the ridge (rot), wid across; z the eaves
    HouseBack,  // the same roof for a back house no street sees: one rolled cube
    GlassTower, // a glass tower: len x wid footprint, floors (30-50) or h
    Landmark,   // the landmark tower: h (250-300) tall, slender
    Count
};
constexpr std::size_t kStandInKinds = static_cast<std::size_t>(StandInKind::Count);
FTCORE_API const char* standInKindName(StandInKind kind);

enum class StandInShape : std::uint8_t { Cube, Sphere, Cylinder, Count };

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
    Lantern, // the landmark's warm lantern
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
};
constexpr std::size_t kStandInPartsMax = 16;

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

} // namespace ftcore
