#pragma once

// The mini-game: the real screen behind MINI gameplay steps (GameplayScreen::Mini). One small,
// generic rhythm round: a short chart of arrow steps on a beat grid scrolls to a judgement line, and
// the player presses each arrow as it lands. The chart comes from generateComboInto (ComboGenerator.h)
// and is judged by ComboSequence (ComboSequence.h), the battle's own step rules; the beat grid is a
// Conductor's (Conductor.h) helpers on the screen's clock (no song is playing, so no cursor is fed).
//
// A round passes when its accuracy reaches the def's `pass`. There is no fail state that blocks the
// story: a round below it is retried (with the next seed) while tries remain, and after the last try
// the story continues anyway. The outcome is only shown, never branched on.
//
//   ftcore::MiniScreen screen;
//   screen.begin(hasStep, def, autoPlay);     // allocates the chart buffers once
//   every frame: screen.press(arrow) for each key press, then
//                screen.tick(deltaSeconds * ftcore::StubScreen::speedFactor(fast, worldDilation));
//   when screen.finished(): mark the carry done and open the return map
//
// Presses are stamped with the round's clock as it stood at the start of the frame (before tick).
// Input is ignored for kInputGrace seconds after each round starts (the press that opened the screen
// can't count) and always under autoplay, where the autopilot presses every note at its own time on a
// clock compressed so the round fits kAutoBudget game seconds, and the result shows for
// kAutoResultTime. Autoplay passes the first try, so an autoplay screen ends within about a second.
//
// Data: Data/Mini/mini.json, ids and numbers only, no story text. Like qte.json it is keyed by scene
// id + gameplay beat index + plan step index, with a `default` entry for every step not listed:
//   { "version": 1,
//     "default": { "bpm": 100, "steps": 12, "lanes": 3, "seed": 7, "pass": 0.6, "tries": 3 },
//     "minis": [ { "scene": "SC-X", "beat": 4, "step": 0, "bpm": 120, "steps": 16 } ] }
//   Keys (all optional in an entry; a missing key takes the built-in default, not the table's):
//   bpm (60..200), steps (4..kMaxMiniSteps), lanes (2..4), subdivision (1, 0.5; beats between
//   steps), seed (0..2^31), pass (0..1, accuracy), tries (1..9), lead (1..4 s before the first
//   note), window (0.5..2, a scale on the judgement windows). A duplicate (scene, beat, step) is an
//   error; unknown keys are warnings.
//
// Loading and begin() allocate; tick() and press() do not (the chart and the result buffers are
// reserved in begin(), and a retry regenerates into them).

#include "ftcore/Api.h"
#include "ftcore/ComboSequence.h"
#include "ftcore/Conductor.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

inline constexpr int kMaxMiniSteps = 32;

struct MiniDef {
    double bpm = 100.0;
    int steps = 12;
    int lanes = 3;
    double subdivision = 1.0; // beats between steps
    std::uint32_t seed = 7;
    double pass = 0.6;        // accuracy (0..1) that passes a round
    int tries = 3;
    double lead = 2.0;        // seconds from the start of a round to its first note (at least)
    double window = 1.0;      // scale on the judgement windows
};

// The built-in round used when no data file is there.
FTCORE_API MiniDef defaultMiniDef();

// How a round ended. Pending while it plays.
enum class MiniOutcome : std::uint8_t { Pending, Cleared, Retry, Continue };
// "", "CLEARED", "AGAIN", "ONWARD". Literals.
FTCORE_API const char* miniOutcomeName(MiniOutcome o);

enum class MiniPhase : std::uint8_t {
    NoStep,  // no story step: the screen does nothing
    Playing,
    Result,  // the round is over: its outcome shows for a moment (then a retry or the end)
    Done     // finished: continue to the return map
};

class MiniScreen {
public:
    static constexpr double kInputGrace = 0.25;
    static constexpr double kResultTime = 1.5;
    static constexpr double kAutoBudget = 0.6;
    static constexpr double kAutoResultTime = 0.25;
    static constexpr double kStrayLockout = 0.15;

    // hasStep false: a screen opened without a valid story carry (NoStep, inert).
    FTCORE_API void begin(bool hasStep, const MiniDef& def, bool autoPlay);
    FTCORE_API void tick(double gameSeconds);
    // A key press (ignored outside Playing, under autoplay and during the grace).
    FTCORE_API void press(Arrow input);

    MiniPhase phase() const { return phase_; }
    bool playing() const { return phase_ == MiniPhase::Playing; }
    bool showingResult() const { return phase_ == MiniPhase::Result; }
    bool finished() const { return phase_ == MiniPhase::Done; }
    bool autoPlay() const { return autoPlay_; }
    const MiniDef& def() const { return def_; }

    // The current round's chart and judgement.
    const ComboSequence& sequence() const { return seq_; }
    // The round's clock (seconds; faster than game time when autoplay compresses it).
    double time() const { return time_; }
    // The beat at the round's clock (for a pulse on the beat).
    double beat() const { return grid_.beatAt(time_); }
    double secondsPerBeat() const { return grid_.secondsPerBeat(); }
    // When the round's last note's window closes.
    FTCORE_API double roundLength() const;
    // 0-based try; tries used so far (counting the current one).
    int attempt() const { return attempt_; }
    int triesUsed() const { return attempt_ + 1; }
    // The current round's outcome (Pending while playing).
    MiniOutcome outcome() const { return outcome_; }
    // The best accuracy over the tries so far, and the last round's.
    float bestAccuracy() const { return best_; }
    float lastAccuracy() const { return last_; }
    bool cleared() const { return cleared_; }
    // Seconds since the result appeared (0 before).
    double resultAge() const { return resultAge_; }
    // The most recent judgement (Pending before the first) and the round time it came at.
    Grade lastGrade() const { return lastGrade_; }
    double lastGradeTime() const { return lastGradeTime_; }

private:
    void startRound();
    void collect(double t);
    void endRound();
    void autopilot(double upTo);

    MiniDef def_{};
    ComboSequence seq_;
    Conductor grid_;
    std::vector<ComboNote> chart_;   // scratch: generated here, then swapped into seq_
    std::vector<NoteResult> results_; // scratch for press/update
    MiniPhase phase_ = MiniPhase::NoStep;
    MiniOutcome outcome_ = MiniOutcome::Pending;
    bool autoPlay_ = false;
    bool cleared_ = false;
    int attempt_ = 0;
    std::size_t autoNext_ = 0;
    double time_ = 0.0;
    double roundAge_ = 0.0;   // game seconds in the current round
    double resultAge_ = 0.0;
    double scale_ = 1.0;
    float best_ = 0.0f, last_ = 0.0f;
    Grade lastGrade_ = Grade::Pending;
    double lastGradeTime_ = 0.0;
};

// ---- data ----

struct MiniEntry {
    std::string scene;
    std::uint32_t beat = 0, step = 0;
    MiniDef def;
};

struct MiniTable {
    MiniDef fallback; // the "default" entry
    std::vector<MiniEntry> entries;

    // The entry for (scene, beat, step), or nullptr. No allocation.
    FTCORE_API const MiniEntry* findEntry(std::string_view scene, std::uint32_t beat, std::uint32_t step) const;
    // The round for (scene, beat, step): the entry's, else the default. No allocation.
    FTCORE_API const MiniDef& find(std::string_view scene, std::uint32_t beat, std::uint32_t step) const;
};

// On failure `out` is empty and *error lists each problem with its path, one per line.
FTCORE_API bool importMiniTable(std::string_view json, MiniTable& out, std::string* error = nullptr,
                                std::vector<std::string>* warnings = nullptr);

} // namespace ftcore
