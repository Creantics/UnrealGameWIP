#pragma once

// The ground of a big world (ftcore/WorldLayout.h): one height field over the whole square, shaped
// by the world's kind and raised or sunk around its sites (WorldSpec::sites). The Unreal map builder
// imports it as the world's Landscape and stands the plazas, buildings and props on it; the game
// itself reads only the district floors the builder saves with the map.
//
//   auto terrain = std::make_unique<ftcore::WorldTerrain>();
//   if (!ftcore::generateWorldTerrain(spec, layout, *terrain, &error)) ...; // allocates: at load
//   const float z = terrain->height(p);                                      // no allocation
//
// The field is kSamples x kSamples heights over a square extent() metres across: the playable square
// (the layout's, side() across, walled in by the map) in the middle and kApron metres of mountains
// around it. kSamples - 1 cells of spacing() metres each: sample (0, 0) lies at (-extent/2,
// -extent/2), ix runs along +x and iy along +y. Between samples the ground is two triangles per cell,
// split along the (ix, iy)-(ix + 1, iy + 1) diagonal, the way a Landscape draws and collides. Heights
// are metres above the world's base, in multiples of kHeightStep, so a 16-bit Landscape at Z scale
// 100 stores them exactly (heightmapValue()).
//
// Every district stands on a pad that is flat at padHeight() out to kFlatRadius, covering the plaza
// and the building ring. A road is flat inside its two pads and climbs in a straight line between
// their edges, never steeper than kMaxRoadGrade, so three straight pieces draw it. The ground beside
// a road banks at kBankSlope at most, and the strip under it sits kRoadCarve below the road.
//
// The sites come first. The entry stays at the base. The climax is the world's highest pad: a
// hilltop in town, a rise among the towers, a steep-sided butte in the ruins. It stands 36 m up
// where the roads to it allow, and lower where that would hide it: the climax is in sight from the
// start, as the ground, with the pads and roads near the line, stays 1.5 m under a line from an eye
// over the entry pad's edge to a point 15 m over the climax's centre. Majors rise or sink by kind
// (town hills and bowls, tower rises, ruin bowls). The other pads settle between the sites and the
// noise, and the roads are graded to fit. Every pad height is a whole number of kHeightStep.
//
// Between the pads each kind makes its own ground: rolling hills in town; broader, higher hills
// among the towers; a low plain in the ruins, broken by mesas and gullies, the mesas kept clear of
// the climax, the roads and the sightline. Off the sheer walls (cliff()) the ground never climbs more
// than kRockSlope from one sample to the next: steeper ground is cut back until it doesn't, so it
// stands as rock at that pitch rather than as a step.
//
// Then rain wears that ground (TerrainOptions::erode): droplets run downhill over the square and a
// margin round it, cutting where they run fast and laying down what they carry where they slow, so
// that the hills take branching rills and valleys. Steep ground wears less, as rock. The pads, the
// roads' shoulders, the ruins' mesas and gullies and the ground just round each don't wear, and
// rain falling beside them mostly drains away. Worn ground stays within the roads' banks and under
// the climax and the sightline. The mountains, the coast, the abyss and the features come after it.
//
// Past the playable square's edge each kind closes the view its own way, never reaching into the
// square. Among the towers the ground rises into mountains whose foot wanders from the edge to 60 m
// out: a band of rock along it in broken stretches, then ridges that climb toward the field's edge and
// hide it from inside. The town has them on three sides and the sea on the fourth, the side whose
// ground lies lowest: a quay drops 7 m or more into the water within its first 6 m, the sea's floor
// falls away beyond it, and the mountains either side run on into the sea as headlands. The ruins run
// on 16 to 114 m past the edge in bays and headlands, crumble about 10 m and then drop sheer into an
// abyss 170 to 190 m deep.
//
// Rain wears the mountains too (TerrainOptions::erodeMountains), once they stand, but never inside
// the square, which it leaves exactly as it was. Their slopes come down a few metres, cut by gullies
// that rills run down, and fans at the gullies' feet bury the broken band of rock. Their steepest
// faces wear less and the steepest not at all, as bare rock. The outer 20 m along the field's edge
// stays exactly as raised, the horizon, and neither the town's coast nor the ruins wear.
//
// Each world has the features that suit it (feature()): the town its sea, the towers up to two
// trenches 18 to 30 m deep, their walls climbing under kRockSlope from a flat floor, and up to three
// reflecting pools on platforms cut level into the ground, the ruins one or two pits beside a road,
// each a funnel round a sheer shaft. The queries answer for lakes too, but none is placed yet. Two
// trenches lie 120 m apart or more, as do two pits, and two pools 80 m. A trench or a pit only cuts
// ground a player could walk round, so it cuts no ground off from the rest. Inside the square every
// feature keeps 25 m from the roads' edges, the pads' blends and the other features, 30 m from the
// walls and out of the sightline's corridor, and none stands above the ground around it, so all of
// the above holds with them. The pools are shallow throughout. Players are kept out of a blocked
// feature.
//
// The sites carry district indices and tiers only. The terrain never reads the story.

#include "ftcore/Api.h"
#include "ftcore/Explore.h"
#include "ftcore/WorldLayout.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ftcore {

// Water on a world's ground, or a cut into it (WorldTerrain::feature()).
struct TerrainFeature {
    enum class Kind : std::uint8_t {
        Sea,    // water at `level` over the half-plane past the playable square's `side` edge
        Lake,   // water at `level` over a star (centre, ray), deeper than kWadeDepth inside its rim
        Pool,   // water at `level` over a rectangle (centre, half), never deeper than kWadeDepth
        Trench, // a cut over a rectangle (centre, half): walls round a floor at `level`, `wall` in from its edge
        Pit,    // a cut over a star (centre, ray): a funnel round a sheer shaft down to its floor at `level`
    };
    static constexpr std::size_t kRays = 32;

    Kind kind = Kind::Sea;
    bool blocked = false;           // players are kept out from `rim` metres inside the footprint's edge
    std::uint8_t side = 0;          // a sea's edge: 0 +x, 1 +y, 2 -x, 3 -y
    float level = 0.f;              // m: the water's surface, or the floor
    float rim = 0.f;                // m
    float wall = 0.f;               // m in from a cut's edge that its walls reach: inside that its floor lies at `level`
    Vec2 centre{};                  // a sea's: the middle of its edge
    Vec2 half{};                    // a rectangle's half sizes
    std::array<float, kRays> ray{}; // a star's radius, m, at angles 2 pi k / kRays from +x

    // A star's radius toward `angle` (radians from +x), straight between its rays in angle. No
    // allocation.
    FTCORE_API float radius(float angle) const;
    // Whether p (world metres) lies inside the footprint shrunk by `inset` metres (grown, for a
    // negative inset; a star's along its rays). No allocation.
    FTCORE_API bool covers(Vec2 p, float inset = 0.f) const;
};

// How generateWorldTerrain shapes the ground. The defaults make the game's ground.
struct TerrainOptions {
    bool erode = true;          // wear the open ground with rain; off only to measure what the wear adds
    bool erodeMountains = true; // wear the mountains past the square's edge; off only to measure, likewise
};

class WorldTerrain {
public:
    static constexpr std::size_t kSamples = 757;      // per side: 6 x 6 Landscape components of 2 x 63 quads
    static constexpr float kApron = 250.f;            // m of mountains around the playable square
    static constexpr float kHeightStep = 1.f / 128.f; // m: a 16-bit Landscape's step at Z scale 100
    static constexpr float kMaxAbsHeight = 200.f;     // m, inside the Landscape's +-256
    static constexpr float kFlatRadius = WorldLayout::kDistrictRadius; // a pad's flat radius
    static constexpr float kMaxRoadGrade = 0.25f;     // a road's steepest climb (rise per metre, 14 degrees)
    static constexpr float kBankSlope = 0.65f;        // the ground beside a road (33 degrees)
    static constexpr float kRoadCarve = 0.15f;        // m: the strip under a road, below the road
    static constexpr float kRoadHalfWidth = WorldLayout::kRoadWidth * 0.5f;
    static constexpr float kShoulder = 1.5f;          // m of ground level with the road either side
    static constexpr float kRockSlope = 1.8f;         // the steepest ground off the sheer walls (61 degrees)
    static constexpr std::size_t kMaxFeatures = 8;
    static constexpr float kWadeDepth = 0.7f;         // m: water deeper than this is blocked
    static constexpr float kRoadMaskFeather = 2.f;    // m either side of a road's edge over which roadMask() fades
    static constexpr std::size_t kShapeNear = 3;      // samples: the radii of shapeMask()'s two rings
    static constexpr std::size_t kShapeFar = 8;
    static constexpr float kShapeFull = 0.5f;         // how far the ground stands over its rings, per metre of their
                                                      // radius, where shapeMask() reads 255 (and under them, 0)

    float side() const { return side_; } // the playable square's: the layout's
    float extent() const { return side_ + 2.f * kApron; }
    float spacing() const { return extent() / static_cast<float>(kSamples - 1); }
    std::size_t districtCount() const { return districtCount_; }
    std::size_t roadCount() const { return roadCount_; }

    // One sample (ix, iy < kSamples), in metres.
    float sample(std::size_t ix, std::size_t iy) const { return heights_[iy * kSamples + ix]; }
    // The sample as a 16-bit Landscape value at Z scale 100: 32768 + height / kHeightStep.
    FTCORE_API std::uint16_t heightmapValue(std::size_t ix, std::size_t iy) const;
    // Whether the sample lies in one of the world's sheer walls, where the ground may drop more than
    // kRockSlope from one sample to the next. Everywhere else no two neighbouring samples, along an
    // axis or a diagonal, differ by more than kRockSlope times the distance between them.
    bool cliff(std::size_t ix, std::size_t iy) const { return cliffs_[iy * kSamples + ix] != 0; }

    // District i's pad height.
    float padHeight(std::size_t district) const { return pads_[district]; }

    // The ground at p (world metres; outside the field, its nearest edge). No allocation.
    FTCORE_API float height(Vec2 p) const;
    // The slope of the triangle under p: (dz/dx, dz/dy). No allocation.
    FTCORE_API Vec2 gradient(Vec2 p) const;
    // Road i (WorldLayout::road(i)) at t in 0..1 from its district a to its district b: flat inside
    // the pads, a straight climb between their edges. No allocation.
    FTCORE_API float roadHeight(std::size_t road, float t) const;

    std::size_t featureCount() const { return featureCount_; }
    // Feature i (< featureCount()); a sea comes first.
    const TerrainFeature& feature(std::size_t i) const { return features_[i]; }
    // How deep water stands over the ground at p: the deepest of the water features over it, 0 where
    // none is. No allocation.
    FTCORE_API float waterDepth(Vec2 p) const;

private:
    friend FTCORE_API bool generateWorldTerrain(const WorldSpec&, const WorldLayout&, WorldTerrain&, std::string*,
                                                const TerrainOptions&);

    struct Road {
        float length = 0.f;
        float from = 0.f, to = 0.f; // its pads' heights, a then b
    };

    float side_ = 0.f;
    std::size_t districtCount_ = 0, roadCount_ = 0;
    std::array<float, WorldLayout::kMaxDistricts> pads_{};
    std::array<Road, WorldLayout::kMaxRoads> roads_{};
    std::size_t featureCount_ = 0;
    std::array<TerrainFeature, kMaxFeatures> features_{};
    std::vector<float> heights_;        // kSamples * kSamples, row iy then column ix
    std::vector<std::uint8_t> cliffs_;  // the same: 1 in a sheer wall
};

// Deterministic in the spec, the layout (generated from that spec) and the options. Fails, and says
// why, when the layout doesn't match the spec, a site names a district the layout doesn't have, or
// the ground can't be shaped (it would leave the height range, or its slopes don't settle).
FTCORE_API bool generateWorldTerrain(const WorldSpec& spec, const WorldLayout& layout, WorldTerrain& out,
                                     std::string* error = nullptr, const TerrainOptions& options = {});

// The roads as a mask over the playable square, for the ground's material: size x size bytes, row iy
// then column ix, pixel (ix, iy) centred at (-side/2 + (ix + 0.5) side/size, -side/2 + (iy + 0.5)
// side/size). Each byte is 255 clamp(0.5 + (kRoadHalfWidth - d) / (2 kRoadMaskFeather), 0, 1), d the
// distance to the nearest road's centre line: 255 out to 1 m, about 128 at the road's edge, 0 from
// 5 m, so a filtered read gives the distance back (d = 5 - 4 byte/255) wherever it lies between 1 and
// 5 m. The outermost ring is 0 whatever lies near it, so a clamped read past the square finds no road.
// Writes all size * size bytes. No allocation.
FTCORE_API void roadMask(const WorldLayout& layout, std::size_t size, std::uint8_t* out);

// The ground's shape as a mask over the whole field, for the ground's material: size x size bytes, row
// iy then column ix, pixel (ix, iy) centred at (-extent/2 + (ix + 0.5) extent/size, -extent/2 + (iy +
// 0.5) extent/size). At each centre t is how far the ground there stands over the mean of eight points
// on a ring round it, per metre of the ring's radius, averaged over rings of kShapeNear and kShapeFar
// samples, and the byte is clamp(round(128 + 127 t / kShapeFull), 0, 255): over 128 on ridges, crests
// and knolls, under it in hollows, gullies and valleys, and exactly 128 on ground that is flat or
// slopes evenly out to the outer ring, such as a pad's core. Writes all size * size bytes. No allocation.
FTCORE_API void shapeMask(const WorldTerrain& terrain, std::size_t size, std::uint8_t* out);

} // namespace ftcore
