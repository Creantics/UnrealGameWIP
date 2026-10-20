// The dialogue backlog: what it records, in what order, with no copies of the text and no
// allocation, and how its ring wraps. Invented text only.

#include "ftcore/Backlog.h"
#include "ftcore/Dialogue.h"
#include "ftcore/StoryDatabase.h"
#include "ftcore/StoryRunner.h"

#include "AllocCounter.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <string>

using namespace ftcore;

namespace {

// SC-A: 0 line, 1 direction, 2 hold, 3 choice (two options), 4 line, 5 unhandled gameplay, 6 end.
void buildScript(StoryBuilder& b) {
    b.setStart("SC-A");
    b.beginChapter("01", "One");
    b.beginScene("SC-A", "A");
    b.line("L-1", "SPK-X", "Alpha one.");        // 0
    b.direction("Beta two.");                    // 1
    b.hold(1.0f);                                // 2
    b.choice("Gamma three?");                    // 3
    b.option("Left", 4);
    b.option("Right", 4);
    b.line("L-2", "SPK-Y", "Delta four.");       // 4
    b.gameplay("ODD", "[no screen]");            // 5
    b.end();                                     // 6
    b.beginScene("SC-B", "B");
    b.line("L-3", "SPK-X", "Epsilon five.");
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

TEST_CASE("the backlog records lines, directions, prompts and choices once each, in order") {
    Script s;
    StoryRunner r(&s.db);
    DialoguePresenter p(&r);
    p.setRevealRate(0.f);
    DialogueBacklog log;
    REQUIRE(r.start());

    auto step = [&]() {
        p.update(1.f / 60.f);
        const bool noted = log.noteShown(r, p);
        CHECK_FALSE(log.noteShown(r, p)); // the same beat is never noted twice
        return noted;
    };

    CHECK(step()); // line
    REQUIRE(applyDialogueCommand(r, p.autoplay()));
    CHECK(step()); // direction
    REQUIRE(applyDialogueCommand(r, p.autoplay()));
    REQUIRE(r.state() == StoryRunner::State::Hold);
    CHECK_FALSE(step()); // a hold shows nothing
    REQUIRE(applyDialogueCommand(r, p.autoplay()));
    REQUIRE(r.state() == StoryRunner::State::Choice);
    CHECK(step()); // the prompt
    CHECK(log.noteChoice(r, 1));
    CHECK_FALSE(log.noteChoice(r, 2)); // no such option
    REQUIRE(r.choose(1));
    CHECK(step()); // the line after it
    REQUIRE(applyDialogueCommand(r, p.autoplay()));
    REQUIRE(r.state() == StoryRunner::State::Gameplay);
    CHECK_FALSE(step()); // a gameplay beat is not logged
    CHECK_FALSE(log.noteChoice(r, 0)); // not on a choice

    REQUIRE(log.size() == 5);
    using K = BacklogEntry::Kind;
    CHECK(log.at(0).kind == K::Line);
    CHECK(DialogueBacklog::speaker(log.at(0), s.db) == "SPK-X");
    CHECK(DialogueBacklog::text(log.at(0), s.db) == "Alpha one.");
    CHECK(log.at(1).kind == K::Direction);
    CHECK(DialogueBacklog::speaker(log.at(1), s.db).empty());
    CHECK(DialogueBacklog::text(log.at(1), s.db) == "Beta two.");
    CHECK(log.at(2).kind == K::Prompt);
    CHECK(DialogueBacklog::text(log.at(2), s.db) == "Gamma three?");
    CHECK(log.at(3).kind == K::Choice);
    CHECK(log.at(3).option == 1);
    CHECK(DialogueBacklog::text(log.at(3), s.db) == "Right");
    CHECK(log.at(4).kind == K::Line);
    CHECK(DialogueBacklog::speaker(log.at(4), s.db) == "SPK-Y");
    CHECK(DialogueBacklog::text(log.at(4), s.db) == "Delta four.");
}

TEST_CASE("the backlog waits for the text to be fully out") {
    Script s;
    StoryRunner r(&s.db);
    DialoguePresenter p(&r);
    p.setRevealRate(10.f); // 10 characters: one second
    DialogueBacklog log;
    REQUIRE(r.start());
    p.update(0.3f);
    CHECK_FALSE(log.noteShown(r, p));
    p.update(0.3f);
    CHECK_FALSE(log.noteShown(r, p));
    p.update(1.0f);
    REQUIRE(p.revealed());
    CHECK(log.noteShown(r, p));
    CHECK(log.size() == 1);

    // A press that finishes the text counts as well.
    REQUIRE(applyDialogueCommand(r, p.press())); // the line is out: advance
    p.sync();
    CHECK_FALSE(p.revealed());
    CHECK_FALSE(log.noteShown(r, p));
    p.revealAll();
    CHECK(log.noteShown(r, p));
    CHECK(log.size() == 2);
}

TEST_CASE("the backlog ring keeps the newest entries and clear empties it") {
    DialogueBacklog log;
    const std::uint32_t total = DialogueBacklog::kCapacity + 50;
    std::uint32_t lastSerial = log.serial();
    for (std::uint32_t i = 0; i < total; ++i) {
        BacklogEntry e;
        e.scene = 0;
        e.beat = i;
        log.push(e);
        CHECK(log.serial() != lastSerial);
        lastSerial = log.serial();
    }
    REQUIRE(log.size() == DialogueBacklog::kCapacity);
    CHECK(log.at(0).beat == 50); // the 50 oldest are gone
    CHECK(log.at(DialogueBacklog::kCapacity - 1).beat == total - 1);
    for (std::uint32_t i = 1; i < log.size(); ++i) CHECK(log.at(i).beat == log.at(i - 1).beat + 1);

    log.clear();
    CHECK(log.size() == 0);
    CHECK(log.serial() != lastSerial);
    BacklogEntry e;
    e.beat = 7;
    log.push(e);
    REQUIRE(log.size() == 1);
    CHECK(log.at(0).beat == 7);
}

TEST_CASE("a cleared backlog notes the current beat again") {
    Script s;
    StoryRunner r(&s.db);
    DialoguePresenter p(&r);
    p.setRevealRate(0.f);
    DialogueBacklog log;
    REQUIRE(r.start());
    p.update(0.f);
    CHECK(log.noteShown(r, p));
    CHECK_FALSE(log.noteShown(r, p));
    log.clear(); // a load or a new game
    CHECK(log.noteShown(r, p));
    CHECK(log.size() == 1);
}

TEST_CASE("entries that no longer fit the database read as empty") {
    Script s;
    BacklogEntry e;
    e.scene = 99;
    CHECK(DialogueBacklog::text(e, s.db).empty());
    e.scene = 0;
    e.beat = 99;
    CHECK(DialogueBacklog::text(e, s.db).empty());
    e.beat = 3; // the choice
    e.kind = BacklogEntry::Kind::Choice;
    e.option = 9;
    CHECK(DialogueBacklog::text(e, s.db).empty());
    CHECK(DialogueBacklog::speaker(e, s.db).empty());
}

TEST_CASE("noting and reading the backlog allocates nothing") {
    Script s;
    StoryRunner r(&s.db);
    DialoguePresenter p(&r);
    p.setRevealRate(0.f);
    DialogueBacklog log;
    REQUIRE(r.start());

    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    std::size_t chars = 0;
    for (int round = 0; round < 300; ++round) {
        r.start();
        for (int i = 0; i < 12 && !r.finished(); ++i) {
            p.update(1.f / 60.f);
            log.noteShown(r, p);
            if (r.state() == StoryRunner::State::Choice) log.noteChoice(r, 0);
            applyDialogueCommand(r, p.autoplay());
        }
    }
    for (std::uint32_t i = 0; i < log.size(); ++i) chars += DialogueBacklog::text(log.at(i), s.db).size();
    const auto after = ftcore_test::allocStats().count;
    CHECK(log.size() == DialogueBacklog::kCapacity);
    CHECK(chars > 0);
    if (counting) CHECK(after == before);
}
