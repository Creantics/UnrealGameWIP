#include "ftcore/ArenaCamera.h"
#include "ftcore/ArenaCloseFrame.h"
#include "ftcore/ArenaLockFrame.h"
#include "ftcore/ArenaLook.h"
#include "ftcore/CameraPreset.h"
#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>
#include <cmath>
#include <limits>
#include <string>

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

TEST_CASE("close frame: at a full melee share the lead's rule allows the look-up (+6 normal, the tall reach pinned)") {
    for (float head : {1.6f, 1.7f, 1.8f}) {
        const ArenaLeadPitch p = arenaLeadPitch(kArenaLook, closeRules(1.f, 0.f, head), closeRig(1.f, 0.f, 1.f, head), kAspect,
                                                closeBody(1.f, 0.f, head), 6.f);
        CAPTURE(head);
        CHECK(p.kept == 2);
        CHECK(p.pitch == doctest::Approx(6.f));
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

TEST_CASE("close frame: a ranged partner's yaw cap is 12 as ruled; a wider one (the switch's) never grows past 12 while the lead is busy") {
    CHECK(kArenaClose.partnerYawMax == 12.f);
    CHECK(kArenaClose.partnerYawRangedMax == 12.f);
    // As shipped: 12 for either kit, busy or not, whatever turn is held.
    for (const bool ranged : {false, true})
        for (const bool busy : {false, true})
            for (const float held : {0.f, 8.f, -18.5f, 30.f, kNaN}) CHECK(arenaClosePartnerYawCap(kArenaClose, ranged, busy, held) == 12.f);
    // The mechanism at a 24 degree cap (-ArenaPartnerYawCap=24).
    ArenaCloseRules r24 = kArenaClose;
    r24.partnerYawRangedMax = 24.f;
    // Melee: 12, busy or not, whatever turn is held.
    CHECK(arenaClosePartnerYawCap(r24, false, false, 0.f) == 12.f);
    CHECK(arenaClosePartnerYawCap(r24, false, true, 20.f) == 12.f);
    // Ranged and free: 24.
    CHECK(arenaClosePartnerYawCap(r24, true, false, 0.f) == 24.f);
    CHECK(arenaClosePartnerYawCap(r24, true, false, kNaN) == 24.f);
    // Ranged and busy: the turn held now (either sign), within 12..24.
    CHECK(arenaClosePartnerYawCap(r24, true, true, 0.f) == 12.f);
    CHECK(arenaClosePartnerYawCap(r24, true, true, 8.f) == 12.f);
    CHECK(arenaClosePartnerYawCap(r24, true, true, -18.5f) == 18.5f);
    CHECK(arenaClosePartnerYawCap(r24, true, true, 30.f) == 24.f);
    CHECK(arenaClosePartnerYawCap(r24, true, true, kNaN) == 12.f);
    // A ranged most under the melee one never lowers it.
    ArenaCloseRules low = r24;
    low.partnerYawRangedMax = 6.f;
    CHECK(arenaClosePartnerYawCap(low, true, false, 0.f) == 12.f);
    // The cap fed back as the rules' partnerYawMax: a partner a 12 degree turn cannot reach, a 24 degree one can, the
    // target kept in its band and the lead in the frame.
    const float d = 4.f;
    const ArenaPoint3 target{d, 0.f, 1.f};
    const ArenaLeadBody body = closeBody(0.f, 0.f, 1.8f);
    const ArenaLeadFrameRules rules = closeRules(0.f, 0.f, 1.8f);
    const ArenaCameraPose pose = lockPose(0.f, 1.f, d);
    ArenaCloseRules wide = r24;
    wide.partnerYawMax = arenaClosePartnerYawCap(r24, true, false, 0.f);
    int gained = 0;
    for (float x = 1.f; x <= 7.f; x += 0.5f)
        for (float y = 1.f; y <= 7.f; y += 0.5f) {
            const ArenaPoint3 mate{x, -y, 1.f};
            if (std::sqrt(x * x + y * y) > kArenaClose.partnerRange) continue;
            const float narrow = arenaClosePartnerYaw(kArenaClose, rules, pose, kAspect, body, mate, target, 1.f);
            const float yaw = arenaClosePartnerYaw(wide, rules, pose, kAspect, body, mate, target, 1.f);
            CAPTURE(x);
            CAPTURE(y);
            CHECK(std::fabs(yaw) <= 24.f + 1e-4f);
            if (narrow != 0.f) CHECK(yaw == narrow); // the smallest turn: unchanged where 12 did it
            if (yaw == 0.f || narrow != 0.f) continue;
            ++gained;
            CHECK(std::fabs(yaw) > 12.f);
            ArenaCameraPose turned = pose;
            turned.yaw += yaw;
            const float tx = arenaFramePoint(turned, kAspect, target).x;
            CHECK(tx >= 0.55f - 1e-4f);
            CHECK(tx <= 0.8f + 1e-4f);
            CHECK(arenaLeadInFrame(rules, arenaLeadShares(turned, kAspect, body)));
        }
    MESSAGE("partner points a 24 degree turn frames and a 12 degree one cannot: " << gained);
    CHECK(gained > 0);
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


TEST_CASE("close frame: the contact share: within 2.5 m of the body of a target over 3 m tall or 1 m wide, out by 3.5 m") {
    // A tall thin one (radius 0): the body is the centre.
    CHECK(arenaCloseContactWant(kArenaClose, 2.f, 7.f, 0.f) == 1.f);
    CHECK(arenaCloseContactWant(kArenaClose, 2.5f, 7.f, 0.f) == 1.f);
    CHECK(arenaCloseContactWant(kArenaClose, 3.f, 7.f, 0.f) == doctest::Approx(0.5f));
    CHECK(arenaCloseContactWant(kArenaClose, 3.5f, 7.f, 0.f) == 0.f);
    // The 7 m giant, 2.2 m round: full at 4.7 m from its centre, none at 5.7 (the stand-off stills at 6, 8, 10 m are clear).
    CHECK(arenaCloseContactWant(kArenaClose, 2.6f, 7.f, 2.2f) == 1.f);
    CHECK(arenaCloseContactWant(kArenaClose, 4.7f, 7.f, 2.2f) == 1.f);
    CHECK(arenaCloseContactWant(kArenaClose, 5.2f, 7.f, 2.2f) == doctest::Approx(0.5f));
    CHECK(arenaCloseContactWant(kArenaClose, 5.7f, 7.f, 2.2f) <= 1e-6f);
    CHECK(arenaCloseContactWant(kArenaClose, 6.f, 7.f, 2.2f) == 0.f);
    // A normal enemy keeps the melee frame; exactly 3 m tall is not over it; a wide one counts at any height.
    CHECK(arenaCloseContactWant(kArenaClose, 1.f, 1.75f, 0.45f) == 0.f);
    CHECK(arenaCloseContactWant(kArenaClose, 1.f, 3.f, 0.45f) == 0.f);
    CHECK(arenaCloseContactWant(kArenaClose, 1.f, 3.01f, 0.45f) == 1.f);
    CHECK(arenaCloseContactWant(kArenaClose, 2.f, 2.f, 1.f) == 1.f);
    CHECK(arenaCloseContactWant(kArenaClose, kNaN, 7.f, 1.f) == 0.f);
    CHECK(arenaCloseContactWant(kArenaClose, -1.f, 7.f, 1.f) == 0.f);
    CHECK(arenaCloseContactWant(kArenaClose, 1.f, kNaN, kNaN) == 0.f);
    CHECK(arenaCloseContactWant(kArenaClose, 1.f, 7.f, kNaN) == 1.f);
    float last = 1.f;
    for (float d = 4.7f; d <= 5.7f; d += 0.02f) {
        const float w = arenaCloseContactWant(kArenaClose, d, 7.f, 2.2f);
        CHECK(w <= last + 1e-6f);
        last = w;
    }
}

TEST_CASE("close frame: the contact share blends in and out over 0.5 s at a steady rate, never past") {
    float c = 0.f;
    int frames = 0;
    while (c < 1.f && frames < 100) {
        const float next = arenaCloseContactStep(kArenaClose, c, 1.f, 1.f / 60.f);
        CHECK(next - c <= 1.f / 30.f + 1e-5f);
        CHECK(arenaCloseContactBlending(c, 1.f));
        c = next;
        ++frames;
    }
    CHECK(frames >= 29);
    CHECK(frames <= 31);
    CHECK(c == 1.f);
    CHECK_FALSE(arenaCloseContactBlending(c, 1.f));
    CHECK(arenaCloseContactStep(kArenaClose, 1.f, 0.f, 0.25f) == doctest::Approx(0.5f));
    CHECK(arenaCloseContactStep(kArenaClose, 0.4f, 0.5f, 1.f) == 0.5f);
    CHECK(arenaCloseContactStep(kArenaClose, 0.4f, kNaN, 0.1f) == 0.4f);
    CHECK(arenaCloseContactStep(kArenaClose, 0.4f, 1.f, 0.f) == 0.4f);
    CHECK(arenaCloseContactStep(kArenaClose, kNaN, 1.f, 0.f) == 0.f);
    CHECK_FALSE(arenaCloseContactBlending(0.f, 0.f));
    CHECK_FALSE(arenaCloseContactBlending(kNaN, 1.f));
}

TEST_CASE("close frame: contact swings 50-70 degrees round the lead, the view along the target's flank, mirrored") {
    const CameraPreset cp = closeCameraPreset();
    const ArenaCloseShape s = arenaCloseShape(kArenaClose, cp, 1.f, 1.f, 1.f, 1.8f);
    const ArenaCameraPose rig0 = closeRig(1.f, 1.f, 1.f, 1.8f);
    const float back = arenaEyeBack(rig0.arm, rig0.up, 0.f);
    const ArenaLeadBody body = closeBody(1.f, 1.f, 1.8f);
    for (float d : {1.f, 1.5f, 2.f, 2.5f, 3.f, 3.5f, 4.f}) {
        // A target up to 1.2 m round (its body 0.6 m clear of the lead's centre).
        const float rad = std::fmin(1.2f, d - 0.6f);
        CAPTURE(d);
        const float r = arenaCloseContactSwing(kArenaClose, 1.f, s.side, d, rad, s.fov, back);
        const float l = arenaCloseContactSwing(kArenaClose, -1.f, -s.side, d, rad, s.fov, back);
        CHECK(r <= -50.f);
        CHECK(r >= -70.f);
        CHECK(l == -r);
        // The swung pose: the lens out to the lead's right, the lead on the left of the frame, the arm unchanged.
        const ArenaCameraPose p = arenaPoseAtPitch(kArenaLook, closeRig(1.f, 1.f, 1.f, 1.8f, r), 0.f);
        CHECK(arenaCameraLens(p).y > 1.f);
        CHECK(p.arm == rig0.arm);
        const ArenaLeadShares lead = arenaLeadShares(p, kAspect, body);
        CHECK(spineOf(lead) < 0.4f);
        // The body's near outline stands in the frame right of the lead, against what lies beyond.
        const ArenaBodySpan span = arenaBodySpan(p, kAspect, ArenaPoint3{d, 0.f, 1.5f}, rad);
        CHECK(span.ahead);
        CHECK(span.left > lead.right);
        CHECK(span.left < 0.9f);
        // Inside the band the swing is the least that puts the centre at 0.88 and the outline at 0.55 or further.
        const float cx = arenaFramePoint(p, kAspect, ArenaPoint3{d, 0.f, 1.5f}).x;
        if (r > -69.9f && r < -50.1f) {
            CHECK(cx >= 0.87f);
            CHECK(span.left >= 0.54f);
            CHECK((cx == doctest::Approx(0.88f).epsilon(0.01) || span.left == doctest::Approx(0.55f).epsilon(0.02)));
        }
        // The left shoulder mirrors it.
        const ArenaCameraPose q = arenaPoseAtPitch(kArenaLook, closeRig(1.f, 1.f, -1.f, 1.8f, l), 0.f);
        CHECK(arenaBodySpan(q, kAspect, ArenaPoint3{d, 0.f, 1.5f}, rad).right == doctest::Approx(1.f - span.left).epsilon(1e-3));
    }
    // The 7 m giant, 2.2 m round, at the 2.6 m it is fought from: the swing is at the band's top; where its outline lands.
    const float g = arenaCloseContactSwing(kArenaClose, 1.f, s.side, 2.6f, 2.2f, s.fov, back);
    const ArenaCameraPose gp = arenaPoseAtPitch(kArenaLook, closeRig(1.f, 1.f, 1.f, 1.8f, g), 0.f);
    const ArenaBodySpan gs = arenaBodySpan(gp, kAspect, ArenaPoint3{2.6f, 0.f, 1.5f}, 2.2f);
    MESSAGE("giant at 2.6 m: swing ", g, ", body from ", gs.left, " to ", gs.right, ", lead right edge ", arenaLeadShares(gp, kAspect, body).right);
    CHECK(g == -70.f);
    // The swing scales with the shoulder's blend, so a swap passes through the line; NaN: the band's least.
    const float full = arenaCloseContactSwing(kArenaClose, 1.f, s.side, 2.5f, 1.f, s.fov, back);
    CHECK(arenaCloseContactSwing(kArenaClose, 0.5f, s.side, 2.5f, 1.f, s.fov, back) == doctest::Approx(0.5f * full));
    CHECK(arenaCloseContactSwing(kArenaClose, 0.f, s.side, 2.5f, 1.f, s.fov, back) == 0.f);
    CHECK(arenaCloseContactSwing(kArenaClose, 1.f, kNaN, 2.5f, 1.f, s.fov, back) == -50.f);
}

TEST_CASE("close frame: at a full contact share the lead's rule allows the 15 degree look-up on any contact target") {
    // A 3.01 m one and a wide one of a normal height: the cut takes the contact share as its tallness.
    for (float height : {3.01f, 1.75f, 7.f}) {
        const float tall = arenaCloseCutTallness(kArenaClose, height, 1.f);
        CHECK(tall == 1.f);
        for (float head : {1.4f, 1.6f, 1.7f, 1.8f}) {
            const ArenaLeadPitch p = arenaLeadPitch(kArenaLook, closeRules(1.f, tall, head), closeRig(1.f, tall, 1.f, head), kAspect,
                                                    closeBody(1.f, tall, head), 15.f);
            CAPTURE(height);
            CAPTURE(head);
            CHECK(p.kept == 2);
            CHECK(p.pitch == doctest::Approx(15.f));
        }
    }
    CHECK(arenaCloseCutTallness(kArenaClose, 1.75f, 0.f) == 0.f);
    CHECK(arenaCloseCutTallness(kArenaClose, 4.75f, 0.2f) == doctest::Approx(0.5f));
    CHECK(arenaCloseCutTallness(kArenaClose, 1.75f, kNaN) == 0.f);
}

TEST_CASE("close frame: the contact yaw blends the drawn turn from the frame's to the swing; the pitch held to 15-25") {
    CHECK(arenaCloseContactYaw(-12.f, -60.f, 0.f) == 0.f);
    CHECK(arenaCloseContactYaw(-12.f, -60.f, 1.f) == doctest::Approx(-48.f));
    CHECK(arenaCloseContactYaw(-12.f, -60.f, 0.5f) == doctest::Approx(-24.f));
    CHECK(arenaCloseContactYaw(-12.f, -60.f, 2.f) == doctest::Approx(-48.f));
    CHECK(arenaCloseContactYaw(kNaN, -60.f, 1.f) == 0.f);
    CHECK(arenaCloseContactYaw(-12.f, -60.f, kNaN) == 0.f);
    float last = 0.f;
    for (float c = 0.f; c <= 1.f; c += 0.05f) {
        const float y = arenaCloseContactYaw(-12.f, -60.f, c);
        CHECK(y <= last + 1e-5f);
        last = y;
    }
    CHECK(arenaCloseContactPitch(kArenaClose, 10.f, 0.f) == 10.f);
    CHECK(arenaCloseContactPitch(kArenaClose, 10.f, 1.f) == 15.f);
    CHECK(arenaCloseContactPitch(kArenaClose, 28.f, 1.f) == 25.f);
    CHECK(arenaCloseContactPitch(kArenaClose, 20.f, 1.f) == 20.f);
    CHECK(arenaCloseContactPitch(kArenaClose, 10.f, 0.5f) == doctest::Approx(12.5f));
    CHECK(arenaCloseContactPitch(kArenaClose, 10.f, kNaN) == 10.f);
    // Off contact the look-up is capped at +12 for any target (a ground partner stays over the bottom edge).
    CHECK(arenaCloseContactPitch(kArenaClose, 28.f, 0.f) == 12.f);
    CHECK(arenaCloseContactPitch(kArenaClose, 28.f, kNaN) == 12.f);
    CHECK(arenaCloseContactPitch(kArenaClose, 12.f, 0.f) == 12.f);
    CHECK(arenaCloseContactPitch(kArenaClose, -3.f, 0.f) == -3.f);
    CHECK(arenaCloseContactPitch(kArenaClose, 28.f, 0.5f) == doctest::Approx(18.5f));
    CHECK(arenaCloseContactPitch(kArenaClose, 28.f, 1.f) == 25.f);
    CHECK(std::isnan(arenaCloseContactPitch(kArenaClose, kNaN, 1.f)));
}

TEST_CASE("close frame: stand-off takes the least look-up that keeps the target's top under the frame's top margin") {
    const ArenaCloseRules& R = kArenaClose;
    const CameraPreset cp = closeCameraPreset();
    const float floorPitch = cp.rig.lockPitch;
    const auto standOff = [&](float shoulder, float d, float h, float& yAt, float& yBelow) {
        const ArenaCloseShape s = arenaCloseShape(R, cp, 0.f, 0.f, shoulder, 1.8f);
        const ArenaCameraPose rig0 = closeRig(0.f, 0.f, shoulder, 1.8f);
        const float back = arenaEyeBack(rig0.arm, rig0.up, arenaSplitPitch(kArenaLook, floorPitch).boom);
        const ArenaCameraPose rig = closeRig(0.f, 0.f, shoulder, 1.8f, arenaFrameTurnAt(kArenaFrame, s.side, d, s.screenX, s.fov, back));
        const ArenaPoint3 top{d, 0.f, h};
        const float p = arenaCloseStandOffPitch(R, kArenaLook, rig, kAspect, top, floorPitch);
        yAt = arenaFramePoint(arenaPoseAtPitch(kArenaLook, rig, p), kAspect, top).y;
        yBelow = arenaFramePoint(arenaPoseAtPitch(kArenaLook, rig, p - 0.1f), kAspect, top).y;
        return p;
    };
    for (const float shoulder : {1.f, -1.f}) {
        for (const float h : {1.8f, 2.4f, 3.5f, 5.f, 7.f}) {
            for (const float d : {4.f, 5.f, 6.f, 8.f, 10.f, 14.f}) {
                CAPTURE(shoulder);
                CAPTURE(h);
                CAPTURE(d);
                float yAt = 0.f, yBelow = 0.f;
                const float p = standOff(shoulder, d, h, yAt, yBelow);
                CHECK(p >= floorPitch);
                CHECK(p <= R.standOffPitchMax);
                if (p < R.standOffPitchMax - 1e-3f) CHECK(yAt <= R.standOffTopY + 1e-3f); // the top is kept...
                if (p > floorPitch + 0.1f) CHECK(yBelow > R.standOffTopY);                // ...by the least look-up
            }
        }
    }
    float yAt = 0.f, yBelow = 0.f;
    // A normal enemy at stand-off range is whole at the preset's own pitch: no look-up at all.
    CHECK(standOff(1.f, 5.f, 1.8f, yAt, yBelow) == floorPitch);
    CHECK(standOff(-1.f, 5.f, 2.4f, yAt, yBelow) == floorPitch);
    // The 7 m tall boss's top needs the whole +12 at 6-8 m (it stays above the margin there) and about +11.6 at 10 m.
    CHECK(standOff(1.f, 6.f, 7.f, yAt, yBelow) == R.standOffPitchMax);
    CHECK(standOff(1.f, 8.f, 7.f, yAt, yBelow) == R.standOffPitchMax);
    const float titan10 = standOff(1.f, 10.f, 7.f, yAt, yBelow);
    CHECK(titan10 > 10.5f);
    CHECK(titan10 < R.standOffPitchMax);
    // Edge inputs: the floor (held to the cap), and nothing to look at behind the lens.
    const ArenaCameraPose rig = closeRig(0.f, 0.f, 1.f, 1.8f);
    CHECK(arenaCloseStandOffPitch(R, kArenaLook, rig, kAspect, ArenaPoint3{8.f, 0.f, 7.f}, 20.f) == R.standOffPitchMax);
    CHECK(arenaCloseStandOffPitch(R, kArenaLook, rig, kAspect, ArenaPoint3{kNaN, 0.f, 7.f}, floorPitch) == floorPitch);
    CHECK(arenaCloseStandOffPitch(R, kArenaLook, rig, kAspect, ArenaPoint3{8.f, 0.f, 7.f}, kNaN) >= 0.f);
    CHECK(arenaCloseStandOffPitch(R, kArenaLook, rig, kNaN, ArenaPoint3{8.f, 0.f, 7.f}, floorPitch) == floorPitch);
    CHECK(arenaCloseStandOffPitch(R, kArenaLook, rig, kAspect, ArenaPoint3{-9.f, 0.f, 7.f}, floorPitch) == floorPitch);
}

TEST_CASE("close frame: the contact pitch takes its two ends apart, a low stand-off pitch and the lock's own look-up") {
    const ArenaCloseRules& R = kArenaClose;
    CHECK(arenaCloseContactPitch(R, 3.f, 28.f, 0.f) == 3.f);
    CHECK(arenaCloseContactPitch(R, 3.f, 28.f, 1.f) == 25.f);
    CHECK(arenaCloseContactPitch(R, 3.f, 10.f, 1.f) == 15.f);
    CHECK(arenaCloseContactPitch(R, 3.f, 28.f, 0.5f) == doctest::Approx(14.f));
    CHECK(arenaCloseContactPitch(R, 20.f, 28.f, 0.f) == 12.f);
    CHECK(arenaCloseContactPitch(R, 3.f, kNaN, 1.f) == 3.f);
    CHECK(arenaCloseContactPitch(R, 3.f, 28.f, kNaN) == 3.f);
    CHECK(std::isnan(arenaCloseContactPitch(R, kNaN, 28.f, 1.f)));
    // The one-pitch form is the two-ended one with both ends the same.
    for (const float p : {-3.f, 6.f, 12.f, 20.f, 28.f})
        for (const float s : {0.f, 0.3f, 1.f}) CHECK(arenaCloseContactPitch(R, p, s) == arenaCloseContactPitch(R, p, p, s));
}

TEST_CASE("close frame: tall contact drops the lens low and holds the look-up to 15, the lead kept from the waist up") {
    const ArenaCloseRules& R = kArenaClose;
    // The low share is the contact share on a target of contactLowTall m or more, and exactly 0 under it: the 3.5 m
    // target's contact framing is the shoulder lens's, unchanged.
    for (const float c : {0.f, 0.3f, 1.f}) {
        CHECK(arenaCloseContactLow(R, c, 3.5f) == 0.f);
        CHECK(arenaCloseContactLow(R, c, 7.f) == c);
    }
    CHECK(arenaCloseContactLow(R, 1.f, 4.f) == 1.f);
    CHECK(arenaCloseContactLow(R, kNaN, 7.f) == 0.f);
    CHECK(arenaCloseContactLow(R, 1.f, kNaN) == 0.f);
    // At share 0 the pitch, the cut and the lead's rule are exactly as given.
    const ArenaLeadFrameRules presetRules = cameraPresetLeadRules(closeCameraPreset());
    for (const float p : {-2.f, 12.f, 25.f}) CHECK(arenaCloseContactLowPitch(R, p, 0.f) == p);
    CHECK(arenaCloseContactLowCut(R, 1.25f, 1.8f, 0.f) == 1.25f);
    CHECK(arenaCloseContactLowLeadRules(presetRules, 0.f).feetMin == presetRules.feetMin);
    CHECK(arenaCloseContactLowLeadRules(presetRules, 0.f).headMax == presetRules.headMax);
    // At share 1: the look-up at most 15, a lower one untouched; the cut at the waist; the feet margin the lock's.
    CHECK(arenaCloseContactLowPitch(R, 25.f, 1.f) == R.contactLowPitchMax);
    CHECK(arenaCloseContactLowPitch(R, 10.f, 1.f) == 10.f);
    CHECK(arenaCloseContactLowPitch(R, 25.f, 0.5f) == doctest::Approx(20.f));
    CHECK(std::isnan(arenaCloseContactLowPitch(R, kNaN, 1.f)));
    CHECK(arenaCloseContactLowPitch(R, 25.f, kNaN) == 25.f);
    CHECK(arenaCloseContactLowCut(R, 1.25f, 1.8f, 1.f) == doctest::Approx(R.contactLowWaist * 1.8f));
    CHECK(arenaCloseContactLowLeadRules(presetRules, 1.f).feetMin == doctest::Approx(kArenaLeadFrame.feetMin));
    CHECK(arenaCloseContactLowLeadRules(presetRules, 1.f).headMax == presetRules.headMax);
    const ArenaLowRules low = arenaCloseContactLowRules(R, 0.55f);
    CHECK(low.eye == R.contactLowEye);
    CHECK(low.side == 0.55f);
    // The frame it gives, on the shoulder rig at a full melee share, either shoulder: the lens 0.9 m up at 15 degrees
    // keeps the lead's waist above the bottom margin and the head under the close preset's head line (75%), and the
    // lead's rule leaves that pitch as asked. Not under 55%: that needs about 17.5 degrees at this eye (the cap holds).
    for (const float shoulder : {-1.f, 1.f}) {
        CAPTURE(shoulder);
        const ArenaCameraPose rig = closeRig(1.f, 1.f, shoulder, 1.8f);
        const ArenaLowRules lr = arenaCloseContactLowRules(R, rig.side);
        const ArenaLeadBody body{1.8f, 0.35f, arenaCloseContactLowCut(R, 1.25f, 1.8f, 1.f)};
        const ArenaLeadFrameRules rules = arenaCloseContactLowLeadRules(presetRules, 1.f);
        const ArenaCameraPose pose = arenaPoseAtPitch(kArenaLook, rig, R.contactLowPitchMax, 1.f, lr);
        const ArenaLeadShares s = arenaLeadShares(pose, kAspect, body);
        CHECK(s.ahead);
        CHECK(s.feet >= rules.feetMin);
        CHECK(s.head <= rules.headMax);
        CHECK(s.head > 0.55f);
        CHECK(pose.side == doctest::Approx(rig.side)); // straight down: the shoulder's own side
        const ArenaLeadPitch held = arenaLeadPitch(kArenaLook, rules, rig, kAspect, body, R.contactLowPitchMax, 1.f, lr);
        CHECK(held.pitch == doctest::Approx(R.contactLowPitchMax));
        MESSAGE("tall contact low lens, shoulder " << shoulder << ": waist at " << s.feet << ", head at " << s.head
                                                   << " of the height; width " << s.left << ".." << s.right);
    }
}
TEST_CASE("close frame: a wall-shortened lens does not dive to keep the cut: the pitch holds at -6 and the cut gives way") {
    const ArenaCloseRules& R = kArenaClose;
    // The dive itself, on the lead's rule alone (melee share 1, asked +6): kept as asked at the full arm, about -13 at
    // 80% of it, about -29 at 60%.
    const ArenaLeadBody body = closeBody(1.f, 0.f, 1.8f);
    const ArenaLeadFrameRules rules = closeRules(1.f, 0.f, 1.8f);
    const auto rulePitch = [&](float wall) {
        return arenaLeadPitch(kArenaLook, rules, closeRig(1.f, 0.f, 1.f, 1.8f, 0.f, wall), kAspect, body, 6.f).pitch;
    };
    CHECK(rulePitch(1.f) == 6.f);
    CHECK(rulePitch(0.8f) < -10.f);
    CHECK(rulePitch(0.6f) < -25.f);
    // Held: unchanged when the rule keeps the asked pitch (or moves it up), the floor when it dives under it.
    CHECK(arenaCloseWallPitch(R, rulePitch(1.f), 6.f) == 6.f);
    CHECK(arenaCloseWallPitch(R, 9.f, 6.f) == 9.f);
    CHECK(arenaCloseWallPitch(R, -4.f, 6.f) == -4.f);
    CHECK(arenaCloseWallPitch(R, rulePitch(0.8f), 6.f) == R.wallPitchMin);
    CHECK(arenaCloseWallPitch(R, rulePitch(0.6f), 6.f) == R.wallPitchMin);
    // An ask under the floor is the floor; NaN inputs.
    CHECK(arenaCloseWallPitch(R, -30.f, -9.f) == -9.f);
    CHECK(arenaCloseWallPitch(R, -30.f, kNaN) == R.wallPitchMin);
    CHECK(std::isnan(arenaCloseWallPitch(R, kNaN, 6.f)));
    // At the floor the lead's head stays under the close preset's head line down to 40% of the arm, the body inside
    // the width; its feet are out of the frame (the cut gives way).
    for (const float wall : {0.8f, 0.6f, 0.4f}) {
        CAPTURE(wall);
        const ArenaLeadShares s = arenaLeadShares(arenaPoseAtPitch(kArenaLook, closeRig(1.f, 0.f, 1.f, 1.8f, 0.f, wall), R.wallPitchMin),
                                                  kAspect, body);
        CHECK(s.ahead);
        CHECK(s.head <= rules.headMax);
        CHECK(s.left > 0.f);
        CHECK(s.right < 1.f);
        MESSAGE("wall " << wall << " at " << R.wallPitchMin << ": cut at " << s.feet << ", head at " << s.head << " of the height");
    }
}

namespace {
// A target's head `d` m out from the lead, straight behind a point of the lead (`across` m to its right, `head` m up)
// as the lens sees it: the lens, that point and the target's head in one line.
ArenaPoint3 behindHead(const ArenaCameraPose& pose, float d, float head, float across = 0.f) {
    const ArenaLensAt lens = arenaCameraLens(pose);
    const float lz = pose.pivot + lens.z; // the lens's height over the floor
    const float dx = -lens.x, dy = across - lens.y, dz = head - lz;
    const float flat = std::sqrt(dx * dx + dy * dy);
    const float k = (flat + d) / flat;
    return ArenaPoint3{lens.x + dx * k, lens.y + dy * k, lz + dz * k};
}
} // namespace

TEST_CASE("close frame: stand-off clearance moves the lens out on its shoulder, then up, until the target's head clears the lead") {
    const ArenaCloseRules& R = kArenaClose;
    CHECK(R.clearSideMax == 0.85f);
    CHECK(R.clearMargin == 0.05f);
    CHECK(R.clearEase == 0.25f);
    int sideUsed = 0, raised = 0, fallbacks = 0;
    for (const float shoulder : {1.f, -1.f}) {
        for (const float d : {3.5f, 5.f, 6.f, 8.f, 10.f}) {
            for (const float head : {1.6f, 1.8f}) {
                CAPTURE(shoulder);
                CAPTURE(d);
                CAPTURE(head);
                const ArenaCameraPose pose = lockPose(0.f, shoulder, d, head);
                const ArenaLeadBody body = closeBody(0.f, 0.f, head);
                const ArenaLeadBody clearBody = arenaCloseClearBody(R, body); // the box the clearance is judged by (0.45 m)
                // A target at the lock's own place, clear of the lead: nothing asked.
                const ArenaPoint3 framed{d, 0.f, 1.7f};
                CHECK_FALSE(arenaCloseHeadHidden(pose, kAspect, body, framed, R.clearMargin));
                const ArenaCloseClear none = arenaCloseStandOffClear(R, pose, kAspect, body, framed);
                CHECK(none.side == 0.f);
                CHECK(none.pivot == 0.f);
                // A target behind the lead: its head's top dead centre (the pivot clears it), a low point at the box's
                // edge further out on the shoulder's side (the side clears it), or its near shoulder 0.3 m out (no pair
                // clears the widened box: the lens stays, though the most move would show the point past the lead's
                // own body; the trade ruled with item 3).
              for (const int kind : {0, 1, 2}) {
                CAPTURE(kind);
                const ArenaPoint3 hid = kind == 2   ? behindHead(pose, d, head - 0.6f, 0.3f * shoulder)
                                        : kind == 1 ? behindHead(pose, d, head - 0.6f, 0.55f * shoulder)
                                                    : behindHead(pose, d, head - 0.05f);
                REQUIRE(arenaCloseHeadHidden(pose, kAspect, clearBody, hid, R.clearMargin));
                const ArenaCloseClear c = arenaCloseStandOffClear(R, pose, kAspect, body, hid);
                CHECK(c.side * shoulder >= 0.f);                                // out on its own shoulder, if at all
                CHECK(std::fabs(pose.side + c.side) <= R.clearSideMax + 1e-4f); // never past the most
                CHECK(c.pivot >= 0.f);
                CHECK(c.pivot <= R.clearPivotUp + 1e-4f);
                ArenaCameraPose cleared = pose;
                cleared.side += c.side;
                cleared.pivot += c.pivot;
                const float room = R.clearSideMax - std::fabs(pose.side);
                const bool stayed = c.side == 0.f && c.pivot == 0.f; // no pair cleared it: the lens stays
                fallbacks += stayed ? 1 : 0;
                CHECK(stayed == (kind == 2));
                if (kind == 2) { // what the old most move would have shown: the point past the lead's own body
                    ArenaCameraPose most = pose;
                    most.side = shoulder * R.clearSideMax;
                    most.pivot += R.clearPivotUp;
                    CHECK_FALSE(arenaCloseHeadHidden(most, kAspect, body, hid, 0.f));
                }
                if (kind == 1) CHECK(c.side != 0.f);
                if (!stayed) {
                    CHECK_FALSE(arenaCloseHeadHidden(cleared, kAspect, clearBody, hid, R.clearMargin));
                    CHECK_FALSE(arenaCloseHeadHidden(cleared, kAspect, body, hid, 0.f)); // the head itself is seen
                }
                // The least move: no pair of steps with a smaller sum clears it (none at all when it asked nothing).
                const float sum = stayed ? 1e9f : std::fabs(c.side) + c.pivot;
                const int sideSteps = static_cast<int>(std::ceil(room / R.clearStep - 1e-4f));
                const int upSteps = static_cast<int>(std::ceil(R.clearPivotUp / R.clearStep - 1e-4f));
                for (int ks = 0; ks <= sideSteps; ++ks)
                    for (int kp = 0; kp <= upSteps; ++kp) {
                        const float add = std::min(room, ks * R.clearStep);
                        const float up = std::min(R.clearPivotUp, kp * R.clearStep);
                        if (add + up >= sum - 1e-4f) continue;
                        ArenaCameraPose less = pose;
                        less.side += shoulder * add;
                        less.pivot += up;
                        CHECK(arenaCloseHeadHidden(less, kAspect, clearBody, hid, R.clearMargin));
                    }
                sideUsed += c.side != 0.f ? 1 : 0;
                raised += c.pivot > 0.f ? 1 : 0;
                // The lead stays in the frame's width and under its top (the lead's rule pitches for the cut).
                const ArenaLeadShares s = arenaLeadShares(cleared, kAspect, body);
                CHECK(s.ahead);
                CHECK(s.left >= 0.f);
                CHECK(s.right <= 1.f);
                CHECK(s.head <= 1.f);
                MESSAGE("shoulder " << shoulder << " d " << d << " head " << head << std::string(kind == 2 ? " near shoulder" : kind == 1 ? " box edge" : " dead centre")
                                     << ": side " << c.side << " m, pivot +" << c.pivot << " m");
              }
            }
        }
    }
    MESSAGE("cleared with the side moved " << sideUsed << ", with the pivot raised " << raised << ", none cleared " << fallbacks);
    CHECK(fallbacks == 20); // the near-shoulder points, each one
    CHECK(raised > 0);
    CHECK(sideUsed == 20);  // the box-edge points, each one
    // Edge inputs.
    const ArenaCameraPose pose = lockPose(0.f, 1.f, 5.f);
    const ArenaLeadBody body = closeBody(0.f, 0.f, 1.8f);
    const ArenaPoint3 hid = behindHead(pose, 5.f, 1.75f);
    CHECK_FALSE(arenaCloseHeadHidden(pose, kNaN, body, hid, 0.f));
    CHECK_FALSE(arenaCloseHeadHidden(pose, kAspect, body, ArenaPoint3{kNaN, 0.f, 1.f}, 0.f));
    CHECK_FALSE(arenaCloseHeadHidden(pose, kAspect, body, ArenaPoint3{-6.f, 0.f, 1.f}, 0.f)); // behind the lens
    CHECK(arenaCloseHeadHidden(pose, kAspect, body, hid, kNaN));                               // a NaN margin reads as 0
    ArenaCameraPose bad = pose;
    bad.side = kNaN;
    CHECK(arenaCloseStandOffClear(R, bad, kAspect, body, hid).side == 0.f);
    ArenaCameraPose wide = pose;
    wide.side = 0.9f; // already past the most: no side left, the pivot does what it can
    CHECK(arenaCloseStandOffClear(R, wide, kAspect, body, behindHead(wide, 5.f, 1.75f)).side == 0.f);
    // No pair clears it (a head low behind the lead's chest, the most move still hides it): the lens stays, 0 and 0, not
    // the most move.
    const ArenaPoint3 low = behindHead(pose, 5.f, 1.0f);
    ArenaCameraPose most = pose;
    most.side = pose.side < 0.f ? -R.clearSideMax : R.clearSideMax;
    most.pivot += R.clearPivotUp;
    REQUIRE(arenaCloseHeadHidden(most, kAspect, arenaCloseClearBody(R, body), low, R.clearMargin));
    const ArenaCloseClear stay = arenaCloseStandOffClear(R, pose, kAspect, body, low);
    CHECK(stay.side == 0.f);
    CHECK(stay.pivot == 0.f);
}

TEST_CASE("close frame: the clearance eases in and out over 0.25 s") {
    const ArenaCloseRules& R = kArenaClose;
    float v = 0.f;
    for (int i = 0; i < 15; ++i) v = arenaCloseClearStep(R, v, 0.3f, 1.f / 60.f); // 0.25 s: one time constant
    CHECK(v == doctest::Approx(0.3f * (1.f - std::exp(-1.f))).epsilon(0.01));
    for (int i = 0; i < 120; ++i) v = arenaCloseClearStep(R, v, 0.3f, 1.f / 60.f);
    CHECK(v == doctest::Approx(0.3f).epsilon(0.001));
    for (int i = 0; i < 15; ++i) v = arenaCloseClearStep(R, v, 0.f, 1.f / 60.f);
    CHECK(v == doctest::Approx(0.3f * std::exp(-1.f)).epsilon(0.01));
    CHECK(v > 0.f);
    CHECK(arenaCloseClearStep(R, 0.2f, 0.f, 0.f) == 0.2f);
    CHECK(arenaCloseClearStep(R, 0.2f, kNaN, 0.1f) == 0.2f);
    CHECK(arenaCloseClearStep(R, 0.2f, 0.f, kNaN) == 0.2f);
    CHECK(arenaCloseClearStep(R, kNaN, 0.f, 0.1f) == 0.f);
}

TEST_CASE("close frame: the framing check takes the lead in its near-side third, the target in its band, the partner in frame") {
    const ArenaCloseRules& R = kArenaClose;
    CHECK(R.frameLeadSlack == 0.05f);
    CHECK(R.framePass == 0.95f);
    CHECK(R.frameNearDistance == 3.f);
    CHECK(R.frameNearTargetMin == 0.45f);
    const ArenaFramePoint mateIn{0.5f, 0.5f, true};
    // The lock's own frame at stand-off and melee distances, either shoulder: every term holds.
    for (const float shoulder : {1.f, -1.f}) {
        for (const float d : {2.5f, 3.5f, 5.f, 6.f, 8.f, 10.f}) {
            CAPTURE(shoulder);
            CAPTURE(d);
            const float share = arenaCloseMeleeWant(R, d);
            const ArenaCameraPose pose = lockPose(share, shoulder, d);
            const ArenaFramePoint lead = arenaFramePoint(pose, kAspect, ArenaPoint3{0.f, 0.f, 1.f});
            const ArenaFramePoint chest = arenaFramePoint(pose, kAspect, ArenaPoint3{d, 0.f, 1.3f});
            REQUIRE(lead.ahead);
            REQUIRE(chest.ahead);
            const ArenaCloseFrameCheck c = arenaCloseFrameCheck(R, shoulder, lead.x, chest.x, d, true, mateIn);
            MESSAGE("shoulder " << shoulder << " d " << d << ": lead " << lead.x << ", target " << chest.x);
            CHECK(c.lead);
            CHECK(c.target);
            CHECK(c.all);
        }
    }
    // The bands, right shoulder and mirrored.
    CHECK(arenaCloseFrameCheck(R, 1.f, 0.30f, 0.60f, 5.f, false, ArenaFramePoint{}).all);
    CHECK(arenaCloseFrameCheck(R, 1.f, 0.38f, 0.60f, 5.f, false, ArenaFramePoint{}).lead);
    CHECK_FALSE(arenaCloseFrameCheck(R, 1.f, 0.40f, 0.60f, 5.f, false, ArenaFramePoint{}).lead);  // past the third and its slack
    CHECK_FALSE(arenaCloseFrameCheck(R, 1.f, 0.70f, 0.60f, 5.f, false, ArenaFramePoint{}).lead);  // the far side
    CHECK_FALSE(arenaCloseFrameCheck(R, 1.f, -0.01f, 0.60f, 5.f, false, ArenaFramePoint{}).lead); // off the frame
    CHECK_FALSE(arenaCloseFrameCheck(R, 1.f, 0.30f, 0.50f, 5.f, false, ArenaFramePoint{}).target);
    CHECK_FALSE(arenaCloseFrameCheck(R, 1.f, 0.30f, 0.85f, 5.f, false, ArenaFramePoint{}).target);
    CHECK(arenaCloseFrameCheck(R, -1.f, 0.70f, 0.40f, 5.f, false, ArenaFramePoint{}).all);
    CHECK(arenaCloseFrameCheck(R, -1.f, 0.62f, 0.40f, 5.f, false, ArenaFramePoint{}).lead);
    CHECK_FALSE(arenaCloseFrameCheck(R, -1.f, 0.30f, 0.40f, 5.f, false, ArenaFramePoint{}).lead);
    CHECK_FALSE(arenaCloseFrameCheck(R, -1.f, 0.70f, 0.60f, 5.f, false, ArenaFramePoint{}).target);
    CHECK(arenaCloseFrameCheck(R, 0.f, 0.30f, 0.60f, 5.f, false, ArenaFramePoint{}).all); // 0 counts as the right shoulder
    // The partner: anywhere in the frame, judged only when on.
    CHECK(arenaCloseFrameCheck(R, 1.f, 0.30f, 0.60f, 5.f, true, ArenaFramePoint{0.02f, 0.9f, true}).all);
    CHECK_FALSE(arenaCloseFrameCheck(R, 1.f, 0.30f, 0.60f, 5.f, true, ArenaFramePoint{1.1f, 0.5f, true}).partner);
    CHECK_FALSE(arenaCloseFrameCheck(R, 1.f, 0.30f, 0.60f, 5.f, true, ArenaFramePoint{0.5f, -0.1f, true}).partner);
    CHECK_FALSE(arenaCloseFrameCheck(R, 1.f, 0.30f, 0.60f, 5.f, true, ArenaFramePoint{0.5f, 0.5f, false}).partner); // behind the lens
    CHECK(arenaCloseFrameCheck(R, 1.f, 0.30f, 0.60f, 5.f, false, ArenaFramePoint{0.5f, 0.5f, false}).partner);
    // Under 3 m the band starts at 0.45 (ruled 11-16): the target sits near the middle at melee range.
    CHECK(arenaCloseFrameCheck(R, 1.f, 0.30f, 0.50f, 2.5f, false, ArenaFramePoint{}).target);
    CHECK(arenaCloseFrameCheck(R, 1.f, 0.30f, 0.45f, 2.9f, false, ArenaFramePoint{}).target);
    CHECK_FALSE(arenaCloseFrameCheck(R, 1.f, 0.30f, 0.44f, 2.9f, false, ArenaFramePoint{}).target);
    CHECK_FALSE(arenaCloseFrameCheck(R, 1.f, 0.30f, 0.50f, 3.f, false, ArenaFramePoint{}).target);  // 3 m: the far band
    CHECK_FALSE(arenaCloseFrameCheck(R, 1.f, 0.30f, 0.85f, 1.f, false, ArenaFramePoint{}).target);  // the top holds
    CHECK(arenaCloseFrameCheck(R, -1.f, 0.70f, 0.50f, 2.f, false, ArenaFramePoint{}).target);       // mirrored
    CHECK_FALSE(arenaCloseFrameCheck(R, -1.f, 0.70f, 0.56f, 2.f, false, ArenaFramePoint{}).target);
    CHECK_FALSE(arenaCloseFrameCheck(R, 1.f, 0.30f, 0.50f, kNaN, false, ArenaFramePoint{}).target); // NaN: the far band
    // NaN shares fail their own term only.
    const ArenaCloseFrameCheck n = arenaCloseFrameCheck(R, 1.f, kNaN, kNaN, 5.f, true, ArenaFramePoint{kNaN, 0.5f, true});
    CHECK_FALSE(n.lead);
    CHECK_FALSE(n.target);
    CHECK_FALSE(n.partner);
    CHECK_FALSE(n.all);
}

TEST_CASE("close clear: the lead's box is taken at 0.45 m or its own radius, the wider") {
    const ArenaCloseRules& R = kArenaClose;
    CHECK(R.clearRadius == 0.45f);
    ArenaLeadBody b;
    CHECK(arenaCloseClearBody(R, b).radius == 0.45f);
    b.radius = 0.6f;
    CHECK(arenaCloseClearBody(R, b).radius == 0.6f);
    b.radius = kNaN;
    CHECK(arenaCloseClearBody(R, b).radius == 0.45f);
    b.radius = 0.35f;
    b.head = 1.7f;
    b.cut = 0.4f;
    const ArenaLeadBody w = arenaCloseClearBody(R, b);
    CHECK(w.head == 1.7f);
    CHECK(w.cut == 0.4f);
}

TEST_CASE("close frame: the clearance holds its ask at the edge of a move, so a swaying head does not flip it") {
    const ArenaCloseRules& R = kArenaClose;
    CHECK(R.clearHold == 0.02f);
    int edges = 0;
    for (const float shoulder : {1.f, -1.f})
        for (const float d : {3.5f, 6.f, 10.f})
            for (const float h : {1.2f, 1.7f, 1.88f}) {
                CAPTURE(shoulder);
                CAPTURE(d);
                CAPTURE(h);
                const ArenaCameraPose pose = lockPose(0.f, shoulder, d, 1.8f);
                const ArenaLeadBody body = closeBody(0.f, 0.f, 1.8f);
                // The first place across where the fresh ask jumps (from nothing to a move, or between moves).
                float edgeA = -1.f;
                ArenaCloseClear prev = arenaCloseStandOffClear(R, pose, kAspect, body, behindHead(pose, d, h, 0.f));
                for (float a = 0.005f; a <= 0.7f; a += 0.005f) {
                    const ArenaCloseClear c = arenaCloseStandOffClear(R, pose, kAspect, body, behindHead(pose, d, h, a * shoulder));
                    if (std::fabs(c.side - prev.side) > 0.04f || std::fabs(c.pivot - prev.pivot) > 0.04f) {
                        edgeA = a;
                        break;
                    }
                    prev = c;
                }
                if (edgeA < 0.f) continue;
                ++edges;
                for (const float amp : {0.01f, 0.03f}) {
                    CAPTURE(amp);
                    // 4 s at 60 Hz, the head swaying `amp` m across the edge at 2 Hz; drawn through the 0.25 s ease.
                    int flipsFresh = 0, flipsHeld = 0;
                    float s = 0.f, p = 0.f, lo = 9.f, hi = -9.f, plo = 9.f, phi = -9.f;
                    ArenaCloseClear fresh0, held;
                    for (int i = 0; i < 240; ++i) {
                        const float t = static_cast<float>(i) / 60.f;
                        const float a = edgeA + amp * std::sin(6.2832f * 2.f * t);
                        const ArenaPoint3 at = behindHead(pose, d, h, a * shoulder);
                        const ArenaCloseClear fresh = arenaCloseStandOffClear(R, pose, kAspect, body, at);
                        const ArenaCloseClear c = arenaCloseStandOffClear(R, pose, kAspect, body, at, held);
                        if (i > 0) {
                            flipsFresh += fresh.side != fresh0.side || fresh.pivot != fresh0.pivot ? 1 : 0;
                            flipsHeld += c.side != held.side || c.pivot != held.pivot ? 1 : 0;
                        }
                        fresh0 = fresh;
                        held = c;
                        s = arenaCloseClearStep(R, s, std::fabs(c.side), 1.f / 60.f);
                        p = arenaCloseClearStep(R, p, c.pivot, 1.f / 60.f);
                        if (t >= 2.f) { // past the ease-in (0.25 s constant)
                            lo = std::min(lo, s);
                            hi = std::max(hi, s);
                            plo = std::min(plo, p);
                            phi = std::max(phi, p);
                        }
                    }
                    MESSAGE("shoulder " << shoulder << " d " << d << " h " << h << " edge " << edgeA << " sway " << amp << ": flips fresh "
                                        << flipsFresh << ", held " << flipsHeld << "; drawn swing side " << hi - lo << " m, pivot " << phi - plo << " m");
                    CHECK(flipsFresh >= 8); // without the hold it chatters
                    CHECK(flipsHeld <= 1);  // with it, at most the one step onto the move
                    CHECK(hi - lo < 0.005f);
                    CHECK(phi - plo < 0.005f);
                }
            }
    CHECK(edges >= 12);
    // The hold lets go: a head clear of the bare view by more than the hold asks nothing, whatever was held.
    const ArenaCameraPose pose = lockPose(0.f, 1.f, 5.f);
    const ArenaLeadBody body = closeBody(0.f, 0.f, 1.8f);
    const ArenaCloseClear big{0.3f, 0.3f};
    const ArenaCloseClear gone = arenaCloseStandOffClear(R, pose, kAspect, body, ArenaPoint3{5.f, 0.f, 1.7f}, big);
    CHECK(gone.side == 0.f);
    CHECK(gone.pivot == 0.f);
    // A held move that no longer clears is dropped for a fresh search (here: none clears, so nothing).
    const ArenaPoint3 low = behindHead(pose, 5.f, 1.0f);
    const ArenaCloseClear dropped = arenaCloseStandOffClear(R, pose, kAspect, body, low, ArenaCloseClear{0.05f, 0.f});
    CHECK(dropped.side == 0.f);
    CHECK(dropped.pivot == 0.f);
    // The held side's size is taken on this pose's shoulder: a left-shoulder pose keeps it on the left.
    const ArenaCameraPose left = lockPose(0.f, -1.f, 5.f);
    const ArenaPoint3 edgeL = behindHead(left, 5.f, 1.2f, -0.55f);
    const ArenaCloseClear askL = arenaCloseStandOffClear(R, left, kAspect, body, edgeL);
    REQUIRE(askL.side < 0.f);
    const ArenaCloseClear keptL = arenaCloseStandOffClear(R, left, kAspect, body, edgeL, ArenaCloseClear{-askL.side, askL.pivot});
    CHECK(keptL.side == askL.side);
    // NaN held: no hold, the fresh ask.
    const ArenaCloseClear nanHeld = arenaCloseStandOffClear(R, left, kAspect, body, edgeL, ArenaCloseClear{kNaN, 0.f});
    CHECK(nanHeld.side == askL.side);
}
