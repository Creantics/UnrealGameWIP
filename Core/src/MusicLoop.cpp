#include "ftcore/MusicLoop.h"

#include <cmath>

namespace ftcore {

LoopRegion loopRegionFor(const AudioCue& cue) {
    LoopRegion r;
    if (cue.hasLoopPoints()) {
        r.start = cue.loopStartSeconds;
        r.end = cue.loopEndSeconds;
    }
    return r;
}

bool loopShouldRestart(const LoopRegion& loop, double cursor, double lookahead) {
    return loop.valid() && cursor + lookahead >= loop.end;
}

double loopRestartPosition(const LoopRegion& loop, double cursor) {
    if (!loop.valid() || cursor < loop.end) return cursor;
    return loop.start + std::fmod(cursor - loop.end, loop.length());
}

double LoopUnwrapper::unwrap(double cursor) {
    if (!loop_.valid()) return cursor;
    if (seen_ && cursor < last_ - 0.5 * loop_.length()) ++wraps_;
    last_ = cursor;
    seen_ = true;
    return cursor + wraps_ * loop_.length();
}

ConductorConfig conductorConfigFor(const AudioCue& cue, double latencyOffset, double fallbackBpm) {
    ConductorConfig c;
    c.bpm = cue.bpm > 0.f ? static_cast<double>(cue.bpm) : fallbackBpm;
    c.offsetSeconds = cue.offsetSeconds;
    c.latencyOffset = latencyOffset;
    return c;
}

bool loopBreaksBeatGrid(const AudioCue& cue) {
    if (cue.bpm <= 0.f || !cue.hasLoopPoints()) return false;
    const double beat = 60.0 / static_cast<double>(cue.bpm);
    const double length = static_cast<double>(cue.loopEndSeconds) - static_cast<double>(cue.loopStartSeconds);
    const double beats = length / beat;
    return std::abs(beats - std::round(beats)) * beat > 0.001;
}

} // namespace ftcore
