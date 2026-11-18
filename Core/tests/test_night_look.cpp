#include <doctest/doctest.h>
#include "ftcore/Backdrop.h"
#include "ftcore/NightLook.h"
#include "ftcore/WorldClock.h"
#include "AllocCounter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>

using namespace ftcore;

TEST_CASE("nightFrame: day leaves the lights as they are") {
    const NightFrame f = nightFrame(NightLook{}, 0.f);
    CHECK(f.sunScale == doctest::Approx(1.f));
    CHECK(f.skyScale == doctest::Approx(1.f));
    CHECK(f.skyScaleRealTime == doctest::Approx(1.f));
    CHECK(f.fogKeep == doctest::Approx(1.f));
    CHECK(f.fogGlow[0] == 0.f);
    CHECK_FALSE(f.lockExposure);
    CHECK(f.whiteMix == 0.f);
    CHECK(f.wetShare == doctest::Approx(NightLook{}.dayWetShare));
}

TEST_CASE("nightFrame: the ground is dry by day and fully wet at night") {
    const NightLook look;
    CHECK(nightFrame(look, 0.f).wetShare == 0.f);  // day wetness is the weather's job, not a constant
    NightLook damp;
    damp.dayWetShare = 0.5f;  // a look may still ask for a damp day
    CHECK(nightFrame(damp, 0.f).wetShare == doctest::Approx(0.5f));
    CHECK(nightFrame(look, 1.f).wetShare == doctest::Approx(1.f));
    CHECK(nightFrame(look, 7.f).wetShare == doctest::Approx(1.f));
    float last = 0.f;
    for (int i = 0; i <= 10; ++i) {
        const float w = nightFrame(look, static_cast<float>(i) / 10.f).wetShare;
        CHECK(w >= last);
        last = w;
    }
    NightLook odd;
    odd.dayWetShare = 3.f;  // clamped: never wetter by day than the look
    CHECK(nightFrame(odd, 0.f).wetShare == doctest::Approx(1.f));
}

TEST_CASE("nightFrame: a zero or negative range locks the exposure at the centre") {
    NightLook look;
    look.exposureRangeEV = -2.f;
    const NightFrame f = nightFrame(look, 1.f);
    CHECK(f.exposureMinEV100 == doctest::Approx(look.exposureEV100));
    CHECK(f.exposureMaxEV100 == doctest::Approx(look.exposureEV100));
}

TEST_CASE("nightFrame: full night is the spec, the same for the clock and a still") {
    NightLook look;
    const NightFrame clock = nightFrame(look, 1.f);
    const NightFrame still = nightFrame(look, 7.f); // clamped
    CHECK(clock.sunScale == doctest::Approx(look.moonShare));
    CHECK(clock.skyScale == doctest::Approx(look.skyFill));
    // A real-time sky, lit by a moon at moonShare under a dome dimmed to skyLuminance, lands at skyFill net.
    CHECK(clock.skyScaleRealTime * clock.sunScale * clock.skyLuminance == doctest::Approx(look.skyFill));
    CHECK(clock.skyLuminance == doctest::Approx(look.skyLuminance));
    CHECK(clock.fogGlow[0] == doctest::Approx(look.glow[0]));
    CHECK(clock.lockExposure);
    CHECK(clock.exposureEV100 == doctest::Approx(look.exposureEV100));
    // A range round the centre, not one locked EV; the white balance at the night's own.
    CHECK(clock.exposureMinEV100 == doctest::Approx(look.exposureEV100 - look.exposureRangeEV));
    CHECK(clock.exposureMaxEV100 == doctest::Approx(look.exposureEV100 + look.exposureRangeEV));
    CHECK(clock.exposureMinEV100 < clock.exposureMaxEV100);
    CHECK(clock.whiteMix == doctest::Approx(1.f));
    CHECK(clock.whiteKelvin == doctest::Approx(look.whiteKelvin));
    CHECK(still.exposureMinEV100 == clock.exposureMinEV100);
    // LED shops cooler than the lamps; the moon and sky fill cool.
    CHECK(look.shopKelvin > look.lampKelvin);
    // The hero corner's timber shops warm (about 2700 K), between the lamps' and the LED shops'.
    CHECK(look.heroShopKelvin == doctest::Approx(2700.f));
    CHECK(look.heroShopKelvin >= look.lampKelvin);
    CHECK(look.heroShopKelvin < look.shopKelvin);
    // The face ruling: the corner's lamp slots warm white (not the paper's amber), the fronts a key, the floor cool and
    // far bluer than skin (R/B well under 0.9), with no specular rectangle in the puddles.
    CHECK(look.heroLampKelvin >= 2400.f);
    CHECK(look.heroLampKelvin <= 2700.f);
    CHECK(look.heroFrontLumens > 0.f);
    CHECK(look.heroSkyLumens > 0.f);
    CHECK(look.heroSkyLumens < look.heroFrontLumens);
    CHECK(look.heroSkyTint[0] / look.heroSkyTint[2] < 0.6f);
    CHECK(look.heroSkySpecular == doctest::Approx(0.f));
    // The fronts' key: a faint sheen on wet setts, never its source's white rectangle.
    CHECK(look.heroFrontSpecular > 0.f);
    CHECK(look.heroFrontSpecular <= 0.25f);
    CHECK(look.moonTint[2] > look.moonTint[0]);
    // Wide, soft lamp pools within the engine's spot cone limit.
    CHECK(look.lampOuterDeg <= 80.f);
    CHECK(look.lampInnerDeg < look.lampOuterDeg);
    CHECK(look.lampSourceCm > 0.f);
    CHECK(still.sunScale == clock.sunScale);
    CHECK(still.skyScaleRealTime == clock.skyScaleRealTime);
    CHECK(still.fogGlow == clock.fogGlow);
    // Warm glow: the haze reads as the city's. The fill's tint leans against the captured sky's deep blue (the moon's
    // own stays cool, above), and the fill is strong enough to show an unlit facade.
    CHECK(look.glow[0] > look.glow[2]);
    CHECK(look.skyTint[0] > look.skyTint[2]);
    // Twilight's fill shows an unlit facade; the deep night's holds the street between lamps at the floor, dark but with
    // its forms (11-15 19:00 ruling: wide-street median 18-24/255 from 20:30, whether the moon is up or not).
    CHECK(look.twilightFill >= 0.5f);
    CHECK(look.twilightFill <= 1.5f);
    CHECK(look.skyFill >= 0.005f);  // night_1116a: 0.015 read 18.2 at 21:00 (the 0.14 of 85106b57 read 128)
    CHECK(look.skyFill <= 0.03f);
}

TEST_CASE("nightFrame: the sky dome darkens with the night, near black at full night, smooth through dusk") {
    const NightLook look;
    // The night sky is near black navy, not a dimmed day sky (the lamps carry the night).
    CHECK(look.skyLuminance >= 0.02f);
    CHECK(look.skyLuminance <= 0.05f);
    // Day exactly as before: the dome untouched and the capture's lift unchanged, to the bit.
    const NightFrame day = nightFrame(look, 0.f);
    CHECK(day.skyLuminance == 1.f);
    CHECK(day.skyScaleRealTime == 1.f);
    NightLook plain = look;
    plain.skyLuminance = 1.f;  // with the dome left alone the capture's lift is exactly the old one at every hour
    // Monotone through dusk, no step (it falls in ratio, under 20% a twentieth); the capture's net fill follows the
    // same curve as before.
    float last = 1.f;
    for (int i = 0; i <= 20; ++i) {
        const float n = static_cast<float>(i) / 20.f;
        const NightFrame f = nightFrame(look, n);
        CHECK(f.skyLuminance <= last);
        CHECK(f.skyLuminance > 0.f);
        CHECK(f.skyLuminance >= 0.8f * last);
        last = f.skyLuminance;
        const NightFrame old = nightFrame(plain, n);
        CHECK(f.skyScaleRealTime * f.skyLuminance == doctest::Approx(old.skyScaleRealTime));
        CHECK(f.sunScale == old.sunScale);  // the moon is not raised to make up for the darker dome
    }
    CHECK(nightFrame(look, 1.f).skyLuminance == doctest::Approx(look.skyLuminance));
    NightLook odd;
    odd.skyLuminance = 0.f;  // clamped above zero, so the capture's lift stays finite
    CHECK(std::isfinite(nightFrame(odd, 1.f).skyScaleRealTime));
}

TEST_CASE("nightFrame: the evening darkens on the sun's depth after NightAmount is full, with no step") {
    const NightLook look;
    const WorldClockSpec spec;
    const auto at = [&](double hours) {
        return nightFrame(look, nightAmount(spec, hours), nightDeep(look, sunElevationDeg(spec, hours)));
    };
    // 19:00: NightAmount is full, but the fill and dome are within 10% of twilight's (the old 19:00 look).
    const NightFrame seven = at(19.0);
    CHECK(seven.night == doctest::Approx(1.f));
    CHECK(seven.skyScale >= 0.9f * look.twilightFill);
    CHECK(seven.skyLuminance >= 0.9f);
    // 22:00 and midnight: exactly the spec, the same as a night still.
    const NightFrame still = nightFrame(look, 1.f);
    for (const double h : {22.0, 0.0}) {
        const NightFrame f = at(h);
        CHECK(f.deep == 1.f);
        CHECK(f.skyScale == doctest::Approx(still.skyScale));
        CHECK(f.skyScaleRealTime == doctest::Approx(still.skyScaleRealTime));
        CHECK(f.skyLuminance == doctest::Approx(still.skyLuminance));
    }
    CHECK(still.deep == 1.f);
    CHECK(still.skyScale == doctest::Approx(look.skyFill));
    // From the sun 6 degrees up to 60 down, every half degree: the dome never brightens and the fill (once the night is
    // full) never rises; the fill moves no more than about 10% a step (2 clock minutes) and the dome no more than about
    // 13% (it falls to the floor by 20:25 now), across the seam at -6 degrees too.
    // Once the night is full, a real-time capture lands at the fill.
    float lastDome = 2.f, lastFill = 0.f;
    for (int i = 0; i <= 132; ++i) {
        const float e = 6.f - 0.5f * static_cast<float>(i);
        const float t = std::clamp((spec.twilightHigh - e) / (spec.twilightHigh - spec.twilightLow), 0.f, 1.f);
        const NightFrame f = nightFrame(look, t * t * (3.f - 2.f * t), nightDeep(look, e));
        CHECK(f.skyLuminance <= lastDome);
        if (i > 0) {
            CHECK(f.skyLuminance >= 0.87f * lastDome);
            CHECK(f.skyScale <= 1.1f * lastFill);
            CHECK(f.skyScale >= 0.85f * lastFill);
            if (e < spec.twilightLow) CHECK(f.skyScale <= lastFill);
        }
        if (e <= spec.twilightLow) CHECK(f.skyScaleRealTime * f.sunScale * f.skyLuminance == doctest::Approx(f.skyScale));
        lastDome = f.skyLuminance;
        lastFill = f.skyScale;
    }
    // Ends crossed or equal: a step at deepHighDeg, still clamped; zero fill falls straight.
    NightLook flat = look;
    flat.deepLowDeg = flat.deepHighDeg;
    CHECK(nightDeep(flat, flat.deepHighDeg + 1.f) == 0.f);
    CHECK(nightDeep(flat, flat.deepHighDeg - 1.f) == 1.f);
    NightLook none = look;
    none.skyFill = 0.f;
    CHECK(nightFrame(none, 1.f, 0.5f).skyScale == doctest::Approx(0.5f * look.twilightFill));
    CHECK(std::isfinite(nightFrame(none, 1.f, 1.f).skyScaleRealTime));
}

TEST_CASE("nightFrameAt: lamps on before the exposure leaves the day's; the street never dips under the floor") {
    // 11-15 19:00 ruling, fault 1: every 5 minutes from 16:00 to 23:00 the predicted wide-street key falls (never
    // rises) to the night floor and holds there; the lamps are all on before the key is under twice the floor; the
    // exposure starts toward the night's only once the lamps are half on.
    // NightKeyModel's constants are fitted to the block-1 strip, so the floor here checks the curves' order against that
    // model, not a render: a guard that a swap or re-order of the curves is caught. The model's floor is its key at the
    // deepest fill in the run (23:00, the moon highest); the flatness against the measured street is the floor fit's own
    // test below.
    const NightLook look;
    const NightKeyModel model;
    const WorldClockSpec spec;
    const float floorKey = model.fillWeight * nightDeepFill(look, sunElevationDeg(spec, 23.0)) + model.lampWeight;
    const auto before = ftcore_test::allocStats().count;
    float last = 1e9f, lampsHalfAt = 99.f, keyUnderTwoFloorAt = 99.f;
    bool reachedFloor = false, monotone = true, lampsFirst = true, exposureAfterLamps = true, flat = true;
    for (int m = 16 * 60; m <= 23 * 60; m += 5) {
        const double h = m / 60.0;
        const float e = sunElevationDeg(spec, h);
        const float n = nightAmount(spec, h);
        const NightFrame f = nightFrameAt(look, n, e);
        const float key = nightSceneKey(look, model, n, e);
        if (!reachedFloor && key > last * (1.f + 1e-5f)) monotone = false;
        if (key < 2.f * floorKey && f.lamps < 1.f) lampsFirst = false;
        if (f.lamps < 0.5f && f.exposureBlend > 0.f) exposureAfterLamps = false;
        if (f.lamps >= 0.5f && lampsHalfAt > 90.f) lampsHalfAt = static_cast<float>(h);
        if (key < 2.f * floorKey && keyUnderTwoFloorAt > 90.f) keyUnderTwoFloorAt = static_cast<float>(h);
        if (key <= floorKey * 1.001f) reachedFloor = true;
        // From 20:30 the model's key stays within the fit's fill range (the derived fill falls with the moon's rise).
        if (h >= 20.5 && key > model.fillWeight * nightDeepFill(look, -34.5f) + model.lampWeight + 1e-6f) flat = false;
        CHECK(key >= floorKey * 0.999f);  // never under the floor
        // From blend 1 on (and above the dusk table) a real-time capture lands at the fill; between, the table's lift.
        if (f.exposureBlend >= 1.f || e >= look.duskLiftTopDeg) {
            CHECK(f.skyScaleRealTime * f.sunScale * f.skyLuminance == doctest::Approx(f.skyScale).epsilon(1e-4));
        }
        last = key;
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(monotone);
    CHECK(lampsFirst);
    CHECK(exposureAfterLamps);
    CHECK(reachedFloor);
    CHECK(flat);
    CHECK(lampsHalfAt < keyUnderTwoFloorAt);
    // The lamps are half on near sunset (18:00, the sun at 0), the key under twice the floor well into the evening.
    CHECK(lampsHalfAt >= 17.9f);
    CHECK(lampsHalfAt <= 18.2f);
    if (ftcore_test::allocHookInstalled()) CHECK(after == before);
}

TEST_CASE("nightDeepFill: the measured street key holds 18-24 from 20:30 to 24:00, continuous, no allocation") {
    // 11-16 01:40 ruling: the fill is derived so the METERED wide-street key is the same at 20:30, 22:00 and 24:00;
    // the floor fit's predicted key at the clock's own fill stays inside 18-24 every 5 minutes from 20:30 to 24:00.
    const NightLook look;
    const WorldClockSpec spec;
    // The default moon is the one the table was measured at; otherwise the fit is off and skyFill holds throughout.
    CHECK(look.moonShare == doctest::Approx(look.floorFit.moonShare).epsilon(1e-4));
    REQUIRE(nightFloorFitActive(look));
    for (std::size_t i = 1; i < look.floorFit.knots.size(); ++i) {
        CHECK(look.floorFit.knots[i].moonDeg > look.floorFit.knots[i - 1].moonDeg);
        CHECK(look.floorFit.knots[i].slope > 0.f);
    }
    const auto before = ftcore_test::allocStats().count;
    float lastFill = 1e9f;
    bool inside = true, falls = true, smooth = true;
    for (int m = 20 * 60 + 30; m <= 24 * 60; m += 5) {
        const double h = m / 60.0;
        const float e = sunElevationDeg(spec, h >= 24.0 ? h - 24.0 : h);
        const NightFrame f = nightFrameAt(look, nightAmount(spec, h >= 24.0 ? h - 24.0 : h), e);
        REQUIRE(f.deep == doctest::Approx(1.f));
        const float key = nightFloorKey(look.floorFit, -e, f.skyScale);
        if (key < 18.f || key > 24.f) inside = false;
        if (f.skyScale > lastFill * (1.f + 1e-5f)) falls = false;  // the moon rises, so the fill only falls
        if (lastFill < 1e8f && f.skyScale < 0.9f * lastFill) smooth = false;  // no step over 10% in 5 minutes
        // A real-time capture still lands at the fill.
        CHECK(f.skyScaleRealTime * f.sunScale * f.skyLuminance == doctest::Approx(f.skyScale).epsilon(1e-4));
        lastFill = f.skyScale;
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(inside);
    CHECK(falls);
    CHECK(smooth);
    if (ftcore_test::allocHookInstalled()) CHECK(after == before);
    // At each measured row the derived fill gives the floor exactly (unclamped), and the fill stays in its bounds.
    for (const NightFloorKnot& k : look.floorFit.knots) {
        const float fill = nightDeepFill(look, -k.moonDeg);
        CHECK(fill >= look.floorFit.fillMin);
        CHECK(fill <= look.twilightFill);
        if (fill > look.floorFit.fillMin) CHECK(nightFloorKey(look.floorFit, k.moonDeg, fill) == doctest::Approx(21.f));
    }
    // Flat outside the knots: above the first row (the sun higher) and below the last.
    CHECK(nightDeepFill(look, -20.f) == doctest::Approx(nightDeepFill(look, -look.floorFit.knots.front().moonDeg)));
    CHECK(nightDeepFill(look, -89.f) == doctest::Approx(nightDeepFill(look, -look.floorFit.knots.back().moonDeg)));
    // Off: a moon other than the table's, or floorKey 0 (an explicit -NightSky=), keeps skyFill at every hour.
    NightLook other = look;
    other.moonShare = 0.03f;
    CHECK_FALSE(nightFloorFitActive(other));
    CHECK(nightDeepFill(other, -53.4f) == other.skyFill);
    NightLook off = look;
    off.floorFit.floorKey = 0.f;
    CHECK(nightDeepFill(off, -68.f) == off.skyFill);
    CHECK(nightFrameAt(off, 1.f, -68.f).skyScale == doctest::Approx(off.skyFill));
    // A row whose slope is 0 can't be solved: skyFill, not a division by zero.
    NightLook bad = look;
    for (NightFloorKnot& k : bad.floorFit.knots) k.slope = 0.f;
    CHECK(nightDeepFill(bad, -53.4f) == bad.skyFill);
}

TEST_CASE("nightFrameAt: the directional light and the real-time sky lift have no step through the horizon") {
    // 11-15 20:12 review: every 0.25 degrees from the sun 8 up to 20 down, neighbours differ by at most 25%, for each
    // moon of the ruled bracket (0.07, 0.045, 0.03); NightAmount from the clock's own twilight band. A near-moonless
    // test switch (moonShare 1e-4) is exempt: the light then falls to the 1e-4 clamp itself.
    const WorldClockSpec spec;
    const auto before = ftcore_test::allocStats().count;
    for (const float moon : {0.07f, 0.045f, 0.03f}) {
        NightLook look;
        look.moonShare = moon;
        // The whole lift (-NightLiftAll) is the one this bound was set on; the lift by the blend has its own test.
        look.duskLiftTable = false;
        float worstSun = 1.f, worstSky = 1.f, lastSun = 0.f, lastSky = 0.f;
        for (int i = 0; i <= 112; ++i) {
            const float e = 8.f - 0.25f * static_cast<float>(i);
            const float t = std::clamp((spec.twilightHigh - e) / (spec.twilightHigh - spec.twilightLow), 0.f, 1.f);
            const NightFrame f = nightFrameAt(look, t * t * (3.f - 2.f * t), e);
            CHECK(std::isfinite(f.sunScale));
            CHECK(std::isfinite(f.skyScaleRealTime));
            if (i > 0) {
                worstSun = std::max(worstSun, std::max(f.sunScale / lastSun, lastSun / f.sunScale));
                worstSky = std::max(worstSky, std::max(f.skyScaleRealTime / lastSky, lastSky / f.skyScaleRealTime));
            }
            lastSun = f.sunScale;
            lastSky = f.skyScaleRealTime;
        }
        CHECK(worstSun <= 1.25f);
        CHECK(worstSky <= 1.25f);
        // Either side of 0 by a hair: the same light.
        const NightFrame up = nightFrameAt(look, 0.5f, 1e-3f);
        const NightFrame down = nightFrameAt(look, 0.5f, -1e-3f);
        CHECK(up.sunScale == doctest::Approx(down.sunScale).epsilon(2e-3));
        CHECK(up.skyScaleRealTime == doctest::Approx(down.skyScaleRealTime).epsilon(2e-3));
    }
    // By day the light is the sun's, untouched; in the deep night it is the moon's at its full share.
    const NightLook look;
    CHECK(nightFrameAt(look, 0.f, 30.f).sunScale == 1.f);
    CHECK(nightFrameAt(look, 1.f, -50.f).sunScale == doctest::Approx(look.moonShare));
    // A moonless (or overcast) night: the lift divides by liftFloorShare, not the 1e-4 clamp, so the capture is lifted
    // at most as for a moon of that share and the street darkens instead of blowing out (block 4's moonless read 209).
    for (const float moon : {1e-4f, 0.f, 0.005f}) {
        NightLook dark = look;
        dark.moonShare = moon;
        dark.floorFit.floorKey = 0.f;
        const NightFrame m = nightFrameAt(dark, 1.f, -53.4f);
        const NightFrame still = nightFrame(dark, 1.f);
        CHECK(m.skyScaleRealTime * look.liftFloorShare * m.skyLuminance == doctest::Approx(m.skyScale));
        CHECK(still.skyScaleRealTime * look.liftFloorShare * still.skyLuminance == doctest::Approx(still.skyScale));
    }
    NightLook dark = look;
    dark.moonShare = 1e-4f;
    dark.floorFit.floorKey = 0.f;
    NightLook ruled = dark;
    ruled.moonShare = 0.03f;  // the smallest ruled moon is lifted exactly, past the floor
    CHECK(nightFrameAt(dark, 1.f, -53.4f).skyScaleRealTime <= nightFrameAt(ruled, 1.f, -53.4f).skyScaleRealTime * 3.001f);
    CHECK(look.liftFloorShare < 0.03f);
    // A zero half-depth is clamped, not a NaN at the horizon.
    NightLook sharp = look;
    sharp.sunSetHalfDeg = 0.f;
    CHECK(std::isfinite(nightFrameAt(sharp, 0.5f, 0.f).sunScale));
    CHECK(std::isfinite(nightFrameAt(sharp, 0.5f, -10.f).skyScaleRealTime));
    const auto after = ftcore_test::allocStats().count;
    if (ftcore_test::allocHookInstalled()) CHECK(after == before);
}

TEST_CASE("nightLampOn: staggered by key, each lamp on for good once its point is passed") {
    int half = 0;
    for (std::uint32_t k = 0; k < 1000; ++k) {
        CHECK_FALSE(nightLampOn(0.f, k));
        CHECK(nightLampOn(1.f, k));
        if (nightLampOn(0.5f, k)) ++half;
        bool was = false;
        for (int i = 0; i <= 20; ++i) {
            const bool on = nightLampOn(static_cast<float>(i) / 20.f, k);
            CHECK((on || !was));  // once on, stays on as the fraction rises
            was = on;
        }
    }
    CHECK(half > 350);
    CHECK(half < 650);
    // Neighbouring keys don't come on together.
    int runs = 0;
    for (std::uint32_t k = 1; k < 200; ++k) runs += nightLampOn(0.5f, k) != nightLampOn(0.5f, k - 1) ? 1 : 0;
    CHECK(runs > 60);
}

TEST_CASE("lamp stagger: the fraction rises with the sun's depth, every lamp on by -4 degrees") {
    const NightLook look;
    float last = 0.f;
    for (int i = 0; i <= 200; ++i) {
        const float e = 6.f - 0.05f * static_cast<float>(i);  // +6 down to -4 degrees
        const float f = nightLampFraction(look, e);
        CHECK(f >= last);
        last = f;
    }
    CHECK(nightLampFraction(look, -4.f) == 1.f);
    int onAtSunset = 0;
    for (std::uint32_t k = 0; k < 500; ++k) {
        CHECK(nightLampOn(nightLampFraction(look, -4.f), k));
        CHECK_FALSE(nightLampOn(nightLampFraction(look, 1.5f), k));
        onAtSunset += nightLampOn(nightLampFraction(look, 0.f), k) ? 1 : 0;
    }
    // At sunset some lamps are on and most still off: they come on through the next quarter hour, not together.
    CHECK(onAtSunset > 0);
    CHECK(onAtSunset < 250);
}

TEST_CASE("nightLampFraction and nightExposureBlend: on the sun's elevation, the same at dawn") {
    const NightLook look;
    CHECK(nightLampFraction(look, look.lampsOnHighDeg + 1.f) == 0.f);
    CHECK(nightLampFraction(look, look.lampsOnLowDeg - 1.f) == 1.f);
    CHECK(nightExposureBlend(look, look.exposureHighDeg + 0.1f) == 0.f);
    CHECK(nightExposureBlend(look, look.exposureLowDeg - 0.1f) == 1.f);
    // The exposure starts no earlier than the lamps' half point.
    float halfDeg = look.lampsOnHighDeg;
    while (nightLampFraction(look, halfDeg) < 0.5f) halfDeg -= 0.01f;
    CHECK(nightExposureBlend(look, halfDeg) == 0.f);
    NightLook step = look;
    step.lampsOnLowDeg = step.lampsOnHighDeg;  // an empty span: a step at the high end
    CHECK(nightLampFraction(step, step.lampsOnHighDeg + 0.1f) == 0.f);
    CHECK(nightLampFraction(step, step.lampsOnHighDeg - 0.1f) == 1.f);
    // A still (no sun given): lamps and exposure follow the night alone.
    CHECK(nightFrame(look, 1.f).lamps == 1.f);
    CHECK(nightFrame(look, 1.f).exposureBlend == 1.f);
    CHECK(nightFrame(look, 0.f).exposureBlend == 0.f);
}

TEST_CASE("nightMoonScale: rises with the moon's elevation, full by 15 degrees; the floor does not need it") {
    const NightLook look;
    // The moon below or at the horizon (the sun up): its rise share, the same either side of 0 (no step).
    CHECK(nightMoonScale(look, 5.f) == doctest::Approx(look.moonRiseShare));
    CHECK(nightMoonScale(look, 0.f) == doctest::Approx(look.moonRiseShare));
    CHECK(nightMoonScale(look, -0.001f) == doctest::Approx(look.moonRiseShare).epsilon(1e-3));
    CHECK(nightMoonScale(look, -look.moonFullDeg) == doctest::Approx(1.f));
    CHECK(nightMoonScale(look, -40.f) == 1.f);
    float last = 0.f;
    for (int i = 0; i <= 30; ++i) {  // the horizon down to the sun 15 degrees under: the moon's rise
        const float s = nightMoonScale(look, -0.5f * static_cast<float>(i));
        CHECK(s >= last);
        last = s;
    }
    const NightFrame low = nightFrameAt(look, 1.f, -3.f);
    const NightFrame high = nightFrameAt(look, 1.f, -40.f);
    CHECK(low.sunScale < high.sunScale);
    CHECK(high.sunScale == doctest::Approx(look.moonShare));
    // The model's floor is the sky's: at one fill (the floor fit off), the same with or without a moon. (The measured
    // street is not: the floor fit is per moon share, nightDeepFill.)
    NightLook fixed = look;
    fixed.floorFit.floorKey = 0.f;
    NightLook moonless = fixed;
    moonless.moonShare = 0.f;
    const NightKeyModel model;
    CHECK(nightSceneKey(moonless, model, 1.f, -50.f) == doctest::Approx(nightSceneKey(fixed, model, 1.f, -50.f)));
    CHECK(nightMoonKey(look, model, 5.f) == 0.f);
    CHECK(nightMoonKey(look, model, -50.f) > nightMoonKey(look, model, -20.f));
}

TEST_CASE("nightCharacterFillLux: off by day, two stops under the lamp key at night, cool and from above") {
    const NightLook look;
    CHECK(nightCharacterFillLux(look, 0.f) == 0.f);
    const float key = look.lampCandela / (look.charFillRefM * look.charFillRefM);
    CHECK(nightCharacterFillLux(look, 1.f) == doctest::Approx(key / 4.f));
    CHECK(nightCharacterFillLux(look, 7.f) == nightCharacterFillLux(look, 1.f));
    CHECK(nightCharacterFillLux(look, 0.5f) == doctest::Approx(key / 8.f));
    CHECK(look.charFillStops == doctest::Approx(2.f));
    CHECK(look.charFillKelvin > look.whiteKelvin);
    CHECK(look.charFillPitchDeg < 0.f);
    NightLook odd;
    odd.charFillRefM = 0.f;  // a floor on the distance, so the fill stays finite
    odd.charFillStops = -3.f; // never over the key
    CHECK(std::isfinite(nightCharacterFillLux(odd, 1.f)));
    CHECK(nightCharacterFillLux(odd, 1.f) == doctest::Approx(look.lampCandela / 0.01f));
}

TEST_CASE("nightCharacterFillYaw: a soft key from the nearest light's side of the view") {
    NightLook look;
    CHECK(look.charFillYawDeg >= 30.f);
    CHECK(look.charFillYawDeg <= 45.f);
    const float yaw = look.charFillYawDeg;
    // View along +x; Unreal's right is +y. A light on the right: the light's travel turns left (negative yaw), so its
    // source sits right of the camera.
    CHECK(nightCharacterFillYaw(look, 1.f, 0.f, 2.f, 3.f) == doctest::Approx(-yaw));
    CHECK(nightCharacterFillYaw(look, 1.f, 0.f, 2.f, -3.f) == doctest::Approx(yaw));
    // Turned with the view: looking along +y, right is -x.
    CHECK(nightCharacterFillYaw(look, 0.f, 1.f, -1.f, 0.f) == doctest::Approx(-yaw));
    CHECK(nightCharacterFillYaw(look, 0.f, 1.f, 1.f, 0.f) == doctest::Approx(yaw));
    // No light, or one dead ahead: from the right.
    CHECK(nightCharacterFillYaw(look, 1.f, 0.f, 0.f, 0.f) == doctest::Approx(-yaw));
    CHECK(nightCharacterFillYaw(look, 1.f, 0.f, 5.f, 0.f) == doctest::Approx(-yaw));
    NightLook odd;
    odd.charFillYawDeg = 200.f;
    CHECK(nightCharacterFillYaw(odd, 1.f, 0.f, 0.f, -1.f) == doctest::Approx(90.f));
    odd.charFillYawDeg = -10.f;
    CHECK(nightCharacterFillYaw(odd, 1.f, 0.f, 0.f, -1.f) == 0.f);
}

TEST_CASE("night fill easing: off by default, the key follows the nearest light and its yaw snaps (as before)") {
    const NightLook look;
    CHECK(look.charFillSwitchNearer == 0.f);
    CHECK(look.charFillYawRateDeg == 0.f);
    CHECK(nightCharacterFillSwitchLight(look, -1.f, 9.f));
    CHECK(nightCharacterFillSwitchLight(look, 5.f, 4.99f));  // any other nearest light: followed at once
    CHECK(nightCharacterFillSwitchLight(look, 5.f, 5.f));    // ... an equally near one too (the nearest, as before)
    const float yaw = look.charFillYawDeg;
    CHECK(nightCharacterFillEaseYaw(look, yaw, -yaw, 1.f / 60.f) == -yaw);  // the target itself, in one frame
    CHECK(nightCharacterFillEaseYaw(look, 0.f, 30.f, 0.f) == 30.f);
    NightLook eased;
    nightCharacterFillEase(eased);  // -NightFillEase
    CHECK(eased.charFillSwitchNearer == kNightFillEaseSwitchNearer);
    CHECK(eased.charFillYawRateDeg == kNightFillEaseYawRateDeg);
    CHECK_FALSE(nightCharacterFillSwitchLight(eased, 5.f, 4.99f));
    CHECK(nightCharacterFillEaseYaw(eased, yaw, -yaw, 1.f / 60.f) != doctest::Approx(-yaw));
}

TEST_CASE("nightCharacterFillSwitchLight, eased: the key moves to a new light only when it is 20% nearer") {
    NightLook look;
    nightCharacterFillEase(look);
    CHECK(look.charFillSwitchNearer == doctest::Approx(0.2f));
    CHECK(nightCharacterFillSwitchLight(look, -1.f, 9.f));   // nothing followed yet
    CHECK_FALSE(nightCharacterFillSwitchLight(look, 5.f, 4.5f));  // 10% nearer: keep the old one
    CHECK_FALSE(nightCharacterFillSwitchLight(look, 5.f, 4.f));   // exactly 20%: keep
    CHECK(nightCharacterFillSwitchLight(look, 5.f, 3.9f));
    NightLook odd;
    odd.charFillSwitchNearer = 3.f;  // clamped to 0.9: a light ten times nearer still wins
    CHECK(nightCharacterFillSwitchLight(odd, 10.f, 0.5f));
    odd.charFillSwitchNearer = -1.f;  // 0 or less is off: the nearest light, an equal one too
    CHECK(nightCharacterFillSwitchLight(odd, 5.f, 4.99f));
    CHECK(nightCharacterFillSwitchLight(odd, 5.f, 5.f));
}

TEST_CASE("nightCharacterFillEaseYaw, eased: the key turns at 90 degrees a second, the short way round") {
    NightLook look;
    nightCharacterFillEase(look);
    CHECK(look.charFillYawRateDeg == doctest::Approx(90.f));
    const float yaw = look.charFillYawDeg;
    // A side flip (+35 to -35) takes 70/90 s: one 60 Hz frame turns 1.5 degrees, not the whole way.
    CHECK(nightCharacterFillEaseYaw(look, yaw, -yaw, 1.f / 60.f) == doctest::Approx(yaw - 1.5f));
    float y = yaw;
    int frames = 0;
    while (y > -yaw + 1e-3f && frames < 1000) {
        y = nightCharacterFillEaseYaw(look, y, -yaw, 1.f / 60.f);
        ++frames;
    }
    CHECK(frames == doctest::Approx(60.f * 2.f * yaw / 90.f).epsilon(0.03));
    CHECK(nightCharacterFillEaseYaw(look, -yaw, -yaw, 1.f) == doctest::Approx(-yaw));  // there: stays
    // Across +-180 it goes the short way and wraps.
    CHECK(nightCharacterFillEaseYaw(look, 170.f, -170.f, 0.1f) == doctest::Approx(179.f));
    CHECK(nightCharacterFillEaseYaw(look, 175.f, -170.f, 0.1f) == doctest::Approx(-176.f));
    CHECK(nightCharacterFillEaseYaw(look, 0.f, 30.f, -1.f) == doctest::Approx(0.f));  // no time, no turn
    NightLook snap;
    snap.charFillYawRateDeg = 0.f;
    CHECK(nightCharacterFillEaseYaw(snap, yaw, -yaw, 1.f / 60.f) == doctest::Approx(-yaw));
}

TEST_CASE("nightCharacterFillKeyShare: most of the fill is the key, the rest ambient") {
    NightLook look;
    CHECK(nightCharacterFillKeyShare(look) > 0.5f);
    CHECK(nightCharacterFillKeyShare(look) < 1.f);
    look.charFillKeyShare = 3.f;
    CHECK(nightCharacterFillKeyShare(look) == 1.f);
    look.charFillKeyShare = -1.f;
    CHECK(nightCharacterFillKeyShare(look) == 0.f);
}

TEST_CASE("nightCharacterFillShare: gone beside a lit bay, whole away from it, smooth between") {
    const NightLook look;
    CHECK(nightCharacterFillShare(look, -1.f) == 1.f);  // no lit bay
    CHECK(nightCharacterFillShare(look, std::nanf("")) == 1.f);
    CHECK(nightCharacterFillShare(look, 0.f) == 0.f);
    CHECK(nightCharacterFillShare(look, look.charFillBayNearM) == 0.f);
    CHECK(nightCharacterFillShare(look, look.charFillBayFarM) == 1.f);
    CHECK(nightCharacterFillShare(look, 100.f) == 1.f);
    const float mid = 0.5f * (look.charFillBayNearM + look.charFillBayFarM);
    CHECK(nightCharacterFillShare(look, mid) == doctest::Approx(0.5f));
    CHECK(nightCharacterFillShare(look, mid - 0.5f) < nightCharacterFillShare(look, mid + 0.5f));
    NightLook odd;
    odd.charFillBayNearM = 9.f;  // near past far: one edge at far
    odd.charFillBayFarM = 4.f;
    CHECK(nightCharacterFillShare(odd, 3.9f) == 0.f);
    CHECK(nightCharacterFillShare(odd, 4.f) == 1.f);
    odd.charFillBayFarM = -2.f;  // a negative far: whole everywhere a bay is known
    CHECK(nightCharacterFillShare(odd, 0.f) == 1.f);
}

TEST_CASE("nightFrame: exposure locks from half night; no allocation") {
    const auto before = ftcore_test::allocStats().count;
    const bool before49 = nightFrame(NightLook{}, 0.49f).lockExposure;
    const bool at50 = nightFrame(NightLook{}, 0.5f).lockExposure;
    NightLook zero;
    zero.moonShare = 0.f;
    const float lifted = nightFrame(zero, 1.f).skyScaleRealTime;
    const auto after = ftcore_test::allocStats().count;
    CHECK_FALSE(before49);
    CHECK(at50);
    CHECK(std::isfinite(lifted));
    if (ftcore_test::allocHookInstalled()) CHECK(after == before);
}

TEST_CASE("nightLampScales: a lone lamp keeps its light, a crowded lane is capped at the floor") {
    NightLook look;
    std::vector<float> xy = {0.f, 0.f, 10000.f, 0.f};
    for (int i = 0; i < 20; ++i) {
        xy.push_back(5000.f + 50.f * static_cast<float>(i)); // 20 lights 0.5 m apart, in cm
        xy.push_back(5000.f);
    }
    std::vector<float> scale(xy.size() / 2, -1.f);
    nightLampScales(look, xy.data(), scale.size(), 100.f, scale.data());
    CHECK(scale[0] == doctest::Approx(1.f));
    CHECK(scale[1] == doctest::Approx(1.f));
    CHECK(scale[2] == doctest::Approx(look.capFloor));
    CHECK(nightLampScale(look, 1) == doctest::Approx(1.f / (1.f + look.capShare)));
    CHECK(nightLampScale(look, 1000) == doctest::Approx(look.capFloor));
}

namespace {
// A test field: flat at 0, a ridge along x = 100 rising to 60 m (a tent 40 m a side), 1 m samples over 0..200.
std::vector<std::uint16_t> ridgeGrid(std::size_t n) {
    std::vector<std::uint16_t> g(n * n);
    for (std::size_t y = 0; y < n; ++y) {
        for (std::size_t x = 0; x < n; ++x) {
            const double d = std::abs(static_cast<double>(x) - 100.0);
            const double h = d < 40.0 ? 60.0 * (1.0 - d / 40.0) : 0.0;
            g[y * n + x] = static_cast<std::uint16_t>(std::lround(32768.0 + h * 128.0));
        }
    }
    return g;
}
} // namespace

TEST_CASE("hillDress: pylons ride the ridge, dressing keeps to the hill and off the boxes, repeatable") {
    const std::vector<std::uint16_t> g = ridgeGrid(201);
    HeightField f{g.data(), 201, 201, 1.0, 0.0, 0.0};
    HillDressSpec spec;
    spec.cellM = 3.f;
    spec.ringM = 10.f;
    spec.pylonStepM = 50.f;
    spec.maxSlope = 2.f;
    MassingBox box;
    box.cx = 100.0;
    box.cy = 150.0;
    box.sx = 20.0;
    box.sy = 20.0;
    const auto a = hillDress(f, spec, 0.0, 0.0, 200.0, 200.0, &box, 1);
    const auto b = hillDress(f, spec, 0.0, 0.0, 200.0, 200.0, &box, 1);
    REQUIRE(a.size() == b.size());
    std::size_t pylons = 0, trees = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].x == b[i].x);
        CHECK(a[i].z >= spec.minHeightM - 0.01f);
        const bool inBox = std::abs(a[i].x - 100.f) < 10.f + spec.clearM - spec.cellM && std::abs(a[i].y - 150.f) < 10.f + spec.clearM - spec.cellM;
        CHECK_FALSE(inBox);
        if (a[i].kind == BackdropKind::Pylon) {
            ++pylons;
            CHECK(std::abs(a[i].x - 100.f) <= 5.f * spec.cellM + 2.f);
        } else if (a[i].kind == BackdropKind::Tree) {
            ++trees;
        }
    }
    CHECK(pylons == 4);
    CHECK(trees > 0);
    spec.maxPoints = 3;
    CHECK(hillDress(f, spec, 0.0, 0.0, 200.0, 200.0, nullptr, 0).size() == 3);
}

TEST_CASE("skylineBand: blocks inside the band, taller further off, repeatable") {
    SkylineSpec s;
    s.x0 = 0.0;
    s.x1 = 1000.0;
    s.y0 = -2000.0;
    s.y1 = -1000.0;
    s.nearX = 500.0;
    s.nearY = 0.0;
    s.count = 400;
    const auto a = skylineBand(s);
    REQUIRE(a.size() == 400);
    CHECK(skylineBand(s)[17].cx == a[17].cx);
    double nearSum = 0.0, farSum = 0.0;
    int nearN = 0, farN = 0;
    for (const BackdropBox& b : a) {
        CHECK(b.cx >= s.x0);
        CHECK(b.cx <= s.x1);
        CHECK(b.cy >= s.y0);
        CHECK(b.cy <= s.y1);
        CHECK(b.h >= s.minH - 0.01f);
        CHECK(b.h <= s.maxH + 0.01f);
        if (b.cy > -1300.f) {
            nearSum += b.h;
            ++nearN;
        } else if (b.cy < -1700.f) {
            farSum += b.h;
            ++farN;
        }
    }
    REQUIRE(nearN > 0);
    REQUIRE(farN > 0);
    CHECK(farSum / farN > nearSum / nearN);
}

TEST_CASE("heightShapeMask: ridge over 128, flat at 128, rows north first; no allocation") {
    const std::vector<std::uint16_t> g = ridgeGrid(201);
    HeightField f{g.data(), 201, 201, 1.0, 0.0, 0.0};
    std::vector<std::uint8_t> mask(16 * 16, 0);
    const auto before = ftcore_test::allocStats().count;
    heightShapeMask(f, 100.0, 100.0, 200.0, 16, true, 8.f, 0.5f, mask.data());
    const auto after = ftcore_test::allocStats().count;
    if (ftcore_test::allocHookInstalled()) CHECK(after == before);
    // Column 7 and 8 straddle x = 100 (the ridge); column 0 is flat ground far from it.
    CHECK(mask[5 * 16 + 7] > 140);
    CHECK(mask[5 * 16 + 0] == 128);
    // An empty field reads even everywhere.
    heightShapeMask(HeightField{}, 0.0, 0.0, 10.0, 16, false, 8.f, 0.5f, mask.data());
    CHECK(mask[0] == 128);
}

TEST_CASE("night look: grey-blue ambient, orange lamps, warm glow") {
    const NightLook look;
    // A pale blue moon (green between red and blue), no channel under 0.6: a grey-blue, not a cobalt.
    CHECK(look.moonTint[1] > look.moonTint[0]);
    CHECK(look.moonTint[2] > look.moonTint[1]);
    CHECK(look.moonTint[0] >= 0.6f);
    CHECK(look.lampKelvin >= 2200.f);
    CHECK(look.lampKelvin <= 2700.f);
    CHECK(look.glow[0] > look.glow[1]);
    CHECK(look.glow[1] > look.glow[2]);
    // As twilight ends the fill shows an unlit facade (dark, not black): the captured sky is a deep blue that carries
    // little luminance, so the share is over the day's, many times the moon; its tint falls from red to blue, against
    // that blue. In the deep night it is the floor (11-15 19:00 ruling), held by the floor fit per moon elevation; the
    // still's skyFill sits inside the fit's range (11-16: the real-time lift divides by the moon, so a fill well under
    // the moon's share still holds the floor).
    CHECK(look.twilightFill >= 0.5f);
    CHECK(look.twilightFill <= 1.5f);
    CHECK(look.twilightFill >= 10.f * look.moonShare);
    CHECK(look.skyFill >= look.floorFit.fillMin);
    CHECK(look.skyFill <= nightDeepFill(look, -34.5f));
    CHECK(look.skyFill >= nightDeepFill(look, -68.f));
    CHECK(look.skyTint[0] >= look.skyTint[1]);
    CHECK(look.skyTint[1] > look.skyTint[2]);
    CHECK(look.skyTint[2] >= 0.4f);
    // The camera's white balance sits under neutral but not far: the frame is not pushed blue.
    CHECK(look.whiteKelvin >= 5400.f);
    CHECK(look.whiteKelvin < 6500.f);
    CHECK(look.lampCandela > look.shopCandela);
}

TEST_CASE("duskAmount: a bell over the sun's elevation, fullest at the peak, weaker in the morning") {
    const DuskLook look;
    CHECK(duskAmount(look, 30.f, true) == doctest::Approx(0.f));
    CHECK(duskAmount(look, look.startDeg, true) == doctest::Approx(0.f));
    CHECK(duskAmount(look, look.peakDeg, true) == doctest::Approx(1.f));
    CHECK(duskAmount(look, look.endDeg, true) == doctest::Approx(0.f));
    CHECK(duskAmount(look, -20.f, true) == doctest::Approx(0.f));
    CHECK(duskAmount(look, 8.f, true) > 0.f);
    CHECK(duskAmount(look, 8.f, true) < 1.f);
    CHECK(duskAmount(look, look.peakDeg, false) == doctest::Approx(look.morningShare));
    float last = 0.f;
    for (float e = look.startDeg; e >= look.peakDeg; e -= 0.5f) {
        const float a = duskAmount(look, e, true);
        CHECK(a >= last - 1e-6f);
        last = a;
    }
    DuskLook broken;
    broken.peakDeg = broken.startDeg + 1.f;
    CHECK(duskAmount(broken, 2.f, true) == doctest::Approx(0.f));
}

TEST_CASE("duskFrame: neutral at zero, the spec at one, magenta sky, dim fill; no allocation") {
    const DuskLook look;
    const auto before = ftcore_test::allocStats().count;
    const DuskFrame none = duskFrame(look, 0.f);
    const DuskFrame full = duskFrame(look, 2.f);
    const auto after = ftcore_test::allocStats().count;
    for (int i = 0; i < 3; ++i) {
        CHECK(none.sunMul[i] == doctest::Approx(1.f));
        CHECK(none.skyGlow[i] == doctest::Approx(1.f));
        CHECK(none.fogMul[i] == doctest::Approx(1.f));
        CHECK(full.sunMul[i] == doctest::Approx(look.sunTint[i]));
        CHECK(full.skyGlow[i] == doctest::Approx(look.skyGlow[i]));
    }
    CHECK(none.fillScale == doctest::Approx(1.f));
    CHECK(none.bloomAdd == doctest::Approx(0.f));
    CHECK(none.flareMix == doctest::Approx(0.f));
    CHECK(full.amount == doctest::Approx(1.f));
    CHECK(full.fillScale == doctest::Approx(look.fillScale));
    CHECK(full.fillScale < 0.7f);  // the skyline reads as a silhouette
    // Magenta-purple sky: red and blue over green.
    CHECK(full.skyGlow[0] > full.skyGlow[1] + 0.3f);
    CHECK(full.skyGlow[2] > full.skyGlow[1] + 0.3f);
    // Warm low sun.
    CHECK(full.sunMul[0] > full.sunMul[1]);
    CHECK(full.sunMul[1] > full.sunMul[2] - 0.2f);
    CHECK(full.flare > 0.f);
    if (ftcore_test::allocHookInstalled()) CHECK(after == before);
}

TEST_CASE("duskPeakHour: an evening hour where the sun stands at the peak, before full night") {
    const DuskLook look;
    const WorldClockSpec spec;
    const double h = duskPeakHour(look, spec.latitudeDeg);
    CHECK(h > 16.5);
    CHECK(h < 19.5);
    CHECK(sunElevationDeg(spec, h) == doctest::Approx(look.peakDeg).epsilon(0.02));
    CHECK(nightAmount(spec, h) < 0.5f);
}

TEST_CASE("dusk row: the still's hour, the lights coming on and the daylight in a room follow the sun's elevation") {
    DuskLook look;
    const WorldClockSpec spec;
    // Without a row the lights follow the night alone.
    CHECK(duskLights(look, 3.f) == 0.f);
    CHECK(duskLights(look, -30.f) == 0.f);
    look.stillDeg = 4.f;
    look.lightsOnDeg = 8.f;
    look.lightsFullDeg = 0.f;
    const double h = duskStillHour(look, spec.latitudeDeg);
    CHECK(h > 16.0);
    CHECK(h < duskPeakHour(look, spec.latitudeDeg));
    CHECK(sunElevationDeg(spec, h) == doctest::Approx(look.stillDeg).epsilon(0.02));
    CHECK(duskLights(look, 20.f) == 0.f);
    CHECK(duskLights(look, 8.f) == 0.f);
    CHECK(duskLights(look, 4.f) == doctest::Approx(0.5f));
    CHECK(duskLights(look, 0.f) == 1.f);
    CHECK(duskLights(look, -10.f) == 1.f);
    CHECK(duskDaylight(look, 40.f) == 1.f);
    CHECK(duskDaylight(look, -10.f) == 0.f);
    const float mid = duskDaylight(look, 4.f);
    CHECK(mid > 0.3f);
    CHECK(mid < 0.8f);
    const auto before = ftcore_test::allocStats().count;
    float sum = 0.f;
    for (int k = -20; k < 40; ++k) sum += duskLights(look, static_cast<float>(k)) + duskDaylight(look, static_cast<float>(k));
    const auto after = ftcore_test::allocStats().count;
    CHECK(sum > 0.f);
    if (ftcore_test::allocHookInstalled()) CHECK(after == before);
}

TEST_CASE("hillPlantMask: the hill 0 (planted), low ground 255, a ramp between; no allocation") {
    const std::vector<std::uint16_t> g = ridgeGrid(201);
    HeightField f{g.data(), 201, 201, 1.0, 0.0, 0.0};
    std::uint8_t m[200 * 200];
    const auto before = ftcore_test::allocStats().count;
    hillPlantMask(f, 100.0, 100.0, 200.0, 200, true, 14.f, 8.f, 9.f, 10.f, m);  // no slope cut
    const auto after = ftcore_test::allocStats().count;
    CHECK(m[100 * 200 + 100] == 0);    // ridge top
    CHECK(m[100 * 200 + 10] == 255);   // flat ground west
    CHECK(m[100 * 200 + 190] == 255);  // flat ground east
    // Monotonic from the ridge out to the flat.
    for (std::size_t ix = 101; ix < 199; ++ix) CHECK(m[100 * 200 + ix + 1] >= m[100 * 200 + ix]);
    bool ramp = false;
    for (std::size_t ix = 100; ix < 200; ++ix) ramp = ramp || (m[100 * 200 + ix] > 0 && m[100 * 200 + ix] < 255);
    CHECK(ramp);
    std::uint8_t e[16];
    hillPlantMask(HeightField{}, 0.0, 0.0, 10.0, 4, true, 14.f, 8.f, 0.55f, 0.75f, e);
    CHECK(e[5] == 255);  // no field: nothing is hill
    hillPlantMask(f, 0.0, 0.0, 1.0, 0, true, 14.f, 8.f, 0.55f, 0.75f, nullptr);  // no crash
    if (ftcore_test::allocHookInstalled()) CHECK(after == before);
    // By slope: the ridge's flanks rise 1.5 in 1, so they stay bare (rock); its level top stays planted.
    hillPlantMask(f, 100.0, 100.0, 200.0, 200, true, 14.f, 8.f, 0.55f, 0.75f, m);
    CHECK(m[100 * 200 + 100] == 0);
    CHECK(m[100 * 200 + 120] == 255);
    CHECK(m[100 * 200 + 80] == 255);
}

namespace {
MassingBox tallBox(const char* kind, double cx, double cy, double sx, double sy, double h) {
    MassingBox b;
    b.kind = kind;
    b.cx = cx;
    b.cy = cy;
    b.sx = sx;
    b.sy = sy;
    b.z0 = 5.0;
    b.z1 = 5.0 + h;
    return b;
}
} // namespace

TEST_CASE("aviationLamps: tallest roofs first, none on low or ground boxes, spaced, capped, repeatable") {
    std::vector<MassingBox> boxes;
    boxes.push_back(tallBox("kit", 0.0, 0.0, 30.0, 16.0, 36.0));     // long roof: two corners
    boxes.push_back(tallBox("kit", 10.0, 5.0, 12.0, 12.0, 33.0));    // too near the first
    boxes.push_back(tallBox("kit", 200.0, 0.0, 12.0, 12.0, 31.0));   // one corner
    boxes.push_back(tallBox("kit", 400.0, 0.0, 12.0, 12.0, 20.0));   // too low
    boxes.push_back(tallBox("cliff", 600.0, 0.0, 24.0, 24.0, 60.0)); // ground, not a roof
    boxes.push_back(tallBox("mast", 800.0, 0.0, 1.0, 1.0, 55.0));    // a mast: one at its top centre
    std::vector<BackdropBox> sky(3);
    sky[0] = {2000.f, 0.f, 30.f, 30.f, 0.f, 100.f};
    sky[1] = {2010.f, 0.f, 30.f, 30.f, 0.f, 90.f};  // too near sky[0]
    sky[2] = {2500.f, 0.f, 30.f, 30.f, 0.f, 50.f};  // too low
    const AviationSpec spec;
    const auto a = aviationLamps(spec, boxes.data(), boxes.size(), sky.data(), sky.size());
    const auto b = aviationLamps(spec, boxes.data(), boxes.size(), sky.data(), sky.size());
    REQUIRE(a.size() == b.size());
    REQUIRE(a.size() == 5);
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].x == b[i].x);
        CHECK(a[i].heightM >= spec.minHeightM);
    }
    CHECK(a[0].heightM == doctest::Approx(55.f));  // the mast is tallest
    CHECK(a[0].x == doctest::Approx(800.f));
    CHECK(a[0].z == doctest::Approx(5.f + 55.f + spec.headM));
    CHECK(a[1].heightM == doctest::Approx(36.f));
    CHECK(a[2].heightM == doctest::Approx(36.f));
    CHECK(std::abs(a[1].x) == doctest::Approx(15.f - spec.insetM));
    CHECK(a[1].x == doctest::Approx(-a[2].x));
    CHECK(a[3].x == doctest::Approx(206.f - spec.insetM));
    CHECK(a[4].x == doctest::Approx(2000.f));
    CHECK(a[4].z == doctest::Approx(100.f + spec.headM));
    AviationSpec few = spec;
    few.maxLamps = 2;
    few.maxSkyline = 0;
    CHECK(aviationLamps(few, boxes.data(), boxes.size(), sky.data(), sky.size()).size() == 2);
    CHECK(aviationLamps(spec, nullptr, 0, nullptr, 0).empty());
}

TEST_CASE("dusk fix guards: frames outside the dusk table unchanged, lamps half on wherever the blend moves") {
    // 11-16 dusk ruling: the dusk table changes no frame at blend 1 (so the night floor table stays valid) nor above
    // its top, and the exposure never leaves the day's before the lamps are at least half on.
    const NightLook look;
    NightLook whole = look;
    whole.duskLiftTable = false;
    const WorldClockSpec spec;
    const auto before = ftcore_test::allocStats().count;
    bool same = true, lampsHalf = true;
    for (int m = 16 * 60; m <= 24 * 60; ++m) {
        const double h = m >= 24 * 60 ? 0.0 : m / 60.0;
        const float e = sunElevationDeg(spec, h);
        const float n = nightAmount(spec, h);
        const NightFrame f = nightFrameAt(look, n, e);
        const NightFrame w = nightFrameAt(whole, n, e);
        if ((f.exposureBlend >= 1.f || e >= look.duskLiftTopDeg) &&
            (f.skyScaleRealTime != w.skyScaleRealTime || f.sunScale != w.sunScale || f.skyScale != w.skyScale)) {
            same = false;
        }
        if (f.exposureBlend > 0.f && f.lamps < 0.5f) lampsHalf = false;
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(same);
    CHECK(lampsHalf);
    // The floor table's knots (20:30 on) are all at blend 1.
    for (const NightFloorKnot& k : look.floorFit.knots) CHECK(nightExposureBlend(look, -k.moonDeg) == 1.f);
    CHECK(nightExposureBlend(look, sunElevationDeg(spec, 20.5)) == 1.f);
    // Each table's knots descend strictly inside its span.
    CHECK(look.duskLiftTopDeg > look.duskLiftDeg[0]);
    for (std::size_t i = 1; i < look.duskLiftDeg.size(); ++i) CHECK(look.duskLiftDeg[i - 1] > look.duskLiftDeg[i]);
    CHECK(look.duskLiftDeg.back() > look.exposureLowDeg);
    CHECK(look.exposureHighDeg > look.duskMinDeg[0]);
    for (std::size_t i = 1; i < look.duskMinDeg.size(); ++i) CHECK(look.duskMinDeg[i - 1] > look.duskMinDeg[i]);
    CHECK(look.duskMinDeg.back() > look.exposureLowDeg);
    if (ftcore_test::allocHookInstalled()) CHECK(after == before);
}

TEST_CASE("dusk table: the knots' values at their elevations, the whole lift and the day's and night's floor outside") {
    const NightLook look;
    const float nightMin = -2.2f;
    for (std::size_t i = 0; i < look.duskLift.size(); ++i) {
        CHECK(duskLiftAt(look, look.duskLiftDeg[i], 7.f) == doctest::Approx(look.duskLift[i]));
    }
    for (std::size_t i = 0; i < look.duskMinEV.size(); ++i) {
        CHECK(duskMinAt(look, look.duskMinDeg[i], -10.f, nightMin) == doctest::Approx(look.duskMinEV[i]));
    }
    CHECK(duskLiftAt(look, look.duskLiftTopDeg + 0.5f, 7.f) == 7.f);
    CHECK(duskLiftAt(look, look.exposureLowDeg, 7.f) == 7.f);
    CHECK(duskLiftAt(look, std::nanf(""), 7.f) == 7.f);
    CHECK(duskMinAt(look, look.exposureHighDeg, -10.f, nightMin) == -10.f);
    CHECK(duskMinAt(look, look.exposureLowDeg, -10.f, nightMin) == nightMin);
    // Continuous: a thousandth of a degree, from 6 up to 9 down, never moves the lift by more than 1% or the floor by
    // more than 0.05 EV (no step at either end of either table).
    float lastLift = duskLiftAt(look, 6.f, 7.f), lastMin = duskMinAt(look, 6.f, -10.f, nightMin);
    float worstLift = 1.f, worstMin = 0.f;
    for (int i = 1; i <= 15000; ++i) {
        const float e = 6.f - 1e-3f * static_cast<float>(i);
        const float l = duskLiftAt(look, e, 7.f), mn = duskMinAt(look, e, -10.f, nightMin);
        worstLift = std::max(worstLift, std::max(l / lastLift, lastLift / l));
        worstMin = std::max(worstMin, std::fabs(mn - lastMin));
        lastLift = l;
        lastMin = mn;
    }
    CHECK(worstLift <= 1.01f);
    CHECK(worstMin <= 0.05f);
}

TEST_CASE("dusk table: the strip-centre key it predicts from blocks 1116e/f falls smoothly from 17:45 to 18:30") {
    // The measured rows (strip-centre key, 0-255): 18:00 and 18:05 at blend 0 (the day's free meter) against the
    // real-time lift r; 18:10 and 18:20 with the meter pinned, against EV at the r they were shot at. 17:45 (101, the
    // shade fade) and 18:30 (39.4, blend 1, unchanged) are the ends. Known fault, not checked: 18:30 -> 19:00 rises
    // x1.21 (39.4 -> 47.7) with the meter pinned too (0 -> 4.9), the moon's own elevation, not the table.
    const NightLook look;
    const WorldClockSpec spec;
    auto frameAt = [&](double h) { return nightFrameAt(look, nightAmount(spec, h), sunElevationDeg(spec, h)); };
    // 18:00: 35.9 at r 2.136, 92.5 at r 4.271 (r^1.37); 18:05: 46.7 at r 1.302, 93.3 at r 4.35 (r^0.57).
    const float r1800 = frameAt(18.0).skyScaleRealTime;
    const float r1805 = frameAt(18.0 + 5.0 / 60.0).skyScaleRealTime;
    const float k1800 = 35.9f * std::pow(r1800 / 2.136f, std::log(92.5f / 35.9f) / std::log(4.271f / 2.136f));
    const float k1805 = 46.7f * std::pow(r1805 / 1.302f, std::log(93.3f / 46.7f) / std::log(4.35f / 1.302f));
    // 18:10 (r 1.683): 21 at EV -4.5, 65 at -5.5; 18:20 (r 17.8): 12.2 at -2.5, 55.5 at -3.5. The floor binds there.
    const NightFrame f1810 = frameAt(18.0 + 10.0 / 60.0), f1820 = frameAt(18.0 + 20.0 / 60.0);
    CHECK(f1810.skyScaleRealTime == doctest::Approx(1.683f).epsilon(0.08));
    CHECK(f1820.skyScaleRealTime == doctest::Approx(17.8f).epsilon(0.08));
    const float e1810 = sunElevationDeg(spec, 18.0 + 10.0 / 60.0), e1820 = sunElevationDeg(spec, 18.0 + 20.0 / 60.0);
    const float ev1810 = nightExposureRange(look, f1810, e1810, -10.f, 20.f).minEV100;
    const float ev1820 = nightExposureRange(look, f1820, e1820, -10.f, 20.f).minEV100;
    const float k1810 = 65.f * std::pow(65.f / 21.f, -5.5f - ev1810);
    const float k1820 = 55.5f * std::pow(55.5f / 12.2f, -3.5f - ev1820);
    // The table's own span only; the ends either side are the code's (the sweep below), not hard-coded reads.
    const float keys[] = {k1800, k1805, k1810, k1820};
    INFO("18:00 " << k1800 << " (r " << r1800 << "), 18:05 " << k1805 << " (r " << r1805 << "), 18:10 " << k1810
                  << " (EV " << ev1810 << "), 18:20 " << k1820 << " (EV " << ev1820 << "); e " << e1810 << ", "
                  << sunElevationDeg(spec, 18.25) << ", " << e1820 << ", " << sunElevationDeg(spec, 18.0 + 25.0 / 60.0)
                  << ", " << sunElevationDeg(spec, 18.0 + 5.0 / 60.0) << ", " << sunElevationDeg(spec, 18.0));
    for (std::size_t i = 1; i < std::size(keys); ++i) {
        CHECK(keys[i] <= keys[i - 1] * 1.02f);
        CHECK(std::max(keys[i], keys[i - 1]) / std::min(keys[i], keys[i - 1]) <= 1.6f);
    }
    CHECK(k1800 == doctest::Approx(75.f).epsilon(0.1));
    CHECK(k1805 == doctest::Approx(68.f).epsilon(0.1));
    CHECK(k1810 == doctest::Approx(60.f).epsilon(0.1));
    CHECK(k1820 == doctest::Approx(48.f).epsilon(0.1));
}

TEST_CASE("dusk: every minute from 17:30 to 18:30, from the code: the key never rises, the camera only closes") {
    // nightSceneKey (the old tone's model) holds its dayKey clamp through this whole hour, so it can only say the key
    // never rises; the fall itself is the knot prediction above. What the code alone does guarantee, minute by minute:
    // once the blend moves, the exposure floor only climbs (the camera only closes); from the 18:10 knot on it never steps
    // more than 0.6 EV in a minute; the real-time lift never jumps by more than x1.35 in a minute. Known and left for the
    // later night block (fault 2, 18:00 -> 18:10): between the blend's start (-2 deg, 18:08.6) and the 18:10 knot the floor
    // climbs from the day's -10 at about 3.3 EV a minute; it binds only if the free meter is under it there.
    const NightLook look;
    const NightKeyModel model;
    const WorldClockSpec spec;
    float lastKey = 1e9f, lastMin = -1e9f, lastLift = -1.f, worstEV = 0.f, worstLift = 1.f;
    bool keyHolds = true, closes = true;
    for (int i = 0; i <= 60; ++i) {
        const double h = 17.5 + i / 60.0;
        const float e = sunElevationDeg(spec, h);
        const float n = nightAmount(spec, h);
        const NightFrame f = nightFrameAt(look, n, e);
        const float key = nightSceneKey(look, model, n, e);
        if (key > lastKey * (1.f + 1e-5f)) keyHolds = false;
        const float mn = nightExposureRange(look, f, e, -10.f, 20.f).minEV100;
        if (f.exposureBlend > 0.f && lastMin > -1e8f) {
            if (mn < lastMin - 1e-4f) closes = false;
            if (e <= look.duskMinDeg[0]) worstEV = std::max(worstEV, mn - lastMin);
        }
        if (lastLift > 0.f) worstLift = std::max(worstLift, std::max(f.skyScaleRealTime / lastLift, lastLift / f.skyScaleRealTime));
        lastKey = key;
        lastMin = f.exposureBlend > 0.f ? mn : lastMin;
        lastLift = f.skyScaleRealTime;
    }
    INFO("worst floor step " << worstEV << " EV/min, worst lift step x" << worstLift << "/min");
    CHECK(keyHolds);
    CHECK(closes);
    CHECK(worstEV <= 0.6f);
    CHECK(worstLift <= 1.35f);
}

TEST_CASE("nightExposureRange: the day's at blend 0, the night's exactly at 1, the dusk table's floor between") {
    NightLook look;
    NightFrame f = nightFrame(look, 1.f);
    f.exposureBlend = 0.f;
    ExposureRange r = nightExposureRange(look, f, -1.f, -10.f, 20.f);
    CHECK(r.minEV100 == -10.f);
    CHECK(r.maxEV100 == 20.f);
    f.exposureBlend = 1.f;
    r = nightExposureRange(look, f, -8.f, -10.f, 20.f);
    CHECK(r.minEV100 == f.exposureMinEV100);
    CHECK(r.maxEV100 == f.exposureMaxEV100);
    look.duskMinEV = {-5.f, -4.f, -3.f, -2.5f};
    f.exposureBlend = 0.25f;
    r = nightExposureRange(look, f, look.duskMinDeg[1], -10.f, 20.f);
    CHECK(r.minEV100 == doctest::Approx(-4.f));
    CHECK(r.maxEV100 == doctest::Approx(20.f + (f.exposureMaxEV100 - 20.f) * 0.25f));
    // Never under the day's own floor; min never above max, even with the table over the max.
    r = nightExposureRange(look, f, look.duskMinDeg[1], -3.5f, 20.f);
    CHECK(r.minEV100 >= -3.5f);
    look.duskMinEV = {30.f, 30.f, 30.f, 30.f};
    for (const float b : {1e-3f, 0.3f, 0.7f, 0.999f}) {
        f.exposureBlend = b;
        for (const float e : look.duskMinDeg) {
            r = nightExposureRange(look, f, e, -10.f, 20.f);
            CHECK(r.minEV100 <= r.maxEV100);
        }
    }
}
TEST_CASE("rim: the lamp that lights the lead most gives the colour and the camera side") {
    using namespace ftcore;
    RimLamp lamps[4];
    // A strong lamp far to the camera's left, a weak one near on the right, one off, one at zero candela.
    lamps[0].at[0] = -800.f; lamps[0].candela = 24.f; lamps[0].on = true;
    lamps[1].at[0] = 150.f;  lamps[1].candela = 2.4f; lamps[1].on = true;
    lamps[2].at[0] = 10.f;   lamps[2].candela = 100.f; lamps[2].on = false;
    lamps[3].at[0] = -5.f;   lamps[3].candela = 0.f;  lamps[3].on = true;
    const float lead[3] = {0.f, 0.f, 0.f};
    const float right[3] = {1.f, 0.f, 0.f};
    RimPick p = rimPickLamp(lamps, 4, lead, right, 50.f);
    // 2.4 / 150^2 = 1.07e-4 beats 24 / 800^2 = 3.75e-5; the off and the dark lamps never count.
    CHECK(p.index == 1);
    CHECK(p.side == doctest::Approx(1.f));
    CHECK(p.weight == doctest::Approx(2.4f / (150.f * 150.f)));
    lamps[1].on = false;
    p = rimPickLamp(lamps, 4, lead, right, 50.f);
    CHECK(p.index == 0);
    CHECK(p.side == doctest::Approx(-1.f));
    // A lamp at the lead's own point is floored at minDist, not infinite.
    lamps[0].at[0] = 0.f;
    p = rimPickLamp(lamps, 4, lead, right, 50.f);
    CHECK(p.weight == doctest::Approx(24.f / 2500.f));
    // None lit: no pick, no side.
    lamps[0].on = false;
    p = rimPickLamp(lamps, 4, lead, right, 50.f);
    CHECK(p.index == -1);
    CHECK(p.side == doctest::Approx(0.f));
    CHECK(rimPickLamp(nullptr, 0, lead, right, 50.f).index == -1);
}

TEST_CASE("rim: the colour eases over half a second") {
    using namespace ftcore;
    CHECK(rimEaseShare(0.f, kRimColourEaseSeconds) == doctest::Approx(0.f));
    CHECK(rimEaseShare(1.f / 60.f, kRimColourEaseSeconds) > 0.f);
    CHECK(rimEaseShare(1.f / 60.f, kRimColourEaseSeconds) < 0.2f);
    // Thirty 60 Hz frames (0.5 s) cover about 95% of a step, whatever the frame rate.
    float left = 1.f;
    for (int i = 0; i < 30; ++i) left *= 1.f - rimEaseShare(1.f / 60.f, kRimColourEaseSeconds);
    CHECK(1.f - left == doctest::Approx(0.95f).epsilon(0.01));
    float left30 = 1.f;
    for (int i = 0; i < 15; ++i) left30 *= 1.f - rimEaseShare(1.f / 30.f, kRimColourEaseSeconds);
    CHECK(left30 == doctest::Approx(left).epsilon(1e-3));
    CHECK(rimEaseShare(0.1f, 0.f) == doctest::Approx(1.f));
}