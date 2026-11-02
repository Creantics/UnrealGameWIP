#include "ftcore/BeatAccess.h"
#include "ftcore/BeatBoss.h"

#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace ftcore;

// The cinematic beat boss's accessibility options (BeatAccess.h): the options and their file, the tuning they
// make, what the windows, the pair modes and the lockout do to the judging, the change that lands at a bar line,
// and the practice tempo. The timings are the test track's (Data/Battle/beat_boss/test_track.json): its first
// phase is 120 bpm 4/4 (a beat 0.5 s) and a fight counts one bar (2 s) in, so track time t is clock 2 + t until
// something stops the track; its second phase is 6/8 at 80 (counted 1.5 s in).

namespace {

BeatBossTrack loadTrack(const char* name) {
    std::string text;
    BeatBossTrack track;
    if (!ftcore_test::readProjectData(std::string("Battle/beat_boss/") + name + ".json", text)) {
        FAIL(name << " not found");
        return track;
    }
    std::string error;
    REQUIRE_MESSAGE(importBeatBossTrack(text, track, &error), error);
    return track;
}

BeatBossDef onlyPhase(const BeatBossDef& def, int phase) {
    BeatBossDef d = def;
    d.phases[0] = def.phases[static_cast<std::size_t>(phase)];
    d.phases[0].endAt = 0.0f;
    d.phaseCount = 1;
    return d;
}

const BeatBossEvent* first(const BeatBossEvents& e, BeatBossEventType t) {
    for (const BeatBossEvent& x : e)
        if (x.type == t) return &x;
    return nullptr;
}

BeatAccessOptions scaled(float windowScale) {
    BeatAccessOptions o;
    o.windowScale = windowScale;
    return o;
}

// The autopilot plays the def at 60 frames a second; true when it wins without a miss.
bool autopilotWins(const BeatBossDef& def, const BeatBossTuning& tuning, double* seconds = nullptr, int* bars = nullptr) {
    BeatBoss f;
    f.begin(def, tuning, 0.0);
    double now = 0.0;
    for (; !f.over() && now < 400.0; now += 1.0 / 60.0) {
        for (int guard = 0; guard < 64; ++guard) {
            const QteAction a = f.autopilot(now);
            if (a.kind == QteAction::Kind::None) break;
            if (a.kind == QteAction::Kind::Press) f.press(a.input, a.time);
            else f.release(a.input, a.time);
        }
        f.update(now);
    }
    if (seconds) *seconds = now;
    if (bars) *bars = f.stats().bars;
    return f.state() == BeatBossState::Victory && f.stats().misses == 0;
}

} // namespace

TEST_CASE("beat access: the defaults are today's behaviour, values are clamped and the file round-trips") {
    const BeatAccessOptions d;
    CHECK(d.windowScale == 1.0f);
    CHECK(d.whiffLockoutMs == 400.0f);
    CHECK(d.pairMode == 0);
    CHECK(d.uiScale == 1.0f);
    CHECK(d.cutsReduced == 0);
    CHECK(d.shakeScale == 1.0f);
    CHECK(d.flashLevel == 0);
    CHECK(d.tempoScale == 1.0f);

    // Default options change nothing.
    const BeatBossTuning base;
    const BeatBossTuning same = beatAccessTuning(base, d);
    CHECK(same.strikePerfect == base.strikePerfect);
    CHECK(same.strikeGood == base.strikeGood);
    CHECK(same.perfect == base.perfect);
    CHECK(same.counterEarly == base.counterEarly);
    CHECK(same.lateLimit() == base.lateLimit());
    CHECK(same.pairWidth == base.pairWidth);
    CHECK(same.counterLockout == base.counterLockout);
    CHECK(same.holdSlack == base.holdSlack);
    CHECK(same.pairMode == 0);

    BeatAccessOptions wild;
    wild.windowScale = 9.0f;
    wild.whiffLockoutMs = -5.0f;
    wild.pairMode = 7;
    wild.uiScale = 0.2f;
    wild.cutsReduced = -1;
    wild.shakeScale = 3.0f;
    wild.flashLevel = 99;
    wild.tempoScale = 0.1f;
    clampBeatAccess(wild);
    CHECK(wild.windowScale == 2.0f);
    CHECK(wild.whiffLockoutMs == 0.0f);
    CHECK(wild.pairMode == 2);
    CHECK(wild.uiScale == 1.0f);
    CHECK(wild.cutsReduced == 0);
    CHECK(wild.shakeScale == 1.0f);
    CHECK(wild.flashLevel == 2);
    CHECK(wild.tempoScale == 0.5f);

    BeatAccessOptions odd;
    odd.windowScale = std::numeric_limits<float>::quiet_NaN();
    odd.uiScale = std::numeric_limits<float>::quiet_NaN();
    clampBeatAccess(odd);
    CHECK(odd.windowScale == 1.0f);
    CHECK(odd.uiScale == 1.0f);
    BeatAccessOptions steps = scaled(1.4f);
    clampBeatAccess(steps);
    CHECK(steps.windowScale == 1.5f); // quarter steps

    BeatAccessOptions o;
    o.windowScale = 1.5f;
    o.whiffLockoutMs = 100.0f;
    o.pairMode = 2;
    o.uiScale = 1.75f;
    o.cutsReduced = 1;
    o.shakeScale = 0.5f;
    o.flashLevel = 2;
    o.tempoScale = 0.75f;
    BeatAccessOptions back;
    std::string error;
    REQUIRE_MESSAGE(decodeBeatAccess(encodeBeatAccess(o), back, &error), error);
    CHECK(back == o);

    BeatAccessOptions partial = o;
    REQUIRE(decodeBeatAccess(R"({"beat.ui_scale": 2.0, "beat.pair_mode": "two", "other": 1})", partial));
    CHECK(partial.uiScale == 2.0f);
    CHECK(partial.pairMode == 0); // a wrong type keeps the default, not the earlier value
    CHECK(partial.windowScale == 1.0f);
    BeatAccessOptions untouched = o;
    CHECK_FALSE(decodeBeatAccess("[1, 2]", untouched, &error));
    CHECK_FALSE(decodeBeatAccess("{not json", untouched, &error));
    CHECK(untouched == o);
}

TEST_CASE("beat access: the windows widen fully and Perfect at half the rate, capped at one and a half") {
    const BeatBossTuning base;
    CHECK(beatPerfectScale(1.0) == doctest::Approx(1.0));
    CHECK(beatPerfectScale(1.5) == doctest::Approx(1.25));
    CHECK(beatPerfectScale(2.0) == doctest::Approx(1.5));
    CHECK(beatPerfectScale(3.0) == doctest::Approx(1.5)); // capped

    const BeatBossTuning t = beatAccessTuning(base, scaled(2.0f));
    CHECK(t.strikeGood == doctest::Approx(0.24));
    CHECK(t.strikePerfect == doctest::Approx(0.075));
    CHECK(t.counterEarly == doctest::Approx(0.60));
    CHECK(t.lateLimit() == doctest::Approx(0.12));
    CHECK(t.perfect == doctest::Approx(0.18));
    CHECK(t.pairWidth == doctest::Approx(0.24));
    CHECK(t.holdSlack == doctest::Approx(base.holdSlack * 2.0));
    // Perfect never grows past Good.
    CHECK(t.strikePerfect < t.strikeGood);
    CHECK(t.perfect * 0.5 < t.lateLimit() + 1e-12);
}

TEST_CASE("beat access: a strike's windows follow the scale, and the result carries no mark of it") {
    const BeatBossTrack track = loadTrack("test_track");
    const BeatBossDef d = onlyPhase(track.def, 0);
    // a_strike's impact is track 1.5, clock 3.5. A press 0.2 s late misses; a press 0.07 s late is Good, and at
    // twice the windows it is Perfect.
    const auto press = [&](float scale, double late) {
        BeatBoss f;
        f.begin(d, beatAccessTuning({}, scaled(scale)), 0.0);
        f.update(3.4);
        const BeatBossEvents e = f.press(ActionButton::Attack, 3.5 + late);
        const BeatBossEvents rest = f.update(3.9);
        const BeatBossEvent* j = first(e, BeatBossEventType::Judged);
        if (!j) j = first(rest, BeatBossEventType::Judged); // a Miss comes when the window runs out
        return j ? j->grade : QteGrade::Pending;
    };
    CHECK(press(1.0f, 0.07) == QteGrade::Good);
    CHECK(press(2.0f, 0.07) == QteGrade::Perfect);
    CHECK(press(1.0f, 0.20) == QteGrade::Miss);
    CHECK(press(2.0f, 0.20) == QteGrade::Good);
    CHECK(press(2.0f, 0.30) == QteGrade::Miss);
    // The stats are the same kind of stats: there is no assisted flag to read.
    BeatBoss f;
    f.begin(d, beatAccessTuning({}, scaled(2.0f)), 0.0);
    CHECK(f.stats().perfects == 0);
}

TEST_CASE("beat access: the late limit and the lockout") {
    const BeatBossTrack track = loadTrack("test_track");
    // The parry's impact is track 1.0, clock 3.0; its window opens counterEarly before.
    const auto whiff = [&](float scale, double whiffLockoutMs) {
        BeatAccessOptions o = scaled(scale);
        o.whiffLockoutMs = static_cast<float>(whiffLockoutMs);
        BeatBoss f;
        f.begin(track.def, beatAccessTuning({}, o), 0.0);
        f.update(2.1);
        const double opens = 3.0 - beatAccessTuning({}, o).counterEarly;
        const BeatBossEvents w = f.press(ActionButton::Guard, opens - 0.05);
        const BeatBossEvent* e = first(w, BeatBossEventType::Whiff);
        return e ? e->amount : -1;
    };
    CHECK(whiff(1.0f, 400.0) == 375);  // min(0.4, 0.75 beats)
    CHECK(whiff(2.0f, 400.0) == 200);  // 0.4 / 2
    CHECK(whiff(1.0f, 100.0) == 100);
    CHECK(whiff(1.0f, 0.0) == 0);
    CHECK(whiff(2.0f, 100.0) == 50);

    // A zero lockout locks nothing out: the next press is judged.
    BeatAccessOptions o;
    o.whiffLockoutMs = 0.0f;
    BeatBoss f;
    f.begin(track.def, beatAccessTuning({}, o), 0.0);
    f.update(2.1);
    f.press(ActionButton::Guard, 2.65);
    CHECK_FALSE(f.counterLockedOut());
    const BeatBossEvents p = f.press(ActionButton::Guard, 2.8);
    REQUIRE(first(p, BeatBossEventType::Judged));
    CHECK(first(p, BeatBossEventType::Judged)->grade == QteGrade::Good);

    // An unanswered counter lands at impact + the late limit: wider windows hold it back longer.
    for (const float scale : {1.0f, 2.0f}) {
        BeatBoss g;
        g.begin(track.def, beatAccessTuning({}, scaled(scale)), 0.0);
        g.update(3.0 + 0.07);
        const bool landed = g.stats().misses > 0;
        CHECK(landed == (scale == 1.0f));
    }
}

TEST_CASE("beat access: pair mode 1 takes the buttons one after the other, 2 takes one press") {
    const BeatBossTrack track = loadTrack("test_track");
    const BeatBossDef d = onlyPhase(track.def, 1);
    // The paired impact is track 3.0, clock 4.5: Guard with Attack.
    const auto play = [&](int mode, double guardAt, double attackAt, bool both) {
        BeatAccessOptions o;
        o.pairMode = mode;
        BeatBoss f;
        f.begin(d, beatAccessTuning({}, o), 0.0);
        f.update(4.1);
        BeatBossEvents all = f.press(ActionButton::Guard, guardAt);
        if (both) {
            const BeatBossEvents b = f.press(ActionButton::Attack, attackAt);
            if (const BeatBossEvent* e = first(b, BeatBossEventType::Judged)) return e->grade;
        }
        if (const BeatBossEvent* e = first(all, BeatBossEventType::Judged)) return e->grade;
        const BeatBossEvents rest = f.update(4.9);
        const BeatBossEvent* e = first(rest, BeatBossEventType::Judged);
        return e ? e->grade : QteGrade::Pending;
    };
    // 0.15 s apart is too far for mode 0, fine for mode 1.
    CHECK(play(0, 4.35, 4.50, true) == QteGrade::Miss);
    CHECK(play(1, 4.35, 4.50, true) == QteGrade::Perfect);
    CHECK(play(1, 4.22, 4.48, true) == QteGrade::Perfect); // 0.26 s apart: order and time free inside the window
    // One press: mode 0 and 1 wait for the other and the move lands; mode 2 counts it as both.
    CHECK(play(0, 4.49, 0.0, false) == QteGrade::Miss);
    CHECK(play(1, 4.49, 0.0, false) == QteGrade::Miss);
    CHECK(play(2, 4.49, 0.0, false) == QteGrade::Perfect);
    // Mode 2 accepts either button.
    BeatAccessOptions o;
    o.pairMode = 2;
    BeatBoss f;
    f.begin(d, beatAccessTuning({}, o), 0.0);
    f.update(4.1);
    CHECK(first(f.press(ActionButton::Attack, 4.5), BeatBossEventType::Judged)->grade == QteGrade::Perfect);
    // The autopilot wins every mode.
    for (int mode = 0; mode <= 2; ++mode) {
        BeatAccessOptions m;
        m.pairMode = mode;
        INFO("pair mode " << mode);
        CHECK(autopilotWins(d, beatAccessTuning({}, m)));
    }
}

TEST_CASE("beat access: an assist change lands at the next bar line and on a retry") {
    const BeatBossTrack track = loadTrack("test_track");
    const BeatBossDef d = onlyPhase(track.def, 0);
    const BeatBossTuning wide = beatAccessTuning({}, scaled(2.0f));

    SUBCASE("asked during the count, it lands at the first bar line; asked in a bar, at the next") {
        BeatBoss f;
        f.begin(d, BeatBossTuning{}, 0.0);
        f.update(1.0);
        f.requestAccess(wide, QteAssist{});
        CHECK(f.accessPending());
        CHECK(f.tuning().strikeGood == doctest::Approx(0.12));
        f.update(1.99);
        CHECK(f.accessPending());
        f.update(2.01);
        CHECK_FALSE(f.accessPending());
        CHECK(f.tuning().strikeGood == doctest::Approx(0.24));

        // Bar 1 is 2 s long (clock 2..4): a request at 2.5 waits for 4.
        f.requestAccess(BeatBossTuning{}, QteAssist{});
        f.update(3.9);
        CHECK(f.accessPending());
        CHECK(f.tuning().strikeGood == doctest::Approx(0.24));
        f.update(4.01);
        CHECK_FALSE(f.accessPending());
        CHECK(f.tuning().strikeGood == doctest::Approx(0.12));
    }
    SUBCASE("a later request replaces an earlier one that has not landed") {
        BeatBoss f;
        f.begin(d, BeatBossTuning{}, 0.0);
        f.update(2.5);
        f.requestAccess(wide, QteAssist{});
        f.requestAccess(beatAccessTuning({}, scaled(1.5f)), QteAssist{});
        f.update(4.01);
        CHECK(f.tuning().strikeGood == doctest::Approx(0.18));
    }
    SUBCASE("auto-complete switched on mid-fight takes over at the bar line, and off again gives control back") {
        BeatBoss f;
        f.begin(d, BeatBossTuning{}, 0.0);
        f.update(2.5);
        QteAssist on;
        on.autoComplete = true;
        f.requestAccess(BeatBossTuning{}, on);
        CHECK_FALSE(f.assist().autoComplete);
        // Until the bar line a press is the player's.
        CHECK(f.press(ActionButton::Jump, 2.6).count > 0);
        f.update(4.01);
        CHECK(f.assist().autoComplete);
        // The autopilot now answers: the next bar's moves are met on time.
        double now = 4.01;
        for (; now < 8.0 && !f.over(); now += 1.0 / 60.0) {
            for (int guard = 0; guard < 64; ++guard) {
                const QteAction a = f.autopilot(now);
                if (a.kind == QteAction::Kind::None) break;
                if (a.kind == QteAction::Kind::Press) f.press(a.input, a.time);
                else f.release(a.input, a.time);
            }
            f.update(now);
        }
        QteAssist off;
        f.requestAccess(BeatBossTuning{}, off);
        f.update(now + 4.0);
        CHECK_FALSE(f.assist().autoComplete);
    }
    SUBCASE("a retry applies it at once") {
        BeatBoss f;
        f.begin(d, BeatBossTuning{}, 0.0);
        double now = 0.0;
        for (; !f.over() && now < 60.0; now += 1.0 / 60.0) f.update(now); // nobody plays: a defeat
        REQUIRE(f.state() == BeatBossState::Defeat);
        f.requestAccess(wide, QteAssist{});
        f.retry(now);
        CHECK_FALSE(f.accessPending());
        CHECK(f.tuning().strikeGood == doctest::Approx(0.24));
    }
}

TEST_CASE("beat access: the practice tempo rescales a track and keeps every rule the loader checks") {
    const BeatBossTrack cine = loadTrack("cine_track");
    const BeatBossTrack test = loadTrack("test_track");
    // The slowest a track can go is set by its slowest phase: 96 bpm cannot go under 40/96.
    CHECK(minBossTempoScale(cine.def) == doctest::Approx(40.0 / 96.0).epsilon(0.001));
    CHECK(minBossTempoScale(test.def) == doctest::Approx(0.5).epsilon(0.001)); // 80 bpm
    CHECK(minBossTempoScale(cine.def) <= 0.5);

    SUBCASE("a scale of 1 or more changes nothing") {
        const BeatBossDef same = rescaleBeatBossDef(cine.def, 1.0);
        CHECK(same.phases[0].bpm == cine.def.phases[0].bpm);
        CHECK(same.phases[1].moves[0].impactAt == cine.def.phases[1].moves[0].impactAt);
        const BeatBossDef more = rescaleBeatBossDef(cine.def, 3.0);
        CHECK(more.phases[2].loopSeconds == cine.def.phases[2].loopSeconds);
    }
    SUBCASE("every time stretches with the tempo, and the bars still sum to the loop") {
        for (const double scale : {0.75, 0.5}) {
            const BeatBossDef slow = rescaleBeatBossDef(cine.def, scale);
            for (std::size_t i = 0; i < cine.def.phaseCount; ++i) {
                const BarMap& a = cine.def.phases[i];
                const BarMap& b = slow.phases[i];
                INFO("scale " << scale << " phase " << i);
                CHECK(b.bpm == doctest::Approx(a.bpm * scale));
                CHECK(b.bpm >= kMinBossBpm - 1e-6);
                CHECK(b.loopSeconds == doctest::Approx(a.loopSeconds / scale));
                CHECK(b.loopSeconds == doctest::Approx(b.barOffset(b.loopBars)).epsilon(1e-4));
                for (std::size_t j = 0; j < a.moveCount; ++j) {
                    CHECK(b.moves[j].tellAt == doctest::Approx(a.moves[j].tellAt / scale));
                    CHECK(b.moves[j].impactAt == doctest::Approx(a.moves[j].impactAt / scale));
                    CHECK(b.moves[j].endAt == doctest::Approx(a.moves[j].endAt / scale));
                    // A move still sits on its bar and beat: its time is that place at the new tempo.
                    CHECK(b.moves[j].impactAt == doctest::Approx(b.barOffset(static_cast<std::size_t>(a.moves[j].impact.bar - 1)) +
                                                                 a.moves[j].impact.eighth * b.eighthSeconds()));
                }
                for (std::size_t j = 0; j < a.restCount; ++j) CHECK(b.rests[j].time == doctest::Approx(a.rests[j].time / scale));
                for (std::size_t j = 0; j < a.silenceCount; ++j)
                    CHECK(b.silences[j].seconds == doctest::Approx(a.silences[j].seconds / scale));
                for (std::size_t j = 0; j < a.finisherCount; ++j)
                    CHECK(b.finisher[j].impactAt == doctest::Approx(a.finisher[j].impactAt / scale));
            }
        }
    }
    SUBCASE("a scale under the floor is held at the floor") {
        const BeatBossDef d = rescaleBeatBossDef(test.def, 0.1);
        CHECK(d.phases[1].bpm >= kMinBossBpm - 1e-6);
        CHECK(d.phases[0].bpm == doctest::Approx(60.0).epsilon(0.001));
    }
    SUBCASE("the same fight, played slower: the autopilot wins it in the same bars and about twice the time") {
        double fast = 0.0, slow = 0.0;
        int fastBars = 0, slowBars = 0;
        REQUIRE(autopilotWins(cine.def, BeatBossTuning{}, &fast, &fastBars));
        REQUIRE(autopilotWins(rescaleBeatBossDef(cine.def, 0.5), BeatBossTuning{}, &slow, &slowBars));
        CHECK(slowBars == fastBars);
        CHECK(slow == doctest::Approx(fast * 2.0).epsilon(0.05));
        double mid = 0.0;
        REQUIRE(autopilotWins(rescaleBeatBossDef(cine.def, 0.75), BeatBossTuning{}, &mid));
        CHECK(mid == doctest::Approx(fast / 0.75).epsilon(0.05));
    }
}

TEST_CASE("beat access: no two windows of one button overlap at the scale the options allow") {
    const BeatBossTrack cine = loadTrack("cine_track");
    const BeatBossTrack test = loadTrack("test_track");

    // A track whose two strikes are a sixteenth apart can be played at 1 but not at more.
    const std::string close = R"({"version": 1, "id": "close", "boss": {"hp": 100}, "lead": {"hp": 100},
      "stems": [{"id": "s"}],
      "phases": [{"id": "p", "bpm": 120, "meter": "4/4", "loop": {"bars": 1, "seconds": 2.0},
        "moves": [{"id": "m1", "tell": "1.1", "impact": "1.3", "answer": "attack", "stem": "s"},
                  {"id": "m2", "tell": "1.1", "impact": "1.3&", "answer": "attack", "stem": "s"}]}]})";
    BeatBossTrack tight;
    std::string error;
    REQUIRE_MESSAGE(importBeatBossTrack(close, tight, &error), error);
    CHECK_FALSE(beatBossWindowsOverlap(tight.def, BeatBossTuning{}));
    CHECK(beatBossWindowsOverlap(tight.def, beatAccessTuning({}, scaled(1.5f))));
    CHECK(maxBeatWindowScale(tight.def, BeatBossTuning{}, 2.0f) == 1.0f);

    // cine_track tolerates the whole range, and the autopilot wins it at the widest windows.
    CHECK(maxBeatWindowScale(cine.def, BeatBossTuning{}, 2.0f) == 2.0f);
    for (const float scale : {1.0f, 1.5f, 2.0f}) {
        INFO("scale " << scale);
        CHECK_FALSE(beatBossWindowsOverlap(cine.def, beatAccessTuning({}, scaled(scale))));
        CHECK(autopilotWins(cine.def, beatAccessTuning({}, scaled(scale))));
    }
    // test_track is tighter; whatever the cap says is safe, and the autopilot wins at it.
    const float safe = maxBeatWindowScale(test.def, BeatBossTuning{}, 2.0f);
    CHECK(safe >= 1.0f);
    CHECK_FALSE(beatBossWindowsOverlap(test.def, beatAccessTuning({}, scaled(safe))));
    CHECK(autopilotWins(test.def, beatAccessTuning({}, scaled(safe))));
}
