#include "ftcore/UiFirstUse.h"

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

void run(UiFirstUse& s, float seconds) {
    for (float t = 0.f; t < seconds - 1e-4f; t += 0.05f) uiFirstUseTick(s, 0.05f);
}

constexpr std::size_t at(UiFirstUseItem item) { return static_cast<std::size_t>(item); }

} // namespace

TEST_CASE("ui first use: the switch reads fresh and seen only") {
    UiFirstUseMode m = UiFirstUseMode::Saved;
    CHECK(uiFirstUseMode("fresh", m));
    CHECK(m == UiFirstUseMode::Fresh);
    CHECK(uiFirstUseMode("seen", m));
    CHECK(m == UiFirstUseMode::Seen);
    CHECK_FALSE(uiFirstUseMode("saved", m));
    CHECK_FALSE(uiFirstUseMode("", m));
    CHECK(m == UiFirstUseMode::Seen);
}

TEST_CASE("ui first use: a label holds 4 s, fades over 1 s, and shows the first three times in a save") {
    UiFirstUse s;
    uiFirstUseBegin(s, 0u, UiFirstUseMode::Saved);
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::LoadLabel) == 0.f);
    for (int time = 1; time <= 3; ++time) {
        CAPTURE(time);
        CHECK(uiFirstUseShow(s, UiFirstUseItem::LoadLabel)); // counted on showing: the host saves now
        CHECK(s.count[at(UiFirstUseItem::LoadLabel)] == time);
        run(s, 3.9f);
        CHECK(uiFirstUseAlpha(s, UiFirstUseItem::LoadLabel) == 1.f);
        run(s, 0.6f);
        CHECK(uiFirstUseAlpha(s, UiFirstUseItem::LoadLabel) == doctest::Approx(0.5f).epsilon(0.05));
        run(s, 0.6f);
        CHECK(uiFirstUseAlpha(s, UiFirstUseItem::LoadLabel) == 0.f);
        CHECK(s.live == 0u);
    }
    CHECK_FALSE(uiFirstUseShow(s, UiFirstUseItem::LoadLabel)); // a fourth time: no
    CHECK(s.count[at(UiFirstUseItem::LoadLabel)] == kUiFirstUseLabelTimes);
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::LoadLabel) == 0.f);
    uiFirstUseUsed(s, UiFirstUseItem::LoadLabel); // labels are not used
    CHECK(s.used == 0u);
    // LOAD FULL is a label like the rest.
    CHECK(uiFirstUseTimes(UiFirstUseItem::LoadFullLabel) == kUiFirstUseLabelTimes);
    CHECK_FALSE(uiFirstUseControl(UiFirstUseItem::LoadFullLabel));
}

TEST_CASE("ui first use: a label does not restart while it is live") {
    UiFirstUse s;
    uiFirstUseBegin(s, 0u, UiFirstUseMode::Saved);
    CHECK(uiFirstUseShow(s, UiFirstUseItem::PoiseLabel));
    run(s, 2.f);
    CHECK_FALSE(uiFirstUseShow(s, UiFirstUseItem::PoiseLabel)); // already on screen: no restart, no count
    CHECK(s.count[at(UiFirstUseItem::PoiseLabel)] == 1);
    run(s, 2.5f); // 4.5 s from the first show, not 2.5 s from the second
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::PoiseLabel) == doctest::Approx(0.5f).epsilon(0.05));
    run(s, 0.6f);
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::PoiseLabel) == 0.f);
    CHECK(uiFirstUseShow(s, UiFirstUseItem::PoiseLabel)); // gone: the next time counts
    CHECK(s.count[at(UiFirstUseItem::PoiseLabel)] == 2);
}

TEST_CASE("ui first use: a label shows when its element starts to be drawn, not on every tick it stays") {
    UiFirstUse s;
    uiFirstUseBegin(s, 0u, UiFirstUseMode::Saved);
    const UiFirstUseItem g = UiFirstUseItem::GuardLabel;
    CHECK_FALSE(uiFirstUseCue(s, g, false));
    CHECK(uiFirstUseCue(s, g, true)); // the guard line appears
    for (int i = 0; i < 200; ++i) {   // and stays well past the label's 5 s
        CHECK_FALSE(uiFirstUseCue(s, g, true));
        uiFirstUseTick(s, 0.05f);
    }
    CHECK(s.count[at(g)] == 1);
    CHECK(uiFirstUseAlpha(s, g) == 0.f);
    CHECK_FALSE(uiFirstUseCue(s, g, false)); // it goes, then comes back: the second time
    CHECK(uiFirstUseCue(s, g, true));
    CHECK(s.count[at(g)] == 2);
    // A start that is not allowed passes and counts nothing (the next start is a new one).
    run(s, 5.1f);
    CHECK_FALSE(uiFirstUseCue(s, g, false));
    CHECK_FALSE(uiFirstUseCue(s, g, true, false));
    CHECK_FALSE(uiFirstUseCue(s, g, true));
    CHECK(s.count[at(g)] == 2);
    CHECK(uiFirstUseAlpha(s, g) == 0.f);
    // The fight's end forgets what was drawn: the next fight's first drawing is a start.
    uiFirstUseEnd(s);
    CHECK(uiFirstUseCue(s, g, true));
    CHECK(s.count[at(g)] == 3);
}

TEST_CASE("ui first use: the rise that fills the name shows LOAD FULL, not LOAD as well") {
    UiFirstUse s;
    uiFirstUseBegin(s, 0u, UiFirstUseMode::Saved);
    // The host's order: LOAD FULL first, then LOAD only while LOAD FULL is off screen.
    const auto cue = [&s](bool load, bool full) {
        uiFirstUseCue(s, UiFirstUseItem::LoadFullLabel, full);
        uiFirstUseCue(s, UiFirstUseItem::LoadLabel, load, uiFirstUseAlpha(s, UiFirstUseItem::LoadFullLabel) <= 0.f);
    };
    cue(true, true); // one blow from empty to full
    CHECK(s.count[at(UiFirstUseItem::LoadFullLabel)] == 1);
    CHECK(s.count[at(UiFirstUseItem::LoadLabel)] == 0);
    cue(false, false); // spent
    cue(true, false);  // a rise while LOAD FULL is still up: not shown behind it, not counted
    CHECK(s.count[at(UiFirstUseItem::LoadLabel)] == 0);
    run(s, 5.1f);
    cue(true, false); // the same rise: no new start
    CHECK(s.count[at(UiFirstUseItem::LoadLabel)] == 0);
    cue(false, false);
    cue(true, false); // a new rise
    CHECK(s.count[at(UiFirstUseItem::LoadLabel)] == 1);
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::LoadLabel) == 1.f);
}

TEST_CASE("ui first use: a control stays until a second after its first use, then fades over 1 s, once a save") {
    UiFirstUse s;
    uiFirstUseBegin(s, 0u, UiFirstUseMode::Saved);
    CHECK(uiFirstUseShow(s, UiFirstUseItem::Dodge));
    run(s, 30.f);
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::Dodge) == 1.f); // unused: no clock
    uiFirstUseUsed(s, UiFirstUseItem::Dodge);
    run(s, 0.9f);
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::Dodge) == 1.f);
    uiFirstUseUsed(s, UiFirstUseItem::Dodge); // a second use does not restart it
    run(s, 0.6f);
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::Dodge) == doctest::Approx(0.5f).epsilon(0.05));
    run(s, 0.6f);
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::Dodge) == 0.f);
    CHECK_FALSE(uiFirstUseShow(s, UiFirstUseItem::Dodge)); // a control is taught once
    uiFirstUseUsed(s, UiFirstUseItem::Jump); // never shown: ignored
    CHECK((s.used & uiFirstUseBit(UiFirstUseItem::Jump)) == 0u);
}

TEST_CASE("ui first use: the fight's end clears the screen and keeps the counts") {
    UiFirstUse s;
    uiFirstUseBegin(s, 0u, UiFirstUseMode::Saved);
    uiFirstUseShow(s, UiFirstUseItem::Attack);
    uiFirstUseShow(s, UiFirstUseItem::GuardLabel);
    uiFirstUseEnd(s);
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::Attack) == 0.f);
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::GuardLabel) == 0.f);
    CHECK(s.count[at(UiFirstUseItem::Attack)] == 1);
    CHECK(s.count[at(UiFirstUseItem::GuardLabel)] == 1);
}

TEST_CASE("ui first use: the counts round-trip through the save, and a save is never lowered") {
    UiFirstUse s;
    uiFirstUseBegin(s, 0u, UiFirstUseMode::Saved);
    CHECK(uiFirstUseSaved(s) == kUiFirstUseCounted); // nothing shown yet
    uiFirstUseShow(s, UiFirstUseItem::Guard);
    for (int i = 0; i < 2; ++i) {
        uiFirstUseShow(s, UiFirstUseItem::WeakPointLabel);
        uiFirstUseEnd(s);
    }
    for (int i = 0; i < 3; ++i) {
        uiFirstUseShow(s, UiFirstUseItem::LoadFullLabel);
        uiFirstUseEnd(s);
    }
    const std::uint32_t saved = uiFirstUseSaved(s);
    CHECK((saved & kUiFirstUseCounted) != 0u);
    UiFirstUse next; // the next fight of the same save
    uiFirstUseBegin(next, saved, UiFirstUseMode::Saved);
    for (std::size_t i = 0; i < kUiFirstUseCount; ++i) {
        CAPTURE(i);
        CHECK(next.count[i] == s.count[i]);
        CHECK(uiFirstUseSavedCount(saved, static_cast<UiFirstUseItem>(i)) == s.count[i]);
    }
    CHECK(uiFirstUseSaved(next) == saved);
    CHECK_FALSE(uiFirstUseShow(next, UiFirstUseItem::Guard));
    CHECK_FALSE(uiFirstUseShow(next, UiFirstUseItem::LoadFullLabel));
    CHECK(uiFirstUseShow(next, UiFirstUseItem::WeakPointLabel)); // its third time
    CHECK(uiFirstUseShow(next, UiFirstUseItem::Attack)); // never taught: shows

    // Joining two saves keeps the higher count of each item, whichever is older.
    UiFirstUse other;
    uiFirstUseBegin(other, 0u, UiFirstUseMode::Saved);
    for (int i = 0; i < 3; ++i) {
        uiFirstUseShow(other, UiFirstUseItem::PoiseLabel);
        uiFirstUseEnd(other);
    }
    const std::uint32_t joined = uiFirstUseMerge(saved, uiFirstUseSaved(other));
    CHECK(uiFirstUseMerge(uiFirstUseSaved(other), saved) == joined);
    CHECK(uiFirstUseSavedCount(joined, UiFirstUseItem::PoiseLabel) == 3);
    CHECK(uiFirstUseSavedCount(joined, UiFirstUseItem::WeakPointLabel) == 2);
    CHECK(uiFirstUseSavedCount(joined, UiFirstUseItem::LoadFullLabel) == 3);
    CHECK(uiFirstUseSavedCount(joined, UiFirstUseItem::Guard) == 1);
    CHECK(uiFirstUseSavedCount(joined, UiFirstUseItem::LoadLabel) == 0);
    CHECK(uiFirstUseMerge(joined, kUiFirstUseCounted) == joined); // a fresh run's empty counts lower nothing
}

TEST_CASE("ui first use: a save from before the counts reads each set bit as shown once") {
    // The old layout: one bit per item, GuardLabel..Jump at bits 0..7.
    const std::uint32_t old = uiFirstUseBit(UiFirstUseItem::LoadLabel) | uiFirstUseBit(UiFirstUseItem::Dodge);
    CHECK((old & kUiFirstUseCounted) == 0u);
    CHECK(uiFirstUseSavedCount(old, UiFirstUseItem::LoadLabel) == 1);
    CHECK(uiFirstUseSavedCount(old, UiFirstUseItem::Dodge) == 1);
    CHECK(uiFirstUseSavedCount(old, UiFirstUseItem::GuardLabel) == 0);
    CHECK(uiFirstUseSavedCount(old, UiFirstUseItem::LoadFullLabel) == 0);
    UiFirstUse s;
    uiFirstUseBegin(s, old, UiFirstUseMode::Saved);
    CHECK_FALSE(uiFirstUseShow(s, UiFirstUseItem::Dodge)); // a control already taught
    CHECK(uiFirstUseShow(s, UiFirstUseItem::LoadLabel));    // a label: two showings left
    CHECK(s.count[at(UiFirstUseItem::LoadLabel)] == 2);
    // All eight old bits: every control taught, every label once.
    const std::uint32_t all = 0xFFu;
    const std::uint32_t joined = uiFirstUseMerge(all, 0u);
    CHECK(uiFirstUseSavedCount(joined, UiFirstUseItem::Jump) == 1);
    CHECK(uiFirstUseSavedCount(joined, UiFirstUseItem::PoiseLabel) == 1);
    CHECK(uiFirstUseSavedCount(joined, UiFirstUseItem::LoadFullLabel) == 0);
}

TEST_CASE("ui first use: fresh ignores the save, seen teaches nothing") {
    UiFirstUse full;
    uiFirstUseBegin(full, 0u, UiFirstUseMode::Seen);
    const std::uint32_t all = uiFirstUseSaved(full);
    UiFirstUse s;
    uiFirstUseBegin(s, all, UiFirstUseMode::Fresh);
    CHECK(uiFirstUseShow(s, UiFirstUseItem::WeakPointLabel));
    CHECK(uiFirstUseShow(s, UiFirstUseItem::LoadFullLabel));
    uiFirstUseBegin(s, 0u, UiFirstUseMode::Seen);
    CHECK(s.live == 0u);
    for (std::size_t i = 0; i < kUiFirstUseCount; ++i) {
        CHECK_FALSE(uiFirstUseShow(s, static_cast<UiFirstUseItem>(i)));
        CHECK_FALSE(uiFirstUseCue(s, static_cast<UiFirstUseItem>(i), true));
        CHECK(uiFirstUseAlpha(s, static_cast<UiFirstUseItem>(i)) == 0.f);
    }
    uiFirstUseBegin(s, all, UiFirstUseMode::Saved);
    CHECK_FALSE(uiFirstUseShow(s, UiFirstUseItem::Guard));
    CHECK_FALSE(uiFirstUseShow(s, UiFirstUseItem::LoadLabel));
}
