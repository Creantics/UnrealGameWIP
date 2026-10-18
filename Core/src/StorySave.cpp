#include "ftcore/StorySave.h"

#include "ftcore/StoryRunner.h"

namespace ftcore {

bool captureStory(const StoryRunner& runner, StorySave& out) {
    out = StorySave{};
    const StoryDatabase* db = runner.database();
    const StoryPosition pos = runner.position();
    if (!db || pos.scene.empty()) return false;
    out.scene = std::string(pos.scene);
    out.beat = pos.beat;
    for (std::uint32_t i = 0; i < runner.flagCount(); ++i) {
        const FlagValue v = runner.flag(i);
        const FlagDef& def = db->flag(i);
        if (v == def.initial) continue;
        SavedStoryFlag f;
        f.id = std::string(db->view(def.id));
        f.kind = v.kind;
        if (v.kind == FlagValue::Kind::String) f.text = std::string(db->flagString(v));
        else f.number = v.value;
        out.flags.push_back(std::move(f));
    }
    return true;
}

bool applyStory(const StorySave& save, StoryRunner& runner, StoryLoadReport* report) {
    if (report) *report = StoryLoadReport{};
    const StoryDatabase* db = runner.database();
    if (!db) return false;
    runner.resetFlags();
    for (const SavedStoryFlag& f : save.flags) {
        FlagValue v;
        bool ok = true;
        switch (f.kind) {
        case FlagValue::Kind::Bool: v = FlagValue::boolean(f.number != 0); break;
        case FlagValue::Kind::Int: v = FlagValue::integer(f.number); break;
        case FlagValue::Kind::String: ok = db->findFlagString(f.text, v); break; // only values the script uses
        }
        if (!ok || !runner.setFlag(f.id, v)) {
            if (report) report->skippedFlags.push_back(f.id);
        }
    }
    if (!runner.restore({save.scene, save.beat})) {
        runner.stop();
        return false;
    }
    return true;
}

} // namespace ftcore
