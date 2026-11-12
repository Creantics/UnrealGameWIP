#include "ftcore/ArenaImpact.h"
#include "AllocCounter.h"
#include "TestAssets.h"
#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 60.f;

ArenaAttack swing(float startup, float active, float recovery, float range, int damage, float poise = 0.f) {
    ArenaAttack a;
    a.startup = startup;
    a.active = active;
    a.recovery = recovery;
    a.range = range;
    a.arcCos = -1.f;
    a.damage = damage;
    a.poiseDamage = poise;
    a.lunge = 0.f;
    a.tracking = 0.f;
    a.stamina = 0.f;
    a.cancel = 0.5f;
    return a;
}

ArenaFighter kit() {
    ArenaFighter f;
    f.hp = 1000;
    f.stamina = 100.f;
    f.poise = 1000.f;
    f.bodyRadius = 0.4f;
    f.lightCount = 1;
    f.light[0] = swing(0.1f, 0.1f, 0.2f, 2.f, 10, 10.f);
    f.heavy = swing(0.3f, 0.1f, 0.3f, 2.f, 30, 0.f);
    f.riposte = swing(0.1f, 0.1f, 0.4f, 2.5f, 50, 0.f);
    f.release = swing(0.1f, 0.1f, 0.4f, 2.5f, 20, 0.f);
    f.parryWindow = 0.2f;
    f.riposteReach = 2.2f;
    f.guardArcCos = 0.f;
    return f;
}

ArenaEnemyKind dummy(int hp = 100, float poise = 15.f) {
    ArenaEnemyKind k;
    k.hp = hp;
    k.poise = poise;
    k.poiseDelay = 10.f;
    k.staggerTime = 1.f;
    k.openTime = 1.5f;
    k.downTime = 1.f;
    k.attackDelay = 1000.f;
    k.bodyRadius = 0.45f;
    k.flinchPoise = 0.f;
    return k;
}

ArenaEnemyKind biter(const ArenaAttack& bite) {
    ArenaEnemyKind k = dummy();
    k.attackDelay = 0.f;
    k.moveCount = 1;
    k.moves[0].attack = bite;
    k.moves[0].maxRange = 3.f;
    k.moves[0].parryable = true;
    return k;
}

struct Rig {
    ArenaDef def;
    ArenaFight fight;
    ArenaBodies bodies;
    ArenaInput in;

    Rig(const ArenaFighter& f, const ArenaEnemyKind& k) {
        def.fighter = f;
        def.enemies[0] = k;
        def.enemyCount = 1;
        def.attackers = 1;
        fight.begin(def, ArenaRing{Vec2{0.f, 0.f}, 30.f}, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
        bodies.enemies[0] = Vec2{1.5f, 0.f};
    }
    void tick() {
        fight.tick(kDt, in, bodies);
        in.light = in.heavy = in.dodge = in.heal = in.lock = in.release = in.pair = false;
    }
    void run(int n) {
        for (int i = 0; i < n; ++i) tick();
    }
    ArenaBrain brain() const { return fight.output().enemies[0].brain; }
    // A light, then the ticks until it is over (0.4 s).
    void light() {
        in.light = true;
        run(26);
    }
};

ArenaEvent event(ArenaEventType type, std::uint8_t source, std::uint8_t target, std::uint8_t move) {
    ArenaEvent e;
    e.type = type;
    e.source = source;
    e.target = target;
    e.move = move;
    return e;
}

ArenaTellBody winding(float x, float y, float fill = 0.5f) {
    ArenaTellBody b;
    b.pos = Vec2{x, y};
    b.winding = true;
    b.fill = fill;
    return b;
}

} // namespace

// ---- the jolt ----

TEST_CASE("arena jolt: a hit by the player takes its move's jolt; a hit on the player its move's, else hurtJolt; blocked, half") {
    ArenaDef def;
    def.fighter = kit();
    def.fighter.light[0].jolt = 0.5f;
    def.fighter.heavy.jolt = 1.4f;
    def.fighter.riposte.jolt = 2.f;
    def.fighter.hurtJolt = 1.f;
    def.enemyCount = 2;
    def.enemies[0] = biter(swing(0.4f, 0.1f, 0.4f, 2.f, 10));
    def.enemies[0].moves[0].attack.jolt = 2.5f;
    def.enemies[1] = biter(swing(0.4f, 0.1f, 0.4f, 2.f, 10)); // its move names no jolt

    CHECK(arenaJoltOf(def, event(ArenaEventType::Hit, kArenaPlayer, 0, 0)) == doctest::Approx(0.5f));
    CHECK(arenaJoltOf(def, event(ArenaEventType::Hit, kArenaPlayer, 1, kArenaHeavyMove)) == doctest::Approx(1.4f));
    CHECK(arenaJoltOf(def, event(ArenaEventType::Hit, kArenaPlayer, 0, kArenaRiposteMove)) == doctest::Approx(2.f));
    CHECK(arenaJoltOf(def, event(ArenaEventType::Hit, 0, kArenaPlayer, 0)) == doctest::Approx(2.5f));
    CHECK(arenaJoltOf(def, event(ArenaEventType::Hit, 1, kArenaPlayer, 0)) == doctest::Approx(1.f));
    CHECK(arenaJoltOf(def, event(ArenaEventType::Blocked, 0, kArenaPlayer, 0)) == doctest::Approx(1.25f));
    CHECK(arenaJoltOf(def, event(ArenaEventType::Blocked, 1, kArenaPlayer, 0)) == doctest::Approx(0.5f));
    // Nothing else jolts, and an unknown move or enemy is 0.
    CHECK(arenaJoltOf(def, event(ArenaEventType::Parried, 0, kArenaPlayer, 0)) == 0.f);
    CHECK(arenaJoltOf(def, event(ArenaEventType::Staggered, kArenaPlayer, 0, 0)) == 0.f);
    CHECK(arenaJoltOf(def, event(ArenaEventType::Hit, kArenaPlayer, 0, kArenaShotMove)) == 0.f); // not a guns kit
    CHECK(arenaJoltOf(def, event(ArenaEventType::Hit, 0, kArenaPlayer, 7)) == 0.f);
    CHECK(arenaJoltOf(def, event(ArenaEventType::Hit, 4, kArenaPlayer, 0)) == 0.f);
    CHECK(arenaJoltOf(def, event(ArenaEventType::HostHit, kArenaPlayer, 0, kArenaHeavyMove)) == 0.f); // a bonus, no blow
}

TEST_CASE("arena jolt: scale 0 is no jolt at all, and the kick is bounded, settles to 0, and never grows") {
    SUBCASE("scale 0, or NaN") {
        ArenaJolt j;
        j.kick(3.f, 0.f, 1.f);
        CHECK_FALSE(j.active());
        CHECK(j.pitch() == 0.f);
        CHECK(j.yaw() == 0.f);
        j.kick(3.f, std::numeric_limits<float>::quiet_NaN(), 1.f);
        j.kick(std::numeric_limits<float>::quiet_NaN(), 1.f, 1.f);
        CHECK_FALSE(j.active());
        for (int i = 0; i < 30; ++i) {
            j.step(kDt);
            CHECK(j.pitch() == 0.f);
            CHECK(j.yaw() == 0.f);
        }
    }
    SUBCASE("a full kick: dips first, stays within its degrees, settles inside kArenaJoltLength") {
        ArenaJolt j;
        j.kick(10.f, 1.f, 1.f); // clamped to kArenaMaxJolt
        CHECK(j.left() == doctest::Approx(kArenaMaxJolt));
        CHECK(j.pitch() == doctest::Approx(-kArenaMaxJolt)); // the view dips at the kick
        float lastLeft = j.left();
        int steps = 0;
        while (j.active() && steps < 100) {
            CHECK(std::fabs(j.pitch()) <= kArenaMaxJolt + 1e-4f);
            CHECK(std::fabs(j.yaw()) <= kArenaJoltYawShare * kArenaMaxJolt + 1e-4f);
            j.step(kDt);
            CHECK(j.left() <= lastLeft);
            lastLeft = j.left();
            ++steps;
        }
        CHECK_FALSE(j.active());
        CHECK(static_cast<float>(steps) * kDt <= kArenaJoltLength + kDt);
        CHECK(j.pitch() == 0.f);
        CHECK(j.yaw() == 0.f);
    }
    SUBCASE("half the setting is half the kick; the side sets the yaw's first swing") {
        ArenaJolt full, half, left;
        full.kick(2.f, 1.f, 1.f);
        half.kick(2.f, 0.5f, 1.f);
        left.kick(2.f, 1.f, -1.f);
        for (int i = 0; i < 4; ++i) {
            full.step(kDt);
            half.step(kDt);
            left.step(kDt);
        }
        CHECK(half.pitch() == doctest::Approx(full.pitch() * 0.5f));
        CHECK(full.yaw() > 0.f);
        CHECK(left.yaw() == doctest::Approx(-full.yaw()));
    }
    SUBCASE("a weaker kick while one runs is ignored; a stronger one replaces it") {
        ArenaJolt j;
        j.kick(2.f, 1.f, 1.f);
        j.step(0.05f);
        const float left = j.left();
        j.kick(0.5f, 1.f, -1.f); // three targets of one blow: the same kick, once
        CHECK(j.left() == doctest::Approx(left));
        CHECK(j.side == 1.f);
        j.kick(2.5f, 1.f, -1.f);
        CHECK(j.left() == doctest::Approx(2.5f));
        CHECK(j.side == -1.f);
    }
    SUBCASE("a bad step changes nothing") {
        ArenaJolt j;
        j.kick(1.f, 1.f, 1.f);
        j.step(-1.f);
        j.step(std::numeric_limits<float>::quiet_NaN());
        CHECK(j.left() == doctest::Approx(1.f));
    }
}

// ---- off-frame tells ----

TEST_CASE("arena off-frame tells: bearings from the camera, + right, and only the winding bodies out of the view") {
    CHECK(arenaBearing({}, Vec2{1.f, 0.f}, Vec2{5.f, 0.f}) == doctest::Approx(0.f));
    CHECK(arenaBearing({}, Vec2{1.f, 0.f}, Vec2{0.f, 5.f}) == doctest::Approx(90.f)); // y is right of x
    CHECK(arenaBearing({}, Vec2{1.f, 0.f}, Vec2{0.f, -5.f}) == doctest::Approx(-90.f));
    CHECK(std::fabs(arenaBearing({}, Vec2{1.f, 0.f}, Vec2{-5.f, 0.f})) == doctest::Approx(180.f));
    CHECK(arenaBearing(Vec2{2.f, 2.f}, Vec2{1.f, 0.f}, Vec2{2.f, 2.f}) == 0.f); // at the eye: in frame

    ArenaView view;
    view.eye = Vec2{-4.f, 0.f}; // the camera stands behind the player at the origin
    view.forward = Vec2{1.f, 0.f};
    view.halfAngle = 45.f;
    view.margin = 5.f;
    std::array<ArenaTellBody, 5> bodies{
        winding(5.f, 0.f),              // 0: straight ahead
        winding(-3.f, 4.f, 0.2f),       // 1: right, beside the camera (out)
        winding(-1.f, -4.f, 2.f),       // 2: left, about 53 degrees (out), fill clamped
        winding(0.f, -3.5f),            // 3: left, about 41 degrees: inside the edge's margin (out)
        ArenaTellBody{Vec2{-3.f, 4.f}}, // 4: out of the view but not winding
    };
    const ArenaOffFrameTells t = arenaOffFrameTells(view, bodies.data(), bodies.size());
    REQUIRE(t.count == 3);
    CHECK(t.items[0].enemy == 1);
    CHECK(t.items[0].bearing > 45.f);
    CHECK(t.items[0].fill == doctest::Approx(0.2f));
    CHECK(t.items[1].enemy == 2);
    CHECK(t.items[1].bearing < -45.f);
    CHECK(t.items[1].fill == 1.f);
    CHECK(t.items[2].enemy == 3);
    CHECK(t.items[2].bearing == doctest::Approx(-41.19f).epsilon(0.01));

    SUBCASE("the cone's apex is the camera, not the player: a body behind the player can be in frame") {
        ArenaView v = view;
        v.margin = 0.f;
        const std::array<ArenaTellBody, 1> behindPlayer{winding(-1.f, 0.5f)};
        CHECK(arenaOffFrameTells(v, behindPlayer.data(), 1).count == 0);
    }
    SUBCASE("a wider view (the aim's field of view is the host's to pass) keeps more in frame") {
        ArenaView wide = view;
        wide.halfAngle = 60.f;
        CHECK(arenaOffFrameTells(wide, bodies.data(), bodies.size()).count == 1); // only the one beside the camera
    }
    SUBCASE("a margin as wide as the view, or a NaN view: only a body dead ahead stays in frame") {
        ArenaView v = view;
        v.margin = 50.f;
        CHECK(arenaOffFrameTells(v, bodies.data(), bodies.size()).count == 3);
        v.margin = 0.f;
        v.halfAngle = std::numeric_limits<float>::quiet_NaN();
        CHECK(arenaOffFrameTells(v, bodies.data(), bodies.size()).count == 3);
    }
    SUBCASE("no bodies, a null list, more than kMaxArenaEnemies") {
        CHECK(arenaOffFrameTells(view, nullptr, 3).count == 0);
        std::array<ArenaTellBody, kMaxArenaEnemies + 2> many{};
        for (ArenaTellBody& b : many) b = winding(-4.f, 4.f);
        CHECK(arenaOffFrameTells(view, many.data(), many.size()).count == kMaxArenaEnemies);
    }
}

TEST_CASE("arena off-frame tells: arenaTellBody winds only through an attack's startup, filling toward the hit") {
    Rig rig(kit(), biter(swing(0.4f, 0.1f, 0.4f, 2.f, 10)));
    CHECK_FALSE(arenaTellBody(rig.fight, 0, Vec2{}).winding);
    float lastFill = -1.f;
    bool wound = false, ended = false;
    for (int i = 0; i < 60 && !ended; ++i) {
        rig.tick();
        const ArenaTellBody b = arenaTellBody(rig.fight, 0, Vec2{1.f, 2.f});
        CHECK(b.pos.y == 2.f);
        if (b.winding) {
            wound = true;
            CHECK(b.fill >= lastFill);
            CHECK(b.fill <= 1.f);
            lastFill = b.fill;
        } else if (wound) {
            ended = true;
            CHECK(rig.fight.output().enemies[0].part != ArenaPart::Startup);
        }
    }
    CHECK(wound);
    CHECK(ended);
    CHECK(lastFill > 0.9f);
    CHECK_FALSE(arenaTellBody(rig.fight, 3, Vec2{}).winding); // no such enemy
}

TEST_CASE("arena off-frame tells and the jolt allocate nothing") {
    REQUIRE(ftcore_test::allocHookInstalled());
    ArenaView view;
    std::array<ArenaTellBody, 3> bodies{winding(-1.f, 4.f), winding(5.f, 0.f), winding(0.f, -5.f)};
    ArenaJolt j;
    const std::uint64_t before = ftcore_test::allocStats().count;
    for (int i = 0; i < 100; ++i) {
        const ArenaOffFrameTells t = arenaOffFrameTells(view, bodies.data(), bodies.size());
        j.kick(static_cast<float>(t.count), 1.f, 1.f);
        j.step(kDt);
    }
    char buf[128];
    arenaPaceText(ArenaStats{}, 3, buf, sizeof buf);
    CHECK(ftcore_test::allocStats().count == before);
}

// ---- data ----

TEST_CASE("arena table: jolt and hurtJolt are read, checked against kArenaMaxJolt, and default to none") {
    const std::string base = R"({ "version": 1, "fighters": [ { "id": "default", "hurtJolt": 0.8,
        "light": [ { "jolt": 0.4 } ], "heavy": { "jolt": 1.2 } } ],
        "enemies": [ { "id": "dummy", "moves": [ { "startup": 0.4, "jolt": 2.5 } ] } ],
        "default": { "enemies": ["dummy"] }, "fights": [] })";
    ArenaTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importArenaTable(base, t, &error, &warnings), error);
    CHECK(warnings.empty());
    CHECK(t.fighters[0].hurtJolt == doctest::Approx(0.8f));
    CHECK(t.fighters[0].light[0].jolt == doctest::Approx(0.4f));
    CHECK(t.fighters[0].heavy.jolt == doctest::Approx(1.2f));
    CHECK(t.fighters[0].riposte.jolt == 0.f);
    CHECK(t.enemies[0].moves[0].attack.jolt == doctest::Approx(2.5f));

    const std::string tooHard = R"({ "version": 1, "fighters": [ { "id": "default", "heavy": { "jolt": 3.5 } } ],
        "enemies": [ { "id": "dummy" } ], "default": { "enemies": ["dummy"] }, "fights": [] })";
    ArenaTable bad;
    CHECK_FALSE(importArenaTable(tooHard, bad, &error));
    CHECK(error.find("heavy.jolt") != std::string::npos);
    const std::string negative = R"({ "version": 1, "fighters": [ { "id": "default", "hurtJolt": -1 } ],
        "enemies": [ { "id": "dummy" } ], "default": { "enemies": ["dummy"] }, "fights": [] })";
    CHECK_FALSE(importArenaTable(negative, bad, &error));
    CHECK(error.find("hurtJolt") != std::string::npos);
}

TEST_CASE("arena data: the shipped kits jolt, heavier blows harder, and every lead inherits a hurt jolt") {
    ArenaTable t;
    std::string text, error;
    if (!ftcore_test::readProjectData("Battle/arena.json", text)) FAIL("Data/Battle/arena.json not found");
    REQUIRE_MESSAGE(importArenaTable(text, t, &error), error);
    for (const char* lead : {"default", "devin", "duy"}) {
        CAPTURE(lead);
        const ArenaFighter& f = t.fighter(lead);
        CHECK(f.hurtJolt > 0.f);
        CHECK(f.light[0].jolt > 0.f);
        CHECK(f.heavy.jolt > f.light[0].jolt);
        CHECK(f.riposte.jolt >= f.heavy.jolt);
        CHECK(f.release.jolt > 0.f);
        for (std::size_t i = 0; i < f.lightCount; ++i) CHECK(f.light[i].jolt <= f.heavy.jolt);
    }
    const ArenaFighter& duy = t.fighter("duy");
    CHECK(duy.shot.jolt > 0.f);
    CHECK(duy.shot.jolt < duy.light[0].jolt); // a round kicks less than a blow
    CHECK(t.fighter("third").hurtJolt > 0.f);
}

// ---- pace ----

TEST_CASE("arena pace: a stagger opens one punish window, a blow in it uses it, and a kill is timed from the first blow") {
    Rig rig(kit(), dummy(100, 15.f));
    rig.light();
    CHECK(rig.fight.stats().hits == 1);
    CHECK(rig.fight.stats().punishWindows == 0);
    rig.light(); // 20 poise of 15: it staggers
    REQUIRE(rig.brain() == ArenaBrain::Staggered);
    CHECK(rig.fight.stats().punishWindows == 1);
    CHECK(rig.fight.stats().punishUsed == 0);
    rig.light(); // lands while it staggers
    CHECK(rig.fight.stats().punishWindows == 1);
    CHECK(rig.fight.stats().punishUsed == 1);
    rig.light(); // a second blow in the same window counts once
    CHECK(rig.fight.stats().punishUsed == 1);
    rig.run(90); // it gets up: the window closes
    REQUIRE(rig.brain() != ArenaBrain::Staggered);
    CHECK(rig.fight.stats().kills == 0);
    CHECK(rig.fight.stats().killCount[0] == 0);
    for (int i = 0; i < 20 && rig.fight.enemyAlive(0); ++i) rig.light();
    REQUIRE_FALSE(rig.fight.enemyAlive(0));
    const ArenaStats& s = rig.fight.stats();
    CHECK(s.kills == 1);
    CHECK(s.killCount[0] == 1);
    // Timed from the first light's hit, about 0.1 s into the fight: within a step of the fight's time less that.
    CHECK(s.killSeconds[0] > rig.fight.time() - 0.6f - 0.15f);
    CHECK(s.killSeconds[0] < rig.fight.time() - 0.05f);
    CHECK(s.punishUsed <= s.punishWindows);
    char line[160];
    const std::size_t n = arenaPaceText(s, 1, line, sizeof line);
    CHECK(n == std::string(line).size());
    CHECK(std::string(line).rfind("pace: kills 1, ttk 0 ", 0) == 0);
    CHECK(std::string(line).find("windows used") != std::string::npos);
}

TEST_CASE("arena pace: a parry's Open turning Down by the riposte is one window, used") {
    Rig rig(kit(), biter(swing(0.4f, 0.15f, 0.5f, 2.f, 20)));
    rig.run(18);
    rig.in.guard = true;
    rig.run(10);
    rig.in.guard = false;
    rig.tick();
    REQUIRE(rig.brain() == ArenaBrain::Open);
    CHECK(rig.fight.stats().punishWindows == 1);
    rig.in.light = true; // the riposte
    rig.run(40);
    CHECK(rig.fight.stats().ripostes == 1);
    REQUIRE(rig.brain() == ArenaBrain::Down);
    CHECK(rig.fight.stats().punishWindows == 1);
    CHECK(rig.fight.stats().punishUsed == 1);
}

TEST_CASE("arena pace: the counters sum over a restart, and a kill with no blow of the player's adds no time") {
    Rig rig(kit(), dummy(10, 1000.f));
    rig.run(60); // a second before the first blow is not counted
    rig.light(); // 10 hp: one light kills
    REQUIRE_FALSE(rig.fight.enemyAlive(0));
    const float first = rig.fight.stats().killSeconds[0];
    CHECK(rig.fight.stats().killCount[0] == 1);
    CHECK(first < 0.1f); // the light's blow is its first and its last: not timed from the fight's start (1.1 s)
    rig.fight.restart();
    rig.run(30); // a pause before the first blow of the second fight is not counted
    rig.light();
    REQUIRE_FALSE(rig.fight.enemyAlive(0));
    CHECK(rig.fight.stats().kills == 2);
    CHECK(rig.fight.stats().killCount[0] == 2);
    CHECK(rig.fight.stats().killSeconds[0] == doctest::Approx(first * 2.f).epsilon(0.05));

    ArenaStats s;
    s.kills = 1; // killed by something that is not a blow of the player's side: no time
    char line[96];
    arenaPaceText(s, 2, line, sizeof line);
    CHECK(std::string(line) == "pace: kills 1, ttk 0 n/a, 1 n/a; punish 0/0 windows used");
}

TEST_CASE("arena pace: a host's blow is a blow of the player's side: it times the kill and uses a window") {
    Rig rig(kit(), dummy(40, 15.f));
    rig.run(30);
    REQUIRE(rig.fight.hostBlow(0, 10, 0)); // outside a tick: the fight's clock, 0.5 s
    rig.run(30);
    rig.light(); // 10 hp and 10 poise: still standing
    rig.light(); // 20 poise of 15: it staggers
    REQUIRE(rig.brain() == ArenaBrain::Staggered);
    CHECK(rig.fight.stats().punishWindows == 1);
    CHECK(rig.fight.stats().punishUsed == 0);
    REQUIRE(rig.fight.hostBlow(0, 5, 0)); // lands in the window; 5 hp left (40 - 10 - 20 - 5)
    CHECK(rig.fight.stats().punishUsed == 1);
    CHECK(rig.fight.enemyAlive(0));
    rig.run(1);
    const float at = rig.fight.time();
    REQUIRE(rig.fight.hostBlow(0, 50, 0)); // the kill
    rig.run(1);
    REQUIRE_FALSE(rig.fight.enemyAlive(0));
    const ArenaStats& s = rig.fight.stats();
    CHECK(s.kills == 1);
    CHECK(s.killCount[0] == 1);
    CHECK(s.killSeconds[0] == doctest::Approx(at - 0.5f).epsilon(0.02)); // from the first host blow, not the first light
}

TEST_CASE("arena pace: the line reads n/a with nothing to time, and always fits its buffer") {
    char line[64];
    CHECK(arenaPaceText(ArenaStats{}, 3, line, sizeof line) == 9);
    CHECK(std::string(line) == "pace: n/a");
    ArenaStats s;
    s.kills = 3;
    s.punishWindows = 6;
    s.punishUsed = 4;
    for (std::size_t i = 0; i < kMaxArenaEnemies; ++i) {
        s.killSeconds[i] = 9.4f * static_cast<float>(i + 1);
        s.killCount[i] = 1;
    }
    char small[24];
    const std::size_t n = arenaPaceText(s, kMaxArenaEnemies, small, sizeof small);
    CHECK(n == sizeof small - 1);
    CHECK(std::string(small).size() == n);
    char big[256];
    arenaPaceText(s, 2, big, sizeof big);
    CHECK(std::string(big) == "pace: kills 3, ttk 0 9.4 s, 1 18.8 s; punish 4/6 windows used");
    CHECK(arenaPaceText(s, 2, nullptr, 10) == 0);
    CHECK(arenaPaceText(s, 2, big, 0) == 0);
}

TEST_CASE("arena off-frame tells: aiming high, a ground enemy winding up under the frame is told") {
    ArenaView view;
    view.eye = {0.f, 0.f};
    view.forward = {1.f, 0.f};
    view.halfAngle = 45.f;
    view.eyeZ = 1.8f;
    view.vHalfAngle = 28.f;
    ArenaTellBody ground;
    ground.pos = {6.f, 0.f};
    ground.winding = true;
    ground.fill = 0.3f;
    ground.low = 0.f;
    ground.high = 1.8f;
    // Level: in frame.
    view.pitch = 0.f;
    CHECK(arenaOffFrameTells(view, &ground, 1).count == 0);
    // Looking 70 degrees up at a hovering boss: the ground enemy straight ahead is under the frame.
    view.pitch = 70.f;
    const ArenaOffFrameTells up = arenaOffFrameTells(view, &ground, 1);
    REQUIRE(up.count == 1);
    CHECK(up.items[0].bearing == doctest::Approx(0.f));
    // A hovering body high up, looked at: in frame. The same body with the view held level: over the frame.
    ArenaTellBody high = ground;
    high.pos = {6.f, 0.f};
    high.low = 12.f;
    high.high = 18.f;
    CHECK(arenaOffFrameTells(view, &high, 1).count == 0);
    view.pitch = 0.f;
    CHECK(arenaOffFrameTells(view, &high, 1).count == 1);
    // A body spanning the frame (its base under, its top over) is in it.
    ArenaTellBody tall = ground;
    tall.high = 30.f;
    view.pitch = 70.f;
    CHECK(arenaOffFrameTells(view, &tall, 1).count == 0);
    // No vertical half-angle: the horizontal test alone, as before.
    view.vHalfAngle = 0.f;
    CHECK(arenaOffFrameTells(view, &ground, 1).count == 0);
}
