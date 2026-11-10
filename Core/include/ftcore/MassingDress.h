#pragma once

// Dressing for a massing map: kerbs along the streets that have pavements, a rail, posts, bollards and lamps along the
// seaward edges of its quays, and shop fronts and other pieces set against the faces of its buildings from a table of
// rules. Plain C++20; positions are in the plan frame (MassingPlan.h). Kerbs and the quay's rail, posts and bollards come
// out as boxes (MassingBox) the map's builder makes as it makes the table's; lamps and face pieces come out as props
// (MassingProp, MassingProps.h) it places as it places a prop table's. Allocates: build time only.

#include "ftcore/Api.h"
#include "ftcore/MassingPlan.h"
#include "ftcore/MassingProps.h"
#include "ftcore/WindowFacade.h"
#include "ftcore/WindowInterior.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

// The world's ground as massingGroundAt reads it (an empty grid: a flat ground at 0).
struct MassingGround {
    const std::uint16_t* grid = nullptr;
    std::size_t samplesX = 0, samplesY = 0;
    double spacing = 0.0, originX = 0.0, originY = 0.0;
};

// A box's footprint as a foot (MassingFoot, MassingProps.h). No allocation.
FTCORE_API MassingFoot massingBoxFoot(const MassingBox& box);

// ---- Kerbs ----

inline constexpr double kMassingKerbWidth = 0.2;     // m across
inline constexpr double kMassingKerbHeight = 0.15;   // m its top stands over its line
inline constexpr double kMassingKerbSink = 0.2;      // m its bottom lies under its line
inline constexpr double kMassingKerbStep = 0.5;      // m between the points a kerb is read at, at most
inline constexpr double kMassingKerbLongest = 12.0;  // m: the longest piece
inline constexpr double kMassingKerbFit = 0.03;      // m: the most a floor under a piece may stray from its line
inline constexpr double kMassingKerbGrade = 0.3;     // m per m: the steepest piece (a steeper line is a step)
inline constexpr double kMassingKerbShortest = 1.0;  // m: a shorter piece is left out

// What massingKerbs built and left out, in metres of kerb line.
struct MassingKerbCounts {
    std::size_t streets = 0;   // streets with pavements and length
    std::size_t pieces = 0;
    std::size_t runs = 0;      // unbroken lines: pieces joined end to end at one height count once
    double built = 0.0;
    double crossing = 0.0;     // left out where a carriageway crosses, or inside a bend
    double boxed = 0.0;        // left out where a box stands
    double wet = 0.0;          // left out on a floor at or under dryAbove
    double shortPieces = 0.0;  // left out as pieces under kMassingKerbShortest
};

// Kerbs along both edges of the carriageway (massingCarriageway) of each street with pavements (pavement > 0) and
// length, each segment of its polyline on its own, as boxes of kind "kerb" (source Invented, cover False, row the
// street's id), which massingLookSurface dresses as pole concrete. A kerb's line runs kMassingKerbWidth / 2 outside the
// carriageway's edge; it is read at points at most kMassingKerbStep apart, each standing for its even share of the
// segment. A point's floor is read as a kerb prop's is (massingPlaceProps: the street's floor at the centreline, from
// tops no higher than kMassingPropDeckReach over the ground there, then the point's own, from tops no higher than
// kMassingPropMaxStep over the higher of its ground and the street's floor). A point is left out:
// - crossing: within half the carriageway plus kMassingKerbWidth of another street's polyline, or of another segment
//   of its own street where it falls square onto that segment (strictly between its ends), so a junction's mouth and
//   a bend's inside stay open;
// - boxed: within kMassingKerbWidth / 2 of the footprint of a box `include` marks that stands over its floor (its
//   highest top corner over floor + kMassingStandClear, its bottom under floor + kMassingPropBody);
// - wet: its floor at or under dryAbove.
// The points kept in a row are split evenly into parts no longer than kMassingKerbLongest, and each part into pieces
// along the segment. The kerb's line passes, at the boundary between two points' shares, through their floors' mean;
// where a run starts (and at a row's end) through the end point's floor carried on half a step towards its neighbour's
// (unless that is steeper than kMassingKerbGrade). A piece starts where the last ended, at that boundary's height, and
// takes the next point while the straight line to the next boundary stays within kMassingKerbFit of each of its points'
// floors and no steeper than kMassingKerbGrade, so the kerb's top runs on without a step. A piece that cannot take even
// one point so starts a run instead; a point that fits no line (a step under it) is left out, as is a piece shorter
// than kMassingKerbShortest, and either breaks the run (counts.runs counts the runs). A piece is a box over its points'
// shares at the segment's rotation (its yaw follows the segment), kMassingKerbWidth across, tilted along u to its
// line's grade (riseU), from kMassingKerbSink under its line to kMassingKerbHeight over it (z0, z1 at its middle), id
// "<street>-KL<n>" left of the street's direction or "<street>-KR<n>" right of it, n counting from 1 along each side in
// segment order. A kerb is low enough to step over.
// `out` and `counts` are cleared first. Allocates: build time only.
FTCORE_API void massingKerbs(const std::vector<MassingStreet>& streets, const MassingGround& ground,
                             const std::vector<MassingBox>& boxes, const std::vector<bool>& include, double dryAbove,
                             std::vector<MassingBox>& out, MassingKerbCounts& counts);

// ---- Quay furniture ----

inline constexpr double kMassingQuayProbe = 1.0;      // m out from an edge the water is looked for
inline constexpr double kMassingQuayStep = 1.0;       // m between an edge's points, at most
inline constexpr double kMassingQuayShortest = 2.0;   // m: a shorter run is left bare
inline constexpr double kMassingQuayEnd = 0.15;       // m a run keeps from its edge's ends
inline constexpr double kMassingRailIn = 0.15;        // m in from the edge: the rail's and the posts' line
inline constexpr double kMassingRailTop = 1.1;        // m over the deck: the rail's top
inline constexpr double kMassingRailBar = 0.06;       // m: the rail's thickness and depth
inline constexpr double kMassingRailPost = 0.08;      // m: a post's side
inline constexpr double kMassingRailPostGap = 2.0;    // m between posts, at most
inline constexpr double kMassingBollardIn = 0.5;      // m in from the edge, to a bollard's centre
inline constexpr double kMassingBollardSide = 0.35;   // m
inline constexpr double kMassingBollardHeight = 0.7;  // m
inline constexpr double kMassingBollardGap = 8.0;     // m between bollards, about
inline constexpr double kMassingQuayLampIn = 1.5;     // m in from the edge
inline constexpr double kMassingQuayLampGap = 25.0;   // m between lamps, about

// Whether a box is a deck a quay's furniture may stand on: flat, built and seen (massingVisible), of kind quay, pier,
// seawall or deck. No allocation.
FTCORE_API bool massingQuayDeck(const MassingBox& box);

// What massingQuay built.
struct MassingQuayCounts {
    std::size_t decks = 0;         // decks with at least one run
    std::size_t runs = 0;
    double seaward = 0.0;          // m of edge over the water
    double blocked = 0.0;          // m of that a box stands on
    double railed = 0.0;           // m of rail
    std::size_t posts = 0, bollards = 0, lamps = 0;
    std::size_t lampsBlocked = 0;  // a box stands where the lamp would
    std::size_t lampsCrowded = 0;  // its foot within kMassingPropGap of one taken
};

// Furniture along the seaward edges of each deck `include` marks (massingQuayDeck), its top z1 (flat; the builder
// passes its boxes with their nudges and raises taken). Each of the four edges of its footprint is read at points at
// most kMassingQuayStep apart; a point is seaward when the ground kMassingQuayProbe out from it lies at or under dryAbove
// and no other box `include` marks holds that probe with its top over dryAbove (a pier or a seawall standing on); it is
// blocked when a box `include` marks, other than the deck, holds the point kMassingRailIn in from the edge and stands
// over the deck (its highest top corner over the top + kMassingStandClear, its bottom under the top + kMassingPropBody).
// Seaward points that aren't blocked join into runs along the edge; a run kept within [kMassingQuayEnd, edge length -
// kMassingQuayEnd] and of kMassingQuayShortest or more takes, as boxes of source Invented, cover False and row the
// deck's id:
// - a rail ("<deck>-QR<n>", kind barrier): kMassingRailBar square, along the run on the line kMassingRailIn in, its top
//   kMassingRailTop over the deck;
// - posts ("<deck>-QP<n>", kind post): kMassingRailPost square on the same line from 0.05 m under the deck into the
//   rail, at both ends of the run and evenly between, at most kMassingRailPostGap apart; where two runs meet at a
//   corner their end posts coincide and one is made;
// - bollards ("<deck>-QB<n>", kind post): max(1, floor(L / kMassingBollardGap)) of them for a run L long, at the
//   middles of its even shares, kMassingBollardIn in, kMassingBollardSide square and kMassingBollardHeight high;
// and, for a run of kMassingQuayLampGap / 2 or more, round(L / kMassingQuayLampGap) lamps as props of rule `lampRule`
// (street: the deck's index) at the middles of its even shares, kMassingQuayLampIn in, facing in, z the deck's top
// plus the lamp rule's dz. A lamp is dropped when a box stands where it would (as blocked above) or its foot
// (massingPropFoot with `lamp`) comes within kMassingPropGap of a foot in `taken` (when given), of a bollard's or of a
// lamp's already set. GalvSteel dresses the rail, posts and bollards (massingLookSurface). The rail stands kMassingRailTop
// high, so a walker never passes it. `outBoxes`, `outLamps` and `counts` are cleared first. Allocates: build time only.
FTCORE_API void massingQuay(const MassingGround& ground, const std::vector<MassingBox>& boxes,
                            const std::vector<bool>& include, double dryAbove, const MassingPropRule& lamp,
                            std::size_t lampRule, const std::vector<MassingFoot>* taken,
                            std::vector<MassingBox>& outBoxes, std::vector<MassingProp>& outLamps,
                            MassingQuayCounts& counts);

// ---- Frontage: pieces against the faces of buildings ----

// One rule: an asset set against the faces of buildings.
struct MassingFrontRule {
    std::string id;                 // unique; as a massing id
    std::string asset;              // the mesh's object path; Core only carries it
    std::vector<std::string> kinds; // the kinds of street whose faces it dresses, "open" for a face fronting none;
                                    // empty for every face
    double width = 0.0;             // m along the face, > 0
    double depth = 0.0;             // m out of the face, the whole mesh, > 0
    double height = 0.0;            // m, > 0
    double out = 0.0;               // m it stands out of the face, > 0: when under depth, the rest is sunk in the wall
    double gap = 0.3;               // m to the next piece along the face, >= 0
    double turn = 0.0;              // degrees (engine yaw) added after the mesh's +X is turned out of the face
    double dz = 0.0;                // m over the floor at the face; kMassingPropBody or more makes it an overhead piece
    double weight = 1.0;            // its share of the pick among the rules that may take a place, > 0
    std::size_t run = 1;            // pieces it sets in a row once picked, >= 1
    std::size_t cap = 0;            // the most it places; 0 for no cap
    double scale = 1.0;             // > 0: the map's builder sets the mesh this much larger (and takes its size so)
};

// Reads a frontage table (CSV, a header row naming the columns; comment rows as in importMassingBoxes). The columns:
//   id          unique; as a massing id
//   asset       a static mesh's object path, not empty
//   width_m, depth_m, height_m  > 0
// and optionally, each left empty for its default:
//   out_m       > 0, default depth_m
//   gap_m       >= 0, default 0.3
//   turn, dz    numbers, default 0
//   weight      > 0, default 1
//   run, cap    whole numbers, run >= 1 (default 1), cap >= 0 (default 0)
//   scale       > 0, default 1
//   kinds       street kinds (or "open") joined by ';', each a-z, A-Z, 0-9, '_' or '-'
// Any other column is skipped. `out` is cleared first. False, with a reason naming the line, on any error.
FTCORE_API bool importMassingFront(std::string_view csv, std::vector<MassingFrontRule>& out,
                                   std::string* error = nullptr);

inline constexpr double kMassingFrontMargin = 0.3;  // m a face's pieces keep from its ends
inline constexpr double kMassingFrontStrip = 3.0;   // m out of a face that must stand clear, at least
inline constexpr double kMassingFrontReach = 4.0;   // m: a face fronts a street whose edge lies this near
inline constexpr double kMassingFrontStep = 1.0;    // m the walk along a face moves on when nothing fits
inline constexpr double kMassingFrontSpread = 0.5;  // m: floors along a piece spreading more are steep
inline constexpr double kMassingFrontBack = 0.5;    // m a sunk piece keeps from the box's far face
inline constexpr double kMassingFrontLift = 0.3;    // m a host's bottom may stand over a piece's foot
inline constexpr double kMassingFrontPadReach = 1.2; // m a ground piece's paving pad runs on past its front
inline constexpr double kMassingFrontPadStep = 0.25; // m a pad's reach is cut by while its front is on a carriageway
inline constexpr double kMassingFrontPadTop = 0.03;  // m a pad's top stands over the floor under it
inline constexpr double kMassingFrontPadSink = 0.3;  // m a pad reaches under the floor under it
inline constexpr double kMassingFrontPadGrid = 0.45; // its floor is read this share of its size either side of its middle
inline constexpr double kMassingFrontPadFit = 0.05;  // m: a floor further off the pad's plane leaves the pad out

// What one frontage rule placed, and why the places it was tried at took nothing from it.
struct MassingFrontCounts {
    std::size_t faces = 0;    // faces it set at least one piece against
    std::size_t tried = 0;
    std::size_t placed = 0;
    std::size_t noWall = 0;   // the host is too low over the piece's foot, or stands too high over it
    std::size_t shallow = 0;  // the sunk part would reach within kMassingFrontBack of the host's far face
    std::size_t blocked = 0;  // a box stands in the strip out of the face
    std::size_t inRoad = 0;   // its front on a carriageway
    std::size_t steep = 0;
    std::size_t wet = 0;
    std::size_t crowded = 0;
    std::size_t capped = 0;   // placed, but dropped by the cap
};
struct MassingFrontTotals {
    std::size_t hosts = 0;
    std::size_t faces = 0;          // four per host
    std::size_t dressed = 0;        // faces with at least one piece on the ground layer
    std::size_t streetFaces = 0;    // faces whose middle fronts a street
    std::size_t streetDressed = 0;  // of those, dressed
    std::size_t padsUneven = 0;     // paving pads left out: the floor under them fits no plane (kMassingFrontPadFit)
};

// The room the part of a placed frontage piece outside its face takes on the plan: `out` deep from the face (x, y)
// along its facing, `width` along the face. No allocation.
FTCORE_API MassingFoot massingFrontFoot(const MassingProp& prop, const MassingFrontRule& rule);

// Sets the rules' pieces against the four side faces of each box `hosts` marks (flat ones only), as Wall props: x, y
// the point of the face at the piece's middle, face its outward facing, street the host's index, z the floor plus dz.
// Rules whose dz is under kMassingPropBody make the ground layer, the rest the overhead layer; each layer walks each face
// on its own, from kMassingFrontMargin to kMassingFrontMargin short of its far end.
//
// At each place along a face the face's kind is that of the street, among those whose edge (half its width from its
// polyline) lies within kMassingFrontReach of the point 0.5 m out of the face 1 m on from the place (or the face's end
// short of that), whose polyline's nearest point lies out from the face, with the nearest edge; "open" for none. The
// rules of the layer whose kinds hold it may take the place; one is picked by weight with a stable hash of the host's
// id, the face, the layer and the place's count, then the rest are tried narrowest first. A picked rule sets up to `run`
// pieces in a row, each taking the next place while it passes. A rule takes a place when its piece fits short of the
// face's margin and passes, in order (failing on the first, and counting there):
// - no wall: the host's lowest top corner under floor + dz + height, or its bottom over floor + dz + kMassingFrontLift;
// - shallow: depth - out over the host's depth across the face less kMassingFrontBack;
// - blocked: a box `include` marks, other than the host, whose footprint meets the strip from 0.05 m to max(out,
//   kMassingFrontStrip) out of the face along the piece's width, standing in its band (its highest top corner over
//   floor + dz + kMassingStandClear, its bottom under floor + dz + height);
// - in the road (ground layer only): its front's corners or middle nearer than half a street's carriageway to its
//   polyline;
// - steep (ground layer only): the floors 0.5 m out of the face at the piece's ends and middle spreading more than
//   kMassingFrontSpread;
// - wet: the floor at or under dryAbove;
// - crowded: massingFrontFoot within kMassingPropGap of a foot placed before it on its layer, or on the ground layer of
//   one in `taken` (when given).
// The floor is read 0.5 m out of the face at the piece's middle, from tops no higher than kMassingPropMaxStep over the
// higher of the ground there and the host's bottom. A place no rule takes moves the walk on by kMassingFrontStep; a
// piece set moves it past the piece and its gap.
//
// A rule whose pieces pass its cap (not 0) takes a place, on a second walk of every face, only when a stable hash of the
// place and its id falls under cap / its count on the first, so the others fill in; any still over are thinned to the cap
// evenly (the k-th kept is the floor(k * n / cap)-th) and counted as capped.
//
// When `outPads` is given, each ground-layer piece kept stands on a paving pad, so a street's walk reaches the piece: a
// flat box of source Invented, cover False, kind landing (paving slab, massingLookSurface), row and id "<host>-FP<n>" (n
// counting from 1 per host, in the order the pieces are kept), at the piece's facing, its width plus its gap along the
// face about the piece's middle, from the face out to its reach (out + kMassingFrontPadReach, cut by kMassingFrontPadStep,
// to no less than out, while its front's corners or middle are in the road as above), laid flush on the floor under it:
// the floor is read (from tops no higher than kMassingPropMaxStep over the higher of the ground and the piece's floor) at
// a 3 x 3 grid kMassingFrontPadGrid of its size either side of its middle along u (out of the face) and w (along it), a
// plane is fitted by least squares, and the pad tilts to it (riseU, riseW) from kMassingFrontPadSink under it to
// kMassingFrontPadTop over it (z0, z1 at its middle); a pad with a floor more than kMassingFrontPadFit off its plane (a
// step under it) is left out and counted in totals.padsUneven. When `outFronting` is given it takes a byte per face of every box, at
// box * 4 + face (faces 0..3 the box's +u, +w, -u and -w sides), 1 where the face's middle fronts a street (a host's face
// whose kind there isn't "open"), else 0. `out`, `counts` (one per rule), `totals` and the two optional outputs are
// cleared first. Allocates: build time only.
FTCORE_API void massingFrontage(const std::vector<MassingFrontRule>& rules, const std::vector<MassingStreet>& streets,
                                const MassingGround& ground, const std::vector<MassingBox>& boxes,
                                const std::vector<bool>& include, const std::vector<bool>& hosts, double dryAbove,
                                const std::vector<MassingFoot>* taken, std::vector<MassingProp>& out,
                                std::vector<MassingFrontCounts>& counts, MassingFrontTotals& totals,
                                std::vector<MassingBox>* outPads = nullptr,
                                std::vector<std::uint8_t>* outFronting = nullptr);

// ---- Roof dressing ----

inline constexpr double kMassingRoofSide = 3.0;      // m: a top narrower than this either way is left bare
inline constexpr double kMassingRoofArea = 16.0;     // m^2: a smaller top is left bare
inline constexpr double kMassingRoofHigh = 2.5;      // m: a top lower over the ground at its middle is left bare
inline constexpr double kMassingRoofProbe = 0.3;     // m out of (and in from) an edge its neighbours are looked for
inline constexpr double kMassingRoofShared = 0.5;    // m: a neighbour's top over the roof's less this shares the edge
inline constexpr double kMassingRoofPostGap = 3.0;   // m between a roof rail's posts, at most
inline constexpr double kMassingRoofEdgeIn = 1.0;    // m the roof's clutter keeps from its edges
inline constexpr double kMassingRoofClutterGap = 0.4; // m between two pieces of clutter, at least
inline constexpr double kMassingRoofTankSide = 1.3;  // m
inline constexpr double kMassingRoofTankHeight = 1.8;
inline constexpr double kMassingRoofUnitLong = 0.9;  // m: an air-conditioning unit's long side
inline constexpr double kMassingRoofUnitShort = 0.36;
inline constexpr double kMassingRoofUnitHeight = 0.65;
inline constexpr double kMassingRoofMastSide = 0.05; // m: an aerial's mast and its cross bar
inline constexpr double kMassingRoofBarLong = 1.2;   // m: an aerial's cross bar
// A court: a terrace whose top is the ground (an academy step, a yard), dressed with rails, racks and standards.
inline constexpr double kMassingCourtRackArea = 150.0;     // m^2 of court for each steel rack
inline constexpr std::size_t kMassingCourtRacks = 40;      // racks on one court, at most
inline constexpr double kMassingCourtRackLong = 1.4;       // m: a rack's bar, its two posts at the ends
inline constexpr double kMassingCourtRackDeep = 0.6;       // m: the foot a rack keeps across its bar
inline constexpr double kMassingCourtRackHigh = 2.3;       // m: the bar's top over the court
inline constexpr double kMassingCourtRackPost = 0.08;      // m: a rack post's side, and its bar's
inline constexpr double kMassingCourtStandardArea = 600.0; // m^2 of court for each lamp standard
inline constexpr std::size_t kMassingCourtStandards = 12;  // standards on one court, at most
inline constexpr double kMassingCourtStandardHigh = 8.0;   // m: a standard's pole over the court
inline constexpr double kMassingCourtStandardSide = 0.18;  // m: the pole's side
inline constexpr double kMassingCourtArmLong = 1.2;        // m: the arm out from the pole's top

// What massingRoofs built.
struct MassingRoofCounts {
    std::size_t roofs = 0;    // tops that took a rail or clutter
    double railed = 0.0;      // m of rail
    std::size_t posts = 0, tanks = 0, units = 0, aerials = 0;
    std::size_t bare = 0;     // hosts left bare: sloped, too small or too low
    std::size_t courts = 0;   // court tops that took a rail, a rack or a standard (not in roofs)
    std::size_t racks = 0, standards = 0;
    // With MassingRoofStyle::layouts only.
    std::size_t huts = 0, sheds = 0, frames = 0, stands = 0;
    std::array<std::size_t, 7> layouts{}; // roof hosts (not courts, not bare) per MassingRoofLayout
};

// ---- Roof layouts (round 5, behind MassingRoofStyle::layouts) ----
//
// Each roof host takes one of seven clutter layouts by a stable hash of its id (weights out of 20 in brackets): Empty (3,
// rails only); Tanks (3, two or three tanks, each on a steel stand); UnitFarm (3, a row of air-conditioning units along
// one edge and a tank); StairHead (4, a stair-head hut at a corner, a tank on its roof and one or two units); Laundry (3,
// two or three drying frames and an aerial); ShedMasts (2, a steel shed and two aerials); Dense (2, a hut, a tank on it,
// a tank on a stand, the r4 units and an aerial). The hut ("<host>-RH<n>", kind hut, kMassingRoofHutLong by
// kMassingRoofHutShort by kMassingRoofHutHeight, the kit's BK-HUT size so its mesh swaps in by kind) tries the four
// inset corners first.
enum class MassingRoofLayout : int { Empty = 0, Tanks, UnitFarm, StairHead, Laundry, ShedMasts, Dense };
inline constexpr int kMassingRoofLayouts = 7;
// The roof's own finish, read by the window master's top face (round 5): light concrete, dark tar, green paint, red-oxide
// paint, weathered grey, pale blue-grey.
inline constexpr int kMassingRoofFinishes = 6;
inline constexpr double kMassingRoofHutLong = 4.0;    // m: stair-head hut (BK-HUT 4.0 x 3.0 x 2.5)
inline constexpr double kMassingRoofHutShort = 3.0;
inline constexpr double kMassingRoofHutHeight = 2.5;
inline constexpr double kMassingRoofStandSide = 1.1;  // m: a tank stand's square
inline constexpr double kMassingRoofStandHeight = 0.6;
inline constexpr double kMassingRoofShedLong = 2.4;   // m: a steel shed
inline constexpr double kMassingRoofShedShort = 1.8;
inline constexpr double kMassingRoofShedHeight = 2.1;
inline constexpr double kMassingRoofFrameLong = 2.6;  // m: a drying frame's lines
inline constexpr double kMassingRoofFrameWide = 0.9;  // m: its T bars across
inline constexpr double kMassingRoofFrameHeight = 1.8;
inline constexpr double kMassingRoofFrameSide = 0.05; // m: its posts and bars
inline constexpr double kMassingRoofUnitPitch = 1.2;  // m between units of a farm row

// A stair-head hut's row is its host's look key (the host's row, or its id cut at the last '_'), so it wears the host's
// finish; its kind is "hut" and its id "<host>-RH<n>", where the BK-HUT kit piece may replace it.
struct MassingRoofStyle {
    bool layouts = false; // false: the r4 clutter, byte for byte
};

// The layout and the roof finish (0 .. kMassingRoofFinishes - 1) a roof host's id picks.
FTCORE_API MassingRoofLayout massingRoofLayout(std::string_view hostId);
FTCORE_API int massingRoofFinish(std::string_view hostId);

// Rails and clutter on the flat tops of the boxes `hosts` marks (of those `include` marks, the boxes read as neighbours).
// A host is left bare when it is sloped, either side is under kMassingRoofSide, its top under kMassingRoofArea or its top
// under kMassingRoofHigh over the ground at its middle. Each of its four edges is read at points at most kMassingQuayStep
// apart from kMassingQuayEnd to kMassingQuayEnd short of its far end; a point is shared when, kMassingRoofProbe out of
// the edge, the ground or the top of another box holding the probe (its bottom under the roof) lies over the roof's top
// less kMassingRoofShared (the roof runs on or meets a wall), and covered when a box other than the host holding the
// point kMassingRoofProbe in stands on the roof (its highest top corner over the top + kMassingStandClear, its bottom
// under the top + kMassingRailTop). Free points join into runs; a run of kMassingQuayShortest or more takes a rail
// ("<host>-RR<n>", kind barrier, kMassingRailBar square, on the line kMassingRailIn in, its top kMassingRailTop over the
// roof) and posts ("<host>-RP<n>", kind post, kMassingRailPost square, from 0.05 m under the roof into the rail) at both
// ends and evenly between, at most kMassingRoofPostGap apart (one where two runs' ends coincide).
//
// Clutter stands in the top kept kMassingRoofEdgeIn in from its edges, each piece at the first of eight spots a stable
// hash of the host's id picks there whose foot keeps kMassingRoofClutterGap from the pieces set before it and whose
// middle and corners no box covers: water tanks ("<host>-RT<n>", kind tank, kMassingRoofTankSide square,
// kMassingRoofTankHeight high), one for a top of 60 m^2 or more and a second by the hash, else one by the hash;
// air-conditioning units ("<host>-RU<n>", kind unit, kMassingRoofUnitLong by kMassingRoofUnitShort by
// kMassingRoofUnitHeight, along either side by the hash), 1 + floor(area / 40) of them, 6 at most; and by the hash an
// aerial: a mast ("<host>-RM<n>", kind aerial, kMassingRoofMastSide square, 2.4 to 3.4 m by the hash) and a cross bar
// ("<host>-RX<n>", kind aerial, kMassingRoofBarLong long) 0.3 m under its top. Every piece is a flat box of source
// Invented, cover False, row the host's id; galvanised steel dresses them (massingLookSurface). `out` and `counts` are
// cleared first. Allocates: build time only.
//
// A host `courts` marks (null: none) is a court, its top the ground: it is not left bare for standing under
// kMassingRoofHigh over the ground, takes the rails above, and, in place of tanks, units and an aerial, steel racks and
// lamp standards set the same way: one rack for each kMassingCourtRackArea of its top (kMassingCourtRacks at most), two
// posts ("<host>-CP<n>", kind post, kMassingCourtRackPost square) under a bar ("<host>-CB<n>", kind barrier,
// kMassingCourtRackLong long, its top kMassingCourtRackHigh over the court), along either side by the hash; and one
// standard for each kMassingCourtStandardArea (kMassingCourtStandards at most), a pole ("<host>-CS<n>", kind mast,
// kMassingCourtStandardSide square, kMassingCourtStandardHigh high) and an arm at its top ("<host>-CA<n>", kind barrier,
// kMassingCourtArmLong long). A dressed court counts in courts, not roofs.
FTCORE_API void massingRoofs(const MassingGround& ground, const std::vector<MassingBox>& boxes,
                             const std::vector<bool>& include, const std::vector<bool>& hosts,
                             std::vector<MassingBox>& out, MassingRoofCounts& counts,
                             const std::vector<bool>* courts = nullptr, const MassingRoofStyle* style = nullptr);

// ---- Stand-ins ----
//
// The frame of flat boxes a crane or boat box (the plan's placeholders, until their kit meshes come) is built as, in
// place of one solid. A crane (kind crane): four legs kMassingCraneLeg square at its footprint's corners up to its portal
// (0.4 of its height); beams round the portal's top; a machine house on them (0.6 by 0.8 of the footprint, 0.14 of the
// height); two masts from the house to the box's top at 0.3 of its width either side, a cross-head joining their tops;
// a boom (kMassingCraneBoom deep) along +u, its top at 0.62 of the height, from sx behind the middle to 2.5 sx past the
// +u side; and two ties (tilted boxes, 0.5 m deep) from the cross-head down to the boom's ends. A boat (kind boat), its
// long side along u or w: a hull narrower below (0.7 of the beam, 0.85 of the length, to 0.45 of the height) under the
// full deck, and a cabin on the deck (0.7 of the beam; a quarter of the length, 0.6 past 20 m; 2 m + a tenth of the
// length high) a tenth of the length in from its -long end. Pieces "<box>-S<n>", the box's kind (so they dress as it
// does), source Invented, cover False, row the box's id, appended to `out`. False, `out` untouched, for another kind, a
// sloped box or an empty one. Allocates: build time only.
inline constexpr double kMassingCraneLeg = 1.0;  // m: a crane leg's side, its portal beams' and masts'
inline constexpr double kMassingCraneBoom = 1.6; // m: the boom's depth and width
FTCORE_API bool massingStandIn(const MassingBox& box, std::vector<MassingBox>& out);

// ---- Windows ----
//
// What a building box's interior-mapped windows (WindowInterior.h) need beyond its shape: its seed, which faces front a
// street (their ground storey is shops), and how far over the box's bottom each face's street floor lies, so the grid's
// first storey starts at the street and not under it where a box sinks into a slope. Faces in boxFace order: 0 +u,
// 1 +w, 2 -u, 3 -w.
struct MassingWindowData {
    std::uint32_t seed = 0;       // windowSeed of the id's hash
    std::uint8_t streetBits = 0;  // bit f set: face f fronts a street
    double base[4] = {};          // m over z0: the ground kMassingWindowProbe outside the face's middle, 0 when the
                                  // ground lies under z0 (a box standing on another), at most the box's height
};
inline constexpr double kMassingWindowProbe = 0.5;
// The window data for `box`. `fronting` holds the box's four fronting bytes (massingFrontage's outFronting from
// box * 4), or is null for none. No allocation.
FTCORE_API MassingWindowData massingWindowData(const MassingBox& box, const MassingGround& ground,
                                               const std::uint8_t* fronting);

// ---- Facades (round 5) ----
//
// A windowed box's r5 facade (WindowFacade.h): its window type by height over its street and its footprint, its wall
// tint and its roof finish, all from the id's hash.
//   20 m or more:                       office or walkway block, half and half
//   12 m or more:                       apartment 50, walkway 30, office 20
//   under 12, fronting a street, narrow (shorter side 10 m or less): shophouse 70, apartment 30
//   else:                               apartment 70, shophouse 30
// The tint: any of the ten on a plain finish (render, board concrete, breeze block), else a neutral one. The roof finish
// is massingRoofFinish of the id (as the roof clutter's). No allocation.
struct MassingFacadeData {
    WindowType type = WindowType::Legacy;
    std::uint32_t wallTint = 0;    // windowWallTint index
    std::uint32_t roofFinish = 0;  // windowRoofFinish index
};
inline constexpr double kMassingFacadeTall = 20.0;
inline constexpr double kMassingFacadeMid = 12.0;
inline constexpr double kMassingFacadeNarrow = 10.0;
FTCORE_API MassingFacadeData massingFacadeData(const MassingBox& box, const MassingWindowData& data);

// ---- Shop lights ----
//
// Where a windowed box's shopfronts spill light onto the street at night: along each face whose street bit `data`
// sets, n = ceil(length / spacing) lights (one for a spacing that isn't positive), the k-th at (k + 0.5) / n of the
// face's length, kMassingShopLightOut out of it and kMassingShopLightUp over its street floor (z0 + data.base[f]),
// facing out of it. A face whose shop storey doesn't fit (the light over the box's top) takes none; a sloped box takes
// none. Appended to `out`; the count appended is returned. Allocates: build time only.
inline constexpr double kMassingShopLightOut = 0.4;      // m out of the face
inline constexpr double kMassingShopLightUp = 2.4;       // m over the street floor: under a shopfront's top and fascia
inline constexpr double kMassingShopLightSpacing = 8.0;  // m along the face between two lights, at most
struct MassingShopLight {
    double x = 0.0, y = 0.0, z = 0.0;  // plan frame, m
    double face = 0.0;                 // plan degrees, counter-clockwise from +x: out of the face
    int faceIndex = 0;                 // 0..3 in boxFace order
};
FTCORE_API std::size_t massingShopLights(const MassingBox& box, const MassingWindowData& data, double spacing,
                                         std::vector<MassingShopLight>& out);

} // namespace ftcore
