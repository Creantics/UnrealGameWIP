// Party followers in the water (WaterFollow.h): when they go in and how, the stagger, the surface slots, the dive delay,
// the under-water band and the camera keep-out, surfacing with the lead, the rescue placement, the exit choice and
// order, the aborts, and the slot clamp to the water.
#include "ftcore/WaterFollow.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

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
