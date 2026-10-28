#include "ftcore/StorySave.h"

#include "ftcore/Json.h"
#include "ftcore/StoryRunner.h"

#include <cstdio>

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

namespace {

constexpr std::int64_t kSaveFormat = 2;    // what encodeStorySave writes
constexpr std::int64_t kOldestFormat = 1;  // the oldest decodeStorySave still reads

bool isHexDigit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

void appendQuoted(std::string& out, const std::string& s) {
    out += '"';
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else {
                out += ch;
            }
        }
    }
    out += '"';
}

const char* kindName(FlagValue::Kind k) {
    return k == FlagValue::Kind::Int ? "int" : k == FlagValue::Kind::String ? "string" : "bool";
}

bool fail(std::string* error, std::string msg) {
    if (error) *error = std::move(msg);
    return false;
}

} // namespace

std::string encodeStorySave(const StorySave& save) {
    std::string out = "{\"format\":" + std::to_string(kSaveFormat) + ",\"scene\":";
    appendQuoted(out, save.scene);
    out += ",\"beat\":" + std::to_string(save.beat) + ",\"flags\":[";
    for (std::size_t i = 0; i < save.flags.size(); ++i) {
        const SavedStoryFlag& f = save.flags[i];
        if (i) out += ',';
        out += "{\"id\":";
        appendQuoted(out, f.id);
        out += ",\"kind\":\"";
        out += kindName(f.kind);
        out += "\",\"number\":" + std::to_string(f.number) + ",\"text\":";
        appendQuoted(out, f.text);
        out += '}';
    }
    out += "],\"seen\":[";
    for (std::size_t i = 0; i < save.seen.size(); ++i) {
        if (i) out += ',';
        out += "{\"scene\":";
        appendQuoted(out, save.seen[i].scene);
        out += ",\"bits\":";
        appendQuoted(out, save.seen[i].bits);
        out += '}';
    }
    out += "],\"party\":{\"xp\":" + std::to_string(save.party.xp) + ",\"gold\":" + std::to_string(save.party.gold) +
           ",\"items\":[";
    for (std::size_t i = 0; i < save.party.items.size(); ++i) {
        const ItemStack& stack = save.party.items[i];
        if (i) out += ',';
        out += "{\"id\":";
        appendQuoted(out, std::string(stack.id.view()));
        out += ",\"count\":" + std::to_string(stack.count) + '}';
    }
    out += "]}}";
    return out;
}

bool decodeStorySave(std::string_view text, StorySave& out, std::string* error) {
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(text, &parseError)) return fail(error, "save is not valid JSON: " + parseError);
    const JsonValue root = doc.root();
    if (!root.isObject()) return fail(error, "save: the top level must be an object");
    const JsonValue format = root["format"];
    if (!format.isInteger()) return fail(error, "save: \"format\" is missing or not an integer");
    if (format.integer() < kOldestFormat || format.integer() > kSaveFormat)
        return fail(error, "save: unsupported format " + std::to_string(format.integer()));
    const bool hasSeen = format.integer() >= 2;
    const JsonValue scene = root["scene"], beat = root["beat"], flags = root["flags"];
    if (!scene.isString()) return fail(error, "save: \"scene\" is missing or not a string");
    if (!beat.isInteger() || beat.integer() < 0 || beat.integer() > 0xFFFFFFFFll)
        return fail(error, "save: \"beat\" is missing or not an integer in range");
    if (!flags.isArray()) return fail(error, "save: \"flags\" is missing or not an array");

    StorySave result;
    result.scene = std::string(scene.string());
    result.beat = static_cast<std::uint32_t>(beat.integer());
    result.flags.reserve(flags.size());
    for (std::uint32_t i = 0; i < flags.size(); ++i) {
        const JsonValue f = flags[i];
        const std::string at = "save: flags[" + std::to_string(i) + "]";
        if (!f.isObject()) return fail(error, at + " is not an object");
        const JsonValue id = f["id"], kind = f["kind"], number = f["number"], str = f["text"];
        if (!id.isString()) return fail(error, at + ".id is missing or not a string");
        if (!kind.isString()) return fail(error, at + ".kind is missing or not a string");
        if (!number.isInteger()) return fail(error, at + ".number is missing or not an integer");
        if (!str.isString()) return fail(error, at + ".text is missing or not a string");
        SavedStoryFlag sf;
        sf.id = std::string(id.string());
        const std::string_view k = kind.string();
        if (k == "bool") sf.kind = FlagValue::Kind::Bool;
        else if (k == "int") sf.kind = FlagValue::Kind::Int;
        else if (k == "string") sf.kind = FlagValue::Kind::String;
        else return fail(error, at + ".kind is unknown: \"" + std::string(k) + "\"");
        sf.number = number.integer();
        sf.text = std::string(str.string());
        result.flags.push_back(std::move(sf));
    }
    if (hasSeen) {
        const JsonValue seen = root["seen"];
        if (!seen.isArray()) return fail(error, "save: \"seen\" is missing or not an array");
        result.seen.reserve(seen.size());
        for (std::uint32_t i = 0; i < seen.size(); ++i) {
            const JsonValue s = seen[i];
            const std::string at = "save: seen[" + std::to_string(i) + "]";
            if (!s.isObject()) return fail(error, at + " is not an object");
            const JsonValue id = s["scene"], bits = s["bits"];
            if (!id.isString()) return fail(error, at + ".scene is missing or not a string");
            if (!bits.isString()) return fail(error, at + ".bits is missing or not a string");
            for (const char c : bits.string())
                if (!isHexDigit(c)) return fail(error, at + ".bits is not hexadecimal");
            result.seen.push_back({std::string(id.string()), std::string(bits.string())});
        }
    }
    const JsonValue party = root["party"];
    if (!party.isAbsent()) {
        if (!party.isObject()) return fail(error, "save: \"party\" is not an object");
        const JsonValue xp = party["xp"], gold = party["gold"];
        if (!xp.isInteger() || xp.integer() < 0 || xp.integer() > kMaxXp)
            return fail(error, "save: party.xp is missing or not an integer in range");
        if (!gold.isInteger() || gold.integer() < 0 || gold.integer() > kMaxGold)
            return fail(error, "save: party.gold is missing or not an integer in range");
        result.party.xp = static_cast<int>(xp.integer());
        result.party.gold = static_cast<int>(gold.integer());
        // Optional (a save from before items has none). Ids are not looked up in any item table.
        const JsonValue items = party["items"];
        if (!items.isAbsent()) {
            if (!items.isArray()) return fail(error, "save: party.items is not an array");
            for (std::uint32_t i = 0; i < items.size(); ++i) {
                const JsonValue entry = items[i];
                const std::string at = "save: party.items[" + std::to_string(i) + "]";
                if (!entry.isObject()) return fail(error, at + " is not an object");
                const JsonValue id = entry["id"], count = entry["count"];
                if (!id.isString() || !validItemId(id.string()))
                    return fail(error, at + ".id is missing or not a valid item id");
                if (!count.isInteger() || count.integer() < 1 || count.integer() > kMaxItemStack)
                    return fail(error, at + ".count is missing or not an integer in 1.." + std::to_string(kMaxItemStack));
                result.party.items.add(id.string(), static_cast<int>(count.integer())); // what does not fit is dropped
            }
        }
    }
    out = std::move(result);
    return true;
}

const char* saveRefusalName(SaveRefusal reason) {
    switch (reason) {
    case SaveRefusal::None: return "ok";
    case SaveRefusal::Empty: return "empty";
    case SaveRefusal::Invalid: return "invalid";
    case SaveRefusal::NoStory: return "no_story";
    case SaveRefusal::SceneMissing: return "scene_missing";
    case SaveRefusal::BeatOutOfRange: return "beat_out_of_range";
    }
    return "invalid";
}

SaveRefusal checkStorySave(std::string_view text, const StoryDatabase* db, StorySave* out) {
    if (text.empty()) return SaveRefusal::Empty;
    StorySave save;
    if (!decodeStorySave(text, save)) return SaveRefusal::Invalid;
    if (!db || !db->loaded()) return SaveRefusal::NoStory;
    const std::uint32_t s = db->findScene(save.scene);
    if (s == kNone) return SaveRefusal::SceneMissing;
    if (save.beat > db->scene(s).beatCount) return SaveRefusal::BeatOutOfRange;
    if (out) *out = std::move(save);
    return SaveRefusal::None;
}

} // namespace ftcore
