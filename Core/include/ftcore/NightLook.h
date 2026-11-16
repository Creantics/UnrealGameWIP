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
#include <cstdint>

namespace ftcore {

struct NightLook {
    float moonShare = 0.07f;       // the directional light at full night, as a share of its day intensity
    float skyFill = 0.14f;         // the sky light in the deep night, as a share of its day intensity (net of the moon):
                                   // the night's floor, so between the lamps reads dark but with form (11-15 19:00
                                   // ruling: the wide street's median at 18-24/255 from 20:30 on; block 1 read 20.4 at
                                   // a fill of 0.139)
    float skyLuminance = 0.03f;    // the sky dome's own brightness in the deep night (SkyAtmosphere SkyLuminanceFactor):
                                   // near black, so the lamps and shops carry the night, not a dimmed day sky. A
                                   // real-time sky capture sees the darker dome too, so its lift takes this back out
                                   // (skyScaleRealTime) and skyFill keeps meaning the fill as a share of the day's.
    // The dusk into the deep night. NightAmount is full by the sun at -6 degrees (about 18:30), so the fill and the dome
    // follow a second curve on the sun's elevation (nightDeep): the fill stands at twilightFill and the dome at the day's
    // as twilight ends, and both fall in ratio to skyFill and skyLuminance from deepHighDeg to deepLowDeg (eased), so the
    // evening darkens continuously with no step. The defaults put 19:00 within a few percent of the twilight's values
    // and hold the deep night's from about 20:25.
    float twilightFill = 1.30f;    // the sky light's fill as twilight ends (the night's old value)
    float deepHighDeg = -12.f;     // the deep night starts with the sun this far down ...
    float deepLowDeg = -33.f;      // ... and is full by here (the sun at about 20:25), so the floor is flat from 20:30
    // The evening's switch-on, one curve on the sun's elevation (11-15 19:00 ruling): the street lamps, the shop and
    // window lights come on from lampsOnHighDeg to lampsOnLowDeg (each lamp at its own hashed point of that fraction,
    // nightLampOn, so they don't pop together), and only then does the exposure leave the day's range for the night's,
    // from exposureHighDeg to exposureLowDeg (eased). exposureHighDeg sits where the lamps are past half on.
    float lampsOnHighDeg = 1.f;
    float lampsOnLowDeg = -4.f;
    float exposureHighDeg = -2.f;
    float exposureLowDeg = -7.f;
    // The moon (the directional light opposite the sun) grows with its own elevation, eased from moonRiseDeg to
    // moonFullDeg, from moonRiseShare of moonShare (a grazing moon lights nothing direct; the share keeps the sky
    // capture it lights from vanishing) up to all of it.
    float moonRiseDeg = 0.f;
    float moonFullDeg = 15.f;
    float moonRiseShare = 0.1f;
    // The night's local exposure shadow contrast and film toe, reached with the exposure blend. Below 0 keeps the world
    // look's own (the default): set only by ruling (-NightShadowContrast=, -NightToe=).
    float shadowContrast = -1.f;
    float filmToe = -1.f;
    float fogKeep = 0.08f;         // the height fog's own inscattering colour kept at full night
    float exposureEV100 = -1.2f;   // the night exposure's centre, from NightAmount kExposureLockAt on ...
    float exposureRangeEV = 1.0f;  // ... and its half-width: auto exposure moves within centre +- this (0 = locked)
    float whiteKelvin = 5600.f;    // the camera's white balance at full night (blended from the day's by NightAmount)
    float dayWetShare = 0.f;       // the look's ground wetness by full day, as a share of its own (full at night):
                                   // the streets are dry by day by default. Day wetness comes only from the weather
                                   // state when the weather system drives it (rain, then drying), never from this.
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
    // A fill on characters alone (lighting channel 1; the street keeps its dark): cool, from the camera's side and a
    // little above, charFillStops under the lamp key (a lamp's candela at charFillRefM, a face under a lamp head), so
    // a face and a silhouette read between lamps. Off unless asked for (-NightCharFill, FTCharacterFillSubsystem.h).
    float charFillStops = 2.f;
    float charFillRefM = 4.f;
    float charFillKelvin = 8000.f;
    float charFillPitchDeg = -20.f;  // below the camera's own pitch (negative: from above)
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
    float deep = 0.f;                        // the deep night's amount the fill and dome stand at (nightDeep)
    float sunScale = 1.f;
    float skyScale = 1.f;
    float skyScaleRealTime = 1.f;
    float skyLuminance = 1.f;                // the sky atmosphere's luminance factor (1 by day)
    float tintMix = 0.f;
    float fogKeep = 1.f;                     // the fog's day colour times this ...
    std::array<float, 3> fogGlow{0.f, 0.f, 0.f}; // ... plus this
    bool lockExposure = false;               // the night's exposure range applies (exposureBlend above 0)
    float exposureBlend = 0.f;               // the exposure range blended from the day's toward the night's by this
    float lamps = 0.f;                       // the lamps', shops' and windows' switch-on fraction (nightLampFraction)
    float moonScale = 1.f;                   // the moon's share of moonShare at its elevation (nightMoonScale)
    float exposureEV100 = 0.f;               // its centre ...
    float exposureMinEV100 = 0.f;            // ... the darkest the scene is metered (the camera opens up no further)
    float exposureMaxEV100 = 0.f;            // ... the brightest (the camera closes down no further)
    float whiteMix = 0.f;                    // blend the camera's white balance toward whiteKelvin by this
    float whiteKelvin = 6500.f;
    float wetShare = 1.f;                    // the look's wetness times this (dayWetShare by day .. 1 at full night)
};

// NightAmount and the deep amount are clamped to 0..1. Shares at or under 0 are taken as 1e-4 so the real-time lift
// stays finite. The two-argument form takes the deep amount as the night's, so nightFrame(look, 1) is the full deep
// night (a night still). The clock passes nightDeep at the sun's elevation. No allocation.
FTCORE_API NightFrame nightFrame(const NightLook& look, float night);
FTCORE_API NightFrame nightFrame(const NightLook& look, float night, float deep);
// The clock's frame: as the three-argument form, with the switch-on, the exposure blend and the moon read from the
// sun's elevation. With the sun up the directional light is the sun, dimmed by the night; with it down it is the moon
// at moonShare times nightMoonScale, and the real-time sky lift divides by that, so the fill stays a share of the day's
// across the horizon with no step. No allocation.
FTCORE_API NightFrame nightFrameAt(const NightLook& look, float night, float sunElevationDeg);
// 0 with the sun above deepHighDeg, 1 below deepLowDeg, eased between. No allocation.
FTCORE_API float nightDeep(const NightLook& look, float sunElevationDeg);
// The lamps', shops' and windows' switch-on fraction: 0 above lampsOnHighDeg, 1 from lampsOnLowDeg down, eased. No
// allocation.
FTCORE_API float nightLampFraction(const NightLook& look, float sunElevationDeg);
// Whether the lamp keyed `key` (a stable id: its index or a hash of its place) is on at a fraction: each lamp has its
// own point in 0.05..0.95 of the fraction, so the street comes on lamp by lamp. All off at 0, all on at 1. No allocation.
FTCORE_API bool nightLampOn(float fraction, std::uint32_t key);
// The exposure's blend from the day's range to the night's: 0 above exposureHighDeg, 1 from exposureLowDeg down. No
// allocation.
FTCORE_API float nightExposureBlend(const NightLook& look, float sunElevationDeg);
// The moon's share of moonShare with the sun at this elevation (the moon stands opposite it): moonRiseShare at the
// horizon, 1 from moonFullDeg up, eased; 1 with the sun up (the light is the sun then). No allocation.
FTCORE_API float nightMoonScale(const NightLook& look, float sunElevationDeg);

// A model of the wide street's metered median (linear, 1 = white) through the evening, for tests and for reading a
// strip against: the sky fill and the lamps light the street, the camera adapts within the day's range (a key it holds
// while it can open up enough) and is clamped by the night's as the exposure blend goes over. Weights fitted to the
// 11-15 strip (block 1: fill 1.24 / 0.55 / 0.14 / 0.033 / 0.012 read 49 / 37 / 20 / 11 / 10 of 255). The moon's direct
// light is left out on purpose: it adds direction and shadow, not the floor (nightMoonKey is its share alone).
struct NightKeyModel {
    float fillWeight = 0.025f;   // the median per unit of sky fill share at the night's exposure
    float lampWeight = 0.0005f;  // the lamps' share of the median between their pools, all on
    float moonWeight = 0.052f;   // the moon's direct light per unit of moonShare x sin(its elevation), where it reaches
    float dayKey = 0.0266f;      // what the camera holds while the day's range can open it up (19:00's 49/255)
    float dayOpenEV = 8.f;       // how far past the night's range the day's range opens the camera
};
// The model's median at a NightAmount and sun elevation. No allocation.
FTCORE_API float nightSceneKey(const NightLook& look, const NightKeyModel& model, float night, float sunElevationDeg);
// The moon's direct share of the median at a sun elevation (0 with the sun up). No allocation.
FTCORE_API float nightMoonKey(const NightLook& look, const NightKeyModel& model, float sunElevationDeg);

// The characters-only fill's illuminance (lux) at a NightAmount: 0 by day, the lamp key's at charFillRefM less
// charFillStops at full night, in between by the night. No allocation.
FTCORE_API float nightCharacterFillLux(const NightLook& look, float night);

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
    // The look table's dusk row (Data/Look/looks.json "dusk") sets every value above and these:
    float stillDeg = 2.f;         // the sun's elevation a dusk still stands at (duskStillHour)
    float exposureBias = 0.f;     // EV added to the camera's exposure bias at full dusk
    float lightsOnDeg = -90.f;    // shop, window and street lights start to come on as the sun sinks through this ...
    float lightsFullDeg = -90.f;  // ... and are all on by this (on <= full: they follow the night alone)
    float dayEndDeg = -6.f;       // daylight inside a room: gone with the sun this low ...
    float dayFullDeg = 12.f;      // ... and full with the sun this high
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
// The hour of the evening the sun stands at stillDeg: where a dusk still is taken. No allocation.
FTCORE_API double duskStillHour(const DuskLook& look, float latitudeDeg);
// The share of the shop, window and street lights on with the sun this many degrees up: 0 above lightsOnDeg, 1 from
// lightsFullDeg down, eased between (0 everywhere when on <= full). A world takes the larger of this and the night's
// amount. No allocation.
FTCORE_API float duskLights(const DuskLook& look, float sunElevationDeg);
// The daylight reaching a room behind glass, as a share of a clear day's: 0 from dayEndDeg down, 1 from dayFullDeg up.
// No allocation.
FTCORE_API float duskDaylight(const DuskLook& look, float sunElevationDeg);

} // namespace ftcore
