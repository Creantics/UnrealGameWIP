#include "ftcore/Calibration.h"
#include "ftcore/Conductor.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace ftcore;

namespace {

constexpr double kFrame = 1.0 / 60.0;

// Audio cursor as a device would report it: truth floored to a multiple of the period.
double quantised(double truth, double period) { return std::floor(truth / period + 1e-9) * period; }

struct Run {
    double maxErr = 0.0; // after warm-up
    bool monotonic = true;
};

Run simulate(Conductor& c, double period, double truthOffset, double rate, double seconds, double warmup) {
    Run r;
    double prev = -1e30;
    for (int i = 0; i * kFrame < seconds; ++i) {
        const double w = 10.0 + i * kFrame;
        const double truth = (w - 10.0) * rate + truthOffset;
        c.observe(quantised(truth, period), w);
        const double t = c.songTime(w);
        if (t < prev) r.monotonic = false;
        prev = t;
        if (i * kFrame > warmup) r.maxErr = std::max(r.maxErr, std::abs(t - truth));
    }
    return r;
}

} // namespace

TEST_CASE("conductor smooths a stepwise cursor") {
    for (double period : {0.010, 0.020, 0.030}) {
        Conductor c;
        const Run r = simulate(c, period, 3.0, 1.0, 8.0, 1.5);
        CHECK(r.monotonic);
        INFO("period " << period << " err " << r.maxErr);
        CHECK(r.maxErr < 0.002);
        CHECK(c.stepEstimate() == doctest::Approx(period).epsilon(0.01));
    }
}

TEST_CASE("conductor follows a slightly fast audio clock") {
    Conductor c;
    const Run r = simulate(c, 0.02, 3.0, 1.0005, 20.0, 1.5);
    CHECK(r.monotonic);
    CHECK(r.maxErr < 0.002);
}

TEST_CASE("conductor freezes while the cursor is stuck, then resumes") {
    Conductor c;
    simulate(c, 0.02, 3.0, 1.0, 3.0, 1.5);
    // Pause: the cursor stays where it was at wall 13.0 (truth 6.0).
    const double stuck = quantised(6.0, 0.02);
    double last = 0.0;
    std::vector<double> held;
    for (int i = 0; i < 90; ++i) {
        const double w = 13.0 + i * kFrame;
        c.observe(stuck, w);
        const double t = c.songTime(w);
        CHECK(t >= last);
        last = t;
        if (w > 13.5) held.push_back(t);
    }
    CHECK(c.paused());
    CHECK(held.back() == held.front());
    CHECK(held.back() <= stuck + 0.1 + 1e-9);
    CHECK(held.back() >= stuck);

    // Resume continuously from the stuck position.
    double prev = held.back();
    for (int i = 0; i < 180; ++i) {
        const double w = 14.5 + i * kFrame;
        const double truth = stuck + i * kFrame;
        c.observe(quantised(truth, 0.02), w);
        const double t = c.songTime(w);
        CHECK(t >= prev);
        prev = t;
        if (i > 90) CHECK(std::abs(t - truth) < 0.003);
    }
    CHECK(!c.paused());
    CHECK(c.seekCount() == 0);
}

TEST_CASE("conductor snaps on seek, backwards too") {
    Conductor c;
    simulate(c, 0.02, 3.0, 1.0, 3.0, 1.5);
    // Seek forward 30 s.
    double w = 13.0;
    c.observe(quantised(36.0, 0.02), w);
    CHECK(c.songTime(w) == doctest::Approx(36.0).epsilon(0.001));
    CHECK(c.seekCount() == 1);
    // Seek back to 2 s.
    w += kFrame;
    c.observe(quantised(2.0, 0.02), w);
    CHECK(c.songTime(w) < 2.1);
    CHECK(c.seekCount() == 2);
    // And it tracks smoothly from there.
    double maxErr = 0.0;
    for (int i = 1; i < 240; ++i) {
        const double ww = w + i * kFrame;
        const double truth = 2.0 + i * kFrame;
        c.observe(quantised(truth, 0.02), ww);
        const double t = c.songTime(ww);
        if (i > 90) maxErr = std::max(maxErr, std::abs(t - truth));
    }
    CHECK(maxErr < 0.002);
}

TEST_CASE("conductor converges after a small clock step without going backwards") {
    Conductor c;
    simulate(c, 0.02, 3.0, 1.0, 3.0, 1.5);
    // The audio clock jumps 40 ms ahead (below the snap threshold): slew, never snap.
    double prev = 0.0, errAt1s = 1.0;
    for (int i = 0; i < 300; ++i) {
        const double w = 13.0 + i * kFrame;
        const double truth = 6.0 + i * kFrame + 0.040;
        c.observe(quantised(truth, 0.02), w);
        const double t = c.songTime(w);
        CHECK(t >= prev);
        prev = t;
        if (i == 120) errAt1s = std::abs(t - truth);
    }
    CHECK(c.seekCount() == 0);
    CHECK(errAt1s < 0.002);
}

TEST_CASE("beat and time conversions") {
    ConductorConfig cfg;
    cfg.bpm = 150.0;
    cfg.offsetSeconds = 0.35;
    Conductor c(cfg);
    CHECK(c.secondsPerBeat() == doctest::Approx(0.4));
    CHECK(c.timeAtBeat(0) == doctest::Approx(0.35));
    CHECK(c.timeAtBeat(10) == doctest::Approx(4.35));
    CHECK(c.beatAt(4.35) == doctest::Approx(10.0));
    CHECK(c.beatAt(0.15) == doctest::Approx(-0.5));
    for (double b : {-2.0, 0.0, 0.25, 7.5, 123.0}) CHECK(c.beatAt(c.timeAtBeat(b)) == doctest::Approx(b));
}

TEST_CASE("event timestamps map to song time with latency compensation") {
    ConductorConfig cfg;
    cfg.latencyOffset = 0.040;
    Conductor c(cfg);
    simulate(c, 0.02, 3.0, 1.0, 4.0, 1.5);
    // An event 5 ms before wall 14.0 happened at truth 7.0 - 0.005; a 40 ms-late player is judged 40 ms earlier.
    const double t = c.songTimeForEvent(14.0 - 0.005);
    CHECK(std::abs(t - (7.0 - 0.005 - 0.040)) < 0.002);
}

TEST_CASE("calibration recovers a known offset from jittery taps") {
    std::vector<double> beats, taps;
    for (int i = 0; i < 32; ++i) beats.push_back(1.0 + i * 0.5);
    unsigned s = 12345;
    auto jitter = [&] {
        s = s * 1664525u + 1013904223u;
        return (static_cast<double>(s >> 8) / static_cast<double>(1u << 24) - 0.5) * 0.030; // +-15 ms
    };
    for (double b : beats) taps.push_back(b + 0.065 + jitter());
    const CalibrationResult r = estimateLatency(taps, beats);
    CHECK(r.samples == 32);
    CHECK(r.reliable);
    CHECK(std::abs(r.offset - 0.065) < 0.003);
    CHECK(r.spread < 0.015);
}

TEST_CASE("calibration rejects outliers") {
    std::vector<double> beats, taps;
    for (int i = 0; i < 24; ++i) beats.push_back(i * 0.5);
    for (int i = 0; i < 24; ++i) taps.push_back(beats[static_cast<std::size_t>(i)] - 0.030 + ((i % 3) - 1) * 0.004);
    // Stray taps between beats (> 1/4 beat away) and a couple of mild early ones.
    taps.push_back(0.25 + 0.001);
    taps.push_back(3.25);
    taps.push_back(7.75);
    taps.push_back(5.0 - 0.080);
    taps.push_back(6.0 - 0.090);
    std::sort(taps.begin(), taps.end());
    const CalibrationResult r = estimateLatency(taps, beats);
    CHECK(r.samples == 26); // three strays dropped
    CHECK(std::abs(r.offset - (-0.030)) < 0.003);
    CHECK(r.reliable);
}

TEST_CASE("calibration is unreliable with few or scattered samples") {
    std::vector<double> beats;
    for (int i = 0; i < 16; ++i) beats.push_back(i * 0.5);
    const std::vector<double> few{0.01, 0.51, 1.01, 1.51, 2.01};
    CalibrationResult r = estimateLatency(few, beats);
    CHECK(r.samples == 5);
    CHECK(!r.reliable);
    CHECK(std::abs(r.offset - 0.01) < 1e-9);

    std::vector<double> scattered;
    for (int i = 0; i < 16; ++i) scattered.push_back(beats[static_cast<std::size_t>(i)] + ((i % 2) ? 0.1 : -0.1));
    r = estimateLatency(scattered, beats);
    CHECK(r.samples == 16);
    CHECK(!r.reliable);

    CHECK(estimateLatency({}, beats).samples == 0);
}

TEST_CASE("conductor keeps tracking when frames outrun its sample ring") {
    // 1000 Hz for 6 s: a 2 s window would need 2000 samples, the ring holds 512.
    Conductor c;
    double prev = -1e30, maxErr = 0.0;
    for (int i = 0; i < 6000; ++i) {
        const double w = 10.0 + i * 0.001;
        const double truth = (w - 10.0) + 3.0;
        c.observe(quantised(truth, 0.01), w);
        const double t = c.songTime(w);
        CHECK(t >= prev);
        prev = t;
        if (i > 1500) maxErr = std::max(maxErr, std::abs(t - truth));
    }
    CHECK(maxErr < 0.002);
    CHECK(c.seekCount() == 0);
}

TEST_CASE("conductor does not allocate while tracking, pausing or seeking") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    Conductor c;
    const std::uint64_t before = ftcore_test::allocStats().count;
    simulate(c, 0.02, 3.0, 1.0, 4.0, 1.5);             // track
    for (int i = 0; i < 30; ++i) c.observe(7.0, 14.0 + i * kFrame); // pause
    c.observe(40.0, 14.6);                              // seek
    (void)c.songTime(14.6);
    (void)c.songTimeForEvent(14.6);
    c.reset();
    CHECK(ftcore_test::allocStats().count == before);
}