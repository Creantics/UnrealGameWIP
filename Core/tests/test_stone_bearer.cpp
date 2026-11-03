#include "ftcore/StoneBearer.h"

#include "ftcore/ArenaFight.h"

#include "AllocCounter.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace ftcore;

// The first Arena boss's own rules (StoneBearer.h) on top of the generic pieces: the weight stacks,
// the nape, the slab, the pedestal, and the shipped Data/Battle/arena_boss/stone_bearer.json.

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

    // The nape: one point, behind (60 degrees either side), x2.
    REQUIRE(b.weakPoints.points.size() == 1);
    CHECK(b.weakPoints.points.items[0].id() == "nape");
    CHECK(b.weakPoints.points.items[0].multiplier == doctest::Approx(2.0));
    CHECK(b.weakPoints.gates[0].facing == doctest::Approx(180.0));
    CHECK(b.weakPoints.gates[0].arc == doctest::Approx(60.0));
    CHECK(b.keepOutRadius > 0.0f);

    // The marked throw is phase 2's; the slab's loss swaps in the halves and drops the block throw.
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

    CHECK(def.weight.cap == 5);
    CHECK(def.weight.shockRingAt == 3);
    CHECK(def.weight.poiseFreezeAt == 5);
    CHECK(def.pedestal.at == doctest::Approx(0.25));
    CHECK(def.pedestal.callEveryBeats == doctest::Approx(4.0));
    CHECK(def.pedestal.releaseWindow == doctest::Approx(0.15));
}

TEST_CASE("stone bearer loader: refusals carry their paths") {
    {
        StoneBearerDef def;
        std::string error;
        std::vector<std::string> warnings;
        CHECK(importStoneBearer(kMini, def, &error, &warnings));
        CHECK(warnings.empty()); // weight and pedestal are not unknown keys
        CHECK(def.weight.guard == 1);
        CHECK(def.pedestal.downTime == doctest::Approx(3.0));
    }
    CHECK(refusal(edited(R"("weight": {)", R"("heft": {)")).find("weight: expected an object") != std::string::npos);
    CHECK(refusal(edited(R"("pedestal": {)", R"("plinth": {)")).find("pedestal: expected an object") !=
          std::string::npos);
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
    CHECK(refusal(edited(R"("callEveryBeats": 4, "releaseWindow": 0.15)", R"("callEveryBeats": 1, "releaseWindow": 0.3)")) ==
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
    CHECK(importStoneBearer(s, def, &error, &warnings));
    REQUIRE(warnings.size() == 3);
    CHECK(warnings[0] == "unknown key: mood");
    CHECK(warnings[1] == "weight: unknown key: heft");
    CHECK(warnings[2] == "pedestal: unknown key: tempo");
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
    CHECK(b.shockRing(move("low_sweep")));
    CHECK_FALSE(b.shockRing(move("edge_jab"))); // not a floor blow
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

    // A stagger reported anyway is lost: the slab stays whole and the live move plays on.
    bool started = false;
    b.startMove(move("slab_drive"), 4.0, CalledZone::Left, &started);
    REQUIRE(started);
    CHECK(countOf(b.onStagger(4.1), ArenaBossEventType::Broke) == 0);
    CHECK_FALSE(b.run().broken());
    CHECK(b.run().liveMove() == move("slab_drive"));
}

TEST_CASE("stone bearer: a frozen boss keeps its slab, and phase 2 and the pedestal still play") {
    const StoneBearerDef& def = shipped();
    StoneBearer b;
    b.begin(def, 0.0);
    for (int i = 0; i < 5; ++i) b.onSoloLift(1.0 + 0.1 * i);
    REQUIRE(b.poiseFrozen());
    b.onStagger(2.0);
    CHECK_FALSE(b.run().broken());

    CHECK(countOf(b.update(3.0, 0.45f), ArenaBossEventType::PhaseChanged) == 1);
    CHECK(b.run().phase() == 1);
    CHECK(b.table().contains(move("marked_throw"))); // the whole table
    CHECK_FALSE(b.table().contains(move("marked_throw_halves")));
    bool started = false;
    b.startMove(move("marked_throw"), 3.5, CalledZone::Left, &started);
    CHECK(started);

    const ArenaBossEvents& e = b.update(4.0, 0.2f);
    REQUIRE(firstOf(e, ArenaBossEventType::Invulnerable));
    CHECK(firstOf(e, ArenaBossEventType::Invulnerable)->value == 1);
    CHECK(b.invulnerable());
    CHECK(b.run().liveMove() == kArenaBossNoMove); // the throw was cancelled
    CHECK(b.onStagger(4.5).empty());
    CHECK_FALSE(b.run().broken());

    const double call = b.pedestal().firstCall(b.run().guest());
    PedestalRelease r = PedestalRelease::NotHolding;
    b.onRelease(call + 0.05, 60, &r);
    CHECK(r == PedestalRelease::Lifted);
    CHECK_FALSE(b.invulnerable());
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
    const ArenaKeepOut keep = arenaKeepOutAt({0, 0}, 0.0f, boss.keepOutBearing, boss.keepOutDistance, boss.keepOutRadius);
    CHECK(keep.centre.x == doctest::Approx(0.0).epsilon(1e-4));
    CHECK(keep.centre.y == doctest::Approx(20.0));
    for (const Vec2 at : {Vec2{0, 0}, Vec2{0, 10}, Vec2{3, 14}, Vec2{-5, 5}}) {
        const Vec2 d{keep.centre.x - at.x, keep.centre.y - at.y};
        const float dist = std::sqrt(d.x * d.x + d.y * d.y);
        const Vec2 dir{d.x / dist, d.y / dist}; // aimed straight at the spot: the worst case
        for (std::size_t i = 0; i < boss.moveCount; ++i) {
            const ArenaBossMove& m = boss.moves[i];
            CAPTURE(m.id());
            const float arc = keep.clampArc(at, dir, 0.5f, m.range);
            const float lane = keep.clampLane(at, dir, 1.0f, m.range);
            CHECK(arc <= m.range);
            CHECK(lane <= m.range);
            CHECK(arc <= dist - keep.radius + 1e-3f);
            CHECK(lane <= dist - keep.radius + 1e-3f);
        }
    }
}

TEST_CASE("stone bearer: a whole scripted fight allocates nothing") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    const StoneBearerDef& def = shipped();
    StoneBearer b;
    int sink = 0;
    int moves = 0;
    StoneBearerHit hit;
    const AimPoint nape{0.0f, 0.78f};
    const std::uint64_t before = ftcore_test::allocStats().count;
    b.begin(def, 0.0);
    double nextMove = 1.0;
    int frame = 0;
    for (double now = kDt; now < 180.0; now += kDt, ++frame) {
        const float hp = static_cast<float>(1.0 - now / 200.0);
        if (now >= nextMove) {
            const ArenaBossMoveTable& t = b.table();
            bool started = false;
            const std::uint8_t m = t.moves[static_cast<std::size_t>(moves) % t.count];
            sink += static_cast<int>(b.startMove(m, now, (moves & 1) ? CalledZone::Left : CalledZone::Right, &started).size());
            sink += started ? 1 : 0;
            ++moves;
            nextMove = now + 4.0;
        }
        const CalledStrike& s = b.run().strike();
        if (!s.complete()) {
            const double press = s.idealPress(s.nextIndex());
            if (now >= press && now < press + kDt) {
                sink += static_cast<int>((moves % 3 == 0 ? b.guardDown(s.nextZone(), now) : b.interceptDown(s.nextZone(), now)).size());
            }
        }
        if (s.hold() == ArenaHold::Intercept && now - s.holdFrom() > 0.2) sink += static_cast<int>(b.interceptUp(now).size());
        if (s.hold() == ArenaHold::Guard && now - s.holdFrom() > 0.2) sink += static_cast<int>(b.guardUp(now).size());
        if (frame % 900 == 450) sink += static_cast<int>(b.onBlocked(def.boss.find("slab_drive"), now).size());
        if (frame % 1300 == 650) sink += static_cast<int>(b.onSoloLift(now).size());
        if (frame % 1500 == 777) sink += static_cast<int>(b.onStagger(now).size());
        if (b.invulnerable()) {
            const double call = b.pedestal().nearestCall(b.run().guest(), now);
            if (std::fabs(now - call) < kDt * 0.5 && b.pedestal().from() + 10.0 < now) {
                PedestalRelease r;
                sink += static_cast<int>(b.onRelease(now, 60, &r).size());
            }
        }
        sink += static_cast<int>(b.onHit({-2, 0}, {0, 0}, {1, 0}, now, &nape, &hit).size());
        sink += static_cast<int>(b.update(now, hp).size());
    }
    const std::uint64_t after = ftcore_test::allocStats().count;
    CHECK(after == before);
    CHECK(sink > 0);
    CHECK(b.run().phase() == 1);
    CHECK(b.pedestal().lifted());
    CHECK(b.run().crit().openings() > 0);
}
