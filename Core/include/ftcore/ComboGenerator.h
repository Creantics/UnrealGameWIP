#pragma once

#include "ftcore/Api.h"
#include "ftcore/CombatTypes.h"
#include "ftcore/ComboSequence.h"

#include <cstdint>
#include <vector>

namespace ftcore {

// How stats and moves map to combo difficulty. A combo's rating (0-10) starts at the move's
// difficulty and shifts by the stat matchup between the player-side performer and the enemy:
//
//   rating = skill.difficulty
//          + techniqueWeight * (opponent.technique - performer.technique)
//          + speedWeight     * (opponent.speed     - performer.speed)
//
// The rating then sets every knob of the chart: length, tempo, judgement window size,
// how many different arrows appear, and how often jumps (two arrows at once) and bursts
// (double-time steps) show up.
struct DifficultyTuning {
    float techniqueWeight = 0.3f;
    float speedWeight = 0.1f;
    float maxRating = 10.0f;

    float extraNotesPerRating = 0.35f; // notes added on top of the move's base length
    int maxNotes = 16;
    double easyInterval = 0.50;        // seconds between steps at rating 0
    double hardInterval = 0.22;        // ... at max rating
    double easyWindowScale = 1.25;     // judgement windows widen when easy
    double hardWindowScale = 0.70;     // ... and tighten when hard
    float jumpChanceAtMax = 0.30f;
    float burstChanceAtMax = 0.35f;
    float jumpsFromRating = 4.0f;      // no jumps below this rating
    float burstsFromRating = 3.0f;     // no bursts below this rating
    double minStepSeconds = 0.12;      // grid charts: bursts never make steps closer than this
};

struct ComboParams {
    float rating = 0.0f;
    int steps = 4;               // steps; a jump step adds two notes
    double interval = 0.5;
    double windowScale = 1.0;
    int lanes = 2;               // distinct arrows in use (2-4)
    float jumpChance = 0.0f;
    float burstChance = 0.0f;

    // Beat grid. bpm <= 0 keeps the seconds-only chart (steps `interval` seconds apart,
    // bursts at half of it). With bpm > 0 notes sit on beats of the song: step spacing is
    // `subdivision` beats (0 = the allowed subdivision nearest `interval` at this bpm), a
    // burst halves it, and time = beatOffset + beat * 60 / bpm.
    double bpm = 0.0;
    double beatOffset = 0.0;       // seconds of the song's beat 0
    double subdivision = 0.0;      // beats between steps; filled in by the bpm overload of comboParams
    double minStepSeconds = 0.0;   // bursts that would be closer than this are not applied (0 = no limit)
};

// Allowed step spacings on the grid, in beats, from slow to fast.
inline constexpr double kGridSubdivisions[] = {1.0, 0.5, 0.25};
// The allowed subdivision whose duration at `bpm` is closest to `intervalSeconds` (ties: slower).
FTCORE_API double snapSubdivision(double intervalSeconds, double bpm);
// Smallest multiple of `step` that is >= beat (tolerating tiny float error).
FTCORE_API double ceilToGrid(double beat, double step);

FTCORE_API float comboRating(float moveDifficulty, const Stats& performer, const Stats& opponent, const DifficultyTuning& tuning);
FTCORE_API ComboParams comboParams(int baseNotes, float rating, const DifficultyTuning& tuning);
// Same, for a song at `bpm` (> 0): the rating still decides density (interval), which is
// then snapped to the nearest allowed subdivision. beatOffset is left for the caller.
FTCORE_API ComboParams comboParams(int baseNotes, float rating, const DifficultyTuning& tuning, double bpm);

// Deterministic for a given seed on every platform (no std:: distributions).
// With params.bpm > 0 the first note lands on the first beat at or after startTime.
// Arrow/jump/burst random draws are identical in both modes, so a seed gives the same
// arrow sequence on the grid as off it.
FTCORE_API std::vector<ComboNote> generateCombo(const ComboParams& params, double startTime, std::uint64_t seed);
// Grid chart starting exactly at `startBeat` (requires params.bpm > 0).
FTCORE_API std::vector<ComboNote> generateComboOnGrid(const ComboParams& params, double startBeat, std::uint64_t seed);

// Same charts, written into a caller-owned buffer (cleared first; no allocation once it has the
// capacity, which is reserved up front otherwise).
FTCORE_API void generateComboInto(std::vector<ComboNote>& out, const ComboParams& params, double startTime, std::uint64_t seed);
FTCORE_API void generateComboOnGridInto(std::vector<ComboNote>& out, const ComboParams& params, double startBeat,
                             std::uint64_t seed);

FTCORE_API JudgeWindows scaled(const JudgeWindows& w, double scale);

} // namespace ftcore
