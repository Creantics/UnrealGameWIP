#include "ftcore/UiHudFit.h"

#include "AllocCounter.h"

#include <ostream>
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>
#include <string_view>

using namespace ftcore;

namespace {
// A stand-in for the host's measure: every character `cw` px wide.
std::size_t measured(std::u16string_view text, float cw, std::array<UiHudWord, kUiHudMaxWords>& w) {
    const std::size_t n = uiHudWords(text, w.data(), w.size());
    for (std::size_t i = 0; i < n; ++i) w[i].w = static_cast<float>(w[i].end - w[i].begin) * cw;
    return n;
}
std::u16string_view lineText(std::u16string_view text, const UiHudFit& f, std::size_t i) {
    return text.substr(f.line[i].begin, f.line[i].end - f.line[i].begin);
}
// The label face's capitals run about 0.6 of the em a character with the HUD's tracking.
constexpr float kCharEm = 0.6f;
} // namespace

TEST_CASE("HUD text scale: each slot grows with the scale and the viewport, never shrinks") {
    for (std::size_t s = 0; s < kUiHudSlotCount; ++s) {
        const UiHudSlot slot = static_cast<UiHudSlot>(s);
        const float base = kUiHudSlots[s].px;
        CHECK(uiHudTextPx(slot, 1080.f, 1.f) == doctest::Approx(base));
        CHECK(uiHudTextPx(slot, 1080.f, 2.f) == doctest::Approx(2.f * base));
        CHECK(uiHudTextPx(slot, 2160.f, 1.5f) == doctest::Approx(3.f * base));
        // Out of range: under 100% (or NaN) reads as 100%, over 200% as 200%.
        CHECK(uiHudTextPx(slot, 1080.f, 0.5f) == doctest::Approx(base));
        CHECK(uiHudTextPx(slot, 1080.f, std::numeric_limits<float>::quiet_NaN()) == doctest::Approx(base));
        CHECK(uiHudTextPx(slot, 1080.f, 3.f) == doctest::Approx(2.f * base));
        // Never below the floor, even in a tiny viewport.
        CHECK(uiHudTextPx(slot, 200.f, 1.f) >= kUiHudMinPx);
        CHECK(uiHudTextPx(slot, 0.f, 1.f) >= kUiHudMinPx);
        // Rising with the scale in the settings' steps, whatever the viewport.
        for (float h : {720.f, 1080.f, 1440.f, 2160.f}) {
            float prev = 0.f;
            for (int pct = 100; pct <= 200; pct += 10) {
                const float px = uiHudTextPx(slot, h, static_cast<float>(pct) / 100.f);
                CHECK(px >= prev);
                prev = px;
            }
        }
    }
    // The table matches the HUD's own sizes and shapes: names never break, the rest show two lines.
    CHECK(uiHudSlotRule(UiHudSlot::LeadName).px == 38.f);
    CHECK(uiHudSlotRule(UiHudSlot::LeadName).lines == 1);
    CHECK(uiHudSlotRule(UiHudSlot::PartnerName).lines == 1);
    CHECK(uiHudSlotRule(UiHudSlot::Objective).lines == 2);
    CHECK(uiHudSlotRule(UiHudSlot::Notice).lines == 2);
    CHECK(uiHudSlotRule(UiHudSlot::BossName).lines == 2);
    CHECK(&uiHudSlotRule(static_cast<UiHudSlot>(200)) == &kUiHudSlots[0]);
}

TEST_CASE("HUD text scale: words split at gaps, the cap runs the last span on") {
    std::array<UiHudWord, 4> w{};
    const std::u16string_view t = u"  Find a  way\tacross ";
    CHECK(uiHudWords(t, w.data(), w.size()) == 4);
    CHECK((t.substr(w[0].begin, w[0].end - w[0].begin) == u"Find"));
    CHECK((t.substr(w[1].begin, w[1].end - w[1].begin) == u"a"));
    CHECK((t.substr(w[2].begin, w[2].end - w[2].begin) == u"way"));
    CHECK((t.substr(w[3].begin, w[3].end - w[3].begin) == u"across"));

    std::array<UiHudWord, 2> two{};
    const std::u16string_view u = u"one two three four ";
    CHECK(uiHudWords(u, two.data(), two.size()) == 2);
    CHECK((u.substr(two[1].begin, two[1].end - two[1].begin) == u"two three four"));

    CHECK(uiHudWords(u"   ", w.data(), w.size()) == 0);
    CHECK(uiHudWords(u"", w.data(), w.size()) == 0);
    CHECK(uiHudWords(u"word", nullptr, 4) == 0);
    CHECK(uiHudWords(u"word", w.data(), 0) == 0);
}

TEST_CASE("HUD text scale: the objective fits whole at 100% and breaks at its middle at 200%, same size") {
    const std::u16string_view obj = u"Find a way across the canal";
    std::array<UiHudWord, kUiHudMaxWords> w{};
    const UiHudSlotRule& r = uiHudSlotRule(UiHudSlot::Objective);

    for (float scale : {1.f, 2.f}) {
        const float px = uiHudTextPx(UiHudSlot::Objective, 1080.f, scale);
        const std::size_t n = measured(obj, kCharEm * px, w);
        UiHudFit f;
        uiHudFit(w.data(), n, kCharEm * px, r.room, r.lines, f);
        CAPTURE(scale);
        CHECK_FALSE(f.over);
        CHECK(f.widest <= r.room);
        if (scale == 1.f) {
            REQUIRE(f.count == 1);
            CHECK((lineText(obj, f, 0) == obj));
        } else {
            // 27 characters at 58 px: about 940 px, over the 720 px room. Never shrunk: two lines, broken where the wider half is least.
            REQUIRE(f.count == 2);
            CHECK(f.shown == 2);
            CHECK(uiHudPages(f) == 1);
            CHECK((lineText(obj, f, 0) == u"Find a way"));
            CHECK((lineText(obj, f, 1) == u"across the canal"));
            CHECK(f.w[0] == doctest::Approx(10.f * kCharEm * px));
            CHECK(f.w[1] == doctest::Approx(16.f * kCharEm * px));
        }
    }
}

TEST_CASE("HUD text scale: two lines break at the gap nearest the middle") {
    // Six even words: the break falls after the third.
    std::array<UiHudWord, 6> w{};
    for (std::size_t i = 0; i < w.size(); ++i) w[i] = UiHudWord{static_cast<std::uint32_t>(4 * i), static_cast<std::uint32_t>(4 * i + 3), 30.f};
    UiHudFit f;
    uiHudFit(w.data(), w.size(), 10.f, 120.f, 2, f); // whole: 6*30 + 5*10 = 230
    REQUIRE(f.count == 2);
    CHECK(f.line[0].end == w[2].end);
    CHECK(f.line[1].begin == w[3].begin);
    CHECK(f.w[0] == doctest::Approx(110.f));
    CHECK(f.w[1] == doctest::Approx(110.f));

    // An uneven pair: a long first word pulls the break left of the word count's middle.
    std::array<UiHudWord, 4> u{{{0, 10, 100.f}, {11, 13, 20.f}, {14, 16, 20.f}, {17, 19, 20.f}}};
    uiHudFit(u.data(), u.size(), 10.f, 110.f, 2, f); // 100 | 20 20 20 = 80: max 100
    REQUIRE(f.count == 2);
    CHECK(f.line[0].end == 10);
    CHECK(f.w[1] == doctest::Approx(80.f));
}

TEST_CASE("HUD text scale: a name never breaks, it runs over whole") {
    std::array<UiHudWord, 2> w{{{0, 5, 300.f}, {6, 12, 320.f}}};
    UiHudFit f;
    uiHudFit(w.data(), w.size(), 20.f, 560.f, uiHudSlotRule(UiHudSlot::LeadName).lines, f);
    REQUIRE(f.count == 1);
    CHECK(f.shown == 1);
    CHECK(f.over);
    CHECK(f.w[0] == doctest::Approx(640.f));
    CHECK(f.line[0].begin == 0);
    CHECK(f.line[0].end == 12);
}

TEST_CASE("HUD text scale: past two lines a window pages through them") {
    // Ten words of 100 px, 10 px gaps, 230 px room: two words a line, five lines, three pages of two.
    std::array<UiHudWord, 10> w{};
    for (std::size_t i = 0; i < w.size(); ++i) w[i] = UiHudWord{static_cast<std::uint32_t>(3 * i), static_cast<std::uint32_t>(3 * i + 2), 100.f};
    UiHudFit f;
    uiHudFit(w.data(), w.size(), 10.f, 230.f, 2, f);
    REQUIRE(f.count == 5);
    CHECK(f.shown == 2);
    CHECK_FALSE(f.over);
    for (std::size_t i = 0; i < static_cast<std::size_t>(f.count); ++i) CHECK(f.w[i] <= 230.f);
    CHECK(uiHudPages(f) == 3);
    CHECK(uiHudPageLine(f, 0.f) == 0);
    CHECK(uiHudPageLine(f, kUiHudPageTime - 0.01f) == 0);
    CHECK(uiHudPageLine(f, kUiHudPageTime + 0.01f) == 2);
    CHECK(uiHudPageLine(f, 2.f * kUiHudPageTime + 0.01f) == 4);
    CHECK(uiHudPageLine(f, 3.f * kUiHudPageTime + 0.01f) == 0); // round again
    CHECK(uiHudPageLine(f, -1.f) == 0);
    CHECK(uiHudPageLine(f, std::numeric_limits<float>::quiet_NaN()) == 0);
    CHECK(uiHudPageLine(f, 1e9f) < static_cast<std::size_t>(f.count));
}

TEST_CASE("HUD text scale: a word wider than the room stands alone, whole") {
    std::array<UiHudWord, 3> w{{{0, 2, 40.f}, {3, 20, 400.f}, {21, 23, 40.f}}};
    UiHudFit f;
    uiHudFit(w.data(), w.size(), 10.f, 200.f, 2, f);
    REQUIRE(f.count == 3);
    CHECK(f.line[1].begin == 3);
    CHECK(f.line[1].end == 20);
    CHECK(f.w[1] == doctest::Approx(400.f));
    CHECK(f.over);
    CHECK(f.widest == doctest::Approx(400.f));
}

TEST_CASE("HUD text scale: past the line cap the rest joins the last line") {
    std::array<UiHudWord, 12> w{};
    for (std::size_t i = 0; i < w.size(); ++i) w[i] = UiHudWord{static_cast<std::uint32_t>(3 * i), static_cast<std::uint32_t>(3 * i + 2), 100.f};
    UiHudFit f;
    uiHudFit(w.data(), w.size(), 10.f, 100.f, 2, f); // one word a line: 12 lines wanted, kUiHudMaxLines held
    REQUIRE(static_cast<std::size_t>(f.count) == kUiHudMaxLines);
    CHECK(f.line[kUiHudMaxLines - 1].begin == w[kUiHudMaxLines - 1].begin);
    CHECK(f.line[kUiHudMaxLines - 1].end == w[11].end);
    CHECK(f.over);
}

TEST_CASE("HUD text scale: nothing to fit") {
    UiHudFit f;
    f.count = 3;
    uiHudFit(nullptr, 0, 10.f, 100.f, 2, f);
    CHECK(f.count == 0);
    CHECK(f.shown == 0);
    CHECK(uiHudPages(f) == 0);
    CHECK(uiHudPageLine(f, 5.f) == 0);
    // A slot of zero lines reads as one.
    std::array<UiHudWord, 2> w{{{0, 3, 100.f}, {4, 7, 100.f}}};
    uiHudFit(w.data(), w.size(), 10.f, 50.f, 0, f);
    CHECK(f.count == 1);
    CHECK(f.over);
}

TEST_CASE("HUD text scale: no allocation") {
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    std::array<UiHudWord, kUiHudMaxWords> w{};
    const std::u16string_view t = u"Reach the lighthouse before the tide turns back";
    float sink = 0.f;
    for (int pct = 100; pct <= 200; pct += 10) {
        const float px = uiHudTextPx(UiHudSlot::Notice, 1080.f, static_cast<float>(pct) / 100.f);
        const std::size_t n = measured(t, kCharEm * px, w);
        UiHudFit f;
        uiHudFit(w.data(), n, kCharEm * px, uiHudSlotRule(UiHudSlot::Notice).room, 2, f);
        sink += f.widest + static_cast<float>(uiHudPageLine(f, 3.f));
    }
    CHECK(sink > 0.f);
    if (counting) CHECK(ftcore_test::allocStats().count == before);
}
