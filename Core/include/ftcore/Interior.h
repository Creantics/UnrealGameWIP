#pragma once

// Enterable rooms: a host box hollowed into a room a lead and a walker can go into through a door in its front.
//
// importRoadRooms reads a doors table (the rows ftcore_blockgen writes: "DOOR-<host>,door,<width>,<pts>" with pts
// "x:y:z;x:y:z", the point 0.32 m out of the front face first and the point 1.68 m in second, and
// "ROOM-<host>,interior_<kind>,0,<four x:y:z corners>") and pairs each ROOM row with its DOOR row by host id.
//
// interiorRoomPlan turns one host box and its door into the pieces of a room, in the plan frame:
// - Shell: the host body less the room (the front wall split into two jambs and a lintel round the door gap, the
//   back and side walls, the mass above the ceiling). Solid.
// - Walk: the room's floor and the door's sill, the pieces the navmesh lays on. Solid.
// - Finish, Skirting, Reveal: thin linings on the shell's inner faces (walls, floor, ceiling, a skirting board round
//   the foot of every wall, the gap's sides and head). No collision. The reveals line the gap from outside it, so the
//   clear opening is exactly gapWidth by gapHeight.
// - Slots: where the dressing stands, by kind, each foot kept kInteriorWalkClear off the walk line (the door's outside
//   point to its inside point to the room's middle). A slot that would come nearer is dropped and counted.
// - Crowd points: the door's outside and inside points, and 2-4 dwell spots in front of the dressing.
// - One light slot under the ceiling's middle.
//
// The front face is the host face the door crosses, picked from the door's outside-to-inside direction, so a host
// may stand at any turn. Plain C++20, no exceptions. Allocates (build time only). The same input gives the same plan.

#include "ftcore/Api.h"
#include "ftcore/CrowdV2.h"
#include "ftcore/HeroCorner.h"
#include "ftcore/MassingPlan.h"
#include "ftcore/MassingProps.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

enum class RoomKind : std::uint8_t { Shop, Eatery, Lobby, Flat };

// "interior_shop", "interior_eatery", "interior_lobby", "interior_flat". False on anything else.
FTCORE_API bool roomKindFromText(std::string_view text, RoomKind& out);
FTCORE_API const char* roomKindText(RoomKind kind);

struct RoadRoom {
    std::string host;           // the host box's id: the DOOR and ROOM rows' id after "DOOR-" / "ROOM-"
    RoomKind kind = RoomKind::Shop;
    double doorWidth = 0.0;     // m, the DOOR row's width_m
    MassingVec doorOut, doorIn; // the DOOR row's two points: 0.32 m out of the front face, then 1.68 m in
    MassingVec corner[4];       // the ROOM row's rectangle (the table's rough room; the plan grows it to the box)
};

// Appends one RoadRoom per ROOM row, in the table's order. A DOOR row with no ROOM row is not a room and is skipped.
// False, with the reason, on a ROOM row with no DOOR row, an unknown kind, a ROOM row without four points or a DOOR
// row without two, or a door width not over 0.
FTCORE_API bool importRoadRooms(std::string_view csv, std::vector<RoadRoom>& out, std::string* error = nullptr);

// The navmesh floor for a door gap: the lead's capsule (radius kInteriorAgentRadius) plus one of Recast's coarsest
// cells (kInteriorNavCell, DefaultEngine.ini CellSize 38 cm) each side. A narrower gap can rasterise shut.
inline constexpr double kInteriorAgentRadius = 0.34;
inline constexpr double kInteriorNavCell = 0.38;
inline constexpr double kInteriorGapMin = 2.0 * kInteriorAgentRadius + 2.0 * kInteriorNavCell; // 1.44 m
inline constexpr double kInteriorCeilingMin = 2.6;  // m of clear height a room needs
inline constexpr double kInteriorSlabMin = 0.15;    // m of the host kept over the ceiling
inline constexpr double kInteriorWalkClear = 0.9;   // m a dressing foot keeps off the walk line
inline constexpr double kInteriorFinish = 0.02;     // m, a lining's thickness
inline constexpr double kInteriorSkirtingHigh = 0.1, kInteriorSkirtingProud = 0.015;
inline constexpr double kInteriorDoorOut = 0.32, kInteriorDoorIn = 1.68; // the crowd door points, m from the face

struct InteriorSpec {
    MassingBox host;            // the box to hollow (level; plan frame)
    MassingVec doorOut, doorIn; // a point out of the front face and one in, at the door's middle (RoadRoom's)
    double gapWidth = 1.6;      // m, the clear opening (at least kInteriorGapMin)
    double gapHeight = 2.2;     // m over the floor
    double floorZ = 0.0;        // the room's finished floor, the door's sill (at or over host.z0)
    double wall = 0.2;          // m, every wall's thickness
    double frontWall = 0.0;     // m, the front wall's thickness (0: wall); a kit shopfront stands in it
    // A kit shopfront fills the gap (placed NoCollide, the shell its collision): the gap is the front's clear opening, so
    // no reveals line it and the sill is walked at the floor, under the front's own threshold.
    bool kitFront = false;
    double ceiling = 3.0;       // m of clear height wanted (less where the host is lower)
    RoomKind kind = RoomKind::Shop;
    std::uint32_t seed = 0;     // breaks the dressing's left-right tie when the door is near the front's middle
};

enum class InteriorPart : std::uint8_t { Shell, Walk, Finish, Skirting, Reveal };
// What a lining shows: the build maps each to a surface of the world library by the room's kind.
enum class InteriorFace : std::uint8_t { None, Wall, Floor, Ceiling, Skirting, Reveal };

struct InteriorBox {
    MassingBox box;
    InteriorPart part = InteriorPart::Shell;
    InteriorFace face = InteriorFace::None;
};

enum class InteriorSlotKind : std::uint8_t { Shelf, Till, Display, Counter, Stool, Table, Pass, PostBoxes, Stair, InnerDoor };
FTCORE_API const char* interiorSlotText(InteriorSlotKind kind);

struct InteriorSlot {
    InteriorSlotKind kind = InteriorSlotKind::Shelf;
    MassingFoot foot;           // plan frame
    double z0 = 0.0, z1 = 0.0;
    double face = 0.0;          // degrees: the way its front looks (plan, counter-clockwise from +x)
};

struct InteriorPlan {
    std::vector<InteriorBox> boxes;
    std::vector<InteriorSlot> slots;
    std::size_t dropped = 0;    // slots left out for coming near the walk line
    MassingVec doorOutside, doorInside;
    std::vector<MassingVec> dwell;
    MassingVec light;
    // The room's frame: its inner floor rectangle (centre, half sizes along the front and into the room, turn) and
    // its clear height. u runs along the front, w into the room.
    MassingFoot room;
    double ceilingTop = 0.0;
    double ux = 1.0, uy = 0.0;  // the front's direction (plan)
    double wx = 0.0, wy = 1.0;  // into the room (plan)
    double gapU0 = 0.0, gapU1 = 0.0; // the clear gap along u, from the room's middle
};

// The plan for one room. False, with the reason: no host face between the door's two points (or the door crosses it
// more than 30 degrees off square), a gap under kInteriorGapMin or reaching within wall + 0.1 m of a corner, a floor
// under the host's foot, a room under 3 m either way, or a clear height under kInteriorCeilingMin or under the gap's
// height plus 0.1 m.
FTCORE_API bool interiorRoomPlan(const InteriorSpec& spec, InteriorPlan& out, std::string* error = nullptr);

// Whether the plan point (x, y) at height z lies inside a box (its turn and z0..z1, the edges counting). No allocation.
FTCORE_API bool interiorBoxHolds(const MassingBox& box, double x, double y, double z);

// The open kit fronts' doorways (DOOR_LEAF_KIT_SPEC: SHOP_KOSHI_54_OPEN and SHOP_GLAZED_81_OPEN with their leaves slid
// open), piece-local: x from the piece's left end seen from outside, the sill and the clear height over the lot floor,
// the depth the front fills behind its face.
struct KitDoorway {
    const char* front;          // the _OPEN kit id
    double x0, x1;              // the clear opening along the piece
    double sill, high;          // the floor line over the lot floor, and the clear height over it
    double deep;                // m the front fills behind its face: the front wall is at least this thick
};
inline constexpr KitDoorway kKoshiDoorway{"SHOP_KOSHI_54_OPEN", 3.06, 5.25, 0.05, 2.25, 0.4};
inline constexpr KitDoorway kGlazedDoorway{"SHOP_GLAZED_81_OPEN", 3.18, 4.92, 0.10, 2.15, 0.3};

// The hero corner's enterable rooms: one per bay both lit and a room bay (HeroCornerSpec bayLit, bayRoom), its host the layout's bay box. The eatery
// bay is an eatery, a lattice front's bay a shop, a glazed front's bay a flats' lobby. Each spec is kitFront, its gap
// the open front's clear opening, its floor the front's sill over the ground. Allocates (the ids).
struct HeroRoom {
    int bay = -1;               // 0-based, south to north
    InteriorSpec spec;
    const KitDoorway* doorway = nullptr;
};
FTCORE_API int heroCornerRooms(const HeroCornerSpec& hero, HeroRoom out[kHeroBays]);

// The rooms as doors-table rows (DOOR-<host> and ROOM-<host>, the strip table's form, importRoadRooms reads them back):
// the frontage and roads keep-clear, the minimap door and the crowd door read the hero doors from these.
FTCORE_API std::string heroCornerDoorRows(const HeroCornerSpec& hero);

// The hero corner's crowd, engine frame of a square `side` metres across (massingEnginePoint): the lane as one street
// (kind "lane", its whole width, no pavement, a point about every 3 m) up its middle from the coast road's closed south
// end (padY0 less kRoadEndClose) over the south apron to laneY1, the alley behind the bays as another (kind "alley",
// bays' backs to padX1) from that end to the road's north one (padY1 plus kRoadEndClose), and a walk-in door per room whose
// plan holds (at the door's outside point, through its inside point, the room's middle inside, the plan's dwell spots).
// Appends to both. The number of doors added. Allocates.
FTCORE_API int heroCornerCrowd(const HeroCornerSpec& hero, double side, std::vector<crowdv2::CrowdStreet>& streets,
                               std::vector<crowdv2::CrowdDoor>& doors);

// A room's dressing from its plan's slots, with the hero corner's props (/Game/Props/HeroCorner). A slot is dressed only
// with pieces that are its own object and fit whole inside its foot (heroItemFootprint's sizes), tiled along its front
// 0.05 m apart and centred, backs flush with the slot's back: shelf units on a shelf run, counters on a counter run,
// crates on a display. Each counter carries bottles, the last one the stock pot and its steam too; goods hang 0.7 m out
// from each dressed shelf run, and a room with any dressing hangs a paper lantern over its middle. A slot no piece is (a till, a stool, a table, the pass,
// post boxes, a stair, an inner door: pieces to make) stays bare and counts in `bare`. No allocation.
struct InteriorDressItem {
    HeroPiece piece = HeroPiece::Crate;
    MassingVec at;                     // plan frame: a standing piece's origin on the floor (heroItemFootprint's), a
                                       // counter's goods on its top, a hanging piece's hook under the ceiling
    double faceX = 0.0, faceY = -1.0;  // plan, unit: the way its front looks
    int slot = -1;                     // the plan slot it stands in or on (-1: the room's own layer)
    MassingBox block;                  // a standing piece's collider (its footprint, the floor to its top); sx 0 for
                                       // a piece on a counter or hanging
};
inline constexpr int kInteriorDressMax = 40;
inline constexpr double kInteriorPieceGap = 0.05;  // m between pieces tiled along a slot
FTCORE_API int interiorDressing(const InteriorPlan& plan, InteriorDressItem* out, int max, int* bare = nullptr);

} // namespace ftcore
