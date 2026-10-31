#pragma once

// The quick-time event: a short timed input sequence outside combat (the QTE and `co-op QTE`
// gameplay steps; GameplayScreen::Qte).
//
// A QteDef is a list of prompts, each with an input, an offset from the start of the sequence and a
// window kind. Prompts are judged with the defence maths of DefenceTimeline.h (this file adds no
// window arithmetic of its own): a Wide prompt is answered like a dodge, a Narrow one like a parry
// (a press inside the wide window but outside the narrow one still counts, as Good).
//
//   Tap   one of the four directions, pressed before `at` (the impact; the press has to come before
//         it). Wide: the last 0.06 s before `at` is Perfect, the rest of the 0.30 s window Good.
//         Narrow: the last 0.12 s is Perfect, the rest of the wide window Good.
//   Hold  a direction pressed like a Tap, then kept down until at + hold. Let go more than
//         kQteHoldSlack early and the prompt is a Miss; otherwise it keeps its tap grade.
//   Mash  a direction pressed `mashCount` times inside [at, at + limit). Done within
//         kQtePerfectFraction of the limit it is Perfect, later Good; short of the count it is a Miss.
//
// A press of another direction than the prompt's is ignored. There is no fail state: a Miss plays
// on to the next prompt, and the overall QteOutcome is only a grade (the story never branches on
// it). A `co-op QTE` alternates its prompts between the two leads (actor slot 0, 1, 0, ...); the
// slot only tells the screen whose prompt it is.
//
//   ftcore::QteSequence q;
//   q.begin(def, coop);
//   for (const ftcore::QteResult& r : q.press(ftcore::Arrow::Left, t)) ...;   // times in order
//   q.release(ftcore::Arrow::Left, t);
//   q.update(now);                      // prompts that ran out unanswered are Misses
//   q.complete(); q.outcome();
//
// Prompts are answered strictly in order (the earliest unresolved one), and the loader keeps them
// apart: each starts at least kQteMinGap after the previous one ends. The autopilot (QteSequence::
// autopilot) presses at the ideal time and never listens to anything else.
//
// QteScreen wraps a sequence with the screen's clock, input grace, result pause and autoplay.
//
// Two player options (ftcore::Settings, passed in as a QteAssist) change the screen: auto-complete
// lets the autopilot play the sequence at its own pace while the player watches (input is ignored),
// and hold toggle makes a Hold prompt latch on its press: letting go does nothing and the prompt
// keeps its tap grade when the hold runs out.
//
// Data: Data/Qte/qte.json, ids and numbers only, no story text. Like encounters.json it is keyed by
// scene id + gameplay beat index + plan step index with a `default` entry for every step not listed:
//   { "version": 1,
//     "default": { "prompts": [ { "input": "left", "at": 1.0 }, { "input": "up", "at": 2.0, "window": "narrow" },
//                               { "input": "down", "at": 3.0, "hold": 0.8 },
//                               { "input": "right", "at": 5.0, "mash": 8, "limit": 1.5 } ] },
//     "qtes": [ { "scene": "SC-X", "beat": 4, "step": 0, "prompts": [ ... ] } ] }
//   Prompt keys: input ("left", "down", "up", "right"), at (seconds from the start, at least
//   kQteMinFirst for the first), window ("wide" default, or "narrow"), hold (seconds), mash (count,
//   needs limit) and limit (seconds; mash only). hold and mash are exclusive. 1..kMaxQtePrompts prompts,
//   `at` strictly after the previous prompt's end plus kQteMinGap. `step` defaults to 0. A duplicate
//   (scene, beat, step) is an error; unknown keys are warnings.
//
// Loading allocates; everything else does not (fixed capacity, results by value).

#include "ftcore/Api.h"
#include "ftcore/Arrow.h"
#include "ftcore/DefenceTimeline.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

enum class QteKind : std::uint8_t { Tap, Hold, Mash };
enum class QteWindow : std::uint8_t { Wide, Narrow };
enum class QteGrade : std::uint8_t { Pending, Perfect, Good, Miss };
// The whole sequence, graded: Perfect when every prompt was Perfect (or there were none), Good when
// none was missed, Fair when at most half were, Poor otherwise.
enum class QteOutcome : std::uint8_t { Perfect, Good, Fair, Poor };

// "PERFECT", "GOOD", "MISS" (or "" for Pending) / "Perfect", "Good", "Fair", "Poor". Literals.
FTCORE_API const char* qteGradeName(QteGrade g);
FTCORE_API const char* qteOutcomeName(QteOutcome o);

inline constexpr std::size_t kMaxQtePrompts = 24;
inline constexpr double kQteTell = 0.6;         // a prompt's ring closes in over this long before `at`
inline constexpr double kQteMinFirst = 0.5;     // the first prompt is no earlier than this
inline constexpr double kQteMinGap = 0.4;       // from the end of one prompt to the start of the next
inline constexpr double kQteMaxAt = 120.0;
inline constexpr double kQteMinHold = 0.2;
inline constexpr double kQteMaxHold = 5.0;
inline constexpr double kQteHoldSlack = 0.15;   // letting go this much early still counts
inline constexpr int kQteMinMash = 2;
inline constexpr int kQteMaxMash = 30;
inline constexpr double kQteMinLimit = 0.5;
inline constexpr double kQteMaxLimit = 8.0;
inline constexpr double kQtePerfectFraction = 0.6; // a mash done by this share of its limit is Perfect

struct QtePrompt {
    QteKind kind = QteKind::Tap;
    Arrow input = Arrow::Left;
    QteWindow window = QteWindow::Wide; // Tap and Hold; a Mash ignores it
    std::uint8_t actor = 0;             // 0 or 1: which lead's prompt (co-op)
    std::uint8_t mashCount = 0;         // Mash: presses needed
    double at = 1.0;                    // seconds after the start of the sequence
    double hold = 0.0;                  // Hold: seconds to keep it down after `at`
    double limit = 0.0;                 // Mash: seconds from `at` to get it done

    // How long the prompt runs past `at`.
    double span() const { return kind == QteKind::Hold ? hold : (kind == QteKind::Mash ? limit : 0.0); }
    double end() const { return at + span(); }
};

// The ring's lead: kQteTell, but never more than the time from the start to `at`.
inline double qteTell(const QtePrompt& p) { return p.at < kQteTell ? p.at : kQteTell; }

struct QteDef {
    std::array<QtePrompt, kMaxQtePrompts> prompts{};
    std::uint8_t count = 0;

    std::size_t size() const { return count; }
    const QtePrompt& operator[](std::size_t i) const { return prompts[i]; }
    // Appends a prompt; false (and nothing added) when full.
    bool add(const QtePrompt& p) {
        if (count >= kMaxQtePrompts) return false;
        prompts[count++] = p;
        return true;
    }
    // When the last prompt ends (0 when empty).
    double length() const { return count == 0 ? 0.0 : prompts[count - 1u].end(); }
};

// The built-in sequence used when no data file is there (a tap, a narrow tap, a hold, a mash).
FTCORE_API QteDef defaultQteDef();

struct QteResult {
    std::uint8_t prompt = 0; // index into the sequence
    QteGrade grade = QteGrade::Pending;
    double time = 0.0;       // press time, or when the prompt ended
};

// Prompts resolved by one call, in order. Fixed capacity: no allocation.
struct QteResults {
    std::array<QteResult, kMaxQtePrompts> items{};
    std::size_t count = 0;

    const QteResult* begin() const { return items.data(); }
    const QteResult* end() const { return items.data() + count; }
    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
    const QteResult& operator[](std::size_t i) const { return items[i]; }
};

// The window widths a QTE is judged with: DefenceWindows' dodge and parry widths, no lockouts (a
// press that misses must not lock out the next prompt).
inline DefenceWindows qteWindows() {
    DefenceWindows w;
    w.dodgeLockout = 0.0;
    w.parryLockout = 0.0;
    w.jumpLockout = 0.0;
    return w;
}

// The player's assist options for a quick-time event (from ftcore::Settings).
struct QteAssist {
    bool autoComplete = false; // the autopilot plays it, at normal speed
    bool holdToggle = false;   // a Hold prompt latches on its press
};

// What the autopilot does next.
struct QteAction {
    enum class Kind : std::uint8_t { None, Press, Release };
    Kind kind = Kind::None;
    Arrow input = Arrow::Left;
    double time = 0.0;
};

class QteSequence {
public:
    // Starts `def` with the clock at 0. coop: prompts alternate between actor slots 0 and 1; else
    // every prompt is slot 0. An empty sequence is complete at once. Hold toggle starts off.
    FTCORE_API void begin(const QteDef& def, bool coop = false, const DefenceWindows& windows = qteWindows());
    // With hold toggle on, release() never ends a hold early: a Hold prompt resolves at the end of its
    // hold with the grade of its press.
    void setHoldToggle(bool on) { holdToggle_ = on; }
    bool holdToggle() const { return holdToggle_; }

    // Moves the clock to `now`: prompts that ran out unanswered are Misses, a held prompt whose time
    // is up resolves. Times (here and in press/release) go in order; an earlier time is read as `now()`.
    FTCORE_API QteResults update(double now);
    // A press of `input` at time t (moves the clock there first). Returns what it resolved: the
    // prompts that ran out before t, then the one it answered, if any.
    FTCORE_API QteResults press(Arrow input, double t);
    // `input` let go at time t. Ends a hold that is let go too early.
    FTCORE_API QteResults release(Arrow input, double t);
    // Ends the sequence at `now`: every prompt still open is a Miss (the autoplay cap).
    FTCORE_API QteResults finish(double now);

    // The next press or release the autopilot makes at or before `now` (None when it is waiting).
    // Apply it with press()/release() at action.time, then ask again until None. The autopilot
    // answers a Narrow prompt in the middle of the parry window, a Wide one in the middle of the
    // perfect-dodge part, lets go of a hold at its end (the Release moves the clock there, which
    // resolves it) and spreads a mash over half its limit.
    FTCORE_API QteAction autopilot(double now) const;

    double now() const { return now_; }
    std::size_t size() const { return def_.size(); }
    const QtePrompt& prompt(std::size_t i) const { return def_[i]; }
    bool coop() const { return coop_; }
    QteGrade grade(std::size_t i) const { return grades_[i]; }
    // The earliest open prompt (size() when none).
    std::size_t current() const { return next_; }
    bool complete() const { return next_ >= def_.size(); }
    // The current hold is being held / its progress 0..1; the current mash's presses so far.
    bool holding() const { return phase_ == Phase::Holding; }
    FTCORE_API double holdProgress() const;
    int mashPresses() const { return mashDone_; }

    // The ring of prompt i at time t: 1 when it appears (kQteTell before `at`) down to 0 at `at`;
    // -1 when it isn't showing (not yet, or the prompt is over).
    FTCORE_API double tellProgress(std::size_t i, double t) const;

    FTCORE_API std::size_t count(QteGrade g) const;
    std::size_t perfects() const { return count(QteGrade::Perfect); }
    std::size_t goods() const { return count(QteGrade::Good); }
    std::size_t misses() const { return count(QteGrade::Miss); }
    int strays() const { return strays_; } // presses of the wrong direction, or that answered nothing
    FTCORE_API QteOutcome outcome() const;

private:
    enum class Phase : std::uint8_t { Awaiting, Holding, Mashing, Done };

    void advance(double t, QteResults& out);
    void resolve(QteGrade g, double time, QteResults& out);
    void arm();
    double idealTime(const QtePrompt& p) const;

    QteDef def_{};
    DefenceWindows windows_{};
    std::array<QteGrade, kMaxQtePrompts> grades_{};
    DefenceTimeline judge_; // the current tap or hold, as a single hit at `at`
    Phase phase_ = Phase::Done;
    QteGrade holdGrade_ = QteGrade::Pending;
    double now_ = 0.0;
    std::size_t next_ = 0;
    int mashDone_ = 0;
    int strays_ = 0;
    bool coop_ = false;
    bool holdToggle_ = false;
};

// The screen around a sequence: a clock, the input grace, the result pause and autoplay.
//
//   ftcore::QteScreen screen;
//   screen.begin(hasStep, def, coop, autoPlay);
//   every frame: screen.press(...) / screen.release(...) for the keys, then
//                screen.tick(deltaSeconds * ftcore::StubScreen::speedFactor(fast, worldDilation));
//   when screen.finished(): mark the carry done and open the return map
//
// Presses are stamped with the clock as it stood at the start of the frame (before tick), so a frame
// that expires a prompt can't swallow a press made in it. Input is ignored for kInputGrace seconds
// (the press that opened the screen) and always under autoplay, where the autopilot plays on a clock
// compressed to fit kAutoBudget seconds if the sequence is longer, and the whole screen ends by
// kAutoCap game seconds at the latest (what is still open is graded a Miss). Auto-complete (an assist,
// not autoplay) also ignores input and lets the autopilot play, but on the normal clock and without
// the cap: the player sees the whole sequence.
enum class QtePhase : std::uint8_t {
    NoStep,  // no story step: the screen does nothing
    Playing,
    Result,  // all prompts resolved: the outcome shows for kResultTime
    Done     // finished: continue to the return map
};

class QteScreen {
public:
    static constexpr double kInputGrace = 0.25;
    static constexpr double kResultTime = 1.0;
    static constexpr double kAutoBudget = 8.0;
    static constexpr double kAutoCap = 10.0;

    // hasStep false: a screen opened without a valid story carry (NoStep, inert). Under autoplay the
    // assist's auto-complete is moot; its hold toggle still applies.
    FTCORE_API void begin(bool hasStep, const QteDef& def, bool coop, bool autoPlay, const QteAssist& assist = {});
    FTCORE_API void tick(double gameSeconds);
    // Key events (ignored outside Playing, under autoplay and during the grace).
    FTCORE_API void press(Arrow input);
    FTCORE_API void release(Arrow input);

    QtePhase phase() const { return phase_; }
    bool playing() const { return phase_ == QtePhase::Playing; }
    bool showingResult() const { return phase_ == QtePhase::Result; }
    bool finished() const { return phase_ == QtePhase::Done; }
    bool autoPlay() const { return autoPlay_; }
    // The assist's auto-complete is playing the sequence (never under autoplay).
    bool autoComplete() const { return autoComplete_; }
    // The sequence's clock (it runs faster than game time when autoplay compresses a long sequence).
    double time() const { return seq_.now(); }
    // Game seconds on the screen.
    double elapsed() const { return elapsed_; }
    // Game seconds since the result appeared (0 before).
    double resultAge() const { return resultAge_; }
    // How much faster the sequence's clock runs than game time (1 unless autoplay compresses it).
    double clockScale() const { return scale_; }
    const QteSequence& sequence() const { return seq_; }
    // Grades resolved since the last call (the screen's popups); the list starts over after it.
    QteResults takeFresh() {
        const QteResults r = fresh_;
        fresh_.count = 0;
        return r;
    }

private:
    void collect(const QteResults& r);

    QteSequence seq_;
    QteResults fresh_;
    QtePhase phase_ = QtePhase::NoStep;
    bool autoPlay_ = false;
    bool autoComplete_ = false;
    double elapsed_ = 0.0;
    double resultAge_ = 0.0;
    double scale_ = 1.0;
};

// ---- data ----

struct QteEntry {
    std::string scene;
    std::uint32_t beat = 0, step = 0;
    QteDef def;
};

struct QteTable {
    QteDef fallback; // the "default" sequence
    std::vector<QteEntry> entries;

    // The entry for (scene, beat, step), or nullptr. No allocation.
    FTCORE_API const QteEntry* findEntry(std::string_view scene, std::uint32_t beat, std::uint32_t step) const;
    // The sequence for (scene, beat, step): the entry's, else the default. No allocation.
    FTCORE_API const QteDef& find(std::string_view scene, std::uint32_t beat, std::uint32_t step) const;
};

// On failure `out` is empty and *error lists each problem with its path, one per line.
FTCORE_API bool importQteTable(std::string_view json, QteTable& out, std::string* error = nullptr,
                               std::vector<std::string>* warnings = nullptr);

} // namespace ftcore
