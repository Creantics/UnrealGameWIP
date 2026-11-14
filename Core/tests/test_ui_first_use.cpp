#include "ftcore/UiFirstUse.h"

#include <doctest/doctest.h>

using namespace ftcore;

namespace {

void run(UiFirstUse& s, float seconds) {
    for (float t = 0.f; t < seconds - 1e-4f; t += 0.05f) uiFirstUseTick(s, 0.05f);
}

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

TEST_CASE("ui first use: a label holds 4 s, fades over 1 s, and never shows again in the save") {
    UiFirstUse s;
    uiFirstUseBegin(s, 0u, UiFirstUseMode::Saved);
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::LoadLabel) == 0.f);
    CHECK(uiFirstUseShow(s, UiFirstUseItem::LoadLabel));
    CHECK(s.taught == uiFirstUseBit(UiFirstUseItem::LoadLabel)); // taught on showing: the host saves now
    CHECK_FALSE(uiFirstUseShow(s, UiFirstUseItem::LoadLabel));   // no second save
    run(s, 3.9f);
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::LoadLabel) == 1.f);
    run(s, 0.6f);
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::LoadLabel) == doctest::Approx(0.5f).epsilon(0.05));
    run(s, 0.6f);
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::LoadLabel) == 0.f);
    CHECK(s.live == 0u);
    uiFirstUseUsed(s, UiFirstUseItem::LoadLabel); // labels are not used
    CHECK(s.used == 0u);

    UiFirstUse next; // the next fight of the same save
    uiFirstUseBegin(next, s.taught, UiFirstUseMode::Saved);
    CHECK_FALSE(uiFirstUseShow(next, UiFirstUseItem::LoadLabel));
    CHECK(uiFirstUseAlpha(next, UiFirstUseItem::LoadLabel) == 0.f);
    CHECK(uiFirstUseShow(next, UiFirstUseItem::PoiseLabel));
}

TEST_CASE("ui first use: a control stays until a second after its first use, then fades over 1 s") {
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
    uiFirstUseUsed(s, UiFirstUseItem::Jump); // never shown: ignored
    CHECK((s.used & uiFirstUseBit(UiFirstUseItem::Jump)) == 0u);
}

TEST_CASE("ui first use: the fight's end clears the screen and keeps what was taught") {
    UiFirstUse s;
    uiFirstUseBegin(s, 0u, UiFirstUseMode::Saved);
    uiFirstUseShow(s, UiFirstUseItem::Attack);
    uiFirstUseShow(s, UiFirstUseItem::GuardLabel);
    uiFirstUseEnd(s);
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::Attack) == 0.f);
    CHECK(uiFirstUseAlpha(s, UiFirstUseItem::GuardLabel) == 0.f);
    CHECK(s.taught == (uiFirstUseBit(UiFirstUseItem::Attack) | uiFirstUseBit(UiFirstUseItem::GuardLabel)));
}

TEST_CASE("ui first use: fresh ignores the save, seen teaches nothing") {
    const std::uint32_t all = (1u << kUiFirstUseCount) - 1u;
    UiFirstUse s;
    uiFirstUseBegin(s, all, UiFirstUseMode::Fresh);
    CHECK(uiFirstUseShow(s, UiFirstUseItem::WeakPointLabel));
    uiFirstUseBegin(s, 0u, UiFirstUseMode::Seen);
    CHECK(s.live == 0u);
    for (std::size_t i = 0; i < kUiFirstUseCount; ++i) {
        CHECK_FALSE(uiFirstUseShow(s, static_cast<UiFirstUseItem>(i)));
        CHECK(uiFirstUseAlpha(s, static_cast<UiFirstUseItem>(i)) == 0.f);
    }
    uiFirstUseBegin(s, all, UiFirstUseMode::Saved);
    CHECK_FALSE(uiFirstUseShow(s, UiFirstUseItem::Guard));
}
