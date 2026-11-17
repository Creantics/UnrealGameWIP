#pragma once
// The player's text scale on the HUD (2026-11-16 order, the item after (A)+(B)): the party names, the objective, a
// notice and the boss's name grow with Settings::textScale (100..200%) by the menus' rules. Text is never shrunk to fit:
// each slot's size is its 1080p size times the text scale (and the viewport), whatever its words. A line wider than its
// slot's room breaks at the space nearest its middle; one that still does not fit in the slot's lines shows a window of
// them (a page of `lines` lines at a time, turning every kUiHudPageTime s). A name never breaks: it stands whole and the
// fit says it ran over its room. A word wider than the room stands alone on its line, whole.
//
//   px = uiHudTextPx(slot, viewportHeight, textScale);           once per font build
//   n  = uiHudWords(text, spans, cap);                           once per text change; the host measures each span at px
//   uiHudFit(words, n, spaceWidth, room * h/1080, rule.lines, fit); then each tick: first = uiHudPageLine(fit, age)
//
// Pure, no allocation; the host measures (Core holds no font).
#include "ftcore/Api.h"
#include "ftcore/UiExplore.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ftcore {

enum class UiHudSlot : std::uint8_t { LeadName, PartnerName, Objective, Notice, BossName };
inline constexpr std::size_t kUiHudSlotCount = 5;

// A slot's 1080p size at 100% (px to the em, the HUD's Hud:: sizes), the width its words may run (px at 1080p, the same
// at every text scale: the screen does not grow with the words) and the lines it shows at once (1: never breaks).
struct UiHudSlotRule {
    float px = 0.f;
    float room = 0.f;
    std::uint8_t lines = 1;
};

// LeadName, PartnerName: the party stack's names over their lines (never broken; a name is a name).
// Objective: under the stack, two lines at most on screen.
// Notice: at the right edge, right-aligned, two lines.
// BossName: over the top line's left end; its room leaves the percent (right-aligned at the line's end, 200%) clear.
inline constexpr std::array<UiHudSlotRule, kUiHudSlotCount> kUiHudSlots{{
    {38.f, 560.f, 1},
    {29.f, 560.f, 1},
    {29.f, 720.f, 2},
    {29.f, 560.f, 2},
    {29.f, 600.f, 2},
}};

inline constexpr float kUiHudMinPx = 14.f;     // no HUD text under this at any viewport (FontPx's floor)
inline constexpr float kUiHudPageTime = 2.5f;  // s a window's page stands before the next
inline constexpr std::size_t kUiHudMaxWords = 32; // spans a text splits into; past it the last span runs to the end
inline constexpr std::size_t kUiHudMaxLines = 6;  // lines a fit holds; past it the rest joins the last line

FTCORE_API const UiHudSlotRule& uiHudSlotRule(UiHudSlot slot);

// The slot's px to the em in a viewport `height` px tall at the player's text scale (clamped to 1..2): never below the
// slot's own size at 100% in that viewport, never below kUiHudMinPx.
FTCORE_API float uiHudTextPx(UiHudSlot slot, float height, float textScale);

// A word: its UTF-16 span [begin, end) in the host's string and its width at the slot's size (the host measures).
struct UiHudWord {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    float w = 0.f;
};

// Splits `text` at spaces (runs of them are one gap; leading and trailing ones belong to no word) into at most `cap`
// spans written to `out` (their w left 0); with more words, the last span runs to the text's last word's end. Returns
// the count.
FTCORE_API std::size_t uiHudWords(std::u16string_view text, UiHudWord* out, std::size_t cap);

struct UiHudFit {
    std::array<UiSubtitleLine, kUiHudMaxLines> line{}; // each line's UTF-16 span (its first word's begin, last word's end)
    std::array<float, kUiHudMaxLines> w{};             // each line's width (for a right-aligned notice)
    std::uint8_t count = 0;  // lines the text broke into
    std::uint8_t shown = 0;  // lines on screen at once (a page): min(count, the slot's lines)
    float widest = 0.f;      // the widest line
    bool over = false;       // a line runs past the room (a name, or a word wider than the room): never shrunk
};

// The fit of `n` measured words, `spaceW` px a gap, in `room` px (the slot's room in the viewport's px) with `lines` on
// screen. One line when it fits or the slot never breaks; else two lines broken at the gap nearest the middle when both
// fit and the slot shows two; else greedy lines of at most `room` (a page of `lines` at a time).
FTCORE_API void uiHudFit(const UiHudWord* words, std::size_t n, float spaceW, float room, std::uint8_t lines, UiHudFit& out);

// Pages the fit shows (1 when every line is on screen), and the first line of the page standing `age` s after the text
// changed (pages turn every kUiHudPageTime s, from the first again after the last).
FTCORE_API std::size_t uiHudPages(const UiHudFit& fit);
FTCORE_API std::size_t uiHudPageLine(const UiHudFit& fit, float age);

} // namespace ftcore
