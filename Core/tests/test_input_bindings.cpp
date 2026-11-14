#include "ftcore/InputBindings.h"

#include <doctest/doctest.h>

#include <ostream>
#include <algorithm>
#include <cstdint>
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

TEST_CASE("input bindings: a file from before the gift keys moves its gift key over and keeps every other binding") {
    std::string error;
    // The old default (gift on G, now gift_heal's): gift_heal keeps G, the heal takes H, the rest as the file says.
    InputBindings b = defaultInputBindings();
    REQUIRE(importInputBindings(
        R"({"version": 1, "keys": {"gift": "G", "guard": "K", "release": "X"}, "pad": {"gift": "Gamepad_DPad_Up"}})", b, &error));
    CHECK(std::string(inputKey(b, InputAction::GiftHeal, InputDevice::Keys)) == "G");
    CHECK(std::string(inputKey(b, InputAction::Gift, InputDevice::Keys)) == "H");
    CHECK(std::string(inputKey(b, InputAction::Guard, InputDevice::Keys)) == "K");
    CHECK(std::string(inputKey(b, InputAction::Release, InputDevice::Keys)) == "X");
    CHECK(std::string(inputKey(b, InputAction::Gift, InputDevice::Pad)) == "Gamepad_DPad_Up");
    CHECK(std::string(inputKey(b, InputAction::GiftGuard, InputDevice::Keys)) == "Y");
    // The player's own heal key stays the heal's.
    InputBindings own = defaultInputBindings();
    REQUIRE(importInputBindings(R"({"version": 1, "keys": {"gift": "N"}})", own, &error));
    CHECK(std::string(inputKey(own, InputAction::Gift, InputDevice::Keys)) == "N");
    CHECK(std::string(inputKey(own, InputAction::GiftHeal, InputDevice::Keys)) == "G");
    // A key the player gave to another action leaves the newer action unbound, and the file imports with every one
    // of the player's bindings kept.
    struct Case {
        const char* text;
        InputAction mine;
        const char* key;
        InputAction left;
    };
    for (const Case& c : {Case{R"({"version": 1, "keys": {"release": "Y", "guard": "K"}})", InputAction::Release, "Y", InputAction::GiftGuard},
                          Case{R"({"version": 1, "keys": {"count": "H", "guard": "K"}})", InputAction::Count, "H", InputAction::Gift},
                          Case{R"({"version": 1, "keys": {"release": "G", "gift": "N", "guard": "K"}})", InputAction::Release, "G", InputAction::GiftHeal},
                          // A file from before take half (its default T added later) with T bound elsewhere.
                          Case{R"({"version": 1, "keys": {"release": "T", "guard": "K"}})", InputAction::Release, "T", InputAction::TakeHalf},
                          // An older default works the same way: no entry for interact, its E given to guard.
                          Case{R"({"version": 1, "keys": {"guard": "E"}})", InputAction::Guard, "E", InputAction::Interact}}) {
        CAPTURE(c.text);
        InputBindings t = defaultInputBindings();
        InputUnbound unbound;
        REQUIRE(importInputBindings(c.text, t, &error, &unbound));
        CHECK(std::string(inputKey(t, c.mine, InputDevice::Keys)) == c.key);
        if (c.mine != InputAction::Guard) CHECK(std::string(inputKey(t, InputAction::Guard, InputDevice::Keys)) == "K");
        CHECK(std::string(inputKey(t, c.left, InputDevice::Keys)).empty());
        CHECK(unbound.left[0][static_cast<std::size_t>(c.left)]);
        CHECK(unbound.count == 1);
    }
    // The migrated heal key is the table's default, whatever the table imported over.
    InputBindings moved = defaultInputBindings();
    REQUIRE(rebindInput(moved, InputAction::Gift, InputDevice::Keys, "J"));
    REQUIRE(importInputBindings(R"({"version": 1, "keys": {"gift": "G"}})", moved, &error));
    CHECK(std::string(inputKey(moved, InputAction::Gift, InputDevice::Keys)) == "H");
    // A file from before the gifts with both their keys bound elsewhere: it imports, both gifts unbound.
    InputBindings gifts = defaultInputBindings();
    InputUnbound giftsLeft;
    REQUIRE(importInputBindings(R"({"version": 1, "keys": {"release": "G", "count": "Y"}})", gifts, &error, &giftsLeft));
    CHECK(std::string(inputKey(gifts, InputAction::Release, InputDevice::Keys)) == "G");
    CHECK(std::string(inputKey(gifts, InputAction::Count, InputDevice::Keys)) == "Y");
    CHECK(std::string(inputKey(gifts, InputAction::GiftHeal, InputDevice::Keys)).empty());
    CHECK(std::string(inputKey(gifts, InputAction::GiftGuard, InputDevice::Keys)).empty());
    CHECK(std::string(inputKey(gifts, InputAction::Gift, InputDevice::Keys)) == "H");
    CHECK(giftsLeft.count == 2);
    // A file that itself binds one key to two actions is refused whole, whatever the actions.
    InputBindings twice = defaultInputBindings();
    const std::string twiceBefore = encodeInputBindings(twice);
    CHECK_FALSE(importInputBindings(R"({"version": 1, "keys": {"release": "T", "take_half": "T"}})", twice, &error));
    CHECK(encodeInputBindings(twice) == twiceBefore);
    // A file that names gift_heal is a new one: a doubled key there is still refused whole.
    InputBindings now = defaultInputBindings();
    const std::string before = encodeInputBindings(now);
    CHECK_FALSE(importInputBindings(R"({"version": 1, "keys": {"gift": "G", "gift_heal": "G"}})", now, &error));
    CHECK(encodeInputBindings(now) == before);
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
        R"({"version": 1, "keys": {"jump": "Z", "guard": "Z"}})", // doubled within the file
    };
    for (const char* text : bad) {
        InputBindings b = start;
        std::string error;
        CHECK_MESSAGE(!importInputBindings(text, b, &error), text);
        CHECK_FALSE(error.empty());
        CHECK(encodeInputBindings(b) == before);
    }
}

TEST_CASE("input bindings: a key may be shared across contexts, and unbinding is allowed where a route remains") {
    InputBindings b = defaultInputBindings();
    // W moves on foot and steps the lines while Read is held: two contexts, no clash.
    CHECK(std::string(inputKey(b, InputAction::LinePrev, InputDevice::Keys)) == "W");
    std::string error;
    // Party keeps its map-hold route on the keyboard.
    CHECK(importInputBindings(R"({"version": 1, "keys": {"read_pick_1": "E", "party": ""}})", b, &error));
    CHECK(b.keys[0][static_cast<std::size_t>(InputAction::Party)].empty());
}

TEST_CASE("input bindings: an override that leaves an action unreachable is refused whole") {
    for (const char* text : {R"({"version": 1, "keys": {"read_pick_1": "E"}, "pad": {"guard": ""}})",
                             R"({"version": 1, "keys": {"pause": ""}})", R"({"version": 1, "pad": {"pause": ""}})",
                             R"({"version": 1, "keys": {"move_forward": ""}})", R"({"version": 1, "keys": {"light": ""}})",
                             R"({"version": 1, "pad": {"light": ""}})", R"({"version": 1, "pad": {"map": ""}})"}) {
        CAPTURE(text);
        InputBindings b = defaultInputBindings();
        const std::string before = encodeInputBindings(b);
        std::string error;
        CHECK_FALSE(importInputBindings(text, b, &error));
        CHECK(error.find("no key or route") != std::string::npos);
        CHECK(encodeInputBindings(b) == before); // nothing of it taken, read_pick_1 included
    }
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
    // Unbinding Guard leaves it with no key or route on the keyboard: refused, as an imported file would be.
    CHECK_FALSE(rebindInput(b, InputAction::Guard, InputDevice::Keys, ""));
    CHECK(encodeInputBindings(b) == before);
    // An action with a route may go unbound.
    REQUIRE(rebindInput(b, InputAction::Party, InputDevice::Keys, ""));
    CHECK(b.keys[0][static_cast<std::size_t>(InputAction::Party)].empty());
    CHECK(validateInputBindings(b));
    CHECK(inputAllReachable(b));
    // A swap that leaves the other action unbound with no route is refused too: Party (unbound) takes Gift's key.
    const std::string unbound = encodeInputBindings(b);
    CHECK_FALSE(rebindInput(b, InputAction::Party, InputDevice::Keys, "G"));
    CHECK(encodeInputBindings(b) == unbound);
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
    CHECK_FALSE(rebindInput(unbound, InputAction::Map, InputDevice::Pad, "")); // refused: it would strand Party
    unbound.keys[1][static_cast<std::size_t>(InputAction::Map)] = InputKeyName{};
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
    CHECK_FALSE(importInputBindings(R"({"version": 1, "keys": {"read": "Q", "read_pick_2": "Q"}})", b, &error));
    CHECK(error.find("read") != std::string::npos);
    CHECK(encodeInputBindings(b) == before);
    // With no entry for Read in the file, the file's own key wins and Read is left unbound on the keyboard.
    InputUnbound left;
    REQUIRE(importInputBindings(R"({"version": 1, "keys": {"read_pick_2": "Q"}})", b, &error, &left));
    CHECK(std::string(inputKey(b, InputAction::Read, InputDevice::Keys)).empty());
    CHECK(left.left[0][static_cast<std::size_t>(InputAction::Read)]);
}

namespace {
// True when every '/'-separated piece of an entry's keys is a key the table holds on that device for one of the
// entry's actions (its short label), or the route an unbound one of them has.
bool piecesFromTable(const InputBindings& b, const InputHintEntry& e, InputDevice d, const std::string& keys) {
    if (keys.empty()) return true;
    std::size_t at = 0;
    while (at <= keys.size()) {
        const std::size_t end = std::min(keys.find('/', at), keys.size());
        const std::string piece = keys.substr(at, end - at);
        bool found = false;
        for (std::uint8_t i = 0; i < e.count; ++i) {
            const InputAction a = e.actions[i];
            const InputKeyName& n = b.keys[static_cast<std::size_t>(d)][static_cast<std::size_t>(a)];
            if (!n.empty()) {
                const std::string label(inputKeyLabel(n.view()));
                found = found || piece == (e.held ? std::string(kInputHoldPrefix) + label : label);
                continue;
            }
            const char* route = inputActionRoute(a, d);
            if (!route) continue;
            const InputKeyName& map = b.keys[static_cast<std::size_t>(d)][static_cast<std::size_t>(InputAction::Map)];
            found = found || piece == inputRouteLabel(route) ||
                    (std::string(route) == "map_hold" && !map.empty() &&
                     piece == std::string(kInputHoldPrefix) + std::string(inputKeyLabel(map.view())));
        }
        if (!found) return false;
        at = end + 1;
    }
    return true;
}

void checkLines(const InputBindings& b) {
    for (const InputHintScreen screen : {InputHintScreen::Arena, InputHintScreen::Field, InputHintScreen::ArenaJump})
        for (const InputDevice d : {InputDevice::Keys, InputDevice::Pad}) {
            CAPTURE(static_cast<int>(screen));
            CAPTURE(static_cast<int>(d));
            const InputHintEntry* entries = nullptr;
            const std::size_t n = inputHintEntries(screen, entries);
            REQUIRE(n > 0);
            std::string expected;
            for (std::size_t i = 0; i < n; ++i) {
                const std::string keys = inputHintKeys(b, entries[i], d);
                CAPTURE(entries[i].label);
                CAPTURE(keys);
                CHECK(piecesFromTable(b, entries[i], d, keys));
                if (keys.empty()) continue;
                if (!expected.empty()) expected += "  ";
                expected += keys + " " + entries[i].label;
            }
            // The line is exactly those entries: nothing else in it can name a key.
            CHECK(inputHintLine(b, screen, d, nullptr) == expected);
        }
}
} // namespace

TEST_CASE("input hints: the arena's and the field's lines name only keys the table holds, on each device") {
    const InputBindings b = defaultInputBindings();
    checkLines(b);
    CHECK(inputHintLine(b, InputHintScreen::Arena, InputDevice::Keys, nullptr) ==
          "W/A/S/D input.move  LMB input.light  Hold LMB input.heavy  RMB input.aim  Ctrl input.guard  Alt input.dodge  H input.heal  "
          "G input.gift_heal  Y input.gift_guard  R input.release  F input.count  1/2/3 input.pick  MMB input.lock  Mouse flick input.switch  "
          "V input.intercept");
    // On a pad the two gifts have no button yet, so the pad's line leaves them out.
    const std::string padArena = inputHintLine(b, InputHintScreen::Arena, InputDevice::Pad, nullptr);
    CHECK(padArena.find("input.gift_") == std::string::npos);
    CHECK(inputAllReachable(b));
    CHECK(inputHintLine(b, InputHintScreen::Field, InputDevice::Pad, nullptr) ==
          "LS input.move  LS full input.run  A input.jump  RT input.light  Hold RT input.heavy  LT input.aim  D-pad left input.crouch  "
          "B input.dodge  X input.use  D-pad right input.lead  "
          "Hold View input.party");
    // None of the retired keys shows on the keyboard lines (H came back as the heal's).
    const std::string keys = inputHintLine(b, InputHintScreen::Arena, InputDevice::Keys, nullptr) + "  " +
                             inputHintLine(b, InputHintScreen::Field, InputDevice::Keys, nullptr);
    for (const char* old : {"J ", "K ", "I/", "L/", "U ", "Q/E", "LMB/J"}) {
        CAPTURE(old);
        CHECK(keys.find(old) == std::string::npos);
    }
}

TEST_CASE("input hints: the lines follow a rebind and leave out what nothing reaches") {
    InputBindings b = defaultInputBindings();
    REQUIRE(rebindInput(b, InputAction::Guard, InputDevice::Keys, "K"));
    REQUIRE(rebindInput(b, InputAction::TargetNext, InputDevice::Keys, "X"));
    CHECK_FALSE(rebindInput(b, InputAction::Gift, InputDevice::Keys, "")); // no route: refused
    REQUIRE(rebindInput(b, InputAction::Party, InputDevice::Keys, ""));     // its route stays
    REQUIRE(rebindInput(b, InputAction::Jump, InputDevice::Pad, "Gamepad_FaceButton_Top")); // swaps with Count
    checkLines(b);
    const std::string arena = inputHintLine(b, InputHintScreen::Arena, InputDevice::Keys, nullptr);
    CHECK(arena.find("K input.guard") != std::string::npos);
    CHECK(arena.find("Ctrl") == std::string::npos);
    CHECK(arena.find("Mouse flick/X input.switch") != std::string::npos);
    CHECK(arena.find("H input.heal") != std::string::npos);
    // A table with an action nothing reaches (only a hand-built one now) leaves its entry out.
    InputBindings stranded = b;
    stranded.keys[0][static_cast<std::size_t>(InputAction::Gift)] = InputKeyName{};
    CHECK(inputHintLine(stranded, InputHintScreen::Arena, InputDevice::Keys, nullptr).find("input.heal") == std::string::npos);
    CHECK(inputHintLine(b, InputHintScreen::Field, InputDevice::Keys, nullptr).find("Hold M input.party") != std::string::npos);
    CHECK(inputHintLine(b, InputHintScreen::Field, InputDevice::Pad, nullptr).find("Y input.jump") != std::string::npos);
    CHECK(inputHintLine(b, InputHintScreen::Arena, InputDevice::Pad, nullptr).find("A input.count") != std::string::npos);
    // The host's words come through the callback.
    const auto word = [](const char* key, void*) { return std::string("<") + key + ">"; };
    CHECK(inputHintLine(b, InputHintScreen::Field, InputDevice::Keys, +word).find("Space <input.jump>") != std::string::npos);
}

TEST_CASE("input hints: an action the import left unbound shows NO KEY, and the fight's notice names each") {
    InputBindings b = defaultInputBindings();
    InputUnbound left;
    std::string error;
    REQUIRE(importInputBindings(R"({"version": 1, "keys": {"release": "G", "count": "Y"}})", b, &error, &left));
    const std::string arena = inputHintLine(b, InputHintScreen::Arena, InputDevice::Keys, nullptr, nullptr, &left);
    CHECK(arena.find("NO KEY input.gift_guard") != std::string::npos);
    CHECK(arena.find("NO KEY input.gift_heal") != std::string::npos);
    // Without the list an unbound action is left out, as before.
    CHECK(inputHintLine(b, InputHintScreen::Arena, InputDevice::Keys, nullptr).find("NO KEY") == std::string::npos);
    // The pad line is untouched: nothing was left unbound there.
    CHECK(inputHintLine(b, InputHintScreen::Arena, InputDevice::Pad, nullptr, nullptr, &left).find("NO KEY") == std::string::npos);
    CHECK(inputUnboundLine(left) == "NO KEY: gift_heal (keyboard), gift_guard (keyboard)");
    CHECK(inputUnboundLine(InputUnbound{}).empty());
}

TEST_CASE("input hints: the jump's arena line is the arena line with the jump after the dodge, on each device") {
    const InputBindings b = defaultInputBindings();
    for (const InputDevice d : {InputDevice::Keys, InputDevice::Pad}) {
        CAPTURE(static_cast<int>(d));
        const std::string plain = inputHintLine(b, InputHintScreen::Arena, d, nullptr);
        const std::string jump = inputHintLine(b, InputHintScreen::ArenaJump, d, nullptr);
        CHECK(plain.find("input.jump") == std::string::npos); // the default line names no jump key
        const std::string piece = (d == InputDevice::Keys ? "Space" : "A") + std::string(" input.jump  ");
        const std::size_t at = jump.find(piece);
        REQUIRE(at != std::string::npos);
        CHECK(jump.substr(0, at) + jump.substr(at + piece.size()) == plain);
        CHECK(jump.find("input.dodge  " + piece) != std::string::npos);
    }
}

TEST_CASE("input hints: key labels") {
    CHECK(inputKeyLabel("LeftMouseButton") == "LMB");
    CHECK(inputKeyLabel("Gamepad_FaceButton_Bottom") == "A");
    CHECK(inputKeyLabel("SpaceBar") == "Space");
    CHECK(inputKeyLabel("Z") == "Z");
    CHECK(inputRouteLabel("right_stick_flick") == "RS flick");
    CHECK(inputRouteLabel("map_hold").empty());
}
