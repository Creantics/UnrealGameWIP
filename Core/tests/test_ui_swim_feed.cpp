#include "ftcore/UiSwimFeed.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

#include <limits>

using namespace ftcore;

namespace {

constexpr float kDt = 0.05f;

// Deep water, a dive allowed, the lead at the surface.
SwimEnv deep() {
    SwimEnv env;
    env.deepAllowed = true;
    return env;
}

// One frame as the host runs it: the swim stepped, the body moved to its position, the HUD input read from it.
struct Lead {
    Swim swim;
    UiSwimFeed feed;
    SwimEnv env = deep();
    UiSwim hud;
    UiFirstUse fu;
    bool changed = false;

    explicit Lead(const SwimTuning& t = SwimTuning{}) : swim(t) { uiFirstUseBegin(fu, 0u, UiFirstUseMode::Saved); }

    UiSwimIn frame(const SwimInput& input, float dt = kDt) {
        const SwimStep& st = swim.step(input, env, dt);
        if (st.positionValid) env.position = st.position;
        const UiSwimIn in = uiSwimInFrom(feed, swim, st, input, env, ShowBasicControls::FirstFights);
        changed = uiSwimStep(hud, fu, in, dt) || changed;
        return in;
    }
};

SwimInput press(bool crouch, bool jump = false) {
    SwimInput in;
    in.crouch = crouch;
    in.jump = jump;
    return in;
}

constexpr std::size_t at(UiFirstUseItem item) { return static_cast<std::size_t>(item); }

} // namespace

TEST_CASE("ui swim feed: DIVE is offered exactly where a crouch press dives") {
    struct Case {
        bool deepAllowed;
        float bedZ;
        float column;
    };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Case cases[] = {
        {true, -10.f, 10.f},   // deep
        {false, -10.f, 10.f},  // not a dive area
        {true, -2.9f, 2.9f},   // too shallow
        {true, -3.f, 3.f},     // just deep enough
        {true, -10.f, nan},    // a bad column: Swim judges the span
        {true, -2.f, 10.f},    // a column that disagrees with the span: the span (2 m) again
    };
    for (const Case& c : cases) {
        CAPTURE(c.bedZ);
        Lead lead;
        lead.env.deepAllowed = c.deepAllowed;
        lead.env.bedZ = c.bedZ;
        lead.env.column = c.column;
        const UiSwimIn idle = lead.frame(SwimInput{});
        CHECK(idle.swimming);
        CHECK_FALSE(idle.canRise);
        const UiSwimIn pressed = lead.frame(press(true));
        const SwimStep& st = lead.swim.last();
        CHECK(st.dived == idle.canDive);
        CHECK(st.diveRefused == !idle.canDive);
        CHECK(pressed.dived == st.dived);
        CHECK(uiSwimPromptAlpha(UiSwim{}, lead.fu, idle, UiFirstUseItem::Dive) == (idle.canDive ? 1.f : 0.f));
    }
}

TEST_CASE("ui swim feed: on an env Swim cannot move in, DIVE is not offered and the press waits") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (int k = 0; k < 4; ++k) {
        CAPTURE(k);
        Lead lead;
        SwimEnv bad = deep(); // the column stays finite: only the guard tells this env is unusable
        if (k == 0) bad.surfaceZ = nan;
        if (k == 1) bad.bedZ = nan;
        if (k == 2) bad.bedZ = bad.surfaceZ + 1.f; // the bed above the surface
        if (k == 3) bad.position.z = nan;
        CHECK_FALSE(uiSwimFeedEnvUsable(bad));
        lead.env = bad;
        const UiSwimIn idle = lead.frame(SwimInput{});
        CHECK_FALSE(idle.canDive);
        CHECK(uiSwimPromptAlpha(lead.hud, lead.fu, idle, UiFirstUseItem::Dive) == 0.f);
        const UiSwimIn pressed = lead.frame(press(true));
        CHECK_FALSE(lead.swim.last().dived); // Swim only pends it
        CHECK_FALSE(pressed.dived);
        // The env comes good: Swim dives on its pended press, and the feed reports that dive.
        lead.env = deep();
        const UiSwimIn good = lead.frame(press(true));
        CHECK(lead.swim.last().dived);
        CHECK(good.dived);
        CHECK(lead.fu.count[at(UiFirstUseItem::Dive)] == 1);
    }
    CHECK(uiSwimFeedEnvUsable(deep()));
}

TEST_CASE("ui swim feed: real swim steps count DIVE and RISE on use, and a swim up through the surface is no rise") {
    Lead lead;
    UiSwimIn in = lead.frame(SwimInput{});
    CHECK(in.canDive);
    CHECK(uiSwimPromptAlpha(lead.hud, lead.fu, in, UiFirstUseItem::Dive) == 1.f);
    CHECK_FALSE(lead.changed);
    in = lead.frame(press(true)); // the dive
    CHECK(in.dived);
    CHECK(in.canRise);
    CHECK_FALSE(in.canDive);
    CHECK(lead.changed);
    CHECK(lead.fu.count[at(UiFirstUseItem::Dive)] == 1);
    CHECK(uiSwimPromptAlpha(lead.hud, lead.fu, in, UiFirstUseItem::Rise) == 1.f);
    for (int i = 0; i < 40 && lead.swim.state() != SwimState::Under; ++i) in = lead.frame(press(true)); // held: no press
    REQUIRE(lead.swim.state() == SwimState::Under);
    CHECK(in.canRise);
    CHECK_FALSE(in.rose);
    // The hints: every action the swim refuses, and Crouch and Jump (their keys read DIVE and RISE).
    CHECK(uiSwimHidden(lead.hud, in) == kUiSwimHintAll);
    lead.changed = false;
    in = lead.frame(press(false, true)); // the rise
    CHECK(lead.swim.state() == SwimState::Surfacing);
    CHECK(in.rose);
    CHECK_FALSE(in.canRise);
    CHECK(lead.changed);
    CHECK(lead.fu.count[at(UiFirstUseItem::Rise)] == 1);
    in = lead.frame(press(false, true));
    CHECK_FALSE(in.rose); // Surfacing on: one rise
    for (int i = 0; i < 40 && lead.swim.state() != SwimState::Surface; ++i) in = lead.frame(press(false, true));
    CHECK(lead.swim.state() == SwimState::Surface);
    CHECK_FALSE(in.rose);

    // Under, just below the surface: swimming up through it is no rise; a jump press that reaches it is one.
    for (int pressJump = 0; pressJump < 2; ++pressJump) {
        CAPTURE(pressJump);
        Lead l;
        l.frame(SwimInput{});
        l.frame(press(true));
        for (int i = 0; i < 40 && l.swim.state() != SwimState::Under; ++i) l.frame(SwimInput{});
        REQUIRE(l.swim.state() == SwimState::Under);
        l.env.position.z = l.env.surfaceZ - 0.01f;
        SwimInput up;
        up.look = SwimVec{0.f, 0.f, 1.f};
        up.forward = 1.f;
        up.jump = pressJump != 0;
        const UiSwimIn last = l.frame(up, 0.1f);
        REQUIRE(l.swim.state() == SwimState::Surface);
        CHECK(last.rose == (pressJump != 0));
    }
}

TEST_CASE("ui swim feed: breath, the drain and the rescue come from the step") {
    SwimTuning t;
    t.breathSeconds = 12.f;
    t.warningSeconds = 10.f;
    t.drainPerSecond = 1.f;
    Lead lead(t);
    lead.frame(SwimInput{});
    UiSwimIn in = lead.frame(press(true));
    CHECK_FALSE(in.warning);
    SwimInput down; // held at the bed: with no move input the lead would drift up and breathe
    down.look = SwimVec{0.f, 0.f, -1.f};
    down.forward = 1.f;
    for (int i = 0; i < 60; ++i) in = lead.frame(down);
    CHECK(in.breath == lead.swim.last().breath);
    CHECK(in.warningSeconds == 10.f);
    CHECK(in.warning);
    CHECK_FALSE(in.draining);
    CHECK(lead.hud.heart);
    for (int i = 0; i < 400 && !in.draining; ++i) in = lead.frame(down);
    CHECK(in.draining);
    CHECK(in.vitality == lead.swim.last().vitality);
    for (int i = 0; i < 400 && !in.rescued; ++i) in = lead.frame(down);
    CHECK(in.rescued);
    CHECK_FALSE(in.canRise);
    CHECK_FALSE(in.canDive);
    CHECK(lead.hud.fade == UiSwimFade::Out);
    // Out of the water: nothing hidden, no prompt.
    const UiSwimIn dry = uiSwimInDry(lead.feed, SwimInput{}, ShowBasicControls::FirstFights);
    CHECK_FALSE(dry.swimming);
    CHECK(uiSwimHidden(UiSwim{}, dry) == 0u);
}

TEST_CASE("ui swim feed: feeding allocates nothing") {
    const bool counting = ftcore_test::allocHookInstalled();
    Lead lead;
    const auto before = ftcore_test::allocStats().count;
    float sum = 0.f;
    for (int i = 0; i < 300; ++i) {
        const UiSwimIn in = lead.frame(press((i / 7) % 2 == 0, (i / 11) % 2 == 0));
        sum += uiSwimPromptAlpha(lead.hud, lead.fu, in, UiFirstUseItem::Dive) + in.breath;
    }
    if (counting) CHECK(ftcore_test::allocStats().count == before);
    CHECK(sum > 0.f);
}
