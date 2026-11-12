#include "ftcore/ShopFront.h"
#include "ftcore/WindowInterior.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cstring>
#include <string>

using namespace ftcore;

TEST_CASE("shop fronts: neighbours never share a kind or a picture") {
    for (std::uint32_t seed = 1; seed < 400; ++seed) {
        for (std::uint32_t col = 0; col < 24; ++col) {
            CHECK(shopFrontKind(seed, col) != shopFrontKind(seed, col + 1));
            CHECK(shopFrontPicture(seed, col) != shopFrontPicture(seed, col + 1));
        }
    }
}

TEST_CASE("shop fronts: every kind, picture, riser, door and sign style is used") {
    bool kinds[kShopFrontKinds] = {}, pics[kWindowShops] = {}, risers[5] = {}, doors[4] = {}, styles[kShopSignStyles] = {};
    bool words[kShopWordCount] = {};
    const ShopFrontSpec spec;
    for (std::uint32_t seed = 1; seed < 300; ++seed)
        for (std::uint32_t col = 0; col < 12; ++col) {
            const ShopBay b = shopFrontBay(spec, seed, col);
            kinds[static_cast<int>(b.kind)] = true;
            REQUIRE(b.picture >= kWindowRooms);
            REQUIRE(b.picture < kWindowRooms + kWindowShops);
            pics[b.picture - kWindowRooms] = true;
            risers[static_cast<int>(b.riser)] = true;
            doors[static_cast<int>(b.door)] = true;
            styles[static_cast<int>(b.style)] = true;
            REQUIRE(b.word < kShopWordCount);
            words[b.word] = true;
            const ShopWordGroup g = shopSignGroup(b.kind);
            CHECK(b.word >= g.first);
            CHECK(b.word < g.first + g.count);
            CHECK(b.ink < kShopInkCount);
            CHECK(b.zoom >= spec.zoomLeast);
            CHECK(b.zoom <= 1.0f);
            CHECK(b.shift >= 0.0f);
            CHECK(b.shift + b.zoom <= 1.0001f);
        }
    for (bool k : kinds) CHECK(k);
    for (bool p : pics) CHECK(p);
    for (bool r : risers) CHECK(r);
    for (bool d : doors) CHECK(d);
    for (bool s : styles) CHECK(s);
    for (bool w : words) CHECK(w);
}

TEST_CASE("shop fronts: risers, doors, shutters and sills hang together") {
    const ShopFrontSpec spec;
    for (std::uint32_t seed = 1; seed < 200; ++seed)
        for (std::uint32_t col = 0; col < 10; ++col) {
            const ShopBay b = shopFrontBay(spec, seed, col);
            if (b.kind == ShopFrontKind::Shutter) {
                CHECK(b.riser == ShopRiser::None);
                CHECK(b.door == ShopDoor::None);
                CHECK(b.housing);
                CHECK(b.shutter >= spec.shutterLeast);
                CHECK(b.shutter <= 1.0f);
                CHECK(shopFrontSill(spec, b, 3.0f, 0.5f) == doctest::Approx(0.02f));
                continue;
            }
            CHECK(b.riser != ShopRiser::None);
            CHECK(b.shutter == 0.0f);
            if (b.kind == ShopFrontKind::Stall) {
                CHECK(b.riserHeight == doctest::Approx(spec.counter));
                CHECK(b.door == ShopDoor::None);
            } else {
                CHECK(b.riserHeight >= spec.riserMin);
                CHECK(b.riserHeight <= spec.riserMax);
                CHECK(b.door != ShopDoor::None);
            }
            if (b.kind == ShopFrontKind::Cafe) CHECK_FALSE(b.housing);
            if (b.kind == ShopFrontKind::Clinic) CHECK(b.door == ShopDoor::Middle);
            const ShopSpan d = shopFrontDoorSpan(spec, b, 3.0f);
            if (b.door == ShopDoor::None) {
                CHECK(d.a == 0.0f);
                CHECK(d.b == 0.0f);
                CHECK(shopFrontSill(spec, b, 3.0f, 0.5f) == doctest::Approx(b.riserHeight));
            } else {
                CHECK(d.a >= 0.0f);
                CHECK(d.b <= 1.0001f);
                CHECK((d.b - d.a) * 3.0f == doctest::Approx(spec.doorWidth));
                CHECK(shopFrontSill(spec, b, 3.0f, 0.5f * (d.a + d.b)) == doctest::Approx(0.02f));
                const float out = d.a > 0.5f ? 0.5f * d.a : 0.5f * (1.0f + d.b);
                CHECK(shopFrontSill(spec, b, 3.0f, out) == doctest::Approx(b.riserHeight));
            }
        }
    // A narrow bay's door takes at most 45% of it.
    ShopBay b;
    b.door = ShopDoor::Left;
    const ShopSpan d = shopFrontDoorSpan(spec, b, 1.2f);
    CHECK(d.b - d.a == doctest::Approx(0.45f));
}

TEST_CASE("shop signs: the words are generic capitals and the face sets them") {
    for (std::uint32_t i = 0; i < kShopWordCount; ++i) {
        const char* w = shopSignWord(i);
        const std::size_t n = std::strlen(w);
        CHECK(n >= 3);
        CHECK(n <= kShopWordLetters);
        for (std::size_t c = 0; c < n; ++c) CHECK((w[c] >= 'A' && w[c] <= 'Z'));
    }
    std::uint32_t covered = 0;
    for (std::uint32_t k = 0; k < kShopFrontKinds; ++k) {
        const ShopWordGroup g = shopSignGroup(static_cast<ShopFrontKind>(k));
        CHECK(g.first == covered);
        CHECK(g.count > 0);
        covered += g.count;
    }
    CHECK(covered == kShopWordCount);
    // T: a full top row, then the middle column.
    for (std::uint32_t x = 0; x < 5; ++x) CHECK(shopGlyphInk('T', x, 0));
    for (std::uint32_t y = 1; y < 7; ++y) {
        CHECK(shopGlyphInk('T', 2, y));
        CHECK_FALSE(shopGlyphInk('T', 0, y));
    }
    CHECK_FALSE(shopGlyphInk(' ', 2, 2));
    CHECK_FALSE(shopGlyphInk('A', 5, 0));
    // Every letter has ink, and no two letters look the same.
    for (char a = 'A'; a <= 'Z'; ++a) {
        int ink = 0;
        for (std::uint32_t y = 0; y < 7; ++y)
            for (std::uint32_t x = 0; x < 5; ++x) ink += shopGlyphInk(a, x, y) ? 1 : 0;
        CHECK(ink > 5);
        for (char b = static_cast<char>(a + 1); b <= 'Z'; ++b) {
            bool same = true;
            for (std::uint32_t y = 0; y < 7 && same; ++y)
                for (std::uint32_t x = 0; x < 5 && same; ++x) same = shopGlyphInk(a, x, y) == shopGlyphInk(b, x, y);
            CHECK_FALSE(same);
        }
    }
}

TEST_CASE("shop signs: every style inks part of its panel, never all of it") {
    for (std::uint32_t s = 0; s < kShopSignStyles; ++s)
        for (std::uint32_t w = 0; w < kShopWordCount; w += 3) {
            ShopBay b;
            b.style = static_cast<ShopSignStyle>(s);
            b.word = w;
            b.ink = w % kShopInkCount;
            int ink = 0, all = 0;
            for (int y = 0; y < 30; ++y)
                for (int x = 0; x < 200; ++x, ++all)
                    ink += shopSignInk(b, 3.0f, 0.36f, (x + 0.5f) / 200.0f, (y + 0.5f) / 30.0f) ? 1 : 0;
            CHECK(ink > all / 50);
            CHECK(ink < all / 2);
        }
    // TEA's first letter: the T's bar at the top of the type, its stem under the bar's middle.
    ShopBay b;
    b.style = ShopSignStyle::BoardWord;
    b.word = 10;
    REQUIRE(std::strcmp(shopSignWord(b.word), "TEA") == 0);
    const float w = 3.0f, h = 0.36f, p = h / 9.0f, tw = 17.0f;
    const float x0 = 0.5f * (w - tw * p), y0 = 0.5f * (h - 7.0f * p);
    CHECK(shopSignInk(b, w, h, (x0 + 0.5f * p) / w, (y0 + 6.5f * p) / h));
    CHECK(shopSignInk(b, w, h, (x0 + 2.5f * p) / w, (y0 + 0.5f * p) / h));
    CHECK_FALSE(shopSignInk(b, w, h, (x0 + 0.5f * p) / w, (y0 + 0.5f * p) / h));
}

TEST_CASE("shop fronts: the choices allocate nothing") {
    const ShopFrontSpec spec;
    const auto before = ftcore_test::allocStats().count;
    float sum = 0.0f;
    for (std::uint32_t col = 0; col < 64; ++col) {
        const ShopBay b = shopFrontBay(spec, 7u, col);
        sum += shopFrontSill(spec, b, 3.0f, 0.3f) + (shopSignInk(b, 3.0f, 0.36f, 0.4f, 0.5f) ? 1.0f : 0.0f);
    }
    CHECK(sum > 0.0f);
    CHECK(ftcore_test::allocStats().count == before);
}

TEST_CASE("shop fronts: the HLSL fragments keep their contract") {
    const std::string pre = shopFrontPreHlsl(ShopFrontSpec{});
    const std::string main = shopFrontHlsl(ShopFrontSpec{}, 0.35f);
    for (const char* name : {"uint kind =", "uint kh =", "float shopSill =", "uint shopPick =", "float shopZoom =",
                             "float shopShift =", "struct FTShop", "FTShop SF;"})
        CHECK(pre.find(name) != std::string::npos);
    for (const char* name : {"float3 own =", "float sp =", "float3 fc =", "bool panel =", "float3 fasciaRgb =",
                             "float3 signLight =", "part = lantern ? 12.0 : part;", "kGlyph[26]", "kWord[30]"})
        CHECK(main.find(name) != std::string::npos);
    for (const std::string* s : {&pre, &main}) {
        int depth = 0, paren = 0;
        for (char c : *s) {
            depth += c == '{' ? 1 : (c == '}' ? -1 : 0);
            paren += c == '(' ? 1 : (c == ')' ? -1 : 0);
            CHECK(depth >= 0);
        }
        CHECK(depth == 0);
        CHECK(paren == 0);
        CHECK(s->find("nan") == std::string::npos);
        CHECK(s->find("inf") == std::string::npos);
    }
    CHECK(shopFrontHlsl(ShopFrontSpec{}, 0.35f) == main);
    CHECK(shopFrontPreHlsl(ShopFrontSpec{}) == pre);
    // No sign word is spelt out in the HLSL (packed only), and no fragment names a real shop.
    CHECK(main.find("NOODLES") == std::string::npos);
}
