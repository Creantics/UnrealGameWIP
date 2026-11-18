#pragma once

// Falling rain around the camera: what the rain's intensity asks of the two Niagara systems (Data/Vfx/rain.json and
// rain_splash.json) and where they stand.
//
// - rain.json's "streaks" fall from a box 30 m wide that rides the camera. Its spawn rate is the user parameter
//   kRainUserRate, its start colour kRainUserColor (the alpha carries the streak opacity, rainStreakAlpha).
// - rain_splash.json lays ripple rings (kRainUserRingRate) and small jumping drops (kRainUserDropRate) on the floor
//   under the camera, out to 13 m.
// - The rates scale linearly with the intensity, 0 at 0; at 1 they are the json's own rates (the *AtFull values).
// - The streaks' box sits aboveCamM over the camera, but never more than columnM over the floor, so the streaks reach
//   the ground under a high camera without a sky of them overhead.
// - The splashes show only when the floor is found and the camera is near it (within splashMaxDropM above it).
//
// Plain C++20, no allocation, no state.

#include "ftcore/Api.h"
#include "ftcore/Weather.h"

namespace ftcore {

// User parameter names (without "User."), shared with Data/Vfx/rain.json and rain_splash.json.
inline constexpr const char* kRainUserRate = "Rate";
inline constexpr const char* kRainUserRingRate = "RingRate";
inline constexpr const char* kRainUserDropRate = "DropRate";
inline constexpr const char* kRainUserColor = "Color";

struct RainFallSpec {
    float streaksPerSAtFull = 12000.f;  // rain.json streaks' rate
    float ringsPerSAtFull = 1200.f;     // rain_splash.json rings' rate
    float dropsPerSAtFull = 500.f;      // rain_splash.json drops' rate
    float aboveCamM = 7.f;              // the streak box over the camera (m)
    float columnM = 12.f;               // ... but at most this far over the floor (m); the streaks fall about 12 m
    float splashMaxDropM = 30.f;        // splashes only while the camera is at most this far over the floor (m)
    float splashBelowM = 0.5f;          // ... and at most this far under it (a camera dipping below a kerb)
};

struct RainFallFrame {
    float streakRate = 0.f;   // per second
    float ringRate = 0.f;
    float dropRate = 0.f;
    float streakAlpha = 0.f;  // 0..1, the start colour's alpha
};

// The rates and opacity at this intensity (0..1; outside or not finite counts as 0 or 1, NaN as 0).
FTCORE_API RainFallFrame rainFallFrame(const RainSpec& rain, const RainFallSpec& spec, float intensity);
// Height (m, any up-axis frame) of the streak box: camZ + aboveCamM, capped at floorZ + columnM when a floor was found.
FTCORE_API float rainColumnTop(const RainFallSpec& spec, float camZ, bool hasFloor, float floorZ);
// Whether the splashes show: a floor was found, the camera at most splashMaxDropM above it and splashBelowM below it.
FTCORE_API bool rainSplashOn(const RainFallSpec& spec, float camZ, bool hasFloor, float floorZ);

// What shows this frame. With the camera under water, nothing: the streaks and splashes are drawn in world space and
// would otherwise fall through the surface into the underwater view. Above it, the streaks, and the splashes per
// rainSplashOn.
struct RainShown {
    bool streaks = false;
    bool splash = false;
};
FTCORE_API RainShown rainShown(const RainFallSpec& spec, float camZ, bool hasFloor, float floorZ, bool cameraUnder);

}  // namespace ftcore
