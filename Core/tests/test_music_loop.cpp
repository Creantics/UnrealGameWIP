#include "ftcore/Conductor.h"
#include "ftcore/MusicLoop.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

AudioCue loopCue(float bpm, float offset, float start, float end) {
    AudioCue c;
    c.id = "MUS-T";
    c.bus = AudioBus::Music;
    c.playback = CuePlayback::Loop;
    c.bpm = bpm;
    c.offsetSeconds = offset;
    c.loopStartSeconds = start;
    c.loopEndSeconds = end;
    return c;
}

} // namespace

TEST_CASE("a cue without loop points gives no loop and passes the cursor through") {
    AudioCue c = loopCue(120.f, 0.f, -1.f, -1.f);
    const LoopRegion r = loopRegionFor(c);
    CHECK_FALSE(r.valid());
    CHECK_FALSE(loopShouldRestart(r, 1000.0, 0.1));
    CHECK(loopRestartPosition(r, 12.5) == doctest::Approx(12.5));
    LoopUnwrapper u(r);
    CHECK(u.unwrap(10.0) == doctest::Approx(10.0));
    CHECK(u.unwrap(0.5) == doctest::Approx(0.5)); // a whole-file wrap stays a seek for the conductor
}

TEST_CASE("restart comes a frame before the end and keeps the phase when it is late") {
    const LoopRegion r = loopRegionFor(loopCue(120.f, 0.5f, 2.5f, 10.5f));
    REQUIRE(r.valid());
    CHECK(r.length() == doctest::Approx(8.0));
    CHECK_FALSE(loopShouldRestart(r, 10.4, 1.0 / 60.0));
    CHECK(loopShouldRestart(r, 10.49, 1.0 / 60.0));
    CHECK(loopRestartPosition(r, 10.5) == doctest::Approx(2.5));
    CHECK(loopRestartPosition(r, 10.52) == doctest::Approx(2.52));
    CHECK(loopRestartPosition(r, 9.0) == doctest::Approx(9.0));
    CHECK(loopRestartPosition(r, 18.75) == doctest::Approx(2.75)); // a whole loop late still lands in range
}

TEST_CASE("the unwrapped cursor keeps rising through several loops and ignores jitter") {
    const LoopRegion r = loopRegionFor(loopCue(120.f, 0.f, 2.f, 6.f));
    LoopUnwrapper u(r);
    double prev = 0.0;
    double cursor = 0.0;
    const double frame = 1.0 / 60.0;
    // Four passes through the loop, played the way the subsystem would play them.
    for (int i = 0; i < 60 * 20; ++i) {
        if (loopShouldRestart(r, cursor, frame)) cursor = loopRestartPosition(r, cursor + frame);
        else cursor += frame;
        const double t = u.unwrap(cursor);
        CHECK(t > prev);
        CHECK(t - prev < 2.0 * frame);
        prev = t;
    }
    CHECK(u.wraps() >= 4);
    CHECK(prev == doctest::Approx(20.0).epsilon(0.01));

    const int wraps = u.wraps();
    const double a = u.unwrap(4.0);
    const double b = u.unwrap(3.99); // a cursor that steps back a little is jitter, not a wrap
    CHECK(u.wraps() == wraps);
    CHECK(a - b == doctest::Approx(0.01));
    u.reset();
    CHECK(u.unwrap(3.0) == doctest::Approx(3.0));
}

TEST_CASE("a conductor fed the unwrapped cursor sees no seek at the loop") {
    const AudioCue cue = loopCue(120.f, 0.f, 2.f, 6.f);
    const LoopRegion r = loopRegionFor(cue);
    Conductor c(conductorConfigFor(cue, 0.0, 100.0));
    LoopUnwrapper u(r);
    double cursor = 0.0, wall = 0.0;
    const double frame = 1.0 / 60.0;
    for (int i = 0; i < 60 * 15; ++i) {
        wall += frame;
        if (loopShouldRestart(r, cursor, frame)) cursor = loopRestartPosition(r, cursor + frame);
        else cursor += frame;
        c.observe(u.unwrap(cursor), wall);
    }
    CHECK(c.seekCount() == 0);
    CHECK(c.songTime(wall) == doctest::Approx(wall).epsilon(0.01));
}

TEST_CASE("the beat grid comes from the cue, with a fallback bpm") {
    const ConductorConfig a = conductorConfigFor(loopCue(96.f, 0.25f, -1.f, -1.f), 0.03, 120.0);
    CHECK(a.bpm == doctest::Approx(96.0));
    CHECK(a.offsetSeconds == doctest::Approx(0.25));
    CHECK(a.latencyOffset == doctest::Approx(0.03));
    const ConductorConfig b = conductorConfigFor(loopCue(0.f, 0.f, -1.f, -1.f), 0.0, 120.0);
    CHECK(b.bpm == doctest::Approx(120.0));
}

TEST_CASE("a loop that isn't a whole number of beats is flagged") {
    CHECK_FALSE(loopBreaksBeatGrid(loopCue(120.f, 0.f, 2.f, 6.f)));  // 8 beats
    CHECK(loopBreaksBeatGrid(loopCue(120.f, 0.f, 2.f, 6.2f)));       // 8.4 beats
    CHECK_FALSE(loopBreaksBeatGrid(loopCue(0.f, 0.f, 2.f, 6.2f)));   // no tempo, nothing to break
    CHECK_FALSE(loopBreaksBeatGrid(loopCue(120.f, 0.f, -1.f, -1.f))); // no loop points
}

TEST_CASE("importAudioCues warns when a loop slips off the beat grid") {
    AudioCueTable table;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE(importAudioCues(R"J({ "version": 1, "cues": [
        { "id": "MUS-A", "bus": "music", "playback": "loop", "bpm": 120, "loopStart": 2, "loopEnd": 6 },
        { "id": "MUS-B", "bus": "music", "playback": "loop", "bpm": 120, "loopStart": 2, "loopEnd": 6.2 } ] })J",
                            table, &error, &warnings));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("cues[1]") != std::string::npos);
}

TEST_CASE("loop tracking does not allocate") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    const LoopRegion r{2.0, 6.0};
    LoopUnwrapper u(r);
    const std::uint64_t before = ftcore_test::allocStats().count;
    double cursor = 0.0, sum = 0.0;
    for (int i = 0; i < 1000; ++i) {
        cursor = loopShouldRestart(r, cursor, 0.02) ? loopRestartPosition(r, cursor + 0.02) : cursor + 0.02;
        sum += u.unwrap(cursor);
    }
    CHECK(sum > 0.0);
    CHECK(ftcore_test::allocStats().count == before);
}
