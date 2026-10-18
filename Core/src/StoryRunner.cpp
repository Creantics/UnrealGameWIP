#include "ftcore/StoryRunner.h"

namespace ftcore {

namespace {
bool anySelectable(const StoryDatabase& db, const Beat& b) {
    for (std::uint32_t i = 0; i < b.optionCount; ++i)
        if (db.option(b, i).selectable) return true;
    return false;
}
} // namespace

void StoryRunner::setDatabase(const StoryDatabase* db) {
    db_ = db;
    flags_.assign(db ? db->flagCount() : 0, FlagValue{});
    resetFlags();
    stop();
}

void StoryRunner::stop() {
    state_ = State::Idle;
    scene_ = kNone;
    beat_ = 0;
    hold_ = 0.f;
}

void StoryRunner::resetFlags() {
    if (!db_) return;
    for (std::uint32_t i = 0; i < flags_.size(); ++i) flags_[i] = db_->flag(i).initial;
}

bool StoryRunner::start(std::string_view sceneId) {
    if (!db_ || !db_->loaded()) {
        state_ = State::Finished;
        return false;
    }
    const std::uint32_t s = sceneId.empty() ? db_->startScene() : db_->findScene(sceneId);
    if (s == kNone) {
        state_ = State::Finished;
        return false;
    }
    resetFlags();
    enterScene(s);
    settle();
    return true;
}

bool StoryRunner::restore(const StoryPosition& pos) {
    if (!db_ || !db_->loaded()) return false;
    const std::uint32_t s = db_->findScene(pos.scene);
    if (s == kNone) return false;
    enterScene(s);
    beat_ = pos.beat;
    settle();
    return true;
}

StoryPosition StoryRunner::position() const {
    StoryPosition p;
    if (scene_ != kNone) {
        p.scene = db_->view(db_->scene(scene_).id);
        p.beat = beat_;
    }
    return p;
}

const Beat* StoryRunner::beat() const {
    if (scene_ == kNone) return nullptr;
    const Scene& sc = db_->scene(scene_);
    return beat_ < sc.beatCount ? &db_->beat(sc, beat_) : nullptr;
}

// ---- moving -----------------------------------------------------------------------------------

void StoryRunner::enterScene(std::uint32_t s) {
    scene_ = s;
    beat_ = 0;
    ++sceneEpoch_;
}

void StoryRunner::endScene(std::uint32_t next) {
    const std::uint32_t target = next != kNone ? next : db_->scene(scene_).orderNext;
    if (target == kNone) {
        state_ = State::Finished;
        ++beatEpoch_;
        return;
    }
    enterScene(target);
}

void StoryRunner::rest(State s) {
    state_ = s;
    ++beatEpoch_;
}

void StoryRunner::applyFlagOp(const FlagOpDef& op) {
    if (op.flag == kNone || op.flag >= flags_.size()) return;
    if (op.op == FlagOp::Set) {
        flags_[op.flag] = op.value;
    } else if (op.op == FlagOp::Clear) {
        // Clearing a counter or enum flag returns it to its default; a boolean becomes false.
        const FlagValue init = db_->flag(op.flag).initial;
        flags_[op.flag] = init.kind == FlagValue::Kind::Bool ? FlagValue::boolean(false) : init;
    }
}

void StoryRunner::skipRequireBlock() {
    const Scene& sc = db_->scene(scene_);
    for (std::uint32_t i = beat_ + 1; i < sc.beatCount; ++i) {
        const Beat& b = db_->beat(sc, i);
        if (b.type == BeatType::Flag && b.flagOp.op == FlagOp::End) {
            beat_ = i + 1;
            return;
        }
    }
    beat_ = sc.beatCount;
}

void StoryRunner::settle() {
    state_ = State::Idle;
    stalled_ = false;
    for (std::uint32_t steps = 0; steps < kMaxStepsPerMove; ++steps) {
        if (scene_ == kNone) {
            state_ = State::Finished;
            return;
        }
        const Scene& sc = db_->scene(scene_);
        if (beat_ >= sc.beatCount) {
            endScene(kNone);
            if (state_ == State::Finished) return;
            continue;
        }
        const Beat& b = db_->beat(sc, beat_);
        switch (b.type) {
        case BeatType::Line: rest(State::Line); return;
        case BeatType::Direction:
            if (b.hidden) break;
            rest(State::Direction);
            return;
        case BeatType::Choice:
            if (!anySelectable(*db_, b)) break; // nothing to pick (an authored greyed-out choice)
            rest(State::Choice);
            return;
        case BeatType::Gameplay: rest(State::Gameplay); return;
        case BeatType::Hold:
            if (b.seconds <= 0.f) break;
            hold_ = b.seconds;
            rest(State::Hold);
            return;
        case BeatType::Cue:
            if (onCue_ && b.cue != kNone) {
                const CueDef& c = db_->cue(b.cue);
                CueEvent e;
                e.id = db_->view(c.id);
                e.kind = db_->view(c.kind);
                e.action = b.cueAction;
                e.cue = b.cue;
                e.scene = scene_;
                e.beat = beat_;
                onCue_(cueUser_, e);
            }
            break;
        case BeatType::Flag:
            if (b.flagOp.op == FlagOp::Require) {
                const FlagValue have = b.flagOp.flag < flags_.size() ? flags_[b.flagOp.flag] : FlagValue{};
                const FlagValue want = b.flagOp.value;
                const bool ok = want.kind == FlagValue::Kind::Bool ? have.truthy() == want.truthy() : have == want;
                if (!ok) {
                    skipRequireBlock();
                    continue;
                }
            } else {
                applyFlagOp(b.flagOp);
            }
            break;
        case BeatType::End:
            endScene(b.nextScene);
            if (state_ == State::Finished) return;
            continue;
        case BeatType::Unknown: break;
        }
        ++beat_;
    }
    stalled_ = true;
    state_ = State::Finished;
}

bool StoryRunner::advance() {
    if (state_ != State::Line && state_ != State::Direction) return false;
    ++beat_;
    settle();
    return true;
}

bool StoryRunner::choose(std::uint32_t i) {
    if (state_ != State::Choice) return false;
    const Beat& b = *beat();
    if (i >= b.optionCount) return false;
    const Option& o = db_->option(b, i);
    if (!o.selectable) return false;
    for (std::uint32_t e = 0; e < o.effectCount; ++e) applyFlagOp(db_->effect(o, e));
    beat_ = o.next == kNone ? beat_ + 1 : o.next;
    settle();
    return true;
}

void StoryRunner::update(float dt) {
    if (state_ != State::Hold) return;
    hold_ -= dt;
    if (hold_ > 0.f) return;
    hold_ = 0.f;
    ++beat_;
    settle();
}

void StoryRunner::skipHold() {
    if (state_ != State::Hold) return;
    hold_ = 0.f;
    ++beat_;
    settle();
}

bool StoryRunner::resume() {
    if (state_ != State::Gameplay) return false;
    ++beat_;
    settle();
    return true;
}

// ---- current beat -----------------------------------------------------------------------------

std::string_view StoryRunner::text() const {
    const Beat* b = beat();
    if (!b || state_ == State::Idle || state_ == State::Finished || state_ == State::Hold) return {};
    return db_->view(b->text);
}

std::string_view StoryRunner::textKey() const {
    const Beat* b = beat();
    if (!b || state_ == State::Idle || state_ == State::Finished || state_ == State::Hold) return {};
    return db_->view(b->loc);
}

std::string_view StoryRunner::speaker() const {
    const Beat* b = beat();
    return b && state_ == State::Line ? db_->view(b->speaker) : std::string_view{};
}

std::string_view StoryRunner::direction() const {
    const Beat* b = beat();
    return b && state_ == State::Line ? db_->view(b->direction) : std::string_view{};
}

std::string_view StoryRunner::lineId() const {
    const Beat* b = beat();
    return b && state_ == State::Line ? db_->view(b->id) : std::string_view{};
}

std::uint32_t StoryRunner::optionCount() const {
    const Beat* b = beat();
    return b && state_ == State::Choice ? b->optionCount : 0;
}

std::string_view StoryRunner::optionText(std::uint32_t i) const {
    if (i >= optionCount()) return {};
    return db_->view(db_->option(*beat(), i).text);
}

std::string_view StoryRunner::optionTextKey(std::uint32_t i) const {
    if (i >= optionCount()) return {};
    return db_->view(db_->option(*beat(), i).loc);
}

bool StoryRunner::optionSelectable(std::uint32_t i) const {
    return i < optionCount() && db_->option(*beat(), i).selectable;
}

bool StoryRunner::optionReturns(std::uint32_t i) const {
    if (i >= optionCount()) return false;
    const Option& o = db_->option(*beat(), i);
    return o.miss || o.next == beat_;
}

std::uint32_t StoryRunner::defaultOption() const {
    const std::uint32_t n = optionCount();
    std::uint32_t fallback = kNone;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (!optionSelectable(i)) continue;
        if (!optionReturns(i)) return i;
        if (fallback == kNone) fallback = i;
    }
    return fallback;
}

bool StoryRunner::scripted() const {
    const Beat* b = beat();
    return b && state_ == State::Choice && b->scripted;
}

GameplayKind StoryRunner::gameplayKind() const {
    const Beat* b = beat();
    return b && state_ == State::Gameplay ? b->kind : GameplayKind::Other;
}

std::string_view StoryRunner::gameplayKindName() const {
    const Beat* b = beat();
    return b && state_ == State::Gameplay ? db_->view(b->kindName) : std::string_view{};
}

std::string_view StoryRunner::param(std::string_view key) const {
    const Beat* b = beat();
    return b && state_ == State::Gameplay ? db_->param(*b, key) : std::string_view{};
}

std::uint32_t StoryRunner::paramCount() const {
    const Beat* b = beat();
    return b && state_ == State::Gameplay ? b->paramCount : 0;
}

std::string_view StoryRunner::paramKey(std::uint32_t i) const {
    return i < paramCount() ? db_->paramKey(*beat(), i) : std::string_view{};
}

std::string_view StoryRunner::paramValue(std::uint32_t i) const {
    return i < paramCount() ? db_->paramValue(*beat(), i) : std::string_view{};
}

// ---- flags ------------------------------------------------------------------------------------

FlagValue StoryRunner::flag(std::string_view id) const {
    if (!db_) return {};
    const std::uint32_t i = db_->findFlag(id);
    return i < flags_.size() ? flags_[i] : FlagValue{};
}

bool StoryRunner::setFlag(std::uint32_t index, FlagValue v) {
    if (index >= flags_.size()) return false;
    flags_[index] = v;
    return true;
}

bool StoryRunner::setFlag(std::string_view id, FlagValue v) {
    return db_ && setFlag(db_->findFlag(id), v);
}

} // namespace ftcore
