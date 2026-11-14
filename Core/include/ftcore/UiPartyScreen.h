#pragma once
// The party screen: the fight HUD's party block opened in place, one row taller, while the fight runs on. In explore
// the same rows, with FOLLOW / WAIT HERE (the whole party's follow toggle) as the last.
//
//   UiPartyScreen s;
//   each tick: const UiPartyOut out = uiPartyScreenStep(s, in, dt);
//              out.map: explore's short press of the map key, let go (the map itself)
//              out.closed: the screen shut this tick; out.picked(): with a row other than the controlled lead's
//   draw: while s.open, the rows (Third, partner, lead, then Follow in explore), s.cursor's row underlined
//
// Open: Party's own key down opens at once. The map key (the pad's only way to Party, route map_hold) opens at once
// in a fight, which has no map; in explore it opens once held kUiPartyHold, and a shorter press is the map.
// Move: one row per flick of the right stick or the mouse, up or down; the Third's row never takes the cursor, and a
// flick past the first or last row stays. A flick counts only after the stick or the mouse has come back to rest, so
// a stick already pushed as the screen opens moves nothing.
// Confirm: letting go of Party (both keys up) shuts the screen on the cursor's row; under the Toggle hold mode the
// opening press's release does nothing and the next press shuts it. The cursor opens on the controlled lead, so
// opening and letting go picks nothing.
// Cancel: `live` false (a pause, a menu, a scene, the focus lost, the fight over) shuts it with no pick, and a key
// still held when input comes back is no press (nor, on its release, the map).
//
// Pure, no allocation. A refused row (UiPartyWhy) still takes the cursor and still picks: the rules refuse the
// request at the step's close, and the HUD says why.
#include "ftcore/Api.h"
#include "ftcore/FightSession.h"

#include <cstddef>
#include <cstdint>

namespace ftcore {

// The rows, top to bottom.
enum class UiPartyRow : std::uint8_t { Third, Partner, Lead, Follow };
inline constexpr std::size_t kUiPartyRows = 4;

// Explore: the map key held this long opens the screen; a shorter press is the map.
inline constexpr float kUiPartyHold = 0.25f;
// A refused switch's call (SWITCH: DOWN) stands this long in the call slot.
inline constexpr float kUiPartySwitchShow = 1.5f;
// A flick: the right stick past kUiPartyStickOut (of 1), or the mouse past kUiPartyMouseOut in one tick; each counts
// again once back under its rest.
inline constexpr float kUiPartyStickOut = 0.7f;
inline constexpr float kUiPartyStickRest = 0.3f;
inline constexpr float kUiPartyMouseOut = 8.f;
inline constexpr float kUiPartyMouseRest = 2.f;

struct UiPartyInput {
    bool live = false;      // the screen may stand: input taken and, in a fight, the fight on; false shuts it, no pick
    bool explore = false;   // explore: the map key's hold opens it, and the rows end with Follow
    bool partyDown = false; // Party's own key down
    bool mapDown = false;   // the map key down (Party's route on the pad)
    bool toggle = false;    // the Toggle hold mode: a press opens, the next press confirms
    float stickY = 0.f;     // the right stick's vertical, up positive
    float mouseY = 0.f;     // the mouse's vertical move this tick, up positive
};

struct UiPartyScreen {
    bool open = false;
    bool explore = false;           // as opened: Follow is a row only in explore
    UiPartyRow cursor = UiPartyRow::Lead;
    bool partyWas = true;           // the keys as of the last step (true at the start: a key held then is no press)
    bool mapWas = true;
    float mapHeld = 0.f;            // explore, closed: the map key's hold so far
    bool mapUsed = true;            // this map press opened, confirmed or outlived the screen: its release is no map
    bool opening = false;           // the press that opened it is still down (Toggle waits for its release)
    bool stickArmed = false;        // the stick is back at rest: its next push past kUiPartyStickOut is a flick
    bool mouseArmed = false;
};

struct UiPartyOut {
    bool opened = false;
    bool closed = false;
    bool map = false;                    // explore: the map key's short press, let go
    UiPartyRow pick = UiPartyRow::Lead;  // with `closed`: the cursor's row (a cancel: Lead)
    bool picked() const { return closed && pick != UiPartyRow::Lead; }
};

FTCORE_API UiPartyOut uiPartyScreenStep(UiPartyScreen& s, const UiPartyInput& in, float dt);

// Whether the cursor may rest on a row: the partner and the lead, and Follow in explore; never the Third.
FTCORE_API bool uiPartyRowSelectable(UiPartyRow r, bool explore);

// Why a lead row's switch would be refused now: its word after the name, the row at 40% ink. Unknown: the rules have
// not said (the row stands undimmed, no word); Ok: taken.
enum class UiPartyWhy : std::uint8_t { Unknown, Ok, Down, NotHere, FightOver, Wait };
inline constexpr std::size_t kUiPartyWhyCount = 6;
FTCORE_API bool uiPartyRefused(UiPartyWhy w);
// A switch the session refused at a step's close (FightTick::switchRefusal), as the row's word: Down DOWN, Defeated
// FIGHT OVER; None Unknown. Each refusal has its case, so a new one fails the Core build (-Wswitch) until it has
// a word.
FTCORE_API UiPartyWhy uiPartyWhyOf(SwitchRefusal r);

} // namespace ftcore
