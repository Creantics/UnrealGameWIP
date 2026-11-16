#include "ftcore/UiExplore.h"

#include "AllocCounter.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <limits>
#include <string>

using namespace ftcore;

namespace {

std::u16string_view lineOf(std::u16string_view t, const UiSubtitleFit& f, std::size_t i) {
    return t.substr(f.line[i].begin, f.line[i].end - f.line[i].begin);
}

std::size_t cps(std::u16string_view s) {
    std::size_t n = 0;
    for (char16_t c : s) n += (c >= 0xDC00 && c <= 0xDFFF) ? 0 : 1;
    return n;
}

} // namespace

TEST_CASE("objective: full for 4 s, then quiet at 40%, back up on a change") {
    UiObjectiveFade f;
    uiObjectiveStep(f, 7, 0.f);
    for (int i = 0; i < 30; ++i) uiObjectiveStep(f, 7, 0.1f);
    CHECK(f.alpha == doctest::Approx(1.f)); // 3 s in
    for (int i = 0; i < 9; ++i) uiObjectiveStep(f, 7, 0.1f);
    CHECK(f.alpha == doctest::Approx(1.f)); // 3.9 s
    for (int i = 0; i < 20; ++i) uiObjectiveStep(f, 7, 0.1f);
    CHECK(f.alpha == doctest::Approx(kUiObjectiveQuiet)); // 5.9 s
    for (int i = 0; i < 100; ++i) uiObjectiveStep(f, 7, 0.1f);
    CHECK(f.alpha == doctest::Approx(kUiObjectiveQuiet)); // stays quiet
    uiObjectiveStep(f, 8, 0.1f);                          // a change: rising, never a jump
    CHECK(f.alpha > kUiObjectiveQuiet);
    CHECK(f.alpha < 1.f);
    for (int i = 0; i < 3; ++i) uiObjectiveStep(f, 8, 0.1f);
    CHECK(f.alpha == doctest::Approx(1.f));
}

TEST_CASE("objective: a bad dt changes nothing") {
    UiObjectiveFade f;
    uiObjectiveStep(f, 1, 0.f);
    uiObjectiveStep(f, 1, std::numeric_limits<float>::quiet_NaN());
    uiObjectiveStep(f, 1, -5.f);
    CHECK(f.age == 0.f);
}

TEST_CASE("notices: one at a time, 2.5 s each, in order") {
    UiNoticeQueue q;
    CHECK(uiNoticeShown(q) == -1);
    CHECK(uiNoticeAlpha(q) == 0.f);
    const auto a = uiNoticePush(q);
    const auto b = uiNoticePush(q);
    CHECK(a != b);
    CHECK(uiNoticeShown(q) == a);
    uiNoticeStep(q, 1.f);
    CHECK(uiNoticeAlpha(q) == doctest::Approx(1.f));
    uiNoticeStep(q, 1.4f); // 2.4 s: fading out
    CHECK(uiNoticeAlpha(q) < 1.f);
    CHECK(uiNoticeAlpha(q) > 0.f);
    CHECK(uiNoticeShown(q) == a);
    uiNoticeStep(q, 0.1f); // 2.5 s: the next
    CHECK(uiNoticeShown(q) == b);
    CHECK(uiNoticeAlpha(q) == 0.f); // fades in from nothing
    uiNoticeStep(q, kUiNoticeTime);
    CHECK(uiNoticeShown(q) == -1);
}

TEST_CASE("notices: a queue of 3; when full the last waiting gives way, the shown one finishes") {
    UiNoticeQueue q;
    const auto a = uiNoticePush(q);
    const auto b = uiNoticePush(q);
    const auto c = uiNoticePush(q);
    CHECK(a != b);
    CHECK(b != c);
    CHECK(a != c);
    CHECK(c < kUiNoticeQueue);
    const auto d = uiNoticePush(q);
    CHECK(d == c); // the host writes the new words over c's
    CHECK(q.count == kUiNoticeQueue);
    CHECK(uiNoticeShown(q) == a);
    uiNoticeStep(q, kUiNoticeTime);
    CHECK(uiNoticeShown(q) == b);
    const auto e = uiNoticePush(q); // a's slot is free again
    CHECK(e == a);
}

TEST_CASE("subtitle fit: 42 characters a line at most, broken at spaces") {
    const std::u16string t = u"Keep to the lit side of the canal tonight. The lamps by the old bridge went out an hour ago.";
    UiSubtitleFit f;
    uiSubtitleFit(t, f);
    REQUIRE(f.count >= 2);
    for (std::size_t i = 0; i < f.count; ++i) {
        const auto l = lineOf(t, f, i);
        CHECK(cps(l) <= kUiSubtitleChars);
        CHECK(!l.empty());
        CHECK(l.front() != u' ');
        CHECK(l.back() != u' ');
    }
    CHECK(lineOf(t, f, 0) == u"Keep to the lit side of the canal tonight.");
    CHECK(uiSubtitlePages(f) == (f.count + 1) / 2);
}

TEST_CASE("subtitle fit: exactly 42 fits, 43 breaks") {
    const std::u16string t42(42, u'a');
    UiSubtitleFit f;
    uiSubtitleFit(t42, f);
    CHECK(f.count == 1);
    const std::u16string t = std::u16string(20, u'a') + u" " + std::u16string(21, u'b') + u" c"; // 42 then " c"
    uiSubtitleFit(t, f);
    REQUIRE(f.count == 2);
    CHECK(cps(lineOf(t, f, 0)) == 42);
    CHECK(lineOf(t, f, 1) == u"c");
}

TEST_CASE("subtitle fit: a word longer than a line stands alone, whole") {
    const std::u16string t = u"go " + std::u16string(50, u'x') + u" now";
    UiSubtitleFit f;
    uiSubtitleFit(t, f);
    REQUIRE(f.count == 3);
    CHECK(lineOf(t, f, 0) == u"go");
    CHECK(cps(lineOf(t, f, 1)) == 50);
    CHECK(lineOf(t, f, 2) == u"now");
}

TEST_CASE("subtitle fit: newlines break; surrogate pairs count as one character") {
    const std::u16string t = u"one\ntwo";
    UiSubtitleFit f;
    uiSubtitleFit(t, f);
    REQUIRE(f.count == 2);
    CHECK(lineOf(t, f, 0) == u"one");
    CHECK(lineOf(t, f, 1) == u"two");
    std::u16string wide;
    for (int i = 0; i < 42; ++i) wide += u"\U0001F600"; // 42 characters in 84 units
    uiSubtitleFit(wide, f);
    CHECK(f.count == 1);
    uiSubtitleFit(u"", f);
    CHECK(f.count == 0);
    uiSubtitleFit(u"   \n  ", f);
    CHECK(f.count == 0);
    CHECK(uiSubtitlePage(f, 10) == 0);
}

TEST_CASE("subtitle fit: past eight lines the rest joins the last; pages follow the reveal") {
    std::u16string t;
    for (int i = 0; i < 12; ++i) t += std::u16string(40, static_cast<char16_t>(u'a' + i)) + u" ";
    UiSubtitleFit f;
    uiSubtitleFit(t, f);
    CHECK(f.count == kUiSubtitleMaxLines);
    CHECK(f.line[kUiSubtitleMaxLines - 1].end >= static_cast<std::uint32_t>(t.size()) - 1);
    CHECK(uiSubtitlePage(f, 0) == 0);
    CHECK(uiSubtitlePage(f, f.line[1].begin + 1) == 0);
    CHECK(uiSubtitlePage(f, f.line[2].begin + 1) == 1);
    CHECK(uiSubtitlePage(f, f.line[5].begin + 1) == 2);
    CHECK(uiSubtitlePages(f) == 4);
}

TEST_CASE("subtitle band: none over a dark picture, 35% where the text falls below 4.5:1") {
    const UiColor ink{0.95f, 0.94f, 0.9f, 1.f};
    float bare = 0.f, banded = 0.f;
    CHECK(uiSubtitleBand(ink, UiColor{0.05f, 0.05f, 0.06f, 1.f}, &bare, &banded) == 0.f);
    CHECK(bare >= kUiSubtitleMinContrast);
    CHECK(uiSubtitleBand(ink, UiColor{0.8f, 0.78f, 0.7f, 1.f}, &bare, &banded) == doctest::Approx(kUiSubtitleBand));
    CHECK(bare < kUiSubtitleMinContrast);
    CHECK(banded > bare);
    CHECK(uiSubtitleBand(ink, UiColor{0.2f, 0.2f, 0.2f, 1.f}) == 0.f); // the pointers are optional
}

TEST_CASE("explore UI: no allocation") {
    const std::u16string t = u"Keep to the lit side of the canal tonight. The lamps went out an hour ago.";
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    UiObjectiveFade f;
    UiNoticeQueue q;
    UiSubtitleFit fit;
    for (int i = 0; i < 100; ++i) {
        uiObjectiveStep(f, static_cast<std::uint32_t>(i / 50), 0.016f);
        if (i % 20 == 0) (void)uiNoticePush(q);
        uiNoticeStep(q, 0.016f);
        (void)uiNoticeAlpha(q);
        uiSubtitleFit(t, fit);
        (void)uiSubtitlePage(fit, static_cast<std::uint32_t>(i));
        (void)uiSubtitleBand(UiColor{1.f, 1.f, 1.f, 1.f}, UiColor{0.5f, 0.5f, 0.5f, 1.f});
    }
    if (counting) CHECK(ftcore_test::allocStats().count == before);
}
