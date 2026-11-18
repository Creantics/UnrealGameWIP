#include "ftcore/RainCover.h"

#include <doctest/doctest.h>

#include <limits>
#include <string>

using namespace ftcore;

TEST_CASE("rainCover: off by default, on only with -RainCover; -NoRainCover wins") {
    CHECK(std::string(kRainCoverSwitch) == "RainCover");
    CHECK(std::string(kRainCoverOffSwitch) == "NoRainCover");
    CHECK_FALSE(rainCoverEnabled(false, false));  // no switch: off (no capture, no term, no assets made)
    CHECK(rainCoverEnabled(true, false));
    CHECK_FALSE(rainCoverEnabled(true, true));
    CHECK_FALSE(rainCoverEnabled(false, true));
    // Off, an emitter marked "cover" draws with its plain material (as before the cover).
    CHECK_FALSE(rainCoverEmitter(true, rainCoverEnabled(false, false)));
    CHECK(rainCoverEmitter(true, rainCoverEnabled(true, false)));
    CHECK_FALSE(rainCoverEmitter(false, true));
    // A material built with the term reads open everywhere with no capture (CoverOn 0): sprite and surface alike.
    const RainCoverSpec spec;
    const std::string none = "if (On < 0.5) return 1.0;";
    CHECK(rainCoverHlsl(spec, true).find(none) != std::string::npos);
    CHECK(rainCoverHlsl(spec, false).find(none) != std::string::npos);
}

TEST_CASE("rainCover: texel, snap and recapture") {
    const RainCoverSpec spec;
    CHECK(rainCoverTexel(spec) == doctest::Approx(0.125f));
    float cx = 0.f, cy = 0.f;
    rainCoverSnap(spec, 10.06f, -3.30f, cx, cy);
    CHECK(cx == doctest::Approx(10.0f));
    CHECK(cy == doctest::Approx(-3.25f));
    RainCoverSpec none;
    none.resolution = 0;
    CHECK(rainCoverTexel(none) == 0.f);
    rainCoverSnap(none, 1.3f, 2.7f, cx, cy);
    CHECK(cx == doctest::Approx(1.3f));

    CHECK(rainCoverRecapture(spec, false, 0.f, 0.f, 0.f, 0.f));
    CHECK_FALSE(rainCoverRecapture(spec, true, 0.f, 0.f, 3.f, 3.9f));  // 4.92 m
    CHECK(rainCoverRecapture(spec, true, 0.f, 0.f, 3.f, 4.f));         // 5 m
    CHECK(rainCoverRecapture(spec, true, 0.f, 0.f, 1e30f, 1e30f));    // not finite squared: recapture
}

TEST_CASE("rainCover: frame and UV (u along +Y, v along -X)") {
    const RainCoverSpec spec;
    const RainCoverFrame f = rainCoverFrameAt(spec, 100.f, 200.f, 5.f);
    CHECK(f.topZ == doctest::Approx(65.f));
    CHECK(f.sizeM == doctest::Approx(64.f));
    float u = 0.f, v = 0.f;
    CHECK(rainCoverUV(f, 100.f, 200.f, u, v));
    CHECK(u == doctest::Approx(0.5f));
    CHECK(v == doctest::Approx(0.5f));
    CHECK(rainCoverUV(f, 100.f, 216.f, u, v));
    CHECK(u == doctest::Approx(0.75f));
    CHECK(rainCoverUV(f, 116.f, 200.f, u, v));
    CHECK(v == doctest::Approx(0.25f));
    CHECK_FALSE(rainCoverUV(f, 100.f, 233.f, u, v));
    CHECK_FALSE(rainCoverUV(RainCoverFrame{}, 0.f, 0.f, u, v));
}

TEST_CASE("rainCover: openness and the bias") {
    CHECK(rainCoverOpen(3.5f, 0.f, 0.3f) == 0.f);   // under an awning at 3.5 m
    CHECK(rainCoverOpen(0.2f, 0.f, 0.3f) == 1.f);   // a kerb's lip
    CHECK(rainCoverOpen(0.f, 0.f, 0.3f) == 1.f);    // the ground itself
    CHECK(rainCoverOpen(-5.f, 0.f, 0.3f) == 1.f);   // nothing over it
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(rainCoverOpen(inf, 0.f, 0.3f) == 1.f);
}

TEST_CASE("rainCover: the fringe blows in at the open side") {
    CHECK(rainCoverFringeWeight(0.25f, 0.75f) == doctest::Approx(0.9167f).epsilon(0.001));
    CHECK(rainCoverFringeWeight(0.5f, 0.75f) == doctest::Approx(0.5833f).epsilon(0.001));
    CHECK(rainCoverFringeWeight(0.75f, 0.75f) == doctest::Approx(0.25f));
    CHECK(rainCoverFringeWeight(2.f, 0.75f) == 0.f);
    CHECK(rainCoverFringeWeight(0.f, 0.f) == 1.f);
    CHECK(rainCoverFringeWeight(0.1f, 0.f) == 0.f);

    float ring[12] = {};
    CHECK(rainCoverWet(1.f, ring, 0.75f) == 1.f);       // open: wet
    CHECK(rainCoverWet(0.f, ring, 0.75f) == 0.f);       // deep under cover: dry
    ring[1] = 1.f;                                      // the nearest ring open on -X: just inside the edge
    CHECK(rainCoverWet(0.f, ring, 0.75f) == doctest::Approx(0.9167f).epsilon(0.001));
    float far[12] = {};
    far[11] = 1.f;                                      // only the outer ring open, on -Y
    CHECK(rainCoverWet(0.f, far, 0.75f) == doctest::Approx(0.25f));
    // Deeper in, wetter never: the gradient falls with depth.
    float mid[12] = {};
    mid[6] = 1.f;
    CHECK(rainCoverWet(0.f, mid, 0.75f) < rainCoverWet(0.f, ring, 0.75f));
    CHECK(rainCoverWet(0.f, mid, 0.75f) > rainCoverWet(0.f, far, 0.75f));
}

TEST_CASE("rainCover: a wall reads the cover out from its face") {
    const RainCoverSpec spec;
    float x = 0.f, y = 0.f;
    rainCoverReadPoint(spec, 10.f, 20.f, 0.f, 0.f, 1.f, x, y);  // a floor: in place
    CHECK(x == 10.f);
    CHECK(y == 20.f);
    rainCoverReadPoint(spec, 10.f, 20.f, -1.f, 0.f, 0.f, x, y);  // a wall facing -X
    CHECK(x == doctest::Approx(9.6f));
    CHECK(y == doctest::Approx(20.f));
    rainCoverReadPoint(spec, 0.f, 0.f, 0.6f, 0.f, 0.8f, x, y);  // a slope: by its horizontal share
    CHECK(x == doctest::Approx(0.24f));
}

TEST_CASE("rainCover: the HLSL") {
    const RainCoverSpec spec;
    const std::string surf = rainCoverHlsl(spec, false);
    const std::string sprite = rainCoverHlsl(spec, true);
    CHECK(surf.find("struct FTRainCover") != std::string::npos);
    CHECK(surf.find('$') == std::string::npos);
    CHECK(sprite.find('$') == std::string::npos);
    CHECK(surf.find("20000.0") != std::string::npos);  // depth 200 m
    CHECK(surf.find("W.z, 30.0)") != std::string::npos);
    CHECK(sprite.find("W.z, 5.0)") != std::string::npos);
    CHECK(surf.find("75.0 * float(k / 4 + 1)") != std::string::npos);
    CHECK(surf.find("static") == std::string::npos);
    CHECK(surf.find("return C.Wet(") != std::string::npos);
    CHECK(sprite.find("return C.Open(") != std::string::npos);
}
