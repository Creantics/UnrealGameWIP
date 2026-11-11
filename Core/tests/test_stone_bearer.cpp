#include "ftcore/StoneBearer.h"

#include "ftcore/ArenaFight.h"
#include "ftcore/BeatChoreo.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace ftcore;

// The first Arena boss's own rules (StoneBearer.h) on top of the generic pieces: the weight stacks,
// the rear weak point, the breakable, the pedestal, and the shipped Data/Battle/arena_boss/stone_bearer.json.

namespace {

constexpr double kDt = 1.0 / 60.0;

const StoneBearerDef& shipped() {
    static StoneBearerDef def;
    static bool loaded = false;
    if (!loaded) {
        std::string text;
        REQUIRE(ftcore_test::readProjectData("Battle/arena_boss/stone_bearer.json", text));
        std::string error;
        REQUIRE_MESSAGE(importStoneBearer(text, def, &error), error);
        loaded = true;
    }
    return def;
}

std::uint8_t move(const char* id) {
    const std::uint8_t m = shipped().boss.find(id);
    REQUIRE(m != kArenaBossNoMove);
    return m;
}

int countOf(const ArenaBossEvents& ev, ArenaBossEventType type) {
    int n = 0;
    for (const ArenaBossEvent& e : ev) n += e.type == type ? 1 : 0;
    return n;
}

const ArenaBossEvent* firstOf(const ArenaBossEvents& ev, ArenaBossEventType type) {
    for (const ArenaBossEvent& e : ev)
        if (e.type == type) return &e;
    return nullptr;
}

// A small file with every key the boss's own loader reads; the cases below edit it.
const char* const kMini = R"({
  "version": 1, "bpm": 120, "hp": 100, "poise": 10, "staggerTime": 1, "openTime": 1,
  "moves": [ { "id": "jab", "answer": "any", "impacts": [1], "active": 0.2, "recovery": 0.5, "range": 2,
               "damage": 0.1 } ],
  "phases": [ { "endAt": 0, "moves": ["jab"] } ],
  "weight": { "cap": 5, "damagePerStack": 0.1, "defencePerStack": 0.1, "shockRingAt": 3, "poiseFreezeAt": 5,
              "guard": 1, "soloLift": 1, "interceptShed": 1, "critShed": 1 },
  "breakable": { "at": 0.5 },
  "pedestal": { "at": 0.25, "firstCallBeats": 4, "callEveryBeats": 4, "releaseWindow": 0.15, "downTime": 3.0 }
})";

std::string edited(const std::string& from, const std::string& to) {
    std::string s = kMini;
    const std::size_t at = s.find(from);
    REQUIRE(at != std::string::npos);
    s.replace(at, from.size(), to);
    return s;
}

std::string refusal(const std::string& json) {
    StoneBearerDef def;
    std::string error;
    CHECK_FALSE(importStoneBearer(json, def, &error));
    CHECK(def.boss.moveCount == 0); // out is default on failure
    return error;
}

// Plays the boss from `begin` until `until`, frame by frame, at `hp`.
void play(StoneBearer& b, double from, double until, float hp) {
    for (double t = from; t < until; t += kDt) b.update(t, hp);
    b.update(until, hp);
}

} // namespace

TEST_CASE("weight stacks: kept in 0..cap, with their scales and thresholds") {
    WeightStacks w;
    w.reset(WeightTuning{});
    CHECK(w.stacks() == 0);
    CHECK(w.damageScale() == doctest::Approx(1.0));
    CHECK(w.takenScale() == doctest::Approx(1.0));
    CHECK(w.change(-1) == 0); // nothing to shed
    CHECK(w.change(3) == 3);
    CHECK(w.damageScale() == doctest::Approx(1.3));
    CHECK(w.takenScale() == doctest::Approx(0.7));
    CHECK(w.shockRing());
    CHECK_FALSE(w.poiseFrozen());
    CHECK(w.change(4) == 2); // capped at 5
    CHECK(w.stacks() == 5);
    CHECK(w.poiseFrozen());
    CHECK(w.takenScale() == doctest::Approx(0.5));
    CHECK(w.change(-9) == -5);
    CHECK(w.stacks() == 0);
    CHECK_FALSE(w.shockRing());

    WeightTuning heavy;
    heavy.defencePerStack = 0.3f; // the loader refuses this; the scale still never goes below 0
    w.reset(heavy);
    w.change(5);
    CHECK(w.takenScale() == doctest::Approx(0.0));
}

TEST_CASE("pedestal phase: once at the hp share, a Release on a call lifts it") {
    GuestCaller guest;
    guest.reset(120.0, GuestCallTuning{}); // a beat is 0.5 s
    PedestalPhase p;
    p.reset(PedestalTuning{});
    CHECK_FALSE(p.holding());
    CHECK(p.release(guest, 1.0, 90) == PedestalRelease::NotHolding);
    CHECK_FALSE(p.update(0.26f, 9.0));
    CHECK(p.update(0.25f, 10.0));
    CHECK_FALSE(p.update(0.1f, 11.0)); // once
    CHECK(p.holding());
    CHECK(p.from() == doctest::Approx(10.0));
    CHECK(p.firstCall(guest) == doctest::Approx(12.0)); // 4 beats on
    CHECK(p.nextCall(guest, 12.1) == doctest::Approx(14.0));
    CHECK(p.nearestCall(guest, 12.9) == doctest::Approx(12.0));
    CHECK(p.nearestCall(guest, 13.0) == doctest::Approx(12.0)); // a tie: the earlier
    CHECK(p.nearestCall(guest, 13.1) == doctest::Approx(14.0));

    CHECK(p.release(guest, 10.5, 90) == PedestalRelease::OffBeat); // before the first call's window
    CHECK(p.release(guest, 12.2, 90) == PedestalRelease::OffBeat); // 0.2 s late
    CHECK(p.release(guest, 13.9, kArenaReleaseOpenLoad) == PedestalRelease::LowLoad); // a third is not above it
    CHECK(p.holding()); // misses cost nothing
    CHECK(p.release(guest, 13.9, kArenaReleaseOpenLoad + 1) == PedestalRelease::Lifted);
    CHECK_FALSE(p.holding());
    CHECK(p.lifted());
    CHECK(p.downUntil() == doctest::Approx(16.9));
    CHECK(p.down(16.8));
    CHECK_FALSE(p.down(16.9));
    CHECK(p.release(guest, 15.9, 90) == PedestalRelease::NotHolding);

    SUBCASE("the window is either side of the call") {
        PedestalPhase q;
        q.reset(PedestalTuning{});
        q.update(0.2f, 10.0);
        CHECK(q.release(guest, 11.86, 90) == PedestalRelease::Lifted); // 0.14 s early
    }
    SUBCASE("the miss shift moves the calls") {
        CHECK(guest.onMarkedMiss());
        PedestalPhase q;
        q.reset(PedestalTuning{});
        q.update(0.2f, 10.0);
        CHECK(q.firstCall(guest) == doctest::Approx(12.25));
        CHECK(q.release(guest, 12.0, 90) == PedestalRelease::OffBeat);
        CHECK(q.release(guest, 14.3, 90) == PedestalRelease::Lifted);
    }
}

TEST_CASE("stone bearer data: the shipped file loads clean, with the outline's timings") {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Battle/arena_boss/stone_bearer.json", text));
    StoneBearerDef def;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importStoneBearer(text, def, &error, &warnings), error);
    CHECK(warnings.empty());
    const ArenaBossDef& b = def.boss;
    CHECK(b.bpm == doctest::Approx(76.0));
    CHECK(b.moveCount == 15);
    CHECK(b.phaseCount == 2);

    const ArenaBossMove& fist = b.moves[b.find("called_fist")];
    CHECK(fist.intercept());
    REQUIRE(fist.impactCount == 1);
    CHECK(fist.impacts[0] == doctest::Approx(3.0));
    CHECK(fist.calls[0] == doctest::Approx(1.0));
    CHECK(fist.zones[0] == CalledZoneRule::Side);
    CHECK(fist.sheds[0]);

    const ArenaBossMove& twice = b.moves[b.find("called_fist_twice")];
    REQUIRE(twice.impactCount == 2);
    CHECK(twice.impacts[0] == doctest::Approx(3.0));
    CHECK(twice.impacts[1] == doctest::Approx(4.0));
    CHECK(twice.calls[0] == doctest::Approx(1.0));
    CHECK(twice.calls[1] == doctest::Approx(2.0));
    CHECK(twice.zones[0] == CalledZoneRule::Side);
    CHECK(twice.zones[1] == CalledZoneRule::Other);

    const ArenaBossMove& thrown = b.moves[b.find("marked_throw")];
    REQUIRE(thrown.impactCount == 1);
    CHECK(thrown.impacts[0] == doctest::Approx(4.0));
    CHECK(thrown.marked);
    CHECK(thrown.zones[0] == CalledZoneRule::Path);
    CHECK(thrown.load == 30);
    CHECK(thrown.opens == doctest::Approx(4.0));
    CHECK(thrown.guardBreak > 0.0f);
    const ArenaBossMove& halves = b.moves[b.find("marked_throw_halves")];
    REQUIRE(halves.impactCount == 2);
    CHECK(halves.impacts[0] == doctest::Approx(4.0));
    CHECK(halves.impacts[1] == doctest::Approx(5.0));
    for (const char* id : {"slab_drive_halves", "low_sweep_halves"}) {
        const ArenaBossMove& m = b.moves[b.find(id)];
        REQUIRE(m.impactCount == 2);
        CHECK(m.impacts[0] == doctest::Approx(2.0));
        CHECK(m.impacts[1] == doctest::Approx(3.0));
    }

    // The nape: one point, behind (60 degrees either side), x2, on the lower back (3.1 m of the figure's 5.1 m).
    REQUIRE(b.weakPoints.points.size() == 1);
    CHECK(b.weakPoints.points.items[0].id() == "nape");
    CHECK(b.weakPoints.points.items[0].multiplier == doctest::Approx(2.0));
    CHECK(b.weakPoints.gates[0].facing == doctest::Approx(180.0));
    CHECK(b.weakPoints.gates[0].arc == doctest::Approx(60.0));
    CHECK(b.weakPoints.points.halfWidth == doctest::Approx(0.45));
    CHECK(b.weakPoints.points.items[0].x == doctest::Approx(0.0));
    CHECK(b.weakPoints.points.items[0].y == doctest::Approx(0.61));
    CHECK(b.weakPoints.points.items[0].radius == doctest::Approx(0.1));
    // The keep-out: 3 m round the guest's spot, 20 m out at bearing 90.
    CHECK(b.keepOutBearing == doctest::Approx(90.0));
    CHECK(b.keepOutDistance == doctest::Approx(20.0));
    CHECK(b.keepOutRadius == doctest::Approx(3.0));

    // The marked throw is phase 2's; the break swaps in the halves and drops the block throw.
    const ArenaBossPhase& p1 = b.phases[0];
    const ArenaBossPhase& p2 = b.phases[1];
    CHECK(p1.endAt == doctest::Approx(0.5));
    CHECK(p1.whole.contains(b.find("block_throw")));
    CHECK_FALSE(p1.broken.contains(b.find("block_throw")));
    CHECK_FALSE(p1.whole.contains(b.find("marked_throw")));
    CHECK(p1.broken.contains(b.find("slab_drive_halves")));
    CHECK(p2.whole.contains(b.find("marked_throw")));
    CHECK(p2.whole.contains(b.find("called_fist_twice")));
    CHECK(p2.broken.contains(b.find("marked_throw_halves")));
    CHECK_FALSE(p2.broken.contains(b.find("marked_throw")));

    // The stack costs. A Guard held against a blow costs 1, except the single side blow whose Guard
    // costs 2; so a two-blow side move guarded twice costs 1 + 1. A miss costs its move's missStacks
    // once per use: 1 for the side blows, 2 for the marked throw, whole or in halves.
    CHECK(fist.guardStacks == 2);
    CHECK(fist.missStacks == 1);
    CHECK(twice.guardStacks == 1);
    CHECK(twice.missStacks == 1);
    CHECK(thrown.guardStacks == 1);
    CHECK(thrown.missStacks == 2);
    CHECK(halves.guardStacks == 1);
    CHECK(halves.missStacks == 2);
    CHECK(b.moves[b.find("ground_punch")].guardStacks == 1);
    CHECK(b.moves[b.find("ground_punch")].missStacks == 0);

    // The body and the generic tuning.
    CHECK(b.hp == 1400);
    CHECK(b.poise == doctest::Approx(175.0));
    CHECK(b.staggerTime == doctest::Approx(2.0));
    CHECK(b.openTime == doctest::Approx(3.0));
    CHECK(b.intercept.window == doctest::Approx(0.15));
    CHECK(b.intercept.rearm == doctest::Approx(0.4));
    CHECK(b.guestCall.fromBeats == doctest::Approx(1.0));
    CHECK(b.guestCall.minLeadBeats == doctest::Approx(2.0));
    CHECK(b.guestCall.missShiftBeats == doctest::Approx(0.5));

    // Every move: impacts (beats), active, recovery, range, damage, damageMax, opens.
    struct Row {
        const char* id;
        ArenaBossAnswer answer;
        float first, second; // impact beats (second 0: one impact)
        float active, recovery, range, damage, damageMax, opens;
    };
    const Row rows[] = {
        {"called_fist", ArenaBossAnswer::Intercept, 3, 0, 0.2f, 1.2f, 2.6f, 0.12f, 0.12f, 3},
        {"called_fist_twice", ArenaBossAnswer::Intercept, 3, 4, 0.2f, 3.0f, 2.6f, 0.12f, 0.24f, 3},
        {"slab_drive", ArenaBossAnswer::Dodge, 2, 0, 0.3f, 1.6f, 4.5f, 0.14f, 0.14f, 0},
        {"slab_drive_halves", ArenaBossAnswer::Dodge, 2, 3, 0.3f, 1.6f, 4.5f, 0.08f, 0.16f, 0},
        {"debris_shower", ArenaBossAnswer::Tile, 3, 0, 1.0f, 1.6f, 10.0f, 0.03f, 0.12f, 0},
        {"debris_shower_twice", ArenaBossAnswer::Tile, 3, 4, 1.0f, 1.6f, 10.0f, 0.03f, 0.12f, 0},
        {"low_sweep", ArenaBossAnswer::Jump, 2, 0, 0.3f, 1.6f, 4.5f, 0.10f, 0.10f, 0},
        {"low_sweep_halves", ArenaBossAnswer::Jump, 2, 3, 0.3f, 1.6f, 4.5f, 0.10f, 0.20f, 0},
        {"ground_punch", ArenaBossAnswer::Intercept, 4, 0, 0.2f, 2.0f, 1.5f, 0.14f, 0.14f, 3},
        {"block_throw", ArenaBossAnswer::Cover, 3, 0, 0.3f, 1.3f, 20.0f, 0.12f, 0.12f, 0},
        {"marked_throw", ArenaBossAnswer::Intercept, 4, 0, 0.3f, 1.4f, 24.0f, 0.14f, 0.14f, 4},
        {"marked_throw_halves", ArenaBossAnswer::Intercept, 4, 5, 0.3f, 1.4f, 24.0f, 0.09f, 0.18f, 4},
        {"edge_jab", ArenaBossAnswer::Any, 1, 0, 0.15f, 0.8f, 2.0f, 0.06f, 0.06f, 0},
        {"stamp", ArenaBossAnswer::Any, 1, 0, 0.15f, 0.8f, 2.0f, 0.06f, 0.06f, 0},
        {"backhand", ArenaBossAnswer::Any, 1, 0, 0.15f, 0.8f, 2.2f, 0.06f, 0.06f, 0},
    };
    REQUIRE(std::size(rows) == b.moveCount);
    for (const Row& r : rows) {
        CAPTURE(r.id);
        const std::uint8_t i = b.find(r.id);
        REQUIRE(i != kArenaBossNoMove);
        const ArenaBossMove& m = b.moves[i];
        CHECK(m.answer == r.answer);
        REQUIRE(m.impactCount == (r.second > 0 ? 2 : 1));
        CHECK(m.impacts[0] == doctest::Approx(r.first));
        if (r.second > 0) CHECK(m.impacts[1] == doctest::Approx(r.second));
        CHECK(m.active == doctest::Approx(r.active));
        CHECK(m.recovery == doctest::Approx(r.recovery));
        CHECK(m.range == doctest::Approx(r.range));
        CHECK(m.damage == doctest::Approx(r.damage));
        CHECK(m.damageMax == doctest::Approx(r.damageMax));
        CHECK(m.opens == doctest::Approx(r.opens));
        CHECK(m.floor == (std::string_view(r.id) == "ground_punch" || std::string_view(r.id) == "stamp"));
    }

    // Every move's calls, zones, sheds, mark, guard break and Load, exactly.
    const auto num = [](double v) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", v);
        return std::string(buf);
    };
    const auto rules = [&](const ArenaBossMove& m) {
        std::string s = "calls";
        for (std::size_t i = 0; i < m.callCount; ++i) s += " " + num(m.calls[i]);
        if (m.intercept()) {
            static const char* const kZones[] = {"side", "other", "path"};
            s += " zones";
            for (std::size_t i = 0; i < m.impactCount; ++i) s += std::string(" ") + kZones[int(m.zones[i])];
            s += " sheds";
            for (std::size_t i = 0; i < m.impactCount; ++i) s += m.sheds[i] ? " 1" : " 0";
        }
        if (m.marked) s += " marked";
        if (m.guardBreak > 0.0f) s += " break " + num(m.guardBreak);
        if (m.load > 0) s += " load " + std::to_string(m.load);
        return s;
    };
    const std::pair<const char*, const char*> kRules[] = {
        {"called_fist", "calls 1 zones side sheds 1"},
        {"called_fist_twice", "calls 1 2 zones side other sheds 0 1"},
        {"slab_drive", "calls"},
        {"slab_drive_halves", "calls"},
        {"debris_shower", "calls 1"},
        {"debris_shower_twice", "calls 1 2"},
        {"low_sweep", "calls"},
        {"low_sweep_halves", "calls"},
        {"ground_punch", "calls 2 zones side sheds 1"},
        {"block_throw", "calls"},
        {"marked_throw", "calls 2 zones path sheds 1 marked break 1.2 load 30"},
        {"marked_throw_halves", "calls 2 3 zones path path sheds 0 1 marked break 1.2 load 30"},
        {"edge_jab", "calls"},
        {"stamp", "calls"},
        {"backhand", "calls"},
    };
    REQUIRE(std::size(kRules) == b.moveCount);
    for (const auto& [id, want] : kRules) {
        CAPTURE(id);
        REQUIRE(b.find(id) != kArenaBossNoMove);
        CHECK(rules(b.moves[b.find(id)]) == want);
    }

    // The tables, exactly. A break in phase 1 keeps the single shower (it doubles only in phase 2).
    const auto ids = [&](const ArenaBossMoveTable& t) {
        std::string s;
        for (const std::uint8_t m : t) s += std::string(b.moves[m].id()) + " ";
        return s;
    };
    CHECK(ids(p1.whole) ==
          "called_fist slab_drive debris_shower low_sweep ground_punch block_throw edge_jab stamp backhand ");
    CHECK(ids(p1.broken) ==
          "called_fist slab_drive_halves debris_shower low_sweep_halves ground_punch edge_jab stamp backhand ");
    CHECK(ids(p2.whole) ==
          "called_fist_twice slab_drive debris_shower_twice low_sweep marked_throw edge_jab stamp backhand ");
    CHECK(ids(p2.broken) == "called_fist_twice slab_drive_halves debris_shower_twice low_sweep_halves "
                            "marked_throw_halves edge_jab stamp backhand ");

    CHECK(def.weight.cap == 5);
    CHECK(def.weight.damagePerStack == doctest::Approx(0.1));
    CHECK(def.weight.defencePerStack == doctest::Approx(0.1));
    CHECK(def.weight.shockRingAt == 3);
    CHECK(def.weight.poiseFreezeAt == 5);
    CHECK(def.weight.guard == 1);
    CHECK(def.weight.soloLift == 1);
    CHECK(def.weight.interceptShed == 1);
    CHECK(def.weight.critShed == 1);
    CHECK(def.breakable.at == doctest::Approx(0.4)); // inside phase 2, above the pedestal
    CHECK(def.breakable.at < b.phases[0].endAt);
    CHECK(def.breakable.at > def.pedestal.at);
    CHECK(def.pedestal.at == doctest::Approx(0.25));
    CHECK(def.pedestal.firstCallBeats == doctest::Approx(4.0));
    CHECK(def.pedestal.callEveryBeats == doctest::Approx(4.0));
    CHECK(def.pedestal.releaseWindow == doctest::Approx(0.15));
    CHECK(def.pedestal.downTime == doctest::Approx(3.0));
}

TEST_CASE("stone bearer clips: the shipped table loads clean, with its own crossfade") {
    std::string text, err;
    std::vector<std::string> warnings;
    REQUIRE(ftcore_test::readProjectData("Anim/stone_bearer_clips.json", text));
    ftcore::BeatClips clips;
    REQUIRE_MESSAGE(ftcore::importBeatClips(text, clips, &err, &warnings), err);
    CHECK(warnings.empty());
    // A 5 m stone body fades between clips slower than the host's default.
    CHECK(clips.blend == doctest::Approx(0.25));
    for (const char* id : {"idle", "idle_p2", "stagger", "face_down", "defeat", "hit_light", "hit_heavy"})
        CHECK_MESSAGE(clips.find(id) >= 0, id);
    // Its walk loops and states the speed it was made at, which the host plays it against: about the kind's 0.8 m/s
    // walking speed, so it plays near its own rate.
    const int walk = clips.find("walk");
    REQUIRE(walk >= 0);
    CHECK(clips[static_cast<std::size_t>(walk)].loop);
    CHECK(clips[static_cast<std::size_t>(walk)].speed == doctest::Approx(0.805));
    CHECK(clips[static_cast<std::size_t>(walk)].length == doctest::Approx(2.667));
    // The go-down into face_down, the get-up out of it and the death on its knees play once, each its own authored length.
    for (const auto& [id, length] : {std::pair{"face_down_in", 0.8}, std::pair{"face_down_out", 1.0}, std::pair{"defeat_kneel", 6.0}}) {
        const int clip = clips.find(id);
        REQUIRE_MESSAGE(clip >= 0, id);
        CHECK_FALSE(clips[static_cast<std::size_t>(clip)].loop);
        CHECK(clips[static_cast<std::size_t>(clip)].length == doctest::Approx(length));
    }
    // The stepping turns loop one 0.8 s cycle each.
    for (const char* id : {"turn_l", "turn_r"}) {
        const int clip = clips.find(id);
        REQUIRE_MESSAGE(clip >= 0, id);
        CHECK(clips[static_cast<std::size_t>(clip)].loop);
        CHECK(clips[static_cast<std::size_t>(clip)].length == doctest::Approx(0.8));
    }
    // The called blows' left-hand clips are their mirrors: the same length, the impact on the same frame.
    for (const char* id : {"called_fist", "ground_punch"}) {
        const int right = clips.find(id);
        const int left = clips.find(std::string(id) + "_l");
        REQUIRE_MESSAGE(right >= 0, id);
        REQUIRE_MESSAGE(left >= 0, id);
        const ftcore::BeatClip& r = clips[static_cast<std::size_t>(right)];
        const ftcore::BeatClip& l = clips[static_cast<std::size_t>(left)];
        CHECK_FALSE(l.loop);
        CHECK(l.length == doctest::Approx(r.length));
        CHECK(l.impact == doctest::Approx(r.impact));
    }
}

TEST_CASE("stone bearer loader: refusals carry their paths") {
    {
        StoneBearerDef def;
        std::string error;
        std::vector<std::string> warnings;
        CHECK(importStoneBearer(kMini, def, &error, &warnings));
        CHECK(warnings.empty()); // weight, breakable and pedestal are not unknown keys
        CHECK(def.weight.guard == 1);
        CHECK(def.breakable.at == doctest::Approx(0.5));
        CHECK(def.pedestal.downTime == doctest::Approx(3.0));
    }
    CHECK(refusal(edited(R"("weight": {)", R"("heft": {)")).find("weight: expected an object") != std::string::npos);
    CHECK(refusal(edited(R"("pedestal": {)", R"("stand": {)")).find("pedestal: expected an object") !=
          std::string::npos);
    CHECK(refusal(edited(R"("breakable": {)", R"("crack": {)")).find("breakable: expected an object") !=
          std::string::npos);
    CHECK(refusal(edited(R"("breakable": { "at": 0.5 })", R"("breakable": { "at": 1 })")) ==
          "breakable.at: expected a number above 0 and below 1");
    CHECK(refusal(edited(R"("breakable": { "at": 0.5 })", R"("breakable": { "at": "half" })")) ==
          "breakable.at: expected a number in 0..1");
    CHECK(refusal(edited(R"("breakable": { "at": 0.5 })", R"("breakable": {})")) ==
          "breakable.at: expected a number in 0..1");
    // The piece breaks before the boss climbs: at or below the pedestal's share is refused.
    CHECK(refusal(edited(R"("breakable": { "at": 0.5 })", R"("breakable": { "at": 0.25 })")) ==
          "breakable.at: expected above pedestal.at");
    CHECK(refusal(edited(R"("breakable": { "at": 0.5 })", R"("breakable": { "at": 0.1 })")) ==
          "breakable.at: expected above pedestal.at");
    // A refused pedestal skips the cross-check: one error, the pedestal's.
    CHECK(refusal(edited(R"("at": 0.25)", R"("at": 0)")) == "pedestal.at: expected a number above 0 and below 1");
    CHECK(refusal(edited(R"("cap": 5)", R"("cap": 0)")).find("weight.cap: expected an integer in 1..10") !=
          std::string::npos);
    CHECK(refusal(edited(R"("shockRingAt": 3)", R"("shockRingAt": 6)")) ==
          "weight.shockRingAt: expected at most weight.cap");
    CHECK(refusal(edited(R"("poiseFreezeAt": 5)", R"("poiseFreezeAt": 6)")) ==
          "weight.poiseFreezeAt: expected at most weight.cap");
    CHECK(refusal(edited(R"("defencePerStack": 0.1)", R"("defencePerStack": 0.2)")) ==
          "weight.defencePerStack: expected weight.cap x it below 1");
    CHECK(refusal(edited(R"("guard": 1)", R"("guard": 6)")) == "weight.guard: expected an integer in 0..5");
    CHECK(refusal(edited(R"("critShed": 1)", R"("critShed": 0.5)")) == "weight.critShed: expected an integer in 0..5");
    CHECK(refusal(edited(R"("damagePerStack": 0.1)", R"("damagePerStack": "lots")")) ==
          "weight.damagePerStack: expected a number in 0..0.5");
    CHECK(refusal(edited(R"("at": 0.25)", R"("at": 1)")) == "pedestal.at: expected a number above 0 and below 1");
    CHECK(refusal(edited(R"("at": 0.25)", R"("at": 0)")) == "pedestal.at: expected a number above 0 and below 1");
    CHECK(refusal(edited(R"("callEveryBeats": 4)", R"("callEveryBeats": 0.5)")) ==
          "pedestal.callEveryBeats: expected a number in 1..16");
    CHECK(refusal(edited(R"("downTime": 3.0)", R"("downTime": 11)")) ==
          "pedestal.downTime: expected a number in 0..10");
    // At 120 bpm one beat is 0.5 s: a 0.3 s window either side would cover the whole gap.
    CHECK(refusal(edited(R"("callEveryBeats": 4, "releaseWindow": 0.15)",
                         R"("callEveryBeats": 1, "releaseWindow": 0.3)")) ==
          "pedestal.releaseWindow: expected below half of pedestal.callEveryBeats");
    // The generic loader's errors and the boss's own come back together, one per line.
    {
        std::string s = edited(R"("bpm": 120)", R"("bpm": 10)");
        const std::size_t at = s.find(R"("guard": 1)");
        s.replace(at, 10, R"("guard": 9)");
        CHECK(refusal(s) == "bpm: expected a number in 40..240\nweight.guard: expected an integer in 0..5");
    }
    CHECK(refusal("{ nope").find("arena boss:") == 0);
    CHECK(refusal("[1, 2]").find("arena boss: expected an object") == 0);

    // Unknown keys are warnings, inside the boss's objects as at the top.
    StoneBearerDef def;
    std::string error;
    std::vector<std::string> warnings;
    std::string s = edited(R"("cap": 5,)", R"("cap": 5, "heft": 2,)");
    s = s.replace(s.find(R"("downTime")"), 10, R"("tempo": 1, "downTime")");
    s = s.replace(s.find(R"("version")"), 9, R"("mood": 1, "version")");
    s = s.replace(s.find(R"("at": 0.5)"), 9, R"("at": 0.5, "when": 2)");
    CHECK(importStoneBearer(s, def, &error, &warnings));
    REQUIRE(warnings.size() == 4);
    CHECK(warnings[0] == "unknown key: mood");
    CHECK(warnings[1] == "weight: unknown key: heft");
    CHECK(warnings[2] == "pedestal: unknown key: tempo");
    CHECK(warnings[3] == "breakable: unknown key: when");
}

TEST_CASE("stone bearer: guards gain stacks, intercepts and the first crit of an opening shed them") {
    const StoneBearerDef& def = shipped();
    const double B = def.boss.beat();
    const std::uint8_t fist = move("called_fist");
    StoneBearer b;
    b.begin(def, 0.0);
    CHECK(b.stacks() == 0);

    // Guarded: +2 (the move's guardStacks).
    bool started = false;
    b.startMove(fist, 1.0, CalledZone::Left, &started);
    REQUIRE(started);
    const double T1 = 1.0 + 3 * B;
    b.guardDown(CalledZone::Left, T1 - 0.5);
    {
        const ArenaBossEvents& e = b.update(T1 + 0.01, 1.0f);
        const ArenaBossEvent* a = firstOf(e, ArenaBossEventType::Answered);
        REQUIRE(a);
        CHECK(a->outcome == CalledOutcome::Guarded);
        const ArenaBossEvent* s = firstOf(e, ArenaBossEventType::StacksChanged);
        REQUIRE(s);
        CHECK(s->value == 2);
        CHECK(s->amount == 2);
        CHECK(s->at == doctest::Approx(T1));
        CHECK(countOf(e, ArenaBossEventType::ShockRing) == 0);
    }
    b.guardUp(T1 + 0.1);

    // Again: 4, past the ring threshold.
    b.startMove(fist, 5.0, CalledZone::Left);
    const double T2 = 5.0 + 3 * B;
    b.guardDown(CalledZone::Left, T2 - 0.5);
    {
        const ArenaBossEvents& e = b.update(T2 + 0.01, 1.0f);
        const ArenaBossEvent* r = firstOf(e, ArenaBossEventType::ShockRing);
        REQUIRE(r);
        CHECK(r->value == 1);
    }
    b.guardUp(T2 + 0.1);
    CHECK(b.stacks() == 4);
    CHECK(b.damageScale() == doctest::Approx(1.4));
    CHECK(b.shockRing(move("stamp")));
    CHECK(b.shockRing(move("ground_punch")));
    CHECK_FALSE(b.shockRing(move("edge_jab")));  // not a floor blow
    CHECK_FALSE(b.shockRing(move("low_sweep"))); // a sweep along the floor, not into it
    CHECK_FALSE(b.shockRing(kArenaBossNoMove));

    // Intercepted (the impact sheds): 3, and the nape opens.
    b.startMove(fist, 9.0, CalledZone::Right);
    const double T3 = 9.0 + 3 * B;
    b.interceptDown(CalledZone::Right, T3 - 0.075);
    {
        const ArenaBossEvents& e = b.update(T3 + 0.01, 1.0f);
        CHECK(firstOf(e, ArenaBossEventType::Answered)->outcome == CalledOutcome::Intercepted);
        CHECK(countOf(e, ArenaBossEventType::CritOpened) == 1);
        const ArenaBossEvent* s = firstOf(e, ArenaBossEventType::StacksChanged);
        REQUIRE(s);
        CHECK(s->amount == -1);
    }
    b.interceptUp(T3 + 0.1);
    CHECK(b.stacks() == 3);

    // The first crit sheds; it lands with the stacks it met.
    StoneBearerHit hit;
    {
        const ArenaBossEvents& e = b.onHit({-2, 0}, {0, 0}, {1, 0}, T3 + 0.5, nullptr, &hit);
        CHECK(hit.crit.crit);
        CHECK(hit.crit.first);
        CHECK(hit.damage == doctest::Approx(2.0 * 0.7));
        CHECK(hit.poise == doctest::Approx(2.0));
        REQUIRE(firstOf(e, ArenaBossEventType::ShockRing));
        CHECK(firstOf(e, ArenaBossEventType::ShockRing)->value == 0);
    }
    CHECK(b.stacks() == 2);
    // A second crit in the same opening sheds nothing.
    b.onHit({-2, 0}, {0, 0}, {1, 0}, T3 + 1.0, nullptr, &hit);
    CHECK(hit.crit.crit);
    CHECK_FALSE(hit.crit.first);
    CHECK(hit.damage == doctest::Approx(2.0 * 0.8));
    CHECK(b.stacks() == 2);
    // From the front: no crit, the body's scale.
    b.onHit({2, 0}, {0, 0}, {1, 0}, T3 + 1.5, nullptr, &hit);
    CHECK_FALSE(hit.crit.crit);
    CHECK(hit.damage == doctest::Approx(0.8));
    CHECK(hit.poise == doctest::Approx(1.0));

    // Missed: +missStacks (1).
    b.startMove(fist, 20.0, CalledZone::Left);
    b.update(20.0 + 3 * B + 0.01, 1.0f);
    CHECK(b.stacks() == 3);

    // A new fight starts with no openings counted.
    CHECK(b.run().crit().openings() > 0);
    b.begin(def, 0.0);
    CHECK(b.run().crit().openings() == 0);
    CHECK(b.stacks() == 0);
}

TEST_CASE("stone bearer: a blow meets the stacks of every answer up to it, with no update between") {
    const StoneBearerDef& def = shipped();
    const double B = def.boss.beat();
    StoneBearer b;
    b.begin(def, 0.0);
    bool ok = false;
    b.startMove(move("called_fist"), 1.0, CalledZone::Left, &ok);
    REQUIRE(ok);
    const double T = 1.0 + 3 * B;
    b.update(T - 0.01, 1.0f);
    CHECK(b.stacks() == 0);
    // The fist is missed at T (+1); the lead's blow at T + 0.005 comes first in the frame.
    StoneBearerHit hit;
    const ArenaBossEvents& e = b.onHit({2, 0}, {0, 0}, {1, 0}, T + 0.005, nullptr, &hit);
    CHECK(countOf(e, ArenaBossEventType::Answered) == 1);
    CHECK(b.stacks() == 1);
    CHECK(hit.damage == doctest::Approx(b.takenScale()));
    CHECK(hit.damage == doctest::Approx(1.0f - def.weight.defencePerStack));
}

TEST_CASE("stone bearer: a stagger is judged with every answer up to it, with no update between") {
    const StoneBearerDef& def = shipped();
    const double B = def.boss.beat();
    const double T = 1.0 + 3 * B; // the fist's impact
    {
        // 4 stacks; the fist Guarded at T (+2: frozen at 5). A stagger after T is ignored, the piece whole.
        StoneBearer b;
        b.begin(def, 0.0);
        for (int i = 0; i < 4; ++i) b.onSoloLift(0.1);
        REQUIRE(b.stacks() == 4);
        bool ok = false;
        b.startMove(move("called_fist"), 1.0, CalledZone::Left, &ok);
        REQUIRE(ok);
        b.guardDown(CalledZone::Left, 1.5);
        b.update(T - 0.05, 1.0f);
        const ArenaBossEvents& e = b.onStagger(T + 0.03);
        CHECK(countOf(e, ArenaBossEventType::Answered) == 1);
        CHECK(countOf(e, ArenaBossEventType::PoiseFrozen) == 1);
        CHECK(countOf(e, ArenaBossEventType::Broke) == 0);
        CHECK(b.stacks() == 5);
        CHECK_FALSE(b.run().broken());
    }
    {
        // 5 stacks (frozen); the fist Intercepted at T sheds one. A stagger after T now breaks the piece.
        StoneBearer b;
        b.begin(def, 0.0);
        for (int i = 0; i < 5; ++i) b.onSoloLift(0.1);
        REQUIRE(b.poiseFrozen());
        bool ok = false;
        b.startMove(move("called_fist"), 1.0, CalledZone::Left, &ok);
        REQUIRE(ok);
        b.update(T - 0.1, 1.0f);
        b.interceptDown(CalledZone::Left, T - 0.05);
        const ArenaBossEvents& e = b.onStagger(T + 0.03);
        const ArenaBossEvent* a = firstOf(e, ArenaBossEventType::Answered);
        REQUIRE(a);
        CHECK(a->outcome == CalledOutcome::Intercepted);
        CHECK(countOf(e, ArenaBossEventType::Broke) == 1);
        CHECK(b.stacks() == 4);
        CHECK(b.run().broken());
    }
}

TEST_CASE("stone bearer: blocks, solo lifts and the poise freeze") {
    const StoneBearerDef& def = shipped();
    StoneBearer b;
    b.begin(def, 0.0);

    // A block counts for a blow with no called answer; a called blow's Guard is counted by the strike.
    CHECK(firstOf(b.onBlocked(move("slab_drive"), 1.0), ArenaBossEventType::StacksChanged));
    CHECK(b.stacks() == 1);
    CHECK(b.onBlocked(move("called_fist"), 1.1).empty());
    CHECK(b.onBlocked(move("marked_throw"), 1.2).empty());
    CHECK(b.onBlocked(kArenaBossNoMove, 1.3).empty());
    CHECK(b.stacks() == 1);

    CHECK(firstOf(b.onSoloLift(2.0), ArenaBossEventType::StacksChanged)->value == 2);
    CHECK(firstOf(b.onSoloLift(2.1), ArenaBossEventType::ShockRing)->value == 1);
    CHECK(countOf(b.onSoloLift(2.2), ArenaBossEventType::PoiseFrozen) == 0);
    {
        const ArenaBossEvents& e = b.onSoloLift(2.3);
        REQUIRE(firstOf(e, ArenaBossEventType::PoiseFrozen));
        CHECK(firstOf(e, ArenaBossEventType::PoiseFrozen)->value == 1);
    }
    CHECK(b.stacks() == 5);
    CHECK(b.poiseFrozen());
    CHECK(b.onSoloLift(2.4).empty()); // capped: no change, no event
    CHECK(b.damageScale() == doctest::Approx(1.5));

    StoneBearerHit hit;
    b.onHit({2, 0}, {0, 0}, {1, 0}, 3.0, nullptr, &hit);
    CHECK(hit.damage == doctest::Approx(0.5));
    CHECK(hit.poise == doctest::Approx(0.0));

    // A stagger reported anyway is lost: the piece stays whole and the live move plays on.
    bool started = false;
    b.startMove(move("slab_drive"), 4.0, CalledZone::Left, &started);
    REQUIRE(started);
    CHECK(countOf(b.onStagger(4.1), ArenaBossEventType::Broke) == 0);
    CHECK_FALSE(b.run().broken());
    CHECK(b.run().liveMove() == move("slab_drive"));
}

TEST_CASE("stone bearer: a frozen boss keeps its carried piece through a stagger, but its hp share still breaks it") {
    // The rule: the poise freeze stops a Stagger, not the beat. Once the hp share reaches breakable.at
    // the piece breaks, frozen or not; phase 2 and the pedestal play as ever.
    const StoneBearerDef& def = shipped();
    StoneBearer b;
    b.begin(def, 0.0);
    for (int i = 0; i < 5; ++i) b.onSoloLift(1.0 + 0.1 * i);
    REQUIRE(b.poiseFrozen());
    b.onStagger(2.0);
    CHECK_FALSE(b.run().broken());

    CHECK(countOf(b.update(3.0, 0.45f), ArenaBossEventType::PhaseChanged) == 1);
    CHECK(b.run().phase() == 1);
    CHECK_FALSE(b.run().broken()); // above the share: still whole
    CHECK(b.table().contains(move("marked_throw"))); // the whole table
    CHECK_FALSE(b.table().contains(move("marked_throw_halves")));
    bool started = false;
    b.startMove(move("marked_throw"), 3.5, CalledZone::Left, &started);
    REQUIRE(started);
    CHECK(b.onStagger(3.6).empty()); // still lost: the throw plays on
    CHECK(b.run().liveMove() == move("marked_throw"));

    {
        // At the share, still frozen: it breaks, and the throw plays on (a break is not a stagger).
        const ArenaBossEvents& e = b.update(3.7, 0.4f);
        REQUIRE(countOf(e, ArenaBossEventType::Broke) == 1);
        CHECK(firstOf(e, ArenaBossEventType::Broke)->move == kArenaBossNoMove);
        CHECK(firstOf(e, ArenaBossEventType::Broke)->at == doctest::Approx(3.7));
    }
    CHECK(b.poiseFrozen());
    CHECK(b.run().broken());
    CHECK(b.run().liveMove() == move("marked_throw"));
    CHECK(b.run().pending());
    CHECK(b.table().contains(move("marked_throw_halves")));
    CHECK_FALSE(b.table().contains(move("marked_throw")));

    const ArenaBossEvents& e = b.update(4.0, 0.2f);
    REQUIRE(firstOf(e, ArenaBossEventType::Invulnerable));
    CHECK(firstOf(e, ArenaBossEventType::Invulnerable)->value == 1);
    CHECK(countOf(e, ArenaBossEventType::Broke) == 0); // once
    CHECK(b.invulnerable());
    CHECK(b.run().liveMove() == kArenaBossNoMove); // the pedestal cancelled the throw
    CHECK(b.onStagger(4.5).empty());

    const double call = b.pedestal().firstCall(b.run().guest());
    PedestalRelease r = PedestalRelease::NotHolding;
    b.onRelease(call + 0.05, 60, &r);
    CHECK(r == PedestalRelease::Lifted);
    CHECK_FALSE(b.invulnerable());
}

TEST_CASE("stone bearer: the hp share breaks the carried piece once, at its share, and the live move plays on") {
    const StoneBearerDef& def = shipped();
    const double B = def.boss.beat();
    const float at = def.breakable.at;
    StoneBearer b;
    b.begin(def, 0.0);
    b.update(0.5, 0.45f); // phase 2, whole
    REQUIRE(b.run().phase() == 1);
    bool started = false;
    b.startMove(move("called_fist_twice"), 1.0, CalledZone::Left, &started);
    REQUIRE(started);
    CHECK(countOf(b.update(1.1, at + 0.01f), ArenaBossEventType::Broke) == 0);
    CHECK_FALSE(b.run().broken());
    {
        const ArenaBossEvents& e = b.update(1.2, at);
        REQUIRE(countOf(e, ArenaBossEventType::Broke) == 1);
        const ArenaBossEvent* broke = firstOf(e, ArenaBossEventType::Broke);
        CHECK(broke->move == kArenaBossNoMove); // it cut nothing
        CHECK(broke->at == doctest::Approx(1.2));
    }
    CHECK(b.run().broken());
    CHECK(b.run().liveMove() == move("called_fist_twice"));
    CHECK(b.run().pending());
    CHECK(b.table().contains(move("marked_throw_halves")));
    CHECK_FALSE(b.table().contains(move("marked_throw")));

    // Both fists still come: their calls and impacts land as scheduled.
    int calls = 0;
    int answered = 0;
    int broke = 0;
    for (double t = 1.2 + kDt; t < 1.0 + 5 * B; t += kDt) {
        const ArenaBossEvents& e = b.update(t, at - 0.05f);
        calls += countOf(e, ArenaBossEventType::Call);
        answered += countOf(e, ArenaBossEventType::Answered);
        broke += countOf(e, ArenaBossEventType::Broke);
    }
    CHECK(calls == 2); // at +1 and +2 beats, both after the break
    CHECK(answered == 2);
    CHECK(broke == 0); // once
    CHECK(b.run().strike().complete());

    // A stagger after it breaks nothing more; it only cuts the move, as ever.
    b.startMove(move("slab_drive_halves"), 6.0, CalledZone::Left, &started);
    REQUIRE(started);
    CHECK(countOf(b.onStagger(6.1), ArenaBossEventType::Broke) == 0);
    CHECK(b.run().liveMove() == kArenaBossNoMove);
    CHECK(countOf(b.update(7.0, 0.3f), ArenaBossEventType::Broke) == 0);
}

TEST_CASE("stone bearer: a stagger before the hp share still breaks the piece, and the share then does nothing") {
    const StoneBearerDef& def = shipped();
    const double B = def.boss.beat();
    StoneBearer b;
    b.begin(def, 0.0);
    bool started = false;
    b.startMove(move("called_fist"), 1.0, CalledZone::Left, &started);
    REQUIRE(started);
    {
        const ArenaBossEvents& e = b.onStagger(1.0 + 2 * B);
        REQUIRE(countOf(e, ArenaBossEventType::Broke) == 1);
        CHECK(firstOf(e, ArenaBossEventType::Broke)->move == move("called_fist")); // a stagger cuts the move
    }
    CHECK(b.run().liveMove() == kArenaBossNoMove);
    CHECK(b.run().broken());
    CHECK(countOf(b.update(3.0, 0.7f), ArenaBossEventType::Broke) == 0);
    CHECK(countOf(b.update(4.0, def.breakable.at), ArenaBossEventType::Broke) == 0);
    CHECK(countOf(b.update(5.0, 0.3f), ArenaBossEventType::Broke) == 0);
    CHECK(b.run().broken());
}

TEST_CASE("stone bearer: a frame past several shares plays them in order: phase, break, pedestal") {
    const StoneBearerDef& def = shipped();
    StoneBearer b;
    b.begin(def, 0.0);
    const ArenaBossEvents& e = b.update(1.0, 0.2f);
    int phase = -1;
    int broke = -1;
    int climb = -1;
    for (std::size_t i = 0; i < e.size(); ++i) {
        if (e[i].type == ArenaBossEventType::PhaseChanged) phase = static_cast<int>(i);
        if (e[i].type == ArenaBossEventType::Broke) broke = static_cast<int>(i);
        if (e[i].type == ArenaBossEventType::Invulnerable) climb = static_cast<int>(i);
    }
    REQUIRE(phase >= 0);
    REQUIRE(broke >= 0);
    REQUIRE(climb >= 0);
    CHECK(phase < broke);
    CHECK(broke < climb);
    CHECK(b.run().broken());
    CHECK(b.invulnerable());
}

TEST_CASE("stone bearer: the pedestal (invulnerable, called every 4 beats, lifted by a Release on a call)") {
    const StoneBearerDef& def = shipped();
    const double B = def.boss.beat();
    StoneBearer b;
    b.begin(def, 0.0);
    b.update(1.0, 0.4f);
    bool started = false;
    b.startMove(move("called_fist_twice"), 2.0, CalledZone::Left, &started);
    REQUIRE(started);

    {
        const ArenaBossEvents& e = b.update(3.0, 0.25f);
        REQUIRE(firstOf(e, ArenaBossEventType::Invulnerable));
        CHECK(firstOf(e, ArenaBossEventType::Invulnerable)->at == doctest::Approx(3.0));
    }
    CHECK(b.run().liveMove() == kArenaBossNoMove);
    CHECK(b.run().strike().complete()); // no impact left to land
    b.startMove(move("edge_jab"), 3.1, CalledZone::Left, &started);
    CHECK_FALSE(started);

    // The calls: 4 beats on, then every 4 beats.
    const double c0 = 3.0 + 4 * B;
    CHECK(b.pedestal().firstCall(b.run().guest()) == doctest::Approx(c0));
    CHECK(countOf(b.update(c0 - 0.01, 0.25f), ArenaBossEventType::Call) == 0);
    {
        const ArenaBossEvents& e = b.update(c0 + 0.01, 0.2f);
        REQUIRE(countOf(e, ArenaBossEventType::Call) == 1);
        CHECK(firstOf(e, ArenaBossEventType::Call)->move == kArenaBossNoMove);
        CHECK(firstOf(e, ArenaBossEventType::Call)->at == doctest::Approx(c0));
        CHECK(countOf(e, ArenaBossEventType::Invulnerable) == 0); // once
    }
    {
        const ArenaBossEvents& e = b.update(c0 + 8 * B + 0.01, 0.2f); // a skipped frame: both calls, in order
        REQUIRE(countOf(e, ArenaBossEventType::Call) == 2);
        CHECK(e[0].at == doctest::Approx(c0 + 4 * B));
        CHECK(e[1].at == doctest::Approx(c0 + 8 * B));
    }

    // While it holds: no damage, no poise damage, no crit.
    StoneBearerHit hit;
    b.onHit({-2, 0}, {0, 0}, {1, 0}, c0 + 8 * B + 0.1, nullptr, &hit);
    CHECK(hit.damage == doctest::Approx(0.0));
    CHECK(hit.poise == doctest::Approx(0.0));
    CHECK_FALSE(hit.crit.crit);

    const double c3 = c0 + 12 * B;
    PedestalRelease r = PedestalRelease::Lifted;
    b.onRelease(c3 - 0.3, 60, &r);
    CHECK(r == PedestalRelease::OffBeat);
    b.onRelease(c3 - 0.1, 20, &r);
    CHECK(r == PedestalRelease::LowLoad);
    CHECK(b.invulnerable());
    {
        const ArenaBossEvents& e = b.onRelease(c3 - 0.1, 60, &r);
        CHECK(r == PedestalRelease::Lifted);
        REQUIRE(firstOf(e, ArenaBossEventType::Invulnerable));
        CHECK(firstOf(e, ArenaBossEventType::Invulnerable)->value == 0);
        const ArenaBossEvent* l = firstOf(e, ArenaBossEventType::Lifted);
        REQUIRE(l);
        CHECK(l->at == doctest::Approx(c3 - 0.1));
        CHECK(l->until == doctest::Approx(c3 - 0.1 + 3.0));
        CHECK(countOf(e, ArenaBossEventType::Call) == 0); // the call it beat is not played
    }
    CHECK_FALSE(b.invulnerable());
    CHECK(b.down());
    b.startMove(move("edge_jab"), c3 + 1.0, CalledZone::Left, &started);
    CHECK_FALSE(started);
    b.onHit({2, 0}, {0, 0}, {1, 0}, c3 + 1.5, nullptr, &hit);
    CHECK(hit.damage == doctest::Approx(1.0)); // down: open to every blow

    {
        const ArenaBossEvents& e = b.update(c3 + 3.0, 0.2f);
        REQUIRE(firstOf(e, ArenaBossEventType::Recovered));
        CHECK(firstOf(e, ArenaBossEventType::Recovered)->at == doctest::Approx(c3 - 0.1 + 3.0));
        CHECK(countOf(e, ArenaBossEventType::Call) == 0);
    }
    CHECK_FALSE(b.down());
    CHECK(countOf(b.update(c3 + 4.0, 0.2f), ArenaBossEventType::Recovered) == 0); // once
    b.startMove(move("edge_jab"), c3 + 4.1, CalledZone::Left, &started);
    CHECK(started);
    b.onRelease(c3 + 5.0, 90, &r);
    CHECK(r == PedestalRelease::NotHolding);
    CHECK(countOf(b.update(0.0, 0.0f), ArenaBossEventType::Invulnerable) == 0); // never again
    play(b, c3 + 5.0, c3 + 20.0, 0.1f);
    CHECK_FALSE(b.invulnerable());
}

TEST_CASE("stone bearer: the break swaps in the halves, and doubles the marked throw in phase 2") {
    const StoneBearerDef& def = shipped();
    const double B = def.boss.beat();
    StoneBearer b;
    b.begin(def, 0.0);
    CHECK(b.table().contains(move("block_throw")));
    {
        const ArenaBossEvents& e = b.onStagger(1.0);
        REQUIRE(countOf(e, ArenaBossEventType::Broke) == 1);
    }
    CHECK(countOf(b.onStagger(1.5), ArenaBossEventType::Broke) == 0); // once
    CHECK(b.table().contains(move("slab_drive_halves")));
    CHECK_FALSE(b.table().contains(move("block_throw")));
    b.update(2.0, 0.45f);
    CHECK(b.table().contains(move("marked_throw_halves")));
    CHECK_FALSE(b.table().contains(move("marked_throw")));
    bool started = false;
    b.startMove(move("marked_throw"), 2.5, CalledZone::Left, &started);
    CHECK_FALSE(started); // not in the broken table
    b.startMove(move("marked_throw_halves"), 3.0, CalledZone::Left, &started);
    REQUIRE(started);
    REQUIRE(b.run().strike().size() == 2);
    CHECK(b.run().strike().impact(0).at == doctest::Approx(3.0 + 4 * B));
    CHECK(b.run().strike().impact(1).at == doctest::Approx(3.0 + 5 * B));
    CHECK(b.run().strike().impact(0).zone == CalledZone::Path);
    CHECK(b.run().strike().impact(1).zone == CalledZone::Path);
}

TEST_CASE("stone bearer: a stagger names the move it cut, none when nothing of it is pending") {
    const StoneBearerDef& def = shipped();
    const double B = def.boss.beat();
    bool started = false;
    {
        StoneBearer b;
        b.begin(def, 0.0);
        b.startMove(move("called_fist"), 1.0, CalledZone::Left, &started);
        REQUIRE(started);
        const ArenaBossEvents& e = b.onStagger(1.0 + 2 * B); // before the blow lands
        REQUIRE(firstOf(e, ArenaBossEventType::Broke));
        CHECK(firstOf(e, ArenaBossEventType::Broke)->move == move("called_fist"));
        CHECK(b.run().liveMove() == kArenaBossNoMove);
    }
    {
        StoneBearer b;
        b.begin(def, 0.0);
        b.startMove(move("called_fist"), 1.0, CalledZone::Left, &started);
        REQUIRE(started);
        b.update(1.0 + 3 * B + 0.01, 1.0f); // it landed (missed): nothing of it is pending
        CHECK_FALSE(b.run().pending());
        CHECK(b.run().liveMove() == move("called_fist")); // live until the next start or a stagger
        const ArenaBossEvents& e = b.onStagger(1.0 + 3 * B + 0.5);
        REQUIRE(firstOf(e, ArenaBossEventType::Broke));
        CHECK(firstOf(e, ArenaBossEventType::Broke)->move == kArenaBossNoMove);
    }
    {
        StoneBearer b;
        b.begin(def, 0.0);
        b.startMove(move("slab_drive"), 1.0, CalledZone::Left, &started); // no call, no called impact
        REQUIRE(started);
        CHECK_FALSE(b.run().pending());
        CHECK(firstOf(b.onStagger(1.2), ArenaBossEventType::Broke)->move == kArenaBossNoMove);
    }
}

TEST_CASE("stone bearer: the thrown halves charge a miss once per use, and a cancel keeps the shift") {
    const StoneBearerDef& def = shipped();
    const double B = def.boss.beat();
    StoneBearer b;
    b.begin(def, 0.0);
    REQUIRE(countOf(b.onStagger(0.5), ArenaBossEventType::Broke) == 1);
    b.update(1.0, 0.45f);
    const std::uint8_t halves = move("marked_throw_halves");
    bool started = false;

    // The first half missed, then the move cut before the second: +2 and the shift, both kept.
    b.startMove(halves, 2.0, CalledZone::Left, &started);
    REQUIRE(started);
    {
        const ArenaBossEvents& e = b.update(2.0 + 4 * B + 0.01, 0.45f); // no hold: out of the path
        REQUIRE(firstOf(e, ArenaBossEventType::Answered));
        CHECK(firstOf(e, ArenaBossEventType::Answered)->outcome == CalledOutcome::Missed);
        CHECK(firstOf(e, ArenaBossEventType::Answered)->value == 1);
        REQUIRE(firstOf(e, ArenaBossEventType::CallsShifted));
        CHECK(firstOf(e, ArenaBossEventType::CallsShifted)->at == doctest::Approx(2.0 + 4 * B));
        CHECK(firstOf(e, ArenaBossEventType::StacksChanged)->amount == 2);
    }
    b.onStagger(2.0 + 4 * B + 0.2); // already broken: only cancels the move
    CHECK(b.run().liveMove() == kArenaBossNoMove);
    CHECK(b.run().guest().shifted());
    CHECK(countOf(b.update(2.0 + 6 * B, 0.45f), ArenaBossEventType::Answered) == 0); // the second half dropped
    CHECK(b.stacks() == 2);

    // Both halves missed: still +2 for the use, the second Answered unflagged, no second shift.
    b.startMove(halves, 10.0, CalledZone::Left, &started);
    REQUIRE(started);
    int answered = 0;
    int flagged = 0;
    int shifted = 0;
    for (double t = 10.0; t < 10.0 + 8 * B; t += kDt) {
        const ArenaBossEvents& e = b.update(t, 0.45f);
        for (const ArenaBossEvent& x : e) {
            answered += x.type == ArenaBossEventType::Answered ? 1 : 0;
            flagged += x.type == ArenaBossEventType::Answered && x.value == 1 ? 1 : 0;
            shifted += x.type == ArenaBossEventType::CallsShifted ? 1 : 0;
        }
    }
    CHECK(answered == 2);
    CHECK(flagged == 1);
    CHECK(shifted == 0); // once a phase
    CHECK(b.stacks() == 4);

    // Both halves guarded: guardStacks for each (1 + 1).
    b.startMove(halves, 20.0, CalledZone::Left, &started);
    REQUIRE(started);
    b.guardDown(CalledZone::Path, 20.0 + 3 * B);
    play(b, 20.0 + 3 * B, 20.0 + 6 * B, 0.45f);
    b.guardUp(20.0 + 6 * B);
    CHECK(b.stacks() == 5); // 4 + 2, kept at the cap
}

TEST_CASE("stone bearer: a non-finite time or hp stalls the boss instead of hanging it") {
    const StoneBearerDef& def = shipped();
    const double B = def.boss.beat();
    const double kInf = std::numeric_limits<double>::infinity();
    const double kNaN = std::numeric_limits<double>::quiet_NaN();
    const float kNaNf = std::numeric_limits<float>::quiet_NaN();

    for (const double bad : {kInf, -kInf, kNaN}) {
        // A fight begun at a bad time starts at 0 and plays its blows normally.
        CAPTURE(bad);
        StoneBearer b;
        b.begin(def, bad);
        CHECK(b.run().now() == 0.0);
        bool ok = false;
        b.startMove(move("called_fist"), 1.0, CalledZone::Left, &ok);
        REQUIRE(ok);
        CHECK(countOf(b.update(1.5, 1.0f), ArenaBossEventType::Answered) == 0);
        CHECK(countOf(b.update(1.0 + 3 * B + 0.01, 1.0f), ArenaBossEventType::Answered) == 1);
        CHECK(b.stacks() == 1); // the miss
    }
    StoneBearer b;
    b.begin(def, 0.0);
    bool started = false;
    b.startMove(move("called_fist"), 1.0, CalledZone::Left, &started);
    REQUIRE(started);
    for (const double bad : {kInf, -kInf, kNaN}) {
        CAPTURE(bad);
        CHECK(countOf(b.update(bad, 1.0f), ArenaBossEventType::Answered) == 0);
        b.interceptDown(CalledZone::Left, bad);
        b.interceptUp(bad);
        b.guardDown(CalledZone::Left, bad);
        b.guardUp(bad);
        StoneBearerHit hit;
        b.onHit({-2, 0}, {0, 0}, {1, 0}, bad, nullptr, &hit);
        CHECK_FALSE(hit.crit.crit);
        PedestalRelease r = PedestalRelease::Lifted;
        b.onRelease(bad, 60, &r);
        CHECK(r == PedestalRelease::NotHolding);
        CHECK(b.run().now() == 1.0);
        CHECK(b.run().liveMove() == move("called_fist"));
        CHECK(b.run().pending()); // its call and impact still to come
        CHECK(b.stacks() == 0);
    }
    // A NaN hp moves no phase and starts no pedestal; the last good share stands.
    b.update(1.1, kNaNf);
    CHECK(b.run().phase() == 0);
    CHECK_FALSE(b.invulnerable());
    // An event at a bad time lands at now.
    const ArenaBossEvents& e = b.onBlocked(move("slab_drive"), kNaN);
    REQUIRE(firstOf(e, ArenaBossEventType::StacksChanged));
    CHECK(firstOf(e, ArenaBossEventType::StacksChanged)->at == doctest::Approx(1.1));
    // The move plays on from there.
    CHECK(countOf(b.update(1.0 + 3 * B + 0.01, 1.0f), ArenaBossEventType::Answered) == 1);

    // On the pedestal: a stalled clock emits no call storm, a junk jump past kMaxTimeJump is ignored,
    // and a long real jump plays only the last period.
    b.update(20.0, 0.2f);
    REQUIRE(b.invulnerable());
    CHECK(countOf(b.update(kInf, 0.2f), ArenaBossEventType::Call) == 0);
    CHECK(countOf(b.update(20.0 + 1.0e7, 0.2f), ArenaBossEventType::Call) == 0);
    CHECK(b.run().now() == 20.0);
    const ArenaBossEvents& jump = b.update(20.0 + 500.0, 0.2f);
    CHECK(countOf(jump, ArenaBossEventType::Call) >= 4);
    CHECK(countOf(jump, ArenaBossEventType::Call) <= 5);
    CHECK(jump.dropped == 0);
    // A NaN Release is off the beat for the pedestal itself; through the boss it is a Release at now.
    PedestalPhase p;
    p.reset(def.pedestal);
    REQUIRE(p.update(0.1f, 0.0));
    CHECK(p.release(b.run().guest(), kNaN, 60) == PedestalRelease::OffBeat);
    CHECK(p.holding());
    ArenaHpTrigger trigger{0.5f};
    CHECK_FALSE(trigger.update(kNaNf));
    CHECK(trigger.update(0.5f));
}

TEST_CASE("stone bearer: a missed marked throw adds 2 and shifts the calls, the pedestal's too") {
    const StoneBearerDef& def = shipped();
    const double B = def.boss.beat();
    StoneBearer b;
    b.begin(def, 0.0);
    b.update(1.0, 0.45f);
    bool started = false;
    b.startMove(move("marked_throw"), 2.0, CalledZone::Left, &started);
    REQUIRE(started);
    b.interceptDown(CalledZone::Left, 2.0 + 4 * B - 0.075); // out of the throw's path: a miss
    {
        const ArenaBossEvents& e = b.update(2.0 + 4 * B + 0.01, 0.45f);
        CHECK(firstOf(e, ArenaBossEventType::Answered)->outcome == CalledOutcome::Missed);
        CHECK(countOf(e, ArenaBossEventType::CallsShifted) == 1);
        REQUIRE(firstOf(e, ArenaBossEventType::StacksChanged));
        CHECK(firstOf(e, ArenaBossEventType::StacksChanged)->amount == 2);
        CHECK(countOf(e, ArenaBossEventType::LoadGained) == 0);
    }
    b.interceptUp(2.0 + 4 * B + 0.1);
    CHECK(b.stacks() == 2);
    CHECK(b.run().guest().shifted());

    // The next called blow's call comes half a beat late.
    b.startMove(move("called_fist_twice"), 8.0, CalledZone::Left, &started);
    REQUIRE(started);
    {
        const ArenaBossEvents& e = b.update(8.0 + 1.5 * B + 0.01, 0.45f);
        REQUIRE(countOf(e, ArenaBossEventType::Call) == 1);
        CHECK(firstOf(e, ArenaBossEventType::Call)->at == doctest::Approx(8.0 + 1.5 * B));
    }
    // So does the pedestal's (the shift lasts the phase).
    b.update(12.0, 0.25f);
    CHECK(b.pedestal().firstCall(b.run().guest()) == doctest::Approx(12.0 + 4.5 * B));
    const ArenaBossEvents& e = b.update(12.0 + 4.5 * B + 0.01, 0.25f);
    REQUIRE(countOf(e, ArenaBossEventType::Call) == 1);
    CHECK(firstOf(e, ArenaBossEventType::Call)->at == doctest::Approx(12.0 + 4.5 * B));
}

TEST_CASE("stone bearer: no blow in the data reaches the guest's spot") {
    const ArenaBossDef& boss = shipped().boss;
    // An arena centred on the origin, yaw 0: the spot sits on +Y (bearing 90), 20 m out, 3 m wide.
    const ArenaKeepOut keep =
        arenaKeepOutAt({0, 0}, 0.0f, boss.keepOutBearing, boss.keepOutDistance, boss.keepOutRadius);
    CHECK(keep.centre.x == doctest::Approx(0.0).epsilon(1e-4));
    CHECK(keep.centre.y == doctest::Approx(20.0));
    // Inside the disc, with a little slack for float rounding. A clamped reach of 0 is no attack (the
    // host drops it), so only the points a blow actually sweeps (beyond the origin) are sampled.
    const auto inside = [&](Vec2 p) {
        const float dx = p.x - keep.centre.x;
        const float dy = p.y - keep.centre.y;
        return dx * dx + dy * dy < (keep.radius - 1e-3f) * (keep.radius - 1e-3f);
    };
    int sampled = 0;
    for (const Vec2 at : {Vec2{0, 0}, Vec2{0, 10}, Vec2{3, 14}, Vec2{-5, 5}, Vec2{0, 15.5f}}) {
        const float aim = std::atan2(keep.centre.y - at.y, keep.centre.x - at.x);
        for (const float turn : {0.0f, 0.35f, -0.35f, 1.0f, -1.0f, 2.5f}) { // at the spot, then off it
            const Vec2 dir{std::cos(aim + turn), std::sin(aim + turn)};
            const Vec2 right{-dir.y, dir.x};
            for (std::size_t i = 0; i < boss.moveCount; ++i) {
                const ArenaBossMove& m = boss.moves[i];
                CAPTURE(m.id());
                CAPTURE(turn);
                // Arcs from a forward cone to all round.
                for (const float arcCos : {0.9f, 0.5f, 0.0f, -0.5f, -1.0f}) {
                    const float reach = keep.clampArc(at, dir, arcCos, m.range);
                    CHECK(reach <= m.range);
                    const float half = std::acos(arcCos);
                    for (int a = 0; a <= 24; ++a) {
                        const float th = -half + 2.0f * half * static_cast<float>(a) / 24.0f;
                        const Vec2 ray{dir.x * std::cos(th) + right.x * std::sin(th),
                                       dir.y * std::cos(th) + right.y * std::sin(th)};
                        for (int r = 1; r <= 24; ++r) {
                            const float d = reach * static_cast<float>(r) / 24.0f;
                            ++sampled;
                            if (inside({at.x + ray.x * d, at.y + ray.y * d})) {
                                FAIL_CHECK("an arc reaches the spot");
                                r = a = 99;
                            }
                        }
                    }
                }
                // Lanes from a thin throw to one wider than the spot.
                for (const float width : {0.25f, 1.0f, 4.0f, 8.0f}) {
                    const float reach = keep.clampLane(at, dir, width, m.range);
                    CHECK(reach <= m.range);
                    if (!(reach > 0.0f)) continue;
                    for (int w = 0; w <= 8; ++w) {
                        const float off = width * (static_cast<float>(w) / 8.0f - 0.5f);
                        for (int r = 1; r <= 24; ++r) {
                            const float d = reach * static_cast<float>(r) / 24.0f;
                            ++sampled;
                            if (inside({at.x + dir.x * d + right.x * off, at.y + dir.y * d + right.y * off})) {
                                FAIL_CHECK("a lane reaches the spot");
                                r = w = 99;
                            }
                        }
                    }
                }
            }
        }
    }
    CHECK(sampled > 100000);
    // A reach that is not above 0, NaN included, is no attack.
    const float kNaNf = std::numeric_limits<float>::quiet_NaN();
    CHECK(keep.clampArc({0, 0}, {0, 1}, 0.5f, kNaNf) == 0.0f);
    CHECK(keep.clampLane({0, 0}, {0, 1}, 1.0f, kNaNf) == 0.0f);
    CHECK(keep.clampLane({0, 0}, {0, 1}, 1.0f, -2.0f) == 0.0f);
    CHECK(ArenaKeepOut{}.clampArc({0, 0}, {0, 1}, 0.5f, kNaNf) == 0.0f);
    // An area pushed off the spot only ever touches its edge.
    for (const Vec2 p : {Vec2{0, 20}, Vec2{0.5f, 19}, Vec2{-2, 22}, Vec2{4, 18}}) {
        const Vec2 c = keep.clampArea(p, 2.0f);
        const float dx = c.x - keep.centre.x;
        const float dy = c.y - keep.centre.y;
        CHECK(std::sqrt(dx * dx + dy * dy) >= keep.radius + 2.0f - 1e-3f);
    }
}

TEST_CASE("stone bearer: a whole scripted fight allocates nothing") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    const StoneBearerDef& def = shipped();
    int sink = 0;
    int moves = 0;
    StoneBearerHit hit;
    const AimPoint nape{0.0f, 0.61f};
    // Twice: with a stagger now and then (the stagger breaks the piece), and with none (the hp share does).
    int hpBreaks = 0;
    const auto fight = [&](StoneBearer& b, bool staggers) {
        b.begin(def, 0.0);
        moves = 0;
        double nextMove = 1.0;
        int frame = 0;
        for (double now = kDt; now < 180.0; now += kDt, ++frame) {
            const float hp = static_cast<float>(1.0 - now / 200.0);
            if (now >= nextMove) {
                const ArenaBossMoveTable& t = b.table();
                bool started = false;
                const std::uint8_t m = t.moves[static_cast<std::size_t>(moves) % t.count];
                const CalledZone side = (moves & 1) ? CalledZone::Left : CalledZone::Right;
                sink += static_cast<int>(b.startMove(m, now, side, &started).size());
                sink += started ? 1 : 0;
                ++moves;
                nextMove = now + 4.0;
            }
            const CalledStrike& s = b.run().strike();
            if (!s.complete()) {
                const double press = s.idealPress(s.nextIndex());
                if (now >= press && now < press + kDt) {
                    const ArenaBossEvents& e =
                        moves % 3 == 0 ? b.guardDown(s.nextZone(), now) : b.interceptDown(s.nextZone(), now);
                    sink += static_cast<int>(e.size());
                }
            }
            if (s.hold() == ArenaHold::Intercept && now - s.holdFrom() > 0.2)
                sink += static_cast<int>(b.interceptUp(now).size());
            if (s.hold() == ArenaHold::Guard && now - s.holdFrom() > 0.2) sink += static_cast<int>(b.guardUp(now).size());
            if (frame % 900 == 450) sink += static_cast<int>(b.onBlocked(def.boss.find("slab_drive"), now).size());
            if (frame % 1300 == 650) sink += static_cast<int>(b.onSoloLift(now).size());
            if (staggers && frame % 1500 == 777) sink += static_cast<int>(b.onStagger(now).size());
            if (b.invulnerable()) {
                const double call = b.pedestal().nearestCall(b.run().guest(), now);
                if (std::fabs(now - call) < kDt * 0.5 && b.pedestal().from() + 10.0 < now) {
                    PedestalRelease r;
                    sink += static_cast<int>(b.onRelease(now, 60, &r).size());
                }
            }
            sink += static_cast<int>(b.onHit({-2, 0}, {0, 0}, {1, 0}, now, &nape, &hit).size());
            const ArenaBossEvents& e = b.update(now, hp);
            sink += static_cast<int>(e.size());
            hpBreaks += countOf(e, ArenaBossEventType::Broke);
        }
    };
    StoneBearer staggered;
    StoneBearer still;
    const std::uint64_t before = ftcore_test::allocStats().count;
    fight(staggered, true);
    fight(still, false);
    const std::uint64_t after = ftcore_test::allocStats().count;
    CHECK(after == before);
    CHECK(sink > 0);
    for (const StoneBearer* f : {&staggered, &still}) {
        CHECK(f->run().phase() == 1);
        CHECK(f->run().broken());
        CHECK(f->pedestal().lifted());
        CHECK(f->run().crit().openings() > 0);
    }
    CHECK(hpBreaks == 1); // the boss that never staggered, on its share
}
