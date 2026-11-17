#include "ftcore/Settings.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <array>
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

// The camera preset from a stored number: Close only for its own value; anything else (a corrupt 9, a -1) is Classic,
// the default lens, never the last one.
CameraPresetId cameraPresetFrom(std::int64_t v) {
    return v == static_cast<std::int64_t>(CameraPresetId::Close) ? CameraPresetId::Close : CameraPresetId::Classic;
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
    s.cameraShake = clampFloat(s.cameraShake, 0.f, 1.f, d.cameraShake);
    s.cameraPreset = cameraPresetFrom(static_cast<std::int64_t>(s.cameraPreset));
    s.hudScale = (std::clamp(s.hudScale, Settings::kMinHudScale, Settings::kMaxHudScale) + 5) / 10 * 10; // the menu's steps
    s.hudSafeMargin = std::clamp(s.hudSafeMargin, Settings::kMinHudSafeMargin, Settings::kMaxHudSafeMargin);
    s.showBasicControls =
        clampEnum(static_cast<std::int64_t>(s.showBasicControls), ShowBasicControls::Never, d.showBasicControls);
    s.textScale = (std::clamp(s.textScale, Settings::kMinTextScale, Settings::kMaxTextScale) + 5) / 10 * 10; // the menu's steps
    s.palette = clampEnum(static_cast<std::int64_t>(s.palette), UiPalette::HighContrast, d.palette);
    s.glyphMode = clampEnum(static_cast<std::int64_t>(s.glyphMode), UiGlyphMode::Pad, d.glyphMode);
}

std::string encodeSettings(const Settings& in) {
    Settings s = in;
    clampSettings(s);
    char buf[896];
    std::snprintf(buf, sizeof buf,
                  "{\"version\":%d,\"textSpeed\":%d,\"autoAdvance\":%s,\"autoDelay\":%.3f,\"skipMode\":%d,"
                  "\"master\":%.3f,\"music\":%.3f,\"voice\":%.3f,\"sfx\":%.3f,\"defenceAssist\":%d,"
                  "\"aimAssist\":%s,\"subtitleSize\":%d,\"reduceFlashes\":%s,\"qteAutoComplete\":%s,\"holdToggle\":%s,"
                  "\"skipChoices\":%s,\"stickFlicks\":%s,\"cameraShake\":%.3f,\"lockFraming\":%s,\"cameraPreset\":%d,"
                  "\"uiFirstUse\":%u,\"hudScale\":%d,\"hudSafeMargin\":%d,\"showBasicControls\":%d,"
                  "\"textScale\":%d,\"palette\":%d,\"reduceMotion\":%s,\"glyphMode\":%d}",
                  Settings::kVersion, static_cast<int>(s.textSpeed), s.autoAdvance ? "true" : "false",
                  static_cast<double>(s.autoDelay), static_cast<int>(s.skipMode), static_cast<double>(s.master),
                  static_cast<double>(s.music), static_cast<double>(s.voice), static_cast<double>(s.sfx),
                  static_cast<int>(s.defenceAssist), s.aimAssist ? "true" : "false", static_cast<int>(s.subtitleSize),
                  s.reduceFlashes ? "true" : "false", s.qteAutoComplete ? "true" : "false",
                  s.holdToggle ? "true" : "false", s.skipChoices ? "true" : "false",
                  s.stickFlicks ? "true" : "false", static_cast<double>(s.cameraShake), s.lockFraming ? "true" : "false",
                  static_cast<int>(s.cameraPreset), static_cast<unsigned>(s.uiFirstUse), s.hudScale, s.hudSafeMargin,
                  static_cast<int>(s.showBasicControls), s.textScale, static_cast<int>(s.palette),
                  s.reduceMotion ? "true" : "false", static_cast<int>(s.glyphMode));
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
    s.stickFlicks = boolField("stickFlicks", s.stickFlicks);
    s.aimAssist = boolField("aimAssist", s.aimAssist);
    s.cameraShake = floatField("cameraShake", s.cameraShake);
    s.lockFraming = boolField("lockFraming", s.lockFraming);
    s.cameraPreset = cameraPresetFrom(intField("cameraPreset", static_cast<std::int64_t>(s.cameraPreset))); // absent: Classic
    const std::int64_t taught = intField("uiFirstUse", 0); // 32 packed bits, so anything outside them is nothing taught
    s.uiFirstUse = taught >= 0 && taught <= 0xFFFFFFFF ? static_cast<std::uint32_t>(taught) : 0u;
    s.hudScale = static_cast<int>(std::clamp<std::int64_t>(intField("hudScale", s.hudScale), Settings::kMinHudScale, Settings::kMaxHudScale));
    s.hudSafeMargin = static_cast<int>(std::clamp<std::int64_t>(intField("hudSafeMargin", s.hudSafeMargin),
                                                                Settings::kMinHudSafeMargin, Settings::kMaxHudSafeMargin));
    s.showBasicControls = clampEnum(intField("showBasicControls", static_cast<std::int64_t>(s.showBasicControls)),
                                    ShowBasicControls::Never, s.showBasicControls);
    s.textScale = static_cast<int>(std::clamp<std::int64_t>(intField("textScale", s.textScale), Settings::kMinTextScale,
                                                            Settings::kMaxTextScale));
    s.palette = clampEnum(intField("palette", static_cast<std::int64_t>(s.palette)), UiPalette::HighContrast, s.palette);
    s.reduceMotion = boolField("reduceMotion", s.reduceMotion);
    s.glyphMode = clampEnum(intField("glyphMode", static_cast<std::int64_t>(s.glyphMode)), UiGlyphMode::Pad, s.glyphMode);
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
    case SettingsRow::AimAssist: return "Aim assist";
    case SettingsRow::SubtitleSize: return "Subtitle size";
    case SettingsRow::ReduceFlashes: return "Reduce flashes";
    case SettingsRow::QteAutoComplete: return "Quick-time events";
    case SettingsRow::HoldToggle: return "Hold inputs";
    case SettingsRow::SkipChoices: return "Skip choices";
    case SettingsRow::StickFlicks: return "Stick flicks";
    case SettingsRow::CameraShake: return "Camera shake";
    case SettingsRow::LockFraming: return "Lock framing";
    case SettingsRow::CameraPreset: return "Camera";
    case SettingsRow::HudScale: return "HUD scale";
    case SettingsRow::HudSafeMargin: return "HUD safe margin";
    case SettingsRow::ShowBasicControls: return "Basic controls";
    case SettingsRow::TextScale: return "Text size";
    case SettingsRow::Palette: return "Colours";
    case SettingsRow::ReduceMotion: return "Reduce motion";
    case SettingsRow::GlyphMode: return "Button prompts";
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
    case SettingsRow::StickFlicks: word = s.stickFlicks ? "On" : "Off"; break;
    case SettingsRow::AimAssist: word = s.aimAssist ? "On" : "Off"; break;
    case SettingsRow::LockFraming: word = s.lockFraming ? "On" : "Off"; break;
    case SettingsRow::CameraPreset: word = s.cameraPreset == CameraPresetId::Close ? "Close" : "Classic"; break;
    case SettingsRow::ShowBasicControls:
        word = s.showBasicControls == ShowBasicControls::FirstFights ? "First fights"
               : s.showBasicControls == ShowBasicControls::Always    ? "Always"
                                                                     : "Never";
        break;
    case SettingsRow::Palette:
        switch (s.palette) {
        case UiPalette::Protan: word = "Protanopia"; break;
        case UiPalette::Deutan: word = "Deuteranopia"; break;
        case UiPalette::Tritan: word = "Tritanopia"; break;
        case UiPalette::HighContrast: word = "High contrast"; break;
        case UiPalette::Default: word = "Default"; break;
        }
        if (!word) word = "Default";
        break;
    case SettingsRow::ReduceMotion: word = s.reduceMotion ? "On" : "Off"; break;
    case SettingsRow::GlyphMode:
        word = s.glyphMode == UiGlyphMode::Keys ? "Keyboard" : s.glyphMode == UiGlyphMode::Pad ? "Controller" : "Last used";
        break;
    case SettingsRow::Back: word = ""; break;
    default: break;
    }
    int n = 0;
    if (word) {
        n = std::snprintf(buf, cap, "%s", word);
    } else if (row == SettingsRow::HudScale) {
        n = std::snprintf(buf, cap, "%d%%", s.hudScale);
    } else if (row == SettingsRow::TextScale) {
        n = std::snprintf(buf, cap, "%d%%", s.textScale);
    } else if (row == SettingsRow::HudSafeMargin) {
        n = std::snprintf(buf, cap, "%d%%", s.hudSafeMargin);
    } else if (row == SettingsRow::AutoDelay) {
        n = std::snprintf(buf, cap, "%.2f s", static_cast<double>(s.autoDelay));
    } else {
        const float v = row == SettingsRow::Master        ? s.master
                        : row == SettingsRow::Music       ? s.music
                        : row == SettingsRow::Voice       ? s.voice
                        : row == SettingsRow::CameraShake ? s.cameraShake
                                                          : s.sfx;
        n = std::snprintf(buf, cap, "%d%%", static_cast<int>(std::lround(v * 100.f)));
    }
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    return static_cast<std::size_t>(n) < cap ? static_cast<std::size_t>(n) : cap - 1;
}

const char* settingsRowKey(SettingsRow row) {
    switch (row) {
    case SettingsRow::TextSpeed: return "settings.text_speed";
    case SettingsRow::AutoAdvance: return "settings.auto_advance";
    case SettingsRow::AutoDelay: return "settings.auto_delay";
    case SettingsRow::SkipMode: return "settings.skip";
    case SettingsRow::Master: return "settings.master_volume";
    case SettingsRow::Music: return "settings.music_volume";
    case SettingsRow::Voice: return "settings.voice_volume";
    case SettingsRow::Sfx: return "settings.effects_volume";
    case SettingsRow::DefenceAssist: return "settings.defence_assist";
    case SettingsRow::AimAssist: return "settings.aim_assist";
    case SettingsRow::SubtitleSize: return "settings.subtitle_size";
    case SettingsRow::ReduceFlashes: return "settings.reduce_flashes";
    case SettingsRow::QteAutoComplete: return "settings.quick_time";
    case SettingsRow::HoldToggle: return "settings.hold_inputs";
    case SettingsRow::SkipChoices: return "settings.skip_choices";
    case SettingsRow::StickFlicks: return "settings.stick_flicks";
    case SettingsRow::CameraShake: return "settings.camera_shake";
    case SettingsRow::LockFraming: return "settings.lock_framing";
    case SettingsRow::CameraPreset: return "settings.camera";
    case SettingsRow::HudScale: return "settings.hud_scale";
    case SettingsRow::HudSafeMargin: return "settings.hud_safe_margin";
    case SettingsRow::ShowBasicControls: return "settings.basic_controls";
    case SettingsRow::TextScale: return "settings.text_size";
    case SettingsRow::Palette: return "settings.colours";
    case SettingsRow::ReduceMotion: return "settings.reduce_motion";
    case SettingsRow::GlyphMode: return "settings.button_prompts";
    case SettingsRow::Back: return "settings.back";
    }
    return "";
}

const char* settingsValueKey(const Settings& s, SettingsRow row) {
    constexpr const char* on = "settings.on";
    constexpr const char* off = "settings.off";
    switch (row) {
    case SettingsRow::TextSpeed:
        return s.textSpeed == TextSpeed::Slow     ? "settings.text_speed.slow"
               : s.textSpeed == TextSpeed::Normal ? "settings.text_speed.normal"
               : s.textSpeed == TextSpeed::Fast   ? "settings.text_speed.fast"
                                                  : "settings.text_speed.instant";
    case SettingsRow::AutoAdvance: return s.autoAdvance ? on : off;
    case SettingsRow::SkipMode: return s.skipMode == SkipMode::Seen ? "settings.skip.seen" : "settings.skip.all";
    case SettingsRow::DefenceAssist: return s.defenceAssist == DefenceAssist::Wide ? "settings.defence_assist.wide" : off;
    case SettingsRow::SubtitleSize:
        return s.subtitleSize == SubtitleSize::Small    ? "settings.subtitle_size.small"
               : s.subtitleSize == SubtitleSize::Medium ? "settings.subtitle_size.medium"
                                                        : "settings.subtitle_size.large";
    case SettingsRow::ReduceFlashes: return s.reduceFlashes ? on : off;
    case SettingsRow::QteAutoComplete: return s.qteAutoComplete ? "settings.quick_time.auto" : "settings.quick_time.play";
    case SettingsRow::HoldToggle: return s.holdToggle ? "settings.hold_inputs.press_once" : "settings.hold_inputs.hold_down";
    case SettingsRow::SkipChoices: return s.skipChoices ? on : off;
    case SettingsRow::StickFlicks: return s.stickFlicks ? on : off;
    case SettingsRow::AimAssist: return s.aimAssist ? on : off;
    case SettingsRow::LockFraming: return s.lockFraming ? on : off;
    case SettingsRow::CameraPreset: return s.cameraPreset == CameraPresetId::Close ? "settings.camera.close" : "settings.camera.classic";
    case SettingsRow::ShowBasicControls:
        return s.showBasicControls == ShowBasicControls::FirstFights ? "settings.basic_controls.first_fights"
               : s.showBasicControls == ShowBasicControls::Always    ? "settings.basic_controls.always"
                                                                     : "settings.basic_controls.never";
    case SettingsRow::Palette:
        switch (s.palette) {
        case UiPalette::Protan: return "settings.colours.protanopia";
        case UiPalette::Deutan: return "settings.colours.deuteranopia";
        case UiPalette::Tritan: return "settings.colours.tritanopia";
        case UiPalette::HighContrast: return "settings.colours.high_contrast";
        case UiPalette::Default: break;
        }
        return "settings.colours.default";
    case SettingsRow::ReduceMotion: return s.reduceMotion ? on : off;
    case SettingsRow::GlyphMode:
        return s.glyphMode == UiGlyphMode::Keys  ? "settings.button_prompts.keyboard"
               : s.glyphMode == UiGlyphMode::Pad ? "settings.button_prompts.controller"
                                                 : "settings.button_prompts.last_used";
    default: return nullptr;
    }
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
    case SettingsRow::StickFlicks: return setBool(s.stickFlicks, dir, wrap);
    case SettingsRow::AimAssist: return setBool(s.aimAssist, dir, wrap);
    case SettingsRow::CameraShake: return stepFloat(s.cameraShake, stepTenth(s.cameraShake, step));
    case SettingsRow::LockFraming: return setBool(s.lockFraming, dir, wrap);
    case SettingsRow::CameraPreset: return stepEnum(s.cameraPreset, dir, CameraPresetId::Close, wrap);
    case SettingsRow::HudScale: {
        const int next = std::clamp(s.hudScale + 10 * step, Settings::kMinHudScale, Settings::kMaxHudScale); // a slider: stops at its ends
        if (next == s.hudScale) return false;
        s.hudScale = next;
        return true;
    }
    case SettingsRow::HudSafeMargin: {
        const int next = std::clamp(s.hudSafeMargin + step, Settings::kMinHudSafeMargin, Settings::kMaxHudSafeMargin); // a slider
        if (next == s.hudSafeMargin) return false;
        s.hudSafeMargin = next;
        return true;
    }
    case SettingsRow::ShowBasicControls: return stepEnum(s.showBasicControls, dir, ShowBasicControls::Never, wrap);
    case SettingsRow::TextScale: {
        const int next = std::clamp(s.textScale + 10 * step, Settings::kMinTextScale, Settings::kMaxTextScale); // a slider
        if (next == s.textScale) return false;
        s.textScale = next;
        return true;
    }
    case SettingsRow::Palette: return stepEnum(s.palette, dir, UiPalette::HighContrast, wrap);
    case SettingsRow::ReduceMotion: return setBool(s.reduceMotion, dir, wrap);
    case SettingsRow::GlyphMode: return stepEnum(s.glyphMode, dir, UiGlyphMode::Pad, wrap);
    case SettingsRow::Back: break;
    }
    return false;
}

namespace {

constexpr SettingsGroup groupOf(SettingsRow row) {
    switch (row) {
    case SettingsRow::TextSpeed:
    case SettingsRow::AutoAdvance:
    case SettingsRow::AutoDelay:
    case SettingsRow::SkipMode:
    case SettingsRow::SkipChoices:
    case SettingsRow::Back: return SettingsGroup::Text;
    case SettingsRow::Master:
    case SettingsRow::Music:
    case SettingsRow::Voice:
    case SettingsRow::Sfx: return SettingsGroup::Sound;
    case SettingsRow::DefenceAssist:
    case SettingsRow::AimAssist:
    case SettingsRow::StickFlicks:
    case SettingsRow::GlyphMode: return SettingsGroup::Play;
    case SettingsRow::CameraShake:
    case SettingsRow::LockFraming:
    case SettingsRow::CameraPreset: return SettingsGroup::Camera;
    case SettingsRow::HudScale:
    case SettingsRow::HudSafeMargin:
    case SettingsRow::ShowBasicControls: return SettingsGroup::Display;
    case SettingsRow::SubtitleSize:
    case SettingsRow::ReduceFlashes:
    case SettingsRow::QteAutoComplete:
    case SettingsRow::HoldToggle:
    case SettingsRow::TextScale:
    case SettingsRow::Palette:
    case SettingsRow::ReduceMotion: return SettingsGroup::Access;
    }
    return SettingsGroup::Text;
}

struct ScreenOrder {
    std::array<SettingsRow, kSettingsRowCount> row{};
    std::array<std::uint8_t, kSettingsRowCount> index{};
};

constexpr ScreenOrder makeScreenOrder() {
    ScreenOrder o{};
    std::uint32_t n = 0;
    for (std::uint32_t g = 0; g < kSettingsGroupCount; ++g) {
        for (std::uint32_t r = 0; r < kSettingsRowCount; ++r) {
            const auto row = static_cast<SettingsRow>(r);
            if (row != SettingsRow::Back && static_cast<std::uint32_t>(groupOf(row)) == g) {
                o.row[n] = row;
                o.index[r] = static_cast<std::uint8_t>(n);
                ++n;
            }
        }
    }
    o.row[n] = SettingsRow::Back;
    o.index[static_cast<std::uint32_t>(SettingsRow::Back)] = static_cast<std::uint8_t>(n);
    return o;
}

constexpr ScreenOrder kScreenOrder = makeScreenOrder();
static_assert(kScreenOrder.row[kSettingsRowCount - 1] == SettingsRow::Back);
static_assert(kScreenOrder.row[0] == SettingsRow::TextSpeed);

} // namespace

SettingsGroup settingsRowGroup(SettingsRow row) { return groupOf(row); }

SettingsRow settingsScreenRow(std::uint32_t index) {
    return index < kSettingsRowCount ? kScreenOrder.row[index] : SettingsRow::Back;
}

std::uint32_t settingsScreenIndex(SettingsRow row) {
    const auto r = static_cast<std::uint32_t>(row);
    return r < kSettingsRowCount ? kScreenOrder.index[r] : kSettingsRowCount - 1;
}

const char* settingsGroupLabel(SettingsGroup group) {
    switch (group) {
    case SettingsGroup::Text: return "Text";
    case SettingsGroup::Sound: return "Sound";
    case SettingsGroup::Play: return "Play";
    case SettingsGroup::Camera: return "Camera";
    case SettingsGroup::Display: return "Display";
    case SettingsGroup::Access: return "Access";
    }
    return "";
}

std::array<OptionFlag, kOptionFlagCount> settingsOptionFlags(const Settings& s) {
    return {{{"F_SYS_OPT_QTE_AUTOCOMPLETE", s.qteAutoComplete},
             {"F_SYS_OPT_HOLD_TOGGLE", s.holdToggle},
             {"F_SYS_OPT_SKIP_CHOICES", s.skipChoices}}};
}

SettingsAction SettingsMenu::press(Settings& s, SettingsKey key) {
    const std::uint32_t at = settingsScreenIndex(selected_);
    switch (key) {
    case SettingsKey::Up:
        if (at == 0) return SettingsAction::None;
        selected_ = settingsScreenRow(at - 1);
        return SettingsAction::Moved;
    case SettingsKey::Down:
        if (at + 1u >= kSettingsRowCount) return SettingsAction::None;
        selected_ = settingsScreenRow(at + 1);
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
