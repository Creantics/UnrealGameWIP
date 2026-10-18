#pragma once

#include "ftcore/Api.h"

#include <span>

namespace ftcore {

struct CalibrationResult {
    double offset = 0.0;  // median(tap - beat), seconds; positive = player taps late. Feed to ConductorConfig::latencyOffset
    double spread = 0.0;  // median absolute deviation of (tap - beat), seconds
    int samples = 0;      // taps that were paired with a beat (outliers excluded)
    bool reliable = false;
};

inline constexpr int kCalibrationMinSamples = 8;
inline constexpr double kCalibrationMaxSpread = 0.030; // seconds

// Pairs each tap with its nearest beat time, drops pairs further apart than
// maxBeatFraction of the beat period (the median beat spacing), and returns the median
// error. `beatTimes` must be ascending with at least two entries; both arrays are in the
// same clock (song time). Offsets larger than the rejection distance cannot be measured;
// calibrate with a slower tempo if you suspect that. Allocates scratch space: call it once
// when a calibration ends, not per frame.
FTCORE_API CalibrationResult estimateLatency(std::span<const double> tapTimes, std::span<const double> beatTimes,
                                             double maxBeatFraction = 0.25);

} // namespace ftcore
