#include "ftcore/FightSession.h"

#include "AllocCounter.h"

#include <cmath>
#include <doctest/doctest.h>

#include <limits>
#include <string>

using namespace ftcore;

namespace {

constexpr double kFrame = 1.0 / 60.0;

// Ticks frames until `done` or `limit` frames; returns whether done.
template <class F>
bool tickUntil(FightSession& s, F done, int limit = 2000) {
    for (int i = 0; i < limit; ++i) {
        if (done()) return true;
        s.tick(kFrame);
    }
    return done();
}

std::size_t countOf(const TableEvents& ev, TableEventType type) {
    std::size_t n = 0;
    for (const TableEvent& e : ev)
        if (e.type == type) ++n;
    return n;
}

// A two-phase boss: one move countered by Guard that exposes the point for 2 s; the lock opens on a shared blow.
BossScriptDef oneMoveBoss() {
    BossScriptDef d;
    d.setId("one");
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
    d.moveCount = 1;
    d.offer.setId("offer");
    d.offer.counter = BossCounter::Answer;
    d.offer.tell = 1.0f;
    d.offer.window = 2.0f;
    d.offer.needsAnswer = true;
    d.phases[0].moves = {0};
    d.phases[0].moveCount = 1;
    d.phases[1].enterAt = 0.5f; // the last phase plays the offer first, so the jab leads a first phase
    d.phases[1].moves = {0};
    d.phases[1].moveCount = 1;
    d.phaseCount = 2;
    return d;
}

// A small press registry: side 0 kinds on w_a0..2 (w_a0 capped at 3), side 1 on w_b0..2, side 1 open while f_open.
const char* kPressCsv =
    "slot,id,kind,max,default,mut,set_scenes,gate,read_scenes,fallback,derive\n"
    "1,f_open,NUM,3,0,free,any,-,-,-,-\n"
    "0,w_a0,CTR,3,0,-,any,-,-,-,-\n"
    "1,w_a1,CTR,65535,0,-,any,-,-,-,-\n"
    "2,w_a2,CTR,65535,0,-,any,-,-,-,-\n"
    "3,w_b0,CTR,65535,0,-,any,-,-,-,-\n"
    "4,w_b1,CTR,65535,0,-,any,-,-,-,-\n"
    "5,w_b2,CTR,65535,0,-,any,-,-,-,-\n";

const char* kPressJson = R"({
  "version": 2,
  "scenes": ["s1"],
  "roles": {
    "pressCounters": [["w_a0", "w_a1", "w_a2"], ["w_b0", "w_b1", "w_b2"]],
    "sideOpen": ["", "f_open"]
  }
})";

// A gift tuning with no cooldowns, so gifts can be given back to back.
GiftTuning freeGifts() {
    GiftTuning g;
    g.cooldown = {0.0, 0.0, 0.0};
    g.healsPerFight = 20;
    return g;
}

} // namespace

TEST_CASE("fight session: one clock that starts anywhere and only moves by a clamped step") {
    FightSession s;
    CHECK(s.begin(FightSessionDef{}, 5000.0).ok());
    CHECK(s.now() == 5000.0);
    CHECK(s.strikeCount().now() == 5000.0);
    CHECK(s.giftSlot().now() == 5000.0);
    CHECK(s.tick(0.1).step == doctest::Approx(0.1));
    CHECK(s.now() == doctest::Approx(5000.1));
    for (const double bad : {-1.0, 0.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity()}) {
        CHECK(s.tick(bad).step == 0.0);
        CHECK(s.now() == doctest::Approx(5000.1));
    }
    CHECK(s.tick(700.0).step == kMaxFightStep); // never a jump a piece would refuse
    CHECK(s.now() == doctest::Approx(5000.1 + kMaxFightStep));
    CHECK(s.strikeCount().now() == s.now());
    CHECK(s.giftSlot().now() == s.now());
    const FightSessionCheck nan = s.begin(FightSessionDef{}, std::numeric_limits<double>::quiet_NaN());
    CHECK(!nan.start);
    CHECK(nan.ok()); // a moved start is reported, not refused: the fight began
    CHECK(s.now() == 0.0);
    // An infinite start clamps to the bound on its side, like any start past it.
    const FightSessionCheck inf = s.begin(FightSessionDef{}, std::numeric_limits<double>::infinity());
    CHECK(!inf.start);
    CHECK(inf.ok());
    CHECK(s.now() == kMaxFightStart);
    CHECK(!s.begin(FightSessionDef{}, -std::numeric_limits<double>::infinity()).start);
    CHECK(s.now() == -kMaxFightStart);
    // A start past kMaxFightStart is clamped and reported; there a frame's step still moves the clock.
    CHECK(s.begin(FightSessionDef{}, kMaxFightStart).ok());
    const FightSessionCheck far = s.begin(FightSessionDef{}, 1.0e17);
    CHECK(!far.start);
    CHECK(far.count);
    CHECK(s.now() == kMaxFightStart);
    s.tick(1.0 / 60.0);
    CHECK(s.now() > kMaxFightStart);
    CHECK(s.now() == doctest::Approx(kMaxFightStart + 1.0 / 60.0).epsilon(1e-15));
    CHECK(!s.begin(FightSessionDef{}, -1.0e17).start);
    CHECK(s.now() == -kMaxFightStart);
}

TEST_CASE("fight session: begin reports a refused tuning and keeps the defaults") {
    FightSession s;
    FightSessionDef d;
    d.count.bpm = 1000.0;
    d.gift.healShare = 2.0f;
    d.places.baseCarry = 0;
    d.lock.keys = 0;
    d.table.minSeats = 0;
    d.hasBoss = true; // the default def has no moves
    const FightSessionCheck c = s.begin(d, 0.0);
    CHECK(!c.ok());
    CHECK(!c.count);
    CHECK(!c.gift);
    CHECK(!c.table);
    CHECK(!c.boss);
    CHECK(!c.places);
    CHECK(!c.lock);
    CHECK(s.places(kLeadB).tuning().baseCarry == PlaceTuning{}.baseCarry);
    CHECK(!s.hasBoss()); // a refused boss def leaves no boss script
    CHECK(s.strikeCount().tuning().bpm == CountTuning{}.bpm);
    CHECK(s.giftSlot().tuning().healShare == GiftTuning{}.healShare);
    CHECK(s.tick(kFrame).boss->empty());
    CHECK(s.counter(BossCounter::Guard, kLeadA) == BossInput::Ignored);
    CHECK(s.weakPointOpen()); // no lock asked for: open, as without a boss
    d.locked = true;
    d.lock = LockTuning{};
    CHECK(!s.begin(d, 0.0).boss);
    CHECK(!s.weakPointOpen()); // the session's lock rules instead
    CHECK(s.offerKey(kUnlockSharedBlow) == UnlockResult::Unlocked);
    CHECK(s.weakPointOpen());
}

TEST_CASE("fight session: hit-stop (zero steps) expires nothing; real time then does") {
    FightSession s;
    FightSessionDef d;
    d.gift = freeGifts();
    REQUIRE(s.begin(d, 100.0).ok());
    REQUIRE(s.selectGift(Gift::Guard));
    const GiftResult g = s.giveGift(kLeadA);
    REQUIRE(g.given);
    REQUIRE(s.callCount(kLeadB) == CountError::None);
    for (int i = 0; i < 100000; ++i) s.tick(0.0);
    CHECK(s.now() == 100.0);
    CHECK(s.giftSlot().guarded(kLeadA, s.now()));
    CHECK(s.strikeCount().live());
    CHECK(!s.strikeCount().open(s.now())); // the window waits for fight time, not frames
    CHECK(tickUntil(s, [&] { return !s.giftSlot().guarded(kLeadA, s.now()); }));
    CHECK(s.now() >= g.guardUntil);
    CHECK(tickUntil(s, [&] { return !s.strikeCount().live(); }));
}

TEST_CASE("fight session: after 2000 s of ticks every piece still answers") {
    FightSession s;
    FightSessionDef d;
    d.gift = freeGifts();
    d.locked = true;
    d.lock.keys = kUnlockSharedBlow | kUnlockRefusal;
    REQUIRE(s.begin(d, 0.0).ok());
    for (int i = 0; i < 8000; ++i) s.tick(kMaxFightStep);
    CHECK(s.now() == doctest::Approx(2000.0));
    REQUIRE(s.selectGift(Gift::Heal));
    CHECK(s.giveGift(kLeadB).given);
    CHECK(s.callCount(kLeadA) == CountError::None);
    CHECK(s.strikeCount().now() == doctest::Approx(2000.0));
    CHECK(s.offerKey(kUnlockSharedBlow) == UnlockResult::Unlocked);
    CHECK(s.weakPointOpen());
    CHECK(tickUntil(s, [&] { return !s.weakPointOpen(); }));
    CHECK(s.lock().opens() == 1);
}

TEST_CASE("fight session: a count joined by a second lead is a shared blow that opens the lock") {
    FightSession s;
    FightSessionDef d;
    d.locked = true;
    d.lock.keys = kUnlockSharedBlow;
    REQUIRE(s.begin(d, 50.0).ok());
    CHECK(!s.weakPointOpen());
    REQUIRE(s.callCount(kLeadA) == CountError::None);
    int cues = 0;
    bool opened = false;
    for (int i = 0; i < 600 && !opened; ++i) {
        const FightTick& t = s.tick(kFrame);
        for (const CountEvent& e : *t.count) {
            if (e.type == CountEventType::Cue && !e.isFalse) ++cues;
            if (e.type == CountEventType::WindowOpened) opened = true;
        }
    }
    REQUIRE(opened);
    CHECK(cues >= 2);
    CHECK(s.judgeBlow(kLeadA).outcome == CountOutcome::Held);
    CHECK(!s.weakPointOpen());
    CHECK(s.judgeBlow(kLeadB).outcome == CountOutcome::OnCount); // joins: a shared blow at once
    CHECK(s.weakPointOpen());
    // The held blow settles on the next tick, OnCount, and refreshes the lock.
    const FightTick& t = s.tick(kFrame);
    bool settled = false;
    for (const CountEvent& e : *t.count)
        if (e.type == CountEventType::BlowSettled && e.outcome == CountOutcome::OnCount) settled = true;
    CHECK(settled);
    CHECK(t.sharedBlows == 1);
    CHECK(s.lock().opens() == 1);
}

TEST_CASE("fight session: without a lock the point is open and keys are refused") {
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    CHECK(s.weakPointOpen());
    CHECK(s.offerKey(kUnlockSharedBlow) == UnlockResult::BadKey);
}

TEST_CASE("fight session: a refusal held to the end of the prompt offers the refusal key") {
    FightSession s;
    FightSessionDef d;
    d.locked = true;
    d.lock.keys = kUnlockRefusal;
    REQUIRE(s.begin(d, 0.0).ok());
    s.offerRefusal();
    CHECK(s.press(SharedAction::RefuseDown, kLeadA).refusal == RefusalInput::Holding);
    s.tick(kFrame);
    CHECK(s.refusal().holding());
    CHECK(s.endRefusal() == RefusalInput::Resolved);
    CHECK(s.refusal().refused());
    CHECK(s.weakPointOpen());
    // An unheld prompt is a deferral: no key.
    REQUIRE(s.begin(d, 0.0).ok());
    s.offerRefusal();
    CHECK(s.endRefusal() == RefusalInput::Resolved);
    CHECK(s.refusal().deferred());
    CHECK(!s.weakPointOpen());
}

namespace {

// A held refusal to the end of a prompt (the refusal key), as the host plays it.
void holdRefusal(FightSession& s) {
    s.offerRefusal();
    REQUIRE(s.press(SharedAction::RefuseDown, kLeadA).refusal == RefusalInput::Holding);
    s.tick(kFrame);
    REQUIRE(s.endRefusal() == RefusalInput::Resolved);
    REQUIRE(s.refusal().refused());
}

// Two leads' blows on one count: a shared blow (the session offers it to the boss script).
void sharedCountBlow(FightSession& s) {
    REQUIRE(s.callCount(kLeadA) == CountError::None);
    REQUIRE(tickUntil(s, [&] { return s.strikeCount().open(s.now()); }));
    REQUIRE(s.judgeBlow(kLeadA).outcome == CountOutcome::Held);
    REQUIRE(s.judgeBlow(kLeadB).outcome == CountOutcome::OnCount);
}

// Begins a fight on oneMoveBoss with that key kind (its lock lists every key) and counters the jab: exposed.
void beginExposed(FightSession& s, BossKey key) {
    FightSessionDef d;
    d.hasBoss = true;
    d.boss = oneMoveBoss();
    d.boss.key = key;
    d.boss.lock.keys = kUnlockAll;
    d.boss.lock.clause = 7;
    d.locked = true; // the session's own lock is not used while a boss script runs
    d.lock.keys = kUnlockAll;
    d.lock.clause = 7;
    REQUIRE(s.begin(d, 0.0).ok());
    REQUIRE(s.hasBoss());
    REQUIRE(tickUntil(s, [&] { return s.boss().state() == BossState::Window; }));
    REQUIRE(s.counter(BossCounter::Guard, kLeadA) == BossInput::Countered);
    REQUIRE(s.boss().exposed());
}

} // namespace

TEST_CASE("fight session: a boss point of the shared kind opens on the shared blow, not a refusal or a read") {
    FightSession s;
    beginExposed(s, BossKey::Shared);
    holdRefusal(s);
    CHECK(!s.weakPointOpen());
    CHECK(s.offerKey(kUnlockRefusal) == UnlockResult::WrongKey);
    CHECK(s.offerKey(kUnlockReadClause, 7) == UnlockResult::WrongKey);
    CHECK(s.boss().lock().opens() == 0);
    sharedCountBlow(s);
    CHECK(s.weakPointOpen());
}

TEST_CASE("fight session: a boss point of the refusal kind opens on a held refusal, only while exposed") {
    FightSession s;
    FightSessionDef d;
    d.hasBoss = true;
    d.boss = oneMoveBoss();
    d.boss.key = BossKey::Refusal;
    d.boss.lock.keys = kUnlockRefusal;
    REQUIRE(s.begin(d, 0.0).ok());
    holdRefusal(s); // not exposed yet
    CHECK(!s.weakPointOpen());
    CHECK(s.offerKey(kUnlockRefusal) == UnlockResult::NotExposed);
    beginExposed(s, BossKey::Refusal);
    sharedCountBlow(s); // the wrong key
    CHECK(!s.weakPointOpen());
    CHECK(s.offerKey(kUnlockReadClause, 7) == UnlockResult::WrongKey);
    CHECK(s.boss().lock().opens() == 0);
    holdRefusal(s);
    CHECK(s.weakPointOpen());
    CHECK(s.boss().lock().opens() == 1);
    CHECK(s.bossHit(10, true).crit);
    // Ending the ended prompt again offers nothing: the open time does not restart.
    const double until = s.boss().lock().until();
    s.tick(kFrame);
    CHECK(s.endRefusal() == RefusalInput::Ignored);
    CHECK(s.boss().lock().until() == until);
    // After the exposure runs out its own key is NotExposed.
    REQUIRE(tickUntil(s, [&] { return !s.boss().exposed(); }));
    CHECK(s.offerKey(kUnlockRefusal) == UnlockResult::NotExposed);
    // A refusal kind needs the refusal key in its lock.
    d.boss.lock.keys = kUnlockSharedBlow;
    CHECK(!s.begin(d, 0.0).boss);
}

TEST_CASE("fight session: a boss point of the read kind opens on its clause, not a refusal or the shared blow") {
    FightSession s;
    beginExposed(s, BossKey::Read);
    holdRefusal(s);
    sharedCountBlow(s);
    CHECK(!s.weakPointOpen());
    CHECK(s.offerKey(kUnlockReadClause, 8) == UnlockResult::WrongClause);
    CHECK(s.boss().lock().opens() == 0);
    CHECK(s.offerKey(kUnlockReadClause, 7) == UnlockResult::Unlocked);
    CHECK(s.weakPointOpen());
    CHECK(s.boss().lock().opens() == 1);
}

namespace {
bool tickHas(FightSession& s, BossEventType type) {
    const FightTick& t = s.tick(kFrame);
    for (std::size_t i = 0; i < t.boss->count; ++i)
        if (t.boss->items[i].type == type) return true;
    return false;
}
} // namespace

TEST_CASE("fight session: a deferred refusal relocks the boss point, by defer or by an unheld end") {
    FightSession s;
    beginExposed(s, BossKey::Read);
    REQUIRE(s.offerKey(kUnlockReadClause, 7) == UnlockResult::Unlocked);
    REQUIRE(s.weakPointOpen());
    s.offerRefusal();
    CHECK(s.deferRefusal() == RefusalInput::Resolved);
    CHECK(!s.weakPointOpen());
    CHECK(s.boss().lock().deferrals() == 1);
    CHECK(tickHas(s, BossEventType::Relocked));
    CHECK(!s.bossHit(10, true).crit);

    // Unlocked again, then an offer that ends unheld: a deferral too.
    REQUIRE(s.offerKey(kUnlockReadClause, 7) == UnlockResult::Unlocked);
    s.offerRefusal();
    CHECK(s.endRefusal() == RefusalInput::Resolved);
    CHECK(s.refusal().deferred());
    CHECK(!s.weakPointOpen());
    CHECK(s.boss().lock().deferrals() == 2);
    // A second end, or a defer with no prompt, defers nothing more.
    CHECK(s.endRefusal() == RefusalInput::Ignored);
    CHECK(s.deferRefusal() == RefusalInput::Ignored);
    CHECK(s.boss().lock().deferrals() == 2);
    // Accepting is a yes but not a deferral: the point stays unlocked.
    REQUIRE(s.offerKey(kUnlockReadClause, 7) == UnlockResult::Unlocked);
    s.offerRefusal();
    CHECK(s.acceptRefusal() == RefusalInput::Resolved);
    CHECK(s.weakPointOpen());
}

TEST_CASE("fight session: a deferred refusal relocks the session's own lock unless relockOnDefer is off") {
    for (const bool relock : {true, false}) {
        CAPTURE(relock);
        FightSession s;
        FightSessionDef d;
        d.locked = true;
        d.lock.keys = kUnlockSharedBlow;
        d.lock.relockOnDefer = relock;
        REQUIRE(s.begin(d, 0.0).ok());
        REQUIRE(s.offerKey(kUnlockSharedBlow) == UnlockResult::Unlocked);
        s.offerRefusal();
        CHECK(s.deferRefusal() == RefusalInput::Resolved);
        CHECK(s.weakPointOpen() == !relock);
        CHECK(s.lock().deferrals() == 1);
    }
}

TEST_CASE("fight session: a tick always points at event buffers, the same ones every tick") {
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    const CountEvents* count = s.tick(kFrame).count;
    const TableEvents* table = s.tick(kFrame).table;
    const FightTick& t = s.tick(kFrame);
    REQUIRE(t.count != nullptr);
    REQUIRE(t.table != nullptr);
    REQUIRE(t.boss != nullptr);
    CHECK(t.count == count);
    CHECK(t.table == table);
    CHECK(t.boss->count == 0);
}

TEST_CASE("fight session: events made between ticks arrive with the next tick") {
    FightSession s;
    FightSessionDef d;
    d.table.setTime = 0.5;
    REQUIRE(s.begin(d, 0.0).ok());
    CHECK(s.layPlace(kLeadA, 11) == PlaceError::None);
    CHECK(s.layPlace(kLeadA, 12) == PlaceError::None);
    CHECK(s.press(SharedAction::LeaveOpen, kLeadA).place == PlaceError::None);
    Load load;
    load.value = kMaxLoad;
    REQUIRE(s.setTable(kLeadA, load, 0) == TableError::None);
    CHECK(load.value == 0);
    const FightTick& t = s.tick(kFrame);
    CHECK(countOf(*t.table, TableEventType::Setting) == 1);
    CHECK(tickUntil(s, [&] { return s.table().set(); }));
    // Emptying a place while the table is set re-checks its seats: below the minimum it gutters.
    s.emptyPlace(kLeadA, 11);
    CHECK(!s.table().set());
    CHECK(countOf(*s.tick(kFrame).table, TableEventType::Guttered) == 1);
    CHECK(s.setTable(kNoFighter, load, 0) == TableError::BadSetter);
    CHECK(s.press(SharedAction::SetTable, kLeadA).accepted == false);
}

TEST_CASE("fight session: a boss script countered then hit on the count unlocks and crits") {
    FightSession s;
    FightSessionDef d;
    d.hasBoss = true;
    d.boss = oneMoveBoss();
    REQUIRE(s.begin(d, 300.0).ok());
    CHECK(s.boss().now() == 300.0);
    REQUIRE(tickUntil(s, [&] { return s.boss().state() == BossState::Window; }));
    REQUIRE(s.counter(BossCounter::Guard, kLeadA) == BossInput::Countered);
    CHECK(s.boss().exposed());
    CHECK(!s.weakPointOpen());
    REQUIRE(s.callCount(kLeadA) == CountError::None);
    REQUIRE(tickUntil(s, [&] { return s.strikeCount().open(s.now()); }));
    CHECK(s.judgeBlow(kLeadA).outcome == CountOutcome::Held);
    CHECK(s.judgeBlow(kLeadB).outcome == CountOutcome::OnCount);
    CHECK(s.weakPointOpen());
    const ScriptHit h = s.bossHit(10, true);
    CHECK(h.crit);
    CHECK(h.damage == 20);
    CHECK(s.offerKey(kUnlockSharedBlow) == UnlockResult::Refreshed); // routed to the script's point
    // A shared counter kind needs the shared input from the host: an open window alone is not it.
    FightSessionDef shared = d;
    shared.boss.moves[0].counter = BossCounter::StrikeOnCount;
    REQUIRE(s.begin(shared, 0.0).ok());
    REQUIRE(tickUntil(s, [&] { return s.now() >= 0.8; }));
    REQUIRE(s.callCount(kLeadA) == CountError::None); // its window opens inside the boss's (2.0 to 2.5)
    REQUIRE(tickUntil(s, [&] { return s.boss().state() == BossState::Window && s.strikeCount().open(s.now()); }));
    CHECK(s.counter(BossCounter::StrikeOnCount, kLeadA) == BossInput::NeedsShared);
    CHECK(s.counter(BossCounter::StrikeOnCount, kLeadA, sharedBlow(CountHit{CountOutcome::OnCount})) == BossInput::Countered);
    REQUIRE(s.begin(d, 300.0).ok());
    REQUIRE(tickUntil(s, [&] { return s.boss().state() == BossState::Window; }));
    REQUIRE(s.counter(BossCounter::Guard, kLeadA) == BossInput::Countered);
    // Hit-stop holds the boss too.
    const double stepEnds = s.boss().stepEnds();
    const BossState st = s.boss().state();
    for (int i = 0; i < 1000; ++i) s.tick(0.0);
    CHECK(s.boss().state() == st);
    CHECK(s.boss().stepEnds() == stepEnds);
}

TEST_CASE("fight session: only a fight the story handed off takes story writes; a standalone or practice one writes none") {
    // The rule the host binds by.
    CHECK_FALSE(fightTakesStory(FightHandOff{}, CombatKind::Arena)); // standalone, demo, practice
    CHECK(fightTakesStory(FightHandOff{true, false, CombatKind::Arena}, CombatKind::Arena));
    CHECK_FALSE(fightTakesStory(FightHandOff{true, true, CombatKind::Arena}, CombatKind::Arena));  // settled: replays are practice
    CHECK_FALSE(fightTakesStory(FightHandOff{true, false, CombatKind::Beat}, CombatKind::Arena)); // another kind's hand-off

    FactRegistry reg;
    std::string err;
    REQUIRE_MESSAGE(importStoryRegistry(kPressCsv, kPressJson, reg, &err), err);
    StoryState story;
    storyReset(story);
    REQUIRE(storyBeginScene(story, reg, storySceneOrdinal(reg, "s1")));
    const auto wideA0 = [&] {
        for (std::uint8_t w = 0; w < kWideSlots; ++w)
            if (reg.wide[w].id() == std::string_view("w_a0")) return storyWide(story, w);
        return 0xFFFFFFFFu;
    };
    FightSessionDef d;
    d.gift = freeGifts();
    // The same fight, begun and restarted, as the host does: bound only when the hand-off says so.
    StoryStepMark mark;
    CHECK_FALSE(storyStepOpen(story, mark)); // nothing marked yet
    REQUIRE(storyMarkStep(story, mark));
    CHECK(storyStepOpen(story, mark));
    const auto run = [&](const FightHandOff& h, bool rollBack) {
        FightSession s;
        for (int begin = 0; begin < 2; ++begin) { // a begin and a restart
            REQUIRE(s.begin(d, 0.0).ok());
            CHECK_FALSE(s.storyBound()); // a binding never outlives its fight
            if (begin > 0 && rollBack) {
                REQUIRE(storyEndStep(story, reg, mark, StepEnd::Retry));
                REQUIRE(storyMarkStep(story, mark));
            }
            if (fightTakesStory(h, CombatKind::Arena)) CHECK(s.bindStory(story, reg, mark) == (begin == 0 || rollBack));
            REQUIRE(s.selectGift(Gift::Heal));
            CHECK(s.giveGift(kLeadA).given);
        }
    };
    run(FightHandOff{}, false);
    CHECK(wideA0() == 0); // a standalone fight's presses never reach the playthrough
    run(FightHandOff{true, true, CombatKind::Arena}, false);
    CHECK(wideA0() == 0);
    run(FightHandOff{true, false, CombatKind::Arena}, true);
    CHECK(wideA0() == 1); // a retry rolled back: the attempt that stands counts once
    REQUIRE(storyEndStep(story, reg, mark, StepEnd::Retry));
    REQUIRE(storyMarkStep(story, mark));
    // A screen that retries without the rollback (forgotten) is refused: the second attempt writes nothing.
    run(FightHandOff{true, false, CombatKind::Arena}, false);
    CHECK(wideA0() == 1);
}

TEST_CASE("fight session: a mark lost between attempts is refused, never re-based") {
    FactRegistry reg;
    std::string err;
    REQUIRE_MESSAGE(importStoryRegistry(kPressCsv, kPressJson, reg, &err), err);
    StoryState story;
    storyReset(story);
    REQUIRE(storyBeginScene(story, reg, storySceneOrdinal(reg, "s1")));
    std::uint8_t a0 = 0xFF;
    for (std::uint8_t w = 0; w < kWideSlots; ++w)
        if (reg.wide[w].id() == std::string_view("w_a0")) a0 = w;
    REQUIRE(a0 != 0xFF);
    FightSessionDef d;
    d.gift = freeGifts();
    StoryStepMark mark; // no step open: a reload into the fight

    FightSession s;
    REQUIRE(s.begin(d, 0.0).ok());
    CHECK(storyReadyBind(story, reg, mark, true) == StepBind::MarkedAtStart);
    REQUIRE(s.bindStory(story, reg, mark));
    REQUIRE(s.selectGift(Gift::Heal));
    CHECK(s.giveGift(kLeadA).given);
    CHECK(storyWide(story, a0) == 1);

    mark = StoryStepMark{}; // the mark is lost mid-fight; the screen restarts
    REQUIRE(s.begin(d, 0.0).ok());
    CHECK(storyReadyBind(story, reg, mark, false) == StepBind::NoStep); // never marked at the dirty state
    CHECK_FALSE(s.bindStory(story, reg, mark));
    REQUIRE(s.selectGift(Gift::Heal));
    CHECK(s.giveGift(kLeadA).given);
    CHECK(storyWide(story, a0) == 1); // the second attempt writes nothing on top

    REQUIRE(storyMarkStep(story, mark)); // a mark that has moved is refused too
    CHECK(storyReadyBind(story, reg, mark, false) == StepBind::Ready);
    REQUIRE(storyAdd(story, reg, a0, 1) == SetResult::Ok);
    CHECK(storyReadyBind(story, reg, mark, false) == StepBind::Moved);
}

TEST_CASE("fight session: gift presses at a lead are presses in the story state") {
    FactRegistry reg;
    std::string err;
    REQUIRE_MESSAGE(importStoryRegistry(kPressCsv, kPressJson, reg, &err), err);
    StoryState story;
    storyReset(story);
    REQUIRE(storyBeginScene(story, reg, storySceneOrdinal(reg, "s1")));
    const auto wide = [&](const char* id) {
        for (std::uint8_t w = 0; w < kWideSlots; ++w)
            if (reg.wide[w].id() == id) return storyWide(story, w);
        return 0xFFFFFFFFu;
    };

    FightSession s;
    FightSessionDef d;
    d.gift = freeGifts();
    d.gift.guardSeconds = 0.0; // Guards back to back: the Guard's cooldown starts when its guard ends
    REQUIRE(s.begin(d, 0.0).ok());
    REQUIRE(s.selectGift(Gift::Heal));
    CHECK(s.giveGift(kLeadA).given); // unbound: nothing written
    CHECK(s.lastStoryWrite() == SetResult::Unchanged);
    CHECK(wide("w_a0") == 0);

    StoryStepMark mark;
    CHECK_FALSE(s.bindStory(story, reg, mark)); // no mark: refused
    CHECK(s.giveGift(kLeadA).given);
    CHECK(wide("w_a0") == 0);

    REQUIRE(storyMarkStep(story, mark));
    REQUIRE(s.bindStory(story, reg, mark));
    CHECK(s.press(SharedAction::Gift, kThirdSlot, kLeadA).accepted);
    CHECK(s.lastStoryWrite() == SetResult::Ok);
    CHECK(wide("w_a0") == 1);
    // A Heal at a lead at full health did nothing: not a press in the story either.
    s.setLeadHealth(kLeadA, 1.0f);
    CHECK(s.leadHealth(kLeadA) == 1.0f);
    CHECK(s.giveGift(kLeadA).error == GiftError::NotHurt);
    CHECK(s.lastStoryWrite() == SetResult::Unchanged);
    CHECK(wide("w_a0") == 1);
    s.setLeadHealth(kLeadA, 0.5f);
    REQUIRE(s.selectGift(Gift::Guard));
    CHECK(s.giveGift(kLeadA).given);
    CHECK(wide("w_a1") == 1);
    // Side 1 is closed until its open fact is set.
    CHECK(s.giveGift(kLeadB).given);
    CHECK(s.lastStoryWrite() == SetResult::RejectedGate);
    CHECK(wide("w_b1") == 0);
    std::uint16_t open = 0;
    for (std::uint16_t f = 1; f < kFactSlots; ++f)
        if (reg.facts[f].id() == std::string_view("f_open")) open = f;
    REQUIRE(open != 0);
    REQUIRE(storySet(story, reg, open, 1) == SetResult::Ok);
    CHECK(s.giveGift(kLeadB).given);
    CHECK(s.lastStoryWrite() == SetResult::Ok);
    CHECK(wide("w_b1") == 1);
    // A press at a non-lead has no side and writes nothing; a capped counter answers Unchanged.
    CHECK(!s.giveGift(kThirdSlot).given);
    CHECK(s.lastStoryWrite() == SetResult::Unchanged);
    REQUIRE(s.selectGift(Gift::Heal));
    for (int i = 0; i < 4; ++i) CHECK(s.giveGift(kLeadA).given);
    CHECK(wide("w_a0") == 3);
    CHECK(s.lastStoryWrite() == SetResult::Unchanged);
    // A restart of the scene rolls the presses back; the retried fight counts afresh.
    REQUIRE(storyRestartScene(story, reg));
    CHECK(wide("w_a0") == 0);
    CHECK(wide("w_a1") == 0);
    REQUIRE(s.begin(d, 0.0).ok());
    CHECK(s.lastStoryWrite() == SetResult::Unchanged);
    REQUIRE(storyMarkStep(story, mark)); // the new fight's step
    REQUIRE(s.bindStory(story, reg, mark));
    REQUIRE(s.selectGift(Gift::Heal));
    CHECK(s.giveGift(kLeadA).given);
    CHECK(s.lastStoryWrite() == SetResult::Ok);
    CHECK(wide("w_a0") == 1);
    s.unbindStory();
    CHECK(s.giveGift(kLeadB).given);
    CHECK(wide("w_b0") == 0);
}

TEST_CASE("fight session: a retry puts the gift tally and the story's press counters back together") {
    // The host's two calls agree: a retried step (storyEndStep Retry) with no finish(true) rolls back the story's
    // per-kind counters and, at the next begin, the slot's tally; a kept step (finish(true), then storyEndStep
    // Keep) keeps both.
    FactRegistry reg;
    std::string err;
    REQUIRE_MESSAGE(importStoryRegistry(kPressCsv, kPressJson, reg, &err), err);
    StoryState story;
    storyReset(story);
    REQUIRE(storyBeginScene(story, reg, storySceneOrdinal(reg, "s1")));
    const auto wide = [&](const char* id) {
        for (std::uint8_t w = 0; w < kWideSlots; ++w)
            if (reg.wide[w].id() == id) return storyWide(story, w);
        return 0xFFFFFFFFu;
    };
    FightSession s;
    FightSessionDef d;
    d.gift = freeGifts();
    d.gift.guardSeconds = 0.0;
    StoryStepMark mark;
    const auto attempt = [&](int heals) {
        REQUIRE(s.begin(d, 0.0).ok());
        REQUIRE(storyMarkStep(story, mark));
        REQUIRE(s.bindStory(story, reg, mark));
        REQUIRE(s.selectGift(Gift::Heal));
        for (int i = 0; i < heals; ++i) CHECK(s.giveGift(kLeadA).given);
    };
    attempt(2);
    CHECK(wide("w_a0") == 2);
    CHECK(s.giftSlot().tally().givenOf(Gift::Heal) == 2);
    // Lost and retried: the story rolls the step back, the next begin the tally.
    REQUIRE(storyEndStep(story, reg, mark, StepEnd::Retry));
    CHECK(wide("w_a0") == 0);
    attempt(1);
    CHECK(wide("w_a0") == 1);
    CHECK(s.giftSlot().tally().givenOf(Gift::Heal) == 1);
    // Won: both keep this attempt's presses into the next fight.
    s.finish(true);
    REQUIRE(storyEndStep(story, reg, mark, StepEnd::Keep));
    attempt(0);
    CHECK(wide("w_a0") == 1);
    CHECK(s.giftSlot().tally().givenOf(Gift::Heal) == 1);
}

TEST_CASE("fight session: refused gift presses at a lead count in the story state too") {
    FactRegistry reg;
    std::string err;
    REQUIRE_MESSAGE(importStoryRegistry(kPressCsv, kPressJson, reg, &err), err);
    StoryState story;
    storyReset(story);
    REQUIRE(storyBeginScene(story, reg, storySceneOrdinal(reg, "s1")));
    const auto wide = [&](const char* id) {
        for (std::uint8_t w = 0; w < kWideSlots; ++w)
            if (reg.wide[w].id() == id) return storyWide(story, w);
        return 0xFFFFFFFFu;
    };

    FightSession s;
    FightSessionDef d;
    d.gift.cooldown = {5.0, 5.0, 5.0};
    REQUIRE(s.begin(d, 0.0).ok());
    StoryStepMark mark;
    REQUIRE(storyMarkStep(story, mark));
    REQUIRE(s.bindStory(story, reg, mark));
    const auto& slot = s.giftSlot();

    // Cooling: the second press is refused but is still a press of its kind on its side.
    REQUIRE(s.selectGift(Gift::Guard));
    CHECK(s.giveGift(kLeadA).given);
    CHECK(wide("w_a1") == 1);
    const GiftResult cool = s.giveGift(kLeadA);
    CHECK(!cool.given);
    CHECK(cool.error == GiftError::Cooling);
    CHECK(s.lastStoryWrite() == SetResult::Ok);
    CHECK(wide("w_a1") == 2);
    CHECK(slot.tally().givenOf(Gift::Guard) == 1);
    CHECK(slot.threads(kLeadA) == 1);

    // A refused count is a Count press.
    REQUIRE(s.callCount(kLeadA) == CountError::None);
    REQUIRE(s.selectGift(Gift::Count));
    const GiftResult cr = s.giveGift(kLeadA);
    CHECK(cr.error == GiftError::CountRefused);
    CHECK(wide("w_a2") == 1);

    // Hostile and vacant slots refuse before any tally, and the press still counts.
    s.giftSlot().setState(SlotState::Hostile);
    REQUIRE(s.selectGift(Gift::Heal));
    CHECK(s.giveGift(kLeadA).error == GiftError::Hostile);
    CHECK(wide("w_a0") == 1);
    s.giftSlot().setState(SlotState::Vacant);
    CHECK(s.giveGift(kLeadA).error == GiftError::Vacant);
    CHECK(wide("w_a0") == 2);

    // A refused press on a closed side is gated like a given one.
    CHECK(!s.giveGift(kLeadB).given);
    CHECK(s.lastStoryWrite() == SetResult::RejectedGate);
    CHECK(wide("w_b0") == 0);
    // At a non-lead there is no side.
    CHECK(s.giveGift(kThirdSlot).error == GiftError::Vacant);
    CHECK(s.lastStoryWrite() == SetResult::Unchanged);

    CHECK(slot.threads(kLeadA) == 1);
    CHECK(slot.tally().givenOf(Gift::Heal) == 0);
    CHECK(slot.tally().givenOf(Gift::Count) == 0);
}

TEST_CASE("fight session: the active lead is a lead, kept across begin") {
    FightSession s;
    CHECK(s.activeLead() == kLeadA);
    CHECK(s.setActiveLead(kLeadB));
    CHECK(s.activeLead() == kLeadA); // asked: the step's close moves it
    CHECK(s.switchRequested() == kLeadB);
    CHECK(s.tick(kFrame).switched == kLeadB);
    CHECK(s.activeLead() == kLeadB);
    CHECK(s.switchRequested() == kNoFighter);
    CHECK(!s.setActiveLead(kThirdSlot));
    CHECK(!s.setActiveLead(kNoFighter));
    CHECK(s.switchRequested() == kNoFighter);
    CHECK(s.activeLead() == kLeadB);
    REQUIRE(s.begin(FightSessionDef{}, 10.0).ok());
    CHECK(s.activeLead() == kLeadB);
    // A switch asked before a begin: the fight opens on it.
    CHECK(s.setActiveLead(kLeadA));
    REQUIRE(s.begin(FightSessionDef{}, 20.0).ok());
    CHECK(s.activeLead() == kLeadA);
    CHECK(s.switchRequested() == kNoFighter);
    CHECK(s.tick(kFrame).switched == kNoFighter);
    // The last ask in a step wins; one for the lead in control reports nothing.
    CHECK(s.setActiveLead(kLeadB));
    CHECK(s.setActiveLead(kLeadA));
    const FightTick& t = s.tick(kFrame);
    CHECK(t.switched == kNoFighter);
    CHECK(t.switchRefused == kNoFighter);
    CHECK(s.activeLead() == kLeadA);
}

TEST_CASE("fight session: a fight's frames allocate nothing") {
    FightSession s;
    FightSessionDef d;
    d.hasBoss = true;
    d.boss = oneMoveBoss();
    d.gift = freeGifts();
    REQUIRE(s.begin(d, 0.0).ok());
    REQUIRE(s.selectGift(Gift::Heal));
    REQUIRE(ftcore_test::allocHookInstalled());
    const auto before = ftcore_test::allocStats().count;
    for (int i = 0; i < 3000; ++i) {
        const FightTick& t = s.tick(i % 7 == 0 ? 0.0 : kFrame);
        if (i % 90 == 0) s.callCount(kLeadA);
        if (s.strikeCount().open(t.now)) {
            s.judgeBlow(kLeadA);
            s.judgeBlow(kLeadB);
        }
        if (s.boss().state() == BossState::Window) s.counter(BossCounter::Guard, kLeadB);
        if (i % 120 == 0) s.giveGift(kLeadA);
        s.bossHit(1, true);
    }
    CHECK(ftcore_test::allocStats().count == before);
}

namespace {
// Begins a plain fight and calls a count with the first lead, ticking until its window is open.
void openCountWindow(FightSession& s) {
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    REQUIRE(s.callCount(kLeadA) == CountError::None);
    REQUIRE(tickUntil(s, [&] { return s.strikeCount().open(s.now()); }));
}
} // namespace

TEST_CASE("fight session: a held blow joined on the count pays its bonus on a later tick") {
    FightSession s;
    openCountWindow(s);
    const CountHit h = s.judgeHeldBlow(kLeadA, 2, 10, 3);
    REQUIRE(h.outcome == CountOutcome::Held);
    CHECK(s.tick(0.0).bonuses.empty()); // nothing settled yet
    const CountHit j = s.judgeHeldBlow(kLeadB, 4, 10, 5);
    REQUIRE(j.outcome == CountOutcome::OnCount); // final at once: no record, the host multiplies
    const FightTick& t = s.tick(kFrame);
    REQUIRE(t.bonuses.size() == 1);
    CHECK(t.bonuses[0].enemy == 2);
    CHECK(t.bonuses[0].move == 3);
    const float mult = CountTuning{}.onCountMultiplier;
    CHECK(t.bonuses[0].extraDamage == static_cast<int>(std::round(10.0 * (static_cast<double>(mult) - 1.0))));
    CHECK(t.heldDropped == 0);
    CHECK(t.heldExpired == 0);
    CHECK(s.tick(kFrame).bonuses.empty()); // paid once
}

TEST_CASE("fight session: a held blow left alone settles at or below x1 and pays nothing") {
    FightSession s;
    openCountWindow(s);
    REQUIRE(s.judgeHeldBlow(kLeadA, 1, 10, 0).outcome == CountOutcome::Held);
    bool settled = false;
    int bonuses = 0;
    int expired = 0;
    for (int i = 0; i < 600 && !settled; ++i) {
        const FightTick& t = s.tick(kFrame);
        bonuses += static_cast<int>(t.bonuses.size());
        expired += t.heldExpired;
        for (const CountEvent& e : *t.count)
            if (e.type == CountEventType::BlowSettled) settled = true;
    }
    REQUIRE(settled);
    CHECK(bonuses == 0);
    CHECK(expired == 0); // freed by its settle, not expired
}

TEST_CASE("fight session: held blows past the buffer are dropped, and begin forgets the rest") {
    FightSession s;
    openCountWindow(s);
    for (std::size_t i = 0; i < kMaxHeldBlows + 2; ++i)
        REQUIRE(s.judgeHeldBlow(kLeadA, 0, 10, 0).outcome == CountOutcome::Held);
    REQUIRE(s.judgeHeldBlow(kLeadB, 0, 10, 0).outcome == CountOutcome::OnCount);
    const FightTick& t = s.tick(kFrame);
    CHECK(t.heldDropped == 2);
    CHECK(t.bonuses.size() == kMaxHeldBlows);
    CHECK(s.tick(kFrame).heldDropped == 0); // reported once

    // A held blow, then a new fight: its settle never pays.
    openCountWindow(s);
    REQUIRE(s.judgeHeldBlow(kLeadA, 0, 10, 0).outcome == CountOutcome::Held);
    openCountWindow(s);
    REQUIRE(s.judgeHeldBlow(kLeadB, 0, 10, 0).outcome == CountOutcome::Held);
    REQUIRE(s.judgeHeldBlow(kLeadA, 0, 10, 0).outcome == CountOutcome::OnCount);
    CHECK(s.tick(kFrame).bonuses.size() == 1);
    // A held blow waited on, then a new fight whose count settles with no held blow of its own: nothing of the
    // last fight is reported in the new one, neither paid nor expired.
    openCountWindow(s);
    REQUIRE(s.judgeHeldBlow(kLeadA, 0, 10, 0).outcome == CountOutcome::Held);
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    REQUIRE(s.callCount(kLeadA) == CountError::None);
    int expired = 0;
    std::size_t paid = 0;
    auto step = [&] {
        const FightTick& st = s.tick(kFrame);
        expired += st.heldExpired;
        paid += st.bonuses.size();
    };
    for (int i = 0; i < 2000 && !s.strikeCount().open(s.now()); ++i) step();
    REQUIRE(s.strikeCount().open(s.now()));
    REQUIRE(s.judgeBlow(kLeadB).outcome == CountOutcome::Held);
    REQUIRE(s.judgeBlow(kLeadA).outcome == CountOutcome::OnCount);
    for (int i = 0; i < 4; ++i) step();
    CHECK(paid == 0);
    CHECK(expired == 0);
    // A zero or negative base waits on nothing.
    openCountWindow(s);
    REQUIRE(s.judgeHeldBlow(kLeadA, 0, 0, 0).outcome == CountOutcome::Held);
    REQUIRE(s.judgeHeldBlow(kLeadB, 0, 10, 0).outcome == CountOutcome::OnCount);
    CHECK(s.tick(kFrame).bonuses.empty());
}

TEST_CASE("fight session: a held blow whose settle is dropped expires at base") {
    FightSession s;
    openCountWindow(s);
    // Enough held blows that the settles overflow the count's event buffer; the session waits on the last ones.
    const std::size_t plain = kMaxCountEvents + 8;
    for (std::size_t i = 0; i < plain; ++i) REQUIRE(s.judgeBlow(kLeadA).outcome == CountOutcome::Held);
    for (std::size_t i = 0; i < kMaxHeldBlows; ++i) REQUIRE(s.judgeHeldBlow(kLeadA, 0, 10, 0).outcome == CountOutcome::Held);
    REQUIRE(s.judgeBlow(kLeadB).outcome == CountOutcome::OnCount);
    const FightTick& t = s.tick(kFrame);
    CHECK(t.count->droppedSettles > 0);
    CHECK(t.heldExpired == static_cast<int>(kMaxHeldBlows));
    CHECK(t.bonuses.empty());
    CHECK(s.tick(kFrame).heldExpired == 0);
}

TEST_CASE("fight session: the gift HUD reads, and a new fight resets the Heals but not the save-wide total") {
    FightSession s;
    FightSessionDef d;
    REQUIRE(s.begin(d, 0.0).ok());
    CHECK(s.giftHealsLeft() == 2);
    CHECK(s.giftHealReadyIn() == 0.0);
    CHECK(s.giftGuardReadyIn() == 0.0);
    REQUIRE(s.selectGift(Gift::Heal));
    CHECK(s.press(SharedAction::Gift, kThirdSlot, kLeadA).accepted);
    CHECK(s.giftHealsLeft() == 1);
    CHECK(s.giftHealReadyIn() == doctest::Approx(8.0));
    REQUIRE(s.selectGift(Gift::Guard));
    CHECK(s.giveGift(kLeadA).given);
    CHECK(s.giftGuardReadyIn() == doctest::Approx(13.2)); // 1.2 s of guard, then 12 s
    CHECK(tickUntil(s, [&] { return s.giftHealReadyIn() == 0.0; }));
    CHECK(s.giftGuardReadyIn() == doctest::Approx(5.2).epsilon(0.02));
    REQUIRE(s.selectGift(Gift::Heal));
    CHECK(s.giveGift(kLeadB).given);
    CHECK(s.giftHealsLeft() == 0);
    CHECK(tickUntil(s, [&] { return s.giftHealReadyIn() == 0.0; }));
    const GiftResult third = s.giveGift(kLeadA);
    CHECK_FALSE(third.given);
    CHECK(third.error == GiftError::NoneLeft);
    CHECK(s.giftSlot().allPresses() == 4);

    // The fight is won; the next fight: two Heals again, cooldowns clear, the save-wide total kept.
    s.finish(true);
    REQUIRE(s.begin(d, s.now() + 1.0).ok());
    CHECK(s.giftHealsLeft() == 2);
    CHECK(s.giftHealReadyIn() == 0.0);
    CHECK(s.giftGuardReadyIn() == 0.0);
    CHECK(s.giftSlot().allPresses() == 4);
    CHECK(s.giftSlot().tally().givenOf(Gift::Heal) == 2);
    CHECK(s.giftSlot().tally().refusedOf(Gift::Heal) == 1);
    CHECK(s.giftSlot().threads(kLeadA) == 2);
    CHECK(s.giveGift(kLeadA).given);
    CHECK(s.giftSlot().allPresses() == 5);
}

TEST_CASE("fight session: a retry (begin after a fight not finished as won) puts the gift tally and threads back") {
    FightSession s;
    FightSessionDef d;
    REQUIRE(s.begin(d, 0.0).ok());
    REQUIRE(s.selectGift(Gift::Heal));
    REQUIRE(s.giveGift(kLeadA).given); // a fight won with one press
    s.finish(true);
    REQUIRE(s.begin(d, 1.0).ok());
    REQUIRE(s.giftSlot().allPresses() == 1);
    // The attempt: two gifts pressed, then lost (no finish, or finish(false)): the retry is back at the start.
    for (const int how : {0, 1}) {
        CAPTURE(how);
        CHECK(s.giveGift(kLeadA).given);
        REQUIRE(s.selectGift(Gift::Guard));
        CHECK(s.giveGift(kLeadB).given);
        CHECK(s.giftSlot().allPresses() == 3);
        CHECK(s.giftHealsLeft() == 1);
        if (how == 1) s.finish(false);
        CHECK_FALSE(s.finishedWon());
        REQUIRE(s.begin(d, s.now() + 1.0).ok());
        CHECK(s.giftSlot().allPresses() == 1);
        CHECK(s.giftSlot().tally().givenOf(Gift::Heal) == 1);
        CHECK(s.giftSlot().tally().givenOf(Gift::Guard) == 0);
        CHECK(s.giftSlot().threads(kLeadA) == 1);
        CHECK(s.giftSlot().threads(kLeadB) == 0);
        CHECK(s.giftHealsLeft() == 2); // the per-fight heal count resets
        CHECK(s.giftGuardReadyIn() == 0.0);
    }
    // Two gifts pressed and the fight won: the next fight keeps them, with two Heals again.
    CHECK(s.giveGift(kLeadA).given);
    REQUIRE(s.selectGift(Gift::Guard));
    CHECK(s.giveGift(kLeadB).given);
    s.finish(true);
    CHECK(s.finishedWon());
    REQUIRE(s.begin(d, s.now() + 1.0).ok());
    CHECK_FALSE(s.finishedWon()); // a begin clears the win
    CHECK(s.giftSlot().allPresses() == 3);
    CHECK(s.giftSlot().threads(kLeadA) == 2);
    CHECK(s.giftSlot().threads(kLeadB) == 1);
    CHECK(s.giftHealsLeft() == 2);
    // A checkpoint loaded mid-fight is the start a retry goes back to.
    GiftSave cp = s.giftSlot().save();
    cp.threads[kLeadB] = 7;
    REQUIRE(s.giftSlot().load(cp));
    CHECK(s.giveGift(kLeadA).given);
    REQUIRE(s.begin(d, s.now() + 1.0).ok());
    CHECK(s.giftSlot().threads(kLeadB) == 7);
    CHECK(s.giftSlot().threads(kLeadA) == 2);
}

TEST_CASE("fight session: a lead's health that is not a number in 0..1 is stored as unknown and reads as hurt") {
    struct Case {
        float in;
        float stored;
        bool full;
    };
    const Case cases[] = {
        {0.999f, 0.999f, false},
        {1.0f, 1.0f, true},
        {std::numeric_limits<float>::quiet_NaN(), kHealthUnknown, false},
        {-0.5f, kHealthUnknown, false},
        {2.0f, kHealthUnknown, false},
        {std::numeric_limits<float>::infinity(), kHealthUnknown, false},
        {-std::numeric_limits<float>::infinity(), kHealthUnknown, false},
        {0.0f, 0.0f, false},
    };
    for (const Case& c : cases) {
        CAPTURE(c.in);
        FightSession s;
        REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
        REQUIRE(s.selectGift(Gift::Heal));
        s.setLeadHealth(kLeadA, c.in);
        CHECK(s.leadHealth(kLeadA) == c.stored);
        CHECK_FALSE(std::isnan(s.leadHealth(kLeadA)));
        const GiftResult r = s.giveGift(kLeadA);
        if (c.full) {
            CHECK(r.error == GiftError::NotHurt);
            CHECK_FALSE(r.given);
        } else if (std::isfinite(c.in) && c.in <= 0.f) { // 0 and under (finite) is down: no gift stands it up
            CHECK(s.down(kLeadA));
            CHECK(r.error == GiftError::Down);
            CHECK_FALSE(r.given);
        } else {
            CHECK(r.given);
        }
    }
    // A bad value after a good one replaces it with unknown, not the last good value.
    FightSession s;
    REQUIRE(s.begin(FightSessionDef{}, 0.0).ok());
    s.setLeadHealth(kLeadB, 1.0f);
    s.setLeadHealth(kLeadB, std::numeric_limits<float>::quiet_NaN());
    CHECK(s.leadHealth(kLeadB) == kHealthUnknown);
    CHECK(s.leadHealth(kLeadA) == kHealthUnknown);
}

TEST_CASE("fight session: a phase retry keeps the story bound and leaves its counters exactly as they are") {
    FactRegistry reg;
    std::string err;
    REQUIRE_MESSAGE(importStoryRegistry(kPressCsv, kPressJson, reg, &err), err);
    StoryState story;
    storyReset(story);
    REQUIRE(storyBeginScene(story, reg, storySceneOrdinal(reg, "s1")));
    std::uint8_t a0 = 0xFF;
    for (std::uint8_t w = 0; w < kWideSlots; ++w)
        if (reg.wide[w].id() == std::string_view("w_a0")) a0 = w;
    REQUIRE(a0 != 0xFF);
    FightSessionDef d;
    d.gift = freeGifts();
    StoryStepMark mark;
    REQUIRE(storyMarkStep(story, mark));
    FightSession s;
    REQUIRE(s.begin(d, 0.0).ok());
    REQUIRE(s.bindStory(story, reg, mark));
    s.setHostCheckpoint(FightHostCheckpoint{});
    REQUIRE(s.selectGift(Gift::Heal));
    CHECK(s.giveGift(kLeadA).given);
    REQUIRE(s.checkpointPhase(1));
    s.setHostCheckpoint(FightHostCheckpoint{});
    CHECK(s.giveGift(kLeadA).given); // in the attempt about to be lost
    REQUIRE(storyWide(story, a0) == 2);
    s.setLeadHealth(kLeadA, 0.f);
    s.setLeadHealth(kLeadB, 0.f);
    s.tick(1.0 / 60.0); // the step closes: the defeat
    REQUIRE(s.defeated());
    const StoryState before = story;
    CHECK(s.retry().at == CheckpointAt::Phase);
    // Nothing rolled back or forward: every fact, counter, entry copy, event and the retry count as they were.
    CHECK(story.facts == before.facts);
    CHECK(story.factsEntry == before.factsEntry);
    CHECK(story.wide == before.wide);
    CHECK(story.wideEntry == before.wideEntry);
    CHECK(story.eventCount == before.eventCount);
    CHECK(story.eventHead == before.eventHead);
    CHECK(story.openScene == before.openScene);
    CHECK(storyWide(story, a0) == 2);
    // Still bound to the fight-start mark: the next press counts on.
    CHECK(s.storyBound());
    REQUIRE(s.selectGift(Gift::Heal));
    CHECK(s.giveGift(kLeadA).given);
    CHECK(s.lastStoryWrite() == SetResult::Ok);
    CHECK(storyWide(story, a0) == 3);
    // A retry to the fight start unbinds, as a begin does.
    FightSession t;
    REQUIRE(t.begin(d, 0.0).ok());
    REQUIRE(storyEndStep(story, reg, mark, StepEnd::Retry));
    REQUIRE(storyMarkStep(story, mark));
    REQUIRE(t.bindStory(story, reg, mark));
    t.setHostCheckpoint(FightHostCheckpoint{});
    CHECK(t.retry().at == CheckpointAt::FightStart);
    CHECK_FALSE(t.storyBound());
}

TEST_CASE("fight session: a gift refused Down at a lead is no press in the story state") {
    FactRegistry reg;
    std::string err;
    REQUIRE_MESSAGE(importStoryRegistry(kPressCsv, kPressJson, reg, &err), err);
    StoryState story;
    storyReset(story);
    REQUIRE(storyBeginScene(story, reg, storySceneOrdinal(reg, "s1")));
    std::uint8_t a0 = 0xFF;
    for (std::uint8_t w = 0; w < kWideSlots; ++w)
        if (reg.wide[w].id() == std::string_view("w_a0")) a0 = w;
    REQUIRE(a0 != 0xFF);
    FightSessionDef d;
    d.gift = freeGifts();
    StoryStepMark mark;
    REQUIRE(storyMarkStep(story, mark));
    FightSession s;
    REQUIRE(s.begin(d, 0.0).ok());
    REQUIRE(s.bindStory(story, reg, mark));
    s.setLeadHealth(kLeadA, 0.f);
    REQUIRE(s.selectGift(Gift::Heal));
    CHECK(s.giveGift(kLeadA).error == GiftError::Down);
    CHECK(s.lastStoryWrite() == SetResult::Unchanged);
    CHECK(storyWide(story, a0) == 0);
}

TEST_CASE("fight session: a defeat costs only the attempt: currency, Load, gift tally and story counters come back, and nothing else moved") {
    FactRegistry reg;
    std::string err;
    // The press registry with a retry counter bound, so the retry's +1 shows.
    const std::string csv = std::string(kPressCsv) + "6,w_retry,CTR,65535,0,-,any,-,-,-,-\n";
    std::string json(kPressJson);
    json.replace(json.find("\"roles\": {"), 10, "\"roles\": {\n    \"retries\": \"w_retry\",");
    REQUIRE_MESSAGE(importStoryRegistry(csv, json, reg, &err), err);
    StoryState story;
    storyReset(story);
    REQUIRE(storyBeginScene(story, reg, storySceneOrdinal(reg, "s1")));
    std::uint8_t a0 = 0xFF;
    for (std::uint8_t w = 0; w < kWideSlots; ++w)
        if (reg.wide[w].id() == std::string_view("w_a0")) a0 = w;
    std::uint8_t retries = 0xFF;
    for (std::uint8_t w = 0; w < kWideSlots; ++w)
        if (reg.wide[w].id() == std::string_view("w_retry")) retries = w;
    REQUIRE(a0 != 0xFF);
    REQUIRE(retries != 0xFF);
    FightSessionDef d;
    d.gift = freeGifts();
    d.gift.guardSeconds = 0.0;
    d.lift = true;
    d.keptLead = kLeadA;
    FightSession s;
    StoryStepMark mark;
    // A fight won with one Heal: the tally and story counter the next fight starts from.
    REQUIRE(s.begin(d, 0.0).ok());
    REQUIRE(storyMarkStep(story, mark));
    REQUIRE(s.bindStory(story, reg, mark));
    REQUIRE(s.selectGift(Gift::Heal));
    REQUIRE(s.giveGift(kLeadA).given);
    s.finish(true);
    REQUIRE(storyEndStep(story, reg, mark, StepEnd::Keep));
    // The fight, set up with known values; the host's part written.
    struct Party {
        std::int64_t currency = 250;
        std::array<int, 2> load{0, 7};
        std::array<int, 2> hp{80, 100};
    } party;
    REQUIRE(s.begin(d, 4.0).ok());
    REQUIRE(storyMarkStep(story, mark));
    REQUIRE(s.bindStory(story, reg, mark));
    s.setLoad(kLeadA, 3); // the kept gauge
    REQUIRE(s.keptLoad() == 3);
    FightHostCheckpoint host;
    host.currency = party.currency;
    host.load = party.load;
    host.hp = party.hp;
    REQUIRE(s.setHostCheckpoint(host) == CheckpointAt::FightStart);
    const std::uint64_t tallyAt = s.giftSlot().allPresses();
    REQUIRE(tallyAt == 1);
    REQUIRE(storyWide(story, a0) == 1);
    const StoryState storyAt = story;
    const std::uint64_t hashAt = s.stateHash();
    // The attempt changes each of them, then is lost.
    REQUIRE(s.selectGift(Gift::Heal));
    REQUIRE(s.giveGift(kLeadA).given);
    party.currency += 75;
    party.load = {0, 2};
    party.hp = {0, 0};
    s.setLoad(kLeadA, 1);
    s.keptParried(10);
    for (int i = 0; i < 30; ++i) s.tick(1.0 / 60.0);
    CHECK(s.keptLoad() != 3);
    CHECK(s.giftSlot().allPresses() == tallyAt + 1);
    CHECK(storyWide(story, a0) == 2);
    CHECK(s.stateHash() != hashAt);
    s.setLeadHealth(kLeadA, 0.f);
    s.setLeadHealth(kLeadB, 0.f);
    REQUIRE(s.tick(1.0 / 60.0).defeated);
    // The retry: the host applies its part, the story rolls the step back (a fight-start retry) and binds anew.
    const FightCheckpoint& cp = s.retry();
    REQUIRE(cp.at == CheckpointAt::FightStart);
    REQUIRE(cp.hostWritten);
    party.currency = cp.host.currency;
    party.load = cp.host.load;
    party.hp = cp.host.hp;
    REQUIRE(storyEndStep(story, reg, mark, StepEnd::Retry));
    REQUIRE(storyMarkStep(story, mark));
    REQUIRE(s.bindStory(story, reg, mark));
    CHECK(party.currency == 250);
    CHECK(party.load[kLeadB] == 7);
    CHECK(party.hp[kLeadA] == 80);
    CHECK(s.keptLoad() == 3);
    CHECK(s.giftSlot().allPresses() == tallyAt);
    CHECK(s.giftSlot().tally().givenOf(Gift::Heal) == 1);
    CHECK(storyWide(story, a0) == 1);
    // The whole story state as it was, but the retry counter (exactly +1) and the event ring (the rollback's
    // event, which the host's log reads).
    CHECK(story.facts == storyAt.facts);
    CHECK(story.factsEntry == storyAt.factsEntry);
    CHECK(story.stamp == storyAt.stamp);
    CHECK(story.stampEntry == storyAt.stampEntry);
    CHECK(story.wideEntry == storyAt.wideEntry);
    CHECK(story.extension == storyAt.extension);
    CHECK(story.extensionLength == storyAt.extensionLength);
    CHECK(story.extensionRaw == storyAt.extensionRaw);
    CHECK(story.openScene == storyAt.openScene);
    CHECK(story.lastCommitted == storyAt.lastCommitted);
    CHECK(story.flags == storyAt.flags);
    CHECK(story.saveSerial == storyAt.saveSerial);
    CHECK(story.registryHash == storyAt.registryHash);
    for (std::uint8_t w = 0; w < kWideSlots; ++w) {
        CAPTURE(static_cast<int>(w));
        CHECK(story.wide[w] == storyAt.wide[w] + (w == retries ? 1u : 0u));
    }
    // Nothing of the session differs from before the fight: every play field hashes the same.
    CHECK(s.stateHash() == hashAt);
}

// Tests only: reaches the session's fields so each edit below changes one alone (FightSession's friend, declared
// only where the test target defines FTCORE_TEST_ACCESS).
#ifndef FTCORE_TEST_ACCESS
#error "the test target defines FTCORE_TEST_ACCESS (Core/CMakeLists.txt)"
#endif
namespace ftcore {
struct FightSessionTestAccess {
    using Edit = void (*)(FightSession&);
    using CpEdit = void (*)(FightSession&, FightCheckpoint&);
    struct Row {
        const char* name;
        Edit edit;
    };
    struct CpRow {
        const char* name;
        CpEdit edit;
    };

    // Each row edits one hashed field alone and must move the hash, so dropping a term from stateHash fails here.
    // Three terms are framing that no single edit can isolate: hasBoss_, heldCount_ and pendingLoads_ each also
    // change how many fixed-width items are hashed after them, so the hash moves with or without the term.
    // A session where every hashed line has something to read: a boss script, a held blow and a pending Load in
    // their buffers, both checkpoints taken and written.
    static void fresh(FightSession& s) {
        FightSessionDef d;
        d.lift = true;
        d.keptLead = kLeadA;
        d.hasBoss = true;
        d.boss = oneMoveBoss();
        REQUIRE(s.begin(d, 0.0).ok());
        s.tick(1.0 / 60.0);
        REQUIRE(s.setHostCheckpoint(FightHostCheckpoint{}) == CheckpointAt::FightStart);
        s.phaseCp_ = s.startCp_;
        s.phaseCp_.at = CheckpointAt::Phase;
        s.heldCount_ = 1;
        s.pendingLoads_ = 1;
    }

    static void run() {
        static StoryState story;
        const Row rows[] = {
            {"now", [](FightSession& s) { s.now_ += 1.0; }},
            {"count", [](FightSession& s) { s.count_.reset(CountTuning{}, s.now_ + 5.0); }},
            {"gift", [](FightSession& s) { s.gift_.setState(SlotState::Hostile); }},
            {"table", [](FightSession& s) { s.table_.reset(TableTuning{}, s.now_ + 5.0); }},
            {"refusal", [](FightSession& s) { s.refusal_.offer(); }},
            {"lock", [](FightSession& s) { s.lock_.reset(LockTuning{}, s.now_ + 5.0); }},
            {"hasBoss", [](FightSession& s) { s.hasBoss_ = false; }},
            {"boss", [](FightSession& s) { s.boss_.update(s.now_ + 3.0); }},
            {"activeLead", [](FightSession& s) { s.activeLead_ = kLeadB; }},
            {"story bound", [](FightSession& s) { s.story_ = &story; }},
            {"lastStory", [](FightSession& s) { s.lastStory_ = SetResult::Ok; }},
            {"heldCount", [](FightSession& s) { s.heldCount_ = 2; }},
            {"heldDropped", [](FightSession& s) { ++s.heldDropped_; }},
            {"held blow", [](FightSession& s) { s.held_[0].blow = 7; }},
            {"held enemy", [](FightSession& s) { s.held_[0].enemy = 3; }},
            {"held move", [](FightSession& s) { s.held_[0].move = 2; }},
            {"held baseDamage", [](FightSession& s) { s.held_[0].baseDamage = 9; }},
            {"held landed", [](FightSession& s) { s.held_[0].landed = 2.f; }},
            {"lastStore A", [](FightSession& s) { s.lastStore_[kLeadA] = 5.0; }},
            {"lastStore B", [](FightSession& s) { s.lastStore_[kLeadB] = 5.0; }},
            {"pendingLoads", [](FightSession& s) { s.pendingLoads_ = 2; }},
            {"pendingLoadsDropped", [](FightSession& s) { ++s.pendingLoadsDropped_; }},
            {"load lead", [](FightSession& s) { s.loadsIn_[0].lead = kLeadB; }},
            {"load amount", [](FightSession& s) { s.loadsIn_[0].amount = 3; }},
            {"load kind", [](FightSession& s) { s.loadsIn_[0].kind = CountLoadKind::LiftSpend; }},
            {"load kept", [](FightSession& s) { s.loadsIn_[0].kept = true; }},
            {"mark bonus", [](FightSession& s) { s.marks_[0].bonus = 0.5f; }},
            {"mark until", [](FightSession& s) { s.marks_[0].until = 1.0; }},
            {"mark used", [](FightSession& s) { s.marks_[0].used = true; }},
            {"last mark", [](FightSession& s) { s.marks_[kMaxMarkedEnemies - 1].bonus = 0.5f; }},
            {"keptOn", [](FightSession& s) { s.keptOn_ = !s.keptOn_; }},
            {"kept", [](FightSession& s) { ++s.kept_; }},
            {"keptDown", [](FightSession& s) { s.keptDown_ = !s.keptDown_; }},
            {"keptCap", [](FightSession& s) { ++s.keptCap_; }},
            {"blowCap", [](FightSession& s) { s.blowCap_ += 1.f; }},
            {"lone", [](FightSession& s) { s.lone_ = !s.lone_; }},
            {"loneEnded", [](FightSession& s) { s.loneEnded_ = !s.loneEnded_; }},
            {"loneEndedBy", [](FightSession& s) { s.loneEndedBy_ = LoneEnd::Gift; }},
            {"loneBeganDue", [](FightSession& s) { s.loneBeganDue_ = !s.loneBeganDue_; }},
            {"loneEndDue", [](FightSession& s) { s.loneEndDue_ = !s.loneEndDue_; }},
            {"loneReleases", [](FightSession& s) { ++s.loneReleases_; }},
            {"loneFolded", [](FightSession& s) { s.loneFolded_ = !s.loneFolded_; }},
            {"lonePooled", [](FightSession& s) { ++s.lonePooled_; }},
            {"leadHealth A", [](FightSession& s) { s.leadHealth_[kLeadA] = 0.3f; }},
            {"leadHealth B", [](FightSession& s) { s.leadHealth_[kLeadB] = 0.3f; }},
            {"won", [](FightSession& s) { s.won_ = !s.won_; }},
            {"start", [](FightSession& s) { s.start_ += 1.0; }},
            {"inFight A", [](FightSession& s) { s.inFight_[kLeadA] = !s.inFight_[kLeadA]; }},
            {"inFight B", [](FightSession& s) { s.inFight_[kLeadB] = !s.inFight_[kLeadB]; }},
            {"down A", [](FightSession& s) { s.down_[kLeadA] = !s.down_[kLeadA]; }},
            {"down B", [](FightSession& s) { s.down_[kLeadB] = !s.down_[kLeadB]; }},
            {"stepDown A", [](FightSession& s) { s.stepDown_[kLeadA] = !s.stepDown_[kLeadA]; }},
            {"stepDown B", [](FightSession& s) { s.stepDown_[kLeadB] = !s.stepDown_[kLeadB]; }},
            {"defeated", [](FightSession& s) { s.defeated_ = !s.defeated_; }},
            {"fellDue", [](FightSession& s) { s.fellDue_ ^= 1; }},
            {"roseDue", [](FightSession& s) { s.roseDue_ ^= 1; }},
            {"heldUpDue", [](FightSession& s) { s.heldUpDue_ ^= 1; }},
            {"tookOverDue", [](FightSession& s) { s.tookOverDue_ = kLeadB; }},
            {"defeatedDue", [](FightSession& s) { s.defeatedDue_ = !s.defeatedDue_; }},
            {"startDue", [](FightSession& s) { s.startDue_ = !s.startDue_; }},
            {"cpDue", [](FightSession& s) { s.cpDue_ ^= 1; }},
            {"switchReq", [](FightSession& s) { s.switchReq_ = kLeadB; }},
            {"switchedDue", [](FightSession& s) { s.switchedDue_ = kLeadB; }},
            {"switchRefusedDue", [](FightSession& s) { s.switchRefusedDue_ = kLeadB; }},
            {"switchRefusalDue", [](FightSession& s) { s.switchRefusalDue_ = LeadSwitch::Down; }},
            {"switchAt", [](FightSession& s) { s.switchAt_ = 5.0; }},
            {"switchCooldown", [](FightSession& s) { s.switchCooldown_ = 2.0; }},
            {"joinReq", [](FightSession& s) { s.joinReq_ = kLeadB; }},
            {"joinHealth", [](FightSession& s) { s.joinHealth_ = 0.5f; }},
            {"joinedDue", [](FightSession& s) { s.joinedDue_ = kLeadB; }},
            {"joinRefusedDue", [](FightSession& s) { s.joinRefusedDue_ = kLeadB; }},
            {"joinRefusalDue", [](FightSession& s) { s.joinRefusalDue_ = LeadJoin::AlreadyIn; }},
        };
        const CpRow cpRows[] = {
            {"at", [](FightSession&, FightCheckpoint& c) { c.at = CheckpointAt::None; }},
            {"phase", [](FightSession&, FightCheckpoint& c) { ++c.phase; }},
            {"now", [](FightSession&, FightCheckpoint& c) { c.now += 1.0; }},
            {"hostWritten", [](FightSession&, FightCheckpoint& c) { c.hostWritten = !c.hostWritten; }},
            {"activeLead", [](FightSession&, FightCheckpoint& c) { c.activeLead = kLeadB; }},
            {"switchRequested", [](FightSession&, FightCheckpoint& c) { c.switchRequested = kLeadB; }},
            {"switchAt", [](FightSession&, FightCheckpoint& c) { c.switchAt = 5.0; }},
            {"inFight A", [](FightSession&, FightCheckpoint& c) { c.inFight[kLeadA] = !c.inFight[kLeadA]; }},
            {"inFight B", [](FightSession&, FightCheckpoint& c) { c.inFight[kLeadB] = !c.inFight[kLeadB]; }},
            {"joinRequested", [](FightSession&, FightCheckpoint& c) { c.joinRequested = kLeadB; }},
            {"joinHealth", [](FightSession&, FightCheckpoint& c) { c.joinHealth = 0.5f; }},
            {"leadHealth A", [](FightSession&, FightCheckpoint& c) { c.leadHealth[kLeadA] = 0.3f; }},
            {"leadHealth B", [](FightSession&, FightCheckpoint& c) { c.leadHealth[kLeadB] = 0.3f; }},
            {"down A", [](FightSession&, FightCheckpoint& c) { c.down[kLeadA] = !c.down[kLeadA]; }},
            {"down B", [](FightSession&, FightCheckpoint& c) { c.down[kLeadB] = !c.down[kLeadB]; }},
            {"gift", [](FightSession&, FightCheckpoint& c) { c.gift.setState(SlotState::Hostile); }},
            {"keptLoad", [](FightSession&, FightCheckpoint& c) { ++c.keptLoad; }},
            {"keptDown", [](FightSession&, FightCheckpoint& c) { c.keptDown = !c.keptDown; }},
            {"lone", [](FightSession&, FightCheckpoint& c) { c.lone = !c.lone; }},
            {"loneEnded", [](FightSession&, FightCheckpoint& c) { c.loneEnded = !c.loneEnded; }},
            {"loneEndedBy", [](FightSession&, FightCheckpoint& c) { c.loneEndedBy = LoneEnd::Gift; }},
            {"loneReleases", [](FightSession&, FightCheckpoint& c) { ++c.loneReleases; }},
            {"lonePooled", [](FightSession&, FightCheckpoint& c) { ++c.lonePooled; }},
            {"boss",
             [](FightSession& s, FightCheckpoint& c) {
                 BossScript b = s.boss_;
                 b.update(s.now_ + 3.0);
                 c.boss = b;
             }},
            {"host hp A", [](FightSession&, FightCheckpoint& c) { ++c.host.hp[kLeadA]; }},
            {"host hp B", [](FightSession&, FightCheckpoint& c) { ++c.host.hp[kLeadB]; }},
            {"host load A", [](FightSession&, FightCheckpoint& c) { ++c.host.load[kLeadA]; }},
            {"host load B", [](FightSession&, FightCheckpoint& c) { ++c.host.load[kLeadB]; }},
            {"host draws", [](FightSession&, FightCheckpoint& c) { ++c.host.draws; }},
            {"host partner", [](FightSession&, FightCheckpoint& c) { ++c.host.partner.rhythm; }},
            {"host order", [](FightSession&, FightCheckpoint& c) { c.host.order = PartnerOrder::Spread; }},
            {"host bossHp", [](FightSession&, FightCheckpoint& c) { ++c.host.bossHp; }},
            {"host bossPhase", [](FightSession&, FightCheckpoint& c) { ++c.host.bossPhase; }},
            {"host currency", [](FightSession&, FightCheckpoint& c) { ++c.host.currency; }},
        };

        FightSession base;
        fresh(base);
        const std::uint64_t h0 = base.stateHash();
        std::size_t moved = 0;
        const auto check = [&](const char* what, auto&& edit) {
            const std::string name = what;
            CAPTURE(name);
            FightSession s;
            fresh(s);
            REQUIRE(s.stateHash() == h0); // the same set-up hashes the same
            edit(s);
            CHECK(s.stateHash() != h0);
            ++moved;
        };
        for (const Row& r : rows) check(r.name, r.edit);
        for (std::size_t i = 0; i < kMaxFighters; ++i) {
            CAPTURE(i);
            check("places", [i](FightSession& s) { CHECK(s.places_[i].leaveOpen() == PlaceError::None); });
        }
        for (const bool phase : {false, true}) {
            CAPTURE(phase);
            for (const CpRow& r : cpRows)
                check(r.name, [&](FightSession& s) { r.edit(s, phase ? s.phaseCp_ : s.startCp_); });
            for (std::size_t i = 0; i < kMaxFighters; ++i) {
                CAPTURE(i);
                check("checkpoint places", [&](FightSession& s) {
                    CHECK((phase ? s.phaseCp_ : s.startCp_).places[i].leaveOpen() == PlaceError::None);
                });
            }
        }
        CHECK(moved == std::size(rows) + kMaxFighters * 3 + 2 * std::size(cpRows));

        // What is left out (see the header) moves nothing: the tick's outputs and stale buffer slots.
        const Edit left[] = {
            [](FightSession& s) { s.tick_.tookOver = kLeadB; },
            [](FightSession& s) { s.bonuses_[0].extraDamage = 5; },
            [](FightSession& s) { s.loadsOut_[0].amount = 5; },
            [](FightSession& s) { s.tickLoads_ = 3; },
            [](FightSession& s) { s.held_[1].blow = 5; },
            [](FightSession& s) { s.loadsIn_[1].amount = 5; },
        };
        for (const Edit e : left) {
            FightSession s;
            fresh(s);
            e(s);
            CHECK(s.stateHash() == h0);
        }
    }
};
} // namespace ftcore

TEST_CASE("fight session: stateHash moves with every hashed field alone, the checkpoints' payloads among them") {
    FightSessionTestAccess::run();
}
