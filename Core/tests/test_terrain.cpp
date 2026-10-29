// The big worlds' ground (ftcore/Terrain.h): each world in Data/Field/worlds.json, generated once,
// held to the header's promises (pads, roads and their banks, the sightline, the features, the
// edges, the queries) and to natural ground: no slope steeper than kRockSlope off the sheer walls,
// no terraces, few flats. Ids and numbers only.

#include "ftcore/Terrain.h"
#include "ftcore/WorldLayout.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace ftcore;

namespace {

using FeatureKind = TerrainFeature::Kind;

constexpr std::size_t kN = WorldTerrain::kSamples;
constexpr float kStep = WorldTerrain::kHeightStep;
constexpr float kStepTol = 0.5f * kStep + 1e-4f; // a height as stored: rounded to a whole step
constexpr float kR = WorldTerrain::kFlatRadius;
constexpr float kLevel = WorldTerrain::kRoadHalfWidth + WorldTerrain::kShoulder; // level with a road out to here
constexpr float kFar = 1e30f;

// Terrain.cpp's numbers behind the header's words.
constexpr float kReach = 90.f;         // m from a road's centre line that its banks still shape the ground
constexpr float kClimaxWant = 36.f;    // m: the climax's height, where the roads up to it allow
constexpr float kClimaxClear = 2.f;    // m a major stays under the climax, at least
constexpr float kEyeInset = 5.f;       // the sightline runs from kEyeInset inside the entry pad's edge,
constexpr float kEyeHeight = 1.7f;     // kEyeHeight over it,
constexpr float kAimHeight = 15.f;     // to kAimHeight over the climax's centre;
constexpr float kSightClear = 1.5f;    // the ground keeps kSightClear under it
constexpr float kCorridorHalf = 15.f;  // within kCorridorHalf of it,
constexpr float kCorridorSlope = 0.5f; // and that ceiling rises at kCorridorSlope beyond
constexpr float kCoastBand = 60.f;     // m inside each edge: the town's coast is the edge whose band lies lowest
constexpr float kSeaUnder = 6.f;       // m the sea lies under the lowest ground in that band, at least
constexpr float kQuayRun = 6.f;        // m past the coast edge within which the quay drops
constexpr float kQuayDrop = 7.f;       // m it drops, at least
constexpr float kRangeOut = 230.f;     // m past the edges, and on, where the mountains stand
constexpr float kRangeOver = 20.f;     // m over the climax, at least
constexpr float kLipNear = 15.f;       // m past the ruins' edges that their ground still runs on, at least
constexpr float kLipFloor = -100.f;    // m: and stays above this there
constexpr float kAbyssOut = 130.f;     // m past the ruins' edges, and on, where the abyss lies
constexpr float kAbyssTop = -170.f;    // m: its floor, between these
constexpr float kAbyssBottom = -190.f;
constexpr float kFeatureClear = 25.f;  // m a feature keeps from the roads' edges, the pads' blends and the other features
constexpr float kWallClear = 30.f;     // m it keeps inside the walls
constexpr float kBlendLeast = 20.f;    // m: a pad's blend reaches this far past its flat radius, at least
constexpr float kTrenchApart = 120.f;  // m between two trenches, at least
constexpr float kPitApart = 120.f;     // m between two pits, at least
constexpr float kPoolApart = 80.f;     // m between two pools, at least
constexpr float kTrenchDepth = 18.f;   // m a trench's floor lies under the ground round it, at least
constexpr float kPitTop = -120.f;      // m: a pit's floor, between these
constexpr float kPitBottom = -170.f;
constexpr float kPoolDepth = 0.6f;     // m of water in a pool, inside its edge
constexpr float kCutBand = 6.f;        // m of walkable ground round a trench or a pit
constexpr float kWalkSlope = 0.99183f; // the steepest ground a player walks up
static_assert(kPoolDepth + kStepTol < WorldTerrain::kWadeDepth, "a pool is shallow throughout");

// Natural ground.
constexpr float kTerraceNear = 0.1f; // m from a whole multiple of a terrace's height: on the terrace
constexpr float kFlatSlope = 0.01f;  // a cell no steeper than this is flat
constexpr double kFlatShare = 0.05;  // of the open cells, at most: natural ground measures about 0.3 to 1.9%, the old terraced ground 64%

float between(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// Terrain.cpp's, step for step: the point of segment a-b nearest p, as its parameter (0 at a, 1 at
// b), and its distance from p.
float nearestOnSegment(Vec2 p, Vec2 a, Vec2 b, float& distance) {
    const float abx = b.x - a.x, aby = b.y - a.y;
    const float len2 = abx * abx + aby * aby;
    const float t = len2 > 0.f ? std::clamp(((p.x - a.x) * abx + (p.y - a.y) * aby) / len2, 0.f, 1.f) : 0.f;
    distance = std::hypot(p.x - (a.x + abx * t), p.y - (a.y + aby * t));
    return t;
}

// How far p lies past the playable square's edge on `side` (0 +x, 1 +y, 2 -x, 3 -y): negative inside.
float past(Vec2 p, float halfSide, int side) {
    const float along = side % 2 == 0 ? p.x : p.y;
    return (side < 2 ? along : -along) - halfSide;
}

// The samples (or points) that break one rule: how many, and the worst and where.
struct Breaches {
    std::size_t count = 0;
    float worst = 0.f;
    std::size_t wx = 0, wy = 0;

    void add(float by, std::size_t ix = 0, std::size_t iy = 0) {
        if (count++ == 0 || by > worst) {
            worst = by;
            wx = ix;
            wy = iy;
        }
    }
};

std::string describe(const Breaches& b) {
    if (b.count == 0) return "none";
    return std::to_string(b.count) + ", the worst by " + std::to_string(b.worst) + " at (" + std::to_string(b.wx) +
           ", " + std::to_string(b.wy) + ")";
}

// A world of worlds.json, its layout and its ground, generated once for all the tests here; and how
// far each sample inside the square lies from the roads, the pads and the features.
struct Generated {
    WorldSpec spec;
    std::unique_ptr<WorldLayout> layout;
    std::unique_ptr<WorldTerrain> terrain;
    bool ok = false;
    std::string error;
    double seconds = 0.0; // generateWorldTerrain's
    int entry = -1, climax = -1;
    std::vector<float> roadGap;            // m from the nearest road's centre line; kFar outside the square
    std::vector<float> padGap;             // m outside the nearest pad's flat radius; kFar outside the square
    std::vector<std::uint8_t> nearFeature; // 1 within kFeatureClear of a feature other than the sea

    float halfSide() const { return 0.5f * spec.side; }
    // Sample (ix, iy)'s place, as the generator puts it.
    Vec2 at(std::size_t ix, std::size_t iy) const {
        const float e = terrain->extent(), c = terrain->spacing();
        return {-0.5f * e + static_cast<float>(ix) * c, -0.5f * e + static_cast<float>(iy) * c};
    }
    bool inside(Vec2 p) const { return std::fabs(p.x) <= halfSide() && std::fabs(p.y) <= halfSide(); }
    float top() const { return terrain->padHeight(static_cast<std::size_t>(climax)); }
};

void measure(Generated& g) {
    const WorldLayout& lay = *g.layout;
    const WorldTerrain& ground = *g.terrain;
    g.roadGap.assign(kN * kN, kFar);
    g.padGap.assign(kN * kN, kFar);
    g.nearFeature.assign(kN * kN, 0);
    for (std::size_t iy = 0; iy < kN; ++iy)
        for (std::size_t ix = 0; ix < kN; ++ix) {
            const Vec2 p = g.at(ix, iy);
            if (!g.inside(p)) continue;
            const std::size_t i = iy * kN + ix;
            for (std::size_t r = 0; r < lay.roadCount(); ++r) {
                float d = 0.f;
                nearestOnSegment(p, lay.district(lay.road(r).a).centre, lay.district(lay.road(r).b).centre, d);
                g.roadGap[i] = std::min(g.roadGap[i], d);
            }
            for (std::size_t k = 0; k < lay.districtCount(); ++k)
                g.padGap[i] = std::min(g.padGap[i], between(p, lay.district(k).centre) - kR);
            for (std::size_t f = 0; f < ground.featureCount(); ++f) {
                const TerrainFeature& feat = ground.feature(f);
                if (feat.kind != FeatureKind::Sea && feat.covers(p, -kFeatureClear)) g.nearFeature[i] = 1;
            }
        }
}

std::vector<Generated> generateAll() {
    std::vector<Generated> all;
    std::string text;
    if (!ftcore_test::readProjectData("Field/worlds.json", text)) return all;
    WorldTable table;
    Generated unread;
    if (!importWorldTable(text, table, &unread.error)) {
        all.push_back(std::move(unread));
        return all;
    }
    for (const WorldSpec& s : table.worlds) {
        Generated g;
        g.spec = s;
        g.layout = std::make_unique<WorldLayout>();
        g.terrain = std::make_unique<WorldTerrain>();
        for (const WorldSite& site : s.sites) {
            if (site.tier == SiteTier::Entry) g.entry = static_cast<int>(site.district);
            if (site.tier == SiteTier::Climax) g.climax = static_cast<int>(site.district);
        }
        if (generateWorldLayout(s, *g.layout, &g.error)) {
            const auto start = std::chrono::steady_clock::now();
            g.ok = generateWorldTerrain(s, *g.layout, *g.terrain, &g.error);
            g.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }
        if (g.ok) measure(g);
        all.push_back(std::move(g));
    }
    return all;
}

// Every world, generated on first use; none (and a note) where the data isn't there.
const std::vector<Generated>& worlds() {
    static const std::vector<Generated> all = generateAll();
    if (all.empty()) MESSAGE("Field/worlds.json not readable; skipping");
    return all;
}

// The samples from lo to hi metres along either axis, and a sample more either way, within the field.
void samplesOver(const WorldTerrain& t, float lo, float hi, std::size_t& i0, std::size_t& i1) {
    const float offset = 0.5f * t.extent(), cell = t.spacing(), last = static_cast<float>(kN - 1);
    i0 = static_cast<std::size_t>(std::clamp(std::floor((lo + offset) / cell) - 1.f, 0.f, last));
    i1 = static_cast<std::size_t>(std::clamp(std::ceil((hi + offset) / cell) + 1.f, 0.f, last));
}

bool isMajor(const Generated& g, std::size_t district) {
    for (const WorldSite& s : g.spec.sites)
        if (s.district == district && s.tier == SiteTier::Major) return true;
    return false;
}

// Open ground: inside the square, two samples or more off the pads and the roads' shoulders, and
// clear of the features.
bool openGround(const Generated& g, std::size_t i) {
    const float margin = 2.f * g.terrain->spacing();
    return g.padGap[i] < kFar && g.padGap[i] >= margin && g.roadGap[i] >= kLevel + margin && g.nearFeature[i] == 0;
}

// The bounds the roads' banks set the ground at p, worked out as Terrain.cpp does: at least lo, at
// most hi (lo > hi where two roads' banks cross).
void bankBounds(const Generated& g, Vec2 p, float& lo, float& hi) {
    lo = -kFar;
    hi = kFar;
    for (std::size_t r = 0; r < g.terrain->roadCount(); ++r) {
        const WorldRoad& road = g.layout->road(r);
        float d = 0.f;
        const float u = nearestOnSegment(p, g.layout->district(road.a).centre, g.layout->district(road.b).centre, d);
        if (d >= kReach) continue;
        const float hr = g.terrain->roadHeight(r, u), bank = WorldTerrain::kBankSlope * std::max(d - kLevel, 0.f);
        lo = std::max(lo, hr - bank);
        hi = std::min(hi, hr + bank);
    }
}

// The sightline: from an eye over the entry pad's edge to a point over the climax's centre, and the
// ceiling it sets the ground, as Terrain.cpp works it out.
struct Sight {
    Vec2 from{}, to{};
    float zFrom = 0.f, zTo = 0.f;

    // The ceiling at p, and how far p lies from the line.
    float ceiling(Vec2 p, float& d) const {
        const float u = nearestOnSegment(p, from, to, d);
        return lerpf(zFrom, zTo, u) - kSightClear + kCorridorSlope * std::max(d - kCorridorHalf, 0.f);
    }
    float off(Vec2 p) const {
        float d = 0.f;
        nearestOnSegment(p, from, to, d);
        return d;
    }
};

Sight sightOf(const Generated& g) {
    const Vec2 a = g.layout->district(static_cast<std::size_t>(g.entry)).centre;
    const Vec2 b = g.layout->district(static_cast<std::size_t>(g.climax)).centre;
    const float inset = (kR - kEyeInset) / between(a, b);
    Sight s;
    s.from = {lerpf(a.x, b.x, inset), lerpf(a.y, b.y, inset)};
    s.to = b;
    s.zFrom = g.terrain->padHeight(static_cast<std::size_t>(g.entry)) + kEyeHeight;
    s.zTo = g.top() + kAimHeight;
    return s;
}

// How far a feature's footprint reaches from its centre along each axis.
Vec2 reachOf(const TerrainFeature& f) {
    if (f.kind == FeatureKind::Lake || f.kind == FeatureKind::Pit) {
        const float most = *std::max_element(f.ray.begin(), f.ray.end());
        return {most, most};
    }
    return f.half;
}

// The town's sea: past the edge whose band of ground inside lies lowest on average, on a whole metre,
// a metre under the lowest pad at least and kSeaUnder under that band's lowest ground.
void checkSea(const Generated& g, const TerrainFeature& sea) {
    const WorldTerrain& t = *g.terrain;
    const float halfSide = g.halfSide();
    CHECK(sea.blocked);
    REQUIRE(static_cast<int>(sea.side) < 4);
    const std::size_t coast = sea.side;
    CHECK(sea.level == std::floor(sea.level));
    float lowestPad = kFar;
    for (std::size_t i = 0; i < t.districtCount(); ++i) lowestPad = std::min(lowestPad, t.padHeight(i));
    CHECK(sea.level <= lowestPad - 1.f);
    const std::array<Vec2, 4> outward{{{1.f, 0.f}, {0.f, 1.f}, {-1.f, 0.f}, {0.f, -1.f}}};
    CHECK(sea.centre.x == outward[coast].x * halfSide);
    CHECK(sea.centre.y == outward[coast].y * halfSide);
    std::array<double, 4> sum{};
    std::array<std::size_t, 4> count{};
    std::array<float, 4> low{kFar, kFar, kFar, kFar};
    for (std::size_t iy = 0; iy < kN; ++iy)
        for (std::size_t ix = 0; ix < kN; ++ix) {
            const Vec2 p = g.at(ix, iy);
            if (!g.inside(p)) continue;
            for (std::size_t s = 0; s < 4; ++s) {
                if (past(p, halfSide, static_cast<int>(s)) < -kCoastBand) continue;
                const float h = t.sample(ix, iy);
                sum[s] += h;
                ++count[s];
                low[s] = std::min(low[s], h);
            }
        }
    for (std::size_t s = 0; s < 4; ++s) {
        INFO("edge " << s);
        REQUIRE(count[s] > 0u);
        CHECK(sum[coast] / static_cast<double>(count[coast]) <= sum[s] / static_cast<double>(count[s]) + 0.01);
    }
    CHECK(sea.level <= low[coast] - kSeaUnder + kStepTol);
}

// A trench or a pit: blocked from a sample outside its edge, its walls climbing from a floor at its
// level that lies `wall` in from its edge and nowhere under it, deep, and walkable ground kCutBand
// round it, so that every walk past it goes round.
void checkCut(const Generated& g, const TerrainFeature& cut) {
    const WorldTerrain& t = *g.terrain;
    const float cell = t.spacing();
    CHECK(cut.blocked);
    CHECK(cut.rim == -cell);
    CHECK(cut.wall > 0.f);
    CHECK(t.height(cut.centre) == cut.level);
    const Vec2 reach = reachOf(cut);
    const float margin = kCutBand + 2.f * cell;
    std::size_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    samplesOver(t, cut.centre.x - reach.x - margin, cut.centre.x + reach.x + margin, x0, x1);
    samplesOver(t, cut.centre.y - reach.y - margin, cut.centre.y + reach.y + margin, y0, y1);
    const auto inBand = [&](std::size_t qx, std::size_t qy) {
        const Vec2 q = g.at(qx, qy);
        return cut.covers(q, -kCutBand) && !cut.covers(q);
    };
    Breaches offFloor, underFloor, steepBand;
    float ringLow = kFar;
    for (std::size_t iy = y0; iy <= y1; ++iy)
        for (std::size_t ix = x0; ix <= x1; ++ix) {
            const Vec2 p = g.at(ix, iy);
            const float h = t.sample(ix, iy);
            if (cut.covers(p)) {
                if (h < cut.level - kStepTol) underFloor.add(cut.level - h, ix, iy);
                if (cut.covers(p, cut.wall) && std::fabs(h - cut.level) > kStepTol) offFloor.add(std::fabs(h - cut.level), ix, iy);
            } else if (cut.covers(p, -cell)) {
                ringLow = std::min(ringLow, h);
            }
            if (ix == x1 || iy == y1 || !inBand(ix, iy) || !inBand(ix + 1, iy) || !inBand(ix, iy + 1) ||
                !inBand(ix + 1, iy + 1))
                continue;
            const float h10 = t.sample(ix + 1, iy), h01 = t.sample(ix, iy + 1), h11 = t.sample(ix + 1, iy + 1);
            const float slope = std::max(std::hypot(h10 - h, h11 - h10), std::hypot(h11 - h01, h01 - h)) / cell;
            if (slope >= kWalkSlope) steepBand.add(slope, ix, iy);
        }
    CHECK_MESSAGE(offFloor.count == 0u, "off the floor: " << describe(offFloor));
    CHECK_MESSAGE(underFloor.count == 0u, "under the floor: " << describe(underFloor));
    CHECK_MESSAGE(steepBand.count == 0u, "too steep to walk round: " << describe(steepBand));
    if (cut.kind == FeatureKind::Trench) {
        CHECK(ringLow - cut.level >= kTrenchDepth - kStepTol);
    } else {
        CHECK(cut.level == std::round(cut.level));
        CHECK(cut.level >= kPitBottom);
        CHECK(cut.level <= kPitTop);
    }
}

// A pool: open to players, kPoolDepth deep inside its edge and never deeper.
void checkPool(const Generated& g, const TerrainFeature& pool) {
    const WorldTerrain& t = *g.terrain;
    const float cell = t.spacing();
    CHECK_FALSE(pool.blocked);
    CHECK(std::fabs(t.waterDepth(pool.centre) - kPoolDepth) <= 1e-3f);
    const Vec2 reach = reachOf(pool);
    std::size_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    samplesOver(t, pool.centre.x - reach.x - cell, pool.centre.x + reach.x + cell, x0, x1);
    samplesOver(t, pool.centre.y - reach.y - cell, pool.centre.y + reach.y + cell, y0, y1);
    Breaches deep;
    for (std::size_t iy = y0; iy <= y1; ++iy)
        for (std::size_t ix = x0; ix <= x1; ++ix) {
            if (!pool.covers(g.at(ix, iy))) continue;
            const float depth = pool.level - t.sample(ix, iy);
            if (depth > kPoolDepth + kStepTol) deep.add(depth, ix, iy);
        }
    CHECK_MESSAGE(deep.count == 0u, "too deep: " << describe(deep));
}

// A feature other than the sea, with a sample more all round: kWallClear inside the walls,
// kFeatureClear from the roads' edges and the pads' blends, and out of the sightline's corridor.
void checkPlacement(const Generated& g, const TerrainFeature& feat, const Sight& sight) {
    const WorldTerrain& t = *g.terrain;
    const float cell = t.spacing(), halfSide = g.halfSide();
    const Vec2 reach = reachOf(feat);
    std::size_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    samplesOver(t, feat.centre.x - reach.x - 2.f * cell, feat.centre.x + reach.x + 2.f * cell, x0, x1);
    samplesOver(t, feat.centre.y - reach.y - 2.f * cell, feat.centre.y + reach.y + 2.f * cell, y0, y1);
    Breaches wall, road, pad, corridor;
    for (std::size_t iy = y0; iy <= y1; ++iy)
        for (std::size_t ix = x0; ix <= x1; ++ix) {
            const Vec2 p = g.at(ix, iy);
            if (!feat.covers(p, -(cell - 0.01f))) continue;
            if (std::fabs(p.x) > halfSide - kWallClear || std::fabs(p.y) > halfSide - kWallClear) {
                wall.add(std::max(std::fabs(p.x), std::fabs(p.y)) - (halfSide - kWallClear), ix, iy);
                continue;
            }
            const std::size_t i = iy * kN + ix;
            if (g.roadGap[i] < kLevel + kFeatureClear - 1e-3f) road.add(kLevel + kFeatureClear - g.roadGap[i], ix, iy);
            if (g.padGap[i] < kBlendLeast + kFeatureClear - 1e-3f) pad.add(kBlendLeast + kFeatureClear - g.padGap[i], ix, iy);
            const float d = sight.off(p);
            if (d < kCorridorHalf) corridor.add(kCorridorHalf - d, ix, iy);
        }
    CHECK_MESSAGE(wall.count == 0u, "near the walls: " << describe(wall));
    CHECK_MESSAGE(road.count == 0u, "near a road: " << describe(road));
    CHECK_MESSAGE(pad.count == 0u, "near a pad: " << describe(pad));
    CHECK_MESSAGE(corridor.count == 0u, "in the sightline's corridor: " << describe(corridor));
}

// Each feature keeps clear of the ones placed before it: kFeatureClear from any, kPoolApart from a
// pool, kPitApart from a pit; and two trenches lie kTrenchApart apart.
void checkApart(const Generated& g) {
    const WorldTerrain& t = *g.terrain;
    const float cell = t.spacing();
    for (std::size_t j = 0; j < t.featureCount(); ++j) {
        const TerrainFeature& later = t.feature(j);
        if (later.kind == FeatureKind::Sea) continue;
        const Vec2 reach = reachOf(later);
        std::size_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        samplesOver(t, later.centre.x - reach.x - cell, later.centre.x + reach.x + cell, x0, x1);
        samplesOver(t, later.centre.y - reach.y - cell, later.centre.y + reach.y + cell, y0, y1);
        for (std::size_t i = 0; i < j; ++i) {
            const TerrainFeature& earlier = t.feature(i);
            if (earlier.kind == FeatureKind::Sea) continue;
            INFO("features " << i << " and " << j);
            if (earlier.kind == FeatureKind::Trench && later.kind == FeatureKind::Trench) {
                const float gx = std::max(std::fabs(later.centre.x - earlier.centre.x) - later.half.x - earlier.half.x, 0.f);
                const float gy = std::max(std::fabs(later.centre.y - earlier.centre.y) - later.half.y - earlier.half.y, 0.f);
                CHECK(std::hypot(gx, gy) >= kTrenchApart - 1e-3f);
            }
            const float by = earlier.kind == FeatureKind::Pool  ? kPoolApart
                             : earlier.kind == FeatureKind::Pit ? kPitApart
                                                                : kFeatureClear;
            Breaches crowded;
            for (std::size_t iy = y0; iy <= y1; ++iy)
                for (std::size_t ix = x0; ix <= x1; ++ix) {
                    const Vec2 p = g.at(ix, iy);
                    if (later.covers(p) && earlier.covers(p, -by)) crowded.add(1.f, ix, iy);
                }
            CHECK_MESSAGE(crowded.count == 0u, "too near: " << describe(crowded));
        }
    }
}

// The ground at p as Terrain.h describes it, with the slope of the triangle under p: two triangles a
// cell, split along the (ix, iy)-(ix + 1, iy + 1) diagonal; outside the field, its nearest edge.
float expectedHeight(const WorldTerrain& t, Vec2 p, Vec2& slope) {
    const float cell = t.spacing(), last = static_cast<float>(kN - 1);
    const float gx = std::clamp((p.x + t.extent() * 0.5f) / cell, 0.f, last);
    const float gy = std::clamp((p.y + t.extent() * 0.5f) / cell, 0.f, last);
    const std::size_t ix = std::min(static_cast<std::size_t>(gx), kN - 2);
    const std::size_t iy = std::min(static_cast<std::size_t>(gy), kN - 2);
    const float fx = gx - static_cast<float>(ix), fy = gy - static_cast<float>(iy);
    const float h00 = t.sample(ix, iy), h10 = t.sample(ix + 1, iy), h01 = t.sample(ix, iy + 1), h11 = t.sample(ix + 1, iy + 1);
    if (fx >= fy) { // (ix, iy), (ix + 1, iy), (ix + 1, iy + 1)
        slope = {(h10 - h00) / cell, (h11 - h10) / cell};
        return h00 + (h10 - h00) * fx + (h11 - h10) * fy;
    }
    slope = {(h11 - h01) / cell, (h01 - h00) / cell}; // (ix, iy), (ix + 1, iy + 1), (ix, iy + 1)
    return h00 + (h11 - h01) * fx + (h01 - h00) * fy;
}

float expectedWater(const WorldTerrain& t, Vec2 p) {
    Vec2 slope{};
    const float ground = expectedHeight(t, p, slope);
    float depth = 0.f;
    for (std::size_t f = 0; f < t.featureCount(); ++f) {
        const TerrainFeature& feat = t.feature(f);
        const bool water = feat.kind == FeatureKind::Sea || feat.kind == FeatureKind::Lake || feat.kind == FeatureKind::Pool;
        if (water && feat.covers(p)) depth = std::max(depth, feat.level - ground);
    }
    return depth;
}

// generateWorldTerrain on a copy of `ground` fails, says `why` after "terrain <id>: ", and leaves the
// copy with no ground.
void checkFails(const WorldTerrain& ground, const WorldSpec& spec, const WorldLayout& lay, const std::string& why) {
    INFO(why);
    auto copy = std::make_unique<WorldTerrain>(ground);
    REQUIRE(copy->side() > 0.f);
    std::string error;
    CHECK_FALSE(generateWorldTerrain(spec, lay, *copy, &error));
    INFO(error);
    CHECK(error.rfind("terrain " + spec.id + ": ", 0) == 0u);
    CHECK(error.find(why) != std::string::npos);
    CHECK(copy->side() == 0.f);
    CHECK(copy->districtCount() == 0u);
    CHECK(copy->roadCount() == 0u);
    CHECK(copy->featureCount() == 0u);
    CHECK(copy->height({0.f, 0.f}) == 0.f);
    CHECK(copy->gradient({0.f, 0.f}).x == 0.f);
    CHECK(copy->gradient({0.f, 0.f}).y == 0.f);
    CHECK(copy->waterDepth({0.f, 0.f}) == 0.f);
}

} // namespace

TEST_CASE("terrain: each world's ground lies on whole steps inside the height range") {
    for (const Generated& g : worlds()) {
        INFO(g.spec.id);
        REQUIRE_MESSAGE(g.ok, g.error);
        const WorldTerrain& t = *g.terrain;
        MESSAGE(g.spec.id << ": ground generated in " << g.seconds << " s");
        CHECK(t.side() == g.spec.side);
        CHECK(t.extent() == g.spec.side + 2.f * WorldTerrain::kApron);
        CHECK(t.districtCount() == g.layout->districtCount());
        CHECK(t.roadCount() == g.layout->roadCount());
        Breaches offStep, outOfRange, badValue;
        for (std::size_t iy = 0; iy < kN; ++iy)
            for (std::size_t ix = 0; ix < kN; ++ix) {
                const float h = t.sample(ix, iy), units = h / kStep;
                const float off = std::fabs(units - std::round(units));
                if (off > 1e-3f) offStep.add(off, ix, iy);
                if (!(std::fabs(h) <= WorldTerrain::kMaxAbsHeight)) outOfRange.add(std::fabs(h), ix, iy);
                const float back = static_cast<float>(static_cast<int>(t.heightmapValue(ix, iy)) - 32768) * kStep;
                if (back != h) badValue.add(std::fabs(back - h), ix, iy);
            }
        CHECK_MESSAGE(offStep.count == 0u, "off a whole step: " << describe(offStep));
        CHECK_MESSAGE(outOfRange.count == 0u, "out of range: " << describe(outOfRange));
        CHECK_MESSAGE(badValue.count == 0u, "heightmap value off: " << describe(badValue));
    }
}

TEST_CASE("terrain: the entry stands at the base and the climax over every other pad and all the ground") {
    for (const Generated& g : worlds()) {
        INFO(g.spec.id);
        REQUIRE_MESSAGE(g.ok, g.error);
        REQUIRE(g.entry >= 0);
        REQUIRE(g.climax >= 0);
        const WorldTerrain& t = *g.terrain;
        const float top = g.top();
        MESSAGE(g.spec.id << ": the climax stands " << top << " m up");
        CHECK(t.padHeight(static_cast<std::size_t>(g.entry)) == 0.f);
        CHECK(top <= kClimaxWant);
        for (std::size_t i = 0; i < t.districtCount(); ++i) {
            INFO("district " << i);
            const float h = t.padHeight(i);
            CHECK(std::fabs(h / kStep - std::round(h / kStep)) <= 1e-3f);
            if (static_cast<int>(i) != g.climax) {
                CHECK(h <= top - (isMajor(g, i) ? kClimaxClear : 1.f));
            }
        }
        Breaches over;
        for (std::size_t iy = 0; iy < kN; ++iy)
            for (std::size_t ix = 0; ix < kN; ++ix) {
                const float h = t.sample(ix, iy);
                if (g.inside(g.at(ix, iy)) && h > top + kStepTol) over.add(h - top, ix, iy);
            }
        CHECK_MESSAGE(over.count == 0u, "over the climax: " << describe(over));
    }
}

TEST_CASE("terrain: every pad is flat out to kFlatRadius") {
    for (const Generated& g : worlds()) {
        INFO(g.spec.id);
        REQUIRE_MESSAGE(g.ok, g.error);
        const WorldTerrain& t = *g.terrain;
        const float inner = kR - 1.42f * t.spacing(); // the triangle under a point this near lies on the pad
        Breaches onSamples, onTriangles;
        for (std::size_t i = 0; i < t.districtCount(); ++i) {
            const Vec2 c = g.layout->district(i).centre;
            const float pad = t.padHeight(i);
            std::size_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
            samplesOver(t, c.x - kR, c.x + kR, x0, x1);
            samplesOver(t, c.y - kR, c.y + kR, y0, y1);
            for (std::size_t iy = y0; iy <= y1; ++iy)
                for (std::size_t ix = x0; ix <= x1; ++ix)
                    if (between(g.at(ix, iy), c) <= kR - 0.01f && t.sample(ix, iy) != pad)
                        onSamples.add(std::fabs(t.sample(ix, iy) - pad), ix, iy);
            for (float dy = -inner; dy <= inner; dy += 2.1f)
                for (float dx = -inner; dx <= inner; dx += 2.1f) {
                    if (std::hypot(dx, dy) > inner) continue;
                    const float off = std::fabs(t.height({c.x + dx, c.y + dy}) - pad);
                    if (off > 1e-4f) onTriangles.add(off, i);
                }
        }
        CHECK_MESSAGE(onSamples.count == 0u, "samples off their pad: " << describe(onSamples));
        CHECK_MESSAGE(onTriangles.count == 0u, "points off their pad: " << describe(onTriangles));
    }
}

TEST_CASE("terrain: a road is flat on its pads, climbs straight and gently between them, and its strip lies just under it") {
    for (const Generated& g : worlds()) {
        INFO(g.spec.id);
        REQUIRE_MESSAGE(g.ok, g.error);
        const WorldTerrain& t = *g.terrain;
        Breaches profile, strip;
        for (std::size_t r = 0; r < t.roadCount(); ++r) {
            INFO("road " << r);
            const WorldRoad& road = g.layout->road(r);
            const Vec2 a = g.layout->district(road.a).centre, b = g.layout->district(road.b).centre;
            const float length = between(a, b), from = t.padHeight(road.a), to = t.padHeight(road.b);
            const float ramp = length - 2.f * kR;
            REQUIRE(ramp > 0.f);
            CHECK(t.roadHeight(r, 0.f) == from);
            CHECK(t.roadHeight(r, 1.f) == to);
            CHECK(std::fabs(to - from) <= WorldTerrain::kMaxRoadGrade * ramp + 1e-3f);
            CHECK(std::fabs(t.roadHeight(r, 0.5f) - 0.5f * (from + to)) <= 1e-4f);
            for (float s = 0.f; s <= length; s += 1.1f) {
                const float u = s / length;
                const float want = s <= kR ? from : s >= length - kR ? to : lerpf(from, to, (s - kR) / ramp);
                const float off = std::fabs(t.roadHeight(r, u) - want);
                if (off > 1e-3f) profile.add(off, r);
                // The strip under it, kRoadCarve below the road once the carve is at its full depth.
                if (s < kR + 8.f || s > length - kR - 8.f) continue;
                const float ground = t.height({lerpf(a.x, b.x, u), lerpf(a.y, b.y, u)});
                const float under = std::fabs(ground - (t.roadHeight(r, u) - WorldTerrain::kRoadCarve));
                if (under > kStepTol + 1e-3f) strip.add(under, r);
            }
        }
        CHECK_MESSAGE(profile.count == 0u, "off the road's profile: " << describe(profile));
        CHECK_MESSAGE(strip.count == 0u, "strip off the road: " << describe(strip));
    }
}

TEST_CASE("terrain: the ground beside a road banks no steeper than kBankSlope") {
    for (const Generated& g : worlds()) {
        INFO(g.spec.id);
        REQUIRE_MESSAGE(g.ok, g.error);
        const WorldTerrain& t = *g.terrain;
        Breaches outside, crossed;
        for (std::size_t iy = 0; iy < kN; ++iy)
            for (std::size_t ix = 0; ix < kN; ++ix) {
                // Beside a road out to kFeatureClear past its edge, off the pads.
                const std::size_t i = iy * kN + ix;
                if (g.padGap[i] <= 1e-3f || g.roadGap[i] < WorldTerrain::kRoadHalfWidth ||
                    g.roadGap[i] >= kLevel + kFeatureClear)
                    continue;
                float lo = 0.f, hi = 0.f;
                bankBounds(g, g.at(ix, iy), lo, hi);
                if (lo > hi) {
                    crossed.add(lo - hi, ix, iy);
                    continue;
                }
                const float h = t.sample(ix, iy), by = std::max(lo - h, h - hi);
                if (by > kStepTol + 1e-3f) outside.add(by, ix, iy);
            }
        MESSAGE(g.spec.id << ": samples where two roads' banks cross: " << describe(crossed));
        CHECK_MESSAGE(outside.count == 0u, "outside the banks: " << describe(outside));
    }
}

TEST_CASE("terrain: the climax is in sight from the entry") {
    for (const Generated& g : worlds()) {
        INFO(g.spec.id);
        REQUIRE_MESSAGE(g.ok, g.error);
        REQUIRE(g.entry >= 0);
        REQUIRE(g.climax >= 0);
        const WorldTerrain& t = *g.terrain;
        const Sight sight = sightOf(g);
        // In the corridor, off the pads: under the ceiling, unless a road's strip or banks hold the
        // ground over it; and under the line itself regardless.
        Breaches unforced, forced, overLine;
        for (std::size_t iy = 0; iy < kN; ++iy)
            for (std::size_t ix = 0; ix < kN; ++ix) {
                const std::size_t i = iy * kN + ix;
                if (g.padGap[i] <= 1e-3f || g.padGap[i] >= kFar) continue;
                const Vec2 p = g.at(ix, iy);
                float d = 0.f;
                const float ceiling = sight.ceiling(p, d);
                if (d >= kCorridorHalf) continue;
                const float h = t.sample(ix, iy);
                if (h > ceiling + kSightClear + kStepTol) overLine.add(h - ceiling - kSightClear, ix, iy);
                if (h <= ceiling + kStepTol) continue;
                float lo = 0.f, hi = 0.f;
                bankBounds(g, p, lo, hi);
                if (g.roadGap[i] < WorldTerrain::kRoadHalfWidth || lo > ceiling) forced.add(h - ceiling, ix, iy);
                else unforced.add(h - ceiling, ix, iy);
            }
        // Along the line from the eye, short of the climax's pad.
        Breaches inTheWay;
        const float run = between(sight.from, sight.to);
        for (float s = 2.f; s <= run - kR; s += 2.f) {
            const float u = s / run;
            const float h = t.height({lerpf(sight.from.x, sight.to.x, u), lerpf(sight.from.y, sight.to.y, u)});
            const float line = lerpf(sight.zFrom, sight.zTo, u);
            if (h > line) inTheWay.add(h - line);
        }
        MESSAGE(g.spec.id << ": over the sightline's ceiling where a road holds it: " << describe(forced));
        CHECK_MESSAGE(unforced.count == 0u, "over the ceiling: " << describe(unforced));
        CHECK_MESSAGE(overLine.count == 0u, "over the line: " << describe(overLine));
        CHECK_MESSAGE(inTheWay.count == 0u, "in the way: " << describe(inTheWay));
    }
}

TEST_CASE("terrain: each world has only its own features, each where it may go and shaped as promised") {
    for (const Generated& g : worlds()) {
        INFO(g.spec.id);
        REQUIRE_MESSAGE(g.ok, g.error);
        REQUIRE(g.entry >= 0);
        REQUIRE(g.climax >= 0);
        const WorldTerrain& t = *g.terrain;
        REQUIRE(t.featureCount() <= WorldTerrain::kMaxFeatures);
        const auto countOf = [&t](FeatureKind kind) {
            std::size_t n = 0;
            for (std::size_t f = 0; f < t.featureCount(); ++f) n += t.feature(f).kind == kind ? 1u : 0u;
            return n;
        };
        const std::size_t seas = countOf(FeatureKind::Sea), lakes = countOf(FeatureKind::Lake);
        const std::size_t pools = countOf(FeatureKind::Pool), trenches = countOf(FeatureKind::Trench);
        const std::size_t pits = countOf(FeatureKind::Pit);
        MESSAGE(g.spec.id << ": " << seas << " sea, " << lakes << " lakes, " << pools << " pools, " << trenches
                          << " trenches, " << pits << " pits");
        switch (g.spec.kind) {
        case WorldKind::Town:
            CHECK(seas == 1u);
            CHECK(lakes <= 1u);
            CHECK(seas + lakes == t.featureCount());
            break;
        case WorldKind::Towers:
            CHECK(trenches <= 2u);
            CHECK(pools <= 3u);
            CHECK(trenches + pools == t.featureCount());
            break;
        case WorldKind::Ruins:
            CHECK(pits <= 2u);
            CHECK(lakes <= 1u);
            CHECK(pits + lakes == t.featureCount());
            break;
        }
        const Sight sight = sightOf(g);
        for (std::size_t f = 0; f < t.featureCount(); ++f) {
            INFO("feature " << f);
            const TerrainFeature& feat = t.feature(f);
            switch (feat.kind) {
            case FeatureKind::Sea:
                CHECK(f == 0u);
                checkSea(g, feat);
                continue;
            case FeatureKind::Trench:
            case FeatureKind::Pit: checkCut(g, feat); break;
            case FeatureKind::Pool: checkPool(g, feat); break;
            case FeatureKind::Lake: break; // none is placed yet
            }
            checkPlacement(g, feat, sight);
        }
        checkApart(g);
    }
}

TEST_CASE("terrain: past the square's edge each kind closes the view its own way") {
    for (const Generated& g : worlds()) {
        INFO(g.spec.id);
        REQUIRE_MESSAGE(g.ok, g.error);
        REQUIRE(g.climax >= 0);
        const WorldTerrain& t = *g.terrain;
        const float halfSide = g.halfSide();
        if (g.spec.kind == WorldKind::Ruins) {
            // The ruins' ground runs on a ragged way past the edge, then breaks off into the abyss.
            Breaches abyss, lip;
            float lowestLip = kFar;
            for (std::size_t iy = 0; iy < kN; ++iy)
                for (std::size_t ix = 0; ix < kN; ++ix) {
                    const Vec2 p = g.at(ix, iy);
                    const float ex = std::fabs(p.x) - halfSide, ey = std::fabs(p.y) - halfSide;
                    const float out = std::hypot(std::max(ex, 0.f), std::max(ey, 0.f)), h = t.sample(ix, iy);
                    if (out >= kAbyssOut && (h > kAbyssTop + kStepTol || h < kAbyssBottom - kStepTol))
                        abyss.add(std::max(h - kAbyssTop, kAbyssBottom - h), ix, iy);
                    if (out > 0.f && out < kLipNear) {
                        lowestLip = std::min(lowestLip, h);
                        if (h <= kLipFloor) lip.add(kLipFloor - h, ix, iy);
                    }
                }
            MESSAGE(g.spec.id << ": the ground past the edge lies " << lowestLip << " m at its lowest within " << kLipNear
                              << " m");
            CHECK_MESSAGE(abyss.count == 0u, "abyss floor off: " << describe(abyss));
            CHECK_MESSAGE(lip.count == 0u, "ground breaking off too soon: " << describe(lip));
            continue;
        }
        int coast = -1;
        if (g.spec.kind == WorldKind::Town) {
            REQUIRE(t.featureCount() >= 1u);
            REQUIRE(t.feature(0).kind == FeatureKind::Sea);
            coast = static_cast<int>(t.feature(0).side);
        }
        // Mountains all round, past the town's coast only as headlands.
        const float top = g.top();
        Breaches low;
        float lowest = kFar;
        for (std::size_t iy = 0; iy < kN; ++iy)
            for (std::size_t ix = 0; ix < kN; ++ix) {
                const Vec2 p = g.at(ix, iy);
                float ex = std::max(past(p, halfSide, 0), past(p, halfSide, 2));
                float ey = std::max(past(p, halfSide, 1), past(p, halfSide, 3));
                if (coast >= 0) {
                    if (past(p, halfSide, coast) > 0.f) continue;
                    const float dry = past(p, halfSide, (coast + 2) % 4); // on the coast's axis only the far edge
                    if (coast % 2 == 0) ex = dry;
                    else ey = dry;
                }
                if (std::hypot(std::max(ex, 0.f), std::max(ey, 0.f)) < kRangeOut) continue;
                const float h = t.sample(ix, iy);
                lowest = std::min(lowest, h);
                if (h < top + kRangeOver) low.add(top + kRangeOver - h, ix, iy);
            }
        MESSAGE(g.spec.id << ": the mountains stand " << lowest << " m at their lowest from " << kRangeOut
                          << " m past the edges");
        CHECK_MESSAGE(low.count == 0u, "mountains too low: " << describe(low));
        if (coast < 0) continue;
        // The town's quay drops kQuayDrop or more within its first kQuayRun, and the sea's floor lies a
        // metre or more under the sea beyond, all along the coast.
        const float sea = t.feature(0).level;
        const bool alongX = coast % 2 == 0; // the coast faces along x: each row of samples runs out along x
        Breaches floorHigh, quay;
        for (std::size_t k = 0; k < kN; ++k) {
            const Vec2 probe = alongX ? g.at(0, k) : g.at(k, 0);
            if (std::fabs(alongX ? probe.y : probe.x) > halfSide) continue;
            float inC = -kFar, inH = 0.f, outC = kFar, outH = 0.f;
            for (std::size_t m = 0; m < kN; ++m) {
                const std::size_t ix = alongX ? m : k, iy = alongX ? k : m;
                const float c = past(g.at(ix, iy), halfSide, coast), h = t.sample(ix, iy);
                if (c >= kQuayRun && h > sea - 1.f + 1e-4f) floorHigh.add(h - (sea - 1.f), ix, iy);
                if (c <= 0.f && c > inC) {
                    inC = c;
                    inH = h;
                }
                if (c >= kQuayRun && c < outC) {
                    outC = c;
                    outH = h;
                }
            }
            if (inH - outH < kQuayDrop - 2.f * kStepTol) quay.add(kQuayDrop - (inH - outH), k);
        }
        CHECK_MESSAGE(floorHigh.count == 0u, "sea floor too high: " << describe(floorHigh));
        CHECK_MESSAGE(quay.count == 0u, "quay too low: " << describe(quay));
    }
}

TEST_CASE("terrain: height, slope and water come from two triangles a cell, without allocating") {
    for (const Generated& g : worlds()) {
        INFO(g.spec.id);
        REQUIRE_MESSAGE(g.ok, g.error);
        const WorldTerrain& t = *g.terrain;
        const float cell = t.spacing();
        constexpr float kInCell[4][2] = {{0.25f, 0.75f}, {0.75f, 0.25f}, {0.5f, 0.2f}, {0.35f, 0.6f}};
        Breaches heightOff, slopeOff, waterOff;
        for (std::size_t iy = 0; iy + 1 < kN; iy += 13)
            for (std::size_t ix = 0; ix + 1 < kN; ix += 11)
                for (const auto& frac : kInCell) {
                    const Vec2 corner = g.at(ix, iy);
                    const Vec2 p{corner.x + frac[0] * cell, corner.y + frac[1] * cell};
                    Vec2 slope{};
                    const float want = expectedHeight(t, p, slope);
                    const Vec2 got = t.gradient(p);
                    const float hOff = std::fabs(t.height(p) - want);
                    const float sOff = std::max(std::fabs(got.x - slope.x), std::fabs(got.y - slope.y));
                    const float wOff = std::fabs(t.waterDepth(p) - expectedWater(t, p));
                    if (hOff > 1e-4f) heightOff.add(hOff, ix, iy);
                    if (sOff > 1e-4f) slopeOff.add(sOff, ix, iy);
                    if (wOff > 1e-4f) waterOff.add(wOff, ix, iy);
                }
        CHECK_MESSAGE(heightOff.count == 0u, "height off: " << describe(heightOff));
        CHECK_MESSAGE(slopeOff.count == 0u, "slope off: " << describe(slopeOff));
        CHECK_MESSAGE(waterOff.count == 0u, "water off: " << describe(waterOff));
        // Outside the field, its nearest edge.
        CHECK(t.height({-1e4f, -1e4f}) == t.sample(0, 0));
        CHECK(t.height({1e4f, -1e4f}) == t.sample(kN - 1, 0));
        CHECK(t.height({-1e4f, 1e4f}) == t.sample(0, kN - 1));
        CHECK(t.height({1e4f, 1e4f}) == t.sample(kN - 1, kN - 1));

        const bool counting = ftcore_test::allocHookInstalled();
        const std::uint64_t before = ftcore_test::allocStats().count;
        float sink = 0.f;
        for (int k = 0; k < 2000; ++k) {
            const float u = static_cast<float>(k) / 2000.f;
            const Vec2 p{lerpf(-0.6f, 0.6f, u) * t.extent(), lerpf(0.55f, -0.55f, u) * t.extent()};
            sink += t.height(p) + t.gradient(p).x + t.waterDepth(p);
            if (t.roadCount() > 0) sink += t.roadHeight(static_cast<std::size_t>(k) % t.roadCount(), u);
            for (std::size_t f = 0; f < t.featureCount(); ++f)
                sink += (t.feature(f).covers(p) ? 1.f : 0.f) + t.feature(f).radius(7.f * u);
        }
        if (counting) {
            CHECK(ftcore_test::allocStats().count == before);
        }
        CHECK(std::isfinite(sink));
    }
}

TEST_CASE("terrain: only the sheer walls are marked as cliffs") {
    for (const Generated& g : worlds()) {
        INFO(g.spec.id);
        REQUIRE_MESSAGE(g.ok, g.error);
        const WorldTerrain& t = *g.terrain;
        const float cell = t.spacing();
        const int coast = g.spec.kind == WorldKind::Town && t.featureCount() > 0 ? static_cast<int>(t.feature(0).side) : -1;
        std::size_t marked = 0;
        Breaches stray;
        for (std::size_t iy = 0; iy < kN; ++iy)
            for (std::size_t ix = 0; ix < kN; ++ix) {
                if (!t.cliff(ix, iy)) continue;
                ++marked;
                const Vec2 p = g.at(ix, iy);
                bool wall = false;
                switch (g.spec.kind) {
                case WorldKind::Town: wall = coast >= 0 && past(p, g.halfSide(), coast) > 0.f; break;
                case WorldKind::Towers: break;
                case WorldKind::Ruins:
                    wall = !g.inside(p);
                    for (std::size_t f = 0; f < t.featureCount() && !wall; ++f)
                        wall = t.feature(f).kind == FeatureKind::Pit && t.feature(f).covers(p, -cell);
                    break;
                }
                if (!wall) stray.add(1.f, ix, iy);
            }
        MESSAGE(g.spec.id << ": " << marked << " samples in sheer walls");
        CHECK_MESSAGE(stray.count == 0u, "marked off the walls: " << describe(stray));
    }
}

TEST_CASE("terrain: off the sheer walls no two neighbouring samples climb more than kRockSlope") {
    for (const Generated& g : worlds()) {
        INFO(g.spec.id);
        REQUIRE_MESSAGE(g.ok, g.error);
        const WorldTerrain& t = *g.terrain;
        const float cell = t.spacing(), diagonal = cell * std::sqrt(2.f);
        Breaches steep; // by the worst rise per metre
        const auto climb = [&](std::size_t ax, std::size_t ay, std::size_t bx, std::size_t by, float run) {
            if (t.cliff(ax, ay) || t.cliff(bx, by)) return;
            const float rise = std::fabs(t.sample(ax, ay) - t.sample(bx, by));
            if (rise > WorldTerrain::kRockSlope * run + 1e-3f) steep.add(rise / run, ax, ay);
        };
        for (std::size_t iy = 0; iy < kN; ++iy)
            for (std::size_t ix = 0; ix < kN; ++ix) {
                if (ix + 1 < kN) climb(ix, iy, ix + 1, iy, cell);
                if (iy + 1 < kN) climb(ix, iy, ix, iy + 1, cell);
                if (ix + 1 < kN && iy + 1 < kN) climb(ix, iy, ix + 1, iy + 1, diagonal);
                if (ix + 1 < kN && iy > 0) climb(ix, iy, ix + 1, iy - 1, diagonal);
            }
        CHECK_MESSAGE(steep.count == 0u, "neighbours climbing more than kRockSlope: " << describe(steep));
    }
}

TEST_CASE("terrain: the open ground has no terraces") {
    for (const Generated& g : worlds()) {
        INFO(g.spec.id);
        REQUIRE_MESSAGE(g.ok, g.error);
        const WorldTerrain& t = *g.terrain;
        // Terraced ground gathers near whole multiples of the terraces' height: 6 m among the towers,
        // 3 m elsewhere. Smooth ground lies within kTerraceNear of one 2 kTerraceNear / period of the
        // time; allow half as much again.
        const float period = g.spec.kind == WorldKind::Towers ? 6.f : 3.f;
        const double most = 1.5 * 2.0 * static_cast<double>(kTerraceNear / period);
        std::size_t open = 0, onTerrace = 0;
        for (std::size_t i = 0; i < kN * kN; ++i) {
            if (!openGround(g, i)) continue;
            ++open;
            const float h = t.sample(i % kN, i / kN);
            if (std::fabs(h - period * std::round(h / period)) <= kTerraceNear) ++onTerrace;
        }
        REQUIRE(open > 0u);
        const double share = static_cast<double>(onTerrace) / static_cast<double>(open);
        MESSAGE(g.spec.id << ": " << 100.0 * share << "% of " << open << " open samples lie within " << kTerraceNear
                          << " m of a multiple of " << period << " m (" << 100.0 * most << "% at most)");
        CHECK(share <= most);
    }
}

TEST_CASE("terrain: the open ground is seldom flat") {
    for (const Generated& g : worlds()) {
        INFO(g.spec.id);
        REQUIRE_MESSAGE(g.ok, g.error);
        const WorldTerrain& t = *g.terrain;
        const float cell = t.spacing();
        std::size_t cells = 0, flat = 0;
        for (std::size_t iy = 0; iy + 1 < kN; ++iy)
            for (std::size_t ix = 0; ix + 1 < kN; ++ix) {
                const std::size_t i = iy * kN + ix;
                if (!openGround(g, i) || !openGround(g, i + 1) || !openGround(g, i + kN) || !openGround(g, i + kN + 1))
                    continue;
                ++cells;
                const float h00 = t.sample(ix, iy), h10 = t.sample(ix + 1, iy), h01 = t.sample(ix, iy + 1),
                            h11 = t.sample(ix + 1, iy + 1);
                if (std::max(std::hypot(h10 - h00, h11 - h10), std::hypot(h11 - h01, h01 - h00)) / cell <= kFlatSlope) ++flat;
            }
        REQUIRE(cells > 0u);
        const double share = static_cast<double>(flat) / static_cast<double>(cells);
        MESSAGE(g.spec.id << ": " << 100.0 * share << "% of " << cells << " open cells slope " << kFlatSlope
                          << " or less (" << 100.0 * kFlatShare << "% at most)");
        CHECK(share <= kFlatShare);
    }
}

TEST_CASE("terrain: a layout or a site that doesn't fit fails, says why and leaves no ground") {
    const Generated* town = nullptr;
    const Generated* other = nullptr;
    for (const Generated& g : worlds()) {
        if (!g.ok) continue;
        if (!town && g.spec.kind == WorldKind::Town) town = &g;
        else if (!other && g.spec.kind != WorldKind::Town) other = &g;
    }
    if (!town || !other) {
        MESSAGE("no town and other world generated; skipping");
        return;
    }
    const WorldTerrain& ground = *town->terrain;
    checkFails(ground, town->spec, *other->layout, "the layout isn't this world's");
    checkFails(ground, town->spec, *std::make_unique<WorldLayout>(), "the layout has no districts");

    const std::size_t n = town->layout->districtCount();
    WorldSpec beyond = town->spec;
    beyond.sites.push_back({static_cast<std::uint32_t>(n), SiteTier::Major});
    checkFails(ground, beyond, *town->layout,
               "site district " + std::to_string(n) + " is past the layout's " + std::to_string(n));

    REQUIRE_FALSE(town->spec.sites.empty());
    WorldSpec twice = town->spec;
    twice.sites.push_back(twice.sites.front());
    checkFails(ground, twice, *town->layout, "district " + std::to_string(twice.sites.front().district) + " is a site twice");

    std::size_t nonSite = n;
    for (std::size_t d = 0; d < n && nonSite == n; ++d) {
        bool site = false;
        for (const WorldSite& s : town->spec.sites) site = site || s.district == d;
        if (!site) nonSite = d;
    }
    REQUIRE(nonSite < n);
    WorldSpec second = town->spec;
    second.sites.push_back({static_cast<std::uint32_t>(nonSite), SiteTier::Entry});
    checkFails(ground, second, *town->layout, "more than one entry or climax");
}

namespace {

constexpr std::size_t kMask = 512; // 1.95 m pixels over a 1 km square: every centre line passes within 1 m of one

// The mask read bilinearly at p (metres) with clamped addressing, as the ground's material samples it.
float maskAt(const std::vector<std::uint8_t>& mask, float side, Vec2 p) {
    const float last = static_cast<float>(kMask - 1);
    const float u = std::clamp((p.x + 0.5f * side) / side * static_cast<float>(kMask) - 0.5f, 0.f, last);
    const float v = std::clamp((p.y + 0.5f * side) / side * static_cast<float>(kMask) - 0.5f, 0.f, last);
    const std::size_t x0 = std::min(static_cast<std::size_t>(u), kMask - 2);
    const std::size_t y0 = std::min(static_cast<std::size_t>(v), kMask - 2);
    const float fx = u - static_cast<float>(x0), fy = v - static_cast<float>(y0);
    const auto at = [&mask](std::size_t x, std::size_t y) { return static_cast<float>(mask[y * kMask + x]); };
    return lerpf(lerpf(at(x0, y0), at(x0 + 1, y0), fx), lerpf(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy);
}

} // namespace

TEST_CASE("terrain: the road mask fades from each road's centre line to nothing 2 m past its edge, without allocating") {
    constexpr float kHalfWidth = WorldTerrain::kRoadHalfWidth;                // 3 m: the road's edge
    constexpr float kGone = kHalfWidth + WorldTerrain::kRoadMaskFeather;      // 5 m: nothing from here
    const auto value = [](float d) {
        return 255.f * std::clamp(0.5f + (kHalfWidth - d) / (2.f * WorldTerrain::kRoadMaskFeather), 0.f, 1.f);
    };
    std::vector<std::uint8_t> mask(kMask * kMask, 0xab);
    for (const Generated& g : worlds()) {
        INFO(g.spec.id);
        REQUIRE_MESSAGE(g.ok, g.error);
        const WorldLayout& lay = *g.layout;
        const float side = lay.side(), pixel = side / static_cast<float>(kMask);
        const auto ends = [&lay](std::size_t r, Vec2& a, Vec2& b) {
            a = lay.district(lay.road(r).a).centre;
            b = lay.district(lay.road(r).b).centre;
        };

        const bool counting = ftcore_test::allocHookInstalled();
        const std::uint64_t before = ftcore_test::allocStats().count;
        roadMask(lay, kMask, mask.data());
        if (counting) {
            CHECK(ftcore_test::allocStats().count == before);
        }

        // The outermost ring is empty, so a clamped read past the square finds no road.
        std::size_t ring = 0;
        for (std::size_t k = 0; k < kMask; ++k)
            ring += static_cast<std::size_t>(mask[k] != 0) + static_cast<std::size_t>(mask[(kMask - 1) * kMask + k] != 0) +
                    static_cast<std::size_t>(mask[k * kMask] != 0) + static_cast<std::size_t>(mask[k * kMask + kMask - 1] != 0);
        CHECK(ring == 0u);
        CHECK(maskAt(mask, side, {-0.5f * side - 50.f, 0.f}) == 0.f);

        // Inside it every pixel holds the nearest road's value: 255 within 1 m of a centre line, 0 from
        // 5 m, the ramp between, and where two roads come near, the nearer one's.
        Breaches off;
        std::size_t junction = 0;
        std::vector<std::size_t> onLine(lay.roadCount(), 0);
        for (std::size_t iy = 1; iy + 1 < kMask; ++iy)
            for (std::size_t ix = 1; ix + 1 < kMask; ++ix) {
                const Vec2 p{-0.5f * side + (static_cast<float>(ix) + 0.5f) * pixel,
                             -0.5f * side + (static_cast<float>(iy) + 0.5f) * pixel};
                const float got = mask[iy * kMask + ix];
                float nearest = kFar, second = kFar;
                for (std::size_t r = 0; r < lay.roadCount(); ++r) {
                    Vec2 a, b;
                    ends(r, a, b);
                    float d = 0.f;
                    nearestOnSegment(p, a, b, d);
                    if (d < nearest) {
                        second = nearest;
                        nearest = d;
                    } else {
                        second = std::min(second, d);
                    }
                    if (d <= 1.f && got == 255.f) ++onLine[r];
                }
                const float by = std::fabs(got - value(nearest));
                if (by > 0.51f) off.add(by, ix, iy);
                if (second < kGone && value(second) + 2.f < value(nearest)) ++junction;
            }
        CHECK_MESSAGE(off.count == 0u, "pixels off their value: " << describe(off));
        for (std::size_t r = 0; r < lay.roadCount(); ++r) CHECK_MESSAGE(onLine[r] > 0u, "road " << r);
        MESSAGE(g.spec.id << ": " << junction << " pixels where two roads come near");
        CHECK(junction > 0u);

        // Read back as the material reads it, away from the other roads: about 128 at a road's edge, so
        // the distance it gives back (5 - 4 v / 255) lands on the edge; under 1.6 m on the centre line;
        // nothing a pixel and a half past 5 m.
        Breaches edgeOff, lineOff, pastOff;
        std::size_t reads = 0;
        for (std::size_t r = 0; r < lay.roadCount(); ++r) {
            Vec2 a, b;
            ends(r, a, b);
            const float len = between(a, b);
            const Vec2 across{-(b.y - a.y) / len, (b.x - a.x) / len};
            for (int k = 4; k <= 16; ++k) {
                const float t = static_cast<float>(k) / 20.f;
                const Vec2 c{lerpf(a.x, b.x, t), lerpf(a.y, b.y, t)};
                bool alone = true;
                for (std::size_t o = 0; o < lay.roadCount() && alone; ++o) {
                    if (o == r) continue;
                    Vec2 oa, ob;
                    ends(o, oa, ob);
                    float d = 0.f;
                    nearestOnSegment(c, oa, ob, d);
                    alone = d > 20.f;
                }
                if (!alone) continue;
                const float line = maskAt(mask, side, c);
                if (line < value(1.6f)) lineOff.add(value(1.6f) - line);
                for (const float s : {-1.f, 1.f}) {
                    ++reads;
                    const float edge = maskAt(mask, side, {c.x + s * kHalfWidth * across.x, c.y + s * kHalfWidth * across.y});
                    if (std::fabs(edge - 127.5f) > 1.5f) edgeOff.add(std::fabs(edge - 127.5f));
                    const float reach = kGone + 1.5f * pixel;
                    const float beyond = maskAt(mask, side, {c.x + s * reach * across.x, c.y + s * reach * across.y});
                    if (beyond != 0.f) pastOff.add(beyond);
                }
            }
        }
        CHECK(reads > 0u);
        CHECK_MESSAGE(edgeOff.count == 0u, "edge reads off 127.5: " << describe(edgeOff));
        CHECK_MESSAGE(lineOff.count == 0u, "centre-line reads under 1.6 m's value: " << describe(lineOff));
        CHECK_MESSAGE(pastOff.count == 0u, "reads past 5 m that aren't 0: " << describe(pastOff));
    }
}
