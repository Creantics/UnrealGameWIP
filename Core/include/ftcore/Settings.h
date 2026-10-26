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
#include "ftcore/Skip.h"

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

    TextSpeed textSpeed = TextSpeed::Normal;
    bool autoAdvance = false;
    float autoDelay = 1.5f; // seconds a fully shown line waits in auto-advance
    SkipMode skipMode = SkipMode::Seen;
    float master = 1.f, music = 0.8f, voice = 1.f, sfx = 1.f; // 0..1
    DefenceAssist defenceAssist = DefenceAssist::Off;
    SubtitleSize subtitleSize = SubtitleSize::Medium;
    bool reduceFlashes = false;
    bool qteAutoComplete = false; // quick-time events play themselves
    bool holdToggle = false;      // a hold input is one press instead of keeping it down
    bool skipChoices = false;     // mirrored to its story flag only, for now

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

    bool operator==(const Settings&) const = default;
};

// Puts every value in range (an enum past its last value becomes the last one; NaN becomes the default).
FTCORE_API void clampSettings(Settings& s);

// ---- JSON text ----
// {"version":1,"textSpeed":1,"autoAdvance":false,"autoDelay":1.5,"skipMode":1,"master":1.0,
//  "music":0.8,"voice":1.0,"sfx":1.0,"defenceAssist":0,"subtitleSize":1,"reduceFlashes":false,
//  "qteAutoComplete":false,"holdToggle":false,"skipChoices":false}
// Enums are written as their number (the order above: textSpeed slow..instant, skipMode all/seen,
// defenceAssist off/wide, subtitleSize small..large).
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
    SubtitleSize,
    ReduceFlashes,
    QteAutoComplete,
    HoldToggle,
    SkipChoices,
    Back,
};
inline constexpr std::uint32_t kSettingsRowCount = 15;

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
// QTE auto-complete, hold toggle and skip choices, as F_SYS_OPT_* flag ids with their values. The
// flags are booleans; a script that declares one gets it set from here at the start of the story,
// after a load and whenever the setting changes.
FTCORE_API std::array<OptionFlag, kOptionFlagCount> settingsOptionFlags(const Settings& s);

class SettingsMenu {
public:
    SettingsRow selected() const { return selected_; }
    void select(SettingsRow row) { selected_ = static_cast<std::uint8_t>(row) < kSettingsRowCount ? row : SettingsRow::TextSpeed; }
    void reset() { selected_ = SettingsRow::TextSpeed; }

    // Up / Down move (no wrap). Left / Right step the selected value. Confirm cycles it (wrapping), or
    // closes on Back.
    FTCORE_API SettingsAction press(Settings& s, SettingsKey key);
    // A mouse click on `row`: selects it, then acts like Left (dir < 0) or like Confirm (otherwise).
    FTCORE_API SettingsAction click(Settings& s, SettingsRow row, int dir);

private:
    SettingsRow selected_ = SettingsRow::TextSpeed;
};

} // namespace ftcore
