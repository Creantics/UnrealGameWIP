#pragma once
// Button prompts for every screen from one table: per action and device, the glyph to draw (a key cap with its
// label, a mouse button, or a pad control drawn from our own shapes: no platform logos) and the label beside it.
//
//   UiGlyphTable t;  buildUiGlyphTable(bindings, t);     // after load and after every rebind (revision bumps)
//   UiDevice d = uiPromptDevice(UiGlyphMode::Auto, lastUsed);
//   const UiGlyph& g = uiGlyph(t, InputAction::Guard, d);
//
// Zero allocation.
#include "ftcore/Api.h"
#include "ftcore/InputBindings.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace ftcore {

enum class UiGlyphShape : std::uint8_t {
    None,      // unbound: draw the label only (the action word with "unbound")
    KeyCap,    // a rounded box with the key's label
    Mouse,     // a mouse with the button lit (part: 0 left, 1 right, 2 middle, 3 wheel up, 4 wheel down)
    FaceButton,// the diamond of four, one lit (part: 0 bottom, 1 right, 2 left, 3 top)
    Shoulder,  // part: 0 left, 1 right
    Trigger,   // part: 0 left, 1 right
    DPad,      // the cross, one arm lit (part: 0 up, 1 right, 2 down, 3 left)
    Stick,     // part: 0 left, 1 right; dir: 0 click, 1 up, 2 right, 3 down, 4 left
    Menu,      // part: 0 view (left), 1 menu (right)
};

struct UiGlyph {
    UiGlyphShape shape = UiGlyphShape::None;
    std::uint8_t part = 0;
    std::uint8_t dir = 0;
    std::array<char, 20> label{}; // short label (inputKeyLabel), cut to 19 bytes
    std::string_view text() const { return std::string_view(label.data()); }
    bool operator==(const UiGlyph&) const = default;
};

struct UiGlyphTable {
    std::array<std::array<UiGlyph, kInputActionCount>, kInputDeviceCount> glyphs{};
    std::uint32_t revision = 0; // bumps whenever a glyph changes
};

// The player's choice of prompt family: follow the last device used, or always one.
enum class UiGlyphMode : std::uint8_t { Auto, Keys, Pad };

// The glyph an engine key name draws as.
FTCORE_API UiGlyph uiGlyphForKey(std::string_view key);

// Fills `t` from `b`; bumps the revision only when something changed. True when it changed.
FTCORE_API bool buildUiGlyphTable(const InputBindings& b, UiGlyphTable& t);

FTCORE_API const UiGlyph& uiGlyph(const UiGlyphTable& t, InputAction a, InputDevice d);

FTCORE_API InputDevice uiPromptDevice(UiGlyphMode mode, InputDevice lastUsed);

} // namespace ftcore
