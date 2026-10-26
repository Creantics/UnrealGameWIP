#include "ftcore/PartyTrail.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace ftcore;

namespace {

float dist(Vec2 a, Vec2 b) { return std::hypot(b.x - a.x, b.y - a.y); }

// Walks the lead from `from` to `to` in steps of `step` metres, pushing each position.
void walk(PartyTrail& t, Vec2 from, Vec2 to, float step) {
    const float d = dist(from, to);
    const int n = static_cast<int>(std::ceil(d / step));
    for (int i = 1; i <= n; ++i) {
        const float f = static_cast<float>(i) / static_cast<float>(n);
        t.push({from.x + (to.x - from.x) * f, from.y + (to.y - from.y) * f});
    }
}

} // namespace

TEST_CASE("party trail: reset seeds a straight line behind the lead's facing") {
    PartyTrail t;
    t.reset({2.f, 3.f}, {0.f, 2.f}, 3.2f);
    CHECK(t.length() == doctest::Approx(3.25f)); // rounded up to whole samples
    const Vec2 p = t.pointBehind(1.6f);
    CHECK(p.x == doctest::Approx(2.f));
    CHECK(p.y == doctest::Approx(1.4f));
    CHECK(t.pointBehind(0.f).y == doctest::Approx(3.f));

    t.reset({0.f, 0.f}, {}, 1.f); // no facing: behind along -x
    CHECK(t.pointBehind(1.f).x == doctest::Approx(-1.f));
}

TEST_CASE("party trail: a straight walk keeps the followers at exact spacing") {
    PartyTrail t;
    t.reset({0.f, 0.f}, {1.f, 0.f}, 2.f * PartyTrail::kFollowerSpacing);
    walk(t, {0.f, 0.f}, {10.f, 0.f}, 0.07f);
    for (int i = 1; i <= 2; ++i) {
        const Vec2 p = t.pointBehind(static_cast<float>(i) * PartyTrail::kFollowerSpacing);
        CHECK(p.x == doctest::Approx(10.f - static_cast<float>(i) * PartyTrail::kFollowerSpacing).epsilon(0.001));
        CHECK(p.y == doctest::Approx(0.f));
    }
}

TEST_CASE("party trail: after a turn the follower stays on the path, not on the chord") {
    for (const float step : {0.05f, 0.4f, 1.3f}) { // small frames and long, fast-forwarded ones
        PartyTrail t;
        t.reset({0.f, 0.f}, {1.f, 0.f}, 0.f);
        walk(t, {0.f, 0.f}, {5.f, 0.f}, step);
        walk(t, {5.f, 0.f}, {5.f, 3.f}, step);
        const Vec2 p = t.pointBehind(4.f); // 3 m back to the corner, 1 m more along the first leg
        CHECK(dist(p, {4.f, 0.f}) < PartyTrail::kSampleSpacing);
        CHECK(dist(t.pointBehind(1.f), {5.f, 2.f}) < 0.05f);
    }
}

TEST_CASE("party trail: a short trail answers with its oldest point") {
    PartyTrail t;
    t.reset({0.f, 0.f}, {1.f, 0.f}, 0.f);
    t.push({0.6f, 0.f});
    CHECK(t.length() == doctest::Approx(0.6f));
    const Vec2 p = t.pointBehind(3.f);
    CHECK(p.x == doctest::Approx(0.f));
    CHECK(p.y == doctest::Approx(0.f));
    t.push({0.6f, 0.f}); // standing still changes nothing
    CHECK(t.pointBehind(0.5f).x == doctest::Approx(0.1f));
}

TEST_CASE("party trail: the ring keeps the newest samples when it overflows") {
    PartyTrail t;
    t.reset({0.f, 0.f}, {1.f, 0.f}, 0.f);
    walk(t, {0.f, 0.f}, {40.f, 0.f}, 0.1f); // 160 samples into 64
    CHECK(t.samples() == PartyTrail::kCapacity);
    const float kept = static_cast<float>(PartyTrail::kCapacity - 1u) * PartyTrail::kSampleSpacing;
    CHECK(t.length() <= kept + PartyTrail::kSampleSpacing);
    CHECK(t.length() >= kept);
    CHECK(t.pointBehind(3.2f).x == doctest::Approx(36.8f).epsilon(0.001));
    CHECK(t.pointBehind(1000.f).x == doctest::Approx(40.f - t.length()).epsilon(0.001));
}

TEST_CASE("party trail: a jump longer than the trail starts it over, facing the jump") {
    PartyTrail t;
    t.reset({0.f, 0.f}, {1.f, 0.f}, 2.f);
    t.push({0.f, 50.f});
    CHECK(t.lead().y == doctest::Approx(50.f));
    CHECK(t.length() == doctest::Approx(0.f));
    t.push({0.f, 51.f});
    CHECK(t.pointBehind(1.f).y == doctest::Approx(50.f));

    PartyTrail fresh; // never reset: the first push starts it
    fresh.push({3.f, 4.f});
    CHECK(fresh.pointBehind(2.f).x == doctest::Approx(3.f));
}

TEST_CASE("party trail allocates nothing") {
    PartyTrail t;
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    t.reset({0.f, 0.f}, {1.f, 0.f}, 3.2f);
    float sum = 0.f;
    for (int i = 0; i < 2000; ++i) {
        const float a = static_cast<float>(i) * 0.01f;
        t.push({a * 3.f, std::sin(a) * 4.f});
        sum += t.pointBehind(1.6f).x + t.pointBehind(3.2f).y + t.length();
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(sum != 0.f);
    if (counting) CHECK(after == before);
}
