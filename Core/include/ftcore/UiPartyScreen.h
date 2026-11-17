#pragma once
// The party screen: the fight HUD's party block opened in place, one row taller, while the fight runs on. In explore
// the same rows, with FOLLOW / WAIT HERE (the whole party's follow toggle) as the last.
//
//   UiPartyScreen s;
//   each tick: const UiPartyOut out = uiPartyScreenStep(s, in, dt);
//              out.map: explore's short press of the map key, let go (the map itself)
//              out.closed: the screen shut this tick; out.picked(): with a row other than the controlled lead's
//   draw: while s.open, the rows (Third, partner, lead, then Follow in explore), each as uiPartyRowView says: the
//         cursor's row at full ink underlined across its name, the others dimmer, a dot before the current lead's name
//
// Open: Party's own key down opens at once. The map key (the pad's only way to Party, route map_hold) opens at once
// in a fight, which has no map; in explore it opens once held kUiPartyHold, and a shorter press is the map.
// Move: one row per flick of the right stick or the mouse, up or down; the Third's row never takes the cursor, nor the
// partner's while no partner is in the fight (`partnerHere` false: the row reads NOT HERE), and a flick past the first
// or last row it may take stays. A cursor on the partner's row when the partner goes goes back to the lead's, and so
// does any cursor when the controlled lead changes under the open screen without its confirm (Tab, a takeover by a
// fall: UiPartyInput::lead moves), so a release then sends nothing. A flick counts only after the stick or the mouse
// has come back to rest, so a stick already pushed as the screen opens moves nothing.
// Confirm: letting go of Party (both keys up) shuts the screen on the cursor's row; under the Toggle hold mode the
// opening press's release does nothing and the next press shuts it. The cursor opens on the controlled lead, so
// opening and letting go picks nothing.
// A tap: the opening press let go within kUiPartyTap with the cursor not moved keeps the screen open (as under Toggle),
// so it can be worked by keys: the row keys (`upDown`, `downDown`: the arrows, W and S) move the cursor one row per
// press, `confirmDown` (Enter, a click, pad A) shuts it on the cursor's row, and Party's or the map key's next press
// does too. These keys work whatever opened it.
// Cancel: `cancelDown` (Esc, pad B) shuts it with no pick. `live` false (a pause, a menu, a scene, the focus lost, the
// fight over) shuts it with no pick too, and a key still held when input comes back is no press (nor, on its release,
// the map).
//
// Pure, no allocation. A refused row (UiPartyWhy Down, Wait, FightOver) still takes the cursor and still picks: the
// rules refuse the request at the step's close, and the HUD says why. NOT HERE is the one refusal that never picks: the
// host passes it as `partnerHere` false, so the cursor never rests there (no body to send a switch to).
#include "ftcore/Api.h"
#include "ftcore/LeadSwitch.h"

#include <cstddef>
#include <cstdint>

namespace ftcore {

// The rows, top to bottom.
enum class UiPartyRow : std::uint8_t { Third, Partner, Lead, Follow };
inline constexpr std::size_t kUiPartyRows = 4;

// Explore: the map key held this long opens the screen; a shorter press is the map.
inline constexpr float kUiPartyHold = 0.25f;
// The opening press let go within this long, the cursor not moved: a tap, and the screen stays open.
inline constexpr float kUiPartyTap = 0.3f;
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
    bool partnerHere = true; // false: no partner in this fight (its row NOT HERE), so its row never takes the cursor
    int lead = -1;           // the controlled lead (the host's id; -1: not said); a change while open puts the cursor on Lead
    bool upDown = false;      // a row key up (the up arrow, W): each press moves the open screen's cursor one row up
    bool downDown = false;    // a row key down (the down arrow, S)
    bool confirmDown = false; // Enter, a click or pad A: a press shuts the open screen on the cursor's row
    bool cancelDown = false;  // Esc or pad B: a press shuts the open screen with no pick
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
    bool partnerHere = true;        // as of the last step (UiPartyInput::partnerHere)
    int lead = -1;                  // as of the last step (UiPartyInput::lead)
    float openFor = 0.f;            // seconds open
    bool moved = false;             // a move was tried since it opened (a flick or a row key)
    bool byPress = false;           // opened by a press (Party's key, or the fight's map key), not explore's map hold
    bool stays = false;             // opened by a tap: stands until a confirm, a cancel or Party's next press
    bool upWas = true, downWas = true, confirmWas = true, cancelWas = true; // the screen's own keys, as of the last step
};

struct UiPartyOut {
    bool opened = false;
    bool closed = false;
    bool map = false;                    // explore: the map key's short press, let go
    UiPartyRow pick = UiPartyRow::Lead;  // with `closed`: the cursor's row (a cancel: Lead)
    bool picked() const { return closed && pick != UiPartyRow::Lead; }
};

FTCORE_API UiPartyOut uiPartyScreenStep(UiPartyScreen& s, const UiPartyInput& in, float dt);

// A scripted pick of the partner's row, as the player would make it (a dev probe of the lead switch), t seconds from the
// pick: from kUiPartyProbeFrom the input is live with the keys up (a key already down when input returns is no press),
// Party held from kUiPartyProbeHold, the stick flicked up over [kUiPartyProbeFlick, kUiPartyProbeFlickEnd), let go at 0
// (the pick); held again over [kUiPartyProbeReopen, kUiPartyProbeReopenEnd) so the screen stands open on the new lead,
// then up until kUiPartyProbeTo, when the release has shut it on the lead's row (no pick). Fills `in` and returns true
// inside [kUiPartyProbeFrom, kUiPartyProbeTo); outside, leaves it and returns false.
inline constexpr float kUiPartyProbeFrom = -1.2f;
inline constexpr float kUiPartyProbeHold = -1.f;
inline constexpr float kUiPartyProbeFlick = -0.6f;
inline constexpr float kUiPartyProbeFlickEnd = -0.4f;
inline constexpr float kUiPartyProbeReopen = 0.4f;
inline constexpr float kUiPartyProbeReopenEnd = 1.6f;
inline constexpr float kUiPartyProbeTo = 1.8f;
FTCORE_API bool uiPartyProbeInput(float t, UiPartyInput& in);

// Whether the cursor may rest on a row: the lead, the partner while one is in the fight (partnerHere), and Follow in
// explore; never the Third.
FTCORE_API bool uiPartyRowSelectable(UiPartyRow r, bool explore, bool partnerHere = true);

// Why a lead row's switch would be refused now: its word after the name, the row at 40% ink. Unknown: no session to
// ask (the row stands undimmed, no word); Ok: taken.
enum class UiPartyWhy : std::uint8_t { Unknown, Ok, Down, NotHere, FightOver, Wait };
inline constexpr std::size_t kUiPartyWhyCount = 6;
FTCORE_API bool uiPartyRefused(UiPartyWhy w);
// The session's answer for a lead (FightSession::leadSwitchReason, the preview, or FightTick::switchRefusal, the
// close's refusal) as the row's word: Ok and Same Ok (no word: Same is the controlled lead's own row), Down DOWN,
// NotInFight and Lone NOT HERE, FightOver FIGHT OVER, Cooldown WAIT. Each answer has its case, so a new one fails
// the Core build (-Wswitch) until it has a word.
FTCORE_API UiPartyWhy uiPartyWhyOf(LeadSwitch r);
// A lead row's word as the host gives it: the session's answer (uiPartyWhyOf), except that a lead with no body in this
// fight is NOT HERE whatever the session says (the one host-side condition: a lead with no body cannot be controlled).
// The partner's row takes the cursor exactly when this is not NotHere (UiPartyInput::partnerHere, uiPartyPartnerHere).
FTCORE_API UiPartyWhy uiPartyLeadWhy(LeadSwitch session, bool body);
FTCORE_API bool uiPartyPartnerHere(UiPartyWhy w);

// The open screen's rows as drawn (the HUD measures the names and draws; the closed block keeps its own inks, and the
// HUD eases from those to these as the screen opens).
inline constexpr float kUiPartyOtherInk = 0.7f;   // a lead row (or Follow) off the cursor
inline constexpr float kUiPartyRefusedInk = 0.4f; // a refused lead row, its word too, on the cursor or off it
inline constexpr float kUiPartyThirdInk = 0.7f;   // the Third's row, never the cursor
// The current lead's dot: a filled circle kUiPartyDotCap of the name's cap height across, centred on its capitals,
// hung kUiPartyDotGap of the cap height before the name's left (in the margin: the name keeps its place over its line).
inline constexpr float kUiPartyDotCap = 0.35f;
inline constexpr float kUiPartyDotGap = 0.35f;

struct UiPartyRowView {
    float ink = 1.f;        // the name's
    float underlineW = 0.f; // the cursor's underline from the name's left, as long as the name (0: none)
    bool dot = false;       // the current lead's dot before the name
};

// A row of the open screen: the cursor's row at full ink with the underline across its measured name (nameW px); a
// refused lead row at kUiPartyRefusedInk, the cursor's or not (the cursor still underlines it); every other lead row
// and Follow at kUiPartyOtherInk; the Third at kUiPartyThirdInk, never underlined. The dot marks the controlled lead's
// row (Lead) whether the cursor is on it or not. `why` counts on the partner's and the lead's rows only. Closed: the
// plain row (full ink, nothing marked).
FTCORE_API UiPartyRowView uiPartyRowView(const UiPartyScreen& s, UiPartyRow r, UiPartyWhy why, float nameW);

struct UiPartyDot {
    float x = 0.f, y = 0.f; // centre, px
    float r = 0.f;
};
// The dot's place for a name drawn from nameX whose capitals are capH px tall, their middle at capMid.
FTCORE_API UiPartyDot uiPartyDot(float nameX, float capMid, float capH);

} // namespace ftcore
