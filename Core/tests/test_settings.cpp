// Settings: defaults, JSON text with clamping, and the menu's rules.

#include "ftcore/Settings.h"

#include "AllocCounter.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cstring>
#include <string>

using namespace ftcore;

TEST_CASE("settings defaults") {
    const Settings s;
    CHECK(s.textSpeed == TextSpeed::Normal);
    CHECK(s.textCharsPerSecond() == 45.f);
    CHECK_FALSE(s.autoAdvance);
    CHECK(s.skipMode == SkipMode::Seen);
    CHECK(s.master == 1.f);
    CHECK(s.defenceAssist == DefenceAssist::Off);
    CHECK(s.subtitleSize == SubtitleSize::Medium);
    CHECK(s.subtitleScale() == 1.f);
    CHECK_FALSE(s.reduceFlashes);

    Settings t;
    t.textSpeed = TextSpeed::Instant;
    CHECK(t.textCharsPerSecond() == 0.f);
    t.textSpeed = TextSpeed::Slow;
    CHECK(t.textCharsPerSecond() < s.textCharsPerSecond());
    t.textSpeed = TextSpeed::Fast;
    CHECK(t.textCharsPerSecond() > s.textCharsPerSecond());
    t.subtitleSize = SubtitleSize::Small;
    CHECK(t.subtitleScale() < 1.f);
    t.subtitleSize = SubtitleSize::Large;
    CHECK(t.subtitleScale() > 1.f);
}

TEST_CASE("settings round-trip through JSON") {
    Settings s;
    s.textSpeed = TextSpeed::Fast;
    s.autoAdvance = true;
    s.autoDelay = 2.75f;
    s.skipMode = SkipMode::All;
    s.master = 0.3f;
    s.music = 0.f;
    s.voice = 0.7f;
    s.sfx = 0.1f;
    s.defenceAssist = DefenceAssist::Wide;
    s.subtitleSize = SubtitleSize::Large;
    s.reduceFlashes = true;
    const std::string text = encodeSettings(s);
    CHECK(text.find("\"version\":1") != std::string::npos);

    Settings out;
    std::string err;
    REQUIRE_MESSAGE(decodeSettings(text, out, &err), err);
    CHECK(out == s);

    Settings defaults;
    REQUIRE(decodeSettings(encodeSettings(defaults), out, &err));
    CHECK(out == defaults);
}

TEST_CASE("settings: missing and wrong-typed fields keep their defaults") {
    Settings out;
    out.textSpeed = TextSpeed::Instant; // overwritten by the decode, not left over
    out.master = 0.2f;
    std::string err;
    REQUIRE_MESSAGE(decodeSettings(R"({"version":1})", out, &err), err);
    CHECK(out == Settings{});

    out.master = 0.2f;
    REQUIRE(decodeSettings(R"({"version":1,"textSpeed":"fast","autoAdvance":1,"autoDelay":"x","master":true,
                              "music":0.5,"future":{"a":[1,2]}})", out, &err));
    CHECK(out.textSpeed == TextSpeed::Normal);
    CHECK_FALSE(out.autoAdvance);
    CHECK(out.autoDelay == Settings{}.autoDelay);
    CHECK(out.master == 1.f);
    CHECK(out.music == 0.5f);
}

TEST_CASE("settings: out-of-range values are clamped") {
    Settings out;
    std::string err;
    REQUIRE_MESSAGE(decodeSettings(R"({"version":1,"textSpeed":9,"skipMode":-4,"autoDelay":99,"master":2.5,
                                       "music":-1,"voice":1e9,"sfx":0.25,"defenceAssist":5,"subtitleSize":7})",
                                   out, &err),
                    err);
    CHECK(out.textSpeed == TextSpeed::Instant);   // past the last: the last
    CHECK(out.skipMode == Settings{}.skipMode);   // negative: the default
    CHECK(out.autoDelay == Settings::kMaxAutoDelay);
    CHECK(out.master == 1.f);
    CHECK(out.music == 0.f);
    CHECK(out.voice == 1.f);
    CHECK(out.sfx == 0.25f);
    CHECK(out.defenceAssist == DefenceAssist::Wide);
    CHECK(out.subtitleSize == SubtitleSize::Large);

    REQUIRE(decodeSettings(R"({"version":1,"autoDelay":0.01})", out, &err));
    CHECK(out.autoDelay == Settings::kMinAutoDelay);

    Settings wild;
    wild.master = -3.f;
    wild.autoDelay = 1000.f;
    wild.textSpeed = static_cast<TextSpeed>(200);
    clampSettings(wild);
    CHECK(wild.master == 0.f);
    CHECK(wild.autoDelay == Settings::kMaxAutoDelay);
    CHECK(wild.textSpeed == TextSpeed::Instant);
}

TEST_CASE("settings: bad text and versions are refused and leave the value alone") {
    Settings keep;
    keep.sfx = 0.4f;
    const char* bad[] = {
        "", "garbage", "[]", "null", "{}",
        R"({"version":"1"})", R"({"version":1.5})", R"({"version":0})", R"({"version":2})", R"({"version":-1})",
        R"({"textSpeed":1})", R"({"version":1)",
    };
    for (const char* text : bad) {
        Settings out = keep;
        std::string err;
        INFO(text);
        CHECK_FALSE(decodeSettings(text, out, &err));
        CHECK_FALSE(err.empty());
        CHECK(out == keep);
    }
    Settings out;
    CHECK_FALSE(decodeSettings("{", out, nullptr)); // no error sink: must not crash
}

TEST_CASE("settings menu: moving, stepping and cycling") {
    Settings s;
    SettingsMenu m;
    CHECK(m.selected() == SettingsRow::TextSpeed);
    CHECK(m.press(s, SettingsKey::Up) == SettingsAction::None); // top: no wrap

    // Right steps without wrapping.
    CHECK(m.press(s, SettingsKey::Right) == SettingsAction::Changed);
    CHECK(s.textSpeed == TextSpeed::Fast);
    CHECK(m.press(s, SettingsKey::Right) == SettingsAction::Changed);
    CHECK(s.textSpeed == TextSpeed::Instant);
    CHECK(m.press(s, SettingsKey::Right) == SettingsAction::None);
    // Confirm wraps.
    CHECK(m.press(s, SettingsKey::Confirm) == SettingsAction::Changed);
    CHECK(s.textSpeed == TextSpeed::Slow);
    CHECK(m.press(s, SettingsKey::Left) == SettingsAction::None); // already the first
    CHECK(s.textSpeed == TextSpeed::Slow);

    CHECK(m.press(s, SettingsKey::Down) == SettingsAction::Moved);
    CHECK(m.selected() == SettingsRow::AutoAdvance);
    CHECK(m.press(s, SettingsKey::Confirm) == SettingsAction::Changed); // toggles
    CHECK(s.autoAdvance);
    CHECK(m.press(s, SettingsKey::Left) == SettingsAction::Changed);   // Left sets it off
    CHECK_FALSE(s.autoAdvance);
    CHECK(m.press(s, SettingsKey::Left) == SettingsAction::None);
    CHECK(m.press(s, SettingsKey::Right) == SettingsAction::Changed);  // Right sets it on
    CHECK(s.autoAdvance);

    // Down to the end: Back closes, and the last row has no row below it.
    for (std::uint32_t i = 2; i < kSettingsRowCount; ++i) CHECK(m.press(s, SettingsKey::Down) == SettingsAction::Moved);
    CHECK(m.selected() == SettingsRow::Back);
    CHECK(m.press(s, SettingsKey::Down) == SettingsAction::None);
    CHECK(m.press(s, SettingsKey::Left) == SettingsAction::None);
    CHECK(m.press(s, SettingsKey::Confirm) == SettingsAction::Close);
}

TEST_CASE("settings menu: sliders step by a tenth and stop at their ends") {
    Settings s;
    s.master = 0.f;
    SettingsMenu m;
    m.select(SettingsRow::Master);
    for (int i = 0; i < 10; ++i) CHECK(m.press(s, SettingsKey::Right) == SettingsAction::Changed);
    CHECK(s.master == 1.f); // exactly, not 0.99999
    CHECK(m.press(s, SettingsKey::Right) == SettingsAction::None);
    CHECK(m.press(s, SettingsKey::Confirm) == SettingsAction::None); // a slider does not wrap
    CHECK(s.master == 1.f);
    for (int i = 0; i < 10; ++i) CHECK(m.press(s, SettingsKey::Left) == SettingsAction::Changed);
    CHECK(s.master == 0.f);
    CHECK(m.press(s, SettingsKey::Left) == SettingsAction::None);

    // A stepped value survives the JSON round trip bit for bit.
    s.music = 0.f;
    for (int i = 0; i < 3; ++i) adjustSetting(s, SettingsRow::Music, 1, false);
    Settings out;
    REQUIRE(decodeSettings(encodeSettings(s), out, nullptr));
    CHECK(out.music == s.music);
    CHECK(out == s);

    // The auto-advance delay moves in quarter seconds within its limits.
    m.select(SettingsRow::AutoDelay);
    s.autoDelay = 1.5f;
    CHECK(m.press(s, SettingsKey::Right) == SettingsAction::Changed);
    CHECK(s.autoDelay == 1.75f);
    for (int i = 0; i < 40; ++i) m.press(s, SettingsKey::Right);
    CHECK(s.autoDelay == Settings::kMaxAutoDelay);
    for (int i = 0; i < 40; ++i) m.press(s, SettingsKey::Left);
    CHECK(s.autoDelay == Settings::kMinAutoDelay);
}

TEST_CASE("settings menu: every row has a label and a value, and every enum row cycles") {
    Settings s;
    SettingsMenu m;
    char value[32];
    for (std::uint32_t i = 0; i < kSettingsRowCount; ++i) {
        const SettingsRow row = static_cast<SettingsRow>(i);
        CHECK(std::strlen(settingsRowLabel(row)) > 0);
        const std::size_t n = settingsRowValue(s, row, value, sizeof value);
        CHECK(n == std::strlen(value));
        CHECK((n == 0) == (row == SettingsRow::Back));
    }
    CHECK(std::strlen(settingsRowLabel(static_cast<SettingsRow>(200))) == 0);

    settingsRowValue(s, SettingsRow::Master, value, sizeof value);
    CHECK(std::string(value) == "100%");
    settingsRowValue(s, SettingsRow::Music, value, sizeof value);
    CHECK(std::string(value) == "80%");
    settingsRowValue(s, SettingsRow::AutoDelay, value, sizeof value);
    CHECK(std::string(value) == "1.50 s");
    settingsRowValue(s, SettingsRow::TextSpeed, value, sizeof value);
    CHECK(std::string(value) == "Normal");

    // A tiny buffer is cut and terminated, not overrun.
    char tiny[4];
    CHECK(settingsRowValue(s, SettingsRow::TextSpeed, tiny, sizeof tiny) == 3);
    CHECK(std::string(tiny) == "Nor");
    CHECK(settingsRowValue(s, SettingsRow::TextSpeed, tiny, 0) == 0);

    // Confirm on each enum row comes back round to where it started.
    for (SettingsRow row : {SettingsRow::TextSpeed, SettingsRow::SkipMode, SettingsRow::DefenceAssist, SettingsRow::SubtitleSize}) {
        const Settings before = s;
        m.select(row);
        int presses = 0;
        do {
            CHECK(m.press(s, SettingsKey::Confirm) == SettingsAction::Changed);
            ++presses;
        } while (!(s == before) && presses < 10);
        CHECK(presses >= 2);
        CHECK(presses <= 4);
    }
}

TEST_CASE("settings menu: a click selects the row, then steps or cycles it") {
    Settings s;
    SettingsMenu m;
    CHECK(m.click(s, SettingsRow::ReduceFlashes, 1) == SettingsAction::Changed);
    CHECK(m.selected() == SettingsRow::ReduceFlashes);
    CHECK(s.reduceFlashes);
    CHECK(m.click(s, SettingsRow::ReduceFlashes, -1) == SettingsAction::Changed); // left half: off
    CHECK_FALSE(s.reduceFlashes);
    CHECK(m.click(s, SettingsRow::Back, 1) == SettingsAction::Close);
    CHECK(m.click(s, static_cast<SettingsRow>(99), 1) == SettingsAction::None);
    CHECK(m.selected() == SettingsRow::Back);
}

TEST_CASE("settings menu allocates nothing") {
    Settings s;
    SettingsMenu m;
    char value[32];
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    std::size_t total = 0;
    for (int round = 0; round < 200; ++round) {
        for (SettingsKey k : {SettingsKey::Down, SettingsKey::Right, SettingsKey::Confirm, SettingsKey::Left, SettingsKey::Up})
            m.press(s, k);
        total += settingsRowValue(s, m.selected(), value, sizeof value);
        total += std::strlen(settingsRowLabel(m.selected()));
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(total > 0);
    if (counting) CHECK(after == before);
}
