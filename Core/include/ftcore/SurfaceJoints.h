#pragma once

// Joint depth on photographed brick, block, tile and paving: the mortar grooves read as recesses. The maps have no
// height map, but the AO channel is a clean joint mask (about 1 on a brick's face, 0.6-0.75 in a joint), so the height
// is h = smoothstep(JointLo, JointHi, ao): 1 on the face, 0 on the joint's floor, and the joint is JointDepthCm deep.
//
// Two HLSL snippets go into the surface master's Custom node (FTBuildSurfacesCommandlet's kSurfaceCode):
//  - the parallax snippet, right after UvX, UvY and UvZ are made. It works out JtDeep, a large-scale (unevenScaleCm)
//    value noise on the world position that makes some courses sit shallower than others, then on the dominant
//    projection marches the view ray down through the height field (parallax occlusion: linear steps, then one
//    interpolation between the last two) and writes the shifted UV back. It fades out between fadeNearM and fadeFarM
//    and on glancing views, and costs a few instructions when the JointDepth switch is 0 (all work sits inside ifs).
//  - the shade snippet, just before the return. It bends the normal by the height's slope (central differences 2.5
//    texels either side, a mip coarser than the colour, through a narrower smoothstep so only the arris bends, held
//    within maxBendDeg: the 1k JPEG AO is noisy and a fine slope turns it into speckle), and because a wall in
//    shade has no direct light for that normal, the recess is also carried by the cavity terms: a directional cavity
//    (the joint's upper wall, under the brick above, darkest; its lower edge, open to the sky, lighter), a wider AO
//    from taps 3 and 6 texels out (a soft dark core, not a one-texel edge), stronger on deeper joints and glancing
//    views. The joint's floor takes a mortar grey (scaled by the tint's luminance) before that darkening, and dirt
//    broken up by Mottle[0]. The faces get a slight per-unit tone and roughness shift from a hash on a grid of
//    JointUnitsU x JointUnitsV units per tile, with every other course offset by half a unit when JointBond is 1. The
//    photo's bricks are not known, so the grid will not line up with them: each cell blends into the next over
//    unitEdge of a cell so a shift never cuts a brick with a hard line. All of this fades between mip shadeLodFull and
//    shadeLodOff (from the camera distance), not at the parallax's distance.
// Neither snippet uses derivatives (the material also compiles as a ray-tracing hit shader, which has none).
//
// Custom node inputs: ViewVec (float3, world cm, pixel to camera, not normalised), JointDepth (float, 0..1 switch),
// JointDepthCm (float, 0 for a surface without joints), JointLo and JointHi (float, the AO thresholds), JointUnitsU and
// JointUnitsV (float, units per tile, 0 for none), JointBond (float, 1 for a running bond, else 0). Also read: WorldPos,
// Tint and the kSurfaceCode locals named in the snippets.
//
// The C++ functions are the HLSL's twins, one term each, for the tests. Plain C++20; the two Hlsl functions allocate
// (build time only), the rest don't.

#include "ftcore/Api.h"

#include <string>
#include <string_view>

namespace ftcore {

struct SurfaceJointSpec {
    float fadeNearM = 12.f, fadeFarM = 15.f;   // parallax full nearer than fadeNearM, none beyond fadeFarM
    float grazeFull = 0.25f, grazeOff = 0.12f; // dot(N, V): parallax full from grazeFull, none at grazeOff and below
    int maxSteps = 8, minSteps = 3;            // march steps: maxSteps at grazeFull and below, minSteps facing square on
    float maxSlope = 4.f;                      // the march's sideways run per unit of depth is held to this
    float normalStrength = 1.3f;               // scale on the slope got from the height
    float cavityAO = 0.45f;                    // AO multiplier at full cavity
    float cavityAlbedo = 0.5f;                 // albedo multiplier at full cavity
    float cavityRough = 0.2f;                  // roughness added at full cavity
    float dirt = 0.25f;                        // extra darkening gathered in the joints, broken up by a noise
    float cavityBase = 0.7f;                   // cavity on a joint's floor away from its edges
    float upperDark = 0.3f;                    // added on a joint's upper wall, under the unit above
    float skyLift = 0.3f;                      // taken off a joint's lower edge, open to the sky
    float wideAO = 0.5f;                       // cavity from the 3-6 texel ring standing higher than the pixel
    float cavityRefCm = 0.8f;                  // the cavity is full on joints this deep and over, less on shallower
    float viewDark = 0.5f;                     // extra cavity on glancing views (the floor hidden by the faces)
    float mortarR = 0.42f, mortarG = 0.40f, mortarB = 0.37f; // mortar albedo, times the tint's luminance
    float mortarTintFloor = 0.5f;              // the tint luminance the mortar is scaled by never falls under this
    float mortarBlend = 0.5f;                  // how far a joint's floor goes to the mortar albedo on a mid-tone wall
    float mortarBlendDark = 0.8f;              // and on a dark one (contrast in shade, where cavity darkening is lost)
    float mortarMidLum = 0.3f, mortarDarkLum = 0.08f; // wall luminance for mortarBlend and for mortarBlendDark
    float normalNarrow = 0.15f;                // the normal's smoothstep starts and ends this share of lo..hi further in
    float maxBendDeg = 55.f;                   // the bent normal stays within this of the face normal
    float mortarRough = 0.9f;                  // and to this roughness
    float toneVar = 0.06f;                     // per-unit albedo shift, plus or minus
    float roughVar = 0.05f;                    // per-unit roughness shift, plus or minus (relative)
    float unitEdge = 0.15f;                    // a unit's tone blends into the next over this share of a unit
    float uneven = 0.35f;                      // share of the depth a large-scale noise takes away (courses vary)
    float unevenScaleCm = 150.f;               // that noise's cell
    float texels = 1024.f;                     // the ARM map's width in texels
    float radPerPixel = 0.0012f;               // the camera's angle per screen pixel (for the distance-based mip)
    float shadeLodFull = 4.f, shadeLodOff = 6.f; // shading full up to this mip, none from that one
};

// The HLSL snippets (see the top of this file for where each goes).
FTCORE_API std::string surfaceJointParallaxHlsl(const SurfaceJointSpec& spec);
FTCORE_API std::string surfaceJointShadeHlsl(const SurfaceJointSpec& spec);

// Height from the AO channel: smoothstep(lo, hi, ao), 1 on the face, 0 on a joint's floor.
FTCORE_API float surfaceJointHeight(float ao, float lo, float hi);
// The parallax weight: the switch (clamped 0..1) times the distance fade times the glancing fade (HLSL JointPar, without
// its dominant-projection and JointDepthCm > 0 terms).
FTCORE_API float surfaceJointParallaxWeight(const SurfaceJointSpec& spec, float distanceM, float nDotV,
                                            float jointDepthSwitch);
// March steps for a view at nDotV: maxSteps at grazeFull and below, falling linearly to minSteps facing square on.
FTCORE_API int surfaceJointSteps(const SurfaceJointSpec& spec, float nDotV);
// The mip level a tile of tileCm shows at distanceCm: log2 of texels per pixel, never below 0.
FTCORE_API float surfaceJointLod(const SurfaceJointSpec& spec, float distanceCm, float tileCm);
// The shade snippet's fade on that mip: 1 up to shadeLodFull, 0 from shadeLodOff.
FTCORE_API float surfaceJointShadeFade(const SurfaceJointSpec& spec, float lod);
// The depth multiplier JtDeep at a world position (cm): 1 - uneven .. 1, a value noise of cell unevenScaleCm.
FTCORE_API float surfaceJointUneven(const SurfaceJointSpec& spec, float xCm, float yCm, float zCm);
// The cavity JtCav before the shade fade: h0 the pixel's height, above and below the heights 3 texels up and down the
// image, ring the mean of the four ring taps, depthCm the joint's depth times JtDeep, nDotV the view.
FTCORE_API float surfaceJointCavity(const SurfaceJointSpec& spec, float h0, float above, float below, float ring,
                                    float depthCm, float nDotV);
// The per-unit tone JtTone (0..1, 0.5 = no shift) at (u, v) in tile units; unitsU or unitsV 0 gives 0.5.
FTCORE_API float surfaceJointUnitTone(const SurfaceJointSpec& spec, float u, float v, float unitsU, float unitsV,
                                      bool bond);

struct SurfaceJointRgb {
    float r = 0.f, g = 0.f, b = 0.f;
};
// The mortar albedo for a surface tint (its luminance, never under mortarTintFloor, scales the spec's grey).
FTCORE_API SurfaceJointRgb surfaceJointMortar(const SurfaceJointSpec& spec, float tintR, float tintG, float tintB);
// How far a joint's floor goes to the mortar on a wall of this albedo luminance: mortarBlend from mortarMidLum up,
// rising to mortarBlendDark at mortarDarkLum and under.
FTCORE_API float surfaceJointMortarBlend(const SurfaceJointSpec& spec, float wallLum);
// The normal's height from the AO: the smoothstep narrowed by normalNarrow at each end (flat face and floor: no slope).
FTCORE_API float surfaceJointNormalHeight(const SurfaceJointSpec& spec, float ao, float lo, float hi);

struct SurfaceJointVec {
    float x = 0.f, y = 0.f, z = 0.f;
};
// The bent normal: normal less slopeU * tu + slopeV * tv, normalised, then held within maxBendDeg of up.
FTCORE_API SurfaceJointVec surfaceJointBend(const SurfaceJointSpec& spec, SurfaceJointVec normal, SurfaceJointVec up,
                                            SurfaceJointVec tu, SurfaceJointVec tv, float slopeU, float slopeV);

// A height field for the reference march: height 0..1 at (u, v) in tile units.
using SurfaceJointHeightFn = float (*)(float u, float v, const void* user);

struct SurfaceJointShift {
    float du = 0.f, dv = 0.f;
};

// The parallax march's twin. viewU, viewV, viewN: the unit vector from the pixel to the camera in the face's frame
// (along Tu, Tv and the face normal); depthTile: the joint depth in tile units (JointDepthCm / TileCm * JtDeep);
// weight: the parallax weight. Returns the UV shift the HLSL adds to the dominant projection's UV at (u, v).
FTCORE_API SurfaceJointShift surfaceJointMarch(const SurfaceJointSpec& spec, SurfaceJointHeightFn height,
                                               const void* user, float u, float v, float viewU, float viewV,
                                               float viewN, float depthTile, float weight);

// A surface's joints: depth in cm (0 for none), the AO thresholds for the height, units per tile (0 for none) and
// whether every other course is offset by half a unit.
struct SurfaceJointSurface {
    float depthCm;
    float aoLo;
    float aoHi;
    float unitsU;
    float unitsV;
    bool bond;
};

// The joints for a surface id, matched by substring (ids carry a world prefix such as "solhaven_"): soot_brick 1.2 cm
// 4 x 13 running bond, breeze_block 0.8 cm 3 x 5, paving_slab 0.65 cm 4 x 4, tactile_paving 0.5 cm 4 x 4,
// ceramic_tile 0.3 cm 8 x 8, board_concrete 0.2 cm, anything else 0; the AO thresholds are 0.6 and 0.92 for all.
FTCORE_API SurfaceJointSurface surfaceJointFor(std::string_view surfaceId) noexcept;

} // namespace ftcore
