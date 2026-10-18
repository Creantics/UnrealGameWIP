#include "ftcore/ComboGenerator.h"

#include "ftcore/Assert.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

namespace {

// SplitMix64: tiny, fast and identical everywhere, unlike std::uniform_*_distribution.
struct Rng {
    std::uint64_t state;
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float unit() { return static_cast<float>(next() >> 40) / static_cast<float>(1ull << 24); }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
};

float ramp(float rating, float from, float max) {
    return max <= from ? 0.0f : std::clamp((rating - from) / (max - from), 0.0f, 1.0f);
}

} // namespace

float comboRating(float moveDifficulty, const Stats& performer, const Stats& opponent, const DifficultyTuning& t) {
    const float r = moveDifficulty +
                    t.techniqueWeight * static_cast<float>(opponent.technique - performer.technique) +
                    t.speedWeight * static_cast<float>(opponent.speed - performer.speed);
    return std::clamp(r, 0.0f, t.maxRating);
}

ComboParams comboParams(int baseNotes, float rating, const DifficultyTuning& t) {
    const float k = std::clamp(rating / t.maxRating, 0.0f, 1.0f);
    ComboParams p;
    p.rating = rating;
    p.steps = std::clamp(baseNotes + static_cast<int>(std::lround(rating * t.extraNotesPerRating)), 1, t.maxNotes);
    p.interval = t.easyInterval + (t.hardInterval - t.easyInterval) * k;
    p.windowScale = t.easyWindowScale + (t.hardWindowScale - t.easyWindowScale) * k;
    p.lanes = 2 + static_cast<int>(std::lround(k * 2.0f));
    p.jumpChance = t.jumpChanceAtMax * ramp(rating, t.jumpsFromRating, t.maxRating);
    p.burstChance = t.burstChanceAtMax * ramp(rating, t.burstsFromRating, t.maxRating);
    return p;
}

double snapSubdivision(double intervalSeconds, double bpm) {
    const double spb = 60.0 / bpm;
    double best = kGridSubdivisions[0];
    double bestErr = std::abs(best * spb - intervalSeconds);
    for (double sub : kGridSubdivisions) {
        const double err = std::abs(sub * spb - intervalSeconds);
        if (err < bestErr - 1e-12) {
            best = sub;
            bestErr = err;
        }
    }
    return best;
}

double ceilToGrid(double beat, double step) { return std::ceil(beat / step - 1e-6) * step; }

ComboParams comboParams(int baseNotes, float rating, const DifficultyTuning& t, double bpm) {
    ComboParams p = comboParams(baseNotes, rating, t);
    if (bpm > 0.0) {
        p.bpm = bpm;
        p.subdivision = snapSubdivision(p.interval, bpm);
        p.minStepSeconds = t.minStepSeconds;
    }
    return p;
}

namespace {

// Shared by both modes. `place(stepIndex, burst)` advances the clock; the random draws are
// made in the same order whichever clock is used.
template <class Advance, class Emit>
void build(const ComboParams& p, std::uint64_t seed, Advance advance, Emit emit) {
    Rng rng{seed};

    // Lane subsets get harder to read as they grow: Left/Right first (one per foot), then Up, then Down.
    static constexpr Arrow kOrder[] = {Arrow::Left, Arrow::Right, Arrow::Up, Arrow::Down};
    const int lanes = std::clamp(p.lanes, 1, 4);

    int prev = -1;
    for (int step = 0; step < p.steps; ++step) {
        if (step > 0) advance(rng.unit() < p.burstChance);

        // Avoid repeating the previous arrow unless only one lane is in use.
        int a = rng.below(lanes);
        if (lanes > 1 && a == prev) a = (a + 1 + rng.below(lanes - 1)) % lanes;
        emit(kOrder[a]);
        prev = a;

        if (lanes > 1 && step > 0 && rng.unit() < p.jumpChance) {
            const int b = (a + 1 + rng.below(lanes - 1)) % lanes;
            emit(kOrder[b]);
        }
    }
}

// Upper bound of notes build() emits: one per step, plus one more for each jump step.
std::size_t maxNotes(const ComboParams& p) { return static_cast<std::size_t>(std::max(p.steps, 0)) * 2; }

} // namespace

void generateComboOnGridInto(std::vector<ComboNote>& notes, const ComboParams& p, double startBeat,
                             std::uint64_t seed) {
    FTCORE_ASSERT(p.bpm > 0.0);
    const double spb = 60.0 / p.bpm;
    const double sub = p.subdivision > 0.0 ? p.subdivision : snapSubdivision(p.interval, p.bpm);
    const double burst = sub * 0.5;
    const bool burstOk = p.minStepSeconds <= 0.0 || burst * spb >= p.minStepSeconds - 1e-9;

    notes.clear();
    notes.reserve(maxNotes(p)); // at most one allocation instead of ~5 doublings
    double beat = startBeat;
    build(
        p, seed, [&](bool isBurst) { beat += (isBurst && burstOk) ? burst : sub; },
        [&](Arrow a) { notes.push_back({a, p.beatOffset + beat * spb, beat}); });
}

void generateComboInto(std::vector<ComboNote>& notes, const ComboParams& p, double startTime, std::uint64_t seed) {
    if (p.bpm > 0.0) {
        const double spb = 60.0 / p.bpm;
        generateComboOnGridInto(notes, p, ceilToGrid((startTime - p.beatOffset) / spb, 1.0), seed);
        return;
    }
    notes.clear();
    notes.reserve(maxNotes(p));
    double t = startTime;
    build(
        p, seed, [&](bool isBurst) { t += isBurst ? p.interval * 0.5 : p.interval; },
        [&](Arrow a) { notes.push_back({a, t, kNoBeat}); });
}

std::vector<ComboNote> generateComboOnGrid(const ComboParams& p, double startBeat, std::uint64_t seed) {
    std::vector<ComboNote> notes;
    generateComboOnGridInto(notes, p, startBeat, seed);
    return notes;
}

std::vector<ComboNote> generateCombo(const ComboParams& p, double startTime, std::uint64_t seed) {
    std::vector<ComboNote> notes;
    generateComboInto(notes, p, startTime, seed);
    return notes;
}

JudgeWindows scaled(const JudgeWindows& w, double scale) {
    return {w.perfect * scale, w.great * scale, w.good * scale};
}

} // namespace ftcore
