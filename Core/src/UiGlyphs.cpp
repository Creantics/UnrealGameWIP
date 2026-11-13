#include "ftcore/UiGlyphs.h"

#include <algorithm>
#include <cstring>

namespace ftcore {

namespace {

struct KeyShape {
    std::string_view key;
    UiGlyphShape shape;
    std::uint8_t part;
    std::uint8_t dir;
};

constexpr KeyShape kKeyShapes[] = {
    {"LeftMouseButton", UiGlyphShape::Mouse, 0, 0},
    {"RightMouseButton", UiGlyphShape::Mouse, 1, 0},
    {"MiddleMouseButton", UiGlyphShape::Mouse, 2, 0},
    {"MouseScrollUp", UiGlyphShape::Mouse, 3, 0},
    {"MouseScrollDown", UiGlyphShape::Mouse, 4, 0},
    {"Gamepad_FaceButton_Bottom", UiGlyphShape::FaceButton, 0, 0},
    {"Gamepad_FaceButton_Right", UiGlyphShape::FaceButton, 1, 0},
    {"Gamepad_FaceButton_Left", UiGlyphShape::FaceButton, 2, 0},
    {"Gamepad_FaceButton_Top", UiGlyphShape::FaceButton, 3, 0},
    {"Gamepad_LeftShoulder", UiGlyphShape::Shoulder, 0, 0},
    {"Gamepad_RightShoulder", UiGlyphShape::Shoulder, 1, 0},
    {"Gamepad_LeftTrigger", UiGlyphShape::Trigger, 0, 0},
    {"Gamepad_RightTrigger", UiGlyphShape::Trigger, 1, 0},
    {"Gamepad_DPad_Up", UiGlyphShape::DPad, 0, 0},
    {"Gamepad_DPad_Right", UiGlyphShape::DPad, 1, 0},
    {"Gamepad_DPad_Down", UiGlyphShape::DPad, 2, 0},
    {"Gamepad_DPad_Left", UiGlyphShape::DPad, 3, 0},
    {"Gamepad_LeftThumbstick", UiGlyphShape::Stick, 0, 0},
    {"Gamepad_RightThumbstick", UiGlyphShape::Stick, 1, 0},
    {"Gamepad_LeftStick_Up", UiGlyphShape::Stick, 0, 1},
    {"Gamepad_LeftStick_Right", UiGlyphShape::Stick, 0, 2},
    {"Gamepad_LeftStick_Down", UiGlyphShape::Stick, 0, 3},
    {"Gamepad_LeftStick_Left", UiGlyphShape::Stick, 0, 4},
    {"Gamepad_RightStick_Up", UiGlyphShape::Stick, 1, 1},
    {"Gamepad_RightStick_Right", UiGlyphShape::Stick, 1, 2},
    {"Gamepad_RightStick_Down", UiGlyphShape::Stick, 1, 3},
    {"Gamepad_RightStick_Left", UiGlyphShape::Stick, 1, 4},
    {"Gamepad_Special_Left", UiGlyphShape::Menu, 0, 0},
    {"Gamepad_Special_Right", UiGlyphShape::Menu, 1, 0},
};

} // namespace

UiGlyph uiGlyphForKey(std::string_view key) {
    UiGlyph g;
    if (key.empty()) return g;
    g.shape = UiGlyphShape::KeyCap;
    for (const KeyShape& k : kKeyShapes) {
        if (k.key == key) {
            g.shape = k.shape;
            g.part = k.part;
            g.dir = k.dir;
            break;
        }
    }
    const std::string_view label = inputKeyLabel(key);
    std::memcpy(g.label.data(), label.data(), std::min(label.size(), g.label.size() - 1));
    return g;
}

bool buildUiGlyphTable(const InputBindings& b, UiGlyphTable& t) {
    bool changed = false;
    for (std::size_t d = 0; d < kInputDeviceCount; ++d) {
        for (std::size_t a = 0; a < kInputActionCount; ++a) {
            const char* key = inputKey(b, static_cast<InputAction>(a), static_cast<InputDevice>(d));
            const UiGlyph g = uiGlyphForKey(key ? std::string_view(key) : std::string_view());
            if (!(g == t.glyphs[d][a])) {
                t.glyphs[d][a] = g;
                changed = true;
            }
        }
    }
    if (changed) ++t.revision;
    return changed;
}

const UiGlyph& uiGlyph(const UiGlyphTable& t, InputAction a, InputDevice d) {
    return t.glyphs[static_cast<std::size_t>(d) % kInputDeviceCount][static_cast<std::size_t>(a) % kInputActionCount];
}

InputDevice uiPromptDevice(UiGlyphMode mode, InputDevice lastUsed) {
    switch (mode) {
    case UiGlyphMode::Keys: return InputDevice::Keys;
    case UiGlyphMode::Pad: return InputDevice::Pad;
    case UiGlyphMode::Auto: break;
    }
    return lastUsed;
}

} // namespace ftcore
