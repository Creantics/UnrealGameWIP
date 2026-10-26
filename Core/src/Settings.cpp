#include "ftcore/Settings.h"

#include "ftcore/Json.h"

#include <cmath>
#include <cstdio>

namespace ftcore {

namespace {

float clampFloat(float v, float lo, float hi, float fallback) {
    if (!(v == v)) return fallback; // NaN
    return v < lo ? lo : v > hi ? hi : v;
}

template <typename E>
E clampEnum(std::int64_t v, E last, E fallback) {
    if (v < 0) return fallback;
    if (v > static_cast<std::int64_t>(last)) return last;
    return static_cast<E>(v);
}

// The next step of a value on a 0.1 grid, kept exact so a saved value reads back the same.
float stepTenth(float v, int dir) {
    const float n = std::round(v * 10.f) + static_cast<float>(dir);
    return clampFloat(n / 10.f, 0.f, 1.f, 0.f);
}

float stepQuarter(float v, int dir) {
    const float n = std::round(v * 4.f) + static_cast<float>(dir);
    return clampFloat(n / 4.f, Settings::kMinAutoDelay, Settings::kMaxAutoDelay, 1.5f);
}

bool setBool(bool& v, int dir, bool wrap) {
    const bool next = wrap ? !v : dir > 0;
    if (next == v) return false;
    v = next;
    return true;
}

template <typename E>
bool stepEnum(E& v, int dir, E last, bool wrap) {
    int i = static_cast<int>(v) + (dir < 0 ? -1 : 1);
    const int top = static_cast<int>(last);
    if (i < 0) i = wrap ? top : 0;
    if (i > top) i = wrap ? 0 : top;
    if (static_cast<E>(i) == v) return false;
    v = static_cast<E>(i);
    return true;
}

bool stepFloat(float& v, float next) {
    if (next == v) return false;
    v = next;
    return true;
}

} // namespace

void clampSettings(Settings& s) {
    const Settings d;
    s.textSpeed = clampEnum(static_cast<std::int64_t>(s.textSpeed), TextSpeed::Instant, d.textSpeed);
    s.skipMode = clampEnum(static_cast<std::int64_t>(s.skipMode), SkipMode::Seen, d.skipMode);
    s.defenceAssist = clampEnum(static_cast<std::int64_t>(s.defenceAssist), DefenceAssist::Wide, d.defenceAssist);
    s.subtitleSize = clampEnum(static_cast<std::int64_t>(s.subtitleSize), SubtitleSize::Large, d.subtitleSize);
    s.autoDelay = clampFloat(s.autoDelay, Settings::kMinAutoDelay, Settings::kMaxAutoDelay, d.autoDelay);
    s.master = clampFloat(s.master, 0.f, 1.f, d.master);
    s.music = clampFloat(s.music, 0.f, 1.f, d.music);
    s.voice = clampFloat(s.voice, 0.f, 1.f, d.voice);
    s.sfx = clampFloat(s.sfx, 0.f, 1.f, d.sfx);
}

std::string encodeSettings(const Settings& in) {
    Settings s = in;
    clampSettings(s);
    char buf[640];
    std::snprintf(buf, sizeof buf,
                  "{\"version\":%d,\"textSpeed\":%d,\"autoAdvance\":%s,\"autoDelay\":%.3f,\"skipMode\":%d,"
                  "\"master\":%.3f,\"music\":%.3f,\"voice\":%.3f,\"sfx\":%.3f,\"defenceAssist\":%d,"
                  "\"subtitleSize\":%d,\"reduceFlashes\":%s,\"qteAutoComplete\":%s,\"holdToggle\":%s,"
                  "\"skipChoices\":%s}",
                  Settings::kVersion, static_cast<int>(s.textSpeed), s.autoAdvance ? "true" : "false",
                  static_cast<double>(s.autoDelay), static_cast<int>(s.skipMode), static_cast<double>(s.master),
                  static_cast<double>(s.music), static_cast<double>(s.voice), static_cast<double>(s.sfx),
                  static_cast<int>(s.defenceAssist), static_cast<int>(s.subtitleSize),
                  s.reduceFlashes ? "true" : "false", s.qteAutoComplete ? "true" : "false",
                  s.holdToggle ? "true" : "false", s.skipChoices ? "true" : "false");
    return buf;
}

bool decodeSettings(std::string_view text, Settings& out, std::string* error) {
    auto fail = [error](const char* msg) {
        if (error) *error = msg;
        return false;
    };
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(text, &parseError)) {
        if (error) *error = "settings are not valid JSON: " + parseError;
        return false;
    }
    const JsonValue root = doc.root();
    if (!root.isObject()) return fail("settings: the top level must be an object");
    const JsonValue version = root["version"];
    if (!version.isInteger()) return fail("settings: \"version\" is missing or not an integer");
    if (version.integer() < 1 || version.integer() > Settings::kVersion) return fail("settings: unsupported version");

    Settings s;
    auto intField = [&root](const char* key, std::int64_t fallback) {
        const JsonValue v = root[key];
        return v.isInteger() ? v.integer() : fallback;
    };
    auto floatField = [&root](const char* key, float fallback) {
        const JsonValue v = root[key];
        return v.isNumber() ? static_cast<float>(v.number()) : fallback;
    };
    auto boolField = [&root](const char* key, bool fallback) {
        const JsonValue v = root[key];
        return v.isBool() ? v.boolean() : fallback;
    };
    s.textSpeed = clampEnum(intField("textSpeed", static_cast<std::int64_t>(s.textSpeed)), TextSpeed::Instant, s.textSpeed);
    s.autoAdvance = boolField("autoAdvance", s.autoAdvance);
    s.autoDelay = floatField("autoDelay", s.autoDelay);
    s.skipMode = clampEnum(intField("skipMode", static_cast<std::int64_t>(s.skipMode)), SkipMode::Seen, s.skipMode);
    s.master = floatField("master", s.master);
    s.music = floatField("music", s.music);
    s.voice = floatField("voice", s.voice);
    s.sfx = floatField("sfx", s.sfx);
    s.defenceAssist =
        clampEnum(intField("defenceAssist", static_cast<std::int64_t>(s.defenceAssist)), DefenceAssist::Wide, s.defenceAssist);
    s.subtitleSize =
        clampEnum(intField("subtitleSize", static_cast<std::int64_t>(s.subtitleSize)), SubtitleSize::Large, s.subtitleSize);
    s.reduceFlashes = boolField("reduceFlashes", s.reduceFlashes);
    s.qteAutoComplete = boolField("qteAutoComplete", s.qteAutoComplete);
    s.holdToggle = boolField("holdToggle", s.holdToggle);
    s.skipChoices = boolField("skipChoices", s.skipChoices);
    clampSettings(s);
    out = s;
    return true;
}

const char* settingsRowLabel(SettingsRow row) {
    switch (row) {
    case SettingsRow::TextSpeed: return "Text speed";
    case SettingsRow::AutoAdvance: return "Auto-advance";
    case SettingsRow::AutoDelay: return "Auto-advance delay";
    case SettingsRow::SkipMode: return "Skip";
    case SettingsRow::Master: return "Master volume";
    case SettingsRow::Music: return "Music volume";
    case SettingsRow::Voice: return "Voice volume";
    case SettingsRow::Sfx: return "Effects volume";
    case SettingsRow::DefenceAssist: return "Defence window assist";
    case SettingsRow::SubtitleSize: return "Subtitle size";
    case SettingsRow::ReduceFlashes: return "Reduce flashes";
    case SettingsRow::QteAutoComplete: return "Quick-time events";
    case SettingsRow::HoldToggle: return "Hold inputs";
    case SettingsRow::SkipChoices: return "Skip choices";
    case SettingsRow::Back: return "Back";
    }
    return "";
}

std::size_t settingsRowValue(const Settings& s, SettingsRow row, char* buf, std::size_t cap) {
    if (!buf || cap == 0) return 0;
    const char* word = nullptr;
    switch (row) {
    case SettingsRow::TextSpeed:
        word = s.textSpeed == TextSpeed::Slow     ? "Slow"
               : s.textSpeed == TextSpeed::Normal ? "Normal"
               : s.textSpeed == TextSpeed::Fast   ? "Fast"
                                                  : "Instant";
        break;
    case SettingsRow::AutoAdvance: word = s.autoAdvance ? "On" : "Off"; break;
    case SettingsRow::SkipMode: word = s.skipMode == SkipMode::Seen ? "Seen lines only" : "All lines"; break;
    case SettingsRow::DefenceAssist: word = s.defenceAssist == DefenceAssist::Wide ? "Wide" : "Off"; break;
    case SettingsRow::SubtitleSize:
        word = s.subtitleSize == SubtitleSize::Small ? "Small" : s.subtitleSize == SubtitleSize::Medium ? "Medium" : "Large";
        break;
    case SettingsRow::ReduceFlashes: word = s.reduceFlashes ? "On" : "Off"; break;
    case SettingsRow::QteAutoComplete: word = s.qteAutoComplete ? "Auto-complete" : "Play"; break;
    case SettingsRow::HoldToggle: word = s.holdToggle ? "Press once" : "Hold down"; break;
    case SettingsRow::SkipChoices: word = s.skipChoices ? "On" : "Off"; break;
    case SettingsRow::Back: word = ""; break;
    default: break;
    }
    int n = 0;
    if (word) {
        n = std::snprintf(buf, cap, "%s", word);
    } else if (row == SettingsRow::AutoDelay) {
        n = std::snprintf(buf, cap, "%.2f s", static_cast<double>(s.autoDelay));
    } else {
        const float v = row == SettingsRow::Master  ? s.master
                        : row == SettingsRow::Music ? s.music
                        : row == SettingsRow::Voice ? s.voice
                                                    : s.sfx;
        n = std::snprintf(buf, cap, "%d%%", static_cast<int>(std::lround(v * 100.f)));
    }
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    return static_cast<std::size_t>(n) < cap ? static_cast<std::size_t>(n) : cap - 1;
}

bool adjustSetting(Settings& s, SettingsRow row, int dir, bool wrap) {
    const int step = dir < 0 ? -1 : 1;
    switch (row) {
    case SettingsRow::TextSpeed: return stepEnum(s.textSpeed, dir, TextSpeed::Instant, wrap);
    case SettingsRow::AutoAdvance: return setBool(s.autoAdvance, dir, wrap);
    case SettingsRow::AutoDelay: return stepFloat(s.autoDelay, stepQuarter(s.autoDelay, step));
    case SettingsRow::SkipMode: return stepEnum(s.skipMode, dir, SkipMode::Seen, wrap);
    case SettingsRow::Master: return stepFloat(s.master, stepTenth(s.master, step));
    case SettingsRow::Music: return stepFloat(s.music, stepTenth(s.music, step));
    case SettingsRow::Voice: return stepFloat(s.voice, stepTenth(s.voice, step));
    case SettingsRow::Sfx: return stepFloat(s.sfx, stepTenth(s.sfx, step));
    case SettingsRow::DefenceAssist: return stepEnum(s.defenceAssist, dir, DefenceAssist::Wide, wrap);
    case SettingsRow::SubtitleSize: return stepEnum(s.subtitleSize, dir, SubtitleSize::Large, wrap);
    case SettingsRow::ReduceFlashes: return setBool(s.reduceFlashes, dir, wrap);
    case SettingsRow::QteAutoComplete: return setBool(s.qteAutoComplete, dir, wrap);
    case SettingsRow::HoldToggle: return setBool(s.holdToggle, dir, wrap);
    case SettingsRow::SkipChoices: return setBool(s.skipChoices, dir, wrap);
    case SettingsRow::Back: break;
    }
    return false;
}

std::array<OptionFlag, kOptionFlagCount> settingsOptionFlags(const Settings& s) {
    return {{{"F_SYS_OPT_QTE_AUTOCOMPLETE", s.qteAutoComplete},
             {"F_SYS_OPT_HOLD_TOGGLE", s.holdToggle},
             {"F_SYS_OPT_SKIP_CHOICES", s.skipChoices}}};
}

SettingsAction SettingsMenu::press(Settings& s, SettingsKey key) {
    const std::uint8_t row = static_cast<std::uint8_t>(selected_);
    switch (key) {
    case SettingsKey::Up:
        if (row == 0) return SettingsAction::None;
        selected_ = static_cast<SettingsRow>(row - 1);
        return SettingsAction::Moved;
    case SettingsKey::Down:
        if (row + 1u >= kSettingsRowCount) return SettingsAction::None;
        selected_ = static_cast<SettingsRow>(row + 1);
        return SettingsAction::Moved;
    case SettingsKey::Left: return adjustSetting(s, selected_, -1, false) ? SettingsAction::Changed : SettingsAction::None;
    case SettingsKey::Right: return adjustSetting(s, selected_, 1, false) ? SettingsAction::Changed : SettingsAction::None;
    case SettingsKey::Confirm:
        if (selected_ == SettingsRow::Back) return SettingsAction::Close;
        return adjustSetting(s, selected_, 1, true) ? SettingsAction::Changed : SettingsAction::None;
    }
    return SettingsAction::None;
}

SettingsAction SettingsMenu::click(Settings& s, SettingsRow row, int dir) {
    if (static_cast<std::uint8_t>(row) >= kSettingsRowCount) return SettingsAction::None;
    selected_ = row;
    return press(s, dir < 0 ? SettingsKey::Left : SettingsKey::Confirm);
}

} // namespace ftcore
