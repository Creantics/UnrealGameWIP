#include "ftcore/ArenaFight.h"
#include "ftcore/Dialogue.h"
#include "ftcore/InputBindings.h"
#include "ftcore/Load.h"
#include "ftcore/ReadCards.h"
#include "ftcore/Settings.h"
#include "ftcore/StoryDatabase.h"
#include "ftcore/StoryRunner.h"
#include "ftcore/UiGlyphs.h"
#include "ftcore/UiStyle.h"
#include "ftcore/UiViews.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <algorithm>
#include <cmath>
#include <ostream>
#include <doctest/doctest.h>

#include <string>

using namespace ftcore;

namespace {

void buildScript(StoryBuilder& b) {
    b.setStart("SC-U");
    b.beginChapter("01", "One");
    b.beginScene("SC-U", "U");
    b.direction("Open.");                  // 0
    b.line("PH-U-001", "SPK", "Hello.");   // 1
    b.choice("Pick?");                     // 2
    b.greyedOption("Greyed");
    b.option("Right", 3);
    b.line("PH-U-002", "OTHER", "Done.");  // 3
    b.end();
}

struct Script {
    StoryDatabase db;
    Script() {
        StoryBuilder b;
        buildScript(b);
        std::string error;
        REQUIRE_MESSAGE(b.finish(db, &error), error);
    }
};

} // namespace

// ---- style -------------------------------------------------------------------------------------------------------

TEST_CASE("ui style: the built-in style and Data/UI/style.json pass the accessibility rules") {
    std::string err;
    CHECK_MESSAGE(validateUiStyle(defaultUiStyle(), &err), err);
    std::string text;
    if (!ftcore_test::readProjectData("UI/style.json", text)) FAIL("Data/UI/style.json not found");
    UiStyle s;
    REQUIRE_MESSAGE(importUiStyle(text, s, &err), err);
    // The data file is the built-in style written out.
    const UiStyle d = defaultUiStyle();
    for (std::size_t w = 0; w < kUiWorldCount; ++w)
        for (std::size_t t = 0; t < kUiTokenCount; ++t) {
            const UiColor a = s.world[w][t];
            const UiColor b = d.world[w][t];
            CHECK(a.r == doctest::Approx(b.r).epsilon(0.003));
            CHECK(a.g == doctest::Approx(b.g).epsilon(0.003));
            CHECK(a.b == doctest::Approx(b.b).epsilon(0.003));
            CHECK(a.a == doctest::Approx(b.a).epsilon(0.003));
        }
    for (const auto& [a, b] : {std::pair{s.loadGold, d.loadGold}, std::pair{s.loadBright, d.loadBright}}) {
        CHECK(a.r == doctest::Approx(b.r).epsilon(0.003));
        CHECK(a.g == doctest::Approx(b.g).epsilon(0.003));
        CHECK(a.b == doctest::Approx(b.b).epsilon(0.003));
    }
    CHECK(std::string_view(s.plate[0].data()) == "bronze");
    for (std::size_t w = 0; w < kUiWorldCount; ++w) {
        CHECK(s.rules[w].marksAccent2 == d.rules[w].marksAccent2);
        CHECK(s.rules[w].dangerUnderline == d.rules[w].dangerUnderline);
    }
    CHECK(s.layers.hairlines);
    CHECK_FALSE(s.layers.worldAnchored);
}

TEST_CASE("ui style: text never draws below 18 px at 1080p or 14 px at any height; the player's scale is 1..2") {
    UiStyle s = defaultUiStyle();
    s.text[static_cast<std::size_t>(UiTextRole::Label)].px = 12.f; // a role set too small is lifted to the floor
    CHECK(uiTextPx(s, UiTextRole::Label, 1080.f, 1.f) == doctest::Approx(18.f));
    CHECK(uiTextPx(s, UiTextRole::Label, 2160.f, 1.f) == doctest::Approx(36.f));
    CHECK(uiTextPx(s, UiTextRole::Label, 720.f, 1.f) == doctest::Approx(14.f)); // 12 by height, lifted to 14
    CHECK(uiTextPx(s, UiTextRole::Subtitle, 1080.f, 0.5f) == doctest::Approx(36.f)); // scale below 1 is 1
    CHECK(uiTextPx(s, UiTextRole::Subtitle, 1080.f, 3.f) == doctest::Approx(72.f));  // above 2 is 2
    CHECK(uiLinePx(s, 1080.f) == doctest::Approx(3.f));
    CHECK(uiLinePx(s, 200.f) == doctest::Approx(1.f));
}

TEST_CASE("ui style: contrast is measured on the backing over the worst picture; weak text is refused") {
    const UiColor white{1.f, 1.f, 1.f, 1.f};
    const UiColor black{0.f, 0.f, 0.f, 1.f};
    CHECK(uiContrast(white, black, white) == doctest::Approx(21.f));
    CHECK(uiContrast(white, white, black) == doctest::Approx(1.f));

    std::string err;
    UiStyle s;
    // A dim grey label on a thin backing fails, and the import leaves the style as it was.
    const char* weak = R"({"worlds": {"lattice": {"ink_dim": "#808080"}}})";
    s = defaultUiStyle();
    s.lineWeight = 7.f;
    CHECK_FALSE(importUiStyle(weak, s, &err));
    CHECK(err.find("lattice/default: ink_dim") != std::string::npos);
    CHECK(s.lineWeight == 7.f);
    // Shapes: each meaning has its own.
    CHECK_FALSE(importUiStyle(R"({"cues": {"ally": "diamond"}})", s, &err));
    CHECK(err.find("share a cue shape") != std::string::npos);
    CHECK_FALSE(importUiStyle(R"({"cues": {"danger": "none"}})", s, &err));
    // Floors may not be lowered.
    CHECK_FALSE(importUiStyle(R"({"min_text_px": 12})", s, &err));
    // Unknown names are errors, not ignored.
    CHECK_FALSE(importUiStyle(R"({"colours": {}})", s, &err));
    CHECK_FALSE(importUiStyle(R"({"worlds": {"mars": {}}})", s, &err));
    CHECK_FALSE(importUiStyle(R"({"worlds": {"lattice": {"inc": "#FFFFFF"}}})", s, &err));
    CHECK_FALSE(importUiStyle(R"({"worlds": {"lattice": {"ink": "white"}}})", s, &err));
    CHECK_FALSE(importUiStyle(R"({"text": {"label": {"font": "comic"}}})", s, &err));
}

TEST_CASE("ui style: the serif faces parse by name; the verbs and numbers default to the small caps") {
    std::string err;
    UiStyle s = defaultUiStyle();
    CHECK(s.text[static_cast<std::size_t>(UiTextRole::Verb)].font == UiFont::SerifScBold);
    CHECK(s.text[static_cast<std::size_t>(UiTextRole::Verb)].upper); // one even height, as on the sheet
    CHECK(s.text[static_cast<std::size_t>(UiTextRole::Verb)].tracking >= 0.2f); // wide tracked
    CHECK(s.text[static_cast<std::size_t>(UiTextRole::Value)].font == UiFont::SerifScBold);
    CHECK(s.text[static_cast<std::size_t>(UiTextRole::Subtitle)].font == UiFont::Serif);
    REQUIRE(importUiStyle(R"({"text": {"label": {"font": "serif_sc"}, "title": {"font": "serif_bold"},
                                       "prompt": {"font": "sans_bold"}}})", s, &err));
    CHECK(s.text[static_cast<std::size_t>(UiTextRole::Label)].font == UiFont::SerifSc);
    CHECK(s.text[static_cast<std::size_t>(UiTextRole::Title)].font == UiFont::SerifBold);
    CHECK(s.text[static_cast<std::size_t>(UiTextRole::Prompt)].font == UiFont::SansBold);
    CHECK_FALSE(importUiStyle(R"({"text": {"label": {"font": "serif_caps"}}})", s, &err));
}

TEST_CASE("ui style: the HUD's margins are its own, the platform's safe padding and the player's percent, per side") {
    const UiMargins none;
    // A PC at 1080p with no padding and 0%: the HUD's own 64 px on every side.
    UiMargins m = uiHudMargins(1920.f, 1080.f, 64.f, none, 0);
    CHECK(m.left == doctest::Approx(64.f));
    CHECK(m.right == doctest::Approx(64.f));
    CHECK(m.top == doctest::Approx(64.f));
    CHECK(m.bottom == doctest::Approx(64.f));
    // 5% at 16:9: 96 px of the width at the sides, 54 px of the height at the top and bottom.
    m = uiHudMargins(1920.f, 1080.f, 64.f, none, 5);
    CHECK(m.left == doctest::Approx(64.f + 96.f));
    CHECK(m.right == doctest::Approx(64.f + 96.f));
    CHECK(m.top == doctest::Approx(64.f + 54.f));
    CHECK(m.bottom == doctest::Approx(64.f + 54.f));
    // 5% at 21:9 (2560x1080): the sides take 5% of the wider width, the top and bottom the same as 16:9.
    m = uiHudMargins(2560.f, 1080.f, 64.f, none, 5);
    CHECK(m.left == doctest::Approx(64.f + 128.f));
    CHECK(m.right == doctest::Approx(64.f + 128.f));
    CHECK(m.top == doctest::Approx(64.f + 54.f));
    CHECK(m.bottom == doctest::Approx(64.f + 54.f));
    // The platform's padding goes under the percent on its own side only.
    UiMargins pad;
    pad.left = 10.f;
    pad.top = 20.f;
    pad.right = 30.f;
    pad.bottom = 40.f;
    m = uiHudMargins(2560.f, 1080.f, 64.f, pad, 5);
    CHECK(m.left == doctest::Approx(64.f + 10.f + 128.f));
    CHECK(m.top == doctest::Approx(64.f + 20.f + 54.f));
    CHECK(m.right == doctest::Approx(64.f + 30.f + 128.f));
    CHECK(m.bottom == doctest::Approx(64.f + 40.f + 54.f));
    // The percent clamps to 0..10; bad input counts as nothing.
    CHECK(uiHudMargins(1920.f, 1080.f, 64.f, none, 40).left == doctest::Approx(64.f + 192.f));
    CHECK(uiHudMargins(1920.f, 1080.f, 64.f, none, -5).left == doctest::Approx(64.f));
    pad.left = -50.f;
    pad.top = std::nanf("");
    m = uiHudMargins(-1.f, 1080.f, 64.f, pad, 5);
    CHECK(m.left == doctest::Approx(64.f));
    CHECK(m.top == doctest::Approx(64.f + 54.f));
    CHECK(uiHudMargins(1920.f, 1080.f, std::nanf(""), none, 0).left == doctest::Approx(0.f));
    // The settings' own range is the one the HUD clamps to.
    CHECK(Settings::kMinHudSafeMargin == 0);
    CHECK(Settings::kMaxHudSafeMargin == 10);
}

TEST_CASE("ui style: text keeps a hairline dark edge; a faint or light edge is refused; new keys are optional") {
    std::string err;
    UiStyle s = defaultUiStyle();
    CHECK(validateUiStyle(s, &err));
    CHECK(uiEdgePx(s, 1080.f) == doctest::Approx(1.f));
    CHECK(uiEdgePx(s, 2160.f) == doctest::Approx(2.f));
    CHECK(uiEdgePx(s, 540.f) == doctest::Approx(1.f)); // never thinner than a pixel
    // A light edge leaves light text unreadable over a bright sky.
    CHECK_FALSE(importUiStyle(R"({"edge": {"color": "#C0C0C0E6"}})", s, &err));
    CHECK(err.find("on the edge") != std::string::npos);
    CHECK_FALSE(importUiStyle(R"({"edge": {"color": "#0A090840"}})", s, &err));
    CHECK_FALSE(importUiStyle(R"({"edge": {"px": 0.5}})", s, &err));
    CHECK_FALSE(importUiStyle(R"({"edge": {"width": 1}})", s, &err));
    CHECK_FALSE(importUiStyle(R"({"edge": {"text": 0.3}})", s, &err)); // a glyph edge that faint loses bright sky
    CHECK(err.find("edge text") != std::string::npos);
    CHECK_FALSE(importUiStyle(R"({"backing_soft": {"hud": 1.5}})", s, &err));
    CHECK_FALSE(importUiStyle(R"({"meter": "dial"})", s, &err));
    CHECK_FALSE(importUiStyle(R"({"load": {"gold": "amber"}})", s, &err));
    CHECK_FALSE(importUiStyle(R"({"load": {"silver": "#FFFFFF"}})", s, &err));
    CHECK_FALSE(importUiStyle(R"({"fade": {"rest_alpha": 2}})", s, &err));
    // Each key is optional; the pick swaps them in.
    s = defaultUiStyle();
    REQUIRE(importUiStyle(R"({"meter": "teeth", "backing_soft": {"hud": 0.3}, "edge": {"px": 2}, "fade": {"rest": 4}})", s, &err));
    CHECK(s.meter == UiMeterShape::Teeth);
    CHECK(uiMeterShapeName(s.meter) == "teeth");
    CHECK(s.backingSoftHud == doctest::Approx(0.3f));
    CHECK(s.backingSoftDialogue == doctest::Approx(defaultUiStyle().backingSoftDialogue));
    CHECK(s.edgePx == doctest::Approx(2.f));
    CHECK(defaultUiStyle().edgeText == doctest::Approx(0.6f));
    CHECK(s.fade.rest == doctest::Approx(4.f));
    CHECK(s.fade.in == doctest::Approx(defaultUiStyle().fade.in));
    // The Load's two golds are their own entries, read like any colour.
    REQUIRE(importUiStyle(R"({"load": {"bright": "#FFFFFF"}})", s, &err));
    CHECK(s.loadBright.b == doctest::Approx(1.f));
    CHECK(s.loadGold == defaultUiStyle().loadGold);
}

TEST_CASE("ui fade: a change shows the block at full, it rests dim after a while, and a new change wakes it") {
    UiFadeTimes t;
    t.in = 0.2f;
    t.rest = 1.f;
    t.out = 0.5f;
    t.restAlpha = 0.4f;
    UiFade f;
    uiFadeStep(f, true, 0.1f, t);
    CHECK(f.alpha == doctest::Approx(0.5f));
    uiFadeStep(f, false, 0.1f, t);
    CHECK(f.alpha == doctest::Approx(1.f));
    for (int i = 0; i < 8; ++i) uiFadeStep(f, false, 0.1f, t); // 1.0 s since the change
    CHECK(f.alpha == doctest::Approx(1.f));
    uiFadeStep(f, false, 0.25f, t); // half the way down
    CHECK(f.alpha == doctest::Approx(0.7f));
    for (int i = 0; i < 10; ++i) uiFadeStep(f, false, 0.1f, t);
    CHECK(f.alpha == doctest::Approx(0.4f)); // never below the rest level
    uiFadeStep(f, true, 0.1f, t);
    CHECK(f.alpha == doctest::Approx(0.9f));
    // From nothing with no changes: it rises only to the rest level.
    UiFade g;
    for (int i = 0; i < 20; ++i) uiFadeStep(g, false, 0.1f, t);
    CHECK(g.alpha == doctest::Approx(0.4f));
}

TEST_CASE("ui style: danger red per world, the weak point in Mark, a danger word ink over red") {
    std::string err;
    std::string text;
    REQUIRE(ftcore_test::readProjectData("UI/style.json", text));
    UiStyle shipped;
    REQUIRE_MESSAGE(importUiStyle(text, shipped, &err), err);
    const UiWorld unw = UiWorld::Unwritten;
    const UiColor reds[] = {{230.f / 255.f, 0.f, 18.f / 255.f, 1.f}, // Solhaven #E60012
        {212.f / 255.f, 0.f, 16.f / 255.f, 1.f},                    // the Lattice #D40010
        {201.f / 255.f, 0.f, 14.f / 255.f, 1.f}};                   // the Unwritten #C9000E
    const UiColor gold{1.f, 224.f / 255.f, 138.f / 255.f, 1.f}; // #FFE08A
    for (const UiStyle& s : {defaultUiStyle(), shipped}) {
        // No world wears accent2 on its marks; every world draws a danger word in ink over a red underline.
        for (std::size_t w = 0; w < kUiWorldCount; ++w) {
            CHECK_FALSE(s.rules[w].marksAccent2);
            CHECK(s.rules[w].dangerUnderline);
        }
        for (std::size_t w = 0; w < kUiWorldCount; ++w) {
            CAPTURE(w);
            const UiWorld wo = static_cast<UiWorld>(w);
            CHECK(uiColor(s, wo, UiPalette::Default, UiToken::Danger) == reds[w]);
            CHECK(uiColor(s, wo, UiPalette::Default, UiToken::WeakPoint) == gold);
            CHECK(uiMarkColor(s, wo, UiPalette::Default, gold) == gold);
        }
        for (const UiPalette p : {UiPalette::Default, UiPalette::Protan, UiPalette::Deutan, UiPalette::Tritan,
                                  UiPalette::HighContrast}) {
            CAPTURE(static_cast<int>(p));
            const UiDangerWord u = uiDangerWord(s, unw, p);
            CHECK(u.underlined);
            CHECK(u.word == uiColor(s, unw, p, UiToken::Ink));
            CHECK(u.underline == uiColor(s, unw, p, UiToken::Danger));
            const UiDangerWord sol = uiDangerWord(s, UiWorld::Solhaven, p);
            CHECK(sol.underlined);
            CHECK(sol.word == uiColor(s, UiWorld::Solhaven, p, UiToken::Ink));
        }
    }
    // A world object that leaves the keys out has the rules off; the keys take booleans only, and only in a world.
    UiStyle t;
    REQUIRE_MESSAGE(importUiStyle(R"({"worlds": {"unwritten": {"ink": "#DCE1E5"}}})", t, &err), err);
    CHECK_FALSE(t.rules[static_cast<std::size_t>(unw)].marksAccent2);
    CHECK_FALSE(t.rules[static_cast<std::size_t>(unw)].dangerUnderline);
    CHECK(uiMarkColor(t, unw, UiPalette::Default, gold) == gold);
    REQUIRE_MESSAGE(importUiStyle(R"({"worlds": {"unwritten": {"marks_accent2": true}}})", t, &err), err);
    CHECK(uiMarkColor(t, unw, UiPalette::Default, gold) == uiColor(t, unw, UiPalette::Default, UiToken::Accent2));
    CHECK_FALSE(importUiStyle(R"({"worlds": {"unwritten": {"marks_accent2": "yes"}}})", t, &err));
    CHECK_FALSE(importUiStyle(R"({"palettes": {"protan": {"danger_underline": true}}})", t, &err));
}

TEST_CASE("ui style: red means harm to the player: the hit cue, the lost hp, the low underline, the call triangle") {
    const auto isRed = [](UiColor c) { return c.r > 0.2f && c.r > 1.5f * c.g && c.r > 1.5f * c.b; };
    const UiColor black{0.f, 0.f, 0.f, 1.f};
    std::string err;
    std::string text;
    REQUIRE(ftcore_test::readProjectData("UI/style.json", text));
    UiStyle shipped;
    REQUIRE_MESSAGE(importUiStyle(text, shipped, &err), err);
    for (const UiStyle& s : {defaultUiStyle(), shipped}) {
        for (std::size_t w = 0; w < kUiWorldCount; ++w) {
            CAPTURE(w);
            const UiWorld wo = static_cast<UiWorld>(w);
            const UiPalette p = UiPalette::Default;
            // The tokens: danger is red; every token a text, value line or the weak point wears is not.
            for (const UiToken tk : {UiToken::Ink, UiToken::InkDim, UiToken::Accent, UiToken::Ally, UiToken::WeakPoint,
                                     UiToken::Backing})
                CHECK_FALSE(isRed(uiColor(s, wo, p, tk)));
            CHECK(isRed(uiColor(s, wo, p, UiToken::Danger)));
            CHECK_FALSE(isRed(s.edge));
            CHECK_FALSE(isRed(s.loadGold));
            const UiDangerSet d = uiDangerSet(s, wo, p);
            CHECK(isRed(d.hitCue));
            CHECK(isRed(d.lowUnderline));
            CHECK(d.cueMark == uiColor(s, wo, p, UiToken::Danger));
            // The darkening layer under the hit cue: the cue's red scaled down, darker than it.
            CHECK(d.hitDark.r == doctest::Approx(d.hitCue.r * kUiHitDarkK));
            CHECK(d.hitDark.b == doctest::Approx(d.hitCue.b * kUiHitDarkK));
            CHECK(uiContrast(d.hitCue, d.hitDark, black) > 1.5f);
            // The low line: the world's fill where the low underline carries the red, else the red itself.
            if (d.lowUnderlined) CHECK(d.lowLine == uiTrackSet(s, wo, p).fill);
            else CHECK(d.lowLine == uiColor(s, wo, p, UiToken::Danger));
        }
        // Every palette's danger holds 3:1 against the track body's own colour and the ink, in every world.
        for (std::size_t w = 0; w < kUiWorldCount; ++w) {
            for (std::size_t p = 0; p < kUiPaletteCount; ++p) {
                CAPTURE(w);
                CAPTURE(p);
                const UiWorld wo = static_cast<UiWorld>(w);
                const UiPalette pal = static_cast<UiPalette>(p);
                UiColor body = uiTrackSet(s, wo, pal).body;
                body.a = 1.f;
                const UiColor dg = uiColor(s, wo, pal, UiToken::Danger);
                CHECK(uiContrast(dg, body, black) >= 3.f);
                CHECK(uiContrast(dg, uiColor(s, wo, pal, UiToken::Ink), black) >= 3.f);
            }
        }
    }
}

TEST_CASE("ui style: the LOAD label shows on the first Load of a fight for 2 s, fades over 1 s, never again") {
    UiLoadLabel g;
    CHECK(uiLoadLabelStep(g, 0, 1.0) == 0.f);  // nothing held: no label
    CHECK(uiLoadLabelStep(g, 10, 2.0) == 1.f); // first held
    CHECK(uiLoadLabelStep(g, 12, 3.9) == 1.f); // 1.9 s: more Load is not a change
    const float fading = uiLoadLabelStep(g, 20, 4.5); // 2.5 s: half way down the 1 s fade
    CHECK(fading == doctest::Approx(0.5f));
    CHECK(uiLoadLabelStep(g, 20, 4.5) == fading); // a second step at the same clock time adds nothing
    CHECK(uiLoadLabelStep(g, 20, 5.0) == 0.f);    // 3 s: gone
    CHECK(uiLoadLabelStep(g, 0, 6.0) == 0.f);     // spent
    CHECK(uiLoadLabelStep(g, 40, 7.0) == 0.f);    // held again in the same fight: no label (the Release, the cap: none)
    g = UiLoadLabel{};                            // a new fight
    CHECK(uiLoadLabelStep(g, 5, 0.5) == 1.f);
}

TEST_CASE("ui style: WEAK shows once a fight, while the weak point is open") {
    UiOnceLabel w;
    CHECK(uiOnceLabelStep(w, false, 0.0) == 0.f);
    CHECK(uiOnceLabelStep(w, true, 1.0) == 1.f);
    CHECK(uiOnceLabelStep(w, false, 1.5) == 0.f); // closed inside the hold: gone with the mark
    CHECK(uiOnceLabelStep(w, true, 2.0) == 1.f);  // reopened inside the first showing's 2 s: the same showing
    CHECK(uiOnceLabelStep(w, true, 3.5) == doctest::Approx(0.5f));
    CHECK(uiOnceLabelStep(w, true, 10.0) == 0.f);
    CHECK(uiLabelLevel(-0.1) == 0.f);
    CHECK(uiLabelLevel(2.0) == 1.f);
    CHECK(uiLabelLevel(3.0) == 0.f);
}

TEST_CASE("ui style: the hit cue is one red in every world, the danger token of the palette shown") {
    std::string err;
    std::string text;
    REQUIRE(ftcore_test::readProjectData("UI/style.json", text));
    UiStyle shipped;
    REQUIRE_MESSAGE(importUiStyle(text, shipped, &err), err);
    for (const UiStyle& s : {defaultUiStyle(), shipped}) {
        for (std::size_t p = 0; p < kUiPaletteCount; ++p) {
            CAPTURE(p);
            const UiPalette pal = static_cast<UiPalette>(p);
            const UiColor red = uiColor(s, UiWorld::Unwritten, pal, UiToken::Danger);
            for (std::size_t w = 0; w < kUiWorldCount; ++w) {
                CAPTURE(w);
                CHECK(uiDangerSet(s, static_cast<UiWorld>(w), pal).hitCue == red);
            }
        }
    }
}

TEST_CASE("ui style: a value line's three tones: the world's backing at 0.45, a white hairline, the world's fill") {
    const UiColor black{0.f, 0.f, 0.f, 1.f};
    std::string err;
    std::string text;
    REQUIRE(ftcore_test::readProjectData("UI/style.json", text));
    UiStyle shipped;
    REQUIRE_MESSAGE(importUiStyle(text, shipped, &err), err);
    const UiColor fills[] = {{0xF2 / 255.f, 0xDD / 255.f, 0xB4 / 255.f, 1.f}, {0x8F / 255.f, 0xD6 / 255.f, 0xE8 / 255.f, 1.f},
        {0xBB / 255.f, 0xC0 / 255.f, 0xC3 / 255.f, 1.f}};
    for (const UiStyle& s : {defaultUiStyle(), shipped}) {
        for (std::size_t w = 0; w < kUiWorldCount; ++w) {
            for (std::size_t p = 0; p < kUiPaletteCount; ++p) {
                CAPTURE(w);
                CAPTURE(p);
                const UiWorld wo = static_cast<UiWorld>(w);
                const UiPalette pal = static_cast<UiPalette>(p);
                const UiTrackSet t = uiTrackSet(s, wo, pal);
                const UiColor back = uiColor(s, wo, pal, UiToken::Backing);
                CHECK(t.body.r == back.r);
                CHECK(t.body.b == back.b);
                CHECK(t.body.a == (pal == UiPalette::HighContrast ? 1.f : kUiTrackBodyA));
                CHECK(t.hairline == uiColor(s, wo, pal, UiToken::Track));
                if (pal == UiPalette::HighContrast) CHECK(t.fill == UiColor{1.f, 1.f, 1.f, 1.f});
                else CHECK(t.fill == fills[w]);
                // The fill against its body's own colour: 10:1 or more.
                UiColor body = t.body;
                body.a = 1.f;
                CHECK(uiContrast(t.fill, body, black) >= 10.f);
            }
        }
    }
}

TEST_CASE("ui style: danger and the hit cue's red hold 3:1 against every world's fill; import refuses less") {
    const UiColor black{0.f, 0.f, 0.f, 1.f};
    std::string err;
    std::string text;
    REQUIRE(ftcore_test::readProjectData("UI/style.json", text));
    UiStyle shipped;
    REQUIRE_MESSAGE(importUiStyle(text, shipped, &err), err);
    for (const UiStyle& s : {defaultUiStyle(), shipped}) {
        CHECK(uiDangerFillContrast(s, UiWorld::Solhaven, UiPalette::Default) == doctest::Approx(3.61f).epsilon(0.01));
        CHECK(uiDangerFillContrast(s, UiWorld::Lattice, UiPalette::Default) == doctest::Approx(3.40f).epsilon(0.01));
        CHECK(uiDangerFillContrast(s, UiWorld::Unwritten, UiPalette::Default) == doctest::Approx(3.28f).epsilon(0.01));
        CHECK(uiDangerFillContrast(s, UiWorld::Lattice, UiPalette::HighContrast) >= 4.5f);
        for (std::size_t w = 0; w < kUiWorldCount; ++w) {
            for (std::size_t p = 0; p < kUiPaletteCount; ++p) {
                CAPTURE(w);
                CAPTURE(p);
                const UiWorld wo = static_cast<UiWorld>(w);
                const UiPalette pal = static_cast<UiPalette>(p);
                CHECK(uiDangerFillContrast(s, wo, pal) >= 3.f);
                CHECK(uiContrast(uiDangerSet(s, wo, pal).hitCue, uiTrackSet(s, wo, pal).fill, black) >= 3.f);
            }
        }
    }
    // The old one red (E60012) on the Lattice's cyan fill is 2.96:1: refused.
    UiStyle t;
    CHECK_FALSE(importUiStyle(R"({"worlds": {"lattice": {"danger": "#E60012"}}})", t, &err));
    CHECK(err.find("danger against the fill") != std::string::npos);
    // The Unwritten's red is the hit cue in every world, so it is held against every world's fill as well.
    CHECK_FALSE(importUiStyle(R"({"worlds": {"unwritten": {"danger": "#E60012"}}})", t, &err));
    CHECK(err.find("danger against the fill") != std::string::npos);
}

TEST_CASE("ui style: the hit cue is full for 100 ms and gone by 350 ms") {
    CHECK(uiHitFlashLevel(-0.01f) == 0.f);
    CHECK(uiHitFlashLevel(0.f) == 1.f);
    CHECK(uiHitFlashLevel(0.1f) == 1.f);
    CHECK(uiHitFlashLevel(0.2f) == doctest::Approx(0.648f).epsilon(0.002));
    CHECK(uiHitFlashLevel(0.3f) < uiHitFlashLevel(0.2f));
    CHECK(uiHitFlashLevel(0.35f) == 0.f);
    CHECK(uiHitFlashLevel(1.f) == 0.f);
    CHECK(kUiHitGlowShare == doctest::Approx(0.06f)); // inside the outer 6 percent of each axis
}

TEST_CASE("ui style: the hit cue's two layers (fB): red 0.38 over dark 0.30 on one f^1.6 curve, gone at 6 percent") {
    const UiHitLayers e = uiHitLayers(0.f, 1.f);
    CHECK(e.dark == doctest::Approx(0.30f));
    CHECK(e.red == doctest::Approx(0.38f));
    const UiHitLayers off = uiHitLayers(0.f, kUiHitOffSide);
    CHECK(off.dark == doctest::Approx(0.30f * 0.25f)); // one share on every edge
    CHECK(off.red == doctest::Approx(0.38f * 0.25f));
    // Halfway in: (1 - smoothstep(0.5)) = 0.5, both layers on the same curve.
    const UiHitLayers h = uiHitLayers(0.5f, 1.f);
    CHECK(h.dark == doctest::Approx(0.30f * std::pow(0.5f, 1.6f)));
    CHECK(h.red == doctest::Approx(0.38f * std::pow(0.5f, 1.6f)));
    CHECK(h.dark / e.dark == doctest::Approx(h.red / e.red));
    CHECK(uiHitLayers(1.f, 1.f).dark == 0.f);
    CHECK(uiHitLayers(1.f, 1.f).red == 0.f);
    CHECK(uiHitLayers(2.f, 1.f).red == 0.f);
    CHECK(uiHitLayers(-1.f, 1.f).red == doctest::Approx(0.38f));
    CHECK(uiHitLayers(0.f, 0.f).dark == 0.f);
    for (int i = 0; i < 10; ++i) {
        const float a = static_cast<float>(i) / 10.f, b = static_cast<float>(i + 1) / 10.f;
        CHECK(uiHitLayers(b, 1.f).dark <= uiHitLayers(a, 1.f).dark);
        CHECK(uiHitLayers(b, 1.f).red <= uiHitLayers(a, 1.f).red);
    }
}

TEST_CASE("ui style: the hit cue's edges: the struck side at full, the others at a quarter; no side, all four") {
    CHECK(uiHitEdges(false, 1.f, 0.f) == UiHitEdges{});
    CHECK(uiHitEdges(true, 0.f, 0.f) == UiHitEdges{});
    CHECK(uiHitEdges(true, 1.f, 0.2f) == UiHitEdges{0.25f, 1.f, 0.25f, 0.25f});
    CHECK(uiHitEdges(true, -0.9f, 0.3f) == UiHitEdges{1.f, 0.25f, 0.25f, 0.25f});
    CHECK(uiHitEdges(true, 0.1f, 2.f) == UiHitEdges{0.25f, 0.25f, 1.f, 0.25f});
    CHECK(uiHitEdges(true, 0.1f, -1.f) == UiHitEdges{0.25f, 0.25f, 0.25f, 1.f});
}

TEST_CASE("ui style: the GUARD label shows for 2 s after the line appears or its state changes, fades over 1 s") {
    CHECK(uiGuardState(1.f, false) == UiGuardState::Full);
    CHECK(uiGuardState(0.5f, false) == UiGuardState::Under);
    CHECK(uiGuardState(0.1f, false) == UiGuardState::Under); // low is not a state of its own
    CHECK(uiGuardState(0.f, false) == UiGuardState::Spent);
    CHECK(uiGuardState(0.5f, true) == UiGuardState::Broken);
    UiGuardLabel g;
    CHECK(uiGuardLabelStep(g, UiGuardState::Full, 0.0) == 0.f);
    CHECK(uiGuardLabelStep(g, UiGuardState::Under, 1.0) == 1.f); // the line appears
    CHECK(uiGuardLabelStep(g, UiGuardState::Under, 2.9) == 1.f); // 1.9 s
    const float fading = uiGuardLabelStep(g, UiGuardState::Under, 3.5); // 2.5 s
    CHECK(fading == doctest::Approx(0.5f));
    CHECK(uiGuardLabelStep(g, UiGuardState::Under, 3.5) == fading); // the same clock time: the same alpha
    CHECK(uiGuardLabelStep(g, UiGuardState::Under, 4.0) == 0.f);    // 3 s: gone
    CHECK(uiGuardLabelStep(g, UiGuardState::Under, 9.0) == 0.f);    // steady play: a bare line
    CHECK(uiGuardLabelStep(g, UiGuardState::Spent, 9.1) == 1.f);    // a change brings it back
    CHECK(uiGuardLabelStep(g, UiGuardState::Broken, 12.0) == 1.f);  // another (the change resets the clock)
    CHECK(uiGuardLabelStep(g, UiGuardState::Broken, 15.0) == 0.f);
    CHECK(uiGuardLabelStep(g, UiGuardState::Full, 15.1) == 0.f);    // back to full: the row goes, no label
}

TEST_CASE("ui style: a colour-vision palette changes only the tokens it lists; layers are data") {
    UiStyle s;
    std::string err;
    REQUIRE_MESSAGE(importUiStyle(R"({"palettes": {"deutan": {"danger": "#8C5A00"}},
        "layers": {"plates": true, "world_anchored": true, "hairlines": false}})", s, &err), err);
    const UiColor d = uiColor(s, UiWorld::Solhaven, UiPalette::Deutan, UiToken::Danger);
    CHECK(d.g == doctest::Approx(90.f / 255.f));
    // A light danger fails 3:1 against the ink: refused.
    UiStyle light;
    CHECK_FALSE(importUiStyle(R"({"palettes": {"deutan": {"danger": "#F0A030"}}})", light, &err));
    CHECK(err.find("danger on the track") != std::string::npos);
    // The ally colour is the world's own (the deutan list above replaced the built-in list).
    CHECK(uiColor(s, UiWorld::Solhaven, UiPalette::Deutan, UiToken::Ally) ==
          uiColor(s, UiWorld::Solhaven, UiPalette::Default, UiToken::Ally));
    CHECK(uiColor(s, UiWorld::Lattice, UiPalette::Default, UiToken::Accent) !=
          uiColor(s, UiWorld::Solhaven, UiPalette::Default, UiToken::Accent));
    CHECK(s.layers.plates);
    CHECK(s.layers.worldAnchored);
    CHECK_FALSE(s.layers.hairlines);
    CHECK(uiCueShape(s, UiToken::WeakPoint) == UiCueShape::Diamond);
}

// ---- glyphs ------------------------------------------------------------------------------------------------------

TEST_CASE("ui glyphs: one table for every screen, keys and pad, rebuilt only when a binding changes") {
    InputBindings b = defaultInputBindings();
    UiGlyphTable t;
    CHECK(buildUiGlyphTable(b, t));
    CHECK(t.revision == 1);
    const UiGlyph& lightKeys = uiGlyph(t, InputAction::Light, InputDevice::Keys);
    CHECK(lightKeys.shape == UiGlyphShape::Mouse);
    CHECK(lightKeys.part == 0);
    CHECK(lightKeys.text() == "LMB");
    const UiGlyph& lightPad = uiGlyph(t, InputAction::Light, InputDevice::Pad);
    CHECK(lightPad.shape == UiGlyphShape::Trigger);
    CHECK(lightPad.part == 1);
    const UiGlyph& jump = uiGlyph(t, InputAction::Jump, InputDevice::Pad);
    CHECK(jump.shape == UiGlyphShape::FaceButton);
    CHECK(jump.part == 0);
    const UiGlyph& gift = uiGlyph(t, InputAction::Gift, InputDevice::Keys);
    CHECK(gift.shape == UiGlyphShape::KeyCap);
    CHECK(gift.text() == "H");
    CHECK(uiGlyph(t, InputAction::FocusNext, InputDevice::Pad).shape == UiGlyphShape::Stick);
    CHECK(uiGlyph(t, InputAction::FocusNext, InputDevice::Pad).dir == 2);

    // Nothing changed: no new revision.
    CHECK_FALSE(buildUiGlyphTable(b, t));
    CHECK(t.revision == 1);
    // A rebind shows on the next build.
    REQUIRE(rebindInput(b, InputAction::Gift, InputDevice::Keys, "N"));
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    CHECK(buildUiGlyphTable(b, t));
    if (counting) CHECK(ftcore_test::allocStats().count == before);
    CHECK(t.revision == 2);
    CHECK(uiGlyph(t, InputAction::Gift, InputDevice::Keys).text() == "N");
    CHECK(uiGlyphForKey("").shape == UiGlyphShape::None);
}

TEST_CASE("ui glyphs: the prompt family follows the last device unless the player fixes it") {
    CHECK(uiPromptDevice(UiGlyphMode::Auto, InputDevice::Pad) == InputDevice::Pad);
    CHECK(uiPromptDevice(UiGlyphMode::Auto, InputDevice::Keys) == InputDevice::Keys);
    CHECK(uiPromptDevice(UiGlyphMode::Keys, InputDevice::Pad) == InputDevice::Keys);
    CHECK(uiPromptDevice(UiGlyphMode::Pad, InputDevice::Keys) == InputDevice::Pad);
}

// ---- arena HUD ---------------------------------------------------------------------------------------------------

TEST_CASE("ui arena view: the lead and target sections come from the fight; revisions move only on change") {
    ArenaDef def = defaultArenaDef();
    ArenaFight fight;
    fight.begin(def, ArenaRing{Vec2{0.f, 0.f}, 30.f}, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
    ArenaBodies bodies;
    bodies.enemies[0] = Vec2{2.f, 0.f};
    ArenaInput in;
    fight.tick(1.f / 60.f, in, bodies);

    ArenaHudView view;
    ArenaHudFrame f;
    f.lead.lead = 1;
    fillArenaHudLead(fight, f.lead);
    fillArenaHudTarget(fight, f.target);
    CHECK(f.lead.lead == 1); // kept
    CHECK(f.lead.hp == def.fighter.hp);
    CHECK(f.lead.hpMax == def.fighter.hp);
    CHECK(f.lead.heals == def.fighter.healCharges);
    CHECK(f.lead.healsMax == def.fighter.healCharges);
    CHECK(f.lead.readMax == kMaxReadStacks);
    CHECK(f.lead.loadMax == kMaxLoad);
    CHECK_FALSE(f.lead.releaseReady);
    CHECK(f.third.planned); // no third-chair data yet: a reserved slot

    CHECK(updateArenaHudView(view, f) == 1u); // only the lead differs from an empty view (the target is not locked)
    CHECK(view.revisionOf(ArenaHudSection::Lead) == 1);
    CHECK(updateArenaHudView(view, f) == 0u);

    // Lock on: the target section changes, the lead's does not.
    in.view = Vec2{1.f, 0.f};
    in.lock = true;
    fight.tick(1.f / 60.f, in, bodies);
    in.lock = false;
    REQUIRE(fight.lock() != kArenaNobody);
    fillArenaHudLead(fight, f.lead);
    fillArenaHudTarget(fight, f.target);
    CHECK(f.target.present);
    CHECK(f.target.locked);
    CHECK(f.target.hpMax == def.enemies[0].hp);
    CHECK(f.target.poise01 == doctest::Approx(1.f));
    const std::uint32_t mask = updateArenaHudView(view, f);
    CHECK((mask & (1u << static_cast<std::uint32_t>(ArenaHudSection::Target))) != 0u);
    CHECK(view.revisionOf(ArenaHudSection::Target) == 1);

    // Guard held shows on the lead; a boss's weak point opening shows on the boss section.
    in.guard = true;
    fight.tick(1.f / 60.f, in, bodies);
    fillArenaHudLead(fight, f.lead);
    CHECK(f.lead.guarding);
    f.boss.present = true;
    f.boss.hp = 50;
    f.boss.hpMax = 100;
    f.boss.phase = 1;
    f.boss.phaseCount = 3;
    f.boss.weakPoint = UiWeakPoint::Open;
    f.boss.way = UiWeakPointWay::Share;
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    const std::uint32_t mask2 = updateArenaHudView(view, f);
    fillArenaHudTarget(fight, f.target);
    if (counting) CHECK(ftcore_test::allocStats().count == before);
    CHECK((mask2 & (1u << static_cast<std::uint32_t>(ArenaHudSection::Lead))) != 0u);
    CHECK((mask2 & (1u << static_cast<std::uint32_t>(ArenaHudSection::Boss))) != 0u);
    CHECK(view.now.boss.weakPoint == UiWeakPoint::Open);

    // A called zone, the lead stepping into it, and a new answer note each wake the boss section, and only it.
    constexpr std::uint32_t bossBit = 1u << static_cast<std::uint32_t>(ArenaHudSection::Boss);
    fillArenaHudTarget(fight, f.target);
    updateArenaHudView(view, f);
    f.boss.call = UiCalledZone::Left;
    CHECK(updateArenaHudView(view, f) == bossBit);
    f.boss.callIn = true;
    CHECK(updateArenaHudView(view, f) == bossBit);
    f.boss.noteSerial = 0;
    CHECK(updateArenaHudView(view, f) == bossBit);
    CHECK(updateArenaHudView(view, f) == 0u);
    CHECK(view.now.boss.rank == 0); // no rank data: nothing drawn
}

TEST_CASE("ui arena view: the carry fields (cap, Release mark, chairs, heal places, the ally) follow the fight") {
    ArenaDef def = defaultArenaDef();
    {   // carrying off: the cap is the track, the Release mark is the constant one, no chairs, no ally
        ArenaFight fight;
        fight.begin(def, ArenaRing{Vec2{0.f, 0.f}, 30.f}, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
        ArenaHudLead lead;
        ArenaHudPartner partner;
        fillArenaHudLead(fight, lead);
        fillArenaHudPartner(fight, partner);
        CHECK(lead.loadCap == kMaxLoad);
        CHECK(lead.releaseAt == kArenaReleaseOpenLoad);
        CHECK(lead.chairCount == 0);
        CHECK(lead.chairs[0] == ChairKind::None);
        CHECK(lead.healsMax == def.fighter.healCharges);
        CHECK_FALSE(lead.tookHalf);
        CHECK_FALSE(partner.present);
        CHECK(partner.hpMax == 0);
    }
    {   // a cap of 70, two Named chairs (one adds a heal charge) and an ally of 120 hp
        ArenaFight fight;
        ArenaChairs chairs;
        chairs.named[0] = ChairEffect{ChairKind::HealCharges, 1.f, 0.f};
        chairs.named[1] = ChairEffect{ChairKind::LoadGain, 0.1f, 3.f};
        chairs.count = 2;
        fight.setLoadCap(70);
        fight.setChairs(chairs);
        fight.setCarry(true);
        fight.setAlly(120, 0.4f);
        fight.begin(def, ArenaRing{Vec2{0.f, 0.f}, 30.f}, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, 1);
        REQUIRE(fight.giveLoad(30) == 30);
        ArenaHudLead lead;
        ArenaHudPartner partner;
        partner.lead = 2;
        fillArenaHudLead(fight, lead);
        fillArenaHudPartner(fight, partner);
        CHECK(lead.loadMax == kMaxLoad);
        CHECK(lead.loadCap == 70);
        CHECK(lead.releaseAt == 23);
        CHECK(lead.load == 30);
        CHECK(lead.releaseReady); // 30 > 23, though under the uncapped 33
        CHECK_FALSE(lead.overloaded);
        CHECK(lead.healsMax == def.fighter.healCharges + 1);
        CHECK(lead.chairCount == 2);
        CHECK(lead.chairs[0] == ChairKind::HealCharges);
        CHECK(lead.chairs[1] == ChairKind::LoadGain);
        CHECK(lead.chairs[2] == ChairKind::None);
        CHECK(partner.present);
        CHECK(partner.lead == 2); // kept
        CHECK(partner.hp == 120);
        CHECK(partner.hpMax == 120);
        CHECK_FALSE(partner.down);
    }
}

// ---- dialogue ----------------------------------------------------------------------------------------------------

TEST_CASE("ui dialogue view: speaker on every line, text by reveal, prompt once complete, options on a choice") {
    Script s;
    StoryRunner r(&s.db);
    REQUIRE(r.start());
    DialoguePresenter p(&r);
    p.setRevealRate(10.f);
    DialogueView v;
    DialogueViewFlags flags;

    p.update(0.f);
    CHECK(updateDialogueView(v, p, flags));
    CHECK(v.kind == DialogueViewKind::Direction);
    CHECK(v.speaker.empty());
    CHECK(v.beatRevision == 1);

    REQUIRE(r.advance()); // the line
    p.update(0.f);
    CHECK(updateDialogueView(v, p, flags));
    CHECK(v.kind == DialogueViewKind::Line);
    CHECK(v.speaker == "SPK");
    CHECK(v.lineId == "PH-U-001");
    CHECK(v.text.empty());
    CHECK_FALSE(v.continuePrompt);
    const std::uint32_t beat = v.beatRevision;

    // Typing: the text revision moves, the beat's does not; no allocation.
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    p.update(0.25f);
    CHECK(updateDialogueView(v, p, flags));
    if (counting) CHECK(ftcore_test::allocStats().count == before);
    CHECK(v.text == "He");
    CHECK(v.revealedChars == 2);
    CHECK(v.fullText.substr(0, 2) == "He"); // the whole line, set once per beat, for wrapping
    CHECK(v.beatRevision == beat);
    const std::uint32_t textRev = v.textRevision;
    CHECK_FALSE(updateDialogueView(v, p, flags)); // nothing new
    CHECK(v.textRevision == textRev);

    p.update(5.f);
    CHECK(updateDialogueView(v, p, flags));
    CHECK(v.complete);
    CHECK(v.continuePrompt);
    CHECK(v.revealedChars == v.fullText.size()); // ASCII line: codepoints == bytes
    const std::uint32_t textDone = v.textRevision;

    // Auto on: only the prompt section moves.
    const std::uint32_t promptRev = v.promptRevision;
    flags.autoOn = true;
    CHECK(updateDialogueView(v, p, flags));
    CHECK(v.promptRevision == promptRev + 1);
    CHECK(v.textRevision == textDone);
    CHECK(v.beatRevision == beat);

    REQUIRE(r.advance()); // the choice
    p.update(5.f);
    CHECK(updateDialogueView(v, p, flags));
    CHECK(v.kind == DialogueViewKind::Choice);
    CHECK(v.speaker.empty());
    REQUIRE(v.optionCount == 2);
    CHECK(v.options[0].text == "Greyed");
    CHECK_FALSE(v.options[0].selectable);
    CHECK(v.options[1].selectable);
    CHECK_FALSE(v.continuePrompt); // a choice waits for a pick, not a press

    // A line by another speaker names them.
    REQUIRE(r.choose(1));
    p.update(0.f);
    CHECK(updateDialogueView(v, p, flags));
    CHECK(v.speaker == "OTHER");

    // No runner: hidden.
    DialoguePresenter none;
    DialogueView hidden;
    updateDialogueView(hidden, none, flags);
    CHECK(hidden.kind == DialogueViewKind::Hidden);
}

TEST_CASE("ui style: the dim behind dialogue options is measured: none on a dark picture, just enough on a bright one") {
    const UiStyle s = defaultUiStyle();
    const UiColor ink{0.953f, 0.933f, 0.902f, 1.f};
    const UiColor back{0.039f, 0.031f, 0.024f, 1.f};
    CHECK(uiOptionDim(s, ink, back, UiColor{0.1f, 0.1f, 0.1f, 1.f}) == 0.f);
    const UiColor grey{0.6f, 0.6f, 0.6f, 1.f};
    const float a = uiOptionDim(s, ink, back, grey);
    CHECK(a > 0.05f);
    CHECK(a < s.optionsDimMax);
    CHECK(uiContrast(ink, UiColor{back.r, back.g, back.b, a}, grey) >= s.optionsDimContrast - 0.01f);
    CHECK(uiContrast(ink, UiColor{back.r, back.g, back.b, a - 0.03f}, grey) < s.optionsDimContrast); // no stronger than needed
    const float onWhite = uiOptionDim(s, ink, back, UiColor{1.f, 1.f, 1.f, 1.f});
    CHECK(onWhite > a); // a brighter picture, a stronger dim
    CHECK(onWhite <= s.optionsDimMax);
    UiStyle strict = s;
    strict.optionsDimContrast = 15.f;
    CHECK(uiOptionDim(strict, ink, back, UiColor{1.f, 1.f, 1.f, 1.f}) == doctest::Approx(s.optionsDimMax)); // capped
    UiStyle loose = s;
    std::string err;
    REQUIRE(importUiStyle(R"({"backing_soft": {"options_contrast": 2, "options_max": 0.4}})", loose, &err));
    CHECK(loose.optionsDimContrast == doctest::Approx(2.f));
    CHECK(loose.optionsDimMax == doctest::Approx(0.4f));
    CHECK(uiOptionDim(loose, ink, back, grey) < a);
    CHECK_FALSE(importUiStyle(R"({"backing_soft": {"options_max": 1.5}})", loose, &err));
}

TEST_CASE("ui style: the picture's brightest share, by luminance, with no allocation") {
    UiLumaHistogram h;
    CHECK(uiLumaBrightest(h, 0.1f).r == 0.f); // nothing measured: black, so no dim
    for (int i = 0; i < 90; ++i) uiLumaAdd(h, UiColor{0.1f, 0.1f, 0.1f, 1.f});
    for (int i = 0; i < 10; ++i) uiLumaAdd(h, UiColor{0.95f, 0.9f, 0.8f, 1.f});
    CHECK(h.total == 100);
    const UiColor top = uiLumaBrightest(h, 0.1f);
    CHECK(top.r == doctest::Approx(0.95f));
    CHECK(top.b == doctest::Approx(0.8f));
    CHECK(uiLumaBrightest(h, 1.f).r == doctest::Approx(0.185f)); // the whole picture's mean
}

TEST_CASE("ui load name: the gold fills to the threshold, brightens to the cap, sweeps once and drains in 0.3 s") {
    ArenaHudLead lead;
    lead.loadCap = 9;
    lead.releaseAt = 3;
    UiLoadName s;
    lead.load = 1;
    uiLoadNameStep(s, lead, 1.f / 60.f);
    CHECK(s.fill == doctest::Approx(1.f / 3.f));
    CHECK(s.bright == 0.f);
    CHECK(s.shimmer < 0.f);
    lead.load = 4;
    lead.releaseReady = true;
    uiLoadNameStep(s, lead, 1.f / 60.f);
    CHECK(s.fill == 1.f);
    CHECK(s.bright == doctest::Approx(1.f / 6.f));
    CHECK(s.shimmer == 0.f); // the crossing
    for (int k = 0; k < 12; ++k) uiLoadNameStep(s, lead, 1.f / 60.f);
    CHECK(s.shimmer == doctest::Approx(0.5f).epsilon(0.01));
    for (int k = 0; k < 13; ++k) uiLoadNameStep(s, lead, 1.f / 60.f);
    CHECK(s.shimmer < 0.f); // one sweep, then steady
    lead.load = 9;
    uiLoadNameStep(s, lead, 1.f / 60.f);
    CHECK(s.bright == 1.f);
    CHECK(s.shimmer < 0.f);
    // The Release spends it all: the gold drains rather than vanishing, and is gone 0.3 s later.
    lead.load = 0;
    lead.releaseReady = false;
    uiLoadNameStep(s, lead, 0.15f);
    CHECK(s.fill == doctest::Approx(0.5f));
    CHECK(s.bright == 0.f);
    uiLoadNameStep(s, lead, 0.16f);
    CHECK(s.fill == 0.f);
    // A cap at the threshold: nothing to brighten toward, no divide by zero.
    lead.loadCap = 3;
    lead.load = 3;
    uiLoadNameStep(s, lead, 1.f / 60.f);
    CHECK(s.fill == 1.f);
    CHECK(s.bright == 0.f);
    // No Load at all.
    UiLoadName none;
    uiLoadNameStep(none, ArenaHudLead{}, 1.f / 60.f);
    CHECK(none.fill == 0.f);
    CHECK(none.bright == 0.f);
}

TEST_CASE("ui lead slot: LOAD while some is held short of the Release, RELEASE once it opens, else nothing") {
    ArenaHudLead lead;
    CHECK(uiLeadSlot(lead) == UiLeadSlot::None); // no gauge
    lead.loadMax = 9;
    lead.loadCap = 9;
    lead.releaseAt = 3;
    CHECK(uiLeadSlot(lead) == UiLeadSlot::None); // a gauge, nothing held
    lead.load = 1;
    CHECK(uiLeadSlot(lead) == UiLeadSlot::Load);
    lead.load = 3; // the name is full of gold here, but the Release opens only above it
    CHECK(uiLeadSlot(lead) == UiLeadSlot::Load);
    lead.load = 4;
    lead.releaseReady = true;
    CHECK(uiLeadSlot(lead) == UiLeadSlot::Release);
    lead.load = 9;
    CHECK(uiLeadSlot(lead) == UiLeadSlot::Release);
    lead.load = 0;
    lead.releaseReady = false;
    CHECK(uiLeadSlot(lead) == UiLeadSlot::None); // spent
    lead.loadMax = 0;
    lead.load = 4;
    lead.releaseReady = true;
    CHECK(uiLeadSlot(lead) == UiLeadSlot::None); // a kit without the gauge says nothing
}

TEST_CASE("ui selected reply: the host's cursor, else the first reply that can be taken") {
    CHECK(uiSelectedReply(-1, 0b111u, 3) == 0);
    CHECK(uiSelectedReply(-1, 0b110u, 3) == 1); // the first is locked
    CHECK(uiSelectedReply(-1, 0b100u, 3) == 2);
    CHECK(uiSelectedReply(-1, 0u, 3) == -1);    // all locked: none drawn as selected
    CHECK(uiSelectedReply(-1, 0b1000u, 3) == -1); // a takeable bit past the replies counts for nothing
    CHECK(uiSelectedReply(2, 0b001u, 3) == 2);  // a cursor wins, even on a locked reply
    CHECK(uiSelectedReply(3, 0b010u, 3) == 1);  // a cursor past the replies is no cursor
    CHECK(uiSelectedReply(-1, 0b1u, 0) == -1);
}

TEST_CASE("ui arena view: the R11 hooks default to nothing drawn (Qte, gift data, the row swap)") {
    // Each hook's null: nothing reported, nothing drawn.
    const ArenaHudLead lead;
    CHECK_FALSE(lead.qte.live);
    CHECK(uiQteLeft01(lead.qte, 0.f) == 0.f);
    CHECK(uiQteLeft01(lead.qte, 5.f) == 0.f);
    const ArenaHudThird third;
    CHECK(third.giftHeals == -1);
    CHECK_FALSE(third.giftGuardReady);
    UiRowSwap swap;
    CHECK(swap.left01 == 0.f);

    // A live Qte's line runs out evenly over its window, and is gone at its end.
    ArenaHudQte q;
    q.live = true;
    q.kind = QteKind::Hold;
    q.input = ActionButton::Guard;
    q.from = 1.f;
    q.to = 3.f;
    CHECK(uiQteLeft01(q, 0.5f) == 1.f); // before the window: full
    CHECK(uiQteLeft01(q, 1.f) == 1.f);
    CHECK(uiQteLeft01(q, 2.f) == doctest::Approx(0.5f));
    CHECK(uiQteLeft01(q, 3.f) == 0.f);
    CHECK(uiQteLeft01(q, 4.f) == 0.f);
    ArenaHudQte empty = q;
    empty.to = empty.from; // an empty window: no line, no divide by zero
    CHECK(uiQteLeft01(empty, 1.f) == 0.f);

    // The Qte sits in the lead's section: its start and end move that revision, the frames between do not.
    ArenaHudView view;
    ArenaHudFrame f;
    f.lead.hp = f.lead.hpMax = 10;
    updateArenaHudView(view, f);
    const std::uint32_t rev = view.revisionOf(ArenaHudSection::Lead);
    f.lead.qte = q;
    CHECK(updateArenaHudView(view, f) == 1u);
    CHECK(updateArenaHudView(view, f) == 0u);
    f.lead.qte = ArenaHudQte{};
    CHECK(updateArenaHudView(view, f) == 1u);
    CHECK(view.revisionOf(ArenaHudSection::Lead) == rev + 2);
    // The gift data sits in the third place's section.
    f.third.giftHeals = 3;
    CHECK(updateArenaHudView(view, f) == (1u << static_cast<unsigned>(ArenaHudSection::Third)));
}

TEST_CASE("ui row swap: the rows trade places over 0.2 s only when the lead's seat changes under a partner row") {
    ArenaHudFrame f;
    f.lead.lead = 0;
    f.partner.present = true;
    f.partner.lead = 1;
    UiRowSwap s;
    uiRowSwapStep(s, f, 1.f / 60.f);
    CHECK(s.left01 == 0.f); // the first seats seen are no switch
    for (int k = 0; k < 30; ++k) uiRowSwapStep(s, f, 1.f / 60.f);
    CHECK(s.left01 == 0.f); // seats that never change never swap

    // The switch: the leads trade seats.
    f.lead.lead = 1;
    f.partner.lead = 0;
    uiRowSwapStep(s, f, 1.f / 60.f);
    CHECK(s.left01 == 1.f);
    uiRowSwapStep(s, f, 0.1f);
    CHECK(s.left01 == doctest::Approx(0.5f));
    uiRowSwapStep(s, f, 0.1f);
    CHECK(s.left01 == 0.f);
    uiRowSwapStep(s, f, 0.1f);
    CHECK(s.left01 == 0.f);

    // No partner row: a change of seat just shows.
    f.partner.present = false;
    f.lead.lead = 2;
    uiRowSwapStep(s, f, 1.f / 60.f);
    CHECK(s.left01 == 0.f);
}

TEST_CASE("ui dialogue view: no reply cursor and no lock reason by default; a cursor moves only the prompt section") {
    CHECK(DialogueViewFlags{}.cursor == -1);
    CHECK(DialogueViewOption{}.lockReason.empty());

    Script s;
    StoryRunner r(&s.db);
    REQUIRE(r.start());
    DialoguePresenter p(&r);
    p.setRevealRate(10.f);
    DialogueView v;
    DialogueViewFlags flags;
    REQUIRE(r.advance()); // the line
    REQUIRE(r.advance()); // the choice
    p.update(5.f);
    updateDialogueView(v, p, flags);
    REQUIRE(v.kind == DialogueViewKind::Choice);
    REQUIRE(v.optionCount == 2);
    CHECK(v.flags.cursor == -1);
    CHECK_FALSE(v.options[0].selectable);
    CHECK(v.options[0].lockReason.empty()); // locked, with no reason: the greyed reply stands alone
    CHECK(v.options[1].lockReason.empty());

    const std::uint32_t beat = v.beatRevision;
    const std::uint32_t prompt = v.promptRevision;
    flags.cursor = 1;
    CHECK(updateDialogueView(v, p, flags));
    CHECK(v.flags.cursor == 1);
    CHECK(v.promptRevision == prompt + 1);
    CHECK(v.beatRevision == beat);
    CHECK_FALSE(updateDialogueView(v, p, flags));
}

TEST_CASE("ui style: the low underline's gap and the guard row's clearance: 8 px, never under 8 device px") {
    // HUD scale 80-130 percent on viewports from 720p to 2160p, at Windows display scales 100-200 percent.
    const float pxs[] = {1.f, 1.f / 1.25f, 1.f / 1.5f, 1.f / 1.75f, 0.5f};
    for (float px : pxs) {
        for (int h = 720; h <= 2160; h += 60) {
            for (int hud = 80; hud <= 130; hud += 5) {
                const float s = std::max(0.25f, (static_cast<float>(h) * px) / 1080.f * (static_cast<float>(hud) / 100.f));
                const ftcore::UiLowGuardGaps g = ftcore::uiLowGuardGaps(s, px);
                CHECK(g.clear >= ftcore::kUiGapMinPx * px * 0.9999f);
                CHECK(g.gap >= ftcore::kUiGapMinPx * px * 0.9999f);
                CHECK(g.under >= px * 0.9999f);
                for (const float v : {g.clear, g.gap, g.under}) {
                    const float units = v / px; // whole device pixels
                    CHECK(units == doctest::Approx(std::floor(units + 0.5f)));
                }
            }
        }
    }
    const ftcore::UiLowGuardGaps one = ftcore::uiLowGuardGaps(1.f, 1.f);
    CHECK(one.gap == 8.f);
    CHECK(one.under == 2.f);
    CHECK(one.clear == 8.f);
    CHECK(ftcore::uiLowGuardGaps(0.75f, 1.f).clear == 8.f); // 720p: 6 px by scale, held at 8
    CHECK(ftcore::uiLowGuardGaps(1.3333f, 1.f).gap == 11.f); // 1440p: 10.7, whole px
}

TEST_CASE("ui style: the hit flash starts only on a later hit; a retry's reset shows none; ages run on the fight clock") {
    UiHitWatch w;
    double t = 0.0;
    CHECK(uiHitFlashLevel(uiHitWatchStep(w, -1e9, t += 0.016)) == 0.f); // the first tick only reads the stamp
    CHECK(uiHitFlashLevel(uiHitWatchStep(w, 3.0, t += 0.016)) == 1.f);  // a hit
    CHECK(uiHitWatchStep(w, 3.0, t) == 0.f);                            // a second step at the same clock time: age 0 still
    for (int i = 0; i < 30; ++i) uiHitWatchStep(w, 3.0, t += 0.016);
    CHECK(uiHitFlashLevel(w.age) == 0.f);
    // A held clock (a pause, a capture's hold) holds the cue where it is.
    UiHitWatch h;
    uiHitWatchStep(h, -1.0, 1.0);
    uiHitWatchStep(h, 1.0, 1.05);
    const float held = uiHitWatchStep(h, 1.0, 1.1);
    for (int i = 0; i < 10; ++i) CHECK(uiHitWatchStep(h, 1.0, 1.1) == held);
    // A retry resets the stamp to "no hit": no flash, whether or not the view also clears its seed.
    CHECK(uiHitFlashLevel(uiHitWatchStep(w, -1e9, t += 0.016)) == 0.f);
    UiHitWatch r;
    uiHitWatchStep(r, 2.0, 2.0);
    CHECK(uiHitFlashLevel(uiHitWatchStep(r, 2.1, 2.1)) == 1.f);
    r.seeded = false;
    CHECK(uiHitFlashLevel(uiHitWatchStep(r, -1e9, 2.2)) == 0.f);
    // A stamp that moves earlier but stays non-negative is no hit either.
    UiHitWatch e;
    uiHitWatchStep(e, 5.0, 5.0);
    CHECK(uiHitFlashLevel(uiHitWatchStep(e, 0.5, 5.1)) == 0.f);
    // The new fight's first hit, earlier on its clock than the last fight's, flashes; its clock ran back, which ages 0.
    CHECK(uiHitFlashLevel(uiHitWatchStep(w, 0.4, 0.4)) == 1.f);
    // At most three cues a second: a later hit under kUiHitRepeat after the last cue's start comes in with no new cue.
    UiHitWatch q;
    uiHitWatchStep(q, -1.0, 0.9);
    CHECK(uiHitWatchStep(q, 1.0, 1.0) == 0.f);
    CHECK(uiHitWatchStep(q, 1.0, 1.2) == doctest::Approx(0.2f));
    CHECK(uiHitWatchStep(q, 1.25, 1.3) == doctest::Approx(0.3f)); // too soon: the age runs on
    CHECK(q.seen == 1.25);
    CHECK(uiHitWatchStep(q, 1.25, 1.4) == doctest::Approx(0.4f));
    CHECK(uiHitWatchStep(q, 1.5, 1.5) == 0.f); // 0.5 s after the last start: a new cue
}

TEST_CASE("ui style: the clock step: 0 first, 0 on no change or a clock run back, else the change") {
    double last = -1.0;
    CHECK(uiClockStep(last, 2.0) == 0.f);
    CHECK(uiClockStep(last, 2.25) == doctest::Approx(0.25f));
    CHECK(uiClockStep(last, 2.25) == 0.f);
    CHECK(uiClockStep(last, 1.0) == 0.f); // a retry's clock
    CHECK(last == 1.0);
    CHECK(uiClockStep(last, 1.5) == doctest::Approx(0.5f));
}

TEST_CASE("ui style: each hit's lost health in red for its own 350 ms, a heal clears it") {
    UiHitSegs s;
    double t = 0.0;
    uiHitSegsStep(s, 1.f, t += 0.016); // the first step only reads
    for (const UiHitSeg& g : s.seg) CHECK(uiHitFlashLevel(g.age) == 0.f);
    uiHitSegsStep(s, 0.8f, t += 0.016);
    CHECK(s.seg[0].lo == doctest::Approx(0.8f));
    CHECK(s.seg[0].hi == doctest::Approx(1.f));
    CHECK(uiHitFlashLevel(s.seg[0].age) == 1.f);
    uiHitSegsStep(s, 0.7f, t += 0.1); // a second blow 100 ms later: its own segment, the first one still live
    CHECK(s.seg[1].lo == doctest::Approx(0.7f));
    CHECK(s.seg[1].hi == doctest::Approx(0.8f));
    CHECK(uiHitFlashLevel(s.seg[1].age) == 1.f);
    CHECK(uiHitFlashLevel(s.seg[0].age) > 0.f);
    for (int i = 0; i < 16; ++i) uiHitSegsStep(s, 0.7f, t += 0.016); // 256 ms more: the first gone, the second fading
    CHECK(uiHitFlashLevel(s.seg[0].age) == 0.f);
    CHECK(uiHitFlashLevel(s.seg[1].age) > 0.f);
    for (int i = 0; i < 10; ++i) uiHitSegsStep(s, 0.7f, t += 0.016);
    CHECK(uiHitFlashLevel(s.seg[1].age) == 0.f);
    for (int i = 0; i < 6; ++i) uiHitSegsStep(s, 0.6f - 0.05f * static_cast<float>(i), t += 0.016); // more blows than the ring holds
    int live = 0;
    for (const UiHitSeg& g : s.seg) live += uiHitFlashLevel(g.age) > 0.f ? 1 : 0;
    CHECK(live == static_cast<int>(kUiHitSegs));
    uiHitSegsStep(s, 0.9f, t += 0.016); // a heal
    for (const UiHitSeg& g : s.seg) CHECK(uiHitFlashLevel(g.age) == 0.f);
}

TEST_CASE("ui style: lost-hp segments: a step at the same time ages nothing; 0.25 s drains to 0.25 of the life; NaN hp") {
    UiHitSegs s;
    uiHitSegsStep(s, 1.f, 10.0);
    uiHitSegsStep(s, 0.5f, 10.0); // a blow
    CHECK(s.seg[0].age == 0.f);
    uiHitSegsStep(s, 0.5f, 10.0); // dt 0
    CHECK(s.seg[0].age == 0.f);
    uiHitSegsStep(s, 0.5f, 10.25);
    CHECK(s.seg[0].age == doctest::Approx(0.25f));
    // Its drawn length lo + (hi - lo) * level: a quarter of a second in, the level by the flash's curve.
    CHECK(uiHitFlashLevel(s.seg[0].age) == doctest::Approx(uiHitFlashLevel(0.25f)));
    const float before = s.last;
    const std::uint8_t next = s.next;
    uiHitSegsStep(s, std::nanf(""), 10.3);
    CHECK(s.last == before);
    CHECK(s.next == next);
}

TEST_CASE("ui style: the boss's percent: floor, at least 1 alive, 0 dead, none with no hpMax") {
    CHECK(uiBossPercent(600, 600) == 100);
    CHECK(uiBossPercent(599, 600) == 99);
    CHECK(uiBossPercent(1, 600) == 1); // 0.17 percent alive shows 1
    CHECK(uiBossPercent(0, 600) == 0);
    CHECK(uiBossPercent(-5, 600) == 0);
    CHECK(uiBossPercent(700, 600) == 100);
    CHECK(uiBossPercent(10, 0) == -1);
    CHECK(uiBossPercent(2000000000, 2000000001) == 99); // no overflow
}

TEST_CASE("ui style: an end cap follows the fill's end and sits on the track at 0 and full") {
    CHECK(uiCapX(100.f, 600.f, 0.f, 12.f) == doctest::Approx(106.f));
    CHECK(uiCapX(100.f, 600.f, 1.f, 12.f) == doctest::Approx(694.f));
    CHECK(uiCapX(100.f, 600.f, 0.5f, 12.f) == doctest::Approx(400.f));
    CHECK(uiCapX(100.f, 600.f, std::nanf(""), 12.f) == doctest::Approx(106.f));
    const UiStyle s = defaultUiStyle();
    CHECK(s.lines.capGrow == 4.f);
    CHECK(s.lines.guardDot == 3.f);
    CHECK(s.lines.guardPitch == 7.f);
}

TEST_CASE("ui style: the signature devices are off in every world until a pick flips one row") {
    std::string err;
    std::string text;
    REQUIRE(ftcore_test::readProjectData("UI/style.json", text));
    UiStyle shipped;
    REQUIRE_MESSAGE(importUiStyle(text, shipped, &err), err);
    for (const UiStyle& s : {defaultUiStyle(), shipped}) {
        for (const UiWorldRules& r : s.rules) {
            CHECK_FALSE(r.sigPulse);
            CHECK_FALSE(r.sigArcs);
            CHECK_FALSE(r.sigMarginRule);
        }
        CHECK(s.marginRule.gap == 12.f);
        CHECK(s.marginRule.width == 2.f);
        CHECK(s.marginRule.alpha == doctest::Approx(0.6f));
    }
    UiStyle one;
    REQUIRE_MESSAGE(importUiStyle(R"({"worlds": {"lattice": {"sig_arcs": true, "danger_underline": true}}})", one, &err), err);
    CHECK(one.rules[1].sigArcs);
    CHECK_FALSE(one.rules[0].sigArcs);
    CHECK_FALSE(importUiStyle(R"({"worlds": {"lattice": {"sig_arcs": 1}}})", one, &err));
    CHECK_FALSE(importUiStyle(R"({"lines": {"guard_dot": 7, "guard_pitch": 7}})", one, &err));
}

TEST_CASE("ui style: the Release pulse: a rising edge sends it, the peak grows after, repeats while overloaded") {
    UiReleasePulse p;
    CHECK(uiReleasePulseStep(p, false, false, 1.0).travel < 0.f);
    UiReleasePulseView v = uiReleasePulseStep(p, true, false, 2.0); // opens
    CHECK(v.travel == 0.f);
    CHECK(v.peak == 1.f);
    v = uiReleasePulseStep(p, true, false, 2.15);
    CHECK(v.travel == doctest::Approx(0.5f));
    const UiReleasePulseView same = uiReleasePulseStep(p, true, false, 2.15); // dt 0 adds nothing
    CHECK(same.travel == v.travel);
    v = uiReleasePulseStep(p, true, false, 2.35); // arrived: the peak grows
    CHECK(v.travel < 0.f);
    CHECK(v.peak == kUiReleaseGrow);
    CHECK(uiReleasePulseStep(p, true, false, 2.5).peak == 1.f); // 0.15 s later, back
    CHECK(uiReleasePulseStep(p, true, false, 9.0).travel < 0.f); // no second pulse while it stays open
    // Overloaded: the 0.15 s peak every 1.2 s.
    CHECK(uiReleasePulseStep(p, true, true, 10.0).peak == kUiReleaseGrow);
    CHECK(uiReleasePulseStep(p, true, true, 10.2).peak == 1.f);
    CHECK(uiReleasePulseStep(p, true, true, 11.25).peak == kUiReleaseGrow);
    CHECK(uiReleasePulseStep(p, true, true, 11.4).peak == 1.f);
    // Shut and open again: a new pulse.
    uiReleasePulseStep(p, false, false, 12.0);
    CHECK(uiReleasePulseStep(p, true, false, 13.0).travel == 0.f);
}

TEST_CASE("ui style: ring arcs: exact ends, the sag at the middle, concentric lines keep their gap") {
    const float chord = 768.f;
    const float sag = uiArcSag(chord, 3.f * 1920.f);
    CHECK(sag == doctest::Approx(12.81f).epsilon(0.01)); // about 13 px over 768 at radius 3 W
    const UiRingPoint a = uiRingPoint(0.f, chord, sag);
    const UiRingPoint b = uiRingPoint(1.f, chord, sag);
    CHECK(a.x == doctest::Approx(0.f).epsilon(1e-4));
    CHECK(a.y == doctest::Approx(0.f).epsilon(1e-4));
    CHECK(b.x == doctest::Approx(chord).epsilon(1e-5));
    CHECK(std::fabs(b.y) < 1e-3f);
    const UiRingPoint m = uiRingPoint(0.5f, chord, sag);
    CHECK(m.x == doctest::Approx(chord / 2.f));
    CHECK(m.y == doctest::Approx(sag));
    CHECK(m.tx == doctest::Approx(1.f));
    // A concentric line `gap` outward stays `gap` from the first at every t.
    for (int i = 0; i <= 10; ++i) {
        const float t = static_cast<float>(i) / 10.f;
        const UiRingPoint p0 = uiRingPoint(t, chord, sag);
        const UiRingPoint p1 = uiRingPoint(t, chord, sag, 14.f);
        CHECK(std::hypot(p1.x - p0.x, p1.y - p0.y) == doctest::Approx(14.f).epsilon(1e-3));
        CHECK(std::hypot(p0.tx, p0.ty) == doctest::Approx(1.f));
    }
    // Sag 0 is the straight line.
    const UiRingPoint flat = uiRingPoint(0.25f, chord, 0.f);
    CHECK(flat.x == doctest::Approx(192.f));
    CHECK(flat.y == 0.f);
    CHECK(uiArcSegments(chord, 4.f) == 192);
    CHECK(uiArcSegments(0.f, 4.f) == 1);
    CHECK(uiArcSag(100.f, 10.f) == 0.f); // no circle that small: straight
}
