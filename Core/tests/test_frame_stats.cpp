#include <doctest/doctest.h>
#include "ftcore/FrameStats.h"

TEST_CASE("frame stats: median and p95 over the kept samples, full buffer refuses more") {
    ftcore::FrameStats stats(100);
    for (int i = 1; i <= 100; ++i) {
        ftcore::FrameSample s;
        s.frameMs = static_cast<float>(101 - i); // descending, so the sort matters
        s.gpuMs = 2.f;
        CHECK(stats.add(s));
    }
    CHECK_FALSE(stats.add(ftcore::FrameSample{}));
    const ftcore::FrameSummary sum = stats.summary();
    CHECK(sum.count == 100);
    CHECK(sum.frame.median == doctest::Approx(50.f));
    CHECK(sum.frame.p95 == doctest::Approx(95.f));
    CHECK(sum.frame.max == doctest::Approx(100.f));
    CHECK(sum.gpu.median == doctest::Approx(2.f));
}

TEST_CASE("frame stats: no samples gives zeros") {
    ftcore::FrameStats stats(4);
    const ftcore::FrameSummary sum = stats.summary();
    CHECK(sum.count == 0);
    CHECK(sum.frame.median == 0.f);
}
