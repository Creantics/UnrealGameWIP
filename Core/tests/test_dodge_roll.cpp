#include "ftcore/DodgeRoll.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

constexpr float kStep = 1.f / 60.f;

void run(const DodgeRoll& d, DodgeState& s, float seconds) {
    for (float t = 0.f; t < seconds - 1e-4f; t += kStep) d.tick(s, kStep);
}

} // namespace

TEST_CASE("A dodge's kind follows the body's speed, and it can't be hurt only inside its cover") {
    DodgeRoll d;
    DodgeState s = d.fresh();
    CHECK(s.stamina == doctest::Approx(100.f));
    CHECK(d.kindFor(s, 0.f, false) == DodgeKind::Short);
    CHECK(d.kindFor(s, 499.f, true) == DodgeKind::Short);
    CHECK(d.kindFor(s, 500.f, true) == DodgeKind::Long);

    REQUIRE(d.start(s, 700.f, true));
    CHECK(s.kind == DodgeKind::Long);
    CHECK(s.speed == doctest::Approx(700.f)); // the sprint's own speed, above longSpeed
    CHECK(s.length == doctest::Approx(0.8f));
    CHECK(s.stamina == doctest::Approx(75.f));
    CHECK_FALSE(d.invulnerable(s)); // not yet: the cover starts at invulnStart
    run(d, s, 0.1f);
    CHECK(d.invulnerable(s));
    run(d, s, 0.35f);
    CHECK_FALSE(d.invulnerable(s)); // past invulnEnd, still rolling
    CHECK(s.active());
    run(d, s, 0.4f);
    CHECK_FALSE(s.active()); // its length gone
    CHECK_FALSE(d.invulnerable(s));

    // A walking body rolls Short, at the same cost; the host's clip length wins over the tuning's.
    DodgeState w = d.fresh();
    REQUIRE(d.start(w, 150.f, false, 1.2f));
    CHECK(w.kind == DodgeKind::Short);
    CHECK(w.speed == doctest::Approx(550.f));
    DodgeState j = d.fresh();
    REQUIRE(d.start(j, 520.f, true));
    CHECK(j.speed == doctest::Approx(650.f)); // a Long roll no slower than longSpeed
    CHECK(w.length == doctest::Approx(1.2f));
    CHECK(w.stamina == doctest::Approx(75.f));
}

TEST_CASE("The next dodge waits out the one going on and its cooldown") {
    DodgeRoll d;
    DodgeState s = d.fresh();
    REQUIRE(d.start(s, 300.f, true));
    const DodgeState during = s;
    CHECK_FALSE(d.start(s, 300.f, true)); // mid-dodge: refused, nothing changes
    CHECK(s.stamina == during.stamina);
    CHECK(s.time == during.time);
    run(d, s, 0.85f);
    REQUIRE_FALSE(s.active());
    CHECK(s.cooldown > 0.f);
    CHECK_FALSE(d.canStart(s, true));
    run(d, s, 0.27f);
    CHECK(d.canStart(s, true));

    // stop() ends one early (a cut-short clip), and the cooldown runs from there.
    REQUIRE(d.start(s, 300.f, true));
    run(d, s, 0.2f);
    d.stop(s);
    CHECK_FALSE(s.active());
    CHECK(s.cooldown == doctest::Approx(0.25f));
    d.stop(s); // twice is harmless
    CHECK(s.cooldown == doctest::Approx(0.25f));
}

TEST_CASE("Out of stamina a dodge is Tired: slow, with only its share of the cover, and stamina comes back after a wait") {
    DodgeTuning t;
    t.cooldown = 0.f;
    t.tiredInvuln = 0.5f;
    DodgeRoll d;
    d.setTuning(t);
    DodgeState s = d.fresh();
    for (int i = 0; i < 4; ++i) {
        REQUIRE(d.start(s, 300.f, true));
        CHECK(s.kind == DodgeKind::Short);
        run(d, s, 0.85f);
    }
    CHECK(s.stamina < 25.f); // what came back mid-run is less than a dodge
    REQUIRE(d.start(s, 800.f, true));
    CHECK(s.kind == DodgeKind::Tired);
    CHECK(s.speed == doctest::Approx(300.f));
    CHECK(s.stamina == 0.f);
    // Covered for half of 0.05-0.4: to 0.225.
    run(d, s, 0.1f);
    CHECK(d.invulnerable(s));
    run(d, s, 0.15f);
    CHECK_FALSE(d.invulnerable(s));
    run(d, s, 0.6f);
    REQUIRE_FALSE(s.active());

    // Nothing comes back for regenDelay after the spend, then staminaRegen a second.
    DodgeState r = d.fresh();
    REQUIRE(d.start(r, 300.f, true));
    run(d, r, 0.9f);
    CHECK(r.stamina == doctest::Approx(75.f));
    run(d, r, 0.6f); // half a second past the delay
    CHECK(r.stamina == doctest::Approx(90.f).epsilon(0.02));
    run(d, r, 1.f);
    CHECK(r.stamina == doctest::Approx(100.f)); // no more than staminaMax

    // A tired roll with no share keeps no cover at all.
    d.setTuning(DodgeTuning{});
    DodgeState e = d.fresh();
    e.stamina = 10.f;
    REQUIRE(d.start(e, 300.f, true));
    CHECK(e.kind == DodgeKind::Tired);
    for (int i = 0; i < 60; ++i) {
        CHECK_FALSE(d.invulnerable(e));
        d.tick(e, kStep);
    }
}

TEST_CASE("Free dodges never tire, and a dodge that needs a direction waits for one") {
    DodgeTuning t;
    t.staminaCost = 0.f;
    t.cooldown = 0.f;
    DodgeRoll d;
    d.setTuning(t);
    DodgeState s = d.fresh();
    for (int i = 0; i < 20; ++i) {
        REQUIRE(d.start(s, 600.f, false));
        CHECK(s.kind == DodgeKind::Long);
        run(d, s, 0.85f);
    }
    CHECK(s.stamina == doctest::Approx(100.f));

    t.needsDirection = 1.f;
    d.setTuning(t);
    CHECK_FALSE(d.canStart(s, false));
    CHECK(d.kindFor(s, 600.f, false) == DodgeKind::None);
    CHECK_FALSE(d.start(s, 600.f, false));
    CHECK(d.start(s, 600.f, true));
}

TEST_CASE("A dodge's tick and queries don't allocate") {
    DodgeRoll d;
    DodgeState s = d.fresh();
    const auto before = ftcore_test::allocStats().count;
    for (int i = 0; i < 2000; ++i) {
        if (i % 50 == 0) d.start(s, static_cast<float>(i % 900), i % 3 != 0);
        d.tick(s, kStep);
        (void)d.invulnerable(s);
    }
    CHECK(ftcore_test::allocStats().count == before);
}
