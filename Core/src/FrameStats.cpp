#include "ftcore/FrameStats.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

FrameStats::FrameStats(std::size_t capacity) : samples_(capacity), scratch_(capacity) {}

bool FrameStats::add(const FrameSample& s) {
    if (used_ >= samples_.size()) {
        return false;
    }
    samples_[used_++] = s;
    return true;
}

float framePercentile(float* v, std::size_t n, double p) {
    if (n == 0) {
        return 0.f;
    }
    const double clamped = std::clamp(p, 0.0, 1.0);
    std::size_t rank = static_cast<std::size_t>(std::ceil(clamped * static_cast<double>(n)));
    rank = rank == 0 ? 0 : rank - 1;
    std::nth_element(v, v + rank, v + n);
    return v[rank];
}

FrameSummary FrameStats::summary() {
    FrameSummary out;
    out.count = used_;
    auto channel = [this](float FrameSample::*field) {
        for (std::size_t i = 0; i < used_; ++i) {
            scratch_[i] = samples_[i].*field;
        }
        FrameChannel c;
        c.median = framePercentile(scratch_.data(), used_, 0.5);
        c.p95 = framePercentile(scratch_.data(), used_, 0.95);
        c.max = framePercentile(scratch_.data(), used_, 1.0);
        return c;
    };
    out.frame = channel(&FrameSample::frameMs);
    out.game = channel(&FrameSample::gameMs);
    out.render = channel(&FrameSample::renderMs);
    out.gpu = channel(&FrameSample::gpuMs);
    out.drawCalls = channel(&FrameSample::drawCalls);
    out.primitives = channel(&FrameSample::primitives);
    return out;
}

} // namespace ftcore
