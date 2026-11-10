#include <doctest/doctest.h>
#include "ftcore/Backdrop.h"
#include "ftcore/NightLook.h"
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
    // Warm glow, cool sky: the haze reads as the city's, the fill as the night's.
    CHECK(look.glow[0] > look.glow[2]);
    CHECK(look.skyTint[2] > look.skyTint[0]);
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
