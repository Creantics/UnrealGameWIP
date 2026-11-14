#include "ftcore/UiFirstUse.h"

#include <algorithm>

namespace ftcore {

namespace {
constexpr std::uint32_t kAll = (1u << kUiFirstUseCount) - 1u;
} // namespace

bool uiFirstUseMode(std::string_view text, UiFirstUseMode& out) {
    if (text == "fresh") out = UiFirstUseMode::Fresh;
    else if (text == "seen") out = UiFirstUseMode::Seen;
    else return false;
    return true;
}

void uiFirstUseBegin(UiFirstUse& s, std::uint32_t saved, UiFirstUseMode mode) {
    s = {};
    s.taught = mode == UiFirstUseMode::Fresh ? 0u : mode == UiFirstUseMode::Seen ? kAll : saved;
}

bool uiFirstUseShow(UiFirstUse& s, UiFirstUseItem item) {
    const std::uint32_t bit = uiFirstUseBit(item);
    if (s.taught & bit) return false;
    s.taught |= bit;
    s.live |= bit;
    s.used &= ~bit;
    s.age[static_cast<std::size_t>(item)] = 0.f;
    return true;
}

void uiFirstUseUsed(UiFirstUse& s, UiFirstUseItem item) {
    const std::uint32_t bit = uiFirstUseBit(item);
    if (!uiFirstUseControl(item) || !(s.live & bit) || (s.used & bit)) return;
    s.used |= bit;
    s.age[static_cast<std::size_t>(item)] = 0.f;
}

void uiFirstUseTick(UiFirstUse& s, float dt) {
    for (std::size_t i = 0; i < kUiFirstUseCount; ++i) {
        const auto item = static_cast<UiFirstUseItem>(i);
        const std::uint32_t bit = uiFirstUseBit(item);
        // A control's clock runs only once it has been used; a label's from the moment it shows.
        if (!(s.live & bit) || (uiFirstUseControl(item) && !(s.used & bit))) continue;
        s.age[i] += dt;
        if (s.age[i] >= (uiFirstUseControl(item) ? kUiFirstUseAfter : kUiFirstUseHold) + kUiFirstUseFade) s.live &= ~bit;
    }
}

float uiFirstUseAlpha(const UiFirstUse& s, UiFirstUseItem item) {
    const std::uint32_t bit = uiFirstUseBit(item);
    if (!(s.live & bit)) return 0.f;
    if (uiFirstUseControl(item) && !(s.used & bit)) return 1.f;
    const float hold = uiFirstUseControl(item) ? kUiFirstUseAfter : kUiFirstUseHold;
    return std::clamp(1.f - (s.age[static_cast<std::size_t>(item)] - hold) / kUiFirstUseFade, 0.f, 1.f);
}

void uiFirstUseEnd(UiFirstUse& s) {
    s.live = 0;
    s.used = 0;
}

} // namespace ftcore
