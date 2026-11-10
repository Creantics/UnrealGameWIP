#pragma once

// Dressing for a massing map: kerbs along the streets that have pavements, a rail, posts, bollards and lamps along the
// seaward edges of its quays, and shop fronts and other pieces set against the faces of its buildings from a table of
// rules. Plain C++20; positions are in the plan frame (MassingPlan.h). Kerbs and the quay's rail, posts and bollards come
// out as boxes (MassingBox) the map's builder makes as it makes the table's; lamps and face pieces come out as props
// (MassingProp, MassingProps.h) it places as it places a prop table's. Allocates: build time only.

#include "ftcore/Api.h"
#include "ftcore/MassingPlan.h"
#include "ftcore/MassingProps.h"

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
inline constexpr double kMassingKerbHeight = 0.15;   // m its top stands over the highest floor along it
inline constexpr double kMassingKerbSink = 0.2;      // m its bottom lies under the lowest
inline constexpr double kMassingKerbStep = 0.5;      // m between the points a kerb is read at, at most
inline constexpr double kMassingKerbLongest = 12.0;  // m: the longest piece
inline constexpr double kMassingKerbRise = 0.08;     // m: the most the floors under one piece may spread
inline constexpr double kMassingKerbShortest = 1.0;  // m: a shorter piece is left out

// What massingKerbs built and left out, in metres of kerb line.
struct MassingKerbCounts {
    std::size_t streets = 0;   // streets with pavements and length
    std::size_t pieces = 0;
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
// The points kept are joined into pieces along the segment, a piece ending before it would pass kMassingKerbLongest or
// its floors would spread more than kMassingKerbRise; a piece shorter than kMassingKerbShortest is left out. A piece is a
// flat box over its points' shares, kMassingKerbWidth across, from kMassingKerbSink under its lowest floor to
// kMassingKerbHeight over its highest, at the segment's rotation, id "<street>-KL<n>" left of the street's direction or
// "<street>-KR<n>" right of it, n counting from 1 along each side in segment order. A kerb is low enough to step over.
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
inline constexpr double kMassingFrontPadTop = 0.03;  // m a pad's top stands over the piece's floor
inline constexpr double kMassingFrontPadSink = 0.3;  // m a pad reaches under the piece's floor

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
// to no less than out, while its front's corners or middle are in the road as above), from kMassingFrontPadSink under the
// piece's floor to kMassingFrontPadTop over it. When `outFronting` is given it takes a byte per face of every box, at
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

// What massingRoofs built.
struct MassingRoofCounts {
    std::size_t roofs = 0;    // tops that took a rail or clutter
    double railed = 0.0;      // m of rail
    std::size_t posts = 0, tanks = 0, units = 0, aerials = 0;
    std::size_t bare = 0;     // hosts left bare: sloped, too small or too low
};

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
FTCORE_API void massingRoofs(const MassingGround& ground, const std::vector<MassingBox>& boxes,
                             const std::vector<bool>& include, const std::vector<bool>& hosts,
                             std::vector<MassingBox>& out, MassingRoofCounts& counts);

} // namespace ftcore
