#include "ftcore/StoryDatabase.h"

#include <algorithm>
#include <charconv>
#include <system_error>

namespace ftcore {

std::string_view beatTypeName(BeatType t) {
    switch (t) {
    case BeatType::Line: return "line";
    case BeatType::Direction: return "direction";
    case BeatType::Choice: return "choice";
    case BeatType::Gameplay: return "gameplay";
    case BeatType::Cue: return "cue";
    case BeatType::Flag: return "flag";
    case BeatType::Hold: return "hold";
    case BeatType::End: return "end";
    case BeatType::Unknown: break;
    }
    return "unknown";
}

std::string_view gameplayKindName(GameplayKind k) {
    switch (k) {
    case GameplayKind::Combat: return "CMB";
    case GameplayKind::Engage: return "ENG";
    case GameplayKind::Play: return "PLAY";
    case GameplayKind::Mini: return "MINI";
    case GameplayKind::Other: break;
    }
    return "OTHER";
}

GameplayKind gameplayKindFromName(std::string_view name) {
    if (name == "CMB") return GameplayKind::Combat;
    if (name == "ENG") return GameplayKind::Engage;
    if (name == "PLAY") return GameplayKind::Play;
    if (name == "MINI") return GameplayKind::Mini;
    return GameplayKind::Other;
}

// ---- database ---------------------------------------------------------------------------------

std::uint32_t StoryDatabase::findScene(std::string_view id) const {
    const auto it = std::lower_bound(sceneById_.begin(), sceneById_.end(), id,
                                     [&](std::uint32_t s, std::string_view key) { return view(scenes_[s].id) < key; });
    return it != sceneById_.end() && view(scenes_[*it].id) == id ? *it : kNone;
}

std::uint32_t StoryDatabase::findFlag(std::string_view id) const {
    const auto it = std::lower_bound(flagById_.begin(), flagById_.end(), id,
                                     [&](std::uint32_t f, std::string_view key) { return view(flags_[f].id) < key; });
    return it != flagById_.end() && view(flags_[*it].id) == id ? *it : kNone;
}

std::string_view StoryDatabase::flagString(FlagValue v) const {
    if (v.kind != FlagValue::Kind::String || v.value < 0 || static_cast<std::size_t>(v.value) >= flagStrings_.size())
        return {};
    return view(flagStrings_[static_cast<std::size_t>(v.value)]);
}

std::string StoryDatabase::formatFlagValue(FlagValue v) const {
    switch (v.kind) {
    case FlagValue::Kind::Bool: return v.value ? "true" : "false";
    case FlagValue::Kind::Int: return std::to_string(v.value);
    case FlagValue::Kind::String: break;
    }
    return std::string(flagString(v));
}

bool StoryDatabase::parseFlagValue(std::string_view text, FlagValue& out) const {
    if (text == "true" || text == "false") {
        out = FlagValue::boolean(text == "true");
        return true;
    }
    if (!text.empty() && (text[0] == '-' || (text[0] >= '0' && text[0] <= '9'))) {
        std::int64_t v = 0;
        const auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
        if (ec == std::errc{} && p == text.data() + text.size()) {
            out = FlagValue::integer(v);
            return true;
        }
    }
    return findFlagString(text, out);
}

bool StoryDatabase::findFlagString(std::string_view text, FlagValue& out) const {
    for (std::size_t i = 0; i < flagStrings_.size(); ++i)
        if (view(flagStrings_[i]) == text) {
            out = {static_cast<std::int64_t>(i), FlagValue::Kind::String};
            return true;
        }
    return false;
}

std::string_view StoryDatabase::param(const Beat& b, std::string_view key) const {
    for (std::uint32_t i = 0; i < b.paramCount; ++i) {
        const auto& kv = params_[b.firstParam + i];
        if (view(kv.first) == key) return view(kv.second);
    }
    return {};
}

// ---- builder ----------------------------------------------------------------------------------

Str StoryBuilder::store(std::string_view s) {
    Str r{static_cast<std::uint32_t>(db_.data_.size()), static_cast<std::uint32_t>(s.size())};
    db_.data_.append(s);
    return r;
}

Str StoryBuilder::storeText(std::string_view s) {
    const auto offset = static_cast<std::uint32_t>(db_.data_.size());
    for (char c : s)
        if (keepEmphasis_ || c != '*') db_.data_ += c;
    Str r{offset, static_cast<std::uint32_t>(db_.data_.size()) - offset};
    db_.longestText_ = std::max<std::size_t>(db_.longestText_, r.length);
    return r;
}

std::uint32_t StoryBuilder::internFlag(std::string_view id) {
    for (std::uint32_t i = 0; i < db_.flags_.size(); ++i)
        if (db_.view(db_.flags_[i].id) == id) return i;
    FlagDef f;
    f.id = store(id);
    db_.flags_.push_back(f);
    return static_cast<std::uint32_t>(db_.flags_.size() - 1);
}

std::uint32_t StoryBuilder::internCue(std::string_view id, std::string_view kind) {
    for (std::uint32_t i = 0; i < db_.cues_.size(); ++i)
        if (db_.view(db_.cues_[i].id) == id) {
            if (db_.cues_[i].kind.empty() && !kind.empty()) db_.cues_[i].kind = store(kind);
            return i;
        }
    CueDef c;
    c.id = store(id);
    c.kind = store(kind);
    db_.cues_.push_back(c);
    return static_cast<std::uint32_t>(db_.cues_.size() - 1);
}

void StoryBuilder::fail(std::string_view what) {
    if (error_.empty()) error_ = std::string(what);
}

void StoryBuilder::setTitle(std::string_view title) { db_.title_ = storeText(title); }

void StoryBuilder::setStart(std::string_view sceneId) { start_ = std::string(sceneId); }

void StoryBuilder::addFlag(std::string_view id, FlagValue initial) {
    if (id.empty()) return fail("flag without an id");
    db_.flags_[internFlag(id)].initial = initial;
}

FlagValue StoryBuilder::stringValue(std::string_view s) {
    for (std::size_t i = 0; i < db_.flagStrings_.size(); ++i)
        if (db_.view(db_.flagStrings_[i]) == s) return {static_cast<std::int64_t>(i), FlagValue::Kind::String};
    db_.flagStrings_.push_back(store(s));
    return {static_cast<std::int64_t>(db_.flagStrings_.size() - 1), FlagValue::Kind::String};
}

void StoryBuilder::addCue(std::string_view id, std::string_view kind) {
    if (id.empty()) return fail("cue without an id");
    internCue(id, kind);
}

void StoryBuilder::beginChapter(std::string_view id, std::string_view title) {
    Chapter ch;
    ch.id = store(id);
    ch.title = storeText(title);
    ch.firstScene = static_cast<std::uint32_t>(db_.scenes_.size());
    db_.chapters_.push_back(ch);
}

void StoryBuilder::beginScene(std::string_view id, std::string_view title, const SceneInfo& info) {
    if (db_.chapters_.empty()) return fail("scene '" + std::string(id) + "' outside a chapter");
    if (id.empty()) return fail("scene without an id");
    for (const Scene& s : db_.scenes_)
        if (db_.view(s.id) == id) return fail("scene '" + std::string(id) + "' is listed twice");
    Chapter& ch = db_.chapters_.back();
    Scene sc;
    sc.id = store(id);
    sc.title = storeText(title);
    sc.chapter = ch.id;
    sc.chapterIndex = static_cast<std::uint32_t>(db_.chapters_.size() - 1);
    sc.world = store(info.world);
    sc.location = store(info.location);
    sc.time = store(info.time);
    sc.mode = store(info.mode);
    sc.gameplay = store(info.gameplay);
    sc.firstBeat = static_cast<std::uint32_t>(db_.beats_.size());
    db_.scenes_.push_back(sc);
    ++ch.sceneCount;
}

Beat* StoryBuilder::addBeat(BeatType type) {
    if (db_.scenes_.empty()) {
        fail(std::string("a ") + std::string(beatTypeName(type)) + " beat outside a scene");
        return nullptr;
    }
    Beat b;
    b.type = type;
    db_.beats_.push_back(b);
    ++db_.scenes_.back().beatCount;
    return &db_.beats_.back();
}

void StoryBuilder::line(std::string_view id, std::string_view speaker, std::string_view text,
                        std::string_view direction, std::string_view loc) {
    Beat* b = addBeat(BeatType::Line);
    if (!b) return;
    b->id = store(id);
    b->speaker = store(speaker);
    b->direction = store(direction);
    b->text = storeText(text);
    b->loc = store(loc);
}

void StoryBuilder::direction(std::string_view text, std::string_view loc) {
    Beat* b = addBeat(BeatType::Direction);
    if (!b) return;
    b->text = storeText(text);
    b->loc = store(loc);
}

void StoryBuilder::hiddenDirection(std::string_view text, std::string_view note) {
    Beat* b = addBeat(BeatType::Direction);
    if (!b) return;
    b->hidden = true;
    b->text = store(text); // kept for tools; never displayed and has no loc key
    b->note = store(note);
}

void StoryBuilder::cue(std::string_view cueId, CueAction action) {
    if (cueId.empty()) return fail("cue beat without a cue id");
    Beat* b = addBeat(BeatType::Cue);
    if (!b) return;
    b->cue = internCue(cueId, {});
    b->cueAction = action;
}

void StoryBuilder::hold(float seconds, std::string_view note) {
    Beat* b = addBeat(BeatType::Hold);
    if (!b) return;
    b->seconds = std::max(0.f, seconds);
    b->note = store(note);
}

void StoryBuilder::choice(std::string_view prompt, bool scripted, std::string_view loc) {
    Beat* b = addBeat(BeatType::Choice);
    if (!b) return;
    b->text = storeText(prompt);
    b->loc = store(loc);
    b->scripted = scripted;
    b->firstOption = static_cast<std::uint32_t>(db_.options_.size());
}

Option* StoryBuilder::addOption(std::string_view text, std::uint32_t next, std::string_view loc) {
    if (db_.scenes_.empty() || db_.scenes_.back().beatCount == 0 || db_.beats_.back().type != BeatType::Choice) {
        fail("option '" + std::string(text) + "' does not follow a choice");
        return nullptr;
    }
    Option o;
    o.text = storeText(text);
    o.loc = store(loc);
    o.next = next;
    o.firstEffect = static_cast<std::uint32_t>(db_.effects_.size());
    db_.options_.push_back(o);
    ++db_.beats_.back().optionCount;
    return &db_.options_.back();
}

void StoryBuilder::option(std::string_view text, std::uint32_t next, std::string_view loc) {
    addOption(text, next, loc);
}

void StoryBuilder::greyedOption(std::string_view text, std::string_view loc) {
    if (Option* o = addOption(text, kNone, loc)) o->selectable = false;
}

void StoryBuilder::missOption(std::string_view text, std::uint32_t next, std::string_view loc) {
    if (Option* o = addOption(text, next, loc)) o->miss = true;
}

void StoryBuilder::effect(FlagOp op, std::string_view flagId, FlagValue value) {
    if (op != FlagOp::Set && op != FlagOp::Clear) return fail("an option effect must set or clear a flag");
    if (flagId.empty()) return fail("option effect without a flag id");
    const bool onOption = !db_.scenes_.empty() && db_.scenes_.back().beatCount > 0 &&
                          db_.beats_.back().type == BeatType::Choice && db_.beats_.back().optionCount > 0;
    if (!onOption) return fail("effect on '" + std::string(flagId) + "' does not follow an option");
    FlagOpDef e;
    e.op = op;
    e.flag = internFlag(flagId);
    e.value = value;
    db_.effects_.push_back(e);
    ++db_.options_.back().effectCount;
}

void StoryBuilder::gameplay(std::string_view kindName, std::string_view text, std::string_view loc) {
    Beat* b = addBeat(BeatType::Gameplay);
    if (!b) return;
    b->kindName = store(kindName);
    b->kind = gameplayKindFromName(kindName);
    b->text = storeText(text);
    b->loc = store(loc);
    b->firstParam = static_cast<std::uint32_t>(db_.params_.size());
}

void StoryBuilder::param(std::string_view key, std::string_view value) {
    if (db_.scenes_.empty() || db_.scenes_.back().beatCount == 0 || db_.beats_.back().type != BeatType::Gameplay)
        return fail("param '" + std::string(key) + "' does not follow a gameplay beat");
    db_.params_.emplace_back(store(key), store(value));
    ++db_.beats_.back().paramCount;
}

void StoryBuilder::flag(FlagOp op, std::string_view flagId, FlagValue value) {
    if (op != FlagOp::End && flagId.empty()) return fail("flag beat without a flag id");
    Beat* b = addBeat(BeatType::Flag);
    if (!b) return;
    b->flagOp.op = op;
    b->flagOp.flag = flagId.empty() ? kNone : internFlag(flagId);
    b->flagOp.value = value;
}

void StoryBuilder::end(std::string_view nextScene) {
    Beat* b = addBeat(BeatType::End);
    if (!b || nextScene.empty()) return;
    endTargets_.emplace_back(static_cast<std::uint32_t>(db_.beats_.size() - 1), store(nextScene));
}

void StoryBuilder::unknown() { addBeat(BeatType::Unknown); }

void StoryBuilder::reset() {
    db_ = StoryDatabase{};
    endTargets_.clear();
    start_.clear();
    error_.clear();
    warnings_.clear();
}

bool StoryBuilder::finish(StoryDatabase& out, std::string* error) {
    StoryDatabase& db = db_;
    if (error_.empty() && db.scenes_.empty()) fail("no scenes");

    if (error_.empty()) {
        const auto sceneCount = static_cast<std::uint32_t>(db.scenes_.size());
        db.sceneById_.resize(sceneCount);
        for (std::uint32_t i = 0; i < sceneCount; ++i) db.sceneById_[i] = i;
        std::sort(db.sceneById_.begin(), db.sceneById_.end(),
                  [&](std::uint32_t a, std::uint32_t b) { return db.view(db.scenes_[a].id) < db.view(db.scenes_[b].id); });
        db.flagById_.resize(db.flags_.size());
        for (std::uint32_t i = 0; i < db.flags_.size(); ++i) db.flagById_[i] = i;
        std::sort(db.flagById_.begin(), db.flagById_.end(),
                  [&](std::uint32_t a, std::uint32_t b) { return db.view(db.flags_[a].id) < db.view(db.flags_[b].id); });

        for (std::uint32_t i = 0; i < sceneCount; ++i) db.scenes_[i].orderNext = i + 1 < sceneCount ? i + 1 : kNone;

        db.start_ = start_.empty() ? 0 : db.findScene(start_);
        if (db.start_ == kNone) fail("unknown start scene '" + start_ + "'");

        for (const auto& [beat, target] : endTargets_) {
            const std::string_view id = db.view(target);
            db.beats_[beat].nextScene = db.findScene(id);
            if (db.beats_[beat].nextScene == kNone)
                warn("end: unknown scene '" + std::string(id) + "' (chapter order used)");
        }

        // Choice targets must stay inside the scene.
        for (const Scene& sc : db.scenes_)
            for (std::uint32_t i = 0; i < sc.beatCount; ++i) {
                const Beat& b = db.beats_[sc.firstBeat + i];
                if (b.type != BeatType::Choice) continue;
                if (b.optionCount == 0)
                    warn("scene '" + std::string(db.view(sc.id)) + "' beat " + std::to_string(i) +
                         ": choice without options (passes through)");
                for (std::uint32_t k = 0; k < b.optionCount; ++k) {
                    Option& o = db.options_[b.firstOption + k];
                    if (o.next == kNone || o.next < sc.beatCount) continue;
                    warn("scene '" + std::string(db.view(sc.id)) + "' beat " + std::to_string(i) + ": option next " +
                         std::to_string(o.next) + " is past the scene's end");
                    o.next = sc.beatCount; // ends the scene
                }
            }
    }

    const bool ok = error_.empty();
    if (ok) {
        db.data_.shrink_to_fit();
        out = std::move(db);
    } else {
        out = StoryDatabase{};
        if (error) *error = error_;
    }
    std::vector<std::string> warnings = std::move(warnings_);
    reset();
    warnings_ = std::move(warnings);
    return ok;
}

} // namespace ftcore
