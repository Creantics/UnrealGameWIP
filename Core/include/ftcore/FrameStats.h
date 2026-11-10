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
    float max = 0.f;
};

struct FrameSummary {
    std::size_t count = 0;
    FrameChannel frame, game, render, gpu, drawCalls, primitives;
};

class FrameStats {
public:
    FTCORE_API explicit FrameStats(std::size_t capacity);
    // Keeps the sample while there is room; false once the buffer is full.
    FTCORE_API bool add(const FrameSample& s);
    std::size_t size() const { return used_; }
    // The percentiles of every channel; sorts a scratch copy (allocated once, at construction).
    FTCORE_API FrameSummary summary();

private:
    std::vector<FrameSample> samples_;
    std::vector<float> scratch_;
    std::size_t used_ = 0;
};

// The p-th percentile (0..1, nearest rank) of v[0..n), reordering v. 0 when n is 0.
FTCORE_API float framePercentile(float* v, std::size_t n, double p);

} // namespace ftcore
