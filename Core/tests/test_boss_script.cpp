// The data-driven boss state machine (BossScript.h), its loader (importBossScript) and the boss files in
// Data/Battle/boss_script against their fixtures (BossScriptFixtures.h).

#include "BossScriptFixtures.h"
#include "ftcore/ArenaBoss.h"
#include "ftcore/StrikeCount.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <doctest/doctest.h>

#include <limits>
#include <string>
#include <vector>

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
    for (const BossScriptDef& d : {gliderBossDef(), bearerBossDef(), snufferBossDef()}) {
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
    // The point's key kind must be in its lock's keys (always, for a kind other than Shared).
    CHECK(bad([](BossScriptDef& d) { d.key = static_cast<BossKey>(kBossKeyKinds); }));
    CHECK(bad([](BossScriptDef& d) { d.key = BossKey::Refusal; }));
    CHECK(bad([](BossScriptDef& d) {
        d.key = BossKey::Read;
        d.lock.keys = kUnlockSharedBlow | kUnlockRefusal;
    }));
    CHECK(bad([](BossScriptDef& d) {
        d.key = BossKey::Refusal;
        d.lock.keys = kUnlockSharedBlow;
        for (std::size_t i = 0; i < d.moveCount; ++i) {
            d.moves[i].exposeSeconds = 0.0f;
            d.moves[i].unlocksOnCounter = false;
        }
    }));
    CHECK(!bad([](BossScriptDef& d) {
        d.key = BossKey::Read;
        d.lock.keys = kUnlockReadClause;
        d.lock.clause = 9;
    }));
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
    k.hit(1600, false, 0.0); // to phase 1 (lull is in it)
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
    ScriptHit h = s.hit(10, true, 0.5);
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

TEST_CASE("boss script: each key kind opens the exposed point, and only that kind") {
    struct Kind {
        BossKey key;
        UnlockKey right;
        UnlockKey wrong1;
        UnlockKey wrong2;
    };
    const Kind kinds[] = {{BossKey::Shared, kUnlockSharedBlow, kUnlockReadClause, kUnlockRefusal},
                          {BossKey::Read, kUnlockReadClause, kUnlockSharedBlow, kUnlockRefusal},
                          {BossKey::Refusal, kUnlockRefusal, kUnlockSharedBlow, kUnlockReadClause}};
    for (const Kind& k : kinds) {
        CAPTURE(static_cast<int>(k.key));
        BossScriptDef d = smallDef();
        d.key = k.key;
        d.lock.keys = kUnlockAll; // the lock lists every key; the script still takes only its own kind
        d.lock.clause = 7;
        BossScript s;
        REQUIRE(s.reset(d, 0.0));
        CHECK(s.offerKey(k.right, 0.5, 7) == UnlockResult::NotExposed);
        s.update(2.1);
        REQUIRE(s.counter(BossCounter::Guard, 0, 2.1, false) == BossInput::Countered);
        REQUIRE(s.exposed());
        CHECK(s.offerKey(k.wrong1, 2.2, 7) == UnlockResult::WrongKey);
        CHECK(s.offerKey(k.wrong2, 2.2, 7) == UnlockResult::WrongKey);
        CHECK(s.sharedBlow(2.2) == (k.key == BossKey::Shared));
        if (k.key == BossKey::Shared) continue; // the shared blow opened it
        CHECK(!s.unlocked());
        CHECK(s.offerKey(static_cast<UnlockKey>(8), 2.2) == UnlockResult::BadKey);
        if (k.key == BossKey::Read) CHECK(s.offerKey(k.right, 2.25, 8) == UnlockResult::WrongClause);
        CHECK(!s.unlocked());
        CHECK(s.lock().opens() == 0);
        CHECK(s.offerKey(k.right, 2.3, 7) == UnlockResult::Unlocked);
        CHECK(s.unlocked());
        CHECK(s.offerKey(k.right, 2.4, 7) == UnlockResult::Refreshed);
        CHECK(countOf(s.update(2.5), BossEventType::Unlocked) == 2);
        CHECK(s.hit(10, true, 2.5).crit);
        s.update(4.2); // the exposure ran out at 4.1
        CHECK(s.offerKey(k.right, 4.2, 7) == UnlockResult::NotExposed);
    }
}

TEST_CASE("boss script: a counter with unlocksOnCounter unlocks the point with its own key kind") {
    const BossKey keys[] = {BossKey::Shared, BossKey::Read, BossKey::Refusal};
    for (const BossKey key : keys) {
        CAPTURE(static_cast<int>(key));
        BossScriptDef d = smallDef();
        d.key = key;
        d.lock.keys = bossUnlockKey(key);
        d.lock.clause = key == BossKey::Read ? 3 : 0;
        d.moves[0].unlocksOnCounter = true;
        BossScript s;
        REQUIRE(s.reset(d, 0.0));
        s.update(2.1);
        REQUIRE(s.counter(BossCounter::Guard, 0, 2.1, false) == BossInput::Countered);
        CHECK(s.unlocked());
    }
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
    CHECK(s.offerKey(kUnlockSharedBlow, t + 0.1) == UnlockResult::NotExposed); // after the end
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

TEST_CASE("boss script: table 1: a guarded move exposes the point, a shared blow unlocks it") {
    const BossScriptDef g = gliderBossDef();
    StrikeCount count;
    CountTuning ct;
    ct.bpm = 60.0;
    REQUIRE(count.reset(ct, 0.0));
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
    // To the last phase: at a tenth of its health the offer plays; it needs answer().
    s.hit(100000, false, now);
    CHECK(s.hp() == 1);
    CHECK(s.phase() == 2);
    t = untilWindow(s, kBossOffer, now);
    REQUIRE(s.current() == kBossOffer);
    CHECK(s.answer(t));
    CHECK(s.counter(BossCounter::Answer, 0, t, false) == BossInput::Countered);
    CHECK(s.ended());
}

TEST_CASE("boss script: table 2: phase 0 rotation; a carry-together counter unlocks; the offer takes one") {
    const BossScriptDef d = bearerBossDef();
    BossScript s;
    REQUIRE(s.reset(d, 0.0));
    // Phase 0 plays only its two moves.
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

TEST_CASE("boss script: table 3: phases follow the resource; adding it back holds the phase; the ward can fall") {
    const BossScriptDef d = snufferBossDef();
    BossScript s;
    REQUIRE(s.reset(d, 0.0));
    CHECK(s.resource() == 8);
    CHECK(s.addResource(-4, 0.1) == -4);
    CHECK(s.phase() == 0);
    CHECK(s.addResource(3, 0.2) == 3); // resource added (capped at 8)
    CHECK(s.addResource(5, 0.2) == 1);
    CHECK(s.resource() == 8);
    // A landed move with resourceOnLand -1 spends one.
    double t = untilWindow(s, d.find("snuff"), 0.2);
    REQUIRE(s.current() == d.find("snuff"));
    const BossEvents& ev = s.update(t + 1.0);
    const BossEvent* rc = firstOf(ev, BossEventType::ResourceChanged);
    REQUIRE(rc);
    CHECK(rc->value == 7);
    CHECK(rc->amount == -1);
    s.addResource(-4, t + 1.0);
    CHECK(s.phase() == 1); // resource 3
    s.addResource(-2, t + 1.0);
    CHECK(s.phase() == 2); // resource 1
    s.addResource(5, t + 1.0);
    CHECK(s.phase() == 2); // never back

    // The ward: three landed harmsWard moves lose the fight.
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

    // Last phase: after answer(), an on-count counter to the offer ends the fight.
    BossScript e;
    REQUIRE(e.reset(d, 0.0));
    e.addResource(-7, 0.0);
    REQUIRE(e.phase() == 2);
    double v = untilWindow(e, d.find("swallow"), 0.0);
    e.setWatched(false, v); // not watched: the counter lands but opens nothing
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

TEST_CASE("boss script: time: late frames play in order, bad times read as now, far futures are ignored") {
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
    // A time more than kMaxTimeJump ahead is ignored; a real late frame plays its steps.
    BossScript c;
    REQUIRE(c.reset(d, 0.0));
    CHECK(c.update(1e9).empty());
    CHECK(c.now() == doctest::Approx(0.0));
    c.update(kMaxTimeJump);
    CHECK(c.now() > 0.0);
    CHECK(c.now() <= kMaxTimeJump);
    CHECK(c.update(kInfT).empty()); // +inf reads as now: nothing new
}

TEST_CASE("boss script: a fight's ticks and inputs allocate nothing") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    const BossScriptDef d = snufferBossDef();
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

namespace {

bool sameMove(const BossMoveDef& a, const BossMoveDef& b) {
    return a.id() == b.id() && a.counter == b.counter && a.tell == b.tell && a.window == b.window &&
           a.recover == b.recover && a.exposeSeconds == b.exposeSeconds && a.unlocksOnCounter == b.unlocksOnCounter &&
           a.harmsWard == b.harmsWard && a.needsAnswer == b.needsAnswer && a.needsWatch == b.needsWatch &&
           a.resourceOnLand == b.resourceOnLand && a.resourceOnCounter == b.resourceOnCounter;
}

bool sameDef(const BossScriptDef& a, const BossScriptDef& b) {
    if (a.id() != b.id() || a.hp != b.hp || a.gauge != b.gauge || a.resourceMax != b.resourceMax ||
        a.resourceStart != b.resourceStart || a.wardLimit != b.wardLimit || a.critMultiplier != b.critMultiplier ||
        a.restSeconds != b.restSeconds || a.key != b.key || a.lock.keys != b.lock.keys || a.lock.clause != b.lock.clause ||
        a.lock.openSeconds != b.lock.openSeconds || a.lock.relockOnDefer != b.lock.relockOnDefer ||
        a.moveCount != b.moveCount || a.phaseCount != b.phaseCount || !sameMove(a.offer, b.offer))
        return false;
    for (std::size_t i = 0; i < a.moveCount; ++i)
        if (!sameMove(a.moves[i], b.moves[i])) return false;
    for (std::size_t p = 0; p < a.phaseCount; ++p) {
        const BossPhaseDef& x = a.phases[p];
        const BossPhaseDef& y = b.phases[p];
        if ((p > 0 && x.enterAt != y.enterAt) || x.moveCount != y.moveCount) return false; // phase 0: no enterAt
        for (std::size_t i = 0; i < x.moveCount; ++i)
            if (x.moves[i] != y.moves[i]) return false;
    }
    return true;
}

constexpr const char* kSmallBoss = R"({
  "version": 1, "id": "small", "hp": 100,
  "lock": { "keys": ["shared_blow"], "openSeconds": 2 },
  "moves": [ { "id": "jab", "counter": "guard", "tell": 1, "window": 0.5, "recover": 1, "exposeSeconds": 2 } ],
  "offer": { "id": "offer", "counter": "answer", "tell": 1, "window": 2, "recover": 0, "needsAnswer": true },
  "phases": [ { "moves": ["jab"] }, { "enterAt": 0.5, "moves": [] } ]
})";

std::string loadError(const std::string& json) {
    BossScriptDef d;
    std::string error;
    if (importBossScript(json, d, &error)) return "";
    CHECK(d.moveCount == 0); // default on failure
    return error;
}

std::string patchedBoss(std::string_view from, std::string_view to) {
    std::string s = kSmallBoss;
    const std::size_t at = s.find(from);
    REQUIRE(at != std::string::npos);
    s.replace(at, from.size(), to);
    return s;
}

} // namespace

TEST_CASE("boss script data: every file in Data/Battle/boss_script loads without warnings and matches its fixture") {
    const BossScriptDef fixtures[] = {gliderBossDef(), bearerBossDef(), snufferBossDef()};
    for (const BossScriptDef& want : fixtures) {
        const std::string name = std::string(want.id());
        CAPTURE(name);
        std::string text;
        if (!ftcore_test::readProjectData("Battle/boss_script/" + name + ".json", text))
            FAIL("Data/Battle/boss_script file not found");
        BossScriptDef got;
        std::string error;
        std::vector<std::string> warnings;
        REQUIRE_MESSAGE(importBossScript(text, got, &error, &warnings), error);
        CHECK(warnings.empty());
        CHECK(sameDef(got, want));
    }
}

TEST_CASE("boss script data: the loader reports each problem with its path") {
    BossScriptDef d;
    std::vector<std::string> warnings;
    REQUIRE(importBossScript(kSmallBoss, d, nullptr, &warnings));
    CHECK(warnings.empty());
    CHECK(std::string(d.id()) == "small");
    CHECK(d.phaseCount == 2);
    CHECK(d.key == BossKey::Shared); // no "key": the shared blow
    BossScriptDef read;
    REQUIRE(importBossScript(patchedBoss("\"lock\": { \"keys\": [\"shared_blow\"]",
                                         "\"key\": \"read\", \"lock\": { \"keys\": [\"read_clause\"], \"clause\": 3"),
                             read, nullptr, &warnings));
    CHECK(warnings.empty());
    CHECK(read.key == BossKey::Read);
    CHECK(read.lock.clause == 3);
    CHECK(d.phases[1].moveCount == 0);
    CHECK(d.moves[0].exposeSeconds == doctest::Approx(2.0f));

    CHECK(loadError("not json").find("boss script:") == 0);
    CHECK(loadError("[]") == "boss script: expected an object");
    CHECK(loadError(patchedBoss("\"version\": 1", "\"version\": 2")) == "version: expected 1");
    CHECK(loadError(patchedBoss("\"id\": \"small\"", "\"id\": \"Small\"")).find("id: expected an id") == 0);
    CHECK(loadError(patchedBoss("\"hp\": 100", "\"hp\": 0")) == "hp: expected an integer in 1..100000");
    CHECK(loadError(patchedBoss("\"counter\": \"guard\"", "\"counter\": \"parry\"")).find("moves[0].counter:") == 0);
    CHECK(loadError(patchedBoss("\"tell\": 1, \"window\": 0.5", "\"tell\": 9, \"window\": 0.5")) ==
          "moves[0].tell: expected a number in 0.2..5");
    CHECK(loadError(patchedBoss("\"moves\": [\"jab\"]", "\"moves\": [\"kick\"]")) ==
          "phases[0].moves[0]: expected the id of a move");
    CHECK(loadError(patchedBoss("\"id\": \"offer\"", "\"id\": \"jab\"")) == "offer.id: a move already has id jab");
    CHECK(loadError(patchedBoss("{ \"moves\": [\"jab\"] }", "{ \"enterAt\": 1, \"moves\": [\"jab\"] }")) ==
          "phases[0].enterAt: the first phase starts the fight");
    CHECK(loadError(patchedBoss("\"lock\": { \"keys\": [\"shared_blow\"]", "\"lock\": { \"keys\": [\"wish\"]")) ==
          "lock.keys[0]: expected \"shared_blow\", \"read_clause\" or \"refusal\"");
    CHECK(loadError(patchedBoss("\"hp\": 100,", "\"hp\": 100, \"resourceMax\": 4,")) ==
          "resourceMax, resourceStart: only with \"gauge\": \"resource\"");
    CHECK(loadError(patchedBoss("\"hp\": 100,", "\"hp\": 100, \"gauge\": \"mana\",")) ==
          "gauge: expected \"hp\" or \"resource\"");
    // Counts past the fixed arrays.
    const std::string jab =
        R"({ "id": "jab", "counter": "guard", "tell": 1, "window": 0.5, "recover": 1, "exposeSeconds": 2 })";
    std::string six = "\"moves\": [" + jab;
    for (int i = 1; i < 6; ++i) six += ", " + jab;
    six += " ]";
    CHECK(loadError(patchedBoss("\"moves\": [ " + jab + " ]", six)).find("moves: expected an array of 1..5 moves") == 0);
    CHECK(loadError(patchedBoss("{ \"enterAt\": 0.5, \"moves\": [] }",
                                "{ \"enterAt\": 0.5, \"moves\": [] }, { \"enterAt\": 0.4, \"moves\": [] }, "
                                "{ \"enterAt\": 0.3, \"moves\": [] }")) == "phases: expected an array of 1..3 phases");
    CHECK(loadError(patchedBoss("{ \"moves\": [\"jab\"] }",
                                "{ \"moves\": [\"jab\", \"jab\", \"jab\", \"jab\", \"jab\", \"jab\"] }")) ==
          "phases[0].moves: expected an array of up to 5 move ids");
    // Malformed entries.
    CHECK(loadError(patchedBoss("\"moves\": [ " + jab + " ]", "\"moves\": [ " + jab + ", " + jab + " ]")) ==
          "moves[1].id: duplicate id jab");
    CHECK(loadError(patchedBoss("\"moves\": [ " + jab + " ]", "\"moves\": [ 1 ]")).find("moves[0]") == 0);
    CHECK(loadError(patchedBoss("\"moves\": [\"jab\"]", "\"moves\": [3]")) ==
          "phases[0].moves[0]: expected the id of a move");
    CHECK(loadError(patchedBoss("\"hp\": 100,", "\"hp\": 100, \"gauge\": \"resource\", \"resourceMax\": 4, "
                                                "\"resourceStart\": 5,"))
              .find("resourceStart: expected at most resourceMax") == 0);
    // The rules across fields, each with its path.
    CHECK(loadError(patchedBoss("\"enterAt\": 0.5", "\"enterAt\": 1")) ==
          "phases[1].enterAt: expected below the start of the phase before");
    CHECK(loadError(patchedBoss("\"enterAt\": 0.5", "\"enterAt\": 0")) ==
          "phases[1].enterAt: expected above 0 (a share of hp)");
    CHECK(loadError(patchedBoss("\"hp\": 100,", "\"hp\": 100, \"gauge\": \"resource\", \"resourceMax\": 4, "
                                                "\"resourceStart\": 4,")) ==
          "phases[1].enterAt: expected a whole number of the resource");
    CHECK(loadError(patchedBoss("\"moves\": [\"jab\"]", "\"moves\": [\"jab\", \"jab\"]")) ==
          "phases[0].moves[1]: the move is listed twice");
    CHECK(loadError(patchedBoss("{ \"moves\": [\"jab\"] }", "{ \"moves\": [] }")) ==
          "phases[0].moves: only the last phase may be empty");
    CHECK(loadError(patchedBoss("\"keys\": [\"shared_blow\"]", "\"keys\": [\"refusal\"]")) ==
          "lock.keys: a move exposes or unlocks the weak point, so expected \"shared_blow\"");
    CHECK(loadError(patchedBoss("\"hp\": 100,", "\"hp\": 100, \"key\": \"refusal\",")) ==
          "lock.keys: a move exposes or unlocks the weak point, so expected \"refusal\"");
    CHECK(loadError(patchedBoss("\"hp\": 100,", "\"hp\": 100, \"key\": \"wish\",")) ==
          "key: expected \"shared\", \"read\" or \"refusal\"");
    {
        std::string s = patchedBoss("\"hp\": 100,", "\"hp\": 100, \"key\": \"read\",");
        const std::string from = ", \"exposeSeconds\": 2 }";
        const std::size_t at = s.find(from);
        REQUIRE(at != std::string::npos);
        s.replace(at, from.size(), " }");
        CHECK(loadError(s) == "lock.keys: the point's key needs \"read_clause\"");
    }
    CHECK(loadError(patchedBoss("\"hp\": 100,", "\"hp\": 100, \"wardLimit\": 2,")) ==
          "wardLimit: no move has harmsWard");
    CHECK(loadError(patchedBoss("\"needsAnswer\": true", "\"needsAnswer\": true, \"harmsWard\": true")) ==
          "offer.harmsWard: the offer cannot harm the ward");
    // Two problems, two lines.
    const std::string two = loadError(patchedBoss("\"hp\": 100", "\"hp\": 0, \"wardLimit\": 99"));
    CHECK(two.find('\n') != std::string::npos);
    // Unknown keys are warnings, not errors.
    warnings.clear();
    CHECK(importBossScript(patchedBoss("\"hp\": 100", "\"hp\": 100, \"colour\": 1"), d, nullptr, &warnings));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0] == "unknown key: colour");
}

TEST_CASE("locked point: the one adapter over the lock") {
    LockedPoint none;
    none.reset(false, LockTuning{}, 0.0);
    CHECK(!none.locked());
    CHECK(none.open(0.0));
    CHECK(none.offer(kUnlockSharedBlow, 1.0) == UnlockResult::BadKey);
    CHECK(!none.onDeferred(1.0));
    CHECK(!none.relock(1.0));

    LockTuning t;
    t.keys = static_cast<std::uint8_t>(kUnlockSharedBlow | kUnlockRefusal);
    t.openSeconds = 2.0;
    LockedPoint p;
    p.reset(true, t, 0.0);
    CHECK(p.locked());
    CHECK(!p.open(0.0));
    CHECK(LockedPoint::opened(p.offer(kUnlockSharedBlow, 1.0)));
    CHECK(p.open(1.5));
    CHECK(p.until() == doctest::Approx(3.0));
    CHECK(LockedPoint::opened(p.offer(kUnlockRefusal, 1.5))); // refreshed
    CHECK(!LockedPoint::opened(p.offer(kUnlockReadClause, 1.6)));
    CHECK(p.relock(1.7));
    CHECK(!p.open(1.7));
    CHECK(!p.relock(1.8)); // already shut
    CHECK(LockedPoint::opened(p.offer(kUnlockSharedBlow, 2.0)));
    CHECK(p.onDeferred(2.1));
    CHECK(p.lock().opens() == 2);
    CHECK(p.lock().deferrals() == 1);
}

TEST_CASE("boss script: a script reset late on the clock unlocks for the full window") {
    const BossScriptDef d = smallDef();
    BossScript s;
    REQUIRE(s.reset(d, 7200.0));
    s.update(7202.1);
    REQUIRE(s.counter(BossCounter::Guard, 0, 7202.1, false) == BossInput::Countered);
    CHECK(s.sharedBlow(7202.3));
    CHECK(s.unlocked());
    CHECK(s.hit(10, true, 7202.4).crit);
}

TEST_CASE("boss script: a time more than kMaxTimeJump ahead is ignored and not stored") {
    const BossScriptDef d = smallDef();
    BossScript a;
    BossScript b;
    REQUIRE(a.reset(d, 0.0));
    REQUIRE(b.reset(d, 0.0));
    CHECK(a.update(1e12).empty());
    for (double t : {13.0, 14.0}) {
        CHECK(a.update(t).size() == b.update(t).size());
        CHECK(a.now() == doctest::Approx(b.now()));
        CHECK(a.state() == b.state());
        CHECK(a.current() == b.current());
    }
    // Exactly kMaxTimeJump ahead still counts.
    CHECK(a.update(14.0 + kMaxTimeJump).size() == b.update(14.0 + kMaxTimeJump).size());
    CHECK(a.now() == doctest::Approx(14.0 + kMaxTimeJump));
}

TEST_CASE("boss script: a long quiet stretch before the shared blow leaves the lock in step") {
    const BossScriptDef d = smallDef();
    BossScript s;
    REQUIRE(s.reset(d, 0.0));
    // 700 s: past kMaxTimeJump, so a lock that did not follow its owner would ignore the offer.
    for (double t = 0.5; t <= 700.0; t += 0.5) s.update(t);
    // Bring up an exposing counter at the next tell, as the first locked-point test does.
    double t = 700.0;
    BossInput in = BossInput::Early;
    while (t < 720.0) {
        s.update(t);
        in = s.counter(BossCounter::Guard, 0, t, false);
        if (in == BossInput::Countered) break;
        t += 0.05;
    }
    REQUIRE(in == BossInput::Countered);
    REQUIRE(s.sharedBlow(t + 0.1));
    CHECK(s.unlocked());
    s.update(t + 2.0); // openSeconds 2 from the blow
    CHECK(s.unlocked());
    s.update(t + 2.2);
    CHECK_FALSE(s.unlocked());
}

TEST_CASE("boss script: one update stops at the step cap and the next ones catch up") {
    BossScriptDef d = smallDef();
    d.restSeconds = 0.0f;
    for (std::size_t i = 0; i < d.moveCount; ++i) {
        d.moves[i].tell = 0.2f;
        d.moves[i].window = 0.1f;
        d.moves[i].recover = 0.0f;
    }
    BossScript s;
    REQUIRE(s.reset(d, 0.0));
    // About 13 steps a second: 500 s is far more than one update's 4096 steps.
    const BossEvents& first = s.update(500.0);
    CHECK(first.count + first.dropped > 0);
    const double lag = s.now();
    CHECK(lag > 100.0);
    CHECK(lag < 500.0);
    CHECK_FALSE(s.ended());
    // The next call is judged against the latest time seen (500), not the lagging step position: a time
    // 550 s on is still within the jump bound, so it plays.
    s.update(1050.0);
    CHECK(s.now() > lag);
    double t = 1050.0;
    for (int i = 0; i < 8 && s.now() < t; ++i) s.update(t);
    CHECK(s.now() == t);
    // Past the bound from the latest time: ignored.
    s.update(t + 700.0);
    CHECK(s.now() == t);
}

TEST_CASE("boss script: checkpointAtPhase is optional, false by default, and must be true or false") {
    BossScriptDef d;
    std::vector<std::string> warnings;
    REQUIRE(importBossScript(kSmallBoss, d, nullptr, &warnings));
    CHECK_FALSE(d.checkpointAtPhase);
    warnings.clear();
    REQUIRE(importBossScript(patchedBoss("\"hp\": 100", "\"hp\": 100, \"checkpointAtPhase\": true"), d, nullptr, &warnings));
    CHECK(d.checkpointAtPhase);
    CHECK(warnings.empty()); // a known key
    CHECK(loadError(patchedBoss("\"hp\": 100", "\"hp\": 100, \"checkpointAtPhase\": \"yes\"")).find(
              "checkpointAtPhase: expected true or false") != std::string::npos);
}
