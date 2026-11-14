// The boss break value (ArenaFight.h, "Boss break"): a boss's own measure beside its poise that, filled by the
// player's blows, makes it break out of a combo with its break move. Regular enemies keep their break-out as before.

#include "ftcore/ArenaFight.h"
#include "TestAssets.h"
#include <ostream>
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 60.f;

ArenaTable shippedTable() {
    std::string text;
    if (!ftcore_test::readProjectData("Battle/arena.json", text)) FAIL("Data/Battle/arena.json not found");
    ArenaTable t;
    std::string err;
    REQUIRE_MESSAGE(importArenaTable(text, t, &err), err);
    return t;
}

// ---- the regular enemies' golden trace ----

struct Fnv {
    std::uint64_t h = 1469598103934665603ull;
    void byte(std::uint8_t b) {
        h ^= b;
        h *= 1099511628211ull;
    }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) byte(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void f32(float f) {
        std::uint32_t v = 0;
        std::memcpy(&v, &f, sizeof v);
        u32(v);
    }
};

bool isBossId(const std::string& id) { return id == "kite_boss" || id == "stone_bearer" || id == "white_glove"; }

// Plays `def` with the autopilot (and the partner's pair press on the tick after each parry) for up to 120 s and
// hashes every event, the player's hp and every body's position each tick.
void hashFight(Fnv& h, const ArenaDef& def, std::uint32_t seed) {
    const ArenaRing ring = arenaRingAt(Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, def.ringRadius, ArenaRing{Vec2{0.f, 0.f}, 36.f});
    ArenaFight fight;
    fight.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, seed);
    ArenaBodies bodies;
    bodies.player = fight.playerStart();
    for (std::size_t i = 0; i < fight.enemyCount(); ++i) bodies.enemies[i] = fight.enemyStart(i);
    bool pairNext = false;
    while (!fight.over() && fight.time() < 120.f) {
        ArenaInput in = fight.autopilot();
        in.pair = pairNext;
        pairNext = false;
        const ArenaOutput& out = fight.tick(kDt, in, bodies);
        arenaStep(fight, bodies, kDt);
        for (const ArenaEvent& e : out.events) {
            h.byte(static_cast<std::uint8_t>(e.type));
            h.byte(e.source);
            h.byte(e.target);
            h.byte(e.move);
            h.u32(static_cast<std::uint32_t>(e.amount));
            h.f32(e.at.x);
            h.f32(e.at.y);
            if (e.type == ArenaEventType::Parried) pairNext = true;
        }
        h.u32(static_cast<std::uint32_t>(fight.hp()));
        h.f32(bodies.player.x);
        h.f32(bodies.player.y);
        for (std::size_t i = 0; i < fight.enemyCount(); ++i) {
            h.f32(bodies.enemies[i].x);
            h.f32(bodies.enemies[i].y);
            h.u32(static_cast<std::uint32_t>(fight.enemyHp(i)));
        }
    }
    h.byte(static_cast<std::uint8_t>(fight.phase()));
    const ArenaStats& s = fight.stats();
    for (int v : {s.hits, s.staggers, s.breakOuts, s.breakDue, s.breakUpright, s.counterStaggers, s.readCounters,
                  s.ripostes, s.parries, s.flinchChances})
        h.u32(static_cast<std::uint32_t>(v));
}

} // namespace

TEST_CASE("arena boss break: regular enemies fight byte for byte as before (a golden trace of every shipped one)") {
    const ArenaTable t = shippedTable();
    Fnv h;
    int fights = 0;
    for (std::size_t e = 0; e < t.enemyIds.size(); ++e) {
        if (isBossId(t.enemyIds[e]) || t.enemies[e].passive) continue;
        ArenaDef def;
        def.fighter = t.fighter("default");
        def.enemyCount = 1;
        def.enemies[0] = t.enemies[e];
        def.attackers = 1;
        for (std::uint32_t seed = 1; seed <= 3; ++seed) hashFight(h, def, seed);
        ++fights;
    }
    for (const ArenaFightEntry& entry : t.entries) {
        bool boss = false;
        for (std::size_t i = 0; i < entry.def.enemyCount; ++i)
            if (entry.def.enemyKind[i] < t.enemyIds.size() && isBossId(t.enemyIds[entry.def.enemyKind[i]])) boss = true;
        if (boss) continue;
        hashFight(h, entry.def, 1);
        ++fights;
    }
    hashFight(h, t.fallback, 1);
    MESSAGE("regular golden: " << fights << " fights, hash " << h.h);
    CHECK(fights > 5);
    CHECK(h.h == 12022742029920829981ull); // measured on the code before the boss break value
}

// ---- the boss break value ----

namespace {

ArenaDef bossDef(const ArenaTable& t, const char* id, bool harmless) {
    ArenaDef def;
    def.fighter = t.fighter("default");
    def.enemyCount = 1;
    def.attackers = 1;
    for (std::size_t e = 0; e < t.enemyIds.size(); ++e)
        if (t.enemyIds[e] == id) def.enemies[0] = t.enemies[e];
    REQUIRE(def.enemies[0].boss);
    if (harmless) // its blows land for nothing, so a long test never ends in a Defeat
        for (std::size_t m = 0; m < def.enemies[0].moveCount; ++m) {
            ArenaAttack& a = def.enemies[0].moves[m].attack;
            a.damage = 0;
            a.poiseDamage = 0.f;
            a.guardDamage = 0.f;
        }
    return def;
}

struct Rig {
    ArenaFight fight;
    ArenaBodies bodies;
    const ArenaOutput* out = nullptr;
    explicit Rig(const ArenaDef& def, std::uint32_t seed = 1) {
        const ArenaRing ring =
            arenaRingAt(Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, def.ringRadius, ArenaRing{Vec2{0.f, 0.f}, 36.f});
        fight.begin(def, ring, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, seed);
        bodies.player = fight.playerStart();
        for (std::size_t i = 0; i < fight.enemyCount(); ++i) bodies.enemies[i] = fight.enemyStart(i);
    }
    const ArenaOutput& step(const ArenaInput& in = ArenaInput{}) {
        out = &fight.tick(kDt, in, bodies);
        arenaStep(fight, bodies, kDt);
        return *out;
    }
    void run(float seconds) {
        const long ticks = std::lround(seconds / kDt);
        for (long k = 0; k < ticks; ++k) step();
    }
    float value() const { return fight.enemyBreakValue(0); }
    int breaks() const { return fight.stats().bossBreaks; }
    bool had(ArenaEventType type, int move = -1) const {
        for (const ArenaEvent& e : out->events)
            if (e.type == type && (move < 0 || e.move == move)) return true;
        return false;
    }
};

// Host blows (1.5 each) until boss 0 breaks, a tick after each: how many it took (99: none in 20).
int hostBlowsToBreak(Rig& r) {
    const int before = r.breaks();
    for (int blows = 1; blows <= 20; ++blows) {
        REQUIRE(r.fight.hostBlow(0, 1, 0));
        r.step();
        if (r.breaks() != before) return blows;
    }
    return 99;
}

// The weights, written out here (not the header's constants), so a changed weight fails.
enum BlowKind { kLight, kHeavy, kRiposte, kRelease, kPair, kRead, kKinds };

struct Oracle {
    const ArenaFight* fight = nullptr;
    float scale = 1.f;
    float pending = 0.f; // this tick's blows on boss 0, weighed and capped
    std::array<int, kKinds> kinds{};
    float pairPoise = -1.f, poiseAtPair = -1.f;
};

ArenaBlowRuling oracleHook(void* user, const ArenaBlow& b) {
    Oracle& o = *static_cast<Oracle*>(user);
    ArenaBlowRuling r;
    r.breakScale = o.scale;
    if (b.source != kArenaPlayer || b.target != 0) return r;
    float w = 0.f;
    BlowKind kind = kLight;
    switch (b.type) {
    case ArenaEventType::Hit:
        kind = b.move == kArenaHeavyMove     ? kHeavy
               : b.move == kArenaRiposteMove ? kRiposte
               : b.move == kArenaReleaseMove ? kRelease
                                             : kLight;
        w = kind == kHeavy ? 2.f : kind == kRiposte ? 1.5f : kind == kRelease ? 3.f : 1.f;
        break;
    case ArenaEventType::CounterStagger:
        kind = kPair;
        w = 3.f;
        o.pairPoise = b.poise;
        o.poiseAtPair = o.fight->enemyPoise(0);
        break;
    case ArenaEventType::ReadCounter:
        kind = kRead;
        w = 1.5f;
        break;
    default: return r;
    }
    o.pending += std::min(w * o.scale, 3.f);
    o.kinds[static_cast<std::size_t>(kind)] = 1;
    return r;
}

struct OracleRun {
    std::array<int, kKinds> checked{}; // ticks checked with each kind of blow
    int pairsOpen = 0;                 // paired counters that left the boss Open on the rest of its poise
    int ticksChecked = 0;
};

// The autopilot (and the partner's pair press after each parry) against a boss that never breaks by its value (every
// threshold 1000) for 90 s: each tick the value moves by exactly the blows the hook ruled, weighed and capped, save the
// ticks a stagger, a break or the decay take part in.
OracleRun playOracle(const ArenaTable& t, float scale) {
    ArenaDef def = bossDef(t, "stone_bearer", true); // a body the riposte reaches round (the kite's is too wide)
    def.enemies[0].hp = 100000;
    def.enemies[0].breakValueAt = {1000.f, 1000.f, 1000.f, 1000.f};
    const float maxPoise = def.enemies[0].poise;
    Rig r(def);
    Oracle o;
    o.fight = &r.fight;
    o.scale = scale;
    r.fight.setBlowHook(oracleHook, &o);
    OracleRun run;
    // The autopilot, with presses of its own mixed in so every kind of blow lands: each parry answered by the pair
    // press, a heavy now and then, every 5 s the boss held Open for a riposte, and every 6.7 s Load given for a
    // Release, the boss brought into their reach for both.
    bool pairNext = false;
    long tick = 0;
    ArenaBrain prev = ArenaBrain::Approach;
    while (!r.fight.over() && r.fight.time() < 90.f) {
        ++tick;
        if (tick % 300 == 150 || tick % 400 == 200) { // the boss stands at its own reach: bring it in to 1.5 m
            const Vec2 d{r.bodies.enemies[0].x - r.bodies.player.x, r.bodies.enemies[0].y - r.bodies.player.y};
            const float len = std::max(std::sqrt(d.x * d.x + d.y * d.y), 1e-3f);
            r.bodies.enemies[0] = Vec2{r.bodies.player.x + d.x * 1.5f / len, r.bodies.player.y + d.y * 1.5f / len};
        }
        if (tick % 300 == 150 && prev != ArenaBrain::Staggered) r.fight.holdEnemy(0, ArenaBrain::Open, 0.5f);
        if (tick % 400 == 200) r.fight.giveLoad(60);
        ArenaInput in = r.fight.autopilot();
        in.pair = pairNext;
        pairNext = false;
        if (tick % 97 == 0) {
            in.light = false;
            in.heavy = true;
        }
        if (tick % 300 > 151 && tick % 300 < 170) in.light = true; // the riposte
        if (tick % 400 > 200 && tick % 400 < 260 && r.fight.load() > r.fight.releaseOpenLoad()) in.release = true;
        o.pending = 0.f;
        o.kinds = {};
        o.pairPoise = -1.f;
        const float before = r.value();
        const int breaksBefore = r.breaks();
        const ArenaOutput& out = r.step(in);
        bool skip = prev == ArenaBrain::Staggered || out.enemies[0].brain == ArenaBrain::Staggered ||
                    r.breaks() != breaksBefore;
        for (const ArenaEvent& e : out.events) {
            if (e.type == ArenaEventType::Parried) pairNext = true;
            if (e.type == ArenaEventType::Staggered || e.type == ArenaEventType::BrokeOut) skip = true;
        }
        prev = out.enemies[0].brain;
        if (o.pairPoise >= 0.f) CHECK(o.pairPoise == doctest::Approx(0.6f * maxPoise)); // 60%, not the whole poise
        if (skip || (o.pending == 0.f && r.value() < before)) continue;                 // a stagger, a break, the decay
        CHECK(r.value() == doctest::Approx(before + o.pending));
        ++run.ticksChecked;
        int kindsThisTick = 0;
        for (std::size_t k = 0; k < kKinds; ++k) {
            run.checked[k] += o.kinds[k];
            kindsThisTick += o.kinds[k];
        }
        if (o.pairPoise >= 0.f && kindsThisTick == 1 && o.poiseAtPair > 0.6f * maxPoise + 1e-3f) {
            CHECK(out.enemies[0].brain == ArenaBrain::Open);
            CHECK(r.fight.enemyPoise(0) == doctest::Approx(o.poiseAtPair - 0.6f * maxPoise));
            ++run.pairsOpen;
        }
    }
    return run;
}

} // namespace

TEST_CASE("arena boss break: the shipped bosses each break with a dodgeable move of their own, and no other kind does") {
    const ArenaTable t = shippedTable();
    int bosses = 0;
    for (std::size_t e = 0; e < t.enemyIds.size(); ++e) {
        const ArenaEnemyKind& k = t.enemies[e];
        CAPTURE(t.enemyIds[e]);
        CHECK(k.boss == isBossId(t.enemyIds[e]));
        if (!k.boss) continue;
        ++bosses;
        CHECK(k.breakAt == 0);
        REQUIRE(k.breakMove >= 0);
        REQUIRE(k.breakMove < static_cast<int>(k.moveCount));
        const ArenaEnemyMove& mv = k.moves[static_cast<std::size_t>(k.breakMove)];
        CHECK(mv.parryable);
        CHECK(mv.chain < 0);
        CHECK(std::string(arenaTellName(mv.tell)) == "backhand"); // its existing call word
        CHECK(mv.attack.startup != doctest::Approx(0.5f));       // the break's windup is its own
    }
    CHECK(bosses == 3);
}

TEST_CASE("arena boss break: every weight, each capped at 3, through the autopilot's own blows") {
    const ArenaTable t = shippedTable();
    for (float scale : {1.f, 2.5f, 3.75f}) { // none, the joined x2.5, the joined and Lift x3.75
        CAPTURE(scale);
        const OracleRun run = playOracle(t, scale);
        MESSAGE("scale " << scale << ": ticks " << run.ticksChecked << ", light " << run.checked[kLight] << ", heavy "
                         << run.checked[kHeavy] << ", riposte " << run.checked[kRiposte] << ", release "
                         << run.checked[kRelease] << ", pair " << run.checked[kPair] << ", read "
                         << run.checked[kRead] << ", pairs left Open " << run.pairsOpen);
        CHECK(run.ticksChecked > 1000);
        for (std::size_t k = 0; k < kKinds; ++k) CHECK(run.checked[k] > 0);
        CHECK(run.pairsOpen > 0);
    }
}

TEST_CASE("arena boss break: a hostBlow adds 1.5, and the value goes back to 0 2.5 s after the last blow") {
    const ArenaTable t = shippedTable();
    Rig r(bossDef(t, "stone_bearer", true));
    r.step();
    CHECK(r.value() == 0.f);
    REQUIRE(r.fight.hostBlow(0, 1, 0));
    CHECK(r.value() == doctest::Approx(1.5f));
    r.step(); // the blow's tick
    r.run(2.f);
    REQUIRE(r.fight.hostBlow(0, 1, 0)); // a second blow starts the 2.5 s again
    CHECK(r.value() == doctest::Approx(3.f));
    r.step();
    r.run(2.4f);
    CHECK(r.value() == doctest::Approx(3.f));
    r.run(0.2f);
    CHECK(r.value() == 0.f);
    CHECK(r.breaks() == 0);
}

TEST_CASE("arena boss break: while the boss is Staggered the value neither rises nor decays") {
    const ArenaTable t = shippedTable();
    Rig r(bossDef(t, "stone_bearer", true));
    r.step();
    REQUIRE(r.fight.hostBlow(0, 1, 0));
    REQUIRE(r.fight.hostBlow(0, 1, 0));
    REQUIRE(r.fight.holdEnemy(0, ArenaBrain::Staggered, 5.f));
    r.step();
    REQUIRE(r.out->enemies[0].brain == ArenaBrain::Staggered);
    r.run(4.5f);
    CHECK(r.value() == doctest::Approx(3.f)); // no decay
    REQUIRE(r.fight.hostBlow(0, 1, 0));
    CHECK(r.value() == doctest::Approx(3.f)); // no rise
    r.run(0.6f);
    REQUIRE(r.out->enemies[0].brain != ArenaBrain::Staggered);
    r.run(2.2f); // the decay counts again only from the hold's end
    CHECK(r.value() == doctest::Approx(3.f));
    r.run(0.5f);
    CHECK(r.value() == 0.f);
}

TEST_CASE("arena boss break: each threshold by phase (10, 8, 6.5 by default; the stone bearer's phases at half and a quarter)") {
    const ArenaTable t = shippedTable();
    struct Case {
        const char* id;
        int phase;
        float share; // the hp share the boss is brought to first
        float at;
        int blows; // host blows of 1.5 that break it
    };
    const Case cases[] = {
        {"kite_boss", 0, 1.f, 10.f, 7},     {"kite_boss", 1, 0.66f, 8.f, 6},   {"kite_boss", 2, 0.33f, 6.5f, 5},
        {"white_glove", 0, 1.f, 10.f, 7},   {"white_glove", 1, 0.6f, 8.f, 6},  {"white_glove", 2, 0.3f, 6.5f, 5},
        {"stone_bearer", 0, 0.6f, 10.f, 7}, {"stone_bearer", 1, 0.5f, 8.f, 6}, {"stone_bearer", 2, 0.25f, 6.5f, 5},
    };
    for (const Case& c : cases) {
        CAPTURE(c.id);
        CAPTURE(c.phase);
        const ArenaDef def = bossDef(t, c.id, true);
        Rig r(def);
        r.step();
        const int down = def.enemies[0].hp - static_cast<int>(c.share * static_cast<float>(def.enemies[0].hp));
        if (down > 0) {
            REQUIRE(r.fight.hostBlow(0, down, 0));
            r.run(3.f); // that blow's value decays
        }
        REQUIRE(r.value() == 0.f);
        CHECK(r.fight.enemyBreakPhase(0) == c.phase);
        CHECK(r.fight.enemyBreakAt(0) == doctest::Approx(c.at));
        CHECK(hostBlowsToBreak(r) == c.blows);
        CHECK(r.had(ArenaEventType::BrokeOut, def.enemies[0].breakMove));
        CHECK(r.had(ArenaEventType::AttackStarted, def.enemies[0].breakMove));
        CHECK(r.fight.stats().bossBreaksForced == 0);
    }
}

TEST_CASE("arena boss break: the tell at 75% of the threshold, once as it rises there") {
    const ArenaTable t = shippedTable();
    Rig r(bossDef(t, "kite_boss", true));
    r.step();
    for (int blow = 1; blow <= 4; ++blow) {
        REQUIRE(r.fight.hostBlow(0, 1, 0));
        r.step();
    }
    CHECK(r.value() == doctest::Approx(6.f));
    CHECK_FALSE(r.fight.enemyBreakTell(0));
    REQUIRE(r.fight.hostBlow(0, 1, 0)); // 7.5: 75% of 10
    r.step();
    CHECK(r.fight.enemyBreakTell(0));
    CHECK(r.had(ArenaEventType::BreakTell));
    for (const ArenaEvent& e : r.out->events)
        if (e.type == ArenaEventType::BreakTell) CHECK(e.amount == 750);
    REQUIRE(r.fight.hostBlow(0, 1, 0)); // 9: still telling, no second event
    r.step();
    CHECK(r.fight.enemyBreakTell(0));
    CHECK_FALSE(r.had(ArenaEventType::BreakTell));
    CHECK(r.fight.stats().bossBreakTells == 1);
    CHECK(r.breaks() == 0);
    REQUIRE(r.fight.hostBlow(0, 1, 0)); // 10.5: the break, and the tell goes with the value
    r.step();
    CHECK(r.breaks() == 1);
    CHECK_FALSE(r.fight.enemyBreakTell(0));
}

TEST_CASE("arena boss break: the break resets the value, winds up 0.5 s, leaves a 1.2 s punish window and locks 6 s") {
    const ArenaTable t = shippedTable();
    const ArenaDef def = bossDef(t, "stone_bearer", true);
    const int move = def.enemies[0].breakMove;
    const ArenaAttack& data = def.enemies[0].moves[static_cast<std::size_t>(move)].attack;
    REQUIRE(data.startup != doctest::Approx(0.5f));
    REQUIRE(data.recovery != doctest::Approx(1.2f));
    Rig r(def);
    r.step();
    REQUIRE(hostBlowsToBreak(r) == 7);
    const float broke = r.fight.time();
    CHECK(r.value() == 0.f);
    CHECK(r.fight.enemyBreakLock(0) == doctest::Approx(6.f));
    REQUIRE(r.out->enemies[0].brain == ArenaBrain::Attack);
    REQUIRE(r.out->enemies[0].move == move);
    // The parts as the host and the autopilot see them: 0.5 s of startup, the move's own active part, 1.2 s of recovery.
    int startup = 0, active = 0, recovery = 0;
    while (r.out->enemies[0].brain == ArenaBrain::Attack && r.fight.time() < broke + 5.f) {
        const ArenaPart part = r.out->enemies[0].part;
        startup += part == ArenaPart::Startup ? 1 : 0;
        active += part == ArenaPart::Active ? 1 : 0;
        recovery += part == ArenaPart::Recovery ? 1 : 0;
        r.step();
    }
    CHECK(static_cast<float>(startup) * kDt == doctest::Approx(0.5f).epsilon(0.05));
    CHECK(static_cast<float>(active) * kDt == doctest::Approx(data.active).epsilon(0.15));
    CHECK(static_cast<float>(recovery) * kDt == doctest::Approx(1.2f).epsilon(0.05));
    // The lock: the value fills past the threshold (two blows a second) but no break begins before 6 s.
    long tick = 0;
    while (r.fight.time() < broke + 5.9f) {
        if (tick++ % 30 == 0) REQUIRE(r.fight.hostBlow(0, 1, 0));
        r.step();
    }
    CHECK(r.value() >= 10.f);
    CHECK(r.breaks() == 1);
    while (r.fight.time() < broke + 6.1f) r.step();
    CHECK(r.breaks() == 2);
}

TEST_CASE("arena boss break: forced after 20 s above 0 with 3 blows, not with 2") {
    const ArenaTable t = shippedTable();
    ArenaDef def = bossDef(t, "stone_bearer", true);
    def.enemies[0].breakValueAt = {1000.f, 1000.f, 1000.f, 1000.f}; // the threshold is out of reach
    SUBCASE("a blow every 2 s: forced at 20 s") {
        Rig r(def);
        r.step();
        const float first = r.fight.time();
        long tick = 0;
        while (r.fight.time() < first + 19.8f) {
            if (tick++ % 120 == 0) REQUIRE(r.fight.hostBlow(0, 1, 0));
            r.step();
        }
        CHECK(r.breaks() == 0);
        while (r.fight.time() < first + 20.2f) r.step();
        CHECK(r.breaks() == 1);
        CHECK(r.fight.stats().bossBreaksForced == 1);
        CHECK(r.value() == 0.f);
    }
    SUBCASE("held Staggered past 20 s: 3 blows break it once it is up, 2 do not") {
        for (int blows : {2, 3}) {
            CAPTURE(blows);
            Rig r(def);
            r.step();
            for (int b = 0; b < blows; ++b) REQUIRE(r.fight.hostBlow(0, 1, 0));
            REQUIRE(r.fight.holdEnemy(0, ArenaBrain::Staggered, 22.f));
            r.run(21.5f); // the run passes 20 s while it is held: due, but not upright
            CHECK(r.breaks() == 0);
            CHECK(r.value() == doctest::Approx(1.5f * static_cast<float>(blows)));
            r.run(1.f);
            CHECK(r.breaks() == (blows >= 3 ? 1 : 0));
            r.run(3.f);
            CHECK(r.value() == 0.f);
            CHECK(r.breaks() == (blows >= 3 ? 1 : 0));
        }
    }
}

TEST_CASE("arena boss break: a set table's split onto a seated boss adds 3") {
    const ArenaTable t = shippedTable();
    Rig r(bossDef(t, "stone_bearer", false));
    ArenaTableSeats seats;
    seats.player = true;
    seats.enemies = 1u;
    seats.enemyShare[0] = 1.f;
    r.fight.setTableSeats(seats);
    bool shared = false;
    while (!shared && !r.fight.over() && r.fight.time() < 60.f) {
        const float before = r.value();
        r.step();
        for (const ArenaEvent& e : r.out->events)
            if (e.type == ArenaEventType::TableShare && e.target == 0) shared = true;
        if (shared) CHECK(r.value() == doctest::Approx(before + 3.f));
    }
    CHECK(shared);
}

TEST_CASE("arena boss break: arena.json refuses a boss without its break move, with a count, or bad phase ends") {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Battle/arena.json", text));
    const auto refused = [&](const std::string& from, const std::string& to, const char* says) {
        CAPTURE(to);
        std::string changed = text;
        const std::size_t at = changed.find(from);
        REQUIRE(at != std::string::npos);
        changed.replace(at, from.size(), to);
        ArenaTable table;
        std::string err;
        CHECK_FALSE(importArenaTable(changed, table, &err));
        CHECK_MESSAGE(err.find(says) != std::string::npos, err);
    };
    refused("\"boss\": true, \"breakMove\": 2,", "\"boss\": true,", "a boss needs the move it breaks out with");
    refused("\"boss\": true, \"breakMove\": 7,", "\"boss\": true, \"breakMove\": 7, \"breakAt\": 4,", "not by a count");
    refused("\"breakPhaseEnds\": [0.5, 0.25]", "\"breakPhaseEnds\": [0.25, 0.5]", "below the one before");
    refused("\"breakPhaseEnds\": [0.5, 0.25]", "\"breakPhaseEnds\": [0.5]", "one hp share per break phase");
    refused("\"boss\": true, \"breakMove\": 2,", "\"breakMove\": 2, \"breakValueAt\": [5],", "are a boss's");
    // A boss's own thresholds: two phases, split at half by default.
    std::string own = text;
    const std::string from = "\"boss\": true, \"breakMove\": 2,";
    const std::size_t at = own.find(from);
    REQUIRE(at != std::string::npos);
    own.replace(at, from.size(), "\"boss\": true, \"breakMove\": 2, \"breakValueAt\": [12, 9],");
    ArenaTable table;
    std::string err;
    REQUIRE_MESSAGE(importArenaTable(own, table, &err), err);
    bool seen = false;
    for (std::size_t e = 0; e < table.enemyIds.size(); ++e) {
        if (table.enemyIds[e] != "white_glove") continue;
        seen = true;
        CHECK(table.enemies[e].breakPhases == 2);
        CHECK(table.enemies[e].breakValueAt[1] == doctest::Approx(9.f));
        CHECK(table.enemies[e].breakPhaseEnds[0] == doctest::Approx(0.5f));
    }
    CHECK(seen);
}
