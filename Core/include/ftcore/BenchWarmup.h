#pragma once

// When a benchmark's warm-up is over. Wall-clock seconds alone end it too early on a cold start: a few frames of
// several seconds each (shader and PSO compiles, first streaming) use up the seconds while hardly any frames run. So
// the warm-up also needs enough smooth frames: it ends once minSeconds have passed AND minSmoothFrames frames under
// smoothMs have run since the start. maxSeconds ends it regardless, so a scene that never settles still gets measured
// (timedOut() says so).
namespace ftcore {

struct BenchWarmup {
    double minSeconds = 5.0;
    int minSmoothFrames = 60;
    float smoothMs = 100.f;
    double maxSeconds = 120.0;

    // Feeds one frame's wall time (ms); true once the warm-up is over (and from then on).
    bool step(double frameMs) {
        if (done_) {
            return true;
        }
        ms_ += frameMs;
        if (frameMs < smoothMs) {
            ++smooth_;
        }
        if (ms_ >= maxSeconds * 1000.0) {
            timedOut_ = true;
            done_ = true;
        } else if (ms_ >= minSeconds * 1000.0 && smooth_ >= minSmoothFrames) {
            done_ = true;
        }
        return done_;
    }
    bool done() const { return done_; }
    bool timedOut() const { return timedOut_; }
    int smoothFrames() const { return smooth_; }
    double seconds() const { return ms_ / 1000.0; }

private:
    double ms_ = 0.0; // summed in ms: whole-ms frames add up exactly
    int smooth_ = 0;
    bool done_ = false;
    bool timedOut_ = false;
};

} // namespace ftcore
