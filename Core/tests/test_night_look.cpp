#include <doctest/doctest.h>
#include "ftcore/Backdrop.h"
#include "ftcore/NightLook.h"
#include "ftcore/WorldClock.h"
#include "AllocCounter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
        // From blend 1 on a real-time capture lands at the fill; before it the lift comes in with the blend (1 at 0).
        if (f.exposureBlend >= 1.f) {
            CHECK(f.skyScaleRealTime * f.sunScale * f.skyLuminance == doctest::Approx(f.skyScale).epsilon(1e-4));
        } else if (f.exposureBlend <= 0.f) {
            CHECK(f.skyScaleRealTime == f.skyScale);
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
        look.liftByBlend = false;
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

TEST_CASE("dusk fix guards: every blend-1 frame unchanged, lamps half on wherever the blend moves") {
    // 11-16 dusk ruling: the lift by the blend changes no frame at blend 1 (so the night floor table stays valid),
    // and the exposure never leaves the day's before the lamps are at least half on.
    const NightLook look;
    NightLook whole = look;
    whole.liftByBlend = false;
    const WorldClockSpec spec;
    const auto before = ftcore_test::allocStats().count;
    bool same = true, lampsHalf = true, liftInside = true;
    for (int m = 16 * 60; m <= 24 * 60; ++m) {
        const double h = m >= 24 * 60 ? 0.0 : m / 60.0;
        const float e = sunElevationDeg(spec, h);
        const float n = nightAmount(spec, h);
        const NightFrame f = nightFrameAt(look, n, e);
        const NightFrame w = nightFrameAt(whole, n, e);
        if (f.exposureBlend >= 1.f &&
            (f.skyScaleRealTime != w.skyScaleRealTime || f.sunScale != w.sunScale || f.skyScale != w.skyScale)) {
            same = false;
        }
        if (f.exposureBlend > 0.f && f.lamps < 0.5f) lampsHalf = false;
        // Between 1 (blend 0) and the whole lift.
        if (f.skyScaleRealTime < f.skyScale * (1.f - 1e-6f) || f.skyScaleRealTime > w.skyScaleRealTime * (1.f + 1e-6f)) {
            liftInside = false;
        }
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(same);
    CHECK(lampsHalf);
    CHECK(liftInside);
    // The floor table's knots (20:30 on) are all at blend 1.
    for (const NightFloorKnot& k : look.floorFit.knots) CHECK(nightExposureBlend(look, -k.moonDeg) == 1.f);
    CHECK(nightExposureBlend(look, sunElevationDeg(spec, 20.5)) == 1.f);
    if (ftcore_test::allocHookInstalled()) CHECK(after == before);
}

TEST_CASE("nightFrameAt: the lift by the blend has no step through the dusk") {
    // Every 0.25 degrees (about a game minute) from the sun 8 up to 20 down: the real-time sky's neighbours differ by
    // a bounded ratio at the ruled moon (0.07: 1.32; the lift then climbs about 30x over the blend's 5 degrees while
    // the range closes; the street's own 5-minute ratio is the strip's to show, not this model's).
    const WorldClockSpec spec;
    for (const float moon : {0.07f}) {
        NightLook look;
        look.moonShare = moon;
        float worst = 1.f, last = 0.f;
        for (int i = 0; i <= 112; ++i) {
            const float e = 8.f - 0.25f * static_cast<float>(i);
            const float t = std::clamp((spec.twilightHigh - e) / (spec.twilightHigh - spec.twilightLow), 0.f, 1.f);
            const NightFrame f = nightFrameAt(look, t * t * (3.f - 2.f * t), e);
            REQUIRE(std::isfinite(f.skyScaleRealTime));
            if (i > 0) worst = std::max(worst, std::max(f.skyScaleRealTime / last, last / f.skyScaleRealTime));
            last = f.skyScaleRealTime;
        }
        INFO("moon " << moon << " worst neighbour ratio " << worst);
        CHECK(worst <= 1.35f);
    }
}

TEST_CASE("nightExposureRange: the day's at blend 0, the night's exactly at 1, the dusk's range between") {
    NightLook look;
    NightFrame f = nightFrame(look, 1.f);
    f.exposureBlend = 0.f;
    ExposureRange r = nightExposureRange(look, f, -10.f, 20.f);
    CHECK(r.minEV100 == -10.f);
    CHECK(r.maxEV100 == 20.f);
    f.exposureBlend = 1.f;
    r = nightExposureRange(look, f, -10.f, 20.f);
    CHECK(r.minEV100 == f.exposureMinEV100);
    CHECK(r.maxEV100 == f.exposureMaxEV100);
    // The defaults leave the day's range: the old lerp from it.
    f.exposureBlend = 0.25f;
    r = nightExposureRange(look, f, -10.f, 20.f);
    CHECK(r.minEV100 == doctest::Approx(-10.f + (f.exposureMinEV100 + 10.f) * 0.25f));
    CHECK(r.maxEV100 == doctest::Approx(20.f + (f.exposureMaxEV100 - 20.f) * 0.25f));
    // A dusk range binds from the blend's first step.
    look.duskExposureMinEV100 = 1.f;
    look.duskExposureMaxEV100 = 4.f;
    f.exposureBlend = 1e-3f;
    r = nightExposureRange(look, f, -10.f, 20.f);
    CHECK(r.minEV100 == doctest::Approx(1.f).epsilon(1e-2));
    CHECK(r.maxEV100 == doctest::Approx(4.f).epsilon(1e-2));
    // Never wider than the day's; min never above max, even with the ends crossed.
    r = nightExposureRange(look, f, 2.f, 3.f);
    CHECK(r.minEV100 >= 2.f - 1e-2f);
    CHECK(r.maxEV100 <= 3.f + 1e-2f);
    look.duskExposureMinEV100 = 9.f;
    look.duskExposureMaxEV100 = -9.f;
    for (const float b : {1e-3f, 0.3f, 0.7f, 0.999f}) {
        f.exposureBlend = b;
        r = nightExposureRange(look, f, -10.f, 20.f);
        CHECK(r.minEV100 <= r.maxEV100);
    }
}