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

// The deep night's floor, measured (11-16 rulings: the METERED wide-street key the same at 20:30, 22:00 and 24:00, held
// by light, not tone). The model: with the moon at elevation m (the sun's depth) and the sky light's fill f, the wide
// street's centre luma median (0-255, the shipped tone) is
//     key(m, f) = keyAtRef(m) + slope(m) x (f - refFill(m)),
// each of keyAtRef, slope and refFill read at the knots and linear between them (flat outside). It is linear in f in
// display units near the floor (the 21:00 bracket 0.015 / 0.021 / 0.03 read 18.2 / 29.6 / 43.4), and its slope depends
// on the moon (the real-time lift divides by it), so the table holds for one moon share only: moonShare. The fill that
// holds the floor is then fill(m) = refFill + (floorKey - keyAtRef) / slope, clamped to fillMin .. twilightFill.
struct NightFloorKnot {
    float moonDeg = 0.f;    // the moon's elevation (the sun's depth) this row was shot at
    float refFill = 0.f;    // the fill the key was read at
    float keyAtRef = 0.f;   // the wide street's centre median (0-255) at that fill
    float slope = 0.f;      // its change per unit of fill there
};
struct NightFloorFit {
    float moonShare = 0.07f;   // the moon share every row was shot at; any other share falls back to skyFill
    float floorKey = 21.f;     // the median the fill holds (0-255); at or under 0, off: the fill is skyFill throughout
    float fillMin = 0.004f;    // the fill never goes under this (the capture keeps a sky to light)
    // Measured at moon 0.07: night_1116a (the 21:00 bracket), night_1116b (block 4, fill 0.015) and night_1116c (block
    // 4b). Each row is the secant between the two measured fills that bracket the floor (the response is concave, so a
    // local secant beats one slope): 20:30 0.020/0.026 read 20.8/30.6; 21:00 0.015/0.021 read 18.2/29.6; 22:00
    // 0.012/0.015 read 23.6/29.2; 24:00 0.005/0.008 read 18.8/24.9.
    std::array<NightFloorKnot, 4> knots{{
        {34.5f, 0.020f, 20.8f, 1633.f},   // 20:30
        {41.0f, 0.015f, 18.2f, 1900.f},   // 21:00
        {53.4f, 0.012f, 23.6f, 1867.f},   // 22:00
        {68.0f, 0.005f, 18.8f, 2033.f},   // 24:00
    }};
};

struct NightLook {
    float moonShare = 0.07f;       // the directional light at full night, as a share of its day intensity
    float skyFill = 0.015f;        // the sky light in the deep night, as a share of its day intensity (net of the moon),
                                   // for a still and whenever the floor fit is off: the night's floor, so between the
                                   // lamps reads dark but with form (11-15 19:00 ruling: the wide street's median at
                                   // 18-24/255 from 20:30 on; night_1116a read 18.2 at 21:00 with this)
    NightFloorFit floorFit;          // the clock's deep fill per moon elevation (nightDeepFill)
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
    // The one directional light through the horizon (11-15 20:12 review): the sun's part, dimmed by the night, halves
    // every sunSetHalfDeg once the sun is down, and the moon's part comes in with NightAmount, so the light's intensity
    // has no step at the horizon (its direction still turns to the moon's there, both grazing). Under 0.05 is taken as
    // 0.05. A look knob: smaller ends the sun's light sooner after sunset (-NightSunSetHalf=).
    float sunSetHalfDeg = 2.f;
    // The real-time sky lift never divides by a light under this share (11-16 ruling: block 4's moonless still read 209
    // because the lift divided by a 1e-4 moon). Under it, as on a moonless or overcast night, the capture is lifted no
    // further, so the street falls darker instead of blowing out. The ruled moons (0.03 and up) stay exact.
    float liftFloorShare = 0.01f;
    // The dusk table (11-16 dusk ruling, fitted on blocks 1116e/f). Between duskLiftTopDeg and exposureLowDeg the
    // real-time sky lift (skyScaleRealTime / skyScale) is the table's, in ratio between its knots, from the whole lift
    // at duskLiftTopDeg and back to it at exposureLowDeg: so every frame from exposureLowDeg down, and from
    // duskLiftTopDeg up, is exactly as before. False keeps the whole lift everywhere (-NightLiftAll).
    // -NightDuskLift=a,b,c,d sets the knots' lifts. Fitted (strip-centre key, target 75/68/60/48 at 18:00/05/10/20):
    // 18:00 between the measured r 2.14 (36) and 4.27 (93); 18:05 between r 1.30 (47) and 4.35 (93); 18:10 and 18:20 at
    // the r their EV-pinned arms were shot at (1.68, 17.8), the floor below doing the rest.
    bool duskLiftTable = true;
    float duskLiftTopDeg = 3.5f;
    // The knots sit at the clock's own sun at 18:00, 18:05, 18:10 and 18:20 (WorldClockSpec defaults).
    std::array<float, 4> duskLiftDeg{0.f, -1.159f, -2.318f, -4.635f};
    std::array<float, 4> duskLift{3.18f, 2.10f, 1.365f, 13.8f};
    // The exposure's floor (min EV100) once the blend is above 0: from the day's at exposureHighDeg through these knots
    // (straight between them) to the night's at exposureLowDeg, so the range binds from the blend's first step and the
    // camera can't open up past the knot's EV (the meter wants to open further here: the street has no key light
    // between sunset and the moon). The max blends from the day's to the night's by the blend.
    // -NightDuskMinEV=a,b,c,d sets the knots' EVs. 18:10 (-5.43) and 18:20 (-3.40) from their pinned arms (1116f: 65 at
    // -5.5, 21 at -4.5; 55.5 at -3.5, 12.2 at -2.5); 18:15 and 18:25 straight between, unmeasured.
    // At the clock's sun at 18:10, 18:15, 18:20 and 18:25.
    std::array<float, 4> duskMinDeg{-2.318f, -3.477f, -4.635f, -5.793f};
    std::array<float, 4> duskMinEV{-5.43f, -4.42f, -3.40f, -2.80f};
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
    // The hero corner's lit rooms are old timber shops under paper lamps, not LED: their pool shop slots burn at this
    // (step 2, 2026-11-16: about 2700 K). 0 keeps shopKelvin.
    float heroShopKelvin = 2700.f;
    // The face ruling (2026-11-16): the corner's own lamp slots (strings, pairs, entrances) stand for the lanterns, but
    // their light is warm white, not the paper's deep amber, so skin keeps its hue variation under them. 0 keeps
    // lampKelvin. Unmeasured: E1 brackets it on the face probes.
    float heroLampKelvin = 2700.f;
    // Its lit bays' fronts as a wide soft key (a rect light the size of each front, facing the lane, at heroShopKelvin),
    // and a cool floor from the lane's open (canal) side: a rect light the lane's length facing the bays, tinted
    // heroSkyTint (far bluer than skin, so a face's shadow side keeps R/B under 0.9), with no specular (no rectangle in
    // the puddles). Both at the night's lamp share; lumens unmeasured, bracketed in E1.
    float heroFrontLumens = 40.f;
    float heroSkyLumens = 25.f;
    std::array<float, 3> heroSkyTint{0.45f, 0.62f, 1.f};
    float heroSkySpecular = 0.f;
    // The fronts' key has no visible source, so on wet setts its full mirror image (a hard white rectangle, E2 11-16)
    // gives it away; a low specular keeps a soft sheen and the lanterns make the reflections.
    float heroFrontSpecular = 0.15f;
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
    // Near a lit shop bay the shop light shapes the figure: the fill is gone within charFillBayNearM of the nearest lit
    // bay's front and whole from charFillBayFarM out (nightCharacterFillShare).
    float charFillBayNearM = 3.f;
    float charFillBayFarM = 7.f;
    // Its shape: charFillKeyShare of the fill is a soft key charFillYawDeg off the view on the nearest light's side
    // (nightCharacterFillYaw), so the figure keeps a light and a shadow side; the rest is a weak ambient along the view.
    float charFillYawDeg = 35.f;
    float charFillKeyShare = 0.7f;
    // The key follows a new nearest light only when it is charFillSwitchNearer (a share) nearer than the one followed
    // (nightCharacterFillSwitchLight), and its yaw turns at charFillYawRateDeg per second at most
    // (nightCharacterFillEaseYaw), so it never flips sides in one frame as the lead walks or the camera turns.
    float charFillSwitchNearer = 0.2f;
    float charFillYawRateDeg = 90.f;
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
// sun's elevation. The directional light is the sun's part, lerp(1, moonShare, night), halving every sunSetHalfDeg
// below the horizon, plus the moon's, night x moonShare x nightMoonScale: continuous through the horizon, the sun's by
// day (the moon's part is 0 at night 0) and the moon's in the deep night. The real-time sky lift divides by it, so the
// fill stays a share of the day's on either side. No allocation.
FTCORE_API NightFrame nightFrameAt(const NightLook& look, float night, float sunElevationDeg);
// 0 with the sun above deepHighDeg, 1 below deepLowDeg, eased between. No allocation.
FTCORE_API float nightDeep(const NightLook& look, float sunElevationDeg);
// Whether the floor fit drives the clock's deep fill: floorKey above 0 and moonShare the table's own (within 1e-4).
FTCORE_API bool nightFloorFitActive(const NightLook& look);
// The deep night's fill with the sun at this elevation: the floor fit's (NightFloorFit) when active, else skyFill.
// Continuous in the elevation. nightFrameAt falls from twilightFill to it on nightDeep. No allocation.
FTCORE_API float nightDeepFill(const NightLook& look, float sunElevationDeg);
// The floor fit's predicted wide-street centre median (0-255) at a moon elevation (the sun's depth) and a fill. No
// allocation.
FTCORE_API float nightFloorKey(const NightFloorFit& fit, float moonElevationDeg, float fill);
// The lamps', shops' and windows' switch-on fraction: 0 above lampsOnHighDeg, 1 from lampsOnLowDeg down, eased. No
// allocation.
FTCORE_API float nightLampFraction(const NightLook& look, float sunElevationDeg);
// Whether the lamp keyed `key` (a stable id: its index or a hash of its place) is on at a fraction: each lamp has its
// own point in 0.05..0.95 of the fraction, so the street comes on lamp by lamp. All off at 0, all on at 1. No allocation.
FTCORE_API bool nightLampOn(float fraction, std::uint32_t key);
// The exposure's blend from the day's range to the night's: 0 above exposureHighDeg, 1 from exposureLowDeg down. No
// allocation.
FTCORE_API float nightExposureBlend(const NightLook& look, float sunElevationDeg);
// The auto exposure's range for a frame, from the day's (the volume's own dayMin..dayMax): the day's at blend 0; from
// above 0 the min is duskMinAt (never under the day's), the max the day's blended toward the night's; exactly the night's
// at blend 1. Min never above max. No allocation.
struct ExposureRange {
    float minEV100 = -10.f;
    float maxEV100 = 20.f;
};
FTCORE_API ExposureRange nightExposureRange(const NightLook& look, const NightFrame& frame, float sunElevationDeg,
                                            float dayMin, float dayMax);
// The dusk table's min EV100 at an elevation: dayMin from exposureHighDeg up, the knots' between, nightMin from
// exposureLowDeg down; straight between neighbours, continuous everywhere. No allocation.
FTCORE_API float duskMinAt(const NightLook& look, float sunElevationDeg, float dayMin, float nightMin);
// The dusk table's real-time lift (on skyScale) at an elevation, given the whole lift there: whole from duskLiftTopDeg
// up and from exposureLowDeg down, the knots' between, in ratio; continuous everywhere. No allocation.
FTCORE_API float duskLiftAt(const NightLook& look, float sunElevationDeg, float whole);
// The moon's share of moonShare with the sun at this elevation (the moon stands opposite it): moonRiseShare at the
// horizon and below it (the sun up), 1 from moonFullDeg up, eased; continuous everywhere. No allocation.
FTCORE_API float nightMoonScale(const NightLook& look, float sunElevationDeg);

// A model of the wide street's metered median (linear, 1 = white) through the evening, for tests and for reading a
// strip against: the sky fill and the lamps light the street, the camera adapts within the day's range (a key it holds
// while it can open up enough) and is clamped by the night's as the exposure blend goes over. Weights fitted to the
// 11-15 strip (block 1: fill 1.24 / 0.55 / 0.14 / 0.033 / 0.012 read 49 / 37 / 20 / 11 / 10 of 255). The moon's direct
// light is left out on purpose: it adds direction and shadow, not the floor (nightMoonKey is its share alone). It is the
// old tone's model and a guard on the curves' order only: the measured floor is NightFloorFit's (11-16).
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
// The share of that fill kept at bayDistanceM from the nearest lit bay's front (negative: no lit bay): 0 inside
// charFillBayNearM, 1 from charFillBayFarM out, a smoothstep between. Far under near is read as one edge at far.
FTCORE_API float nightCharacterFillShare(const NightLook& look, float bayDistanceM);
// The soft key's yaw off the view in degrees, added to the camera's yaw (Unreal's: positive turns right, x forward, y
// right), for a view along (viewX, viewY) and the nearest light at (toLightX, toLightY) from the figure: the key comes
// from that light's side of the view, charFillYawDeg (clamped 0..90) off it. No light (a zero vector) or one dead ahead
// or behind: from the right. No allocation.
FTCORE_API float nightCharacterFillYaw(const NightLook& look, float viewX, float viewY, float toLightX, float toLightY);
// Whether the key moves from the light it follows (followedM away; negative: none followed) to a nearer one newM away:
// only when the new one is charFillSwitchNearer (clamped 0..0.9) nearer. No allocation.
FTCORE_API bool nightCharacterFillSwitchLight(const NightLook& look, float followedM, float newM);
// The key's yaw offset (degrees) a step of dtSeconds on from currentDeg toward targetDeg, turning the short way round
// at charFillYawRateDeg per second at most (0 or less: straight to the target). The result is wrapped to -180..180.
FTCORE_API float nightCharacterFillEaseYaw(const NightLook& look, float currentDeg, float targetDeg, float dtSeconds);
// The key's share of the fill (charFillKeyShare clamped 0..1); the ambient along the view takes the rest.
FTCORE_API float nightCharacterFillKeyShare(const NightLook& look);

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

// The leads' rim colour from the world (2026-11-16 ruling): the lit lamp that lights the lead most (candela over the
// squared distance, the distance floored at minDist) gives its light colour; which side of the camera it stands on
// (+1 camera-right, -1 camera-left) goes to the rim's preferred side. Positions in any one unit. No allocation.
struct RimLamp {
    float at[3] = {0.f, 0.f, 0.f};
    float candela = 0.f;
    bool on = false;
};
struct RimPick {
    int index = -1;       // -1: no lamp lit
    float side = 0.f;     // +1 / -1, 0 with none
    float weight = 0.f;   // candela / distance^2
};
FTCORE_API RimPick rimPickLamp(const RimLamp* lamps, std::size_t count, const float lead[3], const float cameraRight[3],
                               float minDist);
// The share of the way to its target a colour eased over `seconds` moves in dt: 1 - exp(-3 dt / seconds), so about
// 95% of a step in `seconds`; 1 when seconds <= 0. No allocation.
FTCORE_API float rimEaseShare(float dt, float seconds);
inline constexpr float kRimColourEaseSeconds = 0.5f;

} // namespace ftcore
