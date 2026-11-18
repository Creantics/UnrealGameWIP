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
// sight, not now) the row keeps its place with one outline, in ink_dim, and the reason in place of the name. When
// nothing is ready any more (used, or a lead switch lowered the gauge: the rules' unready) the row leaves over
// kUiSpecialFadeOut s with no pulse, as last shown; ready again, it arrives with the pulse. Reduce motion: no pulse,
// and both fades are cuts.
//
// The owner's rulings (2026-11-17): only the controlled lead's special is fired from the row (no partner row, no mark by
// the partner's name). The Third's special is the player's call: a tap of the key fires the lead's, a hold calls the
// Third's when both are ready; with only his ready the row reads CALL and his current name (never a fixed word), and a
// tap calls it. The press is the arena lane's; this row only shows it.
#pragma once

#include "ftcore/Api.h"
#include "ftcore/ArenaSpecial.h"
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
    bool thirdReady = false;               // the Third's special is ready (the player calls it)
};

// The rules' feed (ArenaFight::special(), read while the fight runs): the controlled lead's full gauge, and while a
// press would be refused, why; the Third's full gauge (readyPartner: a party member's only). Nothing is ready while a
// special runs (the gauge empties at the press), and a full gauge whose lead has no move (refusal NoMove: no row for
// that lead in the table) never shows: there is nothing to press. A moveless lead's gauge still fills (the rules');
// the row only ever offers a move that exists.
inline UiSpecialInput uiSpecialInputFrom(const ArenaSpecialView& rules) {
    UiSpecialInput in;
    in.ready = rules.readyPlayer && !rules.running && rules.refusalPlayer != ArenaSpecialRefusal::NoMove;
    in.thirdReady = rules.readyPartner && !rules.running && rules.refusalPartner != ArenaSpecialRefusal::NoMove;
    switch (rules.refusalPlayer) {
    case ArenaSpecialRefusal::Ok:
    case ArenaSpecialRefusal::NotReady: in.why = UiSpecialWhy::None; break;
    case ArenaSpecialRefusal::NoTarget: in.why = UiSpecialWhy::NoTarget; break;
    case ArenaSpecialRefusal::NoSight: in.why = UiSpecialWhy::OutOfSight; break;
    default: in.why = UiSpecialWhy::NotNow; break;
    }
    return in;
}

inline constexpr float kUiSpecialFadeIn = 0.15f;  // s, the arrival fade
inline constexpr float kUiSpecialFadeOut = 0.2f;  // s, the leaving fade (nothing ready any more)
inline constexpr float kUiSpecialPulse = 0.5f;    // s, the one pulse of the outer outline
inline constexpr float kUiSpecialPulseGrow = 16.f; // px at 1080p, how far the pulse's outline grows out
inline constexpr float kUiSpecialRingGap = 5.f;   // px at 1080p, the second outline's distance out from the first

// The prompt stack's row for a prompt with the special row reserved or not. Reserved: rows 0-2 as today (HEAL, GIFT,
// PARTNER), the special at kUiSpecialRow, the basic controls one row up. Not reserved: uiPromptRow.
inline constexpr std::uint8_t kUiSpecialRow = 3;
// Whether the row is reserved: the world's switch (UiWorldRules::specialPrompt) and a fight with specials
// (ArenaFight::specialsOn: a table set since the fight began, steady through it). Without both the stack is as it was.
constexpr bool uiSpecialRowReserved(bool worldSwitch, bool fightHasSpecials) { return worldSwitch && fightHasSpecials; }
constexpr std::uint8_t uiPromptRowAt(UiPrompt p, bool reserve) {
    const std::uint8_t r = uiPromptRow(p);
    return reserve && r >= kUiSpecialRow ? static_cast<std::uint8_t>(r + 1) : r;
}

// The row's state between ticks.
struct UiSpecialState {
    bool ready = false;   // something was ready last tick (the row arriving or standing)
    bool pulsed = false;  // the one pulse has started (at its first live tick)
    float since = 0.f;    // s since it became ready; while leaving, s since it began to leave
    float pulseAt = 0.f;  // `since` when the pulse started
    bool leaving = false; // nothing ready: the row fades out as last shown
    float alphaWas = 0.f; // the alpha last drawn while ready: a leave falls from it (a return mid-leave climbs from
                          // where the leave had got to)
    bool live = false;    // as last shown, held through the leave
    bool call = false;
    UiSpecialWhy why = UiSpecialWhy::None;
};

// What to draw this tick.
struct UiSpecialView {
    bool row = false;      // the row is reserved (the world's switch)
    bool drawn = false;    // a special is ready: the row is drawn
    bool live = false;     // ready and a press would start it: two outlines, full ink, the lead's name
    bool call = false;     // only the Third's is ready: the row reads CALL and his current name
    UiSpecialWhy why = UiSpecialWhy::None; // drawn and not live: the reason shown in place of the name
    float alpha = 0.f;     // the arrival or leaving fade, 0..1 (1 once in, for as long as it is ready)
    float pulse = -1.f;    // the pulse's progress 0..1 while it runs (outer outline grows and fades), else -1
};

// One tick: `reserve` is the world's switch; with it off nothing is drawn and the state is cleared. reduceMotion drops
// the fades and the pulse; reduceFlashes drops the pulse only (the fades are not a flash).
FTCORE_API UiSpecialView uiSpecialStep(UiSpecialState& s, bool reserve, const UiSpecialInput& in, float dt, bool reduceMotion,
                                       bool reduceFlashes = false);

} // namespace ftcore
