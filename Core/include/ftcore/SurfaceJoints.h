#pragma once

// Joint depth on photographed brick and block walls: the mortar grooves read as recesses. The wall's maps have no height
// map, but the AO channel is a clean joint mask (about 1 on a brick's face, 0.6-0.75 in a joint), so the height is
// h = smoothstep(JointLo, JointHi, ao): 1 on the face, 0 on the joint's floor, and the joint is JointDepthCm deep.
//
// Two HLSL snippets go into the surface master's Custom node (FTBuildSurfacesCommandlet's kSurfaceCode):
//  - the parallax snippet, right after UvX, UvY and UvZ are made: on the dominant projection it marches the view ray
//    down through the height field (parallax occlusion: linear steps, then one interpolation between the last two)
//    and writes the shifted UV back. It fades out between fadeNearM and fadeFarM and on glancing views, and costs a
//    few instructions when the JointDepth switch is 0 (the march sits inside a dynamic if).
//  - the shade snippet, just before the return: it bends the normal by the height's slope and darkens, dirties and
//    roughens the joints. It does not fade at fadeFarM, only once a joint is well under a pixel (mip lod 3 to 5).
// Neither uses derivatives (the material also compiles as a ray-tracing hit shader, which has none): the mip level
// comes from the camera distance instead.
//
// New Custom node inputs: ViewVec (float3, world cm, pixel to camera, not normalised), JointDepth (float, 0..1 switch),
// JointDepthCm (float, 0 for a surface without joints), JointLo and JointHi (float, the AO thresholds).
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
    float cavityAO = 0.45f;                    // AO multiplier on a joint's floor
    float cavityAlbedo = 0.5f;                // albedo multiplier on a joint's floor
    float cavityRough = 0.2f;                  // roughness added on a joint's floor
    float dirt = 0.25f;                        // extra darkening gathered in the joints, broken up by a noise
    float uneven = 0.35f;                      // share of the depth a large-scale noise takes away (courses vary)
    float texels = 1024.f;                     // the ARM map's width in texels
    float radPerPixel = 0.0012f;               // the camera's angle per screen pixel (for the distance-based mip)
    float shadeLodFull = 3.f, shadeLodOff = 5.f; // normal and cavity full up to this mip, none from that one
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

// A height field for the reference march: height 0..1 at (u, v) in tile units.
using SurfaceJointHeightFn = float (*)(float u, float v, const void* user);

struct SurfaceJointShift {
    float du = 0.f, dv = 0.f;
};

// The parallax march's twin. viewU, viewV, viewN: the unit vector from the pixel to the camera in the face's frame
// (along Tu, Tv and the face normal); depthTile: the joint depth in tile units (JointDepthCm / TileCm); weight: the
// parallax weight. Returns the UV shift the HLSL adds to the dominant projection's UV at (u, v).
FTCORE_API SurfaceJointShift surfaceJointMarch(const SurfaceJointSpec& spec, SurfaceJointHeightFn height,
                                               const void* user, float u, float v, float viewU, float viewV,
                                               float viewN, float depthTile, float weight);

// A surface's joints: depth in cm (0 for none) and the AO thresholds for the height.
struct SurfaceJointSurface {
    float depthCm;
    float aoLo;
    float aoHi;
};

// The joints for a surface id, matched by substring (ids carry a world prefix such as "solhaven_"): soot_brick 1.0 cm,
// breeze_block 0.8, paving_slab 0.65, tactile_paving 0.5, ceramic_tile 0.3, board_concrete 0.2, anything else 0; the
// AO thresholds are 0.6 and 0.92 for all.
FTCORE_API SurfaceJointSurface surfaceJointFor(std::string_view surfaceId) noexcept;

} // namespace ftcore
