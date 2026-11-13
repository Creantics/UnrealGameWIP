#pragma once
// How much the arena HUD may put on screen at once: the element budget the widget obeys.
//
// A mark is one separately placed thing the eye has to find. The lead's ring with everything round it counts once;
// each party place, each popup, each tell, the Read cards, the lock, the open weak point, the boss line (name, hp,
// notches and pips together), each other hp line, each verb, the cylinder, the wall cue and the lead's state word
// each count once. Text inside a mark is part of it.
//
// At rest (nothing changed for `rules.rest` seconds) the other marks fade out (`actionAlpha`), then only the lead mark
// and the party places show. In action the
// marks are granted in priority order up to `rules.marks`; what does not fit is not drawn.
//
//   UiBudgetState st;
//   each tick:  UiBudgetInput in = ...; in.want = uiMarkWants(view.now, verbsAvailable);  (plus the widget's own wants)
//               const UiBudgetOut& out = uiBudgetStep(st, in, rules);
//
// Popups: UiPopLanes keeps at most `rules.popupsPerBody` per body, coalesces a repeat (same body, kind and amount)
// into a count, and empties when the result opens.
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
    Lead,       // the lead's ring and all it carries
    Tell,       // an incoming blow's windup (over the body or at the screen's edge)
    Cards,      // the Read cards
    Popup,      // one popup
    WeakPoint,  // the weak point open on the body
    Lock,       // the lock round the locked body
    Boss,       // the boss line
    State,      // the lead's state word at the shoulder
    Party,      // one other party place
    Rounds,     // the cylinder by the reticle
    Verb,       // one verb
    Target,     // a second hp line (the target when it is not the boss)
    Foe,        // a thin hp line over a body the top lines do not carry
    Wall,       // the wall cue
};
inline constexpr std::size_t kUiMarkKindCount = 14;

// The verbs in the verb list, as bits of a mask.
enum class UiVerb : std::uint8_t { Guard, Release, Pair, Heal };
inline constexpr std::size_t kUiVerbCount = 4;
inline constexpr std::uint8_t uiVerbBit(UiVerb v) { return static_cast<std::uint8_t>(1u << static_cast<unsigned>(v)); }

struct UiBudgetInput {
    float dt = 0.f;
    bool activity = false;        // an input, a popup, a tell or a hit this tick
    std::uint32_t changed = 0;    // updateArenaHudView's mask of changed sections
    bool bossEvent = false;       // the boss acted (a tell began), was hit, or was locked on anew
    std::uint8_t verbsAvailable = 0; // UiVerb bits that can be used right now
    std::int8_t prompted = -1;    // the verb being prompted (UiVerb index), -1 none
    bool resultOpen = false;
    std::array<std::uint8_t, kUiMarkKindCount> want{}; // how many of each the data asks for this tick
};

struct UiBudgetOut {
    bool rest = false;            // nothing changed for the rest time: the in-action marks are fading out (or gone)
    float actionAlpha = 0.f;      // the in-action marks' show level: 1 in action, fading to 0 over the fade-out at rest
    std::array<std::uint8_t, kUiMarkKindCount> allow{}; // how many of each may be drawn
    std::uint8_t marks = 0;       // the total allowed
    float bossAlpha = 0.f;        // the boss line's show level (0 at rest)
    float verbAlpha = 0.f;        // the verb list's show level (0 at rest)
    std::uint8_t verbsShown = 0;  // UiVerb bits drawn (only the available ones)
    std::int8_t keyRing = -1;     // the one verb with a key ring (the prompted one, when shown), -1 none
    std::uint8_t allowed(UiMarkKind k) const { return allow[static_cast<std::size_t>(k)]; }
};

struct UiBudgetState {
    float sinceChange = 1e9f;     // since the last change or activity (starts at rest)
    float sinceBoss = 1e9f;       // since the boss last acted, was hit or locked anew
    UiFade boss;
    UiFade verbs;
    UiFade action;                // the in-action marks (all but the lead's ring and the party places)
    std::uint8_t maxMarks = 0;    // the most marks allowed in any tick so far (the widget logs it)
    UiBudgetOut out;
};

// What the view's data asks for: a mark only where its field is present (absent data asks for nothing).
// `verbsAvailable` is the UiVerb mask; the widget adds its own wants (popups, tells, cards, lock, foes, wall).
FTCORE_API std::array<std::uint8_t, kUiMarkKindCount> uiMarkWants(const ArenaHudFrame& f, std::uint8_t verbsAvailable);

// The verbs available from the lead's and partner's data.
FTCORE_API std::uint8_t uiVerbsAvailable(const ArenaHudFrame& f);

FTCORE_API const UiBudgetOut& uiBudgetStep(UiBudgetState& st, const UiBudgetInput& in, const UiBudgetRules& rules);

// ---- Popup lanes -----------------------------------------------------------------------------------------------

inline constexpr std::size_t kUiPopMax = 16;

struct UiPop {
    bool live = false;
    std::uint8_t body = 0;   // the host's body index (the lead, the ally, an enemy slot)
    std::uint8_t kind = 0;   // the host's popup kind
    int amount = 0;          // its number (0: none)
    std::uint8_t count = 1;  // repeats coalesced into it
    float age = 0.f;
    std::uint32_t serial = 0; // bumps each time it is pushed or coalesced (the host rebuilds its text then)
};

struct UiPopLanes {
    std::array<UiPop, kUiPopMax> pop{};
    std::uint32_t serial = 0;
};

// Pushes a popup; returns its slot. A repeat of a live one (same body, kind and amount) bumps that one's count and
// age instead; a body over `rules.popupsPerBody` loses its oldest.
FTCORE_API std::uint32_t uiPopPush(UiPopLanes& lanes, std::uint8_t body, std::uint8_t kind, int amount,
    const UiBudgetRules& rules);
// Ages the popups; those past `life` seconds end.
FTCORE_API void uiPopTick(UiPopLanes& lanes, float dt, float life);
// Ends every popup (the result card opened).
FTCORE_API void uiPopClear(UiPopLanes& lanes);
// The popup's lane in its body's stack: 0 the newest.
FTCORE_API std::uint32_t uiPopLane(const UiPopLanes& lanes, std::uint32_t slot);
FTCORE_API std::uint32_t uiPopLiveCount(const UiPopLanes& lanes, std::uint8_t body);

} // namespace ftcore
