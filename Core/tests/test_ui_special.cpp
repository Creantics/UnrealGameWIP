#include "ftcore/UiSpecial.h"

#include "TestAssets.h"

#include <doctest/doctest.h>

#include <limits>
#include <string>

#include <type_traits>

using namespace ftcore;

TEST_CASE("special prompt: the rules' answer carries no level (bools and a reason only)") {
    // No float, int or count may reach the HUD from the gauge: the input is exactly a bool and a reason.
    static_assert(sizeof(UiSpecialInput) == 2 * sizeof(bool) + sizeof(UiSpecialWhy), "ready, why and the Third's ready");
    static_assert(std::is_same_v<decltype(UiSpecialInput::thirdReady), bool>);
    static_assert(std::is_same_v<decltype(UiSpecialInput::ready), bool>);
    static_assert(std::is_same_v<std::underlying_type_t<UiSpecialWhy>, std::uint8_t>);
    CHECK(kUiSpecialWhyCount == 4);
}

TEST_CASE("special prompt: switch off draws as today") {
    for (int p = 0; p < static_cast<int>(kUiPromptCount); ++p) {
        const auto pr = static_cast<UiPrompt>(p);
        CHECK(uiPromptRowAt(pr, false) == uiPromptRow(pr));
    }
    UiSpecialState s;
    const UiSpecialView v = uiSpecialStep(s, false, UiSpecialInput{true, UiSpecialWhy::None}, 0.016f, false);
    CHECK_FALSE(v.row);
    CHECK_FALSE(v.drawn);
    CHECK_FALSE(s.ready);
}

TEST_CASE("special prompt: the reserved row sits above the fight's own prompts, below the basic controls") {
    CHECK(uiPromptRowAt(UiPrompt::Heal, true) == 0);
    CHECK(uiPromptRowAt(UiPrompt::GiftHeal, true) == 1);
    CHECK(uiPromptRowAt(UiPrompt::GiftGuard, true) == 1);
    CHECK(uiPromptRowAt(UiPrompt::Pair, true) == 2);
    CHECK(kUiSpecialRow == 3);
    CHECK(uiPromptRowAt(UiPrompt::Attack, true) == 4);
    CHECK(uiPromptRowAt(UiPrompt::Jump, true) == kUiPromptRows);
    for (int p = 0; p < static_cast<int>(kUiPromptCount); ++p) {
        CHECK(uiPromptRowAt(static_cast<UiPrompt>(p), true) != kUiSpecialRow);
    }
}

TEST_CASE("special prompt: reserved and not ready keeps the row, draws nothing") {
    UiSpecialState s;
    const UiSpecialView v = uiSpecialStep(s, true, UiSpecialInput{}, 0.016f, false);
    CHECK(v.row);
    CHECK_FALSE(v.drawn);
    CHECK(v.pulse < 0.f);
}

TEST_CASE("special prompt: arrival fades in, pulses once, then stands steady until used") {
    UiSpecialState s;
    const UiSpecialInput ready{true, UiSpecialWhy::None};
    UiSpecialView v = uiSpecialStep(s, true, ready, 0.016f, false);
    CHECK(v.drawn);
    CHECK(v.live);
    CHECK(v.alpha == doctest::Approx(0.f));
    CHECK(v.pulse == doctest::Approx(0.f));
    v = uiSpecialStep(s, true, ready, 0.1f, false);
    CHECK(v.alpha == doctest::Approx(0.1f / kUiSpecialFadeIn));
    CHECK(v.pulse == doctest::Approx(0.1f / kUiSpecialPulse));
    v = uiSpecialStep(s, true, ready, 0.45f, false);
    CHECK(v.alpha == doctest::Approx(1.f));
    CHECK(v.pulse < 0.f);
    // Steady: the same view however long it stands (it never rests or fades).
    const UiSpecialView steady = v;
    for (int i = 0; i < 2000; ++i) v = uiSpecialStep(s, true, ready, 0.05f, false);
    CHECK(v.drawn == steady.drawn);
    CHECK(v.live == steady.live);
    CHECK(v.alpha == steady.alpha);
    CHECK(v.pulse == steady.pulse);
    // Used: it leaves over the fade-out; ready again: a new arrival with its own pulse.
    v = uiSpecialStep(s, true, UiSpecialInput{}, 0.016f, false);
    CHECK(v.drawn);
    v = uiSpecialStep(s, true, UiSpecialInput{}, kUiSpecialFadeOut, false);
    CHECK_FALSE(v.drawn);
    v = uiSpecialStep(s, true, ready, 0.016f, false);
    CHECK(v.pulse == doctest::Approx(0.f));
}

TEST_CASE("special prompt: unready (a lead switch) leaves over 0.2 s as last shown, no pulse; Ready returns it pulsing") {
    UiSpecialState s;
    const UiSpecialInput ready{true, UiSpecialWhy::None, false};
    UiSpecialView v = uiSpecialStep(s, true, ready, 0.016f, false);
    v = uiSpecialStep(s, true, ready, 1.f, false);
    REQUIRE(v.alpha == doctest::Approx(1.f));
    REQUIRE(v.pulse < 0.f);
    // The unready: the leave starts at full, held live, never pulsing, falling linearly.
    v = uiSpecialStep(s, true, UiSpecialInput{}, 0.016f, false);
    CHECK(v.drawn);
    CHECK(v.live);
    CHECK_FALSE(v.call);
    CHECK(v.alpha == doctest::Approx(1.f));
    CHECK(v.pulse < 0.f);
    v = uiSpecialStep(s, true, UiSpecialInput{}, 0.1f, false);
    CHECK(v.alpha == doctest::Approx(0.5f));
    CHECK(v.pulse < 0.f);
    v = uiSpecialStep(s, true, UiSpecialInput{}, 0.1f, false);
    CHECK_FALSE(v.drawn);
    v = uiSpecialStep(s, true, UiSpecialInput{}, 1.f, false);
    CHECK_FALSE(v.drawn);
    // Ready again: fades in from 0 and pulses.
    v = uiSpecialStep(s, true, ready, 0.016f, false);
    CHECK(v.alpha == doctest::Approx(0.f));
    CHECK(v.pulse == doctest::Approx(0.f));
}

TEST_CASE("special prompt: Ready mid-leave climbs from the alpha it had and pulses again") {
    UiSpecialState s;
    const UiSpecialInput ready{true, UiSpecialWhy::None, false};
    (void)uiSpecialStep(s, true, ready, 0.016f, false);
    (void)uiSpecialStep(s, true, ready, 1.f, false);
    (void)uiSpecialStep(s, true, UiSpecialInput{}, 0.016f, false);
    UiSpecialView v = uiSpecialStep(s, true, UiSpecialInput{}, 0.1f, false);
    REQUIRE(v.alpha == doctest::Approx(0.5f));
    v = uiSpecialStep(s, true, ready, 0.016f, false);
    CHECK(v.alpha == doctest::Approx(0.5f));
    CHECK(v.pulse == doctest::Approx(0.f));
    v = uiSpecialStep(s, true, ready, 0.5f * kUiSpecialFadeIn, false);
    CHECK(v.alpha == doctest::Approx(1.f));
}

TEST_CASE("special prompt: reduce motion and the switch off cut the leave") {
    for (int off = 0; off < 2; ++off) {
        UiSpecialState s;
        const UiSpecialInput ready{true, UiSpecialWhy::None, false};
        (void)uiSpecialStep(s, true, ready, 0.016f, off == 0);
        (void)uiSpecialStep(s, true, ready, 1.f, off == 0);
        const UiSpecialView v = off == 0 ? uiSpecialStep(s, true, UiSpecialInput{}, 0.016f, true)
                                         : uiSpecialStep(s, false, UiSpecialInput{}, 0.016f, false);
        CHECK_FALSE(v.drawn);
        CHECK_FALSE(s.leaving);
    }
}

TEST_CASE("special prompt: only the Third's ready reads CALL, live, with its pulse; the lead's ready takes the row back") {
    UiSpecialState s;
    const UiSpecialInput third{false, UiSpecialWhy::None, true};
    UiSpecialView v = uiSpecialStep(s, true, third, 0.016f, false);
    CHECK(v.drawn);
    CHECK(v.call);
    CHECK(v.live);
    CHECK(v.pulse == doctest::Approx(0.f));
    // The lead's why does not dim his call (only the lead's own row takes a reason).
    v = uiSpecialStep(s, true, UiSpecialInput{false, UiSpecialWhy::NoTarget, true}, 1.f, false);
    CHECK(v.call);
    CHECK(v.live);
    // Both ready: the lead's row (SPECIAL and the lead's name; a hold calls the Third's), no second pulse.
    v = uiSpecialStep(s, true, UiSpecialInput{true, UiSpecialWhy::None, true}, 0.016f, false);
    CHECK_FALSE(v.call);
    CHECK(v.live);
    CHECK(v.pulse < 0.f);
    // The call used, the lead's still ready: the lead's row stands.
    v = uiSpecialStep(s, true, UiSpecialInput{true, UiSpecialWhy::None, false}, 0.016f, false);
    CHECK(v.drawn);
    CHECK_FALSE(v.call);
    // Nothing ready after a call: it leaves as CALL.
    s = UiSpecialState{};
    (void)uiSpecialStep(s, true, third, 0.016f, false);
    (void)uiSpecialStep(s, true, third, 1.f, false);
    v = uiSpecialStep(s, true, UiSpecialInput{}, 0.016f, false);
    CHECK(v.drawn);
    CHECK(v.call);
}

TEST_CASE("special prompt: no target dims with a reason and one outline; the pulse waits for the first live tick") {
    UiSpecialState s;
    UiSpecialView v = uiSpecialStep(s, true, UiSpecialInput{true, UiSpecialWhy::NoTarget}, 0.016f, false);
    CHECK(v.drawn);
    CHECK_FALSE(v.live);
    CHECK(v.why == UiSpecialWhy::NoTarget);
    CHECK(v.pulse < 0.f);
    v = uiSpecialStep(s, true, UiSpecialInput{true, UiSpecialWhy::OutOfSight}, 1.f, false);
    CHECK(v.why == UiSpecialWhy::OutOfSight);
    CHECK(v.alpha == doctest::Approx(1.f));
    v = uiSpecialStep(s, true, UiSpecialInput{true, UiSpecialWhy::None}, 0.1f, false);
    CHECK(v.live);
    CHECK(v.why == UiSpecialWhy::None);
    CHECK(v.pulse == doctest::Approx(0.f));
    // Dim again and live again: no second pulse.
    v = uiSpecialStep(s, true, UiSpecialInput{true, UiSpecialWhy::NotNow}, 0.6f, false);
    CHECK(v.pulse < 0.f);
    v = uiSpecialStep(s, true, UiSpecialInput{true, UiSpecialWhy::None}, 0.1f, false);
    CHECK(v.pulse < 0.f);
}

TEST_CASE("special prompt: reduce motion cuts in with no pulse") {
    UiSpecialState s;
    const UiSpecialView v = uiSpecialStep(s, true, UiSpecialInput{true, UiSpecialWhy::None}, 0.016f, true);
    CHECK(v.drawn);
    CHECK(v.alpha == doctest::Approx(1.f));
    CHECK(v.pulse < 0.f);
}

TEST_CASE("special prompt: bad time steps hold the clock") {
    UiSpecialState s;
    const UiSpecialInput ready{true, UiSpecialWhy::None};
    (void)uiSpecialStep(s, true, ready, 0.f, false);
    UiSpecialView v = uiSpecialStep(s, true, ready, -1.f, false);
    CHECK(v.alpha == doctest::Approx(0.f));
    v = uiSpecialStep(s, true, ready, std::numeric_limits<float>::quiet_NaN(), false);
    CHECK(v.alpha == doctest::Approx(0.f));
}

TEST_CASE("special prompt: the style's switch (the row per world on); the old open-choice switches are gone") {
    std::string err;
    std::string text;
    REQUIRE(ftcore_test::readProjectData("UI/style.json", text));
    UiStyle shipped;
    REQUIRE_MESSAGE(importUiStyle(text, shipped, &err), err);
    for (const UiWorldRules& r : shipped.rules) CHECK(r.specialPrompt); // the owner's pick, C (2026-11-17)
    for (const UiWorldRules& r : defaultUiStyle().rules) CHECK_FALSE(r.specialPrompt);
    UiStyle bad = defaultUiStyle();
    CHECK_FALSE(importUiStyle(R"({"special": {"third_call": true}})", bad, &err)); // ruled 2026-11-17: no switch
    CHECK_FALSE(importUiStyle(R"({"worlds": {"lattice": {"special_prompt": "yes"}}})", bad, &err));
}

TEST_CASE("special prompt: reduce flashes drops the pulse only; the fade stays") {
    UiSpecialState s;
    const UiSpecialInput ready{true, UiSpecialWhy::None};
    UiSpecialView v = uiSpecialStep(s, true, ready, 0.016f, false, true);
    CHECK(v.drawn);
    CHECK(v.pulse < 0.f);
    v = uiSpecialStep(s, true, ready, 0.1f, false, true);
    CHECK(v.alpha == doctest::Approx(0.1f / kUiSpecialFadeIn));
    CHECK(v.pulse < 0.f);
}
