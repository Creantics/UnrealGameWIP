#pragma once

// The far scenery around a massing map, as data for instancing: the hill's dressing (trees and scrub that thicken on
// its crests, and a pylon line along its ridge) so its scale reads, a skyline band of far blocks past the field's edge,
// and a shape mask the ground material breaks the hill's slopes up by.
//
// Every point and box is in the plan frame (x east, y north, metres; ftcore::massingEnginePoint takes them to the
// engine). Heights come from a uint16 height field read by massingGroundAt. The layouts are seeded and repeatable.
//
// Plain C++20. hillDress and skylineBand allocate their results (build time only); heightShapeMask doesn't allocate.

#include "ftcore/Api.h"
#include "ftcore/MassingPlan.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ftcore {

struct HeightField {
    const std::uint16_t* grid = nullptr;
    std::size_t samplesX = 0, samplesY = 0;
    double spacing = 1.0, originX = 0.0, originY = 0.0;
    double at(double x, double y) const { return massingGroundAt(grid, samplesX, samplesY, spacing, originX, originY, x, y); }
};

enum class BackdropKind : std::uint8_t { Tree, Scrub, Pylon };

struct BackdropPoint {
    float x = 0.f, y = 0.f, z = 0.f; // plan metres, z the ground
    float scale = 1.f;               // a share of the kind's own size (0.6..1.6)
    float yaw = 0.f;                 // degrees
    BackdropKind kind = BackdropKind::Scrub;
};

struct HillDressSpec {
    float minHeightM = 14.f;   // only ground this far over the datum is hill
    float cellM = 9.f;         // one candidate a cell, jittered within it
    float ringM = 40.f;        // the crest test: the ground stands over the mean of 8 points this far round it ...
    float crestRiseM = 3.f;    // ... by this much
    float maxSlope = 1.1f;     // rise over run past which the ground stays bare rock
    float crestTree = 0.7f;    // chance of a tree on a crest cell, scrub takes crestScrub more
    float crestScrub = 0.25f;
    float slopeTree = 0.1f;    // elsewhere on the hill: planted, never bare
    float slopeScrub = 0.22f;
    float clearM = 6.f;        // kept this far from every box's footprint
    float pylonStepM = 150.f;  // the pylon line: one a step along the hill's run (plan y), at the highest ground
    std::size_t maxPoints = 9000;
    std::uint32_t seed = 0x5eedu;
};

// The hill's dressing over the rectangle [x0, x1] x [y0, y1]. Pylons come first, then the rest by row. Boxes (any
// rotation) clear their footprint plus clearM. At most maxPoints points.
FTCORE_API std::vector<BackdropPoint> hillDress(const HeightField& field, const HillDressSpec& spec, double x0, double y0, double x1,
                                                double y1, const MassingBox* boxes, std::size_t boxCount);

struct BackdropBox {
    float cx = 0.f, cy = 0.f; // plan metres
    float sx = 1.f, sy = 1.f; // footprint
    float rot = 0.f;          // degrees
    float h = 1.f;            // height over its foot
};

struct SkylineSpec {
    double x0 = 0.0, y0 = 0.0, x1 = 1.0, y1 = 1.0; // the band, plan metres
    double nearX = 0.0, nearY = 0.0;               // the point the city is seen from: blocks grow taller away from it
    std::size_t count = 100;
    float minH = 18.f, maxH = 90.f;
    float minW = 14.f, maxW = 40.f;
    std::uint32_t seed = 0x5ca1eu;
};

// count blocks inside the band, footprints inside it, heights from minH to maxH (taller with the distance from near,
// and a few towers), seeded.
FTCORE_API std::vector<BackdropBox> skylineBand(const SkylineSpec& spec);

// A shape mask the ground master reads (ShapeMask: 128 even ground, 255 the sharpest ridge, 0 the deepest hollow)
// over a square sideM across centred on plan (centreX, centreY), size x size bytes, row iy then column ix; row 0 is
// the square's north edge when northUp (the engine's -Y, where the material's V starts), else its south edge. Each
// byte compares the ground with the mean of two rings (ringM and 3 ringM round it): 255 where it stands
// fullPerMetre * ring radius over them. No allocation.
FTCORE_API void heightShapeMask(const HeightField& field, double centreX, double centreY, double sideM, std::size_t size, bool northUp,
                                float ringM, float fullPerMetre, std::uint8_t* out);


// The ground's faces among the built boxes: a cliff or terrace box stands for the ground itself, so under -HillLook it
// wears the ground's rock (the ground master's Rock maps) instead of its plain concrete.
FTCORE_API bool backdropRockFace(const MassingBox& box);

// How a rock face wears the rock: the surface master's parameters, so a face of tens of metres shows no repeat.
struct RockFaceLook {
    float tileCm = 450.f;          // one tile of the rock photo
    float variation = 0.16f;       // fine mottle
    float variationScale = 300.f;  // cm
    float macroVariation = 0.32f;  // broad light and dark patches
    float macroScale = 3200.f;     // cm
    float normalStrength = 1.25f;
    float shade = 0.9f;            // the rock's tint times this: faces stand in their own shade more than open ground
};

// The share of the field's samples inside plan [x0, x1] x [y0, y1] (every step m) whose slope (rise over run) is at
// least minSlope; 0 for an empty field or box. No allocation.
FTCORE_API double heightSteepShare(const HeightField& field, double x0, double y0, double x1, double y1, double step, double minSlope);

// The hill's planting (never cones): a Tree point (on crests, and a few on the slopes) is a trunk (a unit cube, trunk set)
// under a crown of three rounded lumps (unit spheres) that overlap into one irregular canopy; a Scrub point is two
// rounded bush mounds sunk a fifth into the ground. Each lump has its own offset, size, yaw and one of
// kBackdropScrubTints tints. Pylons get none. Repeatable from the point.
struct BackdropLump {
    float dx = 0.f, dy = 0.f, dz = 0.f; // its centre from the point's foot, plan metres (dz up)
    float sx = 1.f, sy = 1.f, sz = 1.f; // its size, metres (sz the height; crowns up to 5 m deep, bushes up to 1.8 m)
    bool trunk = false;                 // a trunk: a cube in the bark tint, not a sphere
    float yaw = 0.f;                    // degrees
    std::uint8_t tint = 0;              // 0 .. kBackdropScrubTints - 1
};
constexpr std::size_t kBackdropClumpMax = 4;
constexpr std::size_t kBackdropScrubTints = 3;

// Writes the point's lumps into out (room for kBackdropClumpMax) and returns how many. No allocation.
FTCORE_API std::size_t backdropClump(const BackdropPoint& p, BackdropLump* out);

// The planted share of the ground: a mask the ground master tints by (255 low ground, the wards' paving and yards,
// which keep the ground's own colour; 0 the hill, which takes the green), over the same square, size and row order as
// heightShapeMask. Ground under minHeightM is 255, ground over minHeightM + rampM is 0, a smooth ramp between; and
// by slope (rise over run, 2 m either side): planted up to steepFrom, bare from steepTo (the master's rock takes the
// steep faces), a smooth ramp between. No allocation.
FTCORE_API void hillPlantMask(const HeightField& field, double centreX, double centreY, double sideM, std::size_t size, bool northUp,
                              float minHeightM, float rampM, float steepFrom, float steepTo, std::uint8_t* out);

// Red obstruction lamps on the tallest roofs: data for one instanced emissive head each (no light actors).
struct AviationSpec {
    float minHeightM = 30.f;  // a box this tall over its foot (z1 - z0) carries lamps
    float minSpacingM = 40.f; // no two lamps of different roofs nearer than this, tallest roofs first
    float insetM = 1.2f;      // a lamp stands this far in from the roof's corner
    float longRoofM = 24.f;   // a roof this long gets a second lamp at the far corner
    float headM = 0.5f;       // the lamp's foot stands this far over the roof
    std::size_t maxLamps = 40;
    // The far skyline band's blocks (BackdropBox, top at h) carry one lamp each from this height, up to maxSkyline.
    float skylineMinH = 70.f;
    std::size_t maxSkyline = 24;
};

struct AviationLamp {
    float x = 0.f, y = 0.f, z = 0.f; // plan metres, z the lamp's foot over the datum
    float heightM = 0.f;             // the roof's height over its foot, for logging
};

// Lamps on built boxes of kind kit, tower, mast or crane at least minHeightM tall (a mast or crane, or any roof
// narrower than 2 insetM, gets one at its centre), then on far skyline blocks; tallest first, spaced, capped.
// Repeatable. Allocates its result (build time only).
FTCORE_API std::vector<AviationLamp> aviationLamps(const AviationSpec& spec, const MassingBox* boxes, std::size_t boxCount,
                                                   const BackdropBox* skyline, std::size_t skylineCount);

} // namespace ftcore
