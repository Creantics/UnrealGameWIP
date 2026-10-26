#include "ftcore/Locomotion.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 60.f;

// The Third's retargeted template set, as FTBuildRig measures it.
Locomotion makeLoco() {
    Locomotion l;
    l.setClip(LocoClip::Idle, {7.57f, 0.f, 1, 0.f});
    l.setClip(LocoClip::Walk, {1.5f, 298.f, 2, 0.1f});
    l.setClip(LocoClip::Run, {1.77f, 509.f, 3, 0.4f});
    l.setClip(LocoClip::JumpStart, {0.87f, 0.f, 1, 0.f});
    l.setClip(LocoClip::Fall, {3.f, 0.f, 1, 0.f});
    l.setClip(LocoClip::Land, {0.87f, 0.f, 1, 0.f});
    l.reset();
    return l;
}

void run(Locomotion& l, float seconds, LocoInput in) {
    in.dt = kDt;
    for (int i = 0, n = static_cast<int>(std::lround(seconds / kDt)); i < n; ++i) l.update(in);
}

float weightSum(const Locomotion& l) {
    float s = 0.f;
    for (std::size_t i = 0; i < l.layerCount(); ++i) s += l.layer(i).weight;
    return s;
}

constexpr LocoClip kGaits[] = {LocoClip::Walk, LocoClip::Run, LocoClip::Sprint};

float gaitWeight(const Locomotion& l) {
    float w = 0.f;
    for (const LocoClip c : kGaits) w += l.weight(c);
    return w;
}

// The planted foot's ground speed under the capsule, from the gait clips' blended rates.
float footSpeed(const Locomotion& l) {
    float s = 0.f;
    for (const LocoClip c : kGaits) s += l.weight(c) * l.rate(c) * l.clip(c).groundSpeed;
    const float w = gaitWeight(l);
    return w > 0.f ? s / w : 0.f;
}

// The largest foot-phase gap between gaits that are playing.
float phaseGap(const Locomotion& l) {
    float gap = 0.f;
    for (const LocoClip c : kGaits) {
        if (l.weight(c) <= 0.f) continue;
        float d = std::fabs(l.footPhase(c, l.time(c)) - l.phase());
        gap = std::fmax(gap, std::fmin(d, 1.f - d));
    }
    return gap;
}

} // namespace

TEST_CASE("locomotion: starts idle and stays idle standing still") {
    Locomotion l = makeLoco();
    run(l, 1.f, {});
    CHECK(l.target() == LocoClip::Idle);
    CHECK(l.layerCount() == 1);
    CHECK(l.weight(LocoClip::Idle) == doctest::Approx(1.f));
    CHECK(l.time(LocoClip::Idle) == doctest::Approx(1.f).epsilon(0.02));
}

TEST_CASE("locomotion: the gaits blend by speed with the feet moving at the capsule's speed") {
    Locomotion l = makeLoco();
    run(l, 1.f, {0.f, 200.f}); // below the walk: walk alone, slowed down
    CHECK(l.target() == LocoClip::Walk);
    CHECK(l.weight(LocoClip::Walk) == doctest::Approx(1.f));
    CHECK(l.rate(LocoClip::Walk) == doctest::Approx(200.f / 298.f));

    run(l, 1.f, {0.f, 330.f}); // between walk (298) and run (509)
    CHECK(l.target() == LocoClip::Walk);
    CHECK(l.weight(LocoClip::Run) == doctest::Approx(32.f / 211.f).epsilon(0.01));
    CHECK(gaitWeight(l) == doctest::Approx(1.f));
    CHECK(footSpeed(l) == doctest::Approx(330.f));
    CHECK(phaseGap(l) < 1e-3f);

    run(l, 1.f, {0.f, 700.f}); // above the run: run alone, sped up
    CHECK(l.target() == LocoClip::Run);
    CHECK(l.weight(LocoClip::Run) == doctest::Approx(1.f));
    CHECK(l.rate(LocoClip::Run) == doctest::Approx(700.f / 509.f));
    CHECK(footSpeed(l) == doctest::Approx(700.f));
}

TEST_CASE("locomotion: walk, run and sprint share one stride") {
    Locomotion l = makeLoco();
    l.setClip(LocoClip::Sprint, {1.2f, 646.f, 2, 0.3f});
    l.reset();
    run(l, 1.f, {0.f, 580.f}); // between run (509) and sprint (646), nearer the sprint
    CHECK(l.target() == LocoClip::Sprint);
    CHECK(l.weight(LocoClip::Walk) == 0.f);
    CHECK(l.weight(LocoClip::Sprint) == doctest::Approx(71.f / 137.f).epsilon(0.01));
    CHECK(footSpeed(l) == doctest::Approx(580.f));
    CHECK(phaseGap(l) < 1e-3f);

    run(l, 1.f, {0.f, 700.f});
    CHECK(l.target() == LocoClip::Sprint);
    CHECK(l.weight(LocoClip::Sprint) == doctest::Approx(1.f));
    CHECK(l.rate(LocoClip::Sprint) == doctest::Approx(700.f / 646.f));
    CHECK(l.pitch() == doctest::Approx(l.tuning().runPitch).epsilon(0.01));
}

TEST_CASE("locomotion: on a speed ramp the blend moves smoothly and the lead gait changes once") {
    Locomotion l = makeLoco();
    run(l, 0.5f, {0.f, 330.f});
    int switches = 0;
    LocoClip last = l.target();
    float run0 = l.weight(LocoClip::Run);
    for (int i = 0; i <= 120; ++i) { // 330 -> 700 over two seconds
        l.update({kDt, 330.f + 370.f * static_cast<float>(i) / 120.f});
        switches += l.target() != last;
        last = l.target();
        const float runW = l.weight(LocoClip::Run);
        CHECK(runW >= run0 - 1e-5f);
        CHECK(runW - run0 < 0.05f);
        run0 = runW;
        CHECK(footSpeed(l) == doctest::Approx(330.f + 370.f * static_cast<float>(i) / 120.f));
        CHECK(phaseGap(l) < 1e-3f);
        CHECK(weightSum(l) == doctest::Approx(1.f));
    }
    CHECK(switches == 1);
    CHECK(last == LocoClip::Run);
}

TEST_CASE("locomotion: a gait whose loop holds several strides plays all of them") {
    Locomotion l = makeLoco();
    l.setClip(LocoClip::Run, {2.5f, 462.f, 4, 0.01f});
    l.reset();
    float lo = 1e9f, hi = -1e9f;
    run(l, 0.5f, {0.f, 462.f});
    for (int i = 0; i < 240; ++i) {
        const float before = l.time(LocoClip::Run);
        l.update({kDt, 462.f});
        float step = l.time(LocoClip::Run) - before;
        if (step < 0.f) step += 2.5f;
        CHECK(step == doctest::Approx(kDt).epsilon(0.01)); // no jumps back to the loop's first stride
        lo = std::fmin(lo, l.time(LocoClip::Run));
        hi = std::fmax(hi, l.time(LocoClip::Run));
    }
    CHECK(lo < 0.1f);
    CHECK(hi > 2.4f);
    CHECK(l.rate(LocoClip::Run) == doctest::Approx(1.f));
}

TEST_CASE("locomotion: the gaits start from a left plant") {
    Locomotion l = makeLoco();
    l.update({kDt, 330.f});
    CHECK(l.phase() == doctest::Approx(l.rate(LocoClip::Walk) * kDt * 2.f / 1.5f));
}

TEST_CASE("locomotion: foot phase and time invert each other") {
    Locomotion l = makeLoco();
    for (float p : {0.f, 0.25f, 0.5f, 0.9f}) {
        CHECK(l.footPhase(LocoClip::Run, l.timeAtPhase(LocoClip::Run, p)) == doctest::Approx(p).epsilon(1e-4));
        CHECK(l.footPhase(LocoClip::Walk, l.timeAtPhase(LocoClip::Walk, p)) == doctest::Approx(p).epsilon(1e-4));
    }
    CHECK(l.footPhase(LocoClip::Walk, 0.1f * 1.5f) == doctest::Approx(0.f)); // the left plant
}

TEST_CASE("locomotion: a ground crossfade is mostly done in three time constants") {
    Locomotion l = makeLoco();
    l.update({kDt, 330.f});
    run(l, 3.f * l.tuning().gaitFade - kDt, {0.f, 330.f});
    CHECK(gaitWeight(l) > 0.93f);
    CHECK(weightSum(l) == doctest::Approx(1.f));
}

TEST_CASE("locomotion: a jump plays start, fall and landing, then hands back to the gait") {
    Locomotion l = makeLoco();
    run(l, 1.f, {0.f, 700.f});
    // Take off: up at 520, gravity 980.
    float vz = 520.f, t = 0.f;
    l.update({kDt, 700.f, 0.f, true, vz});
    CHECK(l.state() == LocoState::Rising);
    CHECK(l.target() == LocoClip::JumpStart);
    CHECK(l.time(LocoClip::JumpStart) < 0.05f);
    bool fell = false;
    while (t < 1.06f) {
        vz -= 980.f * kDt;
        t += kDt;
        l.update({kDt, 700.f, 0.f, true, vz});
        fell = fell || l.state() == LocoState::Falling;
    }
    CHECK(fell);
    CHECK(l.target() == LocoClip::Fall);
    // Only a standing landing: at a run it would slide, so the run carries on.
    l.update({kDt, 700.f, 0.f, false, 0.f});
    CHECK(l.state() == LocoState::Ground);
    CHECK(l.target() == LocoClip::Run);
}

TEST_CASE("locomotion: a moving jump takes off and lands with the moving clips") {
    Locomotion l = makeLoco();
    l.setClip(LocoClip::JumpMoving, {0.6f, 460.f, 1, 0.f});
    l.setClip(LocoClip::LandMoving, {0.9f, 560.f, 1, 0.f});
    l.reset();
    run(l, 1.f, {0.f, 700.f});
    l.update({kDt, 700.f, 0.f, true, 520.f});
    CHECK(l.state() == LocoState::Rising);
    CHECK(l.target() == LocoClip::JumpMoving);
    CHECK(l.rate(LocoClip::JumpMoving) == doctest::Approx(700.f / 460.f));
    run(l, 0.6f, {0.f, 700.f, 0.f, true, -200.f});
    CHECK(l.target() == LocoClip::Fall);
    l.update({kDt, 700.f, 0.f, false, 0.f});
    CHECK(l.state() == LocoState::Landing);
    CHECK(l.target() == LocoClip::LandMoving);
    CHECK(l.rate(LocoClip::LandMoving) == doctest::Approx(700.f / 560.f));
    run(l, 0.15f, {0.f, 700.f});
    CHECK(l.state() == LocoState::Landing);
    run(l, 0.1f, {0.f, 700.f});
    CHECK(l.state() == LocoState::Ground); // moving: back to the run after landHoldMoving
    CHECK(l.target() == LocoClip::Run);

    // The landing's rate stays within its limits.
    l.update({kDt, 2000.f, 0.f, true, 520.f});
    CHECK(l.rate(LocoClip::JumpMoving) == doctest::Approx(l.tuning().maxLandRate));
    run(l, 0.6f, {0.f, 2000.f, 0.f, true, -200.f});
    l.update({kDt, 2000.f, 0.f, false, 0.f});
    CHECK(l.target() == LocoClip::LandMoving);
    CHECK(l.rate(LocoClip::LandMoving) == doctest::Approx(l.tuning().maxLandRate));

    // Standing still, the standing clips.
    run(l, 2.f, {});
    l.update({kDt, 0.f, 0.f, true, 520.f});
    CHECK(l.target() == LocoClip::JumpStart);
    run(l, 0.8f, {0.f, 0.f, 0.f, true, -300.f});
    l.update({kDt, 0.f, 0.f, false, 0.f});
    CHECK(l.target() == LocoClip::Land);
}

TEST_CASE("locomotion: landing standing still plays the landing out") {
    Locomotion l = makeLoco();
    l.update({kDt, 0.f, 0.f, true, 500.f});
    run(l, 0.8f, {0.f, 0.f, 0.f, true, -300.f});
    l.update({kDt, 0.f, 0.f, false, 0.f});
    CHECK(l.state() == LocoState::Landing);
    run(l, 0.6f, {});
    CHECK(l.state() == LocoState::Landing);
    run(l, 0.3f, {});
    CHECK(l.state() == LocoState::Ground);
    CHECK(l.target() == LocoClip::Idle);
}

TEST_CASE("locomotion: a step down stays on the ground clips; a short hop skips the landing") {
    Locomotion l = makeLoco();
    run(l, 0.5f, {0.f, 330.f});
    run(l, 0.08f, {0.f, 330.f, 0.f, true, -200.f});
    CHECK(l.state() == LocoState::Ground);
    CHECK(l.target() == LocoClip::Walk);
    l.update({kDt, 330.f});

    run(l, 0.15f, {0.f, 330.f, 0.f, true, 100.f});
    CHECK(l.state() == LocoState::Rising);
    l.update({kDt, 330.f});
    CHECK(l.state() == LocoState::Ground);

    run(l, 0.3f, {0.f, 330.f, 0.f, true, -400.f}); // walked off a ledge
    CHECK(l.state() == LocoState::Falling);
}

TEST_CASE("locomotion: the body banks and leads into turns, more at speed, within limits") {
    Locomotion l = makeLoco();
    run(l, 1.f, {0.f, 700.f, 180.f});
    CHECK(l.lean() > 3.f);
    CHECK(l.lean() <= l.tuning().maxLean + 1e-4f);
    CHECK(l.twist() > 5.f);
    CHECK(l.pitch() == doctest::Approx(l.tuning().runPitch).epsilon(0.01));
    const float fastLean = l.lean();

    Locomotion slow = makeLoco();
    run(slow, 1.f, {0.f, 200.f, 180.f});
    CHECK(slow.lean() < fastLean);
    CHECK(slow.pitch() == doctest::Approx(0.f));

    run(l, 1.f, {0.f, 700.f, -2000.f});
    CHECK(l.lean() == doctest::Approx(-l.tuning().maxLean).epsilon(0.01));
    CHECK(l.twist() == doctest::Approx(-l.tuning().maxTwist).epsilon(0.01));
}

TEST_CASE("locomotion: a body with only idle and walk walks at any speed, capped in rate") {
    Locomotion l;
    l.setClip(LocoClip::Idle, {2.f, 0.f, 1, 0.f});
    l.setClip(LocoClip::Walk, {1.07f, 207.f, 1, 0.f});
    l.reset();
    run(l, 1.f, {0.f, 700.f});
    CHECK(l.target() == LocoClip::Walk);
    CHECK(l.rate(LocoClip::Walk) == doctest::Approx(l.tuning().maxRate));
    l.update({kDt, 700.f, 0.f, true, 500.f}); // no jump clips: stays on the gait
    CHECK(l.state() == LocoState::Falling);
    CHECK(l.target() == LocoClip::Walk);
}

namespace {

// A synthetic gait: `cycles` strides over `length` s, feet half a stride apart, each planted for
// 60% of its stride and sliding back under the root at `speed`, then lifting 15 cm to swing
// forward. `rootSpeed` adds root motion (0 for an in-place clip).
// `dip` lowers the toe to the ground for a moment mid-swing, as real clips do.
void makeGait(GaitFrame* frames, int n, float length, int cycles, float speed, float rootSpeed, float leftPlantAt, bool dip = false) {
    const float stride = speed * length / static_cast<float>(cycles);
    for (int k = 0; k < n; ++k) {
        const float t = length * static_cast<float>(k) / static_cast<float>(n - 1);
        const float rootX = rootSpeed * t;
        frames[k].root = {rootX, 0.f, 90.f};
        for (int side = 0; side < 2; ++side) {
            float p = static_cast<float>(cycles) * t / length - leftPlantAt * static_cast<float>(cycles) + (side ? 0.5f : 0.f);
            p -= std::floor(p);
            float x, z;
            if (p < 0.6f) { // planted: from +stride*0.3 back to -stride*0.3
                x = stride * (0.3f - p);
                z = 0.f;
            } else { // swing forward
                const float s = (p - 0.6f) / 0.4f;
                x = stride * (-0.3f + 0.6f * s);
                z = dip && std::fabs(s - 0.5f) < 0.08f ? 1.f : 15.f * std::sin(3.14159265f * s);
            }
            LocoPoint& foot = side ? frames[k].right : frames[k].left;
            foot = {rootX + x, side ? 10.f : -10.f, 90.f - 90.f + z};
        }
    }
}

} // namespace

TEST_CASE("locomotion: measureGait reads ground speed, strides and the left plant") {
    static GaitFrame frames[91];
    makeGait(frames, 91, 1.5f, 2, 300.f, 0.f, 0.1f);
    LocoClipInfo g = measureGait(frames, 91, 1.5f);
    CHECK(g.length == doctest::Approx(1.5f));
    CHECK(g.groundSpeed == doctest::Approx(300.f).epsilon(0.02));
    CHECK(g.cycles == 2);
    CHECK(g.leftPlant == doctest::Approx(0.1f).epsilon(0.02));

    // Root motion: the same feet relative to the root measure the same.
    makeGait(frames, 91, 1.5f, 2, 300.f, 300.f, 0.1f);
    g = measureGait(frames, 91, 1.5f);
    CHECK(g.groundSpeed == doctest::Approx(300.f).epsilon(0.02));
    CHECK(g.cycles == 2);

    // The planted feet win over the root's travel when the two disagree (mocap feet slip against the
    // root): it's the feet that must stand still.
    makeGait(frames, 91, 1.5f, 2, 300.f, 315.f, 0.1f);
    g = measureGait(frames, 91, 1.5f);
    CHECK(g.groundSpeed == doctest::Approx(300.f).epsilon(0.02));
    CHECK(g.footSpeed == doctest::Approx(300.f).epsilon(0.02));
    CHECK(g.footSide == doctest::Approx(0.f).epsilon(0.01));

    // Planted feet that sweep outward as they slide back (a leg swinging in a turned plane) read
    // their sideways speed.
    makeGait(frames, 91, 1.5f, 2, 300.f, 0.f, 0.1f);
    for (GaitFrame& f : frames) {
        if (f.left.z == 0.f) f.left.y -= 0.4f * (f.left.x - f.root.x);
        if (f.right.z == 0.f) f.right.y += 0.4f * (f.right.x - f.root.x);
    }
    g = measureGait(frames, 91, 1.5f);
    CHECK(g.footSide == doctest::Approx(120.f).epsilon(0.05));

    // A plant across the loop's seam counts once.
    makeGait(frames, 91, 1.77f, 3, 500.f, 0.f, 0.9f);
    g = measureGait(frames, 91, 1.77f);
    CHECK(g.cycles == 3);
    CHECK(g.groundSpeed == doctest::Approx(500.f).epsilon(0.02));

    // A plant starting right on the seam (the last frame, which is the first again) counts once.
    makeGait(frames, 91, 1.5f, 2, 300.f, 0.f, 0.f);
    g = measureGait(frames, 91, 1.5f);
    CHECK(g.cycles == 2);
    CHECK(g.leftPlant == doctest::Approx(0.f));

    // A toe skimming the ground mid-swing isn't a plant.
    makeGait(frames, 91, 1.5f, 2, 300.f, 0.f, 0.1f, true);
    g = measureGait(frames, 91, 1.5f);
    CHECK(g.cycles == 2);
    CHECK(g.groundSpeed == doctest::Approx(300.f).epsilon(0.02));
    CHECK(g.leftPlant == doctest::Approx(0.1f).epsilon(0.02));

    // Feet that never lift (idle): no gait.
    for (GaitFrame& f : frames) f.left.z = f.right.z = 0.f;
    g = measureGait(frames, 91, 2.f);
    CHECK(g.groundSpeed == doctest::Approx(0.f));
    CHECK(g.cycles == 1);
    CHECK(measureGait(nullptr, 0, 1.f).groundSpeed == 0.f);
}

TEST_CASE("locomotion: update does not allocate") {
    Locomotion l = makeLoco();
    const auto before = ftcore_test::allocStats().count;
    for (int i = 0; i < 600; ++i) {
        const float s = static_cast<float>(i % 200) * 4.f;
        l.update({kDt, s, 90.f, i % 150 > 120, i % 150 > 135 ? -300.f : 400.f});
    }
    CHECK(ftcore_test::allocStats().count == before);
}
