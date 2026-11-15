#pragma once

// A block generator for the massing plan: it fills a slice of a map with lots from a small plan table of streets, bands
// and features, by the layout rules (street classes, a build-to row on every street side, back-to-back rows with mid-block
// alleys, lot widths and depths, heights by class with variety, yards, merged walk-ups and slabs, enterable lots), and
// writes the rows a massing box table, a door table and the stand-in point tables take. BlockCheck.h checks the result.
//
// Frame: the plan's (x east, y north, metres). Every generated box is axis-aligned (rot 0). A band's rows run along u
// (x for an x band, y for a y band) and stack along w; its low side is w's low end.
//
// Seeds: a band's seed is blockSeed(its first street's id, its block index), 32-bit FNV-1a, stable across builds and
// platforms.
//
// Plain C++20. The import and the generator allocate (build time only).

#include "ftcore/Api.h"
#include "ftcore/MassingDress.h"
#include "ftcore/MassingPlan.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ftcore {

// 32-bit FNV-1a over bytes, from h (the standard offset basis by default).
FTCORE_API std::uint32_t blockFnv1a(std::string_view bytes, std::uint32_t h = 0x811C9DC5u);
// A band's seed: FNV-1a over the street id, a 0x1F separator and the block index as 4 little-endian bytes.
FTCORE_API std::uint32_t blockSeed(std::string_view streetId, std::uint32_t blockIndex);
// A seed's value in [0, 1) for a key (FNV-1a over the key from the seed).
FTCORE_API double blockHash01(std::uint32_t seed, std::string_view key);

// A small xorshift32 generator (a zero seed is moved off zero).
struct BlockRng {
    std::uint32_t s = 1;
    explicit BlockRng(std::uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    double unit() { return static_cast<double>(next() >> 8) / 16777216.0; } // [0, 1)
    double range(double lo, double hi) { return lo + (hi - lo) * unit(); }
};

// Street classes (sec. 3, the 11-10 ruling): a road 10-14 m with kerbed pavements of 2-3 m; a lane 4-6 m, a ~3 m
// carriageway and kerbed footways of 1-1.5 m (a market lane is one; no kerbless shared lanes); an alley kBlockAlleyMax
// m or narrower, pedestrians only, bollards or a step at each mouth.
enum class BlockClass : std::uint8_t { Road, Lane, Market, Alley };
inline constexpr double kBlockAlleyMax = 3.0; // m: an alley is this wide or narrower
FTCORE_API std::string_view blockClassName(BlockClass c);

struct BlockRect {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    double w() const { return x1 - x0; }
    double h() const { return y1 - y0; }
    bool contains(double x, double y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    bool overlaps(const BlockRect& o, double eps = 1e-6) const {
        return x0 < o.x1 - eps && o.x0 < x1 - eps && y0 < o.y1 - eps && o.y0 < y1 - eps;
    }
};

// A straight street of the plan, centreline x0,y0 -> x1,y1. emit false: a street the plan only reads (a loop road).
struct BlockPlanStreet {
    std::string id, kind;
    BlockClass cls = BlockClass::Lane;
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    double width = 6.0, pavement = 0.0;
    bool emit = true;
};

// A band of rows between two streets (or a street and a back). alongX: the rows run along x. low / high: the street
// ids on w's low and high sides (empty: a back). start / end: the streets at u's low and high ends (corner lots; empty:
// none). rows 0: packed by the rules.
struct BlockPlanBand {
    std::string id;
    bool alongX = true;
    BlockRect r;
    std::string low, high, start, end;
    std::uint32_t block = 0;
    int rows = 0;
    std::string district; // empty: the band's own id
};

enum class BlockFeatureKind : std::uint8_t { Park, Forecourt, Plaza, Precinct, Hall, Podium, Court, Gate };
FTCORE_API std::string_view blockFeatureName(BlockFeatureKind k);

// A feature cuts the lots it overlaps. A podium: floors (value) or an absolute top (top > 0), and a glass tower of side
// width on it (width > 0). A hall: floors (value). A precinct: walls around its rect, open where a gate stands on its
// edge. A gate: two pillars and a beam across its rect's long side. Park, forecourt, plaza, court: open (exempt from the
// paving limit); park, forecourt, plaza and precinct get trees.
struct BlockPlanFeature {
    std::string id;
    BlockFeatureKind kind = BlockFeatureKind::Park;
    BlockRect r;
    double value = 0.0, top = 0.0, width = 0.0;
    std::string district; // empty: the feature's own id
};

// The placed landmark: base side and height (m).
struct BlockPlanLandmark {
    std::string id;
    double x = 0.0, y = 0.0, base = 0.0, height = 0.0;
};

struct BlockPlan {
    std::vector<BlockPlanStreet> streets;
    std::vector<BlockPlanBand> bands;
    std::vector<BlockPlanFeature> features;
    std::vector<BlockPlanLandmark> landmarks;
    std::vector<std::string> drops; // base street ids the slice removes
};

// Reads a plan table (CSV, '#' comments, a header row; columns found by name):
//   type   street | band | feature | landmark | drop
//   id, kind, class (road | lane | market | alley), x0, y0, x1, y1, width, pavement, low, high, start, end, block,
//   value, top, rows, emit (0 / 1, streets; default 1), district (bands and features; optional)
// A street's kind is its streets-table kind; a band's kind is x or y (its rows' axis); a feature's kind its
// BlockFeatureKind name; a landmark's x0, y0 its site, width its base and value its height. False with a line-numbered
// error on a bad row.
FTCORE_API bool importBlockPlan(std::string_view csv, BlockPlan& out, std::string* error = nullptr);

// One generated box as the massing table's row holds it.
struct BlockBox {
    std::string id, row, kind, note;
    double cx = 0.0, cy = 0.0, sx = 0.0, sy = 0.0;
    double z0 = 0.0, z1 = 0.0, ground = 0.0, terrMin = 0.0, terrMax = 0.0, sink = 0.0;
    std::string district;  // the district of its band or feature (also in note as district=<id>)
    bool building = false; // a lot, a podium or a hall (not a wall, paving or gate piece)
    BlockRect rect() const { return {cx - sx * 0.5, cy - sy * 0.5, cx + sx * 0.5, cy + sy * 0.5}; }
};

// A lot's door and room, as the road-door table's DOOR- and ROOM- rows hold them.
struct BlockDoor {
    std::string boxId, roomKind; // interior_shop | interior_flat | interior_lobby
    double ox = 0.0, oy = 0.0;   // 0.32 m outside the front face
    double ix = 0.0, iy = 0.0;   // 1.68 m inside it
    double z = 0.0;
    BlockRect room;
};

struct BlockTree {
    std::string id, kind, host; // kind street | park (parks, plazas, podium roofs: z the roof) | forecourt | precinct
    double x = 0.0, y = 0.0, z = 0.0, height = 0.0, canopy = 0.0;
};

struct BlockHouse {
    std::string id, boxId;
    double cx = 0.0, cy = 0.0, sx = 0.0, sy = 0.0, eave = 0.0, pitch = 0.0;
    char ridge = 'u';
    bool hidden = false;
};

struct BlockTower {
    std::string id, kind, note; // glass | landmark
    double cx = 0.0, cy = 0.0, z0 = 0.0, sx = 0.0, sy = 0.0, height = 0.0;
    double rot = 0.0; // degrees in the plan's frame (generated towers 0; read ones turned into the frame)
    int floors = 0;
};

// The ground the generator stands lots on: the height field, raised to the zone plates (Z rows) where they stand higher.
struct BlockGround {
    MassingGround field;
    std::vector<MassingBox> zones; // zone rows only (may be empty)
    std::vector<bool> zoneOn;      // all true, zones' size (setZones fills both)
    void setZones(std::vector<MassingBox> z) {
        zones = std::move(z);
        zoneOn.assign(zones.size(), true);
    }
    // A local frame (a plan drawn along a turned street grid): at() takes local x, y and samples the world point
    // (ox, oy) + R(deg) (x, y). The field and the zones stay in world terms.
    double frameX = 0.0, frameY = 0.0, frameCos = 1.0, frameSin = 0.0;
    void setFrame(double ox, double oy, double deg);
    void toWorld(double x, double y, double& wx, double& wy) const {
        wx = frameX + frameCos * x - frameSin * y;
        wy = frameY + frameSin * x + frameCos * y;
    }
    void toLocal(double wx, double wy, double& x, double& y) const {
        const double dx = wx - frameX, dy = wy - frameY;
        x = frameCos * dx + frameSin * dy;
        y = -frameSin * dx + frameCos * dy;
    }
    double at(double x, double y) const;
};

// A district's cues, so wards read apart: a colour (#RRGGBB), a sign style and a paving, from fixed palettes in the
// order districts first appear in the plan, or slot k for a plan district written "name@k" (k 0..5). lots: its
// generated lots.
struct BlockDistrict {
    std::string id, colour, sign, paving;
    std::size_t lots = 0;
};

struct BlockGenStats {
    std::size_t lots = 0, shopLots = 0, houseLots = 0, merged = 0, slabs = 0, walkups = 0, yards = 0, enterable = 0;
    std::size_t corners = 0, pitched = 0, hiddenHouses = 0, alleys = 0;
    std::size_t alleysCut = 0; // alley ends run on to a street
    std::size_t roofTrees = 0, stalls = 0, marks = 0; // r10 dressing: podium roof gardens, market stalls, gate roofs and paths
    std::vector<std::string> notes;
};

struct BlockGenResult {
    std::vector<BlockBox> boxes;
    std::vector<BlockDoor> doors;
    std::vector<BlockTree> trees;
    std::vector<BlockHouse> houses;
    std::vector<BlockTower> towers;
    std::vector<BlockPlanStreet> streets; // the plan's emitted streets plus the bands' mid-block alleys
    std::vector<BlockDistrict> districts;
    BlockGenStats stats;
};

// Lot rules (sec. 3).
inline constexpr double kBlockHouseMin = 4.5, kBlockHouseMax = 9.0;
inline constexpr double kBlockShopMin = 6.0, kBlockShopMax = 12.0;
inline constexpr double kBlockCorner = 1.3;
inline constexpr double kBlockRowMin = 10.0, kBlockRowMax = 16.0, kBlockRowAim = 13.0;
inline constexpr double kBlockAlley = 3.0;       // a mid-block alley (the class cap: readable from above)
inline constexpr double kBlockAlleyReach = 20.0; // m an alley end runs on to reach a street
inline constexpr double kBlockApronMax = 18.0;   // m: a bare podium roof run over this gets a roof garden
inline constexpr double kBlockFloor = 3.0, kBlockShopFloor = 3.9;
inline constexpr double kBlockCell = 250.0;
inline constexpr int kBlockSlabsPerCell = 2;

// How many rows a band of depth d between two streets takes (2..6) so each row is 10-16 m deep with n-1 alleys of
// kBlockAlley, one between every pair of rows, the row depth nearest 13 m; 0 if none fits. *rowDepth gets it.
FTCORE_API int blockRowCount(double depth, double* rowDepth = nullptr);

// Splits a frontage of length len into lot widths for a class (houses 4.5-9, shops 6-12; the lots at a corner end
// x1.3), filling it exactly. Appends to out.
FTCORE_API void blockLotWidths(double len, bool shop, bool cornerLow, bool cornerHigh, BlockRng& rng, std::vector<double>& out);

// A lot's floors by its row's class (road 3-6 mode 4, lane and market 2-4 mode 3, alley 1-3 mode 2).
FTCORE_API int blockFloors(BlockClass c, BlockRng& rng);
// A lot's height: the shop floor (or a floor), the floors above at 3 m, and a parapet.
FTCORE_API double blockHeight(int floors, bool shop, double parapet);
// Fixes a row's heights in place (u order): no run of more than 3 identical, at least 4 heights in every 60 m of
// frontage (when the row holds 4 lots there). Changes parapets (0 / 0.6 / 1.2) and, if need be, a floor. Returns the
// changes. centres: the lots' u centres.
FTCORE_API std::size_t blockVaryHeights(std::vector<double>& heights, const std::vector<double>& centres, bool shop);

// Generates the plan's lots, features, doors, trees, houses and towers.
FTCORE_API void blockGenerate(const BlockPlan& plan, const BlockGround& ground, BlockGenResult& out);

// The sweep: whether a base row is kept. A row whose footprint misses rect is kept unless it overlaps a generated
// building; one inside is kept only if it is a zone (src Z, kind zone / zone_slope), a kerb, a marking, a cliff plate or
// loop kerb (id V1-CLF / V1-KB), a plan retaining wall (src BLD, kind wall), a deck, or a gutter whose centre lies on a
// kept street (streets: the output street table).
FTCORE_API bool blockSweepKeep(const MassingBox& b, const BlockRect& rect, const std::vector<MassingStreet>& streets,
                               const std::vector<BlockBox>& generated);

// ---- Route tables (the route rules' generated side; BlockCheck.h measures them) ----

inline constexpr double kBlockMainWidth = 10.0;   // a main street: kerbed (pavement > 0) and this wide or wider
inline constexpr double kBlockMainStraight = 150.0; // m of straight run a main street holds
inline constexpr double kBlockTurnDeg = 5.0;       // a bend under this keeps a run straight

// Street roles: main (a road: kerbed, 10 m or wider), lane (not kind alley, over kBlockAlleyMax m), alley (the rest).
enum class BlockRole : std::uint8_t { Main, Lane, Alley };
FTCORE_API BlockRole blockStreetRole(const MassingStreet& s);
FTCORE_API std::string_view blockRoleName(BlockRole r);
// A street's longest straight run (consecutive segments turning under kBlockTurnDeg), m.
FTCORE_API double blockStraightRun(const MassingStreet& s);
// Whether a street has a point (2 m samples) within g m of rect.
FTCORE_API bool blockStreetNearRect(const MassingStreet& s, const BlockRect& r, double g);

// A street's dressing flags, so main streets read different: trees every tree_every m (0 none), lamps every lamp_every
// m of a lamp kind (mast | lantern_string | wall | none), shop signs per 100 m of frontage, an awning flag and what
// stands at its mouths (junction | dropped_kerb | bollards: an alley is pedestrians only). Role: road | market | lane |
// alley (main: a road).
struct BlockStreetFlag {
    std::string id, kind, role, lampKind, mouth;
    bool main = false, awning = false;
    double width = 0.0, length = 0.0, straight = 0.0, treeEvery = 0.0, lampEvery = 0.0, signsPer100 = 0.0;
};
// A street name plate at a junction (kind kBlockPlateKind): on a building's corner (mount wall) or on a pole on the
// pavement corner (mount pole); rot faces along street a (deg, from +x toward +y); z the ground at its foot.
inline constexpr const char* kBlockPlateKind = "name_plate";
struct BlockPlate {
    std::string id, a, b, mount;
    double x = 0.0, y = 0.0, z = 0.0, rot = 0.0;
};
// The flags of every street with a point within 20 m of rect, and a plate at every junction inside rect between two of
// them within 20 m of rect, its corner within rect (1 m grace) (alley with alley left out). Buildings: boxes of kind kit, box or slab at least 2.5 m tall. Appends.
FTCORE_API void blockRouteTables(const std::vector<MassingStreet>& streets, const std::vector<MassingBox>& boxes,
                                 const BlockRect& rect, const BlockGround& ground, std::vector<BlockStreetFlag>& flags,
                                 std::vector<BlockPlate>& plates);
// A name plate for each named place (park, plaza, precinct) of the plan: on a pole 1 m in from the middle of its side
// nearest a street (of `streets`), street_a the feature's id, street_b its kind, rot along that side. Appends.
FTCORE_API void blockPlacePlates(const std::vector<BlockPlanFeature>& features, const std::vector<MassingStreet>& streets,
                                 const BlockGround& ground, std::vector<BlockPlate>& plates);


// The footprint's axis-aligned bounds.
FTCORE_API BlockRect blockFootprint(const MassingBox& b);
// Whether a point lies on a street (within half its width of a centreline segment).
FTCORE_API bool blockOnStreet(const MassingStreet& s, double x, double y, double grow = 0.0);

} // namespace ftcore
