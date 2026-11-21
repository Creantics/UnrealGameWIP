#pragma once

// The grade of a cloth's anim drive (the spring pulling each sim vertex back toward its skinned place), for FTBuildCloth.
// The map is graded by the cloth's own mask level r = max distance / Max (0 where it is pinned, 1 where it is freest), so it
// follows the same rows as the max-distance ramp: on a cape whose mask is a row ramp, every vertex of a row gets one value.
// The weight is 1 up to `from`, eases (smoothstep) to 0 at `to`, and is 0 beyond. Chaos turns it into a stiffness parameter
// Low + (High - Low) x weight (both 0..1) and then fits that on a log scale: a parameter v pulls a fraction
// (1000^v - 1) / 999 of the way back per solver step (Chaos/PBDStiffness.h, CalcExponentialParameterFit), so 0.5 is about
// 3 % and 1 is rigid. The damping has no map here: one value for every vertex (Chaos then uses its Low only).
// The flag check below (validate) runs before the mesh is touched. Plain C++20, header only, no allocation.

#include <cmath>

namespace ftcore::clothdrive {

// The Third's cape (run 2 first guess): full drive down to mask level 0.2 (row 7, just under the shoulder band), easing
// to Low at the hem. High 0.6 pulls about 6 % back per step, Low 0.4 about 1.5 %: the bell comes back within a few
// tenths of a second while the arm capsules, solved after the drive, still push it out.
inline constexpr float kFrom = 0.2f;
inline constexpr float kTo = 1.f;

// 0..1; NaN reads as 0.
constexpr float unit(float v) { return v > 0.f ? (v < 1.f ? v : 1.f) : 0.f; }

constexpr bool inUnit(float v) { return v >= 0.f && v <= 1.f; } // false for NaN

// The map's weight at mask level r. `from` < `to` (validate() makes sure); NaN r reads as pinned (weight 1).
constexpr float weight(float r, float from, float to) {
    const float x = unit(r);
    if (!(x > from)) {
        return 1.f;
    }
    if (!(x < to)) {
        return 0.f;
    }
    const float t = (x - from) / (to - from);
    return 1.f - t * t * (3.f - 2.f * t);
}

// The stiffness parameter Chaos gives a vertex of that weight.
constexpr float stiffness(float low, float high, float w) { return unit(low) + (unit(high) - unit(low)) * unit(w); }

// The fraction of the way back to the skinned place one solver step takes at stiffness parameter v.
inline float pullPerStep(float v) { return (std::pow(1000.f, unit(v)) - 1.f) / 999.f; }

// What the command line asked for. has* is whether the flag was given.
struct Args {
    bool map = false;          // -AnimDriveMap: write the graded map
    bool hasLow = false;       // -AnimDriveLow=
    float low = 0.f;
    bool hasHigh = false;      // -AnimDriveHigh=
    float high = 0.f;
    bool hasFrom = false;      // -DriveFrom=
    float from = kFrom;
    bool hasTo = false;        // -DriveTo=
    float to = kTo;
    bool hasDamping = false;   // -AnimDriveDamping=
    float damping = 0.f;
    float max = 0.f;           // -Max= (the map divides the max distance by it)
};

enum class Check {
    Ok,
    MapWithoutRange,     // -AnimDriveMap needs -AnimDriveLow= and -AnimDriveHigh= (the default range 0..1 is a rigid top)
    RangeOutOfUnit,      // Low or High outside 0..1
    LowAboveHigh,
    HighWithoutMap,      // without a map Chaos reads Low only, so a High would do nothing
    GradeWithoutMap,     // -DriveFrom= / -DriveTo= without -AnimDriveMap
    GradeOutOfUnit,
    GradeOrder,          // from must be below to
    DampingOutOfUnit,
    MaxNotPositive,      // the map needs -Max= above 0
};

constexpr Check validate(const Args& a) {
    if (a.map && !(a.hasLow && a.hasHigh)) {
        return Check::MapWithoutRange;
    }
    if ((a.hasLow && !inUnit(a.low)) || (a.hasHigh && !inUnit(a.high))) {
        return Check::RangeOutOfUnit;
    }
    if (a.hasLow && a.hasHigh && a.low > a.high) {
        return Check::LowAboveHigh;
    }
    if (a.hasHigh && !a.map) {
        return Check::HighWithoutMap;
    }
    if ((a.hasFrom || a.hasTo) && !a.map) {
        return Check::GradeWithoutMap;
    }
    if (!inUnit(a.from) || !inUnit(a.to)) {
        return Check::GradeOutOfUnit;
    }
    if (!(a.from < a.to)) {
        return Check::GradeOrder;
    }
    if (a.hasDamping && !inUnit(a.damping)) {
        return Check::DampingOutOfUnit;
    }
    if (a.map && !(a.max > 0.f)) {
        return Check::MaxNotPositive;
    }
    return Check::Ok;
}

constexpr const char* describe(Check c) {
    switch (c) {
    case Check::Ok: return "ok";
    case Check::MapWithoutRange: return "-AnimDriveMap needs -AnimDriveLow= and -AnimDriveHigh= (the default range would make the top rigid)";
    case Check::RangeOutOfUnit: return "-AnimDriveLow= and -AnimDriveHigh= must be 0..1";
    case Check::LowAboveHigh: return "-AnimDriveLow= is above -AnimDriveHigh=";
    case Check::HighWithoutMap: return "-AnimDriveHigh= does nothing without -AnimDriveMap (Chaos reads Low only); pass the map or Low alone";
    case Check::GradeWithoutMap: return "-DriveFrom= and -DriveTo= need -AnimDriveMap";
    case Check::GradeOutOfUnit: return "-DriveFrom= and -DriveTo= must be 0..1";
    case Check::GradeOrder: return "-DriveFrom= must be below -DriveTo=";
    case Check::DampingOutOfUnit: return "-AnimDriveDamping= must be 0..1";
    case Check::MaxNotPositive: return "-AnimDriveMap needs -Max= above 0";
    }
    return "unknown";
}

} // namespace ftcore::clothdrive
