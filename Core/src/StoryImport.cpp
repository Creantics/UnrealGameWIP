#include "ftcore/StoryImport.h"

#include "ftcore/Json.h"

#include <algorithm>

namespace ftcore {

namespace {

struct Importer {
    StoryBuilder b;
    std::vector<std::string> errors;
    std::vector<std::string>* warnings = nullptr;

    void error(std::string what) { errors.push_back(std::move(what)); }
    void warn(std::string what) {
        if (warnings) warnings->push_back(std::move(what));
    }

    // Bool, integer or string; false on anything else.
    bool readValue(JsonValue v, FlagValue& out) {
        if (v.isBool()) {
            out = FlagValue::boolean(v.boolean());
            return true;
        }
        if (v.isInteger()) {
            out = FlagValue::integer(v.integer());
            return true;
        }
        if (v.isString()) {
            out = b.stringValue(v.string());
            return true;
        }
        return false;
    }

    // {"op", "id", "value"}: the op defaults to set, the value to false for clear and true otherwise.
    bool readFlagOp(JsonValue j, const std::string& path, FlagOp& op, std::string_view& id, FlagValue& value) {
        const std::string_view name = j["op"].string("set");
        if (name == "set") op = FlagOp::Set;
        else if (name == "clear") op = FlagOp::Clear;
        else if (name == "require") op = FlagOp::Require;
        else if (name == "end") op = FlagOp::End;
        else {
            error(path + ".op: expected one of \"set\", \"clear\", \"require\", \"end\"");
            return false;
        }
        id = j["id"].string();
        if (op != FlagOp::End && id.empty()) {
            error(path + ".id: required");
            return false;
        }
        value = FlagValue::boolean(op != FlagOp::Clear);
        const JsonValue v = j["value"];
        if (!v.isAbsent() && !v.isNull() && !readValue(v, value)) {
            error(path + ".value: expected boolean, integer or string");
            return false;
        }
        return true;
    }

    void choice(JsonValue j, const std::string& path) {
        std::string_view prompt = j["text"].string();
        if (prompt.empty()) prompt = j["label"].string();
        b.choice(prompt, j["scripted"].boolean(), j["loc"].string());
        const JsonValue opts = j["options"];
        if (!opts.isArray()) {
            error(path + ".options: expected an array");
            return;
        }
        for (std::uint32_t k = 0; k < opts.size(); ++k) {
            const JsonValue o = opts[k];
            const std::string opath = path + ".options[" + std::to_string(k) + "]";
            if (!o.isObject()) {
                error(opath + ": expected an object");
                continue;
            }
            std::uint32_t next = kNone;
            const JsonValue n = o["next"];
            if (!n.isAbsent() && !n.isNull()) {
                if (n.isInteger() && n.integer() >= 0 && n.integer() < static_cast<std::int64_t>(kNone))
                    next = static_cast<std::uint32_t>(n.integer());
                else error(opath + ".next: expected a beat index or null");
            }
            const std::string_view text = o["text"].string(), loc = o["loc"].string();
            if (!o["selectable"].boolean(true)) b.greyedOption(text, loc);
            else if (o["miss"].boolean()) b.missOption(text, next, loc);
            else b.option(text, next, loc);
            const JsonValue effects = o["effects"];
            for (std::uint32_t e = 0; e < effects.size() && effects.isArray(); ++e) {
                const JsonValue ej = effects[e];
                const std::string epath = opath + ".effects[" + std::to_string(e) + "]";
                if (!ej.isObject()) {
                    error(epath + ": expected an object");
                    continue;
                }
                FlagOp op = FlagOp::Set;
                std::string_view id;
                FlagValue value;
                if (readFlagOp(ej, epath, op, id, value)) b.effect(op, id, value);
            }
        }
    }

    void beat(JsonValue j, const std::string& path) {
        if (!j.isObject()) {
            error(path + ": expected an object");
            b.unknown();
            return;
        }
        const std::string_view type = j["type"].string();
        if (type == "line") {
            b.line(j["id"].string(), j["speaker"].string(), j["text"].string(), j["direction"].string(),
                   j["loc"].string());
        } else if (type == "direction") {
            if (j["hidden"].boolean()) b.hiddenDirection(j["text"].string(), j["note"].string());
            else b.direction(j["text"].string(), j["loc"].string());
        } else if (type == "choice") {
            choice(j, path);
        } else if (type == "gameplay") {
            b.gameplay(j["kind"].string(), j["text"].string(), j["loc"].string());
            const JsonValue params = j["params"];
            for (std::uint32_t i = 0; i < params.size() && params.isObject(); ++i) {
                const JsonValue v = params.value(i);
                // Strings as they are, null as "", anything else as written.
                b.param(params.key(i), v.isString() ? v.string() : v.isNull() ? std::string_view{} : v.raw());
            }
        } else if (type == "cue") {
            const std::string_view action = j["action"].string("play");
            CueAction a = CueAction::Play;
            if (action == "stop") a = CueAction::Stop;
            else if (action == "stinger") a = CueAction::Stinger;
            else if (action != "play") warn(path + ".action: unknown cue action '" + std::string(action) + "' (played)");
            const std::string_view id = j["id"].string();
            if (id.empty()) {
                error(path + ".id: required");
                b.unknown();
                return;
            }
            b.cue(id, a);
        } else if (type == "flag") {
            FlagOp op = FlagOp::Set;
            std::string_view id;
            FlagValue value;
            if (readFlagOp(j, path, op, id, value)) b.flag(op, id, value);
            else b.unknown();
        } else if (type == "hold") {
            b.hold(static_cast<float>(j["seconds"].number()), j["note"].string());
        } else if (type == "end") {
            b.end(j["next"].string());
        } else {
            warn(path + ".type: unknown beat type '" + std::string(type) + "' (skipped)");
            b.unknown();
        }
    }

    void scene(JsonValue doc, std::string_view id) {
        const std::string sid(id);
        SceneInfo info;
        info.world = doc["world"].string();
        info.location = doc["location"].string();
        info.time = doc["time"].string();
        info.mode = doc["mode"].string();
        info.gameplay = doc["gameplay"].string();
        b.beginScene(id, doc["title"].string(), info);
        const JsonValue beats = doc["beats"];
        if (!beats.isArray()) {
            error(sid + ": beats: expected an array");
            return;
        }
        for (std::uint32_t i = 0; i < beats.size(); ++i) beat(beats[i], sid + ": beats[" + std::to_string(i) + "]");
    }
};

std::string joinLines(const std::vector<std::string>& lines) {
    std::string s;
    for (const std::string& l : lines) {
        if (!s.empty()) s += '\n';
        s += l;
    }
    return s;
}

} // namespace

bool storySceneIds(std::string_view storyJson, std::vector<std::string>& out, std::string* error) {
    out.clear();
    JsonDocument doc;
    std::string err;
    if (!doc.parse(storyJson, &err)) {
        if (error) *error = "story.json: " + err;
        return false;
    }
    const JsonValue chapters = doc.root()["chapters"];
    for (std::uint32_t c = 0; c < chapters.size(); ++c) {
        const JsonValue scenes = chapters[c]["scenes"];
        for (std::uint32_t s = 0; s < scenes.size(); ++s)
            if (scenes[s].isString()) out.emplace_back(scenes[s].string());
    }
    return true;
}

bool importStory(std::string_view storyJson, std::span<const std::string_view> sceneJsons, StoryDatabase& out,
                 std::string* error, std::vector<std::string>* warnings, bool keepEmphasis) {
    out = StoryDatabase{};
    Importer im;
    im.warnings = warnings;
    im.b.keepEmphasis(keepEmphasis);
    auto failed = [&] {
        if (error) *error = joinLines(im.errors);
        return false;
    };

    JsonDocument story;
    std::string err;
    if (!story.parse(storyJson, &err)) {
        im.error("story.json: " + err);
        return failed();
    }
    const JsonValue root = story.root();
    if (!root.isObject()) {
        im.error("story.json: expected an object");
        return failed();
    }
    if (root["format"].integer(1) != 1) im.warn("story.json: format: unknown format version (expected 1); reading anyway");
    im.b.setTitle(root["title"].string());
    im.b.setStart(root["start"].string());

    const JsonValue flags = root["flags"];
    for (std::uint32_t i = 0; i < flags.size() && flags.isArray(); ++i) {
        const JsonValue f = flags[i];
        const std::string path = "story.json: flags[" + std::to_string(i) + "]";
        const std::string_view id = f["id"].string();
        if (id.empty()) {
            im.error(path + ".id: required");
            continue;
        }
        FlagValue initial = FlagValue::boolean(false);
        const JsonValue d = f["default"];
        if (!d.isAbsent() && !d.isNull() && !im.readValue(d, initial))
            im.error(path + ".default: expected boolean, integer or string");
        im.b.addFlag(id, initial);
    }
    const JsonValue cues = root["cues"];
    for (std::uint32_t i = 0; i < cues.size() && cues.isArray(); ++i) {
        const std::string_view id = cues[i]["id"].string();
        if (id.empty()) im.error("story.json: cues[" + std::to_string(i) + "].id: required");
        else im.b.addCue(id, cues[i]["kind"].string());
    }

    // The scene list, chapter by chapter.
    const JsonValue chapters = root["chapters"];
    if (!chapters.isArray()) {
        im.error("story.json: chapters: expected an array");
        return failed();
    }
    std::vector<std::string_view> ids;
    for (std::uint32_t c = 0; c < chapters.size(); ++c) {
        const JsonValue scenes = chapters[c]["scenes"];
        for (std::uint32_t s = 0; s < scenes.size(); ++s) {
            const std::string_view id = scenes[s].string();
            if (id.empty()) continue;
            if (std::find(ids.begin(), ids.end(), id) != ids.end())
                im.error("story.json: chapters[" + std::to_string(c) + "].scenes: scene '" + std::string(id) +
                         "' is listed twice");
            else ids.push_back(id);
        }
    }
    if (ids.empty()) im.error("story.json: chapters: no scenes");

    // Scene documents, matched by id.
    std::vector<JsonDocument> docs(sceneJsons.size());
    std::vector<std::uint32_t> docOf(ids.size(), kNone);
    for (std::size_t i = 0; i < sceneJsons.size(); ++i) {
        const std::string label = "scenes[" + std::to_string(i) + "]";
        if (!docs[i].parse(sceneJsons[i], &err)) {
            im.error(label + ": " + err);
            continue;
        }
        const std::string_view id = docs[i].root()["id"].string();
        const auto it = std::find(ids.begin(), ids.end(), id);
        if (it == ids.end()) {
            im.warn(label + ": scene '" + std::string(id) + "' is not in any chapter (ignored)");
            continue;
        }
        std::uint32_t& slot = docOf[static_cast<std::size_t>(it - ids.begin())];
        if (slot != kNone) {
            im.error(label + ": scene '" + std::string(id) + "' given twice");
            continue;
        }
        slot = static_cast<std::uint32_t>(i);
    }
    for (std::size_t i = 0; i < ids.size(); ++i)
        if (docOf[i] == kNone) im.error("story.json: scene '" + std::string(ids[i]) + "' has no scene document");
    if (!im.errors.empty()) return failed();

    std::size_t next = 0;
    for (std::uint32_t c = 0; c < chapters.size(); ++c) {
        const JsonValue ch = chapters[c];
        const std::string_view chapterId = ch["id"].string();
        if (chapterId.empty()) im.error("story.json: chapters[" + std::to_string(c) + "].id: required");
        im.b.beginChapter(chapterId, ch["title"].string());
        const JsonValue scenes = ch["scenes"];
        for (std::uint32_t s = 0; s < scenes.size(); ++s) {
            const std::string_view id = scenes[s].string();
            // Same walk as above, so the k-th accepted id is ids[k].
            if (id.empty() || next >= ids.size() || ids[next] != id) continue;
            im.scene(docs[docOf[next]].root(), id);
            ++next;
        }
    }

    std::string buildError;
    const bool built = im.b.finish(out, &buildError);
    if (warnings)
        for (const std::string& w : im.b.warnings()) warnings->push_back(w);
    if (!built) im.error(buildError);
    if (!im.errors.empty()) {
        out = StoryDatabase{};
        return failed();
    }
    return true;
}

} // namespace ftcore
