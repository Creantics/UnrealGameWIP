#pragma once

// Dev-only still switches (the Unreal side reads them only outside Shipping):
// - -StillCam=x,y,z,yawDeg,pitchDeg,fovDeg: a fixed view for a still, in -WorldStartAt's units (x and y plan metres on a
//   map in the plan frame, z metres; yaw world degrees; pitch negative looks down; fov horizontal degrees).
// - -Weather=dry: clear weather for the whole run (no rain, no puddles, ground wetness 0, the dry sibling of a rain look).
// A string that is not exactly right is off as a whole, never half read.

#include "ftcore/Api.h"

#include <string_view>

namespace ftcore {

struct StillCam {
    bool on = false;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double yawDeg = 0.0;
    double pitchDeg = 0.0;
    double fovDeg = 0.0;
};

// Exactly six finite numbers, comma separated (spaces allowed round each); pitch in [-89, 89], fov in [10, 150].
// Anything else (a part missing or extra, empty, not a number, out of range) gives on == false.
FTCORE_API StillCam parseStillCam(std::string_view text);

enum class WeatherForce : unsigned char { none, dry };

// "dry" (any case, spaces trimmed) gives dry; anything else none. *known (if given) is false for a non-empty string
// that is not a known word, so the caller can warn.
FTCORE_API WeatherForce parseWeatherForce(std::string_view text, bool* known = nullptr);

// The look a dry run wears in place of `look`: a rain look ("<world>_rain") gives its world's look, any other is kept.
FTCORE_API std::string_view dryLookFor(std::string_view look);

} // namespace ftcore
