#pragma once
// The explore HUD's timing and the subtitle's fit (2026-11-16 ruling, item A): the objective line that quiets to 40%
// after 4 s without change, the notices shown one at a time at the right edge (2.5 s each, a queue of 3), the
// subtitle's breaks (42 characters a line, 2 lines on screen, a longer line in pages), the band behind it (black, the
// least up to 70% that holds 4.5:1 over the picture, none where the bare text already does) and the measured guard of a
// small line with no plate (its soft contour raised in strength, then a shade under it, until it holds its target).
//
//   UiObjectiveFade f;   each tick: uiObjectiveStep(f, objective serial, dt); draw at f.alpha
//   UiNoticeQueue q;     on a notice: slot = uiNoticePush(q), the host keeps the words in its own slot array
//                        each tick: uiNoticeStep(q, dt); draw slot uiNoticeShown(q) at uiNoticeAlpha(q)
//   UiSubtitleFit fit;   once per line: uiSubtitleFit(text, fit); each tick: page = uiSubtitlePage(fit, revealed)
//
// Pure, no allocation; the host feeds what the director reports.
#include "ftcore/Api.h"
#include "ftcore/UiStyle.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ftcore {

// --- Objective --------------------------------------------------------------------------------------------------

inline constexpr float kUiObjectiveHold = 4.f;   // s at full before it quiets
inline constexpr float kUiObjectiveQuiet = 0.4f; // its alpha once quiet
inline constexpr float kUiObjectiveEase = 0.6f;  // s to ease down to quiet
inline constexpr float kUiObjectiveRise = 0.2f;  // s to come back up on a change

struct UiObjectiveFade {
    std::uint32_t serial = 0; // the objective last seen (any change of it restarts the hold)
    bool seen = false;
    float age = 0.f;          // s since the last change
    float alpha = 0.f;        // the line's alpha now
};

// One tick: `serial` is anything that changes when the objective's words do (its index, a counter).
FTCORE_API void uiObjectiveStep(UiObjectiveFade& f, std::uint32_t serial, float dt);

// --- Notices ----------------------------------------------------------------------------------------------------

inline constexpr std::size_t kUiNoticeQueue = 3;  // the one shown and the ones waiting, together
// A full queue hands its last waiting slot to the new notice; with one slot that would be the one on screen.
static_assert(kUiNoticeQueue >= 2, "the notice on screen always finishes: one slot shown, at least one waiting");
inline constexpr float kUiNoticeTime = 2.5f;      // s each notice stands, its fades inside
inline constexpr float kUiNoticeFadeIn = 0.15f;
inline constexpr float kUiNoticeFadeOut = 0.35f;

struct UiNoticeQueue {
    std::array<std::uint8_t, kUiNoticeQueue> order{}; // slots, the shown one first
    std::uint8_t count = 0;
    float age = 0.f;                                   // s the first has stood
};

// A new notice: returns the slot (0..kUiNoticeQueue-1) whose words the host sets now. With the queue full, the last
// waiting notice gives its slot to the new one (the one on screen always finishes).
FTCORE_API std::uint8_t uiNoticePush(UiNoticeQueue& q);
FTCORE_API void uiNoticeStep(UiNoticeQueue& q, float dt);
// The slot on screen, or -1.
FTCORE_API int uiNoticeShown(const UiNoticeQueue& q);
FTCORE_API float uiNoticeAlpha(const UiNoticeQueue& q);

// --- Subtitle ---------------------------------------------------------------------------------------------------

inline constexpr std::size_t kUiSubtitleChars = 42;   // characters a line, at most
inline constexpr std::size_t kUiSubtitleLines = 2;    // lines on screen at once (a page)
inline constexpr std::size_t kUiSubtitleMaxLines = 8; // lines a beat may break into (four pages); the rest joins the last

// A line's text in UTF-16 units [begin, end), as the host's string holds it.
struct UiSubtitleLine {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
};

struct UiSubtitleFit {
    std::array<UiSubtitleLine, kUiSubtitleMaxLines> line{};
    std::uint8_t count = 0;
};

// Greedy breaks at spaces and newlines, at most kUiSubtitleChars characters (code points) a line; a word longer than a
// line stands alone on its own line, whole. Spaces at a break belong to no line.
FTCORE_API void uiSubtitleFit(std::u16string_view text, UiSubtitleFit& out);
// The page (pairs of lines) to show while the reveal stands at UTF-16 unit `revealed`: the page holding its last
// revealed character.
FTCORE_API std::size_t uiSubtitlePage(const UiSubtitleFit& fit, std::uint32_t revealed);
FTCORE_API std::size_t uiSubtitlePages(const UiSubtitleFit& fit);

inline constexpr float kUiSubtitleMinContrast = 4.5f; // the text over the bare picture below this: the band comes in
inline constexpr float kUiSubtitleBandMax = 0.70f;    // the band's black at its middle, at most (2026-11-16: cap lifted)

// The band behind a subtitle line: the least black (0..kUiSubtitleBandMax) under which `ink` stands at
// kUiSubtitleMinContrast over `picture` (the brightest tenth under the line); 0 when the bare text already does, the cap
// when even that falls short. `ratio` (optional) gets the bare contrast, `withBand` the contrast over the band returned.
FTCORE_API float uiSubtitleBand(UiColor ink, UiColor picture, float* ratio = nullptr, float* withBand = nullptr);

// --- A small line's guard (no plate) ----------------------------------------------------------------------------
//
// The objective, a notice, the prompt's verb and the party names stand on the picture over their own soft contour: the
// font's dark edge `spread` px wide (at 1080p) at `alpha`, then one outer pixel at kUiContourOuter of that. The contour
// is judged by its mean darkness over the kUiContourWindow px next to a stroke (the gap to the next stroke at the HUD's
// small sizes, counted from one side only: conservative on purpose, since a 1 px ring reads far weaker than its alpha
// says and the bare picture between letters is what the eye reads them against), as a black backing of that alpha. Raised a step at a time: spread 1, 2, 3 px, each with its
// alpha from the resting one up to kUiContourAlphaMax, the least that holds `target`; at the cap and still short, a
// black shade (0..kUiLineShadeMax) under that line only, the least that holds it.

inline constexpr float kUiContourWindow = 6.f;     // px at 1080 beside a stroke over which the contour is judged
inline constexpr float kUiContourSpreadMax = 3.f;  // px at 1080: the widest the contour grows
inline constexpr float kUiContourAlphaMax = 0.9f;  // the contour's darkest (the edge colour's own alpha)
inline constexpr float kUiContourOuter = 0.5f;     // the outer pixel's alpha, of the contour's
inline constexpr float kUiLineShadeMax = kUiSubtitleBandMax;
inline constexpr float kUiLineMinContrast = 4.5f;  // the small lines
inline constexpr float kUiNameMinContrast = 3.f;   // the party names (large text)

struct UiLineGuard {
    float spread = 1.f; // px at 1080, whole
    float alpha = 0.f;  // the contour's alpha (its inner px)
    float shade = 0.f;  // black under the line (0: none)
    float bare = 0.f;   // the ink over the bare picture
    float ratio = 0.f;  // the ink over the contour (and shade) as returned
};

// The contour's mean darkness over kUiContourWindow px: `spread` px at `alpha`, then its outer pixel.
FTCORE_API float uiContourMean(float spread, float alpha);

// The least guard from the resting contour (`restSpread`, `restAlpha`) under which `ink` holds `target` over `picture`.
FTCORE_API UiLineGuard uiLineGuard(UiColor ink, UiColor picture, float target, float restSpread, float restAlpha);

// The least alpha (from `quiet` up to 1) at which `ink` drawn at that alpha holds `target` over `picture` under the
// contour at its cap: a resting line's floor (the objective's quiet level); 1 when even full ink falls short.
FTCORE_API float uiQuietFloor(UiColor ink, UiColor picture, float target, float quiet);

} // namespace ftcore
