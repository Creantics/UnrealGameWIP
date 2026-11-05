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
// Last, the rain's way over the finished ground (flow()): each sample passes the rain that falls on
// it, and all that reaches it from uphill, on to every lower neighbour, more to the steeper, until it
// runs off the field's edge or into water, a trench's floor or a pit's shaft. Nothing fills the
// hollows: a closed hollow's floor, or a flat such as a pad, keeps what reaches it. Rain falls
// everywhere but on the pads and the roads' shoulders, fading in round them as the wear does, and
// flow() fades the same way, so the pads and the roads read 0.
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

// The swell's bend and the climax's ceiling of a world's core field (WorldTerrain::swell()). Its
// outer fields take the bend (TerrainField::swell) so that their swell is the core's; the ceiling
// holds in the core alone, so an outer field carries it but doesn't apply it.
struct TerrainSwell {
    std::array<Vec2, 2> at{};  // core metres
    std::array<float, 2> by{}; // m
    std::uint8_t count = 0;
    float ceiling = 1e30f;     // m: kClimaxClear under the climax; 1e30 without one
};

// A pad whose height a field takes as given instead of solving it (TerrainField::pins).
struct TerrainPin {
    std::uint8_t district = 0; // in the field's layout
    bool fromBase = true;      // terrainPinHeight() at the district's centre; else `height`
    float height = 0.f;        // m, a whole kHeightStep, inside +-kMaxAbsHeight: may stand past the
                               // pads' +-60 m (an upland hamlet), which binds only solved pads
};

// Where a field lies in a world bigger than one field. The default is the world's core field, the
// one every world had before: centred on the core frame's origin, closed at its own square's edge.
//
// Field (i, j) is centred i x (kSamples - 1) samples east and j north of the core's centre (1500 m a
// field at side 1000): sample (ix, iy) lies at -extent/2 + float(ix + i (kSamples - 1)) spacing on x
// (WorldTerrain::samplePosition()), so two neighbours compute their shared column's positions to the
// same bits. All positions are core metres: the world outline's minus 500 on each axis, as the
// outline's origin is the core square's south-west corner. An outer field's layout gives its
// districts' centres in that frame, and a district two fields hold has the same centre, to the bit,
// in both, so that the pins and the roads' strips match at the seam.
//
// An outer field (i, j) != (0, 0) has no sites, sea or features yet (the plan places them later),
// and no climax's ceiling: its ground before the pads is the core's swell and hills, uncapped. Its
// ground closes only at the world's box, kApron inside its edge as the core's closes at its square's,
// and a pad's centre stays kEdgeMargin inside that. Every pad within kPinReach of a line it shares
// with a neighbouring field, every pad outside its own square, and both ends of every road that
// leaves its square or comes within 90 m (its banks' reach) of such a line, must be pinned, with the
// same heights in every field that holds them; the rest it solves on its own. Its layout holds every
// pinned pad and every road whose banks reach its square, so the strips and shoulders on the shared
// column are the same in both. Where the pins leave a rule no room, whether among the pins or for
// the free pads, generation fails and names the pads: it never moves a pin, clamps a rule or
// re-seeds. A pad pinned far over the ground round it keeps its flat only where the pins round it
// raise that ground too (the slope limit cuts back what stands steeper than kRockSlope). The core
// field takes no pins and no swell (it makes them); its box, if given, is checked and otherwise
// unused.
struct TerrainField {
    static constexpr std::int32_t kMaxBox = 12000; // m: the longest side of a world's box
    static constexpr std::int32_t kBoxStep = 250;  // m: the box's edges lie on multiples of this
    static constexpr std::int32_t kHalo = 126;     // samples round a field for the stitch, later
    static constexpr float kPinReach = WorldLayout::kDistrictRadius + 60.f; // a pad's flat and widest blend

    std::int32_t i = 0, j = 0; // the key: fields east and north of the core
    std::int32_t halo = 0;     // samples; 0 only, until the stitch
    // The world's box, core metres: closed past these less kApron. All 0 for none (the core only).
    std::int32_t west = 0, east = 0, south = 0, north = 0;
    TerrainSwell swell{}; // the core's (WorldTerrain::swell()); outer fields only
    std::array<TerrainPin, WorldLayout::kMaxDistricts> pins{};
    std::uint8_t pinCount = 0;
};

// How generateWorldTerrain shapes the ground. The defaults make the game's ground.
struct TerrainOptions {
    bool erode = true;          // wear the open ground with rain; off only to measure what the wear adds
    bool erodeMountains = true; // wear the mountains past the square's edge; off only to measure, likewise
    TerrainField field{};       // which field of the world; the default is its core
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
    static constexpr float kTunedSide = 1000.f;       // m: the side the rain and flow() were tuned at
    static constexpr float kFlowUnit = 4.f;           // m^2 of rain, about a sample's at kTunedSide, that flow()'s log
                                                      // scale counts in there; at other sides it grows with a sample's area
    static constexpr float kFlowFull = 1e4f;          // m^2 (a hectare) whose rain runs through a sample where flow() reads 1

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
    // How much rain runs through the sample, 0 to 1: log(1 + a / u) / log(1 + kFlowFull / u), clamped
    // at 1, a the square metres of ground whose rain reaches it (where the rain divides, the share that
    // comes this way), its own cell's included, and u kFlowUnit times a sample's area over its area at
    // kTunedSide, so that it counts in about a sample's own rain at every side; then faded toward the pads and
    // the roads as the rain is, to 0 on them. Low on crests and open slopes, rising along the gullies
    // and the valleys' floors and on a closed hollow's floor, which keeps all that reaches it.
    float flow(std::size_t ix, std::size_t iy) const { return flow_[iy * kSamples + ix]; }

    // District i's pad height.
    float padHeight(std::size_t district) const { return pads_[district]; }

    // The field's key (TerrainField) and its centre, core metres: (0, 0) for the core.
    std::int32_t fieldX() const { return fieldX_; }
    std::int32_t fieldY() const { return fieldY_; }
    Vec2 origin() const {
        constexpr auto kStride = static_cast<std::int64_t>(kSamples - 1);
        return {static_cast<float>(fieldX_ * kStride) * spacing(), static_cast<float>(fieldY_ * kStride) * spacing()};
    }
    // Sample (ix, iy)'s position, core metres. No allocation.
    FTCORE_API Vec2 samplePosition(std::size_t ix, std::size_t iy) const;
    // The core's swell, its bend and its climax's ceiling: the core's own, or the one an outer field
    // took (TerrainField::swell), whose bend it used and whose ceiling it carries unapplied.
    const TerrainSwell& swell() const { return swell_; }

    // The ground at p (core metres; outside the field, its nearest edge). No allocation.
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
    std::int32_t fieldX_ = 0, fieldY_ = 0;
    TerrainSwell swell_{};
    std::size_t districtCount_ = 0, roadCount_ = 0;
    std::array<float, WorldLayout::kMaxDistricts> pads_{};
    std::array<Road, WorldLayout::kMaxRoads> roads_{};
    std::size_t featureCount_ = 0;
    std::array<TerrainFeature, kMaxFeatures> features_{};
    std::vector<float> heights_;        // kSamples * kSamples, row iy then column ix
    std::vector<std::uint8_t> cliffs_;  // the same: 1 in a sheer wall
    std::vector<float> flow_;           // the same: flow()
};

// Deterministic in the spec, the layout (generated from that spec) and the options. Fails, and says
// why, when the layout doesn't match the spec, a site names a district the layout doesn't have, or
// the ground can't be shaped (it would leave the height range, or its slopes don't settle).
FTCORE_API bool generateWorldTerrain(const WorldSpec& spec, const WorldLayout& layout, WorldTerrain& out,
                                     std::string* error = nullptr, const TerrainOptions& options = {});

// The height a pinned pad takes from the ground (TerrainPin::fromBase): the ground before the pads,
// with the core's swell bent as `swell` and under no ceiling, averaged over p and eight points
// 30 m round it, as a solved pad starts, then held to the pads' +-60 m and put on the nearest whole
// kHeightStep. A function of the spec, the swell and p only, so every field that pins a pad at p
// gives it the same height to the bit. No allocation.
FTCORE_API float terrainPinHeight(const WorldSpec& spec, const TerrainSwell& swell, Vec2 p);

// The roads as a mask over the playable square, for the ground's material: size x size bytes, row iy
// then column ix, pixel (ix, iy) centred at (-side/2 + (ix + 0.5) side/size, -side/2 + (iy + 0.5)
// side/size), shifted by `origin` (an outer field's, WorldTerrain::origin()). Each byte is 255
// clamp(0.5 + (kRoadHalfWidth - d) / (2 kRoadMaskFeather), 0, 1), d the
// distance to the nearest road's centre line: 255 out to 1 m, about 128 at the road's edge, 0 from
// 5 m, so a filtered read gives the distance back (d = 5 - 4 byte/255) wherever it lies between 1 and
// 5 m. The outermost ring is 0 whatever lies near it, so a clamped read past the square finds no road.
// Writes all size * size bytes. No allocation.
FTCORE_API void roadMask(const WorldLayout& layout, std::size_t size, std::uint8_t* out, Vec2 origin = {});

// The ground's shape as a mask over the whole field, for the ground's material: size x size bytes, row
// iy then column ix, pixel (ix, iy) centred at (-extent/2 + (ix + 0.5) extent/size, -extent/2 + (iy +
// 0.5) extent/size). At each centre t is how far the ground there stands over the mean of eight points
// on a ring round it, per metre of the ring's radius, averaged over rings of kShapeNear and kShapeFar
// samples, and the byte is clamp(round(128 + 127 t / kShapeFull), 0, 255): over 128 on ridges, crests
// and knolls, under it in hollows, gullies and valleys, and exactly 128 on ground that is flat or
// slopes evenly out to the outer ring, such as a pad's core. Writes all size * size bytes. No allocation.
FTCORE_API void shapeMask(const WorldTerrain& terrain, std::size_t size, std::uint8_t* out);

// Where the rain gathers, as a mask over the whole field, for the ground's material: size x size bytes,
// row iy then column ix, pixel centres as shapeMask()'s. Each byte is round(255 f), f the flow() read at
// the centre, straight between the four samples round it: 0 on the pads and the roads, 255 where a
// hectare drains through. All 0 for a terrain that wasn't generated. Writes all size * size bytes. No
// allocation.
FTCORE_API void flowMask(const WorldTerrain& terrain, std::size_t size, std::uint8_t* out);

} // namespace ftcore
