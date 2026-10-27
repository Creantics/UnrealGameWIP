#include "ftcore/Traversal.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kFps = 30.f;
constexpr float kLength = 1.5f;

// A made-up clip at 30 fps: the root runs forward at `speed`; the left foot rests until 0.2 s, the
// right from 0.2 to 0.4 (so it takes off from the right); the right hand rests on a ledge `ahead` cm
// in front of the start and `height` cm up from 0.5 to 0.75 s; both feet land at 1.0 s, on the floor
// beyond (over) or on top (up), and rest there to the end.
std::vector<TraversalFrame> makeClip(bool up, float ahead = 100.f, float height = 100.f, float speed = 200.f) {
    std::vector<TraversalFrame> frames;
    const int n = static_cast<int>(kLength * kFps) + 1;
    const float landZ = up ? height : 0.f;
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / kFps;
        TraversalFrame f;
        f.root = {speed * t, 0.f, 0.f}; // the root bone on the floor, as in the mannequin's clips
        auto foot = [&](float restFrom, float restTo, float side) {
            if (t <= restTo && t >= restFrom) return TravPoint{speed * restFrom, side, 0.f};
            if (t >= 1.f) return TravPoint{ahead + 60.f, side, landZ};
            return TravPoint{speed * t + 20.f, side, 40.f + 30.f * t}; // in the air, moving
        };
        f.ballLeft = foot(-1.f, 0.2f, 10.f);
        f.ballRight = foot(0.2f, 0.4f, -10.f);
        if (t < 0.5f) f.handRight = {ahead - 200.f * (0.5f - t), -20.f, height - 100.f * (0.5f - t)};
        else if (t <= 0.75f) f.handRight = {ahead, -20.f, height};
        else f.handRight = {ahead + 200.f * (t - 0.75f), -20.f, height + 100.f * (t - 0.75f)};
        f.handLeft = {speed * t, 20.f, 100.f + 60.f * t}; // swinging, never at rest
        frames.push_back(f);
    }
    return frames;
}

TraversalClipInfo measure(const std::vector<TraversalFrame>& f) { return measureTraversal(f.data(), f.size(), kLength); }

TraversalObstacle wall(float distance, float height = 100.f) {
    TraversalObstacle o;
    o.distance = distance;
    o.height = height;
    o.depth = 25.f;
    o.backFloor = true;
    return o;
}

TraversalObstacle ledge(float distance, float height = 100.f) {
    TraversalObstacle o;
    o.distance = distance;
    o.height = height;
    o.depth = 200.f;
    return o;
}

} // namespace

TEST_CASE("measureTraversal finds the contact, the landing, the ledge and the take-off foot") {
    const TraversalClipInfo over = measure(makeClip(false));
    CHECK(over.length == doctest::Approx(kLength));
    CHECK(over.contact == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(over.land == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(over.contactHand == 1);
    CHECK(over.ledgeAhead == doctest::Approx(100.f));
    CHECK(over.ledgeHeight == doctest::Approx(100.f));
    CHECK(over.rise == doctest::Approx(0.f));
    CHECK(over.move == TraversalMove::Over);
    CHECK(over.takeoffFoot == 1);
    CHECK(over.entrySpeed == doctest::Approx(200.f));
    CHECK(over.path[kTraversalSamples - 1].x == doctest::Approx(300.f));

    const TraversalClipInfo up = measure(makeClip(true));
    CHECK(up.move == TraversalMove::Up);
    CHECK(up.rise == doctest::Approx(100.f));
}

TEST_CASE("measureTraversal leaves a clip with no hand on a ledge unmeasured") {
    std::vector<TraversalFrame> f = makeClip(false);
    for (TraversalFrame& x : f) x.handRight = x.handLeft;
    CHECK(measure(f).length == 0.f);
    CHECK(measureTraversal(nullptr, 0, 1.f).length == 0.f);
}

TEST_CASE("plan goes over a thin wall and climbs onto a deep ledge") {
    TraversalSet set;
    set.setClip(0, measure(makeClip(false)));
    set.setClip(1, measure(makeClip(true)));
    CHECK(set.plan(wall(100.f), 200.f, 1).clip == 0);
    CHECK(set.plan(ledge(100.f), 200.f, 1).clip == 1);
    // Thin with nothing beyond, or a drop too deep: no plan.
    TraversalObstacle o = wall(100.f);
    o.backFloor = false;
    CHECK_FALSE(set.plan(o, 200.f, 1).ok());
    o = wall(100.f);
    o.backHeight = -400.f;
    CHECK_FALSE(set.plan(o, 200.f, 1).ok());
    // Too high or too low for the clips.
    CHECK_FALSE(set.plan(wall(100.f, 180.f), 200.f, 1).ok());
    CHECK_FALSE(set.plan(wall(100.f, 40.f), 200.f, 1).ok());
}

TEST_CASE("plan takes the nearest height, then entry speed, then the take-off foot") {
    TraversalSet set;
    set.setClip(0, measure(makeClip(true, 100.f, 100.f, 300.f)));
    set.setClip(1, measure(makeClip(true, 100.f, 100.f, 100.f)));
    set.setClip(2, measure(makeClip(true, 100.f, 240.f, 100.f)));
    CHECK(set.plan(ledge(90.f), 280.f, -1).clip == 0);
    CHECK(set.plan(ledge(90.f), 120.f, -1).clip == 1);
    CHECK(set.plan(ledge(90.f, 230.f), 280.f, -1).clip == 2);

    // Two clips alike but for the foot: the one taking off from the body's foot.
    TraversalSet feet;
    std::vector<TraversalFrame> left = makeClip(true);
    for (TraversalFrame& f : left) std::swap(f.ballLeft, f.ballRight);
    feet.setClip(0, measure(makeClip(true)));
    feet.setClip(1, measure(left));
    CHECK(feet.clip(1).takeoffFoot == 0);
    CHECK(feet.plan(ledge(100.f), 200.f, 1).clip == 0);
    CHECK(feet.plan(ledge(100.f), 200.f, 0).clip == 1);
}

TEST_CASE("plan enters the clip where its ledge is as far as the real one") {
    TraversalSet set;
    set.setClip(0, measure(makeClip(false)));
    // At the clip's own distance it starts from the top, with nothing to make up.
    TraversalPlan p = set.plan(wall(100.f), 200.f, 1);
    CHECK(p.start == doctest::Approx(0.f));
    CHECK(p.atContact.x == doctest::Approx(0.f).epsilon(0.01));
    // Nearer: later in, still nothing to make up (the root runs at 200 cm/s: 40 cm is 0.2 s).
    p = set.plan(wall(60.f), 200.f, 1);
    CHECK(p.start == doctest::Approx(0.2f).epsilon(0.01));
    CHECK(std::fabs(p.atContact.x) < 0.5f);
    // Farther than the clip reaches, within maxPush: the offset carries the rest.
    p = set.plan(wall(130.f), 200.f, 1);
    CHECK(p.start == 0.f);
    CHECK(p.atContact.x == doctest::Approx(30.f));
    // Farther still: not yet.
    p = set.plan(wall(200.f), 200.f, 1);
    CHECK_FALSE(p.ok());
    CHECK(p.tooFar);
    // So near that even the latest entry (0.15 s before the contact: 30 cm off) pulls back too far.
    CHECK_FALSE(set.plan(wall(-20.f), 200.f, 1).ok());
    CHECK_FALSE(set.plan(wall(-20.f), 200.f, 1).tooFar);
}

TEST_CASE("rootAt puts the hands on the real ledge and the feet on the real floor") {
    TraversalSet set;
    const std::vector<TraversalFrame> frames = makeClip(false);
    set.setClip(0, measure(frames));
    const TraversalClipInfo& c = set.clip(0);

    TraversalObstacle o = wall(70.f, 120.f);
    o.depth = 45.f;
    o.backHeight = -30.f;
    const TraversalPlan p = set.plan(o, 200.f, 1);
    REQUIRE(p.ok());
    CHECK(p.atContact.z == doctest::Approx(20.f));
    CHECK(p.atLand.x == doctest::Approx(p.atContact.x + 15.f));
    CHECK(p.atLand.z == doctest::Approx(-30.f));

    // At the entry the root is where the body is.
    const TravPoint at0 = set.rootAt(p, p.start);
    CHECK(std::fabs(at0.x) + std::fabs(at0.z) < 0.01f);
    // At the contact the clip's hand, carried with the root, is on the real ledge.
    const std::size_t k = static_cast<std::size_t>(std::lround(c.contact * kFps));
    const TravPoint root = set.rootAt(p, c.contact);
    const TravPoint hand{frames[k].handRight.x - frames[k].root.x + root.x, 0.f, frames[k].handRight.z - frames[k].root.z + root.z};
    CHECK(hand.x == doctest::Approx(70.f).epsilon(0.01));
    CHECK(hand.z == doctest::Approx(120.f).epsilon(0.01));
    // After the landing it holds the landing's offset.
    const TravPoint end = set.rootAt(p, kLength);
    const TravPoint clipEnd = traversalPath(c, kLength);
    const TravPoint from = traversalPath(c, p.start);
    CHECK(end.x == doctest::Approx(clipEnd.x - from.x + p.atLand.x));
    CHECK(end.z == doctest::Approx(-30.f));
    // It eases: halfway to the contact the offset is halfway.
    const float mid = 0.5f * (p.start + c.contact);
    CHECK(set.rootAt(p, mid).z - (traversalPath(c, mid).z - from.z) == doctest::Approx(10.f));
}

TEST_CASE("Traversal plans and paths allocate nothing") {
    TraversalSet set;
    set.setClip(0, measure(makeClip(false)));
    set.setClip(1, measure(makeClip(true)));
    const bool hooked = ftcore_test::allocHookInstalled();
    const std::uint64_t before = ftcore_test::allocStats().count;
    float sum = 0.f;
    for (int i = 0; i < 100; ++i) {
        const TraversalPlan p = set.plan(i % 2 ? wall(60.f + static_cast<float>(i)) : ledge(60.f), 200.f, i % 2);
        if (p.ok()) sum += set.rootAt(p, 0.01f * static_cast<float>(i)).x;
    }
    CHECK(std::isfinite(sum));
    if (hooked) CHECK(ftcore_test::allocStats().count == before);
}
