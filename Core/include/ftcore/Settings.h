#pragma once

// Player settings, independent of any engine: the values, their JSON text, and the rules of the
// settings menu (which row is selected, what a key does to it). The owner stores the JSON (in
// Unreal, a file under Saved/) and draws the rows. Several values are only stored for systems that
// read them later: the volumes (no audio yet), the defence-window assist (combat) and reduce-flashes
// (the style controller). No story text anywhere in here.
//
// Three accessibility options are also story flags, so the script can read them: settingsOptionFlags()
// lists each with its flag id and value, and the story owner copies them into the story's flags (a
// script that doesn't declare one just doesn't get it). QTE auto-complete plays quick-time events by
// themselves, hold toggle turns every hold input into a single press, and skip choices is only
// mirrored to its flag for now.
//
//   ftcore::Settings s;                        // defaults
//   ftcore::decodeSettings(text, s);           // missing fields keep their defaults, values are clamped
//   std::string text = ftcore::encodeSettings(s);
//
//   ftcore::SettingsMenu menu;
//   switch (menu.press(s, ftcore::SettingsKey::Right)) { case ftcore::SettingsAction::Close: ... }
//   char value[24]; ftcore::settingsRowValue(s, menu.selected(), value, sizeof value);
//
// The menu never allocates; settingsRowValue writes into the caller's buffer.

#include "ftcore/Api.h"
#include "ftcore/CameraPresetId.h"
#include "ftcore/Skip.h"
#include "ftcore/UiFirstUse.h"
#include "ftcore/UiGlyphs.h"
#include "ftcore/UiStyle.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ftcore {

enum class TextSpeed : std::uint8_t { Slow, Normal, Fast, Instant };
enum class SubtitleSize : std::uint8_t { Small, Medium, Large };
// Off: the combat stream's normal defence window. Wide: a more forgiving one.
enum class DefenceAssist : std::uint8_t { Off, Wide };

struct Settings {
    static constexpr int kVersion = 1;
    static constexpr float kMinAutoDelay = 0.25f, kMaxAutoDelay = 5.f;
    static constexpr int kMinHudScale = 80, kMaxHudScale = 130; // percent, in steps of 10
    static constexpr int kMinHudSafeMargin = 0, kMaxHudSafeMargin = 10; // percent of the viewport, in steps of 1
    static constexpr int kMinTextScale = 100, kMaxTextScale = 200; // percent, in steps of 10
    static constexpr float kMinSubtitleScale = 0.85f, kMaxSubtitleScale = 2.f;

    TextSpeed textSpeed = TextSpeed::Normal;
    bool autoAdvance = false;
    float autoDelay = 1.5f; // seconds a fully shown line waits in auto-advance
    SkipMode skipMode = SkipMode::Seen;
    float master = 1.f, music = 0.8f, voice = 1.f, sfx = 1.f; // 0..1
    DefenceAssist defenceAssist = DefenceAssist::Off;
    bool aimAssist = true;        // the arena's aimed crosshair turns slower over a body (AimStance's aimAssistTurn)
    SubtitleSize subtitleSize = SubtitleSize::Medium;
    bool reduceFlashes = false;
    bool qteAutoComplete = false; // quick-time events play themselves
    bool holdToggle = false;      // a hold input is one press instead of keeping it down
    bool skipChoices = false;     // mirrored to its story flag only, for now
    bool stickFlicks = false;     // the Turns arrow combos also take the left stick's flicks (StickFlick.h)
    float cameraShake = 1.f;      // 0..1: how hard the arena camera kicks when a blow lands (0: not at all)
    bool lockFraming = true;      // the followed arena view orbits to put the target on its centre line (ArenaLook.h)
    CameraPresetId cameraPreset = CameraPresetId::Classic; // the arena camera's lens (CameraPreset.h): view only, never
                                  // a rule of the fight
    std::uint32_t uiFirstUse = 0; // what the fight HUD has taught this save: its counts, packed by uiFirstUseSaved (UiFirstUse.h)
    int hudScale = 100;           // the fight HUD's size over the viewport's scale, margins included: percent
    int hudSafeMargin = 0;        // more room at the fight HUD's edges: percent of the viewport's width (left and right)
                                  // and height (top and bottom), on top of the platform's title-safe padding (uiHudMargins)
    ShowBasicControls showBasicControls = ShowBasicControls::FirstFights; // the fight HUD's basic control prompts: the
                                  // teaching policy (UiFirstUse.h), not a rule of the fight
    int textScale = 100;          // the new UI's text size: percent of the style's type ramp (uiTextPx's textScale)
    UiPalette palette = UiPalette::Default; // the colour-vision or high-contrast token set every UI colour comes from
    bool reduceMotion = false;    // UI movement (rises, slides, wobbles) stills to fades and cuts
    UiGlyphMode glyphMode = UiGlyphMode::Auto; // button prompts: follow the last device used, or always keys or pad

    // Characters per second the dialogue types out at (0: whole text at once).
    float textCharsPerSecond() const {
        switch (textSpeed) {
        case TextSpeed::Slow: return 22.f;
        case TextSpeed::Normal: return 45.f;
        case TextSpeed::Fast: return 90.f;
        case TextSpeed::Instant: break;
        }
        return 0.f;
    }
    // Multiplier for the dialogue text size.
    float subtitleScale() const {
        switch (subtitleSize) {
        case SubtitleSize::Small: return 0.85f;
        case SubtitleSize::Medium: break;
        case SubtitleSize::Large: return 1.3f;
        }
        return 1.f;
    }

    // The text size as a factor (1.0..2.0) for uiTextPx.
    float uiTextScale() const { return static_cast<float>(textScale) / 100.f; }
    // The subtitle size: the text size times the subtitle size row, kept within 0.85..2.0.
    float subtitleTextScale() const {
        const float k = uiTextScale() * subtitleScale();
        return k < kMinSubtitleScale ? kMinSubtitleScale : k > kMaxSubtitleScale ? kMaxSubtitleScale : k;
    }

    bool operator==(const Settings&) const = default;
};

// Puts every value in range (an enum past its last value becomes the last one; NaN becomes the default).
FTCORE_API void clampSettings(Settings& s);

// ---- JSON text ----
// {"version":1,"textSpeed":1,"autoAdvance":false,"autoDelay":1.5,"skipMode":1,"master":1.0,
//  "music":0.8,"voice":1.0,"sfx":1.0,"defenceAssist":0,"aimAssist":true,"subtitleSize":1,"reduceFlashes":false,
//  "qteAutoComplete":false,"holdToggle":false,"skipChoices":false,"stickFlicks":false,"cameraShake":1.0,
//  "lockFraming":true,"cameraPreset":0,"uiFirstUse":0,"hudScale":100,"hudSafeMargin":0,"showBasicControls":0,
//  "textScale":100,"palette":0,"reduceMotion":false,"glyphMode":0}
// Enums are written as their number (the order above: textSpeed slow..instant, skipMode all/seen,
// defenceAssist off/wide, subtitleSize small..large, cameraPreset classic/close, showBasicControls first
// fights/always/never, palette default/protan/deutan/tritan/high contrast, glyphMode auto/keys/pad).
FTCORE_API std::string encodeSettings(const Settings& s);
// Reads settings text. Needs valid JSON with an integer "version" from 1 to Settings::kVersion;
// otherwise false (`*error` says why) and `out` is untouched. A missing or wrong-typed field keeps
// its default, an out-of-range value is clamped, unknown extra fields are ignored.
FTCORE_API bool decodeSettings(std::string_view text, Settings& out, std::string* error = nullptr);

// ---- the menu ----

enum class SettingsRow : std::uint8_t {
    TextSpeed,
    AutoAdvance,
    AutoDelay,
    SkipMode,
    Master,
    Music,
    Voice,
    Sfx,
    DefenceAssist,
    AimAssist,
    SubtitleSize,
    ReduceFlashes,
    QteAutoComplete,
    HoldToggle,
    SkipChoices,
    StickFlicks,
    CameraShake,
    LockFraming,
    CameraPreset,
    HudScale,
    HudSafeMargin,
    ShowBasicControls,
    TextScale,
    Palette,
    ReduceMotion,
    GlyphMode,
    Back,
};
inline constexpr std::uint32_t kSettingsRowCount = 27;

// The settings screen's sections, in their order on screen. Every row but Back is in exactly one.
enum class SettingsGroup : std::uint8_t { Text, Sound, Play, Camera, Display, Access };
inline constexpr std::uint32_t kSettingsGroupCount = 6;
// The section a row is in (Back: Text, the first; the screen draws it apart).
FTCORE_API SettingsGroup settingsRowGroup(SettingsRow row);
// "Text", "Sound", ...; "" for an out-of-range group.
FTCORE_API const char* settingsGroupLabel(SettingsGroup group);
// The rows in the order the screen shows them: section by section, each section's rows in SettingsRow order, Back
// last. settingsScreenRow(i) is the i-th (Back past the end); settingsScreenIndex is its inverse. The menu's Up / Down
// walk this order.
FTCORE_API SettingsRow settingsScreenRow(std::uint32_t index);
FTCORE_API std::uint32_t settingsScreenIndex(SettingsRow row);

enum class SettingsKey : std::uint8_t { Up, Down, Left, Right, Confirm };

enum class SettingsAction : std::uint8_t {
    None,    // nothing happened (an edge, or a value already at its limit)
    Moved,   // the selection changed
    Changed, // a value changed: apply and save
    Close,   // Back was picked
};

// "Text speed", "Auto-advance", ... ; "" for an out-of-range row.
FTCORE_API const char* settingsRowLabel(SettingsRow row);
// Writes the row's value ("Normal", "On", "70%", "1.5 s") into buf, always NUL-terminated when
// cap > 0; returns its length. Empty for Back.
FTCORE_API std::size_t settingsRowValue(const Settings& s, SettingsRow row, char* buf, std::size_t cap);
// Steps the row's value by dir (-1 or +1). With wrap, an enum or toggle goes round; a slider always
// stops at its ends, and without wrap so does every row (a toggle just sets itself). True when the
// value changed.
FTCORE_API bool adjustSetting(Settings& s, SettingsRow row, int dir, bool wrap);

// ---- options the story can read ----

struct OptionFlag {
    const char* id; // the story flag (a literal)
    bool on;
};
inline constexpr std::size_t kOptionFlagCount = 3;
// The array type is instantiated here, before the exported function returns it by value: MSVC warns (C4686) that a
// specialization first met in a return type may change the return calling convention.
static_assert(sizeof(std::array<OptionFlag, kOptionFlagCount>) == kOptionFlagCount * sizeof(OptionFlag));
// QTE auto-complete, hold toggle and skip choices, as F_SYS_OPT_* flag ids with their values. The
// flags are booleans; a script that declares one gets it set from here at the start of the story,
// after a load and whenever the setting changes.
FTCORE_API std::array<OptionFlag, kOptionFlagCount> settingsOptionFlags(const Settings& s);

class SettingsMenu {
public:
    SettingsRow selected() const { return selected_; }
    void select(SettingsRow row) { selected_ = static_cast<std::uint8_t>(row) < kSettingsRowCount ? row : SettingsRow::TextSpeed; }
    void reset() { selected_ = SettingsRow::TextSpeed; }

    // Up / Down move in screen order (settingsScreenRow; no wrap). Left / Right step the selected value. Confirm cycles it (wrapping), or
    // closes on Back.
    FTCORE_API SettingsAction press(Settings& s, SettingsKey key);
    // A mouse click on `row`: selects it, then acts like Left (dir < 0) or like Confirm (otherwise).
    FTCORE_API SettingsAction click(Settings& s, SettingsRow row, int dir);

private:
    SettingsRow selected_ = SettingsRow::TextSpeed;
};

} // namespace ftcore
