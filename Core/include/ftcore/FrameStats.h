#pragma once

// Frame timing for a profile run: a fixed sample buffer (no allocation once made) and its percentiles.
#include "ftcore/Api.h"

#include <cstddef>
#include <vector>

namespace ftcore {

struct FrameSample {
    float frameMs = 0.f;
    float gameMs = 0.f;
    float renderMs = 0.f;
    float gpuMs = 0.f;
    float drawCalls = 0.f;
    float primitives = 0.f;
};

// One channel's summary over the kept samples.
struct FrameChannel {
    float median = 0.f;
    float p95 = 0.f;
    float p99 = 0.f;
    float max = 0.f;
};

struct FrameSummary {
    std::size_t count = 0;
    FrameChannel frame, game, render, gpu, drawCalls, primitives;
    // Kept frames whose frame time is above kHitchMs and kSevereHitchMs (33 and 50 ms).
    std::size_t over33 = 0;
    std::size_t over50 = 0;
};

constexpr float kHitchMs = 33.f;
constexpr float kSevereHitchMs = 50.f;

// One kept frame over a threshold: its position among the kept samples and its timings.
struct FrameHitch {
    std::size_t index = 0;
    FrameSample sample;
};

class FrameStats {
public:
    FTCORE_API explicit FrameStats(std::size_t capacity);
    // Keeps the sample while there is room; false once the buffer is full.
    FTCORE_API bool add(const FrameSample& s);
    std::size_t size() const { return used_; }
    // The percentiles of every channel; sorts a scratch copy (allocated once, at construction).
    FTCORE_API FrameSummary summary();
    // Writes the kept frames whose frame time is above overMs, in order, up to cap of them, to out; returns how many
    // there are in all (which may be more than cap). No allocation.
    FTCORE_API std::size_t hitches(float overMs, FrameHitch* out, std::size_t cap) const;

private:
    std::vector<FrameSample> samples_;
    std::vector<float> scratch_;
    std::size_t used_ = 0;
};

// The p-th percentile (0..1, nearest rank) of v[0..n), reordering v. 0 when n is 0.
FTCORE_API float framePercentile(float* v, std::size_t n, double p);

} // namespace ftcore
