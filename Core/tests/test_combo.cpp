#include "ftcore/ComboGenerator.h"
#include "ftcore/ComboSequence.h"

#include <doctest/doctest.h>

#include <cmath>
#include <set>

using namespace ftcore;

namespace {
// Windows: perfect 0.04, great 0.08, good 0.12.
ComboSequence three() {
    return ComboSequence({{Arrow::Left, 1.0}, {Arrow::Up, 1.5}, {Arrow::Right, 2.0}}, {0.04, 0.08, 0.12}, 0.3);
}
} // namespace

TEST_CASE("notes are graded by timing error") {
    auto s = three();
    auto r = s.press(Arrow::Left, 1.02);
    REQUIRE(r.size() == 1);
    CHECK(r[0].grade == Grade::Perfect);
    r = s.press(Arrow::Up, 1.44);
    REQUIRE(r.size() == 1);
    CHECK(r[0].grade == Grade::Great);
    CHECK(r[0].error == doctest::Approx(-0.06));
    r = s.press(Arrow::Right, 2.11);
    REQUIRE(r.size() == 1);
    CHECK(r[0].grade == Grade::Good);
    CHECK(s.complete());
    CHECK(s.fullCombo());
    CHECK(s.maxCombo() == 3);
    CHECK(s.accuracy() == doctest::Approx((1.0f + 0.8f + 0.5f) / 3.0f));
}

TEST_CASE("pressing the wrong arrow in a window misses that note") {
    auto s = three();
    auto r = s.press(Arrow::Down, 1.0);
    REQUIRE(r.size() == 1);
    CHECK(r[0].index == 0);
    CHECK(r[0].grade == Grade::Miss);
    CHECK(r[0].wrongArrow);
    CHECK(s.combo() == 0);
    CHECK_FALSE(s.fullCombo());
}

TEST_CASE("stray presses lock input out, punishing mashing") {
    auto s = three();
    CHECK(s.press(Arrow::Left, 0.5).empty());
    CHECK(s.strays() == 1);
    CHECK(s.lockedOut(0.7));
    CHECK(s.press(Arrow::Left, 0.75).empty()); // ignored, and doesn't extend the lockout
    CHECK_FALSE(s.lockedOut(0.81));
    auto r = s.press(Arrow::Left, 1.0);
    REQUIRE(r.size() == 1);
    CHECK(r[0].grade == Grade::Perfect);
}

TEST_CASE("unanswered notes expire as misses") {
    auto s = three();
    CHECK(s.update(1.1).empty());
    auto r = s.update(1.13);
    REQUIRE(r.size() == 1);
    CHECK(r[0].grade == Grade::Miss);
    // A late press expires the overdue note before judging the next one.
    r = s.press(Arrow::Up, 1.5);
    REQUIRE(r.size() == 1);
    r = s.press(Arrow::Right, 2.5);
    REQUIRE(r.size() == 1);
    CHECK(r[0].index == 2);
    CHECK(r[0].grade == Grade::Miss);
    CHECK(s.complete());
    CHECK(s.accuracy() == doctest::Approx(1.0f / 3.0f));
}

TEST_CASE("jumps accept their arrows in either order") {
    ComboSequence s({{Arrow::Left, 1.0}, {Arrow::Right, 1.0}}, {0.04, 0.08, 0.12}, 0.3);
    auto r = s.press(Arrow::Right, 1.0);
    REQUIRE(r.size() == 1);
    CHECK(r[0].index == 1);
    CHECK(s.nextIndex() == 0);
    r = s.press(Arrow::Left, 1.01);
    REQUIRE(r.size() == 1);
    CHECK(r[0].index == 0);
    CHECK(s.complete());
    CHECK(s.count(Grade::Perfect) == 2);
}

TEST_CASE("combo rating follows move difficulty and the stat matchup") {
    DifficultyTuning t;
    Stats player{.speed = 10, .technique = 10};
    Stats weak{.speed = 5, .technique = 5};
    Stats strong{.speed = 20, .technique = 20};
    const float base = comboRating(3.f, player, player, t);
    CHECK(base == doctest::Approx(3.f));
    CHECK(comboRating(3.f, player, weak, t) < base);
    CHECK(comboRating(3.f, player, strong, t) > base);
    CHECK(comboRating(6.f, player, player, t) > base);
    CHECK(comboRating(3.f, strong, player, t) < base); // a skilled player has it easier
    CHECK(comboRating(100.f, player, player, t) == doctest::Approx(t.maxRating));
    CHECK(comboRating(-100.f, player, player, t) == doctest::Approx(0.f));
}

TEST_CASE("harder ratings mean longer, faster, tighter, busier combos") {
    DifficultyTuning t;
    const ComboParams easy = comboParams(4, 0.f, t);
    const ComboParams hard = comboParams(4, 10.f, t);
    CHECK(easy.steps == 4);
    CHECK(hard.steps > easy.steps);
    CHECK(hard.interval < easy.interval);
    CHECK(hard.windowScale < easy.windowScale);
    CHECK(easy.lanes == 2);
    CHECK(hard.lanes == 4);
    CHECK(easy.jumpChance == 0.f);
    CHECK(easy.burstChance == 0.f);
    CHECK(hard.jumpChance > 0.f);
    CHECK(hard.burstChance > 0.f);
}

TEST_CASE("generated combos are deterministic and respect their parameters") {
    DifficultyTuning t;
    const ComboParams p = comboParams(6, 8.f, t);
    const auto a = generateCombo(p, 2.0, 42);
    const auto b = generateCombo(p, 2.0, 42);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].arrow == b[i].arrow);
        CHECK(a[i].time == b[i].time);
    }
    CHECK(a.front().time == 2.0);
    CHECK(a.size() >= static_cast<std::size_t>(p.steps));
    for (std::size_t i = 1; i < a.size(); ++i) CHECK(a[i].time >= a[i - 1].time);

    const auto easy = generateCombo(comboParams(8, 0.f, t), 0.0, 7);
    std::set<Arrow> lanes;
    for (auto& n : easy) lanes.insert(n.arrow);
    CHECK(easy.size() == 8); // no jumps when easy
    CHECK(lanes.size() <= 2);
    for (std::size_t i = 1; i < easy.size(); ++i) {
        CHECK(easy[i].arrow != easy[i - 1].arrow);
        CHECK(easy[i].time - easy[i - 1].time == doctest::Approx(t.easyInterval));
    }
}

// --- beat-grid charts ---

namespace {
ComboParams gridParams(float rating, double bpm, int steps = 12) {
    DifficultyTuning t;
    ComboParams p = comboParams(steps, rating, t, bpm);
    p.steps = steps;
    return p;
}
bool onMultiple(double beat, double unit) {
    const double q = beat / unit;
    return std::abs(q - std::round(q)) < 1e-9;
}
} // namespace

TEST_CASE("seconds-only charts are unchanged by the grid fields") {
    DifficultyTuning t;
    const ComboParams p = comboParams(6, 5.0f, t);
    CHECK(p.bpm == 0.0);
    const auto a = generateCombo(p, 1.25, 77);
    const auto b = generateCombo(p, 1.25, 77);
    REQUIRE(a.size() == b.size());
    CHECK(a.front().time == 1.25);
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].time == b[i].time);
        CHECK(a[i].arrow == b[i].arrow);
        CHECK(a[i].beat == kNoBeat);
    }
}

TEST_CASE("grid notes land on beat multiples and carry beat and time") {
    for (double bpm : {60.0, 93.0, 120.0, 174.0}) {
        for (float rating : {0.0f, 3.0f, 6.0f, 10.0f}) {
            ComboParams p = gridParams(rating, bpm);
            p.beatOffset = 0.37;
            const auto notes = generateCombo(p, 2.0, 42);
            REQUIRE(!notes.empty());
            const double spb = 60.0 / bpm;
            const double minUnit = p.subdivision * 0.5;
            CHECK(onMultiple(notes.front().beat, 1.0));
            CHECK(notes.front().time >= 2.0 - 1e-9);
            CHECK(notes.front().time < 2.0 + spb + 1e-9);
            double prev = -1.0;
            for (const ComboNote& n : notes) {
                CHECK(onMultiple(n.beat, minUnit));
                CHECK(n.time == doctest::Approx(0.37 + n.beat * spb));
                CHECK(n.beat >= prev);
                prev = n.beat;
            }
        }
    }
}

TEST_CASE("grid charts are deterministic and share arrows with the seconds path") {
    const ComboParams p = gridParams(7.0f, 128.0);
    const auto a = generateComboOnGrid(p, 4.0, 1234);
    const auto b = generateComboOnGrid(p, 4.0, 1234);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].arrow == b[i].arrow);
        CHECK(a[i].beat == b[i].beat);
        CHECK(a[i].time == b[i].time);
    }
    ComboParams secs = p;
    secs.bpm = 0.0;
    const auto c = generateCombo(secs, 0.0, 1234);
    REQUIRE(c.size() == a.size());
    for (std::size_t i = 0; i < a.size(); ++i) CHECK(a[i].arrow == c[i].arrow);
}

TEST_CASE("density rises with rating: subdivision gets finer") {
    for (double bpm : {80.0, 120.0, 150.0}) {
        double prev = 10.0;
        for (float rating = 0.0f; rating <= 10.0f; rating += 1.0f) {
            const double sub = gridParams(rating, bpm).subdivision;
            CHECK(sub <= prev);
            prev = sub;
        }
        CHECK(gridParams(0.0f, bpm).subdivision > gridParams(10.0f, bpm).subdivision);
    }
    // 120 bpm: the old 0.50 s .. 0.22 s range snaps to a beat .. half a beat.
    CHECK(gridParams(0.0f, 120.0).subdivision == 1.0);
    CHECK(gridParams(10.0f, 120.0).subdivision == 0.5);
    CHECK(snapSubdivision(0.22, 60.0) == 0.25);
}

TEST_CASE("bursts halve the subdivision, unless that would be too fast") {
    ComboParams p = gridParams(10.0f, 100.0, 16);
    p.subdivision = 0.5; // 0.3 s per step at 100 bpm, so a burst (0.25 beat) is 0.15 s
    p.burstChance = 1.0f;
    p.jumpChance = 0.0f;
    auto notes = generateComboOnGrid(p, 0.0, 9);
    REQUIRE(notes.size() == 16);
    for (std::size_t i = 1; i < notes.size(); ++i) CHECK(notes[i].beat - notes[i - 1].beat == doctest::Approx(p.subdivision * 0.5));

    p.bpm = 200.0; // burst would be 0.075 s < minStepSeconds
    p.subdivision = 0.5;
    notes = generateComboOnGrid(p, 0.0, 9);
    for (std::size_t i = 1; i < notes.size(); ++i) CHECK(notes[i].beat - notes[i - 1].beat == doctest::Approx(0.5));

    p.burstChance = 0.0f;
    p.bpm = 100.0;
    notes = generateComboOnGrid(p, 0.0, 9);
    for (std::size_t i = 1; i < notes.size(); ++i) CHECK(notes[i].beat - notes[i - 1].beat == doctest::Approx(0.5));
}
