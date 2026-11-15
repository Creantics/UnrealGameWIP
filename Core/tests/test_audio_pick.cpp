#include "ftcore/AudioPick.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

using namespace ftcore;

namespace {

constexpr int kPlays = 10000;

AudioPickRule ruleOf(int n) {
    AudioPickRule r;
    r.variants = n;
    return r;
}

} // namespace

TEST_CASE("audioPick: never the same variant twice in a row, never one of the last two from four up") {
    for (int n = 1; n <= kAudioPickMaxVariants; ++n) {
        for (const bool rr : {false, true}) {
            AudioPickRule rule = ruleOf(n);
            rule.roundRobin = rr;
            AudioPickState st;
            audioPickSeed(st, 7u, "SFX-STEP");
            int prev1 = -1, prev2 = -1;
            int bad = 0;
            for (int i = 0; i < kPlays; ++i) {
                const int v = audioPick(rule, st).variant;
                if (v < 0 || v >= n) ++bad;
                if (n >= 2 && v == prev1) ++bad;
                if (n >= 4 && v == prev2) ++bad;
                prev2 = prev1;
                prev1 = v;
            }
            CAPTURE(n);
            CAPTURE(rr);
            CHECK(bad == 0);
        }
    }
}

TEST_CASE("audioPick: avoidLast raises the rule, never lowers it") {
    AudioPickRule rule = ruleOf(6);
    rule.avoidLast = 4;
    CHECK(audioPickAvoid(rule) == 4);
    rule.avoidLast = 0;
    CHECK(audioPickAvoid(rule) == 2);
    rule.variants = 3;
    rule.avoidLast = 4;
    CHECK(audioPickAvoid(rule) == 2); // at most N - 1
    rule.variants = 1;
    CHECK(audioPickAvoid(rule) == 0);

    rule = ruleOf(6);
    rule.avoidLast = 4;
    AudioPickState st;
    audioPickSeed(st, 3u, "SFX-HIT");
    int hist[4] = {-1, -1, -1, -1};
    int bad = 0;
    for (int i = 0; i < kPlays; ++i) {
        const int v = audioPick(rule, st).variant;
        for (const int h : hist) bad += (h == v);
        for (int k = 3; k > 0; --k) hist[k] = hist[k - 1];
        hist[0] = v;
    }
    CHECK(bad == 0);
}

TEST_CASE("audioPick: every variant is used about equally (shuffle bag)") {
    for (const int n : {2, 3, 4, 5, 7, 16}) {
        AudioPickRule rule = ruleOf(n);
        AudioPickState st;
        audioPickSeed(st, 11u, "SFX-IMPACT");
        int counts[kAudioPickMaxVariants] = {};
        for (int i = 0; i < kPlays; ++i) ++counts[audioPick(rule, st).variant];
        const double expect = static_cast<double>(kPlays) / n;
        for (int v = 0; v < n; ++v) {
            CAPTURE(n);
            CAPTURE(v);
            CHECK(std::fabs(counts[v] - expect) <= 1.0 + 0.01 * expect); // a bag uses each once per round
        }
    }
}

TEST_CASE("audioPick: shuffled, not a fixed cycle") {
    AudioPickRule rule = ruleOf(8);
    AudioPickState st;
    audioPickSeed(st, 5u, "SFX-STEP");
    int seq[64];
    for (int& v : seq) v = audioPick(rule, st).variant;
    int sameAsCycle = 0;
    for (int i = 8; i < 64; ++i) sameAsCycle += (seq[i] == seq[i - 8]);
    CHECK(sameAsCycle < 40);
}

TEST_CASE("audioPick: round robin plays the variants in order from a seeded start") {
    AudioPickRule rule = ruleOf(5);
    rule.roundRobin = true;
    AudioPickState st;
    audioPickSeed(st, 1u, "SFX-UI-TICK");
    const int start = audioPick(rule, st).variant;
    for (int i = 1; i < 20; ++i) CHECK(audioPick(rule, st).variant == (start + i) % 5);

    // Different cues start at different places, so they don't run in lockstep.
    int starts[5] = {};
    const char* ids[] = {"SFX-A", "SFX-B", "SFX-C", "SFX-D", "SFX-E", "SFX-F", "SFX-G", "SFX-H", "SFX-I", "SFX-J",
                         "SFX-K", "SFX-L", "SFX-M", "SFX-N", "SFX-O", "SFX-P"};
    for (const char* id : ids) {
        AudioPickState s;
        audioPickSeed(s, 1u, id);
        ++starts[audioPick(rule, s).variant];
    }
    int used = 0;
    for (const int c : starts) used += (c > 0);
    CHECK(used >= 3);
}

TEST_CASE("audioPick: avoidLast over many plays at 2 and 3 variants") {
    // Two variants: strict alternation.
    AudioPickState two;
    audioPickSeed(two, 6u, "SFX-TWO");
    int prev = audioPick(ruleOf(2), two).variant;
    for (int i = 0; i < kPlays; ++i) {
        const int v = audioPick(ruleOf(2), two).variant;
        CHECK(v == 1 - prev);
        prev = v;
    }
    // Three variants avoiding two, and avoidLast raised past N - 1 (clamped to 2): a period-3 cycle.
    for (const int avoid : {2, 4}) {
        AudioPickRule rule = ruleOf(3);
        rule.avoidLast = avoid;
        CHECK(audioPickAvoid(rule) == 2);
        AudioPickState st;
        audioPickSeed(st, 9u, "SFX-THREE");
        int seq[3] = {audioPick(rule, st).variant, audioPick(rule, st).variant, audioPick(rule, st).variant};
        CHECK(seq[0] != seq[1]);
        CHECK(seq[1] != seq[2]);
        CHECK(seq[0] != seq[2]);
        int bad = 0;
        for (int i = 3; i < kPlays; ++i) {
            const int v = audioPick(rule, st).variant;
            bad += (v != seq[i % 3]);
        }
        CAPTURE(avoid);
        CHECK(bad == 0);
    }
}

TEST_CASE("audioPick: the table index separates two cues whose ids hash alike") {
    // The same id stands in for a hash collision: equal keys, different table indices.
    const AudioPickRule rule = ruleOf(6);
    AudioPickState a, b, a2;
    audioPickSeed(a, 3u, "SFX-STEP", 4u);
    audioPickSeed(b, 3u, "SFX-STEP", 5u);
    audioPickSeed(a2, 3u, "SFX-STEP", 4u);
    CHECK(a.rng != b.rng);
    int differ = 0, same = 0;
    for (int i = 0; i < 500; ++i) {
        const int va = audioPick(rule, a).variant;
        differ += (va != audioPick(rule, b).variant);
        same += (va == audioPick(rule, a2).variant);
    }
    CHECK(differ > 100);
    CHECK(same == 500);
}

TEST_CASE("audioPick: the same seed replays the same sequence") {
    AudioPickRule rule = ruleOf(6);
    rule.pitchCents = 40.f;
    rule.volumeDb = 2.f;
    AudioPickState a, b, c;
    audioPickSeed(a, 99u, "SFX-STEP");
    audioPickSeed(b, 99u, "SFX-STEP");
    audioPickSeed(c, 100u, "SFX-STEP");
    int differ = 0;
    for (int i = 0; i < 1000; ++i) {
        const AudioPickResult ra = audioPick(rule, a);
        const AudioPickResult rb = audioPick(rule, b);
        const AudioPickResult rc = audioPick(rule, c);
        CHECK(ra.variant == rb.variant);
        CHECK(ra.pitchCents == rb.pitchCents);
        CHECK(ra.volumeDb == rb.volumeDb);
        differ += (ra.variant != rc.variant);
    }
    CHECK(differ > 100);
    // A copied state continues the same way.
    AudioPickState copy = a;
    for (int i = 0; i < 50; ++i) CHECK(audioPick(rule, copy).variant == audioPick(rule, a).variant);
}

TEST_CASE("audioPick: one cue's sequence doesn't depend on other cues' plays") {
    const AudioPickRule rule = ruleOf(5);
    AudioPickState alone, mixed, other;
    audioPickSeed(alone, 42u, "SFX-STEP");
    audioPickSeed(mixed, 42u, "SFX-STEP");
    audioPickSeed(other, 42u, "SFX-HIT");
    const AudioPickRule otherRule = ruleOf(9);
    int same = 0;
    for (int i = 0; i < 2000; ++i) {
        for (int k = 0; k < i % 4; ++k) audioPick(otherRule, other);
        same += (audioPick(rule, alone).variant == audioPick(rule, mixed).variant);
    }
    CHECK(same == 2000);
    // Two cues under one game seed get different sequences.
    AudioPickState x, y;
    audioPickSeed(x, 42u, "SFX-STEP");
    audioPickSeed(y, 42u, "SFX-HIT");
    int differ = 0;
    for (int i = 0; i < 500; ++i) differ += (audioPick(rule, x).variant != audioPick(rule, y).variant);
    CHECK(differ > 100);
}

TEST_CASE("audioPick: jitter stays in range, is 0 by default and never shifts the variant order") {
    AudioPickRule plain = ruleOf(6);
    AudioPickRule jit = plain;
    jit.pitchCents = 35.f;
    jit.volumeDb = 1.5f;
    AudioPickState a, b;
    audioPickSeed(a, 8u, "SFX-STEP");
    audioPickSeed(b, 8u, "SFX-STEP");
    float pMin = 1e9f, pMax = -1e9f, vMin = 1e9f, vMax = -1e9f;
    for (int i = 0; i < kPlays; ++i) {
        const AudioPickResult ra = audioPick(plain, a);
        const AudioPickResult rb = audioPick(jit, b);
        CHECK(ra.pitchCents == 0.f);
        CHECK(ra.volumeDb == 0.f);
        CHECK(ra.pitchRatio() == 1.f);
        CHECK(ra.variant == rb.variant);
        pMin = std::fmin(pMin, rb.pitchCents);
        pMax = std::fmax(pMax, rb.pitchCents);
        vMin = std::fmin(vMin, rb.volumeDb);
        vMax = std::fmax(vMax, rb.volumeDb);
    }
    CHECK(pMin >= -35.f);
    CHECK(pMax <= 35.f);
    CHECK(vMin >= -1.5f);
    CHECK(vMax <= 1.5f);
    CHECK(pMin < -30.f); // the range is used, both ways
    CHECK(pMax > 30.f);
    CHECK(vMin < -1.3f);
    CHECK(vMax > 1.3f);

    AudioPickResult r;
    r.pitchCents = 1200.f;
    CHECK(r.pitchRatio() == doctest::Approx(2.0));
    r.volumeDb = -6.0206f;
    CHECK(r.volumeLinear() == doctest::Approx(0.5).epsilon(1e-4));
}

TEST_CASE("audioPick: edge cases: one variant, out-of-range counts, a reload with fewer variants, a zero state") {
    AudioPickState st;
    audioPickSeed(st, 1u, "SFX-ONE");
    for (int i = 0; i < 10; ++i) CHECK(audioPick(ruleOf(1), st).variant == 0);
    CHECK(audioPick(ruleOf(0), st).variant == 0);
    CHECK(audioPick(ruleOf(-3), st).variant == 0);
    AudioPickState big;
    for (int i = 0; i < 100; ++i) CHECK(audioPick(ruleOf(40), big).variant < kAudioPickMaxVariants);

    AudioPickState reload;
    audioPickSeed(reload, 2u, "SFX-STEP");
    for (int i = 0; i < 7; ++i) audioPick(ruleOf(8), reload);
    int prev = -1;
    for (int i = 0; i < 100; ++i) {
        const int v = audioPick(ruleOf(3), reload).variant;
        CHECK(v < 3);
        CHECK(v != prev);
        prev = v;
    }

    AudioPickState zero;
    zero.rng = 0;
    const int v = audioPick(ruleOf(4), zero).variant;
    CHECK(v >= 0);
    CHECK(zero.rng != 0);
}

TEST_CASE("audioPick: no allocation") {
    REQUIRE(ftcore_test::allocHookInstalled()); // AllocHook.cpp is linked into ftcore_tests
    AudioPickRule rule = ruleOf(7);
    rule.pitchCents = 20.f;
    AudioPickState st;
    const auto before = ftcore_test::allocStats().count;
    audioPickSeed(st, 4u, "SFX-STEP");
    int sum = 0;
    for (int i = 0; i < 1000; ++i) sum += audioPick(rule, st).variant;
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(sum > 0);
}
