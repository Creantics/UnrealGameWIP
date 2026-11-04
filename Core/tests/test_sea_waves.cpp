#include <doctest/doctest.h>

#include "AllocCounter.h"
#include "ftcore/SeaWaves.h"

#include <cmath>

using ftcore::SceneLook;
using ftcore::SeaWaves;

namespace {

constexpr float kPi = 3.14159265358979323846f;

// The harbour on a windy night: a 1.5 m swell at 8 s outside, an 8 m/s wind over 2 km of water.
SceneLook harbour() {
    SceneLook l;
    l.seaSwellM = 1.5f;
    l.seaSwellPeriodS = 8.f;
    l.seaSwellDeg = 200.f;
    l.windMps = 8.f;
    l.windDeg = 230.f;
    return l;
}

float lengthM(const ftcore::SeaWave& w) { return 2.f * kPi / w.k * 0.01f; }

} // namespace

TEST_CASE("A default look raises a flat sea") {
    const SeaWaves sea = ftcore::seaWaves(SceneLook{});
    for (const ftcore::SeaWave& w : sea.waves) {
        CHECK(w.amplitude == 0.f);
        // The material reads these as MPC_Env's defaults; a NaN there would poison the sea even at amplitude 0.
        float v[4];
        ftcore::packSeaWave(w, v);
        for (float c : v) CHECK(std::isfinite(c));
    }
    CHECK(sea.q == 0.f);
    CHECK(ftcore::seaHeight(sea, 1234.f, -567.f, 42.f) == 0.f);
    CHECK(ftcore::seaJacobian(sea, 0.f, 0.f, 0.f) == 1.f);
}

TEST_CASE("Every wave keeps deep-water dispersion and the swell its length within 2%") {
    SceneLook look = harbour();
    for (float period = 4.f; period <= 20.f; period += 0.5f) {
        look.seaSwellPeriodS = period;
        const SeaWaves sea = ftcore::seaWaves(look);
        for (const ftcore::SeaWave& w : sea.waves) {
            CHECK(std::fabs(w.omega * w.omega - ftcore::kSeaGravity * w.k) <= 1e-4f * w.omega * w.omega);
            // A whole number of turns in the loop, so the sea repeats.
            const float turns = w.omega * ftcore::kSeaLoopS / (2.f * kPi);
            CHECK(std::fabs(turns - std::round(turns)) < 1e-2f);
        }
        // L = g T^2 / (2 pi): 1.56 T^2 metres.
        const float expected = 9.81f * period * period / (2.f * kPi);
        CAPTURE(period);
        CHECK(std::fabs(lengthM(sea.waves[0]) - expected) <= 0.02f * expected);
    }
}

TEST_CASE("The harbour's wind gives short chop, and the heights add up") {
    const SeaWaves sea = ftcore::seaWaves(harbour());
    // The wind sea's main wave (index 4) has a period of 1.2 to 3 seconds: a few metres between crests.
    const float period = 2.f * kPi / sea.waves[4].omega;
    CHECK(period > 1.2f);
    CHECK(period < 3.f);
    // Fetch-limited: 0.0016 * 8 * sqrt(2000 / 9.81) metres; the swell's 1.5 m adds in energy.
    const float windHs = 0.0016f * 8.f * std::sqrt(2000.f / 9.81f) * 100.f;
    const float expected = std::sqrt(150.f * 150.f + windHs * windHs);
    CHECK(std::fabs(ftcore::seaSignificantHeightCm(sea) - expected) <= 0.01f * expected);
    // No chop without it.
    SceneLook calm = harbour();
    calm.seaChop = 0.f;
    CHECK(std::fabs(ftcore::seaSignificantHeightCm(ftcore::seaWaves(calm)) - 150.f) < 0.5f);
}

TEST_CASE("The surface never folds over, whatever the look") {
    const float winds[] = {0.f, 3.f, 8.f, 20.f, 40.f};
    const float fetches[] = {0.1f, 2.f, 50.f, 500.f};
    const float swells[] = {0.f, 1.f, 4.f, 8.f};
    const float chops[] = {0.f, 1.f, 3.f};
    for (const float wind : winds) {
        for (const float fetch : fetches) {
            for (const float swell : swells) {
                for (const float chop : chops) {
                    SceneLook l;
                    l.windMps = wind;
                    l.seaFetchKm = fetch;
                    l.seaSwellM = swell;
                    l.seaSwellPeriodS = 6.f;
                    l.seaChop = chop;
                    const SeaWaves sea = ftcore::seaWaves(l);
                    float fold = 0.f;
                    for (const ftcore::SeaWave& w : sea.waves) {
                        CHECK(w.k * w.amplitude <= ftcore::kSeaMaxSteepness + 1e-6f);
                        fold += w.k * w.amplitude;
                    }
                    CAPTURE(wind);
                    CAPTURE(fetch);
                    CAPTURE(swell);
                    CAPTURE(chop);
                    CHECK(sea.q >= 0.f);
                    CHECK(sea.q <= 1.f);
                    CHECK(sea.q * fold <= ftcore::kSeaMaxFold + 1e-5f);
                    for (int i = 0; i < 40; ++i) {
                        const float x = static_cast<float>(i) * 173.f - 3000.f;
                        const float y = static_cast<float>(i * i) * 7.f - 1500.f;
                        CHECK(ftcore::seaJacobian(sea, x, y, static_cast<float>(i) * 0.37f) > 0.f);
                    }
                }
            }
        }
    }
}

TEST_CASE("The height over a point is the height of the surface point that lands there") {
    SceneLook look = harbour();
    look.windMps = 20.f;
    look.seaChop = 2.f;
    const SeaWaves sea = ftcore::seaWaves(look);
    for (int i = 0; i < 50; ++i) {
        const float x0 = static_cast<float>(i) * 91.f - 2000.f;
        const float y0 = static_cast<float>(i % 7) * 333.f;
        const float t = static_cast<float>(i) * 1.7f;
        float d[3];
        ftcore::seaOffset(sea, x0, y0, t, d);
        CAPTURE(i);
        CHECK(std::fabs(ftcore::seaHeight(sea, x0 + d[0], y0 + d[1], t) - d[2]) < 0.05f);
    }
}

TEST_CASE("The sea is the same every run and repeats over its loop") {
    const SeaWaves a = ftcore::seaWaves(harbour());
    const SeaWaves b = ftcore::seaWaves(harbour());
    for (int i = 0; i < ftcore::kSeaWaveCount; ++i) {
        CHECK(a.waves[i].k == b.waves[i].k);
        CHECK(a.waves[i].amplitude == b.waves[i].amplitude);
        CHECK(a.waves[i].phase == b.waves[i].phase);
        float v[4];
        ftcore::packSeaWave(a.waves[i], v);
        CHECK(std::fabs(std::sqrt(v[0] * v[0] + v[1] * v[1]) - a.waves[i].k) <= 1e-6f * a.waves[i].k);
        CHECK(v[2] == a.waves[i].amplitude);
    }
    CHECK(a.q == b.q);
    float now[3], later[3];
    ftcore::seaOffset(a, 120.f, -80.f, 3.25f, now);
    ftcore::seaOffset(a, 120.f, -80.f, 3.25f + ftcore::kSeaLoopS, later);
    for (int k = 0; k < 3; ++k) CHECK(std::fabs(now[k] - later[k]) < 1e-2f);
}

TEST_CASE("Building and sampling the sea allocates nothing") {
    const SceneLook look = harbour();
    float sink = 0.f;
    const auto before = ftcore_test::allocStats().count;
    for (int i = 0; i < 240; ++i) {
        const SeaWaves sea = ftcore::seaWaves(look);
        sink += ftcore::seaHeight(sea, static_cast<float>(i) * 10.f, 0.f, static_cast<float>(i) / 24.f);
        sink += ftcore::seaJacobian(sea, 0.f, static_cast<float>(i), 1.f);
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(after == before);
    CHECK(sink != 0.f);
}
