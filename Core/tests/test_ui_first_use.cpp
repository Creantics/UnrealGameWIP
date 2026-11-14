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
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::WeakPointLabel) == 0.f);
    for (int time = 1; time <= 3; ++time) {
        CAPTURE(time);
        CHECK(uiFirstUseShow(s, UiFirstUseItem::WeakPointLabel)); // counted on showing: the host saves now
        CHECK(s.count[at(UiFirstUseItem::WeakPointLabel)] == time);
        run(s, 3.9f);
        CHECK(uiFirstUseAlpha(s, UiFirstUseItem::WeakPointLabel) == 1.f);
        run(s, 0.6f);
        CHECK(uiFirstUseAlpha(s, UiFirstUseItem::WeakPointLabel) == doctest::Approx(0.5f).epsilon(0.05));
        run(s, 0.6f);
        CHECK(uiFirstUseAlpha(s, UiFirstUseItem::WeakPointLabel) == 0.f);
        CHECK(s.live == 0u);
    }
    CHECK_FALSE(uiFirstUseShow(s, UiFirstUseItem::WeakPointLabel)); // a fourth time: no
    CHECK(s.count[at(UiFirstUseItem::WeakPointLabel)] == kUiFirstUseLabelTimes);
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::WeakPointLabel) == 0.f);
    uiFirstUseUsed(s, UiFirstUseItem::WeakPointLabel); // labels are not used
    CHECK(s.used == 0u);
    CHECK(uiFirstUseTimes(UiFirstUseItem::WeakPointLabel) == kUiFirstUseLabelTimes);
    CHECK_FALSE(uiFirstUseControl(UiFirstUseItem::WeakPointLabel));
}

TEST_CASE("ui first use: a label does not restart while it is live") {
    UiFirstUse s;
    uiFirstUseBegin(s, 0u, UiFirstUseMode::Saved);
    const UiFirstUseItem w = UiFirstUseItem::WeakPointLabel;
    CHECK(uiFirstUseShow(s, w));
    run(s, 2.f);
    CHECK_FALSE(uiFirstUseShow(s, w)); // already on screen: no restart, no count
    CHECK(s.count[at(w)] == 1);
    run(s, 2.5f); // 4.5 s from the first show, not 2.5 s from the second
    CHECK(uiFirstUseAlpha(s, w) == doctest::Approx(0.5f).epsilon(0.05));
    run(s, 0.6f);
    CHECK(uiFirstUseAlpha(s, w) == 0.f);
    CHECK(uiFirstUseShow(s, w)); // gone: the next time counts
    CHECK(s.count[at(w)] == 2);
}

TEST_CASE("ui first use: a label shows when its element starts to be drawn, not on every tick it stays") {
    UiFirstUse s;
    uiFirstUseBegin(s, 0u, UiFirstUseMode::Saved);
    const UiFirstUseItem g = UiFirstUseItem::WeakPointLabel;
    CHECK_FALSE(uiFirstUseCue(s, g, false));
    CHECK(uiFirstUseCue(s, g, true)); // the weak point appears
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
    uiFirstUseShow(s, UiFirstUseItem::WeakPointLabel);
    uiFirstUseEnd(s);
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::Attack) == 0.f);
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::WeakPointLabel) == 0.f);
    CHECK(s.count[at(UiFirstUseItem::Attack)] == 1);
    CHECK(s.count[at(UiFirstUseItem::WeakPointLabel)] == 1);
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
    uiFirstUseShow(s, UiFirstUseItem::Dodge);
    const std::uint32_t saved = uiFirstUseSaved(s);
    CHECK((saved & kUiFirstUseCounted) != 0u);
    CHECK((saved & 0x3Fu) == 0u);         // the retired slots 0, 1 and 2 are written as zero
    CHECK((saved & (3u << 16)) == 0u);    // and slot 8
    UiFirstUse next; // the next fight of the same save
    uiFirstUseBegin(next, saved, UiFirstUseMode::Saved);
    for (std::size_t i = 0; i < kUiFirstUseCount; ++i) {
        CAPTURE(i);
        CHECK(next.count[i] == s.count[i]);
        CHECK(uiFirstUseSavedCount(saved, static_cast<UiFirstUseItem>(i)) == s.count[i]);
    }
    CHECK(uiFirstUseSaved(next) == saved);
    CHECK_FALSE(uiFirstUseShow(next, UiFirstUseItem::Guard));
    CHECK_FALSE(uiFirstUseShow(next, UiFirstUseItem::Dodge));
    CHECK(uiFirstUseShow(next, UiFirstUseItem::WeakPointLabel)); // its third time
    CHECK(uiFirstUseShow(next, UiFirstUseItem::Attack)); // never taught: shows

    // Joining two saves keeps the higher count of each item, whichever is older.
    UiFirstUse other;
    uiFirstUseBegin(other, 0u, UiFirstUseMode::Saved);
    for (int i = 0; i < 3; ++i) {
        uiFirstUseShow(other, UiFirstUseItem::WeakPointLabel);
        uiFirstUseEnd(other);
    }
    const std::uint32_t joined = uiFirstUseMerge(saved, uiFirstUseSaved(other));
    CHECK(uiFirstUseMerge(uiFirstUseSaved(other), saved) == joined);
    CHECK(uiFirstUseSavedCount(joined, UiFirstUseItem::WeakPointLabel) == 3);
    CHECK(uiFirstUseSavedCount(joined, UiFirstUseItem::Guard) == 1);
    CHECK(uiFirstUseSavedCount(joined, UiFirstUseItem::Dodge) == 1);
    CHECK(uiFirstUseSavedCount(joined, UiFirstUseItem::Jump) == 0);
    CHECK(uiFirstUseMerge(joined, kUiFirstUseCounted) == joined); // a fresh run's empty counts lower nothing
}

TEST_CASE("ui first use: every item keeps the slot older saves wrote it in; the retired slots read as nothing") {
    CHECK(uiFirstUseSlot(UiFirstUseItem::WeakPointLabel) == 3u);
    CHECK(uiFirstUseSlot(UiFirstUseItem::Attack) == 4u);
    CHECK(uiFirstUseSlot(UiFirstUseItem::Guard) == 5u);
    CHECK(uiFirstUseSlot(UiFirstUseItem::Dodge) == 6u);
    CHECK(uiFirstUseSlot(UiFirstUseItem::Jump) == 7u);
    CHECK(uiFirstUseSlot(UiFirstUseItem::LoneCard) == 9u);
    // No two items share a slot, and none sits in a retired one (0, 1, 2, 8) or past the top.
    for (std::size_t i = 0; i < kUiFirstUseCount; ++i) {
        CAPTURE(i);
        const unsigned slot = uiFirstUseSlot(static_cast<UiFirstUseItem>(i));
        CHECK(slot > 2u);
        CHECK(slot != 8u);
        CHECK(slot <= kUiFirstUseTopSlot);
        for (std::size_t j = 0; j < i; ++j) CHECK(uiFirstUseSlot(static_cast<UiFirstUseItem>(j)) != slot);
    }
    // A counted save from the rounds with the GUARD, LOAD, LOAD FULL and POISE labels: slot 0 = 3, slot 1 = 2, slot 2
    // (poise) = 1, slot 3 (weak point) = 2, slot 6 (dodge) = 1, slot 8 = 3.
    const std::uint32_t before = kUiFirstUseCounted | 3u | (2u << 2) | (1u << 4) | (2u << 6) | (1u << 12) | (3u << 16);
    CHECK(uiFirstUseSavedCount(before, UiFirstUseItem::WeakPointLabel) == 2);
    CHECK(uiFirstUseSavedCount(before, UiFirstUseItem::Dodge) == 1);
    CHECK(uiFirstUseSavedCount(before, UiFirstUseItem::Attack) == 0);
    CHECK(uiFirstUseSavedCount(before, UiFirstUseItem::Jump) == 0);
    UiFirstUse s;
    uiFirstUseBegin(s, before, UiFirstUseMode::Saved);
    CHECK(s.count[at(UiFirstUseItem::WeakPointLabel)] == 2);
    CHECK_FALSE(uiFirstUseShow(s, UiFirstUseItem::Dodge));
    CHECK(uiFirstUseShow(s, UiFirstUseItem::Jump));
    // Joined with anything, the retired slots (the poise count with them) are dropped and the rest kept.
    const std::uint32_t joined = uiFirstUseMerge(before, kUiFirstUseCounted);
    CHECK(joined == (kUiFirstUseCounted | (2u << 6) | (1u << 12)));
    CHECK(uiFirstUseSaved(s) == (kUiFirstUseCounted | (2u << 6) | (1u << 12) | (1u << 14)));
}

TEST_CASE("ui first use: a save from before the counts reads each set bit as shown once") {
    // The old layout: one bit per slot, bits 0..7 (bits 0, 1 and 2 were the GUARD, LOAD and POISE labels, now retired).
    const std::uint32_t old = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 6); // LOAD, POISE and WEAK POINT labels, Dodge
    CHECK((old & kUiFirstUseCounted) == 0u);
    CHECK(uiFirstUseSavedCount(old, UiFirstUseItem::WeakPointLabel) == 1);
    CHECK(uiFirstUseSavedCount(old, UiFirstUseItem::Dodge) == 1);
    CHECK(uiFirstUseSavedCount(old, UiFirstUseItem::Attack) == 0);
    UiFirstUse s;
    uiFirstUseBegin(s, old, UiFirstUseMode::Saved);
    CHECK_FALSE(uiFirstUseShow(s, UiFirstUseItem::Dodge));    // a control already taught
    CHECK(uiFirstUseShow(s, UiFirstUseItem::WeakPointLabel)); // a label: two showings left
    CHECK(s.count[at(UiFirstUseItem::WeakPointLabel)] == 2);
    // All eight old bits: every control taught, every label once; the retired bits lend nothing.
    const std::uint32_t all = 0xFFu;
    const std::uint32_t joined = uiFirstUseMerge(all, 0u);
    CHECK(uiFirstUseSavedCount(joined, UiFirstUseItem::Jump) == 1);
    CHECK(uiFirstUseSavedCount(joined, UiFirstUseItem::Attack) == 1);
    CHECK(uiFirstUseSavedCount(joined, UiFirstUseItem::WeakPointLabel) == 1);
    CHECK((joined & 0x3Fu) == 0u);
    CHECK(uiFirstUseSavedCount(1u | 2u | 4u | (1u << 8), UiFirstUseItem::WeakPointLabel) == 0); // only retired bits
}

TEST_CASE("ui first use: fresh ignores the save, seen teaches nothing") {
    UiFirstUse full;
    uiFirstUseBegin(full, 0u, UiFirstUseMode::Seen);
    const std::uint32_t all = uiFirstUseSaved(full);
    UiFirstUse s;
    uiFirstUseBegin(s, all, UiFirstUseMode::Fresh);
    CHECK(uiFirstUseShow(s, UiFirstUseItem::WeakPointLabel));
    CHECK(uiFirstUseShow(s, UiFirstUseItem::Guard));
    uiFirstUseBegin(s, 0u, UiFirstUseMode::Seen);
    CHECK(s.live == 0u);
    for (std::size_t i = 0; i < kUiFirstUseCount; ++i) {
        CHECK_FALSE(uiFirstUseShow(s, static_cast<UiFirstUseItem>(i)));
        CHECK_FALSE(uiFirstUseCue(s, static_cast<UiFirstUseItem>(i), true));
        CHECK(uiFirstUseAlpha(s, static_cast<UiFirstUseItem>(i)) == 0.f);
    }
    uiFirstUseBegin(s, all, UiFirstUseMode::Saved);
    CHECK_FALSE(uiFirstUseShow(s, UiFirstUseItem::Guard));
    CHECK_FALSE(uiFirstUseShow(s, UiFirstUseItem::WeakPointLabel));
}

TEST_CASE("ui first use: the Lone Carry's card counts its first three showings in its own slot") {
    const UiFirstUseItem c = UiFirstUseItem::LoneCard;
    CHECK_FALSE(uiFirstUseControl(c));
    CHECK(uiFirstUseTimes(c) == kUiFirstUseLabelTimes);
    // Saves from before the card never showed it: a counted one with every other item at its most, and the old one-bit
    // layout with every bit it ever had.
    const std::uint32_t before =
        kUiFirstUseCounted | (3u << 6) | (1u << 8) | (1u << 10) | (1u << 12) | (1u << 14) | (3u << 16);
    CHECK(uiFirstUseSavedCount(before, c) == 0);
    CHECK(uiFirstUseSavedCount(0x1FFu, c) == 0);
    UiFirstUse s;
    uiFirstUseBegin(s, before, UiFirstUseMode::Saved);
    for (std::uint8_t time = 1; time <= kUiFirstUseLabelTimes; ++time) {
        CAPTURE(time);
        CHECK(uiFirstUseShow(s, c)); // counted on showing: the host saves now
        CHECK(s.count[at(c)] == time);
        CHECK_FALSE(uiFirstUseShow(s, c)); // not again while it stands
        uiFirstUseEnd(s);
    }
    CHECK_FALSE(uiFirstUseShow(s, c)); // a fourth time: the title alone
    const std::uint32_t saved = uiFirstUseSaved(s);
    CHECK(((saved >> 18) & 3u) == 3u); // slot 9
    CHECK(uiFirstUseSavedCount(saved, c) == 3);
    CHECK(uiFirstUseSavedCount(saved, UiFirstUseItem::WeakPointLabel) == 3); // the rest kept
    CHECK(uiFirstUseSavedCount(uiFirstUseMerge(before, saved), c) == 3);
    // Fresh shows it again; seen never.
    UiFirstUse fresh;
    uiFirstUseBegin(fresh, saved, UiFirstUseMode::Fresh);
    CHECK(uiFirstUseShow(fresh, c));
    UiFirstUse seen;
    uiFirstUseBegin(seen, 0u, UiFirstUseMode::Seen);
    CHECK_FALSE(uiFirstUseShow(seen, c));
}
