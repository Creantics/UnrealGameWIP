// Seen-line tracking, its place in the story save, skip and auto-advance. Invented text only.

#include "ftcore/Dialogue.h"
#include "ftcore/Seen.h"
#include "ftcore/Skip.h"
#include "ftcore/StoryDatabase.h"
#include "ftcore/StoryRunner.h"
#include "ftcore/StorySave.h"

#include "AllocCounter.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <string>

using namespace ftcore;

namespace {

// SC-A: 0-3 lines, 4 choice, 5 line, 6 hold, 7 line, 8 gameplay nothing handles. SC-B: one line.
void buildScript(StoryBuilder& b) {
    b.setStart("SC-A");
    b.beginChapter("01", "One");
    b.beginScene("SC-A", "A");
    b.line("L-0", "SPK", "Zero zero zero.");  // 0
    b.line("L-1", "SPK", "One one one.");     // 1
    b.line("L-2", "SPK", "Two two two.");     // 2
    b.direction("Three three.");              // 3
    b.choice("Four?");                        // 4
    b.option("Yes", 5);
    b.option("No", 5);
    b.line("L-5", "SPK", "Five five.");       // 5
    b.hold(0.5f);                             // 6
    b.line("L-7", "SPK", "Seven seven.");     // 7
    b.gameplay("ODD", "[no screen]");         // 8
    b.end();                                  // 9
    b.beginScene("SC-B", "B");
    b.line("L-B", "SPK", "Bee.");
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

// One frame of an interactive owner: type the text, then let skip decide. True when it moved.
bool frame(StoryRunner& r, DialoguePresenter& p, SkipController& skip, const SeenLines& seen, float dt) {
    p.update(dt);
    return applyDialogueCommand(r, skip.update(dt, p, seen));
}

} // namespace

TEST_CASE("seen lines: mark, read, count, clear and out of range") {
    Script s;
    SeenLines seen;
    CHECK_FALSE(seen.seen(0, 0)); // no database yet
    seen.mark(0, 0);              // ignored
    CHECK(seen.count() == 0);

    seen.setDatabase(&s.db);
    CHECK_FALSE(seen.seen(0, 0));
    seen.mark(0, 0);
    seen.mark(0, 5);
    seen.mark(1, 0);
    seen.mark(0, 5); // twice
    CHECK(seen.seen(0, 0));
    CHECK(seen.seen(0, 5));
    CHECK(seen.seen(1, 0));
    CHECK_FALSE(seen.seen(0, 1));
    CHECK(seen.count() == 3);
    seen.mark(0, 10); // past the scene's last beat
    seen.mark(7, 0);  // no such scene
    CHECK(seen.count() == 3);
    CHECK_FALSE(seen.seen(0, 10));
    CHECK_FALSE(seen.seen(7, 0));
    // Scenes do not share bits.
    CHECK_FALSE(seen.seen(1, 1));

    StoryRunner r(&s.db);
    REQUIRE(r.start());
    CHECK(seen.seen(r)); // beat 0 of the first scene
    REQUIRE(r.advance());
    CHECK_FALSE(seen.seen(r));
    seen.mark(r);
    CHECK(seen.seen(0, 1));

    seen.clear();
    CHECK(seen.count() == 0);
    seen.setDatabase(nullptr);
    CHECK_FALSE(seen.seen(0, 0));
}

TEST_CASE("seen lines round-trip through the save codec by scene id") {
    Script s;
    SeenLines seen;
    seen.setDatabase(&s.db);
    for (std::uint32_t b : {0u, 3u, 4u, 9u}) seen.mark(0, b); // scene A: digits 0..2
    seen.mark(1, 0);                                          // scene B

    StorySave save;
    save.scene = "SC-A";
    captureSeen(seen, save);
    REQUIRE(save.seen.size() == 2);
    CHECK(save.seen[0].scene == "SC-A");
    CHECK(save.seen[0].bits == "912"); // digit 0: beats 0 and 3 (1 + 8), digit 1: beat 4, digit 2: beat 9
    CHECK(save.seen[1].scene == "SC-B");
    CHECK(save.seen[1].bits == "1");

    StorySave decoded;
    std::string err;
    REQUIRE_MESSAGE(decodeStorySave(encodeStorySave(save), decoded, &err), err);
    REQUIRE(decoded.seen.size() == 2);

    SeenLines loaded;
    loaded.setDatabase(&s.db);
    loaded.mark(0, 2); // whatever was there is replaced
    CHECK(applySeen(decoded, loaded) == 5);
    CHECK(loaded.count() == 5);
    for (std::uint32_t b = 0; b < 10; ++b) CHECK(loaded.seen(0, b) == seen.seen(0, b));
    CHECK(loaded.seen(1, 0));
}

TEST_CASE("applying seen lines skips unknown scenes, beats past the end and bad digits") {
    Script s;
    SeenLines seen;
    seen.setDatabase(&s.db);
    StorySave save;
    save.seen.push_back({"SC-GONE", "ffff"});
    save.seen.push_back({"SC-B", "ff"});    // scene B has one beat: only beat 0 counts
    save.seen.push_back({"SC-A", "z1"});    // a bad digit is ignored, the next one is beat 4..7
    CHECK(applySeen(save, seen) == 2);
    CHECK(seen.seen(1, 0));
    CHECK(seen.seen(0, 4));
    CHECK_FALSE(seen.seen(0, 0));
    CHECK(applySeen(StorySave{}, seen) == 0); // an empty save clears
    CHECK(seen.count() == 0);
}

TEST_CASE("a format 1 save still loads, with nothing seen") {
    StorySave out;
    out.seen.push_back({"X", "1"});
    std::string err;
    REQUIRE_MESSAGE(decodeStorySave(R"({"format":1,"scene":"SC-A","beat":2,"flags":[]})", out, &err), err);
    CHECK(out.scene == "SC-A");
    CHECK(out.beat == 2);
    CHECK(out.seen.empty());

    // New saves are format 2 and carry the "seen" list.
    const std::string text = encodeStorySave(out);
    CHECK(text.find("\"format\":2") != std::string::npos);
    CHECK(text.find("\"seen\":[]") != std::string::npos);
}

TEST_CASE("skip fast-forwards lines on a short interval and stops at the choice") {
    Script s;
    StoryRunner r(&s.db);
    DialoguePresenter p(&r);
    p.setRevealRate(20.f); // slow typing: skip must show each line at once
    SeenLines seen;
    seen.setDatabase(&s.db);
    SkipController skip;
    skip.setMode(SkipMode::All);
    REQUIRE(r.start());

    // Not held: nothing moves however long we wait.
    for (int i = 0; i < 100; ++i) CHECK_FALSE(frame(r, p, skip, seen, 0.02f));
    CHECK(r.beatIndex() == 0);
    CHECK_FALSE(skip.skipping());

    skip.setHeld(true);
    int frames = 0, moves = 0;
    while (r.state() != StoryRunner::State::Choice && frames < 500) {
        ++frames;
        if (frame(r, p, skip, seen, 0.02f)) ++moves;
    }
    CHECK(r.state() == StoryRunner::State::Choice);
    CHECK(r.beatIndex() == 4);
    CHECK(moves == 4);
    // 4 lines at 0.08 s each, 0.02 s a frame: about 4 frames a line.
    CHECK(frames >= 14);
    CHECK(frames <= 20);

    // At the choice it waits for the player, however long the key is held.
    for (int i = 0; i < 100; ++i) CHECK_FALSE(frame(r, p, skip, seen, 0.02f));
    CHECK(r.state() == StoryRunner::State::Choice);
    CHECK_FALSE(skip.skipping());
    CHECK(p.optionsVisible()); // the prompt was shown whole

    // After the pick, skipping carries on through the line and the hold to the gameplay beat.
    REQUIRE(r.choose(0));
    frames = 0;
    while (r.state() != StoryRunner::State::Gameplay && frames < 500) {
        ++frames;
        frame(r, p, skip, seen, 0.02f);
    }
    CHECK(r.state() == StoryRunner::State::Gameplay);
    CHECK(r.beatIndex() == 8);
    // The gameplay beat waits for its own press.
    for (int i = 0; i < 50; ++i) CHECK_FALSE(frame(r, p, skip, seen, 0.02f));
    CHECK(r.state() == StoryRunner::State::Gameplay);
}

TEST_CASE("skip passes a hold at once") {
    Script s;
    StoryRunner r(&s.db);
    DialoguePresenter p(&r);
    p.setRevealRate(0.f);
    SeenLines seen;
    seen.setDatabase(&s.db);
    SkipController skip;
    skip.setMode(SkipMode::All);
    REQUIRE(r.restore({"SC-A", 6}));
    REQUIRE(r.state() == StoryRunner::State::Hold);
    skip.setHeld(true);
    CHECK(frame(r, p, skip, seen, 0.001f)); // SkipHold, no interval wait
    CHECK(r.state() == StoryRunner::State::Line);
    CHECK(r.beatIndex() == 7);
}

TEST_CASE("skip in seen mode stops at the first unseen line, which types out normally") {
    Script s;
    StoryRunner r(&s.db);
    DialoguePresenter p(&r);
    p.setRevealRate(10.f);
    SeenLines seen;
    seen.setDatabase(&s.db);
    seen.mark(0, 0);
    seen.mark(0, 1);
    SkipController skip;
    skip.setMode(SkipMode::Seen);
    skip.setHeld(true);
    REQUIRE(r.start());

    for (int i = 0; i < 200; ++i) frame(r, p, skip, seen, 0.02f);
    CHECK(r.beatIndex() == 2); // beat 2 was never seen
    CHECK_FALSE(skip.skipping());
    CHECK(p.revealedChars() > 0); // it is typing at the normal rate, not skipped
    CHECK(p.revealedChars() == p.totalChars()); // ...and 4 s is long enough to finish it
    CHECK(r.beatIndex() == 2);

    // The owner marks a beat seen when the player leaves it; the held key then carries on.
    seen.mark(r);
    for (int i = 0; i < 20; ++i) frame(r, p, skip, seen, 0.02f);
    CHECK(r.beatIndex() >= 3);
    // Beat 3 (a direction) is unseen too: skip stops there again, with its text part-way out.
    CHECK(r.beatIndex() == 3);
    p.update(0.f);
    CHECK(p.revealedChars() < p.totalChars());
}

TEST_CASE("skip allocates nothing") {
    Script s;
    StoryRunner r(&s.db);
    DialoguePresenter p(&r);
    p.setRevealRate(30.f);
    SeenLines seen;
    seen.setDatabase(&s.db);
    SkipController skip;
    skip.setMode(SkipMode::All);
    skip.setHeld(true);
    REQUIRE(r.start());

    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    int moves = 0;
    for (int i = 0; i < 400; ++i) {
        if (frame(r, p, skip, seen, 0.02f)) {
            seen.mark(r);
            ++moves;
        }
        if (r.state() == StoryRunner::State::Choice) r.choose(0);
        if (r.state() == StoryRunner::State::Gameplay) r.resume();
        if (r.finished()) r.start();
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(moves > 20);
    if (counting) CHECK(after == before);
}

TEST_CASE("auto-advance waits for the text, then the delay, and only moves lines and directions") {
    Script s;
    StoryRunner r(&s.db);
    DialoguePresenter p(&r);
    p.setRevealRate(10.f); // "Zero zero zero." is 15 characters: 1.5 s
    AutoAdvancer autoAdv;
    autoAdv.setDelay(1.0f);
    REQUIRE(r.start());

    // Off: never.
    for (int i = 0; i < 100; ++i) {
        p.update(0.05f);
        CHECK(autoAdv.update(0.05f, p).none());
    }
    CHECK(r.beatIndex() == 0);

    autoAdv.setEnabled(true);
    p.revealAll();
    double t = 0.0;
    bool moved = false;
    for (int i = 0; i < 100 && !moved; ++i) {
        p.update(0.05f);
        const DialogueCommand c = autoAdv.update(0.05f, p);
        t += 0.05;
        if (!c.none()) {
            CHECK(c.kind == DialogueCommand::Kind::Advance);
            moved = applyDialogueCommand(r, c);
        }
    }
    CHECK(moved);
    CHECK(t >= 0.95);
    CHECK(t <= 1.15);
    CHECK(r.beatIndex() == 1);

    // The next line starts its wait only once it has been typed out (1.3 s for 13 characters).
    t = 0.0;
    moved = false;
    for (int i = 0; i < 200 && !moved; ++i) {
        p.update(0.05f);
        const DialogueCommand c = autoAdv.update(0.05f, p);
        t += 0.05;
        if (!c.none()) moved = applyDialogueCommand(r, c);
    }
    CHECK(moved);
    CHECK(t >= 2.1);
    CHECK(t <= 2.5);

    // A choice is never advanced for the player.
    REQUIRE(r.restore({"SC-A", 4}));
    for (int i = 0; i < 200; ++i) {
        p.update(0.05f);
        CHECK(autoAdv.update(0.05f, p).none());
    }
    CHECK(r.state() == StoryRunner::State::Choice);
}

TEST_CASE("auto-advance waits for a voiced line to finish, then only the voice tail") {
    Script s;
    StoryRunner r(&s.db);
    DialoguePresenter p(&r);
    AutoAdvancer autoAdv;
    autoAdv.setEnabled(true);
    autoAdv.setDelay(1.0f);
    autoAdv.setVoiceTail(0.3f);
    REQUIRE(r.start());
    p.revealAll();

    // The voice runs 3 s, well past the delay: nothing moves while it plays.
    double t = 0.0;
    bool moved = false;
    for (int i = 0; i < 200 && !moved; ++i) {
        p.update(0.05f);
        const bool busy = t < 3.0;
        const DialogueCommand c = autoAdv.update(0.05f, p, busy);
        t += 0.05;
        if (!c.none()) {
            CHECK(c.kind == DialogueCommand::Kind::Advance);
            moved = applyDialogueCommand(r, c);
        }
    }
    CHECK(moved);
    CHECK(t >= 3.25);
    CHECK(t <= 3.45);
    CHECK(r.beatIndex() == 1);

    // The next line has no voice: back to the full delay.
    p.update(0.f); // picks up the new line
    p.revealAll();
    t = 0.0;
    moved = false;
    for (int i = 0; i < 200 && !moved; ++i) {
        p.update(0.05f);
        const DialogueCommand c = autoAdv.update(0.05f, p, false);
        t += 0.05;
        if (!c.none()) moved = applyDialogueCommand(r, c);
    }
    CHECK(moved);
    CHECK(t >= 0.95);
    CHECK(t <= 1.15);
}
