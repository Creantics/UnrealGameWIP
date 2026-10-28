#include "ftcore/PartyTrail.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

using namespace ftcore;

namespace {

float dist(Vec2 a, Vec2 b) { return std::hypot(b.x - a.x, b.y - a.y); }

// Walks the lead from `from` to `to` in steps of `step` metres, pushing each position.
void walk(PartyTrail& t, Vec2 from, Vec2 to, float step) {
    const float d = dist(from, to);
    const int n = static_cast<int>(std::ceil(d / step));
    for (int i = 1; i <= n; ++i) {
        const float f = static_cast<float>(i) / static_cast<float>(n);
        t.push({from.x + (to.x - from.x) * f, from.y + (to.y - from.y) * f});
    }
}

} // namespace

TEST_CASE("party trail: reset seeds a straight line behind the lead's facing") {
    PartyTrail t;
    t.reset({2.f, 3.f}, {0.f, 2.f}, 3.2f);
    CHECK(t.length() == doctest::Approx(3.25f)); // rounded up to whole samples
    const Vec2 p = t.pointBehind(1.6f);
    CHECK(p.x == doctest::Approx(2.f));
    CHECK(p.y == doctest::Approx(1.4f));
    CHECK(t.pointBehind(0.f).y == doctest::Approx(3.f));

    t.reset({0.f, 0.f}, {}, 1.f); // no facing: behind along -x
    CHECK(t.pointBehind(1.f).x == doctest::Approx(-1.f));
}

TEST_CASE("party trail: a straight walk keeps the followers at exact spacing") {
    PartyTrail t;
    t.reset({0.f, 0.f}, {1.f, 0.f}, 2.f * PartyTrail::kFollowerSpacing);
    walk(t, {0.f, 0.f}, {10.f, 0.f}, 0.07f);
    for (int i = 1; i <= 2; ++i) {
        const Vec2 p = t.pointBehind(static_cast<float>(i) * PartyTrail::kFollowerSpacing);
        CHECK(p.x == doctest::Approx(10.f - static_cast<float>(i) * PartyTrail::kFollowerSpacing).epsilon(0.001));
        CHECK(p.y == doctest::Approx(0.f));
    }
}

TEST_CASE("party trail: after a turn the follower stays on the path, not on the chord") {
    for (const float step : {0.05f, 0.4f, 1.3f}) { // small frames and long, fast-forwarded ones
        PartyTrail t;
        t.reset({0.f, 0.f}, {1.f, 0.f}, 0.f);
        walk(t, {0.f, 0.f}, {5.f, 0.f}, step);
        walk(t, {5.f, 0.f}, {5.f, 3.f}, step);
        const Vec2 p = t.pointBehind(4.f); // 3 m back to the corner, 1 m more along the first leg
        CHECK(dist(p, {4.f, 0.f}) < PartyTrail::kSampleSpacing);
        CHECK(dist(t.pointBehind(1.f), {5.f, 2.f}) < 0.05f);
    }
}

TEST_CASE("party trail: a short trail answers with its oldest point") {
    PartyTrail t;
    t.reset({0.f, 0.f}, {1.f, 0.f}, 0.f);
    t.push({0.6f, 0.f});
    CHECK(t.length() == doctest::Approx(0.6f));
    const Vec2 p = t.pointBehind(3.f);
    CHECK(p.x == doctest::Approx(0.f));
    CHECK(p.y == doctest::Approx(0.f));
    t.push({0.6f, 0.f}); // standing still changes nothing
    CHECK(t.pointBehind(0.5f).x == doctest::Approx(0.1f));
}

TEST_CASE("party trail: the ring keeps the newest samples when it overflows") {
    PartyTrail t;
    t.reset({0.f, 0.f}, {1.f, 0.f}, 0.f);
    walk(t, {0.f, 0.f}, {40.f, 0.f}, 0.1f); // 160 samples into 64
    CHECK(t.samples() == PartyTrail::kCapacity);
    const float kept = static_cast<float>(PartyTrail::kCapacity - 1u) * PartyTrail::kSampleSpacing;
    CHECK(t.length() <= kept + PartyTrail::kSampleSpacing);
    CHECK(t.length() >= kept);
    CHECK(t.pointBehind(3.2f).x == doctest::Approx(36.8f).epsilon(0.001));
    CHECK(t.pointBehind(1000.f).x == doctest::Approx(40.f - t.length()).epsilon(0.001));
}

TEST_CASE("party trail: a jump longer than the trail starts it over, facing the jump") {
    PartyTrail t;
    t.reset({0.f, 0.f}, {1.f, 0.f}, 2.f);
    t.push({0.f, 50.f});
    CHECK(t.lead().y == doctest::Approx(50.f));
    CHECK(t.length() == doctest::Approx(0.f));
    t.push({0.f, 51.f});
    CHECK(t.pointBehind(1.f).y == doctest::Approx(50.f));

    PartyTrail fresh; // never reset: the first push starts it
    fresh.push({3.f, 4.f});
    CHECK(fresh.pointBehind(2.f).x == doctest::Approx(3.f));
}

TEST_CASE("party trail: the odometer counts the lead's way along the trail") {
    PartyTrail t;
    CHECK(t.odometer() == doctest::Approx(0.f));
    t.reset({0.f, 0.f}, {1.f, 0.f}, 1.f);
    const float start = t.odometer();
    walk(t, {0.f, 0.f}, {3.f, 0.f}, 0.07f);
    walk(t, {3.f, 0.f}, {3.f, 2.f}, 0.4f);
    CHECK(t.odometer() - start == doctest::Approx(5.f).epsilon(0.02)); // the corner is cut by at most a sample
    // A point behind the lead is where the lead was when the odometer read that much less.
    CHECK(dist(t.pointBehind(1.f), {3.f, 1.f}) < 0.05f);
}

TEST_CASE("party trail: each follower reaches a turn where the lead began it") {
    PartyTrail t;
    t.reset({0.f, 0.f}, {1.f, 0.f}, 2.f * PartyTrail::kFollowerSpacing);
    std::uint32_t first = 0, second = 0;
    const float a1 = PartyTrail::kFollowerSpacing, a2 = 2.f * PartyTrail::kFollowerSpacing;
    CHECK(t.turnsReached(a1, first) == 0.f);
    walk(t, {0.f, 0.f}, {4.f, 0.f}, 0.05f);
    t.markTurn(90.f); // at x = 4
    Vec2 lead{4.f, 0.f};
    float got1 = 0.f, got2 = 0.f, at1 = -1.f, at2 = -1.f;
    for (int i = 0; i < 120; ++i) {
        const Vec2 next{lead.x, lead.y + 0.05f};
        t.push(next);
        lead = next;
        const float d1 = t.turnsReached(a1, first), d2 = t.turnsReached(a2, second);
        if (d1 != 0.f) { got1 = d1; at1 = lead.y; }
        if (d2 != 0.f) { got2 = d2; at2 = lead.y; }
    }
    CHECK(got1 == 90.f);
    CHECK(got2 == 90.f);
    // Each reaches it once, when the lead is its spacing past the turn (to within a frame's step).
    CHECK(at1 == doctest::Approx(a1).epsilon(0.05));
    CHECK(at2 == doctest::Approx(a2).epsilon(0.05));
    CHECK(t.turnsReached(a1, first) == 0.f);
}

TEST_CASE("party trail: turns reached together sum, a reset forgets them, the ring drops the oldest") {
    PartyTrail t;
    t.reset({0.f, 0.f}, {1.f, 0.f}, 4.f);
    std::uint32_t next = 0;
    t.markTurn(45.f);
    t.push({0.5f, 0.f});
    t.markTurn(-30.f);
    walk(t, {0.5f, 0.f}, {3.f, 0.f}, 0.1f);
    CHECK(t.turnsReached(1.f, next) == doctest::Approx(15.f));

    std::uint32_t late = 0;
    t.markTurn(90.f);
    t.reset({0.f, 0.f}, {1.f, 0.f}, 4.f);
    walk(t, {0.f, 0.f}, {3.f, 0.f}, 0.1f);
    CHECK(t.turnsReached(1.f, late) == 0.f);

    std::uint32_t slow = 0;
    for (int i = 0; i < static_cast<int>(PartyTrail::kTurns) + 3; ++i) {
        t.markTurn(1.f);
        t.push({3.f + 0.3f * static_cast<float>(i + 1), 0.f});
    }
    walk(t, {3.f + 0.3f * static_cast<float>(PartyTrail::kTurns + 3), 0.f}, {20.f, 0.f}, 0.2f);
    CHECK(t.turnsReached(1.f, slow) == doctest::Approx(static_cast<float>(PartyTrail::kTurns)));
}

TEST_CASE("party trail: jumps are reached where the lead took off, each once") {
    PartyTrail t;
    t.reset({0.f, 0.f}, {1.f, 0.f}, 4.f);
    std::uint32_t next = 0;
    walk(t, {0.f, 0.f}, {2.f, 0.f}, 0.05f);
    t.markJump(); // at x = 2
    int got = 0;
    float at = -1.f;
    for (int i = 1; i <= 60; ++i) {
        t.push({2.f + 0.05f * static_cast<float>(i), 0.f});
        if (const int n = t.jumpsReached(1.5f, next); n != 0) {
            got += n;
            at = 2.f + 0.05f * static_cast<float>(i);
        }
    }
    CHECK(got == 1);
    CHECK(at == doctest::Approx(3.5f).epsilon(0.02));
    t.markJump();
    t.reset({0.f, 0.f}, {1.f, 0.f}, 4.f); // a reset forgets it
    walk(t, {0.f, 0.f}, {5.f, 0.f}, 0.1f);
    CHECK(t.jumpsReached(1.f, next) == 0);
}

TEST_CASE("party trail: each point keeps the lead's speed as it passed") {
    PartyTrail t;
    t.reset({0.f, 0.f}, {1.f, 0.f}, 2.f);
    CHECK(t.speedBehind(1.f) < 0.f); // the seeded trail: unknown
    for (int i = 1; i <= 40; ++i) t.push({0.1f * static_cast<float>(i), 0.f}, i <= 20 ? 2.f : 5.f);
    CHECK(t.speedBehind(0.f) == doctest::Approx(5.f));
    CHECK(t.speedBehind(1.f) == doctest::Approx(5.f));
    CHECK(t.speedBehind(3.f) == doctest::Approx(2.f));
    CHECK(t.speedBehind(4.5f) < 0.f); // back on the seed
}

namespace {

// A lead on a scripted run for PartySlot: its position along a path and speed each frame.
struct Lead {
    PartyTrail trail;
    PartySlot slot;
    Vec2 pos{0.f, 0.f};
    float speed = 0.f;
    float t = 0.f;
    static constexpr float kDt = 1.f / 60.f;

    Lead() {
        trail.reset(pos, {1.f, 0.f}, 6.f);
        slot.reset(trail);
    }
    // One frame heading `dir` (degrees) toward speed `want` at 10 m/s^2.
    void step(float dirDeg, float want) {
        const float dv = 10.f * kDt;
        speed = want > speed ? std::min(want, speed + dv) : std::max(want, speed - dv);
        const float r = dirDeg * 3.14159265f / 180.f;
        pos = {pos.x + std::cos(r) * speed * kDt, pos.y + std::sin(r) * speed * kDt};
        trail.push(pos, speed);
        // Braking to a stand, it says how far it has left (as the game's lead does).
        slot.step(trail, speed, kDt, want <= 0.f && speed > 0.f ? speed * speed / 20.f : -1.f);
        t += kDt;
    }
};

} // namespace

TEST_CASE("party slot: on a steady run it holds its place behind and to the side") {
    Lead l;
    for (int i = 0; i < 600; ++i) l.step(0.f, 4.5f);
    const PartySlotTuning k = l.slot.tuning();
    // In its place behind where the lead was a beat ago, give or take the slack.
    const float place = k.spacing + 4.5f * k.delay;
    CHECK(l.slot.arc() >= place - 0.05f);
    CHECK(l.slot.arc() <= place + k.slack + 0.05f);
    CHECK(l.slot.position().y == doctest::Approx(k.side).epsilon(0.02));
    CHECK(l.slot.speed() == doctest::Approx(4.5f).epsilon(0.01));
}

TEST_CASE("party slot: it sets off after the lead and stops after it, never closer than its gaps") {
    Lead l;
    for (int i = 0; i < 60; ++i) l.step(0.f, 0.f); // both standing
    float leadMoved = -1.f, slotMoved = -1.f;
    float minGap = 1e9f, minArc = 1e9f;
    for (int i = 0; i < 180; ++i) {
        l.step(0.f, 4.5f);
        if (leadMoved < 0.f && l.speed > 0.5f) leadMoved = l.t;
        if (slotMoved < 0.f && l.slot.speed() > 0.5f) slotMoved = l.t;
    }
    CHECK(slotMoved - leadMoved >= l.slot.tuning().delay); // a beat late, at least the delay
    CHECK(l.slot.wantSpeed() == doctest::Approx(4.5f).epsilon(0.05));
    CHECK(l.slot.stopDistance() < 0.f);
    float leadStood = -1.f, slotStood = -1.f;
    float stopSaid = -1.f, stopFrom = 0.f, slotFrom = 0.f;
    for (int i = 0; i < 300; ++i) {
        l.step(0.f, 0.f);
        if (stopSaid < 0.f && l.slot.stopDistance() > 0.f) {
            stopSaid = l.slot.stopDistance();
            slotFrom = l.trail.odometer() - l.slot.arc();
        }
        stopFrom = l.trail.odometer() - l.slot.arc();
        if (leadStood < 0.f && l.speed < 0.05f) leadStood = l.t;
        if (slotStood < 0.f && l.slot.speed() < 0.05f) slotStood = l.t;
        minGap = std::min(minGap, dist(l.slot.position(), l.pos));
        minArc = std::min(minArc, l.slot.arc());
    }
    CHECK(slotStood - leadStood >= l.slot.tuning().delay); // arrives late
    REQUIRE(stopSaid > 0.f); // it said it was stopping, and about how far it would go
    CHECK(stopFrom - slotFrom == doctest::Approx(stopSaid).epsilon(0.35));
    CHECK(l.slot.wantSpeed() == 0.f);
    CHECK(minGap >= l.slot.tuning().minGap);
    CHECK(minArc >= l.slot.tuning().minArc - 1e-3f);
    // It comes to rest a little inside its place (a late stop overruns), never past its gaps.
    CHECK(l.slot.arc() >= l.slot.tuning().minArc + 0.2f);
    CHECK(l.slot.arc() <= l.slot.tuning().spacing + l.slot.tuning().slack + 0.05f);
}

TEST_CASE("party slot: through a corner it goes no faster than the lead went there") {
    Lead l;
    for (int i = 0; i < 240; ++i) l.step(0.f, 4.5f);
    // The lead slows to 2 m/s through a 90 degree corner over a second, then runs on.
    float heading = 0.f;
    for (int i = 0; i < 60; ++i) {
        heading += 1.5f;
        l.step(heading, 2.f);
    }
    for (int i = 0; i < 240; ++i) l.step(heading, 4.5f);
    // Replay: where the lead went slowly the follower did too (checked through the run above).
    Lead r;
    float worst = 0.f;
    for (int i = 0; i < 240; ++i) r.step(0.f, 4.5f);
    heading = 0.f;
    for (int i = 0; i < 300; ++i) {
        if (i < 60) heading += 1.5f;
        r.step(heading, i < 60 ? 2.f : 4.5f);
        const float there = r.trail.speedBehind(r.slot.arc());
        if (there >= 0.f) worst = std::max(worst, r.slot.speed() - (there * (1.f + r.slot.tuning().catchUp) + r.slot.tuning().catchUpMin));
    }
    CHECK(worst <= 0.05f);
}

TEST_CASE("party slot: where the path doubles back past the lead, it holds its place") {
    Lead l;
    for (int i = 0; i < 120; ++i) l.step(0.f, 3.f);
    // A tight hairpin to the left, then the lead stands just beside the way it came.
    float heading = 0.f;
    for (int i = 0; i < 40; ++i) {
        heading += 4.5f;
        l.step(heading, 2.f);
    }
    for (int i = 0; i < 30; ++i) l.step(180.f, 2.f);
    float closest = 1e9f;
    Vec2 last = l.slot.position();
    for (int i = 0; i < 240; ++i) {
        l.step(180.f, 0.f);
        const float d = dist(l.slot.position(), l.pos);
        // Within minGap it may only hold or move away.
        if (d < l.slot.tuning().minGap - 1e-3f) CHECK(d >= dist(last, l.pos) - 1e-3f);
        closest = std::min(closest, d);
        last = l.slot.position();
    }
    CHECK(closest >= l.slot.tuning().minGap - 1e-3f);
}

TEST_CASE("party slot: a lead running back past it pushes it aside without stopping it") {
    Lead l;
    for (int i = 0; i < 240; ++i) l.step(0.f, 4.5f);
    // A 135-degree turn in a third of a second, then on: the lead sweeps back along the follower's side.
    float heading = 0.f, closest = 1e9f, slowest = 1e9f;
    for (int i = 0; i < 20; ++i) {
        heading += 135.f / 20.f;
        l.step(heading, 4.5f);
        closest = std::min(closest, dist(l.slot.position(), l.pos));
        slowest = std::min(slowest, l.slot.speed());
    }
    for (int i = 0; i < 60; ++i) {
        l.step(heading, 4.5f);
        closest = std::min(closest, dist(l.slot.position(), l.pos));
        slowest = std::min(slowest, l.slot.speed());
    }
    CHECK(closest >= l.slot.tuning().minGap - 1e-3f);
    CHECK(slowest > 1.f); // it never stands to wait
    // And it goes on along the path after the lead, at its pace.
    for (int i = 0; i < 180; ++i) l.step(heading, 4.5f);
    CHECK(l.slot.speed() == doctest::Approx(4.5f).epsilon(0.05));
    CHECK(l.slot.arc() <= l.slot.tuning().spacing + 4.5f * l.slot.tuning().delay + l.slot.tuning().slack + 0.1f);
}

TEST_CASE("party trail allocates nothing") {
    PartyTrail t;
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    t.reset({0.f, 0.f}, {1.f, 0.f}, 3.2f);
    PartySlot slot;
    slot.reset(t);
    float sum = 0.f;
    std::uint32_t next = 0, jumps = 0;
    for (int i = 0; i < 2000; ++i) {
        const float a = static_cast<float>(i) * 0.01f;
        t.push({a * 3.f, std::sin(a) * 4.f}, 3.f);
        if (i % 100 == 0) t.markTurn(10.f);
        if (i % 300 == 0) t.markJump();
        slot.step(t, 3.f, 1.f / 60.f);
        sum += t.pointBehind(1.6f).x + t.pointBehind(3.2f).y + t.length() + t.turnsReached(1.6f, next) + slot.position().x
            + static_cast<float>(t.jumpsReached(1.6f, jumps)) + t.speedBehind(2.f);
    }
    const auto after = ftcore_test::allocStats().count;
    CHECK(sum != 0.f);
    if (counting) CHECK(after == before);
}
