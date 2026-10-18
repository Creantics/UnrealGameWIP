#include "ftcore/Calibration.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ftcore {

namespace {
double median(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}
} // namespace

CalibrationResult estimateLatency(std::span<const double> taps, std::span<const double> beats, double maxBeatFraction) {
    CalibrationResult r;
    if (beats.size() < 2 || taps.empty()) return r;

    std::vector<double> gaps;
    gaps.reserve(beats.size() - 1);
    for (std::size_t i = 1; i < beats.size(); ++i) gaps.push_back(beats[i] - beats[i - 1]);
    const double limit = median(gaps) * maxBeatFraction;
    if (!(limit > 0.0)) return r;

    std::vector<double> errors;
    errors.reserve(taps.size());
    for (double tap : taps) {
        const auto it = std::lower_bound(beats.begin(), beats.end(), tap);
        double best = 0.0;
        bool have = false;
        if (it != beats.end()) {
            best = tap - *it;
            have = true;
        }
        if (it != beats.begin()) {
            const double e = tap - *(it - 1);
            if (!have || std::abs(e) < std::abs(best)) best = e;
            have = true;
        }
        if (have && std::abs(best) <= limit) errors.push_back(best);
    }
    r.samples = static_cast<int>(errors.size());
    if (errors.empty()) return r;

    r.offset = median(errors);
    std::vector<double> dev;
    dev.reserve(errors.size());
    for (double e : errors) dev.push_back(std::abs(e - r.offset));
    r.spread = median(dev);
    r.reliable = r.samples >= kCalibrationMinSamples && r.spread < kCalibrationMaxSpread;
    return r;
}

} // namespace ftcore
