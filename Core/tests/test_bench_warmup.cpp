#include <doctest/doctest.h>
#include "ftcore/BenchWarmup.h"

TEST_CASE("bench warm-up: slow compile frames use up the seconds but do not end it") {
    ftcore::BenchWarmup w; // 5 s, 60 frames under 100 ms
    CHECK_FALSE(w.step(3000.0));
    CHECK_FALSE(w.step(2500.0)); // 5.5 s passed, 0 smooth frames
    for (int i = 0; i < 59; ++i) {
        CHECK_FALSE(w.step(16.0));
    }
    CHECK(w.step(16.0)); // the 60th smooth frame
    CHECK(w.smoothFrames() == 60);
    CHECK_FALSE(w.timedOut());
    CHECK(w.step(5000.0)); // stays over
}

TEST_CASE("bench warm-up: smooth frames alone wait for the seconds; a frame at the limit is not smooth") {
    ftcore::BenchWarmup w;
    for (int i = 0; i < 300; ++i) {
        CHECK_FALSE(w.step(10.0)); // 3 s
    }
    CHECK_FALSE(w.step(100.0)); // not under 100 ms
    CHECK(w.smoothFrames() == 300);
    for (int i = 0; i < 189; ++i) {
        CHECK_FALSE(w.step(10.0));
    }
    CHECK(w.step(10.0)); // 5.0 s
}

TEST_CASE("bench warm-up: a scene that never settles ends at maxSeconds, marked timed out") {
    ftcore::BenchWarmup w;
    w.maxSeconds = 10.0;
    for (int i = 0; i < 4; ++i) {
        CHECK_FALSE(w.step(2000.0));
    }
    CHECK(w.step(2000.0));
    CHECK(w.timedOut());
}
