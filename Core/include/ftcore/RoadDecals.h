#pragma once

// The roads' wear, as data for instancing: cracks, oil and other stains, manhole covers, patch repairs and kerb grime,
// scattered over a road's carriageway slabs and pavements (RoadBuild::solids). Each decal is a thin plate lying on its
// slab's top (on its grade), a few millimetres over it and under the markings, drawn by one procedural material that
// reads the decal's kind and seed from the instance's custom data (roadDecalHlsl).
//
// Plan frame as MassingBox (x east, y north, metres, rot counter-clockwise from +x; massingEnginePoint takes points to
// the engine). The scatter is seeded and repeatable: a slab's decals depend on the seed and the slab alone.
//
// Plain C++20. roadDecals appends to its output (build time only); the rest doesn't allocate.

#include "ftcore/Api.h"
#include "ftcore/MassingPlan.h"
#include "ftcore/Roads.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ftcore {

enum class RoadDecalKind : std::uint8_t { Crack, Stain, Manhole, Patch, KerbGrime, Count };
inline constexpr std::size_t kRoadDecalKinds = static_cast<std::size_t>(RoadDecalKind::Count);

// A decal's top stands about this far over its slab's top (each kind half a millimetre apart), under the markings'
// 6 mm.
inline constexpr double kRoadDecalLift = 0.003;
// Kerb grime: the strip it lies in, from the gutter edge inwards, and the run each piece covers.
inline constexpr double kRoadGrimeWide = 0.45;
inline constexpr double kRoadGrimeRun = 4.0;

struct RoadDecal {
    RoadDecalKind kind = RoadDecalKind::Crack;
    double cx = 0.0, cy = 0.0;     // plan m: the top face's centre
    double sx = 1.0, sy = 1.0;     // m: along u (rot) and w
    double rot = 0.0;              // degrees
    double top = 0.0;              // m: the top face's height at its centre
    double riseU = 0.0, riseW = 0.0; // its grade, per m along its own u and w (its slab's, turned with it)
    float seed = 0.f;              // [0, 1): picks the pattern
};

// How many of each, per 100 m2 of slab (Manhole: per 100 m of carriageway length; KerbGrime: the chance a kRoadGrimeRun
// stretch of gutter has a piece), all times density.
struct RoadDecalSpec {
    double density = 1.0;
    double crackAsphalt = 0.5, crackPavement = 0.25;
    double stainAsphalt = 0.35, stainPavement = 0.3;
    double patchAsphalt = 0.12;
    double manholePer100m = 1.6;
    double grimeChance = 0.35;
    std::size_t maxDecals = 20000; // over the whole output: the scatter stops adding when it's reached
    std::uint32_t seed = 0x0dec4a15u;
};

struct RoadDecalCounts {
    std::array<std::size_t, kRoadDecalKinds> perKind{};
    std::size_t slabs = 0;           // asphalt, fill and pavement slabs read
    double asphaltM2 = 0.0, pavementM2 = 0.0;
    std::size_t capped = 0;          // decals left out at maxDecals
    std::size_t total() const {
        std::size_t n = 0;
        for (const std::size_t k : perKind) {
            n += k;
        }
        return n;
    }
};

// Which slabs take decals: the carriageway ("asphalt", "fill") and the pavements ("pavement"); kerbs, markings and
// furniture don't.
FTCORE_API bool roadDecalSlab(const RoadSolid& solid);

// Appends the decals of solids[0, count) to out and adds to counts. Every decal lies inside its slab's top (its
// footprint turned with it fits within the slab, 5 cm in from each edge); slabs under 1 m across take none. Cracks,
// stains and patches on the carriageway, cracks and stains on the pavements, manholes along a carriageway half's middle,
// and kerb grime along each carriageway half's low (gutter) edge, its +w side facing the kerb.
FTCORE_API void roadDecals(const RoadSolid* solids, std::size_t count, const RoadDecalSpec& spec, std::vector<RoadDecal>& out,
                           RoadDecalCounts& counts);

// The decal as a box (kind "decal"), z1 its top and z0 under it by height, for massingCubePose and roadCubeCentre.
FTCORE_API MassingBox roadDecalBox(const RoadDecal& decal, double height);

// The instance's custom data the material reads: kind, seed, size along u and w (m).
inline constexpr std::size_t kRoadDecalData = 4;
FTCORE_API void roadDecalData(const RoadDecal& decal, float out[kRoadDecalData]);

// The material's Custom node (HLSL): returns the base colour (float3), with RoughOut and MaskOut (the opacity mask)
// as its two further outputs. Inputs: LocalPos (the 1 m plate's local position, cm), Kind, Seed, SizeU, SizeW (the
// instance's custom data) and Wetness (0..1).
FTCORE_API const char* roadDecalHlsl();

} // namespace ftcore
