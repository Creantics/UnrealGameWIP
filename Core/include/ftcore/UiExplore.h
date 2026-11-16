#pragma once
// The explore HUD's timing and the subtitle's fit (2026-11-16 ruling, item A): the objective line that quiets to 40%
// after 4 s without change, the notices shown one at a time at the right edge (2.5 s each, a queue of 3), the
// subtitle's breaks (42 characters a line, 2 lines on screen, a longer line in pages) and the band behind it (35% black,
// only where the measured contrast under the text falls below 4.5:1).
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
inline constexpr float kUiSubtitleBand = 0.35f;       // the band's black at its middle

// The band behind the subtitle: 0 when `ink` stands at kUiSubtitleMinContrast or more over `picture` (the brightest
// part under the text), else kUiSubtitleBand. `ratio` (optional) gets the bare contrast, `withBand` the contrast over
// the band.
FTCORE_API float uiSubtitleBand(UiColor ink, UiColor picture, float* ratio = nullptr, float* withBand = nullptr);

} // namespace ftcore
