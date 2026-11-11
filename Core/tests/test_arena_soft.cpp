#include "ftcore/ArenaSoftTarget.h"
#include "AllocCounter.h"
#include "TestAssets.h"
#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

ArenaSoftAim softAim(float range = 6.f, float arcCos = 0.3f, float homing = 2.f) {
    ArenaSoftAim a;
    a.range = range;
    a.arcCos = arcCos;
    a.homing = homing;
    return a;
}

ArenaSoftBody body(float x, float y, float radius = 0.4f, bool alive = true) {
    ArenaSoftBody b;
    b.pos = Vec2{x, y};
    b.radius = radius;
    b.alive = alive;
    return b;
}

constexpr Vec2 kAhead{1.f, 0.f};

} // namespace

TEST_CASE("arena soft target: picks in the arc and range, aligned and near") {
    const ArenaSoftAim aim = softAim();
    SUBCASE("straight ahead beats off to the side at the same distance") {
        const std::array<ArenaSoftBody, 2> b{body(3.f * 0.8f, 3.f * 0.6f), body(3.f, 0.f)};
        CHECK(arenaSoftTarget({}, kAhead, b.data(), b.size(), aim) == 1);
    }
    SUBCASE("near and a little off beats far and dead ahead") {
        const std::array<ArenaSoftBody, 2> b{body(5.8f, 0.f), body(1.f, 0.3f)};
        CHECK(arenaSoftTarget({}, kAhead, b.data(), b.size(), aim) == 1);
    }
    SUBCASE("behind, outside the arc, out of range or dead: nobody") {
        const std::array<ArenaSoftBody, 4> b{body(-2.f, 0.f), body(0.f, 2.f), body(7.f, 0.f), body(2.f, 0.f, 0.4f, false)};
        CHECK(arenaSoftTarget({}, kAhead, b.data(), b.size(), aim) == kArenaNobody);
    }
    SUBCASE("range is to the near edge: a big body whose edge is in range counts") {
        const std::array<ArenaSoftBody, 1> b{body(7.f, 0.f, 1.5f)};
        CHECK(arenaSoftTarget({}, kAhead, b.data(), b.size(), aim) == 0);
    }
    SUBCASE("range 0 is off; no bodies is nobody") {
        const std::array<ArenaSoftBody, 1> b{body(2.f, 0.f)};
        CHECK(arenaSoftTarget({}, kAhead, b.data(), b.size(), softAim(0.f)) == kArenaNobody);
        CHECK(arenaSoftTarget({}, kAhead, nullptr, 3, aim) == kArenaNobody);
        CHECK(arenaSoftTarget({}, kAhead, b.data(), 0, aim) == kArenaNobody);
    }
    SUBCASE("ties go to the nearer body, then the lower index") {
        const std::array<ArenaSoftBody, 3> mirror{body(2.f, 1.f), body(2.f, -1.f), body(2.f, 1.f)};
        CHECK(arenaSoftTarget({}, kAhead, mirror.data(), mirror.size(), aim) == 0);
        // Same score (alignment and edge distance), different centres: the bigger body is farther and loses.
        const std::array<ArenaSoftBody, 2> sizes{body(3.f, 0.f, 1.f), body(2.4f, 0.f, 0.4f)};
        CHECK(arenaSoftTarget({}, kAhead, sizes.data(), sizes.size(), aim) == 1);
    }
    SUBCASE("the direction decides: the stick turned left picks the body on the left") {
        const std::array<ArenaSoftBody, 2> b{body(3.f, 0.f), body(0.f, -3.f)};
        CHECK(arenaSoftTarget({}, Vec2{0.f, -1.f}, b.data(), b.size(), aim) == 1);
        CHECK(arenaSoftTarget({}, Vec2{0.f, -5.f}, b.data(), b.size(), aim) == 1); // not unit: same pick
    }
    SUBCASE("a body on top of the attacker counts as straight ahead") {
        const std::array<ArenaSoftBody, 1> b{body(0.f, 0.f)};
        CHECK(arenaSoftTarget({}, kAhead, b.data(), b.size(), aim) == 0);
    }
    SUBCASE("a wider arc takes what a narrow one refuses") {
        const std::array<ArenaSoftBody, 1> b{body(1.f, 2.f)}; // dot ~ 0.45
        CHECK(arenaSoftTarget({}, kAhead, b.data(), b.size(), softAim(6.f, 0.6f)) == kArenaNobody);
        CHECK(arenaSoftTarget({}, kAhead, b.data(), b.size(), softAim(6.f, 0.3f)) == 0);
        const std::array<ArenaSoftBody, 1> behind{body(-2.f, 0.f)};
        CHECK(arenaSoftTarget({}, kAhead, behind.data(), behind.size(), softAim(6.f, -1.f)) == 0); // all round
    }
}

TEST_CASE("arena soft target: the direction is the pushed stick, else the facing") {
    const Vec2 d = arenaSoftDir(Vec2{0.f, 0.5f}, kAhead);
    CHECK(d.x == doctest::Approx(0.f));
    CHECK(d.y == doctest::Approx(1.f));
    const Vec2 rest = arenaSoftDir(Vec2{}, Vec2{0.f, -2.f});
    CHECK(rest.y == doctest::Approx(-1.f));
    const Vec2 none = arenaSoftDir(Vec2{}, Vec2{});
    CHECK(none.x == doctest::Approx(1.f));
}

TEST_CASE("arena homing: closes what the lunge leaves, capped, never past touching") {
    const ArenaSoftAim aim = softAim(6.f, 0.3f, 2.f);
    SUBCASE("already in reach: no homing, faces the body") {
        const ArenaHoming h = arenaHoming({}, 0.4f, kAhead, body(0.f, 2.f), 2.f, 0.4f, aim);
        CHECK(h.close == 0.f);
        CHECK(h.reaches);
        CHECK(h.facing.y == doctest::Approx(1.f));
    }
    SUBCASE("just out of reach: closes the missing part exactly") {
        // reach 2 + 0.4 radius + 0.4 lunge = 2.8; at 3.5 it misses 0.7.
        const ArenaHoming h = arenaHoming({}, 0.4f, kAhead, body(3.5f, 0.f), 2.f, 0.4f, aim);
        CHECK(h.close == doctest::Approx(0.7f));
        CHECK(h.reaches);
    }
    SUBCASE("far: capped at the profile's homing, and says it won't reach") {
        const ArenaHoming h = arenaHoming({}, 0.4f, kAhead, body(6.f, 0.f), 2.f, 0.4f, aim);
        CHECK(h.close == doctest::Approx(2.f));
        CHECK_FALSE(h.reaches);
    }
    SUBCASE("a short attack stops where the bodies touch") {
        // range 0.2: reach 0.6; touching at 0.8; the lunge 0 -> close at most 2.0 - 0.8 = 1.2, though 1.4 is missing.
        const ArenaHoming h = arenaHoming({}, 0.4f, kAhead, body(2.f, 0.f), 0.2f, 0.f, aim);
        CHECK(h.close == doctest::Approx(1.2f));
        CHECK_FALSE(h.reaches);
    }
    SUBCASE("homing 0 is off; a body on top keeps the facing") {
        CHECK(arenaHoming({}, 0.4f, kAhead, body(5.f, 0.f), 2.f, 0.4f, softAim(6.f, 0.3f, 0.f)).close == 0.f);
        const ArenaHoming on = arenaHoming({}, 0.4f, Vec2{0.f, 1.f}, body(0.f, 0.f), 2.f, 0.4f, aim);
        CHECK(on.facing.y == doctest::Approx(1.f));
        CHECK(on.close == 0.f);
    }
}

TEST_CASE("arena homing: the distance covered is the same at any frame rate") {
    const float close = 1.3f, span = 0.35f;
    for (const float dt : {1.f / 30.f, 1.f / 60.f, 1.f / 144.f, 0.1f, 0.5f}) {
        float covered = 0.f;
        for (float t = 0.f; t < 1.f; t += dt) covered += arenaHomingStep(close, span, t, t + dt);
        CHECK(covered == doctest::Approx(close).epsilon(1e-4));
    }
    CHECK(arenaHomingStep(close, span, 0.4f, 0.5f) == 0.f); // after active
    CHECK(arenaHomingStep(close, 0.f, 0.f, 0.016f) == doctest::Approx(close)); // no span: all on the first tick
    CHECK(arenaHomingStep(close, 0.f, 0.016f, 0.032f) == 0.f);
    CHECK(arenaHomingStep(0.f, span, 0.f, 0.1f) == 0.f);
}

TEST_CASE("arena soft target: the riposte, the aim and the lock come first; never asked for while aiming or locked") {
    const Vec2 pushed{0.f, 1.f};
    CHECK(arenaAimSource(true, true, true, true, pushed) == ArenaAimSource::Riposte);
    CHECK(arenaAimSource(false, true, true, true, pushed) == ArenaAimSource::Aim);
    CHECK(arenaAimSource(false, false, true, true, pushed) == ArenaAimSource::Lock);
    CHECK(arenaAimSource(false, false, false, true, pushed) == ArenaAimSource::Soft);
    CHECK(arenaAimSource(false, false, false, false, pushed) == ArenaAimSource::Stick);
    CHECK(arenaAimSource(false, false, false, false, Vec2{}) == ArenaAimSource::Facing);
}

TEST_CASE("arena soft target: the shipped leads' profiles, fast to grounded") {
    ArenaTable t;
    std::string text, error;
    std::vector<std::string> warnings;
    if (!ftcore_test::readProjectData("Battle/arena.json", text)) FAIL("Data/Battle/arena.json not found");
    REQUIRE(importArenaTable(text, t, &error, &warnings));
    const ArenaSoftAim devin = arenaSoftAimOf(t.fighter("devin"));
    const ArenaSoftAim duy = arenaSoftAimOf(t.fighter("duy"));
    const ArenaSoftAim third = arenaSoftAimOf(t.fighter("third"));
    const ArenaSoftAim base = arenaSoftAimOf(t.fighter("default"));
    CHECK(base.range > 0.f); // the grounded baseline the third inherits
    CHECK(third.range == base.range);
    CHECK(third.homing == base.homing);
    CHECK(devin.range > duy.range);
    CHECK(duy.range > third.range);
    CHECK(devin.homing > duy.homing);
    CHECK(duy.homing > third.homing);
    CHECK(devin.arcCos < duy.arcCos); // the wider arc
    CHECK(duy.arcCos < third.arcCos);
}

TEST_CASE("arena soft target: the queries allocate nothing") {
    const std::array<ArenaSoftBody, 3> b{body(3.f, 0.f), body(2.f, 1.f), body(-1.f, 0.f)};
    const ArenaSoftAim aim = softAim();
    const std::uint64_t before = ftcore_test::allocStats().count;
    const std::uint8_t pick = arenaSoftTarget({}, kAhead, b.data(), b.size(), aim);
    REQUIRE(pick < b.size());
    const ArenaHoming h = arenaHoming({}, 0.4f, kAhead, b[pick], 1.f, 0.3f, aim);
    const float step = arenaHomingStep(h.close, 0.3f, 0.f, 0.016f);
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(step >= 0.f);
}
