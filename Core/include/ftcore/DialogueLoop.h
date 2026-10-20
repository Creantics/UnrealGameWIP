#pragma once

// The dialogue loop: the real screen behind CHC and HE gameplay steps (GameplayScreen::Choice). A
// fixed five-step shape, played over one or more rounds:
//
//   1. Claim     the host states the problem (one line).
//   2. Audit     the player picks one of 2-4 options.
//   3. Escalate  a wrong pick: the host answers with that option's own correction line, and the
//                option greys out (it can't be picked again). Back to Audit.
//   4. Concede   the round's second miss: the host's concede line (a new line, not the option's
//                correction). The third miss: the companion supplies the answer in their own voice
//                (the round's companion line), and that resolves the round.
//   5. Tag       after the last round: a closing exchange (one or more lines); then the loop is done
//                and its flags are due.
//
// A right pick resolves the round (its optional reply line plays first); the next round's Claim
// follows, or the Tag after the last. At most kMaxPicks picks across the whole loop: when the cap is
// reached the loop goes to its end after the current line (a wrong last pick plays the companion line
// as for a third miss). No score, no fail state. skip() at any point goes straight to the Tag.
//
// A no-resolve loop has no Concede and no Tag: misses only escalate, and skip() or the end of the
// last round finishes the loop at once; its flags are due on that exit. A round may have every
// option right.
//
//   ftcore::DialogueLoopDef def;
//   if (!ftcore::importDialogueLoop(text, def, &error, &warnings)) ...;   // load time, allocates
//   ftcore::DialogueLoop loop;
//   loop.begin(&def);
//   draw loop.speaker(), loop.text(); when loop.optionsVisible(), options 0..optionCount()-1,
//       greyed when loop.optionGreyed(i)
//   on continue: loop.press();  on option i: loop.choose(i);  on skip: loop.skip();
//   headless:    loop.apply(loop.autoplay());   // the skip path: Skip, then press through the Tag
//   when loop.finished(): set flags flagId(i) = flagValue(i) for i < flagCount()
//
// Loop files hold story text, so they live in the gitignored Data/Story/loops/, one per gameplay
// step: dialogueLoopFileName(scene, beat, step) ("SC-X_4_0.json"). The format is in
// docs/CHOICE_SCREEN.md; Data/Choice/example_loop.json is an invented example.
//
// The runtime (DialogueLoop) never allocates: it holds a pointer to the definition and hands back
// string_views into it, so the definition must outlive it. Errors and warnings from the loader name
// paths and keys only, never line text.

#include "ftcore/Api.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

struct DialogueLoopLine {
    std::string speaker; // a speaker id as the script writes it ("host", "companion")
    std::string text;
    bool empty() const { return text.empty(); }
};

struct DialogueLoopOption {
    std::string text;
    bool right = false;
    // Wrong: the host's correction (Escalate), required. Right: an optional reply before moving on.
    DialogueLoopLine reply;
};

struct DialogueLoopRound {
    static constexpr std::uint32_t kMaxOptions = 4;

    DialogueLoopLine claim;
    DialogueLoopOption options[kMaxOptions];
    std::uint32_t optionCount = 0;
    DialogueLoopLine concede;   // the second miss (resolving loops; required with 2+ wrong options)
    DialogueLoopLine companion; // the third miss (resolving loops; required with 3 wrong options)

    FTCORE_API std::uint32_t wrongCount() const;
    // The most picks this round can take: one with no wrong option, else min(wrong + 1, 3).
    FTCORE_API std::uint32_t worstCasePicks() const;
};

struct DialogueLoopFlag {
    std::string id;
    std::string value = "true"; // as UFTStorySubsystem::SetFlag reads it: "true", "false", an integer or a string value
};

struct DialogueLoopDef {
    static constexpr std::uint32_t kMaxRounds = 5;
    static constexpr std::uint32_t kMaxTagLines = 8;
    static constexpr std::uint32_t kMaxFlags = 8;
    static constexpr std::uint32_t kMaxPicks = 5;

    std::string scene;
    std::uint32_t beat = 0, step = 0;
    bool resolve = true; // false: a no-resolve loop (no Concede, no Tag)
    DialogueLoopRound rounds[kMaxRounds];
    std::uint32_t roundCount = 0;
    DialogueLoopLine tag[kMaxTagLines];
    std::uint32_t tagCount = 0;
    DialogueLoopFlag flags[kMaxFlags];
    std::uint32_t flagCount = 0;

    bool matches(std::string_view sceneId, std::uint32_t beatIndex, std::uint32_t stepIndex) const {
        return scene == sceneId && beat == beatIndex && step == stepIndex;
    }
    // Sum of the rounds' worst-case picks (may exceed kMaxPicks; the cap then ends the loop early).
    FTCORE_API std::uint32_t worstCasePicks() const;
};

// Parses and validates a loop file. On failure `out` is reset and *error lists each problem with
// its path, one per line. Warnings (unknown keys, ignored fields, a worst case past the pick cap, no
// flags) are appended to *warnings; their count is warnings->size().
//
// Errors: bad JSON; version not 1; scene, beat or step missing; no rounds or more than kMaxRounds; a
// missing claim; fewer than 2 or more than 4 options; an option without text or `right`; a round
// with no right option; a wrong option without a reply; in a resolving loop, a round with 2+ wrong
// options and no concede line, or 3 wrong options and no companion line, and no tag line (or more
// than kMaxTagLines); more than kMaxFlags flags or a flag without an id; a malformed line.
FTCORE_API bool importDialogueLoop(std::string_view json, DialogueLoopDef& out, std::string* error = nullptr,
                                   std::vector<std::string>* warnings = nullptr);

// "<scene>_<beat>_<step>.json". Allocates (load time).
FTCORE_API std::string dialogueLoopFileName(std::string_view scene, std::uint32_t beat, std::uint32_t step);

enum class DialogueLoopState : std::uint8_t {
    Idle,      // no loop
    Claim,     // the round's claim line
    Audit,     // waiting for a pick (the claim stays on screen)
    Escalate,  // a wrong option's correction
    Concede,   // the round's concede line (second miss)
    Companion, // the round's companion line (third miss, or a wrong pick at the cap): resolves the round
    Reply,     // a right option's reply
    Tag,       // the closing lines
    Done       // finished: the flags are due
};

// "idle", "claim", "audit", "escalate", "concede", "companion", "reply", "tag", "done"
FTCORE_API std::string_view dialogueLoopStateName(DialogueLoopState state);

struct DialogueLoopCommand {
    enum class Kind : std::uint8_t { None, Press, Choose, Skip };
    Kind kind = Kind::None;
    std::uint32_t option = 0; // Choose

    bool none() const { return kind == Kind::None; }
};

class DialogueLoop {
public:
    // Starts at the first round's claim; a null definition (or one with no round) leaves it Idle.
    FTCORE_API void begin(const DialogueLoopDef* def);
    FTCORE_API void reset();

    const DialogueLoopDef* def() const { return def_; }
    DialogueLoopState state() const { return state_; }
    bool active() const { return state_ != DialogueLoopState::Idle && state_ != DialogueLoopState::Done; }
    bool finished() const { return state_ == DialogueLoopState::Done; }
    // Bumped on every change of what is shown (owners re-cache their strings then).
    std::uint32_t epoch() const { return epoch_; }

    // ---- what to show ----
    // The current line: the claim (also during Audit), a reply, the concede or companion line, or a
    // tag line. Empty when Idle or Done.
    FTCORE_API std::string_view speaker() const;
    FTCORE_API std::string_view text() const;
    bool optionsVisible() const { return state_ == DialogueLoopState::Audit; }
    // The current round's options (0 when there is no round).
    FTCORE_API std::uint32_t optionCount() const;
    FTCORE_API std::string_view optionText(std::uint32_t i) const;
    // A used wrong option: shown dim, can't be picked.
    bool optionGreyed(std::uint32_t i) const { return i < 32 && (greyed_ & (1u << i)) != 0; }
    FTCORE_API bool optionSelectable(std::uint32_t i) const;

    std::uint32_t round() const { return round_; }
    FTCORE_API std::uint32_t roundCount() const;
    std::uint32_t tagLine() const { return tag_; }
    std::uint32_t misses() const { return misses_; } // in the current round
    std::uint32_t picks() const { return picks_; }   // across the loop
    bool skipped() const { return skipped_; }
    // Rounds resolved by the companion line.
    std::uint32_t companionResolves() const { return companionResolves_; }

    // ---- input ----
    // Continue: moves past the current line. False in Audit (pick an option), Idle or Done.
    FTCORE_API bool press();
    // Picks option i in Audit. False when not in Audit, i is out of range or greyed.
    FTCORE_API bool choose(std::uint32_t i);
    // Straight to the Tag (a no-resolve loop finishes). False when Idle, Done or already in the Tag.
    FTCORE_API bool skip();
    // The move an automatic player makes: Skip, then Press through the Tag; None when finished.
    FTCORE_API DialogueLoopCommand autoplay() const;
    FTCORE_API bool apply(const DialogueLoopCommand& command);

    // ---- flags (due once finished()) ----
    FTCORE_API std::uint32_t flagCount() const;
    FTCORE_API std::string_view flagId(std::uint32_t i) const;
    FTCORE_API std::string_view flagValue(std::uint32_t i) const;

private:
    const DialogueLoopRound* currentRound() const;
    const DialogueLoopLine* currentLine() const;
    void setState(DialogueLoopState s);
    void startRound(std::uint32_t round);
    // After a resolving line (a right pick, its reply, a companion line): the next round or the end.
    void finishRound();
    void toEnd();

    const DialogueLoopDef* def_ = nullptr;
    DialogueLoopState state_ = DialogueLoopState::Idle;
    std::uint32_t epoch_ = 0;
    std::uint32_t round_ = 0;
    std::uint32_t tag_ = 0;
    std::uint32_t misses_ = 0;
    std::uint32_t picks_ = 0;
    std::uint32_t greyed_ = 0;   // bit per option of the current round
    std::uint32_t lastPick_ = 0; // the option whose reply is showing
    std::uint32_t companionResolves_ = 0;
    bool skipped_ = false;
};

} // namespace ftcore
