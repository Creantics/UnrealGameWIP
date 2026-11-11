#include "ftcore/AimStance.h"
#include "ftcore/AttackMove.h"
#include "ftcore/FieldStrike.h"
#include "ftcore/FieldUnstick.h"
#include "TestAssets.h"
#include <ostream>
#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 60.f;

ArenaAttack swing(float startup, float active, float recovery) {
    ArenaAttack a;
    a.startup = startup;
    a.active = active;
    a.recovery = recovery;
    return a;
}

} // namespace

TEST_CASE("attack move: the shipped table loads and every weapon keeps the lead moving on a light") {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Battle/attack_move.json", text));
    AttackMoveRules r;
    std::string err;
    REQUIRE_MESSAGE(importAttackMoveRules(text, r, &err), err);
    for (int k = 0; k < 3; ++k) {
        CHECK(r.light[k].speedScale >= 0.5f);
        CHECK(r.light[k].speedScale <= 0.7f);
        CHECK(r.light[k].turnRate > 0.f);
        CHECK(r.heavy[k].commit > 0.f);
        CHECK(r.heavy[k].commit <= 0.4f);
    }
    CHECK(r.upperNone < r.upperFull);
}

TEST_CASE("attack move: the import rejects unknown keys, a light commit and values out of range") {
    AttackMoveRules r;
    const AttackMoveRules before = r;
    std::string err;
    CHECK_FALSE(importAttackMoveRules(R"({"lite": {}})", r, &err));
    CHECK_FALSE(importAttackMoveRules(R"({"light": {"sword": {}}})", r, &err));
    CHECK_FALSE(importAttackMoveRules(R"({"light": {"focus": {"commit": 0.2}}})", r, &err));
    CHECK_FALSE(importAttackMoveRules(R"({"heavy": {"fire": {"speedScale": 1.5}}})", r, &err));
    CHECK_FALSE(importAttackMoveRules(R"({"upperNone": 0.8, "upperFull": 0.6})", r, &err));
    CHECK(r.light[0].speedScale == before.light[0].speedScale);
    REQUIRE(importAttackMoveRules(R"({"heavy": {"cast": {"commit": 0.2}}})", r, &err));
    CHECK(r.heavy[2].commit == doctest::Approx(0.2f));
    CHECK(r.heavy[0].commit == doctest::Approx(before.heavy[0].commit));
}

TEST_CASE("attack move: a light never roots, walks at its scale through active and ramps back in recovery") {
    const AttackMoveRules r;
    const ArenaAttack a = swing(0.2f, 0.1f, 0.4f);
    for (float t = 0.f; t < a.total(); t += kDt) {
        const AttackMoveOut o = attackMove(r, AimKind::Focus, false, a, t);
        CHECK_FALSE(o.rooted);
        CHECK(o.turnRate == doctest::Approx(r.light[0].turnRate)); // capped, never locked
        if (t < 0.3f) CHECK(o.speedScale == doctest::Approx(0.6f));
        else CHECK(o.speedScale >= 0.6f - 1e-5f);
    }
    CHECK(attackMove(r, AimKind::Focus, false, a, 0.5f).speedScale == doctest::Approx(0.8f));
    const AttackMoveOut after = attackMove(r, AimKind::Focus, false, a, a.total() + 0.01f);
    CHECK(after.speedScale == 1.f);
    CHECK(after.turnRate == 0.f);
}

TEST_CASE("attack move: a heavy walks, commits before its hit frame still steering, and holds through active") {
    const AttackMoveRules r; // focus heavy: 0.4 speed, 4 rad/s, commit 0.3
    const ArenaAttack a = swing(0.5f, 0.15f, 0.5f);
    const AttackMoveOut walk = attackMove(r, AimKind::Focus, true, a, 0.1f);
    CHECK_FALSE(walk.rooted);
    CHECK(walk.speedScale == doctest::Approx(0.4f));
    const AttackMoveOut commit = attackMove(r, AimKind::Focus, true, a, 0.3f); // 0.2 s..0.5 s is the commit
    CHECK(commit.rooted);
    CHECK(commit.speedScale == 0.f);
    CHECK(commit.turnRate == doctest::Approx(4.f));
    const AttackMoveOut hit = attackMove(r, AimKind::Focus, true, a, 0.55f);
    CHECK(hit.rooted);
    CHECK(hit.turnRate == 0.f);
    const AttackMoveOut rec = attackMove(r, AimKind::Focus, true, a, 0.65f + 0.25f);
    CHECK_FALSE(rec.rooted);
    CHECK(rec.speedScale == doctest::Approx(0.5f));
    // The root lasts commit + active, about 0.3 s plus the hit.
    float rooted = 0.f;
    for (float t = 0.f; t < a.total(); t += 0.001f) rooted += attackMove(r, AimKind::Focus, true, a, t).rooted ? 0.001f : 0.f;
    CHECK(rooted == doctest::Approx(0.45f).epsilon(0.02));
}

TEST_CASE("attack move: the strike plays on the whole body standing and on the upper body walking") {
    const AttackMoveRules r;
    CHECK(attackUpperShare(r, 0.f) == 0.f);
    CHECK(attackUpperShare(r, r.upperNone) == 0.f);
    CHECK(attackUpperShare(r, r.upperFull) == 1.f);
    CHECK(attackUpperShare(r, 3.f) == 1.f);
    const float mid = attackUpperShare(r, 0.5f * (r.upperNone + r.upperFull));
    CHECK(mid == doctest::Approx(0.5f));
    float last = 0.f;
    for (float s = 0.f; s < 1.f; s += 0.01f) {
        const float u = attackUpperShare(r, s);
        CHECK(u >= last - 1e-6f);
        last = u;
    }
}

TEST_CASE("attack move: the capped turn goes the short way and never past the wanted facing") {
    const Vec2 east{1.f, 0.f};
    const Vec2 north{0.f, 1.f};
    const Vec2 a = turnCapped(east, north, 0.5f);
    CHECK(std::atan2(a.y, a.x) == doctest::Approx(0.5f));
    const Vec2 b = turnCapped(east, Vec2{0.f, -1.f}, 0.5f);
    CHECK(std::atan2(b.y, b.x) == doctest::Approx(-0.5f));
    const Vec2 c = turnCapped(east, north, 3.f);
    CHECK(c.x == doctest::Approx(0.f));
    CHECK(c.y == doctest::Approx(1.f));
    const Vec2 d = turnCapped(east, Vec2{}, 1.f);
    CHECK(d.x == 1.f);
    // Turning at 6 rad/s for a 0.7 s light covers a full about-face: the body keeps up with the stick.
    Vec2 f = east;
    for (float t = 0.f; t < 0.7f; t += kDt) f = turnCapped(f, Vec2{-1.f, 0.f}, 6.f * kDt);
    CHECK(f.x == doctest::Approx(-1.f));
}

TEST_CASE("unstick: the shipped rules load") {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Movement/unstick.json", text));
    UnstickRules r;
    std::string err;
    REQUIRE_MESSAGE(importUnstickRules(text, r, &err), err);
    CHECK(r.stuckTime == doctest::Approx(1.5f));
    CHECK(r.stuckMove == doctest::Approx(0.5f));
    CHECK(r.fallTime == doctest::Approx(4.f));
    CHECK_FALSE(importUnstickRules(R"({"stuckTimer": 1})", r, &err));
    CHECK_FALSE(importUnstickRules(R"({"walkableZ": 2})", r, &err));
}

TEST_CASE("unstick: a too-steep floor slides the lead downhill, a walkable one does not") {
    FieldUnstick u;
    const float steep = std::cos(60.f * 3.14159265f / 180.f); // a 60 degree bank, its normal leaning toward +x
    const Vec2 nxy{std::sin(60.f * 3.14159265f / 180.f), 0.f};
    u.tick(kDt, Vec2{0.f, 0.f}, 0.f, true, nxy, steep, false);
    CHECK(u.sliding());
    CHECK(u.slideDir().x == doctest::Approx(1.f));
    u.tick(kDt, Vec2{0.f, 0.f}, 0.f, true, Vec2{0.2f, 0.f}, 0.98f, false);
    CHECK_FALSE(u.sliding());
    // In the air nothing slides.
    u.tick(kDt, Vec2{0.f, 0.f}, 0.f, false, nxy, steep, true);
    CHECK_FALSE(u.sliding());
}

TEST_CASE("unstick: stuck in a wedge for 1.5 s steps the lead back to a safe point away from the lip") {
    FieldUnstick u;
    // Walk 6 m east on flat ground: safe points every metre.
    for (int i = 0; i <= 60; ++i) u.tick(kDt, Vec2{i * 0.1f, 0.f}, 0.f, true, Vec2{}, 1.f, false);
    CHECK(u.safeCount() == 7);
    // Into the wedge at 6.3 m: a steep bank, jittering in place.
    const Vec2 nxy{0.9f, 0.f};
    bool fired = false;
    float t = 0.f;
    while (!fired && t < 3.f) {
        const float jitter = 0.05f * std::sin(t * 20.f);
        fired = u.tick(kDt, Vec2{6.3f + jitter, 0.f}, -0.4f, true, nxy, 0.3f, false);
        t += kDt;
    }
    REQUIRE(fired);
    CHECK(t == doctest::Approx(1.5f).epsilon(0.03));
    CHECK(u.rescueTo().at.x <= 6.3f - 1.5f + 1e-4f); // backoff from the lip
    CHECK(u.rescueTo().at.x == doctest::Approx(4.f));
    CHECK(u.fade() > 0.9f);
    for (int i = 0; i < 30; ++i) u.tick(kDt, u.rescueTo().at, 0.f, true, Vec2{}, 1.f, false);
    CHECK(u.fade() == 0.f);
}

TEST_CASE("unstick: a slide that gets somewhere and a long drop are not stuck; a fall past 4 s is") {
    FieldUnstick u;
    for (int i = 0; i <= 30; ++i) u.tick(kDt, Vec2{i * 0.1f, 0.f}, 0.f, true, Vec2{}, 1.f, false);
    // Sliding down a long bank at 4 m/s for 3 s: moving, so no rescue.
    bool fired = false;
    for (int i = 0; i < 180 && !fired; ++i)
        fired = u.tick(kDt, Vec2{3.f + i * 4.f * kDt, 0.f}, -i * 2.f * kDt, true, Vec2{0.8f, 0.f}, 0.5f, false);
    CHECK_FALSE(fired);
    // Falling straight down 3 s (a cliff drop): moving in height, under fallTime, no rescue.
    FieldUnstick d;
    for (int i = 0; i <= 30; ++i) d.tick(kDt, Vec2{i * 0.1f, 0.f}, 0.f, true, Vec2{}, 1.f, false);
    fired = false;
    float z = 0.f, vz = 0.f;
    for (int i = 0; i < 180 && !fired; ++i) {
        vz -= 9.8f * kDt;
        z += vz * kDt;
        fired = d.tick(kDt, Vec2{3.f, 0.f}, z, false, Vec2{}, 0.f, true);
    }
    CHECK_FALSE(fired);
    // Still falling past 4 s: rescued.
    float t = 3.f;
    while (!fired && t < 6.f) {
        vz -= 9.8f * kDt;
        z += vz * kDt;
        fired = d.tick(kDt, Vec2{3.f, 0.f}, z, false, Vec2{}, 0.f, true);
        t += kDt;
    }
    CHECK(fired);
    CHECK(t == doctest::Approx(4.f).epsilon(0.02));
}

TEST_CASE("unstick: with no safe point yet there is nowhere to go, so no rescue fires") {
    FieldUnstick u;
    bool fired = false;
    for (int i = 0; i < 200; ++i) fired = fired || u.tick(kDt, Vec2{}, 0.f, true, Vec2{0.9f, 0.f}, 0.3f, false);
    CHECK_FALSE(fired);
    CHECK(u.sliding());
}

namespace {

ArenaTable shippedArenaTable() {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/arena.json", text)) FAIL("Data/Battle/arena.json not found");
    ArenaTable t;
    std::string err;
    REQUIRE_MESSAGE(importArenaTable(text, t, &err), err);
    return t;
}

struct MovingLight {
    float speedY = 0.f;   // the velocity across (+y) halfway into the light
    float facingY = 0.f;  // the facing's y at the light's end
    bool wasLight = false;
};

// A lead standing facing +x throws a light (no lock); once its startup is over the stick goes +y. Measured on the
// light's last tick (the lunge over).
MovingLight arenaMovingLight(float attackSpeed, float attackTurn) {
    const ArenaTable t = shippedArenaTable();
    ArenaDef def = t.fallback;
    def.fighter = t.fighter("devin");
    def.enemyCount = 1;
    ArenaFight fight;
    fight.begin(def, ArenaRing{Vec2{0.f, 0.f}, 40.f}, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
    ArenaBodies bodies;
    bodies.enemies[0] = Vec2{30.f, 30.f}; // far off, out of reach
    ArenaInput in;
    in.attackSpeed = attackSpeed;
    in.attackTurn = attackTurn;
    in.light = true;
    const ArenaOutput* out = &fight.tick(kDt, in, bodies);
    in.light = false;
    const ArenaAttack& a = *arenaPlayerAttack(def.fighter, 0);
    while (out->player.action == ArenaAction::Light && out->player.actionTime < a.startup) out = &fight.tick(kDt, in, bodies);
    in.move = Vec2{0.f, 1.f};
    MovingLight m;
    m.wasLight = out->player.action == ArenaAction::Light;
    while (out->player.action == ArenaAction::Light && out->player.actionTime + kDt < a.total()) {
        m.speedY = out->player.velocity.y;
        m.facingY = out->player.facing.y;
        out = &fight.tick(kDt, in, bodies);
    }
    return m;
}

} // namespace

TEST_CASE("attack move: in the arena a light on the move keeps walking and turning; without it the lead roots") {
    const ArenaTable t = shippedArenaTable();
    const float pace = t.fighter("devin").moveSpeed;
    const MovingLight rooted = arenaMovingLight(0.f, 0.f);
    const MovingLight moving = arenaMovingLight(0.6f, 6.f);
    REQUIRE(rooted.wasLight);
    REQUIRE(moving.wasLight);
    CHECK(std::fabs(rooted.speedY) < 0.05f * pace);           // the old root: no walking across
    CHECK(moving.speedY == doctest::Approx(0.6f * pace).epsilon(0.1)); // walking on at 60 percent
    CHECK(std::fabs(rooted.facingY) < 0.05f);                 // the old hold after startup
    CHECK(moving.facingY > 0.9f);                             // turned on toward the stick through the swing
}

TEST_CASE("attack move: a field strike with nothing to face turns with the body, capped; a reticle holds it") {
    const ArenaTable t = shippedArenaTable();
    const ArenaFighter kit = t.fighter("devin");
    const auto run = [&](float rate, bool reticle) {
        FieldStrike s;
        s.begin(kit);
        if (reticle) s.setReticle(true, Vec2{10.f, 0.f}, 1.f);
        s.tick(kDt, true, false, Vec2{}, Vec2{1.f, 0.f}, nullptr, 0);
        Vec2 f{1.f, 0.f};
        for (int i = 0; i < 12 && s.attacking(); ++i) {
            s.setSteer(Vec2{0.f, 1.f}, rate);
            f = s.tick(kDt, false, false, Vec2{}, Vec2{0.f, 1.f}, nullptr, 0).player.facing;
        }
        return f;
    };
    const Vec2 held = run(0.f, false);
    CHECK(held.x == doctest::Approx(1.f));
    const Vec2 steered = run(6.f, false);
    const float turned = std::atan2(steered.y, steered.x);
    CHECK(turned > 0.5f);
    CHECK(turned <= 6.f * 12.f * kDt + 1e-3f); // never faster than the cap
    const Vec2 aimed = run(6.f, true);
    CHECK(aimed.x == doctest::Approx(1.f).epsilon(0.01));
}

TEST_CASE("unstick: the ring keeps the newest 8 safe points and a rescue goes to the newest one past the backoff") {
    FieldUnstick u;
    for (int i = 0; i <= 120; ++i) u.tick(kDt, Vec2{i * 0.1f, 0.f}, 0.f, true, Vec2{}, 1.f, false); // 0..12 m
    CHECK(u.safeCount() == FieldUnstick::kRing);
    bool fired = false;
    for (int i = 0; i < 200 && !fired; ++i) fired = u.tick(kDt, Vec2{12.2f, 0.f}, -0.5f, true, Vec2{0.9f, 0.f}, 0.3f, false);
    REQUIRE(fired);
    CHECK(u.rescueTo().at.x == doctest::Approx(10.f)); // 12, 11 too near; the older ones (4 m and back) never matter
}

TEST_CASE("unstick: reset and a teleport forget the safe points; a zero frame counts nothing") {
    FieldUnstick u;
    for (int i = 0; i <= 30; ++i) u.tick(kDt, Vec2{i * 0.1f, 0.f}, 0.f, true, Vec2{}, 1.f, false);
    REQUIRE(u.safeCount() > 0);
    u.reset();
    CHECK(u.safeCount() == 0);
    for (int i = 0; i <= 30; ++i) u.tick(kDt, Vec2{i * 0.1f, 0.f}, 0.f, true, Vec2{}, 1.f, false);
    REQUIRE(u.safeCount() == 4);
    u.tick(kDt, Vec2{80.f, 40.f}, 5.f, false, Vec2{}, 1.f, true); // a scene put the lead elsewhere
    CHECK(u.safeCount() == 0);
    const float stuck = u.stuckFor();
    for (int i = 0; i < 100; ++i) CHECK_FALSE(u.tick(0.f, Vec2{80.f, 40.f}, 5.f, true, Vec2{0.9f, 0.f}, 0.3f, false));
    CHECK(u.stuckFor() == stuck);
}

TEST_CASE("unstick: a short jump is never a rescue, and the host's unwalkable verdict wins over an upward normal") {
    FieldUnstick u;
    for (int i = 0; i <= 30; ++i) u.tick(kDt, Vec2{i * 0.1f, 0.f}, 0.f, true, Vec2{}, 1.f, false);
    for (int i = 0; i < 40; ++i) CHECK_FALSE(u.tick(kDt, Vec2{3.f + i * 0.05f, 0.f}, 0.5f * std::sin(i / 40.f * 3.14159f), false, Vec2{}, 1.f, true));
    u.tick(kDt, Vec2{5.f, 0.f}, 0.f, true, Vec2{}, 1.f, false);
    CHECK(u.fallingFor() == 0.f);
    CHECK(u.stuckFor() == 0.f);
    // A lip the host calls unwalkable, normal straight up: counted as stuck, not as safe ground.
    const int before = u.safeCount();
    bool fired = false;
    for (int i = 0; i < 120 && !fired; ++i) fired = u.tick(kDt, Vec2{5.f, 0.f}, 0.f, true, Vec2{}, 1.f, false, 0);
    CHECK(u.safeCount() == before);
    CHECK(fired);
}

TEST_CASE("unstick: the slide pushes the ground velocity downhill up to slideSpeed, and leaves it be when not sliding") {
    FieldUnstick u;
    u.tick(kDt, Vec2{}, 0.f, true, Vec2{0.8f, 0.f}, 0.6f, false);
    REQUIRE(u.sliding());
    Vec2 v{};
    for (int i = 0; i < 120; ++i) v = u.slideVelocity(v, kDt);
    CHECK(v.x == doctest::Approx(u.rules().slideSpeed).epsilon(0.02));
    CHECK(v.y == doctest::Approx(0.f));
    const Vec2 fast = u.slideVelocity(Vec2{9.f, 1.f}, kDt);
    CHECK(fast.x == doctest::Approx(9.f));
    u.tick(kDt, Vec2{}, 0.f, true, Vec2{}, 1.f, false);
    CHECK(u.slideVelocity(Vec2{1.f, 2.f}, kDt).y == doctest::Approx(2.f));
}

TEST_CASE("aim: a heavy reached while busy waits for the attack to end; a new press drops it") {
    PressHold p;
    bool heavy = false;
    for (int i = 0; i < 30; ++i) heavy |= p.tick(kDt, true, 0.25f, true).heavy;
    CHECK_FALSE(heavy);
    CHECK(p.heavyPending());
    CHECK_FALSE(p.tick(kDt, false, 0.25f, true).light); // the release of a heavy is no light
    CHECK(p.tick(kDt, false, 0.25f, false).heavy);       // the attack ended: the heavy goes out
    CHECK_FALSE(p.heavyPending());
    for (int i = 0; i < 30; ++i) p.tick(kDt, true, 0.25f, true);
    p.tick(kDt, false, 0.25f, true);
    p.tick(kDt, true, 0.25f, true); // pressed again before the attack ended
    CHECK_FALSE(p.heavyPending());
}

TEST_CASE("aim: the body's turn toward the aim is capped at turnRate and never overshoots") {
    AimRules r;
    CHECK(aimTurnStep(r, 2.f, kDt) == doctest::Approx(r.turnRate * kDt));
    CHECK(aimTurnStep(r, -2.f, kDt) == doctest::Approx(-r.turnRate * kDt));
    CHECK(aimTurnStep(r, 0.01f, kDt) == doctest::Approx(0.01f));
    CHECK(aimTurnStep(r, 1.f, 0.f) == 0.f);
}
