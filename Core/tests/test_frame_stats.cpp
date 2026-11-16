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

TEST_CASE("frame stats: p99 and the counts over 33 and 50 ms") {
    ftcore::FrameStats stats(200);
    for (int i = 0; i < 200; ++i) {
        ftcore::FrameSample s;
        s.frameMs = 10.f;
        if (i == 20) s.frameMs = 33.f;  // at the line: not over
        if (i == 40) s.frameMs = 40.f;
        if (i == 60) s.frameMs = 50.f;  // over 33, not over 50
        if (i == 80) s.frameMs = 70.f;
        CHECK(stats.add(s));
    }
    const ftcore::FrameSummary sum = stats.summary();
    CHECK(sum.over33 == 3);
    CHECK(sum.over50 == 1);
    CHECK(sum.frame.p99 == doctest::Approx(40.f)); // rank 198 of 200 in the sorted order
    CHECK(sum.frame.max == doctest::Approx(70.f));
}

TEST_CASE("frame stats: the hitch list keeps order and index, counts past its cap") {
    ftcore::FrameStats stats(10);
    const float ms[10] = {10, 60, 10, 35, 10, 10, 80, 10, 10, 34};
    for (float m : ms) {
        ftcore::FrameSample s;
        s.frameMs = m;
        s.gameMs = m / 2.f;
        stats.add(s);
    }
    ftcore::FrameHitch out[2];
    CHECK(stats.hitches(ftcore::kHitchMs, out, 2) == 4);
    CHECK(out[0].index == 1);
    CHECK(out[0].sample.gameMs == doctest::Approx(30.f));
    CHECK(out[1].index == 3);
    CHECK(stats.hitches(ftcore::kSevereHitchMs, nullptr, 0) == 2);
    CHECK(stats.hitches(100.f, out, 2) == 0);
}

TEST_CASE("frame stats: one sample is every percentile") {
    ftcore::FrameStats stats(1);
    ftcore::FrameSample s;
    s.frameMs = 40.f;
    REQUIRE(stats.add(s));
    const ftcore::FrameSummary sum = stats.summary();
    CHECK(sum.count == 1);
    CHECK(sum.frame.median == doctest::Approx(40.f));
    CHECK(sum.frame.p95 == doctest::Approx(40.f));
    CHECK(sum.frame.p99 == doctest::Approx(40.f));
    CHECK(sum.frame.max == doctest::Approx(40.f));
    CHECK(sum.over33 == 1);
    CHECK(sum.over50 == 0);
}