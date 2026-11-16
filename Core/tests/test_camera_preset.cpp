#include "ftcore/ArenaCamera.h"
#include "ftcore/ArenaLockFrame.h"
#include "ftcore/ArenaLook.h"
#include "ftcore/ArenaZone.h"
#include "ftcore/CameraPreset.h"
#include "TestAssets.h"
#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <string>

using namespace ftcore;

namespace {

constexpr float kAspect = 16.f / 9.f;

bool sameRig(const ZoneCamera& a, const ZoneCamera& b) {
    return a.arm == b.arm && a.pivot == b.pivot && a.side == b.side && a.up == b.up && a.fov == b.fov &&
           a.lockPitch == b.lockPitch && a.frameTurnCapDeg == b.frameTurnCapDeg;
}

bool samePreset(const CameraPreset& a, const CameraPreset& b) {
    return sameRig(a.rig, b.rig) && a.cutAt == b.cutAt && a.cutMin == b.cutMin && a.headMax == b.headMax &&
           a.targetX == b.targetX && a.lagSeconds == b.lagSeconds && a.lowMax == b.lowMax;
}

CameraPresets shippedPresets() {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/camera_presets.json", text)) FAIL("Data/Battle/camera_presets.json not found");
    CameraPresets p;
    std::string error;
    if (!importCameraPresets(text, p, &error)) FAIL(error);
    return p;
}

// A preset's rig as a pose at pitch 0 (the arm as given), yaw `yaw`.
ArenaCameraPose rigOf(const ZoneCamera& z, float yaw = 0.f) {
    ArenaCameraPose p;
    p.yaw = yaw;
    p.arm = z.arm;
    p.pivot = z.pivot;
    p.side = z.side;
    p.up = z.up;
    p.fov = z.fov;
    return p;
}

ArenaLeadBody bodyOf(const CameraPreset& p) {
    ArenaLeadBody b;
    b.cut = p.cutAt;
    return b;
}

} // namespace

TEST_CASE("camera preset: classic is the zone's rig and the lead's rule exactly; close is the near lens") {
    const CameraPresets d{};
    CHECK(sameRig(d.classic.rig, ZoneCamera{}));
    CHECK(d.classic.cutAt == 0.f);
    CHECK(d.classic.cutMin == kArenaLeadFrame.feetMin);
    CHECK(d.classic.headMax == kArenaLeadFrame.headMax);
    CHECK(d.classic.targetX == 0.5f);
    CHECK(d.classic.lagSeconds == 0.f);
    CHECK(d.classic.lowMax == 1.f);
    const ArenaLeadFrameRules r = cameraPresetLeadRules(d.classic);
    CHECK(r.feetMin == kArenaLeadFrame.feetMin);
    CHECK(r.headMax == kArenaLeadFrame.headMax);
    CHECK(r.widthMid == kArenaLeadFrame.widthMid);

    const CameraPreset& c = d.close;
    CHECK(c.rig.arm == 2.6f);
    CHECK(c.rig.pivot == 1.35f);
    CHECK(c.rig.side == 0.2f);
    CHECK(c.rig.up == 0.1f);
    CHECK(c.rig.fov == 60.f);
    CHECK(c.rig.lockPitch == -2.f);
    CHECK(c.cutAt == 0.6f);
    CHECK(c.cutMin == -0.04f);
    CHECK(c.lowMax == 0.f);
    CHECK(c.headMax == 0.75f);
    CHECK(c.targetX == 0.6f);
    CHECK(c.lagSeconds == 0.08f);
    const ArenaLeadFrameRules rc = cameraPresetLeadRules(c);
    CHECK(rc.feetMin == -0.04f);
    CHECK(rc.headMax == 0.75f);
    CHECK(rc.widthMid == kArenaLeadFrame.widthMid);

    CHECK(&cameraPreset(d, CameraPresetId::Classic) == &d.classic);
    CHECK(&cameraPreset(d, CameraPresetId::Close) == &d.close);
    CHECK(std::string(cameraPresetName(CameraPresetId::Classic)) == "classic");
    CHECK(std::string(cameraPresetName(CameraPresetId::Close)) == "close");
}

TEST_CASE("camera preset: the shipped file reads to the built-in presets to the bit") {
    const CameraPresets shipped = shippedPresets();
    const CameraPresets d{};
    CHECK(samePreset(shipped.classic, d.classic));
    CHECK(samePreset(shipped.close, d.close));
}

TEST_CASE("camera preset: classic applied to the zone's fit gives the fit back; any preset keeps the zone's framing cap") {
    const CameraPresets d{};
    const ZoneCamera fit{};
    CHECK(sameRig(applyCameraPreset(fit, d.classic), fit));

    ZoneCamera capped{};
    capped.frameTurnCapDeg = 12.5f;
    const ZoneCamera c = applyCameraPreset(capped, d.close);
    CHECK(c.frameTurnCapDeg == 12.5f);
    CHECK(c.arm == d.close.rig.arm);
    CHECK(c.lockPitch == d.close.rig.lockPitch);
    CHECK(sameRig(applyCameraPreset(capped, d.classic), capped));
}

TEST_CASE("camera preset: a bad file is refused with a reason and changes nothing") {
    const CameraPresets before{};
    const auto refused = [&](const char* json, const char* says) {
        CameraPresets p = before;
        std::string error;
        CHECK_FALSE(importCameraPresets(json, p, &error));
        CHECK(error.find(says) != std::string::npos);
        CHECK(samePreset(p.classic, before.classic));
        CHECK(samePreset(p.close, before.close));
    };
    refused("not json", "");
    refused("[1, 2]", "not an object");
    refused(R"({"version": 1, "wide": {"arm": 4}})", "camera_presets.wide: not a known preset");
    refused(R"({"close": {"arm": 3, "zoom": 2}})", "camera_presets.close.zoom: not a known tunable");
    refused(R"({"close": {"arm": 0.5}})", "camera_presets.close.arm: out of range");
    refused(R"({"classic": {"fov": 130}})", "camera_presets.classic.fov: out of range");
    refused(R"({"close": {"targetX": 0.8}})", "camera_presets.close.targetX: out of range");
    refused(R"({"close": {"lagSeconds": -0.1}})", "camera_presets.close.lagSeconds: out of range");
    refused(R"({"close": {"lowMax": 1.5}})", "camera_presets.close.lowMax: out of range");
    refused(R"({"close": {"cutMin": -0.2}})", "camera_presets.close.cutMin: out of range");
    refused(R"({"close": {"cutAt": "high"}})", "camera_presets.close.cutAt: not a number");
    refused(R"({"close": 3})", "camera_presets.close: not an object");
    // A good preset first and a bad one after: the good one is not kept either.
    refused(R"({"classic": {"arm": 9}, "close": {"headMax": 0.1}})", "camera_presets.close.headMax: out of range");

    // A key or a preset left out keeps what was there.
    CameraPresets p = before;
    REQUIRE(importCameraPresets(R"({"version": 1, "close": {"arm": 3.5}})", p, nullptr));
    CHECK(p.close.rig.arm == 3.5f);
    CHECK(p.close.rig.fov == before.close.rig.fov);
    CHECK(samePreset(p.classic, before.classic));
}

TEST_CASE("camera preset: a lead body cut at 0 frames as before; the cut moves only the low points") {
    const CameraPresets d{};
    const ArenaCameraPose pose = arenaPoseAtPitch(kArenaLook, rigOf(d.classic.rig), d.classic.rig.lockPitch);
    ArenaLeadBody zero;
    zero.cut = 0.f;
    const ArenaLeadShares a = arenaLeadShares(pose, kAspect, ArenaLeadBody{});
    const ArenaLeadShares b = arenaLeadShares(pose, kAspect, zero);
    CHECK(a.feet == b.feet);
    CHECK(a.head == b.head);
    CHECK(a.left == b.left);
    CHECK(a.right == b.right);
    // The cut point sits higher in the frame than the feet; the head stays where it was.
    ArenaLeadBody cut;
    cut.cut = 0.6f;
    const ArenaLeadShares c = arenaLeadShares(pose, kAspect, cut);
    CHECK(c.feet > a.feet);
    CHECK(c.head == a.head);
    // Cut at the floor, the low points are the feet's own.
    CHECK(a.feet == doctest::Approx(arenaFramePoint(pose, kAspect, ArenaPoint3{0.f, 0.f, 0.f}).y).epsilon(1e-3));
}

TEST_CASE("camera preset: the close lens at its lock pitch keeps the lead from the cut to the head in its rule") {
    const CameraPresets d{};
    const CameraPreset& c = d.close;
    const ArenaLeadFrameRules rules = cameraPresetLeadRules(c);
    const ArenaCameraPose rig = rigOf(c.rig);
    const ArenaLeadShares s = arenaLeadShares(arenaPoseAtPitch(kArenaLook, rig, c.rig.lockPitch), kAspect, bodyOf(c));
    REQUIRE(s.ahead);
    CHECK(s.feet >= 0.f);
    CHECK(s.head <= c.headMax);
    CHECK(arenaLeadInFrame(rules, s));
    // An ordinary lock is never moved: the solve gives the lock pitch back.
    const ArenaLeadPitch held = arenaLeadPitch(kArenaLook, rules, rig, kAspect, bodyOf(c), c.rig.lockPitch);
    CHECK(held.kept == 2);
    CHECK(held.pitch == c.rig.lockPitch);
    // The whole body (feet in) is not what the close lens keeps: its rule is held at the cut.
    const ArenaLeadShares whole = arenaLeadShares(arenaPoseAtPitch(kArenaLook, rig, c.rig.lockPitch), kAspect, ArenaLeadBody{});
    CHECK(whole.feet < 0.f);
}

TEST_CASE("camera preset: the low shoulder's clearance reads the whole body, whatever the cut") {
    const ZoneCamera z{};
    const ArenaCameraPose rig = rigOf(z);
    ArenaLeadBody cut;
    cut.cut = 0.6f;
    for (float y = -4.f; y <= 4.f; y += 0.25f) {
        for (const float h : {0.1f, 0.3f, 0.5f, 1.f}) {
            for (const float cur : {1.f, -1.f}) {
                const ArenaPoint3 p{10.f, y, h};
                CAPTURE(y);
                CAPTURE(h);
                CHECK(arenaLowSide(kArenaLook, rig, 20.f, kAspect, cut, p, cur) ==
                      arenaLowSide(kArenaLook, rig, 20.f, kAspect, ArenaLeadBody{}, p, cur));
            }
        }
    }
}

TEST_CASE("camera preset: the framing turn at the centre line is today's turn to the bit") {
    const ArenaFrameRules F = kArenaFrame;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const float side : {-1.2f, -0.2f, 0.f, 0.2f, 1.2f, 2.9f}) {
        for (const float d : {-1.f, 0.f, 0.5f, 1.f, 2.4f, 6.f, 12.f, 40.f}) {
            for (const float back : {0.f, 2.6f, 6.4f}) {
                CAPTURE(side);
                CAPTURE(d);
                CHECK(arenaFrameTurnAt(F, side, d, 0.5f, 70.f, back) == arenaFrameTurn(F, side, d));
                CHECK(arenaFrameTurnAt(F, side, d, 0.5f, 60.f, back) == arenaFrameTurn(F, side, d));
            }
        }
    }
    CHECK(arenaFrameTurnAt(F, nan, 6.f, 0.6f, 60.f, 2.6f) == arenaFrameTurn(F, nan, 6.f));
    CHECK(arenaFrameTurnAt(F, 0.2f, nan, 0.6f, 60.f, 2.6f) == arenaFrameTurn(F, 0.2f, nan));
    CHECK(arenaFrameTurnAt(F, 0.2f, 6.f, nan, 60.f, 2.6f) == arenaFrameTurn(F, 0.2f, 6.f));
    CHECK(arenaFrameTurnAt(F, 0.2f, 6.f, 0.6f, nan, 2.6f) == arenaFrameTurn(F, 0.2f, 6.f));
    CHECK(arenaFrameTurnAt(F, 0.2f, 6.f, 0.6f, 60.f, nan) == arenaFrameTurn(F, 0.2f, 6.f));

    // The yaw step with the scene's defaults turns exactly as before.
    ArenaFrameScene s;
    s.followed = true;
    s.side = 1.2f;
    s.distance = 7.f;
    ArenaFrameYaw now{};
    for (int i = 0; i < 120; ++i) now = arenaFrameYawStep(F, now, s, 1.f / 60.f);
    ArenaFrameYaw ref{};
    for (int i = 0; i < 120; ++i) ref.turn = arenaFrameStep(F, ref.turn, arenaFrameTurn(F, s.side, s.distance), 1.f / 60.f);
    CHECK(now.turn == ref.turn);
}

TEST_CASE("camera preset: off the centre line the framing turn puts the target at the preset's share of the width") {
    const CameraPresets d{};
    const CameraPreset& c = d.close;
    const ArenaFrameRules F = kArenaFrame;
    for (const float dist : {3.f, 6.f, 12.f, 25.f}) {
        const float back = arenaEyeBack(c.rig.arm, c.rig.up, 0.f);
        const float t = arenaFrameTurnAt(F, c.rig.side, dist, c.targetX, c.rig.fov, back);
        CAPTURE(dist);
        CHECK(t < 0.f); // a right shoulder and a target right of centre: the view turns left
        CHECK(t >= -F.maxTurn);
        // At a level view the target lands on the share exactly (up to float).
        const ArenaCameraPose level = arenaPoseAtPitch(kArenaLook, rigOf(c.rig, t), 0.f);
        const ArenaFramePoint at = arenaFramePoint(level, kAspect, ArenaPoint3{dist, 0.f, 1.f});
        REQUIRE(at.ahead);
        CHECK(at.x == doctest::Approx(c.targetX).epsilon(1e-3));
        // At the lock pitch (the boom's back worked out at it), within a hundredth, a 7 m boss's face included.
        const float boom = arenaSplitPitch(kArenaLook, c.rig.lockPitch).boom;
        const float t2 = arenaFrameTurnAt(F, c.rig.side, dist, c.targetX, c.rig.fov, arenaEyeBack(c.rig.arm, c.rig.up, boom));
        const ArenaCameraPose locked = arenaPoseAtPitch(kArenaLook, rigOf(c.rig, t2), c.rig.lockPitch);
        for (const float h : {1.f, 7.f * kArenaLookFaceShare}) {
            const ArenaFramePoint p = arenaFramePoint(locked, kAspect, ArenaPoint3{dist, 0.f, h});
            REQUIRE(p.ahead);
            CHECK(std::fabs(p.x - c.targetX) <= 0.01f);
        }
    }
    // Held to the cap both ways; a cap of 0 turns not at all.
    ArenaFrameRules tight = F;
    tight.maxTurn = 5.f;
    CHECK(arenaFrameTurnAt(tight, 0.2f, 3.f, 0.6f, 60.f, 2.6f) == -5.f);
    CHECK(arenaFrameTurnAt(tight, -0.2f, 3.f, 0.4f, 60.f, 2.6f) == 5.f);
    tight.maxTurn = 0.f;
    CHECK(arenaFrameTurnAt(tight, 0.2f, 3.f, 0.6f, 60.f, 2.6f) == 0.f);
    // On the pivot or nearer than nothing: the cap toward the shoulder, never a NaN.
    CHECK(arenaFrameTurnAt(F, 0.2f, 0.f, 0.6f, 60.f, 2.6f) == -F.maxTurn);
    CHECK(std::isfinite(arenaFrameTurnAt(F, 0.2f, -3.f, 0.6f, 60.f, 2.6f)));
    // Mirrored: a left shoulder with the target left of centre turns the other way by as much.
    CHECK(arenaFrameTurnAt(F, -0.2f, 6.f, 0.4f, 60.f, 2.6f) == doctest::Approx(-arenaFrameTurnAt(F, 0.2f, 6.f, 0.6f, 60.f, 2.6f)));

    // The yaw step reads the scene's share.
    ArenaFrameScene s;
    s.followed = true;
    s.side = c.rig.side;
    s.distance = 6.f;
    s.screenX = c.targetX;
    s.fov = c.rig.fov;
    s.back = 2.6f;
    ArenaFrameYaw now{};
    for (int i = 0; i < 600; ++i) now = arenaFrameYawStep(F, now, s, 1.f / 60.f);
    CHECK(now.turn == doctest::Approx(arenaFrameTurnAt(F, s.side, s.distance, s.screenX, s.fov, s.back)));
}

TEST_CASE("camera preset: the close lens's tall-lock look-up is never held back by the cut, and the head keeps its rule") {
    const CameraPresets d{};
    const CameraPreset& c = d.close;
    const ArenaLeadFrameRules rules = cameraPresetLeadRules(c);
    const ArenaCameraPose rig = rigOf(c.rig);
    // The plan's bosses: a 7 m boss at 12 m (+5.5), at 9 m (+11.5), an 11 m boss at 15 m (+14), all from low 0.
    for (const float want : {0.f, 5.5f, 11.5f, 14.f}) {
        CAPTURE(want);
        const ArenaLeadPitch held = arenaLeadPitch(kArenaLook, rules, rig, kAspect, bodyOf(c), want);
        CHECK(held.kept == 2);
        CHECK(held.pitch == want);
        const ArenaLeadShares s = arenaLeadShares(arenaPoseAtPitch(kArenaLook, rig, want), kAspect, bodyOf(c));
        CHECK(s.feet >= c.cutMin);
        CHECK(s.feet < 0.01f); // the cut stays at the bottom edge: the boom swings, the lead holds its place
        CHECK(s.head <= c.headMax);
        CHECK(s.head > 0.65f);
    }
    // The low lens is why close has none: from the 0.6 m eye the head leaves the frame's top at a level look.
    const ArenaLeadShares low = arenaLeadShares(arenaPoseAtPitch(kArenaLook, rig, 0.f, 1.f), kAspect, bodyOf(c));
    CHECK(low.head > 1.f);
}
