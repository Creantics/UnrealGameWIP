#include "ftcore/UiFirstUse.h"

#include <algorithm>

namespace ftcore {

namespace {
constexpr std::size_t at(UiFirstUseItem item) { return static_cast<std::size_t>(item); }
constexpr UiFirstUseItem itemAt(std::size_t i) { return static_cast<UiFirstUseItem>(i); }
} // namespace

bool uiFirstUseMode(std::string_view text, UiFirstUseMode& out) {
    if (text == "fresh") out = UiFirstUseMode::Fresh;
    else if (text == "seen") out = UiFirstUseMode::Seen;
    else return false;
    return true;
}

std::uint8_t uiFirstUseSavedCount(std::uint32_t saved, UiFirstUseItem item) {
    const unsigned i = static_cast<unsigned>(item);
    if (saved & kUiFirstUseCounted) return static_cast<std::uint8_t>((saved >> (2u * i)) & 3u);
    return static_cast<std::uint8_t>((saved >> i) & 1u);
}

std::uint32_t uiFirstUseSaved(const UiFirstUse& s) {
    std::uint32_t out = kUiFirstUseCounted;
    for (std::size_t i = 0; i < kUiFirstUseCount; ++i)
        out |= static_cast<std::uint32_t>(std::min<std::uint8_t>(s.count[i], 3)) << (2u * i);
    return out;
}

std::uint32_t uiFirstUseMerge(std::uint32_t a, std::uint32_t b) {
    std::uint32_t out = kUiFirstUseCounted;
    for (std::size_t i = 0; i < kUiFirstUseCount; ++i) {
        const std::uint8_t c = std::max(uiFirstUseSavedCount(a, itemAt(i)), uiFirstUseSavedCount(b, itemAt(i)));
        out |= static_cast<std::uint32_t>(c) << (2u * i);
    }
    return out;
}

void uiFirstUseBegin(UiFirstUse& s, std::uint32_t saved, UiFirstUseMode mode) {
    s = {};
    for (std::size_t i = 0; i < kUiFirstUseCount; ++i) {
        const UiFirstUseItem item = itemAt(i);
        const std::uint8_t times = uiFirstUseTimes(item);
        s.count[i] = mode == UiFirstUseMode::Fresh ? 0
            : mode == UiFirstUseMode::Seen         ? times
                                                   : std::min(uiFirstUseSavedCount(saved, item), times);
    }
}

bool uiFirstUseShow(UiFirstUse& s, UiFirstUseItem item) {
    const std::uint32_t bit = uiFirstUseBit(item);
    const std::size_t i = at(item);
    if ((s.live & bit) || s.count[i] >= uiFirstUseTimes(item)) return false;
    ++s.count[i];
    s.live |= bit;
    s.used &= ~bit;
    s.age[i] = 0.f;
    return true;
}

bool uiFirstUseCue(UiFirstUse& s, UiFirstUseItem item, bool drawn, bool allowed) {
    const std::uint32_t bit = uiFirstUseBit(item);
    const bool rose = drawn && !(s.cued & bit);
    s.cued = drawn ? (s.cued | bit) : (s.cued & ~bit);
    return rose && allowed && uiFirstUseShow(s, item);
}

void uiFirstUseUsed(UiFirstUse& s, UiFirstUseItem item) {
    const std::uint32_t bit = uiFirstUseBit(item);
    if (!uiFirstUseControl(item) || !(s.live & bit) || (s.used & bit)) return;
    s.used |= bit;
    s.age[at(item)] = 0.f;
}

void uiFirstUseTick(UiFirstUse& s, float dt) {
    for (std::size_t i = 0; i < kUiFirstUseCount; ++i) {
        const UiFirstUseItem item = itemAt(i);
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
    return std::clamp(1.f - (s.age[at(item)] - hold) / kUiFirstUseFade, 0.f, 1.f);
}

void uiFirstUseEnd(UiFirstUse& s) {
    s.live = 0;
    s.used = 0;
    s.cued = 0;
}

} // namespace ftcore
