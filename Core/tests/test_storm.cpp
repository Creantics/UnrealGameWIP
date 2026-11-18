// The storm's sky and lightning (ftcore/Storm.h).

#include "ftcore/Storm.h"

#include <ostream>
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

using namespace ftcore;

namespace {

struct Run {
    std::vector<StormStrike> strikes;
    std::vector<StormStrike> thunders;
    int inFlight = 0;
    int dropped = 0;
};

// The storm stepped at dt for seconds at a fixed rain, optionally held over [holdFrom, holdTo).
Run runStorm(std::uint32_t seed, float dt, double seconds, float rain, double holdFrom = -1.0, double holdTo = -1.0) {
    const StormSpec spec;
    StormState s = stormStart(seed);
    Run r;
    StormEvents ev;
    double wall = 0.0;
    while (wall < seconds) {
        const bool held = wall >= holdFrom && wall < holdTo;
        stormStep(spec, s, dt, rain, held, ev);
        for (int i = 0; i < ev.strikeCount; ++i) r.strikes.push_back(ev.strikes[i]);
        for (int i = 0; i < ev.thunderCount; ++i) r.thunders.push_back(ev.thunders[i]);
        wall += dt;
    }
    r.inFlight = s.thunderCount;
    r.dropped = s.dropped;
    return r;
}

// The most strike times (heads only when headsOnly) in any 1 s window.
int mostInOneSecond(const std::vector<StormStrike>& k, bool headsOnly) {
    int most = 0;
    for (std::size_t i = 0; i < k.size(); ++i) {
        if (headsOnly && !k[i].head) continue;
        int n = 0;
        for (std::size_t j = i; j < k.size() && k[j].atS < k[i].atS + 1.0; ++j)
            if (!headsOnly || k[j].head) ++n;
        most = n > most ? n : most;
    }
    return most;
}

} // namespace

TEST_CASE("stormSky: the overcast closes with the rain; the night's stars and moon go behind it") {
    const StormSpec spec;
    const StormSky clear = stormSky(spec, 0.f);
    CHECK(clear.cover == doctest::Approx(0.f));
    CHECK(clear.sunScale == doctest::Approx(1.f));
    CHECK(clear.starAlpha == doctest::Approx(1.f));
    CHECK(clear.moonAlpha == doctest::Approx(1.f));
    CHECK(clear.windMps == doctest::Approx(spec.windCalmMps));
    const StormSky full = stormSky(spec, 1.f);
    CHECK(full.cover == doctest::Approx(1.f));
    CHECK(full.sunScale == doctest::Approx(spec.sunAtOvercast));
    CHECK(full.skyScale == doctest::Approx(spec.skyAtOvercast));
    CHECK(full.starAlpha == doctest::Approx(0.f));
    CHECK(full.moonAlpha == doctest::Approx(0.f));
    CHECK(full.windMps == doctest::Approx(spec.windStormMps));
    // Monotonic between, the stars going before the moon.
    float last = 0.f;
    for (int i = 0; i <= 20; ++i) {
        const StormSky k = stormSky(spec, i / 20.f);
        CHECK(k.cover >= last);
        CHECK(k.starAlpha <= k.moonAlpha + 1e-6f);
        last = k.cover;
    }
    CHECK(stormSky(spec, std::nanf("")).cover == doctest::Approx(0.f));
}

TEST_CASE("stormStep: one seed, the same strikes at 30 and 120 frames a second") {
    const Run a = runStorm(7u, 1.f / 30.f, 600.0, 1.f);
    const Run b = runStorm(7u, 1.f / 120.f, 600.0, 1.f);
    REQUIRE(a.strikes.size() > 20); // a mean gap of 12 s and clusters over 10 minutes
    REQUIRE(a.strikes.size() == b.strikes.size());
    for (std::size_t i = 0; i < a.strikes.size(); ++i) {
        CHECK(a.strikes[i].atS == b.strikes[i].atS);
        CHECK(a.strikes[i].distanceM == b.strikes[i].distanceM);
        CHECK(a.strikes[i].head == b.strikes[i].head);
    }
    // Another seed, other strikes.
    const Run c = runStorm(8u, 1.f / 30.f, 600.0, 1.f);
    CHECK((c.strikes.size() != a.strikes.size() || c.strikes[0].atS != a.strikes[0].atS));
}

TEST_CASE("stormStep: gaps, clusters, distances and thunder") {
    const StormSpec spec;
    const Run r = runStorm(11u, 1.f / 60.f, 3600.0, 1.f);
    REQUIRE(r.strikes.size() > 100);
    std::size_t heads = 0, members = 0;
    double headSum = 0.0, lastHead = -1.0;
    int headGaps = 0;
    for (std::size_t i = 0; i < r.strikes.size(); ++i) {
        const StormStrike& k = r.strikes[i];
        CHECK(k.distanceM >= spec.minDistM * 0.5f);
        CHECK(k.distanceM <= spec.maxDistM * 1.1f);
        CHECK(k.thunderAtS == doctest::Approx(k.atS + k.distanceM / kThunderMps));
        CHECK(k.tier == thunderTier(spec, k.distanceM));
        if (i > 0) {
            const double gap = k.atS - r.strikes[i - 1].atS;
            CHECK(gap >= (k.head ? spec.headGapMinS : spec.clusterGapMinS) - 1e-9);
        }
        if (k.head) {
            ++heads;
            if (lastHead >= 0.0) {
                headSum += k.atS - lastHead;
                ++headGaps;
            }
            lastHead = k.atS;
        } else {
            ++members;
        }
    }
    CHECK(members > 0);
    // The mean head gap at full rain: the mean gap plus the cluster's span, near 12-16 s.
    const double mean = headSum / headGaps;
    CHECK(mean > 10.0);
    CHECK(mean < 20.0);
    // Every strike's thunder arrives (or is still in flight at the end), in time order, none dropped at this rate.
    CHECK(r.dropped == 0);
    CHECK(r.thunders.size() + static_cast<std::size_t>(r.inFlight) == r.strikes.size());
    for (std::size_t i = 1; i < r.thunders.size(); ++i) CHECK(r.thunders[i].thunderAtS >= r.thunders[i - 1].thunderAtS);
    CHECK(thunderTier(spec, 500.f) == ThunderTier::Near);
    CHECK(thunderTier(spec, 2000.f) == ThunderTier::Mid);
    CHECK(thunderTier(spec, 5000.f) == ThunderTier::Far);
}

TEST_CASE("stormStep: light rain strikes nothing; the strike drawn lapses") {
    const Run r = runStorm(3u, 1.f / 30.f, 600.0, 0.3f);
    CHECK(r.strikes.empty());
    CHECK(r.thunders.empty());
}

TEST_CASE("flashes: at most 3 in any second, 1 swell with Reduce flashes") {
    const Run r = runStorm(5u, 1.f / 60.f, 3600.0, 1.f);
    CHECK(mostInOneSecond(r.strikes, false) <= 3);
    CHECK(mostInOneSecond(r.strikes, true) <= 1);
    // The flash envelope: up on the strike, gone after its fall; the reduced swell is slower and dimmer.
    const StormSpec spec;
    StormState s = stormStart(1u);
    s.lastFlashS = s.lastHeadS = 10.0;
    s.lastFlash = s.lastHead = 1.f;
    s.clockS = 10.03;
    CHECK(stormFlash(spec, s, false) == doctest::Approx(1.f));
    CHECK(stormFlash(spec, s, false, true) == doctest::Approx(0.f));
    s.clockS = 10.0 + 0.03 + spec.flashS + 0.01;
    CHECK(stormFlash(spec, s, false) == doctest::Approx(0.f));
    s.clockS = 10.0 + spec.reducedRiseS;
    CHECK(stormFlash(spec, s, true) == doctest::Approx(spec.reducedScale));
    CHECK(stormFlash(spec, s, true) < 0.5f);
}

TEST_CASE("hold: no strike or thunder while held; after it, the same storm, late, with no burst") {
    // Held for 60 s from wall second 100: the held run is the free run 60 s later, strike for strike.
    const Run free = runStorm(9u, 1.f / 30.f, 400.0, 1.f);
    const Run held = runStorm(9u, 1.f / 30.f, 461.0, 1.f, 100.0, 160.0);
    REQUIRE(held.strikes.size() >= free.strikes.size());
    for (std::size_t i = 0; i < free.strikes.size(); ++i) CHECK(held.strikes[i].atS == free.strikes[i].atS);
    // Storm time stood still: nothing is stamped inside the hold, and the step after it reports no pile.
    StormState s = stormStart(9u);
    StormEvents ev;
    const StormSpec spec;
    for (int i = 0; i < 3000; ++i) stormStep(spec, s, 1.f / 30.f, 1.f, false, ev);
    const double before = s.clockS;
    for (int i = 0; i < 1800; ++i) {
        stormStep(spec, s, 1.f / 30.f, 1.f, true, ev);
        CHECK(ev.strikeCount == 0);
        CHECK(ev.thunderCount == 0);
    }
    CHECK(s.clockS == before);
    CHECK(stormFlash(spec, s, false, true) == doctest::Approx(0.f));
    stormStep(spec, s, 1.f / 30.f, 1.f, false, ev);
    CHECK(ev.strikeCount <= 1);
}

TEST_CASE("stormStep: bad steps move nothing; a full thunder queue drops the farthest") {
    const StormSpec spec;
    StormState s = stormStart(0u);
    CHECK(s.rng != 0u);
    StormEvents ev;
    stormStep(spec, s, std::nanf(""), 1.f, false, ev);
    stormStep(spec, s, -1.f, 1.f, false, ev);
    CHECK(s.clockS == 0.0);
    stormStep(spec, s, 5.f, 1.f, false, ev); // clamped
    CHECK(s.clockS == doctest::Approx(kStormMaxStepS));
    // A full queue of far thunders and a new strike: the near one is kept, one far one dropped.
    StormState q = stormStart(1u);
    for (int i = 0; i < kStormMaxThunder; ++i) {
        q.thunder[i].distanceM = 6000.f + static_cast<float>(i);
        q.thunder[i].thunderAtS = 1000.0;
    }
    q.thunderCount = kStormMaxThunder;
    q.nextS = 0.1;
    stormStep(spec, q, 0.2f, 1.f, false, ev);
    REQUIRE(ev.strikeCount == 1);
    CHECK(q.dropped == 1);
    CHECK(q.thunderCount == kStormMaxThunder);
    bool kept = false;
    for (int i = 0; i < q.thunderCount; ++i) kept = kept || q.thunder[i].atS == doctest::Approx(0.1);
    CHECK(kept == (ev.strikes[0].distanceM < 6000.f + static_cast<float>(kStormMaxThunder - 1)));
}
