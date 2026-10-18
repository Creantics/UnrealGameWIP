#include "ftcore/Conductor.h"

#include <algorithm>
#include <cmath>

namespace ftcore {

void Conductor::reset() {
    const ConductorConfig cfg = cfg_;
    *this = Conductor(cfg);
}

void Conductor::pushSample(const Sample& s) {
    if (count_ == kMaxSamples) {
        head_ = (head_ + 1) % kMaxSamples;
        --count_;
    }
    samples_[(head_ + count_) % kMaxSamples] = s;
    ++count_;
}

void Conductor::estimate() {
    if (count_ == 0) return;
    double sum = 0.0;
    double minDelta = 0.0;
    for (std::size_t i = 0; i < count_; ++i) {
        const Sample& s = sampleAt(i);
        sum += s.d;
        if (s.delta > 0.0 && (minDelta == 0.0 || s.delta < minDelta)) minDelta = s.delta;
    }
    if (cfg_.cursorStepHint > 0.0)
        step_ = cfg_.cursorStepHint;
    else if (minDelta > 0.0)
        step_ = minDelta;
    dEst_ = sum / static_cast<double>(count_) + 0.5 * step_;
}

void Conductor::snapTo(double d) { dEst_ = dOut_ = d; }

void Conductor::observe(double c, double w) {
    if (!seen_) {
        seen_ = true;
        cursor_ = c;
        lastChangeWall_ = lastObsWall_ = lastSlewWall_ = w;
        pushSample({w, c - w, 0.0});
        estimate();
        snapTo(dEst_);
        lastOut_ = std::min(w + dOut_, cursor_ + cfg_.maxExtrapolation);
        return;
    }

    const double delta = c - cursor_;
    const bool changed = std::abs(delta) > 1e-12;
    bool snap = false;

    if (paused_) {
        if (!changed) {
            lastObsWall_ = w;
            return;
        }
        // The cursor moves again: a small forward step is a resume, anything else a seek.
        paused_ = false;
        clearSamples();
        snap = true;
        if (delta < 0.0 || delta > cfg_.snapThreshold) {
            ++seeks_;
            allowBackwards_ = true;
        }
    } else if (changed && std::abs((c - w) - (dEst_ - 0.5 * step_)) > cfg_.snapThreshold) {
        // The cursor disagrees with our model by more than any device period could: seek or restart.
        ++seeks_;
        allowBackwards_ = true;
        clearSamples();
        snap = true;
    } else if (!changed) {
        const double stuck = w - lastChangeWall_;
        if (stuck > cfg_.maxExtrapolation) {
            paused_ = true;
            lastObsWall_ = w;
            return;
        }
        // A cursor that sits still longer than a device period would says nothing about the
        // clock's offset: keep it out of the average.
        if (stuck > 2.0 * std::max(step_, 0.03)) {
            lastObsWall_ = w;
            return;
        }
    }

    if (changed) lastChangeWall_ = w;
    cursor_ = c;
    pushSample({w, c - w, !snap && changed && delta > 0.0 ? delta : 0.0});
    while (count_ > 1 && w - samples_[head_].wall > cfg_.windowSeconds) {
        head_ = (head_ + 1) % kMaxSamples;
        --count_;
    }
    estimate();

    const double dt = std::max(0.0, w - lastSlewWall_);
    lastSlewWall_ = w;
    lastObsWall_ = w;
    const double err = dEst_ - dOut_;
    if (snap || std::abs(err) > cfg_.snapThreshold) {
        snapTo(dEst_);
    } else {
        const double maxStep = cfg_.slewRate * dt;
        dOut_ += std::clamp(err, -maxStep, maxStep);
    }
}

double Conductor::songTime(double w) {
    if (!seen_) return 0.0;
    double v = std::min(w + dOut_, cursor_ + cfg_.maxExtrapolation);
    if (!allowBackwards_) v = std::max(v, lastOut_);
    allowBackwards_ = false;
    lastOut_ = v;
    return v;
}

double Conductor::songTimeForEvent(double eventWall) const {
    if (!seen_) return eventWall - cfg_.latencyOffset;
    const double v = std::min(eventWall + dEst_, cursor_ + cfg_.maxExtrapolation);
    return v - cfg_.latencyOffset;
}

} // namespace ftcore
