#include "ftcore/BeatChoreo.h"

#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
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
    void take(const BeatBossEvents& e, double clock) {
        for (const BeatBossEvent& x : e) events.push_back(x);
        choreo.observe(fight, e, clock);
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
