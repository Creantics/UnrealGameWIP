#include "ftcore/FightSession.h"

#include "AllocCounter.h"

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
    CHECK(!nan.ok());
    CHECK(s.now() == 0.0);
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
    REQUIRE(s.begin(d, 0.0).ok());
    REQUIRE(s.selectGift(Gift::Heal));
    CHECK(s.giveGift(kLeadA).given); // unbound: nothing written
    CHECK(s.lastStoryWrite() == SetResult::Unchanged);
    CHECK(wide("w_a0") == 0);

    s.bindStory(&story, nullptr); // both or neither
    CHECK(s.giveGift(kLeadA).given);
    CHECK(wide("w_a0") == 0);

    s.bindStory(&story, &reg);
    CHECK(s.press(SharedAction::Gift, kThirdSlot, kLeadA).accepted);
    CHECK(s.lastStoryWrite() == SetResult::Ok);
    CHECK(wide("w_a0") == 1);
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
    REQUIRE(s.selectGift(Gift::Heal));
    CHECK(s.giveGift(kLeadA).given);
    CHECK(s.lastStoryWrite() == SetResult::Ok);
    CHECK(wide("w_a0") == 1);
    s.bindStory(nullptr, &reg);
    CHECK(s.giveGift(kLeadB).given);
    CHECK(wide("w_b0") == 0);
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
    s.bindStory(&story, &reg);
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
    CHECK(s.activeLead() == kLeadB);
    CHECK(!s.setActiveLead(kThirdSlot));
    CHECK(!s.setActiveLead(kNoFighter));
    CHECK(s.activeLead() == kLeadB);
    REQUIRE(s.begin(FightSessionDef{}, 10.0).ok());
    CHECK(s.activeLead() == kLeadB);
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
