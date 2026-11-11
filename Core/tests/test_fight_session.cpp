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
    CHECK(s.begin(FightSessionDef{}, std::numeric_limits<double>::quiet_NaN()).ok());
    CHECK(s.now() == 0.0);
}

TEST_CASE("fight session: begin reports a refused tuning and keeps the defaults") {
    FightSession s;
    FightSessionDef d;
    d.count.bpm = 1000.0;
    d.gift.healShare = 2.0f;
    d.seats = 99;
    d.table.minSeats = 0;
    d.hasBoss = true; // the default def has no moves
    const FightSessionCheck c = s.begin(d, 0.0);
    CHECK(!c.ok());
    CHECK(!c.count);
    CHECK(!c.gift);
    CHECK(!c.table);
    CHECK(!c.boss);
    CHECK(c.lock);
    CHECK(s.strikeCount().tuning().bpm == CountTuning{}.bpm);
    CHECK(s.giftSlot().tuning().healShare == GiftTuning{}.healShare);
    CHECK(s.tick(kFrame).boss->empty());
    CHECK(s.counter(BossCounter::Guard, kLeadA) == BossInput::Ignored);
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
    CHECK(s.offerKey(kUnlockSharedBlow) == UnlockResult::BadKey); // the script takes its own keys
    // Hit-stop holds the boss too.
    const double stepEnds = s.boss().stepEnds();
    const BossState st = s.boss().state();
    for (int i = 0; i < 1000; ++i) s.tick(0.0);
    CHECK(s.boss().state() == st);
    CHECK(s.boss().stepEnds() == stepEnds);
}

TEST_CASE("fight session: given gifts are presses in the story state") {
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
    // A refused gift writes nothing; a capped counter answers Unchanged.
    CHECK(!s.giveGift(kThirdSlot).given);
    CHECK(s.lastStoryWrite() == SetResult::Unchanged);
    REQUIRE(s.selectGift(Gift::Heal));
    for (int i = 0; i < 4; ++i) CHECK(s.giveGift(kLeadA).given);
    CHECK(wide("w_a0") == 3);
    CHECK(s.lastStoryWrite() == SetResult::Unchanged);
    s.bindStory(nullptr, &reg);
    CHECK(s.giveGift(kLeadB).given);
    CHECK(wide("w_b0") == 0);
}

TEST_CASE("fight session: a fight's frames allocate nothing") {
    FightSession s;
    FightSessionDef d;
    d.hasBoss = true;
    d.boss = oneMoveBoss();
    d.gift = freeGifts();
    REQUIRE(s.begin(d, 0.0).ok());
    REQUIRE(s.selectGift(Gift::Heal));
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
