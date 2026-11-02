#include "ftcore/BeatChoreo.h"

#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace ftcore;

// The cinematic track (Data/Battle/beat_boss/cine_track.json and .choreo.json): three phases at 96, 100 and 108 bpm
// whose moves are spaced for the real clips (a boss attack on a clear beat with room for its windup, the lead's
// exchanges on attack prompts), one silence and a finisher of two bars in the last two phases. The fight, its
// choreography and what the autopilot, an idle player and a lost phase do with them.

namespace {

constexpr double kFrame = 1.0 / 240.0;

std::string readData(const char* rel) {
    std::string text;
    if (!ftcore_test::readProjectData(rel, text)) FAIL("Data/" << rel << " not found");
    return text;
}

struct Rig {
    BeatBossTrack track;
    BeatClips clips;
    BeatChoreoDef choreo;
};

const Rig& rig() {
    static const Rig r = [] {
        Rig out;
        std::string error;
        std::vector<std::string> warnings;
        REQUIRE_MESSAGE(importBeatBossTrack(readData("Battle/beat_boss/cine_track.json"), out.track, &error, &warnings), error);
        CHECK(warnings.empty());
        REQUIRE_MESSAGE(importBeatClips(readData("Anim/beat_clips.json"), out.clips, &error, &warnings), error);
        CHECK(warnings.empty());
        REQUIRE_MESSAGE(importBeatChoreo(readData("Battle/beat_boss/cine_track.choreo.json"), out.track, out.clips, out.choreo,
                                         &error, &warnings), error);
        CHECK(warnings.empty());
        return out;
    }();
    return r;
}

struct Scene {
    BeatBoss fight;
    BeatChoreo choreo;
    std::vector<BeatBossEvent> events;

    void start(double now = 0.0) {
        fight.begin(rig().track.def, BeatBossTuning{}, now);
        choreo.begin(rig().choreo, rig().clips);
    }
    // A blow that hurt the boss: where it was judged (the answer), when (track time) and when it lands on screen.
    struct Contact {
        BeatBossAnswer answer;
        double judgedAt, lands;
    };
    std::vector<Contact> contacts;
    void take(const BeatBossEvents& e, double clock) {
        for (const BeatBossEvent& x : e) events.push_back(x);
        const std::uint32_t before = choreo.hitContacts();
        choreo.observe(fight, e, clock);
        if (choreo.hitContacts() != before) {
            BeatBossAnswer answer = BeatBossAnswer::Attack;
            for (const BeatBossEvent& x : e)
                if (x.type == BeatBossEventType::Judged) answer = x.answer;
            contacts.push_back(Contact{answer, fight.trackTime(), choreo.hitContact()});
        }
    }
    void frame(double now, bool pilot) {
        if (pilot) {
            for (int guard = 0; guard < 64; ++guard) {
                const QteAction a = fight.autopilot(now);
                if (a.kind == QteAction::Kind::None) break;
                take(a.kind == QteAction::Kind::Press ? fight.press(a.input, a.time) : fight.release(a.input, a.time), a.time);
            }
        }
        take(fight.update(now), now);
    }
    int count(BeatBossEventType t) const {
        return static_cast<int>(std::count_if(events.begin(), events.end(), [t](const BeatBossEvent& e) { return e.type == t; }));
    }
};

template <typename F>
double play(Scene& s, double from, double until, bool pilot, F each) {
    double now = from;
    while (!s.fight.over() && now < until) {
        now += kFrame;
        s.frame(now, pilot);
        each(now);
    }
    return now;
}

std::string clipName(const ChoreoPose& p) { return p.clip >= 0 ? std::string(rig().clips.id(static_cast<std::size_t>(p.clip))) : "-"; }

} // namespace

TEST_CASE("beat cine: the track loads with no warnings and is the shape the demo wants") {
    const Rig& r = rig();
    const BeatBossDef& d = r.track.def;
    REQUIRE(d.phaseCount == 3);
    int silences = 0;
    for (std::size_t p = 0; p < d.phaseCount; ++p) {
        const BarMap& m = d.phases[p];
        CHECK(m.bpm >= 90.0);
        CHECK(m.bpm <= 110.0);
        silences += m.silenceCount;
        // Room for the clips: no two impacts closer than 0.8 s but a finisher's, and every counter's tell gives its
        // boss clip its windup (a boss clip's impact over 1.35 at most).
        for (std::size_t k = 0; k < m.moveCount; ++k) {
            const PinnedMove& mv = m.moves[k];
            const ChoreoMove& cm = r.choreo.phases[p].moves[k];
            if (k > 0) CHECK_MESSAGE(mv.impactAt - m.moves[k - 1].impactAt >= 0.8, "phase " << p << " move " << k);
            if (cm.boss >= 0 && r.clips[static_cast<std::size_t>(cm.boss)].impact > 0.0) {
                const double need = r.clips[static_cast<std::size_t>(cm.boss)].impact / kChoreoMaxRate;
                CHECK_MESSAGE(mv.impactAt - mv.tellAt >= need - 1e-9, "phase " << p << " move " << k << " windup");
            }
        }
    }
    CHECK(silences == 1);
    CHECK(d.phases[1].finisherBars == 2);
    CHECK(d.phases[2].finisherBars == 2);
    CHECK(r.choreo.finisherSlow < 1.0f);
}

TEST_CASE("beat cine: the autopilot wins it, an idle player loses it, and a missed phase retries") {
    SUBCASE("the autopilot wins, with no miss, all three phases, the silence and the finishers") {
        Scene s;
        s.start();
        const double end = play(s, 0.0, 200.0, true, [](double) {});
        CHECK(s.fight.state() == BeatBossState::Victory);
        CHECK(s.fight.stats().misses == 0);
        CHECK(s.fight.stats().damageTaken == 0);
        CHECK(s.fight.stats().silences == 1);
        CHECK(s.fight.stats().finishers >= 2);
        CHECK(s.fight.stats().staggers >= 1);
        CHECK(s.count(BeatBossEventType::PhaseStart) == 3);
        CHECK(s.choreo.unmatched() == 0);
        CHECK(end > 45.0);
        CHECK(end < 130.0);
        MESSAGE("won in " << end << " s, " << s.fight.stats().bars << " bars, " << s.fight.stats().staggers << " staggers");
    }
    SUBCASE("idle loses") {
        Scene s;
        s.start();
        const double end = play(s, 0.0, 200.0, false, [](double) {});
        CHECK(s.fight.state() == BeatBossState::Defeat);
        CHECK(s.fight.stats().damageDealt == 0);
        CHECK(end < 60.0);
    }
    SUBCASE("a missed phase restarts at its checkpoint and the autopilot then wins") {
        for (int lost = 1; lost <= 3; ++lost) {
            CAPTURE(lost);
            Scene s;
            s.start();
            double now = 0.0;
            bool missing = false;
            int retries = 0;
            while (now < 400.0 && !(s.fight.over() && s.fight.state() == BeatBossState::Victory)) {
                now += kFrame;
                if (!missing && retries == 0 && s.fight.phase() + 1 == lost && !s.fight.counting()) missing = true;
                if (s.fight.state() == BeatBossState::Defeat) {
                    s.fight.retry(now);
                    s.choreo.retry(s.fight);
                    ++retries;
                    missing = false;
                    continue;
                }
                s.frame(now, !missing);
            }
            CHECK(retries == 1);
            CHECK(s.fight.state() == BeatBossState::Victory);
            CHECK(s.fight.stats().retries == 1);
            CHECK(s.fight.phase() == 2);
            CHECK(s.count(BeatBossEventType::PhaseStart) >= 3);
        }
    }
}

TEST_CASE("beat cine: every blow of both fighters is on its beat, every windup is played in full and the cuts are on beats") {
    const Rig& r = rig();
    Scene s;
    s.start();
    struct Seen {
        std::uint32_t id;
        std::size_t phase;
        std::uint8_t move;
        bool finisher;
        double tell, impact;
        bool checked = false;
    };
    std::vector<Seen> seen;
    int leadBlows = 0, bossBlows = 0, windups = 0;
    std::set<int> leadClips, bossClips, shots;
    std::size_t cutsSeen = 0;
    int cuts = 0;
    play(s, 0.0, 200.0, true, [&](double) {
        // A cut is scheduled on a beat or a bar line: the newest one, as soon as it exists.
        if (s.choreo.cutCount() != cutsSeen && s.choreo.cutCount() > 0) {
            const double t = s.choreo.cut(s.choreo.cutCount() - 1).time;
            CHECK_MESSAGE(std::fabs(s.choreo.boundary(t, true) - t) < 1e-6, "cut at " << t);
            ++cuts;
        }
        cutsSeen = s.choreo.cutCount();
        for (std::size_t i = 0; i < s.fight.promptCount(); ++i) {
            const BeatBossPrompt& p = s.fight.prompt(i);
            if (std::none_of(seen.begin(), seen.end(), [&](const Seen& x) { return x.id == p.id; }))
                seen.push_back(Seen{p.id, static_cast<std::size_t>(s.fight.phase()), p.move, p.finisher, p.tell, p.impact});
        }
        for (Seen& x : seen) {
            if (x.checked || s.fight.trackTime() < x.impact) continue;
            x.checked = true;
            const ChoreoPhase& cp = r.choreo.phases[x.phase];
            const ChoreoMove& cm = x.finisher ? cp.finisher[x.move] : cp.moves[x.move];
            ChoreoSample smp;
            s.choreo.sample(x.impact, smp);
            if (smp.lead.clip != cm.ok) continue; // (a stagger took the move's place)
            const BeatClip& ok = r.clips[static_cast<std::size_t>(cm.ok)];
            CHECK_MESSAGE(std::fabs(smp.lead.time - ok.impact) / smp.lead.rate < 0.001, clipName(smp.lead) << " at " << smp.lead.time);
            ++leadBlows;
            leadClips.insert(cm.ok);
            // The windup is played in full: just after the clip's start it is near its first frame (it began at 0),
            // not partway in.
            const double over = ok.impact / smp.lead.rate;
            ChoreoSample early;
            s.choreo.sample(x.impact - over + 0.03, early);
            if (early.lead.clip == cm.ok) {
                CHECK_MESSAGE(early.lead.time < 0.03 * kChoreoMaxRate + 0.01, clipName(early.lead) << " " << early.lead.time << " impact " << x.impact);
                ++windups;
            }
            if (cm.boss >= 0 && r.clips[static_cast<std::size_t>(cm.boss)].impact > 0.0) {
                ChoreoSample b;
                s.choreo.sample(x.impact, b);
                if (b.boss.clip == cm.boss) {
                    CHECK(std::fabs(b.boss.time - r.clips[static_cast<std::size_t>(cm.boss)].impact) / b.boss.rate < 0.001);
                    bossClips.insert(cm.boss);
                    ++bossBlows;
                }
            }
            shots.insert(static_cast<int>(cm.shot));
        }
    });
    CHECK(s.fight.state() == BeatBossState::Victory);
    CHECK(leadBlows >= 30);
    CHECK(bossBlows >= 10);
    CHECK(windups >= 25);
    CHECK(s.choreo.followUps() >= 8); // follow-up blows thrown after a Perfect parry or pair
    CHECK(leadClips.size() >= 8);
    CHECK(bossClips.size() >= 4);
    CHECK(shots.size() >= 6);
    CHECK(cuts >= 20);
}

TEST_CASE("beat cine: the choreography is a varied scene, no two moves in a row alike") {
    const Rig& r = rig();
    std::set<int> shots, anchors, boss, lead;
    for (std::size_t p = 0; p < r.choreo.phaseCount; ++p) {
        const ChoreoPhase& cp = r.choreo.phases[p];
        for (std::size_t k = 0; k < cp.moveCount; ++k) {
            const ChoreoMove& m = cp.moves[k];
            shots.insert(static_cast<int>(m.shot));
            anchors.insert(static_cast<int>(m.anchor));
            boss.insert(m.boss);
            lead.insert(m.ok);
            CHECK(m.shot != ChoreoShot::FinisherOrbit);
            const ChoreoMove& prev = cp.moves[(k + cp.moveCount - 1) % cp.moveCount]; // (the loop wraps)
            CHECK_MESSAGE(m.shot != prev.shot, "phase " << p << " move " << k);
        }
        for (std::size_t k = 0; k < cp.finisherCount; ++k) CHECK(cp.finisher[k].shot == ChoreoShot::FinisherOrbit);
    }
    CHECK(shots.size() >= 7);
    CHECK(anchors.size() >= 6);
    CHECK(boss.size() >= 5);
    CHECK(lead.size() >= 8);
}

TEST_CASE("beat cine: every phase has a Release move, and the Load a perfect Guard stores can be spent") {
    const Rig& r = rig();
    for (std::size_t p = 0; p < r.track.def.phaseCount; ++p) {
        const BarMap& m = r.track.def.phases[p];
        int releases = 0;
        for (std::size_t k = 0; k < m.moveCount; ++k) {
            const PinnedMove& mv = m.moves[k];
            if (mv.answer != BeatBossAnswer::Release) continue;
            ++releases;
            CHECK(mv.endAt > mv.impactAt);
            // A hold across the beat line, with room before the next impact and after the one before.
            if (k > 0) CHECK_MESSAGE(mv.impactAt - m.moves[k - 1].impactAt >= 0.8, "phase " << p);
            if (k + 1 < m.moveCount) CHECK_MESSAGE(m.moves[k + 1].impactAt - mv.endAt >= 0.3, "phase " << p);
            // The lead meets it with a clip of its own and the boss does not attack.
            const ChoreoMove& cm = r.choreo.phases[p].moves[k];
            CHECK(cm.ok >= 0);
            CHECK(cm.boss == kNoClip);
        }
        CHECK_MESSAGE(releases == 1, "phase " << p << " has " << releases << " release moves");
    }

    // The autopilot spends Load where it has some: stored by a perfect Guard in the first and the third phase.
    {
        Scene s;
        s.start();
        play(s, 0.0, 200.0, true, [](double) {});
        std::array<int, 3> perPhase{};
        int phase = -1;
        for (const BeatBossEvent& e : s.events) {
            if (e.type == BeatBossEventType::PhaseStart) phase = e.amount;
            if (e.type == BeatBossEventType::Released && phase >= 0 && phase < 3) ++perPhase[static_cast<std::size_t>(phase)];
        }
        CHECK(s.fight.state() == BeatBossState::Victory);
        CHECK(perPhase[0] >= 1);
        CHECK(perPhase[2] >= 1);
        CHECK(s.fight.stats().releases == perPhase[0] + perPhase[1] + perPhase[2]);
        CHECK(s.fight.stats().misses == 0);
    }

    // The second phase has no Guard of its own: a player who saves the first phase's Load (lets its Release pass)
    // spends it in the second, where the same Release is offered.
    {
        Scene s;
        s.start();
        double now = 0.0;
        std::array<int, 3> perPhase{};
        int phase = -1;
        while (!s.fight.over() && now < 200.0) {
            now += kFrame;
            for (int guard = 0; guard < 64; ++guard) {
                const QteAction a = s.fight.autopilot(now);
                if (a.kind == QteAction::Kind::None) break;
                // The first phase's Release press is held back.
                bool saving = false;
                if (a.kind == QteAction::Kind::Press && s.fight.phase() == 0) {
                    const double track = s.fight.trackTime() + (a.time - s.fight.now());
                    for (std::size_t i = 0; i < s.fight.promptCount(); ++i) {
                        const BeatBossPrompt& p = s.fight.prompt(i);
                        saving |= p.answer == BeatBossAnswer::Release && std::fabs(track - p.impact) < 0.01;
                    }
                }
                if (saving) {
                    // Nothing else to do at this moment: let the clock pass the prompt.
                    s.take(s.fight.update(a.time + 0.2), a.time + 0.2);
                    now = std::max(now, a.time + 0.2);
                    continue;
                }
                s.take(a.kind == QteAction::Kind::Press ? s.fight.press(a.input, a.time) : s.fight.release(a.input, a.time), a.time);
            }
            s.take(s.fight.update(now), now);
        }
        for (const BeatBossEvent& e : s.events) {
            if (e.type == BeatBossEventType::PhaseStart) phase = e.amount;
            if (e.type == BeatBossEventType::Released && phase >= 0 && phase < 3) ++perPhase[static_cast<std::size_t>(phase)];
        }
        MESSAGE("saving the first phase's Load: releases per phase " << perPhase[0] << " " << perPhase[1] << " " << perPhase[2]);
        CHECK(perPhase[0] == 0);
        CHECK(perPhase[1] >= 1);
    }
}

TEST_CASE("beat cine: a Perfect parry's counter lands a beat after it, and the blow is announced for the host to wait for") {
    Scene s;
    s.start();
    play(s, 0.0, 200.0, true, [](double) {});
    REQUIRE(s.fight.state() == BeatBossState::Victory);
    int parries = 0;
    int others = 0;
    for (const Scene::Contact& c : s.contacts) {
        const double wait = c.lands - c.judgedAt;
        CHECK(wait >= -0.02);
        if (c.answer == BeatBossAnswer::Parry) {
            ++parries;
            // The counter's windup is a second long; thrown straight from the parry, entered partway through, it lands
            // on the first eighth after 0.4 s: a beat, not the beat and a bar it used to wait for.
            CHECK_MESSAGE(wait <= 0.7, "a parry's counter lands " << wait << " s after it");
            CHECK_MESSAGE(wait >= 0.3, "a parry's counter lands " << wait << " s after it");
        } else if (c.answer == BeatBossAnswer::Attack) {
            ++others;
            CHECK_MESSAGE(wait <= 0.2, "a strike lands " << wait << " s after its verdict");
        }
    }
    CHECK(parries >= 4);
    CHECK(others >= 8);
    CHECK(s.choreo.followUps() >= 8);
    CHECK(s.choreo.hitContacts() == s.contacts.size());
}

TEST_CASE("beat cine: the boss roars in the silence, the camera cuts to it and comes back, and the win freezes then ramps up") {
    const Rig& r = rig();
    REQUIRE(r.choreo.silence.cut);
    CHECK(r.choreo.silence.shot == ChoreoShot::CloseBoss);
    REQUIRE(r.choreo.silence.boss != kNoClip);
    CHECK(r.clips.id(static_cast<std::size_t>(r.choreo.silence.boss)) == "boss_roar");

    Scene s;
    s.start();
    bool silent = false;
    double silenceTrack = 0.0;
    bool onLine = false;
    double silenceAt = 0.0;
    ChoreoShot before = ChoreoShot::Wide;
    ChoreoSample during, late, after;
    bool gotDuring = false, gotLate = false, gotAfter = false;
    int roarFrames = 0, silenceFrames = 0;
    double won = -1.0;
    play(s, 0.0, 200.0, true, [&](double now) {
        ChoreoSample smp;
        s.choreo.sampleNow(smp);
        for (const BeatBossEvent& e : s.events) {
            if (e.type == BeatBossEventType::SilenceStart && !silent && silenceAt == 0.0) {
                silent = true;
                silenceAt = e.time;
                silenceTrack = s.fight.trackTime();
                onLine = std::fabs(s.choreo.boundary(silenceTrack, true) - silenceTrack) < 1e-6;
            }
        }
        if (silent && !s.fight.trackRunning() && !s.fight.over()) {
            ++silenceFrames;
            if (smp.boss.clip == r.choreo.silence.boss) ++roarFrames;
            if (!gotDuring && now >= silenceAt + 0.5) {
                during = smp;
                gotDuring = true;
            }
            if (!gotLate && now >= silenceAt + 3.0) {
                late = smp;
                gotLate = true;
            }
        } else if (silent && s.fight.trackRunning() && !gotAfter && now > silenceAt + 2.0) {
            after = smp;
            gotAfter = true;
        }
        if (!silent) before = smp.shot;
        if (s.fight.state() == BeatBossState::Victory && won < 0.0) won = now;
    });
    REQUIRE(s.fight.state() == BeatBossState::Victory);
    REQUIRE(gotDuring);
    REQUIRE(gotAfter);
    // The silence is two seconds and a bar of count (2 + 2.4 s at 100 bpm): the roar (2.5 s) plays in it from its start.
    CHECK(during.frozen);
    CHECK(during.shot == ChoreoShot::CloseBoss);
    CHECK(during.boss.clip == r.choreo.silence.boss);
    CHECK(during.boss.time == doctest::Approx(0.5).epsilon(0.1));
    CHECK(roarFrames > silenceFrames / 3);
    CHECK(roarFrames < silenceFrames); // then the stance
    if (gotLate) CHECK(late.boss.clip != r.choreo.silence.boss);
    // The track runs again: the shot that was there before is back unless the next move cuts to its own.
    CHECK(after.shot != ChoreoShot::CloseBoss);
    (void)before;
    (void)silenceTrack;
    // The cut to the roar is on the bar line the silence stopped at.
    CHECK(onLine);

    // The win: the final blow holds for 0.2 s, then the visuals ramp up from a quarter speed over 1.5 s, and rest at full.
    ChoreoSample f;
    const double end = s.fight.trackTime();
    s.choreo.sample(end + 0.05, f);
    CHECK(f.hitStop);
    CHECK(f.visualRate == 0.0f);
    s.choreo.sample(end + kVictoryFreeze + 0.001, f);
    CHECK_FALSE(f.hitStop);
    CHECK(f.visualRate == doctest::Approx(kVictoryFrom).epsilon(0.05));
    CHECK(f.slowMo == doctest::Approx(kVictoryFrom).epsilon(0.05));
    float last = f.visualRate;
    for (double t = 0.1; t < kVictoryRamp; t += 0.1) {
        s.choreo.sample(end + kVictoryFreeze + t, f);
        CHECK(f.visualRate > last);
        last = f.visualRate;
    }
    s.choreo.sample(end + kVictoryFreeze + kVictoryRamp + 0.5, f);
    CHECK(f.visualRate == 1.0f);
    CHECK_FALSE(f.hitStop);
    CHECK(f.over);
    (void)won;
}
