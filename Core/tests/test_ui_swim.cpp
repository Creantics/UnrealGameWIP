#include "ftcore/UiSwim.h"

#include "AllocCounter.h"

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

constexpr float kDt = 0.05f;

// Under water with `breath` seconds left, as the swim step reports it (the swim flags refuse attack, dodge, guard and
// weapons out).
UiSwimIn under(float breath) {
    UiSwimIn in;
    in.swimming = true;
    in.under = true;
    in.canRise = true;
    in.breath = breath;
    in.warning = breath <= in.warningSeconds;
    in.draining = breath <= 0.f;
    in.attack = false;
    in.dodge = false;
    in.guard = false;
    in.weaponsOut = false;
    return in;
}

// At the surface in deep water: breath not in use, a dive allowed.
UiSwimIn surface(float breath) {
    UiSwimIn in = under(breath);
    in.under = false;
    in.canRise = false;
    in.canDive = true;
    in.warning = false;
    in.draining = false;
    return in;
}

// Rescued: the swim step's warning and drain cleared, no breath, no vitality.
UiSwimIn rescued() {
    UiSwimIn in = under(0.f);
    in.rescued = true;
    in.canRise = false;
    in.warning = false;
    in.draining = false;
    in.vitality = 0.f;
    return in;
}

// Steps `seconds` with the same input; true when any step changed the teaching counts.
bool run(UiSwim& s, UiFirstUse& fu, const UiSwimIn& in, float seconds) {
    bool changed = false;
    for (float t = 0.f; t < seconds - 1e-4f; t += kDt) changed = uiSwimStep(s, fu, in, kDt) || changed;
    return changed;
}

UiFirstUse taught(UiFirstUseMode mode = UiFirstUseMode::Saved) {
    UiFirstUse fu;
    uiFirstUseBegin(fu, 0u, mode);
    return fu;
}

constexpr std::size_t at(UiFirstUseItem item) { return static_cast<std::size_t>(item); }

} // namespace

TEST_CASE("ui swim: the curve is eased and clamped, from warningSeconds of breath left") {
    CHECK(uiSwimEase(0.f) == 0.f);
    CHECK(uiSwimEase(1.f) == 1.f);
    CHECK(uiSwimEase(0.5f) == doctest::Approx(0.5f));
    CHECK(uiSwimEase(-1.f) == 0.f);
    CHECK(uiSwimEase(2.f) == 1.f);
    CHECK(uiSwimBreathCurve(45.f, 10.f) == 0.f);
    CHECK(uiSwimBreathCurve(10.f, 10.f) == 0.f);
    CHECK(uiSwimBreathCurve(5.f, 10.f) == doctest::Approx(0.5f));
    CHECK(uiSwimBreathCurve(0.f, 10.f) == 1.f);
    CHECK(uiSwimBreathCurve(-1.f, 10.f) == 1.f);
    CHECK(uiSwimBreathCurve(1.f, 0.f) == 0.f); // no warning time: a step at empty
    CHECK(uiSwimBreathCurve(0.f, 0.f) == 1.f);
    CHECK(uiSwimDrainCurve(0.5f, 0.5f) == 0.f);
    CHECK(uiSwimDrainCurve(0.25f, 0.5f) == doctest::Approx(0.5f));
    CHECK(uiSwimDrainCurve(0.f, 0.5f) == 1.f);
    CHECK(uiSwimDrainCurve(0.3f, 0.f) == 1.f);
    CHECK(uiSwimRingTarget(0.f, 0.f) == 0.f);
    CHECK(uiSwimRingTarget(1.f, 0.f) == doctest::Approx(kUiSwimBreathPart));
    CHECK(uiSwimRingTarget(1.f, 1.f) == doctest::Approx(1.f));
    // Never lighter with less breath.
    float last = 0.f;
    for (int i = 0; i <= 24; ++i) {
        CAPTURE(i);
        const float k = uiSwimRingTarget(uiSwimBreathCurve(12.f - 0.5f * static_cast<float>(i), 10.f), 0.f);
        CHECK(k >= last);
        last = k;
    }
}

TEST_CASE("ui swim: the picture darkens and narrows as breath runs out, and opens as it refills") {
    UiSwim s;
    UiFirstUse fu = taught();
    run(s, fu, under(20.f), 1.f);
    CHECK(s.ring == 0.f);
    CHECK(uiSwimEdgeAlpha(s) == 0.f);
    CHECK(uiSwimClearRadius(s) == 1.f);
    // Breath from 10 s to none, one second a second: the ring only grows.
    float last = 0.f;
    float atHalf = 0.f;
    for (int i = 0; i <= 200; ++i) {
        const float breath = std::max(10.f - kDt * static_cast<float>(i), 0.f);
        UiSwimIn in = under(breath);
        in.draining = false; // the drain starts below
        uiSwimStep(s, fu, in, kDt);
        CAPTURE(i);
        CHECK(s.ring >= last - 1e-6f);
        last = s.ring;
        if (i == 100) atHalf = s.ring;
    }
    CHECK(atHalf == doctest::Approx(0.5f * kUiSwimBreathPart).epsilon(0.05));
    CHECK(s.ring == doctest::Approx(kUiSwimBreathPart).epsilon(0.02));
    CHECK(uiSwimEdgeAlpha(s) == doctest::Approx(kUiSwimEdgeMax * kUiSwimBreathPart).epsilon(0.02));
    CHECK(uiSwimClearRadius(s) == doctest::Approx(1.f - kUiSwimNarrow * kUiSwimBreathPart).epsilon(0.02));
    // The drain takes it the rest of the way, never past the most.
    for (int i = 0; i <= 19; ++i) {
        UiSwimIn in = under(0.f);
        in.vitality = 1.f - kDt * static_cast<float>(i);
        uiSwimStep(s, fu, in, kDt);
    }
    CHECK(s.ring > 0.95f);
    CHECK(s.ring <= 1.f);
    CHECK(uiSwimEdgeAlpha(s) <= kUiSwimEdgeMax);
    CHECK(uiSwimClearRadius(s) >= 1.f - kUiSwimNarrow);
    // At the surface breath refills (empty to full in 2 s): the ring opens no faster than the slew, and is gone well
    // inside a second.
    const float before = s.ring;
    uiSwimStep(s, fu, surface(45.f * kDt / 2.f), kDt);
    CHECK(s.ring < before);
    CHECK(s.ring >= before - kUiSwimSlew * kDt - 1e-5f);
    for (int i = 2; i <= 20; ++i) uiSwimStep(s, fu, surface(std::min(45.f * kDt * static_cast<float>(i) / 2.f, 45.f)), kDt);
    CHECK(s.ring == 0.f);
    // Out of the water: nothing.
    run(s, fu, UiSwimIn{}, 0.5f);
    CHECK(s.ring == 0.f);
}

TEST_CASE("ui swim: the heartbeat starts at the warning, quickens with it and the drain, and stops at once") {
    UiSwim s;
    UiFirstUse fu = taught();
    run(s, fu, under(11.f), 0.5f);
    CHECK_FALSE(s.heart);
    CHECK_FALSE(s.beat);
    CHECK(s.bpm == 0.f);
    uiSwimStep(s, fu, under(10.f), kDt);
    CHECK(s.heart);
    CHECK(s.beat); // the first beat falls on the frame it starts
    CHECK(s.bpm == doctest::Approx(kUiSwimHeartRest));
    uiSwimStep(s, fu, under(10.f), kDt);
    CHECK_FALSE(s.beat);
    // Almost no breath: the warning's top rate, about two beats a second.
    int beats = 0;
    for (int i = 0; i < 40; ++i) {
        uiSwimStep(s, fu, under(0.001f), kDt);
        if (s.beat) ++beats;
    }
    CHECK(s.bpm == doctest::Approx(kUiSwimHeartWarn).epsilon(0.01));
    CHECK(beats >= 3);
    CHECK(beats <= 5);
    // The drain: from the warning's top to the drain's as the vitality runs out.
    UiSwimIn drain = under(0.f);
    drain.vitality = 0.5f;
    uiSwimStep(s, fu, drain, kDt);
    CHECK(s.bpm == doctest::Approx(kUiSwimHeartWarn).epsilon(0.01));
    drain.vitality = 0.01f;
    uiSwimStep(s, fu, drain, kDt);
    CHECK(s.bpm == doctest::Approx(kUiSwimHeartDrain).epsilon(0.01));
    CHECK(s.bpm <= kUiSwimHeartDrain);
    CHECK(s.heart);
    // The surface stops it on that frame, and so does an air pocket (breath refilling: no warning).
    uiSwimStep(s, fu, surface(5.f), kDt);
    CHECK_FALSE(s.heart);
    CHECK_FALSE(s.beat);
    CHECK(s.bpm == 0.f);
    uiSwimStep(s, fu, under(5.f), kDt);
    CHECK(s.heart);
    UiSwimIn pocket = under(5.f);
    pocket.warning = false;
    uiSwimStep(s, fu, pocket, kDt);
    CHECK_FALSE(s.heart);
}

TEST_CASE("ui swim: a rescue is a fade to black and back, the ring held until black") {
    UiSwim s;
    UiFirstUse fu = taught();
    UiSwimIn drain = under(0.f);
    drain.vitality = 0.2f;
    run(s, fu, drain, 1.f);
    const float held = s.ring;
    CHECK(held > 0.5f);
    CHECK(s.heart);
    uiSwimStep(s, fu, rescued(), kDt);
    CHECK(s.fade == UiSwimFade::Out);
    CHECK(uiSwimFadeAlpha(s) > 0.f);
    CHECK(uiSwimFadeAlpha(s) < 1.f);
    CHECK_FALSE(s.heart);
    CHECK(uiSwimHidden(s, rescued()) == kUiSwimHintAll);
    run(s, fu, rescued(), 0.25f);
    CHECK(s.fade == UiSwimFade::Out);
    CHECK(s.ring == held); // the ring does not open before the picture is black
    CHECK(uiSwimFadeAlpha(s) > 0.5f);
    run(s, fu, rescued(), 0.1f);
    CHECK(s.fade == UiSwimFade::Black);
    CHECK(uiSwimFadeAlpha(s) == 1.f);
    run(s, fu, rescued(), 2.f);
    CHECK(s.fade == UiSwimFade::Black); // black while rescued
    CHECK_FALSE(s.heart);
    // The host's reset at the exit: full breath at the surface. A moment of black, the ring already open, then back.
    run(s, fu, surface(45.f), 0.1f);
    CHECK(s.fade == UiSwimFade::Black);
    CHECK(s.ring == 0.f);
    run(s, fu, surface(45.f), 0.2f);
    CHECK(s.fade == UiSwimFade::In);
    CHECK(uiSwimFadeAlpha(s) < 1.f);
    CHECK(uiSwimFadeAlpha(s) > 0.5f);
    run(s, fu, surface(45.f), 0.7f);
    CHECK(s.fade == UiSwimFade::Clear);
    CHECK(uiSwimFadeAlpha(s) == 0.f);
    CHECK(uiSwimHidden(s, surface(45.f)) != kUiSwimHintAll);
}

TEST_CASE("ui swim: with no reset the black ends at its cap") {
    UiSwim s;
    UiFirstUse fu = taught();
    run(s, fu, under(0.f), 0.5f);
    run(s, fu, rescued(), 4.f);
    CHECK(s.fade == UiSwimFade::Black);
    run(s, fu, rescued(), 1.5f);
    CHECK(s.fade == UiSwimFade::Clear); // and a rescue still standing does not start another
}

TEST_CASE("ui swim: the hints the swim flags refuse hide, and only in the water") {
    UiSwim s;
    CHECK(uiSwimHidden(s, UiSwimIn{}) == 0u);
    const UiSwimHints flagged = uiSwimHintBit(UiSwimHint::Attack) | uiSwimHintBit(UiSwimHint::Aim)
        | uiSwimHintBit(UiSwimHint::Dodge) | uiSwimHintBit(UiSwimHint::Guard);
    const UiSwimHints top = uiSwimHidden(s, surface(45.f));
    CHECK((top & flagged) == flagged);
    CHECK((top & uiSwimHintBit(UiSwimHint::Crouch)) != 0u);
    CHECK((top & uiSwimHintBit(UiSwimHint::Jump)) == 0u);
    CHECK((uiSwimHidden(s, under(30.f)) & uiSwimHintBit(UiSwimHint::Jump)) != 0u);
    // Each bit follows its own flag.
    UiSwimIn in = under(30.f);
    in.attack = true;
    in.weaponsOut = true;
    const UiSwimHints some = uiSwimHidden(s, in);
    CHECK((some & uiSwimHintBit(UiSwimHint::Attack)) == 0u);
    CHECK((some & uiSwimHintBit(UiSwimHint::Aim)) == 0u);
    CHECK((some & uiSwimHintBit(UiSwimHint::Dodge)) != 0u);
    CHECK((some & uiSwimHintBit(UiSwimHint::Guard)) != 0u);
    in.dodge = true;
    in.guard = true;
    CHECK((uiSwimHidden(s, in) & flagged) == 0u);
}

TEST_CASE("ui swim: DIVE and RISE stand the first time each applies, by the teaching setting") {
    UiSwim s;
    UiFirstUse fu = taught();
    CHECK(run(s, fu, surface(45.f), kDt)); // DIVE counted on showing: the host saves now
    CHECK(uiSwimPromptAlpha(s, fu, surface(45.f), UiFirstUseItem::Dive) == 1.f);
    CHECK(uiSwimPromptAlpha(s, fu, surface(45.f), UiFirstUseItem::Rise) == 0.f);
    CHECK_FALSE(run(s, fu, surface(45.f), 1.f)); // standing: not counted again
    UiSwimIn shallow = surface(45.f);
    shallow.canDive = false;
    CHECK(uiSwimPromptAlpha(s, fu, shallow, UiFirstUseItem::Dive) == 0.f); // drawn only where it applies
    // The dive: DIVE used and gone, RISE shown (and counted) under water.
    UiSwimIn dive = under(45.f);
    dive.dived = true;
    CHECK(run(s, fu, dive, kDt));
    CHECK(uiSwimPromptAlpha(s, fu, under(45.f), UiFirstUseItem::Dive) == 0.f);
    CHECK(uiSwimPromptAlpha(s, fu, under(45.f), UiFirstUseItem::Rise) == 1.f);
    UiSwimIn rise = under(45.f);
    rise.canRise = false; // Surfacing
    rise.rose = true;
    CHECK_FALSE(run(s, fu, rise, kDt));
    CHECK_FALSE(run(s, fu, surface(45.f), 3.f));
    CHECK(uiSwimPromptAlpha(s, fu, surface(45.f), UiFirstUseItem::Dive) == 0.f);
    CHECK_FALSE(run(s, fu, under(45.f), 1.f));
    CHECK(uiSwimPromptAlpha(s, fu, under(45.f), UiFirstUseItem::Rise) == 0.f);
    CHECK(fu.count[at(UiFirstUseItem::Dive)] == 1);
    CHECK(fu.count[at(UiFirstUseItem::Rise)] == 1);
    // A later visit of the same save: neither again.
    UiFirstUse next;
    uiFirstUseBegin(next, uiFirstUseSaved(fu), UiFirstUseMode::Saved);
    UiSwim later;
    CHECK_FALSE(run(later, next, surface(45.f), 1.f));
    CHECK(uiSwimPromptAlpha(later, next, surface(45.f), UiFirstUseItem::Dive) == 0.f);

    // Never: nothing shown, nothing counted.
    UiFirstUse none = taught();
    UiSwim n;
    UiSwimIn never = surface(45.f);
    never.teach = ShowBasicControls::Never;
    CHECK_FALSE(run(n, none, never, 1.f));
    CHECK(uiSwimPromptAlpha(n, none, never, UiFirstUseItem::Dive) == 0.f);
    CHECK(none.count[at(UiFirstUseItem::Dive)] == 0);
    // Always: whenever it applies, taught or not; never while the rescue fades.
    UiSwimIn always = surface(45.f);
    always.teach = ShowBasicControls::Always;
    CHECK(uiSwimPromptAlpha(later, next, always, UiFirstUseItem::Dive) == 1.f);
    CHECK(uiSwimPromptAlpha(later, next, always, UiFirstUseItem::Rise) == 0.f);
    always.canDive = false;
    CHECK(uiSwimPromptAlpha(later, next, always, UiFirstUseItem::Dive) == 0.f);
    UiSwimIn deep = under(45.f);
    deep.teach = ShowBasicControls::Always;
    CHECK(uiSwimPromptAlpha(later, next, deep, UiFirstUseItem::Rise) == 1.f);
    uiSwimStep(later, next, rescued(), kDt);
    CHECK(uiSwimPromptAlpha(later, next, deep, UiFirstUseItem::Rise) == 0.f);
    // Teaching held through a fade: a fresh save under a rescue counts nothing until it clears.
    UiFirstUse fresh = taught();
    CHECK_FALSE(uiSwimStep(later, fresh, surface(45.f), kDt));
    CHECK(fresh.count[at(UiFirstUseItem::Dive)] == 0);
}

TEST_CASE("ui swim: stepping allocates nothing") {
    const bool counting = ftcore_test::allocHookInstalled();
    const auto before = ftcore_test::allocStats().count;
    UiSwim s;
    UiFirstUse fu = taught();
    float sum = 0.f;
    for (int i = 0; i <= 300; ++i) {
        UiSwimIn in = under(std::max(12.f - kDt * static_cast<float>(i), 0.f));
        in.vitality = std::max(1.f - 0.01f * static_cast<float>(std::max(i - 240, 0)), 0.f);
        uiSwimStep(s, fu, in, kDt);
        sum += uiSwimEdgeAlpha(s) + uiSwimClearRadius(s) + uiSwimFadeAlpha(s) + s.bpm
            + uiSwimPromptAlpha(s, fu, in, UiFirstUseItem::Rise) + static_cast<float>(uiSwimHidden(s, in));
    }
    run(s, fu, rescued(), 1.f);
    run(s, fu, surface(45.f), 1.f);
    if (counting) CHECK(ftcore_test::allocStats().count == before);
    CHECK(sum > 0.f);
}
