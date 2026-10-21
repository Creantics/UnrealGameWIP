#pragma once

// Loop points for music: when to jump back, where to land, and how to turn the file cursor
// (which drops back by the loop length at every jump) into a song time that keeps rising, so
// a Conductor fed with it never sees a seek.
//
//   ftcore::LoopRegion loop = ftcore::loopRegionFor(cue); // whole-file loop if no points
//   ftcore::LoopUnwrapper unwrap(loop);
//   // each frame, with the file cursor c:
//   if (ftcore::loopShouldRestart(loop, c, frameSeconds)) player.seek(ftcore::loopRestartPosition(loop, c));
//   conductor.observe(unwrap.unwrap(c), wall);
//
// No allocation.

#include "ftcore/Api.h"
#include "ftcore/AudioCues.h"
#include "ftcore/Conductor.h"

namespace ftcore {

struct LoopRegion {
    double start = -1.0;
    double end = -1.0;

    bool valid() const { return start >= 0.0 && end > start; }
    double length() const { return end - start; }
};

// The cue's loop points, or an invalid region when it has none.
FTCORE_API LoopRegion loopRegionFor(const AudioCue& cue);

// True once the cursor, run on by `lookahead` seconds (about one frame), reaches the loop end.
// Always false for an invalid region.
FTCORE_API bool loopShouldRestart(const LoopRegion& loop, double cursor, double lookahead);

// Where to seek when the cursor has reached or passed the loop end: the loop start plus however
// far past the end it ran, so the beat phase is kept when the jump comes a frame late. A cursor
// before the end is returned unchanged.
FTCORE_API double loopRestartPosition(const LoopRegion& loop, double cursor);

// Turns raw file cursors into a rising song time. A backward step of more than half the loop
// length is a wrap and adds one loop length; smaller backward steps are jitter and pass through.
// With an invalid region the cursor passes through unchanged.
class LoopUnwrapper {
public:
    LoopUnwrapper() = default;
    explicit LoopUnwrapper(LoopRegion loop) : loop_(loop) {}

    void reset() { wraps_ = 0; last_ = 0.0; seen_ = false; }
    void setRegion(LoopRegion loop) { loop_ = loop; reset(); }
    const LoopRegion& region() const { return loop_; }
    int wraps() const { return wraps_; }

    FTCORE_API double unwrap(double cursor);

private:
    LoopRegion loop_;
    int wraps_ = 0;
    double last_ = 0.0;
    bool seen_ = false;
};

// The beat grid a music cue gives: its bpm (or `fallbackBpm` when the cue has none) and offset,
// with the player's latency. Tracking settings keep their defaults.
FTCORE_API ConductorConfig conductorConfigFor(const AudioCue& cue, double latencyOffset, double fallbackBpm);

// True when the cue has a bpm and loop points whose length isn't a whole number of beats
// (within 1 ms), so the beat grid would slip at every loop.
FTCORE_API bool loopBreaksBeatGrid(const AudioCue& cue);

} // namespace ftcore
