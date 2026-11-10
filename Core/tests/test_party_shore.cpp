#include "ftcore/PartyShore.h"
#include "ftcore/PartyTrail.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

using namespace ftcore;

namespace {

// A quay whose edge runs north at x = 10: land to the west, the sea to the east.
bool quay(Vec2 p) { return p.x < 10.f; }

// The quay, a channel 1 m wide, then a pier: land again past x = 11.
bool channel(Vec2 p) { return p.x < 10.f || p.x > 11.f; }

// The halved step's size for a way of `length` metres with the default tuning.
float precision(float length) {
    const PartyShoreTuning t;
    const int steps = std::min(static_cast<int>(std::ceil(length / t.step)), t.maxSteps);
    return length / static_cast<float>(steps) / std::ldexp(1.f, t.halvings);
}

} // namespace

TEST_CASE("party shore: footing is walkable ground found above the sea plus the margin") {
    const double up = 1.0, walk = 0.71; // level ground; about a 44.8 degree limit
    CHECK(shoreFooting(true, 4.0, up, 0.0, 0.1, walk));
    CHECK(shoreFooting(true, 0.11, up, 0.0, 0.1, walk));
    CHECK_FALSE(shoreFooting(true, 0.1, up, 0.0, 0.1, walk)); // at the margin: the sea's
    CHECK_FALSE(shoreFooting(true, 0.0, up, 0.0, 0.1, walk)); // ground at the sea level (a flat under the water)
    CHECK_FALSE(shoreFooting(true, -3.0, up, 0.0, 0.1, walk)); // the bed
    CHECK_FALSE(shoreFooting(false, 4.0, up, 0.0, 0.1, walk)); // nothing found: open water over a deep bed
    CHECK_FALSE(shoreFooting(true, std::numeric_limits<double>::quiet_NaN(), up, 0.0, 0.1, walk));
    CHECK_FALSE(shoreFooting(true, 4.0, std::numeric_limits<double>::quiet_NaN(), 0.0, 0.1, walk));
    // A slope: walkable up to the limit, not past it. The bank from a quay's edge 4 m down to a -3 m bed over a metre is
    // about 82 degrees: dry, but no footing.
    CHECK(shoreFooting(true, 2.0, std::cos(30.0 * 3.14159265358979 / 180.0), 0.0, 0.1, walk));
    CHECK(shoreFooting(true, 2.0, walk, 0.0, 0.1, walk));
    CHECK_FALSE(shoreFooting(true, 2.0, 1.0 / std::hypot(1.0, 7.0), 0.0, 0.1, walk));
    // In centimetres as well, with the margin in centimetres.
    CHECK(shoreFooting(true, 400.0, up, 0.0, 10.0, walk));
    CHECK_FALSE(shoreFooting(true, 5.0, up, 0.0, 10.0, walk));
}

TEST_CASE("party shore: a spot over the sea stands a body's width back from the edge, on the way from the anchor") {
    const PartyShoreTuning t;
    const ShoreStand s = standShortOfSea({8.f, 0.f}, {11.f, 0.f}, quay, t);
    CHECK(s.dry);
    CHECK(s.held);
    CHECK(s.at.y == doctest::Approx(0.f));
    CHECK(s.at.x <= 10.f - t.inset + 1e-4f);
    CHECK(s.at.x >= 10.f - t.inset - precision(3.f) - 1e-4f);
    CHECK(quay(s.at));
}

TEST_CASE("party shore: beside a lead walking along the edge it walks the shore with it") {
    // The lead walks north 0.7 m from the edge, its follower's slot 1 m to its right (out over the water).
    float lastY = -1.f;
    for (int i = 0; i <= 40; ++i) {
        const float y = 0.25f * static_cast<float>(i);
        const ShoreStand s = standShortOfSea({9.3f, y}, {10.3f, y}, quay);
        CHECK(s.held);
        CHECK(quay(s.at));
        CHECK(s.at.y == doctest::Approx(y)); // level with the lead: not held back where it first met the water
        CHECK(s.at.x == doctest::Approx(9.7f).epsilon(0.002));
        CHECK(s.at.y > lastY);
        lastY = s.at.y;
    }
}

TEST_CASE("party shore: it never crosses a stretch of water onto land beyond it") {
    // The spot is on the pier past the channel: dry, but the way there goes over water.
    const ShoreStand s = standShortOfSea({9.f, 0.f}, {12.f, 0.f}, channel);
    CHECK(s.held);
    CHECK(s.at.x < 10.f);
    CHECK(s.at.x == doctest::Approx(10.f - PartyShoreTuning{}.inset).epsilon(0.005));
}

TEST_CASE("party shore: a way with footing all along gives the spot itself") {
    const PartyShoreTuning t;
    const ShoreStand s = standShortOfSea({2.f, 0.f}, {4.f, 1.f}, quay, t);
    CHECK(s.dry);
    CHECK_FALSE(s.held);
    CHECK(s.at.x == 4.f);
    CHECK(s.at.y == 1.f);
    const int steps = static_cast<int>(std::ceil(std::hypot(2.f, 1.f) / t.step));
    CHECK(s.looks == 1 + steps);
}

TEST_CASE("party shore: an anchor with no footing gives the anchor, not dry") {
    const ShoreStand s = standShortOfSea({12.f, 0.f}, {13.f, 0.f}, quay);
    CHECK_FALSE(s.dry);
    CHECK_FALSE(s.held);
    CHECK(s.at.x == 12.f);
    CHECK(s.looks == 1);
}

TEST_CASE("party shore: the inset never takes it back past the anchor") {
    const ShoreStand s = standShortOfSea({9.9f, 0.f}, {11.f, 0.f}, quay);
    CHECK(s.held);
    CHECK(s.at.x == doctest::Approx(9.9f));
    CHECK(s.at.y == doctest::Approx(0.f));
    PartyShoreTuning none;
    none.inset = 0.f;
    const ShoreStand edge = standShortOfSea({9.9f, 0.f}, {11.f, 0.f}, quay, none);
    CHECK(edge.at.x < 10.f);
    CHECK(edge.at.x > 9.98f);
}

TEST_CASE("party shore: a spot on the anchor, or not a number, stays on the anchor") {
    const ShoreStand on = standShortOfSea({5.f, 5.f}, {5.f, 5.f}, quay);
    CHECK(on.dry);
    CHECK_FALSE(on.held);
    CHECK(on.at.x == 5.f);
    CHECK(on.looks == 1);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const ShoreStand bad = standShortOfSea({5.f, 5.f}, {nan, 5.f}, quay);
    CHECK(bad.at.x == 5.f);
    CHECK(bad.at.y == 5.f);
}

TEST_CASE("party shore: the looks are bounded on a long way") {
    const PartyShoreTuning t;
    int calls = 0;
    const ShoreStand s = standShortOfSea({-90.f, 0.f}, {100.f, 0.f}, [&](Vec2 p) {
        ++calls;
        return quay(p);
    }, t);
    CHECK(s.held);
    CHECK(quay(s.at));
    CHECK(calls == s.looks);
    CHECK(s.looks <= 1 + t.maxSteps + t.halvings);
    // A step of 0.25 m would take 760 looks; the capped steps are coarser, so the edge is found to within a halved one.
    CHECK(s.at.x > 10.f - t.inset - 190.f / static_cast<float>(t.maxSteps) / std::ldexp(1.f, t.halvings) - 1e-3f);
}

TEST_CASE("party shore: a slot beside the trail at a quay edge, held on the land") {
    // The lead walks north along a quay 0.5 m from its edge, the sea on the side its follower's slot is on (west of
    // x = 9: a slot a metre to the side of the path is out over the water). Each frame it stands short of the sea on the
    // way from the path point at its arc to its slot.
    auto quayWest = [](Vec2 p) { return p.x > 9.f; };
    PartyTrail trail;
    trail.reset({9.5f, 0.f}, {0.f, 1.f}, 3.2f);
    PartySlot slot;
    slot.reset(trail);
    int held = 0;
    for (int i = 1; i <= 120; ++i) {
        trail.push({9.5f, 0.05f * static_cast<float>(i)}, 3.f);
        slot.step(trail, 3.f, 1.f / 60.f);
        const Vec2 path = trail.pointBehind(slot.arc());
        const ShoreStand s = standShortOfSea(path, slot.position(), quayWest);
        CHECK(s.dry);
        CHECK(quayWest(s.at));
        if (!quayWest(slot.position())) {
            ++held;
            CHECK(s.held);
            CHECK(s.at.x <= path.x + 1e-4f); // out toward its side, not behind the path
            CHECK(s.at.x == doctest::Approx(9.f + PartyShoreTuning{}.inset).epsilon(0.01));
        }
    }
    CHECK(held > 60); // the slot is over the water most of the way: the case is exercised
}

TEST_CASE("party shore allocates nothing") {
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    float sum = 0.f;
    for (int i = 0; i < 100; ++i) {
        const ShoreStand s = standShortOfSea({8.f, static_cast<float>(i)}, {12.f, static_cast<float>(i)}, quay);
        sum += s.at.x;
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(sum > 0.f);
    if (counting) CHECK(after == before);
}
