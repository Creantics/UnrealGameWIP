#pragma once

// Runtime cursor over a StoryDatabase: (scene, beat) plus the story flags.
//
//   StoryRunner r(&db);
//   r.setCueHandler(&onCue, this);          // optional
//   r.start();                              // the story's start scene (or start("SC-07"))
//   each frame:
//     r.update(dt);                         // counts down holds
//     switch (r.state()) {
//     case StoryRunner::State::Line:
//     case StoryRunner::State::Direction:   show r.text(); on confirm r.advance();
//     case StoryRunner::State::Choice:      show r.text() + r.optionText(i); r.choose(i);
//     case StoryRunner::State::Hold:        show nothing new; the runner moves on when the hold ends
//     case StoryRunner::State::Gameplay:    hand off by r.gameplayKind() / r.param(...); r.resume() when done
//     case StoryRunner::State::Finished:    the story is over
//     }
//
// The cursor only ever rests on a line, direction, choice, hold or gameplay beat. Cue, flag and
// end beats are processed on the way: a cue calls the cue handler, a flag op changes the flags (a
// failed `require` skips past the next flag beat with op End in the scene, or to the scene's end
// when there is none), and `end` moves to its target or the next scene in chapter order (finished
// after the last one). Falling off the last beat of a scene counts as an `end` without a target.
// Hidden directions and choices with no selectable option are passed over as well; an option
// whose `next` is its own choice shows the choice again (beatEpoch() changes).
//
// Text comes back as views into the database: the script's text, plus textKey() / optionTextKey()
// for the owner to look up a localised string.
//
// Zero allocation: after setDatabase() (which sizes the flag array), start/advance/choose/update/
// resume, scene changes, cue callbacks and restore() allocate nothing.

#include "ftcore/Api.h"
#include "ftcore/StoryDatabase.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ftcore {

struct CueEvent {
    std::string_view id;   // cue id
    std::string_view kind; // declared cue kind ("music", "sfx", ...; "" when undeclared)
    CueAction action = CueAction::Play;
    std::uint32_t cue = kNone; // index into StoryDatabase::cue()
    std::uint32_t scene = kNone, beat = 0;
};

// Saved cursor. The scene is stored by id so a save survives re-imports that reorder scenes.
struct StoryPosition {
    std::string_view scene; // id (a view: copy it into the save)
    std::uint32_t beat = 0;
};

class StoryRunner {
public:
    enum class State : std::uint8_t { Idle, Line, Direction, Choice, Hold, Gameplay, Finished };
    using CueHandler = void (*)(void* user, const CueEvent& e);

    explicit StoryRunner(const StoryDatabase* db = nullptr) { setDatabase(db); }
    StoryRunner(const StoryRunner&) = delete;
    StoryRunner& operator=(const StoryRunner&) = delete;

    // Sizes the flag storage for `db` (allocates once per database) and stops the runner.
    FTCORE_API void setDatabase(const StoryDatabase* db);
    const StoryDatabase* database() const { return db_; }
    void setCueHandler(CueHandler h, void* user) {
        onCue_ = h;
        cueUser_ = user;
    }

    // Resets the flags to their defaults and starts at `sceneId` (empty: the story's start scene).
    // False (and Finished) when the scene is unknown or there is no database.
    FTCORE_API bool start(std::string_view sceneId = {});
    // Moves the cursor to (scene, beat) without touching the flags (set them first with setFlag),
    // as a load does: the beat is entered again (a hold restarts, a gameplay beat is requested
    // again). A beat past the end of the scene ends the scene. False when the scene is unknown.
    FTCORE_API bool restore(const StoryPosition& pos);
    FTCORE_API void stop(); // back to Idle

    State state() const { return state_; }
    bool finished() const { return state_ == State::Finished; }
    bool waitingForGameplay() const { return state_ == State::Gameplay; }
    // True when the last move processed kMaxStepsPerMove beats without resting (a loop of empty
    // scenes) and gave up: the runner is then Finished.
    bool stalled() const { return stalled_; }

    // ---- cursor ----
    std::uint32_t sceneIndex() const { return scene_; }
    std::uint32_t beatIndex() const { return beat_; }
    FTCORE_API StoryPosition position() const;
    const Scene* scene() const { return scene_ == kNone ? nullptr : &db_->scene(scene_); }
    FTCORE_API const Beat* beat() const;
    // Bumped every time a scene is entered (UI: show the title card when it changes).
    std::uint32_t sceneEpoch() const { return sceneEpoch_; }
    // Bumped every time the cursor rests on a new beat (UI: restart the typewriter when it changes).
    std::uint32_t beatEpoch() const { return beatEpoch_; }

    // ---- current beat, as views into the database ----
    FTCORE_API std::string_view text() const;      // line / direction / choice prompt / gameplay notation; "" otherwise
    FTCORE_API std::string_view textKey() const;   // loc key of text() ("" when none)
    FTCORE_API std::string_view speaker() const;   // line speaker label; "" otherwise
    FTCORE_API std::string_view direction() const; // line delivery tag; "" otherwise
    FTCORE_API std::string_view lineId() const;    // line id; "" otherwise
    FTCORE_API std::uint32_t optionCount() const;  // choice options (0 when not on a choice)
    FTCORE_API std::string_view optionText(std::uint32_t i) const;
    FTCORE_API std::string_view optionTextKey(std::uint32_t i) const;
    FTCORE_API bool optionSelectable(std::uint32_t i) const; // false: greyed out (choose() refuses it)
    // The option does not move the story on: it leads back to this same choice, or it is a miss (a
    // wrong pick in a dialogue exchange, answered by a correction and the prompt again).
    FTCORE_API bool optionReturns(std::uint32_t i) const;
    // The option an automatic player should take: the first selectable option that moves the
    // story on (kNone when not on a choice).
    FTCORE_API std::uint32_t defaultOption() const;
    FTCORE_API bool scripted() const; // a scripted choice (one outcome)
    float holdRemaining() const { return state_ == State::Hold ? hold_ : 0.f; }
    FTCORE_API GameplayKind gameplayKind() const;
    FTCORE_API std::string_view gameplayKindName() const;
    FTCORE_API std::string_view param(std::string_view key) const; // gameplay param as text ("" when absent)
    FTCORE_API std::uint32_t paramCount() const;                   // 0 when not on a gameplay beat
    FTCORE_API std::string_view paramKey(std::uint32_t i) const;
    FTCORE_API std::string_view paramValue(std::uint32_t i) const;

    // ---- moving ----
    FTCORE_API bool advance();               // Line / Direction -> next beat. False otherwise.
    FTCORE_API bool choose(std::uint32_t i); // Choice: runs option i's effects and jumps. False when refused.
    FTCORE_API void update(float dt);        // counts down a hold; continues when it ends
    FTCORE_API bool resume();                // Gameplay: the gameplay is done; continue. False otherwise.
    FTCORE_API void skipHold();              // ends the current hold now (debug / autoplay)

    // ---- flags ----
    std::size_t flagCount() const { return flags_.size(); }
    FlagValue flag(std::uint32_t index) const { return index < flags_.size() ? flags_[index] : FlagValue{}; }
    FTCORE_API FlagValue flag(std::string_view id) const; // {} when unknown
    FTCORE_API bool setFlag(std::uint32_t index, FlagValue v);
    FTCORE_API bool setFlag(std::string_view id, FlagValue v); // false when the flag is unknown
    FTCORE_API void resetFlags();

    // Number of beats processed in one go before the runner gives up (a loop of empty scenes).
    static constexpr std::uint32_t kMaxStepsPerMove = 100000;

private:
    void enterScene(std::uint32_t scene);
    void endScene(std::uint32_t next);
    void settle(); // processes pass-through beats until the cursor rests or the story ends
    void applyFlagOp(const FlagOpDef& op);
    void skipRequireBlock();
    void rest(State s);

    const StoryDatabase* db_ = nullptr;
    std::vector<FlagValue> flags_;
    CueHandler onCue_ = nullptr;
    void* cueUser_ = nullptr;
    State state_ = State::Idle;
    std::uint32_t scene_ = kNone, beat_ = 0;
    std::uint32_t sceneEpoch_ = 0, beatEpoch_ = 0;
    float hold_ = 0.f;
    bool stalled_ = false;
};

} // namespace ftcore
