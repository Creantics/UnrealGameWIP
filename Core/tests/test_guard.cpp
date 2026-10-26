#include "ftcore/ExploreDemo.h"
#include "ftcore/Guard.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 30.f;
constexpr std::uint32_t kAllClear = ~0u;

float dist(Vec2 a, Vec2 b) { return std::hypot(b.x - a.x, b.y - a.y); }

// A guard walking a square, facing +x.
GuardSpec square() {
    GuardSpec s;
    s.route[0] = {0.f, 0.f};
    s.route[1] = {6.f, 0.f};
    s.route[2] = {6.f, 6.f};
    s.route[3] = {0.f, 6.f};
    s.routeCount = 4;
    return s;
}

GuardSpec sentry(Vec2 at, Vec2 facing) {
    GuardSpec s;
    s.route[0] = at;
    s.routeCount = 1;
    s.facing = facing;
    return s;
}

// Steps until the guard enters `state` or `seconds` pass; returns the seconds taken (or -1).
float runUntil(GuardSet& g, int id, GuardState state, Vec2 target, std::uint32_t sight, float seconds) {
    for (float t = 0.f; t < seconds; t += kDt) {
        g.step(target, sight, kDt);
        if (g.state(id) == state) return t + kDt;
    }
    return -1.f;
}

} // namespace

TEST_CASE("guard: walks its route in order with a pause at each point, then loops") {
    GuardSet g;
    const int id = g.add(square());
    REQUIRE(id == 0);
    CHECK(g.state(id) == GuardState::Idle);
    CHECK(g.position(id).x == doctest::Approx(0.f));

    const Vec2 far{100.f, 100.f}; // never seen
    int visited[8] = {};
    int count = 0;
    GuardState last = g.state(id);
    float seconds = 0.f;
    while (count < 5 && seconds < 60.f) {
        g.step(far, kAllClear, kDt);
        seconds += kDt;
        if (last == GuardState::Patrol && g.state(id) == GuardState::Idle) visited[count++] = g.waypoint(id);
        last = g.state(id);
    }
    REQUIRE(count == 5);
    CHECK(visited[0] == 1);
    CHECK(visited[1] == 2);
    CHECK(visited[2] == 3);
    CHECK(visited[3] == 0);
    CHECK(visited[4] == 1);
    // Four 6 m sides at 1.5 m/s plus a 1 s pause before each: 5 legs take about 25 s.
    CHECK(seconds == doctest::Approx(25.f).epsilon(0.05));
    CHECK(g.position(id).x == doctest::Approx(6.f));
    CHECK(g.position(id).y == doctest::Approx(0.f));
}

TEST_CASE("guard: notices inside the cone or the sense radius, only with a clear line") {
    GuardSet g;
    const int id = g.add(sentry({0.f, 0.f}, {1.f, 0.f}));

    CHECK(g.inSenseRange(id, {6.f, 1.f}));    // ahead, in range
    CHECK_FALSE(g.inSenseRange(id, {9.f, 0.f})); // ahead, too far
    CHECK_FALSE(g.inSenseRange(id, {3.f, 5.f})); // in range, outside the cone
    CHECK_FALSE(g.inSenseRange(id, {-5.f, 0.f})); // behind
    CHECK(g.inSenseRange(id, {-2.f, 0.f}));   // behind but within the sense radius

    // A blocked line never notices.
    for (int i = 0; i < 60; ++i) g.step({5.f, 0.f}, 0u, kDt);
    CHECK(g.state(id) == GuardState::Idle);

    GuardEvents ev;
    g.step({5.f, 0.f}, kAllClear, kDt, &ev);
    CHECK(g.state(id) == GuardState::Notice);
    CHECK(ev.bits[0] == kGuardNoticed);

    // It stands for the reaction time, then chases.
    const float t = runUntil(g, id, GuardState::Chase, {5.f, 0.f}, kAllClear, 2.f);
    CHECK(t == doctest::Approx(0.5f).epsilon(0.1));
    CHECK(g.position(id).x < 0.2f);
}

TEST_CASE("guard: a notice that loses sight before the reaction ends goes back") {
    GuardSet g;
    const int id = g.add(sentry({0.f, 0.f}, {1.f, 0.f}));
    g.step({5.f, 0.f}, kAllClear, kDt);
    REQUIRE(g.state(id) == GuardState::Notice);
    GuardEvents ev;
    bool lost = false;
    for (int i = 0; i < 30 && !lost; ++i) {
        g.step({5.f, 0.f}, 0u, kDt, &ev);
        lost = (ev.bits[0] & kGuardLost) != 0;
    }
    CHECK(lost);
    CHECK(g.state(id) == GuardState::Return);
    g.step({50.f, 0.f}, kAllClear, kDt);
    CHECK(g.state(id) == GuardState::Idle); // a sentry is already home
    CHECK(g.facing(id).x == doctest::Approx(1.f));
}

TEST_CASE("guard: a chase follows the player, gives up out of sight and walks back to its route") {
    GuardSet g;
    const int id = g.add(square());
    Vec2 player{4.f, 1.f};
    REQUIRE(runUntil(g, id, GuardState::Chase, player, kAllClear, 2.f) > 0.f);

    // The player runs off at 6.2 m/s along +y; the guard closes in while it can see.
    for (int i = 0; i < 30; ++i) {
        player.y += 6.2f * kDt;
        g.step(player, kAllClear, kDt);
    }
    CHECK(g.state(id) == GuardState::Chase);
    CHECK(g.position(id).y > 2.f);

    // Behind a wall: the guard goes to where it last saw the player, then gives up after 2 s.
    const Vec2 seenAt = player;
    player = {40.f, 40.f};
    GuardEvents ev;
    float seconds = 0.f;
    while (g.state(id) == GuardState::Chase && seconds < 5.f) {
        g.step(player, 0u, kDt, &ev);
        seconds += kDt;
    }
    CHECK(seconds == doctest::Approx(2.f).epsilon(0.05));
    CHECK(ev.bits[0] == kGuardLost);
    CHECK(dist(g.position(id), seenAt) < 0.01f);

    // Back to the waypoint it was walking to, then on with the patrol.
    const float back = runUntil(g, id, GuardState::Idle, player, kAllClear, 30.f);
    CHECK(back > 0.f);
    const Vec2 home = g.spec(id).route[static_cast<std::size_t>(g.waypoint(id))];
    CHECK(dist(g.position(id), home) < 0.01f);
    CHECK(runUntil(g, id, GuardState::Patrol, player, kAllClear, 2.f) > 0.f);
}

TEST_CASE("guard: contact fires once, then the guard ignores the player for its cooldown") {
    GuardSet g;
    const int id = g.add(sentry({0.f, 0.f}, {1.f, 0.f}));
    const Vec2 player{3.f, 0.f}; // standing still: the guard walks up to it
    int touched = -1;
    float seconds = 0.f;
    GuardEvents ev;
    while (touched < 0 && seconds < 5.f) {
        touched = g.step(player, kAllClear, kDt, &ev);
        seconds += kDt;
    }
    CHECK(touched == id);
    CHECK((ev.bits[0] & kGuardContact) != 0);
    CHECK(g.state(id) == GuardState::Return);
    CHECK(g.cooldown(id) == doctest::Approx(4.f));
    CHECK(dist(g.position(id), player) < 0.8f);

    // The player stays put: no second contact and no notice while it cools down.
    int again = 0;
    for (float t = 0.f; t < 3.9f; t += kDt) {
        if (g.step(player, kAllClear, kDt, &ev) >= 0) ++again;
        CHECK((ev.bits[0] & kGuardNoticed) == 0);
    }
    CHECK(again == 0);
    CHECK(g.state(id) == GuardState::Idle); // walked home
    // Cooled down and still in front of it: it comes again.
    CHECK(runUntil(g, id, GuardState::Notice, player, kAllClear, 1.f) > 0.f);
}

TEST_CASE("guard: defeat, reset, hold-off and save") {
    GuardSet g;
    const int a = g.add(sentry({20.f, 20.f}, {1.f, 0.f}));
    const int b = g.add(square());
    REQUIRE(b == 1);

    g.defeat(a);
    CHECK_FALSE(g.active(a));
    CHECK_FALSE(g.inSenseRange(a, {21.f, 20.f}));
    CHECK(g.step({20.1f, 20.f}, kAllClear, kDt) == -1); // standing on a defeated guard
    const GuardSave s = g.save();
    CHECK(s.defeated == 1u);

    GuardSet fresh;
    fresh.add(sentry({20.f, 20.f}, {1.f, 0.f}));
    fresh.add(square());
    fresh.load(s);
    CHECK_FALSE(fresh.active(0));
    CHECK(fresh.active(1));

    // Hold-off: nobody notices or touches for the time given, even standing on a guard.
    fresh.holdOff(3.f);
    for (float t = 0.f; t < 2.9f; t += kDt) CHECK(fresh.step(fresh.position(1), kAllClear, kDt) == -1);

    // Reset puts the guard back at its first point and keeps the cooldown it has.
    for (int i = 0; i < 200; ++i) g.step({100.f, 100.f}, kAllClear, kDt);
    CHECK(dist(g.position(b), {0.f, 0.f}) > 1.f);
    g.holdOff(2.f);
    g.reset(b);
    CHECK(g.state(b) == GuardState::Idle);
    CHECK(g.position(b).x == doctest::Approx(0.f));
    CHECK(g.position(b).y == doctest::Approx(0.f));
    CHECK(g.cooldown(b) == doctest::Approx(2.f));
}

TEST_CASE("guard: capacity and bad specs are refused") {
    GuardSet g;
    GuardSpec empty;
    empty.routeCount = 0;
    CHECK(g.add(empty) == -1);
    GuardSpec zeroFacing = sentry({1.f, 1.f}, {0.f, 0.f});
    CHECK(g.add(zeroFacing) == 0);
    CHECK(g.facing(0).x == doctest::Approx(1.f));
    GuardSpec tooLong = square();
    tooLong.routeCount = 200;
    CHECK(g.add(tooLong) == 1);
    CHECK(g.spec(1).routeCount == GuardSpec::kMaxWaypoints);
    while (g.add(square()) >= 0) {
    }
    CHECK(g.count() == GuardSet::kMaxGuards);
    CHECK_FALSE(g.active(-1));
    CHECK_FALSE(g.active(static_cast<int>(GuardSet::kMaxGuards)));
}

TEST_CASE("guard: stepping never allocates") {
    GuardSet g = demo::makeGuards();
    Vec2 player{0.f, 0.f};
    const std::uint64_t before = ftcore_test::allocStats().count;
    GuardEvents ev;
    for (int i = 0; i < 3000; ++i) {
        player.x = 20.f + 10.f * std::sin(static_cast<float>(i) * 0.01f);
        player.y = 10.f * std::cos(static_cast<float>(i) * 0.013f);
        std::uint32_t sight = 0;
        for (int id = 0; id < static_cast<int>(g.count()); ++id) {
            if (g.inSenseRange(id, player)) sight |= 1u << id;
        }
        if (g.step(player, sight, kDt, &ev) >= 0) g.holdOff(1.f);
    }
    if (ftcore_test::allocHookInstalled()) CHECK(ftcore_test::allocStats().count == before);
}

TEST_CASE("demo guards: routes stay clear of every prop") {
    for (const GuardSpec& s : demo::kGuardSpecs) {
        REQUIRE(s.routeCount >= 2);
        for (std::size_t i = 0; i < s.routeCount; ++i) {
            const Vec2 a = s.route[i];
            const Vec2 b = s.route[(i + 1) % s.routeCount];
            for (const demo::FieldObstacle& o : demo::kFieldObstacles) {
                CHECK(demo::segmentDistance(a, b, o.pos) > o.radius + demo::kGuardPropMargin);
            }
        }
    }
}

// The pilot walks the demo at the pawn's walk speed and is allowed to be caught: each contact is a
// fight the pilot wins (the guard is defeated) and the field holds the guards off for a moment, as
// on the map.
TEST_CASE("demo guards: the pilot still finishes the demo, winning each guard fight") {
    Exploration e = demo::makeExploration();
    GuardSet guards = demo::makeGuards();
    Vec2 pos = demo::kExploreStart;
    Vec2 facing{1.f, 0.f};
    int fights = 0;
    int caughtBy[4] = {-1, -1, -1, -1};
    float seconds = 0.f;
    const std::uint64_t before = ftcore_test::allocStats().count;
    while (!e.allDone() && seconds < 300.f) {
        const int hit = demo::pilotStep(e, pos, facing, 3.3f, kDt);
        if (hit >= 0 && e.item(hit).kind == InteractKind::Encounter) e.clearEncounter(hit);
        const int touched = guards.step(pos, kAllClear, kDt);
        if (touched >= 0) {
            if (fights < 4) caughtBy[fights] = touched;
            ++fights;
            guards.defeat(touched);
            guards.holdOff(demo::kGuardHoldOffSeconds);
        }
        seconds += kDt;
    }
    if (ftcore_test::allocHookInstalled()) CHECK(ftcore_test::allocStats().count == before);
    CHECK(e.allDone());
    CHECK(seconds < 90.f);
    // The east guard has its back turned as the pilot takes its shard; the south one sees it at
    // its shard and catches it on the way to the gate.
    REQUIRE(fights == 1);
    CHECK(caughtBy[0] == 1);
    CHECK(guards.save().defeated == 2u);
    CHECK(guards.active(0));
}
