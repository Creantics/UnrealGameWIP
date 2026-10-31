#include "ftcore/Qte.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

QtePrompt tap(ActionButton a, double at, QteWindow w = QteWindow::Wide) {
    QtePrompt p;
    p.input = a;
    p.at = at;
    p.window = w;
    return p;
}

QtePrompt hold(ActionButton a, double at, double seconds) {
    QtePrompt p = tap(a, at);
    p.kind = QteKind::Hold;
    p.hold = seconds;
    return p;
}

QtePrompt mash(ActionButton a, double at, int count, double limit) {
    QtePrompt p = tap(a, at);
    p.kind = QteKind::Mash;
    p.mashCount = static_cast<std::uint8_t>(count);
    p.limit = limit;
    return p;
}

QteDef defOf(std::initializer_list<QtePrompt> prompts) {
    QteDef d;
    for (const QtePrompt& p : prompts) d.add(p);
    return d;
}

// The grade of the one-prompt sequence `p` pressed at `t` (Pending when nothing resolved).
QteGrade tapGrade(const QtePrompt& p, double t) {
    QteSequence q;
    q.begin(defOf({p}));
    q.press(p.input, t);
    q.update(p.at + 1.0);
    return q.grade(0);
}

constexpr const char* kTable = R"J({
  "version": 1,
  "default": { "prompts": [ { "input": "attack", "at": 1.0 }, { "input": "guard", "at": 2.0, "window": "narrow" } ] },
  "qtes": [
    { "scene": "SC-T1", "beat": 3, "prompts": [ { "input": "jump", "at": 0.8, "hold": 1.0 },
                                               { "input": "dodge", "at": 3.0, "mash": 6, "limit": 2.0 } ] },
    { "scene": "SC-T1", "beat": 3, "step": 2, "prompts": [ { "input": "guard", "at": 1.5 } ] }
  ]
})J";

} // namespace

TEST_CASE("qte: a wide tap is Perfect late in the window, Good earlier, a Miss outside it") {
    const QtePrompt p = tap(ActionButton::Attack, 1.0);
    CHECK(tapGrade(p, 0.97) == QteGrade::Perfect); // the last 0.06 s
    CHECK(tapGrade(p, 0.85) == QteGrade::Good);    // inside the 0.30 s window
    CHECK(tapGrade(p, 0.60) == QteGrade::Miss);    // too early: it ran out unanswered
    CHECK(tapGrade(p, 1.00) == QteGrade::Miss);    // the impact itself is too late
    CHECK(tapGrade(p, 1.20) == QteGrade::Miss);
}

TEST_CASE("qte: a narrow tap is Perfect in the parry window and Good in the wide one") {
    const QtePrompt p = tap(ActionButton::Guard, 1.0, QteWindow::Narrow);
    CHECK(tapGrade(p, 0.93) == QteGrade::Perfect);
    CHECK(tapGrade(p, 0.80) == QteGrade::Good);
    CHECK(tapGrade(p, 0.60) == QteGrade::Miss);
}

TEST_CASE("qte: the window widths are DefenceTimeline's") {
    const DefenceWindows w = qteWindows();
    const DefenceWindows d;
    CHECK(w.dodge == d.dodge);
    CHECK(w.perfectDodge == d.perfectDodge);
    CHECK(w.parry == d.parry);
    CHECK(w.parryLockout == 0.0); // a miss must not lock out the next prompt
    // Just inside and outside the wide window.
    const QtePrompt p = tap(ActionButton::Attack, 1.0);
    CHECK(tapGrade(p, 1.0 - d.dodge + 0.01) == QteGrade::Good);
    CHECK(tapGrade(p, 1.0 - d.dodge - 0.01) == QteGrade::Miss);
}

TEST_CASE("qte: a wrong direction is ignored, and a miss plays on") {
    QteSequence q;
    q.begin(defOf({tap(ActionButton::Attack, 1.0), tap(ActionButton::Dodge, 2.0), tap(ActionButton::Jump, 3.0)}));
    CHECK(q.press(ActionButton::Guard, 0.95).empty());
    CHECK(q.strays() == 1);
    CHECK(q.grade(0) == QteGrade::Pending);
    // Prompt 0 runs out; the next one answers normally (a narrow miss earlier doesn't lock anything).
    QteResults r = q.press(ActionButton::Dodge, 1.97);
    REQUIRE(r.size() == 2);
    CHECK(r[0].prompt == 0);
    CHECK(r[0].grade == QteGrade::Miss);
    CHECK(r[1].prompt == 1);
    CHECK(r[1].grade == QteGrade::Perfect);
    CHECK_FALSE(q.complete());
    q.press(ActionButton::Jump, 2.85);
    CHECK(q.complete());
    CHECK(q.perfects() == 1);
    CHECK(q.goods() == 1);
    CHECK(q.misses() == 1);
    CHECK(q.outcome() == QteOutcome::Fair);
}

TEST_CASE("qte: a hold keeps its tap grade when held, and misses when let go early") {
    const QteDef def = defOf({hold(ActionButton::Jump, 1.0, 1.0)});
    {
        QteSequence q;
        q.begin(def);
        CHECK(q.press(ActionButton::Jump, 0.97).empty());
        CHECK(q.holding());
        CHECK(q.holdProgress() == doctest::Approx(0.0).epsilon(0.001));
        q.update(1.5);
        CHECK(q.holdProgress() == doctest::Approx(0.5));
        CHECK(q.release(ActionButton::Jump, 1.9).empty()); // inside the slack of the end: fine
        const QteResults r = q.update(2.0);
        REQUIRE(r.size() == 1);
        CHECK(r[0].grade == QteGrade::Perfect);
        CHECK(q.complete());
    }
    {
        QteSequence q;
        q.begin(def);
        q.press(ActionButton::Jump, 0.85);
        const QteResults r = q.release(ActionButton::Jump, 1.4);
        REQUIRE(r.size() == 1);
        CHECK(r[0].grade == QteGrade::Miss);
        CHECK(q.complete());
    }
    {
        QteSequence q; // a good press held to the end stays Good
        q.begin(def);
        q.press(ActionButton::Jump, 0.85);
        q.update(2.0);
        CHECK(q.grade(0) == QteGrade::Good);
    }
}

TEST_CASE("qte: a mash is graded by how soon the count is reached") {
    const QteDef def = defOf({mash(ActionButton::Dodge, 1.0, 5, 2.0)});
    {
        QteSequence q; // done within 0.6 of the limit (1.2 s): Perfect
        q.begin(def);
        for (int i = 0; i < 5; ++i) q.press(ActionButton::Dodge, 1.1 + 0.1 * i);
        CHECK(q.complete());
        CHECK(q.grade(0) == QteGrade::Perfect);
    }
    {
        QteSequence q; // done late: Good
        q.begin(def);
        for (int i = 0; i < 5; ++i) q.press(ActionButton::Dodge, 1.9 + 0.1 * i);
        CHECK(q.grade(0) == QteGrade::Good);
    }
    {
        QteSequence q; // short of the count: Miss when the time is up
        q.begin(def);
        for (int i = 0; i < 4; ++i) q.press(ActionButton::Dodge, 1.1 + 0.1 * i);
        CHECK(q.mashPresses() == 4);
        CHECK_FALSE(q.complete());
        q.update(3.0);
        CHECK(q.grade(0) == QteGrade::Miss);
        CHECK(q.outcome() == QteOutcome::Poor);
    }
}

TEST_CASE("qte: co-op alternates the actor slot, a plain QTE uses slot 0") {
    const QteDef def = defOf({tap(ActionButton::Attack, 1.0), tap(ActionButton::Attack, 2.0), tap(ActionButton::Attack, 3.0)});
    QteSequence q;
    q.begin(def, true);
    CHECK(q.coop());
    CHECK(q.prompt(0).actor == 0);
    CHECK(q.prompt(1).actor == 1);
    CHECK(q.prompt(2).actor == 0);
    q.begin(def, false);
    CHECK(q.prompt(1).actor == 0);
}

TEST_CASE("qte: the overall outcome is graded, never a fail") {
    QteSequence q;
    q.begin(QteDef{});
    CHECK(q.complete());
    CHECK(q.outcome() == QteOutcome::Perfect);

    const QteDef four = defOf({tap(ActionButton::Attack, 1.0), tap(ActionButton::Attack, 2.0), tap(ActionButton::Attack, 3.0), tap(ActionButton::Attack, 4.0)});
    q.begin(four);
    q.press(ActionButton::Attack, 0.97);
    q.press(ActionButton::Attack, 1.97);
    q.press(ActionButton::Attack, 2.97);
    q.press(ActionButton::Attack, 3.97);
    CHECK(q.outcome() == QteOutcome::Perfect);

    q.begin(four);
    q.press(ActionButton::Attack, 0.85);
    q.press(ActionButton::Attack, 1.97);
    q.update(10.0);
    CHECK(q.misses() == 2);
    CHECK(q.outcome() == QteOutcome::Fair); // half missed

    q.begin(four);
    q.update(10.0);
    CHECK(q.complete());
    CHECK(q.outcome() == QteOutcome::Poor); // the story still goes on
    CHECK(std::strcmp(qteOutcomeName(QteOutcome::Poor), "Poor") == 0);
    CHECK(std::strcmp(qteGradeName(QteGrade::Perfect), "PERFECT") == 0);
    CHECK(std::strcmp(qteGradeName(QteGrade::Pending), "") == 0);
}

TEST_CASE("qte: finish grades what is still open as a Miss") {
    QteSequence q;
    q.begin(defOf({tap(ActionButton::Attack, 1.0), hold(ActionButton::Guard, 3.0, 1.0), mash(ActionButton::Jump, 6.0, 4, 1.0)}));
    q.press(ActionButton::Attack, 0.97);
    const QteResults r = q.finish(3.5);
    CHECK(r.size() == 2);
    CHECK(q.complete());
    CHECK(q.grade(0) == QteGrade::Perfect);
    CHECK(q.grade(1) == QteGrade::Miss);
    CHECK(q.grade(2) == QteGrade::Miss);
}

TEST_CASE("qte: the ring closes from 1 to 0 over the tell, and only while the prompt is open") {
    QteSequence q;
    q.begin(defOf({tap(ActionButton::Attack, 2.0)}));
    CHECK(q.tellProgress(0, 1.0) == -1.0); // not yet
    CHECK(q.tellProgress(0, 1.5) == doctest::Approx(0.5 / 0.6));
    CHECK(q.tellProgress(0, 1.7) == doctest::Approx(0.5));
    CHECK(q.tellProgress(0, 2.0) == -1.0);
    CHECK(q.tellProgress(5, 1.7) == -1.0);
    q.press(ActionButton::Attack, 1.9);
    CHECK(q.tellProgress(0, 1.95) == -1.0); // answered
    // A prompt near the start has a shorter ring.
    CHECK(qteTell(tap(ActionButton::Attack, 0.5)) == doctest::Approx(0.5));
    CHECK(qteTell(tap(ActionButton::Attack, 3.0)) == doctest::Approx(kQteTell));
}

TEST_CASE("qte: under hold toggle a hold latches on its press, and begin turns the toggle off") {
    const QteDef def = defOf({hold(ActionButton::Jump, 1.0, 1.0)});
    QteSequence q;
    q.begin(def);
    q.setHoldToggle(true);
    CHECK(q.holdToggle());
    q.press(ActionButton::Jump, 0.97);
    CHECK(q.release(ActionButton::Jump, 1.1).empty()); // let go at once: still held
    CHECK(q.holding());
    const QteResults r = q.update(2.0);
    REQUIRE(r.size() == 1);
    CHECK(r[0].grade == QteGrade::Perfect);

    q.begin(def);
    CHECK_FALSE(q.holdToggle());
    q.press(ActionButton::Jump, 0.97);
    const QteResults early = q.release(ActionButton::Jump, 1.1);
    REQUIRE(early.size() == 1);
    CHECK(early[0].grade == QteGrade::Miss);
}

TEST_CASE("qte: the screen's assist turns on hold toggle and auto-complete") {
    const QteDef def = defOf({tap(ActionButton::Attack, 1.0), hold(ActionButton::Jump, 2.0, 1.0)});
    QteScreen s;
    s.begin(true, def, false, false, QteAssist{false, true});
    CHECK(s.sequence().holdToggle());
    CHECK_FALSE(s.autoComplete());

    s.begin(true, def, false, false);
    CHECK_FALSE(s.sequence().holdToggle());

    // Auto-complete plays itself perfectly on the normal clock and ignores the player.
    s.begin(true, def, false, false, QteAssist{true, false});
    CHECK(s.autoComplete());
    CHECK(s.clockScale() == 1.0);
    double game = 0.0;
    while (!s.finished() && game < 30.0) {
        s.press(ActionButton::Guard); // a stray press would count against the player
        s.tick(1.0 / 60.0);
        game += 1.0 / 60.0;
    }
    REQUIRE(s.finished());
    CHECK(s.sequence().strays() == 0);
    CHECK(s.sequence().outcome() == QteOutcome::Perfect);
    CHECK(game >= def.length()); // not compressed

    // Under autoplay the autopilot already drives it, so auto-complete stays off; no step, off too.
    s.begin(true, def, false, true, QteAssist{true, false});
    CHECK_FALSE(s.autoComplete());
    s.begin(false, def, false, false, QteAssist{true, false});
    CHECK_FALSE(s.autoComplete());
}

TEST_CASE("qte: the autopilot plays the default sequence perfectly at any frame rate") {
    for (const double dt : {1.0 / 60.0, 1.0 / 30.0, 0.13, 0.5}) {
        QteScreen s;
        s.begin(true, defaultQteDef(), false, true);
        int frames = 0;
        while (!s.finished() && frames++ < 4000) s.tick(dt);
        REQUIRE(s.finished());
        CHECK(s.sequence().perfects() == s.sequence().size());
        CHECK(s.sequence().outcome() == QteOutcome::Perfect);
    }
}

TEST_CASE("qte: autoplay fits a long sequence in the cap") {
    QteDef def;
    for (int i = 0; i < 12; ++i) def.add(tap(kActionButtons[static_cast<std::size_t>(i) & 3u], 1.0 + 2.5 * i));
    REQUIRE(def.length() > 25.0);
    QteScreen s;
    s.begin(true, def, true, true);
    CHECK(s.clockScale() > 3.0);
    double game = 0.0;
    while (!s.finished() && game < 60.0) {
        s.tick(0.1);
        game += 0.1;
    }
    REQUIRE(s.finished());
    CHECK(game <= QteScreen::kAutoCap + QteScreen::kResultTime + 0.2);
    CHECK(s.sequence().perfects() == 12);
}

TEST_CASE("qte: one huge frame still plays every prompt at its own time") {
    QteScreen s;
    s.begin(true, defaultQteDef(), false, true);
    s.tick(QteScreen::kAutoCap + 5.0);
    CHECK(s.sequence().complete());
    CHECK(s.showingResult());
    CHECK(s.sequence().outcome() == QteOutcome::Perfect);
}

TEST_CASE("qte: the screen shows the result for a second, then finishes") {
    QteScreen s;
    s.begin(true, defOf({tap(ActionButton::Attack, 1.0)}), false, false);
    CHECK(s.playing());
    s.tick(0.5);
    s.press(ActionButton::Attack); // inside the wide window (0.5 s before is too early)
    CHECK(s.sequence().strays() == 1);
    s.tick(0.4); // t = 0.9: 0.1 s before the impact
    s.press(ActionButton::Attack);
    CHECK(s.sequence().grade(0) == QteGrade::Good);
    const QteResults fresh = s.takeFresh();
    REQUIRE(fresh.size() == 1);
    CHECK(fresh[0].grade == QteGrade::Good);
    CHECK(s.takeFresh().empty());
    s.tick(0.01);
    CHECK(s.showingResult());
    s.tick(QteScreen::kResultTime - 0.1);
    CHECK_FALSE(s.finished());
    s.tick(0.2);
    CHECK(s.finished());
}

TEST_CASE("qte: input is ignored in the grace, under autoplay and with no step") {
    QteScreen s;
    s.begin(true, defOf({tap(ActionButton::Attack, 0.6)}), false, false);
    s.press(ActionButton::Attack); // t = 0: the press that opened the screen
    CHECK(s.sequence().strays() == 0);
    CHECK(s.sequence().grade(0) == QteGrade::Pending);

    s.begin(true, defOf({tap(ActionButton::Attack, 1.0)}), false, true);
    s.tick(0.95);
    s.press(ActionButton::Attack); // a human press under autoplay
    CHECK(s.sequence().strays() == 0);

    s.begin(false, defaultQteDef(), false, true);
    CHECK(s.phase() == QtePhase::NoStep);
    s.tick(5.0);
    CHECK(s.phase() == QtePhase::NoStep);
    CHECK(s.takeFresh().empty());
}

TEST_CASE("qte: running a sequence allocates nothing") {
    QteScreen s;
    QteDef def = defaultQteDef();
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    s.begin(true, def, true, true);
    int frames = 0;
    while (!s.finished() && frames++ < 4000) {
        s.tick(1.0 / 60.0);
        (void)s.takeFresh();
    }
    QteSequence q;
    q.begin(def, true);
    q.press(ActionButton::Attack, 0.97);
    q.release(ActionButton::Attack, 1.0);
    q.update(9.0);
    (void)q.outcome();
    const auto allocs = ftcore_test::allocStats().count - before;
    if (counting) CHECK(allocs == 0);
    CHECK(s.finished());
}

TEST_CASE("qte table: a fixture loads and is keyed by scene, beat and step") {
    QteTable t;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importQteTable(kTable, t, &err, &warnings), err);
    CHECK(warnings.empty());
    CHECK(t.fallback.size() == 2);
    CHECK(t.fallback[1].window == QteWindow::Narrow);
    REQUIRE(t.entries.size() == 2);

    const QteDef& a = t.find("SC-T1", 3, 0);
    REQUIRE(a.size() == 2);
    CHECK(a[0].kind == QteKind::Hold);
    CHECK(a[0].input == ActionButton::Jump);
    CHECK(a[0].hold == doctest::Approx(1.0));
    CHECK(a[1].kind == QteKind::Mash);
    CHECK(a[1].mashCount == 6);
    CHECK(a[1].limit == doctest::Approx(2.0));
    CHECK(t.find("SC-T1", 3, 2).size() == 1);
    CHECK(t.find("SC-T1", 4, 0).size() == 2); // the default
    CHECK(t.find("SC-OTHER", 3, 0).size() == 2);
    CHECK(t.findEntry("SC-T1", 3, 1) == nullptr);
}

TEST_CASE("qte table: bad data is refused with paths, unknown keys are warnings") {
    QteTable t;
    std::string err;
    std::vector<std::string> warnings;
    const auto bad = [&](const char* json) {
        err.clear();
        return !importQteTable(json, t, &err);
    };
    CHECK(bad("{ \"version\": 1 }")); // no default
    CHECK(bad("not json"));
    CHECK(bad(R"J({ "default": { "prompts": [] } })J"));
    CHECK(bad(R"J({ "default": { "prompts": [ { "input": "diagonal", "at": 1.0 } ] } })J"));
    CHECK(err.find("default.prompts[0].input") != std::string::npos);
    // The QTE is not a battle: arrows are not an input (they belong to the Turns battle's combos).
    for (const char* arrow : {"left", "down", "up", "right"}) {
        const std::string json = std::string(R"J({ "default": { "prompts": [ { "input": ")J") + arrow +
                                 R"J(", "at": 1.0 } ] } })J";
        CHECK(bad(json.c_str()));
    }
    CHECK(bad(R"J({ "default": { "prompts": [ { "input": "attack", "at": 0.1 } ] } })J")); // too early
    CHECK(bad(R"J({ "default": { "prompts": [ { "input": "attack", "at": 1.0 }, { "input": "guard", "at": 1.2 } ] } })J")); // gap
    CHECK(bad(R"J({ "default": { "prompts": [ { "input": "attack", "at": 1.0, "hold": 2.0 }, { "input": "guard", "at": 2.5 } ] } })J"));
    CHECK(bad(R"J({ "default": { "prompts": [ { "input": "attack", "at": 1.0, "hold": 1.0, "mash": 4, "limit": 1.0 } ] } })J"));
    CHECK(bad(R"J({ "default": { "prompts": [ { "input": "attack", "at": 1.0, "mash": 4 } ] } })J")); // no limit
    CHECK(bad(R"J({ "default": { "prompts": [ { "input": "attack", "at": 1.0, "limit": 1.0 } ] } })J"));
    CHECK(bad(R"J({ "default": { "prompts": [ { "input": "attack", "at": 1.0, "window": "huge" } ] } })J"));
    CHECK(bad(R"J({ "default": { "prompts": [ { "input": "attack", "at": 1.0 } ] },
                    "qtes": [ { "scene": "A", "beat": 1, "prompts": [ { "input": "guard", "at": 1.0 } ] },
                              { "scene": "A", "beat": 1, "prompts": [ { "input": "guard", "at": 1.0 } ] } ] })J"));
    CHECK(err.find("duplicate") != std::string::npos);
    CHECK(t.entries.empty()); // failure leaves it empty

    REQUIRE(importQteTable(R"J({ "default": { "prompts": [ { "input": "attack", "at": 1.0, "color": 3 } ], "x": 1 } })J", t, &err,
                           &warnings));
    CHECK(warnings.size() == 2);
}

TEST_CASE("qte table: the built-in default is valid data") {
    const QteDef def = defaultQteDef();
    REQUIRE(def.size() > 0);
    double earliest = kQteMinFirst;
    for (std::size_t i = 0; i < def.size(); ++i) {
        CHECK(def[i].at >= earliest);
        earliest = def[i].end() + kQteMinGap;
    }
    QteScreen s;
    s.begin(true, def, false, true);
    CHECK(s.clockScale() == 1.0); // fits the autoplay budget as it is
}

TEST_CASE("qte table: Data/Qte/qte.json loads") {
    std::string text;
    if (!ftcore_test::readProjectData("Qte/qte.json", text)) {
        MESSAGE("Data/Qte/qte.json not found; skipped");
        return;
    }
    QteTable t;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importQteTable(text, t, &err, &warnings), err);
    CHECK(warnings.empty());
    CHECK(t.fallback.size() > 0);
    for (const QteEntry& e : t.entries) CHECK(e.def.size() > 0);
}

TEST_CASE("action buttons: names and data spellings round-trip") {
    for (ActionButton b : kActionButtons) {
        ActionButton read = b == ActionButton::Attack ? ActionButton::Jump : ActionButton::Attack;
        CHECK(parseActionButton(actionButtonKey(b), read));
        CHECK(read == b);
        CHECK(std::strlen(actionButtonName(b)) > 0);
    }
    ActionButton untouched = ActionButton::Guard;
    CHECK_FALSE(parseActionButton("left", untouched));
    CHECK_FALSE(parseActionButton("ATTACK", untouched));
    CHECK_FALSE(parseActionButton("", untouched));
    CHECK(untouched == ActionButton::Guard);
    CHECK(std::strcmp(actionButtonName(ActionButton::Dodge), "DODGE") == 0);
}
