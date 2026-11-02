#include "ftcore/StickFlick.h"
#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <utility>

using namespace ftcore;

namespace {
// Rests the stick for a frame so it is armed.
void rest(StickFlick& f) { CHECK_FALSE(f.update(0.f, 0.f).has_value()); }
} // namespace

TEST_CASE("a flick fires on crossing 0.6 along the dominant axis") {
    StickFlick f;
    rest(f);
    CHECK_FALSE(f.update(-0.4f, 0.f).has_value());
    CHECK_FALSE(f.update(-0.59f, 0.f).has_value());
    const auto a = f.update(-0.61f, 0.f);
    REQUIRE(a.has_value());
    CHECK(*a == Arrow::Left);
    // Held over: nothing more.
    CHECK_FALSE(f.update(-1.f, 0.f).has_value());
    CHECK_FALSE(f.update(-0.8f, 0.f).has_value());
}

TEST_CASE("all four directions; y is up-positive; a tie goes to the horizontal") {
    const std::pair<Arrow, std::pair<float, float>> cases[] = {{Arrow::Left, {-1.f, 0.f}},
                                                              {Arrow::Right, {1.f, 0.f}},
                                                              {Arrow::Up, {0.f, 1.f}},
                                                              {Arrow::Down, {0.f, -1.f}}};
    for (const auto& c : cases) {
        StickFlick f;
        rest(f);
        const auto a = f.update(c.second.first, c.second.second);
        REQUIRE(a.has_value());
        CHECK(*a == c.first);
    }
    StickFlick f;
    rest(f);
    CHECK(*f.update(0.7f, 0.7f) == Arrow::Right);
    StickFlick g;
    rest(g);
    CHECK(*g.update(0.5f, -0.7f) == Arrow::Down);
}

TEST_CASE("it re-arms under 0.3, not before") {
    StickFlick f;
    rest(f);
    CHECK(f.update(0.9f, 0.f).has_value());
    CHECK_FALSE(f.update(0.4f, 0.f).has_value()); // not at rest yet
    CHECK_FALSE(f.update(0.9f, 0.f).has_value()); // a second push without a rest is nothing
    CHECK_FALSE(f.update(0.29f, 0.f).has_value());
    const auto again = f.update(0.9f, 0.f);
    REQUIRE(again.has_value());
    CHECK(*again == Arrow::Right);
}

TEST_CASE("a flick straight to the opposite direction needs no rest") {
    StickFlick f;
    rest(f);
    CHECK(*f.update(-0.9f, 0.f) == Arrow::Left);
    CHECK(*f.update(0.9f, 0.f) == Arrow::Right);
    CHECK(*f.update(-0.9f, 0.f) == Arrow::Left);
    // Held over after the swing: nothing more.
    CHECK_FALSE(f.update(-0.95f, 0.f).has_value());
    // Up and down too.
    StickFlick g;
    rest(g);
    CHECK(*g.update(0.f, 0.9f) == Arrow::Up);
    CHECK(*g.update(0.f, -0.9f) == Arrow::Down);
    // A perpendicular swing is not an opposite: it waits for the rest.
    StickFlick h;
    rest(h);
    CHECK(*h.update(0.9f, 0.f) == Arrow::Right);
    CHECK_FALSE(h.update(0.f, 0.9f).has_value());
    rest(h);
    CHECK(*h.update(0.f, 0.9f) == Arrow::Up);
}

TEST_CASE("the stick must come to rest before the first flick") {
    StickFlick f; // a fresh detector: a stick already held over fires nothing
    CHECK_FALSE(f.update(0.f, 1.f).has_value());
    CHECK_FALSE(f.update(0.f, 1.f).has_value());
    CHECK_FALSE(f.update(0.f, 0.4f).has_value());
    rest(f);
    CHECK(*f.update(0.f, 1.f) == Arrow::Up);
    // reset() asks for the rest again.
    f.reset();
    CHECK_FALSE(f.update(0.f, 1.f).has_value());
    rest(f);
    CHECK(*f.update(0.f, 1.f) == Arrow::Up);
}

TEST_CASE("the update allocates nothing") {
    StickFlick f;
    rest(f);
    const auto before = ftcore_test::allocStats();
    int fired = 0;
    for (int i = 0; i < 2000; ++i) {
        const float x = (i % 7 < 3 ? 1.f : -1.f) * static_cast<float>(i % 5) * 0.25f;
        const float y = static_cast<float>((i * 37) % 100) * 0.01f - 0.5f;
        fired += f.update(x, y).has_value() ? 1 : 0;
    }
    CHECK(fired > 0);
    if (ftcore_test::allocHookInstalled()) CHECK(ftcore_test::allocStats().count == before.count);
}
