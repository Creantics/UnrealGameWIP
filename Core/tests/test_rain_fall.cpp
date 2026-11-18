#include <doctest/doctest.h>
#include "ftcore/RainFall.h"
#include "ftcore/Vfx.h"
#include "AllocCounter.h"
#include "TestAssets.h"

#include <limits>
#include <string>

using namespace ftcore;

namespace {

bool loadRainEffect(const char* name, VfxEffect& out) {
    std::string text, err;
    if (!ftcore_test::readProjectData(std::string("Vfx/") + name + ".json", text)) return false;
    return importVfxEffect(text, out, &err);
}

const VfxEmitter* emitterNamed(const VfxEffect& e, const char* name) {
    for (const VfxEmitter& em : e.emitters)
        if (em.name == name) return &em;
    return nullptr;
}

}  // namespace

TEST_CASE("rain fall: no rain spawns nothing; full rain is the json's rates at full opacity") {
    const RainSpec rain;
    const RainFallSpec spec;
    const RainFallFrame none = rainFallFrame(rain, spec, 0.f);
    CHECK(none.streakRate == 0.f);
    CHECK(none.ringRate == 0.f);
    CHECK(none.dropRate == 0.f);
    CHECK(none.streakAlpha == 0.f);
    const RainFallFrame full = rainFallFrame(rain, spec, 1.f);
    CHECK(full.streakRate == doctest::Approx(spec.streaksPerSAtFull));
    CHECK(full.ringRate == doctest::Approx(spec.ringsPerSAtFull));
    CHECK(full.dropRate == doctest::Approx(spec.dropsPerSAtFull));
    CHECK(full.streakAlpha == doctest::Approx(1.f));
}

TEST_CASE("rain fall: rates are linear in the intensity; the opacity starts at the light-rain floor") {
    const RainSpec rain;
    const RainFallSpec spec;
    const RainFallFrame half = rainFallFrame(rain, spec, 0.5f);
    CHECK(half.streakRate == doctest::Approx(0.5f * spec.streaksPerSAtFull));
    CHECK(half.ringRate == doctest::Approx(0.5f * spec.ringsPerSAtFull));
    CHECK(half.dropRate == doctest::Approx(0.5f * spec.dropsPerSAtFull));
    CHECK(half.streakAlpha == doctest::Approx(rainStreakAlpha(rain, 0.5f)));
    const RainFallFrame light = rainFallFrame(rain, spec, 1e-4f);
    CHECK(light.streakRate > 0.f);
    CHECK(light.streakAlpha >= rain.streakAlphaMin);
    float last = -1.f;
    for (int k = 0; k <= 20; ++k) {
        const float r = rainFallFrame(rain, spec, float(k) / 20.f).streakRate;
        CHECK(r >= last);
        last = r;
    }
}

TEST_CASE("rain fall: out-of-range and non-finite intensities are clamped") {
    const RainSpec rain;
    const RainFallSpec spec;
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(rainFallFrame(rain, spec, -2.f).streakRate == 0.f);
    CHECK(rainFallFrame(rain, spec, nan).streakRate == 0.f);
    CHECK(rainFallFrame(rain, spec, nan).streakAlpha == 0.f);
    CHECK(rainFallFrame(rain, spec, -inf).streakRate == 0.f);
    CHECK(rainFallFrame(rain, spec, 7.f).streakRate == doctest::Approx(spec.streaksPerSAtFull));
    CHECK(rainFallFrame(rain, spec, inf).streakRate == doctest::Approx(spec.streaksPerSAtFull));
    RainFallSpec bad;
    bad.ringsPerSAtFull = -5.f;
    bad.dropsPerSAtFull = nan;
    CHECK(rainFallFrame(rain, bad, 1.f).ringRate == 0.f);
    CHECK(rainFallFrame(rain, bad, 1.f).dropRate == 0.f);
}

TEST_CASE("rain fall: the streak box rides over the camera, capped over the floor") {
    const RainFallSpec spec;
    // No floor found: the box over the camera.
    CHECK(rainColumnTop(spec, 10.f, false, 0.f) == doctest::Approx(10.f + spec.aboveCamM));
    // A camera near the floor: the box over the camera.
    CHECK(rainColumnTop(spec, 1.7f, true, 0.f) == doctest::Approx(1.7f + spec.aboveCamM));
    // A high camera: the box at columnM over the floor, so the streaks reach the ground.
    CHECK(rainColumnTop(spec, 20.f, true, 0.f) == doctest::Approx(spec.columnM));
    CHECK(rainColumnTop(spec, 25.f, true, 4.f) == doctest::Approx(4.f + spec.columnM));
    // A bad floor height is ignored.
    CHECK(rainColumnTop(spec, 2.f, true, std::numeric_limits<float>::quiet_NaN()) == doctest::Approx(2.f + spec.aboveCamM));
}

TEST_CASE("rain fall: splashes show only near a found floor") {
    const RainFallSpec spec;
    CHECK(rainSplashOn(spec, 1.7f, true, 0.f));
    CHECK(rainSplashOn(spec, spec.splashMaxDropM, true, 0.f));
    CHECK_FALSE(rainSplashOn(spec, spec.splashMaxDropM + 0.1f, true, 0.f));
    CHECK(rainSplashOn(spec, -0.4f, true, 0.f));
    CHECK_FALSE(rainSplashOn(spec, -1.f, true, 0.f));
    CHECK_FALSE(rainSplashOn(spec, 1.7f, false, 0.f));
    CHECK_FALSE(rainSplashOn(spec, std::numeric_limits<float>::quiet_NaN(), true, 0.f));
}

TEST_CASE("rain fall: the json effects use the same knob names and full rates") {
    const RainFallSpec spec;
    VfxEffect rain;
    REQUIRE(loadRainEffect("rain", rain));
    const VfxEmitter* streaks = emitterNamed(rain, "streaks");
    REQUIRE(streaks != nullptr);
    CHECK(streaks->rateParam == kRainUserRate);
    CHECK(streaks->colorParam == kRainUserColor);
    CHECK(streaks->rate == doctest::Approx(spec.streaksPerSAtFull));
    CHECK(streaks->lit);
    CHECK(streaks->nearFade);
    // Short loops: Niagara reads a bound rate once per loop.
    CHECK(streaks->duration <= 1.f);

    VfxEffect splash;
    REQUIRE(loadRainEffect("rain_splash", splash));
    const VfxEmitter* rings = emitterNamed(splash, "rings");
    const VfxEmitter* drops = emitterNamed(splash, "drops");
    REQUIRE(rings != nullptr);
    REQUIRE(drops != nullptr);
    CHECK(rings->rateParam == kRainUserRingRate);
    CHECK(drops->rateParam == kRainUserDropRate);
    CHECK(rings->colorParam == kRainUserColor);
    CHECK(drops->colorParam == kRainUserColor);
    CHECK(rings->rate == doctest::Approx(spec.ringsPerSAtFull));
    CHECK(drops->rate == doctest::Approx(spec.dropsPerSAtFull));
    CHECK(rings->sprite == VfxSprite::Ring);
    CHECK(rings->lit);
    CHECK(rings->duration <= 1.f);
    CHECK(drops->duration <= 1.f);

    // The plan keeps the knobs and the ring.
    VfxNiagaraPlan plan;
    planVfxNiagara(splash, plan);
    REQUIRE(plan.emitters.size() == 2);
    CHECK(plan.emitters[0].rateParam == kRainUserRingRate);
    CHECK(plan.emitters[0].sprite == VfxSprite::Ring);
    CHECK(plan.emitters[0].facing == VfxNiagaraFacing::Horizontal);
    planVfxNiagara(rain, plan);
    REQUIRE(plan.emitters.size() == 1);
    CHECK(plan.emitters[0].rateParam == kRainUserRate);
    CHECK(plan.emitters[0].lit);
    CHECK(plan.emitters[0].nearFade);
    CHECK(plan.emitters[0].facing == VfxNiagaraFacing::VelocityAligned);
}

TEST_CASE("rain fall: a frame allocates nothing") {
    const RainSpec rain;
    const RainFallSpec spec;
    const auto before = ftcore_test::allocStats().count;
    float sink = 0.f;
    for (int k = 0; k < 100; ++k) {
        const RainFallFrame f = rainFallFrame(rain, spec, float(k) / 100.f);
        sink += f.streakRate + rainColumnTop(spec, float(k), true, 0.f) + (rainSplashOn(spec, float(k), true, 0.f) ? 1.f : 0.f);
    }
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(sink > 0.f);
}
