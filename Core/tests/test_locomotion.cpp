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
    l.tuning().runPitch = 12.f; // the game's run leans from its clip alone; a lean here keeps the run's share testable
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
    CHECK(l.pitch() == doctest::Approx(l.tuning().runPitch + l.tuning().sprintPitch).epsilon(0.01));
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

TEST_CASE("locomotion: the air pose holds the limbs in the air, the arms into the landing") {
    Locomotion l = makeLoco();
    run(l, 1.f, {0.f, 330.f});
    CHECK(l.limbs().arms == 0.f); // no air pose: nothing over the limbs
    l.update({kDt, 330.f, 0.f, true, 500.f});
    CHECK(l.limbs().legs == 0.f);

    l.setClip(LocoClip::AirPose, {0.5f, 0.f, 1, 0.f});
    l.reset();
    run(l, 1.f, {0.f, 330.f});
    CHECK(l.limbs().arms == 0.f);
    CHECK(l.limbs().legs == 0.f);
    // Take off: in by 0.15 s, held at the clip's end.
    l.update({kDt, 330.f, 0.f, true, 500.f});
    CHECK(l.limbs().legs > 0.f);
    CHECK(l.limbs().legs < 0.5f);
    run(l, 0.15f, {0.f, 330.f, 0.f, true, 300.f});
    CHECK(l.limbs().arms >= 0.95f);
    CHECK(l.limbs().legs >= 0.95f);
    CHECK(l.limbs().time == doctest::Approx(0.5f));
    CHECK(l.limbs().clip == LocoClip::AirPose);
    run(l, 0.6f, {0.f, 0.f, 0.f, true, -300.f});

    // Touchdown: the legs ease out into the landing, the arms hold for landArms, then ease out too.
    l.update({kDt, 0.f, 0.f, false, 0.f});
    CHECK(l.state() == LocoState::Landing);
    run(l, 0.15f, {});
    CHECK(l.limbs().legs <= 0.05f);
    CHECK(l.limbs().arms == doctest::Approx(1.f));
    run(l, l.tuning().landArms - 0.15f, {});
    CHECK(l.limbs().arms > 0.5f);
    run(l, 0.16f, {});
    CHECK(l.limbs().arms <= 0.05f);
    CHECK(l.limbs().legs == 0.f);

    // A moving landing hands back to the run sooner: the arms ease out with it.
    run(l, 1.f, {0.f, 700.f});
    l.update({kDt, 700.f, 0.f, true, 520.f});
    run(l, 0.6f, {0.f, 700.f, 0.f, true, -200.f});
    CHECK(l.limbs().arms >= 0.95f);
    l.update({kDt, 700.f, 0.f, false, 0.f});
    run(l, 0.15f, {0.f, 700.f});
    CHECK(l.limbs().legs <= 0.05f);
    run(l, l.tuning().landArms + 0.2f, {0.f, 700.f});
    CHECK(l.state() == LocoState::Ground);
    CHECK(l.limbs().arms == 0.f);
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

TEST_CASE("locomotion: the head stays level against the lean and the bank") {
    Locomotion l = makeLoco();
    run(l, 1.f, {0.f, 509.f, 180.f});
    CHECK(l.pitch() == doctest::Approx(l.tuning().runPitch).epsilon(0.01));
    CHECK(l.headPitch() == doctest::Approx(-l.pitch() * l.tuning().headLevel));
    CHECK(l.headRoll() == doctest::Approx(-l.lean() * l.tuning().headLevelRoll));
    CHECK(l.headYaw() == 0.f); // no turn clip, so no look ahead of the chest
    run(l, 1.f, {0.f, 0.f});
    CHECK(std::fabs(l.headPitch()) < 0.01f);
    CHECK(std::fabs(l.headRoll()) < 0.01f);
}

TEST_CASE("locomotion: the lean leads a start, leans back into a stop and bobs as it stands") {
    Locomotion l = makeLoco();
    const LocoTuning& t = l.tuning();
    LocoInput in{0.f, 100.f};
    in.wantSpeed = 509.f;
    run(l, 0.5f, in);
    CHECK(l.pitch() == doctest::Approx(t.startPitch * 409.f / 509.f).epsilon(0.01));
    CHECK(l.headPitch() == doctest::Approx(-l.pitch() * t.headLevel));

    in.groundSpeed = 509.f;
    run(l, 1.f, in); // up to speed: just the run's lean
    CHECK(l.pitch() == doctest::Approx(t.runPitch).epsilon(0.01));

    in.wantSpeed = 0.f;
    run(l, 1.f, in); // braking at full speed leans back, the run's lean gone
    CHECK(l.pitch() == doctest::Approx(-t.stopPitch).epsilon(0.01));
    in.groundSpeed = 250.f;
    run(l, 1.f, in);
    CHECK(l.pitch() == doctest::Approx(-t.stopPitch * 250.f / 509.f).epsilon(0.01));

    in.groundSpeed = 0.f;
    float peak = 0.f;
    for (int i = 0; i < 30; ++i) {
        run(l, kDt, in);
        peak = std::fmax(peak, l.pitch());
    }
    CHECK(peak > 0.5f * t.settlePitch);
    run(l, 1.f, in);
    CHECK(std::fabs(l.pitch()) < 0.01f);

    // Speed with no heading said (a follower without one): no start or stop lean, no bob.
    Locomotion plain = makeLoco();
    run(plain, 1.f, {0.f, 250.f});
    CHECK(std::fabs(plain.pitch()) < 0.01f);
    peak = 0.f;
    for (int i = 0; i < 30; ++i) {
        run(plain, kDt, {0.f, 0.f});
        peak = std::fmax(peak, std::fabs(plain.pitch()));
    }
    CHECK(peak < 0.01f);
}

TEST_CASE("locomotion: the body tucks in the air and dips into the landing") {
    Locomotion l = makeLoco();
    run(l, 1.f, {0.f, 509.f});
    run(l, 0.2f, {0.f, 509.f, 0.f, true, 300.f});
    CHECK(l.state() == LocoState::Rising);
    run(l, 0.3f, {0.f, 509.f, 0.f, true, -300.f});
    CHECK(l.pitch() == doctest::Approx(l.tuning().airPitch).epsilon(0.05));
    float peak = 0.f;
    for (int i = 0; i < 12; ++i) {
        run(l, kDt, {0.f, 0.f});
        peak = std::fmax(peak, l.pitch());
    }
    CHECK(peak > l.tuning().airPitch);
    run(l, 1.f, {0.f, 0.f});
    CHECK(std::fabs(l.pitch()) < 0.01f);
}

TEST_CASE("locomotion: the elbows are held bent at a run, not at the walk or in the air") {
    Locomotion l = makeLoco();
    run(l, 1.f, {0.f, 509.f});
    CHECK(l.elbowHold() == doctest::Approx(l.tuning().elbowHold).epsilon(0.01));
    run(l, 1.f, {0.f, 298.f});
    CHECK(l.elbowHold() < 0.01f);
    run(l, 1.f, {0.f, 509.f});
    run(l, 0.6f, {0.f, 509.f, 0.f, true, -300.f});
    CHECK(l.elbowHold() < 0.05f);
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
// A foot at foot phase p (0 at its plant) of a `stride` cm stride, relative to the root along x.
void stepFoot(float p, float stride, bool dip, float& x, float& z) {
    p -= std::floor(p);
    if (p < 0.6f) { // planted: from +stride*0.3 back to -stride*0.3
        x = stride * (0.3f - p);
        z = 0.f;
    } else { // swing forward
        const float s = (p - 0.6f) / 0.4f;
        x = stride * (-0.3f + 0.6f * s);
        z = dip && std::fabs(s - 0.5f) < 0.08f ? 1.f : 15.f * std::sin(3.14159265f * s);
    }
}

void makeGait(GaitFrame* frames, int n, float length, int cycles, float speed, float rootSpeed, float leftPlantAt, bool dip = false) {
    const float stride = speed * length / static_cast<float>(cycles);
    for (int k = 0; k < n; ++k) {
        const float t = length * static_cast<float>(k) / static_cast<float>(n - 1);
        const float rootX = rootSpeed * t;
        frames[k].root = {rootX, 0.f, 90.f};
        for (int side = 0; side < 2; ++side) {
            const float p = static_cast<float>(cycles) * t / length - leftPlantAt * static_cast<float>(cycles) + (side ? 0.5f : 0.f);
            float x, z;
            stepFoot(p, stride, dip, x, z);
            LocoPoint& foot = side ? frames[k].right : frames[k].left;
            foot = {rootX + x, side ? 10.f : -10.f, z};
        }
    }
}

// A synthetic start or stop: the root comes path(t) cm along x and the feet step as in makeGait, at
// foot phase phase0 + path / stride (so the planted foot stands still), standing where the root does.
template <class Path>
void makeMove(GaitFrame* frames, int n, float length, float stride, float phase0, Path path) {
    for (int k = 0; k < n; ++k) {
        const float d = path(length * static_cast<float>(k) / static_cast<float>(n - 1));
        frames[k].root = {d, 0.f, 90.f};
        for (int side = 0; side < 2; ++side) {
            float x, z;
            stepFoot(phase0 + d / stride + (side ? 0.5f : 0.f), stride, false, x, z);
            LocoPoint& foot = side ? frames[k].right : frames[k].left;
            foot = {d + x, side ? 10.f : -10.f, z};
        }
    }
}

// The run below: 450 cm/s, a stride (left plant to left plant) every 0.6 s.
constexpr float kStride = 270.f;
// Its start speeds up evenly to 450 by 0.8 s (180 cm), then holds it to the end, 1.3 s; the left
// foot lifts first.
float startPath(float t) { return t < 0.8f ? 281.25f * t * t : 180.f + 450.f * (t - 0.8f); }
constexpr float kStartPhase = 0.75f;
// Its stops slow evenly from 450 to a stand at 1 s (225 cm), then settle to 2 s, the left foot
// planted last (phase 0.3 at the end) or the right (the left at 0.8).
float stopPath(float t) { return t < 1.f ? 450.f * t - 225.f * t * t : 225.f; }
constexpr float kStopLeftPhase = 0.3f - 225.f / kStride;
constexpr float kStopRightPhase = 0.8f - 225.f / kStride;

// A clip's root path at t, from its measured track.
float trackPath(const LocoClipInfo& i, float t) {
    const float f = std::fmin(std::fmax(t / i.moveTime, 0.f), 1.f) * static_cast<float>(kMoveSamples - 1);
    const std::size_t k = std::min(static_cast<std::size_t>(f), kMoveSamples - 2);
    return i.path[k] + (i.path[k + 1] - i.path[k]) * (f - static_cast<float>(k));
}

// How far apart two foot phases are, around the loop.
float phaseApart(float a, float b) {
    const float d = std::fabs(a - b) - std::floor(std::fabs(a - b));
    return std::fmin(d, 1.f - d);
}

Locomotion makeMoveLoco() {
    static GaitFrame frames[121];
    Locomotion l;
    l.setClip(LocoClip::Idle, {7.57f, 0.f, 1, 0.f});
    makeGait(frames, 37, 0.6f, 1, 450.f, 450.f, 0.f);
    l.setClip(LocoClip::Run, measureGait(frames, 37, 0.6f));
    makeMove(frames, 79, 1.3f, kStride, kStartPhase, startPath);
    l.setClip(LocoClip::RunStart, measureMove(frames, 79, 1.3f));
    makeMove(frames, 121, 2.f, kStride, kStopLeftPhase, stopPath);
    l.setClip(LocoClip::RunStopLeft, measureMove(frames, 121, 2.f));
    makeMove(frames, 121, 2.f, kStride, kStopRightPhase, stopPath);
    l.setClip(LocoClip::RunStopRight, measureMove(frames, 121, 2.f));
    l.reset();
    return l;
}

// The lead's capsule: speeding up at 1024 cm/s^2 while heading off, braking at 1000 to a stop.
struct Capsule {
    float speed = 0.f;
    LocoInput step(float want) {
        speed = want > 0.f ? std::fmin(speed + 1024.f * kDt, want) : std::fmax(speed - 1000.f * kDt, 0.f);
        return {kDt, speed, 0.f, false, 0.f, want, want > 0.f ? -1.f : speed * speed / 2000.f};
    }
};

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

TEST_CASE("locomotion: measureMove reads the root's travel and when it stops") {
    static GaitFrame frames[121];
    makeMove(frames, 121, 2.f, kStride, kStopLeftPhase, stopPath);
    LocoClipInfo m = measureMove(frames, 121, 2.f);
    CHECK(m.length == doctest::Approx(2.f));
    // The root ends 225 cm along, within kMoveStill of it from 0.953 s on.
    CHECK(m.moveTime == doctest::Approx(0.97f).epsilon(0.02));
    CHECK(m.travel == doctest::Approx(225.f).epsilon(0.01));
    CHECK(m.path[0] == 0.f);
    for (std::size_t k = 0; k + 1 < kMoveSamples; ++k) CHECK(m.path[k + 1] >= m.path[k]);
    CHECK(trackPath(m, 0.5f) == doctest::Approx(stopPath(0.5f)).epsilon(0.01));
    // The feet relative to the root: the left planted at the end, half a stride behind where it plants.
    CHECK(m.feet[kMoveSamples - 1].left.z == doctest::Approx(-90.f));
    CHECK(std::fabs(m.feet[kMoveSamples - 1].left.x) < 1.f);

    // A start moves to its end.
    makeMove(frames, 79, 1.3f, kStride, kStartPhase, startPath);
    m = measureMove(frames, 79, 1.3f);
    CHECK(m.moveTime == doctest::Approx(1.3f));
    CHECK(m.travel == doctest::Approx(startPath(1.3f)).epsilon(0.01));

    // A root that stays put has the whole clip as its track and no travel; measureGait measures the move too.
    makeGait(frames, 91, 1.5f, 2, 300.f, 0.f, 0.1f);
    m = measureGait(frames, 91, 1.5f);
    CHECK(m.moveTime == doctest::Approx(1.5f));
    CHECK(m.travel == 0.f);
    CHECK(m.groundSpeed == doctest::Approx(300.f).epsilon(0.02));
    CHECK(measureMove(nullptr, 0, 1.f).moveTime == 0.f);
}

TEST_CASE("locomotion: heading off plays the start at the distance covered, then the gait in step") {
    Locomotion l = makeMoveLoco();
    const LocoClipInfo& s = l.clip(LocoClip::RunStart);
    Capsule c;
    float moved = 0.f, last = 0.f;
    bool starting = false, handed = false;
    for (int i = 0; i < 120 && !handed; ++i) {
        const LocoInput in = c.step(450.f);
        moved += in.groundSpeed * kDt;
        l.update(in);
        if (l.state() != LocoState::Starting) {
            handed = starting;
            continue;
        }
        CHECK(l.target() == LocoClip::RunStart);
        const float t = l.time(LocoClip::RunStart);
        if (starting) {
            CHECK(t >= last);
            CHECK(t - last <= kDt * l.tuning().moveRate + 1e-4f);
            // At the point the capsule has come to, unless held back by the rate cap.
            if (t - last < kDt * l.tuning().moveRate * 0.99f) CHECK(std::fabs(trackPath(s, t) - moved) < 2.f);
        } else {
            CHECK(t == 0.f);
        }
        starting = true;
        last = t;
    }
    REQUIRE(handed);
    CHECK(l.state() == LocoState::Ground);
    CHECK(l.target() == LocoClip::Run);
    // The run picks up the start's foot phase where it handed over (and has stepped one frame on).
    const float startPhase = kStartPhase + trackPath(s, last) / kStride;
    CHECK(phaseApart(l.phase(), startPhase + kDt * 450.f / kStride) < 0.04f);
    CHECK(phaseApart(l.footPhase(LocoClip::Run, l.time(LocoClip::Run)), l.phase()) < 0.01f);
}

TEST_CASE("locomotion: braking plays the stop on the matching foot at the distance left, then idles") {
    LocoClip picked[2] = {LocoClip::Count, LocoClip::Count};
    for (int trial = 0; trial < 2; ++trial) {
        Locomotion l = makeMoveLoco();
        Capsule c;
        // Run on, for half a stride more the second time.
        for (int i = 0; i < 90 + trial * 18; ++i) l.update(c.step(450.f));
        REQUIRE(l.target() == LocoClip::Run);
        float runPhase = l.phase();
        bool stopping = false, settled = false;
        float last = 0.f;
        for (int i = 0; i < 240; ++i) {
            const LocoInput in = c.step(0.f);
            l.update(in);
            if (l.state() != LocoState::Stopping) {
                if (stopping) break;
                // Not yet braking for stopDelay: the run plays on.
                CHECK(l.target() == LocoClip::Run);
                CHECK(static_cast<float>(i + 1) * kDt < l.tuning().stopDelay + kDt);
                runPhase = l.phase();
                continue;
            }
            const LocoClip stop = l.target();
            const LocoClipInfo& s = l.clip(stop);
            const float t = l.time(stop);
            if (!stopping) {
                picked[trial] = stop;
                // Its feet are the nearer to the run's: both step with the distance covered, so how far
                // apart they are is set by where the capsule stops, and the feet planting last halves it.
                const float run = runPhase + kDt * 450.f / kStride;
                const float mine = (stop == LocoClip::RunStopLeft ? kStopLeftPhase : kStopRightPhase) + trackPath(s, t) / kStride;
                CHECK(phaseApart(mine, run) <= 0.25f);
                CHECK(phaseApart(mine, run) <= phaseApart(mine + 0.5f, run));
            } else {
                CHECK(t >= last);
                if (c.speed > 0.f && t - last < kDt * l.tuning().moveRate * 0.99f) {
                    // At the point whose remaining travel is the capsule's.
                    CHECK(std::fabs(s.travel - trackPath(s, t) - in.stopDistance) < 2.f);
                }
            }
            // Once the capsule stands, the clip soon stands too.
            if (c.speed == 0.f && t >= s.moveTime) settled = true;
            stopping = true;
            last = t;
            runPhase = l.phase();
        }
        CHECK(stopping);
        CHECK(settled);
        // Settled, it hands over to idle near its end.
        CHECK(l.state() == LocoState::Ground);
        CHECK(l.target() == LocoClip::Idle);
        CHECK(last >= l.clip(picked[trial]).length - l.tuning().stopSettle - 2.f * kDt);
    }
    // Half a stride apart, the other foot.
    CHECK(picked[0] != picked[1]);
}

TEST_CASE("locomotion: heading off again mid-stop goes back to the gait in step") {
    Locomotion l = makeMoveLoco();
    Capsule c;
    for (int i = 0; i < 90; ++i) l.update(c.step(450.f));
    for (int i = 0; i < 12; ++i) l.update(c.step(0.f));
    REQUIRE(l.state() == LocoState::Stopping);
    const LocoClip stop = l.target();
    const float t = l.time(stop);
    const float phase0 = stop == LocoClip::RunStopLeft ? kStopLeftPhase : kStopRightPhase;
    l.update(c.step(450.f));
    CHECK(l.state() == LocoState::Ground);
    CHECK(l.target() == LocoClip::Run);
    CHECK(phaseApart(l.phase(), phase0 + trackPath(l.clip(stop), t) / kStride) < 0.08f);
}

TEST_CASE("locomotion: letting go mid-start stops; a blocked or unknown heading doesn't start; a jump leaves the start") {
    Locomotion l = makeMoveLoco();
    Capsule c;
    for (int i = 0; i < 20; ++i) l.update(c.step(450.f));
    REQUIRE(l.state() == LocoState::Starting);
    // A frame without input (a dropped frame) plays the start on; letting go for stopDelay stops.
    l.update(c.step(0.f));
    CHECK(l.state() == LocoState::Starting);
    l.update(c.step(450.f));
    CHECK(l.state() == LocoState::Starting);
    int frames = 0;
    while (l.state() == LocoState::Starting && frames < 10) {
        l.update(c.step(0.f));
        ++frames;
    }
    CHECK(l.state() == LocoState::Stopping);
    CHECK(static_cast<float>(frames) * kDt >= l.tuning().stopDelay - 1e-4f);
    CHECK(static_cast<float>(frames - 1) * kDt < l.tuning().stopDelay);

    // The same on the run: a dropped frame keeps running.
    l = makeMoveLoco();
    c = Capsule{};
    for (int i = 0; i < 90; ++i) l.update(c.step(450.f));
    REQUIRE(l.target() == LocoClip::Run);
    l.update(c.step(0.f));
    l.update(c.step(450.f));
    CHECK(l.state() == LocoState::Ground);
    CHECK(l.target() == LocoClip::Run);

    // Pressing into a wall: heading off, but the capsule doesn't move.
    l = makeMoveLoco();
    run(l, 1.f, {kDt, 0.f, 0.f, false, 0.f, 450.f, -1.f});
    CHECK(l.state() == LocoState::Ground);
    CHECK(l.target() == LocoClip::Idle);

    // A caller that doesn't say where the body heads gets the gaits alone.
    l = makeMoveLoco();
    c = Capsule{};
    for (int i = 0; i < 60; ++i) {
        LocoInput in = c.step(450.f);
        in.wantSpeed = -1.f;
        l.update(in);
        CHECK(l.state() == LocoState::Ground);
    }
    for (int i = 0; i < 60; ++i) {
        LocoInput in = c.step(0.f);
        in.wantSpeed = in.stopDistance = -1.f;
        l.update(in);
        CHECK(l.state() == LocoState::Ground);
    }

    l = makeMoveLoco();
    c = Capsule{};
    for (int i = 0; i < 20; ++i) l.update(c.step(450.f));
    REQUIRE(l.state() == LocoState::Starting);
    LocoInput in = c.step(450.f);
    in.falling = true;
    in.verticalSpeed = 400.f;
    l.update(in);
    CHECK(l.state() == LocoState::Falling); // no jump clips here
}

namespace {

float smoothStep(float t, float a, float b) {
    const float u = std::fmin(std::fmax((t - a) / (b - a), 0.f), 1.f);
    return u * u * (3.f - 2.f * u);
}

// A synthetic turn: the root comes path(t) cm along a heading that turns by `turn` degrees between
// t0 and t1 (eased), the feet stepping as in makeMove in the root's own frame.
template <class Path>
void makeTurn(GaitFrame* frames, int n, float length, float phase0, Path path, float turn, float t0, float t1) {
    float x = 0.f, y = 0.f, prev = 0.f;
    for (int k = 0; k < n; ++k) {
        const float t = length * static_cast<float>(k) / static_cast<float>(n - 1);
        const float d = path(t);
        const float yaw = turn * smoothStep(t, t0, t1);
        const float a = yaw * 3.14159265f / 180.f, c = std::cos(a), s = std::sin(a);
        x += (d - prev) * c;
        y += (d - prev) * s;
        prev = d;
        frames[k].root = {x, y, 90.f};
        frames[k].yaw = yaw;
        for (int side = 0; side < 2; ++side) {
            float lx, z;
            stepFoot(phase0 + d / kStride + (side ? 0.5f : 0.f), kStride, false, lx, z);
            const float ly = side ? 10.f : -10.f;
            LocoPoint& foot = side ? frames[k].right : frames[k].left;
            foot = {x + lx * c - ly * s, y + lx * s + ly * c, z};
        }
    }
}

float runPath(float t) { return 450.f * t; }
float standPath(float) { return 0.f; }

// makeMoveLoco with running turns right by 45 and 90 (on either foot) and left by 90, each running
// 0.8 s before turning; a turning start right by 180; and a turn on the spot right by 90.
Locomotion makeTurnLoco() {
    static GaitFrame frames[121];
    Locomotion l = makeMoveLoco();
    for (const bool rightFoot : {false, true}) {
        const float phase0 = rightFoot ? 0.5f : 0.f;
        makeTurn(frames, 121, 2.f, phase0, runPath, 90.f, 0.8f, 1.2f);
        l.setClip(locoRunTurn(true, 1, rightFoot), measureMove(frames, 121, 2.f));
        makeTurn(frames, 121, 2.f, phase0, runPath, 45.f, 0.8f, 1.f);
        l.setClip(locoRunTurn(true, 0, rightFoot), measureMove(frames, 121, 2.f));
    }
    makeTurn(frames, 121, 2.f, 0.f, runPath, -90.f, 0.8f, 1.2f);
    l.setClip(locoRunTurn(false, 1, false), measureMove(frames, 121, 2.f));
    makeTurn(frames, 79, 1.3f, kStartPhase, startPath, 180.f, 0.1f, 0.5f);
    l.setClip(locoReface(true, 1), measureMove(frames, 79, 1.3f));
    makeTurn(frames, 91, 1.5f, 0.05f, standPath, 90.f, 0.1f, 0.5f);
    l.setClip(locoStandTurn(true, 1), measureMove(frames, 91, 1.5f));
    l.reset();
    return l;
}

// The foot phase of one of makeTurnLoco's running turns at t.
float runTurnPhase(LocoClip c, float t) { return (static_cast<int>(c) % 2 ? 0.5f : 0.f) + runPath(t) / kStride; }

// A body running at 450, steering, facing yaw 0, settled on the run.
LocoInput runningIn(Locomotion& l) {
    LocoInput in{kDt, 450.f};
    in.wantSpeed = 450.f;
    in.steer = in.rootMotion = true;
    for (int i = 0; i < 120; ++i) l.update(in);
    return in;
}

} // namespace

TEST_CASE("locomotion: measureMove reads a clip's turn, and its feet in the root's turned frame") {
    static GaitFrame frames[121];
    makeTurn(frames, 121, 2.f, 0.f, runPath, -90.f, 0.8f, 1.2f);
    const LocoClipInfo m = measureMove(frames, 121, 2.f);
    CHECK(m.turn == doctest::Approx(-90.f));
    // Eased: 5% done at 0.8 + 0.4 * 0.135, 95% at 0.8 + 0.4 * 0.865 (to the frame).
    CHECK(m.turnStart == doctest::Approx(0.854f).epsilon(0.03));
    CHECK(m.turnEnd == doctest::Approx(1.146f).epsilon(0.03));
    CHECK(m.yaw[0] == 0.f);
    CHECK(m.yaw[kMoveSamples - 1] == doctest::Approx(-90.f));
    CHECK(m.travel == doctest::Approx(900.f).epsilon(0.01));
    // The feet stay to either side of the root as it turns: turned back with it.
    for (const LocoFeet& f : m.feet) {
        CHECK(f.left.y == doctest::Approx(-10.f).epsilon(0.01));
        CHECK(f.right.y == doctest::Approx(10.f).epsilon(0.01));
    }
    // Unwrapped through +-180.
    makeTurn(frames, 121, 2.f, 0.f, runPath, 200.f, 0.8f, 1.2f);
    for (GaitFrame& f : frames) f.yaw -= f.yaw > 180.f ? 360.f : 0.f;
    CHECK(measureMove(frames, 121, 2.f).turn == doctest::Approx(200.f));
    // A clip that barely turns has no turn.
    makeTurn(frames, 121, 2.f, 0.f, runPath, 3.f, 0.8f, 1.2f);
    const LocoClipInfo still = measureMove(frames, 121, 2.f);
    CHECK(still.turn == 0.f);
    CHECK(still.turnEnd == 0.f);
}

TEST_CASE("locomotion: heading well off the facing at a run plays the nearest running turn on the matching foot, then the run in step") {
    Locomotion l = makeTurnLoco();
    LocoInput in = runningIn(l);
    REQUIRE(l.target() == LocoClip::Run);
    CHECK(l.yawStep() == 0.f);
    const float gaitPhase = l.phase();
    in.wantYaw = 90.f;
    l.update(in);
    REQUIRE(l.turning());
    const LocoClip turn = l.turnClip();
    CHECK((turn == locoRunTurn(true, 1, false) || turn == locoRunTurn(true, 1, true)));
    CHECK(l.turnWarp() == doctest::Approx(1.f).epsilon(0.01));
    CHECK(l.turnRootDriven());
    const LocoClipInfo& ti = l.clip(turn);
    // It enters a little before its turn, where its feet are where the run's were.
    CHECK(l.turnFrom() >= ti.turnStart - l.tuning().turnEarly - 1e-4f);
    CHECK(l.turnFrom() <= ti.turnStart - l.tuning().turnLate + 1e-4f);
    CHECK(phaseApart(runTurnPhase(turn, l.turnFrom()), gaitPhase) < 0.05f);
    // Played at rate 1 for the caller's root motion; the body takes the clip's yaw.
    int frames = 0;
    float last = l.turnTo();
    while (l.turning() && frames < 200) {
        in.yaw = in.moveYaw = l.turnYaw();
        l.update(in);
        if (l.turning()) {
            CHECK(l.turnFrom() == doctest::Approx(last));
            CHECK(l.turnTo() - l.turnFrom() == doctest::Approx(kDt).epsilon(0.01));
            last = l.turnTo();
        }
        ++frames;
    }
    REQUIRE(!l.turning());
    CHECK(l.state() == LocoState::Ground);
    CHECK(l.target() == LocoClip::Run);
    CHECK(in.yaw == doctest::Approx(90.f).epsilon(0.02));
    CHECK(last >= ti.turnEnd + l.tuning().turnTail - kDt);
    // The run picks up the turn's foot phase where it handed over.
    CHECK(phaseApart(l.phase(), runTurnPhase(turn, last)) < 0.06f);
    // Facing the new way, it runs on without turning again.
    for (int i = 0; i < 60; ++i) {
        in.moveYaw = 90.f;
        in.yaw += l.yawStep();
        l.update(in);
        CHECK(!l.turning());
    }
    CHECK(in.yaw == doctest::Approx(90.f).epsilon(0.01));
}

TEST_CASE("locomotion: a running turn picks the nearest angle and scales its yaw to the one asked for") {
    struct Case {
        float want;
        LocoClip clip0, clip1;
        float warp;
    };
    const Case cases[] = {
        {60.f, locoRunTurn(true, 0, false), locoRunTurn(true, 0, true), 1.3f},  // nearer 45; capped
        {80.f, locoRunTurn(true, 1, false), locoRunTurn(true, 1, true), 80.f / 90.f},
        {-100.f, locoRunTurn(false, 1, false), locoRunTurn(false, 1, false), 100.f / 90.f},
    };
    for (const Case& c : cases) {
        Locomotion l = makeTurnLoco();
        LocoInput in = runningIn(l);
        in.wantYaw = c.want;
        l.update(in);
        REQUIRE(l.turning());
        CHECK((l.turnClip() == c.clip0 || l.turnClip() == c.clip1));
        CHECK(l.turnWarp() == doctest::Approx(c.warp).epsilon(0.01));
        const float picked = l.clip(c.clip0).turn;
        while (l.turning()) {
            in.yaw = l.turnYaw();
            l.update(in);
        }
        CHECK(in.yaw == doctest::Approx(c.warp * picked).epsilon(0.02));
    }

    // A small change of heading, or one at a walk, doesn't turn: the facing eases toward the way the
    // body moves.
    Locomotion l = makeTurnLoco();
    LocoInput in = runningIn(l);
    in.wantYaw = in.moveYaw = 20.f;
    l.update(in);
    CHECK(!l.turning());
    CHECK(l.yawStep() == doctest::Approx(20.f * (1.f - std::exp(-kDt / l.tuning().faceTime))));
    in.groundSpeed = in.wantSpeed = 200.f;
    in.wantYaw = 90.f;
    for (int i = 0; i < 30; ++i) l.update(in);
    CHECK(!l.turning());
    // Nor does a caller that doesn't steer.
    l = makeTurnLoco();
    in = runningIn(l);
    in.steer = false;
    in.wantYaw = in.moveYaw = 90.f;
    l.update(in);
    CHECK(!l.turning());
    CHECK(l.yawStep() == 0.f);
}

TEST_CASE("locomotion: a follower replays a running turn at the distance it has come") {
    Locomotion l = makeTurnLoco();
    LocoInput in = runningIn(l);
    in.steer = in.rootMotion = false;
    in.turnBy = 90.f;
    l.update(in);
    REQUIRE(l.turning());
    CHECK(!l.turnRootDriven());
    in.turnBy = 0.f;
    const LocoClip turn = l.turnClip();
    const LocoClipInfo& ti = l.clip(turn);
    const float enter = l.time(turn);
    float moved = 0.f, lastYaw = 0.f;
    while (l.turning()) {
        l.update(in);
        moved += 450.f * kDt;
        if (!l.turning()) break;
        CHECK(trackPath(ti, l.time(turn)) - trackPath(ti, enter) == doctest::Approx(moved).epsilon(0.01));
        CHECK(l.turnYaw() >= lastYaw - 1e-3f);
        lastYaw = l.turnYaw();
    }
    CHECK(l.target() == LocoClip::Run);
    CHECK(lastYaw == doctest::Approx(90.f).epsilon(0.02));
}

TEST_CASE("locomotion: a follower drops a replayed turn until it has settled into its run") {
    Locomotion l = makeTurnLoco();
    LocoInput in{kDt, 0.f};
    in.wantSpeed = 450.f;
    // Setting off: the speed ramps up from a stand, and the turn reaches it just after the start
    // hands over to the gaits, once they are most of the blend (where it used to play).
    float speed = 0.f;
    int ground = 0;
    for (int i = 0; i < 300 && ground < 3; ++i) {
        speed = std::fmin(speed + 1000.f * kDt, 450.f);
        in.groundSpeed = speed;
        l.update(in);
        const float gaits = l.weight(LocoClip::Walk) + l.weight(LocoClip::Run) + l.weight(LocoClip::Sprint);
        if (l.state() == LocoState::Ground && gaits >= 0.5f) ++ground;
    }
    REQUIRE(ground == 3);
    in.turnBy = 135.f;
    l.update(in);
    CHECK(!l.turning());
    in.turnBy = 0.f;
    // Settled on the run, the next one plays.
    for (int i = 0; i < 90; ++i) l.update(in);
    in.turnBy = 135.f;
    l.update(in);
    CHECK(l.turning());
}

TEST_CASE("locomotion: a replayed turn keeps near the clip's own pace when the body outruns it") {
    for (const float speed : {900.f, 200.f}) {
        Locomotion l = makeTurnLoco();
        LocoInput in = runningIn(l);
        in.steer = in.rootMotion = false;
        in.turnBy = 90.f;
        l.update(in);
        REQUIRE(l.turning());
        in.turnBy = 0.f;
        in.groundSpeed = speed;
        const LocoClip turn = l.turnClip();
        float t = l.time(turn);
        for (int i = 0; i < 400 && l.turning(); ++i) {
            l.update(in);
            if (!l.turning()) break;
            const float step = l.time(turn) - t;
            CHECK(step <= l.tuning().turnRateMax * kDt + 1e-4f);
            CHECK(step >= l.tuning().turnRateMin * kDt - 1e-4f);
            t = l.time(turn);
        }
        CHECK(!l.turning());
    }
}

TEST_CASE("locomotion: setting off well away from the facing plays the turning start, then the gait") {
    Locomotion l = makeTurnLoco();
    Capsule c;
    LocoInput in = c.step(450.f);
    in.steer = in.rootMotion = true;
    in.wantYaw = 170.f;
    l.update(in);
    REQUIRE(l.turning());
    CHECK(l.turnClip() == locoReface(true, 1));
    CHECK(l.turnWarp() == doctest::Approx(170.f / 180.f));
    CHECK(l.turnFrom() == 0.f);
    int frames = 0;
    while (l.turning() && frames < 200) {
        in = c.step(450.f);
        in.steer = in.rootMotion = true;
        in.yaw = l.turnYaw();
        in.wantYaw = 170.f;
        l.update(in);
        ++frames;
    }
    CHECK(l.state() == LocoState::Ground);
    CHECK(l.target() == LocoClip::Run);
    CHECK(in.yaw == doctest::Approx(170.f).epsilon(0.02));

    // Setting off ahead plays the plain start.
    l = makeTurnLoco();
    c = Capsule{};
    in = c.step(450.f);
    in.steer = true;
    in.wantYaw = 30.f;
    l.update(in);
    CHECK(l.state() == LocoState::Starting);
}

TEST_CASE("locomotion: asked to face elsewhere when standing it turns on the spot") {
    Locomotion l = makeTurnLoco();
    LocoInput in{kDt, 0.f};
    in.wantSpeed = 0.f;
    in.steer = in.rootMotion = in.face = true;
    in.faceYaw = 20.f; // under faceMin
    run(l, 0.5f, in);
    CHECK(!l.turning());
    in.faceYaw = 85.f;
    l.update(in);
    REQUIRE(l.turning());
    CHECK(l.turnClip() == locoStandTurn(true, 1));
    CHECK(l.turnFrom() == 0.f);
    int frames = 0;
    while (l.turning() && frames < 200) {
        in.yaw = l.turnYaw();
        l.update(in);
        ++frames;
    }
    CHECK(l.target() == LocoClip::Idle);
    CHECK(in.yaw == doctest::Approx(85.f).epsilon(0.02));
    CHECK(static_cast<float>(frames) * kDt == doctest::Approx(l.clip(locoStandTurn(true, 1)).turnEnd + l.tuning().turnTail).epsilon(0.1));
    run(l, 0.5f, in);
    CHECK(!l.turning());

    // A follower, which doesn't steer (its caller faces it the way it moves), turns on the spot too.
    l = makeTurnLoco();
    in = LocoInput{kDt, 0.f};
    in.wantSpeed = 0.f;
    in.face = true;
    in.faceYaw = 100.f;
    l.update(in);
    REQUIRE(l.turning());
    CHECK(l.turnClip() == locoStandTurn(true, 1));
    frames = 0;
    while (l.turning() && frames < 200) { // it plays through though nothing moves it: the caller turns it by turnYaw
        in.yaw = l.turnYaw();
        l.update(in);
        ++frames;
    }
    CHECK(in.yaw == doctest::Approx(100.f).epsilon(0.02));

    // Heading off mid-turn leaves it; so does a jump.
    l = makeTurnLoco();
    in.steer = in.rootMotion = true;
    in.faceYaw = 85.f;
    in.yaw = 0.f;
    l.update(in);
    REQUIRE(l.turning());
    in.wantSpeed = 450.f;
    in.face = false;
    l.update(in);
    CHECK(!l.turning());
    l = makeTurnLoco();
    in = runningIn(l);
    in.wantYaw = 90.f;
    l.update(in);
    REQUIRE(l.turning());
    in.falling = true;
    in.verticalSpeed = 400.f;
    l.update(in);
    CHECK(l.state() == LocoState::Falling);
}

TEST_CASE("locomotion: update does not allocate") {
    Locomotion l = makeLoco();
    l.setClip(LocoClip::AirPose, {0.5f, 0.f, 1, 0.f});
    const auto before = ftcore_test::allocStats().count;
    for (int i = 0; i < 600; ++i) {
        const float s = static_cast<float>(i % 200) * 4.f;
        l.update({kDt, s, 90.f, i % 150 > 120, i % 150 > 135 ? -300.f : 400.f});
    }
    CHECK(ftcore_test::allocStats().count == before);
    Locomotion m = makeMoveLoco();
    const auto moving = ftcore_test::allocStats().count;
    Capsule c;
    for (int i = 0; i < 600; ++i) m.update(c.step(i % 200 < 100 ? 450.f : 0.f));
    CHECK(ftcore_test::allocStats().count == moving);
    Locomotion t = makeTurnLoco();
    const auto turning = ftcore_test::allocStats().count;
    c = Capsule{};
    float yaw = 0.f;
    for (int i = 0; i < 600; ++i) {
        LocoInput in = c.step(i % 200 < 150 ? 450.f : 0.f);
        in.steer = in.rootMotion = in.face = true;
        in.yaw = in.moveYaw = yaw;
        in.wantYaw = in.faceYaw = static_cast<float>((i / 50) % 4) * 90.f;
        t.update(in);
        yaw += t.turning() ? 1.f : t.yawStep();
    }
    CHECK(ftcore_test::allocStats().count == turning);
}

TEST_CASE("locomotion: in a turn clip the chest and head lead the hips, and the body banks") {
    Locomotion l = makeTurnLoco();
    LocoInput in = runningIn(l);
    in.steer = in.rootMotion = false;
    in.turnBy = 90.f;
    l.update(in);
    REQUIRE(l.turning());
    in.turnBy = 0.f;
    float yaw = 0.f, peak = 0.f, yawAtPeak = 0.f, maxLean = 0.f, maxHead = 0.f;
    while (l.turning()) {
        const float before = yaw;
        yaw = l.turnYaw();
        in.turnRate = (yaw - before) / kDt;
        l.update(in);
        if (!l.turning()) break;
        if (l.twist() > peak) {
            peak = l.twist();
            yawAtPeak = l.turnYaw();
        }
        maxLean = std::fmax(maxLean, l.lean());
        maxHead = std::fmax(maxHead, l.headYaw());
        CHECK(l.twist() >= 0.f);
    }
    CHECK(peak > 0.6f * l.tuning().maxTwist);
    CHECK(yawAtPeak < 0.5f * 90.f); // ahead early: the chest is round before the hips are halfway
    CHECK(maxHead > 0.5f * l.tuning().maxHeadYaw);
    CHECK(maxLean > 5.f);
    CHECK(maxLean <= l.tuning().maxLean + 1e-4f);
    // Unwound by the time the turn hands back to the run.
    CHECK(l.twist() < 0.25f * peak);
    CHECK(std::fabs(l.headYaw()) < 0.25f * maxHead);

    // On the spot it leads the same way but doesn't bank.
    l = makeTurnLoco();
    in = LocoInput{kDt, 0.f};
    in.wantSpeed = 0.f;
    in.face = true;
    in.faceYaw = 90.f;
    l.update(in);
    REQUIRE(l.turning());
    peak = maxLean = 0.f;
    yaw = 0.f;
    while (l.turning()) {
        const float before = yaw;
        yaw = l.turnYaw();
        in.yaw = yaw;
        in.turnRate = (yaw - before) / kDt;
        l.update(in);
        peak = std::fmax(peak, l.twist());
        maxLean = std::fmax(maxLean, std::fabs(l.lean()));
    }
    CHECK(peak > 0.6f * l.tuning().maxTwist);
    CHECK(maxLean < 0.5f);
}
