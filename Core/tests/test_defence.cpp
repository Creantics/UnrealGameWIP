#include "ftcore/Battle.h"
#include "ftcore/BattleDemo.h"
#include "ftcore/DefenceTimeline.h"
#include "AllocCounter.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

using namespace ftcore;

namespace {

constexpr double kStart = 10.0;

AttackTimeline timeline(std::initializer_list<TimelineHit> hits) {
    AttackTimeline t;
    t.clear();
    for (const TimelineHit& h : hits) t.add(h);
    return t;
}

TimelineHit hitAt(double at, bool jumpable = false, float weight = 1.0f) { return {at, 0.4, 0.0, weight, jumpable}; }

// One plain hit, impact at 11.0.
DefenceTimeline single(bool jumpable = false) {
    DefenceTimeline d;
    d.reset(timeline({hitAt(1.0, jumpable)}), kStart);
    return d;
}

} // namespace

TEST_CASE("defence: a skill without timeline data gets the default single hit") {
    const Skill legacy;
    REQUIRE(legacy.timeline.size() == 1);
    CHECK(legacy.timeline[0].at == kDefaultImpact);
    CHECK(legacy.timeline[0].tell == kDefaultTell);
    CHECK(legacy.timeline[0].telegraph == 0.0);
    CHECK(legacy.timeline[0].weight == 1.0f);
    CHECK_FALSE(legacy.timeline[0].jumpable);

    DefenceTimeline d;
    CHECK(d.complete()); // nothing until reset
    d.reset(legacy.timeline, kStart);
    REQUIRE(d.size() == 1);
    CHECK(d.impact(0) == doctest::Approx(kStart + kDefaultImpact));
    CHECK_FALSE(d.complete());
    // The tell runs from 0.4 s before the impact up to it.
    CHECK_FALSE(d.tellActive(0, d.impact(0) - kDefaultTell - 0.01));
    CHECK(d.tellActive(0, d.impact(0) - kDefaultTell + 0.01));
    CHECK_FALSE(d.tellActive(0, d.impact(0)));
    CHECK_FALSE(d.telegraphActive(0, d.impact(0) - 0.1)); // no telegraph by default
}

TEST_CASE("defence: parry window edges") {
    SUBCASE("just inside") {
        DefenceTimeline d = single();
        const DefenceResults r = d.press(DefenceInput::Parry, 11.0 - 0.11);
        REQUIRE(r.size() == 1);
        CHECK(r[0].outcome == DefenceOutcome::Perfect);
        CHECK(r[0].refund);
        CHECK(d.complete());
        CHECK(d.allParried());
    }
    SUBCASE("just outside (too early): a miss that locks parry out") {
        DefenceTimeline d = single();
        CHECK(d.press(DefenceInput::Parry, 11.0 - 0.13).empty());
        CHECK(d.misses() == 1);
        CHECK(d.lockedOut(DefenceInput::Parry, 11.0 - 0.05));
        CHECK(d.outcome(0) == DefenceOutcome::Pending);
    }
    SUBCASE("at the impact is too late") {
        DefenceTimeline d = single();
        const DefenceResults r = d.press(DefenceInput::Parry, 11.0);
        REQUIRE(r.size() == 1);
        CHECK(r[0].outcome == DefenceOutcome::Hit);
        CHECK(r[0].time == doctest::Approx(11.0));
        CHECK(d.complete());
        CHECK_FALSE(d.allParried());
    }
    SUBCASE("after the impact the hit already landed") {
        DefenceTimeline d = single();
        CHECK(d.update(11.05).size() == 1);
        CHECK(d.outcome(0) == DefenceOutcome::Hit);
        CHECK(d.press(DefenceInput::Parry, 11.06).empty()); // nothing left to answer
    }
}

TEST_CASE("defence: dodge window edges and the perfect dodge") {
    SUBCASE("just inside the wide window: dodged, no refund") {
        DefenceTimeline d = single();
        const DefenceResults r = d.press(DefenceInput::Dodge, 11.0 - 0.29);
        REQUIRE(r.size() == 1);
        CHECK(r[0].outcome == DefenceOutcome::Dodged);
        CHECK_FALSE(r[0].refund);
    }
    SUBCASE("just outside the wide window: a miss without lockout") {
        DefenceTimeline d = single();
        CHECK(d.press(DefenceInput::Dodge, 11.0 - 0.31).empty());
        CHECK_FALSE(d.lockedOut(DefenceInput::Dodge, 11.0 - 0.30));
        const DefenceResults r = d.press(DefenceInput::Dodge, 11.0 - 0.2);
        REQUIRE(r.size() == 1);
        CHECK(r[0].outcome == DefenceOutcome::Dodged);
    }
    SUBCASE("inside the last 0.06 s: a perfect dodge refunds") {
        DefenceTimeline d = single();
        const DefenceResults r = d.press(DefenceInput::Dodge, 11.0 - 0.05);
        REQUIRE(r.size() == 1);
        CHECK(r[0].outcome == DefenceOutcome::Dodged);
        CHECK(r[0].refund);
        CHECK(d.refunds() == 1);
        CHECK_FALSE(d.allParried()); // dodges don't enable the counter
    }
    SUBCASE("just before the perfect part: no refund") {
        DefenceTimeline d = single();
        const DefenceResults r = d.press(DefenceInput::Dodge, 11.0 - 0.07);
        REQUIRE(r.size() == 1);
        CHECK_FALSE(r[0].refund);
    }
}

TEST_CASE("defence: a missed parry locks parry out; mashing dodge is fine") {
    SUBCASE("parry mash") {
        DefenceTimeline d = single();
        CHECK(d.press(DefenceInput::Parry, 10.5).empty()); // too early: lockout until 10.9
        CHECK(d.press(DefenceInput::Parry, 10.89).empty()); // inside the window, but locked out
        CHECK(d.ignored() == 1);
        CHECK(d.misses() == 1);
        CHECK(d.outcome(0) == DefenceOutcome::Pending);
        // Dodge isn't affected by the parry lockout.
        CHECK_FALSE(d.lockedOut(DefenceInput::Dodge, 10.89));
        const DefenceResults r = d.press(DefenceInput::Parry, 10.95); // lockout over
        REQUIRE(r.size() == 1);
        CHECK(r[0].outcome == DefenceOutcome::Perfect);
    }
    SUBCASE("a mashed parry that stays locked out lets the hit land") {
        DefenceTimeline d = single();
        d.press(DefenceInput::Parry, 10.62);
        d.press(DefenceInput::Parry, 10.95); // still locked (until 11.02)
        CHECK(d.update(11.0).size() == 1);
        CHECK(d.outcome(0) == DefenceOutcome::Hit);
    }
    SUBCASE("dodge mash every 50 ms answers the hit as soon as the window opens") {
        DefenceTimeline d = single();
        int answered = 0;
        for (double t = 10.0; t < 11.0 && !d.complete(); t += 0.05)
            for (const DefenceResult& r : d.press(DefenceInput::Dodge, t)) answered += r.outcome == DefenceOutcome::Dodged;
        CHECK(answered == 1);
        CHECK(d.outcome(0) == DefenceOutcome::Dodged);
        CHECK(d.ignored() == 0);
        CHECK(d.misses() > 5);
    }
}

TEST_CASE("defence: multi-hit combos") {
    const AttackTimeline three = timeline({hitAt(0.5), hitAt(0.8), hitAt(1.1)});
    SUBCASE("every hit parried: all perfect, refunds and the counter flag") {
        DefenceTimeline d;
        d.reset(three, kStart);
        for (std::size_t i = 0; i < 3; ++i) {
            const DefenceResults r = d.press(DefenceInput::Parry, d.idealPress(i, DefenceInput::Parry));
            REQUIRE(r.size() == 1);
            CHECK(r[0].hit == i);
            CHECK(r[0].outcome == DefenceOutcome::Perfect);
        }
        CHECK(d.complete());
        CHECK(d.allParried());
        CHECK(d.refunds() == 3);
        CHECK(d.count(DefenceOutcome::Perfect) == 3);
    }
    SUBCASE("a press resolves the hits that landed before it, then answers the next") {
        DefenceTimeline d;
        d.reset(three, kStart);
        CHECK(d.press(DefenceInput::Parry, d.idealPress(0, DefenceInput::Parry)).size() == 1);
        const DefenceResults r = d.press(DefenceInput::Dodge, 11.1 - 0.2); // hit 1 (10.8) landed already
        REQUIRE(r.size() == 2);
        CHECK(r[0].hit == 1);
        CHECK(r[0].outcome == DefenceOutcome::Hit);
        CHECK(r[1].hit == 2);
        CHECK(r[1].outcome == DefenceOutcome::Dodged);
        CHECK(d.complete());
        CHECK_FALSE(d.allParried());
    }
    SUBCASE("no presses: every hit lands, one update can resolve several") {
        DefenceTimeline d;
        d.reset(three, kStart);
        CHECK(d.update(10.6).size() == 1);
        CHECK(d.advance(1.0).size() == 2);
        CHECK(d.count(DefenceOutcome::Hit) == 3);
        CHECK(d.now() == doctest::Approx(11.6));
    }
    SUBCASE("each press answers only the earliest pending hit") {
        // Hits 0.2 s apart: a dodge inside both windows takes the first one.
        DefenceTimeline d;
        d.reset(timeline({hitAt(1.0), hitAt(1.2)}), kStart);
        const DefenceResults r = d.press(DefenceInput::Dodge, 10.95);
        REQUIRE(r.size() == 1);
        CHECK(r[0].hit == 0);
        CHECK(d.nextIndex() == 1);
    }
}

TEST_CASE("defence: jumpable hits") {
    SUBCASE("jump answers a jumpable hit, without a refund") {
        DefenceTimeline d = single(true);
        const DefenceResults r = d.press(DefenceInput::Jump, 11.0 - 0.2);
        REQUIRE(r.size() == 1);
        CHECK(r[0].outcome == DefenceOutcome::Jumped);
        CHECK_FALSE(r[0].refund);
    }
    SUBCASE("a dodge can't avoid a jumpable hit") {
        DefenceTimeline d = single(true);
        CHECK_FALSE(d.accepts(0, DefenceInput::Dodge));
        CHECK(d.press(DefenceInput::Dodge, 11.0 - 0.05).empty());
        CHECK(d.update(11.0)[0].outcome == DefenceOutcome::Hit);
    }
    SUBCASE("a parry still works on a jumpable hit") {
        DefenceTimeline d = single(true);
        CHECK(d.press(DefenceInput::Parry, d.idealPress(0, DefenceInput::Parry))[0].outcome == DefenceOutcome::Perfect);
    }
    SUBCASE("jumping a plain hit is a miss and locks jump out") {
        DefenceTimeline d = single(false);
        CHECK_FALSE(d.accepts(0, DefenceInput::Jump));
        CHECK(d.press(DefenceInput::Jump, 11.0 - 0.2).empty());
        CHECK(d.lockedOut(DefenceInput::Jump, 11.0 - 0.1));
        CHECK(d.outcome(0) == DefenceOutcome::Pending);
    }
    SUBCASE("jump window edges") {
        DefenceTimeline in = single(true);
        CHECK(in.press(DefenceInput::Jump, 11.0 - 0.24).size() == 1);
        DefenceTimeline out = single(true);
        CHECK(out.press(DefenceInput::Jump, 11.0 - 0.26).empty());
    }
}

TEST_CASE("defence: telegraphs for slow, big hits") {
    DefenceTimeline d;
    d.reset(timeline({{2.0, 0.5, 1.5, 1.0f, true}}), kStart);
    CHECK_FALSE(d.telegraphActive(0, 10.4));
    CHECK(d.telegraphActive(0, 10.6));
    CHECK_FALSE(d.tellActive(0, 10.6));
    CHECK(d.tellActive(0, 11.6));
    CHECK_FALSE(d.telegraphActive(0, 12.0));
}

namespace {

Combatant defender(int hp = 100) {
    Combatant c;
    c.name = "Hero";
    c.stats = {hp, 20, 0, 1, 10};
    c.hp = hp;
    c.skills = {demo::makeSkill("Strike", 0, 1, Targeting::SingleEnemy, 1.0f, 4, 1.0f)};
    return c;
}

Combatant attacker(AttackTimeline t) {
    Combatant c;
    c.name = "Foe";
    c.stats = {500, 10, 0, 50, 10};
    c.hp = 500;
    Skill s = demo::makeSkill("Blow", 0, 0, Targeting::SingleEnemy, 1.0f, 4, 2.0f);
    s.timeline = t;
    c.skills = {s};
    return c;
}

// Starts a battle and runs to the enemy's first attack (the enemy is faster, so it acts first).
void toEnemyAttack(Battle& b, double& now) {
    b.start(now);
    while (b.phase() != Phase::EnemySkill) {
        if (b.phase() == Phase::AwaitingCommand) REQUIRE(b.command(0, 1, now) == CommandError::None);
        now += 0.05;
        b.update(now);
        REQUIRE(now < 100.0);
    }
}

std::vector<BattleEvent> defended(const std::vector<BattleEvent>& events) {
    std::vector<BattleEvent> out;
    for (const BattleEvent& e : events)
        if (e.type == EventType::Defended) out.push_back(e);
    return out;
}

} // namespace

TEST_CASE("defence in battle: perfect parries and dodges refund AP, capped at max") {
    const AttackTimeline two = timeline({hitAt(0.8), hitAt(1.4)});
    SUBCASE("two parries: two refunds and the counter") {
        Battle b({defender()}, {attacker(two)});
        double now = 0.0;
        toEnemyAttack(b, now);
        const int ap = b.combatant(0).ap;
        b.drainEvents();
        demo::pilotDefend(b, 1e9);
        const auto events = b.drainEvents();
        const auto d = defended(events);
        REQUIRE(d.size() == 2);
        CHECK(d[0].amount == 1);
        CHECK(d[1].amount == 1);
        CHECK(d[1].beat == 1);
        CHECK(b.combatant(0).ap == ap + 2 + b.tuning().apOnCounter);
        CHECK(b.combatant(0).hp == 100);
        CHECK(b.phase() == Phase::Recovery);
    }
    SUBCASE("perfect dodges refund too, but don't counter") {
        Battle b({defender()}, {attacker(two)});
        double now = 0.0;
        toEnemyAttack(b, now);
        const int ap = b.combatant(0).ap;
        b.drainEvents();
        CHECK(demo::pilotDefend(b, 1e9, DefenceInput::Dodge) == 2);
        const auto events = b.drainEvents();
        CHECK(defended(events)[0].label == "PERFECT");
        CHECK(b.combatant(0).ap == ap + 2);
        CHECK(b.combatant(1).hp == 500); // no counter
        CHECK(b.combatant(0).hp == 100);
    }
    SUBCASE("refunds stop at max AP") {
        Combatant hero = defender();
        hero.maxAp = 3;
        BattleTuning tuning;
        tuning.startingAp = 2;
        Battle b({hero}, {attacker(two)}, tuning);
        double now = 0.0;
        toEnemyAttack(b, now);
        REQUIRE(b.combatant(0).ap == 2);
        b.drainEvents();
        demo::pilotDefend(b, 1e9, DefenceInput::Dodge);
        const auto d = defended(b.drainEvents());
        REQUIRE(d.size() == 2);
        CHECK(d[0].amount == 1);
        CHECK(d[1].amount == 0);
        CHECK(b.combatant(0).ap == 3);
    }
    SUBCASE("a skill with no hits ends at once") {
        AttackTimeline none;
        none.clear();
        Battle b({defender()}, {attacker(none)});
        double now = 0.0;
        b.start(now);
        b.drainEvents();
        b.update(now + 1.0);
        CHECK(b.phase() != Phase::EnemySkill);
        CHECK(b.combatant(0).hp == 100);
    }
}

TEST_CASE("defence autopilot: perfect runs of the demo fight take no damage") {
    std::vector<Combatant> party = demo::party();
    int fullHp = 0;
    for (const Combatant& c : party) fullHp += c.hp;
    for (const double step : {1.0 / 120.0, 0.13}) {
        CAPTURE(step);
        const demo::HeadlessResult parried = demo::playHeadless(party, demo::enemies(), true, step);
        CHECK(parried.outcome == Phase::Victory);
        CHECK(parried.enemyAttacks >= 1);
        CHECK(parried.partyHpLeft == fullHp);
        const demo::HeadlessResult dodged =
            demo::playHeadless(party, demo::enemies(), true, step, 1200.0, DefenceInput::Dodge);
        CHECK(dodged.outcome == Phase::Victory);
        CHECK(dodged.partyHpLeft == fullHp);
    }
    // Jumpable hits: the dodge pilot jumps them.
    Battle b({defender()}, {attacker(timeline({hitAt(1.0, true), hitAt(1.5)}))});
    double now = 0.0;
    toEnemyAttack(b, now);
    b.drainEvents();
    demo::pilotDefend(b, 1e9, DefenceInput::Dodge);
    const auto d = defended(b.drainEvents());
    REQUIRE(d.size() == 2);
    CHECK(d[0].label == "JUMP");
    CHECK(d[1].label == "PERFECT");
    CHECK(b.combatant(0).hp == 100);
}

// The Unreal autopilot presses a little ahead of the battle clock, so the next updates come with
// earlier times than the press. The clock never runs backwards and nothing is judged twice.
TEST_CASE("defence: updates earlier than a press change nothing") {
    DefenceTimeline d;
    d.reset(timeline({hitAt(1.0), hitAt(1.5)}), kStart);
    const double early = kStart + 0.5;
    CHECK(d.update(early).size() == 0);
    const double ideal = d.idealPress(0, DefenceInput::Parry);
    REQUIRE(ideal > early);
    const DefenceResults pressed = d.press(DefenceInput::Parry, ideal);
    REQUIRE(pressed.size() == 1);
    CHECK(d.update(early).size() == 0);
    CHECK(d.update(ideal - 0.01).size() == 0);
    CHECK(d.now() == ideal);
    CHECK(d.outcome(0) == DefenceOutcome::Perfect);
    CHECK(d.nextIndex() == 1);
    const DefenceResults landed = d.update(d.impact(1) + 0.01);
    REQUIRE(landed.size() == 1);
    CHECK(landed[0].hit == 1);
    CHECK(d.outcome(1) == DefenceOutcome::Hit);

    // In a battle: the pilot parries both hits half a second ahead, then the clock catches up.
    Battle b({defender()}, {attacker(timeline({hitAt(1.0), hitAt(1.3)}))});
    double now = 0.0;
    toEnemyAttack(b, now);
    b.drainEvents();
    int presses = 0;
    while (b.phase() == Phase::EnemySkill && now < 30.0) {
        presses += demo::pilotDefend(b, now + 0.5);
        now += 1.0 / 60.0;
        b.update(now);
    }
    CHECK(presses == 2);
    const auto events = defended(b.drainEvents());
    REQUIRE(events.size() == 2);
    CHECK(events[0].label == "PERFECT");
    CHECK(events[1].label == "PERFECT");
    CHECK(b.combatant(0).hp == 100);
}

TEST_CASE("defence: no allocation on reset, update or press") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    const AttackTimeline three = timeline({hitAt(0.5), hitAt(0.8, true), hitAt(1.1)});
    DefenceTimeline d;
    const std::uint64_t before = ftcore_test::allocStats().count;
    int resolved = 0;
    for (int round = 0; round < 50; ++round) {
        d.reset(three, kStart);
        resolved += static_cast<int>(d.press(DefenceInput::Parry, 10.2).size()); // a miss
        resolved += static_cast<int>(d.press(DefenceInput::Dodge, 10.47).size());
        resolved += static_cast<int>(d.update(10.9).size());
        resolved += static_cast<int>(d.press(DefenceInput::Jump, 11.0).size());
        resolved += static_cast<int>(d.advance(1.0).size());
    }
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(resolved == 150);
}

TEST_CASE("defence: a press just after the impact is late, not a miss") {
    SUBCASE("the hit already landed: late, no lockout") {
        DefenceTimeline d = single();
        REQUIRE(d.update(11.05).size() == 1);
        CHECK(d.outcome(0) == DefenceOutcome::Hit);
        CHECK(d.press(DefenceInput::Parry, 11.1).empty());
        CHECK(d.late(0));
        CHECK(d.lateCount() == 1);
        CHECK(d.lastLate() == 0);
        CHECK(d.misses() == 0);
        CHECK_FALSE(d.lockedOut(DefenceInput::Parry, 11.15));
        // A second press is no longer late: it is a miss.
        d.press(DefenceInput::Dodge, 11.12);
        CHECK(d.lateCount() == 1);
        CHECK(d.misses() == 1);
    }
    SUBCASE("the late press itself expires the hit") {
        DefenceTimeline d = single();
        const DefenceResults r = d.press(DefenceInput::Dodge, 11.0 + 0.19);
        REQUIRE(r.size() == 1);
        CHECK(r[0].outcome == DefenceOutcome::Hit);
        CHECK(d.late(0));
    }
    SUBCASE("past the late window it is a plain miss") {
        DefenceTimeline d = single();
        d.update(11.05);
        d.press(DefenceInput::Parry, 11.0 + d.windows().late + 0.01);
        CHECK_FALSE(d.late(0));
        CHECK(d.misses() == 1);
    }
    SUBCASE("an input the hit doesn't take is never late") {
        DefenceTimeline d = single(false);
        d.update(11.05);
        d.press(DefenceInput::Jump, 11.1);
        CHECK_FALSE(d.late(0));
        CHECK(d.misses() == 1);
    }
    SUBCASE("reset clears it") {
        DefenceTimeline d = single();
        d.update(11.05);
        d.press(DefenceInput::Parry, 11.1);
        d.reset(timeline({hitAt(1.0)}), kStart);
        CHECK_FALSE(d.late(0));
        CHECK(d.lateCount() == 0);
    }
}

TEST_CASE("defence in battle: a late press after the last hit says LATE in the recovery") {
    Battle b({defender()}, {attacker(timeline({hitAt(0.8)}))});
    double now = 0.0;
    toEnemyAttack(b, now);
    const double impact = b.defence().impact(0);
    b.update(impact + 0.02);
    REQUIRE(b.phase() == Phase::Recovery);
    const int hp = b.combatant(0).hp;
    CHECK(hp < 100);
    b.drainEvents();
    b.defend(DefenceInput::Parry, impact + 0.08);
    const auto d = defended(b.drainEvents());
    REQUIRE(d.size() == 1);
    CHECK(d[0].label == "LATE");
    CHECK(d[0].grade == Grade::Miss);
    CHECK(d[0].beat == 0);
    CHECK(d[0].target == 0);
    CHECK(d[0].amount == 0);
    CHECK(b.combatant(0).hp == hp); // nothing else changes
    CHECK(b.phase() == Phase::Recovery);
    // Once the recovery is over the press is ignored.
    b.update(impact + 10.0);
    b.drainEvents();
    b.defend(DefenceInput::Parry, impact + 10.01);
    CHECK(defended(b.drainEvents()).empty());
}

// --- the song's beat ---

TEST_CASE("snapToBeat moves each hit later onto a beat, keeps them apart and leaves bpm 0 alone") {
    // The demo Shade's three quick hits, 0.3 s apart, on a 120 bpm song whose beat 0 is at 0.1 s.
    const AttackTimeline data = timeline({{0.8, 0.4, 0.0, 1.0f, false}, {1.1, 0.4, 0.0, 1.0f, false}, {1.4, 0.4, 0.7, 2.0f, true}});
    const AttackTimeline t = snapToBeat(data, kStart, 120.0, 0.1);
    REQUIRE(t.size() == 3);
    double previous = 0.0;
    for (std::size_t i = 0; i < t.size(); ++i) {
        CAPTURE(i);
        const double beats = (kStart + t[i].at - 0.1) / 0.5;
        CHECK(beats == doctest::Approx(std::round(beats)));
        CHECK(t[i].at >= data[i].at - 1e-9); // never earlier
        CHECK(t[i].at > previous + 0.5 - 1e-9); // a beat or more after the last one
        CHECK(t[i].tell == data[i].tell);
        CHECK(t[i].tell <= t[i].at);
        CHECK(t[i].telegraph <= t[i].at);
        previous = t[i].at;
    }
    CHECK(t[0].at == doctest::Approx(1.1)); // 10.8 s -> beat 21.4 -> beat 22 at 11.1 s
    CHECK(t[1].at == doctest::Approx(1.6)); // 11.1 s is beat 22, taken: the next one
    CHECK(t[2].at == doctest::Approx(2.1));
    CHECK(t[2].weight == 2.0f);
    CHECK(t[2].jumpable);

    // A hit already on a beat stays there.
    const AttackTimeline on = snapToBeat(timeline({{1.0, 0.4, 0.0, 1.0f, false}}), 0.0, 120.0, 0.0);
    CHECK(on[0].at == doctest::Approx(1.0));
    // No music: unchanged.
    CHECK(sameTimeline(snapToBeat(data, kStart, 0.0, 0.1), data));
    CHECK(sameTimeline(snapToBeat(data, kStart, -60.0, 0.1), data));
    // Full timelines, any banner time: still on beats, in order, never earlier.
    AttackTimeline full;
    full.clear();
    for (int i = 0; i < static_cast<int>(kMaxTimelineHits); ++i) full.add({0.5 + 0.05 * i, 0.4, 0.0, 1.0f, false});
    for (const double start : {0.0, 0.37, 3.99, 123.456}) {
        CAPTURE(start);
        const AttackTimeline f = snapToBeat(full, start, 72.0, 0.2);
        REQUIRE(f.size() == kMaxTimelineHits);
        for (std::size_t i = 0; i < f.size(); ++i) {
            const double beats = (start + f[i].at - 0.2) / (60.0 / 72.0);
            CHECK(beats == doctest::Approx(std::round(beats)).epsilon(1e-9));
            CHECK(f[i].at >= full[i].at - 1e-9);
            if (i > 0) CHECK(f[i].at > f[i - 1].at);
        }
    }
}
