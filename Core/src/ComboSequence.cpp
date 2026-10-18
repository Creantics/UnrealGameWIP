#include "ftcore/ComboSequence.h"

#include "ftcore/Assert.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

const char* arrowName(Arrow a) {
    switch (a) {
    case Arrow::Left: return "LEFT";
    case Arrow::Down: return "DOWN";
    case Arrow::Up: return "UP";
    case Arrow::Right: return "RIGHT";
    }
    return "?";
}

ComboSequence::ComboSequence(std::vector<ComboNote> notes, JudgeWindows windows, double lockout, GradeWeights weights)
    : notes_(std::move(notes)), grades_(notes_.size(), Grade::Pending), windows_(windows), weights_(weights),
      lockout_(lockout) {
    FTCORE_ASSERT(std::is_sorted(notes_.begin(), notes_.end(),
                             [](const ComboNote& a, const ComboNote& b) { return a.time < b.time; }));
    FTCORE_ASSERT(windows_.perfect <= windows_.great && windows_.great <= windows_.good);
}

std::size_t ComboSequence::count(Grade g) const {
    return static_cast<std::size_t>(std::count(grades_.begin(), grades_.end(), g));
}

Grade ComboSequence::gradeFor(double error) const {
    const double e = std::abs(error);
    if (e <= windows_.perfect) return Grade::Perfect;
    if (e <= windows_.great) return Grade::Great;
    return Grade::Good;
}

NoteResult ComboSequence::judge(std::size_t i, Grade g, double error, bool wrong) {
    grades_[i] = g;
    ++judged_;
    score_ += weights_.of(g);
    combo_ = g == Grade::Miss ? 0 : combo_ + 1;
    maxCombo_ = std::max(maxCombo_, combo_);
    advance();
    return {i, g, error, wrong};
}

void ComboSequence::advance() {
    while (next_ < notes_.size() && grades_[next_] != Grade::Pending) ++next_;
}

void ComboSequence::reserve(std::size_t notes) {
    notes_.reserve(notes);
    grades_.reserve(notes);
}

void ComboSequence::reset(std::vector<ComboNote>& notes, JudgeWindows windows, double lockout, GradeWeights weights) {
    FTCORE_ASSERT(std::is_sorted(notes.begin(), notes.end(),
                             [](const ComboNote& a, const ComboNote& b) { return a.time < b.time; }));
    FTCORE_ASSERT(windows.perfect <= windows.great && windows.great <= windows.good);
    notes_.swap(notes);
    grades_.assign(notes_.size(), Grade::Pending);
    windows_ = windows;
    weights_ = weights;
    lockout_ = lockout;
    lockoutUntil_ = -1e30;
    next_ = judged_ = 0;
    score_ = 0.0f;
    combo_ = maxCombo_ = strays_ = 0;
}

void ComboSequence::update(double now, std::vector<NoteResult>& out) {
    for (std::size_t i = next_; i < notes_.size() && notes_[i].time + windows_.good < now; ++i)
        if (grades_[i] == Grade::Pending) out.push_back(judge(i, Grade::Miss, 0.0, false));
}

std::vector<NoteResult> ComboSequence::update(double now) {
    std::vector<NoteResult> out;
    update(now, out);
    return out;
}

std::vector<NoteResult> ComboSequence::press(Arrow arrow, double t) {
    std::vector<NoteResult> out;
    press(arrow, t, out);
    return out;
}

void ComboSequence::press(Arrow arrow, double t, std::vector<NoteResult>& out) {
    update(t, out);
    if (complete() || lockedOut(t)) return;

    // Pending notes from next_ onward are not expired, so "in window" only needs the early edge.
    std::size_t firstInWindow = notes_.size();
    for (std::size_t i = next_; i < notes_.size() && notes_[i].time - windows_.good <= t; ++i) {
        if (grades_[i] != Grade::Pending) continue;
        if (notes_[i].arrow == arrow) {
            const double error = t - notes_[i].time;
            out.push_back(judge(i, gradeFor(error), error, false));
            return;
        }
        firstInWindow = std::min(firstInWindow, i);
    }
    if (firstInWindow < notes_.size()) {
        out.push_back(judge(firstInWindow, Grade::Miss, t - notes_[firstInWindow].time, true));
    } else {
        ++strays_;
        lockoutUntil_ = t + lockout_;
    }
}

} // namespace ftcore
