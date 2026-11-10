#pragma once

// The city night: one spec that both the running world clock and a night still read, so the two give the same look.
//
// NightLook holds the full-night values: the moon's share of the day sun, the sky light's fill, the height fog's own
// colour kept and the warm glow the city throws into it (absolute luminance, so the haze near the horizon reads warm
// over the streets' light), the night's exposure range and white balance, and the street lamps' and shop spill lights'
// candela, reach and colour. The exposure is a range, not one EV: in the dark between lamps the camera opens up to
// exposureEV100 - exposureRangeEV so the moonlit fill keeps its detail, and under a lamp it closes down to
// exposureEV100 + exposureRangeEV so near surfaces don't burn white. The white balance moves toward whiteKelvin, so the
// warm lamps read warm and the LED shops and the moon read cool instead of the whole frame going one orange. nightFrame() blends it in by NightAmount (0 day, 1 full night) into the scales a world applies to its
// lights as they stood by day. nightLampScales() caps the light where lamps or shop lights crowd: each one's intensity
// share falls with the others within capRadiusM, down to capFloor, so a dense lane doesn't burn out while a lone lamp
// keeps its pool.
//
// Plain C++20. nightFrame and nightLampScale don't allocate; nightLampScales allocates (load time only).

#include "ftcore/Api.h"

#include <array>
#include <cstddef>

namespace ftcore {

struct NightLook {
    float moonShare = 0.10f;       // the directional light at full night, as a share of its day intensity
    float skyFill = 0.65f;         // the sky light at full night, as a share of its day intensity (net of the moon)
    float fogKeep = 0.08f;         // the height fog's own inscattering colour kept at full night
    float exposureEV100 = -1.5f;   // the night exposure's centre, from NightAmount kExposureLockAt on ...
    float exposureRangeEV = 1.25f; // ... and its half-width: auto exposure moves within centre +- this (0 = locked)
    float whiteKelvin = 5000.f;    // the camera's white balance at full night (blended from the day's by NightAmount)
    float dayWetShare = 0.58f;     // the look's ground wetness by full day, as a share of its own (full at night):
                                   // Solhaven's 0.6 is about 0.35 by day
    float lampCandela = 18.f;      // warm-white street lamps
    float lampReachM = 30.f;       // attenuation radius: the inverse-square window is about 0.9 at 15 m, not 0.5
    float lampKelvin = 2700.f;
    float lampOuterDeg = 80.f;     // the pooled lamps' cone half-angles (the engine's widest is 80) and lens radius
    float lampInnerDeg = 55.f;     // (cm; larger stretches the lamp's highlight on wet ground into a streak)
    float lampSourceCm = 20.f;
    float shopCandela = 12.f;      // LED shop spill, cooler than the lamps beside them
    float shopReachM = 9.f;
    float shopKelvin = 4000.f;
    std::array<float, 3> glow{0.026f, 0.019f, 0.013f};   // fog inscattering added at full night (linear luminance)
    std::array<float, 3> moonTint{0.55f, 0.68f, 1.f};
    std::array<float, 3> skyTint{0.60f, 0.72f, 1.f};
    float capRadiusM = 15.f;       // lights within this of each other share their light
    float capShare = 0.2f;         // each neighbour within capRadiusM takes this much off (1 / (1 + capShare n))
    float capFloor = 0.4f;         // no light falls under this share of its candela
};

inline constexpr float kExposureLockAt = 0.5f;

// What a world applies at a NightAmount: multiply each light's day intensity by these and blend its colour toward the
// tint by tintMix. A real-time sky capture already dims with the sun, so it takes skyScaleRealTime, which lifts it by
// the sun's share back to skyScale's level.
struct NightFrame {
    float night = 0.f;
    float sunScale = 1.f;
    float skyScale = 1.f;
    float skyScaleRealTime = 1.f;
    float tintMix = 0.f;
    float fogKeep = 1.f;                     // the fog's day colour times this ...
    std::array<float, 3> fogGlow{0.f, 0.f, 0.f}; // ... plus this
    bool lockExposure = false;               // the night's exposure range applies (from kExposureLockAt on)
    float exposureEV100 = 0.f;               // its centre ...
    float exposureMinEV100 = 0.f;            // ... the darkest the scene is metered (the camera opens up no further)
    float exposureMaxEV100 = 0.f;            // ... the brightest (the camera closes down no further)
    float whiteMix = 0.f;                    // blend the camera's white balance toward whiteKelvin by this
    float whiteKelvin = 6500.f;
    float wetShare = 1.f;                    // the look's wetness times this (dayWetShare by day .. 1 at full night)
};

// NightAmount is clamped to 0..1. Shares at or under 0 are taken as 1e-4 so the real-time lift stays finite.
FTCORE_API NightFrame nightFrame(const NightLook& look, float night);

// The intensity share of a light with `neighbours` others within capRadiusM.
FTCORE_API float nightLampScale(const NightLook& look, std::size_t neighbours);

// For count lights at xy pairs (any unit; unitsPerMetre converts capRadiusM), each one's share into out (count floats).
// Neighbours are counted up to kNightCapCount. Allocates (a LightPicker and its scratch), so load time only.
inline constexpr std::size_t kNightCapCount = 32;
FTCORE_API void nightLampScales(const NightLook& look, const float* xy, std::size_t count, float unitsPerMetre, float* out);

} // namespace ftcore
