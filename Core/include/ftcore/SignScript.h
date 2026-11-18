// Solhaven's sign script: an invented lettering drawn from strokes, with no font and no real script behind it. A sign
// form (the Latin reading the names table gives, "Tavren") is laid out letter by letter under one hanging headline,
// each letter a fixed set of one to three strokes on a 3 x 3 point grid. The map draws it small above district names.
//
// Units: a glyph box is kSignGlyphW wide and 1 high, y down from the headline at y 0; letters advance kSignGlyphW +
// kSignGap; a space advances kSignSpace. Zero allocation.
#pragma once

#include "ftcore/Api.h"

#include <cstddef>
#include <string_view>

namespace ftcore {

struct SignStroke {
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
};
inline constexpr float kSignGlyphW = 0.6f;
inline constexpr float kSignGap = 0.22f;
inline constexpr float kSignSpace = 0.5f;
inline constexpr std::size_t kSignMaxStrokesPerGlyph = 3;

// The strokes of one letter (a-z, either case) into out (room for kSignMaxStrokesPerGlyph), in the glyph's own box,
// the headline left out. Returns the count; 0 for anything that is not a letter.
FTCORE_API std::size_t signGlyphStrokes(char letter, SignStroke* out);
// The word's strokes into out (at most cap): one headline per run of letters first, then each letter's strokes, placed.
// Characters other than letters and spaces are skipped. width gets the laid-out width. Returns the count.
FTCORE_API std::size_t signScriptStrokes(std::string_view word, SignStroke* out, std::size_t cap, float* width = nullptr);

} // namespace ftcore
