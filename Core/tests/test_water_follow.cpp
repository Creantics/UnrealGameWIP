// Party followers in the water (WaterFollow.h): when they go in and how, the stagger, the surface slots, the dive delay,
// the under-water band and the camera keep-out, surfacing with the lead, the rescue placement, the exit choice and
// order, the aborts, and the slot clamp to the water.
#include "ftcore/WaterFollow.h"
#include "ftcore/WaterSurfaceFeet.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace ftcore;

namespace {

constexpr float kDt = 1.f / 30.f;

// The open sea over x >= 0, 10 m deep; land at x < 0.
WaterBodies sea() {
    WaterBodies w;
    w.add(waterSea(0.f, 0.f, -10.f, 0.f));
    return w;
}

WaterFollowLead swimmingLead(SwimVec p = {5.f, 0.f, -1.f}, Vec2 v = {}) {
    WaterFollowLead l;
    l.swimming = true;
    l.position = p;
    l.velocity = v;
    l.camera = {p.x - 4.f, p.y, p.z + 1.5f};
    return l;
}

WaterFollowMember ashore(float y) {
    WaterFollowMember m;
    m.active = true;
    m.position = {-2.f, y, 0.5f};
    m.edge = {0.f, y};
    m.edgeOut = {1.f, 0.f};
    m.surfaceZ = 0.f;
    m.bedZ = -10.f;
    return m;
}

struct Scene {
    WaterFollowParty party;
    WaterFollowLead lead = swimmingLead();
    WaterFollowMember members[2] = {ashore(1.f), ashore(-1.f)};
    std::vector<WaterExit> exits;
    WaterBodies water = sea();
    float t = 0.f;
    bool useWater = true;
    void step(int n = 1) {
        for (int i = 0; i < n; ++i) {
            party.step(lead, members, 2, exits.data(), exits.size(), kDt, useWater ? &water : nullptr);
            t += kDt;
        }
    }
    const WaterFollowOrder& o(int i) const { return party.order(i); }
    // Steps until member i's order is in `p` (at most `seconds`); the time it took, or -1.
    float until(int i, WaterFollowPhase p, float seconds = 10.f) {
        const float t0 = t;
        while (t - t0 < seconds) {
            step();
            if (o(i).phase == p) return t - t0;
        }
        return -1.f;
    }
    // Both in the water at the surface (they walked to their edge and jumped).
    void bothIn() {
        step(30);
        members[0].atEntry = members[1].atEntry = true;
        REQUIRE(until(1, WaterFollowPhase::Entering) >= 0.f);
        members[0].entered = members[1].entered = true;
        members[0].position = {1.f, 1.f, -1.f};
        members[1].position = {1.f, -1.f, -1.f};
        step();
        REQUIRE(o(0).phase == WaterFollowPhase::Surface);
        REQUIRE(o(1).phase == WaterFollowPhase::Surface);
    }
};

WaterExit steps(Vec2 top, Vec2 foot) {
    WaterExit e;
    e.kind = WaterExitKind::Steps;
    e.top = top;
    e.foot = foot;
    e.out = {-1.f, 0.f};
    return e;
}

WaterExit ladder(Vec2 foot) {
    WaterExit e;
    e.kind = WaterExitKind::Ladder;
    e.foot = foot;
    e.top = {foot.x - 1.f, foot.y};
    e.out = {-1.f, 0.f};
    return e;
}

float dist(SwimVec a, SwimVec b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

} // namespace

TEST_CASE("water follow: nobody goes in until the lead has swum 0.8 s") {
    Scene s;
    s.step(23); // 0.77 s
    CHECK(s.o(0).phase == WaterFollowPhase::Ashore);
    CHECK(s.o(1).phase == WaterFollowPhase::Ashore);
    s.step(1);
    CHECK(s.o(0).phase == WaterFollowPhase::ToEntry);
    CHECK(s.o(0).changed);
    CHECK(s.o(1).phase == WaterFollowPhase::ToEntry);
    // A lead only wading (not swimming) never draws them in.
    Scene w;
    w.lead.swimming = false;
    w.step(90);
    CHECK(w.o(0).phase == WaterFollowPhase::Ashore);
}

TEST_CASE("water follow: steps within 8 m of the path are walked in by; else a jump from the edge where it stands") {
    const WaterExit near[] = {ladder({0.5f, 0.f}), steps({-1.f, 6.f}, {1.f, 6.f})};
    const WaterFollowEntry a = waterFollowEntry(near, 2, {-2.f, 1.f}, {5.f, 0.f}, {0.f, 1.f}, {1.f, 0.f});
    CHECK(a.kind == WaterFollowEntryKind::Steps); // the ladder is no way in
    CHECK(a.exitIndex == 1);
    CHECK(a.point.y == doctest::Approx(6.f));
    CHECK(a.into.x == doctest::Approx(1.f));
    // The nearer of two flights wins.
    const WaterExit two[] = {steps({-1.f, 6.f}, {1.f, 6.f}), steps({-1.f, -3.f}, {1.f, -3.f})};
    CHECK(waterFollowEntry(two, 2, {-2.f, 1.f}, {5.f, 0.f}, {0.f, 1.f}, {1.f, 0.f}).exitIndex == 1);
    // Further than 8 m off the path: the jump from its own edge, a metre out.
    const WaterExit far[] = {steps({-1.f, 12.f}, {1.f, 12.f})};
    const WaterFollowEntry j = waterFollowEntry(far, 1, {-2.f, 1.f}, {5.f, 0.f}, {0.f, 1.f}, {1.f, 0.f});
    CHECK(j.kind == WaterFollowEntryKind::Jump);
    CHECK(j.point.x == doctest::Approx(0.f));
    CHECK(j.point.y == doctest::Approx(1.f));
    CHECK(j.into.x == doctest::Approx(1.f));
    CHECK(waterFollowEntry(nullptr, 0, {-2.f, 1.f}, {5.f, 0.f}, {0.f, 1.f}, {1.f, 0.f}).kind == WaterFollowEntryKind::Jump);
    // The party takes the same choice and walks to its point.
    Scene s;
    s.exits = {steps({-1.f, 3.f}, {1.f, 3.f})};
    s.step(24);
    CHECK(s.o(0).entry.kind == WaterFollowEntryKind::Steps);
    CHECK(s.o(0).target.y == doctest::Approx(3.f));
    CHECK(s.o(0).speed < 0.f); // the party's own walk speed
}

TEST_CASE("water follow: the entries are staggered 0.4-0.8 s, in order, so two never land together") {
    Scene s;
    s.step(24);
    s.members[0].atEntry = true;
    s.members[1].atEntry = true;
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::Entering);
    CHECK(s.o(1).phase == WaterFollowPhase::ToEntry);
    const float gap = s.until(1, WaterFollowPhase::Entering);
    CHECK(gap >= s.party.stagger(1) - 1e-3f);
    CHECK(gap <= s.party.stagger(1) + kDt + 1e-3f);
    for (int i = 0; i < kWaterFollowMax; ++i) {
        CHECK(s.party.stagger(i) >= 0.4f);
        CHECK(s.party.stagger(i) <= 0.8f);
    }
    // The second may not go before the first, even if it reaches its edge first.
    Scene r;
    r.step(24);
    r.members[1].atEntry = true;
    r.step(30);
    CHECK(r.o(1).phase == WaterFollowPhase::ToEntry);
    r.members[0].atEntry = true;
    r.step();
    CHECK(r.o(0).phase == WaterFollowPhase::Entering);
    CHECK(r.o(0).startEntry);
}

TEST_CASE("water follow: no entry while held or traversing, and none once the lead is back out") {
    Scene s;
    s.members[0].held = true;
    s.members[1].traversing = true;
    s.step(40);
    CHECK(s.o(0).phase == WaterFollowPhase::Ashore);
    s.members[1].atEntry = true;
    s.step(30);
    CHECK(s.o(1).phase == WaterFollowPhase::ToEntry); // chose its entry, but does not start it mid-traversal
    s.members[1].traversing = false;
    s.step();
    CHECK(s.o(1).phase == WaterFollowPhase::Entering);
    // The lead climbs out before they went in: they stay ashore.
    Scene a;
    a.step(24);
    REQUIRE(a.o(0).phase == WaterFollowPhase::ToEntry);
    a.lead.swimming = false;
    a.lead.exiting = true;
    a.step();
    CHECK(a.o(0).phase == WaterFollowPhase::Ashore);
    CHECK(a.o(1).phase == WaterFollowPhase::Ashore);
    a.step(60);
    CHECK(a.o(0).phase == WaterFollowPhase::Ashore);
    // Held after choosing: back ashore.
    Scene h;
    h.step(24);
    h.members[0].held = true;
    h.step();
    CHECK(h.o(0).phase == WaterFollowPhase::Ashore);
}

TEST_CASE("water follow: surface slots 2.5 m behind and 1.5 m to alternating sides, turned with the lead's travel") {
    Scene s;
    s.bothIn();
    s.lead = swimmingLead({10.f, 0.f, -1.f}, {1.5f, 0.f});
    s.step();
    // Travelling +X: behind is -X; right of +X is +Y (the engine's frame).
    CHECK(s.o(0).target.x == doctest::Approx(7.5f));
    CHECK(s.o(0).target.y == doctest::Approx(1.5f));
    CHECK(s.o(1).target.x == doctest::Approx(7.5f));
    CHECK(s.o(1).target.y == doctest::Approx(-1.5f));
    CHECK(s.o(0).target.z == doctest::Approx(0.f)); // at the surface
    // Off the slot: the lead's cap plus 20%.
    CHECK(s.o(0).speed == doctest::Approx(1.5f * 1.2f));
    CHECK(s.o(0).sprint);
    // On the slot: the lead's speed.
    s.members[0].position = {7.5f, 1.5f, -1.f};
    s.step();
    CHECK(s.o(0).speed == doctest::Approx(1.5f));
    // The lead turns to +Y: the slots turn with it (right of +Y is -X).
    s.lead.velocity = {0.f, 1.5f};
    s.step();
    CHECK(s.o(0).target.x == doctest::Approx(10.f - 1.5f));
    CHECK(s.o(0).target.y == doctest::Approx(-2.5f));
    // A sprinting lead raises the cap.
    s.lead.sprint = true;
    s.step();
    CHECK(s.o(1).speed == doctest::Approx(2.4f * 1.2f));
}

TEST_CASE("water follow: when the lead treads, the followers settle at their slots and tread") {
    Scene s;
    s.bothIn();
    s.lead = swimmingLead({10.f, 0.f, -1.f}, {0.2f, 0.f});
    s.lead.yaw = 0.f;
    s.step();
    const SwimVec slot = s.o(0).target;
    s.members[0].position = {slot.x + 0.5f, slot.y, -1.f};
    s.step();
    CHECK(s.o(0).speed == doctest::Approx(0.4f)); // settling the last half metre slowly
    s.members[0].position = {slot.x, slot.y, -1.f};
    s.step();
    CHECK(s.o(0).speed == 0.f);
    CHECK(s.o(0).phase == WaterFollowPhase::Surface);
}

TEST_CASE("water follow: slots are kept in the water, pulled in toward the lead") {
    Scene s;
    s.bothIn();
    // The lead 1.5 m off the quay swimming away from it: the slot behind would be on land.
    s.lead = swimmingLead({1.5f, 0.f, -1.f}, {1.5f, 0.f});
    s.step();
    CHECK(s.o(0).target.x >= 0.f);
    CHECK(s.water.depth(Vec2{s.o(0).target.x, s.o(0).target.y}) > kWaterSwimDepth);
}

TEST_CASE("water follow: they dive 0.3 s x (order + 1) after the lead, and surface with it") {
    Scene s;
    s.bothIn();
    s.lead.swim = SwimState::Diving;
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::DiveWait);
    CHECK(s.o(1).phase == WaterFollowPhase::DiveWait);
    float t0 = -1.f, t1 = -1.f;
    for (int i = 0; i < 60; ++i) {
        s.step();
        if (s.o(0).dived) t0 = float(i + 1) * kDt;
        if (s.o(1).dived) t1 = float(i + 1) * kDt;
    }
    CHECK(t0 == doctest::Approx(0.3f).epsilon(0.12));
    CHECK(t1 == doctest::Approx(0.6f).epsilon(0.06));
    CHECK(s.o(0).phase == WaterFollowPhase::Under);
    CHECK(s.o(0).swim == SwimState::Under);
    // The lead surfaces: both surface on the same step, with the event for their WaterLoco.
    s.lead.swim = SwimState::Surface;
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::Surface);
    CHECK(s.o(0).surfaced);
    CHECK(s.o(1).surfaced);
    CHECK(s.o(1).swim == SwimState::Surface);
    // A lead that is back up before a follower's delay ran: that follower never dives.
    Scene q;
    q.bothIn();
    q.lead.swim = SwimState::Diving;
    q.step(12); // 0.4 s: the first has dived, the second waits
    CHECK(q.o(0).phase == WaterFollowPhase::Under);
    CHECK(q.o(1).phase == WaterFollowPhase::DiveWait);
    q.lead.swim = SwimState::Surface;
    q.step();
    CHECK(q.o(1).phase == WaterFollowPhase::Surface);
    CHECK_FALSE(q.o(1).surfaced);
}

TEST_CASE("water follow: under water they keep 4-6 m from the lead, out of the shot, under the surface, over the bed") {
    Scene s;
    s.bothIn();
    s.lead = swimmingLead({14.f, 0.f, -3.f}, {2.2f, 0.f}); // under the 10 m teleport
    s.lead.swim = SwimState::Under;
    s.step(30);
    REQUIRE(s.o(1).phase == WaterFollowPhase::Under);
    for (int i = 0; i < 2; ++i) {
        const SwimVec t = s.o(i).target;
        const float r = dist(t, s.lead.position);
        CHECK(r >= 4.f - 1e-3f);
        CHECK(r <= 6.f + 1e-3f);
        CHECK(t.x < s.lead.position.x); // behind
        CHECK(t.z <= -0.6f + 1e-4f);
    }
    CHECK(s.o(0).target.y > 0.f); // to its side
    CHECK(s.o(1).target.y < 0.f);
    // Inside the band: it drifts with the lead; outside: it catches up at Swim's cap plus 20%.
    s.members[0].position = s.o(0).target;
    s.step();
    CHECK(s.o(0).speed == 0.f);
    s.members[0].position = {5.f, 0.f, -3.f};
    s.step();
    CHECK(s.o(0).speed == doctest::Approx(2.2f * 1.2f));
    // Too close is out of the band too.
    s.members[0].position = {13.f, -1.f, -3.f};
    s.step();
    CHECK(s.o(0).speed > 0.f);
    // A camera straight behind the slot: the slot is pushed out of the 0.8 m keep-out around camera-to-lead.
    s.lead.camera = {12.f, -3.f, -3.f};
    s.step();
    const SwimVec t = s.o(0).target;
    const float abx = s.lead.position.x - s.lead.camera.x, aby = s.lead.position.y - s.lead.camera.y;
    const float l2 = abx * abx + aby * aby;
    float u = ((t.x - s.lead.camera.x) * abx + (t.y - s.lead.camera.y) * aby) / l2;
    u = u < 0.f ? 0.f : (u > 1.f ? 1.f : u);
    const SwimVec c{s.lead.camera.x + abx * u, s.lead.camera.y + aby * u, -3.f};
    CHECK(dist(t, c) >= 0.8f);
    // In the shot at a good range: it moves.
    s.members[0].position = c;
    s.step();
    CHECK(s.o(0).speed > 0.f);
    // A shallow bed (no water set given: the follower's own bed): the slot stays 0.9 m over it.
    s.useWater = false;
    s.members[0].bedZ = -2.f;
    s.lead.position.z = -1.8f;
    s.step();
    CHECK(s.o(0).target.z == doctest::Approx(-1.1f));
}

TEST_CASE("water follow: the lead's rescue places them with it; they have no breath or rescue of their own") {
    Scene s;
    s.bothIn();
    s.lead.swim = SwimState::Under;
    s.step(90); // long under water: nothing happens to them
    CHECK(s.o(0).phase == WaterFollowPhase::Under);
    s.lead.swim = SwimState::Rescued;
    s.lead.swimming = false;
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::Placed);
    CHECK(s.o(0).place);
    CHECK(s.o(1).place);
    s.step();
    CHECK_FALSE(s.o(0).place); // once
    s.lead.swim = SwimState::Surface;
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::Ashore);
}

TEST_CASE("water follow: the exit is the lead's within 25 m, else the nearest") {
    const WaterExit ex[] = {ladder({0.f, 0.f}), ladder({0.f, 40.f}), ladder({0.f, 60.f})};
    CHECK(waterFollowExit(ex, 3, 1, {5.f, 30.f}) == 1);
    CHECK(waterFollowExit(ex, 3, 1, {5.f, 70.f}) == 2);  // the lead's is 30 m away: the nearest
    CHECK(waterFollowExit(ex, 3, -1, {5.f, 10.f}) == 0); // the lead left by none
    CHECK(waterFollowExit(ex, 3, 7, {5.f, 10.f}) == 0);  // a bad index
    CHECK(waterFollowExit(nullptr, 0, 0, {5.f, 10.f}) == -1);
}

TEST_CASE("water follow: they climb out after the lead, in order, 0.6 s apart") {
    Scene s;
    s.exits = {ladder({1.f, 0.f})};
    s.bothIn();
    s.lead.swimming = false;
    s.lead.exiting = true;
    s.lead.exitIndex = 0;
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::ToExit);
    CHECK(s.o(0).exitIndex == 0);
    CHECK(s.o(0).speed > 0.f);
    // The queue waits out from the foot by order.
    CHECK(s.o(1).target.x == doctest::Approx(1.f + 1.2f));
    s.members[0].atExit = s.members[1].atExit = true;
    s.step(30);
    CHECK(s.o(0).phase == WaterFollowPhase::ExitWait); // the lead is still climbing
    s.lead.out = true;
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::Climbing);
    CHECK(s.o(0).startClimb);
    CHECK(s.o(1).phase == WaterFollowPhase::ExitWait);
    const float gap = s.until(1, WaterFollowPhase::Climbing);
    CHECK(gap == doctest::Approx(0.6f).epsilon(0.06));
    s.members[0].climbed = true;
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::Ashore);
    // The second, though at its foot first, waits for the first to start.
    Scene r;
    r.exits = {ladder({1.f, 0.f})};
    r.bothIn();
    r.lead.swimming = false;
    r.lead.exiting = true;
    r.lead.out = true;
    r.step();
    r.members[1].atExit = true;
    r.step(30);
    CHECK(r.o(1).phase == WaterFollowPhase::ExitWait);
    CHECK(r.o(0).phase == WaterFollowPhase::ToExit);
    r.members[0].atExit = true;
    r.step(2);
    CHECK(r.o(0).phase == WaterFollowPhase::Climbing);
    CHECK(r.o(1).phase == WaterFollowPhase::ExitWait);
}

TEST_CASE("water follow: left in the water with the lead out by no exit move, they make their own way out") {
    Scene s;
    s.exits = {ladder({1.f, 0.f})};
    s.bothIn();
    s.lead.swimming = false; // a scene took the lead out
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::ToExit);
    CHECK(s.o(0).exitIndex == 0);
}

TEST_CASE("water follow: the lead swims again from the bank: the exit queue goes back to the surface") {
    Scene s;
    s.exits = {ladder({1.f, 0.f})};
    s.bothIn();
    s.lead.swimming = false;
    s.lead.exiting = true;
    s.lead.exitIndex = 0;
    s.step();
    REQUIRE(s.o(0).phase == WaterFollowPhase::ToExit);
    s.members[1].atExit = true;
    s.step();
    REQUIRE(s.o(1).phase == WaterFollowPhase::ExitWait);
    // It turns back: swimming, not exiting, 4 m from the ladder's foot.
    s.lead.swimming = true;
    s.lead.exiting = false;
    s.lead.position = {5.f, 0.f, -1.f};
    s.step(15); // 0.5 s: not yet (the hold)
    CHECK(s.o(0).phase == WaterFollowPhase::ToExit);
    CHECK(s.o(1).phase == WaterFollowPhase::ExitWait);
    CHECK(s.until(0, WaterFollowPhase::Surface, 1.f) >= 0.f);
    CHECK(s.o(1).phase == WaterFollowPhase::Surface);
    CHECK(s.o(0).exitIndex == -1);
    // Near the foot the queue holds (the lead may be lining up its climb).
    Scene n;
    n.exits = {ladder({1.f, 0.f})};
    n.bothIn();
    n.lead.swimming = false;
    n.lead.exiting = true;
    n.lead.exitIndex = 0;
    n.step();
    n.lead.swimming = true;
    n.lead.exiting = false;
    n.lead.position = {2.5f, 0.f, -1.f};
    n.step(60);
    CHECK(n.o(0).phase == WaterFollowPhase::ToExit);
}

TEST_CASE("water follow: a held follower leaves the climb queue; the ones after it climb") {
    Scene s;
    s.exits = {ladder({1.f, 0.f})};
    s.bothIn();
    s.lead.swimming = false;
    s.lead.exiting = true;
    s.lead.exitIndex = 0;
    s.lead.out = true;
    s.members[0].held = true; // a scene holds the first; it never reaches its foot
    s.step();
    REQUIRE(s.o(0).phase == WaterFollowPhase::ToExit);
    s.members[1].atExit = true;
    CHECK(s.until(1, WaterFollowPhase::Climbing, 1.f) >= 0.f);
    CHECK(s.o(0).phase == WaterFollowPhase::ToExit);
    // A held one at its foot does not climb while held.
    s.members[0].atExit = true;
    s.step(30);
    CHECK(s.o(0).phase == WaterFollowPhase::ExitWait);
    s.members[0].held = false;
    s.step(2);
    CHECK(s.o(0).phase == WaterFollowPhase::Climbing);
}

TEST_CASE("water follow: the watchdog places a follower stuck 20 s (never reaching its exit, or far off the lead)") {
    Scene s;
    s.exits = {ladder({1.f, 0.f})};
    s.bothIn();
    s.lead.swimming = false;
    s.lead.exiting = true;
    s.lead.exitIndex = 0;
    s.lead.out = true;
    s.step();
    REQUIRE(s.o(0).phase == WaterFollowPhase::ToExit);
    s.members[1].atExit = true;
    // The first never comes nearer its foot (stuck on something).
    const float t = s.until(0, WaterFollowPhase::Placed, 25.f);
    CHECK(t == doctest::Approx(20.f).epsilon(0.02));
    CHECK(s.o(0).changed);
    // Placed, the next step it is ashore (the lead is out); the one behind it climbed meanwhile.
    CHECK(s.o(1).phase != WaterFollowPhase::ExitWait);
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::Ashore);
    // Coming nearer resets it: 15 s each way with progress between is no expiry.
    Scene p;
    p.exits = {ladder({30.f, 0.f})};
    p.bothIn();
    p.lead.swimming = false;
    p.lead.exiting = true;
    p.lead.exitIndex = 0;
    p.step();
    REQUIRE(p.o(0).phase == WaterFollowPhase::ToExit);
    p.step(450);
    p.members[0].position.x += 5.f;
    p.step(450);
    CHECK(p.o(0).phase == WaterFollowPhase::ToExit);
    CHECK(p.party.stuckFor(0) == doctest::Approx(15.f).epsilon(0.02));
    // Waiting at the foot behind one still on its way is explained: slot 1 outlasts slot 0's stuck swim.
    Scene q;
    q.exits = {ladder({1.f, 0.f})};
    q.bothIn();
    q.lead.swimming = false;
    q.lead.exiting = true;
    q.lead.exitIndex = 0;
    q.lead.out = true;
    q.members[0].position = {20.f, 1.f, -1.f};
    q.step();
    q.members[1].atExit = true;
    for (int k = 0; k < 15; ++k) { // slot 0 comes 1 m nearer a second for 15 s, then sticks
        q.members[0].position.x -= 1.f;
        q.step(30);
    }
    q.step(30 * 10);
    CHECK(q.o(0).phase == WaterFollowPhase::ToExit);
    CHECK(q.o(1).phase == WaterFollowPhase::ExitWait);
    CHECK(q.party.stuckFor(1) == 0.f);
    CHECK(q.until(0, WaterFollowPhase::Placed, 12.f) >= 0.f); // then slot 0 is placed and slot 1 climbs
    CHECK(q.until(1, WaterFollowPhase::Climbing, 3.f) >= 0.f);
    // The lead treads by the foot (turned back, within exitReturn): the waiting ones are not placed.
    Scene b;
    b.exits = {ladder({1.f, 0.f})};
    b.bothIn();
    b.lead.swimming = false;
    b.lead.exiting = true;
    b.lead.exitIndex = 0;
    b.step();
    b.members[0].atExit = b.members[1].atExit = true;
    b.step();
    REQUIRE(b.o(0).phase == WaterFollowPhase::ExitWait);
    b.lead.swimming = true;
    b.lead.exiting = false;
    b.lead.position = {2.5f, 0.f, -1.f};
    b.step(30 * 30);
    CHECK(b.o(0).phase == WaterFollowPhase::ExitWait);
    CHECK(b.o(1).phase == WaterFollowPhase::ExitWait);
    // Catching up from far behind a lead sprinting away is progress, not stuck.
    Scene c;
    c.bothIn();
    c.members[0].position = {-25.f, 0.f, -1.f};
    for (int k = 0; k < 30 * 30; ++k) {
        c.lead.position.x += 3.2f / 30.f;
        c.members[0].position.x += 3.4f / 30.f;
        c.step();
    }
    CHECK(c.o(0).phase == WaterFollowPhase::Surface);
    // At the surface: far from the lead for 20 s.
    Scene f;
    f.bothIn();
    f.members[0].position = {40.f, 0.f, -1.f}; // 35 m off a lead that does not move
    CHECK(f.until(0, WaterFollowPhase::Placed, 25.f) == doctest::Approx(20.f).epsilon(0.02));
    CHECK(f.o(1).phase == WaterFollowPhase::Surface); // the near one is not stuck
    // Placed in open water with the lead swimming: rejoined by the lead, not frozen 35 m off.
    CHECK(f.o(0).rejoin);
    CHECK(std::fabs(f.o(0).target.x - f.lead.position.x) < 4.f);
    f.members[0].position = f.o(0).target;
    f.step();
    CHECK(f.o(0).phase == WaterFollowPhase::Surface);
}

TEST_CASE("water follow: a lead teleported mid-swim, or back from a park on dry ground, has the swimmers placed") {
    Scene s;
    s.bothIn();
    s.lead.position = {60.f, 0.f, -1.f}; // a scene or a fight moved it 55 m
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::Placed);
    CHECK(s.o(0).place);
    CHECK(s.o(1).phase == WaterFollowPhase::Placed);
    // The lead still swims: the placed step rejoins them at their surface slots by the lead (not frozen where they were).
    CHECK(s.o(0).rejoin);
    CHECK(s.o(0).target.x == doctest::Approx(60.f - 2.5f).epsilon(0.05));
    CHECK(s.o(0).target.y > 0.f);
    CHECK(s.o(1).target.y < 0.f);
    s.members[0].position = s.o(0).target; // the host put them there
    s.members[1].position = s.o(1).target;
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::Surface);
    CHECK(s.o(1).phase == WaterFollowPhase::Surface);
    CHECK_FALSE(s.o(0).rejoin);
    // It swims on with the lead from there.
    s.lead.velocity = {1.5f, 0.f};
    for (int k = 0; k < 60; ++k) {
        s.lead.position.x += 1.5f / 30.f;
        s.step();
    }
    CHECK(s.o(0).phase == WaterFollowPhase::Surface);
    CHECK(s.o(0).speed > 0.f);
    // A rejoin step the host missed (the member not moved): asked again, still Placed, until it is there.
    Scene m;
    m.bothIn();
    m.lead.position = {60.f, 0.f, -1.f};
    m.step();
    REQUIRE(m.o(0).rejoin);
    m.step(3);
    CHECK(m.o(0).phase == WaterFollowPhase::Placed);
    CHECK(m.o(0).rejoin);
    m.members[0].position = m.o(0).target;
    m.step();
    CHECK(m.o(0).phase == WaterFollowPhase::Surface);
    // The slot on a block: the host put it at the first water toward the lead, 5 m off the slot (past rejoinNear).
    // Reported as placedAt, it is accepted there and swims, never frozen in Placed.
    Scene pb;
    pb.bothIn();
    pb.lead.position = {60.f, 0.f, -1.f};
    pb.step();
    REQUIRE(pb.o(0).rejoin);
    const Vec2 off{pb.o(0).target.x + 5.f, pb.o(0).target.y};
    pb.members[0].position.x = off.x;
    pb.members[0].position.y = off.y;
    pb.step();
    CHECK(pb.o(0).phase == WaterFollowPhase::Placed); // unreported: still asked again
    pb.members[0].hasPlacedAt = true;
    pb.members[0].placedAt = off;
    pb.step();
    CHECK(pb.o(0).phase == WaterFollowPhase::Surface);
    // A report far from where it stands is not taken.
    Scene pf;
    pf.bothIn();
    pf.lead.position = {60.f, 0.f, -1.f};
    pf.step();
    REQUIRE(pf.o(0).rejoin);
    pf.members[0].hasPlacedAt = true;
    pf.members[0].placedAt = {pf.o(0).target.x + 5.f, pf.o(0).target.y};
    pf.step(3);
    CHECK(pf.o(0).phase == WaterFollowPhase::Placed);
    // Rejoined, then the lead is out of the water: Placed one more step without a rejoin (the host lands it), then ashore.
    Scene rj;
    rj.bothIn();
    rj.lead.position = {60.f, 0.f, -1.f};
    rj.step();
    REQUIRE(rj.o(0).rejoin);
    rj.members[0].position = rj.o(0).target;
    rj.lead.swimming = false;
    rj.step();
    CHECK(rj.o(0).phase == WaterFollowPhase::Placed);
    CHECK_FALSE(rj.o(0).rejoin);
    rj.step();
    CHECK(rj.o(0).phase == WaterFollowPhase::Ashore);
    // Placed with the lead out of the water: ashore the next step.
    Scene a;
    a.bothIn();
    a.lead.position = {60.f, 0.f, 1.f};
    a.lead.swimming = false;
    a.step();
    REQUIRE(a.o(0).phase == WaterFollowPhase::Placed);
    CHECK_FALSE(a.o(0).rejoin);
    a.step();
    CHECK(a.o(0).phase == WaterFollowPhase::Ashore);
    // A teleport onto dry ground.
    Scene d;
    d.bothIn();
    d.lead.swimming = false;
    d.lead.position = {-40.f, 0.f, 1.f};
    d.step();
    CHECK(d.o(0).phase == WaterFollowPhase::Placed);
    // Back from a park on dry ground without moving far: placed, not a swim to an exit.
    Scene r;
    r.exits = {ladder({1.f, 0.f})};
    r.bothIn();
    r.lead.swimming = false;
    r.lead.resumed = true;
    r.lead.position = {-2.f, 0.f, 0.5f};
    r.step();
    CHECK(r.o(0).phase == WaterFollowPhase::Placed);
    CHECK(r.o(1).phase == WaterFollowPhase::Placed);
    // Back from a park still swimming: the plan goes on.
    Scene w;
    w.bothIn();
    w.lead.resumed = true;
    w.step();
    CHECK(w.o(0).phase == WaterFollowPhase::Surface);
    // A teleport while one walks to its entry: back ashore, and the swim's wait starts again.
    Scene e;
    e.step(30);
    REQUIRE(e.o(0).phase == WaterFollowPhase::ToEntry);
    e.lead.position = {40.f, 0.f, -1.f};
    e.step();
    CHECK(e.o(0).phase == WaterFollowPhase::Ashore);
    CHECK(e.until(0, WaterFollowPhase::ToEntry, 2.f) == doctest::Approx(0.8f).epsilon(0.06));
}

TEST_CASE("water follow: a lens hide is visual only: the swimmer keeps its phase; a party hide clears the slot") {
    Scene s;
    s.bothIn();
    s.step(30);
    REQUIRE(s.o(0).phase == WaterFollowPhase::Surface);
    const SwimVec target = s.o(0).target;
    s.members[0].lensHidden = true; // the camera passes through it
    s.step(30);
    CHECK(s.o(0).phase == WaterFollowPhase::Surface);
    CHECK(s.o(0).target.x == doctest::Approx(target.x));
    s.members[0].lensHidden = false;
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::Surface);
    // The party hidden (P or LB): inactive, its slot clears.
    s.members[1].active = false;
    s.step();
    CHECK(s.o(1).phase == WaterFollowPhase::Ashore);
}

TEST_CASE("water follow: a walk to an entry not over in 15 s is given up, and picked again") {
    Scene s;
    s.step(30);
    REQUIRE(s.o(0).phase == WaterFollowPhase::ToEntry);
    const float t = s.until(0, WaterFollowPhase::Ashore, 20.f); // it never gets there
    CHECK(t == doctest::Approx(15.f).epsilon(0.02));
    CHECK(s.o(0).entry.kind == WaterFollowEntryKind::None);
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::ToEntry);
    // The second walk given up while the lead swims: placed with the lead, in the water (a rejoin), then at the surface.
    CHECK(s.until(0, WaterFollowPhase::Placed, 20.f) == doctest::Approx(15.f).epsilon(0.02));
    CHECK(s.o(0).rejoin);
    s.members[0].position = s.o(0).target;
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::Surface);
    // A lead out of the water between the two forgets the first.
    Scene f;
    f.step(30);
    REQUIRE(f.until(0, WaterFollowPhase::Ashore, 20.f) >= 0.f);
    f.lead.swimming = false;
    f.step(2);
    f.lead.swimming = true;
    REQUIRE(f.until(0, WaterFollowPhase::ToEntry, 2.f) >= 0.f);
    CHECK(f.until(0, WaterFollowPhase::Ashore, 20.f) == doctest::Approx(15.f).epsilon(0.02));
    // A walk given up because the lead was lost (moved past the teleport distance in one step) is no failed entry.
    Scene g;
    g.step(30);
    REQUIRE(g.o(0).phase == WaterFollowPhase::ToEntry);
    g.lead.position.x += 20.f;
    g.step();
    CHECK(g.o(0).phase == WaterFollowPhase::Ashore);
    CHECK(g.party.entryFailsFor(0) == 0);
    REQUIRE(g.until(0, WaterFollowPhase::ToEntry, 2.f) >= 0.f);
    // Its next walk given up is the first failure: ashore again, not placed.
    CHECK(g.until(0, WaterFollowPhase::Ashore, 20.f) == doctest::Approx(15.f).epsilon(0.02));
    CHECK(g.party.entryFailsFor(0) == 1);
}

TEST_CASE("water follow: with no water found toward the lead, a follower stays ashore") {
    Scene s;
    s.members[0].hasEdge = false;
    s.step(30);
    CHECK(s.o(0).phase == WaterFollowPhase::Ashore);
    CHECK(s.o(0).entry.kind == WaterFollowEntryKind::None);
    CHECK(s.o(1).phase == WaterFollowPhase::ToEntry);
    // Steps near its path need no edge.
    Scene t;
    t.exits = {steps({-1.f, 1.f}, {1.f, 1.f})};
    t.members[0].hasEdge = false;
    t.step(30);
    CHECK(t.o(0).phase == WaterFollowPhase::ToEntry);
    CHECK(t.o(0).entry.kind == WaterFollowEntryKind::Steps);
}

TEST_CASE("water follow: an inactive slot is cleared; a step allocates nothing") {
    Scene s;
    s.bothIn();
    s.members[1].active = false;
    s.step();
    CHECK(s.o(1).phase == WaterFollowPhase::Ashore);
    REQUIRE(ftcore_test::allocHookInstalled());
    const std::uint64_t before = ftcore_test::allocStats().count;
    s.members[1].active = true;
    for (int i = 0; i < 600; ++i) {
        s.lead.swim = i > 200 && i < 400 ? SwimState::Under : SwimState::Surface;
        s.lead.velocity = {std::cos(float(i) * 0.02f) * 1.5f, std::sin(float(i) * 0.02f) * 1.5f};
        s.lead.exiting = i > 500;
        s.lead.out = i > 540;
        s.members[0].atEntry = s.members[1].atEntry = true;
        s.members[0].entered = s.members[1].entered = true;
        s.members[0].atExit = s.members[1].atExit = i > 520;
        s.step();
    }
    CHECK(ftcore_test::allocStats().count == before);
}

namespace {

WaterFollowTuning fastExit() {
    WaterFollowTuning t;
    waterFollowFastExit(t);
    return t;
}

// A sea ladder as the road net builds one (Roads.cpp): its foot 0.35 m out from the wall's face, its top 1.2 m onto the
// quay, the quay `top` m over the water (0 here).
WaterExit seaLadder(float top) {
    WaterExit e = ladder({3.f, 0.f});
    e.top = {3.f - 1.55f, 0.f};
    e.topZ = top;
    return e;
}

// The lead climbs the ladder (the host: up in clamp(rise x 0.5, 0.9, 2.4) s) from its foot with both partners at their
// surface slots behind it; a simulated host swims each to its place in the queue at the order's speed and climbs it out,
// by the WaterClimbPath (climbPath) or by the host's own ladder move without it (up in 2.2 s, its first sixth the swim to
// the foot). Times from the lead out; the largest step a partner's feet make between two frames.
struct ExitRun {
    float leadOut = 0.f;
    float ashore[2] = {-1.f, -1.f};
    float worstStep = 0.f;
    float closestUnderLead = 99.f; // m: a climbing partner's head under the climbing lead's feet, the least
};

ExitRun ladderExit(const WaterFollowTuning& tuning, float wallTop, float fps) {
    constexpr float kDraft = 1.28f;
    const float dt = 1.f / fps;
    WaterFollowParty party(tuning);
    const WaterExit exit = seaLadder(wallTop);
    const WaterBodies water = sea();
    WaterFollowLead lead = swimmingLead({3.f, 0.f, -0.4f});
    lead.yaw = 180.f;
    WaterFollowMember m[2];
    for (int i = 0; i < 2; ++i) {
        m[i].active = true;
        m[i].position = {5.5f, i == 0 ? -1.5f : 1.5f, -kDraft};
        m[i].surfaceZ = 0.f;
        m[i].bedZ = -10.f;
    }
    // In the water at their slots: the party's phases as after a swim (an entry each, then the surface).
    for (int k = 0; k < 120; ++k) {
        for (WaterFollowMember& x : m) x.atEntry = x.entered = true;
        party.step(lead, m, 2, &exit, 1, dt, &water);
    }
    REQUIRE(party.order(0).phase == WaterFollowPhase::Surface);
    REQUIRE(party.order(1).phase == WaterFollowPhase::Surface);
    for (int i = 0; i < 2; ++i) m[i].position = {5.5f, i == 0 ? -1.5f : 1.5f, -kDraft};
    ExitRun r;
    const float rise = wallTop + kDraft;
    r.leadOut = std::clamp(rise * 0.5f, 0.9f, 2.4f);
    lead.swimming = false;
    lead.exitIndex = 0;
    WaterClimbPath path[2];
    SwimVec from[2];
    float climbT[2] = {-1.f, -1.f};
    for (float t = 0.f; t < 12.f; t += dt) {
        lead.out = t >= r.leadOut;
        lead.exiting = !lead.out;
        const float leadFeet = lead.out ? wallTop : -kDraft + rise * (t / r.leadOut);
        party.step(lead, m, 2, &exit, 1, dt, &water);
        for (int i = 0; i < 2; ++i) {
            const WaterFollowOrder& o = party.order(i);
            const SwimVec was = m[i].position;
            if (o.phase == WaterFollowPhase::ToExit) {
                const float dx = o.target.x - was.x, dy = o.target.y - was.y, d = std::sqrt(dx * dx + dy * dy);
                const float go = std::min(d, o.speed * dt);
                if (d > 1e-4f) m[i].position = {was.x + dx / d * go, was.y + dy / d * go, was.z};
                m[i].atExit = d - go <= tuning.atExit;
            } else if (o.phase == WaterFollowPhase::Climbing) {
                if (climbT[i] < 0.f) {
                    climbT[i] = 0.f;
                    from[i] = was;
                    path[i] = waterClimbPath(tuning, &exit, was, {});
                }
                climbT[i] += dt;
                if (tuning.climbPath) {
                    m[i].position = path[i].at(climbT[i]);
                    m[i].climbed = climbT[i] >= path[i].seconds();
                } else {
                    // FTPartyFollower's ladder move: up in the first half, across in the second, the first sixth the
                    // swim from its place in the queue to the foot.
                    const float u = std::min(climbT[i] / tuning.ladderSeconds, 1.f);
                    const float up = std::clamp(u * 2.f, 0.f, 1.f), across = std::clamp(u * 2.f - 1.f, 0.f, 1.f);
                    const float sw = std::clamp(u * 6.f, 0.f, 1.f);
                    const Vec2 flat = across > 0.f ? Vec2{exit.foot.x + (exit.top.x - exit.foot.x) * across,
                                                          exit.foot.y + (exit.top.y - exit.foot.y) * across}
                                                   : Vec2{from[i].x + (exit.foot.x - from[i].x) * sw,
                                                          from[i].y + (exit.foot.y - from[i].y) * sw};
                    m[i].position = {flat.x, flat.y, from[i].z + (wallTop - from[i].z) * up};
                    m[i].climbed = u >= 1.f;
                }
                if (!lead.out && m[i].position.z > -kDraft + 0.01f)
                    r.closestUnderLead = std::min(r.closestUnderLead, leadFeet - (m[i].position.z + 1.8f));
            } else if (o.phase == WaterFollowPhase::Ashore && climbT[i] >= 0.f && r.ashore[i] < 0.f) {
                r.ashore[i] = t - r.leadOut;
            }
            r.worstStep = std::max(r.worstStep, dist(was, m[i].position));
        }
    }
    return r;
}

} // namespace

TEST_CASE("water follow (fast exit): the way out is the lead's ladder, the flight it walked out by, else one within 25 m") {
    const WaterExit ex[] = {ladder({1.f, 0.f}), steps({-1.f, 10.f}, {1.f, 6.f})};
    CHECK(waterFollowWalkedOut(ex, 2, {0.f, 8.f}) == 1);   // on the flight's line, foot to top
    CHECK(waterFollowWalkedOut(ex, 2, {0.9f, 0.f}) == -1); // by the ladder: no flight
    CHECK(waterFollowExit(ex, 2, -1, {40.f, 0.f}, 25.f, true) == -1);
    CHECK(waterFollowExit(ex, 2, -1, {40.f, 0.f}) == 0); // without nearOnly, as before: the nearest however far
    // The lead walked out by the flight with the ladder nearer both partners: they take the flight.
    Scene s;
    s.party = WaterFollowParty(fastExit());
    s.exits = {ladder({1.f, 0.f}), steps({-1.f, 10.f}, {1.f, 6.f})};
    s.bothIn();
    s.lead.swimming = false;
    s.lead.out = true;
    s.lead.position = {0.5f, 7.5f, 0.5f};
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::ToExit);
    CHECK(s.o(0).exitIndex == 1);
    CHECK(s.o(1).exitIndex == 1);
    // Without the switch each takes the exit nearest itself.
    Scene d;
    d.exits = s.exits;
    d.bothIn();
    d.lead.swimming = false;
    d.lead.out = true;
    d.lead.position = {0.5f, 7.5f, 0.5f};
    d.step();
    CHECK(d.o(0).exitIndex == 0);
    // The lead's ladder climb within 25 m: that ladder.
    Scene c;
    c.party = WaterFollowParty(fastExit());
    c.exits = {ladder({1.f, 0.f}), ladder({1.f, 20.f})};
    c.bothIn();
    c.lead.swimming = false;
    c.lead.exiting = true;
    c.lead.exitIndex = 1;
    c.step();
    CHECK(c.o(0).exitIndex == 1);
    CHECK(c.o(1).exitIndex == 1);
    // A mantle (no exit) with the only way out 60 m off, in water that may not be this one: none, the host's mantle.
    Scene f;
    f.party = WaterFollowParty(fastExit());
    f.exits = {ladder({1.f, 60.f})};
    f.bothIn();
    f.lead.swimming = false;
    f.lead.out = true;
    f.lead.position = {-1.f, 0.f, 0.5f};
    f.step();
    CHECK(f.o(0).phase == WaterFollowPhase::ToExit);
    CHECK(f.o(0).exitIndex == -1);
    CHECK(f.o(1).exitIndex == -1);
}

TEST_CASE("water follow (fast exit): up a sea ladder both partners are out within 4 s of the lead, 20 cm a frame at most") {
    // The open sea's quay at the watchdog spot stands 4-5 m over the water; a canal's bank about 1 m.
    for (const float wall : {4.9f, 4.f, 1.f}) {
        for (const float fps : {30.f, 20.f}) {
            const ExitRun r = ladderExit(fastExit(), wall, fps);
            MESSAGE("fast exit, wall " << wall << " m at " << fps << " fps: lead out at " << r.leadOut
                                       << " s; partners on the quay " << r.ashore[0] << " s and " << r.ashore[1]
                                       << " s after it; worst step " << r.worstStep * 100.f
                                       << " cm; least head room under the lead " << r.closestUnderLead << " m");
            CHECK(r.ashore[0] >= 0.f);
            CHECK(r.ashore[1] >= 0.f);
            CHECK(r.ashore[0] <= 4.f);
            CHECK(r.ashore[1] <= 4.f);
            CHECK(r.worstStep <= 0.2f);
            CHECK(r.closestUnderLead >= 0.f); // never climbing into the lead's feet
        }
    }
    // Without the switch (the host's ladder move): the same quay is climbed in 2.2 s, its first sixth a dash to the foot,
    // and the climb waits for the lead to be out; that is the jump the path removes.
    const ExitRun d = ladderExit(WaterFollowTuning{}, 4.9f, 30.f);
    MESSAGE("default, wall 4.9 m at 30 fps: partners " << d.ashore[0] << " s and " << d.ashore[1]
                                                     << " s after the lead; worst step " << d.worstStep * 100.f << " cm");
    CHECK(d.worstStep > 0.2f);
}

TEST_CASE("water follow (fast exit): the climb path: the swim to the foot, up, across; a flight one walk as before") {
    const WaterFollowTuning t = fastExit();
    const WaterExit l = seaLadder(4.f);
    const WaterClimbPath p = waterClimbPath(t, &l, {4.2f, 0.f, -1.28f}, {});
    CHECK(p.swim == doctest::Approx(1.2f / t.footSwimSpeed));
    CHECK(p.climb == doctest::Approx(5.28f / t.ladderSpeed));
    CHECK(p.across == doctest::Approx(1.55f / t.acrossSpeed));
    CHECK(p.leg(0.1f) == 0);
    CHECK(p.leg(p.swim + 0.1f) == 1);
    CHECK(p.leg(p.swim + p.climb + 0.1f) == 2);
    CHECK(p.at(p.swim).x == doctest::Approx(3.f));
    CHECK(p.at(p.swim + p.climb).z == doctest::Approx(4.f));
    CHECK(p.at(p.seconds() + 1.f).x == doctest::Approx(1.45f));
    // A mantle (no exit): up the bank where it is, then across to the host's landing.
    const WaterClimbPath mt = waterClimbPath(t, nullptr, {1.f, 0.f, -1.28f}, {-1.f, 0.f, 0.5f});
    CHECK(mt.swim == doctest::Approx(0.f));
    CHECK(mt.climb == doctest::Approx(1.78f / t.ladderSpeed));
    CHECK(mt.at(mt.seconds()).x == doctest::Approx(-1.f));
    // A flight: one walk at the steps' speed, at least 0.5 s, as the host's own.
    WaterExit st = steps({-1.f, 0.f}, {3.f, 0.f});
    st.topZ = 1.f;
    const WaterClimbPath w = waterClimbPath(t, &st, {3.f, 0.f, -1.28f}, {});
    CHECK(w.swim == doctest::Approx(0.f));
    CHECK(w.climb == doctest::Approx(0.f));
    CHECK(w.across == doctest::Approx(std::sqrt(16.f + 2.28f * 2.28f) / t.stepsSpeed));
}

namespace {

// The 11-19 Strip run (it2_exit3.log) replayed: ladder 0 (foot (39335, 5750), top 1.55 m onto the quay, 2.95 m up from
// the swimmers' feet), the lead 1.51 s up it, both partners sent to the queue as it starts up, from where they were:
// slot 0 at (39680, 5505), slot 1 at (39732, 5742) cm (here in metres from the sea ladder's foot at (3, 0)). The host
// as measured there: each swims to its place at its own mean speed (slot 0 1.36 m/s, slot 1 1.41 m/s), and one waiting
// at its place drifts on toward the foot, slowing at 1.1 m/s^2 (slot 1 drifted 0.9 m); the climb is Core's path.
struct ReplayRun {
    float ashore[2] = {-1.f, -1.f};
    float footAt[2] = {-1.f, -1.f}; // when each reached the ladder's foot (its climb path's swim over)
    float swim[2] = {0.f, 0.f};     // each one's swim to the foot on its climb path, from where its climb started
    float worstWait[2] = {0.f, 0.f}; // the watchdog's longest count while each waited at its place (ExitWait)
    float worstStep = 0.f;
};

ReplayRun stripExitReplay(const WaterFollowTuning& tuning) {
    constexpr float kDraft = 1.28f, kRise = 2.95f, kLeadUp = 1.51f, kDt60 = 1.f / 60.f;
    const float speed[2] = {1.36f, 1.41f};
    WaterFollowParty party(tuning);
    const WaterExit exit = seaLadder(kRise - kDraft);
    const WaterBodies water = sea();
    WaterFollowLead lead = swimmingLead({3.f, 0.f, -0.4f});
    lead.yaw = 180.f;
    WaterFollowMember m[2];
    const SwimVec start[2] = {{3.f + 3.45f, -2.45f, -kDraft}, {3.f + 3.97f, -0.08f, -kDraft}};
    for (int i = 0; i < 2; ++i) {
        m[i].active = true;
        m[i].position = start[i];
        m[i].surfaceZ = 0.f;
        m[i].bedZ = -10.f;
    }
    for (int k = 0; k < 120; ++k) {
        for (WaterFollowMember& x : m) x.atEntry = x.entered = true;
        party.step(lead, m, 2, &exit, 1, kDt60, &water);
    }
    REQUIRE(party.order(0).phase == WaterFollowPhase::Surface);
    REQUIRE(party.order(1).phase == WaterFollowPhase::Surface);
    for (int i = 0; i < 2; ++i) m[i].position = start[i];
    ReplayRun r;
    lead.swimming = false;
    lead.exitIndex = 0;
    WaterClimbPath path[2];
    float climbT[2] = {-1.f, -1.f}, drift[2] = {0.f, 0.f};
    Vec2 way[2] = {};
    for (float t = 0.f; t < 12.f; t += kDt60) {
        lead.out = t >= kLeadUp;
        lead.exiting = !lead.out;
        party.step(lead, m, 2, &exit, 1, kDt60, &water);
        for (int i = 0; i < 2; ++i) {
            const WaterFollowOrder& o = party.order(i);
            const SwimVec was = m[i].position;
            if (o.phase == WaterFollowPhase::ExitWait) r.worstWait[i] = std::max(r.worstWait[i], party.stuckFor(i));
            if (o.phase == WaterFollowPhase::ToExit) {
                const float dx = o.target.x - was.x, dy = o.target.y - was.y, d = std::sqrt(dx * dx + dy * dy);
                const float go = std::min(d, speed[i] * kDt60);
                if (d > 1e-4f) {
                    way[i] = {dx / d, dy / d};
                    m[i].position = {was.x + way[i].x * go, was.y + way[i].y * go, was.z};
                }
                drift[i] = speed[i];
                m[i].atExit = d - go <= tuning.atExit;
            } else if (o.phase == WaterFollowPhase::ExitWait) {
                drift[i] = std::max(0.f, drift[i] - 1.1f * kDt60);
                m[i].position = {was.x + way[i].x * drift[i] * kDt60, was.y + way[i].y * drift[i] * kDt60, was.z};
            } else if (o.phase == WaterFollowPhase::Climbing) {
                if (climbT[i] < 0.f) {
                    climbT[i] = 0.f;
                    path[i] = waterClimbPath(tuning, &exit, was, {});
                    r.swim[i] = path[i].swim;
                }
                climbT[i] += kDt60;
                m[i].position = path[i].at(climbT[i]);
                m[i].climbed = climbT[i] >= path[i].seconds();
                if (r.footAt[i] < 0.f && climbT[i] >= path[i].swim) r.footAt[i] = t - kLeadUp;
            } else if (o.phase == WaterFollowPhase::Ashore && climbT[i] >= 0.f && r.ashore[i] < 0.f) {
                r.ashore[i] = t - kLeadUp;
            }
            r.worstStep = std::max(r.worstStep, dist(was, m[i].position));
        }
    }
    return r;
}

} // namespace

TEST_CASE("water follow (fast exit): the 11-19 Strip exit replayed; one at its place climbs ahead of one still swimming") {
    // In index order (the 11-19 rules): slot 1 waits at its place for slot 0, still swimming to the foot, then climbGap;
    // ashore past 4 s, as measured (4.25 s).
    WaterFollowTuning inOrder = fastExit();
    inOrder.exitReadyFirst = false;
    const ReplayRun a = stripExitReplay(inOrder);
    MESSAGE("index order: partners ashore " << a.ashore[0] << " s and " << a.ashore[1] << " s after the lead out");
    CHECK(a.ashore[1] > 4.f);
    CHECK(a.ashore[1] < 4.6f);
    // In index order: slot 0 at the foot and ashore first, slot 1 after it.
    CHECK(a.footAt[0] >= 0.f);
    CHECK(a.footAt[0] < a.footAt[1]);
    CHECK(a.ashore[0] >= 0.f);
    CHECK(a.ashore[0] < a.ashore[1]);
    // Ready first: slot 1 climbs from its place at once; slot 0 reaches the foot climbGap after it. Both by 3.7 s.
    const WaterFollowTuning t = fastExit();
    REQUIRE(t.exitReadyFirst);
    const ReplayRun b = stripExitReplay(t);
    MESSAGE("ready first: partners ashore " << b.ashore[0] << " s and " << b.ashore[1] << " s after the lead out; at the foot "
                                            << b.footAt[1] << " s then " << b.footAt[0] << " s; worst step "
                                            << b.worstStep * 100.f << " cm");
    CHECK(b.ashore[0] >= 0.f);
    CHECK(b.ashore[1] >= 0.f);
    CHECK(b.ashore[0] <= 3.7f);
    CHECK(b.ashore[1] <= 3.7f);
    CHECK(b.footAt[1] < b.footAt[0]);
    CHECK(b.footAt[0] - b.footAt[1] >= t.climbGap - 0.02f);
    CHECK(b.worstStep <= 0.2f);
    // The watchdog at the foot (07:20 ruling): it counts a wait at its place only with the queue clear, where the one
    // wait left is the spacing (climbGap, and the last climber's swim to the foot). Here it stays within that bound and
    // ends in a climb, far under the watchdog.
    MESSAGE("watchdog at the places: slot 0 " << b.worstWait[0] << " s, slot 1 " << b.worstWait[1] << " s; foot swims "
                                             << b.swim[0] << " s and " << b.swim[1] << " s");
    for (int i = 0; i < 2; ++i) {
        CHECK(b.worstWait[i] <= b.swim[1 - i] + t.climbGap + 2.f / 60.f);
        CHECK(b.worstWait[i] < t.watchdog);
    }
    CHECK(b.worstWait[0] + b.worstWait[1] > 0.f); // the spacing wait was met (else the bound above proves nothing)
}

TEST_CASE("water follow (fast exit): ready first keeps the index order when both wait at their places") {
    // Both at their places before the lead is out: slot 0 first, slot 1 climbGap later (and its foot climbGap later).
    Scene s;
    s.party = WaterFollowParty(fastExit());
    s.exits = {seaLadder(1.f)};
    s.bothIn();
    s.lead.swimming = false;
    s.lead.exiting = true;
    s.lead.exitIndex = 0;
    s.lead.position = {3.f, 0.f, 0.f};
    s.step();
    REQUIRE(s.o(0).phase == WaterFollowPhase::ToExit);
    REQUIRE(s.o(1).phase == WaterFollowPhase::ToExit);
    s.members[0].position = {3.3f, 0.f, -1.28f};
    s.members[1].position = {4.5f, 0.f, -1.28f};
    s.members[0].atExit = s.members[1].atExit = true;
    s.step();
    REQUIRE(s.o(0).phase == WaterFollowPhase::ExitWait);
    REQUIRE(s.o(1).phase == WaterFollowPhase::ExitWait);
    s.lead.exiting = false;
    s.lead.out = true;
    CHECK(s.until(0, WaterFollowPhase::Climbing, 2.f) >= 0.f);
    CHECK(s.o(1).phase == WaterFollowPhase::ExitWait);
    const float gap = s.until(1, WaterFollowPhase::Climbing, 3.f);
    CHECK(gap >= fastExit().climbGap - kDt);
}

namespace {

// A start in the water (11-18 items 3 and 5): both members spawned at the lead's place, the plan started in the water,
// and the host's first step (dt 0, before the first placement).
void startAtLead(Scene& s) {
    s.members[0].position = s.members[1].position = s.lead.position;
    s.party.startInWater();
    s.party.step(s.lead, s.members, 2, s.exits.data(), s.exits.size(), 0.f, &s.water);
}

// The host puts each member asked to rejoin at its target.
void hostRejoins(Scene& s) {
    for (int i = 0; i < 2; ++i)
        if (s.o(i).rejoin) s.members[i].position = s.o(i).target;
}

bool landPhase(WaterFollowPhase p) {
    return p == WaterFollowPhase::Ashore || p == WaterFollowPhase::ToEntry || p == WaterFollowPhase::Entering;
}

} // namespace

TEST_CASE("water follow (start in the water): rejoined at the surface slots on the first step, then swimming") {
    Scene s;
    startAtLead(s);
    for (int i = 0; i < 2; ++i) {
        CHECK(s.o(i).phase == WaterFollowPhase::Placed);
        CHECK(s.o(i).rejoin);
        CHECK(s.water.depth(Vec2{s.o(i).target.x, s.o(i).target.y}) > kWaterSwimDepth);
    }
    CHECK(s.o(0).target.x == doctest::Approx(5.f - 2.5f).epsilon(0.05));
    CHECK(s.o(0).target.y > 0.f);
    CHECK(s.o(1).target.y < 0.f);
    hostRejoins(s);
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::Surface);
    CHECK(s.o(1).phase == WaterFollowPhase::Surface);
    CHECK_FALSE(s.o(0).rejoin);
    CHECK_FALSE(s.party.starting());
    // It swims on with the lead from there: never ashore, never to an entry.
    s.lead.velocity = {1.5f, 0.f};
    for (int k = 0; k < 90; ++k) {
        s.lead.position.x += 1.5f / 30.f;
        s.step();
        CHECK_FALSE(landPhase(s.o(0).phase));
        CHECK_FALSE(landPhase(s.o(1).phase));
    }
    CHECK(s.o(0).phase == WaterFollowPhase::Surface);
    // The same with a first step of a frame's length.
    Scene f;
    f.members[0].position = f.members[1].position = f.lead.position;
    f.party.startInWater();
    f.step();
    CHECK(f.o(0).rejoin);
    hostRejoins(f);
    f.step();
    CHECK(f.o(0).phase == WaterFollowPhase::Surface);
}

TEST_CASE("water follow (start in the water): the lead's swim seen late is waited for, at most the start's hold") {
    // Seen 10 frames late: asked to rejoin each frame meanwhile, never ashore, to an entry or to an exit; then swimming.
    Scene s;
    s.exits = {ladder({1.f, 0.f})};
    s.lead.swimming = false;
    startAtLead(s);
    for (int k = 0; k < 10; ++k) {
        CHECK(s.party.starting());
        CHECK(s.o(0).phase == WaterFollowPhase::Placed);
        CHECK(s.o(1).phase == WaterFollowPhase::Placed);
        CHECK(s.o(0).rejoin);
        CHECK(s.o(1).rejoin);
        hostRejoins(s);
        s.step();
    }
    s.lead.swimming = true;
    hostRejoins(s);
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::Surface);
    CHECK(s.o(1).phase == WaterFollowPhase::Surface);
    CHECK_FALSE(s.party.starting());
    s.step(60);
    CHECK(s.o(0).phase == WaterFollowPhase::Surface);
    // Never seen: the hold runs out (1 s), one more placed step without a rejoin (the host lands it), then ashore.
    Scene n;
    n.exits = {ladder({1.f, 0.f})};
    n.lead.swimming = false;
    n.lead.position = {-2.f, 0.f, 0.5f};
    startAtLead(n);
    float rejoinFor = 0.f;
    while (n.o(0).rejoin && rejoinFor < 3.f) {
        hostRejoins(n);
        n.step();
        rejoinFor += kDt;
    }
    CHECK(rejoinFor >= n.party.tuning().startHold - 0.01f);
    CHECK(rejoinFor <= n.party.tuning().startHold + 3.f * kDt);
    CHECK(n.o(0).phase == WaterFollowPhase::Placed);
    n.step();
    CHECK(n.o(0).phase == WaterFollowPhase::Ashore);
    CHECK(n.o(1).phase == WaterFollowPhase::Ashore);
    n.step(60); // no swim: no entry
    CHECK(n.o(0).phase == WaterFollowPhase::Ashore);
}

TEST_CASE("water follow (start in the water): a lead under water, or climbing out, at the start") {
    // Under: rejoined at the surface, then it dives after its delay as after the lead's own dive.
    Scene u;
    u.lead.swim = SwimState::Under;
    u.lead.position.z = -4.f;
    startAtLead(u);
    CHECK(u.o(0).rejoin);
    CHECK(u.o(0).target.z == doctest::Approx(0.f));
    hostRejoins(u);
    u.step();
    CHECK(u.o(0).phase == WaterFollowPhase::Surface);
    CHECK(u.until(1, WaterFollowPhase::Under, 2.f) > 0.f);
    CHECK(u.o(0).phase == WaterFollowPhase::Under);
    // Climbing out by a ladder: rejoined at the surface, then out by the lead's ladder, never to an entry.
    Scene e;
    e.exits = {ladder({1.f, 0.f}), ladder({1.f, 12.f})};
    e.lead.swimming = false;
    e.lead.exiting = true;
    e.lead.exitIndex = 1;
    startAtLead(e);
    CHECK(e.o(0).rejoin);
    hostRejoins(e);
    e.step();
    CHECK(e.o(0).phase == WaterFollowPhase::Surface);
    e.step();
    CHECK(e.o(0).phase == WaterFollowPhase::ToExit);
    CHECK(e.o(1).phase == WaterFollowPhase::ToExit);
    CHECK(e.o(0).exitIndex == 1);
    for (int k = 0; k < 60; ++k) {
        e.step();
        CHECK_FALSE(landPhase(e.o(0).phase));
    }
}

TEST_CASE("water follow (start in the water): without the start, the plan begins ashore as before; slots kept in water") {
    // No start: members at the lead are ashore to the plan, with no rejoin, and walk to an entry after the 0.8 s.
    Scene d;
    d.step();
    CHECK(d.o(0).phase == WaterFollowPhase::Ashore);
    CHECK_FALSE(d.o(0).rejoin);
    CHECK_FALSE(d.party.starting());
    CHECK(d.until(0, WaterFollowPhase::ToEntry, 2.f) == doctest::Approx(0.8f - kDt).epsilon(0.08));
    // By the quay, swimming away from it: the slots behind would be on land; they are pulled into the water.
    Scene q;
    q.lead = swimmingLead({1.5f, 0.f, -1.f}, {1.5f, 0.f});
    startAtLead(q);
    for (int i = 0; i < 2; ++i) {
        CHECK(q.o(i).rejoin);
        CHECK(q.o(i).target.x >= 0.f);
        CHECK(q.water.depth(Vec2{q.o(i).target.x, q.o(i).target.y}) > kWaterSwimDepth);
    }
    // A member not active at the start (not yet in the party) is not in the plan: its slot is cleared.
    Scene a;
    a.members[1].active = false;
    startAtLead(a);
    CHECK(a.o(0).phase == WaterFollowPhase::Placed);
    CHECK(a.o(1).phase == WaterFollowPhase::Ashore);
    CHECK_FALSE(a.o(1).rejoin);
    // A start after a swim drops the old plan (an exit queue, a dive) for the start's.
    Scene r;
    r.exits = {ladder({1.f, 0.f})};
    r.bothIn();
    r.lead.swimming = false;
    r.lead.exiting = true;
    r.step();
    REQUIRE(r.o(0).phase == WaterFollowPhase::ToExit);
    r.lead.exiting = false;
    r.lead.swimming = true;
    r.party.startInWater();
    r.step();
    CHECK(r.o(0).phase == WaterFollowPhase::Placed);
    CHECK(r.o(0).rejoin);
}

TEST_CASE("water follow (switch while swimming): the switch's start decision") {
    // swimming, under, climbing out, rescued
    CHECK(waterSwitchStart(true, false, false, false) == WaterSwitchStart::InWater);
    CHECK(waterSwitchStart(false, true, false, false) == WaterSwitchStart::InWater);
    CHECK(waterSwitchStart(true, true, false, false) == WaterSwitchStart::InWater);
    CHECK(waterSwitchStart(false, false, false, false) == WaterSwitchStart::Ashore);
    CHECK(waterSwitchStart(true, false, true, false) == WaterSwitchStart::Refused);
    CHECK(waterSwitchStart(false, false, true, false) == WaterSwitchStart::Refused);
    CHECK(waterSwitchStart(true, false, false, true) == WaterSwitchStart::Refused);
    CHECK(waterSwitchStart(false, true, false, true) == WaterSwitchStart::Refused);
    CHECK(waterSwitchStart(false, false, true, true) == WaterSwitchStart::Refused);
}

TEST_CASE("water follow (start in the water): a lead seen out of the water and resumed holds the start, not lost") {
    // The host's first step after a switch or a scene: the new lead's swim flag not set yet, its last climb's out flag
    // still up, and the planner just resumed. The members stay placed and are asked to rejoin; none is sent ashore,
    // placed back on land or queued for an exit.
    Scene s;
    s.exits = {ladder({1.f, 0.f})};
    s.lead.swimming = false;
    s.lead.out = true;
    s.lead.resumed = true;
    startAtLead(s);
    for (int i = 0; i < 2; ++i) {
        CHECK(s.o(i).phase == WaterFollowPhase::Placed);
        CHECK(s.o(i).rejoin);
        CHECK_FALSE(s.o(i).place);
    }
    s.lead.resumed = false;
    for (int k = 0; k < 5; ++k) {
        hostRejoins(s);
        s.step();
        CHECK(s.o(0).phase == WaterFollowPhase::Placed);
        CHECK(s.o(1).phase == WaterFollowPhase::Placed);
        CHECK_FALSE(s.o(0).place);
        CHECK(s.party.starting());
    }
    // The swim is seen: both at the surface, then swimming on.
    s.lead.swimming = true;
    s.lead.out = false;
    hostRejoins(s);
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::Surface);
    CHECK(s.o(1).phase == WaterFollowPhase::Surface);
    s.step(30);
    CHECK(s.o(0).phase == WaterFollowPhase::Surface);
    CHECK(s.o(1).phase == WaterFollowPhase::Surface);
    // Resumed with the swim already seen (a lead switch where the flag is up on the first step): straight to the surface.
    Scene r;
    r.lead.resumed = true;
    startAtLead(r);
    CHECK(r.o(0).rejoin);
    CHECK_FALSE(r.o(0).place);
    hostRejoins(r);
    r.lead.resumed = false;
    r.step();
    CHECK(r.o(0).phase == WaterFollowPhase::Surface);
    CHECK(r.o(1).phase == WaterFollowPhase::Surface);
}

// 11-20 swim proof, runs 5 and 6: followers in the lead's water only, and kept in it.
namespace {

// The sea (body 0, x >= 0) and a canal beside it on land (body 1, x -12..-8): other water, no swim to the sea.
WaterBodies seaAndCanal() {
    WaterBodies w = sea();
    WaterBody c;
    c.kind = WaterKind::Canal;
    c.shape = WaterShape::Box;
    c.level = 0.f;
    c.bed = -3.f;
    c.centre = {-10.f, 0.f};
    c.half = {2.f, 30.f};
    w.add(c);
    return w;
}

// The surface swimmer's state at p in the sea, as the host samples it: the feet by the surface rule over the bed there,
// a block (x 3..4, y -1..1) standing out of the water read as Dry (inside a solid), a shelf at `shelfBed` for x 6..7.
struct SeaSampler {
    const WaterBodies* water = nullptr;
    Vec2 at{};
    float shelfBed = -0.5f;
    mutable int calls = 0;
    WaterState operator()(Vec2 step) const {
        ++calls;
        const Vec2 p{at.x + step.x, at.y + step.y};
        if (p.x >= 3.f && p.x <= 4.f && p.y >= -1.f && p.y <= 1.f) return WaterState{};
        const float bed = p.x >= 6.f && p.x <= 7.f ? shelfBed : -10.f;
        WaterCapsule c;
        c.p = p;
        c.feetZ = waterSurfaceFeetZ(waterSwimFeetZ(0.f, 0.f), 0.f, bed);
        c.groundZ = bed;
        c.hasGround = true;
        c.swimming = true;
        return waterState(*water, c, 0.f);
    }
};

} // namespace

TEST_CASE("water follow (11-20): steps into other water than the lead's are not an entry; a body with none gives the jump") {
    const WaterBodies water = seaAndCanal();
    REQUIRE(water.bodyAt({5.f, 0.f}) == 0);
    REQUIRE(water.bodyAt({-11.f, 2.f}) == 1);
    const Vec2 from{-15.f, 1.f}, lead{5.f, 0.f}, edge{0.f, 1.f}, out{1.f, 0.f};
    const WaterExit canalSteps[] = {steps({-13.f, 2.f}, {-11.f, 2.f})};
    // 1. A flight whose foot is in the canal is rejected (run 5); with no bodies given it is still taken (as before).
    const WaterFollowEntry a = waterFollowEntry(canalSteps, 1, from, lead, edge, out, 8.f, &water);
    CHECK(a.kind == WaterFollowEntryKind::Jump);
    CHECK(a.exitIndex == -1);
    CHECK_FALSE(a.otherWater); // the jump from the sea's edge lands in the lead's body
    CHECK(waterFollowEntry(canalSteps, 1, from, lead, edge, out, 8.f).kind == WaterFollowEntryKind::Steps);
    // 2. A flight into the lead's own body is still chosen, over a nearer one into the canal.
    const WaterExit both[] = {steps({-13.f, 1.f}, {-11.f, 1.f}), steps({-1.f, 4.f}, {1.f, 4.f})};
    const WaterFollowEntry b = waterFollowEntry(both, 2, from, lead, edge, out, 8.f, &water);
    CHECK(b.kind == WaterFollowEntryKind::Steps);
    CHECK(b.exitIndex == 1);
    CHECK(b.into.x == doctest::Approx(1.f));
    // ... and one whose foot is in no body at all (a polygon a little short of the sea's edge) is not rejected.
    const WaterExit shortFoot[] = {steps({-2.f, 4.f}, {-0.2f, 4.f})};
    REQUIRE(water.bodyAt({-0.2f, 4.f}) == -1);
    CHECK(waterFollowEntry(shortFoot, 1, from, lead, edge, out, 8.f, &water).kind == WaterFollowEntryKind::Steps);
    // 3. The lead's body has no exits at all (the Strip's sea, 0 of 31): a jump, never no entry.
    CHECK(waterFollowEntry(canalSteps, 1, from, lead, edge, out, 8.f, &water).kind != WaterFollowEntryKind::None);
    CHECK(waterFollowEntry(nullptr, 0, from, lead, edge, out, 8.f, &water).kind == WaterFollowEntryKind::Jump);
    // ... and a jump whose landing is in the canal is marked: other water.
    const WaterFollowEntry c = waterFollowEntry(canalSteps, 1, from, lead, {-12.5f, 1.f}, out, 8.f, &water);
    CHECK(c.kind == WaterFollowEntryKind::Jump);
    CHECK(c.otherWater);
    // The party: such a jump is no way to the lead, so the follower is rejoined with it in the sea, then swims.
    Scene s;
    s.water = seaAndCanal();
    s.exits = {steps({-13.f, 1.f}, {-11.f, 1.f})};
    s.members[0].position = {-15.f, 1.f, 0.5f};
    s.members[0].edge = {-12.5f, 1.f};
    const float t = s.until(0, WaterFollowPhase::Placed, 2.f);
    CHECK(t >= 0.f);
    CHECK(s.o(0).place);
    CHECK(s.o(0).rejoin);
    CHECK(s.water.bodyAt({s.o(0).target.x, s.o(0).target.y}) == 0);
    CHECK(s.o(1).phase == WaterFollowPhase::ToEntry); // the other one's edge is the sea's: it jumps in as before
    CHECK(s.o(1).entry.kind == WaterFollowEntryKind::Jump);
    s.members[0].position = s.o(0).target;
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::Surface);
}

TEST_CASE("water follow (11-21): a jump cut off from the lead by a solid is not taken; another bank point is, else placed") {
    const WaterBodies water = seaAndCanal();
    const Vec2 from{-3.f, 1.f}, lead{5.f, 0.f};
    // Replay 6: the bank point toward the lead lands in a pocket behind a block (the host found the swim cut off).
    const WaterEntryEdge pocket{{0.f, 1.f}, {1.f, 0.f}, true};
    const WaterEntryEdge clear{{0.f, 12.f}, {1.f, 0.f}, false};
    const WaterEntryEdge canal{{-12.5f, 1.f}, {1.f, 0.f}, false};
    // 1. Cut off, a clear point further along the bank: the jump is from there.
    const WaterEntryEdge two[] = {pocket, clear};
    WaterFollowEntry e = waterFollowEntry(nullptr, 0, from, lead, two, 2, 8.f, &water);
    CHECK(e.kind == WaterFollowEntryKind::Jump);
    CHECK_FALSE(e.cutOff);
    CHECK_FALSE(e.otherWater);
    CHECK(e.point.y == doctest::Approx(12.f));
    CHECK(e.into.x == doctest::Approx(1.f));
    // 2. A clear point that lands in other water is passed over too (the run-5 body check holds for the others).
    const WaterEntryEdge three[] = {pocket, canal, clear};
    e = waterFollowEntry(nullptr, 0, from, lead, three, 3, 8.f, &water);
    CHECK(e.point.y == doctest::Approx(12.f));
    CHECK_FALSE(e.otherWater);
    // 3. Every point cut off (or in other water): the first, marked cut off.
    const WaterEntryEdge none[] = {pocket, canal};
    e = waterFollowEntry(nullptr, 0, from, lead, none, 2, 8.f, &water);
    CHECK(e.kind == WaterFollowEntryKind::Jump);
    CHECK(e.cutOff);
    CHECK(e.point.x == doctest::Approx(0.f));
    CHECK(e.point.y == doctest::Approx(1.f));
    // 4. Steps into the lead's body still come before any jump; no edges and no steps is None.
    const WaterExit flight[] = {steps({-1.f, 4.f}, {1.f, 4.f})};
    CHECK(waterFollowEntry(flight, 1, from, lead, none, 2, 8.f, &water).kind == WaterFollowEntryKind::Steps);
    CHECK(waterFollowEntry(nullptr, 0, from, lead, none, 0, 8.f, &water).kind == WaterFollowEntryKind::None);

    // The party: slot 0's only bank point is cut off, so it is placed with the lead at plan time (noEntry, a rejoin into
    // the lead's water), never sent to jump into the pocket; slot 1 is offered a clear point along the bank and jumps there.
    Scene s;
    s.water = seaAndCanal();
    s.members[0].position = {-3.f, 1.f, 0.5f};
    s.members[0].edge = {0.f, 1.f};
    s.members[0].edgeCutOff = true;
    s.members[1].position = {-3.f, -1.f, 0.5f};
    s.members[1].edge = {0.f, -1.f};
    s.members[1].edgeCutOff = true;
    s.members[1].altEdges[0] = clear;
    s.members[1].altEdgeCount = 1;
    bool noEntry = false, everEntry0 = false;
    for (int k = 0; k < 80 && s.o(0).phase != WaterFollowPhase::Placed; ++k) {
        s.step();
        noEntry = noEntry || s.o(0).noEntry;
        everEntry0 = everEntry0 || s.o(0).phase == WaterFollowPhase::ToEntry;
    }
    CHECK(s.o(0).phase == WaterFollowPhase::Placed);
    CHECK(noEntry);
    CHECK(s.o(0).place);
    CHECK(s.o(0).rejoin);
    CHECK(s.water.bodyAt({s.o(0).target.x, s.o(0).target.y}) == 0);
    CHECK_FALSE(everEntry0);
    s.step();
    CHECK(s.o(1).phase == WaterFollowPhase::ToEntry);
    CHECK(s.o(1).entry.kind == WaterFollowEntryKind::Jump);
    CHECK(s.o(1).entry.point.y == doctest::Approx(12.f));
    CHECK_FALSE(s.o(1).noEntry);
    CHECK_FALSE(s.o(0).noEntry); // reported on the step it was decided only
}

TEST_CASE("water follow (11-20): the surface step stays in the water: taken, slid along the edge, or held") {
    const WaterBodies water = sea();
    SeaSampler at{&water, {1.f, 0.f}};
    // Open water: taken as it is.
    WaterSurfaceStep r = waterSurfaceStep({0.05f, 0.f}, 1.f, at);
    CHECK(r.kind == WaterStepKind::Take);
    CHECK(r.step.x == doctest::Approx(0.05f));
    // Treading (no step): taken, nothing sampled.
    at.calls = 0;
    r = waterSurfaceStep({0.f, 0.f}, 1.f, at);
    CHECK(r.kind == WaterStepKind::Take);
    CHECK(at.calls == 0);
    // Over a shelf under the surface (0.5 m of water; the feet just over the bed): a wade, still in the water.
    at.at = {5.98f, 3.f};
    const WaterState shelf = at({0.05f, 0.f});
    CHECK(shelf.mode != WaterMode::Swim);
    CHECK(waterIn(shelf));
    CHECK(waterSurfaceStep({0.05f, 0.f}, 1.f, at).kind == WaterStepKind::Take);
    // A shelf at the surface (no water over it: the feet over the surface): out.
    at.shelfBed = 0.f;
    CHECK_FALSE(waterIn(at({0.05f, 0.f})));
    // Onto the land (x < 0): slid along the edge, the side given first, at the cosine's share (half at 60 degrees).
    at.at = {0.02f, 2.f};
    r = waterSurfaceStep({-0.05f, 0.f}, -1.f, at);
    CHECK(r.kind == WaterStepKind::Slide);
    CHECK(waterIn(at(r.step)));
    CHECK(r.side == -1.f);
    CHECK(std::hypot(r.step.x, r.step.y) == doctest::Approx(0.025f));
    // Into the block standing out of the sea: slid, never into it.
    at.at = {2.98f, 0.f};
    r = waterSurfaceStep({0.05f, 0.f}, 1.f, at);
    CHECK(r.kind == WaterStepKind::Slide);
    CHECK(at.at.x + r.step.x < 3.f);
    CHECK(r.side == 1.f);
    // Nowhere in the water: held where it is.
    const auto dry = [](Vec2) { return WaterState{}; };
    r = waterSurfaceStep({0.05f, 0.f}, 1.f, dry);
    CHECK(r.kind == WaterStepKind::Hold);
    CHECK(r.step.x == 0.f);
    CHECK(r.step.y == 0.f);
}

TEST_CASE("water follow (11-20): a swim round the block to a spot behind it never leaves the water, and allocates nothing") {
    const WaterBodies water = sea();
    SeaSampler at{&water, {1.5f, 0.2f}};
    const Vec2 to{5.5f, -0.2f};
    REQUIRE(ftcore_test::allocHookInstalled());
    const std::uint64_t before = ftcore_test::allocStats().count;
    float side = 1.f;
    int held = 0;
    int out = 0;
    bool reached = false;
    for (int i = 0; i < 30 * 20 && !reached; ++i) {
        const float dx = to.x - at.at.x, dy = to.y - at.at.y, far = std::hypot(dx, dy);
        const float len = std::min(1.5f * kDt, far);
        const WaterSurfaceStep r = waterSurfaceStep({dx / far * len, dy / far * len}, side, at);
        side = r.side;
        held += r.kind == WaterStepKind::Hold ? 1 : 0;
        at.at = {at.at.x + r.step.x, at.at.y + r.step.y};
        out += waterIn(at({0.f, 0.f})) ? 0 : 1;
        reached = far < 0.05f;
    }
    CHECK(ftcore_test::allocStats().count == before);
    CHECK(out == 0);
    CHECK(reached);
    CHECK(held == 0);
}

TEST_CASE("water follow (11-20): a follower blocked at the edge is rejoined by the watchdog, though the lead comes nearer") {
    Scene s;
    s.bothIn();
    // Held at the edge 4 m from the lead, which swims slowly toward it (run 5: the shrinking gap kept the count at 0).
    s.members[0].blocked = true;
    s.lead.velocity = {-0.1f, 0.f};
    float t = 0.f;
    while (t < 25.f && s.o(0).phase != WaterFollowPhase::Placed) {
        s.lead.position.x -= 0.1f * kDt;
        s.step();
        t += kDt;
    }
    CHECK(t == doctest::Approx(20.f).epsilon(0.02));
    CHECK(s.o(0).place);
    CHECK(s.o(0).rejoin);
    CHECK(s.o(1).phase == WaterFollowPhase::Surface); // not blocked: not placed
    s.members[0].position = s.o(0).target;
    s.members[0].blocked = false;
    s.step();
    CHECK(s.o(0).phase == WaterFollowPhase::Surface);
    // A full step resets the count: blocked 15 s, one free step, blocked 15 s more is no rejoin.
    Scene r;
    r.bothIn();
    r.members[0].blocked = true;
    r.step(450);
    CHECK(r.party.stuckFor(0) == doctest::Approx(15.f).epsilon(0.02));
    r.members[0].blocked = false;
    r.step();
    CHECK(r.party.stuckFor(0) == 0.f);
    r.members[0].blocked = true;
    r.step(450);
    CHECK(r.o(0).phase == WaterFollowPhase::Surface);
}

TEST_CASE("water follow (11-20): a follower sliding along the edge is counted only while it does not get on") {
    // Slid in place (turned, never 0.5 m from where the count began): rejoined at the watchdog's time, the lead near.
    Scene s;
    s.bothIn();
    s.members[0].turned = true;
    float t = 0.f;
    while (t < 25.f && s.o(0).phase != WaterFollowPhase::Placed) {
        s.step();
        t += kDt;
    }
    CHECK(t == doctest::Approx(20.f).epsilon(0.02));
    CHECK(s.o(0).rejoin);
    // Sliding along a quay wall and getting on with the lead (the gap kept): a swim, no rejoin.
    Scene g;
    g.bothIn();
    g.members[0].turned = true;
    for (int i = 0; i < 30 * 25; ++i) {
        g.members[0].position.y += 0.03f;
        g.lead.position.y += 0.03f;
        g.step();
    }
    CHECK(g.o(0).phase == WaterFollowPhase::Surface);
    CHECK(g.party.stuckFor(0) < 1.f);
}
