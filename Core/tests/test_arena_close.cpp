#include "ftcore/ArenaCamera.h"
#include "ftcore/ArenaCloseFrame.h"
#include "ftcore/ArenaLockFrame.h"
#include "ftcore/ArenaLook.h"
#include "ftcore/CameraPreset.h"
#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>
#include <cmath>
#include <limits>

using namespace ftcore;

namespace {
constexpr float kAspect = 16.f / 9.f;
// What the rule allows of +28 on a fully tall target at a full melee share, by the lead's height (measured: a 1.4 m
// lead's shoulders reach the cut's floor first).
constexpr float kTallReach14 = 25.07f, kTallReach16 = 28.f, kTallReach17 = 28.f, kTallReach18 = 28.f;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

// The close rig at a melee share, a tallness and a shoulder, as the host builds it: a pose at yaw `yaw`, pitch 0.
ArenaCameraPose closeRig(float share, float tallness, float shoulder, float head, float yaw = 0.f, float wall = 1.f) {
    const CameraPreset cp = closeCameraPreset();
    const ArenaCloseShape s = arenaCloseShape(kArenaClose, cp, share, tallness, shoulder, head);
    return ArenaCameraPose{yaw, 0.f, 0.f, cp.rig.arm * wall, s.pivot, s.side * wall, cp.rig.up * wall, s.fov};
}

ArenaLeadFrameRules closeRules(float share, float tallness, float head) {
    const CameraPreset cp = closeCameraPreset();
    CameraPreset lens = cp;
    lens.cutMin = arenaCloseShape(kArenaClose, cp, share, tallness, 1.f, head).cutMin;
    return cameraPresetLeadRules(lens);
}

ArenaLeadBody closeBody(float share, float tallness, float head) {
    return ArenaLeadBody{head, 0.35f, arenaCloseShape(kArenaClose, closeCameraPreset(), share, tallness, 1.f, head).cutAt};
}

float spineOf(const ArenaLeadShares& s) { return 0.5f * (s.left + s.right); }

// The pose the lock draws on a target `d` m ahead (the line at yaw 0): the framing turn that puts it at the shape's
// screenX, at the preset's lock pitch.
ArenaCameraPose lockPose(float share, float shoulder, float d, float head = 1.8f) {
    const CameraPreset cp = closeCameraPreset();
    const ArenaCloseShape s = arenaCloseShape(kArenaClose, cp, share, 0.f, shoulder, head);
    const ArenaCameraPose rig0 = closeRig(share, 0.f, shoulder, head);
    const float back = arenaEyeBack(rig0.arm, rig0.up, arenaSplitPitch(kArenaLook, cp.rig.lockPitch).boom);
    const float turn = arenaFrameTurnAt(kArenaFrame, s.side, d, s.screenX, s.fov, back);
    return arenaPoseAtPitch(kArenaLook, closeRig(share, 0.f, shoulder, head, turn), cp.rig.lockPitch);
}
} // namespace

TEST_CASE("close frame: the melee share comes from the distance, smoothstepped from 5 m in to 2.5 m") {
    CHECK(arenaCloseMeleeWant(kArenaClose, 0.f) == 1.f);
    CHECK(arenaCloseMeleeWant(kArenaClose, 2.5f) == 1.f);
    CHECK(arenaCloseMeleeWant(kArenaClose, 5.f) == 0.f);
    CHECK(arenaCloseMeleeWant(kArenaClose, 12.f) == 0.f);
    CHECK(arenaCloseMeleeWant(kArenaClose, 3.75f) == doctest::Approx(0.5f));
    CHECK(arenaCloseMeleeWant(kArenaClose, kNaN) == 0.f);
    CHECK(arenaCloseMeleeWant(kArenaClose, -1.f) == 0.f);
    float last = 1.f;
    for (float d = 2.5f; d <= 5.f; d += 0.05f) {
        const float w = arenaCloseMeleeWant(kArenaClose, d);
        CHECK(w <= last + 1e-6f);
        last = w;
    }
}

TEST_CASE("close frame: the share eases over 0.3 s and holds still for a dodge's first 0.15 s") {
    float s = 0.f;
    for (int i = 0; i < 18; ++i) s = arenaCloseMeleeStep(kArenaClose, s, 1.f, 1.f / 60.f, false);
    CHECK(s == doctest::Approx(1.f - std::exp(-1.f)).epsilon(0.01));
    for (int i = 0; i < 600; ++i) s = arenaCloseMeleeStep(kArenaClose, s, 1.f, 1.f / 60.f, false);
    CHECK(s <= 1.f);
    CHECK(s > 0.999f);
    CHECK(arenaCloseMeleeStep(kArenaClose, 0.4f, 1.f, 0.1f, true) == 0.4f);
    CHECK(arenaCloseMeleeStep(kArenaClose, 0.4f, kNaN, 0.1f, false) == 0.4f);
    CHECK(arenaCloseMeleeStep(kArenaClose, kNaN, 1.f, 0.f, false) == 0.f);
    CHECK(arenaCloseDodgeHolds(kArenaClose, 0.f));
    CHECK(arenaCloseDodgeHolds(kArenaClose, 0.149f));
    CHECK_FALSE(arenaCloseDodgeHolds(kArenaClose, 0.15f));
    CHECK_FALSE(arenaCloseDodgeHolds(kArenaClose, -1.f));
    CHECK_FALSE(arenaCloseDodgeHolds(kArenaClose, kNaN));
}

TEST_CASE("close frame: the shape at share 0 on the right shoulder is the preset's own, and mirrors on the left") {
    const CameraPreset cp = closeCameraPreset();
    const ArenaCloseShape far = arenaCloseShape(kArenaClose, cp, 0.f, 0.f, 1.f, 1.8f);
    CHECK(far.pivot == cp.rig.pivot);
    CHECK(far.side == cp.rig.side);
    CHECK(far.fov == cp.rig.fov);
    CHECK(far.screenX == doctest::Approx(cp.targetX).epsilon(1e-6));
    CHECK(far.cutAt == cameraPresetCut(cp, 1.8f));
    CHECK(far.cutMin == cp.cutMin);
    const ArenaCloseShape left = arenaCloseShape(kArenaClose, cp, 0.f, 0.f, -1.f, 1.8f);
    CHECK(left.side == -cp.rig.side);
    CHECK(left.screenX == doctest::Approx(1.f - cp.targetX));
    const ArenaCloseShape mid = arenaCloseShape(kArenaClose, cp, 0.f, 0.f, 0.f, 1.8f); // halfway through a swap
    CHECK(mid.side == 0.f);
    CHECK(mid.screenX == 0.5f);
    const ArenaCloseShape melee = arenaCloseShape(kArenaClose, cp, 1.f, 0.f, 1.f, 1.8f);
    CHECK(melee.pivot == doctest::Approx(cp.rig.pivot + 0.25f));
    CHECK(melee.fov == doctest::Approx(68.f));
    CHECK(melee.screenX == doctest::Approx(0.64f));
    CHECK(melee.cutAt == doctest::Approx(0.6f)); // a normal enemy keeps the thighs
    CHECK(melee.cutMin == doctest::Approx(-0.10f));
    CHECK(arenaCloseShape(kArenaClose, cp, 1.f, 0.f, -1.f, 1.8f).screenX == doctest::Approx(0.36f));
    // A tall one raises the cut toward the shoulders, never over them, never under the preset's own cut.
    CHECK(arenaCloseShape(kArenaClose, cp, 1.f, 1.f, 1.f, 1.8f).cutAt == doctest::Approx(1.3f));
    CHECK(arenaCloseShape(kArenaClose, cp, 1.f, 1.f, 1.f, 1.4f).cutAt == doctest::Approx(1.05f));
    CHECK(arenaCloseShape(kArenaClose, cp, 0.f, 1.f, 1.f, 1.0f).cutAt == doctest::Approx(cameraPresetCut(cp, 1.0f)));
    CHECK(arenaCloseShape(kArenaClose, cp, kNaN, kNaN, kNaN, 1.8f).side == cp.rig.side);
}

TEST_CASE("close frame: the side puts the lead's spine at 30-35% of the width, far and in melee, mirrored on the left") {
    for (float share : {0.f, 0.5f, 1.f}) {
        for (float head : {1.6f, 1.7f, 1.8f}) {
            const ArenaLeadShares r = arenaLeadShares(arenaPoseAtPitch(kArenaLook, closeRig(share, 0.f, 1.f, head), -2.f), kAspect,
                                                      closeBody(share, 0.f, head));
            const ArenaLeadShares l = arenaLeadShares(arenaPoseAtPitch(kArenaLook, closeRig(share, 0.f, -1.f, head), -2.f), kAspect,
                                                      closeBody(share, 0.f, head));
            CAPTURE(share);
            CAPTURE(head);
            CHECK(spineOf(r) >= 0.30f);
            CHECK(spineOf(r) <= 0.35f);
            CHECK(spineOf(l) == doctest::Approx(1.f - spineOf(r)).epsilon(1e-4));
            CHECK(arenaLeadInFrame(closeRules(share, 0.f, head), r));
            CHECK(arenaLeadInFrame(closeRules(share, 0.f, head), l));
        }
    }
    // The far spine, written down (side 0.55: 0.316; the 0.65 asked for sits at 0.283, the bar's edge).
    const ArenaLeadShares far = arenaLeadShares(arenaPoseAtPitch(kArenaLook, closeRig(0.f, 0.f, 1.f, 1.8f), -2.f), kAspect,
                                                closeBody(0.f, 0.f, 1.8f));
    CHECK(spineOf(far) == doctest::Approx(0.316f).epsilon(0.01));
    // A wall that shortens the lens to 70% (the arm, the side and the height alike): in melee the rule still holds
    // (at -21.9); far, only the weaker rule (visible, at -30.6), the same as the old 0.2 m side gave (the side is not
    // what a short arm loses).
    for (float share : {0.f, 1.f}) {
        const ArenaLeadPitch p = arenaLeadPitch(kArenaLook, closeRules(share, 0.f, 1.8f), closeRig(share, 0.f, 1.f, 1.8f, 0.f, 0.7f), kAspect,
                                                closeBody(share, 0.f, 1.8f), -2.f);
        const ArenaLeadShares w = arenaLeadShares(arenaPoseAtPitch(kArenaLook, closeRig(share, 0.f, 1.f, 1.8f, 0.f, 0.7f), p.pitch), kAspect,
                                                  closeBody(share, 0.f, 1.8f));
        CHECK(p.kept == (share > 0.f ? 2 : 1));
        CHECK(spineOf(w) > 0.25f);
        CHECK(spineOf(w) < 0.36f);
    }
}

TEST_CASE("close frame: the lock puts the target beside the lead, mirrored by the shoulder, the turn mirrored too") {
    for (float d : {4.f, 6.f, 8.f}) {
        const ArenaCameraPose r = lockPose(0.f, 1.f, d);
        const ArenaCameraPose l = lockPose(0.f, -1.f, d);
        CAPTURE(d);
        CHECK(l.yaw == doctest::Approx(-r.yaw).epsilon(1e-4));
        const ArenaFramePoint tr = arenaFramePoint(r, kAspect, ArenaPoint3{d, 0.f, 1.f});
        const ArenaFramePoint tl = arenaFramePoint(l, kAspect, ArenaPoint3{d, 0.f, 1.f});
        CHECK(tr.x == doctest::Approx(0.6f).epsilon(0.01));
        CHECK(tl.x == doctest::Approx(0.4f).epsilon(0.01));
        // The lead's body is clear of the target's (0.5 m each side of its centre) at 4 m and more: under 15% overlap.
        const ArenaLeadShares s = arenaLeadShares(r, kAspect, closeBody(0.f, 0.f, 1.8f));
        const float tL = arenaFramePoint(r, kAspect, ArenaPoint3{d, -0.5f, 1.f}).x;
        const float tR = arenaFramePoint(r, kAspect, ArenaPoint3{d, 0.5f, 1.f}).x;
        CHECK(std::fmax(0.f, std::fmin(s.right, tR) - std::fmax(s.left, tL)) / (tR - tL) <= 0.15f);
    }
}

TEST_CASE("close frame: the melee look-up goes toward the head, capped by the target's height, far pitch at share 0") {
    const CameraPreset cp = closeCameraPreset();
    const float farPitch = -2.f;
    CHECK(arenaCloseMeleePitch(kArenaClose, kArenaLook, 1.6f, 2.6f, 0.1f, 2.f, 2.f, 0.f, farPitch) == farPitch);
    CHECK(arenaCloseMeleePitch(kArenaClose, kArenaLook, 1.6f, 2.6f, 0.1f, 2.f, 2.f, kNaN, farPitch) == farPitch);
    const float normal = arenaCloseMeleePitch(kArenaClose, kArenaLook, 1.6f, 2.6f, 0.1f, 2.f, 2.4f, 1.f, farPitch);
    CHECK(normal > farPitch);
    CHECK(normal <= 10.f + 1e-4f);
    const float tall = arenaCloseMeleePitch(kArenaClose, kArenaLook, 1.6f, 2.6f, 0.1f, 2.f, 7.f, 1.f, farPitch);
    CHECK(tall == doctest::Approx(28.f));
    const float half = arenaCloseMeleePitch(kArenaClose, kArenaLook, 1.6f, 2.6f, 0.1f, 2.f, 7.f, 0.5f, farPitch);
    CHECK(half == doctest::Approx(0.5f * (farPitch + 28.f)));
    // Never under the far pitch: a far pitch already over the look-up stays.
    CHECK(arenaCloseMeleePitch(kArenaClose, kArenaLook, 1.6f, 2.6f, 0.1f, 2.f, 1.f, 1.f, 12.f) == doctest::Approx(12.f));
    (void)cp;
}

TEST_CASE("close frame: at a full melee share the lead's rule allows the look-up (+10 normal, the tall reach pinned)") {
    for (float head : {1.6f, 1.7f, 1.8f}) {
        const ArenaLeadPitch p = arenaLeadPitch(kArenaLook, closeRules(1.f, 0.f, head), closeRig(1.f, 0.f, 1.f, head), kAspect,
                                                closeBody(1.f, 0.f, head), 10.f);
        CAPTURE(head);
        CHECK(p.kept == 2);
        CHECK(p.pitch == doctest::Approx(10.f));
    }
    // The far rule on the raised pivot (what the shape replaces) held the same lens at -4.4: the cut sank under it.
    const CameraPreset cp = closeCameraPreset();
    const ArenaLeadPitch old = arenaLeadPitch(kArenaLook, cameraPresetLeadRules(cp), closeRig(1.f, 0.f, 1.f, 1.8f), kAspect,
                                              ArenaLeadBody{1.8f, 0.35f, 0.6f}, 10.f);
    CHECK(old.pitch < 0.f);
    // A fully tall target: what the rule allows of +28, by the lead's height.
    const float reach[] = {kTallReach14, kTallReach16, kTallReach17, kTallReach18};
    const float heads[] = {1.4f, 1.6f, 1.7f, 1.8f};
    for (int i = 0; i < 4; ++i) {
        const float head = heads[i];
        const ArenaLeadPitch p = arenaLeadPitch(kArenaLook, closeRules(1.f, 1.f, head), closeRig(1.f, 1.f, 1.f, head), kAspect,
                                                closeBody(1.f, 1.f, head), 28.f);
        MESSAGE("tall reach head ", head, ": ", p.pitch, " kept ", p.kept);
        CHECK(p.kept == 2);
        CHECK(p.pitch == doctest::Approx(reach[i]).epsilon(0.02));
    }
}

TEST_CASE("close frame: the shoulder pick: a wall first, then the partner, then the room, else it stays") {
    CHECK(arenaCloseShoulderPick(kArenaClose, 0.5f, 1.f, true, false, 1.f) == -1.f);  // the right is blocked
    CHECK(arenaCloseShoulderPick(kArenaClose, 1.f, 0.5f, false, true, -1.f) == 1.f);  // the left is blocked
    CHECK(arenaCloseShoulderPick(kArenaClose, 1.f, 1.f, false, true, 1.f) == -1.f);   // the partner only from the left
    CHECK(arenaCloseShoulderPick(kArenaClose, 1.f, 0.9f, true, true, -1.f) == 1.f);   // more room on the right
    CHECK(arenaCloseShoulderPick(kArenaClose, 1.f, 0.97f, true, true, -1.f) == -1.f); // a near tie stays
    CHECK(arenaCloseShoulderPick(kArenaClose, 0.5f, 0.4f, false, false, -1.f) == 1.f); // both blocked: the more room
    CHECK(arenaCloseShoulderPick(kArenaClose, 1.f, 1.f, false, false, 0.f) == 1.f);
    CHECK(arenaCloseShoulderPick(kArenaClose, kNaN, 1.f, false, false, 1.f) == -1.f); // no reading: blocked
}

TEST_CASE("close frame: a shoulder swap takes 0.4 s, at a constant rate, never past the side") {
    float s = 1.f;
    for (int i = 0; i < 12; ++i) s = arenaCloseShoulderStep(kArenaClose, s, -1.f, 1.f / 60.f);
    CHECK(s == doctest::Approx(0.f).epsilon(1e-4));
    for (int i = 0; i < 13; ++i) s = arenaCloseShoulderStep(kArenaClose, s, -1.f, 1.f / 60.f);
    CHECK(s == -1.f);
    CHECK(arenaCloseShoulderStep(kArenaClose, -1.f, -1.f, 1.f) == -1.f);
    CHECK(arenaCloseShoulderStep(kArenaClose, kNaN, 1.f, 0.1f) == 1.f);
    CHECK(arenaCloseShoulderStep(kArenaClose, 0.3f, 1.f, kNaN) == 0.3f);
}

TEST_CASE("close frame: through a shoulder swap a held stick keeps its basis; let go it follows, past the blend it eases back") {
    CHECK_FALSE(arenaCloseSwapping(1.f, 1.f));
    CHECK_FALSE(arenaCloseSwapping(-1.f, -1.f));
    CHECK_FALSE(arenaCloseSwapping(1.f, 0.f)); // 0 counts as right
    CHECK(arenaCloseSwapping(0.4f, -1.f));
    CHECK_FALSE(arenaCloseSwapping(kNaN, 1.f));
    // A swap right to left while the view's yaw swings 2 degrees a frame: the held stick's basis stays at 10.
    const float dt = 1.f / 60.f;
    const auto step = [dt](ArenaStickBasis b, float v, bool swapping, bool held) {
        return arenaCloseStickBasis(kArenaClose, b, v, swapping, held, dt);
    };
    float s = 1.f;
    ArenaStickBasis basis = step(ArenaStickBasis{}, 10.f, false, true);
    CHECK(basis.yaw == 10.f);
    float view = 10.f;
    for (int i = 0; i < 6; ++i) {
        s = arenaCloseShoulderStep(kArenaClose, s, -1.f, dt);
        view += 2.f;
        basis = step(basis, view, arenaCloseSwapping(s, -1.f), true);
        CHECK(basis.yaw == 10.f);
    }
    // An attack freezes the blend (no step): still swapping, still held.
    basis = step(basis, view + 2.f, arenaCloseSwapping(arenaCloseShoulderStep(kArenaClose, s, -1.f, 0.f), -1.f), true);
    CHECK(basis.yaw == 10.f);
    // Let go mid-blend: the view's yaw at once; pushed again, held from there.
    basis = step(basis, view, true, false);
    CHECK(basis.yaw == view);
    const float latched = view;
    view = latched + 40.f; // the rest of the swap turns the drawn yaw 40 degrees
    basis = step(basis, view, true, true);
    CHECK(basis.yaw == latched);
    // The blend ends with the stick still held: no one-frame snap, a steady ease onto the view over 0.25 s.
    while (arenaCloseSwapping(s, -1.f)) s = arenaCloseShoulderStep(kArenaClose, s, -1.f, dt);
    float last = basis.yaw;
    int frames = 0;
    while (basis.yaw != view && frames < 60) {
        basis = step(basis, view, arenaCloseSwapping(s, -1.f), true);
        ++frames;
        CHECK(basis.yaw > last);
        CHECK(basis.yaw - last <= 40.f * dt / 0.25f + 1e-3f);
        last = basis.yaw;
    }
    CHECK(frames >= 14);
    CHECK(frames <= 16);
    CHECK(basis.rate == 0.f);
    CHECK_FALSE(basis.lag);
    // With no swap behind it a held stick reads the view exactly as it turns (the free camera, the lock's framing).
    for (int i = 0; i < 5; ++i) {
        view += 7.f;
        basis = step(basis, view, false, true);
        CHECK(basis.yaw == view);
    }
    // The short way round across the wrap: 170 to -170 is 20 degrees right.
    ArenaStickBasis wrap{170.f, 0.f, true, true};
    wrap = step(wrap, -170.f, false, true);
    CHECK(wrap.yaw == doctest::Approx(170.f + 20.f * dt / 0.25f));
    // Nothing set yet, or a NaN view: the view, or no change.
    CHECK(step(ArenaStickBasis{}, 33.f, true, true).yaw == 33.f);
    CHECK(step(wrap, kNaN, false, true).yaw == wrap.yaw);
}

TEST_CASE("close frame: the partner yaw turns up to 12 degrees toward the partner, keeping the lead and the target") {
    const float d = 4.f;
    const ArenaPoint3 target{d, 0.f, 1.f};
    const ArenaLeadBody body = closeBody(0.f, 0.f, 1.8f);
    const ArenaLeadFrameRules rules = closeRules(0.f, 0.f, 1.8f);
    for (float shoulder : {1.f, -1.f}) {
        const ArenaCameraPose pose = lockPose(0.f, shoulder, d);
        CAPTURE(shoulder);
        // In the frame already: no turn.
        CHECK(arenaClosePartnerYaw(kArenaClose, rules, pose, kAspect, body, ArenaPoint3{d, shoulder * 1.5f, 1.f}, target, shoulder) == 0.f);
        // Out past 8 m: no turn.
        CHECK(arenaClosePartnerYaw(kArenaClose, rules, pose, kAspect, body, ArenaPoint3{0.f, -9.f, 1.f}, target, shoulder) == 0.f);
        // Just off the left edge (beside the lead, on the far side from the target): a turn left that brings it in.
        for (float y = 2.f; y <= 4.f; y += 0.25f) {
            const ArenaPoint3 mate{1.f, -y, 1.f};
            const ArenaFramePoint before = arenaFramePoint(pose, kAspect, mate);
            if (before.ahead && before.x >= kPartnerFrustumMargin) continue;
            const float yaw = arenaClosePartnerYaw(kArenaClose, rules, pose, kAspect, body, mate, target, shoulder);
            if (yaw == 0.f) continue;
            CAPTURE(y);
            CHECK(yaw < 0.f);
            CHECK(yaw >= -12.f);
            ArenaCameraPose turned = pose;
            turned.yaw += yaw;
            const ArenaFramePoint after = arenaFramePoint(turned, kAspect, mate);
            CHECK(after.ahead);
            CHECK(after.x >= kPartnerFrustumMargin);
            const float tx = arenaFramePoint(turned, kAspect, target).x;
            CHECK(tx >= (shoulder > 0.f ? 0.55f : 0.2f) - 1e-4f);
            CHECK(tx <= (shoulder > 0.f ? 0.8f : 0.45f) + 1e-4f);
            CHECK(arenaLeadInFrame(rules, arenaLeadShares(turned, kAspect, body)));
        }
    }
    // Behind the lens, past any 12 degree turn: none.
    const ArenaCameraPose pose = lockPose(0.f, 1.f, d);
    CHECK(arenaClosePartnerYaw(kArenaClose, rules, pose, kAspect, body, ArenaPoint3{-4.f, 0.f, 1.f}, target, 1.f) == 0.f);
}

TEST_CASE("close frame: the partner on the brain's stations round the target is in the frame on 60% of them or more") {
    // A model, not the fight: the stations of rule 10 (the ring 1.5 m round the target, 60 to 120 degrees from the lead,
    // either side), the target 2 to 6 m ahead, the close lens far and in melee, with the close partner yaw.
    int in = 0, n = 0;
    for (float d : {2.f, 2.4f, 3.f, 4.f, 6.f}) {
        const float share = arenaCloseMeleeWant(kArenaClose, d);
        const ArenaCameraPose pose = lockPose(share, 1.f, d);
        const ArenaLeadBody body = closeBody(share, 0.f, 1.8f);
        const ArenaLeadFrameRules rules = closeRules(share, 0.f, 1.8f);
        for (float sgn : {1.f, -1.f}) {
            for (float ang = 60.f; ang <= 120.f + 1e-3f; ang += 15.f) {
                const float a = ang * 3.14159265f / 180.f;
                const ArenaPoint3 mate{d - 1.5f * std::cos(a), sgn * 1.5f * std::sin(a), 1.f};
                ArenaCameraPose p = pose;
                p.yaw += arenaClosePartnerYaw(kArenaClose, rules, pose, kAspect, body, mate, ArenaPoint3{d, 0.f, 1.f}, 1.f);
                const ArenaFramePoint f = arenaFramePoint(p, kAspect, mate);
                in += f.ahead && f.x >= kPartnerFrustumMargin && f.x <= 1.f - kPartnerFrustumMargin;
                ++n;
            }
        }
    }
    MESSAGE("partner in frame on the station model: ", in, " of ", n);
    CHECK(in * 10 >= n * 6);
}

TEST_CASE("close frame: a point in the lens's ground frame") {
    const ArenaCameraPose pose{0.f, 0.f, 0.f, 2.6f, 1.35f, 0.55f, 0.1f, 60.f};
    const ArenaLensGround g = arenaLensGround(pose, ArenaPoint3{0.f, 0.f, 1.f});
    CHECK(g.ahead == doctest::Approx(2.6f));
    CHECK(g.right == doctest::Approx(-0.55f));
    ArenaCameraPose turned = pose;
    turned.yaw = 90.f;
    const ArenaLensGround h = arenaLensGround(turned, ArenaPoint3{0.f, 5.f, 1.f});
    CHECK(h.ahead == doctest::Approx(7.6f));
    CHECK(h.right == doctest::Approx(-0.55f).epsilon(1e-3));
}

