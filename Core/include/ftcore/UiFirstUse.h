#pragma once
// What the fight HUD teaches: a short label beside an element the first three times it shows in a save, and the basic
// controls through the first fight, each until a second after its first use (once a save).
//
// The counts live in Settings::uiFirstUse (uiFirstUseSaved packs them). An item is counted the moment it shows, so a
// fight cut short does not show it for nothing again; the host saves only when uiFirstUseShow or uiFirstUseCue reports
// a change, never per frame.
//
//   UiFirstUse fu; uiFirstUseBegin(fu, settings.uiFirstUse, mode);
//   on fight start: for each control, uiFirstUseShow(fu, item);
//   each tick, per label: if (uiFirstUseCue(fu, item, its element drawn)) save uiFirstUseSaved(fu);
//   each tick: uiFirstUseUsed(...) for each control used; uiFirstUseTick(fu, dt); draw at uiFirstUseAlpha(fu, item)
//   on fight end: uiFirstUseEnd(fu)
//
// Zero allocation.
#include "ftcore/Api.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ftcore {

enum class UiFirstUseItem : std::uint8_t {
    WeakPointLabel,             // a label: holds, then fades
    Attack, Guard, Dodge, Jump, // controls: until a second after the first use, then fade
};
inline constexpr std::size_t kUiFirstUseCount = 5;
inline constexpr std::uint8_t kUiFirstUseLabelTimes = 3; // a label shows the first three times in a save
inline constexpr float kUiFirstUseHold = 4.f;   // a label's full-ink seconds
inline constexpr float kUiFirstUseFade = 1.f;   // a label's or a control's fade
inline constexpr float kUiFirstUseAfter = 1.f;  // a control's full-ink seconds after its first use

// The -FTUiFirstUse switch: Saved uses the save's counts, Fresh teaches everything again, Seen teaches nothing.
enum class UiFirstUseMode : std::uint8_t { Saved, Fresh, Seen };
// "fresh" or "seen"; false (and `out` untouched) for anything else.
FTCORE_API bool uiFirstUseMode(std::string_view text, UiFirstUseMode& out);

struct UiFirstUse {
    std::uint8_t count[kUiFirstUseCount] = {}; // times shown in this save
    std::uint32_t live = 0;              // bit per item: on screen now
    std::uint32_t used = 0;              // bit per control: used, counting down
    std::uint32_t cued = 0;              // bit per label: its element was drawn at the last uiFirstUseCue
    float age[kUiFirstUseCount] = {};    // a label's seconds since shown; a control's since its first use
};

constexpr std::uint32_t uiFirstUseBit(UiFirstUseItem item) { return 1u << static_cast<unsigned>(item); }
constexpr bool uiFirstUseControl(UiFirstUseItem item) {
    return item >= UiFirstUseItem::Attack && item <= UiFirstUseItem::Jump;
}
// How many times a save shows the item: a label three, a control once.
constexpr std::uint8_t uiFirstUseTimes(UiFirstUseItem item) { return uiFirstUseControl(item) ? 1 : kUiFirstUseLabelTimes; }

// Settings::uiFirstUse. With this bit set, two bits per slot (slot n at bits 2n and 2n+1) count an item's showings.
// Without it (a save from before the counts), one bit per slot, a set bit read as shown once.
inline constexpr std::uint32_t kUiFirstUseCounted = 1u << 31;
// The item's slot in a saved value. Slots 0, 1, 2 and 8 held labels since retired (slot 2 the POISE label, now always
// drawn with its line): they are read as nothing and written as zero, and every remaining item keeps the slot an older
// save wrote it in. A new item takes a slot past kUiFirstUseTopSlot; no item ever moves.
constexpr unsigned uiFirstUseSlot(UiFirstUseItem item) {
    switch (item) {
    case UiFirstUseItem::WeakPointLabel: return 3;
    case UiFirstUseItem::Attack: return 4;
    case UiFirstUseItem::Guard: return 5;
    case UiFirstUseItem::Dodge: return 6;
    case UiFirstUseItem::Jump: return 7;
    }
    return 0; // a retired slot: nothing
}
inline constexpr unsigned kUiFirstUseTopSlot = 8; // the highest slot any save has written, a retired one included
static_assert(kUiFirstUseCount == 5, "a new item takes a slot past kUiFirstUseTopSlot in uiFirstUseSlot");
static_assert(2 * kUiFirstUseTopSlot + 1 < 31, "two bits per slot below the layout bit");
// The item's count in a saved value of either layout.
FTCORE_API std::uint8_t uiFirstUseSavedCount(std::uint32_t saved, UiFirstUseItem item);
// The counts packed for the save (always the counted layout).
FTCORE_API std::uint32_t uiFirstUseSaved(const UiFirstUse& s);
// Two saved values joined: per item the higher count, so writing one never lowers the other.
FTCORE_API std::uint32_t uiFirstUseMerge(std::uint32_t a, std::uint32_t b);

FTCORE_API void uiFirstUseBegin(UiFirstUse& s, std::uint32_t saved, UiFirstUseMode mode);
// Shows the item if the save has shown it fewer than uiFirstUseTimes and it is not on screen now (a live label is never
// restarted); true when that changed the counts (the host saves then).
FTCORE_API bool uiFirstUseShow(UiFirstUse& s, UiFirstUseItem item);
// A label's element drawn this tick or not: the label shows when the element starts to be drawn, not on every tick it
// stays. With `allowed` false that start passes and counts nothing. True when the counts changed.
FTCORE_API bool uiFirstUseCue(UiFirstUse& s, UiFirstUseItem item, bool drawn, bool allowed = true);
// A live control was used: it fades kUiFirstUseAfter later. Its first use only; anything else is ignored.
FTCORE_API void uiFirstUseUsed(UiFirstUse& s, UiFirstUseItem item);
FTCORE_API void uiFirstUseTick(UiFirstUse& s, float dt);
// 0 when not live.
FTCORE_API float uiFirstUseAlpha(const UiFirstUse& s, UiFirstUseItem item);
// The fight is over: nothing it was teaching stays on screen (the counts stay).
FTCORE_API void uiFirstUseEnd(UiFirstUse& s);

} // namespace ftcore
