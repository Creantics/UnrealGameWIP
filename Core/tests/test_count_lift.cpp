// The count's Load and the Lift (FightSession def.lift, StrikeCount setLift), and a Release's mark.
#include "ftcore/Chairs.h"
#include "ftcore/FightSession.h"

#include <doctest/doctest.h>

#include <string>

using namespace ftcore;

namespace {

// 60 bpm: the "now" 2 s after the call, window [now - 0.12, now + 0.18], cooldown 1.5 s after it.
FightSessionDef liftDef(bool on = true) {
    FightSessionDef d;
    d.count.bpm = 60.0;
    d.lift = on;
    return d;
}

// Ticks the session to `t` in steps of at most 0.25 s.
void tickTo(FightSession& s, double t) {
    while (s.now() < t - 1e-12) {
        const double step = t - s.now();
        s.tick(step < 0.25 ? step : 0.25);
    }
}

// Calls the count with `caller` and ticks to its "now".
void toNow(FightSession& s, FighterId caller = kLeadA) {
    REQUIRE(s.callCount(caller) == CountError::None);
    tickTo(s, s.strikeCount().nowAt());
}

// Two gauges the test applies the session's Load changes to (as a host would: a store never past the cap).
struct Gauges {
    int load[2] = {0, 0};
    int cap = 100;
    int stores = 0;
    int spends = 0;
    void apply(const FightTick& t) {
        for (const CountLoadChange& c : t.loads) {
            REQUIRE(isLead(c.lead));
            int& l = load[c.lead];
            if (c.kind == CountLoadKind::CountStore) {
                l = l + c.amount < cap ? l + c.amount : cap;
                ++stores;
            } else {
                l -= c.amount;
                ++spends;
            }
        }
    }
};

// Both leads join a window: A's blow first (held), B's second (joins).
CountHit joinBoth(FightSession& s, int baseA = 14, std::uint8_t enemy = 0) {
    const CountHit a = s.judgeHeldBlow(kLeadA, enemy, baseA, 1);
    REQUIRE(a.outcome == CountOutcome::Held);
    return s.judgeHeldBlow(kLeadB, enemy, baseA, 1);
}

} // namespace

TEST_CASE("count lift: a joined window stores countLoad on each lead joiner") {
    FightSession s;
    REQUIRE(s.begin(liftDef(), 0.0).ok());
    Gauges g;
    g.load[0] = 40;
    g.load[1] = 20;
    s.setLoad(kLeadA, g.load[0]);
    s.setLoad(kLeadB, g.load[1]);
    toNow(s);
    CHECK(joinBoth(s).outcome == CountOutcome::OnCount);
    g.apply(s.tick(0.0));
    CHECK(g.load[0] == 50);
    CHECK(g.load[1] == 30);
    CHECK(g.stores == 2);
}

TEST_CASE("count lift: two joiners at the threshold lift the window") {
    FightSession s;
    REQUIRE(s.begin(liftDef(), 0.0).ok());
    Gauges g;
    g.load[0] = 80;
    g.load[1] = 40;
    s.setLoad(kLeadA, 80);
    s.setLoad(kLeadB, 40);
    toNow(s);
    const CountHit b = joinBoth(s, 14);
    CHECK(b.outcome == CountOutcome::Lift);
    CHECK(b.multiplier == doctest::Approx(3.75f));
    CHECK(sharedBlow(b));
    CHECK(s.strikeCount().lifted());
    const FightTick& t = s.tick(0.0);
    // The held blow settles as a Lift too: 14 at base, then round(14 x 2.75) = 39 more, 53 in all.
    REQUIRE(t.bonuses.size() == 1);
    CHECK(14 + t.bonuses[0].extraDamage == 53);
    CHECK(t.sharedBlows == 0); // no lock, no boss: nothing to open
    g.apply(t);
    CHECK(g.load[0] == 50);
    CHECK(g.load[1] == 10);
    CHECK(g.stores == 0); // a Lift stores none
    CHECK(g.spends == 2);
}

TEST_CASE("count lift: one short of the threshold is an ordinary joined count") {
    FightSession s;
    REQUIRE(s.begin(liftDef(), 0.0).ok());
    Gauges g;
    g.load[0] = 80;
    g.load[1] = 39;
    s.setLoad(kLeadA, 80);
    s.setLoad(kLeadB, 39);
    toNow(s);
    const CountHit b = joinBoth(s, 14);
    CHECK(b.outcome == CountOutcome::OnCount);
    CHECK(b.multiplier == doctest::Approx(2.5f));
    CHECK(static_cast<int>(14.f * b.multiplier + 0.5f) == 35);
    g.apply(s.tick(0.0));
    CHECK(g.load[0] == 90);
    CHECK(g.load[1] == 49);
}

TEST_CASE("count lift: a joiner with less than the cost pays what it holds") {
    FightSession s;
    REQUIRE(s.begin(liftDef(), 0.0).ok());
    Gauges g;
    g.load[0] = 99;
    g.load[1] = 21;
    s.setLoad(kLeadA, 99);
    s.setLoad(kLeadB, 21);
    toNow(s);
    CHECK(joinBoth(s).outcome == CountOutcome::Lift);
    g.apply(s.tick(0.0));
    CHECK(g.load[0] == 69);
    CHECK(g.load[1] == 0);
}

TEST_CASE("count lift: a joiner with no gauge counts 0 and stores nothing") {
    FightSession s;
    REQUIRE(s.begin(liftDef(), 0.0).ok());
    Gauges g;
    g.load[0] = 99;
    s.setLoad(kLeadA, 99);
    s.setLoad(kLeadB, -1); // an ally standing in as the second lead: no gauge
    toNow(s);
    CHECK(joinBoth(s).outcome == CountOutcome::OnCount);
    CHECK(s.strikeCount().lastJoin().loadSum == 99);
    const FightTick& t = s.tick(0.0);
    REQUIRE(t.loads.size() == 1);
    CHECK(t.loads[0].lead == kLeadA);
    g.apply(t);
    CHECK(g.load[0] == 100); // +1, to the cap
}

TEST_CASE("count lift: a lead stores from the count at most once per countLoadEvery") {
    FightSession s;
    REQUIRE(s.begin(liftDef(), 0.0).ok());
    s.setLoad(kLeadA, 0);
    s.setLoad(kLeadB, 0);
    toNow(s);
    const double first = s.now();
    joinBoth(s);
    CHECK(s.tick(0.0).loads.size() == 2);

    SUBCASE("7.9 s later: nothing") {
        tickTo(s, first + 7.9 - 2.0);
        toNow(s);
        CHECK(s.now() == doctest::Approx(first + 7.9));
        CHECK(joinBoth(s).outcome == CountOutcome::OnCount);
        CHECK(s.tick(0.0).loads.empty());
    }
    SUBCASE("8.0 s later: both store again") {
        tickTo(s, first + 8.0 - 2.0);
        toNow(s);
        joinBoth(s);
        CHECK(s.tick(0.0).loads.size() == 2);
    }
    SUBCASE("countLoadEvery 0: no limit") {
        FightSessionDef d = liftDef();
        d.liftTuning.countLoadEvery = 0.0;
        REQUIRE(s.begin(d, 0.0).ok());
        s.setLoad(kLeadA, 0);
        s.setLoad(kLeadB, 0);
        toNow(s);
        joinBoth(s);
        CHECK(s.tick(0.0).loads.size() == 2);
        tickTo(s, s.strikeCount().readyAt());
        toNow(s);
        joinBoth(s);
        CHECK(s.tick(0.0).loads.size() == 2);
    }
}

TEST_CASE("count lift: the third slot's count has the caller as the second joiner") {
    FightSession s;
    REQUIRE(s.begin(liftDef(), 0.0).ok());
    s.setLoad(kLeadA, 30);
    REQUIRE(s.selectGift(Gift::Count));
    REQUIRE(s.giveGift(kLeadA).given);
    CHECK(s.strikeCount().caller() == kThirdSlot);
    tickTo(s, s.strikeCount().nowAt());
    const CountHit h = s.judgeHeldBlow(kLeadA, 0, 14, 1);
    CHECK(h.outcome == CountOutcome::OnCount);
    CHECK(h.multiplier == doctest::Approx(2.5f));
    CHECK(s.strikeCount().lastJoin().first == kThirdSlot);
    const FightTick& t = s.tick(0.0);
    REQUIRE(t.loads.size() == 1); // the third slot stores nothing
    CHECK(t.loads[0].lead == kLeadA);
    CHECK(t.loads[0].amount == 10);
}

TEST_CASE("count lift: off (the default), nothing changes") {
    FightSession s;
    REQUIRE(s.begin(liftDef(false), 0.0).ok());
    CHECK_FALSE(s.strikeCount().liftOn());
    s.setLoad(kLeadA, 90);
    s.setLoad(kLeadB, 90);
    toNow(s);
    CHECK(joinBoth(s).outcome == CountOutcome::OnCount);
    CHECK(s.tick(0.0).loads.empty());
    // A third-slot count still waits for a second striker.
    tickTo(s, s.strikeCount().readyAt());
    REQUIRE(s.selectGift(Gift::Count));
    REQUIRE(s.giveGift(kLeadA).given);
    tickTo(s, s.strikeCount().nowAt());
    CHECK(s.judgeHeldBlow(kLeadA, 0, 14, 1).outcome == CountOutcome::Held);
}

TEST_CASE("count lift: a mark adds to the next joined window on its enemy only") {
    FightSession s;
    REQUIRE(s.begin(liftDef(), 0.0).ok());
    s.setLoad(kLeadA, 0);
    s.setLoad(kLeadB, 0);
    REQUIRE(s.markEnemy(0, 0.25f, 8.0));
    CHECK_FALSE(s.markEnemy(kMaxMarkedEnemies, 0.25f, 8.0));
    CHECK_FALSE(s.markEnemy(1, 0.0f, 8.0));
    toNow(s);
    const CountHit b = joinBoth(s, 14, 0);
    CHECK(b.multiplier == doctest::Approx(2.5f * 1.25f));
    const FightTick& t = s.tick(0.0);
    REQUIRE(t.bonuses.size() == 1); // the held blow takes the mark at its settle
    CHECK(t.bonuses[0].extraDamage == 30); // round(14 x 2.125) = 29.75
    CHECK(s.judgeHeldBlow(kLeadA, 1, 14, 1).multiplier == doctest::Approx(2.5f)); // another enemy: no mark
    CHECK(s.mark(0).used);
    // The window over, the mark is gone.
    tickTo(s, s.strikeCount().windowEnd() + 0.1);
    CHECK_FALSE(s.mark(0).bonus > 0.f);
}

TEST_CASE("count lift: a Lift on a marked enemy, the mark at its cap") {
    FightSession s;
    REQUIRE(s.begin(liftDef(), 0.0).ok());
    s.setLoad(kLeadA, 80);
    s.setLoad(kLeadB, 40);
    REQUIRE(s.markEnemy(2, 0.9f, 8.0)); // clamped to 0.5
    CHECK(s.mark(2).bonus == doctest::Approx(kMaxReleaseMark));
    toNow(s);
    CHECK(joinBoth(s, 14, 2).multiplier == doctest::Approx(5.625f));
}

TEST_CASE("count lift: an unused mark ends at its time") {
    FightSession s;
    REQUIRE(s.begin(liftDef(), 0.0).ok());
    REQUIRE(s.markEnemy(0, 0.25f, 1.0));
    tickTo(s, 1.5);
    CHECK_FALSE(s.mark(0).bonus > 0.f);
    s.setLoad(kLeadA, 0);
    s.setLoad(kLeadB, 0);
    toNow(s);
    CHECK(joinBoth(s).multiplier == doctest::Approx(2.5f));
}

TEST_CASE("count lift: the tuning keeps the hardest blow at x10 or under") {
    const CountTuning c;
    LiftTuning l;
    CHECK(validLiftTuning(l, c));
    l.liftMultiplier = 2.6f; // 2.5 x 2.6 x 1.5 = 9.75
    CHECK(validLiftTuning(l, c));
    l.liftMultiplier = 2.7f; // out of range, and 10.125 over the product
    CHECK_FALSE(validLiftTuning(l, c));
    CountTuning hard = c;
    hard.onCountMultiplier = 3.0f; // 3 x 2.6 x 1.5 = 11.7: in every range, over the product
    l.liftMultiplier = 2.6f;
    CHECK_FALSE(validLiftTuning(l, hard));
    CountTuning silenced = c;
    silenced.silencedMultiplier = 3.0f;
    CHECK_FALSE(validLiftTuning(l, silenced));
    LiftTuning r;
    r.countLoad = 51;
    CHECK_FALSE(validLiftTuning(r, c));
    r = LiftTuning{};
    r.countLoadEvery = 30.5;
    CHECK_FALSE(validLiftTuning(r, c));
    r = LiftTuning{};
    r.liftThreshold = 0;
    CHECK_FALSE(validLiftTuning(r, c));
    r = LiftTuning{};
    r.liftCost = 101;
    CHECK_FALSE(validLiftTuning(r, c));

    FightSession s;
    FightSessionDef d = liftDef();
    d.liftTuning.liftMultiplier = 2.7f;
    const FightSessionCheck k = s.begin(d, 0.0);
    CHECK_FALSE(k.lift);
    CHECK_FALSE(s.strikeCount().liftOn());
}

TEST_CASE("count lift: a lead at its cap joins only with a Release, which has spent its Load") {
    FightSession s;
    REQUIRE(s.begin(liftDef(), 0.0).ok());
    s.setLoad(kLeadA, 100);
    toNow(s);
    REQUIRE(s.judgeHeldBlow(kLeadB, 0, 14, 1).outcome == CountOutcome::Held);
    s.setLoad(kLeadA, 0); // the Release spent it at its start
    s.setLoad(kLeadB, 90);
    CHECK(s.judgeHeldBlow(kLeadA, 0, 14, 1).outcome == CountOutcome::OnCount);
    CHECK(s.strikeCount().lastJoin().loadSum == 90);
}

TEST_CASE("chairs: releaseMark reads, sums to its cap and keeps the longest seconds") {
    ChairTable t;
    std::string err;
    REQUIRE(importChairTable(R"({ "version": 1, "chairs": [
        { "person": 1, "key": "chair_m1", "kind": "releaseMark", "a": 0.25, "b": 8 },
        { "person": 2, "key": "chair_m2", "kind": "releaseMark", "a": 0.4, "b": 12 } ] })",
                             t, &err));
    REQUIRE(t.rows.size() == 2);
    CHECK(std::string(chairKindName(ChairKind::ReleaseMark)) == "releaseMark");
    ArenaChairs c;
    c.named[0] = t.rows[0].effect;
    c.count = 1;
    CHECK(c.releaseMark() == doctest::Approx(0.25f));
    CHECK(c.releaseMarkSeconds() == doctest::Approx(8.f));
    c.named[1] = t.rows[1].effect;
    c.count = 2;
    CHECK(c.releaseMark() == doctest::Approx(kChairReleaseMarkCap));
    CHECK(c.releaseMarkSeconds() == doctest::Approx(12.f));
    ChairTable bad;
    CHECK_FALSE(importChairTable(R"({ "version": 1, "chairs": [
        { "person": 1, "key": "chair_m1", "kind": "releaseMark", "a": 0.6, "b": 8 } ] })",
                                 bad, &err));
    CHECK_FALSE(importChairTable(R"({ "version": 1, "chairs": [
        { "person": 1, "key": "chair_m1", "kind": "releaseMark", "a": 0.2, "b": 0.5 } ] })",
                                 bad, &err));
}
