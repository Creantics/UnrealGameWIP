#include "ftcore/Banter.h"

#include "ftcore/DialogueLoop.h"
#include "ftcore/Json.h"

#include <algorithm>
#include <initializer_list>

namespace ftcore {

// ---- definition ----

std::uint32_t BanterDef::speakerIndex(std::string_view speaker) const {
    for (std::uint32_t i = 0; i < speakerCount && i < kMaxSpeakers; ++i)
        if (speakers[i] == speaker) return i;
    return speakerCount;
}

double BanterDef::length() const {
    double t = 0.0;
    for (std::uint32_t i = 0; i < lineCount && i < kMaxLines; ++i) t += banterHold(lines[i]);
    return t;
}

double banterHold(const BanterLine& line) {
    if (line.hold > 0.0) return line.hold;
    const double t = kBanterHoldBase + kBanterHoldPerByte * static_cast<double>(line.text.size());
    return std::clamp(t, kBanterMinHold, kBanterMaxHold);
}

std::string banterFileName(std::string_view scene, std::uint32_t beat, std::uint32_t step) {
    return dialogueLoopFileName(scene, beat, step);
}

// ---- loader ----

namespace {

struct BanterLoad {
    std::vector<std::string> errors;
    std::vector<std::string>* warnings = nullptr;

    void error(const std::string& path, std::string_view what) { errors.push_back(path + ": " + std::string(what)); }
    // Key names only: keys are format, not story text.
    void unknownKeys(JsonValue j, const std::string& path, std::initializer_list<std::string_view> keys) {
        for (std::uint32_t k = 0; k < j.size(); ++k) {
            const std::string_view key = j.key(k);
            if (std::find(keys.begin(), keys.end(), key) == keys.end() && warnings)
                warnings->push_back(path + ": unknown key: " + std::string(key));
        }
    }
};

bool banterIndex(JsonValue root, const char* key, std::uint32_t& out, BanterLoad& L) {
    const JsonValue v = root[key];
    if (!v.isInteger() || v.integer() < 0 || v.integer() > 1000000) {
        L.error(key, "expected an integer in 0..1000000");
        return false;
    }
    out = static_cast<std::uint32_t>(v.integer());
    return true;
}

} // namespace

bool importBanter(std::string_view json, BanterDef& out, std::string* error, std::vector<std::string>* warnings) {
    out = BanterDef{};
    BanterLoad L;
    L.warnings = warnings;
    const auto finish = [&]() {
        if (L.errors.empty()) return true;
        out = BanterDef{};
        if (error) {
            error->clear();
            for (const std::string& e : L.errors) {
                if (!error->empty()) *error += '\n';
                *error += e;
            }
        }
        return false;
    };

    JsonDocument doc;
    std::string err;
    if (!doc.parse(json, &err)) {
        L.error("banter file", err);
        return finish();
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) {
        L.error("banter file", "expected an object");
        return finish();
    }
    if (const JsonValue v = root["version"]; !(v.isInteger() && v.integer() == 1)) L.error("version", "expected 1");
    L.unknownKeys(root, "banter file", {"version", "scene", "beat", "step", "lines"});

    const JsonValue scene = root["scene"];
    if (!scene.isString() || scene.string().empty()) L.error("scene", "expected a scene id");
    else out.scene = std::string(scene.string());
    banterIndex(root, "beat", out.beat, L);
    banterIndex(root, "step", out.step, L);

    const JsonValue lines = root["lines"];
    if (!lines.isArray() || lines.size() == 0) {
        L.error("lines", "expected 1 to " + std::to_string(BanterDef::kMaxLines) + " lines");
    } else if (lines.size() > BanterDef::kMaxLines) {
        L.error("lines", "more than " + std::to_string(BanterDef::kMaxLines) + " lines");
    } else {
        for (std::uint32_t i = 0; i < lines.size(); ++i) {
            const JsonValue j = lines[i];
            const std::string path = "lines[" + std::to_string(i) + "]";
            if (!j.isObject()) {
                L.error(path, "expected an object");
                continue;
            }
            L.unknownKeys(j, path, {"speaker", "text", "hold"});
            BanterLine& line = out.lines[out.lineCount++];
            const JsonValue speaker = j["speaker"];
            const JsonValue text = j["text"];
            const JsonValue hold = j["hold"];
            if (!speaker.isString() || speaker.string().empty()) L.error(path + ".speaker", "expected a non-empty string");
            else line.speaker = std::string(speaker.string());
            if (!text.isString() || text.string().empty()) L.error(path + ".text", "expected a non-empty string");
            else line.text = std::string(text.string());
            if (!hold.isAbsent()) {
                const double h = hold.number(-1.0);
                if (!hold.isNumber() || h < kBanterHoldFloor || h > kBanterHoldCeiling) L.error(path + ".hold", "expected seconds in 0.5..15");
                else line.hold = h;
            }
            if (line.speaker.empty() || out.speakerIndex(line.speaker) < out.speakerCount) continue;
            if (out.speakerCount >= BanterDef::kMaxSpeakers) {
                L.error(path + ".speaker", "more than " + std::to_string(BanterDef::kMaxSpeakers) + " speakers");
                continue;
            }
            out.speakers[out.speakerCount++] = line.speaker;
        }
    }
    return finish();
}

// ---- runtime ----

void BanterSequence::reset() {
    def_ = nullptr;
    line_ = presses_ = shown_ = 0;
    age_ = hold_ = 0.0;
    skipped_ = false;
    done_ = true;
    ++epoch_;
}

void BanterSequence::begin(const BanterDef* def) {
    reset();
    if (!def || def->lineCount == 0) return;
    def_ = def;
    done_ = false;
    show(0);
}

std::uint32_t BanterSequence::count() const {
    return def_ ? std::min(def_->lineCount, BanterDef::kMaxLines) : 0u;
}

void BanterSequence::show(std::uint32_t line) {
    ++epoch_;
    age_ = 0.0;
    if (line >= count()) {
        done_ = true;
        hold_ = 0.0;
        return;
    }
    line_ = line;
    ++shown_;
    hold_ = banterHold(def_->lines[line]);
}

bool BanterSequence::tick(double seconds) {
    if (!active() || seconds <= 0.0) return false;
    age_ += seconds;
    bool changed = false;
    while (active() && age_ >= hold_) {
        const double over = age_ - hold_;
        show(line_ + 1);
        age_ = active() ? over : 0.0;
        changed = true;
    }
    return changed;
}

bool BanterSequence::press() {
    if (!active()) return false;
    ++presses_;
    show(line_ + 1);
    return true;
}

bool BanterSequence::skip() {
    if (!active()) return false;
    skipped_ = true;
    done_ = true;
    age_ = hold_ = 0.0;
    ++epoch_;
    return true;
}

std::string_view BanterSequence::speaker() const {
    return active() ? std::string_view(def_->lines[line_].speaker) : std::string_view();
}

std::string_view BanterSequence::text() const {
    return active() ? std::string_view(def_->lines[line_].text) : std::string_view();
}

BanterSide BanterSequence::side() const {
    if (!active()) return BanterSide::Left;
    return def_->speakerIndex(def_->lines[line_].speaker) % 2u == 0u ? BanterSide::Left : BanterSide::Right;
}

} // namespace ftcore
