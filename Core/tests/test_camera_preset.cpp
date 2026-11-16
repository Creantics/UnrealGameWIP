#include "ftcore/AimStance.h"
#include "ftcore/ArenaCamera.h"
#include "ftcore/ArenaLockFrame.h"
#include "ftcore/ArenaLook.h"
#include "ftcore/ArenaZone.h"
#include "ftcore/CameraPreset.h"
#include "ftcore/FieldStrike.h" // FieldBody: the aim stance's tick
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

ArenaLeadBody bodyOf(const CameraPreset& p, float head = ArenaLeadBody{}.head) {
    ArenaLeadBody b;
    b.head = head;
    b.cut = cameraPresetCut(p, head);
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
    CHECK(c.rig.side == 0.55f);
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

TEST_CASE("camera preset: classic gives the zone's own fit back untouched; close keeps the zone's framing cap") {
    const CameraPresets d{};
    const ZoneCamera fit{};
    CHECK(sameRig(applyCameraPreset(fit, d, CameraPresetId::Classic), fit));

    // A zone's own fit that is not the default (a longer arm, a nearer shoulder, a capped turn) is left alone by
    // classic, field for field: classic is the zone's rig, not ZoneCamera{}.
    ZoneCamera own{};
    own.arm = 8.f;
    own.pivot = 1.7f;
    own.side = 1.f;
    own.up = 0.4f;
    own.fov = 75.f;
    own.lockPitch = -14.f;
    own.frameTurnCapDeg = 12.5f;
    CHECK(sameRig(applyCameraPreset(own, d, CameraPresetId::Classic), own));
    CHECK(sameRig(applyCameraPreset(own, d, static_cast<CameraPresetId>(7)), own)); // an id past the last: classic

    const ZoneCamera c = applyCameraPreset(own, d, CameraPresetId::Close);
    CHECK(c.frameTurnCapDeg == 12.5f);
    CHECK(c.arm == d.close.rig.arm);
    CHECK(c.pivot == d.close.rig.pivot);
    CHECK(c.side == d.close.rig.side);
    CHECK(c.lockPitch == d.close.rig.lockPitch);
}

TEST_CASE("camera preset: classic's camera outputs are main's literal values to the bit") {
    // Copied from main (ff739c81) as hexfloats: the zone's default rig through the lead's frame, the pose and the
    // framing turn, before the presets existed. The branch's classic path (applyCameraPreset, cameraPresetLeadRules,
    // cameraPresetCut, cameraPresetLowCap, arenaFrameTurnAt at the centre share) must give them back exactly.
    const CameraPresets d{};
    const CameraPreset& cl = d.classic;
    const ZoneCamera z = applyCameraPreset(ZoneCamera{}, d, CameraPresetId::Classic);
    CHECK(z.arm == 0x1.99999ap+2f);
    CHECK(z.pivot == 0x1.8p+0f);
    CHECK(z.side == 0x1.333334p+0f);
    CHECK(z.up == 0x1.666666p-1f);
    CHECK(z.fov == 0x1.18p+6f);
    CHECK(z.lockPitch == -0x1.4p+4f);
    const ArenaCameraPose rig = rigOf(z);
    const ArenaLeadFrameRules rules = cameraPresetLeadRules(cl);
    const ArenaLeadBody body = bodyOf(cl);
    CHECK(body.cut == 0.f);
    struct Pin {
        float pitch, feet, head, left, right, solved;
    };
    const Pin pins[] = {
        {-20.f, 0x1.cd4828p-4f, 0x1.a9b222p-2f, 0x1.4c0634p-2f, 0x1.a617ccp-2f, -0x1.4p+4f},
        {-10.f, 0x1.5c48d0p-4f, 0x1.ad2778p-2f, 0x1.4d74d0p-2f, 0x1.a2af0cp-2f, -0x1.4p+3f},
        {0.f, 0x1.0497e8p-4f, 0x1.aec114p-2f, 0x1.4ee8e0p-2f, 0x1.9ee2ccp-2f, 0.f},
        {5.f, 0x1.c81c10p-5f, 0x1.aeda98p-2f, 0x1.4b377ep-2f, 0x1.9f47f4p-2f, 0x1.102920p+1f},
        {14.f, 0x1.7f0fd0p-5f, 0x1.ade022p-2f, 0x1.4443d4p-2f, 0x1.9ff990p-2f, 0x1.102800p+1f},
    };
    for (const Pin& p : pins) {
        CAPTURE(p.pitch);
        const ArenaLeadShares s = arenaLeadShares(arenaPoseAtPitch(kArenaLook, rig, p.pitch), kAspect, body);
        CHECK(s.ahead);
        CHECK(s.feet == p.feet);
        CHECK(s.head == p.head);
        CHECK(s.left == p.left);
        CHECK(s.right == p.right);
        const ArenaLeadPitch lp = arenaLeadPitch(kArenaLook, rules, rig, kAspect, body, p.pitch);
        CHECK(lp.pitch == p.solved);
        CHECK(lp.kept == 2);
    }
    CHECK(cameraPresetLowCap(cl, arenaLowFit(kArenaLook, rules, rig, kAspect, body)) == 0x1p+0f);
    AimStance aim;
    const float boom[] = {-0x1.4p+4f, -0x1.4p+3f, 0.f, 0x1.4p+2f, 0x1.cp+3f};
    for (int i = 0; i < 5; ++i) {
        CAPTURE(i);
        const ArenaCameraPose c = arenaCameraPose(z, aim, ArenaFrameYaw{10.f, -5.f}, pins[i].pitch);
        CHECK(c.yaw == 0x1.4p+2f);
        CHECK(c.boomPitch == boom[i]);
        CHECK(c.tilt == 0.f);
        CHECK(c.arm == 0x1.99999ap+2f);
        CHECK(c.pivot == 0x1.8p+0f);
        CHECK(c.side == 0x1.333334p+0f);
        CHECK(c.up == 0x1.666666p-1f);
        CHECK(c.fov == 0x1.18p+6f);
    }
    // The framing turn the host asks for under classic (the centre share) is main's arenaFrameTurn to the bit.
    const float turns[][2] = {{3.f, -0x1.794036p+4f}, {6.f, -0x1.712ec4p+3f}, {12.f, -0x1.6f4e90p+2f}};
    for (const auto& t : turns) {
        CAPTURE(t[0]);
        CHECK(arenaFrameTurnAt(kArenaFrame, z.side, t[0], cl.targetX, z.fov, arenaEyeBack(z.arm, z.up, 0.f)) == t[1]);
    }
}

TEST_CASE("camera preset: the cut is held to half the lead's height; the low cap keeps a NaN fit and classic's fits") {
    const CameraPresets d{};
    CHECK(cameraPresetCut(d.close, 1.8f) == 0.6f);
    CHECK(cameraPresetCut(d.close, 1.4f) == 0.6f);
    CHECK(cameraPresetCut(d.close, 1.f) == 0.5f);
    CHECK(cameraPresetCut(d.close, 0.f) == 0.f);
    CHECK(cameraPresetCut(d.close, -1.f) == 0.f);
    CHECK(cameraPresetCut(d.close, std::numeric_limits<float>::quiet_NaN()) == 0.f);
    for (const float head : {0.5f, 1.4f, 1.8f, 2.1f}) CHECK(cameraPresetCut(d.classic, head) == 0.f);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(std::isnan(cameraPresetLowCap(d.close, nan)));
    CHECK(std::isnan(cameraPresetLowCap(d.classic, nan)));
    CHECK(cameraPresetLowCap(d.close, 0.7f) == 0.f);
    for (const float fit : {0.f, 0.25f, 0.6f, 1.f}) CHECK(cameraPresetLowCap(d.classic, fit) == fit);
}

TEST_CASE("camera preset: close keeps every look-up for leads 1.4 to 1.8 m tall; past 1.85 m the head rule is the limit") {
    const CameraPresets d{};
    const CameraPreset& c = d.close;
    const ArenaLeadFrameRules rules = cameraPresetLeadRules(c);
    const ArenaCameraPose rig = rigOf(c.rig);
    for (const float head : {1.4f, 1.5f, 1.6f, 1.68f, 1.7f, 1.75f, 1.8f}) {
        for (const float want : {c.rig.lockPitch, 0.f, 5.5f, 11.5f, 14.f}) {
            CAPTURE(head);
            CAPTURE(want);
            const ArenaLeadPitch held = arenaLeadPitch(kArenaLook, rules, rig, kAspect, bodyOf(c, head), want);
            CHECK(held.kept == 2);
            CHECK(held.pitch == want);
        }
    }
    // A 2 m lead's head leaves close's rule at its own lock pitch, whatever the cut: the limit, written down (a lead
    // over about 1.85 m under close is an open question for the lens, not for the cut).
    const ArenaLeadShares tall = arenaLeadShares(arenaPoseAtPitch(kArenaLook, rig, c.rig.lockPitch), kAspect, bodyOf(c, 2.f));
    CHECK(tall.head > c.headMax);
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
    refused(R"({"close": {"fov": 130}})", "camera_presets.close.fov: out of range");
    // Classic's rig is the zone's own: a rig key on it is refused, whatever its value.
    refused(R"({"classic": {"fov": 70}})", "camera_presets.classic.fov: classic's rig is the zone's own");
    refused(R"({"classic": {"arm": 6.4}})", "camera_presets.classic.arm: classic's rig is the zone's own");
    refused(R"({"classic": {"lockPitch": -20}})", "camera_presets.classic.lockPitch: classic's rig is the zone's own");
    // The seventh key on is not the rig's: classic takes it.
    CameraPresets lens = before;
    REQUIRE(importCameraPresets(R"({"classic": {"cutAt": 0.0, "targetX": 0.5}})", lens, nullptr));
    CHECK(samePreset(lens.classic, before.classic));
    refused(R"({"close": {"cutAt": 1.2}})", "camera_presets.close.cutAt: out of range");
    refused(R"({"close": {"cutMin": 0.3, "headMax": 0.3}})", "camera_presets.close.cutMin: not under headMax");
    refused(R"({"close": {"targetX": 0.8}})", "camera_presets.close.targetX: out of range");
    refused(R"({"close": {"lagSeconds": -0.1}})", "camera_presets.close.lagSeconds: out of range");
    refused(R"({"close": {"lowMax": 1.5}})", "camera_presets.close.lowMax: out of range");
    refused(R"({"close": {"cutMin": -0.2}})", "camera_presets.close.cutMin: out of range");
    refused(R"({"close": {"cutAt": "high"}})", "camera_presets.close.cutAt: not a number");
    refused(R"({"close": 3})", "camera_presets.close: not an object");
    // A good preset first and a bad one after: the good one is not kept either.
    refused(R"({"classic": {"lowMax": 0.5}, "close": {"headMax": 0.1}})", "camera_presets.close.headMax: out of range");

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

namespace {

// The framing turn the host asks for with a target `dist` m down the lock's line, under each preset at its lock pitch:
// classic at the centre share (arenaFrameTurn), close at its own share from its own eye.
float lockTurn(const CameraPreset& p, float dist) {
    const float boom = arenaSplitPitch(kArenaLook, p.rig.lockPitch).boom;
    return arenaFrameTurnAt(kArenaFrame, p.rig.side, dist, p.targetX, p.rig.fov, arenaEyeBack(p.rig.arm, p.rig.up, boom));
}

// Where on the frame's width (share) a walk from the lead along the floor direction `dir` heads: the walk's far point
// (its vanishing point, taken 2 km out at the lead's middle height) as `pose` sees it.
float walkHeading(const ArenaCameraPose& pose, Vec2 dir) {
    const ArenaFramePoint far = arenaFramePoint(pose, kAspect, ArenaPoint3{2000.f * dir.x, 2000.f * dir.y, 1.f});
    return far.ahead ? far.x : -1.f;
}

// How far (degrees, + clockwise) the screen line of a 0.5 m step along `dir`, at the lead's middle (1 m up), leans
// from straight up the screen: the perspective's own lean, a lead off the frame's centre seeing its walk converge.
float screenLean(const ArenaCameraPose& pose, Vec2 dir) {
    const ArenaFramePoint a = arenaFramePoint(pose, kAspect, ArenaPoint3{0.f, 0.f, 1.f});
    const ArenaFramePoint b = arenaFramePoint(pose, kAspect, ArenaPoint3{0.5f * dir.x, 0.5f * dir.y, 1.f});
    return std::atan2((b.x - a.x) * kAspect, b.y - a.y) * 57.29578f;
}

// A full aim's pose for the zone camera `z` at yaw 0 and view pitch `pitch`.
ArenaCameraPose aimedPose(const ZoneCamera& z, float pitch) {
    AimStance aim;
    for (int i = 0; i < 240; ++i) aim.tick(1.f / 60.f, true, FieldBody::Free);
    return arenaCameraPose(z, aim, ArenaFrameYaw{0.f, 0.f}, pitch);
}

// The crosshair's ray as the host casts it: from the lens, along the drawn yaw and the view pitch (boom + tilt).
void crosshairRay(const ArenaCameraPose& pose, ArenaPoint3& from, ArenaPoint3& to) {
    const ArenaLensAt l = arenaCameraLens(pose);
    from = ArenaPoint3{l.x, l.y, pose.pivot + l.z};
    const float p = (pose.boomPitch + pose.tilt) / 57.29578f;
    const float y = pose.yaw / 57.29578f;
    const float reach = 80.f;
    to = ArenaPoint3{from.x + reach * std::cos(p) * std::cos(y), from.y + reach * std::cos(p) * std::sin(y), from.z + reach * std::sin(p)};
}

// The view pitch that puts the aimed crosshair through the height `h` at `x` m ahead: the player's aim on a point.
float aimPitchAt(const ZoneCamera& z, float x, float h) {
    float lo = -40.f, hi = 40.f;
    for (int i = 0; i < 60; ++i) {
        const float mid = 0.5f * (lo + hi);
        ArenaPoint3 from, to;
        crosshairRay(aimedPose(z, mid), from, to);
        const float s = (x - from.x) / (to.x - from.x);
        (from.z + s * (to.z - from.z) < h ? lo : hi) = mid;
    }
    return 0.5f * (lo + hi);
}

// The body the crosshair's ray enters first (-1: none).
int crosshairPick(const ArenaCameraPose& pose, const ArenaBodyCylinder* bodies, int n) {
    ArenaPoint3 from, to;
    crosshairRay(pose, from, to);
    int best = -1;
    float bestT = 2.f;
    for (int i = 0; i < n; ++i) {
        float t = 0.f;
        if (arenaRayEntersBody(from, to, bodies[i], t) && t < bestT) {
            best = i;
            bestT = t;
        }
    }
    return best;
}

} // namespace

TEST_CASE("camera preset: under close the stick's up walks into the screen toward the frame's centre, read off the drawn yaw") {
    // The stick reads the drawn yaw (the lens's own facing: the lock's line plus the framing turn), as under classic.
    // Pushed up, the lead walks along the lens's facing: on screen the walk heads for the frame's centre at the
    // horizon, under both presets and at every turn. Read off the lock's line instead (the turn left out), it would
    // head off the centre by the turn: toward the shifted aim axis, which is not what up on the stick means.
    const CameraPresets d{};
    for (const float dist : {2.4f, 3.f, 6.f, 8.f, 12.f}) {
        CAPTURE(dist);
        float lean[2] = {};
        for (int k = 0; k < 2; ++k) {
            const CameraPreset& p = k == 0 ? d.classic : d.close;
            const float turn = lockTurn(p, dist);
            const float drawn = arenaFrameViewYaw(ArenaFrameYaw{0.f, turn});
            CHECK(drawn == turn);
            const ArenaCameraPose pose = arenaPoseAtPitch(kArenaLook, rigOf(p.rig, drawn), p.rig.lockPitch);
            CAPTURE(k);
            CHECK(walkHeading(pose, arenaStickToWorld(drawn, 1.f, 0.f)) == doctest::Approx(0.5f).epsilon(1e-3));
            if (std::fabs(turn) > 1.f) {
                // The line's yaw heads off the centre, the turn's way round.
                CHECK(std::fabs(walkHeading(pose, arenaStickToWorld(0.f, 1.f, 0.f)) - 0.5f) > 0.01f);
            }
            // The step's own lean at the lead is the perspective's alone: the same at every distance, whatever the turn
            // (classic about 18.6 degrees from its high lens; close about 45.5 from its low one 0.55 m to the side, the
            // lead a third of the way in from the left: up on the stick walks it on screen at about 45 degrees toward the
            // frame's centre, where the old 0.2 m side leaned 20.3).
            lean[k] = screenLean(pose, arenaStickToWorld(drawn, 1.f, 0.f));
            CAPTURE(lean[k]);
            CHECK(std::fabs(lean[k]) < (k == 0 ? 22.f : 47.f));
        }
        CHECK(std::fabs(lean[0]) == doctest::Approx(18.62f).epsilon(2e-3));
        CHECK(std::fabs(lean[1]) == doctest::Approx(45.47f).epsilon(2e-3));
    }
}

TEST_CASE("camera preset: the aimed crosshair picks the same body under close as under classic") {
    // The aimed arm is the preset's times armScale, the shoulder and fov the aim's own for both: the ray's plan line
    // is the same (0.55 m right of the pivot along the yaw), only its height differs. Three layouts on that line, the
    // player putting the crosshair on the point named; both lenses must enter the same body first.
    const CameraPresets d{};
    const ZoneCamera lens[2] = {applyCameraPreset(ZoneCamera{}, d, CameraPresetId::Classic),
                                applyCameraPreset(ZoneCamera{}, d, CameraPresetId::Close)};
    const float line = AimRules{}.shoulderRight / 100.f;
    struct Layout {
        ArenaBodyCylinder bodies[3];
        int n;
        float aimX, aimH;
        int want;
    };
    const Layout layouts[] = {
        // One body 10 m out, a second 1.5 m to its side: the chest of the first.
        {{{Vec2{10.f, line}, 0.f, 1.8f, 0.4f}, {Vec2{10.f, line + 1.5f}, 0.f, 1.8f, 0.4f}, {}}, 2, 10.f, 1.2f, 0},
        // A body at 7 m in front of the one aimed at 14 m: the nearer takes the crosshair under both.
        {{{Vec2{14.f, line}, 0.f, 1.8f, 0.4f}, {Vec2{7.f, line}, 0.f, 1.8f, 0.4f}, {}}, 2, 14.f, 1.2f, 1},
        // A 7 m boss's face at 12 m over a 1 m body at 5 m and past a man 2 m aside: the boss.
        {{{Vec2{12.f, line}, 0.f, 7.f, 1.5f}, {Vec2{5.f, line}, 0.f, 1.f, 0.4f}, {Vec2{6.f, line + 2.f}, 0.f, 1.8f, 0.4f}}, 3, 12.f, 6.5f, 0},
    };
    for (const Layout& l : layouts) {
        CAPTURE(l.aimX);
        int pick[2];
        for (int k = 0; k < 2; ++k) pick[k] = crosshairPick(aimedPose(lens[k], aimPitchAt(lens[k], l.aimX, l.aimH)), l.bodies, l.n);
        CHECK(pick[0] == l.want);
        CHECK(pick[1] == l.want);
    }
    // The close lens is the lower and nearer of the two (it is what differs).
    ArenaPoint3 fc, tc, fs, ts;
    crosshairRay(aimedPose(lens[0], 0.f), fc, tc);
    crosshairRay(aimedPose(lens[1], 0.f), fs, ts);
    CHECK(fs.z < fc.z);
    CHECK(fs.x > fc.x);
    CHECK(fs.y == doctest::Approx(fc.y).epsilon(1e-4));
}

TEST_CASE("camera preset: under close the partner yaw stays inside its 5 degree range and is never NaN") {
    const CameraPresets d{};
    for (int k = 0; k < 2; ++k) {
        const CameraPreset& p = k == 0 ? d.classic : d.close;
        const ArenaLeadFrameRules rules = cameraPresetLeadRules(p);
        for (const float dist : {2.4f, 6.f, 12.f}) {
            const ArenaCameraPose pose = arenaPoseAtPitch(kArenaLook, rigOf(p.rig, lockTurn(p, dist)), p.rig.lockPitch);
            int turned = 0;
            for (float x = -6.f; x <= 6.f; x += 0.5f) {
                for (float y = -6.f; y <= 6.f; y += 0.5f) {
                    const float yaw = arenaPartnerYaw(kArenaPartnerYaw, rules, pose, kAspect, bodyOf(p), ArenaPoint3{x, y, 1.f});
                    CAPTURE(k);
                    CAPTURE(x);
                    CAPTURE(y);
                    REQUIRE(std::isfinite(yaw));
                    CHECK(std::fabs(yaw) <= kArenaPartnerYaw.maxYaw);
                    turned += yaw != 0.f ? 1 : 0;
                }
            }
            CHECK(turned > 0); // the term does turn somewhere on the grid, under both
        }
    }
}
