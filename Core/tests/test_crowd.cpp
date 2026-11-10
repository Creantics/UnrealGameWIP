#include "ftcore/Crowd.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace ftcore;

namespace {

float dist(Vec2 a, Vec2 b) { return std::hypot(b.x - a.x, b.y - a.y); }

// A square grid of streets `n` x `n` nodes, `spacing` m apart, sharing their crossing points, centred on 0.
std::vector<CrowdStreet> gridStreets(int n, float spacing, float width = 8.f, float pavement = 2.f) {
    std::vector<CrowdStreet> out;
    const float o = -0.5f * spacing * static_cast<float>(n - 1);
    for (int r = 0; r < n; ++r) {
        CrowdStreet h, v;
        h.kind = v.kind = "street";
        h.width = v.width = width;
        h.pavement = v.pavement = pavement;
        for (int c = 0; c < n; ++c) {
            h.points.push_back({o + spacing * static_cast<float>(c), o + spacing * static_cast<float>(r)});
            v.points.push_back({o + spacing * static_cast<float>(r), o + spacing * static_cast<float>(c)});
        }
        out.push_back(h);
        out.push_back(v);
    }
    return out;
}

CrowdStreet line(Vec2 a, Vec2 b, float width = 6.f, const char* kind = "street") {
    CrowdStreet s;
    s.kind = kind;
    s.width = width;
    s.points = {a, b};
    return s;
}

CrowdGraphTuning loose() {
    CrowdGraphTuning t;
    t.minComponent = 1;
    return t;
}

// Calm, exact tuning for the transition tests: no jitter, no idling, no automatic population.
CrowdTuning quiet() {
    CrowdTuning t;
    t.reactJitter = 0.f;
    t.idleChance = 0.f;
    t.walkSpeedJitter = 0.f;
    t.headingChange = 0.f;
    t.reactRadius = 0.f; // the close-range rule has its own tests
    return t;
}

std::uint32_t nodeAt(const CrowdGraph& g, Vec2 p) { return g.nearest(p, 1.f); }

// k such that node n's k-th edge leads to m.
std::uint32_t edgeTo(const CrowdGraph& g, std::uint32_t n, std::uint32_t m) {
    for (std::uint32_t k = 0; k < g.degree(n); ++k)
        if (g.edge(n, k).to == m) return k;
    return 0;
}

void run(CrowdSim& sim, float seconds, Vec2 player = {0.f, 0.f}) {
    const int steps = static_cast<int>(std::lround(seconds * 15.f));
    for (int i = 0; i < steps; ++i) sim.tick(1.f / 15.f, player, {0.f, 0.f});
}

// The states a member went through, each change once, over `seconds`.
std::vector<CrowdState> trace(CrowdSim& sim, std::uint32_t a, float seconds, Vec2 player = {0.f, 0.f}) {
    std::vector<CrowdState> seen{sim.agent(a).state};
    const int steps = static_cast<int>(std::lround(seconds * 15.f));
    for (int i = 0; i < steps; ++i) {
        sim.tick(1.f / 15.f, player, {0.f, 0.f});
        if (sim.agent(a).state != seen.back()) seen.push_back(sim.agent(a).state);
    }
    return seen;
}

// Whether `want` appears in `seen` in order (not necessarily adjacent).
bool inOrder(const std::vector<CrowdState>& seen, std::initializer_list<CrowdState> want) {
    auto it = seen.begin();
    for (const CrowdState w : want) {
        it = std::find(it, seen.end(), w);
        if (it == seen.end()) return false;
        ++it;
    }
    return true;
}

std::string envText(const char* name) {
    std::string value;
#ifdef _MSC_VER
    char* raw = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&raw, &length, name) == 0 && raw != nullptr) value = raw;
    std::free(raw);
#else
    if (const char* raw = std::getenv(name)) value = raw;
#endif
    return value;
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("crowd streets: kind and pavement read, points moved to the engine frame") {
    const char* csv = "# comment\n"
                      "id,kind,width_m,pts,pavement_m\n"
                      "ST-1,street,10,0:0;100:0,2\n"
                      "ST-2,stair,4,50:0;50:50,\n";
    std::vector<CrowdStreet> s;
    std::string err;
    REQUIRE(importCrowdStreets(csv, 100.0, s, &err));
    REQUIRE(s.size() == 2);
    CHECK(s[0].kind == "street");
    CHECK(s[0].pavement == doctest::Approx(2.f));
    CHECK(s[1].kind == "stair");
    CHECK(s[1].pavement == 0.f);
    // plan (0,0) in a 100 m square is the engine's (-50, +50): X east, Y south.
    CHECK(s[0].points[0].x == doctest::Approx(-50.f));
    CHECK(s[0].points[0].y == doctest::Approx(50.f));
    CHECK(s[1].points[1].y == doctest::Approx(0.f));

    CHECK_FALSE(importCrowdStreets("id,kind,width_m,pts,pavement_m\nST-1,street,4,0:0;1:0,3\n", 100.0, s, &err));
}

TEST_CASE("crowd graph: shared points, a T-junction and a crossing all join; unwalked kinds and islands drop out") {
    std::vector<CrowdStreet> s;
    s.push_back(line({0.f, 0.f}, {100.f, 0.f}));             // main
    s.push_back(line({50.f, 2.5f}, {50.f, 60.f}));          // T: stops 2.5 m short of main's centre line
    s.push_back(line({80.f, -40.f}, {80.f, 40.f}));         // crosses main mid-segment
    s.push_back(line({0.f, 50.f}, {0.f, 90.f}, 6.f, "expressway")); // not walked
    s.push_back(line({300.f, 300.f}, {310.f, 300.f}));      // a 2-node island
    CrowdGraphTuning t;
    t.minComponent = 5;
    CrowdGraph g;
    REQUIRE(g.build(s, t));
    const std::uint32_t tFoot = nodeAt(g, {50.f, 0.f});
    const std::uint32_t tEnd = nodeAt(g, {50.f, 2.5f});
    const std::uint32_t cross = nodeAt(g, {80.f, 0.f});
    REQUIRE(tFoot != UINT32_MAX);
    REQUIRE(cross != UINT32_MAX);
    CHECK(g.find(tEnd, tFoot) != nullptr);
    CHECK(g.degree(cross) == 4);
    CHECK(g.degree(tFoot) == 3);
    CHECK(nodeAt(g, {0.f, 70.f}) == UINT32_MAX); // expressway absent
    const std::uint32_t island = nodeAt(g, {300.f, 300.f});
    REQUIRE(island != UINT32_MAX);
    CHECK_FALSE(g.spawnable(island));
    CHECK(g.spawnable(cross));
    CHECK(g.componentCount() == 2);
    // Edge lanes: no pavement → walk the middle, keep clear of the edge.
    const CrowdEdge* e = g.find(nodeAt(g, {0.f, 0.f}), tFoot);
    REQUIRE(e);
    CHECK(e->lane == 0.f);
    CHECK(e->laneHalf == doctest::Approx(3.f - std::max(t.edgeMargin, t.wallInset)));
}

TEST_CASE("crowd graph: two streets ending on the same spot of a third share one junction node") {
    std::vector<CrowdStreet> s;
    s.push_back(line({0.f, 0.f}, {40.f, 0.f}));
    s.push_back(line({20.f, 30.f}, {20.f, 2.5f}));   // from the north, stops short of the centre line
    s.push_back(line({20.f, -30.f}, {20.f, -2.5f})); // from the south, the same foot
    CrowdGraph g;
    REQUIRE(g.build(s, loose()));
    const std::uint32_t foot = nodeAt(g, {20.f, 0.f});
    REQUIRE(foot != UINT32_MAX);
    CHECK(g.degree(foot) == 4);
    CHECK(g.nodeCount() == 7);
}

TEST_CASE("crowd tuning: the shipped file loads; unknown or inconsistent members are refused") {
    CrowdTuning t;
    std::string err;
    const std::string text = readFile(std::string(FTCORE_PROJECT_DATA_DIR) + "/Crowd/crowd.json");
    REQUIRE(!text.empty());
    REQUIRE_MESSAGE(importCrowdTuning(text, t, &err), err);
    CHECK(t.targetAgents == 300);
    CHECK(t.step == doctest::Approx(1.f / 15.f));
    CHECK(t.events[static_cast<int>(CrowdEventKind::Death)].sightOnly);

    CrowdTuning u;
    CHECK_FALSE(importCrowdTuning(R"({"move": {"walkSpeeed": 1}})", u, &err));
    CHECK(err.find("walkSpeeed") != std::string::npos);
    CHECK_FALSE(importCrowdTuning(R"({"react": {"noticeFear": 0.5, "backFear": 0.3}})", u, &err));
    CHECK_FALSE(importCrowdTuning(R"({"events": {"explosion": {"radius": 3}}})", u, &err));
    CHECK(u.walkSpeed == CrowdTuning{}.walkSpeed); // left as it was
}

TEST_CASE("crowd perception: radius, delay, sight-only facing, felt range and occlusion") {
    CrowdGraph g;
    REQUIRE(g.build({line({-200.f, 0.f}, {200.f, 0.f})}, loose()));
    CrowdTuning t = quiet();
    t.walkSpeed = 0.01f;
    CrowdSim sim;
    REQUIRE(sim.init(g, t, 7));
    sim.setPopulationFrozen(true);
    const std::uint32_t a = sim.spawnAt(nodeAt(g, {-200.f, 0.f}), 0, 0.5f); // faces +X (towards 200)
    REQUIRE(a != UINT32_MAX);
    run(sim, 0.2f);
    const Vec2 p = sim.agent(a).pos;

    SUBCASE("out of range: nothing") {
        sim.pushEvent({CrowdEventKind::Strike, {p.x + 31.f, p.y}});
        run(sim, 2.f);
        CHECK(sim.counts().perceived == 0);
    }
    SUBCASE("in range: after base + distance delay") {
        sim.pushEvent({CrowdEventKind::Strike, {p.x + 20.f, p.y}}); // delay 0.2 + 0.2 = 0.4 s
        run(sim, 0.27f);
        CHECK(sim.counts().perceived == 0);
        run(sim, 0.27f);
        CHECK(sim.counts().perceived == 1);
        CHECK(sim.agent(a).fear > 0.1f);
    }
    SUBCASE("sight-only behind: unseen; in front or felt: seen") {
        sim.pushEvent({CrowdEventKind::Death, {p.x - 10.f, p.y}});
        run(sim, 1.f);
        CHECK(sim.counts().perceived == 0);
        sim.pushEvent({CrowdEventKind::Death, {p.x - 2.f, p.y}}); // within feelRadius
        run(sim, 1.f);
        CHECK(sim.counts().perceived == 1);
        sim.pushEvent({CrowdEventKind::Death, {sim.agent(a).pos.x + 10.f, p.y}});
        run(sim, 1.f);
        CHECK(sim.counts().perceived == 2);
    }
    SUBCASE("occluded: hearing softened, sight lost") {
        struct Wall {
            static bool blocked(void*, Vec2, Vec2) { return true; }
        };
        sim.setOcclusion(&Wall::blocked, nullptr);
        sim.pushEvent({CrowdEventKind::Strike, {p.x + 15.f, p.y}});
        run(sim, 1.f);
        CHECK(sim.counts().perceived == 1);
        CHECK(sim.counts().occlusionQueries == 1);
        CHECK(sim.agent(a).fear < t.events[1].intensity * 0.5f * t.occludedHearing + 0.01f);
        sim.pushEvent({CrowdEventKind::Death, {sim.agent(a).pos.x + 10.f, p.y}});
        run(sim, 1.f);
        CHECK(sim.counts().perceived == 1);
    }
}

TEST_CASE("crowd reactions: notice, back away, flee to a safe distance, calm down and resume") {
    CrowdGraph g;
    REQUIRE(g.build(gridStreets(9, 30.f), loose()));
    CrowdTuning t = quiet();
    CrowdSim sim;
    REQUIRE(sim.init(g, t, 11));
    sim.setPopulationFrozen(true);
    const std::uint32_t n = nodeAt(g, {0.f, 0.f});
    const std::uint32_t a = sim.spawnAt(n, 0, 0.5f);
    REQUIRE(a != UINT32_MAX);
    run(sim, 0.1f);

    SUBCASE("a small fear: notice, then walk on") {
        CrowdEvent e{CrowdEventKind::LoudNoise, {sim.agent(a).pos.x + 20.f, sim.agent(a).pos.y}};
        sim.pushEvent(e); // 0.35 * (1 - 20/40) = 0.175: notice only
        run(sim, 1.f);
        CHECK(sim.agent(a).state == CrowdState::Notice);
        CHECK(sim.agent(a).speed == 0.f);
        run(sim, 4.f);
        CHECK(sim.agent(a).state == CrowdState::Walk);
        CHECK(sim.counts().resumed == 1);
    }
    SUBCASE("a middling fear: back away, facing the threat") {
        const Vec2 at{sim.agent(a).pos.x + 5.f, sim.agent(a).pos.y};
        sim.pushEvent({CrowdEventKind::LoudNoise, at, 1.f, 1.2f}); // 0.35 * 0.875 * 1.2 = 0.37: between backFear 0.3 and fleeFear 0.55
        run(sim, 1.5f);
        CHECK(sim.agent(a).state == CrowdState::BackAway);
        CHECK(sim.agent(a).anim == CrowdAnim::BackStep);
        const float yawToThreat = std::atan2(at.y - sim.agent(a).pos.y, at.x - sim.agent(a).pos.x) * 57.29578f;
        CHECK(std::fabs(std::remainder(sim.agent(a).yaw - yawToThreat, 360.f)) < 20.f);
    }
    SUBCASE("a fight: flee along the streets to the safe distance, recover, resume") {
        const Vec2 fight{sim.agent(a).pos.x + 10.f, sim.agent(a).pos.y + 3.f};
        sim.pushEvent({CrowdEventKind::FightStart, fight});
        run(sim, 0.7f);
        CHECK(sim.agent(a).state == CrowdState::Flee);
        CHECK(sim.agent(a).anim == CrowdAnim::Run);
        float maxOff = 0.f;
        for (int i = 0; i < 15 * 40 && sim.agent(a).state == CrowdState::Flee; ++i) {
            sim.tick(1.f / 15.f, {0.f, 0.f}, {0.f, 0.f});
            // On a street the whole way: within the grid's street lines' half width.
            const Vec2 p = sim.agent(a).pos;
            const float gx = std::fabs(std::remainder(p.x, 30.f)), gy = std::fabs(std::remainder(p.y, 30.f));
            maxOff = std::max(maxOff, std::min(gx, gy));
        }
        CHECK(maxOff <= 4.f);
        CHECK(sim.agent(a).state == CrowdState::Recover);
        CHECK(dist(sim.agent(a).pos, fight) >= t.safeDist - 4.f);
        CHECK(sim.counts().fled == 1);
        run(sim, 30.f);
        CHECK(sim.agent(a).state != CrowdState::Flee);
        CHECK(sim.counts().resumed >= 1);
    }
    SUBCASE("safe but still afraid: no flip between recover and flee") {
        const Vec2 fight{sim.agent(a).pos.x + 10.f, sim.agent(a).pos.y + 3.f};
        sim.pushEvent({CrowdEventKind::FightStart, fight});
        for (int i = 0; i < 15 * 40 && sim.agent(a).state != CrowdState::Recover; ++i) sim.tick(1.f / 15.f, {0.f, 0.f}, {0.f, 0.f});
        REQUIRE(sim.agent(a).state == CrowdState::Recover);
        for (int k = 0; k < 20; ++k) { // loud enough to keep fear above flee at the safe distance
            sim.pushEvent({CrowdEventKind::Strike, fight, 4.f, 4.f});
            run(sim, 0.5f);
        }
        CHECK(sim.agent(a).fear >= t.fleeFear);
        CHECK(sim.counts().fled == 1);
        CHECK(sim.agent(a).state != CrowdState::Flee);
    }
}

TEST_CASE("crowd recover: wherever a run ends near the safe distance, a loud fight there never starts a second run") {
    CrowdGraph g;
    REQUIRE(g.build(gridStreets(9, 30.f), loose()));
    const CrowdTuning t = quiet();
    for (const float ox : {6.f, 10.f, 18.f, 25.f}) {
        for (const float oy : {-4.f, 0.f, 3.f, 8.f, 14.f}) {
            CAPTURE(ox);
            CAPTURE(oy);
            CrowdSim sim;
            REQUIRE(sim.init(g, t, 11));
            sim.setPopulationFrozen(true);
            const std::uint32_t a = sim.spawnAt(nodeAt(g, {0.f, 0.f}), 0, 0.5f);
            REQUIRE(a != UINT32_MAX);
            run(sim, 0.1f);
            const Vec2 fight{sim.agent(a).pos.x + ox, sim.agent(a).pos.y + oy};
            sim.pushEvent({CrowdEventKind::FightStart, fight});
            for (int k = 0; k < 40; ++k) { // a long, loud fight: fear stays above flee at the safe distance
                sim.pushEvent({CrowdEventKind::Strike, fight, 4.f, 4.f});
                run(sim, 0.5f);
            }
            CHECK(sim.counts().fleers == 1);
            CHECK(sim.counts().maxFleeEntries == 1);
            CHECK(sim.counts().maxPlans <= 4);
        }
    }
}

TEST_CASE("crowd flee: a crowd that runs from one fight doesn't end up standing inside itself, then walks on") {
    CrowdGraph g;
    REQUIRE(g.build(gridStreets(9, 30.f), loose()));
    CrowdTuning t = quiet();
    t.reactJitter = 0.25f;
    CrowdSim sim;
    REQUIRE(sim.init(g, t, 21));
    sim.setPopulationFrozen(true);
    // 40 members on the streets round the middle junction, all running from the same fight.
    std::vector<std::uint32_t> ids;
    for (const Vec2 at : {Vec2{0.f, 0.f}, Vec2{30.f, 0.f}, Vec2{0.f, 30.f}, Vec2{-30.f, 0.f}, Vec2{0.f, -30.f}}) {
        const std::uint32_t n = nodeAt(g, at);
        for (std::uint32_t k = 0; k < 8; ++k) {
            const std::uint32_t a = sim.spawnAt(n, k % g.degree(n), 0.5f);
            REQUIRE(a != UINT32_MAX);
            ids.push_back(a);
        }
    }
    run(sim, 0.5f);
    sim.pushEvent({CrowdEventKind::FightStart, {5.f, 5.f}});
    constexpr float kBody = 0.45f; // m: two members closer than this stand inside each other
    float closest = 1e9f;
    int closeSamples = 0;
    for (int s = 0; s < 120; ++s) { // 60 s, sampled every half second
        run(sim, 0.5f);
        if (s < 8) continue; // the first seconds: still on the move
        for (std::size_t x = 0; x < ids.size(); ++x) {
            const CrowdAgentView& p = sim.agent(ids[x]);
            if (!p.live || p.speed > 0.f) continue;
            for (std::size_t y = x + 1; y < ids.size(); ++y) {
                const CrowdAgentView& q = sim.agent(ids[y]);
                if (!q.live || q.speed > 0.f) continue;
                const float d = std::hypot(p.pos.x - q.pos.x, p.pos.y - q.pos.y);
                closest = std::min(closest, d);
                if (d < kBody) ++closeSamples;
            }
        }
    }
    CHECK(sim.counts().fleers >= 20);
    CAPTURE(closest);
    CHECK(closeSamples == 0);
    // Calm again: the runners walk on rather than stand where they stopped.
    int walking = 0;
    for (const std::uint32_t a : ids) walking += sim.agent(a).state == CrowdState::Walk && sim.agent(a).speed > 0.f;
    CHECK(walking * 4 >= static_cast<int>(ids.size()) * 3);
}

TEST_CASE("crowd flee path: picks the way away from the threat; cornered in a dead end, cowers") {
    SUBCASE("on a long street the run goes away from the threat") {
        CrowdGraph g;
        REQUIRE(g.build({line({-200.f, 0.f}, {-100.f, 0.f}), line({-100.f, 0.f}, {0.f, 0.f}), line({0.f, 0.f}, {100.f, 0.f}),
                         line({100.f, 0.f}, {200.f, 0.f})},
                        loose()));
        CrowdTuning t = quiet();
        CrowdSim sim;
        REQUIRE(sim.init(g, t, 3));
        sim.setPopulationFrozen(true);
        const std::uint32_t a = sim.spawnAt(nodeAt(g, {0.f, 0.f}), edgeTo(g, nodeAt(g, {0.f, 0.f}), nodeAt(g, {100.f, 0.f})), 0.5f);
        run(sim, 1.f); // walking +X, now at about x = 1.3
        sim.pushEvent({CrowdEventKind::Gunshot, {sim.agent(a).pos.x + 12.f, 0.f}});
        run(sim, 5.f);
        CHECK(sim.agent(a).state == CrowdState::Flee);
        CHECK(sim.agent(a).pos.x < -5.f); // turned round: the threat is to the east
    }
    SUBCASE("a dead end with the threat at its mouth") {
        CrowdGraph g;
        CrowdGraphTuning gt;
        gt.minComponent = 1;
        REQUIRE(g.build({line({0.f, 0.f}, {20.f, 0.f}), line({20.f, 0.f}, {200.f, 0.f})}, gt));
        CrowdTuning t = quiet();
        CrowdSim sim;
        REQUIRE(sim.init(g, t, 5));
        sim.setPopulationFrozen(true);
        const std::uint32_t end = nodeAt(g, {0.f, 0.f});
        const std::uint32_t a = sim.spawnAt(end, 0, 0.5f);
        sim.pushEvent({CrowdEventKind::Gunshot, {18.f, 0.f}});
        run(sim, 1.f);
        CHECK(sim.agent(a).state == CrowdState::Cower);
        CHECK(sim.agent(a).anim == CrowdAnim::Cower);
        CHECK(sim.counts().cowered == 1);
        CHECK(sim.counts().planFails >= 1);
        run(sim, 60.f);
        CHECK(sim.agent(a).state != CrowdState::Cower);
    }
}

TEST_CASE("crowd transitions: idle, escalation, back-away recovery, flee again, replan") {
    CrowdGraph g;
    REQUIRE(g.build(gridStreets(9, 30.f), loose()));
    CrowdTuning t = quiet();
    SUBCASE("a walker idles at a junction and walks on") {
        t.idleChance = 1.f;
        t.idleMin = t.idleMax = 2.f;
        CrowdSim sim;
        REQUIRE(sim.init(g, t, 21));
        sim.setPopulationFrozen(true);
        const std::uint32_t a = sim.spawnAt(nodeAt(g, {0.f, 0.f}), 0, 0.5f);
        const auto seen = trace(sim, a, 30.f); // 30 m at 1.3 m/s: past the next junction
        CHECK(inOrder(seen, {CrowdState::Walk, CrowdState::Idle, CrowdState::Walk}));
        CHECK(sim.counts().resumed == 0); // idling is not a reaction
    }
    SUBCASE("reactions") {
        CrowdSim sim;
        REQUIRE(sim.init(g, t, 11));
        sim.setPopulationFrozen(true);
        const std::uint32_t a = sim.spawnAt(nodeAt(g, {0.f, 0.f}), 0, 0.5f);
        run(sim, 0.1f);
        const Vec2 p = sim.agent(a).pos;
        SUBCASE("notice, then back away, then flee as the fear builds") {
            sim.pushEvent({CrowdEventKind::LoudNoise, {p.x + 20.f, p.y}}); // 0.175: notice
            auto seen = trace(sim, a, 1.f);
            sim.pushEvent({CrowdEventKind::LoudNoise, {p.x + 5.f, p.y}, 1.f, 0.6f}); // +0.18: about 0.35, back away
            auto more = trace(sim, a, 1.f);
            seen.insert(seen.end(), more.begin(), more.end());
            sim.pushEvent({CrowdEventKind::Strike, {p.x + 5.f, p.y}}); // +0.375: past fleeFear
            more = trace(sim, a, 1.5f);
            seen.insert(seen.end(), more.begin(), more.end());
            CHECK(inOrder(seen, {CrowdState::Notice, CrowdState::BackAway, CrowdState::Flee}));
        }
        SUBCASE("backing away calms into recover, then walk") {
            sim.pushEvent({CrowdEventKind::LoudNoise, {p.x + 5.f, p.y}, 1.f, 1.2f});
            const auto seen = trace(sim, a, 15.f);
            CHECK(inOrder(seen, {CrowdState::BackAway, CrowdState::Recover, CrowdState::Walk}));
            CHECK(sim.counts().backedAway == 1);
            CHECK(sim.counts().fled == 0);
        }
        SUBCASE("a new fight while recovering sends it running again") {
            sim.pushEvent({CrowdEventKind::FightStart, {p.x + 10.f, p.y + 3.f}});
            std::vector<CrowdState> seen{sim.agent(a).state};
            for (int i = 0; i < 15 * 40 && sim.agent(a).state != CrowdState::Recover; ++i) {
                sim.tick(1.f / 15.f, {0.f, 0.f}, {0.f, 0.f});
                if (sim.agent(a).state != seen.back()) seen.push_back(sim.agent(a).state);
            }
            REQUIRE(sim.agent(a).state == CrowdState::Recover);
            const Vec2 q = sim.agent(a).pos;
            sim.pushEvent({CrowdEventKind::FightStart, {q.x + 8.f, q.y}});
            const auto more = trace(sim, a, 2.f);
            seen.insert(seen.end(), more.begin(), more.end());
            CHECK(inOrder(seen, {CrowdState::Flee, CrowdState::Recover, CrowdState::Flee}));
            CHECK(sim.counts().fled == 2);
        }
        SUBCASE("a fleeing member replans when the threat moves") {
            sim.pushEvent({CrowdEventKind::FightStart, {p.x + 10.f, p.y + 3.f}});
            run(sim, 1.f);
            REQUIRE(sim.agent(a).state == CrowdState::Flee);
            const auto plans = sim.counts().plans;
            const Vec2 q = sim.agent(a).pos;
            sim.pushEvent({CrowdEventKind::FightStart, {q.x - 20.f, q.y - 20.f}}); // well over 15 m from the first threat
            run(sim, 1.f);
            CHECK(sim.counts().plans > plans);
            CHECK(sim.counts().planFails == 0);
        }
    }
}

TEST_CASE("crowd cower: recovers when calm; flees on a retry once a way opens") {
    CrowdGraph g;
    REQUIRE(g.build({line({0.f, 0.f}, {20.f, 0.f}), line({20.f, 0.f}, {200.f, 0.f})}, loose()));
    CrowdTuning t = quiet();
    CrowdSim sim;
    REQUIRE(sim.init(g, t, 5));
    sim.setPopulationFrozen(true);
    const std::uint32_t a = sim.spawnAt(nodeAt(g, {0.f, 0.f}), 0, 0.5f);
    sim.pushEvent({CrowdEventKind::Gunshot, {18.f, 0.f}});
    run(sim, 1.f);
    REQUIRE(sim.agent(a).state == CrowdState::Cower);
    SUBCASE("calm: cower, recover, walk") {
        const auto seen = trace(sim, a, 90.f);
        CHECK(inOrder(seen, {CrowdState::Cower, CrowdState::Recover, CrowdState::Walk}));
        CHECK(sim.counts().resumed == 1);
    }
    SUBCASE("the threat moves to the far end: the retry plans and it flees") {
        sim.pushEvent({CrowdEventKind::Gunshot, {195.f, 0.f}, 3.f, 1.f});
        const auto seen = trace(sim, a, 8.f); // a retry that fails before the shot is heard waits 4 s (the backoff)
        CHECK(inOrder(seen, {CrowdState::Cower, CrowdState::Flee}));
    }
}

TEST_CASE("crowd flee path: a threat close to a junction leaves streets open; never past the threat; partial runs") {
    SUBCASE("a threat 4 or 6 m off a junction, on each street") {
        CrowdGraph g;
        REQUIRE(g.build(gridStreets(9, 30.f), loose()));
        const Vec2 offsets[] = {{4.f, 0.f}, {6.f, 0.f}, {0.f, 4.f}, {-6.f, 0.f}, {0.f, -4.f}};
        for (const Vec2 off : offsets) {
            CAPTURE(off.x);
            CAPTURE(off.y);
            CrowdSim sim;
            REQUIRE(sim.init(g, quiet(), 13));
            sim.setPopulationFrozen(true);
            const std::uint32_t a = sim.spawnAt(nodeAt(g, {0.f, 0.f}), 0, 0.5f);
            const Vec2 threat{sim.agent(a).pos.x + off.x, sim.agent(a).pos.y + off.y};
            const float startD = dist(sim.agent(a).pos, threat);
            sim.pushEvent({CrowdEventKind::FightStart, threat});
            run(sim, 0.5f);
            REQUIRE(sim.agent(a).state == CrowdState::Flee);
            float closest = 1e9f;
            for (int i = 0; i < 15 * 40 && sim.agent(a).state == CrowdState::Flee; ++i) {
                sim.tick(1.f / 15.f, {0.f, 0.f}, {0.f, 0.f});
                closest = std::min(closest, dist(sim.agent(a).pos, threat));
            }
            CHECK(sim.counts().planFails == 0);
            CHECK(sim.counts().cowered == 0);
            CHECK(closest >= startD - 1.5f); // never towards the fight (a lane change is allowed)
            CHECK(dist(sim.agent(a).pos, threat) >= 50.f);
        }
    }
    SUBCASE("a threat ahead on its own street: the run never passes within blockRadius of it") {
        CrowdGraph g;
        REQUIRE(g.build(gridStreets(9, 30.f), loose()));
        CrowdTuning t = quiet();
        CrowdSim sim;
        REQUIRE(sim.init(g, t, 7));
        sim.setPopulationFrozen(true);
        const std::uint32_t n = nodeAt(g, {0.f, 0.f});
        const std::uint32_t a = sim.spawnAt(n, edgeTo(g, n, nodeAt(g, {30.f, 0.f})), 0.5f);
        const Vec2 threat{15.f, 0.f};
        sim.pushEvent({CrowdEventKind::Gunshot, threat});
        float closest = 1e9f;
        for (int i = 0; i < 15 * 40; ++i) {
            sim.tick(1.f / 15.f, {0.f, 0.f}, {0.f, 0.f});
            if (sim.agent(a).state == CrowdState::Flee) closest = std::min(closest, dist(sim.agent(a).pos, threat));
        }
        CHECK(sim.counts().fled == 1);
        CHECK(closest >= t.blockRadius);
    }
    SUBCASE("a street too short for the safe distance: a partial run to its end, then cower") {
        CrowdGraph g;
        CrowdStreet st = line({0.f, 0.f}, {50.f, 0.f});
        st.points = {{0.f, 0.f}, {10.f, 0.f}, {20.f, 0.f}, {30.f, 0.f}, {40.f, 0.f}, {50.f, 0.f}};
        REQUIRE(g.build({st}, loose()));
        CrowdSim sim;
        REQUIRE(sim.init(g, quiet(), 9));
        sim.setPopulationFrozen(true);
        const std::uint32_t n = nodeAt(g, {20.f, 0.f});
        const std::uint32_t a = sim.spawnAt(n, edgeTo(g, n, nodeAt(g, {10.f, 0.f})), 0.5f);
        sim.pushEvent({CrowdEventKind::Gunshot, {50.f, 0.f}});
        const auto seen = trace(sim, a, 12.f);
        CHECK(inOrder(seen, {CrowdState::Flee, CrowdState::Cower}));
        CHECK(sim.agent(a).pos.x < 3.f);
        CHECK(sim.counts().fled == 1);
    }
}

TEST_CASE("crowd walkers keep to the pavement, round the player too, and never jump at corners") {
    SUBCASE("a straight street, walkers both ways and a player walking up the pavement") {
        CrowdGraph g;
        CrowdStreet st = line({-150.f, 0.f}, {150.f, 0.f}, 8.f);
        st.pavement = 2.f;
        st.points.clear();
        for (int i = 0; i <= 30; ++i) st.points.push_back(Vec2{-150.f + 10.f * static_cast<float>(i), 0.f});
        REQUIRE(g.build({st}, loose()));
        CrowdTuning t = quiet();
        t.maxAgents = t.targetAgents = 120;
        CrowdSim sim;
        REQUIRE(sim.init(g, t, 31));
        sim.setPopulationFrozen(true);
        for (std::uint32_t n = 0; n < g.nodeCount(); ++n)
            for (std::uint32_t k = 0; k < g.degree(n); ++k) sim.spawnAt(n, k, 0.5f);
        REQUIRE(sim.liveCount() >= 50);
        // lane 3 (half 4 - pavement/2), laneHalf 0.8 (pavement/2 - 0.2): the pavement band is |y| in [2.2, 3.8].
        float inner = 1e9f, outer = 0.f;
        for (int i = 0; i < 15 * 60; ++i) {
            const Vec2 player{-60.f + 0.1f * static_cast<float>(i), 2.8f};
            sim.tick(1.f / 15.f, player, {1.5f, 0.f});
            for (std::uint32_t j = 0; j < sim.capacity(); ++j) {
                const CrowdAgentView& v = sim.agent(j);
                if (!v.live || v.state != CrowdState::Walk || std::fabs(v.pos.x) > 140.f) continue;
                inner = std::min(inner, std::fabs(v.pos.y));
                outer = std::max(outer, std::fabs(v.pos.y));
            }
        }
        CHECK(inner >= 2.15f);
        CHECK(outer <= 3.85f);
    }
    SUBCASE("a busy grid for two minutes: no walking step jumps") {
        CrowdGraph g;
        REQUIRE(g.build(gridStreets(15, 25.f), loose()));
        CrowdTuning t;
        CrowdSim sim;
        REQUIRE(sim.init(g, t, 41));
        const float most = (t.walkSpeed + t.walkSpeedJitter) * t.step + t.laneSpeed * t.step + 0.02f;
        float worst = 0.f;
        for (int i = 0; i < 15 * 120; ++i) {
            sim.tick(1.f / 15.f, {0.f, 0.f}, {0.f, 0.f});
            for (std::uint32_t j = 0; j < sim.capacity(); ++j) {
                const CrowdAgentView& v = sim.agent(j);
                if (v.live && v.state == CrowdState::Walk) worst = std::max(worst, dist(v.prevPos, v.pos));
            }
        }
        CHECK(worst <= most);
    }
}

TEST_CASE("crowd limits: a full pool refuses a spawn, a full event queue drops and counts, a bad dt is ignored") {
    CrowdGraph g;
    REQUIRE(g.build(gridStreets(5, 30.f), loose()));
    CrowdTuning t = quiet();
    t.maxAgents = t.targetAgents = 3;
    t.maxDrawn = 2;
    CrowdSim sim;
    REQUIRE(sim.init(g, t, 1));
    sim.setPopulationFrozen(true);
    const std::uint32_t n = nodeAt(g, {0.f, 0.f});
    CHECK(sim.spawnAt(n, 0, 0.5f) != UINT32_MAX);
    CHECK(sim.spawnAt(n, 1, 0.5f) != UINT32_MAX);
    CHECK(sim.spawnAt(n, 2, 0.5f) != UINT32_MAX);
    CHECK(sim.spawnAt(n, 3, 0.5f) == UINT32_MAX);
    CHECK(sim.liveCount() == 3);
    for (std::uint32_t i = 0; i < kCrowdMaxEvents + 6; ++i) sim.pushEvent({CrowdEventKind::LoudNoise, {200.f, 200.f}});
    CHECK(sim.counts().eventsDropped == 6);
    sim.tick(std::nanf(""), {0.f, 0.f}, {0.f, 0.f});
    sim.tick(-1.f, {0.f, 0.f}, {0.f, 0.f});
    CHECK(sim.steps() == 0);
    sim.tick(1.f / 15.f, {0.f, 0.f}, {0.f, 0.f});
    CHECK(sim.steps() == 1);
}

TEST_CASE("crowd determinism under uneven frame slicing") {
    CrowdGraph g;
    REQUIRE(g.build(gridStreets(15, 25.f), loose()));
    CrowdTuning t;
    t.targetAgents = 120;
    CrowdSim a, b;
    REQUIRE(a.init(g, t, 77));
    REQUIRE(b.init(g, t, 77));
    // a: whole steps; b: frames of 7, 23, 41 and 61 ms. The fight is pushed at the same step count in both.
    const float frames[] = {0.007f, 0.023f, 0.041f, 0.061f};
    std::uint32_t f = 0;
    bool pushed = false;
    while (b.steps() < 600) {
        if (!pushed && b.steps() == 150) b.pushEvent({CrowdEventKind::FightStart, {10.f, 10.f}}), pushed = true;
        b.tick(frames[f++ % 4], {0.f, 0.f}, {0.f, 0.f});
    }
    for (int i = 0; i < 600; ++i) {
        if (a.steps() == 150) a.pushEvent({CrowdEventKind::FightStart, {10.f, 10.f}});
        a.tick(1.f / 15.f, {0.f, 0.f}, {0.f, 0.f});
    }
    REQUIRE(a.steps() == b.steps());
    CHECK(a.digest() == b.digest());
    CHECK(a.counts().fled > 0);
}

TEST_CASE("crowd panic spreads to a neighbour out of earshot, weaker than the spreader's") {
    CrowdGraph g;
    CrowdStreet st = line({0.f, 0.f}, {300.f, 0.f}, 10.f);
    st.points.insert(st.points.begin() + 1, Vec2{5.f, 0.f});
    REQUIRE(g.build({st}, loose()));
    CrowdTuning t = quiet();
    t.walkSpeed = 0.01f;
    CrowdSim sim;
    REQUIRE(sim.init(g, t, 9));
    sim.setPopulationFrozen(true);
    const std::uint32_t a = sim.spawnAt(nodeAt(g, {0.f, 0.f}), 0, 0.f);
    const std::uint32_t b = sim.spawnAt(nodeAt(g, {5.f, 0.f}), 0, 0.f);
    REQUIRE(b != UINT32_MAX);
    // Radius 22 m: a (20 m) hears it, b (25 m) doesn't. a takes 4.5 * 2/22 = 0.41: flees (timid: 0.33).
    sim.pushEvent({CrowdEventKind::Strike, {-20.f, 0.f}, 22.f / 30.f, 10.f});
    run(sim, 2.f);
    CHECK(sim.counts().perceived == 1);
    CHECK(sim.agent(a).state == CrowdState::Flee);
    CHECK(sim.counts().spread >= 1);
    CHECK(sim.agent(b).fear > 0.1f);
    CHECK(sim.agent(b).fear < sim.agent(a).fear);
    CHECK((sim.agent(b).state == CrowdState::BackAway || sim.agent(b).state == CrowdState::Notice));
}

TEST_CASE("crowd personality: the same blow sends a timid member running and a bold one only staring") {
    CrowdGraph g;
    REQUIRE(g.build(gridStreets(7, 30.f), loose()));
    CrowdTuning t = quiet();
    t.walkSpeed = 0.01f;
    t.spreadGain = 0.f;
    CrowdSim sim;
    REQUIRE(sim.init(g, t, 13));
    sim.setPopulationFrozen(true);
    const std::uint32_t n = nodeAt(g, {0.f, 0.f});
    const std::uint32_t timid = sim.spawnAt(n, 0, 0.f);
    const std::uint32_t bold = sim.spawnAt(n, 0, 1.f);
    run(sim, 0.1f);
    sim.pushEvent({CrowdEventKind::Strike, {sim.agent(timid).pos.x + 7.f, sim.agent(timid).pos.y}}); // 0.45 * 23/30 = 0.345
    run(sim, 1.f);
    CHECK(sim.agent(timid).state == CrowdState::Flee);  // flee at 0.55 * 0.6 = 0.33
    CHECK(sim.agent(bold).state == CrowdState::Notice); // notice 0.19, back 0.48
}

TEST_CASE("crowd population: pool limit, spawn ring, despawn, drawn set") {
    CrowdGraph g;
    REQUIRE(g.build(gridStreets(25, 20.f), loose())); // 480 m across
    CrowdTuning t;
    t.maxAgents = 40;
    t.targetAgents = 40;
    t.maxDrawn = 10;
    CrowdSim sim;
    REQUIRE(sim.init(g, t, 17));
    run(sim, 3.f);
    CHECK(sim.liveCount() == 40);
    CHECK(sim.capacity() == 40);
    std::uint32_t drawn = 0;
    float farDrawn = 0.f, nearUndrawn = 1e9f;
    for (std::uint32_t i = 0; i < sim.capacity(); ++i) {
        const CrowdAgentView& v = sim.agent(i);
        if (!v.live) continue;
        const float d = dist(v.pos, {0.f, 0.f});
        CHECK(d <= t.despawnDist);
        if (v.drawn) ++drawn, farDrawn = std::max(farDrawn, d);
        else nearUndrawn = std::min(nearUndrawn, d);
    }
    CHECK(drawn == 10);
    CHECK(farDrawn <= nearUndrawn + 1.f);
    // The player walks away: the old crowd despawns, a new one fills in around them.
    const auto despawned = sim.counts().despawned;
    run(sim, 2.f, {230.f, 230.f});
    CHECK(sim.counts().despawned > despawned);
    for (std::uint32_t i = 0; i < sim.capacity(); ++i)
        if (sim.agent(i).live) CHECK(dist(sim.agent(i).pos, {230.f, 230.f}) <= t.despawnDist);
    CHECK(sim.liveCount() <= 40);
}

TEST_CASE("crowd determinism: same seed and events, same crowd, whatever the frame slicing") {
    CrowdGraph g;
    REQUIRE(g.build(gridStreets(15, 25.f), loose()));
    CrowdTuning t;
    t.targetAgents = 120;
    CrowdSim a, b, c;
    REQUIRE(a.init(g, t, 99));
    REQUIRE(b.init(g, t, 99));
    REQUIRE(c.init(g, t, 100));
    for (int i = 0; i < 300; ++i) {
        if (i == 100) {
            a.pushEvent({CrowdEventKind::FightStart, {10.f, 10.f}, 3.f, 1.5f}); // reaches the spawn ring
            b.pushEvent({CrowdEventKind::FightStart, {10.f, 10.f}, 3.f, 1.5f}); // reaches the spawn ring
            c.pushEvent({CrowdEventKind::FightStart, {10.f, 10.f}, 3.f, 1.5f}); // reaches the spawn ring
        }
        a.tick(1.f / 15.f, {0.f, 0.f}, {0.f, 0.f});
        b.tick(1.f / 30.f, {0.f, 0.f}, {0.f, 0.f});
        b.tick(1.f / 30.f, {0.f, 0.f}, {0.f, 0.f});
        c.tick(1.f / 15.f, {0.f, 0.f}, {0.f, 0.f});
    }
    CHECK(a.steps() == b.steps());
    CHECK(a.digest() == b.digest());
    CHECK(a.digest() != c.digest());
    CHECK(a.counts().fled > 0);
}

TEST_CASE("crowd tick allocates nothing and meets its budget at the target count") {
    CrowdGraph g;
    REQUIRE(g.build(gridStreets(40, 15.f), loose())); // 585 m across, 1600 nodes
    CrowdTuning t; // 300 target, 512 pool
    CrowdSim sim;
    REQUIRE(sim.init(g, t, 1));
    run(sim, 5.f); // fill
    CHECK(sim.liveCount() == t.targetAgents);
    struct Open {
        static bool blocked(void*, Vec2 a, Vec2 b) { return std::fabs(a.x - b.x) > 40.f; }
    };
    sim.setOcclusion(&Open::blocked, nullptr);
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kSteps = 900; // 60 s
    for (int i = 0; i < kSteps; ++i) {
        if (i % 30 == 0) sim.pushEvent({CrowdEventKind::Strike, {static_cast<float>(i % 90) - 45.f, 0.f}});
        if (i == 300) sim.pushEvent({CrowdEventKind::FightStart, {0.f, 0.f}});
        if (i == 450) sim.pushEvent({CrowdEventKind::Gunshot, {20.f, -20.f}});
        const float px = std::sin(static_cast<float>(i) * 0.01f) * 30.f;
        sim.tick(1.f / 15.f, {px, 0.f}, {1.f, 0.f});
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kSteps;
    const auto after = ftcore_test::allocStats().count;
    if (counting) CHECK(after == before);
    std::printf("crowd perf: %u live, %.3f ms per step (fled %llu, cowered %llu, plans %llu)\n", sim.liveCount(), ms,
                static_cast<unsigned long long>(sim.counts().fled), static_cast<unsigned long long>(sim.counts().cowered),
                static_cast<unsigned long long>(sim.counts().plans));
    CHECK(sim.counts().fled > 0);
#ifdef NDEBUG
    CHECK(ms < 1.0);
#else
    CHECK(ms < 20.0); // Debug builds: a loose bound only
#endif
}

// Opt-in: FT_CROWD_STREETS=<streets csv> (FT_CROWD_SIDE, default 1000) builds the graph from a real street table
// (kept outside the repo) and runs a minute with a fight in the middle. Skipped without it.
TEST_CASE("crowd r3: wall inset, first-second reaction, no clumps, no stuck runners") {
    CrowdGraph g;
    CrowdGraphTuning gt = loose();
    REQUIRE(g.build(gridStreets(9, 30.f), gt)); // 8 m wide, 2 m pavements: building fronts 4 m off the centre line
    // Every lane stays wallInset clear of the building fronts.
    for (std::uint32_t n = 0; n < g.nodeCount(); ++n)
        for (std::uint32_t k = 0; k < g.degree(n); ++k) {
            const CrowdEdge& e = g.edge(n, k);
            CHECK(e.lane + e.laneHalf <= 4.f - gt.wallInset + 1e-4f);
        }
    CrowdTuning t;
    t.targetAgents = t.maxAgents = 120;
    t.despawnDist = 400.f;
    CrowdSim sim;
    REQUIRE(sim.init(g, t, 33));
    // Off a building front: no farther than 4 m - wallInset from the nearest street centre line (grid lines 30 m apart).
    auto offFronts = [&](Vec2 p) {
        const float dx = std::fabs(p.x - 30.f * std::round(p.x / 30.f)), dy = std::fabs(p.y - 30.f * std::round(p.y / 30.f));
        return std::min(dx, dy) <= 4.f - gt.wallInset + 0.05f;
    };
    std::uint32_t inFront = 0;
    auto sweep = [&] {
        for (std::uint32_t i = 0; i < sim.capacity(); ++i)
            if (sim.agent(i).live && !offFronts(sim.agent(i).pos)) ++inFront;
    };
    // Calm fill: right after the spawn and while walking, nobody stands inside anybody.
    sim.tick(1.f / 15.f, {0.f, 0.f}, {0.f, 0.f});
    CHECK(sim.closePairs(0.45f) == 0);
    std::uint32_t calmPairs = 0;
    for (int s = 0; s < 20; ++s) {
        run(sim, 0.5f);
        calmPairs += sim.closePairs(0.45f);
        sweep();
    }
    CHECK(calmPairs == 0);
    // A fight next to the busiest spot: within reactRadius, everyone has turned away and gained 1.5 m by t = 1 s.
    Vec2 fight{0.f, 0.f};
    std::uint32_t best = 0;
    for (std::uint32_t i = 0; i < sim.capacity(); ++i) {
        if (!sim.agent(i).live) continue;
        std::uint32_t near = 0;
        for (std::uint32_t j = 0; j < sim.capacity(); ++j) near += sim.agent(j).live && dist(sim.agent(i).pos, sim.agent(j).pos) < 12.f;
        if (near > best) best = near, fight = sim.agent(i).pos;
    }
    CrowdReactionProbe probe;
    probe.begin(sim, fight, t.reactRadius);
    REQUIRE(probe.count() >= 5);
    sim.pushEvent({CrowdEventKind::FightStart, fight});
    run(sim, 1.f);
    const CrowdReactionResult r = probe.evaluate(sim, 1.5f);
    CAPTURE(r.inside);
    CAPTURE(r.away);
    CAPTURE(r.turned);
    CAPTURE(r.toward);
    CAPTURE(r.worstGain);
    CHECK(r.ok());
    // The run and after: nobody stuck, no members standing in a run pose, nobody in a building front.
    std::uint32_t runStill = 0;
    for (int s = 0; s < 120; ++s) {
        run(sim, 0.25f);
        sweep();
        for (std::uint32_t i = 0; i < sim.capacity(); ++i) {
            const CrowdAgentView& v = sim.agent(i);
            if (v.live && v.anim == CrowdAnim::Run && v.speed <= 0.05f) ++runStill;
        }
    }
    CHECK(runStill == 0);
    CHECK(sim.counts().stuck == 0);
    CHECK(inFront == 0);
}

TEST_CASE("crowd on a real street table (opt-in)") {
    const std::string path = envText("FT_CROWD_STREETS");
    if (path.empty()) return;
    const std::string sideText = envText("FT_CROWD_SIDE");
    const double side = sideText.empty() ? 1000.0 : std::atof(sideText.c_str());
    std::vector<CrowdStreet> streets;
    std::string err;
    REQUIRE_MESSAGE(importCrowdStreets(readFile(path), side, streets, &err), err);
    CrowdTuning t;
    REQUIRE(importCrowdTuning(readFile(std::string(FTCORE_PROJECT_DATA_DIR) + "/Crowd/crowd.json"), t, &err));
    CrowdGraph g;
    REQUIRE_MESSAGE(g.build(streets, t.graph, &err), err);
    std::uint32_t n0 = 0;
    for (std::uint32_t n = 0; n < g.nodeCount(); ++n) n0 += g.degree(n) == 0;
    std::printf("crowd real: %zu streets, %u nodes (%u unconnected), %u edges, %u components, %u spawnable\n",
                streets.size(), g.nodeCount(), n0, g.edgeCount() / 2, g.componentCount(), g.spawnableCount());
    // The player stands on the most connected node near the middle.
    const Vec2 mid{(g.boundsMin().x + g.boundsMax().x) * 0.5f, (g.boundsMin().y + g.boundsMax().y) * 0.5f};
    std::uint32_t start = g.nearest(mid, 200.f);
    REQUIRE(start != UINT32_MAX);
    Vec2 player = g.node(start);
    // The fight: FT_CROWD_AT="x,y" (m, engine frame), with the player on the nearest node; else the player's node.
    Vec2 fight = player;
    const std::string at = envText("FT_CROWD_AT");
    if (!at.empty()) {
        fight.x = std::strtof(at.c_str(), nullptr);
        const auto comma = at.find(',');
        if (comma != std::string::npos) fight.y = std::strtof(at.c_str() + comma + 1, nullptr);
        const std::uint32_t pn = g.nearest(fight, 50.f);
        if (pn != UINT32_MAX) player = g.node(pn);
    }
    // Pairs closer than a body: both standing / one standing / both moving.
    auto pairKinds = [](const CrowdSim& s, std::uint32_t out[3]) {
        out[0] = out[1] = out[2] = 0;
        for (std::uint32_t i = 0; i < s.capacity(); ++i)
            for (std::uint32_t j = i + 1; j < s.capacity(); ++j) {
                const CrowdAgentView &p = s.agent(i), &q = s.agent(j);
                if (!p.live || !q.live || dist(p.pos, q.pos) >= 0.45f) continue;
                ++out[(p.speed > 0.f) + (q.speed > 0.f)];
            }
    };
    const std::string seedText = envText("FT_CROWD_SEED");
    CrowdSim sim;
    REQUIRE(sim.init(g, t, seedText.empty() ? 1u : static_cast<std::uint32_t>(std::atoi(seedText.c_str()))));
    run(sim, 20.f, player);
    const std::uint32_t before = sim.liveCount();
    std::uint32_t calm[3];
    pairKinds(sim, calm);
    CrowdReactionProbe probe;
    probe.begin(sim, fight, t.reactRadius);
    sim.pushEvent({CrowdEventKind::FightStart, fight});
    run(sim, 1.f, player);
    const CrowdReactionResult r = probe.evaluate(sim, 1.5f);
    std::printf("crowd real: calm close pairs standing %u mixed %u walking %u; first second: inside %u away %u turned %u toward %u worst gain %.2f m: %s\n",
                calm[0], calm[1], calm[2], r.inside, r.away, r.turned, r.toward, static_cast<double>(r.worstGain), r.ok() ? "ok" : "FAILED");
    std::uint32_t runStill = 0, pairs[3] = {0, 0, 0};
    for (int i = 0; i < 38; ++i) {
        if (i % 2 == 0) sim.pushEvent({CrowdEventKind::Strike, fight});
        run(sim, 0.25f, player);
        std::uint32_t k3[3];
        pairKinds(sim, k3);
        for (int k = 0; k < 3; ++k) pairs[k] += k3[k];
        for (std::uint32_t k = 0; k < sim.capacity(); ++k)
            runStill += sim.agent(k).live && sim.agent(k).anim == CrowdAnim::Run && sim.agent(k).speed <= 0.05f;
    }
    std::printf("crowd real: during the fight close-pair samples standing %u mixed %u moving %u, run pose at zero speed %u, stuck %llu\n", pairs[0], pairs[1], pairs[2], runStill,
                static_cast<unsigned long long>(sim.counts().stuck));
    const std::uint32_t fleeing = sim.countIn(CrowdState::Flee), cowering = sim.countIn(CrowdState::Cower);
    run(sim, 60.f, player);
    const CrowdCounts& c = sim.counts();
    std::printf("crowd real: live %u before, flee %u cower %u during; perceived %llu fled %llu cowered %llu resumed %llu "
                "spread %llu plans %llu fails %llu; live %u after, walking %u\n",
                before, fleeing, cowering, static_cast<unsigned long long>(c.perceived), static_cast<unsigned long long>(c.fled),
                static_cast<unsigned long long>(c.cowered), static_cast<unsigned long long>(c.resumed),
                static_cast<unsigned long long>(c.spread), static_cast<unsigned long long>(c.plans),
                static_cast<unsigned long long>(c.planFails), sim.liveCount(), sim.countIn(CrowdState::Walk));
    CHECK(c.fled > 0);
}
