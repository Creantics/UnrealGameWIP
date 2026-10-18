#include "ftcore/StoryDatabase.h"
#include "ftcore/StoryRunner.h"
#include "ftcore/StorySave.h"

#include "AllocCounter.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

using namespace ftcore;

namespace {

// Test fixture (the only place sample ids appear): two chapters, four scenes, declared the way an
// importer would feed them.
void buildFixture(StoryBuilder& b) {
    b.setTitle("Fixture");
    b.setStart("SC-A");
    b.addFlag("F_BOOL", FlagValue::boolean(false));
    b.addFlag("F_COUNT", FlagValue::integer(3));
    b.addFlag("F_ENUM", b.stringValue("UNSET"));
    b.addCue("MUS-X", "music");

    b.beginChapter("01", "Chapter 1 · One");
    b.beginScene("SC-A", "Alpha", {"Waking", "Hall", "dawn", "CMB → ENG", "One short wave."});
    b.direction("Opening *text*.", "story.SC-A.d001");                         // 0
    b.hiddenDirection("[OPTIONAL: note]", "OPTIONAL");                          // 1
    b.cue("MUS-X", CueAction::Play);                                            // 2
    b.line("VO-A-001", "SPK1", "Hello there.", "quietly", "story.SC-A.VO-A-001"); // 3
    b.hold(1.0f, "Tell");                                                       // 4
    b.choice("Well?", false, "story.SC-A.c001");                                // 5
    b.option("Ask again", 5, "story.SC-A.c001.o1");
    b.option("Go", kNone, "story.SC-A.c001.o2");
    b.effect(FlagOp::Set, "F_ENUM", b.stringValue("SAT"));
    b.greyedOption("Greyed", "story.SC-A.c001.o3");
    b.flag(FlagOp::Require, "F_BOOL", FlagValue::boolean(true));                // 6
    b.line("VO-A-002", "SPK1", "Skipped.");                                     // 7
    b.flag(FlagOp::End);                                                        // 8
    b.choice("Nobody");                                                         // 9
    b.greyedOption("x");
    b.gameplay("CMB", "[GAMEPLAY: fight]", "story.SC-A.g001");                  // 10
    b.param("enemy", "wolf");
    b.param("count", "2");
    b.unknown();                                                                // 11
    b.end("SC-D");                                                              // 12

    b.beginScene("SC-B", "Beta", {"Waking", "Yard", "noon", "PLAY", {}});
    b.line("VO-B-001", "SPK2", "B.");
    b.gameplay("PLAY", "[walk]");

    b.beginChapter("02", "Chapter 2 · Two");
    b.beginScene("SC-C", "Gamma", {"Dream", "Shore", "dusk", "ENG", {}});
    b.flag(FlagOp::Set, "F_BOOL", FlagValue::boolean(true));
    b.flag(FlagOp::Require, "F_BOOL", FlagValue::boolean(true));
    b.line("VO-C-001", "SPK1", "Allowed.");
    b.flag(FlagOp::End);
    b.flag(FlagOp::Clear, "F_COUNT");
    b.end();

    b.beginScene("SC-D", "Delta", {"Dream", "Shore", "night", "MINI", {}});
    b.hold(0.f);
    b.line("VO-D-001", "SPK1", "D.");
}

struct Fixture {
    StoryDatabase db;
    Fixture() {
        StoryBuilder b;
        buildFixture(b);
        std::string error;
        const bool ok = b.finish(db, &error);
        INFO(error);
        REQUIRE(ok);
        CHECK(b.warnings().empty());
    }
};

struct CueLog {
    int count = 0;
    std::string_view last;
    static void onCue(void* user, const CueEvent& e) {
        auto* self = static_cast<CueLog*>(user);
        ++self->count;
        self->last = e.id;
    }
};

} // namespace

TEST_CASE("story database resolves scenes, text and indices") {
    Fixture f;
    const StoryDatabase& db = f.db;
    CHECK(db.title() == "Fixture");
    REQUIRE(db.sceneCount() == 4);
    CHECK(db.view(db.scene(db.startScene()).id) == "SC-A");
    CHECK(db.findScene("SC-C") == 2);
    CHECK(db.findScene("SC-Z") == kNone);
    const Scene& a = db.scene(0);
    CHECK(db.view(a.title) == "Alpha");
    CHECK(db.view(a.mode) == "CMB → ENG");
    CHECK(db.view(a.gameplay) == "One short wave.");
    CHECK(db.view(a.world) == "Waking");
    CHECK(db.view(a.chapter) == "01");
    CHECK(db.view(db.scene(2).chapter) == "02");
    CHECK(db.scene(1).orderNext == 2);
    CHECK(db.scene(3).orderNext == kNone);
    CHECK(db.view(db.chapter(1).title) == "Chapter 2 · Two");
    CHECK(db.chapter(1).firstScene == 2);
    CHECK(db.chapter(1).sceneCount == 2);
    CHECK(db.view(db.beat(a, 0).text) == "Opening text."); // emphasis stripped
    CHECK(db.view(db.beat(a, 0).loc) == "story.SC-A.d001");
    CHECK(db.beat(a, 1).hidden);
    CHECK(db.view(db.beat(a, 1).note) == "OPTIONAL");
    CHECK(db.beat(a, 11).type == BeatType::Unknown);
    CHECK(db.beat(a, 12).nextScene == db.findScene("SC-D"));
    CHECK(db.flagCount() == 3);
    CHECK(db.flag(db.findFlag("F_COUNT")).initial == FlagValue::integer(3));
    CHECK(db.flagString(db.flag(db.findFlag("F_ENUM")).initial) == "UNSET");
    CHECK(db.findFlag("F_NOPE") == kNone);
    CHECK(db.cueCount() == 1);
    CHECK(db.view(db.cue(0).kind) == "music");
    CHECK(db.param(db.beat(a, 10), "enemy") == "wolf");
    CHECK(db.param(db.beat(a, 10), "count") == "2");
    CHECK(db.param(db.beat(a, 10), "missing").empty());
    CHECK(db.longestText() == std::string_view("[GAMEPLAY: fight]").size());
}

TEST_CASE("story runner walks every beat type") {
    Fixture f;
    StoryRunner r(&f.db);
    CueLog cues;
    r.setCueHandler(&CueLog::onCue, &cues);
    REQUIRE(r.start());
    const std::uint32_t epoch0 = r.sceneEpoch();

    // direction (the hidden direction after it is skipped; the cue fires on the way)
    CHECK(r.state() == StoryRunner::State::Direction);
    CHECK(r.text() == "Opening text.");
    CHECK(r.textKey() == "story.SC-A.d001");
    CHECK(r.speaker().empty());
    CHECK(r.advance());
    CHECK(cues.count == 1);
    CHECK(cues.last == "MUS-X");

    // line
    CHECK(r.state() == StoryRunner::State::Line);
    CHECK(r.speaker() == "SPK1");
    CHECK(r.direction() == "quietly");
    CHECK(r.lineId() == "VO-A-001");
    CHECK(r.text() == "Hello there.");
    CHECK_FALSE(r.choose(0));
    CHECK_FALSE(r.resume());
    CHECK(r.advance());

    // hold: nothing advances it; time does
    CHECK(r.state() == StoryRunner::State::Hold);
    CHECK_FALSE(r.advance());
    CHECK(r.text().empty());
    r.update(0.6f);
    CHECK(r.state() == StoryRunner::State::Hold);
    CHECK(r.holdRemaining() == doctest::Approx(0.4f));
    r.update(0.6f);

    // choice: self-loop re-shows, greyed option refused, effect applied
    REQUIRE(r.state() == StoryRunner::State::Choice);
    CHECK(r.text() == "Well?");
    CHECK(r.optionCount() == 3);
    CHECK(r.optionText(1) == "Go");
    CHECK(r.optionTextKey(1) == "story.SC-A.c001.o2");
    CHECK(r.optionReturns(0));
    CHECK_FALSE(r.optionSelectable(2));
    CHECK(r.defaultOption() == 1);
    const std::uint32_t before = r.beatEpoch();
    CHECK(r.choose(0));
    CHECK(r.state() == StoryRunner::State::Choice);
    CHECK(r.beatEpoch() != before);
    CHECK_FALSE(r.choose(2));
    CHECK_FALSE(r.choose(7));
    CHECK(r.choose(1));
    CHECK(f.db.flagString(r.flag("F_ENUM")) == "SAT");

    // failed require skips its block; the unselectable choice passes; gameplay requested
    REQUIRE(r.state() == StoryRunner::State::Gameplay);
    CHECK(r.waitingForGameplay());
    CHECK(r.gameplayKind() == GameplayKind::Combat);
    CHECK(r.gameplayKindName() == "CMB");
    CHECK(r.param("enemy") == "wolf");
    REQUIRE(r.paramCount() == 2);
    CHECK(r.paramKey(1) == "count");
    CHECK(r.paramValue(1) == "2");
    CHECK(r.paramKey(2).empty());
    CHECK(r.sceneEpoch() == epoch0);
    CHECK(r.resume());

    // unknown beat passes; end jumps to SC-D (whose 0 s hold passes through)
    CHECK(r.sceneEpoch() == epoch0 + 1);
    CHECK(r.position().scene == "SC-D");
    CHECK(r.state() == StoryRunner::State::Line);
    CHECK(r.text() == "D.");
    CHECK(r.advance());
    // SC-D is last in chapter order
    CHECK(r.finished());
    CHECK_FALSE(r.stalled());
    CHECK_FALSE(r.advance());
}

TEST_CASE("story runner chains scenes in chapter order and applies flag ops") {
    Fixture f;
    StoryRunner r(&f.db);
    REQUIRE(r.start("SC-B"));
    CHECK(r.text() == "B.");
    r.advance();
    CHECK(r.gameplayKind() == GameplayKind::Play);
    CHECK(r.paramCount() == 0);
    r.resume(); // falls off SC-B -> SC-C (next chapter)
    CHECK(r.position().scene == "SC-C");
    CHECK(r.text() == "Allowed."); // require satisfied by the set before it
    CHECK(r.flag("F_BOOL").truthy());
    r.advance();
    CHECK(r.flag("F_COUNT") == FlagValue::integer(3)); // clear on a counter: back to its default
    CHECK(r.position().scene == "SC-D");
    CHECK_FALSE(r.start("SC-NOPE"));
    CHECK(r.finished());
}

TEST_CASE("story runner saves and restores its position and flags") {
    Fixture f;
    StoryRunner r(&f.db);
    r.start();
    r.advance();
    r.advance();
    r.skipHold();
    REQUIRE(r.state() == StoryRunner::State::Choice);
    r.choose(1);
    REQUIRE(r.state() == StoryRunner::State::Gameplay);

    // what a save keeps: scene id, beat index, each flag as text
    const std::string scene(r.position().scene);
    const std::uint32_t beat = r.position().beat;
    std::vector<std::pair<std::string, std::string>> flags;
    for (std::uint32_t i = 0; i < r.flagCount(); ++i)
        flags.emplace_back(std::string(f.db.view(f.db.flag(i).id)), f.db.formatFlagValue(r.flag(i)));
    CHECK(flags[2].second == "SAT");
    CHECK(flags[1].second == "3");
    CHECK(flags[0].second == "false");

    StoryRunner q(&f.db);
    for (auto& [id, text] : flags) {
        FlagValue v;
        REQUIRE(f.db.parseFlagValue(text, v));
        CHECK(q.setFlag(id, v));
    }
    FlagValue bogus;
    CHECK_FALSE(f.db.parseFlagValue("NEVER_IN_SCRIPT", bogus));
    CHECK_FALSE(f.db.parseFlagValue("12x", bogus));
    CHECK(f.db.parseFlagValue("-4", bogus));
    CHECK(bogus == FlagValue::integer(-4));
    CHECK_FALSE(q.setFlag("F_NOPE", FlagValue::boolean(true)));
    REQUIRE(q.restore({scene, beat}));
    CHECK(q.state() == StoryRunner::State::Gameplay);
    CHECK(q.param("enemy") == "wolf");
    CHECK(f.db.flagString(q.flag("F_ENUM")) == "SAT");
    CHECK_FALSE(q.restore({"SC-NOPE", 0}));

    // a beat past the scene's end continues with the next scene
    REQUIRE(q.restore({"SC-B", 99}));
    CHECK(q.position().scene == "SC-C");
}

TEST_CASE("story runner moves without allocating") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    Fixture f;
    StoryRunner r(&f.db);
    CueLog cues;
    r.setCueHandler(&CueLog::onCue, &cues);
    const auto before = ftcore_test::allocStats().count;
    int moves = 0;
    for (int pass = 0; pass < 3; ++pass) {
        r.start();
        while (!r.finished() && moves < 10000) {
            ++moves;
            switch (r.state()) {
            case StoryRunner::State::Line:
            case StoryRunner::State::Direction: r.advance(); break;
            case StoryRunner::State::Choice: r.choose(r.defaultOption()); break;
            case StoryRunner::State::Hold: r.update(0.25f); break;
            case StoryRunner::State::Gameplay: r.resume(); break;
            default: r.stop(); break;
            }
            (void)r.text();
            (void)r.position();
            (void)r.flag("F_ENUM");
        }
        r.restore({"SC-B", 0});
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(after == before);
    CHECK(moves > 10);
    CHECK(r.state() != StoryRunner::State::Idle);
}

TEST_CASE("a miss option corrects and moves on; the autoplay option is the right one") {
    StoryBuilder b;
    b.beginChapter("01", "One");
    b.beginScene("SC-X", "X");
    b.addFlag("F_DONE", FlagValue::boolean(false));
    b.choice("Who?");               // 0
    b.missOption("Wrong");
    b.option("Right", 3);
    b.line("PH-X-001", "ZEE", "Try again.");   // 1
    b.choice("Who?");               // 2
    b.missOption("Wrong");
    b.option("Right", 3);
    b.flag(FlagOp::Set, "F_DONE"); // 3
    b.line("PH-X-002", "ZEE", "Yes."); // 4
    b.end();                        // 5
    StoryDatabase db;
    std::string error;
    REQUIRE_MESSAGE(b.finish(db, &error), error);
    const Scene& sc = db.scene(0);
    CHECK(db.option(db.beat(sc, 0), 0).miss);
    CHECK_FALSE(db.option(db.beat(sc, 0), 1).miss);

    StoryRunner r(&db);
    REQUIRE(r.start());
    REQUIRE(r.state() == StoryRunner::State::Choice);
    CHECK(r.optionReturns(0)); // a miss does not move the story on
    CHECK_FALSE(r.optionReturns(1));
    CHECK(r.defaultOption() == 1);
    CHECK(r.choose(0)); // wrong: the correction, then the question again
    CHECK(r.state() == StoryRunner::State::Line);
    CHECK(r.text() == "Try again.");
    CHECK(r.advance());
    REQUIRE(r.state() == StoryRunner::State::Choice);
    CHECK(r.beatIndex() == 2);
    CHECK(r.choose(r.defaultOption()));
    CHECK(r.flag("F_DONE").truthy());
    CHECK(r.text() == "Yes.");

    // A right first pick skips the correction.
    REQUIRE(r.start());
    CHECK(r.choose(1));
    CHECK(r.text() == "Yes.");
}

TEST_CASE("a choice with hundreds of options reaches each one; greyed ones are refused") {
    constexpr std::uint32_t kWide = 300;
    StoryBuilder b;
    b.beginChapter("01", "One");
    b.beginScene("SC-W", "Wide");
    b.choice("Pick one");                                   // 0
    for (std::uint32_t i = 0; i < kWide; ++i) b.option("o" + std::to_string(i), 2 + i);
    b.choice("Only the last");                              // 1
    for (std::uint32_t i = 0; i + 1 < kWide; ++i) b.greyedOption("g" + std::to_string(i));
    b.option("last", 2);
    std::vector<std::string> texts;
    for (std::uint32_t i = 0; i < kWide; ++i) texts.push_back("line " + std::to_string(i));
    for (std::uint32_t i = 0; i < kWide; ++i) b.line("PH-W-" + std::to_string(i), "ZEE", texts[i]); // 2 + i
    b.end();
    StoryDatabase db;
    std::string error;
    REQUIRE_MESSAGE(b.finish(db, &error), error);

    StoryRunner r(&db);
    REQUIRE(r.start());
    REQUIRE(r.optionCount() == kWide);
    CHECK(r.optionText(kWide - 1) == "o299");
    CHECK(r.defaultOption() == 0);
    CHECK_FALSE(r.choose(kWide));
    int wrong = 0;
    for (std::uint32_t i = 0; i < kWide; ++i) {
        REQUIRE(r.restore({"SC-W", 0}));
        if (!r.choose(i) || r.text() != texts[i]) ++wrong;
    }
    CHECK(wrong == 0);

    REQUIRE(r.restore({"SC-W", 1}));
    REQUIRE(r.optionCount() == kWide);
    CHECK(r.defaultOption() == kWide - 1);
    CHECK_FALSE(r.optionSelectable(0));
    CHECK_FALSE(r.choose(0));
    CHECK(r.state() == StoryRunner::State::Choice);
    CHECK(r.choose(kWide - 1));
    CHECK(r.text() == "line 0");
}

TEST_CASE("story builder warns about recoverable mistakes") {
    StoryBuilder b;
    b.keepEmphasis(true);
    b.beginChapter("01", "One");
    b.beginScene("SC-1", "One");
    b.cue("SFX-UNDECLARED");                                    // 0
    b.flag(FlagOp::Set, "F_UNDECLARED", FlagValue::integer(2)); // 1
    b.choice("Pick");                                           // 2
    b.option("Far", 40); // past the scene's end: ends the scene
    b.choice("Empty");   // 3, no options: passes through
    b.end("SC-NOWHERE"); // 4, unknown target: chapter order
    b.beginScene("SC-2", "Two");
    b.line("L", "S", "*kept*");
    StoryDatabase db;
    REQUIRE(b.finish(db));
    CHECK(b.warnings().size() == 3);
    const Scene& one = db.scene(0);
    CHECK(db.option(db.beat(one, 2), 0).next == one.beatCount);
    CHECK(db.beat(one, 4).nextScene == kNone);
    CHECK(db.findFlag("F_UNDECLARED") != kNone);
    CHECK(db.cueCount() == 1);
    CHECK(db.view(db.cue(0).kind).empty());

    StoryRunner r(&db);
    REQUIRE(r.start());
    REQUIRE(r.state() == StoryRunner::State::Choice);
    CHECK(r.choose(0));
    CHECK(r.position().scene == "SC-2");
    CHECK(r.text() == "*kept*");
    CHECK(r.flag("F_UNDECLARED") == FlagValue::integer(2));

    b.reset();
    CHECK(b.warnings().empty());
}

TEST_CASE("story builder rejects broken scripts") {
    auto failsWith = [](void (*build)(StoryBuilder&), std::string_view needle) {
        StoryBuilder b;
        build(b);
        StoryDatabase db;
        std::string error;
        const bool ok = b.finish(db, &error);
        INFO(error);
        CHECK_FALSE(ok);
        CHECK(error.find(needle) != std::string::npos);
        CHECK_FALSE(db.loaded());
    };
    failsWith([](StoryBuilder&) {}, "no scenes");
    failsWith([](StoryBuilder& b) { b.line("L", "S", "orphan"); }, "outside a scene");
    failsWith([](StoryBuilder& b) { b.beginScene("SC-1", "One"); }, "outside a chapter");
    failsWith(
        [](StoryBuilder& b) {
            b.beginChapter("01", "One");
            b.beginScene("SC-1", "One");
            b.beginScene("SC-1", "Again");
        },
        "listed twice");
    failsWith(
        [](StoryBuilder& b) {
            b.setStart("SC-NOPE");
            b.beginChapter("01", "One");
            b.beginScene("SC-1", "One");
        },
        "start scene");
    failsWith(
        [](StoryBuilder& b) {
            b.beginChapter("01", "One");
            b.beginScene("SC-1", "One");
            b.line("L", "S", "x");
            b.option("stray");
        },
        "does not follow a choice");
    failsWith(
        [](StoryBuilder& b) {
            b.beginChapter("01", "One");
            b.beginScene("SC-1", "One");
            b.choice("Pick");
            b.effect(FlagOp::Set, "F");
        },
        "does not follow an option");
    failsWith(
        [](StoryBuilder& b) {
            b.beginChapter("01", "One");
            b.beginScene("SC-1", "One");
            b.line("L", "S", "x");
            b.param("k", "v");
        },
        "gameplay beat");

    // A builder is reusable after a failure.
    StoryBuilder b;
    b.line("L", "S", "orphan");
    StoryDatabase db;
    CHECK_FALSE(b.finish(db));
    b.beginChapter("01", "One");
    b.beginScene("SC-1", "One");
    b.line("L", "S", "x");
    CHECK(b.finish(db));
    CHECK(db.loaded());
}

TEST_CASE("story runner gives up on a loop of empty scenes") {
    StoryBuilder b;
    b.beginChapter("01", "One");
    b.beginScene("SC-1", "One");
    b.end("SC-2");
    b.beginScene("SC-2", "Two");
    b.end("SC-1");
    StoryDatabase db;
    REQUIRE(b.finish(db));
    StoryRunner r(&db);
    CHECK(r.start());
    CHECK(r.finished());
    CHECK(r.stalled());
}

TEST_CASE("story save keeps the cursor and the changed flags") {
    Fixture f;
    StoryRunner r(&f.db);
    StorySave s;
    CHECK_FALSE(captureStory(r, s)); // idle: nothing to save
    r.start();
    r.advance();
    r.advance();
    r.skipHold();
    REQUIRE(r.state() == StoryRunner::State::Choice);
    r.choose(1);
    REQUIRE(r.state() == StoryRunner::State::Gameplay);
    REQUIRE(r.setFlag("F_COUNT", FlagValue::integer(-7)));

    REQUIRE(captureStory(r, s));
    CHECK(s.scene == "SC-A");
    CHECK(s.beat == 10);
    REQUIRE(s.flags.size() == 2); // F_BOOL is still at its default
    CHECK(s.flags[0].id == "F_COUNT");
    CHECK(s.flags[0].kind == FlagValue::Kind::Int);
    CHECK(s.flags[0].number == -7);
    CHECK(s.flags[1].id == "F_ENUM");
    CHECK(s.flags[1].kind == FlagValue::Kind::String);
    CHECK(s.flags[1].text == "SAT");

    StoryRunner q(&f.db);
    q.start("SC-C"); // sets F_BOOL; applying the save resets it
    StoryLoadReport report;
    REQUIRE(applyStory(s, q, &report));
    CHECK(report.skippedFlags.empty());
    CHECK(q.state() == StoryRunner::State::Gameplay);
    CHECK(q.param("enemy") == "wolf");
    CHECK(q.flag("F_COUNT") == FlagValue::integer(-7));
    CHECK(f.db.flagString(q.flag("F_ENUM")) == "SAT");
    CHECK_FALSE(q.flag("F_BOOL").truthy());

    // A save from an older script: a flag and a string value it no longer has.
    StorySave old = s;
    old.flags.push_back({"F_GONE", FlagValue::Kind::Bool, 1, {}});
    old.flags.push_back({"F_ENUM", FlagValue::Kind::String, 0, "RETIRED"});
    REQUIRE(applyStory(old, q, &report));
    REQUIRE(report.skippedFlags.size() == 2);
    CHECK(report.skippedFlags[0] == "F_GONE");
    CHECK(f.db.flagString(q.flag("F_ENUM")) == "SAT"); // the earlier entry still applied

    old.scene = "SC-NOPE";
    CHECK_FALSE(applyStory(old, q));
    CHECK(q.state() == StoryRunner::State::Idle);
}
