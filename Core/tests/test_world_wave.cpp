#include <doctest/doctest.h>

#include "ftcore/WorldWave.h"

#include "AllocCounter.h"

#include <cmath>
#include <initializer_list>
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
