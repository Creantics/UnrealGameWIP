#pragma once

// Drivable roads for a massing map: a straight road with a crowned carriageway, kerbs, pavements, dropped-kerb lane
// mouths, a signalised crossroads with a stub arm and rounded corners, zebra crossings, stop and lane lines, parking
// bays, a bus stop, a guard rail, utility poles and wires, signs, street lamps and parked vehicles. Plain C++20; plan
// frame, metres (MassingPlan.h). Everything comes out as solids (a MassingBox, a shape and a palette tint) the map's
// builder makes with the engine's cube and cylinder, and lamps as props (MassingProp) it places as it places a prop
// table's. Left-hand traffic: a vehicle keeps to the left of its way. Allocates: build time only.

#include "ftcore/Api.h"
#include "ftcore/MassingDress.h"
#include "ftcore/MassingPlan.h"
#include "ftcore/MassingProps.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

struct RoadGraph; // ftcore/RoadGraph.h

// Road section standards (VAST_CITY_PLAN.md section 17).
inline constexpr double kRoadLaneWidth = 3.5;       // m: one traffic lane (3.0 to 3.5)
inline constexpr double kRoadKerbRise = 0.15;       // m: kerb face over the gutter, mountable at low speed
inline constexpr double kRoadKerbWide = 0.15;       // m: the kerb stone's top
inline constexpr double kRoadCrossfall = 0.02;      // the carriageway falls this much per m from its crown to each kerb
inline constexpr double kRoadSurfaceLift = 0.03;    // m: the gutter over the road's ground (no z-fighting)
inline constexpr double kRoadSlabDepth = 0.3;       // m: every surface slab's depth under its top
inline constexpr double kRoadMaxGrade = 0.08;       // the steepest grade along a road
inline constexpr double kRoadKerbClear = 0.5;       // m: nothing stands nearer the kerb face than this
inline constexpr double kRoadCornerRadius = 6.0;    // m: a road-to-road corner's kerb radius, at least
inline constexpr double kRoadMouthFlare = 3.0;      // m: a lane mouth's dropped kerb runs this far past each lane edge
inline constexpr double kRoadLine = 0.15;           // m: centre, edge and bay lines
inline constexpr double kRoadCentreDash = 5.0;      // m: centre line dash and gap
inline constexpr double kRoadStopLine = 0.45;       // m: a stop line's width
inline constexpr double kRoadStopBack = 2.0;        // m: a stop line stands this far before its zebra
inline constexpr double kRoadZebraBar = 0.45;       // m: a zebra bar and the gap after it
inline constexpr double kRoadZebraWide = 4.0;       // m: a zebra's width along the road it crosses
inline constexpr double kRoadZebraOff = 2.0;        // m: a zebra stands this far past the corner's tangent
inline constexpr double kRoadRampDeep = 1.5;        // m: a zebra's dropped-kerb ramp into the pavement
inline constexpr double kRoadBayLong = 6.0;         // m: a parking bay
inline constexpr double kRoadBayDeep = 2.0;         // m: a bay's depth into the pavement
inline constexpr double kRoadBayKeepJunction = 5.0; // m: no bay this near a junction, a zebra or a stop line
inline constexpr double kRoadBayKeepBus = 10.0;     // m: no bay this near a bus stop's pole either way
inline constexpr double kRoadBayKeepEnd = 10.0;     // m: no bay this near a road's end
inline constexpr double kRoadLampSpacing = 25.0;    // m: road lamps each side, staggered
inline constexpr double kRoadLampBack = 1.0;        // m: a road lamp's spot behind the kerb face
inline constexpr double kRoadPoleSpacing = 30.0;    // m: utility poles
inline constexpr double kRoadSignalPoleBack = 0.8;  // m: a signal pole behind the kerb face

// A lane meeting a road's side.
struct RoadMouth {
    std::string lane;  // the lane's id, a massing id
    double at = 0.0;   // m along the road from its first point to the lane's centre line
    double width = 0.0; // m: the lane's width
    bool left = true;  // on the road's left, looking from its first point to its second
    double stub = 0.0; // m: > 0, a signalised crossroads, its stub arm this long on the other side
};

struct RoadBusStop {
    double at = 0.0; // m along the road: the pole
    bool left = false;
};

// A road: a straight carriageway between pavements.
struct RoadSpec {
    std::string id, kind;
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0; // plan frame, its centre line's ends
    double width = 0.0;                            // m, wall to wall: carriageway + 2 pavements
    double pavement = 0.0;                         // m each side
    double lane = kRoadLaneWidth;                  // m: each traffic lane
    double speedKmh = 40.0;
    bool oneWay = false;                           // one way, first point to second
    double ground = 0.0;                           // m: the ground it lies on (level along its length)
    double mouthBack = 0.0;                        // m: lanes end this far behind the pavement's back edge
    double mouthZ = 0.0;                           // m: the lanes' surface there
    bool parkLeft = false, parkRight = false;      // bays on that side
    bool railLeft = false, railRight = false;      // a guard rail along that side's back edge
    bool poleLeft = false, poleRight = false;      // utility poles along that side
    bool closedEnds = false;                       // a barrier and a no-entry sign across each end
    std::vector<RoadMouth> mouths;
    std::vector<RoadBusStop> busStops;
};

// Reads a road table (CSV, a header row naming the columns; '#' comment rows). Columns:
//   id          a massing id
//   kind        a-z, A-Z, 0-9, '_' or '-'
//   width_m     wall to wall, > 2 pavement_m
//   pts         "x:y;x:y", plan frame: the centre line's two ends
//   pavement_m  each side, >= 0
//   lane_m      optional (3.5): each lane, 3.0 to 3.5
//   speed_kmh   optional (40)
//   one_way     optional: 1/true for one way, first point to second
//   ground_z    the road's ground, m
//   mouth_back_m, mouth_z  optional: where the lanes meeting it end behind the pavement, and their surface's height
//   mouths      optional: "lane@at@width@L|R[@stub]" joined by ';' (at along the road from its first point)
//   bus_stops   optional: "at@L|R" joined by ';'
//   parking, rail, poles  optional: "", "L", "R" or "LR": the sides with bays, a guard rail, utility poles
//   ends        optional: "closed" or "open"
// The carriageway (width - 2 pavement) must hold the lanes: two (one each way) or one (one way). `out` is cleared
// first. False with the reason, naming the line, on any error.
FTCORE_API bool importRoads(std::string_view csv, std::vector<RoadSpec>& out, std::string* error = nullptr);

enum class RoadShape : std::uint8_t { Cube, Cylinder };
// A cylinder's axis: up, along the box's u (its rot) or along its w.
enum class RoadAxis : std::uint8_t { Up, AlongU, AlongW };

// The palette the solids wear (roadTint gives the colour).
enum class RoadTint : std::uint8_t {
    Asphalt,
    AsphaltPatch,
    Pavement,
    Kerb,
    LineWhite,
    LineYellow,
    Steel,
    PoleConcrete,
    SignalBody,
    LensRed,
    LensAmber,
    LensGreen,
    SignRed,
    SignBlue,
    SignWhite,
    Glass,
    Wood,
    BarrierRed,
    Wire,
    Tyre,
    CarWhite,
    CarSilver,
    CarBlack,
    CarRed,
    CarBlue,
    CarGreen,
    CarCream,
    TaxiYellow,
    BusGreen,
    Count
};
struct RoadColor {
    float r = 0.f, g = 0.f, b = 0.f, roughness = -1.f; // linear; roughness < 0: the master's
};
FTCORE_API RoadColor roadTint(RoadTint tint);
FTCORE_API const char* roadTintName(RoadTint tint);

enum class RoadVehicleKind : std::uint8_t { Kei, Sedan, Taxi, Van, KeiTruck, Bus, Scooter, Bicycle, Count };
FTCORE_API const char* roadVehicleName(RoadVehicleKind kind); // "kei", "sedan", ..., the tag's suffix
// Length, width and height, m.
FTCORE_API MassingVec roadVehicleSize(RoadVehicleKind kind);

// One piece: a box in the plan frame (its id the actor's label, its kind what it is), the engine shape that stands in
// for it, its tint, whether it collides, and a tag ("" none): FTVehicle_<kind> on every part of a vehicle, FTSignal on a
// signal's lenses, FTRoadSign on a sign's face.
struct RoadSolid {
    MassingBox box;
    RoadShape shape = RoadShape::Cube;
    RoadAxis axis = RoadAxis::Up;
    RoadTint tint = RoadTint::Asphalt;
    bool collide = false;
    std::string tag;
};

struct RoadPoint {
    double x = 0.0, y = 0.0;
};
// A quad in the plan frame, counter-clockwise.
struct RoadQuad {
    std::string id;
    std::array<RoadPoint, 4> pts{};
    std::string street, junction; // a network's zebra: the street it crosses and the junction it serves ("" none)
};

// A parking bay: its quad and its side, a vehicle fits inside it with nothing over its edge into the carriageway.
struct RoadBay {
    std::string id;
    MassingFoot foot; // the bay's rectangle
    double at = 0.0;  // m along its road
    double z = 0.0;   // m: its surface
    double heading = 0.0; // plan degrees: the way its lane runs (left-hand traffic)
    bool left = false;
    std::string street; // a network's bay: its street ("" a road's)
};

// A signalised junction: its phases (A: the road's own traffic; B: the arms crossing it), its box and its crossings.
struct RoadJunction {
    std::string id;
    double x = 0.0, y = 0.0; // the centre
    std::string road, lane;  // the road and the lane meeting it
    MassingFoot box;         // the carriageway inside the corners
};

struct RoadVehicle {
    std::string id;
    RoadVehicleKind kind = RoadVehicleKind::Sedan;
    double x = 0.0, y = 0.0, z = 0.0; // plan frame: the centre of its foot on the ground
    double rot = 0.0;                 // plan degrees: the way its nose points
    RoadTint body = RoadTint::CarWhite;
    std::string where;                // its bay's id, or the street it stands along
};
// Its foot on the plan (length along rot).
FTCORE_API MassingFoot roadVehicleFoot(const RoadVehicle& v);
// Its stand-in: a body, a cabin (glass for a two-wheeler's nothing), wheels as cylinders along w, all tagged
// FTVehicle_<kind>, no collision on the wheels. Appends to `out`.
FTCORE_API void roadVehicleSolids(const RoadVehicle& v, std::vector<RoadSolid>& out);

struct RoadCounts {
    std::size_t solids = 0, collide = 0, markings = 0, zebras = 0, bays = 0, lamps = 0, signals = 0, signs = 0,
                poles = 0, mouths = 0, junctions = 0;
};

// Everything a road builds.
struct RoadBuild {
    std::vector<RoadSolid> solids;
    std::vector<MassingProp> lamps;      // a TQ-04 street lamp each (face: towards the road's centre line)
    std::vector<RoadBay> bays;
    std::vector<RoadQuad> crossings;     // the zebras
    std::vector<RoadJunction> junctions;
    std::vector<MassingFoot> carriageway; // the carriageway's rectangles: the main one, a stub's, its corners'
    std::vector<MassingFoot> keepOut;    // where nothing parks: the junction boxes, crossings, stop lines and bus stops
    std::vector<MassingFoot> furniture;  // poles, signs, the shelter, the rail: the props keep off them
    std::vector<MassingFoot> band;       // wall to wall: the road's, a stub's, a mouth's dropped kerb
    RoadCounts counts;
};

// Builds a road (appends to `out`). Its surfaces: the carriageway as two slabs falling kRoadCrossfall from the crown to
// the gutters (the ground plus kRoadSurfaceLift), kerbs kRoadKerbRise high, level pavements, each lane mouth a dropped
// kerb sloping from the lane's surface to the gutter, kRoadMouthFlare past each lane edge; each crossroads a stub arm on
// the other side with kRoadCornerRadius kerbs, zebras on the road's two arms and across the stub, stop lines, a signal
// pole at each far-left corner and pedestrian signals at each zebra's ends. Bays (kRoadBayLong by kRoadBayDeep, cut
// into the pavement) fill each parking side's runs clear of the junctions, crossings, mouths, bus stops and ends.
FTCORE_API void roadBuild(const RoadSpec& road, RoadBuild& out);

// Whether a foot comes nearer than `clear` to a carriageway rectangle (massingFeetOverlap with gap `clear`).
FTCORE_API bool roadInCarriageway(const RoadBuild& build, const MassingFoot& foot, double clear);
// Whether a vehicle's foot lies wholly inside the bay (every corner within its rectangle).
FTCORE_API bool roadFitsBay(const RoadBay& bay, const MassingFoot& vehicle);

// A door of a doors table (sol_lookv1_doors_*.csv: rows of kind "door", "x:y:z;x:y:z" from inside out, width_m wide):
// its foot grown by `clear` all round. Rows of other kinds are skipped. False, with the reason, on a bad row.
FTCORE_API bool importRoadDoors(std::string_view csv, double clear, std::vector<MassingFoot>& out,
                                std::string* error = nullptr);

struct RoadParkCounts {
    std::size_t bays = 0, parked = 0, empty = 0, blocked = 0;
};
// Parks vehicles in the bays (appends to `out`): one in about four bays left empty by a hash of `seed` and the bay; the
// kind and body colour by hash (kei, sedan, taxi, van, kei truck); each centred in its bay along the road. A vehicle
// that would leave its bay, enter the carriageway or a keep-out, or come near a door or a foot in `taken` is not parked
// (counted blocked).
FTCORE_API RoadParkCounts roadParkVehicles(const RoadBuild& build, const std::vector<MassingFoot>& doors,
                                           const std::vector<MassingFoot>* taken, std::uint32_t seed,
                                           std::vector<RoadVehicle>& out);

// Two-wheelers (scooters and bicycles) parked along the lanes (streets no wider than `laneUpTo` with two points, inside
// [x0, x1] by [y0, y1]): every `spacing` m along each side, by hash one spot in three, parallel to the lane
// `edgeIn` m in from its edge, and in groups of two to three along the road's back of pavement on the side with mouths.
// A spot near a door, a foot in `taken`, a vehicle placed before it or a road's carriageway or keep-out is skipped.
struct RoadTwoWheelOptions {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    double laneUpTo = 6.0;
    double spacing = 9.0;
    double edgeIn = 0.6;
    std::uint32_t seed = 0;
};
FTCORE_API std::size_t roadTwoWheelers(const std::vector<MassingStreet>& lanes, const RoadTwoWheelOptions& opts,
                                       const RoadBuild* road, const RoadSpec* spec, const MassingGround& ground,
                                       const std::vector<MassingFoot>& doors, const std::vector<MassingFoot>* taken,
                                       std::vector<RoadVehicle>& out);

// Lamps along the lanes so their pools overlap (the night fix): spots every `spacing` m along each side of each lane
// in the box, staggered by half, `edgeIn` m in from the lane's edge, facing its centre line; a spot within `keep` m of
// a lamp in `existing` (plan x, y) or of one placed before it, or whose foot (footX by footY) comes near a door or a
// foot in `taken`, is skipped. Each lamp stands on the ground. Appends to `out`; returns how many.
struct RoadLaneLampOptions {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    double laneUpTo = 6.0;
    double spacing = 12.0;
    double edgeIn = 0.9;
    double keep = 7.0;
    double footX = 0.6, footY = 0.48;
};
FTCORE_API std::size_t roadLaneLamps(const std::vector<MassingStreet>& lanes, const RoadLaneLampOptions& opts,
                                     const MassingGround& ground, const std::vector<RoadPoint>& existing,
                                     const std::vector<MassingFoot>& doors, const std::vector<MassingFoot>* taken,
                                     std::vector<MassingProp>& out);

// The rows of `boxes` a road clears: decks and piers whose footprints overlap its band (or a stub's, or a corner's)
// widened by `margin`. Returns their indices; the builder hides them (they stay in the table, which is positional).
FTCORE_API std::vector<std::size_t> roadClearedBoxes(const RoadBuild& build, const std::vector<MassingBox>& boxes,
                                                     double margin);

// A box row a footbridge may keep over a road: its underside this far over the carriageway's top, at least.
inline constexpr double kRoadBridgeClear = 4.8;
// The highest face a vehicle drives over without a wall: a kerb or pavement face this much over the lane is a fault.
inline constexpr double kRoadFaceMax = 0.03;
// The rows of `boxes` that stand in a carriageway: built rows (massingBuilt) whose footprints overlap the footprint of
// a carriageway slab (a collider of kind asphalt or fill) and that stand over the slab's top by more than kRoadFaceMax
// with an underside lower than kRoadBridgeClear over it. Zone, hidden, open and proxy rows never clash, so after the
// builder hides the clashing rows (kind "underpass") the same call returns none. Indices in table order.
FTCORE_API std::vector<std::size_t> roadBoxClashes(const RoadBuild& build, const std::vector<MassingBox>& boxes);

// A fault where a kerb or pavement meets the carriageway: the piece's id, a plan point and what it is.
struct RoadFace {
    std::string piece;
    double x = 0.0, y = 0.0;
    std::string fault; // "lane <id>", "carriageway <n>" (1-based in build.carriageway) or "step <other piece>"
};
// The kerb and pavement pieces (kinds kerb and pavement) a vehicle meets as a face over kRoadFaceMax:
//   - lane: a lane centre line of `graph` crosses its footprint (the point: the middle of the crossing), unless a
//     carriageway slab (a collider of kind asphalt or fill) under that point stands within kRoadFaceMax of the piece's
//     top (flush) or more than 2 m from it (the lane passes over or under it: a deck);
//   - carriageway: it lies more than 15 cm (a kerb stone) into a carriageway rectangle (build.carriageway) on its level
//     (no slab under the point, or one within 2 m of the piece's top);
//   - step: it overlaps another kerb or pavement piece by more than 15 cm with their tops more than kRoadFaceMax (and
//     less than 2 m) apart there.
// One row per piece and fault. Junction mouths and lane mouths cut the kerbs and pavements, so a built road has none.
FTCORE_API std::vector<RoadFace> roadKerbFaces(const RoadBuild& build, const RoadGraph* graph);
// The boxes of `kerbs` (massingKerbs' pieces of the other streets) with a lane or carriageway fault against `build`:
// -Kerbs leaves them out where those streets cross a road. Indices, in order.
FTCORE_API std::vector<std::size_t> roadKerbsInCarriageway(const RoadBuild& build, const RoadGraph* graph,
                                                           const std::vector<MassingBox>& kerbs);

// ---- Road networks ----

// A street of a road network: a polyline centre line, kerbed and marked like a road, meeting the others at junctions.
struct RoadNetStreet {
    std::string id, kind;
    std::vector<RoadPoint> pts;  // plan frame, at least two
    double width = 0.0;          // m, wall to wall
    double pavement = 0.0;       // m each side, > 0
    double speedKmh = 40.0;
    bool oneWay = false;
    bool deck = false;           // a bridge deck, level at deckZ, on piers, with rails along both backs
    double deckZ = 0.0;          // m: a deck's ground (its gutter stands kRoadSurfaceLift over it)
    bool park = false;           // bays along both sides where they fit (never on a deck)
    std::vector<RoadMouth> mouths; // lanes meeting its sides (at: m along its points; no stubs)
    double mouthBack = 0.0;      // m: a mouth's dropped kerb runs this far behind the pavement
    double mouthZ = 0.0;         // m: the lane's surface at the mouth's back; 0: the pavement's
};
// Whether a roads table is a network table: its header names pts but no ground_z (the ground is the world's).
FTCORE_API bool roadNetTable(std::string_view csv);
// Reads a network table (CSV, a header row; '#' comment rows). Columns:
//   id, kind      as importRoads
//   width_m       wall to wall
//   pts           "x:y;x:y;...", at least two points, plan frame
//   pavement_m    each side, > 0; the carriageway (width - 2 pavement) holds two 3.0 m lanes (one if one way)
//   speed_kmh     optional (40)
//   one_way       optional: 1/true, along its points
//   deck_z        optional: a bridge deck at that ground height
//   parking       optional: 1/true for bays along both sides; empty or 0 none
//   mouths        optional: "lane@at@width@L|R;..." as importRoads, with no stub
//   mouth_back_m, mouth_z  optional, as importRoads
// `out` is cleared first. False, with the reason naming the line, on any error.
FTCORE_API bool importRoadNet(std::string_view csv, std::vector<RoadNetStreet>& out, std::string* error = nullptr);
// As a massing street (z 0), for the road graph.
FTCORE_API MassingStreet roadNetStreet(const RoadNetStreet& street);

struct RoadNetReport {
    std::size_t nodes = 0, edges = 0, junctions = 0, bends = 0, ends = 0, pieces = 0;
    std::size_t steep = 0;   // pieces steeper than kRoadMaxGrade
    std::size_t pokes = 0;   // samples where the ground stands over the road's top (5 cm or more)
    std::size_t floats = 0;  // samples where the ground lies more than 3 m under a road's slab (not a deck)
    std::size_t tight = 0;   // junction arms too short for their zebra and stop line (signals only)
    std::size_t piers = 0;
    std::size_t pruned = 0;  // kerb and pavement pieces left out: a lane across them or a carriageway under them
    double maxGrade = 0.0;   // along any piece
    std::string maxGradeAt;  // the steepest piece: "<edge> (x, y)"
    double maxStep = 0.0;    // m: the largest step where an arm's crowned carriageway meets a junction's level fill
    double maxPoke = 0.0;    // m: the most the ground stands over a road's top
    std::size_t pokesOver = 0; // pokes of more than 0.3 m (noted; the rest are a crown's skin, counted only)
    std::vector<std::string> notes; // one line each: steep pieces, pokes, floats and tight arms (the first few of each)
};
// Builds a road network (appends to `out`). The streets are split at their junctions (roadGraphNet, kRoadGraphSnap).
// Each edge: a crowned carriageway falling kRoadCrossfall to the gutters, kerbs kRoadKerbRise high and level pavements,
// following the ground (massingGroundAt; a deck its deckZ) in straight pieces, level across each junction; edge and
// dashed centre lines. Each junction: a level asphalt fill, kerbs rounded to kRoadCornerRadius between neighbouring arms
// (a ring of kerb and pavement round the outside of a bend). Each junction of three or more arms: a zebra across each
// arm kRoadZebraOff past its corners, dropped-kerb ramps, a stop line before it on the inbound half, a vehicle signal on
// the inbound kerb and pedestrian signals at both ends of each zebra, phases A (the first arm's axis, as
// roadGraphJunctions) and B; ids JN-0001... numbered as roadGraphJunctions numbers them. Bays on parking streets clear
// of the corners and zebras; lamps every kRoadLampSpacing each side, staggered; a deck's piers every 30 m down to the
// ground and a rail along each back edge. Lane mouths as roadBuild's. Slabs deepen where the ground falls away (to 3 m).
// Last, the kerb and pavement pieces roadKerbFaces would find across its own lanes or in its own carriageway (two
// junctions close together) are left out (report: pruned).
FTCORE_API void roadNetBuild(const std::vector<RoadNetStreet>& streets, const MassingGround& ground, RoadBuild& out,
                             RoadNetReport* report = nullptr);

} // namespace ftcore
