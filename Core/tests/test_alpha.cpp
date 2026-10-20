// The alpha gate, Core side: the whole real script played by the dialogue presenter's autoplay,
// every gameplay beat expanded through its GameplayPlan and each step resolved at once by a fake
// screen. The Unreal run (-AlphaAutoPlay) does the same with real screens. Counts only: nothing
// textual is printed or compared.

#include "ftcore/Dialogue.h"
#include "ftcore/GameplayPlan.h"
#include "ftcore/StoryRunner.h"

#include "AllocCounter.h"
#include "RealStory.h"

#include <ostream> // doctest prints std::string_view through operator<<
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

using namespace ftcore;

namespace {

// A screen that finishes (and wins) the moment it opens.
struct FakeScreens {
    std::uint32_t opened[4] = {}; // by GameplayScreen
    std::uint32_t wrong = 0;      // a step that asked for the dialogue box

    bool play(const GameplayStep& step, std::uint32_t index, std::uint32_t count) {
        if (step.screen == GameplayScreen::Box || index >= count) {
            ++wrong;
            return false;
        }
        ++opened[static_cast<std::uint8_t>(step.screen)];
        return true;
    }
    std::uint32_t total() const { return opened[0] + opened[1] + opened[2] + opened[3]; }
};

} // namespace

TEST_CASE("alpha: the real script plays start to end through every gameplay step") {
    FTCORE_REQUIRE_REAL_STORY(s);
    const StoryDatabase& db = s.db;
    StoryRunner r(&db);
    DialoguePresenter p(&r);
    p.setRevealRate(0.f);
    GameplayPlan plan;
    FakeScreens screens;
    std::vector<bool> seen(db.sceneCount(), false);
    REQUIRE(r.start());

    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    std::uint32_t beats = 0, unknown = 0, dropped = 0, failed = 0, stuck = 0;
    int moves = 0;
    while (!r.finished() && moves < 200000) {
        if (r.sceneIndex() < seen.size()) seen[r.sceneIndex()] = true;
        if (r.waitingForGameplay()) {
            ++beats;
            if (!plan.lower(r)) ++failed;
            unknown += plan.unknownTokens();
            dropped += plan.droppedSteps();
            bool done = !plan.empty();
            for (std::uint32_t i = 0; i < plan.size(); ++i) done = screens.play(plan.step(i), i, plan.size()) && done;
            if (!done) ++failed;
            if (!r.resume()) { // back to the story only after the last step
                ++failed;
                break;
            }
            ++moves;
            continue;
        }
        p.update(1.f / 60.f);
        const DialogueCommand c = p.autoplay();
        if (c.none() || !applyDialogueCommand(r, c)) {
            if (++stuck > 3) break;
            continue;
        }
        stuck = 0;
        ++moves;
    }
    const auto allocs = ftcore_test::allocStats().count - before;

    std::uint32_t scenes = 0;
    for (const bool b : seen) scenes += b ? 1u : 0u;
    const std::uint32_t battle = screens.opened[static_cast<std::uint8_t>(GameplayScreen::Battle)];
    const std::uint32_t field = screens.opened[static_cast<std::uint8_t>(GameplayScreen::Field)];
    const std::uint32_t stub = screens.opened[static_cast<std::uint8_t>(GameplayScreen::Stub)];

    CHECK(r.finished());
    CHECK_FALSE(r.stalled());
    CHECK(stuck == 0);
    CHECK(db.sceneCount() == 73);
    CHECK(scenes == 73);
    CHECK(beats == 74);
    CHECK(failed == 0);
    CHECK(unknown == 0);
    CHECK(dropped == 0);
    CHECK(screens.wrong == 0);
    CHECK(battle > 0);
    CHECK(field > 0);
    CHECK(stub > 0);
    CHECK(screens.total() == battle + field + stub);
    if (counting) CHECK(allocs == 0);
    MESSAGE("alpha: " << scenes << " scenes, " << beats << " beats, " << screens.total() << " screens (" << battle
                      << " battle, " << field << " field, " << stub << " stub), " << moves << " moves");
}
