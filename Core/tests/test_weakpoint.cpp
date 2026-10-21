#include "ftcore/Battle.h"
#include "ftcore/BattleDemo.h"
#include "ftcore/Encounter.h"
#include "ftcore/WeakPoint.h"
#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace ftcore;

namespace {

constexpr double kStart = 20.0;

// No sway: the aim point is the reticle.
AimTuning steady() {
    AimTuning t;
    t.swayX = 0.0f;
    t.swayY = 0.0f;
    return t;
}

WeakPoints twoPoints() {
    return demo::makeWeakPoints(0.5f, {demo::makeWeakPoint("core", 0.0f, 0.55f, 0.12f, 2.5f),
                                       demo::makeWeakPoint("knee", -0.28f, 0.15f, 0.10f, 1.5f)});
}

Combatant gunner() {
    Combatant c;
    c.name = "Hero";
    c.stats = {100, 20, 0, 20, 10};
    c.hp = 100;
    c.skills = {{"Strike", 0, 1, Targeting::SingleEnemy, 1.0f, 4, 1.0f}};
    return c;
}

Combatant target(WeakPoints points, int hp = 500) {
    Combatant c;
    c.name = "Target";
    c.stats = {hp, 10, 0, 5, 10};
    c.hp = hp;
    c.skills = {{"Claw", 0, 0, Targeting::SingleEnemy, 1.0f, 4, 2.0f}};
    c.weakPoints = points;
    return c;
}

BattleTuning steadyTuning() {
    BattleTuning t;
    t.leadIn = 0.5;
    t.recovery = 0.5;
    t.aim = steady();
    return t;
}

const BattleEvent* findEvent(const std::vector<BattleEvent>& events, EventType type) {
    for (const BattleEvent& e : events)
        if (e.type == type) return &e;
    return nullptr;
}

const char* kRoster = R"({"version":1,"enemies":[
  {"id":"brute","name":"Brute","stats":{"maxHp":10,"attack":1,"defense":0,"speed":1,"technique":1},
   "skills":[{"id":"slam","name":"Slam","power":1,"notes":3,"difficulty":1}]}]})";

} // namespace

TEST_CASE("weak points: a shot is judged on the weak point it lands in, else body or graze") {
    const WeakPoints p = twoPoints();
    const AimTuning t = steady();
    CHECK(p.best() == 0);

    ShotResult r = judgeShot(p, {0.0f, 0.55f}, t);
    CHECK(r.kind == ShotKind::Weak);
    CHECK(r.weakPoint == 0);
    CHECK(r.multiplier == doctest::Approx(2.5f));

    r = judgeShot(p, {0.0f + 0.119f, 0.55f}, t); // inside the edge
    CHECK(r.kind == ShotKind::Weak);
    r = judgeShot(p, {-0.28f, 0.06f}, t);
    CHECK(r.kind == ShotKind::Weak);
    CHECK(r.weakPoint == 1);
    CHECK(r.multiplier == doctest::Approx(1.5f));

    r = judgeShot(p, {0.3f, 0.9f}, t); // on the body, no weak point
    CHECK(r.kind == ShotKind::Body);
    CHECK(r.weakPoint == kMaxWeakPoints);
    CHECK(r.multiplier == doctest::Approx(t.bodyMultiplier));

    r = judgeShot(p, {0.6f, 0.5f}, t); // off the body
    CHECK(r.kind == ShotKind::Graze);
    CHECK(r.multiplier == doctest::Approx(t.grazeMultiplier));
    r = judgeShot(p, {0.0f, -0.05f}, t);
    CHECK(r.kind == ShotKind::Graze);

    // Overlapping circles: the bigger multiplier wins.
    WeakPoints overlap = demo::makeWeakPoints(0.5f, {demo::makeWeakPoint("a", 0.0f, 0.5f, 0.2f, 1.5f),
                                                     demo::makeWeakPoint("b", 0.05f, 0.5f, 0.2f, 3.0f)});
    r = judgeShot(overlap, {0.0f, 0.5f}, t);
    CHECK(r.weakPoint == 1);

    CHECK(std::string_view(shotKindName(ShotKind::Weak)) == "WEAK POINT");
    CHECK(std::string_view(shotKindName(ShotKind::Body)) == "HIT");
    CHECK(std::string_view(shotKindName(ShotKind::Graze)) == "GRAZE");
}

TEST_CASE("weak points: a figure holds at most three; ids are fixed buffers") {
    WeakPoints p;
    CHECK(p.empty());
    CHECK(p.best() == 0);
    for (int i = 0; i < 3; ++i) CHECK(p.add(demo::makeWeakPoint("x", 0.0f, 0.5f, 0.1f, 1.0f + i)));
    CHECK_FALSE(p.add(demo::makeWeakPoint("y", 0.0f, 0.5f, 0.1f, 2.0f)));
    CHECK(p.size() == 3);
    CHECK(p.best() == 2);
    WeakPoint w;
    w.setId("a_very_long_identifier_indeed");
    CHECK(w.id().size() == kWeakPointIdCapacity - 1);
}

TEST_CASE("aim shot: the reticle starts centred, moves, clamps, and sways on a fake clock") {
    AimShot a;
    AimTuning t; // with sway
    a.reset(twoPoints(), kStart, t);
    CHECK(a.reticle().x == 0.0f);
    CHECK(a.reticle().y == 0.5f);
    CHECK(a.deadline() == doctest::Approx(kStart + t.timeLimit));
    CHECK_FALSE(a.expired(kStart + t.timeLimit - 0.01));
    CHECK(a.expired(kStart + t.timeLimit));

    a.move(0.1f, 0.2f);
    CHECK(a.reticle().x == doctest::Approx(0.1f));
    CHECK(a.reticle().y == doctest::Approx(0.7f));
    a.move(10.0f, -10.0f);
    CHECK(a.reticle().x == doctest::Approx(a.maxX()));
    CHECK(a.reticle().y == doctest::Approx(a.minY()));
    CHECK(a.maxX() == doctest::Approx(0.5f + t.margin));

    // Sway: zero at the start, bounded, periodic.
    CHECK(a.sway(kStart).x == doctest::Approx(0.0f));
    for (double s = 0.0; s < 4.0; s += 0.05) {
        CHECK(std::abs(a.sway(kStart + s).x) <= t.swayX + 1e-6f);
        CHECK(std::abs(a.sway(kStart + s).y) <= t.swayY + 1e-6f);
    }
    CHECK(a.sway(kStart + t.swayPeriodX * 0.25).x == doctest::Approx(t.swayX));
    CHECK(a.sway(kStart + 0.4).x == doctest::Approx(a.sway(kStart + 0.4 + t.swayPeriodX).x));

    // reticleFor puts the aim point exactly on the point at that time, sway included.
    for (double s : {0.0, 0.37, 1.1, 2.9}) {
        const AimPoint r = a.reticleFor(0, kStart + s);
        a.set(r.x, r.y);
        const ShotResult shot = a.judge(kStart + s);
        CHECK(shot.kind == ShotKind::Weak);
        CHECK(shot.weakPoint == 0);
        CHECK(shot.at.x == doctest::Approx(0.0f));
        CHECK(shot.at.y == doctest::Approx(0.55f));
    }
    // A reticle on the point's centre can sway out of a small circle: the shot is a timing too.
    WeakPoints tiny = demo::makeWeakPoints(0.5f, {demo::makeWeakPoint("pin", 0.0f, 0.5f, kMinWeakRadius, 2.0f)});
    t.swayX = 0.2f;
    a.reset(tiny, kStart, t);
    a.set(0.0f, 0.5f);
    CHECK(a.judge(kStart).kind == ShotKind::Weak);
    CHECK(a.judge(kStart + t.swayPeriodX * 0.25).kind == ShotKind::Body);
}

TEST_CASE("aim shot: reset, move, judge and sway never allocate") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    const WeakPoints p = twoPoints();
    AimShot a;
    const std::uint64_t before = ftcore_test::allocStats().count;
    int weak = 0;
    for (int i = 0; i < 100; ++i) {
        a.reset(p, kStart + i, AimTuning{});
        a.move(0.01f, 0.0f);
        const AimPoint r = a.reticleFor(p.best(), kStart + i + 0.5);
        a.set(r.x, r.y);
        if (a.judge(kStart + i + 0.5).kind == ShotKind::Weak) ++weak;
    }
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(weak == 100);
}

TEST_CASE("aim in battle: AP, damage per shot kind, events, recovery") {
    struct Case {
        float x, y;
        ShotKind kind;
        int damage; // 20 attack * 1.4 power = 28 before the multiplier
        Grade grade;
    };
    for (const Case c : {Case{0.0f, 0.55f, ShotKind::Weak, 70, Grade::Perfect},
                         Case{-0.28f, 0.15f, ShotKind::Weak, 42, Grade::Perfect},
                         Case{0.3f, 0.9f, ShotKind::Body, 28, Grade::Good},
                         Case{0.8f, 0.5f, ShotKind::Graze, 14, Grade::Miss}}) {
        CAPTURE(c.x);
        Battle b({gunner()}, {target(twoPoints())}, steadyTuning());
        b.start(kStart);
        REQUIRE(b.phase() == Phase::AwaitingCommand);
        REQUIRE(b.actor() == 0);
        const int ap = b.combatant(0).ap;
        b.drainEvents();
        REQUIRE(b.aim(1, kStart) == CommandError::None);
        CHECK(b.phase() == Phase::PartyAim);
        CHECK(b.aimTarget() == 1);
        CHECK(b.combatant(0).ap == ap - b.tuning().aim.apCost);
        CHECK(b.aimShot().weakPoints().size() == 2);
        const auto started = b.drainEvents();
        REQUIRE(findEvent(started, EventType::SkillStarted));
        CHECK(findEvent(started, EventType::SkillStarted)->label == "AIM");

        b.update(kStart + 1.0); // nothing happens before the deadline
        CHECK(b.phase() == Phase::PartyAim);
        b.setReticle(c.x, c.y);
        REQUIRE(b.fire(kStart + 1.0));
        CHECK(b.lastShot().kind == c.kind);
        const auto events = b.drainEvents();
        const BattleEvent* hit = findEvent(events, EventType::Hit);
        REQUIRE(hit);
        CHECK(hit->amount == c.damage);
        CHECK(hit->grade == c.grade);
        CHECK(hit->label == shotKindName(c.kind));
        CHECK(hit->source == 0);
        CHECK(hit->target == 1);
        CHECK(b.combatant(1).hp == 500 - c.damage);
        CHECK(b.phase() == Phase::Recovery);
        CHECK_FALSE(b.fire(kStart + 1.1)); // one shot
    }
}

TEST_CASE("aim in battle: refusals, cancel refunds, timeout fires where the reticle is") {
    SUBCASE("refusals") {
        Battle b({gunner()}, {target(twoPoints())}, steadyTuning());
        CHECK(b.aim(1, 0.0) == CommandError::WrongPhase);
        b.start(kStart);
        CHECK(b.aim(0, kStart) == CommandError::InvalidTarget); // a party member
        CHECK(b.aim(7, kStart) == CommandError::InvalidTarget);
        BattleTuning poor = steadyTuning();
        poor.startingAp = 1;
        Battle c({gunner()}, {target(twoPoints())}, poor);
        c.start(kStart);
        CHECK(c.aim(1, kStart) == CommandError::NotEnoughAp);
        CHECK_FALSE(c.fire(kStart));
        CHECK_FALSE(c.cancelAim(kStart));
    }
    SUBCASE("cancel gives the AP back and the same member chooses again") {
        Battle b({gunner()}, {target(twoPoints())}, steadyTuning());
        b.start(kStart);
        const int ap = b.combatant(0).ap;
        REQUIRE(b.aim(1, kStart) == CommandError::None);
        b.moveReticle(0.2f, 0.0f);
        CHECK(b.cancelAim(kStart + 0.5));
        CHECK(b.phase() == Phase::AwaitingCommand);
        CHECK(b.actor() == 0);
        CHECK(b.combatant(0).ap == ap);
        CHECK(b.combatant(1).hp == 500);
        b.moveReticle(0.1f, 0.0f); // ignored outside aim
        // Aim again: the reticle starts centred.
        REQUIRE(b.aim(1, kStart + 1.0) == CommandError::None);
        CHECK(b.aimShot().reticle().x == 0.0f);
    }
    SUBCASE("the deadline fires on its own, judged at the deadline") {
        Battle b({gunner()}, {target(twoPoints())}, steadyTuning());
        b.start(kStart);
        REQUIRE(b.aim(1, kStart) == CommandError::None);
        b.setReticle(0.0f, 0.55f);
        b.drainEvents();
        b.update(kStart + b.tuning().aim.timeLimit + 0.3);
        CHECK(b.lastShot().kind == ShotKind::Weak);
        CHECK(b.combatant(1).hp == 500 - 70);
        const auto events = b.drainEvents();
        const BattleEvent* hit = findEvent(events, EventType::Hit);
        REQUIRE(hit);
        CHECK(hit->time == doctest::Approx(kStart + b.tuning().aim.timeLimit));
        CHECK(b.phase() == Phase::Recovery);
    }
    SUBCASE("no weak points: the centre is a body shot") {
        Battle b({gunner()}, {target(WeakPoints{})}, steadyTuning());
        b.start(kStart);
        REQUIRE(b.aim(1, kStart) == CommandError::None);
        REQUIRE(b.fire(kStart + 0.2));
        CHECK(b.lastShot().kind == ShotKind::Body);
        CHECK(b.combatant(1).hp == 500 - 28);
    }
    SUBCASE("a killing shot ends the battle") {
        Battle b({gunner()}, {target(twoPoints(), 50)}, steadyTuning());
        b.start(kStart);
        REQUIRE(b.aim(1, kStart) == CommandError::None);
        b.setReticle(0.0f, 0.55f);
        REQUIRE(b.fire(kStart + 0.1));
        CHECK(b.phase() == Phase::Victory);
    }
}

TEST_CASE("aim in battle: aiming and firing never allocate once warmed up") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    Battle b({gunner()}, {target(twoPoints(), 100000)}, steadyTuning());
    b.start(kStart);
    double now = kStart;
    int shots = 0;
    auto round = [&] {
        while (b.phase() != Phase::AwaitingCommand) {
            now += 0.05;
            demo::pilotDefend(b, now);
            b.update(now);
        }
        if (b.combatant(0).ap < b.tuning().aim.apCost) {
            REQUIRE(b.command(0, 1, now) == CommandError::None);
            while (b.phase() == Phase::PartySkill) {
                demo::pilotPress(b, now);
                now += 0.05;
                b.update(now);
            }
            return;
        }
        REQUIRE(b.aim(1, now) == CommandError::None);
        b.moveReticle(0.01f, 0.0f);
        if (demo::pilotShoot(b, 0, now + 0.1)) ++shots;
        b.clearEvents();
    };
    for (int i = 0; i < 6; ++i) round(); // warm up
    b.clearEvents();
    const std::uint64_t before = ftcore_test::allocStats().count;
    for (int i = 0; i < 20; ++i) round();
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(shots > 5);
}

TEST_CASE("weakpoints.json: parse, limits, errors and warnings") {
    EnemyRoster roster;
    std::string err;
    REQUIRE(importEnemyRoster(kRoster, roster, &err));

    WeakPointTable table;
    std::vector<std::string> warnings;
    REQUIRE(importWeakPointTable(R"({"version":1,"weakpoints":{
        "brute":{"halfWidth":0.5,"points":[{"id":"core","x":0,"y":0.55,"radius":0.12,"multiplier":2.5},
                                           {"id":"knee","x":-0.28,"y":0.15,"radius":0.1,"multiplier":1.5}]},
        "ghost":{"points":[]}}})",
                                 roster, table, &err, &warnings));
    REQUIRE(table.entries.size() == 2);
    const WeakPoints* brute = table.find("brute");
    REQUIRE(brute);
    CHECK(sameWeakPoints(*brute, twoPoints()));
    CHECK(table.find("nobody") == nullptr);
    REQUIRE(warnings.size() == 1); // ghost isn't in the roster
    CHECK(warnings[0].find("ghost") != std::string::npos);

    struct Bad {
        const char* json;
        const char* fragment;
    };
    for (const Bad bad : {
             Bad{"[]", "expected an object"},
             Bad{R"({"version":2,"weakpoints":{}})", "version"},
             Bad{R"({"weakpoints":[]})", "keyed by enemy id"},
             Bad{R"({"weakpoints":{"brute":{"halfWidth":0}}})", "halfWidth"},
             Bad{R"({"weakpoints":{"brute":{"points":[{"id":"A","x":0,"y":0.5,"radius":0.1,"multiplier":2}]}}})", ".id"},
             Bad{R"({"weakpoints":{"brute":{"points":[{"id":"a","x":0,"y":0.5,"radius":0.1,"multiplier":2},
                                                      {"id":"a","x":0,"y":0.5,"radius":0.1,"multiplier":2}]}}})",
                 "duplicate"},
             Bad{R"({"weakpoints":{"brute":{"points":[{"id":"a","x":2,"y":0.5,"radius":0.1,"multiplier":2}]}}})", ".x"},
             Bad{R"({"weakpoints":{"brute":{"points":[{"id":"a","x":0,"y":0.5,"radius":0.01,"multiplier":2}]}}})", ".radius"},
             Bad{R"({"weakpoints":{"brute":{"points":[{"id":"a","x":0,"y":0.5,"radius":0.1,"multiplier":9}]}}})", ".multiplier"},
             Bad{R"({"weakpoints":{"brute":{"points":[{"id":"a","x":0,"y":0.5,"radius":0.1}]}}})", ".multiplier"},
             Bad{R"({"weakpoints":{"brute":{"points":[{"id":"a","x":0,"y":0.5,"radius":0.1,"multiplier":2},
                 {"id":"b","x":0,"y":0.5,"radius":0.1,"multiplier":2},{"id":"c","x":0,"y":0.5,"radius":0.1,"multiplier":2},
                 {"id":"d","x":0,"y":0.5,"radius":0.1,"multiplier":2}]}}})",
                 "0..3"},
         }) {
        CAPTURE(bad.json);
        WeakPointTable t;
        std::string e;
        CHECK_FALSE(importWeakPointTable(bad.json, roster, t, &e));
        CHECK(t.entries.empty());
        CHECK(e.find(bad.fragment) != std::string::npos);
    }

    // Unknown keys are warnings.
    warnings.clear();
    CHECK(importWeakPointTable(R"({"weakpoints":{"brute":{"colour":1,"points":[]}},"extra":0})", roster, table, &err,
                               &warnings));
    CHECK(warnings.size() == 2);
}

TEST_CASE("applyWeakPoints follows the line-up order") {
    EnemyRoster roster;
    std::string err;
    REQUIRE(importEnemyRoster(R"({"version":1,"enemies":[
      {"id":"a","name":"A","stats":{"maxHp":10,"attack":1,"defense":0,"speed":1,"technique":1},
       "skills":[{"id":"s","name":"S","power":1,"notes":3,"difficulty":1}]},
      {"id":"b","name":"B","stats":{"maxHp":10,"attack":1,"defense":0,"speed":1,"technique":1},
       "skills":[{"id":"s","name":"S","power":1,"notes":3,"difficulty":1}]}]})",
                              roster, &err));
    WeakPointTable table;
    REQUIRE(importWeakPointTable(
        R"({"weakpoints":{"b":{"halfWidth":0.3,"points":[{"id":"p","x":0,"y":0.5,"radius":0.1,"multiplier":2}]}}})",
        roster, table, &err));
    EncounterLineup lineup;
    lineup.enemies = {1, 0, 1};
    std::vector<Combatant> foes;
    makeEnemies(roster, lineup, foes);
    REQUIRE(foes.size() == 3);
    applyWeakPoints(table, roster, lineup, foes);
    CHECK(foes[0].weakPoints.size() == 1);
    CHECK(foes[0].weakPoints.halfWidth == doctest::Approx(0.3f));
    CHECK(foes[1].weakPoints.empty());
    CHECK(foes[2].weakPoints.size() == 1);
}

TEST_CASE("Data/Battle/weakpoints.json loads clean, names real enemies and matches the demo") {
    std::string enemiesText, weakText, err;
    REQUIRE(ftcore_test::readProjectData("Battle/enemies.json", enemiesText));
    REQUIRE(ftcore_test::readProjectData("Battle/weakpoints.json", weakText));
    EnemyRoster roster;
    REQUIRE(importEnemyRoster(enemiesText, roster, &err));
    WeakPointTable table;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importWeakPointTable(weakText, roster, table, &err, &warnings), err);
    CHECK(warnings.empty());
    for (const WeakPointEntry& e : table.entries) {
        CAPTURE(e.enemy);
        CHECK(roster.find(e.enemy) >= 0);
        CHECK(e.points.size() >= 1);
        for (const WeakPoint& p : e.points) {
            CHECK(p.multiplier >= 1.5f);
            CHECK(p.multiplier <= 2.5f);
            // The circle sits on the body, so a shot on its centre is never a graze.
            CHECK(std::abs(p.x) <= e.points.halfWidth);
        }
    }
    REQUIRE(table.find("brute"));
    REQUIRE(table.find("shade"));
    CHECK(sameWeakPoints(*table.find("brute"), demo::bruteWeakPoints()));
    CHECK(sameWeakPoints(*table.find("shade"), demo::shadeWeakPoints()));
}

TEST_CASE("headless: the demo fight with weak points is won and the pilot aims") {
    for (const double step : {1.0 / 120.0, 0.13}) {
        CAPTURE(step);
        const demo::HeadlessResult r = demo::playHeadless(demo::party(), demo::enemies(), true, step);
        CHECK_FALSE(r.commandFailed);
        CHECK(r.outcome == Phase::Victory);
        CHECK(r.aims > 0);
        CHECK(r.weakHits == r.aims); // the pilot never misses its weak point
        MESSAGE("step " << step << ": victory in " << r.endTime << " s, " << r.partyTurns << " turns, " << r.aims
                        << " aimed");
        // Without presses the pilot never aims.
        const demo::HeadlessResult idle = demo::playHeadless(demo::party(), demo::enemies(), false, step);
        CHECK(idle.aims == 0);
        CHECK(idle.outcome == Phase::Defeat);
    }
}
