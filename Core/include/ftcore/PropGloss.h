#pragma once

// Props' surfaces: the import guard that keeps glossy or wrongly metal materials out, and the three layers the prop
// masters (M_FTPropFlat, M_FTPropTex, built in the Unreal module by FTImportProp) put over a material's authored
// values:
//
//   break-up  a material with no roughness map (RoughBreakup 1) moves its roughness up to +-breakupAmp on two scales of
//             world-space value noise (0.35 m and 0.06 m), rougher in cavities (AO); a smooth material (under
//             smoothFrom) keeps its gloss
//   grime     dust on up-facing surfaces (vertex normal z) and in cavities: rougher by up to grimeRough, a little less
//             saturated and a little toward the dust colour; "Grime" 0..1 per world (propGrimeFor: on in Solhaven)
//   wet       the look's Wetness: up faces soak (sides a little, undersides not), darken, and the roughness falls toward
//             WetRough (0.35) but never under wetFloor (0.25); only a material already smoother than that (glass,
//             water, glaze: the guard's whitelist) stays under it. Dust keeps the soak patchy. No puddles on a prop.
//
// Every layer is neutral at its parameter's 0 (RoughBreakup 0, Grime 0, Wetness 0): a material on a prop master with
// those values looks exactly as it did on the character master. propSurfaceHlsl() writes the layers as one Custom
// node's code; propSurface() is the same maths for the tests.
//
// The guard (propGlossCheck) fails a material when
//   - its name is not a metal (propNameMetal) and its metallic is over kPropMetalMax, or
//   - its name is not smooth (propNameSmooth: glaze, glass, water, mirror, polish, chrome) and its median roughness is
//     under kPropGlossMedianMin, or
//   - it has a roughness map, is not smooth by name, and the map's p90 - p10 is under kPropGlossSpreadMin (a flat
//     material, factor only, skips this rule; the break-up gives it its spread in the master).
// Names are matched on tokens: split at anything not a letter or digit, at a lower-to-upper case step and between
// letters and digits, any case, a word plus s, es, d, ed, y, en, ware or work also matching ("glazed", "ironwork").
//
// Plain C++20, float maths like the GPU's. No allocation except propSurfaceHlsl (build time only).

#include "ftcore/Api.h"
#include "ftcore/Glb.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace ftcore {

// ---- The import guard -------------------------------------------------------------------------------------------

inline constexpr float kPropGlossMedianMin = 0.40f;  // median roughness under this fails (not smooth by name)
inline constexpr float kPropGlossSpreadMin = 0.10f;  // a map's p90 - p10 under this fails (mapped, not smooth by name)
inline constexpr float kPropMetalMax = 0.5f;         // metallic over this fails on a name that is not a metal

// Whether a name is on the smooth whitelist (glaze, glass, water, mirror, polish, chrome) or names a metal (iron,
// metal, metallic, brass, bronze, steel, copper, chrome, zinc, silver, gold, gilt, pewter, nickel, aluminium,
// aluminum, alu, galv, galvanised, galvanized). No allocation.
FTCORE_API bool propNameSmooth(std::string_view name);
FTCORE_API bool propNameMetal(std::string_view name);

// One material as imported. Roughness and metallic are effective values: the map's channel times its factor (G for
// roughness, B for metallic in a glTF metallic-roughness map), or the factor alone (all three percentiles) on a flat
// material.
struct PropGlossInput {
    std::string_view name;
    bool hasMap = false;  // a roughness map of its own is bound (not the master's white default)
    float p10 = 1.f;
    float p50 = 1.f;
    float p90 = 1.f;
    float metallic = 0.f;  // the median
};

enum class PropGlossRule : std::uint8_t { None, Metal, Glossy, FlatMap };

struct PropGlossResult {
    bool pass = true;
    PropGlossRule failed = PropGlossRule::None;  // the first rule that failed, in the order Metal, Glossy, FlatMap
    bool wrongMetal = false;
    bool glossy = false;
    bool flatMap = false;
    bool smooth = false;         // on the smooth whitelist by name
    bool metal = false;          // a metal by name
    bool spreadChecked = false;  // false on a flat material and on a smooth one (the rule was skipped)
};
FTCORE_API PropGlossResult propGlossCheck(const PropGlossInput& in);
// A static line for a failed rule ("" for None). No allocation.
FTCORE_API const char* propGlossReason(PropGlossRule rule);

// The value (0..1) at share p (0..1) of a 256-bin histogram of 8-bit values: the lowest bin whose running count
// reaches p of the total (p 0: the lowest bin with any count). 0 on an empty histogram. No allocation.
FTCORE_API float propHistogramPercentile(const std::array<std::uint32_t, 256>& hist, float p);

// ---- Grime per world --------------------------------------------------------------------------------------------

// The prop masters' Grime for a world: Solhaven (and its ink variant) 1; the parked worlds 0 until their look is
// set; None (no world given) is Solhaven's. No allocation.
FTCORE_API float propGrimeFor(AssetPack world);
// Whether a name is a liquid surface (water, broth, soup, liquid; not oil or stock, which name oiled timber and stockpots): no grime on it, whatever the
// world (dust does not settle on a liquid). No allocation.
FTCORE_API bool propNameLiquid(std::string_view name);

// ---- The prop masters' layers -----------------------------------------------------------------------------------

inline constexpr float kPropWetRoughDefault = 0.35f;                           // the masters' WetRough
inline constexpr std::array<float, 3> kPropDustColor{0.30f, 0.27f, 0.22f};  // the masters' DustColor (linear)

struct PropSurfaceSpec {
    // Break-up.
    float breakupAmp = 0.08f;   // the most the roughness moves either way
    float coarseM = 0.35f;      // the two noise cells, metres
    float fineM = 0.06f;
    float coarseShare = 0.6f;   // the coarse noise's share (the fine one has the rest)
    float breakupGain = 1.6f;   // contrast: brings the noise's typical swing near the full amplitude
    float cavityShare = 0.5f;   // a full cavity (AO 0) pushes the break-up this far toward rough
    float smoothFrom = 0.4f;    // under this authored roughness the break-up fades out (glass keeps its gloss)
    // Grime.
    float grimeRough = 0.2f;    // added roughness at full dust
    float grimeDesat = 0.25f;   // share of the colour's saturation lost at full dust
    float grimeTint = 0.18f;    // share of the way to the dust colour at full dust
    float upFrom = 0.3f;        // vertex normal z: no top dust below upFrom, full from upTo (the wet's up too)
    float upTo = 0.8f;
    float grimePatchM = 0.5f;   // the dust's patch noise cell, metres
    float grimeFloor = 0.4f;    // dust on an up face in a clean patch, as a share of a dusty patch's
    float cavityDust = 0.8f;    // dust in a full cavity, any facing
    // Wet.
    float wetFloor = 0.25f;         // the lowest roughness wetting reaches (WetRough is clamped up to it)
    float soakDarken = 0.6f;        // albedo multiplier at full soak, a dielectric
    float metalDarkenShare = 0.2f;  // a metal darkens by this share of a dielectric's darkening
    float sideShare = 0.35f;        // soak on a vertical face, as a share of an up face's
    float downFrom = 0.2f;          // -(vertex normal z): an underside is dry from downTo
    float downTo = 0.6f;
    float dampShare = 0.45f;        // soak in a drying patch, as a share of a wet patch's (the ground's)
    float patchM = 2.0f;            // the drying-patch noise cell, metres
    float roughCurve = 0.8f;        // the roughness falls by soak^roughCurve
    float grimePatchy = 0.6f;       // at full dust the soak varies by this share on the dust-soak noise
    float grimeSoakM = 0.12f;       // that noise's cell, metres
};

struct PropSurfaceIn {
    std::array<float, 3> albedo{0.5f, 0.5f, 0.5f};
    float rough = 0.6f;
    float metallic = 0.f;
    float ao = 1.f;
    float xM = 0.f;  // world position, metres
    float yM = 0.f;
    float zM = 0.f;
    float normalZ = 1.f;  // the vertex normal's z (unit length)
    float wetness = 0.f;  // the look collection's Wetness
    float weather = 1.f;  // the master's Weather (1 out in the open, 0 under cover)
    float breakup = 0.f;  // RoughBreakup
    float grime = 0.f;    // Grime
    std::array<float, 3> dustColor = kPropDustColor;
    float wetRough = kPropWetRoughDefault;
};

struct PropSurfaceOut {
    std::array<float, 3> albedo{0.f, 0.f, 0.f};
    float rough = 0.f;
    float breakup = 0.f;  // the break-up's signed offset to the roughness
    float dust = 0.f;     // 0..1
    float soak = 0.f;     // 0..1
};

// The 3D value lattice the layers use: streetLatticeValue with z folded into x and y (as the HLSL's L3), 0..1, and
// its smooth (smoothstep-faded) trilinear noise, 0..1. No allocation.
FTCORE_API float propLatticeValue(int x, int y, int z);
FTCORE_API float propNoise(float x, float y, float z);

// The three layers at one point, as the HLSL. No allocation.
FTCORE_API PropSurfaceOut propSurface(const PropSurfaceIn& in, const PropSurfaceSpec& spec = {});

// The Custom node's code. Inputs: Albedo (float3), Rough, Metal, AO, WorldPos (float3, cm), VertexNormal (float3),
// Wetness, Weather, Breakup, Grime, DustColor (float3), WetRough. Returns the albedo; additional output RoughOut
// (float1). Holds streetLatticeHlsl() in a struct (FTPropNoise).
FTCORE_API std::string propSurfaceHlsl(const PropSurfaceSpec& spec = {});

}  // namespace ftcore
