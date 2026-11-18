#pragma once

// The day's own look (-DayLook, off by default): what a world wears by full day beyond its look row, so the day stops
// borrowing the night's settings. Every lever is keyed to the sun's elevation, never to an hour on the command line:
// the day amount is dayShadeAmount (1 by full day, eased out on the sun's elevation, 0 before the lamps start and all
// night), and at amount 0 every function below hands back the row's own value exactly, so the night is untouched.
//
//   dayLookShade        the day's shadow floor, as a bracket step on DayShadeLook (step 1, 2, 3: sky light 2.0/1.7/1.4,
//                       shadow contrast 0.6/0.7/0.8, highlight contrast 0.85/0.825/0.8, diffuse boost 1.6/1.45/1.3; any
//                       other step keeps the defaults), and the fog's cap (fogMaxOpacity) for the aerial perspective.
//   dayLookScene        the row blended toward the day by the amount: the fog's density (aerial perspective), the split
//                       tone (the shadow tint toward a cool hue and the highlight tint toward a warm one, blended in OKLab
//                       a/b so the hue never jumps) and the sun's colour temperature. Two fields are not blended, since
//                       a moving value would pop, so both are held all session from the moment the look is set
//                       (heldDay: the day amount then was over 0); a look set at night keeps the row's own all session,
//                       so a night start is the row exactly:
//                         windMps     the larger of the row's and the day's, at every hour: the clouds, the sea and
//                                     the plants all move by wind times time, so a wind that changes speed jumps them.
//                         cloudCover  the day's cover at every hour when the row has none of its own; the row's
//                                     otherwise. The layer swaps material between no cover and any cover (the
//                                     engine's own clouds against the world's), so the cover never changes with the
//                                     hour: the swap happens once, when the look is set.
//   dayLookCloudsOn     the clouds' shadows on the ground (the sun's cloud shadows): on until the night is full
//                       (night < 1), off by night, when the same light is the moon and a rain night's full cover would
//                       darken the street.
//   dayLookSignStrength the far signs' light: the night's strength at full lights, by day signWhiteRatio times the
//                       luminance of a sunlit white face (the sun's lux times its elevation's sine plus a sky share, over
//                       pi), blended by the lights share (so a sign is at the night's strength whenever the shop lights
//                       are, never on a day exposure).
//   dayLookContactShadow the sun's contact shadow length (screen share), blended from the light's own by the amount.
//
// A lever is switched off by a negative value in the spec (sunKelvin: 0 or less; shadeStep: any but 1..3), so a lever that
// fails its proof drops alone: that field keeps the row's own (the sign, the night's strength; the contact shadow, the
// light's own; the fog cap, the defaults').
//
// Plain C++20; nothing allocates.

#include "ftcore/Api.h"
#include "ftcore/DayShade.h"
#include "ftcore/SceneLook.h"

namespace ftcore {

struct DayLookSpec {
    int shadeStep = 2;
    float fogDensity = 0.02f;
    float fogMaxOpacity = 0.85f;
    float shadowTintHue = 240.f; // OKLab hue, degrees: a cool blue
    float shadowTintChroma = 0.025f;
    float highlightTintHue = 70.f; // a warm yellow-orange
    float highlightTintChroma = 0.012f;
    float sunKelvin = 5400.f;
    float contactShadowLength = 0.02f;
    float windMps = 4.f;
    float cloudCover = 0.5f;
    float signWhiteRatio = 1.75f;
    float signSkyShare = 0.25f;
};

inline constexpr int kDayLookShadeSteps = 3;

// Defaults with the bracket step's four DayShade levers and the day's fog cap (spec.fogMaxOpacity) written over them.
FTCORE_API DayShadeLook dayLookShade(const DayShadeLook& defaults, const DayLookSpec& spec);

// The row blended toward the day by amount (clamped to 0..1; NaN reads as 0). At amount 0 the result equals the row in
// every field except windMps and cloudCover, which take the day's only when heldDay (see above); a row with wind of at
// least spec.windMps and a cover of its own (the rain night's) comes back exactly either way.
FTCORE_API SceneLook dayLookScene(const SceneLook& row, const DayLookSpec& spec, float amount, bool heldDay = true);

// On until the night is full: night (0..1) under 1. NaN reads as full night.
FTCORE_API bool dayLookCloudsOn(float night);

// The far signs' strength (SignStrength) for the sun's lux and elevation and the lights share (0..1). Exactly
// nightStrength at lights 1 or more; never negative.
FTCORE_API float dayLookSignStrength(const DayLookSpec& spec, float nightStrength, float sunLux, float sunElevationDeg,
                                     float lights);

// The sun's contact shadow length: own at amount 0 (exactly), the day's at 1.
FTCORE_API float dayLookContactShadow(const DayLookSpec& spec, float own, float amount);

} // namespace ftcore
