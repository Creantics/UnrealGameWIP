#pragma once
// What the fight HUD teaches once per save: a short label beside four elements the first time each shows, and the
// basic controls through the first fight, each until a second after its first use.
//
// The taught bits live in Settings::uiFirstUse. An item is taught the moment it shows, so a fight cut short does not
// teach it twice; the host saves only when uiFirstUseShow reports a change, never per frame.
//
//   UiFirstUse fu; uiFirstUseBegin(fu, settings.uiFirstUse, mode);
//   on fight start: for each control, uiFirstUseShow(fu, item);
//   when an element first shows: if (uiFirstUseShow(fu, item)) save fu.taught;
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
    GuardLabel, LoadLabel, PoiseLabel, WeakPointLabel, // labels: hold, then fade
    Attack, Guard, Dodge, Jump,                        // controls: until a second after the first use, then fade
};
inline constexpr std::size_t kUiFirstUseCount = 8;
inline constexpr float kUiFirstUseHold = 4.f;   // a label's full-ink seconds
inline constexpr float kUiFirstUseFade = 1.f;   // a label's or a control's fade
inline constexpr float kUiFirstUseAfter = 1.f;  // a control's full-ink seconds after its first use

// The -FTUiFirstUse switch: Saved uses the save's bits, Fresh teaches everything again, Seen teaches nothing.
enum class UiFirstUseMode : std::uint8_t { Saved, Fresh, Seen };
// "fresh" or "seen"; false (and `out` untouched) for anything else.
FTCORE_API bool uiFirstUseMode(std::string_view text, UiFirstUseMode& out);

struct UiFirstUse {
    std::uint32_t taught = 0;            // bit per item: shown in this save
    std::uint32_t live = 0;              // bit per item: on screen now
    std::uint32_t used = 0;              // bit per control: used, counting down
    float age[kUiFirstUseCount] = {};    // a label's seconds since shown; a control's since its first use
};

constexpr std::uint32_t uiFirstUseBit(UiFirstUseItem item) { return 1u << static_cast<unsigned>(item); }
constexpr bool uiFirstUseControl(UiFirstUseItem item) { return item >= UiFirstUseItem::Attack; }

FTCORE_API void uiFirstUseBegin(UiFirstUse& s, std::uint32_t saved, UiFirstUseMode mode);
// Shows the item if this save has not been taught it; true when that changed `taught` (the host saves then).
FTCORE_API bool uiFirstUseShow(UiFirstUse& s, UiFirstUseItem item);
// A live control was used: it fades kUiFirstUseAfter later. Its first use only; anything else is ignored.
FTCORE_API void uiFirstUseUsed(UiFirstUse& s, UiFirstUseItem item);
FTCORE_API void uiFirstUseTick(UiFirstUse& s, float dt);
// 0 when not live.
FTCORE_API float uiFirstUseAlpha(const UiFirstUse& s, UiFirstUseItem item);
// The fight is over: nothing it was teaching stays on screen.
FTCORE_API void uiFirstUseEnd(UiFirstUse& s);

} // namespace ftcore
