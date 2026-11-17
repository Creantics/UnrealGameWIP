#include <doctest/doctest.h>
#include "ftcore/Weather.h"
#include "AllocCounter.h"

#include <cmath>
#include <initializer_list>
#include <limits>

using namespace ftcore;

namespace {

RainState run(const RainSpec& spec, RainState s, float seconds, float dt = 1.f / 60.f) {
    const int n = static_cast<int>(std::lround(seconds / dt));
    for (int i = 0; i < n; ++i) s = rainStep(spec, s, dt);
    return s;
}

}  // namespace

TEST_CASE("weather: the intensity ramps to its target in rampUpS and back in rampDownS") {
    const RainSpec spec;
    RainState s;
    s.target = 1.f;
    const float dt = 1.f / 60.f;
    s = run(spec, s, spec.rampUpS - 2.f * dt);
    CHECK(s.intensity < 1.f);
    s = run(spec, s, 3.f * dt);
    CHECK(s.intensity == doctest::Approx(1.f));
    s.target = 0.f;
    s = run(spec, s, spec.rampDownS - 2.f * dt);
    CHECK(s.intensity > 0.f);
    s = run(spec, s, 3.f * dt);
    CHECK(s.intensity == 0.f);
}

TEST_CASE("weather: no rain from a dry start stays dry") {
    const RainSpec spec;
    const RainState s = run(spec, RainState{}, 600.f);
    CHECK(s.intensity == 0.f);
    CHECK(s.wetness == 0.f);
    CHECK(s.puddle == 0.f);
    CHECK(rainBand(s.intensity) == 0);
    CHECK(rainStreakAlpha(spec, s.intensity) == 0.f);
}

TEST_CASE("weather: wetness rises while it rains, then dries; puddles lag on the way up and outlast it on the way down") {
    const RainSpec spec;
    RainState s;
    s.target = 1.f;
    const float dt = 1.f / 30.f;
    float lastWet = 0.f;
    bool rising = true, puddleBehind = true;
    for (int i = 0; i < 30 * 240; ++i) {
        s = rainStep(spec, s, dt);
        rising = rising && s.wetness >= lastWet;
        puddleBehind = puddleBehind && s.puddle <= s.wetness + 1e-6f;
        lastWet = s.wetness;
    }
    CHECK(rising);
    CHECK(puddleBehind);
    CHECK(s.wetness == doctest::Approx(1.f));
    CHECK(s.puddle > 0.5f);
    // Rain stops: the ground dries, never wetter than a step before; the standing water outlasts the soak.
    s.target = 0.f;
    bool drying = true, puddleOutlasts = false;
    for (int i = 0; i < 30 * 900; ++i) {
        s = rainStep(spec, s, dt);
        drying = drying && s.wetness <= lastWet;
        puddleOutlasts = puddleOutlasts || s.puddle > s.wetness;
        lastWet = s.wetness;
    }
    CHECK(drying);
    CHECK(puddleOutlasts);
    CHECK(s.wetness < 0.01f);
}

TEST_CASE("weather: light rain holds a partial soak; soakAtIntensity and heavier soak fully") {
    const RainSpec spec;
    CHECK(rainSoakTarget(spec, 0.f) == 0.f);
    CHECK(rainSoakTarget(spec, 0.25f) == doctest::Approx(0.5f));
    CHECK(rainSoakTarget(spec, spec.soakAtIntensity) == doctest::Approx(1.f));
    CHECK(rainSoakTarget(spec, 1.f) == 1.f);
    RainState s;
    s.target = 0.25f;
    s = run(spec, s, 900.f);
    CHECK(s.wetness == doctest::Approx(0.5f));
    // After a downpour a drizzle lets the street dry only down to the drizzle's own soak, no lower.
    RainState wet = rainSettled(spec, 1.f);
    wet.target = 0.25f;
    wet = run(spec, wet, 3000.f);
    CHECK(wet.wetness == doctest::Approx(0.5f));
}

TEST_CASE("weather: rainSettled starts at the settled state of its target") {
    const RainSpec spec;
    const RainState s = rainSettled(spec, 0.8f);
    CHECK(s.target == doctest::Approx(0.8f));
    CHECK(s.intensity == doctest::Approx(0.8f));
    CHECK(s.wetness == doctest::Approx(1.f));
    const RainState after = run(spec, s, 60.f);
    CHECK(after.intensity == doctest::Approx(s.intensity));
    CHECK(after.wetness == doctest::Approx(s.wetness));
    CHECK(after.puddle == doctest::Approx(s.puddle));
    CHECK(rainSettled(spec, 0.f).wetness == 0.f);
    CHECK(rainSettled(spec, 5.f).intensity == 1.f);
}

TEST_CASE("weather: dt of 0, negative or not finite is a no-op; a long dt is clamped to kRainMaxStepS") {
    const RainSpec spec;
    RainState s;
    s.target = 1.f;
    CHECK(rainStep(spec, s, 0.f).intensity == 0.f);
    CHECK(rainStep(spec, s, -1.f).intensity == 0.f);
    CHECK(rainStep(spec, s, std::numeric_limits<float>::quiet_NaN()).intensity == 0.f);
    CHECK(rainStep(spec, s, std::numeric_limits<float>::infinity()).intensity == 0.f);
    const RainState big = rainStep(spec, s, 100.f);
    const RainState capped = rainStep(spec, s, kRainMaxStepS);
    CHECK(big.intensity == doctest::Approx(capped.intensity));
    CHECK(big.intensity == doctest::Approx(kRainMaxStepS / spec.rampUpS));
    RainState odd;
    odd.target = 7.f;
    odd.wetness = -3.f;
    odd.puddle = std::numeric_limits<float>::quiet_NaN();
    const RainState fixed = rainStep(spec, odd, 0.f);
    CHECK(fixed.target == 1.f);
    CHECK(fixed.wetness == 0.f);
    CHECK(fixed.puddle == 0.f);
}

TEST_CASE("weather: the ground's Wetness in the dry state is today's look.wetness * wetShare") {
    const RainState dry;
    for (float look : {0.f, 0.3f, 0.65f, 1.f})
        for (float share : {0.f, 0.5f, 1.f}) CHECK(weatherGroundWetness(look, share, dry) == look * share);
    RainState rain;
    rain.wetness = 0.9f;
    CHECK(weatherGroundWetness(0.5f, 1.f, rain) == doctest::Approx(0.9f));
    rain.wetness = 0.2f;
    CHECK(weatherGroundWetness(0.5f, 1.f, rain) == doctest::Approx(0.5f));  // rain never dries the look's damp floor
}

TEST_CASE("weather: a lead's surface wetness is the rain's (by exposure) or its own water state, whichever is wetter") {
    RainState rain;
    rain.wetness = 0.7f;
    CHECK(leadSurfaceWetness(rain, 0.f) == doctest::Approx(0.7f));
    CHECK(leadSurfaceWetness(rain, 0.f, 0.f) == 0.f);       // under cover: the rain does not reach it
    CHECK(leadSurfaceWetness(rain, 0.9f) == doctest::Approx(0.9f));  // out of the canal: its own soak wins
    CHECK(leadSurfaceWetness(rain, 0.4f, 0.5f) == doctest::Approx(0.4f));
    CHECK(leadSurfaceWetness(RainState{}, 0.f) == 0.f);
    CHECK(leadSurfaceWetness(rain, 3.f, 2.f) == 1.f);       // clamped
    RainState bad;
    bad.wetness = std::numeric_limits<float>::quiet_NaN();
    CHECK(leadSurfaceWetness(bad, 0.2f) == doctest::Approx(0.2f));
}

TEST_CASE("weather: bands, streak opacity and mist scale") {
    const RainSpec spec;
    CHECK(rainBand(0.f) == 0);
    CHECK(rainBand(1e-4f) == 1);
    CHECK(rainBand(0.5f) == 2);
    CHECK(rainBand(1.f) == 3);
    int last = 0;
    bool monotone = true;
    for (int i = 0; i <= 100; ++i) {
        const int b = rainBand(static_cast<float>(i) / 100.f);
        monotone = monotone && b >= last;
        last = b;
    }
    CHECK(monotone);
    CHECK(rainStreakAlpha(spec, 1e-4f) == doctest::Approx(spec.streakAlphaMin).epsilon(0.01));
    CHECK(rainStreakAlpha(spec, 1.f) == doctest::Approx(1.f));
    CHECK(rainMistScale(spec, 0.f) == doctest::Approx(1.f));
    CHECK(rainMistScale(spec, 1.f) == doctest::Approx(spec.mistAtFull));
}

TEST_CASE("weather: stepping allocates nothing") {
    const RainSpec spec;
    RainState s;
    s.target = 1.f;
    const auto before = ftcore_test::allocStats().count;
    s = run(spec, s, 30.f);
    const float lead = leadSurfaceWetness(s, 0.1f, 1.f) + weatherGroundWetness(0.2f, 1.f, s);
    const auto after = ftcore_test::allocStats().count;
    CHECK(after == before);
    CHECK(lead > 0.f);
}
