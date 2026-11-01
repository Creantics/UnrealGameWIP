#include "ftcore/BeatBoss.h"
#include "ftcore/ComboSequence.h" // comboSequencesBuilt: the fence test

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

using namespace ftcore;

// The beat boss (docs/COMBAT_SYSTEMS.md, "Beat boss"): the loader and its refusals, the bar map and
// the beat line, judging, the streak, silences, finishers, checkpoints, the autopilot and allocation.
//
// The test track (Data/Battle/beat_boss/test_track.json) is synthetic. Its first phase is 120 bpm in
// 4/4 (a beat 0.5 s, an eighth 0.25 s) with a 4-bar loop of 4/4, 4/4, 5/4 and 3/4 (8 s); its second is
// 6/8 at 80 (a dotted beat 0.75 s, an eighth 0.25 s) with a 5-bar loop whose third bar is 7/8 (7.75 s).
// A fight begun at 0 counts one bar in (2.0 s), so the first phase's track time t is at clock 2.0 + t
// until something stops the track. A fight of the second phase alone counts 1.5 s in.

namespace {

BeatBossTrack loadTrack() {
    std::string text;
    BeatBossTrack track;
    if (!ftcore_test::readProjectData("Battle/beat_boss/test_track.json", text)) {
        FAIL("Data/Battle/beat_boss/test_track.json not found");
        return track;
    }
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importBeatBossTrack(text, track, &error, &warnings), error);
    CHECK(warnings.empty());
    return track;
}

// The first phase alone (the last, so it ends with the boss down), or the second.
BeatBossDef onlyPhase(const BeatBossDef& def, int phase) {
    BeatBossDef d = def;
    d.phases[0] = def.phases[static_cast<std::size_t>(phase)];
    d.phases[0].endAt = 0.0f;
    d.phaseCount = 1;
    return d;
}

struct Log {
    std::vector<BeatBossEvent> events;
    void add(const BeatBossEvents& e) {
        for (const BeatBossEvent& x : e) events.push_back(x);
    }
    int count(BeatBossEventType t) const {
        return static_cast<int>(std::count_if(events.begin(), events.end(),
                                              [t](const BeatBossEvent& e) { return e.type == t; }));
    }
    const BeatBossEvent* find(BeatBossEventType t, std::size_t from = 0) const {
        for (std::size_t i = from; i < events.size(); ++i)
            if (events[i].type == t) return &events[i];
        return nullptr;
    }
};

bool has(const BeatBossEvents& e, BeatBossEventType t) {
    for (const BeatBossEvent& x : e)
        if (x.type == t) return true;
    return false;
}

const BeatBossEvent* first(const BeatBossEvents& e, BeatBossEventType t) {
    for (const BeatBossEvent& x : e)
        if (x.type == t) return &x;
    return nullptr;
}

// One host frame: the autopilot's moves up to `now`, then the clock.
void pilotFrame(BeatBoss& f, double now, Log& log) {
    for (int guard = 0; guard < 64; ++guard) {
        const QteAction a = f.autopilot(now);
        if (a.kind == QteAction::Kind::None) break;
        log.add(a.kind == QteAction::Kind::Press ? f.press(a.input, a.time) : f.release(a.input, a.time));
    }
    log.add(f.update(now));
}

// Frames of `dt` from `from` until the fight is over or `until`; returns the clock it stopped at.
double play(BeatBoss& f, double from, double dt, double until, Log& log, bool pilot) {
    double now = from;
    while (!f.over() && now < until) {
        now += dt;
        if (pilot) pilotFrame(f, now, log);
        else log.add(f.update(now));
    }
    return now;
}

constexpr int kWinBars = 24; // the autopilot beats the test track within this many bars (it takes 17)

std::string swapText(std::string s, const std::string& from, const std::string& to) {
    const std::size_t at = s.find(from);
    REQUIRE_MESSAGE(at != std::string::npos, from);
    s.replace(at, from.size(), to);
    return s;
}

// A small valid track: 120 bpm, bars of 4/4 and 3/4 (3.5 s).
const std::string kMini = R"({"version": 1, "id": "mini", "boss": {"hp": 100}, "lead": {"hp": 100},
  "stems": [{"id": "s"}],
  "phases": [{"id": "p", "bpm": 120, "meter": "4/4", "loop": {"bars": 2, "seconds": 3.5},
    "bars": [{"bar": 2, "meter": "3/4"}],
    "moves": [{"id": "m1", "tell": "1.1", "impact": "1.3", "answer": "parry", "stem": "s", "miss": 0.2},
              {"id": "m2", "tell": "2.1", "impact": "2.2", "answer": "attack", "stem": "s"}]}]})";

void refused(const std::string& json, const std::string& needle) {
    BeatBossTrack t;
    std::string error;
    CHECK_FALSE(importBeatBossTrack(json, t, &error));
    CHECK_MESSAGE(error.find(needle) != std::string::npos, error);
    CHECK(t.def.phaseCount == 0);
}

} // namespace

TEST_CASE("beat boss: the test track loads with no warnings, and its bar maps are what the file says") {
    const BeatBossTrack track = loadTrack();
    const BeatBossDef& d = track.def;
    CHECK(track.id == "test_track");
    REQUIRE(d.phaseCount == 2);
    CHECK(track.phaseIds == std::vector<std::string>{"phase_a", "phase_b"});
    CHECK(d.stemCount == 3);
    CHECK(d.rewardCount == 3);

    const BarMap& a = d.phases[0];
    CHECK(a.beatEighths == 2);
    CHECK(a.beatSeconds() == doctest::Approx(0.5));
    CHECK(a.loopBars == 4);
    CHECK(a.bars[0].eighths == 8);
    CHECK(a.bars[2].eighths == 10); // 5/4
    CHECK(a.bars[3].eighths == 6);  // 3/4
    CHECK(a.barOffset(3) == doctest::Approx(6.5));
    CHECK(a.barOffset(4) == doctest::Approx(8.0));
    CHECK(a.moves[0].impactAt == doctest::Approx(1.0));
    CHECK(a.moves[4].answer == BeatBossAnswer::Release);
    CHECK(a.moves[4].endAt == doctest::Approx(6.0));
    CHECK(a.rests[2].time == doctest::Approx(6.25)); // "3.5&"
    CHECK(a.finisherCount == 3);
    CHECK(a.silences[0].hp == doctest::Approx(0.8f));

    // 6/8 counts in dotted quarters.
    const BarMap& b = d.phases[1];
    CHECK(b.beatEighths == 3);
    CHECK(b.beatSeconds() == doctest::Approx(0.75));
    CHECK(b.eighthSeconds() == doctest::Approx(0.25));
    CHECK(b.bars[1].emptyBeats == 0b10);
    CHECK(b.bars[2].eighths == 7); // 7/8: beats of 3, 3 and 1 eighths
    CHECK(b.loopSeconds == doctest::Approx(7.75));
    CHECK(b.moves[2].answer == BeatBossAnswer::Paired);
    CHECK(b.moves[2].button == ActionButton::Guard);
    CHECK(b.moves[2].second == ActionButton::Attack);
    CHECK(b.moves[2].opens);
    CHECK(b.moves[5].endAt == doctest::Approx(7.25)); // "5.2&"
    CHECK(b.finisher[2].impactAt == doctest::Approx(1.25)); // "1.2a"
    CHECK(b.silences[0].bar == 1);
    CHECK(b.silences[0].loop == 2);
    CHECK(b.count == BeatCountKind::Voice);

    // The shipped data keeps the rules the loader enforces.
    for (std::size_t p = 0; p < d.phaseCount; ++p) {
        const BarMap& m = d.phases[p];
        CHECK(m.bpm <= kMaxBossBpm);
        for (std::size_t i = 0; i < m.moveCount; ++i) {
            const PinnedMove& mv = m.moves[i];
            CHECK(mv.missCost <= kMaxBossMissCost);
            CHECK(mv.impactAt - mv.tellAt >= kMinBossTell - 1e-9);
            if (isBossCounter(mv.answer)) CHECK(mv.impactAt - mv.tellAt >= m.beatSeconds() - 1e-9);
            for (std::size_t k = i + 1; k < m.moveCount; ++k)
                CHECK(std::fabs(m.moves[k].impactAt - mv.impactAt) >= kMinBossGap);
        }
    }
}

TEST_CASE("beat boss: positions") {
    BossPos p;
    CHECK(parseBossPos("1.1", 2, p));
    CHECK(p.bar == 1);
    CHECK(p.eighth == 0);
    CHECK(parseBossPos("4.2&", 2, p));
    CHECK(p.bar == 4);
    CHECK(p.eighth == 3);
    CHECK(parseBossPos("2.2a", 3, p));
    CHECK(p.eighth == 5);
    CHECK(parseBossPos("2.2&", 3, p));
    CHECK(p.eighth == 4);
    for (const char* bad : {"", "1", "1.", ".1", "0.1", "1.0", "1.1&&", "1.1a", "a.1", "1.1 ", "1,1"})
        CHECK_MESSAGE(!parseBossPos(bad, 2, p), bad);
    CHECK_FALSE(parseBossPos("1.1&a", 3, p));

    CHECK(std::string(beatBossAnswerKey(BeatBossAnswer::Paired)) == "paired");
    BeatBossAnswer a = BeatBossAnswer::Attack;
    CHECK(parseBeatBossAnswer("release", a));
    CHECK(a == BeatBossAnswer::Release);
    CHECK_FALSE(parseBeatBossAnswer("up", a));
}

TEST_CASE("beat boss: the loader refuses, each with its path") {
    {
        BeatBossTrack t;
        std::string error;
        std::vector<std::string> warnings;
        REQUIRE_MESSAGE(importBeatBossTrack(kMini, t, &error, &warnings), error);
        CHECK(warnings.empty());
    }
    SUBCASE("bad JSON") { refused(R"({"version": 1,)", "beat boss file: "); }
    SUBCASE("a prompt on an empty beat") {
        refused(swapText(kMini, R"("meter": "4/4",)", R"("meter": "4/4", "empty": [3],)"),
                R"(phases[0].moves[0].impact: "1.3" is on an empty beat)");
    }
    SUBCASE("a prompt on a lost beat: past a shortened bar's last beat") {
        refused(swapText(kMini, R"("impact": "2.2")", R"("impact": "2.4")"),
                R"(phases[0].moves[1].impact: "2.4" is a lost beat (bar 2 is 3/4))");
    }
    SUBCASE("a prompt on a lost beat: an eighth past a 7/8 bar's end") {
        std::string s = swapText(kMini, R"("bar": 2, "meter": "3/4")", R"("bar": 2, "meter": "7/8")");
        s = swapText(s, R"("seconds": 3.5)", R"("seconds": 3.75)");
        BeatBossTrack t;
        std::string error;
        CHECK_MESSAGE(importBeatBossTrack(swapText(s, R"("impact": "2.2")", R"("impact": "2.4")"), t, &error), error);
        refused(swapText(s, R"("impact": "2.2")", R"("impact": "2.4&")"),
                R"(phases[0].moves[1].impact: "2.4&" is a lost beat (bar 2 is 7/8))");
    }
    SUBCASE("6/8 has two beats a bar") {
        std::string s = swapText(kMini, R"("meter": "4/4")", R"("meter": "6/8")");
        s = swapText(s, R"("bars": [{"bar": 2, "meter": "3/4"}],)", "");
        s = swapText(s, R"("bpm": 120)", R"("bpm": 80)");
        s = swapText(s, R"("seconds": 3.5)", R"("seconds": 3.0)");
        s = swapText(s, R"("impact": "1.3")", R"("impact": "1.2")");
        s = swapText(s, R"("impact": "2.2")", R"("impact": "2.2a")");
        BeatBossTrack t;
        std::string error;
        REQUIRE_MESSAGE(importBeatBossTrack(s, t, &error), error);
        CHECK(t.def.phases[0].moves[1].impactAt == doctest::Approx(1.5 + 0.75 + 0.5));
        refused(swapText(s, R"("impact": "2.2a")", R"("impact": "2.3")"), R"("2.3" is a lost beat (bar 2 is 6/8))");
    }
    SUBCASE("a tell under 0.4 s") {
        refused(swapText(kMini, R"("impact": "2.2")", R"("impact": "2.1&")"),
                "phases[0].moves[1].tell: 0.25 s before the impact, under the 0.4 s floor");
    }
    SUBCASE("a counter's tell under one beat") {
        std::string s = swapText(kMini, R"("bpm": 120)", R"("bpm": 60)");
        s = swapText(s, R"("seconds": 3.5)", R"("seconds": 7)");
        refused(swapText(s, R"("tell": "1.1", "impact": "1.3")", R"("tell": "1.2", "impact": "1.2&")"),
                "phases[0].moves[0].tell: a counter's tell spans at least one beat");
    }
    SUBCASE("a tempo over 150") {
        std::string s = swapText(kMini, R"("bpm": 120)", R"("bpm": 151)");
        refused(s, "phases[0].bpm: expected a number in 40..150");
        std::string ok = swapText(kMini, R"("bpm": 120)", R"("bpm": 150)");
        ok = swapText(ok, R"("seconds": 3.5)", R"("seconds": 2.8)");
        BeatBossTrack t;
        std::string error;
        CHECK_MESSAGE(importBeatBossTrack(ok, t, &error), error);
    }
    SUBCASE("two prompts closer than 0.2 s") {
        refused(swapText(kMini, R"("tell": "2.1", "impact": "2.2")", R"("tell": "1.1", "impact": "1.3")"),
                "phases[0].moves[1].impact: 0 s from moves[0]'s, under 0.2 s");
        // At 150 in 6/8 an eighth is 0.133 s.
        std::string s = swapText(kMini, R"("meter": "4/4")", R"("meter": "6/8")");
        s = swapText(s, R"("bars": [{"bar": 2, "meter": "3/4"}],)", "");
        s = swapText(s, R"("bpm": 120)", R"("bpm": 150)");
        s = swapText(s, R"("seconds": 3.5)", R"("seconds": 1.6)");
        s = swapText(s, R"("impact": "1.3")", R"("impact": "2.1")");
        s = swapText(s, R"("tell": "2.1", "impact": "2.2")", R"("tell": "1.1", "impact": "2.2")");
        BeatBossTrack t;
        std::string error;
        CHECK_MESSAGE(importBeatBossTrack(s, t, &error), error);
        refused(swapText(s, R"("impact": "2.2")", R"("impact": "2.1&")"), "phases[0].moves[1].impact: 0.133333 s from");
    }
    SUBCASE("an impact inside a hold") {
        std::string s = swapText(kMini, R"("answer": "attack")", R"("answer": "hold", "end": "3.1")");
        s = swapText(s, R"("tell": "1.1", "impact": "1.3")", R"("tell": "2.1", "impact": "2.3")");
        refused(s, "phases[0].moves[0].impact: inside the hold of moves[1]");
    }
    SUBCASE("phase bars that don't sum to the loop") {
        refused(swapText(kMini, R"("seconds": 3.5)", R"("seconds": 4.0)"),
                "phases[0].loop: its 2 bars sum to 3.5 s, not 4 s");
    }
    SUBCASE("a miss cost over 35%") {
        refused(swapText(kMini, R"("miss": 0.2)", R"("miss": 0.36)"), "phases[0].moves[0].miss: expected a number in 0..0.35");
        BeatBossTrack t;
        std::string error;
        CHECK_MESSAGE(importBeatBossTrack(swapText(kMini, R"("miss": 0.2)", R"("miss": 0.35)"), t, &error), error);
    }
    SUBCASE("a miss cost on a strike, or none on a counter") {
        refused(swapText(kMini, R"("answer": "attack", "stem": "s")", R"("answer": "attack", "stem": "s", "miss": 0.1)"),
                "phases[0].moves[1].miss: only a counter");
        refused(swapText(kMini, R"(, "miss": 0.2)", ""), "phases[0].moves[0].miss: a counter needs its miss cost");
    }
    SUBCASE("answers, pairs and stems") {
        refused(swapText(kMini, R"("answer": "parry")", R"("answer": "up")"), "phases[0].moves[0].answer: expected attack");
        refused(swapText(kMini, R"("answer": "parry")", R"("answer": "paired", "pair": ["guard", "guard"])"),
                "phases[0].moves[0].pair: expected two different buttons");
        refused(swapText(kMini, R"("answer": "attack", "stem": "s")", R"("answer": "attack", "stem": "x")"),
                "phases[0].moves[1].stem: expected one of the track's stems");
        refused(swapText(kMini, R"("answer": "attack")", R"("answer": "hold")"), "phases[0].moves[1].end: expected a place");
    }
    SUBCASE("the last phase ends with the boss down") {
        refused(swapText(kMini, R"("bpm": 120,)", R"("bpm": 120, "endAt": 0.5,)"), "phases[0].endAt: the last phase");
    }
    SUBCASE("unknown keys are warnings") {
        BeatBossTrack t;
        std::string error;
        std::vector<std::string> warnings;
        CHECK(importBeatBossTrack(swapText(kMini, R"("bpm": 120,)", R"("bpm": 120, "tempo": 3,)"), t, &error, &warnings));
        REQUIRE(warnings.size() == 1);
        CHECK(warnings[0] == "phases[0]: unknown key: tempo");
    }
}

TEST_CASE("beat boss: the beat line lays out the current bar and the next as the bar map has them") {
    const BeatBossTrack track = loadTrack();
    const BarMap& m = track.def.phases[0];
    BeatBoss f;
    f.begin(track.def, BeatBossTuning{}, 0.0);
    BeatLine line;

    // The lead-in: a count bar of four numerals, then bar 1 with its first moves not yet told.
    f.beatLine(line);
    CHECK(line.counting);
    CHECK_FALSE(line.frozen);
    CHECK(line.bars[0].count);
    CHECK(line.bars[0].beats == 4);
    CHECK(line.bars[1].number == 1);
    CHECK(line.seconds == doctest::Approx(4.0));
    CHECK(line.ticks[0].numeral == 1);
    CHECK(line.ticks[3].numeral == 4);
    CHECK(line.ticks[4].kind == BeatTickKind::BarLine);
    CHECK(line.ticks[4].numeral == 0);
    REQUIRE(line.markerCount == 2); // bar 1's parry and strike
    CHECK(line.markers[0].prompt == 0);
    CHECK(line.markers[0].x == doctest::Approx(3.0 / 4.0));
    CHECK(line.markers[1].x == doctest::Approx(3.5 / 4.0));
    REQUIRE(line.restCount == 2); // "1.2", "1.4&"
    CHECK(line.rests[0].x == doctest::Approx(2.5 / 4.0));

    // Bar by bar through the first loop, idle (the lead survives a loop of misses).
    Log log;
    int checked = 0;
    int lastBar = 0;
    for (double now = 0.0; now < 2.0 + 8.0 + 0.1; now += 1.0 / 120.0) {
        log.add(f.update(now));
        if (!f.trackRunning() || f.bar() == lastBar) continue;
        lastBar = f.bar();
        const std::size_t b = static_cast<std::size_t>(f.bar() - 1);
        const std::size_t next = (b + 1) % m.loopBars;
        f.beatLine(line);
        CHECK_FALSE(line.counting);
        CHECK(line.bars[0].number == f.bar());
        CHECK(line.bars[0].eighths == m.bars[b].eighths);
        CHECK(line.bars[0].seconds == doctest::Approx(m.barSeconds(b)));
        CHECK(line.bars[1].number == static_cast<int>(next + 1));
        CHECK(line.bars[1].seconds == doctest::Approx(m.barSeconds(next)));
        CHECK(line.bars[0].longBar == (m.bars[b].eighths > 8));
        CHECK(line.bars[0].shortBar == (m.bars[b].eighths < 8));
        const std::size_t beats = line.bars[0].beats + line.bars[1].beats;
        CHECK(line.bars[0].beats == m.bars[b].eighths / 2);
        REQUIRE(line.tickCount == beats + 1);
        CHECK(line.ticks[0].kind == BeatTickKind::BarLine);
        CHECK(line.ticks[line.bars[0].beats].kind == BeatTickKind::BarLine);
        CHECK(line.ticks[line.bars[0].beats].at == doctest::Approx(m.barSeconds(b)));
        CHECK(line.ticks[beats].kind == BeatTickKind::End);
        CHECK(line.ticks[beats].x == doctest::Approx(1.0f));
        for (std::size_t i = 1; i < line.tickCount; ++i) CHECK(line.ticks[i].x > line.ticks[i - 1].x);
        CHECK(line.playheadX >= 0.0f);
        CHECK(line.playheadX < line.ticks[line.bars[0].beats].x);
        // Each rest of these two bars is where the map puts it.
        std::size_t rests = 0;
        for (std::size_t r = 0; r < m.restCount; ++r)
            if (static_cast<std::size_t>(m.rests[r].at.bar) == b + 1 || static_cast<std::size_t>(m.rests[r].at.bar) == next + 1) ++rests;
        CHECK(line.restCount == rests);
        ++checked;
    }
    CHECK(checked == 5); // bars 1 to 4, and the next loop's bar 1

    // In bar 1: its two open prompts are on the line with their ids, and bar 2's two moves (the hold
    // with its end) are not told yet.
    BeatBoss g;
    g.begin(track.def, BeatBossTuning{}, 0.0);
    g.update(2.1);
    g.beatLine(line);
    REQUIRE(line.markerCount == 4);
    int told = 0;
    for (std::size_t i = 0; i < line.markerCount; ++i) {
        const BeatLineMarker& mk = line.markers[i];
        if (mk.prompt != 0) ++told;
        if (mk.answer == BeatBossAnswer::Hold) {
            CHECK(mk.prompt == 0);
            CHECK(mk.x == doctest::Approx(2.5 / 4.0));
            CHECK(mk.endX == doctest::Approx(3.0 / 4.0));
        }
    }
    CHECK(told == 2);
    CHECK(line.playhead == doctest::Approx(0.1));

    // The 6/8 phase: bar 2's empty beat is hollow, bar 3 (7/8) is a long bar of three beats.
    BeatBoss h;
    h.begin(onlyPhase(track.def, 1), BeatBossTuning{}, 0.0);
    h.update(1.5 + 1.5 + 0.01);
    REQUIRE(h.bar() == 2);
    h.beatLine(line);
    CHECK(line.bars[0].beats == 2);
    CHECK(line.ticks[1].kind == BeatTickKind::Empty);
    CHECK(line.bars[1].beats == 3);
    CHECK(line.bars[1].longBar);
    CHECK(line.bars[1].seconds == doctest::Approx(1.75));
    CHECK(line.ticks[4].at == doctest::Approx(1.5 + 1.5)); // its third beat: one eighth
}

TEST_CASE("beat boss: judging, the lockout, the streak and the guard") {
    const BeatBossTrack track = loadTrack();
    const BeatBossTuning tuning{};

    SUBCASE("a counter's whiff locks the counters out, then the parry lands") {
        BeatBoss f;
        f.begin(track.def, tuning, 0.0);
        f.update(2.1);
        CHECK(has(f.press(ActionButton::Jump, 2.2), BeatBossEventType::Stray));
        const BeatBossEvents w = f.press(ActionButton::Guard, 2.65); // the parry's window opens at 2.7
        REQUIRE(first(w, BeatBossEventType::Whiff));
        CHECK(first(w, BeatBossEventType::Whiff)->amount == 375); // min(0.4, 0.75 beats)
        CHECK(f.counterLockedOut());
        CHECK(has(f.press(ActionButton::Guard, 2.95), BeatBossEventType::LockedOut));
        const BeatBossEvents p = f.press(ActionButton::Guard, 3.03);
        REQUIRE(first(p, BeatBossEventType::Judged));
        CHECK(first(p, BeatBossEventType::Judged)->grade == QteGrade::Perfect);
        CHECK(first(p, BeatBossEventType::BossHit)->amount == 14);
        CHECK(f.stats().whiffs == 1);
        CHECK(f.stats().ignored == 1);
        CHECK(f.stats().strays == 1);
        // A strike is Good at 0.1 s late; holds keep going through a lockout.
        CHECK(first(f.press(ActionButton::Attack, 3.6), BeatBossEventType::BossHit)->amount == 6);
        f.press(ActionButton::Attack, 4.5);                                // the hold
        CHECK(has(f.press(ActionButton::Dodge, 4.6), BeatBossEventType::Whiff)); // the dodge shows from 4.5
        const BeatBossEvents h = f.release(ActionButton::Attack, 5.0);
        REQUIRE(first(h, BeatBossEventType::Judged));
        CHECK(first(h, BeatBossEventType::Judged)->grade == QteGrade::Perfect);
        CHECK(first(h, BeatBossEventType::BossHit)->amount == 6); // one beat held
        CHECK(f.streak() == 3);
        // The dodge, after its lockout: perfect, and it deals nothing.
        const BeatBossEvents d = f.press(ActionButton::Dodge, 5.45);
        CHECK(first(d, BeatBossEventType::Judged)->grade == QteGrade::Perfect);
        CHECK_FALSE(has(d, BeatBossEventType::BossHit));
        CHECK(f.bossHp() == 400 - 14 - 6 - 6);
    }
    SUBCASE("a miss resets the streak; a missed counter costs its share, a missed strike nothing") {
        BeatBoss f;
        f.begin(track.def, tuning, 0.0);
        f.update(2.1);
        f.press(ActionButton::Guard, 3.0);
        f.press(ActionButton::Attack, 3.5);
        CHECK(f.streak() == 2);
        const BeatBossEvents m = f.update(4.7); // the hold at 4.5 goes unpressed
        REQUIRE(first(m, BeatBossEventType::Judged));
        CHECK(first(m, BeatBossEventType::Judged)->grade == QteGrade::Miss);
        CHECK_FALSE(has(m, BeatBossEventType::LeadHit));
        CHECK(f.streak() == 0);
        CHECK(f.hp() == 100);
        const BeatBossEvents d = f.update(5.6); // the dodge at 5.5 lands
        CHECK(first(d, BeatBossEventType::LeadHit)->amount == 15);
        CHECK(f.hp() == 85);
    }
    SUBCASE("a guard held through the impact; let go, the blow lands") {
        // The guard's impact is track 7.5 (clock 9.5); the lead takes 35 from the loop's misses first.
        BeatBoss f;
        f.begin(track.def, tuning, 0.0);
        f.update(9.3);
        f.press(ActionButton::Guard, 9.47);
        const BeatBossEvents g = f.update(9.5);
        CHECK(first(g, BeatBossEventType::Judged)->grade == QteGrade::Perfect);
        CHECK(first(g, BeatBossEventType::LoadStored)->amount == 20);
        CHECK(f.load() == 20);
        CHECK(f.hp() == 65);

        BeatBoss e;
        e.begin(track.def, tuning, 0.0);
        e.update(9.3);
        e.press(ActionButton::Guard, 9.3);
        e.release(ActionButton::Guard, 9.4);
        const BeatBossEvents l = e.update(9.6);
        CHECK(first(l, BeatBossEventType::Judged)->grade == QteGrade::Miss);
        CHECK(first(l, BeatBossEventType::LeadHit)->amount == 20);
        CHECK(e.load() == 0);

        // Held early: Good, no Load. With holdToggle the press alone holds it.
        QteAssist toggle;
        toggle.holdToggle = true;
        BeatBoss t;
        t.begin(track.def, tuning, 0.0, toggle);
        t.update(9.3);
        t.press(ActionButton::Guard, 9.3);
        t.release(ActionButton::Guard, 9.4);
        const BeatBossEvents k = t.update(9.6);
        CHECK(first(k, BeatBossEventType::Judged)->grade == QteGrade::Good);
        CHECK_FALSE(has(k, BeatBossEventType::LeadHit));
        CHECK(t.load() == 0);
    }
    SUBCASE("a release is offered only with Load, spends it all, and breaks if let go early") {
        for (const bool early : {false, true}) {
            BeatBoss f;
            f.begin(track.def, tuning, 0.0);
            f.update(6.1); // bar 3 of the first loop: no Load, no release
            for (std::size_t i = 0; i < f.promptCount(); ++i) CHECK(f.prompt(i).answer != BeatBossAnswer::Release);
            f.update(9.3);
            f.press(ActionButton::Guard, 9.5);
            f.update(14.1); // bar 3 of the second loop: the release's impact is track 12.5
            bool offered = false;
            for (std::size_t i = 0; i < f.promptCount(); ++i) offered |= f.prompt(i).answer == BeatBossAnswer::Release;
            CHECK(offered);
            f.press(ActionButton::Attack, 14.5);
            const BeatBossEvents r = f.release(ActionButton::Attack, early ? 15.0 : 16.0);
            if (early) {
                CHECK(first(r, BeatBossEventType::Judged)->grade == QteGrade::Miss);
                CHECK(f.load() == 20);
                CHECK_FALSE(has(r, BeatBossEventType::LeadHit));
            } else {
                CHECK(first(r, BeatBossEventType::Released)->amount == 20);
                CHECK(first(r, BeatBossEventType::BossHit)->amount == 20);
                CHECK(f.load() == 0);
                CHECK(f.stats().releases == 1);
            }
        }
    }
    SUBCASE("a paired counter needs both buttons together") {
        const BeatBossDef d = onlyPhase(track.def, 1);
        BeatBoss f; // the paired impact is track 3.0, clock 4.5
        f.begin(d, tuning, 0.0);
        f.update(4.3);
        CHECK(f.press(ActionButton::Guard, 4.48).count == 0);
        const BeatBossEvents p = f.press(ActionButton::Attack, 4.52);
        CHECK(first(p, BeatBossEventType::Judged)->grade == QteGrade::Perfect);
        CHECK(first(p, BeatBossEventType::BossHit)->amount == 14);
        CHECK(first(p, BeatBossEventType::RestOpened)->amount == 1);
        CHECK(f.openRests() == 1);

        BeatBoss g;
        g.begin(d, tuning, 0.0);
        g.update(4.2);
        g.press(ActionButton::Guard, 4.25);
        g.press(ActionButton::Attack, 4.5); // 0.25 s apart
        const BeatBossEvents l = g.update(4.6);
        CHECK(first(l, BeatBossEventType::Judged)->grade == QteGrade::Miss);
        CHECK(first(l, BeatBossEventType::LeadHit)->amount == 25);
    }
}

TEST_CASE("beat boss: a finisher's miss breaks it and the loop resumes; answered, it takes the boss down") {
    const BeatBossTrack track = loadTrack();
    BeatBossDef d = onlyPhase(track.def, 0);
    d.phases[0].finisherAt = 1.0f; // offered at the first bar line of every loop
    d.phases[0].silenceCount = 0;
    BeatBoss f;
    f.begin(d, BeatBossTuning{}, 0.0);
    Log log;
    log.add(f.update(4.01));
    CHECK(log.count(BeatBossEventType::FinisherStart) == 1);
    CHECK(f.inFinisher());
    CHECK(f.bar() == 0);
    CHECK(f.promptCount() == 3);
    log.add(f.update(4.7)); // its first strike (4.5) goes unanswered
    CHECK(log.count(BeatBossEventType::FinisherBroken) == 1);
    CHECK(f.promptCount() == 0);
    CHECK(f.hp() == 80); // the parry; the broken finisher costs nothing
    log.add(f.update(6.01));
    CHECK_FALSE(f.inFinisher());
    CHECK(f.bar() == 2);
    CHECK(f.loop() == 1);
    // Loop 2 opens with it again (bars 1, finisher, 2, 3, 4 took 10 s of track): strikes at 12.5, 13, 13.5.
    log.add(f.update(12.3));
    CHECK(f.inFinisher());
    CHECK(f.loop() == 1); // between loops: the next bar starts loop 2
    CHECK(first(f.press(ActionButton::Attack, 12.5), BeatBossEventType::BossHit)->amount == 134);
    CHECK(first(f.press(ActionButton::Attack, 13.0), BeatBossEventType::BossHit)->amount == 133);
    const BeatBossEvents last = f.press(ActionButton::Attack, 13.5);
    CHECK(first(last, BeatBossEventType::BossHit)->amount == 133);
    CHECK(has(last, BeatBossEventType::Victory));
    CHECK(f.state() == BeatBossState::Victory);
    CHECK(f.stats().finishers == 1);
    CHECK(f.update(20.0).count == 0);
}

TEST_CASE("beat boss: in a scripted silence the track stops, nothing lands, and a bar counts it back") {
    const BeatBossTrack track = loadTrack();
    BeatBoss f;
    f.begin(track.def, BeatBossTuning{}, 0.0);
    Log log;
    double now = 0.0;
    const BeatBossEvent* start = nullptr;
    while (!f.over() && now < 120.0 && !start) {
        now += 1.0 / 60.0;
        pilotFrame(f, now, log);
        start = log.find(BeatBossEventType::SilenceStart);
    }
    REQUIRE(start);
    CHECK(start->amount == 2000);
    CHECK(f.bossHp() <= 320); // at 80% of 400
    CHECK(f.silent());
    CHECK_FALSE(f.trackRunning());
    const std::size_t from = log.events.size();
    const double silenceAt = start->time;
    const double trackAt = f.trackTime();
    const int bossHp = f.bossHp();
    const int hp = f.hp();
    const std::size_t prompts = f.promptCount();

    BeatLine line;
    f.beatLine(line);
    CHECK(line.frozen);
    CHECK(line.counting);
    CHECK(line.playhead == 0.0);
    CHECK(line.bars[0].count);
    CHECK(line.bars[1].number == f.bar());

    // Presses in the silence answer nothing.
    log.add(f.press(ActionButton::Guard, now + 0.5));
    log.add(f.press(ActionButton::Attack, now + 0.6));
    now += 0.6;
    CHECK(log.count(BeatBossEventType::Stray) >= 2);

    const BeatBossEvent* resumed = nullptr;
    while (!resumed && now < silenceAt + 10.0) {
        now += 1.0 / 60.0;
        pilotFrame(f, now, log);
        for (std::size_t i = from; i < log.events.size() && !resumed; ++i) {
            const BeatBossEvent& e = log.events[i];
            if (e.type == BeatBossEventType::Resumed) {
                resumed = &e;
                break;
            }
            CHECK(e.type != BeatBossEventType::Judged);
            CHECK(e.type != BeatBossEventType::BossHit);
            CHECK(e.type != BeatBossEventType::LeadHit);
        }
        if (!resumed) {
            CHECK(f.trackTime() == trackAt);
            CHECK(f.bossHp() == bossHp);
            CHECK(f.hp() == hp);
            CHECK(f.promptCount() == prompts);
        }
    }
    REQUIRE(resumed);
    CHECK(resumed->time - silenceAt == doctest::Approx(2.0 + 2.0)); // the silence, then one 4/4 bar
    std::vector<int> counts;
    for (std::size_t i = from; i < log.events.size(); ++i)
        if (log.events[i].type == BeatBossEventType::Count) counts.push_back(log.events[i].amount);
    CHECK(counts == std::vector<int>{1, 2, 3, 4});

    // It plays on to the end.
    play(f, now, 1.0 / 60.0, 200.0, log, true);
    CHECK(f.state() == BeatBossState::Victory);
}

TEST_CASE("beat boss: one miss costs at most 35%, and doing nothing loses") {
    const BeatBossTrack track = loadTrack();
    BeatBoss f;
    f.begin(track.def, BeatBossTuning{}, 0.0);
    Log log;
    play(f, 0.0, 1.0 / 60.0, 120.0, log, false);
    CHECK(f.state() == BeatBossState::Defeat);
    CHECK(f.stats().damageDealt == 0);
    CHECK(f.bossHp() == 400);
    int hits = 0;
    for (const BeatBossEvent& e : log.events) {
        if (e.type != BeatBossEventType::LeadHit) continue;
        ++hits;
        CHECK(e.amount <= 35);
    }
    CHECK(hits >= 3); // no single miss ends it
    CHECK(f.stats().bars <= 12);

    // The cap itself: a counter of 35% costs 35.
    std::string json = R"({"version": 1, "id": "cap", "boss": {"hp": 100}, "lead": {"hp": 100}, "stems": [{"id": "s"}],
      "phases": [{"id": "p", "bpm": 120, "meter": "4/4", "loop": {"bars": 1, "seconds": 2},
        "moves": [{"id": "m", "tell": "1.1", "impact": "1.3", "answer": "dodge", "stem": "s", "miss": 0.35}]}]})";
    BeatBossTrack cap;
    std::string error;
    REQUIRE_MESSAGE(importBeatBossTrack(json, cap, &error), error);
    BeatBoss g;
    g.begin(cap.def, BeatBossTuning{}, 0.0);
    const BeatBossEvents l = g.update(3.1);
    CHECK(first(l, BeatBossEventType::LeadHit)->amount == 35);
    CHECK(g.hp() == 65);
}

TEST_CASE("beat boss: the autopilot wins the test track within the bar budget, at any frame rate") {
    const BeatBossTrack track = loadTrack();
    for (const double dt : {1.0 / 120.0, 1.0 / 60.0, 0.13}) {
        CAPTURE(dt);
        BeatBoss f;
        f.begin(track.def, BeatBossTuning{}, 0.0);
        Log log;
        play(f, 0.0, dt, 300.0, log, true);
        CHECK(f.state() == BeatBossState::Victory);
        CHECK(f.stats().bars <= kWinBars);
        CHECK(f.stats().misses == 0);
        CHECK(f.stats().strays == 0);
        CHECK(f.stats().damageTaken == 0);
        CHECK(f.stats().silences == 2);
        CHECK(f.stats().finishers >= 1);
        CHECK(f.stats().releases >= 1);
        CHECK(f.stats().loadStored > 0);
        CHECK(f.stats().maxStreak == f.stats().perfects + f.stats().goods); // one unbroken streak
        CHECK(log.count(BeatBossEventType::PhaseStart) == 2);
        CHECK(log.find(BeatBossEventType::SilenceStart));
        int maxMult = 0;
        for (const BeatBossEvent& e : log.events)
            if (e.type == BeatBossEventType::Multiplier) maxMult = std::max(maxMult, e.amount);
        CHECK(maxMult == 4);
        CHECK(f.multiplier() <= kMaxBossMultiplier);
    }
    // With the auto-complete assist, update() alone plays it the same way.
    QteAssist autoPlay;
    autoPlay.autoComplete = true;
    BeatBoss a;
    a.begin(track.def, BeatBossTuning{}, 0.0, autoPlay);
    Log log;
    play(a, 0.0, 1.0 / 60.0, 300.0, log, false);
    CHECK(a.state() == BeatBossState::Victory);
    CHECK(a.stats().misses == 0);
    CHECK(a.stats().bars <= kWinBars);
}

TEST_CASE("beat boss: the auto-complete assist wins at uneven frame times too, never stalling on a press it can't land") {
    // A host's frames are never even: each frame here is dt stretched by up to +-jitter, from a fixed
    // sequence, so a frame can land anywhere in an answer's window, or just past it.
    const BeatBossTrack track = loadTrack();
    int runs = 0;
    for (const bool toggle : {false, true})
    for (const double start : {0.0, 1.733})
    for (const double dt : {0.0004, 0.002, 1.0 / 30.0, 1.0 / 60.0, 1.0 / 90.0, 1.0 / 144.0, 0.05, 0.13}) {
        for (const double jitter : {0.0, 0.3, 0.6}) {
            for (unsigned seed = 1; seed <= 12; ++seed) {
                CAPTURE(dt);
                CAPTURE(jitter);
                CAPTURE(seed);
                CAPTURE(toggle);
                CAPTURE(start);
                QteAssist autoPlay;
                autoPlay.autoComplete = true;
                autoPlay.holdToggle = toggle;
                BeatBoss f;
                f.begin(track.def, BeatBossTuning{}, start, autoPlay);
                unsigned s = seed * 2654435761u;
                double now = start;
                double lastProgress = start;
                double lastNow = f.now();
                bool stalled = false;
                while (!f.over() && now < start + 300.0) {
                    s = s * 1664525u + 1013904223u;
                    const double u = static_cast<double>(s >> 8) / static_cast<double>(1u << 24); // [0, 1)
                    now += dt * (1.0 + jitter * (2.0 * u - 1.0));
                    (void)f.update(now);
                    if (f.now() > lastNow) {
                        lastNow = f.now();
                        lastProgress = now;
                    }
                    if (now - lastProgress > 1.0) { // the fight's clock stood still for a second
                        stalled = true;
                        break;
                    }
                }
                ++runs;
                REQUIRE_FALSE(stalled);
                CHECK(f.state() == BeatBossState::Victory);
                CHECK(f.stats().misses == 0);
                CHECK(f.stats().bars <= kWinBars);
            }
        }
    }
    CHECK(runs == 2 * 2 * 8 * 3 * 12);
}

TEST_CASE("beat boss: the auto-complete assist rides out a long frame anywhere in the fight") {
    // A hitch (a load, a shader compile) hands update() one long frame; the fight must carry on after it.
    const BeatBossTrack track = loadTrack();
    QteAssist autoPlay;
    autoPlay.autoComplete = true;
    for (const double hitch : {0.25, 0.6, 1.0, 2.5}) {
        for (double at = 0.5; at < 40.0; at += 0.173) {
            CAPTURE(hitch);
            CAPTURE(at);
            BeatBoss f;
            f.begin(track.def, BeatBossTuning{}, 0.0, autoPlay);
            double now = 0.0;
            bool hitched = false;
            while (!f.over() && now < 300.0) {
                if (!hitched && now >= at) {
                    now += hitch;
                    hitched = true;
                } else {
                    now += 1.0 / 60.0;
                }
                (void)f.update(now);
                if (!f.over()) REQUIRE(f.now() == doctest::Approx(now));
            }
            CHECK(f.state() == BeatBossState::Victory);
        }
    }
}

TEST_CASE("beat boss: the auto-complete assist crosses a bar line however the fight's start rounds") {
    // Recorded from a host: the fight began at an uneven wall time, and a frame landed just past the
    // first phase's second bar line (begin + 4 s). The bar line as horizon() put it in wall time and as
    // advance() reached it in track time differed in the last bit, so the fight stood on the line for good.
    const BeatBossTrack track = loadTrack();
    QteAssist autoPlay;
    autoPlay.autoComplete = true;
    const double begin = 0.65682400241494177;
    for (const double last : {4.6560614556074142, 4.65682400241494177, 4.6567, 4.65681})
    {
        CAPTURE(last);
        BeatBoss f;
        f.begin(track.def, BeatBossTuning{}, begin, autoPlay);
        double now = begin;
        while (now + 0.0004 < last) {
            now += 0.0004;
            (void)f.update(now);
        }
        (void)f.update(last);
        now = 4.6569608561694622;
        (void)f.update(now);
        REQUIRE(f.now() == doctest::Approx(now).epsilon(1e-12));
        while (!f.over() && now < begin + 300.0) {
            now += 0.0004;
            (void)f.update(now);
        }
        CHECK(f.state() == BeatBossState::Victory);
    }
    // Starts spread over a second, at the host's frame rate: every one plays through.
    for (int i = 0; i < 40; ++i) {
        const double start = 0.6568240024149417 + 0.0253 * i;
        CAPTURE(start);
        BeatBoss f;
        f.begin(track.def, BeatBossTuning{}, start, autoPlay);
        double now = start;
        bool stalled = false;
        double lastNow = f.now();
        double lastProgress = now;
        while (!f.over() && now < start + 300.0) {
            now += 0.00045;
            (void)f.update(now);
            if (f.now() > lastNow) {
                lastNow = f.now();
                lastProgress = now;
            } else if (now - lastProgress > 0.5) {
                stalled = true;
                break;
            }
        }
        REQUIRE_FALSE(stalled);
        CHECK(f.state() == BeatBossState::Victory);
    }
}

TEST_CASE("beat boss: rests open on answers, enough at once stagger the boss and the next bar has no counters") {
    const BeatBossTrack track = loadTrack();
    BeatBossDef d = onlyPhase(track.def, 1);
    d.staggerOpens = 2;
    BeatBoss f;
    f.begin(d, BeatBossTuning{}, 0.0);
    Log log;
    double now = 0.0;
    std::uint32_t lastId = 0;
    while (!f.over() && now < 60.0 && log.count(BeatBossEventType::Stagger) == 0) {
        now += 1.0 / 60.0;
        pilotFrame(f, now, log);
        for (std::size_t i = 0; i < f.promptCount(); ++i) lastId = std::max(lastId, f.prompt(i).id);
    }
    REQUIRE(log.count(BeatBossEventType::Stagger) == 1);
    CHECK(f.openRests() == 0);
    for (std::size_t i = 0; i < f.promptCount(); ++i) CHECK_FALSE(isBossCounter(f.prompt(i).answer));
    const int bars = f.stats().bars;
    while (f.stats().bars == bars && now < 80.0) {
        now += 1.0 / 60.0;
        pilotFrame(f, now, log);
    }
    // The bar has a counter in the map, and none of it was told.
    REQUIRE(f.bar() > 0);
    int mapCounters = 0;
    for (std::size_t i = 0; i < d.phases[0].moveCount; ++i) {
        const PinnedMove& mv = d.phases[0].moves[i];
        if (mv.tell.bar == f.bar() && isBossCounter(mv.answer)) ++mapCounters;
    }
    CHECK(mapCounters > 0);
    for (std::size_t i = 0; i < f.promptCount(); ++i)
        if (f.prompt(i).id > lastId) CHECK_FALSE(isBossCounter(f.prompt(i).answer));
    CHECK(f.stats().staggers == 1);

    // A miss closes the last rest opened.
    BeatBoss g;
    g.begin(d, BeatBossTuning{}, 0.0);
    g.update(4.3);
    g.press(ActionButton::Guard, 4.5);
    g.press(ActionButton::Attack, 4.5);
    REQUIRE(g.openRests() == 1);
    const BeatBossEvents m = g.update(6.2); // the strike at track 4.5 (clock 6.0) passes
    CHECK(first(m, BeatBossEventType::RestClosed)->amount == 0);
    CHECK(g.openRests() == 0);
}

TEST_CASE("beat boss: retry restarts at the phase's checkpoint") {
    const BeatBossTrack track = loadTrack();
    BeatBoss f;
    f.begin(track.def, BeatBossTuning{}, 0.0);
    Log log;
    double now = 0.0;
    while (!f.over() && now < 200.0 && f.phase() == 0) {
        now += 1.0 / 60.0;
        pilotFrame(f, now, log);
    }
    REQUIRE(f.phase() == 1);
    CHECK(f.checkpointPhase() == 1);
    const int bossAtCheckpoint = f.bossHp();
    const int loadAtCheckpoint = f.load();
    CHECK(bossAtCheckpoint == 240); // phase one's floor, 60% of 400
    // Some damage, then nothing until the lead falls.
    now = play(f, now, 1.0 / 60.0, now + 4.0, log, true);
    CHECK(f.bossHp() < bossAtCheckpoint);
    now = play(f, now, 1.0 / 60.0, now + 120.0, log, false);
    REQUIRE(f.state() == BeatBossState::Defeat);

    f.retry(now);
    CHECK(f.state() == BeatBossState::Playing);
    CHECK(f.phase() == 1);
    CHECK(f.bossHp() == bossAtCheckpoint);
    CHECK(f.load() == loadAtCheckpoint);
    CHECK(f.hp() == f.maxHp());
    CHECK(f.streak() == 0);
    CHECK(f.openRests() == 0);
    CHECK(f.promptCount() == 0);
    CHECK(f.counting());
    CHECK(f.stats().retries == 1);
    // One bar of 6/8 counts back (two dotted beats), then the phase's bar 1.
    Log back;
    const double at = now;
    while (back.count(BeatBossEventType::Resumed) == 0 && now < at + 5.0) {
        now += 1.0 / 60.0;
        back.add(f.update(now));
    }
    CHECK(back.count(BeatBossEventType::Count) == 2);
    CHECK(back.find(BeatBossEventType::Resumed)->time - at == doctest::Approx(1.5));
    REQUIRE(back.find(BeatBossEventType::PhaseStart));
    CHECK(back.find(BeatBossEventType::PhaseStart)->amount == 1);
    CHECK(f.bar() == 1);
    CHECK(f.loop() == 1);
    play(f, now, 1.0 / 60.0, now + 200.0, log, true);
    CHECK(f.state() == BeatBossState::Victory);

    // A retry in the first phase starts the whole fight again.
    BeatBoss g;
    g.begin(track.def, BeatBossTuning{}, 0.0);
    Log idle;
    const double end = play(g, 0.0, 1.0 / 60.0, 120.0, idle, false);
    REQUIRE(g.state() == BeatBossState::Defeat);
    g.retry(end);
    CHECK(g.phase() == 0);
    CHECK(g.bossHp() == 400);
    CHECK(g.hp() == 100);
}

TEST_CASE("beat boss: the same inputs give the same fight") {
    const BeatBossTrack track = loadTrack();
    BeatBoss a;
    BeatBoss b;
    a.begin(track.def, BeatBossTuning{}, 0.0);
    b.begin(track.def, BeatBossTuning{}, 0.0);
    Log la;
    Log lb;
    play(a, 0.0, 1.0 / 60.0, 300.0, la, true);
    play(b, 0.0, 1.0 / 60.0, 300.0, lb, true);
    REQUIRE(la.events.size() == lb.events.size());
    for (std::size_t i = 0; i < la.events.size(); ++i) {
        CHECK(la.events[i].type == lb.events[i].type);
        CHECK(la.events[i].amount == lb.events[i].amount);
        CHECK(la.events[i].time == lb.events[i].time);
    }
    CHECK(a.stats().bars == b.stats().bars);
}

TEST_CASE("beat boss: update, press, release, autopilot, beatLine and retry allocate nothing over a whole fight") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    // Everything that allocates (the track) is made first.
    const BeatBossTrack track = loadTrack();
    const BeatBossTuning tuning{};
    QteAssist autoPlay;
    autoPlay.autoComplete = true;
    BeatBoss won;
    BeatBoss lost;
    BeatBoss assisted;
    BeatLine line;

    int sink = 0;
    const std::uint64_t before = ftcore_test::allocStats().count;
    // A won fight: the autopilot, with a stray press and release thrown in each frame, and the line.
    won.begin(track.def, tuning, 0.0);
    for (double now = 1.0 / 60.0; !won.over() && now < 300.0; now += 1.0 / 60.0) {
        for (int guard = 0; guard < 16; ++guard) {
            const QteAction a = won.autopilot(now);
            if (a.kind == QteAction::Kind::None) break;
            sink += static_cast<int>(a.kind == QteAction::Kind::Press ? won.press(a.input, a.time).size()
                                                                       : won.release(a.input, a.time).size());
        }
        sink += static_cast<int>(won.update(now).size());
        sink += static_cast<int>(won.release(ActionButton::Jump, now).size());
        won.beatLine(line);
        sink += static_cast<int>(line.tickCount);
    }
    // A lost fight, retried, and lost again.
    lost.begin(track.def, tuning, 0.0);
    double now = 0.0;
    for (int round = 0; round < 2; ++round) {
        for (; !lost.over() && now < 400.0; now += 1.0 / 60.0) {
            sink += static_cast<int>(lost.update(now).size());
            sink += static_cast<int>(lost.press(ActionButton::Dodge, now).size());
            lost.beatLine(line);
        }
        lost.retry(now);
    }
    // The assist.
    assisted.begin(track.def, tuning, 0.0, autoPlay);
    for (double t = 0.0; !assisted.over() && t < 300.0; t += 1.0 / 30.0) sink += static_cast<int>(assisted.update(t).size());
    const std::uint64_t after = ftcore_test::allocStats().count;
    CHECK(after == before);
    CHECK(won.state() == BeatBossState::Victory);
    CHECK(assisted.state() == BeatBossState::Victory);
    CHECK(lost.stats().retries == 2);
    CHECK(sink > 0);
}

TEST_CASE("beat boss: answers with action buttons only (no arrow combo is ever built)") {
    const BeatBossTrack track = loadTrack();
    const std::uint64_t before = comboSequencesBuilt();
    BeatBoss f;
    f.begin(track.def, BeatBossTuning{}, 0.0);
    Log log;
    play(f, 0.0, 1.0 / 60.0, 300.0, log, true);
    CHECK(f.state() == BeatBossState::Victory);
    CHECK(comboSequencesBuilt() == before);
}
