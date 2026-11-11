// The data-driven boss state machine (BossScript.h) and the first three boss tables (BossScriptData.h).

#include "ftcore/BossScriptData.h"
#include "ftcore/StrikeCount.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <limits>
#include <string>

using namespace ftcore;

namespace {

constexpr double kInfT = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

int countOf(const BossEvents& ev, BossEventType type) {
    int n = 0;
    for (const BossEvent& e : ev)
        if (e.type == type) ++n;
    return n;
}
const BossEvent* firstOf(const BossEvents& ev, BossEventType type) {
    for (const BossEvent& e : ev)
        if (e.type == type) return &e;
    return nullptr;
}

// Plays until the named move's window is open (or `limit` seconds pass). Returns the time.
double untilWindow(BossScript& s, std::uint8_t move, double t, double limit = 120.0) {
    const double end = t + limit;
    while (t < end) {
        if (s.state() == BossState::Window && s.current() == move) return t;
        t = s.stepEnds() > t ? s.stepEnds() : t + 0.01;
        s.update(t);
    }
    return t;
}

// A small two-phase boss: hp gauge, three moves.
BossScriptDef smallDef() {
    BossScriptDef d;
    d.setId("small");
    d.hp = 100;
    d.restSeconds = 1.0f;
    d.lock.keys = kUnlockSharedBlow;
    d.lock.openSeconds = 2.0;
    d.moves[0].setId("jab");
    d.moves[0].counter = BossCounter::Guard;
    d.moves[0].tell = 1.0f;
    d.moves[0].window = 0.5f;
    d.moves[0].recover = 1.0f;
    d.moves[0].exposeSeconds = 2.0f;
    d.moves[1].setId("sweep");
    d.moves[1].counter = BossCounter::Dodge;
    d.moves[1].tell = 1.0f;
    d.moves[1].window = 0.5f;
    d.moves[1].recover = 1.0f;
    d.moves[2].setId("press");
    d.moves[2].counter = BossCounter::CarryTogether;
    d.moves[2].tell = 1.0f;
    d.moves[2].window = 1.0f;
    d.moves[2].recover = 1.0f;
    d.moves[2].unlocksOnCounter = true;
    d.moveCount = 3;
    d.offer.setId("offer");
    d.offer.counter = BossCounter::Answer;
    d.offer.tell = 1.0f;
    d.offer.window = 2.0f;
    d.offer.needsAnswer = true;
    d.phases[0].moves = {0, 1};
    d.phases[0].moveCount = 2;
    d.phases[1].enterAt = 0.5f;
    d.phases[1].moves = {2};
    d.phases[1].moveCount = 1;
    d.phaseCount = 2;
    return d;
}

} // namespace

TEST_CASE("boss script: the three boss tables are valid, five moves each, three phases") {
    for (const BossScriptDef& d : {gliderBossDef(), bearerBossDef(), lampBossDef()}) {
        CAPTURE(std::string(d.id()));
        CHECK(validBossScript(d));
        CHECK(d.moveCount == 5);
        CHECK(d.phaseCount == 3);
        CHECK((d.lock.keys & kUnlockSharedBlow) != 0);
        bool opens = false;
        for (std::size_t i = 0; i < d.moveCount; ++i)
            opens = opens || d.moves[i].exposeSeconds > 0.0f || d.moves[i].unlocksOnCounter;
        CHECK(opens); // some move leads to the weak point
        BossScript s;
        CHECK(s.reset(d, 0.0));
    }
    CHECK(gliderBossDef().find("swoop") == 0);
    CHECK(gliderBossDef().find("nope") == kBossNoMove);
    BossScript idle;
    CHECK(idle.move(0).id().empty()); // no def: an empty move
    const BossScriptDef g = gliderBossDef();
    REQUIRE(idle.reset(g, 0.0));
    CHECK(idle.move(kBossNoMove).id().empty());
    CHECK(std::string(idle.move(kBossOffer).id()) == "offer");
}

TEST_CASE("boss script: validation rejects bad defs and the script stays inert") {
    const BossScriptDef good = smallDef();
    REQUIRE(validBossScript(good));
    auto bad = [&](auto edit) {
        BossScriptDef d = good;
        edit(d);
        return !validBossScript(d);
    };
    CHECK(bad([](BossScriptDef& d) { d.setId(""); }));
    CHECK(bad([](BossScriptDef& d) { d.setId("Upper"); }));
    CHECK(bad([](BossScriptDef& d) { d.hp = 0; }));
    CHECK(bad([](BossScriptDef& d) { d.critMultiplier = static_cast<float>(kNaN); }));
    CHECK(bad([](BossScriptDef& d) { d.moveCount = 0; }));
    CHECK(bad([](BossScriptDef& d) { d.moveCount = 6; }));
    CHECK(bad([](BossScriptDef& d) { d.moves[1].setId("jab"); }));          // duplicate id
    CHECK(bad([](BossScriptDef& d) { d.offer.setId("jab"); }));             // offer shares an id
    CHECK(bad([](BossScriptDef& d) { d.moves[0].tell = 0.1f; }));
    CHECK(bad([](BossScriptDef& d) { d.moves[0].window = 11.0f; }));
    CHECK(bad([](BossScriptDef& d) { d.moves[0].counter = static_cast<BossCounter>(99); }));
    CHECK(bad([](BossScriptDef& d) { d.phases[0].moves = {0, 0}; }));       // repeated in a rotation
    CHECK(bad([](BossScriptDef& d) { d.phases[0].moves = {0, 4}; }));       // unknown move
    CHECK(bad([](BossScriptDef& d) { d.phases[0].moveCount = 0; }));        // only the last may be empty
    CHECK(bad([](BossScriptDef& d) { d.phases[1].enterAt = 1.0f; }));       // must fall
    CHECK(bad([](BossScriptDef& d) { d.phases[1].enterAt = 0.0f; }));
    CHECK(bad([](BossScriptDef& d) { d.lock.keys = kUnlockRefusal; }));     // exposes, but no shared key
    CHECK(bad([](BossScriptDef& d) { d.wardLimit = 2; }));                  // a ward nothing harms
    CHECK(bad([](BossScriptDef& d) { d.offer.harmsWard = true; }));
    CHECK(bad([](BossScriptDef& d) {
        d.gauge = BossGauge::Resource;
        d.resourceMax = 4;
        d.resourceStart = 4;
        d.phases[1].enterAt = 1.5f; // not whole
    }));
    CHECK(!bad([](BossScriptDef& d) {
        d.gauge = BossGauge::Resource;
        d.resourceMax = 4;
        d.resourceStart = 4;
        d.phases[1].enterAt = 2.0f;
    }));
    CHECK(!bad([](BossScriptDef& d) { d.phases[1].moveCount = 0; })); // the last phase may play the offer alone

    BossScriptDef broken = good;
    broken.hp = 0;
    BossScript s;
    CHECK(!s.reset(broken, 0.0));
    CHECK(!s.valid());
    CHECK(s.counter(BossCounter::Guard, 0, 1.0, false) == BossInput::Ignored);
    CHECK(s.hit(10, true, 1.0).damage == 0);
    CHECK(s.update(100.0).empty());
    CHECK(!s.ended());
}

TEST_CASE("boss script: a move is tell, window, recovery; uncountered it lands") {
    const BossScriptDef d = smallDef();
    BossScript s;
    REQUIRE(s.reset(d, 0.0));
    CHECK(s.state() == BossState::Rest);
    const BossEvents& a = s.update(1.0);
    REQUIRE(a.size() == 1);
    CHECK(a[0].type == BossEventType::Tell);
    CHECK(a[0].move == 0);
    CHECK(a[0].until == doctest::Approx(2.0));
    CHECK(s.counter(BossCounter::Guard, 0, 1.5, false) == BossInput::Early);
    s.update(2.0);
    CHECK(s.state() == BossState::Window);
    const BossEvents& b = s.update(2.6); // window closed at 2.5
    CHECK(countOf(b, BossEventType::Landed) == 1);
    CHECK(s.state() == BossState::Recover);
    CHECK(s.counter(BossCounter::Guard, 0, 2.7, false) == BossInput::Late);
    const BossEvents& c = s.update(4.6); // recovered at 3.5, rest to 4.5, next tell
    CHECK(countOf(c, BossEventType::Recovered) == 1);
    const BossEvent* tell = firstOf(c, BossEventType::Tell);
    REQUIRE(tell);
    CHECK(tell->move == 1); // the rotation moves on
    CHECK(tell->at == doctest::Approx(4.5));
}

TEST_CASE("boss script: counters: right kind in the window, shared ones need the shared input") {
    const BossScriptDef d = smallDef();
    BossScript s;
    REQUIRE(s.reset(d, 0.0));
    s.update(2.1);
    REQUIRE(s.state() == BossState::Window);
    CHECK(s.counter(BossCounter::Dodge, 0, 2.2, false) == BossInput::WrongCounter);
    CHECK(s.counter(BossCounter::Guard, kEnemyFighter, 2.2, false) == BossInput::Ignored);
    CHECK(s.counter(static_cast<BossCounter>(77), 0, 2.2, false) == BossInput::Ignored);
    CHECK(s.counter(BossCounter::Guard, 0, 2.2, false) == BossInput::Countered);
    CHECK(s.state() == BossState::Recover);
    const BossEvents& ev = s.update(2.2);
    CHECK(countOf(ev, BossEventType::Countered) == 1);
    CHECK(countOf(ev, BossEventType::Landed) == 0);
    CHECK(countOf(ev, BossEventType::Exposed) == 1);
    CHECK(s.exposed());
    CHECK(s.exposedUntil() == doctest::Approx(4.2));

    // A shared counter without the shared input is refused; with it, taken.
    const BossScriptDef g = gliderBossDef();
    BossScript k;
    REQUIRE(k.reset(g, 0.0));
    k.hit(1600, false, 0.0); // to the grounded phase (lull is in it)
    const double t = untilWindow(k, g.find("lull"), 0.0);
    REQUIRE(k.current() == g.find("lull"));
    CHECK(k.counter(BossCounter::StrikeOnCount, 0, t, false) == BossInput::NeedsShared);
    CHECK(k.counter(BossCounter::StrikeOnCount, 0, t, true) == BossInput::Countered);
}

TEST_CASE("boss script: the locked weak point: exposed by a counter, opened by a shared blow, crits only then") {
    const BossScriptDef d = smallDef();
    BossScript s;
    REQUIRE(s.reset(d, 0.0));
    CHECK(!s.unlocked());
    BossHit h = s.hit(10, true, 0.5);
    CHECK(!h.crit);
    CHECK(h.damage == 10);
    CHECK(!s.sharedBlow(0.6)); // not exposed: nothing
    s.update(2.1);
    REQUIRE(s.counter(BossCounter::Guard, 0, 2.1, false) == BossInput::Countered);
    CHECK(!s.hit(5, true, 2.2).crit); // exposed but still locked
    CHECK(s.sharedBlow(2.3));
    CHECK(s.unlocked());
    h = s.hit(10, true, 2.4);
    CHECK(h.crit);
    CHECK(h.damage == 20);
    CHECK(!s.hit(10, false, 2.4).crit); // not on the point
    const BossEvents& ev = s.update(2.5);
    CHECK(countOf(ev, BossEventType::Unlocked) == 1);
    const BossEvent* crit = firstOf(ev, BossEventType::Crit);
    REQUIRE(crit);
    CHECK(crit->value == 20);
    // The lock shuts at 4.3, the exposure at 4.1.
    const BossEvents& later = s.update(4.4);
    CHECK(countOf(later, BossEventType::ExposeEnded) == 1);
    CHECK(!s.unlocked());
    CHECK(!s.hit(10, true, 4.5).crit);
    CHECK(!s.sharedBlow(4.6)); // no longer exposed
}

TEST_CASE("boss script: hp never falls below 1; phases follow the gauge and never go back") {
    const BossScriptDef d = smallDef();
    BossScript s;
    REQUIRE(s.reset(d, 0.0));
    CHECK(s.hit(-5, false, 0.1).damage == 0);
    CHECK(s.hit(30, false, 0.1).damage == 30);
    CHECK(s.phase() == 0);
    CHECK(s.hit(20, false, 0.2).damage == 20); // 50/100: at enterAt
    CHECK(s.phase() == 1);
    const BossEvents& ev = s.update(0.3);
    const BossEvent* pc = firstOf(ev, BossEventType::PhaseChanged);
    REQUIRE(pc);
    CHECK(pc->value == 1);
    CHECK(s.hit(1000, false, 0.4).damage == 49);
    CHECK(s.hp() == 1);
    CHECK(s.hit(1000, false, 0.5).damage == 0);
    CHECK(!s.ended()); // zero health is not the end
    CHECK(s.phase() == 1);
}

TEST_CASE("boss script: the end is the answered offer") {
    const BossScriptDef d = smallDef();
    BossScript s;
    REQUIRE(s.reset(d, 0.0));
    CHECK(!s.answer(0.1)); // only in the last phase
    s.hit(60, false, 0.1);
    REQUIRE(s.phase() == 1);
    // The last phase plays the offer first.
    double t = untilWindow(s, kBossOffer, 0.1);
    REQUIRE(s.current() == kBossOffer);
    CHECK(s.counter(BossCounter::Answer, 0, t, false) == BossInput::NeedsAnswer);
    // Unanswered, the offer lands and the rotation goes on to the phase's move, then back to the offer.
    t = untilWindow(s, 2, t);
    REQUIRE(s.current() == 2);
    t = untilWindow(s, kBossOffer, t);
    REQUIRE(s.current() == kBossOffer);
    CHECK(s.answer(t));
    CHECK(s.counter(BossCounter::Answer, 1, t + 0.1, false) == BossInput::Countered);
    CHECK(s.ended());
    const BossEvents& ev = s.update(t + 0.2);
    CHECK(countOf(ev, BossEventType::Ended) == 1);
    CHECK(s.counter(BossCounter::Guard, 0, t + 0.3, false) == BossInput::Over);
    CHECK(s.hit(5, false, t + 0.3).damage == 0);
    CHECK(s.update(t + 100.0).empty()); // nothing more plays
}

TEST_CASE("boss script: carrying together needs two different fighters on the count") {
    const BossScriptDef d = smallDef();
    BossScript s;
    REQUIRE(s.reset(d, 0.0));
    s.hit(60, false, 0.0);
    const double t = untilWindow(s, 2, 0.0);
    REQUIRE(s.current() == 2);
    CHECK(s.counter(BossCounter::CarryTogether, 0, t, false) == BossInput::NeedsShared);
    CHECK(s.counter(BossCounter::CarryTogether, 0, t, true) == BossInput::NeedsPartner);
    CHECK(s.counter(BossCounter::CarryTogether, 0, t + 0.1, true) == BossInput::NeedsPartner); // same fighter
    CHECK(s.counter(BossCounter::CarryTogether, 2, t + 0.2, true) == BossInput::Countered);
    CHECK(s.unlocked()); // unlocksOnCounter
    const BossEvents& ev = s.update(t + 0.3);
    CHECK(countOf(ev, BossEventType::Unlocked) == 1);
    CHECK(s.hit(10, true, t + 0.4).crit);
}

TEST_CASE("boss script: glider: a guarded swoop pins it, a blow on the count opens the weak point") {
    const BossScriptDef g = gliderBossDef();
    StrikeCount count;
    CountTuning ct;
    ct.bpm = 60.0;
    REQUIRE(count.reset(ct));
    BossScript s;
    REQUIRE(s.reset(g, 0.0));
    double t = untilWindow(s, g.find("swoop"), 0.0);
    REQUIRE(s.counter(BossCounter::Guard, 0, t, false) == BossInput::Countered);
    REQUIRE(s.exposed());
    // Two fighters land together on a called count: the blow is shared.
    REQUIRE(count.call(0, t, CountVoice::Voice) == CountError::None);
    const double now = t + 2.0;
    count.update(now);
    const CountHit a = count.judgeBlow(1, now);
    const CountHit b = count.judgeBlow(0, now);
    CHECK(a.outcome == CountOutcome::Held);
    REQUIRE(b.outcome == CountOutcome::OnCount);
    CHECK(s.sharedBlow(now) == sharedBlow(b));
    CHECK(s.unlocked());
    CHECK(s.hit(100, true, now).damage == 250);
    // To the last phase: at a tenth of its health the offer comes, answered by a bystander.
    s.hit(100000, false, now);
    CHECK(s.hp() == 1);
    CHECK(s.phase() == 2);
    t = untilWindow(s, kBossOffer, now);
    REQUIRE(s.current() == kBossOffer);
    CHECK(s.answer(t));
    CHECK(s.counter(BossCounter::Answer, 0, t, false) == BossInput::Countered);
    CHECK(s.ended());
}

TEST_CASE("boss script: bearer: carry phase is the turn and the shoulder; the free end opens it; carried home") {
    const BossScriptDef d = bearerBossDef();
    BossScript s;
    REQUIRE(s.reset(d, 0.0));
    // Phase one plays only the turn and the shoulder shift.
    double t = 0.0;
    for (int i = 0; i < 6; ++i) {
        const BossEvents& ev = s.update(t += 1.0);
        for (const BossEvent& e : ev)
            if (e.type == BossEventType::Tell) CHECK((e.move == d.find("turn") || e.move == d.find("shoulder_shift")));
    }
    s.hit(3000, false, t); // to the last phase
    REQUIRE(s.phase() == 2);
    t = untilWindow(s, d.find("free_end"), t);
    REQUIRE(s.current() == d.find("free_end"));
    CHECK(s.counter(BossCounter::CarryTogether, 0, t, true) == BossInput::NeedsPartner);
    CHECK(s.counter(BossCounter::CarryTogether, 1, t, true) == BossInput::Countered);
    CHECK(s.unlocked());
    t = untilWindow(s, kBossOffer, t);
    REQUIRE(s.current() == kBossOffer);
    CHECK(s.counter(BossCounter::CarryTogether, 2, t, true) == BossInput::NeedsPartner);
    CHECK(s.counter(BossCounter::CarryTogether, 1, t, true) == BossInput::Countered); // no answer() needed
    CHECK(s.ended());
}

TEST_CASE("boss script: lamp eater: phases follow the lamps; relit lamps hold it back; the ward can fall") {
    const BossScriptDef d = lampBossDef();
    BossScript s;
    REQUIRE(s.reset(d, 0.0));
    CHECK(s.resource() == 8);
    CHECK(s.addResource(-4, 0.1) == -4);
    CHECK(s.phase() == 0);
    CHECK(s.addResource(3, 0.2) == 3); // a lamp relit (capped at 8)
    CHECK(s.addResource(5, 0.2) == 1);
    CHECK(s.resource() == 8);
    // A snuff that lands eats a lamp.
    double t = untilWindow(s, d.find("snuff"), 0.2);
    REQUIRE(s.current() == d.find("snuff"));
    const BossEvents& ev = s.update(t + 1.0);
    const BossEvent* rc = firstOf(ev, BossEventType::ResourceChanged);
    REQUIRE(rc);
    CHECK(rc->value == 7);
    CHECK(rc->amount == -1);
    s.addResource(-4, t + 1.0);
    CHECK(s.phase() == 1); // three lamps
    s.addResource(-2, t + 1.0);
    CHECK(s.phase() == 2); // one lamp
    s.addResource(5, t + 1.0);
    CHECK(s.phase() == 2); // never back

    // The ward: three uncountered reaches lose the fight.
    BossScript w;
    REQUIRE(w.reset(d, 0.0));
    double u = 0.0;
    for (int i = 0; i < 3; ++i) {
        u = untilWindow(w, d.find("reach"), u);
        REQUIRE(w.current() == d.find("reach"));
        w.update(u += 1.0);
    }
    CHECK(w.lost());
    CHECK(w.wardHarm() == 3);
    CHECK(w.counter(BossCounter::TakeHalf, 0, u, true) == BossInput::Over);

    // The end: in the last phase the bystander answers, a blow on the count finishes it.
    BossScript e;
    REQUIRE(e.reset(d, 0.0));
    e.addResource(-7, 0.0);
    REQUIRE(e.phase() == 2);
    double v = untilWindow(e, d.find("swallow"), 0.0);
    e.setWatched(false, v); // no lamp behind it: the counter lands but opens nothing
    CHECK(!e.watched());
    REQUIRE(e.counter(BossCounter::StrikeOnCount, 0, v, true) == BossInput::Countered);
    CHECK(!e.unlocked());
    e.setWatched(true, v);
    v = untilWindow(e, d.find("swallow"), v + 0.1);
    REQUIRE(e.current() == d.find("swallow"));
    REQUIRE(e.counter(BossCounter::StrikeOnCount, 0, v, true) == BossInput::Countered);
    CHECK(e.unlocked());
    CHECK(e.hit(100, true, v).crit);
    v = untilWindow(e, kBossOffer, v);
    REQUIRE(e.current() == kBossOffer);
    CHECK(e.counter(BossCounter::StrikeOnCount, 0, v, true) == BossInput::NeedsAnswer);
    CHECK(e.answer(v));
    CHECK(e.counter(BossCounter::StrikeOnCount, 0, v, true) == BossInput::Countered);
    CHECK(e.ended());
}

TEST_CASE("boss script: time: late frames play in order, bad times read as now, far futures are bounded") {
    const BossScriptDef d = smallDef();
    BossScript a;
    BossScript b;
    REQUIRE(a.reset(d, 0.0));
    REQUIRE(b.reset(d, 0.0));
    // One late frame plays the same steps as many small ones.
    int stepsA = 0;
    for (double t = 0.05; t <= 10.0; t += 0.05) stepsA += static_cast<int>(a.update(t).size());
    const BossEvents& late = b.update(10.0);
    CHECK(static_cast<int>(late.size()) == stepsA);
    for (std::size_t i = 1; i < late.size(); ++i) CHECK(late[i - 1].at <= late[i].at);
    CHECK(a.state() == b.state());
    CHECK(a.current() == b.current());
    // NaN and earlier times read as the latest seen.
    CHECK(b.update(kNaN).empty());
    CHECK(b.update(3.0).empty());
    CHECK(b.now() == doctest::Approx(10.0));
    // A far-future time plays a bounded number of steps per call, and keeps going on the next.
    BossScript c;
    REQUIRE(c.reset(d, 0.0));
    c.update(1e9);
    CHECK(c.now() < 1e9);
    const double before = c.now();
    c.update(1e9);
    CHECK(c.now() > before);
    CHECK(c.update(kInfT).empty()); // +inf reads as now: nothing new
}

TEST_CASE("boss script: a fight's ticks and inputs allocate nothing") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    const BossScriptDef d = lampBossDef();
    BossScript s;
    REQUIRE(s.reset(d, 0.0));
    const auto before = ftcore_test::allocStats().count;
    double t = 0.0;
    for (int i = 0; i < 2000 && !s.ended() && !s.lost(); ++i) {
        t += 1.0 / 60.0;
        s.update(t);
        if (s.state() == BossState::Window) {
            s.counter(s.move(s.current()).counter, 0, t, true);
            s.counter(s.move(s.current()).counter, 1, t, true);
        }
        s.hit(3, (i % 7) == 0, t);
        s.sharedBlow(t);
        if (i % 120 == 0) s.addResource(-1, t);
        if (s.phase() == 2) s.answer(t);
    }
    CHECK(ftcore_test::allocStats().count == before);
}
