#pragma once

// Rain under cover: where the rain can't reach, there are no streaks or splashes and the ground stays dry.
//
// A top-down orthographic depth capture around the camera (sizeM square, `resolution` texels a side, static geometry
// only) holds the highest occluder's Z at each texel: captureTopZ minus the captured depth. A point is under cover when
// the occluder over it stands more than biasM over it (the ground itself, or a box top the point lies on, reads as
// its own occluder and stays open).
//
// - The capture's centre snaps to whole texels (no shimmer as it moves) and is recaptured only when the camera has moved
//   recaptureM from it in plan, or on the first frame.
// - Frame: engine metres here (the HLSL works in cm). The capture looks straight down (pitch -90, yaw 0, roll 0): the
//   image's right (u) runs along +Y and its down (v) along -X: u = (y - cy) / size + 0.5, v = (cx - x) / size + 0.5.
//   Outside the square a point counts as open (rain falls, the ground soaks), as with no capture at all.
// - Streaks and splashes (particle sprites): killed where covered (alpha 0); a streak pixel is tested at its own Z.
// - Wetness: openness, with a fringe. A covered point within fringeM of open ground (on one of four sides: the rain
//   blows in) takes some wet: the nearest open sample's weight, 1.25 - r / fringeM, clamped 0..1, sampled at fringeM / 3,
//   2 fringeM / 3 and fringeM along +-X and +-Y (a slow dry-to-wet gradient over the fringe).
// - A wall's face reads the cover just out from it: its point moves wallOffsetM along the horizontal part of its normal
//   (a facade under its own roof edge stays wet below an open sky; one under an awning dry).
//
// The C++ functions mirror the HLSL one term each, for tests. Plain C++20; rainCoverHlsl allocates (build time only),
// the rest doesn't.

#include "ftcore/Api.h"

#include <string>

namespace ftcore {

// Material parameter collection names (Data/Look: the cover's own collection, MPC_RainCover).
inline constexpr const char* kRainCoverMpc = "MPC_RainCover";
inline constexpr const char* kRainCoverParamFrame = "CoverFrame";  // vector: centre x, y (cm), size (cm), top Z (cm)
inline constexpr const char* kRainCoverParamOn = "CoverOn";        // scalar: 1 with a capture, 0 none (all open)
// The capture's render target asset, sampled by every material that reads the cover.
inline constexpr const char* kRainCoverTarget = "/Game/Rendering/Rain/RT_RainCover";

struct RainCoverSpec {
    float sizeM = 64.f;          // the capture square (the streak box is 30 m, the splashes 13 m: room for a 5 m drift)
    int resolution = 512;        // texels a side (12.5 cm at 64 m)
    float recaptureM = 5.f;      // camera plan move that triggers a recapture
    float aboveM = 60.f;         // the capture stands this far over the camera
    float depthM = 200.f;        // ... and reads this far down (far plane); deeper counts as open
    float biasM = 0.3f;          // an occluder must stand this far over a point to cover it
    float streakBiasM = 0.05f;   // ... over a streak or splash pixel
    float wallOffsetM = 0.4f;    // a wall face reads the cover this far out along its normal
    float fringeM = 0.75f;       // the wet fringe that blows in under cover at the open sides
};

struct RainCoverFrame {
    float cx = 0.f, cy = 0.f;  // centre (m)
    float sizeM = 0.f;
    float topZ = 0.f;          // the capture's own Z (m): occluder Z = topZ - depth
};

// The texel size (m); 0 for a spec with no size or resolution.
FTCORE_API float rainCoverTexel(const RainCoverSpec& spec);
// The centre for a camera at (x, y): snapped to whole texels.
FTCORE_API void rainCoverSnap(const RainCoverSpec& spec, float camX, float camY, float& cx, float& cy);
// Whether to recapture: none yet, or the camera has moved at least recaptureM in plan from the capture's centre.
FTCORE_API bool rainCoverRecapture(const RainCoverSpec& spec, bool haveCapture, float cx, float cy, float camX, float camY);
// The frame for a camera at (x, y, z) (snapped centre, top aboveM over it).
FTCORE_API RainCoverFrame rainCoverFrameAt(const RainCoverSpec& spec, float camX, float camY, float camZ);
// The capture's UV of a plan point (see the frame above); false outside the square.
FTCORE_API bool rainCoverUV(const RainCoverFrame& frame, float x, float y, float& u, float& v);
// Openness of a point at z under an occluder at occluderZ: 1 open, 0 covered (occluderZ > z + bias).
FTCORE_API float rainCoverOpen(float occluderZ, float z, float bias);
// The fringe weight of an open sample r m away: 1.25 - r / fringe, clamped 0..1 (1 with no fringe at r 0).
FTCORE_API float rainCoverFringeWeight(float r, float fringeM);
// The wet share 0..1 of a point from its own openness and the 12 ring samples' (rings fringe/3, 2/3, 1 in that order,
// each +X, -X, +Y, -Y): its own when open, else the best ring sample's openness times its weight.
FTCORE_API float rainCoverWet(float openHere, const float (&ring)[12], float fringeM);
// Where a pixel reads the cover: (x, y) moved wallOffsetM along the horizontal part of its normal (nx, ny, nz), by the
// horizontal share (a floor not at all, a wall fully).
FTCORE_API void rainCoverReadPoint(const RainCoverSpec& spec, float x, float y, float nx, float ny, float nz, float& rx, float& ry);

// The HLSL: struct FTRainCover with Open(...) for sprites and Wet(...) for surfaces, as one custom node's code. Inputs
// the node declares: CoverTex (Texture2D), CoverTexSampler, CoverFrame (float4, cm), CoverOn (float), WorldPos (cm),
// Normal (world, unit). Kind "sprite" returns the openness, "surface" the wet share.
FTCORE_API std::string rainCoverHlsl(const RainCoverSpec& spec, bool sprite);

}  // namespace ftcore
