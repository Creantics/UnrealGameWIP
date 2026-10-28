#pragma once

// The screen around a beat fight (CombatKind::Beat; docs/BEAT_SCREEN.md), as QteScreen is for the quick-time
// event: the state machine the game mode ticks. It owns the BeatFight, runs it on the host's clock, plays the
// autopilot under autoplay and walks the screen through its phases:
//
//   NoStep       opened without a fight to play (no valid story carry): inert.
//   Playing      the fight runs. Input counts once kInputGrace has passed; autoplay ignores it.
//   Result       Victory or Defeat shows for kResultTime (kAutoResultTime under autoplay).
//   RetryPrompt  after a defeat: choose(Retry) restarts the same fight (the same seed), choose(Quit) ends the
//                step lost. Autoplay never stops here: it retries by itself, up to kAutoRetries times.
//   Done         finished: the host marks the carry done (won() says how it went) and goes on.
//
//   ftcore::BeatScreen screen;
//   screen.begin(hasStep, setup, seed, autoPlay, assist, clockNow);
//   every frame: screen.press(arrow, eventTime) / screen.release(arrow, eventTime) for the keys, then
//                screen.tick(clockNow);                       // draw from screen.fight() and screen.now()
//   at the RetryPrompt: screen.choose(ftcore::BeatChoice::Retry) or (ftcore::BeatChoice::Quit)
//   when screen.finished(): mark the carry done, won when screen.won(), and go on
//
// The clock. Every time here is in seconds on the fight's clock (BeatFight.h): the music's song time when
// music plays, else a pause-aware wall clock. tick() takes the clock as it reads now, press() and release()
// the time of the event (the host's songTimeForEvent). Time never runs back: an earlier time is read as the
// latest one seen. begin() takes the clock at the start; the fight's first bar line is a bar after it.
//
// Input. Presses and releases are ignored outside Playing, under autoplay, and for kInputGrace after the
// fight starts or restarts (the press that opened the screen, or chose Retry). Under the assist's
// auto-complete the fight plays itself and ignores them too (BeatFight does that).
//
// Autoplay plays the fight with BeatFight's autopilot: each frame the screen asks autopilot(now) until it has
// nothing to do, applying every press and release at its own time, then updates the fight. A long frame is
// played in steps of at most kAutoStep, so the autopilot sees every bar before it falls due. A defeat
// restarts the fight on its own after the result, kAutoRetries times at most; then the screen ends lost and
// gaveUp() is true (the host logs a counted warning). With a step, autoplay is not the assist's
// auto-complete: that one only applies to a screen a player is watching.
//
// Counting. stats() are the fight's and keep counting through retries (restarts included); playSeconds() is
// the clock the fight spent playing, over every try. The screen never counts the story step: the host marks
// the carry and the story's dispatcher counts each step once.
//
// Fixed capacity: nothing allocates after begin().

#include "ftcore/Api.h"
#include "ftcore/BeatFight.h"

#include <cstdint>

namespace ftcore {

enum class BeatScreenPhase : std::uint8_t {
    NoStep,      // no fight to play: the screen does nothing
    Playing,
    Result,      // the fight is over: its outcome shows for kResultTime
    RetryPrompt, // after a defeat: Retry or Quit
    Done         // finished: continue to the return map
};

// How the screen ended (None until it is Done).
enum class BeatOutcome : std::uint8_t { None, Won, Lost };

// What the player picks at the RetryPrompt.
enum class BeatChoice : std::uint8_t { Retry, Quit };

class BeatScreen {
public:
    // Input is ignored this long after the fight starts or restarts (clock seconds).
    static constexpr double kInputGrace = 0.25;
    // Victory or Defeat shows this long before the screen moves on, and this long under autoplay.
    static constexpr double kResultTime = 2.0;
    static constexpr double kAutoResultTime = 0.5;
    // Retry and Quit ignore the first part of the prompt, so the press that ended the fight can't pick one.
    static constexpr double kPromptGrace = 0.5;
    // Autoplay retries a lost fight this many times, then gives up.
    static constexpr int kAutoRetries = 3;
    // The autopilot plays a long frame in steps of at most this many clock seconds; a frame longer than
    // kAutoMaxSteps of them has the rest of it played in one jump.
    static constexpr double kAutoStep = 0.1;
    static constexpr int kAutoMaxSteps = 4096;

    // hasStep false: nothing to play (NoStep, inert). The fight starts with its clock at `now`. Under
    // autoplay (only with a step) the assist's auto-complete is moot; its hold toggle still applies.
    FTCORE_API void begin(bool hasStep, const BeatSetup& setup, std::uint32_t seed, bool autoPlay,
                          const QteAssist& assist, double now);
    // Moves the clock to `now`: bars are generated, prompts run out, the autopilot plays (autoplay), the
    // result times out and a lost autoplay fight restarts.
    FTCORE_API void tick(double now);
    // Key events at the time t of the event (ignored outside Playing, under autoplay and in the grace).
    FTCORE_API void press(Arrow input, double t);
    FTCORE_API void release(Arrow input, double t);
    // The player's pick at the RetryPrompt. False (and nothing happens) outside it, under autoplay and in
    // the first kPromptGrace of it.
    FTCORE_API bool choose(BeatChoice choice);

    BeatScreenPhase phase() const { return phase_; }
    bool playing() const { return phase_ == BeatScreenPhase::Playing; }
    bool showingResult() const { return phase_ == BeatScreenPhase::Result; }
    bool askingRetry() const { return phase_ == BeatScreenPhase::RetryPrompt; }
    bool finished() const { return phase_ == BeatScreenPhase::Done; }
    bool autoPlay() const { return autoPlay_; }
    // The assist's auto-complete is playing the fight (never under autoplay).
    bool autoComplete() const { return autoComplete_; }

    // How it ended: Won once a victory is Done, Lost once a Quit or an autoplay that gave up is.
    BeatOutcome outcome() const { return outcome_; }
    bool won() const { return outcome_ == BeatOutcome::Won; }
    // Autoplay lost the fight and every retry: the host logs a counted warning.
    bool gaveUp() const { return gaveUp_; }
    // Restarts autoplay has made.
    int autoRetries() const { return autoRetries_; }

    // The fight, for drawing (promptCount(), prompt(i), approach(), hp()...), and the clock it is drawn at.
    const BeatFight& fight() const { return fight_; }
    double now() const { return now_; }
    // The counts the log line needs; they keep counting through retries.
    const BeatStats& stats() const { return fight_.stats(); }
    // Clock seconds spent playing, over every try (not the result or the prompt).
    double playSeconds() const { return playSeconds_; }
    // Clock seconds since the fight ended (0 while it plays): the result's age, which goes on through the
    // prompt.
    FTCORE_API double resultAge() const;
    // Beats until the first bar starts while the fight is in its lead-in (the HUD counts them down), else 0.
    FTCORE_API double leadInBeats() const;

    // The events since the last call (the screen's popups); the list starts over after it. Events past the
    // capacity of one list are lost: take them every frame.
    BeatEvents takeFresh() {
        const BeatEvents r = fresh_;
        fresh_.count = 0;
        return r;
    }

private:
    void run(double now);
    void pilot(double horizon);
    void settle(double before);
    void afterResult();
    void restartFight();
    void collect(const BeatEvents& e);

    BeatFight fight_;
    BeatEvents fresh_;
    BeatScreenPhase phase_ = BeatScreenPhase::NoStep;
    BeatOutcome outcome_ = BeatOutcome::None;
    bool autoPlay_ = false;
    bool autoComplete_ = false;
    bool gaveUp_ = false;
    int autoRetries_ = 0;
    double now_ = 0.0;
    double playSeconds_ = 0.0;
    double graceUntil_ = 0.0;
    double endedAt_ = 0.0;  // when the fight ended
    double promptAt_ = 0.0; // when the RetryPrompt appeared
};

} // namespace ftcore
