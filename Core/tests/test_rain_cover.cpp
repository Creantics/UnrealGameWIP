#include "ftcore/RainCover.h"

#include <doctest/doctest.h>

#include <cmath>
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

TEST_CASE("rainCover tiles (3b): index by floor, frame and the shared texel lattice") {
    const RainCoverSpec spec;
    CHECK(rainCoverTileAt(spec, 0.f, 0.f) == RainCoverTile{0, 0});
    CHECK(rainCoverTileAt(spec, 63.999f, 63.999f) == RainCoverTile{0, 0});
    CHECK(rainCoverTileAt(spec, 64.f, -0.01f) == RainCoverTile{1, -1});
    CHECK(rainCoverTileAt(spec, -64.f, -64.01f) == RainCoverTile{-1, -2});
    CHECK(rainCoverTileAt(spec, -800.f, 1920.f) == RainCoverTile{-13, 30});  // the streets' far ends
    CHECK(rainCoverTileAt(spec, std::numeric_limits<float>::quiet_NaN(), 5.f) == RainCoverTile{0, 0});
    CHECK(rainCoverTileAt(spec, 2.0e6f, 5.f) == RainCoverTile{0, 0});
    const RainCoverFrame f = rainCoverTileFrame(spec, RainCoverTile{-2, 3}, 250.f);
    CHECK(f.cx == doctest::Approx(-96.f));
    CHECK(f.cy == doctest::Approx(224.f));
    CHECK(f.sizeM == 64.f);
    CHECK(f.topZ == 250.f);

    // A tile texel agrees with rainCoverUV on the tile's frame, at every corner texel and one inside, either side of 0.
    const float t = rainCoverTexel(spec);
    for (const RainCoverTile tile : {RainCoverTile{0, 0}, RainCoverTile{-13, 30}, RainCoverTile{-1, -1}}) {
        const RainCoverFrame tf = rainCoverTileFrame(spec, tile, 0.f);
        for (const int i : {0, 1, 255, 511}) {
            for (const int j : {0, 300, 511}) {
                // texel centre i along +X, j along +Y from the tile's low corner
                const float x = static_cast<float>(tile.x) * 64.f + (static_cast<float>(i) + 0.5f) * t;
                const float y = static_cast<float>(tile.y) * 64.f + (static_cast<float>(j) + 0.5f) * t;
                RainCoverTile got;
                int px = -1, py = -1;
                REQUIRE(rainCoverTileTexel(spec, x, y, got, px, py));
                CHECK(got == tile);
                float u = 0.f, v = 0.f;
                REQUIRE(rainCoverUV(tf, x, y, u, v));
                CHECK(px == static_cast<int>(u * 512.f));
                CHECK(py == static_cast<int>(v * 512.f));
            }
        }
    }
    // A snapped square's texel centre (any centre on the 12.5 cm lattice) lands on one tile texel: the four squares that
    // straddle a tile corner map onto four tiles with no gap and no overlap.
    RainCoverTile a, b;
    int ax = 0, ay = 0, bx = 0, by = 0;
    REQUIRE(rainCoverTileTexel(spec, 64.f - 0.5f * t, 128.f - 0.5f * t, a, ax, ay));
    REQUIRE(rainCoverTileTexel(spec, 64.f + 0.5f * t, 128.f + 0.5f * t, b, bx, by));
    CHECK(a == RainCoverTile{0, 1});
    CHECK(b == RainCoverTile{1, 2});
    CHECK(ax == 511);  // +Y edge of tile (0, 1)
    CHECK(ay == 0);    // +X edge: the image's top row
    CHECK(bx == 0);
    CHECK(by == 511);
    RainCoverTile none;
    int nx = 7, ny = 7;
    CHECK_FALSE(rainCoverTileTexel(spec, std::numeric_limits<float>::infinity(), 0.f, none, nx, ny));
    RainCoverSpec empty;
    empty.resolution = 0;
    CHECK_FALSE(rainCoverTileTexel(empty, 1.f, 1.f, none, nx, ny));
}

TEST_CASE("rainCover ring (3b): toroidal slots, and the tiles a move uploads") {
    // The nine tiles round any centre take nine different slots.
    for (const RainCoverTile c : {RainCoverTile{0, 0}, RainCoverTile{-13, 30}, RainCoverTile{-1, -2}}) {
        bool used[9] = {};
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int s = rainCoverRingSlot(RainCoverTile{c.x + dx, c.y + dy});
                REQUIRE(s >= 0);
                REQUIRE(s < 9);
                CHECK_FALSE(used[s]);
                used[s] = true;
            }
    }
    CHECK(rainCoverRingSlot(RainCoverTile{-1, -1}) == 8);
    CHECK(rainCoverRingSlot(RainCoverTile{3, -3}) == 0);

    RainCoverTile in[9];
    CHECK(rainCoverRingEntered(RainCoverTile{4, 4}, RainCoverTile{4, 4}, in) == 0);
    // Across an edge: 3 tiles enter, each into the slot the tile leaving its column frees.
    REQUIRE(rainCoverRingEntered(RainCoverTile{0, 0}, RainCoverTile{1, 0}, in) == 3);
    for (int k = 0; k < 3; ++k) {
        CHECK(in[k].x == 2);
        CHECK(rainCoverRingSlot(in[k]) == rainCoverRingSlot(RainCoverTile{-1, in[k].y}));
    }
    CHECK(rainCoverRingEntered(RainCoverTile{-1, 0}, RainCoverTile{-1, -1}, in) == 3);
    CHECK(rainCoverRingEntered(RainCoverTile{0, 0}, RainCoverTile{1, 1}, in) == 5);  // across a corner
    CHECK(rainCoverRingEntered(RainCoverTile{0, 0}, RainCoverTile{2, 0}, in) == 6);
    CHECK(rainCoverRingEntered(RainCoverTile{0, 0}, RainCoverTile{3, 0}, in) == 9);  // a jump: the whole ring
}

TEST_CASE("rainCover R16 (3b): none is its own code, heights within half a step") {
    CHECK(rainCoverR16Error(0.f) == 0.f);
    CHECK(rainCoverR16Error(200.f) == doctest::Approx(200.f / 131068.f));
    CHECK(rainCoverR16Error(200.f) < 0.002f);   // a 200 m range is inside 2 mm
    CHECK(rainCoverR16Error(263.f) > 0.002f);   // ... 263 m is not
    CHECK(rainCoverR16Error(std::numeric_limits<float>::quiet_NaN()) == 0.f);

    // depths below a top at 250 m; far 200 m: 200 and more, or not finite, is no occluder
    const float far = 200.f;
    const float depth[8] = {0.f, 12.345f, 199.99f, 200.f, 250.f, std::numeric_limits<float>::infinity(), 77.7f,
                            std::numeric_limits<float>::quiet_NaN()};
    unsigned short code[8] = {};
    RainCoverR16Range r;
    CHECK(rainCoverR16Pack(depth, 8, 250.f, far, code, r, 0.002f));
    CHECK(r.hits == 4);
    CHECK(r.minZ == doctest::Approx(50.01f));
    CHECK(r.maxZ == 250.f);
    CHECK(code[0] == kRainCoverR16Top);
    CHECK(code[2] == 1u);
    for (const int i : {3, 4, 5, 7}) {
        CHECK(code[i] == kRainCoverR16None);
        float z = 1.f;
        CHECK_FALSE(rainCoverR16Unpack(code[i], r, z));
    }
    for (const int i : {0, 1, 2, 6}) {
        CHECK(code[i] != kRainCoverR16None);
        float z = 0.f;
        REQUIRE(rainCoverR16Unpack(code[i], r, z));
        CHECK(std::fabs(z - (250.f - depth[i])) <= rainCoverR16Error(r.maxZ - r.minZ) + 1e-4f);
    }
    // One height only (a flat street tile): range 0, every occluder code 1, back exactly.
    const float flat[3] = {40.f, 40.f, 300.f};
    CHECK(rainCoverR16Pack(flat, 3, 10.f, far, code, r, 0.002f));
    CHECK(r.hits == 2);
    CHECK(code[0] == 1u);
    CHECK(code[2] == kRainCoverR16None);
    float z = 0.f;
    REQUIRE(rainCoverR16Unpack(code[1], r, z));
    CHECK(z == -30.f);
    // A range past the tolerance packs but says so; an empty tile has no hits and packs all none.
    const float wide[2] = {0.f, 199.f};
    CHECK_FALSE(rainCoverR16Pack(wide, 2, 300.f, 400.f, code, r, 0.0005f));
    const float empty[2] = {200.f, 500.f};
    CHECK(rainCoverR16Pack(empty, 2, 0.f, far, code, r, 0.002f));
    CHECK(r.hits == 0);
    CHECK(code[0] == kRainCoverR16None);
    CHECK(code[1] == kRainCoverR16None);
    CHECK_FALSE(rainCoverR16Pack(depth, 8, std::numeric_limits<float>::quiet_NaN(), far, code, r, 0.002f));
    CHECK(code[0] == kRainCoverR16None);
    CHECK_FALSE(rainCoverR16Pack(nullptr, 8, 0.f, far, code, r, 0.002f));
}
