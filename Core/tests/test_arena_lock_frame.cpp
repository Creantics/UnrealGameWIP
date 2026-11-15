#include "ftcore/ArenaCamera.h"
#include "ftcore/ArenaLockFrame.h"
#include "ftcore/ArenaLook.h"
#include "ftcore/ArenaZone.h"
#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>
#include <cmath>

using namespace ftcore;

namespace {
// The zone's rig as a pose at pitch 0 (the arm as given), yaw `yaw`.
ArenaCameraPose rigOf(float arm, float side, float fov, float yaw = 0.f) {
    const ZoneCamera z{};
    ArenaCameraPose p;
    p.yaw = yaw;
    p.arm = arm;
    p.pivot = z.pivot;
    p.side = side;
    p.up = z.up;
    p.fov = fov;
    return p;
}

bool keeps(const ArenaCameraPose& rig, float aspect, float pitch, float low = 0.f) {
    return arenaLeadInFrame(kArenaLeadFrame, arenaLeadShares(arenaPoseAtPitch(kArenaLook, rig, pitch, low), aspect, ArenaLeadBody{}));
}
} // namespace

TEST_CASE("arena lock frame: a point straight down the view's axis is the frame's centre; behind the lens is not ahead") {
    const ArenaCameraPose rig = arenaPoseAtPitch(kArenaLook, rigOf(6.4f, 1.2f, 70.f, 30.f), 20.f);
    const float view = (rig.boomPitch + rig.tilt) * 3.14159265f / 180.f;
    const ArenaLensAt lens = arenaCameraLens(rig);
    const float y = rig.yaw * 3.14159265f / 180.f;
    const ArenaPoint3 ahead{lens.x + 10.f * std::cos(view) * std::cos(y), lens.y + 10.f * std::cos(view) * std::sin(y),
                            rig.pivot + lens.z + 10.f * std::sin(view)};
    const ArenaFramePoint c = arenaFramePoint(rig, 16.f / 9.f, ahead);
    CHECK(c.ahead);
    CHECK(c.x == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(c.y == doctest::Approx(0.5f).epsilon(1e-4));
    const ArenaPoint3 behind{lens.x - 10.f * std::cos(y), lens.y - 10.f * std::sin(y), rig.pivot + lens.z};
    CHECK_FALSE(arenaFramePoint(rig, 16.f / 9.f, behind).ahead);
}

TEST_CASE("arena lock frame: the old face look-up on a tall boss loses the lead; the held pitch keeps it") {
    // The default rig at 16:9 locked on a 12 m boss 8 m ahead: the look through 70% of its height (the old wanted pitch)
    // puts the lead's feet under the frame. The held pitch keeps the rule, and the face is then above the frame.
    const ZoneCamera z{};
    const ArenaCameraPose rig = rigOf(z.arm, z.side, z.fov);
    const float aspect = 16.f / 9.f;
    const float old = arenaFrameLockPitch(kArenaLook, z.pivot, z.arm, z.up, z.lockPitch, 8.f, 0.f, 12.f);
    CHECK_FALSE(keeps(rig, aspect, old));
    const ArenaLockFrame f = arenaLockFrame(kArenaLook, kArenaLeadFrame, rig, aspect, ArenaLeadBody{}, 8.f, 12.f * 0.7f, z.lockPitch);
    CHECK(f.kept == 2);
    CHECK(f.pitch < f.want);
    const ArenaLeadPitch lo = arenaLeadPitch(kArenaLook, kArenaLeadFrame, rig, aspect, ArenaLeadBody{}, kArenaLook.viewMin);
    MESSAGE("default rig 16:9: old look-up " << old << ", held " << f.pitch << ", lowest kept " << lo.pitch);
    CHECK(keeps(rig, aspect, f.pitch));
    CHECK(f.aimAbove);
    // The held pitch is the rule's edge: a little more up loses it.
    CHECK_FALSE(keeps(rig, aspect, f.pitch + 0.5f));
    // The chest of the same boss sits inside what the rule allows: no marker.
    const ArenaLockFrame chest = arenaLockFrame(kArenaLook, kArenaLeadFrame, rig, aspect, ArenaLeadBody{}, 8.f, 1.f, z.lockPitch);
    CHECK_FALSE(chest.aimAbove);
}

TEST_CASE("arena lock frame: every pitch asked, on every rig, comes back keeping the rule (sweep)") {
    const ZoneCamera z{};
    const float aspects[] = {16.f / 9.f, 21.f / 9.f, 4.f / 3.f, 9.f / 16.f};
    const float arms[] = {z.arm, z.arm * 0.6f, 4.0f, 3.0f};
    const float sides[] = {z.side, 0.6f};
    const float fovs[] = {z.fov, 55.f, 90.f};
    int held = 0;
    int rigsKept = 0;
    for (const float aspect : aspects)
        for (const float arm : arms)
            for (const float side : sides)
                for (const float fov : fovs)
                for (const float low : {0.f, 0.5f, 1.f})
                {
                    const ArenaCameraPose rig = rigOf(arm, side, fov, 17.f);
                    // Whether any pitch keeps the rule for this rig (a fine brute sweep): a frame too narrow for the
                    // lead at this arm has none.
                    bool any = false;
                    for (float q = kArenaLook.viewMin; q <= kArenaLook.viewMax && !any; q += 0.1f) any = keeps(rig, aspect, q, low);
                    if (any) ++rigsKept;
                    for (float want = kArenaLook.viewMin; want <= kArenaLook.viewMax; want += 5.f) {
                        const ArenaLeadPitch p = arenaLeadPitch(kArenaLook, kArenaLeadFrame, rig, aspect, ArenaLeadBody{}, want, low);
                        CAPTURE(low);
                        CAPTURE(aspect);
                        CAPTURE(arm);
                        CAPTURE(side);
                        CAPTURE(fov);
                        CAPTURE(want);
                        if (!any) {
                            CHECK(p.kept < 2);
                            continue;
                        }
                        REQUIRE(p.kept == 2);
                        CHECK(keeps(rig, aspect, p.pitch, low));
                        if (keeps(rig, aspect, want, low)) {
                            CHECK(p.pitch == want);
                            continue;
                        }
                        ++held;
                        // Nearest: no kept pitch just past the one given, toward the asked one.
                        const float dir = want > p.pitch ? 1.f : -1.f;
                        const float nudge = std::fmin(0.2f, std::fabs(want - p.pitch));
                        CHECK_FALSE(keeps(rig, aspect, p.pitch + dir * nudge, low));
                    }
                }
    CHECK(held > 0);
    MESSAGE("rigs with a kept pitch: " << rigsKept << " of 288");
    CHECK(rigsKept >= 160); // measured 170: the narrow lenses on the shortest arms have none
    for (const float aspect : aspects)
        for (const float low : {0.f, 1.f}) {
            bool any = false;
            for (float q = kArenaLook.viewMin; q <= kArenaLook.viewMax && !any; q += 0.1f) any = keeps(rigOf(z.arm, z.side, z.fov), aspect, q, low);
            CAPTURE(aspect);
            CAPTURE(low);
            CHECK(any); // the default rig keeps it at every aspect, low or not
        }
}

TEST_CASE("arena lock frame: the yaw never moves the lead in the frame (the turn orbits the pivot)") {
    const ArenaCameraPose a = arenaPoseAtPitch(kArenaLook, rigOf(6.4f, 1.2f, 70.f, 0.f), 10.f);
    for (float yaw = -170.f; yaw <= 180.f; yaw += 37.f) {
        ArenaCameraPose b = a;
        b.yaw = yaw;
        const ArenaLeadShares sa = arenaLeadShares(a, 16.f / 9.f, ArenaLeadBody{});
        const ArenaLeadShares sb = arenaLeadShares(b, 16.f / 9.f, ArenaLeadBody{});
        CHECK(sb.feet == doctest::Approx(sa.feet).epsilon(1e-4));
        CHECK(sb.head == doctest::Approx(sa.head).epsilon(1e-4));
        CHECK(sb.left == doctest::Approx(sa.left).epsilon(1e-4));
    }
}

TEST_CASE("arena lock frame: a lens pulled in close gives the weaker rule, then the asked pitch") {
    // An arm of 0.5 m: the lead fills the frame, the rule cannot hold.
    const ArenaCameraPose rig = rigOf(0.5f, 0.3f, 70.f);
    const ArenaLeadPitch p = arenaLeadPitch(kArenaLook, kArenaLeadFrame, rig, 16.f / 9.f, ArenaLeadBody{}, 30.f);
    CHECK(p.kept < 2);
    if (p.kept == 0) CHECK(p.pitch == 30.f);
}

TEST_CASE("arena lock frame: the aim height is the telegraph's, else the weak point's, else the chest") {
    CHECK(arenaLockAimHeight(0.f, 10.f, 7.f, 3.f) == 7.f);
    CHECK(arenaLockAimHeight(0.f, 10.f, -1.f, 3.f) == 3.f);
    CHECK(arenaLockAimHeight(2.f, 12.f, -1.f, -1.f) == doctest::Approx(2.f + kArenaChestShare * 10.f));
    CHECK(arenaLockAimHeight(2.f, 1.f, -1.f, -1.f) == 2.f); // a top under the base: the base
}

TEST_CASE("arena lock frame: the partner's soft turn is at most 5 degrees and keeps the lead's rule") {
    const float aspect = 16.f / 9.f;
    const ArenaCameraPose rig = rigOf(6.4f, 1.2f, 70.f, 0.f);
    const ArenaLeadPitch held = arenaLeadPitch(kArenaLook, kArenaLeadFrame, rig, aspect, ArenaLeadBody{}, 5.f);
    REQUIRE(held.kept == 2);
    const ArenaCameraPose pose = arenaPoseAtPitch(kArenaLook, rig, held.pitch);
    // In the frame already: no turn.
    CHECK(arenaPartnerYaw(kArenaPartnerYaw, kArenaLeadFrame, pose, aspect, ArenaLeadBody{}, ArenaPoint3{3.f, 1.f, 1.f}) == 0.f);
    // Just past the left edge: a small turn left brings it in.
    float lo = -1.f;
    for (float y = 0.f; y < 30.f; y += 0.1f) {
        const ArenaFramePoint f = arenaFramePoint(pose, aspect, ArenaPoint3{6.f, -y, 1.f});
        if (f.x < kArenaPartnerYaw.margin) {
            lo = y;
            break;
        }
    }
    REQUIRE(lo > 0.f);
    const float near = arenaPartnerYaw(kArenaPartnerYaw, kArenaLeadFrame, pose, aspect, ArenaLeadBody{}, ArenaPoint3{6.f, -(lo + 0.3f), 1.f});
    CHECK(near < 0.f);
    CHECK(near >= -kArenaPartnerYaw.maxYaw);
    // Far off to the side: more than 5 degrees would be needed, so none.
    CHECK(arenaPartnerYaw(kArenaPartnerYaw, kArenaLeadFrame, pose, aspect, ArenaLeadBody{}, ArenaPoint3{0.f, -8.f, 1.f}) == 0.f);
}

TEST_CASE("arena lock frame: the low lens shows more of a tall enemy without pulling out (arm and field of view kept)") {
    const ZoneCamera z{};
    const ArenaCameraPose rig = rigOf(z.arm, z.side, z.fov);
    const float aspect = 16.f / 9.f;
    const ArenaLeadPitch high = arenaLeadPitch(kArenaLook, kArenaLeadFrame, rig, aspect, ArenaLeadBody{}, kArenaLook.viewMax);
    const ArenaLeadPitch low = arenaLeadPitch(kArenaLook, kArenaLeadFrame, rig, aspect, ArenaLeadBody{}, kArenaLook.viewMax, 1.f);
    REQUIRE(high.kept == 2);
    REQUIRE(low.kept == 2);
    const ArenaCameraPose p = arenaPoseAtPitch(kArenaLook, rig, low.pitch, 1.f);
    const ArenaLensAt lens = arenaCameraLens(p);
    MESSAGE("highest kept pitch: zone rig " << high.pitch << ", low " << low.pitch << "; low eye " << p.pivot + lens.z);
    CHECK(p.pivot + lens.z == doctest::Approx(kArenaLow.eye).epsilon(1e-3));
    CHECK(p.arm == rig.arm);
    CHECK(p.fov == rig.fov);
    CHECK(p.side == doctest::Approx(kArenaLow.side));
    CHECK(p.boomPitch + p.tilt == doctest::Approx(low.pitch).epsilon(1e-4));
    CHECK(low.pitch > high.pitch + 8.f); // the frame's top reaches that much higher up the enemy
    // The lead sits in the lower part of the frame there: the head under the rule's line, the feet over the edge.
    const ArenaLeadShares s = arenaLeadShares(p, aspect, ArenaLeadBody{});
    CHECK(s.feet >= kArenaLeadFrame.feetMin);
    CHECK(s.head <= kArenaLeadFrame.headMax);
    // Easing in: every blend in between keeps it at the pitch it is held to.
    for (float w = 0.f; w <= 1.f; w += 0.1f) {
        const ArenaLeadPitch q = arenaLeadPitch(kArenaLook, kArenaLeadFrame, rig, aspect, ArenaLeadBody{}, kArenaLook.viewMax, w);
        CAPTURE(w);
        CHECK(keeps(rig, aspect, q.pitch, w));
    }
}

TEST_CASE("arena lock frame: the boom that puts the lens at a height") {
    const ZoneCamera z{};
    const float b = arenaLowBoom(z.pivot, z.arm, z.up, 0.6f);
    CHECK(arenaEyeHeight(z.pivot, z.arm, z.up, b) == doctest::Approx(0.6f).epsilon(1e-3));
    CHECK(arenaLowBoom(z.pivot, z.arm, z.up, 5.f) == 0.f); // already lower at a level boom
}

TEST_CASE("arena lock frame: a pose's frustum sees what its frame shows, and keeps a margin clear") {
    const ArenaCameraPose rig = rigOf(6.4f, 1.2f, 70.f, 30.f);
    const ArenaCameraPose pose = arenaPoseAtPitch(kArenaLook, rig, 5.f);
    const float aspect = 16.f / 9.f;
    const Vec2 at{3.f, -2.f};
    const ArenaFrustum f = arenaPoseFrustum(pose, at, aspect);
    REQUIRE(f.on);
    CHECK(f.vfov < pose.fov);
    // Points over a grid around the lead: the frustum agrees with the frame's shares (0..1) away from the edges.
    int agreed = 0, tried = 0;
    for (int i = -6; i <= 6; ++i)
        for (int j = 0; j <= 6; ++j)
            for (int k = 0; k <= 4; ++k) {
                const ArenaPoint3 rel{static_cast<float>(i), static_cast<float>(j) - 3.f, static_cast<float>(k)};
                const ArenaFramePoint q = arenaFramePoint(pose, aspect, rel);
                const bool inside = q.ahead && q.x > 0.f && q.x < 1.f && q.y > 0.f && q.y < 1.f;
                const bool nearEdge = q.ahead && (std::fabs(q.x) < 0.01f || std::fabs(q.x - 1.f) < 0.01f ||
                                                  std::fabs(q.y) < 0.01f || std::fabs(q.y - 1.f) < 0.01f);
                if (nearEdge) continue;
                ++tried;
                agreed += arenaInFrustum(f, ArenaPoint3{at.x + rel.x, at.y + rel.y, rel.z}) == inside ? 1 : 0;
            }
    CHECK(tried > 300);
    CHECK(agreed == tried);
    // The margin: a point at 5% of the width from the left edge is in the frustum, not within a 10% margin.
    {
        // search along the view's right for the point at x = 0.05 on the lead's floor depth
        const ArenaPoint3 lead{0.f, 0.f, 1.f};
        const ArenaFramePoint c = arenaFramePoint(pose, aspect, lead);
        REQUIRE(c.ahead);
        const float y = pose.yaw * 3.14159265f / 180.f;
        const ArenaPoint3 right{-std::sin(y), std::cos(y), 0.f};
        float lo = -20.f, hi = 0.f;
        for (int n = 0; n < 40; ++n) {
            const float m = 0.5f * (lo + hi);
            const ArenaFramePoint q = arenaFramePoint(pose, aspect, ArenaPoint3{lead.x + m * right.x, lead.y + m * right.y, lead.z});
            if (q.x < 0.05f) lo = m;
            else hi = m;
        }
        const float m = 0.5f * (lo + hi);
        const ArenaPoint3 p{at.x + lead.x + m * right.x, at.y + lead.y + m * right.y, lead.z};
        CHECK(arenaInFrustum(f, p));
        CHECK_FALSE(arenaInFrustum(f, p, 0.10f));
    }
    // Behind the lens, an off frustum and a bad aspect: nothing is in.
    CHECK_FALSE(arenaInFrustum(f, ArenaPoint3{f.pos.x - 10.f * f.forward.x, f.pos.y - 10.f * f.forward.y, f.pos.z}));
    CHECK_FALSE(arenaInFrustum(ArenaFrustum{}, ArenaPoint3{at.x, at.y, 1.f}));
    CHECK_FALSE(arenaPoseFrustum(pose, at, 0.f).on);
}
