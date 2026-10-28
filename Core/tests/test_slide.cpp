#include "ftcore/Slide.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kFps = 30.f;
constexpr float kFrame = 1.f / kFps;
constexpr int kIntoFrames = 30;     // 1 s
constexpr int kOutFrames = 42;      // 1.4 s
constexpr float kStep = 1.f / 60.f; // the game's frame

float seconds(int frames) { return static_cast<float>(frames) * kFrame; }

float smoothstep(float u) {
    u = std::clamp(u, 0.f, 1.f);
    return u * u * (3.f - 2.f * u);
}

// A made-up Into at 30 fps, 1 s long: the root runs forward at `speed` throughout while the pelvis goes
// from 90 cm down to 40 (smoothly from 0.3 to 0.6 s: down from frame 17). The other foot rests for the
// first 3 frames, the take-off foot `foot` (0 left, 1 right) from frame 4 to 9; both are in the air after,
// until the other comes to rest on the floor again from frame 21, past the down.
std::vector<TraversalFrame> makeInto(float speed, int foot) {
    std::vector<TraversalFrame> frames;
    for (int i = 0; i <= kIntoFrames; ++i) {
        const float t = seconds(i);
        TraversalFrame f;
        f.root = {speed * t, 0.f, 0.f};
        f.pelvis = {speed * t, 0.f, 90.f - 50.f * smoothstep((t - 0.3f) / 0.3f)};
        const TravPoint air{speed * t + 20.f, 0.f, 30.f};
        TravPoint takeoff = air, other = air;
        if (i >= 4 && i <= 9) takeoff = {speed * seconds(4), 0.f, 0.f};
        if (i <= 2) other = {0.f, 0.f, 0.f};
        if (i >= 21) other = {speed * seconds(21), 0.f, 0.f};
        takeoff.y = foot == 0 ? 10.f : -10.f;
        other.y = -takeoff.y;
        f.ballLeft = foot == 0 ? takeoff : other;
        f.ballRight = foot == 0 ? other : takeoff;
        frames.push_back(f);
    }
    return frames;
}

// A made-up Out at 30 fps, 1.4 s long: the pelvis comes up from 40 cm to 90 (smoothly from 0.2 to 0.6 s:
// up from frame 17) while the root goes on at `from` cm/s until frame 15 and at `to` after, until frame
// `still` (still from there). The left foot slides along the floor with the root until it plants at frame
// 10; the right is in the air until it comes down beside it at frame 18. Without `plants`, both feet slide
// along with the root throughout.
std::vector<TraversalFrame> makeOut(float from, float to, int still = 1000, bool plants = true) {
    std::vector<TraversalFrame> frames;
    float x = 0.f;
    TravPoint left, right;
    for (int i = 0; i <= kOutFrames; ++i) {
        if (i > 0) x += (i > still ? 0.f : i <= 15 ? from : to) * kFrame;
        TraversalFrame f;
        f.root = {x, 0.f, 0.f};
        f.pelvis = {x, 0.f, 40.f + 50.f * smoothstep((seconds(i) - 0.2f) / 0.4f)};
        if (!plants || i <= 10) left = {x + 15.f, 10.f, 0.f};
        if (!plants) right = {x + 15.f, -10.f, 0.f};
        else if (i < 18) right = {x + 30.f, -10.f, 20.f};
        else if (i == 18) right = {x + 15.f, -10.f, 0.f};
        f.ballLeft = left;
        f.ballRight = right;
        frames.push_back(f);
    }
    return frames;
}

SlideIntoInfo intoOf(const std::vector<TraversalFrame>& f) { return measureSlideInto(f.data(), f.size(), seconds(kIntoFrames)); }
SlideOutInfo outOf(const std::vector<TraversalFrame>& f) { return measureSlideOut(f.data(), f.size(), seconds(kOutFrames)); }

// Intos made at a run (450 cm/s) and a sprint (650), each off either foot; a Loop; and every get-up: the
// still ones creeping to a stop at 1 s, the moving ones going on at a walk, a run, a sprint and a crouch walk.
SlideSet makeSet() {
    SlideSet set;
    set.setInto(0, intoOf(makeInto(450.f, 0)));
    set.setInto(1, intoOf(makeInto(450.f, 1)));
    set.setInto(2, intoOf(makeInto(650.f, 0)));
    set.setInto(3, intoOf(makeInto(650.f, 1)));
    set.setLoop(0.6f);
    set.setOut(SlideOut::IdleStand, outOf(makeOut(200.f, 30.f, 30)));
    set.setOut(SlideOut::IdleCrouch, outOf(makeOut(200.f, 30.f, 30)));
    set.setOut(SlideOut::Walk, outOf(makeOut(200.f, 180.f)));
    set.setOut(SlideOut::Run, outOf(makeOut(200.f, 450.f)));
    set.setOut(SlideOut::Sprint, outOf(makeOut(200.f, 650.f)));
    set.setOut(SlideOut::CrouchWalk, outOf(makeOut(200.f, 150.f)));
    return set;
}

// Steps a slide at 60 fps with the same input until it gets up (at most 20 s); how far it went.
float slideUntilUp(const SlideSet& set, SlideState& s, const SlideInput& in) {
    float distance = 0.f;
    for (int i = 0; i < 1200 && s.sliding() && s.phase != SlidePhase::Out; ++i) {
        set.step(s, in, kStep);
        distance += s.speed * kStep;
    }
    return distance;
}

// A sprint at 700 cm/s off the right foot slid until it gets up.
SlideState slideToOut(const SlideSet& set, const SlideInput& in) {
    SlideState s = set.start(700.f, 1);
    slideUntilUp(set, s, in);
    return s;
}

} // namespace

TEST_CASE("measureSlideInto reads the take-off foot and when it plants, the down and the speeds, not a rest past the down") {
    for (int foot : {0, 1}) {
        const SlideIntoInfo c = intoOf(makeInto(450.f, foot));
        CHECK(c.length == doctest::Approx(1.f));
        CHECK(c.takeoffFoot == foot);
        CHECK(c.takeoff == doctest::Approx(seconds(4)));
        CHECK(c.down == doctest::Approx(seconds(17)));
        CHECK(c.entrySpeed == doctest::Approx(450.f));
        CHECK(c.slideSpeed == doctest::Approx(450.f));
    }
    // No foot at rest before the down (both skim along with the root): it is entered from the start.
    std::vector<TraversalFrame> air = makeInto(450.f, 0);
    for (std::size_t i = 0; i < 17; ++i) air[i].ballLeft = air[i].ballRight = {air[i].root.x + 20.f, 0.f, 30.f};
    const SlideIntoInfo c = intoOf(air);
    CHECK(c.length > 0.f);
    CHECK(c.takeoffFoot == -1);
    CHECK(c.takeoff == 0.f);
    CHECK(c.entrySpeed == doctest::Approx(450.f));
}

TEST_CASE("A clip whose pelvis goes down too little is no Into, and one whose pelvis comes up too little no Out") {
    std::vector<TraversalFrame> shallow = makeInto(450.f, 0);
    for (TraversalFrame& f : shallow) f.pelvis.z = 90.f - 0.3f * (90.f - f.pelvis.z); // 15 cm down
    CHECK(intoOf(shallow).length == 0.f);
    CHECK(intoOf(makeOut(200.f, 450.f)).length == 0.f);
    CHECK(outOf(makeInto(450.f, 0)).length == 0.f);
    const std::vector<TraversalFrame> into = makeInto(450.f, 0);
    CHECK(measureSlideInto(nullptr, 0, 1.f).length == 0.f);
    CHECK(measureSlideInto(into.data(), 2, 1.f).length == 0.f);
    CHECK(measureSlideInto(into.data(), into.size(), 0.f).length == 0.f);
    CHECK(measureSlideOut(nullptr, 0, 1.f).length == 0.f);
    CHECK(slideOutSpeed(SlideOutInfo{}, 0.5f) == 0.f);
}

TEST_CASE("measureSlideOut reads the first plant, when the pelvis is up, when the body has settled, and its speed") {
    const SlideOutInfo run = outOf(makeOut(200.f, 450.f));
    CHECK(run.length == doctest::Approx(1.4f));
    CHECK(run.plant == doctest::Approx(seconds(10)));
    CHECK(run.up == doctest::Approx(seconds(17)));
    CHECK(run.settle == -1.f); // never still
    CHECK(run.endSpeed == doctest::Approx(450.f));
    CHECK(slideOutSpeed(run, 0.f) == doctest::Approx(200.f));
    CHECK(slideOutSpeed(run, 0.1f) == doctest::Approx(200.f));
    CHECK(slideOutSpeed(run, 1.2f) == doctest::Approx(450.f));
    CHECK(slideOutSpeed(run, 5.f) == doctest::Approx(450.f));
    float last = 0.f;
    for (float t = 0.f; t <= 1.4f; t += 0.05f) {
        const float v = slideOutSpeed(run, t);
        CHECK(v >= last - 1e-3f);
        last = v;
    }

    // Still from frame 30: read over two frames either side, still from frame 32.
    const SlideOutInfo still = outOf(makeOut(200.f, 30.f, 30));
    CHECK(still.plant == doctest::Approx(seconds(10)));
    CHECK(still.settle == doctest::Approx(seconds(32)));
    CHECK(still.endSpeed == doctest::Approx(0.f));
    // Still before its pelvis is up (and near its end height a frame before), it has settled once up.
    const SlideOutInfo early = outOf(makeOut(200.f, 0.f, 12));
    CHECK(early.settle == doctest::Approx(early.up));
    // No foot coming to rest: it plants when the pelvis is up.
    const SlideOutInfo skim = outOf(makeOut(200.f, 450.f, 1000, false));
    CHECK(skim.length > 0.f);
    CHECK(skim.plant == doctest::Approx(skim.up));
}

TEST_CASE("start takes the Into nearest the body's speed, then its foot, from its take-off at the body's pace") {
    const SlideSet set = makeSet();
    const SlideState s = set.start(700.f, 1);
    REQUIRE(s.sliding());
    CHECK(s.phase == SlidePhase::Into);
    CHECK(s.into == 3);
    CHECK(s.clip == 3);
    CHECK_FALSE(s.loop);
    CHECK(s.clipTime == doctest::Approx(set.into(3).takeoff));
    CHECK(s.start == s.clipTime);
    CHECK(s.speed == 700.f);
    CHECK(s.entryRate == doctest::Approx(700.f / 650.f));
    CHECK(set.start(700.f, 0).into == 2);
    CHECK(set.start(460.f, 0).into == 0);
    CHECK(set.start(460.f, 1).into == 1);
    CHECK(set.start(460.f, -1).into == 0); // the same cost: index order
    CHECK(set.start(1200.f, 1).entryRate == doctest::Approx(set.tuning().maxEntryRate));
    SlideSet sprint;
    sprint.setInto(0, set.into(2));
    CHECK(sprint.start(400.f, 0).entryRate == doctest::Approx(sprint.tuning().minEntryRate));
    // Too slow to slide, or nothing to slide with.
    CHECK_FALSE(set.start(300.f, 0).sliding());
    CHECK_FALSE(SlideSet{}.start(700.f, 0).sliding());
    CHECK_FALSE(sprint.setInto(kSlideIntos, set.into(0)));
}

TEST_CASE("rate eases from the body's pace at the take-off to the clip's own by the down") {
    const SlideSet set = makeSet();
    SlideState s = set.start(780.f, 1); // 1.2 times the sprint Into's pace
    const SlideIntoInfo& c = set.into(static_cast<std::size_t>(s.into));
    CHECK(set.rate(s) == doctest::Approx(1.2f));
    float last = set.rate(s);
    for (SlideState at = s; at.clipTime < c.down; at.clipTime += 0.01f) {
        const float r = set.rate(at);
        CHECK(r <= last + 1e-6f);
        CHECK(r >= 1.f - 1e-6f);
        last = r;
    }
    s.clipTime = c.down;
    CHECK(set.rate(s) == 1.f);
    CHECK(set.rate(SlideState{}) == 1.f);
}

TEST_CASE("The body keeps its speed until it is down; from there the slide slows by friction, the Into still playing") {
    const SlideSet set = makeSet();
    SlideState s = set.start(700.f, 1);
    for (int i = 0; i < 120 && s.phase == SlidePhase::Into; ++i) {
        CHECK(s.speed == 700.f);
        set.step(s, SlideInput{}, kStep);
    }
    REQUIRE(s.phase == SlidePhase::Down);
    CHECK(s.clipTime >= set.into(3).down);
    CHECK(s.speed == 700.f);
    set.step(s, SlideInput{}, kStep);
    CHECK(s.speed == doctest::Approx(700.f - set.tuning().friction * kStep));
    CHECK(s.clip == s.into); // the Into's tail is the slide
}

TEST_CASE("A slide goes farther downhill and from a faster entry, less far uphill; a steep one speeds it up to its limit") {
    const SlideSet set = makeSet();
    auto distance = [&](float speed, float slope) {
        SlideState s = set.start(speed, 1);
        SlideInput in;
        in.slope = slope;
        return slideUntilUp(set, s, in);
    };
    // About 0.42 s at 700 to the down, then (700^2 - 250^2) / (2 x 400) slowing to the get-up.
    const float flat = distance(700.f, 0.f);
    CHECK(flat == doctest::Approx(826.f).epsilon(0.03));
    CHECK(distance(700.f, -0.1f) > flat + 100.f);
    CHECK(distance(700.f, 0.1f) < flat - 100.f);
    CHECK(distance(500.f, 0.f) < flat - 200.f);

    SlideInput steep;
    steep.slope = -0.5f; // gravity's pull past friction
    SlideState s = set.start(700.f, 1);
    for (int i = 0; i < 600; ++i) set.step(s, steep, kStep);
    CHECK(s.phase == SlidePhase::Down);
    CHECK(s.speed == doctest::Approx(set.tuning().maxSpeed));
    SlideState fast = set.start(1000.f, 1); // in past the limit: holds its speed
    for (int i = 0; i < 600; ++i) set.step(fast, steep, kStep);
    CHECK(fast.speed == doctest::Approx(1000.f));
}

TEST_CASE("The body gets up once the slide is slower than outSpeed, at the slide's speed, from the slide pose") {
    const SlideSet set = makeSet();
    SlideState s = set.start(700.f, 1);
    float before = s.speed;
    for (int i = 0; i < 1200 && s.phase != SlidePhase::Out; ++i) {
        before = s.speed;
        set.step(s, SlideInput{}, kStep);
    }
    REQUIRE(s.phase == SlidePhase::Out);
    CHECK(before >= set.tuning().outSpeed);
    CHECK(s.outFrom < set.tuning().outSpeed);
    CHECK(s.speed == s.outFrom);
    CHECK(s.out == SlideOut::IdleCrouch); // still crouched, no stick
    CHECK(s.clip == slideOutClip(SlideOut::IdleCrouch));
    CHECK(s.clipTime == 0.f);
    CHECK_FALSE(s.loop);
}

TEST_CASE("The get-up follows what the player asks for, sprint over crouch, falling back to the nearest there is") {
    const SlideSet set = makeSet();
    SlideInput in;
    CHECK(set.chooseOut(in) == SlideOut::IdleCrouch);
    in.crouch = false;
    CHECK(set.chooseOut(in) == SlideOut::IdleStand);
    in.sprint = true; // no stick: still
    CHECK(set.chooseOut(in) == SlideOut::IdleStand);
    in.wantSpeed = 700.f;
    CHECK(set.chooseOut(in) == SlideOut::Sprint);
    in.crouch = true;
    CHECK(set.chooseOut(in) == SlideOut::Sprint); // a sprint stands up out of the crouch
    in.sprint = false;
    in.wantSpeed = 150.f;
    CHECK(set.chooseOut(in) == SlideOut::CrouchWalk);
    in.crouch = false;
    CHECK(set.chooseOut(in) == SlideOut::Walk); // nearer the walk's 180 than the run's 450
    in.wantSpeed = 400.f;
    CHECK(set.chooseOut(in) == SlideOut::Run);
    CHECK(crouchedOut(SlideOut::IdleCrouch));
    CHECK(crouchedOut(SlideOut::CrouchWalk));
    CHECK_FALSE(crouchedOut(SlideOut::Sprint));
    CHECK_FALSE(crouchedOut(SlideOut::IdleStand));

    // With only a run and a stand to get up with.
    SlideSet few;
    few.setOut(SlideOut::Run, set.out(SlideOut::Run));
    few.setOut(SlideOut::IdleStand, set.out(SlideOut::IdleStand));
    SlideInput ask;
    CHECK(few.chooseOut(ask) == SlideOut::IdleStand);
    ask.wantSpeed = 700.f;
    ask.sprint = true;
    CHECK(few.chooseOut(ask) == SlideOut::Run);
    ask.sprint = false;
    CHECK(few.chooseOut(ask) == SlideOut::Run); // a crouch walk: the run before the stand
    ask.crouch = false;
    ask.wantSpeed = 100.f;
    CHECK(few.chooseOut(ask) == SlideOut::Run); // a walk
}

TEST_CASE("Without a get-up to play, the slide hands straight back once it is over") {
    SlideSet set;
    set.setInto(0, intoOf(makeInto(650.f, 0)));
    SlideState s = set.start(700.f, 0);
    for (int i = 0; i < 1200 && s.sliding(); ++i) set.step(s, SlideInput{}, kStep);
    CHECK_FALSE(s.sliding());
    CHECK(s.outFrom < set.tuning().outSpeed);
    CHECK(s.speed == s.outFrom);
}

TEST_CASE("Letting go of the crouch or running into something before the down gets up at the down") {
    const SlideSet set = makeSet();
    SlideState s = set.start(700.f, 1);
    const float down = set.into(3).down;
    SlideInput off;
    off.crouch = false;
    set.step(s, off, kStep);
    CHECK(s.phase == SlidePhase::Into);
    CHECK(s.getUp);
    CHECK(s.speed == 700.f);
    float last = s.clipTime;
    for (int i = 0; i < 120 && s.phase == SlidePhase::Into; ++i) {
        last = s.clipTime;
        set.step(s, off, kStep);
    }
    CHECK(last < down);
    CHECK(s.phase == SlidePhase::Out);
    CHECK(s.out == SlideOut::IdleStand);
    CHECK(s.outFrom == 700.f);

    SlideState b = set.start(700.f, 1);
    SlideInput wall;
    wall.blocked = true;
    set.step(b, wall, kStep);
    CHECK(b.speed == 0.f);
    CHECK(b.phase == SlidePhase::Into);
    for (int i = 0; i < 120 && b.phase == SlidePhase::Into; ++i) set.step(b, SlideInput{}, kStep);
    CHECK(b.phase == SlidePhase::Out);
    CHECK(b.outFrom == 0.f);
    CHECK(b.out == SlideOut::IdleCrouch);
}

TEST_CASE("Getting up, the speed eases from the slide's to the clip's own by the first plant, and is the clip's from there") {
    const SlideSet set = makeSet();
    SlideState s = set.start(700.f, 1);
    SlideInput in;
    in.wantSpeed = 450.f;
    for (int i = 0; i < 60; ++i) set.step(s, in, kStep); // 1 s: down and sliding
    REQUIRE(s.phase == SlidePhase::Down);
    in.crouch = false; // let go: up into a run
    set.step(s, in, kStep);
    REQUIRE(s.phase == SlidePhase::Out);
    CHECK(s.out == SlideOut::Run);
    const SlideOutInfo& o = set.out(SlideOut::Run);
    const float atPlant = slideOutSpeed(o, o.plant);
    CHECK(s.outFrom > atPlant + 100.f);
    float last = s.outFrom;
    for (int i = 0; i < 120 && s.sliding(); ++i) {
        set.step(s, in, kStep);
        if (s.clipTime < o.plant) {
            CHECK(s.speed <= last + 1e-3f);
            CHECK(s.speed >= atPlant - 1e-3f);
        } else {
            CHECK(s.speed == doctest::Approx(slideOutSpeed(o, s.clipTime)));
        }
        last = s.speed;
    }
    CHECK_FALSE(s.sliding());
}

TEST_CASE("A get-up hands back once settled, or exitAfter past being up for a body that wants to move, never past its end") {
    const SlideSet set = makeSet();
    const float exitAfter = set.tuning().exitAfter;
    SlideInput push;
    push.wantSpeed = 300.f;

    // Left still: once settled.
    const SlideOutInfo& still = set.out(SlideOut::IdleCrouch);
    REQUIRE(still.settle > still.up + exitAfter);
    SlideState s = slideToOut(set, SlideInput{});
    REQUIRE(s.out == SlideOut::IdleCrouch);
    float last = 0.f;
    for (int i = 0; i < 120 && s.sliding(); ++i) {
        last = s.clipTime;
        set.step(s, SlideInput{}, kStep);
    }
    CHECK_FALSE(s.sliding());
    CHECK(last < still.settle);
    CHECK(s.clipTime >= still.settle);

    // The stick pushed partway: exitAfter past being up.
    SlideState p = slideToOut(set, SlideInput{});
    for (int i = 0; i < 12; ++i) set.step(p, SlideInput{}, kStep);
    for (int i = 0; i < 120 && p.sliding(); ++i) {
        last = p.clipTime;
        set.step(p, push, kStep);
    }
    CHECK(last < still.up + exitAfter);
    CHECK(p.clipTime >= still.up + exitAfter);
    // Pushed once that is past: at once.
    SlideState late = slideToOut(set, SlideInput{});
    for (int i = 0; i < 120 && late.clipTime < 0.95f; ++i) set.step(late, SlideInput{}, kStep);
    REQUIRE(late.sliding());
    set.step(late, push, kStep);
    CHECK_FALSE(late.sliding());

    // Up into a run, which never settles: exitAfter past being up, the stick let go or not.
    SlideInput run;
    run.crouch = false;
    run.wantSpeed = 450.f;
    SlideState m = slideToOut(set, run);
    REQUIRE(m.out == SlideOut::Run);
    const SlideOutInfo& moving = set.out(SlideOut::Run);
    for (int i = 0; i < 120 && m.sliding(); ++i) {
        last = m.clipTime;
        set.step(m, SlideInput{}, kStep);
    }
    CHECK(last < moving.up + exitAfter);
    CHECK(m.clipTime >= moving.up + exitAfter);

    // Settled before that: the stick pushed hands back once settled.
    SlideSet quick = makeSet();
    quick.setOut(SlideOut::IdleCrouch, outOf(makeOut(200.f, 0.f, 12)));
    const float settled = quick.out(SlideOut::IdleCrouch).settle;
    REQUIRE(settled < quick.out(SlideOut::IdleCrouch).up + exitAfter);
    SlideState q = slideToOut(quick, SlideInput{});
    for (int i = 0; i < 120 && q.sliding(); ++i) {
        last = q.clipTime;
        quick.step(q, push, kStep);
    }
    CHECK(last < settled);
    CHECK(q.clipTime >= settled);

    // Never past the clip's end.
    SlideSet slow = makeSet();
    SlideTuning t;
    t.exitAfter = 100.f;
    slow.setTuning(t);
    SlideState e = slideToOut(slow, run);
    for (int i = 0; i < 200 && e.sliding(); ++i) slow.step(e, run, kStep);
    CHECK_FALSE(e.sliding());
    CHECK(e.clipTime == doctest::Approx(moving.length));
}

TEST_CASE("The Into runs on into the Loop at its end, which wraps; with no Loop the Into's last pose holds") {
    SlideSet set = makeSet();
    SlideTuning t;
    t.friction = 0.f; // never slows
    set.setTuning(t);
    SlideState s = set.start(700.f, 1);
    const SlideIntoInfo& c = set.into(3);
    float last = s.clipTime;
    for (int i = 0; i < 600 && s.clip == s.into; ++i) {
        last = s.clipTime;
        set.step(s, SlideInput{}, kStep);
    }
    REQUIRE(s.clip == kSlideLoop);
    CHECK(s.loop);
    CHECK(s.phase == SlidePhase::Down);
    CHECK(s.clipTime == doctest::Approx(last + kStep - c.length));
    bool wrapped = false;
    for (int i = 0; i < 120; ++i) {
        last = s.clipTime;
        set.step(s, SlideInput{}, kStep);
        CHECK(s.clip == kSlideLoop);
        CHECK(s.clipTime >= 0.f);
        CHECK(s.clipTime < set.loopLength());
        wrapped = wrapped || s.clipTime < last;
    }
    CHECK(wrapped);

    set.setLoop(0.f);
    SlideState h = set.start(700.f, 1);
    for (int i = 0; i < 120; ++i) set.step(h, SlideInput{}, kStep);
    CHECK(h.clip == h.into);
    CHECK_FALSE(h.loop);
    CHECK(h.clipTime == doctest::Approx(c.length));
    CHECK(h.phase == SlidePhase::Down);
}

TEST_CASE("The body turns toward the stick at no more than steerRate until it gets up, and not after") {
    const SlideSet set = makeSet();
    SlideState s = set.start(700.f, 1);
    SlideInput in;
    in.turn = 90.f;
    set.step(s, in, kStep);
    CHECK(s.yawStep == doctest::Approx(set.tuning().steerRate * kStep));
    in.turn = -0.5f;
    set.step(s, in, kStep);
    CHECK(s.yawStep == doctest::Approx(-0.5f));
    in.turn = -90.f;
    set.step(s, in, kStep);
    CHECK(s.yawStep == doctest::Approx(-set.tuning().steerRate * kStep));
    in.crouch = false;
    for (int i = 0; i < 120 && s.phase != SlidePhase::Out; ++i) set.step(s, in, kStep);
    REQUIRE(s.phase == SlidePhase::Out);
    set.step(s, in, kStep);
    CHECK(s.yawStep == 0.f);
}

TEST_CASE("Slides allocate nothing") {
    const SlideSet set = makeSet();
    const bool hooked = ftcore_test::allocHookInstalled();
    const std::uint64_t before = ftcore_test::allocStats().count;
    float sum = 0.f;
    for (int i = 0; i < 20; ++i) {
        SlideState s = set.start(400.f + 20.f * static_cast<float>(i), i % 2);
        SlideInput in;
        in.slope = 0.01f * static_cast<float>(i % 5) - 0.02f;
        in.turn = static_cast<float>(i);
        in.wantSpeed = static_cast<float>(i % 3) * 200.f;
        in.sprint = i % 4 == 0;
        for (int k = 0; k < 300 && s.sliding(); ++k) {
            set.step(s, in, kStep);
            sum += s.speed + s.clipTime + s.yawStep + set.rate(s);
        }
    }
    CHECK(std::isfinite(sum));
    if (hooked) CHECK(ftcore_test::allocStats().count == before);
}
