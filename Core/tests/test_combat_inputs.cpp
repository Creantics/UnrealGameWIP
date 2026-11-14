// The shared combat inputs other than the count (test_strike_count.cpp): the third slot's gifts,
// laying places, setting the table, held refusals and the locked weak point.

#include "ftcore/GiftSlot.h"
#include "ftcore/HeldRefusal.h"
#include "ftcore/Places.h"
#include "ftcore/TableSet.h"
#include "ftcore/WeakPointLock.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <limits>

using namespace ftcore;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

CountTuning tempo60() {
    CountTuning t;
    t.bpm = 60.0;
    return t;
}

} // namespace

// ---- Gifts -----------------------------------------------------------------------------------------

TEST_CASE("gifts: each gift is given to a lead, counted, and leaves a thread") {
    GiftSlot g;
    StrikeCount count;
    REQUIRE(count.reset(tempo60(), 0.0));
    CHECK(g.state() == SlotState::Companion);
    CHECK(g.selected() == Gift::Heal);

    GiftResult r = g.press(kLeadA, 0.0, &count);
    CHECK(r.given);
    CHECK(r.gift == Gift::Heal);
    CHECK(r.target == kLeadA);
    CHECK(r.error == GiftError::None);
    CHECK(r.healShare == doctest::Approx(0.15f)); // what the host applies
    CHECK(r.healLoad == 20);

    REQUIRE(g.select(Gift::Guard));
    r = g.press(kLeadB, 1.0);
    CHECK(r.given);
    CHECK(r.guardScale == doctest::Approx(0.25f));
    CHECK(r.guardUntil == doctest::Approx(2.2));
    CHECK(r.healLoad == 0);
    CHECK(g.guarded(kLeadB, 1.5));
    CHECK(g.guardScaleFor(kLeadB, 1.5) == doctest::Approx(0.25f));
    CHECK(g.guardScaleFor(kLeadA, 1.5) == doctest::Approx(1.0f));
    CHECK(g.guardScaleFor(kLeadB, 2.2) == doctest::Approx(1.0f)); // guardSeconds 1.2 ran out
    CHECK_FALSE(g.guarded(kLeadB, 2.2));

    REQUIRE(g.select(Gift::Count));
    r = g.press(kLeadA, 2.0, &count);
    CHECK(r.given);
    CHECK(count.live());
    CHECK(count.caller() == kThirdSlot);
    // The slot's count: one lead's blow alone waits; a second fighter's blow in the window boosts both.
    CHECK(count.judgeBlow(kLeadA, 4.0).outcome == CountOutcome::Held);
    CHECK(count.judgeBlow(kLeadB, 4.05).outcome == CountOutcome::OnCount);

    CHECK(g.tally().count(Gift::Heal) == 1);
    CHECK(g.tally().count(Gift::Guard) == 1);
    CHECK(g.tally().count(Gift::Count) == 1);
    CHECK(g.tally().givenOf(Gift::Count) == 1);
    CHECK(g.tally().total() == 3);
    CHECK(g.threads(kLeadA) == 2);
    CHECK(g.threads(kLeadB) == 1);
    CHECK(g.threads(kThirdSlot) == 0);
}

TEST_CASE("gifts: cooldowns per gift; a press refused for its cooldown is tallied as refused") {
    GiftSlot g;
    CHECK(g.press(kLeadA, 0.0).given);
    const GiftResult cooling = g.press(kLeadA, 7.9);
    CHECK_FALSE(cooling.given);
    CHECK(cooling.error == GiftError::Cooling);
    CHECK(cooling.healLoad == 0); // nothing to apply
    CHECK_FALSE(g.ready(Gift::Heal, 7.9));
    CHECK(g.readyAt(Gift::Heal) == doctest::Approx(8.0)); // the refused press did not restart it
    CHECK(g.threads(kLeadA) == 1);
    // Another gift is not held up by the heal's cooldown.
    REQUIRE(g.select(Gift::Guard));
    CHECK(g.press(kLeadA, 7.9).given);
    REQUIRE(g.select(Gift::Heal));
    CHECK(g.press(kLeadA, 8.0).given);
    CHECK(g.tally().givenOf(Gift::Heal) == 2);
    CHECK(g.tally().refusedOf(Gift::Heal) == 1);
    CHECK(g.tally().count(Gift::Heal) == 3);
    CHECK(g.tally().total() == 4);
    CHECK(g.allPresses() == 4);
    CHECK(g.threads(kLeadA) == 3);
}

TEST_CASE("gifts: bad targets and selections; a bad target is tallied as refused") {
    GiftSlot g;
    CHECK(g.press(kThirdSlot, 0.0).error == GiftError::BadTarget);
    CHECK(g.press(kEnemyFighter, 0.0).error == GiftError::BadTarget);
    CHECK(g.press(kNoFighter, 0.0).error == GiftError::BadTarget);
    CHECK_FALSE(g.select(Gift::None));
    CHECK_FALSE(g.select(static_cast<Gift>(9)));
    CHECK(g.selected() == Gift::Heal);
    CHECK(g.tally().givenOf(Gift::Heal) == 0);
    CHECK(g.tally().refusedOf(Gift::Heal) == 3);
    CHECK(g.tally().total() == 3);
    CHECK(g.tally().mostUsed() == Gift::Heal);
    CHECK(g.ready(Gift::Heal, 0.0)); // no cooldown started
    CHECK(g.threads(kLeadA) == 0);
    CHECK(g.tally().refusedOf(static_cast<Gift>(9)) == 0);
}

TEST_CASE("gifts: a count gift with no count, or one the count refuses, is not given but is tallied") {
    GiftSlot g;
    REQUIRE(g.select(Gift::Count));
    const GiftResult none = g.press(kLeadA, 0.0, nullptr);
    CHECK_FALSE(none.given);
    CHECK(none.error == GiftError::CountRefused);
    CHECK(g.tally().refusedOf(Gift::Count) == 1);
    CHECK(g.ready(Gift::Count, 0.0));
    StrikeCount count;
    REQUIRE(count.reset(tempo60(), 0.0));
    REQUIRE(count.call(kLeadB, 0.0) == CountError::None);
    const GiftResult r = g.press(kLeadA, 0.5, &count);
    CHECK_FALSE(r.given);
    CHECK(r.error == GiftError::CountRefused);
    CHECK(g.tally().givenOf(Gift::Count) == 0);
    CHECK(g.tally().refusedOf(Gift::Count) == 2);
    CHECK(g.tally().total() == 2);
    CHECK(g.threads(kLeadA) == 0);
    CHECK(g.ready(Gift::Count, 0.5)); // no cooldown started
    CHECK(count.caller() == kLeadB);  // the lead's count stands
}

TEST_CASE("gifts: the count gift at its cooldown boundary") {
    GiftSlot g;
    StrikeCount count;
    REQUIRE(count.reset(tempo60(), 0.0));
    REQUIRE(g.select(Gift::Count));
    REQUIRE(g.press(kLeadA, 0.0, &count).given);
    CHECK(g.readyAt(Gift::Count) == doctest::Approx(4.0));
    count.update(3.9);
    const GiftResult early = g.press(kLeadA, std::nextafter(4.0, 0.0), &count);
    CHECK(early.error == GiftError::Cooling);
    CHECK(count.readyAt() < 4.0); // the count itself would have taken it
    const GiftResult onTime = g.press(kLeadB, 4.0, &count);
    CHECK(onTime.given);
    CHECK(count.caller() == kThirdSlot);
    CHECK(count.nowAt() == doctest::Approx(6.0));
    CHECK(g.tally().givenOf(Gift::Count) == 2);
    CHECK(g.tally().refusedOf(Gift::Count) == 1);
}

TEST_CASE("gifts: a vacant slot and a hostile slot do nothing, but every press is remembered") {
    GiftSlot g;
    StrikeCount count;
    g.setState(SlotState::Vacant);
    REQUIRE(g.select(Gift::Count));
    const GiftResult r = g.press(kLeadA, 0.0, &count);
    CHECK_FALSE(r.given);
    CHECK(r.error == GiftError::Vacant);
    CHECK_FALSE(count.live());
    CHECK(g.press(kLeadB, 0.1).error == GiftError::Vacant);
    CHECK(g.vacantPresses() == 2);
    CHECK(g.tally().total() == 0);
    CHECK(g.threads(kLeadA) == 0);

    g.setState(SlotState::Hostile);
    CHECK(g.press(kLeadA, 0.2, &count).error == GiftError::Hostile);
    CHECK(g.vacantPresses() == 2);
    CHECK(g.hostilePresses() == 1);
    CHECK(g.tally().total() == 0);
    CHECK(g.allPresses() == 3);
    CHECK_FALSE(count.live());

    g.setState(SlotState::Companion);
    CHECK(g.press(kLeadA, 0.3, &count).given);
    CHECK(g.allPresses() == 4);
    CHECK(g.press(kLeadA, 0.4, &count).error == GiftError::Cooling);
    CHECK(g.allPresses() == 5);
}

TEST_CASE("gifts: most used counts every press of a gift, with ties going to the earliest gift") {
    GiftTally t;
    CHECK(t.mostUsed() == Gift::None);
    t.given = {0, 3, 3};
    CHECK(t.mostUsed() == Gift::Guard);
    t.given = {2, 1, 5};
    CHECK(t.mostUsed() == Gift::Count);
    t.given = {4, 4, 4};
    CHECK(t.mostUsed() == Gift::Heal);
    t.given = {0, 0, 1};
    CHECK(t.mostUsed() == Gift::Count);
    t.refused = {2, 0, 0, 0}; // two refused heals outweigh one given count
    CHECK(t.mostUsed() == Gift::Heal);
    t.given = {0, 0, 0};
    t.refused = {0, 0, 0, 5}; // presses with no gift are no gift's
    CHECK(t.mostUsed() == Gift::None);
    CHECK(t.total() == 5);
    CHECK(t.count(Gift::None) == 5);
    CHECK(t.givenOf(Gift::None) == 0);
}

TEST_CASE("gifts: counters saturate instead of wrapping") {
    GiftSlot g;
    GiftSave s;
    s.given = {std::numeric_limits<std::uint32_t>::max(), 0, 0};
    s.refused = {std::numeric_limits<std::uint32_t>::max(), 0, 0, 0};
    s.threads = {std::numeric_limits<std::uint32_t>::max(), 0};
    s.vacantPresses = std::numeric_limits<std::uint32_t>::max();
    REQUIRE(g.load(s));
    CHECK(g.press(kLeadA, 0.0).given);
    CHECK(g.tally().givenOf(Gift::Heal) == std::numeric_limits<std::uint32_t>::max());
    CHECK(g.threads(kLeadA) == std::numeric_limits<std::uint32_t>::max());
    CHECK_FALSE(g.press(kLeadA, 0.5).given);
    CHECK(g.tally().refusedOf(Gift::Heal) == std::numeric_limits<std::uint32_t>::max());
    CHECK(g.tally().total() == 2ull * std::numeric_limits<std::uint32_t>::max());
    g.setState(SlotState::Vacant);
    g.press(kLeadA, 1.0);
    CHECK(g.vacantPresses() == std::numeric_limits<std::uint32_t>::max());
}

TEST_CASE("gifts: the tally, threads, choice and state survive save and load; the fight state does not") {
    GiftSlot a;
    GiftTuning quickGuard; // the second Guard at 10 s needs a cooldown under 10 s
    quickGuard.cooldown[1] = 6.0;
    REQUIRE(a.reset(quickGuard, 0.0));
    StrikeCount count;
    REQUIRE(count.reset(tempo60(), 0.0));
    a.press(kLeadA, 0.0);
    REQUIRE(a.select(Gift::Guard));
    a.press(kLeadB, 0.0);
    a.press(kLeadB, 10.0);
    REQUIRE(a.select(Gift::Count));
    a.press(kLeadA, 10.0, &count);
    a.press(kLeadA, 10.5, &count); // refused: cooling
    a.setState(SlotState::Vacant);
    a.press(kLeadA, 11.0);
    a.setState(SlotState::Hostile);
    a.press(kLeadA, 11.5);
    a.press(kLeadB, 11.6);
    a.setState(SlotState::Companion);
    REQUIRE(a.select(Gift::Guard));

    const GiftSave s = a.save();
    CHECK(s.version == kGiftSaveVersion);
    GiftSlot b;
    REQUIRE(b.load(s));
    CHECK(b.tally().count(Gift::Heal) == 1);
    CHECK(b.tally().count(Gift::Guard) == 2);
    CHECK(b.tally().givenOf(Gift::Count) == 1);
    CHECK(b.tally().refusedOf(Gift::Count) == 1);
    CHECK(b.tally().mostUsed() == Gift::Guard); // a tie with Count goes to the earlier gift
    CHECK(b.vacantPresses() == 1);
    CHECK(b.hostilePresses() == 2);
    CHECK(b.allPresses() == 8);
    CHECK(b.threads(kLeadA) == 2);
    CHECK(b.threads(kLeadB) == 2);
    CHECK(b.selected() == Gift::Guard);
    CHECK(b.state() == SlotState::Companion);
    // Cooldowns are fight state: the loaded slot can give at once.
    CHECK(b.press(kLeadA, 0.0).given);
    // A second round trip is stable.
    GiftSlot c;
    REQUIRE(c.load(b.save()));
    CHECK(c.save().given == b.save().given);
    CHECK(c.save().refused == b.save().refused);
}

TEST_CASE("gifts: a version 1 save loads, with no refused presses") {
    GiftSave v1;
    v1.version = 1;
    v1.given = {3, 1, 0};
    v1.refused = {9, 9, 9, 9}; // not in version 1: ignored
    v1.vacantPresses = 2;
    v1.state = static_cast<std::uint8_t>(SlotState::Vacant);
    GiftSlot g;
    REQUIRE(g.load(v1));
    CHECK(g.tally().givenOf(Gift::Heal) == 3);
    CHECK(g.tally().refusedOf(Gift::Heal) == 0);
    CHECK(g.tally().total() == 4);
    CHECK(g.vacantPresses() == 2);
    CHECK(g.state() == SlotState::Vacant);
    CHECK(static_cast<int>(SlotState::Vacant) == 1); // the stored integer is unchanged
    CHECK(g.save().version == kGiftSaveVersion);
}

TEST_CASE("gifts: bad saves are refused and change nothing") {
    GiftSlot g;
    g.press(kLeadA, 0.0);
    GiftSave newer = g.save();
    newer.version = kGiftSaveVersion + 1;
    CHECK_FALSE(g.load(newer));
    GiftSave zero = g.save();
    zero.version = 0;
    CHECK_FALSE(g.load(zero));
    GiftSave badGift = g.save();
    badGift.selected = 3;
    CHECK_FALSE(g.load(badGift));
    GiftSave badState = g.save();
    badState.state = 3;
    CHECK_FALSE(g.load(badState));
    CHECK(g.tally().count(Gift::Heal) == 1);
    CHECK(g.threads(kLeadA) == 1);
}

TEST_CASE("gifts: tuning is validated; NaN and earlier times read as the latest") {
    GiftSlot g;
    GiftTuning bad;
    bad.cooldown[1] = -1.0;
    CHECK_FALSE(g.reset(bad, 0.0));
    CHECK(g.tuning().cooldown[1] == doctest::Approx(12.0));
    GiftTuning nanGuard;
    nanGuard.guardSeconds = kNaN;
    CHECK_FALSE(validGiftTuning(nanGuard));

    GiftSlot h;
    CHECK(h.press(kLeadA, 5.0).given);
    // An earlier time is read as 5: still cooling.
    CHECK(h.press(kLeadA, 100.0 - 100.0).error == GiftError::Cooling);
    CHECK(h.press(kLeadA, kNaN).error == GiftError::Cooling);
    CHECK(h.press(kLeadA, 13.0).given);
}

// ---- Places ----------------------------------------------------------------------------------------

TEST_CASE("gifts: update keeps the clock moving through a long stretch without presses") {
    GiftSlot g;
    REQUIRE(g.press(kLeadA, 0.0).given);
    for (double t = 1.0; t <= 700.0; t += 1.0) g.update(t); // a frame each second, no presses
    CHECK(g.now() == 700.0);
    CHECK(g.ready(Gift::Heal, 701.0));
    const GiftResult r = g.press(kLeadA, 701.0);
    CHECK(r.given);
    CHECK(g.readyAt(Gift::Heal) == doctest::Approx(701.0 + g.tuning().cooldown[0]));
    g.update(702.0);
    CHECK(g.now() == 702.0);
    g.update(1e12); // junk is still ignored
    CHECK(g.now() == 702.0);
}

TEST_CASE("gifts: a slot not ticked for more than kMaxTimeJump stops at its clock (the host must tick it)") {
    GiftSlot g;
    REQUIRE(g.press(kLeadA, 0.0).given);
    // No update for 700 s: every later time is more than kMaxTimeJump ahead and is ignored.
    g.press(kLeadA, 700.0);
    g.update(701.0);
    CHECK(g.now() == 0.0);
    CHECK_FALSE(g.ready(Gift::Heal, 702.0));
    // A start time puts it back on the fight clock.
    g.resetFight(702.0);
    CHECK(g.now() == 702.0);
    CHECK(g.press(kLeadA, 702.5).given);
}

TEST_CASE("places: each named place raises the carry cap, to kMaxLoad at most") {
    PlaceTable p;
    REQUIRE(p.reset(5));
    CHECK(p.carryCap() == 40);
    CHECK(p.layPlace(11) == PlaceError::None);
    CHECK(p.carryCap() == 60);
    CHECK(p.layPlace(12) == PlaceError::None);
    CHECK(p.layPlace(13) == PlaceError::None);
    CHECK(p.carryCap() == 100);
    CHECK(p.layPlace(14) == PlaceError::None);
    CHECK(p.carryCap() == kMaxLoad);
    CHECK(p.named() == 4);
    CHECK(p.find(12) == 1);
    CHECK(p.find(99) == -1);
}

TEST_CASE("places: the rating limits chairs; a person once; no one is not a person") {
    PlaceTable p;
    REQUIRE(p.reset(2));
    CHECK(p.layPlace(kNoPerson) == PlaceError::BadPerson);
    CHECK(p.layPlace(1) == PlaceError::None);
    CHECK(p.layPlace(1) == PlaceError::AlreadySeated);
    CHECK(p.leaveOpen() == PlaceError::None);
    CHECK(p.layPlace(2) == PlaceError::NoSeat);
    CHECK(p.leaveOpen() == PlaceError::NoSeat);
    CHECK_FALSE(p.raiseSeats(1));
    CHECK_FALSE(p.raiseSeats(9));
    CHECK(p.raiseSeats(3));
    CHECK(p.layPlace(2) == PlaceError::None);
    CHECK(p.seats() == 3);
    PlaceTable q;
    q.reset(0);
    CHECK(q.seats() == 1);
    q.reset(20);
    CHECK(q.seats() == static_cast<int>(kMaxPlaces));
}

TEST_CASE("places: an open chair counts at the table but carries nothing") {
    PlaceTable p;
    REQUIRE(p.reset(4));
    p.layPlace(1);
    p.layPlace(2);
    p.leaveOpen();
    CHECK(p.carryCap() == 80);
    CHECK(p.tableSeats() == 3);
    CHECK(p.countOf(PlaceState::LeftOpen) == 1);
}

TEST_CASE("places: an emptied place drops the cap and its weight falls on the fighter") {
    PlaceTable p;
    REQUIRE(p.reset(3));
    p.layPlace(1);
    p.layPlace(2);
    p.layPlace(3);
    Load load;
    load.store(90);
    const PlaceEmptied r = p.emptyPlace(2, &load);
    CHECK(r.emptied);
    CHECK(r.chair == 1);
    CHECK(r.capBefore == 100);
    CHECK(r.capAfter == 80);
    CHECK(r.fell == 10); // only room to kMaxLoad
    CHECK(load.value == 100);
    CHECK(r.excess == 20);
    CHECK(p.overCap(load));
    CHECK(p.full(load));
    CHECK(p.place(1).state == PlaceState::Emptied);
    CHECK(p.place(1).person == 2);
    CHECK(p.tableSeats() == 2); // an emptied chair does not count
    // The chair stays taken until cleared.
    CHECK(p.layPlace(4) == PlaceError::NoSeat);
    CHECK(p.emptyPlace(2).error == PlaceError::NotSeated);
    CHECK(p.emptyPlace(kNoPerson).error == PlaceError::NotSeated);
    CHECK(p.clearPlace(0) == PlaceError::BadChair); // a named chair empties, it is never cleared
    CHECK(p.clearPlace(7) == PlaceError::BadChair);
    CHECK(p.clearPlace(-1) == PlaceError::BadChair);
    CHECK(p.clearPlace(1) == PlaceError::None);
    CHECK(p.layPlace(4) == PlaceError::None);

    // Without a Load the result still says what fell would be nothing stored.
    PlaceTable q;
    q.reset(1);
    q.layPlace(5);
    const PlaceEmptied r2 = q.emptyPlace(5);
    CHECK(r2.emptied);
    CHECK(r2.fell == 0);
    CHECK(r2.capAfter == 40);
}

TEST_CASE("places: one chair per person; an emptied chair holds its person until cleared") {
    PlaceTable p;
    REQUIRE(p.reset(4));
    REQUIRE(p.layPlace(1) == PlaceError::None);
    CHECK(p.layPlace(1) == PlaceError::AlreadySeated);
    REQUIRE(p.emptyPlace(1).emptied);
    CHECK(p.holding(1));
    CHECK(p.find(1) == -1); // emptied: not named
    CHECK(p.layPlace(1) == PlaceError::AlreadySeated);
    CHECK(p.named() == 0);
    REQUIRE(p.clearPlace(0) == PlaceError::None);
    CHECK_FALSE(p.holding(1));
    CHECK(p.layPlace(1) == PlaceError::None);
    CHECK(p.find(1) == 0);
    CHECK_FALSE(p.holding(kNoPerson));

    // A save with one person named in one chair and emptied in another does not load.
    PlaceSave twice = p.save();
    twice.states[2] = static_cast<std::uint8_t>(PlaceState::Emptied);
    twice.persons[2] = 1;
    PlaceTable q;
    CHECK_FALSE(q.load(twice));
    CHECK(q.named() == 0);
}

TEST_CASE("places: an emptied place below the new cap leaves no excess") {
    PlaceTable p;
    p.reset(2);
    p.layPlace(1);
    Load load;
    load.store(10);
    const PlaceEmptied r = p.emptyPlace(1, &load);
    CHECK(r.fell == 20);
    CHECK(load.value == 30);
    CHECK(r.excess == 0);
    CHECK_FALSE(p.overCap(load));
}

TEST_CASE("places: storeCapped stops at the place cap") {
    PlaceTable p;
    p.reset(3);
    p.layPlace(1); // cap 60
    Load load;
    CHECK(p.storeCapped(load, 50) == 50);
    CHECK(p.storeCapped(load, 50) == 10);
    CHECK(p.full(load));
    CHECK(p.storeCapped(load, 5) == 0);
    CHECK(p.storeCapped(load, -5) == 0);
    load.store(30); // past the cap by another path
    CHECK(p.storeCapped(load, 5) == 0);
    CHECK(load.value == 90);
}

TEST_CASE("places: save and load round trip; bad saves change nothing") {
    PlaceTable a;
    a.reset(4);
    a.layPlace(7);
    a.leaveOpen();
    a.layPlace(9);
    a.emptyPlace(9);
    const PlaceSave s = a.save();
    PlaceTable b;
    REQUIRE(b.load(s));
    CHECK(b.seats() == 4);
    CHECK(b.place(0).state == PlaceState::Named);
    CHECK(b.place(0).person == 7);
    CHECK(b.place(1).state == PlaceState::LeftOpen);
    CHECK(b.place(2).state == PlaceState::Emptied);
    CHECK(b.place(2).person == 9);
    CHECK(b.place(3).state == PlaceState::Free);
    CHECK(b.carryCap() == a.carryCap());

    PlaceSave bad = s;
    bad.version = 2;
    CHECK_FALSE(b.load(bad));
    bad = s;
    bad.seats = 0;
    CHECK_FALSE(b.load(bad));
    bad = s;
    bad.seats = 9;
    CHECK_FALSE(b.load(bad));
    bad = s;
    bad.states[1] = 4;
    CHECK_FALSE(b.load(bad));
    bad = s;
    bad.states[5] = static_cast<std::uint8_t>(PlaceState::Named); // past the rating
    bad.persons[5] = 3;
    CHECK_FALSE(b.load(bad));
    bad = s;
    bad.persons[0] = kNoPerson;
    CHECK_FALSE(b.load(bad));
    bad = s;
    bad.states[3] = static_cast<std::uint8_t>(PlaceState::Named);
    bad.persons[3] = 7; // named twice
    CHECK_FALSE(b.load(bad));
    CHECK(b.place(0).person == 7);
    CHECK(b.named() == 1);
}

TEST_CASE("places: bad tuning falls back to the defaults") {
    PlaceTable p;
    PlaceTuning t;
    t.baseCarry = 0;
    CHECK_FALSE(p.reset(3, t));
    CHECK(p.carryCap() == 40);
}

// ---- The table -------------------------------------------------------------------------------------

namespace {

// Three places named, a full Load.
struct Setter {
    PlaceTable places;
    Load load;
    Setter() {
        places.reset(3);
        places.layPlace(1);
        places.layPlace(2);
        places.layPlace(3);
        load.store(100);
    }
};

constexpr std::uint8_t kParty = fighterBit(kLeadA) | fighterBit(kLeadB) | fighterBit(kThirdSlot);

} // namespace

TEST_CASE("table: set at full Load with three seats, shared, then cleared and cooled") {
    Setter s;
    TableSet t;
    REQUIRE(t.begin(kLeadA, s.load, s.places, kParty, 10.0) == TableError::None);
    CHECK(t.state() == TableState::Setting);
    CHECK(t.setter() == kLeadA);
    const TableEvents& e1 = t.update(10.0);
    REQUIRE(e1.size() == 1);
    CHECK(e1[0].type == TableEventType::Setting);
    // Not shared while setting.
    CHECK(t.split(kLeadB, 30.0f, 10.5).share[kLeadB] == doctest::Approx(30.0f));

    const TableEvents& e2 = t.update(10.9);
    REQUIRE(e2.size() == 1);
    CHECK(e2[0].type == TableEventType::Set);
    CHECK(t.set());

    const TableSplit sp = t.split(kLeadB, 30.0f, 11.0);
    CHECK(sp.shared);
    CHECK(sp.share[kLeadA] == doctest::Approx(10.0f));
    CHECK(sp.share[kLeadB] == doctest::Approx(10.0f));
    CHECK(sp.share[kThirdSlot] == doctest::Approx(10.0f));
    CHECK(sp.share[kEnemyFighter] == doctest::Approx(0.0f));

    const TableEvents& e3 = t.update(16.9);
    REQUIRE(e3.size() == 1);
    CHECK(e3[0].type == TableEventType::Cleared);
    CHECK(e3[0].at == doctest::Approx(16.8));
    CHECK(t.state() == TableState::Cooling);
    CHECK(t.begin(kLeadA, s.load, s.places, kParty, 20.0) == TableError::Cooling);
    const TableEvents& e4 = t.update(36.9);
    REQUIRE(e4.size() == 1);
    CHECK(e4[0].type == TableEventType::Ready);
    CHECK(t.begin(kLeadA, s.load, s.places, kParty, 37.0) == TableError::NotFull); // the first begin spent it
    s.load.store(100);
    CHECK(t.begin(kLeadA, s.load, s.places, kParty, 37.0) == TableError::None);
}

TEST_CASE("table: begin spends the setter's Load on success and leaves it on an error") {
    Setter s;
    TableSet t;
    REQUIRE(s.load.value == 100);
    REQUIRE(t.begin(kLeadA, s.load, s.places, kParty, 0.0) == TableError::None);
    CHECK(s.load.value == 0);
    Load other;
    other.store(100);
    CHECK(t.begin(kLeadB, other, s.places, kParty, 0.1) == TableError::Busy);
    CHECK(other.value == 100);
    TableSet u;
    CHECK(u.begin(kEnemyFighter, other, s.places, kParty, 0.0) == TableError::BadSetter);
    CHECK(other.value == 100);
    PlaceTable two;
    two.reset(3);
    two.layPlace(1);
    two.layPlace(2);
    CHECK(u.begin(kLeadA, other, two, kParty, 0.0) == TableError::TooFewSeats);
    CHECK(other.value == 100);
}

TEST_CASE("table: a time more than kMaxTimeJump ahead is ignored and not stored") {
    Setter sa;
    Setter sb;
    TableSet a;
    TableSet b;
    REQUIRE(a.begin(kLeadA, sa.load, sa.places, kParty, 0.0) == TableError::None);
    REQUIRE(b.begin(kLeadA, sb.load, sb.places, kParty, 0.0) == TableError::None);
    REQUIRE(a.update(0.0).size() == 1); // Setting
    REQUIRE(b.update(0.0).size() == 1);
    CHECK(a.update(1e12).empty()); // junk: nothing happens, the clock stays at 0
    CHECK(a.update(1e300).empty());
    for (double t : {13.0, 14.0, 30.0}) {
        const TableEvents& ea = a.update(t);
        const TableEvents& eb = b.update(t);
        REQUIRE(ea.size() == eb.size());
        for (std::size_t i = 0; i < ea.size(); ++i) {
            CHECK(ea[i].type == eb[i].type);
            CHECK(ea[i].at == doctest::Approx(eb[i].at));
        }
        CHECK(a.state() == b.state());
        CHECK(a.until() == doctest::Approx(b.until()));
    }
    CHECK(a.state() == TableState::Ready);
    // A step of exactly kMaxTimeJump still counts.
    TableSet c;
    Setter sc;
    REQUIRE(c.begin(kLeadA, sc.load, sc.places, kParty, 0.0) == TableError::None);
    CHECK_FALSE(c.update(kMaxTimeJump).empty());
    // A reset or begin may start late on the clock.
    TableSet d;
    Setter sd;
    REQUIRE(d.reset({}, 7200.0));
    REQUIRE(d.begin(kLeadA, sd.load, sd.places, kParty, 7200.0) == TableError::None);
    CHECK(d.until() == doctest::Approx(7200.8));
}

TEST_CASE("table: events past the buffer are counted as dropped and reach the host") {
    TableTuning tune;
    tune.setTime = 0.0;
    tune.cooldown = 0.0;
    TableSet t;
    REQUIRE(t.reset(tune, 0.0));
    Setter s;
    // Each begin and cancel makes Setting, Set, Cleared and Ready: five rounds make 20 events.
    for (int i = 0; i < 5; ++i) {
        s.load.store(100);
        REQUIRE(t.begin(kLeadA, s.load, s.places, kParty, 1.0) == TableError::None);
        REQUIRE(t.cancel(1.0));
    }
    const TableEvents& ev = t.update(1.0);
    CHECK(ev.size() == kMaxTableEvents);
    CHECK(ev.dropped == 5 * 4 - kMaxTableEvents);
    const TableEvents& next = t.update(2.0);
    CHECK(next.empty());
    CHECK(next.dropped == 0);
}

TEST_CASE("table: needs full Load and three seats named or left open") {
    TableSet t;
    PlaceTable two;
    two.reset(3);
    two.layPlace(1);
    two.layPlace(2);
    Load full;
    full.store(100);
    CHECK(t.begin(kLeadA, full, two, kParty, 0.0) == TableError::TooFewSeats);
    two.leaveOpen(); // a chair deliberately left open makes the third
    CHECK(t.begin(kLeadA, full, two, kParty, 0.0) == TableError::None);

    TableSet u;
    Setter s;
    Load half;
    half.store(50);
    CHECK(u.begin(kLeadA, half, s.places, kParty, 0.0) == TableError::NotFull);
    // An emptied chair does not count.
    s.places.emptyPlace(3);
    Load cap80;
    cap80.store(80);
    CHECK(u.begin(kLeadA, cap80, s.places, kParty, 0.0) == TableError::TooFewSeats);
    CHECK(u.begin(kEnemyFighter, s.load, s.places, kParty, 0.0) == TableError::BadSetter);
    CHECK(u.state() == TableState::Ready);
}

TEST_CASE("table: interrupted while setting, it fails and cools down longer than nothing") {
    Setter s;
    TableSet t;
    REQUIRE(t.begin(kLeadA, s.load, s.places, kParty, 0.0) == TableError::None);
    CHECK(t.begin(kLeadB, s.load, s.places, kParty, 0.1) == TableError::Busy);
    CHECK(t.interrupt(0.5));
    CHECK(t.failed());
    CHECK(t.state() == TableState::Cooling);
    CHECK(t.until() == doctest::Approx(8.5));
    CHECK_FALSE(t.interrupt(0.6));
    const TableEvents& ev = t.update(0.6);
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].type == TableEventType::Setting);
    CHECK(ev[1].type == TableEventType::Failed);
    CHECK(t.begin(kLeadA, s.load, s.places, kParty, 8.0) == TableError::Cooling);
    CHECK(s.load.value == 0); // the Load stays spent
    s.load.store(100);
    CHECK(t.begin(kLeadA, s.load, s.places, kParty, 8.5) == TableError::None);
    CHECK_FALSE(t.failed());
}

TEST_CASE("table: an interrupt after it is set does nothing; cancel clears it") {
    Setter s;
    TableSet t;
    REQUIRE(t.begin(kLeadA, s.load, s.places, kParty, 0.0) == TableError::None);
    t.update(1.0);
    CHECK(t.set());
    CHECK_FALSE(t.interrupt(1.0));
    CHECK(t.set());
    CHECK(t.cancel(2.0));
    CHECK_FALSE(t.cancel(2.0));
    CHECK(t.state() == TableState::Cooling);
    CHECK(t.until() == doctest::Approx(22.0));
}

TEST_CASE("table: a place emptied under a set table gutters it out; a chair left open holds it") {
    Setter s;
    TableSet t;
    REQUIRE(t.begin(kLeadA, s.load, s.places, kParty, 0.0) == TableError::None);
    t.update(1.0);
    REQUIRE(t.set());
    CHECK_FALSE(t.checkSeats(s.places, 1.0)); // three seats: it holds
    REQUIRE(t.seatEnemy(1.0));
    s.places.emptyPlace(3);
    CHECK(t.checkSeats(s.places, 2.0));
    CHECK(t.failed());
    CHECK(t.state() == TableState::Cooling);
    CHECK(t.until() == doctest::Approx(10.0));
    CHECK_FALSE(t.enemySeated());
    CHECK_FALSE(t.split(kLeadB, 30.0f, 2.0).shared);
    const TableEvents& ev = t.update(2.0);
    REQUIRE_FALSE(ev.empty());
    CHECK(ev[ev.size() - 1].type == TableEventType::Guttered);
    CHECK(ev[ev.size() - 1].at == doctest::Approx(2.0));
    CHECK_FALSE(t.checkSeats(s.places, 2.5)); // already out

    // Two named and one left open: an open seat is a seat, and the table holds.
    TableSet u;
    PlaceTable open;
    open.reset(3);
    open.layPlace(1);
    open.layPlace(2);
    open.leaveOpen();
    Load full;
    full.store(100);
    REQUIRE(u.begin(kLeadA, full, open, kParty, 0.0) == TableError::None);
    CHECK_FALSE(u.checkSeats(open, 0.2));
    CHECK(u.state() == TableState::Setting);
    open.emptyPlace(2);
    CHECK(u.checkSeats(open, 0.3)); // while setting too
    CHECK(u.failed());
}

TEST_CASE("table: a seated enemy takes its share of every blow on the party; a blow on it is its own") {
    Setter s;
    TableSet t;
    REQUIRE(t.begin(kLeadA, s.load, s.places, fighterBit(kLeadB), 0.0) == TableError::None);
    CHECK(t.seated() == (fighterBit(kLeadA) | fighterBit(kLeadB))); // the setter always sits
    CHECK_FALSE(t.seatEnemy(0.5)); // not set yet
    t.update(0.8);
    CHECK(t.seatEnemy(1.0));
    CHECK_FALSE(t.seatEnemy(1.1));
    CHECK(t.enemySeated());

    const TableSplit sp = t.split(kLeadA, 30.0f, 1.2);
    CHECK(sp.share[kLeadA] == doctest::Approx(10.0f));
    CHECK(sp.share[kLeadB] == doctest::Approx(10.0f));
    CHECK(sp.share[kThirdSlot] == doctest::Approx(0.0f)); // not seated
    CHECK(sp.share[kEnemyFighter] == doctest::Approx(10.0f));

    const TableSplit onEnemy = t.split(kEnemyFighter, 30.0f, 1.3);
    CHECK_FALSE(onEnemy.shared);
    CHECK(onEnemy.share[kEnemyFighter] == doctest::Approx(30.0f));
    CHECK(onEnemy.share[kLeadA] == doctest::Approx(0.0f));

    const TableSplit unseated = t.split(kThirdSlot, 30.0f, 1.4);
    CHECK_FALSE(unseated.shared);
    CHECK(unseated.share[kThirdSlot] == doctest::Approx(30.0f));

    CHECK(t.split(kNoFighter, 30.0f, 1.5).share[kLeadA] == doctest::Approx(0.0f));
    CHECK(t.split(kLeadA, -5.0f, 1.5).share[kLeadA] == doctest::Approx(0.0f));

    // The enemy stands when the table clears.
    t.update(10.0);
    CHECK_FALSE(t.enemySeated());
    CHECK(t.split(kLeadA, 30.0f, 10.0).share[kLeadA] == doctest::Approx(30.0f));
    const TableEvents& ev = t.update(10.0);
    CHECK(ev.empty());
}

TEST_CASE("table: enemy share is tunable; a setter alone shares with no one") {
    TableTuning tune;
    tune.enemyShare = 2.0f;
    TableSet t;
    REQUIRE(t.reset(tune, 0.0));
    Setter s;
    REQUIRE(t.begin(kLeadB, s.load, s.places, 0, 0.0) == TableError::None);
    t.update(1.0);
    const TableSplit alone = t.split(kLeadB, 20.0f, 1.0);
    CHECK_FALSE(alone.shared);
    CHECK(alone.share[kLeadB] == doctest::Approx(20.0f));
    REQUIRE(t.seatEnemy(1.0));
    const TableSplit sp = t.split(kLeadB, 20.0f, 1.1);
    CHECK(sp.shared);
    CHECK(sp.share[kLeadB] == doctest::Approx(10.0f));
    CHECK(sp.share[kEnemyFighter] == doctest::Approx(20.0f));
}

TEST_CASE("table: one late frame plays setting, set, cleared and ready in order") {
    Setter s;
    TableSet t;
    REQUIRE(t.begin(kLeadA, s.load, s.places, kParty, 0.0) == TableError::None);
    const TableEvents& ev = t.update(500.0);
    REQUIRE(ev.size() == 4);
    CHECK(ev[0].type == TableEventType::Setting);
    CHECK(ev[1].type == TableEventType::Set);
    CHECK(ev[1].at == doctest::Approx(0.8));
    CHECK(ev[2].type == TableEventType::Cleared);
    CHECK(ev[2].at == doctest::Approx(6.8));
    CHECK(ev[3].type == TableEventType::Ready);
    CHECK(ev[3].at == doctest::Approx(26.8));
    CHECK(t.state() == TableState::Ready);
}

TEST_CASE("table: zero set time and zero cooldown; bad tuning; NaN time") {
    TableTuning tune;
    tune.setTime = 0.0;
    tune.cooldown = 0.0;
    TableSet t;
    REQUIRE(t.reset(tune, 0.0));
    Setter s;
    REQUIRE(t.begin(kLeadA, s.load, s.places, kParty, 1.0) == TableError::None);
    CHECK(t.set());
    CHECK(t.cancel(kNaN)); // read as 1.0
    CHECK(t.state() == TableState::Ready);
    const TableEvents& ev = t.update(1.0);
    REQUIRE(ev.size() == 4);
    CHECK(ev[3].type == TableEventType::Ready);
    CHECK(ev[3].at == doctest::Approx(1.0));

    TableTuning bad;
    bad.minSeats = 0;
    CHECK_FALSE(t.reset(bad, 0.0));
    CHECK(t.tuning().minSeats == 3);
    bad = TableTuning{};
    bad.setDuration = kNaN;
    CHECK_FALSE(validTableTuning(bad));
}

// ---- Held refusals ---------------------------------------------------------------------------------

TEST_CASE("refusal: a hold kept to the end of the offer refuses it") {
    HeldRefusal r;
    CHECK(r.state() == RefusalState::Idle);
    CHECK(r.holdDown() == RefusalInput::Ignored); // no offer yet
    r.holdUp();
    r.offer();
    CHECK(r.holdDown() == RefusalInput::Holding);
    CHECK(r.holding());
    CHECK(r.offerEnded() == RefusalInput::Resolved);
    CHECK(r.refused());
    CHECK_FALSE(r.accepted());
    CHECK(r.resolved());
    CHECK(r.repeats() == 0);
    // Resolved: further input is ignored.
    CHECK(r.holdUp() == RefusalInput::Ignored);
    CHECK(r.offerEnded() == RefusalInput::Ignored);
    CHECK(r.defer() == RefusalInput::Ignored);
    CHECK(r.refused());
}

TEST_CASE("refusal: releasing early repeats the prompt, with no limit") {
    HeldRefusal r;
    r.offer();
    for (int i = 1; i <= 50; ++i) {
        CHECK(r.holdDown() == RefusalInput::Holding);
        CHECK(r.holdUp() == RefusalInput::RepeatPrompt);
        CHECK(r.repeats() == i);
        CHECK(r.state() == RefusalState::Offered);
    }
    CHECK(r.holdDown() == RefusalInput::Holding);
    CHECK(r.offerEnded() == RefusalInput::Resolved);
    CHECK(r.refused());
    CHECK(r.repeats() == 50);
}

TEST_CASE("refusal: an end with no hold accepts") {
    HeldRefusal unheld;
    unheld.offer();
    CHECK(unheld.offerEnded() == RefusalInput::Resolved);
    CHECK(unheld.deferred());
    CHECK(unheld.accepted());

    HeldRefusal released;
    released.offer();
    released.holdDown();
    released.holdUp();
    CHECK(released.offerEnded() == RefusalInput::Resolved);
    CHECK(released.deferred());
    CHECK(released.accepted());

    HeldRefusal said;
    said.offer();
    said.holdDown();
    CHECK(said.defer() == RefusalInput::Resolved); // even mid-hold
    CHECK(said.deferred());
    CHECK(said.accepted());

    HeldRefusal yes;
    yes.offer();
    CHECK(yes.accept() == RefusalInput::Resolved);
    CHECK(yes.state() == RefusalState::Accepted);
    CHECK(yes.accepted());
    CHECK_FALSE(yes.deferred());

    HeldRefusal holdingYes;
    holdingYes.offer();
    holdingYes.holdDown();
    CHECK(holdingYes.accept() == RefusalInput::Ignored); // the hold is on: no accepting through it
}

TEST_CASE("refusal: a button already down when the offer starts does not count") {
    HeldRefusal r;
    r.holdDown();
    r.offer();
    CHECK(r.holdDown() == RefusalInput::Ignored); // repeat of the same press
    CHECK(r.offerEnded() == RefusalInput::Resolved);
    CHECK(r.deferred());

    HeldRefusal s;
    s.holdDown();
    s.offer();
    CHECK(s.holdUp() == RefusalInput::Ignored);
    CHECK(s.repeats() == 0);
    CHECK(s.holdDown() == RefusalInput::Holding);
    s.offerEnded();
    CHECK(s.refused());
}

TEST_CASE("refusal: a new offer starts over") {
    HeldRefusal r;
    r.offer();
    r.holdDown();
    r.holdUp();
    r.offerEnded();
    CHECK(r.deferred());
    r.offer();
    CHECK(r.state() == RefusalState::Offered);
    CHECK(r.repeats() == 0);
    r.holdDown();
    r.offerEnded();
    CHECK(r.refused());
}

// ---- The locked weak point -------------------------------------------------------------------------

TEST_CASE("lock: a shared blow opens a sharing lock for its open time") {
    WeakPointLock l;
    LockTuning tune;
    tune.keys = kUnlockSharedBlow;
    REQUIRE(l.reset(tune, 0.0));
    CHECK_FALSE(l.open(0.0));
    CHECK(l.offer(kUnlockRefusal, 0.0) == UnlockResult::WrongKey);
    CHECK(l.offer(kUnlockReadClause, 0.0, 5) == UnlockResult::WrongKey);
    CHECK_FALSE(l.open(0.0));

    StrikeCount count;
    REQUIRE(count.reset(tempo60(), 0.0));
    REQUIRE(count.call(kLeadA, 0.0) == CountError::None);
    const CountHit own = count.judgeBlow(kLeadA, 2.0);
    CHECK(own.outcome == CountOutcome::Held);
    CHECK_FALSE(sharedBlow(own));
    const CountHit hit = count.judgeBlow(kLeadB, 2.0);
    REQUIRE(sharedBlow(hit));
    // The held blow settles shared too, by an event.
    bool settledShared = false;
    for (const CountEvent& e : count.update(2.0)) settledShared = settledShared || (sharedBlow(e) && e.blow == own.blow);
    CHECK(settledShared);
    CHECK(l.offer(kUnlockSharedBlow, 2.0) == UnlockResult::Unlocked);
    CHECK(l.open(2.0));
    CHECK(l.until() == doctest::Approx(5.0));
    CHECK(l.open(4.99));
    CHECK_FALSE(l.open(5.0));
    CHECK(l.opens() == 1);
    // Again while open: the time restarts, no new opening.
    CHECK(l.offer(kUnlockSharedBlow, 4.0) == UnlockResult::Refreshed);
    CHECK(l.until() == doctest::Approx(7.0));
    CHECK(l.opens() == 1);
    // After it shut: a new opening.
    CHECK(l.offer(kUnlockSharedBlow, 8.0) == UnlockResult::Unlocked);
    CHECK(l.opens() == 2);
}

TEST_CASE("lock: reading the right hidden clause opens a reading lock; a wrong one does not") {
    WeakPointLock l;
    LockTuning tune;
    tune.keys = kUnlockReadClause;
    tune.clause = 42;
    REQUIRE(l.reset(tune, 0.0));
    CHECK(l.offer(kUnlockReadClause, 0.0, 41) == UnlockResult::WrongClause);
    CHECK(l.offer(kUnlockReadClause, 0.0, 0) == UnlockResult::WrongClause);
    CHECK_FALSE(l.open(0.0));
    CHECK(l.offer(kUnlockReadClause, 0.0, 42) == UnlockResult::Unlocked);
    CHECK(l.open(1.0));
    CHECK(l.offer(kUnlockSharedBlow, 0.5) == UnlockResult::WrongKey);
}

TEST_CASE("lock: a refusal opens a refusing lock; a deferral relocks it") {
    WeakPointLock l;
    LockTuning tune;
    tune.keys = kUnlockRefusal;
    tune.openSeconds = 0.0; // open until relocked
    REQUIRE(l.reset(tune, 0.0));
    HeldRefusal r;
    r.offer();
    r.holdDown();
    r.offerEnded();
    REQUIRE(r.refused());
    CHECK(l.offer(kUnlockRefusal, 1.0) == UnlockResult::Unlocked);
    CHECK(l.open(1000.0));
    CHECK(l.onDeferred(1000.0));
    CHECK_FALSE(l.open(1000.0));
    CHECK(l.deferrals() == 1);
    CHECK_FALSE(l.onDeferred(1001.0)); // already locked
    CHECK(l.deferrals() == 2);

    LockTuning keep = tune;
    keep.relockOnDefer = false;
    WeakPointLock m;
    REQUIRE(m.reset(keep, 0.0));
    m.offer(kUnlockRefusal, 0.0);
    CHECK_FALSE(m.onDeferred(1.0));
    CHECK(m.open(1.0));
    m.relock(2.0);
    CHECK_FALSE(m.open(2.0));
}

TEST_CASE("lock: a boss can take two keys; bad keys and tuning are refused") {
    WeakPointLock l;
    LockTuning tune;
    tune.keys = kUnlockSharedBlow | kUnlockRefusal;
    REQUIRE(l.reset(tune, 0.0));
    CHECK(l.offer(kUnlockRefusal, 0.0) == UnlockResult::Unlocked);
    CHECK(l.offer(static_cast<UnlockKey>(3), 0.0) == UnlockResult::BadKey);
    CHECK(l.offer(static_cast<UnlockKey>(0), 0.0) == UnlockResult::BadKey);

    LockTuning none;
    none.keys = 0;
    CHECK_FALSE(validLockTuning(none));
    LockTuning noClause;
    noClause.keys = kUnlockReadClause;
    CHECK_FALSE(validLockTuning(noClause));
    LockTuning extra;
    extra.keys = 8;
    CHECK_FALSE(validLockTuning(extra));
    LockTuning shortOpen;
    shortOpen.openSeconds = 0.05;
    CHECK_FALSE(validLockTuning(shortOpen));
    CHECK_FALSE(l.reset(shortOpen, 0.0));
    CHECK(l.tuning().keys == kUnlockSharedBlow);
}

TEST_CASE("lock: update closes a run-out window and keeps the clock moving between offers") {
    WeakPointLock l;
    LockTuning tune;
    tune.keys = kUnlockRefusal;
    tune.openSeconds = 3.0;
    REQUIRE(l.reset(tune, 0.0));
    for (double t = 1.0; t <= 700.0; t += 1.0) l.update(t);
    CHECK(l.offer(kUnlockRefusal, 701.0) == UnlockResult::Unlocked);
    CHECK(l.until() == doctest::Approx(704.0));
    l.update(703.5);
    CHECK(l.open(703.5));
    l.update(704.0);
    CHECK_FALSE(l.open(704.0));
    l.update(1e12); // junk is still ignored
    CHECK(l.offer(kUnlockRefusal, 705.0) == UnlockResult::Unlocked);
}

TEST_CASE("lock: earlier and NaN times read as the latest") {
    WeakPointLock l;
    REQUIRE(l.reset({}, 0.0));
    CHECK(l.offer(kUnlockSharedBlow, 10.0) == UnlockResult::Unlocked);
    CHECK(l.offer(kUnlockSharedBlow, kNaN) == UnlockResult::Refreshed);
    CHECK(l.until() == doctest::Approx(13.0));
    l.relock(0.0);
    CHECK_FALSE(l.open(10.0));
}

TEST_CASE("lock: a time more than kMaxTimeJump ahead is ignored and not stored, queries included") {
    WeakPointLock a;
    WeakPointLock b;
    REQUIRE(a.reset({}, 0.0));
    REQUIRE(b.reset({}, 0.0));
    CHECK(a.offer(kUnlockRefusal, 1e12) == UnlockResult::WrongKey); // junk: the clock stays at 0
    CHECK(a.offer(kUnlockSharedBlow, 13.0) == UnlockResult::Unlocked);
    CHECK(b.offer(kUnlockSharedBlow, 13.0) == UnlockResult::Unlocked);
    CHECK(a.until() == doctest::Approx(16.0));
    CHECK(a.until() == doctest::Approx(b.until()));
    CHECK(a.open(14.0));
    CHECK(a.open(1e12)); // a junk query reads as the clock (13): still open
    CHECK(a.offer(kUnlockSharedBlow, 14.0) == UnlockResult::Refreshed);
    CHECK(a.until() == doctest::Approx(17.0));
    CHECK_FALSE(a.open(17.0));
    // A reset starts the clock late with no jump bound: the window is open for its full length.
    WeakPointLock late;
    REQUIRE(late.reset({}, 7200.0));
    CHECK(late.offer(kUnlockSharedBlow, 7201.0) == UnlockResult::Unlocked);
    CHECK(late.until() == doctest::Approx(7204.0));
    CHECK(late.open(7203.9));
    LockedPoint p;
    p.reset(true, LockTuning{}, 1e5);
    CHECK(p.offer(kUnlockSharedBlow, 1e5 + 1.0) == UnlockResult::Unlocked);
    CHECK(p.open(1e5 + 3.9));
    CHECK_FALSE(p.open(1e5 + 4.0));
}

TEST_CASE("gifts: a time more than kMaxTimeJump ahead is ignored and not stored, queries included") {
    GiftSlot a;
    GiftSlot b;
    REQUIRE(a.press(kLeadA, 1e12).given); // given at the clock (0); the junk time is not stored
    REQUIRE(b.press(kLeadA, 0.0).given);
    CHECK(a.now() == doctest::Approx(0.0));
    for (double t : {13.0, 14.0}) {
        const GiftResult ra = a.press(kLeadA, t);
        const GiftResult rb = b.press(kLeadA, t);
        CHECK(ra.given == rb.given);
        CHECK(ra.error == rb.error);
        CHECK(a.now() == doctest::Approx(b.now()));
        CHECK(a.readyAt(Gift::Heal) == doctest::Approx(b.readyAt(Gift::Heal)));
    }
    CHECK(a.readyAt(Gift::Heal) == doctest::Approx(21.0));
    REQUIRE(a.select(Gift::Guard));
    REQUIRE(a.press(kLeadB, 15.0).given);
    CHECK(a.guarded(kLeadB, 1e12)); // a junk query reads as the clock (15)
    CHECK_FALSE(a.ready(Gift::Heal, 1e12));
    // resetFight starts the clock late with no jump bound.
    GiftSlot late;
    late.resetFight(7200.0);
    CHECK(late.now() == doctest::Approx(7200.0));
    REQUIRE(late.press(kLeadA, 7201.0).given);
    CHECK(late.readyAt(Gift::Heal) == doctest::Approx(7209.0));
}

TEST_CASE("combat inputs: a scripted fight of every input allocates nothing") {
    REQUIRE(ftcore_test::allocHookInstalled()); // a missing hook fails the test, it never skips it
    StrikeCount count;
    REQUIRE(count.reset(tempo60(), 0.0));
    GiftSlot gifts;
    Setter s;
    TableSet table;
    HeldRefusal refusal;
    WeakPointLock lock;
    REQUIRE(lock.reset({}, 0.0));
    std::size_t sink = 0;
    const std::uint64_t before = ftcore_test::allocStats().count;
    for (int frame = 0; frame < 7200; ++frame) {
        const double now = frame / 60.0;
        if (frame % 240 == 0) {
            gifts.select(static_cast<Gift>((frame / 240) % 3));
            sink += gifts.press(static_cast<FighterId>((frame / 240) % 2), now, &count).given;
        }
        if (frame % 5 == 0) {
            const CountHit h = count.judgeBlow(static_cast<FighterId>(frame % 2), now);
            if (sharedBlow(h)) sink += lock.offer(kUnlockSharedBlow, now) == UnlockResult::Unlocked;
            sink += table.checkSeats(s.places, now);
            const TableSplit sp = table.split(kLeadA, 10.0f, now);
            sink += sp.shared;
        }
        if (frame % 1800 == 10) {
            s.load.store(100);
            sink += table.begin(kLeadA, s.load, s.places, kParty, now) == TableError::None;
        }
        if (frame % 1800 == 200) sink += table.seatEnemy(now);
        if (frame % 600 == 0) refusal.offer();
        if (frame % 600 == 30) refusal.holdDown();
        if (frame % 600 == 300) refusal.offerEnded();
        if (frame % 600 == 301) refusal.holdUp();
        for (const CountEvent& e : count.update(now))
            if (sharedBlow(e)) sink += lock.offer(kUnlockSharedBlow, now) == UnlockResult::Unlocked;
        sink += table.update(now).size();
    }
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(sink > 0);
    CHECK(lock.opens() > 0);
    CHECK(gifts.tally().total() > 0);
}

TEST_CASE("combat inputs: small guards (place index, new gift tuning, a lost button, a bad gift)") {
    PlaceTable p;
    p.reset(3);
    p.layPlace(1);
    CHECK(p.place(0).state == PlaceState::Named);
    CHECK(p.place(-1).state == PlaceState::Free);
    CHECK(p.place(3).state == PlaceState::Free);
    CHECK(p.place(1000).state == PlaceState::Free);

    GiftSlot g;
    g.press(kLeadA, 0.0);
    GiftTuning slow;
    slow.cooldown[0] = 30.0;
    CHECK(g.setTuning(slow));
    CHECK(g.tally().total() == 1); // the save survives new tuning
    GiftTuning bad;
    bad.guardScale = 2.0f;
    CHECK_FALSE(g.setTuning(bad));
    CHECK(g.tuning().cooldown[0] == doctest::Approx(30.0));
    CHECK_FALSE(g.ready(static_cast<Gift>(9), 1e9));
    CHECK(g.readyAt(static_cast<Gift>(9)) == std::numeric_limits<double>::infinity());

    HeldRefusal r;
    r.holdDown(); // down before the offer
    r.offer();
    CHECK(r.holdDown() == RefusalInput::Ignored);
    r.clearButton(); // focus lost: the stale press is forgotten
    CHECK(r.holdDown() == RefusalInput::Holding);
    r.clearButton(); // a hold in progress stays a hold
    CHECK(r.holding());
    CHECK(r.offerEnded() == RefusalInput::Resolved);
    CHECK(r.refused());
}

TEST_CASE("gifts: a load keeps the slot's clock, so a slot loaded late keeps working") {
    GiftSlot a;
    a.resetFight(7200.0);
    REQUIRE(a.press(kLeadA, 7201.0).given);
    REQUIRE(a.load(a.save()));
    CHECK(a.now() == doctest::Approx(7201.0));
    CHECK(a.ready(Gift::Heal, 7201.0)); // cooldowns cleared by the load
    REQUIRE(a.press(kLeadA, 7202.0).given);
    CHECK(a.readyAt(Gift::Heal) == doctest::Approx(7210.0));
}

// ---- Gift limits -----------------------------------------------------------------------------------

TEST_CASE("gifts: the defaults are 2 Heals a fight at 15% and 8 s apart, a Guard of x0.25 for 1.2 s on 12 s") {
    const GiftTuning t;
    CHECK(t.healsPerFight == 2);
    CHECK(t.healShare == doctest::Approx(0.15f));
    CHECK(t.cooldown[0] == doctest::Approx(8.0));
    CHECK(t.guardScale == doctest::Approx(0.25f));
    CHECK(t.guardSeconds == doctest::Approx(1.2));
    CHECK(t.cooldown[1] == doctest::Approx(12.0));
    CHECK(t.cooldown[2] == doctest::Approx(4.0)); // the Count is unchanged
    CHECK(validGiftTuning(t));
}

TEST_CASE("gifts: a Heal at a lead at full health is refused NotHurt: no charge, no cooldown, no thread, no tally") {
    GiftSlot g;
    const std::uint64_t before = g.allPresses();
    GiftResult r = g.press(kLeadA, 0.0, nullptr, 1.0f);
    CHECK_FALSE(r.given);
    CHECK(r.error == GiftError::NotHurt);
    CHECK(r.gift == Gift::Heal);
    CHECK(r.healShare == 0.0f);
    CHECK(r.healLoad == 0);
    CHECK(g.healsLeft() == 2);
    CHECK(g.readyIn(Gift::Heal, 0.0) == 0.0);
    CHECK(g.threads(kLeadA) == 0u);
    CHECK(g.tally().givenOf(Gift::Heal) == 0u);
    CHECK(g.tally().refusedOf(Gift::Heal) == 0u);
    CHECK(g.allPresses() == before);
    // Only exactly 1 is full: over 1, NaN and an infinity are not a health and read as hurt (a fresh slot each, so
    // this slot's Heals stay); a hair under is hurt, and unknown (the default) reads as hurt.
    for (const float odd : {1.5f, 2.f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        GiftSlot o;
        CHECK(o.press(kLeadA, 0.0, nullptr, odd).given);
    }
    r = g.press(kLeadA, 0.0, nullptr, 0.99f);
    REQUIRE(r.given);
    CHECK(g.healsLeft() == 1);
    CHECK(g.threads(kLeadA) == 1u);
    CHECK(g.allPresses() == before + 1);
    // At full health while cooling: NotHurt (nothing to heal comes first), and still not tallied.
    CHECK(g.press(kLeadA, 1.0, nullptr, 1.0f).error == GiftError::NotHurt);
    CHECK(g.allPresses() == before + 1);
    CHECK(g.press(kLeadA, 1.0).error == GiftError::Cooling);
    CHECK(g.allPresses() == before + 2);
    // Only the Heal asks: a Guard at full health is given.
    REQUIRE(g.select(Gift::Guard));
    CHECK(g.press(kLeadB, 2.0, nullptr, 1.0f).given);
    // A slot that does nothing still answers first.
    REQUIRE(g.select(Gift::Heal));
    g.setState(SlotState::Vacant);
    CHECK(g.press(kLeadA, 20.0, nullptr, 1.0f).error == GiftError::Vacant);
    g.setState(SlotState::Companion);
    CHECK(g.press(kThirdSlot, 20.0, nullptr, 1.0f).error == GiftError::BadTarget);
}

TEST_CASE("gifts: at most 2 Heals a fight, 8 s apart; the third is refused NoneLeft and still counted") {
    GiftSlot g;
    CHECK(g.healsLeft() == 2);
    CHECK(g.readyIn(Gift::Heal, 0.0) == 0.0);
    GiftResult r = g.press(kLeadA, 0.0);
    REQUIRE(r.given);
    CHECK(r.healShare == doctest::Approx(0.15f));
    CHECK(g.healsLeft() == 1);
    CHECK(g.readyIn(Gift::Heal, 0.0) == doctest::Approx(8.0));
    CHECK(g.readyIn(Gift::Heal, 5.0) == doctest::Approx(3.0));
    // The spacing: 7.9 s is too soon, 8 s is not.
    r = g.press(kLeadB, 7.9);
    CHECK_FALSE(r.given);
    CHECK(r.error == GiftError::Cooling);
    CHECK(r.healShare == 0.0f);
    CHECK(g.healsLeft() == 1);
    r = g.press(kLeadB, 8.0);
    REQUIRE(r.given);
    CHECK(g.healsLeft() == 0);
    // The count: none left, however long the wait; nothing given, no thread, no cooldown restarted.
    r = g.press(kLeadA, 100.0);
    CHECK_FALSE(r.given);
    CHECK(r.error == GiftError::NoneLeft);
    CHECK(r.healShare == 0.0f);
    CHECK(g.healsLeft() == 0);
    CHECK(g.threads(kLeadA) == 1);
    CHECK(g.readyAt(Gift::Heal) == doctest::Approx(16.0));
    CHECK(g.readyIn(Gift::Heal, 100.0) == 0.0); // the cooldown is over; healsLeft says none are left
    // Every press counts toward the save-wide total.
    CHECK(g.tally().givenOf(Gift::Heal) == 2);
    CHECK(g.tally().refusedOf(Gift::Heal) == 2);
    CHECK(g.tally().total() == 4);
    // The other gifts are not limited by the Heal's count.
    REQUIRE(g.select(Gift::Guard));
    CHECK(g.press(kLeadA, 100.0).given);
}

TEST_CASE("gifts: a Guard is x0.25 damage for 1.2 s, then cools 12 s from the guard's end (13.2 s press to press)") {
    GiftSlot g;
    REQUIRE(g.select(Gift::Guard));
    CHECK(g.readyIn(Gift::Guard, 0.0) == 0.0);
    const GiftResult r = g.press(kLeadA, 10.0);
    REQUIRE(r.given);
    CHECK(r.guardScale == doctest::Approx(0.25f));
    CHECK(r.guardUntil == doctest::Approx(11.2));
    CHECK(g.guardScaleFor(kLeadA, 10.0) == doctest::Approx(0.25f));
    CHECK(g.guardScaleFor(kLeadA, 11.1) == doctest::Approx(0.25f));
    CHECK(g.guardScaleFor(kLeadA, 11.2) == doctest::Approx(1.0f));
    CHECK(g.guardScaleFor(kLeadB, 10.5) == doctest::Approx(1.0f)); // only the target
    // 100 damage on the guarded lead is 25.
    CHECK(100.0f * g.guardScaleFor(kLeadA, 10.5) == doctest::Approx(25.0f));
    CHECK(g.readyIn(Gift::Guard, 10.0) == doctest::Approx(13.2));
    CHECK(g.readyIn(Gift::Guard, 11.2) == doctest::Approx(12.0)); // the guard ends: 12 s from here
    CHECK(g.readyIn(Gift::Guard, 16.0) == doctest::Approx(7.2));
    CHECK(g.press(kLeadA, 22.0).error == GiftError::Cooling); // 12 s from the press is not enough
    CHECK(g.press(kLeadA, 23.1).error == GiftError::Cooling);
    CHECK(g.readyIn(Gift::Guard, 23.1) == doctest::Approx(0.1));
    CHECK(g.press(kLeadA, 23.2).given);
    CHECK(g.readyIn(Gift::Guard, 23.2) == doctest::Approx(13.2));
    // The Guard has no per-fight count.
    CHECK(g.healsLeft() == 2);
    CHECK(g.readyIn(Gift::None, 23.2) == 0.0);
}

TEST_CASE("gifts: beginFight resets the fight's Heals, cooldowns and guards but keeps the save-wide tally") {
    GiftSlot g;
    g.press(kLeadA, 0.0);
    g.press(kLeadA, 8.0);
    g.press(kLeadA, 16.0); // refused: none left
    REQUIRE(g.select(Gift::Guard));
    g.press(kLeadB, 16.0);
    g.setState(SlotState::Vacant);
    g.press(kLeadB, 17.0);
    REQUIRE(g.healsLeft() == 0);
    const std::uint64_t before = g.allPresses();
    REQUIRE(before == 5);

    CHECK(g.beginFight(GiftTuning{}, 100.0));
    CHECK(g.healsLeft() == 2);
    CHECK(g.readyIn(Gift::Heal, 100.0) == 0.0);
    CHECK(g.readyIn(Gift::Guard, 100.0) == 0.0);
    CHECK_FALSE(g.guarded(kLeadB, 100.0));
    CHECK(g.selected() == Gift::Heal);
    CHECK(g.state() == SlotState::Companion);
    CHECK(g.allPresses() == before);
    CHECK(g.tally().givenOf(Gift::Heal) == 2);
    CHECK(g.tally().refusedOf(Gift::Heal) == 1);
    CHECK(g.vacantPresses() == 1);
    CHECK(g.threads(kLeadA) == 2);
    CHECK(g.press(kLeadA, 100.0).given);
    CHECK(g.allPresses() == before + 1);
    // Bad tuning: the defaults, false, and the tally still kept.
    GiftTuning bad;
    bad.healsPerFight = 21;
    CHECK_FALSE(validGiftTuning(bad));
    CHECK_FALSE(g.beginFight(bad, 200.0));
    CHECK(g.tuning().healsPerFight == 2);
    CHECK(g.allPresses() == before + 1);
    // reset() still wipes everything.
    CHECK(g.reset({}, 300.0));
    CHECK(g.allPresses() == 0);
}

TEST_CASE("gifts: healsPerFight 0 gives no Heal; a load keeps the fight's Heal count cleared") {
    GiftTuning none;
    none.healsPerFight = 0;
    GiftSlot g;
    REQUIRE(g.reset(none, 0.0));
    CHECK(g.healsLeft() == 0);
    CHECK(g.press(kLeadA, 0.0).error == GiftError::NoneLeft);
    GiftSlot h;
    h.press(kLeadA, 0.0);
    h.press(kLeadA, 8.0);
    REQUIRE(h.healsLeft() == 0);
    REQUIRE(h.load(h.save())); // a load is never mid-fight
    CHECK(h.healsLeft() == 2);
}

TEST_CASE("gifts: pressing and the HUD reads allocate nothing") {
    GiftSlot g;
    const auto before = ftcore_test::allocStats().count;
    for (int i = 0; i < 40; ++i) {
        g.press(kLeadA, 0.5 * i);
        (void)g.healsLeft();
        (void)g.readyIn(Gift::Heal, 0.5 * i);
        (void)g.readyIn(Gift::Guard, 0.5 * i);
    }
    CHECK(ftcore_test::allocStats().count - before == 0);
}
