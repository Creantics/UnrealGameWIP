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
    for (const auto& [a, b] : {std::pair{s.loadGold, d.loadGold}, std::pair{s.loadBright, d.loadBright}}) {
        CHECK(a.r == doctest::Approx(b.r).epsilon(0.003));
        CHECK(a.g == doctest::Approx(b.g).epsilon(0.003));
        CHECK(a.b == doctest::Approx(b.b).epsilon(0.003));
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
