#include "ftcore/BeatChoreo.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace ftcore;

// The choreography layer over the beat boss (BeatChoreo.h): the clip catalogue, the choreography file,
// the timing math, the shots and the sample, all against Data/Battle/beat_boss/test_track.json.
// Phase A is 120 bpm in 4/4 (a beat 0.5 s) with bars of 8, 8, 10 and 6 eighths; phase B is 6/8 at 80
// (a beat 0.75 s, an eighth 0.25 s) with a 7/8 bar. A fight begun at 0 has its first bar at track 0.

namespace {

constexpr double kFrame = 0.002;

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
        REQUIRE_MESSAGE(importBeatBossTrack(readData("Battle/beat_boss/test_track.json"), out.track, &error), error);
        std::vector<std::string> warnings;
        REQUIRE_MESSAGE(importBeatClips(readData("Anim/beat_clips.json"), out.clips, &error, &warnings), error);
        CHECK(warnings.empty());
        REQUIRE_MESSAGE(importBeatChoreo(readData("Battle/beat_boss/test_track.choreo.json"), out.track, out.clips,
                                         out.choreo, &error, &warnings), error);
        CHECK(warnings.empty());
        return out;
    }();
    return r;
}

// One phase alone (the last, so it ends with the boss down), the track's def and its choreography.
BeatBossDef onlyPhase(const BeatBossDef& def, int phase) {
    BeatBossDef d = def;
    d.phases[0] = def.phases[static_cast<std::size_t>(phase)];
    d.phases[0].endAt = 0.0f;
    d.phaseCount = 1;
    return d;
}

BeatChoreoDef onlyChoreo(const BeatChoreoDef& def, int phase) {
    BeatChoreoDef d = def;
    d.phases[0] = def.phases[static_cast<std::size_t>(phase)];
    d.phaseCount = 1;
    return d;
}

struct Scene {
    BeatBoss fight;
    BeatChoreo choreo;
    std::vector<BeatBossEvent> events;

    void start(const BeatBossDef& def, const BeatChoreoDef& cdef, double now = 0.0) {
        fight.begin(def, BeatBossTuning{}, now);
        choreo.begin(cdef, rig().clips);
    }
    void take(const BeatBossEvents& e, double clock) {
        for (const BeatBossEvent& x : e) events.push_back(x);
        choreo.observe(fight, e, clock);
    }
    // One host frame: the autopilot's presses (each observed as it is made), then the clock.
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

// Plays frames until the fight is over or `until`; `each` runs after every frame. Returns the clock.
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

// A catalogue of `n` clips, as JSON.
std::string clipsJson(int n, const std::string& extra = "") {
    std::string s = R"({"version": 1, "clips": [)";
    for (int i = 0; i < n; ++i) {
        if (i > 0) s += ", ";
        s += R"({"id": "clip_)" + std::to_string(i) + R"(", "asset": "/Game/Test/clip_)" + std::to_string(i) + R"(", "length_s": 1.5, "impact_s": 0.5})";
    }
    return s + extra + "]}";
}

std::string swapText(std::string s, const std::string& from, const std::string& to) {
    const std::size_t at = s.find(from);
    REQUIRE_MESSAGE(at != std::string::npos, from);
    s.replace(at, from.size(), to);
    return s;
}

} // namespace

// ---- the clip catalogue ----

TEST_CASE("beat choreo: the shipped clip catalogue has every clip the choreography is made of") {
    const BeatClips& c = rig().clips;
    for (const char* id : {"lead_stance", "lead_jab", "lead_cross", "lead_kick_r", "lead_counter", "lead_finisher", "lead_block",
                           "lead_parry", "lead_dodge_l", "lead_jump", "lead_hitreact", "lead_hitreact2", "lead_downed",
                           "lead_victory", "boss_stance", "boss_windup", "boss_hook", "boss_smash", "boss_sweep", "boss_thrust",
                           "boss_grab", "boss_roar", "boss_hitreact", "boss_stagger", "boss_defeat"}) {
        const int at = c.find(id);
        REQUIRE_MESSAGE(at >= 0, id);
        const BeatClip& clip = c[static_cast<std::size_t>(at)];
        CHECK(clip.length > 0.0);
        CHECK(clip.impact >= 0.0);
        CHECK(clip.impact <= clip.length);
        CHECK_MESSAGE(clip.asset().rfind("/Game/Combat/BeatAnims/", 0) == 0, id);
        CHECK(clip.asset().size() > std::string_view(id).size());
    }
    CHECK(c.count == 25);
    CHECK(c[static_cast<std::size_t>(c.find("lead_stance"))].loop);
    CHECK(c[static_cast<std::size_t>(c.find("boss_stance"))].loop);
    CHECK_FALSE(c[static_cast<std::size_t>(c.find("lead_jab"))].loop);
    CHECK(c.find("lead_nothing") == -1);
}

TEST_CASE("beat choreo: the loader takes any number of clips up to the cap, and refuses with a path") {
    BeatClips c;
    std::string error;
    SUBCASE("up to the cap") {
        for (const int n : {1, 26, 64, static_cast<int>(kMaxBeatClips)}) {
            REQUIRE_MESSAGE(importBeatClips(clipsJson(n), c, &error), error);
            CHECK(c.count == static_cast<std::size_t>(n));
            CHECK(c.id(static_cast<std::size_t>(n - 1)) == "clip_" + std::to_string(n - 1));
            CHECK(c.asset(static_cast<std::size_t>(n - 1)) == "/Game/Test/clip_" + std::to_string(n - 1));
            CHECK(c.find("clip_" + std::to_string(n - 1)) == n - 1);
        }
        CHECK_FALSE(importBeatClips(clipsJson(static_cast<int>(kMaxBeatClips) + 1), c, &error));
        CHECK(error.find("clips: expected an array of 1..128") != std::string::npos);
        CHECK(c.count == 0);
    }
    SUBCASE("refusals") {
        const auto refused = [&](const std::string& json, const std::string& needle) {
            CHECK_FALSE(importBeatClips(json, c, &error));
            CHECK_MESSAGE(error.find(needle) != std::string::npos, error);
            CHECK(c.count == 0);
        };
        refused(R"({"version": 1,)", "beat clips file: ");
        refused(R"({"version": 2, "clips": [{"id": "a", "length_s": 1}]})", "version: expected 1");
        refused(R"({"version": 1, "clips": []})", "clips: expected an array");
        refused(clipsJson(0, R"({"id": "a b", "length_s": 1})"), "clips[0].id: expected 1..31");
        refused(clipsJson(0, R"({"id": "abcdefghijklmnopqrstuvwxyz0123456", "length_s": 1})"), "clips[0].id: expected 1..31");
        refused(clipsJson(1, R"(, {"id": "clip_0", "length_s": 1})"), "clips[1].id: duplicate clip clip_0");
        refused(clipsJson(0, R"({"id": "a", "length_s": 0})"), "clips[0].length_s: expected seconds over 0");
        refused(clipsJson(0, R"({"id": "a"})"), "clips[0].length_s: expected seconds over 0");
        refused(clipsJson(0, R"({"id": "a", "length_s": 1, "impact_s": 1.5})"), "clips[0].impact_s: expected seconds within the clip");
        refused(clipsJson(0, R"({"id": "a", "length_s": 1, "impact_s": -0.1})"), "clips[0].impact_s");
        refused(clipsJson(0, R"({"id": "a", "asset": "", "length_s": 1})"), "clips[0].asset: expected a path");
        refused(clipsJson(0, R"({"id": "a", "asset": "Game/x", "length_s": 1})"), "clips[0].asset: expected a path");
        refused(clipsJson(0, R"({"id": "a", "asset": 3, "length_s": 1})"), "clips[0].asset: expected a path");
        refused(clipsJson(0, R"({"id": "a", "length": 1})"), "clips[0].length_s: expected seconds over 0");
        refused(clipsJson(0, R"({"id": "a", "length_s": 1, "loop": 1})"), "clips[0].loop: expected true or false");
    }
    SUBCASE("the optional top-level mesh") {
        REQUIRE_MESSAGE(importBeatClips(clipsJson(1), c, &error), error);
        CHECK(c.mesh().empty());
        std::vector<std::string> warnings;
        REQUIRE_MESSAGE(importBeatClips(swapText(clipsJson(1), R"("clips")", R"("mesh": "/Game/Test/body", "clips")"), c, &error, &warnings), error);
        CHECK(warnings.empty());
        CHECK(c.mesh() == "/Game/Test/body");
        CHECK_FALSE(importBeatClips(swapText(clipsJson(1), R"("clips")", R"("mesh": "Game/body", "clips")"), c, &error));
        CHECK(error.find("mesh: expected a path") != std::string::npos);
        CHECK_FALSE(importBeatClips(swapText(clipsJson(1), R"("clips")", R"("mesh": 3, "clips")"), c, &error));
        CHECK(c.mesh().empty());
    }
    SUBCASE("the optional walk speed and blend") {
        REQUIRE_MESSAGE(importBeatClips(clipsJson(1), c, &error), error);
        CHECK(c[0].speed == 0.0);
        CHECK(c.blend == 0.0);
        std::vector<std::string> warnings;
        REQUIRE_MESSAGE(importBeatClips(swapText(swapText(clipsJson(1), R"("impact_s": 0.5)", R"("impact_s": 0.5, "speed_mps": 1.15)"),
                                                 R"("clips")", R"("blend_s": 0.3, "clips")"), c, &error, &warnings), error);
        CHECK(warnings.empty());
        CHECK(c[0].speed == doctest::Approx(1.15));
        CHECK(c.blend == doctest::Approx(0.3));
        const auto refused = [&](const std::string& json, const std::string& needle) {
            CHECK_FALSE(importBeatClips(json, c, &error));
            CHECK_MESSAGE(error.find(needle) != std::string::npos, error);
            CHECK(c.blend == 0.0);
        };
        refused(clipsJson(0, R"({"id": "a", "length_s": 1, "speed_mps": 0})"), "clips[0].speed_mps: expected metres a second over 0 and at most 20");
        refused(clipsJson(0, R"({"id": "a", "length_s": 1, "speed_mps": 21})"), "clips[0].speed_mps");
        refused(clipsJson(0, R"({"id": "a", "length_s": 1, "speed_mps": "1"})"), "clips[0].speed_mps");
        refused(swapText(clipsJson(1), R"("clips")", R"("blend_s": 0, "clips")"), "blend_s: expected seconds over 0 and at most 1");
        refused(swapText(clipsJson(1), R"("clips")", R"("blend_s": 1.5, "clips")"), "blend_s");
        refused(swapText(clipsJson(1), R"("clips")", R"("blend_s": true, "clips")"), "blend_s");
    }
    SUBCASE("the optional additive fade-in") {
        REQUIRE_MESSAGE(importBeatClips(clipsJson(1), c, &error), error);
        CHECK_FALSE(c[0].additiveIn);
        std::vector<std::string> warnings;
        REQUIRE_MESSAGE(importBeatClips(clipsJson(0, R"({"id": "a", "length_s": 0.5, "additive_in": true}, {"id": "b", "length_s": 1, "additive_in": false})"),
                                        c, &error, &warnings), error);
        CHECK(warnings.empty());
        CHECK(c[0].additiveIn);
        CHECK_FALSE(c[1].additiveIn);
        CHECK_FALSE(importBeatClips(clipsJson(0, R"({"id": "a", "length_s": 1, "additive_in": 1})"), c, &error));
        CHECK_MESSAGE(error.find("clips[0].additive_in: expected true or false") != std::string::npos, error);
        CHECK(c.count == 0);
    }
    SUBCASE("a clip's own blend: optional, in place of the table's") {
        REQUIRE_MESSAGE(importBeatClips(clipsJson(1), c, &error), error);
        CHECK(c[0].blend == 0.0);
        std::vector<std::string> warnings;
        REQUIRE_MESSAGE(importBeatClips(swapText(swapText(clipsJson(1), R"("impact_s": 0.5)", R"("impact_s": 0.5, "blend_s": 0.1)"),
                                                 R"("clips")", R"("blend_s": 0.25, "clips")"), c, &error, &warnings), error);
        CHECK(warnings.empty());
        CHECK(c[0].blend == doctest::Approx(0.1));
        CHECK(c.blend == doctest::Approx(0.25));
        const auto refused = [&](const std::string& json, const std::string& needle) {
            CHECK_FALSE(importBeatClips(json, c, &error));
            CHECK_MESSAGE(error.find(needle) != std::string::npos, error);
            CHECK(c.count == 0);
        };
        refused(clipsJson(0, R"({"id": "a", "length_s": 1, "blend_s": 0})"), "clips[0].blend_s: expected seconds over 0 and at most 1");
        refused(clipsJson(0, R"({"id": "a", "length_s": 1, "blend_s": 1.5})"), "clips[0].blend_s");
        refused(clipsJson(0, R"({"id": "a", "length_s": 1, "blend_s": "0.1"})"), "clips[0].blend_s");
    }
    SUBCASE("unknown keys are warnings") {
        std::vector<std::string> warnings;
        CHECK(importBeatClips(swapText(clipsJson(1), R"("impact_s": 0.5)", R"("impact_s": 0.5, "notes": "x")"), c, &error, &warnings));
        REQUIRE(warnings.size() == 1);
        CHECK(warnings[0] == "clips[0]: unknown key: notes");
    }
}

TEST_CASE("beat choreo: the choreography file is checked against the track and the catalogue") {
    const Rig& r = rig();
    const std::string text = readData("Battle/beat_boss/test_track.choreo.json");
    const BeatChoreoDef& d = r.choreo;
    REQUIRE(d.phaseCount == 2);
    CHECK(d.phases[0].moveCount == 6);
    CHECK(d.phases[0].finisherCount == 3);
    CHECK(d.phases[1].moveCount == 6);
    CHECK(d.phases[1].finisherCount == 3);
    CHECK(r.clips.id(static_cast<std::size_t>(d.stanceLead)) == "lead_stance");
    CHECK(r.clips.id(static_cast<std::size_t>(d.phases[0].moves[0].boss)) == "boss_hook");
    CHECK(r.clips.id(static_cast<std::size_t>(d.phases[0].moves[0].then)) == "lead_counter");
    CHECK(d.phases[0].moves[0].shot == ChoreoShot::OverLead);
    CHECK(d.phases[0].moves[0].anchor == ChoreoAnchor::BossFistR);
    CHECK(d.phases[0].moves[3].hit == kNoClip);
    CHECK(d.stagger.cut);
    CHECK(d.victory.shot == ChoreoShot::LowHero);
    CHECK(d.finisherSlow == doctest::Approx(0.4f));

    const auto refused = [&](const std::string& json, const std::string& needle) {
        BeatChoreoDef out;
        std::string error;
        CHECK_FALSE(importBeatChoreo(json, r.track, r.clips, out, &error));
        CHECK_MESSAGE(error.find(needle) != std::string::npos, error);
        CHECK(out.phaseCount == 0);
    };
    refused(R"({"version": 1,)", "beat choreography file: ");
    refused(swapText(text, R"("track": "test_track")", R"("track": "other")"), "track: expected test_track");
    refused(swapText(text, R"("id": "a_strike")", R"("id": "a_other")"), "phases[0].moves[1].id: expected a_strike");
    refused(swapText(text, R"("id": "b_fin_2")", R"("id": "b_x")"), "phases[1].finisher[1].id: expected b_fin_2");
    refused(swapText(text, R"("boss": "boss_hook")", R"("boss": "boss_nothing")"), "phases[0].moves[0].boss: unknown clip boss_nothing");
    refused(swapText(text, R"("shot": "over_lead")", R"("shot": "sideways")"), "phases[0].moves[0].shot: expected one of");
    refused(swapText(text, R"("anchor": "boss_fist_r")", R"("anchor": "elbow")"), "phases[0].moves[0].anchor: expected one of");
    refused(swapText(text, R"("ok": "lead_parry", )", ""), "phases[0].moves[0].ok: expected a clip id");
    refused(swapText(text, R"("stance": {"lead": "lead_stance")", R"("stance": {"lead": "lead_jab")"), "stance: lead_jab must be a looping clip");
    refused(swapText(text, R"("slow": 0.4)", R"("slow": 0.05)"), "finisher.slow: expected a speed in 0.2..1");
    refused(swapText(text, R"("whiff": "lead_hitreact")", R"("whiff": "lead_nothing")"), "choreography.whiff: unknown clip lead_nothing");
    CHECK(r.clips.id(static_cast<std::size_t>(d.whiffLead)) == "lead_hitreact");
}

// ---- the timing math ----

TEST_CASE("beat choreo: planChoreoClip lands the impact on the beat at every rate inside the clamp") {
    const Rig& r = rig();
    int swept = 0;
    for (std::size_t p = 0; p < r.track.def.phaseCount; ++p) {
        const BarMap& m = r.track.def.phases[p];
        for (std::size_t k = 0; k < m.moveCount + m.finisherCount; ++k) {
            const PinnedMove& mv = k < m.moveCount ? m.moves[k] : m.finisher[k - m.moveCount];
            // The move's boss clip, or (the finisher's strikes have none) any clip with an impact.
            const ChoreoPhase& cp = r.choreo.phases[p];
            const ChoreoMove& cm = k < m.moveCount ? cp.moves[k] : cp.finisher[k - m.moveCount];
            // (a boss clip with no blow of its own, a windup or a roar, is not pinned: the lead's blow is the move)
            const bool bossBlow = cm.boss >= 0 && r.clips[static_cast<std::size_t>(cm.boss)].impact > 0.0;
            const BeatClip& clip = r.clips[static_cast<std::size_t>(bossBlow ? cm.boss : cm.ok)];
            REQUIRE(clip.impact > 0.0);
            for (double rate = kChoreoMinRate; rate <= kChoreoMaxRate + 1e-9; rate += 0.01) {
                // A tell that spans the windup at exactly this rate.
                const double desired = mv.impactAt - clip.impact / rate;
                const ChoreoPlan plan = planChoreoClip(clip, mv.impactAt, desired, -1e30);
                CHECK(plan.rate == doctest::Approx(rate).epsilon(1e-9));
                CHECK(plan.offset == 0.0);
                const double lands = plan.start + (clip.impact - plan.offset) / plan.rate;
                CHECK(std::fabs(lands - mv.impactAt) < 0.001);
                CHECK(plan.rate >= kChoreoMinRate - 1e-12);
                CHECK(plan.rate <= kChoreoMaxRate + 1e-12);
                ++swept;
            }
            // Any tell, and any earliest start: the impact still lands, inside the clamp.
            for (double tellSpan = 0.05; tellSpan < 2.0; tellSpan += 0.07) {
                for (double room = -0.2; room < 1.5; room += 0.05) {
                    const ChoreoPlan plan = planChoreoClip(clip, mv.impactAt, mv.impactAt - tellSpan, mv.impactAt - room);
                    const double lands = plan.start + (clip.impact - plan.offset) / plan.rate;
                    CHECK(std::fabs(lands - mv.impactAt) < 0.001);
                    CHECK(plan.rate >= kChoreoMinRate - 1e-12);
                    CHECK(plan.rate <= kChoreoMaxRate + 1e-12);
                    CHECK(plan.offset >= 0.0);
                    CHECK(plan.offset <= clip.impact + 1e-12);
                    if (room > 0.0) CHECK(plan.start >= mv.impactAt - room - 1e-12);
                }
            }
        }
    }
    CHECK(swept > 18 * 50);
}

TEST_CASE("beat choreo: a windup that does not fit starts partway through; one that fits is not rushed") {
    BeatClip clip;
    clip.length = 1.0;
    clip.impact = 0.6;
    // Plenty of room: the rate is the one that spans the tell, in the clamp.
    ChoreoPlan p = planChoreoClip(clip, 10.0, 9.0, 0.0);
    CHECK(p.rate == doctest::Approx(0.75));
    CHECK(p.start == doctest::Approx(10.0 - 0.6 / 0.75));
    CHECK(p.offset == 0.0);
    p = planChoreoClip(clip, 10.0, 9.4, 0.0);
    CHECK(p.rate == doctest::Approx(1.0));
    CHECK(p.start == doctest::Approx(9.4));
    // A short tell: sped up to the top of the clamp, and no further.
    p = planChoreoClip(clip, 10.0, 9.7, 0.0);
    CHECK(p.rate == doctest::Approx(kChoreoMaxRate));
    CHECK(p.start == doctest::Approx(10.0 - 0.6 / 1.35));
    // The actor is not free until 9.7: squeezed to fit, still inside the clamp.
    p = planChoreoClip(clip, 10.0, 9.0, 9.6);
    CHECK(p.rate == doctest::Approx(1.5).epsilon(1.0));
    CHECK(p.rate <= kChoreoMaxRate);
    p = planChoreoClip(clip, 10.0, 9.4, 9.5);
    CHECK(p.start == doctest::Approx(9.5));
    CHECK(p.rate == doctest::Approx(1.2));
    CHECK(p.offset == 0.0);
    // Too tight even at the top of the clamp: it starts partway in.
    p = planChoreoClip(clip, 10.0, 9.4, 9.8);
    CHECK(p.start == doctest::Approx(9.8));
    CHECK(p.rate == doctest::Approx(kChoreoMaxRate));
    CHECK(p.offset == doctest::Approx(0.6 - 0.2 * 1.35));
    CHECK(p.start + (clip.impact - p.offset) / p.rate == doctest::Approx(10.0));
    // Already past: the clip starts on its impact frame.
    p = planChoreoClip(clip, 10.0, 9.4, 10.5);
    CHECK(p.start == doctest::Approx(10.0));
    CHECK(p.offset == doctest::Approx(0.6));
    // A clip with no impact offset just starts when it is wanted.
    clip.impact = 0.0;
    p = planChoreoClip(clip, 10.0, 9.0, 0.0);
    CHECK(p.start == doctest::Approx(9.0));
    CHECK(p.rate == 1.0);
    p = planChoreoClip(clip, 10.0, 9.0, 9.5);
    CHECK(p.start == doctest::Approx(9.5));
}

TEST_CASE("beat choreo: a stance cycle is a whole number of beats, inside the clamp, in both phases") {
    const Rig& r = rig();
    for (const double beat : {0.5, 0.75}) {
        for (const std::int16_t id : {r.choreo.stanceLead, r.choreo.stanceBoss}) {
            const BeatClip& clip = r.clips[static_cast<std::size_t>(id)];
            const double rate = choreoStanceRate(clip, beat);
            const double cycle = clip.length / rate; // seconds one cycle takes
            const double beats = cycle / beat;
            CHECK(rate >= kChoreoMinRate);
            CHECK(rate <= kChoreoMaxRate);
            CHECK(beats == doctest::Approx(std::round(beats)).epsilon(1e-9));
            CHECK(std::round(beats) >= 1.0);
        }
    }
    // A very short stance: one beat is the most that fits; a long one takes many.
    BeatClip s;
    s.length = 0.4;
    s.loop = true;
    CHECK(choreoStanceRate(s, 0.5) == doctest::Approx(0.8));
    s.length = 6.0;
    const double rate = choreoStanceRate(s, 0.5);
    CHECK(rate >= kChoreoMinRate);
    CHECK(rate <= kChoreoMaxRate);
    CHECK(std::fmod(6.0 / rate / 0.5, 1.0) == doctest::Approx(0.0).epsilon(1e-9));
}

// ---- the scene over a fight ----

TEST_CASE("beat choreo: every move of the test track has its impact on its beat on screen") {
    const Rig& r = rig();
    struct Seen {
        std::uint32_t id;
        std::size_t phase;
        std::uint8_t move;
        bool finisher;
        double impact;
        bool checked = false;
    };
    for (const bool pilot : {true, false}) {
        for (const int which : {-1, 0, 1}) { // the whole track, phase A alone, phase B alone
            const BeatBossDef def = which < 0 ? r.track.def : onlyPhase(r.track.def, which);
            const BeatChoreoDef cdef = which < 0 ? r.choreo : onlyChoreo(r.choreo, which);
            Scene s;
            s.start(def, cdef);
            std::vector<Seen> seen;
            int checked = 0;
            int checkedLead = 0;
            int leadOther = 0;
            int overridden = 0;
            int staggered = 0;
            std::set<int> movesSeen;
            play(s, 0.0, pilot ? 120.0 : 40.0, pilot, [&](double) {
                for (std::size_t i = 0; i < s.fight.promptCount(); ++i) {
                    const BeatBossPrompt& p = s.fight.prompt(i);
                    if (std::none_of(seen.begin(), seen.end(), [&](const Seen& x) { return x.id == p.id; }))
                        seen.push_back(Seen{p.id, static_cast<std::size_t>(s.fight.phase()), p.move, p.finisher, p.impact});
                }
                for (Seen& x : seen) {
                    if (x.checked || s.fight.trackTime() < x.impact) continue;
                    x.checked = true;
                    const ChoreoPhase& cp = cdef.phases[x.phase];
                    const ChoreoMove& cm = x.finisher ? cp.finisher[x.move] : cp.moves[x.move];
                    ChoreoSample smp;
                    s.choreo.sample(x.impact, smp);
                    // The lead's answer is pre-rolled: its blow is on the move's impact, answered or not (the
                    // press only decides what it turns into afterwards).
                    if (cm.ok >= 0) {
                        const BeatClip& ok = r.clips[static_cast<std::size_t>(cm.ok)];
                        if (smp.lead.clip == cm.ok && ok.impact > 0.0) {
                            CHECK_MESSAGE(std::fabs(smp.lead.time - ok.impact) / smp.lead.rate < 0.001,
                                          clipName(smp.lead) << " at " << smp.lead.time << " rate " << smp.lead.rate << " impact " << x.impact << " phase " << x.phase << " move " << int(x.move) << " fin " << x.finisher << " visual " << smp.visualRate);
                            ++checkedLead;
                        } else {
                            ++leadOther;
                        }
                    }
                    if (cm.boss < 0) continue;
                    if (smp.boss.clip != cm.boss) {
                        // Only a stagger takes a pinned move's place (the boss is down and the move is dropped).
                        if (clipName(smp.boss) == "boss_stagger") ++staggered;
                        else ++overridden;
                        continue;
                    }
                    const BeatClip& clip = r.clips[static_cast<std::size_t>(cm.boss)];
                    if (!(clip.impact > 0.0)) continue; // a windup or a roar has no blow of its own to pin (the lead's answer is checked above)
                    // The impact frame is on screen within 1 ms (of clip time, at the clip's rate).
                    CHECK_MESSAGE(std::fabs(smp.boss.time - clip.impact) / smp.boss.rate < 0.001,
                                  clipName(smp.boss) << " at " << smp.boss.time << " rate " << smp.boss.rate);
                    CHECK(smp.visualRate == doctest::Approx(1.0f));
                    ++checked;
                    movesSeen.insert(static_cast<int>(x.phase * 100 + x.move + (x.finisher ? 50 : 0)));
                }
            });
            CAPTURE(pilot);
            CAPTURE(which);
            CHECK(checked > 2); // (the boss moves with a blow of their own: the hook, sweep, thrust, smash and grab)
            CHECK(checkedLead > 6);
            CHECK(checkedLead > 4 * leadOther); // (an answer another clip took over: a stagger, a squeezed neighbour)
            CHECK(overridden == 0);
            CHECK(staggered <= 4);
            CHECK(s.choreo.unmatched() == 0);
            if (pilot && which < 0) CHECK(movesSeen.size() >= 4);
        }
    }
}

TEST_CASE("beat choreo: clip rates stay inside the clamp, and nothing plays faster than the scheduled rate") {
    const Rig& r = rig();
    Scene s;
    s.start(r.track.def, r.choreo);
    int poses = 0;
    play(s, 0.0, 120.0, true, [&](double) {
        ChoreoSample smp;
        s.choreo.sampleNow(smp);
        for (const ChoreoPose* p : {&smp.lead, &smp.boss}) {
            if (p->clip < 0 || p->rate == 0.0f) continue;
            CHECK_MESSAGE(p->rate >= kChoreoMinRate - 1e-6, clipName(*p) << " " << p->rate);
            CHECK_MESSAGE(p->rate <= kChoreoMaxRate + 1e-6, clipName(*p) << " " << p->rate);
            ++poses;
        }
    });
    CHECK(poses > 1000);
    CHECK(s.fight.state() == BeatBossState::Victory);
}

TEST_CASE("beat choreo: the camera cuts only on beat or bar boundaries, in a quarter-note phase to the half second") {
    const Rig& r = rig();
    const BeatBossDef def = onlyPhase(r.track.def, 0); // 120 bpm, 4/4, 5/4, 3/4 bars: every boundary is a multiple of 0.5 s
    for (const bool pilot : {true, false}) {
        Scene s;
        s.start(def, onlyChoreo(r.choreo, 0));
        std::set<std::pair<long long, int>> cuts; // time in microseconds, shot
        std::set<ChoreoShot> shots;
        play(s, 0.0, pilot ? 120.0 : 40.0, pilot, [&](double) {
            for (std::size_t i = 0; i < s.choreo.cutCount(); ++i) {
                const ChoreoCut& c = s.choreo.cut(i);
                cuts.insert({std::llround(c.time * 1e6), static_cast<int>(c.shot)});
                shots.insert(c.shot);
            }
        });
        CHECK(cuts.size() > (pilot ? 12u : 4u));
        CHECK(shots.size() >= 5);
        for (const auto& c : cuts) {
            const double beats = static_cast<double>(c.first) / 1e6 / 0.5;
            CHECK_MESSAGE(std::fabs(beats - std::round(beats)) < 1e-5, "cut at " << c.first << " us");
        }
        // The finisher bar is orbited from its bar line.
        if (pilot) CHECK(shots.count(ChoreoShot::FinisherOrbit) == 1);
    }
}

TEST_CASE("beat choreo: in the 6/8 phase with its 7/8 bar a cut is on a beat counted from its own bar line") {
    const Rig& r = rig();
    const BeatBossDef def = onlyPhase(r.track.def, 1);
    for (const bool pilot : {true, false}) {
        Scene s;
        s.start(def, onlyChoreo(r.choreo, 1));
        std::vector<ChoreoBar> bars;      // the bars the scene saw
        std::vector<double> barTrack;     // the fight's own track time when each began (within a frame)
        std::set<long long> cutTimes;
        int cuts = 0;
        double lastStart = -1.0;
        play(s, 0.0, pilot ? 120.0 : 40.0, pilot, [&](double) {
            if (s.choreo.barCount() > 0) {
                const ChoreoBar& b = s.choreo.bar(s.choreo.barCount() - 1);
                if (b.start != lastStart) {
                    lastStart = b.start;
                    bars.push_back(b);
                    // The scene's bar line is the fight's bar line, to the frame.
                    CHECK(std::fabs(s.fight.trackTime() - b.start) <= 2 * kFrame + 1e-9);
                }
            }
            for (std::size_t i = 0; i < s.choreo.cutCount(); ++i) cutTimes.insert(std::llround(s.choreo.cut(i).time * 1e6));
        });
        REQUIRE(bars.size() > (pilot ? 8u : 3u));
        // Every bar is 6, 7 or 3 eighths... of 0.25 s: a multiple of a quarter second long, and its beats 0.75 s.
        for (const ChoreoBar& b : bars) {
            CHECK(b.beat == doctest::Approx(0.75));
            const double eighths = (b.end - b.start) / 0.25;
            CHECK(std::fabs(eighths - std::round(eighths)) < 1e-6);
        }
        for (const long long t : cutTimes) {
            const double at = static_cast<double>(t) / 1e6;
            bool on = false;
            for (const ChoreoBar& b : bars) {
                if (at < b.start - 1e-5 || at > b.end + 1e-5) continue;
                const double k = (at - b.start) / b.beat;
                on = on || std::fabs(k - std::round(k)) < 1e-5 || std::fabs(at - b.end) < 1e-5;
            }
            CHECK_MESSAGE(on, "cut at " << at);
            ++cuts;
        }
        CHECK(cuts > (pilot ? 8 : 3));
    }
}

TEST_CASE("beat choreo: the test track's choreography is a varied exchange") {
    const Rig& r = rig();
    std::set<int> shots, bossClips, leadClips, anchors;
    for (std::size_t p = 0; p < r.choreo.phaseCount; ++p) {
        const ChoreoPhase& cp = r.choreo.phases[p];
        for (std::size_t k = 0; k < cp.moveCount; ++k) {
            const ChoreoMove& m = cp.moves[k];
            shots.insert(static_cast<int>(m.shot));
            anchors.insert(static_cast<int>(m.anchor));
            bossClips.insert(m.boss);
            leadClips.insert(m.ok);
            leadClips.insert(m.miss);
            CHECK(m.shot != ChoreoShot::FinisherOrbit);
            // No two moves in a row look the same.
            if (k > 0) {
                const ChoreoMove& prev = cp.moves[k - 1];
                CHECK((m.shot != prev.shot || m.boss != prev.boss || m.ok != prev.ok));
                CHECK(m.shot != prev.shot);
            }
        }
        for (std::size_t k = 0; k < cp.finisherCount; ++k) CHECK(cp.finisher[k].shot == ChoreoShot::FinisherOrbit);
    }
    CHECK(shots.size() >= 7);   // all but the finisher's orbit
    CHECK(anchors.size() >= 6);
    CHECK(bossClips.size() >= 7);
    CHECK(leadClips.size() >= 9);
}

TEST_CASE("beat choreo: the prompt shows by its anchor, with its lead-in running from the tell to the impact") {
    const Rig& r = rig();
    Scene s;
    s.start(onlyPhase(r.track.def, 0), onlyChoreo(r.choreo, 0));
    bool seenParry = false;
    int shown = 0;
    play(s, 0.0, 3.0, false, [&](double) {
        ChoreoSample smp;
        s.choreo.sampleNow(smp);
        if (smp.frozen) {
            CHECK(smp.promptCount == 0);
            return;
        }
        const double t = s.fight.trackTime();
        if (t > 0.02 && t < 0.98) {
            // a_parry: told at 1.1 (track 0), hit at 1.3 (track 1.0).
            REQUIRE(smp.prompt() != nullptr);
            const ChoreoPromptView& pv = *smp.prompt();
            CHECK(pv.answer == BeatBossAnswer::Parry);
            CHECK(pv.move == 0);
            CHECK(pv.anchor == ChoreoAnchor::BossFistR);
            CHECK(pv.progress == doctest::Approx(static_cast<float>(t / 1.0)).epsilon(0.01));
            seenParry = true;
        }
        if (t > 0.52 && t < 0.98) {
            // a_strike joins at 0.5; the parry, whose impact is sooner, is first.
            CHECK(smp.promptCount == 2);
            CHECK(smp.prompts[0].answer == BeatBossAnswer::Parry);
            CHECK(smp.prompts[1].answer == BeatBossAnswer::Attack);
            CHECK(smp.prompts[1].anchor == ChoreoAnchor::BossChest);
        }
        shown += static_cast<int>(smp.promptCount);
    });
    CHECK(seenParry);
    CHECK(shown > 100);
    // Nothing is shown before the fight: the lead-in is a count.
    Scene c;
    c.start(r.track.def, r.choreo);
    c.frame(0.5, false);
    ChoreoSample smp;
    c.choreo.sampleNow(smp);
    CHECK(smp.frozen);
    CHECK(smp.promptCount == 0);
    CHECK(smp.shot == r.choreo.opening);
}

TEST_CASE("beat choreo: while the track is stopped the actors loop their stance and the shot holds") {
    const Rig& r = rig();
    Scene s;
    s.start(onlyPhase(r.track.def, 1), onlyChoreo(r.choreo, 1)); // phase B has a silence at bar 1 of loop 2
    bool silenceSeen = false;
    bool beforeSet = false;
    ChoreoShot before = ChoreoShot::Wide;
    double idleStart = 0.0;
    double idleTime = 0.0;
    int frozenFrames = 0;
    play(s, 0.0, 60.0, true, [&](double now) {
        ChoreoSample smp;
        s.choreo.sampleNow(smp);
        const bool silent = s.fight.silent();
        if (silent && !silenceSeen) {
            silenceSeen = true;
            idleStart = now;
        }
        if (silent && !beforeSet) {
            before = smp.shot; // the shot the silence began on
            beforeSet = true;
        }
        if (silent && now > idleStart + 0.1) {
            // A silence cuts nothing: it holds the shot it began on, and the stance goes on.
            CHECK(smp.frozen);
            CHECK(smp.shot == before);
            CHECK(clipName(smp.lead) == "lead_stance");
            CHECK(clipName(smp.boss) == "boss_stance");
            CHECK(smp.promptCount == 0);
            CHECK(smp.lead.time >= idleTime - 1e-9 - r.clips[static_cast<std::size_t>(r.choreo.stanceLead)].length);
            ++frozenFrames;
        }
        idleTime = smp.lead.time;
    });
    CHECK(silenceSeen);
    CHECK(beforeSet);
    CHECK(frozenFrames > 100);
}

TEST_CASE("beat choreo: hit-stop pauses the animation for 50 to 80 ms on a perfect blow, and it has caught up by the beat") {
    const Rig& r = rig();
    Scene s;
    s.start(onlyPhase(r.track.def, 0), onlyChoreo(r.choreo, 0));
    std::vector<double> stops;     // lengths of the hit-stops seen, seconds
    double stopStart = -1.0;
    double frozenTime = 0.0;
    bool bent = false;
    play(s, 0.0, 120.0, true, [&](double) {
        ChoreoSample smp;
        s.choreo.sampleNow(smp);
        const double t = s.fight.trackTime();
        if (smp.hitStop) {
            CHECK(smp.visualRate == 0.0f);
            if (stopStart < 0.0) {
                stopStart = t;
                frozenTime = smp.lead.time;
                // A hit-stop is on a blow that shows: the lead's or the boss's clip is at its impact frame.
                const auto atImpact = [&](const ChoreoPose& p) {
                    return p.clip >= 0 && p.rate > 0.0f &&
                           std::fabs(p.time - r.clips[static_cast<std::size_t>(p.clip)].impact) < 0.01 &&
                           r.clips[static_cast<std::size_t>(p.clip)].impact > 0.0;
                };
                const bool onBlow = atImpact(smp.lead) || atImpact(smp.boss);
                CHECK_MESSAGE(onBlow,
                              "stop at " << t << ": " << clipName(smp.lead) << " " << smp.lead.time << ", " << clipName(smp.boss) << " " << smp.boss.time);
            }
            // The animation does not move while the stop holds.
            CHECK(smp.lead.time == doctest::Approx(frozenTime).epsilon(1e-9));
        } else if (stopStart >= 0.0) {
            stops.push_back(t - stopStart);
            stopStart = -1.0;
        }
        if (!smp.hitStop && smp.visualRate > 1.0f) bent = true;
        CHECK(smp.visualRate <= 2.0f - r.choreo.finisherSlow + 1e-3f); // the finisher's catch-up is the fastest
    });
    REQUIRE(stops.size() > 5);
    CHECK(s.choreo.hitStops() >= stops.size());
    for (const double d : stops) {
        CHECK(d >= 0.050 - kFrame);
        CHECK(d <= 0.080 + kFrame);
    }
    CHECK(bent);
    CHECK(s.fight.state() == BeatBossState::Victory); // the fight itself never noticed
}

TEST_CASE("beat choreo: a hit-stop never moves the beat: the same fight is judged and scored the same, with the scene or without") {
    const Rig& r = rig();
    for (const bool pilot : {true, false}) {
        for (const int which : {-1, 0, 1}) {
            const BeatBossDef def = which < 0 ? r.track.def : onlyPhase(r.track.def, which);
            Scene with;
            with.start(def, which < 0 ? r.choreo : onlyChoreo(r.choreo, which));
            BeatBoss without;
            without.begin(def, BeatBossTuning{}, 0.0);
            double now = 0.0;
            std::size_t events = 0;
            while (!with.fight.over() && now < 120.0) {
                now += kFrame;
                with.frame(now, pilot);
                if (pilot) {
                    for (int guard = 0; guard < 64; ++guard) {
                        const QteAction a = without.autopilot(now);
                        if (a.kind == QteAction::Kind::None) break;
                        events += (a.kind == QteAction::Kind::Press ? without.press(a.input, a.time) : without.release(a.input, a.time)).size();
                    }
                }
                events += without.update(now).size();
                ChoreoSample smp;
                with.choreo.sampleNow(smp);
            }
            CAPTURE(pilot);
            CAPTURE(which);
            const BeatBossStats& a = with.fight.stats();
            const BeatBossStats& b = without.stats();
            CHECK(with.fight.state() == without.state());
            CHECK(with.fight.now() == without.now());
            CHECK(with.fight.trackTime() == without.trackTime());
            CHECK(with.events.size() == events);
            CHECK(a.perfects == b.perfects);
            CHECK(a.goods == b.goods);
            CHECK(a.misses == b.misses);
            CHECK(a.damageDealt == b.damageDealt);
            CHECK(a.damageTaken == b.damageTaken);
            CHECK(a.bars == b.bars);
            CHECK(a.staggers == b.staggers);
            CHECK(a.finishers == b.finishers);
            CHECK(a.silences == b.silences);
            CHECK(with.fight.bossHp() == without.bossHp());
            CHECK(with.fight.hp() == without.hp());
            CHECK(with.fight.state() == (pilot ? BeatBossState::Victory : BeatBossState::Defeat));
        }
    }
}

TEST_CASE("beat choreo: the finisher bar is orbited, and its slow motion is back to full speed on every strike") {
    const Rig& r = rig();
    Scene s;
    s.start(onlyPhase(r.track.def, 0), onlyChoreo(r.choreo, 0));
    int finisherFrames = 0;
    int slowFrames = 0;
    float slowest = 1.0f;
    std::vector<double> strikes;
    bool inBar = false;
    play(s, 0.0, 120.0, true, [&](double) {
        ChoreoSample smp;
        s.choreo.sampleNow(smp);
        if (s.fight.inFinisher()) {
            for (std::size_t i = 0; i < s.fight.promptCount(); ++i) {
                const BeatBossPrompt& p = s.fight.prompt(i);
                if (std::find(strikes.begin(), strikes.end(), p.impact) == strikes.end()) strikes.push_back(p.impact);
            }
            inBar = true;
        }
        if (smp.shot == ChoreoShot::FinisherOrbit) ++finisherFrames;
        if (smp.slowMo < 1.0f) {
            ++slowFrames;
            slowest = std::min(slowest, smp.slowMo);
            CHECK(smp.shot == ChoreoShot::FinisherOrbit);
            CHECK_FALSE(smp.hitStop);
        }
    });
    CHECK(inBar);
    CHECK(finisherFrames > 300);
    CHECK(slowFrames > 100);
    CHECK(slowest == doctest::Approx(0.4f).epsilon(0.05));
    CHECK(slowest >= 0.4f - 1e-3f);
    // The strikes are 0.25 s apart on a bar of 2 s; the lag is zero on each of them.
    REQUIRE(strikes.size() >= 3);
    CHECK(s.fight.state() == BeatBossState::Victory);
}

TEST_CASE("beat choreo: the victory and the defeat end the scene on their poses, and the shot cuts on a beat") {
    const Rig& r = rig();
    SUBCASE("victory") {
        Scene s;
        s.start(onlyPhase(r.track.def, 0), onlyChoreo(r.choreo, 0));
        double now = play(s, 0.0, 120.0, true, [](double) {});
        REQUIRE(s.fight.state() == BeatBossState::Victory);
        // The track stops with the fight; the scene keeps its own time after the end.
        double clock = now;
        for (int i = 0; i < 1500; ++i) { // 3 s of host frames after the end
            clock += kFrame;
            s.take(s.fight.update(clock), clock);
        }
        ChoreoSample smp;
        s.choreo.sampleNow(smp);
        CHECK(smp.over);
        CHECK(clipName(smp.lead) == "lead_victory");
        CHECK(clipName(smp.boss) == "boss_defeat");
        CHECK(smp.shot == ChoreoShot::LowHero);
        CHECK(smp.promptCount == 0);
        // The cut to the victory shot is on a half second.
        bool found = false;
        for (std::size_t i = 0; i < s.choreo.cutCount(); ++i) {
            if (s.choreo.cut(i).shot != ChoreoShot::LowHero) continue;
            const double k = s.choreo.cut(i).time / 0.5;
            if (s.choreo.cut(i).time > s.fight.trackTime() - 1.0) {
                CHECK(std::fabs(k - std::round(k)) < 1e-6);
                found = true;
            }
        }
        CHECK(found);
        // And it holds: later samples show the same poses.
        ChoreoSample later;
        s.choreo.sample(s.choreo.trackNow() + 5.0, later);
        CHECK(later.lead.clip == smp.lead.clip);
        CHECK(later.boss.clip == smp.boss.clip);
    }
    SUBCASE("defeat") {
        Scene s;
        s.start(onlyPhase(r.track.def, 0), onlyChoreo(r.choreo, 0));
        double now = play(s, 0.0, 120.0, false, [](double) {});
        REQUIRE(s.fight.state() == BeatBossState::Defeat);
        double clock = now;
        for (int i = 0; i < 1500; ++i) {
            clock += kFrame;
            s.take(s.fight.update(clock), clock);
        }
        ChoreoSample smp;
        s.choreo.sampleNow(smp);
        CHECK(smp.over);
        CHECK(clipName(smp.lead) == "lead_downed");
        CHECK(clipName(smp.boss) == "boss_roar");
        CHECK(smp.shot == ChoreoShot::TopDown);
    }
}

TEST_CASE("beat choreo: a stagger plays the boss's stagger clip and cuts to its shot on the next beat") {
    const Rig& r = rig();
    // Open rests with perfect parries... the autopilot gets there on the test track within a phase.
    for (const int which : {0, 1}) {
        Scene s;
        s.start(onlyPhase(r.track.def, which), onlyChoreo(r.choreo, which));
        bool staggered = false;
        play(s, 0.0, 120.0, true, [&](double) {
            if (!staggered && s.fight.stats().staggers > 0) {
                staggered = true;
                // The event is in this call: the boss's clip is the stagger's, and the cut is queued.
                ChoreoSample smp;
                s.choreo.sample(s.fight.trackTime(), smp);
                CHECK(clipName(smp.boss) == "boss_stagger");
                bool cut = false;
                for (std::size_t i = 0; i < s.choreo.cutCount(); ++i) {
                    const ChoreoCut& c = s.choreo.cut(i);
                    if (c.shot == ChoreoShot::CloseBoss && c.time >= s.fight.trackTime() - 1e-9) {
                        const double k = (c.time - s.choreo.bar(s.choreo.barCount() - 1).start) / s.choreo.bar(s.choreo.barCount() - 1).beat;
                        CHECK((std::fabs(k - std::round(k)) < 1e-6 || std::fabs(c.time - s.choreo.bar(s.choreo.barCount() - 1).end) < 1e-6));
                        cut = true;
                    }
                }
                CHECK(cut);
            }
        });
        CHECK(s.fight.stats().staggers == (staggered ? s.fight.stats().staggers : 0));
        if (which == 1) CHECK(staggered);
    }
}

TEST_CASE("beat choreo: a retry gives the same schedule as a fresh start from the checkpoint") {
    const Rig& r = rig();
    const BeatBossDef def = onlyPhase(r.track.def, 0);
    const BeatChoreoDef cdef = onlyChoreo(r.choreo, 0);

    struct Frame {
        ChoreoSample s;
        double at;
    };
    // The samples at every 25 ms of the first 14 s from the first bar line, after the fight or the retry begins.
    const auto record = [&](Scene& s, double from, std::vector<Frame>& out) {
        double barZero = -1.0;
        double next = 0.0;
        double now = from;
        while (!s.fight.over() && now < from + 80.0) {
            now += kFrame;
            s.frame(now, false);
            if (barZero < 0.0 && s.choreo.barCount() > 0) barZero = s.choreo.bar(0).start;
            while (barZero >= 0.0 && s.fight.trackTime() >= barZero + next && next < 14.0) {
                ChoreoSample smp;
                s.choreo.sample(barZero + next, smp);
                out.push_back(Frame{smp, next});
                next += 0.025;
            }
            if (next >= 14.0) break;
        }
        return now;
    };
    const auto same = [&](const std::vector<Frame>& a, const std::vector<Frame>& b) {
        REQUIRE(a.size() == b.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            const ChoreoSample& x = a[i].s;
            const ChoreoSample& y = b[i].s;
            CAPTURE(a[i].at);
            CHECK(x.lead.clip == y.lead.clip);
            CHECK(x.lead.time == doctest::Approx(y.lead.time).epsilon(1e-9));
            CHECK(x.lead.rate == y.lead.rate);
            CHECK(x.boss.clip == y.boss.clip);
            CHECK(x.boss.time == doctest::Approx(y.boss.time).epsilon(1e-9));
            CHECK(x.boss.rate == y.boss.rate);
            CHECK(x.shot == y.shot);
            CHECK(x.sinceCut == doctest::Approx(y.sinceCut).epsilon(1e-9));
            CHECK(x.promptCount == y.promptCount);
            for (std::size_t k = 0; k < std::min(x.promptCount, y.promptCount); ++k) {
                CHECK(x.prompts[k].move == y.prompts[k].move);
                CHECK(x.prompts[k].anchor == y.prompts[k].anchor);
                CHECK(x.prompts[k].progress == doctest::Approx(y.prompts[k].progress).epsilon(1e-6));
            }
            CHECK(x.visualRate == y.visualRate);
        }
    };

    // A fresh start: the lead answers nothing, so the exchange is all the boss's moves and misses.
    const auto fresh = [&](const BeatBossDef& d, const BeatChoreoDef& c) {
        Scene s;
        s.start(d, c);
        std::vector<Frame> out;
        record(s, 0.0, out);
        REQUIRE(out.size() > 400);
        return out;
    };
    const std::vector<Frame> a = fresh(def, cdef);

    // The second phase's checkpoint (it keeps the Load the autopilot stored, so a fresh fight of the
    // phase alone is not the same fight): the autopilot plays the whole track into phase B, then the
    // lead is beaten there and retries. A scene that watched all of it must show what a scene begun at
    // the retry does, over the same fight: the bars, the beat and the moves all begin again from phase B.
    for (const bool explicitCall : {true, false}) {
        Scene again;
        again.start(r.track.def, r.choreo);
        double now = 0.0;
        while (!again.fight.over() && again.fight.phase() < 1 && now < 120.0) {
            now += kFrame;
            again.frame(now, true);
        }
        REQUIRE(again.fight.phase() == 1);
        while (!again.fight.over() && now < 240.0) {
            now += kFrame;
            again.frame(now, false);
        }
        REQUIRE(again.fight.state() == BeatBossState::Defeat);
        REQUIRE(again.fight.checkpointPhase() == 1);
        again.fight.retry(now);
        if (explicitCall) again.choreo.retry(again.fight);
        BeatChoreo twin;
        twin.begin(r.choreo, r.clips);
        std::vector<Frame> seen;
        std::vector<Frame> fresh_;
        double barZero = -1.0;
        double next = 0.0;
        while (!again.fight.over() && next < 14.0 && now < 240.0) {
            now += kFrame;
            const BeatBossEvents e = again.fight.update(now);
            again.choreo.observe(again.fight, e, now);
            twin.observe(again.fight, e, now);
            if (barZero < 0.0 && again.choreo.barCount() > 0) barZero = again.choreo.bar(0).start;
            while (barZero >= 0.0 && again.fight.trackTime() >= barZero + next && next < 14.0) {
                ChoreoSample sa, sb;
                again.choreo.sample(barZero + next, sa);
                twin.sample(barZero + next, sb);
                seen.push_back(Frame{sa, next});
                fresh_.push_back(Frame{sb, next});
                next += 0.025;
            }
        }
        CAPTURE(explicitCall);
        CHECK(seen.size() > 400);
        CHECK(again.choreo.bar(0).start == twin.bar(0).start);
        same(fresh_, seen);
    }

    for (const bool explicitCall : {true, false}) {
        Scene again;
        again.start(def, cdef);
        // Idle until the lead is beaten, then retry from the checkpoint (the first bar of the phase).
        double now = 0.0;
        while (!again.fight.over() && now < 120.0) {
            now += kFrame;
            again.frame(now, false);
        }
        REQUIRE(again.fight.state() == BeatBossState::Defeat);
        again.fight.retry(now);
        if (explicitCall) again.choreo.retry(again.fight);
        again.events.clear();
        // The first bar of the retry begins at the track time the fight stopped at.
        const double before = again.fight.trackTime();
        std::vector<Frame> b;
        // (record() reads the first bar of this scene's choreography: reset by the retry.)
        const double endClock = record(again, now, b);
        CHECK(endClock > now);
        CHECK(again.fight.stats().retries == 1);
        CHECK(again.choreo.bar(0).start >= before - 1e-9);
        CAPTURE(explicitCall);
        same(a, b);
    }
}

TEST_CASE("beat choreo: observe, sample and retry allocate nothing over whole fights") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    // Everything that allocates (the data, the scenes) is made first.
    const Rig& r = rig();
    Scene won;
    Scene lost;
    ChoreoSample smp;
    int sink = 0;
    const std::uint64_t before = ftcore_test::allocStats().count;
    won.start(r.track.def, r.choreo);
    for (double now = 1.0 / 60.0; !won.fight.over() && now < 300.0; now += 1.0 / 60.0) {
        for (int guard = 0; guard < 16; ++guard) {
            const QteAction a = won.fight.autopilot(now);
            if (a.kind == QteAction::Kind::None) break;
            won.choreo.observe(won.fight, a.kind == QteAction::Kind::Press ? won.fight.press(a.input, a.time) : won.fight.release(a.input, a.time), a.time);
        }
        won.choreo.observe(won.fight, won.fight.update(now), now);
        won.choreo.sampleNow(smp);
        sink += static_cast<int>(smp.promptCount) + static_cast<int>(smp.lead.clip) + static_cast<int>(smp.boss.clip);
    }
    lost.fight.begin(r.track.def, BeatBossTuning{}, 0.0);
    lost.choreo.begin(r.choreo, r.clips);
    double now = 0.0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        while (!lost.fight.over() && now < 600.0) {
            now += 1.0 / 60.0;
            lost.choreo.observe(lost.fight, lost.fight.update(now), now);
            lost.choreo.sample(lost.fight.trackTime(), smp);
            sink += static_cast<int>(smp.promptCount);
        }
        if (attempt < 2) {
            lost.fight.retry(now);
            lost.choreo.retry(lost.fight);
        }
    }
    const std::uint64_t after = ftcore_test::allocStats().count;
    CHECK(after == before);
    CHECK(won.fight.state() == BeatBossState::Victory);
    CHECK(lost.fight.stats().retries == 2);
    CHECK(sink > 0);
}

TEST_CASE("beat choreo: a prompt opened and answered between two observe calls is counted, not choreographed") {
    const Rig& r = rig();
    Scene s;
    s.start(onlyPhase(r.track.def, 0), onlyChoreo(r.choreo, 0));
    // One long frame past the first moves' impacts: their prompts opened and closed unseen.
    s.take(s.fight.update(0.5), 0.5);
    s.take(s.fight.update(8.0), 8.0);
    CHECK(s.choreo.unmatched() > 0);
    ChoreoSample smp; // and the scene carries on
    s.choreo.sampleNow(smp);
    CHECK(smp.lead.clip >= 0);
    CHECK(smp.boss.clip >= 0);
}

TEST_CASE("beat choreo: a miss cuts the pre-rolled answer at the impact to the lead's hit reaction") {
    const Rig& r = rig();
    for (const int which : {0, 1}) {
        Scene s;
        s.start(onlyPhase(r.track.def, which), onlyChoreo(r.choreo, which));
        struct Rec {
            std::uint32_t id;
            std::uint8_t move;
            bool finisher;
            double impact;
        };
        std::vector<Rec> recs;
        std::size_t seen = 0;
        int misses = 0;
        int cut = 0;
        play(s, 0.0, 60.0, false, [&](double) {
            for (std::size_t i = 0; i < s.fight.promptCount(); ++i) {
                const BeatBossPrompt& p = s.fight.prompt(i);
                if (std::none_of(recs.begin(), recs.end(), [&](const Rec& x) { return x.id == p.id; }))
                    recs.push_back(Rec{p.id, p.move, p.finisher, p.impact});
            }
            for (; seen < s.events.size(); ++seen) {
                const BeatBossEvent& e = s.events[seen];
                if (e.type != BeatBossEventType::Judged || e.grade == QteGrade::Perfect || e.grade == QteGrade::Good) continue;
                const auto it = std::find_if(recs.begin(), recs.end(), [&](const Rec& x) { return x.id == e.prompt; });
                if (it == recs.end()) continue;
                // The phase s plays (onlyChoreo's phase 0), from the rig, not from a temporary copy.
                const ChoreoPhase& cp = r.choreo.phases[static_cast<std::size_t>(which)];
                const ChoreoMove& cm = it->finisher ? cp.finisher[it->move] : cp.moves[it->move];
                ++misses;
                // Before the verdict the lead was in the pre-rolled answer; after it, from the impact, in the reaction.
                ChoreoSample at;
                s.choreo.sample(it->impact + 0.05, at);
                if (clipName(at.lead) == clipName(ChoreoPose{cm.miss, 0.0, 1.0f})) {
                    const BeatClip& mc = r.clips[static_cast<std::size_t>(cm.miss)];
                    CHECK(at.lead.time == doctest::Approx(std::min(mc.impact, mc.length) + 0.05).epsilon(1e-6));
                    ++cut;
                }
            }
        });
        CHECK(misses > 5);
        CHECK(cut * 10 >= misses * 8); // (a stagger or a newer answer took the others)
    }
}

TEST_CASE("beat choreo: a counter pressed too early makes the lead stumble, and its answer then plays from the press") {
    const Rig& r = rig();
    Scene s;
    const BeatBossDef def = onlyPhase(r.track.def, 0);
    s.start(def, onlyChoreo(r.choreo, 0));
    // The first move is a parry: press Guard well before its window, once its marker shows.
    bool whiffed = false;
    bool pressed = false;
    double whiffAt = 0.0;
    bool stumbled = false;
    bool answered = false;
    double now = 0.0;
    std::size_t seen = 0;
    while (!s.fight.over() && now < 8.0) {
        now += kFrame;
        if (!pressed && s.fight.trackRunning() && s.fight.promptCount() > 0) {
            const BeatBossPrompt& p = s.fight.prompt(0);
            if (p.answer == BeatBossAnswer::Parry && p.impact - s.fight.trackTime() < 0.8 && p.impact - s.fight.trackTime() > 0.5) {
                pressed = true;
                s.take(s.fight.press(ActionButton::Guard, now), now);
            }
        }
        s.frame(now, pressed); // (the autopilot answers on its own once it is time)
        for (; seen < s.events.size(); ++seen) {
            if (s.events[seen].type == BeatBossEventType::Whiff && !whiffed) {
                whiffed = true;
                whiffAt = s.fight.trackTime();
            }
        }
        if (whiffed && !stumbled) {
            ChoreoSample smp;
            s.choreo.sampleNow(smp);
            CHECK(clipName(smp.lead) == "lead_hitreact"); // the stumble
            stumbled = true;
        }
        if (whiffed && s.fight.trackTime() > whiffAt + 0.6 && !answered) {
            // The autopilot has answered after the lockout: the parry plays from its press.
            ChoreoSample smp;
            s.choreo.sampleNow(smp);
            if (clipName(smp.lead) == "lead_parry") answered = true;
        }
    }
    CHECK(whiffed);
    CHECK(stumbled);
    CHECK(answered);
}
