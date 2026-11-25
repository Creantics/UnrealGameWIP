#include "ftcore/RainCover.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

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

namespace {

// The ring HLSL's texel arithmetic, line for line (cm in, ints as HLSL's truncating / and %), to hold it to the C++.
void hlslRingTexel(float pxCm, float pyCm, int res, float texelCm, int& tx, int& ty, int& slot, int& ax, int& ay) {
    const auto floorDiv = [](int a, int b) { return a >= 0 ? a / b : (a - b + 1) / b; };
    const auto posMod3 = [](int a) {
        const int m = a % 3;
        return m < 0 ? m + 3 : m;
    };
    const int gx = static_cast<int>(std::floor(pxCm / texelCm)), gy = static_cast<int>(std::floor(pyCm / texelCm));
    tx = floorDiv(gx, res);
    ty = floorDiv(gy, res);
    const int lx = gx - tx * res, ly = gy - ty * res;
    const int mx = posMod3(tx), my = posMod3(ty);
    slot = mx + 3 * my;
    ax = mx * res + ly;
    ay = my * res + (res - 1) - lx;
}

}  // namespace

TEST_CASE("rainCover ring: slot names, slot vectors and atlas texels") {
    const RainCoverSpec spec;
    CHECK(std::string(rainCoverSlotParam(0)) == "CoverSlot0");
    CHECK(std::string(rainCoverSlotParam(8)) == "CoverSlot8");
    CHECK(std::string(rainCoverSlotParam(9)).empty());
    CHECK(std::string(rainCoverSlotParam(-1)).empty());
    CHECK(rainCoverAtlasSide(spec) == 1536);
    const RainCoverSlot none;
    CHECK(none.tx == kRainCoverSlotNone);  // (0, 0) is a real tile: an unset slot must not read as it
    const RainCoverSlot s = rainCoverSlotFor(RainCoverTile{-1, 4}, RainCoverR16Range{2.5f, 10.f, 7});
    CHECK(s.tx == -1.f);
    CHECK(s.ty == 4.f);
    CHECK(s.minZcm == doctest::Approx(250.f));
    CHECK(s.maxZcm == doctest::Approx(1000.f));

    RainCoverTile t;
    int slot = -1, ax = -1, ay = -1;
    REQUIRE(rainCoverRingTexel(spec, 0.06f, 0.06f, t, slot, ax, ay));
    CHECK(t == RainCoverTile{0, 0});
    CHECK(slot == 0);
    CHECK(ax == 0);
    CHECK(ay == 511);
    // Tile (-1, 4): slot posmod(-1, 3) + 3 * posmod(4, 3) = 2 + 3 = 5, block at (1024, 512).
    REQUIRE(rainCoverRingTexel(spec, -0.01f, 4 * 64.f + 0.01f, t, slot, ax, ay));
    CHECK(t == RainCoverTile{-1, 4});
    CHECK(slot == 5);
    CHECK(ax == 1024 + 0);
    CHECK(ay == 512 + 0);  // x just under 0: the last texel along +X, the image's top row
    int ox = 0, oy = 0;
    rainCoverRingOrigin(spec, 5, ox, oy);
    CHECK(ox == 1024);
    CHECK(oy == 512);
    CHECK_FALSE(rainCoverRingTexel(spec, std::numeric_limits<float>::infinity(), 0.f, t, slot, ax, ay));

    // The HLSL's integer arithmetic lands on the same texel as the C++ across the strip (off texel edges).
    int bad = 0;
    for (int i = 0; i < 4000; ++i) {
        const float x = -832.f + 2816.f * static_cast<float>((i * 7919) % 4000) / 4000.f + 0.031f;
        const float y = -300.f + 900.f * static_cast<float>((i * 104729) % 4000) / 4000.f + 0.047f;
        REQUIRE(rainCoverRingTexel(spec, x, y, t, slot, ax, ay));
        int hx = 0, hy = 0, hs = 0, hax = 0, hay = 0;
        hlslRingTexel(x * 100.f, y * 100.f, spec.resolution, rainCoverTexel(spec) * 100.f, hx, hy, hs, hax, hay);
        if (hx != t.x || hy != t.y || hs != slot || hax != ax || hay != ay) ++bad;
    }
    CHECK(bad == 0);
}

TEST_CASE("rainCover ring: the CPU OpenAt over an atlas") {
    const RainCoverSpec spec;
    const int side = rainCoverAtlasSide(spec);
    std::vector<unsigned short> atlas(static_cast<std::size_t>(side) * static_cast<std::size_t>(side), 0);
    RainCoverSlot slots[9];
    // Tile (2, -1) (slot 2 + 3 * 2 = 8) holds a roof at 6 m over one texel, range 0..12 m.
    const RainCoverTile tile{2, -1};
    const RainCoverR16Range range{0.f, 12.f, 1};
    const float x = 2 * 64.f + 10.03f, y = -64.f + 20.03f;
    RainCoverTile at;
    int slot = 0, ax = 0, ay = 0;
    REQUIRE(rainCoverRingTexel(spec, x, y, at, slot, ax, ay));
    REQUIRE(at == tile);
    REQUIRE(slot == 8);
    atlas[static_cast<std::size_t>(ay) * static_cast<std::size_t>(side) + static_cast<std::size_t>(ax)] =
        static_cast<unsigned short>(1u + 32767u);  // 12 * 32767 / 65534 = 6 m
    // Unset slot: open even with the code in place.
    CHECK(rainCoverRingOpen(spec, atlas.data(), slots, x, y, 0.f, spec.biasM) == 1.f);
    slots[8] = rainCoverSlotFor(tile, range);
    CHECK(rainCoverRingOpen(spec, atlas.data(), slots, x, y, 0.f, spec.biasM) == 0.f);   // under the roof
    CHECK(rainCoverRingOpen(spec, atlas.data(), slots, x, y, 5.8f, spec.biasM) == 1.f);  // within bias of it
    CHECK(rainCoverRingOpen(spec, atlas.data(), slots, x, y, 5.6f, spec.biasM) == 0.f);
    CHECK(rainCoverRingOpen(spec, atlas.data(), slots, x + 0.2f, y, 0.f, spec.biasM) == 1.f);  // next texel: none
    // The slot holding another tile three tiles over (same slot): open there, as the HLSL.
    slots[8] = rainCoverSlotFor(RainCoverTile{5, -1}, range);
    CHECK(rainCoverRingOpen(spec, atlas.data(), slots, x, y, 0.f, spec.biasM) == 1.f);
    CHECK(rainCoverRingOpen(spec, nullptr, slots, x, y, 0.f, spec.biasM) == 1.f);
}

TEST_CASE("rainCover ring: the HLSL is gated by a branch and returns once") {
    const RainCoverSpec spec;
    for (const bool sprite : {true, false}) {
        const std::string h = rainCoverRingHlsl(spec, sprite);
        CHECK(h.find("[branch] if (On >= 0.5) {") != std::string::npos);
        CHECK(h.find("return 1.0;") == std::string::npos);  // no early outs: one return per method
        CHECK(h.find("floor(P / 12.5)") != std::string::npos);
        CHECK(h.find("FloorDiv(g.x, 512)") != std::string::npos);
        CHECK(h.find("(512 - 1)") != std::string::npos);
        CHECK(h.find("C.Sl[0] = CoverSlot0;") != std::string::npos);
        CHECK(h.find("C.Sl[8] = CoverSlot8;") != std::string::npos);
        CHECK(h.find('$') == std::string::npos);
        CHECK(h.find("CoverFrame") == std::string::npos);
        CHECK(h.find(sprite ? "return C.Open(CoverTex, CoverOn, WorldPos);" : "return C.Wet(CoverTex, CoverOn, WorldPos, Normal);") !=
              std::string::npos);
    }
}

TEST_CASE("rainCover tile file v2: a 4096-byte header, payload sector-aligned") {
    RainCoverTileHeader h;
    h.tile = RainCoverTile{-13, 7};
    h.res = 512;
    h.hits = 1234;
    h.topZ = 412.5f;
    h.minZ = -3.25f;
    h.maxZ = 88.f;
    h.key = 0x0123456789abcdefull;
    std::vector<unsigned char> buf(kRainCoverFileHeader, 0xcd);
    rainCoverHeaderWrite(h, buf.data());
    CHECK(buf[0] == 'F');
    CHECK(buf[3] == '2');
    CHECK(buf[kRainCoverFileHeader - 1] == 0);  // padded with zeros
    RainCoverTileHeader r;
    REQUIRE(rainCoverHeaderRead(buf.data(), kRainCoverFileHeader, r));
    CHECK(r.tile == h.tile);
    CHECK(r.res == 512);
    CHECK(r.hits == 1234);
    CHECK(r.topZ == 412.5f);
    CHECK(r.minZ == -3.25f);
    CHECK(r.maxZ == 88.f);
    CHECK(r.key == h.key);
    CHECK(rainCoverPayloadBytes(r) == 524288);
    CHECK(kRainCoverFileHeader % 4096 == 0);
    CHECK_FALSE(rainCoverHeaderRead(buf.data(), kRainCoverFileHeader - 1, r));
    buf[3] = '1';
    CHECK_FALSE(rainCoverHeaderRead(buf.data(), kRainCoverFileHeader, r));
    buf[3] = '2';
    buf[4] = 1;  // version 1
    CHECK_FALSE(rainCoverHeaderRead(buf.data(), kRainCoverFileHeader, r));
    CHECK_FALSE(rainCoverHeaderRead(nullptr, kRainCoverFileHeader, r));

    // Header only: no hits, or full and flat; a partial flat tile or a ranged one carries its payload.
    CHECK(rainCoverTileHeaderOnly(RainCoverR16Range{0.f, 0.f, 0}, 16));
    CHECK(rainCoverTileHeaderOnly(RainCoverR16Range{3.f, 3.f, 16}, 16));
    CHECK_FALSE(rainCoverTileHeaderOnly(RainCoverR16Range{3.f, 3.f, 15}, 16));
    CHECK_FALSE(rainCoverTileHeaderOnly(RainCoverR16Range{3.f, 4.f, 16}, 16));
    RainCoverTileHeader flat;
    flat.hits = 16;
    CHECK(rainCoverPayloadBytes(flat) == 0);
    unsigned short codes[16] = {};
    rainCoverTileFill(flat, 4, codes);
    CHECK(codes[0] == 1u);
    CHECK(codes[15] == 1u);
    flat.hits = 0;
    rainCoverTileFill(flat, 4, codes);
    CHECK(codes[7] == kRainCoverR16None);
}

TEST_CASE("rainCover tile list lines") {
    RainCoverTile t;
    CHECK(rainCoverTileLine("t -13 7", t));
    CHECK(t == RainCoverTile{-13, 7});
    CHECK(rainCoverTileLine("t 0 0\n", t));
    CHECK(rainCoverTileLine("  t 30 -2  \r\n", t));
    CHECK(t == RainCoverTile{30, -2});
    CHECK_FALSE(rainCoverTileLine("# 11-25 strip", t));
    CHECK_FALSE(rainCoverTileLine("t 1", t));
    CHECK_FALSE(rainCoverTileLine("t 1 2 3", t));
    CHECK_FALSE(rainCoverTileLine("", t));
    CHECK_FALSE(rainCoverTileLine(nullptr, t));
}
