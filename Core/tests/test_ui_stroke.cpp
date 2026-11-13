#include "ftcore/UiStroke.h"

#include "AllocCounter.h"

#include <ostream>
#include <doctest/doctest.h>

#include <cmath>

using namespace ftcore;

TEST_CASE("ui stroke: an arc is deterministic, round and stays within the hand's wobble")
{
    UiStrokeStyle st;
    st.width = 4.f;
    st.wobble = 0.8f;
    st.seed = 21;
    UiStroke a;
    UiStroke b;
    uiStrokeArc(a, 190.f, 880.f, 70.f, -135.f, 135.f, st);
    uiStrokeArc(b, 190.f, 880.f, 70.f, -135.f, 135.f, st);
    REQUIRE(a.count > 2u);
    REQUIRE(a.count == b.count);
    for (std::uint32_t i = 0; i < a.count; ++i) {
        CHECK(a.pt[i].x == b.pt[i].x);
        CHECK(a.pt[i].y == b.pt[i].y);
        const float r = std::hypot(a.pt[i].x - 190.f, a.pt[i].y - 880.f);
        CHECK(std::fabs(r - 70.f) <= 0.8f + 1e-3f);
    }
    // 0 = up, clockwise: -135 starts lower left, 135 ends lower right
    CHECK(a.pt[0].x < 190.f);
    CHECK(a.pt[0].y > 880.f);
    CHECK(a.pt[a.count - 1].x > 190.f);
    CHECK(a.pt[a.count - 1].y > 880.f);

    st.seed = 22;
    uiStrokeArc(b, 190.f, 880.f, 70.f, -135.f, 135.f, st);
    bool differs = false;
    for (std::uint32_t i = 0; i < a.count; ++i) {
        differs = differs || a.pt[i].x != b.pt[i].x;
    }
    CHECK(differs);
}

TEST_CASE("ui stroke: the ends thin and fade, the middle is full")
{
    UiStrokeStyle st;
    st.width = 4.f;
    st.endWidth = 0.35f;
    st.endAlpha = 0.55f;
    UiStroke s;
    uiStrokeLine(s, 0.f, 0.f, 400.f, 0.f, st);
    REQUIRE(s.count > 10u);
    CHECK(s.pt[0].halfWidth == doctest::Approx(2.f * 0.35f));
    CHECK(s.pt[0].alpha == doctest::Approx(0.55f));
    const UiStrokePoint& mid = s.pt[s.count / 2];
    CHECK(mid.halfWidth == doctest::Approx(2.f));
    CHECK(mid.alpha == doctest::Approx(1.f));
    CHECK(s.pt[s.count - 1].x == doctest::Approx(400.f));
}

TEST_CASE("ui stroke: nothing for a zero sweep, never more than the cap, broken drops runs")
{
    UiStrokeStyle st;
    UiStroke s;
    uiStrokeArc(s, 0.f, 0.f, 70.f, 30.f, 30.f, st);
    CHECK(s.count == 0u);
    uiStrokeLine(s, 5.f, 5.f, 5.f, 5.f, st);
    CHECK(s.count == 0u);

    uiStrokeLine(s, 0.f, 0.f, 5000.f, 0.f, st); // far longer than cap * step
    CHECK(s.count <= kUiStrokeMaxPoints);
    CHECK(s.count > kUiStrokeMaxPoints / 2);
    CHECK(s.pt[s.count - 1].x == doctest::Approx(5000.f));

    st.broken = true;
    uiStrokeArc(s, 0.f, 0.f, 70.f, -135.f, 135.f, st);
    std::uint32_t gone = 0;
    for (std::uint32_t i = 0; i < s.count; ++i) {
        gone += s.pt[i].alpha == 0.f ? 1u : 0u;
    }
    CHECK(gone > 0u);
    CHECK(gone < s.count);
}

TEST_CASE("ui stroke: a meter's line does not crawl as it shrinks, and its gaps stay on the track")
{
    UiStrokeStyle st;
    st.seed = 7;
    st.broken = true;
    UiStroke full;
    UiStroke less;
    uiStrokeArc(full, 190.f, 880.f, 70.f, -135.f, 135.f, st, 0.f, 0.6f);
    uiStrokeArc(less, 190.f, 880.f, 70.f, -135.f, 135.f, st, 0.f, 0.4f);
    REQUIRE(less.count > 4u);
    REQUIRE(full.count > less.count);
    for (std::uint32_t i = 0; i + 1 < less.count; ++i) { // the last point is the drawn end, not a step
        CHECK(less.pt[i].x == doctest::Approx(full.pt[i].x));
        CHECK(less.pt[i].y == doctest::Approx(full.pt[i].y));
        CHECK((less.pt[i].alpha == 0.f) == (full.pt[i].alpha == 0.f));
    }
    // a part that starts later on the track: the same place, the same point
    UiStroke late;
    uiStrokeLine(full, 0.f, 0.f, 400.f, 0.f, st);
    uiStrokeLine(late, 0.f, 0.f, 400.f, 0.f, st, 0.5f, 1.f);
    REQUIRE(late.count > 2u);
    CHECK(late.pt[0].x == doctest::Approx(200.f).epsilon(0.01));
    CHECK(late.pt[1].x == doctest::Approx(full.pt[late.pt[1].x > 0.f ? static_cast<std::uint32_t>(late.pt[1].x / 4.f + 0.5f) : 0u].x));
}

TEST_CASE("ui stroke: no heap allocation")
{
    UiStroke s;
    UiStrokeStyle st;
    st.broken = true;
    const auto before = ftcore_test::allocStats().count;
    for (int i = 0; i < 50; ++i) {
        uiStrokeArc(s, 190.f, 880.f, 84.f, -135.f, static_cast<float>(i * 5 - 135), st);
        uiStrokeLine(s, 0.f, 0.f, 680.f, 0.f, st);
    }
    CHECK(ftcore_test::allocStats().count == before);
}
