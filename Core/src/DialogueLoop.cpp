#include "ftcore/DialogueLoop.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <initializer_list>

namespace ftcore {

// ---- definition ----

std::uint32_t DialogueLoopRound::wrongCount() const {
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < optionCount && i < kMaxOptions; ++i)
        if (!options[i].right) ++n;
    return n;
}

std::uint32_t DialogueLoopRound::worstCasePicks() const {
    const std::uint32_t wrong = wrongCount();
    return wrong == 0 ? 1u : std::min(wrong + 1u, 3u);
}

std::uint32_t DialogueLoopDef::worstCasePicks() const {
    std::uint32_t n = 0;
    for (std::uint32_t r = 0; r < roundCount && r < kMaxRounds; ++r) n += rounds[r].worstCasePicks();
    return n;
}

std::string dialogueLoopFileName(std::string_view scene, std::uint32_t beat, std::uint32_t step) {
    std::string name(scene);
    name += '_';
    name += std::to_string(beat);
    name += '_';
    name += std::to_string(step);
    name += ".json";
    return name;
}

// ---- loader ----

namespace {

struct LoopLoad {
    std::vector<std::string> errors;
    std::vector<std::string>* warnings = nullptr;

    void error(const std::string& path, std::string_view what) { errors.push_back(path + ": " + std::string(what)); }
    void warn(const std::string& path, std::string_view what) {
        if (warnings) warnings->push_back(path + ": " + std::string(what));
    }
    // Key names only: keys are format, not story text.
    void unknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys) {
        for (std::uint32_t k = 0; k < j.size(); ++k) {
            const std::string_view key = j.key(k);
            if (std::find(keys.begin(), keys.end(), key) == keys.end()) warn(path, "unknown key: " + std::string(key));
        }
    }
};

// A line: a string (spoken by `defaultSpeaker`) or { "speaker": id, "text": text }.
bool loopLine(JsonValue j, std::string_view defaultSpeaker, const std::string& path, DialogueLoopLine& out, LoopLoad& L) {
    if (j.isString()) {
        if (j.string().empty()) {
            L.error(path, "empty line");
            return false;
        }
        out.speaker = std::string(defaultSpeaker);
        out.text = std::string(j.string());
        return true;
    }
    if (j.isObject()) {
        L.unknownKeys(j, path, {"speaker", "text"});
        const JsonValue speaker = j["speaker"];
        const JsonValue text = j["text"];
        bool ok = true;
        if (!speaker.isAbsent() && (!speaker.isString() || speaker.string().empty())) {
            L.error(path + ".speaker", "expected a non-empty string");
            ok = false;
        }
        if (!text.isString() || text.string().empty()) {
            L.error(path + ".text", "expected a non-empty string");
            ok = false;
        }
        if (!ok) return false;
        out.speaker = std::string(speaker.isAbsent() ? defaultSpeaker : speaker.string());
        out.text = std::string(text.string());
        return true;
    }
    L.error(path, "expected a line (a string or an object with speaker and text)");
    return false;
}

bool loopIndex(JsonValue root, const char* key, std::uint32_t& out, LoopLoad& L) {
    const JsonValue v = root[key];
    if (!v.isInteger() || v.integer() < 0 || v.integer() > 100000) {
        L.error(key, "expected a non-negative integer");
        return false;
    }
    out = static_cast<std::uint32_t>(v.integer());
    return true;
}

void loopRound(JsonValue j, const std::string& path, const DialogueLoopDef& def, std::string_view host,
               std::string_view companion, DialogueLoopRound& out, LoopLoad& L) {
    if (!j.isObject()) {
        L.error(path, "expected an object");
        return;
    }
    L.unknownKeys(j, path, {"claim", "options", "concede", "companion"});
    if (j["claim"].isAbsent())
        L.error(path + ".claim", "missing");
    else
        loopLine(j["claim"], host, path + ".claim", out.claim, L);

    const JsonValue options = j["options"];
    if (!options.isArray() || options.size() < 2 || options.size() > DialogueLoopRound::kMaxOptions) {
        L.error(path + ".options", "expected 2 to 4 options");
        return;
    }
    out.optionCount = options.size();
    std::uint32_t right = 0;
    for (std::uint32_t i = 0; i < options.size(); ++i) {
        const std::string opath = path + ".options[" + std::to_string(i) + "]";
        const JsonValue o = options[i];
        DialogueLoopOption& opt = out.options[i];
        if (!o.isObject()) {
            L.error(opath, "expected an object");
            continue;
        }
        L.unknownKeys(o, opath, {"text", "right", "reply"});
        if (!o["text"].isString() || o["text"].string().empty())
            L.error(opath + ".text", "expected a non-empty string");
        else
            opt.text = std::string(o["text"].string());
        if (!o["right"].isBool()) {
            L.error(opath + ".right", "expected true or false");
            continue;
        }
        opt.right = o["right"].boolean();
        if (opt.right) ++right;
        if (!o["reply"].isAbsent())
            loopLine(o["reply"], host, opath + ".reply", opt.reply, L);
        else if (!opt.right)
            L.error(opath + ".reply", "a wrong option needs the host's correction");
    }
    if (right == 0) L.error(path + ".options", "no right option");

    const std::uint32_t wrong = out.wrongCount();
    if (def.resolve) {
        if (!j["concede"].isAbsent())
            loopLine(j["concede"], host, path + ".concede", out.concede, L);
        else if (wrong >= 2)
            L.error(path + ".concede", "a round with 2 or more wrong options needs a concede line");
        if (!j["companion"].isAbsent())
            loopLine(j["companion"], companion, path + ".companion", out.companion, L);
        else if (wrong >= 3)
            L.error(path + ".companion", "a round with 3 wrong options needs a companion line");
    } else {
        if (j.has("concede")) L.warn(path + ".concede", "ignored in a no-resolve loop");
        if (j.has("companion")) L.warn(path + ".companion", "ignored in a no-resolve loop");
    }
}

void loopFlag(JsonValue j, const std::string& path, DialogueLoopFlag& out, LoopLoad& L) {
    if (j.isString()) {
        if (j.string().empty()) L.error(path, "empty flag id");
        out.id = std::string(j.string());
        out.value = "true";
        return;
    }
    if (!j.isObject()) {
        L.error(path, "expected a flag id or { \"id\", \"value\" }");
        return;
    }
    L.unknownKeys(j, path, {"id", "value"});
    if (!j["id"].isString() || j["id"].string().empty()) {
        L.error(path + ".id", "expected a non-empty string");
        return;
    }
    out.id = std::string(j["id"].string());
    const JsonValue v = j["value"];
    if (v.isAbsent())
        out.value = "true";
    else if (v.isBool())
        out.value = v.boolean() ? "true" : "false";
    else if (v.isInteger())
        out.value = std::string(v.raw());
    else if (v.isString() && !v.string().empty())
        out.value = std::string(v.string());
    else
        L.error(path + ".value", "expected true, false, an integer or a non-empty string");
}

} // namespace

bool importDialogueLoop(std::string_view json, DialogueLoopDef& out, std::string* error, std::vector<std::string>* warnings) {
    out = DialogueLoopDef();
    LoopLoad L;
    L.warnings = warnings;

    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(json, &parseError)) {
        if (error) *error = "bad JSON: " + parseError;
        return false;
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        if (error) *error = "expected a top-level object";
        return false;
    }
    L.unknownKeys(root, "loop", {"version", "scene", "beat", "step", "resolve", "host", "companion", "rounds", "tag", "flags"});

    if (!root["version"].isInteger() || root["version"].integer() != 1) L.error("version", "expected 1");
    if (!root["scene"].isString() || root["scene"].string().empty())
        L.error("scene", "expected a non-empty scene id");
    else
        out.scene = std::string(root["scene"].string());
    loopIndex(root, "beat", out.beat, L);
    loopIndex(root, "step", out.step, L);

    const JsonValue resolve = root["resolve"];
    if (!resolve.isAbsent() && !resolve.isBool())
        L.error("resolve", "expected true or false");
    else
        out.resolve = resolve.boolean(true);

    std::string_view host = "host", companion = "companion";
    if (!root["host"].isAbsent()) {
        if (!root["host"].isString() || root["host"].string().empty())
            L.error("host", "expected a non-empty speaker id");
        else
            host = root["host"].string();
    }
    if (!root["companion"].isAbsent()) {
        if (!root["companion"].isString() || root["companion"].string().empty())
            L.error("companion", "expected a non-empty speaker id");
        else
            companion = root["companion"].string();
    }

    const JsonValue rounds = root["rounds"];
    if (!rounds.isArray() || rounds.size() == 0 || rounds.size() > DialogueLoopDef::kMaxRounds) {
        L.error("rounds", "expected 1 to 5 rounds");
    } else {
        out.roundCount = rounds.size();
        for (std::uint32_t r = 0; r < rounds.size(); ++r)
            loopRound(rounds[r], "rounds[" + std::to_string(r) + "]", out, host, companion, out.rounds[r], L);
    }

    const JsonValue tag = root["tag"];
    if (out.resolve) {
        if (!tag.isArray() || tag.size() == 0 || tag.size() > DialogueLoopDef::kMaxTagLines) {
            L.error("tag", "expected 1 to 8 lines");
        } else {
            out.tagCount = tag.size();
            for (std::uint32_t i = 0; i < tag.size(); ++i)
                loopLine(tag[i], host, "tag[" + std::to_string(i) + "]", out.tag[i], L);
        }
    } else if (!tag.isAbsent()) {
        L.warn("tag", "ignored in a no-resolve loop");
    }

    const JsonValue flags = root["flags"];
    if (flags.isAbsent() || (flags.isArray() && flags.size() == 0)) {
        L.warn("flags", "the loop sets no flag");
    } else if (!flags.isArray() || flags.size() > DialogueLoopDef::kMaxFlags) {
        L.error("flags", "expected up to 8 flags");
    } else {
        out.flagCount = flags.size();
        for (std::uint32_t i = 0; i < flags.size(); ++i)
            loopFlag(flags[i], "flags[" + std::to_string(i) + "]", out.flags[i], L);
    }

    if (L.errors.empty() && out.worstCasePicks() > DialogueLoopDef::kMaxPicks)
        L.warn("rounds", "the worst case takes " + std::to_string(out.worstCasePicks()) +
                             " picks; the cap of 5 ends the loop early");

    if (!L.errors.empty()) {
        if (error) {
            error->clear();
            for (const std::string& e : L.errors) {
                if (!error->empty()) *error += '\n';
                *error += e;
            }
        }
        out = DialogueLoopDef();
        return false;
    }
    return true;
}

// ---- runtime ----

std::string_view dialogueLoopStateName(DialogueLoopState state) {
    switch (state) {
    case DialogueLoopState::Idle: return "idle";
    case DialogueLoopState::Claim: return "claim";
    case DialogueLoopState::Audit: return "audit";
    case DialogueLoopState::Escalate: return "escalate";
    case DialogueLoopState::Concede: return "concede";
    case DialogueLoopState::Companion: return "companion";
    case DialogueLoopState::Reply: return "reply";
    case DialogueLoopState::Tag: return "tag";
    case DialogueLoopState::Done: return "done";
    }
    return "";
}

void DialogueLoop::reset() {
    const std::uint32_t epoch = epoch_ + 1;
    *this = DialogueLoop();
    epoch_ = epoch;
}

void DialogueLoop::begin(const DialogueLoopDef* def) {
    reset();
    if (!def || def->roundCount == 0) return;
    def_ = def;
    startRound(0);
}

void DialogueLoop::setState(DialogueLoopState s) {
    state_ = s;
    ++epoch_;
}

void DialogueLoop::startRound(std::uint32_t round) {
    round_ = round;
    misses_ = 0;
    greyed_ = 0;
    setState(DialogueLoopState::Claim);
}

void DialogueLoop::toEnd() {
    if (def_ && def_->resolve && def_->tagCount > 0) {
        tag_ = 0;
        setState(DialogueLoopState::Tag);
    } else {
        setState(DialogueLoopState::Done);
    }
}

void DialogueLoop::finishRound() {
    if (picks_ >= DialogueLoopDef::kMaxPicks || round_ + 1 >= roundCount())
        toEnd();
    else
        startRound(round_ + 1);
}

const DialogueLoopRound* DialogueLoop::currentRound() const {
    if (!def_ || round_ >= def_->roundCount || round_ >= DialogueLoopDef::kMaxRounds) return nullptr;
    return &def_->rounds[round_];
}

const DialogueLoopLine* DialogueLoop::currentLine() const {
    const DialogueLoopRound* r = currentRound();
    switch (state_) {
    case DialogueLoopState::Claim:
    case DialogueLoopState::Audit: return r ? &r->claim : nullptr;
    case DialogueLoopState::Escalate:
    case DialogueLoopState::Reply: return r && lastPick_ < r->optionCount ? &r->options[lastPick_].reply : nullptr;
    case DialogueLoopState::Concede: return r ? &r->concede : nullptr;
    case DialogueLoopState::Companion: return r ? &r->companion : nullptr;
    case DialogueLoopState::Tag:
        return def_ && tag_ < def_->tagCount && tag_ < DialogueLoopDef::kMaxTagLines ? &def_->tag[tag_] : nullptr;
    case DialogueLoopState::Idle:
    case DialogueLoopState::Done: break;
    }
    return nullptr;
}

std::string_view DialogueLoop::speaker() const {
    const DialogueLoopLine* l = currentLine();
    return l ? std::string_view(l->speaker) : std::string_view();
}

std::string_view DialogueLoop::text() const {
    const DialogueLoopLine* l = currentLine();
    return l ? std::string_view(l->text) : std::string_view();
}

std::uint32_t DialogueLoop::roundCount() const { return def_ ? def_->roundCount : 0; }

std::uint32_t DialogueLoop::optionCount() const {
    const DialogueLoopRound* r = currentRound();
    return r ? r->optionCount : 0;
}

std::string_view DialogueLoop::optionText(std::uint32_t i) const {
    const DialogueLoopRound* r = currentRound();
    return r && i < r->optionCount ? std::string_view(r->options[i].text) : std::string_view();
}

bool DialogueLoop::optionSelectable(std::uint32_t i) const {
    return state_ == DialogueLoopState::Audit && i < optionCount() && !optionGreyed(i) && picks_ < DialogueLoopDef::kMaxPicks;
}

bool DialogueLoop::press() {
    switch (state_) {
    case DialogueLoopState::Claim: setState(DialogueLoopState::Audit); return true;
    case DialogueLoopState::Escalate:
    case DialogueLoopState::Concede:
        // Back to the pick, unless the pick cap is reached.
        if (picks_ >= DialogueLoopDef::kMaxPicks)
            toEnd();
        else
            setState(DialogueLoopState::Audit);
        return true;
    case DialogueLoopState::Companion:
    case DialogueLoopState::Reply: finishRound(); return true;
    case DialogueLoopState::Tag:
        if (def_ && tag_ + 1 < def_->tagCount) {
            ++tag_;
            ++epoch_;
        } else {
            setState(DialogueLoopState::Done);
        }
        return true;
    case DialogueLoopState::Idle:
    case DialogueLoopState::Audit:
    case DialogueLoopState::Done: break;
    }
    return false;
}

bool DialogueLoop::choose(std::uint32_t i) {
    if (!optionSelectable(i)) return false;
    const DialogueLoopRound& r = *currentRound();
    const DialogueLoopOption& o = r.options[i];
    ++picks_;
    lastPick_ = i;
    if (o.right) {
        if (o.reply.empty())
            finishRound();
        else
            setState(DialogueLoopState::Reply);
        return true;
    }
    greyed_ |= 1u << i;
    ++misses_;
    if (!def_->resolve) {
        setState(DialogueLoopState::Escalate);
        return true;
    }
    const bool atCap = picks_ >= DialogueLoopDef::kMaxPicks;
    if ((misses_ >= 3 || atCap) && !r.companion.empty()) {
        ++companionResolves_;
        setState(DialogueLoopState::Companion);
    } else if (misses_ >= 2 && !r.concede.empty()) {
        setState(DialogueLoopState::Concede);
    } else {
        setState(DialogueLoopState::Escalate);
    }
    return true;
}

bool DialogueLoop::skip() {
    if (!active() || state_ == DialogueLoopState::Tag) return false;
    skipped_ = true;
    toEnd();
    return true;
}

DialogueLoopCommand DialogueLoop::autoplay() const {
    if (!active()) return {};
    if (state_ == DialogueLoopState::Tag) return {DialogueLoopCommand::Kind::Press, 0};
    return {DialogueLoopCommand::Kind::Skip, 0};
}

bool DialogueLoop::apply(const DialogueLoopCommand& command) {
    switch (command.kind) {
    case DialogueLoopCommand::Kind::Press: return press();
    case DialogueLoopCommand::Kind::Choose: return choose(command.option);
    case DialogueLoopCommand::Kind::Skip: return skip();
    case DialogueLoopCommand::Kind::None: break;
    }
    return false;
}

std::uint32_t DialogueLoop::flagCount() const { return def_ ? def_->flagCount : 0; }

std::string_view DialogueLoop::flagId(std::uint32_t i) const {
    return def_ && i < def_->flagCount && i < DialogueLoopDef::kMaxFlags ? std::string_view(def_->flags[i].id) : std::string_view();
}

std::string_view DialogueLoop::flagValue(std::uint32_t i) const {
    return def_ && i < def_->flagCount && i < DialogueLoopDef::kMaxFlags ? std::string_view(def_->flags[i].value) : std::string_view();
}

} // namespace ftcore
