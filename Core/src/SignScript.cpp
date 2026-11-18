#include "ftcore/SignScript.h"

#include <cstdint>

namespace ftcore {

namespace {

// Each stroke joins two points of the 3 x 3 grid, written as column, row, column, row (0..2; row 0 is the headline).
struct GlyphDef {
    std::uint8_t n;
    std::uint8_t s[kSignMaxStrokesPerGlyph][4];
};
// a-z. Every letter differs (test_sign_script checks): stems left (L), centre (C) and right (R), the mid bar (M), the
// foot (B), the two diagonals (D \, E /) and short or bent pieces.
constexpr GlyphDef kGlyphs[26] = {
    {2, {{1, 0, 1, 2}, {0, 2, 2, 2}}},              // a: C B
    {2, {{0, 0, 0, 2}, {0, 1, 2, 1}}},              // b: L M
    {2, {{2, 0, 2, 2}, {0, 2, 2, 2}}},              // c: R B
    {2, {{0, 0, 0, 2}, {0, 0, 2, 2}}},              // d: L D
    {2, {{1, 0, 1, 1}, {0, 1, 2, 1}}},              // e: short C, M
    {1, {{2, 0, 0, 2}}},                            // f: E
    {2, {{0, 0, 0, 2}, {2, 0, 2, 2}}},              // g: L R
    {2, {{1, 0, 1, 2}, {0, 1, 2, 1}}},              // h: C M
    {1, {{1, 0, 1, 2}}},                            // i: C
    {2, {{0, 0, 0, 1}, {0, 2, 2, 2}}},              // j: short L, B
    {2, {{0, 0, 0, 2}, {0, 1, 2, 2}}},              // k: L, kick
    {1, {{0, 0, 0, 2}}},                            // l: L
    {3, {{0, 0, 0, 2}, {1, 0, 1, 2}, {2, 0, 2, 2}}}, // m: L C R
    {2, {{2, 0, 2, 2}, {0, 1, 2, 1}}},              // n: R M
    {3, {{0, 0, 0, 2}, {2, 0, 2, 2}, {0, 2, 2, 2}}}, // o: L R B
    {2, {{0, 0, 1, 2}, {1, 2, 2, 0}}},              // p: V
    {3, {{0, 0, 1, 2}, {1, 2, 2, 0}, {0, 1, 2, 1}}}, // q: V M
    {2, {{2, 0, 2, 2}, {2, 1, 0, 2}}},              // r: R, kick back
    {2, {{0, 0, 2, 2}, {0, 1, 2, 1}}},              // s: D M
    {2, {{2, 0, 0, 2}, {0, 1, 2, 1}}},              // t: E M
    {2, {{0, 0, 0, 2}, {0, 2, 2, 2}}},              // u: L B
    {2, {{2, 0, 2, 2}, {0, 0, 0, 1}}},              // v: R, short L
    {2, {{0, 0, 2, 2}, {2, 0, 0, 2}}},              // w: D E
    {2, {{0, 1, 1, 2}, {1, 2, 2, 1}}},              // x: low V
    {2, {{1, 0, 1, 2}, {2, 1, 1, 2}}},              // y: C, hook
    {2, {{0, 2, 2, 2}, {2, 0, 2, 1}}},              // z: B, short R
};

int letterIndex(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= 'A' && c <= 'Z') return c - 'A';
    return -1;
}

} // namespace

std::size_t signGlyphStrokes(char letter, SignStroke* out) {
    const int i = letterIndex(letter);
    if (i < 0 || !out) return 0;
    const GlyphDef& g = kGlyphs[i];
    const float step = kSignGlyphW * 0.5f;
    for (std::uint8_t k = 0; k < g.n; ++k)
        out[k] = SignStroke{g.s[k][0] * step, g.s[k][1] * 0.5f, g.s[k][2] * step, g.s[k][3] * 0.5f};
    return g.n;
}

std::size_t signScriptStrokes(std::string_view word, SignStroke* out, std::size_t cap, float* width) {
    std::size_t count = 0;
    float x = 0.f, right = 0.f;
    float runStart = 0.f;
    bool inRun = false;
    auto put = [&](const SignStroke& s) {
        if (out && count < cap) out[count] = s;
        if (count < cap) ++count;
    };
    auto closeRun = [&]() {
        if (inRun) put(SignStroke{runStart, 0.f, right, 0.f});
        inRun = false;
    };
    // Headlines first: one per run of letters.
    for (char c : word) {
        if (letterIndex(c) >= 0) {
            if (!inRun) {
                runStart = x;
                inRun = true;
            }
            right = x + kSignGlyphW;
            x += kSignGlyphW + kSignGap;
        } else if (c == ' ') {
            closeRun();
            x += kSignSpace;
        }
    }
    closeRun();
    // Then the letters.
    x = 0.f;
    for (char c : word) {
        SignStroke g[kSignMaxStrokesPerGlyph];
        const std::size_t n = signGlyphStrokes(c, g);
        if (n > 0) {
            for (std::size_t k = 0; k < n; ++k) put(SignStroke{g[k].x0 + x, g[k].y0, g[k].x1 + x, g[k].y1});
            x += kSignGlyphW + kSignGap;
        } else if (c == ' ') {
            x += kSignSpace;
        }
    }
    if (width) *width = right;
    return count;
}

} // namespace ftcore
