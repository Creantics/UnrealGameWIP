#include "ftcore/StrikeCount.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

using namespace ftcore;

namespace {

// 60 bpm: one beat a second. Cues at t, t + 1, the "now" at t + 2; window [now - 0.12, now + 0.18].
CountTuning tempo60() {
    CountTuning t;
    t.bpm = 60.0;
    return t;
}

StrikeCount count60() {
    StrikeCount c;
    REQUIRE(c.reset(tempo60()));
    return c;
}

// The settled-blow events in `ev`, in order.
int settled(const CountEvents& ev, CountEvent* out, int cap) {
    int n = 0;
    for (const CountEvent& e : ev)
        if (e.type == CountEventType::BlowSettled && n < cap) out[n++] = e;
    return n;
}

} // namespace

TEST_CASE("count: tuning is validated, a bad one falls back to the defaults") {
    StrikeCount c;
    CountTuning bad = tempo60();
    bad.windowBefore = 1.0; // a whole beat: the window would reach the previous cue
    CHECK_FALSE(c.reset(bad));
    CHECK(c.tuning().bpm == doctest::Approx(CountTuning{}.bpm));
    CountTuning nan = tempo60();
    nan.bpm = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(validCountTuning(nan));
    CountTuning heavyAlone = tempo60();
    heavyAlone.callerAloneMultiplier = 1.5f;
    CHECK_FALSE(validCountTuning(heavyAlone));
    CHECK(validCountTuning(CountTuning{}));
    CHECK(CountTuning{}.callerAloneMultiplier == doctest::Approx(0.5f));
    CHECK(CountTuning{}.falseMultiplier == doctest::Approx(0.0f));
}

TEST_CASE("count: two blows together in the window both hit far harder, the caller's included") {
    StrikeCount c = count60();
    REQUIRE(c.call(kLeadA, 10.0) == CountError::None);
    CHECK(c.update(10.0).size() == 1);
    CHECK(c.nowAt() == doctest::Approx(12.0));

    const CountHit own = c.judgeBlow(kLeadA, 12.0);
    CHECK(own.outcome == CountOutcome::Held);
    CHECK(own.multiplier == doctest::Approx(1.0f));
    CHECK(own.caller == kLeadA);
    CHECK(own.blow != 0);
    CHECK(c.heldBlows() == 1);
    CHECK_FALSE(c.joined());

    const CountHit partner = c.judgeBlow(kLeadB, 12.05);
    CHECK(partner.outcome == CountOutcome::OnCount);
    CHECK(partner.multiplier == doctest::Approx(2.5f));
    CHECK(partner.blow != own.blow);
    CHECK_FALSE(partner.punishable);
    CHECK(c.joined());
    CHECK(c.heldBlows() == 0);

    // Every later blow in the joined window is boosted at once, the same fighter's too.
    CHECK(c.judgeBlow(kThirdSlot, 12.1).outcome == CountOutcome::OnCount);
    CHECK(c.judgeBlow(kLeadB, 12.15).outcome == CountOutcome::OnCount);

    const CountEvents& ev = c.update(12.15);
    CountEvent s[4];
    REQUIRE(settled(ev, s, 4) == 1);
    CHECK(s[0].blow == own.blow);
    CHECK(s[0].striker == kLeadA);
    CHECK(s[0].outcome == CountOutcome::OnCount);
    CHECK(s[0].multiplier == doctest::Approx(2.5f));
    CHECK(s[0].at == doctest::Approx(12.05));
    CountEvent s2[4];
    CHECK(settled(c.update(20.0), s2, 4) == 0); // nothing more settles when the window closes
}

TEST_CASE("count: a partner first, the caller second, joins the same way") {
    StrikeCount c = count60();
    REQUIRE(c.call(kThirdSlot, 0.0) == CountError::None);
    const CountHit first = c.judgeBlow(kLeadA, 1.9);
    CHECK(first.outcome == CountOutcome::Held);
    CHECK(c.judgeBlow(kThirdSlot, 2.0).outcome == CountOutcome::OnCount);
    CountEvent s[4];
    REQUIRE(settled(c.update(2.0), s, 4) == 1);
    CHECK(s[0].blow == first.blow);
    CHECK(s[0].outcome == CountOutcome::OnCount);
}

TEST_CASE("count: a call nobody answered lands light; a lone partner's blow is plain") {
    StrikeCount c = count60();
    REQUIRE(c.call(kLeadA, 0.0) == CountError::None);
    c.update(0.0);
    const CountHit a1 = c.judgeBlow(kLeadA, 2.0);
    const CountHit a2 = c.judgeBlow(kLeadA, 2.1); // the same fighter again is no partner
    CHECK(a1.outcome == CountOutcome::Held);
    CHECK(a2.outcome == CountOutcome::Held);
    CHECK(c.heldBlows() == 2);
    const CountEvents& ev = c.update(3.0);
    REQUIRE(ev.size() == 6);
    CHECK(ev[3].type == CountEventType::WindowClosed);
    CHECK(ev[4].type == CountEventType::BlowSettled);
    CHECK(ev[4].blow == a1.blow);
    CHECK(ev[4].outcome == CountOutcome::Unanswered);
    CHECK(ev[4].multiplier == doctest::Approx(0.5f));
    CHECK(ev[4].at == doctest::Approx(2.18));
    CHECK(ev[5].blow == a2.blow);
    CHECK(ev[5].outcome == CountOutcome::Unanswered);

    StrikeCount d = count60();
    REQUIRE(d.call(kLeadA, 0.0) == CountError::None);
    const CountHit b = d.judgeBlow(kLeadB, 2.0);
    CHECK(b.outcome == CountOutcome::Held);
    CountEvent s[4];
    REQUIRE(settled(d.update(3.0), s, 4) == 1);
    CHECK(s[0].outcome == CountOutcome::Alone);
    CHECK(s[0].multiplier == doctest::Approx(1.0f));
    CHECK(s[0].striker == kLeadB);
}

TEST_CASE("count: the caller-alone weight is a knob") {
    CountTuning t = tempo60();
    t.callerAloneMultiplier = 0.25f;
    StrikeCount c;
    REQUIRE(c.reset(t));
    REQUIRE(c.call(kLeadB, 0.0) == CountError::None);
    c.judgeBlow(kLeadB, 2.0);
    CountEvent s[2];
    REQUIRE(settled(c.update(5.0), s, 2) == 1);
    CHECK(s[0].multiplier == doctest::Approx(0.25f));
}

TEST_CASE("count: no count at all is a lone blow, settled at once") {
    StrikeCount c = count60();
    const CountHit h = c.judgeBlow(kLeadB, 5.0);
    CHECK(h.outcome == CountOutcome::Alone);
    CHECK(h.multiplier == doctest::Approx(1.0f));
    CHECK(h.caller == kNoFighter);
    CHECK(h.blow != 0);
    CHECK(c.update(6.0).empty());
    CHECK_FALSE(c.silenced()); // nothing called
}

TEST_CASE("count: the window's edges are inclusive") {
    StrikeCount c = count60();
    REQUIRE(c.call(kLeadA, 0.0) == CountError::None);
    CHECK(c.windowStart() == doctest::Approx(1.88));
    CHECK(c.windowEnd() == doctest::Approx(2.18));
    CHECK(c.judgeBlow(kLeadB, c.windowStart()).outcome == CountOutcome::Held);
    CHECK(c.judgeBlow(kThirdSlot, c.windowEnd()).outcome == CountOutcome::OnCount);

    StrikeCount e = count60();
    REQUIRE(e.call(kLeadA, 0.0) == CountError::None);
    CHECK(e.judgeBlow(kLeadB, std::nextafter(e.windowStart(), 0.0)).outcome == CountOutcome::Alone);
    StrikeCount f = count60();
    REQUIRE(f.call(kLeadA, 0.0) == CountError::None);
    CHECK(f.judgeBlow(kLeadB, std::nextafter(f.windowEnd(), 10.0)).outcome == CountOutcome::Alone);
}

TEST_CASE("count: calls are refused while live, while cooling, and for the enemy or a bad id") {
    StrikeCount c = count60();
    CHECK(c.call(kEnemyFighter, 0.0) == CountError::BadCaller);
    CHECK(c.call(7, 0.0) == CountError::BadCaller);
    REQUIRE(c.call(kLeadA, 0.0) == CountError::None);
    CHECK(c.call(kLeadB, 1.0) == CountError::Live);
    CHECK(c.readyAt() == doctest::Approx(2.18 + 1.5));
    c.judgeBlow(kLeadA, 2.0);
    c.judgeBlow(kLeadB, 2.0);
    CHECK(c.joined());
    CHECK(c.call(kLeadB, 3.0) == CountError::Cooling);
    CHECK(c.call(kLeadB, 3.7) == CountError::None);
    CHECK(c.caller() == kLeadB);
    CHECK_FALSE(c.joined()); // the new count starts unjoined
    CHECK(c.judgeBlow(kLeadA, 3.7 + 2.0).outcome == CountOutcome::Held);
}

TEST_CASE("count: a count replaced before an update still plays its due steps and settles its blows") {
    CountTuning t = tempo60();
    t.cooldown = 0.0;
    StrikeCount c;
    REQUIRE(c.reset(t));
    REQUIRE(c.call(kLeadA, 0.0) == CountError::None);
    const CountHit h = c.judgeBlow(kLeadA, 2.0);
    REQUIRE(c.call(kLeadB, 2.5) == CountError::None);
    const CountEvents& ev = c.update(2.5);
    REQUIRE(ev.size() == 7);
    CHECK(ev[0].caller == kLeadA); // the old count's cues 0 and 1, window, "now", close
    CHECK(ev[4].type == CountEventType::WindowClosed);
    CHECK(ev[5].type == CountEventType::BlowSettled);
    CHECK(ev[5].blow == h.blow);
    CHECK(ev[5].outcome == CountOutcome::Unanswered);
    CHECK(ev[5].caller == kLeadA);
    CHECK(ev[6].type == CountEventType::Cue);
    CHECK(ev[6].caller == kLeadB);
    double last = -1.0;
    for (const CountEvent& e : ev) {
        CHECK(e.at >= last);
        last = e.at;
    }
}

TEST_CASE("count: a silenced count has no caller, boosts two blows together, but less and on a narrower window") {
    StrikeCount c = count60();
    REQUIRE(c.call(kNoFighter, 0.0, CountVoice::Voice) == CountError::None);
    CHECK(c.silenced());
    const CountEvents& ev = c.update(0.0);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].voice == CountVoice::Click);
    CHECK(ev[0].caller == kNoFighter);
    CHECK(c.windowStart() == doctest::Approx(2.0 - 0.12 * 0.6));
    CHECK(c.windowEnd() == doctest::Approx(2.0 + 0.18 * 0.6));
    CHECK(c.judgeBlow(kLeadA, 1.9).outcome == CountOutcome::Alone); // inside the full window only
    CHECK(c.judgeBlow(kLeadA, 2.0).outcome == CountOutcome::Held);
    const CountHit h = c.judgeBlow(kLeadB, 2.05);
    CHECK(h.outcome == CountOutcome::OnCount);
    CHECK(h.multiplier == doctest::Approx(1.6f));
    CountEvent s[2];
    REQUIRE(settled(c.update(2.05), s, 2) == 1);
    CHECK(s[0].multiplier == doctest::Approx(1.6f));

    // One blow alone on a silenced count: nobody called, so it is plain, never light.
    StrikeCount d = count60();
    REQUIRE(d.call(kNoFighter, 0.0) == CountError::None);
    d.judgeBlow(kLeadA, 2.0);
    REQUIRE(settled(d.update(3.0), s, 2) == 1);
    CHECK(s[0].outcome == CountOutcome::Alone);
    CHECK(s[0].multiplier == doctest::Approx(1.0f));
    CHECK_FALSE(d.silenced()); // over
}

TEST_CASE("count: cues and window edges play in order, one late frame plays them all") {
    StrikeCount c = count60();
    REQUIRE(c.call(kLeadB, 0.0, CountVoice::Click) == CountError::None);
    const CountEvents& first = c.update(0.0);
    REQUIRE(first.size() == 1);
    CHECK(first[0].type == CountEventType::Cue);
    CHECK(first[0].index == 0);
    CHECK_FALSE(first[0].isNow);
    CHECK(first[0].voice == CountVoice::Click);
    CHECK(first[0].caller == kLeadB);
    CHECK(c.update(0.5).empty());

    const CountEvents& rest = c.update(10.0);
    REQUIRE(rest.size() == 4);
    CHECK(rest[0].type == CountEventType::Cue);
    CHECK(rest[0].index == 1);
    CHECK(rest[0].at == doctest::Approx(1.0));
    CHECK(rest[1].type == CountEventType::WindowOpened);
    CHECK(rest[1].at == doctest::Approx(1.88));
    CHECK(rest[2].type == CountEventType::Cue);
    CHECK(rest[2].index == 2);
    CHECK(rest[2].isNow);
    CHECK(rest[2].at == doctest::Approx(2.0));
    CHECK(rest[3].type == CountEventType::WindowClosed);
    CHECK(rest[3].at == doctest::Approx(2.18));
    CHECK_FALSE(c.live());
    CHECK(c.update(20.0).empty());
}

TEST_CASE("count: a false count early or late, flagged; a blow on it misses and leaves the fighter open") {
    for (const double shift : {-0.4, 0.4}) {
        CAPTURE(shift);
        StrikeCount c = count60();
        c.falseCall(0.0, shift);
        CHECK(c.falseLive());
        CHECK(c.falseNowAt() == doctest::Approx(2.0 + shift));
        const CountHit h = c.judgeBlow(kLeadA, 2.0 + shift);
        CHECK(h.outcome == CountOutcome::FalseCount);
        CHECK(h.multiplier == doctest::Approx(0.0f));
        CHECK(h.punishable);
        StrikeCount other = count60();
        other.falseCall(0.0, shift);
        CHECK(other.judgeBlow(kLeadB, 2.0).outcome == CountOutcome::Alone);

        const CountEvents& ev = c.update(5.0);
        REQUIRE(ev.size() == 3);
        for (const CountEvent& e : ev) {
            CHECK(e.isFalse);
            CHECK(e.caller == kEnemyFighter);
            CHECK(e.type == CountEventType::Cue);
        }
        CHECK(ev[1].at == doctest::Approx(1.0)); // the second cue keeps the tempo
        CHECK(ev[2].isNow);
        CHECK(ev[2].at == doctest::Approx(2.0 + shift));
        CHECK_FALSE(c.falseLive());
    }
    CountTuning t = tempo60();
    t.falseMultiplier = 0.5f;
    StrikeCount g;
    REQUIRE(g.reset(t));
    g.falseCall(0.0, 0.3);
    CHECK(g.judgeBlow(kLeadA, 2.3).multiplier == doctest::Approx(0.5f));
}

TEST_CASE("count: a false count's shift stays within a beat; NaN shift is on time") {
    StrikeCount c = count60();
    c.falseCall(0.0, -5.0);
    CHECK(c.falseNowAt() == doctest::Approx(1.0)); // never before its own second cue
    c.falseCall(0.5, std::numeric_limits<double>::quiet_NaN());
    CHECK(c.falseNowAt() == doctest::Approx(2.5));
    c.falseCall(1.0, 1e9);
    CHECK(c.falseNowAt() == doctest::Approx(4.0));
    c.falseCall(1.0, std::numeric_limits<double>::infinity());
    CHECK(c.falseNowAt() == doctest::Approx(3.0));
    c.update(10.0);
    CHECK_FALSE(c.falseLive());
}

TEST_CASE("count: a true window wins over an overlapping false one, and a new false count replaces the old") {
    StrikeCount c = count60();
    REQUIRE(c.call(kLeadA, 0.0) == CountError::None);
    c.falseCall(0.0, 0.1);
    CHECK(c.judgeBlow(kLeadB, 2.1).outcome == CountOutcome::Held);
    CHECK(c.judgeBlow(kLeadA, 2.15).outcome == CountOutcome::OnCount);
    CHECK(c.judgeBlow(kThirdSlot, 2.2).outcome == CountOutcome::FalseCount); // past the true window

    StrikeCount d = count60();
    d.falseCall(0.0, 0.3);
    CHECK(d.update(0.0).size() == 1);
    d.falseCall(0.5, -0.3);
    CHECK(d.falseNowAt() == doctest::Approx(2.2));
    const CountEvents& ev = d.update(10.0);
    REQUIRE(ev.size() == 3); // the new one's three cues; the old one's two not yet due are dropped
    CHECK(ev[0].at == doctest::Approx(0.5));
    CHECK(ev[2].at == doctest::Approx(2.2));
}

TEST_CASE("count: true and false cues interleave in time order, true first on a tie") {
    StrikeCount c = count60();
    REQUIRE(c.call(kLeadA, 0.0) == CountError::None);
    c.falseCall(0.0, 0.5);
    const CountEvents& ev = c.update(10.0);
    REQUIRE(ev.size() == 8);
    double last = -1.0;
    for (const CountEvent& e : ev) {
        CHECK(e.at >= last);
        last = e.at;
    }
    CHECK_FALSE(ev[0].isFalse);
    CHECK(ev[1].isFalse);
    CHECK(ev[7].isFalse);
    CHECK(ev[7].isNow);
}

TEST_CASE("count: cancel plays what was due, closes an opened window, settles held blows") {
    StrikeCount c = count60();
    REQUIRE(c.call(kLeadA, 0.0) == CountError::None);
    CHECK(c.update(0.0).size() == 1);
    const CountHit h = c.judgeBlow(kLeadA, 1.95);
    c.cancel(2.0);
    CHECK_FALSE(c.live());
    CHECK(c.heldBlows() == 0);
    const CountEvents& ev = c.update(2.0);
    REQUIRE(ev.size() == 5);
    CHECK(ev[0].index == 1);
    CHECK(ev[1].type == CountEventType::WindowOpened);
    CHECK(ev[2].isNow);
    CHECK(ev[3].type == CountEventType::WindowClosed);
    CHECK(ev[3].at == doctest::Approx(2.0));
    CHECK(ev[4].type == CountEventType::BlowSettled);
    CHECK(ev[4].blow == h.blow);
    CHECK(ev[4].outcome == CountOutcome::Unanswered);
    CHECK(c.readyAt() == doctest::Approx(3.5));
    CHECK(c.call(kLeadB, 3.4) == CountError::Cooling);
    CHECK(c.judgeBlow(kLeadB, 2.05).outcome == CountOutcome::Alone); // read as 3.4: the window is gone
    CHECK(c.update(3.45).empty());
    CHECK(c.call(kLeadB, 3.5) == CountError::None);

    // Cancelled before its window: only the cues due, no window events.
    StrikeCount e = count60();
    REQUIRE(e.call(kLeadA, 0.0) == CountError::None);
    e.cancel(1.5);
    const CountEvents& ev2 = e.update(5.0);
    REQUIRE(ev2.size() == 2);
    CHECK(ev2[1].index == 1);

    StrikeCount d = count60();
    d.falseCall(0.0, 0.0);
    d.cancelFalse(0.5);
    CHECK_FALSE(d.falseLive());
    CHECK(d.judgeBlow(kLeadA, 2.0).outcome == CountOutcome::Alone);
    CHECK(d.update(3.0).size() == 1); // its first cue was due
}

TEST_CASE("count: earlier and non-finite times read as the latest seen") {
    StrikeCount c = count60();
    c.update(5.0);
    REQUIRE(c.call(kLeadA, 1.0) == CountError::None); // read as 5
    CHECK(c.nowAt() == doctest::Approx(7.0));
    CHECK(c.judgeBlow(kLeadB, std::numeric_limits<double>::quiet_NaN()).outcome == CountOutcome::Alone);
    CHECK(c.now() == doctest::Approx(5.0));
    CHECK(c.judgeBlow(kLeadB, std::numeric_limits<double>::infinity()).outcome == CountOutcome::Alone);
    CHECK(c.now() == doctest::Approx(5.0));
    CHECK(c.judgeBlow(kLeadB, 7.0).outcome == CountOutcome::Held);
    CHECK(c.judgeBlow(kLeadA, 6.0).outcome == CountOutcome::OnCount); // read as 7
}

TEST_CASE("count: enemy and bad strikers are never held or boosted, and take no blow id") {
    StrikeCount c = count60();
    REQUIRE(c.call(kLeadA, 0.0) == CountError::None);
    const CountHit e = c.judgeBlow(kEnemyFighter, 2.0);
    CHECK(e.outcome == CountOutcome::Alone);
    CHECK(e.blow == 0);
    CHECK(c.judgeBlow(kNoFighter, 2.0).outcome == CountOutcome::Alone);
    CHECK(c.heldBlows() == 0);
    CHECK(c.judgeBlow(kLeadB, 2.0).outcome == CountOutcome::Held);
    CHECK_FALSE(c.joined());
}

TEST_CASE("count: past kMaxHeldBlows lone blows settle at once; a partner still joins the held ones") {
    StrikeCount c = count60();
    REQUIRE(c.call(kLeadA, 0.0) == CountError::None);
    c.update(0.0);
    for (std::size_t i = 0; i < kMaxHeldBlows; ++i) CHECK(c.judgeBlow(kLeadA, 2.0).outcome == CountOutcome::Held);
    const CountHit extra = c.judgeBlow(kLeadA, 2.0);
    CHECK(extra.outcome == CountOutcome::Unanswered);
    CHECK(extra.multiplier == doctest::Approx(0.5f));
    CHECK(c.judgeBlow(kLeadB, 2.1).outcome == CountOutcome::OnCount);
    CountEvent s[kMaxHeldBlows + 2];
    CHECK(settled(c.update(2.1), s, static_cast<int>(kMaxHeldBlows) + 2) == static_cast<int>(kMaxHeldBlows));
    CHECK(s[0].outcome == CountOutcome::OnCount);
}

TEST_CASE("count: blow ids skip 0 when they wrap") {
    StrikeCount c = count60();
    std::uint16_t lastId = 0;
    bool sawZero = false;
    for (int i = 0; i < 70000; ++i) {
        lastId = c.judgeBlow(kLeadA, 0.0).blow;
        sawZero = sawZero || lastId == 0;
    }
    CHECK_FALSE(sawZero);
    CHECK(lastId == static_cast<std::uint16_t>(70000 - 65535));
}

TEST_CASE("count: a different tempo and lead") {
    CountTuning t;
    t.bpm = 120.0;
    t.leadBeats = 3;
    StrikeCount c;
    REQUIRE(c.reset(t));
    REQUIRE(c.call(kLeadB, 0.0) == CountError::None);
    CHECK(c.nowAt() == doctest::Approx(1.5));
    const CountEvents& ev = c.update(5.0);
    REQUIRE(ev.size() == 6);
    CHECK(ev[3].isNow == false);
    CHECK(ev[3].type == CountEventType::WindowOpened);
    CHECK(ev[4].index == 3);
    CHECK(ev[4].isNow);
}

TEST_CASE("count: a long fight of counts allocates nothing and is deterministic") {
    auto run = [](std::uint64_t* allocs) {
        StrikeCount c = count60();
        int boosted = 0;
        std::size_t events = 0;
        std::size_t dropped = 0;
        const std::uint64_t before = ftcore_test::allocStats().count;
        for (int frame = 0; frame < 6000; ++frame) {
            const double now = frame / 60.0;
            if (frame % 300 == 0) c.call(static_cast<FighterId>((frame / 300) % 3), now);
            if (frame % 900 == 450) c.cancel(now);
            if (frame % 420 == 7) c.falseCall(now, (frame % 2) ? 0.3 : -0.3);
            if (frame % 3 == 0)
                boosted += c.judgeBlow(static_cast<FighterId>((frame / 3) % 3), now).outcome == CountOutcome::OnCount;
            const CountEvents& ev = c.update(now);
            for (const CountEvent& e : ev)
                boosted += e.type == CountEventType::BlowSettled && e.outcome == CountOutcome::OnCount;
            events += ev.size();
            dropped += ev.dropped;
        }
        *allocs = ftcore_test::allocStats().count - before;
        CHECK(dropped == 0);
        return boosted * 100000 + static_cast<int>(events);
    };
    std::uint64_t a1 = 0, a2 = 0;
    const int r1 = run(&a1);
    const int r2 = run(&a2);
    CHECK(r1 == r2);
    CHECK(r1 > 100000);
    REQUIRE(ftcore_test::allocHookInstalled());
    CHECK(a1 == 0);
    CHECK(a2 == 0);
}
