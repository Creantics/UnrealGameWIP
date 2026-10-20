#include "ftcore/Explore.h"
#include "ftcore/ExploreDemo.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace ftcore;

namespace {
Interactable spot(InteractKind kind, float x, float y, float radius, std::uint8_t objective, std::uint32_t needs = 0) {
    Interactable it;
    it.kind = kind;
    it.pos = {x, y};
    it.radius = radius;
    it.objective = objective;
    it.needs = needs;
    return it;
}
} // namespace

TEST_CASE("focus picks the nearest usable thing in range and in front") {
    Exploration e;
    const int obj = e.addObjective(2);
    const int closest = e.add(spot(InteractKind::Pickup, 1.f, 0.f, 2.f, static_cast<std::uint8_t>(obj)));
    const int further = e.add(spot(InteractKind::Pickup, 1.8f, 0.2f, 2.f, static_cast<std::uint8_t>(obj)));
    const int behind = e.add(spot(InteractKind::Pickup, -0.5f, 0.f, 2.f, static_cast<std::uint8_t>(obj)));
    e.add(spot(InteractKind::Encounter, 0.5f, 0.f, 2.f, Exploration::kNoObjective));

    CHECK(e.focus({0.f, 0.f}, {1.f, 0.f}) == closest);
    CHECK(e.focus({0.f, 0.f}, {-1.f, 0.f}) == behind);
    // No facing: the cone is skipped, so the nearest wins whatever the direction.
    CHECK(e.focus({0.f, 0.f}, {0.f, 0.f}) == behind);
    // Out of range of everything.
    CHECK(e.focus({10.f, 0.f}, {1.f, 0.f}) == -1);

    CHECK(e.interact(closest) == InteractResult::Used);
    CHECK(e.focus({0.f, 0.f}, {1.f, 0.f}) == further);
    CHECK(e.interact(closest) == InteractResult::Gone);
}

TEST_CASE("locked things can be focused but not used until their objectives are done") {
    Exploration e;
    const int talk = e.addObjective(1);
    const int gate = e.addObjective(1);
    const int npc = e.add(spot(InteractKind::Talk, 0.f, 3.f, 2.f, static_cast<std::uint8_t>(talk)));
    const int door = e.add(spot(InteractKind::Gate, 1.f, 0.f, 2.f, static_cast<std::uint8_t>(gate), 1u << talk));

    CHECK(e.focus({0.f, 0.f}, {1.f, 0.f}) == door);
    CHECK_FALSE(e.isAvailable(door));
    CHECK(e.interact(door) == InteractResult::Locked);
    CHECK(e.currentObjective() == talk);

    CHECK(e.interact(npc) == InteractResult::Used);
    CHECK(e.objectiveDone(talk));
    CHECK(e.currentObjective() == gate);
    CHECK(e.interact(door) == InteractResult::Used);
    CHECK(e.allDone());
    CHECK(e.currentObjective() == -1);
}

TEST_CASE("encounters trigger by position, stay until cleared and are never focused") {
    Exploration e;
    const int win = e.addObjective(1);
    const int open = e.addObjective(1);
    const int lever = e.add(spot(InteractKind::Pickup, 0.f, -5.f, 1.f, static_cast<std::uint8_t>(open)));
    const int ambush = e.add(spot(InteractKind::Encounter, 5.f, 0.f, 2.f, static_cast<std::uint8_t>(win), 1u << open));

    CHECK(e.encounterAt({5.f, 0.f}) == -1); // locked behind the lever
    CHECK(e.interact(lever) == InteractResult::Used);
    CHECK(e.encounterAt({5.5f, 0.5f}) == ambush);
    CHECK(e.encounterAt({8.f, 0.f}) == -1);
    CHECK(e.focus({5.f, 0.f}, {0.f, 0.f}) == -1);
    CHECK(e.interact(ambush) == InteractResult::Gone);

    // A lost fight leaves it in place; a won one clears it and counts.
    CHECK(e.encounterAt({5.f, 0.f}) == ambush);
    e.clearEncounter(ambush);
    CHECK(e.encounterAt({5.f, 0.f}) == -1);
    CHECK(e.objectiveDone(win));
}

TEST_CASE("save and load round-trip, and load clamps a bad save") {
    Exploration e = demo::makeExploration();
    CHECK(e.interact(0) == InteractResult::Used);
    CHECK(e.interact(2) == InteractResult::Used);
    const ExploreState s = e.save();

    Exploration f = demo::makeExploration();
    f.load(s);
    CHECK(f.isUsed(0));
    CHECK(f.isUsed(2));
    CHECK_FALSE(f.isUsed(1));
    CHECK(f.progress(demo::kObjTalk) == 1);
    CHECK(f.progress(demo::kObjShards) == 1);
    CHECK(f.currentObjective() == demo::kObjShards);

    ExploreState bad;
    bad.used = 0xFFFFFFFFu;
    bad.progress.fill(200);
    Exploration g = demo::makeExploration();
    g.load(bad);
    CHECK(g.progress(demo::kObjShards) == 3);
    CHECK(g.save().used == (1u << demo::kExploreSpots.size()) - 1u);
}

TEST_CASE("capacity limits refuse instead of overflowing") {
    Exploration e;
    for (std::size_t i = 0; i < Exploration::kMaxObjectives; ++i) CHECK(e.addObjective(1) == static_cast<int>(i));
    CHECK(e.addObjective(1) == -1);
    for (std::size_t i = 0; i < Exploration::kMaxInteractables; ++i)
        CHECK(e.add(spot(InteractKind::Pickup, static_cast<float>(i), 0.f, 1.f, 0)) == static_cast<int>(i));
    CHECK(e.add(spot(InteractKind::Pickup, 0.f, 0.f, 1.f, 0)) == -1);
}

TEST_CASE("the demo pilot walks the whole exploration demo in order without allocating") {
    Exploration e = demo::makeExploration();
    Vec2 pos = demo::kExploreStart;
    Vec2 facing{1.f, 0.f};
    int used[8] = {};
    int usedCount = 0;
    float seconds = 0.f;
    const float dt = 1.f / 30.f;
    const std::uint64_t before = ftcore_test::allocStats().count;
    while (!e.allDone() && seconds < 300.f) {
        const int hit = demo::pilotStep(e, pos, facing, 4.f, dt);
        if (hit >= 0) {
            if (e.item(hit).kind == InteractKind::Encounter) e.clearEncounter(hit);
            if (usedCount < 8) used[usedCount++] = hit;
        }
        seconds += dt;
    }
    if (ftcore_test::allocHookInstalled()) CHECK(ftcore_test::allocStats().count == before);
    CHECK(e.allDone());
    REQUIRE(usedCount == 6);
    // Duy, then the shards nearest-first from where the pilot stands, the gate and the ambush.
    CHECK(used[0] == 0);
    CHECK(used[1] == 1);
    CHECK(used[2] == 2);
    CHECK(used[3] == 3);
    CHECK(used[4] == 4);
    CHECK(used[5] == 5);
    CHECK(seconds < 60.f);
}

TEST_CASE("field goals: deterministic choice, spread, and every goal reachable fast without a prop in the way") {
    CHECK(demo::fieldGoalIndex("sceneA", 2, 0) == demo::fieldGoalIndex("sceneA", 2, 0));
    CHECK(demo::fieldGoalIndex("sceneA", 2, 0) < demo::kFieldGoals.size());
    bool seen[demo::kFieldGoals.size()] = {};
    for (int beat = 0; beat < 40; ++beat)
        for (int step = 0; step < 3; ++step) seen[demo::fieldGoalIndex("sceneB", beat, step)] = true;
    for (const bool s : seen) CHECK(s);

    const float dt = 1.f / 30.f;
    const std::uint64_t before = ftcore_test::allocStats().count;
    for (const Vec2 goal : demo::kFieldGoals) {
        Vec2 pos = demo::kExploreStart;
        Vec2 facing{1.f, 0.f};
        float seconds = 0.f;
        float best = 1e9f;
        float lastProgress = 0.f;
        bool done = false;
        while (seconds < 30.f) {
            if (demo::fieldPilotStep(pos, facing, goal, 3.3f, dt)) {
                done = true;
                break;
            }
            seconds += dt;
            const float d = std::sqrt((goal.x - pos.x) * (goal.x - pos.x) + (goal.y - pos.y) * (goal.y - pos.y));
            if (d < best - 0.1f) {
                best = d;
                lastProgress = seconds;
            }
            CHECK(seconds - lastProgress < 2.f); // never stalls
        }
        CHECK(done);
        CHECK(seconds < 10.f);
        CHECK(demo::fieldStepUsable(demo::fieldGoalInReach(pos, facing, goal), seconds));
        for (const demo::FieldObstacle& o : demo::kFieldObstacles) {
            // The straight line clears every prop, and the marker isn't inside one.
            CHECK(demo::segmentDistance(demo::kExploreStart, goal, o.pos) > o.radius + 0.4f);
            const float dx = o.pos.x - goal.x;
            const float dy = o.pos.y - goal.y;
            CHECK(std::sqrt(dx * dx + dy * dy) > o.radius + 0.4f);
        }
    }
    if (ftcore_test::allocHookInstalled()) CHECK(ftcore_test::allocStats().count == before);
}

TEST_CASE("field step: use needs the marker in reach and facing, or the fallback time") {
    const Vec2 goal = demo::kFieldGoals[0];
    const Vec2 near{goal.x - 1.f, goal.y};
    CHECK(demo::fieldGoalInReach(near, {1.f, 0.f}, goal));
    CHECK(demo::fieldGoalInReach(near, {0.f, 0.f}, goal));
    CHECK_FALSE(demo::fieldGoalInReach(near, {-1.f, 0.f}, goal));
    CHECK_FALSE(demo::fieldGoalInReach({goal.x - 5.f, goal.y}, {1.f, 0.f}, goal));
    CHECK_FALSE(demo::fieldStepUsable(false, 10.f));
    CHECK_FALSE(demo::fieldStepUsable(false, demo::kFieldFallbackSeconds - 1.f));
    CHECK(demo::fieldStepUsable(false, demo::kFieldFallbackSeconds));
    CHECK(demo::fieldStepUsable(true, 0.f));
}
