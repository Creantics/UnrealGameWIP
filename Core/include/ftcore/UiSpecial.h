// The special prompt: the fight HUD's row that says the controlled lead's special move is ready, with the key to press.
// Each lead fills a hidden gauge; the HUD never shows it (no meter of any kind, ever). The HUD learns only two things,
// both from the rules: the special is ready, and if it cannot start now, why. Nothing here takes a level, a share or a
// count, so nothing drawn from it can read as a gauge.
//
// The row (r19c option C, the owner's pick, 2026-11-17): its own row in the bottom-right prompt stack, above the fight's
// own prompts (HEAL, GIFT, PARTNER) and below the basic controls. Under the world's switch (UiWorldRules::specialPrompt,
// "special_prompt") the row is reserved whether or not a special is ready, so the stack never moves when it appears;
// with the switch off the stack is as it was (uiPromptRowAt with reserve false is uiPromptRow).
//
// What it draws: a doubled key cap (a second outline a little out from the first, always closed, never filled or
// partial), the word SPECIAL and the lead's name. On arrival it fades in over kUiSpecialFadeIn s and its outer outline
// pulses once (grows kUiSpecialPulseGrow px and fades over kUiSpecialPulse s); then it stands at full ink until the
// special is used. It never rests or fades with the other prompts. While the special cannot start (no target, out of
// sight, not now) the row keeps its place with one outline, in ink_dim, and the reason in place of the name. Reduce
// motion: no pulse, and the fade is a cut.
//
// Per-member choices waiting on the owner (the Third called by the player or acting himself; a mark for a ready
// partner) are data switches, off (UiStyle::special, UiSpecialSwitches in UiStyle.h; style.json "special"): off, only
// the controlled lead's row exists.
#pragma once

#include "ftcore/Api.h"
#include "ftcore/UiBudget.h"

#include <cstddef>
#include <cstdint>

namespace ftcore {

// Why a ready special cannot start now (the rules give it; the arena lane owns the press).
enum class UiSpecialWhy : std::uint8_t { None, NoTarget, OutOfSight, NotNow };
inline constexpr std::size_t kUiSpecialWhyCount = 4;

// The rules' answer this tick: bools and a reason only, by design (the gauge's level never reaches the HUD).
struct UiSpecialInput {
    bool ready = false;                    // the controlled lead's special is ready
    UiSpecialWhy why = UiSpecialWhy::None; // while ready: None if a press would start it now
};

inline constexpr float kUiSpecialFadeIn = 0.15f;  // s, the arrival fade
inline constexpr float kUiSpecialPulse = 0.5f;    // s, the one pulse of the outer outline
inline constexpr float kUiSpecialPulseGrow = 16.f; // px at 1080p, how far the pulse's outline grows out
inline constexpr float kUiSpecialRingGap = 5.f;   // px at 1080p, the second outline's distance out from the first

// The prompt stack's row for a prompt with the special row reserved or not. Reserved: rows 0-2 as today (HEAL, GIFT,
// PARTNER), the special at kUiSpecialRow, the basic controls one row up. Not reserved: uiPromptRow.
inline constexpr std::uint8_t kUiSpecialRow = 3;
constexpr std::uint8_t uiPromptRowAt(UiPrompt p, bool reserve) {
    const std::uint8_t r = uiPromptRow(p);
    return reserve && r >= kUiSpecialRow ? static_cast<std::uint8_t>(r + 1) : r;
}

// The row's state between ticks.
struct UiSpecialState {
    bool ready = false;  // ready last tick
    bool pulsed = false; // the one pulse has started (at its first live tick)
    float since = 0.f;   // s since it became ready
    float pulseAt = 0.f; // `since` when the pulse started
};

// What to draw this tick.
struct UiSpecialView {
    bool row = false;      // the row is reserved (the world's switch)
    bool drawn = false;    // a special is ready: the row is drawn
    bool live = false;     // ready and a press would start it: two outlines, full ink, the lead's name
    UiSpecialWhy why = UiSpecialWhy::None; // drawn and not live: the reason shown in place of the name
    float alpha = 0.f;     // the arrival fade, 0..1 (1 once in, for as long as it is ready)
    float pulse = -1.f;    // the pulse's progress 0..1 while it runs (outer outline grows and fades), else -1
};

// One tick: `reserve` is the world's switch; with it off nothing is drawn and the state is cleared.
FTCORE_API UiSpecialView uiSpecialStep(UiSpecialState& s, bool reserve, const UiSpecialInput& in, float dt, bool reduceMotion);

} // namespace ftcore
