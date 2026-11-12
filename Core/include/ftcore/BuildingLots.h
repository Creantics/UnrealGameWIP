#pragma once

// Street frontage made from the modular building kit (build time only; everything here allocates).
//
// 1. A kit table (Data/Look/building_kit_pieces.csv) lists each piece: its size, bounds minimum, triangle count,
//    material slots and snap points. Kit conventions: metres, Z up, the facade faces -Y, the wall body runs y 0 to
//    +0.25, the origin is the bottom-left corner of the wall face (x = 0 is the left edge seen from the street).
// 2. A frontage run is a straight street edge from A (its left end seen from the street) to B. Its local x runs
//    from A to B and its local +y (into the buildings) is x turned 90 degrees counter-clockwise.
// 3. planFrontage takes the massing boxes whose street face lies on a run, merges touching ones into parcels and
//    re-splits each parcel into lots on a 0.9 m grid: a type, a storey count from the old box height, a setback,
//    a roof, a wall surface, palette slots, a sign set and a clutter seed per lot, all from seeds.
// 4. checkFrontage tests the street rules (no equal facades within 40 m, neighbours differ in at least two traits,
//    at least three types in any 60 m, no three equal wall colours in a row); planFrontage repairs by re-rolling.
// 5. facadePlacements turns one lot into kit pieces and plain boxes in the plan frame.
//
// Use:
//   ftcore::KitTable kit; ftcore::parseKitTable(csvText, kit, &error);
//   std::vector<ftcore::FrontageRun> runs; ftcore::parseFrontageRuns(runsCsv, runs, &error);
//   ftcore::FrontagePlan plan; ftcore::planFrontage(runs, boxes, plan);
//   for (lot : plan.lots) ftcore::facadePlacements(kit, plan.runs[lot.run], lot, out);

#include "ftcore/Api.h"
#include "ftcore/MassingPlan.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

struct KitSnap {
    std::string name;
    double x = 0.0, y = 0.0, z = 0.0;
};

struct KitPiece {
    std::string id;
    std::string group;
    double sx = 0.0, sy = 0.0, sz = 0.0;
    double minX = 0.0, minY = 0.0, minZ = 0.0;
    int triangles = 0;
    std::vector<std::string> slots;
    std::vector<KitSnap> snaps;
    const KitSnap* snap(std::string_view name) const;
};

struct KitTable {
    std::vector<KitPiece> pieces;
    int find(std::string_view id) const; // -1 when absent
};

// Columns by header name: id, group, size_x, size_y, size_z, min ("x/y/z"), triangles, slots ("a;b"),
// snaps ("NAME:x/y/z;..."). Other columns are ignored; quoted cells may hold commas.
FTCORE_API bool parseKitTable(std::string_view csv, KitTable& out, std::string* error = nullptr);

struct FrontageRun {
    std::string id;
    double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0; // plan frame, m; A is the left end seen from the street
    double maxDepth = 14.0;                        // m, lots go no deeper
    bool cornerAtA = false;                        // the first lot is a corner shop whose return faces past A
    double length() const;
    double yaw() const;                            // degrees, counter-clockwise from plan +x to the run's +x
    // The plan point s m along the run and y m inward from its line.
    void toPlan(double s, double y, double& px, double& py) const;
};

// Columns: id, ax, ay, bx, by, max_depth, corner (0/1).
FTCORE_API bool parseFrontageRuns(std::string_view csv, std::vector<FrontageRun>& out, std::string* error = nullptr);

enum class LotType : std::uint8_t { Corner, Pencil, WalkUp, Tiled, TownHouse };
enum class RoofKind : std::uint8_t { Flat, Gable, Hip };
enum class WallSurface : std::uint8_t { Tile, Concrete, Stucco };

FTCORE_API const char* lotTypeName(LotType t);

constexpr double kLotGrid = 0.9;     // m
constexpr double kShopStorey = 3.6;  // m, the glazed and shutter shop storey
constexpr double kKoshiStorey = 3.0; // m, the timber shop storey
constexpr double kUpperStorey = 3.0; // m

struct BuildingLot {
    int run = 0;                      // index into the plan's runs
    std::vector<std::string> parents; // the replaced boxes it overlaps
    double s0 = 0.0, s1 = 0.0;        // along the run, m
    double depth = 0.0;               // m from the run line
    double floorZ = 0.0;              // the ground-floor level, m (the caller may lift it to the ground's high end)
    double footZ = 0.0;               // the lowest ground under the lot; a plinth fills footZ to floorZ
    double oldTop = 0.0;              // the replaced boxes' top, m
    LotType type = LotType::WalkUp;
    int floors = 3;                   // storeys including the ground one
    double setback = 0.0;             // m, 0 to 0.45
    RoofKind roof = RoofKind::Flat;
    WallSurface wall = WallSurface::Concrete;
    std::uint8_t wallColour = 0, trimColour = 0, accent = 0, timber = 0, roofColour = 0, signColour = 0;
    std::uint8_t groundVariant = 0, upperVariant = 0, signSet = 0;
    std::uint32_t clutterSeed = 0;
    std::uint32_t seed = 0;
    double width() const { return s1 - s0; }
    int units() const;                // width in 0.9 m grid steps
    double groundStorey() const { return type == LotType::TownHouse ? kKoshiStorey : kShopStorey; }
    double top() const { return floorZ + groundStorey() + kUpperStorey * (floors - 1); }
};

struct FrontagePlan {
    std::vector<FrontageRun> runs;
    std::vector<BuildingLot> lots;          // in run order, then along each run
    std::vector<std::string> replaced;      // every box the lots stand in for (hide these)
    int repairs = 0;                        // trait re-rolls the street rules needed
};

// Picks the boxes on each run (a face within 0.4 m of the run line, parallel to it, inside, at least 0.5 m of
// overlap), merges parcels whose spans touch (gap < 0.3 m), re-splits them into lots and repairs the street rules.
// Lot floorZ is the highest parent z0 plus 0.5 (the massing boxes stand 0.5 m into the ground), footZ the lowest z0.
FTCORE_API void planFrontage(const std::vector<FrontageRun>& runs, const std::vector<MassingBox>& boxes, FrontagePlan& out);

// A lot's traits, the facade signature hashes all but its clutter seed and position.
FTCORE_API std::uint32_t lotSignature(const BuildingLot& lot);
FTCORE_API int lotTraitDifferences(const BuildingLot& a, const BuildingLot& b);

struct FrontageCheck {
    bool ok = true;
    int equalWithin40 = 0;    // pairs on one run with equal signatures less than 40 m apart (centre to centre)
    int neighbourTwins = 0;   // touching neighbours differing in fewer than two traits
    int poorWindows = 0;      // 60 m windows with fewer than three types
    int colourTriples = 0;    // three neighbours in a row with one wall colour
    std::string firstFault;
};
FTCORE_API FrontageCheck checkFrontage(const std::vector<BuildingLot>& lots);

// Placements in the plan frame. piece >= 0: a kit piece whose origin stands at (x, y, z), turned yaw degrees
// counter-clockwise, scaled (sx, sy, sz) (1, 1, 1 but for stretched blanks). piece < 0: a plain box of role Body,
// Plinth or Deck centred on (x, y) from z up sz, sx along the run, sy across it, turned yaw.
enum class PlacementRole : std::uint8_t { Piece, Clutter, Body, Plinth, Deck };

struct KitPlacement {
    int piece = -1;
    PlacementRole role = PlacementRole::Piece;
    int lot = 0;
    double x = 0.0, y = 0.0, z = 0.0;
    double yaw = 0.0;
    double sx = 1.0, sy = 1.0, sz = 1.0;
    std::uint32_t seed = 0;   // per placement: lit windows, sign glyph rows
};

// Appends the lot's placements (lotIndex is written into each). Returns false when a piece the grammar needs is
// missing from the kit table.
FTCORE_API bool facadePlacements(const KitTable& kit, const FrontageRun& run, const BuildingLot& lot, int lotIndex,
                                 std::vector<KitPlacement>& out, std::string* error = nullptr);

// Restrained wall colours, trims, timbers, roof tiles; saturated accents only for signs and awnings. Display (sRGB)
// colours, 0..1: the material decodes them to linear and uses them as the surface's colour (its map gives the pattern).
struct KitRgb {
    float r = 0.0f, g = 0.0f, b = 0.0f;
};
enum class KitPalette : std::uint8_t { Wall, Trim, Timber, Roof, Accent, SignFg };
FTCORE_API int kitPaletteSize(KitPalette p);
FTCORE_API KitRgb kitPaletteColour(KitPalette p, int index);

struct KitCensus {
    int lots = 0, pieces = 0, boxes = 0;
    long long triangles = 0; // pieces' triangles plus 12 per box
    std::array<int, 5> types{};
};
FTCORE_API KitCensus kitCensus(const KitTable& kit, const FrontagePlan& plan, const std::vector<KitPlacement>& placements);

} // namespace ftcore
