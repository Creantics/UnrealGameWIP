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
        c.p99 = framePercentile(scratch_.data(), used_, 0.99);
        c.max = framePercentile(scratch_.data(), used_, 1.0);
        return c;
    };
    out.frame = channel(&FrameSample::frameMs);
    out.game = channel(&FrameSample::gameMs);
    out.render = channel(&FrameSample::renderMs);
    out.gpu = channel(&FrameSample::gpuMs);
    out.drawCalls = channel(&FrameSample::drawCalls);
    out.primitives = channel(&FrameSample::primitives);
    for (std::size_t i = 0; i < used_; ++i) {
        out.over33 += samples_[i].frameMs > kHitchMs ? 1 : 0;
        out.over50 += samples_[i].frameMs > kSevereHitchMs ? 1 : 0;
    }
    return out;
}

std::size_t FrameStats::hitches(float overMs, FrameHitch* out, std::size_t cap) const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < used_; ++i) {
        if (samples_[i].frameMs > overMs) {
            if (out && n < cap) {
                out[n].index = i;
                out[n].sample = samples_[i];
            }
            ++n;
        }
    }
    return n;
}

} // namespace ftcore
