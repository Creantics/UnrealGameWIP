#include "ftcore/AimStance.h"
#include "ftcore/ArenaFight.h"
#include "ftcore/ArenaLook.h"
#include "ftcore/ArenaZone.h"
#include "TestAssets.h"
#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>
#include <cmath>
#include <limits>
#include <string>

using namespace ftcore;

namespace {

ArenaTable lookArena() {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/arena.json", text)) FAIL("Data/Battle/arena.json not found");
    ArenaTable t;
    std::string err;
    REQUIRE_MESSAGE(importArenaTable(text, t, &err), err);
    return t;
}

AimRules lookAim() {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/aim.json", text)) FAIL("Data/Battle/aim.json not found");
    AimRules r;
    std::string err;
    REQUIRE_MESSAGE(importAimRules(text, r, &err), err);
    return r;
}

// The tallest standing point of any enemy kind in the table (its height times its look's scale, plus its hover).
float tallestTop(const ArenaTable& t) {
    float top = 0.f;
    for (const ArenaEnemyKind& k : t.enemies) {
        const float scale = k.look.skinned() ? k.look.scale : 1.f;
        const float hover = k.look.skinned() ? k.look.hover : 0.f;
        top = std::fmax(top, arenaEnemyHeight(k) * scale + hover);
    }
    return top;
}

} // namespace

TEST_CASE("arena look: the view pitch reaches near straight up and is held to its limits") {
    const ArenaLookLimits& L = kArenaLook;
    CHECK(L.viewMax >= 80.f);
    CHECK(L.viewMax < 90.f); // never past straight up (the view would flip)
    CHECK(arenaClampViewPitch(L, 85.f) == doctest::Approx(85.f));
    CHECK(arenaClampViewPitch(L, 120.f) == doctest::Approx(L.viewMax));
    CHECK(arenaClampViewPitch(L, -90.f) == doctest::Approx(L.viewMin));
    CHECK(arenaClampViewPitch(L, std::numeric_limits<float>::quiet_NaN()) == 0.f);
}

TEST_CASE("arena look: the boom stops at boomMax and the lens tilts the rest") {
    const ArenaLookLimits& L = kArenaLook;
    for (float v = -60.f; v <= 95.f; v += 2.5f) {
        const ArenaPitchSplit s = arenaSplitPitch(L, v);
        CHECK(s.boom <= L.boomMax + 1e-4f);
        CHECK(s.tilt >= 0.f);
        CHECK(s.boom + s.tilt == doctest::Approx(arenaClampViewPitch(L, v)));
        if (v <= L.boomMax) CHECK(s.tilt == 0.f);
    }
}

TEST_CASE("arena look: at the boom's highest the lens stays over the floor, free and aimed, at the same arm") {
    const ArenaLookLimits& L = kArenaLook;
    const ZoneCamera cam{};
    const AimRules aim = lookAim();
    const float arms[2] = {cam.arm, cam.arm * aim.armScale};
    for (float arm : arms) {
        CAPTURE(arm);
        CHECK(arenaEyeHeight(cam.pivot, arm, cam.up, L.boomMax) >= L.minEye);
        // Looking further up never moves the lens: the boom is held, only the lens tilts.
        const ArenaPitchSplit a = arenaSplitPitch(L, 40.f), b = arenaSplitPitch(L, L.viewMax);
        CHECK(arenaEyeHeight(cam.pivot, arm, cam.up, a.boom) == doctest::Approx(arenaEyeHeight(cam.pivot, arm, cam.up, b.boom)));
        CHECK(arenaEyeBack(arm, cam.up, a.boom) == doctest::Approx(arenaEyeBack(arm, cam.up, b.boom)));
        // And the lens stays behind the lead, never over or inside it.
        CHECK(arenaEyeBack(arm, cam.up, L.boomMax) > 1.f);
    }
}

TEST_CASE("arena look: the tallest boss's top can be put under the crosshair from anywhere it can stand") {
    const ArenaLookLimits& L = kArenaLook;
    const ArenaTable t = lookArena();
    const float top = tallestTop(t);
    REQUIRE(top > 9.f); // the hovering kite is the tallest (9.1 m and a 0.9 m hover)
    const ZoneCamera cam{};
    const AimRules aim = lookAim();
    const float arms[2] = {cam.arm, cam.arm * aim.armScale};
    for (float arm : arms) {
        // From touching its body (the biggest body radius plus the lead's) out to across a big ring.
        for (float away = 2.5f; away <= 40.f; away += 0.5f) {
            CAPTURE(arm);
            CAPTURE(away);
            const float p = arenaLookUpPitch(L, cam.pivot, arm, cam.up, away, top, cam.lockPitch);
            CHECK(p < L.viewMax);
            CHECK(arenaSightHeightAt(L, cam.pivot, arm, cam.up, away, p) == doctest::Approx(top).epsilon(0.002));
        }
    }
    // It needs well over the old 15 degree limit: from the kite's preferred range, aimed, its top is above 30 degrees.
    CHECK(arenaLookUpPitch(L, cam.pivot, cam.arm * aim.armScale, cam.up, 6.5f, top, cam.lockPitch) > 30.f);
}

namespace {

// The followed view looks up at a boss this tall or taller, through this share of its height.
constexpr float kTallBoss = kArenaLookTallBoss;
constexpr float kFaceShare = kArenaLookFaceShare;

struct Seen {
    float x = 0.f;     // across the screen: -1 the left edge, +1 the right (the field of view is horizontal)
    float y = 0.f;     // up the screen from its centre, in the same units (0: on the view's centre line)
    float ahead = 0.f; // m along the view
};

// The point (px, py, pz) through the arena rig's lens: the lock's line along +x from the lead's feet at the origin, the
// view at viewPitch (the boom and the lens split it) and turned `turn` degrees off that line (UE: + turns right, +y is
// the right at 0). `lens` gets the lens's place.
Seen seenFromLens(const ZoneCamera& cam, float viewPitch, float turn, float px, float py, float pz, float lens[3]) {
    const float k = 3.14159265358979f / 180.f;
    const ArenaPitchSplit split = arenaSplitPitch(kArenaLook, viewPitch);
    const float b = split.boom * k;
    const float v = arenaClampViewPitch(kArenaLook, viewPitch) * k;
    const float y = turn * k;
    const float fb[3] = {std::cos(b) * std::cos(y), std::cos(b) * std::sin(y), std::sin(b)};
    const float ub[3] = {-std::sin(b) * std::cos(y), -std::sin(b) * std::sin(y), std::cos(b)};
    const float r[3] = {-std::sin(y), std::cos(y), 0.f};
    const float fv[3] = {std::cos(v) * std::cos(y), std::cos(v) * std::sin(y), std::sin(v)};
    const float pivot[3] = {0.f, 0.f, cam.pivot};
    for (int i = 0; i < 3; ++i) lens[i] = pivot[i] - fb[i] * cam.arm + r[i] * cam.side + ub[i] * cam.up;
    const float d[3] = {px - lens[0], py - lens[1], pz - lens[2]};
    Seen s;
    s.ahead = d[0] * fv[0] + d[1] * fv[1] + d[2] * fv[2];
    s.x = (d[0] * r[0] + d[1] * r[1]) / s.ahead / std::tan(cam.fov * 0.5f * k);
    const float uv[3] = {-std::sin(v) * std::cos(y), -std::sin(v) * std::sin(y), std::cos(v)};
    s.y = (d[0] * uv[0] + d[1] * uv[1] + d[2] * uv[2]) / s.ahead / std::tan(cam.fov * 0.5f * k);
    return s;
}

// The rigs the framing is checked on: the default zone camera, the aim's pulled-in one, and wider, narrower and
// left-shoulder ones (side, arm and fov all moved).
ZoneCamera rigAt(float arm, float side, float fov) {
    ZoneCamera c{};
    c.arm = arm;
    c.side = side;
    c.fov = fov;
    return c;
}
const ZoneCamera kRigs[] = {
    ZoneCamera{},
    rigAt(ZoneCamera{}.arm * AimRules{}.armScale, AimRules{}.shoulderRight / 100.f, AimRules{}.fov),
    rigAt(9.f, 1.8f, 90.f),
    rigAt(4.f, 0.8f, 55.f),
    rigAt(6.4f, -1.2f, 70.f),
};

// The followed view's pitch the host eases to, as Core gives it.
float lockPitchOf(const ZoneCamera& cam, float d, float turn, float tall) {
    return arenaFrameLockPitch(kArenaLook, cam.pivot, cam.arm, cam.up, cam.lockPitch, d, turn, tall);
}

} // namespace

TEST_CASE("arena frame: the turn puts the target on the view's centre line, held to its cap, opposite the shoulder") {
    const ArenaFrameRules& F = kArenaFrame;
    CHECK(arenaFrameTurn(F, 0.f, 5.f) == 0.f);
    CHECK(arenaFrameTurn(F, std::numeric_limits<float>::quiet_NaN(), 5.f) == 0.f);
    CHECK(arenaFrameTurn(F, 1.2f, std::numeric_limits<float>::quiet_NaN()) == 0.f);
    CHECK(arenaFrameTurn(F, 1.2f, 0.f) == doctest::Approx(-F.maxTurn));
    CHECK(arenaFrameTurn(F, 1.2f, 1.f) == doctest::Approx(-F.maxTurn));
    CHECK(arenaFrameTurn(F, -1.2f, 1.f) == doctest::Approx(F.maxTurn));
    // From |side| / sin(maxTurn) out it is the exact turn; it shrinks with the distance and never passes the cap.
    const float from = 1.2f / std::sin(F.maxTurn * 3.14159265358979f / 180.f);
    CHECK(arenaFrameTurn(F, 1.2f, from + 0.01f) > -F.maxTurn);
    float last = -F.maxTurn;
    for (float d = 0.f; d <= 60.f; d += 0.05f) {
        const float t = arenaFrameTurn(F, 1.2f, d);
        CHECK(t <= 0.f);
        CHECK(t >= -F.maxTurn);
        CHECK(t >= last - 1e-5f);
        last = t;
    }
}

TEST_CASE("arena frame: for each rig, the target centred clear of the lead, the lens as far as the free camera's, every kind") {
    const ArenaTable t = lookArena();
    const ArenaFrameRules& F = kArenaFrame;
    const float leadRadius = 0.4f; // Core's default body
    const float chest = 1.f;
    int checked = 0;
    int thirds = 0;
    for (const ZoneCamera& cam : kRigs)
    for (std::size_t i = 0; i < t.enemies.size(); ++i) {
        const ArenaEnemyKind& kind = t.enemies[i];
        const std::string& id = t.enemyIds[i];
        const float scale = kind.look.skinned() ? kind.look.scale : 1.f;
        const float tall = arenaEnemyHeight(kind) * scale + (kind.look.skinned() ? kind.look.hover : 0.f);
        const float mid = tall >= kTallBoss ? tall * kFaceShare : tall * 0.5f;
        // From touching its body out to 15 m.
        for (float d = kind.bodyRadius * scale + leadRadius; d <= 15.f; d += 0.25f) {
            CAPTURE(id);
            CAPTURE(d);
            CAPTURE(cam.side);
            CAPTURE(cam.arm);
            CAPTURE(cam.fov);
            const float centred = std::fabs(cam.side) / std::sin(F.maxTurn * 3.14159265358979f / 180.f);
            const float turn = arenaFrameTurn(F, cam.side, d);
            const float p0 = lockPitchOf(cam, d, 0.f, tall);
            const float p1 = lockPitchOf(cam, d, turn, tall);
            float lens0[3];
            float lens1[3];
            const Seen lead0 = seenFromLens(cam, p0, 0.f, 0.f, 0.f, chest, lens0);
            const Seen foe0 = seenFromLens(cam, p0, 0.f, d, 0.f, mid, lens0);
            const Seen lead = seenFromLens(cam, p1, turn, 0.f, 0.f, chest, lens1);
            const Seen foe = seenFromLens(cam, p1, turn, d, 0.f, mid, lens1);
            // Both in the middle third wherever the free rig itself keeps the lead there (the default rig always).
            if (std::fabs(lead0.x) <= 1.f / 3.f) {
                CHECK(std::fabs(lead.x) <= 1.f / 3.f);
                CHECK(std::fabs(foe.x) <= 1.f / 3.f);
                ++thirds;
            }
            CHECK(foe.ahead > 0.f);
            // The target on the far side of the centre from the lead's shoulder, and further apart than unframed.
            const float sgn = cam.side >= 0.f ? 1.f : -1.f;
            CHECK(sgn * (foe.x - lead.x) > 0.f);
            CHECK(sgn * (foe.x - lead.x) >= sgn * (foe0.x - lead0.x) - 1e-5f);
            if (d >= centred) CHECK(std::fabs(foe.x) < 1e-3f);
            // The lens exactly the rig's own distance from the pivot at the pitch the host eases to: the turn is an orbit
            // round the pivot and the framing never writes the arm, the side, the up or the pivot (an invariant of what
            // the framing leaves alone, not of a clamp).
            const float lp[3] = {lens1[0], lens1[1], lens1[2] - cam.pivot};
            const float rig = std::sqrt(cam.arm * cam.arm + cam.side * cam.side + cam.up * cam.up);
            CHECK(std::sqrt(lp[0] * lp[0] + lp[1] * lp[1] + lp[2] * lp[2]) == doctest::Approx(rig).epsilon(1e-5));
            ++checked;
        }
    }
    CHECK(checked > 500);
    CHECK(thirds > 100);
}

TEST_CASE("arena frame: the look up at a tall boss goes through its face in the turned view, for each rig") {
    const ArenaTable t = lookArena();
    const ArenaFrameRules& F = kArenaFrame;
    int faced = 0;
    int unturnedMisses = 0;
    for (const ZoneCamera& cam : kRigs)
    for (std::size_t i = 0; i < t.enemies.size(); ++i) {
        const ArenaEnemyKind& kind = t.enemies[i];
        const float scale = kind.look.skinned() ? kind.look.scale : 1.f;
        const float tall = arenaEnemyHeight(kind) * scale + (kind.look.skinned() ? kind.look.hover : 0.f);
        if (tall < kTallBoss) {
            // Short: the lock's own pitch, turned or not.
            CHECK(lockPitchOf(cam, 3.f, arenaFrameTurn(F, cam.side, 3.f), tall) == cam.lockPitch);
            continue;
        }
        for (float d = kind.bodyRadius * scale + 0.4f; d <= 15.f; d += 0.25f) {
            CAPTURE(t.enemyIds[i]);
            CAPTURE(d);
            CAPTURE(cam.side);
            const float turn = arenaFrameTurn(F, cam.side, d);
            const float p = lockPitchOf(cam, d, turn, tall);
            if (p >= kArenaLook.viewMax - 0.01f || p <= cam.lockPitch + 0.01f) continue; // held at a limit: nothing to solve
            float lens[3];
            const Seen face = seenFromLens(cam, p, turn, d, 0.f, tall * kFaceShare, lens);
            CHECK(std::fabs(face.y) < 2e-3f);
            // The solve with the unturned distance misses the face in the turned view: the cos(turn) is what puts it there.
            const float naive = arenaLookUpPitch(kArenaLook, cam.pivot, cam.arm, cam.up, d, tall * kFaceShare, cam.lockPitch);
            if (std::fabs(seenFromLens(cam, naive, turn, d, 0.f, tall * kFaceShare, lens).y) > 0.02f) ++unturnedMisses;
            ++faced;
        }
    }
    CHECK(faced > 20);
    CHECK(unturnedMisses > 0);
}

TEST_CASE("arena frame: the stick moves the lead along the drawn view, and a release leaves its basis where it was") {
    const ArenaFrameRules& F = kArenaFrame;
    const float k = 3.14159265358979f / 180.f;
    const float dt = 1.f / 60.f;
    // The basis is the drawn yaw (line + turn) at every line and turn, the wrap included: up the screen is the view's
    // heading and right is its right.
    for (float line : {-180.f, -179.5f, -90.f, 0.f, 45.f, 179.5f, 180.f})
    for (float turn : {-F.maxTurn, -12.f, 0.f, 12.f, F.maxTurn}) {
        CAPTURE(line);
        CAPTURE(turn);
        const float v = arenaFrameViewYaw(ArenaFrameYaw{line, turn});
        const Vec2 up = arenaStickToWorld(v, 1.f, 0.f);
        const Vec2 right = arenaStickToWorld(v, 0.f, 1.f);
        CHECK(up.x == doctest::Approx(std::cos((line + turn) * k)).epsilon(1e-4));
        CHECK(up.y == doctest::Approx(std::sin((line + turn) * k)).epsilon(1e-4));
        CHECK(right.x == doctest::Approx(-std::sin((line + turn) * k)).epsilon(1e-4));
        CHECK(right.y == doctest::Approx(std::cos((line + turn) * k)).epsilon(1e-4));
    }
    // A lock eased in on a target at contact, the stick held, then let go: the frame of the release moves the held
    // stick's world way by less than 1e-3 degrees, near the wrap too.
    for (float start : {0.f, 170.f, -175.f}) {
        CAPTURE(start);
        ArenaFrameYaw yaw{start, 0.f};
        ArenaFrameScene s;
        s.followed = true;
        s.hasLine = true;
        s.lineWant = start + 8.f;
        s.side = 1.2f;
        s.distance = 1.5f;
        for (int i = 0; i < 120; ++i) yaw = arenaFrameYawStep(F, yaw, s, dt);
        REQUIRE(yaw.turn == doctest::Approx(-F.maxTurn));
        const float lineBefore = yaw.line;
        const Vec2 before = arenaStickToWorld(arenaFrameViewYaw(yaw), 0.6f, 0.8f);
        s.followed = false;
        yaw = arenaFrameYawStep(F, yaw, s, dt);
        CHECK(yaw.turn == 0.f);
        const Vec2 after = arenaStickToWorld(arenaFrameViewYaw(yaw), 0.6f, 0.8f);
        const float turned = std::atan2(before.x * after.y - before.y * after.x, before.x * after.x + before.y * after.y) / k;
        CHECK(std::fabs(turned) < 1e-3f);
        // A basis on the line alone would have jumped by the whole turn here: the check above can tell the two apart.
        CHECK(std::fabs(arenaFrameViewYaw(ArenaFrameYaw{lineBefore, 0.f}) - arenaFrameViewYaw(yaw)) > 1.f);
    }
    // Under the lock at the cap, the stick pulled straight down the screen still asks for the hop back, and pushed
    // sideways for a side dash: within ArenaFight's back cone (kArenaDodgeBackCos, 45 degrees) and outside it.
    {
        const float lineYaw = 20.f;
        const float v = arenaFrameViewYaw(ArenaFrameYaw{lineYaw, -F.maxTurn});
        const Vec2 to{std::cos(lineYaw * k), std::sin(lineYaw * k)};
        const Vec2 back = arenaStickToWorld(v, -1.f, 0.f);
        const Vec2 side = arenaStickToWorld(v, 0.f, 1.f);
        const Vec2 ahead = arenaStickToWorld(v, 1.f, 0.f);
        constexpr float kBackCos = 0.7071f; // ArenaFight.cpp's kArenaDodgeBackCos
        CHECK(back.x * to.x + back.y * to.y <= -kBackCos);
        CHECK(std::fabs(side.x * to.x + side.y * to.y) < kBackCos);
        CHECK(ahead.x * to.x + ahead.y * to.y >= kBackCos);
    }
}

TEST_CASE("arena frame: the yaw step: no turn while aiming or with framing off, a fold when nothing is followed, held in a hit pause") {
    const ArenaFrameRules& F = kArenaFrame;
    const float dt = 1.f / 60.f;
    ArenaFrameScene s;
    s.followed = true;
    s.hasLine = true;
    s.lineWant = 0.f;
    s.side = 1.2f;
    s.distance = 1.5f;
    // Followed: the turn goes to the cap and the line onto the target.
    ArenaFrameYaw yaw{30.f, 0.f};
    for (int i = 0; i < 180; ++i) yaw = arenaFrameYawStep(F, yaw, s, dt);
    CHECK(yaw.turn == doctest::Approx(-F.maxTurn));
    CHECK(std::fabs(yaw.line) < 1e-3f);
    // Aiming, and framing off: the turn eases to exactly 0 with no jump, and stays 0.
    for (int mode = 0; mode < 2; ++mode) {
        CAPTURE(mode);
        ArenaFrameScene a = s;
        a.aiming = mode == 0;
        a.framing = mode == 0;
        ArenaFrameYaw y = yaw;
        const float most = F.maxTurn * (1.f - std::exp(-F.rate * dt)) + 1e-4f;
        int steps = 0;
        while (y.turn != 0.f && steps < 600) {
            const ArenaFrameYaw next = arenaFrameYawStep(F, y, a, dt);
            CHECK(std::fabs(next.turn - y.turn) <= most);
            y = next;
            ++steps;
        }
        CHECK(y.turn == 0.f);
        CHECK(steps * dt < 1.5f);
        for (int i = 0; i < 60; ++i) y = arenaFrameYawStep(F, y, a, dt);
        CHECK(y.turn == 0.f);
    }
    // Nothing followed: the turn goes into the line and the drawn yaw holds.
    ArenaFrameScene free = s;
    free.followed = false;
    const ArenaFrameYaw folded = arenaFrameYawStep(F, yaw, free, dt);
    CHECK(folded.turn == 0.f);
    CHECK(std::fabs(arenaFrameViewYaw(folded) - arenaFrameViewYaw(yaw)) < 1e-4f);
    // The hit pause: nothing moves, followed or not, aiming or not, line and turn bit-identical.
    for (int c = 0; c < 4; ++c) {
        ArenaFrameScene h = s;
        h.followed = (c & 1) != 0;
        h.aiming = (c & 2) != 0;
        h.lineWant = 90.f;
        h.held = true;
        const ArenaFrameYaw kept = arenaFrameYawStep(F, ArenaFrameYaw{12.5f, -17.25f}, h, dt);
        CHECK(kept.line == 12.5f);
        CHECK(kept.turn == -17.25f);
    }
    // dt 0 holds too, and a NaN turn counts as 0.
    const ArenaFrameYaw z = arenaFrameYawStep(F, ArenaFrameYaw{5.f, -3.f}, s, 0.f);
    CHECK(z.line == 5.f);
    CHECK(z.turn == -3.f);
    CHECK(arenaFrameYawStep(F, ArenaFrameYaw{5.f, std::numeric_limits<float>::quiet_NaN()}, free, dt).turn == 0.f);
}

TEST_CASE("arena frame: aiming in and out under a lock, the target's place on screen swings back no more than 1% of the width") {
    // The aim's own blend (the arm in, the shoulder narrower, the fov 70 -> 55 over 0.15 s) moves the target a little
    // back against its way even with no framing; the framing easing out over the blend adds a swing of the same size.
    // Both stay under 0.02 of the half-width (1% of the screen's width), for aiming in and letting go.
    const ArenaFrameRules& F = kArenaFrame;
    const ZoneCamera rig{};
    const AimRules aimRules{};
    const float dt = 1.f / 60.f;
    for (float d : {1.2f, 2.4f, 4.f, 8.f})
    for (int letGo = 0; letGo < 2; ++letGo)
    for (int framing = 0; framing < 2; ++framing) {
        CAPTURE(d);
        CAPTURE(letGo);
        CAPTURE(framing);
        AimStance aim;
        ArenaFrameScene s;
        s.followed = true;
        s.hasLine = true;
        s.side = rig.side;
        s.distance = d;
        s.framing = framing != 0;
        ArenaFrameYaw yaw{0.f, framing ? arenaFrameTurn(F, rig.side, d) : 0.f};
        if (letGo) {
            // Fully aimed first (the turn 0), then let go.
            for (int i = 0; i < 60; ++i) aim.tick(dt, true, FieldBody::Free);
            yaw.turn = 0.f;
        }
        float xs[150];
        for (int i = 0; i < 150; ++i) {
            aim.tick(dt, letGo == 0, FieldBody::Free);
            s.aiming = aim.aiming();
            yaw = arenaFrameYawStep(F, yaw, s, dt);
            ZoneCamera cam = rig;
            cam.arm = aim.blend(rig.arm, rig.arm * aimRules.armScale);
            cam.side = aim.blend(rig.side, aimRules.shoulderRight / 100.f);
            cam.fov = aim.blend(rig.fov, aimRules.fov);
            float lens[3];
            xs[i] = seenFromLens(cam, rig.lockPitch, yaw.turn, d, 0.f, 1.f, lens).x;
        }
        const float net = xs[149] - xs[0];
        float back = 0.f;
        for (int i = 1; i < 150; ++i) {
            const float dx = xs[i] - xs[i - 1];
            if (dx * net < 0.f) back += std::fabs(dx);
        }
        CHECK(std::fabs(xs[149] - xs[148]) < 1e-5f); // settled
        CHECK(back < 0.02f);
    }
}

TEST_CASE("arena frame: the applied turn eases to its want with no jump, arrives, and an off framing stays exactly 0") {
    const ArenaFrameRules& F = kArenaFrame;
    const float dt = 1.f / 60.f;
    // Off: 0 toward 0 is 0, every step.
    float off = 0.f;
    for (int i = 0; i < 600; ++i) off = arenaFrameStep(F, off, 0.f, dt);
    CHECK(off == 0.f);
    CHECK(arenaFrameStep(F, 3.f, -10.f, 0.f) == 3.f);
    CHECK(arenaFrameStep(F, 3.f, std::numeric_limits<float>::quiet_NaN(), dt) == 3.f);
    CHECK(arenaFrameStep(F, std::numeric_limits<float>::quiet_NaN(), 0.f, dt) == 0.f);
    // The lock taken from far (0) to a target at the cap, a switch to a far one, and the lock let go (back to 0): each
    // step no larger than the ease's first step from the cap, never past the want, and there in under 1.5 s.
    const float most = F.maxTurn * (1.f - std::exp(-F.rate * dt)) + 1e-4f;
    const float wants[3] = {-F.maxTurn, arenaFrameTurn(F, 1.2f, 15.f), 0.f};
    float a = 0.f;
    for (float want : wants) {
        int steps = 0;
        while (a != want && steps < 600) {
            const float next = arenaFrameStep(F, a, want, dt);
            CHECK(std::fabs(next - a) <= most);
            CHECK(std::fabs(want - next) <= std::fabs(want - a));
            CHECK((want - next) * (want - a) >= 0.f);
            a = next;
            ++steps;
        }
        CHECK(a == want);
        CHECK(steps * dt < 1.5f);
    }
}

TEST_CASE("arena look: a low target keeps the lock's own pitch, an unreachable one the top limit") {
    const ArenaLookLimits& L = kArenaLook;
    const ZoneCamera cam{};
    // Under where the lock's own pitch already looks at that range: the lock's pitch, never lower.
    const float under = arenaSightHeightAt(L, cam.pivot, cam.arm, cam.up, 8.f, cam.lockPitch) - 0.5f;
    CHECK(arenaLookUpPitch(L, cam.pivot, cam.arm, cam.up, 8.f, under, cam.lockPitch) == doctest::Approx(cam.lockPitch));
    CHECK(arenaLookUpPitch(L, cam.pivot, cam.arm, cam.up, 0.f, 500.f, cam.lockPitch) == doctest::Approx(L.viewMax));
    // The answer rises with the target's height.
    float last = -90.f;
    for (float h = 1.f; h <= 12.f; h += 1.f) {
        const float p = arenaLookUpPitch(L, cam.pivot, cam.arm, cam.up, 6.f, h, cam.lockPitch);
        CHECK(p >= last);
        last = p;
    }
}

TEST_CASE("arenaRayEntersBody: the crosshair's ray meets a body only from outside, between its base and top") {
    ArenaBodyCylinder body;
    body.at = {10.f, 0.f};
    body.base = 0.f;
    body.top = 2.f;
    body.radius = 0.5f;
    float t = -1.f;
    // Straight at its middle from 10 m away: enters at its near side, 9.5 m along a 20 m line.
    REQUIRE(arenaRayEntersBody({0.f, 0.f, 1.f}, {20.f, 0.f, 1.f}, body, t));
    CHECK(t == doctest::Approx(9.5f / 20.f));
    // The lens inside a flanker's body: no hit (it would put the crosshair on the camera and the shot backward).
    CHECK_FALSE(arenaRayEntersBody({10.2f, 0.f, 1.f}, {30.f, 0.f, 1.f}, body, t));
    // A body behind the lens.
    CHECK_FALSE(arenaRayEntersBody({20.f, 0.f, 1.f}, {40.f, 0.f, 1.f}, body, t));
    // Just over its head, and just beside it.
    CHECK_FALSE(arenaRayEntersBody({0.f, 0.f, 2.05f}, {20.f, 0.f, 2.05f}, body, t));
    CHECK_FALSE(arenaRayEntersBody({0.f, 0.6f, 1.f}, {20.f, 0.6f, 1.f}, body, t));
    // Short of it: the segment ends before the body.
    CHECK_FALSE(arenaRayEntersBody({0.f, 0.f, 1.f}, {9.f, 0.f, 1.f}, body, t));
    // Down through its top from above it.
    REQUIRE(arenaRayEntersBody({10.f, 0.f, 5.f}, {10.f, 0.f, -1.f}, body, t));
    CHECK(t == doctest::Approx(0.5f));
    // A steep line up into it from under its base comes in through the base.
    ArenaBodyCylinder high = body;
    high.base = 3.f;
    high.top = 6.f;
    REQUIRE(arenaRayEntersBody({9.9f, 0.f, 1.f}, {10.1f, 0.f, 9.f}, high, t));
    CHECK(t == doctest::Approx(0.25f));
}

TEST_CASE("arenaBodyCylinder: a hovering, scaled body; the gap under it is empty") {
    const ArenaTable table = lookArena();
    const ArenaEnemyKind* hovering = nullptr;
    for (const ArenaEnemyKind& k : table.enemies)
        if (k.look.skinned() && k.look.hover > 0.f) hovering = &k;
    REQUIRE(hovering != nullptr);
    ArenaEnemyKind scaled = *hovering; // the data's hovering kind, drawn twice its size
    scaled.look.scale = 2.f;
    const ArenaEnemyKind* kite = &scaled;
    const ArenaBodyCylinder b = arenaBodyCylinder(*kite, {12.f, 0.f}, 0.f);
    CHECK(b.base == doctest::Approx(kite->look.hover));
    CHECK(b.top == doctest::Approx(kite->look.hover + arenaEnemyHeight(*kite) * kite->look.scale));
    CHECK(b.radius == doctest::Approx(kite->bodyRadius * kite->look.scale));
    float t = 0.f;
    // A level line through the gap under it misses; one through its body hits.
    CHECK_FALSE(arenaRayEntersBody({0.f, 0.f, kite->look.hover * 0.5f}, {40.f, 0.f, kite->look.hover * 0.5f}, b, t));
    const float mid = (b.base + b.top) * 0.5f;
    CHECK(arenaRayEntersBody({0.f, 0.f, mid}, {40.f, 0.f, mid}, b, t));
    // The scaled radius: a line beside the unscaled radius, inside the scaled one, hits.
    const float y = kite->bodyRadius * (1.f + kite->look.scale) * 0.5f;
    CHECK(arenaRayEntersBody({0.f, y, mid}, {40.f, y, mid}, b, t));
}
