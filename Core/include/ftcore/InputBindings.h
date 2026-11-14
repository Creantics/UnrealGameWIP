#pragma once

// The player's bindings for walking about and fighting: every action, the key it has on the keyboard and mouse and
// the button it has on a pad, and, for each held action, whether it is held or pressed once to toggle. Keys are the
// engine's key names ("LeftMouseButton", "Gamepad_LeftShoulder"), kept as short fixed strings, so this file knows no
// engine. An empty name is unbound (the pad's run is the stick pushed all the way, the lock switch a flick).
//
// Two contexts: Foot (exploring and fighting, one set, so a fight in the field and in the arena answer to the same
// keys) and ReadHeld (only while Read is held: those keys stand in for whatever Foot gives them). Within a context
// and a device no key does two things; validateInputBindings checks it and rebindInput swaps on a clash. One pair is
// the exception: Crouch is read only outside a fight and Intercept only inside one (inputActionScope), so the two may
// share a key, and by default they share the pad's left stick click.
//
// The quick-time and mini-game prompts keep their own four action buttons (ActionButton.h); menus, dialogue and the
// van keep theirs. Nothing here allocates except import, encode and the error text.
//
//   ftcore::InputBindings b = ftcore::defaultInputBindings();
//   ftcore::importInputBindings(text, b, &error);    // the player's overrides, over the defaults
//   const char* key = ftcore::inputKey(b, ftcore::InputAction::Guard, ftcore::InputDevice::Keys);
//   ftcore::HeldInput guard; bool down = guard.tick(keyDown, ftcore::inputHoldToggles(b, InputAction::Guard, setting));

#include "ftcore/Api.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ftcore {

enum class InputAction : std::uint8_t {
    // Foot
    MoveForward,
    MoveBack,
    MoveLeft,
    MoveRight,
    Light, // tap; held to the heavy hold, the heavy (aiming a gun: one round per press)
    Aim,   // held
    Guard, // held; a tap just before a hit parries; a refusal is a held Guard
    Read,  // held
    Gift,
    Count,
    Release,
    Roll,
    Jump,
    Interact,
    Run,       // held
    Crouch,    // held
    Intercept, // held
    LockOn,
    TargetPrev,
    TargetNext,
    SwitchLead,
    Party,
    Map,
    Pause,
    // ReadHeld
    ReadPick1,
    ReadPick2,
    ReadPick3,
    FocusPrev,
    FocusNext,
    LinePrev,
    LineNext,
    // Foot (appended)
    TakeHalf,
    GiftHeal, // the third slot's two gifts, each picked and given at once (Gift is the lead's own heal)
    GiftGuard,
    PartnerOrder, // the partner's order: StayClose and Spread in turn (arena)
};
inline constexpr std::size_t kInputActionCount = static_cast<std::size_t>(InputAction::PartnerOrder) + 1;

enum class InputContext : std::uint8_t { Foot, ReadHeld };
// Where a Foot action is read: anywhere, only outside a fight (Field), or only inside one (Fight). A Field action and a
// Fight action never fire in the same place, so they may share a key (inputKeyClash).
enum class InputScope : std::uint8_t { Any, Field, Fight };
enum class InputDevice : std::uint8_t { Keys, Pad };
inline constexpr std::size_t kInputDeviceCount = 2;

// An engine key name; empty is unbound. 31 characters at most.
struct InputKeyName {
    std::array<char, 32> text{};
    std::string_view view() const { return std::string_view(text.data()); }
    bool empty() const { return text[0] == '\0'; }
};

// Per held action: follow the game-wide hold-toggle setting, or always hold, or always press once to toggle.
enum class HoldMode : std::uint8_t { Setting, Hold, Toggle };

struct InputBindings {
    std::array<std::array<InputKeyName, kInputActionCount>, kInputDeviceCount> keys{};
    std::array<HoldMode, kInputActionCount> hold{};
};

// The data spelling: "light", "guard", "read_pick_1", ... A literal.
FTCORE_API const char* inputActionKey(InputAction a);
FTCORE_API bool parseInputAction(std::string_view key, InputAction& out);
FTCORE_API InputContext inputActionContext(InputAction a);
// Whether the action is held (Aim, Guard, Read, Run, Crouch, Intercept): the ones with a hold-to-toggle option.
FTCORE_API bool inputActionHeld(InputAction a);
// Crouch is Field and Intercept is Fight; every other action is Any.
FTCORE_API InputScope inputActionScope(InputAction a);
// Whether the host reads the action there: false for a Field action in a fight and a Fight action outside one. A host
// that reads a shared key asks this first, so the one button does one thing in each place.
FTCORE_API bool inputActionLive(InputAction a, bool inFight);

FTCORE_API InputBindings defaultInputBindings();
FTCORE_API const char* inputKey(const InputBindings& b, InputAction a, InputDevice d);
// Whether a held action is pressed once to toggle, given the game-wide setting. False for a press action.
FTCORE_API bool inputHoldToggles(const InputBindings& b, InputAction a, bool settingHoldToggle);

// A key name the table accepts: 1 to 31 of A-Z, a-z, 0-9 and '_'.
FTCORE_API bool validInputKeyName(std::string_view key);
// The action in `a`'s context already on `key` for that device, other than `a`; false when none.
FTCORE_API bool inputKeyClash(const InputBindings& b, InputAction a, InputDevice d, std::string_view key, InputAction& clash);
// Puts `key` (empty: unbind) on `a` for that device. A key another action in the same context has moves to it from
// `a` (the two swap), and `swapped` names it. False (nothing changed) for a bad key name, a result
// validateInputBindings refuses, or one that leaves an action with no key or route (inputAllReachable), as an
// imported file is refused.
FTCORE_API bool rebindInput(InputBindings& b, InputAction a, InputDevice d, std::string_view key, InputAction* swapped = nullptr);
// How an action with no key on that device is still reached: "left_stick" (moving), "left_stick_full" (the stick
// pushed all the way runs), "mouse_flick" / "right_stick_flick" (locked on, a flick switches the target),
// "map_hold" (holding the map button shows or hides the party), "guard_interact" (take half on a pad: the pad's Guard
// button down plus Interact), "party_menu" (switch lead on a pad, ruled 2026-11-14: the party menu does it, so no
// controls line names a pad key for it). Null when the action has no such route.
FTCORE_API const char* inputActionRoute(InputAction a, InputDevice d);
// Holding the map button this long, on either device, is the party toggle (inputActionRoute "map_hold").
inline constexpr float kInputMapHoldForParty = 0.6f;
// Every action has a key or a route on both devices (the defaults' promise; an override may unbind).
FTCORE_API bool inputAllReachable(const InputBindings& b, InputAction* missing = nullptr);
// No key twice in one context for one device, every name valid, and no ReadHeld key on Read's or Guard's key (a
// refusal is a held Guard, so Guard must keep working while Read is held). The first problem in `error`.
FTCORE_API bool validateInputBindings(const InputBindings& b, std::string* error = nullptr);

// {"version": 2, "keys": {"guard": "LeftControl"}, "pad": {"guard": "Gamepad_LeftShoulder"}, "hold": {"guard": "toggle"}}
// read over `out`: a key the file names wins, every other key is the table's default, and a hold mode left out keeps
// its value. Unknown actions, bad names,
// "hold" on a press action and two of the file's own entries on one key are refused, and `out` is left as it was.
// An action the file has no entry for takes its default; when a key the file binds is that default, the player's
// binding wins and the action is left unbound (InputUnbound): the host shows it as NO KEY and says so at a fight's start.
// Version 1 is a file saved before the pad gifts had buttons: its pad entry "" for gift_heal or gift_guard (the only
// value it could hold) reads as no entry, so the gift takes its D-pad default or, on a key the file keeps for crouch
// or switch lead, NO KEY. encodeInputBindings writes kInputBindingsVersion.
inline constexpr int kInputBindingsVersion = 2;
struct InputUnbound {
    std::array<std::array<bool, kInputActionCount>, kInputDeviceCount> left{};
    std::uint32_t count = 0;
};
FTCORE_API bool importInputBindings(std::string_view text, InputBindings& out, std::string* error = nullptr,
                                    InputUnbound* unbound = nullptr);
// The partner's order has no pad button and no route until the pad's layout is ruled for it: inputAllReachable lets
// it go unbound on the pad. The two gifts have D-pad buttons (ruled 2026-11-14) but may still go unbound there, so a
// version-1 file's NO KEY gifts, saved again, read back.
FTCORE_API bool inputPadPending(InputAction a);
// Every binding and every held action's mode; importInputBindings reads it back.
FTCORE_API std::string encodeInputBindings(const InputBindings& b);

// ---- controls lines ----
// The arena's and the field's controls line, built from the table for one device, so it names only keys the player
// has (and follows the overrides). An entry is a group of actions under one word: the four moves are one entry
// ("W/A/S/D"), the two target switches another. Each action shows its key's short label ("LMB", "Ctrl", "RT", "A"),
// or, unbound, its route ("LS", "RS flick", "Hold M"); an entry nothing reaches on that device is left out.

// ArenaJump: the arena's line with the jump after the dodge, for runs with the jump's binding on (-ArenaJump, until
// the jump is promoted); the plain arena line names no jump key, so it sends no one to a key that does nothing.
enum class InputHintScreen : std::uint8_t { Arena, Field, ArenaJump };
inline constexpr std::size_t kInputHintScreenCount = 3;

struct InputHintEntry {
    std::array<InputAction, 4> actions{};
    std::uint8_t count = 0;
    const char* label = ""; // the word's key in Data/Loc/<culture>/ui.json ("input.light")
    bool held = false;      // the action's key held, not tapped: each key shows after kInputHoldPrefix ("Hold LMB")
};

// The map-hold route's prefix before the map key's label ("Hold M").
inline constexpr const char* kInputHoldPrefix = "Hold ";

// The screen's entries, in the line's order.
FTCORE_API std::size_t inputHintEntries(InputHintScreen screen, const InputHintEntry*& out);
// A short label for an engine key name ("LeftMouseButton" -> "LMB", "Gamepad_FaceButton_Bottom" -> "A"); a name
// with no label (a letter) shows as it is.
FTCORE_API std::string_view inputKeyLabel(std::string_view key);
// A route's label ("left_stick" -> "LS", "right_stick_flick" -> "RS flick"); empty for map_hold, which shows as
// kInputHoldPrefix and the map key.
FTCORE_API std::string_view inputRouteLabel(std::string_view route);
// What an action the import left unbound (InputUnbound) shows in place of a key, so the player sees it has none.
inline constexpr const char* kInputNoKey = "NO KEY";

// The keys one entry shows on a device, '/' between them, each once; empty when none of its actions is reachable. An
// action `unbound` marks on that device shows kInputNoKey.
FTCORE_API std::string inputHintKeys(const InputBindings& b, const InputHintEntry& e, InputDevice d,
                                     const InputUnbound* unbound = nullptr);
// The word for a label key (the host's localised text); null shows the key itself.
using InputHintText = std::string (*)(const char* labelKey, void* user);
// The whole line: "<keys> <word>" per entry, two spaces apart. Allocates: build it when the device or the
// bindings change, not every frame.
FTCORE_API std::string inputHintLine(const InputBindings& b, InputHintScreen screen, InputDevice d, InputHintText text,
                                     void* user = nullptr, const InputUnbound* unbound = nullptr);
// One line naming each action the import left unbound, with the device: "NO KEY: take_half (keyboard), gift_heal
// (pad)". Empty when none. A fight's host shows it at the start. Allocates.
FTCORE_API std::string inputUnboundLine(const InputUnbound& unbound);

// A held input with the toggle option: held, it is down while the key is; toggled, each press flips it.
class HeldInput {
public:
    bool tick(bool keyDown, bool toggle) {
        if (toggle) {
            if (keyDown && !wasDown_) on_ = !on_;
        } else {
            on_ = keyDown;
        }
        wasDown_ = keyDown;
        return on_;
    }
    // The input taken away (a menu, a pause, a scene): up, and a toggle let go. A key still down when input comes
    // back is not a new press.
    void reset() {
        on_ = false;
        wasDown_ = true;
    }
    bool on() const { return on_; }

private:
    bool on_ = false;
    bool wasDown_ = false;
};

} // namespace ftcore
