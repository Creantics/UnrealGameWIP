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

// The key prompts, bottom right: HEAL, the two GIFT prompts, RELOAD, PARTNER (the paired counter's short window), then
// the first fight's basic controls.
enum class UiPrompt : std::uint8_t { Heal, GiftHeal, GiftGuard, Reload, Pair, Attack, Guard, Dodge, Jump };
inline constexpr std::size_t kUiPromptCount = 9;
using UiPromptMask = std::uint16_t;              // UiPrompt bits
static_assert(kUiPromptCount <= 16, "UiPromptMask holds one bit per prompt");
inline constexpr UiPromptMask uiPromptBit(UiPrompt p) { return static_cast<UiPromptMask>(1u << static_cast<unsigned>(p)); }

// Each prompt owns a fixed row, counted up from the bottom margin (0 is the lowest), and never moves when others open or
// close; an empty row stays empty. Row 1 HEAL, row 2 GIFT (GIFT HEAL and GIFT GUARD share it), row 3 RELOAD, row 4
// PARTNER, rows 5 to 7 ATTACK, GUARD and DODGE, row 8 JUMP (when it is bound).
constexpr std::uint8_t uiPromptRow(UiPrompt p) {
    switch (p) {
    case UiPrompt::Heal: return 0;
    case UiPrompt::GiftHeal:
    case UiPrompt::GiftGuard: return 1;
    case UiPrompt::Reload: return 2;
    case UiPrompt::Pair: return 3;
    case UiPrompt::Attack: return 4;
    case UiPrompt::Guard: return 5;
    case UiPrompt::Dodge: return 6;
    case UiPrompt::Jump: return 7;
    }
    return 0;
}
inline constexpr std::uint8_t kUiPromptRows = 8; // the rows there are, so at most this many at once
static_assert(uiPromptRow(UiPrompt::Jump) + 1 == kUiPromptRows, "the top row is JUMP's");

struct UiBudgetInput {
    float dt = 0.f;
    bool activity = false;        // an input, an event mark, a tell or a hit this tick
    std::uint32_t changed = 0;    // updateArenaHudView's mask of changed sections
    bool bossEvent = false;       // the boss acted (a tell began), was hit, or was locked on anew
    UiPromptMask prompts = 0;     // UiPrompt bits open right now (and drawable: uiPromptsDrawable)
    bool resultOpen = false;
    std::array<std::uint8_t, kUiMarkKindCount> want{}; // how many of each the data asks for this tick
};

struct UiBudgetOut {
    bool rest = false;            // nothing changed for the rest time: the in-action marks are fading out (or gone)
    float actionAlpha = 0.f;      // the in-action marks' show level: 1 in action, fading to 0 over the fade-out at rest
    std::array<std::uint8_t, kUiMarkKindCount> allow{}; // how many of each may be drawn
    std::uint8_t marks = 0;       // the total allowed
    float bossAlpha = 0.f;        // the boss line's show level (0 at rest)
    UiPromptMask promptsShown = 0; // UiPrompt bits drawn: the open ones, first in order, as many as granted
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
FTCORE_API std::array<std::uint8_t, kUiMarkKindCount> uiMarkWants(const ArenaHudFrame& f, UiPromptMask prompts);

// The prompts the data opens (the first four never while the lead is down):
//   Heal       hurt with heals left, and not overloaded (the fight refuses a heal until a Release)
//   GiftHeal   hurt with Gift Heals left (the third place's giftHeals; -1, not reported, opens nothing)
//   GiftGuard  the Gift Guard off its cooldown (giftGuardReady)
//   Reload     the gun empty (a cylinder with no rounds) and not already reloading
//   Pair       the fight's paired window open (`pairOpen`, ArenaFight::pairOpen) with the partner up
// The basic controls are the first-use teaching's (UiFirstUse.h).
FTCORE_API UiPromptMask uiPromptsOpen(const ArenaHudFrame& f, bool pairOpen);

// Whether a basic control (Attack, Guard, Dodge, Jump) can be used now, so its first-use row may stand: none while the
// lead is down, no Attack while overloaded (the fight refuses light, heavy and shot until a Release), no Guard while the
// guard is broken. A prompt that cannot be used is not drawn, never greyed. The data's rows answer true: uiPromptsOpen
// already opens them only when usable.
FTCORE_API bool uiControlUsable(const ArenaHudLead& lead, UiPrompt p);

// One prompt per row, one row per key, and only rows that have a key. A prompt with no key on the device (not in
// `keyed`) is dropped. So is one whose key a lower open prompt already shows (`sameKeyBelow[p]`: the lower prompts bound
// to p's key): the one left is the lowest, because a shared key does what the lowest live row says (the heal before a
// gift). So is one whose row a lower prompt already holds (GIFT HEAL keeps the GIFT row over GIFT GUARD). This runs
// before the budget, so a prompt that would draw nothing takes no mark.
FTCORE_API UiPromptMask uiPromptsDrawable(UiPromptMask open, UiPromptMask keyed,
                                          const std::array<UiPromptMask, kUiPromptCount>& sameKeyBelow);

FTCORE_API const UiBudgetOut& uiBudgetStep(UiBudgetState& st, const UiBudgetInput& in, const UiBudgetRules& rules);

} // namespace ftcore
