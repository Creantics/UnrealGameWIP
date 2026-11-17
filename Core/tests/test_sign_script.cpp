#include "ftcore/SignScript.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

using namespace ftcore;

TEST_CASE("sign script: every letter has its own strokes, inside its box, either case") {
    std::vector<std::vector<float>> seen;
    for (char c = 'a'; c <= 'z'; ++c) {
        SignStroke s[kSignMaxStrokesPerGlyph];
        const std::size_t n = signGlyphStrokes(c, s);
        REQUIRE(n >= 1);
        REQUIRE(n <= kSignMaxStrokesPerGlyph);
        std::vector<std::vector<float>> strokes;
        for (std::size_t k = 0; k < n; ++k) {
            CHECK(std::min(s[k].x0, s[k].x1) >= 0.f);
            CHECK(std::max(s[k].x0, s[k].x1) <= kSignGlyphW + 1e-5f);
            CHECK(std::min(s[k].y0, s[k].y1) >= 0.f);
            CHECK(std::max(s[k].y0, s[k].y1) <= 1.f);
            // An undirected stroke, for comparing sets.
            const bool flip = s[k].x0 > s[k].x1 || (s[k].x0 == s[k].x1 && s[k].y0 > s[k].y1);
            strokes.push_back(flip ? std::vector<float>{s[k].x1, s[k].y1, s[k].x0, s[k].y0}
                                   : std::vector<float>{s[k].x0, s[k].y0, s[k].x1, s[k].y1});
        }
        std::sort(strokes.begin(), strokes.end());
        std::vector<float> key;
        for (const auto& v : strokes) key.insert(key.end(), v.begin(), v.end());
        CHECK(std::find(seen.begin(), seen.end(), key) == seen.end());
        seen.push_back(key);
        SignStroke u[kSignMaxStrokesPerGlyph];
        CHECK(signGlyphStrokes(static_cast<char>(c - 32), u) == n); // the capital
    }
    SignStroke s[kSignMaxStrokesPerGlyph];
    CHECK(signGlyphStrokes('3', s) == 0);
    CHECK(signGlyphStrokes('-', s) == 0);
}

TEST_CASE("sign script: a word lays out under one headline per run, letters advancing, without allocating") {
    SignStroke out[64];
    float w = 0.f;
    const bool hooked = ftcore_test::allocHookInstalled();
    const std::uint64_t before = ftcore_test::allocStats().count;
    const std::size_t n = signScriptStrokes("Tavren", out, 64, &w);
    if (hooked) CHECK(ftcore_test::allocStats().count == before);
    // t 2 + a 2 + v 2 + r 2 + e 2 + n 2, and one headline.
    CHECK(n == 13);
    CHECK(out[0].y0 == 0.f);
    CHECK(out[0].y1 == 0.f);
    CHECK(out[0].x0 == doctest::Approx(0.f));
    CHECK(out[0].x1 == doctest::Approx(6.f * kSignGlyphW + 5.f * kSignGap));
    CHECK(w == doctest::Approx(out[0].x1));
    // The second letter's strokes start one advance in.
    CHECK(std::min(out[3].x0, out[3].x1) >= kSignGlyphW + kSignGap - 1e-5f);
    // Two words: two headlines.
    const std::size_t two = signScriptStrokes("ab cd", out, 64, &w);
    CHECK(two == 2 + 8);
    CHECK(out[1].x0 == doctest::Approx(2.f * (kSignGlyphW + kSignGap) + kSignSpace));
    // The cap holds.
    CHECK(signScriptStrokes("Tavren", out, 4, nullptr) == 4);
    CHECK(signScriptStrokes("", out, 64, &w) == 0);
    CHECK(w == 0.f);
    CHECK(signScriptStrokes("Ta-v", nullptr, 0, nullptr) == 0);
}
