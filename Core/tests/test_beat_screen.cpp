#include "ftcore/BeatScreen.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace ftcore;

// The beat screen (docs/BEAT_SCREEN.md): the phases, autoplay and its retries, the Retry / Quit prompt, the
// input grace, the assists and the counts the host logs.
//
// Most tests run the default fight at 120 bpm: a beat is 0.5 s and an eighth 0.25 s. A fight begun at 0
// with the default one-bar lead-in has its first bar line at beat 4 (2.0 s), and the bar is generated
// 1.0 s before that.

namespace {

constexpr double kFrame = 1.0 / 60.0;

BeatPattern patternOf(const char* text) {
    BeatPattern p;
    std::string error;
    REQUIRE_MESSAGE(parseBeatPattern(text, p, &error), error);
    return p;
}

BeatSetup setupOf(const BeatFightDef& def, const BeatTuning& tuning = BeatTuning{}) {
    BeatSetup s;
    s.def = def;
    s.tuning = tuning;
    return s;
}

BeatSetup defaultSetup() { return setupOf(defaultBeatFightDef()); }

// The default tuning with the finisher off, so a test sees only its pattern's prompts.
BeatTuning plain() {
    BeatTuning t;
    t.finisherAt = 0.0;
    return t;
}

// The default fight with one enemy that plays these patterns, against a lead with `leadHp`.
BeatFightDef oneEnemy(std::initializer_list<const char*> patterns, int enemyHp, int leadHp) {
    BeatFightDef def = defaultBeatFightDef();
    def.hp = leadHp;
    BeatEnemy& e = def.enemies[0];
    e.hp = enemyHp;
    e.patternCount = static_cast<std::uint8_t>(patterns.size());
    std::size_t i = 0;
    for (const char* p : patterns) e.patterns[i++] = patternOf(p);
    return def;
}

bool isFinished(const BeatScreen& s) { return s.finished(); }
bool isResult(const BeatScreen& s) { return s.showingResult(); }
bool isRetryPrompt(const BeatScreen& s) { return s.askingRetry(); }

// Frames of `dt` from `clock` until `done(screen)` holds or `limit` clock seconds have passed. False when
// the limit ran out first.
template <class Done>
bool tickUntil(BeatScreen& s, double& clock, double limit, Done done, double dt = kFrame) {
    const double end = clock + limit;
    while (clock < end) {
        if (done(s)) return true;
        clock += dt;
        s.tick(clock);
    }
    return done(s);
}

// Every prompt the fight shows over an attempt, as text ("sA0 cD8 "): the kind, the arrow and the eighth
// from the first bar line. Two attempts of the same fight give the same text.
struct Watcher {
    std::uint32_t lastId = 0;
    std::string text;

    void look(const BeatFight& f) {
        for (std::size_t i = 0; i < f.promptCount(); ++i) {
            const BeatPrompt& p = f.prompt(i);
            if (p.id <= lastId) continue;
            lastId = p.id;
            text += p.kind == BeatKind::Strike ? 's' : (p.kind == BeatKind::Counter ? 'c' : 'h');
            text += "AGDJ"[static_cast<int>(p.input)];
            text += std::to_string(static_cast<int>(std::lround((p.beat - f.firstBarBeat()) * 2.0)));
            text += p.finisher ? "! " : " ";
        }
    }
};

// The bars a player who presses nothing sees in one try of the default fight with `seed`.
std::string barsSeen(std::uint32_t seed) {
    BeatScreen s;
    s.begin(true, defaultSetup(), seed, false, {}, 0.0);
    double clock = 0.0;
    Watcher w;
    for (int frames = 0; !s.askingRetry() && frames < 60 * 600; ++frames) {
        clock += kFrame;
        s.tick(clock);
        w.look(s.fight());
    }
    return w.text;
}

int countOf(const BeatEvents& e, BeatEventType type) {
    int n = 0;
    for (const BeatEvent& ev : e) n += ev.type == type ? 1 : 0;
    return n;
}

} // namespace

// ---- autoplay ----

TEST_CASE("beat screen: autoplay wins the default fight at any frame rate, with no retry and no stray") {
    for (const double dt : {1.0 / 60.0, 1.0 / 30.0, 0.13, 0.5, 2.0}) {
        CAPTURE(dt);
        BeatScreen s;
        s.begin(true, defaultSetup(), 1, true, {}, 0.0);
        CHECK(s.autoPlay());
        CHECK(s.playing());
        double clock = 0.0;
        REQUIRE(tickUntil(s, clock, 600.0, isFinished, dt));
        CHECK(s.won());
        CHECK(s.outcome() == BeatOutcome::Won);
        CHECK(s.fight().phase() == BeatPhase::Victory);
        CHECK_FALSE(s.gaveUp());
        CHECK(s.autoRetries() == 0);
        CHECK(s.stats().restarts == 0);
        // The autopilot is perfect, whatever the frame: nothing missed, no stray, no damage taken.
        CHECK(s.stats().perfects > 0);
        CHECK(s.stats().goods == 0);
        CHECK(s.stats().misses == 0);
        CHECK(s.stats().strays == 0);
        CHECK(s.stats().whiffs == 0);
        CHECK(s.fight().hp() == s.fight().maxHp());
        CHECK(s.playSeconds() > 0.0);
    }
}

TEST_CASE("beat screen: autoplay wins the default fight and every fight in beat.json") {
    std::vector<std::pair<std::string, BeatSetup>> fights;
    fights.emplace_back("built-in default", defaultSetup());
    std::string text;
    if (!ftcore_test::readProjectData("Battle/beat.json", text)) {
        FAIL("Data/Battle/beat.json not found");
    }
    BeatTable table;
    std::string error;
    REQUIRE_MESSAGE(importBeatTable(text, table, &error), error);
    fights.emplace_back("beat.json default", table.fallback);
    for (const BeatEntry& e : table.entries) fights.emplace_back("beat.json " + e.scene, e.setup);
    // Every enemy of the file on its own, and all of them together, at a slow, a normal and a fast tempo.
    for (const double bpm : {60.0, 120.0, 240.0}) {
        for (std::size_t i = 0; i <= table.enemies.size(); ++i) {
            BeatSetup s = table.fallback;
            s.def.bpm = bpm;
            if (i < table.enemies.size()) {
                s.def.enemyCount = 1;
                s.def.enemies[0] = table.enemies[i];
            } else {
                s.def.enemyCount = static_cast<std::uint8_t>(std::min(table.enemies.size(), kMaxBeatEnemies));
                for (std::size_t k = 0; k < s.def.enemyCount; ++k) s.def.enemies[k] = table.enemies[k];
            }
            const std::string name = (i < table.enemies.size() ? table.enemyIds[i] : std::string("all")) + " at " +
                                     std::to_string(static_cast<int>(bpm)) + " bpm";
            fights.emplace_back(name, s);
        }
    }
    for (const auto& entry : fights) {
        CAPTURE(entry.first);
        for (std::uint32_t seed = 1; seed <= 3; ++seed) {
            CAPTURE(seed);
            BeatScreen s;
            s.begin(true, entry.second, seed, true, {}, 0.0);
            double clock = 0.0;
            REQUIRE(tickUntil(s, clock, 1200.0, isFinished));
            CHECK(s.won());
            CHECK_FALSE(s.gaveUp());
            CHECK(s.stats().restarts == 0);
            CHECK(s.stats().misses == 0);
            CHECK(s.stats().strays == 0);
            CHECK(s.fight().hp() == entry.second.def.hp);
        }
    }
}

TEST_CASE("beat screen: autoplay ignores the player") {
    BeatScreen s;
    s.begin(true, defaultSetup(), 1, true, {}, 0.0);
    double clock = 0.0;
    bool pressed = false;
    for (int frames = 0; frames < 60 * 600 && !s.finished(); ++frames) {
        clock += kFrame;
        s.press(ActionButton::Attack, clock); // a player mashing away
        s.release(ActionButton::Attack, clock);
        s.tick(clock);
        pressed = true;
        if (s.askingRetry()) break;
    }
    CHECK(pressed);
    CHECK(s.finished());
    CHECK(s.won());
    CHECK(s.stats().strays == 0);
    CHECK(s.stats().misses == 0);
    CHECK_FALSE(s.choose(BeatChoice::Quit)); // there is no prompt to answer
}

TEST_CASE("beat screen: autoplay retries a lost fight three times, then ends the step lost") {
    // The counter's windows are empty, so not even the autopilot can answer it: it is lost at the first one.
    BeatTuning tuning = plain();
    tuning.counter.dodge = 0.0;
    tuning.counter.parry = 0.0;
    const BeatSetup setup = setupOf(oneEnemy({"cD . . . . . . ."}, 60, 1), tuning);
    BeatScreen s;
    s.begin(true, setup, 1, true, {}, 0.0);
    double clock = 0.0;
    bool sawRetry = false;
    int lastRetries = 0;
    for (int frames = 0; frames < 60 * 600 && !s.finished(); ++frames) {
        clock += kFrame;
        s.tick(clock);
        // Autoplay never stops at the prompt, and it counts each restart as it makes it.
        CHECK_FALSE(s.askingRetry());
        if (s.autoRetries() != lastRetries) {
            sawRetry = true;
            CHECK(s.autoRetries() == lastRetries + 1);
            CHECK(s.playing());
            lastRetries = s.autoRetries();
        }
    }
    REQUIRE(s.finished());
    CHECK(sawRetry);
    CHECK(s.outcome() == BeatOutcome::Lost);
    CHECK_FALSE(s.won());
    CHECK(s.gaveUp());
    CHECK(s.autoRetries() == BeatScreen::kAutoRetries);
    CHECK(s.stats().restarts == BeatScreen::kAutoRetries);
    CHECK(s.fight().phase() == BeatPhase::Defeat);
}

TEST_CASE("beat screen: one huge frame still plays the whole fight") {
    BeatScreen a;
    a.begin(true, defaultSetup(), 1, true, {}, 0.0);
    a.tick(1e9);
    CHECK(a.showingResult());
    CHECK(a.fight().phase() == BeatPhase::Victory);
    CHECK(a.stats().misses == 0);
    CHECK(a.now() == doctest::Approx(1e9));
    a.tick(1e9 + BeatScreen::kAutoResultTime + 0.1);
    CHECK(a.finished());
    CHECK(a.won());

    // A player who never pressed anything loses in one frame too, and the result then waits for the player.
    BeatScreen h;
    h.begin(true, defaultSetup(), 1, false, {}, 0.0);
    h.tick(1e9);
    CHECK(h.showingResult());
    CHECK(h.fight().phase() == BeatPhase::Defeat);
    CHECK(h.resultAge() == doctest::Approx(0.0));
}

// ---- a lost fight and the prompt ----

TEST_CASE("beat screen: with no input the fight is lost, the result shows, then the retry prompt asks") {
    BeatScreen s;
    s.begin(true, defaultSetup(), 1, false, {}, 0.0);
    CHECK(s.playing());
    CHECK_FALSE(s.autoPlay());
    CHECK_FALSE(s.autoComplete());
    double clock = 0.0;
    REQUIRE(tickUntil(s, clock, 600.0, isResult));
    CHECK(s.fight().phase() == BeatPhase::Defeat);
    CHECK(s.fight().hp() == 0);
    CHECK(s.stats().perfects == 0);
    CHECK(s.resultAge() == doctest::Approx(0.0));
    CHECK(s.outcome() == BeatOutcome::None);
    const double shown = clock;

    REQUIRE(tickUntil(s, clock, 10.0, isRetryPrompt));
    CHECK(clock - shown == doctest::Approx(BeatScreen::kResultTime).epsilon(0.02));
    CHECK(s.resultAge() >= BeatScreen::kResultTime);
    CHECK(s.outcome() == BeatOutcome::None);
    CHECK_FALSE(s.finished());

    // It waits for the player: nothing moves on its own, however long it takes.
    for (int i = 0; i < 100; ++i) {
        clock += 1.0;
        s.tick(clock);
    }
    CHECK(s.askingRetry());
    CHECK(s.stats().restarts == 0);
    CHECK(s.autoRetries() == 0);
    CHECK_FALSE(s.gaveUp());
}

TEST_CASE("beat screen: Retry and Quit wait out the prompt's grace, and mean nothing outside the prompt") {
    BeatScreen s;
    s.begin(true, defaultSetup(), 1, false, {}, 0.0);
    CHECK_FALSE(s.choose(BeatChoice::Retry)); // still playing
    CHECK_FALSE(s.choose(BeatChoice::Quit));
    CHECK(s.playing());

    double clock = 0.0;
    REQUIRE(tickUntil(s, clock, 600.0, isRetryPrompt));
    CHECK_FALSE(s.choose(BeatChoice::Retry)); // the press that ended the fight can't pick
    CHECK_FALSE(s.choose(BeatChoice::Quit));
    CHECK(s.askingRetry());
    clock += BeatScreen::kPromptGrace * 0.5;
    s.tick(clock);
    CHECK_FALSE(s.choose(BeatChoice::Quit));
    clock += BeatScreen::kPromptGrace;
    s.tick(clock);
    CHECK(s.choose(BeatChoice::Quit));
    CHECK(s.finished());
    CHECK(s.outcome() == BeatOutcome::Lost);
    CHECK_FALSE(s.won());
    CHECK_FALSE(s.gaveUp()); // the player quit; autoplay didn't give up
    CHECK(s.stats().restarts == 0);

    // Done is final.
    CHECK_FALSE(s.choose(BeatChoice::Retry));
    s.tick(clock + 100.0);
    CHECK(s.finished());
    CHECK(s.outcome() == BeatOutcome::Lost);
}

TEST_CASE("beat screen: Retry restarts the same fight, with the same bars in the same order") {
    BeatScreen s;
    s.begin(true, defaultSetup(), 7, false, {}, 0.0);
    double clock = 0.0;
    double played = 0.0; // clock seconds of frames that began while the fight was playing
    Watcher first;
    for (int frames = 0; !s.askingRetry() && frames < 60 * 600; ++frames) {
        if (s.playing()) played += kFrame;
        clock += kFrame;
        s.tick(clock);
        first.look(s.fight());
    }
    REQUIRE(s.askingRetry());
    CHECK(s.fight().phase() == BeatPhase::Defeat);
    CHECK_FALSE(first.text.empty());

    clock += BeatScreen::kPromptGrace;
    s.tick(clock);
    REQUIRE(s.choose(BeatChoice::Retry));
    // The same fight from the top, the counts carried on.
    CHECK(s.playing());
    CHECK(s.fight().phase() == BeatPhase::Playing);
    CHECK(s.fight().hp() == s.fight().maxHp());
    CHECK(s.fight().chain() == 0);
    CHECK(s.fight().promptCount() == 0);
    CHECK(s.stats().restarts == 1);
    CHECK(s.takeFresh().empty());
    CHECK(s.outcome() == BeatOutcome::None);

    Watcher second;
    for (int frames = 0; !s.askingRetry() && frames < 60 * 600; ++frames) {
        if (s.playing()) played += kFrame;
        clock += kFrame;
        s.tick(clock);
        second.look(s.fight());
    }
    REQUIRE(s.askingRetry());
    CHECK(second.text == first.text);
    CHECK(s.stats().restarts == 1);
    CHECK(s.playSeconds() == doctest::Approx(played));

    // A fresh screen with that seed plays the same bars as well, and some other seed plays other bars (the
    // default fight has two patterns, so only some seeds differ).
    CHECK(barsSeen(7) == first.text);
    bool differs = false;
    for (std::uint32_t seed = 1; seed <= 16 && !differs; ++seed) {
        if (seed != 7) differs = barsSeen(seed) != first.text;
    }
    CHECK(differs);
}

TEST_CASE("beat screen: after a Retry the grace drops early presses again") {
    BeatScreen s;
    s.begin(true, defaultSetup(), 1, false, {}, 0.0);
    double clock = 0.0;
    REQUIRE(tickUntil(s, clock, 600.0, isRetryPrompt));
    clock += BeatScreen::kPromptGrace;
    s.tick(clock);
    REQUIRE(s.choose(BeatChoice::Retry));
    const int strays = s.stats().strays;
    s.press(ActionButton::Attack, clock + 0.1); // inside the grace of the new start
    CHECK(s.stats().strays == strays);
    s.press(ActionButton::Attack, clock + BeatScreen::kInputGrace + 0.05); // nothing is open yet: a stray
    CHECK(s.stats().strays == strays + 1);
}

// ---- input ----

TEST_CASE("beat screen: presses in the grace after the start are dropped, later ones count") {
    BeatScreen s;
    s.begin(true, defaultSetup(), 1, false, {}, 5.0); // the clock starts at 5 s
    s.tick(5.05);
    s.press(ActionButton::Attack, 5.1); // the press that opened the screen
    s.release(ActionButton::Attack, 5.2);
    CHECK(s.stats().strays == 0);
    s.press(ActionButton::Attack, 5.0 + BeatScreen::kInputGrace + 0.05);
    CHECK(s.stats().strays == 1);
    CHECK(s.takeFresh().size() == 1);
}

TEST_CASE("beat screen: a screen with no step stays inert") {
    BeatScreen idle; // never begun
    CHECK(idle.phase() == BeatScreenPhase::NoStep);
    idle.tick(5.0);
    CHECK(idle.phase() == BeatScreenPhase::NoStep);

    BeatScreen s;
    s.begin(false, defaultSetup(), 1, true, {}, 0.0);
    CHECK(s.phase() == BeatScreenPhase::NoStep);
    CHECK_FALSE(s.autoPlay()); // there is nothing to play
    CHECK_FALSE(s.autoComplete());
    s.tick(100.0);
    s.press(ActionButton::Attack, 100.0);
    s.release(ActionButton::Attack, 100.0);
    CHECK_FALSE(s.choose(BeatChoice::Retry));
    CHECK(s.phase() == BeatScreenPhase::NoStep);
    CHECK_FALSE(s.playing());
    CHECK_FALSE(s.finished());
    CHECK(s.outcome() == BeatOutcome::None);
    CHECK(s.now() == doctest::Approx(0.0));
    CHECK(s.playSeconds() == doctest::Approx(0.0));
    CHECK(s.takeFresh().empty());
    CHECK(s.stats().perfects == 0);
    CHECK(s.stats().strays == 0);
    CHECK(s.stats().bars == 0);
    CHECK(s.leadInBeats() == doctest::Approx(0.0));
}

TEST_CASE("beat screen: the press that wins the fight shows the result at once, and the screen finishes after it") {
    // One strike kills the only enemy (5 hp against a strike of 10).
    BeatScreen s;
    s.begin(true, setupOf(oneEnemy({"sA . . . . . . ."}, 5, 100), plain()), 3, false, {}, 0.0);
    s.tick(1.0); // the first bar is generated 1.0 s before its line at 2.0 s
    REQUIRE(s.fight().promptCount() == 1);
    s.takeFresh();
    s.press(ActionButton::Attack, 2.0); // on the strike's eighth
    CHECK(s.showingResult());
    CHECK(s.fight().phase() == BeatPhase::Victory);
    CHECK(s.outcome() == BeatOutcome::None); // not done until the result has been shown
    const BeatEvents fresh = s.takeFresh();
    CHECK(countOf(fresh, BeatEventType::Judged) == 1);
    CHECK(countOf(fresh, BeatEventType::EnemyHit) == 1);
    CHECK(countOf(fresh, BeatEventType::EnemyDown) == 1);
    CHECK(countOf(fresh, BeatEventType::Victory) == 1);
    CHECK(fresh[0].grade == QteGrade::Perfect);
    CHECK(s.takeFresh().empty()); // the list starts over after a call
    CHECK(s.stats().perfects == 1);
    CHECK(s.stats().maxChain == 1);
    CHECK(s.resultAge() == doctest::Approx(0.0));

    // The result shows for kResultTime.
    s.tick(2.0 + BeatScreen::kResultTime - 0.1);
    CHECK(s.showingResult());
    CHECK_FALSE(s.finished());
    CHECK(s.resultAge() == doctest::Approx(BeatScreen::kResultTime - 0.1));
    // Input does nothing now.
    s.press(ActionButton::Attack, 3.5);
    CHECK(s.stats().strays == 0);
    s.tick(2.0 + BeatScreen::kResultTime + 0.1);
    CHECK(s.finished());
    CHECK(s.won());
    CHECK(s.outcome() == BeatOutcome::Won);
    CHECK_FALSE(s.gaveUp());
    CHECK(s.playSeconds() == doctest::Approx(2.0));
}

TEST_CASE("beat screen: the clock never runs back, and the lead-in counts beats down") {
    BeatScreen s;
    s.begin(true, defaultSetup(), 1, false, {}, 0.0);
    CHECK(s.leadInBeats() == doctest::Approx(4.0)); // the first bar line is at beat 4
    s.tick(1.0);
    CHECK(s.now() == doctest::Approx(1.0));
    CHECK(s.leadInBeats() == doctest::Approx(2.0));
    s.tick(0.5); // an earlier time is read as the latest one
    CHECK(s.now() == doctest::Approx(1.0));
    CHECK(s.leadInBeats() == doctest::Approx(2.0));
    s.tick(2.5);
    CHECK(s.leadInBeats() == doctest::Approx(0.0));
    CHECK(s.fight().stats().bars == 1);
    CHECK(s.fight().turnEnemy() == 0);

    // Not playing, nothing to count.
    BeatScreen idle;
    CHECK(idle.leadInBeats() == doctest::Approx(0.0));
}

// ---- the assists ----

TEST_CASE("beat screen: the assist's auto-complete plays the fight and ignores the player; autoplay is not auto-complete") {
    QteAssist assist;
    assist.autoComplete = true;
    BeatScreen s;
    s.begin(true, defaultSetup(), 1, false, assist, 0.0);
    CHECK(s.autoComplete());
    CHECK_FALSE(s.autoPlay());
    double clock = 0.0;
    bool won = false;
    for (int frames = 0; frames < 60 * 600 && !s.showingResult(); ++frames) {
        clock += kFrame;
        s.press(ActionButton::Guard, clock); // whatever the player does is ignored
        s.release(ActionButton::Guard, clock);
        s.tick(clock);
        won = s.fight().phase() == BeatPhase::Victory;
    }
    CHECK(won);
    CHECK(s.showingResult());
    CHECK(s.stats().strays == 0);
    CHECK(s.stats().misses == 0);
    CHECK(s.stats().damageTaken == 0);
    // A player is watching: the result shows for the full time, as for any player.
    s.tick(clock + BeatScreen::kResultTime - 0.1);
    CHECK_FALSE(s.finished());
    s.tick(clock + BeatScreen::kResultTime + 0.1);
    CHECK(s.finished());
    CHECK(s.won());

    // Under autoplay the assist is moot, and with no step neither applies.
    BeatScreen a;
    a.begin(true, defaultSetup(), 1, true, assist, 0.0);
    CHECK(a.autoPlay());
    CHECK_FALSE(a.autoComplete());
    BeatScreen n;
    n.begin(false, defaultSetup(), 1, true, assist, 0.0);
    CHECK_FALSE(n.autoPlay());
    CHECK_FALSE(n.autoComplete());
}

TEST_CASE("beat screen: hold toggle latches a hold on its press") {
    const BeatSetup setup = setupOf(oneEnemy({"hD2 - . . . . . ."}, 1000, 1000), plain());
    // Without the assist, letting go early breaks the hold.
    {
        BeatScreen s;
        s.begin(true, setup, 1, false, {}, 0.0);
        s.tick(1.0);
        s.press(ActionButton::Dodge, 2.0);
        s.release(ActionButton::Dodge, 2.1);
        s.tick(2.6);
        CHECK(s.stats().misses == 1);
        CHECK(s.stats().perfects == 0);
    }
    // With it the release does nothing, and the hold completes with its press's grade.
    {
        QteAssist assist;
        assist.holdToggle = true;
        BeatScreen s;
        s.begin(true, setup, 1, false, assist, 0.0);
        s.tick(1.0);
        s.press(ActionButton::Dodge, 2.0);
        s.release(ActionButton::Dodge, 2.1);
        s.tick(2.6);
        CHECK(s.stats().misses == 0);
        CHECK(s.stats().perfects == 1);
        // Under autoplay the toggle still applies and the fight is won as before.
        BeatScreen a;
        a.begin(true, defaultSetup(), 1, true, assist, 0.0);
        double clock = 0.0;
        REQUIRE(tickUntil(a, clock, 600.0, isFinished));
        CHECK(a.won());
        CHECK(a.stats().misses == 0);
    }
}

// ---- allocation ----

TEST_CASE("beat screen: playing, retrying and finishing allocate nothing after begin") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    // Everything that allocates (the setups) is made first.
    const BeatSetup normal = defaultSetup();
    BeatTuning tuning = plain();
    tuning.counter.dodge = 0.0;
    tuning.counter.parry = 0.0;
    const BeatSetup hopeless = setupOf(oneEnemy({"cD . . . . . . ."}, 60, 1), tuning);
    QteAssist assist;
    assist.autoComplete = true;

    int sink = 0;
    const std::uint64_t before = ftcore_test::allocStats().count;
    {
        // Autoplay wins a fight.
        BeatScreen s;
        s.begin(true, normal, 1, true, {}, 0.0);
        double clock = 0.0;
        for (int frames = 0; frames < 60 * 600 && !s.finished(); ++frames) {
            clock += kFrame;
            sink += static_cast<int>(s.takeFresh().size());
            s.tick(clock);
        }
        sink += static_cast<int>(s.phase());
    }
    {
        // A player's fight is lost, retried and quit, with keys thrown at it all the while.
        BeatScreen s;
        s.begin(true, normal, 2, false, {}, 0.0);
        double clock = 0.0;
        for (int frames = 0; frames < 60 * 600 && !s.askingRetry(); ++frames) {
            clock += kFrame;
            s.press(ActionButton::Dodge, clock);
            s.release(ActionButton::Dodge, clock);
            s.tick(clock);
        }
        clock += BeatScreen::kPromptGrace + kFrame;
        s.tick(clock);
        sink += s.choose(BeatChoice::Retry) ? 1 : 0;
        for (int frames = 0; frames < 60 * 600 && !s.askingRetry(); ++frames) {
            clock += kFrame;
            s.tick(clock);
        }
        clock += BeatScreen::kPromptGrace + kFrame;
        s.tick(clock);
        sink += s.choose(BeatChoice::Quit) ? 1 : 0;
        sink += static_cast<int>(s.outcome());
    }
    {
        // Autoplay that gives up, and an auto-completing screen.
        BeatScreen s;
        s.begin(true, hopeless, 1, true, {}, 0.0);
        double clock = 0.0;
        for (int frames = 0; frames < 60 * 600 && !s.finished(); ++frames) {
            clock += kFrame;
            s.tick(clock);
        }
        sink += s.gaveUp() ? 1 : 0;
        BeatScreen c;
        c.begin(true, normal, 3, false, assist, 0.0);
        c.tick(1e6);
        sink += static_cast<int>(c.phase());
    }
    const std::uint64_t allocs = ftcore_test::allocStats().count - before;
    CHECK(allocs == 0);
    CHECK(sink > 0);
}
