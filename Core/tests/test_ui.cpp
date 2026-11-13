#include "ftcore/ArenaFight.h"
#include "ftcore/Dialogue.h"
#include "ftcore/InputBindings.h"
#include "ftcore/Load.h"
#include "ftcore/ReadCards.h"
#include "ftcore/StoryDatabase.h"
#include "ftcore/StoryRunner.h"
#include "ftcore/UiGlyphs.h"
#include "ftcore/UiStyle.h"
#include "ftcore/UiViews.h"

#include "AllocCounter.h"
#include "TestAssets.h"

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
    CHECK(std::string_view(s.plate[0].data()) == "bronze");
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

TEST_CASE("ui style: a colour-vision palette changes only the tokens it lists; layers are data") {
    UiStyle s;
    std::string err;
    REQUIRE_MESSAGE(importUiStyle(R"({"palettes": {"deutan": {"danger": "#F0A030"}},
        "layers": {"plates": true, "world_anchored": true, "hairlines": false}})", s, &err), err);
    const UiColor d = uiColor(s, UiWorld::Solhaven, UiPalette::Deutan, UiToken::Danger);
    CHECK(d.g == doctest::Approx(160.f / 255.f));
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
    CHECK(gift.text() == "G");
    CHECK(uiGlyph(t, InputAction::FocusNext, InputDevice::Pad).shape == UiGlyphShape::Stick);
    CHECK(uiGlyph(t, InputAction::FocusNext, InputDevice::Pad).dir == 2);

    // Nothing changed: no new revision.
    CHECK_FALSE(buildUiGlyphTable(b, t));
    CHECK(t.revision == 1);
    // A rebind shows on the next build.
    REQUIRE(rebindInput(b, InputAction::Gift, InputDevice::Keys, "H"));
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    CHECK(buildUiGlyphTable(b, t));
    if (counting) CHECK(ftcore_test::allocStats().count == before);
    CHECK(t.revision == 2);
    CHECK(uiGlyph(t, InputAction::Gift, InputDevice::Keys).text() == "H");
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
    CHECK(v.beatRevision == beat);
    const std::uint32_t textRev = v.textRevision;
    CHECK_FALSE(updateDialogueView(v, p, flags)); // nothing new
    CHECK(v.textRevision == textRev);

    p.update(5.f);
    CHECK(updateDialogueView(v, p, flags));
    CHECK(v.complete);
    CHECK(v.continuePrompt);
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
