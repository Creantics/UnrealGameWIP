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
    CountTuning zeroBoosts = tempo60();
    zeroBoosts.perFighterBoosts = 0;
    CHECK_FALSE(validCountTuning(zeroBoosts));
    CHECK(validCountTuning(CountTuning{}));
}

TEST_CASE("count: a partner's blow on the count hits far harder; the caller's own and a lone blow do not") {
    StrikeCount c = count60();
    REQUIRE(c.call(kLeadA, 10.0) == CountError::None);
    CHECK(c.live());
    CHECK(c.nowAt() == doctest::Approx(12.0));

    const CountHit own = c.judgeBlow(kLeadA, 12.0);
    CHECK(own.outcome == CountOutcome::CallersOwn);
    CHECK(own.multiplier == doctest::Approx(1.0f));
    CHECK(own.caller == kLeadA);

    const CountHit partner = c.judgeBlow(kLeadB, 12.05);
    CHECK(partner.outcome == CountOutcome::OnCount);
    CHECK(partner.multiplier == doctest::Approx(2.5f));
    CHECK_FALSE(partner.punishable);

    // The same fighter's second blow in the window is plain.
    CHECK(c.judgeBlow(kLeadB, 12.1).outcome == CountOutcome::Spent);
    // The third slot is a partner too.
    CHECK(c.judgeBlow(kThirdSlot, 12.15).outcome == CountOutcome::OnCount);

    // Outside the window: alone.
    StrikeCount d = count60();
    REQUIRE(d.call(kLeadA, 0.0) == CountError::None);
    CHECK(d.judgeBlow(kLeadB, 1.5).outcome == CountOutcome::Alone);
    CHECK(d.judgeBlow(kLeadB, 2.5).outcome == CountOutcome::Alone);
}

TEST_CASE("count: no count at all is a lone blow") {
    StrikeCount c = count60();
    const CountHit h = c.judgeBlow(kLeadB, 5.0);
    CHECK(h.outcome == CountOutcome::Alone);
    CHECK(h.multiplier == doctest::Approx(1.0f));
    CHECK(h.caller == kNoFighter);
}

TEST_CASE("count: the window's edges are inclusive") {
    StrikeCount c = count60();
    REQUIRE(c.call(kLeadA, 0.0) == CountError::None);
    CHECK(c.windowStart() == doctest::Approx(1.88));
    CHECK(c.windowEnd() == doctest::Approx(2.18));
    CHECK(c.judgeBlow(kLeadB, c.windowStart()).outcome == CountOutcome::OnCount);
    CHECK(c.judgeBlow(kThirdSlot, c.windowEnd()).outcome == CountOutcome::OnCount);

    StrikeCount e = count60();
    REQUIRE(e.call(kLeadA, 0.0) == CountError::None);
    CHECK(e.judgeBlow(kLeadB, std::nextafter(e.windowStart(), 0.0)).outcome == CountOutcome::Alone);
    StrikeCount f = count60();
    REQUIRE(f.call(kLeadA, 0.0) == CountError::None);
    CHECK(f.judgeBlow(kLeadB, std::nextafter(f.windowEnd(), 10.0)).outcome == CountOutcome::Alone);
}

TEST_CASE("count: more boosts per fighter when tuned") {
    CountTuning t = tempo60();
    t.perFighterBoosts = 2;
    StrikeCount c;
    REQUIRE(c.reset(t));
    REQUIRE(c.call(kThirdSlot, 0.0) == CountError::None);
    CHECK(c.judgeBlow(kLeadA, 1.95).outcome == CountOutcome::OnCount);
    CHECK(c.judgeBlow(kLeadA, 2.0).outcome == CountOutcome::OnCount);
    CHECK(c.judgeBlow(kLeadA, 2.05).outcome == CountOutcome::Spent);
    CHECK(c.judgeBlow(kThirdSlot, 2.1).outcome == CountOutcome::CallersOwn);
}

TEST_CASE("count: calls are refused while live, while cooling, and for the enemy or a bad id") {
    StrikeCount c = count60();
    CHECK(c.call(kEnemyFighter, 0.0) == CountError::BadCaller);
    CHECK(c.call(7, 0.0) == CountError::BadCaller);
    REQUIRE(c.call(kLeadA, 0.0) == CountError::None);
    CHECK(c.call(kLeadB, 1.0) == CountError::Live);
    CHECK(c.readyAt() == doctest::Approx(2.18 + 1.5));
    CHECK(c.call(kLeadB, 3.0) == CountError::Cooling);
    CHECK(c.call(kLeadB, 3.7) == CountError::None);
    CHECK(c.caller() == kLeadB);
    // The new count's boosts start fresh.
    CHECK(c.judgeBlow(kLeadA, 3.7 + 2.0).outcome == CountOutcome::OnCount);
}

TEST_CASE("count: a silenced count has no caller, boosts anyone, but less and on a narrower window") {
    StrikeCount c = count60();
    REQUIRE(c.call(kNoFighter, 0.0, CountVoice::Voice) == CountError::None);
    CHECK(c.silenced());
    // Its cues are clicks whatever voice was asked for.
    const CountEvents& ev = c.update(0.0);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].voice == CountVoice::Click);
    CHECK(ev[0].caller == kNoFighter);
    CHECK(c.windowStart() == doctest::Approx(2.0 - 0.12 * 0.6));
    CHECK(c.windowEnd() == doctest::Approx(2.0 + 0.18 * 0.6));
    // Outside the narrowed window but inside the full one: alone.
    CHECK(c.judgeBlow(kLeadA, 1.9).outcome == CountOutcome::Alone);
    const CountHit h = c.judgeBlow(kLeadA, 2.0);
    CHECK(h.outcome == CountOutcome::OnCount);
    CHECK(h.multiplier == doctest::Approx(1.6f));
    CHECK(c.judgeBlow(kLeadB, 2.05).outcome == CountOutcome::OnCount);
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

TEST_CASE("count: a false count early or late, flagged, punishable, never boosting") {
    for (const double shift : {-0.4, 0.4}) {
        CAPTURE(shift);
        StrikeCount c = count60();
        c.falseCall(0.0, shift);
        CHECK(c.falseLive());
        CHECK(c.falseNowAt() == doctest::Approx(2.0 + shift));
        const CountHit h = c.judgeBlow(kLeadA, 2.0 + shift);
        CHECK(h.outcome == CountOutcome::FalseCount);
        CHECK(h.multiplier == doctest::Approx(0.75f));
        CHECK(h.punishable);
        // On the true tempo's "now" (outside the false window) the blow is simply alone.
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
}

TEST_CASE("count: a false count shifted too early stays after its own second cue; NaN shift is on time") {
    StrikeCount c = count60();
    c.falseCall(0.0, -5.0);
    CHECK(c.falseNowAt() == doctest::Approx(1.0));
    c.falseCall(0.5, std::numeric_limits<double>::quiet_NaN());
    CHECK(c.falseNowAt() == doctest::Approx(2.5));
}

TEST_CASE("count: a true window wins over an overlapping false one, and a new false count replaces the old") {
    StrikeCount c = count60();
    REQUIRE(c.call(kLeadA, 0.0) == CountError::None);
    c.falseCall(0.0, 0.1);
    CHECK(c.judgeBlow(kLeadB, 2.1).outcome == CountOutcome::OnCount);
    // The caller's own blow in the overlap is still judged by the true count.
    CHECK(c.judgeBlow(kLeadA, 2.15).outcome == CountOutcome::CallersOwn);
    // After the true window, inside the false one: false.
    CHECK(c.judgeBlow(kThirdSlot, 2.2).outcome == CountOutcome::FalseCount);

    StrikeCount d = count60();
    d.falseCall(0.0, 0.3);
    CHECK(d.update(0.0).size() == 1);
    d.falseCall(0.5, -0.3);
    CHECK(d.falseNowAt() == doctest::Approx(2.2));
    const CountEvents& ev = d.update(10.0);
    REQUIRE(ev.size() == 3); // the new one's three cues; the old one's two left are dropped
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

TEST_CASE("count: cancel drops the window and its cues; the cooldown runs from the cancel") {
    StrikeCount c = count60();
    REQUIRE(c.call(kLeadA, 0.0) == CountError::None);
    CHECK(c.update(0.0).size() == 1);
    c.cancel(1.5);
    CHECK_FALSE(c.live());
    CHECK(c.readyAt() == doctest::Approx(3.0));
    CHECK(c.call(kLeadB, 2.9) == CountError::Cooling);
    CHECK(c.judgeBlow(kLeadB, 2.0).outcome == CountOutcome::Alone); // read as 2.9: the window is gone
    CHECK(c.update(2.95).empty());
    CHECK(c.call(kLeadB, 3.0) == CountError::None);

    StrikeCount d = count60();
    d.falseCall(0.0, 0.0);
    d.cancelFalse(0.5);
    CHECK_FALSE(d.falseLive());
    CHECK(d.judgeBlow(kLeadA, 2.0).outcome == CountOutcome::Alone);
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
    CHECK(c.judgeBlow(kLeadB, 7.0).outcome == CountOutcome::OnCount);
}

TEST_CASE("count: enemy and bad strikers are never boosted") {
    StrikeCount c = count60();
    REQUIRE(c.call(kLeadA, 0.0) == CountError::None);
    CHECK(c.judgeBlow(kEnemyFighter, 2.0).outcome == CountOutcome::Alone);
    CHECK(c.judgeBlow(kNoFighter, 2.0).outcome == CountOutcome::Alone);
    CHECK(c.judgeBlow(kLeadB, 2.0).outcome == CountOutcome::OnCount);
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
        const std::uint64_t before = ftcore_test::allocStats().count;
        for (int frame = 0; frame < 6000; ++frame) {
            const double now = frame / 60.0;
            if (frame % 300 == 0) c.call(static_cast<FighterId>((frame / 300) % 3), now);
            if (frame % 420 == 7) c.falseCall(now, (frame % 2) ? 0.3 : -0.3);
            if (frame % 7 == 0)
                boosted += c.judgeBlow(static_cast<FighterId>(frame % 3), now).outcome == CountOutcome::OnCount;
            events += c.update(now).size();
        }
        *allocs = ftcore_test::allocStats().count - before;
        return boosted * 100000 + static_cast<int>(events);
    };
    std::uint64_t a1 = 0, a2 = 0;
    const int r1 = run(&a1);
    const int r2 = run(&a2);
    CHECK(r1 == r2);
    CHECK(r1 > 100000);
    if (ftcore_test::allocHookInstalled()) {
        CHECK(a1 == 0);
        CHECK(a2 == 0);
    }
}
