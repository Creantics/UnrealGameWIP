#include "ftcore/AimStance.h"
#include "ftcore/ArenaFight.h"
#include "ftcore/FieldStrike.h"
#include "TestAssets.h"
#include <ostream>
#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 60.f;

ArenaTable shippedArena() {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/arena.json", text)) FAIL("Data/Battle/arena.json not found");
    ArenaTable t;
    std::string err;
    REQUIRE_MESSAGE(importArenaTable(text, t, &err), err);
    return t;
}

AimRules shippedAim() {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/aim.json", text)) FAIL("Data/Battle/aim.json not found");
    AimRules r;
    std::string err;
    REQUIRE_MESSAGE(importAimRules(text, r, &err), err);
    return r;
}

// Ticks until the attack started this frame ends, collecting hits and the distance lunged.
struct Swing {
    int hits = 0;
    float lunged = 0.f;
    Vec2 facing{};
};
Swing swing(FieldStrike& s, bool heavy, Vec2 pos, const FieldTarget* t, std::size_t n) {
    Swing w;
    const ArenaOutput* o = &s.tick(kDt, !heavy, heavy, pos, Vec2{1.f, 0.f}, t, n);
    for (int i = 0; i < 240; ++i) {
        for (const ArenaEvent& e : o->events)
            if (e.type == ArenaEventType::Hit) ++w.hits;
        w.lunged += std::sqrt(o->player.velocity.x * o->player.velocity.x + o->player.velocity.y * o->player.velocity.y) * kDt;
        if (o->player.part == ArenaPart::Startup || o->player.part == ArenaPart::Active) w.facing = o->player.facing;
        if (!s.attacking()) break;
        o = &s.tick(kDt, false, false, pos, Vec2{1.f, 0.f}, t, n);
    }
    return w;
}

} // namespace

TEST_CASE("aim: the shipped rules load and are sane") {
    const AimRules r = shippedAim();
    CHECK(r.blendIn > 0.f);
    CHECK(r.blendOut > 0.f);
    CHECK(r.speedScale < 1.f);
    CHECK(r.armScale < 1.f);
    CHECK(r.heavyHold == doctest::Approx(0.25f));
}

TEST_CASE("aim: the loader refuses unknown keys, wrong types and out-of-range values") {
    AimRules r;
    std::string err;
    CHECK_FALSE(importAimRules("[]", r, &err));
    CHECK_FALSE(importAimRules("{\"blendIn\": \"x\"}", r, &err));
    CHECK(err.find("blendIn") != std::string::npos);
    CHECK_FALSE(importAimRules("{\"speedScale\": 3}", r, &err));
    CHECK_FALSE(importAimRules("{\"zoom\": 1}", r, &err));
    CHECK(err.find("zoom") != std::string::npos);
    AimRules keep;
    keep.fov = 40.f;
    CHECK(importAimRules("{\"version\": 1, \"turnRate\": 9}", keep, &err));
    CHECK(keep.fov == doctest::Approx(40.f));
    CHECK(keep.turnRate == doctest::Approx(9.f));
}

TEST_CASE("aim: the stance blends in over blendIn and out over blendOut") {
    AimStance a;
    AimRules r;
    r.blendIn = 0.2f;
    r.blendOut = 0.4f;
    r.speedScale = 0.5f;
    a.setRules(r);
    CHECK(a.phase() == AimPhase::Off);
    CHECK(a.speedScale() == doctest::Approx(1.f));
    a.tick(0.1f, true, FieldBody::Free);
    CHECK(a.phase() == AimPhase::In);
    CHECK(a.aiming());
    CHECK(a.alpha() == doctest::Approx(0.5f));
    a.tick(0.1f, true, FieldBody::Free);
    CHECK(a.phase() == AimPhase::Held);
    CHECK(a.alpha() == doctest::Approx(1.f));
    CHECK(a.speedScale() == doctest::Approx(0.5f));
    CHECK(a.blend(70.f, 55.f) == doctest::Approx(55.f));
    a.tick(0.2f, false, FieldBody::Free);
    CHECK(a.phase() == AimPhase::Out);
    CHECK_FALSE(a.aiming());
    CHECK(a.alpha() == doctest::Approx(0.5f));
    a.tick(0.2f, false, FieldBody::Free);
    CHECK(a.phase() == AimPhase::Off);
    CHECK(a.alpha() == doctest::Approx(0.f));
}

TEST_CASE("aim: the body's own moves block it; falling keeps it; a roll holds it out until the roll ends") {
    for (FieldBody b : {FieldBody::Seated, FieldBody::Hanging, FieldBody::Traversing, FieldBody::Sliding, FieldBody::Rolling})
        CHECK_FALSE(aimAllowed(b));
    CHECK(aimAllowed(FieldBody::Free));
    CHECK(aimAllowed(FieldBody::Falling));
    AimStance a;
    a.tick(1.f, true, FieldBody::Free);
    REQUIRE(a.phase() == AimPhase::Held);
    a.tick(kDt, true, FieldBody::Falling);
    CHECK(a.aiming());
    a.tick(kDt, true, FieldBody::Seated);
    CHECK_FALSE(a.aiming());
    CHECK(a.phase() == AimPhase::Out);
    a.tick(1.f, true, FieldBody::Rolling);
    CHECK(a.phase() == AimPhase::Off);
    a.tick(kDt, true, FieldBody::Free); // still held after the roll: back in
    CHECK(a.phase() == AimPhase::In);
}

TEST_CASE("aim: what each lead's aimed attack does") {
    const ArenaTable t = shippedArena();
    CHECK(aimKindOf(t.fighter("duy")) == AimKind::Fire);
    CHECK(aimKindOf(t.fighter("third")) == AimKind::Focus); // no weapon of his own
    CHECK(aimKindOf(t.fighter("devin")) == AimKind::Focus);
    ArenaFighter magic;
    magic.weapon = ArenaWeapon::Magic; // no lead uses it; the kind stays for old data
    CHECK(aimKindOf(magic) == AimKind::Cast);
}

TEST_CASE("aim: the attack button is light on a tap and heavy on a hold, once") {
    PressHold p;
    const float hold = 0.25f;
    PressHold::Out o = p.tick(kDt, true, hold);
    CHECK_FALSE(o.light);
    CHECK_FALSE(o.heavy);
    for (int i = 0; i < 5; ++i) CHECK_FALSE(p.tick(kDt, true, hold).heavy);
    o = p.tick(kDt, false, hold);
    CHECK(o.light);
    CHECK_FALSE(o.heavy);
    int heavies = 0;
    int lights = 0;
    for (int i = 0; i < 60; ++i) {
        o = p.tick(kDt, true, hold);
        heavies += o.heavy;
        lights += o.light;
    }
    o = p.tick(kDt, false, hold);
    heavies += o.heavy;
    lights += o.light;
    CHECK(heavies == 1);
    CHECK(lights == 0);
    CHECK_FALSE(p.tick(kDt, false, hold).light); // up and staying up: nothing
    p.tick(kDt, true, hold);
    p.reset();
    CHECK_FALSE(p.tick(kDt, false, hold).light); // a press forgotten gives nothing on its release
}

TEST_CASE("aim: an aimed shot faces the reticle and hits only what lies on its line") {
    const ArenaTable t = shippedArena();
    FieldStrike s;
    s.begin(t.fighter("duy"));
    // Two dummies 6 m out: one straight ahead (+x), one off to the left (+y). The reticle is on the left one.
    const FieldTarget targets[2] = {FieldTarget{Vec2{6.f, 0.f}, 0.4f, true, 1.3f, 1}, FieldTarget{Vec2{0.f, 6.f}, 0.4f, true, 1.3f, 2}};
    s.setReticle(true, Vec2{0.f, 6.f}, 1.4f);
    const Swing w = swing(s, false, Vec2{}, targets, 2);
    CHECK(w.facing.y == doctest::Approx(1.f).epsilon(0.01));
    CHECK(w.hits == 1);
    CHECK(s.aim().aimed);
    CHECK(s.aim().id == 2u);
    CHECK(s.aim().height == doctest::Approx(1.4f));
    // The reticle on empty ground between them: nothing on the line, no hit, the aim still at the point.
    FieldStrike s2;
    s2.begin(t.fighter("duy"));
    s2.setReticle(true, Vec2{4.f, 4.f}, 1.f);
    const Swing w2 = swing(s2, false, Vec2{}, targets, 2);
    CHECK(w2.hits == 0);
    CHECK(s2.aim().aimed);
    CHECK(s2.aim().target == 0xFF);
    CHECK(s2.aim().at.x == doctest::Approx(4.f));
    // No reticle: the old aim, the nearest in reach (a gun's 9 m) in any direction.
    FieldStrike s3;
    s3.begin(t.fighter("duy"));
    const Swing w3 = swing(s3, false, Vec2{}, targets, 2);
    CHECK(w3.hits == 1);
}

TEST_CASE("aim: the focus stance's light lunges toward the reticle and stops short of it") {
    const ArenaTable t = shippedArena();
    const ArenaFighter kit = t.fighter("devin");
    const ArenaAttack* first = arenaPlayerAttack(kit, 0);
    REQUIRE(first);
    FieldStrike s;
    s.begin(kit);
    s.setReticle(true, Vec2{0.f, 5.f}, 1.3f, 2.f, 0.6f);
    const Swing w = swing(s, false, Vec2{}, nullptr, 0);
    CHECK(w.facing.y == doctest::Approx(1.f).epsilon(0.01));
    CHECK(w.lunged == doctest::Approx(first->lunge * 2.f).epsilon(0.05));
    // The point close: the lunge ends 0.6 x range short of it.
    FieldStrike near;
    near.begin(kit);
    const float d = first->range * 0.6f + 0.3f;
    near.setReticle(true, Vec2{d, 0.f}, 1.3f, 2.f, 0.6f);
    CHECK(swing(near, false, Vec2{}, nullptr, 0).lunged == doctest::Approx(0.3f).epsilon(0.05));
    // No reticle: no lunge in the field.
    FieldStrike plain;
    plain.begin(kit);
    CHECK(swing(plain, false, Vec2{}, nullptr, 0).lunged == doctest::Approx(0.f));
}

TEST_CASE("aim: in the arena the aimed player turns to the camera's aim, walks slower and attacks that way, lock or not") {
    const ArenaTable t = shippedArena();
    ArenaDef def = t.fallback;
    def.fighter = t.fighter("duy");
    def.enemyCount = 1;
    ArenaFight fight;
    fight.begin(def, ArenaRing{Vec2{0.f, 0.f}, 30.f}, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
    ArenaBodies bodies;
    bodies.enemies[0] = Vec2{8.f, 0.f}; // ahead, +x
    ArenaInput in;
    in.lock = true; // locked on the enemy ahead
    fight.tick(kDt, in, bodies);
    in.lock = false;
    // Aim to the left (+y): the body turns there although the lock is ahead.
    in.aim = true;
    in.aimDir = Vec2{0.f, 1.f};
    in.aimTurn = 14.f;
    in.aimSpeed = 0.5f;
    const ArenaOutput* out = nullptr;
    for (int i = 0; i < 30; ++i) out = &fight.tick(kDt, in, bodies);
    CHECK(out->player.facing.y == doctest::Approx(1.f).epsilon(0.01));
    // Walking forward (+y) for a second: at most half the kit's speed.
    in.move = Vec2{0.f, 1.f};
    for (int i = 0; i < 60; ++i) out = &fight.tick(kDt, in, bodies);
    const float speed = std::sqrt(out->player.velocity.x * out->player.velocity.x + out->player.velocity.y * out->player.velocity.y);
    CHECK(speed == doctest::Approx(def.fighter.moveSpeed * 0.5f).epsilon(0.02));
    // A light now goes to the aim (+y), not the lock.
    in.move = Vec2{};
    in.light = true;
    out = &fight.tick(kDt, in, bodies);
    in.light = false;
    for (int i = 0; i < 6; ++i) out = &fight.tick(kDt, in, bodies);
    CHECK(out->player.action == ArenaAction::Light);
    CHECK(out->player.facing.y == doctest::Approx(1.f).epsilon(0.01));
    // Let go: the lock takes the facing back.
    in.aim = false;
    for (int i = 0; i < 90; ++i) out = &fight.tick(kDt, in, bodies);
    CHECK(out->player.facing.x > 0.9f);
}
