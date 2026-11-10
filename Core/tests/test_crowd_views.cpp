// S-332: where a run ends (most walk on, a few look back with room of their own), no run pose below walking speed,
// and nobody popping in or out in front of a camera.
#include "ftcore/Crowd.h"

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

using namespace ftcore;

namespace {

std::vector<CrowdStreet> grid(int n, float spacing) {
    std::vector<CrowdStreet> out;
    const float o = -0.5f * spacing * static_cast<float>(n - 1);
    for (int r = 0; r < n; ++r) {
        CrowdStreet h, v;
        h.kind = v.kind = "street";
        h.width = v.width = 8.f;
        h.pavement = v.pavement = 2.f;
        for (int c = 0; c < n; ++c) {
            h.points.push_back({o + spacing * static_cast<float>(c), o + spacing * static_cast<float>(r)});
            v.points.push_back({o + spacing * static_cast<float>(r), o + spacing * static_cast<float>(c)});
        }
        out.push_back(h);
        out.push_back(v);
    }
    return out;
}

CrowdGraphTuning loose() {
    CrowdGraphTuning t;
    t.minComponent = 1;
    return t;
}

void tick(CrowdSim& sim, Vec2 player) { sim.tick(1.f / 15.f, player, {0.f, 0.f}); }

} // namespace

TEST_CASE("crowd flee end: most walk on out of the area, the few who stop look back with room, never a run pose standing") {
    CrowdGraph g;
    REQUIRE(g.build(grid(16, 20.f), loose())); // 300 m across
    CrowdTuning t;
    t.targetAgents = 220;
    std::uint32_t worstGroup = 0, slowRuns = 0;
    std::uint64_t watched = 0, walkedOn = 0;
    for (std::uint32_t seed = 1; seed <= 4; ++seed) {
        CrowdSim sim;
        REQUIRE(sim.init(g, t, seed));
        for (int s = 0; s < 15 * 15; ++s) tick(sim, {0.f, 0.f});
        sim.pushEvent({CrowdEventKind::FightStart, {0.f, 0.f}});
        for (int s = 0; s < 15 * 40; ++s) {
            if (s % 30 == 0) sim.pushEvent({CrowdEventKind::Strike, {0.f, 0.f}});
            tick(sim, {0.f, 0.f});
            slowRuns += sim.runPoseSlow();
            if (s % 8 == 0) worstGroup = std::max(worstGroup, sim.largestStoppedGroup(3.f));
        }
        watched += sim.counts().watched;
        walkedOn += sim.counts().walkedOn;
    }
    MESSAGE("watched ", watched, " walked on ", walkedOn, ", largest stopped group ", worstGroup);
    CHECK(walkedOn > 0);
    CHECK(walkedOn > watched); // most walk on
    CHECK(watched > 0);        // and a few stop and look
    CHECK(slowRuns == 0);
    CHECK(worstGroup <= 3);
}

TEST_CASE("crowd views: nobody spawns, despawns, or is drawn or undrawn in front of a camera, while running past it") {
    CrowdGraph g;
    REQUIRE(g.build(grid(16, 20.f), loose()));
    CrowdTuning t;
    t.targetAgents = 220;
    t.maxDrawn = 48;
    for (std::uint32_t seed = 1; seed <= 3; ++seed) {
        CrowdSim sim;
        REQUIRE(sim.init(g, t, seed));
        const CrowdViewCone cams[3] = {{{-14.f, -4.f}, 15.f, 35.f, 40.f}, {{14.f, -8.f}, 150.f, 35.f, 40.f}, {{0.f, 0.f}, 0.f, 180.f, 15.f}};
        sim.setViews(cams, 3);
        CHECK(sim.inView({0.f, 0.f}));
        CHECK(sim.inView({10.f, 10.f}));
        CHECK_FALSE(sim.inView({-100.f, 0.f}));
        // From the start (the first ticks fill the streets) with the cameras set, as the wrapper does.
        for (int s = 0; s < 15 * 15; ++s) tick(sim, {0.f, 0.f});
        sim.setDrawFocus(true, {0.f, 0.f});
        const auto before = sim.counts().popsInView;
        sim.pushEvent({CrowdEventKind::FightStart, {0.f, 0.f}});
        for (int s = 0; s < 15 * 45; ++s) tick(sim, {0.f, 0.f});
        CHECK(sim.counts().popsInView == before);
        CHECK(sim.counts().fled > 0);
    }
    // No cameras: the drawn pick is by distance only and nothing is counted.
    CrowdSim sim;
    REQUIRE(sim.init(g, t, 9));
    sim.setViews(nullptr, 0);
    CHECK_FALSE(sim.inView({0.f, 0.f}));
}

TEST_CASE("crowd views: a steady view never draws a member in sight; a cut (a new camera) shows a new picture") {
    CrowdGraph g;
    REQUIRE(g.build(grid(16, 20.f), loose()));
    CrowdTuning t;
    t.targetAgents = 220;
    t.maxDrawn = 8; // fewer drawn than in sight: the pick must not swap members in front of the camera
    CrowdSim sim;
    REQUIRE(sim.init(g, t, 5));
    sim.setDrawFocus(true, {0.f, 0.f});
    const CrowdViewCone east{{0.f, 0.f}, 0.f, 35.f, 40.f}, north{{0.f, 0.f}, 90.f, 35.f, 40.f};
    auto drawnIn = [&](const CrowdViewCone& c) {
        std::uint32_t n = 0;
        for (std::uint32_t i = 0; i < sim.capacity(); ++i) {
            const CrowdAgentView& v = sim.agent(i);
            if (!v.live || !v.drawn || std::hypot(v.pos.x - c.eye.x, v.pos.y - c.eye.y) > c.range) continue;
            float off = std::fabs(std::atan2(v.pos.y - c.eye.y, v.pos.x - c.eye.x) * 57.29578f - c.yaw);
            if (off > 180.f) off = 360.f - off;
            n += off <= c.halfFov;
        }
        return n;
    };
    for (int s = 0; s < 15 * 30; ++s) {
        sim.setViews(&east, 1);
        tick(sim, {0.f, 0.f});
    }
    CHECK(sim.counts().popsInView == 0);
    // The camera cuts to the north: members there may be drawn at once (nothing was seen there a step before).
    sim.setViews(&north, 1);
    tick(sim, {0.f, 0.f});
    CHECK(drawnIn(north) > 0);
    CHECK(sim.counts().popsInView == 0);
    for (int s = 0; s < 15 * 30; ++s) {
        sim.setViews(&north, 1);
        tick(sim, {0.f, 0.f});
    }
    CHECK(sim.counts().popsInView == 0);
}
