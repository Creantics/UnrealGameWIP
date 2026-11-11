#include "ftcore/ArenaBoss.h"
#include "ftcore/HeldRefusal.h"

#include "AllocCounter.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <limits>

#include <cmath>
#include <string>
#include <vector>

using namespace ftcore;

// The generic Arena boss pieces (ArenaBoss.h): called blows and their answers, the gated weak points,
// the breakable, phases, the guest's calls, the keep-out, the loader and the run that ties them up.
//
// The test boss below is synthetic, at 120 bpm (a beat is 0.5 s) so times read off directly.

namespace {

constexpr double kDt = 1.0 / 60.0;

const char* const kTestBoss = R"({
  "version": 1, "bpm": 120, "hp": 1000, "poise": 100, "staggerTime": 2.0, "openTime": 3.0,
  "intercept": { "window": 0.15, "rearm": 0.4 },
  "guestCall": { "fromBeats": 1, "minLeadBeats": 2, "missShiftBeats": 0.5 },
  "weakPoints": { "halfWidth": 0.5, "points": [
      { "id": "back", "x": 0, "y": 0.8, "radius": 0.1, "multiplier": 2.0, "facing": 180, "arc": 60 } ] },
  "keepOut": { "bearing": 90, "distance": 20, "radius": 3 },
  "moves": [
    { "id": "fist", "answer": "intercept", "impacts": [3], "calls": [1], "zones": ["side"], "sheds": [true],
      "active": 0.2, "recovery": 1.2, "range": 2.5, "damage": 0.1, "guardStacks": 2, "missStacks": 1, "opens": 3 },
    { "id": "twice", "answer": "intercept", "impacts": [3, 4], "calls": [1, 2], "zones": ["side", "other"],
      "sheds": [false, true], "active": 0.2, "recovery": 1.2, "range": 2.5, "damage": 0.1, "opens": 3 },
    { "id": "throw", "answer": "intercept", "impacts": [4], "calls": [2], "zones": ["path"], "marked": true,
      "active": 0.3, "recovery": 1.5, "range": 20, "damage": 0.2, "guardBreak": 1.2, "opens": 4, "load": 30 },
    { "id": "sweep", "answer": "jump", "impacts": [2], "active": 0.3, "recovery": 1.6, "range": 4.5, "damage": 0.1,
      "floor": true },
    { "id": "jab", "answer": "any", "impacts": [1], "active": 0.2, "recovery": 0.6, "range": 2, "damage": 0.06 }
  ],
  "phases": [
    { "endAt": 0.5, "moves": ["fist", "throw", "sweep", "jab"], "brokenMoves": ["twice", "throw", "jab"] },
    { "endAt": 0, "moves": ["twice", "throw", "jab"] }
  ]
})";

ArenaBossDef loadTestBoss() {
    ArenaBossDef def;
    std::string error;
    std::vector<std::string> warnings;
    const bool ok = importArenaBoss(kTestBoss, def, &error, &warnings);
    INFO(error);
    REQUIRE(ok);
    CHECK(warnings.empty());
    return def;
}

// The test boss with `from` replaced by `to` (once).
std::string patched(const std::string& from, const std::string& to) {
    std::string s = kTestBoss;
    const std::size_t at = s.find(from);
    REQUIRE(at != std::string::npos);
    s.replace(at, from.size(), to);
    return s;
}

std::string importError(const std::string& json) {
    ArenaBossDef def;
    std::string error;
    CHECK_FALSE(importArenaBoss(json, def, &error));
    // `out` is left default, all of it.
    const ArenaBossDef fresh;
    CHECK(def.bpm == fresh.bpm);
    CHECK(def.hp == fresh.hp);
    CHECK(def.poise == fresh.poise);
    CHECK(def.intercept.window == fresh.intercept.window);
    CHECK(def.guestCall.minLeadBeats == fresh.guestCall.minLeadBeats);
    CHECK(def.weakPoints.points.size() == 0);
    CHECK(def.keepOutRadius == 0.0f);
    CHECK(def.moveCount == 0);
    CHECK(def.phaseCount == 0);
    CHECK(def.moves[0].id().empty());
    return error;
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

int countOf(const ArenaBossEvents& events, ArenaBossEventType type) {
    int n = 0;
    for (const ArenaBossEvent& e : events) n += e.type == type ? 1 : 0;
    return n;
}

const ArenaBossEvent* findEvent(const ArenaBossEvents& events, ArenaBossEventType type) {
    for (const ArenaBossEvent& e : events)
        if (e.type == type) return &e;
    return nullptr;
}

CalledImpact impactAt(double at, CalledZone zone) {
    CalledImpact i;
    i.at = at;
    i.zone = zone;
    return i;
}

} // namespace

// ---- Geometry --------------------------------------------------------------------------------------

TEST_CASE("arena boss: beats to seconds") {
    CHECK(arenaBeats(1.0, 120.0) == doctest::Approx(0.5));
    CHECK(arenaBeats(3.0, 76.0) == doctest::Approx(180.0 / 76.0));
    CHECK(arenaBeats(4.0, 0.0) == 0.0);
}

TEST_CASE("arena boss: the side of the boss a body stands on follows its facing") {
    const Vec2 pos{0, 0};
    CHECK(arenaSideOf(pos, {1, 0}, {1, 1}) == CalledZone::Right); // +Y is right of +X
    CHECK(arenaSideOf(pos, {1, 0}, {1, -1}) == CalledZone::Left);
    CHECK(arenaSideOf(pos, {0, 1}, {-1, 1}) == CalledZone::Right); // facing +Y, its right is -X
    CHECK(arenaSideOf(pos, {1, 0}, {3, 0}) == CalledZone::Left);  // on the line
    CHECK(arenaSideOf({5, 5}, {-1, 0}, {4, 4}) == CalledZone::Right);
}

TEST_CASE("arena boss: the lane holds what is between its ends and within half its width") {
    CHECK(arenaInLane({0, 0}, {10, 0}, 2.0f, {5, 0.9f}));
    CHECK(arenaInLane({0, 0}, {10, 0}, 2.0f, {0, 1.0f}));  // the start and the edge are in
    CHECK(arenaInLane({0, 0}, {10, 0}, 2.0f, {10, -1.0f})); // the end is in
    CHECK_FALSE(arenaInLane({0, 0}, {10, 0}, 2.0f, {5, 1.1f}));
    CHECK_FALSE(arenaInLane({0, 0}, {10, 0}, 2.0f, {-0.1f, 0}));
    CHECK_FALSE(arenaInLane({0, 0}, {10, 0}, 2.0f, {10.1f, 0}));
    CHECK_FALSE(arenaInLane({1, 1}, {1, 1}, 2.0f, {1, 1})); // no lane
    CHECK(arenaZoneOf(CalledZone::Path, {0, 0}, {1, 0}, {10, 0}, 2.0f, {5, 0.5f}) == CalledZone::Path);
    CHECK(arenaZoneOf(CalledZone::Path, {0, 0}, {1, 0}, {10, 0}, 2.0f, {5, 3}) == CalledZone::Right);
    CHECK(arenaZoneOf(CalledZone::Left, {0, 0}, {1, 0}, {10, 0}, 2.0f, {5, 0.5f}) == CalledZone::Right);
}

// ---- Called blows ----------------------------------------------------------------------------------

TEST_CASE("called blow: an Intercept on the called side within the window turns it; its ends") {
    const double T = 10.0;
    const CalledImpact one = impactAt(T, CalledZone::Left);

    SUBCASE("at the window's start: turned") {
        CalledStrike s;
        s.start(&one, 1);
        CHECK(s.interceptDown(CalledZone::Left, T - 0.15).empty());
        const CalledResults r = s.update(T);
        REQUIRE(r.size() == 1);
        CHECK(r[0].outcome == CalledOutcome::Intercepted);
        CHECK(r[0].at == T);
        CHECK(s.complete());
    }
    SUBCASE("the ideal press: turned") {
        CalledStrike s;
        s.start(&one, 1);
        s.interceptDown(CalledZone::Left, s.idealPress(0));
        CHECK(s.idealPress(0) == doctest::Approx(T - 0.075));
        CHECK(s.update(T + 1.0)[0].outcome == CalledOutcome::Intercepted);
    }
    SUBCASE("just before the window: held, not turned") {
        CalledStrike s;
        s.start(&one, 1);
        s.interceptDown(CalledZone::Left, T - 0.151);
        CHECK(s.update(T)[0].outcome == CalledOutcome::Guarded);
    }
    SUBCASE("on the impact itself: too late, it lands first") {
        CalledStrike s;
        s.start(&one, 1);
        const CalledResults r = s.interceptDown(CalledZone::Left, T);
        REQUIRE(r.size() == 1);
        CHECK(r[0].outcome == CalledOutcome::Missed);
        CHECK(s.hold() == ArenaHold::Intercept); // the press still registers, for what comes next
    }
    SUBCASE("on the wrong side: lands like a miss") {
        CalledStrike s;
        s.start(&one, 1);
        s.interceptDown(CalledZone::Right, T - 0.1);
        CHECK(s.update(T)[0].outcome == CalledOutcome::WrongZone);
    }
    SUBCASE("a release on the impact still holds; a release before it does not") {
        CalledStrike s;
        s.start(&one, 1);
        s.interceptDown(CalledZone::Left, T - 0.1);
        const CalledResults r = s.interceptUp(T);
        REQUIRE(r.size() == 1);
        CHECK(r[0].outcome == CalledOutcome::Intercepted);
        CalledStrike early;
        early.start(&one, 1);
        early.interceptDown(CalledZone::Left, T - 0.1);
        CHECK(early.interceptUp(T - 0.01).empty());
        CHECK(early.update(T)[0].outcome == CalledOutcome::Missed);
    }
    SUBCASE("no hold: missed") {
        CalledStrike s;
        s.start(&one, 1);
        CHECK(s.update(T - 0.001).empty());
        CHECK(s.update(T)[0].outcome == CalledOutcome::Missed);
        CHECK(calledOutcomeName(CalledOutcome::Missed) == std::string("MISSED"));
        CHECK(calledOutcomeName(CalledOutcome::Pending) == std::string(""));
    }
}

TEST_CASE("called blow: a Guard meets a side blow from either side, never a path blow from outside it") {
    const double T = 5.0;
    const CalledImpact left = impactAt(T, CalledZone::Left);
    CalledStrike s;
    s.start(&left, 1);
    s.guardDown(CalledZone::Right, T - 2.0);
    CHECK(s.update(T)[0].outcome == CalledOutcome::Guarded);

    const CalledImpact path = impactAt(T + 5.0, CalledZone::Path);
    CalledStrike p;
    p.start(&path, 1);
    p.guardDown(CalledZone::Left, T);
    CHECK(p.update(T + 5.0)[0].outcome == CalledOutcome::Missed); // out of the path is the miss
    CalledStrike q;
    q.start(&path, 1);
    q.guardDown(CalledZone::Path, T);
    CHECK(q.update(T + 5.0)[0].outcome == CalledOutcome::Guarded);
    CalledStrike r;
    r.start(&path, 1);
    r.interceptDown(CalledZone::Path, T + 5.0 - 0.1);
    CHECK(r.update(T + 5.0)[0].outcome == CalledOutcome::Intercepted);
    CalledStrike w;
    w.start(&path, 1);
    w.interceptDown(CalledZone::Right, T + 5.0 - 0.1);
    CHECK(w.update(T + 5.0)[0].outcome == CalledOutcome::Missed);
}

TEST_CASE("called blow: the clock starts at reset; a junk far-future time is ignored, even first") {
    const CalledImpact one = impactAt(20.0, CalledZone::Left);
    CalledStrike s;
    s.setWindows(CalledWindows{0.3, 0.5});
    s.reset(13.0);
    s.start(&one, 1);
    CHECK(s.now() == 13.0);
    CHECK(s.windows().window == 0.3); // kept
    CHECK(s.update(1e300).empty()); // junk as the first time: not stored, the impact does not resolve
    CHECK(s.now() == 13.0);
    CHECK_FALSE(s.complete());
    CHECK(s.update(20.0).size() == 1);

    CalledStrike neg; // a fight clock may start below 0
    neg.reset(-50.0);
    CHECK(neg.now() == -50.0);
    neg.reset(std::numeric_limits<double>::quiet_NaN());
    CHECK(neg.now() == 0.0);
    CalledStrike fresh;
    CHECK(fresh.now() == 0.0);
    CHECK(fresh.update(1e300).empty());
    CHECK(fresh.now() == 0.0);

    // Used without a reset on a clock past kMaxTimeJump, a strike stalls: the owner must reset it first.
    const CalledImpact late = impactAt(705.0, CalledZone::Left);
    CalledStrike stale;
    stale.start(&late, 1);
    CHECK(stale.update(710.0).empty());
    CHECK(stale.now() == 0.0);
    CHECK_FALSE(stale.complete());
    stale.reset(700.0);
    stale.start(&late, 1);
    CHECK(stale.update(710.0).size() == 1);
    // ArenaBossRun::begin resets its strike to the fight's start, however late.
    ArenaBossRun run;
    run.begin(ArenaBossDef{}, 5000.0);
    CHECK(run.strike().now() == 5000.0);
}

TEST_CASE("called blow: one hold at a time, and the Intercept rearms") {
    const double T = 10.0;
    const CalledImpact one = impactAt(T, CalledZone::Right);

    SUBCASE("an Intercept tapped during a held Guard never registers") {
        CalledStrike s;
        s.start(&one, 1);
        s.guardDown(CalledZone::Right, T - 1.0);
        s.interceptDown(CalledZone::Right, T - 0.1);
        CHECK(s.hold() == ArenaHold::Guard);
        CHECK_FALSE(s.rearming(T - 0.05)); // ignored presses arm nothing
        s.interceptUp(T - 0.05);           // nor does their release drop the Guard
        CHECK(s.hold() == ArenaHold::Guard);
        CHECK(s.update(T)[0].outcome == CalledOutcome::Guarded);
    }
    SUBCASE("pressing early and again inside the rearm is not free") {
        CalledStrike s;
        s.start(&one, 1);
        s.interceptDown(CalledZone::Right, T - 0.5);
        s.interceptUp(T - 0.45);
        CHECK(s.rearming(T - 0.12));
        s.interceptDown(CalledZone::Right, T - 0.12); // ignored: under 0.4 s since the last accepted press
        CHECK(s.hold() == ArenaHold::None);
        CHECK(s.update(T)[0].outcome == CalledOutcome::Missed);
    }
    SUBCASE("a press after the rearm counts") {
        CalledStrike s;
        s.start(&one, 1);
        s.interceptDown(CalledZone::Right, T - 0.6);
        s.interceptUp(T - 0.55);
        CHECK_FALSE(s.rearming(T - 0.1));
        s.interceptDown(CalledZone::Right, T - 0.1);
        CHECK(s.update(T)[0].outcome == CalledOutcome::Intercepted);
    }
    SUBCASE("a Guard held into the impact and an Intercept after it") {
        CalledStrike s;
        s.start(&one, 1);
        s.guardDown(CalledZone::Left, T - 0.3);
        s.guardUp(T - 0.12);
        s.interceptDown(CalledZone::Right, T - 0.1);
        CHECK(s.update(T)[0].outcome == CalledOutcome::Intercepted);
    }
}

TEST_CASE("called blow: two impacts, each answered on its own side; sorting, cancel, time order") {
    const CalledImpact two[] = {impactAt(11.0, CalledZone::Left), impactAt(10.0, CalledZone::Right)};
    CalledStrike s;
    s.start(two, 2);
    REQUIRE(s.size() == 2);
    CHECK(s.impact(0).at == 10.0); // sorted
    CHECK(s.nextZone() == CalledZone::Right);
    s.interceptDown(CalledZone::Right, 9.9);
    const CalledResults first = s.interceptUp(10.05); // held through the impact
    REQUIRE(first.size() == 1);
    CHECK(first[0].outcome == CalledOutcome::Intercepted);
    CHECK(s.nextZone() == CalledZone::Left);
    s.interceptDown(CalledZone::Left, 10.9); // 1.0 s after the last accepted press: rearmed
    const CalledResults r = s.update(11.0);
    REQUIRE(r.size() == 1);
    CHECK(r[0].impact == 1);
    CHECK(r[0].outcome == CalledOutcome::Intercepted);
    s.interceptUp(11.1);

    // Both impacts at once resolve together, in order, with one hold.
    const CalledImpact same[] = {impactAt(20.0, CalledZone::Left), impactAt(20.0, CalledZone::Right)};
    s.start(same, 2);
    s.guardDown(CalledZone::Left, 19.0);
    const CalledResults both = s.update(25.0);
    REQUIRE(both.size() == 2);
    CHECK(both[0].outcome == CalledOutcome::Guarded);
    CHECK(both[1].outcome == CalledOutcome::Guarded);
    CHECK(s.hold() == ArenaHold::Guard); // the hold carries over start()
    s.guardUp(25.0);

    // A cancel drops what is pending; an earlier time is read as now.
    s.start(two, 2);
    s.update(10.5);
    s.cancel();
    CHECK(s.complete());
    CHECK(s.update(30.0).empty());
    CHECK(s.update(29.0).empty());
    CHECK(s.now() == 30.0);

    // More than kMaxCalledImpacts: the rest are dropped.
    const CalledImpact many[] = {impactAt(40, CalledZone::Left), impactAt(41, CalledZone::Left),
                                 impactAt(42, CalledZone::Left), impactAt(43, CalledZone::Left),
                                 impactAt(44, CalledZone::Left)};
    s.start(many, 5);
    CHECK(s.size() == kMaxCalledImpacts);
}

// ---- Weak points -----------------------------------------------------------------------------------

TEST_CASE("weak points: the gate passes only blows from its arc; all round passes everything but the centre") {
    const ArenaWeakGate behind{180.0f, 60.0f};
    const Vec2 pos{0, 0};
    const Vec2 f{1, 0};
    CHECK(arenaGatePasses(behind, pos, f, {-3, 0}));
    CHECK(arenaGatePasses(behind, pos, f, {-3, 1.5f}));   // 27 degrees off
    CHECK(arenaGatePasses(behind, pos, f, {-1, 1.7f}));   // 59.5 degrees off
    CHECK_FALSE(arenaGatePasses(behind, pos, f, {-1, 1.8f})); // 61 degrees off
    CHECK_FALSE(arenaGatePasses(behind, pos, f, {3, 0}));
    CHECK_FALSE(arenaGatePasses(behind, pos, f, {0, 0})); // on the centre
    const ArenaWeakGate right{90.0f, 30.0f};
    CHECK(arenaGatePasses(right, pos, f, {0, 4}));       // +Y is the right of +X
    CHECK_FALSE(arenaGatePasses(right, pos, f, {0, -4}));
    const ArenaWeakGate all{};
    CHECK(arenaGatePasses(all, pos, f, {3, 0}));
    CHECK(arenaGatePasses(all, pos, f, {0, 0}));
}

TEST_CASE("weak points: a crit needs the window open, the gate and the point; the first of each opening is flagged") {
    ArenaWeakPoints wp;
    WeakPoint back;
    back.setId("back");
    back.x = 0.0f;
    back.y = 0.8f;
    back.radius = 0.1f;
    back.multiplier = 2.0f;
    wp.points.add(back);
    wp.gates[0] = {180.0f, 60.0f};
    CritWindow c;
    c.reset(wp);
    const Vec2 pos{0, 0};
    const Vec2 f{1, 0};
    const Vec2 rear{-2, 0};
    const AimPoint on{0.0f, 0.8f};

    CHECK_FALSE(c.judge(rear, pos, f, on, 1.0).crit); // closed
    c.open(1.0, 3.0);
    CHECK(c.isOpen(1.0));
    CHECK_FALSE(c.isOpen(4.0)); // [from, until)
    CHECK_FALSE(c.judge({2, 0}, pos, f, on, 1.5).crit);           // from the front
    CHECK_FALSE(c.judge(rear, pos, f, {0.3f, 0.3f}, 1.5).crit);   // the body, not the point
    const CritResult first = c.judge(rear, pos, f, on, 1.5);
    CHECK(first.crit);
    CHECK(first.point == 0);
    CHECK(first.multiplier == 2.0f);
    CHECK(first.first);
    const CritResult second = c.judgeLockOn(rear, pos, f, 2.0);
    CHECK(second.crit);
    CHECK_FALSE(second.first);
    CHECK(c.critsThisOpening() == 2);
    CHECK_FALSE(c.judgeLockOn({2, 0}, pos, f, 2.0).crit); // no point passes from the front

    c.open(2.5, 4.0); // a new opening replaces the old
    CHECK(c.openings() == 2);
    CHECK(c.closesAt() == doctest::Approx(6.5));
    CHECK(c.judge(rear, pos, f, on, 6.0).first);
    CHECK_FALSE(c.judge(rear, pos, f, on, 6.5).crit);
}

// ---- The breakable, phases, triggers --------------------------------------------------------------

TEST_CASE("breakable: breaks on the first Stagger only") {
    ArenaBreakable b;
    CHECK(b.onStagger(3.0));
    CHECK(b.broken);
    CHECK(b.brokeAt == 3.0);
    CHECK_FALSE(b.onStagger(4.0));
    CHECK(b.brokeAt == 3.0);
}

TEST_CASE("phases: floors fall through, several at once, never back; the trigger fires once") {
    ArenaBossPhase ph[3];
    ph[0].endAt = 0.7f;
    ph[0].whole.add(0);
    ph[0].broken.add(1);
    ph[1].endAt = 0.4f;
    ph[1].whole.add(2);
    ph[2].endAt = 0.0f;
    ph[2].whole.add(3);
    ArenaBossPhaseTrack t;
    t.reset(ph, 3);
    CHECK(t.phase() == 0);
    CHECK(t.table(false)[0] == 0);
    CHECK(t.table(true)[0] == 1);
    CHECK(t.update(0.71f) == 0);
    CHECK(t.update(0.7f) == 1); // at the floor
    CHECK(t.phase() == 1);
    CHECK(t.update(0.9f) == 0); // never back
    CHECK(t.phase() == 1);
    t.reset(ph, 3);
    CHECK(t.update(0.1f) == 2); // two floors in one update
    CHECK(t.update(0.0f) == 0); // never past the last
    CHECK(t.phase() == 2);
    CHECK(t.table(false)[0] == 3);

    ArenaHpTrigger trig{0.25f};
    CHECK_FALSE(trig.update(0.26f));
    CHECK(trig.update(0.25f));
    CHECK_FALSE(trig.update(0.1f));

    ArenaBossMoveTable full;
    for (std::uint8_t i = 0; i < kMaxArenaPhaseMoves; ++i) CHECK(full.add(i));
    CHECK_FALSE(full.add(99));
    CHECK(full.contains(5));
    CHECK_FALSE(full.contains(99));
}

// ---- The guest's calls -----------------------------------------------------------------------------

TEST_CASE("guest calls: cue time, the miss shift for the phase, repeating calls") {
    GuestCaller g;
    g.reset(120.0, GuestCallTuning{});
    CHECK(g.cue(10.0, 1.0) == doctest::Approx(10.5));
    CHECK(g.onMarkedMiss());
    CHECK_FALSE(g.onMarkedMiss()); // not cumulative
    CHECK(g.cue(10.0, 1.0) == doctest::Approx(10.75));
    CHECK(g.shiftBeats() == 0.5);
    g.onPhaseChange();
    CHECK_FALSE(g.shifted());
    CHECK(g.cue(10.0, 1.0) == doctest::Approx(10.5));

    // Every 4 beats (2 s) from 10.
    CHECK(g.nextRepeat(10.0, 4.0, 9.0) == doctest::Approx(10.0));
    CHECK(g.nextRepeat(10.0, 4.0, 10.0) == doctest::Approx(10.0));
    CHECK(g.nextRepeat(10.0, 4.0, 10.01) == doctest::Approx(12.0));
    CHECK(g.nextRepeat(10.0, 4.0, 14.0) == doctest::Approx(14.0)); // on a call
    CHECK(g.nearestRepeat(10.0, 4.0, 12.9) == doctest::Approx(12.0));
    CHECK(g.nearestRepeat(10.0, 4.0, 13.0) == doctest::Approx(12.0)); // a tie: the earlier
    CHECK(g.nearestRepeat(10.0, 4.0, 13.1) == doctest::Approx(14.0));
    CHECK(g.nearestRepeat(10.0, 4.0, 3.0) == doctest::Approx(10.0));
    g.onMarkedMiss();
    CHECK(g.nextRepeat(10.0, 4.0, 10.1) == doctest::Approx(10.25));
    CHECK(g.nextRepeat(10.0, 4.0, 10.3) == doctest::Approx(12.25));
    CHECK(g.nearestRepeat(10.0, 4.0, 12.0) == doctest::Approx(12.25));

    // At the boss's own tempo the beat is not a round number: 4 beats at 76 is 240/76 s.
    GuestCaller slow;
    slow.reset(76.0, GuestCallTuning{});
    const double every = 240.0 / 76.0;
    for (int k = 1; k < 200; ++k) CHECK(slow.nextRepeat(0.0, 4.0, k * every) == doctest::Approx(k * every));
}

// ---- The keep-out ----------------------------------------------------------------------------------

TEST_CASE("keep-out: arcs stop at the disc's edge") {
    ArenaKeepOut k;
    k.centre = {10, 0};
    k.radius = 2;
    // Straight at it.
    CHECK(k.clampArc({0, 0}, {1, 0}, 0.5f, 20.0f) == doctest::Approx(8.0f));
    CHECK(k.clampArc({0, 0}, {1, 0}, 0.5f, 5.0f) == doctest::Approx(5.0f)); // short anyway
    // Facing away: the disc is behind the arc.
    CHECK(k.clampArc({0, 0}, {-1, 0}, 0.5f, 20.0f) == doctest::Approx(20.0f));
    // All round reaches it from behind.
    CHECK(k.clampArc({0, 0}, {-1, 0}, -1.0f, 20.0f) == doctest::Approx(8.0f));
    // The origin inside the disc: no reach at all.
    CHECK(k.clampArc({9, 0}, {-1, 0}, 0.5f, 20.0f) == 0.0f);
    // Facing +Y with an 80-degree half-arc: the disc's centre is 90 degrees off, so the arc's edge
    // towards +X (10 degrees off it) is what enters the disc, a little further than its nearest point.
    const float rad10 = 10.0f * 3.14159265f / 180.0f;
    const float edge = k.clampArc({0, 0}, {0, 1}, std::cos(80.0f * 3.14159265f / 180.0f), 20.0f);
    CHECK(edge > 8.0f);
    CHECK(edge < 20.0f);
    const Vec2 e{edge * std::cos(rad10), edge * std::sin(rad10)};
    CHECK(std::hypot(e.x - 10.0f, e.y) == doctest::Approx(2.0f).epsilon(0.001));
    // A 60-degree half-arc: its edge passes 5 m off the centre, clear of the disc.
    CHECK(k.clampArc({0, 0}, {0, 1}, 0.5f, 20.0f) == 20.0f);
    // A narrow arc that passes by: untouched.
    CHECK(k.clampArc({0, 0}, {0, 1}, 0.95f, 20.0f) == doctest::Approx(20.0f));
    // None: untouched.
    ArenaKeepOut none;
    CHECK(none.clampArc({0, 0}, {1, 0}, 0.5f, 20.0f) == 20.0f);
}

TEST_CASE("keep-out: lanes and areas") {
    ArenaKeepOut k;
    k.centre = {10, 0};
    k.radius = 2;
    // A lane aimed at the centre ends at the edge; one off centre is clipped by its own edge.
    CHECK(k.clampLane({0, 0}, {1, 0}, 1.0f, 30.0f) == doctest::Approx(8.0f));
    CHECK(k.clampLane({0, 2}, {1, 0}, 1.0f, 30.0f) == doctest::Approx(10.0f - std::sqrt(4.0f - 2.25f)));
    CHECK(k.clampLane({0, 2}, {1, 0}, 4.0f, 30.0f) == doctest::Approx(8.0f)); // wide enough to meet it square-on
    CHECK(k.clampLane({0, 3}, {1, 0}, 2.0f, 30.0f) == 30.0f);                // its edge only touches the disc
    CHECK(k.clampLane({0, 5}, {1, 0}, 1.0f, 30.0f) == 30.0f);                // passes by
    CHECK(k.clampLane({0, 0}, {-1, 0}, 1.0f, 30.0f) == 30.0f);               // behind
    CHECK(k.clampLane({11, 0}, {1, 0}, 1.0f, 30.0f) == 0.0f);                // from inside

    const Vec2 a = k.clampArea({12, 0}, 1.0f);
    CHECK(a.x == doctest::Approx(13.0f));
    CHECK(a.y == doctest::Approx(0.0f));
    const Vec2 same = k.clampArea({20, 0}, 1.0f);
    CHECK(same.x == 20.0f);
    const Vec2 centre = k.clampArea({10, 0}, 1.0f);
    CHECK(centre.x == doctest::Approx(13.0f));
    CHECK(centre.y == doctest::Approx(0.0f));

    const ArenaKeepOut at = arenaKeepOutAt({100, 0}, 90.0f, 90.0f, 20.0f, 3.0f); // yaw 90 + bearing 90: -X
    CHECK(at.centre.x == doctest::Approx(80.0f));
    CHECK(at.centre.y == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(at.radius == 3.0f);
}

// ---- The loader ------------------------------------------------------------------------------------

TEST_CASE("arena boss loader: the test boss loads with every field") {
    const ArenaBossDef def = loadTestBoss();
    CHECK(def.bpm == 120.0);
    CHECK(def.hp == 1000);
    CHECK(def.beat() == doctest::Approx(0.5));
    CHECK(def.intercept.window == doctest::Approx(0.15));
    CHECK(def.guestCall.missShiftBeats == doctest::Approx(0.5));
    REQUIRE(def.weakPoints.points.size() == 1);
    CHECK(def.weakPoints.points[0].id() == "back");
    CHECK(def.weakPoints.gates[0].facing == 180.0f);
    CHECK(def.weakPoints.gates[0].arc == 60.0f);
    CHECK(def.keepOutBearing == 90.0f);
    CHECK(def.keepOutRadius == 3.0f);
    REQUIRE(def.moveCount == 5);
    const ArenaBossMove& twice = def.moves[def.find("twice")];
    CHECK(twice.impactCount == 2);
    CHECK(twice.callCount == 2);
    CHECK(twice.zones[1] == CalledZoneRule::Other);
    CHECK(twice.sheds[1]);
    CHECK_FALSE(twice.sheds[0]);
    CHECK(twice.damageMax == doctest::Approx(0.2f)); // damage x impacts by default
    const ArenaBossMove& thr = def.moves[def.find("throw")];
    CHECK(thr.marked);
    CHECK(thr.load == 30);
    CHECK(thr.guardBreak == doctest::Approx(1.2f));
    CHECK(thr.zones[0] == CalledZoneRule::Path);
    const ArenaBossMove& fist = def.moves[def.find("fist")];
    CHECK(fist.guardStacks == 2);
    CHECK(fist.missStacks == 1);
    CHECK(def.moves[def.find("sweep")].floor);
    CHECK(def.moves[def.find("sweep")].answer == ArenaBossAnswer::Jump);
    CHECK_FALSE(def.moves[def.find("jab")].called());
    CHECK(def.find("nothing") == kArenaBossNoMove);
    REQUIRE(def.phaseCount == 2);
    CHECK(def.phases[0].whole.size() == 4);
    CHECK(def.phases[0].broken.size() == 3);
    CHECK(def.phases[1].broken.size() == 3); // default: the whole table
}

TEST_CASE("arena boss loader: refusals carry their path; unknown keys warn; own keys don't") {
    CHECK(has(importError("{"), "arena boss:"));
    CHECK(has(importError("[]"), "expected an object"));
    CHECK(has(importError(patched("\"version\": 1", "\"version\": 2")), "version: expected 1"));
    CHECK(has(importError(patched("\"bpm\": 120", "\"bpm\": 20")), "bpm: expected a number in 40..240"));
    CHECK(has(importError(patched("\"hp\": 1000", "\"hp\": 10.5")), "hp: expected an integer"));
    CHECK(has(importError(patched("\"window\": 0.15", "\"window\": 0.9")), "intercept.window:"));
    CHECK(has(importError(patched("\"missShiftBeats\": 0.5", "\"missShiftBeats\": 2")),
              "guestCall.missShiftBeats: expected below"));
    // A call less than the lead before its impact, or before fromBeats.
    CHECK(has(importError(patched("\"calls\": [1], \"zones\"", "\"calls\": [1.5], \"zones\"")),
              "moves[0].calls[0]: a call lands at least guestCall.minLeadBeats"));
    CHECK(has(importError(patched("\"calls\": [1], \"zones\"", "\"calls\": [0.5], \"zones\"")),
              "moves[0].calls[0]: a call lands at least guestCall.fromBeats"));
    CHECK(has(importError(patched("\"calls\": [1, 2]", "\"calls\": [1]")),
              "moves[1].calls: expected one call per impact"));
    CHECK(has(importError(patched("\"impacts\": [3, 4]", "\"impacts\": [4, 3]")),
              "moves[1].impacts[1]: expected beats strictly rising"));
    CHECK(has(importError(patched("\"impacts\": [3, 4]", "\"impacts\": [3, 3.5]")),
              "moves[1].impacts[1]: an intercept impact comes more than intercept.rearm"));
    CHECK(has(importError(patched("\"impacts\": [1]", "\"impacts\": [0]")),
              "moves[4].impacts[0]: expected a beat in (0, 32]"));
    CHECK(has(importError(patched("\"zones\": [\"side\", \"other\"]", "\"zones\": [\"side\", \"up\"]")),
              "moves[1].zones[1]: expected side, other or path"));
    CHECK(has(importError(patched("\"zones\": [\"path\"], ", "")), "moves[2].zones: expected one zone"));
    CHECK(has(importError(patched("\"floor\": true", "\"floor\": true, \"load\": 5")),
              "moves[3].load: only an intercept move has it"));
    CHECK(has(importError(patched("\"floor\": true", "\"floor\": true, \"guardStacks\": 1")),
              "moves[3].guardStacks: only an intercept move has it"));
    CHECK(has(importError(patched("\"floor\": true", "\"floor\": true, \"missStacks\": 1")),
              "moves[3].missStacks: only an intercept move has it"));
    CHECK(has(importError(patched("\"answer\": \"jump\"", "\"answer\": \"run\"")), "moves[3].answer:"));
    CHECK(has(importError(patched("\"id\": \"jab\"", "\"id\": \"fist\"")), "moves[4].id: duplicate id fist"));
    CHECK(has(importError(patched("\"id\": \"jab\"", "\"id\": \"Jab\"")), "moves[4].id: expected 1..23 characters"));
    CHECK(has(importError(patched("\"damage\": 0.06", "\"damage\": 0.06, \"damageMax\": 0.01")),
              "moves[4].damageMax:"));
    CHECK(has(importError(patched("\"sheds\": [true]", "\"sheds\": [1]")),
              "moves[0].sheds[0]: expected true or false"));
    CHECK(has(importError(patched("\"guardStacks\": 2", "\"guardStacks\": 6")), "moves[0].guardStacks:"));
    CHECK(has(importError(patched("\"endAt\": 0.5", "\"endAt\": 1")), "phases[0].endAt:"));
    CHECK(has(importError(patched("\"endAt\": 0, ", "\"endAt\": 0.6, ")),
              "phases[1].endAt: expected below the phase before"));
    CHECK(has(importError(patched("\"endAt\": 0, ", "\"endAt\": 0.2, ")), "phases[1].endAt: the last phase ends at 0"));
    CHECK(has(importError(patched("[\"twice\", \"throw\", \"jab\"] }\n  ]", "[\"twice\", \"kick\"] }\n  ]")),
              "phases[1].moves[1]: no such move"));
    CHECK(has(importError(patched("\"brokenMoves\": [\"twice\", \"throw\", \"jab\"]",
                                  "\"brokenMoves\": [\"twice\", \"twice\"]")),
              "phases[0].brokenMoves[1]: the move is already in the table"));
    CHECK(has(importError(patched("\"radius\": 0.1", "\"radius\": 0.9")), "weakPoints.points[0].radius:"));
    CHECK(has(importError(patched("\"arc\": 60", "\"arc\": 200")), "weakPoints.points[0].arc:"));
    CHECK(has(importError(patched("\"distance\": 20", "\"distance\": -1")), "keepOut.distance:"));
    CHECK(has(importError(patched(", \"radius\": 3 }", " }")), "keepOut.radius: expected a number in 0..20"));
    CHECK(has(importError(patched("\"halfWidth\": 0.5", "\"halfWidth\": 0")),
              "weakPoints.halfWidth: expected a number above 0"));
    CHECK(has(importError(patched("\"arc\": 60 } ] }",
                                  "\"arc\": 60 }, { \"id\": \"back\", \"x\": 0, \"y\": 0.5, \"radius\": 0.1, "
                                  "\"multiplier\": 2.0 } ] }")),
              "weakPoints.points[1].id: duplicate id back"));
    CHECK(has(importError(patched("\"calls\": [1, 2]", "\"calls\": [1, 0.5]")),
              "moves[1].calls[1]: expected calls strictly rising"));
    {
        // Too many moves, too many phases.
        std::string moves;
        for (std::size_t i = 0; i < kMaxArenaBossMoves; ++i)
            moves += "{ \"id\": \"m" + std::to_string(i) +
                     "\", \"answer\": \"any\", \"impacts\": [1], \"active\": 0.2, \"recovery\": 0.6, "
                     "\"range\": 2, \"damage\": 0.06 },\n";
        CHECK(has(importError(patched("\"moves\": [\n", "\"moves\": [\n" + moves)),
                  "moves: expected an array of 1.." + std::to_string(kMaxArenaBossMoves) + " moves"));
        std::string phases;
        for (std::size_t i = 0; i < kMaxArenaBossPhases; ++i) phases += "{ \"endAt\": 0.9, \"moves\": [\"jab\"] },\n";
        CHECK(has(importError(patched("\"phases\": [\n", "\"phases\": [\n" + phases)),
                  "phases: expected an array of 1.." + std::to_string(kMaxArenaBossPhases) + " phases"));
    }
    // Several problems: one per line.
    const std::string many = importError(patched("\"bpm\": 120, \"hp\": 1000", "\"bpm\": 1, \"hp\": 0"));
    CHECK(has(many, "bpm:"));
    CHECK(has(many, "\nhp:"));

    ArenaBossDef def;
    std::string error;
    std::vector<std::string> warnings;
    // A damageMax equal to damage loads, also where the float of damage rounds above the decimal.
    for (const char* d : {"0.1", "0.14", "0.3"}) {
        CAPTURE(d);
        const std::string both = std::string("\"damage\": ") + d + ", \"damageMax\": " + d;
        CHECK(importArenaBoss(patched("\"damage\": 0.06", both), def, &error));
        CHECK(def.moves[4].damageMax == def.moves[4].damage);
    }
    CHECK(importArenaBoss(patched("\"version\": 1", "\"version\": 1, \"tempo\": 3, \"weight\": {}"), def, &error,
                          &warnings, {"weight"}));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0] == "unknown key: tempo");
    warnings.clear();
    CHECK(importArenaBoss(patched("\"floor\": true", "\"floor\": true, \"spin\": 1"), def, &error, &warnings));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0] == "moves[3]: unknown key: spin");
}

// ---- The standalone weak-point file ----------------------------------------------------------------

namespace {

const char* const kTestWeak = R"({
  "version": 1,
  "weakPoints": { "halfWidth": 0.4, "points": [
      { "id": "seam", "x": 0, "y": 0.7, "radius": 0.06, "multiplier": 2.0, "facing": 0, "arc": 60 },
      { "id": "back", "x": 0.1, "y": 0.5, "radius": 0.1, "multiplier": 3.0, "facing": 180 } ] }
})";

std::string weakPatched(const std::string& from, const std::string& to) {
    std::string s = kTestWeak;
    const std::size_t at = s.find(from);
    REQUIRE(at != std::string::npos);
    s.replace(at, from.size(), to);
    return s;
}

std::string weakError(const std::string& json) {
    ArenaWeakPoints out;
    std::string error;
    CHECK_FALSE(importArenaWeakPoints(json, out, &error));
    // `out` is left default, all of it.
    CHECK(out.points.size() == 0);
    CHECK(out.points.halfWidth == kDefaultHalfWidth);
    CHECK(out.gates[0].arc == 180.0f);
    return error;
}

} // namespace

TEST_CASE("arena boss loader: a move's contact point loads; without one there is none; bad ones refuse") {
    const std::string jab = "\"id\": \"jab\",";
    {
        ArenaBossDef def;
        std::string error;
        std::vector<std::string> warnings;
        REQUIRE(importArenaBoss(patched(jab, jab + " \"contact\": {\"socket\": \"hand_r\", \"ahead\": 2.1, "
                                                  "\"side\": -1.5, \"up\": 1.68},"),
                                def, &error, &warnings));
        CHECK(warnings.empty());
        const ArenaBossMove& m = def.moves[def.find("jab")];
        CHECK(m.hasContact);
        CHECK(m.contactSocket() == "hand_r");
        CHECK(m.contactAhead == doctest::Approx(2.1f));
        CHECK(m.contactSide == doctest::Approx(-1.5f));
        CHECK(m.contactUp == doctest::Approx(1.68f));
        CHECK_FALSE(def.moves[def.find("fist")].hasContact);
        CHECK(def.moves[def.find("fist")].contactSocket().empty());
    }
    {
        ArenaBossDef def;
        std::string error;
        std::vector<std::string> warnings;
        REQUIRE(importArenaBoss(patched(jab, jab + " \"contact\": {\"socket\": \"slab\", \"ahead\": 1, "
                                                  "\"side\": 0, \"up\": 3, \"frame\": 4},"),
                                def, &error, &warnings));
        REQUIRE(warnings.size() == 1);
        CHECK(has(warnings[0], "moves[4].contact: unknown key: frame"));
    }
    CHECK(has(importError(patched(jab, jab + " \"contact\": 3,")), "moves[4].contact: expected an object"));
    CHECK(has(importError(patched(jab, jab + " \"contact\": {\"socket\": \"Hand R\", \"ahead\": 1, \"side\": 0, "
                                                "\"up\": 1},")),
              "moves[4].contact.socket: expected"));
    CHECK(has(importError(patched(jab, jab + " \"contact\": {\"socket\": \"hand_r\", \"ahead\": 30, "
                                                "\"side\": 0, \"up\": 1},")),
              "moves[4].contact.ahead: expected a number in -20..20"));
    CHECK(has(importError(patched(jab, jab + " \"contact\": {\"socket\": \"hand_r\", \"ahead\": 1, \"up\": 1},")),
              "moves[4].contact.side: expected a number"));
}

TEST_CASE("arena weak points loader: a good file loads its points and gates") {
    ArenaWeakPoints out;
    std::string error;
    std::vector<std::string> warnings;
    const bool ok = importArenaWeakPoints(kTestWeak, out, &error, &warnings);
    INFO(error);
    REQUIRE(ok);
    CHECK(warnings.empty());
    CHECK(out.points.halfWidth == 0.4f);
    REQUIRE(out.points.size() == 2);
    CHECK(out.points[0].id() == "seam");
    CHECK(out.points[0].y == 0.7f);
    CHECK(out.points[0].radius == 0.06f);
    CHECK(out.gates[0].facing == 0.0f);
    CHECK(out.gates[0].arc == 60.0f);
    CHECK(out.points[1].id() == "back");
    CHECK(out.points[1].multiplier == 3.0f);
    CHECK(out.gates[1].facing == 180.0f);
    CHECK(out.gates[1].arc == 180.0f); // absent: all round

    // The version may be left out, and so may the points (a body with no weak spot).
    CHECK(importArenaWeakPoints(R"({ "weakPoints": { } })", out, &error));
    CHECK(out.points.size() == 0);
    CHECK(out.points.halfWidth == kDefaultHalfWidth);
}

TEST_CASE("arena weak points loader: refusals carry their path") {
    CHECK(has(weakError("{"), "arena weak points:"));
    CHECK(has(weakError("[]"), "arena weak points: expected an object"));
    CHECK(has(weakError("{ \"version\": 2, \"weakPoints\": {} }"), "version: expected 1"));
    CHECK(has(weakError("{ \"version\": 1 }"), "weakPoints: expected an object"));
    CHECK(has(weakError("{ \"weakPoints\": [] }"), "weakPoints: expected an object"));
    CHECK(has(weakError(weakPatched("\"radius\": 0.06", "\"radius\": 0.9")), "weakPoints.points[0].radius:"));
    CHECK(has(weakError(weakPatched("\"arc\": 60", "\"arc\": 200")), "weakPoints.points[0].arc:"));
    CHECK(has(weakError(weakPatched("\"y\": 0.5", "\"y\": 1.5")), "weakPoints.points[1].y:"));
    CHECK(has(weakError(weakPatched("\"id\": \"back\"", "\"id\": \"seam\"")),
              "weakPoints.points[1].id: duplicate id seam"));
    CHECK(has(weakError(weakPatched("\"id\": \"back\"", "\"id\": \"Back\"")), "weakPoints.points[1].id: expected 1..15"));
    CHECK(has(weakError(weakPatched("\"halfWidth\": 0.4", "\"halfWidth\": 0")),
              "weakPoints.halfWidth: expected a number above 0"));
    CHECK(has(weakError(weakPatched("\"multiplier\": 3.0, ", "")), "weakPoints.points[1].multiplier:"));
    // More than kMaxWeakPoints.
    std::string many = "{ \"weakPoints\": { \"points\": [";
    for (std::size_t i = 0; i <= kMaxWeakPoints; ++i)
        many += std::string(i ? "," : "") + "{ \"id\": \"p" + std::to_string(i) +
                "\", \"x\": 0, \"y\": 0.5, \"radius\": 0.1, \"multiplier\": 2 }";
    many += "] } }";
    CHECK(has(weakError(many), "weakPoints.points: expected an array of 0.." + std::to_string(kMaxWeakPoints)));
    // Several problems: one per line.
    std::string two = weakPatched("\"radius\": 0.06", "\"radius\": 0.9");
    two.replace(two.find("0.5,"), 3, "1.5");
    const std::string both = weakError(two);
    CHECK(has(both, "weakPoints.points[0].radius:"));
    CHECK(has(both, "\nweakPoints.points[1].y:"));
}

TEST_CASE("arena weak points loader: unknown keys warn with their path") {
    ArenaWeakPoints out;
    std::string error;
    std::vector<std::string> warnings;
    CHECK(importArenaWeakPoints(weakPatched("\"version\": 1,", "\"version\": 1, \"note\": 1,"), out, &error, &warnings));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0] == "unknown key: note");
    warnings.clear();
    CHECK(importArenaWeakPoints(weakPatched("\"halfWidth\": 0.4", "\"halfWidth\": 0.4, \"depth\": 1"), out, &error,
                                &warnings));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0] == "weakPoints: unknown key: depth");
    warnings.clear();
    CHECK(importArenaWeakPoints(weakPatched("\"arc\": 60", "\"arc\": 60, \"glow\": 1"), out, &error, &warnings));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0] == "weakPoints.points[0]: unknown key: glow");
    // Without a warnings list, nothing is collected and nothing breaks.
    CHECK(importArenaWeakPoints(weakPatched("\"arc\": 60", "\"arc\": 60, \"glow\": 1"), out));
}

// ---- The run ---------------------------------------------------------------------------------------


TEST_CASE("arena boss run: a called blow turned on its side calls, answers, opens the weak points") {
    const ArenaBossDef def = loadTestBoss();
    ArenaBossRun run;
    run.begin(def, 0.0);
    const std::uint8_t fist = def.find("fist");
    bool started = false;
    CHECK(run.startMove(fist, 10.0, CalledZone::Left, &started).empty());
    CHECK(started);
    CHECK(run.liveMove() == fist);
    CHECK(run.liveStart() == 10.0);
    CHECK(run.strike().impact(0).at == doctest::Approx(11.5)); // 3 beats
    CHECK(run.strike().impact(0).sheds);

    const ArenaBossEvents& call = run.update(10.6, 1.0f);
    REQUIRE(call.size() == 1);
    CHECK(call[0].type == ArenaBossEventType::Call);
    CHECK(call[0].at == doctest::Approx(10.5)); // 1 beat in
    CHECK(call[0].move == fist);
    CHECK(call[0].impact == 0);

    CHECK(run.interceptDown(CalledZone::Left, 11.4).empty());
    const ArenaBossEvents& done = run.update(11.6, 1.0f);
    REQUIRE(done.size() == 2);
    CHECK(done[0].type == ArenaBossEventType::Answered);
    CHECK(done[0].outcome == CalledOutcome::Intercepted);
    CHECK(done[0].at == doctest::Approx(11.5));
    CHECK(done[1].type == ArenaBossEventType::CritOpened);
    CHECK(done[1].at == doctest::Approx(11.5));
    CHECK(done[1].until == doctest::Approx(14.5)); // opens 3 s
    CHECK(run.crit().isOpen(12.0));
    run.interceptUp(11.7);

    // A crit from behind while open; a second in the same opening is not the first.
    CritResult r;
    const ArenaBossEvents& hit = run.onHit({-2, 0}, {0, 0}, {1, 0}, 12.0, nullptr, &r);
    REQUIRE(hit.size() == 1);
    CHECK(hit[0].type == ArenaBossEventType::Crit);
    CHECK(hit[0].value == 1);
    CHECK(r.multiplier == 2.0f);
    const AimPoint on{0.0f, 0.8f};
    CHECK(run.onHit({-2, 0}, {0, 0}, {1, 0}, 12.5, &on, &r)[0].value == 0);
    CHECK(run.onHit({2, 0}, {0, 0}, {1, 0}, 12.6, &on, &r).empty()); // from the front
    CHECK_FALSE(r.crit);

    const ArenaBossEvents& shut = run.update(15.0, 1.0f);
    REQUIRE(shut.size() == 1);
    CHECK(shut[0].type == ArenaBossEventType::CritClosed);
    CHECK(shut[0].at == doctest::Approx(14.5));
    CHECK(run.update(16.0, 1.0f).empty());
    CHECK(run.onHit({-2, 0}, {0, 0}, {1, 0}, 16.0, nullptr, &r).empty());
}

TEST_CASE("arena boss run: the marked throw intercepted gives Load; missed, it shifts calls for the phase") {
    const ArenaBossDef def = loadTestBoss();
    const std::uint8_t thr = def.find("throw");
    const std::uint8_t fist = def.find("fist");

    SUBCASE("intercepted in the path: crit 4 s and Load") {
        ArenaBossRun run;
        run.begin(def, 0.0);
        run.startMove(thr, 20.0, CalledZone::Right);
        CHECK(run.strike().impact(0).zone == CalledZone::Path);
        CHECK(run.strike().impact(0).at == doctest::Approx(22.0)); // lands 4 beats after the start
        CHECK(run.update(21.0, 1.0f)[0].at == doctest::Approx(21.0)); // the call, 2 beats in
        run.interceptDown(CalledZone::Path, 21.9);
        const ArenaBossEvents& e = run.update(22.0, 1.0f);
        REQUIRE(e.size() == 3);
        CHECK(e[0].outcome == CalledOutcome::Intercepted);
        CHECK(e[1].type == ArenaBossEventType::CritOpened);
        CHECK(e[1].until == doctest::Approx(26.0));
        CHECK(e[2].type == ArenaBossEventType::LoadGained);
        CHECK(e[2].value == 30);
        CHECK_FALSE(run.guest().shifted());
    }
    SUBCASE("missed: calls come half a beat late until the phase changes") {
        ArenaBossRun run;
        run.begin(def, 0.0);
        run.startMove(thr, 20.0, CalledZone::Right);
        CHECK(countOf(run.guardDown(CalledZone::Left, 21.5), ArenaBossEventType::Call) == 1); // out of the path
        const ArenaBossEvents& e = run.update(22.5, 1.0f);
        const ArenaBossEvent* answered = findEvent(e, ArenaBossEventType::Answered);
        REQUIRE(answered);
        CHECK(answered->outcome == CalledOutcome::Missed);
        REQUIRE(findEvent(e, ArenaBossEventType::CallsShifted));
        CHECK(findEvent(e, ArenaBossEventType::CallsShifted)->at == doctest::Approx(22.0));
        CHECK(countOf(e, ArenaBossEventType::CritOpened) == 0);
        run.guardUp(22.6);
        CHECK(run.guest().shifted());

        // The next called blow: its call 1.5 beats in, its impact unmoved.
        run.startMove(fist, 30.0, CalledZone::Left);
        CHECK(run.update(30.6, 1.0f).empty());
        CHECK(run.update(30.75, 1.0f)[0].at == doctest::Approx(30.75));
        CHECK(run.strike().impact(0).at == doctest::Approx(31.5));

        // Missing it again turns nothing more on.
        run.startMove(thr, 40.0, CalledZone::Left);
        CHECK(countOf(run.update(43.0, 1.0f), ArenaBossEventType::CallsShifted) == 0);

        // The phase changes: the shift is gone.
        const ArenaBossEvents& p = run.update(44.0, 0.5f);
        REQUIRE(p.size() == 1);
        CHECK(p[0].type == ArenaBossEventType::PhaseChanged);
        CHECK(p[0].value == 1);
        CHECK_FALSE(run.guest().shifted());
    }
    SUBCASE("a guard is neither an intercept nor a miss: no crit, no shift") {
        ArenaBossRun run;
        run.begin(def, 0.0);
        run.startMove(thr, 20.0, CalledZone::Right);
        run.guardDown(CalledZone::Path, 21.5);
        const ArenaBossEvents& e = run.update(22.0, 1.0f);
        REQUIRE(findEvent(e, ArenaBossEventType::Answered));
        CHECK(findEvent(e, ArenaBossEventType::Answered)->outcome == CalledOutcome::Guarded);
        CHECK(countOf(e, ArenaBossEventType::CritOpened) == 0);
        CHECK(countOf(e, ArenaBossEventType::CallsShifted) == 0);
        CHECK_FALSE(run.guest().shifted());
    }
    SUBCASE("an Intercept outside the path is a miss") {
        ArenaBossRun run;
        run.begin(def, 0.0);
        run.startMove(thr, 20.0, CalledZone::Right);
        run.interceptDown(CalledZone::Left, 21.9);
        const ArenaBossEvents& e = run.update(22.0, 1.0f);
        REQUIRE(findEvent(e, ArenaBossEventType::Answered));
        CHECK(findEvent(e, ArenaBossEventType::Answered)->outcome == CalledOutcome::Missed);
        CHECK(countOf(e, ArenaBossEventType::CallsShifted) == 1);
    }
}

TEST_CASE("arena boss run: the break swaps the tables once; moves outside the table are refused") {
    const ArenaBossDef def = loadTestBoss();
    ArenaBossRun run;
    run.begin(def, 0.0);
    const std::uint8_t fist = def.find("fist");
    const std::uint8_t twice = def.find("twice");
    bool started = true;
    run.startMove(twice, 1.0, CalledZone::Left, &started);
    CHECK_FALSE(started); // the whole table has no doubled blow
    CHECK(run.liveMove() == kArenaBossNoMove);
    run.startMove(200, 1.0, CalledZone::Left, &started);
    CHECK_FALSE(started);

    run.startMove(fist, 2.0, CalledZone::Left, &started);
    CHECK(started);
    const ArenaBossEvents& b = run.onStagger(2.4);
    REQUIRE(b.size() == 1);
    CHECK(b[0].type == ArenaBossEventType::Broke);
    CHECK(b[0].at == 2.4);
    CHECK(run.broken());
    CHECK(run.liveMove() == kArenaBossNoMove); // the stagger cancels the blow
    CHECK(run.update(10.0, 1.0f).empty());      // its call and impact are dropped
    CHECK(run.onStagger(11.0).empty());         // breaks once
    run.startMove(fist, 12.0, CalledZone::Left, &started);
    CHECK_FALSE(started); // the broken table
    run.startMove(twice, 12.0, CalledZone::Right, &started);
    CHECK(started);
    CHECK(run.strike().impact(0).zone == CalledZone::Right);
    CHECK(run.strike().impact(1).zone == CalledZone::Left); // "other"
    CHECK(run.strike().impact(1).sheds);

    // Each fist answered on its side in turn, held through its impact: the crit opens after the second.
    run.interceptDown(CalledZone::Right, 13.4);
    const ArenaBossEvents& first = run.interceptUp(13.55);
    REQUIRE(countOf(first, ArenaBossEventType::Answered) == 1);
    CHECK(findEvent(first, ArenaBossEventType::Answered)->outcome == CalledOutcome::Intercepted);
    CHECK(countOf(first, ArenaBossEventType::CritOpened) == 0);
    run.interceptDown(CalledZone::Left, 13.9);
    const ArenaBossEvents& second = run.update(14.0, 1.0f);
    REQUIRE(second.size() == 2);
    CHECK(second[0].impact == 1);
    CHECK(second[0].outcome == CalledOutcome::Intercepted);
    CHECK(second[1].type == ArenaBossEventType::CritOpened);
}

TEST_CASE("arena boss run: events come in time order, calls first on a tie, phase first") {
    const ArenaBossDef def = loadTestBoss();
    ArenaBossRun run;
    run.begin(def, 0.0);
    const std::uint8_t fist = def.find("fist");
    run.startMove(fist, 0.0, CalledZone::Left);
    run.interceptDown(CalledZone::Left, 1.4);
    run.update(1.5, 1.0f); // turned: the window is open to 4.5
    run.interceptUp(1.6);
    run.startMove(fist, 4.0, CalledZone::Left); // call at 4.5 with the window shutting, impact at 5.5
    const ArenaBossEvents& e = run.update(6.0, 0.4f);
    REQUIRE(e.size() == 4);
    CHECK(e[0].type == ArenaBossEventType::PhaseChanged);
    CHECK(e[1].type == ArenaBossEventType::Call); // a tie with the close: the call first
    CHECK(e[2].type == ArenaBossEventType::CritClosed);
    CHECK(e[3].type == ArenaBossEventType::Answered);
    CHECK(e[3].outcome == CalledOutcome::Missed);
    CHECK(e[1].at == doctest::Approx(4.5));
    CHECK(e[2].at == doctest::Approx(4.5));
    CHECK(e[3].at == doctest::Approx(5.5));
}

TEST_CASE("arena boss run: a fight clock that starts below 0 plays its blows on time") {
    const ArenaBossDef def = loadTestBoss();
    const std::uint8_t fist = def.find("fist");
    {
        // Left alone: the impact (3 beats, 1.5 s) resolves at -8.5, not when the clock passes 0.
        ArenaBossRun run;
        run.begin(def, -10.0);
        bool ok = false;
        run.startMove(fist, -10.0, CalledZone::Left, &ok);
        REQUIRE(ok);
        CHECK(countOf(run.update(-9.0, 1.0f), ArenaBossEventType::Answered) == 0);
        CHECK(countOf(run.update(-8.6, 1.0f), ArenaBossEventType::Answered) == 0);
        const ArenaBossEvent* a = findEvent(run.update(-8.4, 1.0f), ArenaBossEventType::Answered);
        REQUIRE(a);
        CHECK(a->outcome == CalledOutcome::Missed);
        CHECK(a->at == doctest::Approx(-8.5));
    }
    {
        // A press in the window, below 0, turns it.
        ArenaBossRun run;
        run.begin(def, -10.0);
        bool ok = false;
        run.startMove(fist, -10.0, CalledZone::Left, &ok);
        REQUIRE(ok);
        CHECK(countOf(run.interceptDown(CalledZone::Left, -8.55), ArenaBossEventType::Answered) == 0);
        CHECK(run.strike().holdFrom() == doctest::Approx(-8.55));
        const ArenaBossEvent* a = findEvent(run.update(-8.4, 1.0f), ArenaBossEventType::Answered);
        REQUIRE(a);
        CHECK(a->outcome == CalledOutcome::Intercepted);
    }
    {
        // The strike on its own, reset to a start below 0 and never updated before its impacts.
        CalledStrike s;
        s.reset(-10.0);
        const CalledImpact imp = impactAt(-5.0, CalledZone::Left);
        s.start(&imp, 1);
        CHECK(s.update(-6.0).empty());
        CHECK(s.interceptDown(CalledZone::Left, -5.1).empty());
        const CalledResults r = s.update(-4.9);
        REQUIRE(r.size() == 1);
        CHECK(r[0].outcome == CalledOutcome::Intercepted);
    }
    {
        // A bare strike ignores a time that is not finite, in updates and presses.
        CalledStrike s;
        const CalledImpact imp = impactAt(2.0, CalledZone::Left);
        s.start(&imp, 1);
        CHECK(s.update(1.0).empty());
        for (const double bad : {HUGE_VAL, -HUGE_VAL, std::nan("")}) {
            CHECK(s.update(bad).empty());
            CHECK(s.guardDown(CalledZone::Left, bad).empty());
            CHECK(s.guardUp(bad).empty());
            CHECK(s.now() == 1.0);
        }
        CHECK(s.interceptDown(CalledZone::Left, 1.9).empty());
        CHECK(s.holdFrom() == doctest::Approx(1.9));
        const CalledResults r = s.update(2.1);
        REQUIRE(r.size() == 1);
        CHECK(r[0].outcome == CalledOutcome::Intercepted);
    }
}

TEST_CASE("arena boss events: past the cap they are dropped and counted; clear resets both") {
    ArenaBossEvents events;
    for (std::size_t i = 0; i < kMaxArenaBossEvents + 5; ++i)
        events.push({ArenaBossEventType::Call, kArenaBossNoMove, 0, CalledOutcome::Pending, int(i), 0, 0.0, 0.0});
    CHECK(events.size() == kMaxArenaBossEvents);
    CHECK(events.dropped == 5);
    CHECK(events.items[kMaxArenaBossEvents - 1].value == int(kMaxArenaBossEvents) - 1); // the first ones kept
    events.clear();
    CHECK(events.size() == 0);
    CHECK(events.dropped == 0);
}

TEST_CASE("arena boss run: a whole scripted fight allocates nothing") {
    if (!ftcore_test::allocHookInstalled()) {
        MESSAGE("alloc hook not linked; skipping");
        return;
    }
    const ArenaBossDef def = loadTestBoss();
    ArenaBossRun run;
    const std::uint8_t order[] = {def.find("fist"), def.find("throw"), def.find("sweep"), def.find("jab"),
                                  def.find("twice")};
    int sink = 0;
    int moves = 0;
    CritResult r;
    const AimPoint on{0.0f, 0.8f};
    const std::uint64_t before = ftcore_test::allocStats().count;
    run.begin(def, 0.0);
    double nextMove = 1.0;
    int frame = 0;
    for (double now = kDt; now < 120.0; now += kDt, ++frame) {
        const float hp = static_cast<float>(1.0 - now / 120.0);
        if (now >= nextMove) {
            bool started = false;
            const CalledZone side = (moves & 1) ? CalledZone::Left : CalledZone::Right;
            sink += static_cast<int>(run.startMove(order[moves % 5], now, side, &started).size());
            sink += started ? 1 : 0;
            ++moves;
            nextMove = now + 3.0;
        }
        if (!run.strike().complete()) {
            const double press = run.strike().idealPress(run.strike().nextIndex());
            if (now >= press && now < press + kDt)
                sink += static_cast<int>(run.interceptDown(run.strike().nextZone(), now).size());
        }
        if (run.strike().hold() != ArenaHold::None && now - run.strike().holdFrom() > 0.2)
            sink += static_cast<int>(run.interceptUp(now).size());
        if (frame % 1500 == 777) sink += static_cast<int>(run.onStagger(now).size());
        sink += static_cast<int>(run.onHit({-2, 0}, {0, 0}, {1, 0}, now, &on, &r).size());
        sink += static_cast<int>(run.update(now, hp).size());
    }
    const std::uint64_t after = ftcore_test::allocStats().count;
    CHECK(after == before);
    CHECK(sink > 0);
    CHECK(run.phase() == 1);
    CHECK(run.broken());
    CHECK(run.crit().openings() > 0);
}

// ---- The locked weak point -------------------------------------------------------------------------

namespace {

const char* const kGateEnd = "\"facing\": 180, \"arc\": 60 } ] }";

std::string withLock(const std::string& lock) {
    return patched(kGateEnd, "\"facing\": 180, \"arc\": 60 } ], \"lock\": " + lock + " }");
}

ArenaBossDef lockedBoss(const std::string& lock) {
    ArenaBossDef def;
    std::string error;
    std::vector<std::string> warnings;
    const bool ok = importArenaBoss(withLock(lock), def, &error, &warnings);
    INFO(error);
    REQUIRE(ok);
    CHECK(warnings.empty());
    return def;
}

std::string lockError(const std::string& lock) { return importError(withLock(lock)); }

bool has(const ArenaBossEvents& ev, ArenaBossEventType type) {
    for (const ArenaBossEvent& e : ev)
        if (e.type == type) return true;
    return false;
}

} // namespace

TEST_CASE("locked weak point: the loader reads the lock; absent, the points are not locked") {
    CHECK_FALSE(loadTestBoss().weakPoints.locked);
    const ArenaBossDef a = lockedBoss(R"({ "keys": ["shared_blow"] })");
    CHECK(a.weakPoints.locked);
    CHECK(a.weakPoints.lock.keys == kUnlockSharedBlow);
    CHECK(a.weakPoints.lock.openSeconds == doctest::Approx(3.0));
    CHECK(a.weakPoints.lock.relockOnDefer);
    const ArenaBossDef b =
        lockedBoss(R"({ "keys": ["read_clause", "refusal"], "clause": 7, "openSeconds": 0, "relockOnDefer": false })");
    CHECK(b.weakPoints.lock.keys == (kUnlockReadClause | kUnlockRefusal));
    CHECK(b.weakPoints.lock.clause == 7);
    CHECK(b.weakPoints.lock.openSeconds == 0.0);
    CHECK_FALSE(b.weakPoints.lock.relockOnDefer);
    CHECK(validLockTuning(b.weakPoints.lock));

    // The standalone weak-point file takes the lock too.
    ArenaWeakPoints w;
    REQUIRE(importArenaWeakPoints(
        R"({ "version": 1, "weakPoints": { "points": [], "lock": { "keys": ["refusal"], "openSeconds": 5 } } })", w));
    CHECK(w.locked);
    CHECK(w.lock.keys == kUnlockRefusal);
}

TEST_CASE("locked weak point: bad locks are refused with their path") {
    CHECK(lockError(R"({ "keys": [] })").find("weakPoints.lock.keys") != std::string::npos);
    CHECK(lockError(R"({ "keys": ["kick"] })").find("weakPoints.lock.keys[0]") != std::string::npos);
    CHECK(lockError(R"({ "keys": ["refusal", "refusal"] })").find("duplicate") != std::string::npos);
    CHECK(lockError(R"({ "keys": ["read_clause"] })").find("weakPoints.lock.clause") != std::string::npos);
    CHECK(lockError(R"({ "keys": ["refusal"], "clause": 3 })").find("weakPoints.lock.clause") != std::string::npos);
    CHECK(lockError(R"({ "keys": ["refusal"], "openSeconds": 0.05 })").find("openSeconds") != std::string::npos);
    CHECK(lockError(R"({ "keys": ["refusal"], "openSeconds": 31 })").find("openSeconds") != std::string::npos);
    CHECK(lockError(R"({ "keys": ["refusal"], "relockOnDefer": 1 })").find("relockOnDefer") != std::string::npos);
    CHECK(lockError(R"("refusal")").find("weakPoints.lock") != std::string::npos);
    std::vector<std::string> warnings;
    ArenaBossDef def;
    REQUIRE(importArenaBoss(withLock(R"({ "keys": ["refusal"], "x": 1 })"), def, nullptr, &warnings));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("weakPoints.lock") != std::string::npos);
}

TEST_CASE("locked weak point: no crit while locked, even through an intercept opening; a shared blow opens it") {
    const ArenaBossDef def = lockedBoss(R"({ "keys": ["shared_blow"] })");
    ArenaBossRun run;
    run.begin(def, 0.0);
    CHECK(run.weakPointLocked());
    CHECK_FALSE(run.weakPointOpen());
    CritResult r;
    CHECK(run.onHit({-2, 0}, {0, 0}, {1, 0}, 1.0, nullptr, &r).empty());
    CHECK_FALSE(r.crit);

    // A full intercept opens the window, but the locked point takes no crit.
    run.startMove(def.find("fist"), 10.0, CalledZone::Left);
    run.interceptDown(CalledZone::Left, 11.4);
    CHECK(has(run.update(11.6, 1.0f), ArenaBossEventType::CritOpened));
    run.interceptUp(11.7);
    CHECK(run.crit().isOpen(12.0));
    CHECK(run.onHit({-2, 0}, {0, 0}, {1, 0}, 12.0, nullptr, &r).empty());
    CHECK_FALSE(r.crit);

    // The wrong key does nothing; the shared blow unlocks it for 3 s.
    UnlockResult u = UnlockResult::Unlocked;
    CHECK(run.offerUnlock(kUnlockRefusal, 12.1, 0, &u).empty());
    CHECK(u == UnlockResult::WrongKey);
    const ArenaBossEvents& ev = run.offerUnlock(kUnlockSharedBlow, 12.2, 0, &u);
    CHECK(u == UnlockResult::Unlocked);
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].type == ArenaBossEventType::Unlocked);
    CHECK(ev[0].value == kUnlockSharedBlow);
    CHECK(ev[0].until == doctest::Approx(15.2));
    CHECK(ev[1].type == ArenaBossEventType::CritOpened);
    CHECK(ev[1].until == doctest::Approx(15.2)); // the lock outlasts the intercept's 14.5
    CHECK(run.weakPointOpen());
    const ArenaBossEvents& hit = run.onHit({-2, 0}, {0, 0}, {1, 0}, 12.5, nullptr, &r);
    REQUIRE(hit.size() == 1);
    CHECK(hit[0].type == ArenaBossEventType::Crit);
    CHECK(hit[0].value == 1);

    // It shuts on its own time: CritClosed at 15.2, then no crit.
    const ArenaBossEvents& shut = run.update(16.0, 1.0f);
    REQUIRE(shut.size() == 1);
    CHECK(shut[0].type == ArenaBossEventType::CritClosed);
    CHECK(shut[0].at == doctest::Approx(15.2));
    CHECK_FALSE(run.weakPointOpen());
    CHECK(run.onHit({-2, 0}, {0, 0}, {1, 0}, 16.0, nullptr, &r).empty());
    CHECK(run.lock().opens() == 1);
}

TEST_CASE("locked weak point: a run that begins late on the clock opens for the full window") {
    const ArenaBossDef def = lockedBoss(R"({ "keys": ["shared_blow"] })");
    for (double start : {3601.0, 7200.0, 1e6}) {
        ArenaBossRun run;
        run.begin(def, start);
        UnlockResult u = UnlockResult::BadKey;
        const ArenaBossEvents& ev = run.offerUnlock(kUnlockSharedBlow, start + 1.0, 0, &u);
        CHECK(u == UnlockResult::Unlocked);
        REQUIRE_FALSE(ev.empty());
        CHECK(ev[0].type == ArenaBossEventType::Unlocked);
        CHECK(ev[0].until == doctest::Approx(start + 4.0));
        CHECK(run.lock().open(start + 3.9));
    }
}

TEST_CASE("locked weak point: a long quiet stretch before the first offer leaves the lock in step") {
    const ArenaBossDef def = lockedBoss(R"({ "keys": ["shared_blow"] })");
    ArenaBossRun run;
    run.begin(def, 0.0);
    // 700 s: past kMaxTimeJump, so a lock that did not follow its owner would ignore the offer.
    for (double t = 1.0; t <= 700.0; t += 1.0) run.update(t, 1.0f);
    UnlockResult u = UnlockResult::BadKey;
    const ArenaBossEvents& ev = run.offerUnlock(kUnlockSharedBlow, 700.0, 0, &u);
    CHECK(u == UnlockResult::Unlocked);
    REQUIRE_FALSE(ev.empty());
    CHECK(ev[0].until == doctest::Approx(703.0));
    CHECK(run.weakPointOpen());
    run.update(703.5, 1.0f);
    CHECK_FALSE(run.weakPointOpen());
}

TEST_CASE("locked weak point: a junk far-future time is ignored and not stored by the run") {
    const ArenaBossDef def = lockedBoss(R"({ "keys": ["shared_blow"] })");
    ArenaBossRun junk, clean;
    junk.begin(def, 0.0);
    clean.begin(def, 0.0);
    UnlockResult u = UnlockResult::BadKey;
    junk.offerUnlock(kUnlockSharedBlow, 1e300, 0, &u);
    CHECK(junk.now() == 0.0);
    junk.relock(1e12);
    CHECK(junk.now() == 0.0);
    for (const double t : {13.0, 14.0}) {
        junk.update(t, 1.0f);
        clean.update(t, 1.0f);
    }
    CHECK(junk.now() == clean.now());
    junk.offerUnlock(kUnlockSharedBlow, 14.0, 0, &u);
    CHECK(u == UnlockResult::Unlocked);
    CHECK(junk.weakPointOpen());
}

TEST_CASE("locked weak point: a longer intercept opening is kept; an offer while open restarts the lock") {
    const ArenaBossDef def = lockedBoss(R"({ "keys": ["shared_blow"], "openSeconds": 1 })");
    ArenaBossRun run;
    run.begin(def, 0.0);
    run.startMove(def.find("fist"), 10.0, CalledZone::Left);
    run.interceptDown(CalledZone::Left, 11.4);
    run.update(11.6, 1.0f);
    run.interceptUp(11.7);
    const ArenaBossEvents& ev = run.offerUnlock(kUnlockSharedBlow, 12.0);
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].until == doctest::Approx(13.0));
    CHECK(ev[1].until == doctest::Approx(14.5)); // the window keeps the intercept's end
    const ArenaBossEvents& again = run.offerUnlock(kUnlockSharedBlow, 12.8);
    REQUIRE(again.size() == 2);
    CHECK(again[0].value == 0);
    CHECK(again[0].amount == 1);
    CHECK(again[0].until == doctest::Approx(13.8));
    CritResult r;
    run.onHit({-2, 0}, {0, 0}, {1, 0}, 13.5, nullptr, &r);
    CHECK(r.crit);
    run.onHit({-2, 0}, {0, 0}, {1, 0}, 14.0, nullptr, &r); // the window is open, the lock is not
    CHECK_FALSE(r.crit);
    CHECK(run.lock().opens() == 1);
}

TEST_CASE("locked weak point: a read clause must match; a refusal opens, a deferral shuts it") {
    const ArenaBossDef def = lockedBoss(R"({ "keys": ["read_clause", "refusal"], "clause": 4, "openSeconds": 0 })");
    ArenaBossRun run;
    run.begin(def, 0.0);
    UnlockResult u = UnlockResult::Unlocked;
    CHECK(run.offerUnlock(kUnlockReadClause, 1.0, 3, &u).empty());
    CHECK(u == UnlockResult::WrongClause);
    CHECK(run.offerUnlock(kUnlockSharedBlow, 1.0, 0, &u).empty());
    CHECK(u == UnlockResult::WrongKey);
    const ArenaBossEvents& ev = run.offerUnlock(kUnlockReadClause, 1.0, 4, &u);
    CHECK(u == UnlockResult::Unlocked);
    REQUIRE(ev.size() == 2);
    CHECK(std::isinf(ev[0].until));
    CHECK(run.update(500.0, 1.0f).empty()); // open until a relock
    CHECK(run.weakPointOpen());
    CritResult r;
    run.onHit({-2, 0}, {0, 0}, {1, 0}, 500.0, nullptr, &r);
    CHECK(r.crit);

    const ArenaBossEvents& shut = run.onDeferred(501.0);
    REQUIRE(shut.size() == 2);
    CHECK(shut[0].type == ArenaBossEventType::Relocked);
    CHECK(shut[1].type == ArenaBossEventType::CritClosed);
    CHECK(shut[1].at == doctest::Approx(501.0));
    CHECK_FALSE(run.weakPointOpen());
    CHECK(run.onDeferred(502.0).empty()); // already shut
    CHECK(run.lock().deferrals() == 2);

    CHECK(run.offerUnlock(kUnlockRefusal, 503.0, 0, &u).size() == 2);
    CHECK(u == UnlockResult::Unlocked);
    CHECK(run.relock(504.0).size() == 2);
    CHECK(run.relock(505.0).empty());
    CHECK(run.lock().opens() == 2);
}

TEST_CASE("locked weak point: a deferral without relockOnDefer leaves it open; an unlocked boss ignores keys") {
    const ArenaBossDef def = lockedBoss(R"({ "keys": ["refusal"], "relockOnDefer": false })");
    ArenaBossRun run;
    run.begin(def, 0.0);
    run.offerUnlock(kUnlockRefusal, 1.0);
    CHECK(run.onDeferred(1.5).empty());
    CHECK(run.weakPointOpen());

    const ArenaBossDef plain = loadTestBoss();
    ArenaBossRun p;
    p.begin(plain, 0.0);
    CHECK(p.weakPointOpen());
    UnlockResult u = UnlockResult::Unlocked;
    CHECK(p.offerUnlock(kUnlockSharedBlow, 1.0, 0, &u).empty());
    CHECK(u == UnlockResult::BadKey);
    CHECK(p.onDeferred(1.0).empty());
    CHECK(p.relock(1.0).empty());
}

TEST_CASE("locked weak point: a count-driven fight allocates nothing") {
    const ArenaBossDef def = lockedBoss(R"({ "keys": ["shared_blow", "refusal"], "openSeconds": 2 })");
    ArenaBossRun run;
    StrikeCount count;
    HeldRefusal refusal;
    int crits = 0;
    CritResult r;
    const std::uint64_t before = ftcore_test::allocStats().count;
    run.begin(def, 0.0);
    for (int frame = 0; frame < 3600; ++frame) {
        const double now = frame / 60.0;
        if (frame % 300 == 0) count.call(kLeadA, now);
        if (frame % 4 == 0) {
            const CountHit h = count.judgeBlow(static_cast<FighterId>((frame / 4) % 2), now);
            if (sharedBlow(h)) run.offerUnlock(kUnlockSharedBlow, now);
        }
        for (const CountEvent& e : count.update(now))
            if (sharedBlow(e)) run.offerUnlock(kUnlockSharedBlow, now);
        if (frame % 900 == 0) refusal.offer();
        if (frame % 900 == 450 && refusal.offerEnded() == RefusalInput::Resolved) {
            if (refusal.deferred()) run.onDeferred(now);
            else run.offerUnlock(kUnlockRefusal, now);
        }
        crits += static_cast<int>(run.onHit({-2, 0}, {0, 0}, {1, 0}, now, nullptr, &r).size());
        run.update(now, 1.0f);
    }
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(crits > 0);
    CHECK(run.lock().opens() > 0);
    CHECK(run.lock().deferrals() > 0);
}
