#include "ftcore/ArenaCamera.h"
#include "ftcore/ArenaLockFrame.h"
#include "ftcore/ArenaLook.h"
#include "ftcore/ArenaZone.h"
#include "ftcore/PartnerBrain.h"
#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>
#include <chrono>
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

bool keeps(const ArenaCameraPose& rig, float aspect, float pitch, float low = 0.f, const ArenaLowRules& lr = kArenaLow) {
    return arenaLeadInFrame(kArenaLeadFrame, arenaLeadShares(arenaPoseAtPitch(kArenaLook, rig, pitch, low, lr), aspect, ArenaLeadBody{}));
}

bool anyKept(const ArenaCameraPose& rig, float aspect, float low = 0.f, const ArenaLowRules& lr = kArenaLow) {
    for (float q = kArenaLook.viewMin; q <= kArenaLook.viewMax; q += 0.1f)
        if (keeps(rig, aspect, q, low, lr)) return true;
    return false;
}

// How far the lead's extremes pass the frame's edges at a pitch (shares, summed), as the solve's fallback measures it.
float outside(const ArenaCameraPose& rig, float aspect, float pitch, float low = 0.f) {
    const ArenaLeadShares s = arenaLeadShares(arenaPoseAtPitch(kArenaLook, rig, pitch, low), aspect, ArenaLeadBody{});
    if (!s.ahead) return 1e3f;
    return kArenaFeetGive * std::fmax(0.f, -s.feet) + std::fmax(0.f, s.head - 1.f) + std::fmax(0.f, -s.left) + std::fmax(0.f, s.right - 1.f);
}

// The zone's rig with a wall shortening its arm to `arm`: the spring arm pulls the lens in along its line, so the
// shoulder and the height shrink with it.
ArenaCameraPose shortRig(float arm) {
    const ZoneCamera z{};
    ArenaCameraPose p = rigOf(arm, z.side * arm / z.arm, z.fov);
    p.up = z.up * arm / z.arm;
    return p;
}

bool inView(const ArenaFramePoint& q) { return q.ahead && q.x >= 0.f && q.x <= 1.f && q.y >= 0.f && q.y <= 1.f; }
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
    CHECK(rigsKept >= 150); // measured 158 (the low shoulder at the zone's 1.2 m): the narrow lenses on the shortest arms have none
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
        CHECK(sb.right == doctest::Approx(sa.right).epsilon(1e-4));
    }
}

TEST_CASE("arena lock frame: a lens pulled in close gives the weaker rule, then the pitch that loses the least of the lead") {
    // An arm of 0.5 m: the lead fills the frame, the rule cannot hold.
    const float aspect = 16.f / 9.f;
    const ArenaCameraPose rig = rigOf(0.5f, 0.3f, 70.f);
    for (const float want : {30.f, 0.f, -30.f, 60.f}) {
        const ArenaLeadPitch p = arenaLeadPitch(kArenaLook, kArenaLeadFrame, rig, aspect, ArenaLeadBody{}, want);
        CAPTURE(want);
        CHECK(p.kept < 2);
        if (p.kept != 0) continue;
        // Never the asked pitch as it is: the least outside over the whole range (to a fine brute sweep's 0.01).
        float least = 1e9f;
        for (float q = kArenaLook.viewMin; q <= kArenaLook.viewMax; q += 0.1f) least = std::fmin(least, outside(rig, aspect, q));
        CHECK(outside(rig, aspect, p.pitch) <= least + 0.01f);
        CHECK(outside(rig, aspect, p.pitch) <= outside(rig, aspect, want));
    }
}

TEST_CASE("arena lock frame: the shipped rig keeps the rule as the arm shortens, then keeps the head and chest") {
    // The zone's rig at 16:9 as a wall shortens its arm (the shoulder and the height shrink with it, as the spring arm
    // does). The rule holds down to an arm of 2.4 m; the full low lens is whole down to 5 m and the host's low share
    // yields under that (arenaLowFit). Under 3 m the test asks no more than that the head and the chest stay in frame.
    const ZoneCamera z{};
    const float aspect = 16.f / 9.f;
    for (const float asked : {0.f, 1.f}) {
        const float floorArm = 3.f;
        float least = z.arm;
        // By whole tenths of a metre (a float step could land at 2.9999 and skip the strict 3 m case).
        for (int tenths = static_cast<int>(std::lround(z.arm * 10.f)); tenths >= 20; --tenths) {
            const float arm = static_cast<float>(tenths) / 10.f;
            const ArenaCameraPose rig = shortRig(arm);
            ArenaLowRules lr = kArenaLow; // the low shoulder shrinks with the arm as well (the host passes it scaled)
            lr.side = kArenaLow.side * arm / z.arm;
            // The host's low share: what it asks, no more than the arm allows.
            const float fit = arenaLowFit(kArenaLook, kArenaLeadFrame, rig, aspect, ArenaLeadBody{}, lr);
            const float low = std::fmin(asked, fit);
            if (arm >= 5.f) CHECK(fit == 1.f); // the full low lens down to an arm of 5 m
            CAPTURE(asked);
            CAPTURE(fit);
            CAPTURE(arm);
            if (anyKept(rig, aspect, low, lr)) least = arm;
            for (float want = kArenaLook.viewMin; want <= kArenaLook.viewMax; want += 15.f) {
                CAPTURE(want);
                const ArenaLeadPitch p = arenaLeadPitch(kArenaLook, kArenaLeadFrame, rig, aspect, ArenaLeadBody{}, want, low, lr);
                if (arm >= floorArm) {
                    REQUIRE(p.kept == 2);
                    CHECK(keeps(rig, aspect, p.pitch, low, lr));
                    continue;
                }
                if (p.kept == 2) continue;
                const ArenaCameraPose pose = arenaPoseAtPitch(kArenaLook, rig, p.pitch, low, lr);
                CHECK(inView(arenaFramePoint(pose, aspect, ArenaPoint3{0.f, 0.f, ArenaLeadBody{}.head})));
                CHECK(inView(arenaFramePoint(pose, aspect, ArenaPoint3{0.f, 0.f, 1.2f})));
            }
        }
        MESSAGE("shipped rig, low asked " << asked << ": the rule kept down to arm " << least << " m");
        CHECK(least <= floorArm);
    }
}

TEST_CASE("arena lock frame: when not even the whole body fits, the feet are cut and the head stays") {
    // The shipped rig pulled in closer than 2 m (whole tenths, to 0.7 m): where arenaLeadPitch falls back to its last
    // case (kept 0), the head is in the frame and the feet are what leaves it. (At 0.5 and 0.6 m the lens is so near
    // that the head's top passes the frame too: there the least-outside pitch still gives the feet first, by
    // kArenaFeetGive, but cannot keep the whole head.)
    const ZoneCamera z{};
    const float aspect = 16.f / 9.f;
    int cut = 0;
    CHECK(kArenaHeadKeptArm == doctest::Approx(0.7f));
    for (int tenths = 19; tenths >= 7; --tenths) {
        const float arm = static_cast<float>(tenths) / 10.f;
        const ArenaCameraPose rig = shortRig(arm);
        CAPTURE(arm);
        for (float want = kArenaLook.viewMin; want <= kArenaLook.viewMax; want += 15.f) {
            CAPTURE(want);
            const ArenaLeadPitch p = arenaLeadPitch(kArenaLook, kArenaLeadFrame, rig, aspect, ArenaLeadBody{}, want);
            if (p.kept != 0) continue;
            ++cut;
            const ArenaCameraPose pose = arenaPoseAtPitch(kArenaLook, rig, p.pitch);
            CHECK(inView(arenaFramePoint(pose, aspect, ArenaPoint3{0.f, 0.f, ArenaLeadBody{}.head})));
            CHECK_FALSE(inView(arenaFramePoint(pose, aspect, ArenaPoint3{0.f, 0.f, 0.f})));
        }
    }
    MESSAGE("close rigs where the body cannot fit: " << cut);
    CHECK(cut > 0);
    // Under kArenaHeadKeptArm (0.6 and 0.5 m) the head's top leaves the frame too, at some view the fallback takes.
    for (const int tenths : {6, 5}) {
        const float arm = static_cast<float>(tenths) / 10.f;
        const ArenaCameraPose rig = shortRig(arm);
        CAPTURE(arm);
        bool headOut = false;
        for (float want = kArenaLook.viewMin; want <= kArenaLook.viewMax; want += 15.f) {
            const ArenaLeadPitch p = arenaLeadPitch(kArenaLook, kArenaLeadFrame, rig, aspect, ArenaLeadBody{}, want);
            if (p.kept != 0) continue;
            const ArenaCameraPose pose = arenaPoseAtPitch(kArenaLook, rig, p.pitch);
            headOut = headOut || !inView(arenaFramePoint(pose, aspect, ArenaPoint3{0.f, 0.f, ArenaLeadBody{}.head}));
        }
        CHECK(headOut);
        CHECK(arm < kArenaHeadKeptArm);
    }
}

TEST_CASE("arena lock frame: an ordinary lock is never moved (target up to 2.5 m, 3 to 12 m away, lead in frame)") {
    // The hold runs on every followed target: on an ordinary one, with the lead already in frame, the solved pose is the
    // unsolved one (pitch within 0.5 degrees, the lens within 2 cm).
    const ZoneCamera z{};
    const AimStance aim{};
    int tried = 0;
    for (const float aspect : {16.f / 9.f, 21.f / 9.f, 4.f / 3.f})
        for (float dist = 3.f; dist <= 12.01f; dist += 0.5f)
            for (float tall = 0.5f; tall <= 2.51f; tall += 0.5f)
                for (const float turn : {0.f, 20.f, -35.f}) {
                    const float want = arenaFrameLockPitch(kArenaLook, z.pivot, z.arm, z.up, z.lockPitch, dist, turn, tall);
                    const ArenaCameraPose drawn = arenaCameraPose(z, aim, ArenaFrameYaw{40.f, turn}, want);
                    ArenaCameraPose rig = drawn;
                    rig.boomPitch = rig.tilt = 0.f;
                    if (!keeps(rig, aspect, want)) continue; // the lead not in frame as asked: not this test's case
                    ++tried;
                    const ArenaLeadPitch held = arenaLeadPitch(kArenaLook, kArenaLeadFrame, rig, aspect, ArenaLeadBody{}, want);
                    const ArenaCameraPose solved = arenaPoseAtPitch(kArenaLook, rig, held.pitch);
                    CAPTURE(aspect);
                    CAPTURE(dist);
                    CAPTURE(tall);
                    CAPTURE(turn);
                    CHECK(held.kept == 2);
                    CHECK(held.evals == 1);
                    CHECK(std::fabs((solved.boomPitch + solved.tilt) - (drawn.boomPitch + drawn.tilt)) <= 0.5f);
                    const ArenaLensAt a = arenaCameraLens(solved), b = arenaCameraLens(drawn);
                    const float d = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
                    CHECK(d <= 0.02f);
                    CHECK(solved.side == drawn.side);
                    CHECK(solved.yaw == drawn.yaw);
                }
    MESSAGE("ordinary locks with the lead in frame: " << tried);
    CHECK(tried > 300);
}

TEST_CASE("arena lock frame: the low shoulder goes to the side that keeps the aim point clear, and holds on a near tie") {
    const ZoneCamera z{};
    const float aspect = 16.f / 9.f;
    const ArenaCameraPose rig = rigOf(z.arm, z.side, z.fov);
    const float pitch = 20.f;
    const auto clearFrom = [&](float side, ArenaPoint3 p) {
        ArenaLowRules lr = kArenaLow;
        lr.side = side;
        const ArenaCameraPose pose = arenaPoseAtPitch(kArenaLook, rig, pitch, 1.f, lr);
        const ArenaFramePoint a = arenaFramePoint(pose, aspect, p);
        const ArenaLeadShares s = arenaLeadShares(pose, aspect, ArenaLeadBody{});
        if (!a.ahead || a.y > s.head || a.y < s.feet) return 1.f;
        return std::fmax(0.f, std::fmax(s.left - a.x, a.x - s.right));
    };
    // A point 1 m up and 10 m ahead that the lead hides from the right shoulder and the left one shows.
    ArenaPoint3 hidden{};
    bool found = false;
    for (float y = -8.f; y <= 8.f && !found; y += 0.05f) {
        const ArenaPoint3 p{10.f, y, 1.f};
        if (clearFrom(kArenaLow.side, p) <= 0.f && clearFrom(-kArenaLow.side, p) >= 0.1f) {
            hidden = p;
            found = true;
        }
    }
    REQUIRE(found);
    CHECK(arenaLowSide(kArenaLook, rig, pitch, aspect, ArenaLeadBody{}, hidden, 1.f) == -1.f);
    CHECK(arenaLowSide(kArenaLook, rig, pitch, aspect, ArenaLeadBody{}, ArenaPoint3{hidden.x, -hidden.y, hidden.z}, -1.f) == 1.f);
    // Clear from where it is (high over the lead's head): no flip.
    CHECK(arenaLowSide(kArenaLook, rig, pitch, aspect, ArenaLeadBody{}, ArenaPoint3{10.f, 0.f, 9.f}, -1.f) == -1.f);
    CHECK(arenaLowSide(kArenaLook, rig, pitch, aspect, ArenaLeadBody{}, ArenaPoint3{10.f, 0.f, 9.f}, 1.f) == 1.f);
    // Hidden from both sides alike (at the lead itself): no flip either way.
    for (const float cur : {1.f, -1.f}) {
        CAPTURE(cur);
        CHECK(arenaLowSide(kArenaLook, rig, pitch, aspect, ArenaLeadBody{}, ArenaPoint3{0.01f, 0.f, 0.9f}, cur) == cur);
    }
}

TEST_CASE("arena lock frame: a frame's whole solve on a tall boss costs under 0.1 ms") {
    // The host's per-frame solve on a 12 m boss, as SteerCamera calls it once a frame: the low side, the aim pitch, the
    // hold (the lead's rule) and the aim's frame point. The aim far above what the rule allows, the low lens eased in;
    // and a wall-shortened arm where nothing keeps the rule (the fallback searches the whole range).
    const ZoneCamera z{};
    const float aspect = 16.f / 9.f;
    struct Case {
        float arm;
        float low;
    };
    for (const Case c : {Case{z.arm, 0.5f}, Case{z.arm, 1.f}, Case{2.5f, 1.f}}) {
        const ArenaCameraPose rig = rigOf(c.arm, z.side, z.fov, 25.f);
        ArenaLowRules lr = kArenaLow;
        const int n = 400;
        float sink = 0.f;
        int evals = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) {
            const float y = rig.yaw * 3.14159265f / 180.f;
            const ArenaPoint3 aim{8.f * std::cos(y), 8.f * std::sin(y), 8.4f + 0.001f * static_cast<float>(i)};
            const float side = arenaLowSide(kArenaLook, rig, 20.f, aspect, ArenaLeadBody{}, aim, 1.f, kArenaLowSide, lr);
            lr.side = side * kArenaLow.side;
            const float want = arenaLockAimPitch(kArenaLook, rig, aspect, aim, z.lockPitch, c.low, lr);
            const ArenaLeadPitch held = arenaLeadPitch(kArenaLook, kArenaLeadFrame, rig, aspect, ArenaLeadBody{}, want, c.low, lr);
            const ArenaFramePoint at = arenaFramePoint(arenaPoseAtPitch(kArenaLook, rig, held.pitch, c.low, lr), aspect, aim);
            sink += held.pitch + at.x;
            evals = held.evals;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
        CAPTURE(c.arm);
        CAPTURE(c.low);
        MESSAGE("solve per frame, arm " << c.arm << " low " << c.low << ": " << ms * 1000.0 << " us, " << evals << " lead tests");
        CHECK(std::isfinite(sink));
        CHECK(ms < 0.1);
    }
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

TEST_CASE("arena lock frame: the pose's partner camera is whole and the brain's frame agrees with the frustum") {
    const ArenaCameraPose pose = arenaPoseAtPitch(kArenaLook, rigOf(6.4f, 1.2f, 70.f, 30.f), 5.f);
    const float aspect = 16.f / 9.f, height = 1.75f;
    const Vec2 at{3.f, -2.f};
    const PartnerCameraView cam = arenaPoseCamera(pose, at, aspect, height);
    REQUIRE(partnerCameraWhole(cam));
    CHECK_FALSE(arenaPoseCamera(pose, at, 0.f, height).given);
    const ArenaFrustum f = arenaPoseFrustum(pose, at, aspect);
    // Ground points around the lead: partnerInFrame (feet, middle, head at kPartnerFrustumMargin) against the frustum
    // test of the same three points, a point within 1% of the margin's edge left out.
    const auto all = [&](Vec2 g, float m) {
        bool in = true;
        for (const float z : {0.f, 0.5f * height, height}) in = in && arenaInFrustum(f, ArenaPoint3{g.x, g.y, z}, m);
        return in;
    };
    int tried = 0, agreed = 0, inside = 0;
    for (int i = -20; i <= 20; ++i)
        for (int j = -20; j <= 20; ++j) {
            const Vec2 g{at.x + 0.5f * static_cast<float>(i), at.y + 0.5f * static_cast<float>(j)};
            const bool surelyIn = all(g, kPartnerFrustumMargin + 0.01f);
            const bool surelyOut = !all(g, kPartnerFrustumMargin - 0.01f);
            if (!surelyIn && !surelyOut) continue;
            ++tried;
            inside += surelyIn ? 1 : 0;
            agreed += partnerInFrame(cam, kPartnerFrustumMargin, g) == surelyIn ? 1 : 0;
        }
    CHECK(tried > 1000);
    CHECK(inside > 50);
    CHECK(agreed == tried);
}

TEST_CASE("arena lock frame: a point hidden behind a body from the lens") {
    // A body 0.5 m round, 2 m tall, at (5, 0); the eye at (0, 0, 1.6).
    const ArenaPoint3 eye{0.f, 0.f, 1.6f};
    const Vec2 at{5.f, 0.f};
    const auto hidden = [&](ArenaPoint3 p) { return arenaHiddenBehind(eye, p, at, 0.5f, 0.f, 2.f); };
    CHECK(hidden({8.f, 0.f, 1.f}));       // straight behind
    CHECK(hidden({8.f, 0.3f, 1.7f}));     // behind, a little off the middle
    CHECK_FALSE(hidden({8.f, 1.5f, 1.f})); // behind but well to the side: seen past it
    CHECK_FALSE(hidden({3.f, 0.f, 1.f})); // in front of it
    CHECK_FALSE(hidden({8.f, 0.f, 4.5f})); // high over its top
    CHECK(hidden({5.2f, 0.f, 1.f}));      // inside it
    CHECK_FALSE(hidden({4.6f, 1.f, 1.f})); // beside it, nearer than its middle
    // Over the top from a high lens: the line clears the head.
    CHECK_FALSE(arenaHiddenBehind({0.f, 0.f, 6.f}, {8.f, 0.f, 1.f}, at, 0.5f, 0.f, 2.f));
    // From a low lens the same point is hidden.
    CHECK(arenaHiddenBehind({0.f, 0.f, 0.5f}, {8.f, 0.f, 1.f}, at, 0.5f, 0.f, 2.f));
    // A hovering body: a line under it passes.
    CHECK_FALSE(arenaHiddenBehind({0.f, 0.f, 0.3f}, {8.f, 0.f, 0.3f}, at, 0.5f, 1.f, 3.f));
    // The eye inside the body hides nothing; straight down beside it neither; bad cylinders neither.
    CHECK_FALSE(arenaHiddenBehind({5.f, 0.f, 1.f}, {8.f, 0.f, 1.f}, at, 0.5f, 0.f, 2.f));
    CHECK_FALSE(arenaHiddenBehind({7.f, 0.f, 5.f}, {7.f, 0.f, 0.f}, at, 0.5f, 0.f, 2.f));
    CHECK(arenaHiddenBehind({5.1f, 0.f, 5.f}, {5.1f, 0.f, -1.f}, at, 0.5f, 0.f, 2.f));
    CHECK_FALSE(arenaHiddenBehind(eye, {8.f, 0.f, 1.f}, at, 0.f, 0.f, 2.f));
    CHECK_FALSE(arenaHiddenBehind(eye, {8.f, 0.f, 1.f}, at, 0.5f, 2.f, 2.f));
    CHECK_FALSE(arenaHiddenBehind(eye, {8.f, 0.f, std::nanf("")}, at, 0.5f, 0.f, 2.f));
}
