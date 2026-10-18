#pragma once

#include "ftcore/Api.h"

#include <array>
#include <cstddef>

namespace ftcore {

struct ConductorConfig {
    double bpm = 120.0;
    double offsetSeconds = 0.0;  // song time of beat 0
    // Calibration: how late (seconds) this player's presses arrive relative to what they
    // hear. Positive = taps land after the beat. Subtracted from event times; see
    // estimateLatency() for a measured value.
    double latencyOffset = 0.0;

    // Tracking behaviour (defaults suit a 10-30 ms device period and 60 Hz frames).
    double windowSeconds = 2.0;      // observations older than this are forgotten
    double snapThreshold = 0.100;    // clock disagreement beyond this is a seek/restart: snap
    double slewRate = 0.05;          // max seconds of correction per second of wall time below it
    double cursorStepHint = 0.0;     // device period if known (0 = learn it from the cursor)
    double maxExtrapolation = 0.100; // never run further than this past the latest cursor value
};

// Turns the audio cursor, which advances in device-period steps, into a smooth song clock.
//
// Math. Let c be the cursor observed at wall time w. The cursor is the true audio position
// floored to a multiple of the step P, so truth(w) - c is uniform in [0, P) and
//     d(w) = truth(w) - w  ~=  mean over a window of (c_i - w_i)  +  P / 2.
// P is learnt as the smallest positive cursor increment in the window (or cursorStepHint).
// The averaged offset dEst is the best estimate; the displayed clock uses a separate offset
// dOut that chases dEst at no more than slewRate (so the clock runs at 0.95x-1.05x and is
// strictly monotonic) and snaps to it if the two differ by more than snapThreshold:
//     songTime(w) = min(w + dOut, latestCursor + maxExtrapolation), never decreasing.
// The cap makes a paused cursor freeze the clock within maxExtrapolation; when the cursor
// moves again continuously the clock stalls briefly until the audio catches up. A cursor
// that jumps while playing, or after a pause by more than snapThreshold, is a seek: the clock
// snaps (and may go backwards; seekCount() increments).
// All times are seconds in the audio's own timeline: pass the audible clock to get what the
// player hears, or the raw one to ignore output latency.
//
// Never allocates: observations live in a fixed ring of kMaxSamples. If frames come faster
// than kMaxSamples per windowSeconds, the oldest are dropped early and the window shrinks.
class Conductor {
public:
    static constexpr std::size_t kMaxSamples = 512;

    Conductor() = default;
    explicit Conductor(ConductorConfig cfg) : cfg_(cfg) {}

    const ConductorConfig& config() const { return cfg_; }
    void setBpm(double bpm) { cfg_.bpm = bpm; }
    void setOffsetSeconds(double s) { cfg_.offsetSeconds = s; }
    void setLatencyOffset(double s) { cfg_.latencyOffset = s; }

    // Forget all observations.
    FTCORE_API void reset();
    // Feed the audio cursor (seconds) as read at wall time `wallTime` (seconds, any epoch
    // as long as it is the one used everywhere). Call once per frame.
    FTCORE_API void observe(double audioCursorSeconds, double wallTime);

    bool valid() const { return seen_; }
    // True while the cursor has stopped advancing (paused or stalled).
    bool paused() const { return paused_; }
    int seekCount() const { return seeks_; }
    // Learnt device period, 0 if unknown.
    double stepEstimate() const { return step_; }

    // Smooth song time for display/scheduling at wall time (call with nondecreasing times).
    // 0 before the first observation.
    FTCORE_API double songTime(double wallTime);
    // Song time of an input event that happened at `eventWallTime`, latency-compensated.
    // This is what judgement must use. Does not touch the display clock.
    FTCORE_API double songTimeForEvent(double eventWallTime) const;

    // Beat grid helpers (no latency applied).
    double beatAt(double songTime) const { return (songTime - cfg_.offsetSeconds) * cfg_.bpm / 60.0; }
    double timeAtBeat(double beat) const { return cfg_.offsetSeconds + beat * 60.0 / cfg_.bpm; }
    double secondsPerBeat() const { return 60.0 / cfg_.bpm; }

private:
    struct Sample {
        double wall;
        double d;      // cursor - wall
        double delta;  // positive cursor increment vs the previous sample, else 0
    };
    void estimate();
    void snapTo(double d);
    void pushSample(const Sample& s);
    void clearSamples() { head_ = count_ = 0; }
    const Sample& sampleAt(std::size_t i) const { return samples_[(head_ + i) % kMaxSamples]; }

    ConductorConfig cfg_;
    std::array<Sample, kMaxSamples> samples_{};
    std::size_t head_ = 0;  // oldest sample
    std::size_t count_ = 0;
    bool seen_ = false, paused_ = false;
    double cursor_ = 0.0;  // latest cursor value
    double lastChangeWall_ = 0.0;
    double lastObsWall_ = 0.0;
    double step_ = 0.0;
    double dEst_ = 0.0, dOut_ = 0.0;
    double lastOut_ = 0.0;
    double lastSlewWall_ = 0.0;
    bool allowBackwards_ = false;
    int seeks_ = 0;
};

} // namespace ftcore
