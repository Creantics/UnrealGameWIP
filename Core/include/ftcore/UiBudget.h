#pragma once
// How much the arena HUD may put on screen at once: the element budget the widget obeys.
//
// A mark is one separately placed thing the eye has to find. The lead's row (name, Load, hp and guard lines, RELEASE)
// counts once; the partner's row, each live event mark (UiEventMarks.h), each tell, the Read row, the lock, the open
// weak point, the boss line (name, hp, notches and the call word together), each other hp line, each prompt and the
// cylinder each count once. Text inside a mark is part of it.
//
// At rest (nothing changed for `rules.rest` seconds) the other marks fade out (`actionAlpha`), then only the party
// rows and the prompts show: a prompt stands for as long as its action is in the fight. In action the marks are
// granted in priority order up to `rules.marks`; what does not fit is not drawn. The prompt rows are fixed and never
// dropped, so they sit outside the cap: every row the prompts fill is granted.
//
//   UiBudgetState st;
//   each tick:  const UiPromptReasons why = uiPromptReasons(fight, giftHealWhy, giftGuardWhy);
//               UiBudgetInput in = ...; in.prompts = uiPromptsDrawable(uiPromptsPresent(why, teaching), keyed, below);
//               in.want = uiMarkWants(view.now, in.prompts);  (plus the widget's own wants)
//               const UiBudgetOut& out = uiBudgetStep(st, in, rules);
//               dimmed = uiPromptsDimmed(why, noKey);
//
// Zero allocation.
#include "ftcore/Api.h"
#include "ftcore/PressReason.h"
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
    Prompt,     // one row of key prompts (one prompt, or in the GIFT row both gifts)
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
// close; an empty row stays empty. Row 1 HEAL, row 2 GIFT (GIFT HEAL and GIFT GUARD share it: with both ready both
// stand, GIFT GUARD at the margin and GIFT HEAL to its left, and either alone takes the margin), row 3 RELOAD, row 4
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
inline constexpr std::uint8_t kUiPromptRows = 8; // the rows there are, so at most this many rows at once
static_assert(uiPromptRow(UiPrompt::Jump) + 1 == kUiPromptRows, "the top row is JUMP's");
// The one row two prompts may hold at once: the two gifts'. Every other row holds one.
constexpr bool uiPromptSharesRow(UiPrompt p) { return p == UiPrompt::GiftHeal || p == UiPrompt::GiftGuard; }
// The rows a prompt mask fills (a prompt mark is a row: the GIFT row costs one with one gift or two).
FTCORE_API std::uint8_t uiPromptRowsOf(UiPromptMask m);

struct UiBudgetInput {
    float dt = 0.f;
    bool activity = false;        // an input, an event mark, a tell or a hit this tick
    std::uint32_t changed = 0;    // updateArenaHudView's mask of changed sections
    bool bossEvent = false;       // the boss acted (a tell began), was hit, or was locked on anew
    UiPromptMask prompts = 0;     // UiPrompt bits drawn this tick (present and drawable: uiPromptsDrawable)
    bool resultOpen = false;
    std::array<std::uint8_t, kUiMarkKindCount> want{}; // how many of each the data asks for this tick
};

struct UiBudgetOut {
    bool rest = false;            // nothing changed for the rest time: the in-action marks are fading out (or gone)
    float actionAlpha = 0.f;      // the in-action marks' show level: 1 in action, fading to 0 over the fade-out at rest
    std::array<std::uint8_t, kUiMarkKindCount> allow{}; // how many of each may be drawn
    std::uint8_t marks = 0;       // the total allowed (the prompt rows' included; the cap holds for the others)
    float bossAlpha = 0.f;        // the boss line's show level (0 at rest)
    UiPromptMask promptsShown = 0; // UiPrompt bits drawn: `prompts`, every row granted
    std::uint8_t allowed(UiMarkKind k) const { return allow[static_cast<std::size_t>(k)]; }
};

struct UiBudgetState {
    float sinceChange = 1e9f;     // since the last change or activity (starts at rest)
    float sinceBoss = 1e9f;       // since the boss last acted, was hit or locked anew
    UiFade boss;
    UiFade action;                // the in-action marks (all but the lead's ring and the party places)
    std::uint8_t maxMarks = 0;    // the most marks allowed in any tick so far, the prompt rows aside (the widget logs it)
    UiBudgetOut out;
};

// What the view's data asks for: a mark only where its field is present (absent data asks for nothing).
// `prompts` is the UiPrompt mask; the widget adds its own wants (event marks, tells, the Read row, lock, foes).
FTCORE_API std::array<std::uint8_t, kUiMarkKindCount> uiMarkWants(const ArenaHudFrame& f, UiPromptMask prompts);

// Each prompt's answer from the rules (PressReason.h): would its press be taken now, and if not, why. The HUD asks the
// rules; it never copies them, so a hint cannot say other than its press does.
//   Heal       the fight's own heal (ArenaFight::pressReason(ArenaPress::Heal)): Absent with no heal charges in the kit
//   GiftHeal   the session's gift slot (FightSession::giftReason(Gift::Heal, lead, lead health)), from the host:
//   GiftGuard  Absent with no session or no third place, as the host passes it
//   Reload     Absent: the reload is automatic, there is no reload press (no InputAction) to ask about
//   Pair       Absent with no partner in the fight (allyMaxHp 0: the press is the partner's); else ArenaPress::Pair
//   Attack     ArenaPress::Light; Guard, Dodge, Jump their own presses (Absent where the kit has none)
using UiPromptReasons = std::array<PressReason, kUiPromptCount>;
FTCORE_API UiPromptReasons uiPromptReasons(const ArenaFight& fight, PressReason giftHeal, PressReason giftGuard);

// The prompts the fight has: each whose action is in this fight (its answer is not Absent), the basic controls (Attack,
// Guard, Dodge, Jump) only while the first-use teaching holds them (`teaching`, UiPrompt bits; UiFirstUse.h). A prompt
// not present is not drawn; every other stands in its row whether its press is taken now or not.
FTCORE_API UiPromptMask uiPromptsPresent(const UiPromptReasons& why, UiPromptMask teaching);

// The prompts drawn dimmed (ink at 40 percent, the key cap too): each whose press would be refused now (pressRefused:
// its reason word stands after it), and each with no key on the device (`noKey`: the import left its action unbound,
// so its key box says NO KEY). Busy is not dimmed: that press waits in the buffer and is taken.
FTCORE_API UiPromptMask uiPromptsDimmed(const UiPromptReasons& why, UiPromptMask noKey);

// One prompt per row (two in the GIFT row), one row per key, and only rows something reaches on the device. A prompt
// nothing reaches on the device (not in `keyed`: no key, no route, and not an unbound action's NO KEY) is dropped. So is
// one whose key a lower present prompt already shows (`sameKeyBelow[p]`: the lower prompts bound to p's key; NO KEY is
// no key, so never shared): the one left is the lowest, because a shared key does what the lowest row says (the heal
// before a gift). So is one whose row a lower prompt already holds, but for the GIFT row, which holds both gifts.
FTCORE_API UiPromptMask uiPromptsDrawable(UiPromptMask open, UiPromptMask keyed,
                                          const std::array<UiPromptMask, kUiPromptCount>& sameKeyBelow);

FTCORE_API const UiBudgetOut& uiBudgetStep(UiBudgetState& st, const UiBudgetInput& in, const UiBudgetRules& rules);

} // namespace ftcore
