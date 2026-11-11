#include "ftcore/InputBindings.h"

#include <doctest/doctest.h>

#include <ostream>
#include <string>

using namespace ftcore;

namespace {
InputAction act(std::size_t i) { return static_cast<InputAction>(i); }
} // namespace

TEST_CASE("input bindings: the defaults are one set with nothing doubled") {
    const InputBindings b = defaultInputBindings();
    std::string error;
    CHECK(validateInputBindings(b, &error));
    CHECK(error.empty());
    CHECK(std::string(inputKey(b, InputAction::Guard, InputDevice::Keys)) == "LeftControl");
    CHECK(std::string(inputKey(b, InputAction::Guard, InputDevice::Pad)) == "Gamepad_LeftShoulder");
    CHECK(std::string(inputKey(b, InputAction::Aim, InputDevice::Keys)) == "RightMouseButton");
    CHECK(std::string(inputKey(b, InputAction::Light, InputDevice::Keys)) == "LeftMouseButton");
    // Every key-and-mouse Foot action has a key except the lock switch (a mouse flick).
    for (std::size_t i = 0; i < kInputActionCount; ++i) {
        const InputAction a = act(i);
        if (inputActionContext(a) != InputContext::Foot || a == InputAction::TargetPrev || a == InputAction::TargetNext)
            continue;
        CHECK_MESSAGE(!b.keys[0][i].empty(), inputActionKey(a));
    }
}

TEST_CASE("input bindings: action spellings round-trip and the held set is the toggle set") {
    for (std::size_t i = 0; i < kInputActionCount; ++i) {
        InputAction back{};
        REQUIRE(parseInputAction(inputActionKey(act(i)), back));
        CHECK(back == act(i));
    }
    InputAction none{};
    CHECK_FALSE(parseInputAction("heal", none));
    const InputBindings b = defaultInputBindings();
    for (std::size_t i = 0; i < kInputActionCount; ++i) {
        const InputAction a = act(i);
        const bool held = a == InputAction::Aim || a == InputAction::Guard || a == InputAction::Read ||
                          a == InputAction::Run || a == InputAction::Crouch || a == InputAction::Intercept;
        CHECK(inputActionHeld(a) == held);
        CHECK(inputHoldToggles(b, a, true) == held);
        CHECK_FALSE(inputHoldToggles(b, a, false));
    }
}

TEST_CASE("input bindings: import reads over the defaults and encode reads back") {
    InputBindings b = defaultInputBindings();
    std::string error;
    REQUIRE(importInputBindings(R"({"version": 1, "keys": {"guard": "Z"}, "hold": {"guard": "toggle", "aim": "hold"}})", b, &error));
    CHECK(std::string(inputKey(b, InputAction::Guard, InputDevice::Keys)) == "Z");
    CHECK(std::string(inputKey(b, InputAction::Guard, InputDevice::Pad)) == "Gamepad_LeftShoulder");
    CHECK(inputHoldToggles(b, InputAction::Guard, false));
    CHECK_FALSE(inputHoldToggles(b, InputAction::Aim, true));

    InputBindings back = defaultInputBindings();
    REQUIRE(importInputBindings(encodeInputBindings(b), back, &error));
    CHECK(encodeInputBindings(back) == encodeInputBindings(b));
    CHECK(back.hold == b.hold);
}

TEST_CASE("input bindings: a bad file is refused whole and changes nothing") {
    const InputBindings start = defaultInputBindings();
    const std::string before = encodeInputBindings(start);
    const char* bad[] = {
        "not json",
        R"({"keys": {"guard": "Z"}})",                       // no version
        R"({"version": 2})",                                   // unknown version
        R"({"version": 1, "extra": {}})",                      // unknown member
        R"({"version": 1, "keys": {"heal": "H"}})",            // unknown action
        R"({"version": 1, "keys": {"guard": "Left Ctrl"}})",   // not a key name
        R"({"version": 1, "keys": {"guard": 3}})",             // not a string
        R"({"version": 1, "hold": {"jump": "toggle"}})",       // hold on a press action
        R"({"version": 1, "hold": {"guard": "sometimes"}})",   // unknown mode
        R"({"version": 1, "keys": {"jump": "Z", "guard": "Z"}})", // doubled
        R"({"version": 1, "keys": {"guard": "E"}})",           // doubled with a default
    };
    for (const char* text : bad) {
        InputBindings b = start;
        std::string error;
        CHECK_MESSAGE(!importInputBindings(text, b, &error), text);
        CHECK_FALSE(error.empty());
        CHECK(encodeInputBindings(b) == before);
    }
}

TEST_CASE("input bindings: a key may be shared across contexts, and unbinding is allowed") {
    InputBindings b = defaultInputBindings();
    // W moves on foot and steps the lines while Read is held: two contexts, no clash.
    CHECK(std::string(inputKey(b, InputAction::LinePrev, InputDevice::Keys)) == "W");
    std::string error;
    CHECK(importInputBindings(R"({"version": 1, "keys": {"read_pick_1": "E"}, "pad": {"guard": ""}})", b, &error));
    CHECK(b.keys[1][static_cast<std::size_t>(InputAction::Guard)].empty());
}

TEST_CASE("input bindings: rebinding onto a used key swaps the two") {
    InputBindings b = defaultInputBindings();
    InputAction swapped = InputAction::Pause;
    REQUIRE(rebindInput(b, InputAction::Guard, InputDevice::Keys, "E", &swapped));
    CHECK(swapped == InputAction::Interact);
    CHECK(std::string(inputKey(b, InputAction::Guard, InputDevice::Keys)) == "E");
    CHECK(std::string(inputKey(b, InputAction::Interact, InputDevice::Keys)) == "LeftControl");
    CHECK(validateInputBindings(b));

    const std::string before = encodeInputBindings(b);
    CHECK_FALSE(rebindInput(b, InputAction::Guard, InputDevice::Keys, "Left Ctrl"));
    CHECK(encodeInputBindings(b) == before);
    REQUIRE(rebindInput(b, InputAction::Guard, InputDevice::Keys, ""));
    CHECK(b.keys[0][static_cast<std::size_t>(InputAction::Guard)].empty());
    CHECK(validateInputBindings(b));
}

TEST_CASE("held input: held follows the key, toggled flips on each press, reset lets go") {
    HeldInput h;
    CHECK(h.tick(true, false));
    CHECK_FALSE(h.tick(false, false));

    HeldInput t;
    CHECK(t.tick(true, true));
    CHECK(t.tick(true, true));  // still held: no second flip
    CHECK(t.tick(false, true)); // let go: stays on
    CHECK_FALSE(t.tick(true, true));
    CHECK_FALSE(t.tick(false, true));
    CHECK(t.tick(true, true));

    t.reset();
    CHECK_FALSE(t.on());
    CHECK_FALSE(t.tick(true, true)); // the key still down from before is not a press
    CHECK_FALSE(t.tick(false, true));
    CHECK(t.tick(true, true));

    HeldInput r;
    r.reset();
    CHECK(r.tick(true, false)); // held mode reads the key as it is
}

TEST_CASE("input bindings: in every mode no key does two things, and every action is reachable on both devices") {
    const InputBindings b = defaultInputBindings();
    InputAction missing = InputAction::Pause;
    CHECK_MESSAGE(inputAllReachable(b, &missing), inputActionKey(missing));
    for (std::size_t d = 0; d < kInputDeviceCount; ++d) {
        // Foot alone, and Read held (the ReadHeld keys over Foot's: a Foot action keeps a key only if no ReadHeld
        // action takes it).
        for (const bool readHeld : {false, true}) {
            for (std::size_t i = 0; i < kInputActionCount; ++i) {
                const InputAction a = act(i);
                const InputContext ci = inputActionContext(a);
                if (b.keys[d][i].empty() || (!readHeld && ci == InputContext::ReadHeld)) continue;
                for (std::size_t j = i + 1; j < kInputActionCount; ++j) {
                    const InputContext cj = inputActionContext(act(j));
                    if (b.keys[d][j].view() != b.keys[d][i].view() || (!readHeld && cj == InputContext::ReadHeld)) continue;
                    // The same key in both contexts is the overlay's shadow, not a double: allowed only while one
                    // is the Foot action it stands in for.
                    CHECK_MESSAGE(ci != cj, inputActionKey(a), " and ", inputActionKey(act(j)));
                }
            }
        }
        // Read and Guard keep their keys while Read is held.
        for (std::size_t j = 0; j < kInputActionCount; ++j) {
            if (inputActionContext(act(j)) != InputContext::ReadHeld) continue;
            CHECK(b.keys[d][j].view() != b.keys[d][static_cast<std::size_t>(InputAction::Read)].view());
            CHECK(b.keys[d][j].view() != b.keys[d][static_cast<std::size_t>(InputAction::Guard)].view());
        }
    }
    // Guard keeps a button that can be held (a shoulder, a modifier key) with its toggle option.
    CHECK(inputActionHeld(InputAction::Guard));
    InputBindings unbound = b;
    REQUIRE(rebindInput(unbound, InputAction::Map, InputDevice::Pad, ""));
    CHECK_FALSE(inputAllReachable(unbound, &missing));
    CHECK(missing == InputAction::Party); // its pad route rides the map button
}

TEST_CASE("input bindings: a Read-held key may not sit on Read's or Guard's key") {
    InputBindings b = defaultInputBindings();
    const std::string before = encodeInputBindings(b);
    CHECK_FALSE(rebindInput(b, InputAction::ReadPick1, InputDevice::Keys, "LeftControl"));
    CHECK_FALSE(rebindInput(b, InputAction::LineNext, InputDevice::Pad, "Gamepad_RightShoulder"));
    CHECK(encodeInputBindings(b) == before);
    std::string error;
    CHECK_FALSE(importInputBindings(R"({"version": 1, "keys": {"read_pick_2": "Q"}})", b, &error));
    CHECK(error.find("read") != std::string::npos);
}
