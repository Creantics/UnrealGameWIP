#include "ftcore/Skip.h"

namespace ftcore {

DialogueCommand SkipController::update(float dt, DialoguePresenter& presenter, const SeenLines& seen) {
    skipping_ = false;
    if (!held_ || !presenter.runner()) {
        timer_ = 0.f;
        return {};
    }
    presenter.sync();
    const StoryRunner& runner = *presenter.runner();
    switch (presenter.state()) {
    case StoryRunner::State::Line:
    case StoryRunner::State::Direction:
        if (mode_ == SkipMode::Seen && !seen.seen(runner)) {
            timer_ = 0.f; // an unseen line: let it play
            return {};
        }
        skipping_ = true;
        if (!presenter.revealed()) presenter.revealAll();
        if (dt > 0.f) timer_ += dt;
        if (timer_ < interval_) return {};
        timer_ = 0.f;
        return presenter.autoplay(); // Advance
    case StoryRunner::State::Hold:
        skipping_ = true;
        return presenter.autoplay(); // SkipHold
    default: break;
    }
    timer_ = 0.f; // a choice, a gameplay beat or the end: skipping stops here
    return {};
}

DialogueCommand AutoAdvancer::update(float dt, const DialoguePresenter& presenter) {
    const StoryRunner::State state = presenter.state();
    if (!enabled_ || (state != StoryRunner::State::Line && state != StoryRunner::State::Direction)) {
        timer_ = 0.f;
        return {};
    }
    if (presenter.beatEpoch() != epoch_) {
        epoch_ = presenter.beatEpoch();
        timer_ = 0.f;
    }
    if (!presenter.revealed()) {
        timer_ = 0.f;
        return {};
    }
    if (dt > 0.f) timer_ += dt;
    if (timer_ < delay_) return {};
    timer_ = 0.f;
    return presenter.autoplay();
}

} // namespace ftcore
