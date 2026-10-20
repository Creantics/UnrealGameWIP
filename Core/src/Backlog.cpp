#include "ftcore/Backlog.h"

namespace ftcore {

bool DialogueBacklog::noteShown(const StoryRunner& runner, const DialoguePresenter& presenter) {
    if (!presenter.revealed() || presenter.totalChars() == 0) return false;
    if (runner.beatEpoch() != presenter.beatEpoch() || runner.state() != presenter.state()) return false;
    if (presenter.beatEpoch() == lastEpoch_) return false;
    BacklogEntry e;
    switch (presenter.state()) {
    case StoryRunner::State::Line: e.kind = BacklogEntry::Kind::Line; break;
    case StoryRunner::State::Direction: e.kind = BacklogEntry::Kind::Direction; break;
    case StoryRunner::State::Choice: e.kind = BacklogEntry::Kind::Prompt; break;
    default: return false;
    }
    lastEpoch_ = presenter.beatEpoch();
    e.scene = runner.sceneIndex();
    e.beat = runner.beatIndex();
    push(e);
    return true;
}

bool DialogueBacklog::noteChoice(const StoryRunner& runner, std::uint32_t option) {
    if (runner.state() != StoryRunner::State::Choice || option >= runner.optionCount()) return false;
    BacklogEntry e;
    e.kind = BacklogEntry::Kind::Choice;
    e.scene = runner.sceneIndex();
    e.beat = runner.beatIndex();
    e.option = option;
    push(e);
    return true;
}

namespace {
const Beat* beatOf(const BacklogEntry& e, const StoryDatabase& db) {
    if (e.scene >= db.sceneCount()) return nullptr;
    const Scene& s = db.scene(e.scene);
    if (e.beat >= s.beatCount) return nullptr;
    return &db.beat(s, e.beat);
}
} // namespace

std::string_view DialogueBacklog::speaker(const BacklogEntry& e, const StoryDatabase& db) {
    if (e.kind != BacklogEntry::Kind::Line) return {};
    const Beat* b = beatOf(e, db);
    return b ? db.view(b->speaker) : std::string_view{};
}

std::string_view DialogueBacklog::text(const BacklogEntry& e, const StoryDatabase& db) {
    const Beat* b = beatOf(e, db);
    if (!b) return {};
    if (e.kind == BacklogEntry::Kind::Choice) {
        if (e.option >= b->optionCount) return {};
        return db.view(db.option(*b, e.option).text);
    }
    return db.view(b->text);
}

} // namespace ftcore
