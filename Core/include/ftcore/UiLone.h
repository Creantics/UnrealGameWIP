#pragma once
// The Lone Carry on the fight HUD (LoneCarry.h): the bargain card as it begins, the word in the lead's state slot while
// it lasts and for a moment after it ends, and the roll's lines (built here, drawn nowhere yet).
//
//   UiLone s;
//   each tick: uiLoneStep(s, {fight live, alone, began, full, attack, endedBy}, dt)
//   draw: the card at uiLoneCardAlpha(s) (s.full: the columns, else the title alone), its lines from uiLoneCard(bargain)
//         (built when the bargain changes); the slot's word by uiLeadWord(gauge slot, uiLoneWord(s))
//
// Pure: the host feeds what the fight and the session report. Lines are kinds with their figures; the HUD maps each
// kind to its words. No allocation.
#include "ftcore/Api.h"
#include "ftcore/LoneCarry.h"
#include "ftcore/UiViews.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ftcore {

// A figure after a line's words: "×1.3", "100", "+14.5%" (UTF-8, NUL-terminated; empty: none).
struct UiLoneFigure {
    char text[12] = {};
    std::string_view view() const;
    bool empty() const { return text[0] == 0; }
};

// The bargain card's lines, by the LoneBargain field each comes from.
enum class UiLoneLine : std::uint8_t {
    Release,     // release:          RELEASE ×1.3 (only above ×1)
    Load,        // loadGain:         LOAD ×1.5 (only above ×1)
    LoadCap,     // loadCap:          LOAD CAP 100 (only above 0)
    DamageTaken, // damageTaken:      DAMAGE TAKEN ×1.15 (only above ×1)
    NoCount,     // noCount:          THE COUNT
    NoTable,     // noTable:          THE TABLE
    NoTakeHalf,  // noTakeHalf:       TAKE HALF
    GiftEnds,    // giftReceivedEnds: A GIFT RECEIVED ENDS IT (always last)
};
inline constexpr std::size_t kUiLoneLineKinds = 8;

struct UiLoneCardLine {
    UiLoneLine kind = UiLoneLine::Release;
    UiLoneFigure figure;
};

inline constexpr std::size_t kUiLoneGainMax = 3; // YOU GAIN
inline constexpr std::size_t kUiLoneGiveMax = 5; // YOU GIVE UP

struct UiLoneCard {
    std::array<UiLoneCardLine, kUiLoneGainMax> gain{};
    std::uint8_t gainCount = 0;
    std::array<UiLoneCardLine, kUiLoneGiveMax> give{};
    std::uint8_t giveCount = 0;
};

// The card's two columns from the bargain: gains in field order, then what is given up (damage taken, each refusal
// that holds, and last the gift's end). A multiplier at ×1 (to the hundredth) or below changes nothing and has no line.
FTCORE_API UiLoneCard uiLoneCard(const LoneBargain& b);

// The roll's lines (LoneRoll): none until it is shown (the first lone Release).
enum class UiLoneRollLine : std::uint8_t {
    Releases,    // releases:                      "12"
    Pooled,      // pooled:                        "340"
    Extra,       // effect.totalPermille:          "+14.5%" (only above 0)
    Figures,     // effect.figures:                "1" (only above 0)
    FigureShare, // effect.figurePermille:         "15%" (with Figures)
    BossHp,      // effect.hpPermille:             "+9.5%" (only above 0)
};
inline constexpr std::size_t kUiLoneRollMax = 6;

struct UiLoneRollRow {
    UiLoneRollLine kind = UiLoneRollLine::Releases;
    UiLoneFigure figure;
};

struct UiLoneRoll {
    std::array<UiLoneRollRow, kUiLoneRollMax> rows{};
    std::uint8_t count = 0;
};
FTCORE_API UiLoneRoll uiLoneRoll(const LoneRoll& r);

// ---- when the card and the words show ----

inline constexpr float kUiLoneCardHold = 6.f;   // the card's full-ink seconds, unless an attack comes first
inline constexpr float kUiLoneCardFade = 0.5f;
inline constexpr float kUiLoneEndedShow = 1.5f; // the end's word in the slot

enum class UiLoneWord : std::uint8_t { None, Alone, EndedGift, EndedPartner };

struct UiLoneInput {
    bool fight = false;  // a fight under way (false: none, or over; everything goes and nothing shows at its end)
    bool alone = false;  // the lead carries alone now
    bool began = false;  // it began this tick (the host's edge on its bargain coming due): the card shows
    bool full = false;   // with `began`: the whole card (the first three showings in a save), else the title alone
    bool attack = false; // an attack this tick: a holding card starts its fade
    LoneEnd endedBy = LoneEnd::None; // why it ended, as the session reports, read on the tick `alone` falls
};

struct UiLone {
    bool card = false;    // the card stands (holding or fading)
    bool full = false;
    bool holding = false;
    float held = 0.f;     // seconds it has held
    float fadeLeft = 0.f;
    bool alone = false;   // as of the last step
    UiLoneWord ended = UiLoneWord::None;
    float endedLeft = 0.f;
};

// The card holds kUiLoneCardHold from `began`, or until an attack or the end of the Lone Carry, then fades over
// kUiLoneCardFade. When `alone` falls, a gift's or the partner's end shows its word for kUiLoneEndedShow; the fight's
// own end shows nothing.
FTCORE_API void uiLoneStep(UiLone& s, const UiLoneInput& in, float dt);
FTCORE_API float uiLoneCardAlpha(const UiLone& s);
// ALONE while it lasts, else the end's word while it shows, else none.
FTCORE_API UiLoneWord uiLoneWord(const UiLone& s);

// The lead's state slot, right of the name: one word, by priority RELEASE, LOAD, then the Lone Carry's word (ALONE,
// or the end's in its place).
enum class UiLeadWord : std::uint8_t { None, Release, Load, Alone, EndedGift, EndedPartner };
FTCORE_API UiLeadWord uiLeadWord(UiLeadSlot gauge, UiLoneWord lone);

} // namespace ftcore
