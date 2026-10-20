#pragma once

// What the dialogue box shows for a StoryRunner, and what a press means.
//
//   DialoguePresenter p(&runner);
//   p.setRevealRate(40.f);                  // characters per second (0: whole text at once)
//   each frame:
//     p.update(dt);                         // follows the runner's beat; types the text out
//     draw p.revealedText(); when p.optionsVisible(), the options
//   on confirm:   apply(p.press())          // first press finishes the text, the next one moves on
//   on option i:  apply(p.choose(i))
//   autoplay:     apply(p.autoplay())       // once the text is out, the move an automatic player makes
//
// The presenter never moves the runner itself: it hands back a DialogueCommand and the owner
// applies it (in Unreal through the story subsystem, so its events fire). Characters are counted
// in UTF-8 codepoints, so a multibyte character appears whole.
//
// A gameplay beat waits in the box. With hand-off off (the default) press() resumes it; with
// hand-off on, a beat that has a screen (gameplayScreen()) hands off instead and the owner resumes
// the story when that screen is done. A kind with no screen always resumes. A hold shows nothing and
// press() does nothing (the runner moves on when the hold ends); autoplay() skips it.
//
// Zero allocation.

#include "ftcore/Api.h"
#include "ftcore/StoryRunner.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ftcore {

// Where a gameplay beat is played (story_format.md: CMB battle; ENG and PLAY field; anything else
// has no screen and waits in the dialogue box). Stub is the placeholder screen a GameplayPlan step
// with no real screen yet opens (GameplayPlan.h); gameplayScreen() never returns it.
// Choice, Qte and Cutscene are the screens a GameplayPlan step names for CHC, QTE (and co-op QTE)
// and CG / HE; each is owned by its own stream and routes to Stub until that screen exists.
enum class GameplayScreen : std::uint8_t { Box, Battle, Field, Stub, Choice, Qte, Cutscene };
inline constexpr std::uint32_t kGameplayScreenCount = 7;
FTCORE_API GameplayScreen gameplayScreen(GameplayKind kind);
// "box", "battle", "field", "stub", "choice", "qte", "cutscene"
FTCORE_API std::string_view gameplayScreenName(GameplayScreen screen);

// The screen that really opens for `screen`. A stream that builds its screen changes only its own
// case here (Choice, Qte or Cutscene returns itself); the others keep returning Stub.
constexpr GameplayScreen screenRoutesTo(GameplayScreen screen) {
    switch (screen) {
    case GameplayScreen::Choice:
    case GameplayScreen::Qte:
    case GameplayScreen::Cutscene: return GameplayScreen::Stub;
    case GameplayScreen::Box:
    case GameplayScreen::Battle:
    case GameplayScreen::Field:
    case GameplayScreen::Stub: break;
    }
    return screen;
}

struct DialogueCommand {
    // Handoff: open the gameplay beat's screen (option holds the GameplayScreen); the runner stays
    // on the beat until the owner resumes it.
    enum class Kind : std::uint8_t { None, Advance, Choose, Resume, SkipHold, Handoff };
    Kind kind = Kind::None;
    std::uint32_t option = kNone; // Choose: the option. Handoff: the GameplayScreen.

    bool none() const { return kind == Kind::None; }
};

// Applies `c` to `r` directly (tests and owners without events). False when the runner refused it
// or there is nothing to apply (None, Handoff).
FTCORE_API bool applyDialogueCommand(StoryRunner& r, const DialogueCommand& c);

// Number of codepoints in UTF-8 `text` (continuation bytes are not counted).
FTCORE_API std::uint32_t utf8Length(std::string_view text);
// Byte length of the first `chars` codepoints of `text` (all of it when there are fewer).
FTCORE_API std::size_t utf8PrefixBytes(std::string_view text, std::uint32_t chars);

class DialoguePresenter {
public:
    explicit DialoguePresenter(const StoryRunner* runner = nullptr) { setRunner(runner); }

    FTCORE_API void setRunner(const StoryRunner* runner);
    const StoryRunner* runner() const { return runner_; }
    // Characters per second; 0 or less shows each text whole at once.
    void setRevealRate(float charsPerSecond) { rate_ = charsPerSecond; }
    float revealRate() const { return rate_; }
    // On: press() and autoplay() hand gameplay beats with a screen off instead of resuming them.
    void setHandoff(bool on) { handoff_ = on; }
    bool handoff() const { return handoff_; }

    // Picks up a new beat (restarting the typewriter). True when the beat changed. update(),
    // press() and choose() call it; call it yourself after moving the runner to read the new beat
    // before the next update.
    FTCORE_API bool sync();
    // sync(), then types out dt worth of characters.
    FTCORE_API void update(float dt);

    // ---- what to show ----
    StoryRunner::State state() const { return state_; }
    std::uint32_t beatEpoch() const { return epoch_; }
    std::uint32_t totalChars() const { return total_; }
    std::uint32_t revealedChars() const { return shown_; }
    bool revealed() const { return shown_ >= total_; }
    // The typed-out part of the runner's text().
    FTCORE_API std::string_view revealedText() const;
    // A choice whose prompt is fully out: show and accept its options.
    bool optionsVisible() const { return state_ == StoryRunner::State::Choice && revealed(); }
    // Seconds on the current beat.
    float beatTime() const { return beatTime_; }

    // ---- input ----
    // Confirm: finishes the text when it is still typing (None), else Advance on a line or a
    // direction, Resume or Handoff on a gameplay beat. None on a choice (pick an option) or a hold.
    FTCORE_API DialogueCommand press();
    // Option i (0-based): finishes the text when it is still typing (None); Choose once the options
    // show and i is selectable; None otherwise.
    FTCORE_API DialogueCommand choose(std::uint32_t i);
    // The move an automatic player makes now: nothing until the text is out, then Advance, the
    // runner's defaultOption() (else the first selectable option), Resume or Handoff, or SkipHold.
    FTCORE_API DialogueCommand autoplay() const;
    // Shows the whole text now.
    void revealAll() { reveal(total_); }

private:
    void reveal(std::uint32_t chars);
    DialogueCommand gameplayCommand() const;

    const StoryRunner* runner_ = nullptr;
    StoryRunner::State state_ = StoryRunner::State::Idle;
    std::uint32_t epoch_ = 0;
    std::string_view text_;
    std::uint32_t total_ = 0, shown_ = 0;
    std::size_t shownBytes_ = 0;
    float rate_ = 40.f;
    float progress_ = 0.f; // characters, fractional
    float beatTime_ = 0.f;
    bool handoff_ = false;
    bool fresh_ = true; // read the runner on the next sync() whatever its epoch
};

} // namespace ftcore
