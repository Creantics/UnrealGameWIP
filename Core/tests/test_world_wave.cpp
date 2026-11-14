#include <doctest/doctest.h>

#include "ftcore/WorldWave.h"

#include "AllocCounter.h"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>

using namespace ftcore;

namespace {

WaveVec3 offsetFrom(const WaveVec3& origin, float distance) {
    // Along a diagonal, so every axis takes part.
    const float s = distance / std::sqrt(3.f);
    return WaveVec3{origin.x + s, origin.y - s, origin.z + s};
}

} // namespace

TEST_CASE("worldWaveRadiusAt: still through the prelude, 40 m/s to 30 m, then easing to 25 m/s") {
    const WorldWaveParams p;
    CHECK(worldWaveRadiusAt(p, -1.f) == doctest::Approx(0.f));
    CHECK(worldWaveRadiusAt(p, 0.f) == doctest::Approx(0.f));
    CHECK(worldWaveRadiusAt(p, 0.375f) == doctest::Approx(15.f));
    CHECK(worldWaveRadiusAt(p, 0.75f) == doctest::Approx(30.f));
    // u = 1.25: 30 + 25 * 1.25 + 15 * 0.5 * (1 - e^-2.5)
    const float expected = 30.f + 31.25f + 7.5f * (1.f - std::exp(-2.5f));
    CHECK(worldWaveRadiusAt(p, 2.f) == doctest::Approx(expected).epsilon(1e-5));
    CHECK(worldWaveRadiusAt(p, 2.f) == doctest::Approx(68.134f).epsilon(1e-4));
}

TEST_CASE("worldWaveRadiusAt: no jump in speed where the fast phase joins the ease") {
    const WorldWaveParams p;
    const float h = 1e-3f;
    const float before = (worldWaveRadiusAt(p, 0.75f) - worldWaveRadiusAt(p, 0.75f - h)) / h;
    const float after = (worldWaveRadiusAt(p, 0.75f + h) - worldWaveRadiusAt(p, 0.75f)) / h;
    CHECK(before == doctest::Approx(40.f).epsilon(0.005));
    CHECK(after == doctest::Approx(40.f).epsilon(0.005));
    // Far out the speed has eased to the slow speed.
    const float late = (worldWaveRadiusAt(p, 6.f + h) - worldWaveRadiusAt(p, 6.f)) / h;
    CHECK(late == doctest::Approx(25.f).epsilon(0.01));
}

TEST_CASE("worldWaveBandWidth: 1.5 m at the origin, 4 m at 60 m and held") {
    const WorldWaveParams p;
    CHECK(worldWaveBandWidth(p, 0.f) == doctest::Approx(1.5f));
    CHECK(worldWaveBandWidth(p, 30.f) == doctest::Approx(2.75f));
    CHECK(worldWaveBandWidth(p, 60.f) == doctest::Approx(4.f));
    CHECK(worldWaveBandWidth(p, 100.f) == doctest::Approx(4.f));
}

TEST_CASE("evaluateWorldWave: the prelude runs 0.3 s before the front moves") {
    WorldWaveParams p;
    p.startTime = 10.0;
    const WorldWaveFrame early = evaluateWorldWave(p, 9.9);
    CHECK_FALSE(early.active);
    CHECK(early.preludeProgress == doctest::Approx(0.f));
    CHECK(early.radius == doctest::Approx(0.f));

    const WorldWaveFrame atStart = evaluateWorldWave(p, 10.0);
    CHECK(atStart.active);
    CHECK(atStart.preludeProgress == doctest::Approx(0.f));
    CHECK(atStart.radius == doctest::Approx(0.f));
    CHECK(atStart.bandWidth == doctest::Approx(1.5f));

    const WorldWaveFrame half = evaluateWorldWave(p, 10.15);
    CHECK(half.preludeProgress == doctest::Approx(0.5f));
    CHECK(half.radius == doctest::Approx(0.f));

    const WorldWaveFrame done = evaluateWorldWave(p, 10.3);
    CHECK(done.preludeProgress == doctest::Approx(1.f));
    CHECK(done.radius == doctest::Approx(0.f));

    const WorldWaveFrame moving = evaluateWorldWave(p, 10.3 + 0.375);
    CHECK(moving.preludeProgress == doctest::Approx(1.f));
    CHECK(moving.radius == doctest::Approx(15.f).epsilon(1e-4));
}

TEST_CASE("classifyWorldWave: old beyond the front, new inside the band, the band between") {
    WorldWaveParams p;
    p.origin = WaveVec3{10.f, -5.f, 2.f};
    // tau = 0.5: r = 20, w = 1.5 + 2.5 * 20 / 60
    const WorldWaveFrame f = evaluateWorldWave(p, 0.8);
    REQUIRE(f.radius == doctest::Approx(20.f).epsilon(1e-4));
    const float w = 1.5f + 2.5f * 20.f / 60.f;
    REQUIRE(f.bandWidth == doctest::Approx(w).epsilon(1e-4));

    const WaveClass beyond = classifyWorldWave(f, p.origin, offsetFrom(p.origin, 25.f));
    CHECK(beyond.side == WaveSide::Old);
    CHECK(beyond.band == doctest::Approx(0.f));

    const WaveClass inside = classifyWorldWave(f, p.origin, offsetFrom(p.origin, 10.f));
    CHECK(inside.side == WaveSide::New);
    CHECK(inside.band == doctest::Approx(1.f));

    const WaveClass band = classifyWorldWave(f, p.origin, offsetFrom(p.origin, 19.f));
    CHECK(band.side == WaveSide::Band);
    CHECK(band.band == doctest::Approx(1.f / w).epsilon(1e-3));

    const WaveClass innerEdge = classifyWorldWave(f, p.origin, offsetFrom(p.origin, 20.f - w + 0.01f));
    CHECK(innerEdge.side == WaveSide::Band);
    CHECK(innerEdge.band > 0.99f);

    // Before the front moves, everything but the origin itself is the old world.
    const WorldWaveFrame still = evaluateWorldWave(p, 0.1);
    CHECK(classifyWorldWave(still, p.origin, offsetFrom(p.origin, 0.5f)).side == WaveSide::Old);
}

TEST_CASE("classifyWorldWave: Reverse swaps the sides, the band's value stays geometric") {
    WorldWaveParams p;
    p.direction = WaveDirection::Reverse;
    const WorldWaveFrame f = evaluateWorldWave(p, 0.8);
    CHECK(f.direction == WaveDirection::Reverse);
    const float w = f.bandWidth;
    CHECK(classifyWorldWave(f, p.origin, offsetFrom(p.origin, 25.f)).side == WaveSide::New);
    CHECK(classifyWorldWave(f, p.origin, offsetFrom(p.origin, 10.f)).side == WaveSide::Old);
    const WaveClass band = classifyWorldWave(f, p.origin, offsetFrom(p.origin, 19.f));
    CHECK(band.side == WaveSide::Band);
    CHECK(band.band == doctest::Approx(1.f / w).epsilon(1e-3));
}

TEST_CASE("updateWorldWave: cameraInside ramps over 0.25 s from the moment the inner edge passes") {
    const WorldWaveParams p;  // origin at 0, start at 0
    const WaveVec3 viewer = offsetFrom(p.origin, 10.f);
    // The inner edge r - w reaches 10 m where r (1 - 1/24) = 11.5: r = 12, tau = 0.3, t = 0.6.
    WorldWaveState s;
    float at[61] = {};
    for (int i = 0; i <= 60; ++i) at[i] = updateWorldWave(s, p, i / 30.0, viewer).frame.cameraInside;
    CHECK(s.crossed);
    CHECK(s.crossTime == doctest::Approx(0.6).epsilon(1e-4));
    CHECK(at[17] == doctest::Approx(0.f));
    CHECK(at[18] == doctest::Approx(0.f).epsilon(1e-3));
    CHECK(at[21] == doctest::Approx(0.4f).epsilon(1e-3));   // t = 0.7
    CHECK(at[24] == doctest::Approx(0.8f).epsilon(1e-3));   // t = 0.8
    CHECK(at[26] == doctest::Approx(1.f));                 // t = 0.8667
    CHECK(at[60] == doctest::Approx(1.f));

    // A coarse step places the crossing between its two steps: the same ramp.
    WorldWaveState coarse;
    updateWorldWave(coarse, p, 0.5, viewer);
    const WorldWaveStep late = updateWorldWave(coarse, p, 0.75, viewer);
    CHECK(coarse.crossTime == doctest::Approx(0.6).epsilon(1e-4));
    CHECK(late.frame.cameraInside == doctest::Approx(0.6f).epsilon(1e-3));

    // A viewer the wave never reaches stays out.
    WorldWaveState distant;
    const WorldWaveStep away = updateWorldWave(distant, p, 3.0, offsetFrom(p.origin, 500.f));
    CHECK(away.frame.cameraInside == doctest::Approx(0.f));
    CHECK_FALSE(distant.crossed);
}

TEST_CASE("updateWorldWave: Prelude, FrontPassedListener and Settled each fire once, in order; no allocation") {
    WorldWaveParams p;
    p.startTime = 0.5;
    const WaveVec3 listener = offsetFrom(p.origin, 50.f);
    WorldWaveState s;
    int count[kWorldWaveCueCount] = {};
    double first[kWorldWaveCueCount] = {-1.0, -1.0, -1.0};
    const WorldWaveCue cues[kWorldWaveCueCount] = {WorldWaveCue::Prelude, WorldWaveCue::FrontPassedListener,
                                                   WorldWaveCue::Settled};
    const auto before = ftcore_test::allocStats().count;
    for (int i = 0; i <= 30 * 30; ++i) {
        const double t = i / 30.0;
        const WorldWaveStep step = updateWorldWave(s, p, t, listener);
        for (int c = 0; c < kWorldWaveCueCount; ++c) {
            if (!hasWorldWaveCue(step.cues, cues[c])) continue;
            ++count[c];
            if (first[c] < 0.0) first[c] = t;
        }
    }
    const auto after = ftcore_test::allocStats().count;
    if (ftcore_test::allocHookInstalled()) CHECK(after == before);
    for (int c = 0; c < kWorldWaveCueCount; ++c) CHECK(count[c] == 1);
    CHECK(first[0] == doctest::Approx(0.5));
    // r = 50 m about 0.59 s into the ease: t = 0.5 + 0.3 + 0.75 + 0.59.
    CHECK(first[1] > 2.1);
    CHECK(first[1] < 2.2);
    // r = 400 m: 30 + 25 u + 7.5 (1 - e^-2u) = 400, u = 14.5: t = 0.5 + 1.05 + 14.5.
    CHECK(first[2] > 16.0);
    CHECK(first[2] < 16.1);
    CHECK(first[0] < first[1]);
    CHECK(first[1] < first[2]);
    CHECK(std::string(worldWaveCueName(WorldWaveCue::FrontPassedListener)) == "FrontPassedListener");
}

namespace {

struct TagCase {
    WaveClass where;
    WorldTagState state;
};

TagCase tagAt(WorldTag tag, WaveDirection direction, float distance) {
    WorldWaveParams p;
    p.direction = direction;
    const WorldWaveFrame f = evaluateWorldWave(p, 0.8);  // r = 20, w = 1.5 + 2.5 / 3
    TagCase c;
    c.where = classifyWorldWave(f, p.origin, offsetFrom(p.origin, distance));
    c.state = worldTagState(tag, c.where, direction);
    return c;
}

// The band's value at 19 m with r = 20: 1 / w.
float bandAt19() { return 1.f / (1.5f + 2.5f / 3.f); }

} // namespace

TEST_CASE("worldTagState: Both is drawn whole with collision on, on every side, in either direction") {
    for (const WaveDirection dir : {WaveDirection::Forward, WaveDirection::Reverse}) {
        for (const float at : {25.f, 19.f, 10.f}) {
            const TagCase c = tagAt(WorldTag::Both, dir, at);
            CHECK(c.state.drawn);
            CHECK(c.state.cutValue == doctest::Approx(0.f));
            CHECK(c.state.collisionOn);
        }
    }
}

TEST_CASE("worldTagState: OldOnly, Forward: whole beyond the front, eaten across the band, gone inside") {
    const TagCase beyond = tagAt(WorldTag::OldOnly, WaveDirection::Forward, 25.f);
    REQUIRE(beyond.where.side == WaveSide::Old);
    CHECK(beyond.state.drawn);
    CHECK(beyond.state.cutValue == doctest::Approx(0.f));
    CHECK(beyond.state.collisionOn);

    const TagCase band = tagAt(WorldTag::OldOnly, WaveDirection::Forward, 19.f);
    REQUIRE(band.where.side == WaveSide::Band);
    CHECK(band.state.drawn);
    CHECK(band.state.cutValue == doctest::Approx(bandAt19()).epsilon(1e-3));
    CHECK(band.state.collisionOn);

    const TagCase inside = tagAt(WorldTag::OldOnly, WaveDirection::Forward, 10.f);
    REQUIRE(inside.where.side == WaveSide::New);
    CHECK_FALSE(inside.state.drawn);
    CHECK(inside.state.cutValue == doctest::Approx(1.f));
    CHECK_FALSE(inside.state.collisionOn);
}

TEST_CASE("worldTagState: NewOnly, Forward: absent beyond the front, drawn in across the band, whole inside") {
    const TagCase beyond = tagAt(WorldTag::NewOnly, WaveDirection::Forward, 25.f);
    CHECK_FALSE(beyond.state.drawn);
    CHECK(beyond.state.cutValue == doctest::Approx(1.f));
    CHECK_FALSE(beyond.state.collisionOn);

    const TagCase band = tagAt(WorldTag::NewOnly, WaveDirection::Forward, 19.f);
    CHECK(band.state.drawn);
    CHECK(band.state.cutValue == doctest::Approx(1.f - bandAt19()).epsilon(1e-3));
    CHECK_FALSE(band.state.collisionOn);

    const TagCase inside = tagAt(WorldTag::NewOnly, WaveDirection::Forward, 10.f);
    CHECK(inside.state.drawn);
    CHECK(inside.state.cutValue == doctest::Approx(0.f));
    CHECK(inside.state.collisionOn);
}

TEST_CASE("worldTagState: Reverse brings OldOnly back inside the sphere and cuts NewOnly there") {
    const TagCase oldInside = tagAt(WorldTag::OldOnly, WaveDirection::Reverse, 10.f);
    REQUIRE(oldInside.where.side == WaveSide::Old);
    CHECK(oldInside.state.drawn);
    CHECK(oldInside.state.cutValue == doctest::Approx(0.f));
    CHECK(oldInside.state.collisionOn);

    const TagCase oldBeyond = tagAt(WorldTag::OldOnly, WaveDirection::Reverse, 25.f);
    REQUIRE(oldBeyond.where.side == WaveSide::New);
    CHECK_FALSE(oldBeyond.state.drawn);
    CHECK(oldBeyond.state.cutValue == doctest::Approx(1.f));
    CHECK_FALSE(oldBeyond.state.collisionOn);

    // In the band the second world's share is 1 - band for Reverse.
    const TagCase oldBand = tagAt(WorldTag::OldOnly, WaveDirection::Reverse, 19.f);
    CHECK(oldBand.state.drawn);
    CHECK(oldBand.state.cutValue == doctest::Approx(1.f - bandAt19()).epsilon(1e-3));
    CHECK(oldBand.state.collisionOn);

    const TagCase newInside = tagAt(WorldTag::NewOnly, WaveDirection::Reverse, 10.f);
    CHECK_FALSE(newInside.state.drawn);
    CHECK(newInside.state.cutValue == doctest::Approx(1.f));
    CHECK_FALSE(newInside.state.collisionOn);

    const TagCase newBand = tagAt(WorldTag::NewOnly, WaveDirection::Reverse, 19.f);
    CHECK(newBand.state.drawn);
    CHECK(newBand.state.cutValue == doctest::Approx(bandAt19()).epsilon(1e-3));
    CHECK_FALSE(newBand.state.collisionOn);

    const TagCase newBeyond = tagAt(WorldTag::NewOnly, WaveDirection::Reverse, 25.f);
    CHECK(newBeyond.state.drawn);
    CHECK(newBeyond.state.cutValue == doctest::Approx(0.f));
    CHECK(newBeyond.state.collisionOn);
}

namespace {

// Steps one thing 50 m from the origin (footprint 2 m) through a Forward wave at 30 fps for six seconds. The front's
// outer edge reaches 48 m at t = 1.575 (step 48) and passes 50 m at t = 1.642 (step 50).
struct ThingRun {
    WorldWaveThingStep steps[181];
    WorldWaveThing thing;
};

template <typename PawnIn>
void runThing(ThingRun& run, WorldTag tag, WaveDirection direction, PawnIn pawnIn) {
    WorldWaveParams p;
    p.direction = direction;
    run.thing = WorldWaveThing{};
    run.thing.pivot = offsetFrom(p.origin, 50.f);
    run.thing.radius = 2.f;
    run.thing.tag = tag;
    WorldWaveState s;
    for (int i = 0; i <= 180; ++i) {
        const double t = i / 30.0;
        const WorldWaveStep w = updateWorldWave(s, p, t, WaveVec3{});
        run.steps[i] = updateWorldWaveThing(run.thing, p, w.frame, t, pawnIn(t));
    }
}

int firstChange(const ThingRun& run, int from) {
    for (int i = from; i <= 180; ++i)
        if (run.steps[i].collisionChanged) return i;
    return -1;
}

} // namespace

TEST_CASE("updateWorldWaveThing: OldOnly keeps collision until 0.2 s after the front passes, then loses it") {
    ThingRun run;
    runThing(run, WorldTag::OldOnly, WaveDirection::Forward, [](double) { return false; });
    CHECK(run.steps[0].collisionChanged);
    CHECK(run.steps[0].collisionOn);
    REQUIRE(run.thing.passed);
    CHECK(run.thing.passTime == doctest::Approx(50.0 / 30.0));
    const int flip = firstChange(run, 1);
    REQUIRE(flip > 0);
    CHECK(flip == 56);  // 50 + 0.2 s at 30 fps
    CHECK_FALSE(run.steps[flip].collisionOn);
    CHECK(firstChange(run, flip + 1) == -1);
    for (int i = 0; i <= 180; ++i) {
        CHECK_FALSE(run.steps[i].localFront);
        CHECK_FALSE(run.steps[i].held);
    }
    // Shown until the band's inner edge is past its far side: r - w > 52 m first at step 56 (r = 56.45, w = 3.85).
    for (int i = 0; i <= 55; ++i) CHECK(run.steps[i].drawn);
    for (int i = 56; i <= 180; ++i) CHECK_FALSE(run.steps[i].drawn);
}

TEST_CASE("updateWorldWaveThing: NewOnly with a clear footprint gets collision 0.2 s after the front passes") {
    ThingRun run;
    runThing(run, WorldTag::NewOnly, WaveDirection::Forward, [](double) { return false; });
    CHECK(run.steps[0].collisionChanged);
    CHECK_FALSE(run.steps[0].collisionOn);
    const int flip = firstChange(run, 1);
    CHECK(flip == 56);
    CHECK(run.steps[flip].collisionOn);
    CHECK(firstChange(run, flip + 1) == -1);
    CHECK_FALSE(run.thing.held);
    CHECK_FALSE(run.thing.released);
    // Not shown until the front reaches its near side (48 m, step 48), then shown for good.
    for (int i = 0; i < 48; ++i) CHECK_FALSE(run.steps[i].drawn);
    for (int i = 48; i <= 180; ++i) CHECK(run.steps[i].drawn);
}

TEST_CASE("updateWorldWaveThing: Both keeps collision on from the first step and never changes") {
    ThingRun run;
    runThing(run, WorldTag::Both, WaveDirection::Forward, [](double t) { return t > 1.5 && t < 2.5; });
    CHECK(run.steps[0].collisionChanged);
    CHECK(run.steps[0].collisionOn);
    CHECK(firstChange(run, 1) == -1);
    CHECK_FALSE(run.thing.held);
    for (int i = 0; i <= 180; ++i) CHECK(run.steps[i].drawn);
}

TEST_CASE("updateWorldWaveThing: a pawn in a NewOnly footprint holds it, then it cuts in on its own front; no "
          "allocation") {
    ThingRun run;
    const auto before = ftcore_test::allocStats().count;
    runThing(run, WorldTag::NewOnly, WaveDirection::Forward, [](double t) { return t < 3.0; });
    const auto after = ftcore_test::allocStats().count;
    if (ftcore_test::allocHookInstalled()) CHECK(after == before);

    // Arrives at step 48 with the pawn inside: held, hidden, no collision, until the pawn leaves at t = 3 (step 90).
    CHECK_FALSE(run.steps[47].held);
    CHECK_FALSE(run.steps[47].drawn);  // not reached yet
    for (int i = 48; i < 90; ++i) {
        CHECK(run.steps[i].held);
        CHECK_FALSE(run.steps[i].drawn);
        CHECK_FALSE(run.steps[i].collisionOn);
        CHECK_FALSE(run.steps[i].localFront);
    }
    // Released: its own front starts at the near side (48 m) and moves at the slow speed.
    CHECK_FALSE(run.steps[90].held);
    CHECK(run.steps[90].drawn);
    CHECK(run.steps[90].localFront);
    CHECK(run.steps[90].localRadius == doctest::Approx(48.f));
    CHECK(run.steps[91].localRadius == doctest::Approx(48.f + 25.f / 30.f).epsilon(1e-4));
    // It passes the pivot (50 m) at step 93 (50.5 m); collision 0.2 s later at step 99.
    REQUIRE(run.thing.localPassed);
    CHECK(run.thing.localPassTime == doctest::Approx(93.0 / 30.0));
    const int flip = firstChange(run, 1);
    CHECK(flip == 99);
    CHECK(run.steps[flip].collisionOn);
    CHECK(firstChange(run, flip + 1) == -1);
    // Done past the far side plus a band (50 + 2 + 4 = 56 m, step 100); then the wave's own radius rules.
    CHECK(run.steps[99].localFront);
    CHECK_FALSE(run.steps[100].localFront);
    CHECK_FALSE(run.steps[180].localFront);
    CHECK(run.steps[180].collisionOn);
}

TEST_CASE("updateWorldWaveThing: a pawn stepping in after the arrival delays the collision until it leaves") {
    ThingRun run;
    runThing(run, WorldTag::NewOnly, WaveDirection::Forward, [](double t) { return t > 1.62 && t < 2.5; });
    CHECK_FALSE(run.thing.held);  // the footprint was clear when the front arrived (step 48)
    for (int i = 0; i < 75; ++i) CHECK_FALSE(run.steps[i].collisionOn);
    CHECK(firstChange(run, 1) == 75);  // t = 2.5, the first step with the footprint clear
    CHECK(run.steps[75].collisionOn);
    for (int i = 48; i <= 180; ++i) CHECK(run.steps[i].drawn);
}

TEST_CASE("updateWorldWaveThing: Reverse holds an OldOnly arrival the same way and cuts NewOnly away") {
    ThingRun held;
    runThing(held, WorldTag::OldOnly, WaveDirection::Reverse, [](double t) { return t < 2.0; });
    CHECK_FALSE(held.steps[0].collisionOn);
    CHECK(held.steps[48].held);
    CHECK_FALSE(held.steps[48].drawn);
    CHECK(held.steps[60].localFront);  // released at t = 2 (step 60)
    CHECK(held.steps[180].collisionOn);

    ThingRun leaving;
    runThing(leaving, WorldTag::NewOnly, WaveDirection::Reverse, [](double) { return true; });
    CHECK(leaving.steps[0].collisionOn);
    CHECK(firstChange(leaving, 1) == 56);
    CHECK_FALSE(leaving.steps[56].collisionOn);
    CHECK_FALSE(leaving.thing.held);  // a thing the wave takes away is never held
}

TEST_CASE("evaluateWorldWave: the radius stops at maxRadius and the wave settles") {
    WorldWaveParams p;
    const WorldWaveFrame end = evaluateWorldWave(p, 100.0);
    CHECK(end.radius == doctest::Approx(400.f));
    CHECK(end.bandWidth == doctest::Approx(4.f));
    CHECK(end.settled);
    CHECK_FALSE(end.active);

    p.maxRadius = 20.f;
    const WorldWaveFrame tight = evaluateWorldWave(p, 0.3 + 0.5);
    CHECK(tight.radius == doctest::Approx(20.f));
    CHECK(tight.settled);
    const WorldWaveFrame before = evaluateWorldWave(p, 0.3 + 0.49);
    CHECK(before.radius < 20.f);
    CHECK_FALSE(before.settled);
    CHECK(before.active);
}

TEST_CASE("streetWaveParams: 12 m at 0.6 s, 60 m by 2.5 s, a 2 to 5 m band with tongues a band ahead") {
    const WorldWaveParams p = streetWaveParams();
    CHECK(WorldWaveParams{}.tongueLead == 0.f);
    CHECK(p.tongueLead == doctest::Approx(1.f));
    CHECK(evaluateWorldWave(p, 0.3).radius == doctest::Approx(0.f));
    CHECK(evaluateWorldWave(p, 0.6).radius == doctest::Approx(12.f));
    CHECK(evaluateWorldWave(p, 0.8).radius == doctest::Approx(20.f));  // the fast phase ends at 20 m
    const float at25 = evaluateWorldWave(p, 2.5).radius;
    CHECK(at25 > 60.f);
    CHECK(at25 < 61.5f);
    CHECK(evaluateWorldWave(p, 2.4).radius < 60.f);
    CHECK(worldWaveBandWidth(p, 0.f) == doctest::Approx(2.f));
    CHECK(worldWaveBandWidth(p, 60.f) == doctest::Approx(5.f));
    CHECK(worldWaveBandWidth(p, 200.f) == doctest::Approx(5.f));
}

namespace {

// The first step (120 fps) a thing 20 m out with a 1.5 m footprint is drawn, and the first it is not.
void firstDrawnSteps(const WorldWaveParams& p, WorldTag tag, int& firstOn, int& firstOff) {
    WorldWaveThing thing;
    thing.pivot = offsetFrom(p.origin, 20.f);
    thing.radius = 1.5f;
    thing.tag = tag;
    WorldWaveState s;
    firstOn = -1;
    firstOff = -1;
    for (int i = 0; i <= 480; ++i) {
        const double t = i / 120.0;
        const WorldWaveStep w = updateWorldWave(s, p, t, WaveVec3{});
        const WorldWaveThingStep step = updateWorldWaveThing(thing, p, w.frame, t, false);
        if (step.drawn && firstOn < 0) firstOn = i;
        if (!step.drawn && firstOff < 0) firstOff = i;
    }
}

} // namespace

TEST_CASE("updateWorldWaveThing: the tongues' lead draws an arrival early and leaves a departure as it was") {
    WorldWaveParams lead = streetWaveParams();
    WorldWaveParams none = lead;
    none.tongueLead = 0.f;
    int onLead = 0, offLead = 0, onNone = 0, offNone = 0;
    firstDrawnSteps(lead, WorldTag::NewOnly, onLead, offLead);
    firstDrawnSteps(none, WorldTag::NewOnly, onNone, offNone);
    // r + w > 18.5 m with w = 2 + 3 r / 60: r > 15.71 m, t > 0.693 s; without the lead r > 18.5 m, t > 0.7625 s.
    CHECK(onLead == 84);
    CHECK(onNone == 92);
    CHECK(offLead == 0);  // not drawn before it arrives
    // A departure ends when the inner edge passes its far side either way: r - w > 21.5 m.
    firstDrawnSteps(lead, WorldTag::OldOnly, onLead, offLead);
    firstDrawnSteps(none, WorldTag::OldOnly, onNone, offNone);
    CHECK(onLead == 0);
    CHECK(offLead == offNone);
    CHECK(offLead > 0);
}

// ---- Input guards ----

TEST_CASE("beginWorldWave: refuses non-finite origin, start time, speeds, maxRadius and shape, with the reason") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    WorldWaveState s;
    WorldWaveParams good = streetWaveParams();
    CHECK(beginWorldWave(s, good) == WorldWaveRefusal::None);
    CHECK(s.running);

    struct Bad {
        WorldWaveParams p;
        WorldWaveRefusal why;
    };
    Bad cases[13];
    for (Bad& c : cases) c.p = good;
    cases[0].p.origin.x = nan;            cases[0].why = WorldWaveRefusal::BadOrigin;
    cases[1].p.origin.z = inf;            cases[1].why = WorldWaveRefusal::BadOrigin;
    cases[2].p.startTime = std::numeric_limits<double>::quiet_NaN(); cases[2].why = WorldWaveRefusal::BadStartTime;
    cases[3].p.fastSpeed = 0.f;           cases[3].why = WorldWaveRefusal::BadSpeed;
    cases[4].p.slowSpeed = -1.f;          cases[4].why = WorldWaveRefusal::BadSpeed;
    cases[5].p.fastSpeed = inf;           cases[5].why = WorldWaveRefusal::BadSpeed;
    cases[6].p.maxRadius = 0.f;           cases[6].why = WorldWaveRefusal::BadMaxRadius;
    cases[7].p.maxRadius = -5.f;          cases[7].why = WorldWaveRefusal::BadMaxRadius;
    cases[8].p.maxRadius = nan;           cases[8].why = WorldWaveRefusal::BadMaxRadius;
    cases[9].p.bandAtFar = nan;           cases[9].why = WorldWaveRefusal::BadShape;
    cases[10].p.prelude = -0.1f;          cases[10].why = WorldWaveRefusal::BadShape;
    cases[11].p.insideRamp = inf;         cases[11].why = WorldWaveRefusal::BadShape;
    cases[12].p.tongueLead = -1.f;        cases[12].why = WorldWaveRefusal::BadShape;
    for (const Bad& c : cases) {
        WorldWaveState t;
        REQUIRE(beginWorldWave(t, good) == WorldWaveRefusal::None);
        CHECK(beginWorldWave(t, c.p) == c.why);
        CHECK_FALSE(t.running); // the old wave ended, the new one never started
        CHECK(checkWorldWaveParams(c.p) == c.why);
        // Stepping bad parameters runs nothing: an inactive frame, no cues, at any time.
        for (const double time : {0.0, 1.0, 5.0, 100.0}) {
            const WorldWaveStep step = updateWorldWave(t, c.p, time, WaveVec3{});
            CHECK(step.cues == 0);
            CHECK_FALSE(step.frame.active);
            CHECK(step.frame.radius == 0.f);
            CHECK(std::isfinite(step.frame.radius));
        }
        CHECK_FALSE(evaluateWorldWave(c.p, 2.0).active);
    }
    CHECK(std::string(worldWaveRefusalName(WorldWaveRefusal::BadSpeed)) == "BadSpeed");
}

TEST_CASE("beginWorldWave: a restart while running starts over from zero") {
    WorldWaveParams p;
    const WaveVec3 viewer = offsetFrom(p.origin, 10.f);
    WorldWaveState s;
    REQUIRE(beginWorldWave(s, p) == WorldWaveRefusal::None);
    std::uint8_t fired = 0;
    for (int i = 0; i <= 60; ++i) fired |= updateWorldWave(s, p, i / 30.0, viewer).cues;
    REQUIRE(hasWorldWaveCue(fired, WorldWaveCue::Prelude));
    REQUIRE(hasWorldWaveCue(fired, WorldWaveCue::FrontPassedListener));
    REQUIRE(s.crossed);
    // Again from 5 s: every cue fires again, and the old wave's clock does not hold the new one back.
    p.startTime = 5.0;
    REQUIRE(beginWorldWave(s, p) == WorldWaveRefusal::None);
    CHECK(s.running);
    CHECK_FALSE(s.preludeFired);
    CHECK_FALSE(s.crossed);
    CHECK_FALSE(s.hasLast);
    const WorldWaveStep first = updateWorldWave(s, p, 5.0, viewer);
    CHECK(hasWorldWaveCue(first.cues, WorldWaveCue::Prelude));
    CHECK(first.time == doctest::Approx(5.0));
}

TEST_CASE("updateWorldWave: a non-finite or backward time is a step of 0; the clock never runs backward") {
    WorldWaveParams p;
    const WaveVec3 viewer = offsetFrom(p.origin, 200.f);
    WorldWaveState s;
    REQUIRE(beginWorldWave(s, p) == WorldWaveRefusal::None);
    const WorldWaveStep at2 = updateWorldWave(s, p, 2.0, viewer);
    REQUIRE(at2.frame.radius > 0.f);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (const double bad : {nan, inf, -inf, 1.0, 0.0, -50.0}) {
        const WorldWaveStep step = updateWorldWave(s, p, bad, viewer);
        CHECK(step.time == doctest::Approx(2.0));
        CHECK(step.frame.radius == doctest::Approx(at2.frame.radius));
        CHECK(step.cues == 0);
        CHECK(s.lastTime == doctest::Approx(2.0));
    }
    const WorldWaveStep at3 = updateWorldWave(s, p, 3.0, viewer);
    CHECK(at3.time == doctest::Approx(3.0));
    CHECK(at3.frame.radius > at2.frame.radius);
    // Before the start nothing has run, so a NaN there is simply nothing.
    WorldWaveState fresh;
    p.startTime = 1.0;
    const WorldWaveStep none = updateWorldWave(fresh, p, nan, viewer);
    CHECK(none.cues == 0);
    CHECK_FALSE(none.frame.active);
    CHECK(std::isfinite(none.time));
    CHECK(worldWaveRadiusAt(p, std::numeric_limits<float>::quiet_NaN()) == 0.f);
    CHECK_FALSE(evaluateWorldWave(p, nan).active);
}

TEST_CASE("updateWorldWave: a NaN viewer does not eat the cues; they fire by time alone") {
    WorldWaveParams p;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const WaveVec3 lost{nan, 0.f, nan};
    // Never a finite viewer: the origin stands in, so the front passes it as soon as it moves.
    WorldWaveState s;
    REQUIRE(beginWorldWave(s, p) == WorldWaveRefusal::None);
    std::uint8_t fired = 0;
    double frontAt = -1.0;
    for (int i = 0; i <= 30 * 20; ++i) {
        const double t = i / 30.0;
        const WorldWaveStep step = updateWorldWave(s, p, t, lost);
        if (frontAt < 0.0 && hasWorldWaveCue(step.cues, WorldWaveCue::FrontPassedListener)) frontAt = t;
        fired |= step.cues;
        CHECK(std::isfinite(step.frame.cameraInside));
    }
    CHECK(hasWorldWaveCue(fired, WorldWaveCue::Prelude));
    CHECK(hasWorldWaveCue(fired, WorldWaveCue::FrontPassedListener));
    CHECK(hasWorldWaveCue(fired, WorldWaveCue::Settled));
    CHECK(frontAt > 0.3);
    CHECK(frontAt < 0.4);
    // A viewer lost mid-wave keeps its last place: the cue fires when the front reaches it there.
    WorldWaveState m;
    REQUIRE(beginWorldWave(m, p) == WorldWaveRefusal::None);
    const WaveVec3 at50 = offsetFrom(p.origin, 50.f);
    updateWorldWave(m, p, 0.0, at50);
    double lostAt = -1.0;
    for (int i = 1; i <= 30 * 5; ++i) {
        const double t = i / 30.0;
        if (hasWorldWaveCue(updateWorldWave(m, p, t, lost).cues, WorldWaveCue::FrontPassedListener) && lostAt < 0.0) {
            lostAt = t;
        }
    }
    CHECK(lostAt > 1.5);
    CHECK(lostAt < 1.7);
}

TEST_CASE("updateWorldWaveThing: a non-finite pivot or footprint is never reached and keeps its start state") {
    WorldWaveParams p;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    WorldWaveThing a;
    a.pivot = WaveVec3{nan, 0.f, 0.f};
    a.tag = WorldTag::OldOnly;
    WorldWaveThing b;
    b.pivot = offsetFrom(p.origin, 5.f);
    b.radius = nan;
    b.tag = WorldTag::NewOnly;
    WorldWaveState s;
    REQUIRE(beginWorldWave(s, p) == WorldWaveRefusal::None);
    for (int i = 0; i <= 30 * 20; ++i) {
        const WorldWaveStep w = updateWorldWave(s, p, i / 30.0, WaveVec3{});
        const WorldWaveThingStep sa = updateWorldWaveThing(a, p, w.frame, w.time, false);
        const WorldWaveThingStep sb = updateWorldWaveThing(b, p, w.frame, w.time, false);
        CHECK(sa.drawn);
        CHECK(sa.collisionOn);
        CHECK_FALSE(sb.drawn);
        CHECK_FALSE(sb.collisionOn);
    }
    CHECK_FALSE(a.passed);
    CHECK_FALSE(b.arrived);
}
