#pragma once

// A benchmark camera path: timed eye and look-at keys read from text, sampled by time. Fixed storage, no allocation.
// Text: one key per line, "t ex ey ez ax ay az" (seconds, then the eye and the point looked at, any one unit);
// '#' starts a comment, blank lines are skipped. Every value must be finite (no nan or inf); times must rise strictly;
// at least two keys, at most kMaxKeys.
#include "ftcore/Api.h"

#include <cstddef>
#include <string_view>

namespace ftcore {

struct BenchPose {
    double eye[3] = {0.0, 0.0, 0.0};
    double at[3] = {0.0, 0.0, 0.0};
};

struct BenchKey {
    double t = 0.0;
    BenchPose pose;
};

class BenchPath {
public:
    static constexpr std::size_t kMaxKeys = 64;

    // Replaces the keys with those in text. On failure keeps no keys and, if badLine is given, sets it to the
    // 1-based line at fault (0 when the whole text is at fault: fewer than two keys).
    FTCORE_API bool parse(std::string_view text, int* badLine = nullptr);
    // The pose at time t (seconds from the first key): a uniform Catmull-Rom curve through the keys for the eye and
    // for the look-at point, held at the first and last keys outside the path.
    FTCORE_API BenchPose sample(double t) const;
    std::size_t size() const { return count_; }
    const BenchKey& key(std::size_t i) const { return keys_[i]; }
    // Seconds from the first key to the last; 0 with fewer than two keys.
    double duration() const { return count_ < 2 ? 0.0 : keys_[count_ - 1].t - keys_[0].t; }

private:
    BenchKey keys_[kMaxKeys];
    std::size_t count_ = 0;
};

} // namespace ftcore
