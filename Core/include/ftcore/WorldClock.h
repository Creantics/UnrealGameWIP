#pragma once

// The world's time of day: a clock that runs a game day in a set number of real minutes, the sun it puts in the sky,
// how dark that makes it (NightAmount), and the time curves other systems read (how busy the streets are, how dangerous).
//
// The clock is plain data so the save can hold it and story or cutscene code can set, pause and resume it. Hours run
// 0..24 (12 = noon) and wrap, counting a new day each time they pass midnight forwards. The sun comes from the hour and
// a latitude on an equinox day (declination 0), so it rises at 06:00 and sets at 18:00 everywhere. NightAmount is 0 with
// the sun more than twilightHigh degrees up and 1 with it more than twilightLow degrees down, smoothstepped between.
//
// Curves are data (Data/time_curves.csv): a header row of hours and one row per curve, read piecewise linear and wrapped
// at 24 h, so no system hard-codes an hour. Plain C++20. No allocation except parseTimeCurves (load time only).

#include "ftcore/Api.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace ftcore {

struct WorldClockSpec {
    float latitudeDeg = 22.0f;  // the city's latitude (coastal, subtropical)
    float twilightHigh = 6.0f;  // sun elevation (degrees) at which night starts to fall
    float twilightLow = -6.0f;  // sun elevation at which it is full night
    // The sun's path turned this many degrees clockwise (seen from above) against the plan's compass: the plan's
    // north is not the sky's. 58 puts the start hour's sun (kWorldClockStartHours, about 28 degrees up) about 20 degrees
    // off the city's main north-south street, so it rakes along the street's facades and throws long shadows.
    float azimuthOffsetDeg = 58.0f;
};

// The hour a fresh game day starts at (a low morning sun); the map builders bake the same sun into their maps.
inline constexpr double kWorldClockStartHours = 8.0;

inline constexpr float kWorldClockDefaultDayMinutes = 48.0f;  // 2 real minutes per game hour
inline constexpr float kWorldClockMinDayMinutes = 1.0f;
inline constexpr float kWorldClockMaxDayMinutes = 24.0f * 60.0f;  // real time

// The clock's whole state: what a save holds.
struct WorldClock {
    double hours = 12.0;                                // 0 <= hours < 24
    bool paused = false;
    float dayMinutes = kWorldClockDefaultDayMinutes;    // real minutes per game day
    std::uint32_t day = 0;                              // days passed since the clock started
};

// Advances an unpaused clock by dtSeconds of real time; a new day each time it passes midnight. Negative or non-finite
// dt does nothing. No allocation.
FTCORE_API void worldClockTick(WorldClock& clock, float dtSeconds);
// Sets the hour (wrapped into 0..24; the day count is left alone, so a cutscene that jumps to 06:00 does not add a
// day). Non-finite hours are ignored. No allocation.
FTCORE_API void worldClockSet(WorldClock& clock, double hours);
// Moves the clock forward to the next time it reads `hours` (the same hour counts as now), counting the days it
// passes: "sleep until morning". No allocation.
FTCORE_API void worldClockAdvanceTo(WorldClock& clock, double hours);
FTCORE_API void worldClockPause(WorldClock& clock);
FTCORE_API void worldClockResume(WorldClock& clock);
// Sets the real minutes per game day, clamped to kWorldClockMinDayMinutes..kWorldClockMaxDayMinutes. No allocation.
FTCORE_API void worldClockSetDayMinutes(WorldClock& clock, float minutes);
// The clock made valid: hours wrapped into 0..24 (0 when not finite), day length clamped (default when not finite).
// Loading a save runs it. No allocation.
FTCORE_API WorldClock worldClockSanitized(const WorldClock& clock);

// The sun's direction at this hour: a unit vector, x east, y north, z up (the plan's compass, after
// azimuthOffsetDeg). No allocation.
struct SunDirection {
    float east = 0.0f, north = 0.0f, up = 1.0f;
};
FTCORE_API SunDirection sunDirection(const WorldClockSpec& spec, double hours);
// The sun's elevation above the horizon in degrees (-90..90). No allocation.
FTCORE_API float sunElevationDeg(const WorldClockSpec& spec, double hours);
// 0 by day .. 1 by night, from the sun's elevation. No allocation.
FTCORE_API float nightAmount(const WorldClockSpec& spec, double hours);

// A curve over the day: up to kTimeCurveMaxKeys (hour, value) keys, hours rising in 0..24, read piecewise linear and
// wrapped (the last key runs on to the first one 24 h later). No allocation.
inline constexpr std::uint32_t kTimeCurveMaxKeys = 24;
struct TimeCurve {
    std::array<float, kTimeCurveMaxKeys> hours{};
    std::array<float, kTimeCurveMaxKeys> values{};
    std::uint32_t count = 0;
};
FTCORE_API float timeCurveAt(const TimeCurve& curve, double hours);  // 0 when the curve has no keys

// The curves the game reads. CrowdDensity: share of the daytime crowd out on the streets (0..1). Danger: how dangerous
// the streets are (0..1), for the enemy spawner.
struct TimeCurves {
    TimeCurve crowd;
    TimeCurve danger;
};
// Reads Data/time_curves.csv: comment rows start with '#'; a header row "curve,<hour>,<hour>,..." (hours rising,
// 0 <= h < 24); then one row per curve (CrowdDensity, Danger, both required, each once) with a value in 0..1 per hour.
// False with "line N: ..." on anything else. Allocates: load time only.
FTCORE_API bool parseTimeCurves(std::string_view csv, TimeCurves& out, std::string* error = nullptr);

}  // namespace ftcore
