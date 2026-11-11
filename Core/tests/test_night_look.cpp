#include <doctest/doctest.h>
#include "ftcore/Backdrop.h"
#include "ftcore/NightLook.h"
#include "ftcore/WorldClock.h"
#include "AllocCounter.h"

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

TEST_CASE("nightFrame: the ground is damp by day and fully wet at night") {
    const NightLook look;
    CHECK(0.6f * nightFrame(look, 0.f).wetShare == doctest::Approx(0.35f).epsilon(0.02));
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
    // A real-time sky, lit by a moon at moonShare, lands at skyFill net.
    CHECK(clock.skyScaleRealTime * clock.sunScale == doctest::Approx(look.skyFill));
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
    CHECK(look.skyFill >= 0.5f);
    CHECK(look.skyFill <= 1.5f);
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
    // The fill shows an unlit facade (dark, not black): the captured night sky is a deep blue that carries little
    // luminance, so the share is over the day's; its tint falls from red to blue, against that blue.
    CHECK(look.skyFill >= 0.5f);
    CHECK(look.skyFill <= 1.5f);
    // A face turned from the moon stays within about two stops of a moonlit one: the fill is many times the moon.
    CHECK(look.skyFill >= 10.f * look.moonShare);
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
