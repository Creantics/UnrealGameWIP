#include "ftcore/Dialogue.h"

namespace ftcore {

namespace {
bool isContinuation(char c) { return (static_cast<unsigned char>(c) & 0xC0u) == 0x80u; }
} // namespace

std::uint32_t utf8Length(std::string_view text) {
    std::uint32_t n = 0;
    for (const char c : text)
        if (!isContinuation(c)) ++n;
    return n;
}

std::size_t utf8PrefixBytes(std::string_view text, std::uint32_t chars) {
    std::size_t i = 0;
    std::uint32_t seen = 0;
    while (i < text.size()) {
        if (!isContinuation(text[i])) {
            if (seen == chars) break;
            ++seen;
        }
        ++i;
    }
    return i;
}

bool applyDialogueCommand(StoryRunner& r, const DialogueCommand& c) {
    switch (c.kind) {
    case DialogueCommand::Kind::Advance: return r.advance();
    case DialogueCommand::Kind::Choose: return r.choose(c.option);
    case DialogueCommand::Kind::Resume: return r.resume();
    case DialogueCommand::Kind::SkipHold:
        if (r.state() != StoryRunner::State::Hold) return false;
        r.skipHold();
        return true;
    case DialogueCommand::Kind::None: break;
    }
    return false;
}

void DialoguePresenter::setRunner(const StoryRunner* runner) {
    runner_ = runner;
    fresh_ = true;
    sync();
}

bool DialoguePresenter::sync() {
    const StoryRunner::State s = runner_ ? runner_->state() : StoryRunner::State::Idle;
    const std::uint32_t e = runner_ ? runner_->beatEpoch() : 0;
    if (!fresh_ && s == state_ && e == epoch_) return false;
    fresh_ = false;
    state_ = s;
    epoch_ = e;
    text_ = runner_ ? runner_->text() : std::string_view{};
    total_ = utf8Length(text_);
    shown_ = 0;
    shownBytes_ = 0;
    progress_ = 0.f;
    beatTime_ = 0.f;
    if (rate_ <= 0.f) reveal(total_);
    return true;
}

void DialoguePresenter::update(float dt) {
    sync();
    if (dt <= 0.f) return;
    beatTime_ += dt;
    if (revealed()) return;
    if (rate_ <= 0.f) {
        reveal(total_);
        return;
    }
    progress_ += dt * rate_;
    const float cap = static_cast<float>(total_);
    if (progress_ > cap) progress_ = cap;
    reveal(static_cast<std::uint32_t>(progress_));
}

void DialoguePresenter::reveal(std::uint32_t chars) {
    if (chars > total_) chars = total_;
    if (chars <= shown_) return;
    // Walk on from the last revealed character rather than from the start.
    std::size_t i = shownBytes_;
    std::uint32_t seen = shown_;
    while (i < text_.size()) {
        if (!isContinuation(text_[i])) {
            if (seen == chars) break;
            ++seen;
        }
        ++i;
    }
    shown_ = chars;
    shownBytes_ = i;
    if (static_cast<float>(shown_) > progress_) progress_ = static_cast<float>(shown_);
}

std::string_view DialoguePresenter::revealedText() const { return text_.substr(0, shownBytes_); }

DialogueCommand DialoguePresenter::press() {
    sync();
    if (!revealed()) {
        revealAll();
        return {};
    }
    switch (state_) {
    case StoryRunner::State::Line:
    case StoryRunner::State::Direction: return {DialogueCommand::Kind::Advance};
    case StoryRunner::State::Gameplay: return {DialogueCommand::Kind::Resume};
    default: return {};
    }
}

DialogueCommand DialoguePresenter::choose(std::uint32_t i) {
    sync();
    if (state_ != StoryRunner::State::Choice) return {};
    if (!revealed()) {
        revealAll();
        return {};
    }
    if (i >= runner_->optionCount() || !runner_->optionSelectable(i)) return {};
    return {DialogueCommand::Kind::Choose, i};
}

DialogueCommand DialoguePresenter::autoplay() const {
    if (!runner_ || !revealed()) return {};
    // Read the runner's own state: the owner may not have synced since its last move.
    if (runner_->beatEpoch() != epoch_ || runner_->state() != state_) return {};
    switch (state_) {
    case StoryRunner::State::Line:
    case StoryRunner::State::Direction: return {DialogueCommand::Kind::Advance};
    case StoryRunner::State::Gameplay: return {DialogueCommand::Kind::Resume};
    case StoryRunner::State::Hold: return {DialogueCommand::Kind::SkipHold};
    case StoryRunner::State::Choice: {
        std::uint32_t pick = runner_->defaultOption();
        for (std::uint32_t i = 0; pick == kNone && i < runner_->optionCount(); ++i)
            if (runner_->optionSelectable(i)) pick = i;
        if (pick == kNone) return {};
        return {DialogueCommand::Kind::Choose, pick};
    }
    default: return {};
    }
}

} // namespace ftcore
