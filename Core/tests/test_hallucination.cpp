#include "AllocCounter.h"
#include "TestAssets.h"

#include "doctest/doctest.h"

#include "ftcore/Hallucination.h"
#include "ftcore/HallucinationArena.h"

#include <array>
#include <cmath>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

using Start = HallucinationStart;
using Ev = HallucinationEventType;

// `perLead` keys for each lead, answering to place bit 1 (even) or 2 (odd).
TragedyTable makeTable(int perLead) {
    TragedyTable t;
    t.places = {"p0", "p1"};
    for (const FighterId who : {kLeadA, kLeadB})
        for (int i = 0; i < perLead; ++i) {
            TragedyDef d;
            d.id = std::string(who == kLeadA ? "a" : "b") + std::to_string(i);
            d.lead = who;
            d.places = (i % 2 == 0) ? 1u : 2u;
            d.voice = "v";
            d.figure = "f";
            t.keys.push_back(d);
        }
    return t;
}

struct Rig {
    TragedyTable table;
    TragedyPool pool;
    Hallucination h;
    HallucinationInput in;
    std::uint32_t rng = 7;
    std::array<int, 8> seen{}; // events by type, over the rig's life

    explicit Rig(int perLead = 2, bool unlockAll = true, HallucinationFrequency f = HallucinationFrequency::Normal)
        : table(makeTable(perLead)), pool(table), h(HallucinationTuning{}, f) {
        for (int k = 0; unlockAll && k < static_cast<int>(table.keys.size()); ++k) pool.unlock(k);
        step(0.0);
    }
    void count() {
        for (int i = 0; i < h.eventCount(); ++i) ++seen[static_cast<std::size_t>(h.events()[i].type)];
    }
    void step(double dt) {
        h.tick(dt, in);
        count();
    }
    void run(double seconds) {
        for (double t = 0.0; t < seconds - 1e-9; t += 0.25) step(0.25);
    }
    Start start(bool scripted = false, int named = -1, IntensityBand band = IntensityBand::Low, std::uint32_t place = 0) {
        const int before = h.eventCount();
        const Start s = h.start(pool, rng, place, band, scripted, named);
        if (h.eventCount() > before) ++seen[static_cast<std::size_t>(Ev::Started)];
        return s;
    }
    int saw(Ev e) const { return seen[static_cast<std::size_t>(e)]; }
};

// Every field of an arena input away from its default.
ArenaInput everyInput() {
    ArenaInput a;
    a.move = Vec2{0.6f, -0.8f};
    a.view = Vec2{0.f, 1.f};
    a.light = a.heavy = a.dodge = a.jump = a.heal = a.guard = a.lock = a.release = a.pair = a.aim = true;
    a.lockSwitch = 1;
    a.read = 2;
    a.aimDir = Vec2{0.f, -1.f};
    a.aimTurn = 3.f;
    a.aimSpeed = 0.5f;
    a.aimPitched = true;
    a.aimPitch = 0.3f;
    a.aimBody = 1;
    a.attackSpeed = 0.7f;
    a.attackTurn = 4.f;
    a.takeHalf = true;
    return a;
}

bool same(const ArenaInput& a, const ArenaInput& b) {
    return a.move.x == b.move.x && a.move.y == b.move.y && a.view.x == b.view.x && a.view.y == b.view.y &&
           a.light == b.light && a.heavy == b.heavy && a.dodge == b.dodge && a.jump == b.jump && a.heal == b.heal &&
           a.guard == b.guard && a.lock == b.lock && a.lockSwitch == b.lockSwitch && a.release == b.release &&
           a.pair == b.pair && a.read == b.read && a.aim == b.aim && a.aimDir.x == b.aimDir.x &&
           a.aimDir.y == b.aimDir.y && a.aimTurn == b.aimTurn && a.aimSpeed == b.aimSpeed &&
           a.aimPitched == b.aimPitched && a.aimPitch == b.aimPitch && a.aimBody == b.aimBody && a.attackSpeed == b.attackSpeed &&
           a.attackTurn == b.attackTurn && a.takeHalf == b.takeHalf;
}

// A new tuning field must be added to the three helpers below. Two tripwires: the size, and in each helper's own body
// a structured binding of exactly the current 14 members, which fails to compile when a member is added or removed
// (padding cannot hide it).
static_assert(sizeof(HallucinationTuning) == 13 * sizeof(float) + 8, "add the new field to the tuning helpers");

// Every tuning field away from its default.
void dirtyAll(HallucinationTuning& t) {
    [[maybe_unused]] const auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13] = t; // 14 members
    t.holdSeconds = 1.25f;
    t.expireSeconds = 11.f;
    t.gapSeconds = 5.f;
    t.switchSeconds = 3.f;
    t.refuseSeconds = 0.75f;
    t.refusalCooldownSeconds = 9.f;
    t.pullSeconds = 2.f;
    t.recentSeconds = 60.f;
    t.fewerGapScale = 7.f;
    t.lonePoolFloor = 777.f;
    t.counselCooldownSeconds = 0.25f;
    t.proxyReactSeconds = 4.5f;
    t.escrowDelaySeconds = 12.f;
    t.actFloors.fill(IntensityBand::High);
}

bool sameTuning(const HallucinationTuning& a, const HallucinationTuning& b) {
    [[maybe_unused]] const auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13] = a; // 14 members
    return a.holdSeconds == b.holdSeconds && a.expireSeconds == b.expireSeconds && a.gapSeconds == b.gapSeconds &&
           a.switchSeconds == b.switchSeconds && a.refuseSeconds == b.refuseSeconds &&
           a.refusalCooldownSeconds == b.refusalCooldownSeconds && a.pullSeconds == b.pullSeconds &&
           a.recentSeconds == b.recentSeconds && a.fewerGapScale == b.fewerGapScale &&
           a.lonePoolFloor == b.lonePoolFloor && a.counselCooldownSeconds == b.counselCooldownSeconds &&
           a.proxyReactSeconds == b.proxyReactSeconds && a.escrowDelaySeconds == b.escrowDelaySeconds &&
           a.actFloors == b.actFloors;
}

// Each field of `a` differs from `b`'s.
bool allDiffer(const HallucinationTuning& a, const HallucinationTuning& b) {
    [[maybe_unused]] const auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13] = a; // 14 members
    bool floorsDiffer = true;
    for (std::size_t i = 0; i < a.actFloors.size(); ++i) floorsDiffer = floorsDiffer && a.actFloors[i] != b.actFloors[i];
    return a.holdSeconds != b.holdSeconds && a.expireSeconds != b.expireSeconds && a.gapSeconds != b.gapSeconds &&
           a.switchSeconds != b.switchSeconds && a.refuseSeconds != b.refuseSeconds &&
           a.refusalCooldownSeconds != b.refusalCooldownSeconds && a.pullSeconds != b.pullSeconds &&
           a.recentSeconds != b.recentSeconds && a.fewerGapScale != b.fewerGapScale &&
           a.lonePoolFloor != b.lonePoolFloor && a.counselCooldownSeconds != b.counselCooldownSeconds &&
           a.proxyReactSeconds != b.proxyReactSeconds && a.escrowDelaySeconds != b.escrowDelaySeconds && floorsDiffer;
}

} // namespace

TEST_CASE("a tragedy key is never picked before it is unlocked, and only for its own lead") {
    const TragedyTable t = makeTable(2); // a0 a1 b0 b1
    TragedyPool pool(t);
    std::uint32_t rng = 3;
    CHECK(pool.pick(rng, kLeadA, 0, 0.0, 0.0) == -1);
    CHECK(pool.unlock(2));
    CHECK_FALSE(pool.unlock(4));
    for (int i = 0; i < 20; ++i) {
        CHECK(pool.pick(rng, kLeadA, 0, 0.0, 0.0) == -1);
        CHECK(pool.pick(rng, kLeadB, 0, 0.0, 0.0) == 2);
    }

    Rig r(2, false);
    CHECK(r.start() == Start::NoKey);
    CHECK(r.start(true, 0) == Start::NoKey);
    r.pool.unlock(2);
    CHECK(r.start(true, 2) == Start::NoKey); // lead b's key, lead a active
    r.pool.unlock(1);
    CHECK(r.start() == Start::Started);
    CHECK(r.h.key() == 1);
}

TEST_CASE("a place picks only the keys that answer to it") {
    const TragedyTable t = makeTable(4);
    TragedyPool pool(t);
    for (int k = 0; k < 8; ++k) pool.unlock(k);
    std::uint32_t rng = 11;
    for (int i = 0; i < 20; ++i) {
        const int k = pool.pick(rng, kLeadA, 2u, 0.0, 0.0);
        CHECK((k == 1 || k == 3));
    }
    CHECK(tragedyPlaceBit(t, "p1") == 2u);
    CHECK(tragedyPlaceBit(t, "p9") == 0u);
}

TEST_CASE("a key shown lately is passed over while another fits, and picked when it is the only one") {
    const TragedyTable t = makeTable(2);
    TragedyPool pool(t);
    pool.unlock(0);
    pool.unlock(1);
    pool.shown(0, 100.0);
    std::uint32_t rng = 5;
    for (int i = 0; i < 20; ++i) CHECK(pool.pick(rng, kLeadA, 0, 200.0, 1800.0) == 1);
    CHECK(pool.pick(rng, kLeadA, 1u, 200.0, 1800.0) == 0); // the only one for place p0
    bool both = false;
    for (int i = 0; i < 20; ++i) both = both || pool.pick(rng, kLeadA, 0, 2000.0, 1800.0) == 0;
    CHECK(both); // no longer recent
}

TEST_CASE("the same seed picks the same keys") {
    const TragedyTable t = makeTable(5);
    TragedyPool pool(t);
    for (int k = 0; k < 10; ++k) pool.unlock(k);
    std::uint32_t a = 42, b = 42;
    int distinct = 0, last = -1;
    for (int i = 0; i < 32; ++i) {
        const int ka = pool.pick(a, kLeadA, 0, 0.0, 0.0);
        CHECK(ka == pool.pick(b, kLeadA, 0, 0.0, 0.0));
        CHECK((ka >= 0 && ka < 5));
        distinct += ka != last ? 1 : 0;
        last = ka;
    }
    CHECK(distinct > 1);

    std::uint32_t zero = 0;
    CHECK(tragedyRng(zero) != 0u);
}

// Over 10 000 picks, each key's share (first picks from 10 000 seeds, and picks from one stream) within 2 points.
void checkShares(const TragedyPool& pool, bool loneWeighted, double now, const std::array<double, 3>& want) {
    std::array<int, 3> bySeed{}, byStream{};
    std::uint32_t stream = 99;
    for (std::uint32_t i = 0; i < 10000; ++i) {
        std::uint32_t seed = i * 2654435761u + 1u;
        const int a = pool.pick(seed, kLeadA, 0, now, 1800.0, loneWeighted);
        const int b = pool.pick(stream, kLeadA, 0, now, 1800.0, loneWeighted);
        REQUIRE((a >= 0 && a < 3 && b >= 0 && b < 3));
        ++bySeed[static_cast<std::size_t>(a)];
        ++byStream[static_cast<std::size_t>(b)];
    }
    for (std::size_t k = 0; k < 3; ++k) {
        CHECK(std::abs(bySeed[k] / 10000.0 - want[k]) < 0.02);
        CHECK(std::abs(byStream[k] / 10000.0 - want[k]) < 0.02);
    }
}

TEST_CASE("picks are weighted: the newest x3, a caused key x2, the lone key x2 while Lone Carry weighs it; they multiply") {
    TragedyTable t = makeTable(3); // a0 a1 a2, b0 b1 b2
    t.keys[0].caused = t.keys[1].caused = true;
    t.keys[2].lone = true;
    TragedyPool pool(t);
    CHECK(pool.newest(kLeadA) == -1);
    pool.unlock(2);
    pool.unlock(1);
    pool.unlock(0); // a0: the newest, and caused
    pool.unlock(1); // again: it keeps its place
    CHECK(pool.newest(kLeadA) == 0);
    CHECK(pool.newest(kLeadB) == -1);
    pool.unlock(3);
    CHECK(pool.newest(kLeadB) == 3); // each lead its own
    CHECK(pool.newest(kLeadA) == 0);
    CHECK(pool.weight(0) == 6u);
    CHECK(pool.weight(1) == 2u);
    CHECK(pool.weight(2) == 1u);
    CHECK(pool.weight(2, true) == 2u);
    CHECK(pool.weight(0, true) == 6u);
    CHECK(pool.weight(4) == 0u); // not unlocked
    CHECK(pool.weight(3) == 3u);

    checkShares(pool, false, 0.0, {6.0 / 9.0, 2.0 / 9.0, 1.0 / 9.0});
    checkShares(pool, true, 0.0, {6.0 / 10.0, 2.0 / 10.0, 2.0 / 10.0});
    pool.shown(0, 100.0); // the newest shown lately: passed over, and its x3 is not handed on
    checkShares(pool, false, 200.0, {0.0, 2.0 / 3.0, 1.0 / 3.0});

    HallucinationTuning tune;
    CHECK(tune.lonePoolFloor == 300.f);
    CHECK_FALSE(loneWeighs(0, tune));
    CHECK_FALSE(loneWeighs(299, tune));
    CHECK(loneWeighs(300, tune));
    tune.lonePoolFloor = 0.f;
    CHECK_FALSE(loneWeighs(0, tune)); // no lone Releases at all
    CHECK(loneWeighs(1, tune));
}

TEST_CASE("a start weighs the lone key by the pooled Load it is given") {
    TragedyTable t = makeTable(2); // a0 a1
    t.keys[0].lone = true;
    int differ = 0;
    for (std::uint32_t seed = 1; seed <= 200; ++seed) {
        for (const std::uint32_t pooled : {0u, 299u, 300u}) {
            TragedyPool pool(t);
            pool.unlock(0);
            pool.unlock(1);
            const TragedyPool before = pool; // nothing shown yet
            Hallucination h;
            h.tick(0.0, HallucinationInput{});
            std::uint32_t mine = seed, theirs = seed;
            HallucinationStartIn s;
            s.band = IntensityBand::Low;
            s.loneLoadPool = pooled;
            REQUIRE(h.start(pool, mine, s) == Start::Started);
            const int want = before.pick(theirs, kLeadA, 0, 0.0, 1800.0, pooled >= 300u);
            CHECK(h.key() == want);
            CHECK(mine == theirs);
            if (pooled == 300u) {
                std::uint32_t plain = seed;
                differ += before.pick(plain, kLeadA, 0, 0.0, 1800.0, false) != want ? 1 : 0;
            }
        }
    }
    CHECK(differ > 0); // the weight changed some picks
}

TEST_CASE("only a scripted start flagged as a scare reaches the ScarePacer, and only once") {
    ScareMoment m;
    REQUIRE(scareMomentFromId("scare_001", m));
    ScarePacer pacer;
    pacer.advance(200.0); // past a new pacer's quiet

    Rig r;
    HallucinationStartIn s;
    s.band = IntensityBand::Low;
    s.scripted = true;
    s.named = 0;
    s.scare = m; // the moment, but not flagged
    CHECK(r.h.start(r.pool, r.rng, s, &pacer) == Start::Started);
    CHECK(r.h.scareForm() == ScareForm::Unpaced);
    CHECK(pacer.inWindow() == 0);
    CHECK(pacer.lastTime() < 0.0);
    r.in.turnedAway = true;
    r.step(0.25);
    REQUIRE_FALSE(r.h.active());
    r.in.turnedAway = false;

    Rig u; // flagged but not scripted: an ambient start is never a scare
    HallucinationStartIn a = s;
    a.scripted = false;
    a.named = -1;
    a.flaggedAsScare = true;
    CHECK(u.h.start(u.pool, u.rng, a, &pacer) == Start::Started);
    CHECK(u.h.scareForm() == ScareForm::Unpaced);
    CHECK(pacer.inWindow() == 0);

    s.flaggedAsScare = true;
    CHECK(r.h.start(r.pool, r.rng, s, &pacer) == Start::Started);
    CHECK(r.h.scareForm() == ScareForm::Full);
    CHECK(pacer.inWindow() == 1);
    CHECK(pacer.lastKind() == ScareKind::Hallucination);
    CHECK(r.h.start(r.pool, r.rng, s, &pacer) == Start::Busy); // refused starts reach nothing
    r.in.accepting = true;
    r.run(10.0);
    CHECK(pacer.inWindow() == 1); // ticks add nothing
    pacer.complete(m);
    CHECK(pacer.seen(m));

    Rig n(2, false); // no key: nothing reaches the pacer
    CHECK(n.h.start(n.pool, n.rng, s, &pacer) == Start::NoKey);
    CHECK(pacer.inWindow() == 1);

    ScarePacer quiet; // a new pacer is quiet: the offer still starts, its scare beat calm, nothing recorded
    Rig q;
    CHECK(q.h.start(q.pool, q.rng, s, &quiet) == Start::Started);
    CHECK(q.h.scareForm() == ScareForm::Calm);
    CHECK(quiet.inWindow() == 0);
}

TEST_CASE("one at a time, then the gap; a scripted one skips the gap but not a running one") {
    Rig r;
    CHECK(r.start() == Start::Started);
    CHECK(r.start() == Start::Busy);
    CHECK(r.start(true, 1) == Start::Busy);
    r.run(29.75);
    CHECK(r.h.active());
    r.run(0.25);
    CHECK_FALSE(r.h.active());
    CHECK(r.saw(Ev::Expired) == 1);
    CHECK(r.start() == Start::Gap);
    r.run(89.75);
    CHECK(r.start() == Start::Gap);
    r.run(0.25);
    CHECK(r.start() == Start::Started);
    r.run(30.0);
    CHECK(r.start() == Start::Gap);
    CHECK(r.start(true, 0) == Start::Started);
    CHECK(r.h.key() == 0);
}

TEST_CASE("a lead switch tears the open one, ends its hold, and holds off new ones for switchSeconds") {
    Rig r;
    CHECK(r.start() == Start::Started);
    r.in.accepting = true;
    r.run(2.0);
    CHECK(r.h.holdLevel() == doctest::Approx(0.5));
    r.in.lead = kLeadB;
    r.step(0.25);
    CHECK(r.saw(Ev::Torn) == 1);
    CHECK_FALSE(r.h.active());
    CHECK(r.h.holdLevel() == 0.f);

    Rig s;
    s.in.lead = kLeadB;
    s.step(0.25);
    CHECK(s.start() == Start::SwitchDelay);
    s.run(19.75);
    CHECK(s.start() == Start::SwitchDelay);
    s.run(0.25);
    CHECK(s.start() == Start::Started);
    CHECK(s.pool.lead(s.h.key()) == kLeadB);

    Rig c; // a scripted one does not wait for the switch delay
    c.in.lead = kLeadB;
    c.step(0.25);
    CHECK(c.start() == Start::SwitchDelay);
    CHECK(c.start(true) == Start::Started);
    CHECK(c.pool.lead(c.h.key()) == kLeadB);

    Rig h; // a switch while held tears it too, and the lead is free
    h.start();
    h.in.accepting = true;
    h.run(4.0);
    REQUIRE(h.h.held());
    h.in.lead = kLeadB;
    h.step(0.25);
    CHECK(h.saw(Ev::Torn) == 1);
    CHECK_FALSE(h.h.held());
    CHECK_FALSE(h.h.active());
    CHECK(h.h.holdLevel() == 0.f);
}

TEST_CASE("a bad step counts as no time at all") {
    Rig r;
    r.start();
    const double t = r.h.now();
    r.step(-5.0);
    r.step(std::nan(""));
    r.step(HUGE_VAL);
    CHECK(r.h.now() == t);
    CHECK(r.h.active());
    r.run(30.0);
    CHECK(r.saw(Ev::Expired) == 1);
    CHECK(r.start() == Start::Gap);
}

TEST_CASE("only accepting builds the hold: holdSeconds to held, and stepping out drops it to nothing") {
    Rig r;
    CHECK(r.start() == Start::Started);
    r.in.partnerPresent = true;
    r.in.inFight = true;
    r.run(10.0);
    CHECK(r.h.holdLevel() == 0.f);
    r.in.accepting = true;
    r.run(2.0);
    CHECK(r.h.holdLevel() == doctest::Approx(0.5));
    CHECK(r.saw(Ev::HoldBegan) == 1);
    r.in.accepting = false;
    r.step(0.25);
    CHECK(r.h.holdLevel() == 0.f);
    r.in.accepting = true;
    r.run(3.75);
    CHECK_FALSE(r.h.held());
    r.step(0.25);
    CHECK(r.h.held());
    CHECK(r.saw(Ev::HoldBegan) == 2);
    CHECK(r.saw(Ev::Held) == 1);
}

TEST_CASE("a held lead stays held past the expiry until refused or pulled back") {
    Rig r;
    r.start();
    r.in.accepting = true;
    r.run(4.0);
    REQUIRE(r.h.held());
    r.in.accepting = false;
    r.in.inFight = true;
    r.run(60.0); // no partner here
    CHECK(r.h.held());
    CHECK(r.saw(Ev::Expired) == 0);
    r.in.inFight = false;
    r.in.partnerPresent = true;
    r.run(10.0); // outside a fight only a pull ends it
    CHECK(r.h.held());
    r.in.partnerPull = true;
    r.step(0.25);
    CHECK(r.saw(Ev::PulledBack) == 1);
    CHECK_FALSE(r.h.active());

    Rig f;
    f.in.partnerPresent = true;
    f.in.inFight = true;
    f.start();
    f.in.accepting = true;
    f.run(4.0);
    REQUIRE(f.h.held());
    f.run(5.75);
    CHECK(f.h.held());
    f.step(0.25);
    CHECK(f.saw(Ev::PulledBack) == 1);

    Rig g;
    g.start();
    g.in.accepting = true;
    g.run(4.0);
    g.in.refuse = true;
    g.run(0.5);
    CHECK(g.saw(Ev::Refused) == 1);
}

TEST_CASE("a held lead is blank to the fight: the guard feeds only the refusal, and a partial hold slows the stick") {
    const ArenaInput raw = everyInput();
    Rig r;
    CHECK(same(hallucinationArenaInput(r.h, raw), raw)); // no offer: all of it goes through
    r.start();
    CHECK(same(hallucinationArenaInput(r.h, raw), raw)); // shown, not taken: all of it

    r.in.accepting = true;
    r.run(1.0); // a quarter of the hold: the stick at three quarters, the rest untouched
    REQUIRE(r.h.holdLevel() == doctest::Approx(0.25f));
    ArenaInput slowed = raw;
    slowed.move = Vec2{raw.move.x * 0.75f, raw.move.y * 0.75f};
    CHECK(same(hallucinationArenaInput(r.h, raw), slowed));

    r.run(3.0);
    REQUIRE(r.h.held());
    ArenaInput blank;
    blank.view = raw.view; // the camera's forward only
    CHECK(same(hallucinationArenaInput(r.h, raw), blank));

    // The host sends the raw guard to the refusal: held refuseSeconds it refuses, and not once does the fight see it.
    r.in.accepting = false;
    int guarded = 0, steps = 0;
    while (r.h.held() && steps < 8) {
        r.in.refuse = raw.guard;
        r.step(0.25);
        ++steps;
        if (r.h.held()) guarded += hallucinationArenaInput(r.h, raw).guard ? 1 : 0;
    }
    CHECK(r.saw(Ev::Refused) == 1);
    CHECK(steps == 2); // 0.5 s
    CHECK(guarded == 0);
    ArenaInput kept = raw; // refused with the guard still down: the press stays the refusal's, no guard and no parry
    kept.guard = false;
    CHECK(same(hallucinationArenaInput(r.h, raw), kept));
    r.step(0.25);
    CHECK(same(hallucinationArenaInput(r.h, raw), kept));
    r.in.refuse = false;
    r.step(0.25);
    CHECK(same(hallucinationArenaInput(r.h, raw), raw)); // let go: the next press is a guard

    Rig t; // torn by a switch with the guard down: that press stays the refusal's too
    t.start();
    t.in.accepting = true;
    t.run(4.0);
    REQUIRE(t.h.held());
    t.in.refuse = true;
    t.step(0.25);
    t.in.lead = kLeadB;
    t.step(0.25);
    CHECK(t.saw(Ev::Torn) == 1);
    CHECK(t.saw(Ev::Refused) == 0);
    CHECK_FALSE(hallucinationArenaInput(t.h, raw).guard);
    t.in.refuse = false;
    t.step(0.25);
    CHECK(hallucinationArenaInput(t.h, raw).guard);

    Rig o; // a guard down on an offer only shown or holding is the fight's as well as the refusal's
    o.start();
    o.in.refuse = true;
    o.step(0.25);
    CHECK(o.h.active());
    CHECK_FALSE(o.h.refuseKept());
    CHECK(hallucinationArenaInput(o.h, raw).guard);
    o.step(0.25);
    CHECK(o.saw(Ev::Refused) == 1);
    CHECK_FALSE(o.h.refuseKept());
    CHECK(hallucinationArenaInput(o.h, raw).guard);

    Rig p; // pulled back with the guard down (pressed under refuseSeconds before the pull): that press stays the refusal's
    p.in.partnerPresent = true;
    p.start();
    p.in.accepting = true;
    p.run(4.0);
    REQUIRE(p.h.held());
    p.in.accepting = false;
    p.in.refuse = true;
    p.step(0.1);
    p.in.partnerPull = true;
    p.step(0.1);
    p.in.partnerPull = false;
    CHECK(p.saw(Ev::PulledBack) == 1);
    CHECK(p.saw(Ev::Refused) == 0);
    CHECK_FALSE(p.h.active());
    CHECK(p.h.refuseKept());
    CHECK(same(hallucinationArenaInput(p.h, raw), kept));
    p.step(0.25);
    CHECK(same(hallucinationArenaInput(p.h, raw), kept)); // still down: still the refusal's
    p.in.refuse = false;
    p.step(0.25);
    CHECK(same(hallucinationArenaInput(p.h, raw), raw)); // let go
    p.in.refuse = true;
    p.step(0.25);
    CHECK_FALSE(p.h.refuseKept()); // a guard first pressed after the pull-back is a plain guard
    CHECK(hallucinationArenaInput(p.h, raw).guard);
}

TEST_CASE("the refusal: held refuseSeconds it closes the offer; a wrong guess, the cooldown and a stale button") {
    Rig r;
    r.start();
    r.in.refuse = true;
    r.step(0.25);
    CHECK(r.h.active());
    r.step(0.25);
    CHECK(r.saw(Ev::Refused) == 1);
    CHECK(r.h.refusalCooldown() == doctest::Approx(4.0));

    Rig w; // a refusal with nothing offered
    w.in.refuse = true;
    w.step(0.25);
    w.in.refuse = false;
    w.step(0.25);
    CHECK(w.h.refusalCooldown() == doctest::Approx(3.75));
    CHECK(w.start() == Start::Started);
    w.in.refuse = true;
    w.run(1.0); // pressed while cooling: nothing
    CHECK(w.h.active());
    w.in.refuse = false;
    w.run(3.0);
    CHECK(w.h.refusalCooldown() == 0.f);
    w.in.refuse = true;
    w.run(0.5);
    CHECK(w.saw(Ev::Refused) == 1);

    Rig s; // the button already down when the offer starts
    s.in.refuse = true;
    s.run(4.0);
    CHECK(s.start() == Start::Started);
    s.run(2.0);
    CHECK(s.h.active());
    s.in.refuse = false;
    s.step(0.25);
    s.in.refuse = true;
    s.run(0.5);
    CHECK(s.saw(Ev::Refused) == 1);

    Rig t; // turning away closes it, with no cooldown
    t.start();
    t.in.turnedAway = true;
    t.step(0.25);
    CHECK(t.saw(Ev::Refused) == 1);
    CHECK(t.h.refusalCooldown() == 0.f);
}

TEST_CASE("the band is the place's or the act's floor, whichever is higher") {
    CHECK(intensityBand(IntensityBand::Low, 0) == IntensityBand::Low);
    CHECK(intensityBand(IntensityBand::None, 3) == IntensityBand::Low);
    CHECK(intensityBand(IntensityBand::None, 4) == IntensityBand::Mid);
    CHECK(intensityBand(IntensityBand::High, 0) == IntensityBand::High);
    CHECK(intensityBand(IntensityBand::Mid, 3) == IntensityBand::Mid);
    CHECK(intensityBand(IntensityBand::None, -5) == IntensityBand::None);
    CHECK(intensityBand(IntensityBand::None, 99) == IntensityBand::Mid);
    Rig r;
    CHECK(r.start(false, -1, IntensityBand::None) == Start::NoBand);
    CHECK(r.start(false, -1, IntensityBand::Mid) == Start::Started);
    CHECK(r.h.band() == IntensityBand::Mid);
    r.run(30.0);
    CHECK(r.h.band() == IntensityBand::None);
}

TEST_CASE("StoryOnly starts only scripted ones; Fewer stretches the gap") {
    Rig s(2, true, HallucinationFrequency::StoryOnly);
    CHECK(s.start() == Start::StoryOnly);
    CHECK(s.start(true) == Start::Started);

    Rig f(2, true, HallucinationFrequency::Fewer);
    CHECK(f.start() == Start::Started);
    f.run(30.0);
    f.run(90.0);
    CHECK(f.start() == Start::Gap);
    f.run(89.75);
    CHECK(f.start() == Start::Gap);
    f.run(0.25);
    CHECK(f.start() == Start::Started);
}

TEST_CASE("a long run of offers, holds, refusals and switches allocates nothing") {
    Rig r(8);
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    int started = 0;
    for (int i = 0; i < 20000; ++i) {
        r.in.accepting = (i / 40) % 3 == 0;
        r.in.refuse = (i / 7) % 11 == 0;
        r.in.lead = (i / 3000) % 2 == 0 ? kLeadA : kLeadB;
        r.in.partnerPresent = (i / 500) % 2 == 0;
        r.in.inFight = true;
        if (r.start(i % 900 == 0, -1, IntensityBand::Mid) == Start::Started) ++started;
        r.step(0.25);
    }
    const auto allocs = ftcore_test::allocStats().count - before;
    if (counting) CHECK(allocs == 0);
    CHECK(started > 10);
}

TEST_CASE("the shipped hallucination tables load, and bad ones are refused") {
    std::string text;
    REQUIRE(ftcore_test::readProjectData("Unwritten/tragedies.json", text));
    TragedyTable t;
    std::string err;
    CHECK(importTragedies(text, t, &err));
    CHECK(err.empty());
    REQUIRE(t.keys.size() == 2);
    CHECK(t.keys[0].lead == kLeadA);
    CHECK(t.keys[1].lead == kLeadB);
    CHECK(t.keys[1].places == tragedyPlaceBit(t, "school"));
    CHECK(tragedyKey(t, "trag_b01") == 1);
    CHECK(t.keys[0].places == tragedyPlaceBit(t, "playground"));
    CHECK(t.keys[0].voice == "vo_trag_a02");
    CHECK(t.keys[0].figure == "fig_trag_a02");

    REQUIRE(ftcore_test::readProjectData("Unwritten/hallucination.json", text));
    HallucinationTuning h;
    h.holdSeconds = 1.f;
    CHECK(importHallucinationTuning(text, h, &err));
    const HallucinationTuning d;
    CHECK(h.holdSeconds == d.holdSeconds);
    CHECK(h.refuseSeconds == d.refuseSeconds);
    CHECK(h.actFloors == d.actFloors);
    CHECK(h.lonePoolFloor == d.lonePoolFloor);
    CHECK(sameTuning(h, d)); // the shipped file is the defaults
    CHECK(t.keys[0].caused == false);
    CHECK(t.keys[1].lone == false);

    HallucinationTuning o;
    CHECK(importHallucinationTuning(R"({"version":1,"holdSeconds":2,"expireSeconds":40,"gapSeconds":60,"switchSeconds":10,
        "refuseSeconds":0.75,"refusalCooldownSeconds":3,"pullSeconds":8,"recentSeconds":600,"fewerGapScale":3,"lonePoolFloor":50,
        "counselCooldownSeconds":1,"proxyReactSeconds":6,"escrowDelaySeconds":30,"actFloors":["low","low","mid","mid","high"]})", o, &err));
    CHECK(o.holdSeconds == 2.f);
    CHECK(o.expireSeconds == 40.f);
    CHECK(o.gapSeconds == 60.f);
    CHECK(o.switchSeconds == 10.f);
    CHECK(o.refuseSeconds == 0.75f);
    CHECK(o.refusalCooldownSeconds == 3.f);
    CHECK(o.pullSeconds == 8.f);
    CHECK(o.recentSeconds == 600.f);
    CHECK(o.fewerGapScale == 3.f);
    CHECK(o.lonePoolFloor == 50.f);
    CHECK(o.counselCooldownSeconds == 1.f);
    CHECK(o.proxyReactSeconds == 6.f);
    CHECK(o.escrowDelaySeconds == 30.f);
    CHECK(o.actFloors[0] == IntensityBand::Low);
    CHECK(o.actFloors[2] == IntensityBand::Mid);
    CHECK(o.actFloors[4] == IntensityBand::High);

    const char* const badTables[] = {
        R"({"version":2,"places":["p"],"keys":[]})",
        R"({"version":1,"places":["p"],"keys":[],"extra":1})",
        R"({"version":1,"places":["p","p"],"keys":[]})",
        R"({"version":1,"places":["p"],"keys":[{"id":"x","lead":"c","places":["p"],"voice":"v","figure":"f"}]})",
        R"({"version":1,"places":["p"],"keys":[{"id":"x","lead":"a","places":["q"],"voice":"v","figure":"f"}]})",
        R"({"version":1,"places":["p"],"keys":[{"id":"x","lead":"a","places":["p"],"figure":"f"}]})",
        R"({"version":1,"places":["p"],"keys":[{"id":"x","lead":"a","places":["p"],"voice":"v","figure":"f","line":"t"}]})",
        R"({"version":1,"places":["p"],"keys":[{"id":"x","lead":"a","places":["p"],"voice":"v","figure":"f","caused":1}]})",
        R"({"version":1,"places":["p"],"keys":[{"id":"x","lead":"a","places":["p"],"voice":"v","figure":"f","lone":"yes"}]})",
        R"({"version":1,"places":["p"],"keys":[{"id":"x","lead":"a","places":["p"],"voice":"v","figure":"f"},
                                              {"id":"x","lead":"b","places":["p"],"voice":"v","figure":"f"}]})",
    };
    for (const char* bad : badTables) {
        CHECK_FALSE(importTragedies(bad, t, &err));
        CHECK_FALSE(err.empty());
        CHECK(t.keys.empty());
    }
    std::string many = R"({"version":1,"places":["p"],"keys":[)";
    for (int i = 0; i < 65; ++i)
        many += std::string(i ? "," : "") + R"({"id":"k)" + std::to_string(i) + R"(","lead":"a","places":["p"],"voice":"v","figure":"f"})";
    CHECK_FALSE(importTragedies(many + "]}", t, &err));
    REQUIRE(importTragedies(R"({"version":1,"places":["p"],"keys":[{"id":"x","lead":"a","places":["p"],"voice":"v",
        "figure":"f","caused":true,"lone":true},{"id":"y","lead":"b","places":["p"],"voice":"v","figure":"f","caused":false}]})",
                            t, &err));
    CHECK(t.keys[0].caused);
    CHECK(t.keys[0].lone);
    CHECK_FALSE(t.keys[1].caused);
    CHECK_FALSE(t.keys[1].lone);

    const char* const badTuning[] = {
        R"({"version":1,"holdSeconds":-1})",
        R"({"version":1,"actFloors":["none","none","low","mid"]})",
        R"({"version":1,"actFloors":["none","none","none","low","max"]})",
        R"({"version":1,"gap":90})",
        R"({"holdSeconds":4})",
        R"({"version":1,"lonePoolFloor":-1})",
        R"({"version":1,"lonePoolFloor":2e9})",
        R"({"version":1,"counselCooldownSeconds":-1})",
        R"({"version":1,"counselCooldownSeconds":61})",
        R"({"version":1,"proxyReactSeconds":-1})",
        R"({"version":1,"proxyReactSeconds":6.5})",
        R"({"version":1,"escrowDelaySeconds":-1})",
        R"({"version":1,"escrowDelaySeconds":31})",
    };
    for (const char* bad : badTuning) { // a refused import resets `out` to the defaults, never half-applied
        dirtyAll(h);
        REQUIRE(allDiffer(h, d));
        CHECK_FALSE(importHallucinationTuning(bad, h, &err));
        CHECK_MESSAGE(sameTuning(h, d), bad);
    }
}

TEST_CASE("a fight fed a held lead's input never blocks or parries, held or kept; a fresh guard after it does") {
    // What the runner probe's refuseheld counts (no block or parry in held or kept steps), in Core: the built-in fight,
    // its enemies pinned in front of the lead, the lead standing still with the guard as the only button.
    ArenaDef def = defaultArenaDef();
    def.fighter.hp *= 50;
    for (std::size_t i = 0; i < def.enemyCount; ++i) def.enemies[i].hp *= 50;
    ArenaFight fight;
    constexpr std::uint32_t kSeed = 3; // pinned: the preconditions below are this seed's, with the built-in tuning
    fight.begin(def, ArenaRing{Vec2{0.f, 0.f}, 30.f}, Vec2{0.f, 0.f}, Vec2{1.f, 0.f}, kSeed);
    ArenaBodies bodies;
    bodies.player = fight.playerStart();
    const Vec2 face = fight.playerStartFacing();
    for (std::size_t i = 0; i < fight.enemyCount(); ++i)
        bodies.enemies[i] = Vec2{bodies.player.x + face.x * 1.6f - face.y * (0.5f * static_cast<float>(i)),
                                 bodies.player.y + face.y * 1.6f + face.x * (0.5f * static_cast<float>(i))};

    TragedyTable table = makeTable(1);
    TragedyPool pool(table);
    pool.unlock(0);
    HallucinationTuning tune;
    tune.refuseSeconds = 12.f; // a long refusal, so the lead stands held with the guard down for a while
    tune.expireSeconds = 1000.f;
    Hallucination h(tune);
    HallucinationInput hin; // alone: only the refusal ends the hold
    std::uint32_t rng = 7;
    h.tick(0.0, hin);
    REQUIRE(h.start(pool, rng, 0u, IntensityBand::Low, false) == Start::Started);

    // Phases by fight time: 0 holding (guard up), 1 held with the guard down, 2 kept (refused, guard still down),
    // 3 let go, 4 a fresh guard.
    std::array<int, 5> blocks{}, attacks{}, hpLost{};
    const float dt = 1.f / 60.f;
    bool refused = false;
    ArenaAction atPress = ArenaAction::Idle; // the lead's action on the tick the fresh guard goes down
    bool pressed = false;
    int guardSteps4 = 0;                      // phase 4 ticks the lead stood in Guard
    while (fight.time() < 40.f && !fight.over()) {
        const float t = fight.time();
        hin.accepting = t < 4.5f;
        hin.refuse = (t >= 5.f && t < 27.f) || t >= 28.f;
        h.tick(dt, hin);
        for (int i = 0; i < h.eventCount(); ++i) refused = refused || h.events()[i].type == Ev::Refused;
        const int phase = t < 5.f ? 0 : !refused ? 1 : t < 27.f ? 2 : t < 28.f ? 3 : 4;
        if (phase == 1) REQUIRE(h.held());
        if (phase == 2) REQUIRE(h.refuseKept());
        ArenaInput raw;
        raw.view = face;
        raw.guard = hin.refuse; // the host's guard button is the refusal
        if (phase == 4 && !pressed) {
            pressed = true;
            atPress = fight.output().player.action;
        }
        const int before = fight.hp();
        const ArenaOutput& out = fight.tick(dt, hallucinationArenaInput(h, raw), bodies);
        hpLost[static_cast<std::size_t>(phase)] += before - fight.hp();
        guardSteps4 += phase == 4 && out.player.action == ArenaAction::Guard ? 1 : 0;
        for (const ArenaEvent& e : out.events) {
            const std::size_t p = static_cast<std::size_t>(phase);
            blocks[p] += (e.type == ArenaEventType::Blocked || e.type == ArenaEventType::Parried) ? 1 : 0;
            attacks[p] += (e.type == ArenaEventType::AttackStarted && e.source < kMaxArenaEnemies) ? 1 : 0;
        }
    }
    REQUIRE(refused);
    for (const int p : {1, 2}) { // swung at with the guard down, and not once met
        CHECK_MESSAGE(attacks[static_cast<std::size_t>(p)] > 0, "phase " << p);
        CHECK_MESSAGE(hpLost[static_cast<std::size_t>(p)] > 0, "phase " << p);
        CHECK_MESSAGE(blocks[static_cast<std::size_t>(p)] == 0, "phase " << p);
    }
    // The control's preconditions, so a tuning change fails here by name instead of as a missing block.
    REQUIRE_MESSAGE(pressed, "the fight ended before the fresh guard");
    REQUIRE_MESSAGE((atPress != ArenaAction::Hitstun && atPress != ArenaAction::GuardBreak && atPress != ArenaAction::Downed),
                    "the lead is staggered when the fresh guard goes down (action " << static_cast<int>(atPress) << ")");
    REQUIRE_MESSAGE(attacks[4] > 0, "no enemy swings after the fresh guard");
    REQUIRE_MESSAGE(guardSteps4 > 0, "the lead never stands in Guard after the fresh press");
    CHECK(blocks[4] > 0); // the same guard, pressed afresh, meets the same swings
}

TEST_CASE("the unlock order is saved by id: a restore into a reordered table keeps each lead's newest key") {
    TragedyTable a = makeTable(2); // a0 a1 b0 b1
    a.keys[0].caused = true;
    TragedyPool pa(a);
    for (const char* id : {"a1", "b1", "b0", "a0"}) REQUIRE(pa.unlock(tragedyKey(a, id)));
    REQUIRE(pa.newest(kLeadA) == tragedyKey(a, "a0")); // unlocked last, though first in the table
    TragedySave s;
    CHECK(captureTragedies(a, pa, s) == 0);
    CHECK(s.unlocked == std::vector<std::string>{"a1", "b1", "b0", "a0"});

    TragedyTable b; // the same keys, re-imported in another order
    b.places = a.places;
    for (const char* id : {"b1", "a0", "b0", "a1"}) b.keys.push_back(a.keys[static_cast<std::size_t>(tragedyKey(a, id))]);
    TragedyPool pb(b);
    CHECK(applyTragedies(s, b, pb) == 0);
    CHECK(pb.newest(kLeadA) == tragedyKey(b, "a0"));
    CHECK(pb.newest(kLeadB) == tragedyKey(b, "b0"));
    for (const char* id : {"a0", "a1", "b0", "b1"})
        CHECK_MESSAGE(pb.weight(tragedyKey(b, id)) == pa.weight(tragedyKey(a, id)), id);
    CHECK(pb.weight(tragedyKey(b, "a0")) == 6u); // newest x3, caused x2
    TragedySave again;
    CHECK(captureTragedies(b, pb, again) == 0);
    CHECK(again == s);

    // Restored as a set in table order instead, a1 would be the newest: the order is what the save keeps.
    TragedyPool set(b);
    for (int k = 0; k < static_cast<int>(b.keys.size()); ++k) set.unlock(k);
    CHECK(set.newest(kLeadA) == tragedyKey(b, "a1"));

    TragedyTable c = b; // a key gone from the table: skipped and counted, the rest keep their order
    c.keys.erase(c.keys.begin() + tragedyKey(c, "a0"));
    TragedyPool pc(c);
    CHECK(applyTragedies(s, c, pc) == 1);
    CHECK(pc.newest(kLeadA) == tragedyKey(c, "a1"));
    CHECK(pc.newest(kLeadB) == tragedyKey(c, "b0"));
    CHECK(applyTragedies(TragedySave{}, c, pc) == 0);
}

TEST_CASE("place bleed: only a High band bleeds a place in, and only while its offer is open") {
    CHECK_FALSE(placeBleeds(IntensityBand::None));
    CHECK_FALSE(placeBleeds(IntensityBand::Low));
    CHECK_FALSE(placeBleeds(IntensityBand::Mid));
    CHECK(placeBleeds(IntensityBand::High));
    CHECK(placeBleeds(intensityBand(IntensityBand::High, 0))); // the place's own High, under any act floor
    CHECK_FALSE(placeBleeds(intensityBand(IntensityBand::Low, kMaxAct))); // the act floors stop at Mid
    for (const IntensityBand b : {IntensityBand::Low, IntensityBand::Mid, IntensityBand::High}) {
        Rig r;
        CHECK_FALSE(r.h.bleeds());
        REQUIRE(r.start(false, -1, b) == Start::Started);
        CHECK(r.h.bleeds() == (b == IntensityBand::High));
        r.in.accepting = true;
        r.run(4.0);
        REQUIRE(r.h.held());
        CHECK(r.h.bleeds() == (b == IntensityBand::High)); // held, it still bleeds
    }

    // Every close of an open High offer ends the bleed: the page is its own again.
    const auto open = [](Rig& r) {
        REQUIRE(r.start(false, -1, IntensityBand::High) == Start::Started);
        REQUIRE(r.h.bleeds());
    };
    Rig refused;
    open(refused);
    refused.in.refuse = true;
    refused.run(0.5);
    REQUIRE(refused.saw(Ev::Refused) == 1);
    CHECK_FALSE(refused.h.bleeds());

    Rig expired;
    open(expired);
    expired.run(30.0);
    REQUIRE(expired.saw(Ev::Expired) == 1);
    CHECK_FALSE(expired.h.bleeds());

    Rig torn;
    open(torn);
    torn.in.lead = kLeadB;
    torn.step(0.25);
    REQUIRE(torn.saw(Ev::Torn) == 1);
    CHECK_FALSE(torn.h.bleeds());

    Rig pulled;
    open(pulled);
    pulled.in.accepting = true;
    pulled.run(4.0);
    REQUIRE(pulled.h.held());
    REQUIRE(pulled.h.bleeds());
    pulled.in.accepting = false;
    pulled.in.partnerPresent = true;
    pulled.in.partnerPull = true;
    pulled.step(0.25);
    REQUIRE(pulled.saw(Ev::PulledBack) == 1);
    CHECK_FALSE(pulled.h.bleeds());

    Rig away;
    open(away);
    away.in.turnedAway = true;
    away.step(0.25);
    REQUIRE(away.saw(Ev::Refused) == 1);
    CHECK_FALSE(away.h.bleeds());
}

TEST_CASE("a capture against a smaller table counts the pool keys it has no row for and leaves them out") {
    TragedyTable big = makeTable(2); // a0 a1 b0 b1
    TragedyPool pool(big);
    for (const char* id : {"b1", "a0", "b0", "a1"}) REQUIRE(pool.unlock(tragedyKey(big, id)));
    TragedyTable small = big;
    small.keys.resize(2); // a0 a1: b0 and b1 (rows 2 and 3) are past its end
    TragedySave s;
    s.unlocked = {"stale"}; // a capture replaces what was there
    CHECK(captureTragedies(small, pool, s) == 2);
    CHECK(s.unlocked == std::vector<std::string>{"a0", "a1"});
    TragedyPool locked(big);
    CHECK(captureTragedies(small, locked, s) == 0); // a locked key past the end is not a dropped one
    CHECK(s.unlocked.empty());
}

TEST_CASE("Counsel cuts the refusal cooldown to min(tuned, counselCooldownSeconds) and never lengthens it") {
    const auto cooldownAfter = [](float tuned, BriefId brief, bool wrongGuess) {
        HallucinationTuning t;
        t.refusalCooldownSeconds = tuned;
        Rig r;
        r.h = Hallucination(t);
        r.in.brief = brief;
        r.step(0.0);
        if (!wrongGuess) REQUIRE(r.start() == Start::Started);
        r.in.refuse = true;
        r.step(0.25);
        if (!wrongGuess) {
            r.step(0.25);
            REQUIRE(r.saw(Ev::Refused) == 1);
        }
        return r.h.refusalCooldown();
    };
    for (const bool wrongGuess : {false, true}) {
        CAPTURE(wrongGuess);
        CHECK(cooldownAfter(4.f, BriefId::Counsel, wrongGuess) == doctest::Approx(2.0)); // the default 4 s gives 2 s
        CHECK(cooldownAfter(1.5f, BriefId::Counsel, wrongGuess) == doctest::Approx(1.5)); // a tuned 1.5 s stays
        CHECK(cooldownAfter(4.f, BriefId::Count, wrongGuess) == doctest::Approx(4.0));   // no Brief: unchanged
        CHECK(cooldownAfter(4.f, BriefId::Proxy, wrongGuess) == doctest::Approx(4.0));   // another Brief: unchanged
    }
    const HallucinationTuning d;
    CHECK(d.counselCooldownSeconds == 2.f);
}

TEST_CASE("a Proxy partner refuses for a held lead: react, press, Refused; the fight's pull wins if it comes first") {
    // Held at 10 s in a fight with the partner present.
    const auto heldAt10 = [](Rig& r, BriefId partner, bool able) {
        r.in.partnerPresent = true;
        r.in.inFight = true;
        r.in.partnerBrief = partner;
        r.in.partnerAble = able;
        REQUIRE(r.start() == Start::Started);
        r.run(6.0);
        r.in.accepting = true;
        r.run(4.0);
        REQUIRE(r.h.held());
        REQUIRE(r.h.now() == doctest::Approx(10.0));
        r.in.accepting = false;
    };

    Rig a; // able throughout: the press starts at 11 s and closes the offer at 11.5 s
    heldAt10(a, BriefId::Proxy, true);
    a.run(1.25);
    CHECK(a.h.held());
    a.step(0.25);
    CHECK(a.saw(Ev::Refused) == 1);
    CHECK(a.saw(Ev::PulledBack) == 0);
    CHECK(a.h.now() == doctest::Approx(11.5));
    CHECK_FALSE(a.h.active());
    CHECK(a.h.refusalCooldown() == doctest::Approx(4.0)); // the refusal cooldown runs from the close
    a.step(0.25);
    CHECK(a.h.refusalCooldown() == doctest::Approx(3.75));

    Rig b; // unable until 13 s: the press starts then, Refused at 13.5 s
    heldAt10(b, BriefId::Proxy, false);
    b.run(3.0);
    CHECK(b.h.held());
    b.in.partnerAble = true;
    b.step(0.25);
    CHECK(b.h.held());
    b.step(0.25);
    CHECK(b.saw(Ev::Refused) == 1);
    CHECK(b.h.now() == doctest::Approx(13.5));

    Rig c; // unable throughout: the pull at 6 s held wins
    heldAt10(c, BriefId::Proxy, false);
    c.run(5.75);
    CHECK(c.h.held());
    c.step(0.25);
    CHECK(c.saw(Ev::PulledBack) == 1);
    CHECK(c.saw(Ev::Refused) == 0);
    CHECK(c.h.now() == doctest::Approx(16.0));

    for (const BriefId other : {BriefId::Count, BriefId::Counsel, BriefId::Escrow}) { // no Proxy: today's behaviour
        Rig n;
        heldAt10(n, other, true);
        n.run(5.75);
        CHECK(n.h.held());
        n.step(0.25);
        CHECK(n.saw(Ev::PulledBack) == 1);
        CHECK(n.saw(Ev::Refused) == 0);
    }

    Rig e; // losing the ability mid-press drops the press; a fresh one starts when able
    heldAt10(e, BriefId::Proxy, true);
    e.run(1.25); // pressing since 11 s
    e.in.partnerAble = false;
    e.step(0.25); // unable 11.25 to 11.5 s: no close
    CHECK(e.h.held());
    e.in.partnerAble = true;
    e.step(0.25); // a fresh press from 11.5 s
    CHECK(e.h.held());
    e.step(0.25);
    CHECK(e.saw(Ev::Refused) == 1);
    CHECK(e.h.now() == doctest::Approx(12.0));

    Rig g; // a held Counsel lead's cooldown is cut
    g.in.brief = BriefId::Counsel;
    heldAt10(g, BriefId::Proxy, true);
    g.run(1.5);
    REQUIRE(g.saw(Ev::Refused) == 1);
    CHECK(g.h.refusalCooldown() == doctest::Approx(2.0));

    Rig s; // a partner not present does nothing
    heldAt10(s, BriefId::Proxy, true);
    s.in.partnerPresent = false;
    s.run(10.0);
    CHECK(s.h.held());
    CHECK(s.saw(Ev::Refused) == 0);

    const HallucinationTuning d;
    CHECK(d.proxyReactSeconds == 1.f);
}

TEST_CASE("Escrow: the first 5 s inside an offer build no hold, so Held comes at 9 s; stepping out drops both") {
    Rig a; // staying in from 0 s
    a.in.brief = BriefId::Escrow;
    a.step(0.0);
    REQUIRE(a.start() == Start::Started);
    a.in.accepting = true;
    a.run(5.0);
    CHECK(a.h.holdLevel() == 0.f); // the lead is not slowed during the wait
    CHECK(a.saw(Ev::HoldBegan) == 0);
    a.run(3.75);
    CHECK_FALSE(a.h.held());
    CHECK(a.saw(Ev::HoldBegan) == 1);
    a.step(0.25);
    CHECK(a.h.held());
    CHECK(a.saw(Ev::Held) == 1);
    CHECK(a.h.now() == doctest::Approx(9.0));

    Rig b; // out at 4.9 s and back in: the 5 s start again
    b.in.brief = BriefId::Escrow;
    b.step(0.0);
    REQUIRE(b.start() == Start::Started);
    b.in.accepting = true;
    b.step(4.9);
    b.in.accepting = false;
    b.step(0.1);
    b.in.accepting = true;
    b.step(4.9);
    CHECK(b.h.holdLevel() == 0.f); // without the restart 3.8 s of hold would be built here
    b.step(0.1);
    b.run(3.75);
    CHECK_FALSE(b.h.held());
    b.run(0.5);
    CHECK(b.h.held());

    Rig c; // out at 7 s, with 2 s of hold built: both drop to nothing, and the next entry waits 5 s again
    c.in.brief = BriefId::Escrow;
    c.step(0.0);
    REQUIRE(c.start() == Start::Started);
    c.in.accepting = true;
    c.run(7.0);
    CHECK(c.h.holdLevel() == doctest::Approx(0.5));
    c.in.accepting = false;
    c.step(0.25);
    CHECK(c.h.holdLevel() == 0.f);
    c.in.accepting = true;
    c.run(5.0);
    CHECK(c.h.holdLevel() == 0.f);
    c.run(3.75);
    CHECK_FALSE(c.h.held());
    c.step(0.25);
    CHECK(c.h.held());

    for (const BriefId other : {BriefId::Count, BriefId::Counsel, BriefId::Proxy}) { // without Escrow: Held at 4 s
        Rig n;
        n.in.brief = other;
        n.step(0.0);
        REQUIRE(n.start() == Start::Started);
        n.in.accepting = true;
        n.run(3.75);
        CHECK_FALSE(n.h.held());
        n.step(0.25);
        CHECK(n.h.held());
        CHECK(n.h.now() == doctest::Approx(4.0));
    }

    Rig e; // an offer ending during the wait leaves nothing for the next one
    e.in.brief = BriefId::Escrow;
    e.step(0.0);
    REQUIRE(e.start(true) == Start::Started);
    e.in.accepting = true;
    e.run(3.0);
    e.in.lead = kLeadB; // torn
    e.step(0.25);
    REQUIRE(e.saw(Ev::Torn) == 1);
    REQUIRE(e.start(true) == Start::Started);
    e.run(4.75);
    CHECK(e.h.holdLevel() == 0.f);

    const HallucinationTuning d;
    CHECK(d.escrowDelaySeconds == 5.f);
}

TEST_CASE("a Proxy press broken by the partner leaving, losing Proxy or being unable starts afresh: Refused at 12.1 s") {
    // Held at 10 s; the partner presses from 11.0 s, breaks from 11.2 s to 11.6 s, and is back at 11.6 s.
    enum class Break { Absent, BriefChanged, Unable };
    for (const Break how : {Break::Absent, Break::BriefChanged, Break::Unable}) {
        CAPTURE(static_cast<int>(how));
        Rig r;
        r.in.partnerPresent = true;
        r.in.inFight = true;
        r.in.partnerBrief = BriefId::Proxy;
        r.in.partnerAble = true;
        REQUIRE(r.start() == Start::Started);
        r.run(6.0);
        r.in.accepting = true;
        r.run(4.0);
        REQUIRE(r.h.held());
        r.in.accepting = false;
        for (int i = 0; i < 12; ++i) r.step(0.1); // to 11.2 s: pressing since 11.0 s
        REQUIRE(r.h.held());
        if (how == Break::Absent) r.in.partnerPresent = false;
        if (how == Break::BriefChanged) r.in.partnerBrief = BriefId::Count;
        if (how == Break::Unable) r.in.partnerAble = false;
        for (int i = 0; i < 4; ++i) r.step(0.1); // 11.2 to 11.6 s
        REQUIRE(r.h.held());
        r.in.partnerPresent = true;
        r.in.partnerBrief = BriefId::Proxy;
        r.in.partnerAble = true;
        for (int i = 0; i < 4; ++i) r.step(0.1); // 11.9 s would be the frozen press's close
        CHECK(r.h.held());
        r.step(0.1);
        CHECK(r.saw(Ev::Refused) == 1);
        CHECK(r.h.now() == doctest::Approx(12.1));
    }
}

TEST_CASE("Proxy timing: Refused lands within one step of Held + 1.5 s at 1/30, 1/60 and 1/120 s steps") {
    for (const double dt : {1.0 / 30.0, 1.0 / 60.0, 1.0 / 120.0}) {
        CAPTURE(dt);
        Rig r;
        r.in.partnerPresent = true;
        r.in.inFight = true;
        r.in.partnerBrief = BriefId::Proxy;
        r.in.partnerAble = true;
        REQUIRE(r.start() == Start::Started);
        r.in.accepting = true;
        for (int i = 0; i < 2000 && !r.h.held(); ++i) r.step(dt);
        REQUIRE(r.h.held());
        const double heldAt = r.h.now();
        r.in.accepting = false;
        for (int i = 0; i < 2000 && r.saw(Ev::Refused) == 0; ++i) r.step(dt);
        REQUIRE(r.saw(Ev::Refused) == 1);
        CHECK(r.saw(Ev::PulledBack) == 0);
        const double err = r.h.now() - (heldAt + 1.5);
        CAPTURE(err);
        CHECK(std::abs(err) <= dt + 1e-9);
    }
}

TEST_CASE("the lead's own refusal and the partner's press closing in one step count one Refused") {
    Rig r;
    r.in.partnerPresent = true;
    r.in.inFight = true;
    r.in.partnerBrief = BriefId::Proxy;
    r.in.partnerAble = true;
    REQUIRE(r.start() == Start::Started);
    r.run(6.0);
    r.in.accepting = true;
    r.run(4.0);
    REQUIRE(r.h.held());
    r.in.accepting = false;
    r.run(1.0); // 11 s: the partner's press starts
    r.in.refuse = true; // the lead's own press from 11 s
    r.step(0.25);
    CHECK(r.h.held());
    r.step(0.25); // 11.5 s: both would close here
    CHECK(r.saw(Ev::Refused) == 1);
    CHECK(r.h.eventCount() == 1);
    CHECK(r.h.now() == doctest::Approx(11.5));
    r.step(0.25);
    CHECK(r.saw(Ev::Refused) == 1);
}

TEST_CASE("Counsel's cut is the refusing lead's own: a Counsel partner leaves a lead's full cooldown") {
    Rig r;
    r.in.brief = BriefId::Count;
    r.in.partnerPresent = true;
    r.in.partnerBrief = BriefId::Counsel;
    r.in.partnerAble = true;
    REQUIRE(r.start() == Start::Started);
    r.in.refuse = true;
    r.run(0.5);
    REQUIRE(r.saw(Ev::Refused) == 1);
    CHECK(r.h.refusalCooldown() == doctest::Approx(4.0));
}

TEST_CASE("Escrow with escrowDelaySeconds 0 is no Escrow at all, step for step") {
    HallucinationTuning t;
    t.escrowDelaySeconds = 0.f;
    Rig e;
    e.h = Hallucination(t);
    e.in.brief = BriefId::Escrow;
    e.step(0.0);
    Rig n;
    n.step(0.0);
    REQUIRE(e.start() == Start::Started);
    REQUIRE(n.start() == Start::Started);
    e.in.accepting = n.in.accepting = true;
    for (int i = 0; i < 20; ++i) {
        CAPTURE(i);
        e.step(0.25);
        n.step(0.25);
        CHECK(e.h.holdLevel() == n.h.holdLevel());
        CHECK(e.h.held() == n.h.held());
        CHECK(e.saw(Ev::HoldBegan) == n.saw(Ev::HoldBegan));
        CHECK(e.saw(Ev::Held) == n.saw(Ev::Held));
        CHECK_FALSE(e.h.escrowWaiting());
    }
    CHECK(e.saw(Ev::Held) == 1);
}

TEST_CASE("Escrow's wait is shown: waiting with the seconds left from entry to 5 s, and not after") {
    Rig r;
    r.in.brief = BriefId::Escrow;
    r.step(0.0);
    REQUIRE(r.start() == Start::Started);
    CHECK_FALSE(r.h.escrowWaiting()); // not inside the offer yet
    r.in.accepting = true;
    for (int i = 1; i < 20; ++i) {
        CAPTURE(i);
        r.step(0.25);
        CHECK(r.h.escrowWaiting());
        CHECK(r.h.escrowWaitLeft() == doctest::Approx(5.0 - 0.25 * i));
        CHECK(r.h.holdLevel() == 0.f);
    }
    r.step(0.25); // 5 s: the wait is over
    CHECK_FALSE(r.h.escrowWaiting());
    CHECK(r.h.escrowWaitLeft() == 0.f);
    r.step(0.25);
    CHECK_FALSE(r.h.escrowWaiting());
    CHECK(r.h.holdLevel() > 0.f);
    r.in.accepting = false; // out: no wait shown
    r.step(0.25);
    CHECK_FALSE(r.h.escrowWaiting());

    Rig n; // no Escrow: never waiting
    n.step(0.0);
    REQUIRE(n.start() == Start::Started);
    n.in.accepting = true;
    n.step(0.25);
    CHECK_FALSE(n.h.escrowWaiting());
}

TEST_CASE("a tuning file without the Brief link fields keeps their defaults") {
    HallucinationTuning h;
    std::string err;
    REQUIRE(importHallucinationTuning(R"({"version":1,"holdSeconds":3,"refusalCooldownSeconds":5})", h, &err));
    const HallucinationTuning d;
    CHECK(h.holdSeconds == 3.f);
    CHECK(h.counselCooldownSeconds == d.counselCooldownSeconds);
    CHECK(h.proxyReactSeconds == d.proxyReactSeconds);
    CHECK(h.escrowDelaySeconds == d.escrowDelaySeconds);
}
