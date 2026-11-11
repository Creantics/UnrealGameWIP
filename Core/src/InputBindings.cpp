#include "ftcore/InputBindings.h"

#include "ftcore/Json.h"

namespace ftcore {

namespace {

struct ActionRow {
    InputAction action;
    const char* key;
    InputContext context;
    bool held;
    const char* keys; // default keyboard and mouse
    const char* pad;  // default pad
};

// One row per action, in enum order. The defaults: right mouse aims and left mouse is the light everywhere; Guard
// sits on the left hand (Left Ctrl, LB) so it can be held while moving and aiming.
constexpr std::array<ActionRow, kInputActionCount> kRows{{
    {InputAction::MoveForward, "move_forward", InputContext::Foot, false, "W", ""},
    {InputAction::MoveBack, "move_back", InputContext::Foot, false, "S", ""},
    {InputAction::MoveLeft, "move_left", InputContext::Foot, false, "A", ""},
    {InputAction::MoveRight, "move_right", InputContext::Foot, false, "D", ""},
    {InputAction::Light, "light", InputContext::Foot, false, "LeftMouseButton", "Gamepad_RightTrigger"},
    {InputAction::Aim, "aim", InputContext::Foot, true, "RightMouseButton", "Gamepad_LeftTrigger"},
    {InputAction::Guard, "guard", InputContext::Foot, true, "LeftControl", "Gamepad_LeftShoulder"},
    {InputAction::Read, "read", InputContext::Foot, true, "Q", "Gamepad_RightShoulder"},
    {InputAction::Gift, "gift", InputContext::Foot, false, "G", "Gamepad_DPad_Up"},
    {InputAction::Count, "count", InputContext::Foot, false, "F", "Gamepad_FaceButton_Top"},
    {InputAction::Release, "release", InputContext::Foot, false, "R", "Gamepad_DPad_Down"},
    {InputAction::Roll, "roll", InputContext::Foot, false, "LeftAlt", "Gamepad_FaceButton_Right"},
    {InputAction::Jump, "jump", InputContext::Foot, false, "SpaceBar", "Gamepad_FaceButton_Bottom"},
    {InputAction::Interact, "interact", InputContext::Foot, false, "E", "Gamepad_FaceButton_Left"},
    {InputAction::Run, "run", InputContext::Foot, true, "LeftShift", ""},
    {InputAction::Crouch, "crouch", InputContext::Foot, true, "C", "Gamepad_DPad_Left"},
    {InputAction::Intercept, "intercept", InputContext::Foot, true, "V", "Gamepad_LeftThumbstick"},
    {InputAction::LockOn, "lock_on", InputContext::Foot, false, "MiddleMouseButton", "Gamepad_RightThumbstick"},
    {InputAction::TargetPrev, "target_prev", InputContext::Foot, false, "", ""},
    {InputAction::TargetNext, "target_next", InputContext::Foot, false, "", ""},
    {InputAction::SwitchLead, "switch_lead", InputContext::Foot, false, "Tab", "Gamepad_DPad_Right"},
    {InputAction::Party, "party", InputContext::Foot, false, "P", ""},
    {InputAction::Map, "map", InputContext::Foot, false, "M", "Gamepad_Special_Left"},
    {InputAction::Pause, "pause", InputContext::Foot, false, "Escape", "Gamepad_Special_Right"},
    {InputAction::ReadPick1, "read_pick_1", InputContext::ReadHeld, false, "One", "Gamepad_DPad_Left"},
    {InputAction::ReadPick2, "read_pick_2", InputContext::ReadHeld, false, "Two", "Gamepad_DPad_Up"},
    {InputAction::ReadPick3, "read_pick_3", InputContext::ReadHeld, false, "Three", "Gamepad_DPad_Right"},
    {InputAction::FocusPrev, "focus_prev", InputContext::ReadHeld, false, "MouseScrollUp", "Gamepad_RightStick_Left"},
    {InputAction::FocusNext, "focus_next", InputContext::ReadHeld, false, "MouseScrollDown", "Gamepad_RightStick_Right"},
    {InputAction::LinePrev, "line_prev", InputContext::ReadHeld, false, "W", "Gamepad_LeftStick_Up"},
    {InputAction::LineNext, "line_next", InputContext::ReadHeld, false, "S", "Gamepad_LeftStick_Down"},
}};

const ActionRow& row(InputAction a) { return kRows[static_cast<std::size_t>(a) % kInputActionCount]; }

void setName(InputKeyName& n, std::string_view key) {
    n.text.fill('\0');
    for (std::size_t i = 0; i < key.size() && i + 1 < n.text.size(); ++i) n.text[i] = key[i];
}

const char* deviceKey(InputDevice d) { return d == InputDevice::Pad ? "pad" : "keys"; }

const char* holdKey(HoldMode m) {
    switch (m) {
    case HoldMode::Hold: return "hold";
    case HoldMode::Toggle: return "toggle";
    case HoldMode::Setting: break;
    }
    return "setting";
}

} // namespace

const char* inputActionKey(InputAction a) { return row(a).key; }

bool parseInputAction(std::string_view key, InputAction& out) {
    for (const ActionRow& r : kRows) {
        if (key == r.key) {
            out = r.action;
            return true;
        }
    }
    return false;
}

InputContext inputActionContext(InputAction a) { return row(a).context; }
bool inputActionHeld(InputAction a) { return row(a).held; }

InputBindings defaultInputBindings() {
    InputBindings b;
    for (const ActionRow& r : kRows) {
        const std::size_t i = static_cast<std::size_t>(r.action);
        setName(b.keys[0][i], r.keys);
        setName(b.keys[1][i], r.pad);
        b.hold[i] = HoldMode::Setting;
    }
    return b;
}

const char* inputKey(const InputBindings& b, InputAction a, InputDevice d) {
    return b.keys[static_cast<std::size_t>(d) % kInputDeviceCount][static_cast<std::size_t>(a) % kInputActionCount].text.data();
}

bool inputHoldToggles(const InputBindings& b, InputAction a, bool settingHoldToggle) {
    if (!inputActionHeld(a)) return false;
    switch (b.hold[static_cast<std::size_t>(a) % kInputActionCount]) {
    case HoldMode::Hold: return false;
    case HoldMode::Toggle: return true;
    case HoldMode::Setting: break;
    }
    return settingHoldToggle;
}

bool validInputKeyName(std::string_view key) {
    if (key.empty() || key.size() > 31) return false;
    for (const char c : key) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

bool inputKeyClash(const InputBindings& b, InputAction a, InputDevice d, std::string_view key, InputAction& clash) {
    if (key.empty()) return false;
    const auto& keys = b.keys[static_cast<std::size_t>(d) % kInputDeviceCount];
    for (const ActionRow& r : kRows) {
        if (r.action == a || r.context != inputActionContext(a)) continue;
        if (keys[static_cast<std::size_t>(r.action)].view() == key) {
            clash = r.action;
            return true;
        }
    }
    return false;
}

bool rebindInput(InputBindings& b, InputAction a, InputDevice d, std::string_view key, InputAction* swapped) {
    if (!key.empty() && !validInputKeyName(key)) return false;
    auto& keys = b.keys[static_cast<std::size_t>(d) % kInputDeviceCount];
    InputKeyName& mine = keys[static_cast<std::size_t>(a) % kInputActionCount];
    InputAction other{};
    if (inputKeyClash(b, a, d, key, other)) {
        keys[static_cast<std::size_t>(other)] = mine; // the two swap
        if (swapped) *swapped = other;
    }
    setName(mine, key);
    return true;
}

bool validateInputBindings(const InputBindings& b, std::string* error) {
    for (std::size_t d = 0; d < kInputDeviceCount; ++d) {
        const InputDevice device = static_cast<InputDevice>(d);
        for (const ActionRow& r : kRows) {
            const InputKeyName& n = b.keys[d][static_cast<std::size_t>(r.action)];
            if (n.empty()) continue;
            if (!validInputKeyName(n.view())) {
                if (error) *error = std::string(deviceKey(device)) + "." + r.key + ": not a key name";
                return false;
            }
            InputAction clash{};
            if (inputKeyClash(b, r.action, device, n.view(), clash)) {
                if (error)
                    *error = std::string(deviceKey(device)) + "." + r.key + ": " + std::string(n.view()) + " is also " +
                             inputActionKey(clash);
                return false;
            }
        }
    }
    return true;
}

bool importInputBindings(std::string_view text, InputBindings& out, std::string* error) {
    const auto fail = [error](std::string msg) {
        if (error) *error = std::move(msg);
        return false;
    };
    JsonDocument doc;
    std::string parseError;
    if (!doc.parse(text, &parseError)) return fail("bindings are not valid JSON: " + parseError);
    const JsonValue root = doc.root();
    if (!root.isObject()) return fail("bindings: the top level must be an object");
    if (!root["version"].isInteger() || root["version"].integer() != 1) return fail("bindings: \"version\" must be 1");
    InputBindings b = out;
    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        if (k != "version" && k != "keys" && k != "pad" && k != "hold") return fail("bindings: unknown key \"" + std::string(k) + "\"");
    }
    for (std::size_t d = 0; d < kInputDeviceCount; ++d) {
        const char* dk = deviceKey(static_cast<InputDevice>(d));
        const JsonValue keys = root[dk];
        if (keys.isAbsent()) continue;
        if (!keys.isObject()) return fail(std::string("bindings.") + dk + ": must be an object");
        for (std::uint32_t i = 0; i < keys.size(); ++i) {
            InputAction a{};
            const std::string where = std::string("bindings.") + dk + "." + std::string(keys.key(i));
            if (!parseInputAction(keys.key(i), a)) return fail(where + ": no such action");
            const JsonValue v = keys.value(i);
            if (!v.isString() || (!v.string().empty() && !validInputKeyName(v.string())))
                return fail(where + ": must be a key name or \"\"");
            setName(b.keys[d][static_cast<std::size_t>(a)], v.string());
        }
    }
    const JsonValue hold = root["hold"];
    if (!hold.isAbsent()) {
        if (!hold.isObject()) return fail("bindings.hold: must be an object");
        for (std::uint32_t i = 0; i < hold.size(); ++i) {
            InputAction a{};
            const std::string where = "bindings.hold." + std::string(hold.key(i));
            if (!parseInputAction(hold.key(i), a)) return fail(where + ": no such action");
            if (!inputActionHeld(a)) return fail(where + ": not a held action");
            const std::string_view m = hold.value(i).string();
            if (m == "setting") b.hold[static_cast<std::size_t>(a)] = HoldMode::Setting;
            else if (m == "hold") b.hold[static_cast<std::size_t>(a)] = HoldMode::Hold;
            else if (m == "toggle") b.hold[static_cast<std::size_t>(a)] = HoldMode::Toggle;
            else return fail(where + ": expected one of setting, hold, toggle");
        }
    }
    std::string why;
    if (!validateInputBindings(b, &why)) return fail("bindings: " + why);
    out = b;
    return true;
}

std::string encodeInputBindings(const InputBindings& b) {
    std::string s = "{\"version\": 1";
    for (std::size_t d = 0; d < kInputDeviceCount; ++d) {
        s += ", \"";
        s += deviceKey(static_cast<InputDevice>(d));
        s += "\": {";
        for (std::size_t i = 0; i < kInputActionCount; ++i) {
            if (i) s += ", ";
            s += '"';
            s += kRows[i].key;
            s += "\": \"";
            s += b.keys[d][i].view();
            s += '"';
        }
        s += '}';
    }
    s += ", \"hold\": {";
    bool first = true;
    for (std::size_t i = 0; i < kInputActionCount; ++i) {
        if (!kRows[i].held) continue;
        if (!first) s += ", ";
        first = false;
        s += '"';
        s += kRows[i].key;
        s += "\": \"";
        s += holdKey(b.hold[i]);
        s += '"';
    }
    s += "}}";
    return s;
}

} // namespace ftcore
