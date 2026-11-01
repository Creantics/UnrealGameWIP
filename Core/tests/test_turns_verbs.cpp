// The Turns battle's verbs beyond the arrow combos: poise and stagger, Guard, Load, Release, Overload,
// Intercept, Torn and Take Half.
#include "AllocCounter.h"
#include "ftcore/Battle.h"
#include "ftcore/BattleDemo.h"
#include "ftcore/Encounter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ostream> // doctest prints std::string_view through operator<<
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

Combatant lead(int speed = 5) {
    Combatant c;
    c.name = "Lead";
    c.stats = {100, 20, 0, speed, 10};
    c.hp = 100;
    c.skills = {{"Strike", 0, 1, Targeting::SingleEnemy, 1.0f, 4, 1.0f}};
    return c;
}

Combatant foe(int poise, int speed = 50) {
    Combatant c;
    c.name = "Foe";
    c.stats = {500, 10, 0, speed, 10};
    c.stats.maxPoise = poise;
    c.hp = 500;
    c.skills = {{"Claw", 0, 0, Targeting::SingleEnemy, 1.0f, 4, 2.0f}};
    return c;
}

BattleTuning tuning() {
    BattleTuning t;
    t.leadIn = 0.5;
    t.recovery = 0.5;
    return t;
}

int count(const std::vector<BattleEvent>& events, EventType type) {
    int n = 0;
    for (const BattleEvent& e : events) n += e.type == type;
    return n;
}

// Steps to the next enemy turn, collecting the events on the way; false if none came.
bool toEnemyAttack(Battle& b, double& now, std::vector<BattleEvent>& seen) {
    for (int i = 0; i < 2000 && b.phase() != Phase::EnemySkill; ++i) {
        now += 0.05;
        b.update(now);
        for (const BattleEvent& e : b.drainEvents()) seen.push_back(e);
    }
    return b.phase() == Phase::EnemySkill;
}

// Parries every hit of the current enemy attack at its ideal time.
void parryAll(Battle& b, std::vector<BattleEvent>& seen) {
    while (b.phase() == Phase::EnemySkill && !b.defence().complete()) {
        const DefenceTimeline& d = b.defence();
        b.defend(DefenceInput::Parry, d.idealPress(d.nextIndex(), DefenceInput::Parry));
    }
    for (const BattleEvent& e : b.drainEvents()) seen.push_back(e);
}

} // namespace

TEST_CASE("poise: counters wear it down, and at 0 the enemy is staggered and skips its next turn") {
    Battle b({lead()}, {foe(25)}, tuning());
    REQUIRE(b.combatant(1).poise == 25);
    double now = 0.0;
    b.start(now);
    std::vector<BattleEvent> seen;
    for (const BattleEvent& e : b.drainEvents()) seen.push_back(e);

    // Two counters: 25 - 10 - 10 = 5, still standing.
    for (int i = 0; i < 2; ++i) {
        REQUIRE(toEnemyAttack(b, now, seen));
        parryAll(b, seen);
    }
    CHECK(b.combatant(1).poise == 5);
    CHECK_FALSE(b.combatant(1).staggered);
    CHECK(count(seen, EventType::PoiseLost) == 2);
    CHECK(count(seen, EventType::Staggered) == 0);

    // The third takes the last 5 and breaks it.
    seen.clear();
    REQUIRE(toEnemyAttack(b, now, seen));
    parryAll(b, seen);
    CHECK(b.combatant(1).poise == 0);
    CHECK(b.combatant(1).staggered);
    REQUIRE(count(seen, EventType::PoiseLost) == 1);
    for (const BattleEvent& e : seen)
        if (e.type == EventType::PoiseLost) {
            CHECK(e.amount == 5);
            CHECK(e.source == 0);
            CHECK(e.target == 1);
        }
    CHECK(count(seen, EventType::Staggered) == 1);
    CHECK(b.stats().staggers == 1);

    // Its next turn passes: Steadied with the poise restored, no attack; the turn after, it attacks.
    seen.clear();
    REQUIRE(toEnemyAttack(b, now, seen));
    int steadied = 0;
    for (const BattleEvent& e : seen)
        if (e.type == EventType::Steadied) {
            ++steadied;
            CHECK(e.source == 1);
            CHECK(e.amount == 25);
        }
    CHECK(steadied == 1);
    CHECK(count(seen, EventType::SkillStarted) == 1); // only the attack this loop stopped on
    CHECK(count(seen, EventType::TurnStarted) == 2);  // the skipped turn and this one
    CHECK(b.combatant(1).poise == 25);
    CHECK_FALSE(b.combatant(1).staggered);
}

TEST_CASE("poise: an enemy without poise is never staggered") {
    Battle b({lead()}, {foe(0)}, tuning());
    double now = 0.0;
    b.start(now);
    std::vector<BattleEvent> seen;
    for (int i = 0; i < 4; ++i) {
        REQUIRE(toEnemyAttack(b, now, seen));
        parryAll(b, seen);
    }
    CHECK(count(seen, EventType::Counter) == 4);
    CHECK(count(seen, EventType::PoiseLost) == 0);
    CHECK(count(seen, EventType::Staggered) == 0);
    CHECK(b.stats().staggers == 0);
}

TEST_CASE("poise: counterPoise sets what a counter costs, 0 for nothing") {
    BattleTuning t = tuning();
    t.counterPoise = 30;
    Battle b({lead()}, {foe(30)}, t);
    double now = 0.0;
    b.start(now);
    std::vector<BattleEvent> seen;
    REQUIRE(toEnemyAttack(b, now, seen));
    parryAll(b, seen);
    CHECK(b.combatant(1).staggered);

    t.counterPoise = 0;
    Battle none({lead()}, {foe(30)}, t);
    now = 0.0;
    none.start(now);
    seen.clear();
    REQUIRE(toEnemyAttack(none, now, seen));
    parryAll(none, seen);
    CHECK(none.combatant(1).poise == 30);
    CHECK(count(seen, EventType::PoiseLost) == 0);
}

TEST_CASE("poise: the enemy data takes an optional stats.poise") {
    EnemyRoster roster;
    std::string err;
    std::vector<std::string> warnings;
    const auto load = [&](const char* stats) {
        const std::string json = std::string(R"({"enemies":[{"id":"pup","stats":)") + stats +
                                 R"(,"skills":[{"id":"snap","power":1.0}]}]})";
        err.clear();
        warnings.clear();
        return importEnemyRoster(json, roster, &err, &warnings);
    };
    REQUIRE_MESSAGE(load(R"({"maxHp":50,"attack":9,"defense":2,"speed":6,"technique":7,"poise":20})"), err);
    CHECK(warnings.empty());
    CHECK(roster.enemies[0].stats.maxPoise == 20);
    std::vector<Combatant> made;
    makeEnemies(roster, EncounterLineup{.enemies = {0}, .tier = 4}, made);
    REQUIRE(made.size() == 1);
    CHECK(made[0].stats.maxPoise == 20); // tiers leave poise alone

    REQUIRE_MESSAGE(load(R"({"maxHp":50,"attack":9,"defense":2,"speed":6,"technique":7})"), err);
    CHECK(roster.enemies[0].stats.maxPoise == 0);

    CHECK_FALSE(load(R"({"maxHp":50,"poise":-1})"));
    CHECK(err.find("enemies[0].stats.poise") != std::string::npos);
    CHECK_FALSE(load(R"({"maxHp":50,"poise":1001})"));
    CHECK_FALSE(load(R"({"maxHp":50,"poise":"high"})"));
}

// ---- Guard, Load, Release, Overload ----

namespace {

Combatant guardLead(int speed = 5, int load = 0) {
    Combatant c = lead(speed);
    c.verbs = verbBit(Verb::Guard) | verbBit(Verb::Release);
    c.load.value = load;
    return c;
}

std::vector<BattleEvent> drain(Battle& b) { return b.drainEvents(); }

const BattleEvent* first(const std::vector<BattleEvent>& events, EventType type) {
    for (const BattleEvent& e : events)
        if (e.type == type) return &e;
    return nullptr;
}

// Plays every note of the party combo on time; `miss` of them get no press (they are missed).
void playCombo(Battle& b, std::size_t miss = 0) {
    const ComboSequence& c = b.combo();
    const std::size_t n = c.size();
    double end = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        end = c.note(i).time;
        if (i < miss) continue;
        b.press(c.note(i).arrow, c.note(i).time);
    }
    b.update(end + 2.0);
}

// Guards every hit of the current enemy attack: perfectly (a hold begun at each hit's ideal time and
// let go just after it), or held from `heldFrom` seconds before the first impact to the end.
void guardAll(Battle& b, bool perfect, double heldFrom = 0.3) {
    const DefenceTimeline& d = b.defence();
    const std::size_t n = d.size();
    const double last = d.impact(n - 1);
    if (!perfect) {
        b.defend(DefenceInput::Guard, d.impact(0) - heldFrom);
        b.update(last + 0.01);
        return;
    }
    for (std::size_t i = 0; i < n && b.phase() == Phase::EnemySkill; ++i) {
        b.defend(DefenceInput::Guard, d.idealPress(i, DefenceInput::Guard));
        b.releaseGuard(d.impact(i) + 0.001);
    }
    b.update(last + 0.01);
}

} // namespace

TEST_CASE("guard: a hold is perfect exactly where a parry would land, held before that") {
    AttackTimeline t;
    t.clear();
    t.add({1.0, 0.4, 0.0, 1.0f, false});
    t.add({2.0, 0.4, 0.0, 1.0f, true}); // a jumpable hit: Guard still answers it
    for (int k = -40; k <= 40; ++k) {
        const double before = 0.12 + k * 0.0025; // around the parry's edge
        DefenceTimeline parry;
        parry.reset(t, 10.0);
        DefenceTimeline guard;
        guard.reset(t, 10.0);
        const double at = guard.impact(0) - before;
        const bool parried = !parry.press(DefenceInput::Parry, at).empty();
        guard.press(DefenceInput::Guard, at);
        const DefenceResults r = guard.update(guard.impact(0));
        REQUIRE(r.size() == 1);
        CHECK(r[0].outcome == (parried ? DefenceOutcome::PerfectGuard : DefenceOutcome::Guarded));
        CHECK_FALSE(r[0].refund);
    }

    DefenceTimeline d;
    d.reset(t, 10.0);
    // Held from well before the first hit and never let go: the first is guarded, and so is the
    // second (the hold began long before it).
    d.press(DefenceInput::Guard, 10.5);
    d.press(DefenceInput::Guard, 10.95); // already holding: the hold keeps its start
    CHECK(d.guardFrom() == doctest::Approx(10.5));
    const DefenceResults all = d.update(13.0);
    REQUIRE(all.size() == 2);
    CHECK(all[0].outcome == DefenceOutcome::Guarded);
    CHECK(all[1].outcome == DefenceOutcome::Guarded);
    CHECK(d.misses() == 0);
    CHECK_FALSE(d.lockedOut(DefenceInput::Guard, 13.0));
    CHECK(d.lockedOut(DefenceInput::Parry, 13.0) == false);

    // Let go before the impact: the hit lands. A new hold at the second hit's ideal time is perfect.
    d.reset(t, 10.0);
    d.press(DefenceInput::Guard, 10.5);
    CHECK(d.releaseGuard(10.9).empty());
    CHECK_FALSE(d.guarding());
    CHECK(d.update(11.0)[0].outcome == DefenceOutcome::Hit);
    d.press(DefenceInput::Guard, d.idealPress(1, DefenceInput::Guard));
    const DefenceResults second = d.releaseGuard(12.5);
    REQUIRE(second.size() == 1);
    CHECK(second[0].outcome == DefenceOutcome::PerfectGuard);
    CHECK(std::string_view(defenceOutcomeName(DefenceOutcome::PerfectGuard)) == "PERFECT GUARD");
    CHECK(std::string_view(defenceOutcomeName(DefenceOutcome::Guarded)) == "GUARD");
}

TEST_CASE("guard in battle: perfect stores the blow as Load, held lets half land, no verb no guard") {
    SUBCASE("perfect") {
        Battle b({guardLead()}, {foe(0)}, tuning());
        double now = 0.0;
        b.start(now);
        std::vector<BattleEvent> seen;
        REQUIRE(toEnemyAttack(b, now, seen));
        guardAll(b, true);
        const auto events = drain(b);
        CHECK(b.combatant(0).hp == 100);
        CHECK(b.combatant(0).load.value == 10); // the whole blow: 10 attack, no defence
        const BattleEvent* stored = first(events, EventType::LoadStored);
        REQUIRE(stored);
        int sum = 0;
        for (const BattleEvent& e : events) sum += e.type == EventType::LoadStored ? e.amount : 0;
        CHECK(sum == 10);
        CHECK(stored->target == 0);
        CHECK(stored->source == 1);
        const BattleEvent* defended = first(events, EventType::Defended);
        REQUIRE(defended);
        CHECK(defended->label == "PERFECT GUARD");
        CHECK(defended->grade == Grade::Perfect);
        CHECK(first(events, EventType::Counter) == nullptr); // a Guard banks the hit; it never counters
        CHECK(b.stats().loadStored == 10);
        CHECK(b.stats().perfectDefences == 1);
    }
    SUBCASE("held") {
        Battle b({guardLead()}, {foe(0)}, tuning());
        double now = 0.0;
        b.start(now);
        std::vector<BattleEvent> seen;
        REQUIRE(toEnemyAttack(b, now, seen));
        guardAll(b, false);
        const auto events = drain(b);
        CHECK(b.combatant(0).hp == 95); // half the 10
        CHECK(b.combatant(0).load.value == 0);
        REQUIRE(first(events, EventType::Defended));
        CHECK(first(events, EventType::Defended)->label == "GUARD");
        CHECK(first(events, EventType::LoadStored) == nullptr);
    }
    SUBCASE("a member without the verb cannot guard") {
        Battle b({lead()}, {foe(0)}, tuning());
        double now = 0.0;
        b.start(now);
        std::vector<BattleEvent> seen;
        REQUIRE(toEnemyAttack(b, now, seen));
        b.defend(DefenceInput::Guard, b.defence().idealPress(0, DefenceInput::Guard));
        CHECK_FALSE(b.defence().guarding());
        b.update(b.defence().impact(b.defence().size() - 1) + 0.01);
        CHECK(b.combatant(0).hp == 90);
    }
}

TEST_CASE("release: the combo is 4 + Load/20 notes and its accuracy sets the poise lost") {
    for (const int load : {20, 60, 100}) {
        Battle b({guardLead(50, load)}, {foe(20, 5), foe(20, 5), foe(20, 5), foe(20, 5)}, tuning());
        b.start(0.0);
        REQUIRE(b.phase() == Phase::AwaitingCommand);
        REQUIRE(b.release(0.0) == CommandError::None);
        CHECK(b.releasing());
        CHECK(b.combo().size() == static_cast<std::size_t>(4 + load / 20));
        CHECK(b.combatant(0).load.value == 0); // spent at once
        REQUIRE(b.activeSkill());
        CHECK(b.activeSkill()->name == "Release");
        drain(b);
        playCombo(b);
        const auto events = drain(b);
        const BattleEvent* released = first(events, EventType::Released);
        REQUIRE(released);
        CHECK(released->amount == load);
        CHECK(released->source == 0);
        int lost = 0;
        int staggered = 0;
        for (const BattleEvent& e : events) {
            lost += e.type == EventType::PoiseLost ? e.amount : 0;
            staggered += e.type == EventType::Staggered;
        }
        CHECK(lost == 4 * std::min(load, 20));
        CHECK(staggered == (load >= 20 ? 4 : 0));
        for (CombatantId id = 1; id <= 4; ++id) CHECK(b.combatant(id).hp < 500); // it hits every enemy
        CHECK(b.stats().releases == 1);
        CHECK_FALSE(b.releasing());
    }

    // Half the notes: half the poise.
    Battle half({guardLead(50, 80)}, {foe(100, 5)}, tuning());
    half.start(0.0);
    REQUIRE(half.release(0.0) == CommandError::None);
    REQUIRE(half.combo().size() == 8);
    drain(half);
    playCombo(half, 4);
    const auto events = drain(half);
    const BattleEvent* lost = first(events, EventType::PoiseLost);
    REQUIRE(lost);
    CHECK(lost->amount == static_cast<int>(std::lround(80.0f * half.combo().accuracy())));
    CHECK(lost->amount < 80);
}

TEST_CASE("release: refused without the verb, without Load, or outside a command") {
    Battle noVerb({lead(50)}, {foe(20, 5)}, tuning());
    noVerb.start(0.0);
    CHECK(noVerb.release(0.0) == CommandError::NoSuchVerb);
    Battle noLoad({guardLead(50, 0)}, {foe(20, 5)}, tuning());
    noLoad.start(0.0);
    CHECK(noLoad.release(0.0) == CommandError::NoLoad);
    CHECK(noLoad.phase() == Phase::AwaitingCommand);
    Battle wrong({guardLead(50, 40)}, {foe(20, 5)}, tuning());
    CHECK(wrong.release(0.0) == CommandError::WrongPhase); // not started
}

TEST_CASE("overload: a full Load locks every command but Release, until it is spent") {
    Battle b({guardLead(50, kMaxLoad)}, {foe(0, 5)}, tuning());
    b.start(0.0);
    REQUIRE(b.phase() == Phase::AwaitingCommand);
    CHECK(b.command(0, 1, 0.0) == CommandError::Overloaded);
    CHECK(b.aim(1, 0.0) == CommandError::Overloaded);
    CHECK(b.useItem(0, 0, 0.0) == CommandError::Overloaded);
    CHECK(b.phase() == Phase::AwaitingCommand);
    REQUIRE(b.release(0.0) == CommandError::None);
    playCombo(b);
    // The lead is quicker: its next turn comes before the foe's, and the lock is gone.
    double now = 10.0;
    for (int i = 0; i < 400 && b.phase() != Phase::AwaitingCommand; ++i) b.update(now += 0.05);
    REQUIRE(b.phase() == Phase::AwaitingCommand);
    CHECK(b.command(0, 1, now) == CommandError::None);
}

TEST_CASE("overload: storing up to the cap emits Overloaded once") {
    Battle b({guardLead(5, 95)}, {foe(0)}, tuning());
    double now = 0.0;
    b.start(now);
    std::vector<BattleEvent> seen;
    REQUIRE(toEnemyAttack(b, now, seen));
    guardAll(b, true);
    const auto events = drain(b);
    // Only the room left (5 of the blow's 10) is stored.
    REQUIRE(count(events, EventType::LoadStored) == 1);
    CHECK(first(events, EventType::LoadStored)->amount == 5);
    REQUIRE(count(events, EventType::Overloaded) == 1);
    CHECK(first(events, EventType::Overloaded)->target == 0);
    CHECK(b.combatant(0).load.overloaded());
    CHECK(b.combatant(0).hp == 100);
}

// The demo's foes go for the second member, so the first lead fights them alone here.
TEST_CASE("guard pilot: the first lead alone wins the demo fight by guarding, storing and releasing") {
    const std::vector<Combatant> alone{demo::party(4)[0]};
    for (const double step : {1.0 / 120.0, 0.13}) {
        const demo::HeadlessResult r = demo::playHeadless(alone, demo::enemies(), true, step, 1200.0, DefenceInput::Guard);
        CAPTURE(step);
        CHECK_FALSE(r.commandFailed);
        CHECK(r.outcome == Phase::Victory);
        CHECK(r.loadStored > 0);
        CHECK(r.releases >= 1);
    }
    const demo::HeadlessResult shaky =
        demo::playHeadless(alone, demo::enemies(), true, 1.0 / 120.0, 1200.0, DefenceInput::Guard, {0.85, 0.75});
    CHECK_FALSE(shaky.commandFailed);
    CHECK(shaky.outcome == Phase::Victory);
}

// ---- Intercept, Torn, Take Half ----

namespace {

// Two leads: the first (who can Intercept and Take Half) at full health, the second (the defender
// the foe goes for, being the more wounded) at `hurt` HP of 100.
std::vector<Combatant> pair(int speed = 5, int hurt = 80) {
    Combatant a = lead(speed);
    a.verbs = verbBit(Verb::Intercept) | verbBit(Verb::TakeHalf) | verbBit(Verb::Release);
    Combatant b = lead(speed);
    b.hp = hurt;
    return {a, b};
}

// Intercepts every hit of the current enemy attack: perfectly (a hold begun at each hit's ideal time
// and let go just after it) or held from 0.3 s before the first impact to the end.
void interceptAll(Battle& b, bool perfect) {
    const DefenceTimeline& d = b.defence();
    const std::size_t n = d.size();
    const double last = d.impact(n - 1);
    if (!perfect) {
        b.defend(DefenceInput::Intercept, d.impact(0) - 0.3);
        b.update(last + 0.01);
        return;
    }
    for (std::size_t i = 0; i < n && b.phase() == Phase::EnemySkill; ++i) {
        b.defend(DefenceInput::Intercept, d.idealPress(i, DefenceInput::Intercept));
        b.releaseGuard(d.impact(i) + 0.001);
    }
    b.update(last + 0.01);
}

// Lets every hit of the current enemy attack land.
void takeAll(Battle& b, double& now) {
    now = b.defence().impact(b.defence().size() - 1) + 0.01;
    b.update(now);
}

// Steps to the next party command, parrying every enemy attack on the way; false if none came.
bool toPartyTurn(Battle& b, double& now, std::vector<BattleEvent>& seen) {
    for (int i = 0; i < 4000 && b.phase() != Phase::AwaitingCommand; ++i) {
        if (b.phase() == Phase::Victory || b.phase() == Phase::Defeat) return false;
        if (b.phase() == Phase::EnemySkill) {
            parryAll(b, seen);
            now = std::max(now, b.defence().impact(b.defence().size() - 1) + 0.01);
        }
        now += 0.05;
        b.update(now);
        for (const BattleEvent& e : b.drainEvents()) seen.push_back(e);
    }
    return b.phase() == Phase::AwaitingCommand;
}

Combatant tearingFoe(int poise = 0, int speed = 50) {
    Combatant c = foe(poise, speed);
    c.skills[0].torn = true;
    return c;
}

} // namespace

TEST_CASE("intercept: a hold is perfect exactly where a parry would land, intercepted before that") {
    AttackTimeline t;
    t.clear();
    t.add({1.0, 0.4, 0.0, 1.0f, false});
    t.add({2.0, 0.4, 0.0, 1.0f, true});
    for (int k = -40; k <= 40; ++k) {
        const double before = 0.12 + k * 0.0025;
        DefenceTimeline parry;
        parry.reset(t, 10.0);
        DefenceTimeline hold;
        hold.reset(t, 10.0);
        const double at = hold.impact(0) - before;
        const bool parried = !parry.press(DefenceInput::Parry, at).empty();
        hold.press(DefenceInput::Intercept, at);
        CHECK(hold.intercepting());
        const DefenceResults r = hold.update(hold.impact(0));
        REQUIRE(r.size() == 1);
        CHECK(r[0].outcome == (parried ? DefenceOutcome::PerfectIntercept : DefenceOutcome::Intercepted));
        CHECK_FALSE(r[0].refund);
    }
    for (std::size_t i = 0; i < 2; ++i) {
        DefenceTimeline d;
        d.reset(t, 10.0);
        CHECK(d.idealPress(i, DefenceInput::Intercept) == doctest::Approx(d.idealPress(i, DefenceInput::Parry)));
    }
    // One hold at a time: a Guard press while an Intercept is held keeps the Intercept.
    DefenceTimeline d;
    d.reset(t, 10.0);
    d.press(DefenceInput::Intercept, 10.5);
    d.press(DefenceInput::Guard, 10.95);
    CHECK(d.intercepting());
    CHECK(d.guardFrom() == doctest::Approx(10.5));
    CHECK(d.update(11.0)[0].outcome == DefenceOutcome::Intercepted);
    CHECK(std::string_view(defenceOutcomeName(DefenceOutcome::PerfectIntercept)) == "PERFECT INTERCEPT");
    CHECK(std::string_view(defenceOutcomeName(DefenceOutcome::Intercepted)) == "INTERCEPT");
}

TEST_CASE("intercept in battle: perfect stores the blow on the interceptor and leaves the target unharmed") {
    BattleTuning t = tuning();
    t.interceptPoise = 15;
    Battle b(pair(), {foe(100)}, t);
    double now = 0.0;
    b.start(now);
    std::vector<BattleEvent> seen;
    REQUIRE(toEnemyAttack(b, now, seen));
    REQUIRE(b.defender() == 1);
    REQUIRE(b.interceptor());
    CHECK(*b.interceptor() == 0);
    drain(b);
    interceptAll(b, true);
    const auto events = drain(b);
    CHECK(b.combatant(1).hp == 80); // the target: untouched
    CHECK(b.combatant(0).hp == 100);
    CHECK(b.combatant(0).load.value == 10); // the whole blow, on the interceptor
    CHECK(b.combatant(1).load.value == 0);
    const BattleEvent* defended = first(events, EventType::Defended);
    REQUIRE(defended);
    CHECK(defended->label == "PERFECT INTERCEPT");
    CHECK(defended->target == 0);
    CHECK(defended->grade == Grade::Perfect);
    const BattleEvent* stored = first(events, EventType::LoadStored);
    REQUIRE(stored);
    CHECK(stored->target == 0);
    // The attacker pays the tuned poise for each hit, and no counter comes.
    const int hits = static_cast<int>(b.defence().size());
    int lost = 0;
    for (const BattleEvent& e : events)
        if (e.type == EventType::PoiseLost) {
            lost += e.amount;
            CHECK(e.source == 0);
            CHECK(e.target == 2);
        }
    CHECK(lost == 15 * hits);
    CHECK(first(events, EventType::Counter) == nullptr);
    CHECK(b.stats().intercepts == hits);
    CHECK(b.stats().perfectDefences == hits);
    CHECK(b.stats().loadStored == 10);
    CHECK(b.stats().damageTaken == 0);
}

TEST_CASE("intercept in battle: a held one puts guardShare of the blow on the interceptor") {
    Battle b(pair(), {foe(100)}, tuning());
    double now = 0.0;
    b.start(now);
    std::vector<BattleEvent> seen;
    REQUIRE(toEnemyAttack(b, now, seen));
    drain(b);
    interceptAll(b, false);
    const auto events = drain(b);
    CHECK(b.combatant(1).hp == 80);
    CHECK(b.combatant(0).hp == 95); // half the 10
    CHECK(b.combatant(0).load.value == 0);
    REQUIRE(first(events, EventType::Defended));
    CHECK(first(events, EventType::Defended)->label == "INTERCEPT");
    CHECK(first(events, EventType::PoiseLost) == nullptr);
    CHECK(b.combatant(2).poise == 100);
}

TEST_CASE("intercept: never for a child, never by the defender itself, and only with the verb") {
    SUBCASE("a child target") {
        std::vector<Combatant> party = pair();
        party[1].child = true;
        Battle b(party, {foe(0)}, tuning());
        double now = 0.0;
        b.start(now);
        std::vector<BattleEvent> seen;
        REQUIRE(toEnemyAttack(b, now, seen));
        REQUIRE(b.defender() == 1);
        CHECK_FALSE(b.interceptor());
        b.defend(DefenceInput::Intercept, b.defence().idealPress(0, DefenceInput::Intercept));
        CHECK_FALSE(b.defence().guarding());
        takeAll(b, now);
        CHECK(b.combatant(1).hp == 70);
        CHECK(b.combatant(0).load.value == 0);
    }
    SUBCASE("the verb-holder is the target") {
        std::vector<Combatant> party = pair();
        party[0].hp = 60; // now the first lead is the more wounded
        Battle b(party, {foe(0)}, tuning());
        double now = 0.0;
        b.start(now);
        std::vector<BattleEvent> seen;
        REQUIRE(toEnemyAttack(b, now, seen));
        REQUIRE(b.defender() == 0);
        CHECK_FALSE(b.interceptor());
    }
    SUBCASE("no one has the verb") {
        Battle b({lead(), lead()}, {foe(0)}, tuning());
        double now = 0.0;
        b.start(now);
        std::vector<BattleEvent> seen;
        REQUIRE(toEnemyAttack(b, now, seen));
        CHECK_FALSE(b.interceptor());
    }
    SUBCASE("outside an enemy attack") {
        Battle b(pair(50), {foe(0, 5)}, tuning());
        b.start(0.0);
        REQUIRE(b.phase() == Phase::AwaitingCommand);
        CHECK_FALSE(b.interceptor());
    }
}

TEST_CASE("torn: a tearing hit that lands leaves the defender Torn, and it ticks 2% a turn for 3 turns") {
    Battle b({lead()}, {tearingFoe(0, 10)}, tuning()); // two foe turns to each of the lead's
    double now = 0.0;
    b.start(now);
    std::vector<BattleEvent> seen;
    REQUIRE(toEnemyAttack(b, now, seen));
    drain(b);
    takeAll(b, now);
    const auto events = drain(b);
    const BattleEvent* torn = first(events, EventType::TornApplied);
    REQUIRE(torn);
    CHECK(torn->target == 0);
    CHECK(torn->source == 1);
    CHECK(torn->amount == 3);
    CHECK(count(events, EventType::TornApplied) == 1); // once per attack, however many hits land
    CHECK(b.combatant(0).tornTurns == 3);
    CHECK(b.combatant(0).hp == 90);

    // Its next three turns each start with a 2 HP tick (2% of 100); the fourth does not.
    for (int turn = 0; turn < 4; ++turn) {
        seen.clear();
        const int before = b.combatant(0).hp;
        REQUIRE(toPartyTurn(b, now, seen));
        int ticks = 0;
        for (const BattleEvent& e : seen)
            if (e.type == EventType::Hit && e.label == "TORN") {
                ++ticks;
                CHECK(e.source == 0);
                CHECK(e.target == 0);
                CHECK(e.amount == 2);
            }
        CHECK(ticks == (turn < 3 ? 1 : 0));
        CHECK(b.combatant(0).hp == before - (turn < 3 ? 2 : 0)); // the foe's attacks were all parried
        CHECK(b.combatant(0).tornTurns == std::max(0, 2 - turn));
        REQUIRE(b.command(0, 1, now) == CommandError::None);
        const double end = b.combo().note(b.combo().size() - 1).time;
        playCombo(b);
        now = std::max(now, end + 2.0);
    }
    CHECK(b.stats().damageTaken == 10 + 6);
}

TEST_CASE("torn: tornTurns 0 tears no one, and a parried or intercepted tearing hit tears no one") {
    BattleTuning off = tuning();
    off.tornTurns = 0;
    Battle quiet({lead()}, {tearingFoe()}, off);
    double t0 = 0.0;
    quiet.start(t0);
    std::vector<BattleEvent> none;
    REQUIRE(toEnemyAttack(quiet, t0, none));
    takeAll(quiet, t0);
    CHECK(count(drain(quiet), EventType::TornApplied) == 0);
    CHECK(quiet.combatant(0).tornTurns == 0);

    Battle b(pair(), {tearingFoe()}, tuning());
    double now = 0.0;
    b.start(now);
    std::vector<BattleEvent> seen;
    REQUIRE(toEnemyAttack(b, now, seen));
    parryAll(b, seen);
    now = std::max(now, b.defence().impact(b.defence().size() - 1) + 0.01);
    REQUIRE(toEnemyAttack(b, now, seen));
    interceptAll(b, false);
    for (const BattleEvent& e : drain(b)) seen.push_back(e);
    CHECK(count(seen, EventType::TornApplied) == 0);
    CHECK(b.combatant(0).tornTurns == 0);
    CHECK(b.combatant(1).tornTurns == 0);
}

TEST_CASE("take half: evens Load and Torn with an ally and heals both") {
    std::vector<Combatant> party = pair(50, 80);
    party[1].stats.speed = 40; // the first lead acts first
    party[0].load.value = 31;
    party[1].tornTurns = 3;
    party[1].tornShare = 0.04f;
    party[1].hp = 98;
    party[0].hp = 80;
    Battle b(party, {foe(0, 5)}, tuning());
    b.start(0.0);
    REQUIRE(b.phase() == Phase::AwaitingCommand);
    REQUIRE(b.actor() == 0);
    CHECK(b.checkTakeHalf(1) == CommandError::None);
    drain(b);
    REQUIRE(b.takeHalf(1, 0.0) == CommandError::None);
    CHECK(b.phase() == Phase::Recovery);
    CHECK(b.combatant(0).load.value == 16); // 31 split, the odd point with the actor
    CHECK(b.combatant(1).load.value == 15);
    for (const CombatantId id : {CombatantId{0}, CombatantId{1}}) {
        CHECK(b.combatant(id).tornTurns == 3); // one tear becomes two at half strength
        CHECK(b.combatant(id).tornShare == doctest::Approx(0.02f));
    }
    CHECK(b.combatant(0).hp == 85);  // 5% of 100
    CHECK(b.combatant(1).hp == 100); // capped at max
    const auto events = drain(b);
    REQUIRE(count(events, EventType::HalfTaken) == 2);
    const BattleEvent* actorSide = first(events, EventType::HalfTaken);
    CHECK(actorSide->source == 0);
    CHECK(actorSide->target == 0);
    CHECK(actorSide->amount == 5);
    CHECK(b.stats().takeHalves == 1);

    // A Torn member alone (no Load anywhere) still has something to share.
    std::vector<Combatant> torn = pair(50, 100);
    torn[1].stats.speed = 40;
    torn[0].tornTurns = 3;
    torn[0].tornShare = 0.02f;
    Battle t(torn, {foe(0, 5)}, tuning());
    t.start(0.0); // the first lead's turn starts with a tick: 2 turns of Torn left
    REQUIRE(t.combatant(0).tornTurns == 2);
    REQUIRE(t.takeHalf(1, 0.0) == CommandError::None);
    CHECK(t.combatant(1).tornTurns == 2);
    CHECK(t.combatant(1).tornShare == doctest::Approx(0.01f));
}

TEST_CASE("take half: open while overloaded, and sharing lifts the lock") {
    std::vector<Combatant> party = pair(50, 100);
    party[0].load.value = kMaxLoad;
    party[1].stats.speed = 40;
    Battle b(party, {foe(0, 5)}, tuning());
    b.start(0.0);
    REQUIRE(b.actor() == 0);
    CHECK(b.command(0, 2, 0.0) == CommandError::Overloaded);
    REQUIRE(b.takeHalf(1, 0.0) == CommandError::None);
    CHECK(b.combatant(0).load.value == 50);
    CHECK(b.combatant(1).load.value == 50);
    CHECK_FALSE(b.combatant(0).load.overloaded());
}

TEST_CASE("take half: refused without the verb, a living ally, something to share, or the phase") {
    SUBCASE("a lone lead") {
        std::vector<Combatant> one{pair(50)[0]};
        one[0].load.value = 40;
        Battle b(one, {foe(0, 5)}, tuning());
        b.start(0.0);
        CHECK(b.takeHalf(0, 0.0) == CommandError::InvalidTarget); // itself
        CHECK(b.takeHalf(1, 0.0) == CommandError::InvalidTarget); // the enemy
        CHECK(b.takeHalf(7, 0.0) == CommandError::InvalidTarget); // no one
        CHECK(b.phase() == Phase::AwaitingCommand);
    }
    SUBCASE("a downed ally") {
        std::vector<Combatant> party = pair(50);
        party[0].load.value = 40;
        party[1].hp = 0;
        Battle b(party, {foe(0, 5)}, tuning());
        b.start(0.0);
        REQUIRE(b.actor() == 0);
        CHECK(b.takeHalf(1, 0.0) == CommandError::InvalidTarget);
    }
    SUBCASE("without the verb") {
        std::vector<Combatant> party = pair(50);
        party[0].verbs = verbBit(Verb::Intercept);
        party[0].load.value = 40;
        Battle b(party, {foe(0, 5)}, tuning());
        b.start(0.0);
        CHECK(b.takeHalf(1, 0.0) == CommandError::NoSuchVerb);
    }
    SUBCASE("nothing to share") {
        std::vector<Combatant> party = pair(50);
        party[1].stats.speed = 40;
        Battle b(party, {foe(0, 5)}, tuning());
        b.start(0.0);
        REQUIRE(b.actor() == 0);
        CHECK(b.takeHalf(1, 0.0) == CommandError::NothingToShare); // no Load, no Torn
        std::vector<Combatant> evenParty = pair(50);
        evenParty[1].stats.speed = 40;
        evenParty[0].load.value = 20;
        evenParty[1].load.value = 20;
        evenParty[0].tornTurns = 2; // its turn's tick leaves 1, as the ally has
        evenParty[1].tornTurns = 1;
        evenParty[0].tornShare = 0.01f;
        evenParty[1].tornShare = 0.01f;
        Battle even(evenParty, {foe(0, 5)}, tuning());
        even.start(0.0);
        REQUIRE(even.actor() == 0);
        CHECK(even.takeHalf(1, 0.0) == CommandError::NothingToShare); // already even
        CHECK(even.phase() == Phase::AwaitingCommand);
    }
    SUBCASE("outside a command") {
        std::vector<Combatant> party = pair(5);
        party[0].load.value = 40;
        Battle b(party, {foe(0)}, tuning());
        CHECK(b.takeHalf(1, 0.0) == CommandError::WrongPhase); // not started
        double now = 0.0;
        b.start(now);
        std::vector<BattleEvent> seen;
        REQUIRE(toEnemyAttack(b, now, seen));
        CHECK(b.takeHalf(1, now) == CommandError::WrongPhase);
    }
}

TEST_CASE("torn: the enemy data takes an optional skills[].torn") {
    EnemyRoster roster;
    std::string err;
    std::vector<std::string> warnings;
    const auto load = [&](const char* skill) {
        const std::string json = std::string(R"({"enemies":[{"id":"pup","stats":{"maxHp":50},"skills":[)") + skill + "]}]}";
        err.clear();
        warnings.clear();
        return importEnemyRoster(json, roster, &err, &warnings);
    };
    REQUIRE_MESSAGE(load(R"({"id":"bite","power":1.0,"torn":true})"), err);
    CHECK(warnings.empty());
    CHECK(roster.enemies[0].skills[0].torn);
    REQUIRE_MESSAGE(load(R"({"id":"bite","power":1.0})"), err);
    CHECK_FALSE(roster.enemies[0].skills[0].torn);
    CHECK_FALSE(load(R"({"id":"bite","power":1.0,"torn":1})"));
    CHECK(err.find("enemies[0].skills[0].torn") != std::string::npos);
}

namespace {

// The demo foes with every skill tearing: the fight the pilot's Take Half is for.
std::vector<Combatant> tearingDemoFoes() {
    std::vector<Combatant> foes = demo::enemies();
    for (Combatant& c : foes)
        for (Skill& s : c.skills) s.torn = true;
    return foes;
}

} // namespace

TEST_CASE("guard pilot: the demo party wins by intercepting for the second lead, and takes half when torn") {
    for (const double step : {1.0 / 120.0, 0.13}) {
        CAPTURE(step);
        const demo::HeadlessResult r =
            demo::playHeadless(demo::party(4), demo::enemies(), true, step, 1200.0, DefenceInput::Guard);
        CHECK_FALSE(r.commandFailed);
        CHECK(r.outcome == Phase::Victory);
        CHECK(r.intercepts > 0); // the foes go for the second lead, and the first takes the blows
        CHECK(r.loadStored > 0);
        CHECK(r.releases >= 1);
        CHECK(r.takeHalves == 0); // no foe tears
    }
    // A shaky pilot lets some hits land; on tearing foes it evens the Torn with Take Half.
    for (const double step : {1.0 / 120.0, 0.13}) {
        CAPTURE(step);
        const demo::HeadlessResult r = demo::playHeadless(demo::party(4), tearingDemoFoes(), true, step, 1200.0,
                                                          DefenceInput::Guard, {0.85, 0.75});
        CHECK_FALSE(r.commandFailed);
        CHECK(r.outcome == Phase::Victory);
        CHECK(r.intercepts > 0);
        CHECK(r.takeHalves > 0);
    }
    // The idle player still loses.
    const demo::HeadlessResult idle =
        demo::playHeadless(demo::party(4), tearingDemoFoes(), false, 1.0 / 120.0, 1200.0, DefenceInput::Guard);
    CHECK(idle.outcome == Phase::Defeat);
}

TEST_CASE("guard pilot: a whole fight with Intercept, Torn, Take Half and Release allocates nothing per step") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    Battle b(demo::party(4), tearingDemoFoes(), demo::tuning());
    const demo::PilotSkill skill{0.85, 0.75};
    demo::PilotState state;
    const double step = 1.0 / 120.0;
    double now = 0.0;
    b.start(now);
    std::uint64_t allocs = 0;
    int steps = 0;
    BattleStats warm{};
    for (; steps < 200000 && b.phase() != Phase::Victory && b.phase() != Phase::Defeat; ++steps) {
        const auto before = ftcore_test::allocStats().count;
        if (b.phase() == Phase::AwaitingCommand) {
            const CombatantId ally = demo::pilotTakeHalf(b);
            if (ally != demo::kNoAlly) REQUIRE(b.takeHalf(ally, now) == CommandError::None);
        }
        if (b.phase() == Phase::AwaitingCommand) {
            const demo::PilotChoice choice = demo::pilotChoice(b);
            const CommandError e = choice.release ? b.release(now) : b.command(choice.skill, choice.target, now);
            REQUIRE(e == CommandError::None);
        }
        demo::pilotPress(b, now, skill, state);
        demo::pilotDefend(b, now + step, DefenceInput::Guard, skill, state);
        now += step;
        b.update(now);
        b.clearEvents();
        const auto spent = ftcore_test::allocStats().count - before;
        if (now < 20.0) warm = b.stats(); // the first seconds grow the pools
        else allocs += spent;
    }
    CHECK(b.phase() == Phase::Victory);
    // The verbs all came after the warm-up, and none of it allocated.
    CHECK(b.stats().intercepts > warm.intercepts);
    CHECK(b.stats().takeHalves > warm.takeHalves);
    CHECK(b.stats().releases > warm.releases);
    CHECK(allocs == 0);
}
