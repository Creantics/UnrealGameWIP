#include "ftcore/StoryBridge.h"

namespace ftcore {

namespace {
void addError(std::string* error, const std::string& line) {
    if (!error) return;
    if (!error->empty()) error->push_back('\n');
    error->append(line);
}
} // namespace

bool StoryBridge::bind(StoryRunner& runner, StoryState& state, const FactRegistry& reg,
                       std::span<const StorySceneLink> links, std::string* error) {
    unbind();
    if (error) error->clear();
    const StoryDatabase* db = runner.database();
    if (!db || !db->loaded()) {
        addError(error, "the runner has no story");
        return false;
    }
    bool ok = true;
    flagFact_.assign(db->flagCount(), 0);
    for (std::uint32_t i = 0; i < db->flagCount(); ++i) {
        const std::string_view id = db->view(db->flag(i).id);
        if (!id.starts_with(kFlagPrefix)) continue;
        const std::string_view factId = id.substr(kFlagPrefix.size());
        const std::uint16_t slot = storyFactSlot(reg, factId);
        if (slot == 0) {
            addError(error, "flag '" + std::string(id) + "' names no fact of the registry");
            ok = false;
        } else if (db->flag(i).initial.kind == FlagValue::Kind::String) {
            addError(error, "flag '" + std::string(id) + "' is a string flag; a fact flag is a bool or an int");
            ok = false;
        } else {
            flagFact_[i] = slot;
        }
    }
    sceneOrdinal_.assign(db->sceneCount(), kNoScene);
    if (links.empty()) {
        for (std::uint32_t s = 0; s < db->sceneCount(); ++s)
            sceneOrdinal_[s] = storySceneOrdinal(reg, db->view(db->scene(s).id));
    } else {
        for (const StorySceneLink& l : links) {
            const std::uint32_t s = db->findScene(l.scene);
            if (s == kNone) {
                addError(error, "the scene map names scene '" + l.scene + "', which the story does not have");
                ok = false;
            } else {
                sceneOrdinal_[s] = l.ordinal;
            }
        }
    }
    std::uint32_t flags = 0, scenes = 0;
    for (const std::uint16_t f : flagFact_) flags += f != 0 ? 1u : 0u;
    for (const std::uint8_t o : sceneOrdinal_) scenes += o != kNoScene ? 1u : 0u;
    if (ok && flags > 0 && scenes == 0) {
        addError(error, "the story has fact flags, but none of its scenes plays a registry scene");
        ok = false;
    }
    if (!ok) {
        flagFact_.clear();
        sceneOrdinal_.clear();
        return false;
    }
    boundFlags_ = flags;
    mappedScenes_ = scenes;
    runner_ = &runner;
    db_ = db;
    state_ = &state;
    reg_ = &reg;
    lastScene_ = runner.sceneIndex();
    runner.setSceneHandler(&StoryBridge::onScene, this);
    runner.setFlagHandler(&StoryBridge::onFlag, this);
    return true;
}

void StoryBridge::unbind() {
    if (runner_) {
        runner_->setSceneHandler(nullptr, nullptr);
        runner_->setFlagHandler(nullptr, nullptr);
    }
    runner_ = nullptr;
    db_ = nullptr;
    state_ = nullptr;
    reg_ = nullptr;
    flagFact_.clear();
    sceneOrdinal_.clear();
    lastScene_ = kNone;
    restartScene_ = kNone;
    restoring_ = false;
    syncMissed_ = false;
    boundFlags_ = mappedScenes_ = 0;
}

void StoryBridge::onScene(void* user, StoryRunner& r, SceneEntry how) {
    StoryBridge& b = *static_cast<StoryBridge*>(user);
    if (&r != b.runner_ || r.database() != b.db_) return;
    const std::uint32_t scene = r.sceneIndex();
    if (how == SceneEntry::Start) {
        b.restoring_ = false; // a new game instead of the load
    } else if (how == SceneEntry::Restore) {
        b.restoring_ = true; // the state is not decoded yet: sync() takes over
        b.syncMissed_ = false;
        return;
    }
    if (b.pausedForLoad(r)) return;
    switch (how) {
    case SceneEntry::Start:
        storyReset(*b.state_);
        b.lastScene_ = kNone;
        b.enter(scene);
        break;
    case SceneEntry::Next: b.enter(scene); break;
    case SceneEntry::Restore: break;
    case SceneEntry::Restart: {
        const std::uint8_t ord = b.ordinalOf(scene);
        if (ord == kNoScene) break;
        if (b.state_->openScene != ord || !b.restartState(scene)) {
            StoryBridgeReport rep;
            rep.kind = StoryBridgeReport::Kind::SceneRefused;
            rep.scene = scene;
            rep.ordinal = ord;
            b.report(rep);
        }
        b.push(ord);
        break;
    }
    case SceneEntry::End: {
        const StorySceneStep step = storyEnterScene(*b.state_, *b.reg_, kNoScene);
        if (step.refused) {
            StoryBridgeReport rep;
            rep.kind = StoryBridgeReport::Kind::SceneRefused;
            rep.scene = scene;
            b.report(rep);
        }
        b.lastScene_ = kNone;
        b.restartScene_ = kNone;
        break;
    }
    }
}

void StoryBridge::enter(std::uint32_t scene) {
    const std::uint8_t ord = ordinalOf(scene);
    const std::uint32_t previous = lastScene_;
    lastScene_ = scene;
    if (ord == kNoScene) {
        restartScene_ = kNone;
        return;
    }
    // A second runner scene of the registry scene already open carries on in it.
    if (!(state_->openScene == ord && previous != scene && previous != kNone)) {
        const StorySceneStep step = storyEnterScene(*state_, *reg_, ord);
        if (step.refused) {
            StoryBridgeReport rep;
            rep.kind = StoryBridgeReport::Kind::SceneRefused;
            rep.scene = scene;
            rep.ordinal = ord;
            report(rep);
        }
    }
    markRestartPoint(scene);
    push(ord);
}

void StoryBridge::markRestartPoint(std::uint32_t scene) {
    restartFacts_ = state_->facts;
    restartStamp_ = state_->stamp;
    restartWide_ = state_->wide;
    restartScene_ = scene;
}

// storyRestartScene from the runner scene's own starting point: the state's entry record is swapped for it
// for the call (the retry count and the event still come from the state), then put back, so the registry
// scene's own start is kept for its commit.
bool StoryBridge::restartState(std::uint32_t scene) {
    if (restartScene_ != scene) return storyRestartScene(*state_, *reg_);
    std::swap(state_->factsEntry, restartFacts_);
    std::swap(state_->stampEntry, restartStamp_);
    std::swap(state_->wideEntry, restartWide_);
    const bool ok = storyRestartScene(*state_, *reg_);
    std::swap(state_->factsEntry, restartFacts_);
    std::swap(state_->stampEntry, restartStamp_);
    std::swap(state_->wideEntry, restartWide_);
    return ok;
}

void StoryBridge::push(std::uint8_t ord) {
    for (std::uint32_t i = 0; i < flagFact_.size(); ++i)
        if (flagFact_[i] != 0) pushOne(i, ord);
}

void StoryBridge::pushOne(std::uint32_t flag, std::uint8_t ord) {
    const FactRow& row = reg_->facts[flagFact_[flag]];
    const bool visible =
        ord != kNoScene && (row.readAll || row.reads.has(ord) || row.setAny || row.setScenes.has(ord));
    if (!visible) {
        runner_->setFlag(flag, db_->flag(flag).initial);
        return;
    }
    const FactRead r = storyGet(*state_, *reg_, flagFact_[flag]);
    if (db_->flag(flag).initial.kind == FlagValue::Kind::Bool)
        runner_->setFlag(flag, FlagValue::boolean(r.status == FactStatus::Set));
    else
        runner_->setFlag(flag, FlagValue::integer(r.value));
}

void StoryBridge::onFlag(void* user, StoryRunner& r, std::uint32_t flag) {
    StoryBridge& b = *static_cast<StoryBridge*>(user);
    if (&r != b.runner_ || r.database() != b.db_ || flag >= b.flagFact_.size() || b.flagFact_[flag] == 0 ||
        b.pausedForLoad(r))
        return;
    const FlagValue v = r.flag(flag);
    StoryBridgeReport rep;
    rep.flag = flag;
    rep.fact = b.flagFact_[flag];
    rep.value = v.value;
    rep.scene = r.sceneIndex();
    rep.ordinal = b.state_->openScene;
    const std::int64_t want = v.kind == FlagValue::Kind::Bool ? (v.value != 0 ? 1 : 0) : v.value;
    if (want == 0) {
        // A clear: the state has none (a stored 0 means never set), so it is not sent to storySet.
        if (storyGet(*b.state_, *b.reg_, rep.fact).status == FactStatus::Set) {
            rep.kind = StoryBridgeReport::Kind::ClearIgnored;
            b.report(rep);
        }
        b.pushOne(flag, b.ordinalOf(r.sceneIndex()));
        return;
    }
    if (want < 0 || want > 255) {
        rep.result = SetResult::RejectedRange;
    } else {
        rep.result = storySet(*b.state_, *b.reg_, rep.fact, static_cast<std::uint8_t>(want));
    }
    if (rep.result != SetResult::Ok && rep.result != SetResult::Unchanged) b.report(rep);
    b.pushOne(flag, b.ordinalOf(r.sceneIndex()));
}

// True while the bridge waits for sync() after a restore. A move after restore() returned means sync()
// was missed: reported once, and the bridge stays paused (it cannot know the state was decoded).
bool StoryBridge::pausedForLoad(const StoryRunner& r) {
    if (!restoring_) return false;
    if (!r.restoring() && !syncMissed_) {
        syncMissed_ = true;
        StoryBridgeReport rep;
        rep.kind = StoryBridgeReport::Kind::SyncMissing;
        rep.scene = r.sceneIndex();
        rep.ordinal = ordinalOf(rep.scene);
        report(rep);
    }
    return true;
}

void StoryBridge::sync() {
    if (!runner_ || runner_->database() != db_) return;
    restoring_ = false;
    syncMissed_ = false;
    const std::uint32_t scene = runner_->sceneIndex();
    lastScene_ = scene;
    restartScene_ = kNone;
    if (runner_->finished()) {
        if (state_->openScene != kNoScene) storyEnterScene(*state_, *reg_, kNoScene);
        return;
    }
    const std::uint8_t ord = ordinalOf(scene);
    if (ord == kNoScene) return;
    if (state_->openScene != ord) {
        const StorySceneStep step = storyEnterScene(*state_, *reg_, ord);
        if (step.refused) {
            StoryBridgeReport rep;
            rep.kind = StoryBridgeReport::Kind::SceneRefused;
            rep.scene = scene;
            rep.ordinal = ord;
            report(rep);
        }
    }
    markRestartPoint(scene); // the load point, as the runner's restart point is its restored flags
    push(ord);
}

} // namespace ftcore
