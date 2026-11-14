#pragma once
// How much the arena HUD may put on screen at once: the element budget the widget obeys.
//
// A mark is one separately placed thing the eye has to find. The lead's row (name, Load, hp and guard lines, RELEASE)
// counts once; the partner's row, each live event mark (UiEventMarks.h), each tell, the Read row, the lock, the open
// weak point, the boss line (name, hp, notches and the call word together), each other hp line, each prompt and the
// cylinder each count once. Text inside a mark is part of it.
//
// At rest (nothing changed for `rules.rest` seconds) the other marks fade out (`actionAlpha`), then only the party
// rows and the prompts show: a prompt stands for as long as its press is open. In action the marks are granted in
// priority order up to `rules.marks`; what does not fit is not drawn.
//
//   UiBudgetState st;
//   each tick:  UiBudgetInput in = ...; in.prompts = uiPromptsOpen(view.now, fight.pairOpen()) | the first-use controls;
//               in.want = uiMarkWants(view.now, in.prompts);  (plus the widget's own wants)
//               const UiBudgetOut& out = uiBudgetStep(st, in, rules);
//
// Zero allocation.
#include "ftcore/Api.h"
#include "ftcore/UiStyle.h"
#include "ftcore/UiViews.h"

#include <array>
#include <cstdint>

namespace ftcore {

// Highest priority first: what is kept when the marks run out.
enum class UiMarkKind : std::uint8_t {
    Lead,       // the lead's row and all it carries
    Tell,       // an incoming blow's windup (over the body or at the screen's edge)
    Cards,      // the Read row
    Event,      // one live event mark (UiEventMarks.h)
    WeakPoint,  // the weak point open on the body
    Lock,       // the lock round the locked body
    Boss,       // the boss line
    Party,      // one other party row
    Rounds,     // the cylinder by the lock mark or the aim dot
    Prompt,     // one key prompt
    Target,     // a second hp line (the target when it is not the boss)
    Foe,        // a thin hp line over a body the top lines do not carry
};
inline constexpr std::size_t kUiMarkKindCount = 12;

// The key prompts, bottom right, in the order they claim a row and stack upward from the bottom: the heal, the paired
// counter's short window, then the first fight's basic controls. (The ruled gift and reload rows sit between the heal
// and the partner once their inputs exist.)
enum class UiPrompt : std::uint8_t { Heal, Pair, Attack, Guard, Dodge, Jump };
inline constexpr std::size_t kUiPromptCount = 6;
inline constexpr std::uint8_t kUiPromptRows = 4; // at most this many at once
inline constexpr std::uint8_t uiPromptBit(UiPrompt p) { return static_cast<std::uint8_t>(1u << static_cast<unsigned>(p)); }

struct UiBudgetInput {
    float dt = 0.f;
    bool activity = false;        // an input, an event mark, a tell or a hit this tick
    std::uint32_t changed = 0;    // updateArenaHudView's mask of changed sections
    bool bossEvent = false;       // the boss acted (a tell began), was hit, or was locked on anew
    std::uint8_t prompts = 0;     // UiPrompt bits open right now
    bool resultOpen = false;
    std::array<std::uint8_t, kUiMarkKindCount> want{}; // how many of each the data asks for this tick
};

struct UiBudgetOut {
    bool rest = false;            // nothing changed for the rest time: the in-action marks are fading out (or gone)
    float actionAlpha = 0.f;      // the in-action marks' show level: 1 in action, fading to 0 over the fade-out at rest
    std::array<std::uint8_t, kUiMarkKindCount> allow{}; // how many of each may be drawn
    std::uint8_t marks = 0;       // the total allowed
    float bossAlpha = 0.f;        // the boss line's show level (0 at rest)
    std::uint8_t promptsShown = 0; // UiPrompt bits drawn: the open ones, first in order, as many as granted
    std::uint8_t allowed(UiMarkKind k) const { return allow[static_cast<std::size_t>(k)]; }
};

struct UiBudgetState {
    float sinceChange = 1e9f;     // since the last change or activity (starts at rest)
    float sinceBoss = 1e9f;       // since the boss last acted, was hit or locked anew
    UiFade boss;
    UiFade action;                // the in-action marks (all but the lead's ring and the party places)
    std::uint8_t maxMarks = 0;    // the most marks allowed in any tick so far (the widget logs it)
    UiBudgetOut out;
};

// What the view's data asks for: a mark only where its field is present (absent data asks for nothing).
// `prompts` is the UiPrompt mask; the widget adds its own wants (event marks, tells, the Read row, lock, foes).
FTCORE_API std::array<std::uint8_t, kUiMarkKindCount> uiMarkWants(const ArenaHudFrame& f, std::uint8_t prompts);

// The prompts the data opens: Heal while hurt with heals left, Pair while the fight's paired window is open (`pairOpen`,
// ArenaFight::pairOpen) and the partner stands. The basic controls are the first-use teaching's (UiFirstUse.h).
FTCORE_API std::uint8_t uiPromptsOpen(const ArenaHudFrame& f, bool pairOpen);

FTCORE_API const UiBudgetOut& uiBudgetStep(UiBudgetState& st, const UiBudgetInput& in, const UiBudgetRules& rules);

} // namespace ftcore
