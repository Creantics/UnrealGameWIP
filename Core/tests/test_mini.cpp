#include "ftcore/Mini.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

// Runs `s` at 60 fps until it finishes or `seconds` of game time pass.
void runFor(MiniScreen& s, double seconds) {
    for (int i = 0; i < static_cast<int>(seconds * 60.0) && !s.finished(); ++i) s.tick(1.0 / 60.0);
}

// Plays the current round by hand: presses each note on time while `hit(i)` says so.
template <class Hit>
void playRound(MiniScreen& s, Hit hit) {
    const int tries = s.attempt();
    std::size_t next = 0;
    for (int frame = 0; frame < 6000 && s.playing() && s.attempt() == tries; ++frame) {
        const ComboSequence& q = s.sequence();
        if (next < q.size() && s.time() >= q.note(next).time) {
            if (hit(next)) s.press(miniButton(q.note(next).arrow));
            ++next;
        }
        s.tick(1.0 / 240.0);
    }
}

constexpr const char* kTable = R"J({
  "version": 1,
  "default": { "bpm": 90, "steps": 8, "lanes": 2, "seed": 3, "pass": 0.5, "tries": 2 },
  "minis": [
    { "scene": "SC-T1", "beat": 3, "bpm": 140, "steps": 16, "lanes": 4, "subdivision": 0.5 },
    { "scene": "SC-T1", "beat": 3, "step": 2, "tries": 1, "window": 1.5 }
  ]
})J";

} // namespace

TEST_CASE("mini: the round is a beat-grid chart of the def's shape") {
    MiniScreen s;
    MiniDef d;
    d.bpm = 120.0;
    d.steps = 10;
    d.lanes = 2;
    d.lead = 2.0;
    s.begin(true, d, false);
    REQUIRE(s.playing());
    const ComboSequence& q = s.sequence();
    REQUIRE(q.size() == 10);
    CHECK(q.note(0).time >= 2.0 - 1e-9);
    for (std::size_t i = 1; i < q.size(); ++i) {
        CHECK(q.note(i).time - q.note(i - 1).time == doctest::Approx(0.5));
        CHECK((q.note(i).arrow == Arrow::Left || q.note(i).arrow == Arrow::Right));
    }
    CHECK(s.secondsPerBeat() == doctest::Approx(0.5));
    CHECK(s.roundLength() == doctest::Approx(q.note(9).time + q.windows().good));
}

TEST_CASE("mini: hitting every note clears the round, then the screen finishes") {
    MiniScreen s;
    s.begin(true, defaultMiniDef(), false);
    playRound(s, [](std::size_t) { return true; });
    REQUIRE(s.showingResult());
    CHECK(s.outcome() == MiniOutcome::Cleared);
    CHECK(s.cleared());
    CHECK(s.lastAccuracy() == doctest::Approx(1.0f));
    CHECK(s.lastGrade() == Grade::Perfect);
    CHECK(std::strcmp(miniOutcomeName(s.outcome()), "CLEARED") == 0);
    runFor(s, MiniScreen::kResultTime + 0.1);
    CHECK(s.finished());
    CHECK(s.triesUsed() == 1);
}

TEST_CASE("mini: a failed round retries with a fresh chart, the last try continues anyway") {
    MiniScreen s;
    MiniDef d = defaultMiniDef();
    d.tries = 2;
    d.pass = 0.6;
    s.begin(true, d, false);
    std::vector<Arrow> first;
    for (std::size_t i = 0; i < s.sequence().size(); ++i) first.push_back(s.sequence().note(i).arrow);

    playRound(s, [](std::size_t i) { return i % 3 == 0; }); // about a third: below the pass mark
    REQUIRE(s.showingResult());
    CHECK(s.outcome() == MiniOutcome::Retry);
    CHECK_FALSE(s.cleared());
    runFor(s, MiniScreen::kResultTime + 0.05);
    REQUIRE(s.playing());
    CHECK(s.attempt() == 1);
    bool same = true;
    for (std::size_t i = 0; i < s.sequence().size() && i < first.size(); ++i) same = same && s.sequence().note(i).arrow == first[i];
    CHECK_FALSE(same); // the next seed

    playRound(s, [](std::size_t) { return false; });
    REQUIRE(s.showingResult());
    CHECK(s.outcome() == MiniOutcome::Continue); // no fail state: the story goes on
    CHECK(s.lastAccuracy() == doctest::Approx(0.0f));
    CHECK(s.bestAccuracy() > 0.0f);
    runFor(s, MiniScreen::kResultTime + 0.1);
    CHECK(s.finished());
    CHECK(s.triesUsed() == 2);
}

TEST_CASE("mini: input is ignored in the grace, under autoplay and with no step") {
    MiniScreen s;
    MiniDef d = defaultMiniDef();
    d.lead = 1.0;
    s.begin(true, d, false);
    s.press(miniButton(Arrow::Left));
    s.press(miniButton(Arrow::Right));
    CHECK(s.sequence().strays() == 0);
    runFor(s, 0.3);
    s.press(miniButton(Arrow::Up)); // nothing in range: a stray
    CHECK(s.sequence().strays() == 1);

    MiniScreen none;
    none.begin(false, d, false);
    CHECK(none.phase() == MiniPhase::NoStep);
    none.tick(5.0);
    none.press(miniButton(Arrow::Left));
    CHECK(none.phase() == MiniPhase::NoStep);
}

TEST_CASE("mini: autoplay clears the first try within about a second at any frame rate") {
    for (const double dt : {1.0 / 60.0, 1.0 / 7.5, 0.5, 2.0}) {
        CAPTURE(dt);
        MiniScreen s;
        MiniDef d = defaultMiniDef();
        d.steps = kMaxMiniSteps;
        d.bpm = 60.0;
        s.begin(true, d, true);
        double t = 0.0;
        while (!s.finished() && t < 10.0) {
            s.press(miniButton(Arrow::Left)); // ignored
            s.tick(dt);
            t += dt;
        }
        CHECK(s.finished());
        CHECK(s.cleared());
        CHECK(s.triesUsed() == 1);
        CHECK(s.lastAccuracy() == doctest::Approx(1.0f));
        CHECK(s.sequence().strays() == 0);
        if (dt < 0.1) CHECK(t <= MiniScreen::kAutoBudget + MiniScreen::kAutoResultTime + 0.1);
    }
}

TEST_CASE("mini: running a screen allocates nothing after begin") {
    MiniScreen s;
    MiniDef d = defaultMiniDef();
    d.steps = kMaxMiniSteps;
    d.lanes = 4;
    d.tries = 3;
    d.pass = 1.0;
    s.begin(true, d, false); // allocates its buffers
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    int frames = 0;
    std::size_t next = 0;
    int attempt = 0;
    while (!s.finished() && frames++ < 60 * 120) {
        if (s.attempt() != attempt) {
            attempt = s.attempt();
            next = 0;
        }
        const ComboSequence& q = s.sequence();
        if (s.playing() && next < q.size() && s.time() >= q.note(next).time) {
            if (next % 2 == 0) s.press(miniButton(q.note(next).arrow));
            ++next;
        }
        s.tick(1.0 / 60.0);
        (void)s.beat();
    }
    const auto allocs = ftcore_test::allocStats().count - before;
    if (counting) CHECK(allocs == 0);
    CHECK(s.finished());
    CHECK(s.triesUsed() == 3); // never passes at 1.0, so it used every try and went on
}

TEST_CASE("mini table: a fixture loads and is keyed by scene, beat and step") {
    MiniTable t;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importMiniTable(kTable, t, &err, &warnings), err);
    CHECK(warnings.empty());
    CHECK(t.fallback.bpm == doctest::Approx(90.0));
    CHECK(t.fallback.steps == 8);
    CHECK(t.fallback.tries == 2);
    REQUIRE(t.entries.size() == 2);
    const MiniDef& a = t.find("SC-T1", 3, 0);
    CHECK(a.bpm == doctest::Approx(140.0));
    CHECK(a.steps == 16);
    CHECK(a.lanes == 4);
    CHECK(a.subdivision == doctest::Approx(0.5));
    CHECK(a.tries == defaultMiniDef().tries); // a missing key is the built-in default
    CHECK(t.find("SC-T1", 3, 2).window == doctest::Approx(1.5));
    CHECK(t.find("SC-T1", 4, 0).bpm == doctest::Approx(90.0)); // the default
    CHECK(t.findEntry("SC-T1", 3, 1) == nullptr);
}

TEST_CASE("mini table: bad data is refused with paths, unknown keys are warnings") {
    MiniTable t;
    std::string err;
    const auto bad = [&](const char* json) {
        err.clear();
        return !importMiniTable(json, t, &err);
    };
    CHECK(bad("{ \"version\": 1 }")); // no default
    CHECK(bad("not json"));
    CHECK(bad(R"J({ "version": 2, "default": {} })J"));
    CHECK(bad(R"J({ "default": { "bpm": 20 } })J"));
    CHECK(err.find("default.bpm") != std::string::npos);
    CHECK(bad(R"J({ "default": { "steps": 99 } })J"));
    CHECK(bad(R"J({ "default": { "lanes": 1 } })J"));
    CHECK(bad(R"J({ "default": { "subdivision": 0.75 } })J"));
    CHECK(bad(R"J({ "default": { "pass": 1.5 } })J"));
    CHECK(bad(R"J({ "default": { "tries": 0 } })J"));
    CHECK(bad(R"J({ "default": { "steps": 8.5 } })J"));
    CHECK(bad(R"J({ "default": {}, "minis": [ { "beat": 1 } ] })J"));
    CHECK(bad(R"J({ "default": {}, "minis": [ { "scene": "A", "beat": 1 }, { "scene": "A", "beat": 1, "step": 0 } ] })J"));
    CHECK(err.find("duplicate") != std::string::npos);
    CHECK(t.entries.empty());

    std::vector<std::string> warnings;
    REQUIRE(importMiniTable(R"J({ "default": { "colour": 3 }, "x": 1, "minis": [ { "scene": "A", "beat": 0, "y": 2 } ] })J", t, &err,
                            &warnings));
    CHECK(warnings.size() == 3);
}

TEST_CASE("mini table: the committed table loads with only a default") {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Mini/mini.json", text));
    MiniTable t;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importMiniTable(text, t, &err, &warnings), err);
    CHECK(warnings.empty());
    CHECK(t.entries.empty());
    CHECK(t.fallback.steps == defaultMiniDef().steps);
}
