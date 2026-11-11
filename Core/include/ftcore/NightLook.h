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
    float skyFill = 0.60f;         // the sky light at full night, as a share of its day intensity (net of the moon)
    float fogKeep = 0.08f;         // the height fog's own inscattering colour kept at full night
    float exposureEV100 = -1.2f;   // the night exposure's centre, from NightAmount kExposureLockAt on ...
    float exposureRangeEV = 1.0f;  // ... and its half-width: auto exposure moves within centre +- this (0 = locked)
    float whiteKelvin = 5600.f;    // the camera's white balance at full night (blended from the day's by NightAmount)
    float dayWetShare = 0.58f;     // the look's ground wetness by full day, as a share of its own (full at night):
                                   // Solhaven's 0.6 is about 0.35 by day
    float lampCandela = 24.f;      // warm-white street lamps
    float lampReachM = 30.f;       // attenuation radius: the inverse-square window is about 0.9 at 15 m, not 0.5
    float lampKelvin = 2400.f;     // orange street glow (2200-2700 K reads as the avenues' warm line)
    float lampOuterDeg = 80.f;     // the pooled lamps' cone half-angles (the engine's widest is 80) and lens radius
    float lampInnerDeg = 55.f;     // (cm; larger stretches the lamp's highlight on wet ground into a streak)
    float lampSourceCm = 20.f;
    float shopCandela = 15.f;      // LED shop spill, cooler than the lamps beside them
    float shopReachM = 9.f;
    float shopKelvin = 4000.f;
    std::array<float, 3> glow{0.030f, 0.019f, 0.010f};   // fog inscattering added at full night (linear luminance)
    // The moon is a pale blue. The sky light captures the atmosphere it lights, which is a far deeper blue than the
    // moon itself, so the fill's tint leans the other way (red over blue): net, an unlit facade reads a grey-blue, dark
    // but there, not a cobalt black.
    std::array<float, 3> moonTint{0.70f, 0.82f, 1.f};
    std::array<float, 3> skyTint{1.f, 0.72f, 0.46f};
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

// The dusk: a low sun's saturated sky (a magenta-purple dome, a warm pink sun, a sky fill held low so the city stands
// as a silhouette against it, and a lens flare off the sun). duskAmount is a bell over the sun's elevation: 0 above
// startDeg, 1 at peakDeg, 0 again below endDeg; a morning sun takes morningShare of it. duskFrame turns an amount into
// the blends a world applies to the lights it had by day, before the night's (nightFrame) go over them, so as the night
// rises it wins. The clock and a still at a dusk hour read the same two functions. No allocation.
struct DuskLook {
    float startDeg = 14.f;    // the sun's elevation (degrees) dusk starts from ...
    float peakDeg = 2.f;      // ... is fullest at ...
    float endDeg = -7.f;      // ... and is gone by
    float morningShare = 0.4f;
    std::array<float, 3> sunTint{1.f, 0.56f, 0.44f};     // the sun's colour times this at full dusk (pink-orange)
    std::array<float, 3> skyGlow{1.25f, 0.70f, 1.18f};   // the atmosphere's sky luminance factor at full dusk (magenta-purple)
    std::array<float, 3> fillTint{0.95f, 0.66f, 0.92f};  // the sky light's colour times this
    float fillScale = 0.55f;  // the sky light's intensity times this: faces in shade go dark, the skyline a silhouette
    std::array<float, 3> fogTint{1.f, 0.62f, 0.86f};     // the height fog's inscattering times this
    float flare = 1.6f;       // lens flare intensity at full dusk (blended from the volume's own; the engine default is 1)
    float flareThreshold = 2.5f; // the flare threshold at full dusk (the engine default 8 hides a low sun's flare)
    float bloomAdd = 0.35f;   // added to the volume's bloom at full dusk
};

struct DuskFrame {
    float amount = 0.f;
    std::array<float, 3> sunMul{1.f, 1.f, 1.f};    // multiply the light's day colour by these
    std::array<float, 3> skyGlow{1.f, 1.f, 1.f};   // the atmosphere's sky luminance factor (times its own)
    std::array<float, 3> fillMul{1.f, 1.f, 1.f};
    float fillScale = 1.f;
    std::array<float, 3> fogMul{1.f, 1.f, 1.f};
    float flareMix = 0.f;      // blend the volume's lens flare toward flare / flareThreshold by this
    float flare = 0.f;
    float flareThreshold = 8.f;
    float bloomAdd = 0.f;
};

// 0..1 for a sun this many degrees up, in the evening or (scaled by morningShare) the morning. No allocation.
FTCORE_API float duskAmount(const DuskLook& look, float sunElevationDeg, bool evening);
// The blends at an amount (clamped 0..1). No allocation.
FTCORE_API DuskFrame duskFrame(const DuskLook& look, float amount);
// The hour of the evening the sun stands at peakDeg, for a still or a -TimeOfDay at full dusk. No allocation.
FTCORE_API double duskPeakHour(const DuskLook& look, float latitudeDeg);

} // namespace ftcore
