#include "ftcore/BeatScreen.h"

#include <algorithm>

namespace ftcore {

namespace {

// The autopilot's moves in one step of the clock. A step is at most BeatScreen::kAutoStep long, so a
// handful is plenty; the bound only keeps a bad fight from looping.
constexpr int kBeatMaxPilotMoves = 64;

} // namespace

void BeatScreen::begin(bool hasStep, const BeatSetup& setup, std::uint32_t seed, bool autoPlay, const QteAssist& assist,
                       double now) {
    autoPlay_ = hasStep && autoPlay;
    autoComplete_ = hasStep && !autoPlay_ && assist.autoComplete;
    // Under autoplay the screen runs the autopilot itself, so the fight must not also play on its own.
    QteAssist used = assist;
    used.autoComplete = autoComplete_;
    fight_.begin(setup.def, setup.tuning, now, seed, used);
    fresh_.count = 0;
    outcome_ = BeatOutcome::None;
    gaveUp_ = false;
    autoRetries_ = 0;
    now_ = now;
    playSeconds_ = 0.0;
    graceUntil_ = now + kInputGrace;
    endedAt_ = now;
    promptAt_ = now;
    phase_ = hasStep ? BeatScreenPhase::Playing : BeatScreenPhase::NoStep;
}

void BeatScreen::collect(const BeatEvents& e) {
    for (const BeatEvent& ev : e)
        if (fresh_.count < fresh_.items.size()) fresh_.items[fresh_.count++] = ev;
}

// The autopilot's moves up to `horizon`, each at its own time.
void BeatScreen::pilot(double horizon) {
    for (int moves = 0; moves < kBeatMaxPilotMoves && fight_.phase() == BeatPhase::Playing; ++moves) {
        const QteAction a = fight_.autopilot(horizon);
        if (a.kind == QteAction::Kind::None) return;
        collect(a.kind == QteAction::Kind::Press ? fight_.press(a.input, a.time) : fight_.release(a.input, a.time));
    }
}

// After a call that moved the fight's clock from `before`: the time played, the screen's clock, and the
// end of the fight.
void BeatScreen::settle(double before) {
    const double after = fight_.now();
    if (after > before) playSeconds_ += after - before;
    now_ = std::max(now_, after);
    if (phase_ == BeatScreenPhase::Playing && fight_.over()) {
        phase_ = BeatScreenPhase::Result;
        endedAt_ = now_;
    }
}

// Moves the fight to `now`; under autoplay the autopilot plays on the way.
void BeatScreen::run(double now) {
    const double before = fight_.now();
    if (autoPlay_) {
        int steps = 0;
        while (fight_.phase() == BeatPhase::Playing && fight_.now() < now && steps++ < kAutoMaxSteps) {
            const double target = std::min(now, fight_.now() + kAutoStep);
            pilot(target);
            if (fight_.phase() != BeatPhase::Playing) break;
            collect(fight_.update(target));
        }
    }
    // Under autoplay this is only what a frame longer than kAutoMaxSteps steps leaves; otherwise the whole
    // move (auto-complete plays inside update()).
    if (fight_.phase() == BeatPhase::Playing) collect(fight_.update(now));
    settle(before);
}

void BeatScreen::tick(double now) {
    if (phase_ == BeatScreenPhase::NoStep || phase_ == BeatScreenPhase::Done) return;
    if (!(now > now_)) now = now_; // time never runs back (a NaN doesn't move it either)
    if (phase_ == BeatScreenPhase::Playing) {
        run(now);
        now_ = std::max(now_, now);
        // A fight that ended in this call shows its result from the end of the frame, so a long frame
        // can't use the result up unseen.
        if (phase_ == BeatScreenPhase::Result) endedAt_ = now_;
        return;
    }
    now_ = now;
    if (phase_ == BeatScreenPhase::Result &&
        now_ - endedAt_ >= (autoPlay_ ? kAutoResultTime : kResultTime))
        afterResult();
}

// The result has been shown: a victory is done; a defeat asks, or under autoplay tries again (or gives up).
void BeatScreen::afterResult() {
    if (fight_.phase() == BeatPhase::Victory) {
        outcome_ = BeatOutcome::Won;
        phase_ = BeatScreenPhase::Done;
        return;
    }
    if (!autoPlay_) {
        phase_ = BeatScreenPhase::RetryPrompt;
        promptAt_ = now_;
        return;
    }
    if (autoRetries_ < kAutoRetries) {
        ++autoRetries_;
        restartFight();
        return;
    }
    gaveUp_ = true;
    outcome_ = BeatOutcome::Lost;
    phase_ = BeatScreenPhase::Done;
}

void BeatScreen::restartFight() {
    fight_.restart(now_);
    fresh_.count = 0;
    graceUntil_ = now_ + kInputGrace;
    phase_ = BeatScreenPhase::Playing;
}

void BeatScreen::press(Arrow input, double t) {
    if (phase_ != BeatScreenPhase::Playing || autoPlay_ || t < graceUntil_) return;
    const double before = fight_.now();
    collect(fight_.press(input, t));
    settle(before);
}

void BeatScreen::release(Arrow input, double t) {
    if (phase_ != BeatScreenPhase::Playing || autoPlay_ || t < graceUntil_) return;
    const double before = fight_.now();
    collect(fight_.release(input, t));
    settle(before);
}

bool BeatScreen::choose(BeatChoice choice) {
    if (phase_ != BeatScreenPhase::RetryPrompt || autoPlay_ || now_ - promptAt_ < kPromptGrace) return false;
    if (choice == BeatChoice::Retry) {
        restartFight();
    } else {
        outcome_ = BeatOutcome::Lost;
        phase_ = BeatScreenPhase::Done;
    }
    return true;
}

double BeatScreen::resultAge() const {
    const bool over = phase_ == BeatScreenPhase::Result || phase_ == BeatScreenPhase::RetryPrompt ||
                      phase_ == BeatScreenPhase::Done;
    return over ? std::max(0.0, now_ - endedAt_) : 0.0;
}

double BeatScreen::leadInBeats() const {
    if (phase_ != BeatScreenPhase::Playing) return 0.0;
    return std::max(0.0, fight_.firstBarBeat() - fight_.beatAt(now_));
}

} // namespace ftcore
