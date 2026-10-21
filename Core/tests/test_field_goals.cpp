// The per-step tables: Data/Field/goals.json (loader and walk checks) and, when the real script is
// there, that the QTE, cutscene and field tables name every such step of the story and nothing else.
// Ids and counts only; no story text is read or printed.

#include "ftcore/Cutscene.h"
#include "ftcore/Dialogue.h"
#include "ftcore/ExploreDemo.h"
#include "ftcore/FieldGoals.h"
#include "ftcore/GameplayPlan.h"
#include "ftcore/Qte.h"
#include "ftcore/StoryRunner.h"

#include "RealStory.h"
#include "TestAssets.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cmath>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace ftcore;

namespace {

using Key = std::tuple<std::string, std::uint32_t, std::uint32_t>;

constexpr const char* kGoals = R"J({
  "version": 1,
  "default": { "goal": { "x": 6, "y": 8 } },
  "goals": [
    { "scene": "SC-T1", "beat": 2, "goal": { "x": 10, "y": -8 } },
    { "scene": "SC-T1", "beat": 2, "step": 1, "goal": { "x": 12.5, "y": 4 }, "via": { "x": 5, "y": 2 } }
  ]
})J";

// The (scene, beat, step) of every step of the real script whose own screen is `screen`.
std::set<Key> storySteps(const StoryDatabase& db, GameplayScreen screen) {
    std::set<Key> keys;
    StoryRunner r(&db);
    DialoguePresenter p(&r);
    p.setRevealRate(0.f);
    GameplayPlan plan;
    REQUIRE(r.start());
    int moves = 0;
    while (!r.finished() && moves < 200000) {
        if (r.waitingForGameplay()) {
            plan.lower(r);
            for (std::uint32_t i = 0; i < plan.size(); ++i)
                if (plan.step(i).screen == screen)
                    keys.emplace(std::string(r.position().scene), r.position().beat, i);
            if (!r.resume()) break;
            ++moves;
            continue;
        }
        p.update(1.f / 60.f);
        const DialogueCommand c = p.autoplay();
        if (c.none() || !applyDialogueCommand(r, c)) break;
        ++moves;
    }
    return keys;
}

bool loadTable(const char* rel, std::string& text) {
    if (!ftcore_test::readProjectData(rel, text)) {
        MESSAGE(rel << " not readable; skipping");
        return false;
    }
    return true;
}

} // namespace

TEST_CASE("field goals: the loader reads goals, waypoints and the default") {
    FieldGoalTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importFieldGoalTable(kGoals, t, &error, &warnings), error);
    CHECK(warnings.empty());
    CHECK(t.hasFallback);
    CHECK(t.fallback.goal.x == doctest::Approx(6.f));
    REQUIRE(t.entries.size() == 2);
    const FieldGoalEntry* a = t.findEntry("SC-T1", 2, 0);
    REQUIRE(a);
    CHECK(a->goal.y == doctest::Approx(-8.f));
    CHECK_FALSE(a->hasVia);
    const FieldGoalEntry* b = t.findEntry("SC-T1", 2, 1);
    REQUIRE(b);
    CHECK(b->hasVia);
    CHECK(b->via.x == doctest::Approx(5.f));
    CHECK(t.findEntry("SC-T1", 3, 0) == nullptr);
    CHECK(t.findEntry("SC-T2", 2, 0) == nullptr);
}

TEST_CASE("field goals: the loader refuses bad files and warns on unknown keys") {
    FieldGoalTable t;
    std::string error;
    std::vector<std::string> warnings;
    CHECK_FALSE(importFieldGoalTable("{", t, &error));
    CHECK_FALSE(importFieldGoalTable(R"({"version": 2})", t, &error));
    // out of the walkable box, missing goal, duplicate, bad beat, bad waypoint
    CHECK_FALSE(importFieldGoalTable(R"({"goals":[{"scene":"A","beat":1,"goal":{"x":40,"y":0}}]})", t, &error));
    CHECK(t.entries.empty());
    CHECK_FALSE(importFieldGoalTable(R"({"goals":[{"scene":"A","beat":1,"goal":{"x":5,"y":30}}]})", t, &error));
    CHECK_FALSE(importFieldGoalTable(R"({"goals":[{"scene":"A","beat":1}]})", t, &error));
    CHECK_FALSE(importFieldGoalTable(R"({"goals":[{"scene":"A","beat":-1,"goal":{"x":5,"y":0}}]})", t, &error));
    CHECK_FALSE(importFieldGoalTable(R"({"goals":[{"scene":"A","beat":1,"goal":{"x":5,"y":0},"via":{"x":5}}]})", t, &error));
    CHECK_FALSE(importFieldGoalTable(
        R"({"goals":[{"scene":"A","beat":1,"goal":{"x":5,"y":0}},{"scene":"A","beat":1,"step":0,"goal":{"x":6,"y":0}}]})", t,
        &error));
    CHECK(error.find("duplicate") != std::string::npos);
    REQUIRE(importFieldGoalTable(R"({"extra":1,"goals":[{"scene":"A","beat":1,"goal":{"x":5,"y":0,"z":1}}]})", t, &error,
                                 &warnings));
    CHECK(warnings.size() == 2);
    CHECK_FALSE(t.hasFallback);
}

TEST_CASE("field goals: every committed goal is on clear ground and the pilot reaches it in under 10 s") {
    std::string text;
    if (!loadTable("Field/goals.json", text)) return;
    FieldGoalTable t;
    std::string error;
    std::vector<std::string> warnings;
    REQUIRE_MESSAGE(importFieldGoalTable(text, t, &error, &warnings), error);
    CHECK(warnings.empty());
    CHECK(t.hasFallback);
    CHECK_FALSE(t.entries.empty());
    const float dt = 1.f / 30.f;
    for (const FieldGoalEntry& e : t.entries) {
        CAPTURE(e.scene);
        CAPTURE(e.beat);
        CAPTURE(e.step);
        // The marker (and waypoint) are clear of every prop, and each leg of the walk clears them too.
        const Vec2 mid = e.hasVia ? e.via : e.goal;
        for (const demo::FieldObstacle& o : demo::kFieldObstacles) {
            CHECK(demo::segmentDistance(demo::kExploreStart, mid, o.pos) > o.radius + 0.4f);
            if (e.hasVia) CHECK(demo::segmentDistance(e.via, e.goal, o.pos) > o.radius + 0.4f);
            const Vec2 end = e.goal;
            CHECK(std::hypot(o.pos.x - end.x, o.pos.y - end.y) > o.radius + 0.4f);
            if (e.hasVia) CHECK(std::hypot(o.pos.x - e.via.x, o.pos.y - e.via.y) > o.radius + 0.4f);
        }
        CHECK(std::hypot(e.goal.x, e.goal.y) > demo::kFieldGoalRadius + 3.f); // not on top of the start

        Vec2 pos = demo::kExploreStart;
        Vec2 facing{1.f, 0.f};
        bool viaDone = false;
        float seconds = 0.f;
        bool done = false;
        while (seconds < 12.f) {
            if (demo::fieldPilotStepVia(pos, facing, e.goal, e.via, e.hasVia, viaDone, 3.3f, dt)) {
                done = true;
                break;
            }
            seconds += dt;
        }
        CHECK(done);
        CHECK(seconds < 10.f);
    }
}

TEST_CASE("field goals: a waypoint is passed before the marker") {
    Vec2 pos = demo::kExploreStart;
    Vec2 facing{1.f, 0.f};
    const Vec2 goal{4.f, 10.f};
    const Vec2 via{10.f, -4.f};
    bool viaDone = false;
    float closest = 1e9f;
    for (int i = 0; i < 600; ++i) {
        if (demo::fieldPilotStepVia(pos, facing, goal, via, true, viaDone, 3.3f, 1.f / 30.f)) break;
        closest = std::fmin(closest, std::hypot(pos.x - via.x, pos.y - via.y));
    }
    CHECK(viaDone);
    CHECK(closest <= demo::kFieldViaRadius + 0.2f);
    CHECK(demo::fieldGoalInReach(pos, facing, goal));
}

TEST_CASE("step tables: the QTE, cutscene and field tables name every such step of the script, and only those") {
    FTCORE_REQUIRE_REAL_STORY(s);
    std::string qteText, cutText, fieldText;
    if (!loadTable("Qte/qte.json", qteText) || !loadTable("Cutscene/cutscenes.json", cutText) ||
        !loadTable("Field/goals.json", fieldText))
        return;

    QteTable qte;
    CutsceneTable cuts;
    FieldGoalTable field;
    std::string error;
    REQUIRE_MESSAGE(importQteTable(qteText, qte, &error), error);
    REQUIRE_MESSAGE(importCutsceneTable(cutText, cuts, &error), error);
    REQUIRE_MESSAGE(importFieldGoalTable(fieldText, field, &error), error);

    const std::set<Key> qteSteps = storySteps(s.db, GameplayScreen::Qte);
    const std::set<Key> cutSteps = storySteps(s.db, GameplayScreen::Cutscene);
    const std::set<Key> fieldSteps = storySteps(s.db, GameplayScreen::Field);
    CHECK(qteSteps.size() == 5);
    CHECK(cutSteps.size() == 6);
    CHECK(fieldSteps.size() == 91);

    std::set<Key> have;
    for (const QteEntry& e : qte.entries) have.emplace(e.scene, e.beat, e.step);
    CHECK(have == qteSteps);
    have.clear();
    for (const CutsceneEntry& e : cuts.entries) have.emplace(e.scene, e.beat, e.step);
    CHECK(have == cutSteps);
    have.clear();
    for (const FieldGoalEntry& e : field.entries) have.emplace(e.scene, e.beat, e.step);
    CHECK(have == fieldSteps);

    // Each entry is real play: a QTE fits the screen's clock and the lengths grow with the story; a
    // cutscene has 3..8 shots.
    for (const QteEntry& e : qte.entries) {
        CHECK(e.def.size() >= 4);
        CHECK(e.def.length() <= 8.0);
    }
    for (const CutsceneEntry& e : cuts.entries) {
        CHECK(e.plan.size() >= 3);
        CHECK(e.plan.size() <= 8);
    }
}
