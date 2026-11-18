#include <doctest/doctest.h>
#include "ftcore/DayLook.h"
#include "ftcore/DayShade.h"
#include "ftcore/NightLook.h"
#include "ftcore/SceneLook.h"
#include "ftcore/WorldClock.h"
#include "AllocCounter.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace ftcore;

namespace {
// A dry day row and a rain night row, the levers' fields as the Solhaven looks have them.
SceneLook dryRow() {
    SceneLook l;
    l.sunKelvin = 6000.f;
    l.fogDensity = 0.011f;
    l.shadowTintHue = 250.f;
    l.shadowTintChroma = 0.008f;
    l.highlightTintHue = 70.f;
    l.highlightTintChroma = 0.006f;
    return l;
}
SceneLook rainRow() {
    SceneLook l = dryRow();
    l.fogDensity = 0.05f;
    l.shadowTintHue = 220.f;
    l.shadowTintChroma = 0.05f;
    l.cloudCover = 1.f;
    l.windMps = 4.f;
    return l;
}
float lightsAt(const WorldClockSpec& clock, double h) {
    const float e = sunElevationDeg(clock, h);
    return std::max(nightFrameAt(NightLook{}, nightAmount(clock, h), e).lamps, duskLights(DuskLook{}, e));
}
} // namespace

TEST_CASE("dayLookShade: the bracket's three steps, the fog cap, and any other step keeps the defaults") {
    const DayShadeLook def;
    DayLookSpec spec;
    spec.shadeStep = 1;
    DayShadeLook s = dayLookShade(def, spec);
    CHECK(s.skyScale == 2.0f);
    CHECK(s.shadowContrast == 0.6f);
    CHECK(s.highlightContrast == 0.85f);
    CHECK(s.diffuseBoost == 1.6f);
    CHECK(s.fogMaxOpacity == 0.85f);
    CHECK(s.filmToe == def.filmToe);
    CHECK(s.minEV100 == def.minEV100);
    spec.shadeStep = 3;
    s = dayLookShade(def, spec);
    CHECK(s.skyScale == 1.4f);
    CHECK(s.shadowContrast == 0.8f);
    CHECK(s.highlightContrast == 0.8f);
    CHECK(s.diffuseBoost == 1.3f);
    // Each step lifts the shade less than the one before, and every step less than the user's floor (the defaults).
    float prevSky = def.skyScale + 1.f;
    for (int k = 1; k <= kDayLookShadeSteps; ++k) {
        spec.shadeStep = k;
        const DayShadeLook t = dayLookShade(def, spec);
        CHECK(t.skyScale < prevSky);
        CHECK(t.skyScale < def.skyScale);
        CHECK(t.shadowContrast > def.shadowContrast);
        CHECK(t.diffuseBoost < def.diffuseBoost);
        prevSky = t.skyScale;
    }
    spec.shadeStep = 0;
    s = dayLookShade(def, spec);
    CHECK(s.skyScale == def.skyScale);
    CHECK(s.shadowContrast == def.shadowContrast);
    CHECK(s.fogMaxOpacity == 0.85f);
    spec.shadeStep = 4;
    CHECK(dayLookShade(def, spec).skyScale == def.skyScale);
}

TEST_CASE("dayLookScene: amount 0 keeps the row; a rain night comes back exactly") {
    const DayLookSpec spec;
    const SceneLook rain = rainRow();
    CHECK(dayLookScene(rain, spec, 0.f) == rain);
    CHECK(dayLookScene(rain, spec, -1.f) == rain);
    CHECK(dayLookScene(rain, spec, std::numeric_limits<float>::quiet_NaN()) == rain);
    // A dry row keeps every field but the two held ones.
    const SceneLook dry = dryRow();
    SceneLook expect = dry;
    expect.windMps = spec.windMps;
    expect.cloudCover = spec.cloudCover;
    CHECK(dayLookScene(dry, spec, 0.f) == expect);
}

TEST_CASE("dayLookScene: a look set at night keeps the row's wind and cover all session") {
    const DayLookSpec spec;
    const SceneLook dry = dryRow();
    // The night it was set at: the row exactly.
    CHECK(dayLookScene(dry, spec, 0.f, false) == dry);
    // Into the day: the blended fields move, the held two stay the row's.
    const SceneLook day = dayLookScene(dry, spec, 1.f, false);
    CHECK(day.windMps == dry.windMps);
    CHECK(day.cloudCover == dry.cloudCover);
    CHECK(day.fogDensity == doctest::Approx(spec.fogDensity));
    // Held by day: the day's at every hour, the night included.
    CHECK(dayLookScene(dry, spec, 0.f, true).windMps == std::max(dry.windMps, spec.windMps));
    CHECK(dayLookScene(dry, spec, 0.f, true).cloudCover == doctest::Approx(spec.cloudCover));
    // The rain night is the row either way.
    const SceneLook rain = rainRow();
    CHECK(dayLookScene(rain, spec, 0.f, false) == rain);
    CHECK(dayLookScene(rain, spec, 0.f, true) == rain);
}

TEST_CASE("dayLookScene: full day takes the day's fog, split tone and sun") {
    const DayLookSpec spec;
    const SceneLook d = dayLookScene(dryRow(), spec, 1.f);
    CHECK(d.fogDensity == doctest::Approx(spec.fogDensity));
    CHECK(d.sunKelvin == doctest::Approx(spec.sunKelvin));
    CHECK(d.shadowTintHue == doctest::Approx(spec.shadowTintHue).epsilon(1e-4));
    CHECK(d.shadowTintChroma == doctest::Approx(spec.shadowTintChroma));
    CHECK(d.highlightTintHue == doctest::Approx(spec.highlightTintHue).epsilon(1e-4));
    CHECK(d.highlightTintChroma == doctest::Approx(spec.highlightTintChroma));
    // The wind and the cover hold at every amount.
    CHECK(d.windMps == spec.windMps);
    CHECK(d.cloudCover == spec.cloudCover);
    CHECK(dayLookScene(dryRow(), spec, 0.3f).windMps == spec.windMps);
    // A row's own stronger wind and own cover win.
    SceneLook windy = dryRow();
    windy.windMps = 9.f;
    windy.cloudCover = 0.2f;
    const SceneLook w = dayLookScene(windy, spec, 1.f);
    CHECK(w.windMps == 9.f);
    CHECK(w.cloudCover == 0.2f);
}

TEST_CASE("dayLookScene: the split tone's hue takes the short way and never jumps") {
    DayLookSpec spec;
    SceneLook row = dryRow();
    row.shadowTintHue = 350.f;
    row.shadowTintChroma = 0.02f;
    spec.shadowTintHue = 10.f;
    spec.shadowTintChroma = 0.02f;
    const SceneLook half = dayLookScene(row, spec, 0.5f);
    // Halfway between 350 and 10 the short way is 0 (360), not 180.
    CHECK((half.shadowTintHue < 1.f || half.shadowTintHue > 359.f));
    // From no tint at all the hue is the day's from the first step.
    row.shadowTintChroma = 0.f;
    row.shadowTintHue = 0.f;
    spec.shadowTintHue = 240.f;
    const SceneLook first = dayLookScene(row, spec, 0.01f);
    CHECK(first.shadowTintHue == doctest::Approx(240.f).epsilon(1e-4));
    CHECK(first.shadowTintChroma == doctest::Approx(0.01f * spec.shadowTintChroma));
    // Clamped to the field's range.
    spec.shadowTintChroma = 1.f;
    CHECK(dayLookScene(row, spec, 1.f).shadowTintChroma <= 0.05f + 1e-6f);
}

TEST_CASE("dayLookCloudsOn: the cloud shadows go only when the night is full") {
    CHECK(dayLookCloudsOn(0.f));
    CHECK(dayLookCloudsOn(0.99f));
    CHECK_FALSE(dayLookCloudsOn(1.f));
    CHECK_FALSE(dayLookCloudsOn(std::numeric_limits<float>::quiet_NaN()));
}

TEST_CASE("dayLookSignStrength: the night's strength under full lights, near a sunlit white by day") {
    const DayLookSpec spec;
    CHECK(dayLookSignStrength(spec, 40.f, 6.f, 60.f, 1.f) == 40.f);
    CHECK(dayLookSignStrength(spec, 40.f, 6.f, 60.f, 2.f) == 40.f);
    CHECK(dayLookSignStrength(spec, 40.f, 6.f, -20.f, std::numeric_limits<float>::quiet_NaN()) == 40.f);
    const float white = 6.f * (std::sin(60.f * 3.14159265f / 180.f) + spec.signSkyShare) / 3.14159265f;
    CHECK(dayLookSignStrength(spec, 40.f, 6.f, 60.f, 0.f) == doctest::Approx(spec.signWhiteRatio * white));
    // By day the signs are a few times a white face, not the night's forty.
    CHECK(dayLookSignStrength(spec, 40.f, 6.f, 60.f, 0.f) < 10.f);
    CHECK(dayLookSignStrength(spec, 40.f, 6.f, 60.f, 0.5f) ==
          doctest::Approx(0.5f * (spec.signWhiteRatio * white + 40.f)));
    CHECK(dayLookSignStrength(spec, -5.f, 6.f, 60.f, 1.f) == 0.f);
    CHECK(dayLookSignStrength(spec, 40.f, -6.f, 60.f, 0.f) == 0.f);
}

TEST_CASE("dayLookContactShadow: the light's own at amount 0, the day's at 1") {
    const DayLookSpec spec;
    CHECK(dayLookContactShadow(spec, 0.f, 0.f) == 0.f);
    CHECK(dayLookContactShadow(spec, 0.07f, 0.f) == 0.07f);
    CHECK(dayLookContactShadow(spec, 0.f, std::numeric_limits<float>::quiet_NaN()) == 0.f);
    CHECK(dayLookContactShadow(spec, 0.f, 1.f) == doctest::Approx(spec.contactShadowLength));
    CHECK(dayLookContactShadow(spec, 0.f, 0.5f) == doctest::Approx(0.5f * spec.contactShadowLength));
}

TEST_CASE("day look: dusk at 1-min steps, the amount never rises, the signs stay under the night's, the night is the row's") {
    const WorldClockSpec clock;
    const DayShadeFade fade;
    const DayLookSpec spec;
    const SceneLook row = dryRow();
    float prevAmount = 2.f, sign = 0.f;
    for (int m = 0; m <= 240; ++m) {
        const double h = 16.0 + m / 60.0;
        const float e = sunElevationDeg(clock, h);
        const float night = nightAmount(clock, h);
        const float a = dayShadeAmount(fade, night, e);
        CHECK(a <= prevAmount);
        prevAmount = a;
        const SceneLook l = dayLookScene(row, spec, a);
        // Fog between the row's and the day's, the hue never off the span between them.
        CHECK(l.fogDensity >= std::min(row.fogDensity, spec.fogDensity) - 1e-6f);
        CHECK(l.fogDensity <= std::max(row.fogDensity, spec.fogDensity) + 1e-6f);
        CHECK(l.shadowTintHue >= spec.shadowTintHue - 1e-3f);
        CHECK(l.shadowTintHue <= row.shadowTintHue + 1e-3f);
        sign = dayLookSignStrength(spec, 40.f, 6.f, e, lightsAt(clock, h));
        CHECK(sign >= 0.f);
        CHECK(sign <= 40.f);
        if (a <= 0.f) {
            CHECK(l.fogDensity == row.fogDensity);
            CHECK(l.sunKelvin == row.sunKelvin);
            CHECK(l.shadowTintChroma == row.shadowTintChroma);
        }
    }
    CHECK(prevAmount == 0.f);
    CHECK(sign == 40.f);
}

TEST_CASE("day look: a lever switched off (a negative value) keeps the row's own") {
    DayLookSpec off;
    off.fogDensity = -1.f;
    off.sunKelvin = 0.f;
    off.shadowTintChroma = -1.f;
    off.highlightTintChroma = -1.f;
    off.windMps = -1.f;
    off.cloudCover = -1.f;
    off.contactShadowLength = -1.f;
    off.signWhiteRatio = -1.f;
    off.fogMaxOpacity = -1.f;
    off.shadeStep = 0;
    const SceneLook dry = dryRow();
    CHECK(dayLookScene(dry, off, 1.f) == dry);
    CHECK(dayLookScene(dry, off, 0.5f) == dry);
    CHECK(dayLookContactShadow(off, 0.03f, 1.f) == 0.03f);
    CHECK(dayLookSignStrength(off, 40.f, 6.f, 60.f, 0.f) == 40.f);
    const DayShadeLook def;
    const DayShadeLook s = dayLookShade(def, off);
    CHECK(s.fogMaxOpacity == def.fogMaxOpacity);
    CHECK(s.skyScale == def.skyScale);
    // One lever off leaves the others on.
    DayLookSpec fogOff;
    fogOff.fogDensity = -1.f;
    const SceneLook f = dayLookScene(dry, fogOff, 1.f);
    CHECK(f.fogDensity == dry.fogDensity);
    CHECK(f.sunKelvin == doctest::Approx(fogOff.sunKelvin));
}

TEST_CASE("day look: allocation-free") {
    const DayLookSpec spec;
    const SceneLook row = dryRow();
    const ftcore_test::AllocStats before = ftcore_test::allocStats();
    float sink = 0.f;
    for (int i = 0; i <= 100; ++i) {
        const float a = static_cast<float>(i) / 100.f;
        sink += dayLookScene(row, spec, a).fogDensity;
        sink += dayLookSignStrength(spec, 40.f, 6.f, 30.f, a);
        sink += dayLookContactShadow(spec, 0.f, a);
        sink += dayLookShade(DayShadeLook{}, spec).skyScale;
    }
    const ftcore_test::AllocStats after = ftcore_test::allocStats();
    CHECK(after.count == before.count);
    CHECK(sink > 0.f);
}
