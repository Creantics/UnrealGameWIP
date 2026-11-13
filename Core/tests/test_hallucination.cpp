#include "AllocCounter.h"
#include "TestAssets.h"

#include "doctest/doctest.h"

#include "ftcore/Hallucination.h"

#include <array>
#include <string>

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

    REQUIRE(ftcore_test::readProjectData("Unwritten/hallucination.json", text));
    HallucinationTuning h;
    h.holdSeconds = 1.f;
    CHECK(importHallucinationTuning(text, h, &err));
    const HallucinationTuning d;
    CHECK(h.holdSeconds == d.holdSeconds);
    CHECK(h.refuseSeconds == d.refuseSeconds);
    CHECK(h.actFloors == d.actFloors);

    const char* const badTables[] = {
        R"({"version":2,"places":["p"],"keys":[]})",
        R"({"version":1,"places":["p"],"keys":[],"extra":1})",
        R"({"version":1,"places":["p","p"],"keys":[]})",
        R"({"version":1,"places":["p"],"keys":[{"id":"x","lead":"c","places":["p"],"voice":"v","figure":"f"}]})",
        R"({"version":1,"places":["p"],"keys":[{"id":"x","lead":"a","places":["q"],"voice":"v","figure":"f"}]})",
        R"({"version":1,"places":["p"],"keys":[{"id":"x","lead":"a","places":["p"],"figure":"f"}]})",
        R"({"version":1,"places":["p"],"keys":[{"id":"x","lead":"a","places":["p"],"voice":"v","figure":"f","line":"t"}]})",
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

    const char* const badTuning[] = {
        R"({"version":1,"holdSeconds":-1})",
        R"({"version":1,"actFloors":["none","none","low","mid"]})",
        R"({"version":1,"actFloors":["none","none","none","low","max"]})",
        R"({"version":1,"gap":90})",
        R"({"holdSeconds":4})",
    };
    for (const char* bad : badTuning) {
        CHECK_FALSE(importHallucinationTuning(bad, h, &err));
        CHECK(h.holdSeconds == d.holdSeconds);
    }
}
