#include "ftcore/Dialogue.h"
#include "ftcore/StoryDatabase.h"
#include "ftcore/StoryRunner.h"

#include "AllocCounter.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <string>

using namespace ftcore;

namespace {

// Every beat type the box handles: a direction, a line with multibyte text, a hold, a choice with
// a miss and a greyed option, an unhandled gameplay beat, and a second scene.
void buildScript(StoryBuilder& b) {
    b.setStart("SC-P");
    b.beginChapter("01", "One");
    b.beginScene("SC-P", "P");
    b.direction("Open.");                            // 0
    b.line("PH-P-001", "SPK", "Caf\xC3\xA9 \xE2\x9C\x93!"); // 1: "Café ✓!" (7 characters, 10 bytes)
    b.hold(2.0f);                                    // 2
    b.choice("Pick?");                               // 3
    b.missOption("Wrong");
    b.greyedOption("Greyed");
    b.option("Right", 5);
    b.line("PH-P-002", "SPK", "Again.");             // 4
    b.gameplay("ODD", "[a beat nothing handles]");   // 5
    b.end();                                         // 6
    b.beginScene("SC-Q", "Q");
    b.line("PH-Q-001", "SPK", "Last.");
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

TEST_CASE("utf8 helpers count codepoints, not bytes") {
    CHECK(utf8Length("") == 0);
    CHECK(utf8Length("abc") == 3);
    CHECK(utf8Length("Caf\xC3\xA9") == 4);
    CHECK(utf8Length("\xF0\x9F\x8E\xB5x") == 2); // a 4-byte character, then x
    CHECK(utf8PrefixBytes("Caf\xC3\xA9!", 3) == 3);
    CHECK(utf8PrefixBytes("Caf\xC3\xA9!", 4) == 5); // the 2-byte character comes whole
    CHECK(utf8PrefixBytes("Caf\xC3\xA9!", 99) == 6);
    CHECK(utf8PrefixBytes("\xF0\x9F\x8E\xB5x", 1) == 4);
    CHECK(utf8PrefixBytes("abc", 0) == 0);
}

TEST_CASE("the typewriter reveals whole characters at its rate") {
    Script s;
    StoryRunner r(&s.db);
    REQUIRE(r.start());
    DialoguePresenter p(&r);
    p.setRevealRate(10.f);
    REQUIRE(r.advance()); // onto the multibyte line
    CHECK(p.sync());
    CHECK(p.state() == StoryRunner::State::Line);
    CHECK(p.totalChars() == 7);
    CHECK(p.revealedChars() == 0);
    CHECK(p.revealedText().empty());
    p.update(0.25f); // 2.5 characters: 2 shown
    CHECK(p.revealedChars() == 2);
    CHECK(p.revealedText() == "Ca");
    p.update(0.2f); // 4.5: the é comes whole
    CHECK(p.revealedChars() == 4);
    CHECK(p.revealedText() == "Caf\xC3\xA9");
    p.update(0.1f); // 5.5: the space
    CHECK(p.revealedText() == "Caf\xC3\xA9 ");
    p.update(10.f);
    CHECK(p.revealed());
    CHECK(p.revealedText() == r.text());
    CHECK(p.beatTime() == doctest::Approx(10.55f));

    // A rate of 0 shows each text whole at once.
    p.setRevealRate(0.f);
    REQUIRE(r.start());
    CHECK(p.sync());
    CHECK(p.revealed());
    CHECK(p.revealedText() == "Open.");
}

TEST_CASE("the first press finishes the text, the second moves on") {
    Script s;
    StoryRunner r(&s.db);
    REQUIRE(r.start());
    DialoguePresenter p(&r);
    p.setRevealRate(5.f);
    p.update(0.2f);
    CHECK(p.revealedChars() == 1);
    CHECK(p.press().none());
    CHECK(p.revealed());
    const DialogueCommand next = p.press();
    CHECK(next.kind == DialogueCommand::Kind::Advance);
    REQUIRE(applyDialogueCommand(r, next));
    p.update(0.f);
    CHECK(p.state() == StoryRunner::State::Line);
    CHECK(p.revealedChars() == 0); // a new beat restarts the typewriter

    // A hold shows nothing and ignores presses; the runner moves on by itself.
    p.revealAll();
    REQUIRE(applyDialogueCommand(r, p.press()));
    p.update(0.f);
    CHECK(p.state() == StoryRunner::State::Hold);
    CHECK(p.revealed()); // nothing to type
    CHECK(p.press().none());
    r.update(2.5f);
    p.update(0.f);
    CHECK(p.state() == StoryRunner::State::Choice);
}

TEST_CASE("options show once the prompt is out; greyed ones are refused") {
    Script s;
    StoryRunner r(&s.db);
    REQUIRE(r.start());
    r.advance();
    r.advance();
    r.skipHold();
    REQUIRE(r.state() == StoryRunner::State::Choice);
    DialoguePresenter p(&r);
    p.setRevealRate(1.f);
    CHECK_FALSE(p.optionsVisible());
    CHECK(p.press().none()); // finishes the prompt...
    CHECK(p.optionsVisible());
    CHECK(p.press().none()); // ...and a press never picks an option

    p.setRunner(&r); // the prompt types out again
    CHECK_FALSE(p.optionsVisible());
    CHECK(p.choose(2).none()); // a pick while typing only finishes the prompt
    CHECK(p.optionsVisible());
    CHECK(p.choose(1).none()); // greyed
    CHECK(p.choose(9).none()); // no such option
    const DialogueCommand miss = p.choose(0);
    CHECK(miss.kind == DialogueCommand::Kind::Choose);
    CHECK(miss.option == 0);
    REQUIRE(applyDialogueCommand(r, miss));
    CHECK(r.state() == StoryRunner::State::Line); // the correction

    // An unhandled gameplay beat waits in the box; a press resumes it.
    REQUIRE(r.restore({"SC-P", 5}));
    p.update(0.f);
    CHECK(p.state() == StoryRunner::State::Gameplay);
    p.revealAll();
    const DialogueCommand go = p.press();
    CHECK(go.kind == DialogueCommand::Kind::Resume);
    REQUIRE(applyDialogueCommand(r, go));
    CHECK(r.sceneIndex() == s.db.findScene("SC-Q"));
}

TEST_CASE("autoplay waits for the text, then plays the script to the end without allocating") {
    Script s;
    StoryRunner r(&s.db);
    DialoguePresenter p(&r);
    p.setRevealRate(30.f);
    REQUIRE(r.start());

    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    int frames = 0, moves = 0, earlyMoves = 0, refused = 0;
    std::uint32_t lastScene = r.sceneEpoch(), scenes = 1;
    while (!r.finished() && frames < 10000) {
        ++frames;
        p.update(1.f / 60.f);
        const DialogueCommand c = p.autoplay();
        if (c.none()) continue;
        if (!p.revealed()) ++earlyMoves;
        if (c.kind == DialogueCommand::Kind::Choose && r.optionReturns(c.option)) ++refused; // never the miss
        if (!applyDialogueCommand(r, c)) ++refused;
        ++moves;
        if (r.sceneEpoch() != lastScene) {
            lastScene = r.sceneEpoch();
            ++scenes;
        }
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(r.finished());
    CHECK_FALSE(r.stalled());
    CHECK(earlyMoves == 0);
    CHECK(refused == 0);
    CHECK(moves == 6); // direction, line, hold, choice (right first time), gameplay, last line
    CHECK(scenes == 2);
    CHECK(frames > 20); // the typing took time
    if (counting) CHECK(after == before);

    // Finished: nothing more to do.
    p.update(0.f);
    CHECK(p.state() == StoryRunner::State::Finished);
    CHECK(p.autoplay().none());
    CHECK(p.press().none());
}

TEST_CASE("autoplay ignores a stale view and a presenter without a runner") {
    Script s;
    StoryRunner r(&s.db);
    DialoguePresenter p(&r);
    p.setRevealRate(0.f);
    REQUIRE(r.start());
    p.update(0.f);
    REQUIRE(applyDialogueCommand(r, p.autoplay()));
    CHECK(p.autoplay().none()); // the runner moved; not synced yet
    p.update(0.f);
    CHECK(p.autoplay().kind == DialogueCommand::Kind::Advance);

    DialoguePresenter empty;
    empty.update(1.f);
    CHECK(empty.state() == StoryRunner::State::Idle);
    CHECK(empty.revealedText().empty());
    CHECK(empty.press().none());
    CHECK(empty.choose(0).none());
    CHECK(empty.autoplay().none());
    CHECK_FALSE(applyDialogueCommand(r, DialogueCommand{}));
}
