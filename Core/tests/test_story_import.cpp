#include "ftcore/Json.h"
#include "ftcore/StoryImport.h"
#include "ftcore/StoryRunner.h"
#include "ftcore/StorySave.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

using namespace ftcore;

namespace {

// Test fixture (the newengine story test's): two chapters, four scenes, every beat type.
constexpr const char* kStory = R"J({
  "format": 1, "title": "Fixture", "start": "SC-A", "futureField": {"x": 1},
  "chapters": [
    { "id": "01", "title": "Chapter 1 · One", "scenes": ["SC-A", "SC-B"] },
    { "id": "02", "title": "Chapter 2 · Two", "scenes": ["SC-C", "SC-D"] }
  ],
  "flags": [ { "id": "F_BOOL", "default": false }, { "id": "F_COUNT", "default": 3 },
             { "id": "F_ENUM", "default": "UNSET" } ],
  "cues": [ { "id": "MUS-X", "kind": "music" } ]
})J";

constexpr const char* kSceneA = R"J({
  "id": "SC-A", "title": "Alpha", "chapter": "01", "world": "Waking", "location": "Hall", "time": "dawn",
  "mode": "CMB → ENG", "gameplay": "One short wave.", "cast": ["MIRA"], "props": [], "cues": ["MUS-X"],
  "beats": [
    { "type": "direction", "text": "Open *now*.", "loc": "story.SC-A.d001" },
    { "type": "direction", "text": "[OPTIONAL: note]", "note": "OPTIONAL", "hidden": true },
    { "type": "cue", "id": "MUS-X", "action": "play" },
    { "type": "line", "id": "VO-A-001", "speaker": "MIRA", "direction": "quietly", "text": "Hello.", "loc": "story.SC-A.VO-A-001" },
    { "type": "hold", "seconds": 1.0, "note": "Tell" },
    { "type": "choice", "label": "Well?", "loc": "story.SC-A.c001", "scripted": false, "options": [
        { "text": "Ask again", "loc": "story.SC-A.c001.o1", "effects": [], "next": 5 },
        { "text": "Go", "loc": "story.SC-A.c001.o2", "effects": [ { "op": "set", "id": "F_ENUM", "value": "SAT" } ], "next": null },
        { "text": "Greyed", "loc": "story.SC-A.c001.o3", "effects": [], "next": null, "selectable": false } ] },
    { "type": "flag", "op": "require", "id": "F_BOOL", "value": true },
    { "type": "line", "id": "VO-A-002", "speaker": "MIRA", "text": "Skipped.", "loc": "story.SC-A.VO-A-002" },
    { "type": "flag", "op": "end" },
    { "type": "choice", "label": "Nobody", "loc": "story.SC-A.c002", "scripted": false, "options": [
        { "text": "x", "loc": "story.SC-A.c002.o1", "effects": [], "next": null, "selectable": false } ] },
    { "type": "gameplay", "kind": "CMB", "text": "[GAMEPLAY → fight]", "loc": "story.SC-A.g001",
      "params": { "enemy": "wolf", "count": 2, "ratio": 1.50, "none": null, "list": [1, 2] } },
    { "type": "wobble", "text": "unknown" },
    { "type": "end", "next": "SC-D" }
  ] })J";

constexpr const char* kSceneB = R"J({
  "id": "SC-B", "title": "Beta", "chapter": "01", "world": "Waking", "location": "Yard", "time": "noon", "mode": "PLAY",
  "beats": [ { "type": "line", "id": "VO-B-001", "speaker": "TEO", "text": "B.", "loc": "story.SC-B.VO-B-001" },
             { "type": "gameplay", "kind": "PLAY", "text": "[walk]", "loc": "story.SC-B.g001" } ] })J";

constexpr const char* kSceneC = R"J({
  "id": "SC-C", "title": "Gamma", "chapter": "02", "world": "Dream", "location": "Shore", "time": "dusk", "mode": "ENG",
  "beats": [
    { "type": "flag", "op": "set", "id": "F_BOOL", "value": true },
    { "type": "flag", "op": "require", "id": "F_BOOL", "value": true },
    { "type": "line", "id": "VO-C-001", "speaker": "MIRA", "text": "Allowed.", "loc": "story.SC-C.VO-C-001" },
    { "type": "flag", "op": "end" },
    { "type": "flag", "op": "clear", "id": "F_COUNT" },
    { "type": "end" } ] })J";

constexpr const char* kSceneD = R"J({
  "id": "SC-D", "title": "Delta", "chapter": "02", "world": "Dream", "location": "Shore", "time": "night", "mode": "MINI",
  "beats": [ { "type": "hold", "seconds": 0 }, { "type": "line", "id": "VO-D-001", "speaker": "MIRA", "text": "D.", "loc": "story.SC-D.VO-D-001" } ] })J";

struct Fixture {
    StoryDatabase db;
    std::vector<std::string> warnings;
    Fixture() {
        // Scenes given out of play order: matched by id.
        const std::array<std::string_view, 4> scenes{kSceneD, kSceneA, kSceneC, kSceneB};
        std::string err;
        REQUIRE_MESSAGE(importStory(kStory, scenes, db, &err, &warnings), err);
    }
};

bool contains(const std::vector<std::string>& lines, std::string_view part) {
    for (const std::string& l : lines)
        if (l.find(part) != std::string::npos) return true;
    return false;
}

} // namespace

TEST_CASE("story import resolves scenes, text and indices") {
    Fixture f;
    const StoryDatabase& db = f.db;
    CHECK(db.title() == "Fixture");
    REQUIRE(db.sceneCount() == 4);
    CHECK(db.view(db.scene(db.startScene()).id) == "SC-A");
    CHECK(db.findScene("SC-C") == 2);
    CHECK(db.findScene("SC-Z") == kNone);
    CHECK(db.chapterCount() == 2);
    CHECK(db.view(db.chapter(1).title) == "Chapter 2 · Two");
    CHECK(db.chapter(1).firstScene == 2);
    const Scene& a = db.scene(0);
    CHECK(db.view(a.title) == "Alpha");
    CHECK(db.view(a.mode) == "CMB → ENG");
    CHECK(db.view(a.gameplay) == "One short wave.");
    CHECK(db.view(a.world) == "Waking");
    CHECK(db.view(a.location) == "Hall");
    CHECK(db.view(a.time) == "dawn");
    CHECK(db.view(a.chapter) == "01");
    REQUIRE(a.beatCount == 13);
    CHECK(db.view(db.beat(a, 0).text) == "Open now."); // emphasis stripped
    CHECK(db.view(db.beat(a, 0).loc) == "story.SC-A.d001");
    CHECK(db.beat(a, 1).hidden);
    CHECK(db.view(db.beat(a, 1).note) == "OPTIONAL");
    CHECK(db.beat(a, 2).type == BeatType::Cue);
    const Beat& line = db.beat(a, 3);
    CHECK(line.type == BeatType::Line);
    CHECK(db.view(line.id) == "VO-A-001");
    CHECK(db.view(line.speaker) == "MIRA");
    CHECK(db.view(line.direction) == "quietly");
    CHECK(db.beat(a, 4).seconds == doctest::Approx(1.f));
    CHECK(db.view(db.beat(a, 4).note) == "Tell");
    const Beat& choice = db.beat(a, 5);
    REQUIRE(choice.type == BeatType::Choice);
    CHECK(db.view(choice.text) == "Well?"); // from `label`
    REQUIRE(choice.optionCount == 3);
    CHECK(db.option(choice, 0).next == 5);
    CHECK(db.option(choice, 1).next == kNone);
    REQUIRE(db.option(choice, 1).effectCount == 1);
    CHECK(db.flagString(db.effect(db.option(choice, 1), 0).value) == "SAT");
    CHECK_FALSE(db.option(choice, 2).selectable);
    CHECK(db.beat(a, 6).flagOp.op == FlagOp::Require);
    CHECK(db.beat(a, 8).flagOp.op == FlagOp::End);
    CHECK(db.beat(a, 8).flagOp.flag == kNone);
    const Beat& fight = db.beat(a, 10);
    CHECK(fight.kind == GameplayKind::Combat);
    CHECK(db.param(fight, "enemy") == "wolf");
    CHECK(db.param(fight, "count") == "2");
    CHECK(db.param(fight, "ratio") == "1.50");
    CHECK(db.param(fight, "none").empty());
    CHECK(db.param(fight, "list") == "[1, 2]");
    CHECK(fight.paramCount == 5);
    CHECK(db.beat(a, 11).type == BeatType::Unknown);
    CHECK(db.beat(a, 12).nextScene == db.findScene("SC-D"));
    CHECK(db.flagCount() == 3);
    CHECK(db.flag(db.findFlag("F_BOOL")).initial == FlagValue::boolean(false));
    CHECK(db.flag(db.findFlag("F_COUNT")).initial == FlagValue::integer(3));
    CHECK(db.flagString(db.flag(db.findFlag("F_ENUM")).initial) == "UNSET");
    CHECK(db.cueCount() == 1);
    CHECK(db.view(db.cue(0).kind) == "music");
    const Scene& c = db.scene(2);
    CHECK(db.beat(c, 4).flagOp.op == FlagOp::Clear);
    CHECK(db.beat(c, 4).flagOp.value == FlagValue::boolean(false));
    CHECK(contains(f.warnings, "unknown beat type 'wobble'"));
}

TEST_CASE("story import feeds a runner that plays the fixture") {
    Fixture f;
    StoryRunner r(&f.db);
    REQUIRE(r.start());
    CHECK(r.text() == "Open now.");
    CHECK(r.advance());
    CHECK(r.text() == "Hello.");
    CHECK(r.advance());
    r.skipHold();
    REQUIRE(r.state() == StoryRunner::State::Choice);
    CHECK(r.choose(1));
    CHECK(f.db.flagString(r.flag("F_ENUM")) == "SAT");
    REQUIRE(r.state() == StoryRunner::State::Gameplay); // failed require skipped, greyed choice passed
    CHECK(r.param("enemy") == "wolf");
    CHECK(r.resume());
    CHECK(r.position().scene == "SC-D");
    CHECK(r.text() == "D.");
}

TEST_CASE("story import keeps emphasis when asked") {
    const std::array<std::string_view, 4> scenes{kSceneA, kSceneB, kSceneC, kSceneD};
    StoryDatabase db;
    REQUIRE(importStory(kStory, scenes, db, nullptr, nullptr, true));
    CHECK(db.view(db.beat(db.scene(0), 0).text) == "Open *now*.");
}

TEST_CASE("story import lists scene ids in chapter order") {
    std::vector<std::string> ids;
    REQUIRE(storySceneIds(kStory, ids));
    CHECK(ids == std::vector<std::string>{"SC-A", "SC-B", "SC-C", "SC-D"});
    std::string err;
    CHECK_FALSE(storySceneIds("{", ids, &err));
    CHECK(err.find("story.json: line 1") == 0);
}

TEST_CASE("story import reports errors and warnings") {
    const std::array<std::string_view, 4> all{kSceneA, kSceneB, kSceneC, kSceneD};
    StoryDatabase db;
    std::string err;

    SUBCASE("bad story JSON") {
        CHECK_FALSE(importStory("{\"chapters\": [", all, db, &err));
        CHECK(err.find("story.json: line 1") == 0);
    }
    SUBCASE("no chapters") {
        CHECK_FALSE(importStory(R"({"title": "x"})", all, db, &err));
        CHECK(err.find("chapters") != std::string::npos);
    }
    SUBCASE("a listed scene without a document") {
        const std::array<std::string_view, 3> three{kSceneA, kSceneB, kSceneC};
        CHECK_FALSE(importStory(kStory, three, db, &err));
        CHECK(err.find("scene 'SC-D' has no scene document") != std::string::npos);
    }
    SUBCASE("a scene listed twice") {
        constexpr const char* story = R"({"chapters": [{"id": "1", "scenes": ["SC-B", "SC-B"]}]})";
        const std::array<std::string_view, 1> one{kSceneB};
        CHECK_FALSE(importStory(story, one, db, &err));
        CHECK(err.find("listed twice") != std::string::npos);
    }
    SUBCASE("a scene document given twice") {
        const std::array<std::string_view, 5> five{kSceneA, kSceneB, kSceneC, kSceneD, kSceneB};
        CHECK_FALSE(importStory(kStory, five, db, &err));
        CHECK(err.find("given twice") != std::string::npos);
    }
    SUBCASE("an unknown start scene") {
        constexpr const char* story = R"({"start": "SC-Q", "chapters": [{"id": "1", "scenes": ["SC-B"]}]})";
        const std::array<std::string_view, 1> one{kSceneB};
        CHECK_FALSE(importStory(story, one, db, &err));
        CHECK(err.find("unknown start scene 'SC-Q'") != std::string::npos);
    }
    SUBCASE("bad beats") {
        constexpr const char* story = R"({"chapters": [{"id": "1", "scenes": ["SC-X"]}]})";
        constexpr const char* scene = R"({"id": "SC-X", "beats": [
            { "type": "flag", "op": "toggle", "id": "F" },
            { "type": "flag", "op": "set" },
            { "type": "flag", "op": "set", "id": "F", "value": 1.5 },
            { "type": "choice", "label": "?", "options": [ { "text": "a", "next": -1 } ] } ]})";
        const std::array<std::string_view, 1> one{scene};
        CHECK_FALSE(importStory(story, one, db, &err));
        CHECK(err.find("SC-X: beats[0].op") != std::string::npos);
        CHECK(err.find("SC-X: beats[1].id: required") != std::string::npos);
        CHECK(err.find("SC-X: beats[2].value") != std::string::npos);
        CHECK(err.find("SC-X: beats[3].options[0].next") != std::string::npos);
    }
    SUBCASE("warnings only") {
        constexpr const char* story = R"({"format": 2, "chapters": [{"id": "1", "scenes": ["SC-X"]}]})";
        constexpr const char* scene = R"({"id": "SC-X", "beats": [
            { "type": "cue", "id": "SFX", "action": "loop" },
            { "type": "choice", "label": "?", "options": [ { "text": "a", "next": 9 } ] },
            { "type": "end", "next": "SC-NOWHERE" } ]})";
        const std::array<std::string_view, 2> two{scene, kSceneB};
        std::vector<std::string> warnings;
        REQUIRE_MESSAGE(importStory(story, two, db, &err, &warnings), err);
        CHECK(contains(warnings, "format"));
        CHECK(contains(warnings, "unknown cue action 'loop'"));
        CHECK(contains(warnings, "scene 'SC-B' is not in any chapter"));
        CHECK(contains(warnings, "past the scene's end"));
        CHECK(contains(warnings, "unknown scene 'SC-NOWHERE'"));
        const Scene& x = db.scene(0);
        CHECK(db.option(db.beat(x, 1), 0).next == 3);
        CHECK(db.beat(x, 2).nextScene == kNone);
    }
    if (!err.empty()) CHECK_FALSE(db.loaded());
}

TEST_CASE("a miss option corrects and moves on; the autoplay option is the right one") {
    constexpr const char* story = R"J({ "format": 1, "title": "Ex", "start": "SC-X",
      "chapters": [ { "id": "01", "title": "One", "scenes": ["SC-X"] } ],
      "flags": [ { "id": "F_DONE", "default": false } ] })J";
    constexpr const char* scene = R"J({ "id": "SC-X", "title": "X", "chapter": "01", "beats": [
      { "type": "choice", "label": "Who?", "scripted": false, "options": [
          { "text": "Wrong", "effects": [], "next": null, "miss": true },
          { "text": "Right", "effects": [], "next": 3 } ] },
      { "type": "line", "id": "PH-X-001", "speaker": "ZEE", "text": "Try again." },
      { "type": "choice", "label": "Who?", "scripted": false, "options": [
          { "text": "Wrong", "effects": [], "next": null, "miss": true },
          { "text": "Right", "effects": [], "next": 3 } ] },
      { "type": "flag", "op": "set", "id": "F_DONE", "value": true },
      { "type": "line", "id": "PH-X-002", "speaker": "ZEE", "text": "Yes." },
      { "type": "end" } ] })J";
    const std::array<std::string_view, 1> scenes{scene};
    StoryDatabase db;
    std::string err;
    REQUIRE_MESSAGE(importStory(story, scenes, db, &err), err);
    const Scene& sc = db.scene(0);
    CHECK(db.option(db.beat(sc, 0), 0).miss);
    CHECK_FALSE(db.option(db.beat(sc, 0), 1).miss);

    StoryRunner r(&db);
    REQUIRE(r.start());
    REQUIRE(r.state() == StoryRunner::State::Choice);
    CHECK(r.optionReturns(0));
    CHECK_FALSE(r.optionReturns(1));
    CHECK(r.defaultOption() == 1);
    CHECK(r.choose(0));
    CHECK(r.state() == StoryRunner::State::Line);
    CHECK(r.text() == "Try again.");
    CHECK(r.advance());
    REQUIRE(r.state() == StoryRunner::State::Choice);
    CHECK(r.beatIndex() == 2);
    CHECK(r.choose(r.defaultOption()));
    CHECK(r.flag("F_DONE").truthy());
    CHECK(r.text() == "Yes.");

    REQUIRE(r.start());
    CHECK(r.choose(1));
    CHECK(r.text() == "Yes.");
}

// ---- the real script (newengine assets/story) ---------------------------------------------------

namespace {

struct RealStory {
    StoryDatabase db;
    bool present = false;
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
};

// Loaded once: the script is large and every test only reads it.
const RealStory& realStory() {
    static const RealStory story = [] {
        RealStory s;
        std::string storyText;
        if (!ftcore_test::readAsset("story/story.json", storyText)) return s;
        s.present = true;
        std::vector<std::string> ids;
        if (!storySceneIds(storyText, ids, &s.error)) return s;
        std::vector<std::string> texts(ids.size());
        for (std::size_t i = 0; i < ids.size(); ++i)
            if (!ftcore_test::readAsset("story/scenes/" + ids[i] + ".json", texts[i])) {
                s.error = "cannot read scenes/" + ids[i] + ".json";
                return s;
            }
        const std::vector<std::string_view> views(texts.begin(), texts.end());
        s.ok = importStory(storyText, views, s.db, &s.error, &s.warnings);
        return s;
    }();
    return story;
}

#define FTCORE_REQUIRE_REAL_STORY(s)                                                                                   \
    const RealStory& s = realStory();                                                                                  \
    if (!s.present) {                                                                                                  \
        MESSAGE("newengine assets/story not present; skipping");                                                       \
        return;                                                                                                        \
    }                                                                                                                  \
    REQUIRE_MESSAGE(s.ok, s.error)

enum class Pick { Right, MissThenRight, AlwaysMiss };

struct SceneRun {
    int choices = 0, misses = 0, moves = 0;
    bool left = false; // the story left the scene (or finished) within the move budget
};

std::uint32_t firstMiss(const StoryRunner& r) {
    for (std::uint32_t i = 0; i < r.optionCount(); ++i)
        if (r.optionSelectable(i) && r.optionReturns(i)) return i;
    return kNone;
}

// Plays one scene from its first beat with the flags as they are (set them first), every choice by `pick`.
SceneRun playScene(StoryRunner& r, std::string_view id, Pick pick) {
    SceneRun run;
    REQUIRE(r.restore({id, 0}));
    const std::uint32_t scene = r.sceneIndex();
    bool lastMissed = false;
    while (!r.finished() && r.sceneIndex() == scene && run.moves < 5000) {
        ++run.moves;
        switch (r.state()) {
        case StoryRunner::State::Line:
        case StoryRunner::State::Direction: r.advance(); break;
        case StoryRunner::State::Hold: r.skipHold(); break;
        case StoryRunner::State::Gameplay: r.resume(); break;
        case StoryRunner::State::Choice: {
            std::uint32_t i = r.defaultOption();
            const std::uint32_t miss = firstMiss(r);
            if (miss != kNone && (pick == Pick::AlwaysMiss || (pick == Pick::MissThenRight && !lastMissed))) i = miss;
            lastMissed = i == miss && miss != kNone;
            ++run.choices;
            if (lastMissed) ++run.misses;
            r.choose(i);
            break;
        }
        default: r.stop(); break;
        }
    }
    run.left = r.finished() || r.sceneIndex() != scene;
    return run;
}

constexpr const char* kExchangeScenes[] = {"SC-09-HE1", "SC-10-HE1", "SC-13-HE1", "SC-15-HE1",
                                           "SC-15-HE2", "SC-18-Q1",  "SC-21-HE1"};

} // namespace

TEST_CASE("real story script loads and plays to the end") {
    FTCORE_REQUIRE_REAL_STORY(s);
    const StoryDatabase& db = s.db;
    CHECK(db.sceneCount() >= 1);
    CHECK(db.beatCount() >= db.sceneCount());
    MESSAGE("scenes " << db.sceneCount() << ", beats " << db.beatCount() << ", flags " << db.flagCount() << ", "
                      << s.warnings.size() << " warnings");
    for (const std::string& w : s.warnings) MESSAGE("warning: " << w);

    // Every shown beat's loc key is in the English string table.
    std::string locText;
    if (ftcore_test::readAsset("story/loc/en.json", locText)) {
        JsonDocument loc;
        std::string err;
        REQUIRE_MESSAGE(loc.parse(locText, &err), err);
        REQUIRE(loc.root().isObject());
        std::size_t missing = 0, keyed = 0;
        for (std::uint32_t sc = 0; sc < db.sceneCount(); ++sc) {
            const Scene& scene = db.scene(sc);
            for (std::uint32_t b = 0; b < scene.beatCount; ++b) {
                const Beat& beat = db.beat(scene, b);
                if (beat.hidden || beat.loc.empty()) continue;
                ++keyed;
                if (!loc.root()[db.view(beat.loc)].isString()) {
                    ++missing;
                    if (missing <= 5) MESSAGE("no string for " << db.view(beat.loc));
                }
            }
        }
        CHECK(keyed > 0);
        CHECK(missing == 0);
    } else {
        MESSAGE("story/loc/en.json not present; loc keys not checked");
    }

    StoryRunner r(&db);
    REQUIRE(r.start());

    // Into the first combat hand-off, the way the RPG autoplay drives it.
    int moves = 0;
    std::uint32_t scenesSeen = 1, lastEpoch = r.sceneEpoch();
    while (!r.finished() && !(r.waitingForGameplay() && r.gameplayKind() == GameplayKind::Combat) && moves < 100000) {
        ++moves;
        switch (r.state()) {
        case StoryRunner::State::Line:
        case StoryRunner::State::Direction: r.advance(); break;
        case StoryRunner::State::Choice: r.choose(r.defaultOption()); break;
        case StoryRunner::State::Hold: r.skipHold(); break;
        case StoryRunner::State::Gameplay: r.resume(); break;
        default: break;
        }
        if (r.sceneEpoch() != lastEpoch) {
            lastEpoch = r.sceneEpoch();
            ++scenesSeen;
        }
    }
    REQUIRE(r.waitingForGameplay());
    CHECK(r.gameplayKind() == GameplayKind::Combat);
    CHECK_FALSE(r.text().empty());
    MESSAGE("first CMB at " << r.position().scene << " beat " << r.position().beat << " after " << scenesSeen
                            << " scenes");

    // ... and on through the whole script without allocating, every choice taking the autoplay option.
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    std::uint32_t choices = 0, holds = 0, gameplay = 0, reshown = 0, emptyText = 0, badReshow = 0, noPick = 0;
    // (no doctest assertions inside the measured loop: count problems and check after)
    while (!r.finished() && moves < 200000) {
        ++moves;
        switch (r.state()) {
        case StoryRunner::State::Line:
        case StoryRunner::State::Direction:
            if (r.text().empty()) ++emptyText;
            r.advance();
            break;
        case StoryRunner::State::Choice: {
            ++choices;
            const std::uint32_t pick = r.defaultOption();
            if (pick == kNone) {
                ++noPick;
                r.stop();
                break;
            }
            // Exercise a returning option once: an option leading back re-shows the prompt; a `miss` (a wrong
            // pick in a dialogue exchange) moves on to the companion's correction, and the loop carries on there.
            bool missed = false, tried = false;
            for (std::uint32_t i = 0; i < r.optionCount() && !tried; ++i)
                if (r.optionSelectable(i) && r.optionReturns(i)) {
                    const std::uint32_t at = r.beatIndex();
                    const bool miss = db.option(db.beat(db.scene(r.sceneIndex()), at), i).miss;
                    r.choose(i);
                    if (!miss && (r.state() != StoryRunner::State::Choice || r.beatIndex() != at)) ++badReshow;
                    if (miss && r.beatIndex() == at) ++badReshow;
                    ++reshown;
                    tried = true;
                    missed = miss;
                }
            if (!missed) r.choose(pick);
            break;
        }
        case StoryRunner::State::Hold:
            ++holds;
            r.update(10.f);
            break;
        case StoryRunner::State::Gameplay:
            ++gameplay;
            r.resume();
            break;
        default: r.stop(); break;
        }
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(emptyText == 0);
    CHECK(badReshow == 0);
    CHECK(noPick == 0);
    if (counting) CHECK(after == before);
    CHECK(r.finished());
    CHECK(choices > 0);
    CHECK(gameplay > 0);
    MESSAGE("played to the end: " << choices << " choices (" << reshown << " re-shown), " << holds << " holds, "
                                  << gameplay << " gameplay beats, " << moves << " moves");
}

namespace {

bool sameRunner(const StoryRunner& a, const StoryRunner& b) {
    if (a.state() != b.state() || a.sceneIndex() != b.sceneIndex() || a.beatIndex() != b.beatIndex()) return false;
    if (a.flagCount() != b.flagCount()) return false;
    for (std::uint32_t i = 0; i < a.flagCount(); ++i)
        if (!(a.flag(i) == b.flag(i))) return false;
    return true;
}

bool sameSave(const StorySave& a, const StorySave& b) {
    if (a.scene != b.scene || a.beat != b.beat || a.flags.size() != b.flags.size()) return false;
    for (std::size_t i = 0; i < a.flags.size(); ++i) {
        const SavedStoryFlag &x = a.flags[i], &y = b.flags[i];
        if (x.id != y.id || x.kind != y.kind || x.number != y.number || x.text != y.text) return false;
    }
    return true;
}

// One autoplay move: the same input for any runner in the same state.
void autoplayMove(StoryRunner& r, std::uint32_t pick) {
    switch (r.state()) {
    case StoryRunner::State::Line:
    case StoryRunner::State::Direction: r.advance(); break;
    case StoryRunner::State::Choice: r.choose(pick); break;
    case StoryRunner::State::Hold: r.skipHold(); break;
    case StoryRunner::State::Gameplay: r.resume(); break;
    default: r.stop(); break;
    }
}

} // namespace

TEST_CASE("a save taken anywhere in the real script loads into a runner that plays on identically") {
    FTCORE_REQUIRE_REAL_STORY(s);
    StoryRunner r(&s.db), loaded(&s.db);
    REQUIRE(r.start());
    // Save at every scene entry and every 37th move; load into a fresh runner, then drive both with the
    // same inputs and compare cursor and flags after every move until the next save point.
    int moves = 0, saves = 0, badCapture = 0, badApply = 0, badResave = 0, diverged = 0;
    std::uint32_t lastEpoch = ~0u;
    bool tracking = false;
    while (!r.finished() && moves < 200000) {
        if (r.sceneEpoch() != lastEpoch || moves % 37 == 0) {
            lastEpoch = r.sceneEpoch();
            StorySave save, resave;
            if (!captureStory(r, save)) {
                ++badCapture;
                break;
            }
            loaded.setDatabase(&s.db);
            StoryLoadReport report;
            if (!applyStory(save, loaded, &report) || !report.skippedFlags.empty()) ++badApply;
            if (!captureStory(loaded, resave) || !sameSave(save, resave)) ++badResave;
            if (!sameRunner(r, loaded)) ++diverged;
            ++saves;
            tracking = true;
        }
        const std::uint32_t pick = r.state() == StoryRunner::State::Choice ? r.defaultOption() : kNone;
        autoplayMove(r, pick);
        if (tracking) {
            autoplayMove(loaded, pick);
            if (!sameRunner(r, loaded)) {
                ++diverged;
                if (diverged <= 3) MESSAGE("diverged at " << r.position().scene << " beat " << r.position().beat);
                tracking = false;
            }
        }
        ++moves;
    }
    CHECK(r.finished());
    CHECK(badCapture == 0);
    CHECK(badApply == 0);
    CHECK(badResave == 0);
    CHECK(diverged == 0);
    CHECK(saves >= static_cast<int>(s.db.sceneCount()) / 2);
    MESSAGE(saves << " saves round-tripped over " << moves << " moves");
}

// ---- dialogue exchanges (the former MINI beats, newengine docs/story_mini_dialogue.md) -----------

TEST_CASE("the former MINI beats are dialogue exchanges in the real script") {
    FTCORE_REQUIRE_REAL_STORY(s);
    const StoryDatabase& db = s.db;
    for (const char* id : kExchangeScenes) {
        CAPTURE(id);
        const std::uint32_t si = db.findScene(id);
        REQUIRE(si != kNone);
        const Scene& sc = db.scene(si);
        int choices = 0, misses = 0, minis = 0, hiddenMini = 0;
        for (std::uint32_t b = 0; b < sc.beatCount; ++b) {
            const Beat& beat = db.beat(sc, b);
            if (beat.type == BeatType::Gameplay && beat.kind == GameplayKind::Mini) ++minis;
            if (beat.type == BeatType::Direction && beat.hidden && db.view(beat.note) == "MINI") ++hiddenMini;
            if (beat.type != BeatType::Choice) continue;
            ++choices;
            CHECK(beat.optionCount <= 4);
            for (std::uint32_t o = 0; o < beat.optionCount; ++o) misses += db.option(beat, o).miss ? 1 : 0;
        }
        CHECK(minis == 0);      // no mini-game hand-off left
        CHECK(hiddenMini >= 1); // the original notation is kept as a designer note
        CHECK(choices >= 2);
        if (std::string_view(id) != "SC-10-HE1") CHECK(misses > 0); // SC-10: no wrong answers
    }
}

TEST_CASE("dialogue exchanges set their flags whichever way the player answers") {
    FTCORE_REQUIRE_REAL_STORY(s);
    struct Done {
        const char* scene;
        const char* flags[2];
    };
    constexpr Done kDone[] = {
        {"SC-09-HE1", {"F_HE_MARGINALIA_DONE", nullptr}},
        {"SC-10-HE1", {"F_HE_RECUT_DONE", nullptr}},
        {"SC-13-HE1", {"F_HE_LANTERN_GUARD_DONE", nullptr}},
        {"SC-15-HE1", {"F_HE_CARRY_DONE", nullptr}},
        {"SC-15-HE2", {"F_HE_AMENDMENT_DONE", nullptr}},
        {"SC-18-Q1", {nullptr, nullptr}},
        {"SC-21-HE1", {"F_HE_HOME_RESISTS_MINA_DONE", "F_HE_HOME_RESISTS_LATTICE_DONE"}},
    };
    StoryRunner r(&s.db);
    for (const Pick pick : {Pick::Right, Pick::MissThenRight, Pick::AlwaysMiss}) {
        for (const Done& d : kDone) {
            CAPTURE(d.scene);
            CAPTURE(static_cast<int>(pick));
            r.resetFlags();
            const SceneRun run = playScene(r, d.scene, pick);
            CHECK(run.left); // no loop: three misses get the answer supplied and the exchange moves on
            CHECK(run.choices >= 2);
            if (pick == Pick::Right) CHECK(run.misses == 0);
            else if (std::string_view(d.scene) != "SC-10-HE1") CHECK(run.misses > 0);
            for (const char* f : d.flags)
                if (f) CHECK(r.flag(f).truthy());
        }
    }
}

TEST_CASE("SC-13-HE1 counts first-try triage calls in F_HE_LANTERN_GUARD_SCORE") {
    FTCORE_REQUIRE_REAL_STORY(s);
    StoryRunner r(&s.db);
    r.resetFlags();
    playScene(r, "SC-13-HE1", Pick::Right);
    CHECK(r.flag("F_HE_LANTERN_GUARD_SCORE") == FlagValue::integer(3));
    r.resetFlags();
    CHECK(r.setFlag("F_HE_LANTERN_GUARD_SCORE", FlagValue::integer(2))); // a replay starts from zero
    const SceneRun miss = playScene(r, "SC-13-HE1", Pick::AlwaysMiss);
    CHECK(miss.misses == 9); // three ladders, three misses each: the answer is supplied every time
    CHECK(r.flag("F_HE_LANTERN_GUARD_SCORE") == FlagValue::integer(0));
    CHECK(r.flag("F_HE_LANTERN_GUARD_DONE").truthy());
    r.resetFlags();
    playScene(r, "SC-13-HE1", Pick::MissThenRight); // miss, then right on the retry: no smudge for a retry
    CHECK(r.flag("F_HE_LANTERN_GUARD_SCORE") == FlagValue::integer(0));
}

TEST_CASE("SC-18-Q1: pinned clues pre-fill the hands column") {
    FTCORE_REQUIRE_REAL_STORY(s);
    StoryRunner r(&s.db);
    r.resetFlags();
    const SceneRun unpinned = playScene(r, "SC-18-Q1", Pick::AlwaysMiss);
    r.resetFlags();
    CHECK(r.setFlag("F_CH_DIAGRAM_PINS", FlagValue::integer(2)));
    const SceneRun pinned = playScene(r, "SC-18-Q1", Pick::AlwaysMiss);
    CHECK(unpinned.left);
    CHECK(pinned.left);
    CHECK(unpinned.misses == 6); // laugh + taps ladders
    CHECK(pinned.misses == 3);   // laugh only: the taps are a scripted pick
    CHECK(pinned.choices == unpinned.choices - 2); // three asks become one scripted pick
}

TEST_CASE("SC-10-HE1 sets one F_CH_RECUT_* flag per choice") {
    FTCORE_REQUIRE_REAL_STORY(s);
    StoryRunner r(&s.db);
    r.resetFlags();
    const SceneRun run = playScene(r, "SC-10-HE1", Pick::Right);
    CHECK(run.choices == 4);
    CHECK(s.db.flagString(r.flag("F_CH_RECUT_START")) == "FROM_THE_FREEZE");
    CHECK(r.flag("F_CH_RECUT_SPEECH") == FlagValue::boolean(true));
    CHECK(s.db.flagString(r.flag("F_CH_RECUT_HOLD")) == "SHORT");
    CHECK(s.db.flagString(r.flag("F_CH_RECUT_SOUNDBED")) == "COLD_HUM");
    CHECK(r.flag("F_HE_RECUT_DONE").truthy());
}
