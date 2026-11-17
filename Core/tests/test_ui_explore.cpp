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

TEST_CASE("subtitle band: none over a dark picture, the least up to 70% that holds 4.5:1") {
    const UiColor ink{0.95f, 0.94f, 0.9f, 1.f};
    float bare = 0.f, banded = 0.f;
    CHECK(uiSubtitleBand(ink, UiColor{0.05f, 0.05f, 0.06f, 1.f}, &bare, &banded) == 0.f);
    CHECK(bare >= kUiSubtitleMinContrast);
    // Solhaven daylight: the bare line at about 1.8; the old 35% band left it near 4.0, the measured one holds it.
    const UiColor day{0.8f, 0.78f, 0.7f, 1.f};
    const float band = uiSubtitleBand(ink, day, &bare, &banded);
    CHECK(bare < kUiSubtitleMinContrast);
    CHECK(band > 0.35f);
    CHECK(band <= kUiSubtitleBandMax);
    CHECK(banded >= kUiSubtitleMinContrast);
    CHECK(banded < kUiSubtitleMinContrast + 0.05f); // the least: no darker than it needs
    // A paler picture needs more band.
    CHECK(uiSubtitleBand(ink, UiColor{0.9f, 0.9f, 0.88f, 1.f}) > band);
    // Over white the cap: whatever it gives is reported, never more than the cap.
    CHECK(uiSubtitleBand(ink, UiColor{1.f, 1.f, 1.f, 1.f}, &bare, &banded) <= kUiSubtitleBandMax);
    CHECK(uiSubtitleBand(ink, UiColor{0.2f, 0.2f, 0.2f, 1.f}) == 0.f); // the pointers are optional
}

TEST_CASE("least backing: the dialogue's option dim is the same search") {
    const UiColor ink{0.95f, 0.94f, 0.9f, 1.f};
    const UiColor black{0.f, 0.f, 0.f, 1.f};
    const UiColor grey{0.6f, 0.6f, 0.6f, 1.f};
    const float a = uiLeastBacking(ink, black, grey, 4.5f, 0.f, 1.f);
    CHECK(a > 0.f);
    CHECK(uiContrast(ink, UiColor{0.f, 0.f, 0.f, a}, grey) >= 4.5f);
    CHECK(uiContrast(ink, UiColor{0.f, 0.f, 0.f, a - 0.01f}, grey) < 4.5f);
    CHECK(uiLeastBacking(ink, black, grey, 4.5f, 0.9f, 1.f) == doctest::Approx(0.9f)); // already holds at `from`
    CHECK(uiLeastBacking(ink, black, UiColor{1.f, 1.f, 1.f, 1.f}, 30.f, 0.f, 0.5f) == doctest::Approx(0.5f)); // short: the cap
}

TEST_CASE("line guard: the contour is judged by its mean, so today's ring does not pass a bright picture") {
    const UiColor ink{0.95f, 0.94f, 0.9f, 1.f};
    const float restA = 0.54f; // the edge colour's 0.9 times the words' 0.6 share
    CHECK(uiContourMean(1.f, restA) == doctest::Approx(restA * 1.5f / kUiContourWindow));
    CHECK(uiContourMean(3.f, kUiContourAlphaMax) == doctest::Approx(kUiContourAlphaMax * 3.5f / kUiContourWindow));
    CHECK(uiContourMean(kUiContourWindow, 1.f) == doctest::Approx(1.f));
    CHECK(uiContourMean(2.f, 0.6f) == doctest::Approx(0.6f * 2.5f / kUiContourWindow));
    // Pictures under which the bare objective (3.30) and notice (2.24) were measured in Solhaven daylight.
    for (const float v : {0.55f, 0.72f}) {
        const UiColor pic{v, v, v * 0.95f, 1.f};
        const float bare = uiContrast(ink, UiColor{0.f, 0.f, 0.f, 0.f}, pic);
        CHECK(bare < 4.5f);
        CHECK(uiContrast(ink, UiColor{0.f, 0.f, 0.f, uiContourMean(1.f, restA)}, pic) < 4.5f); // today's: a fail
        const UiLineGuard g = uiLineGuard(ink, pic, kUiLineMinContrast, 1.f, restA);
        CHECK(g.bare == doctest::Approx(bare));
        CHECK(g.ratio >= kUiLineMinContrast);
        CHECK(uiContourMean(g.spread, g.alpha) > uiContourMean(1.f, restA)); // the contour had to grow
        CHECK(g.alpha <= kUiContourAlphaMax);
        CHECK(g.shade == 0.f);
    }
    // A dark picture keeps the resting contour.
    const UiLineGuard dark = uiLineGuard(ink, UiColor{0.1f, 0.1f, 0.12f, 1.f}, kUiLineMinContrast, 1.f, restA);
    CHECK(dark.spread == 1.f);
    CHECK(dark.alpha == doctest::Approx(restA));
    CHECK(dark.shade == 0.f);
    // Names hold 3:1 with less.
    const UiColor pic{0.72f, 0.72f, 0.68f, 1.f};
    const UiLineGuard name = uiLineGuard(ink, pic, kUiNameMinContrast, 1.f, restA);
    const UiLineGuard small = uiLineGuard(ink, pic, kUiLineMinContrast, 1.f, restA);
    CHECK(uiContourMean(name.spread, name.alpha) <= uiContourMean(small.spread, small.alpha));
}

TEST_CASE("line guard: a dim ink needs the shade under the line; the quiet floor rises above 40%") {
    const UiColor dim{0.95f, 0.94f, 0.9f, 0.6f};
    const UiColor white{1.f, 1.f, 1.f, 1.f};
    const UiLineGuard g = uiLineGuard(dim, white, kUiLineMinContrast, 1.f, 0.54f);
    CHECK(g.spread == kUiContourSpreadMax);
    CHECK(g.alpha == doctest::Approx(kUiContourAlphaMax));
    CHECK(g.shade > 0.f);
    CHECK(g.shade <= kUiLineShadeMax);
    const UiColor ink{0.95f, 0.94f, 0.9f, 1.f};
    // 40% ink cannot hold 4.5:1 even over black (at most about 3.6).
    CHECK(uiContrast(UiColor{ink.r, ink.g, ink.b, kUiObjectiveQuiet}, UiColor{0.f, 0.f, 0.f, 1.f}, white) < 4.5f);
    const UiColor day{0.8f, 0.78f, 0.7f, 1.f};
    const float q = uiQuietFloor(ink, day, kUiLineMinContrast, kUiObjectiveQuiet);
    CHECK(q > kUiObjectiveQuiet);
    CHECK(q < 1.f);
    // Even over a dark picture 40% falls short, so the floor sits a little above it there, and lower than in daylight.
    const float qDark = uiQuietFloor(ink, UiColor{0.1f, 0.1f, 0.12f, 1.f}, kUiLineMinContrast, kUiObjectiveQuiet);
    CHECK(qDark > kUiObjectiveQuiet);
    CHECK(qDark < q);
    CHECK(uiQuietFloor(ink, UiColor{0.1f, 0.1f, 0.12f, 1.f}, 3.f, kUiObjectiveQuiet) == doctest::Approx(kUiObjectiveQuiet));
    const UiLineGuard rest = uiLineGuard(UiColor{ink.r, ink.g, ink.b, q}, day, kUiLineMinContrast, 1.f, 0.54f);
    CHECK(rest.ratio >= kUiLineMinContrast);
    CHECK(rest.shade == 0.f);
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
        (void)uiLineGuard(UiColor{1.f, 1.f, 1.f, 1.f}, UiColor{0.7f, 0.7f, 0.7f, 1.f}, 4.5f, 1.f, 0.54f);
        (void)uiQuietFloor(UiColor{1.f, 1.f, 1.f, 1.f}, UiColor{0.7f, 0.7f, 0.7f, 1.f}, 4.5f, 0.4f);
    }
    if (counting) CHECK(ftcore_test::allocStats().count == before);
}

// ---- the read skip (2026-11-17) ----

namespace {
UiReadView readView(float x = 100.f, float yaw = 10.f) {
    UiReadView v;
    v.x0 = x;
    v.y0 = 900.f;
    v.x1 = x + 400.f;
    v.y1 = 940.f;
    v.px = 1000.f;
    v.py = -200.f;
    v.pz = 150.f;
    v.yaw = yaw;
    v.pitch = -10.f;
    return v;
}
const UiColor kGrey{0.5f, 0.5f, 0.5f, 1.f};
} // namespace

TEST_CASE("uiReadSkip: never before two real reads") {
    UiReadMemo m;
    CHECK_FALSE(uiReadSkip(m, readView(), 0.0));
    uiReadNote(m, readView(), kGrey, 1.0);
    CHECK_FALSE(uiReadSkip(m, readView(), 1.5));
    uiReadNote(m, readView(), kGrey, 2.0);
    CHECK(uiReadSkip(m, readView(), 2.5));
}

TEST_CASE("uiReadSkip: a still view with a steady picture skips until the forced read") {
    UiReadMemo m;
    uiReadNote(m, readView(), kGrey, 1.0);
    uiReadNote(m, readView(), UiColor{0.51f, 0.51f, 0.51f, 1.f}, 2.0);
    CHECK(uiReadSkip(m, readView(), 2.0 + kUiReadSkipForceS - 0.1));
    CHECK_FALSE(uiReadSkip(m, readView(), 2.0 + kUiReadSkipForceS));
    CHECK_FALSE(uiReadSkip(m, readView(), 1.0)); // time ran back: read
}

TEST_CASE("uiReadSkip: a changing picture, a moved box or a moving camera reads") {
    UiReadMemo m;
    uiReadNote(m, readView(), UiColor{0.4f, 0.4f, 0.4f, 1.f}, 1.0);
    uiReadNote(m, readView(), kGrey, 2.0);
    CHECK_FALSE(uiReadSkip(m, readView(), 2.5)); // 0.4 to 0.5 is over 5%

    uiReadNote(m, readView(), kGrey, 3.0);
    REQUIRE(uiReadSkip(m, readView(), 3.5));
    CHECK(uiReadSkip(m, readView(100.f + kUiReadSkipBoxPx), 3.5));
    CHECK_FALSE(uiReadSkip(m, readView(100.f + 2.f * kUiReadSkipBoxPx), 3.5));
    CHECK_FALSE(uiReadSkip(m, readView(100.f, 10.f + 2.f * kUiReadSkipTurnDeg), 3.5));
    UiReadView moved = readView();
    moved.py += 2.f * kUiReadSkipMove;
    CHECK_FALSE(uiReadSkip(m, moved, 3.5));
    UiReadView tilted = readView();
    tilted.pitch += 2.f * kUiReadSkipTurnDeg;
    CHECK_FALSE(uiReadSkip(m, tilted, 3.5));
}

TEST_CASE("uiReadSkip: the turn is the short way round") {
    UiReadMemo m;
    uiReadNote(m, readView(100.f, 179.9f), kGrey, 1.0);
    uiReadNote(m, readView(100.f, 179.9f), kGrey, 2.0);
    CHECK(uiReadSkip(m, readView(100.f, -179.9f), 2.5));
    CHECK(uiReadSkip(m, readView(100.f, 179.9f + 360.f), 2.5));
}

TEST_CASE("uiReadSkip: dark pictures agree by the absolute margin") {
    UiReadMemo m;
    uiReadNote(m, readView(), UiColor{0.010f, 0.010f, 0.010f, 1.f}, 1.0);
    uiReadNote(m, readView(), UiColor{0.015f, 0.015f, 0.015f, 1.f}, 2.0);
    CHECK(uiReadSkip(m, readView(), 2.5)); // 50% apart, but under kUiReadSkipLumaAbs
}

TEST_CASE("uiReadSkip: non-finite input never skips and a bad note forgets") {
    UiReadMemo m;
    uiReadNote(m, readView(), kGrey, 1.0);
    uiReadNote(m, readView(), kGrey, 2.0);
    UiReadView bad = readView();
    bad.yaw = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(uiReadSkip(m, bad, 2.5));
    CHECK_FALSE(uiReadSkip(m, readView(), std::numeric_limits<double>::infinity()));
    uiReadNote(m, readView(), UiColor{std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f, 1.f}, 3.0);
    CHECK_FALSE(m.valid);
    CHECK_FALSE(uiReadSkip(m, readView(), 3.5));
}

TEST_CASE("uiReadSkip and uiReadNote do not allocate") {
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    UiReadMemo m;
    for (int i = 0; i < 100; ++i) {
        uiReadNote(m, readView(), kGrey, i);
        (void)uiReadSkip(m, readView(), i + 0.5);
    }
    if (counting) CHECK(ftcore_test::allocStats().count == before);
}
