#pragma once

// A gameplay beat's `mode` param lowered to the ordered screens that play it.
//
//   ftcore::GameplayPlan plan;
//   plan.lower(runner);                     // the runner's current gameplay beat
//   for (std::uint32_t i = 0; i < plan.size(); ++i) {
//       const ftcore::GameplayStep& s = plan.step(i);
//       open s.screen; show s.tokenName() and s.label()
//   }
//
// The mode is designer notation, a chain of tokens:
//   - separators `→` (or `->`), `+` and `,` all flatten to one sequence, in written order;
//   - a parenthetical after a token, as in `CMB (duel)`, becomes that step's label (two of them
//     join with ", ");
//   - a bare lower-case word (no upper-case letter, as `intercut`) is a qualifier too: it joins the
//     previous step's label (or the next step's when nothing came before);
//   - a token may be multi-word (`co-op QTE`); separators inside parentheses don't split;
//   - quotes and a list wrapper around the whole mode (`('ENG', 'CMB')`) are ignored.
//
// Token to screen: CMB battle; ENG and PLAY field; CHC and HE (a dialogue loop) choice; QTE and
// `co-op QTE` qte; CG cutscene; BNT and MINI the stub screen. Any other token goes to the stub screen as well and counts
// in unknownTokens(). Choice (the dialogue loop) and Cutscene open their own screens; Qte opens the stub
// screen until its own screen exists (screenRoutesTo in Dialogue.h). An empty mode (or one with no step in it) falls back to the beat's kind, lowered the same way.
//
// Fixed capacity (kMaxSteps): steps past it are dropped and counted (droppedSteps()). Token names
// and labels are copied into fixed buffers (cut at a UTF-8 character boundary when too long), so a
// plan outlives the string it was lowered from. Zero allocation.

#include "ftcore/Api.h"
#include "ftcore/Dialogue.h"
#include "ftcore/StoryDatabase.h"
#include "ftcore/StoryRunner.h"

#include <cstdint>
#include <string_view>

namespace ftcore {

enum class GameplayToken : std::uint8_t { Cmb, Eng, Play, Chc, Bnt, Cg, He, Qte, CoopQte, Mini, Unknown };

// "CMB", "ENG", ..., "co-op QTE", "MINI"; "" for Unknown.
FTCORE_API std::string_view gameplayTokenName(GameplayToken token);
// The token written as `name` (ASCII case-insensitive); Unknown for anything else.
FTCORE_API GameplayToken gameplayTokenFromName(std::string_view name);
// Battle for CMB, Field for ENG and PLAY, Choice for CHC and HE, Qte for QTE and co-op QTE, Cutscene
// for CG, Stub for everything else (Unknown included).
FTCORE_API GameplayScreen gameplayTokenScreen(GameplayToken token);

struct GameplayStep {
    static constexpr std::uint32_t kNameCapacity = 16;
    static constexpr std::uint32_t kLabelCapacity = 40;

    GameplayToken token = GameplayToken::Unknown;
    GameplayScreen screen = GameplayScreen::Stub;
    char name[kNameCapacity] = {};   // canonical name; an unknown token as written
    char labelText[kLabelCapacity] = {};
    std::uint8_t nameLength = 0, labelLength = 0;

    std::string_view tokenName() const { return {name, nameLength}; }
    // The step's qualifiers ("duel", "loop, intercut"); "" when none.
    std::string_view label() const { return {labelText, labelLength}; }
};

class GameplayPlan {
public:
    static constexpr std::uint32_t kMaxSteps = 8;

    FTCORE_API void clear();
    // Lowers `mode`; when it holds no step, lowers `fallbackKind` (the beat's kind as written)
    // instead. `minutes`: the beat's `minutes` param as text ("" when absent). False when the plan
    // ends up empty.
    FTCORE_API bool lower(std::string_view mode, std::string_view fallbackKind = {}, std::string_view minutes = {});
    // A gameplay beat of `db` (its `mode` and `minutes` params, its kind as the fallback).
    FTCORE_API bool lower(const StoryDatabase& db, const Beat& gameplay);
    // The runner's current beat; false (and empty) when it is not on a gameplay beat.
    FTCORE_API bool lower(const StoryRunner& runner);

    std::uint32_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    const GameplayStep& step(std::uint32_t i) const { return steps_[i]; }
    // Steps that open `screen`.
    FTCORE_API std::uint32_t count(GameplayScreen screen) const;

    // Tokens that are not in the known set (each one is still a stub step).
    std::uint32_t unknownTokens() const { return unknown_; }
    // Steps past kMaxSteps, not kept.
    std::uint32_t droppedSteps() const { return dropped_; }
    std::uint32_t warnings() const { return unknown_ + dropped_; }
    // The beat's `minutes` param (a rough play length for tuning); 0 when absent or unreadable.
    std::uint32_t minutes() const { return minutes_; }

private:
    void lowerChain(std::string_view mode);
    void addPiece(std::string_view piece);
    void addStep(std::string_view token, std::string_view label);
    void addQualifier(std::string_view qualifier);

    GameplayStep steps_[kMaxSteps];
    std::uint32_t count_ = 0;
    std::uint32_t unknown_ = 0, dropped_ = 0;
    std::uint32_t minutes_ = 0;
    char pending_[GameplayStep::kLabelCapacity] = {}; // a qualifier before the first step
    std::uint8_t pendingLength_ = 0;
};

} // namespace ftcore
