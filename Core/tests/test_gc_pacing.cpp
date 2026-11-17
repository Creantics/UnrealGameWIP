#include <doctest/doctest.h>
#include "ftcore/GcPacing.h"

TEST_CASE("gc pacing: a hidden moment collects once the gap has passed, not twice in a row") {
    ftcore::GcPacing p; // 30 s gap, clock starts at 0
    CHECK_FALSE(p.hiddenMoment(10.0)); // too soon after the start (the load collected)
    CHECK(p.hiddenMoment(31.0));
    CHECK_FALSE(p.hiddenMoment(33.0)); // menu closed and opened again
    CHECK(p.hiddenMoment(61.0));
}

TEST_CASE("gc pacing: the engine's own collection restarts the gap") {
    ftcore::GcPacing p;
    p.collected(100.0);
    CHECK_FALSE(p.hiddenMoment(120.0));
    CHECK(p.hiddenMoment(130.0));
    CHECK(p.last() == doctest::Approx(130.0));
}
