#pragma once

#include "ftcore/Api.h"
#include "ftcore/Arrow.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ftcore {

enum class Grade : std::uint8_t { Pending, Perfect, Great, Good, Miss };

// Sentinel for ComboNote::beat on charts that were not generated on a beat grid.
inline constexpr double kNoBeat = -1.0;

struct ComboNote {
    Arrow arrow = Arrow::Left;
    double time = 0.0;      // seconds; this is what judgement uses
    double beat = kNoBeat;  // absolute song beat for grid charts (time = offset + beat * 60 / bpm), else kNoBeat
};

// Judgement half-widths in seconds around each note's time.
struct JudgeWindows {
    double perfect = 0.045;
    double great = 0.090;
    double good = 0.135;
};

// How much each grade counts towards accuracy (Miss is always 0).
struct GradeWeights {
    float perfect = 1.0f;
    float great = 0.8f;
    float good = 0.5f;

    float of(Grade g) const {
        switch (g) {
        case Grade::Perfect: return perfect;
        case Grade::Great: return great;
        case Grade::Good: return good;
        default: return 0.0f;
        }
    }
};

struct NoteResult {
    std::size_t index = 0;
    Grade grade = Grade::Pending;
    double error = 0.0;      // press time - note time (negative = early); 0 for expired notes
    bool wrongArrow = false; // judged Miss because a different arrow was pressed in its window
};

// A string of directional notes the player steps through, DDR-style. Used both to power
// attacks (accuracy scales damage dealt) and to defend (accuracy scales damage blocked).
//
// Rules:
//  - A press judges the earliest pending note in its window with the same arrow
//    (notes sharing a time form a "jump" and may be hit in any order).
//  - A press inside a pending note's window with no matching arrow is a wrong step:
//    the earliest such note is judged Miss.
//  - A press with no note in range is a stray: it starts a lockout during which input
//    is ignored, so mashing ahead of the notes is punished.
//  - Notes whose window passes unanswered are judged Miss.
// Presses must be fed in time order.
// How many combos were ever built or restarted (every constructor and every reset() counts one), for the
// test that an Arena or a Beat fight builds none (the combos belong to the Turns battle alone).
FTCORE_API std::uint64_t comboSequencesBuilt();

class ComboSequence {
public:
    FTCORE_API ComboSequence();
    FTCORE_API ComboSequence(std::vector<ComboNote> notes, JudgeWindows windows, double lockout, GradeWeights weights = {});

    // Returns every note resolved by this call: expirations first, then the press itself.
    FTCORE_API std::vector<NoteResult> press(Arrow arrow, double t);
    // Resolves expired notes as Miss and returns them in order.
    FTCORE_API std::vector<NoteResult> update(double now);
    // Allocation-free forms: the results are appended to `out` (a caller-owned, reused buffer).
    FTCORE_API void press(Arrow arrow, double t, std::vector<NoteResult>& out);
    FTCORE_API void update(double now, std::vector<NoteResult>& out);

    // Starts a fresh combo in place, reusing this object's storage. `notes` is swapped in (it
    // must be sorted by time) and receives the previous notes buffer back, so a caller that keeps
    // one scratch vector never allocates once both buffers have grown.
    FTCORE_API void reset(std::vector<ComboNote>& notes, JudgeWindows windows, double lockout, GradeWeights weights = {});
    // Pre-sizes the note/grade storage for combos of up to `notes` notes.
    FTCORE_API void reserve(std::size_t notes);

    bool complete() const { return next_ >= notes_.size(); }
    bool lockedOut(double t) const { return t < lockoutUntil_; }
    std::size_t size() const { return notes_.size(); }
    std::size_t nextIndex() const { return next_; } // earliest unjudged note
    const ComboNote& note(std::size_t i) const { return notes_[i]; }
    const std::vector<ComboNote>& notes() const { return notes_; }
    Grade grade(std::size_t i) const { return grades_[i]; }
    const JudgeWindows& windows() const { return windows_; }
    const GradeWeights& weights() const { return weights_; }

    std::size_t judged() const { return judged_; }
    FTCORE_API std::size_t count(Grade g) const;
    int strays() const { return strays_; }
    int combo() const { return combo_; }
    int maxCombo() const { return maxCombo_; }
    // Sum of grade weights of judged notes.
    float score() const { return score_; }
    // score() over the whole combo, 0..1. Unjudged notes count as 0 until they resolve.
    float accuracy() const { return notes_.empty() ? 0.0f : score_ / static_cast<float>(notes_.size()); }
    // No misses so far.
    bool fullCombo() const { return count(Grade::Miss) == 0; }

private:
    NoteResult judge(std::size_t i, Grade g, double error, bool wrong);
    Grade gradeFor(double error) const;
    void advance();

    std::vector<ComboNote> notes_;
    std::vector<Grade> grades_;
    JudgeWindows windows_;
    GradeWeights weights_;
    double lockout_ = 0.0;
    double lockoutUntil_ = -1e30;
    std::size_t next_ = 0;
    std::size_t judged_ = 0;
    float score_ = 0.0f;
    int combo_ = 0, maxCombo_ = 0, strays_ = 0;
};

} // namespace ftcore
