// The count's Load and the Lift (FightSession def.lift, StrikeCount setLift), and a Release's mark.
#include "ftcore/Chairs.h"
#include "ftcore/CountLiftData.h"
#include "ftcore/FightSession.h"
#include "ftcore/GiftSlot.h"

#include "TestAssets.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
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
    CHECK(t.loads[0].amount == 5); // counting alone stores the smaller solo amount
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

namespace {

FightSessionDef keptDef(int named = 1) {
    FightSessionDef d = liftDef();
    d.keptLead = kLeadB;
    d.keptNamed = named;
    return d;
}

} // namespace

TEST_CASE("count lift: the kept gauge fills from parries, blocks and joined counts, capped by its chairs") {
    FightSession s;
    REQUIRE(s.begin(keptDef(), 0.0).ok());
    CHECK(s.keepsLoad(kLeadB));
    CHECK_FALSE(s.keepsLoad(kLeadA));
    CHECK(s.keptLead() == kLeadB);
    CHECK(s.keptCap() == 60); // 40 + 20 x 1 Named
    CHECK(s.keptLoad() == 0);
    CHECK(s.keptParried(22) == 22);
    CHECK(s.keptBlocked(11) == 3); // rnd(2.75)
    CHECK(s.keptBlocked(0) == 0);
    FightTick t = s.tick(0.0);
    REQUIRE(t.loads.size() == 2);
    CHECK(t.loads[0].kind == CountLoadKind::ParryStore);
    CHECK(t.loads[0].amount == 22);
    CHECK(t.loads[0].kept);
    CHECK(t.loads[1].kind == CountLoadKind::BlockStore);
    CHECK(t.loads[1].amount == 3);

    s.setLoad(kLeadA, 40);
    toNow(s);
    CHECK(joinBoth(s).outcome == CountOutcome::OnCount);
    CHECK(s.keptLoad() == 35);
    t = s.tick(0.0);
    REQUIRE(t.loads.size() == 2);
    CHECK(t.loads[0].lead == kLeadA);
    CHECK_FALSE(t.loads[0].kept); // the host's gauge: the host applies it
    CHECK(t.loads[1].lead == kLeadB);
    CHECK(t.loads[1].kept);
    CHECK(t.loads[1].amount == 10);

    CHECK(s.keptParried(100) == 25); // to the cap
    CHECK(s.keptLoad() == 60);
    CHECK(s.keptParried(5) == 0);
    s.setLoad(kLeadB, 99); // a host override is clamped to the cap
    CHECK(s.keptLoad() == 60);
    s.setLoad(kLeadB, -5);
    CHECK(s.keptLoad() == 0);
}

TEST_CASE("count lift: the kept gauge counts toward a Lift and pays it") {
    FightSession s;
    REQUIRE(s.begin(keptDef(3), 0.0).ok());
    CHECK(s.keptCap() == 100);
    CHECK(s.keptParried(40) == 40);
    s.setLoad(kLeadA, 80);
    toNow(s);
    const CountHit h = joinBoth(s);
    CHECK(h.outcome == CountOutcome::Lift);
    CHECK(s.keptLoad() == 10);
    const FightTick t = s.tick(0.0);
    int spentA = 0, spentB = 0;
    for (const CountLoadChange& c : t.loads) {
        if (c.kind != CountLoadKind::LiftSpend) continue;
        if (c.lead == kLeadA) {
            CHECK_FALSE(c.kept);
            spentA += c.amount;
        } else {
            CHECK(c.kept);
            spentB += c.amount;
        }
    }
    CHECK(spentA == 30);
    CHECK(spentB == 30);
}

TEST_CASE("count lift: the kept gauge's caps and refusals") {
    FightSession s;
    REQUIRE(s.begin(keptDef(0), 0.0).ok());
    CHECK(s.keptCap() == 40);
    REQUIRE(s.begin(keptDef(4), 0.0).ok());
    CHECK(s.keptCap() == 100); // 120 clamped

    FightSessionDef bad = keptDef();
    bad.keptLead = kThirdSlot;
    FightSessionCheck c = s.begin(bad, 0.0);
    CHECK_FALSE(c.kept);
    CHECK_FALSE(c.ok());
    CHECK_FALSE(s.keepsLoad(kThirdSlot));
    CHECK(s.keptParried(20) == 0);

    bad = keptDef(9);
    CHECK_FALSE(s.begin(bad, 0.0).kept);
    bad = keptDef();
    bad.keptBlockShare = 1.5f;
    CHECK_FALSE(s.begin(bad, 0.0).kept);

    FightSessionDef off = keptDef();
    off.lift = false; // no Lift: no kept gauge
    REQUIRE(s.begin(off, 0.0).ok());
    CHECK_FALSE(s.keepsLoad(kLeadB));
    CHECK(s.keptParried(20) == 0);
    CHECK(s.keptCap() == 0);
}

TEST_CASE("count lift: the blow cap keeps the worst stack (seam crit x Lift x mark) at 6x") {
    // Seam crit x2, a Lift (x2.5 count x1.5) on an enemy marked at its cap (x1.5): 11.25 before the cap.
    const float worst = 2.0f * 2.5f * 1.5f * 1.5f;
    CHECK(worst == doctest::Approx(11.25f));
    CHECK(capBlowScale(worst, kDefaultBlowCap) == doctest::Approx(6.0f));
    CHECK(capBlowScale(worst, 0.f) == doctest::Approx(11.25f)); // 0: no cap
    CHECK(capBlowScale(3.0f, kDefaultBlowCap) == doctest::Approx(3.0f));
    CHECK(validBlowCap(0.f));
    CHECK(validBlowCap(6.f));
    CHECK_FALSE(validBlowCap(0.5f));
    CHECK_FALSE(validBlowCap(21.f));

    // Through the session: a final Lift on a marked enemy, the host's crit x2 on top, kept at the cap.
    FightSessionDef d = liftDef();
    d.blowCap = kDefaultBlowCap;
    FightSession s;
    REQUIRE(s.begin(d, 0.0).ok());
    CHECK(s.blowCap() == doctest::Approx(6.0f));
    s.setLoad(kLeadA, 80);
    s.setLoad(kLeadB, 40);
    REQUIRE(s.markEnemy(2, 0.5f, 8.0));
    toNow(s);
    // A held at x2 (its crit), base 14 before the crit; B's blow lifts the window.
    REQUIRE(s.judgeHeldBlow(kLeadA, 2, 14, 1, 2.0f).outcome == CountOutcome::Held);
    const CountHit b = s.judgeHeldBlow(kLeadB, 2, 14, 1, 2.0f);
    CHECK(b.outcome == CountOutcome::Lift);
    CHECK(b.multiplier == doctest::Approx(5.625f));
    CHECK(s.capBlow(2.0f * b.multiplier) == doctest::Approx(6.0f));
    // A's settle: from the base before the crit, under the cap: 14 x (6 - 2) = 56, not 14 x 2 x 4.625.
    const FightTick& t = s.tick(0.0);
    REQUIRE(t.bonuses.size() == 1);
    CHECK(t.bonuses[0].extraDamage == 56);
}

TEST_CASE("count lift: a held settle reaches the same total as an immediate blow") {
    // One rule, the product: base x capBlowScale(landed x mult, cap) in all; the bonus is what is past the landing.
    // Uncapped: 14 x 2 x 2.5 = 70 in all, 28 landed, 42 more.
    CHECK(heldSettleBonus(14, 2.0f, 2.5f, 0.f) == 42);
    // Capped with room (x5 under x6): the same.
    CHECK(heldSettleBonus(14, 2.0f, 2.5f, 6.f) == 42);
    // Capped: x11.25 kept to x6, 84 in all: 56 more.
    CHECK(heldSettleBonus(14, 2.0f, 5.625f, 6.f) == 56);
    // Held and immediate agree, for a sweep of crits and boosts under the cap.
    for (const float crit : {1.0f, 1.5f, 2.0f}) {
        for (const float mult : {1.25f, 2.5f, 3.75f, 5.625f}) {
            const int immediate = static_cast<int>(std::lround(14.0 * capBlowScale(crit * mult, kDefaultBlowCap)));
            const int held = static_cast<int>(std::lround(14.0 * crit)) + heldSettleBonus(14, crit, mult, kDefaultBlowCap);
            CHECK(std::abs(immediate - held) <= 1); // rounding only
        }
    }
    // A crit already at the cap: nothing more.
    CHECK(heldSettleBonus(14, 6.0f, 2.5f, 6.f) == 0);
    CHECK(heldSettleBonus(14, 1.0f, 1.0f, 6.f) == 0);
    CHECK(heldSettleBonus(0, 1.0f, 2.5f, 6.f) == 0);

    // The default (no cap): a held blow's record is its base as given, as before.
    FightSession s;
    REQUIRE(s.begin(liftDef(), 0.0).ok());
    CHECK(s.blowCap() == 0.f);
    CHECK(s.capBlow(11.25f) == doctest::Approx(11.25f));
    s.setLoad(kLeadA, 0);
    s.setLoad(kLeadB, 0);
    toNow(s);
    joinBoth(s, 28);
    const FightTick& t = s.tick(0.0);
    REQUIRE(t.bonuses.size() == 1);
    CHECK(t.bonuses[0].extraDamage == 42); // 28 x 1.5
}

TEST_CASE("count lift: a refused blow cap is no cap") {
    FightSessionDef d = liftDef();
    d.blowCap = 0.5f;
    FightSession s;
    const FightSessionCheck c = s.begin(d, 0.0);
    CHECK_FALSE(c.cap);
    CHECK_FALSE(c.ok());
    CHECK(s.blowCap() == 0.f);
}

TEST_CASE("count lift: counting alone through the third slot stores the solo amount") {
    FightSessionDef d = liftDef();
    d.liftTuning.soloCountLoad = 0;
    FightSession s;
    REQUIRE(s.begin(d, 0.0).ok());
    s.setLoad(kLeadA, 30);
    REQUIRE(s.selectGift(Gift::Count));
    REQUIRE(s.giveGift(kLeadA).given);
    tickTo(s, s.strikeCount().nowAt());
    CHECK(s.judgeHeldBlow(kLeadA, 0, 14, 1).outcome == CountOutcome::OnCount);
    CHECK(s.tick(0.0).loads.empty()); // solo 0: calling alone stores nothing
    LiftTuning l;
    CHECK(l.soloCountLoad == 5);
    CHECK(l.soloCountLoad < l.countLoad);
    l.soloCountLoad = 51;
    CHECK_FALSE(validLiftTuning(l, CountTuning{}));
}

TEST_CASE("count lift: a shared-blow lock opens only with a source") {
    LockTuning shared;
    shared.keys = kUnlockSharedBlow;
    CHECK_FALSE(lockCanOpen(shared, SharedBlowSources{}));
    SharedBlowSources s;
    s.takeHalf = true;
    CHECK(lockCanOpen(shared, s));
    s = {};
    s.tableShare = true;
    CHECK(lockCanOpen(shared, s));
    s = {};
    s.sharedCount = true;
    CHECK(lockCanOpen(shared, s));
    LockTuning refusal;
    refusal.keys = kUnlockRefusal;
    CHECK(lockCanOpen(refusal, SharedBlowSources{}));

    // The session's lock takes a take half's (or a table share's) shared blow with no count at all.
    FightSessionDef d = liftDef();
    d.locked = true;
    d.lock.keys = kUnlockSharedBlow;
    d.lock.openSeconds = 6.0;
    FightSession f;
    REQUIRE(f.begin(d, 0.0).ok());
    CHECK_FALSE(f.weakPointOpen());
    CHECK(f.offerKey(kUnlockSharedBlow) == UnlockResult::Unlocked);
    CHECK(f.weakPointOpen());
    tickTo(f, 6.5);
    CHECK_FALSE(f.weakPointOpen());
}

TEST_CASE("count lift: count_lift.json reads, keeps defaults for missing fields and refuses bad ones") {
    CountLiftData d;
    std::string err;
    REQUIRE(importCountLift(R"({ "version": 1, "soloCountLoad": 4, "blowCap": 5.5 })", d, &err));
    CHECK(d.lift.soloCountLoad == 4);
    CHECK(d.lift.countLoad == 10);
    CHECK(d.blowCap == doctest::Approx(5.5f));
    REQUIRE(importCountLift(R"({ "version": 1 })", d, &err));
    CHECK(d.blowCap == doctest::Approx(kDefaultBlowCap));
    CHECK(d.lift.soloCountLoad == 5);
    CHECK_FALSE(importCountLift(R"({ "version": 2 })", d, &err));
    CHECK_FALSE(importCountLift(R"({ "version": 1, "blowCap": 0.5 })", d, &err));
    CHECK(err.find("blowCap") != std::string::npos);
    CHECK(d.blowCap == doctest::Approx(kDefaultBlowCap)); // refused: the defaults
    CHECK_FALSE(importCountLift(R"({ "version": 1, "soloCountLoad": 2.5 })", d, &err));
    CHECK_FALSE(importCountLift(R"({ "version": 1, "liftCost": 101 })", d, &err));
    CHECK_FALSE(importCountLift(R"({ "version": 1, "extra": 1 })", d, &err));
    CHECK_FALSE(importCountLift("[]", d, &err));
}

TEST_CASE("count lift: a down partner stores, spends and counts nothing") {
    FightSession s;
    REQUIRE(s.begin(keptDef(3), 0.0).ok());
    CHECK(s.keptParried(50) == 50);
    s.setKeptDown(true);
    CHECK(s.keptDown());
    CHECK(s.keptParried(20) == 0);
    CHECK(s.keptBlocked(40) == 0);
    s.setLoad(kLeadA, 100); // 100 + its 50 would lift; down, it counts 0
    toNow(s);
    const CountHit h = joinBoth(s);
    CHECK(h.outcome == CountOutcome::OnCount);
    const FightTick t = s.tick(0.0);
    for (const CountLoadChange& c : t.loads) CHECK_FALSE(c.kept); // no store, no spend
    CHECK(s.keptLoad() == 50);
    // The next begin has it up again.
    REQUIRE(s.begin(keptDef(3), 0.0).ok());
    CHECK_FALSE(s.keptDown());
}

TEST_CASE("count lift: a kept joiner holding under the cost pays what it holds") {
    FightSession s;
    REQUIRE(s.begin(keptDef(3), 0.0).ok());
    CHECK(s.keptParried(20) == 20);
    s.setLoad(kLeadA, 100);
    toNow(s);
    CHECK(joinBoth(s).outcome == CountOutcome::Lift); // 100 + 20 = 120
    const FightTick t = s.tick(0.0);
    int keptSpent = 0, leadSpent = 0;
    for (const CountLoadChange& c : t.loads) {
        if (c.kind != CountLoadKind::LiftSpend) continue;
        (c.kept ? keptSpent : leadSpent) += c.amount;
    }
    CHECK(keptSpent == 20);
    CHECK(leadSpent == 30);
    CHECK(s.keptLoad() == 0);
}

TEST_CASE("lone carry: the session's stance starts Lone, refuses the table and ends only on a given gift") {
    FightSessionDef d = liftDef();
    d.loneLead = kLeadA;
    FightSession s;
    REQUIRE(s.begin(d, 0.0).ok());
    CHECK(s.lone());
    CHECK(s.loneLead() == kLeadA);
    Load load;
    load.value = 100;
    CHECK(s.setTable(kLeadA, load, 3) == TableError::Lone);
    CHECK(s.setTable(kLeadB, load, 3) == TableError::Lone);
    CHECK(s.setTable(kThirdSlot, load, 3) == TableError::Lone);
    CHECK(load.value == 100);

    // A press refused as Vacant or Hostile leaves it Lone.
    REQUIRE(s.selectGift(Gift::Heal));
    s.giftSlot().setState(SlotState::Vacant);
    CHECK_FALSE(s.giveGift(kLeadA).given);
    CHECK(s.lone());
    s.giftSlot().setState(SlotState::Hostile);
    CHECK_FALSE(s.giveGift(kLeadA).given);
    CHECK(s.lone());
    // Given: the gift lands, and the stance is over for the fight.
    s.giftSlot().setState(SlotState::Companion);
    const GiftResult g = s.giveGift(kLeadA);
    CHECK(g.given);
    CHECK(g.gift == Gift::Heal);
    CHECK_FALSE(s.lone());
    CHECK(s.loneEnded());
    CHECK(s.loneLead() == kNoFighter);
    CHECK(s.setTable(kLeadA, load, 3) != TableError::Lone);
    // A press refused while cooling changes nothing either way.
    CHECK_FALSE(s.giveGift(kLeadA).given);
    CHECK_FALSE(s.lone());
    // begin makes it Lone again.
    REQUIRE(s.begin(d, 0.0).ok());
    CHECK(s.lone());
    CHECK_FALSE(s.loneEnded());
}

TEST_CASE("lone carry: a Count gift refused while a count is live leaves it Lone") {
    FightSessionDef d = liftDef();
    d.loneLead = kLeadA;
    FightSession s;
    REQUIRE(s.begin(d, 0.0).ok());
    REQUIRE(s.callCount(kLeadA) == CountError::None);
    REQUIRE(s.selectGift(Gift::Count));
    const GiftResult r = s.giveGift(kLeadA);
    CHECK_FALSE(r.given);
    CHECK(r.error == GiftError::CountRefused);
    CHECK(s.lone());
    CHECK_FALSE(s.loneEnded());
}

TEST_CASE("lone carry: a refused stance is nobody Lone") {
    FightSessionDef d = liftDef();
    d.loneLead = kThirdSlot;
    FightSession s;
    FightSessionCheck c = s.begin(d, 0.0);
    CHECK_FALSE(c.lone);
    CHECK_FALSE(c.ok());
    CHECK_FALSE(s.lone());
    d.loneLead = kLeadB;
    d.loneTuning.taken = 2.5f;
    c = s.begin(d, 0.0);
    CHECK_FALSE(c.lone);
    CHECK_FALSE(s.lone());
    d.loneTuning = LoneTuning{};
    REQUIRE(s.begin(d, 0.0).ok());
    CHECK(s.loneLead() == kLeadB);
}

TEST_CASE("lone carry: nobody joins a Lone lead's count; the call alone lands light") {
    FightSessionDef d = liftDef();
    d.loneLead = kLeadA;
    FightSession s;
    REQUIRE(s.begin(d, 0.0).ok());
    s.setLoad(kLeadA, 80);
    s.setLoad(kLeadB, 80);
    toNow(s);
    REQUIRE(s.judgeHeldBlow(kLeadA, 0, 14, 1).outcome == CountOutcome::Held);
    const CountHit b = s.judgeHeldBlow(kLeadB, 0, 14, 1);
    CHECK(b.outcome == CountOutcome::Alone);
    CHECK(b.multiplier == doctest::Approx(1.0f));
    bool unanswered = false;
    int lifts = 0;
    for (int k = 0; k < 8; ++k) {
        const FightTick& t = s.tick(0.25);
        CHECK(t.loads.empty()); // no joined count: nothing stored, no Lift spent
        CHECK(t.sharedBlows == 0);
        for (const CountEvent& e : *t.count) {
            if (e.type == CountEventType::BlowSettled && e.outcome == CountOutcome::Unanswered) {
                unanswered = true;
                CHECK(e.multiplier == doctest::Approx(0.5f));
            }
            if (e.type == CountEventType::BlowSettled && e.outcome == CountOutcome::Lift) ++lifts;
        }
    }
    CHECK(unanswered);
    CHECK(lifts == 0);
}

TEST_CASE("lone carry: Releases pool while Lone, saturating; none after a gift ends it") {
    FightSessionDef d = liftDef();
    d.loneLead = kLeadA;
    FightSession s;
    REQUIRE(s.begin(d, 0.0).ok());
    CHECK(s.poolRelease(kLeadA, 100));
    CHECK_FALSE(s.poolRelease(kLeadB, 50)); // not the Lone lead
    CHECK_FALSE(s.poolRelease(kLeadA, 0));
    CHECK(s.lonePooled() == 100u);
    LoneSave save;
    save.pooled = 4294967200u;
    CHECK(poolLoneRelease(save, 100) == 4294967295u);
    CHECK(poolLoneRelease(save, 1) == 4294967295u);
    LoneSave fresh;
    CHECK(poolLoneRelease(fresh, static_cast<int>(s.lonePooled())) == 100u);
    REQUIRE(s.selectGift(Gift::Guard));
    REQUIRE(s.giveGift(kLeadA).given);
    CHECK_FALSE(s.poolRelease(kLeadA, 60));
    CHECK(s.lonePooled() == 100u);
    REQUIRE(s.begin(d, 0.0).ok());
    CHECK(s.lonePooled() == 0u);
}

TEST_CASE("lone carry: forced when the third slot is Vacant or Hostile; the worst Lone blow fits the cap") {
    CHECK_FALSE(loneForced(SlotState::Companion));
    CHECK(loneForced(SlotState::Vacant));
    CHECK(loneForced(SlotState::Hostile));
    // A crit x2 on a Release at the largest release scale (3): 6, inside the default blow cap.
    CHECK(capBlowScale(2.0f * 3.0f, kDefaultBlowCap) == doctest::Approx(6.0f));
    LoneTuning t;
    CHECK(validLoneTuning(t));
    t.cap = 0;
    CHECK_FALSE(validLoneTuning(t));
}

TEST_CASE("lone carry: lone_carry.json reads, keeps defaults for missing fields and refuses bad ones") {
    LoneTuning t;
    std::string err;
    REQUIRE(importLoneCarry(R"({ "version": 1, "cap": 90, "taken": 1.2 })", t, &err));
    CHECK(t.cap == 90);
    CHECK(t.taken == doctest::Approx(1.2f));
    CHECK(t.gain == doctest::Approx(1.5f));
    REQUIRE(importLoneCarry(R"({ "version": 1 })", t, &err));
    CHECK(t.cap == 100);
    CHECK_FALSE(importLoneCarry(R"({ "version": 2 })", t, &err));
    CHECK_FALSE(importLoneCarry(R"({ "version": 1, "cap": 101 })", t, &err));
    CHECK(err.find("cap") != std::string::npos);
    CHECK_FALSE(importLoneCarry(R"({ "version": 1, "cap": 50.5 })", t, &err));
    CHECK_FALSE(importLoneCarry(R"({ "version": 1, "gain": 0.9 })", t, &err));
    CHECK_FALSE(importLoneCarry(R"({ "version": 1, "release": "x" })", t, &err));
    CHECK_FALSE(importLoneCarry(R"({ "version": 1, "extra": 1 })", t, &err));
    CHECK(t.cap == 100); // refused: the defaults
}

TEST_CASE("lone carry: a host can ask before begin whether the stance will be accepted") {
    CHECK(loneStanceAccepted(kLeadA, LoneTuning{}));
    CHECK(loneStanceAccepted(kLeadB, LoneTuning{}));
    CHECK_FALSE(loneStanceAccepted(kThirdSlot, LoneTuning{}));
    LoneTuning bad;
    bad.taken = 2.5f;
    CHECK_FALSE(loneStanceAccepted(kLeadA, bad));
    // The session agrees with the host's answer.
    for (const FighterId lead : {kLeadA, kThirdSlot}) {
        FightSessionDef d;
        d.loneLead = lead;
        FightSession s;
        s.begin(d, 0.0);
        CHECK(s.lone() == loneStanceAccepted(lead, d.loneTuning));
    }
}

TEST_CASE("gifts: gifts.json is the defaults; the importer keeps defaults for missing fields and refuses bad ones") {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Battle/gifts.json", text));
    GiftTuning t;
    t.healsPerFight = 9;
    std::string err;
    REQUIRE(importGiftTuning(text, t, &err));
    const GiftTuning d;
    CHECK(t.cooldown == d.cooldown);
    CHECK(t.healShare == d.healShare);
    CHECK(t.healLoad == d.healLoad);
    CHECK(t.healsPerFight == d.healsPerFight);
    CHECK(t.guardScale == d.guardScale);
    CHECK(t.guardSeconds == d.guardSeconds);

    REQUIRE(importGiftTuning(R"({"version":1,"healsPerFight":3,"guardCooldown":10})", t, &err));
    CHECK(t.healsPerFight == 3);
    CHECK(t.cooldown[1] == 10.0);
    CHECK(t.cooldown[0] == d.cooldown[0]);
    CHECK(t.guardScale == d.guardScale);
    REQUIRE(importGiftTuning(R"({"version":1,"healCooldown":60,"guardCooldown":0,"countCooldown":0,"healShare":1,
        "healLoad":0,"healsPerFight":20,"guardScale":0,"guardSeconds":10})", t, &err));
    CHECK(t.cooldown[0] == 60.0);
    CHECK(t.healShare == 1.f);
    CHECK(t.healsPerFight == 20);
    CHECK(t.guardScale == 0.f);
    CHECK(t.guardSeconds == 10.0);

    for (const char* bad : {
             R"({"version":2})",
             R"({})",
             R"([])",
             R"({"version":1,"extra":1})",
             R"({"version":1,"healCooldown":-1})",
             R"({"version":1,"healCooldown":61})",
             R"({"version":1,"guardCooldown":-1})",
             R"({"version":1,"guardCooldown":61})",
             R"({"version":1,"countCooldown":-1})",
             R"({"version":1,"countCooldown":61})",
             R"({"version":1,"healShare":-0.1})",
             R"({"version":1,"healShare":1.1})",
             R"({"version":1,"healLoad":-1})",
             R"({"version":1,"healLoad":101})",
             R"({"version":1,"healLoad":2.5})",
             R"({"version":1,"healsPerFight":-1})",
             R"({"version":1,"healsPerFight":21})",
             R"({"version":1,"healsPerFight":1.5})",
             R"({"version":1,"guardScale":-0.1})",
             R"({"version":1,"guardScale":1.1})",
             R"({"version":1,"guardSeconds":-1})",
             R"({"version":1,"guardSeconds":11})",
             R"({"version":1,"guardSeconds":"x"})",
         }) {
        CAPTURE(bad);
        t.healsPerFight = 9;
        CHECK_FALSE(importGiftTuning(bad, t, &err));
        CHECK_FALSE(err.empty());
        CHECK(t.healsPerFight == d.healsPerFight); // refused: the defaults
    }
}
