#include "ftcore/Traversal.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kFps = 30.f;
constexpr float kLength = 1.5f;
constexpr float kRamp = 0.2f;   // the root's climb onto the top and down off it
constexpr float kDepth = 40.f;  // how far it runs along the top, going over

// A made-up clip at 30 fps, its root tracing the obstacle as the mannequin's clips do: it runs forward at
// `speed`, starts up kRamp before reaching the front edge `ahead` cm in front of its start (`height` cm
// up), then goes kDepth along the top and back down to the floor (over) or stays up (up). The left foot
// rests until 0.1 s, the right from 0.1 to 0.3 (so it takes off from the right); both are in the air
// after, and on the end floor from 1.2 s.
std::vector<TraversalFrame> makeClip(bool up, float ahead = 100.f, float height = 100.f, float speed = 200.f, float drop = 0.f) {
    std::vector<TraversalFrame> frames;
    const int n = static_cast<int>(kLength * kFps) + 1;
    const float top = ahead / speed;
    const float leave = top + kDepth / speed;
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / kFps;
        float z = height * std::clamp((t - (top - kRamp)) / kRamp, 0.f, 1.f);
        if (!up && t > leave) z = height - (height - drop) * std::clamp((t - leave) / kRamp, 0.f, 1.f);
        TraversalFrame f;
        f.root = {speed * t, 0.f, z};
        const float endZ = up ? height : drop;
        auto foot = [&](float restFrom, float restTo, float side) {
            if (t <= restTo && t >= restFrom) return TravPoint{speed * restFrom, side, 0.f};
            if (t >= 1.2f) return TravPoint{speed * 1.2f, side, endZ};
            return TravPoint{speed * t + 20.f, side, z + 30.f}; // in the air, moving
        };
        f.ballLeft = foot(-1.f, 0.1f, 10.f);
        f.ballRight = foot(0.1f, 0.3f, -10.f);
        frames.push_back(f);
    }
    return frames;
}

TraversalClipInfo measure(const std::vector<TraversalFrame>& f) { return measureTraversal(f.data(), f.size(), kLength); }

TraversalObstacle wall(float distance, float height = 100.f, float depth = 25.f) {
    TraversalObstacle o;
    o.distance = distance;
    o.height = height;
    o.depth = depth;
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

constexpr float kFrame = 1.f / kFps;

constexpr float kRollLength = 1.2f;

// A made-up roll at 30 fps, its root on the floor as the mannequin's are: the root runs forward at
// `speed` (but at `tuck` x speed from 0.35 to 0.75 s, as a real roll's slows in its tuck) while the pelvis
// goes from 90 cm down to `low` (smoothly from 0.1 to 0.3 s), stays there until 0.6 and comes back up to
// 90 by 0.9. The left foot rests until 0.1 s, the right from 0.1 to 0.3 (so it takes off from the right);
// both are off the floor after, and back on it from 1 s.
std::vector<TraversalFrame> makeRoll(float speed = 300.f, float low = 40.f, float tuck = 1.f) {
    std::vector<TraversalFrame> frames;
    const int n = static_cast<int>(kRollLength * kFps) + 1;
    auto smooth = [](float u) {
        u = std::clamp(u, 0.f, 1.f);
        return u * u * (3.f - 2.f * u);
    };
    auto ahead = [&](float t) { return speed * (t - (1.f - tuck) * std::clamp(t - 0.35f, 0.f, 0.4f)); };
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / kFps;
        const float down = smooth((t - 0.1f) / 0.2f) - smooth((t - 0.6f) / 0.3f);
        TraversalFrame f;
        f.root = {ahead(t), 0.f, 0.f};
        f.pelvis = {ahead(t), 0.f, 90.f - (90.f - low) * down};
        auto foot = [&](float restFrom, float restTo, float side) {
            if (t <= restTo && t >= restFrom) return TravPoint{ahead(std::max(restFrom, 0.f)), side, 0.f};
            if (t >= 1.f) return TravPoint{ahead(1.f), side, 0.f};
            return TravPoint{ahead(t) + 20.f, side, 40.f}; // over the body, moving
        };
        f.ballLeft = foot(-1.f, 0.1f, 10.f);
        f.ballRight = foot(0.1f, 0.3f, -10.f);
        frames.push_back(f);
    }
    return frames;
}

TraversalClipInfo rollOf(const std::vector<TraversalFrame>& f) { return measureRoll(f.data(), f.size(), kRollLength); }

// Feet as retargeted mocap has them at a run, on makeClip's or makeRoll's frames: the right's rest (0.1 to
// 0.3 s) creeps forward at 60 cm/s and jitters 0.5 cm either way, every other step near 90 cm/s, while the
// left skims the floor after its own rest, at the body's speed.
void runFeet(std::vector<TraversalFrame>& frames) {
    for (std::size_t i = 3; i <= 9; ++i) {
        frames[i].ballRight.x += 60.f * (static_cast<float>(i) * kFrame - 0.1f) + (i % 2 ? 0.5f : -0.5f);
        if (i > 3) frames[i].ballLeft.z = 0.f;
    }
}

constexpr int kFallFrames = 12; // 0.4 s

// makeRoll's roll after a fall, as a land roll's clip opens: for kFallFrames before it the whole body
// comes down from `height` at a steady rate (still coming in at 300 cm/s), its last frame in the air
// height / kFallFrames up, and touches down on makeRoll's first frame.
std::vector<TraversalFrame> makeLandRoll(float height = 300.f) {
    const std::vector<TraversalFrame> roll = makeRoll();
    std::vector<TraversalFrame> frames;
    for (int i = 0; i < kFallFrames; ++i) {
        const float u = static_cast<float>(kFallFrames - i) / static_cast<float>(kFallFrames); // the fall still to come
        TraversalFrame f = roll.front();
        for (TravPoint* p : {&f.root, &f.pelvis, &f.ballLeft, &f.ballRight}) {
            p->x -= 300.f * kFallFrames * kFrame * u;
            p->z += height * u;
        }
        frames.push_back(f);
    }
    frames.insert(frames.end(), roll.begin(), roll.end());
    return frames;
}

TraversalClipInfo landRollOf(const std::vector<TraversalFrame>& f) {
    return measureRoll(f.data(), f.size(), static_cast<float>(f.size() - 1) * kFrame);
}

constexpr float kCatchLength = 2.f;
constexpr float kCatchTime = 0.6f; // frame 18
constexpr float kTop = 300.f;
constexpr float kGravity = 980.f;

// A made-up catch at 30 fps, its edge at the origin and its top kTop up, as the mannequin's catches are: the
// root falls onto the catch at 1000 cm/s down and 50 forward, `hang` cm under the top and `ahead` behind the
// edge, at kCatchTime, climbs steadily onto the top and 20 cm past the edge over 0.3 s, and goes on at `exit`.
std::vector<TraversalFrame> makeCatch(float hang = 64.f, float ahead = 15.f, float exit = 0.f) {
    std::vector<TraversalFrame> frames;
    const int n = static_cast<int>(kCatchLength * kFps) + 1;
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / kFps;
        TraversalFrame f;
        if (t <= kCatchTime) {
            f.root = {-ahead - 50.f * (kCatchTime - t), 0.f, kTop - hang + 1000.f * (kCatchTime - t)};
        } else if (t <= kCatchTime + 0.3f) {
            const float u = (t - kCatchTime) / 0.3f;
            f.root = {-ahead + (20.f + ahead) * u, 0.f, kTop - hang * (1.f - u)};
        } else {
            f.root = {20.f + exit * (t - kCatchTime - 0.3f), 0.f, kTop};
        }
        frames.push_back(f);
    }
    return frames;
}

TraversalClipInfo catchOf(const std::vector<TraversalFrame>& f) { return measureCatch(f.data(), f.size(), kCatchLength); }

} // namespace

TEST_CASE("measureTraversal reads the obstacle from the root: rise, front edge, top, landing") {
    const TraversalClipInfo over = measure(makeClip(false));
    CHECK(over.length == doctest::Approx(kLength));
    CHECK(over.move == TraversalMove::Over);
    // The root starts up at 0.3 s (the first frame past 5% is 0.333), reaches the edge at 0.5 s and
    // 100 cm ahead, leaves the top at 0.7 s (first frame below 95%: 0.733), and is down by 0.9 s.
    CHECK(over.contact == doctest::Approx(10 * kFrame));
    CHECK(over.ledgeAhead == doctest::Approx(100.f));
    CHECK(over.ledgeHeight == doctest::Approx(100.f));
    CHECK(over.leave == doctest::Approx(22 * kFrame));
    CHECK(over.topDepth == doctest::Approx(200.f * 7 * kFrame));
    CHECK(over.land == doctest::Approx(27 * kFrame));
    CHECK(over.rise == doctest::Approx(0.f));
    CHECK(over.takeoffFoot == 1);
    CHECK(over.entrySpeed == doctest::Approx(200.f));
    CHECK(over.path[kTraversalSamples - 1].x == doctest::Approx(300.f));

    const TraversalClipInfo up = measure(makeClip(true));
    CHECK(up.move == TraversalMove::Up);
    CHECK(up.rise == doctest::Approx(100.f));
    CHECK(up.land == doctest::Approx(15 * kFrame)); // on the top as it reaches the edge
    CHECK(up.leave == up.land);
    CHECK(up.topDepth == 0.f);
}

TEST_CASE("measureTraversal leaves a clip with no obstacle, or one dropping off an edge, unmeasured") {
    CHECK(measure(makeClip(false, 100.f, 10.f)).length == 0.f);          // hardly rises
    CHECK(measure(makeClip(false, 100.f, 100.f, 200.f, -500.f)).length == 0.f); // over, then a long fall
    CHECK(measure(makeClip(false, 100.f, 100.f, 200.f, -30.f)).length > 0.f);   // a short step down is fine
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

TEST_CASE("plan enters the clip where its edge is as far as the real one") {
    TraversalSet set;
    set.setClip(0, measure(makeClip(false)));
    // At the clip's own distance it starts from the top, with nothing to make up.
    TraversalPlan p = set.plan(wall(100.f), 200.f, 1);
    CHECK(p.start == doctest::Approx(0.f));
    CHECK(p.atContact.x == doctest::Approx(0.f).epsilon(0.01));
    // Nearer: later in, still nothing to make up (the root runs at 200 cm/s: 30 cm is 0.15 s).
    p = set.plan(wall(70.f), 200.f, 1);
    CHECK(p.start == doctest::Approx(0.15f).epsilon(0.01));
    CHECK(std::fabs(p.atContact.x) < 0.5f);
    // Farther than the clip reaches, within maxPush: the offset carries the rest.
    p = set.plan(wall(130.f), 200.f, 1);
    CHECK(p.start == 0.f);
    CHECK(p.atContact.x == doctest::Approx(30.f));
    // Farther still: not yet.
    p = set.plan(wall(200.f), 200.f, 1);
    CHECK_FALSE(p.ok());
    CHECK(p.tooFar);
    // So near that even the latest entry (0.15 s before the contact: 37 cm off) pulls back too far.
    CHECK_FALSE(set.plan(wall(-20.f), 200.f, 1).ok());
    CHECK_FALSE(set.plan(wall(-20.f), 200.f, 1).tooFar);
}

TEST_CASE("rootAt meets the real edge and height on the top, and the real floor after") {
    TraversalSet set;
    set.setClip(0, measure(makeClip(false)));
    const TraversalClipInfo& c = set.clip(0);

    TraversalObstacle o = wall(70.f, 120.f);
    o.backHeight = -30.f;
    const TraversalPlan p = set.plan(o, 200.f, 1);
    REQUIRE(p.ok());
    CHECK(p.atContact.z == doctest::Approx(20.f));
    CHECK(p.atLeave.x == doctest::Approx(p.atContact.x)); // the wall is thinner than the clip's top
    CHECK(p.atLand.z == doctest::Approx(-30.f));

    // At the entry the root is where the body is.
    const TravPoint at0 = set.rootAt(p, p.start);
    CHECK(std::fabs(at0.x) + std::fabs(at0.z) < 0.01f);
    // Reaching the top it is at the real edge, at the real height, and stays up along the top.
    const TravPoint edge = set.rootAt(p, 0.5f);
    CHECK(edge.x == doctest::Approx(70.f).epsilon(0.01));
    CHECK(edge.z == doctest::Approx(120.f).epsilon(0.01));
    CHECK(set.rootAt(p, 0.6f).z == doctest::Approx(120.f).epsilon(0.01));
    // After the landing it holds the landing's offset: on the real far floor.
    CHECK(set.rootAt(p, kLength).z == doctest::Approx(-30.f));
    // It eases: halfway to the contact the offset is halfway.
    const float mid = 0.5f * (p.start + c.contact);
    const TravPoint from = traversalPath(c, p.start);
    CHECK(set.rootAt(p, mid).z - (traversalPath(c, mid).z - from.z) == doctest::Approx(10.f));

    // A top deeper than the clip's (still thin enough to go over): the extra is made up along the top.
    const TraversalPlan deep = set.plan(wall(70.f, 100.f, 55.f), 200.f, 1);
    REQUIRE(deep.ok());
    CHECK(deep.atLeave.x - deep.atContact.x == doctest::Approx(55.f - c.topDepth));
    CHECK(deep.atLand.x == doctest::Approx(deep.atLeave.x));
}

TEST_CASE("exitTime hands back just after the landing, within the clip") {
    TraversalSet set;
    set.setClip(0, measure(makeClip(false)));
    const TraversalClipInfo& c = set.clip(0);
    const TraversalPlan p = set.plan(wall(70.f), 200.f, 1);
    REQUIRE(p.ok());
    CHECK(set.exitTime(p) == doctest::Approx(std::min(c.land + 0.3f, c.length)));
    TraversalTuning t;
    t.exitAfter = 100.f;
    set.setTuning(t);
    CHECK(set.exitTime(p) == doctest::Approx(c.length));
    CHECK(set.exitTime(TraversalPlan{}) == 0.f);
}

TEST_CASE("rate plays the clip at the body's speed at the entry, easing to its own by the contact") {
    TraversalSet set;
    set.setClip(0, measure(makeClip(false))); // comes in at 200 cm/s
    const TraversalClipInfo& c = set.clip(0);
    const TraversalPlan p = set.plan(wall(70.f), 260.f, 1);
    REQUIRE(p.ok());
    CHECK(p.entryRate == doctest::Approx(1.3f));
    CHECK(set.rate(p, p.start) == doctest::Approx(1.3f));
    CHECK(set.rate(p, c.contact) == doctest::Approx(1.f));
    CHECK(set.rate(p, c.land) == 1.f);
    // Steadily down from one to the other.
    float last = set.rate(p, p.start);
    for (float t = p.start; t <= c.contact; t += 0.01f) {
        const float r = set.rate(p, t);
        CHECK(r <= last + 1e-6f);
        CHECK(r >= 1.f - 1e-6f);
        last = r;
    }
    // Advanced by dt x rate, the root goes on at the body's speed as it enters.
    const float dt = 1.f / 60.f;
    const float t1 = p.start + dt * set.rate(p, p.start);
    CHECK(set.advance(p, p.start, dt) == doctest::Approx(t1));
    CHECK((set.rootAt(p, t1).x - set.rootAt(p, p.start).x) / dt == doctest::Approx(260.f).epsilon(0.02));
    for (float t = p.start; t < kLength; t += 0.1f) CHECK(set.advance(p, t, dt) == doctest::Approx(t + dt * set.rate(p, t)));
    // Slower than the clip, and within limits.
    CHECK(set.plan(wall(70.f), 180.f, 1).entryRate == doctest::Approx(0.9f));
    CHECK(set.plan(wall(70.f), 1000.f, 1).entryRate == doctest::Approx(set.tuning().maxEntryRate));
    CHECK(set.plan(wall(70.f), 0.f, 1).entryRate == doctest::Approx(set.tuning().minEntryRate));
    CHECK(set.rate(TraversalPlan{}, 0.5f) == 1.f);
    CHECK(set.advance(TraversalPlan{}, 0.5f, 0.1f) == doctest::Approx(0.6f));
}

TEST_CASE("measureRoll reads the roll from the pelvis: the tuck, back up, how far down") {
    const std::vector<TraversalFrame> frames = makeRoll();
    const TraversalClipInfo r = rollOf(frames);
    CHECK(r.length == doctest::Approx(kRollLength));
    CHECK(r.move == TraversalMove::Roll);
    // Tucked at 0.3 s (the first frame within 5% of the way down), back up at 0.867 (the first past 95%
    // of the way up), 50 cm down.
    CHECK(r.contact == doctest::Approx(9 * kFrame));
    CHECK(r.land == doctest::Approx(26 * kFrame));
    CHECK(r.leave == r.land);
    CHECK(r.dip == doctest::Approx(50.f));
    CHECK(r.rise == 0.f);
    CHECK(r.touchdown == 0.f); // on the floor from the start
    CHECK(r.takeoffFoot == 1);
    CHECK(r.entrySpeed == doctest::Approx(300.f));
    CHECK(r.path[kTraversalSamples - 1].x == doctest::Approx(360.f));
    CHECK(r.ledgeAhead == 0.f);
    CHECK(r.ledgeHeight == 0.f);
    // Its root never rises: no traversal to measure in it.
    CHECK(measureTraversal(frames.data(), frames.size(), kRollLength).length == 0.f);
}

TEST_CASE("measureRoll leaves a shallow dip, an unsampled pelvis or a root off the floor unmeasured") {
    CHECK(rollOf(makeRoll(300.f, 70.f)).length == 0.f); // 20 cm down
    std::vector<TraversalFrame> frames = makeRoll();
    for (TraversalFrame& f : frames) f.pelvis = {};
    CHECK(rollOf(frames).length == 0.f);
    // A root that follows the pelvis down would carry the body into the floor: it touches down only once
    // it is back up, with no dip left after.
    frames = makeRoll();
    for (TraversalFrame& f : frames) f.root.z = f.pelvis.z - 90.f;
    CHECK(rollOf(frames).length == 0.f);
    CHECK(rollOf(makeClip(false)).length == 0.f); // a vault
    CHECK(measureRoll(nullptr, 0, 1.f).length == 0.f);
}

TEST_CASE("The take-off foot is read through a planted foot's slip and jitter at a run, not off a foot skimming along") {
    // At 400 cm/s a foot moving less than a quarter of the root's way over the rest window (10 cm over 3
    // frames) is planted, though every other step of the right's is near 90 cm/s and its net 7 cm is past
    // kRestSpeed's 4. The left, skimming the floor at the body's speed, is not.
    std::vector<TraversalFrame> roll = makeRoll(400.f);
    runFeet(roll);
    CHECK(rollOf(roll).takeoffFoot == 1);
    std::vector<TraversalFrame> hurdle = makeClip(false, 200.f, 100.f, 400.f);
    runFeet(hurdle);
    const TraversalClipInfo over = measure(hurdle);
    REQUIRE(over.length > 0.f);
    CHECK(over.takeoffFoot == 1);
    // Under a body at 100 cm/s the same slip is a foot on the move: the left's rest before it is the last.
    std::vector<TraversalFrame> walk = makeRoll(100.f);
    runFeet(walk);
    CHECK(rollOf(walk).takeoffFoot == 0);
}

TEST_CASE("measureRoll reads a land roll from its touchdown, and planRoll enters it there") {
    // Down from 3 m (its last frame in the air 25 cm up) or from 1.2 m (10 cm up: within kMinObstacle of the
    // floor, but not on it yet), it touches down 0.4 s in and rolls as makeRoll does from there.
    for (const float height : {300.f, 120.f}) {
        CAPTURE(height);
        const std::vector<TraversalFrame> frames = makeLandRoll(height);
        const TraversalClipInfo r = landRollOf(frames);
        REQUIRE(r.length == doctest::Approx(static_cast<float>(frames.size() - 1) * kFrame));
        CHECK(r.move == TraversalMove::Roll);
        CHECK(r.touchdown == doctest::Approx(kFallFrames * kFrame));
        CHECK(r.contact == doctest::Approx((kFallFrames + 9) * kFrame));
        CHECK(r.land == doctest::Approx((kFallFrames + 26) * kFrame));
        CHECK(r.dip == doctest::Approx(50.f));
        CHECK(r.rise == 0.f);
        CHECK(r.takeoffFoot == 1);
        CHECK(r.entrySpeed == doctest::Approx(300.f));
        // The path runs from the touchdown, the fall left out.
        CHECK(r.path[0].z == 0.f);
        CHECK(r.path[kTraversalSamples - 1].x == doctest::Approx(360.f));

        TraversalSet set;
        set.setClip(0, r);
        const TraversalPlan p = set.planRoll(390.f, 1, true);
        REQUIRE(p.ok());
        CHECK(p.start == doctest::Approx(r.touchdown));
        CHECK(p.speed == doctest::Approx(390.f));
        CHECK(set.rate(p, p.start) == doctest::Approx(1.3f).epsilon(1e-3));
        CHECK(set.exitTime(p) >= r.land);
        for (int i = 0; i <= 12; ++i) {
            const float t = p.start + 0.1f * static_cast<float>(i);
            const TravPoint at = set.rootAt(p, t);
            CHECK(at.x == doctest::Approx(300.f * (t - p.start)));
            CHECK(at.z == 0.f);
        }
    }
    // A fall onto the floor and no roll after it.
    std::vector<TraversalFrame> landing = makeLandRoll();
    for (std::size_t i = kFallFrames; i < landing.size(); ++i) landing[i].pelvis.z = 90.f;
    CHECK(landRollOf(landing).length == 0.f);
}

TEST_CASE("planRoll takes the roll nearest the body's speed, then its foot, from the start along its own path") {
    TraversalSet set;
    set.setClip(0, rollOf(makeRoll(400.f))); // a sprint's roll
    set.setClip(1, rollOf(makeRoll(150.f))); // a walk's
    CHECK(set.planRoll(450.f, -1).clip == 0);
    CHECK(set.planRoll(120.f, -1).clip == 1);

    const TraversalPlan p = set.planRoll(380.f, 1);
    REQUIRE(p.ok());
    CHECK(p.start == 0.f);
    CHECK(p.speed == doctest::Approx(550.f)); // rollMinSpeed
    CHECK(p.entryRate == doctest::Approx(550.f / 400.f).epsilon(1e-3));
    for (int i = 0; i <= 12; ++i) {
        const float t = 0.1f * static_cast<float>(i);
        const TravPoint at = set.rootAt(p, t);
        CHECK(at.x == doctest::Approx(traversalPath(set.clip(0), t).x));
        CHECK(at.z == 0.f);
    }

    // Two rolls alike but for the foot: the one taking off from the body's foot.
    TraversalSet feet;
    std::vector<TraversalFrame> left = makeRoll();
    for (TraversalFrame& f : left) std::swap(f.ballLeft, f.ballRight);
    feet.setClip(0, rollOf(makeRoll()));
    feet.setClip(1, rollOf(left));
    CHECK(feet.clip(1).takeoffFoot == 0);
    CHECK(feet.planRoll(300.f, 1).clip == 0);
    CHECK(feet.planRoll(300.f, 0).clip == 1);

    CHECK_FALSE(TraversalSet{}.planRoll(300.f, 1).ok());
    TraversalSet hurdles;
    hurdles.setClip(0, measure(makeClip(false)));
    CHECK_FALSE(hurdles.planRoll(300.f, 1).ok());
}

TEST_CASE("plan never takes a roll, even for an obstacle low enough to pass for its height") {
    TraversalSet set;
    set.setClip(0, rollOf(makeRoll()));
    // A 25 cm wall within a roll's reach: its height is within the slack of a roll's (none).
    CHECK_FALSE(set.plan(wall(30.f, 25.f), 300.f, 1).ok());
    CHECK_FALSE(set.plan(wall(70.f, 25.f), 300.f, 1).tooFar);
    // With a hurdle for it, the hurdle, though the roll's entry speed is nearer the body's.
    set.setClip(1, measure(makeClip(false, 100.f, 25.f)));
    CHECK(set.plan(wall(30.f, 25.f), 300.f, 1).clip == 1);
}

TEST_CASE("planRoll keeps the body's speed, no slower than rollMinSpeed, or a landing's than its clip") {
    TraversalSet set;
    set.setClip(0, rollOf(makeRoll(400.f)));
    CHECK(set.planRoll(300.f, 1).speed == doctest::Approx(550.f));
    CHECK(set.planRoll(700.f, 1).speed == doctest::Approx(700.f));
    CHECK(set.planRoll(300.f, 1, true).speed == doctest::Approx(400.f));
    CHECK(set.planRoll(500.f, 1, true).speed == doctest::Approx(500.f));
    // With no floor, a body standing still rolls at the clip's own pace.
    TraversalTuning t;
    t.rollMinSpeed = 0.f;
    set.setTuning(t);
    const TraversalPlan still = set.planRoll(0.f, 1);
    REQUIRE(still.ok());
    CHECK(still.speed == 0.f);
    CHECK(still.entryRate == 1.f);
    CHECK(set.rate(still, 0.5f) == 1.f);
    CHECK(set.advance(still, 0.5f, 0.1f) == doctest::Approx(0.6f));
}

TEST_CASE("A roll keeps its speed through the tuck: advance() plays its clip faster where its root slows") {
    TraversalSet set;
    set.setClip(0, rollOf(makeRoll(400.f, 40.f, 0.65f))); // in at 400 cm/s, 260 in the tuck (0.35 to 0.75 s)
    const TraversalClipInfo& c = set.clip(0);
    REQUIRE(c.entrySpeed == doctest::Approx(400.f));
    const TraversalPlan p = set.planRoll(300.f, 1);
    REQUIRE(p.ok());
    CHECK(p.speed == doctest::Approx(550.f));
    CHECK(p.entryRate == doctest::Approx(550.f / 400.f).epsilon(1e-3));
    CHECK(set.rate(p, 0.5f) == doctest::Approx(550.f / 260.f).epsilon(1e-3));
    // Frame by frame at 60 fps the root goes 550 cm/s over the ground all the way, the clip playing no
    // slower than at its entry and no faster than in the tuck: the body is back up when it has gone the
    // way there at 550.
    const float dt = 1.f / 60.f;
    float t = p.start;
    float clock = 0.f;
    float upAt = 0.f;
    while (true) {
        const float next = set.advance(p, t, dt);
        if (next >= c.length) break;
        CAPTURE(t);
        CHECK((set.rootAt(p, next).x - set.rootAt(p, t).x) / dt == doctest::Approx(550.f).epsilon(0.01));
        CHECK((next - t) / dt >= 550.f / 400.f - 1e-3f);
        CHECK((next - t) / dt <= 550.f / 260.f + 1e-3f);
        clock += dt;
        if (upAt == 0.f && next >= c.land) upAt = clock;
        t = next;
    }
    const float there = set.rootAt(p, c.land).x / 550.f;
    CHECK(upAt > there - 1e-3f);
    CHECK(upAt < there + dt + 1e-3f);

    // At 900 the tuck would need its clip played 3.5 times over: it plays at rollMaxRate, the body going
    // 650 cm/s (2.5 x 260) there.
    const TraversalPlan fast = set.planRoll(900.f, 1);
    CHECK(fast.entryRate == doctest::Approx(900.f / 400.f).epsilon(1e-3));
    CHECK(set.rate(fast, 0.5f) == doctest::Approx(set.tuning().rollMaxRate));
    const float on = set.advance(fast, 0.5f, dt);
    CHECK(on == doctest::Approx(0.5f + dt * set.tuning().rollMaxRate));
    CHECK((set.rootAt(fast, on).x - set.rootAt(fast, 0.5f).x) / dt == doctest::Approx(650.f).epsilon(0.01));
    // A clip running faster than the roll goes plays no slower than minEntryRate.
    TraversalSet quick;
    quick.setClip(0, rollOf(makeRoll(800.f)));
    const TraversalPlan slow = quick.planRoll(300.f, 1);
    REQUIRE(slow.ok());
    CHECK(quick.rate(slow, 0.5f) == doctest::Approx(quick.tuning().minEntryRate));
    CHECK(quick.advance(slow, 0.5f, dt) == doctest::Approx(0.5f + dt * quick.tuning().minEntryRate));
}

TEST_CASE("A roll hands back exitAfter past the body being back up, or once it is up for a body turning off its way") {
    TraversalSet set;
    set.setClip(0, rollOf(makeRoll()));
    set.setClip(1, measure(makeClip(false)));
    const TraversalClipInfo& c = set.clip(0);
    const TraversalPlan p = set.planRoll(390.f, 1);
    REQUIRE(p.clip == 0);
    CHECK(set.exitTime(p) == doctest::Approx(c.land + 0.3f));
    CHECK(set.exitTime(p, 20.f) == doctest::Approx(c.land + 0.3f));
    CHECK(set.exitTime(p, -350.f) == doctest::Approx(c.land + 0.3f)); // 10 degrees off
    for (const float off : {30.f, 45.f, -45.f, 90.f, 180.f, 200.f, -170.f}) {
        CAPTURE(off);
        CHECK(set.exitTime(p, off) == doctest::Approx(c.land));
    }
    TraversalTuning t;
    t.exitAfter = 100.f; // never past its end
    set.setTuning(t);
    CHECK(set.exitTime(p) == doctest::Approx(c.length));
    CHECK(set.exitTime(p, 90.f) == doctest::Approx(c.land));
    // A vault's body goes exitAfter past its landing whichever way it wants to go.
    set.setTuning(TraversalTuning{});
    const TraversalPlan v = set.plan(wall(70.f), 200.f, 1);
    REQUIRE(v.clip == 1);
    CHECK(set.exitTime(v, 90.f) == doctest::Approx(set.exitTime(v)));
}

TEST_CASE("measureCatch reads the catch, the top and the landing from the root, the edge at the origin") {
    const TraversalClipInfo c = catchOf(makeCatch(64.f, 15.f, 300.f));
    CHECK(c.length == doctest::Approx(kCatchLength));
    CHECK(c.move == TraversalMove::Catch);
    // Caught at 0.6 s, 15 cm behind the edge and 64 under the top; on the top for good from 0.9 s (the first
    // frame within kSettle of it), going on at 300 cm/s.
    CHECK(c.contact == doctest::Approx(18 * kFrame));
    CHECK(c.touchdown == c.contact);
    CHECK(c.ledgeAhead == doctest::Approx(15.f));
    CHECK(c.ledgeHeight == doctest::Approx(64.f));
    CHECK(c.rise == c.ledgeHeight);
    CHECK(c.land == doctest::Approx(27 * kFrame));
    CHECK(c.leave == c.land);
    CHECK(c.exitSpeed == doctest::Approx(300.f));
    CHECK(c.takeoffFoot == -1);
    // The path runs from the catch, the fall left out.
    CHECK(c.path[0].z == 0.f);
    CHECK(c.path[kTraversalSamples - 1].x == doctest::Approx(15.f + 20.f + 300.f * 1.1f));
    CHECK(c.path[kTraversalSamples - 1].z == doctest::Approx(64.f));
    // A hang from the edge, the top far above; it stays up there.
    const TraversalClipInfo hang = catchOf(makeCatch(186.f, 43.f));
    CHECK(hang.ledgeHeight == doctest::Approx(186.f));
    CHECK(hang.ledgeAhead == doctest::Approx(43.f));
    CHECK(std::fabs(hang.exitSpeed) < 0.01f);
    // The top near the hips: on it sooner.
    CHECK(catchOf(makeCatch(16.f)).land == doctest::Approx(25 * kFrame));
}

TEST_CASE("measureCatch leaves a clip that comes down onto nothing, or only at its end, unmeasured") {
    const std::vector<TraversalFrame> mantle = makeClip(true), roll = makeRoll();
    CHECK(measureCatch(mantle.data(), mantle.size(), kLength).length == 0.f); // up from the floor
    CHECK(measureCatch(roll.data(), roll.size(), kRollLength).length == 0.f);
    std::vector<TraversalFrame> falling = makeCatch();
    for (std::size_t i = 0; i < falling.size(); ++i) falling[i].root.z = 1000.f - 10.f * static_cast<float>(i);
    CHECK(catchOf(falling).length == 0.f);
    CHECK(measureCatch(nullptr, 0, 1.f).length == 0.f);
}

TEST_CASE("planCatch takes the catch whose height the body's flight meets") {
    TraversalSet set;
    set.setClip(0, catchOf(makeCatch(16.f)));  // the top at the hips
    set.setClip(1, catchOf(makeCatch(64.f)));  // at the chest
    set.setClip(2, catchOf(makeCatch(186.f))); // a hang
    // At the top of its jump, 60 cm from where it catches: there in 0.2 s, 19.6 cm lower.
    const TravPoint v{300.f, 0.f, 0.f};
    CHECK(set.planCatch(ledge(75.f, 166.f), v, kGravity, 0.f).clip == 2);
    CHECK(set.planCatch(ledge(75.f, 44.f), v, kGravity, 0.f).clip == 1);
    CHECK(set.planCatch(ledge(75.f, -4.f), v, kGravity, 0.f).clip == 0);
    // Between the chest's reach and the hang's: none, the edge not too far.
    const TraversalPlan between = set.planCatch(ledge(75.f, 105.f), v, kGravity, 0.f);
    CHECK_FALSE(between.ok());
    CHECK_FALSE(between.tooFar);
    // A top below the feet: at the hips from the top of the jump, at the chest falling fast.
    CHECK(set.planCatch(ledge(75.f, -16.f), v, kGravity, 0.f).clip == 0);
    CHECK(set.planCatch(ledge(75.f, -16.f), {300.f, 0.f, -300.f}, kGravity, 0.f).clip == 1);
    // A top the flight has the feet over by then is landed on, not caught.
    CHECK_FALSE(set.planCatch(ledge(75.f, -25.f), v, kGravity, 0.f).ok());
    // A top too thin to stand on is not caught.
    TraversalObstacle thin = ledge(75.f, 44.f);
    thin.depth = 30.f;
    CHECK_FALSE(set.planCatch(thin, v, kGravity, 0.f).ok());
}

TEST_CASE("planCatch waits while the body closes in, and times the catch by when it gets there") {
    TraversalSet set;
    set.setClip(0, catchOf(makeCatch())); // caught 15 cm behind the edge, 64 under the top
    const TraversalClipInfo& c = set.clip(0);
    const TravPoint v{300.f, 0.f, 0.f};
    // 0.5 s out: not yet.
    TraversalPlan p = set.planCatch(ledge(15.f + 150.f, 44.f), v, kGravity, 0.f);
    CHECK_FALSE(p.ok());
    CHECK(p.tooFar);
    // 0.4 s out (maxCatchLead): entered 0.4 s before the catch, nothing missed ahead.
    p = set.planCatch(ledge(15.f + 120.f, -14.f), v, kGravity, 0.f);
    REQUIRE(p.ok());
    CHECK(p.start == doctest::Approx(c.contact - 0.4f));
    CHECK(p.atContact.x == doctest::Approx(120.f));
    CHECK(p.atContact.z == doctest::Approx(-78.f));
    CHECK(p.atLand.z == p.atContact.z);
    // 0.1 s out, or nearer: entered minCatchLead before it, the root drawn back what it is nearer.
    p = set.planCatch(ledge(15.f + 30.f, 59.f), v, kGravity, 0.f);
    REQUIRE(p.ok());
    CHECK(p.start == doctest::Approx(c.contact - 0.1f));
    p = set.planCatch(ledge(15.f, 59.f), v, kGravity, 0.f);
    REQUIRE(p.ok());
    CHECK(p.start == doctest::Approx(c.contact - 0.1f));
    CHECK(p.atContact.x == doctest::Approx(0.f));
    // Past the catch by more than maxCatchMiss then: none.
    p = set.planCatch(ledge(-5.f, 59.f), v, kGravity, 0.f);
    CHECK_FALSE(p.ok());
    CHECK_FALSE(p.tooFar);
    // A body hardly closing in (not maxCatchMiss in maxCatchLead) is caught as its flight meets the catch's
    // height (44.1 cm down: 0.3 s), what it misses ahead within maxCatchMiss.
    const TravPoint slow{50.f, 0.f, 0.f};
    p = set.planCatch(ledge(15.f + 50.f, 19.9f), slow, kGravity, 0.f);
    REQUIRE(p.ok());
    CHECK(p.start == doctest::Approx(c.contact - 0.3f).epsilon(0.001));
    CHECK(p.atContact.x == doctest::Approx(50.f));
    p = set.planCatch(ledge(15.f + 70.f, 19.9f), slow, kGravity, 0.f);
    CHECK_FALSE(p.ok());
    CHECK(p.tooFar);
}

TEST_CASE("planCatch waits for a body closing in still rising toward a catch well above its flight") {
    TraversalSet set;
    set.setClip(0, catchOf(makeCatch())); // caught 15 cm behind the edge, 64 under the top
    const TraversalClipInfo& c = set.clip(0);
    // Closing in at 300 cm/s and going up at 300: there in 0.2 s, 40.4 cm up and still rising.
    const TravPoint v{300.f, 0.f, 300.f};
    // The catch 10 cm above that (within maxCatchStretch): caught there.
    TraversalPlan p = set.planCatch(ledge(75.f, 114.4f), v, kGravity, 0.f);
    REQUIRE(p.ok());
    CHECK(p.start == doctest::Approx(c.contact - 0.2f));
    // 30 cm above it: it waits, the catch nearer as the body rises.
    p = set.planCatch(ledge(75.f, 134.4f), v, kGravity, 0.f);
    CHECK_FALSE(p.ok());
    CHECK(p.tooFar);
    // Already coming down by then (going up at 150): caught, the root drawn up the 30 cm its flight falls short.
    p = set.planCatch(ledge(75.f, 104.4f), {300.f, 0.f, 150.f}, kGravity, 0.f);
    REQUIRE(p.ok());
    CHECK(p.start == doctest::Approx(c.contact - 0.2f));
    // Stopped at the wall, still going up at 300: its flight meets the catch 30 cm up, 0.126 s on.
    p = set.planCatch(ledge(15.f, 94.f), {0.f, 0.f, 300.f}, kGravity, 0.f);
    REQUIRE(p.ok());
    CHECK(p.start == doctest::Approx(c.contact - (300.f - std::sqrt(300.f * 300.f - 2.f * kGravity * 30.f)) / kGravity).epsilon(0.001));
}

TEST_CASE("planCatch times a body not closing in by its flight: as it meets the catch's height, or tops out under it") {
    TraversalSet set;
    set.setClip(0, catchOf(makeCatch())); // caught 15 cm behind the edge, 64 under the top
    const TraversalClipInfo& c = set.clip(0);
    // Jumping in place 35 cm from the wall: the catch 84.4 cm up is met rising, 0.2 s on.
    TraversalPlan p = set.planCatch(ledge(35.f, 148.4f), {0.f, 0.f, 520.f}, kGravity, 0.f);
    REQUIRE(p.ok());
    CHECK(p.start == doctest::Approx(c.contact - 0.2f).epsilon(0.001));
    CHECK(p.atContact.x == doctest::Approx(20.f));
    CHECK(p.atContact.z == doctest::Approx(84.4f));
    // 130 cm up it is met 0.403 s on, past maxCatchLead: not yet.
    p = set.planCatch(ledge(35.f, 194.f), {0.f, 0.f, 520.f}, kGravity, 0.f);
    CHECK_FALSE(p.ok());
    CHECK(p.tooFar);
    // A little later in the same jump it is.
    p = set.planCatch(ledge(35.f, 146.9f), {0.f, 0.f, 422.f}, kGravity, 0.f);
    REQUIRE(p.ok());
    CHECK(p.start == doctest::Approx(c.contact - 0.303f).epsilon(0.01));
    // Higher than its flight goes: with that 0.53 s on, not yet; with it 0.31 s on, caught there, the root
    // drawn up the 14 cm the flight falls short.
    p = set.planCatch(ledge(35.f, 214.f), {0.f, 0.f, 520.f}, kGravity, 0.f);
    CHECK_FALSE(p.ok());
    CHECK(p.tooFar);
    p = set.planCatch(ledge(35.f, 124.f), {0.f, 0.f, 300.f}, kGravity, 0.f);
    REQUIRE(p.ok());
    CHECK(p.start == doctest::Approx(c.contact - 300.f / kGravity).epsilon(0.001));
    CHECK(p.atContact.z == doctest::Approx(60.f));
    // Falling past it already: caught minCatchLead on, within maxCatchMiss of where the flight is by then.
    p = set.planCatch(ledge(35.f, 64.f), {0.f, 0.f, -300.f}, kGravity, 0.f);
    REQUIRE(p.ok());
    CHECK(p.start == doctest::Approx(c.contact - 0.1f));
    p = set.planCatch(ledge(35.f, 74.f), {0.f, 0.f, -300.f}, kGravity, 0.f);
    CHECK_FALSE(p.ok());
    CHECK_FALSE(p.tooFar);
}

TEST_CASE("planCatch takes the catch going on nearest the body's speed; across, the body goes on its way") {
    TraversalSet set;
    set.setClip(0, catchOf(makeCatch(64.f, 15.f, 0.f)));   // stands up there
    set.setClip(1, catchOf(makeCatch(64.f, 15.f, 450.f))); // runs on
    const TravPoint v{300.f, 0.f, 0.f};
    CHECK(set.planCatch(ledge(75.f, 44.f), v, kGravity, 0.f).clip == 0);
    CHECK(set.planCatch(ledge(75.f, 44.f), v, kGravity, 420.f).clip == 1);
    const TraversalPlan p = set.planCatch(ledge(75.f, 44.f), {300.f, 100.f, 0.f}, kGravity, 0.f);
    REQUIRE(p.ok());
    CHECK(p.atContact.y == doctest::Approx(20.f)); // 0.2 s at 100 cm/s
    CHECK(p.entryVelocity.y == 100.f);
    CHECK(p.entryRate == 1.f);
}

TEST_CASE("rootAt curves from the body into the catch, then follows the clip onto the top") {
    TraversalSet set;
    set.setClip(0, catchOf(makeCatch(64.f, 15.f, 300.f)));
    const TraversalClipInfo& c = set.clip(0);
    // Coming down at 100 cm/s, 0.25 s from the catch: it falls 55.6 cm by then.
    const TravPoint v{300.f, 0.f, -100.f};
    const TraversalPlan p = set.planCatch(ledge(90.f, 8.f), v, kGravity, 300.f);
    REQUIRE(p.ok());
    CHECK(p.start == doctest::Approx(c.contact - 0.25f));
    // At the entry the root is where the body is, going its way.
    const float h = 0.001f;
    const TravPoint at0 = set.rootAt(p, p.start);
    CHECK(std::fabs(at0.x) + std::fabs(at0.y) + std::fabs(at0.z) < 0.01f);
    const TravPoint at1 = set.rootAt(p, p.start + h);
    CHECK((at1.x - at0.x) / h == doctest::Approx(300.f).epsilon(0.05));
    CHECK((at1.z - at0.z) / h == doctest::Approx(-100.f).epsilon(0.05));
    // At the catch the clip's edge is the real one.
    const TravPoint caught = set.rootAt(p, c.contact);
    CHECK(caught.x + c.ledgeAhead == doctest::Approx(90.f));
    CHECK(caught.z + c.ledgeHeight == doctest::Approx(8.f));
    // It goes into the catch the way the clip goes on from it (35 cm ahead and 64 up over 0.3 s).
    const TravPoint before = set.rootAt(p, c.contact - h);
    const TravPoint after = set.rootAt(p, c.contact + h);
    CHECK((caught.x - before.x) / h == doctest::Approx(35.f / 0.3f).epsilon(0.05));
    CHECK((caught.z - before.z) / h == doctest::Approx(64.f / 0.3f).epsilon(0.05));
    CHECK((after.z - caught.z) / h == doctest::Approx(64.f / 0.3f).epsilon(0.05));
    // Then up onto the real top and on along it.
    CHECK(set.rootAt(p, c.land + 0.1f).z == doctest::Approx(8.f));
    CHECK(set.rootAt(p, c.length).z == doctest::Approx(8.f));
    CHECK(set.rootAt(p, c.length).x == doctest::Approx(90.f + 20.f + 300.f * 1.1f));
    // At its own pace throughout, handing back just after the landing.
    CHECK(set.rate(p, p.start) == 1.f);
    CHECK(set.rate(p, c.contact) == 1.f);
    CHECK(set.exitTime(p) == doctest::Approx(c.land + 0.3f));
}

TEST_CASE("plan and planRoll never take a catch, nor planCatch anything else") {
    TraversalSet set;
    set.setClip(0, catchOf(makeCatch()));
    // A deep ledge at the catch's own height and edge.
    CHECK_FALSE(set.plan(ledge(15.f, 64.f), 300.f, 1).ok());
    CHECK_FALSE(set.plan(ledge(15.f, 64.f), 300.f, 1).tooFar);
    CHECK_FALSE(set.planRoll(300.f, 1).ok());
    TraversalSet others;
    others.setClip(0, measure(makeClip(true)));
    others.setClip(1, rollOf(makeRoll()));
    CHECK_FALSE(others.planCatch(ledge(75.f, 44.f), {300.f, 0.f, 0.f}, kGravity, 0.f).ok());
    CHECK_FALSE(others.planCatch(ledge(75.f, 100.f), {300.f, 0.f, 0.f}, kGravity, 0.f).ok());
}

TEST_CASE("Traversal plans and paths allocate nothing") {
    TraversalSet set;
    set.setClip(0, measure(makeClip(false)));
    set.setClip(1, measure(makeClip(true)));
    set.setClip(2, rollOf(makeRoll()));
    set.setClip(kMaxTraversalClips - 1, catchOf(makeCatch()));
    const bool hooked = ftcore_test::allocHookInstalled();
    const std::uint64_t before = ftcore_test::allocStats().count;
    float sum = 0.f;
    for (int i = 0; i < 100; ++i) {
        const float t = 0.01f * static_cast<float>(i);
        const TraversalPlan p = set.plan(i % 2 ? wall(60.f + static_cast<float>(i)) : ledge(60.f), 200.f, i % 2);
        if (p.ok()) sum += set.rootAt(p, t).x + set.rate(p, t) + set.advance(p, t, 1.f / 60.f);
        const TraversalPlan r = set.planRoll(100.f + 5.f * static_cast<float>(i), i % 2, i % 3 == 0);
        if (r.ok()) sum += set.rootAt(r, t).x + set.rate(r, t) + set.advance(r, t, 1.f / 60.f) + set.exitTime(r, 45.f);
        const TraversalPlan k = set.planCatch(ledge(15.f + static_cast<float>(i), 44.f), {300.f, 0.f, 0.f}, kGravity, 0.f);
        if (k.ok()) sum += set.rootAt(k, 0.3f + 2.f * t).z + set.rate(k, t) + set.exitTime(k);
    }
    CHECK(std::isfinite(sum));
    if (hooked) CHECK(ftcore_test::allocStats().count == before);
}
