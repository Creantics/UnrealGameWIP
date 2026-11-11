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

// Road section standards.
inline constexpr double kRoadLaneWidth = 3.5;       // m: one traffic lane (3.0 to 3.5)
inline constexpr double kRoadKerbRise = 0.15;       // m: kerb face over the gutter, mountable at low speed
inline constexpr double kRoadKerbWide = 0.15;       // m: the kerb stone's top
inline constexpr double kRoadCrossfall = 0.02;      // the carriageway falls this much per m from its crown to each kerb
inline constexpr double kRoadSurfaceLift = 0.03;    // m: the gutter over the road's ground (no z-fighting)
inline constexpr double kRoadSlabDepth = 0.3;       // m: every surface slab's depth under its top
// A junction corner's pavement pieces (the ring behind a rounded kerb, a bend's outer ring, the apron) are this deep:
// the surface master tells them by it and lays their tiles on the world's grid, so the pieces of a fan share one square
// grid (StreetSurfaceSpec::cornerSlabM). No straight pavement is within 8 mm of it.
inline constexpr double kRoadCornerPaveDeep = kRoadSlabDepth + 0.011;
inline constexpr double kRoadMaxGrade = 0.08;       // the steepest grade along a road
inline constexpr double kRoadKerbClear = 0.5;       // m: nothing stands nearer the kerb face than this
inline constexpr double kRoadCornerRadius = 6.0;    // m: a road-to-road corner's kerb radius, at least
inline constexpr double kRoadCornerApron = 6.0;     // m: a junction corner's apron, the pavement on into the block's corner
inline constexpr double kRoadApronLap = 0.4;        // m: how far an apron reaches back under the pavements
inline constexpr double kRoadApronSquare = 0.26;    // |cos| of the angle between the arms under which a corner gets one
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
    Tactile,    // the ramps' tactile paving (yellow, domed)
    Footway,    // a lane's raised footway and an alley's paving (light concrete pavers; Street 5)
    Water,      // a canal's water (a stand-in plane)
    Gutter,     // the dark gutter strip at a lane kerb's foot
    Drain,      // a gutter's drain grate
    Count
};
struct RoadColor {
    float r = 0.f, g = 0.f, b = 0.f, roughness = -1.f; // linear; roughness < 0: the master's
};
FTCORE_API RoadColor roadTint(RoadTint tint);
FTCORE_API const char* roadTintName(RoadTint tint);
// The world surface master's "Street" value for a tint (its street layer): asphalt and patches 1, pavement 2, kerb 3,
// lines 4, footway 5, tactile kRoadStreetTactile; the rest 0 (none).
inline constexpr int kRoadStreetTactile = 6;
FTCORE_API int roadTintStreet(RoadTint tint);

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
    bool instanced = false; // one of many alike (a network's poles, wires and signs): laid as an instance, not an actor
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
    // roadFootways: the walked tops it laid (footway slabs, drop-kerb ramps, alley paving; riseU set) and the lanes'
    // carriageways (a rectangle per segment). Neither joins band or carriageway, so the props, the clash and clearing
    // passes and the decals don't see them.
    std::vector<MassingBox> footways;
    std::vector<MassingFoot> laneWays;
    RoadCounts counts;
};

// Builds a road (appends to `out`). Its surfaces: the carriageway as two slabs falling kRoadCrossfall from the crown to
// the gutters (the ground plus kRoadSurfaceLift), kerbs kRoadKerbRise high, level pavements, each lane mouth a dropped
// kerb sloping from the lane's surface to the gutter, kRoadMouthFlare past each lane edge; each crossroads a stub arm on
// the other side with kRoadCornerRadius kerbs, zebras on the road's two arms and across the stub, stop lines, a signal
// pole at each far-left corner and pedestrian signals at each zebra's ends. Bays (kRoadBayLong by kRoadBayDeep, cut
// into the pavement) fill each parking side's runs clear of the junctions, crossings, mouths, bus stops and ends.
FTCORE_API void roadBuild(const RoadSpec& road, RoadBuild& out);

// Where a tilted road cube's centre goes (plan x, y and z, m) so its top face is centred over the box's (cx, cy) on its
// plane: massingCubePose turns the cube, and its top's middle then stands off the plumb line by half its height times
// the tilt, which opens a slit between a crowned carriageway's two halves and a seam between pieces of unequal depth.
// A level box: (cx, cy, z0 + height / 2).
FTCORE_API MassingVec roadCubeCentre(const MassingBox& box, double height);

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
    // roadFootways' build: a spot on a footway stands on its top (roadFootwayTop), not the ground; a spot on a lane's
    // carriageway (its laneWays) is skipped.
    const RoadBuild* footways = nullptr;
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
    // roadFootways' build: on a kerbed or paved street (roadFootClass not None) a spot must stand wholly on a footway
    // (its foot's corners and centre, roadFootwayTop) and stands on the highest top there; spots off the footways (a
    // carriageway, a lane's kerbless side) are skipped. A stair's lamps stand on the ground as before.
    const RoadBuild* footways = nullptr;
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
// Then roadCrossingOverlaps' rows, so the road check rejects them with the faces.
FTCORE_API std::vector<RoadFace> roadKerbFaces(const RoadBuild& build, const RoadGraph* graph);
// Two crossings (build.crossings) whose quads overlap in plan by more than 5 cm: one row per pair, piece the first's
// id, fault "crossing <the other's id>", the point between their centres. A scramble's two diagonals (ids -ZD, the same
// junction) cross by design and are not a pair: the second's bars stop at the first's band (roadNetBuild).
FTCORE_API std::vector<RoadFace> roadCrossingOverlaps(const RoadBuild& build);
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
    bool poleLeft = false, poleRight = false; // utility poles and wires along that side (left: of its points' way)
    std::vector<RoadPoint> scramble; // its junctions run as scrambles: an all-walk phase W and diagonal zebras (a point
                                     // within kRoadGraphSnap of the junction's centre names it)
};
// Whether a roads table is a network table: its header names pts but no ground_z (the ground is the world's).
FTCORE_API bool roadNetTable(std::string_view csv);
// Reads a network table (CSV, a header row; '#' comment rows). A row of kind canal is a canal (RoadCanal), not a
// street: pts its two plan corners "x0:y0;x1:y1", optional column canal "floor_z:water_z[:bridge_at]"; it goes to
// `canals` (skipped when that is null). Columns:
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
//   poles         optional: L, R or LR, utility poles with wires along that side (left of the way its points run)
//   scramble      optional: "x:y;..." the junctions on it that run as scrambles (roadNetBuild)
// `out` is cleared first. False, with the reason naming the line, on any error.
struct RoadCanal;
FTCORE_API bool importRoadNet(std::string_view csv, std::vector<RoadNetStreet>& out, std::string* error = nullptr,
                              std::vector<RoadCanal>* canals = nullptr);
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
    std::size_t aprons = 0;  // near-square junction corners whose pavement carries on into the block's corner
    std::size_t scrambles = 0; // signalised junctions run as scrambles
    double maxGrade = 0.0;   // along any piece
    std::string maxGradeAt;  // the steepest piece: "<edge> (x, y)"
    double maxStep = 0.0;    // m: the largest step where an arm's crowned carriageway meets a junction's fill (crowned too: 0)
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
// A near-square corner (kRoadApronSquare) carries its pavement kRoadCornerApron on into the block's corner where the
// ground lets the slab sit (report: aprons).
// A scramble (a signalised junction a street's `scramble` names): its pedestrian signals on phase W (all-walk) and up
// to two diagonal zebras (ids <junction>-ZD<n>, kRoadZebraWide wide) between its corners and straight kerbs across the
// fill, each 25-65 degrees to every arm, in pieces tilted to the fill and 6 mm over it; where the two cross, the first's
// bars show and the second's stop at its band; kerbs as any junction's.
// Last, the kerb and pavement pieces roadKerbFaces would find across its own lanes or in its own carriageway (two
// junctions close together) are left out (report: pruned).
FTCORE_API void roadNetBuild(const std::vector<RoadNetStreet>& streets, const MassingGround& ground, RoadBuild& out,
                             RoadNetReport* report = nullptr);

// ---- Canal edge: railing, mast and footbridge points (placed by the stand-in builder, not modelled here) ----

inline constexpr double kQuayRailHigh = 1.1;   // m: a railing span's top over its foot
inline constexpr double kQuaySpanMax = 2.0;    // m: a railing span's length, at most
inline constexpr double kQuayRailIn = 0.15;    // m: the rail's line back from the water's edge
inline constexpr double kQuayPost = 0.08;      // m: a run's closing post (a bridge_pier row) side
inline constexpr double kQuayMastHigh = 8.0;   // m
inline constexpr double kQuayMastPlate = 0.3;  // m: a mast's base plate side; the rail breaks round it
inline constexpr double kQuayMastClear = 1.0;  // m a mast keeps from the road's furniture feet
inline constexpr double kQuayBridgeWide = 2.4; // m: the footbridge's walking width
inline constexpr double kQuayBridgeRise = 0.45; // m: its middle deck over the higher bank
inline constexpr double kQuayBridgeDeck = 2.2;  // m: its level middle's length
inline constexpr double kQuaySlab = 0.35;       // m: a deck's, landing's or ramp's thickness

// A canal between two roads: an axis-aligned plan rectangle (bank back to bank), the banks along its long side, each a
// coping `bank` wide before the water.
struct RoadCanal {
    std::string id = "CN-01";
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    double bank = 0.4;       // m: each bank's coping, inside the rectangle
    double bridgeAt = 0.0;   // plan coordinate along the long side of the footbridge's centre line; <= 0: none
    double mastGap = 25.0;   // m between masts along each bank, about (the two banks staggered by half)
    double floorZ = 1.5;     // m: the channel's floor top
    double waterZ = 2.5;     // m: the water's surface
    double wall = 1.0;       // m: each quay wall's thickness, from the rectangle's edge in (sides and ends)
    double coping = 0.25;    // m: the coping slab on each wall's top
};
// One row of the point table (kinds railing_span, lamp_mast, bridge_deck, bridge_landing, bridge_stair, bridge_pier).
struct RoadQuayPoint {
    std::string kind, id, group, deck;
    double x = 0.0, y = 0.0, z0 = 0.0, z1 = 0.0;
    double rot = 0.0;    // plan degrees, the long axis u
    double length = 0.0, width = 0.0;
    double slope = 0.0;  // rise per m along +u
};
struct RoadQuayCounts {
    std::size_t spans = 0, posts = 0, masts = 0, bridge = 0;
    std::size_t mastsMoved = 0;  // masts nudged along the bank off the road's furniture
    std::size_t roadInWater = 0; // road band feet (carriageway, kerb, pavement) reaching into the water
    double railed = 0.0;         // m of railing
    double bridgeAt = 0.0;       // where the footbridge went (moved off the furniture), 0: none
};
// The canal's points: each bank's top is the road pavement's top just behind it in `build` (else the ground); a rail
// line kQuayRailIn back from each water edge, broken round the footbridge and each mast, cut into runs of equal spans of
// kQuaySpanMax at most (rot along the run, z0 the bank top, z1 = z0 + kQuayRailHigh) and a closing post (a bridge_pier
// row kQuayPost square, rail high) at each run's +u end; a lamp mast every mastGap on each bank, the second staggered
// by half, each nudged up to 6 m along the bank to keep kQuayMastClear off `build.furniture` (its head facing the
// road behind it); the footbridge FB-01 across at bridgeAt (moved up to 10 m to clear the furniture by 0.5 m): a
// landing on each coping, a ramp up kQuayBridgeRise from each water edge (bridge_stair, rot uphill) and a level deck
// kQuayBridgeDeck long, railed along both sides. Ids <canal>-<kind>-<n>; groups QR-<canal>-W|E-<n> and FB-01; deck is
// the canal's id. `out` is cleared first. Allocates: build time only.
FTCORE_API void roadQuayPoints(const RoadCanal& canal, const RoadBuild& build, const MassingGround& ground,
                               std::vector<RoadQuayPoint>& out, RoadQuayCounts* counts = nullptr);
// The channel as road solids, appended to `build` (ids <canal>-<part>-<n>, kinds canal_*): a quay wall along each side
// and across each end, `wall` thick from the rectangle's edge in, from 0.3 m under the floor up to the bank top less
// `coping`, in pieces of 10 m at most along the banks (each bank's top as roadQuayPoints reads it); a coping slab
// (Kerb, walked on) over each wall piece up to the bank top; the floor (PoleConcrete, walked on) between the walls,
// 0.3 m deep up to floorZ; the water (Water, no collision) a 5 cm plate up to waterZ between the walls. The ground stays
// as it is: under ground higher than waterZ the channel is buried until the height field is cut. Returns the solids
// added (0 for a rectangle under 4 x (2 wall + 1) m).
FTCORE_API std::size_t roadCanalBuild(const RoadCanal& canal, const MassingGround& ground, RoadBuild& build);
// The point table as CSV: '#' comment rows, then kind,id,group,x,y,z0,z1,rot,length_m,width_m,slope,deck.
FTCORE_API std::string roadQuayCsv(const std::vector<RoadQuayPoint>& points);

// ---- Footways: a raised, kerbed pavement on every street a vehicle uses outside the road network ----

inline constexpr double kFootLaneCarriage = 3.0;  // m: a lane's one-way carriageway (lanes 4 to 6 m)
inline constexpr double kFootLaneMin = 1.0;       // m: a lane footway, kerb stone included, at least
inline constexpr double kFootLaneMax = 1.5;       // m: ... at most
inline constexpr double kFootRoadMin = 2.0;       // m: a street of 7 m or more: each footway, at least
inline constexpr double kFootRoadMax = 3.0;       // m: ... at most (0.2 of the width between)
inline constexpr double kFootAlleyUpTo = 3.0;     // m: a street no wider is an alley: pedestrians only
inline constexpr double kFootGutter = 0.3;        // m: the dark gutter strip at the kerb's foot, in the carriageway
inline constexpr double kFootDrainEvery = 12.0;   // m: a drain grate in the gutter
inline constexpr double kFootRamp = 1.2;          // m: a drop kerb's ramp each side of a mouth
inline constexpr double kFootTactile = 0.6;       // m: the tactile strip at a ramp's foot
inline constexpr double kFootLaneUpTo = 6.0;      // m: a vehicle street this wide or narrower is a one-way lane
inline constexpr double kFootStep = 1.0;          // m: the footway's line is read at most this far apart
inline constexpr double kFootFit = 0.03;          // m: a piece's top keeps this near its samples' tops
inline constexpr double kFootLongest = 12.0;      // m: the longest piece
inline constexpr double kFootShortest = 0.6;      // m: a shorter run is left out
inline constexpr double kFootDryAbove = 0.5;      // m: no footway where the ground is this low or lower (the sea)
inline constexpr double kFootBollardDia = 0.15;   // m
inline constexpr double kFootBollardHigh = 0.9;   // m
inline constexpr double kFootBollardGap = 1.3;    // m: the widest gap between an alley mouth's bollards (and walls)

// What a streets-table row is to the footways: a vehicle street (kerbed footways), a pedestrian way (an alley or a
// market arcade: paved, bollards at its mouths) or neither (stairs; no row).
enum class RoadFootClass : std::uint8_t { None, Vehicle, Pedestrian };
// Vehicle: width over kFootAlleyUpTo and kind not stair, link_stair, alley or market_arcade. Pedestrian: kind alley or
// market_arcade, or any other non-stair row kFootAlleyUpTo wide or less. None: stair and link_stair.
FTCORE_API RoadFootClass roadFootClass(const MassingStreet& street);

// A vehicle street's section across its polyline (t: to its left, looking along its points).
struct RoadFootSection {
    double cwLo = 0.0, cwHi = 0.0;     // the carriageway, kerb face to kerb face
    double footLeft = 0.0;              // m: the left footway, kerb stone included (0 none)
    double footRight = 0.0;             // m: the right one
};
// Lanes (width up to 6 m): a kFootLaneCarriage carriageway and the rest as footways of kFootLaneMin to kFootLaneMax:
// both sides when each gets kFootLaneMin, else one side (the left), the carriageway beside it. Wider streets: footways
// of 0.2 of the width clamped to kFootRoadMin..kFootRoadMax both sides, or the street's own pavement_m when it has one.
// So 6 m: 1.5 + 3.0 + 1.5; 5 m: 1.0 + 3.0 + 1.0; 4 m: 1.0 (left) + 3.0; 8 m: 2.0 + 4.0 + 2.0; 10 m: 2.0 + 6.0 + 2.0;
// 14 m: 2.8 + 8.4 + 2.8. A street too narrow for that (under kFootLaneCarriage + kFootLaneMin) gets none.
FTCORE_API RoadFootSection roadFootSection(const MassingStreet& street);

// One alley mouth: an alley end meeting a vehicle street or a road's band.
struct RoadAlleyMouth {
    std::string alley, street; // street: the vehicle street's id, or "road" for the road network's band
    RoadPoint at;              // the alley's end
    std::size_t bollards = 0;
};
struct RoadFootStreet {
    std::string id;
    double length = 0.0;
    double kerbed[2] = {0.0, 0.0}; // m of kerb laid along the left [0] and right [1] side
    bool wantLeft = false, wantRight = false;
};
struct RoadFootReport {
    std::size_t vehicle = 0, pedestrian = 0, skipped = 0; // skipped: no-class rows and the road network's own rows
    std::size_t pieces = 0;      // footway pieces (each with a kerb)
    std::size_t gutters = 0;     // gutter strips: one runs on under several pieces where the ground lets it
    std::size_t hidden = 0;      // pieces whose slab is left out, the ground standing over all of it (the kerb stays)
    std::size_t ramps = 0, tactiles = 0, drains = 0, bollards = 0, paving = 0;
    std::size_t mouths = 0;      // gaps where another vehicle street's carriageway meets one (drop kerbs both sides)
    std::size_t buried = 0;      // footway pieces whose wall-side ground stands over their top by 5 cm or more
    std::size_t floating = 0;    // pieces deepened by more than 0.5 m over falling ground
    double kerbM = 0.0, footM2 = 0.0;
    std::vector<RoadFootStreet> streets;  // one per vehicle street, in table order
    std::vector<RoadAlleyMouth> alleyMouths;
};
// Builds the footways (appends to `out`: solids, footways, laneWays, furniture for the bollards). `streets` is the
// massing streets table; rows named in `skipIds` (the road network's own streets) are skipped; `net` (or null) is the
// road network's build, whose bands the footways stop short of. Each vehicle street, segment by segment: on each side
// with a footway (roadFootSection), a kerb stone kRoadKerbRise + kRoadSurfaceLift over the ground (kinds kerb), the footway slab level
// with the kerb's top back to the wall line (kind footway, tint Footway), a gutter strip kFootGutter wide in the
// carriageway flush with it (12 mm over the ground under it; kind gutter, tint Gutter, matt) with a drain grate every
// kFootDrainEvery on its line,
// all following the ground read along the kerb line every kFootStep in pieces fitted within kFootFit (kFootLongest at
// most), outer corners of bends lapped, inner ones cut back. A sample is left out within another vehicle street's
// carriageway (a mouth: the last kFootRamp of footway each side becomes a drop-kerb ramp down to the gutter, with a
// kFootTactile tactile strip at its foot), where this street ends inside another's band, inside the network's bands,
// and over the sea (ground at or under kFootDryAbove). Each pedestrian way: a paving slab wall to wall, 5 cm over the
// ground; at each end meeting a vehicle street or the network's band, a row of bollards across it (gaps no wider than
// kFootBollardGap) 0.4 m inside the other's wall line. The vehicle street's footway runs on across an alley mouth, so
// the alley meets it at a kerb-high step. Allocates: build time only.
FTCORE_API void roadFootways(const std::vector<MassingStreet>& streets, const std::vector<std::string>& skipIds,
                             const MassingGround& ground, const RoadBuild* net, RoadBuild& out,
                             RoadFootReport* report = nullptr);
// The top of the highest footway piece (build.footways) whose footprint holds the plan point; false when none does.
FTCORE_API bool roadFootwayTop(const RoadBuild& build, double x, double y, double& top);

// The footway check's rejects, one line each: a vehicle street that wants a footway on a side (RoadFootStreet) and has
// kerb along less than `share` of its length on every such side ("no kerbed footway"), and an alley mouth with no
// bollard and no footway piece within 1.5 m of its end ("open alley mouth").
FTCORE_API std::vector<std::string> roadFootRejects(const RoadFootReport& report, const RoadBuild& build,
                                                    double share = 0.4);

} // namespace ftcore
