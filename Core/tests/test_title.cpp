// The title menu's rules and the save check that decides whether Continue is enabled. Invented
// ids only.

#include "ftcore/StoryDatabase.h"
#include "ftcore/StoryRunner.h"
#include "ftcore/StorySave.h"
#include "ftcore/TitleMenu.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <string>

using namespace ftcore;

TEST_CASE("title menu without a save") {
    TitleMenu m;
    CHECK(m.selected() == TitleItem::NewGame);
    CHECK(m.enabled(TitleItem::NewGame));
    CHECK(m.enabled(TitleItem::Settings));
    CHECK_FALSE(m.enabled(TitleItem::Continue));
    CHECK(m.press(TitleKey::Up) == TitleAction::None);     // at the top
    CHECK(m.press(TitleKey::Pick2) == TitleAction::None);  // a greyed shortcut does nothing
    CHECK(m.selected() == TitleItem::NewGame);
    CHECK(m.press(TitleKey::Confirm) == TitleAction::NewGame);
    CHECK(m.press(TitleKey::Pick1) == TitleAction::NewGame);
    CHECK(m.press(TitleKey::Down) == TitleAction::Moved);  // steps over the greyed Continue
    CHECK(m.selected() == TitleItem::Settings);
    CHECK(m.press(TitleKey::Down) == TitleAction::None);   // at the bottom
    CHECK(m.press(TitleKey::Confirm) == TitleAction::Settings);
    CHECK(m.press(TitleKey::Up) == TitleAction::Moved);
    CHECK(m.selected() == TitleItem::NewGame);
    m.hover(TitleItem::Continue); // greyed: the mouse does not select it
    CHECK(m.selected() == TitleItem::NewGame);
    m.hover(TitleItem::Settings);
    CHECK(m.selected() == TitleItem::Settings);
}

TEST_CASE("title menu with a save") {
    TitleMenu m;
    m.setContinueEnabled(true);
    CHECK(m.selected() == TitleItem::Continue); // starts on Continue
    CHECK(m.press(TitleKey::Confirm) == TitleAction::Continue);
    CHECK(m.press(TitleKey::Down) == TitleAction::Moved);
    CHECK(m.selected() == TitleItem::Settings);
    CHECK(m.press(TitleKey::Down) == TitleAction::None); // at the bottom: no wrap
    CHECK(m.press(TitleKey::Pick3) == TitleAction::Settings);
    CHECK(m.press(TitleKey::Up) == TitleAction::Moved);
    CHECK(m.press(TitleKey::Up) == TitleAction::Moved);
    CHECK(m.selected() == TitleItem::NewGame);
    CHECK(m.press(TitleKey::Up) == TitleAction::None);
    CHECK(m.press(TitleKey::Confirm) == TitleAction::NewGame);
    CHECK(m.press(TitleKey::Pick2) == TitleAction::Continue); // shortcuts ignore the selection
    CHECK(m.selected() == TitleItem::NewGame);
    CHECK(m.press(TitleKey::Down) == TitleAction::Moved);
    CHECK(m.selected() == TitleItem::Continue);

    m.setContinueEnabled(false); // the save went bad: selection falls back
    CHECK(m.selected() == TitleItem::NewGame);
    CHECK(m.press(TitleKey::Pick2) == TitleAction::None);
}

namespace {

void buildDb(StoryDatabase& db) {
    StoryBuilder b;
    b.setTitle("Fixture");
    b.setStart("SC-A");
    b.addFlag("F_COUNT", FlagValue::integer(1));
    b.beginChapter("01", "Chapter 1");
    b.beginScene("SC-A", "Alpha", {"Waking", "Hall", "dawn", "CMB", {}});
    b.line("VO-A-001", "SPK1", "One.");
    b.line("VO-A-002", "SPK1", "Two.");
    b.beginScene("SC-B", "Beta", {"Waking", "Yard", "noon", "ENG", {}});
    b.line("VO-B-001", "SPK1", "Three.");
    std::string error;
    const bool ok = b.finish(db, &error);
    INFO(error);
    REQUIRE(ok);
}

} // namespace

TEST_CASE("checkStorySave refuses what can't be loaded") {
    StoryDatabase db;
    buildDb(db);

    StorySave s;
    s.scene = "SC-B";
    s.beat = 0;
    const std::string good = encodeStorySave(s);

    StorySave got;
    CHECK(checkStorySave(good, &db, &got) == SaveRefusal::None);
    CHECK(got.scene == "SC-B");

    got = StorySave{};
    CHECK(checkStorySave("", &db, &got) == SaveRefusal::Empty);
    CHECK(checkStorySave("{not json", &db, &got) == SaveRefusal::Invalid);
    CHECK(checkStorySave("{\"format\":9,\"scene\":\"SC-A\",\"beat\":0,\"flags\":[]}", &db, &got) == SaveRefusal::Invalid);
    CHECK(checkStorySave("{\"format\":1,\"scene\":\"SC-A\",\"flags\":[]}", &db, &got) == SaveRefusal::Invalid);
    CHECK(checkStorySave(good, nullptr, &got) == SaveRefusal::NoStory);
    StoryDatabase empty;
    CHECK(checkStorySave(good, &empty, &got) == SaveRefusal::NoStory);

    s.scene = "SC-GONE";
    CHECK(checkStorySave(encodeStorySave(s), &db, &got) == SaveRefusal::SceneMissing);
    s.scene = "SC-A";
    s.beat = 500;
    CHECK(checkStorySave(encodeStorySave(s), &db, &got) == SaveRefusal::BeatOutOfRange);
    CHECK(got.scene.empty()); // untouched by every refusal

    CHECK(std::string(saveRefusalName(SaveRefusal::None)) == "ok");
    CHECK(std::string(saveRefusalName(SaveRefusal::SceneMissing)) == "scene_missing");
}

TEST_CASE("a save taken at a scene start round-trips through the runner") {
    StoryDatabase db;
    buildDb(db);
    StoryRunner r(&db);
    REQUIRE(r.start("SC-B"));
    StorySave s;
    REQUIRE(captureStory(r, s));
    const std::string text = encodeStorySave(s);
    CHECK(checkStorySave(text, &db) == SaveRefusal::None);

    StoryRunner q(&db);
    StorySave decoded;
    REQUIRE(decodeStorySave(text, decoded));
    REQUIRE(applyStory(decoded, q));
    CHECK(q.position().scene == "SC-B");
    CHECK(q.position().beat == 0);
}
