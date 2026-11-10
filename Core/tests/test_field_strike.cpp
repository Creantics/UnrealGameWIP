#include "ftcore/ArenaFight.h"
#include "ftcore/FieldStrike.h"
#include "TestAssets.h"
#include <ostream>
#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <string>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 60.f;

ArenaTable shipped() {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/arena.json", text)) FAIL("Data/Battle/arena.json not found");
    ArenaTable t;
    std::string err;
    REQUIRE_MESSAGE(importArenaTable(text, t, &err), err);
    return t;
}

struct Run {
    int started = 0;
    int hits = 0;
    int damage = 0;
    std::uint8_t lastMove = 0xFF;
    bool sawActive = false;
};

// Ticks `seconds` with nothing pressed, collecting the events.
void idle(FieldStrike& s, float seconds, Run& r, const FieldTarget* t, std::size_t n, Vec2 pos = {}, Vec2 face = {1.f, 0.f}) {
    for (float x = 0.f; x < seconds; x += kDt) {
        const ArenaOutput& o = s.tick(kDt, false, false, pos, face, t, n);
        if (o.player.part == ArenaPart::Active) r.sawActive = true;
        for (const ArenaEvent& e : o.events) {
            CHECK(e.source == kArenaPlayer);
            if (e.type == ArenaEventType::AttackStarted) {
                ++r.started;
                r.lastMove = e.move;
            }
            if (e.type == ArenaEventType::Hit) {
                ++r.hits;
                r.damage += e.amount;
            }
        }
    }
}

void press(FieldStrike& s, bool heavy, Run& r, const FieldTarget* t, std::size_t n, Vec2 pos = {}, Vec2 face = {1.f, 0.f}) {
    const ArenaOutput& o = s.tick(kDt, !heavy, heavy, pos, face, t, n);
    for (const ArenaEvent& e : o.events) {
        if (e.type == ArenaEventType::AttackStarted) {
            ++r.started;
            r.lastMove = e.move;
        }
        if (e.type == ArenaEventType::Hit) {
            ++r.hits;
            r.damage += e.amount;
        }
    }
}

} // namespace

TEST_CASE("field strike: every lead's light hits a dummy in front, once per attack, with the kit's damage") {
    const ArenaTable table = shipped();
    for (const char* lead : {"devin", "duy", "third"}) {
        CAPTURE(lead);
        const ArenaFighter kit = table.fighter(lead);
        FieldStrike s;
        s.begin(kit);
        const ArenaAttack* first = arenaPlayerAttack(kit, 0);
        REQUIRE(first != nullptr);
        // Just inside reach, straight ahead.
        const FieldTarget dummy{Vec2{first->range * 0.8f, 0.f}, 0.4f, true};
        Run r;
        press(s, false, r, &dummy, 1);
        CHECK(r.started == 1);
        CHECK(r.lastMove == 0);
        CHECK(s.attacking());
        CHECK(s.output().player.action == ArenaAction::Light);
        idle(s, first->total() + 0.1f, r, &dummy, 1);
        CHECK(r.sawActive);
        CHECK(r.hits == 1);
        CHECK(r.damage == first->damage);
        CHECK_FALSE(s.attacking());
        CHECK(s.output().player.action == ArenaAction::Idle);
        CHECK(s.hits() == 1);
        // The heavy too.
        Run h;
        press(s, true, h, &dummy, 1);
        CHECK(h.lastMove == kArenaHeavyMove);
        CHECK(s.output().player.action == ArenaAction::Heavy);
        idle(s, kit.heavy.total() + 0.1f, h, &dummy, 1);
        if (fieldStrikeReaches(kit.heavy, Vec2{}, Vec2{1.f, 0.f}, dummy.at, dummy.radius)) {
            CHECK(h.hits == 1);
            CHECK(h.damage == kit.heavy.damage);
        }
    }
}

TEST_CASE("field strike: a target behind in reach is turned to; nothing out of reach or dead is hit") {
    const ArenaTable table = shipped();
    const ArenaFighter kit = table.fighter("devin");
    FieldStrike s;
    s.begin(kit);
    const FieldTarget targets[3] = {
        {Vec2{-1.f, 0.f}, 0.4f, true},  // behind
        {Vec2{40.f, 0.f}, 0.4f, true},  // far
        {Vec2{1.f, 0.f}, 0.4f, false},  // in front, gone
    };
    Run r;
    press(s, false, r, targets, 3);
    idle(s, 2.f, r, targets, 3);
    CHECK(r.started == 1);
    CHECK(r.hits == 1);
    CHECK(s.aim().target == 0);
    CHECK(s.aim().at.x == doctest::Approx(-0.6f));
    // With only the far and the dead one, nothing.
    FieldStrike s2;
    s2.begin(kit);
    Run r2;
    press(s2, false, r2, targets + 1, 2);
    idle(s2, 2.f, r2, targets + 1, 2);
    CHECK(r2.started == 1);
    CHECK(r2.hits == 0);
}

TEST_CASE("field strike: Duy's shot reaches down its lane, the Third's cast hits at range") {
    const ArenaTable table = shipped();
    const ArenaFighter duy = table.fighter("duy");
    const ArenaAttack* shot = arenaPlayerAttack(duy, 0);
    REQUIRE(shot != nullptr);
    CHECK(shot->range > 5.f);
    FieldStrike s;
    s.begin(duy);
    const FieldTarget far{Vec2{shot->range - 1.f, 0.f}, 0.4f, true};
    Run r;
    press(s, false, r, &far, 1);
    idle(s, shot->total() + 0.1f, r, &far, 1);
    CHECK(r.hits == 1);
    CHECK(arenaPlayerStrike(duy, 0) == FightClip::WeaponRight);

    const ArenaFighter third = table.fighter("third");
    const ArenaAttack* cast = arenaPlayerAttack(third, 0);
    REQUIRE(cast != nullptr);
    FieldStrike m;
    m.begin(third);
    const FieldTarget mid{Vec2{cast->range, 0.f}, 0.4f, true};
    Run c;
    press(m, false, c, &mid, 1);
    idle(m, cast->total() + 0.1f, c, &mid, 1);
    CHECK(c.hits == 1);
    CHECK(arenaPlayerStrike(third, 0) == FightClip::Cast);
}

TEST_CASE("field strike: the light chain goes on when pressed in time, wraps, and starts over after a pause") {
    const ArenaTable table = shipped();
    const ArenaFighter kit = table.fighter("devin");
    REQUIRE(kit.lightCount >= 2);
    FieldStrike s;
    s.begin(kit);
    Run r;
    std::uint8_t moves[8] = {};
    int n = 0;
    for (int i = 0; i < kit.lightCount + 1; ++i) {
        press(s, false, r, nullptr, 0);
        moves[n++] = r.lastMove;
        const ArenaAttack* a = arenaPlayerAttack(kit, r.lastMove);
        REQUIRE(a != nullptr);
        // Into the recovery's cancel point, then the next press.
        idle(s, a->startup + a->active + a->recovery * a->cancel + 0.02f, r, nullptr, 0);
    }
    for (int i = 0; i < kit.lightCount; ++i) CHECK(moves[i] == i);
    CHECK(moves[kit.lightCount] == 0); // wrapped
    idle(s, 3.f, r, nullptr, 0);
    press(s, false, r, nullptr, 0);
    CHECK(r.lastMove == 0); // after a pause: the first again
}

TEST_CASE("field strike: a press during the attack is buffered, not lost, and the facing is held") {
    const ArenaTable table = shipped();
    const ArenaFighter kit = table.fighter("devin");
    FieldStrike s;
    s.begin(kit);
    Run r;
    press(s, false, r, nullptr, 0, Vec2{}, Vec2{0.f, 1.f});
    const ArenaAttack* a = arenaPlayerAttack(kit, 0);
    REQUIRE(a != nullptr);
    // Turning mid-attack doesn't turn the strike.
    idle(s, a->startup * 0.5f, r, nullptr, 0, Vec2{}, Vec2{1.f, 0.f});
    CHECK(s.output().player.facing.y == doctest::Approx(1.f));
    // Pressed just before the cancel point: it starts once it can.
    const float toCancel = a->startup + a->active + a->recovery * a->cancel - s.output().player.actionTime;
    const float early = toCancel - kit.bufferTime * 0.5f;
    if (early > 0.f) idle(s, early, r, nullptr, 0);
    press(s, false, r, nullptr, 0);
    idle(s, kit.bufferTime, r, nullptr, 0);
    CHECK(r.started == 2);
}

TEST_CASE("field strike: maxTargets limits a strike to the nearest ones") {
    ArenaFighter kit;
    kit.light[0].range = 3.f;
    kit.light[0].arcCos = -1.f;
    kit.light[0].maxTargets = 1;
    kit.lightCount = 1;
    FieldStrike s;
    s.begin(kit);
    const FieldTarget t[2] = {{Vec2{2.f, 0.f}, 0.4f, true}, {Vec2{1.f, 0.f}, 0.4f, true}};
    Run r;
    press(s, false, r, t, 2);
    std::uint8_t hitTarget = 0xFF;
    for (float x = 0.f; x < 2.f; x += kDt) {
        for (const ArenaEvent& e : s.tick(kDt, false, false, Vec2{}, Vec2{1.f, 0.f}, t, 2).events)
            if (e.type == ArenaEventType::Hit) {
                ++r.hits;
                hitTarget = e.target;
            }
    }
    CHECK(r.hits == 1);
    CHECK(hitTarget == 1);
}

TEST_CASE("field strike: the lane test and the arc test") {
    ArenaAttack lane;
    lane.range = 10.f;
    lane.width = 0.4f;
    CHECK(fieldStrikeReaches(lane, Vec2{}, Vec2{1.f, 0.f}, Vec2{9.f, 0.3f}, 0.4f));
    CHECK_FALSE(fieldStrikeReaches(lane, Vec2{}, Vec2{1.f, 0.f}, Vec2{9.f, 1.f}, 0.4f));
    CHECK_FALSE(fieldStrikeReaches(lane, Vec2{}, Vec2{1.f, 0.f}, Vec2{-1.f, 0.f}, 0.4f));
    ArenaAttack arc;
    arc.range = 2.f;
    arc.arcCos = 0.5f;
    CHECK(fieldStrikeReaches(arc, Vec2{}, Vec2{1.f, 0.f}, Vec2{1.f, 1.f}, 0.4f));
    CHECK_FALSE(fieldStrikeReaches(arc, Vec2{}, Vec2{1.f, 0.f}, Vec2{0.f, 1.5f}, 0.4f));
    CHECK(fieldStrikeReaches(arc, Vec2{}, Vec2{1.f, 0.f}, Vec2{0.f, 0.f}, 0.4f));
}

TEST_CASE("field strike: an attack turns to the nearest target in reach and aims at its near side, chest high") {
    ArenaFighter kit;
    kit.light[0].range = 2.f;
    kit.light[0].arcCos = 0.7f;
    kit.lightCount = 1;
    FieldStrike s;
    s.begin(kit);
    // One off to the side within reach, one nearer but out of reach (behind a wall of range), one dead and nearest.
    FieldTarget t[3] = {{Vec2{0.f, 1.5f}, 0.4f, true, 1.1f}, {Vec2{-5.f, 0.f}, 0.4f, true, 1.2f},
                        {Vec2{0.5f, 0.f}, 0.4f, false, 1.2f}};
    Run r;
    press(s, false, r, t, 3);
    REQUIRE(r.started == 1);
    const FieldAim& a = s.aim();
    CHECK(a.aimed);
    CHECK(a.target == 0);
    CHECK(a.at.x == doctest::Approx(0.f));
    CHECK(a.at.y == doctest::Approx(1.1f));
    CHECK(a.height == doctest::Approx(1.1f));
    CHECK(s.output().player.facing.x == doctest::Approx(0.f).epsilon(1e-4));
    CHECK(s.output().player.facing.y == doctest::Approx(1.f));
    // Facing +x as the host reported, the side target still takes the blow: the attack turned at it.
    idle(s, 2.f, r, t, 3);
    CHECK(r.hits == 1);
}

TEST_CASE("field strike: no target in reach aims straight ahead at the lead's chest") {
    ArenaFighter kit;
    kit.light[0].range = 1.5f;
    kit.lightCount = 1;
    FieldStrike s;
    s.begin(kit);
    s.setChestHeight(1.4f);
    const FieldTarget far{Vec2{4.f, 0.f}, 0.4f, true, 1.2f};
    Run r;
    press(s, false, r, &far, 1, Vec2{1.f, 1.f}, Vec2{0.f, -2.f});
    REQUIRE(r.started == 1);
    CHECK_FALSE(s.aim().aimed);
    CHECK(s.aim().at.x == doctest::Approx(1.f));
    CHECK(s.aim().at.y == doctest::Approx(-0.5f));
    CHECK(s.aim().height == doctest::Approx(1.4f));
}

TEST_CASE("field strike: the aim follows a target moving during startup and holds once active") {
    ArenaFighter kit;
    kit.light[0].startup = 0.3f;
    kit.light[0].active = 0.1f;
    kit.light[0].range = 3.f;
    kit.lightCount = 1;
    FieldStrike s;
    s.begin(kit);
    FieldTarget t{Vec2{2.f, 0.f}, 0.4f, true, 1.2f};
    Run r;
    press(s, false, r, &t, 1);
    CHECK(s.aim().at.x == doctest::Approx(1.6f));
    t.at = Vec2{2.f, 1.f};
    s.tick(kDt, false, false, Vec2{}, Vec2{1.f, 0.f}, &t, 1);
    const Vec2 moved = s.aim().at;
    CHECK(moved.y > 0.1f);
    while (s.output().player.part != ArenaPart::Active) s.tick(kDt, false, false, Vec2{}, Vec2{1.f, 0.f}, &t, 1);
    const Vec2 held = s.aim().at;
    t.at = Vec2{-2.f, 0.f};
    s.tick(kDt, false, false, Vec2{}, Vec2{1.f, 0.f}, &t, 1);
    CHECK(s.aim().at.x == doctest::Approx(held.x));
    CHECK(s.aim().at.y == doctest::Approx(held.y));
}

TEST_CASE("field strike: the hit point is the near side of the body") {
    const Vec2 p = fieldHitPoint(Vec2{0.f, 0.f}, Vec2{3.f, 4.f}, 0.5f);
    CHECK(p.x == doctest::Approx(2.7f));
    CHECK(p.y == doctest::Approx(3.6f));
    const Vec2 inside = fieldHitPoint(Vec2{3.1f, 4.f}, Vec2{3.f, 4.f}, 0.5f);
    CHECK(inside.x == doctest::Approx(3.f));
}

namespace {

// A light chain of three and a short heavy, all round, 3 m reach.
ArenaFighter roundKit() {
    ArenaFighter kit;
    for (ArenaAttack& a : kit.light) {
        a.range = 3.f;
        a.arcCos = -1.f;
    }
    kit.lightCount = 3;
    kit.heavy.range = 3.f;
    kit.heavy.arcCos = -1.f;
    kit.heavy.startup = 0.05f;
    kit.heavy.active = 0.05f;
    kit.heavy.recovery = 0.1f;
    return kit;
}

// Ticks dt once and counts the hits, keeping the last hit's target index.
int tickHits(FieldStrike& s, float dt, const FieldTarget* t, std::size_t n, std::uint8_t* last = nullptr) {
    int hits = 0;
    for (const ArenaEvent& e : s.tick(dt, false, false, Vec2{}, Vec2{1.f, 0.f}, t, n).events)
        if (e.type == ArenaEventType::Hit) {
            ++hits;
            if (last) *last = e.target;
        }
    return hits;
}

constexpr float kChest = kFieldChestHeight;

} // namespace

TEST_CASE("field strike: an attack press during each traversal state is rejected and not kept") {
    const ArenaFighter kit = roundKit();
    const FieldTarget dummy{Vec2{1.f, 0.f}, 0.4f, true};
    for (FieldBody body : {FieldBody::Traversing, FieldBody::Sliding, FieldBody::Rolling, FieldBody::Hanging}) {
        CAPTURE(static_cast<int>(body));
        FieldStrike s;
        s.begin(kit);
        s.setBody(body);
        CHECK_FALSE(s.canAttack());
        Run r;
        press(s, false, r, &dummy, 1);
        press(s, true, r, &dummy, 1);
        idle(s, 0.05f, r, &dummy, 1);
        CHECK(r.started == 0);
        CHECK_FALSE(s.attacking());
        CHECK(s.output().player.action == ArenaAction::Idle);
        // Free again within the buffer time: the dropped press does not come back.
        s.setBody(FieldBody::Free);
        idle(s, kit.bufferTime + 0.5f, r, &dummy, 1);
        CHECK(r.started == 0);
        CHECK(r.hits == 0);
    }
}

TEST_CASE("field strike: a traversal starting mid-attack cancels it, with no hit after") {
    const ArenaFighter kit = roundKit();
    const FieldTarget dummy{Vec2{1.f, 0.f}, 0.4f, true};
    FieldStrike s;
    s.begin(kit);
    Run r;
    press(s, false, r, &dummy, 1);
    idle(s, kit.light[0].startup * 0.5f, r, &dummy, 1);
    REQUIRE(s.attacking());
    s.setBody(FieldBody::Traversing);
    idle(s, kDt, r, &dummy, 1);
    CHECK_FALSE(s.attacking());
    CHECK(s.output().player.action == ArenaAction::Idle);
    s.setBody(FieldBody::Free);
    idle(s, 1.f, r, &dummy, 1);
    CHECK(r.started == 1);
    CHECK(r.hits == 0);
    // The cut ended the chain: the next light is the first.
    press(s, false, r, &dummy, 1);
    CHECK(r.lastMove == 0);
}

TEST_CASE("field strike: a long frame that passes over the active part still hits") {
    const ArenaFighter kit = roundKit();
    const FieldTarget dummy{Vec2{1.f, 0.f}, 0.4f, true};
    FieldStrike s;
    s.begin(kit);
    Run r;
    press(s, false, r, &dummy, 1);
    // One frame from early startup past the end of the whole attack.
    CHECK(tickHits(s, 2.f, &dummy, 1) == 1);
    CHECK_FALSE(s.attacking());
    CHECK(s.hits() == 1);
}

TEST_CASE("field strike: a target with non-finite numbers is skipped") {
    const ArenaFighter kit = roundKit();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const FieldTarget t[4] = {{Vec2{nan, 0.f}, 0.4f, true}, {Vec2{0.5f, 0.f}, nan, true},
                              {Vec2{0.6f, 0.f}, 0.4f, true, inf}, {Vec2{1.f, 0.f}, 0.4f, true}};
    FieldStrike s;
    s.begin(kit);
    Run r;
    press(s, false, r, t, 4);
    CHECK(s.aim().aimed);
    CHECK(s.aim().target == 3);
    CHECK(std::isfinite(s.aim().at.x));
    CHECK(std::isfinite(s.aim().height));
    std::uint8_t last = 0xFF;
    int hits = 0;
    for (float x = 0.f; x < 1.f; x += kDt) hits += tickHits(s, kDt, t, 4, &last);
    CHECK(hits == 1);
    CHECK(last == 3);
}

TEST_CASE("field strike: hits are keyed by the target id, so the list may change order mid-attack") {
    ArenaFighter kit = roundKit();
    kit.light[0].active = 0.3f;
    FieldStrike s;
    s.begin(kit);
    const FieldTarget t[2] = {{Vec2{1.f, 0.f}, 0.4f, true, kChest, 7}, {Vec2{10.f, 0.f}, 0.4f, true, kChest, 9}};
    Run r;
    press(s, false, r, t, 2);
    int hits = 0;
    std::uint8_t last = 0xFF;
    while (hits == 0 && s.attacking()) hits += tickHits(s, kDt, t, 2, &last);
    REQUIRE(hits == 1);
    CHECK(last == 0);
    // Still active: the list comes back the other way round, the second target now in reach.
    const FieldTarget swapped[2] = {{Vec2{0.f, 1.5f}, 0.4f, true, kChest, 9}, t[0]};
    hits += tickHits(s, kDt, swapped, 2, &last);
    CHECK(hits == 2);
    CHECK(last == 0); // the id-9 target, now first in the list; id 7 is not hit twice
    for (float x = 0.f; x < 1.f; x += kDt) hits += tickHits(s, kDt, swapped, 2);
    CHECK(hits == 2);
}

TEST_CASE("field strike: a heavy ends the light chain") {
    const ArenaFighter kit = roundKit();
    FieldStrike s;
    s.begin(kit);
    Run r;
    const ArenaAttack& l = kit.light[0];
    const ArenaAttack& h = kit.heavy;
    REQUIRE(h.total() < kit.chainGap);
    press(s, false, r, nullptr, 0);
    CHECK(r.lastMove == 0);
    idle(s, l.startup + l.active + l.recovery * l.cancel + 0.02f, r, nullptr, 0);
    press(s, true, r, nullptr, 0);
    CHECK(r.lastMove == kArenaHeavyMove);
    // The heavy is short: the next light comes well inside chainGap of the last light.
    idle(s, h.startup + h.active + h.recovery * h.cancel + 0.02f, r, nullptr, 0);
    press(s, false, r, nullptr, 0);
    CHECK(r.started == 3);
    CHECK(r.lastMove == 0);
}

TEST_CASE("field strike: of more than eight targets the nearest eight count") {
    const ArenaFighter kit = roundKit();
    FieldTarget t[12];
    // The first four in the list are the farthest (still in reach).
    for (int i = 0; i < 12; ++i) {
        const float d = i < 4 ? 2.8f : 1.f + 0.05f * static_cast<float>(i);
        const float ang = 0.5f * static_cast<float>(i);
        t[i] = FieldTarget{Vec2{d * std::cos(ang), d * std::sin(ang)}, 0.1f, true};
    }
    FieldStrike s;
    s.begin(kit);
    Run r;
    press(s, false, r, t, 12);
    CHECK(s.aim().target == 4); // the nearest
    bool hit[12] = {};
    int hits = 0;
    for (float x = 0.f; x < 1.f; x += kDt)
        for (const ArenaEvent& e : s.tick(kDt, false, false, Vec2{}, Vec2{1.f, 0.f}, t, 12).events)
            if (e.type == ArenaEventType::Hit) {
                ++hits;
                REQUIRE(e.target < 12);
                hit[e.target] = true;
            }
    CHECK(hits == 8);
    for (int i = 0; i < 4; ++i) CHECK_FALSE(hit[i]);
    for (int i = 4; i < 12; ++i) CHECK(hit[i]);
}

TEST_CASE("field strike: empty or null target lists, and a target leaving mid-attack") {
    const ArenaFighter kit = roundKit();
    const FieldTarget dummy{Vec2{1.f, 0.f}, 0.4f, true};
    SUBCASE("null with a count") {
        FieldStrike s;
        s.begin(kit);
        Run r;
        press(s, false, r, nullptr, 5);
        idle(s, 1.f, r, nullptr, 5);
        CHECK(r.started == 1);
        CHECK(r.hits == 0);
        CHECK_FALSE(s.aim().aimed);
    }
    SUBCASE("empty") {
        FieldStrike s;
        s.begin(kit);
        Run r;
        press(s, false, r, &dummy, 0);
        idle(s, 1.f, r, &dummy, 0);
        CHECK(r.started == 1);
        CHECK(r.hits == 0);
        CHECK_FALSE(s.aim().aimed);
    }
    SUBCASE("leaves during startup") {
        FieldStrike s;
        s.begin(kit);
        Run r;
        const FieldTarget two[2] = {{Vec2{2.f, 0.f}, 0.4f, true, kChest, 1}, {Vec2{1.f, 0.f}, 0.4f, true, kChest, 2}};
        press(s, false, r, two, 2);
        CHECK(s.aim().id == 2);
        // The aimed one is gone from the list; the other is still in reach and is turned to.
        std::uint8_t last = 0xFF;
        int hits = 0;
        for (float x = 0.f; x < 1.f; x += kDt) hits += tickHits(s, kDt, two, 1, &last);
        CHECK(hits == 1);
        CHECK(last == 0);
    }
    SUBCASE("leaves during active") {
        ArenaFighter k = kit;
        k.light[0].active = 0.3f;
        FieldStrike s;
        s.begin(k);
        Run r;
        const FieldTarget two[2] = {{Vec2{1.f, 0.f}, 0.4f, true, kChest, 1}, {Vec2{20.f, 0.f}, 0.4f, true, kChest, 2}};
        press(s, false, r, two, 2);
        int hits = 0;
        while (hits == 0 && s.attacking()) hits += tickHits(s, kDt, two, 2);
        REQUIRE(hits == 1);
        for (float x = 0.f; x < 1.f; x += kDt) hits += tickHits(s, kDt, nullptr, 0);
        CHECK(hits == 1);
        CHECK_FALSE(s.attacking());
    }
}
