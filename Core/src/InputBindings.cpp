#include "ftcore/InputBindings.h"

#include "ftcore/Json.h"

#include <algorithm>
#include <iterator>

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
    // The lead's own heal (its id stays "gift" for saved bindings; the third slot's gifts are GiftHeal and GiftGuard).
    {InputAction::Gift, "gift", InputContext::Foot, false, "H", "Gamepad_DPad_Up"},
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
    // Pad: no free Foot button; take half is Guard held plus Interact (route "guard_interact").
    {InputAction::TakeHalf, "take_half", InputContext::Foot, false, "T", ""},
    // Pad: no free Foot button either, and no modifier chord; unbound there until the pad's layout is ruled
    // (inputPadPending).
    {InputAction::GiftHeal, "gift_heal", InputContext::Foot, false, "G", ""},
    {InputAction::GiftGuard, "gift_guard", InputContext::Foot, false, "Y", ""},
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
    InputBindings next = b;
    auto& keys = next.keys[static_cast<std::size_t>(d) % kInputDeviceCount];
    InputKeyName& mine = keys[static_cast<std::size_t>(a) % kInputActionCount];
    InputAction other{};
    const bool clash = inputKeyClash(next, a, d, key, other);
    if (clash) keys[static_cast<std::size_t>(other)] = mine; // the two swap
    setName(mine, key);
    if (!validateInputBindings(next)) return false; // e.g. a Read-held key onto Read's or Guard's
    if (!inputAllReachable(next)) return false;     // as importInputBindings: no action left with no key or route
    b = next;
    if (clash && swapped) *swapped = other;
    return true;
}

const char* inputActionRoute(InputAction a, InputDevice d) {
    const bool pad = d == InputDevice::Pad;
    switch (a) {
    case InputAction::MoveForward:
    case InputAction::MoveBack:
    case InputAction::MoveLeft:
    case InputAction::MoveRight: return pad ? "left_stick" : nullptr;
    case InputAction::Run: return pad ? "left_stick_full" : nullptr;
    case InputAction::TargetPrev:
    case InputAction::TargetNext: return pad ? "right_stick_flick" : "mouse_flick";
    case InputAction::Party: return "map_hold";
    case InputAction::TakeHalf: return pad ? "guard_interact" : nullptr;
    default: break;
    }
    return nullptr;
}

bool inputPadPending(InputAction a) { return a == InputAction::GiftHeal || a == InputAction::GiftGuard; }

namespace {

// Whether two actions may not share a key on one device: the same context, or a Read-held action on Read's or Guard's
// key (validateInputBindings' two rules).
bool inputKeysConflict(InputAction a, InputAction b) {
    if (a == b) return false;
    const InputContext ca = inputActionContext(a), cb = inputActionContext(b);
    if (ca == cb) return true;
    const InputAction foot = ca == InputContext::Foot ? a : b;
    return foot == InputAction::Read || foot == InputAction::Guard;
}

bool reachableExcept(const InputBindings& b, const InputUnbound* skip, InputAction* missing) {
    for (std::size_t d = 0; d < kInputDeviceCount; ++d) {
        const InputDevice device = static_cast<InputDevice>(d);
        for (const ActionRow& r : kRows) {
            if (!b.keys[d][static_cast<std::size_t>(r.action)].empty()) continue;
            if (device == InputDevice::Pad && inputPadPending(r.action)) continue;
            if (skip && skip->left[d][static_cast<std::size_t>(r.action)]) continue;
            const char* route = inputActionRoute(r.action, device);
            // The party route rides the map button, so it needs one.
            const bool ok = route && (r.action != InputAction::Party || !b.keys[d][static_cast<std::size_t>(InputAction::Map)].empty());
            if (!ok) {
                if (missing) *missing = r.action;
                return false;
            }
        }
    }
    return true;
}

} // namespace

bool inputAllReachable(const InputBindings& b, InputAction* missing) {
    return reachableExcept(b, nullptr, missing);
}

bool validateInputBindings(const InputBindings& b, std::string* error) {
    for (std::size_t d = 0; d < kInputDeviceCount; ++d) {
        const InputDevice device = static_cast<InputDevice>(d);
        for (const ActionRow& r : kRows) {
            const InputKeyName& n = b.keys[d][static_cast<std::size_t>(r.action)];
            if (n.empty()) continue;
            if (r.context == InputContext::ReadHeld) {
                for (const InputAction kept : {InputAction::Read, InputAction::Guard}) {
                    if (b.keys[d][static_cast<std::size_t>(kept)].view() == n.view()) {
                        if (error)
                            *error = std::string(deviceKey(device)) + "." + r.key + ": " + std::string(n.view()) +
                                     " is " + inputActionKey(kept) + ", which works while Read is held";
                        return false;
                    }
                }
            }
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

bool importInputBindings(std::string_view text, InputBindings& out, std::string* error, InputUnbound* unbound) {
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
    const InputBindings defaults = defaultInputBindings();
    InputUnbound left;
    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const std::string_view k = root.key(i);
        if (k != "version" && k != "keys" && k != "pad" && k != "hold") return fail("bindings: unknown key \"" + std::string(k) + "\"");
    }
    for (std::size_t d = 0; d < kInputDeviceCount; ++d) {
        const char* dk = deviceKey(static_cast<InputDevice>(d));
        const JsonValue keys = root[dk];
        if (keys.isAbsent()) {
            b.keys[d] = defaults.keys[d]; // no entry for any action on this device: every one takes its default
            continue;
        }
        if (!keys.isObject()) return fail(std::string("bindings.") + dk + ": must be an object");
        std::array<bool, kInputActionCount> chosen{}; // the player's own bindings: they always win
        for (std::uint32_t i = 0; i < keys.size(); ++i) {
            InputAction a{};
            const std::string where = std::string("bindings.") + dk + "." + std::string(keys.key(i));
            if (!parseInputAction(keys.key(i), a)) return fail(where + ": no such action");
            const JsonValue v = keys.value(i);
            if (!v.isString() || (!v.string().empty() && !validInputKeyName(v.string())))
                return fail(where + ": must be a key name or \"\"");
            setName(b.keys[d][static_cast<std::size_t>(a)], v.string());
            chosen[static_cast<std::size_t>(a)] = true;
        }
        auto& row = b.keys[d];
        // A file saved before the gift slot had keys of its own names no gift_heal. Its gift key on gift_heal's
        // key (the old default, G) moves there: gift_heal keeps it and the heal's row takes the table's default, as
        // if the player had never chosen it. A gift on any other key stays the heal's.
        const std::size_t gi = static_cast<std::size_t>(InputAction::Gift);
        const std::size_t hi = static_cast<std::size_t>(InputAction::GiftHeal);
        if (chosen[gi] && !chosen[hi] && !row[gi].empty() && row[gi].view() == row[hi].view()) {
            row[gi] = defaults.keys[d][gi];
            chosen[gi] = false;
        }
        // An action the file has no entry for is new to it (added after the file was saved, or left out): it takes
        // its default, unless a key the file binds is that default, when the player's binding wins and the action is
        // left unbound (InputUnbound). Only two entries of the file's own on one key refuse it (validateInputBindings).
        for (const ActionRow& r : kRows) {
            const std::size_t ai = static_cast<std::size_t>(r.action);
            if (chosen[ai]) continue;
            row[ai] = defaults.keys[d][ai];
            if (row[ai].empty()) continue;
            for (const ActionRow& o : kRows) {
                const std::size_t oi = static_cast<std::size_t>(o.action);
                if (!chosen[oi] || !inputKeysConflict(r.action, o.action) || row[oi].view() != row[ai].view()) continue;
                row[ai] = InputKeyName{};
                left.left[d][ai] = true;
                ++left.count;
                break;
            }
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
    // An override may unbind a key, not an action: every action keeps a key or a route on both devices.
    InputAction missing{};
    if (!reachableExcept(b, &left, &missing))
        return fail(std::string("bindings: ") + inputActionKey(missing) + " has no key or route on one device");
    out = b;
    if (unbound) *unbound = left;
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

// ---- controls lines ----

namespace {

using A = InputAction;
// Only actions the host reads (ctest hints_read scans Source/ for each); Read and a short-press Map have no host
// logic yet, so they stay out.
constexpr InputHintEntry kArenaHint[] = {
    {{A::MoveForward, A::MoveLeft, A::MoveBack, A::MoveRight}, 4, "input.move", false},
    {{A::Light}, 1, "input.light", false},
    {{A::Light}, 1, "input.heavy", true},
    {{A::Aim}, 1, "input.aim", false},
    {{A::Guard}, 1, "input.guard", false},
    {{A::Roll}, 1, "input.dodge", false},
    {{A::Gift}, 1, "input.heal", false},
    {{A::GiftHeal}, 1, "input.gift_heal", false},
    {{A::GiftGuard}, 1, "input.gift_guard", false},
    {{A::Release}, 1, "input.release", false},
    {{A::Count}, 1, "input.count", false},
    {{A::ReadPick1, A::ReadPick2, A::ReadPick3}, 3, "input.pick", false},
    {{A::LockOn}, 1, "input.lock", false},
    {{A::TargetPrev, A::TargetNext}, 2, "input.switch", false},
    {{A::Intercept}, 1, "input.intercept", false},
};
// The arena's line with the jump after the dodge (InputHintScreen::ArenaJump); everything else as kArenaHint.
constexpr InputHintEntry kArenaJumpHint[] = {
    {{A::MoveForward, A::MoveLeft, A::MoveBack, A::MoveRight}, 4, "input.move", false},
    {{A::Light}, 1, "input.light", false},
    {{A::Light}, 1, "input.heavy", true},
    {{A::Aim}, 1, "input.aim", false},
    {{A::Guard}, 1, "input.guard", false},
    {{A::Roll}, 1, "input.dodge", false},
    {{A::Jump}, 1, "input.jump", false},
    {{A::Gift}, 1, "input.heal", false},
    {{A::GiftHeal}, 1, "input.gift_heal", false},
    {{A::GiftGuard}, 1, "input.gift_guard", false},
    {{A::Release}, 1, "input.release", false},
    {{A::Count}, 1, "input.count", false},
    {{A::ReadPick1, A::ReadPick2, A::ReadPick3}, 3, "input.pick", false},
    {{A::LockOn}, 1, "input.lock", false},
    {{A::TargetPrev, A::TargetNext}, 2, "input.switch", false},
    {{A::Intercept}, 1, "input.intercept", false},
};
static_assert(std::size(kArenaJumpHint) == std::size(kArenaHint) + 1);
constexpr InputHintEntry kFieldHint[] = {
    {{A::MoveForward, A::MoveLeft, A::MoveBack, A::MoveRight}, 4, "input.move", false},
    {{A::Run}, 1, "input.run", false},
    {{A::Jump}, 1, "input.jump", false},
    {{A::Light}, 1, "input.light", false},
    {{A::Light}, 1, "input.heavy", true},
    {{A::Aim}, 1, "input.aim", false},
    {{A::Crouch}, 1, "input.crouch", false},
    {{A::Roll}, 1, "input.dodge", false},
    {{A::Interact}, 1, "input.use", false},
    {{A::SwitchLead}, 1, "input.lead", false},
    {{A::Party}, 1, "input.party", false},
};

// What the host reads but no controls line names, each on purpose (ctest hints_read checks both ways): the map key
// shows only as the party hold's route ("Hold M"), the pause key is the menu key every screen shares, and take half
// plays only in carrying runs (a test switch) until it is promoted, so the default controls line stays as it was.
constexpr InputAction kUnhinted[] = {A::Map, A::Pause, A::TakeHalf};
static_assert(std::size(kUnhinted) > 0);

struct KeyLabel {
    const char* key;
    const char* label;
};
constexpr KeyLabel kKeyLabels[] = {
    {"LeftMouseButton", "LMB"}, {"RightMouseButton", "RMB"}, {"MiddleMouseButton", "MMB"},
    {"ThumbMouseButton", "Mouse 4"}, {"ThumbMouseButton2", "Mouse 5"}, {"MouseScrollUp", "Wheel up"},
    {"MouseScrollDown", "Wheel down"}, {"LeftControl", "Ctrl"}, {"RightControl", "Right Ctrl"}, {"LeftShift", "Shift"},
    {"RightShift", "Right Shift"}, {"LeftAlt", "Alt"}, {"RightAlt", "Right Alt"}, {"SpaceBar", "Space"},
    {"Escape", "Esc"}, {"Enter", "Enter"}, {"BackSpace", "Backspace"}, {"CapsLock", "Caps"},
    {"Zero", "0"}, {"One", "1"}, {"Two", "2"}, {"Three", "3"}, {"Four", "4"}, {"Five", "5"}, {"Six", "6"},
    {"Seven", "7"}, {"Eight", "8"}, {"Nine", "9"}, {"Up", "Up"}, {"Down", "Down"}, {"Left", "Left"}, {"Right", "Right"},
    {"Gamepad_RightTrigger", "RT"}, {"Gamepad_LeftTrigger", "LT"}, {"Gamepad_RightShoulder", "RB"},
    {"Gamepad_LeftShoulder", "LB"}, {"Gamepad_FaceButton_Bottom", "A"}, {"Gamepad_FaceButton_Right", "B"},
    {"Gamepad_FaceButton_Left", "X"}, {"Gamepad_FaceButton_Top", "Y"}, {"Gamepad_DPad_Up", "D-pad up"},
    {"Gamepad_DPad_Down", "D-pad down"}, {"Gamepad_DPad_Left", "D-pad left"}, {"Gamepad_DPad_Right", "D-pad right"},
    {"Gamepad_LeftThumbstick", "LS click"}, {"Gamepad_RightThumbstick", "RS click"}, {"Gamepad_Special_Left", "View"},
    {"Gamepad_Special_Right", "Menu"}, {"Gamepad_LeftStick_Up", "LS up"}, {"Gamepad_LeftStick_Down", "LS down"},
    {"Gamepad_LeftStick_Left", "LS left"}, {"Gamepad_LeftStick_Right", "LS right"}, {"Gamepad_RightStick_Up", "RS up"},
    {"Gamepad_RightStick_Down", "RS down"}, {"Gamepad_RightStick_Left", "RS left"}, {"Gamepad_RightStick_Right", "RS right"},
};

void addPiece(std::string& keys, std::string_view piece) {
    if (piece.empty()) return;
    // Each piece once ("RS flick" for both target switches).
    std::size_t at = 0;
    while (at <= keys.size()) {
        const std::size_t end = std::min(keys.find('/', at), keys.size());
        if (std::string_view(keys).substr(at, end - at) == piece) return;
        at = end + 1;
    }
    if (!keys.empty()) keys += '/';
    keys += piece;
}

} // namespace

std::size_t inputHintEntries(InputHintScreen screen, const InputHintEntry*& out) {
    if (screen == InputHintScreen::Field) {
        out = kFieldHint;
        return std::size(kFieldHint);
    }
    if (screen == InputHintScreen::ArenaJump) {
        out = kArenaJumpHint;
        return std::size(kArenaJumpHint);
    }
    out = kArenaHint;
    return std::size(kArenaHint);
}

std::string_view inputKeyLabel(std::string_view key) {
    for (const KeyLabel& k : kKeyLabels)
        if (key == k.key) return k.label;
    return key; // letters and anything unlisted show as the engine names them
}

std::string_view inputRouteLabel(std::string_view route) {
    if (route == "left_stick") return "LS";
    if (route == "left_stick_full") return "LS full";
    if (route == "mouse_flick") return "Mouse flick";
    if (route == "right_stick_flick") return "RS flick";
    if (route == "guard_interact") return "Guard+Interact";
    return std::string_view();
}

std::string inputHintKeys(const InputBindings& b, const InputHintEntry& e, InputDevice d, const InputUnbound* unbound) {
    std::string keys;
    for (std::uint8_t i = 0; i < e.count && i < e.actions.size(); ++i) {
        const InputAction a = e.actions[i];
        const InputKeyName& n = b.keys[static_cast<std::size_t>(d) % kInputDeviceCount][static_cast<std::size_t>(a) % kInputActionCount];
        if (n.empty() && unbound &&
            unbound->left[static_cast<std::size_t>(d) % kInputDeviceCount][static_cast<std::size_t>(a) % kInputActionCount]) {
            addPiece(keys, kInputNoKey);
            continue;
        }
        if (!n.empty()) {
            addPiece(keys, e.held ? std::string(kInputHoldPrefix) + std::string(inputKeyLabel(n.view())) : std::string(inputKeyLabel(n.view())));
            continue;
        }
        const char* route = inputActionRoute(a, d);
        if (!route) continue;
        if (std::string_view(route) == "map_hold") {
            const InputKeyName& map = b.keys[static_cast<std::size_t>(d) % kInputDeviceCount][static_cast<std::size_t>(InputAction::Map)];
            if (!map.empty()) addPiece(keys, std::string(kInputHoldPrefix) + std::string(inputKeyLabel(map.view())));
            continue;
        }
        addPiece(keys, inputRouteLabel(route));
    }
    return keys;
}

std::string inputActionKeys(const InputBindings& b, InputAction a, InputDevice d) {
    const auto keyOf = [&b, d](InputAction x) {
        return b.keys[static_cast<std::size_t>(d) % kInputDeviceCount][static_cast<std::size_t>(x) % kInputActionCount].view();
    };
    const std::string_view own = keyOf(a);
    if (!own.empty()) return std::string(inputKeyLabel(own));
    const char* route = inputActionRoute(a, d);
    if (!route) return std::string();
    const std::string_view r(route);
    if (r == "map_hold") {
        const std::string_view map = keyOf(InputAction::Map);
        return map.empty() ? std::string() : std::string(kInputHoldPrefix) + std::string(inputKeyLabel(map));
    }
    // Guard held plus a second button: the second is the one the route names.
    InputAction second = InputAction::Guard;
    if (r == "guard_interact") second = InputAction::Interact;
    if (second == InputAction::Guard) return std::string(inputRouteLabel(r));
    const std::string_view guard = keyOf(InputAction::Guard);
    const std::string_view other = keyOf(second);
    if (guard.empty() || other.empty()) return std::string();
    std::string keys(inputKeyLabel(guard));
    keys += '+';
    keys += inputKeyLabel(other);
    return keys;
}

std::string inputHintLine(const InputBindings& b, InputHintScreen screen, InputDevice d, InputHintText text, void* user,
                          const InputUnbound* unbound) {
    const InputHintEntry* entries = nullptr;
    const std::size_t count = inputHintEntries(screen, entries);
    std::string line;
    for (std::size_t i = 0; i < count; ++i) {
        const std::string keys = inputHintKeys(b, entries[i], d, unbound);
        if (keys.empty()) continue; // nothing reaches it on this device: the line names no key for it
        if (!line.empty()) line += "  ";
        line += keys;
        line += ' ';
        line += text ? text(entries[i].label, user) : std::string(entries[i].label);
    }
    return line;
}

std::string inputUnboundLine(const InputUnbound& unbound) {
    std::string line;
    for (std::size_t d = 0; d < kInputDeviceCount; ++d) {
        for (std::size_t a = 0; a < kInputActionCount; ++a) {
            if (!unbound.left[d][a]) continue;
            line += line.empty() ? std::string(kInputNoKey) + ": " : std::string(", ");
            line += inputActionKey(static_cast<InputAction>(a));
            line += d == 0 ? " (keyboard)" : " (pad)";
        }
    }
    return line;
}

} // namespace ftcore
