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

// Street classes (sec. 3): a kerbed road 10-14 m, a shared lane 4-6 m (a market lane is one), an alley 2-3 m.
enum class BlockClass : std::uint8_t { Road, Lane, Market, Alley };
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
//   value, top, rows, emit (0 / 1, streets; default 1)
// A street's kind is its streets-table kind; a band's kind is x or y (its rows' axis); a feature's kind its
// BlockFeatureKind name; a landmark's x0, y0 its site, width its base and value its height. False with a line-numbered
// error on a bad row.
FTCORE_API bool importBlockPlan(std::string_view csv, BlockPlan& out, std::string* error = nullptr);

// One generated box as the massing table's row holds it.
struct BlockBox {
    std::string id, row, kind, note;
    double cx = 0.0, cy = 0.0, sx = 0.0, sy = 0.0;
    double z0 = 0.0, z1 = 0.0, ground = 0.0, terrMin = 0.0, terrMax = 0.0, sink = 0.0;
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
    std::string id, kind, host; // kind street | park | forecourt | precinct | plaza
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
    double at(double x, double y) const;
};

struct BlockGenStats {
    std::size_t lots = 0, shopLots = 0, houseLots = 0, merged = 0, slabs = 0, walkups = 0, yards = 0, enterable = 0;
    std::size_t corners = 0, pitched = 0, hiddenHouses = 0, alleys = 0;
    std::vector<std::string> notes;
};

struct BlockGenResult {
    std::vector<BlockBox> boxes;
    std::vector<BlockDoor> doors;
    std::vector<BlockTree> trees;
    std::vector<BlockHouse> houses;
    std::vector<BlockTower> towers;
    std::vector<BlockPlanStreet> streets; // the plan's emitted streets plus the bands' mid-block alleys
    BlockGenStats stats;
};

// Lot rules (sec. 3).
inline constexpr double kBlockHouseMin = 4.5, kBlockHouseMax = 9.0;
inline constexpr double kBlockShopMin = 6.0, kBlockShopMax = 12.0;
inline constexpr double kBlockCorner = 1.3;
inline constexpr double kBlockRowMin = 10.0, kBlockRowMax = 16.0, kBlockRowAim = 13.0;
inline constexpr double kBlockAlley = 2.5;
inline constexpr double kBlockFloor = 3.0, kBlockShopFloor = 3.9;
inline constexpr double kBlockCell = 250.0;
inline constexpr int kBlockSlabsPerCell = 2;

// How many rows a band of depth d between two streets takes (2..6) so each row is 10-16 m deep with floor((n-1)/2)
// alleys of kBlockAlley between back-to-back pairs, the row depth nearest 13 m; 0 if none fits. *rowDepth gets it.
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

// The footprint's axis-aligned bounds.
FTCORE_API BlockRect blockFootprint(const MassingBox& b);
// Whether a point lies on a street (within half its width of a centreline segment).
FTCORE_API bool blockOnStreet(const MassingStreet& s, double x, double y, double grow = 0.0);

} // namespace ftcore
